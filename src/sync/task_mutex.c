#include <veloco/sync.h>

#include "sync_internal.h"

#include <stdlib.h>

typedef struct vl_task_mutex_impl {
    vl_runtime_impl_t *runtime;
    vl_task_t *owner;
    vl_sync_wait_queue_t waiters;
} vl_task_mutex_impl_t;

int vl_task_mutex_init(vl_task_mutex_t *mutex, vl_runtime_t *runtime)
{
    vl_task_mutex_impl_t *impl;

    if (mutex == NULL || mutex->impl != NULL || runtime == NULL ||
        runtime->impl == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    impl = calloc(1, sizeof(*impl));
    if (impl == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    impl->runtime = runtime->impl;
    mutex->impl = impl;
    return VL_OK;
}

int vl_task_mutex_lock(vl_task_mutex_t *mutex)
{
    vl_task_mutex_impl_t *impl = mutex != NULL ? mutex->impl : NULL;
    vl_p_t *p;
    vl_task_t *task;

    if (impl == NULL || vl_sync_current(impl->runtime, &p, &task) != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    pthread_mutex_lock(&impl->runtime->mutex);
    if (impl->owner == NULL) {
        impl->owner = task;
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_OK;
    }
    if (impl->owner == task ||
        vl_sync_park_locked(impl->runtime, p, task) != VL_OK) {
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_ERROR_INVALID_STATE;
    }
    vl_sync_wait_push(&impl->waiters, task);
    pthread_mutex_unlock(&impl->runtime->mutex);
    return vl_sync_resume_result(task);
}

int vl_task_mutex_unlock(vl_task_mutex_t *mutex)
{
    vl_task_mutex_impl_t *impl = mutex != NULL ? mutex->impl : NULL;
    vl_p_t *p;
    vl_task_t *task;
    vl_task_t *next;

    if (impl == NULL || vl_sync_current(impl->runtime, &p, &task) != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    (void)p;
    pthread_mutex_lock(&impl->runtime->mutex);
    if (impl->owner != task) {
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_ERROR_INVALID_STATE;
    }
    next = vl_sync_wait_pop(&impl->waiters);
    impl->owner = next;
    if (next != NULL) {
        next->wait_result = VL_OK;
        vl_task_wake_locked(next);
    }
    pthread_mutex_unlock(&impl->runtime->mutex);
    if (next != NULL) {
        vl_runtime_wake_workers(impl->runtime);
    }
    return VL_OK;
}

int vl_task_mutex_destroy(vl_task_mutex_t *mutex)
{
    vl_task_mutex_impl_t *impl = mutex != NULL ? mutex->impl : NULL;

    if (impl == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&impl->runtime->mutex);
    if (impl->owner != NULL || impl->waiters.length != 0) {
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_ERROR_INVALID_STATE;
    }
    mutex->impl = NULL;
    pthread_mutex_unlock(&impl->runtime->mutex);
    free(impl);
    return VL_OK;
}
