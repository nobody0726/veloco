#include "memory_internal.h"

static const size_t vl_size_classes[VL_SIZE_CLASS_COUNT] = {
    8,     16,    24,    32,    48,    64,    80,    96,    112,   128,
    160,   192,   224,   256,   320,   384,   448,   512,   640,   768,
    896,   1024,  1280,  1536,  1792,  2048,  2560,  3072,  3584,  4096,
    5120,  6144,  7168,  8192,  10240, 12288, 14336, 16384, 20480, 24576,
    28672, 32768
};

size_t vl_size_class_index(size_t size)
{
    size_t low = 0;
    size_t high = VL_SIZE_CLASS_COUNT;

    while (low < high) {
        size_t middle = low + (high - low) / 2;

        if (vl_size_classes[middle] < size) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

size_t vl_size_class_capacity(size_t index)
{
    return index < VL_SIZE_CLASS_COUNT ? vl_size_classes[index] : 0;
}

size_t vl_size_class_count(void)
{
    return VL_SIZE_CLASS_COUNT;
}
