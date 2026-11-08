/* MENU > OPTIONS: Majora's Mask's rows (the kit's miyoo_options.c draws the page; docs/OPTIONS.md).
 *
 * One row per setting: its debug.cfg key, the values the page offers, the default, a help line, and whether it
 * applies at once (apply() below) or after a restart. The game reads debug.cfg at start through this table
 * (hh_mm.c main: miyoo_options_load, each row's line -> its variable), so only the keys listed here are read
 * from the file; launch.sh's variables still work without a line. env: the variable the code reads, MM_<KEY>
 * by default; the renderer and the sound shared with Ocarina of Time read OOT_<KEY>. */
#if defined(MIYOO)

#include "miyoo_options.h"

extern void HhInput_FineAimReload(void); /* hh_input.c */
extern int gfx_soft_hud_box;             /* gfx_soft.c: OOT_HUDFILTER, read on each HUD draw */

static void FineAimApply(const char* value) {
    (void)value;
    HhInput_FineAimReload();
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
    { .label = "DIALOG TEXT", .key = "msgbox1to1", .env = "OOT_MSGBOX1TO1",
      MIYOO_OPT_VALUES({ "1", "N64 SIZE" }, { "0", "NARROW" }), .def = "1",
      .help = "THE LETTERS AT THE N64'S SIZE, OR THE OLD NARROWER BOX" },
    { .label = "SMOOTH HUD", .key = "hudfilter", .env = "OOT_HUDFILTER", MIYOO_OPT_ONOFF, .def = "1",
      .when = MIYOO_OPT_NOW, .apply = HudFilterApply, .help = "HUD, SPRITES AND TEXT SMOOTHED; OFF: PLAIN SCALING" },
    { .label = "SOUND" },
    { .label = "SOFT SOUND CUTS", .key = "declick", .env = "OOT_DECLICK", MIYOO_OPT_ONOFF, .def = "1",
      .help = "SOUND FADES AROUND THE MENU AND LOADS; OFF: HARD CUTS" },
};
const int miyoo_options_count = (int)(sizeof(miyoo_options) / sizeof(miyoo_options[0]));

#endif
