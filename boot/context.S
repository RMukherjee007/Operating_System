/* FulmenOS AArch64 Context Switch & EL1 <-> EL0 Transitions */

.section ".bss"
.balign 16
el0_return_ctx:
    .skip 104

.section ".text"
.global cpu_switch_to
.global task_trampoline
.global enter_el0
.global exit_el0

/*
 * void cpu_switch_to(struct cpu_context *prev, struct cpu_context *next)
 * x0 = prev, x1 = next
 */
cpu_switch_to:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    mov     x9, sp
    stp     x29, x9,  [x0, #80]
    str     x30,      [x0, #96]

    ldp     x19, x20, [x1, #0]
    ldp     x21, x22, [x1, #16]
    ldp     x23, x24, [x1, #32]
    ldp     x25, x26, [x1, #48]
    ldp     x27, x28, [x1, #64]
    ldp     x29, x9,  [x1, #80]
    mov     sp, x9
    ldr     x30,      [x1, #96]
    ret

/*
 * Trampoline for newly spawned kernel threads.
 * x19 = entry function pointer, x20 = void *arg.
 */
task_trampoline:
    msr     daifclr, #2
    mov     x0, x20
    blr     x19
    bl      task_exit
1:  wfe
    b       1b

/*
 * int enter_el0(uint64_t entry, uint64_t user_sp, uint64_t argc, uint64_t argv)
 * x0 = entry, x1 = user_sp, x2 = argc, x3 = argv
 */
enter_el0:
    adrp    x9, el0_return_ctx
    add     x9, x9, :lo12:el0_return_ctx
    stp     x19, x20, [x9, #0]
    stp     x21, x22, [x9, #16]
    stp     x23, x24, [x9, #32]
    stp     x25, x26, [x9, #48]
    stp     x27, x28, [x9, #64]
    mov     x10, sp
    stp     x29, x10, [x9, #80]
    str     x30,      [x9, #96]

    /* Configure EL0t execution state (SPSR_EL1 = 0, IRQs enabled in EL0) */
    msr     elr_el1, x0
    msr     sp_el0,  x1
    msr     spsr_el1, xzr

    /* Pass argc/argv to user _start in x0/x1 and scrub remaining GPRs */
    mov     x0, x2
    mov     x1, x3
    mov     x2,  xzr
    mov     x3,  xzr
    mov     x4,  xzr
    mov     x5,  xzr
    mov     x6,  xzr
    mov     x7,  xzr
    mov     x8,  xzr
    mov     x9,  xzr
    mov     x10, xzr
    mov     x11, xzr
    mov     x12, xzr
    mov     x13, xzr
    mov     x14, xzr
    mov     x15, xzr
    mov     x16, xzr
    mov     x17, xzr
    mov     x18, xzr
    mov     x19, xzr
    mov     x20, xzr
    mov     x21, xzr
    mov     x22, xzr
    mov     x23, xzr
    mov     x24, xzr
    mov     x25, xzr
    mov     x26, xzr
    mov     x27, xzr
    mov     x28, xzr
    mov     x29, xzr
    mov     x30, xzr
    eret

/*
 * void exit_el0(int exit_code)
 * Restores EL1 kernel context saved by enter_el0 and returns exit_code.
 */
exit_el0:
    adrp    x9, el0_return_ctx
    add     x9, x9, :lo12:el0_return_ctx
    ldp     x19, x20, [x9, #0]
    ldp     x21, x22, [x9, #16]
    ldp     x23, x24, [x9, #32]
    ldp     x25, x26, [x9, #48]
    ldp     x27, x28, [x9, #64]
    ldp     x29, x10, [x9, #80]
    mov     sp, x10
    ldr     x30,      [x9, #96]

    /* Re-enable IRQs since we arrived via el0_sync trap */
    msr     daifclr, #2
    ret
