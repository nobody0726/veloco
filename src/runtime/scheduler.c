#include "runtime_internal.h"

#include <veloco/memory.h>

#include <string.h>

void vl_task_queue_push(vl_task_queue_t *queue, vl_task_t *task)
{
    if (queue == NULL || task == NULL || task->queued != 0) {
        return;
    }
    task->queue_next = NULL;
    task->queued = 1;
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
    task->queued = 0;
    --queue->length;
    return task;
}

int vl_runtime_is_owner(const vl_runtime_impl_t *runtime)
{
    return runtime != NULL &&
           pthread_equal(runtime->owner_thread, pthread_self());
}

void vl_task_enqueue(vl_runtime_impl_t *runtime, vl_task_t *task)
{
    if (runtime == NULL || task == NULL || task->state != VL_TASK_RUNNABLE) {
        return;
    }
    vl_task_queue_push(&runtime->runnable, task);
}

void vl_task_cancel_all(vl_runtime_impl_t *runtime)
{
    vl_task_t *task;

    if (runtime == NULL) {
        return;
    }
    while ((task = vl_task_queue_pop(&runtime->runnable)) != NULL) {
        task->state = VL_TASK_CANCELLED;
        ++runtime->stats.cancelled;
    }
    for (task = runtime->all_tasks; task != NULL; task = task->all_next) {
        if (!vl_task_is_terminal(task)) {
            task->state = VL_TASK_CANCELLED;
            ++runtime->stats.cancelled;
        }
    }
}

void vl_runtime_get_stats(const vl_runtime_t *runtime,
                          vl_runtime_stats_t *out)
{
    const vl_runtime_impl_t *impl;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (runtime == NULL) {
        return;
    }
    impl = runtime->impl;
    if (impl == NULL) {
        return;
    }
    *out = impl->stats;
    out->runnable = impl->runnable.length;
}
