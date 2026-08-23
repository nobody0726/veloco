#ifndef VELOCO_TIMER_HEAP_H
#define VELOCO_TIMER_HEAP_H

#include <veloco/common.h>

#include <stddef.h>
#include <stdint.h>

typedef struct vl_timer_node {
    uint64_t deadline_ns;
    size_t heap_index;
    int active;
    void *owner;
} vl_timer_node_t;

typedef struct vl_timer_heap {
    vl_timer_node_t **items;
    size_t length;
    size_t capacity;
} vl_timer_heap_t;

int vl_timer_heap_init(vl_timer_heap_t *heap);
void vl_timer_heap_destroy(vl_timer_heap_t *heap);
int vl_timer_heap_push(vl_timer_heap_t *heap, vl_timer_node_t *node);
vl_timer_node_t *vl_timer_heap_peek(const vl_timer_heap_t *heap);
vl_timer_node_t *vl_timer_heap_pop(vl_timer_heap_t *heap);
void vl_timer_heap_remove(vl_timer_heap_t *heap, vl_timer_node_t *node);

#endif
