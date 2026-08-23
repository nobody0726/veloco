#ifndef VELOCO_TASK_H
#define VELOCO_TASK_H

#include <veloco/common.h>

typedef struct vl_runtime vl_runtime_t;
typedef struct vl_task vl_task_t;

typedef enum vl_task_state {
    VL_TASK_NEW = 0,
    VL_TASK_RUNNABLE = 1,
    VL_TASK_RUNNING = 2,
    VL_TASK_WAITING = 3,
    VL_TASK_SLEEPING = 4,
    VL_TASK_DONE = 5,
    VL_TASK_CANCELLED = 6
} vl_task_state_t;

typedef void (*vl_task_fn)(void *arg);

VL_API vl_task_t *vl_spawn(vl_runtime_t *runtime, vl_task_fn fn, void *arg);
VL_API void vl_yield(void);
VL_API int vl_join(vl_task_t *task);
VL_API vl_task_state_t vl_task_state(const vl_task_t *task);

#endif
