#ifndef VELOCO_HTTP_INTERNAL_H
#define VELOCO_HTTP_INTERNAL_H

#include <veloco/http.h>

#include <stddef.h>
#include <stdatomic.h>

#define VL_HTTP_ROUTE_CAPACITY 64

typedef struct vl_http_route_entry {
    char method[VL_HTTP_MAX_METHOD];
    char path[VL_HTTP_MAX_PATH];
    vl_http_handler_t handler;
    void *user_data;
} vl_http_route_entry_t;

struct vl_http_server {
    vl_http_config_t config;
    vl_http_route_entry_t routes[VL_HTTP_ROUTE_CAPACITY];
    size_t route_count;
    _Atomic size_t active_connections;
    _Atomic int shutdown_requested;
};

const vl_http_route_entry_t *vl_http_find_route(
    const vl_http_server_t *server, const char *method, const char *path);
const char *vl_http_reason_phrase(int status);
int vl_http_connection_run(vl_http_server_t *server, vl_io_t *io, int fd);

#endif
