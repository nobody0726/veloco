#include "test.h"

#include <veloco/io.h>
#include <veloco/runtime.h>
#include <veloco/task.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int poll_once(vl_io_t *io, vl_io_completion_t *completion)
{
    return vl_io_poll(io, 1000, completion);
}

static int io_init_epoll(vl_io_t *io)
{
    const vl_io_config_t config = {VL_IO_BACKEND_EPOLL, 0};

    return vl_io_init_with_config(io, &config);
}

VL_TEST(io_default_selects_an_available_backend)
{
    vl_io_t io = {0};
    vl_io_backend_t backend;

    VL_REQUIRE(vl_io_init(&io) == VL_OK);
    backend = vl_io_backend(&io);
    VL_ASSERT(backend == VL_IO_BACKEND_EPOLL ||
              backend == VL_IO_BACKEND_URING);
    vl_io_destroy(&io);
}

VL_TEST(io_loopback_connect_accept_send_receive_and_eof)
{
    vl_io_t io = {0};
    vl_io_completion_t completion;
    vl_io_request_t request;
    uint16_t port;
    uint64_t listener_generation;
    int listener = -1;
    int client = -1;
    int accepted = -1;
    char received[32] = {0};
    char message[] = "veloco";

    VL_REQUIRE(io_init_epoll(&io) == VL_OK);
    listener = vl_socket_listen_loopback(0, 8, &port);
    VL_REQUIRE(listener >= 0);
    listener_generation = vl_socket_generation(listener);
    VL_ASSERT(listener_generation != 0);
    client = vl_socket_connect_loopback(port);
    VL_REQUIRE(client >= 0);

    memset(&request, 0, sizeof(request));
    request.op = VL_IO_CONNECT;
    request.fd = client;
    request.generation = vl_socket_generation(client);
    VL_REQUIRE(vl_io_submit(&io, &request) == VL_OK);
    VL_REQUIRE(poll_once(&io, &completion) == VL_OK);
    VL_ASSERT(completion.request == &request);
    VL_ASSERT(completion.task == NULL);
    VL_ASSERT(completion.generation == request.generation);
    VL_ASSERT(completion.result == 0);

    memset(&request, 0, sizeof(request));
    request.op = VL_IO_ACCEPT;
    request.fd = listener;
    request.generation = listener_generation;
    VL_REQUIRE(vl_io_submit(&io, &request) == VL_OK);
    VL_REQUIRE(poll_once(&io, &completion) == VL_OK);
    VL_REQUIRE(completion.result >= 0);
    accepted = (int)completion.result;
    VL_ASSERT(vl_socket_generation(accepted) != 0);

    memset(&request, 0, sizeof(request));
    request.op = VL_IO_SEND;
    request.fd = client;
    request.buf = message;
    request.len = sizeof(message) - 1;
    request.generation = vl_socket_generation(client);
    VL_REQUIRE(vl_io_submit(&io, &request) == VL_OK);
    VL_REQUIRE(poll_once(&io, &completion) == VL_OK);
    VL_ASSERT(completion.result > 0);
    VL_ASSERT((size_t)completion.result <= sizeof(message) - 1);

    memset(&request, 0, sizeof(request));
    request.op = VL_IO_RECV;
    request.fd = accepted;
    request.buf = received;
    request.len = 3;
    request.generation = vl_socket_generation(accepted);
    VL_REQUIRE(vl_io_submit(&io, &request) == VL_OK);
    VL_REQUIRE(poll_once(&io, &completion) == VL_OK);
    VL_ASSERT(completion.result == 3);
    VL_ASSERT(memcmp(received, message, 3) == 0);

    memset(&request, 0, sizeof(request));
    request.op = VL_IO_RECV;
    request.fd = accepted;
    request.buf = received + 3;
    request.len = sizeof(received) - 3;
    request.generation = vl_socket_generation(accepted);
    VL_REQUIRE(vl_io_submit(&io, &request) == VL_OK);
    VL_REQUIRE(poll_once(&io, &completion) == VL_OK);
    VL_ASSERT(completion.result == (ssize_t)(sizeof(message) - 4));
    VL_ASSERT(memcmp(received, message, sizeof(message) - 1) == 0);

    VL_REQUIRE(vl_socket_close(client) == VL_OK);
    memset(&request, 0, sizeof(request));
    request.op = VL_IO_RECV;
    request.fd = accepted;
    request.buf = received;
    request.len = sizeof(received);
    request.generation = vl_socket_generation(accepted);
    VL_REQUIRE(vl_io_submit(&io, &request) == VL_OK);
    VL_REQUIRE(poll_once(&io, &completion) == VL_OK);
    VL_ASSERT(completion.result == 0);
    VL_ASSERT((completion.events & VL_IO_EVENT_EOF) != 0);

    VL_REQUIRE(vl_socket_close(accepted) == VL_OK);
    VL_REQUIRE(vl_socket_close(listener) == VL_OK);
    vl_io_destroy(&io);
}

typedef struct io_task_fixture {
    vl_io_t *io;
    int reader_fd;
    int writer_fd;
    vl_io_request_t request;
    int submit_status;
    char received;
} io_task_fixture_t;

static void io_reader_task(void *argument)
{
    io_task_fixture_t *fixture = argument;

    fixture->request.op = VL_IO_RECV;
    fixture->request.fd = fixture->reader_fd;
    fixture->request.buf = &fixture->received;
    fixture->request.len = 1;
    fixture->request.generation = vl_socket_generation(fixture->reader_fd);
    fixture->request.task = vl_task_current();
    fixture->submit_status = vl_io_submit(fixture->io, &fixture->request);
}

static void io_writer_task(void *argument)
{
    io_task_fixture_t *fixture = argument;
    char byte = 'x';

    VL_ASSERT(send(fixture->writer_fd, &byte, 1, MSG_NOSIGNAL) == 1);
}

VL_TEST(io_completion_wakes_waiting_task)
{
    vl_runtime_t runtime = {0};
    vl_io_t io = {0};
    io_task_fixture_t fixture = {0};
    int sockets[2] = {-1, -1};

    VL_REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          0, sockets) == 0);
    VL_REQUIRE(vl_socket_track(sockets[0]) == VL_OK);
    VL_REQUIRE(vl_socket_track(sockets[1]) == VL_OK);
    VL_REQUIRE(io_init_epoll(&io) == VL_OK);
    VL_REQUIRE(vl_runtime_init(&runtime) == VL_OK);
    fixture.io = &io;
    fixture.reader_fd = sockets[0];
    fixture.writer_fd = sockets[1];
    fixture.submit_status = VL_ERROR_INVALID_STATE;
    VL_REQUIRE(vl_spawn(&runtime, io_reader_task, &fixture) != NULL);
    VL_REQUIRE(vl_spawn(&runtime, io_writer_task, &fixture) != NULL);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(fixture.submit_status == VL_OK);
    VL_ASSERT(fixture.request.completed == 1);
    VL_ASSERT(fixture.request.result == 1);
    VL_ASSERT(fixture.received == 'x');
    VL_ASSERT((fixture.request.events & VL_IO_EVENT_READABLE) != 0);

    vl_io_destroy(&io);
    vl_runtime_shutdown(&runtime);
    VL_REQUIRE(vl_socket_close(sockets[0]) == VL_OK);
    VL_REQUIRE(vl_socket_close(sockets[1]) == VL_OK);
}

typedef struct io_cancel_task_fixture {
    vl_io_t *io;
    int fd;
    vl_io_request_t request;
    int submit_status;
    int cancel_status;
    char byte;
} io_cancel_task_fixture_t;

static void io_cancelled_reader_task(void *argument)
{
    io_cancel_task_fixture_t *fixture = argument;

    fixture->request.op = VL_IO_RECV;
    fixture->request.fd = fixture->fd;
    fixture->request.buf = &fixture->byte;
    fixture->request.len = 1;
    fixture->request.generation = vl_socket_generation(fixture->fd);
    fixture->request.task = vl_task_current();
    fixture->submit_status = vl_io_submit(fixture->io, &fixture->request);
}

static void io_canceller_task(void *argument)
{
    io_cancel_task_fixture_t *fixture = argument;

    fixture->cancel_status = vl_io_cancel(fixture->io, &fixture->request);
}

VL_TEST(io_cancellation_wakes_waiting_task)
{
    vl_runtime_t runtime = {0};
    vl_io_t io = {0};
    io_cancel_task_fixture_t fixture = {0};
    int sockets[2] = {-1, -1};

    VL_REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          0, sockets) == 0);
    VL_REQUIRE(vl_socket_track(sockets[0]) == VL_OK);
    VL_REQUIRE(vl_socket_track(sockets[1]) == VL_OK);
    VL_REQUIRE(io_init_epoll(&io) == VL_OK);
    VL_REQUIRE(vl_runtime_init(&runtime) == VL_OK);
    fixture.io = &io;
    fixture.fd = sockets[0];
    fixture.submit_status = VL_ERROR_INVALID_STATE;
    fixture.cancel_status = VL_ERROR_INVALID_STATE;
    VL_REQUIRE(vl_spawn(&runtime, io_cancelled_reader_task, &fixture) != NULL);
    VL_REQUIRE(vl_spawn(&runtime, io_canceller_task, &fixture) != NULL);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(fixture.cancel_status == VL_OK);
    VL_ASSERT(fixture.submit_status == VL_OK);
    VL_ASSERT(fixture.request.completed == 1);
    VL_ASSERT(fixture.request.result == -ECANCELED);

    vl_io_destroy(&io);
    vl_runtime_shutdown(&runtime);
    VL_REQUIRE(vl_socket_close(sockets[0]) == VL_OK);
    VL_REQUIRE(vl_socket_close(sockets[1]) == VL_OK);
}

typedef struct io_shutdown_fixture {
    vl_runtime_t *runtime;
    io_cancel_task_fixture_t *reader;
} io_shutdown_fixture_t;

static void io_shutdown_task(void *argument)
{
    io_shutdown_fixture_t *fixture = argument;

    vl_runtime_request_shutdown(fixture->runtime);
}

VL_TEST(io_pending_request_requires_backend_teardown_before_runtime_teardown)
{
    vl_runtime_t runtime = {0};
    vl_io_t io = {0};
    io_cancel_task_fixture_t reader = {0};
    io_shutdown_fixture_t shutdown_fixture;
    vl_task_t *reader_task;
    int sockets[2] = {-1, -1};

    VL_REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          0, sockets) == 0);
    VL_REQUIRE(vl_socket_track(sockets[0]) == VL_OK);
    VL_REQUIRE(vl_socket_track(sockets[1]) == VL_OK);
    VL_REQUIRE(io_init_epoll(&io) == VL_OK);
    VL_REQUIRE(vl_runtime_init(&runtime) == VL_OK);
    reader.io = &io;
    reader.fd = sockets[0];
    reader.submit_status = VL_ERROR_INVALID_STATE;
    shutdown_fixture.runtime = &runtime;
    shutdown_fixture.reader = &reader;
    reader_task = vl_spawn(&runtime, io_cancelled_reader_task, &reader);
    VL_REQUIRE(reader_task != NULL);
    VL_REQUIRE(vl_spawn(&runtime, io_shutdown_task, &shutdown_fixture) != NULL);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(vl_task_state(reader_task) == VL_TASK_CANCELLED);
    vl_runtime_shutdown(&runtime);
    VL_ASSERT(runtime.impl != NULL);
    vl_io_destroy(&io);
    vl_runtime_shutdown(&runtime);
    VL_ASSERT(runtime.impl == NULL);
    VL_REQUIRE(vl_socket_close(sockets[0]) == VL_OK);
    VL_REQUIRE(vl_socket_close(sockets[1]) == VL_OK);
}

VL_TEST(io_nonblocking_send_can_complete_partially)
{
    enum { PAYLOAD_SIZE = 1024 * 1024 };
    vl_io_t io = {0};
    vl_io_completion_t completion;
    vl_io_request_t request = {0};
    int sockets[2] = {-1, -1};
    int send_buffer = 4096;
    unsigned char *payload;

    VL_REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          0, sockets) == 0);
    VL_REQUIRE(vl_socket_track(sockets[0]) == VL_OK);
    VL_REQUIRE(vl_socket_track(sockets[1]) == VL_OK);
    VL_REQUIRE(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer,
                          sizeof(send_buffer)) == 0);
    payload = malloc(PAYLOAD_SIZE);
    VL_REQUIRE(payload != NULL);
    memset(payload, 0x5a, PAYLOAD_SIZE);
    VL_REQUIRE(io_init_epoll(&io) == VL_OK);

    request.op = VL_IO_SEND;
    request.fd = sockets[0];
    request.buf = payload;
    request.len = PAYLOAD_SIZE;
    request.generation = vl_socket_generation(sockets[0]);
    VL_REQUIRE(vl_io_submit(&io, &request) == VL_OK);
    VL_REQUIRE(poll_once(&io, &completion) == VL_OK);
    VL_ASSERT(completion.result > 0);
    VL_ASSERT(completion.result < PAYLOAD_SIZE);
    VL_ASSERT((completion.events & VL_IO_EVENT_WRITABLE) != 0);

    vl_io_destroy(&io);
    free(payload);
    VL_REQUIRE(vl_socket_close(sockets[0]) == VL_OK);
    VL_REQUIRE(vl_socket_close(sockets[1]) == VL_OK);
}

VL_TEST(io_cancel_returns_owned_completion)
{
    vl_io_t io = {0};
    vl_io_completion_t completion;
    vl_io_request_t request = {0};
    uint16_t port;
    int listener;

    VL_REQUIRE(io_init_epoll(&io) == VL_OK);
    listener = vl_socket_listen_loopback(0, 8, &port);
    VL_REQUIRE(listener >= 0);
    request.op = VL_IO_ACCEPT;
    request.fd = listener;
    request.generation = vl_socket_generation(listener);
    VL_REQUIRE(vl_io_submit(&io, &request) == VL_OK);
    VL_REQUIRE(vl_io_cancel(&io, &request) == VL_OK);
    VL_REQUIRE(vl_io_poll(&io, 0, &completion) == VL_OK);
    VL_ASSERT(completion.request == &request);
    VL_ASSERT(completion.result == -ECANCELED);
    VL_ASSERT(completion.generation == request.generation);
    VL_ASSERT(vl_io_poll(&io, 0, &completion) == VL_ERROR_WOULD_BLOCK);
    VL_ASSERT(vl_socket_close(listener) == VL_OK);
    vl_io_destroy(&io);
}

VL_TEST(io_close_before_wakeup_reports_stale_generation)
{
    vl_io_t io = {0};
    vl_io_completion_t completion;
    vl_io_request_t request = {0};
    uint16_t port;
    uint64_t generation;
    int listener;
    int reuse_source[2] = {-1, -1};

    VL_REQUIRE(io_init_epoll(&io) == VL_OK);
    listener = vl_socket_listen_loopback(0, 8, &port);
    VL_REQUIRE(listener >= 0);
    generation = vl_socket_generation(listener);
    request.op = VL_IO_ACCEPT;
    request.fd = listener;
    request.generation = generation;
    VL_REQUIRE(vl_io_submit(&io, &request) == VL_OK);
    VL_REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          0, reuse_source) == 0);
    VL_REQUIRE(vl_socket_close(listener) == VL_OK);
    VL_REQUIRE(dup2(reuse_source[0], listener) == listener);
    VL_REQUIRE(vl_socket_track(listener) == VL_OK);
    VL_ASSERT(vl_socket_generation(listener) > generation);
    VL_REQUIRE(vl_io_poll(&io, 0, &completion) == VL_OK);
    VL_ASSERT(completion.request == &request);
    VL_ASSERT(completion.result == -ESTALE);
    VL_REQUIRE(vl_socket_close(listener) == VL_OK);
    VL_REQUIRE(close(reuse_source[0]) == 0);
    VL_REQUIRE(close(reuse_source[1]) == 0);
    vl_io_destroy(&io);
}

VL_TEST(io_rejects_unsupported_and_duplicate_requests)
{
    vl_io_t io = {0};
    vl_io_request_t first = {0};
    vl_io_request_t duplicate = {0};
    uint16_t port;
    int listener;

    VL_REQUIRE(io_init_epoll(&io) == VL_OK);
    listener = vl_socket_listen_loopback(0, 8, &port);
    VL_REQUIRE(listener >= 0);
    first.op = VL_IO_ACCEPT;
    first.fd = listener;
    first.generation = vl_socket_generation(listener);
    VL_REQUIRE(vl_io_submit(&io, &first) == VL_OK);
    duplicate = first;
    VL_ASSERT(vl_io_submit(&io, &duplicate) == VL_ERROR_INVALID_STATE);
    VL_ASSERT(vl_io_cancel(&io, &first) == VL_OK);
    duplicate.op = VL_IO_TIMEOUT;
    VL_ASSERT(vl_io_submit(&io, &duplicate) == VL_ERROR_UNSUPPORTED);
    VL_ASSERT(vl_socket_close(listener) == VL_OK);
    vl_io_destroy(&io);
}

VL_TEST(io_rejects_untracked_descriptors)
{
    vl_io_t io = {0};
    vl_io_request_t request = {0};
    int sockets[2] = {-1, -1};

    VL_REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          0, sockets) == 0);
    VL_REQUIRE(io_init_epoll(&io) == VL_OK);
    request.op = VL_IO_RECV;
    request.fd = sockets[0];
    VL_ASSERT(vl_io_submit(&io, &request) == VL_ERROR_INVALID_STATE);
    vl_io_destroy(&io);
    VL_REQUIRE(close(sockets[0]) == 0);
    VL_REQUIRE(close(sockets[1]) == 0);
}

VL_TEST(io_allows_one_process_wide_waiter_per_descriptor)
{
    vl_io_t first_io = {0};
    vl_io_t second_io = {0};
    vl_io_completion_t completion;
    vl_io_request_t first = {0};
    vl_io_request_t second = {0};
    int sockets[2] = {-1, -1};
    char byte;

    VL_REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          0, sockets) == 0);
    VL_REQUIRE(vl_socket_track(sockets[0]) == VL_OK);
    VL_REQUIRE(vl_socket_track(sockets[1]) == VL_OK);
    VL_REQUIRE(io_init_epoll(&first_io) == VL_OK);
    VL_REQUIRE(io_init_epoll(&second_io) == VL_OK);
    first.op = VL_IO_RECV;
    first.fd = sockets[0];
    first.buf = &byte;
    first.len = 1;
    first.generation = vl_socket_generation(sockets[0]);
    second = first;
    VL_REQUIRE(vl_io_submit(&first_io, &first) == VL_OK);
    VL_ASSERT(vl_io_submit(&second_io, &second) == VL_ERROR_INVALID_STATE);
    VL_REQUIRE(vl_io_cancel(&first_io, &first) == VL_OK);
    VL_REQUIRE(vl_io_poll(&first_io, 0, &completion) == VL_OK);
    VL_REQUIRE(vl_io_submit(&second_io, &second) == VL_OK);
    VL_REQUIRE(vl_io_cancel(&second_io, &second) == VL_OK);
    VL_REQUIRE(vl_io_poll(&second_io, 0, &completion) == VL_OK);
    vl_io_destroy(&second_io);
    vl_io_destroy(&first_io);
    VL_REQUIRE(vl_socket_close(sockets[0]) == VL_OK);
    VL_REQUIRE(vl_socket_close(sockets[1]) == VL_OK);
}

typedef struct io_non_owner_fixture {
    vl_io_t *io;
    int status;
} io_non_owner_fixture_t;

static void *poll_from_non_owner(void *argument)
{
    io_non_owner_fixture_t *fixture = argument;
    vl_io_completion_t completion;

    fixture->status = vl_io_poll(fixture->io, 0, &completion);
    return NULL;
}

VL_TEST(io_rejects_non_owner_thread)
{
    vl_io_t io = {0};
    io_non_owner_fixture_t fixture;
    pthread_t thread;

    VL_REQUIRE(io_init_epoll(&io) == VL_OK);
    fixture.io = &io;
    fixture.status = VL_OK;
    VL_REQUIRE(pthread_create(&thread, NULL, poll_from_non_owner, &fixture) ==
               0);
    VL_REQUIRE(pthread_join(thread, NULL) == 0);
    VL_ASSERT(fixture.status == VL_ERROR_INVALID_STATE);
    vl_io_destroy(&io);
}

void vl_register_io_tests(void)
{
    vl_test_add("io_default_selects_an_available_backend",
                io_default_selects_an_available_backend);
    vl_test_add("io_loopback_connect_accept_send_receive_and_eof",
                io_loopback_connect_accept_send_receive_and_eof);
    vl_test_add("io_cancel_returns_owned_completion",
                io_cancel_returns_owned_completion);
    vl_test_add("io_nonblocking_send_can_complete_partially",
                io_nonblocking_send_can_complete_partially);
    vl_test_add("io_completion_wakes_waiting_task",
                io_completion_wakes_waiting_task);
    vl_test_add("io_cancellation_wakes_waiting_task",
                io_cancellation_wakes_waiting_task);
    vl_test_add(
        "io_pending_request_requires_backend_teardown_before_runtime_teardown",
        io_pending_request_requires_backend_teardown_before_runtime_teardown);
    vl_test_add("io_close_before_wakeup_reports_stale_generation",
                io_close_before_wakeup_reports_stale_generation);
    vl_test_add("io_rejects_unsupported_and_duplicate_requests",
                io_rejects_unsupported_and_duplicate_requests);
    vl_test_add("io_rejects_untracked_descriptors",
                io_rejects_untracked_descriptors);
    vl_test_add("io_allows_one_process_wide_waiter_per_descriptor",
                io_allows_one_process_wide_waiter_per_descriptor);
    vl_test_add("io_rejects_non_owner_thread",
                io_rejects_non_owner_thread);
}
