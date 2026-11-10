.intel_syntax
.text

/* ApplyArgs: current +0, other +8, relation +16, expected flags +24,
 * initial RAX/RDX +32/+40, scratch +48, observed RAX/RDX +56/+64. */
.macro DEFINE_APPLY function, site, fallback, commit
.globl \function
\function:
    push rbx
    push rbp
    push rdi
    push r12
    push r13
    push r15
    sub rsp, 0x68
    mov rbx, rcx
    mov rdi, qword ptr [rbx]
    mov r12, qword ptr [rbx + 8]
    mov r13, qword ptr [rbx + 16]
    mov rbp, qword ptr [rbx + 48]
    mov rax, qword ptr [rbx + 32]
    mov rdx, qword ptr [rbx + 40]
.globl \site
\site:
    .byte 0x45, 0x32, 0xFF
    .byte 0x32, 0xC9
    .byte 0x45, 0x32, 0xC0
    .byte 0x44, 0x88, 0x85, 0x40, 0x02, 0x00, 0x00
    .byte 0x45, 0x32, 0xC9
    .byte 0x44, 0x88, 0x4C, 0x24, 0x38
.globl \fallback
\fallback:
    mov qword ptr [rbx + 56], rax
    mov qword ptr [rbx + 64], rdx
    movzx r10d, byte ptr [rbx + 24]
    mov r15d, r10d
    and r15d, 1
    mov ecx, r10d
    shr ecx, 1
    and ecx, 1
    mov r9d, r10d
    shr r9d, 2
    and r9d, 1
    mov r8d, r10d
    shr r8d, 3
    and r8d, 1
.globl \commit
\commit:
    mov al, byte ptr [r13 + 0x45]
    and al, 0xA3
    test r15b, r15b
    jz 1f
    or al, 0x10
1:
    test cl, cl
    jz 2f
    or al, 0x08
2:
    test r9b, r9b
    jz 3f
    or al, 0x04
3:
    test r8b, r8b
    jz 4f
    or al, 0x40
4:
    mov byte ptr [r13 + 0x45], al
    movzx eax, al
    mov qword ptr [rbx + 72], rsp
    add rsp, 0x68
    pop r15
    pop r13
    pop r12
    pop rdi
    pop rbp
    pop rbx
    ret
.endm

DEFINE_APPLY apply_original, apply_original_site, apply_original_fallback, apply_original_commit
DEFINE_APPLY apply_fixture, apply_fixture_site, apply_fixture_fallback, apply_fixture_commit
DEFINE_APPLY timing_fixture, timing_fixture_site, timing_fixture_fallback, timing_fixture_commit

/* VerifyArgs: current/other/relation +0/+8/+16, volatile inputs +24..+64,
 * register outputs +72..+136. */
.macro DEFINE_VERIFY function, site, continuation
.globl \function
\function:
    push rbx
    push rbp
    push rdi
    push r12
    push r13
    push r14
    push rsi
    sub rsp, 0x60
    mov rbx, rcx
    lea rbp, [rsp + 0x80]
    /* The marker at +0x60 deliberately models original stack data, but it
     * overlaps this fixture's saved RSI. Keep a second copy in a local slot. */
    mov qword ptr [rsp + 0x58], rsi
    mov qword ptr [rsp], 0x1234
    mov qword ptr [rsp + 0x60], 0x5678
    mov rdi, qword ptr [rbx]
    mov rax, qword ptr [rbx + 8]
    mov qword ptr [rsp + 0x40], rax
    mov r12, rax
    not r12
    mov r10, r12
    mov r13, qword ptr [rbx + 16]
    mov rax, qword ptr [rbx + 24]
    mov rdx, qword ptr [rbx + 40]
    mov r8, qword ptr [rbx + 48]
    mov r9, qword ptr [rbx + 56]
    mov r11, qword ptr [rbx + 64]
    mov rcx, qword ptr [rbx + 32]
.globl \site
\site:
    .byte 0x4C, 0x8B, 0x75, 0x80
    .byte 0x48, 0x8B, 0x74, 0x24, 0x60
.globl \continuation
\continuation:
    mov qword ptr [rbx + 72], rax
    mov qword ptr [rbx + 80], rcx
    mov qword ptr [rbx + 88], rdx
    mov qword ptr [rbx + 96], r8
    mov qword ptr [rbx + 104], r9
    mov qword ptr [rbx + 112], r11
    mov qword ptr [rbx + 120], rdi
    mov qword ptr [rbx + 128], r10
    mov qword ptr [rbx + 136], r13
    mov qword ptr [rbx + 144], rsp
    add rsp, 0x60
    pop rsi
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rbp
    pop rbx
    mov rsi, qword ptr [rsp - 0x40]
    ret
.endm

DEFINE_VERIFY verify_original, verify_original_site, verify_original_continuation
DEFINE_VERIFY verify_fixture, verify_fixture_site, verify_fixture_continuation
