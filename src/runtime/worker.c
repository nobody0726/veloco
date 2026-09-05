#include "runtime_internal.h"

#include <veloco/io.h>

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <unistd.h>

static void vl_worker_drain_eventfd(vl_p_t *p)
{
    uint64_t value;
    ssize_t result;

    do {
        result = read(p->event_fd, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
}

static vl_task_t *vl_worker_take_global(vl_p_t *p)
{
    vl_runtime_impl_t *runtime = p->runtime;
    vl_task_t *task = NULL;
    size_t candidates;
    size_t index;
    size_t pulled = 0;

    pthread_mutex_lock(&runtime->mutex);
    candidates = runtime->global_runnable.length;
    for (index = 0; index < candidates; ++index) {
        vl_task_t *candidate =
            vl_task_queue_pop(&runtime->global_runnable);

        if (candidate->fiber == NULL || candidate->last_p == p) {
            task = candidate;
            atomic_store_explicit(&task->queued, 0,
                                  memory_order_release);
            pulled = 1;
            break;
        }
        vl_task_queue_push(&runtime->global_runnable, candidate);
    }
    candidates = runtime->global_runnable.length;
    for (index = 0;
         runtime->worker_count > 1 && pulled < VL_STEAL_BATCH_MAX &&
         index < candidates;
         ++index) {
        vl_task_t *extra = vl_task_queue_pop(&runtime->global_runnable);

        if (extra->fiber != NULL) {
            vl_task_queue_push(&runtime->global_runnable, extra);
            continue;
        }
        if (vl_run_queue_owner_push(&p->runnable, extra) != VL_OK) {
            vl_task_queue_push(&runtime->global_runnable, extra);
            break;
        }
        ++p->stats.local_pushes;
        ++pulled;
    }
    p->stats.global_pulls += pulled;
    pthread_mutex_unlock(&runtime->mutex);
    return task;
}

static vl_task_t *vl_worker_steal(vl_p_t *p)
{
    vl_runtime_impl_t *runtime = p->runtime;
    void *items[VL_STEAL_BATCH_MAX];
    size_t offset;

    for (offset = 1; offset < runtime->worker_count; ++offset) {
        vl_p_t *victim = &runtime->ps[(p->id + offset) %
                                     runtime->worker_count];
        size_t count;
        size_t index;

        pthread_mutex_lock(&runtime->mutex);
        ++p->stats.steal_attempts;
        pthread_mutex_unlock(&runtime->mutex);
        count = vl_run_queue_steal_batch(&victim->runnable, items,
                                         VL_STEAL_BATCH_MAX);
        if (count == 0) {
            continue;
        }
        pthread_mutex_lock(&runtime->mutex);
        p->stats.steals += count;
        runtime->stats.steals += count;
        for (index = 1; index < count; ++index) {
            if (vl_run_queue_owner_push(&p->runnable, items[index]) == VL_OK) {
                ++p->stats.local_pushes;
            } else {
                vl_task_queue_push(&runtime->global_runnable, items[index]);
            }
        }
        pthread_mutex_unlock(&runtime->mutex);
        atomic_store_explicit(&((vl_task_t *)items[0])->queued, 0,
                              memory_order_release);
        return items[0];
    }
    return NULL;
}

static vl_task_t *vl_worker_next_task(vl_p_t *p)
{
    vl_task_t *task = vl_run_queue_owner_pop(&p->runnable);

    if (task != NULL) {
        atomic_store_explicit(&task->queued, 0, memory_order_release);
        return task;
    }
    task = vl_worker_take_global(p);
    return task != NULL ? task : vl_worker_steal(p);
}

static void vl_worker_stop_run_locked(vl_runtime_impl_t *runtime, int status)
{
    if (atomic_load_explicit(&runtime->running, memory_order_relaxed)) {
        runtime->run_status = status;
        atomic_store_explicit(&runtime->running, 0, memory_order_release);
        pthread_cond_broadcast(&runtime->condition);
    }
}

static void vl_worker_execute(vl_p_t *p, vl_task_t *task)
{
    vl_runtime_impl_t *runtime = p->runtime;
    vl_fiber_t *destroy_fiber = NULL;
    long result = 0;
    int resume_status = VL_OK;
    int task_state;
    int cancelled_before_resume = 0;

    pthread_mutex_lock(&runtime->mutex);
    task_state = atomic_load_explicit(&task->state, memory_order_relaxed);
    if (task_state != VL_TASK_RUNNABLE ||
        !atomic_load_explicit(&runtime->running, memory_order_relaxed)) {
        pthread_mutex_unlock(&runtime->mutex);
        return;
    }
    if (runtime->shutdown_requested) {
        atomic_store_explicit(&task->state, VL_TASK_CANCELLED,
                              memory_order_release);
        if (runtime->live_tasks != 0) {
            --runtime->live_tasks;
        }
        ++runtime->stats.cancelled;
        cancelled_before_resume = 1;
    } else {
        task->executing = 1;
        task->last_p = p;
        atomic_store_explicit(&task->state, VL_TASK_RUNNING,
                              memory_order_release);
        ++runtime->active_workers;
    }
    pthread_mutex_unlock(&runtime->mutex);

    if (cancelled_before_resume) {
        pthread_mutex_lock(&runtime->mutex);
        if (runtime->live_tasks == 0) {
            vl_worker_stop_run_locked(runtime, VL_OK);
        }
        pthread_mutex_unlock(&runtime->mutex);
        vl_runtime_wake_workers(runtime);
        return;
    }
    if (task->fiber == NULL) {
        resume_status = vl_fiber_create(&p->fiber_sched, &task->fiber,
                                        runtime->task_stack_size,
                                        vl_task_entry, task);
    }
    if (resume_status == VL_OK) {
        pthread_mutex_lock(&runtime->mutex);
        ++runtime->stats.task_switches;
        pthread_mutex_unlock(&runtime->mutex);
        vl_context_set_current(p, task);
        resume_status = vl_fiber_resume(&p->fiber_sched, task->fiber, 0,
                                        &result);
        vl_context_set_current(p, NULL);
    }
    (void)result;

    pthread_mutex_lock(&runtime->mutex);
    task->executing = 0;
    --runtime->active_workers;
    ++p->stats.executed;
    task_state = atomic_load_explicit(&task->state, memory_order_relaxed);
    if (resume_status != VL_OK) {
        atomic_store_explicit(&task->state, VL_TASK_CANCELLED,
                              memory_order_release);
        if (runtime->live_tasks != 0) {
            --runtime->live_tasks;
        }
        ++runtime->stats.cancelled;
        destroy_fiber = task->fiber;
        task->fiber = NULL;
    } else if (vl_fiber_get_state(task->fiber) == VL_FIBER_DONE) {
        atomic_store_explicit(&task->state, VL_TASK_DONE,
                              memory_order_release);
        vl_task_complete_locked(task);
        destroy_fiber = task->fiber;
        task->fiber = NULL;
    } else {
        if (task->wake_pending &&
            (task_state == VL_TASK_WAITING ||
             task_state == VL_TASK_SLEEPING)) {
            task->wake_pending = 0;
            atomic_store_explicit(&task->state, VL_TASK_RUNNABLE,
                                  memory_order_release);
            task_state = VL_TASK_RUNNABLE;
        }
        if (task_state == VL_TASK_RUNNABLE) {
            vl_task_enqueue_local_locked(p, task);
        } else if (task_state == VL_TASK_RUNNING) {
            atomic_store_explicit(&task->state, VL_TASK_CANCELLED,
                                  memory_order_release);
            if (runtime->live_tasks != 0) {
                --runtime->live_tasks;
            }
            ++runtime->stats.cancelled;
            destroy_fiber = task->fiber;
            task->fiber = NULL;
        }
    }
    if (runtime->live_tasks == 0) {
        vl_worker_stop_run_locked(runtime, VL_OK);
    }
    if (runtime->active_workers == 0) {
        pthread_cond_broadcast(&runtime->condition);
    }
    pthread_mutex_unlock(&runtime->mutex);
    if (destroy_fiber != NULL) {
        vl_fiber_destroy(destroy_fiber);
    }
    vl_runtime_wake_workers(runtime);
}

static int vl_worker_poll_io(vl_p_t *p)
{
    vl_runtime_impl_t *runtime = p->runtime;
    vl_io_t *io;
    vl_io_completion_t completion;

    if (p->id != 0) {
        return VL_ERROR_WOULD_BLOCK;
    }
    pthread_mutex_lock(&runtime->mutex);
    io = runtime->io_waiting != 0 ? runtime->io_driver : NULL;
    pthread_mutex_unlock(&runtime->mutex);
    return io != NULL ? vl_io_poll(io, 0, &completion)
                      : VL_ERROR_WOULD_BLOCK;
}

static void vl_worker_idle(vl_p_t *p)
{
    vl_runtime_impl_t *runtime = p->runtime;
    struct pollfd descriptor;
    int poll_timeout;
    int should_wait;

    pthread_mutex_lock(&runtime->mutex);
    if (!atomic_load_explicit(&runtime->running, memory_order_relaxed)) {
        pthread_mutex_unlock(&runtime->mutex);
        return;
    }
    if (!p->idle) {
        p->idle = 1;
        ++runtime->idle_workers;
        ++p->stats.parks;
    }
    if (runtime->idle_workers == runtime->worker_count &&
        runtime->global_runnable.length == 0 &&
        runtime->live_tasks != 0 && runtime->timer_count == 0 &&
        runtime->io_waiting == 0) {
        size_t index;
        int local_work = 0;

        for (index = 0; index < runtime->worker_count; ++index) {
            if (vl_run_queue_length(&runtime->ps[index].runnable) != 0) {
                local_work = 1;
                break;
            }
        }
        if (!local_work) {
            vl_worker_stop_run_locked(runtime, VL_ERROR_INVALID_STATE);
        }
    }
    should_wait = atomic_load_explicit(&runtime->running,
                                       memory_order_relaxed);
    poll_timeout = vl_timers_timeout_ms(p);
    if (runtime->io_waiting != 0 && p->id == 0 &&
        (poll_timeout < 0 || poll_timeout > 10)) {
        poll_timeout = 10;
    }
    pthread_mutex_unlock(&runtime->mutex);
    if (!should_wait) {
        vl_runtime_wake_workers(runtime);
    } else {
        descriptor.fd = p->event_fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        (void)poll(&descriptor, 1, poll_timeout);
        vl_worker_drain_eventfd(p);
    }
    pthread_mutex_lock(&runtime->mutex);
    if (p->idle) {
        p->idle = 0;
        if (runtime->idle_workers != 0) {
            --runtime->idle_workers;
        }
    }
    pthread_mutex_unlock(&runtime->mutex);
}

int vl_worker_run(vl_p_t *p)
{
    vl_runtime_impl_t *runtime;

    if (p == NULL || (runtime = p->runtime) == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    vl_context_set_current(p, NULL);
    while (atomic_load_explicit(&runtime->running, memory_order_acquire)) {
        vl_task_t *task;

        vl_timers_expire(p, vl_runtime_now_ns());
        while (vl_worker_poll_io(p) == VL_OK) {
        }
        task = vl_worker_next_task(p);
        if (task != NULL) {
            vl_worker_execute(p, task);
        } else {
            vl_worker_idle(p);
        }
    }
    vl_context_set_current(NULL, NULL);
    return VL_OK;
}

void vl_worker_destroy_owned_fibers(vl_p_t *p)
{
    vl_task_t *task;

    if (p == NULL || p->runtime == NULL) {
        return;
    }
    for (task = p->runtime->all_tasks; task != NULL; task = task->all_next) {
        if (task->fiber != NULL && task->last_p == p) {
            vl_fiber_destroy(task->fiber);
            task->fiber = NULL;
        }
    }
}

static void *vl_worker_thread_main(void *argument)
{
    vl_p_t *p = argument;
    vl_runtime_impl_t *runtime = p->runtime;
    struct pollfd descriptor;

    if (vl_fiber_sched_init(&p->fiber_sched) != VL_OK) {
        pthread_mutex_lock(&runtime->mutex);
        p->init_status = VL_ERROR_SYSTEM;
        ++runtime->workers_ready;
        pthread_cond_broadcast(&runtime->condition);
        pthread_mutex_unlock(&runtime->mutex);
        return NULL;
    }
    pthread_mutex_lock(&runtime->mutex);
    p->init_status = VL_OK;
    ++runtime->workers_ready;
    pthread_cond_broadcast(&runtime->condition);
    pthread_mutex_unlock(&runtime->mutex);

    descriptor.fd = p->event_fd;
    descriptor.events = POLLIN;
    while (!atomic_load_explicit(&runtime->stop_workers,
                                 memory_order_acquire)) {
        descriptor.revents = 0;
        (void)poll(&descriptor, 1, -1);
        vl_worker_drain_eventfd(p);
        if (atomic_load_explicit(&runtime->stop_workers,
                                 memory_order_acquire)) {
            break;
        }
        if (atomic_load_explicit(&runtime->running, memory_order_acquire)) {
            (void)vl_worker_run(p);
        }
    }
    pthread_mutex_lock(&runtime->mutex);
    vl_worker_destroy_owned_fibers(p);
    pthread_mutex_unlock(&runtime->mutex);
    vl_fiber_sched_destroy(&p->fiber_sched);
    return NULL;
}

int vl_worker_start(vl_p_t *p)
{
    if (p == NULL || p->runtime == NULL || p->thread_started) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (pthread_create(&p->thread, NULL, vl_worker_thread_main, p) != 0) {
        return VL_ERROR_SYSTEM;
    }
    p->thread_started = 1;
    return VL_OK;
}

void vl_worker_stop(vl_p_t *p)
{
    if (p == NULL || !p->thread_started) {
        return;
    }
    pthread_join(p->thread, NULL);
    p->thread_started = 0;
}
