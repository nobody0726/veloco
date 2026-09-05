#ifndef VELOCO_RUNTIME_H
#define VELOCO_RUNTIME_H

#include <veloco/common.h>

#include <stddef.h>

#define VL_RUNTIME_DEFAULT_TASK_STACK_SIZE ((size_t)(64 * 1024))
#define VL_RUNTIME_DEFAULT_WORKER_COUNT ((size_t)1)

typedef struct vl_runtime {
    void *impl;
} vl_runtime_t;

typedef struct vl_runtime_config {
    size_t task_stack_size;
    size_t worker_count;
} vl_runtime_config_t;

typedef struct vl_runtime_stats {
    size_t spawned;
    size_t completed;
    size_t cancelled;
    size_t runnable;
    size_t worker_count;
    size_t steals;
    size_t parks;
    size_t task_switches;
} vl_runtime_stats_t;

typedef struct vl_runtime_p_stats {
    size_t executed;
    size_t local_pushes;
    size_t global_pulls;
    size_t steal_attempts;
    size_t steals;
    size_t parks;
} vl_runtime_p_stats_t;

VL_API int vl_runtime_init(vl_runtime_t *runtime);
VL_API int vl_runtime_init_with_config(vl_runtime_t *runtime,
                                       const vl_runtime_config_t *config);
VL_API int vl_runtime_run(vl_runtime_t *runtime);
VL_API void vl_runtime_request_shutdown(vl_runtime_t *runtime);
VL_API void vl_runtime_get_stats(const vl_runtime_t *runtime,
                                 vl_runtime_stats_t *out);
VL_API size_t vl_runtime_worker_count(const vl_runtime_t *runtime);
VL_API int vl_runtime_get_p_stats(const vl_runtime_t *runtime, size_t p_index,
                                  vl_runtime_p_stats_t *out);
VL_API void vl_runtime_shutdown(vl_runtime_t *runtime);

#endif
