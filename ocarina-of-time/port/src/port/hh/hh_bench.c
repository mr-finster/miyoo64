/*
 * The on-device bench (round 50; GoldenEye's hh_bench.c, itself F-Zero X's): the frame rate of a
 * fixed set of views, with nothing to press. The lead's release target: 1% low >= 18 fps at the
 * 20 fps cap outside loads, and no periodic HITCH beat.
 *
 * Launched as oot_bench (the same binary under another name) or with
 * OOT_BENCH=1, the process runs the game once per view as a child: a debug boot straight into the
 * view (the debug save, never the player's file), Link walked by an input script, paced as in play
 * (20 fps, sound on). Each view runs twice, interleaved, so the spread between its two runs shows
 * the noise. A child times the frames from OOT_BENCH_FROM to its exit and appends one BENCH line
 * to bench.log next to console.log (HhPerf_BenchReport), then shuts SDL down (gfx_wm_shutdown:
 * GoldenEye's second run hung at its first flip without it) and exits. Saves and the child's own
 * console.log go to /tmp/oot_bench, wiped before each run. A run still going after 180 s is
 * stopped by the parent.
 *
 * Quitting stops the whole bench (the user, 2026-09-28: "it starts a new scene when i try"). EXIT GAME
 * ends a child with status 3 (hh_main.c): the parent logs "stopped by the user" and exits to the
 * menu. A SIGTERM (the OS's power-off): the parent logs it, stops the child and exits, so the
 * power-off goes ahead with no resume of a debug boot.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

const char* port_save_dir(void);

#define HH_BENCH_QUIT 3 /* a child's exit status when the player quit (hh_main.c) */
static volatile sig_atomic_t sBenchSignal; /* a terminate, for the parent */
static void bench_signal(int sig) {
    sBenchSignal = sig;
}

typedef struct {
    const char* name;
    const char* env; /* space-separated NAME=value pairs */
} BenchView;
static const BenchView kView[] = {
/* 2026-10-02 (the lead): round 1's settled views (each measured on two device Benches, 23f4901f and 7743e06c; numbers
 * in BUILDLOG "device Bench of 23f4901f") are dropped to keep the Bench near half an hour: grain OFF, fastwater, lean
 * rows OFF, half-res sky, N64 shade, opaque lean OFF, frame-side OFF (x2), integer clip flags, pace carry OFF (x2),
 * raster speedups OFF (x4), sky deferred OFF. Their switches still work from debug.cfg. */
    { "Kokiri Forest", "OOT_ENTRANCE=0xEE" },
    { "Hyrule Field day", "OOT_ENTRANCE=0xCD" },
    { "Lake Hylia", "OOT_ENTRANCE=0x102" },
    { "Deku Tree basement water", "OOT_ENTRANCE=0x0 OOT_WARPROOM=3,-165.9,-905,-132.2,0xD8B8,60" },
    { "Death Mountain Trail", "OOT_ENTRANCE=0x13D" },
    { "Morpha's room", "OOT_ENTRANCE=0x417 OOT_ADULT=1" },
    /* 2026-10-02, FPS round 2 (rasterizer): its exact switches (the z-tested (A - K) * TEXEL0 + K rows in NEON, the cycle-1
     * texel evaluator's operand sources) off, against each scene's default view (same pixels: render ms is the
     * measure); and the next-row prefetch (OOT_PLD, exact, default off until these views show it faster) */
#define HH_RASTER2_OFF "OOT_LERPLEAN=0 OOT_TPEFAST=0"
    { "Deku Tree basement water, round-2 raster OFF", "OOT_ENTRANCE=0x0 OOT_WARPROOM=3,-165.9,-905,-132.2,0xD8B8,60 " HH_RASTER2_OFF },
    { "Kokiri Forest, round-2 raster OFF", "OOT_ENTRANCE=0xEE " HH_RASTER2_OFF },
    { "Morpha's room, round-2 raster OFF", "OOT_ENTRANCE=0x417 OOT_ADULT=1 " HH_RASTER2_OFF },
    { "Deku Tree basement water, prefetch ON", "OOT_ENTRANCE=0x0 OOT_WARPROOM=3,-165.9,-905,-132.2,0xD8B8,60 OOT_PLD=1" },
    { "Kokiri Forest, prefetch ON", "OOT_ENTRANCE=0xEE OOT_PLD=1" },
#undef HH_RASTER2_OFF
    /* 2026-10-02: the edge fixes priced (OOT_EDGETINT's end-of-3D filter, OOT_CRACKFILL's fill), against the default views */
    { "Kokiri Forest, edge tint + crack fill OFF", "OOT_ENTRANCE=0xEE OOT_EDGETINT=0 OOT_CRACKFILL=0" },
    { "Hyrule Field day, edge tint + crack fill OFF", "OOT_ENTRANCE=0xCD OOT_EDGETINT=0 OOT_CRACKFILL=0" },
    { "Hyrule Field day, crack scan OFF", "OOT_ENTRANCE=0xCD OOT_CRACKSCAN=0" },
    /* 2026-10-04: OOT_NEARW (the eye's clip at w >= 1/64 and the span depth it needs) priced against the default views */
    { "Kokiri Forest, near clip old", "OOT_ENTRANCE=0xEE OOT_NEARW=0" },
    { "Hyrule Field day, near clip old", "OOT_ENTRANCE=0xCD OOT_NEARW=0" },
    /* 2026-10-02, the half-resolution water test switch (a look trade, default off): the water drawn at half
     * resolution and scaled up, against "Deku Tree basement water" */
    { "Deku Tree basement water, half-res water OFF", "OOT_ENTRANCE=0x0 OOT_WARPROOM=3,-165.9,-905,-132.2,0xD8B8,60 OOT_HALFWATER=0" },
    /* 2026-10-07, the cross-port speed round (SM64's exact renderer speedups taken where they apply; BUILDLOG "2026-10-07:
     * SM64's speed round, where it applies"): OOT_FASTROWS and OOT_ZSKIP both off (the code before the round) against
     * each default view above, and each one off alone in Kokiri Forest and Death Mountain Trail (an OFF view is this
     * build with that switch off: the new code's off path, 0.1-1.2% slower under qemu than the build before) */
#define HH_XSPEED_OFF "OOT_FASTROWS=0 OOT_ZSKIP=0"
    { "Kokiri Forest, speed round OFF", "OOT_ENTRANCE=0xEE " HH_XSPEED_OFF },
    { "Hyrule Field day, speed round OFF", "OOT_ENTRANCE=0xCD " HH_XSPEED_OFF },
    { "Lake Hylia, speed round OFF", "OOT_ENTRANCE=0x102 " HH_XSPEED_OFF },
    { "Deku Tree basement water, speed round OFF", "OOT_ENTRANCE=0x0 OOT_WARPROOM=3,-165.9,-905,-132.2,0xD8B8,60 " HH_XSPEED_OFF },
    { "Death Mountain Trail, speed round OFF", "OOT_ENTRANCE=0x13D " HH_XSPEED_OFF },
    { "Morpha's room, speed round OFF", "OOT_ENTRANCE=0x417 OOT_ADULT=1 " HH_XSPEED_OFF },
    { "Kokiri Forest, fast rows OFF", "OOT_ENTRANCE=0xEE OOT_FASTROWS=0" },
    { "Kokiri Forest, hidden-span skip OFF", "OOT_ENTRANCE=0xEE OOT_ZSKIP=0" },
    { "Death Mountain Trail, fast rows OFF", "OOT_ENTRANCE=0x13D OOT_FASTROWS=0" },
    { "Death Mountain Trail, hidden-span skip OFF", "OOT_ENTRANCE=0x13D OOT_ZSKIP=0" },
#undef HH_XSPEED_OFF
    /* 2026-10-01: the profile of the default water (exact, lean rows); its fps against "Deku Tree basement water" is
     * the profiler's cost */
    { "Deku Tree basement water, profiled", "OOT_ENTRANCE=0x0 OOT_WARPROOM=3,-165.9,-905,-132.2,0xD8B8,60 OOT_PROF=1 OOT_PROF_FROM=150 OOT_PROF_OUT=/mnt/SDCARD/App/OcarinaOfTime/data/prof_basement.txt" },
};
#define NVIEW1 ((int)(sizeof(kView) / sizeof(kView[0])))
/* "OoT Bench 2" (2026-10-04, the final speed review: "every area and effect runs well"): the heaviest and most varied
 * moments of the whole-game cost scan (ARM build under qemu, BUILDLOG "2026-10-04: final speed review"), the defaults,
 * each run as the Bench's views are (402 frames at the cap, twice); a view may move its window (OOT_BENCH_FROM /
 * OOT_EXIT_AT: the vision's flames from frame 800, the fights from 1200, A tapped every 30 reads). The last five price
 * that review's exact speedups (each "<scene>, <switch> OFF"), the last four the flame round's. Launched as oot_bench2 (the same binary under
 * another name) or with OOT_BENCH=2; the log is the same bench.log, its session line says "oot bench2". */
static const BenchView kView2[] = {
    { "Song of Storms, Kakariko", "OOT_ENTRANCE=0xDB OOT_SPAWN=0x18B,0,145" },
    { "Song of Storms, Hyrule Field", "OOT_ENTRANCE=0xCD OOT_SPAWN=0x18B,0,145" },
    { "Deku Tree's vision, the flames", "OOT_ENTRANCE=0xEE OOT_CUTSCENE=0xFFF1 OOT_EVENTCHK=0x07,0x09 OOT_BENCH_FROM=800 OOT_EXIT_AT=1202 OOT_INPUT=100:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:" },
    { "Ganondorf fight", "OOT_ENTRANCE=0x41F OOT_ADULT=1 OOT_BENCH_FROM=1200 OOT_EXIT_AT=1602 OOT_INPUT=100:,220:u,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a" },
    { "Barinade fight", "OOT_ENTRANCE=0x301 OOT_BENCH_FROM=1200 OOT_EXIT_AT=1602 OOT_INPUT=100:,80:u,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a" },
    { "Volvagia fight", "OOT_ENTRANCE=0x305 OOT_ADULT=1 OOT_WARPROOM=1,340,150,0,-0x4000,110 OOT_BENCH_FROM=1200 OOT_EXIT_AT=1602 OOT_INPUT=100:,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a" },
    { "Barinade's room", "OOT_ENTRANCE=0x301" },
    { "Water Temple", "OOT_ENTRANCE=0x10 OOT_ADULT=1" },
    { "Zora's Domain", "OOT_ENTRANCE=0x108" },
    { "Zora's Domain frozen", "OOT_ENTRANCE=0x108 OOT_ADULT=1" },
    { "Haunted Wasteland", "OOT_ENTRANCE=0x130 OOT_ADULT=1" },
    { "Ganon's Tower", "OOT_ENTRANCE=0x41B OOT_ADULT=1" },
    { "Ganon's Castle", "OOT_ENTRANCE=0x467 OOT_ADULT=1" },
    { "Fishing pond", "OOT_ENTRANCE=0x45F" },
    { "Hyrule Field adult", "OOT_ENTRANCE=0xCD OOT_ADULT=1" },
    { "Zora's River", "OOT_ENTRANCE=0xEA" },
    { "Death Mountain Crater", "OOT_ENTRANCE=0x147 OOT_ADULT=1" },
    { "Great Fairy's fountain", "OOT_ENTRANCE=0x315" },
    { "Song of Storms, Kakariko, sprite bilinear OFF", "OOT_ENTRANCE=0xDB OOT_SPAWN=0x18B,0,145 OOT_MAGBIL2D=0" },
    { "Song of Storms, Kakariko, detail tint OFF", "OOT_ENTRANCE=0xDB OOT_SPAWN=0x18B,0,145 OOT_DETLERP=0" },
    { "Haunted Wasteland, sprite bilinear OFF", "OOT_ENTRANCE=0x130 OOT_ADULT=1 OOT_MAGBIL2D=0" },
    { "Zora's Domain, detail clamp OFF", "OOT_ENTRANCE=0x108 OOT_DETCLAMP=0" },
    { "Ganon's Castle, no-z blend rows OFF", "OOT_ENTRANCE=0x467 OOT_ADULT=1 OOT_NOZLEAN=0" },
    /* 2026-10-04, the flame overlays and the final sweep (BUILDLOG "2026-10-04: the flame overlays"): their exact switches
     * off, against the views above (an OFF view is the new build with one switch off, not the build before) */
    { "Deku Tree's vision, the flames, inside taps OFF", "OOT_ENTRANCE=0xEE OOT_CUTSCENE=0xFFF1 OOT_EVENTCHK=0x07,0x09 OOT_BENCH_FROM=800 OOT_EXIT_AT=1202 OOT_INPUT=100:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26: OOT_CLAMPTAP=0" },
    { "Deku Tree's vision, the flames, early alpha test OFF", "OOT_ENTRANCE=0xEE OOT_CUTSCENE=0xFFF1 OOT_EVENTCHK=0x07,0x09 OOT_BENCH_FROM=800 OOT_EXIT_AT=1202 OOT_INPUT=100:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26:,4:a,26: OOT_ALPHASKIP=0" },
    { "Volvagia fight, flame texel rows OFF", "OOT_ENTRANCE=0x305 OOT_ADULT=1 OOT_WARPROOM=1,340,150,0,-0x4000,110 OOT_BENCH_FROM=1200 OOT_EXIT_AT=1602 OOT_INPUT=100:,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a OOT_TPEROWS=0" },
    { "Volvagia fight, lava blend NEON OFF", "OOT_ENTRANCE=0x305 OOT_ADULT=1 OOT_WARPROOM=1,340,150,0,-0x4000,110 OOT_BENCH_FROM=1200 OOT_EXIT_AT=1602 OOT_INPUT=100:,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a,28:,2:a OOT_BLENDNEON=0" },
};
#define NVIEW2 ((int)(sizeof(kView2) / sizeof(kView2[0])))
static int sBenchSet = 1; /* 1: kView ("OoT Bench"), 2: kView2 ("OoT Bench 2") */
#define NVIEW (sBenchSet == 2 ? NVIEW2 : NVIEW1)
#define VIEWS (sBenchSet == 2 ? kView2 : kView)
/* frames 150..550 timed: 20 s at the cap. The walk: turn and run so the camera sweeps the view. */
#define BENCH_FROM "150"
#define BENCH_EXIT "552"
#define BENCH_INPUT "120:,60:u,40:ul,60:u,40:ur,60:u,30:,40:l,60:u,40:r,60:u,40:dl,60:u"

static void bench_setenv_list(const char* list) {
    char buf[1024], *p, *save = NULL;

    snprintf(buf, sizeof(buf), "%s", list);
    for (p = strtok_r(buf, " ", &save); p != NULL; p = strtok_r(NULL, " ", &save)) {
        char* eq = strchr(p, '=');
        if (eq != NULL) {
            *eq = '\0';
            setenv(p, eq + 1, 1);
        }
    }
}

int hh_bench_wanted(const char* argv0) {
    const char* base = strrchr(argv0, '/');
    const char* e = getenv("OOT_BENCH");

    base = base ? base + 1 : argv0;
    if (getenv("OOT_BENCH_CHILD") != NULL) {
        return 0;
    }
    if (strcmp(base, "oot_bench2") == 0 || (e != NULL && e[0] == '2')) {
        sBenchSet = 2;
        return 1;
    }
    return strcmp(base, "oot_bench") == 0 || (e != NULL && e[0] == '1');
}

int hh_bench_main(void) {
    char self[512], card[512], log[600], assets[512];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    int rep, v;

    if (n <= 0) {
        printf("[bench] no /proc/self/exe\n");
        return 1;
    }
    self[n] = '\0';
    snprintf(card, sizeof(card), "%s", port_save_dir()); /* App/OcarinaOfTime/data on the device */
    snprintf(assets, sizeof(assets), "%s", getenv("OOT_ASSETS") ? getenv("OOT_ASSETS") : card);
    snprintf(log, sizeof(log), "%s/bench.log", card);
    {
        FILE* f = fopen(log, "a");
        extern const unsigned int hh_build_stamp;
        if (f != NULL) {
            fprintf(f, "BENCHSESSION oot%s build %08x | %d views x 2 runs, frames %s..%s, 20 fps cap\n",
                    sBenchSet == 2 ? " bench2" : "", (unsigned)hh_build_stamp, NVIEW, BENCH_FROM, BENCH_EXIT);
            fclose(f);
        }
    }
    {   /* first start (hh_convert.c): the bench builds the game data like the game does */
        extern void HhConvert_Ensure(const char* root);
        extern void HhRomScreen_Shutdown(void);
        HhConvert_Ensure(assets);
        HhRomScreen_Shutdown(); /* SDL down before any child: a held display hangs the child's (hh_romscreen.c) */
    }
    {   /* the game data must be this build's: a child with another build's embedded.bin stops at the
         * no-data screen (round 53: v51's first bench, 11 runs, no BENCH line) */
        char emb[600];
        unsigned char tr[8] = { 0 };
        extern const unsigned int hh_build_stamp;
        FILE* e;
        snprintf(emb, sizeof(emb), "%s/embedded.bin", assets);
        e = fopen(emb, "rb");
        const int ok = e != NULL && fseek(e, -8, SEEK_END) == 0 && fread(tr, 1, 8, e) == 8 &&
                       (memcmp(tr + 4, "HHEB", 4) == 0 || memcmp(tr + 4, "FKEB", 4) == 0) &&
                       (unsigned)(tr[0] | (tr[1] << 8) | (tr[2] << 16) | ((unsigned)tr[3] << 24)) == hh_build_stamp;
        if (e != NULL) fclose(e);
        if (!ok) {
            FILE* f = fopen(log, "a");
            if (f != NULL) {
                fprintf(f, "BENCHSTOP %s is %s (build %02x%02x%02x%02x), this is build %08x: the first-start build did not run\n",
                        emb, e == NULL ? "missing" : "from another build", tr[3], tr[2], tr[1], tr[0], (unsigned)hh_build_stamp);
                fclose(f);
            }
            printf("[bench] %s: not this build's data\n", emb);
            return 1;
        }
    }
    printf("[bench] %d views x 2 runs -> %s\n", NVIEW, log);
    fflush(stdout);
    /* tests: OOT_BENCH_QUICK=1 -- one run of the first two views, frames 60..92 (=2: two runs);
     * OOT_BENCH_TESTQUIT=<run>,<view>: that child takes EXIT GAME's path at frame 70 */
    const int quick = getenv("OOT_BENCH_QUICK") != NULL;
    const int qreps = quick && getenv("OOT_BENCH_QUICK")[0] == '2' ? 2 : 1;
    int tqRun = 0, tqView = 0;
    if (getenv("OOT_BENCH_TESTQUIT") != NULL) sscanf(getenv("OOT_BENCH_TESTQUIT"), "%d,%d", &tqRun, &tqView);
    signal(SIGUSR1, bench_signal); /* a user signal */
    signal(SIGTERM, bench_signal);
    signal(SIGCHLD, SIG_DFL); /* a launcher's SIG_IGN would auto-reap the children: waitpid loses their status */
    const char* stop = NULL; /* why the bench stopped early */
    int stopView = 0, stopRun = 0;
    for (rep = 0; rep < (quick ? qreps : 2) && stop == NULL; rep++) {
        for (v = 0; v < (quick ? 2 : NVIEW) && stop == NULL; v++) {
            system("rm -rf /tmp/oot_bench; mkdir -p /tmp/oot_bench"); /* every run boots with no save */
            const pid_t pid = fork();
            if (pid == 0) {
                char tag[128], pre[256];
                char* args[16] = { (char*)"oot", NULL };
                int na = 0;
                snprintf(tag, sizeof(tag), "%s (run %d)", VIEWS[v].name, rep + 1);
                setenv("OOT_BENCH_CHILD", tag, 1);
                setenv("OOT_BENCH_LOG", log, 1);
                setenv("OOT_BENCH_FROM", quick ? "60" : BENCH_FROM, 1);
                setenv("OOT_EXIT_AT", quick ? "92" : BENCH_EXIT, 1);
                setenv("OOT_DATA", "/tmp/oot_bench", 1);
                setenv("OOT_ASSETS", assets, 1);
                setenv("OOT_DAYTIME", "0x8000", 1);
                setenv("OOT_TIMELOCK", "0x8000", 1);
                setenv("OOT_INPUT", BENCH_INPUT, 1);
                bench_setenv_list(VIEWS[v].env);
                if (rep + 1 == tqRun && v + 1 == tqView) setenv("OOT_EXITSAVE_AT", "70", 1);
                /* tests: OOT_BENCH_EXEC="qemu-arm -L <sysroot>" runs each child through qemu (no
                 * binfmt there), OOT_BENCH_SELF the guest binary (/proc/self/exe is qemu there) */
                if (getenv("OOT_BENCH_EXEC") != NULL) {
                    char *p, *save = NULL;
                    snprintf(pre, sizeof(pre), "%s", getenv("OOT_BENCH_EXEC"));
                    for (p = strtok_r(pre, " ", &save); p != NULL && na < 13; p = strtok_r(NULL, " ", &save)) {
                        args[na++] = p;
                    }
                    args[na++] = getenv("OOT_BENCH_SELF") ? getenv("OOT_BENCH_SELF") : self;
                    args[na] = NULL;
                    execv(args[0], args);
                    _exit(127);
                }
                execv(self, args);
                _exit(127);
            }
            if (pid > 0) {
                const time_t t0 = time(NULL);
                int st = 0;
                pid_t r;
                while ((r = waitpid(pid, &st, WNOHANG)) == 0 && time(NULL) - t0 < 180 && !sBenchSignal) {
                    usleep(200000);
                }
                if (r == 0 && sBenchSignal) {
                    kill(pid, SIGTERM);
                    usleep(300000);
                    kill(pid, SIGKILL);
                    waitpid(pid, &st, 0);
                    stop = sBenchSignal == SIGUSR1 ? "a user signal" : "a terminate signal";
                    stopView = v + 1; stopRun = rep + 1;
                } else if (r == 0) {
                    kill(pid, SIGKILL);
                    waitpid(pid, &st, 0);
                    FILE* f = fopen(log, "a");
                    if (f != NULL) {
                        fprintf(f, "BENCH %s (run %d) | stopped after 180 s\n", VIEWS[v].name, rep + 1);
                        fclose(f);
                    }
                } else if (WIFEXITED(st) && WEXITSTATUS(st) == HH_BENCH_QUIT) {
                    stop = "the user (EXIT GAME)";
                    stopView = v + 1; stopRun = rep + 1;
                } else if (r > 0 && (!WIFEXITED(st) || WEXITSTATUS(st) != 0)) {
                    printf("[bench] %s (run %d): child ended with status 0x%x\n", VIEWS[v].name, rep + 1, st);
                    FILE* f = fopen(log, "a");
                    if (f != NULL) {
                        fprintf(f, "BENCH %s (run %d) | ended with status 0x%x after %ld s\n", VIEWS[v].name, rep + 1, st,
                                (long)(time(NULL) - t0));
                        fclose(f);
                    }
                }
            } else {
                printf("[bench] fork failed\n");
            }
            fflush(stdout);
            if (stop == NULL && sBenchSignal) {
                stop = sBenchSignal == SIGUSR1 ? "a user signal" : "a terminate signal";
                stopView = v + 1; stopRun = rep + 1;
            }
            if (stop == NULL) {
                sleep(1); /* the display driver settles between runs */
            }
        }
    }
    {
        FILE* f = fopen(log, "a");
        if (f != NULL) {
            if (stop != NULL) {
                fprintf(f, "BENCHSTOP stopped by %s at view %d of %d (%s, run %d)\n", stop, stopView, NVIEW,
                        VIEWS[stopView - 1].name, stopRun);
            } else {
                fprintf(f, "BENCHEND all runs done\n");
            }
            fclose(f);
        }
    }
    printf("[bench] %s\n", stop ? "stopped" : "done");
    fflush(stdout);
    return 0;
}
