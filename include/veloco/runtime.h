#ifndef VELOCO_RUNTIME_H
#define VELOCO_RUNTIME_H

#include <veloco/common.h>

#include <stddef.h>

#define VL_RUNTIME_DEFAULT_TASK_STACK_SIZE ((size_t)(64 * 1024))

typedef struct vl_runtime {
    void *impl;
} vl_runtime_t;

typedef struct vl_runtime_config {
    size_t task_stack_size;
} vl_runtime_config_t;

typedef struct vl_runtime_stats {
    size_t spawned;
    size_t completed;
    size_t cancelled;
    size_t runnable;
} vl_runtime_stats_t;

VL_API int vl_runtime_init(vl_runtime_t *runtime);
VL_API int vl_runtime_init_with_config(vl_runtime_t *runtime,
                                       const vl_runtime_config_t *config);
VL_API int vl_runtime_run(vl_runtime_t *runtime);
VL_API void vl_runtime_request_shutdown(vl_runtime_t *runtime);
VL_API void vl_runtime_get_stats(const vl_runtime_t *runtime,
                                 vl_runtime_stats_t *out);
VL_API void vl_runtime_shutdown(vl_runtime_t *runtime);

#endif
