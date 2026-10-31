/* hh_os.c: libultra's OS on the host -- threads, message queues, events, timers, time,
 * and the kernel loop that plays the N64's interrupt handler and idle thread.
 *
 * Game threads are coroutines on the host's main thread (hh_ctx.S swaps stacks): exactly
 * one runs at a time and nothing is preempted between two OS calls. Scheduling follows
 * libultra: the highest-priority runnable thread runs, FIFO among equals; a running thread
 * keeps the CPU until it blocks, yields, or an OS call readies a thread of strictly higher
 * priority (which then runs first, as __osEnqueueAndYield does).
 *
 * The kernel runs on the host stack when every game thread is blocked, or when an OS call
 * (a "checkpoint") finds an interrupt due: an RSP task to execute, a VI retrace, a timer.
 * It executes the task (renderer or audio mixer), sends due events as messages exactly as
 * libultra's exception handler does, and dispatches: the interrupted thread again, unless
 * a thread of strictly higher priority became runnable.
 *
 * Time: osGetCount() counts at 46.875 MHz but only moves in event steps -- to the time of
 * each retrace, timer or wake-up as it is delivered. Game code in between takes no time,
 * so frame deltas are whole retraces, as on the N64 where frames start at a retrace.
 * Paced (default): an event is delivered once the host clock reaches its time, so game
 * time follows real time, including when the host is too slow for 30 fps. Unpaced
 * (BK_NOPACE=1): an event is delivered only when every thread is blocked, and time jumps
 * straight to it: runs are deterministic and as fast as the host.
 */
#include <ultra64.h>
#include <PR/os.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
extern int sched_yield(void); /* not <sched.h>: the game has a sched.h of its own on the include path */
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <elf.h>
#include "hh_os.h"
#include "gfx/port.h"
#include "savestate.h" /* SAVESTATE_EXCLUDE: host state a state load must not overwrite (port/kit) */

typedef struct { void *sp; } hh_ctx;
void hh_ctx_switch(hh_ctx *from, hh_ctx *to);
void hh_ctx_trampoline(void);
void hh_thread_main(void);

#define HH_MAX_THREADS 16
#define HH_MAX_TIMERS  16
#define HH_MAX_EVENTS  32
#define HH_CANARY_WORDS 64
#define HH_CANARY 0x5AFEC0DEu
/* Paced: more than this many retraces behind the host clock and the excess is dropped
 * (a stall -- file I/O, a menu -- would otherwise replay as a burst of retraces). */
#define HH_MAX_LAG (15ull * HH_VI_COUNTS)

enum { HH_T_FREE = 0 }; /* else OS_STATE_STOPPED / RUNNABLE / RUNNING / WAITING */

typedef struct HhThread {
    OSThread *t;
    void (*entry)(void *);
    void *arg;
    hh_ctx ctx;
    u8 *stack;
    u32 stack_size;
    int ctx_ready;
    int state;
    int dead;
    int idle;               /* OS_PRIORITY_IDLE: the N64 idle loop, never run (the kernel idles) */
    u32 seq;                /* when it became runnable/waiting: FIFO among equal priorities */
    OSMesgQueue *wait_q;    /* blocked in osRecvMesg (wait_send 0) or osSendMesg (1) */
    int wait_send;
    u64 wake_at;            /* hh_os_sleep_until deadline, 0 = none */
    OSPri pri;              /* copy of t->priority, to catch game writes over OSThreads */
    OSId id;
} HhThread;

typedef struct { OSMesgQueue *mq; OSMesg msg; } HhEvent;

int hh_os_trace SAVESTATE_EXCLUDE;

static HhThread g_thr[HH_MAX_THREADS];
static HhThread *g_cur;          /* running game thread; NULL = kernel */
static HhThread *g_interrupted;  /* thread the kernel was entered from at a checkpoint */
static hh_ctx g_kctx SAVESTATE_EXCLUDE; /* the kernel's own host stack: rewritten on every switch */
static u32 g_seq;
static OSIntMask g_intmask = OS_IM_ALL;
static OSTask *g_rsp_task;
static HhEvent g_events[HH_MAX_EVENTS];

static OSTimer *g_timer[HH_MAX_TIMERS];
static u64 g_timer_at[HH_MAX_TIMERS];

static int g_paced SAVESTATE_EXCLUDE = 1;
static u64 g_vnow;               /* the game's clock, in counts */
static u64 g_real_base_ns SAVESTATE_EXCLUDE;       /* host time of count 0 (paced) */
static u64 g_next_vi = HH_VI_COUNTS;
static u32 g_vi_count;
static OSMesgQueue *g_vi_mq;
static OSMesg g_vi_msg;
static u32 g_vi_retrace = 1;
static u32 g_spin;               /* osGetCount calls since the last switch (unpaced) */
static int g_force_advance;      /* unpaced: a thread spins on the count; move time on */

/* Game-thread stacks live in our .bss, inside the 0x80000000-0x80FFFFFF window, so any
 * stack address the game converts to a physical one stays valid. */
#define HH_STACK_POOL (3u << 20)
static u8 g_stack_pool[HH_STACK_POOL] __attribute__((aligned(4096)));
static u32 g_stack_used;

/* ------------------------------------------------------------------------------------ */
/* host time                                                                            */

/* The kernel's own clock_gettime in the vDSO (round 34): every OS call reads the host clock
 * (hh_checkpoint -> hh_event_due), a raw syscall each time -- ~1% of the device frame. At the first
 * read both are timed against each other; the vDSO one is used only if it returns the same clock and
 * costs under half the syscall (a vDSO whose timer the kernel cannot read from user space falls back
 * to the syscall inside). BK_CLOCK=syscall: never. The choice is logged once. */
typedef int (*HhCgt32)(int, void *);
static HhCgt32 hh_vdso_cgt SAVESTATE_EXCLUDE;
static int hh_clock_mode SAVESTATE_EXCLUDE = -1; /* 0 syscall, 1 vDSO */

static u64 hh_host_ns_sys(void)
{
#if defined(SYS_clock_gettime32)
    /* musl 1.2 tries the time64 syscall first; the handheld's 4.14 kernel lacks it (MK64 r24) */
    struct { long s, ns; } t;
    if (syscall(SYS_clock_gettime32, CLOCK_MONOTONIC, &t) == 0)
        return (u64)t.s * 1000000000ull + (u64)t.ns;
#endif
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
    }
}

static u64 hh_host_ns_vdso(void)
{
    struct { long s, ns; } t;
    if (hh_vdso_cgt(CLOCK_MONOTONIC, &t) == 0)
        return (u64)t.s * 1000000000ull + (u64)t.ns;
    return hh_host_ns_sys();
}

#if defined(__linux__) && __SIZEOF_POINTER__ == 4
/* __vdso_clock_gettime (32-bit time) from the vDSO image's dynamic symbols, as musl's __vdsosym */
static void *hh_vdso_sym(const char *want)
{
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)(uintptr_t)getauxval(AT_SYSINFO_EHDR);
    const Elf32_Phdr *ph;
    const Elf32_Shdr *sh;
    uintptr_t bias = 0;
    int i, have = 0;
    if (eh == NULL || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS32)
        return NULL;
    ph = (const Elf32_Phdr *)((const char *)eh + eh->e_phoff);
    for (i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) { bias = (uintptr_t)eh + ph[i].p_offset - ph[i].p_vaddr; have = 1; break; }
    if (!have || eh->e_shoff == 0)
        return NULL;
    sh = (const Elf32_Shdr *)((const char *)eh + eh->e_shoff);
    for (i = 0; i < eh->e_shnum; i++) {
        const Elf32_Sym *sym;
        const char *str;
        unsigned n, k;
        if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum)
            continue;
        sym = (const Elf32_Sym *)((const char *)eh + sh[i].sh_offset);
        str = (const char *)eh + sh[sh[i].sh_link].sh_offset;
        n = sh[i].sh_size / sizeof(Elf32_Sym);
        for (k = 0; k < n; k++)
            if (ELF32_ST_TYPE(sym[k].st_info) == STT_FUNC && sym[k].st_shndx != SHN_UNDEF &&
                strcmp(str + sym[k].st_name, want) == 0)
                return (void *)(bias + sym[k].st_value);
    }
    return NULL;
}
#else
static void *hh_vdso_sym(const char *want) { (void)want; return NULL; }
#endif

/* 256 reads of each clock, the syscall's around the vDSO's; 0 if the vDSO's readings fall outside */
static int hh_clock_trial(u64 *vdso_ns, u64 *sys_ns)
{
    u64 t0, t1, t2, a, b;
    int i;
    t0 = hh_host_ns_sys();
    for (i = 0; i < 256; i++) (void)hh_host_ns_sys();
    t1 = hh_host_ns_sys();
    a = hh_host_ns_vdso();
    for (i = 0; i < 256; i++) (void)hh_host_ns_vdso();
    b = hh_host_ns_vdso();
    t2 = hh_host_ns_sys();
    *vdso_ns = (b - a) / 257;
    *sys_ns = (t1 - t0) / 257;
    return a >= t1 && b <= t2;
}

static void hh_clock_pick(void)
{
    const char *e = getenv("BK_CLOCK");
    u64 v = ~0ull, s = ~0ull, tv, ts;
    int i, same = 1;
    hh_clock_mode = 0;
    if (e != NULL && strcmp(e, "syscall") == 0)
        return;
    hh_vdso_cgt = (HhCgt32)hh_vdso_sym("__vdso_clock_gettime");
    if (hh_vdso_cgt == NULL) {
        fprintf(stderr, "[hh_os] clock: no vDSO clock_gettime, syscall\n");
        return;
    }
    /* the best of three (a device run once caught an interrupt inside the vDSO's 256 reads: 3854 ns) */
    for (i = 0; i < 3; i++) {
        same &= hh_clock_trial(&tv, &ts);
        if (tv < v) v = tv;
        if (ts < s) s = ts;
    }
    hh_clock_mode = same && v * 2 < s;
    fprintf(stderr, "[hh_os] clock: vDSO %u ns/read vs syscall %u ns/read, %s\n", (unsigned)v, (unsigned)s,
            hh_clock_mode ? "vDSO" : "syscall");
}

u64 hh_host_ns(void)
{
    if (hh_clock_mode > 0)
        return hh_host_ns_vdso();
    if (hh_clock_mode < 0) {
        hh_clock_pick();
        if (hh_clock_mode > 0)
            return hh_host_ns_vdso();
    }
    return hh_host_ns_sys();
}

/* 46.875 counts per microsecond = 3/64 per nanosecond */
static u64 hh_real_counts(void) { return (hh_host_ns() - g_real_base_ns) * 3 / 64; }
static u64 hh_counts_to_ns(u64 c) { return c * 64 / 3; }

/* The idle wait to the next event. Any sleep on the Miyoo Mini costs a whole kernel tick, 9.2-9.6 ms however short the
 * request (measured on the Mini Plus), so a sub-retrace gap slept was a frame's worth of time lost. Only the part of a
 * wait above HH_SPIN_NS is slept; the rest is a sched_yield loop on the monotonic clock, to the same deadline (Perfect
 * Dark's pacer, pdmain.c). On a kernel whose sleeps are exact the deadline, and so the timing, is the same.
 * BK_SPINWAIT=0: sleep the whole wait, as before. The PERF line's sleep bucket gets the time actually waited. */
#define HH_SPIN_NS 12000000ull
static void hh_sleep_ns(u64 ns)
{
    static int spin SAVESTATE_EXCLUDE = -1;
    struct timespec ts;
    const u64 t0 = hh_host_ns(), until = t0 + ns;
    u64 now = t0;
    if (spin < 0) {
        const char *e = getenv("BK_SPINWAIT");
        spin = !(e && e[0] == '0');
    }
    while (now < until) {
        const u64 left = until - now;
        if (!spin || left > HH_SPIN_NS) {
            const u64 s = spin ? left - HH_SPIN_NS : left;
            ts.tv_sec = (time_t)(s / 1000000000ull);
            ts.tv_nsec = (long)(s % 1000000000ull);
            nanosleep(&ts, NULL);
        } else {
            sched_yield();
        }
        now = hh_host_ns();
    }
    PORT_PROF_ADD(PROF_SLEEP, (uint32_t)((now - t0) / 1000u)); /* the PERF line's sleep bucket (hh_gfx.c): actual */
}

void hh_os_rebase_clock(void)
{
    g_real_base_ns = hh_host_ns() - hh_counts_to_ns(g_vnow);
}

/* Host time the game should not see (round 25: a save's write + fsync, ~0.25 s on the device):
 * shift the clock by the time between begin and end, keeping any lag it already had. A state load
 * in between re-bases the clock itself (hh_os_state_loaded), and then this does nothing. */
static u64 g_pause_t0 SAVESTATE_EXCLUDE;
static int g_rebased SAVESTATE_EXCLUDE;
void hh_os_pause_begin(void) { g_pause_t0 = hh_host_ns(); g_rebased = 0; }
void hh_os_pause_end(void)
{
    if (!g_rebased && g_pause_t0 != 0)
        g_real_base_ns += hh_host_ns() - g_pause_t0;
    g_pause_t0 = 0;
}

u64 hh_os_now(void) { return g_vnow; }

/* ------------------------------------------------------------------------------------ */
/* diagnostics                                                                          */

static const char *hh_state_name(int s)
{
    switch (s) {
    case HH_T_FREE: return "free";
    case OS_STATE_STOPPED: return "stopped";
    case OS_STATE_RUNNABLE: return "runnable";
    case OS_STATE_RUNNING: return "running";
    case OS_STATE_WAITING: return "waiting";
    }
    return "?";
}

void hh_os_dump(const char *why)
{
    int i;
    fprintf(stderr, "[hh_os] %s: vi %u, count %llu, current %d\n", why, g_vi_count,
            (unsigned long long)g_vnow, g_cur ? (int)g_cur->t->id : -1);
    for (i = 0; i < HH_MAX_THREADS; i++) {
        HhThread *f = &g_thr[i];
        if (f->state == HH_T_FREE)
            continue;
        fprintf(stderr, "  thread %p id %d pri %d %s%s%s q=%p%s wake=%llu stack %u KB",
                (void *)f->t, (int)f->t->id, (int)f->t->priority, hh_state_name(f->state), f->dead ? " (exited)" : "",
                f->idle ? " (idle)" : "", (void *)f->wait_q, f->wait_send ? " (send)" : "",
                (unsigned long long)f->wake_at, f->stack_size >> 10);
        if (f->wait_q != NULL) /* the queue it blocks on: its fill (the address maps to a symbol with nm) */
            fprintf(stderr, " (q %d of %d)", (int)f->wait_q->validCount, (int)f->wait_q->msgCount);
        fprintf(stderr, "\n");
    }
}

void hh_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    hh_os_dump("fatal");
    fflush(stderr);
    abort();
}

/* ------------------------------------------------------------------------------------ */
/* coroutines                                                                           */

static HhThread *hh_find(OSThread *t)
{
    int i;
    if (t == NULL)
        return g_cur;
    for (i = 0; i < HH_MAX_THREADS; i++)
        if (g_thr[i].state != HH_T_FREE && g_thr[i].t == t)
            return &g_thr[i];
    return NULL;
}

static void hh_ctx_init(HhThread *f)
{
    uintptr_t top = ((uintptr_t)f->stack + f->stack_size) & ~(uintptr_t)15;
    u32 *sp;
#if defined(__i386__)
    /* hh_ctx_switch pops edi, esi, ebx, ebp, then returns into the trampoline with
     * %esp = top - 16 (16-byte aligned for its call). */
    sp = (u32 *)(top - 16 - 20);
    memset(sp, 0, 20);
    sp[4] = (u32)(uintptr_t)hh_ctx_trampoline;
#elif defined(__arm__)
    /* vpop d8-d15 (64 bytes), pop r4-r12 (36) and pc (4): %sp = top afterwards */
    sp = (u32 *)(top - 104);
    memset(sp, 0, 104);
    sp[25] = (u32)(uintptr_t)hh_ctx_trampoline;
#endif
    f->ctx.sp = sp;
    f->ctx_ready = 1;
}

static void hh_check_stack(HhThread *f)
{
    int i;
    const u32 *w = (const u32 *)f->stack;
    for (i = 0; i < HH_CANARY_WORDS; i++)
        if (w[i] != HH_CANARY)
            hh_fatal("stack overflow in game thread %d", (int)f->t->id);
}

/* The OSThreads live in game memory: catch a game write over one early. */
static void hh_check_threads(HhThread *ran)
{
    int i;
    for (i = 0; i < HH_MAX_THREADS; i++) {
        HhThread *f = &g_thr[i];
        if (f->state == HH_T_FREE)
            continue;
        if (f->t->priority != f->pri || f->t->id != f->id)
            hh_fatal("OSThread %p (id %d) overwritten while thread %d ran: priority %08x id %08x",
                     (void *)f->t, (int)f->id, (int)ran->id, (u32)f->t->priority, (u32)f->t->id);
    }
}

/* Debug bisection from game code (BK_OSCHECK=1): hhDebugCheck("label"). */
void hhDebugCheck(const char *where)
{
    if (getenv("BK_OSCHECK") != NULL && g_cur != NULL) {
        fprintf(stderr, "[hh_os] check %s\n", where);
        hh_check_threads(g_cur);
    }
}

/* Game thread -> kernel. Returns when the kernel dispatches this thread again. */
static void hh_to_kernel(void)
{
    HhThread *self = g_cur;
    g_spin = 0;
    hh_ctx_switch(&self->ctx, &g_kctx);
}

static void hh_rsp_main(void) __attribute__((noreturn));
void hh_thread_main(void)
{
    HhThread *self = g_cur;
    if (self == NULL)
        hh_rsp_main(); /* the graphics task's own stack (BK_RSPYIELD): entered from the kernel */
    self->entry(self->arg);
    /* returning from a thread's entry = __osCleanupThread -> osDestroyThread(NULL) */
    if (hh_os_trace)
        fprintf(stderr, "[hh_os] thread %d exited\n", (int)self->t->id);
    self->dead = 1;
    self->state = OS_STATE_STOPPED;
    self->t->state = OS_STATE_STOPPED;
    hh_to_kernel();
    hh_fatal("exited thread %d resumed", (int)self->t->id);
}

static void hh_set_state(HhThread *f, int state)
{
    f->state = state;
    f->t->state = (u16)state;
    if (state == OS_STATE_RUNNABLE || state == OS_STATE_WAITING)
        f->seq = ++g_seq;
}

/* BK_RSPYIELD: a graphics task in flight on its own stack (see hh_rsp_run) -- while it is, only threads above the
 * main thread's priority run (the scheduler, the audio thread, the system threads). */
static OSTask *g_rsp_cur SAVESTATE_EXCLUDE;
#define HH_RSP_FLOOR 10 /* MAIN_THREAD_PRIORITY (src/thread_config.h): the game's logic and display lists */
static int hh_parked(const HhThread *f) { return g_rsp_cur != NULL && f->t->priority <= HH_RSP_FLOOR; }

/* Highest-priority runnable thread, FIFO among equals. */
static HhThread *hh_best_runnable(void)
{
    HhThread *best = NULL;
    int i;
    for (i = 0; i < HH_MAX_THREADS; i++) {
        HhThread *f = &g_thr[i];
        if (f->state != OS_STATE_RUNNABLE || hh_parked(f))
            continue;
        if (best == NULL || f->t->priority > best->t->priority ||
            (f->t->priority == best->t->priority && f->seq < best->seq))
            best = f;
    }
    return best;
}

/* A game thread made another runnable, or changed priorities: yield if outranked. */
static void hh_preempt_check(void)
{
    HhThread *self = g_cur, *best;
    if (self == NULL)
        return; /* kernel context: the dispatch loop decides */
    best = hh_best_runnable();
    if (best != NULL && best->t->priority > self->t->priority) {
        hh_set_state(self, OS_STATE_RUNNABLE);
        hh_to_kernel();
    }
}

static void hh_wake(HhThread *f)
{
    f->wait_q = NULL;
    f->wake_at = 0;
    hh_set_state(f, OS_STATE_RUNNABLE);
}

/* The first thread blocked on mq (receive or send side), by priority then FIFO. */
static void hh_wake_waiter(OSMesgQueue *mq, int sending)
{
    HhThread *best = NULL;
    int i;
    for (i = 0; i < HH_MAX_THREADS; i++) {
        HhThread *f = &g_thr[i];
        if (f->state != OS_STATE_WAITING || f->wait_q != mq || f->wait_send != sending)
            continue;
        if (best == NULL || f->t->priority > best->t->priority ||
            (f->t->priority == best->t->priority && f->seq < best->seq))
            best = f;
    }
    if (best != NULL)
        hh_wake(best);
}

static void hh_block_on(OSMesgQueue *mq, int sending)
{
    HhThread *self = g_cur;
    if (self == NULL)
        hh_fatal("the kernel would block on queue %p", (void *)mq);
    self->wait_q = mq;
    self->wait_send = sending;
    hh_set_state(self, OS_STATE_WAITING);
    hh_to_kernel();
}

/* ------------------------------------------------------------------------------------ */
/* events and time                                                                      */

static int hh_event_due(void)
{
    u64 real, next;
    int i;
    if (g_rsp_task != NULL)
        return 1;
    if (!g_paced)
        return 0;
    next = g_next_vi;
    for (i = 0; i < HH_MAX_TIMERS; i++)
        if (g_timer[i] != NULL && g_timer_at[i] < next)
            next = g_timer_at[i];
    for (i = 0; i < HH_MAX_THREADS; i++)
        if (g_thr[i].wake_at != 0 && g_thr[i].wake_at < next)
            next = g_thr[i].wake_at;
    real = hh_real_counts();
    return next <= real;
}

/* Every OS call starts here: take a due interrupt now, as the N64 would have already. */
static int g_oscheck = -1;
static void hh_check_threads(HhThread *ran);

static void hh_checkpoint(void)
{
    if (g_oscheck < 0)
        g_oscheck = getenv("BK_OSCHECK") != NULL;
    if (g_oscheck && g_cur != NULL)
        hh_check_threads(g_cur);
    if (g_cur == NULL || g_intmask != OS_IM_ALL || !hh_event_due())
        return;
    g_interrupted = g_cur;
    hh_to_kernel();
}

static unsigned hh_lost_msgs SAVESTATE_EXCLUDE; /* interrupt messages lost to a full queue (logged) */
static void hh_send_nb(OSMesgQueue *mq, OSMesg msg)
{
    if (mq->validCount >= mq->msgCount) { /* full: the interrupt's message is lost, as on the N64 */
        hh_lost_msgs++;
        if (hh_lost_msgs <= 8 || (hh_lost_msgs & (hh_lost_msgs - 1)) == 0)
            fprintf(stderr, "[hh_os] vi %u: message %p lost, queue %p full (%d), %u lost so far\n", g_vi_count,
                    (void *)msg, (void *)mq, (int)mq->msgCount, hh_lost_msgs);
        return;
    }
    mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    mq->validCount++;
    hh_wake_waiter(mq, 0);
}

void hh_os_post_event(OSEvent e)
{
    if (e < HH_MAX_EVENTS && g_events[e].mq != NULL)
        hh_send_nb(g_events[e].mq, g_events[e].msg);
    hh_preempt_check();
}

/* The retrace message leaves the last two places of its queue free (round 25, the Cradle save
 * freeze). GoldenEye's scheduler takes retraces, RSP-done and RDP-done on one queue of 8. After a
 * stall (a 254 ms save on the device) the paced clock delivers up to HH_MAX_LAG = 15 retraces at
 * once and fills it; the scheduler, handling the first, starts the frame's graphics task, which
 * runs at once (osSpTaskStartGo) and posts RSP-done and RDP-done into the 7 retraces still queued
 * -- RDP-done was lost, and the scheduler waited for it forever: no new frame, the music (audio
 * runs apart) playing on, the controls dead. A retrace that finds the queue nearly full is dropped
 * instead (the scheduler is behind: it has retraces to count). Never reached at the N64's pace, nor
 * in the unpaced tests (one event at a time), so nothing changes there. */
static unsigned hh_vi_skipped SAVESTATE_EXCLUDE;
static void hh_vi_tick(void)
{
    g_vi_count++;
    hh_vi_retrace(g_vi_count);
    if (g_vi_mq != NULL && g_vi_retrace != 0 && g_vi_count % g_vi_retrace == 0) {
        if (g_vi_mq->msgCount >= 4 && g_vi_mq->validCount + 2 >= g_vi_mq->msgCount) {
            ++hh_vi_skipped; /* its own statement: read and written in one expression was unsequenced (UB) */
            if ((hh_vi_skipped & (hh_vi_skipped - 1)) == 0) /* log the 1st, 2nd, 4th, 8th ... */
                fprintf(stderr, "[hh_os] vi %u: retrace held back, queue %d of %d (%u so far)\n", g_vi_count,
                        (int)g_vi_mq->validCount, (int)g_vi_mq->msgCount, hh_vi_skipped);
            return;
        }
        hh_send_nb(g_vi_mq, g_vi_msg);
    }
}

/* Deliver the earliest pending event if its time is <= limit. Ties: retrace, timers,
 * sleepers. Returns 0 when nothing was due. */
static int hh_deliver_one(u64 limit)
{
    u64 at = g_next_vi;
    int kind = 0, idx = -1, i;
    for (i = 0; i < HH_MAX_TIMERS; i++)
        if (g_timer[i] != NULL && g_timer_at[i] < at) {
            at = g_timer_at[i];
            kind = 1;
            idx = i;
        }
    for (i = 0; i < HH_MAX_THREADS; i++)
        if (g_thr[i].wake_at != 0 && g_thr[i].wake_at < at) {
            at = g_thr[i].wake_at;
            kind = 2;
            idx = i;
        }
    if (at > limit)
        return 0;
    if (at > g_vnow)
        g_vnow = at;
    if (kind == 0) {
        g_next_vi += HH_VI_COUNTS;
        hh_vi_tick();
    } else if (kind == 1) {
        OSTimer *t = g_timer[idx];
        if (t->interval != 0) {
            g_timer_at[idx] += t->interval;
        } else {
            g_timer[idx] = NULL;
        }
        if (t->mq != NULL)
            hh_send_nb(t->mq, t->msg);
    } else {
        hh_wake(&g_thr[idx]);
    }
    return 1;
}

static u64 hh_next_event_time(void)
{
    u64 at = g_next_vi;
    int i;
    for (i = 0; i < HH_MAX_TIMERS; i++)
        if (g_timer[i] != NULL && g_timer_at[i] < at)
            at = g_timer_at[i];
    for (i = 0; i < HH_MAX_THREADS; i++)
        if (g_thr[i].wake_at != 0 && g_thr[i].wake_at < at)
            at = g_thr[i].wake_at;
    return at;
}

/* Where the kernel loop is, for the watchdog's log line (hh_plat.c's handheld_loop_detail reads it
 * from another thread): its turns, and the step it is in. A game
 * thread that makes no OS call keeps the host thread (nothing is preempted), so the turns stop, and
 * neither the frame hook (savestate_check) nor a gfx task (where the power request is taken) runs; a
 * game that makes no new frame turns on without either. Host state, written by the kernel only. */
static volatile u32 g_turns SAVESTATE_EXCLUDE;
static volatile int g_where SAVESTATE_EXCLUDE; /* HH_WHERE_* */

u32 hh_os_where(int *where, int *thread)
{
    HhThread *f = g_cur;
    *where = g_where;
    *thread = f != NULL ? (int)f->id : -1;
    return g_turns;
}

/* ------------------------------------------------------------------------------------ */
/* the graphics task on its own stack: the sound keeps up with a slow frame (BK_RSPYIELD)  */

/* 2026-10-02, the user: blowing up barrels at the Dam and dying, "the explosions and smoke are so slow and dropped
 * fps that it started to skip sound". The kernel ran a frame's graphics task in one piece: for its whole 100-240 ms
 * no retrace was delivered, so the game's audio thread (a sched client, every second retrace) mixed nothing, and the
 * card's ring (~150-220 ms ahead) ran dry -- "aud dry" in the PERF line, a refill of silence each time.
 * On the N64 the RSP and RDP draw in parallel with the CPU: retraces arrive, the audio thread mixes on time, while a
 * frame is still being drawn. Here the task now runs on a stack of its own (a coroutine, as the game threads are);
 * once it has run BK_RSPYIELD ms (default 50) it hands the host back to the kernel at the renderer's next poll
 * (hh_rsp_poll: between display-list commands and triangles) whenever an event is due. The kernel delivers it and
 * runs the threads above the main thread's priority -- the scheduler (retraces, the pad, the music fade) and the
 * audio thread (its frame mixed and queued to the AI at once: BK_AUDDIRECT) -- and resumes the task when they block.
 * The main thread (the game's logic and its display lists) stays parked until the task is done, as before, so the
 * frame draws the same display list over the same memory: the same pixels. The sound is the game's own mixer's,
 * now produced on time; nothing is resampled. A frame shorter than the threshold runs in one piece, exactly as
 * before. BK_RSPYIELD=0: the task in one piece always (the old kernel); unpaced runs (BK_NOPACE) never yield. */
static hh_ctx g_rsp_ctx SAVESTATE_EXCLUDE;
static HhThread g_rsp_thr SAVESTATE_EXCLUDE;      /* only its stack and context are used */
static int g_rsp_yield SAVESTATE_EXCLUDE = -1;    /* BK_RSPYIELD: -1 not read, 0 off, else on */
static u64 g_rsp_after_ns SAVESTATE_EXCLUDE;      /* run this long before yielding */
static int g_rsp_in SAVESTATE_EXCLUDE;            /* the coroutine is running */
static int g_rsp_done SAVESTATE_EXCLUDE;          /* it finished g_rsp_cur */
static u64 g_rsp_t0 SAVESTATE_EXCLUDE, g_rsp_left SAVESTATE_EXCLUDE; /* host ns: task start, last yield */
uint32_t hh_rsp_away_us SAVESTATE_EXCLUDE;         /* this task's time handed to the kernel (hh_gfx.c) */
uint32_t hh_rsp_yields SAVESTATE_EXCLUDE;          /* running total (PERF) */
#define HH_RSP_STACK (4u << 20)

static int hh_rsp_enabled(void)
{
    if (g_rsp_yield < 0) {
        const char *e = getenv("BK_RSPYIELD");
        const int ms = e && e[0] ? atoi(e) : 50;
        g_rsp_yield = ms > 0 && g_paced;
        g_rsp_after_ns = (u64)(ms > 0 ? ms : 0) * 1000000ull;
        if (g_rsp_yield) {
            void *m = mmap(NULL, HH_RSP_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (m == MAP_FAILED) {
                fprintf(stderr, "[hh_os] BK_RSPYIELD: no stack, off\n");
                g_rsp_yield = 0;
            } else {
                mprotect(m, 4096, PROT_NONE); /* a guard page: an overflow faults instead of writing past */
                g_rsp_thr.stack = (u8 *)m;
                g_rsp_thr.stack_size = HH_RSP_STACK;
                hh_ctx_init(&g_rsp_thr);
                g_rsp_ctx = g_rsp_thr.ctx;
            }
        }
    }
    return g_rsp_yield;
}

/* The coroutine: run the task in flight, report it done, wait for the next. */
static void hh_rsp_main(void)
{
    for (;;) {
        hh_plat_rsp_task(g_rsp_cur);
        g_rsp_done = 1;
        hh_ctx_switch(&g_rsp_ctx, &g_kctx);
    }
}

/* Kernel: run (or resume) the task in flight until it is done or yields; done = its SP and DP events. */
static void hh_rsp_run(void)
{
    g_where = HH_WHERE_DRAW;
    g_rsp_in = 1;
    if (g_rsp_left != 0) {
        hh_rsp_away_us += (u32)((hh_host_ns() - g_rsp_left) / 1000u);
        g_rsp_left = 0;
    }
    hh_ctx_switch(&g_kctx, &g_rsp_ctx);
    g_rsp_in = 0;
    if (g_rsp_done) {
        g_rsp_done = 0;
        g_rsp_cur = NULL;
        hh_os_post_event(OS_EVENT_SP);
        hh_os_post_event(OS_EVENT_DP); /* XBUS: the RDP finished with it */
    } else {
        g_rsp_left = hh_host_ns();
        hh_rsp_yields++;
    }
}

/* The renderer's poll (gfx_pc.c, gfx_soft.c), inside the task: back to the kernel when the task has run its
 * threshold and an event is due (interrupts enabled, as at a checkpoint). Returns the microseconds the kernel
 * then took, which the caller's own timer leaves out. Anywhere else (no task in flight, the desktop tools) 0. */
uint32_t hh_rsp_poll(void)
{
    u64 now, back;
    if (!g_rsp_in || g_intmask != OS_IM_ALL)
        return 0;
    now = hh_host_ns();
    if (now - g_rsp_t0 < g_rsp_after_ns || !hh_event_due())
        return 0;
    hh_ctx_switch(&g_rsp_ctx, &g_kctx);
    back = hh_host_ns();
    return (uint32_t)((back - now) / 1000u);
}

/* Kernel: execute a started RSP task, then deliver whatever the host clock says is due. */
static void hh_kernel_service(void)
{
    for (;;) {
        g_where = HH_WHERE_EVENTS;
        if (g_rsp_task != NULL) {
            OSTask *task = g_rsp_task;
            g_rsp_task = NULL;
            g_where = task->t.type == M_GFXTASK ? HH_WHERE_DRAW : HH_WHERE_AUDIO;
            if (task->t.type == M_GFXTASK && hh_rsp_enabled()) {
                g_rsp_cur = task;
                g_rsp_t0 = hh_host_ns();
                g_rsp_left = 0;
                hh_rsp_away_us = 0;
                hh_rsp_run();
                continue;
            }
            hh_plat_rsp_task(task);
            hh_os_post_event(OS_EVENT_SP);
            if (task->t.type == M_GFXTASK)
                hh_os_post_event(OS_EVENT_DP); /* XBUS: the RDP finished with it */
            continue;
        }
        if (!g_paced) {
            if (g_force_advance) {
                g_force_advance = 0;
                hh_deliver_one(hh_next_event_time());
            }
            return;
        }
        {
            u64 real = hh_real_counts();
            if (real > g_vnow + HH_MAX_LAG) {
                g_real_base_ns += hh_counts_to_ns(real - (g_vnow + HH_MAX_LAG));
                real = g_vnow + HH_MAX_LAG;
            }
            if (!hh_deliver_one(real))
                return;
        }
    }
}

static HhThread *hh_pick_next(void)
{
    HhThread *best = hh_best_runnable(), *intr = g_interrupted;
    g_interrupted = NULL;
    if (intr != NULL && intr->state == OS_STATE_RUNNING) {
        if (!hh_parked(intr) && (best == NULL || best->t->priority <= intr->t->priority))
            return intr;
        hh_set_state(intr, OS_STATE_RUNNABLE);
    }
    return best;
}

/* Uncapped frames (default ON since 2026-10-10, the user's choice after the device test; BK_UNCAP=0 / debug.cfg uncap=0:
 * off; the Miyoo Mini build's, the GoldenEye port 0b2140c): a frame's graphics task starts when the game submits it (sched.c
 * __scHandleKick) instead of at the next retrace, and need not wait for the retrace that shows the last frame
 * (__scTaskReady). The game's time is unchanged: osGetCount still moves in whole retraces, to each one as the host
 * clock passes it, and the game's frame step (frametiming.c) is the retraces since its last frame -- 2, 2, 3, 2, 3 ...
 * at 2.4 retraces a frame, adding up to the real time; the game's floor of one retrace a frame stays. What changes is
 * only when a frame starts: as soon as the last one is submitted, not at the next retrace (the handheld's paced PERF
 * lines show 6-10 ms of that wait, "sleep", in a 40-55 ms frame). BK_UNCAP=0: the N64's pickup at the retrace. */
int hh_uncap(void)
{
    static int on SAVESTATE_EXCLUDE = -1;
    if (on < 0) {
        const char *e = getenv("BK_UNCAP");
        on = !(e && e[0] == '0');
        fprintf(stderr, "uncap: %s (debug.cfg uncap=0: frames start at retraces, as on the N64)\n", on ? "on" : "off");
    }
    return on;
}

/* The host clock in retraces since the game's clock base (a continuous count, where osGetCount moves in whole
 * retraces), for the fractional frame step (hh_plat.c hhFracStep, BK_FRACSTEP); -1 when unpaced (BK_NOPACE: no real
 * time). Pauses and state loads move the base (hh_os_pause_end, hh_os_state_loaded); the step resyncs on a jump. */
double hh_os_real_retraces(void)
{
    return g_paced ? (double)hh_real_counts() / (double)HH_VI_COUNTS : -1.0;
}

void hh_os_init(int paced)
{
    g_paced = paced;
    hh_os_trace = getenv("BK_OSTRACE") != NULL;
    g_real_base_ns = hh_host_ns();
}

/* Save states (port/kit/platform/savestate.c): once a frame, with every game thread parked --
 * contexts and stacks (g_stack_pool) all in the snapshot, the kernel on its own host stack, no event
 * half delivered -- the kit saves or loads here (hh_plat.c's hh_frame_hook -> savestate_check). */
int hh_frame_boundary SAVESTATE_EXCLUDE; /* set by hh_plat_vi when a new frame is shown */
void hh_frame_hook(void) __attribute__((weak));

/* After a state load (gfx_savestate_fixup): the game's clock came back with the state, the host's
 * didn't -- re-base so real time reads as the restored game time (else a paced run would wait out,
 * or drop, the difference between the two sessions). */
void hh_os_state_loaded(void)
{
    extern void hh_plat_state_loaded(void);
    hh_plat_state_loaded(); /* the frame watchdog's last-frame retrace is this session's */
    hh_os_rebase_clock();
    g_rebased = 1;
    g_spin = 0;
    g_force_advance = 0;
}

/* Where the host's time went, running totals in us (the STALL line, hh_gfx.c): [0..7] the game threads by id & 7,
 * HH_ACCT_HOOK the frame hook (save states), HH_ACCT_SERVICE the kernel's service (audio tasks, a graphics task run
 * in one piece, event delivery), HH_ACCT_DRAW a graphics task run as a coroutine, HH_ACCT_SLEEP the idle sleeps; and the
 * longest single turn of one game thread (no OS call in it) since hh_os_acct_slice_reset, with its id. */
uint32_t hh_os_acct_us[HH_ACCT_N] SAVESTATE_EXCLUDE;
uint32_t hh_os_slice_max_us SAVESTATE_EXCLUDE;
int hh_os_slice_max_id SAVESTATE_EXCLUDE;
static u64 g_acct_t SAVESTATE_EXCLUDE;
static void hh_acct(int b)
{
    const u64 now = hh_host_ns();
    const uint32_t us = g_acct_t != 0 ? (uint32_t)((now - g_acct_t) / 1000u) : 0;
    hh_os_acct_us[b] += us;
    if (b < 8 && us > hh_os_slice_max_us) {
        hh_os_slice_max_us = us;
        hh_os_slice_max_id = b;
    }
    g_acct_t = now;
}

void hh_os_run(void)
{
    for (;;) {
        HhThread *f;
        g_turns++;
        if (hh_frame_boundary && hh_frame_hook != NULL && g_rsp_cur == NULL) { /* not with a frame half drawn */
            hh_frame_boundary = 0;
            g_where = HH_WHERE_CHECK;
            hh_acct(HH_ACCT_SERVICE);
            hh_frame_hook();
            hh_acct(HH_ACCT_HOOK);
        }
        hh_kernel_service();
        hh_acct(HH_ACCT_SERVICE);
        f = hh_pick_next();
        if (f != NULL) {
            if (hh_os_trace)
                fprintf(stderr, "[hh_os] vi %u -> thread %d\n", g_vi_count, (int)f->t->id);
            if (!f->ctx_ready)
                hh_ctx_init(f);
            f->state = OS_STATE_RUNNING;
            f->t->state = OS_STATE_RUNNING;
            g_cur = f;
            g_where = HH_WHERE_THREAD;
            hh_ctx_switch(&g_kctx, &f->ctx);
            g_cur = NULL;
            hh_acct((int)(f->id & 7));
            hh_check_stack(f);
            hh_check_threads(f);
            continue;
        }
        if (g_rsp_cur != NULL) { /* the threads that may run are blocked: the graphics task goes on */
            hh_rsp_run();
            hh_acct(HH_ACCT_DRAW);
            continue;
        }
        /* idle: every thread is blocked until the next event */
        {
            u64 at = hh_next_event_time();
            if (g_paced) {
                u64 real = hh_real_counts();
                g_where = HH_WHERE_SLEEP;
                if (at > real)
                    hh_sleep_ns(hh_counts_to_ns(at - real));
                hh_acct(HH_ACCT_SLEEP);
            }
            g_where = HH_WHERE_EVENTS;
            hh_deliver_one(at);
        }
    }
}

void hh_os_sleep_until(u64 count)
{
    HhThread *self = g_cur;
    if (self == NULL)
        hh_fatal("hh_os_sleep_until from the kernel");
    if (count <= g_vnow)
        return;
    self->wake_at = count;
    self->wait_q = NULL;
    hh_set_state(self, OS_STATE_WAITING);
    hh_to_kernel();
}

/* A busy-wait on state another thread changes (music.c's alCSPGetState loops): on the
 * N64 an interrupt lets the other thread in; here the waiter sleeps to the next event. */
void hhSpinWait(void)
{
    if (g_cur != NULL)
        hh_os_sleep_until(hh_next_event_time());
}

/* waitForNextFrame (PORT): sleep until the count register reads 'count' (wrap-safe). */
void hhSleepUntilCount(u32 count)
{
    hh_os_sleep_until(g_vnow + (u32)(count - (u32)g_vnow));
}

/* ------------------------------------------------------------------------------------ */
/* libultra: threads                                                                    */

void osCreateThread(OSThread *t, OSId id, void (*entry)(void *), void *arg, void *sp, OSPri pri)
{
    HhThread *f = hh_find(t);
    int i;
    (void)sp; /* native code needs far more than the N64 stacks: ours come from the pool */
    if (f == NULL) {
        for (i = 0; i < HH_MAX_THREADS && g_thr[i].state != HH_T_FREE; i++)
            ;
        if (i == HH_MAX_THREADS)
            hh_fatal("too many threads");
        f = &g_thr[i];
        f->stack_size = (id == 3) ? (1u << 20) : (256u << 10); /* MAIN_THREAD_ID */
        if (g_stack_used + f->stack_size > HH_STACK_POOL)
            hh_fatal("thread stack pool exhausted");
        f->stack = g_stack_pool + g_stack_used;
        g_stack_used += f->stack_size;
        for (i = 0; i < HH_CANARY_WORDS; i++)
            ((u32 *)f->stack)[i] = HH_CANARY;
    }
    f->t = t;
    f->entry = entry;
    f->arg = arg;
    f->ctx_ready = 0;
    f->dead = 0;
    f->idle = (pri == OS_PRIORITY_IDLE);
    f->wait_q = NULL;
    f->wake_at = 0;
    t->next = NULL;
    t->queue = NULL;
    t->tlnext = NULL;
    t->id = id;
    t->priority = pri;
    f->pri = pri;
    f->id = id;
    t->flags = 0;
    t->fp = 0;
    hh_set_state(f, OS_STATE_STOPPED);
    if (hh_os_trace)
        fprintf(stderr, "[hh_os] create thread %d pri %d%s\n", (int)id, (int)pri, f->idle ? " (idle: not run)" : "");
}

void osStartThread(OSThread *t)
{
    HhThread *f = hh_find(t);
    if (f == NULL)
        hh_fatal("osStartThread on an unknown thread");
    if (f->state != OS_STATE_STOPPED || f->dead || f->idle)
        return;
    hh_set_state(f, OS_STATE_RUNNABLE);
    hh_preempt_check();
}

void osStopThread(OSThread *t)
{
    HhThread *f = hh_find(t);
    if (f == NULL)
        return;
    f->wait_q = NULL;
    f->wake_at = 0;
    hh_set_state(f, OS_STATE_STOPPED);
    if (f == g_cur)
        hh_to_kernel();
}

void osDestroyThread(OSThread *t)
{
    HhThread *f = hh_find(t);
    if (f == NULL)
        return;
    f->dead = 1;
    osStopThread(t);
}

void osSetThreadPri(OSThread *t, OSPri pri)
{
    HhThread *f = hh_find(t);
    if (f == NULL)
        return;
    hh_checkpoint();
    f->t->priority = pri;
    f->pri = pri;
    hh_preempt_check();
}

OSPri osGetThreadPri(OSThread *t)
{
    HhThread *f = hh_find(t);
    return f ? f->t->priority : 0;
}

OSId osGetThreadId(OSThread *t)
{
    HhThread *f = hh_find(t);
    return f ? f->t->id : 0;
}

void osYieldThread(void)
{
    if (g_cur == NULL)
        return;
    hh_set_state(g_cur, OS_STATE_RUNNABLE);
    hh_to_kernel();
}

/* ------------------------------------------------------------------------------------ */
/* libultra: message queues and events                                                  */

void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msg, s32 count)
{
    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msg;
}

s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag)
{
    hh_checkpoint();
    while (mq->validCount == 0) {
        if (flag == OS_MESG_NOBLOCK)
            return -1;
        hh_block_on(mq, 0);
    }
    if (msg != NULL)
        *msg = mq->msg[mq->first];
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    hh_wake_waiter(mq, 1);
    hh_preempt_check();
    return 0;
}

s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flag)
{
    hh_checkpoint();
    while (mq->validCount >= mq->msgCount) {
        if (flag == OS_MESG_NOBLOCK)
            return -1;
        hh_block_on(mq, 1);
    }
    mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    mq->validCount++;
    hh_wake_waiter(mq, 0);
    hh_preempt_check();
    return 0;
}

s32 osJamMesg(OSMesgQueue *mq, OSMesg msg, s32 flag)
{
    hh_checkpoint();
    while (mq->validCount >= mq->msgCount) {
        if (flag == OS_MESG_NOBLOCK)
            return -1;
        hh_block_on(mq, 1);
    }
    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    mq->msg[mq->first] = msg;
    mq->validCount++;
    hh_wake_waiter(mq, 0);
    hh_preempt_check();
    return 0;
}

void osSetEventMesg(OSEvent e, OSMesgQueue *mq, OSMesg msg)
{
    if (e >= HH_MAX_EVENTS)
        return;
    g_events[e].mq = mq;
    g_events[e].msg = msg;
}

OSIntMask osSetIntMask(OSIntMask mask)
{
    OSIntMask old = g_intmask;
    g_intmask = mask;
    if (mask == OS_IM_ALL)
        hh_checkpoint(); /* interrupts held off meanwhile are taken now */
    return old;
}

/* The retrace client (sched.c's osCreateScheduler). The VI itself always ticks. */
void osCreateViManager(OSPri pri) { (void)pri; }

void osViSetEvent(OSMesgQueue *mq, OSMesg msg, u32 retraceCount)
{
    g_vi_mq = mq;
    g_vi_msg = msg;
    g_vi_retrace = retraceCount;
}

/* The RSP: osSpTaskStartGo hands the task to the kernel, which runs it before anything
 * else (the calling thread resumes right after, as if the RSP were very fast). */
void osSpTaskLoad(OSTask *task) { (void)task; }

void osSpTaskStartGo(OSTask *task)
{
    if (g_rsp_task != NULL || g_rsp_cur != NULL)
        hh_fatal("RSP task started while another is pending%s", g_rsp_cur != NULL ? " (a graphics task in flight)" : "");
    g_rsp_task = task;
    if (g_cur != NULL) {
        g_interrupted = g_cur;
        hh_to_kernel();
    }
}

void osSpTaskYield(void) {}
OSYieldResult osSpTaskYielded(OSTask *task) { (void)task; return 0; }

/* ------------------------------------------------------------------------------------ */
/* libultra: time                                                                       */

u32 osGetCount(void)
{
    hh_checkpoint();
    if (!g_paced && g_cur != NULL && ++g_spin > 1000000) {
        /* Unpaced time only moves when every thread blocks: a thread spinning on the
         * count would never see it change. Let the next event happen. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[hh_os] thread %d spins on osGetCount; advancing time\n", (int)g_cur->t->id);
        }
        g_force_advance = 1;
        g_interrupted = g_cur;
        hh_to_kernel();
    }
    return (u32)g_vnow;
}

OSTime osGetTime(void)
{
    hh_checkpoint();
    return g_vnow;
}

void osSetTime(OSTime t)
{
    (void)t; /* unused by the game; the clock stays monotonic */
}

int osSetTimer(OSTimer *t, OSTime countdown, OSTime interval, OSMesgQueue *mq, OSMesg msg)
{
    int i, slot = -1;
    hh_checkpoint();
    t->next = NULL;
    t->prev = NULL;
    t->interval = interval;
    t->value = countdown != 0 ? countdown : interval;
    t->mq = mq;
    t->msg = msg;
    for (i = 0; i < HH_MAX_TIMERS; i++) {
        if (g_timer[i] == t) {
            slot = i;
            break;
        }
        if (g_timer[i] == NULL && slot < 0)
            slot = i;
    }
    if (slot < 0)
        hh_fatal("too many timers");
    g_timer[slot] = t;
    g_timer_at[slot] = g_vnow + t->value;
    return 0;
}

int osStopTimer(OSTimer *t)
{
    int i;
    for (i = 0; i < HH_MAX_TIMERS; i++)
        if (g_timer[i] == t) {
            g_timer[i] = NULL;
            return 0;
        }
    return -1;
}
