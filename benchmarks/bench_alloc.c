#include <veloco/memory.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VL_ALLOC_BENCH_OPERATIONS UINT64_C(1000000)
#define VL_ALLOC_BENCH_SLOTS 4096

static uint64_t elapsed_ns(const struct timespec *start,
                           const struct timespec *end)
{
    return (uint64_t)(end->tv_sec - start->tv_sec) * UINT64_C(1000000000) +
           (uint64_t)(end->tv_nsec - start->tv_nsec);
}

static int parse_allocator(int argc, char **argv, int *use_veloco)
{
    int index;

    *use_veloco = 1;
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--allocator") == 0 && index + 1 < argc) {
            ++index;
            if (strcmp(argv[index], "libc") == 0) {
                *use_veloco = 0;
            } else if (strcmp(argv[index], "veloco") != 0) {
                return 1;
            }
        } else if (strcmp(argv[index], "--threads") == 0 && index + 1 < argc) {
            ++index;
            if (strcmp(argv[index], "1") != 0) {
                fprintf(stderr, "Task 3 benchmark supports --threads 1 only\n");
                return 1;
            }
        } else {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    void *slots[VL_ALLOC_BENCH_SLOTS] = {0};
    int use_veloco;
    struct timespec start;
    struct timespec end;
    vl_allocator_stats_t stats = {0};
    uint64_t operation;
    uint64_t duration;
    int initialized = 0;
    int result = 1;

    if (parse_allocator(argc, argv, &use_veloco) != 0) {
        fprintf(stderr, "usage: %s --allocator libc|veloco --threads 1\n",
                argv[0]);
        return 2;
    }
    if (use_veloco && vl_allocator_init() != VL_OK) {
        return 1;
    }
    initialized = use_veloco;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        goto cleanup;
    }
    for (operation = 0; operation < VL_ALLOC_BENCH_OPERATIONS; ++operation) {
        size_t slot = (size_t)(operation % VL_ALLOC_BENCH_SLOTS);
        size_t size = 16 + (size_t)(operation % 2048);

        if (use_veloco) {
            vl_free(slots[slot]);
            slots[slot] = vl_malloc(size);
        } else {
            free(slots[slot]);
            slots[slot] = malloc(size);
        }
        if (slots[slot] == NULL) {
            goto cleanup;
        }
        ((unsigned char *)slots[slot])[0] = (unsigned char)operation;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    for (operation = 0; operation < VL_ALLOC_BENCH_SLOTS; ++operation) {
        if (use_veloco) {
            vl_free(slots[operation]);
        } else {
            free(slots[operation]);
        }
    }
    if (use_veloco) {
        vl_allocator_get_stats(&stats);
    }

    if (result != 0) {
        if (initialized) {
            vl_allocator_shutdown();
        }
        return result;
    }

    duration = elapsed_ns(&start, &end);
    if (duration == 0) {
        if (initialized) {
            vl_allocator_shutdown();
        }
        return 1;
    }
    printf("allocator=%s\n", use_veloco ? "veloco" : "libc");
    printf("threads=1\n");
    printf("operations=%" PRIu64 "\n", VL_ALLOC_BENCH_OPERATIONS);
    printf("elapsed_ns=%" PRIu64 "\n", duration);
    printf("allocations_per_second=%.0f\n",
           (double)VL_ALLOC_BENCH_OPERATIONS * 1000000000.0 /
               (double)duration);
    printf("active_bytes=%zu\n", stats.active_bytes);
    printf("active_objects=%zu\n", stats.active_objects);
    printf("cache_hits=%zu\n", stats.cache_hits);
    printf("central_refills=%zu\n", stats.central_refills);
    printf("mapped_bytes=%zu\n", stats.mapped_bytes);
    printf("cross_p_frees=%zu\n", stats.cross_p_frees);

    if (initialized) {
        vl_allocator_shutdown();
    }
    return 0;
}
