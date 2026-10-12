#include "sched.h"

#include "array_count.h"
#include "oot_psp_performance.h"
#include "oot_psp_renderer.h"

#include <pspkernel.h>
#include <stdlib.h>
#include <string.h>

#define OOT_PSP_VI_RATE_HZ          60U
#define OOT_PSP_FRAME_BASE_USEC     16666U
#define OOT_PSP_FRAME_REMAINDER     40U

/*
 * Graphics pacing state.
 *
 * Uses the PSP's inexpensive 32-bit microsecond timer.
 *
 * sceKernelGetSystemTimeLow() wraps approximately every 71 minutes,
 * but signed timestamp subtraction handles that safely for the small
 * intervals used here.
 */
static u32 sNextGfxCompletionUsec;
static u32 sGfxPacingRemainder;
static s32 sGfxPacingInitialized;


/*
 * Wrap-safe time difference.
 *
 * Positive:
 *     a is after b
 *
 * Negative:
 *     a is before b
 */
static inline s32 SchedPsp_TimeDiff(u32 a, u32 b) {
    return (s32)(a - b);
}


/*
 * Return the N64 VI update rate associated with this task.
 *
 * Normally this is 1:
 *
 *     60 / 1 = 60 Hz
 *
 * updateRate == 2:
 *
 *     60 / 2 = 30 Hz
 */
static inline u32 SchedPsp_GetUpdateRate(const OSScTask* task) {
    if (task != NULL &&
        task->framebuffer != NULL &&
        task->framebuffer->updateRate > 0) {
        return (u32)task->framebuffer->updateRate;
    }

    return 1U;
}


/*
 * Calculate the duration of this VI interval.
 *
 * 1,000,000 / 60 is:
 *
 *     16666 remainder 40
 *
 * Instead of performing a 64-bit division every frame, we use:
 *
 *     16666 us
 *
 * and accumulate the fractional remainder.
 *
 * The resulting sequence for 60 Hz is effectively:
 *
 *     16666
 *     16667
 *     16667
 *     16666
 *     16667
 *     16667
 *     ...
 *
 * which averages exactly 60 Hz over time.
 */
static inline u32 SchedPsp_GetFrameUsec(const OSScTask* task) {
    const u32 updateRate = SchedPsp_GetUpdateRate(task);

    u32 frameUsec =
        OOT_PSP_FRAME_BASE_USEC * updateRate;

    sGfxPacingRemainder +=
        OOT_PSP_FRAME_REMAINDER * updateRate;

    if (sGfxPacingRemainder >= OOT_PSP_VI_RATE_HZ) {
        const u32 extraUsec =
            sGfxPacingRemainder / OOT_PSP_VI_RATE_HZ;

        frameUsec += extraUsec;

        sGfxPacingRemainder -=
            extraUsec * OOT_PSP_VI_RATE_HZ;
    }

    return frameUsec;
}


/*
 * Pace a presented graphics frame.
 *
 * IMPORTANT:
 *
 * There is intentionally NO catch-up behavior here.
 *
 * If rendering finishes early:
 *
 *     sleep until the expected VI deadline
 *
 * If rendering finishes late:
 *
 *     discard the missed deadline immediately
 *     restart pacing from "now"
 *
 * This prevents a slow frame from causing several subsequent frames to
 * run back-to-back at 100% CPU trying to catch up.
 */
/*
 * OOT_PACECARRY=<ms> (debug.cfg pacecarry=<ms>; default 17 since the 2026-10-02 Bench; 0 = off, the old pacer; 1..50).
 * Bench of 23f4901f, 2 runs: Kokiri Forest frames over 55 ms 10 -> 6, 1% low 17.8 -> 18.1 fps; Morpha's room 21 -> 18,
 * 16.3 -> 16.55 fps; render ms unchanged.
 *
 * Without it every frame interval is max(frame period, the frame's own work): a frame that ends
 * late restarts the timeline from its end, so the next frame, however quick, still waits a whole
 * period after it. A scene whose frames average under the period but vary around it (Kokiri
 * Forest: render 46.3 ms, 66 of 402 Bench frames over 55 ms, 19.33 fps) loses every overrun for
 * good. With a carry, a frame late by at most that much keeps the timeline: the next deadline
 * stays on the period's grid, so a quick frame after a slow one repays the overrun (55 then 45 ms
 * instead of 55 then 50), and the average comes back to the period whenever the work allows. A
 * frame later than the carry restarts the timeline as before, so a scene that cannot keep up never
 * runs frames back to back to catch up; the carry bounds how much a deadline can be pulled in.
 * Only when frames are shown changes: every frame's content is the same (the game advances one
 * update a frame either way).
 */
static s32 SchedPsp_CarryUsec(void) {
    static s32 sCarry = -1;

    if (sCarry < 0) {
        const char* e = getenv("OOT_PACECARRY");
        s32 ms = e != NULL ? atoi(e) : 17;

        if (ms < 0) {
            ms = 0;
        }
        if (ms > 50) {
            ms = 50;
        }
        sCarry = ms * 1000;
    }
    return sCarry;
}

static void SchedPsp_PaceGfxTask(OSScTask* task) {
    const u32 now = sceKernelGetSystemTimeLow();
    const u32 frameUsec = SchedPsp_GetFrameUsec(task);

    /*
     * First presented frame establishes our pacing timeline.
     */
    if (!sGfxPacingInitialized) {
        sNextGfxCompletionUsec = now;
        sGfxPacingInitialized = 1;
    }

    {
        const s32 waitUsec =
            SchedPsp_TimeDiff(
                sNextGfxCompletionUsec,
                now);

        if (waitUsec > 0 && waitUsec <= 1000000) {
            /*
             * Frame finished early.
             *
             * Sleep only for the remaining amount of this VI.
             */
            sceKernelDelayThread((u32)waitUsec);
        } else if (waitUsec != 0) {
            /*
             * A "wait" over a second is no frame's remainder (a frame is a
             * few VIs): the 32-bit clock moved more than 2^31 us (35.8 min)
             * since the last frame -- the handheld menu left open that long --
             * and the difference wrapped positive. Slept, the game held its
             * last frame for up to 35 more minutes with the music playing
             * (the bug sweep, 2026-10-01). It is a late frame.
             *
             * Frame missed its deadline.
             *
             * NO CATCH-UP.
             *
             * Throw away all accumulated pacing debt and begin the
             * next interval relative to the current time.
             *
             * OOT_PACECARRY: late by no more than the carry, the timeline is kept.
             */
            if (!(waitUsec < 0 && -waitUsec <= SchedPsp_CarryUsec())) {
                sNextGfxCompletionUsec = now;
            }
        }
    }

    /*
     * Establish the target for the next presented frame.
     *
     * If we were on time, this advances the existing timeline.
     *
     * If we were late, sNextGfxCompletionUsec was reset to 'now',
     * so the next target becomes:
     *
     *     now + frameUsec
     */
    sNextGfxCompletionUsec += frameUsec;
}


/* The deadline of the frame being rendered: gfx_sdl12.c shows it then, not when its render ends
 * (sNextGfxCompletionUsec, set by the last frame's pacing). 0 before the first frame. */
int HhSched_FrameDeadline(uint32_t* usec) {
    if (!sGfxPacingInitialized) {
        return 0;
    }
    *usec = sNextGfxCompletionUsec;
    return 1;
}


void Sched_Notify(Scheduler* sc) {
    OSScTask* task;

    if (sc == NULL) {
        return;
    }

    while (osRecvMesg(
               &sc->cmdQueue,
               (OSMesg*)&task,
               OS_MESG_NOBLOCK) == 0) {

        if (task == NULL) {
            continue;
        }

        if (task->list.t.type == M_NJPEGTASK) {
            /* The JPEG microcode (prerendered room backgrounds) on the CPU. */
            extern void HhJpeg_RunTask(OSTask* task);
            extern void HhPerf_Jpeg(u32 us);
            const u32 jpegStart = sceKernelGetSystemTimeLow();

            HhJpeg_RunTask(&task->list);
            HhPerf_Jpeg(sceKernelGetSystemTimeLow() - jpegStart);
        } else if (task->list.t.type == M_GFXTASK) {

            /*
             * Execute the graphics display list.
             */
            OotPspRenderer_RenderTask(&task->list);

            /*
             * Only a task that actually presents a framebuffer should
             * consume a VI pacing interval.
             *
             * Render-only graphics tasks complete immediately.
             */
            if ((task->flags & OS_SC_SWAPBUFFER) &&
                task->framebuffer != NULL) {

                osViSwapBuffer(
                    task->framebuffer->swapBuffer);

#if defined(OOTDEBUG)

                {
                    const u64 paceStartUsec =
                        OotPspPerformance_Now();

                    SchedPsp_PaceGfxTask(task);

                    OotPspPerformance_RecordPacing(
                        OotPspPerformance_Now() -
                        paceStartUsec);
                }

#else

                {
                    extern const char* volatile gHhLoopStage; /* hh_main.c: the loop note */

                    gHhLoopStage = "pacer sleep";
                    SchedPsp_PaceGfxTask(task);
                    gHhLoopStage = "audio update";
                }

#endif
                {
                    extern void HhPerf_PaceDone(void); /* HITCH lines: the swap + sleep */

                    HhPerf_PaceDone();
                }
            }
        }

        /*
         * Notify whoever submitted the task that it has completed.
         */
        if (task->msgQueue != NULL) {
            osSendMesg(
                task->msgQueue,
                task->msg,
                OS_MESG_NOBLOCK);
        }
    }
}


void Sched_Init(
    Scheduler* sc,
    void* stack,
    OSPri priority,
    u8 viModeType,
    UNK_TYPE arg4,
    IrqMgr* irqMgr
) {
    if (sc == NULL) {
        return;
    }

    memset(sc, 0, sizeof(*sc));

    osCreateMesgQueue(
        &sc->interruptQueue,
        sc->interruptMsgBuf,
        ARRAY_COUNT(sc->interruptMsgBuf));

    osCreateMesgQueue(
        &sc->cmdQueue,
        sc->cmdMsgBuf,
        ARRAY_COUNT(sc->cmdMsgBuf));

    sc->retraceCount = 1;
    sc->isFirstSwap = true;

    /*
     * Start with a fresh graphics pacing timeline.
     */
    sNextGfxCompletionUsec = 0;
    sGfxPacingRemainder = 0;
    sGfxPacingInitialized = 0;

    OotPspRenderer_Init();

    (void)stack;
    (void)priority;
    (void)viModeType;
    (void)arg4;
    (void)irqMgr;
}


void Sched_FlushTaskQueue(void) {
}
