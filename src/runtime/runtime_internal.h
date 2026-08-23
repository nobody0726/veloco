#ifndef VELOCO_RUNTIME_INTERNAL_H
#define VELOCO_RUNTIME_INTERNAL_H

#include <veloco/fiber.h>
#include <veloco/runtime.h>
#include <veloco/task.h>
#include <veloco/timer.h>

#include "run_queue.h"
#include "../time/timer_heap.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

#define VL_P_RUN_QUEUE_CAPACITY ((size_t)1024)
#define VL_STEAL_BATCH_MAX ((size_t)8)

typedef struct vl_runtime_impl vl_runtime_impl_t;
typedef struct vl_p vl_p_t;
typedef struct vl_io vl_io_t;
typedef struct vl_io_completion vl_io_completion_t;

struct vl_task {
    vl_runtime_impl_t *runtime;
    vl_fiber_t *fiber;
    vl_task_fn fn;
    void *arg;
    _Atomic int state;
    _Atomic int queued;
    vl_task_t *queue_next;
    vl_task_t *all_next;
    vl_task_t *waiters_head;
    vl_task_t *waiters_tail;
    vl_task_t *waiter_next;
    vl_task_t *waiting_on;
    void *wait_value;
    void **wait_output;
    int wait_result;
    vl_timer_node_t timer_node;
    vl_p_t *timer_p;
    int timer_result;
    vl_p_t *last_p;
    int executing;
    int wake_pending;
    int waiting_for_io;
};

typedef struct vl_task_queue {
    vl_task_t *head;
    vl_task_t *tail;
    size_t length;
} vl_task_queue_t;

struct vl_p {
    vl_runtime_impl_t *runtime;
    size_t id;
    vl_run_queue_t runnable;
    vl_fiber_sched_t fiber_sched;
    pthread_t thread;
    int event_fd;
    int thread_started;
    int init_status;
    int idle;
    vl_task_t *current;
    vl_runtime_p_stats_t stats;
    vl_timer_heap_t timers;
};

struct vl_runtime_impl {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    vl_task_queue_t global_runnable;
    vl_task_t *all_tasks;
    pthread_t owner_thread;
    vl_p_t *ps;
    size_t worker_count;
    size_t workers_ready;
    size_t task_stack_size;
    size_t live_tasks;
    size_t active_workers;
    size_t idle_workers;
    size_t timer_count;
    vl_io_t *io_driver;
    size_t io_waiting;
    _Atomic int running;
    int run_status;
    int shutdown_requested;
    _Atomic int stop_workers;
    vl_runtime_stats_t stats;
};

void vl_task_queue_push(vl_task_queue_t *queue, vl_task_t *task);
vl_task_t *vl_task_queue_pop(vl_task_queue_t *queue);
long vl_task_entry(void *argument);

int vl_runtime_is_owner(const vl_runtime_impl_t *runtime);
int vl_task_is_terminal(const vl_task_t *task);
void vl_task_enqueue_global_locked(vl_runtime_impl_t *runtime,
                                   vl_task_t *task);
void vl_task_enqueue_local_locked(vl_p_t *p, vl_task_t *task);
void vl_task_wake_locked(vl_task_t *task);
void vl_task_complete_locked(vl_task_t *task);
void vl_runtime_wake_workers(vl_runtime_impl_t *runtime);
void vl_task_cancel_all(vl_runtime_impl_t *runtime);
void vl_runtime_destroy_tasks(vl_runtime_impl_t *runtime);

vl_p_t *vl_current_p(void);
vl_runtime_impl_t *vl_current_runtime(void);
void vl_context_set_current(vl_p_t *p, vl_task_t *task);
int vl_task_prepare_park(vl_task_state_t state);
void vl_task_commit_park(void);

int vl_worker_start(vl_p_t *p);
void vl_worker_stop(vl_p_t *p);
int vl_worker_run(vl_p_t *p);
void vl_worker_destroy_owned_fibers(vl_p_t *p);

int vl_task_park_for_io(vl_task_t *task, vl_io_t *io);
int vl_task_can_park_for_io(vl_task_t *task, vl_io_t *io);
int vl_task_complete_io(vl_task_t *task,
                        const vl_io_completion_t *completion);

uint64_t vl_runtime_now_ns(void);
void vl_timers_expire(vl_p_t *p, uint64_t now_ns);
int vl_timers_timeout_ms(vl_p_t *p);

#endif
