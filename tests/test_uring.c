#include <veloco/io.h>
#include <veloco/runtime.h>
#include <veloco/task.h>

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define VL_URING_SKIP 77

static int failures;

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #expression);                                             \
            ++failures;                                                       \
            return;                                                           \
        }                                                                     \
    } while (0)

static int uring_init(vl_io_t *io)
{
    const vl_io_config_t config = {VL_IO_BACKEND_URING, 64};

    return vl_io_init_with_config(io, &config);
}

static int poll_completion(vl_io_t *io, vl_io_request_t *request,
                           vl_io_completion_t *completion)
{
    vl_io_completion_t current;
    int index;

    for (index = 0; index < 8; ++index) {
        int status = vl_io_poll(io, 2000, &current);

        if (status != VL_OK) {
            return status;
        }
        if (current.request == request) {
            *completion = current;
            return VL_OK;
        }
    }
    return VL_ERROR_INVALID_STATE;
}

static void test_explicit_backend_identity(void)
{
    vl_io_t io = {0};

    CHECK(uring_init(&io) == VL_OK);
    CHECK(vl_io_backend(&io) == VL_IO_BACKEND_URING);
    vl_io_destroy(&io);
}

static void test_connect_accept_send_recv_and_timeout(void)
{
    vl_io_t io = {0};
    vl_io_request_t connect_request = {0};
    vl_io_request_t accept_request = {0};
    vl_io_request_t send_request = {0};
    vl_io_request_t recv_request = {0};
    vl_io_request_t timeout_request = {0};
    vl_io_completion_t completion;
    struct sockaddr_in address;
    uint16_t port = 0;
    int listener = -1;
    int client = -1;
    int accepted = -1;
    char message[] = "uring";
    char received[sizeof(message)] = {0};

    CHECK(uring_init(&io) == VL_OK);
    listener = vl_socket_listen_loopback(0, 8, &port);
    CHECK(listener >= 0);
    client = vl_socket_create_tcp();
    CHECK(client >= 0);

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    connect_request.op = VL_IO_CONNECT;
    connect_request.fd = client;
    connect_request.address = (const struct sockaddr *)&address;
    connect_request.address_len = sizeof(address);
    connect_request.generation = vl_socket_generation(client);
    accept_request.op = VL_IO_ACCEPT;
    accept_request.fd = listener;
    accept_request.generation = vl_socket_generation(listener);
    CHECK(vl_io_submit(&io, &connect_request) == VL_OK);
    CHECK(poll_completion(&io, &connect_request, &completion) == VL_OK);
    CHECK(completion.request == &connect_request);
    CHECK(completion.task == connect_request.task);
    CHECK(completion.generation == connect_request.generation);
    CHECK(completion.result == 0);
    CHECK(vl_io_submit(&io, &accept_request) == VL_OK);
    CHECK(poll_completion(&io, &accept_request, &completion) == VL_OK);
    CHECK(completion.request == &accept_request);
    CHECK(completion.task == accept_request.task);
    CHECK(completion.generation == accept_request.generation);
    CHECK(completion.result >= 0);
    accepted = (int)completion.result;
    CHECK(vl_socket_generation(accepted) != 0);

    recv_request.op = VL_IO_RECV;
    recv_request.fd = accepted;
    recv_request.buf = received;
    recv_request.len = sizeof(message) - 1;
    recv_request.generation = vl_socket_generation(accepted);
    send_request.op = VL_IO_SEND;
    send_request.fd = client;
    send_request.buf = message;
    send_request.len = sizeof(message) - 1;
    send_request.generation = vl_socket_generation(client);
    CHECK(vl_io_submit(&io, &send_request) == VL_OK);
    CHECK(poll_completion(&io, &send_request, &completion) == VL_OK);
    CHECK(completion.request == &send_request);
    CHECK(completion.task == send_request.task);
    CHECK(completion.generation == send_request.generation);
    CHECK(completion.result == (ssize_t)(sizeof(message) - 1));
    CHECK(vl_io_submit(&io, &recv_request) == VL_OK);
    CHECK(poll_completion(&io, &recv_request, &completion) == VL_OK);
    CHECK(completion.request == &recv_request);
    CHECK(completion.task == recv_request.task);
    CHECK(completion.generation == recv_request.generation);
    CHECK(completion.result == (ssize_t)(sizeof(message) - 1));
    CHECK(memcmp(message, received, sizeof(message) - 1) == 0);

    timeout_request.op = VL_IO_TIMEOUT;
    timeout_request.timeout_ns = 1000000;
    CHECK(vl_io_submit(&io, &timeout_request) == VL_OK);
    CHECK(poll_completion(&io, &timeout_request, &completion) == VL_OK);
    CHECK(completion.request == &timeout_request);
    CHECK(completion.task == timeout_request.task);
    CHECK(completion.generation == 0);
    CHECK(completion.result == -ETIME);

    CHECK(vl_socket_close(accepted) == VL_OK);
    CHECK(vl_socket_close(client) == VL_OK);
    CHECK(vl_socket_close(listener) == VL_OK);
    vl_io_destroy(&io);
}

static void test_negative_cqe_result(void)
{
    vl_io_t io = {0};
    vl_io_request_t request = {0};
    vl_io_completion_t completion;
    int sockets[2] = {-1, -1};

    CHECK(uring_init(&io) == VL_OK);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                     0, sockets) == 0);
    CHECK(vl_socket_track(sockets[0]) == VL_OK);
    CHECK(vl_socket_track(sockets[1]) == VL_OK);
    request.op = VL_IO_RECV;
    request.fd = sockets[0];
    request.buf = (void *)(uintptr_t)1;
    request.len = 1;
    request.generation = vl_socket_generation(sockets[0]);
    CHECK(vl_io_submit(&io, &request) == VL_OK);
    CHECK(send(sockets[1], "x", 1, MSG_NOSIGNAL) == 1);
    CHECK(poll_completion(&io, &request, &completion) == VL_OK);
    CHECK(completion.request == &request);
    CHECK(completion.task == request.task);
    CHECK(completion.generation == request.generation);
    CHECK(completion.result == -EFAULT);
    CHECK((completion.events & VL_IO_EVENT_ERROR) != 0);
    CHECK(vl_socket_close(sockets[0]) == VL_OK);
    CHECK(vl_socket_close(sockets[1]) == VL_OK);
    vl_io_destroy(&io);
}

static void test_async_cancel_completes_once(void)
{
    vl_io_t io = {0};
    vl_io_request_t request = {0};
    vl_io_completion_t completion;
    int sockets[2] = {-1, -1};
    char byte;
    int iteration;

    CHECK(uring_init(&io) == VL_OK);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                     0, sockets) == 0);
    CHECK(vl_socket_track(sockets[0]) == VL_OK);
    CHECK(vl_socket_track(sockets[1]) == VL_OK);
    for (iteration = 0; iteration < 64; ++iteration) {
        memset(&request, 0, sizeof(request));
        request.op = VL_IO_RECV;
        request.fd = sockets[0];
        request.buf = &byte;
        request.len = 1;
        request.generation = vl_socket_generation(sockets[0]);
        CHECK(vl_io_submit(&io, &request) == VL_OK);
        CHECK(vl_io_cancel(&io, &request) == VL_OK);
        CHECK(poll_completion(&io, &request, &completion) == VL_OK);
        CHECK(completion.request == &request);
        CHECK(completion.task == request.task);
        CHECK(completion.generation == request.generation);
        CHECK(completion.result == -ECANCELED);
        CHECK(vl_io_poll(&io, 0, &completion) == VL_ERROR_WOULD_BLOCK);
    }
    CHECK(vl_socket_close(sockets[0]) == VL_OK);
    CHECK(vl_socket_close(sockets[1]) == VL_OK);
    vl_io_destroy(&io);
}

static void test_close_before_cqe_becomes_stale(void)
{
    vl_io_t io = {0};
    vl_io_request_t request = {0};
    vl_io_completion_t completion;
    int sockets[2] = {-1, -1};
    char byte = 0;

    CHECK(uring_init(&io) == VL_OK);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                     0, sockets) == 0);
    CHECK(vl_socket_track(sockets[0]) == VL_OK);
    CHECK(vl_socket_track(sockets[1]) == VL_OK);
    request.op = VL_IO_RECV;
    request.fd = sockets[0];
    request.buf = &byte;
    request.len = 1;
    request.generation = vl_socket_generation(sockets[0]);
    CHECK(vl_io_submit(&io, &request) == VL_OK);
    CHECK(vl_socket_close(sockets[0]) == VL_OK);
    CHECK(poll_completion(&io, &request, &completion) == VL_OK);
    CHECK(completion.request == &request);
    CHECK(completion.task == request.task);
    CHECK(completion.generation == request.generation);
    CHECK(completion.result == -ESTALE);
    CHECK((completion.events & VL_IO_EVENT_ERROR) != 0);
    CHECK(vl_socket_close(sockets[1]) == VL_OK);
    vl_io_destroy(&io);
}

typedef struct task_fixture {
    vl_io_t *io;
    int reader;
    int writer;
    char byte;
    vl_io_request_t request;
    vl_task_t *task_identity;
    int submit_status;
} task_fixture_t;

static void uring_reader_task(void *argument)
{
    task_fixture_t *fixture = argument;

    fixture->task_identity = vl_task_current();
    fixture->request.op = VL_IO_RECV;
    fixture->request.fd = fixture->reader;
    fixture->request.buf = &fixture->byte;
    fixture->request.len = 1;
    fixture->request.generation = vl_socket_generation(fixture->reader);
    fixture->request.task = fixture->task_identity;
    fixture->submit_status = vl_io_submit(fixture->io, &fixture->request);
}

static void uring_writer_task(void *argument)
{
    task_fixture_t *fixture = argument;
    char byte = 't';

    if (send(fixture->writer, &byte, 1, MSG_NOSIGNAL) != 1) {
        ++failures;
    }
}

static void test_task_identity_survives_cqe(void)
{
    vl_io_t io = {0};
    vl_runtime_t runtime = {0};
    task_fixture_t fixture = {0};
    int sockets[2] = {-1, -1};

    CHECK(uring_init(&io) == VL_OK);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                     0, sockets) == 0);
    CHECK(vl_socket_track(sockets[0]) == VL_OK);
    CHECK(vl_socket_track(sockets[1]) == VL_OK);
    CHECK(vl_runtime_init(&runtime) == VL_OK);
    fixture.io = &io;
    fixture.reader = sockets[0];
    fixture.writer = sockets[1];
    fixture.submit_status = VL_ERROR_INVALID_STATE;
    CHECK(vl_spawn(&runtime, uring_reader_task, &fixture) != NULL);
    CHECK(vl_spawn(&runtime, uring_writer_task, &fixture) != NULL);
    CHECK(vl_runtime_run(&runtime) == VL_OK);
    CHECK(fixture.submit_status == VL_OK);
    CHECK(fixture.request.task == fixture.task_identity);
    CHECK(fixture.request.generation == vl_socket_generation(sockets[0]));
    CHECK(fixture.request.completed == 1);
    CHECK(fixture.request.result == 1);
    CHECK(fixture.byte == 't');
    vl_io_destroy(&io);
    vl_runtime_shutdown(&runtime);
    CHECK(vl_socket_close(sockets[0]) == VL_OK);
    CHECK(vl_socket_close(sockets[1]) == VL_OK);
}

int main(void)
{
    vl_io_t probe = {0};
    int status = uring_init(&probe);

    if (status == VL_ERROR_UNSUPPORTED) {
        fprintf(stderr, "SKIP: kernel does not permit io_uring\n");
        return VL_URING_SKIP;
    }
    if (status != VL_OK) {
        fprintf(stderr, "io_uring probe failed with status %d\n", status);
        return 1;
    }
    vl_io_destroy(&probe);

    test_explicit_backend_identity();
    test_connect_accept_send_recv_and_timeout();
    test_negative_cqe_result();
    test_async_cancel_completes_once();
    test_close_before_cqe_becomes_stale();
    test_task_identity_survives_cqe();
    if (failures != 0) {
        fprintf(stderr, "%d io_uring test failure(s)\n", failures);
        return 1;
    }
    puts("PASS: io_uring backend completion suite");
    return 0;
}
