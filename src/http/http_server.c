#include "http_internal.h"

void vl_http_server_request_shutdown(vl_http_server_t *server)
{
    if (server != NULL) {
        server->shutdown_requested = 1;
    }
}

int vl_http_server_listen_loopback(vl_http_server_t *server, uint16_t port,
                                   int backlog, uint16_t *bound_port)
{
    if (server == NULL || server->shutdown_requested) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    return vl_socket_listen_loopback(port, backlog, bound_port);
}
