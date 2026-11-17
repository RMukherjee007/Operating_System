/* FulmenOS AArch64 Exception Vector Table (VBAR_EL1) */

.section ".text"
.global vector_table

/* Save 272-byte struct pt_regs onto SP_EL1 */
.macro kernel_entry
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]

    mrs     x21, sp_el0
    mrs     x22, elr_el1
    mrs     x23, spsr_el1
    stp     x30, x21, [sp, #240]
    stp     x22, x23, [sp, #256]
.endm

/* Restore 272-byte struct pt_regs from SP_EL1 and eret */
.macro kernel_exit
    ldp     x22, x23, [sp, #256]
    ldp     x30, x21, [sp, #240]
    msr     elr_el1, x22
    msr     spsr_el1, x23
    msr     sp_el0, x21

    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    add     sp, sp, #272
    eret
.endm

.macro ventry label
    .balign 128
    b       \label
.endm

.macro invalid_handler type
    kernel_entry
    mov     x0, #\type
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    mrs     x3, far_el1
    bl      bad_exception_handler
1:  wfe
    b       1b
.endm

.balign 2048
vector_table:
    /* Current EL with SP0 */
    ventry  sync_invalid_el1t
    ventry  irq_invalid_el1t
    ventry  fiq_invalid_el1t
    ventry  error_invalid_el1t

    /* Current EL with SPx (EL1h) */
    ventry  el1_sync
    ventry  el1_irq
    ventry  fiq_invalid_el1h
    ventry  error_invalid_el1h

    /* Lower EL using AArch64 (EL0 -> EL1) */
    ventry  el0_sync
    ventry  el0_irq
    ventry  fiq_invalid_el0_64
    ventry  error_invalid_el0_64

    /* Lower EL using AArch32 */
    ventry  sync_invalid_el0_32
    ventry  irq_invalid_el0_32
    ventry  fiq_invalid_el0_32
    ventry  error_invalid_el0_32

el1_sync:
    kernel_entry
    mov     x0, sp
    mrs     x1, esr_el1
    mrs     x2, far_el1
    mov     x3, #0
    bl      sync_exception_handler
    kernel_exit

el1_irq:
    kernel_entry
    mov     x0, sp
    bl      irq_handler
    kernel_exit

el0_sync:
    kernel_entry
    mov     x0, sp
    mrs     x1, esr_el1
    mrs     x2, far_el1
    mov     x3, #1
    bl      sync_exception_handler
    kernel_exit

el0_irq:
    kernel_entry
    mov     x0, sp
    bl      irq_handler
    kernel_exit

sync_invalid_el1t:   invalid_handler 0
irq_invalid_el1t:    invalid_handler 1
fiq_invalid_el1t:    invalid_handler 2
error_invalid_el1t:  invalid_handler 3
fiq_invalid_el1h:    invalid_handler 6
error_invalid_el1h:  invalid_handler 7
fiq_invalid_el0_64:  invalid_handler 10
error_invalid_el0_64:invalid_handler 11
sync_invalid_el0_32: invalid_handler 12
irq_invalid_el0_32:  invalid_handler 13
fiq_invalid_el0_32:  invalid_handler 14
error_invalid_el0_32:invalid_handler 15
