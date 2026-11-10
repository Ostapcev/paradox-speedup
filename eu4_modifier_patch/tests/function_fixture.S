.intel_syntax
.text

.globl modifier_fixture
modifier_fixture:
    .byte 0x40, 0x55, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x48
    mov rax, rcx
    mov edx, dword ptr [rdx]
    add dword ptr [rax], edx
    add rsp, 0x48
    pop r13
    pop rbp
    ret

.globl call_modifier_fixture
call_modifier_fixture:
    sub rsp, 0x28
    call modifier_fixture
.globl fixture_return_site
fixture_return_site:
    add rsp, 0x28
    ret

.globl modifier_copy_fixture
modifier_copy_fixture:
    .byte 0x40, 0x53, 0x55, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x02
    cmp rax, qword ptr [rdx]
    jne modifier_copy_failed
    mov rax, rcx
    add rsp, 0x20
    pop rdi
    pop rbp
    pop rbx
    ret
modifier_copy_failed:
    xor eax, eax
    add rsp, 0x20
    pop rdi
    pop rbp
    pop rbx
    ret

.globl call_modifier_copy_fixture
call_modifier_copy_fixture:
    sub rsp, 0x28
    call modifier_copy_fixture
.globl modifier_copy_return_site
modifier_copy_return_site:
    add rsp, 0x28
    ret

.globl three_arg_fixture
three_arg_fixture:
    .byte 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55
    mov rax, r8
    pop rbp
    ret

.globl call_three_arg_fixture
call_three_arg_fixture:
    sub rsp, 0x28
    call three_arg_fixture
.globl three_arg_return_site
three_arg_return_site:
    add rsp, 0x28
    ret

.globl cache_reset_fixture
cache_reset_fixture:
    .byte 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9
    cmp rbx, rcx
    jne cache_reset_failed
    mov rax, rbx
    add rsp, 0x20
    pop rbx
    ret
cache_reset_failed:
    xor eax, eax
    add rsp, 0x20
    pop rbx
    ret

.globl call_cache_reset_fixture
call_cache_reset_fixture:
    sub rsp, 0x28
    call cache_reset_fixture
.globl cache_reset_return_site
cache_reset_return_site:
    add rsp, 0x28
    ret

.globl cache_rebuild_fixture
cache_rebuild_fixture:
    .byte 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55
    mov rax, rdx
    pop rbp
    ret

.globl call_cache_rebuild_fixture
call_cache_rebuild_fixture:
    sub rsp, 0x28
    call cache_rebuild_fixture
.globl cache_rebuild_return_site
cache_rebuild_return_site:
    add rsp, 0x28
    ret
