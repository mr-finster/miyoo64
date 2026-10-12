#ifndef HH_GAME_H
#define HH_GAME_H
/* SM64's settings for the kit's platform files (kit/ports/sm64/hh_game.h, k1),
 * and the game-side hooks into the handheld layer (TARGET_HANDHELD only). */

#define HH_GAME_ID   "sm64"          /* file names, the state build id, the relaunch's fallback name */
#define HH_GAME_TITLE "SUPER MARIO 64" /* the Miyoo menu's heading (kit miyoo_menu.c) */
#define HH_ENV(name) "SM64_" name     /* environment knobs: SM64_DATA, SM64_RESUME_ANSWER, SM64_MENU_TEST */
#define HH_SS_MAGIC  0x534D3653u    /* 'SM6S': save-state header magic */
/* Sparse save states (kit k1.5): each range as a 4 KB page bitmap plus only its non-zero pages;
 * full-size states (the header's nranges without 0x100) still load. */
#define HH_SS_SPARSE 1
/* port/Makefile links the asset region's game-state head first and marks its end
 * with __assets_state_end; the kit's savestate.c picks the symbol up by itself. */

/* The frame being built is a menu screen (title logo, file select, star
 * select): its 3D models are laid out with its 2D text for the 320-wide
 * screen, so gfx_pc maps them like the text -- squeezed onto the square
 * screen -- instead of cropping them like the world. Set during the geo
 * walk, read by gfx_run for the same frame, then cleared. */
extern int gfx_ui_frame;
static inline void hh_ui_frame(void) { gfx_ui_frame = 1; }

/* Screen-space x of a 320-wide layout, mapped the way the 3D world is on a
 * screen narrower than 4:3: the middle of the view at the N64's scale. For
 * flat drawing tied to world positions (the wipe transitions). */
float hh_crop_x(float x);

#endif
