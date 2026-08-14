#ifndef VELOCO_FIBER_H
#define VELOCO_FIBER_H

#include <veloco/common.h>

#include <stddef.h>

typedef struct vl_fiber vl_fiber_t;

typedef struct vl_fiber_sched {
    void *impl;
} vl_fiber_sched_t;

typedef long (*vl_fiber_fn)(void *arg);

typedef enum vl_fiber_state {
    VL_FIBER_READY = 0,
    VL_FIBER_RUNNING = 1,
    VL_FIBER_SUSPENDED = 2,
    VL_FIBER_DONE = 3
} vl_fiber_state_t;

/*
 * A scheduler and all fibers created from it are bound to the thread that
 * initializes the scheduler. One scheduler may be active per thread.
 */
VL_API int vl_fiber_sched_init(vl_fiber_sched_t *sched);
VL_API void vl_fiber_sched_destroy(vl_fiber_sched_t *sched);

/* stack_size is usable stack space; the implementation adds a guard page. */
VL_API int vl_fiber_create(vl_fiber_sched_t *sched, vl_fiber_t **out,
                           size_t stack_size, vl_fiber_fn fn, void *arg);

/* return_value receives either the value yielded or the function result. */
VL_API int vl_fiber_resume(vl_fiber_sched_t *sched, vl_fiber_t *fiber,
                           long send_value, long *return_value);
VL_API long vl_fiber_yield(vl_fiber_sched_t *sched, long send_value);
VL_API vl_fiber_state_t vl_fiber_get_state(const vl_fiber_t *fiber);
VL_API void vl_fiber_destroy(vl_fiber_t *fiber);

#endif
