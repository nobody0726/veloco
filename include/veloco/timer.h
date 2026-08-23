#ifndef VELOCO_TIMER_H
#define VELOCO_TIMER_H

#include <veloco/common.h>
#include <veloco/runtime.h>

#include <stdint.h>

typedef struct vl_timer {
    void *impl;
} vl_timer_t;

/* Arm from a running Task; completion returns VL_OK or VL_ERROR_CANCELLED. */
VL_API int vl_timer_init(vl_timer_t *timer, vl_runtime_t *runtime);
VL_API int vl_timer_arm(vl_timer_t *timer, uint64_t delay_ns);
VL_API int vl_timer_cancel(vl_timer_t *timer);
VL_API int vl_timer_destroy(vl_timer_t *timer);
VL_API int vl_sleep_ns(uint64_t delay_ns);

#endif
