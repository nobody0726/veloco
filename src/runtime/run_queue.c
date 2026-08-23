#include "run_queue.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int vl_is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

int vl_run_queue_init(vl_run_queue_t *queue, size_t capacity)
{
    if (queue == NULL || queue->slots != NULL || capacity < 2 ||
        !vl_is_power_of_two(capacity) ||
        capacity > SIZE_MAX / sizeof(*queue->slots)) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    queue->slots = calloc(capacity, sizeof(*queue->slots));
    if (queue->slots == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    queue->capacity = capacity;
    queue->mask = capacity - 1;
    atomic_init(&queue->top, 0);
    atomic_init(&queue->bottom, 0);
    return VL_OK;
}

void vl_run_queue_destroy(vl_run_queue_t *queue)
{
    if (queue == NULL) {
        return;
    }
    free(queue->slots);
    memset(queue, 0, sizeof(*queue));
}

int vl_run_queue_owner_push(vl_run_queue_t *queue, void *item)
{
    size_t bottom;
    size_t top;

    if (queue == NULL || queue->slots == NULL || item == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    bottom = atomic_load_explicit(&queue->bottom, memory_order_relaxed);
    top = atomic_load_explicit(&queue->top, memory_order_acquire);
    if (bottom - top >= queue->capacity) {
        return VL_ERROR_WOULD_BLOCK;
    }
    atomic_store_explicit(&queue->slots[bottom & queue->mask], item,
                          memory_order_relaxed);
    /* Publish the slot contents before thieves observe the new bottom. */
    atomic_store_explicit(&queue->bottom, bottom + 1, memory_order_release);
    return VL_OK;
}

void *vl_run_queue_owner_pop(vl_run_queue_t *queue)
{
    size_t bottom;
    size_t top;
    void *item;

    if (queue == NULL || queue->slots == NULL) {
        return NULL;
    }
    bottom = atomic_load_explicit(&queue->bottom, memory_order_relaxed);
    if (bottom == 0) {
        return NULL;
    }
    --bottom;
    atomic_store_explicit(&queue->bottom, bottom, memory_order_relaxed);
    /* Order the speculative decrement against a thief's top CAS. */
    atomic_thread_fence(memory_order_seq_cst);
    top = atomic_load_explicit(&queue->top, memory_order_relaxed);
    if (top > bottom) {
        atomic_store_explicit(&queue->bottom, top, memory_order_relaxed);
        return NULL;
    }
    item = atomic_load_explicit(&queue->slots[bottom & queue->mask],
                                memory_order_relaxed);
    if (top == bottom) {
        size_t expected = top;

        if (!atomic_compare_exchange_strong_explicit(
                &queue->top, &expected, top + 1, memory_order_seq_cst,
                memory_order_relaxed)) {
            item = NULL;
        }
        atomic_store_explicit(&queue->bottom, top + 1,
                              memory_order_relaxed);
    }
    return item;
}

void *vl_run_queue_steal(vl_run_queue_t *queue)
{
    size_t top;
    size_t bottom;
    size_t expected;
    void *item;

    if (queue == NULL || queue->slots == NULL) {
        return NULL;
    }
    top = atomic_load_explicit(&queue->top, memory_order_acquire);
    /* Pair with owner pop's fence for the one-item race. */
    atomic_thread_fence(memory_order_seq_cst);
    bottom = atomic_load_explicit(&queue->bottom, memory_order_acquire);
    if (top >= bottom) {
        return NULL;
    }
    item = atomic_load_explicit(&queue->slots[top & queue->mask],
                                memory_order_relaxed);
    expected = top;
    if (!atomic_compare_exchange_strong_explicit(
            &queue->top, &expected, top + 1, memory_order_seq_cst,
            memory_order_relaxed)) {
        return NULL;
    }
    return item;
}

size_t vl_run_queue_steal_batch(vl_run_queue_t *queue, void **items,
                                size_t maximum)
{
    size_t count = 0;

    if (queue == NULL || items == NULL) {
        return 0;
    }
    while (count < maximum) {
        void *item = vl_run_queue_steal(queue);

        if (item == NULL) {
            break;
        }
        items[count++] = item;
    }
    return count;
}

size_t vl_run_queue_length(const vl_run_queue_t *queue)
{
    size_t top;
    size_t bottom;

    if (queue == NULL || queue->slots == NULL) {
        return 0;
    }
    top = atomic_load_explicit(&queue->top, memory_order_acquire);
    bottom = atomic_load_explicit(&queue->bottom, memory_order_acquire);
    return bottom > top ? bottom - top : 0;
}
