    .text
    .globl vl_test_callee_saved_registers
    .type vl_test_callee_saved_registers, %function
vl_test_callee_saved_registers:
    sub sp, sp, #176
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    stp x0, x1, [sp, #160]

    mov x19, #19
    mov x20, #20
    mov x21, #21
    mov x22, #22
    mov x23, #23
    mov x24, #24
    mov x25, #25
    mov x26, #26
    mov x27, #27
    mov x28, #28
    mov x29, #29
    mov x9, #108
    fmov d8, x9
    mov x9, #109
    fmov d9, x9
    mov x9, #110
    fmov d10, x9
    mov x9, #111
    fmov d11, x9
    mov x9, #112
    fmov d12, x9
    mov x9, #113
    fmov d13, x9
    mov x9, #114
    fmov d14, x9
    mov x9, #115
    fmov d15, x9

    ldp x16, x0, [sp, #160]
    blr x16

    mov x0, #0
    cmp x19, #19
    b.ne .Labi_done
    cmp x20, #20
    b.ne .Labi_done
    cmp x21, #21
    b.ne .Labi_done
    cmp x22, #22
    b.ne .Labi_done
    cmp x23, #23
    b.ne .Labi_done
    cmp x24, #24
    b.ne .Labi_done
    cmp x25, #25
    b.ne .Labi_done
    cmp x26, #26
    b.ne .Labi_done
    cmp x27, #27
    b.ne .Labi_done
    cmp x28, #28
    b.ne .Labi_done
    cmp x29, #29
    b.ne .Labi_done
    fmov x9, d8
    cmp x9, #108
    b.ne .Labi_done
    fmov x9, d9
    cmp x9, #109
    b.ne .Labi_done
    fmov x9, d10
    cmp x9, #110
    b.ne .Labi_done
    fmov x9, d11
    cmp x9, #111
    b.ne .Labi_done
    fmov x9, d12
    cmp x9, #112
    b.ne .Labi_done
    fmov x9, d13
    cmp x9, #113
    b.ne .Labi_done
    fmov x9, d14
    cmp x9, #114
    b.ne .Labi_done
    fmov x9, d15
    cmp x9, #115
    b.ne .Labi_done
    mov x0, #1

.Labi_done:
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #176
    ret
    .size vl_test_callee_saved_registers, .-vl_test_callee_saved_registers

    .section .note.GNU-stack,"",%progbits
