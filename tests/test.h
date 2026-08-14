#ifndef VELOCO_TEST_H
#define VELOCO_TEST_H

typedef void (*vl_test_fn)(void);

#define VL_TEST(name) static void name(void)
#define VL_ASSERT(expr)                                                        \
    do {                                                                       \
        if (!(expr)) {                                                         \
            vl_test_fail(__FILE__, __LINE__, #expr);                           \
        }                                                                      \
    } while (0)
#define VL_REQUIRE(expr)                                                       \
    do {                                                                       \
        if (!(expr)) {                                                         \
            vl_test_fail(__FILE__, __LINE__, #expr);                           \
            return;                                                            \
        }                                                                      \
    } while (0)

void vl_test_fail(const char *file, int line, const char *expr);
void vl_test_add(const char *name, vl_test_fn fn);
int vl_test_run_all(void);

#endif
