#include "memory_internal.h"

#include <stdlib.h>
#include <string.h>

#if defined(VELOCO_MEMORY_DEBUG)
#define VL_DEBUG_CANARY UINT64_C(0xd0c0b0a090807060)
#define VL_DEBUG_POISON ((unsigned char)0xa5)
#endif

void vl_debug_prepare(vl_object_header_t *header, void *user)
{
#if defined(VELOCO_MEMORY_DEBUG)
    unsigned char *canary = (unsigned char *)user + header->requested_size;
    uint64_t value = VL_DEBUG_CANARY;

    header->debug_padding = 0;
    header->prefix_canary = value;
    memcpy(canary, &value, sizeof(value));
#else
    (void)header;
    (void)user;
#endif
}

int vl_debug_validate(vl_object_header_t *header, void *user)
{
#if defined(VELOCO_MEMORY_DEBUG)
    unsigned char *canary;
    uint64_t value;

    if (header->magic != VL_OBJECT_MAGIC ||
        header->prefix_canary != VL_DEBUG_CANARY ||
        header->state != VL_OBJECT_ALLOCATED) {
        vl_debug_abort();
        return 0;
    }
    canary = (unsigned char *)user + header->requested_size;
    memcpy(&value, canary, sizeof(value));
    if (value != VL_DEBUG_CANARY) {
        vl_debug_abort();
        return 0;
    }
#else
    (void)header;
    (void)user;
#endif
    return 1;
}

void vl_debug_abort(void)
{
#if defined(VELOCO_MEMORY_DEBUG)
    abort();
#endif
}

void vl_debug_poison(void *user, size_t capacity)
{
#if defined(VELOCO_MEMORY_DEBUG)
    memset(user, VL_DEBUG_POISON, capacity);
#else
    (void)user;
    (void)capacity;
#endif
}
