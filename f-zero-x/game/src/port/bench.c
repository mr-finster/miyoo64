/**
 * The on-device benchmark: device A/B tests with nothing to press.
 *
 * Launched as `fzerox_bench` (the same binary under another name) or with FZX_BENCH=1, the game drives itself (the autopilot) into the Mute City GP
 * race and snapshots its memory at BENCH_START (save states, kept in RAM). Then, for each
 * run, it restores the snapshot and races the same 10 s, so every run draws exactly the
 * same race. Every run is 100% SMOOTH, the game's only 3D setting (round 9); a variant
 * changes one setting from the build's own, and the variants run twice, interleaved, so the spread
 * between a variant's two runs shows the noise. The first 2 s of each run are warm-up
 * (textures re-import after the restore, the pacer settles). One BENCH line per run goes
 * to the log, the handheld notification bar shows the run, and the game quits after the
 * last one. FZX_BENCH_DUMP=1 also saves one frame per run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libultra/ultra64.h"
#include "port_os.h"
#include "port_platform.h"
#include "gfx/port.h"
#include "gfx/savestate.h"
#include "gfx/handheld_menu.h"

extern int gfx_soft_bilinear;
extern int gfx_soft_tap3;   /* gfx_soft.c: the N64's three-point filter */
extern int gfx_soft_rcprow; /* gfx_soft.c: tri_row's reciprocals */
extern int gfx_soft_swar;   /* gfx_soft.c: the packed red/blue opaque blends */
extern int gfx_soft_defer;  /* gfx_soft.c: deferred background batches */
extern int gfx_soft_vis;    /* gfx_soft.c: visible-only 3D */
extern int gfx_soft_visneon; /* gfx_soft.c: the visible-only scans eight and sixteen pixels a compare */
extern int gfx_soft_compneon; /* gfx_soft.c: the composite over 2D in NEON */
extern int gfx_soft_hudswar; /* gfx_soft.c: the HUD box loop with div255 and packed red/blue */
extern int gfx_soft_fastrows; /* gfx_soft.c: FZX_FASTROWS, a hot triangle's rows in tri_rows (device build) */
extern int gfx_soft_spanvec;  /* gfx_soft.c: FZX_SPANVEC, hot_row's float state as NEON vectors (device build) */
extern int gfx_soft_walllod;  /* gfx_soft.c: FZX_WALLLOD, the walls sampled at the N64's pixel centres */
extern int gfx_fill100;       /* gfx_pc.c: FZX_FILL100, the 3D at 100% fills the screen */
int port_clock_fast(int on); /* port_main.c: the generic-timer clock */
void port_pacer_set_uneven(int on); /* port_main.c: the 45 and 40 fps cadences */
void port_pacer_set_fine(int on); /* port_main.c: the 52.5, 50 and 48 fps cadences */
void gfx_wm_set_present_mode(int mode); /* gfx_sdl12.c: 0 staging copy, 1 direct, 2 direct + prefetch */
void gfx_wm_set_screen_direct(int on); /* gfx_sdl12.c: the composite into the screen */
void gfx_soft_set_neon(int on); /* gfx_soft.c: the NEON paths (device build) */
void gfx_soft_set_fuse(int on); /* gfx_soft.c: the fused bilinear + combine (device build) */
void gfx_soft_set_pair(int on); /* gfx_soft.c: the fused opaque combine two pixels at a time (device build) */
void gfx_soft_set_quad(int on); /* gfx_soft.c: the combines four pixels at a time (device build) */
extern void port_request_exit(void);
extern const char* port_data_path(const char* name);
void port_perf_restart(void); /* port_main.c */
void port_pacer_reset(void);  /* port_main.c */
void port_sprof_start(void);                              /* sdl/sprof.c */
void port_sprof_stop(const char* path, const char* tag); /* sdl/sprof.c */

#define BENCH_START 1100 /* VI: the autopilot's race is running */
#define BENCH_LEN 600    /* VIs per setting: 10 s */
#define BENCH_WARM 120   /* VIs of warm-up after each restore, not counted */
#define BENCH_DUMP_AT 300

static const struct {
    const char* name;
    int vis, quad, neon, pres, uneven, res, tap3, bilin, rcprow, scr, swar, hudswar, visneon, compneon, fastrows, spanvec, walllod, fill100, prof;
} kVar[] = {
    /* the build as it ships, then the same with the device profiler sampling (sdl/sprof.c; its own
     * ms read ~13% high): <data>/bench_prof.bin, read with tools/sprof.py / tools/sprof_lines.sh and
     * the build's unstripped binary. (Round 16's "no bil3n": the 2-pixel NEON 3-tap lost 0.3-0.4 ms
     * on the device and was removed.) The 2026-10-07 speed round's windows (FASTROWS, SPANVEC) were read on the
     * device and are gone; 2026-10-09: the wall LOD (FZX_WALLLOD, on by default) against the same without it, and the
     * screen filled at 100% (FZX_FILL100, on by default: the box's full square drawn instead of 222x208) against the
     * bars. The 3D at 100% (the default since 2026-10-03). */
    { "ship", 1, 0, 0, 2, 2, 100, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0 },       /* the defaults */
    { "wall LOD off", 1, 0, 0, 2, 2, 100, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0 },
    { "fill off", 1, 0, 0, 2, 2, 100, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0 },
    { "ship, prof", 1, 0, 0, 2, 2, 100, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
};
#define NVAR ((int) (sizeof(kVar) / sizeof(kVar[0])))
#define NCFG (NVAR * 2) /* runs: every variant twice, interleaved */

static int sActive;
static int sDump;
static int sCfg = -1;       /* run in progress; -1 = still driving to BENCH_START */
static u32 sStartVi;        /* the retrace it started at (BENCH_START after a restore) */
static int sMeasuring;
static struct {
    u32 secs, pics, skips, slips, lateMax, underruns;
    double gfx, r3d, r2d, tex, comp, pres, pwait, clr, logic, audio; /* us, summed over the seconds */
    double px;                                /* pixels per picture, summed over the seconds */
} sAcc;
static char sLines[NCFG][260];

int port_bench_active(void) {
    return sActive;
}

void port_bench_parse(int argc, char** argv) {
    const char* e = getenv("FZX_BENCH");
    const char* name = argc > 0 && argv[0] ? strrchr(argv[0], '/') : NULL;
    name = name ? name + 1 : (argc > 0 && argv[0] ? argv[0] : "");
    sActive = (e && e[0] == '1') || strcmp(name, "fzerox_bench") == 0;
    e = getenv("FZX_BENCH_DUMP");
    sDump = e && e[0] == '1';
    if (sActive) {
        port_log("BENCH: %d runs x %d s (%d variants twice, 100%% SMOOTH), from vi %d\n", NCFG, BENCH_LEN / 60, NVAR,
                 BENCH_START);
    }
}

static void start_cfg(int k) {
    char msg[40];
    const int v = k % NVAR;
    int clock;
    sCfg = k;
    sStartVi = port_os_vi_count();
    sMeasuring = 0;
    memset(&sAcc, 0, sizeof(sAcc));
    port_res3d_set(kVar[v].res);
    gfx_soft_bilinear = kVar[v].bilin;
    gfx_soft_tap3 = kVar[v].tap3;
    gfx_soft_rcprow = kVar[v].rcprow;
    gfx_wm_set_screen_direct(kVar[v].scr);
    gfx_soft_swar = kVar[v].swar;
    gfx_soft_hudswar = kVar[v].hudswar;
    gfx_soft_visneon = kVar[v].visneon;
    gfx_soft_compneon = kVar[v].compneon;
    gfx_soft_fastrows = kVar[v].fastrows;
    gfx_soft_spanvec = kVar[v].spanvec;
    gfx_soft_walllod = kVar[v].walllod;
    gfx_fill100 = kVar[v].fill100;
    gfx_soft_vis = kVar[v].vis;
    gfx_soft_defer = 1;
    gfx_soft_set_quad(kVar[v].quad);
    gfx_soft_set_neon(kVar[v].neon);
    gfx_soft_set_fuse(1);
    gfx_soft_set_pair(1);
    gfx_wm_set_present_mode(kVar[v].pres);
    port_pacer_set_uneven(kVar[v].uneven > 0);
    port_pacer_set_fine(kVar[v].uneven > 1);
    clock = port_clock_fast(1);
    port_pacer_reset();
    snprintf(msg, sizeof(msg), "BENCH %d/%d: %s", k + 1, NCFG, kVar[v].name);
    handheld_menu_notify(msg);
    port_log("BENCH start %s (vis %d, quad %d, neon %d, present %d, uneven %d, res %d, tap3 %d, bilin %d, rcp %d, scr %d, swar %d, hudswar %d, visneon %d, compneon %d, fastrows %d, spanvec %d, walllod %d, fill100 %d, clock %s) at vi %u\n",
             kVar[v].name, kVar[v].vis, kVar[v].quad, kVar[v].neon, kVar[v].pres, kVar[v].uneven, kVar[v].res, kVar[v].tap3,
             kVar[v].bilin, kVar[v].rcprow, kVar[v].scr, kVar[v].swar, kVar[v].hudswar, kVar[v].visneon, kVar[v].compneon, kVar[v].fastrows, kVar[v].spanvec, kVar[v].walllod, kVar[v].fill100, clock ? "user space" : "syscall", (unsigned) sStartVi);
}

static void report(int k) {
    const double sec = sAcc.secs ? (double) sAcc.secs : 1.0;
    const double p = sAcc.pics ? (double) sAcc.pics : 1.0;
    /* other: the display list, vertices and triangle setup outside the raster buckets */
    const double other = sAcc.gfx - sAcc.r3d - sAcc.r2d - sAcc.tex - sAcc.comp - sAcc.pres;
    snprintf(sLines[k], sizeof(sLines[k]),
             "BENCH %-13s %5.1f fps | per picture gfx %5.1f ms (3d %5.1f 2d %4.1f comp %4.1f pres %4.1f wait %4.2f other %4.1f clr %4.2f) | "
             "per tick logic %.1f audio %.2f ms | late max %.0f ms | slips %u underruns %u | px %.0fk",
             kVar[k % NVAR].name, sAcc.pics / sec, sAcc.gfx / p / 1000.0, sAcc.r3d / p / 1000.0, sAcc.r2d / p / 1000.0,
             sAcc.comp / p / 1000.0, sAcc.pres / p / 1000.0, sAcc.pwait / p / 1000.0, other / p / 1000.0, sAcc.clr / p / 1000.0,
             sAcc.logic / sec / 60.0 / 1000.0, sAcc.audio / sec / 60.0 / 1000.0, sAcc.lateMax / 1000.0,
             (unsigned) sAcc.slips, (unsigned) sAcc.underruns, sAcc.px / sec / 1000.0);
    port_log("%s\n", sLines[k]);
}

/* Between retraces, after savestate_check (port_main.c). */
void port_bench_check(void) {
    u32 vi;
    if (!sActive) {
        return;
    }
    vi = port_os_vi_count();
    if (sCfg < 0) {
        if (vi >= BENCH_START) {
            if (!savestate_mem_snapshot()) {
                port_log("BENCH: no memory for the snapshot; stopping\n");
                sActive = 0;
                port_request_exit();
                return;
            }
            start_cfg(0);
        }
        return;
    }
    if (!sMeasuring && vi >= sStartVi + BENCH_WARM) {
        sMeasuring = 1;
        port_perf_restart(); /* the PERF seconds line up with the measured window */
        if (kVar[sCfg % NVAR].prof) port_sprof_start();
    }
    if (sDump && vi == sStartVi + BENCH_DUMP_AT) {
        char name[64];
        snprintf(name, sizeof(name), "bench_%d.ppm", sCfg);
        port_platform_dump(port_data_path(name));
    }
    if (vi >= sStartVi + BENCH_LEN) {
        int i;
        if (kVar[sCfg % NVAR].prof) port_sprof_stop(port_data_path("bench_prof.bin"), kVar[sCfg % NVAR].name);
        report(sCfg);
        if (sCfg + 1 < NCFG && savestate_mem_restore()) {
            start_cfg(sCfg + 1);
            return;
        }
        port_log("BENCH done: the same %d s of the Mute City GP race per run\n", (BENCH_LEN - BENCH_WARM) / 60);
        gfx_soft_vis = 1;
        gfx_soft_defer = 1;
        port_res3d_set(100);
        gfx_soft_bilinear = 1;
        gfx_soft_tap3 = 1;
        gfx_soft_rcprow = 0;
        gfx_wm_set_screen_direct(0);
        gfx_soft_swar = 1;
        gfx_soft_hudswar = 1;
        gfx_soft_visneon = 1;
        gfx_soft_compneon = 1;
        gfx_soft_fastrows = 1;
        gfx_soft_spanvec = 1;
        gfx_soft_walllod = -1; /* the player's setting again (FZX_WALLLOD) */
        gfx_fill100 = -1;      /* and FZX_FILL100 */
        gfx_soft_set_quad(0);
        gfx_soft_set_neon(0);
        gfx_soft_set_fuse(1);
        gfx_soft_set_pair(1);
        gfx_wm_set_present_mode(2);
        port_pacer_set_uneven(1);
        port_clock_fast(1);
        for (i = 0; i <= sCfg; i++) {
            port_log("%s\n", sLines[i]);
        }
        handheld_menu_notify("BENCH DONE");
        sActive = 0;
        port_request_exit();
    }
}

/* perf_vi (port_main.c), once a second: that second's totals. */
void port_bench_note_second(u32 pics, u32 skips, u32 slips, u32 lateMaxUs, u32 underruns, const u32* sum, u32 pxPerPic) {
    if (!sActive || sCfg < 0 || !sMeasuring) {
        return;
    }
    sAcc.secs++;
    sAcc.pics += pics;
    sAcc.skips += skips;
    sAcc.slips += slips;
    sAcc.underruns += underruns;
    if (lateMaxUs > sAcc.lateMax) {
        sAcc.lateMax = lateMaxUs;
    }
    sAcc.gfx += sum[PROF_GFX];
    sAcc.r3d += sum[PROF_R3D];
    sAcc.r2d += sum[PROF_R2D];
    sAcc.tex += sum[PROF_TEX];
    sAcc.comp += sum[PROF_COMP];
    sAcc.pres += sum[PROF_PRES];
    sAcc.pwait += sum[PROF_PWAIT];
    sAcc.clr += sum[PROF_CLR];
    sAcc.logic += sum[PROF_LOGIC];
    sAcc.audio += sum[PROF_AUDIO];
    sAcc.px += pxPerPic;
}
