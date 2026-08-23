#include "runtime_internal.h"

#include <veloco/memory.h>
#include <veloco/io.h>

#include <stdlib.h>
#include <string.h>

static _Thread_local vl_runtime_impl_t *vl_current_runtime;

static int vl_runtime_has_nonterminal_tasks(const vl_runtime_impl_t *runtime)
{
    const vl_task_t *task;

    for (task = runtime->all_tasks; task != NULL; task = task->all_next) {
        if (!vl_task_is_terminal(task)) {
            return 1;
        }
    }
    return 0;
}

static long vl_task_entry(void *argument)
{
    vl_task_t *task = argument;

    task->fn(task->arg);
    return 0;
}

static void vl_task_wake_waiters(vl_runtime_impl_t *runtime,
                                 vl_task_t *task)
{
    vl_task_t *waiter = task->waiters;

    task->waiters = NULL;
    while (waiter != NULL) {
        vl_task_t *next = waiter->waiter_next;

        waiter->waiter_next = NULL;
        waiter->waiting_on = NULL;
        if (waiter->state == VL_TASK_WAITING) {
            waiter->state = VL_TASK_RUNNABLE;
            vl_task_enqueue(runtime, waiter);
        }
        waiter = next;
    }
}

int vl_task_is_terminal(const vl_task_t *task)
{
    return task != NULL &&
           (task->state == VL_TASK_DONE ||
            task->state == VL_TASK_CANCELLED);
}

static int vl_task_complete(vl_runtime_impl_t *runtime, vl_task_t *task)
{
    if (runtime == NULL || task == NULL || task->state != VL_TASK_RUNNING ||
        vl_fiber_get_state(task->fiber) != VL_FIBER_DONE) {
        return VL_ERROR_INVALID_STATE;
    }
    task->state = VL_TASK_DONE;
    ++runtime->stats.completed;
    vl_task_wake_waiters(runtime, task);
    return VL_OK;
}

vl_task_t *vl_spawn(vl_runtime_t *runtime, vl_task_fn fn, void *arg)
{
    vl_runtime_impl_t *impl;
    vl_task_t *task;

    if (runtime == NULL || fn == NULL || runtime->impl == NULL) {
        return NULL;
    }
    impl = runtime->impl;
    if (!vl_runtime_is_owner(impl) || impl->shutdown_requested != 0) {
        return NULL;
    }
    task = vl_malloc(sizeof(*task));
    if (task == NULL) {
        return NULL;
    }
    memset(task, 0, sizeof(*task));
    task->runtime = impl;
    task->fn = fn;
    task->arg = arg;
    task->state = VL_TASK_NEW;
    if (vl_fiber_create(&impl->fiber_sched, &task->fiber,
                        impl->task_stack_size, vl_task_entry, task) != VL_OK) {
        vl_free(task);
        return NULL;
    }
    task->all_next = impl->all_tasks;
    impl->all_tasks = task;
    task->state = VL_TASK_RUNNABLE;
    vl_task_enqueue(impl, task);
    ++impl->stats.spawned;
    return task;
}

void vl_yield(void)
{
    vl_runtime_impl_t *runtime = vl_current_runtime;
    vl_task_t *task;

    if (runtime == NULL || (task = runtime->current) == NULL ||
        task->state != VL_TASK_RUNNING) {
        return;
    }
    task->state = VL_TASK_RUNNABLE;
    vl_task_enqueue(runtime, task);
    (void)vl_fiber_yield(&runtime->fiber_sched, 0);
}

vl_task_t *vl_task_current(void)
{
    return vl_current_runtime != NULL ? vl_current_runtime->current : NULL;
}

int vl_task_can_park_for_io(vl_task_t *task, vl_io_t *io)
{
    vl_runtime_impl_t *runtime = vl_current_runtime;

    if (task == NULL || io == NULL || runtime == NULL ||
        runtime->current != task || task->runtime != runtime ||
        task->state != VL_TASK_RUNNING || task->waiting_for_io) {
        return VL_ERROR_INVALID_STATE;
    }
    if (runtime->io_waiting != 0 && runtime->io_driver != io) {
        return VL_ERROR_INVALID_STATE;
    }
    return VL_OK;
}

int vl_task_park_for_io(vl_task_t *task, vl_io_t *io)
{
    vl_runtime_impl_t *runtime = vl_current_runtime;

    if (vl_task_can_park_for_io(task, io) != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    runtime->io_driver = io;
    task->waiting_for_io = 1;
    task->state = VL_TASK_WAITING;
    ++runtime->io_waiting;
    (void)vl_fiber_yield(&runtime->fiber_sched, 0);
    return task->state == VL_TASK_RUNNING ? VL_OK : VL_ERROR_INVALID_STATE;
}

int vl_task_complete_io(vl_task_t *task,
                        const vl_io_completion_t *completion)
{
    vl_runtime_impl_t *runtime;

    if (task == NULL || completion == NULL ||
        (runtime = task->runtime) == NULL || !vl_runtime_is_owner(runtime) ||
        task->state != VL_TASK_WAITING || !task->waiting_for_io ||
        runtime->io_waiting == 0) {
        return VL_ERROR_INVALID_STATE;
    }
    task->waiting_for_io = 0;
    --runtime->io_waiting;
    if (runtime->io_waiting == 0) {
        runtime->io_driver = NULL;
    }
    task->state = VL_TASK_RUNNABLE;
    vl_task_enqueue(runtime, task);
    return VL_OK;
}

int vl_join(vl_task_t *task)
{
    vl_runtime_impl_t *runtime;
    vl_task_t *current;

    if (task == NULL || task->runtime == NULL ||
        !vl_runtime_is_owner(task->runtime)) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    runtime = task->runtime;
    if (vl_task_is_terminal(task)) {
        return task->state == VL_TASK_DONE ? VL_OK : VL_ERROR_INVALID_STATE;
    }
    current = runtime->current;
    if (current == NULL || current == task || current->state != VL_TASK_RUNNING) {
        return VL_ERROR_INVALID_STATE;
    }
    current->state = VL_TASK_WAITING;
    current->waiting_on = task;
    current->waiter_next = task->waiters;
    task->waiters = current;
    (void)vl_fiber_yield(&runtime->fiber_sched, 0);
    return task->state == VL_TASK_DONE ? VL_OK : VL_ERROR_INVALID_STATE;
}

vl_task_state_t vl_task_state(const vl_task_t *task)
{
    return task != NULL ? task->state : VL_TASK_CANCELLED;
}

static int vl_runtime_poll_io(vl_runtime_impl_t *runtime, int timeout_ms)
{
    vl_io_completion_t completion;
    int status;

    if (runtime->io_driver == NULL || runtime->io_waiting == 0) {
        return VL_ERROR_WOULD_BLOCK;
    }
    status = vl_io_poll(runtime->io_driver, timeout_ms, &completion);
    return status;
}

int vl_runtime_run_internal(vl_runtime_impl_t *runtime)
{
    vl_task_t *task;
    long result;

    for (;;) {
        task = vl_task_queue_pop(&runtime->runnable);
        if (task == NULL) {
            int poll_status;

            if (runtime->io_waiting == 0) {
                break;
            }
            if (runtime->shutdown_requested != 0) {
                vl_task_cancel_all(runtime);
                break;
            }
            poll_status = vl_runtime_poll_io(runtime, -1);
            if (poll_status != VL_OK) {
                return poll_status;
            }
            continue;
        }
        if (runtime->shutdown_requested != 0) {
            task->state = VL_TASK_CANCELLED;
            ++runtime->stats.cancelled;
            continue;
        }
        if (task->state != VL_TASK_RUNNABLE) {
            continue;
        }
        task->state = VL_TASK_RUNNING;
        runtime->current = task;
        vl_current_runtime = runtime;
        if (vl_fiber_resume(&runtime->fiber_sched, task->fiber, 0, &result) !=
            VL_OK) {
            task->state = VL_TASK_CANCELLED;
            ++runtime->stats.cancelled;
        } else if (vl_fiber_get_state(task->fiber) == VL_FIBER_DONE) {
            (void)vl_task_complete(runtime, task);
        } else if (task->state == VL_TASK_RUNNING) {
            task->state = VL_TASK_CANCELLED;
            ++runtime->stats.cancelled;
        }
        (void)result;
        runtime->current = NULL;
        vl_current_runtime = NULL;
        while (vl_runtime_poll_io(runtime, 0) == VL_OK) {
        }
    }
    return VL_OK;
}

void vl_runtime_destroy_tasks(vl_runtime_impl_t *runtime)
{
    vl_task_t *task;
    vl_task_t *next;

    for (task = runtime->all_tasks; task != NULL; task = next) {
        next = task->all_next;
        vl_fiber_destroy(task->fiber);
        vl_free(task);
    }
    runtime->all_tasks = NULL;
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

    if (runtime == NULL || runtime->impl != NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (config != NULL && config->task_stack_size != 0) {
        stack_size = config->task_stack_size;
    }
    if (vl_allocator_init() != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    impl = calloc(1, sizeof(*impl));
    if (impl == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    impl->owner_thread = pthread_self();
    impl->task_stack_size = stack_size;
    if (vl_fiber_sched_init(&impl->fiber_sched) != VL_OK) {
        free(impl);
        vl_allocator_shutdown();
        return VL_ERROR_SYSTEM;
    }
    runtime->impl = impl;
    return VL_OK;
}

int vl_runtime_run(vl_runtime_t *runtime)
{
    vl_runtime_impl_t *impl;

    if (runtime == NULL || (impl = runtime->impl) == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (!vl_runtime_is_owner(impl) || impl->current != NULL) {
        return VL_ERROR_INVALID_STATE;
    }
    {
        int run_status = vl_runtime_run_internal(impl);

        if (run_status != VL_OK) {
            return run_status;
        }
    }
    if (impl->shutdown_requested != 0) {
        vl_task_cancel_all(impl);
        return VL_OK;
    }
    return vl_runtime_has_nonterminal_tasks(impl) ? VL_ERROR_INVALID_STATE
                                                  : VL_OK;
}

void vl_runtime_request_shutdown(vl_runtime_t *runtime)
{
    vl_runtime_impl_t *impl;

    if (runtime == NULL || (impl = runtime->impl) == NULL ||
        !vl_runtime_is_owner(impl)) {
        return;
    }
    impl->shutdown_requested = 1;
}

void vl_runtime_shutdown(vl_runtime_t *runtime)
{
    vl_runtime_impl_t *impl;

    if (runtime == NULL || (impl = runtime->impl) == NULL ||
        !vl_runtime_is_owner(impl) || impl->current != NULL ||
        (impl->io_driver != NULL && impl->io_driver->impl != NULL)) {
        return;
    }
    vl_task_cancel_all(impl);
    vl_runtime_destroy_tasks(impl);
    vl_fiber_sched_destroy(&impl->fiber_sched);
    free(impl);
    runtime->impl = NULL;
    vl_allocator_shutdown();
}
