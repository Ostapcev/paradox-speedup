.intel_syntax
.text

/*
 * EU4 parallel-for wait-loop entry at RVA 0x179ee4.
 * RDI is the stack task object and its shared state pointer is at +0x10.
 * Only the already-drained branch (state+0x50 == 1) uses efficient waiting.
 * The original index distribution and task callbacks are never changed.
 */
.globl knowledge_scheduler_wait_begin
.extern knowledge_scheduler_efficient_wait
knowledge_scheduler_wait_begin:
    mov rdx, qword ptr [rdi + 0x10]
    cmp byte ptr [rdx + 0x50], 1
    jne knowledge_scheduler_wait_original
    mov rcx, rdx
    sub rsp, 0x20
    call qword ptr [rip + knowledge_scheduler_wait_target]
    add rsp, 0x20
    .byte 0xE9
.globl knowledge_scheduler_wait_top_displacement
knowledge_scheduler_wait_top_displacement:
    .long 0

knowledge_scheduler_wait_original:
    /* Original instructions 0x179ee4..0x179ef1. */
    .byte 0x0F, 0x57, 0xC0
    .byte 0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20
    .byte 0x48, 0x8B, 0x5F, 0x18
    .byte 0xE9
.globl knowledge_scheduler_wait_resume_displacement
knowledge_scheduler_wait_resume_displacement:
    .long 0
    .p2align 3, 0x90
knowledge_scheduler_wait_target:
    .quad knowledge_scheduler_efficient_wait
.globl knowledge_scheduler_wait_end
knowledge_scheduler_wait_end:
