#include "timer_heap.h"

#include <stdlib.h>

static void vl_timer_heap_swap(vl_timer_heap_t *heap, size_t left,
                               size_t right)
{
    vl_timer_node_t *node = heap->items[left];

    heap->items[left] = heap->items[right];
    heap->items[right] = node;
    heap->items[left]->heap_index = left;
    heap->items[right]->heap_index = right;
}

static void vl_timer_heap_up(vl_timer_heap_t *heap, size_t index)
{
    while (index != 0) {
        size_t parent = (index - 1) / 2;

        if (heap->items[parent]->deadline_ns <=
            heap->items[index]->deadline_ns) {
            break;
        }
        vl_timer_heap_swap(heap, parent, index);
        index = parent;
    }
}

static void vl_timer_heap_down(vl_timer_heap_t *heap, size_t index)
{
    for (;;) {
        size_t left = index * 2 + 1;
        size_t right = left + 1;
        size_t smallest = index;

        if (left < heap->length &&
            heap->items[left]->deadline_ns <
                heap->items[smallest]->deadline_ns) {
            smallest = left;
        }
        if (right < heap->length &&
            heap->items[right]->deadline_ns <
                heap->items[smallest]->deadline_ns) {
            smallest = right;
        }
        if (smallest == index) {
            return;
        }
        vl_timer_heap_swap(heap, index, smallest);
        index = smallest;
    }
}

int vl_timer_heap_init(vl_timer_heap_t *heap)
{
    if (heap == NULL || heap->items != NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    return VL_OK;
}

void vl_timer_heap_destroy(vl_timer_heap_t *heap)
{
    if (heap == NULL) {
        return;
    }
    free(heap->items);
    heap->items = NULL;
    heap->length = 0;
    heap->capacity = 0;
}

int vl_timer_heap_push(vl_timer_heap_t *heap, vl_timer_node_t *node)
{
    vl_timer_node_t **items;
    size_t capacity;

    if (heap == NULL || node == NULL || node->active) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (heap->length == heap->capacity) {
        capacity = heap->capacity == 0 ? 16 : heap->capacity * 2;
        if (capacity < heap->capacity ||
            capacity > SIZE_MAX / sizeof(*items)) {
            return VL_ERROR_OUT_OF_MEMORY;
        }
        items = realloc(heap->items, capacity * sizeof(*items));
        if (items == NULL) {
            return VL_ERROR_OUT_OF_MEMORY;
        }
        heap->items = items;
        heap->capacity = capacity;
    }
    node->active = 1;
    node->heap_index = heap->length;
    heap->items[heap->length++] = node;
    vl_timer_heap_up(heap, node->heap_index);
    return VL_OK;
}

vl_timer_node_t *vl_timer_heap_peek(const vl_timer_heap_t *heap)
{
    return heap != NULL && heap->length != 0 ? heap->items[0] : NULL;
}

vl_timer_node_t *vl_timer_heap_pop(vl_timer_heap_t *heap)
{
    vl_timer_node_t *node;

    if (heap == NULL || heap->length == 0) {
        return NULL;
    }
    node = heap->items[0];
    node->active = 0;
    --heap->length;
    if (heap->length != 0) {
        heap->items[0] = heap->items[heap->length];
        heap->items[0]->heap_index = 0;
        vl_timer_heap_down(heap, 0);
    }
    return node;
}

void vl_timer_heap_remove(vl_timer_heap_t *heap, vl_timer_node_t *node)
{
    size_t index;

    if (heap == NULL || node == NULL || !node->active ||
        node->heap_index >= heap->length) {
        return;
    }
    index = node->heap_index;
    node->active = 0;
    --heap->length;
    if (index != heap->length) {
        heap->items[index] = heap->items[heap->length];
        heap->items[index]->heap_index = index;
        if (index != 0 && heap->items[index]->deadline_ns <
                              heap->items[(index - 1) / 2]->deadline_ns) {
            vl_timer_heap_up(heap, index);
        } else {
            vl_timer_heap_down(heap, index);
        }
    }
}
