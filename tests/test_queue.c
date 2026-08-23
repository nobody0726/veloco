#include "test.h"

#include <veloco/runtime.h>
#include <veloco/task.h>

#include "run_queue.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

typedef struct queue_trace {
    int values[8];
    size_t length;
} queue_trace_t;

typedef struct queue_task_arg {
    queue_trace_t *trace;
    int value;
} queue_task_arg_t;

static void record_value(void *argument)
{
    queue_task_arg_t *task = argument;

    task->trace->values[task->trace->length++] = task->value;
}

VL_TEST(queue_preserves_spawn_order)
{
    vl_runtime_t runtime = {0};
    vl_runtime_stats_t stats;
    queue_trace_t trace = {0};
    queue_task_arg_t args[3] = {
        {&trace, 1},
        {&trace, 2},
        {&trace, 3},
    };

    VL_REQUIRE(vl_runtime_init(&runtime) == VL_OK);
    VL_REQUIRE(vl_spawn(&runtime, record_value, &args[0]) != NULL);
    VL_REQUIRE(vl_spawn(&runtime, record_value, &args[1]) != NULL);
    VL_REQUIRE(vl_spawn(&runtime, record_value, &args[2]) != NULL);
    vl_runtime_get_stats(&runtime, &stats);
    VL_ASSERT(stats.runnable == 3);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(trace.length == 3);
    VL_ASSERT(trace.values[0] == 1);
    VL_ASSERT(trace.values[1] == 2);
    VL_ASSERT(trace.values[2] == 3);
    vl_runtime_get_stats(&runtime, &stats);
    VL_ASSERT(stats.runnable == 0);
    vl_runtime_shutdown(&runtime);
}

VL_TEST(queue_empty_steal_returns_null)
{
    vl_run_queue_t queue = {0};

    VL_REQUIRE(vl_run_queue_init(&queue, 8) == VL_OK);
    VL_ASSERT(vl_run_queue_steal(&queue) == NULL);
    VL_ASSERT(vl_run_queue_length(&queue) == 0);
    vl_run_queue_destroy(&queue);
}

typedef struct queue_race_fixture {
    vl_run_queue_t *queue;
    pthread_barrier_t barrier;
    void *stolen;
} queue_race_fixture_t;

static void *steal_one_after_barrier(void *argument)
{
    queue_race_fixture_t *fixture = argument;

    (void)pthread_barrier_wait(&fixture->barrier);
    fixture->stolen = vl_run_queue_steal(fixture->queue);
    return NULL;
}

VL_TEST(queue_single_item_owner_thief_race_has_one_winner)
{
    vl_run_queue_t queue = {0};
    queue_race_fixture_t fixture = {0};
    pthread_t thief;
    uintptr_t token = 1;
    void *popped;

    VL_REQUIRE(vl_run_queue_init(&queue, 8) == VL_OK);
    VL_REQUIRE(vl_run_queue_owner_push(&queue, &token) == VL_OK);
    fixture.queue = &queue;
    VL_REQUIRE(pthread_barrier_init(&fixture.barrier, NULL, 2) == 0);
    VL_REQUIRE(pthread_create(&thief, NULL, steal_one_after_barrier,
                              &fixture) == 0);
    (void)pthread_barrier_wait(&fixture.barrier);
    popped = vl_run_queue_owner_pop(&queue);
    VL_REQUIRE(pthread_join(thief, NULL) == 0);
    VL_ASSERT((popped == &token) != (fixture.stolen == &token));
    VL_ASSERT(vl_run_queue_length(&queue) == 0);
    pthread_barrier_destroy(&fixture.barrier);
    vl_run_queue_destroy(&queue);
}

VL_TEST(queue_batch_steal_transfers_each_item_once)
{
    vl_run_queue_t queue = {0};
    uintptr_t tokens[16];
    void *stolen[16] = {0};
    size_t index;
    size_t count;

    VL_REQUIRE(vl_run_queue_init(&queue, 32) == VL_OK);
    for (index = 0; index < 16; ++index) {
        tokens[index] = index;
        VL_REQUIRE(vl_run_queue_owner_push(&queue, &tokens[index]) == VL_OK);
    }
    count = vl_run_queue_steal_batch(&queue, stolen, 8);
    VL_ASSERT(count == 8);
    for (index = 0; index < count; ++index) {
        VL_ASSERT(stolen[index] == &tokens[index]);
    }
    VL_ASSERT(vl_run_queue_length(&queue) == 8);
    vl_run_queue_destroy(&queue);
}

typedef struct queue_stress_fixture {
    vl_run_queue_t *queue;
    _Atomic unsigned *seen;
    size_t token_count;
    _Atomic size_t consumed;
} queue_stress_fixture_t;

static void *queue_stress_thief(void *argument)
{
    queue_stress_fixture_t *fixture = argument;

    while (atomic_load_explicit(&fixture->consumed, memory_order_acquire) <
           fixture->token_count) {
        size_t *token = vl_run_queue_steal(fixture->queue);

        if (token != NULL) {
            atomic_fetch_add_explicit(&fixture->seen[*token], 1,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&fixture->consumed, 1,
                                      memory_order_release);
        }
    }
    return NULL;
}

VL_TEST(queue_concurrent_thieves_consume_every_token_once)
{
    enum { TOKEN_COUNT = 512, THIEF_COUNT = 4 };
    vl_run_queue_t queue = {0};
    queue_stress_fixture_t fixture = {0};
    _Atomic unsigned seen[TOKEN_COUNT];
    size_t tokens[TOKEN_COUNT];
    pthread_t thieves[THIEF_COUNT];
    size_t index;

    VL_REQUIRE(vl_run_queue_init(&queue, 1024) == VL_OK);
    fixture.queue = &queue;
    fixture.seen = seen;
    fixture.token_count = TOKEN_COUNT;
    atomic_init(&fixture.consumed, 0);
    for (index = 0; index < TOKEN_COUNT; ++index) {
        atomic_init(&seen[index], 0);
        tokens[index] = index;
        VL_REQUIRE(vl_run_queue_owner_push(&queue, &tokens[index]) == VL_OK);
    }
    for (index = 0; index < THIEF_COUNT; ++index) {
        VL_REQUIRE(pthread_create(&thieves[index], NULL,
                                  queue_stress_thief, &fixture) == 0);
    }
    for (index = 0; index < THIEF_COUNT; ++index) {
        VL_REQUIRE(pthread_join(thieves[index], NULL) == 0);
    }
    for (index = 0; index < TOKEN_COUNT; ++index) {
        VL_ASSERT(atomic_load_explicit(&seen[index], memory_order_relaxed) ==
                  1);
    }
    vl_run_queue_destroy(&queue);
}

void vl_register_queue_tests(void)
{
    vl_test_add("queue_preserves_spawn_order", queue_preserves_spawn_order);
    vl_test_add("queue_empty_steal_returns_null",
                queue_empty_steal_returns_null);
    vl_test_add("queue_single_item_owner_thief_race_has_one_winner",
                queue_single_item_owner_thief_race_has_one_winner);
    vl_test_add("queue_batch_steal_transfers_each_item_once",
                queue_batch_steal_transfers_each_item_once);
    vl_test_add("queue_concurrent_thieves_consume_every_token_once",
                queue_concurrent_thieves_consume_every_token_once);
}
