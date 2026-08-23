#include "memory_internal.h"

#include <stdint.h>
#include <string.h>

static size_t vl_span_object_stride(size_t capacity)
{
    size_t bytes = sizeof(vl_object_header_t) + capacity;

#if defined(VELOCO_MEMORY_DEBUG)
    bytes += sizeof(uint64_t);
#endif
    return vl_memory_align_up(bytes, VL_MEMORY_ALIGNMENT);
}

static void vl_span_insert_central(vl_span_t *span)
{
    span->central_next = vl_memory_global.central[span->class_index];
    vl_memory_global.central[span->class_index] = span;
}

int vl_span_create(size_t class_index, vl_span_t **out)
{
    vl_span_t *span;
    size_t capacity;
    size_t stride;
    size_t object_area;
    size_t object_pages;
    size_t pages;
    size_t mapping_size;
    size_t index;
    unsigned char *objects;
    void *user;

    if (out == NULL || *out != NULL || class_index >= VL_SIZE_CLASS_COUNT) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    capacity = vl_size_class_capacity(class_index);
    stride = vl_span_object_stride(capacity);
    object_area = 64 * 1024;
    if (stride > object_area / 2) {
        object_area = stride * 2;
    }
    object_pages =
        (object_area + vl_memory_global.page_size - 1) /
        vl_memory_global.page_size;
    if (object_pages == 0 || object_pages == SIZE_MAX) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    pages = object_pages + 1;
    span = vl_page_heap_acquire(pages, &mapping_size);
    if (span == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }

    memset(span, 0, sizeof(*span));
    span->mapping_base = span;
    span->mapping_size = mapping_size;
    span->class_index = class_index;
    span->capacity = capacity;
    span->owner_p = vl_memory_current_p();
    span->object_stride = stride;
    objects = (unsigned char *)span->mapping_base +
              vl_memory_global.page_size;
    span->object_count =
        (mapping_size - vl_memory_global.page_size) / span->object_stride;
    if (span->object_count == 0) {
        vl_page_heap_release(span->mapping_base, span->mapping_size);
        return VL_ERROR_SYSTEM;
    }

    for (index = 0; index < span->object_count; ++index) {
        vl_object_header_t *header =
            (vl_object_header_t *)(objects + index * span->object_stride);

        header->magic = VL_OBJECT_MAGIC;
        header->kind = VL_OBJECT_SMALL;
        header->state = VL_OBJECT_FREE;
        header->requested_size = 0;
        header->capacity = capacity;
        header->owner_p = span->owner_p;
        header->reserved = 0;
        header->span = span;
        header->mapping_base = NULL;
        header->mapping_size = 0;
        header->large_next = NULL;
        user = vl_object_user_from_header(header);
        *(void **)user = span->central_free;
        span->central_free = user;
    }
    span->central_free_count = span->object_count;
    span->free_count = span->object_count;
    span->all_next = vl_memory_global.all_spans;
    vl_memory_global.all_spans = span;
    vl_span_insert_central(span);
    *out = span;
    return VL_OK;
}

void *vl_span_take_central(vl_span_t *span)
{
    void *user;

    if (span == NULL || span->central_free == NULL) {
        return NULL;
    }
    user = span->central_free;
    span->central_free = *(void **)user;
    --span->central_free_count;
    ++span->cached_count;
    return user;
}

void vl_span_move_to_central(vl_span_t *span, void *user)
{
    if (span == NULL || user == NULL) {
        return;
    }
    *(void **)user = span->central_free;
    span->central_free = user;
    ++span->central_free_count;
    if (span->cached_count > 0) {
        --span->cached_count;
    }
}

static void vl_span_remove_from_central(vl_span_t *span)
{
    vl_span_t **cursor = &vl_memory_global.central[span->class_index];

    while (*cursor != NULL && *cursor != span) {
        cursor = &(*cursor)->central_next;
    }
    if (*cursor == span) {
        *cursor = span->central_next;
    }
}

static void vl_span_remove_from_all(vl_span_t *span)
{
    vl_span_t **cursor = &vl_memory_global.all_spans;

    while (*cursor != NULL && *cursor != span) {
        cursor = &(*cursor)->all_next;
    }
    if (*cursor == span) {
        *cursor = span->all_next;
    }
}

int vl_span_reclaim_if_empty(vl_span_t *span)
{
    void *mapping_base;
    size_t mapping_size;

    if (span == NULL || span->active_count != 0 || span->cached_count != 0 ||
        span->central_free_count != span->object_count) {
        return 0;
    }
    mapping_base = span->mapping_base;
    mapping_size = span->mapping_size;
    vl_span_remove_from_central(span);
    vl_span_remove_from_all(span);
    vl_page_heap_release(mapping_base, mapping_size);
    return 1;
}

void vl_span_class_stats(size_t class_index, vl_memory_class_stats_t *out)
{
    vl_span_t *span;

    memset(out, 0, sizeof(*out));
    if (class_index >= VL_SIZE_CLASS_COUNT) {
        return;
    }
    for (span = vl_memory_global.all_spans; span != NULL;
         span = span->all_next) {
        if (span->class_index == class_index) {
            ++out->span_count;
            out->object_count += span->object_count;
            out->active_objects += span->active_count;
            out->free_objects += span->free_count;
        }
    }
}
