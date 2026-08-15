#ifndef VELOCO_MEMORY_INTERNAL_H
#define VELOCO_MEMORY_INTERNAL_H

#include <veloco/memory.h>

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define VL_SIZE_CLASS_COUNT ((size_t)42)
#define VL_MEMORY_ALIGNMENT ((size_t)16)
#define VL_CACHE_MAX ((size_t)64)
#define VL_CACHE_REFILL ((size_t)32)

#define VL_OBJECT_MAGIC UINT64_C(0x56454c4f434f4d45)
#define VL_OBJECT_ALLOCATED ((uint32_t)1)
#define VL_OBJECT_FREE ((uint32_t)2)
#define VL_OBJECT_SMALL ((uint32_t)1)
#define VL_OBJECT_LARGE ((uint32_t)2)

typedef struct vl_memory_class_stats {
    size_t span_count;
    size_t object_count;
    size_t active_objects;
    size_t free_objects;
} vl_memory_class_stats_t;

typedef struct vl_span vl_span_t;

typedef struct vl_object_header {
    uint64_t magic;
    uint32_t kind;
    uint32_t state;
    size_t requested_size;
    size_t capacity;
    vl_span_t *span;
    void *mapping_base;
    size_t mapping_size;
    struct vl_object_header *large_next;
#if defined(VELOCO_MEMORY_DEBUG)
    uint64_t debug_padding;
    uint64_t prefix_canary;
#endif
} vl_object_header_t;

_Static_assert(sizeof(vl_object_header_t) % VL_MEMORY_ALIGNMENT == 0,
               "object header must preserve public alignment");

struct vl_span {
    void *mapping_base;
    size_t mapping_size;
    size_t class_index;
    size_t capacity;
    size_t object_stride;
    size_t object_count;
    size_t central_free_count;
    size_t free_count;
    size_t active_count;
    size_t cached_count;
    void *central_free;
    vl_span_t *central_next;
    vl_span_t *all_next;
};

typedef struct vl_cache {
    void *head;
    size_t count;
} vl_cache_t;

typedef struct vl_memory_state {
    int initialized;
    size_t page_size;
    pthread_t owner_thread;
    vl_allocator_stats_t stats;
    vl_cache_t caches[VL_SIZE_CLASS_COUNT];
    vl_span_t *central[VL_SIZE_CLASS_COUNT];
    vl_span_t *all_spans;
    vl_object_header_t *large_allocations;
} vl_memory_state_t;

extern vl_memory_state_t vl_memory_global;

int vl_memory_is_owner(void);
int vl_memory_ensure_initialized(void);
size_t vl_memory_align_up(size_t value, size_t alignment);

void *vl_page_heap_acquire(size_t pages, size_t *mapped_size);
void vl_page_heap_release(void *mapping_base, size_t mapped_size);

int vl_span_create(size_t class_index, vl_span_t **out);
void *vl_span_take_central(vl_span_t *span);
void vl_span_move_to_central(vl_span_t *span, void *user);
int vl_span_reclaim_if_empty(vl_span_t *span);
void vl_span_class_stats(size_t class_index, vl_memory_class_stats_t *out);

static inline vl_object_header_t *vl_object_header_from_user(void *user)
{
    return (vl_object_header_t *)((unsigned char *)user -
                                  sizeof(vl_object_header_t));
}

static inline void *vl_object_user_from_header(vl_object_header_t *header)
{
    return (unsigned char *)header + sizeof(*header);
}

void vl_debug_prepare(vl_object_header_t *header, void *user);
int vl_debug_validate(vl_object_header_t *header, void *user);
void vl_debug_poison(void *user, size_t capacity);
void vl_debug_abort(void);

size_t vl_size_class_index(size_t size);
size_t vl_size_class_capacity(size_t index);
size_t vl_size_class_count(void);
int vl_memory_get_class_stats(size_t index, vl_memory_class_stats_t *out);

#endif
