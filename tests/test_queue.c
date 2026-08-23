#include "test.h"

#include <veloco/runtime.h>
#include <veloco/task.h>

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

void vl_register_queue_tests(void)
{
    vl_test_add("queue_preserves_spawn_order", queue_preserves_spawn_order);
}
