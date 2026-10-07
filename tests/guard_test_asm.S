    .intel_syntax noprefix
    .text
    .p2align 4, 0xCC
    .globl test_pushrbx_func
test_pushrbx_func:
    .byte 0x40, 0x53          /* push rbx (REX form, as MSVC emits it) */
    mov rax, rcx
    pop rbx
    ret

    .p2align 4, 0xCC
    .globl test_clobber_and_fault
test_clobber_and_fault:
    mov rbx, 0x1111
    mov rbp, 0x2222
    mov rsi, 0x3333
    mov rdi, 0x4444
    mov r12, 0x5555
    mov r13, 0x6666
    mov r14, 0x7777
    mov r15, 0x8888
    pxor xmm6, xmm6
    pxor xmm7, xmm7
    pxor xmm15, xmm15
    sub rsp, 0x1000           /* leave the stack pointer somewhere else too */
    mov qword ptr [0x20], rax /* fault */
    ret
