#include "memory_internal.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

vl_memory_state_t vl_memory_global;

size_t vl_memory_align_up(size_t value, size_t alignment)
{
    size_t remainder;

    if (alignment == 0) {
        return 0;
    }
    remainder = value & (alignment - 1);
    if (remainder == 0) {
        return value;
    }
    if (value > SIZE_MAX - (alignment - remainder)) {
        return 0;
    }
    return value + alignment - remainder;
}

int vl_memory_is_owner(void)
{
    return vl_memory_global.initialized &&
           pthread_equal(vl_memory_global.owner_thread, pthread_self());
}

int vl_memory_ensure_initialized(void)
{
    if (vl_memory_global.initialized) {
        return vl_memory_is_owner() ? VL_OK : VL_ERROR_INVALID_STATE;
    }
    return vl_allocator_init();
}

int vl_allocator_init(void)
{
    long page_size;

    if (vl_memory_global.initialized) {
        return vl_memory_is_owner() ? VL_OK : VL_ERROR_INVALID_STATE;
    }
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || ((size_t)page_size & ((size_t)page_size - 1)) != 0) {
        return VL_ERROR_SYSTEM;
    }
    memset(&vl_memory_global, 0, sizeof(vl_memory_global));
    vl_memory_global.initialized = 1;
    vl_memory_global.page_size = (size_t)page_size;
    vl_memory_global.owner_thread = pthread_self();
    return VL_OK;
}

void vl_allocator_shutdown(void)
{
    vl_span_t *span;
    vl_span_t *next_span;
    vl_object_header_t *large;
    vl_object_header_t *next_large;

    if (!vl_memory_is_owner()) {
        return;
    }
    for (span = vl_memory_global.all_spans; span != NULL; span = next_span) {
        next_span = span->all_next;
        vl_page_heap_release(span->mapping_base, span->mapping_size);
    }
    for (large = vl_memory_global.large_allocations; large != NULL;
         large = next_large) {
        next_large = large->large_next;
        vl_page_heap_release(large->mapping_base, large->mapping_size);
    }
    memset(&vl_memory_global, 0, sizeof(vl_memory_global));
}

static int vl_stats_can_allocate(size_t size)
{
    return vl_memory_global.stats.active_bytes <= SIZE_MAX - size &&
           vl_memory_global.stats.active_objects != SIZE_MAX;
}

void vl_allocator_get_stats(vl_allocator_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    if (!vl_memory_global.initialized) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = vl_memory_global.stats;
}

static vl_span_t *vl_find_refill_span(size_t class_index)
{
    vl_span_t *span;

    for (span = vl_memory_global.central[class_index]; span != NULL;
         span = span->central_next) {
        if (span->central_free != NULL) {
            return span;
        }
    }
    return NULL;
}

static int vl_refill_cache(size_t class_index)
{
    vl_cache_t *cache = &vl_memory_global.caches[class_index];
    vl_span_t *span = vl_find_refill_span(class_index);
    size_t count = 0;

    if (span == NULL && vl_span_create(class_index, &span) != VL_OK) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    while (count < VL_CACHE_REFILL && span->central_free != NULL) {
        void *user = vl_span_take_central(span);

        *(void **)user = cache->head;
        cache->head = user;
        ++cache->count;
        ++count;
    }
    if (count != 0) {
        ++vl_memory_global.stats.central_refills;
    }
    return count != 0 ? VL_OK : VL_ERROR_OUT_OF_MEMORY;
}

static void *vl_cache_pop(size_t class_index)
{
    vl_cache_t *cache = &vl_memory_global.caches[class_index];
    void *user;
    vl_object_header_t *header;

    if (cache->head == NULL && vl_refill_cache(class_index) != VL_OK) {
        return NULL;
    }
    user = cache->head;
    cache->head = *(void **)user;
    --cache->count;
    header = vl_object_header_from_user(user);
    --header->span->cached_count;
    --header->span->free_count;
    ++header->span->active_count;
    return user;
}

static void vl_drain_cache(size_t class_index)
{
    vl_cache_t *cache = &vl_memory_global.caches[class_index];
    size_t count = 0;

    while (count < VL_CACHE_REFILL && cache->head != NULL) {
        void *user = cache->head;
        vl_object_header_t *header = vl_object_header_from_user(user);
        vl_span_t *span = header->span;

        cache->head = *(void **)user;
        --cache->count;
        vl_span_move_to_central(span, user);
        ++count;
        (void)vl_span_reclaim_if_empty(span);
    }
}

static void vl_cache_push(vl_object_header_t *header, void *user)
{
    vl_cache_t *cache = &vl_memory_global.caches[header->span->class_index];

    vl_debug_poison(user, header->capacity);
    *(void **)user = cache->head;
    cache->head = user;
    ++cache->count;
    ++header->span->cached_count;
    ++header->span->free_count;
    --header->span->active_count;
    if (cache->count > VL_CACHE_MAX) {
        vl_drain_cache(header->span->class_index);
    }
}

static void vl_initialize_header(vl_object_header_t *header, uint32_t kind,
                                 size_t capacity, size_t requested,
                                 vl_span_t *span, void *mapping_base,
                                 size_t mapping_size)
{
    header->magic = VL_OBJECT_MAGIC;
    header->kind = kind;
    header->state = VL_OBJECT_ALLOCATED;
    header->requested_size = requested;
    header->capacity = capacity;
    header->span = span;
    header->mapping_base = mapping_base;
    header->mapping_size = mapping_size;
    header->large_next = NULL;
}

static void vl_large_insert(vl_object_header_t *header)
{
    header->large_next = vl_memory_global.large_allocations;
    vl_memory_global.large_allocations = header;
}

static void vl_large_remove(vl_object_header_t *header)
{
    vl_object_header_t **cursor = &vl_memory_global.large_allocations;

    while (*cursor != NULL && *cursor != header) {
        cursor = &(*cursor)->large_next;
    }
    if (*cursor == header) {
        *cursor = header->large_next;
    }
}

static void *vl_malloc_large(size_t size)
{
    vl_object_header_t *header;
    size_t bytes;
    size_t pages;
    size_t mapping_size;
    void *mapping;
    size_t suffix = 0;

#if defined(VELOCO_MEMORY_DEBUG)
    suffix = sizeof(uint64_t);
#endif
    if (size > SIZE_MAX - sizeof(*header) - suffix) {
        return NULL;
    }
    bytes = sizeof(*header) + size + suffix;
    if (bytes > SIZE_MAX - (vl_memory_global.page_size - 1)) {
        return NULL;
    }
    pages = (bytes + vl_memory_global.page_size - 1) /
            vl_memory_global.page_size;
    if (pages == 0) {
        return NULL;
    }
    mapping = vl_page_heap_acquire(pages, &mapping_size);
    if (mapping == NULL) {
        return NULL;
    }
    header = mapping;
    vl_initialize_header(header, VL_OBJECT_LARGE, size, size, NULL, mapping,
                         mapping_size);
    vl_large_insert(header);
    vl_memory_global.stats.active_bytes += size;
    ++vl_memory_global.stats.active_objects;
    vl_debug_prepare(header, vl_object_user_from_header(header));
    return vl_object_user_from_header(header);
}

void *vl_malloc(size_t size)
{
    size_t class_index;
    size_t capacity;
    void *user;
    vl_object_header_t *header;

    if (size == 0) {
        size = 1;
    }
    if (vl_memory_ensure_initialized() != VL_OK) {
        return NULL;
    }
    if (!vl_stats_can_allocate(size)) {
        return NULL;
    }
    class_index = vl_size_class_index(size);
    if (class_index >= VL_SIZE_CLASS_COUNT) {
        return vl_malloc_large(size);
    }
    if (vl_memory_global.caches[class_index].head != NULL) {
        ++vl_memory_global.stats.cache_hits;
    }
    user = vl_cache_pop(class_index);
    if (user == NULL) {
        return NULL;
    }
    header = vl_object_header_from_user(user);
    capacity = vl_size_class_capacity(class_index);
    vl_initialize_header(header, VL_OBJECT_SMALL, capacity, size,
                         header->span, NULL, 0);
    vl_memory_global.stats.active_bytes += size;
    ++vl_memory_global.stats.active_objects;
    vl_debug_prepare(header, user);
    return user;
}

void vl_free(void *ptr)
{
    vl_object_header_t *header;
    size_t requested;

    if (ptr == NULL) {
        return;
    }
    if (!vl_memory_is_owner()) {
        return;
    }
    header = vl_object_header_from_user(ptr);
    if (header->magic != VL_OBJECT_MAGIC ||
        header->state != VL_OBJECT_ALLOCATED) {
        vl_debug_abort();
        return;
    }
    if (!vl_debug_validate(header, ptr)) {
        return;
    }
    if (vl_memory_global.stats.active_objects == 0 ||
        vl_memory_global.stats.active_bytes < header->requested_size) {
        vl_debug_abort();
        return;
    }
    requested = header->requested_size;
    header->state = VL_OBJECT_FREE;
    vl_memory_global.stats.active_bytes -= requested;
    --vl_memory_global.stats.active_objects;
    if (header->kind == VL_OBJECT_LARGE) {
        vl_large_remove(header);
        vl_page_heap_release(header->mapping_base, header->mapping_size);
        return;
    }
    vl_cache_push(header, ptr);
}

void *vl_calloc(size_t count, size_t size)
{
    void *ptr;

    if (count != 0 && size > SIZE_MAX / count) {
        return NULL;
    }
    ptr = vl_malloc(count * size);
    if (ptr != NULL) {
        memset(ptr, 0, count * size);
    }
    return ptr;
}

void *vl_realloc(void *ptr, size_t size)
{
    vl_object_header_t *header;
    void *replacement;
    size_t copy_size;

    if (ptr == NULL) {
        return vl_malloc(size);
    }
    if (size == 0) {
        vl_free(ptr);
        return NULL;
    }
    if (!vl_memory_is_owner()) {
        return NULL;
    }
    header = vl_object_header_from_user(ptr);
    if (header->magic != VL_OBJECT_MAGIC ||
        header->state != VL_OBJECT_ALLOCATED || !vl_debug_validate(header, ptr)) {
        vl_debug_abort();
        return NULL;
    }
    if (size <= header->capacity) {
        size_t retained_bytes;

        if (vl_memory_global.stats.active_bytes < header->requested_size) {
            vl_debug_abort();
            return NULL;
        }
        retained_bytes = vl_memory_global.stats.active_bytes -
                         header->requested_size;
        if (retained_bytes > SIZE_MAX - size) {
            return NULL;
        }
        header->requested_size = size;
        vl_memory_global.stats.active_bytes = retained_bytes + size;
        vl_debug_prepare(header, ptr);
        return ptr;
    }
    replacement = vl_malloc(size);
    if (replacement == NULL) {
        return NULL;
    }
    copy_size = header->requested_size < size ? header->requested_size : size;
    memcpy(replacement, ptr, copy_size);
    vl_free(ptr);
    return replacement;
}

int vl_memory_get_class_stats(size_t index, vl_memory_class_stats_t *out)
{
    if (out == NULL || index >= VL_SIZE_CLASS_COUNT) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (vl_memory_ensure_initialized() != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    vl_span_class_stats(index, out);
    return VL_OK;
}
