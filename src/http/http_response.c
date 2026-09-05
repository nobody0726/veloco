#include "http_internal.h"

#include <veloco/task.h>

#include <stdio.h>
#include <string.h>

const char *vl_http_reason_phrase(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 413:
        return "Payload Too Large";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    default:
        return "OK";
    }
}

void vl_http_response_init(vl_http_response_t *response)
{
    if (response == NULL) {
        return;
    }
    memset(response, 0, sizeof(*response));
    response->status = 200;
    response->reason = vl_http_reason_phrase(200);
}

int vl_http_response_set_status(vl_http_response_t *response, int status)
{
    if (response == NULL || status < 100 || status > 599) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    response->status = status;
    response->reason = vl_http_reason_phrase(status);
    return VL_OK;
}

int vl_http_response_set_body(vl_http_response_t *response, const char *body,
                              size_t length)
{
    if (response == NULL || (body == NULL && length != 0) ||
        length >= sizeof(response->body)) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (length != 0) {
        memcpy(response->body, body, length);
    }
    response->body[length] = '\0';
    response->body_length = length;
    return VL_OK;
}

static int vl_http_send_all(vl_io_t *io, int fd, const char *data,
                            size_t length)
{
    size_t sent = 0;

    while (sent < length) {
        vl_io_request_t request = {0};

        request.op = VL_IO_SEND;
        request.fd = fd;
        request.buf = (void *)(data + sent);
        request.len = length - sent;
        request.generation = vl_socket_generation(fd);
        request.task = vl_task_current();
        if (vl_io_submit(io, &request) != VL_OK || request.result < 0) {
            return VL_ERROR_SYSTEM;
        }
        sent += (size_t)request.result;
    }
    return VL_OK;
}

int vl_http_response_write(vl_io_t *io, int fd, vl_http_response_t *response)
{
    char head[512];
    int head_length;

    if (io == NULL || fd < 0 || response == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (response->chunked) {
        head_length = snprintf(head, sizeof(head),
                               "HTTP/1.1 %d %s\r\n"
                               "Transfer-Encoding: chunked\r\n"
                               "Connection: %s\r\n"
                               "\r\n",
                               response->status, response->reason,
                               response->keep_alive ? "keep-alive"
                                                   : "close");
    } else {
        head_length = snprintf(head, sizeof(head),
                               "HTTP/1.1 %d %s\r\n"
                               "Content-Length: %zu\r\n"
                               "Connection: %s\r\n"
                               "\r\n",
                               response->status, response->reason,
                               response->body_length,
                               response->keep_alive ? "keep-alive"
                                                   : "close");
    }
    if (head_length < 0 || (size_t)head_length >= sizeof(head)) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (vl_http_send_all(io, fd, head, (size_t)head_length) != VL_OK) {
        return VL_ERROR_SYSTEM;
    }
    if (response->chunked) {
        char chunk_head[32];
        int chunk_head_length;

        chunk_head_length = snprintf(chunk_head, sizeof(chunk_head), "%zx\r\n",
                                     response->body_length);
        if (chunk_head_length < 0 ||
            (size_t)chunk_head_length >= sizeof(chunk_head)) {
            return VL_ERROR_INVALID_ARGUMENT;
        }
        if (vl_http_send_all(io, fd, chunk_head,
                             (size_t)chunk_head_length) != VL_OK) {
            return VL_ERROR_SYSTEM;
        }
        if (response->body_length != 0 &&
            vl_http_send_all(io, fd, response->body, response->body_length) !=
                VL_OK) {
            return VL_ERROR_SYSTEM;
        }
        if (vl_http_send_all(io, fd, "\r\n0\r\n\r\n", 7) != VL_OK) {
            return VL_ERROR_SYSTEM;
        }
    } else if (response->body_length != 0 &&
               vl_http_send_all(io, fd, response->body,
                                response->body_length) != VL_OK) {
        return VL_ERROR_SYSTEM;
    }
    return VL_OK;
}
