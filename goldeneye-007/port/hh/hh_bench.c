/*
 * The on-device bench (round 19; F-Zero X's bench.c without save states): device A/B timing
 * of the speed switches, with nothing to press.
 *
 * Launched as ge007_bench (the "GoldenEye Bench" launcher: the same binary under another name) or
 * with GE_BENCH=1, the process sets up the card (log, debug.cfg, ROM) and then runs the game
 * once per variant as a child: the Dam from its stage boot, walked, turned and fired by a pad
 * script (armlevels'), unpaced (GE_NOPACE=1), so every run draws exactly the same frames
 * whatever its speed. Each variant runs twice, interleaved, so the spread between a variant's
 * two runs shows the noise. A child times the frames of VI 700-1300 and writes one BENCH line
 * to the log (hh_gfx.c), then shuts SDL down (gfx_wm_shutdown: without it the second run hung
 * at its first flip on the device) and exits; saves go to /tmp, never the player's eeprom.bin.
 * A run still going after 150 s is stopped by the watchdog, which logs where it was.
 * No sound in the runs (GE_AUDIO=0: unpaced, the mixer would outrun the card), but for the paced views at the end.
 *
 * Bench 2 (2026-10-04; launched as ge007_bench2, the "GoldenEye Bench 2" launcher, or GE_BENCH=2): the same chain over
 * kView2, 18 places (a level each, Bond placed and held by GE_BENCHPOS, bondview2.c), VI 460-1060 timed, two runs.
 * Its BENCH lines are tagged "B2 <view>".
 *
 * Bench 3 (2026-10-10; launched as ge007_bench3, the "GoldenEye Bench 3" launcher, or GE_BENCH=3): a short one, kView3 --
 * four places x (four exact variants unpaced, uncap off / on paced with sound) x two runs, about 14 minutes on the device. Its BENCH lines are tagged "B3 <view>".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ultra64.h>
#include "hh_plat.h"

const char *hh_card_setup(void);

/* GE_SPROF's file: next to the log (the Miyoo Mini's App/GoldenEye/data) */
#if defined(TARGET_MIYOO)
#define BENCH_PROF "GE_SPROF=/mnt/SDCARD/App/GoldenEye/data/bench_prof.bin"
#else
#define BENCH_PROF "GE_SPROF=" HH_DEFAULT_SAVE_DIR "/bench_prof.bin"
#endif

static const struct {
    const char *name;
    const char *env; /* space-separated NAME=value pairs over the build's defaults */
} kVariant[] = {
    { "all on", "" },
    /* 2026-10-07 (ge-next): SM64's speed round in GoldenEye's lean rows (exact; read against "all on"; GE_SPANVEC and
     * GE_ZSKIP default on, GE_FASTROWS off since the device Benches of 2026-10-10) */
    { "fast rows ON", "GE_FASTROWS=1" },                 /* a lean triangle's row loop in a small function (GE_FASTROWS; default off since the 2026-10-10 Benches) */
    { "span vectors OFF", "GE_SPANVEC=0" },              /* the lean rows' span set-up as NEON vectors (GE_SPANVEC) */
    { "z skip OFF", "GE_ZSKIP=0" },                      /* a span wholly behind the depth buffer skipped (GE_ZSKIP) */
    { "2026-10-07 round OFF", "GE_FASTROWS=0 GE_SPANVEC=0 GE_ZSKIP=0" }, /* the code as v1.0.2 */
    { "lean rows OFF", "GE_LEANROW=0" },                 /* 2026-10-01: hot_row/hot_row_vk as one lean function (default on) */
    { "lean VK rows OFF", "GE_LEANVK=0" },               /* the lean hot_row only */
    { "lean triangles ON", "GE_LEANTRI=1" },             /* the row loop in hot_tri_lean (default off since the 2026-10-02 Bench) */
    { "lean no-depth rows OFF", "GE_LEANNOZ=0" },        /* the gun and hands, the sea: raster_tri's spans again */
    { "no hot_row", "GE_HOTROW=0" },
    { "staging present", "GE_PRES=0" },
    { "present without prefetch", "GE_PRES=1" },         /* 2026-10-01: the dithered present prefetches too (GE_PRES=2) */
    { "2D skip OFF", "GE_2DSKIP=0" },                    /* 2026-10-01: sprites under the alpha reject skipped whole */
    { "mip bias 1", "GE_MIPBIAS=1" },                    /* round 22: cheaper distant textures (blurrier), for the user to judge */
    { "mip LOD old", "GE_MIPLOD=0" },                    /* 2026-10-03: the old level pick, nearest texels off level 0 (GE_MIPLOD, default 2) */
    { "mip far 2", "GE_MIPFAR=2" },                      /* 2026-10-05: levels 2+ nearest, no blend (GE_MIPFAR, experimental, default off) */
    { "mip far 3", "GE_MIPFAR=3" },                      /* levels 3+ nearest, no blend */
    { "mip LOD fast pick OFF", "GE_LODFAST=0" },         /* 2026-10-04: GE_MIPLOD's level pick inline, compares on the core (GE_LODFAST, default on) */
    { "mip LOD texel pairs OFF", "GE_TBILPAIR=0 GE_BILPAIR=0" }, /* 2026-10-04: a bilinear quad's row as one load in the lean rows (GE_TBILPAIR, GE_BILPAIR) */
    { "2026-10-04 paths OFF", "GE_LODFAST=0 GE_TBILPAIR=0 GE_BILPAIR=0 GE_TBILNEON=0" }, /* the code as the 9367b1b Bench */
    { "lean clamp OFF", "GE_LEANCLAMP=0" },              /* 2026-10-04 final sweep: clamped textures' spans in the lean rows (GE_LEANCLAMP, default on) */
    /* 2026-10-05 (ge-mipneon): GE_MIPLOD=2's price, exact speedups (each default on) */
    { "mip two-level span loop OFF", "GE_TBILSPAN=0" },  /* the two-level spans' loop with fewer core registers (GE_TBILSPAN) */
    { "bilinear span loop OFF", "GE_BILSPAN=0" },        /* the same loop for the one-level bilinear spans (GE_BILSPAN) */
    { "mip LOD lean pick OFF", "GE_LODLEAN=0" },         /* the level pick's max on NEON, its tiles without branches (GE_LODLEAN) */
    { "clamped spans OFF", "GE_CLAMPSPAN=0" },           /* a clamped texture's spans past its edge in the lean loops (GE_CLAMPSPAN) */
    { "2026-10-05 mip paths OFF", "GE_TBILSPAN=0 GE_BILSPAN=0 GE_LODLEAN=0 GE_CLAMPSPAN=0" }, /* the code as v1.0.1 */
    { "mip LOD N64-exact", "GE_MIPLOD=1" },              /* the RDP's rule as is (softer) */
    { "mip LOD min-axis", "GE_MIPLOD=3" },               /* the level from the smaller texel step */
    { "combiner per slot", "GE_RAWFAST=1" },             /* round 37: the combiner compiled per state (default 2) */
    { "combiner per channel", "GE_RAWFAST=0" },          /* round 34: raw_cycle's inputs once per slot */
    { "syscall clock", "GE_CLOCK=syscall" },             /* round 34: the vDSO clock (167 vs 610 ns a read on the device) */
    { "round 33 paths", "GE_RAWFAST=0 GE_CLOCK=syscall" },
    { "half-res sky", "GE_HALFSKY=2" },                  /* round 43: the old default (off now, the user) */
    { "detail grain", "GE_DETAILGRAIN=1" },                 /* round 43: off by default (the user: barely visible at 240) */
    { "brilinear", "GE_TRILIN=0" },
    { "N64 shade", "GE_PSHADE=0" },                     /* round 43: perspective-correct shade is the default now */                       /* round 43: full trilinear is the default now (the user) */
    { "no dither", "GE_DITHER=0" },                      /* round 39: the panel's ordered dither */
    { "Surface 2, fog split", "GE_ARGS=-level_43 GE_FOGPX=1" }, /* round 41: per-pixel-exact fog (off by default since round 43) */
    { "Surface 2, game fog", "GE_ARGS=-level_43 GE_FOGPX=0" },
    { "Surface 2, fog split, old cut colour", "GE_ARGS=-level_43 GE_FOGPX=1 GE_FOGPXSHADE=0" }, /* 2026-10-04: the split's snow waves (GE_FOGPXSHADE) */
    { "Cradle, fog split", "GE_ARGS=-level_41 GE_FOGPX=1" }, /* the most fog splits of the 20 missions (worst case; off there by default) */
    { "Cradle, game fog", "GE_ARGS=-level_41 GE_FOGPX=0" },
    { "Cradle, 2026-10-04 paths OFF", "GE_ARGS=-level_41 GE_LODFAST=0 GE_TBILPAIR=0 GE_BILPAIR=0 GE_TBILNEON=0" },
    { "explosions", "GE_EXPLODE=1" },                    /* round 40: smoke and explosions around Bond (the user: "lag inside smoke") */
    { "explosions, lean blended rows OFF", "GE_EXPLODE=1 GE_LEANBLEND=0" }, /* 2026-10-01: the smoke's rows (default on) */
    { "explosions, lean 56x56 sprites OFF", "GE_EXPLODE=1 GE_LEANNP2=0" }, /* 2026-10-02: non-power-of-two sprites in the lean rows (default on) */
    { "explosions, 2D fill OFF", "GE_EXPLODE=1 GE_2DFILL=0" }, /* 2026-10-02: the solid blended rectangles (fades, flashes) on NEON */
    { "explosions, sprite loop in asm", "GE_EXPLODE=1 GE_LEANASM=1" }, /* 2026-10-03: the 56x56 sprites' pixel loop hand-scheduled (GE_LEANASM; default off since the t6 Bench) */
    { "near clip old", "GE_NEARW=0" },                   /* 2026-10-04: GE_NEARW off: the w >= 2 clip and the old span depth */
    { "T-junction fill OFF", "GE_TJFILL=0" },            /* 2026-10-04: GE_TJFILL off: the levels' T-junction cracks show (its price) */
    { "explosions, near clip old", "GE_EXPLODE=1 GE_NEARW=0" },
    { "explosions, fill rule OFF", "GE_EXPLODE=1 GE_FILLRULE=0" }, /* 2026-10-03: owned edges for blended non-AA draws (GE_FILLRULE, default on): its price in the smoke */
    { "explosions, mip LOD old", "GE_EXPLODE=1 GE_MIPLOD=0" }, /* 2026-10-03: GE_MIPLOD's price in the smoke */
    { "explosions, fill rule fast ON", "GE_EXPLODE=1 GE_OWNFAST=1" }, /* 2026-10-04: the owned-edge row ends without VFP compares (GE_OWNFAST; default off since the t10 Bench: 58.11 off vs 58.36 ms on) */
    { "explosions, 2026-10-04 paths OFF", "GE_EXPLODE=1 GE_LODFAST=0 GE_TBILPAIR=0 GE_BILPAIR=0 GE_OWNFAST=0 GE_TBILNEON=0" }, /* the code as the 9367b1b Bench */
    { "explosions, profiled", "GE_EXPLODE=1 " BENCH_PROF },
    { "all on, profiled", BENCH_PROF }, /* round 30: hh_sprof.c, tools/sprof.py */
    /* 2026-10-02 (GE_RSPYIELD, hh_os.c): paced, with the sound on, as played -- the BENCH line's "aud dry" counts the
     * card's underruns; the frames are not the same run to run (real time), the underruns are the measure */
    { "explosions, paced with sound", "GE_EXPLODE=1 GE_NOPACE=- GE_AUDIO=1" },
    { "explosions, paced with sound, RSPYIELD OFF", "GE_EXPLODE=1 GE_NOPACE=- GE_AUDIO=1 GE_RSPYIELD=0" },
};
#define NVAR ((int)(sizeof(kVariant) / sizeof(kVariant[0])))

/* Bench 2 (2026-10-04; "GoldenEye Bench 2" launcher = ge007_bench2, or GE_BENCH=2): the heaviest and most varied
 * places of the game, one view each, the build as it is (the last views: a switch off), so the device's frame time in every
 * kind of scene comes out of one run. Chosen by the 2026-10-04 cost scan of 81 moments (qemu instructions,
 * calibrated on the Bench); the comment is that scan's device estimate and what the view exercises.
 * A view is a level from its stage boot, the intro skipped (B at VI 150, 210, 270), Bond placed at VI 400
 * (GE_BENCHPOS: x, y above the floor, z, facing; bondview2.c) and held there (no damage, the guards' AI
 * stopped), standing still; VI 460-1060 timed (300 frames). Unpaced and without sound, like Bench 1. */
#define B2_PAD "GE_PADSCRIPT=150:b,156:,210:b,216:,270:b,276:"
static const struct {
    const char *name;
    const char *env;
} kView2[] = {
    { "Jungle start", "GE_ARGS=-level_37" },                                  /* 76.5 ms: layered foliage, lerp + blended mip rows */
    { "Frigate engine room", "GE_ARGS=-level_26 GE_BENCHPOS=404,608,-4634,15" }, /* 76.1: rails and grates, the most triangles */
    { "Cradle platform railings", "GE_ARGS=-level_41 GE_BENCHPOS=1281,1508,-2202,15" }, /* 66.7: lattice over the sky, fog */
    { "Egyptian pool hall", "GE_ARGS=-level_32" },                            /* 62.2: column overdraw, water */
    { "Aztec shuttle hangar", "GE_ARGS=-level_28 GE_BENCHPOS=-5000,425,1382,255" }, /* 60.9: big mip-mapped walls */
    { "Caverns big cavern", "GE_ARGS=-level_39 GE_BENCHPOS=-6177,631,-1454,285" }, /* 59.0: geometry and vertex load */
    { "explosions (as Bench 1)", "GE_ARGS=-level_33 GE_EXPLODE=1 GE_BENCH_FROM=700 GE_EXIT_AT=1300 "
      "GE_PADSCRIPT=600:b,606:,700:u,1000:ul,1100:u,1200:ub,1206:u,1250:ub,1256:u" }, /* 56.1: sprites, smoke, 2D fill */
    { "Cradle bridge", "GE_ARGS=-level_41 GE_BENCHPOS=5324,765,-2414,45" },   /* 56.5: fog plus lerp rows */
    { "Frigate deck, helicopter", "GE_ARGS=-level_26 GE_BENCHPOS=565,1120,-7188,15" }, /* 53.1: sea, sky, blended rows */
    { "Control big hall", "GE_ARGS=-level_23 GE_BENCHPOS=4422,220,-565,75" }, /* 52.2: textured rows, guards */
    { "Facility tank hall", "GE_ARGS=-level_34 GE_BENCHPOS=8996,-273,-3567,75" }, /* 51.8: lerp rows indoors */
    { "Streets roadblock", "GE_ARGS=-level_29 GE_BENCHPOS=1299,170,16052,75" }, /* 50.1: the city at night, guards */
    { "Bunker I monitor room", "GE_ARGS=-level_09 GE_BENCHPOS=744,332,-712,45" }, /* 49.8: bilinear blended spans (monitors) */
    { "Dam top by the lake", "GE_ARGS=-level_33 GE_BENCHPOS=-8671,-466,-4301,285" }, /* 49.2: a heavier Dam view */
    { "Train freight car", "GE_ARGS=-level_25 GE_BENCHPOS=-16552,463,273,255" }, /* 48.8: dark interiors */
    { "Aztec storage", "GE_ARGS=-level_28 GE_BENCHPOS=-7105,425,2258,285" },  /* 48.4: the most mip-mapped spans */
    { "Depot chain fence", "GE_ARGS=-level_30 GE_BENCHPOS=1575,136,-6655,195" }, /* 47.8: blended rows (the fence) */
    /* 44.7: 2D, no-depth bilinear spans; START at VI 400, the watch is up by VI ~520, so VI 560-1160 is timed */
    { "watch menu", "GE_ARGS=-level_33 " B2_PAD ",400:s,406: GE_BENCH_FROM=560 GE_EXIT_AT=1160" },
    /* the Cradle's intro, unskipped (the user, 2026-10-04: Trevelyan's run across the dish platform): the game's second
     * intro camera pinned (GE_INTROCAM=1, the one with the run), no buttons, VI 60-620 timed (the flight over the
     * platform, down the tower, round Bond) */
    { "Cradle intro", "GE_ARGS=-level_41 GE_INTROCAM=1 GE_PADSCRIPT=- GE_BENCH_FROM=60 GE_EXIT_AT=620" },
    /* the 2026-10-04 final sweep's switch, off, where it pays most (against the same places above) */
    { "Aztec shuttle hangar, lean clamp OFF", "GE_ARGS=-level_28 GE_BENCHPOS=-5000,425,1382,255 GE_LEANCLAMP=0" },
    { "Jungle start, lean clamp OFF", "GE_ARGS=-level_37 GE_LEANCLAMP=0" },
    /* 2026-10-05 (ge-cradle): the Cradle's two switches against the two Cradle views above (GE_EDGESKIP default off since
     * the device Bench 2 of 2026-10-05: its views are "edge skip ON") */
    { "Cradle platform railings, edge skip ON", "GE_ARGS=-level_41 GE_BENCHPOS=1281,1508,-2202,15 GE_EDGESKIP=1" },
    { "Cradle bridge, edge skip ON", "GE_ARGS=-level_41 GE_BENCHPOS=5324,765,-2414,45 GE_EDGESKIP=1" },
    { "Cradle platform railings, empty triangles OFF", "GE_ARGS=-level_41 GE_BENCHPOS=1281,1508,-2202,15 GE_EMPTYTRI=0" },
    { "Cradle bridge, empty triangles OFF", "GE_ARGS=-level_41 GE_BENCHPOS=5324,765,-2414,45 GE_EMPTYTRI=0" },
    /* 2026-10-05 (ge-mipneon): GE_MIPLOD=2's speedups, each off, on the most mip-mapped view (against "Aztec storage") */
    { "Aztec storage, mip two-level span loop OFF", "GE_ARGS=-level_28 GE_BENCHPOS=-7105,425,2258,285 GE_TBILSPAN=0" },
    { "Aztec storage, bilinear span loop OFF", "GE_ARGS=-level_28 GE_BENCHPOS=-7105,425,2258,285 GE_BILSPAN=0" },
    { "Aztec storage, mip LOD lean pick OFF", "GE_ARGS=-level_28 GE_BENCHPOS=-7105,425,2258,285 GE_LODLEAN=0" },
    { "Aztec storage, clamped spans OFF", "GE_ARGS=-level_28 GE_BENCHPOS=-7105,425,2258,285 GE_CLAMPSPAN=0" },
    { "Aztec storage, 2026-10-05 mip paths OFF",
      "GE_ARGS=-level_28 GE_BENCHPOS=-7105,425,2258,285 GE_TBILSPAN=0 GE_BILSPAN=0 GE_LODLEAN=0 GE_CLAMPSPAN=0" },
    /* 2026-10-07 (ge-next): SM64's speed round, each switch flipped on the view it helps most (against "Jungle start";
     * GE_FASTROWS default off since 2026-10-10, so its view is "ON"), and all three off on two more (against "Cradle
     * platform railings", "Frigate engine room") */
    { "Jungle start, fast rows ON", "GE_ARGS=-level_37 GE_FASTROWS=1" },
    { "Jungle start, span vectors OFF", "GE_ARGS=-level_37 GE_SPANVEC=0" },
    { "Jungle start, z skip OFF", "GE_ARGS=-level_37 GE_ZSKIP=0" },
    { "Jungle start, 2026-10-07 round OFF", "GE_ARGS=-level_37 GE_FASTROWS=0 GE_SPANVEC=0 GE_ZSKIP=0" },
    { "Cradle platform railings, 2026-10-07 round OFF",
      "GE_ARGS=-level_41 GE_BENCHPOS=1281,1508,-2202,15 GE_FASTROWS=0 GE_SPANVEC=0 GE_ZSKIP=0" },
    { "Frigate engine room, 2026-10-07 round OFF",
      "GE_ARGS=-level_26 GE_BENCHPOS=404,608,-4634,15 GE_FASTROWS=0 GE_SPANVEC=0 GE_ZSKIP=0" },
};
#define NVIEW2 ((int)(sizeof(kView2) / sizeof(kView2[0])))

/* Bench 3 (2026-10-10; "GoldenEye Bench 3" launcher = ge007_bench3, or GE_BENCH=3), a short one (~14 min).
 * 1. The two exact switches that also leave GE_FASTROWS's row loop, priced again now that it is off by default. On
 *    2026-10-10 "lean triangles ON" (Bench 1, the Dam: 34.77 ms against all on 35.20) and "edge skip ON" (Bench 2:
 *    railings 63.55 against 64.27, bridge 53.55 against 54.52) came out faster than "all on" -- but either switch takes
 *    a lean triangle off the fast rows, which lost too (Dam "fast rows OFF" 34.47, Jungle 73.12 against 73.72). Each
 *    place: all on (the defaults), GE_LEANTRI=1, GE_EDGESKIP=1, both, back to back. Unpaced and without sound, like
 *    Benches 1 and 2, 100 frames each: the placed views keep Bench 2's timeline (the intro skipped by B at VI 150, 210,
 *    270, Bond placed at 400 and held), timed VI 460-660; the Dam is Bench 1's walk with the intro skipped at VI 150
 *    instead of 600 (the same path from the start pad, 450 VIs earlier: the walk from VI 250), timed on its straight
 *    stretch, VI 250-450 (Bench 1's 700-900).
 * 2. GE_UNCAP (uncapped frame pacing, default on since 2026-10-10): the same four places paced with sound, GE_UNCAP=0
 *    against 1; it changes when frames start, not what they draw, so it shows only in real time: the BENCH line's wall
 *    (the frame interval, fps = 1000 / wall), "aud dry" (the card's underruns). 4 s timed each. Paced, the VIs run in
 *    real time from the process start, and the first frames come later in VIs than unpaced: Bench 2's early B presses
 *    left 3 of 16 paced qemu runs in the intro (a loaded host). So these press B at VI 270, 330, 390 (a skipped intro's
 *    later presses fire, before the placement, as Bench 2's do), place Bond at 500 and time VI 560-800; the Dam walk:
 *    B at 150, 210, 270, the walk from 330, timed 330-570. 16 of 16 qemu runs (the same loaded host) got their view.
 * The list runs once, place after place, then again. */
#define B3_EXIT "660"
#define B3_DAM "GE_ARGS=-level_33 GE_PADSCRIPT=150:b,156:,250:u,550:ul GE_BENCH_FROM=250 GE_EXIT_AT=450"
#define B3_JUNGLE "GE_ARGS=-level_37"
#define B3_RAIL "GE_ARGS=-level_41 GE_BENCHPOS=1281,1508,-2202,15"
#define B3_FRIGATE "GE_ARGS=-level_26 GE_BENCHPOS=404,608,-4634,15"
#define B3_LEANTRI " GE_LEANTRI=1"
#define B3_EDGESKIP " GE_EDGESKIP=1"
#define B3_PACED " GE_NOPACE=- GE_AUDIO=1 GE_PADSCRIPT=270:b,276:,330:b,336:,390:b,396: GE_BENCHPOS_VI=500 GE_BENCH_FROM=560 GE_EXIT_AT=800"
#define B3_PACED_DAM " GE_NOPACE=- GE_AUDIO=1 GE_PADSCRIPT=150:b,156:,210:b,216:,270:b,276:,330:u,630:ul GE_BENCH_FROM=330 GE_EXIT_AT=570"
#define B3_UNCAP " GE_UNCAP=1"
#define B3_CAPPED " GE_UNCAP=0" /* the default is on: the "uncap OFF" views say so */
static const struct {
    const char *name;
    const char *env;
} kView3[] = {
    { "Dam walk", B3_DAM },
    { "Dam walk, lean triangles ON", B3_DAM B3_LEANTRI },
    { "Dam walk, edge skip ON", B3_DAM B3_EDGESKIP },
    { "Dam walk, both ON", B3_DAM B3_LEANTRI B3_EDGESKIP },
    { "Dam walk, paced with sound, uncap OFF", B3_DAM B3_PACED_DAM B3_CAPPED },
    { "Dam walk, paced with sound, uncap ON", B3_DAM B3_PACED_DAM B3_UNCAP },
    { "Jungle start", B3_JUNGLE },
    { "Jungle start, lean triangles ON", B3_JUNGLE B3_LEANTRI },
    { "Jungle start, edge skip ON", B3_JUNGLE B3_EDGESKIP },
    { "Jungle start, both ON", B3_JUNGLE B3_LEANTRI B3_EDGESKIP },
    { "Jungle start, paced with sound, uncap OFF", B3_JUNGLE B3_PACED B3_CAPPED },
    { "Jungle start, paced with sound, uncap ON", B3_JUNGLE B3_PACED B3_UNCAP },
    { "Cradle platform railings", B3_RAIL },
    { "Cradle platform railings, lean triangles ON", B3_RAIL B3_LEANTRI },
    { "Cradle platform railings, edge skip ON", B3_RAIL B3_EDGESKIP },
    { "Cradle platform railings, both ON", B3_RAIL B3_LEANTRI B3_EDGESKIP },
    { "Cradle platform railings, paced with sound, uncap OFF", B3_RAIL B3_PACED B3_CAPPED },
    { "Cradle platform railings, paced with sound, uncap ON", B3_RAIL B3_PACED B3_UNCAP },
    { "Frigate engine room", B3_FRIGATE },
    { "Frigate engine room, lean triangles ON", B3_FRIGATE B3_LEANTRI },
    { "Frigate engine room, edge skip ON", B3_FRIGATE B3_EDGESKIP },
    { "Frigate engine room, both ON", B3_FRIGATE B3_LEANTRI B3_EDGESKIP },
    { "Frigate engine room, paced with sound, uncap OFF", B3_FRIGATE B3_PACED B3_CAPPED },
    { "Frigate engine room, paced with sound, uncap ON", B3_FRIGATE B3_PACED B3_UNCAP },
};
#define NVIEW3 ((int)(sizeof(kView3) / sizeof(kView3[0])))

static void bench_setenv_list(const char *list)
{
    char buf[512], *p, *save = NULL; /* Bench 3's paced Dam walk: ~210 */
    snprintf(buf, sizeof(buf), "%s", list);
    for (p = strtok_r(buf, " ", &save); p != NULL; p = strtok_r(NULL, " ", &save)) {
        char *eq = strchr(p, '=');
        if (eq == NULL)
            continue;
        *eq = 0;
        if (strcmp(eq + 1, "-") == 0)
            unsetenv(p); /* NAME=-: not set (GE_NOPACE=-: a paced run) */
        else
            setenv(p, eq + 1, 1);
    }
}

static int sList = 1; /* which bench: 1 (the switches on the Dam), 2 (the places, kView2) or 3 (kView3) */

int hh_bench_wanted(const char *argv0)
{
    const char *base = strrchr(argv0, '/');
    const char *e = getenv("GE_BENCH");
    base = base ? base + 1 : argv0;
    if (getenv("GE_BENCH_CHILD") != NULL)
        return 0;
    if (strcmp(base, "ge007_bench3") == 0 || (e != NULL && e[0] == '3')) {
        sList = 3;
        return 1;
    }
    if (strcmp(base, "ge007_bench2") == 0 || (e != NULL && e[0] == '2')) {
        sList = 2;
        return 1;
    }
    return strcmp(base, "ge007_bench") == 0 || (e != NULL && e[0] == '1');
}

int hh_bench_main(void)
{
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    const int nrun = sList == 3 ? NVIEW3 : sList == 2 ? NVIEW2 : NVAR;
    int rep, v;
    if (n <= 0) {
        printf("[bench] no /proc/self/exe\n");
        return 1;
    }
    self[n] = 0;
#ifdef TARGET_HANDHELD
    hh_card_setup(); /* the log (children append to it), debug.cfg, the ROM once */
#endif
    if (sList == 3)
        printf("[bench] Bench 3: %d views x 2 runs: unpaced VI 460-" B3_EXIT " (the Dam walk 250-450), paced VI 560-800 (330-570)\n", NVIEW3);
    else if (sList == 2)
        printf("[bench] Bench 2: %d views x 2 runs: each place from its stage boot, VI 460-1060 timed\n", NVIEW2);
    else
        printf("[bench] %d variants x 2 runs: the Dam from its stage boot, VI 700-1300 timed\n", NVAR);
    fflush(stdout);
    for (rep = 0; rep < 2; rep++)
        for (v = 0; v < nrun; v++) {
            const char *vname = sList == 3 ? kView3[v].name : sList == 2 ? kView2[v].name : kVariant[v].name;
            unlink("/tmp/ge_bench/eeprom.bin"); /* every run boots with no save, as the first did */
            pid_t pid = fork();
            if (pid == 0) {
                char tag[96], pre[256];
                char *args[16] = { (char *)"ge007", NULL };
                const char *wd = getenv("GE_BENCH_WATCHDOG");
                int na = 0;
                snprintf(tag, sizeof(tag), "%s%s (run %d)", sList == 3 ? "B3 " : sList == 2 ? "B2 " : "", vname, rep + 1);
                setenv("GE_BENCH_CHILD", tag, 1);
                setenv("GE_ARGS", "-level_33", 1);
                setenv("GE_NOPACE", "1", 1);
                setenv("GE_AUDIO", "0", 1);
                setenv("GE_EXIT_AT", "1300", 1);
                setenv("GE_BENCH_FROM", "700", 1);
                setenv("GE_SAVE_DIR", "/tmp/ge_bench", 1);
                setenv("GE_WATCHDOG", wd ? wd : "150", 1); /* a stuck run reports where, and the next one runs */
                setenv("GE_PADSCRIPT", "600:b,606:,700:u,1000:ul,1100:u,1200:ub,1206:u,1250:ub,1256:u", 1);
                if (sList >= 2) { /* Bench 2's timeline (above; Bench 3: VI 460-660), then the view's own values */
                    setenv("GE_BENCH_FROM", "460", 1);
                    setenv("GE_EXIT_AT", sList == 3 ? B3_EXIT : "1060", 1);
                    setenv("GE_BENCHPOS_VI", "400", 1);
                    bench_setenv_list(B2_PAD);
                    bench_setenv_list(sList == 3 ? kView3[v].env : kView2[v].env);
                } else {
                    bench_setenv_list(kVariant[v].env);
                }
                /* tests: GE_BENCH_EXEC="qemu-arm -L <sysroot> <loader>" runs each child through
                 * qemu (no binfmt there), GE_BENCH_SELF the guest binary (/proc/self/exe is the
                 * loader under qemu) */
                if (getenv("GE_BENCH_EXEC") != NULL) {
                    char *p, *save = NULL;
                    snprintf(pre, sizeof(pre), "%s", getenv("GE_BENCH_EXEC"));
                    for (p = strtok_r(pre, " ", &save); p != NULL && na < 13; p = strtok_r(NULL, " ", &save))
                        args[na++] = p;
                    args[na++] = getenv("GE_BENCH_SELF") ? getenv("GE_BENCH_SELF") : self;
                    args[na] = NULL;
                    execv(args[0], args);
                    _exit(127);
                }
                execv(self, args);
                _exit(127);
            }
            if (pid > 0) {
                int st = 0;
                waitpid(pid, &st, 0);
                if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
                    printf("[bench] %s (run %d): child ended with status 0x%x\n", vname, rep + 1, st);
            } else {
                printf("[bench] fork failed\n");
            }
            fflush(stdout);
            sleep(1); /* the display driver settles between runs */
        }
    printf("[bench] done\n");
    fflush(stdout);
    return 0;
}
