#include "test.h"

#include <veloco/http.h>
#include <veloco/io.h>
#include <veloco/runtime.h>

#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static void health_handler(const vl_http_request_t *request,
                           vl_http_response_t *response, void *user_data)
{
    (void)request;
    (void)user_data;
    VL_ASSERT(vl_http_response_set_body(response, "ok", 2) == VL_OK);
}

static void streaming_handler(const vl_http_request_t *request,
                              vl_http_response_t *response, void *user_data)
{
    (void)request;
    (void)user_data;
    response->chunked = 1;
    VL_ASSERT(vl_http_response_set_body(response, "ok", 2) == VL_OK);
}

VL_TEST(http_router_writes_fixed_length_response_from_connection_task)
{
    vl_runtime_t runtime = {0};
    vl_io_t io = {0};
    vl_http_server_t *server = NULL;
    char request[] = "GET /health HTTP/1.1\r\n"
                     "Host: localhost\r\n"
                     "Connection: close\r\n"
                     "\r\n";
    char response[256] = {0};
    int sockets[2] = {-1, -1};
    const vl_io_config_t io_config = {VL_IO_BACKEND_EPOLL, 0};

    VL_REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          0, sockets) == 0);
    VL_REQUIRE(vl_socket_track(sockets[0]) == VL_OK);
    VL_REQUIRE(vl_socket_track(sockets[1]) == VL_OK);
    VL_REQUIRE(vl_runtime_init(&runtime) == VL_OK);
    VL_REQUIRE(vl_io_init_with_config(&io, &io_config) == VL_OK);
    VL_REQUIRE(vl_http_server_init(&server, NULL) == VL_OK);
    VL_REQUIRE(vl_http_route(server, "GET", "/health", health_handler,
                             NULL) == VL_OK);
    VL_REQUIRE(write(sockets[1], request, sizeof(request) - 1) ==
               (ssize_t)(sizeof(request) - 1));
    VL_REQUIRE(vl_http_spawn_connection(server, &runtime, &io, sockets[0]) ==
               VL_OK);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_REQUIRE(read(sockets[1], response, sizeof(response) - 1) > 0);
    VL_ASSERT(strstr(response, "HTTP/1.1 200 OK\r\n") != NULL);
    VL_ASSERT(strstr(response, "Content-Length: 2\r\n") != NULL);
    VL_ASSERT(strstr(response, "\r\n\r\nok") != NULL);

    vl_http_server_destroy(server);
    vl_io_destroy(&io);
    vl_runtime_shutdown(&runtime);
    VL_REQUIRE(vl_socket_close(sockets[1]) == VL_OK);
}

VL_TEST(http_server_returns_bounded_error_for_malformed_request)
{
    vl_runtime_t runtime = {0};
    vl_io_t io = {0};
    vl_http_server_t *server = NULL;
    char request[] = "GET / HTTP/1.1\r\n"
                     "Bad header\r\n"
                     "\r\n";
    char response[256] = {0};
    int sockets[2] = {-1, -1};
    const vl_io_config_t io_config = {VL_IO_BACKEND_EPOLL, 0};

    VL_REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          0, sockets) == 0);
    VL_REQUIRE(vl_socket_track(sockets[0]) == VL_OK);
    VL_REQUIRE(vl_socket_track(sockets[1]) == VL_OK);
    VL_REQUIRE(vl_runtime_init(&runtime) == VL_OK);
    VL_REQUIRE(vl_io_init_with_config(&io, &io_config) == VL_OK);
    VL_REQUIRE(vl_http_server_init(&server, NULL) == VL_OK);
    VL_REQUIRE(write(sockets[1], request, sizeof(request) - 1) ==
               (ssize_t)(sizeof(request) - 1));
    VL_REQUIRE(vl_http_spawn_connection(server, &runtime, &io, sockets[0]) ==
               VL_OK);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_REQUIRE(read(sockets[1], response, sizeof(response) - 1) > 0);
    VL_ASSERT(strstr(response, "HTTP/1.1 400 Bad Request\r\n") != NULL);
    VL_ASSERT(strstr(response, "Content-Length: 11\r\n") != NULL);
    VL_ASSERT(strstr(response, "\r\n\r\nBad Request") != NULL);

    vl_http_server_destroy(server);
    vl_io_destroy(&io);
    vl_runtime_shutdown(&runtime);
    VL_REQUIRE(vl_socket_close(sockets[1]) == VL_OK);
}

VL_TEST(http_router_writes_chunked_response_from_connection_task)
{
    vl_runtime_t runtime = {0};
    vl_io_t io = {0};
    vl_http_server_t *server = NULL;
    char request[] = "GET /stream HTTP/1.1\r\n"
                     "Host: localhost\r\n"
                     "Connection: close\r\n"
                     "\r\n";
    char response[256] = {0};
    int sockets[2] = {-1, -1};
    const vl_io_config_t io_config = {VL_IO_BACKEND_EPOLL, 0};

    VL_REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          0, sockets) == 0);
    VL_REQUIRE(vl_socket_track(sockets[0]) == VL_OK);
    VL_REQUIRE(vl_socket_track(sockets[1]) == VL_OK);
    VL_REQUIRE(vl_runtime_init(&runtime) == VL_OK);
    VL_REQUIRE(vl_io_init_with_config(&io, &io_config) == VL_OK);
    VL_REQUIRE(vl_http_server_init(&server, NULL) == VL_OK);
    VL_REQUIRE(vl_http_route(server, "GET", "/stream", streaming_handler,
                             NULL) == VL_OK);
    VL_REQUIRE(write(sockets[1], request, sizeof(request) - 1) ==
               (ssize_t)(sizeof(request) - 1));
    VL_REQUIRE(vl_http_spawn_connection(server, &runtime, &io, sockets[0]) ==
               VL_OK);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_REQUIRE(read(sockets[1], response, sizeof(response) - 1) > 0);
    VL_ASSERT(strstr(response, "HTTP/1.1 200 OK\r\n") != NULL);
    VL_ASSERT(strstr(response, "Transfer-Encoding: chunked\r\n") != NULL);
    VL_ASSERT(strstr(response, "\r\n2\r\nok\r\n0\r\n\r\n") != NULL);

    vl_http_server_destroy(server);
    vl_io_destroy(&io);
    vl_runtime_shutdown(&runtime);
    VL_REQUIRE(vl_socket_close(sockets[1]) == VL_OK);
}

void vl_register_http_server_tests(void)
{
    vl_test_add("http_router_writes_fixed_length_response_from_connection_task",
                http_router_writes_fixed_length_response_from_connection_task);
    vl_test_add("http_server_returns_bounded_error_for_malformed_request",
                http_server_returns_bounded_error_for_malformed_request);
    vl_test_add("http_router_writes_chunked_response_from_connection_task",
                http_router_writes_chunked_response_from_connection_task);
}
