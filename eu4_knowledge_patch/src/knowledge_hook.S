.intel_syntax
.text

/*
 * Position-independent replacement for the vector tag scan at EU4 0x941e80.
 * Entry: RDX = current tag, RAX = end tag, R8W = wanted country tag.
 * RCX/RAX/RDX are dead after the original scan and may be clobbered.
 * The two final rel32 jumps are relocated by hot_loop_patch.cpp.
 */
.globl knowledge_hook_begin
knowledge_hook_begin:
    /* Peel the first pair: short lists avoid the unroll setup entirely. */
    cmp word ptr [rdx + 4], r8w
    je knowledge_hook_found
    add rdx, 8
    cmp rdx, rax
    je knowledge_hook_out
    cmp word ptr [rdx + 4], r8w
    je knowledge_hook_found
    add rdx, 8
    cmp rdx, rax
    je knowledge_hook_out

    /* RCX = end of complete pairs; RAX remains the original end. */
    mov rcx, rax
    sub rcx, rdx
    shr rcx, 4
    shl rcx, 4
    add rcx, rdx
    cmp rdx, rcx
    je knowledge_hook_tail
knowledge_hook_pairs:
    cmp word ptr [rdx + 4], r8w
    je knowledge_hook_found
    cmp word ptr [rdx + 12], r8w
    je knowledge_hook_found
    add rdx, 16
    cmp rdx, rcx
    jne knowledge_hook_pairs
knowledge_hook_tail:
    cmp rdx, rax
    je knowledge_hook_out
    cmp word ptr [rdx + 4], r8w
    je knowledge_hook_found
    jmp knowledge_hook_out

knowledge_hook_found:
    .byte 0xE9
.globl knowledge_hook_found_displacement
knowledge_hook_found_displacement:
    .long 0
knowledge_hook_out:
    .byte 0xE9
.globl knowledge_hook_out_displacement
knowledge_hook_out_displacement:
    .long 0
.globl knowledge_hook_end
knowledge_hook_end:
