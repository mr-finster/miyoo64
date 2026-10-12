/* hh_ctx.S: stack switching for the game-thread coroutines (hh_os.c).
 *
 *   void hh_ctx_switch(hh_ctx *from, hh_ctx *to);
 *
 * Saves the callee-saved registers of the caller on its own stack, stores the stack
 * pointer in *from, loads the one in *to and restores that context's registers. A fresh
 * context (hh_ctx_init in hh_os.c) "returns" into hh_ctx_trampoline, which calls
 * hh_thread_main() and never returns. The FP control registers (MXCSR / FPSCR) are
 * shared by all contexts and are not switched.
 */

#if defined(__i386__)
        .text
        .globl  hh_ctx_switch
        .type   hh_ctx_switch, @function
hh_ctx_switch:
        movl    4(%esp), %eax           /* from */
        movl    8(%esp), %edx           /* to */
        pushl   %ebp
        pushl   %ebx
        pushl   %esi
        pushl   %edi
        movl    %esp, (%eax)
        movl    (%edx), %esp
        popl    %edi
        popl    %esi
        popl    %ebx
        popl    %ebp
        ret
        .size   hh_ctx_switch, .-hh_ctx_switch

        /* Entered by hh_ctx_switch's ret with %esp 16-byte aligned. */
        .globl  hh_ctx_trampoline
        .type   hh_ctx_trampoline, @function
hh_ctx_trampoline:
        xorl    %ebp, %ebp
        call    hh_thread_main
        ud2
        .size   hh_ctx_trampoline, .-hh_ctx_trampoline

#elif defined(__arm__)
        .syntax unified
        .arm
        .fpu    neon-vfpv4
        .text
        .globl  hh_ctx_switch
        .type   hh_ctx_switch, %function
hh_ctx_switch:                          @ r0 = from, r1 = to
        push    {r4-r12, lr}            @ r12 only keeps the frame 8-byte aligned
        vpush   {d8-d15}
        str     sp, [r0]
        ldr     sp, [r1]
        vpop    {d8-d15}
        pop     {r4-r12, pc}
        .size   hh_ctx_switch, .-hh_ctx_switch

        .globl  hh_ctx_trampoline
        .type   hh_ctx_trampoline, %function
hh_ctx_trampoline:
        mov     fp, #0
        mov     lr, #0
        bl      hh_thread_main
        udf     #0
        .size   hh_ctx_trampoline, .-hh_ctx_trampoline
#else
#error "hh_ctx.S: unsupported architecture"
#endif

        .section .note.GNU-stack,"",%progbits
