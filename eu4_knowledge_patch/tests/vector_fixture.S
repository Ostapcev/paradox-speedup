.intel_syntax
.text

/* A callable fixture with the same register contract as the EU4 scan. */
.globl vector_fixture
vector_fixture:
    cmp rcx, rdx
    je vector_fixture_out
    mov rax, rdx
    mov rdx, rcx
.globl vector_fixture_site
vector_fixture_site:
    mov rcx, qword ptr [rdx]
    shr rcx, 32
    cmp cx, r8w
    je vector_fixture_found
    add rdx, 8
    cmp rdx, rax
    jne vector_fixture_site
    jmp vector_fixture_out
.globl vector_fixture_found
vector_fixture_found:
    mov eax, 1
    ret
.globl vector_fixture_out
vector_fixture_out:
    xor eax, eax
    ret
