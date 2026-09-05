#include <veloco/http.h>
#include <veloco/io.h>
#include <veloco/memory.h>
#include <veloco/runtime.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct bench_config {
    vl_io_backend_t backend;
    size_t workers;
    size_t requests;
    size_t concurrency;
} bench_config_t;

typedef struct bench_result {
    uint64_t duration_ns;
    uint64_t p50_ns;
    uint64_t p95_ns;
    uint64_t p99_ns;
    vl_runtime_stats_t runtime_stats;
    vl_allocator_stats_t allocator_stats;
    vl_io_stats_t io_stats;
    size_t active_connections;
} bench_result_t;

static uint64_t elapsed_ns(const struct timespec *start,
                           const struct timespec *end)
{
    return (uint64_t)(end->tv_sec - start->tv_sec) * UINT64_C(1000000000) +
           (uint64_t)(end->tv_nsec - start->tv_nsec);
}

static int cmp_u64(const void *left, const void *right)
{
    uint64_t l = *(const uint64_t *)left;
    uint64_t r = *(const uint64_t *)right;

    return l < r ? -1 : l > r ? 1 : 0;
}

static uint64_t percentile(uint64_t *values, size_t count, size_t pct)
{
    size_t index;

    if (count == 0) {
        return 0;
    }
    qsort(values, count, sizeof(values[0]), cmp_u64);
    index = (count * pct + 99) / 100;
    if (index == 0) {
        index = 1;
    }
    if (index > count) {
        index = count;
    }
    return values[index - 1];
}

static int parse_size(const char *value, size_t *out)
{
    char *end = NULL;
    unsigned long parsed;

    if (value == NULL || out == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    *out = (size_t)parsed;
    return VL_OK;
}

static int parse_backend(const char *value, vl_io_backend_t *out)
{
    if (value == NULL || out == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (strcmp(value, "epoll") == 0) {
        *out = VL_IO_BACKEND_EPOLL;
        return VL_OK;
    }
    if (strcmp(value, "uring") == 0) {
        *out = VL_IO_BACKEND_URING;
        return VL_OK;
    }
    return VL_ERROR_INVALID_ARGUMENT;
}

static int parse_args(int argc, char **argv, bench_config_t *config)
{
    size_t index;

    config->backend = VL_IO_BACKEND_EPOLL;
    config->workers = 1;
    config->requests = 1000;
    config->concurrency = 16;
    for (index = 1; index < (size_t)argc; ++index) {
        if (strcmp(argv[index], "--backend") == 0 && index + 1 < (size_t)argc) {
            if (parse_backend(argv[++index], &config->backend) != VL_OK) {
                return VL_ERROR_INVALID_ARGUMENT;
            }
        } else if (strcmp(argv[index], "--workers") == 0 &&
                   index + 1 < (size_t)argc) {
            if (parse_size(argv[++index], &config->workers) != VL_OK) {
                return VL_ERROR_INVALID_ARGUMENT;
            }
        } else if (strcmp(argv[index], "--requests") == 0 &&
                   index + 1 < (size_t)argc) {
            if (parse_size(argv[++index], &config->requests) != VL_OK) {
                return VL_ERROR_INVALID_ARGUMENT;
            }
        } else if (strcmp(argv[index], "--concurrency") == 0 &&
                   index + 1 < (size_t)argc) {
            if (parse_size(argv[++index], &config->concurrency) != VL_OK) {
                return VL_ERROR_INVALID_ARGUMENT;
            }
        } else {
            return VL_ERROR_INVALID_ARGUMENT;
        }
    }
    if (config->workers == 0 || config->requests == 0 ||
        config->concurrency == 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (config->concurrency > config->requests) {
        config->concurrency = config->requests;
    }
    return VL_OK;
}

static void health_handler(const vl_http_request_t *request,
                           vl_http_response_t *response, void *user_data)
{
    (void)request;
    (void)user_data;
    (void)vl_http_response_set_body(response, "ok", 2);
}

static int drain_client_response(int fd, char *buffer, size_t capacity)
{
    size_t length = 0;

    for (;;) {
        ssize_t got = read(fd, buffer + length, capacity - 1 - length);

        if (got > 0) {
            length += (size_t)got;
            if (length >= capacity - 1) {
                break;
            }
            continue;
        }
        if (got == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        return VL_ERROR_SYSTEM;
    }
    buffer[length] = '\0';
    return length > 0 ? VL_OK : VL_ERROR_WOULD_BLOCK;
}

static int run_batch(vl_runtime_t *runtime, vl_io_t *io,
                     vl_http_server_t *server, const char *request,
                     size_t batch_count, uint64_t *latencies_ns)
{
    int *server_fds = calloc(batch_count, sizeof(*server_fds));
    int *client_fds = calloc(batch_count, sizeof(*client_fds));
    struct timespec start;
    struct timespec end;
    size_t index;
    int status = VL_OK;

    if (server_fds == NULL || client_fds == NULL) {
        free(server_fds);
        free(client_fds);
        return VL_ERROR_OUT_OF_MEMORY;
    }
    for (index = 0; index < batch_count; ++index) {
        server_fds[index] = -1;
        client_fds[index] = -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        free(server_fds);
        free(client_fds);
        return VL_ERROR_SYSTEM;
    }
    for (index = 0; index < batch_count; ++index) {
        int sockets[2] = {-1, -1};

        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                       sockets) != 0) {
            status = VL_ERROR_SYSTEM;
            break;
        }
        if (vl_socket_track(sockets[0]) != VL_OK ||
            vl_socket_track(sockets[1]) != VL_OK) {
            status = VL_ERROR_INVALID_STATE;
            (void)vl_socket_close(sockets[0]);
            (void)vl_socket_close(sockets[1]);
            break;
        }
        if (write(sockets[1], request, strlen(request)) < 0) {
            status = VL_ERROR_SYSTEM;
            (void)vl_socket_close(sockets[0]);
            (void)vl_socket_close(sockets[1]);
            break;
        }
        if (vl_http_spawn_connection(server, runtime, io, sockets[0]) !=
            VL_OK) {
            status = VL_ERROR_INVALID_STATE;
            (void)vl_socket_close(sockets[0]);
            (void)vl_socket_close(sockets[1]);
            break;
        }
        server_fds[index] = sockets[0];
        client_fds[index] = sockets[1];
    }
    if (status != VL_OK) {
        for (index = 0; index < batch_count; ++index) {
            if (server_fds[index] >= 0) {
                (void)vl_socket_close(server_fds[index]);
            }
            if (client_fds[index] >= 0) {
                (void)vl_socket_close(client_fds[index]);
            }
        }
        free(server_fds);
        free(client_fds);
        return status;
    }
    while (vl_http_server_active_connections(server) > 0) {
        if (vl_runtime_run(runtime) != VL_OK) {
            status = VL_ERROR_INVALID_STATE;
            break;
        }
    }
    if (status == VL_OK && clock_gettime(CLOCK_MONOTONIC, &end) == 0) {
        uint64_t batch_ns = elapsed_ns(&start, &end);

        for (index = 0; index < batch_count; ++index) {
            char response[1024];

            if (drain_client_response(client_fds[index], response,
                                      sizeof(response)) == VL_OK) {
                latencies_ns[index] = batch_ns;
            } else {
                latencies_ns[index] = batch_ns;
            }
        }
    } else {
        status = VL_ERROR_SYSTEM;
    }
    for (index = 0; index < batch_count; ++index) {
        if (server_fds[index] >= 0) {
            (void)vl_socket_close(server_fds[index]);
        }
        if (client_fds[index] >= 0) {
            (void)vl_socket_close(client_fds[index]);
        }
    }
    free(server_fds);
    free(client_fds);
    return status;
}

static int run_benchmark(const bench_config_t *config, bench_result_t *result)
{
    vl_runtime_t runtime = {0};
    vl_runtime_config_t runtime_config = {0};
    vl_io_t io = {0};
    vl_io_config_t io_config = {0};
    vl_http_server_t *server = NULL;
    const char *request =
        "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    uint64_t *latencies_ns;
    struct timespec total_start;
    struct timespec total_end;
    size_t completed = 0;
    int status;

    runtime_config.worker_count = config->workers;
    io_config.backend = config->backend;
    io_config.queue_depth = 0;
    latencies_ns = calloc(config->requests, sizeof(*latencies_ns));
    if (latencies_ns == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    if (vl_runtime_init_with_config(&runtime, &runtime_config) != VL_OK ||
        vl_io_init_with_config(&io, &io_config) != VL_OK ||
        vl_http_server_init(&server, NULL) != VL_OK ||
        vl_http_route(server, "GET", "/health", health_handler, NULL) !=
            VL_OK) {
        status = VL_ERROR_INVALID_STATE;
        goto cleanup;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &total_start) != 0) {
        status = VL_ERROR_SYSTEM;
        goto cleanup;
    }

    while (completed < config->requests) {
        size_t batch = config->requests - completed;

        if (batch > config->concurrency) {
            batch = config->concurrency;
        }
        status = run_batch(&runtime, &io, server, request, batch,
                           &latencies_ns[completed]);
        if (status != VL_OK) {
            goto cleanup;
        }
        completed += batch;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &total_end) != 0) {
        status = VL_ERROR_SYSTEM;
        goto cleanup;
    }

    result->duration_ns = elapsed_ns(&total_start, &total_end);
    result->p50_ns = percentile(latencies_ns, config->requests, 50);
    result->p95_ns = percentile(latencies_ns, config->requests, 95);
    result->p99_ns = percentile(latencies_ns, config->requests, 99);
    vl_runtime_get_stats(&runtime, &result->runtime_stats);
    vl_allocator_get_stats(&result->allocator_stats);
    vl_io_get_stats(&io, &result->io_stats);
    result->active_connections = vl_http_server_active_connections(server);
    status = VL_OK;

cleanup:
    vl_http_server_destroy(server);
    vl_io_destroy(&io);
    vl_runtime_shutdown(&runtime);
    free(latencies_ns);
    return status;
}

int main(int argc, char **argv)
{
    bench_config_t config;
    bench_result_t result;
    double qps;
    int status;

    if (parse_args(argc, argv, &config) != VL_OK) {
        fprintf(stderr,
                "usage: %s [--backend epoll|uring] [--workers N] "
                "[--requests N] [--concurrency N]\n",
                argv[0]);
        return 1;
    }
    status = run_benchmark(&config, &result);
    if (status != VL_OK) {
        fprintf(stderr, "bench_http failed: %d\n", status);
        return 1;
    }
    qps = (double)config.requests * 1000000000.0 /
          (double)(result.duration_ns == 0 ? 1 : result.duration_ns);
#if defined(__x86_64__)
    puts("architecture=x86_64");
#elif defined(__aarch64__)
    puts("architecture=aarch64");
#endif
    printf("backend=%s\n",
           config.backend == VL_IO_BACKEND_URING ? "uring" : "epoll");
    printf("worker_count=%zu\n", config.workers);
    printf("requests=%zu\n", config.requests);
    printf("concurrency=%zu\n", config.concurrency);
    printf("duration_ns=%" PRIu64 "\n", result.duration_ns);
    printf("qps=%.0f\n", qps);
    printf("p50_ns=%" PRIu64 "\n", result.p50_ns);
    printf("p95_ns=%" PRIu64 "\n", result.p95_ns);
    printf("p99_ns=%" PRIu64 "\n", result.p99_ns);
    printf("task_switches=%zu\n", result.runtime_stats.task_switches);
    printf("runnable=%zu\n", result.runtime_stats.runnable);
    printf("steals=%zu\n", result.runtime_stats.steals);
    printf("parks=%zu\n", result.runtime_stats.parks);
    printf("mmap_calls=%zu\n", result.allocator_stats.mmap_calls);
    printf("cache_hits=%zu\n", result.allocator_stats.cache_hits);
    printf("central_refills=%zu\n", result.allocator_stats.central_refills);
    printf("io_submissions=%zu\n", result.io_stats.submissions);
    printf("io_completions=%zu\n", result.io_stats.completions);
    printf("io_cancellations=%zu\n", result.io_stats.cancellations);
    printf("active_connections=%zu\n", result.active_connections);
    return 0;
}
