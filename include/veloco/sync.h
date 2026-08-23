#ifndef VELOCO_SYNC_H
#define VELOCO_SYNC_H

#include <veloco/common.h>
#include <veloco/runtime.h>

#include <stddef.h>

typedef struct vl_task_mutex {
    void *impl;
} vl_task_mutex_t;

typedef struct vl_semaphore {
    void *impl;
} vl_semaphore_t;

typedef struct vl_wait_group {
    void *impl;
} vl_wait_group_t;

typedef struct vl_channel {
    void *impl;
} vl_channel_t;

VL_API int vl_task_mutex_init(vl_task_mutex_t *mutex, vl_runtime_t *runtime);
VL_API int vl_task_mutex_lock(vl_task_mutex_t *mutex);
VL_API int vl_task_mutex_unlock(vl_task_mutex_t *mutex);
VL_API int vl_task_mutex_destroy(vl_task_mutex_t *mutex);

VL_API int vl_semaphore_init(vl_semaphore_t *semaphore,
                             vl_runtime_t *runtime, size_t permits);
VL_API int vl_semaphore_wait(vl_semaphore_t *semaphore);
VL_API int vl_semaphore_post(vl_semaphore_t *semaphore);
VL_API int vl_semaphore_destroy(vl_semaphore_t *semaphore);

VL_API int vl_wait_group_init(vl_wait_group_t *group, vl_runtime_t *runtime);
VL_API int vl_wait_group_add(vl_wait_group_t *group, ptrdiff_t delta);
VL_API int vl_wait_group_done(vl_wait_group_t *group);
VL_API int vl_wait_group_wait(vl_wait_group_t *group);
VL_API int vl_wait_group_destroy(vl_wait_group_t *group);

VL_API int vl_channel_init(vl_channel_t *channel, vl_runtime_t *runtime,
                           size_t capacity);
VL_API int vl_channel_send(vl_channel_t *channel, void *value);
VL_API int vl_channel_receive(vl_channel_t *channel, void **out);
VL_API int vl_channel_close(vl_channel_t *channel);
VL_API int vl_channel_destroy(vl_channel_t *channel);

#endif
