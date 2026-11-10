.intel_syntax
.text

# Add enters with RAX=current entry, RCX=index, RDX=count and EDI=key.
# Arrays below four entries avoid SIMD setup; larger arrays use four-lane blocks.
.globl add_search_relay
add_search_relay:
    cmp rdx, 4
    jb add_search_small
    push r10
    movd xmm0, edi
    pshufd xmm0, xmm0, 0
    mov r11, rdx
    and r11, -4
add_search_vector_loop:
    cmp rcx, r11
    jae add_search_tail
    movdqu xmm1, xmmword ptr [rax]
    movdqu xmm2, xmmword ptr [rax + 16]
    pcmpeqd xmm1, xmm0
    pcmpeqd xmm2, xmm0
    movmskps r10d, xmm1
    test r10b, 0x05
    jnz add_search_first_vector_found
    movmskps r10d, xmm2
    test r10b, 0x05
    jnz add_search_second_vector_found
    add rax, 32
    add rcx, 4
    jmp add_search_vector_loop
add_search_second_vector_found:
    add rax, 16
    add rcx, 2
add_search_first_vector_found:
    and r10d, 0x05
    bsf r10d, r10d
    shr r10d, 1
    add rcx, r10
    lea rax, [rax + r10*8]
    pop r10
    jmp qword ptr [rip + add_search_found_target]
add_search_tail:
    cmp rcx, rdx
    jae add_search_not_found
    cmp dword ptr [rax], edi
    je add_search_scalar_found
    inc rcx
    add rax, 8
    jmp add_search_tail
add_search_scalar_found:
    pop r10
    jmp qword ptr [rip + add_search_found_target]
add_search_not_found:
    pop r10
    jmp qword ptr [rip + add_search_not_found_target]
add_search_small:
    cmp rcx, rdx
    jae add_search_small_not_found
    cmp dword ptr [rax], edi
    je add_search_small_found
    inc rcx
    add rax, 8
    jmp add_search_small
add_search_small_found:
    jmp qword ptr [rip + add_search_found_target]
add_search_small_not_found:
    jmp qword ptr [rip + add_search_not_found_target]

# Subtract enters with RCX=current entry, RAX=index, RDX=count and EDI=key.
.globl subtract_search_relay
subtract_search_relay:
    cmp rdx, 4
    jb subtract_search_small
    push r10
    movd xmm0, edi
    pshufd xmm0, xmm0, 0
    mov r11, rdx
    and r11, -4
subtract_search_vector_loop:
    cmp rax, r11
    jae subtract_search_tail
    movdqu xmm1, xmmword ptr [rcx]
    movdqu xmm2, xmmword ptr [rcx + 16]
    pcmpeqd xmm1, xmm0
    pcmpeqd xmm2, xmm0
    movmskps r10d, xmm1
    test r10b, 0x05
    jnz subtract_search_first_vector_found
    movmskps r10d, xmm2
    test r10b, 0x05
    jnz subtract_search_second_vector_found
    add rcx, 32
    add rax, 4
    jmp subtract_search_vector_loop
subtract_search_second_vector_found:
    add rcx, 16
    add rax, 2
subtract_search_first_vector_found:
    and r10d, 0x05
    bsf r10d, r10d
    shr r10d, 1
    add rax, r10
    lea rcx, [rcx + r10*8]
    pop r10
    jmp qword ptr [rip + subtract_search_found_target]
subtract_search_tail:
    cmp rax, rdx
    jae subtract_search_not_found
    cmp dword ptr [rcx], edi
    je subtract_search_scalar_found
    inc rax
    add rcx, 8
    jmp subtract_search_tail
subtract_search_scalar_found:
    pop r10
    jmp qword ptr [rip + subtract_search_found_target]
subtract_search_not_found:
    pop r10
    jmp qword ptr [rip + subtract_search_not_found_target]
subtract_search_small:
    cmp rax, rdx
    jae subtract_search_small_not_found
    cmp dword ptr [rcx], edi
    je subtract_search_small_found
    inc rax
    add rcx, 8
    jmp subtract_search_small
subtract_search_small_found:
    jmp qword ptr [rip + subtract_search_found_target]
subtract_search_small_not_found:
    jmp qword ptr [rip + subtract_search_not_found_target]
