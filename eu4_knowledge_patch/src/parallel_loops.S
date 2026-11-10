.intel_syntax
.text

/*
 * Parallel phase 2 entry at EU4 RVA 0x941dc7.
 * RDI=current country, R12=other country, R13=relation. The tag-indexed byte
 * contains religion/core/claim/visible in bits 0..3 and validity bits 4..5.
 */
.globl knowledge_parallel_apply_begin
knowledge_parallel_apply_begin:
    /* RAX/RDX are live in the displaced path but are scratch in phase 2. */
    push rax
    push rdx
    .byte 0x65, 0x48, 0x8B, 0x0C, 0x25
.globl knowledge_parallel_apply_tls
knowledge_parallel_apply_tls:
    .long 0
    test rcx, rcx
    je knowledge_parallel_apply_fallback
    cmp qword ptr [rcx], rdi
    jne knowledge_parallel_apply_fallback
    movzx eax, word ptr [r12 + 0x24]
    movzx edx, byte ptr [rcx + rax + 0x4008]
    mov eax, edx
    and eax, 0x30
    cmp eax, 0x20                    /* valid and no virtual fallback */
    jne knowledge_parallel_apply_fallback
    mov r15d, edx
    and r15d, 1
    mov ecx, edx
    shr ecx, 1
    and ecx, 1
    mov r9d, edx
    shr r9d, 2
    and r9d, 1
    mov r8d, edx
    shr r8d, 3
    and r8d, 1
    add rsp, 16
    .byte 0xE9
.globl knowledge_parallel_apply_commit_displacement
knowledge_parallel_apply_commit_displacement:
    .long 0

knowledge_parallel_apply_fallback:
    pop rdx
    pop rax
    /* Original instructions 0x941dc7..0x941dde. */
    .byte 0x45, 0x32, 0xFF
    .byte 0x32, 0xC9
    .byte 0x45, 0x32, 0xC0
    .byte 0x44, 0x88, 0x85, 0x40, 0x02, 0x00, 0x00
    .byte 0x45, 0x32, 0xC9
    .byte 0x44, 0x88, 0x4C, 0x24, 0x38
    .byte 0xE9
.globl knowledge_parallel_apply_fallback_displacement
knowledge_parallel_apply_fallback_displacement:
    .long 0
.globl knowledge_parallel_apply_end
knowledge_parallel_apply_end:

/* Read-only timer around the original per-country province loop. */
.globl knowledge_parallel_timing_begin
knowledge_parallel_timing_begin:
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    sub rsp, 0x28
    call qword ptr [rip + knowledge_parallel_timing_start_target]
    add rsp, 0x28
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    .byte 0x45, 0x32, 0xFF
    .byte 0x32, 0xC9
    .byte 0x45, 0x32, 0xC0
    .byte 0x44, 0x88, 0x85, 0x40, 0x02, 0x00, 0x00
    .byte 0x45, 0x32, 0xC9
    .byte 0x44, 0x88, 0x4C, 0x24, 0x38
    .byte 0xE9
.globl knowledge_parallel_timing_continuation_displacement
knowledge_parallel_timing_continuation_displacement:
    .long 0
    .p2align 3, 0x90
knowledge_parallel_timing_start_target:
    .quad knowledge_parallel_timing_start
.globl knowledge_parallel_timing_end
knowledge_parallel_timing_end:

/* Shadow verifier at 0x942047. It never writes game state. */
.globl knowledge_parallel_verify_begin
.extern knowledge_parallel_verify
knowledge_parallel_verify_begin:
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    sub rsp, 0x28
    mov rcx, rdi
    /* R12 is the owned-province iterator by 0x942047, not the country.
     * Original other=RSP+0x40; our pushes and shadow space add 0x60. */
    mov rdx, qword ptr [rsp + 0xa0]
    mov r8, r13
    /* The whole stub is copied: a direct rel32 CALL would retain a stale
     * displacement. The local pointer remains valid at the new RIP. */
    call qword ptr [rip + knowledge_parallel_verify_target]
    add rsp, 0x28
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    /* Original instructions 0x942047..0x942050. */
    .byte 0x4C, 0x8B, 0x75, 0x80
    .byte 0x48, 0x8B, 0x74, 0x24, 0x60
    .byte 0xE9
.globl knowledge_parallel_verify_continuation_displacement
knowledge_parallel_verify_continuation_displacement:
    .long 0
    .p2align 3, 0x90
knowledge_parallel_verify_target:
    .quad knowledge_parallel_verify
.globl knowledge_parallel_verify_end
knowledge_parallel_verify_end:
