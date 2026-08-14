#include "fiber_context.h"

    .text
    .globl vl_context_switch
    .hidden vl_context_switch
    .type vl_context_switch, %function
vl_context_switch:
    stp x19, x20, [x0, #VL_CTX_X19]
    stp x21, x22, [x0, #VL_CTX_X21]
    stp x23, x24, [x0, #VL_CTX_X23]
    stp x25, x26, [x0, #VL_CTX_X25]
    stp x27, x28, [x0, #VL_CTX_X27]
    stp x29, x30, [x0, #VL_CTX_X29]
    mov x2, sp
    str x2, [x0, #VL_CTX_SP]
    stp d8, d9, [x0, #VL_CTX_D8]
    stp d10, d11, [x0, #VL_CTX_D10]
    stp d12, d13, [x0, #VL_CTX_D12]
    stp d14, d15, [x0, #VL_CTX_D14]

    ldp x19, x20, [x1, #VL_CTX_X19]
    ldp x21, x22, [x1, #VL_CTX_X21]
    ldp x23, x24, [x1, #VL_CTX_X23]
    ldp x25, x26, [x1, #VL_CTX_X25]
    ldp x27, x28, [x1, #VL_CTX_X27]
    ldp x29, x30, [x1, #VL_CTX_X29]
    ldr x2, [x1, #VL_CTX_SP]
    mov sp, x2
    ldp d8, d9, [x1, #VL_CTX_D8]
    ldp d10, d11, [x1, #VL_CTX_D10]
    ldp d12, d13, [x1, #VL_CTX_D12]
    ldp d14, d15, [x1, #VL_CTX_D14]
    br x30
    .size vl_context_switch, .-vl_context_switch

    .section .note.GNU-stack,"",%progbits
