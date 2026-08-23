#include "test.h"

#include <veloco/runtime.h>
#include <veloco/task.h>
#include <veloco/timer.h>

#include "../src/time/timer_heap.h"

#include <stdatomic.h>
#include <time.h>

static uint64_t test_now_ns(void)
{
    struct timespec now;

    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

VL_TEST(timer_heap_orders_and_removes_nodes)
{
    vl_timer_heap_t heap = {0};
    vl_timer_node_t first = {.deadline_ns = 30};
    vl_timer_node_t second = {.deadline_ns = 10};
    vl_timer_node_t third = {.deadline_ns = 20};

    VL_REQUIRE(vl_timer_heap_init(&heap) == VL_OK);
    VL_REQUIRE(vl_timer_heap_push(&heap, &first) == VL_OK);
    VL_REQUIRE(vl_timer_heap_push(&heap, &second) == VL_OK);
    VL_REQUIRE(vl_timer_heap_push(&heap, &third) == VL_OK);
    VL_ASSERT(vl_timer_heap_peek(&heap) == &second);
    vl_timer_heap_remove(&heap, &second);
    VL_ASSERT(vl_timer_heap_pop(&heap) == &third);
    VL_ASSERT(vl_timer_heap_pop(&heap) == &first);
    VL_ASSERT(vl_timer_heap_pop(&heap) == NULL);
    vl_timer_heap_destroy(&heap);
}

typedef struct sleep_fixture {
    _Atomic int status;
    _Atomic uint64_t elapsed_ns;
} sleep_fixture_t;

static void sleeping_task(void *argument)
{
    sleep_fixture_t *fixture = argument;
    uint64_t started = test_now_ns();
    int status = vl_sleep_ns(UINT64_C(5000000));

    atomic_store_explicit(&fixture->elapsed_ns, test_now_ns() - started,
                          memory_order_release);
    atomic_store_explicit(&fixture->status, status, memory_order_release);
}

VL_TEST(task_sleep_wakes_after_monotonic_deadline)
{
    vl_runtime_t runtime = {0};
    vl_runtime_config_t config = {.worker_count = 4};
    sleep_fixture_t fixture = {0};

    VL_REQUIRE(vl_runtime_init_with_config(&runtime, &config) == VL_OK);
    VL_REQUIRE(vl_spawn(&runtime, sleeping_task, &fixture) != NULL);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(atomic_load_explicit(&fixture.status, memory_order_acquire) ==
              VL_OK);
    VL_ASSERT(atomic_load_explicit(&fixture.elapsed_ns,
                                   memory_order_acquire) >=
              UINT64_C(3000000));
    vl_runtime_shutdown(&runtime);
}

typedef struct cancel_fixture {
    vl_timer_t timer;
    _Atomic int armed;
    _Atomic int arm_status;
    _Atomic int cancel_status;
} cancel_fixture_t;

static void timer_owner_task(void *argument)
{
    cancel_fixture_t *fixture = argument;
    atomic_store_explicit(&fixture->armed, 1, memory_order_release);
    int status = vl_timer_arm(&fixture->timer, UINT64_C(1000000000));

    atomic_store_explicit(&fixture->arm_status, status, memory_order_release);
}

static void timer_canceller_task(void *argument)
{
    cancel_fixture_t *fixture = argument;
    int status;

    while (atomic_load_explicit(&fixture->armed, memory_order_acquire) == 0) {
        vl_yield();
    }
    do {
        status = vl_timer_cancel(&fixture->timer);
        if (status == VL_ERROR_INVALID_STATE) {
            vl_yield();
        }
    } while (status == VL_ERROR_INVALID_STATE);
    atomic_store_explicit(&fixture->cancel_status, status,
                          memory_order_release);
}

VL_TEST(timer_cancel_wakes_a_waiting_task_once)
{
    vl_runtime_t runtime = {0};
    vl_runtime_config_t config = {.worker_count = 4};
    cancel_fixture_t fixture = {0};

    VL_REQUIRE(vl_runtime_init_with_config(&runtime, &config) == VL_OK);
    VL_REQUIRE(vl_timer_init(&fixture.timer, &runtime) == VL_OK);
    VL_REQUIRE(vl_spawn(&runtime, timer_owner_task, &fixture) != NULL);
    VL_REQUIRE(vl_spawn(&runtime, timer_canceller_task, &fixture) != NULL);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(atomic_load_explicit(&fixture.cancel_status,
                                   memory_order_acquire) == VL_OK);
    VL_ASSERT(atomic_load_explicit(&fixture.arm_status,
                                   memory_order_acquire) == VL_ERROR_CANCELLED);
    VL_REQUIRE(vl_timer_destroy(&fixture.timer) == VL_OK);
    vl_runtime_shutdown(&runtime);
}

void vl_register_timer_tests(void)
{
    vl_test_add("timer_heap_orders_and_removes_nodes",
                timer_heap_orders_and_removes_nodes);
    vl_test_add("task_sleep_wakes_after_monotonic_deadline",
                task_sleep_wakes_after_monotonic_deadline);
    vl_test_add("timer_cancel_wakes_a_waiting_task_once",
                timer_cancel_wakes_a_waiting_task_once);
}
