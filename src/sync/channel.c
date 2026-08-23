#include <veloco/sync.h>

#include "sync_internal.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct vl_channel_impl {
    vl_runtime_impl_t *runtime;
    void **buffer;
    size_t capacity;
    size_t head;
    size_t length;
    int closed;
    vl_sync_wait_queue_t senders;
    vl_sync_wait_queue_t receivers;
} vl_channel_impl_t;

static void vl_channel_buffer_push(vl_channel_impl_t *impl, void *value)
{
    size_t tail = (impl->head + impl->length) % impl->capacity;

    impl->buffer[tail] = value;
    ++impl->length;
}

static void *vl_channel_buffer_pop(vl_channel_impl_t *impl)
{
    void *value = impl->buffer[impl->head];

    impl->head = (impl->head + 1) % impl->capacity;
    --impl->length;
    return value;
}

int vl_channel_init(vl_channel_t *channel, vl_runtime_t *runtime,
                    size_t capacity)
{
    vl_channel_impl_t *impl;

    if (channel == NULL || channel->impl != NULL || runtime == NULL ||
        runtime->impl == NULL || capacity > SIZE_MAX / sizeof(void *)) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    impl = calloc(1, sizeof(*impl));
    if (impl == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    if (capacity != 0) {
        impl->buffer = calloc(capacity, sizeof(*impl->buffer));
        if (impl->buffer == NULL) {
            free(impl);
            return VL_ERROR_OUT_OF_MEMORY;
        }
    }
    impl->runtime = runtime->impl;
    impl->capacity = capacity;
    channel->impl = impl;
    return VL_OK;
}

int vl_channel_send(vl_channel_t *channel, void *value)
{
    vl_channel_impl_t *impl = channel != NULL ? channel->impl : NULL;
    vl_p_t *p;
    vl_task_t *task;
    vl_task_t *receiver;

    if (impl == NULL || vl_sync_current(impl->runtime, &p, &task) != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    pthread_mutex_lock(&impl->runtime->mutex);
    if (impl->closed) {
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_ERROR_CLOSED;
    }
    receiver = vl_sync_wait_pop(&impl->receivers);
    if (receiver != NULL) {
        *receiver->wait_output = value;
        receiver->wait_output = NULL;
        receiver->wait_result = VL_OK;
        vl_task_wake_locked(receiver);
        pthread_mutex_unlock(&impl->runtime->mutex);
        vl_runtime_wake_workers(impl->runtime);
        return VL_OK;
    }
    if (impl->length < impl->capacity) {
        vl_channel_buffer_push(impl, value);
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_OK;
    }
    if (vl_sync_park_locked(impl->runtime, p, task) != VL_OK) {
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_ERROR_INVALID_STATE;
    }
    task->wait_value = value;
    vl_sync_wait_push(&impl->senders, task);
    pthread_mutex_unlock(&impl->runtime->mutex);
    return vl_sync_resume_result(task);
}

int vl_channel_receive(vl_channel_t *channel, void **out)
{
    vl_channel_impl_t *impl = channel != NULL ? channel->impl : NULL;
    vl_p_t *p;
    vl_task_t *task;
    vl_task_t *sender;

    if (impl == NULL || out == NULL ||
        vl_sync_current(impl->runtime, &p, &task) != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    pthread_mutex_lock(&impl->runtime->mutex);
    if (impl->length != 0) {
        *out = vl_channel_buffer_pop(impl);
        sender = vl_sync_wait_pop(&impl->senders);
        if (sender != NULL) {
            vl_channel_buffer_push(impl, sender->wait_value);
            sender->wait_value = NULL;
            sender->wait_result = VL_OK;
            vl_task_wake_locked(sender);
        }
        pthread_mutex_unlock(&impl->runtime->mutex);
        if (sender != NULL) {
            vl_runtime_wake_workers(impl->runtime);
        }
        return VL_OK;
    }
    sender = vl_sync_wait_pop(&impl->senders);
    if (sender != NULL) {
        *out = sender->wait_value;
        sender->wait_value = NULL;
        sender->wait_result = VL_OK;
        vl_task_wake_locked(sender);
        pthread_mutex_unlock(&impl->runtime->mutex);
        vl_runtime_wake_workers(impl->runtime);
        return VL_OK;
    }
    if (impl->closed) {
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_ERROR_CLOSED;
    }
    if (vl_sync_park_locked(impl->runtime, p, task) != VL_OK) {
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_ERROR_INVALID_STATE;
    }
    task->wait_output = out;
    vl_sync_wait_push(&impl->receivers, task);
    pthread_mutex_unlock(&impl->runtime->mutex);
    return vl_sync_resume_result(task);
}

int vl_channel_close(vl_channel_t *channel)
{
    vl_channel_impl_t *impl = channel != NULL ? channel->impl : NULL;
    vl_task_t *waiter;
    int wake = 0;

    if (impl == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&impl->runtime->mutex);
    if (impl->closed) {
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_ERROR_CLOSED;
    }
    impl->closed = 1;
    while ((waiter = vl_sync_wait_pop(&impl->senders)) != NULL) {
        waiter->wait_value = NULL;
        waiter->wait_result = VL_ERROR_CLOSED;
        vl_task_wake_locked(waiter);
        wake = 1;
    }
    while ((waiter = vl_sync_wait_pop(&impl->receivers)) != NULL) {
        waiter->wait_output = NULL;
        waiter->wait_result = VL_ERROR_CLOSED;
        vl_task_wake_locked(waiter);
        wake = 1;
    }
    pthread_mutex_unlock(&impl->runtime->mutex);
    if (wake) {
        vl_runtime_wake_workers(impl->runtime);
    }
    return VL_OK;
}

int vl_channel_destroy(vl_channel_t *channel)
{
    vl_channel_impl_t *impl = channel != NULL ? channel->impl : NULL;

    if (impl == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&impl->runtime->mutex);
    if (impl->senders.length != 0 || impl->receivers.length != 0) {
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_ERROR_INVALID_STATE;
    }
    channel->impl = NULL;
    pthread_mutex_unlock(&impl->runtime->mutex);
    free(impl->buffer);
    free(impl);
    return VL_OK;
}
