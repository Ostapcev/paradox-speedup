.intel_syntax
.text

/* invoke_with_rdi(function, argument, current_country) */
.globl invoke_with_rdi
invoke_with_rdi:
    push rdi
    sub rsp, 0x20
    mov rax, rcx
    mov rcx, rdx
    mov rdi, r8
    call rax
    add rsp, 0x20
    pop rdi
    ret
