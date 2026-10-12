/* Fiber context switch, ARM EABI hard-float (handheld).
 *   void port_ctx_switch(void** save_sp, void* load_sp);
 * Saves r4-r11, lr and d8-d15 (the AAPCS callee-saved set) on the current
 * stack, stores sp in *save_sp, switches to load_sp and restores the same
 * layout. A new fiber's stack is pre-built by ultra_os.c so the final pop
 * loads pc = port_fiber_start. */
    .syntax unified
    .arm
    .fpu vfpv3-d16
    .text
    .globl port_ctx_switch
    .type port_ctx_switch, %function
port_ctx_switch:
    push {r4-r11, lr}
    vpush {d8-d15}
    str sp, [r0]
    mov sp, r1
    vpop {d8-d15}
    pop {r4-r11, pc}
    .size port_ctx_switch, .-port_ctx_switch
    .section .note.GNU-stack,"",%progbits
