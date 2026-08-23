#ifndef VELOCO_SYNC_INTERNAL_H
#define VELOCO_SYNC_INTERNAL_H

#include "../runtime/runtime_internal.h"

#include <limits.h>

typedef struct vl_sync_wait_queue {
    vl_task_t *head;
    vl_task_t *tail;
    size_t length;
} vl_sync_wait_queue_t;

static inline void vl_sync_wait_push(vl_sync_wait_queue_t *queue,
                                     vl_task_t *task)
{
    task->waiter_next = NULL;
    if (queue->tail == NULL) {
        queue->head = task;
    } else {
        queue->tail->waiter_next = task;
    }
    queue->tail = task;
    ++queue->length;
}

static inline vl_task_t *vl_sync_wait_pop(vl_sync_wait_queue_t *queue)
{
    vl_task_t *task = queue->head;

    if (task == NULL) {
        return NULL;
    }
    queue->head = task->waiter_next;
    if (queue->head == NULL) {
        queue->tail = NULL;
    }
    task->waiter_next = NULL;
    --queue->length;
    return task;
}

static inline int vl_sync_current(vl_runtime_impl_t *runtime, vl_p_t **out_p,
                                  vl_task_t **out_task)
{
    vl_p_t *p = vl_current_p();
    vl_task_t *task = p != NULL ? p->current : NULL;

    if (runtime == NULL || p == NULL || task == NULL ||
        p->runtime != runtime || task->runtime != runtime) {
        return VL_ERROR_INVALID_STATE;
    }
    *out_p = p;
    *out_task = task;
    return VL_OK;
}

static inline int vl_sync_park_locked(vl_runtime_impl_t *runtime, vl_p_t *p,
                                      vl_task_t *task)
{
    if (atomic_load_explicit(&task->state, memory_order_relaxed) !=
            VL_TASK_RUNNING ||
        !task->executing) {
        return VL_ERROR_INVALID_STATE;
    }
    atomic_store_explicit(&task->state, VL_TASK_WAITING,
                          memory_order_release);
    task->wait_result = VL_OK;
    ++runtime->stats.parks;
    ++p->stats.parks;
    return VL_OK;
}

static inline int vl_sync_resume_result(vl_task_t *task)
{
    vl_task_commit_park();
    return task->wait_result;
}

#endif
