    .text
    .globl vl_test_callee_saved_registers
    .type vl_test_callee_saved_registers, @function
vl_test_callee_saved_registers:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $24, %rsp
    movq %rdi, 0(%rsp)
    movq %rsi, 8(%rsp)

    movabsq $0x1111111111111111, %rbx
    movabsq $0x2222222222222222, %rbp
    movabsq $0x3333333333333333, %r12
    movabsq $0x4444444444444444, %r13
    movabsq $0x5555555555555555, %r14
    movabsq $0x6666666666666666, %r15
    movq 8(%rsp), %rdi
    call *0(%rsp)

    xorl %eax, %eax
    movabsq $0x1111111111111111, %rcx
    cmpq %rcx, %rbx
    jne .Labi_done
    movabsq $0x2222222222222222, %rcx
    cmpq %rcx, %rbp
    jne .Labi_done
    movabsq $0x3333333333333333, %rcx
    cmpq %rcx, %r12
    jne .Labi_done
    movabsq $0x4444444444444444, %rcx
    cmpq %rcx, %r13
    jne .Labi_done
    movabsq $0x5555555555555555, %rcx
    cmpq %rcx, %r14
    jne .Labi_done
    movabsq $0x6666666666666666, %rcx
    cmpq %rcx, %r15
    jne .Labi_done
    movl $1, %eax

.Labi_done:
    addq $24, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
    .size vl_test_callee_saved_registers, .-vl_test_callee_saved_registers

    .section .note.GNU-stack,"",@progbits
