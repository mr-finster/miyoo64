/* miyoo_remap.h: MENU > CONTROLS, the button remap shared by every Miyoo port (miyoo_remap.c).
 *
 * The remap is a permutation of the Miyoo's ten remappable buttons (A B X Y L1 L2 R1 R2 START
 * SELECT), applied where the port takes a key (its controller_handheld_key): a pressed button
 * acts as the button it stands in for, so every mode the port has (aiming, chords, the menus'
 * own layout, GoldenEye's and Perfect Dark's control styles) keeps working on the new button.
 * MENU and the d-pad are never remapped. Saved per game in <data dir>/controls.cfg; without the
 * file (or with the defaults) every key passes unchanged.
 *
 * The canonical copy lives in kit-remap/ at the repository root; each port vendors an identical
 * copy next to its miyoo_menu.c (kit-remap/sync.sh copies and checks them). */
#ifndef MIYOO_REMAP_H
#define MIYOO_REMAP_H

/* The Miyoo's buttons (MB_DPAD: the four directions, never remapped). */
enum {
    MB_NONE, MB_A, MB_B, MB_X, MB_Y, MB_L1, MB_L2, MB_R1, MB_R2, MB_START, MB_SELECT, MB_DPAD, MB_COUNT
};
#define MB_REMAPPABLE 10 /* MB_A .. MB_SELECT */

/* The N64's inputs (the rows' icons); MR_TEXT: a port action drawn as its name, no N64 icon. */
enum {
    MR_A, MR_B, MR_Z, MR_START, MR_L, MR_R, MR_CUP, MR_CDOWN, MR_CLEFT, MR_CRIGHT, MR_STICK, MR_DPAD, MR_TEXT
};

/* One row of the screen: the N64 input and the Miyoo button that gives it in the port's default
 * layout (in play). alt: a second button that gives it too (0: none). mod: a button held with
 * btn for it (a chord, e.g. START + L1 = N64 L; 0: none). A row whose btn is MB_NONE or
 * MB_DPAD, or with a mod, is shown but cannot be changed here. what: a few words (or NULL). */
struct miyoo_remap_row {
    unsigned char n64, btn, alt, mod;
    const char *what;
};

/* The port's layout, next to its key mapping. */
extern const struct miyoo_remap_row miyoo_remap_rows[];
extern const int miyoo_remap_nrows;

/* An SDL 1.2 key sym as the player pressed it -> the sym the port's mapping should see. */
int miyoo_remap_key(int sym);

struct SDL_Surface;
/* The CONTROLS screen, modal, over the menu's dimmed backdrop: draw is the menu's compose surface
 * (640x480 or the panel's size), backdrop the dimmed frame of the same size, show() puts draw on
 * the panel, key(sym, 0) lets go of a key in the game (the menu's pass-through), should_close()
 * ends the screen at once (power off, sleep). show and should_close may not be NULL; key may. */
void miyoo_remap_screen(struct SDL_Surface *draw, struct SDL_Surface *backdrop, void (*show)(void),
                        void (*key)(int sym, int down), int (*should_close)(void));

/* Headless check (<GAME>_MENU_TEST): the screen's states to <dir>/controls_*.ppm. */
void miyoo_remap_test_dump(struct SDL_Surface *draw, struct SDL_Surface *backdrop, const char *dir);

#endif
