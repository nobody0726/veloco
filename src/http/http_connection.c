#include "http_internal.h"

#include <veloco/task.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct vl_http_connection_task {
    vl_http_server_t *server;
    vl_io_t *io;
    int fd;
} vl_http_connection_task_t;

static int vl_http_error_response(vl_io_t *io, int fd, int status)
{
    vl_http_response_t response;

    vl_http_response_init(&response);
    (void)vl_http_response_set_status(&response, status);
    if (status == 400) {
        (void)vl_http_response_set_body(&response, "Bad Request", 11);
    } else if (status == 413) {
        (void)vl_http_response_set_body(&response, "Payload Too Large", 17);
    } else if (status == 431) {
        (void)vl_http_response_set_body(&response, "Header Too Large", 16);
    } else {
        (void)vl_http_response_set_body(&response, "Not Found", 9);
    }
    return vl_http_response_write(io, fd, &response);
}

static int vl_http_read_request(vl_http_server_t *server, vl_io_t *io, int fd,
                                vl_http_parser_t *parser)
{
    char buffer[1024];

    for (;;) {
        vl_io_request_t request = {0};
        vl_http_parse_status_t status;

        request.op = VL_IO_RECV;
        request.fd = fd;
        request.buf = buffer;
        request.len = sizeof(buffer);
        request.generation = vl_socket_generation(fd);
        request.task = vl_task_current();
        if (vl_io_submit(io, &request) != VL_OK) {
            return VL_ERROR_SYSTEM;
        }
        if (request.result <= 0) {
            return request.result == 0 ? VL_ERROR_CLOSED : VL_ERROR_SYSTEM;
        }
        status = vl_http_parser_feed(parser, buffer, (size_t)request.result);
        if (status == VL_HTTP_PARSE_COMPLETE) {
            return VL_OK;
        }
        if (status == VL_HTTP_PARSE_ERROR) {
            int http_status = (int)vl_http_parser_error(parser);

            if (http_status == 0) {
                http_status = 400;
            }
            (void)vl_http_error_response(io, fd, http_status);
            return VL_ERROR_INVALID_ARGUMENT;
        }
        if (atomic_load_explicit(&server->shutdown_requested,
                                 memory_order_acquire)) {
            return VL_ERROR_CANCELLED;
        }
    }
}

int vl_http_connection_run(vl_http_server_t *server, vl_io_t *io, int fd)
{
    vl_http_parser_t parser;
    vl_http_response_t response;
    const vl_http_request_t *request;
    const vl_http_route_entry_t *route;
    int status;

    if (server == NULL || io == NULL || fd < 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    vl_http_parser_init(&parser, &server->config);
    status = vl_http_read_request(server, io, fd, &parser);
    if (status != VL_OK) {
        vl_http_parser_destroy(&parser);
        return status == VL_ERROR_INVALID_ARGUMENT ? VL_OK : status;
    }
    request = vl_http_parser_request(&parser);
    if (request == NULL) {
        vl_http_parser_destroy(&parser);
        return VL_ERROR_INVALID_STATE;
    }
    route = vl_http_find_route(server, request->method, request->path);
    if (route == NULL) {
        vl_http_parser_destroy(&parser);
        return vl_http_error_response(io, fd, 404);
    }
    vl_http_response_init(&response);
    response.keep_alive = request->keep_alive;
    route->handler(request, &response, route->user_data);
    status = vl_http_response_write(io, fd, &response);
    vl_http_parser_destroy(&parser);
    return status;
}

static void vl_http_connection_task(void *argument)
{
    vl_http_connection_task_t *task = argument;

    (void)vl_http_connection_run(task->server, task->io, task->fd);
    (void)vl_socket_close(task->fd);
    (void)atomic_fetch_sub_explicit(&task->server->active_connections, 1,
                                    memory_order_acq_rel);
    free(task);
}

int vl_http_spawn_connection(vl_http_server_t *server, vl_runtime_t *runtime,
                             vl_io_t *io, int fd)
{
    vl_http_connection_task_t *task;

    if (server == NULL || runtime == NULL || io == NULL || fd < 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (atomic_load_explicit(&server->shutdown_requested,
                             memory_order_acquire) ||
        atomic_load_explicit(&server->active_connections,
                             memory_order_acquire) >=
            server->config.max_connections) {
        return VL_ERROR_CLOSED;
    }
    task = calloc(1, sizeof(*task));
    if (task == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    task->server = server;
    task->io = io;
    task->fd = fd;
    atomic_fetch_add_explicit(&server->active_connections, 1,
                              memory_order_acq_rel);
    if (vl_spawn(runtime, vl_http_connection_task, task) == NULL) {
        atomic_fetch_sub_explicit(&server->active_connections, 1,
                                  memory_order_acq_rel);
        free(task);
        return VL_ERROR_INVALID_STATE;
    }
    return VL_OK;
}
