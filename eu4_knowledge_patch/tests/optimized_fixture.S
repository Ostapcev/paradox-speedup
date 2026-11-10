.intel_syntax
.text

/* Args: current +0, other +8, entries/head +16, end +24, wanted +32. */
.globl core_original
core_original:
    push rdi
    mov rdi, qword ptr [rcx]
    mov r10, qword ptr [rcx + 8]
    mov rdx, qword ptr [rcx + 16]
    mov rax, qword ptr [rcx + 24]
    mov r8, qword ptr [rcx + 32]
    cmp rdx, rax
    je core_original_out
core_original_site:
    .byte 0x48, 0x8B, 0x0A, 0x48, 0xC1, 0xE9, 0x20, 0x66, 0x41, 0x3B, 0xC8
    je core_original_found
    add rdx, 8
    cmp rdx, rax
    jne core_original_site
    jmp core_original_out
core_original_found:
    mov eax, 1
    pop rdi
    ret
core_original_out:
    xor eax, eax
    pop rdi
    ret

.globl core_fixture
core_fixture:
    push rdi
    mov rdi, qword ptr [rcx]
    mov r10, qword ptr [rcx + 8]
    mov rdx, qword ptr [rcx + 16]
    mov rax, qword ptr [rcx + 24]
    mov r8, qword ptr [rcx + 32]
    cmp rdx, rax
    je core_fixture_out
.globl core_fixture_site
core_fixture_site:
    .byte 0x48, 0x8B, 0x0A, 0x48, 0xC1, 0xE9, 0x20, 0x66, 0x41, 0x3B, 0xC8
.globl core_fixture_continuation
core_fixture_continuation:
    je core_fixture_found
    add rdx, 8
    cmp rdx, rax
    jne core_fixture_site
    jmp core_fixture_out
.globl core_fixture_found
core_fixture_found:
    mov eax, 1
    pop rdi
    ret
.globl core_fixture_out
core_fixture_out:
    xor eax, eax
    pop rdi
    ret

.globl claim_original
claim_original:
    push rdi
    mov rdi, qword ptr [rcx]
    mov r10, qword ptr [rcx + 8]
    mov rax, qword ptr [rcx + 16]
    mov rdx, qword ptr [rcx + 32]
    test rax, rax
    je claim_original_out
claim_original_site:
    .byte 0x48, 0x8B, 0x08, 0x48, 0xC1, 0xE9, 0x20, 0x66, 0x3B, 0xCA
    je claim_original_found
    mov rcx, qword ptr [rax + 0x10]
    mov rax, rcx
    test rcx, rcx
    jne claim_original_site
    jmp claim_original_out
claim_original_found:
    mov eax, 1
    pop rdi
    ret
claim_original_out:
    xor eax, eax
    pop rdi
    ret

.globl claim_fixture
claim_fixture:
    push rdi
    mov rdi, qword ptr [rcx]
    mov r10, qword ptr [rcx + 8]
    mov rax, qword ptr [rcx + 16]
    mov rdx, qword ptr [rcx + 32]
    test rax, rax
    je claim_fixture_out
.globl claim_fixture_site
claim_fixture_site:
    .byte 0x48, 0x8B, 0x08, 0x48, 0xC1, 0xE9, 0x20, 0x66, 0x3B, 0xCA
.globl claim_fixture_continuation
claim_fixture_continuation:
    je claim_fixture_found
    mov rcx, qword ptr [rax + 0x10]
    mov rax, rcx
    test rcx, rcx
    jne claim_fixture_site
    jmp claim_fixture_out
.globl claim_fixture_found
claim_fixture_found:
    mov eax, 1
    pop rdi
    ret
.globl claim_fixture_out
claim_fixture_out:
    xor eax, eax
    pop rdi
    ret
