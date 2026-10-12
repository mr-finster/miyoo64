#include "ultra64.h"

#include "attributes.h"

#include <pspctrl.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <psprtc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "oot_psp_audio_backend.h"
#include "oot_psp_controls.h"
#include "versions.h"
#include "hh_game.h"

#define OOT_PSP_MAX_THREADS 16
#define OOT_PSP_MAX_TIMERS  16

/*
 * PSP note:
 *   Valid SceUID thread/sema ids are positive.
 *   0 is not a valid thread id for sceKernelStartThread().
 *
 * The old timer backend created one PSP thread per osSetTimer() call. If the
 * same OSTimer was reset repeatedly, the old thread id could be overwritten
 * and leaked. This replacement uses one timer service thread for all OSTimers.
 */
#define OOT_PSP_INVALID_UID (-1)

typedef struct {
    OSThread* thread;
    SceUID sceThreadId;
    void (*entry)(void*);
    void* arg;
} OotPspThreadSlot;

typedef struct {
    OSTimer* timer;
    OSMesgQueue* mq;
    OSMesg msg;
    u64 dueUsec;
    u64 intervalUsec;
    volatile s32 active;
} OotPspTimerSlot;

typedef struct {
    OSMesgQueue* mq;
    OSMesg msg;
} OotPspPendingTimerMsg;

static OotPspThreadSlot sThreadSlots[OOT_PSP_MAX_THREADS];
static OotPspTimerSlot sTimerSlots[OOT_PSP_MAX_TIMERS];
static OSContPad sControllerPads[MAXCONTROLLERS];
static OSContStatus sControllerStatus[MAXCONTROLLERS];
static OSMesgQueue* sPiCmdQueue;
static OSMesgQueue* sViEventQueue;
static OSMesg sViEventMsg;
static u32 sIntMask = OS_IM_ALL;
static void* sCurrentFramebuffer;
static void* sNextFramebuffer;
static OSViMode* sCurrentViMode;
static OSViContext sCurrentViContext;

static SceUID sTimerServiceThreadId = OOT_PSP_INVALID_UID;
static volatile s32 sTimerServiceRunning;
/* The count register the audio engine mixes into its random numbers (osGetCount): machine
 * state the game observes, so a save state holds it (hh_game.h) and a loaded game draws the
 * same numbers the saved one would have. */
static volatile u32 sFastCount HH_GS_BSS;
static volatile s32 sFastCountInitialized HH_GS_BSS;

s32 osRomType = 0;
void* osRomBase = NULL;
#if OOT_PAL_N64
s32 osTvType = OS_TV_PAL;
#else
s32 osTvType = OS_TV_NTSC;
#endif
s32 osResetType = 0;
s32 osCicId = 0;
s32 osVersion = 0;
u32 osMemSize = 0x02000000;
s32 osAppNMIBuffer[0x10];
u64 osClockRate = OS_CLOCK_RATE;
__osHwInt __osHwIntTable[1];
OSIntMask __OSGlobalIntMask = OS_IM_ALL;
OSPiHandle* __osPiTable = NULL;
u8 __osContLastCmd = CONT_CMD_READ_BUTTON;
OSPifRam __osContPifRam;
OSPifRam __osPfsPifRam;
u8 __osMaxControllers = 1;

static s32 OotPsp_IsValidUid(SceUID uid) {
    return uid > 0;
}

static u64 OotPsp_GetUsec(void) {
    return sceKernelGetSystemTimeWide();
}

static u64 OotPsp_CyclesToUsec(OSTime cycles) {
    if (cycles == 0) {
        return 0;
    }

    return (u64)OS_CYCLES_TO_USEC(cycles);
}

static void OotPsp_ClearThreadSlot(OotPspThreadSlot* slot) {
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
        slot->sceThreadId = OOT_PSP_INVALID_UID;
    }
}

static void OotPsp_ClearTimerSlot(OotPspTimerSlot* slot) {
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
}

static OotPspThreadSlot* OotPsp_FindThreadSlot(OSThread* thread) {
    s32 i;

    for (i = 0; i < OOT_PSP_MAX_THREADS; i++) {
        if (sThreadSlots[i].thread == thread) {
            return &sThreadSlots[i];
        }
    }

    return NULL;
}

static OotPspThreadSlot* OotPsp_FindThreadSlotBySceId(SceUID sceThreadId) {
    s32 i;

    if (!OotPsp_IsValidUid(sceThreadId)) {
        return NULL;
    }

    for (i = 0; i < OOT_PSP_MAX_THREADS; i++) {
        if (sThreadSlots[i].sceThreadId == sceThreadId) {
            return &sThreadSlots[i];
        }
    }

    return NULL;
}

static OotPspThreadSlot* OotPsp_AllocThreadSlot(OSThread* thread) {
    s32 i;

    for (i = 0; i < OOT_PSP_MAX_THREADS; i++) {
        if (sThreadSlots[i].thread == NULL) {
            OotPsp_ClearThreadSlot(&sThreadSlots[i]);
            sThreadSlots[i].thread = thread;
            return &sThreadSlots[i];
        }
    }

    return NULL;
}

static int OotPsp_ThreadTrampoline(SceSize args, void* argp) {
    OSThread* thread = (OSThread*)argp;
    OotPspThreadSlot* slot = OotPsp_FindThreadSlot(thread);

    (void)args;

    if (slot != NULL && slot->entry != NULL) {
        thread->state = OS_STATE_RUNNING;
        slot->entry(slot->arg);
        thread->state = OS_STATE_STOPPED;
        OotPsp_ClearThreadSlot(slot);
    }

    sceKernelExitDeleteThread(0);
    return 0;
}

static OotPspTimerSlot* OotPsp_FindTimerSlot(OSTimer* timer) {
    s32 i;

    if (timer == NULL) {
        return NULL;
    }

    for (i = 0; i < OOT_PSP_MAX_TIMERS; i++) {
        if (sTimerSlots[i].timer == timer) {
            return &sTimerSlots[i];
        }
    }

    return NULL;
}

static OotPspTimerSlot* OotPsp_AllocTimerSlot(OSTimer* timer) {
    s32 i;

    for (i = 0; i < OOT_PSP_MAX_TIMERS; i++) {
        if (sTimerSlots[i].timer == NULL) {
            OotPsp_ClearTimerSlot(&sTimerSlots[i]);
            sTimerSlots[i].timer = timer;
            return &sTimerSlots[i];
        }
    }

    return NULL;
}

/* Timers and message queues block on condition variables (handheld). The PSP
 * layer polled: every blocked osRecvMesg/osSendMesg slept 1 ms and retried,
 * and the timer service woke every 1 ms even between the retrace timer's
 * 16.6 ms events -- ~1000 wakeups a second per parked thread, which the
 * device profile showed as ~6% of the CPU in mutex/clock/TLS overhead. Now a
 * send or receive wakes the waiters, and the timer service sleeps until its
 * next deadline (osSetTimer/osStopTimer wake it early). */
static pthread_mutex_t sTimerMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sTimerCond;
static u32 sTimerGen; /* bumped by osSetTimer/osStopTimer, under sTimerMutex */
static pthread_mutex_t sMesgMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sMesgCond;

/* Both conditions time out on CLOCK_MONOTONIC (a wall-clock step must not
 * stretch a timer sleep). Initialised before main, so before any thread can
 * wait on them -- re-initialising a condition that has waiters is undefined. */
__attribute__((constructor)) static void HhCond_Init(void) {
    pthread_condattr_t attr;

    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&sTimerCond, &attr);
    pthread_cond_init(&sMesgCond, &attr);
    pthread_condattr_destroy(&attr);
}

/* Absolute CLOCK_MONOTONIC deadline `usec` from now (pthread_cond_timedwait);
 * port_time_us64 is that clock in one syscall on the device. */
static struct timespec HhDeadlineIn(u32 usec) {
    extern uint64_t port_time_us64(void);
    const uint64_t at = port_time_us64() + usec;
    struct timespec ts;

    ts.tv_sec = (time_t)(at / 1000000u);
    ts.tv_nsec = (long)(at % 1000000u) * 1000L;
    return ts;
}

static s32 OotPsp_EnsureTimerLock(void) {
    return 0;
}

static void OotPsp_LockTimers(void) {
    pthread_mutex_lock(&sTimerMutex);
}

static void OotPsp_UnlockTimers(void) {
    pthread_mutex_unlock(&sTimerMutex);
}

static int OotPsp_TimerServiceThread(SceSize args, void* argp) {
    (void)args;
    (void)argp;

    while (sTimerServiceRunning) {
        OotPspPendingTimerMsg pending[OOT_PSP_MAX_TIMERS];
        s32 pendingCount = 0;
        s32 anyActive = false;
        u64 now = OotPsp_GetUsec();
        u32 sleepUsec = 100000;
        u32 gen;
        s32 i;

        OotPsp_LockTimers();
        gen = sTimerGen;

        for (i = 0; i < OOT_PSP_MAX_TIMERS; i++) {
            OotPspTimerSlot* slot = &sTimerSlots[i];

            if (!slot->active || slot->timer == NULL) {
                continue;
            }

            anyActive = true;

            if (slot->dueUsec <= now) {
                if (slot->mq != NULL && pendingCount < OOT_PSP_MAX_TIMERS) {
                    pending[pendingCount].mq = slot->mq;
                    pending[pendingCount].msg = slot->msg;
                    pendingCount++;
                }

                if (slot->intervalUsec != 0) {
                    /*
                     * Keep only one event per service wake. This prevents a
                     * delayed frame from causing a huge timer catch-up burst.
                     */
                    slot->dueUsec = now + slot->intervalUsec;
                } else {
                    OotPsp_ClearTimerSlot(slot);
                    continue;
                }
            }
        }

        for (i = 0; i < OOT_PSP_MAX_TIMERS; i++) {
            OotPspTimerSlot* slot = &sTimerSlots[i];

            if (slot->active && slot->timer != NULL) {
                u64 delta = (slot->dueUsec > now) ? (slot->dueUsec - now) : 0;

                anyActive = true;
                if (delta < sleepUsec) {
                    sleepUsec = (u32)delta;
                }
            }
        }

        OotPsp_UnlockTimers();

        for (i = 0; i < pendingCount; i++) {
            osSendMesg(pending[i].mq, pending[i].msg, OS_MESG_NOBLOCK);
        }

        {
            struct timespec deadline;

            if (!anyActive) {
                sleepUsec = 100000; /* idle: osSetTimer wakes us */
            } else if (sleepUsec < 100) {
                sleepUsec = 100;
            }
            deadline = HhDeadlineIn(sleepUsec);
            OotPsp_LockTimers();
            /* A timer set or stopped while the messages above went out would
             * have signalled nobody: rescan instead of sleeping on stale data. */
            if (gen == sTimerGen && sTimerServiceRunning) {
                pthread_cond_timedwait(&sTimerCond, &sTimerMutex, &deadline);
            }
            OotPsp_UnlockTimers();
        }
    }

    sTimerServiceThreadId = OOT_PSP_INVALID_UID;
    sceKernelExitDeleteThread(0);
    return 0;
}

static s32 OotPsp_EnsureTimerService(void) {
    SceUID threadId;
    s32 ret;

    if (OotPsp_EnsureTimerLock() < 0) {
        return -1;
    }

    if (OotPsp_IsValidUid(sTimerServiceThreadId)) {
        return 0;
    }

    sTimerServiceRunning = true;

    threadId = sceKernelCreateThread("oot-timers", OotPsp_TimerServiceThread, 0x18, 0x2000, THREAD_ATTR_USER, NULL);
    if (!OotPsp_IsValidUid(threadId)) {
        sTimerServiceRunning = false;
        sTimerServiceThreadId = OOT_PSP_INVALID_UID;
        return -1;
    }

    ret = sceKernelStartThread(threadId, 0, NULL);
    if (ret < 0) {
        sceKernelDeleteThread(threadId);
        sTimerServiceRunning = false;
        sTimerServiceThreadId = OOT_PSP_INVALID_UID;
        return -1;
    }

    sTimerServiceThreadId = threadId;
    return 0;
}

static void OotPsp_StopTimerService(void) {
    SceUID threadId = sTimerServiceThreadId;

    OotPsp_LockTimers();
    sTimerServiceRunning = false;
    sTimerServiceThreadId = OOT_PSP_INVALID_UID;
    pthread_cond_signal(&sTimerCond);
    OotPsp_UnlockTimers();

    if (OotPsp_IsValidUid(threadId) && threadId != sceKernelGetThreadId()) {
        sceKernelTerminateDeleteThread(threadId);
    }
}

static s32 OotPsp_EnsureMesgQueueLock(void) {
    return 0;
}

static void OotPsp_StopMesgQueueLock(void) {
}

static void OotPsp_LockMesgQueue(UNUSED OSMesgQueue* mq) {
    pthread_mutex_lock(&sMesgMutex);
}

static void OotPsp_UnlockMesgQueue(UNUSED OSMesgQueue* mq) {
    pthread_mutex_unlock(&sMesgMutex);
}

/* Called with the queue lock held: wait for any queue to change (a send, a
 * receive or a jam broadcasts). The timeout only bounds a missed wake-up. */
static void HhMesg_Wait(void) {
    struct timespec deadline = HhDeadlineIn(50000);

    pthread_cond_timedwait(&sMesgCond, &sMesgMutex, &deadline);
}

/* The scene-load and actor-spawn step trace ("oot-psp play ...", "actor ...", "game arena/realloc ...") is the
 * bring-up trace: off in normal play, back with OOT_LOGVERBOSE=1 (debug.cfg: logverbose=1). Errors and the
 * other "oot-psp" lines (assets, saves, audio guards) always print. */
static int HhLogTraceOn(const char* fmt) {
    static int sVerbose = -1;

    if (strncmp(fmt, "oot-psp play ", 13) != 0 && strncmp(fmt, "oot-psp actor ", 14) != 0 &&
        strncmp(fmt, "oot-psp game ", 13) != 0) {
        return 1;
    }
    if (sVerbose < 0) {
        const char* e = getenv("OOT_LOGVERBOSE");

        sVerbose = e != NULL && e[0] == '1';
    }
    return sVerbose;
}

void osSyncPrintf(const char* fmt, ...) {
    va_list args;

    if (!HhLogTraceOn(fmt)) {
        return;
    }
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void isPrintfInit(void) {
}

void __osInitialize_common(void) {
    s32 i;

    OotPsp_StopTimerService();
    OotPsp_StopMesgQueueLock();

    memset(sThreadSlots, 0, sizeof(sThreadSlots));
    memset(sTimerSlots, 0, sizeof(sTimerSlots));
    memset(sControllerPads, 0, sizeof(sControllerPads));
    memset(sControllerStatus, 0, sizeof(sControllerStatus));
    memset(&sCurrentViContext, 0, sizeof(sCurrentViContext));

    for (i = 0; i < OOT_PSP_MAX_THREADS; i++) {
        sThreadSlots[i].sceThreadId = OOT_PSP_INVALID_UID;
    }

    osMemSize = 0x02000000;
    osClockRate = OS_CLOCK_RATE;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    OotPsp_EnsureMesgQueueLock();
}

void __osInitialize_autodetect(void) {
}

u32 osGetMemSize(void) {
    return osMemSize;
}

void osCreateMesgQueue(OSMesgQueue* mq, OSMesg* msgBuf, s32 count) {
    if (mq == NULL) {
        return;
    }

    /*
     * Do not create one PSP semaphore per OSMesgQueue.
     *
     * Libultra-style queues often do not have a matching destroy call, and OOT
     * creates queues in several hot paths. A per-queue sceKernelCreateSema()
     * leaks kernel objects until PPSSPP/PSP reports:
     *   "Unable to allocate kernel object, too many objects slots in use."
     *
     * mtqueue/fullqueue are not used as real wait queues in this PSP shim.
     * Synchronization is handled by one shared PSP semaphore.
     */
    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msgBuf;

    if (count <= 0 || msgBuf == NULL) {
        return;
    }

    OotPsp_EnsureMesgQueueLock();
}

s32 osSendMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    if (mq == NULL || mq->msg == NULL || mq->msgCount <= 0) {
        return -1;
    }

    OotPsp_LockMesgQueue(mq);
    while (MQ_IS_FULL(mq)) {
        if (flag == OS_MESG_NOBLOCK) {
            OotPsp_UnlockMesgQueue(mq);
            return -1;
        }
        HhMesg_Wait();
    }
    mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    mq->validCount++;
    pthread_cond_broadcast(&sMesgCond);
    OotPsp_UnlockMesgQueue(mq);
    return 0;
}

s32 osJamMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    if (mq == NULL || mq->msg == NULL || mq->msgCount <= 0) {
        return -1;
    }

    OotPsp_LockMesgQueue(mq);
    while (MQ_IS_FULL(mq)) {
        if (flag == OS_MESG_NOBLOCK) {
            OotPsp_UnlockMesgQueue(mq);
            return -1;
        }
        HhMesg_Wait();
    }
    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    mq->msg[mq->first] = msg;
    mq->validCount++;
    pthread_cond_broadcast(&sMesgCond);
    OotPsp_UnlockMesgQueue(mq);
    return 0;
}

s32 osRecvMesg(OSMesgQueue* mq, OSMesg* msg, s32 flag) {
    if (mq == NULL || mq->msg == NULL || mq->msgCount <= 0) {
        return -1;
    }

    OotPsp_LockMesgQueue(mq);
    while (MQ_IS_EMPTY(mq)) {
        if (flag == OS_MESG_NOBLOCK) {
            OotPsp_UnlockMesgQueue(mq);
            return -1;
        }
        HhMesg_Wait();
    }
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }

    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    pthread_cond_broadcast(&sMesgCond);
    OotPsp_UnlockMesgQueue(mq);
    return 0;
}

void osSetEventMesg(UNUSED OSEvent e, UNUSED OSMesgQueue* mq, UNUSED OSMesg msg) {
}

void osCreateThread(OSThread* thread, OSId id, void (*entry)(void*), void* arg, void* sp, OSPri pri) {
    OotPspThreadSlot* slot;
    SceUID threadId;
    char name[32];

    if (thread == NULL) {
        return;
    }

    slot = OotPsp_FindThreadSlot(thread);
    if (slot != NULL && OotPsp_IsValidUid(slot->sceThreadId)) {
        sceKernelTerminateDeleteThread(slot->sceThreadId);
        OotPsp_ClearThreadSlot(slot);
    }

    memset(thread, 0, sizeof(*thread));
    thread->id = id;
    thread->priority = pri;
    thread->state = OS_STATE_STOPPED;
    thread->context.sp = (u32)(uintptr_t)sp;

    slot = OotPsp_AllocThreadSlot(thread);
    if (slot == NULL) {
        return;
    }

    snprintf(name, sizeof(name), "oot-thr-%ld", (long)id);

    slot->entry = entry;
    slot->arg = arg;
    slot->sceThreadId = OOT_PSP_INVALID_UID;

    threadId = sceKernelCreateThread(name, OotPsp_ThreadTrampoline, 0x20 + (OS_PRIORITY_APPMAX - pri), 0x20000,
                                     THREAD_ATTR_USER | THREAD_ATTR_VFPU, NULL);
    if (!OotPsp_IsValidUid(threadId)) {
        OotPsp_ClearThreadSlot(slot);
        return;
    }

    slot->sceThreadId = threadId;
}

void osStartThread(OSThread* thread) {
    OotPspThreadSlot* slot;
    s32 ret;

    if (thread == NULL) {
        return;
    }

    slot = OotPsp_FindThreadSlot(thread);
    if (slot == NULL || !OotPsp_IsValidUid(slot->sceThreadId)) {
        return;
    }

    if (thread->state == OS_STATE_RUNNING || thread->state == OS_STATE_RUNNABLE) {
        return;
    }

    thread->state = OS_STATE_RUNNABLE;

    ret = sceKernelStartThread(slot->sceThreadId, sizeof(thread), thread);
    if (ret < 0) {
        thread->state = OS_STATE_STOPPED;
    }
}

void osDestroyThread(OSThread* thread) {
    OotPspThreadSlot* slot;
    SceUID threadId;

    if (thread == NULL) {
        threadId = sceKernelGetThreadId();
        slot = OotPsp_FindThreadSlotBySceId(threadId);
        if (slot != NULL) {
            if (slot->thread != NULL) {
                slot->thread->state = OS_STATE_STOPPED;
            }
            OotPsp_ClearThreadSlot(slot);
        }

        sceKernelExitDeleteThread(0);
        return;
    }

    slot = OotPsp_FindThreadSlot(thread);
    if (slot == NULL) {
        thread->state = OS_STATE_STOPPED;
        return;
    }

    threadId = slot->sceThreadId;

    OotPsp_ClearThreadSlot(slot);
    thread->state = OS_STATE_STOPPED;

    if (OotPsp_IsValidUid(threadId)) {
        if (threadId == sceKernelGetThreadId()) {
            sceKernelExitDeleteThread(0);
        } else {
            sceKernelTerminateDeleteThread(threadId);
        }
    }
}

void osStopThread(OSThread* thread) {
    osDestroyThread(thread);
}

void osYieldThread(void) {
    sceKernelDelayThread(0);
}

void osSetThreadPri(OSThread* thread, OSPri pri) {
    OotPspThreadSlot* slot = OotPsp_FindThreadSlot(thread);

    if (thread != NULL) {
        thread->priority = pri;
    }

    if (slot != NULL && OotPsp_IsValidUid(slot->sceThreadId)) {
        sceKernelChangeThreadPriority(slot->sceThreadId, 0x20 + (OS_PRIORITY_APPMAX - pri));
    }
}

OSPri osGetThreadPri(OSThread* thread) {
    if (thread == NULL) {
        return 0;
    }

    return thread->priority;
}

OSId osGetThreadId(OSThread* thread) {
    if (thread == NULL) {
        return sceKernelGetThreadId();
    }

    return thread->id;
}

OSThread* __osGetActiveQueue(void) {
    return NULL;
}

OSThread* __osGetCurrFaultedThread(void) {
    return NULL;
}

OSTime osGetTime(void) {
    u64 ticks = 0;

    sceRtcGetCurrentTick(&ticks);
    return ticks;
}

void osSetTime(UNUSED OSTime time) {
}

u32 osGetCount(void) {
    if (!sFastCountInitialized) {
        sFastCount = sceKernelGetSystemTimeLow();
        sFastCountInitialized = true;
    }

    /*
     * PSP timing and OSTimer services use their own microsecond clock. The game
     * calls osGetCount only as a changing entropy source, so avoid a kernel
     * syscall on every audio random update.
     */
    /*
     * This value is only mixed into the audio PRNG; it is not a clock and no
     * caller depends on observing every increment.  An atomic add emits an
     * LL/SC retry loop plus memory barriers on Allegrex.  Audio and rendering
     * share one PSP core, so that synchronization can steal a disproportionate
     * amount of frame time when a busy scene also drives frequent audio
     * updates.  A volatile word update preserves the changing entropy source
     * without cross-thread serialization.
     */
    sFastCount += 0x9E3779B9U;
    return sFastCount;
}

s32 osSetTimer(OSTimer* timer, OSTime countdown, OSTime interval, OSMesgQueue* mq, OSMesg msg) {
    OotPspTimerSlot* slot;
    u64 now;

    if (timer == NULL) {
        return -1;
    }

    if (OotPsp_EnsureTimerService() < 0) {
        return -1;
    }

    now = OotPsp_GetUsec();

    OotPsp_LockTimers();

    slot = OotPsp_FindTimerSlot(timer);
    if (slot == NULL) {
        slot = OotPsp_AllocTimerSlot(timer);
    }

    if (slot == NULL) {
        OotPsp_UnlockTimers();
        return -1;
    }

    timer->value = countdown;
    timer->interval = interval;
    timer->mq = mq;
    timer->msg = msg;

    slot->timer = timer;
    slot->mq = mq;
    slot->msg = msg;
    slot->dueUsec = now + OotPsp_CyclesToUsec(countdown);
    slot->intervalUsec = OotPsp_CyclesToUsec(interval);
    slot->active = true;

    sTimerGen++;
    pthread_cond_signal(&sTimerCond); /* the service may be sleeping past this deadline */
    OotPsp_UnlockTimers();
    return 0;
}

s32 osStopTimer(OSTimer* timer) {
    OotPspTimerSlot* slot;

    if (timer == NULL) {
        return -1;
    }

    if (OotPsp_EnsureTimerLock() < 0) {
        return -1;
    }

    OotPsp_LockTimers();

    slot = OotPsp_FindTimerSlot(timer);
    if (slot == NULL) {
        OotPsp_UnlockTimers();
        return -1;
    }

    OotPsp_ClearTimerSlot(slot);

    OotPsp_UnlockTimers();
    return 0;
}

OSIntMask osGetIntMask(void) {
    return sIntMask;
}

OSIntMask osSetIntMask(OSIntMask mask) {
    OSIntMask prev = sIntMask;

    sIntMask = mask;
    return prev;
}

s32 __osDisableInt(void) {
    return 0;
}

void __osRestoreInt(UNUSED s32 enable) {
}

void __osSetHWIntrRoutine(UNUSED OSHWIntr intr, UNUSED s32 (*callback)(void), UNUSED void* sp) {
}

void __osGetHWIntrRoutine(UNUSED OSHWIntr intr, s32 (**callbackOut)(void), void** spOut) {
    if (callbackOut != NULL) {
        *callbackOut = NULL;
    }
    if (spOut != NULL) {
        *spOut = NULL;
    }
}

void __osSetSR(UNUSED u32 value) {
}

u32 __osGetSR(void) {
    return 0;
}

void __osSetFpcCsr(UNUSED u32 value) {
}

u32 __osGetFpcCsr(void) {
    return 0;
}

u32 __osGetCause(void) {
    return 0;
}

void __osSetCompare(UNUSED u32 value) {
}

void __osSetWatchLo(UNUSED u32 value) {
}

u32 __osProbeTLB(UNUSED void* addr) {
    return 0;
}

void osUnmapTLBAll(void) {
}

void osMapTLBRdb(void) {
}

void osWritebackDCache(UNUSED void* vaddr, UNUSED s32 nbytes) {
}

void osInvalDCache(UNUSED void* vaddr, UNUSED s32 nbytes) {
}

void osWritebackDCacheAll(void) {
}

void osInvalICache(UNUSED void* vaddr, UNUSED s32 nbytes) {
}

u32 osVirtualToPhysical(void* vaddr) {
    return (u32)(uintptr_t)vaddr;
}

void osCreatePiManager(OSPri pri, OSMesgQueue* cmdQueue, OSMesg* cmdBuf, s32 cmdMsgCnt) {
    (void)pri;
    sPiCmdQueue = cmdQueue;
    osCreateMesgQueue(cmdQueue, cmdBuf, cmdMsgCnt);
}

OSMesgQueue* osPiGetCmdQueue(void) {
    return sPiCmdQueue;
}

static OSPiHandle sCartHandle;

OSPiHandle* osCartRomInit(void) {
    memset(&sCartHandle, 0, sizeof(sCartHandle));
    sCartHandle.type = DEVICE_TYPE_CART;
    return &sCartHandle;
}

OSPiHandle* osDriveRomInit(void) {
    return &sCartHandle;
}

s32 osEPiStartDma(OSPiHandle* handle, OSIoMesg* mb, s32 direction) {
    (void)handle;
    (void)direction;

    if (mb != NULL && mb->dramAddr != NULL && mb->devAddr != 0 && mb->size != 0) {
        memcpy(mb->dramAddr, (void*)(uintptr_t)mb->devAddr, mb->size);
    }

    if (mb != NULL && mb->hdr.retQueue != NULL) {
        osSendMesg(mb->hdr.retQueue, mb, OS_MESG_NOBLOCK);
    }

    return 0;
}

s32 osPiStartDma(OSIoMesg* mb, s32 priority, s32 direction, uintptr_t devAddr, void* vAddr, size_t nbytes,
                 OSMesgQueue* mq) {
    (void)priority;
    (void)direction;

    if (vAddr != NULL && devAddr != 0 && nbytes != 0) {
        memcpy(vAddr, (const void*)devAddr, nbytes);
    }

    if (mq != NULL) {
        osSendMesg(mq, mb, OS_MESG_NOBLOCK);
    }

    return 0;
}

void osCreateViManager(UNUSED OSPri pri) {
}

void osViSetMode(OSViMode* mode) {
    sCurrentViMode = mode;
    sCurrentViContext.modep = mode;
}

void osViBlack(UNUSED u8 active) {
}

void osViSetSpecialFeatures(UNUSED u32 func) {
}

void osViSwapBuffer(void* frameBufPtr) {
    sCurrentFramebuffer = frameBufPtr;
    sNextFramebuffer = frameBufPtr;
    sCurrentViContext.framep = frameBufPtr;
}

void* osViGetCurrentFramebuffer(void) {
    return sCurrentFramebuffer;
}

void* osViGetNextFramebuffer(void) {
    return sNextFramebuffer;
}

void osViSetEvent(OSMesgQueue* mq, OSMesg msg, u32 retraceCount) {
    sViEventQueue = mq;
    sViEventMsg = msg;
    sCurrentViContext.mq = mq;
    sCurrentViContext.msg = msg;
    sCurrentViContext.retraceCount = retraceCount;
}

void osViSetYScale(f32 scale) {
    sCurrentViContext.y.factor = scale;
}

void osViSetXScale(f32 value) {
    sCurrentViContext.x.factor = value;
}

void osViExtendVStart(UNUSED u32 value) {
}

OSViContext* __osViGetCurrentContext(void) {
    return &sCurrentViContext;
}

s32 osContInit(OSMesgQueue* mq, u8* ctlBitfield, OSContStatus* status) {
    s32 i;

    (void)mq;

    for (i = 0; i < MAXCONTROLLERS; i++) {
        sControllerStatus[i].type = 0;
        sControllerStatus[i].status = 0;
        sControllerStatus[i].errno = CONT_ERR_NO_CONTROLLER;
    }

    sControllerStatus[0].type = CONT_TYPE_NORMAL;
    sControllerStatus[0].status = 0;
    sControllerStatus[0].errno = 0;

    if (ctlBitfield != NULL) {
        *ctlBitfield = 1;
    }
    if (status != NULL) {
        memcpy(status, sControllerStatus, sizeof(sControllerStatus));
    }

    return 0;
}

s32 osContStartQuery(UNUSED OSMesgQueue* mq) {
    return 0;
}

void osContGetQuery(OSContStatus* data) {
    if (data != NULL) {
        memcpy(data, sControllerStatus, sizeof(sControllerStatus));
    }
}

s32 osContSetCh(UNUSED u8 ch) {
    return 0;
}

s32 osContStartReadData(UNUSED OSMesgQueue* mq) {
    SceCtrlData pad;

    memset(sControllerPads, 0, sizeof(sControllerPads));
    sceCtrlPeekBufferPositive(&pad, 1);

    sControllerPads[0].button = OotPspControls_MapButtons(pad.Buttons);
    sControllerPads[0].stick_x = OotPspControls_MapStick(pad.Lx);
    sControllerPads[0].stick_y = -OotPspControls_MapStick(pad.Ly);
    sControllerPads[0].errno = 0;

    return 0;
}

void osContGetReadData(OSContPad* contData) {
    if (contData != NULL) {
        memcpy(contData, sControllerPads, sizeof(sControllerPads));
    }
}

s32 osPfsIsPlug(UNUSED OSMesgQueue* mq, u8* pattern) {
    if (pattern != NULL) {
        *pattern = 0;
    }
    return PFS_ERR_NOPACK;
}

s32 osPfsInitPak(UNUSED OSMesgQueue* queue, UNUSED OSPfs* pfs, UNUSED s32 channel) {
    return PFS_ERR_NOPACK;
}

s32 osMotorInit(UNUSED OSMesgQueue* ctrlrqueue, UNUSED OSPfs* pfs, UNUSED s32 channel) {
    return PFS_ERR_DEVICE;
}

s32 __osMotorAccess(UNUSED OSPfs* pfs, UNUSED s32 vibrate) {
    return PFS_ERR_DEVICE;
}

s32 osAfterPreNMI(void) {
    return 0;
}

void osSpTaskLoad(UNUSED OSTask* intp) {
}

void osSpTaskStartGo(UNUSED OSTask* tp) {
    if (sViEventQueue != NULL) {
        osSendMesg(sViEventQueue, sViEventMsg, OS_MESG_NOBLOCK);
    }
}

void osSpTaskYield(void) {
}

u32 osSpTaskYielded(UNUSED OSTask* task) {
    return 0;
}

u32 osDpGetStatus(void) {
    return 0;
}

void osDpSetStatus(UNUSED u32 status) {
}

s32 osAiSetFrequency(u32 frequency) {
    return OotPspAudioBackend_SetFrequency(frequency);
}

u32 osAiGetLength(void) {
    return OotPspAudioBackend_GetLength();
}
