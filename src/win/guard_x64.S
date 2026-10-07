/*
 * int sm2m_guarded_call(void (*fn)(void*), void* arg, GuardBuf* buf)   (Windows x64)
 *
 * Saves every callee-saved register (rbx rbp rsi rdi r12-r15 xmm6-xmm15) in
 * its own frame, stores the frame's rsp and a resume address in buf, then
 * calls fn(arg) and returns 0. If fn faults, guard.cpp's vectored exception
 * handler sets rsp = buf->rsp and rip = buf->resume, and the normal epilogue
 * restores the caller's registers and returns 1 - even if fn left the stack or
 * registers in a mess. Full SEH unwind info so the OS unwinder can walk
 * through this frame like any other.
 */
    .intel_syntax noprefix
    .text
    .p2align 4
    .globl sm2m_guarded_call
    .def sm2m_guarded_call
    .scl 2
    .type 32
    .endef
    .seh_proc sm2m_guarded_call
sm2m_guarded_call:
    push rbp
    .seh_pushreg rbp
    push rbx
    .seh_pushreg rbx
    push rsi
    .seh_pushreg rsi
    push rdi
    .seh_pushreg rdi
    push r12
    .seh_pushreg r12
    push r13
    .seh_pushreg r13
    push r14
    .seh_pushreg r14
    push r15
    .seh_pushreg r15
    /* entry rsp = 8 mod 16; 8 pushes keep it 8 mod 16; 0xC8 makes it 16-aligned.
       [rsp+0x00,0x20) shadow space, [rsp+0x20,0xC0) xmm6-xmm15, 8 bytes pad. */
    sub rsp, 0xC8
    .seh_stackalloc 0xC8
    movdqa xmmword ptr [rsp+0x20], xmm6
    .seh_savexmm xmm6, 0x20
    movdqa xmmword ptr [rsp+0x30], xmm7
    .seh_savexmm xmm7, 0x30
    movdqa xmmword ptr [rsp+0x40], xmm8
    .seh_savexmm xmm8, 0x40
    movdqa xmmword ptr [rsp+0x50], xmm9
    .seh_savexmm xmm9, 0x50
    movdqa xmmword ptr [rsp+0x60], xmm10
    .seh_savexmm xmm10, 0x60
    movdqa xmmword ptr [rsp+0x70], xmm11
    .seh_savexmm xmm11, 0x70
    movdqa xmmword ptr [rsp+0x80], xmm12
    .seh_savexmm xmm12, 0x80
    movdqa xmmword ptr [rsp+0x90], xmm13
    .seh_savexmm xmm13, 0x90
    movdqa xmmword ptr [rsp+0xA0], xmm14
    .seh_savexmm xmm14, 0xA0
    movdqa xmmword ptr [rsp+0xB0], xmm15
    .seh_savexmm xmm15, 0xB0
    .seh_endprologue

    /* Zero the callee's shadow space: if fn returns with an unbalanced stack
       (e.g. it was entered a few bytes past a push), its ret lands on 0 and
       faults cleanly instead of jumping somewhere plausible. */
    xor eax, eax
    mov qword ptr [rsp], rax
    mov qword ptr [rsp+8], rax
    mov qword ptr [rsp+16], rax
    mov qword ptr [rsp+24], rax
    mov qword ptr [r8], rsp
    lea rax, [rip + .Lsm2m_resume]
    mov qword ptr [r8 + 8], rax
    mov rax, rcx
    mov rcx, rdx
    call rax
    xor eax, eax
    jmp .Lsm2m_done

.Lsm2m_resume:
    cld
    mov eax, 1

.Lsm2m_done:
    movdqa xmm6, xmmword ptr [rsp+0x20]
    movdqa xmm7, xmmword ptr [rsp+0x30]
    movdqa xmm8, xmmword ptr [rsp+0x40]
    movdqa xmm9, xmmword ptr [rsp+0x50]
    movdqa xmm10, xmmword ptr [rsp+0x60]
    movdqa xmm11, xmmword ptr [rsp+0x70]
    movdqa xmm12, xmmword ptr [rsp+0x80]
    movdqa xmm13, xmmword ptr [rsp+0x90]
    movdqa xmm14, xmmword ptr [rsp+0xA0]
    movdqa xmm15, xmmword ptr [rsp+0xB0]
    add rsp, 0xC8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    pop rbp
    ret
    .seh_endproc
