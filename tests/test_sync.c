#include "test.h"

#include <veloco/runtime.h>
#include <veloco/sync.h>
#include <veloco/task.h>

#include <stdatomic.h>
#include <stdint.h>

#define SYNC_WORKERS ((size_t)4)

typedef struct mutex_fixture {
    vl_task_mutex_t mutex;
    size_t value;
    _Atomic int failed;
} mutex_fixture_t;

static void mutex_increment(void *argument)
{
    mutex_fixture_t *fixture = argument;
    size_t index;

    for (index = 0; index < 100; ++index) {
        size_t before;

        if (vl_task_mutex_lock(&fixture->mutex) != VL_OK) {
            atomic_store_explicit(&fixture->failed, 1, memory_order_release);
            return;
        }
        before = fixture->value;
        vl_yield();
        fixture->value = before + 1;
        if (vl_task_mutex_unlock(&fixture->mutex) != VL_OK) {
            atomic_store_explicit(&fixture->failed, 1, memory_order_release);
            return;
        }
    }
}

VL_TEST(task_mutex_serializes_yielding_critical_sections)
{
    vl_runtime_t runtime = {0};
    vl_runtime_config_t config = {.worker_count = SYNC_WORKERS};
    mutex_fixture_t fixture = {0};
    size_t index;

    VL_REQUIRE(vl_runtime_init_with_config(&runtime, &config) == VL_OK);
    VL_REQUIRE(vl_task_mutex_init(&fixture.mutex, &runtime) == VL_OK);
    for (index = 0; index < 8; ++index) {
        VL_REQUIRE(vl_spawn(&runtime, mutex_increment, &fixture) != NULL);
    }
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(fixture.value == 800);
    VL_ASSERT(atomic_load_explicit(&fixture.failed, memory_order_acquire) ==
              0);
    VL_REQUIRE(vl_task_mutex_destroy(&fixture.mutex) == VL_OK);
    vl_runtime_shutdown(&runtime);
}

typedef struct semaphore_fixture {
    vl_semaphore_t semaphore;
    _Atomic size_t active;
    _Atomic size_t maximum;
    _Atomic int failed;
} semaphore_fixture_t;

static void update_maximum(_Atomic size_t *maximum, size_t value)
{
    size_t observed = atomic_load_explicit(maximum, memory_order_relaxed);

    while (observed < value &&
           !atomic_compare_exchange_weak_explicit(
               maximum, &observed, value, memory_order_relaxed,
               memory_order_relaxed)) {
    }
}

static void semaphore_user(void *argument)
{
    semaphore_fixture_t *fixture = argument;
    size_t active;

    if (vl_semaphore_wait(&fixture->semaphore) != VL_OK) {
        atomic_store_explicit(&fixture->failed, 1, memory_order_release);
        return;
    }
    active = atomic_fetch_add_explicit(&fixture->active, 1,
                                       memory_order_acq_rel) + 1;
    update_maximum(&fixture->maximum, active);
    vl_yield();
    atomic_fetch_sub_explicit(&fixture->active, 1, memory_order_acq_rel);
    if (vl_semaphore_post(&fixture->semaphore) != VL_OK) {
        atomic_store_explicit(&fixture->failed, 1, memory_order_release);
    }
}

VL_TEST(semaphore_limits_concurrent_tasks_without_blocking_workers)
{
    vl_runtime_t runtime = {0};
    vl_runtime_config_t config = {.worker_count = SYNC_WORKERS};
    semaphore_fixture_t fixture = {0};
    size_t index;

    VL_REQUIRE(vl_runtime_init_with_config(&runtime, &config) == VL_OK);
    VL_REQUIRE(vl_semaphore_init(&fixture.semaphore, &runtime, 2) == VL_OK);
    for (index = 0; index < 32; ++index) {
        VL_REQUIRE(vl_spawn(&runtime, semaphore_user, &fixture) != NULL);
    }
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(atomic_load_explicit(&fixture.active, memory_order_acquire) ==
              0);
    VL_ASSERT(atomic_load_explicit(&fixture.maximum, memory_order_acquire) ==
              2);
    VL_ASSERT(atomic_load_explicit(&fixture.failed, memory_order_acquire) ==
              0);
    VL_REQUIRE(vl_semaphore_destroy(&fixture.semaphore) == VL_OK);
    vl_runtime_shutdown(&runtime);
}

typedef struct wait_group_fixture {
    vl_wait_group_t group;
    _Atomic size_t workers_done;
    _Atomic size_t waiters_done;
    _Atomic int failed;
} wait_group_fixture_t;

static void wait_group_worker(void *argument)
{
    wait_group_fixture_t *fixture = argument;

    vl_yield();
    atomic_fetch_add_explicit(&fixture->workers_done, 1,
                              memory_order_release);
    if (vl_wait_group_done(&fixture->group) != VL_OK) {
        atomic_store_explicit(&fixture->failed, 1, memory_order_release);
    }
}

static void wait_group_waiter(void *argument)
{
    wait_group_fixture_t *fixture = argument;

    if (vl_wait_group_wait(&fixture->group) != VL_OK ||
        atomic_load_explicit(&fixture->workers_done, memory_order_acquire) !=
            16) {
        atomic_store_explicit(&fixture->failed, 1, memory_order_release);
        return;
    }
    atomic_fetch_add_explicit(&fixture->waiters_done, 1,
                              memory_order_release);
}

VL_TEST(wait_group_wakes_all_waiters_at_zero)
{
    vl_runtime_t runtime = {0};
    vl_runtime_config_t config = {.worker_count = SYNC_WORKERS};
    wait_group_fixture_t fixture = {0};
    size_t index;

    VL_REQUIRE(vl_runtime_init_with_config(&runtime, &config) == VL_OK);
    VL_REQUIRE(vl_wait_group_init(&fixture.group, &runtime) == VL_OK);
    VL_REQUIRE(vl_wait_group_add(&fixture.group, 16) == VL_OK);
    for (index = 0; index < 4; ++index) {
        VL_REQUIRE(vl_spawn(&runtime, wait_group_waiter, &fixture) != NULL);
    }
    for (index = 0; index < 16; ++index) {
        VL_REQUIRE(vl_spawn(&runtime, wait_group_worker, &fixture) != NULL);
    }
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(atomic_load_explicit(&fixture.waiters_done,
                                   memory_order_acquire) == 4);
    VL_ASSERT(atomic_load_explicit(&fixture.failed, memory_order_acquire) ==
              0);
    VL_REQUIRE(vl_wait_group_destroy(&fixture.group) == VL_OK);
    vl_runtime_shutdown(&runtime);
}

typedef struct channel_fixture {
    vl_channel_t channel;
    uintptr_t received[64];
    size_t count;
    _Atomic int sender_status;
    _Atomic int receiver_status;
} channel_fixture_t;

static void channel_sender(void *argument)
{
    channel_fixture_t *fixture = argument;
    uintptr_t value;

    for (value = 1; value <= 64; ++value) {
        int status = vl_channel_send(&fixture->channel, (void *)value);

        if (status != VL_OK) {
            atomic_store_explicit(&fixture->sender_status, status,
                                  memory_order_release);
            return;
        }
    }
    atomic_store_explicit(&fixture->sender_status, VL_OK,
                          memory_order_release);
}

static void channel_receiver(void *argument)
{
    channel_fixture_t *fixture = argument;

    while (fixture->count < 64) {
        void *value = NULL;
        int status = vl_channel_receive(&fixture->channel, &value);

        if (status != VL_OK) {
            atomic_store_explicit(&fixture->receiver_status, status,
                                  memory_order_release);
            return;
        }
        fixture->received[fixture->count++] = (uintptr_t)value;
        vl_yield();
    }
    atomic_store_explicit(&fixture->receiver_status, VL_OK,
                          memory_order_release);
}

VL_TEST(buffered_channel_preserves_fifo_across_parking)
{
    vl_runtime_t runtime = {0};
    vl_runtime_config_t config = {.worker_count = SYNC_WORKERS};
    channel_fixture_t fixture = {0};
    size_t index;

    VL_REQUIRE(vl_runtime_init_with_config(&runtime, &config) == VL_OK);
    VL_REQUIRE(vl_channel_init(&fixture.channel, &runtime, 3) == VL_OK);
    VL_REQUIRE(vl_spawn(&runtime, channel_sender, &fixture) != NULL);
    VL_REQUIRE(vl_spawn(&runtime, channel_receiver, &fixture) != NULL);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(fixture.count == 64);
    for (index = 0; index < 64; ++index) {
        VL_ASSERT(fixture.received[index] == index + 1);
    }
    VL_ASSERT(atomic_load_explicit(&fixture.sender_status,
                                   memory_order_acquire) == VL_OK);
    VL_ASSERT(atomic_load_explicit(&fixture.receiver_status,
                                   memory_order_acquire) == VL_OK);
    VL_REQUIRE(vl_channel_destroy(&fixture.channel) == VL_OK);
    vl_runtime_shutdown(&runtime);
}

static void closed_channel_receiver(void *argument)
{
    channel_fixture_t *fixture = argument;
    void *value = NULL;
    int status = vl_channel_receive(&fixture->channel, &value);

    atomic_store_explicit(&fixture->receiver_status, status,
                          memory_order_release);
}

static void channel_closer(void *argument)
{
    channel_fixture_t *fixture = argument;

    vl_yield();
    atomic_store_explicit(&fixture->sender_status,
                          vl_channel_close(&fixture->channel),
                          memory_order_release);
}

VL_TEST(channel_close_wakes_parked_receiver)
{
    vl_runtime_t runtime = {0};
    vl_runtime_config_t config = {.worker_count = SYNC_WORKERS};
    channel_fixture_t fixture = {0};

    atomic_init(&fixture.receiver_status, VL_ERROR_INVALID_STATE);
    atomic_init(&fixture.sender_status, VL_ERROR_INVALID_STATE);
    VL_REQUIRE(vl_runtime_init_with_config(&runtime, &config) == VL_OK);
    VL_REQUIRE(vl_channel_init(&fixture.channel, &runtime, 0) == VL_OK);
    VL_REQUIRE(vl_spawn(&runtime, closed_channel_receiver, &fixture) != NULL);
    VL_REQUIRE(vl_spawn(&runtime, channel_closer, &fixture) != NULL);
    VL_REQUIRE(vl_runtime_run(&runtime) == VL_OK);
    VL_ASSERT(atomic_load_explicit(&fixture.sender_status,
                                   memory_order_acquire) == VL_OK);
    VL_ASSERT(atomic_load_explicit(&fixture.receiver_status,
                                   memory_order_acquire) == VL_ERROR_CLOSED);
    VL_REQUIRE(vl_channel_destroy(&fixture.channel) == VL_OK);
    vl_runtime_shutdown(&runtime);
}

void vl_register_sync_tests(void)
{
    vl_test_add("task_mutex_serializes_yielding_critical_sections",
                task_mutex_serializes_yielding_critical_sections);
    vl_test_add("semaphore_limits_concurrent_tasks_without_blocking_workers",
                semaphore_limits_concurrent_tasks_without_blocking_workers);
    vl_test_add("wait_group_wakes_all_waiters_at_zero",
                wait_group_wakes_all_waiters_at_zero);
    vl_test_add("buffered_channel_preserves_fifo_across_parking",
                buffered_channel_preserves_fifo_across_parking);
    vl_test_add("channel_close_wakes_parked_receiver",
                channel_close_wakes_parked_receiver);
}
