.intel_syntax
.text

# uint64_t add_search_fixture(entries=rcx, count=rdx, key=r8d)
.globl add_search_fixture
add_search_fixture:
    mov rax, rcx
    push rdi
    mov edi, r8d
    xor ecx, ecx
    movabs r8, 0x123456789abcdef0
    movabs r9, 0x1020304050607080
    movabs r10, 0xfedcba9876543210
.globl add_search_loop
add_search_loop:
    .byte 0x39, 0x38, 0x0F, 0x84, 0x96, 0x00, 0x00, 0x00
    .byte 0x48, 0xFF, 0xC1, 0x48, 0x83, 0xC0, 0x08, 0x48, 0x3B, 0xCA, 0x7C, 0xEC
.globl add_search_fixture_not_found
add_search_fixture_not_found:
    jmp add_search_fixture_validate
.globl add_search_fixture_found
add_search_fixture_found:
    jmp add_search_fixture_validate
add_search_fixture_validate:
    movabs r11, 0x123456789abcdef0
    cmp r8, r11
    jne add_search_fixture_failed
    movabs r11, 0x1020304050607080
    cmp r9, r11
    jne add_search_fixture_failed
    movabs r11, 0xfedcba9876543210
    cmp r10, r11
    jne add_search_fixture_failed
    cmp rax, qword ptr [rip + search_fixture_expected_pointer]
    jne add_search_fixture_failed
    mov rax, rcx
    pop rdi
    ret
add_search_fixture_failed:
    mov rax, -1
    pop rdi
    ret

# uint64_t subtract_search_fixture(entries=rcx, count=rdx, key=r8d)
.globl subtract_search_fixture
subtract_search_fixture:
    push rdi
    mov edi, r8d
    xor eax, eax
    movabs r8, 0x123456789abcdef0
    movabs r9, 0x1020304050607080
    movabs r10, 0xfedcba9876543210
.globl subtract_search_loop
subtract_search_loop:
    .byte 0x39, 0x39, 0x0F, 0x84, 0x95, 0x00, 0x00, 0x00
    .byte 0x48, 0xFF, 0xC0, 0x48, 0x83, 0xC1, 0x08, 0x48, 0x3B, 0xC2, 0x7C, 0xEC
.globl subtract_search_fixture_not_found
subtract_search_fixture_not_found:
    jmp subtract_search_fixture_validate
.globl subtract_search_fixture_found
subtract_search_fixture_found:
    jmp subtract_search_fixture_validate
subtract_search_fixture_validate:
    movabs r11, 0x123456789abcdef0
    cmp r8, r11
    jne subtract_search_fixture_failed
    movabs r11, 0x1020304050607080
    cmp r9, r11
    jne subtract_search_fixture_failed
    movabs r11, 0xfedcba9876543210
    cmp r10, r11
    jne subtract_search_fixture_failed
    cmp rcx, qword ptr [rip + search_fixture_expected_pointer]
    jne subtract_search_fixture_failed
    pop rdi
    ret
subtract_search_fixture_failed:
    mov rax, -1
    pop rdi
    ret

.data
.globl search_fixture_expected_pointer
search_fixture_expected_pointer:
    .quad 0
