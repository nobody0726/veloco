#include "runtime_internal.h"

#include <veloco/io.h>
#include <veloco/memory.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define VL_RUNTIME_MAX_WORKERS ((size_t)64)

void vl_task_queue_push(vl_task_queue_t *queue, vl_task_t *task)
{
    task->queue_next = NULL;
    if (queue->tail == NULL) {
        queue->head = task;
    } else {
        queue->tail->queue_next = task;
    }
    queue->tail = task;
    ++queue->length;
}

vl_task_t *vl_task_queue_pop(vl_task_queue_t *queue)
{
    vl_task_t *task;

    if (queue == NULL || queue->head == NULL) {
        return NULL;
    }
    task = queue->head;
    queue->head = task->queue_next;
    if (queue->head == NULL) {
        queue->tail = NULL;
    }
    task->queue_next = NULL;
    --queue->length;
    return task;
}

int vl_runtime_is_owner(const vl_runtime_impl_t *runtime)
{
    return runtime != NULL &&
           pthread_equal(runtime->owner_thread, pthread_self());
}

void vl_runtime_wake_workers(vl_runtime_impl_t *runtime)
{
    size_t index;
    uint64_t value = 1;

    if (runtime == NULL || runtime->ps == NULL) {
        return;
    }
    for (index = 0; index < runtime->worker_count; ++index) {
        ssize_t result;

        do {
            result = write(runtime->ps[index].event_fd, &value,
                           sizeof(value));
        } while (result < 0 && errno == EINTR);
        if (result < 0 && errno != EAGAIN) {
            continue;
        }
    }
}

static int vl_task_mark_queued(vl_task_t *task)
{
    int expected = 0;

    return atomic_compare_exchange_strong_explicit(
        &task->queued, &expected, 1, memory_order_acq_rel,
        memory_order_relaxed);
}

void vl_task_enqueue_global_locked(vl_runtime_impl_t *runtime,
                                   vl_task_t *task)
{
    if (runtime == NULL || task == NULL ||
        atomic_load_explicit(&task->state, memory_order_acquire) !=
            VL_TASK_RUNNABLE ||
        !vl_task_mark_queued(task)) {
        return;
    }
    vl_task_queue_push(&runtime->global_runnable, task);
}

void vl_task_enqueue_local_locked(vl_p_t *p, vl_task_t *task)
{
    vl_runtime_impl_t *runtime;

    if (p == NULL || task == NULL ||
        atomic_load_explicit(&task->state, memory_order_acquire) !=
            VL_TASK_RUNNABLE ||
        !vl_task_mark_queued(task)) {
        return;
    }
    runtime = p->runtime;
    if (runtime->worker_count == 1 || task->fiber != NULL) {
        vl_task_queue_push(&runtime->global_runnable, task);
        return;
    }
    if (vl_run_queue_owner_push(&p->runnable, task) == VL_OK) {
        ++p->stats.local_pushes;
        return;
    }
    vl_task_queue_push(&runtime->global_runnable, task);
}

void vl_task_wake_locked(vl_task_t *task)
{
    vl_runtime_impl_t *runtime;
    int state;

    if (task == NULL || (runtime = task->runtime) == NULL) {
        return;
    }
    state = atomic_load_explicit(&task->state, memory_order_acquire);
    if (state != VL_TASK_WAITING && state != VL_TASK_SLEEPING) {
        return;
    }
    if (task->executing) {
        task->wake_pending = 1;
        return;
    }
    atomic_store_explicit(&task->state, VL_TASK_RUNNABLE,
                          memory_order_release);
    vl_task_enqueue_global_locked(runtime, task);
}

void vl_task_cancel_all(vl_runtime_impl_t *runtime)
{
    vl_task_t *task;

    if (runtime == NULL) {
        return;
    }
    pthread_mutex_lock(&runtime->mutex);
    for (task = runtime->all_tasks; task != NULL; task = task->all_next) {
        if (!vl_task_is_terminal(task) && !task->executing) {
            atomic_store_explicit(&task->state, VL_TASK_CANCELLED,
                                  memory_order_release);
            if (task->waiting_for_io && runtime->io_waiting != 0) {
                task->waiting_for_io = 0;
                --runtime->io_waiting;
            }
            if (runtime->live_tasks != 0) {
                --runtime->live_tasks;
            }
            ++runtime->stats.cancelled;
        }
    }
    pthread_mutex_unlock(&runtime->mutex);
}

static size_t vl_runtime_runnable_count(const vl_runtime_impl_t *runtime)
{
    size_t count = runtime->global_runnable.length;
    size_t index;

    for (index = 0; index < runtime->worker_count; ++index) {
        count += vl_run_queue_length(&runtime->ps[index].runnable);
    }
    return count;
}

void vl_runtime_get_stats(const vl_runtime_t *runtime,
                          vl_runtime_stats_t *out)
{
    vl_runtime_impl_t *impl;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (runtime == NULL || (impl = runtime->impl) == NULL) {
        return;
    }
    pthread_mutex_lock(&impl->mutex);
    *out = impl->stats;
    out->runnable = vl_runtime_runnable_count(impl);
    pthread_mutex_unlock(&impl->mutex);
}

size_t vl_runtime_worker_count(const vl_runtime_t *runtime)
{
    vl_runtime_impl_t *impl = runtime != NULL ? runtime->impl : NULL;

    return impl != NULL ? impl->worker_count : 0;
}

int vl_runtime_get_p_stats(const vl_runtime_t *runtime, size_t p_index,
                           vl_runtime_p_stats_t *out)
{
    vl_runtime_impl_t *impl;

    if (runtime == NULL || out == NULL ||
        (impl = runtime->impl) == NULL || p_index >= impl->worker_count) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&impl->mutex);
    *out = impl->ps[p_index].stats;
    pthread_mutex_unlock(&impl->mutex);
    return VL_OK;
}

static void vl_runtime_cleanup_partial(vl_runtime_impl_t *impl,
                                       size_t initialized_ps)
{
    size_t index;

    if (impl == NULL) {
        return;
    }
    pthread_mutex_lock(&impl->mutex);
    atomic_store_explicit(&impl->stop_workers, 1, memory_order_release);
    pthread_mutex_unlock(&impl->mutex);
    vl_runtime_wake_workers(impl);
    for (index = 1; index < initialized_ps; ++index) {
        if (impl->ps[index].thread_started) {
            vl_worker_stop(&impl->ps[index]);
        }
    }
    if (initialized_ps != 0 && impl->ps[0].fiber_sched.impl != NULL) {
        vl_fiber_sched_destroy(&impl->ps[0].fiber_sched);
    }
    for (index = 0; index < initialized_ps; ++index) {
        if (impl->ps[index].event_fd >= 0) {
            close(impl->ps[index].event_fd);
        }
        vl_run_queue_destroy(&impl->ps[index].runnable);
    }
    free(impl->ps);
    pthread_cond_destroy(&impl->condition);
    pthread_mutex_destroy(&impl->mutex);
    free(impl);
}

int vl_runtime_init(vl_runtime_t *runtime)
{
    return vl_runtime_init_with_config(runtime, NULL);
}

int vl_runtime_init_with_config(vl_runtime_t *runtime,
                                const vl_runtime_config_t *config)
{
    vl_runtime_impl_t *impl;
    size_t stack_size = VL_RUNTIME_DEFAULT_TASK_STACK_SIZE;
    size_t worker_count = VL_RUNTIME_DEFAULT_WORKER_COUNT;
    size_t index;

    if (runtime == NULL || runtime->impl != NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (config != NULL) {
        if (config->task_stack_size != 0) {
            stack_size = config->task_stack_size;
        }
        if (config->worker_count != 0) {
            worker_count = config->worker_count;
        }
    }
    if (worker_count == 0 || worker_count > VL_RUNTIME_MAX_WORKERS) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (vl_allocator_init() != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    impl = calloc(1, sizeof(*impl));
    if (impl == NULL) {
        vl_allocator_shutdown();
        return VL_ERROR_OUT_OF_MEMORY;
    }
    if (pthread_mutex_init(&impl->mutex, NULL) != 0) {
        free(impl);
        vl_allocator_shutdown();
        return VL_ERROR_SYSTEM;
    }
    if (pthread_cond_init(&impl->condition, NULL) != 0) {
        pthread_mutex_destroy(&impl->mutex);
        free(impl);
        vl_allocator_shutdown();
        return VL_ERROR_SYSTEM;
    }
    impl->ps = calloc(worker_count, sizeof(*impl->ps));
    if (impl->ps == NULL) {
        vl_runtime_cleanup_partial(impl, 0);
        vl_allocator_shutdown();
        return VL_ERROR_OUT_OF_MEMORY;
    }
    impl->owner_thread = pthread_self();
    impl->task_stack_size = stack_size;
    impl->worker_count = worker_count;
    impl->stats.worker_count = worker_count;
    impl->run_status = VL_OK;
    atomic_init(&impl->running, 0);
    atomic_init(&impl->stop_workers, 0);
    for (index = 0; index < worker_count; ++index) {
        vl_p_t *p = &impl->ps[index];

        p->runtime = impl;
        p->id = index;
        p->event_fd = -1;
        p->init_status = VL_ERROR_SYSTEM;
        if (vl_run_queue_init(&p->runnable, VL_P_RUN_QUEUE_CAPACITY) != VL_OK) {
            vl_runtime_cleanup_partial(impl, index);
            vl_allocator_shutdown();
            return VL_ERROR_OUT_OF_MEMORY;
        }
        p->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (p->event_fd < 0) {
            vl_runtime_cleanup_partial(impl, index + 1);
            vl_allocator_shutdown();
            return VL_ERROR_SYSTEM;
        }
        if (index == 0) {
            if (vl_fiber_sched_init(&p->fiber_sched) != VL_OK) {
                vl_runtime_cleanup_partial(impl, index + 1);
                vl_allocator_shutdown();
                return VL_ERROR_SYSTEM;
            }
            p->init_status = VL_OK;
        } else if (vl_worker_start(p) != VL_OK) {
            vl_runtime_cleanup_partial(impl, index + 1);
            vl_allocator_shutdown();
            return VL_ERROR_SYSTEM;
        }
    }
    pthread_mutex_lock(&impl->mutex);
    while (impl->workers_ready + 1 < worker_count) {
        pthread_cond_wait(&impl->condition, &impl->mutex);
    }
    for (index = 1; index < worker_count; ++index) {
        if (impl->ps[index].init_status != VL_OK) {
            pthread_mutex_unlock(&impl->mutex);
            vl_runtime_cleanup_partial(impl, worker_count);
            vl_allocator_shutdown();
            return VL_ERROR_SYSTEM;
        }
    }
    pthread_mutex_unlock(&impl->mutex);
    runtime->impl = impl;
    return VL_OK;
}

int vl_runtime_run(vl_runtime_t *runtime)
{
    vl_runtime_impl_t *impl;
    int status;

    if (runtime == NULL || (impl = runtime->impl) == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (!vl_runtime_is_owner(impl) || vl_current_p() != NULL) {
        return VL_ERROR_INVALID_STATE;
    }
    pthread_mutex_lock(&impl->mutex);
    if (atomic_load_explicit(&impl->running, memory_order_acquire)) {
        pthread_mutex_unlock(&impl->mutex);
        return VL_ERROR_INVALID_STATE;
    }
    if (impl->live_tasks == 0) {
        pthread_mutex_unlock(&impl->mutex);
        return VL_OK;
    }
    impl->run_status = VL_OK;
    impl->idle_workers = 0;
    atomic_store_explicit(&impl->running, 1, memory_order_release);
    pthread_mutex_unlock(&impl->mutex);
    vl_runtime_wake_workers(impl);
    (void)vl_worker_run(&impl->ps[0]);

    pthread_mutex_lock(&impl->mutex);
    while (impl->active_workers != 0) {
        pthread_cond_wait(&impl->condition, &impl->mutex);
    }
    status = impl->run_status;
    pthread_mutex_unlock(&impl->mutex);
    if (impl->shutdown_requested) {
        vl_task_cancel_all(impl);
        return VL_OK;
    }
    return status;
}

void vl_runtime_request_shutdown(vl_runtime_t *runtime)
{
    vl_runtime_impl_t *impl;

    if (runtime == NULL || (impl = runtime->impl) == NULL) {
        return;
    }
    pthread_mutex_lock(&impl->mutex);
    impl->shutdown_requested = 1;
    atomic_store_explicit(&impl->running, 0, memory_order_release);
    pthread_mutex_unlock(&impl->mutex);
    vl_runtime_wake_workers(impl);
}

void vl_runtime_shutdown(vl_runtime_t *runtime)
{
    vl_runtime_impl_t *impl;
    size_t index;

    if (runtime == NULL || (impl = runtime->impl) == NULL ||
        !vl_runtime_is_owner(impl) || vl_current_p() != NULL ||
        (impl->io_driver != NULL && impl->io_driver->impl != NULL)) {
        return;
    }
    pthread_mutex_lock(&impl->mutex);
    atomic_store_explicit(&impl->running, 0, memory_order_release);
    atomic_store_explicit(&impl->stop_workers, 1, memory_order_release);
    pthread_mutex_unlock(&impl->mutex);
    vl_runtime_wake_workers(impl);
    for (index = 1; index < impl->worker_count; ++index) {
        vl_worker_stop(&impl->ps[index]);
    }
    vl_task_cancel_all(impl);
    vl_worker_destroy_owned_fibers(&impl->ps[0]);
    vl_fiber_sched_destroy(&impl->ps[0].fiber_sched);
    vl_runtime_destroy_tasks(impl);
    for (index = 0; index < impl->worker_count; ++index) {
        close(impl->ps[index].event_fd);
        vl_run_queue_destroy(&impl->ps[index].runnable);
    }
    free(impl->ps);
    pthread_cond_destroy(&impl->condition);
    pthread_mutex_destroy(&impl->mutex);
    free(impl);
    runtime->impl = NULL;
    vl_allocator_shutdown();
}
