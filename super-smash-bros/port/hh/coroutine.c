/* coroutine.c -- the game's N64 OS threads run as coroutines on one host
 * thread (the BattleShip model, see bs/n64_stubs.c). musl has no ucontext,
 * so the switch is a few lines of assembly for each target: armv7 (handheld,
 * register layout from BattleShip's coroutine_armv7.S, MIT) and i386 (the
 * -m32 desktop proxy).
 *
 * Stacks come from static pools, not malloc: a save state has to capture
 * every coroutine's stack and context at a fixed address (non-PIE binary),
 * so they must live in .bss, inside the save region. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "coroutine.h"
#include "port_log.h"

#if defined(__arm__)
typedef struct { uint32_t r4_r11[8]; uint32_t sp, lr; uint64_t d8_d15[8]; } PortCtx;
#elif defined(__i386__)
typedef struct { uint32_t ebx, esi, edi, ebp, esp; } PortCtx;
#else
#error "coroutine.c: only armv7 and i386 (32-bit) targets are supported"
#endif

struct PortCoroutine {
    PortCtx ctx;        /* the coroutine's own registers while suspended */
    PortCtx caller;     /* whoever resumed it, restored on yield */
    void (*entry)(void *);
    void *arg;
    int finished;
    int pool;           /* which pool the stack came from */
    int slot;
    PortCoroutine *prev;/* coroutine that was current when this one resumed */
};

extern void port_coroutine_swap(PortCtx *save, PortCtx *load);
void port_coroutine_trampoline_c(PortCoroutine *co);

#if defined(__arm__)
__asm__(
    ".syntax unified\n .arm\n .fpu vfpv4\n .text\n .balign 4\n"
    ".globl port_coroutine_swap\n .type port_coroutine_swap, %function\n"
    "port_coroutine_swap:\n"
    "  stmia r0, {r4-r11}\n  str sp, [r0, #32]\n  str lr, [r0, #36]\n"
    "  add r2, r0, #40\n  vstmia r2, {d8-d15}\n"
    "  add r2, r1, #40\n  vldmia r2, {d8-d15}\n"
    "  ldmia r1, {r4-r11}\n  ldr sp, [r1, #32]\n  ldr lr, [r1, #36]\n  bx lr\n"
    ".size port_coroutine_swap, .-port_coroutine_swap\n"
    ".globl port_coroutine_tramp\n .type port_coroutine_tramp, %function\n"
    "port_coroutine_tramp:\n"
    "  mov r11, #0\n  mov lr, #0\n  mov r0, r4\n  bl port_coroutine_trampoline_c\n  udf #0\n"
    ".size port_coroutine_tramp, .-port_coroutine_tramp\n");
#elif defined(__i386__)
__asm__(
    ".text\n .globl port_coroutine_swap\n .type port_coroutine_swap, @function\n"
    "port_coroutine_swap:\n"
    "  movl 4(%esp), %eax\n  movl 8(%esp), %edx\n"
    "  movl %ebx, 0(%eax)\n  movl %esi, 4(%eax)\n  movl %edi, 8(%eax)\n"
    "  movl %ebp, 12(%eax)\n  movl %esp, 16(%eax)\n"
    "  movl 0(%edx), %ebx\n  movl 4(%edx), %esi\n  movl 8(%edx), %edi\n"
    "  movl 12(%edx), %ebp\n  movl 16(%edx), %esp\n  ret\n"
    ".size port_coroutine_swap, .-port_coroutine_swap\n"
    ".globl port_coroutine_tramp\n .type port_coroutine_tramp, @function\n"
    "port_coroutine_tramp:\n"
    "  xorl %ebp, %ebp\n  pushl %ebx\n  call port_coroutine_trampoline_c\n  ud2\n"
    ".size port_coroutine_tramp, .-port_coroutine_tramp\n");
#endif
extern void port_coroutine_tramp(void);

/* ---- static stack pools ---------------------------------------------------
 * N64 stacks are 1-4 KB; 32-bit ARM frames are about the same size, so these
 * are generous. Pool 0: the boot coroutine and the OS service threads.
 * Pool 1: GObj process threads (the game creates and destroys these per
 * scene). Sizes are logged at the high-water mark to tune later. */
#define POOL0_SLOTS 12
#define POOL0_SIZE  (128 * 1024)
#define POOL1_SLOTS 96
#define POOL1_SIZE  (32 * 1024)

static uint8_t sPool0[POOL0_SLOTS][POOL0_SIZE] __attribute__((aligned(16)));
static uint8_t sPool1[POOL1_SLOTS][POOL1_SIZE] __attribute__((aligned(16)));
static PortCoroutine sCo0[POOL0_SLOTS], sCo1[POOL1_SLOTS];
static uint8_t sUsed0[POOL0_SLOTS], sUsed1[POOL1_SLOTS];
static int sHigh1;

static PortCoroutine *sCurrent;  /* NULL = the host (main) thread */

void port_coroutine_init_main(void)
{
    sCurrent = NULL;
}

void port_coroutine_trampoline_c(PortCoroutine *co)
{
    co->entry(co->arg);
    co->finished = 1;
    for (;;) {
        port_coroutine_yield();
    }
}

PortCoroutine *port_coroutine_create(void (*entry)(void *), void *arg, size_t stack_size)
{
    int pool = (stack_size > POOL1_SIZE) ? 0 : 1;
    int n = pool ? POOL1_SLOTS : POOL0_SLOTS;
    uint8_t *used = pool ? sUsed1 : sUsed0;
    int slot;

    for (slot = 0; slot < n && used[slot]; slot++) {}
    if (slot == n) {
        port_log("SSB64: coroutine pool %d exhausted (%d slots)\n", pool, n);
        return NULL;
    }
    used[slot] = 1;
    if (pool == 1 && slot + 1 > sHigh1) {
        sHigh1 = slot + 1;
        if (sHigh1 % 8 == 0) port_log("SSB64: GObj coroutines in use: %d\n", sHigh1);
    }

    PortCoroutine *co = pool ? &sCo1[slot] : &sCo0[slot];
    uint8_t *base = pool ? sPool1[slot] : sPool0[slot];
    size_t size = pool ? POOL1_SIZE : POOL0_SIZE;
    uintptr_t top = ((uintptr_t)(base + size)) & ~(uintptr_t)15;

    memset(co, 0, sizeof(*co));
    co->entry = entry;
    co->arg = arg;
    co->pool = pool;
    co->slot = slot;
#if defined(__arm__)
    co->ctx.r4_r11[0] = (uint32_t)co;          /* r4 -> trampoline arg */
    co->ctx.sp = (uint32_t)top;
    co->ctx.lr = (uint32_t)port_coroutine_tramp;
#else
    /* swap's `ret` pops the trampoline; its push+call then runs with a
     * 16-byte-aligned stack as the i386 SysV ABI expects. */
    uintptr_t sp = top - 16;
    *(uint32_t *)sp = (uint32_t)port_coroutine_tramp;
    co->ctx.esp = (uint32_t)sp;
    co->ctx.ebx = (uint32_t)co;
#endif
    return co;
}

void port_coroutine_destroy(PortCoroutine *co)
{
    if (co == NULL) return;
    if (co == sCurrent) {
        port_log("SSB64: coroutine destroyed while running\n");
        return;
    }
    if (co->pool) sUsed1[co->slot] = 0; else sUsed0[co->slot] = 0;
}

void port_coroutine_resume(PortCoroutine *co)
{
    if (co == NULL || co->finished || co == sCurrent) return;
    co->prev = sCurrent;
    sCurrent = co;
    port_coroutine_swap(&co->caller, &co->ctx);
    sCurrent = co->prev;
}

void port_coroutine_yield(void)
{
    PortCoroutine *co = sCurrent;
    if (co == NULL) return;   /* the host thread cannot yield */
    port_coroutine_swap(&co->ctx, &co->caller);
}

int port_coroutine_is_finished(PortCoroutine *co)
{
    return co == NULL || co->finished;
}

int port_coroutine_in_coroutine(void)
{
    return sCurrent != NULL;
}
