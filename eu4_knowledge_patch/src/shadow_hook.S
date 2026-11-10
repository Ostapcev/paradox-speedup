.intel_syntax
.text

/*
 * Transparent wrapper for the CALL at EU4 RVA 0x93ff47.
 * RDI is the current CCountry at this site. The original target and all of
 * its arguments run first; verification cannot alter the returned RAX/XMM0.
 */
.globl knowledge_shadow_hook
.extern knowledge_original_target
.extern knowledge_shadow_verify
knowledge_shadow_hook:
    sub rsp, 0x58
    mov qword ptr [rsp + 0x20], rdi
    call qword ptr [rip + knowledge_original_target]
    mov qword ptr [rsp + 0x28], rax
    movsd qword ptr [rsp + 0x30], xmm0
    mov rcx, qword ptr [rsp + 0x20]
    call knowledge_shadow_verify
    mov rax, qword ptr [rsp + 0x28]
    movsd xmm0, qword ptr [rsp + 0x30]
    add rsp, 0x58
    ret
