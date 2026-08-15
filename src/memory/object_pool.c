#include "memory_internal.h"

#include <stdlib.h>

typedef struct vl_pool_item {
    void *object;
    int active;
    struct vl_pool_item *next;
} vl_pool_item_t;

typedef struct vl_pool_impl {
    size_t object_size;
    vl_pool_item_t *items;
} vl_pool_impl_t;

int vl_pool_init(vl_pool_t *pool, size_t object_size)
{
    vl_pool_impl_t *impl;

    if (pool == NULL || pool->impl != NULL || object_size == 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (vl_memory_ensure_initialized() != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    impl = calloc(1, sizeof(*impl));
    if (impl == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    impl->object_size = object_size;
    pool->impl = impl;
    return VL_OK;
}

void *vl_pool_alloc(vl_pool_t *pool)
{
    vl_pool_impl_t *impl;
    vl_pool_item_t *item;

    if (pool == NULL) {
        return NULL;
    }
    impl = pool->impl;
    if (impl == NULL || vl_memory_ensure_initialized() != VL_OK) {
        return NULL;
    }
    for (item = impl->items; item != NULL; item = item->next) {
        if (!item->active) {
            item->active = 1;
            return item->object;
        }
    }
    item = calloc(1, sizeof(*item));
    if (item == NULL) {
        return NULL;
    }
    item->object = vl_malloc(impl->object_size);
    if (item->object == NULL) {
        free(item);
        return NULL;
    }
    item->active = 1;
    item->next = impl->items;
    impl->items = item;
    return item->object;
}

int vl_pool_free(vl_pool_t *pool, void *ptr)
{
    vl_pool_impl_t *impl;
    vl_pool_item_t *item;

    if (pool == NULL || ptr == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (!vl_memory_is_owner()) {
        return VL_ERROR_INVALID_STATE;
    }
    impl = pool->impl;
    if (impl == NULL) {
        return VL_ERROR_INVALID_STATE;
    }
    for (item = impl->items; item != NULL; item = item->next) {
        if (item->object == ptr) {
            if (!item->active) {
                vl_debug_abort();
                return VL_ERROR_INVALID_STATE;
            }
            item->active = 0;
            return VL_OK;
        }
    }
    return VL_ERROR_INVALID_ARGUMENT;
}

size_t vl_pool_object_size(const vl_pool_t *pool)
{
    const vl_pool_impl_t *impl = pool != NULL ? pool->impl : NULL;

    return impl != NULL ? impl->object_size : 0;
}

void vl_pool_destroy(vl_pool_t *pool)
{
    vl_pool_impl_t *impl;
    vl_pool_item_t *item;
    vl_pool_item_t *next;

    if (pool == NULL || !vl_memory_is_owner()) {
        return;
    }
    impl = pool->impl;
    if (impl == NULL) {
        return;
    }
    for (item = impl->items; item != NULL; item = next) {
        next = item->next;
        vl_free(item->object);
        free(item);
    }
    free(impl);
    pool->impl = NULL;
}
