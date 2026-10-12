/* pace_test.c: the real frame pacer (src/port/hh/hh_sched.c, SchedPsp_PaceGfxTask) on a simulated clock (2026-10-01,
 * the FPS hunt). Each frame's work (game update + render) is drawn from a distribution; the pacer sleeps on the
 * simulated clock; the frame interval is measured where the Bench measures it (the end of the render, hh_renderer.c).
 * Prints the mean fps and the share of intervals over 55 ms, so OOT_PACECARRY's effect on a scene's frame-time
 * spread can be read before the device Bench, and checks the pacer's invariants:
 *   - OOT_PACECARRY=0 (default): frame start to frame start is max(period, work) exactly (the old pacer: no
 *     catch-up);
 *   - any carry: start to start is never shorter than the frame's own work (no frame is cut short or run back to
 *     back to catch up), and no sleep is longer than one period.
 * (The Bench's interval, render end to render end, is period + work_i - work_i-1 on time: its >55 ms count includes
 * that jitter.)
 *
 *   pace_test <mean ms> <sd ms> <tail %> <tail extra ms> [frames] [seed]
 * Build: tools/handheld/pace_test.sh (host gcc; the pacer is integer code). */
#include "hh_sched.c"

#include <stdio.h>
#include <math.h>

static u32 sClock;      /* the simulated sceKernelGetSystemTimeLow, us */
static u32 sLastSleep;
SceUInt32 sceKernelGetSystemTimeLow(void) { return sClock; }
int sceKernelDelayThread(SceUInt delay) { sClock += delay; sLastSleep = delay; return 0; }

/* the rest of hh_sched.c's references (Sched_Notify is not run) */
void OotPspRenderer_RenderTask(const OSTask* t) { (void)t; }
void OotPspRenderer_Init(void) {}
void HhJpeg_RunTask(OSTask* t) { (void)t; }
void HhPerf_Jpeg(u32 us) { (void)us; }
void HhPerf_PaceDone(void) {}
const char* volatile gHhLoopStage;
s32 osRecvMesg(OSMesgQueue* mq, OSMesg* m, s32 f) { (void)mq; (void)m; (void)f; return -1; }
s32 osSendMesg(OSMesgQueue* mq, OSMesg m, s32 f) { (void)mq; (void)m; (void)f; return 0; }
void osCreateMesgQueue(OSMesgQueue* mq, OSMesg* b, s32 n) { (void)mq; (void)b; (void)n; }
void osViSwapBuffer(void* fb) { (void)fb; }

static uint32_t rs = 1;
static double urand(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return (rs + 0.5) / 4294967296.0; }
static double nrand(void) { return sqrt(-2.0 * log(urand())) * cos(6.283185307179586 * urand()); }

int main(int argc, char** argv) {
    const double mean = argc > 1 ? atof(argv[1]) : 48.0, sd = argc > 2 ? atof(argv[2]) : 3.0;
    const double tail = argc > 3 ? atof(argv[3]) / 100.0 : 0.1, extra = argc > 4 ? atof(argv[4]) : 10.0;
    const int frames = argc > 5 ? atoi(argv[5]) : 4000;
    rs = argc > 6 ? (uint32_t)atoi(argv[6]) : 1;
    const s32 carry = SchedPsp_CarryUsec();
    static CfbInfo fb;
    static OSScTask task;
    fb.updateRate = 3; /* OoT's 20 fps */
    task.framebuffer = &fb;
    sClock = 1000000;
    u32 lastEnd = 0, lastStart = 0, lastWork = 0;
    double sum = 0;
    int n = 0, over55 = 0, bad = 0;
    for (int f = 0; f < frames; f++) {
        double w = mean + sd * nrand();
        if (urand() < tail) w += extra * urand() * 2.0;
        if (w < 5) w = 5;
        const u32 work = (u32)(w * 1000.0);
        const u32 start = sClock;
        sClock += work;                 /* logic + render */
        const u32 end = sClock;         /* HhPerf_Frame: the interval is measured here */
        if (lastEnd != 0) {
            const u32 iv = end - lastEnd;
            sum += iv; n++;
            over55 += iv > 55000;
        }
        if (lastStart != 0) {
            const u32 ss = start - lastStart; /* start to start: never shorter than the previous frame's work */
            if (ss < lastWork) bad++;
            if (carry == 0 && f > 2) {  /* the old pacer: exactly max(period, work) (+-1 us: the 16666/16667 VI steps) */
                const u32 want = lastWork > 50000 ? lastWork : 50000;
                if (ss + 1 < want || ss > want + 1) bad++;
            }
        }
        lastStart = start;
        lastWork = work;
        lastEnd = end;
        sLastSleep = 0;
        SchedPsp_PaceGfxTask(&task);
        if (sLastSleep > 50001) bad++;  /* no sleep longer than one period */
    }
    printf("carry %d ms: mean work %.1f ms (sd %.1f, %.0f%% +%.0f ms): fps %.2f, >55 ms %.1f%%, invariant failures %d\n",
           (int)(carry / 1000), mean, sd, tail * 100, extra, 1e6 * n / sum, 100.0 * over55 / n, bad);
    return bad != 0;
}
