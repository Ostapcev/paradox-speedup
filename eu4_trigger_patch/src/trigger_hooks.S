.intel_syntax
.text

/* Batch entry at EU4 RVA 0x97fa13. The displaced instruction loads R15 with
 * DAT_14242bd80. trigger_batch_begin returns that same value in RAX. */
.globl trigger_batch_entry_begin
.extern trigger_batch_begin
trigger_batch_entry_begin:
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    sub rsp, 0x28
    mov rcx, r14
    /* RSI is only loaded from [rsp+0x38] at 0x97fa21, after this hook.
     * Read the filter from its stack slot; at 0x97fa13 RSI is stale. */
    mov rdx, qword ptr [rsp + 0x98]
    mov r8, qword ptr [rsp + 0xa0]
    call qword ptr [rip + trigger_batch_entry_target]
    mov r15, rax
    add rsp, 0x28
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    .byte 0xE9
.globl trigger_batch_entry_continuation_displacement
trigger_batch_entry_continuation_displacement:
    .long 0
    .p2align 3, 0x90
trigger_batch_entry_target:
    .quad trigger_batch_begin
.globl trigger_batch_entry_end
trigger_batch_entry_end:

/* Original evaluation first, shadow comparison second. RCX is trigger+0x30,
 * hence the option address is RCX-0x30. No game result is replaced. */
.globl trigger_first_hook
.extern trigger_original_target
.extern trigger_first_verify
.extern trigger_qpc_now
trigger_first_hook:
    sub rsp, 0x58
    mov qword ptr [rsp + 0x20], rcx
    mov qword ptr [rsp + 0x40], rdx
    call trigger_qpc_now
    mov qword ptr [rsp + 0x38], rax
    mov rcx, qword ptr [rsp + 0x20]
    mov rdx, qword ptr [rsp + 0x40]
    call qword ptr [rip + trigger_original_target]
    mov qword ptr [rsp + 0x28], rax
    movsd qword ptr [rsp + 0x30], xmm0
    mov rcx, qword ptr [rsp + 0x20]
    sub rcx, 0x30
    movzx edx, al
    mov r8, qword ptr [rsp + 0x38]
    call trigger_first_verify
    mov rax, qword ptr [rsp + 0x28]
    movsd xmm0, qword ptr [rsp + 0x30]
    add rsp, 0x58
    ret

.globl trigger_second_hook
.extern trigger_second_verify
trigger_second_hook:
    sub rsp, 0x58
    mov qword ptr [rsp + 0x20], rcx
    mov qword ptr [rsp + 0x40], rdx
    call trigger_qpc_now
    mov qword ptr [rsp + 0x38], rax
    mov rcx, qword ptr [rsp + 0x20]
    mov rdx, qword ptr [rsp + 0x40]
    call qword ptr [rip + trigger_original_target]
    mov qword ptr [rsp + 0x28], rax
    movsd qword ptr [rsp + 0x30], xmm0
    mov rcx, qword ptr [rsp + 0x20]
    sub rcx, 0x30
    movzx edx, al
    mov r8, qword ptr [rsp + 0x38]
    call trigger_second_verify
    mov rax, qword ptr [rsp + 0x28]
    movsd xmm0, qword ptr [rsp + 0x30]
    add rsp, 0x58
    ret

/* Normal loop exit at RVA 0x97fc5f calls FUN_140974d00. Preserve its result. */
.globl trigger_finish_hook
.extern trigger_finish_original
.extern trigger_batch_end
trigger_finish_hook:
    sub rsp, 0x58
    call qword ptr [rip + trigger_finish_original]
    mov qword ptr [rsp + 0x28], rax
    movsd qword ptr [rsp + 0x30], xmm0
    call trigger_batch_end
    mov rax, qword ptr [rsp + 0x28]
    movsd xmm0, qword ptr [rsp + 0x30]
    add rsp, 0x58
    ret
