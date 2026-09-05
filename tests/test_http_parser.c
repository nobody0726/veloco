#include "test.h"

#include <veloco/http.h>

#include <string.h>

static void feed_bytes(vl_http_parser_t *parser, const char *input,
                       vl_http_parse_status_t expected)
{
    size_t index;
    vl_http_parse_status_t status = VL_HTTP_PARSE_NEED_MORE;

    for (index = 0; input[index] != '\0'; ++index) {
        status = vl_http_parser_feed(parser, &input[index], 1);
        if (input[index + 1] != '\0') {
            VL_ASSERT(status == VL_HTTP_PARSE_NEED_MORE);
        }
    }
    VL_ASSERT(status == expected);
}

VL_TEST(http_parser_accepts_fragmented_keep_alive_request)
{
    vl_http_parser_t parser;
    const vl_http_request_t *request;
    const char *input = "GET /health HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "Connection: keep-alive\r\n"
                        "\r\n";

    vl_http_parser_init(&parser, NULL);
    feed_bytes(&parser, input, VL_HTTP_PARSE_COMPLETE);
    request = vl_http_parser_request(&parser);
    VL_REQUIRE(request != NULL);
    VL_ASSERT(strcmp(request->method, "GET") == 0);
    VL_ASSERT(strcmp(request->path, "/health") == 0);
    VL_ASSERT(request->http_major == 1);
    VL_ASSERT(request->http_minor == 1);
    VL_ASSERT(request->header_count == 2);
    VL_ASSERT(request->keep_alive == 1);
    VL_ASSERT(request->body_length == 0);
    vl_http_parser_destroy(&parser);
}

VL_TEST(http_parser_rejects_duplicate_content_length)
{
    vl_http_parser_t parser;
    const char *input = "POST /x HTTP/1.1\r\n"
                        "Content-Length: 1\r\n"
                        "Content-Length: 2\r\n"
                        "\r\n";

    vl_http_parser_init(&parser, NULL);
    VL_ASSERT(vl_http_parser_feed(&parser, input, strlen(input)) ==
              VL_HTTP_PARSE_ERROR);
    VL_ASSERT(vl_http_parser_error(&parser) == VL_HTTP_ERROR_BAD_REQUEST);
    vl_http_parser_destroy(&parser);
}

VL_TEST(http_parser_enforces_body_limit)
{
    vl_http_parser_t parser;
    vl_http_config_t config;
    const char *input = "POST /x HTTP/1.1\r\n"
                        "Content-Length: 4\r\n"
                        "\r\n"
                        "data";

    vl_http_config_default(&config);
    config.max_body_bytes = 3;
    vl_http_parser_init(&parser, &config);
    VL_ASSERT(vl_http_parser_feed(&parser, input, strlen(input)) ==
              VL_HTTP_PARSE_ERROR);
    VL_ASSERT(vl_http_parser_error(&parser) ==
              VL_HTTP_ERROR_PAYLOAD_TOO_LARGE);
    vl_http_parser_destroy(&parser);
}

VL_TEST(http_parser_rejects_invalid_header)
{
    vl_http_parser_t parser;
    const char *input = "GET / HTTP/1.1\r\n"
                        "Broken header\r\n"
                        "\r\n";

    vl_http_parser_init(&parser, NULL);
    VL_ASSERT(vl_http_parser_feed(&parser, input, strlen(input)) ==
              VL_HTTP_PARSE_ERROR);
    VL_ASSERT(vl_http_parser_error(&parser) == VL_HTTP_ERROR_BAD_REQUEST);
    vl_http_parser_destroy(&parser);
}

VL_TEST(http_parser_rejects_oversized_request_line)
{
    vl_http_parser_t parser;
    vl_http_config_t config;
    const char *input = "GET /toolong HTTP/1.1\r\n\r\n";

    vl_http_config_default(&config);
    config.max_request_line = 8;
    vl_http_parser_init(&parser, &config);
    VL_ASSERT(vl_http_parser_feed(&parser, input, strlen(input)) ==
              VL_HTTP_PARSE_ERROR);
    VL_ASSERT(vl_http_parser_error(&parser) == VL_HTTP_ERROR_BAD_REQUEST);
    vl_http_parser_destroy(&parser);
}

void vl_register_http_parser_tests(void)
{
    vl_test_add("http_parser_accepts_fragmented_keep_alive_request",
                http_parser_accepts_fragmented_keep_alive_request);
    vl_test_add("http_parser_rejects_duplicate_content_length",
                http_parser_rejects_duplicate_content_length);
    vl_test_add("http_parser_enforces_body_limit",
                http_parser_enforces_body_limit);
    vl_test_add("http_parser_rejects_invalid_header",
                http_parser_rejects_invalid_header);
    vl_test_add("http_parser_rejects_oversized_request_line",
                http_parser_rejects_oversized_request_line);
}
