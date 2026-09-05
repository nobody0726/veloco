#ifndef VELOCO_HTTP_H
#define VELOCO_HTTP_H

#include <veloco/common.h>
#include <veloco/io.h>
#include <veloco/runtime.h>

#include <stddef.h>
#include <stdint.h>

#define VL_HTTP_MAX_METHOD 8
#define VL_HTTP_MAX_PATH 256
#define VL_HTTP_MAX_HEADERS 32
#define VL_HTTP_MAX_HEADER_NAME 64
#define VL_HTTP_MAX_HEADER_VALUE 256
#define VL_HTTP_DEFAULT_MAX_REQUEST_LINE ((size_t)1024)
#define VL_HTTP_DEFAULT_MAX_HEADER_BYTES ((size_t)8192)
#define VL_HTTP_DEFAULT_MAX_BODY_BYTES ((size_t)(64 * 1024))
#define VL_HTTP_DEFAULT_MAX_CONNECTIONS ((size_t)1024)
#define VL_HTTP_DEFAULT_READ_TIMEOUT_NS UINT64_C(5000000000)
#define VL_HTTP_DEFAULT_WRITE_TIMEOUT_NS UINT64_C(5000000000)

typedef struct vl_http_config {
    size_t max_request_line;
    size_t max_header_bytes;
    size_t max_body_bytes;
    size_t max_connections;
    uint64_t read_timeout_ns;
    uint64_t write_timeout_ns;
} vl_http_config_t;

typedef struct vl_http_header {
    char name[VL_HTTP_MAX_HEADER_NAME];
    char value[VL_HTTP_MAX_HEADER_VALUE];
} vl_http_header_t;

typedef struct vl_http_request {
    char method[VL_HTTP_MAX_METHOD];
    char path[VL_HTTP_MAX_PATH];
    int http_major;
    int http_minor;
    vl_http_header_t headers[VL_HTTP_MAX_HEADERS];
    size_t header_count;
    size_t content_length;
    int keep_alive;
    const char *body;
    size_t body_length;
} vl_http_request_t;

typedef enum vl_http_parse_status {
    VL_HTTP_PARSE_NEED_MORE = 0,
    VL_HTTP_PARSE_COMPLETE = 1,
    VL_HTTP_PARSE_ERROR = 2
} vl_http_parse_status_t;

typedef enum vl_http_error {
    VL_HTTP_ERROR_NONE = 0,
    VL_HTTP_ERROR_BAD_REQUEST = 400,
    VL_HTTP_ERROR_LENGTH_REQUIRED = 411,
    VL_HTTP_ERROR_PAYLOAD_TOO_LARGE = 413,
    VL_HTTP_ERROR_HEADER_TOO_LARGE = 431
} vl_http_error_t;

typedef struct vl_http_parser {
    vl_http_config_t config;
    vl_http_request_t request;
    char *buffer;
    size_t capacity;
    size_t length;
    size_t header_end;
    vl_http_error_t error;
    int complete;
} vl_http_parser_t;

typedef struct vl_http_response {
    int status;
    const char *reason;
    char body[1024];
    size_t body_length;
    int keep_alive;
    int chunked;
} vl_http_response_t;

typedef struct vl_http_server vl_http_server_t;
typedef void (*vl_http_handler_t)(const vl_http_request_t *request,
                                  vl_http_response_t *response,
                                  void *user_data);

VL_API void vl_http_config_default(vl_http_config_t *config);
VL_API void vl_http_parser_init(vl_http_parser_t *parser,
                                const vl_http_config_t *config);
VL_API void vl_http_parser_destroy(vl_http_parser_t *parser);
VL_API vl_http_parse_status_t vl_http_parser_feed(vl_http_parser_t *parser,
                                                  const char *data,
                                                  size_t length);
VL_API const vl_http_request_t *vl_http_parser_request(
    const vl_http_parser_t *parser);
VL_API vl_http_error_t vl_http_parser_error(const vl_http_parser_t *parser);

VL_API int vl_http_server_init(vl_http_server_t **server,
                               const vl_http_config_t *config);
VL_API void vl_http_server_destroy(vl_http_server_t *server);
VL_API int vl_http_route(vl_http_server_t *server, const char *method,
                         const char *path, vl_http_handler_t handler,
                         void *user_data);
VL_API void vl_http_server_request_shutdown(vl_http_server_t *server);
VL_API int vl_http_server_listen_loopback(vl_http_server_t *server,
                                          uint16_t port, int backlog,
                                          uint16_t *bound_port);
VL_API int vl_http_spawn_connection(vl_http_server_t *server,
                                    vl_runtime_t *runtime, vl_io_t *io,
                                    int fd);

VL_API void vl_http_response_init(vl_http_response_t *response);
VL_API int vl_http_response_set_status(vl_http_response_t *response,
                                       int status);
VL_API int vl_http_response_set_body(vl_http_response_t *response,
                                     const char *body, size_t length);
VL_API int vl_http_response_write(vl_io_t *io, int fd,
                                  vl_http_response_t *response);

#endif
