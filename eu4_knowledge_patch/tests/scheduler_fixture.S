.intel_syntax
.text

/* FixtureArgs begins with the same fields used by the hook: state +0x10,
 * control block +0x18. RSP is 16-byte aligned at the patched site. */
.globl scheduler_fixture
scheduler_fixture:
    push r12
    push rbx
    push rdi
    sub rsp, 0x40
    mov r12, rcx
    mov rdi, rcx
.globl scheduler_fixture_site
scheduler_fixture_site:
    .byte 0x0F, 0x57, 0xC0
    .byte 0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20
    .byte 0x48, 0x8B, 0x5F, 0x18
.globl scheduler_fixture_resume
scheduler_fixture_resume:
    mov qword ptr [r12 + 0x28], rbx
    mov rax, qword ptr [rsp + 0x20]
    mov qword ptr [r12 + 0x30], rax
    mov rax, qword ptr [rsp + 0x28]
    mov qword ptr [r12 + 0x38], rax
    jmp scheduler_fixture_epilogue

.globl scheduler_fixture_top
scheduler_fixture_top:
    mov qword ptr [r12 + 0x20], 1

scheduler_fixture_epilogue:
    mov qword ptr [r12 + 0x40], rsp
    mov qword ptr [r12 + 0x48], rdi
    mov qword ptr [r12 + 0x50], r12
    add rsp, 0x40
    pop rdi
    pop rbx
    pop r12
    ret
