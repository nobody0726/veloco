#include "test.h"

#include <veloco/common.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

void vl_register_fiber_tests(void);
void vl_register_memory_tests(void);
void vl_register_task_tests(void);
void vl_register_queue_tests(void);
void vl_register_io_tests(void);
void vl_register_sync_tests(void);
void vl_register_timer_tests(void);
void vl_register_http_parser_tests(void);
void vl_register_http_server_tests(void);

#define VL_TEST_CAPACITY 128

typedef struct vl_test_case {
    const char *name;
    vl_test_fn fn;
} vl_test_case_t;

static vl_test_case_t vl_test_cases[VL_TEST_CAPACITY];
static size_t vl_test_count;
static int vl_test_failures;

void vl_test_fail(const char *file, int line, const char *expr)
{
    ++vl_test_failures;
    fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, expr);
}

void vl_test_add(const char *name, vl_test_fn fn)
{
    if (vl_test_count >= VL_TEST_CAPACITY) {
        vl_test_fail(__FILE__, __LINE__, "test registry capacity");
        return;
    }

    vl_test_cases[vl_test_count].name = name;
    vl_test_cases[vl_test_count].fn = fn;
    ++vl_test_count;
}

int vl_test_run_all(const char *name_filter)
{
    size_t index;
    size_t executed = 0;

    for (index = 0; index < vl_test_count; ++index) {
        int failures_before = vl_test_failures;

        if (name_filter != NULL &&
            strcmp(name_filter, vl_test_cases[index].name) != 0) {
            continue;
        }
        ++executed;
        vl_test_cases[index].fn();
        if (vl_test_failures == failures_before) {
            printf("PASS %s\n", vl_test_cases[index].name);
        } else {
            printf("FAIL %s\n", vl_test_cases[index].name);
        }
    }

    printf("%zu tests, %d failures\n", executed, vl_test_failures);
    return executed != 0 && vl_test_failures == 0 ? 0 : 1;
}

VL_TEST(common_status_codes_follow_sign_convention)
{
    VL_ASSERT(VL_OK == 0);
    VL_ASSERT(VL_ERROR_INVALID_ARGUMENT < 0);
    VL_ASSERT(VL_ERROR_OUT_OF_MEMORY < 0);
    VL_ASSERT(VL_ERROR_WOULD_BLOCK < 0);
}

VL_TEST(common_version_is_available)
{
    VL_ASSERT(VL_VERSION_MAJOR == 0);
    VL_ASSERT(VL_VERSION_MINOR == 1);
    VL_ASSERT(VL_VERSION_PATCH == 0);
}

int main(int argc, char **argv)
{
    const char *group = argc == 3 && strcmp(argv[1], "--group") == 0
                            ? argv[2]
                            : "all";
    const char *name_filter = argc == 3 && strcmp(argv[1], "--test") == 0
                                  ? argv[2]
                                  : NULL;

    if (strcmp(group, "all") == 0 || strcmp(group, "common") == 0) {
        vl_test_add("common_status_codes_follow_sign_convention",
                    common_status_codes_follow_sign_convention);
        vl_test_add("common_version_is_available", common_version_is_available);
    }
    if (strcmp(group, "all") == 0 || strcmp(group, "fiber") == 0) {
        vl_register_fiber_tests();
    }
    if (strcmp(group, "all") == 0 || strcmp(group, "memory") == 0) {
        vl_register_memory_tests();
    }
    if (strcmp(group, "all") == 0 || strcmp(group, "task") == 0) {
        vl_register_task_tests();
    }
    if (strcmp(group, "all") == 0 || strcmp(group, "queue") == 0) {
        vl_register_queue_tests();
    }
    if (strcmp(group, "all") == 0 || strcmp(group, "io") == 0) {
        vl_register_io_tests();
    }
    if (strcmp(group, "all") == 0 || strcmp(group, "sync") == 0) {
        vl_register_sync_tests();
    }
    if (strcmp(group, "all") == 0 || strcmp(group, "timer") == 0) {
        vl_register_timer_tests();
    }
    if (strcmp(group, "all") == 0 || strcmp(group, "http") == 0) {
        vl_register_http_parser_tests();
        vl_register_http_server_tests();
    }
    return vl_test_run_all(name_filter);
}
