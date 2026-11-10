#include "sched.h"

#include "array_count.h"
#include "oot_psp_performance.h"
#include "oot_psp_renderer.h"

#include <pspkernel.h>
#include "hh_interp.h"
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

/*
 * The pacing wait. Any sleep on the Miyoo Mini costs a whole kernel tick,
 * 9.2-9.6 ms however short the request (measured on the Mini Plus), so a
 * frame's remainder under ~10 ms slept was up to a frame late. Only the part
 * of a wait above 12 ms is slept; the rest is a sched_yield loop on the
 * microsecond clock, to the same deadline. On a kernel whose sleeps are
 * exact the timing is the same. OOT_SPINWAIT=0: one sleep, as before.
 */
extern int sched_yield(void); /* not <sched.h>: the game's sched.h is on the include path */
static void SchedPsp_Wait(u32 waitUsec) {
    static s32 sSpin = -1;
    const u32 start = sceKernelGetSystemTimeLow();
    u32 done;

    if (sSpin < 0) {
        const char* e = getenv("OOT_SPINWAIT");

        sSpin = !(e != NULL && e[0] == '0');
    }
    if (!sSpin) {
        sceKernelDelayThread(waitUsec);
        return;
    }
    while ((done = sceKernelGetSystemTimeLow() - start) < waitUsec) {
        if (waitUsec - done > 12000U) {
            sceKernelDelayThread(waitUsec - done - 12000U);
        } else {
            sched_yield();
        }
    }
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
            SchedPsp_Wait((u32)waitUsec);
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


/*
 * OOT_FPS30 (debug.cfg fps30=1, MENU > OPTIONS > 30 FPS; off by default): the game's 20 updates a second shown at
 * 30 frames a second, the in-between frames drawn from the frame's own display list with the matrices blended
 * between the last update and this one (hh_interp.c records them in Play_Draw; gfx_pc.c swaps them in). The game's
 * speed is unchanged. Two updates make three pictures, 2 vsyncs each:
 *   tick A: the point 1/3 of the way from the last update to this one, then this update itself;
 *   tick B: the point 2/3 of the way from the last update to this one (this update itself is not shown).
 * So the screen shows u - 2/3, u, u + 2/3, u + 4/3, u + 2, ...: evenly spaced, two vsyncs apart. Every picture is held
 * by vsyncs on the present thread (gfx_sdl12.c), which also paces the game: 3 vsyncs an update at the panel's
 * real rate (59.7 Hz on the Mini Plus) instead of the 60 Hz clock below. The game thread is at most two pictures
 * ahead of the screen.
 * When the pictures don't fit (the average draw over 30 ms), both ticks show only the update (20 fps, 3 vsyncs
 * each) until it fits again (under 27 ms, or the last one under 24). Outside the game (title, file select) and in
 * the pause menu (30 updates a second already) every frame is the game's own, held its VI count.
 * Written for the Miyoo Mini port (cacuracaptors/oot-miyoomini, MIT) after Ship of Harkinian's approach.
 */
extern int port_fi_enabled; /* hh_interp.c */
extern void gfx_sdl12_next_hold(int vsyncs);
extern int gfx_sdl12_hold_ok(void);
static FiRepl* sFiRepl;
static int sFiTickA = 1, sFiFall;
static float sFiDrawMs = 15.0f, sFiLastMs = 15.0f;

static void SchedPsp_FiDraw(OSScTask* task, const FiRepl* repl, int hold) {
    extern uint32_t gfx_sdl12_present_wait_us(void);
    const u32 t0 = sceKernelGetSystemTimeLow();
    float ms;

    gHhFiRepl = repl;
    gfx_sdl12_next_hold(hold);
    OotPspRenderer_RenderTask(&task->list);
    gHhFiRepl = NULL;
    /* the draw's own time: the wait for the screen to take the last picture left out */
    ms = ((s32)(sceKernelGetSystemTimeLow() - t0) - (s32)gfx_sdl12_present_wait_us()) / 1000.0f;
    if (ms < 500.0f) {
        sFiLastMs = ms;
        sFiDrawMs = sFiDrawMs * 0.75f + ms * 0.25f;
    }
}

/* 1: the task was drawn, shown and paced here; 0: the usual way */
static int SchedPsp_FiTask(OSScTask* task) {
    int cs, ps;

    static int sEnvRead;

    if (!sEnvRead) {
        const char* e = getenv("OOT_FPS30");

        sEnvRead = 1;
        port_fi_enabled = e != NULL && e[0] == '1';
    }
    if (!port_fi_enabled || !gfx_sdl12_hold_ok() || !(task->flags & OS_SC_SWAPBUFFER) || task->framebuffer == NULL) {
        gfx_sdl12_next_hold(0);
        return 0;
    }
    if (sFiRepl == NULL) {
        sFiRepl = (FiRepl*)calloc(1, sizeof(FiRepl));
        if (sFiRepl == NULL) {
            port_fi_enabled = 0;
            return 0;
        }
    }
    if (!port_fi_pair(task->list.t.data_ptr, &cs, &ps)) {
        /* not a recorded game frame: the frame itself, its VI count */
        sFiTickA = 1;
        SchedPsp_FiDraw(task, NULL, (int)SchedPsp_GetUpdateRate(task));
    } else {
        if (sFiTickA) {
            /* the decision holds for the pair A + B */
            if (!sFiFall && sFiDrawMs > 30.0f) {
                sFiFall = 1;
            } else if (sFiFall && (sFiDrawMs < 27.0f || sFiLastMs < 24.0f)) {
                sFiFall = 0;
            }
        }
        if (sFiFall) {
            SchedPsp_FiDraw(task, NULL, 3);
        } else if (sFiTickA) {
            port_fi_build(cs, ps, 1.0f / 3.0f, sFiRepl);
            SchedPsp_FiDraw(task, sFiRepl, 2);
            SchedPsp_FiDraw(task, NULL, 2);
        } else {
            port_fi_build(cs, ps, 2.0f / 3.0f, sFiRepl);
            SchedPsp_FiDraw(task, sFiRepl, 2);
        }
        sFiTickA ^= 1;
    }
    osViSwapBuffer(task->framebuffer->swapBuffer);
    {
        extern void HhPerf_PaceDone(void); /* HITCH lines: the swap */

        HhPerf_PaceDone();
    }
    /* no clock pacing: the present thread's vsync holds pace the game. A later return to the usual way starts a
     * fresh 60 Hz timeline. */
    sGfxPacingInitialized = 0;
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
        } else if (task->list.t.type == M_GFXTASK && SchedPsp_FiTask(task)) {
            /* OOT_FPS30: drawn, shown and paced above */
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
