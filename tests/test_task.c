#include "test.h"

#include <veloco/runtime.h>
#include <veloco/task.h>

typedef struct task_trace {
    int events[16];
    size_t length;
} task_trace_t;

static void trace_event(task_trace_t *trace, int event)
{
    if (trace->length < sizeof(trace->events) / sizeof(trace->events[0])) {
        trace->events[trace->length++] = event;
    }
}

static void completes_once(void *argument)
{
    task_trace_t *trace = argument;

    trace_event(trace, 1);
}

VL_TEST(task_moves_from_runnable_to_done_once)
{
    vl_runtime_t runtime = {0};
    vl_runtime_stats_t stats;
    task_trace_t trace = {0};
    vl_task_t *task;

    VL_REQUIRE(vl_runtime_init(&runtime) == VL_OK);
    task = vl_spawn(&runtime, completes_once, &trace);
    VL_REQUIRE(task != NULL);
    VL_ASSERT(vl_task_state(task) == VL_TASK_RUNNABLE);
    vl_runtime_get_stats(&runtime, &stats);
    VL_ASSERT(stats.spawned == 1);
    VL_ASSERT(stats.runnable == 1);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(trace.length == 1);
    VL_ASSERT(vl_task_state(task) == VL_TASK_DONE);
    VL_ASSERT(vl_join(task) == VL_OK);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(trace.length == 1);
    vl_runtime_get_stats(&runtime, &stats);
    VL_ASSERT(stats.completed == 1);
    VL_ASSERT(stats.runnable == 0);
    vl_runtime_shutdown(&runtime);
}

static void yields_twice(void *argument)
{
    task_trace_t *trace = argument;

    trace_event(trace, 1);
    vl_yield();
    trace_event(trace, 2);
    vl_yield();
    trace_event(trace, 3);
}

VL_TEST(task_yield_returns_to_fifo_scheduler)
{
    vl_runtime_t runtime = {0};
    task_trace_t trace = {0};

    VL_REQUIRE(vl_runtime_init(&runtime) == VL_OK);
    VL_REQUIRE(vl_spawn(&runtime, yields_twice, &trace) != NULL);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(trace.length == 3);
    VL_ASSERT(trace.events[0] == 1);
    VL_ASSERT(trace.events[1] == 2);
    VL_ASSERT(trace.events[2] == 3);
    vl_runtime_shutdown(&runtime);
}

typedef struct join_fixture {
    task_trace_t trace;
    vl_task_t *target;
} join_fixture_t;

static void join_target(void *argument)
{
    join_fixture_t *fixture = argument;

    trace_event(&fixture->trace, 1);
    vl_yield();
    trace_event(&fixture->trace, 2);
}

static void join_waiter(void *argument)
{
    join_fixture_t *fixture = argument;

    trace_event(&fixture->trace, 3);
    VL_ASSERT(vl_join(fixture->target) == VL_OK);
    trace_event(&fixture->trace, 4);
}

VL_TEST(task_join_parks_until_target_completes)
{
    vl_runtime_t runtime = {0};
    join_fixture_t fixture = {0};
    vl_task_t *waiter;

    VL_REQUIRE(vl_runtime_init(&runtime) == VL_OK);
    fixture.target = vl_spawn(&runtime, join_target, &fixture);
    VL_REQUIRE(fixture.target != NULL);
    waiter = vl_spawn(&runtime, join_waiter, &fixture);
    VL_REQUIRE(waiter != NULL);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(fixture.trace.length == 4);
    VL_ASSERT(fixture.trace.events[0] == 1);
    VL_ASSERT(fixture.trace.events[1] == 3);
    VL_ASSERT(fixture.trace.events[2] == 2);
    VL_ASSERT(fixture.trace.events[3] == 4);
    VL_ASSERT(vl_task_state(fixture.target) == VL_TASK_DONE);
    VL_ASSERT(vl_task_state(waiter) == VL_TASK_DONE);
    vl_runtime_shutdown(&runtime);
}

VL_TEST(runtime_shutdown_cancels_unrun_tasks)
{
    vl_runtime_t runtime = {0};
    vl_runtime_stats_t stats;
    task_trace_t trace = {0};
    vl_task_t *task;

    VL_REQUIRE(vl_runtime_init(&runtime) == VL_OK);
    task = vl_spawn(&runtime, completes_once, &trace);
    VL_REQUIRE(task != NULL);
    vl_runtime_request_shutdown(&runtime);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(vl_task_state(task) == VL_TASK_CANCELLED);
    VL_ASSERT(trace.length == 0);
    vl_runtime_get_stats(&runtime, &stats);
    VL_ASSERT(stats.cancelled == 1);
    vl_runtime_shutdown(&runtime);
}

void vl_register_task_tests(void)
{
    vl_test_add("task_moves_from_runnable_to_done_once",
                task_moves_from_runnable_to_done_once);
    vl_test_add("task_yield_returns_to_fifo_scheduler",
                task_yield_returns_to_fifo_scheduler);
    vl_test_add("task_join_parks_until_target_completes",
                task_join_parks_until_target_completes);
    vl_test_add("runtime_shutdown_cancels_unrun_tasks",
                runtime_shutdown_cancels_unrun_tasks);
}
