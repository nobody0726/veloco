#include <veloco/sync.h>

#include "sync_internal.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct vl_wait_group_impl {
    vl_runtime_impl_t *runtime;
    size_t count;
    vl_sync_wait_queue_t waiters;
} vl_wait_group_impl_t;

int vl_wait_group_init(vl_wait_group_t *group, vl_runtime_t *runtime)
{
    vl_wait_group_impl_t *impl;

    if (group == NULL || group->impl != NULL || runtime == NULL ||
        runtime->impl == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    impl = calloc(1, sizeof(*impl));
    if (impl == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    impl->runtime = runtime->impl;
    group->impl = impl;
    return VL_OK;
}

int vl_wait_group_add(vl_wait_group_t *group, ptrdiff_t delta)
{
    vl_wait_group_impl_t *impl = group != NULL ? group->impl : NULL;
    vl_task_t *waiter;
    int wake = 0;

    if (impl == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&impl->runtime->mutex);
    if ((delta < 0 && (size_t)(-(delta + 1)) + 1 > impl->count) ||
        (delta > 0 && (size_t)delta > SIZE_MAX - impl->count) ||
        (delta > 0 && impl->waiters.length != 0)) {
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_ERROR_INVALID_STATE;
    }
    if (delta < 0) {
        impl->count -= (size_t)(-(delta + 1)) + 1;
    } else {
        impl->count += (size_t)delta;
    }
    if (impl->count == 0) {
        while ((waiter = vl_sync_wait_pop(&impl->waiters)) != NULL) {
            waiter->wait_result = VL_OK;
            vl_task_wake_locked(waiter);
            wake = 1;
        }
    }
    pthread_mutex_unlock(&impl->runtime->mutex);
    if (wake) {
        vl_runtime_wake_workers(impl->runtime);
    }
    return VL_OK;
}

int vl_wait_group_done(vl_wait_group_t *group)
{
    return vl_wait_group_add(group, -1);
}

int vl_wait_group_wait(vl_wait_group_t *group)
{
    vl_wait_group_impl_t *impl = group != NULL ? group->impl : NULL;
    vl_p_t *p;
    vl_task_t *task;

    if (impl == NULL || vl_sync_current(impl->runtime, &p, &task) != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    pthread_mutex_lock(&impl->runtime->mutex);
    if (impl->count == 0) {
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

int vl_wait_group_destroy(vl_wait_group_t *group)
{
    vl_wait_group_impl_t *impl = group != NULL ? group->impl : NULL;

    if (impl == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&impl->runtime->mutex);
    if (impl->count != 0 || impl->waiters.length != 0) {
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_ERROR_INVALID_STATE;
    }
    group->impl = NULL;
    pthread_mutex_unlock(&impl->runtime->mutex);
    free(impl);
    return VL_OK;
}
