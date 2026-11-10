.intel_syntax
.text

.globl trigger_entry_fixture
.globl trigger_entry_fixture_site
.globl trigger_entry_fixture_continuation
trigger_entry_fixture:
    push rbp
    mov rbp, rsp
    push rdi
    sub rsp, 0x68
    mov rdi, rcx
    mov r14, qword ptr [rdi]
    mov rax, qword ptr [rdi + 8]
    mov qword ptr [rsp + 0x38], rax
    /* Match EU4 0x97fa13: the filter is on the stack, while RSI still
     * contains a stale value until the later instruction at 0x97fa21. */
    mov rsi, 0xdeadbeef
    mov rax, qword ptr [rdi + 16]
    mov qword ptr [rsp + 0x40], rax
    mov rax, qword ptr [rdi + 24]
    mov rcx, qword ptr [rdi + 32]
    mov rdx, qword ptr [rdi + 40]
    mov r8, qword ptr [rdi + 48]
    mov r9, qword ptr [rdi + 56]
    mov r10, qword ptr [rdi + 64]
    mov r11, qword ptr [rdi + 72]
trigger_entry_fixture_site:
    .byte 0x4C, 0x8B, 0x3D, 0x66, 0xC3, 0xAA, 0x01
trigger_entry_fixture_continuation:
    mov qword ptr [rdi + 80], rax
    mov qword ptr [rdi + 88], rcx
    mov qword ptr [rdi + 96], rdx
    mov qword ptr [rdi + 104], r8
    mov qword ptr [rdi + 112], r9
    mov qword ptr [rdi + 120], r10
    mov qword ptr [rdi + 128], r11
    mov qword ptr [rdi + 136], r15
    mov qword ptr [rdi + 144], rsp
    mov rax, qword ptr [rdi + 80]
    add rsp, 0x68
    pop rdi
    pop rbp
    ret
