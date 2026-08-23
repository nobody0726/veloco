#include <veloco/sync.h>

#include "sync_internal.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct vl_semaphore_impl {
    vl_runtime_impl_t *runtime;
    size_t permits;
    vl_sync_wait_queue_t waiters;
} vl_semaphore_impl_t;

int vl_semaphore_init(vl_semaphore_t *semaphore, vl_runtime_t *runtime,
                      size_t permits)
{
    vl_semaphore_impl_t *impl;

    if (semaphore == NULL || semaphore->impl != NULL || runtime == NULL ||
        runtime->impl == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    impl = calloc(1, sizeof(*impl));
    if (impl == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    impl->runtime = runtime->impl;
    impl->permits = permits;
    semaphore->impl = impl;
    return VL_OK;
}

int vl_semaphore_wait(vl_semaphore_t *semaphore)
{
    vl_semaphore_impl_t *impl = semaphore != NULL ? semaphore->impl : NULL;
    vl_p_t *p;
    vl_task_t *task;

    if (impl == NULL || vl_sync_current(impl->runtime, &p, &task) != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    pthread_mutex_lock(&impl->runtime->mutex);
    if (impl->permits != 0) {
        --impl->permits;
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_OK;
    }
    if (vl_sync_park_locked(impl->runtime, p, task) != VL_OK) {
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_ERROR_INVALID_STATE;
    }
    vl_sync_wait_push(&impl->waiters, task);
    pthread_mutex_unlock(&impl->runtime->mutex);
    return vl_sync_resume_result(task);
}

int vl_semaphore_post(vl_semaphore_t *semaphore)
{
    vl_semaphore_impl_t *impl = semaphore != NULL ? semaphore->impl : NULL;
    vl_task_t *waiter;

    if (impl == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&impl->runtime->mutex);
    waiter = vl_sync_wait_pop(&impl->waiters);
    if (waiter != NULL) {
        waiter->wait_result = VL_OK;
        vl_task_wake_locked(waiter);
    } else if (impl->permits == SIZE_MAX) {
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_ERROR_INVALID_STATE;
    } else {
        ++impl->permits;
    }
    pthread_mutex_unlock(&impl->runtime->mutex);
    if (waiter != NULL) {
        vl_runtime_wake_workers(impl->runtime);
    }
    return VL_OK;
}

int vl_semaphore_destroy(vl_semaphore_t *semaphore)
{
    vl_semaphore_impl_t *impl = semaphore != NULL ? semaphore->impl : NULL;

    if (impl == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&impl->runtime->mutex);
    if (impl->waiters.length != 0) {
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_ERROR_INVALID_STATE;
    }
    semaphore->impl = NULL;
    pthread_mutex_unlock(&impl->runtime->mutex);
    free(impl);
    return VL_OK;
}
