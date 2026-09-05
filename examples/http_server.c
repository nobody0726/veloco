#include <veloco/http.h>
#include <veloco/io.h>
#include <veloco/runtime.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void health(const vl_http_request_t *request,
                   vl_http_response_t *response, void *user_data)
{
    (void)request;
    (void)user_data;
    (void)vl_http_response_set_body(response, "ok", 2);
}

int main(void)
{
    vl_runtime_t runtime = {0};
    vl_io_t io = {0};
    vl_http_server_t *server = NULL;
    uint16_t port = 0;
    int listener;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (vl_runtime_init(&runtime) != VL_OK || vl_io_init(&io) != VL_OK ||
        vl_http_server_init(&server, NULL) != VL_OK ||
        vl_http_route(server, "GET", "/health", health, NULL) != VL_OK) {
        fprintf(stderr, "failed to initialize veloco-httpd\n");
        return 1;
    }
    listener = vl_http_server_listen_loopback(server, 8080, 128, &port);
    if (listener < 0) {
        fprintf(stderr, "failed to listen on loopback: %d\n", listener);
        return 1;
    }
    printf("veloco-httpd listening on 127.0.0.1:%u\n", port);
    while (!stop_requested) {
        int fd = vl_socket_accept(listener);

        if (fd >= 0) {
            (void)vl_http_spawn_connection(server, &runtime, &io, fd);
            (void)vl_runtime_run(&runtime);
        }
    }
    vl_http_server_request_shutdown(server);
    (void)vl_socket_close(listener);
    while (vl_http_server_active_connections(server) > 0) {
        (void)vl_runtime_run(&runtime);
    }
    vl_http_server_destroy(server);
    vl_io_destroy(&io);
    vl_runtime_shutdown(&runtime);
    return 0;
}
