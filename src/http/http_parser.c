#include "http_internal.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void vl_http_set_error(vl_http_parser_t *parser, vl_http_error_t error)
{
    parser->error = error;
}

void vl_http_config_default(vl_http_config_t *config)
{
    if (config == NULL) {
        return;
    }
    config->max_request_line = VL_HTTP_DEFAULT_MAX_REQUEST_LINE;
    config->max_header_bytes = VL_HTTP_DEFAULT_MAX_HEADER_BYTES;
    config->max_body_bytes = VL_HTTP_DEFAULT_MAX_BODY_BYTES;
    config->max_connections = VL_HTTP_DEFAULT_MAX_CONNECTIONS;
    config->read_timeout_ns = VL_HTTP_DEFAULT_READ_TIMEOUT_NS;
    config->write_timeout_ns = VL_HTTP_DEFAULT_WRITE_TIMEOUT_NS;
}

void vl_http_parser_init(vl_http_parser_t *parser,
                         const vl_http_config_t *config)
{
    size_t capacity;

    if (parser == NULL) {
        return;
    }
    memset(parser, 0, sizeof(*parser));
    vl_http_config_default(&parser->config);
    if (config != NULL) {
        parser->config = *config;
    }
    if (parser->config.max_request_line == 0 ||
        parser->config.max_request_line > VL_HTTP_DEFAULT_MAX_HEADER_BYTES) {
        parser->config.max_request_line = VL_HTTP_DEFAULT_MAX_REQUEST_LINE;
    }
    if (parser->config.max_header_bytes == 0 ||
        parser->config.max_header_bytes > VL_HTTP_DEFAULT_MAX_HEADER_BYTES) {
        parser->config.max_header_bytes = VL_HTTP_DEFAULT_MAX_HEADER_BYTES;
    }
    if (parser->config.max_body_bytes > VL_HTTP_DEFAULT_MAX_BODY_BYTES) {
        parser->config.max_body_bytes = VL_HTTP_DEFAULT_MAX_BODY_BYTES;
    }
    capacity = parser->config.max_header_bytes + parser->config.max_body_bytes;
    parser->buffer = malloc(capacity + 1);
    if (parser->buffer != NULL) {
        parser->capacity = capacity;
    }
}

void vl_http_parser_destroy(vl_http_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }
    free(parser->buffer);
    parser->buffer = NULL;
    parser->capacity = 0;
}

static char *vl_http_find_header_end(char *buffer, size_t length)
{
    size_t index;

    if (length < 4) {
        return NULL;
    }
    for (index = 0; index + 3 < length; ++index) {
        if (buffer[index] == '\r' && buffer[index + 1] == '\n' &&
            buffer[index + 2] == '\r' && buffer[index + 3] == '\n') {
            return buffer + index + 4;
        }
    }
    return NULL;
}

static int vl_http_token_copy(char *dest, size_t dest_size, const char *start,
                              size_t length)
{
    if (length == 0 || length >= dest_size) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    memcpy(dest, start, length);
    dest[length] = '\0';
    return VL_OK;
}

static int vl_http_parse_content_length(const char *value, size_t *out)
{
    unsigned long parsed;
    char *end = NULL;

    if (value == NULL || *value == '\0') {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed > (unsigned long)SIZE_MAX) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    *out = (size_t)parsed;
    return VL_OK;
}

static int vl_http_equals_ci(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) !=
            tolower((unsigned char)*right)) {
            return 0;
        }
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static void vl_http_trim(char **start, char **end)
{
    while (*start < *end && isspace((unsigned char)**start)) {
        ++*start;
    }
    while (*end > *start && isspace((unsigned char)(*end)[-1])) {
        --*end;
    }
}

static int vl_http_parse_request_line(vl_http_parser_t *parser, char *line)
{
    char *method = line;
    char *path;
    char *version;
    char *space;

    space = strchr(method, ' ');
    if (space == NULL || (size_t)(space - method) >
                             parser->config.max_request_line) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    *space = '\0';
    path = space + 1;
    space = strchr(path, ' ');
    if (space == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    *space = '\0';
    version = space + 1;
    if (strncmp(version, "HTTP/1.", 7) != 0 ||
        (version[7] != '0' && version[7] != '1') || version[8] != '\0') {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0 &&
        strcmp(method, "HEAD") != 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (path[0] != '/') {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (vl_http_token_copy(parser->request.method,
                           sizeof(parser->request.method), method,
                           strlen(method)) != VL_OK ||
        vl_http_token_copy(parser->request.path, sizeof(parser->request.path),
                           path, strlen(path)) != VL_OK) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    parser->request.http_major = 1;
    parser->request.http_minor = version[7] - '0';
    parser->request.keep_alive = parser->request.http_minor == 1;
    return VL_OK;
}

static int vl_http_parse_headers(vl_http_parser_t *parser, char *cursor)
{
    int saw_content_length = 0;

    for (;;) {
        char *line_end = strstr(cursor, "\r\n");
        char *colon;
        char *name_start;
        char *name_end;
        char *value_start;
        char *value_end;
        vl_http_header_t *header;

        if (line_end == NULL) {
            return VL_ERROR_INVALID_ARGUMENT;
        }
        if (line_end == cursor) {
            return VL_OK;
        }
        if (parser->request.header_count >= VL_HTTP_MAX_HEADERS) {
            return VL_ERROR_INVALID_ARGUMENT;
        }
        *line_end = '\0';
        colon = strchr(cursor, ':');
        if (colon == NULL || colon == cursor) {
            return VL_ERROR_INVALID_ARGUMENT;
        }
        name_start = cursor;
        name_end = colon;
        value_start = colon + 1;
        value_end = line_end;
        vl_http_trim(&name_start, &name_end);
        vl_http_trim(&value_start, &value_end);
        header = &parser->request.headers[parser->request.header_count];
        if (vl_http_token_copy(header->name, sizeof(header->name),
                               name_start,
                               (size_t)(name_end - name_start)) != VL_OK ||
            vl_http_token_copy(header->value, sizeof(header->value),
                               value_start,
                               (size_t)(value_end - value_start)) != VL_OK) {
            return VL_ERROR_INVALID_ARGUMENT;
        }
        ++parser->request.header_count;
        if (vl_http_equals_ci(header->name, "Content-Length")) {
            if (saw_content_length ||
                vl_http_parse_content_length(header->value,
                                             &parser->request.content_length) !=
                    VL_OK) {
                return VL_ERROR_INVALID_ARGUMENT;
            }
            saw_content_length = 1;
        } else if (vl_http_equals_ci(header->name, "Connection")) {
            if (vl_http_equals_ci(header->value, "close")) {
                parser->request.keep_alive = 0;
            } else if (vl_http_equals_ci(header->value, "keep-alive")) {
                parser->request.keep_alive = 1;
            }
        }
        cursor = line_end + 2;
    }
}

static int vl_http_parse_head(vl_http_parser_t *parser)
{
    char *head_end = parser->buffer + parser->header_end;
    char *request_line_end;

    request_line_end = strstr(parser->buffer, "\r\n");
    if (request_line_end == NULL ||
        (size_t)(request_line_end - parser->buffer) >
            parser->config.max_request_line) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    *request_line_end = '\0';
    if (vl_http_parse_request_line(parser, parser->buffer) != VL_OK) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (vl_http_parse_headers(parser, request_line_end + 2) != VL_OK) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    parser->buffer[parser->header_end - 4] = '\r';
    if (parser->request.content_length > parser->config.max_body_bytes) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    parser->request.body = head_end;
    return VL_OK;
}

vl_http_parse_status_t vl_http_parser_feed(vl_http_parser_t *parser,
                                           const char *data, size_t length)
{
    char *header_end;
    size_t total_limit;

    if (parser == NULL || (data == NULL && length != 0)) {
        return VL_HTTP_PARSE_ERROR;
    }
    if (parser->complete || parser->error != VL_HTTP_ERROR_NONE) {
        return parser->error == VL_HTTP_ERROR_NONE ? VL_HTTP_PARSE_COMPLETE
                                                   : VL_HTTP_PARSE_ERROR;
    }
    total_limit = parser->capacity;
    if (parser->buffer == NULL ||
        length > parser->capacity - parser->length ||
        length > total_limit - parser->length) {
        vl_http_set_error(parser, VL_HTTP_ERROR_PAYLOAD_TOO_LARGE);
        return VL_HTTP_PARSE_ERROR;
    }
    memcpy(parser->buffer + parser->length, data, length);
    parser->length += length;
    parser->buffer[parser->length] = '\0';
    if (parser->header_end == 0) {
        header_end = vl_http_find_header_end(parser->buffer, parser->length);
        if (header_end == NULL) {
            if (parser->length > parser->config.max_header_bytes) {
                vl_http_set_error(parser, VL_HTTP_ERROR_HEADER_TOO_LARGE);
                return VL_HTTP_PARSE_ERROR;
            }
            return VL_HTTP_PARSE_NEED_MORE;
        }
        parser->header_end = (size_t)(header_end - parser->buffer);
        if (parser->header_end > parser->config.max_header_bytes) {
            vl_http_set_error(parser, VL_HTTP_ERROR_HEADER_TOO_LARGE);
            return VL_HTTP_PARSE_ERROR;
        }
        if (vl_http_parse_head(parser) != VL_OK) {
            vl_http_set_error(
                parser,
                parser->request.content_length > parser->config.max_body_bytes
                    ? VL_HTTP_ERROR_PAYLOAD_TOO_LARGE
                    : VL_HTTP_ERROR_BAD_REQUEST);
            return VL_HTTP_PARSE_ERROR;
        }
    }
    if (parser->length - parser->header_end <
        parser->request.content_length) {
        return VL_HTTP_PARSE_NEED_MORE;
    }
    parser->request.body_length = parser->request.content_length;
    parser->complete = 1;
    return VL_HTTP_PARSE_COMPLETE;
}

const vl_http_request_t *vl_http_parser_request(
    const vl_http_parser_t *parser)
{
    return parser != NULL && parser->complete ? &parser->request : NULL;
}

vl_http_error_t vl_http_parser_error(const vl_http_parser_t *parser)
{
    return parser != NULL ? parser->error : VL_HTTP_ERROR_BAD_REQUEST;
}
