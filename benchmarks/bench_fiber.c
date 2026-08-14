#include <veloco/fiber.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define VL_BENCH_PAIRS UINT64_C(1000000)
#define VL_BENCH_WARMUP_PAIRS UINT64_C(10000)

typedef struct bench_fixture {
    vl_fiber_sched_t *sched;
    uint64_t pairs;
} bench_fixture_t;

static long benchmark_fiber(void *arg)
{
    bench_fixture_t *fixture = arg;
    uint64_t index;

    for (index = 0; index < fixture->pairs; ++index) {
        (void)vl_fiber_yield(fixture->sched, (long)index);
    }
    return 0;
}

static uint64_t elapsed_ns(const struct timespec *start,
                           const struct timespec *end)
{
    return (uint64_t)(end->tv_sec - start->tv_sec) * UINT64_C(1000000000) +
           (uint64_t)(end->tv_nsec - start->tv_nsec);
}

static int run_pairs(vl_fiber_sched_t *sched, uint64_t pairs,
                     uint64_t *duration_ns)
{
    bench_fixture_t fixture = {.sched = sched, .pairs = pairs};
    vl_fiber_t *fiber = NULL;
    struct timespec start;
    struct timespec end;
    uint64_t index;
    long value = 0;

    if (vl_fiber_create(sched, &fiber, 64 * 1024, benchmark_fiber, &fixture) !=
        VL_OK) {
        return 1;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        vl_fiber_destroy(fiber);
        return 1;
    }
    for (index = 0; index < pairs; ++index) {
        if (vl_fiber_resume(sched, fiber, 0, &value) != VL_OK) {
            vl_fiber_destroy(fiber);
            return 1;
        }
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0 ||
        vl_fiber_resume(sched, fiber, 0, &value) != VL_OK) {
        vl_fiber_destroy(fiber);
        return 1;
    }

    *duration_ns = elapsed_ns(&start, &end);
    vl_fiber_destroy(fiber);
    return 0;
}

int main(void)
{
    vl_fiber_sched_t sched = {0};
    uint64_t warmup_ns;
    uint64_t duration_ns;
    uint64_t switches = VL_BENCH_PAIRS * 2;
    double switches_per_second;

    if (vl_fiber_sched_init(&sched) != VL_OK ||
        run_pairs(&sched, VL_BENCH_WARMUP_PAIRS, &warmup_ns) != 0 ||
        run_pairs(&sched, VL_BENCH_PAIRS, &duration_ns) != 0) {
        fprintf(stderr, "fiber benchmark failed\n");
        return 1;
    }

    switches_per_second =
        (double)switches * 1000000000.0 / (double)duration_ns;
#if defined(__x86_64__)
    printf("architecture=x86_64\n");
#elif defined(__aarch64__)
    printf("architecture=aarch64\n");
#endif
    printf("yield_resume_pairs=%" PRIu64 "\n", VL_BENCH_PAIRS);
    printf("context_switches=%" PRIu64 "\n", switches);
    printf("elapsed_ns=%" PRIu64 "\n", duration_ns);
    printf("switches_per_second=%.0f\n", switches_per_second);

    vl_fiber_sched_destroy(&sched);
    return 0;
}
