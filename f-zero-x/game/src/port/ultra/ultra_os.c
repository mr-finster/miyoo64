/**
 * libultra replacement: threads, message queues, events, clock, VI, SP, AI, SI,
 * PI and the odds and ends the game links against (see port_os.h for the model).
 *
 * Threads are cooperative fibers. Everything a fiber needs to resume - its saved
 * stack pointer and its stack - is in game memory (OSThread.context and
 * sFiberStacks), so a save state of game memory is a save state of every thread.
 * libultra's priority rules are kept: the highest-priority runnable thread runs;
 * waking a higher-priority thread (osSendMesg, osStartThread, osSetThreadPri)
 * switches to it immediately; equal priorities are FIFO. The N64's asynchronous
 * preemption (a VI interrupting a busy loop) happens at the calls the game's busy
 * loops make: osGetTime crossing a retrace, osViGetCurrentFramebuffer while a swap
 * is pending.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "libultra/ultra64.h"
#include "port_os.h"

/* ------------------------------------------------------------------ fibers --- */
#define FIBER_MAX 12
#define FIBER_STACK_SIZE (256 * 1024)

/* Our use of OSThread.context (0x1B0 bytes of MIPS register save area). */
typedef struct {
    void* sp;             /* saved host stack pointer while switched out */
    void (*entry)(void*); /* start routine */
    void* arg;
    s32 slot;             /* index into sFiberStacks */
} FiberCtx;
#define FIBER(t) ((FiberCtx*) &(t)->context)

static u8 sFiberStacks[FIBER_MAX][FIBER_STACK_SIZE] __attribute__((aligned(16)));
static OSThread* sFiberOwner[FIBER_MAX];
static OSThread* sRunQueue;     /* runnable threads, highest priority first, FIFO within one */
static OSThread* sRunning;      /* current fiber; NULL = the host */
static OSThread* sAllThreads;   /* tlnext list */
static void* sHostSp;           /* host context while a fiber runs (not game state) */

extern void port_ctx_switch(void** save_sp, void* load_sp);

static void enqueue(OSThread** q, OSThread* t) {
    while (*q != NULL && (*q)->priority >= t->priority) {
        q = &(*q)->next;
    }
    t->next = *q;
    *q = t;
    t->queue = NULL;
}

static void unlink_from(OSThread** q, OSThread* t) {
    while (*q != NULL) {
        if (*q == t) {
            *q = t->next;
            t->next = NULL;
            return;
        }
        q = &(*q)->next;
    }
}

static void switch_to(OSThread* next) {
    OSThread* prev = sRunning;
    if (next == prev) {
        return;
    }
    sRunning = next;
    if (next != NULL) {
        next->state = OS_STATE_RUNNING;
    }
    if (prev != NULL && prev->state == OS_STATE_RUNNING) {
        prev->state = OS_STATE_RUNNABLE;
    }
    port_ctx_switch(prev ? &FIBER(prev)->sp : &sHostSp, next ? FIBER(next)->sp : sHostSp);
}

/* Run whichever thread should run now (the head of the run queue), or the host. */
static void reschedule(void) {
    switch_to(sRunQueue);
}

static void fiber_start(void) {
    OSThread* t = sRunning;
    FIBER(t)->entry(FIBER(t)->arg);
    /* An N64 thread function returning: stop it for good. */
    osStopThread(NULL);
    port_fatal("fiber %d resumed after stopping", (int) t->id);
}

static void* build_initial_stack(s32 slot) {
    u8* top = sFiberStacks[slot] + FIBER_STACK_SIZE;
#if defined(__i386__)
    /* popped: edi esi ebx ebp, then ret -> fiber_start; the zero word above it
     * is fiber_start's (never used) return address, placed so that at entry
     * esp % 16 == 12, as after a normal call on a 16-aligned stack. */
    u32* fake_ret = (u32*) (top - 4); /* top is 16-aligned, so this slot is 12 mod 16 */
    u32* frame = fake_ret - 5;
    *fake_ret = 0;
    frame[0] = 0; frame[1] = 0; frame[2] = 0; frame[3] = 0; /* edi esi ebx ebp */
    frame[4] = (u32) (uintptr_t) fiber_start;
    return frame;
#elif defined(__arm__)
    /* popped: d8-d15 (16 words), r4-r11 (8 words), pc = fiber_start. */
    u32* sp = (u32*) top;
    sp -= 25;
    memset(sp, 0, 24 * sizeof(u32));
    sp[24] = (u32) (uintptr_t) fiber_start;
    return sp;
#else
#error "port_ctx_switch: unsupported host"
#endif
}

void port_os_block_on_host(void) {
    if (sRunning != NULL) {
        switch_to(NULL);
    }
}

void port_os_run(void) {
    if (sRunning == NULL) {
        reschedule();
    }
}

/* The running game thread's id, -1 on the host: for a diagnostic log line (port_main.c
 * handheld_loop_detail), read from another thread. sRunning only ever points at the game's
 * static OSThreads. */
int port_os_running_id(void) {
    OSThread* t = sRunning;
    return t != NULL ? (int) t->id : -1;
}

/* ----------------------------------------------------------------- threads --- */
void osCreateThread(OSThread* t, OSId id, void (*entry)(void*), void* arg, void* sp, OSPri pri) {
    s32 slot;
    (void) sp; /* the N64 stack is too small for native frames; fibers use sFiberStacks */
    for (slot = 0; slot < FIBER_MAX; slot++) {
        if (sFiberOwner[slot] == NULL || sFiberOwner[slot] == t) {
            break;
        }
    }
    if (slot == FIBER_MAX) {
        port_fatal("osCreateThread: out of fiber stacks (id %d)", (int) id);
    }
    sFiberOwner[slot] = t;
    memset(t, 0, sizeof(*t));
    t->id = id;
    t->priority = pri;
    t->state = OS_STATE_STOPPED;
    FIBER(t)->entry = entry;
    FIBER(t)->arg = arg;
    FIBER(t)->slot = slot;
    FIBER(t)->sp = build_initial_stack(slot);
    t->tlnext = sAllThreads;
    sAllThreads = t;
}

void osStartThread(OSThread* t) {
    if (t->state != OS_STATE_STOPPED && t->state != OS_STATE_WAITING) {
        return;
    }
    if (t->state == OS_STATE_WAITING && t->queue != NULL) {
        unlink_from(t->queue, t);
    }
    t->state = OS_STATE_RUNNABLE;
    enqueue(&sRunQueue, t);
    if (sRunning != NULL && t->priority > sRunning->priority) {
        unlink_from(&sRunQueue, sRunning);
        enqueue(&sRunQueue, sRunning);
        reschedule();
    }
}

void osStopThread(OSThread* t) {
    if (t == NULL) {
        t = sRunning;
    }
    if (t == NULL) {
        return;
    }
    unlink_from(&sRunQueue, t);
    if (t->queue != NULL) {
        unlink_from(t->queue, t);
        t->queue = NULL;
    }
    t->state = OS_STATE_STOPPED;
    if (t == sRunning) {
        reschedule();
    }
}

void osSetThreadPri(OSThread* t, OSPri pri) {
    if (t == NULL) {
        t = sRunning;
    }
    if (t == NULL) {
        return;
    }
    t->priority = pri;
    if (t->state == OS_STATE_RUNNABLE || t->state == OS_STATE_RUNNING) {
        unlink_from(&sRunQueue, t);
        enqueue(&sRunQueue, t);
    } else if (t->queue != NULL) {
        OSThread** q = t->queue;
        unlink_from(q, t);
        enqueue(q, t);
        t->queue = q;
    }
    if (sRunning != NULL && sRunQueue != sRunning) {
        reschedule();
    }
}

OSPri osGetThreadPri(OSThread* t) {
    if (t == NULL) {
        t = sRunning;
    }
    return t ? t->priority : 0;
}

OSId osGetThreadId(OSThread* t) {
    if (t == NULL) {
        t = sRunning;
    }
    return t ? t->id : 0;
}

void osYieldThread(void) {
    if (sRunning != NULL) {
        unlink_from(&sRunQueue, sRunning);
        enqueue(&sRunQueue, sRunning);
        reschedule();
    }
}

OSThread* __osGetActiveQueue(void) {
    return sAllThreads;
}

/* -------------------------------------------------------------- messages --- */
void osCreateMesgQueue(OSMesgQueue* mq, OSMesg* msg, s32 count) {
    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msg;
}

/* Block the running fiber on q until something wakes it. */
static void block_on(OSThread** q) {
    OSThread* self = sRunning;
    if (self == NULL) {
        port_fatal("a blocking libultra call was made from the host context");
    }
    unlink_from(&sRunQueue, self);
    self->state = OS_STATE_WAITING;
    enqueue(q, self);
    self->queue = q;
    reschedule();
}

static void wake_first(OSThread** q) {
    OSThread* t = *q;
    if (t == NULL) {
        return;
    }
    *q = t->next;
    t->next = NULL;
    t->queue = NULL;
    t->state = OS_STATE_RUNNABLE;
    enqueue(&sRunQueue, t);
    if (sRunning != NULL && t->priority > sRunning->priority) {
        unlink_from(&sRunQueue, sRunning);
        enqueue(&sRunQueue, sRunning);
        reschedule();
    }
}

s32 osSendMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    while (mq->validCount >= mq->msgCount) {
        if (flag != OS_MESG_BLOCK || sRunning == NULL) {
            return -1;
        }
        block_on(&mq->fullqueue);
    }
    mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    mq->validCount++;
    wake_first(&mq->mtqueue);
    return 0;
}

s32 osJamMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    while (mq->validCount >= mq->msgCount) {
        if (flag != OS_MESG_BLOCK || sRunning == NULL) {
            return -1;
        }
        block_on(&mq->fullqueue);
    }
    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    mq->msg[mq->first] = msg;
    mq->validCount++;
    wake_first(&mq->mtqueue);
    return 0;
}

s32 osRecvMesg(OSMesgQueue* mq, OSMesg* msg, s32 flag) {
    while (mq->validCount == 0) {
        if (flag == OS_MESG_NOBLOCK) {
            return -1;
        }
        block_on(&mq->mtqueue);
    }
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    wake_first(&mq->fullqueue);
    return 0;
}

/* ---------------------------------------------------------------- events --- */
typedef struct {
    OSMesgQueue* mq;
    OSMesg msg;
} EventSlot;
static EventSlot sEvents[OS_NUM_EVENTS];

void osSetEventMesg(OSEvent e, OSMesgQueue* mq, OSMesg msg) {
    if (e < OS_NUM_EVENTS) {
        sEvents[e].mq = mq;
        sEvents[e].msg = msg;
    }
}

void port_os_event(s32 e) {
    if (e >= 0 && e < OS_NUM_EVENTS && sEvents[e].mq != NULL) {
        osSendMesg(sEvents[e].mq, sEvents[e].msg, OS_MESG_NOBLOCK);
    }
}

/* ----------------------------------------------------------------- clock --- */
/* Virtual, deterministic: the count moves one retrace per VI, plus a little per
 * osGetTime call so the game's busy-wait loops make progress; a loop that runs
 * into the next retrace hands control to the host, which raises the VI. */
#define COUNT_PER_CALL 2000u
static u64 sClock;   /* count since boot */
static u64 sTimeBase; /* osSetTime offset */
static u32 sViCount;

OSTime osGetTime(void) {
    {
        /* FZX_CLOCKLOG=<vi>: log each osGetTime up to that retrace (thread id + caller). */
        static long sLogTo = -2;
        if (sLogTo == -2) {
            const char* e = getenv("FZX_CLOCKLOG");
            sLogTo = e ? atol(e) : -1;
        }
        if ((long) sViCount <= sLogTo) {
            port_log("clk vi %u thread %d clock %llu from %p\n", (unsigned) sViCount, sRunning ? (int) sRunning->id : -1,
                     (unsigned long long) sClock, __builtin_return_address(0));
        }
    }
    sClock += COUNT_PER_CALL;
    if (sRunning != NULL && sClock >= (u64) (sViCount + 1) * PORT_COUNT_PER_VI) {
        port_os_block_on_host();
    }
    return sClock - sTimeBase;
}

void osSetTime(OSTime t) {
    sTimeBase = sClock - t;
}

u32 osGetCount(void) {
    return (u32) (sClock += 1);
}

/* -------------------------------------------------------------------- VI --- */
static void* sViCurrent;
static void* sViNext;
static OSMesgQueue* sViMq;
static OSMesg sViMsg;
static u32 sViRetrace = 1;
static s32 sViBlack;

void osCreateViManager(OSPri pri) { (void) pri; }
void osViSetMode(OSViMode* mode) { (void) mode; }
void osViSetSpecialFeatures(u32 func) { (void) func; }
void osViSetYScale(f32 scale) { (void) scale; }
void osViBlack(u8 active) { sViBlack = active; }

void osViSetEvent(OSMesgQueue* mq, OSMesg msg, u32 retraceCount) {
    sViMq = mq;
    sViMsg = msg;
    sViRetrace = retraceCount ? retraceCount : 1;
}

void osViSwapBuffer(void* fb) {
    sViNext = fb;
}

void* osViGetNextFramebuffer(void) {
    return sViNext;
}

void* osViGetCurrentFramebuffer(void) {
    if (sViNext != sViCurrent && sRunning != NULL) {
        /* the swap happens at the next retrace: let the host raise it */
        port_os_block_on_host();
    }
    return sViCurrent;
}

/* What the VI scans out now; NULL while blanked (osViBlack) or before any swap. */
void* port_os_vi_framebuffer(void) {
    return sViBlack ? NULL : sViCurrent;
}

void port_os_vi(void) {
    sViCount++;
    if (sClock < (u64) sViCount * PORT_COUNT_PER_VI) {
        sClock = (u64) sViCount * PORT_COUNT_PER_VI;
    }
    sViCurrent = sViNext;
    if (sViMq != NULL && (sViCount % sViRetrace) == 0) {
        osSendMesg(sViMq, sViMsg, OS_MESG_NOBLOCK);
    }
}

u32 port_os_vi_count(void) { return sViCount; }
void* port_os_vi_current_fb(void) { return sViBlack ? NULL : sViCurrent; }

/* -------------------------------------------------------------------- SP --- */
static OSTask* sSpLoaded;
static OSTask* sSpPending;

void osSpTaskLoad(OSTask* task) { sSpLoaded = task; }
void osSpTaskStartGo(OSTask* task) { sSpPending = task ? task : sSpLoaded; }
void osSpTaskYield(void) { }
OSYieldResult osSpTaskYielded(OSTask* task) { (void) task; return 0; }

OSTask* port_os_take_sp_task(void) {
    OSTask* t = sSpPending;
    sSpPending = NULL;
    return t;
}

u32 osDpGetStatus(void) { return 0; }
void osDpSetStatus(u32 data) { (void) data; }

/* -------------------------------------------------------------------- AI --- */
s32 osAiSetFrequency(u32 frequency) {
    port_audio_set_frequency(frequency);
    return frequency;
}

u32 osAiGetLength(void) {
    return port_audio_queued_bytes();
}

s32 osAiSetNextBuffer(void* buf, u32 size) {
    port_audio_push((const s16*) buf, size);
    return 0;
}

/* -------------------------------------------------------------------- SI --- */
s32 osContInit(OSMesgQueue* mq, u8* bitpattern, OSContStatus* status) {
    (void) mq;
    *bitpattern = 1;
    for (s32 i = 0; i < MAXCONTROLLERS; i++) {
        status[i].type = (i == 0) ? CONT_TYPE_NORMAL : 0;
        status[i].status = 0;
        status[i].errno = (i == 0) ? 0 : CONT_NO_RESPONSE_ERROR;
    }
    return 0;
}

s32 osContStartReadData(OSMesgQueue* mq) {
    (void) mq;
    port_os_event(OS_EVENT_SI);
    return 0;
}

void osContGetReadData(OSContPad* pads) {
    for (s32 i = 0; i < MAXCONTROLLERS; i++) {
        memset(&pads[i], 0, sizeof(OSContPad));
        pads[i].errno = (i == 0) ? 0 : CONT_NO_RESPONSE_ERROR;
    }
    port_input_read(&pads[0]);
}

s32 osMotorInit(OSMesgQueue* mq, OSPfs* pfs, int channel) { (void) mq; (void) pfs; (void) channel; return PFS_ERR_NOPACK; }
s32 __osMotorAccess(OSPfs* pfs, s32 flag) { (void) pfs; (void) flag; return PFS_ERR_NOPACK; }
s32 osPfsInitPak(OSMesgQueue* mq, OSPfs* pfs, int channel) { (void) mq; (void) pfs; (void) channel; return PFS_ERR_NOPACK; }
s32 osPfsFindFile(OSPfs* pfs, u16 company, u32 game, u8* name, u8* ext, s32* fileNo) {
    (void) pfs; (void) company; (void) game; (void) name; (void) ext; (void) fileNo;
    return PFS_ERR_NOPACK;
}
s32 osPfsAllocateFile(OSPfs* pfs, u16 company, u32 game, u8* name, u8* ext, int length, s32* fileNo) {
    (void) pfs; (void) company; (void) game; (void) name; (void) ext; (void) length; (void) fileNo;
    return PFS_ERR_NOPACK;
}
s32 osPfsReadWriteFile(OSPfs* pfs, s32 fileNo, u8 flag, int offset, int size, u8* data) {
    (void) pfs; (void) fileNo; (void) flag; (void) offset; (void) size; (void) data;
    return PFS_ERR_NOPACK;
}

/* -------------------------------------------------------------------- PI --- */
static OSPiHandle sCartHandle;
static OSPiHandle sDriveHandle;

/* SRAM: 32 KB, a file in the data dir, written whole and atomically. The image is game memory (save
 * states hold it); the file side, which a state load must not change, is port_misc.c's: an
 * unreadable fzerox.sram is never written over, and a failed write shows GAME SAVE FAILED. */
#define SRAM_SIZE 0x8000
static u8 sSram[SRAM_SIZE];
static s32 sSramLoaded;
/* The game has read the SRAM (its boot read, func_i10_80115E30) in this timeline. In the state with sSram
 * (statefix, 2026-10-09): sSramLoaded is always 1 in a state (sram_load runs at port_os_init, before the
 * game), so a state made before the game's boot read carries the SRAM image of the process that made it;
 * port_misc.c's hh_game_save_reapply puts the card's image there, and the restored boot reads that. */
static s32 sSramGameRead;

extern size_t port_sram_read(void* img, size_t size);
extern void port_sram_write(const void* img, size_t size);

static void sram_load(void) {
    if (sSramLoaded) {
        return;
    }
    sSramLoaded = 1;
    memset(sSram, 0, sizeof(sSram));
    (void) port_sram_read(sSram, sizeof(sSram)); /* fzerox.sram, or the .tmp of a rename that did not finish */
}

static void sram_access(s32 direction, u32 offset, void* dram, u32 size) {
    sram_load();
    offset &= 0x7FFFF;
    if (offset >= SRAM_SIZE) {
        return;
    }
    if (offset + size > SRAM_SIZE) {
        size = SRAM_SIZE - offset;
    }
    if (direction == OS_READ) {
        memcpy(dram, sSram + offset, size);
        sSramGameRead = 1;
    } else {
        memcpy(sSram + offset, dram, size);
        port_sram_write(sSram, sizeof(sSram));
    }
}

/* port_misc.c's hh_game_save_reapply (statefix A): the SRAM image a load restored, and whether the game
 * had read it by then. */
u8* port_sram_image(void) {
    return sSram;
}
s32 port_sram_game_read(void) {
    return sSramGameRead;
}

OSPiHandle* osCartRomInit(void) {
    sCartHandle.type = DEVICE_TYPE_CART;
    sCartHandle.baseAddress = 0xB0000000;
    return &sCartHandle;
}

OSPiHandle* osDriveRomInit(void) {
    sDriveHandle.type = DEVICE_TYPE_64DD;
    sDriveHandle.baseAddress = 0xA5000000;
    return &sDriveHandle;
}

s32 osEPiLinkHandle(OSPiHandle* handle) { (void) handle; return 0; }
void osCreatePiManager(OSPri pri, OSMesgQueue* cmdQ, OSMesg* cmdBuf, s32 cmdMsgCnt) {
    (void) pri; (void) cmdQ; (void) cmdBuf; (void) cmdMsgCnt;
}

s32 osEPiStartDma(OSPiHandle* handle, OSIoMesg* mb, s32 direction) {
    if (handle == &sCartHandle) {
        if (direction == OS_READ) {
            port_rom_dma(mb->devAddr, mb->dramAddr, mb->size);
        }
    } else if (handle != NULL && (handle->baseAddress & 0x1FFFFFFF) == 0x08000000) {
        sram_access(direction, mb->devAddr, mb->dramAddr, mb->size);
    }
    if (mb->hdr.retQueue != NULL) {
        osSendMesg(mb->hdr.retQueue, (OSMesg) mb, OS_MESG_NOBLOCK);
    }
    return 0;
}

static OSPiHandle sLeoHandle;

OSPiHandle* osLeoDiskInit(void) {
    sLeoHandle.type = DEVICE_TYPE_64DD;
    sLeoHandle.baseAddress = 0xA5000000;
    return &sLeoHandle;
}

s32 osEPiReadIo(OSPiHandle* handle, u32 devAddr, u32* data) {
    (void) devAddr;
    /* No 64DD: every presence bit set (LeoDD_CheckPresence reads "absent"). */
    *data = (handle == &sLeoHandle || handle == &sDriveHandle) ? 0xFFFFFFFF : 0;
    return 0;
}

s32 osEPiWriteIo(OSPiHandle* handle, u32 devAddr, u32 data) {
    (void) handle; (void) devAddr; (void) data;
    return 0;
}

/* ------------------------------------------------------------ misc system --- */
s32 osTvType = OS_TV_NTSC;
s32 osResetType = 0;
u32 osMemSize = 0x400000;
s32 osAppNMIBuffer[16];

void osInitialize(void) { }
u32 osGetMemSize(void) { return 0x400000; }
OSIntMask osGetIntMask(void) { return 0; }
OSIntMask osSetIntMask(OSIntMask mask) { (void) mask; return 0; }
void __osSetHWIntrRoutine(OSHWIntr intr, s32 (*handler)(void), void* sp) { (void) intr; (void) handler; (void) sp; }

void osInvalDCache(void* p, s32 n) { (void) p; (void) n; }
void osInvalICache(void* p, s32 n) { (void) p; (void) n; }
void osWritebackDCache(void* p, s32 n) { (void) p; (void) n; }
void osWritebackDCacheAll(void) { }

/* The RDRAM block is addressed natively: no KSEG0/physical split. */
u32 osVirtualToPhysical(void* addr) { return (u32) (uintptr_t) addr; }
void* osPhysicalToVirtual(u32 addr) { return (void*) (uintptr_t) addr; }

/* The idle thread's `while (true) {}` (sys_main.c, HANDHELD-patched): every other
 * thread is blocked, so give the host its turn. */
void port_idle(void) {
    port_os_block_on_host();
}

/* RSP microcode images: never executed, but the scheduler takes their
 * addresses (task->ucode) and G_LOAD_UCODE commands name them, so each needs a
 * distinct address the renderer can recognise (see gfx_pc.c). */
long long int rspbootTextStart[2], rspbootTextEnd[2];
long long int gspF3DEX2_fifoTextStart[2], gspF3DEX2_fifoDataStart[2];
long long int gspF3DLX2_Rej_fifoTextStart[2], gspF3DLX2_Rej_fifoDataStart[2];
long long int gspF3DFLX2_Rej_fifoTextStart[2], gspF3DFLX2_Rej_fifoDataStart[2];
long long int aspMainTextStart[2], aspMainDataStart[2], aspMainDataEnd[2];

/* The one float libm constant the game references (a quiet NaN). */
const float __libm_qnan_f = __builtin_nanf("");

void port_os_init(void) {
    sram_load();
}

/* VI mode tables the idle thread hands to osViSetMode (contents unused). */
OSViMode osViModeNtscLan1;
OSViMode osViModeMpalLan1;

/* libultra's formatter core, used by the crash screen (Fault_Printf). */
int _Printf(void* (*prout)(void*, const char*, size_t), char* arg, const char* fmt, va_list args) {
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n < 0) {
        return n;
    }
    if (n >= (int) sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    prout(arg, buf, (size_t) n);
    return n;
}
