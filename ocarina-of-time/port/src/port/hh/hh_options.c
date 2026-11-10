/* MENU > OPTIONS: Ocarina of Time's rows (the kit's miyoo_options.c draws the page; docs/OPTIONS.md).
 *
 * One row per setting: its debug.cfg key, which the game already reads at start (hh_main.c HhLoadDebugCfg:
 * key=value -> OOT_<KEY>), the values the page offers, the default (SETTINGS.md), a help line, and whether it
 * applies at once (apply() below) or after a restart. A new setting is one more row here; its key and
 * SETTINGS.md's line come first. */
#if defined(MIYOO)

#include <stdlib.h>
#include "miyoo_options.h"

extern const char* port_save_dir(void);
extern void HhInput_FineAimReload(void); /* hh_input.c */
extern int gfx_soft_hud_box;             /* gfx_soft.c: OOT_HUDFILTER, read on each HUD draw */

/* debug.cfg's folder, as hh_main.c's HhAssetRoot: $OOT_ASSETS (tests), else the data folder */
const char* miyoo_options_dir(void) {
    const char* root = getenv("OOT_ASSETS");

    return root != NULL ? root : port_save_dir();
}

static void FineAimApply(const char* value) {
    (void)value;
    HhInput_FineAimReload();
}

static void Fps30Apply(const char* value) {
    extern int port_fi_enabled; /* hh_interp.c; hh_sched.c's 30 fps ticks follow it from the next frame */

    port_fi_enabled = value[0] == '1';
}

static void HudFilterApply(const char* value) {
    gfx_soft_hud_box = value[0] != '0';
}

const struct miyoo_option miyoo_options[] = {
    { .label = "CONTROLS" },
    { .label = "FINE AIM", .key = "fineaim", MIYOO_OPT_ONOFF, .def = "1", .when = MIYOO_OPT_NOW, .apply = FineAimApply,
      .help = "AIMING AND FIRST PERSON: THE D-PAD STARTS GENTLY, THEN SPEEDS UP" },
    { .label = "FINE AIM START", .key = "fineaimpct",
      MIYOO_OPT_VALUES({ "10", "10%" }, { "20", "20%" }, { "30", "30%" }, { "50", "50%" }, { "75", "75%" }),
      .def = "20", .when = MIYOO_OPT_NOW, .apply = FineAimApply,
      .help = "FINE AIM: HOW HARD THE D-PAD PUSHES AT FIRST" },
    { .label = "FINE AIM FULL AT", .key = "fineaimms",
      MIYOO_OPT_VALUES({ "500", "0.5 S" }, { "1000", "1 S" }, { "1500", "1.5 S" }, { "2000", "2 S" }),
      .def = "1000", .when = MIYOO_OPT_NOW, .apply = FineAimApply,
      .help = "FINE AIM: WHEN A HELD D-PAD REACHES FULL SPEED" },
    { .label = "PICTURE" },
    { .label = "30 FPS", .key = "fps30", MIYOO_OPT_ONOFF, .def = "0", .when = MIYOO_OPT_NOW, .apply = Fps30Apply,
      .help = "IN-BETWEEN FRAMES: SMOOTHER MOTION, THE SAME GAME SPEED" },
    { .label = "PRERENDERED ROOMS", .key = "crop3d",
      MIYOO_OPT_VALUES({ "2", "FOLLOW LINK" }, { "1", "CENTRED" }, { "0", "SQUEEZED" }), .def = "2",
      .help = "TRUE SHAPE, THE VIEW FOLLOWING LINK OR CENTRED; OR SQUEEZED TO FIT" },
    { .label = "DIALOG TEXT", .key = "msgbox1to1", MIYOO_OPT_VALUES({ "1", "N64 SIZE" }, { "0", "NARROW" }),
      .def = "1", .help = "THE LETTERS AT THE N64'S SIZE, OR THE OLD NARROWER BOX" },
    { .label = "SMOOTH HUD", .key = "hudfilter", MIYOO_OPT_ONOFF, .def = "1", .when = MIYOO_OPT_NOW,
      .apply = HudFilterApply, .help = "HUD, SPRITES AND TEXT SMOOTHED; OFF: PLAIN SCALING" },
    { .label = "DEKU WATER GRAIN", .key = "watergrain", MIYOO_OPT_ONOFF, .def = "1",
      .help = "THE DEKU TREE BASEMENT WATER'S FINE GRAIN; OFF IS A LITTLE FASTER" },
    { .label = "SOUND" },
    { .label = "SOFT SOUND CUTS", .key = "declick", MIYOO_OPT_ONOFF, .def = "1",
      .help = "SOUND FADES AROUND THE MENU AND LOADS; OFF: HARD CUTS" },
};
const int miyoo_options_count = (int)(sizeof(miyoo_options) / sizeof(miyoo_options[0]));

#endif
