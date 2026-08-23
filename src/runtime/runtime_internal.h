#ifndef VELOCO_RUNTIME_INTERNAL_H
#define VELOCO_RUNTIME_INTERNAL_H

#include <veloco/runtime.h>
#include <veloco/task.h>
#include <veloco/fiber.h>

#include <pthread.h>
#include <stddef.h>

typedef struct vl_task_waiter vl_task_waiter_t;
typedef struct vl_runtime_impl vl_runtime_impl_t;

struct vl_task {
    vl_runtime_impl_t *runtime;
    vl_fiber_t *fiber;
    vl_task_fn fn;
    void *arg;
    vl_task_state_t state;
    int queued;
    vl_task_t *queue_next;
    vl_task_t *all_next;
    vl_task_t *waiters;
    vl_task_t *waiter_next;
    vl_task_t *waiting_on;
};

typedef struct vl_task_queue {
    vl_task_t *head;
    vl_task_t *tail;
    size_t length;
} vl_task_queue_t;

struct vl_runtime_impl {
    vl_fiber_sched_t fiber_sched;
    vl_task_queue_t runnable;
    vl_task_t *all_tasks;
    vl_task_t *current;
    pthread_t owner_thread;
    size_t task_stack_size;
    int shutdown_requested;
    vl_runtime_stats_t stats;
};

void vl_task_queue_push(vl_task_queue_t *queue, vl_task_t *task);
vl_task_t *vl_task_queue_pop(vl_task_queue_t *queue);

int vl_runtime_is_owner(const vl_runtime_impl_t *runtime);
int vl_task_is_terminal(const vl_task_t *task);
void vl_task_enqueue(vl_runtime_impl_t *runtime, vl_task_t *task);
void vl_task_cancel_all(vl_runtime_impl_t *runtime);
void vl_runtime_run_internal(vl_runtime_impl_t *runtime);
void vl_runtime_destroy_tasks(vl_runtime_impl_t *runtime);

#endif
