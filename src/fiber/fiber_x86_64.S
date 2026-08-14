#include "fiber_context.h"

    .text
    .globl vl_context_switch
    .hidden vl_context_switch
    .type vl_context_switch, @function
vl_context_switch:
    movq %rbx, VL_CTX_RBX(%rdi)
    movq %rbp, VL_CTX_RBP(%rdi)
    movq %r12, VL_CTX_R12(%rdi)
    movq %r13, VL_CTX_R13(%rdi)
    movq %r14, VL_CTX_R14(%rdi)
    movq %r15, VL_CTX_R15(%rdi)
    movq %rsp, VL_CTX_RSP(%rdi)
    leaq .Lresume(%rip), %rax
    movq %rax, VL_CTX_RIP(%rdi)

    movq VL_CTX_RBX(%rsi), %rbx
    movq VL_CTX_RBP(%rsi), %rbp
    movq VL_CTX_R12(%rsi), %r12
    movq VL_CTX_R13(%rsi), %r13
    movq VL_CTX_R14(%rsi), %r14
    movq VL_CTX_R15(%rsi), %r15
    movq VL_CTX_RSP(%rsi), %rsp
    jmp *VL_CTX_RIP(%rsi)

.Lresume:
    ret
    .size vl_context_switch, .-vl_context_switch

    .section .note.GNU-stack,"",@progbits
