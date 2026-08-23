#ifndef VELOCO_RUN_QUEUE_H
#define VELOCO_RUN_QUEUE_H

#include <veloco/common.h>

#include <stdatomic.h>
#include <stddef.h>

typedef struct vl_run_queue {
    _Atomic size_t top;
    _Atomic size_t bottom;
    _Atomic(void *) *slots;
    size_t capacity;
    size_t mask;
} vl_run_queue_t;

int vl_run_queue_init(vl_run_queue_t *queue, size_t capacity);
void vl_run_queue_destroy(vl_run_queue_t *queue);
int vl_run_queue_owner_push(vl_run_queue_t *queue, void *item);
void *vl_run_queue_owner_pop(vl_run_queue_t *queue);
void *vl_run_queue_steal(vl_run_queue_t *queue);
size_t vl_run_queue_steal_batch(vl_run_queue_t *queue, void **items,
                                size_t maximum);
size_t vl_run_queue_length(const vl_run_queue_t *queue);

#endif
