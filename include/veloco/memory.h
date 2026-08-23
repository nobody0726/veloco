#ifndef VELOCO_MEMORY_H
#define VELOCO_MEMORY_H

#include <veloco/common.h>

#include <stddef.h>

#define VL_MEMORY_SMALL_MAX ((size_t)32768)
#define VL_ARENA_DEFAULT_BLOCK_SIZE ((size_t)(64 * 1024))

typedef struct vl_allocator_stats {
    size_t active_bytes;
    size_t active_objects;
    size_t cache_hits;
    size_t central_refills;
    size_t mapped_bytes;
    size_t cross_p_frees;
} vl_allocator_stats_t;

typedef struct vl_arena {
    void *impl;
} vl_arena_t;

typedef struct vl_pool {
    void *impl;
} vl_pool_t;

/*
 * vl_malloc/vl_free are safe across Runtime worker threads. Arena and pool
 * handles own their implementation pointers and must not be copied or used
 * concurrently while live. Destroy every arena and pool before allocator
 * shutdown. Memory from an arena or pool remains owned by that handle and
 * must not be passed to vl_free; return pool objects with vl_pool_free.
 */
VL_API int vl_allocator_init(void);
VL_API void vl_allocator_shutdown(void);
VL_API void vl_allocator_get_stats(vl_allocator_stats_t *out);

VL_API void *vl_malloc(size_t size);
VL_API void vl_free(void *ptr);
VL_API void *vl_calloc(size_t count, size_t size);
VL_API void *vl_realloc(void *ptr, size_t size);

VL_API int vl_arena_init(vl_arena_t *arena, size_t block_size);
VL_API void *vl_arena_alloc(vl_arena_t *arena, size_t size, size_t alignment);
VL_API void vl_arena_reset(vl_arena_t *arena);
VL_API void vl_arena_destroy(vl_arena_t *arena);

VL_API int vl_pool_init(vl_pool_t *pool, size_t object_size);
VL_API void *vl_pool_alloc(vl_pool_t *pool);
VL_API int vl_pool_free(vl_pool_t *pool, void *ptr);
VL_API size_t vl_pool_object_size(const vl_pool_t *pool);
VL_API void vl_pool_destroy(vl_pool_t *pool);

#endif
