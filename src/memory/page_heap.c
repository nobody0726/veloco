#include "memory_internal.h"

#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

void *vl_page_heap_acquire(size_t pages, size_t *mapped_size)
{
    void *mapping;
    size_t bytes;

    if (!vl_memory_global.initialized || pages == 0 ||
        pages > SIZE_MAX / vl_memory_global.page_size) {
        return NULL;
    }
    bytes = pages * vl_memory_global.page_size;
    if (vl_memory_global.stats.mapped_bytes > SIZE_MAX - bytes) {
        return NULL;
    }
    mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        return NULL;
    }
    vl_memory_global.stats.mapped_bytes += bytes;
    if (mapped_size != NULL) {
        *mapped_size = bytes;
    }
    return mapping;
}

void vl_page_heap_release(void *mapping_base, size_t mapped_size)
{
    if (mapping_base == NULL || mapped_size == 0) {
        return;
    }
    if (munmap(mapping_base, mapped_size) != 0) {
        return;
    }
    if (vl_memory_global.stats.mapped_bytes >= mapped_size) {
        vl_memory_global.stats.mapped_bytes -= mapped_size;
    } else {
        vl_memory_global.stats.mapped_bytes = 0;
    }
}
