.intel_syntax
.text

.globl vector_original
vector_original:
    cmp rcx, rdx
    je vector_original_not_found
vector_original_loop:
    mov rax, qword ptr [rcx]
    shr rax, 32
    cmp ax, r8w
    je vector_original_found
    add rcx, 8
    cmp rcx, rdx
    jne vector_original_loop
vector_original_not_found:
    xor eax, eax
    ret
vector_original_found:
    mov eax, 1
    ret

.globl vector_optimized
vector_optimized:
    cmp rcx, rdx
    je vector_optimized_not_found
vector_optimized_loop:
    cmp word ptr [rcx + 4], r8w
    je vector_optimized_found
    add rcx, 8
    cmp rcx, rdx
    jne vector_optimized_loop
vector_optimized_not_found:
    xor eax, eax
    ret
vector_optimized_found:
    mov eax, 1
    ret

.globl vector_unrolled
vector_unrolled:
    mov rax, rdx
    sub rax, rcx
    shr rax, 4
    shl rax, 4
    add rax, rcx
    cmp rcx, rax
    je vector_unrolled_tail
vector_unrolled_loop:
    cmp word ptr [rcx + 4], r8w
    je vector_unrolled_found
    cmp word ptr [rcx + 12], r8w
    je vector_unrolled_found
    add rcx, 16
    cmp rcx, rax
    jne vector_unrolled_loop
vector_unrolled_tail:
    cmp rcx, rdx
    je vector_unrolled_not_found
    cmp word ptr [rcx + 4], r8w
    je vector_unrolled_found
vector_unrolled_not_found:
    xor eax, eax
    ret
vector_unrolled_found:
    mov eax, 1
    ret

.globl linked_original
linked_original:
    test rcx, rcx
    je linked_original_not_found
linked_original_loop:
    mov rax, qword ptr [rcx]
    shr rax, 32
    cmp ax, dx
    je linked_original_found
    mov rcx, qword ptr [rcx + 16]
    test rcx, rcx
    jne linked_original_loop
linked_original_not_found:
    xor eax, eax
    ret
linked_original_found:
    mov eax, 1
    ret

.globl linked_optimized
linked_optimized:
    test rcx, rcx
    je linked_optimized_not_found
linked_optimized_loop:
    cmp word ptr [rcx + 4], dx
    je linked_optimized_found
    mov rcx, qword ptr [rcx + 16]
    test rcx, rcx
    jne linked_optimized_loop
linked_optimized_not_found:
    xor eax, eax
    ret
linked_optimized_found:
    mov eax, 1
    ret
