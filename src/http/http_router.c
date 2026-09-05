#include "http_internal.h"

#include <stdlib.h>
#include <string.h>

int vl_http_server_init(vl_http_server_t **server,
                        const vl_http_config_t *config)
{
    vl_http_server_t *impl;

    if (server == NULL || *server != NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    impl = calloc(1, sizeof(*impl));
    if (impl == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    vl_http_config_default(&impl->config);
    if (config != NULL) {
        impl->config = *config;
    }
    if (impl->config.max_connections == 0) {
        impl->config.max_connections = VL_HTTP_DEFAULT_MAX_CONNECTIONS;
    }
    *server = impl;
    return VL_OK;
}

void vl_http_server_destroy(vl_http_server_t *server)
{
    free(server);
}

int vl_http_route(vl_http_server_t *server, const char *method,
                  const char *path, vl_http_handler_t handler,
                  void *user_data)
{
    vl_http_route_entry_t *route;

    if (server == NULL || method == NULL || path == NULL || handler == NULL ||
        strlen(method) >= VL_HTTP_MAX_METHOD ||
        strlen(path) >= VL_HTTP_MAX_PATH) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (server->route_count >= VL_HTTP_ROUTE_CAPACITY) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    route = &server->routes[server->route_count++];
    strcpy(route->method, method);
    strcpy(route->path, path);
    route->handler = handler;
    route->user_data = user_data;
    return VL_OK;
}

const vl_http_route_entry_t *vl_http_find_route(
    const vl_http_server_t *server, const char *method, const char *path)
{
    size_t index;

    if (server == NULL || method == NULL || path == NULL) {
        return NULL;
    }
    for (index = 0; index < server->route_count; ++index) {
        const vl_http_route_entry_t *route = &server->routes[index];

        if (strcmp(route->method, method) == 0 &&
            strcmp(route->path, path) == 0) {
            return route;
        }
    }
    return NULL;
}
