/*
 * Cortex-M fault entry + synthetic unwind frame.
 *
 * On exception entry the hardware pushes R0-R3, R12, LR, PC, xPSR (and the
 * FP extension S0-S15/FPSCR when EXC_RETURN bit 4 is clear).  R4-R11 are not
 * stacked, so the handler saves them itself.
 *
 * libgcc's ARM EHABI unwinder walks from one function's stack frame to its
 * caller by executing the unwind opcodes stored in .ARM.exidx.  A fault
 * handler has no ordinary call chain to the interrupted function, so this
 * file manufactures the missing link: the `bl fault_handler_c` below is
 * covered by a hand-written .fnstart/.unwind_raw entry that makes the
 * unwinder pop the hardware exception frame as if it were a normal caller
 * frame.
 *
 * Register contract with the raw unwind opcodes:
 *   r11 = F, base of the hardware core frame (R0..xPSR)
 *   r10 = saved R4-R8/R10/R11 block on the handler stack
 *   r9  = SP at the moment of the fault (F + frame size + alignment pad)
 *
 * The r9 value cannot be restored from the exception frame (it is not part
 * of the hardware-stacked set), and the unwind opcodes consume it last.  It
 * is only used as the SP for the interrupted frame; GCC never uses r9 as a
 * frame pointer, so losing its interrupted value does not affect the walk.
 *
 * See README.md in this directory for the full design and limitations.
 */

    .syntax unified
    .thumb

    .section .after_vectors,"ax",%progbits

/* ------------------------------------------------------------------ */
/* Vector entries.  Each records the fault id in r2 and falls through   */
/* to the common entry.                                                 */
/* ------------------------------------------------------------------ */

    .global HardFault_Handler
    .type   HardFault_Handler, %function
    .thumb_func
HardFault_Handler:
    movs    r2, #0
    b       .Lfault_common
    .size   HardFault_Handler, . - HardFault_Handler

    .global MemManage_Handler
    .type   MemManage_Handler, %function
    .thumb_func
MemManage_Handler:
    movs    r2, #1
    b       .Lfault_common
    .size   MemManage_Handler, . - MemManage_Handler

    .global BusFault_Handler
    .type   BusFault_Handler, %function
    .thumb_func
BusFault_Handler:
    movs    r2, #2
    b       .Lfault_common
    .size   BusFault_Handler, . - BusFault_Handler

    .global UsageFault_Handler
    .type   UsageFault_Handler, %function
    .thumb_func
UsageFault_Handler:
    movs    r2, #3
    b       .Lfault_common
    .size   UsageFault_Handler, . - UsageFault_Handler

/* ------------------------------------------------------------------ */
/* Common entry.                                                       */
/*                                                                     */
/* In:  LR = EXC_RETURN                                                */
/*      r2 = fault id                                                  */
/* Out: calls fault_handler_c(F, EXC_RETURN, fault id, saved regs)      */
/* ------------------------------------------------------------------ */

    .thumb_func
.Lfault_common:
    /*
     * Pick the stack the exception frame was pushed on:
     *   EXC_RETURN bit 2: 0 = MSP, 1 = PSP
     *
     * The CPU already updated that stack pointer to the frame base, so
     * the value read here is F (lowest address of R0), not the
     * pre-exception SP.
     */
    tst     lr, #4
    ite     eq
    mrseq   r12, msp
    mrsne   r12, psp             /* r12 = F, the hardware frame base */

    /*
     * Save the callee-saved registers the hardware did not stack.  The
     * block lives on the handler stack, above the C handler's own frame.
     * Layout must match callee_saved_t: r4,r5,r6,r7,r8,r10,r11,r9.
     */
    sub     sp, sp, #32
    stmia   sp, {r4-r8, r10, r11}
    str     r9, [sp, #28]
    mov     r10, sp             /* saved-register block */

    /*
     * Compute the interrupted SP: F + frame size, plus the 4-byte pad the
     * hardware inserted when the pre-exception SP was not 8-byte aligned
     * (recorded in stacked xPSR bit 9).
     */
    ldr     r3, [r12, #0x1C]    /* stacked xPSR */
    ubfx    r9, r3, #9, #1
    lsl     r9, r9, #2          /* 0 or 4 */

    tst     lr, #0x10
    ite     eq
    addeq   r9, r9, #0x68
    addne   r9, r9, #0x20
    add     r9, r9, r12         /* r9 = interrupted SP */

    mov     r11, r12            /* r11 = hardware core frame base */
    mov     r0, r12             /* arg 0: exception_frame_t * */
    mov     r1, lr              /* arg 1: EXC_RETURN */
    mov     r3, r10             /* arg 3: saved R4-R11 */

    /*
     * The unwinder reaches this call site through fault_handler_c's saved
     * LR.  The hand-written unwind record below describes the exception
     * frame as the caller frame of this "function".
     */
    .fnstart
.Lfault_bl:
    bl      fault_handler_c
    .unwind_raw 0, 0x9b, 0xb1, 0x0f, 0x8d, 0x00, 0x9a, 0x80, 0xdf, 0x99, 0xb0
    .fnend

.Lfault_hang:
    b       .Lfault_hang
