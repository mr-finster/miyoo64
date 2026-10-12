/**
 * The level bench (2026-10-06): the executable named sm64_bench, or SM64_BENCH=2.
 *
 * As GoldenEye's Bench 2: fixed views of the heavy places, each timed, every view twice, in one run on
 * the card with nobody at the controls, then the game exits. Each view is entered from the debug level
 * select (the course's own start, as SM64_LEVEL does; act 1, file A of the test save), Mario left
 * standing. A view is the start camera, or the start camera turned round (four C-left taps), or King
 * Whomp's defeat dust staged at Mario (the SM64_DUSTTEST bursts, every 20 frames). After 120 frames to
 * settle, 90-frame windows are timed: every switch at its default ("all-on"), then each switch of the
 * latest speed round off in turn (2026-10-07, round 2: skyclr, lean, fullst; round 1's fastrows, spanvec, shadepx,
 * zskip, neonnoto and wraprr stay on), then all of them off together ("round-off": the build before the round).
 * Every view once (until 2026-10-06: two runs, one window off per view, the switches taking turns). One line per timed window:
 *   BENCH <level>: <view> (run 1, all-on | <switch>-off | round-off) | frames 90 | work .. ms (max ..) | logic ..
 *   audio .. gfx .. r3d .. r2d .. | tris .. kpx .. | over 33 ms ..
 * "work" is the frame's time without the pacing sleep. The bench never writes the save (hh_eeprom.c),
 * shows no resume prompt and leaves no auto save: the Test build's progress in the test data folder stays.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ultra64.h>
#include "port.h"
#include "savestate.h"
#include "sm64.h"
#include "level_table.h"
#include "game/level_update.h"
#include "game/object_list_processor.h"
#include "game/game_init.h"
#include "game/area.h"

extern void port_log_always(const char *fmt, ...);
extern void port_request_exit(void);
extern int port_bench_switch_count(void);
extern const char *port_bench_switch_name(int i);
extern void port_bench_switch_off(int i); /* -1: all at their defaults */
extern int port_bench_round_count(void);           /* the latest round's switches (2026-10-07: round 2) */
extern const char *port_bench_round_name(int i);
extern void port_bench_round_off(unsigned mask);   /* those in mask off, every other switch at its default */
extern void fade_into_special_warp(u32 arg, u32 color);

typedef struct { s16 level; s8 act; s8 turns; s8 dust; const char *lname, *vname; } BenchView;
static const BenchView sViews[] = {
    { LEVEL_BOB, 1, 0, 0, "Bob-omb Battlefield", "start" },
    { LEVEL_BOB, 1, 4, 0, "Bob-omb Battlefield", "start, camera turned" },
    { LEVEL_WF, 1, 0, 0, "Whomp's Fortress", "start" },
    { LEVEL_WF, 1, 4, 0, "Whomp's Fortress", "start, camera turned" },
    { LEVEL_WF, 1, 0, 1, "Whomp's Fortress", "King Whomp's dust (staged)" },
    { LEVEL_JRB, 1, 0, 0, "Jolly Roger Bay", "start" },
    { LEVEL_JRB, 1, 4, 0, "Jolly Roger Bay", "start, camera turned" },
    { LEVEL_DDD, 1, 0, 0, "Dire Dire Docks", "start" },
    { LEVEL_DDD, 1, 4, 0, "Dire Dire Docks", "start, camera turned" },
    { LEVEL_WDW, 1, 0, 0, "Wet-Dry World", "start" },
    { LEVEL_WDW, 1, 4, 0, "Wet-Dry World", "start, camera turned" },
    { LEVEL_TTM, 1, 0, 0, "Tall Tall Mountain", "start" },
    { LEVEL_TTM, 1, 4, 0, "Tall Tall Mountain", "start, camera turned" },
    { LEVEL_TTC, 1, 0, 0, "Tick Tock Clock", "start" },
    { LEVEL_TTC, 1, 4, 0, "Tick Tock Clock", "start, camera turned" },
    { LEVEL_RR, 1, 0, 0, "Rainbow Ride", "start" },
    { LEVEL_RR, 1, 4, 0, "Rainbow Ride", "start, camera turned" },
    { LEVEL_CASTLE, 1, 0, 0, "castle lobby", "start" },
    { LEVEL_CASTLE, 1, 4, 0, "castle lobby", "start, camera turned" },
    { LEVEL_CASTLE_GROUNDS, 1, 0, 0, "castle grounds", "start" },
    { LEVEL_CASTLE_GROUNDS, 1, 4, 0, "castle grounds", "start, camera turned" },
};
#define NVIEW ((int) (sizeof(sViews) / sizeof(sViews[0])))
#define NRUN 1
static int sSettle SAVESTATE_EXCLUDE = 120, sWindow SAVESTATE_EXCLUDE = 90; /* SM64_BENCH_QUICK=1 (tests): 90 and 30 */
#define SETTLE sSettle
#define WINDOW sWindow

enum { PH_SELECT, PH_LEVEL, PH_LEAVING, PH_DONE };
static int sOn SAVESTATE_EXCLUDE = -1;
static int sView SAVESTATE_EXCLUDE, sRun SAVESTATE_EXCLUDE, sPhase SAVESTATE_EXCLUDE;
static u32 sTick SAVESTATE_EXCLUDE, sSelectAt SAVESTATE_EXCLUDE, sInLevel SAVESTATE_EXCLUDE;
typedef struct { u32 n, over; double work, logic, audio, gfx, r3d, r2d, worst; uint64_t px0, tri0; } Acc;
static Acc sAcc SAVESTATE_EXCLUDE;
static int sOneView SAVESTATE_EXCLUDE;
/* A qemu marker (refs/qprof marks.c): called at each timed window's start and end, so a window's instructions can be
 * profiled on their own. Does nothing. */
void __attribute__((noinline)) port_bench2_mark(int at) { __asm__ volatile("" : : "r"(at) : "memory"); }

int port_bench2_on(void) {
    if (sOn < 0) {
        extern int port_prog_is(const char *prefix);
        const char *e = getenv("SM64_BENCH");
        sOn = e ? e[0] == '2' : port_prog_is("sm64_bench");
        if (sOn) {
            const char *q = getenv("SM64_BENCH_QUICK");
            if (q && q[0] == '1') { sSettle = 90; sWindow = 30; }
            /* SM64_BENCH_VIEW=<i> (tests, qemu): that view only, run 1, then exit; SM64_BENCH_WIN=<n>: the window length */
            const char *vv = getenv("SM64_BENCH_VIEW"), *wn = getenv("SM64_BENCH_WIN");
            if (vv) { sView = atoi(vv); sOneView = 1; if (sView < 0 || sView >= NVIEW) sView = 0; }
            if (wn && atoi(wn) > 0) sWindow = atoi(wn);
            port_log_always("[bench] level bench: %d views x %d run: each from the level select, %d frames to settle, then "
                            "%d-frame windows: all-on, each of the round's %d switches off, the round off; then exit\n",
                            NVIEW, NRUN, SETTLE, WINDOW, port_bench_round_count());
        }
    }
    return sOn;
}

/* The level the level select picks (controller_sdl.c port_debug_level). */
int port_bench2_level(void) { return sViews[sView < NVIEW ? sView : 0].level; }

/* title_screen.c intro_level_select, every frame it runs: file A of the test save, the view's act. */
void port_bench2_in_select(void) {
    extern s16 gCurrSaveFileNum, gCurrActNum;
    sSelectAt = sTick;
    gCurrSaveFileNum = 1;
    gCurrActNum = sViews[sView < NVIEW ? sView : 0].act;
}

/* object_list_processor.c: King Whomp's dust this frame (the SM64_DUSTTEST bursts). */
int port_bench2_dust_now(void) {
    return sOn > 0 && sPhase == PH_LEVEL && sView < NVIEW && sViews[sView].dust && sInLevel >= 30 && sInLevel % 20 == 10;
}

/* controller_sdl.c read_pad: the bench's buttons this frame. */
u16 port_bench2_pad(void) {
    if (sOn <= 0) return 0;
    if (sPhase == PH_SELECT && sTick - sSelectAt < 3 && sTick % 30 < 3) return START_BUTTON;
    /* past the logo, the Mario head and any demo to the level select: START 4 frames of every 60 while there
     * is no Mario (the SM64_AUTO autopilot's taps) */
    if (sPhase == PH_SELECT && gMarioObject == NULL && sTick % 60 < 4) return START_BUTTON;
    if (sPhase == PH_LEVEL && sView < NVIEW) {
        const int turns = sViews[sView].turns;
        for (int t = 0; t < turns; t++) {
            const u32 at = 30 + 12 * (u32) t; /* a tap: two frames down, ten up */
            if (sInLevel >= at && sInLevel < at + 2) return L_CBUTTONS;
        }
    }
    return 0;
}

static void acc_line(const char *variant) {
    extern uint64_t gfx_soft_span_px, gfx_soft_tris;
    const BenchView *v = &sViews[sView];
    const double n = sAcc.n ? sAcc.n : 1;
    char tick[24] = ""; /* quick mode (tests): the iteration it ended at, to find the view's frame dumps */
    if (sWindow != 120) snprintf(tick, sizeof(tick), " | at %u", sTick);
    port_log_always("BENCH %s: %s (run %d, %s) | frames %u | work %.2f ms (max %.1f) | logic %.2f audio %.2f gfx %.2f "
                    "r3d %.2f r2d %.2f | tris %.0f kpx %.1f | over 33 ms %u%s\n",
                    v->lname, v->vname, sRun + 1, variant, sAcc.n, sAcc.work / n, sAcc.worst, sAcc.logic / n,
                    sAcc.audio / n, sAcc.gfx / n, sAcc.r3d / n, sAcc.r2d / n,
                    (double) (gfx_soft_tris - sAcc.tri0) / n, (double) (gfx_soft_span_px - sAcc.px0) / n / 1000.0, sAcc.over, tick);
}
static void acc_start(void) {
    extern uint64_t gfx_soft_span_px, gfx_soft_tris;
    memset(&sAcc, 0, sizeof(sAcc));
    sAcc.px0 = gfx_soft_span_px; sAcc.tri0 = gfx_soft_tris;
}

/* hh_main.c prof_iteration_end, after every iteration: the times in microseconds. */
void port_bench2_frame(u32 work_us, u32 logic_us, u32 audio_us, u32 gfx_us, u32 r3d_us, u32 r2d_us) {
    if (port_bench2_on() <= 0 || sPhase == PH_DONE) return;
    sTick++;
    const int in_select = sTick - sSelectAt < 3;
    switch (sPhase) {
        case PH_SELECT:
            if (!in_select && gMarioObject != NULL && gCurrDemoInput == NULL && gCurrLevelNum == sViews[sView].level
                && sSelectAt != 0) { /* entered from the level select, not a title demo */
                sPhase = PH_LEVEL;
                sInLevel = 0;
            }
            break;
        case PH_LEVEL: {
            if (in_select || gCurrDemoInput != NULL) { sPhase = PH_SELECT; port_bench_switch_off(-1); break; } /* not ours: again */
            sInLevel++;
            if (sInLevel == SETTLE) { port_bench_switch_off(-1); acc_start(); port_bench2_mark(0); }
            if (sInLevel > SETTLE) {
                sAcc.n++;
                sAcc.work += work_us / 1000.0; sAcc.logic += logic_us / 1000.0; sAcc.audio += audio_us / 1000.0;
                sAcc.gfx += gfx_us / 1000.0; sAcc.r3d += r3d_us / 1000.0; sAcc.r2d += r2d_us / 1000.0;
                if (work_us / 1000.0 > sAcc.worst) sAcc.worst = work_us / 1000.0;
                if (work_us > 33333) sAcc.over++;
            }
            /* the windows: 0 all-on, 1..m each switch of this round off in turn, m + 1 every one of them off ("round-off") */
            if (sInLevel > SETTLE && (sInLevel - SETTLE) % WINDOW == 0) {
                const int w = (sInLevel - SETTLE) / WINDOW, m = port_bench_round_count(), last = sOneView ? 1 : m + 2;
                port_bench2_mark(w);
                char name[48];
                snprintf(name, sizeof(name), "%s", w == 1 ? "all-on" : w <= m + 1 ? port_bench_round_name(w - 2) : "round-off");
                acc_line(name);
                if (w >= last) {
                    port_bench_switch_off(-1);
                    if (sOneView) { sPhase = PH_DONE; port_log_always("[bench] done\n"); port_request_exit(); break; }
                    fade_into_special_warp(-9, 1); /* back to the level select (the debug select's own exit) */
                    sPhase = PH_LEAVING;
                    break;
                }
                if (w <= m) port_bench_round_off(1u << (w - 1)); /* the next window: switch w - 1 off */
                else port_bench_round_off(~0u);                   /* the last: all of them off */
                acc_start();
            }
            break;
        }
        case PH_LEAVING:
            if (in_select) {
                if (sOneView) sRun = NRUN;
                else if (++sView >= NVIEW) { sView = 0; sRun++; }
                if (sRun >= NRUN) {
                    sPhase = PH_DONE;
                    port_log_always("[bench] done\n");
                    port_request_exit();
                } else {
                    sPhase = PH_SELECT;
                }
            }
            break;
    }
}
