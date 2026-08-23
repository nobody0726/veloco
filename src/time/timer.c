#include <veloco/timer.h>

#include "../runtime/runtime_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct vl_timer_impl {
    vl_runtime_impl_t *runtime;
    vl_task_t *task;
    vl_p_t *p;
    vl_timer_node_t node;
} vl_timer_impl_t;

static int vl_timer_current(vl_runtime_impl_t *runtime, vl_p_t **out_p,
                            vl_task_t **out_task)
{
    vl_p_t *p = vl_current_p();
    vl_task_t *task = p != NULL ? p->current : NULL;

    if (p == NULL || task == NULL || p->runtime != runtime ||
        task->runtime != runtime) {
        return VL_ERROR_INVALID_STATE;
    }
    *out_p = p;
    *out_task = task;
    return VL_OK;
}

int vl_timer_init(vl_timer_t *timer, vl_runtime_t *runtime)
{
    vl_timer_impl_t *impl;

    if (timer == NULL || timer->impl != NULL || runtime == NULL ||
        runtime->impl == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    impl = calloc(1, sizeof(*impl));
    if (impl == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    impl->runtime = runtime->impl;
    impl->node.owner = impl;
    timer->impl = impl;
    return VL_OK;
}

int vl_timer_arm(vl_timer_t *timer, uint64_t delay_ns)
{
    vl_timer_impl_t *impl = timer != NULL ? timer->impl : NULL;
    vl_p_t *p;
    vl_task_t *task;
    uint64_t now;
    int result;

    if (impl == NULL || vl_timer_current(impl->runtime, &p, &task) != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    now = vl_runtime_now_ns();
    if (delay_ns > UINT64_MAX - now) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&impl->runtime->mutex);
    if (impl->node.active || impl->task != NULL ||
        atomic_load_explicit(&task->state, memory_order_relaxed) !=
            VL_TASK_RUNNING || !task->executing) {
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_ERROR_INVALID_STATE;
    }
    atomic_store_explicit(&task->state, VL_TASK_SLEEPING,
                          memory_order_release);
    task->timer_result = VL_OK;
    impl->task = task;
    impl->p = p;
    impl->node.deadline_ns = now + delay_ns;
    result = vl_timer_heap_push(&p->timers, &impl->node);
    if (result == VL_OK) {
        ++impl->runtime->timer_count;
        ++impl->runtime->stats.parks;
        ++p->stats.parks;
    } else {
        impl->task = NULL;
        impl->p = NULL;
        atomic_store_explicit(&task->state, VL_TASK_RUNNING,
                              memory_order_release);
    }
    pthread_mutex_unlock(&impl->runtime->mutex);
    if (result != VL_OK) {
        return result;
    }
    vl_task_commit_park();
    result = task->timer_result;
    impl->task = NULL;
    impl->p = NULL;
    return result;
}

int vl_timer_cancel(vl_timer_t *timer)
{
    vl_timer_impl_t *impl = timer != NULL ? timer->impl : NULL;
    vl_task_t *task;

    if (impl == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&impl->runtime->mutex);
    if (!impl->node.active || impl->p == NULL || impl->task == NULL) {
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_ERROR_INVALID_STATE;
    }
    task = impl->task;
    vl_timer_heap_remove(&impl->p->timers, &impl->node);
    impl->task = NULL;
    impl->p = NULL;
    if (impl->runtime->timer_count != 0) {
        --impl->runtime->timer_count;
    }
    task->timer_result = VL_ERROR_CANCELLED;
    vl_task_wake_locked(task);
    pthread_mutex_unlock(&impl->runtime->mutex);
    vl_runtime_wake_workers(impl->runtime);
    return VL_OK;
}

int vl_timer_destroy(vl_timer_t *timer)
{
    vl_timer_impl_t *impl = timer != NULL ? timer->impl : NULL;

    if (impl == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&impl->runtime->mutex);
    if (impl->node.active || impl->task != NULL) {
        pthread_mutex_unlock(&impl->runtime->mutex);
        return VL_ERROR_INVALID_STATE;
    }
    timer->impl = NULL;
    pthread_mutex_unlock(&impl->runtime->mutex);
    free(impl);
    return VL_OK;
}

int vl_sleep_ns(uint64_t delay_ns)
{
    vl_p_t *p = vl_current_p();
    vl_timer_t timer = {0};
    int result;

    if (p == NULL || p->runtime == NULL) {
        return VL_ERROR_INVALID_STATE;
    }
    result = vl_timer_init(&timer, &(vl_runtime_t){.impl = p->runtime});
    if (result == VL_OK) {
        result = vl_timer_arm(&timer, delay_ns);
        (void)vl_timer_destroy(&timer);
    }
    return result;
}

void vl_timers_expire(vl_p_t *p, uint64_t now_ns)
{
    vl_runtime_impl_t *runtime;

    if (p == NULL || (runtime = p->runtime) == NULL) {
        return;
    }
    pthread_mutex_lock(&runtime->mutex);
    while (vl_timer_heap_peek(&p->timers) != NULL &&
           vl_timer_heap_peek(&p->timers)->deadline_ns <= now_ns) {
        vl_timer_node_t *node = vl_timer_heap_pop(&p->timers);
        vl_timer_impl_t *impl = node->owner;
        vl_task_t *task = impl->task;

        impl->task = NULL;
        impl->p = NULL;
        if (runtime->timer_count != 0) {
            --runtime->timer_count;
        }
        if (task != NULL) {
            task->timer_result = VL_OK;
            vl_task_wake_locked(task);
        }
    }
    pthread_mutex_unlock(&runtime->mutex);
}

int vl_timers_timeout_ms(vl_p_t *p)
{
    vl_timer_node_t *node;
    uint64_t now;
    uint64_t remaining;

    if (p == NULL) {
        return -1;
    }
    node = vl_timer_heap_peek(&p->timers);
    now = vl_runtime_now_ns();
    if (node == NULL || node->deadline_ns <= now) {
        return 0;
    }
    remaining = node->deadline_ns - now;
    if (remaining / UINT64_C(1000000) >= (uint64_t)INT_MAX) {
        return INT_MAX;
    }
    return (int)((remaining + UINT64_C(999999)) / UINT64_C(1000000));
}
