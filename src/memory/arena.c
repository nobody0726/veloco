#include "memory_internal.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct vl_arena_block {
    size_t mapping_size;
    size_t capacity;
    size_t used;
    struct vl_arena_block *next;
} vl_arena_block_t;

typedef struct vl_arena_impl {
    size_t block_size;
    vl_arena_block_t *blocks;
} vl_arena_impl_t;

static void *vl_arena_block_data(vl_arena_block_t *block)
{
    return (unsigned char *)block + vl_memory_global.page_size;
}

static vl_arena_block_t *vl_arena_new_block(vl_arena_impl_t *impl,
                                             size_t minimum_capacity)
{
    vl_arena_block_t *block;
    size_t capacity = impl->block_size > minimum_capacity
                          ? impl->block_size
                          : minimum_capacity;
    size_t object_pages;
    size_t pages;
    size_t mapping_size;

    if (capacity > SIZE_MAX - (vl_memory_global.page_size - 1)) {
        return NULL;
    }
    object_pages =
        (capacity + vl_memory_global.page_size - 1) /
        vl_memory_global.page_size;
    if (object_pages == 0 || object_pages == SIZE_MAX) {
        return NULL;
    }
    pages = object_pages + 1;
    block = vl_page_heap_acquire(pages, &mapping_size);
    if (block == NULL) {
        return NULL;
    }
    block->mapping_size = mapping_size;
    block->capacity = block->mapping_size - vl_memory_global.page_size;
    block->used = 0;
    block->next = impl->blocks;
    impl->blocks = block;
    return block;
}

static void vl_arena_release_head_block(vl_arena_impl_t *impl,
                                        vl_arena_block_t *block)
{
    if (impl->blocks == block) {
        impl->blocks = block->next;
    }
    vl_page_heap_release(block, block->mapping_size);
}

int vl_arena_init(vl_arena_t *arena, size_t block_size)
{
    vl_arena_impl_t *impl;

    if (arena == NULL || arena->impl != NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (vl_memory_ensure_initialized() != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    if (block_size == 0) {
        block_size = VL_ARENA_DEFAULT_BLOCK_SIZE;
    }
    if (block_size > SIZE_MAX - vl_memory_global.page_size) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    impl = calloc(1, sizeof(*impl));
    if (impl == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    impl->block_size = block_size;
    arena->impl = impl;
    return VL_OK;
}

void *vl_arena_alloc(vl_arena_t *arena, size_t size, size_t alignment)
{
    vl_arena_impl_t *impl;
    vl_arena_block_t *block;
    uintptr_t base;
    uintptr_t current;
    size_t offset;
    int new_block = 0;

    if (arena == NULL || size == 0 || alignment == 0 ||
        (alignment & (alignment - 1)) != 0) {
        return NULL;
    }
    impl = arena->impl;
    if (impl == NULL || vl_memory_ensure_initialized() != VL_OK) {
        return NULL;
    }
    block = impl->blocks;
    if (block == NULL) {
        if (size > SIZE_MAX - alignment) {
            return NULL;
        }
        block = vl_arena_new_block(impl, size + alignment);
        if (block == NULL) {
            return NULL;
        }
        new_block = 1;
    }
    base = (uintptr_t)vl_arena_block_data(block);
    if (block->used > UINTPTR_MAX - base) {
        if (new_block) {
            vl_arena_release_head_block(impl, block);
        }
        return NULL;
    }
    current = base + block->used;
    if (current > UINTPTR_MAX - (alignment - 1)) {
        if (new_block) {
            vl_arena_release_head_block(impl, block);
        }
        return NULL;
    }
    offset = (size_t)((current + alignment - 1) &
                      ~(uintptr_t)(alignment - 1));
    offset -= (size_t)base;
    if (offset > block->capacity || size > block->capacity - offset) {
        if (size > SIZE_MAX - alignment) {
            return NULL;
        }
        block = vl_arena_new_block(impl, size + alignment);
        if (block == NULL) {
            return NULL;
        }
        base = (uintptr_t)vl_arena_block_data(block);
        if (base > UINTPTR_MAX - (alignment - 1)) {
            vl_arena_release_head_block(impl, block);
            return NULL;
        }
        offset = (size_t)((base + alignment - 1) &
                          ~(uintptr_t)(alignment - 1));
        offset -= (size_t)base;
        if (offset > block->capacity || size > block->capacity - offset) {
            vl_arena_release_head_block(impl, block);
            return NULL;
        }
    }
    block->used = offset + size;
    return (unsigned char *)vl_arena_block_data(block) + offset;
}

void vl_arena_reset(vl_arena_t *arena)
{
    vl_arena_impl_t *impl;
    vl_arena_block_t *block;
    vl_arena_block_t *next;

    if (arena == NULL || !vl_memory_is_owner()) {
        return;
    }
    impl = arena->impl;
    if (impl == NULL) {
        return;
    }
    for (block = impl->blocks; block != NULL; block = next) {
        next = block->next;
        vl_page_heap_release(block, block->mapping_size);
    }
    impl->blocks = NULL;
}

void vl_arena_destroy(vl_arena_t *arena)
{
    vl_arena_impl_t *impl;

    if (arena == NULL || !vl_memory_is_owner()) {
        return;
    }
    impl = arena->impl;
    if (impl == NULL) {
        return;
    }
    vl_arena_reset(arena);
    free(impl);
    arena->impl = NULL;
}
