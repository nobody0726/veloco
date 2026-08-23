#include "test.h"

#include <veloco/runtime.h>
#include <veloco/task.h>

#include <stdatomic.h>
#include <sched.h>
#include <stdlib.h>

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

#define MULTI_WORKER_COUNT ((size_t)4)
#define MULTI_TASK_COUNT ((size_t)256)

typedef struct multi_task_argument {
    _Atomic unsigned *seen;
    _Atomic size_t *completed;
    _Atomic int *blocker_claimed;
    size_t total;
    size_t index;
    size_t yields;
} multi_task_argument_t;

static void multi_task_once(void *argument)
{
    multi_task_argument_t *item = argument;
    size_t index;

    atomic_fetch_add_explicit(&item->seen[item->index], 1,
                              memory_order_relaxed);
    if (item->blocker_claimed != NULL &&
        atomic_exchange_explicit(item->blocker_claimed, 1,
                                 memory_order_acq_rel) == 0) {
        while (atomic_load_explicit(item->completed,
                                    memory_order_acquire) + 1 < item->total) {
            sched_yield();
        }
        atomic_fetch_add_explicit(item->completed, 1,
                                  memory_order_release);
        return;
    }
    for (index = 0; index < item->yields; ++index) {
        vl_yield();
    }
    atomic_fetch_add_explicit(item->completed, 1, memory_order_release);
}

VL_TEST(multi_worker_executes_every_task_exactly_once_and_steals)
{
    vl_runtime_t runtime = {0};
    vl_runtime_config_t config = {
        .worker_count = MULTI_WORKER_COUNT,
    };
    multi_task_argument_t arguments[MULTI_TASK_COUNT];
    _Atomic unsigned seen[MULTI_TASK_COUNT] = {0};
    _Atomic size_t completed = 0;
    _Atomic int blocker_claimed = 0;
    vl_runtime_stats_t stats;
    size_t index;
    size_t executed = 0;

    VL_REQUIRE(vl_runtime_init_with_config(&runtime, &config) == VL_OK);
    for (index = 0; index < MULTI_TASK_COUNT; ++index) {
        arguments[index].seen = seen;
        arguments[index].completed = &completed;
        arguments[index].blocker_claimed = &blocker_claimed;
        arguments[index].total = MULTI_TASK_COUNT;
        arguments[index].index = index;
        arguments[index].yields = 4;
        VL_REQUIRE(vl_spawn(&runtime, multi_task_once, &arguments[index]) !=
                   NULL);
    }
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(atomic_load_explicit(&completed, memory_order_acquire) ==
              MULTI_TASK_COUNT);
    for (index = 0; index < MULTI_TASK_COUNT; ++index) {
        VL_ASSERT(atomic_load_explicit(&seen[index], memory_order_relaxed) ==
                  1);
    }
    vl_runtime_get_stats(&runtime, &stats);
    VL_ASSERT(stats.worker_count == MULTI_WORKER_COUNT);
    VL_ASSERT(stats.completed == MULTI_TASK_COUNT);
    VL_ASSERT(stats.steals != 0);
    for (index = 0; index < MULTI_WORKER_COUNT; ++index) {
        vl_runtime_p_stats_t p_stats;

        VL_REQUIRE(vl_runtime_get_p_stats(&runtime, index, &p_stats) ==
                   VL_OK);
        executed += p_stats.executed;
    }
    VL_ASSERT(executed == MULTI_TASK_COUNT * 5 - 4);
    vl_runtime_shutdown(&runtime);
}

typedef struct nested_spawn_fixture {
    vl_runtime_t *runtime;
    multi_task_argument_t *arguments;
    _Atomic unsigned *seen;
    _Atomic size_t completed;
    _Atomic int spawn_failed;
    size_t count;
} nested_spawn_fixture_t;

static void nested_spawn_parent(void *argument)
{
    nested_spawn_fixture_t *fixture = argument;
    size_t index;

    for (index = 0; index < fixture->count; ++index) {
        fixture->arguments[index].seen = fixture->seen;
        fixture->arguments[index].completed = &fixture->completed;
        fixture->arguments[index].blocker_claimed = NULL;
        fixture->arguments[index].total = fixture->count;
        fixture->arguments[index].index = index;
        fixture->arguments[index].yields = 2;
        if (vl_spawn(fixture->runtime, multi_task_once,
                     &fixture->arguments[index]) == NULL) {
            atomic_store_explicit(&fixture->spawn_failed, 1,
                                  memory_order_release);
            return;
        }
    }
}

VL_TEST(task_can_spawn_work_from_a_worker)
{
    enum { child_count = 64 };
    vl_runtime_t runtime = {0};
    vl_runtime_config_t config = {
        .worker_count = MULTI_WORKER_COUNT,
    };
    multi_task_argument_t arguments[child_count];
    _Atomic unsigned seen[child_count] = {0};
    nested_spawn_fixture_t fixture = {
        .runtime = &runtime,
        .arguments = arguments,
        .seen = seen,
        .count = child_count,
    };
    size_t index;

    VL_REQUIRE(vl_runtime_init_with_config(&runtime, &config) == VL_OK);
    VL_REQUIRE(vl_spawn(&runtime, nested_spawn_parent, &fixture) != NULL);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(atomic_load_explicit(&fixture.spawn_failed,
                                   memory_order_acquire) == 0);
    VL_ASSERT(atomic_load_explicit(&fixture.completed,
                                   memory_order_acquire) == child_count);
    for (index = 0; index < child_count; ++index) {
        VL_ASSERT(atomic_load_explicit(&seen[index], memory_order_relaxed) ==
                  1);
    }
    vl_runtime_shutdown(&runtime);
}

typedef struct concurrent_join_fixture {
    vl_task_t *target;
    _Atomic size_t joined;
    _Atomic size_t join_failed;
} concurrent_join_fixture_t;

static void concurrent_join_target(void *argument)
{
    size_t index;

    (void)argument;
    for (index = 0; index < 64; ++index) {
        vl_yield();
    }
}

static void concurrent_join_waiter(void *argument)
{
    concurrent_join_fixture_t *fixture = argument;

    if (vl_join(fixture->target) == VL_OK) {
        atomic_fetch_add_explicit(&fixture->joined, 1,
                                  memory_order_release);
    } else {
        atomic_fetch_add_explicit(&fixture->join_failed, 1,
                                  memory_order_release);
    }
}

VL_TEST(multi_worker_join_wakes_each_waiter_once)
{
    enum { waiter_count = 32 };
    vl_runtime_t runtime = {0};
    vl_runtime_config_t config = {
        .worker_count = MULTI_WORKER_COUNT,
    };
    concurrent_join_fixture_t fixture = {0};
    size_t index;

    VL_REQUIRE(vl_runtime_init_with_config(&runtime, &config) == VL_OK);
    fixture.target = vl_spawn(&runtime, concurrent_join_target, NULL);
    VL_REQUIRE(fixture.target != NULL);
    for (index = 0; index < waiter_count; ++index) {
        VL_REQUIRE(vl_spawn(&runtime, concurrent_join_waiter, &fixture) !=
                   NULL);
    }
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(atomic_load_explicit(&fixture.joined, memory_order_acquire) ==
              waiter_count);
    VL_ASSERT(atomic_load_explicit(&fixture.join_failed,
                                   memory_order_acquire) == 0);
    vl_runtime_shutdown(&runtime);
}

typedef struct shutdown_fixture {
    vl_runtime_t *runtime;
    _Atomic size_t started;
} shutdown_fixture_t;

static void shutdown_yielder(void *argument)
{
    shutdown_fixture_t *fixture = argument;

    atomic_fetch_add_explicit(&fixture->started, 1, memory_order_release);
    for (;;) {
        vl_yield();
        if (vl_task_state(vl_task_current()) != VL_TASK_RUNNING) {
            return;
        }
    }
}

static void shutdown_requester(void *argument)
{
    shutdown_fixture_t *fixture = argument;

    while (atomic_load_explicit(&fixture->started, memory_order_acquire) < 4) {
        vl_yield();
    }
    vl_runtime_request_shutdown(fixture->runtime);
}

VL_TEST(shutdown_cancels_runnable_tasks_after_workers_quiesce)
{
    vl_runtime_t runtime = {0};
    vl_runtime_config_t config = {
        .worker_count = MULTI_WORKER_COUNT,
    };
    shutdown_fixture_t fixture = {.runtime = &runtime};
    vl_runtime_stats_t stats;
    size_t index;

    VL_REQUIRE(vl_runtime_init_with_config(&runtime, &config) == VL_OK);
    for (index = 0; index < 16; ++index) {
        VL_REQUIRE(vl_spawn(&runtime, shutdown_yielder, &fixture) != NULL);
    }
    VL_REQUIRE(vl_spawn(&runtime, shutdown_requester, &fixture) != NULL);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    vl_runtime_get_stats(&runtime, &stats);
    VL_ASSERT(stats.completed == 1);
    VL_ASSERT(stats.cancelled == 16);
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
    vl_test_add("multi_worker_executes_every_task_exactly_once_and_steals",
                multi_worker_executes_every_task_exactly_once_and_steals);
    vl_test_add("task_can_spawn_work_from_a_worker",
                task_can_spawn_work_from_a_worker);
    vl_test_add("multi_worker_join_wakes_each_waiter_once",
                multi_worker_join_wakes_each_waiter_once);
    vl_test_add("shutdown_cancels_runnable_tasks_after_workers_quiesce",
                shutdown_cancels_runnable_tasks_after_workers_quiesce);
}
