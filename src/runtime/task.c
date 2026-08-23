#include "runtime_internal.h"

#include <veloco/io.h>

#include <stdlib.h>

long vl_task_entry(void *argument)
{
    vl_task_t *task = argument;

    task->fn(task->arg);
    return 0;
}

int vl_task_is_terminal(const vl_task_t *task)
{
    int state = task != NULL
                    ? atomic_load_explicit(&task->state, memory_order_acquire)
                    : VL_TASK_CANCELLED;

    return state == VL_TASK_DONE || state == VL_TASK_CANCELLED;
}

vl_task_t *vl_spawn(vl_runtime_t *runtime, vl_task_fn fn, void *arg)
{
    vl_runtime_impl_t *impl;
    vl_task_t *task;

    if (runtime == NULL || fn == NULL || runtime->impl == NULL) {
        return NULL;
    }
    impl = runtime->impl;
    if (!vl_runtime_is_owner(impl) && vl_current_runtime() != impl) {
        return NULL;
    }
    task = calloc(1, sizeof(*task));
    if (task == NULL) {
        return NULL;
    }
    task->runtime = impl;
    task->fn = fn;
    task->arg = arg;
    atomic_init(&task->state, VL_TASK_NEW);
    atomic_init(&task->queued, 0);

    pthread_mutex_lock(&impl->mutex);
    if (impl->shutdown_requested) {
        pthread_mutex_unlock(&impl->mutex);
        free(task);
        return NULL;
    }
    task->all_next = impl->all_tasks;
    impl->all_tasks = task;
    atomic_store_explicit(&task->state, VL_TASK_RUNNABLE,
                          memory_order_release);
    vl_task_enqueue_global_locked(impl, task);
    ++impl->live_tasks;
    ++impl->stats.spawned;
    pthread_mutex_unlock(&impl->mutex);
    vl_runtime_wake_workers(impl);
    return task;
}

void vl_yield(void)
{
    vl_p_t *p = vl_current_p();
    vl_task_t *task = p != NULL ? p->current : NULL;
    vl_runtime_impl_t *runtime = p != NULL ? p->runtime : NULL;

    if (task == NULL || runtime == NULL) {
        return;
    }
    pthread_mutex_lock(&runtime->mutex);
    if (atomic_load_explicit(&task->state, memory_order_relaxed) !=
            VL_TASK_RUNNING ||
        !task->executing) {
        pthread_mutex_unlock(&runtime->mutex);
        return;
    }
    atomic_store_explicit(&task->state, VL_TASK_RUNNABLE,
                          memory_order_release);
    pthread_mutex_unlock(&runtime->mutex);
    (void)vl_fiber_yield(&p->fiber_sched, 0);
}

vl_task_t *vl_task_current(void)
{
    vl_p_t *p = vl_current_p();

    return p != NULL ? p->current : NULL;
}

int vl_join(vl_task_t *task)
{
    vl_p_t *p;
    vl_task_t *current;
    vl_runtime_impl_t *runtime;
    int target_state;

    if (task == NULL || (runtime = task->runtime) == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    target_state = atomic_load_explicit(&task->state, memory_order_acquire);
    if (target_state == VL_TASK_DONE) {
        return VL_OK;
    }
    p = vl_current_p();
    current = p != NULL ? p->current : NULL;
    if (current == NULL || current == task || current->runtime != runtime) {
        return VL_ERROR_INVALID_STATE;
    }
    pthread_mutex_lock(&runtime->mutex);
    target_state = atomic_load_explicit(&task->state, memory_order_relaxed);
    if (target_state == VL_TASK_DONE) {
        pthread_mutex_unlock(&runtime->mutex);
        return VL_OK;
    }
    if (target_state == VL_TASK_CANCELLED ||
        atomic_load_explicit(&current->state, memory_order_relaxed) !=
            VL_TASK_RUNNING) {
        pthread_mutex_unlock(&runtime->mutex);
        return VL_ERROR_INVALID_STATE;
    }
    atomic_store_explicit(&current->state, VL_TASK_WAITING,
                          memory_order_release);
    current->waiting_on = task;
    current->waiter_next = NULL;
    if (task->waiters_tail == NULL) {
        task->waiters_head = current;
    } else {
        task->waiters_tail->waiter_next = current;
    }
    task->waiters_tail = current;
    ++runtime->stats.parks;
    ++p->stats.parks;
    pthread_mutex_unlock(&runtime->mutex);
    (void)vl_fiber_yield(&p->fiber_sched, 0);
    return atomic_load_explicit(&task->state, memory_order_acquire) ==
                   VL_TASK_DONE
               ? VL_OK
               : VL_ERROR_INVALID_STATE;
}

vl_task_state_t vl_task_state(const vl_task_t *task)
{
    return task != NULL
               ? (vl_task_state_t)atomic_load_explicit(
                     &task->state, memory_order_acquire)
               : VL_TASK_CANCELLED;
}

void vl_task_complete_locked(vl_task_t *task)
{
    vl_runtime_impl_t *runtime = task->runtime;
    vl_task_t *waiter = task->waiters_head;

    task->waiters_head = NULL;
    task->waiters_tail = NULL;
    while (waiter != NULL) {
        vl_task_t *next = waiter->waiter_next;

        waiter->waiter_next = NULL;
        waiter->waiting_on = NULL;
        vl_task_wake_locked(waiter);
        waiter = next;
    }
    if (runtime->live_tasks != 0) {
        --runtime->live_tasks;
    }
    ++runtime->stats.completed;
}

int vl_task_can_park_for_io(vl_task_t *task, vl_io_t *io)
{
    vl_p_t *p = vl_current_p();
    vl_runtime_impl_t *runtime = p != NULL ? p->runtime : NULL;

    if (task == NULL || io == NULL || runtime == NULL || p->id != 0 ||
        p->current != task || task->runtime != runtime ||
        atomic_load_explicit(&task->state, memory_order_acquire) !=
            VL_TASK_RUNNING ||
        task->waiting_for_io) {
        return VL_ERROR_INVALID_STATE;
    }
    if (runtime->io_waiting != 0 && runtime->io_driver != io) {
        return VL_ERROR_INVALID_STATE;
    }
    return VL_OK;
}

int vl_task_park_for_io(vl_task_t *task, vl_io_t *io)
{
    vl_p_t *p = vl_current_p();
    vl_runtime_impl_t *runtime = p != NULL ? p->runtime : NULL;

    if (vl_task_can_park_for_io(task, io) != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    pthread_mutex_lock(&runtime->mutex);
    runtime->io_driver = io;
    task->waiting_for_io = 1;
    atomic_store_explicit(&task->state, VL_TASK_WAITING,
                          memory_order_release);
    ++runtime->io_waiting;
    ++runtime->stats.parks;
    ++p->stats.parks;
    pthread_mutex_unlock(&runtime->mutex);
    (void)vl_fiber_yield(&p->fiber_sched, 0);
    return atomic_load_explicit(&task->state, memory_order_acquire) ==
                   VL_TASK_RUNNING
               ? VL_OK
               : VL_ERROR_INVALID_STATE;
}

int vl_task_complete_io(vl_task_t *task,
                        const vl_io_completion_t *completion)
{
    vl_runtime_impl_t *runtime;

    if (task == NULL || completion == NULL ||
        (runtime = task->runtime) == NULL) {
        return VL_ERROR_INVALID_STATE;
    }
    pthread_mutex_lock(&runtime->mutex);
    if (atomic_load_explicit(&task->state, memory_order_relaxed) !=
            VL_TASK_WAITING ||
        !task->waiting_for_io || runtime->io_waiting == 0) {
        pthread_mutex_unlock(&runtime->mutex);
        return VL_ERROR_INVALID_STATE;
    }
    task->waiting_for_io = 0;
    --runtime->io_waiting;
    if (runtime->io_waiting == 0) {
        runtime->io_driver = NULL;
    }
    vl_task_wake_locked(task);
    pthread_mutex_unlock(&runtime->mutex);
    vl_runtime_wake_workers(runtime);
    return VL_OK;
}

void vl_runtime_destroy_tasks(vl_runtime_impl_t *runtime)
{
    vl_task_t *task;
    vl_task_t *next;

    for (task = runtime->all_tasks; task != NULL; task = next) {
        next = task->all_next;
        free(task);
    }
    runtime->all_tasks = NULL;
}
