#include "test.h"

#include "memory_internal.h"

#include <veloco/memory.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VL_MEMORY_TEST_WITH_ASAN 1
#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
#define VL_MEMORY_TEST_WITH_ASAN 1
#endif

VL_TEST(memory_size_classes_are_sorted_and_cover_small_allocations)
{
    size_t previous = 0;
    size_t index;
    size_t size;

    VL_ASSERT(vl_size_class_count() == VL_SIZE_CLASS_COUNT);
    for (index = 0; index < vl_size_class_count(); ++index) {
        size_t capacity = vl_size_class_capacity(index);

        VL_ASSERT(capacity > previous);
        VL_ASSERT(capacity <= VL_MEMORY_SMALL_MAX);
        previous = capacity;
    }
    VL_ASSERT(previous == VL_MEMORY_SMALL_MAX);

    for (size = 1; size <= VL_MEMORY_SMALL_MAX; ++size) {
        index = vl_size_class_index(size);
        VL_ASSERT(index < vl_size_class_count());
        VL_ASSERT(vl_size_class_capacity(index) >= size);
        if (index > 0) {
            VL_ASSERT(vl_size_class_capacity(index - 1) < size);
        }
    }
    VL_ASSERT(vl_size_class_index(VL_MEMORY_SMALL_MAX + 1) ==
              vl_size_class_count());
}

VL_TEST(memory_allocations_are_aligned_for_every_small_size)
{
    size_t size;

    VL_REQUIRE(vl_allocator_init() == VL_OK);
    for (size = 1; size <= VL_MEMORY_SMALL_MAX; ++size) {
        unsigned char *ptr = vl_malloc(size);

        VL_REQUIRE(ptr != NULL);
        VL_ASSERT((uintptr_t)ptr % _Alignof(max_align_t) == 0);
        ptr[0] = 0x11;
        ptr[size - 1] = 0x22;
        vl_free(ptr);
    }
    vl_free(NULL);
    vl_allocator_shutdown();
}

VL_TEST(memory_calloc_and_realloc_follow_c_semantics)
{
    unsigned char *ptr;
    unsigned char *grown;
    size_t index;

    VL_REQUIRE(vl_allocator_init() == VL_OK);
    ptr = vl_calloc(32, sizeof(*ptr));
    VL_REQUIRE(ptr != NULL);
    for (index = 0; index < 32; ++index) {
        VL_ASSERT(ptr[index] == 0);
        ptr[index] = (unsigned char)(index + 1);
    }

    grown = vl_realloc(ptr, 128);
    VL_REQUIRE(grown != NULL);
    for (index = 0; index < 32; ++index) {
        VL_ASSERT(grown[index] == (unsigned char)(index + 1));
    }
    VL_ASSERT(vl_calloc(SIZE_MAX, 2) == NULL);
    VL_ASSERT(vl_realloc(grown, 0) == NULL);

    ptr = vl_realloc(NULL, 24);
    VL_REQUIRE(ptr != NULL);
    vl_free(ptr);
    vl_allocator_shutdown();
}

VL_TEST(memory_span_counts_return_to_zero_after_frees)
{
    enum { OBJECT_COUNT = 256 };
    void *objects[OBJECT_COUNT];
    vl_memory_class_stats_t stats;
    size_t class_index = vl_size_class_index(64);
    size_t index;

    VL_REQUIRE(vl_allocator_init() == VL_OK);
    for (index = 0; index < OBJECT_COUNT; ++index) {
        objects[index] = vl_malloc(64);
        VL_REQUIRE(objects[index] != NULL);
    }
    VL_REQUIRE(vl_memory_get_class_stats(class_index, &stats) == VL_OK);
    VL_ASSERT(stats.span_count > 0);
    VL_ASSERT(stats.active_objects == OBJECT_COUNT);

    for (index = 0; index < OBJECT_COUNT; ++index) {
        vl_free(objects[index]);
    }
    VL_REQUIRE(vl_memory_get_class_stats(class_index, &stats) == VL_OK);
    VL_ASSERT(stats.active_objects == 0);
    VL_ASSERT(stats.free_objects == stats.object_count);
    vl_allocator_shutdown();
}

VL_TEST(memory_large_mapping_is_returned_to_page_heap)
{
    vl_allocator_stats_t before;
    vl_allocator_stats_t during;
    vl_allocator_stats_t after;
    void *ptr;

    VL_REQUIRE(vl_allocator_init() == VL_OK);
    vl_allocator_get_stats(&before);
    ptr = vl_malloc(VL_MEMORY_SMALL_MAX + 4096);
    VL_REQUIRE(ptr != NULL);
    vl_allocator_get_stats(&during);
    VL_ASSERT(during.mapped_bytes > before.mapped_bytes);
    VL_ASSERT(during.active_objects == before.active_objects + 1);
    vl_free(ptr);
    vl_allocator_get_stats(&after);
    VL_ASSERT(after.mapped_bytes == before.mapped_bytes);
    VL_ASSERT(after.active_objects == before.active_objects);
    vl_allocator_shutdown();
}

VL_TEST(memory_statistics_track_requested_bytes_and_objects)
{
    vl_allocator_stats_t stats;
    void *small;
    void *large;

    VL_REQUIRE(vl_allocator_init() == VL_OK);
    small = vl_malloc(7);
    large = vl_malloc(VL_MEMORY_SMALL_MAX + 17);
    VL_REQUIRE(small != NULL);
    VL_REQUIRE(large != NULL);
    vl_allocator_get_stats(&stats);
    VL_ASSERT(stats.active_bytes == VL_MEMORY_SMALL_MAX + 24);
    VL_ASSERT(stats.active_objects == 2);

    small = vl_realloc(small, 8);
    VL_REQUIRE(small != NULL);
    vl_allocator_get_stats(&stats);
    VL_ASSERT(stats.active_bytes == VL_MEMORY_SMALL_MAX + 25);
    VL_ASSERT(stats.active_objects == 2);

    vl_free(small);
    vl_free(large);
    vl_allocator_get_stats(&stats);
    VL_ASSERT(stats.active_bytes == 0);
    VL_ASSERT(stats.active_objects == 0);
    vl_allocator_shutdown();
}

VL_TEST(memory_statistics_reject_counter_overflow)
{
    vl_allocator_stats_t before;
    vl_allocator_stats_t after;

    VL_REQUIRE(vl_allocator_init() == VL_OK);
    vl_allocator_get_stats(&before);
    vl_memory_global.stats.active_bytes = SIZE_MAX;
    VL_ASSERT(vl_malloc(1) == NULL);
    vl_memory_global.stats.active_bytes = 0;
    vl_memory_global.stats.active_objects = SIZE_MAX;
    VL_ASSERT(vl_malloc(1) == NULL);
    vl_memory_global.stats.active_objects = 0;
    vl_allocator_get_stats(&after);
    VL_ASSERT(after.mapped_bytes == before.mapped_bytes);
    VL_ASSERT(after.active_bytes == 0);
    VL_ASSERT(after.active_objects == 0);
    vl_allocator_shutdown();
}

typedef struct memory_non_owner_result {
    int init_status;
    void *allocation;
} memory_non_owner_result_t;

static void *memory_allocate_from_non_owner(void *argument)
{
    memory_non_owner_result_t *result = argument;

    vl_memory_bind_p(1);
    result->init_status = vl_allocator_init();
    result->allocation = vl_malloc(16);
    return NULL;
}

VL_TEST(memory_allocator_allows_cross_p_free)
{
    pthread_t thread;
    memory_non_owner_result_t result = {0};
    vl_allocator_stats_t stats;

    VL_REQUIRE(vl_allocator_init() == VL_OK);
    VL_REQUIRE(pthread_create(&thread, NULL, memory_allocate_from_non_owner,
                              &result) == 0);
    VL_REQUIRE(pthread_join(thread, NULL) == 0);
    VL_ASSERT(result.init_status == VL_OK);
    VL_REQUIRE(result.allocation != NULL);
    vl_free(result.allocation);
    vl_allocator_get_stats(&stats);
    VL_ASSERT(stats.active_bytes == 0);
    VL_ASSERT(stats.active_objects == 0);
    VL_ASSERT(stats.cross_p_frees == 1);
    vl_allocator_shutdown();
}

typedef struct allocator_worker_fixture {
    pthread_barrier_t *barrier;
    void ***slots;
    size_t index;
    _Atomic int *failed;
} allocator_worker_fixture_t;

static void *allocator_worker(void *argument)
{
    allocator_worker_fixture_t *fixture = argument;
    size_t index;

    vl_memory_bind_p(fixture->index + 1);
    for (index = 0; index < 64; ++index) {
        fixture->slots[fixture->index][index] =
            index == 0 ? vl_malloc(40000 + fixture->index * 8)
                       : vl_malloc(32 + index);
        if (fixture->slots[fixture->index][index] == NULL) {
            atomic_store_explicit(fixture->failed, 1, memory_order_release);
        }
    }
    (void)pthread_barrier_wait(fixture->barrier);
    for (index = 0; index < 64; ++index) {
        vl_free(fixture->slots[(fixture->index + 1) % 4][index]);
    }
    (void)pthread_barrier_wait(fixture->barrier);
    return NULL;
}

VL_TEST(memory_allocator_handles_concurrent_p_local_caches)
{
    enum { worker_count = 4, allocation_count = 64 };
    pthread_t threads[worker_count];
    pthread_barrier_t barrier;
    allocator_worker_fixture_t fixtures[worker_count];
    void *slots[worker_count][allocation_count] = {{0}};
    void **slot_rows[worker_count];
    _Atomic int failed = 0;
    vl_allocator_stats_t stats;
    size_t index;

    VL_REQUIRE(vl_allocator_init() == VL_OK);
    VL_REQUIRE(pthread_barrier_init(&barrier, NULL, worker_count) == 0);
    for (index = 0; index < worker_count; ++index) {
        slot_rows[index] = slots[index];
        fixtures[index].barrier = &barrier;
        fixtures[index].slots = slot_rows;
        fixtures[index].index = index;
        fixtures[index].failed = &failed;
        VL_REQUIRE(pthread_create(&threads[index], NULL, allocator_worker,
                                  &fixtures[index]) == 0);
    }
    for (index = 0; index < worker_count; ++index) {
        VL_REQUIRE(pthread_join(threads[index], NULL) == 0);
    }
    pthread_barrier_destroy(&barrier);
    VL_ASSERT(atomic_load_explicit(&failed, memory_order_acquire) == 0);
    vl_allocator_get_stats(&stats);
    VL_ASSERT(stats.active_objects == 0);
    VL_ASSERT(stats.active_bytes == 0);
    VL_ASSERT(stats.cross_p_frees >= worker_count);
    vl_allocator_shutdown();
}

VL_TEST(memory_invalid_handles_do_not_initialize_allocator)
{
    vl_arena_t arena = {0};
    vl_pool_t pool = {0};

    VL_ASSERT(!vl_memory_global.initialized);
    VL_ASSERT(vl_arena_alloc(&arena, 16, 16) == NULL);
    VL_ASSERT(vl_pool_alloc(&pool) == NULL);
    VL_ASSERT(!vl_memory_global.initialized);
}

VL_TEST(memory_arena_reset_releases_all_blocks)
{
    vl_arena_t arena = {0};
    vl_allocator_stats_t before;
    vl_allocator_stats_t during;
    vl_allocator_stats_t after;
    unsigned char *first;
    unsigned char *second;

    VL_REQUIRE(vl_allocator_init() == VL_OK);
    vl_allocator_get_stats(&before);
    VL_REQUIRE(vl_arena_init(&arena, 4096) == VL_OK);
    first = vl_arena_alloc(&arena, 3000, 64);
    second = vl_arena_alloc(&arena, 3000, 64);
    VL_REQUIRE(first != NULL);
    VL_REQUIRE(second != NULL);
    VL_ASSERT((uintptr_t)first % 64 == 0);
    VL_ASSERT((uintptr_t)second % 64 == 0);
    vl_allocator_get_stats(&during);
    VL_ASSERT(during.mapped_bytes > before.mapped_bytes);

    vl_arena_reset(&arena);
    vl_allocator_get_stats(&after);
    VL_ASSERT(after.mapped_bytes == before.mapped_bytes);
    vl_arena_destroy(&arena);
    vl_allocator_shutdown();
}

VL_TEST(memory_pool_reuses_fixed_size_objects)
{
    vl_pool_t pool = {0};
    vl_allocator_stats_t stats;
    void *first;
    void *second;

    VL_REQUIRE(vl_allocator_init() == VL_OK);
    VL_REQUIRE(vl_pool_init(&pool, 96) == VL_OK);
    first = vl_pool_alloc(&pool);
    VL_REQUIRE(first != NULL);
    VL_ASSERT(vl_pool_object_size(&pool) == 96);
    VL_ASSERT(vl_pool_free(&pool, first) == VL_OK);
    vl_allocator_get_stats(&stats);
    VL_ASSERT(stats.active_bytes == 96);
    VL_ASSERT(stats.active_objects == 1);
    second = vl_pool_alloc(&pool);
    VL_REQUIRE(second != NULL);
    VL_ASSERT(second == first);
    VL_ASSERT(vl_pool_object_size(&pool) == 96);
    VL_ASSERT(vl_pool_free(&pool, second) == VL_OK);
    vl_pool_destroy(&pool);
    vl_allocator_get_stats(&stats);
    VL_ASSERT(stats.active_bytes == 0);
    VL_ASSERT(stats.active_objects == 0);
    vl_allocator_shutdown();
}

#if defined(VELOCO_MEMORY_DEBUG)
static int child_received_signal(pid_t child, int signal_number,
                                 int alternate_signal)
{
    int status = 0;

    if (waitpid(child, &status, 0) != child) {
        return 0;
    }
    return WIFSIGNALED(status) &&
           (WTERMSIG(status) == signal_number ||
            (alternate_signal != 0 && WTERMSIG(status) == alternate_signal));
}

VL_TEST(memory_debug_detects_double_free)
{
    pid_t child = fork();

    VL_REQUIRE(child >= 0);
    if (child == 0) {
        void *ptr;

        if (vl_allocator_init() != VL_OK) {
            _exit(2);
        }
        ptr = vl_malloc(24);
        if (ptr == NULL) {
            _exit(3);
        }
        vl_free(ptr);
        vl_free(ptr);
        _exit(0);
    }
    VL_ASSERT(child_received_signal(child, SIGABRT, 0));
}

VL_TEST(memory_debug_detects_suffix_canary_damage)
{
    pid_t child = fork();

    VL_REQUIRE(child >= 0);
    if (child == 0) {
        unsigned char *ptr;

        if (vl_allocator_init() != VL_OK) {
            _exit(2);
        }
        ptr = vl_malloc(24);
        if (ptr == NULL) {
            _exit(3);
        }
        ptr[24] = 0xff;
        vl_free(ptr);
        _exit(0);
    }
    VL_ASSERT(child_received_signal(child, SIGABRT, 0));
}

VL_TEST(memory_debug_detects_prefix_canary_damage)
{
    pid_t child = fork();

    VL_REQUIRE(child >= 0);
    if (child == 0) {
        unsigned char *ptr;

        if (vl_allocator_init() != VL_OK) {
            _exit(2);
        }
        ptr = vl_malloc(24);
        if (ptr == NULL) {
            _exit(3);
        }
        ptr[-1] ^= 0xff;
        vl_free(ptr);
        _exit(0);
    }
    VL_ASSERT(child_received_signal(child, SIGABRT, 0));
}

VL_TEST(memory_debug_poisons_freed_payload)
{
    unsigned char *ptr;
    size_t index;

    VL_REQUIRE(vl_allocator_init() == VL_OK);
    ptr = vl_malloc(24);
    VL_REQUIRE(ptr != NULL);
    memset(ptr, 0, 24);
    vl_free(ptr);
    for (index = sizeof(void *); index < 24; ++index) {
        VL_ASSERT(ptr[index] == 0xa5);
    }
    vl_allocator_shutdown();
}

VL_TEST(memory_debug_arena_pointer_is_invalid_after_reset)
{
    pid_t child = fork();

    VL_REQUIRE(child >= 0);
    if (child == 0) {
        vl_arena_t arena = {0};
        volatile unsigned char *ptr;

        if (vl_allocator_init() != VL_OK ||
            vl_arena_init(&arena, 4096) != VL_OK) {
            _exit(2);
        }
        ptr = vl_arena_alloc(&arena, 32, 16);
        if (ptr == NULL) {
            _exit(3);
        }
        vl_arena_reset(&arena);
        *ptr = 1;
        _exit(0);
    }
#if defined(VL_MEMORY_TEST_WITH_ASAN)
    VL_ASSERT(child_received_signal(child, SIGSEGV, SIGABRT));
#else
    VL_ASSERT(child_received_signal(child, SIGSEGV, 0));
#endif
}
#endif

void vl_register_memory_tests(void)
{
    vl_test_add("memory_size_classes_are_sorted_and_cover_small_allocations",
                memory_size_classes_are_sorted_and_cover_small_allocations);
    vl_test_add("memory_allocations_are_aligned_for_every_small_size",
                memory_allocations_are_aligned_for_every_small_size);
    vl_test_add("memory_calloc_and_realloc_follow_c_semantics",
                memory_calloc_and_realloc_follow_c_semantics);
    vl_test_add("memory_span_counts_return_to_zero_after_frees",
                memory_span_counts_return_to_zero_after_frees);
    vl_test_add("memory_large_mapping_is_returned_to_page_heap",
                memory_large_mapping_is_returned_to_page_heap);
    vl_test_add("memory_statistics_track_requested_bytes_and_objects",
                memory_statistics_track_requested_bytes_and_objects);
    vl_test_add("memory_statistics_reject_counter_overflow",
                memory_statistics_reject_counter_overflow);
    vl_test_add("memory_allocator_allows_cross_p_free",
                memory_allocator_allows_cross_p_free);
    vl_test_add("memory_allocator_handles_concurrent_p_local_caches",
                memory_allocator_handles_concurrent_p_local_caches);
    vl_test_add("memory_invalid_handles_do_not_initialize_allocator",
                memory_invalid_handles_do_not_initialize_allocator);
    vl_test_add("memory_arena_reset_releases_all_blocks",
                memory_arena_reset_releases_all_blocks);
    vl_test_add("memory_pool_reuses_fixed_size_objects",
                memory_pool_reuses_fixed_size_objects);
#if defined(VELOCO_MEMORY_DEBUG)
    vl_test_add("memory_debug_detects_double_free",
                memory_debug_detects_double_free);
    vl_test_add("memory_debug_detects_suffix_canary_damage",
                memory_debug_detects_suffix_canary_damage);
    vl_test_add("memory_debug_detects_prefix_canary_damage",
                memory_debug_detects_prefix_canary_damage);
    vl_test_add("memory_debug_poisons_freed_payload",
                memory_debug_poisons_freed_payload);
    vl_test_add("memory_debug_arena_pointer_is_invalid_after_reset",
                memory_debug_arena_pointer_is_invalid_after_reset);
#endif
}
