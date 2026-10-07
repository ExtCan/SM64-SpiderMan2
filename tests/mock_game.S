/*
 * Mock "game" code for the smoke test. These functions reproduce the exact
 * byte patterns from SM2ScriptTemplate (see bindings.ini) so the mod's scanner
 * finds them in this host's .text section, and their rip-relative operands
 * point at fake engine globals defined in smoke_host.cpp.
 */
    .intel_syntax noprefix
    .text

    /* HeroSystem constructor: 48 89 05 ** ** ** ** 89 35 ?? ?? ?? ?? 48 89 35 */
    .p2align 4, 0xCC
    .globl fake_hero_ctor
fake_hero_ctor:
    .byte 0x48, 0x89, 0x05
    .long g_heroSystem - (. + 4)
    .byte 0x89, 0x35
    .long g_scratch - (. + 4)
    .byte 0x48, 0x89, 0x35
    .long g_scratch - (. + 4)
    ret

    /* Actor* GetActor(ActorHandle*) with the template's exact encoding. */
    .p2align 4, 0xCC
    .globl fake_get_actor
fake_get_actor:
    .byte 0x44, 0x8B, 0x01              /* mov r8d, [rcx]         */
    .byte 0x41, 0x8B, 0xD0              /* mov edx, r8d           */
    .byte 0xC1, 0xEA, 0x14              /* shr edx, 20            */
    .byte 0x81, 0xE2, 0xFF, 0x07, 0x00, 0x00 /* and edx, 0x7FF    */
    .byte 0x74
    .byte ga_null - (. + 1)             /* je null                */
    .byte 0x41, 0x81, 0xE0, 0xFF, 0xFF, 0x0F, 0x00 /* and r8d, 0xFFFFF */
    .byte 0x4B, 0x8D, 0x0C, 0x40        /* lea rcx, [r8+r8*2]     */
    .byte 0x48, 0xC1, 0xE1, 0x06        /* shl rcx, 6             */
    .byte 0x48, 0x03, 0x0D              /* add rcx, [rip+array]   */
    .long g_actorArray - (. + 4)
    .byte 0x44, 0x3B, 0x05              /* cmp r8d, [rip+count]   */
    .long g_actorCount - (. + 4)
    .byte 0x73
    .byte ga_null - (. + 1)             /* jae null               */
    .byte 0x0F, 0xB7, 0x41, 0x08        /* movzx eax, word [rcx+8] (the slot's serial) */
    .byte 0x3B, 0xD0                    /* cmp edx, eax           */
    .byte 0x74
    .byte ga_done - (. + 1)             /* je done                */
ga_null:
    .byte 0x33, 0xC9                    /* xor ecx, ecx           */
ga_done:
    .byte 0x48, 0x8B, 0xC1              /* mov rax, rcx           */
    ret

    /* Transform::SetPosition(Transform*, Vector3*). The template's pattern is
       "0F 10 51 ?? 4C 8B C1". Variants (see tests/run_smoke.sh):
         default          : like the user's game build - the function now starts
                            with "40 53" (push rbx), so the pattern matches 2
                            bytes in; the mod must recover the real start.
         MOCK_BAD_SETPOS  : the pattern matches mid-function after non-prologue
                            bytes; the mod must refuse to call it.
         MOCK_CRASH_SETPOS: the function itself faults; the crash guard must
                            catch it and switch to direct writes. */
    .p2align 4, 0xCC
    .globl fake_set_position
fake_set_position:
#if defined(MOCK_BAD_SETPOS)
    .byte 0x8B, 0xC3                    /* mov eax, ebx (not a prologue) */
    .byte 0x0F, 0x10, 0x51, 0x30        /* movups xmm2, [rcx+30h] */
    .byte 0x4C, 0x8B, 0xC1              /* mov r8, rcx */
    ret
#elif defined(MOCK_CRASH_SETPOS)
    .byte 0x0F, 0x10, 0x51, 0x30
    .byte 0x4C, 0x8B, 0xC1
    xor eax, eax
    mov dword ptr [rax], eax            /* fault: write to null */
    ret
#else
    .byte 0x40, 0x53                    /* push rbx (REX form, as MSVC emits it) */
    .byte 0x0F, 0x10, 0x51, 0x30        /* movups xmm2, [rcx+30h] */
    .byte 0x4C, 0x8B, 0xC1              /* mov r8, rcx */
    mov eax, dword ptr [rdx]
    mov dword ptr [r8 + 0x30], eax
    mov eax, dword ptr [rdx + 4]
    mov dword ptr [r8 + 0x34], eax
    mov eax, dword ptr [rdx + 8]
    mov dword ptr [r8 + 0x38], eax
    pop rbx
    ret
#endif

    /* Transform::SetPosition with the byte pattern of the current game build
       (bindings.ini [TransformSetPosition]); writes the position unless the
       byte at +0x5F is set, like the real one. */
    .p2align 4, 0xCC
    .globl fake_transform_set_position
fake_transform_set_position:
    .byte 0x48, 0x89, 0x5C, 0x24, 0x08          /* mov [rsp+8], rbx          */
    .byte 0x57                                  /* push rdi                  */
    .byte 0x48, 0x83, 0xEC, 0x30                /* sub rsp, 0x30             */
    .byte 0x80, 0x79, 0x5F, 0x00                /* cmp byte [rcx+0x5F], 0    */
    .byte 0x48, 0x8B, 0xFA                      /* mov rdi, rdx              */
    .byte 0xC5, 0xF8, 0x29, 0x74, 0x24, 0x20    /* vmovaps [rsp+0x20], xmm6  */
    .byte 0x48, 0x8B, 0xD9                      /* mov rbx, rcx              */
    .byte 0xC5, 0xF8, 0x10, 0x71, 0x30          /* vmovups xmm6, [rcx+0x30]  */
    .byte 0x75
    .byte tsp_skip - (. + 1)                    /* jnz skip                  */
    .byte 0x45, 0x33, 0xC0                      /* xor r8d, r8d              */
    .byte 0x48, 0x8B, 0xD1                      /* mov rdx, rcx              */
    .byte 0xE8
    .long tsp_stub - (. + 4)                    /* call stub                 */
    .byte 0x8B, 0x07                            /* mov eax, [rdi]            */
    .byte 0x89, 0x43, 0x30                      /* mov [rbx+0x30], eax       */
    .byte 0x8B, 0x47, 0x04                      /* mov eax, [rdi+4]          */
    .byte 0x89, 0x43, 0x34                      /* mov [rbx+0x34], eax       */
    .byte 0x8B, 0x47, 0x08                      /* mov eax, [rdi+8]          */
    .byte 0x89, 0x43, 0x38                      /* mov [rbx+0x38], eax       */
tsp_skip:
    .byte 0xC5, 0xF8, 0x28, 0x74, 0x24, 0x20    /* vmovaps xmm6, [rsp+0x20]  */
    .byte 0x48, 0x8B, 0x5C, 0x24, 0x40          /* mov rbx, [rsp+0x40]       */
    .byte 0x48, 0x83, 0xC4, 0x30                /* add rsp, 0x30             */
    .byte 0x5F                                  /* pop rdi                   */
    .byte 0xC3                                  /* ret                       */
tsp_stub:
    .byte 0xC3

    /* Transform::Hide / Unhide ([TransformHide] / [TransformUnhide]): the
       hidden bit is 0x20 of the flags dword at +0x5C; the frame stamp at +0x6C
       stands in for the renderer's dirty bitset. */
    .p2align 4, 0xCC
    .globl fake_transform_hide
fake_transform_hide:
    .byte 0x8B, 0x41, 0x5C                      /* mov eax, [rcx+0x5C]       */
    .byte 0xA8, 0x20                            /* test al, 0x20             */
    .byte 0x75
    .byte th_done - (. + 1)                     /* jnz done                  */
    .byte 0x83, 0xC8, 0x20                      /* or eax, 0x20              */
    .byte 0x89, 0x41, 0x5C                      /* mov [rcx+0x5C], eax       */
    .byte 0x8B, 0x41, 0x64                      /* mov eax, [rcx+0x64]       */
    .byte 0x8B, 0x15
    .long g_renderFrame - (. + 4)               /* mov edx, [rip+frame]      */
    .byte 0x89, 0x51, 0x6C                      /* mov [rcx+0x6C], edx       */
th_done:
    .byte 0xC3

    .p2align 4, 0xCC
    .globl fake_transform_unhide
fake_transform_unhide:
    .byte 0x8B, 0x41, 0x5C                      /* mov eax, [rcx+0x5C]       */
    .byte 0xA8, 0x20                            /* test al, 0x20             */
    .byte 0x74
    .byte tu_done - (. + 1)                     /* jz done                   */
    .byte 0x83, 0xE0, 0xDF                      /* and eax, ~0x20            */
    .byte 0x89, 0x41, 0x5C                      /* mov [rcx+0x5C], eax       */
    .byte 0x8B, 0x41, 0x64                      /* mov eax, [rcx+0x64]       */
    .byte 0x8B, 0x15
    .long g_renderFrame - (. + 4)               /* mov edx, [rip+frame]      */
    .byte 0x89, 0x51, 0x6C                      /* mov [rcx+0x6C], edx       */
tu_done:
    .byte 0xC3

    /* Transform::SetMatrix(Transform*, const Matrix44*, Vector3* scale)
       ([TransformSetMatrix]): copies the 64-byte matrix (row 3 = position). */
    .p2align 4, 0xCC
    .globl fake_transform_set_matrix
fake_transform_set_matrix:
    .byte 0x48, 0x8B, 0xC4                      /* mov rax, rsp              */
    .byte 0x48, 0x89, 0x58, 0x08                /* mov [rax+8], rbx          */
    .byte 0x48, 0x89, 0x70, 0x10                /* mov [rax+0x10], rsi       */
    .byte 0x48, 0x89, 0x78, 0x18                /* mov [rax+0x18], rdi       */
    .byte 0x41, 0x56                            /* push r14                  */
    .byte 0x48, 0x83, 0xEC, 0x60                /* sub rsp, 0x60             */
    .byte 0x80, 0x79, 0x5F, 0x00                /* cmp byte [rcx+0x5F], 0    */
    .byte 0x49, 0x8B, 0xF8                      /* mov rdi, r8               */
    movups xmm0, [rdx]
    movups [rcx], xmm0
    movups xmm0, [rdx + 0x10]
    movups [rcx + 0x10], xmm0
    movups xmm0, [rdx + 0x20]
    movups [rcx + 0x20], xmm0
    movups xmm0, [rdx + 0x30]
    movups [rcx + 0x30], xmm0
    add rsp, 0x60
    pop r14
    mov rbx, [rsp + 8]
    mov rsi, [rsp + 0x10]
    mov rdi, [rsp + 0x18]
    ret

    /* Transform::SetMatrixEx(t, matrix, rotation, scale)
       ([TransformSetMatrixEx]): four arguments - what goes at +0x70 it works
       out from the matrix (here: 1 1 1). */
    .p2align 4, 0xCC
    .globl fake_transform_set_matrix_ex
fake_transform_set_matrix_ex:
    .byte 0x48, 0x89, 0x5C, 0x24, 0x08          /* mov [rsp+8], rbx          */
    .byte 0x48, 0x89, 0x6C, 0x24, 0x10          /* mov [rsp+0x10], rbp       */
    .byte 0x48, 0x89, 0x74, 0x24, 0x18          /* mov [rsp+0x18], rsi       */
    .byte 0x57                                  /* push rdi                  */
    .byte 0x48, 0x83, 0xEC, 0x30                /* sub rsp, 0x30             */
    .byte 0x48, 0x8B, 0xDA                      /* mov rbx, rdx              */
    .byte 0x48, 0x8B, 0xE9                      /* mov rbp, rcx              */
    .byte 0x48, 0x8B, 0xCB                      /* mov rcx, rbx              */
    .byte 0x48, 0x8D, 0x54, 0x24, 0x20          /* lea rdx, [rsp+0x20]       */
    mov rsi, r9
    mov rdi, r8
    call fsmx_extra
    movups xmm0, [rbx]
    movups [rbp], xmm0
    movups xmm0, [rbx + 0x10]
    movups [rbp + 0x10], xmm0
    movups xmm0, [rbx + 0x20]
    movups [rbp + 0x20], xmm0
    movups xmm0, [rbx + 0x30]
    movups [rbp + 0x30], xmm0
    movups xmm0, [rdi]
    movups [rbp + 0x40], xmm0
    movsd xmm0, [rsi]
    movsd [rbp + 0x50], xmm0
    mov eax, [rsi + 8]
    mov [rbp + 0x58], eax
    movsd xmm0, [rsp + 0x20]
    movsd [rbp + 0x70], xmm0
    mov eax, [rsp + 0x28]
    mov [rbp + 0x78], eax
    mov eax, DWORD PTR [rip + g_renderFrame]
    mov [rbp + 0x6C], eax
    mov rbx, [rsp + 0x40]
    mov rbp, [rsp + 0x48]
    mov rsi, [rsp + 0x50]
    add rsp, 0x30
    pop rdi
    ret
fsmx_extra:
    mov DWORD PTR [rdx], 0x3F800000
    mov DWORD PTR [rdx + 4], 0x3F800000
    mov DWORD PTR [rdx + 8], 0x3F800000
    xor eax, eax
    ret

    /* Transform::SetMatrixEx2(t, matrix, rotation, scale, flags, extra)
       ([TransformSetMatrixEx2]), like the game's: SIX arguments - the flags
       byte and a pointer to three floats are on the stack, and the pointer is
       read (0.5.0's first build passed five: the game crashed). */
    .p2align 4, 0xCC
    .globl fake_transform_set_matrix_ex2
fake_transform_set_matrix_ex2:
    .byte 0xC5, 0xFC, 0x10, 0x02                /* vmovups ymm0, [rdx]       */
    .byte 0xC5, 0xFC, 0x11, 0x01                /* vmovups [rcx], ymm0       */
    .byte 0xC5, 0xFC, 0x10, 0x4A, 0x20          /* vmovups ymm1, [rdx+0x20]  */
    .byte 0xC5, 0xFC, 0x11, 0x49, 0x20          /* vmovups [rcx+0x20], ymm1  */
    .byte 0xC4, 0xC1, 0x78, 0x10, 0x00          /* vmovups xmm0, [r8]        */
    .byte 0xC5, 0xF8, 0x11, 0x41, 0x40          /* vmovups [rcx+0x40], xmm0  */
    .byte 0xC4, 0xC1, 0x7B, 0x10, 0x01          /* vmovsd xmm0, [r9]         */
    .byte 0xC5, 0xFB, 0x11, 0x41, 0x50          /* vmovsd [rcx+0x50], xmm0   */
    mov eax, [r9 + 8]
    mov r10, rcx
    mov [rcx + 0x58], eax
    movzx eax, BYTE PTR [rsp + 0x28]
    mov ecx, [rcx + 0x5C]
    and ecx, 0xFFFFFFE0
    or ecx, eax
    mov rax, [rsp + 0x30]
    mov [r10 + 0x5C], ecx
    movsd xmm0, [rax]
    movsd [r10 + 0x70], xmm0
    mov eax, [rax + 8]
    mov [r10 + 0x78], eax
    mov eax, DWORD PTR [rip + g_renderFrame]
    mov [r10 + 0x6C], eax
    .byte 0xC5, 0xF8, 0x77                      /* vzeroupper                */
    ret

    /* Transform::MarkDirty(Transform*, int frame) ([TransformMarkDirty]):
       called after code that wrote the matrix itself. */
    .p2align 4, 0xCC
    .globl fake_transform_mark_dirty
fake_transform_mark_dirty:
    .byte 0x44, 0x8B, 0x05
    .long g_renderFrame - (. + 4)               /* mov r8d, [rip+frame]      */
    .byte 0x83, 0xFA, 0xFF                      /* cmp edx, -1               */
    .byte 0x8B, 0x41, 0x64                      /* mov eax, [rcx+0x64]       */
    .byte 0x44, 0x0F, 0x45, 0xC2                /* cmovne r8d, edx           */
    .byte 0x44, 0x89, 0x41, 0x6C                /* mov [rcx+0x6C], r8d       */
    ret

    /* Camera2::CameraTarget virtuals ([CameraTarget] slots 10, 11, 18). The
       target's actor is at +0x10; the actor's first field is its transform. */
    .p2align 4, 0xCC
    .globl fake_ct_get_position
fake_ct_get_position:
    .byte 0x40, 0x53                            /* push rbx                  */
    .byte 0x48, 0x83, 0xEC, 0x20                /* sub rsp, 0x20             */
    .byte 0x48, 0x8B, 0x49, 0x10                /* mov rcx, [rcx+0x10]       */
    .byte 0x48, 0x8B, 0xDA                      /* mov rbx, rdx              */
    mov rax, [rcx]
    movsd xmm0, [rax + 0x30]
    mov eax, [rax + 0x38]
    movsd [rbx], xmm0
    mov [rbx + 8], eax
    mov rax, rbx
    add rsp, 0x20
    pop rbx
    ret

    .p2align 4, 0xCC
    .globl fake_ct_get_matrix
fake_ct_get_matrix:
    .byte 0x48, 0x8B, 0x41, 0x10                /* mov rax, [rcx+0x10]       */
    .byte 0x48, 0x8B, 0x08                      /* mov rcx, [rax]            */
    movups xmm0, [rcx]
    movups [rdx], xmm0
    movups xmm0, [rcx + 0x10]
    movups [rdx + 0x10], xmm0
    movups xmm0, [rcx + 0x20]
    movups [rdx + 0x20], xmm0
    movups xmm0, [rcx + 0x30]
    movups [rdx + 0x30], xmm0
    mov rax, rdx
    ret

    /* The tracked joint: 1 m above the actor's position. */
    .p2align 4, 0xCC
    .globl fake_ct_get_track
fake_ct_get_track:
    .byte 0x48, 0x89, 0x5C, 0x24, 0x08          /* mov [rsp+8], rbx          */
    .byte 0x57                                  /* push rdi                  */
    .byte 0x48, 0x83, 0xEC, 0x20                /* sub rsp, 0x20             */
    .byte 0x48, 0x8B, 0x59, 0x10                /* mov rbx, [rcx+0x10]       */
    .byte 0x48, 0x8B, 0xFA                      /* mov rdi, rdx              */
    mov rax, [rbx]
    movss xmm0, [rax + 0x30]
    movss [rdi], xmm0
    movss xmm0, [rax + 0x34]
    addss xmm0, [rip + ct_joint_up]
    movss [rdi + 4], xmm0
    movss xmm0, [rax + 0x38]
    movss [rdi + 8], xmm0
    mov rax, rdi
    add rsp, 0x20
    pop rdi
    mov rbx, [rsp + 8]
    ret
ct_joint_up:
    .float 1.0

    .p2align 4, 0xCC
    .globl fake_ct_nop
fake_ct_nop:
    xor eax, eax
    ret

    /* BehaviorPedestrianInteract's activation (vtable slot 1, [PedestrianInteract]):
       the prologue of the game's, then counts the call. */
    .p2align 4, 0xCC
    .globl fake_ped_enter
fake_ped_enter:
    .byte 0x48, 0x89, 0x5C, 0x24, 0x08          /* mov [rsp+8], rbx          */
    .byte 0x48, 0x89, 0x74, 0x24, 0x10          /* mov [rsp+0x10], rsi       */
    .byte 0x57                                  /* push rdi                  */
    .byte 0x48, 0x83, 0xEC, 0x20                /* sub rsp, 0x20             */
    .byte 0x48, 0x8B, 0xF2                      /* mov rsi, rdx              */
    .byte 0x48, 0x8B, 0xD9                      /* mov rbx, rcx              */
    .byte 0xE8
    .long fpe_base - (. + 4)                    /* call base                 */
    .byte 0x48, 0x8D, 0x4B, 0x64                /* lea rcx, [rbx+0x64]       */
    mov eax, dword ptr [rsi]
    mov dword ptr [rbx + 0x68], eax
    lock inc dword ptr [rip + g_pedEnterCount]
    mov rbx, [rsp + 0x30]
    mov rsi, [rsp + 0x38]
    add rsp, 0x20
    pop rdi
    ret
fpe_base:
    ret

/* ------------------------------------------------------------------------
 * The game's physics and damage (bindings.ini [PhysicsRaycast] ...
 * [DamageSphere]): the game's byte patterns at the start of each function,
 * then a body that does what the mod needs from it, mostly in
 * deferred_host.cpp (mock_*). Only with -DMOCK_PHYSICS (the end-to-end host;
 * the smoke host has no physics).
 * ---------------------------------------------------------------------- */
#ifdef MOCK_PHYSICS

    /* Somewhere the game casts a ray ([PhysicsRaycast], resolve = call):
       lea r13, [rip+tag] ... mov rcx, [rip+g_physics] ; call the ray cast.
       Never run - only its bytes are looked for. */
    .p2align 4, 0xCC
    .globl fake_phys_caller
fake_phys_caller:
    .byte 0x4C, 0x8D, 0x2D                      /* lea r13, [rip+tag]        */
    .long fpc_tag - (. + 4)
    .byte 0x4C, 0x89, 0x6C, 0x24, 0x20          /* mov [rsp+0x20], r13       */
    .byte 0x4C, 0x8B, 0xCD                      /* mov r9, rbp               */
    .byte 0x4D, 0x8B, 0xC7                      /* mov r8, r15               */
    .byte 0x48, 0x8D, 0x54, 0x24, 0x60          /* lea rdx, [rsp+0x60]       */
    .byte 0x48, 0x8B, 0x0D                      /* mov rcx, [rip+g_physics]  */
    .long g_physics - (. + 4)
    .byte 0xE8                                  /* call the ray cast         */
    .long fake_phys_ray - (. + 4)
    ret
fpc_tag:
    .asciz "mock: the game's own ray"

    /* The ray cast: add rcx, 0x90 (the physics system's query system) ;
       jmp CastRayImmediate (mock_ray_cast). */
    .p2align 4, 0xCC
    .globl fake_phys_ray
fake_phys_ray:
    .byte 0x48, 0x81, 0xC1, 0x90, 0x00, 0x00, 0x00 /* add rcx, 0x90          */
    .byte 0xE9                                  /* jmp                       */
    .long mock_ray_cast - (. + 4)

    /* The physics frame ([PhysicsFrame]): steps the world (mock_physics_step)
       unless it is paused or the timestep x time scale is 0. The mod hooks it. */
    .p2align 4, 0xCC
    .globl fake_phys_frame
fake_phys_frame:
    .byte 0x48, 0x89, 0x5C, 0x24, 0x08          /* mov [rsp+8], rbx          */
    .byte 0x57                                  /* push rdi                  */
    .byte 0x48, 0x83, 0xEC, 0x30                /* sub rsp, 0x30             */
    cmp byte ptr [rip + g_physPaused], 0        /* 80 3D disp32 00           */
    .byte 0x48, 0x8B, 0xD9                      /* mov rbx, rcx              */
    .byte 0x75                                  /* jnz skip                  */
    .byte fpf_skip - (. + 1)
    .byte 0xC5, 0xFA, 0x10, 0x05                /* vmovss xmm0, [rip+timestep] */
    .long g_physTimestep - (. + 4)
    .byte 0xC5, 0xFA, 0x59, 0x15                /* vmulss xmm2, xmm0, [rip+scale] */
    .long g_physTimescale - (. + 4)
    .byte 0xC5, 0xF0, 0x57, 0xC9                /* vxorps xmm1, xmm1, xmm1   */
    .byte 0xC5, 0xF8, 0x2F, 0xD1                /* vcomiss xmm2, xmm1        */
    .byte 0x76                                  /* jbe skip                  */
    .byte fpf_skip - (. + 1)
    .byte 0x48, 0x8B, 0x0D                      /* mov rcx, [rip+g_physics]  */
    .long g_physics - (. + 4)
    .byte 0x40, 0x32, 0xFF                      /* xor dil, dil              */
    .byte 0xE8                                  /* call step                 */
    .long mock_physics_step - (. + 4)
fpf_skip:
    .byte 0x48, 0x8B, 0x5C, 0x24, 0x40          /* mov rbx, [rsp+0x40]       */
    .byte 0x48, 0x83, 0xC4, 0x30                /* add rsp, 0x30             */
    .byte 0x5F                                  /* pop rdi                   */
    .byte 0xC3                                  /* ret                       */

    /* CollRequest::CollRequest(type) ([CollRequestInit]). The mock's request:
       +0 live (-1 = released), +4 u16 hits to collect, +8 query type,
       +0x10 the transform left out (-1: none). */
    .p2align 4, 0xCC
    .globl fake_req_init
fake_req_init:
    .byte 0x48, 0x89, 0x5C, 0x24, 0x10          /* mov [rsp+0x10], rbx       */
    .byte 0x48, 0x89, 0x4C, 0x24, 0x08          /* mov [rsp+8], rcx          */
    .byte 0x57                                  /* push rdi                  */
    .byte 0x48, 0x83, 0xEC, 0x20                /* sub rsp, 0x20             */
    .byte 0x8B, 0xDA                            /* mov ebx, edx              */
    .byte 0x48, 0x8B, 0xF9                      /* mov rdi, rcx              */
    .byte 0xE8                                  /* call base                 */
    .long fri_stub - (. + 4)
    .byte 0x90                                  /* nop                       */
    .byte 0x48, 0x8B, 0xCF                      /* mov rcx, rdi              */
    .byte 0xE8                                  /* call                      */
    .long fri_stub - (. + 4)
    .byte 0x66, 0xC7, 0x47, 0x04, 0x01, 0x00    /* mov word [rdi+4], 1       */
    .byte 0x44, 0x8B, 0xC3                      /* mov r8d, ebx              */
    .byte 0x49, 0xC1, 0xE0, 0x05                /* shl r8, 5                 */
    .byte 0x4C, 0x03, 0x05                      /* add r8, [rip+types]       */
    .long g_collTypes - (. + 4)
    mov dword ptr [rdi], 1
    mov dword ptr [rdi + 8], ebx
    mov dword ptr [rdi + 0x10], -1
    lock inc dword ptr [rip + g_collLive]
    mov rax, rdi
    add rsp, 0x20
    pop rdi
    mov rbx, [rsp + 0x10]
    ret
fri_stub:
    ret

    /* CollRequest::~CollRequest ([CollRequestRelease]). */
    .p2align 4, 0xCC
    .globl fake_req_release
fake_req_release:
    .byte 0x40, 0x53                            /* push rbx                  */
    .byte 0x48, 0x83, 0xEC, 0x20                /* sub rsp, 0x20             */
    .byte 0x48, 0x8B, 0xD9                      /* mov rbx, rcx              */
    .byte 0x83, 0x39, 0xFF                      /* cmp dword [rcx], -1       */
    .byte 0x74                                  /* je done                   */
    .byte frr_done - (. + 1)
    .byte 0xFF, 0x0D                            /* dec dword [rip+live]      */
    .long g_collLive - (. + 4)
    .byte 0x8B, 0x11                            /* mov edx, [rcx]            */
    .byte 0x48, 0x8D, 0x0D                      /* lea rcx, [rip+pool]       */
    .long g_collPool - (. + 4)
    .byte 0xE8                                  /* call free                 */
    .long frr_stub - (. + 4)
    .byte 0xC7, 0x03, 0xFF, 0xFF, 0xFF, 0xFF    /* mov dword [rbx], -1       */
frr_done:
    add rsp, 0x20
    pop rbx
    ret
frr_stub:
    ret

    /* CollRequest::AddIgnoreActor(Actor*) ([CollRequestIgnoreActor]): the
       actor's transform's id (+0x64) goes into the request. */
    .p2align 4, 0xCC
    .globl fake_req_ignore
fake_req_ignore:
    .byte 0x48, 0x85, 0xD2                      /* test rdx, rdx             */
    .byte 0x74                                  /* jz out                    */
    .byte fia_out - (. + 1)
    .byte 0x53                                  /* push rbx                  */
    .byte 0x48, 0x83, 0xEC, 0x20                /* sub rsp, 0x20             */
    .byte 0x48, 0x8B, 0x02                      /* mov rax, [rdx]            */
    .byte 0x48, 0x8B, 0xD9                      /* mov rbx, rcx              */
    .byte 0x48, 0x85, 0xC0                      /* test rax, rax             */
    .byte 0x74                                  /* jz done                   */
    .byte fia_done - (. + 1)
    .byte 0x8B, 0x40, 0x64                      /* mov eax, [rax+0x64]       */
    .byte 0x83, 0x39, 0xFF                      /* cmp dword [rcx], -1       */
    .byte 0x89, 0x44, 0x24, 0x38                /* mov [rsp+0x38], eax       */
    .byte 0x75                                  /* jnz store                 */
    .byte fia_store - (. + 1)
fia_store:
    mov dword ptr [rbx + 0x10], eax
fia_done:
    add rsp, 0x20
    pop rbx
fia_out:
    ret

    /* The Actor* behind hit `edx` of a query result ([QueryResultActor]):
       the record's transform (+8: 24-bit index, +0xB: has one), its actor
       handle (+0x68) if flagged (+0x5C & 0x1000), GetActor. */
    .p2align 4, 0xCC
    .globl fake_result_actor
fake_result_actor:
    .byte 0x48, 0x83, 0xEC, 0x28                /* sub rsp, 0x28             */
    .byte 0x48, 0x8B, 0x09                      /* mov rcx, [rcx]            */
    .byte 0x48, 0x83, 0xC1, 0x08                /* add rcx, 8                */
    .byte 0x4C, 0x63, 0xC2                      /* movsxd r8, edx            */
    .byte 0x49, 0xC1, 0xE0, 0x06                /* shl r8, 6                 */
    .byte 0x49, 0x03, 0xC8                      /* add rcx, r8               */
    .byte 0x44, 0x0F, 0xB6, 0x41, 0x03          /* movzx r8d, byte [rcx+3]   */
    .byte 0x45, 0x85, 0xC0                      /* test r8d, r8d             */
    .byte 0x74                                  /* jz null                   */
    .byte fra_null - (. + 1)
    .byte 0x8B, 0x11                            /* mov edx, [rcx]            */
    .byte 0x81, 0xE2, 0xFF, 0xFF, 0xFF, 0x00    /* and edx, 0xFFFFFF         */
    .byte 0xE8                                  /* call lookup               */
    .long mock_transform_lookup - (. + 4)
    .byte 0x48, 0x85, 0xC0                      /* test rax, rax             */
    .byte 0x74                                  /* jz null                   */
    .byte fra_null - (. + 1)
    .byte 0xF7, 0x40, 0x5C, 0x00, 0x10, 0x00, 0x00 /* test dword [rax+0x5C], 0x1000 */
    .byte 0x74                                  /* jz null                   */
    .byte fra_null - (. + 1)
    .byte 0x48, 0x8D, 0x48, 0x68                /* lea rcx, [rax+0x68]       */
    .byte 0xE8                                  /* call GetActor             */
    .long fake_get_actor - (. + 4)
    .byte 0x48, 0x83, 0xC4, 0x28                /* add rsp, 0x28             */
    .byte 0xC3                                  /* ret                       */
fra_null:
    .byte 0x33, 0xC0                            /* xor eax, eax              */
    .byte 0x48, 0x83, 0xC4, 0x28                /* add rsp, 0x28             */
    .byte 0xC3                                  /* ret                       */

    /* The physics query allocator ([PhysicsQueryPool]): only its counter's
       offset (0x1E880 in the query system) and the limit (256) are read.
       Never run. */
    .p2align 4, 0xCC
    .globl fake_query_alloc
fake_query_alloc:
    .byte 0x48, 0x89, 0x5C, 0x24, 0x10          /* mov [rsp+0x10], rbx       */
    .byte 0x48, 0x89, 0x6C, 0x24, 0x18          /* mov [rsp+0x18], rbp       */
    .byte 0x48, 0x89, 0x74, 0x24, 0x20          /* mov [rsp+0x20], rsi       */
    .byte 0x57                                  /* push rdi                  */
    .byte 0x41, 0x56                            /* push r14                  */
    .byte 0x41, 0x57                            /* push r15                  */
    .byte 0x48, 0x83, 0xEC, 0x20                /* sub rsp, 0x20             */
    .byte 0x8B, 0xEA                            /* mov ebp, edx              */
    .byte 0xB8, 0x01, 0x00, 0x00, 0x00          /* mov eax, 1                */
    .byte 0xF0, 0x0F, 0xC1, 0x81                /* lock xadd [rcx+counter], eax */
    .long 0x1E880
    .byte 0x3D                                  /* cmp eax, limit            */
    .long 256
    .byte 0x7D                                  /* jge full                  */
    .byte fqa_full - (. + 1)
fqa_full:
    .byte 0x33, 0xC0                            /* xor eax, eax              */
    .byte 0x48, 0x83, 0xC4, 0x20                /* add rsp, 0x20             */
    .byte 0x41, 0x5F                            /* pop r15                   */
    .byte 0x41, 0x5E                            /* pop r14                   */
    .byte 0x5F                                  /* pop rdi                   */
    .byte 0x48, 0x8B, 0x5C, 0x24, 0x10          /* mov rbx, [rsp+0x10]       */
    .byte 0x48, 0x8B, 0x6C, 0x24, 0x18          /* mov rbp, [rsp+0x18]       */
    .byte 0x48, 0x8B, 0x74, 0x24, 0x20          /* mov rsi, [rsp+0x20]       */
    .byte 0xC3                                  /* ret                       */

    /* DamageSystem::DamageSphere(this, const Vec3& centre, float radius,
       const CollRequest&) ([DamageSphere]): a new DamageRequest (entry +
       0x68) from mock_damage_alloc / mock_damage_fill. */
    .p2align 4, 0xCC
    .globl fake_damage_sphere
fake_damage_sphere:
    .byte 0x48, 0x89, 0x5C, 0x24, 0x08          /* mov [rsp+8], rbx          */
    .byte 0x48, 0x89, 0x6C, 0x24, 0x10          /* mov [rsp+0x10], rbp       */
    .byte 0x48, 0x89, 0x74, 0x24, 0x18          /* mov [rsp+0x18], rsi       */
    .byte 0x57                                  /* push rdi                  */
    .byte 0x48, 0x83, 0xEC, 0x60                /* sub rsp, 0x60             */
    .byte 0xC5, 0xF8, 0x29, 0x74, 0x24, 0x50    /* vmovaps [rsp+0x50], xmm6  */
    .byte 0x49, 0x8B, 0xE9                      /* mov rbp, r9 (request)     */
    .byte 0xC5, 0xF8, 0x28, 0xF2                /* vmovaps xmm6, xmm2 (radius) */
    .byte 0x48, 0x8B, 0xF2                      /* mov rsi, rdx (centre)     */
    .byte 0x48, 0x8B, 0xF9                      /* mov rdi, rcx (system)     */
    .byte 0xE8                                  /* call alloc                */
    .long mock_damage_alloc - (. + 4)
    .byte 0x48, 0x8B, 0xD8                      /* mov rbx, rax              */
    .byte 0x48, 0x85, 0xC0                      /* test rax, rax             */
    .byte 0x0F, 0x84                            /* jz fail                   */
    .long fds_fail - (. + 4)
    .byte 0x48, 0x8B, 0xD5                      /* mov rdx, rbp              */
    .byte 0xC7, 0x00, 0x04, 0x00, 0x00, 0x00    /* mov dword [rax], 4        */
    .byte 0x48, 0x8B, 0xC8                      /* mov rcx, rax              */
    .byte 0x48, 0x8B, 0xD6                      /* mov rdx, rsi              */
    .byte 0xC5, 0xF8, 0x28, 0xD6                /* vmovaps xmm2, xmm6        */
    .byte 0x4C, 0x8B, 0xCD                      /* mov r9, rbp               */
    .byte 0xE8                                  /* call fill                 */
    .long mock_damage_fill - (. + 4)
    .byte 0xEB                                  /* jmp done                  */
    .byte fds_done - (. + 1)
fds_fail:
    .byte 0x33, 0xC0                            /* xor eax, eax              */
fds_done:
    .byte 0xC5, 0xF8, 0x28, 0x74, 0x24, 0x50    /* vmovaps xmm6, [rsp+0x50]  */
    .byte 0x48, 0x8B, 0x5C, 0x24, 0x70          /* mov rbx, [rsp+0x70]       */
    .byte 0x48, 0x8B, 0x6C, 0x24, 0x78          /* mov rbp, [rsp+0x78]       */
    .byte 0x48, 0x8B, 0xB4, 0x24, 0x80, 0x00, 0x00, 0x00 /* mov rsi, [rsp+0x80] */
    .byte 0x48, 0x83, 0xC4, 0x60                /* add rsp, 0x60             */
    .byte 0x5F                                  /* pop rdi                   */
    .byte 0xC3                                  /* ret                       */

    /* DamageSystem::DamageActor(this, const ActorHandle*) ([DamageActor]): a
       new DamageRequest for that actor (entry + 0x68), like the game's: type
       1 at +0, the handle at +4. */
    .p2align 4, 0xCC
    .globl fake_damage_actor
fake_damage_actor:
    .byte 0x48, 0x89, 0x5C, 0x24, 0x08          /* mov [rsp+8], rbx          */
    .byte 0x57                                  /* push rdi                  */
    .byte 0x48, 0x83, 0xEC, 0x20                /* sub rsp, 0x20             */
    .byte 0x48, 0x8B, 0xFA                      /* mov rdi, rdx (handle)     */
    .byte 0x48, 0x8B, 0xD9                      /* mov rbx, rcx (system)     */
    .byte 0xE8                                  /* call alloc                */
    .long mock_damage_alloc - (. + 4)
    .byte 0x48, 0x85, 0xC0                      /* test rax, rax             */
    .byte 0x74                                  /* je fail                   */
    .byte fda_fail - (. + 1)
    .byte 0xC7, 0x00, 0x01, 0x00, 0x00, 0x00    /* mov dword [rax], 1        */
    .byte 0x8B, 0x0F                            /* mov ecx, [rdi]            */
    .byte 0x89, 0x48, 0x04                      /* mov [rax+4], ecx          */
    .byte 0x48, 0x83, 0xC0, 0x68                /* add rax, 0x68             */
    .byte 0x48, 0x8B, 0x5C, 0x24, 0x30          /* mov rbx, [rsp+0x30]       */
    .byte 0x48, 0x83, 0xC4, 0x20                /* add rsp, 0x20             */
    .byte 0x5F                                  /* pop rdi                   */
    .byte 0xC3                                  /* ret                       */
fda_fail:
    .byte 0x33, 0xC0                            /* xor eax, eax              */
    .byte 0x48, 0x8B, 0x5C, 0x24, 0x30          /* mov rbx, [rsp+0x30]       */
    .byte 0x48, 0x83, 0xC4, 0x20                /* add rsp, 0x20             */
    .byte 0x5F                                  /* pop rdi                   */
    .byte 0xC3                                  /* ret                       */

    /* Photo mode's camera-mode switch ([PhotoMode]): leaving or entering the
       selfie mode (index 3) it passes the PhotomodeSystem object - lea rcx,
       [rip+X] - to the hero / selfie swap. Never run. */
    .p2align 4, 0xCC
    .globl fake_photo_switch
fake_photo_switch:
    .byte 0x48, 0x89, 0x5C, 0x24, 0x08          /* mov [rsp+8], rbx          */
    .byte 0x48, 0x89, 0x74, 0x24, 0x10          /* mov [rsp+0x10], rsi       */
    .byte 0x57                                  /* push rdi                  */
    .byte 0x48, 0x83, 0xEC, 0x20                /* sub rsp, 0x20             */
    .byte 0x41, 0x8B, 0xF0                      /* mov esi, r8d              */
    .byte 0x48, 0x8B, 0xF9                      /* mov rdi, rcx              */
    .byte 0xE8                                  /* call mode index           */
    .long fps_stub - (. + 4)
    .byte 0x8B, 0xD6                            /* mov edx, esi              */
    .byte 0x48, 0x8B, 0xCF                      /* mov rcx, rdi              */
    .byte 0x8B, 0xD8                            /* mov ebx, eax              */
    .byte 0xE8                                  /* call mode index           */
    .long fps_stub - (. + 4)
    .byte 0x8B, 0xF8                            /* mov edi, eax              */
    .byte 0x83, 0xFB, 0x03                      /* cmp ebx, 3                */
    .byte 0x75, 0x0C                            /* jne +12                   */
    .byte 0x48, 0x8D, 0x0D                      /* lea rcx, [rip+photo mode] */
    .long g_photoMode - (. + 4)
    .byte 0xE8                                  /* call swap back            */
    .long fps_stub - (. + 4)
    .byte 0x48, 0x8B, 0x5C, 0x24, 0x30          /* mov rbx, [rsp+0x30]       */
    .byte 0x48, 0x8B, 0x74, 0x24, 0x38          /* mov rsi, [rsp+0x38]       */
    .byte 0x48, 0x83, 0xC4, 0x20                /* add rsp, 0x20             */
    .byte 0x5F                                  /* pop rdi                   */
    .byte 0xC3                                  /* ret                       */
fps_stub:
    .byte 0x33, 0xC0                            /* xor eax, eax              */
    .byte 0xC3                                  /* ret                       */

    /* Where the game damages: lea rcx, [rip+DamageSystem] ; call DamageSphere.
       Never run. */
    .p2align 4, 0xCC
    .globl fake_damage_caller
fake_damage_caller:
    .byte 0x48, 0x8D, 0x0D                      /* lea rcx, [rip+system]     */
    .long g_damageSystem - (. + 4)
    .byte 0xE8                                  /* call DamageSphere         */
    .long fake_damage_sphere - (. + 4)
    ret

    /* Where the game asks its component registry for a type
       ([ComponentRegistry], [GetComponentInfo]): lea rcx, [rip+registry] ;
       call GetComponentInfo(registry, crc of the name). Never run. */
    .p2align 4, 0xCC
    .globl fake_component_lookup
fake_component_lookup:
    .byte 0x48, 0x8D, 0x0D                      /* lea rcx, [rip+registry]   */
    .long g_componentRegistry - (. + 4)
    .byte 0xE8                                  /* call GetComponentInfo     */
    .long mock_get_component_info - (. + 4)
    .byte 0x49, 0x8B, 0x4E, 0x10                /* mov rcx, [r14+0x10]       */
    .byte 0x48, 0x8D, 0x54, 0x24, 0x20          /* lea rdx, [rsp+0x20]       */
    .byte 0x48, 0x83, 0xC1, 0x58                /* add rcx, 0x58             */
    .byte 0xC3                                  /* ret                       */
#endif /* MOCK_PHYSICS */
