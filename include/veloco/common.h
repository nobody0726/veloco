#ifndef VELOCO_COMMON_H
#define VELOCO_COMMON_H

#include <stddef.h>
#include <stdint.h>

#define VL_VERSION_MAJOR 0
#define VL_VERSION_MINOR 1
#define VL_VERSION_PATCH 0

typedef int8_t vl_i8;
typedef uint8_t vl_u8;
typedef int16_t vl_i16;
typedef uint16_t vl_u16;
typedef int32_t vl_i32;
typedef uint32_t vl_u32;
typedef int64_t vl_i64;
typedef uint64_t vl_u64;
typedef size_t vl_usize;

typedef enum vl_status {
    VL_OK = 0,
    VL_ERROR_INVALID_ARGUMENT = -1,
    VL_ERROR_OUT_OF_MEMORY = -2,
    VL_ERROR_SYSTEM = -3,
    VL_ERROR_UNSUPPORTED = -4
} vl_status_t;

#if defined(__GNUC__) || defined(__clang__)
#define VL_LIKELY(expr) __builtin_expect(!!(expr), 1)
#define VL_UNLIKELY(expr) __builtin_expect(!!(expr), 0)
#define VL_API __attribute__((visibility("default")))
#define VL_INTERNAL __attribute__((visibility("hidden")))
#else
#define VL_LIKELY(expr) (!!(expr))
#define VL_UNLIKELY(expr) (!!(expr))
#define VL_API
#define VL_INTERNAL
#endif

#endif
