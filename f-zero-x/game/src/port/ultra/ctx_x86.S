/* Fiber context switch, i386 cdecl (desktop proxy).
 *   void port_ctx_switch(void** save_sp, void* load_sp);
 * Saves the callee-saved registers on the current stack, stores its sp in
 * *save_sp, switches to load_sp and pops the same layout. A new fiber's stack
 * is pre-built by ultra_os.c so the final `ret` lands in port_fiber_start. */
    .text
    .globl port_ctx_switch
    .type port_ctx_switch, @function
port_ctx_switch:
    movl 4(%esp), %eax
    movl 8(%esp), %edx
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl %esp, (%eax)
    movl %edx, %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret
    .size port_ctx_switch, .-port_ctx_switch
    .section .note.GNU-stack,"",@progbits
