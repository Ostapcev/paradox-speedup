.intel_syntax
.text

/*
 * Snapshot layout: country +0, core TagBits +8, claim TagBits +0x2008.
 * Core entry: RDI=current country, R10=other country, RDX=begin, RAX=end,
 * R8W=current tag. RCX is dead in the original loop.
 */
.globl knowledge_optimized_core_begin
knowledge_optimized_core_begin:
    push rax
    push rdx
    .byte 0x65, 0x48, 0x8B, 0x0C, 0x25
.globl knowledge_optimized_core_tls
knowledge_optimized_core_tls:
    .long 0
    test rcx, rcx
    je knowledge_optimized_core_fallback
    cmp qword ptr [rcx], rdi
    jne knowledge_optimized_core_fallback
    movzx eax, word ptr [r10 + 0x24]
    bt qword ptr [rcx + 8], rax
    jc knowledge_optimized_core_found
    pop rdx
    pop rax
    jmp knowledge_optimized_core_out
knowledge_optimized_core_found:
    pop rdx
    pop rax
    .byte 0xE9
.globl knowledge_optimized_core_found_displacement
knowledge_optimized_core_found_displacement:
    .long 0
knowledge_optimized_core_fallback:
    pop rdx
    pop rax
    .byte 0x48, 0x8B, 0x0A, 0x48, 0xC1, 0xE9, 0x20, 0x66, 0x41, 0x3B, 0xC8
    .byte 0xE9
.globl knowledge_optimized_core_continuation_displacement
knowledge_optimized_core_continuation_displacement:
    .long 0
knowledge_optimized_core_out:
    .byte 0xE9
.globl knowledge_optimized_core_out_displacement
knowledge_optimized_core_out_displacement:
    .long 0
.globl knowledge_optimized_core_end
knowledge_optimized_core_end:

/* Claim entry: RAX=head, RDX=current tag; other registers are the same. */
.globl knowledge_optimized_claim_begin
knowledge_optimized_claim_begin:
    push rax
    push rdx
    .byte 0x65, 0x48, 0x8B, 0x0C, 0x25
.globl knowledge_optimized_claim_tls
knowledge_optimized_claim_tls:
    .long 0
    test rcx, rcx
    je knowledge_optimized_claim_fallback
    cmp qword ptr [rcx], rdi
    jne knowledge_optimized_claim_fallback
    movzx eax, word ptr [r10 + 0x24]
    bt qword ptr [rcx + 0x2008], rax
    jc knowledge_optimized_claim_found
    pop rdx
    pop rax
    jmp knowledge_optimized_claim_out
knowledge_optimized_claim_found:
    pop rdx
    pop rax
    .byte 0xE9
.globl knowledge_optimized_claim_found_displacement
knowledge_optimized_claim_found_displacement:
    .long 0
knowledge_optimized_claim_fallback:
    pop rdx
    pop rax
    .byte 0x48, 0x8B, 0x08, 0x48, 0xC1, 0xE9, 0x20, 0x66, 0x3B, 0xCA
    .byte 0xE9
.globl knowledge_optimized_claim_continuation_displacement
knowledge_optimized_claim_continuation_displacement:
    .long 0
knowledge_optimized_claim_out:
    .byte 0xE9
.globl knowledge_optimized_claim_out_displacement
knowledge_optimized_claim_out_displacement:
    .long 0
.globl knowledge_optimized_claim_end
knowledge_optimized_claim_end:
