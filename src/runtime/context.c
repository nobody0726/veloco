#include "runtime_internal.h"

static _Thread_local vl_p_t *vl_tls_p;

vl_p_t *vl_current_p(void)
{
    return vl_tls_p;
}

vl_runtime_impl_t *vl_current_runtime(void)
{
    return vl_tls_p != NULL ? vl_tls_p->runtime : NULL;
}

void vl_context_set_current(vl_p_t *p, vl_task_t *task)
{
    vl_tls_p = p;
    if (p != NULL) {
        p->current = task;
    }
}

int vl_task_prepare_park(vl_task_state_t state)
{
    vl_p_t *p = vl_current_p();
    vl_task_t *task = p != NULL ? p->current : NULL;
    vl_runtime_impl_t *runtime = p != NULL ? p->runtime : NULL;

    if (task == NULL || runtime == NULL ||
        (state != VL_TASK_WAITING && state != VL_TASK_SLEEPING)) {
        return VL_ERROR_INVALID_STATE;
    }
    pthread_mutex_lock(&runtime->mutex);
    if (atomic_load_explicit(&task->state, memory_order_relaxed) !=
            VL_TASK_RUNNING ||
        !task->executing) {
        pthread_mutex_unlock(&runtime->mutex);
        return VL_ERROR_INVALID_STATE;
    }
    atomic_store_explicit(&task->state, state, memory_order_release);
    ++runtime->stats.parks;
    ++p->stats.parks;
    pthread_mutex_unlock(&runtime->mutex);
    return VL_OK;
}

void vl_task_commit_park(void)
{
    vl_p_t *p = vl_current_p();

    if (p == NULL || p->current == NULL) {
        return;
    }
    (void)vl_fiber_yield(&p->fiber_sched, 0);
}
