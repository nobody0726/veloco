#ifndef VELOCO_FIBER_CONTEXT_H
#define VELOCO_FIBER_CONTEXT_H

#if defined(__x86_64__)

#define VL_CTX_RBX 0
#define VL_CTX_RBP 8
#define VL_CTX_R12 16
#define VL_CTX_R13 24
#define VL_CTX_R14 32
#define VL_CTX_R15 40
#define VL_CTX_RSP 48
#define VL_CTX_RIP 56
#define VL_CTX_SIZE 64

#elif defined(__aarch64__)

#define VL_CTX_X19 0
#define VL_CTX_X21 16
#define VL_CTX_X23 32
#define VL_CTX_X25 48
#define VL_CTX_X27 64
#define VL_CTX_X29 80
#define VL_CTX_SP 96
#define VL_CTX_D8 104
#define VL_CTX_D10 120
#define VL_CTX_D12 136
#define VL_CTX_D14 152
#define VL_CTX_SIZE 168

#else
#error "Veloco fibers support only x86_64 and AArch64"
#endif

#if !defined(__ASSEMBLER__)

#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__)
typedef struct vl_fiber_context {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rsp;
    uint64_t rip;
} vl_fiber_context_t;

_Static_assert(offsetof(vl_fiber_context_t, rbx) == VL_CTX_RBX,
               "rbx offset must match assembly");
_Static_assert(offsetof(vl_fiber_context_t, rsp) == VL_CTX_RSP,
               "rsp offset must match assembly");
_Static_assert(offsetof(vl_fiber_context_t, rip) == VL_CTX_RIP,
               "rip offset must match assembly");
#elif defined(__aarch64__)
typedef struct vl_fiber_context {
    uint64_t x19;
    uint64_t x20;
    uint64_t x21;
    uint64_t x22;
    uint64_t x23;
    uint64_t x24;
    uint64_t x25;
    uint64_t x26;
    uint64_t x27;
    uint64_t x28;
    uint64_t x29;
    uint64_t x30;
    uint64_t sp;
    uint64_t d8;
    uint64_t d9;
    uint64_t d10;
    uint64_t d11;
    uint64_t d12;
    uint64_t d13;
    uint64_t d14;
    uint64_t d15;
} vl_fiber_context_t;

_Static_assert(offsetof(vl_fiber_context_t, x19) == VL_CTX_X19,
               "x19 offset must match assembly");
_Static_assert(offsetof(vl_fiber_context_t, sp) == VL_CTX_SP,
               "sp offset must match assembly");
_Static_assert(offsetof(vl_fiber_context_t, d8) == VL_CTX_D8,
               "d8 offset must match assembly");
_Static_assert(offsetof(vl_fiber_context_t, d14) == VL_CTX_D14,
               "d14 offset must match assembly");
#endif

_Static_assert(sizeof(vl_fiber_context_t) == VL_CTX_SIZE,
               "context size must match assembly");

#endif
#endif
