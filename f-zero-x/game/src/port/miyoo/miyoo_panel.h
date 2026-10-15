/* miyoo_panel: the Miyoo Mini family's screen, signals and paths for the ports (kit
 * proposal KM1, the shared Miyoo kit). Built only with -DMIYOO (the port keeps HANDHELD defined: the
 * soft renderer, SDL 1.2 and the rest of the kit are the handheld's).
 *
 * The game renders its own 320x240 (the N64's 4:3: projections and HUD as the N64 laid them out).
 * The panel is 640x480 on the Mini, Mini Plus and Flip, 752x560 on the Mini v4; the frame is
 * scaled by the largest whole factor that fits (2x on every model so far) and centred.
 *
 * Found on the user's Mini Plus (Onion), 2026-09-30 / 10-01, in the MK64, GoldenEye and OoT ports:
 *  - the device's SDL accepts a 16 bpp mode but shows it as streaks: the panel is opened at 32 bpp;
 *  - that SDL hands over the panel as mounted, upside down: every picture is turned 180 degrees
 *    (<GAME>_ROTATE=0 for an SDL that already turns it);
 *  - MI_GFX wants alpha opaque.
 *
 * Usage (gfx_sdl12.c under #if defined(MIYOO)): miyoo_panel_open() in place of SDL_SetVideoMode;
 * each frame, the game thread copies its 320x240 ARGB frame out and the present thread calls
 * miyoo_panel_present32(frame, w, h) under the present lock, then miyoo_panel_flip() outside it.
 * The menu (miyoo_menu.c) draws 640x480 and calls miyoo_panel_show_full(). */
#ifndef MIYOO_PANEL_H
#define MIYOO_PANEL_H
#include <stdint.h>
#include <signal.h>

struct SDL_Surface;

/* The panel, opened once (later calls return it). Size: /dev/fb0's visible size,
 * <GAME>_MIYOO_MODE=<w>x<h> to force one, 640x480 if SDL refuses. NULL if SDL can't open any. */
struct SDL_Surface *miyoo_panel_open(void);
struct SDL_Surface *miyoo_panel_surface(void);
int miyoo_panel_rotated(void);   /* 1: pictures are turned 180 degrees (the default) */

/* A 32-bit 0x??RRGGBB frame (any size up to the panel) onto the panel, scaled by the largest
 * whole factor that fits, centred, turned, alpha forced opaque. The current notice (miyoo_menu.c)
 * is drawn into src first, so src must be the port's own staging copy. 2x is NEON on the device. */
void miyoo_panel_present32(uint32_t *src, int w, int h);
/* SDL_Flip of the panel: call it outside the present lock (it can wait for vsync). */
void miyoo_panel_flip(void);
/* A 640x480 32 bpp software surface (menus, prompts, first-start screens) onto the panel, centred
 * and turned, then flipped. */
void miyoo_panel_show_full(struct SDL_Surface *s);

/* data/ next to the executable (wherever the launcher keeps the game: App/<Game>, a MinUI .pak),
 * or $<GAME>_DATA when the launcher sets it. Created if missing. */
const char *miyoo_data_dir(void);
/* The executable's own folder: rom.z64 there is the player's ROM (the auto-ROM contract: a personal
 * zip puts it there). */
const char *miyoo_exe_dir(void);
/* Where to look for the player's ROM after rom.z64 and the data dir, two folders deep each:
 * "/mnt/SDCARD/Roms/N64", "/mnt/SDCARD"; NULL-terminated. */
extern const char *const miyoo_rom_roots[];

/* The OS ending the game (Onion's and the stock OS's power-off, MinUI's, any plain kill) sends
 * SIGTERM: the handler sets *flag; the game loop then takes EXIT GAME's path (auto save, quit). */
void miyoo_term_install(volatile sig_atomic_t *flag);

/* miyoo_menu.c: the notice on a presented frame, and the menus' 8x8 bitmap font (each pixel k x k,
 * 16 or 32 bpp software surfaces; also for a port's first-start screens). */
void miyoo_menu_overlay(struct SDL_Surface *screen);
void miyoo_text(struct SDL_Surface *s, int x, int y, int k, const char *t, uint32_t argb);
int  miyoo_text_w(const char *t, int k);

/* The Mini's SDL 1.2 keys (stock / Onion / MinUI SDL): */
#define MIYOO_KEY_UP     273
#define MIYOO_KEY_DOWN   274
#define MIYOO_KEY_RIGHT  275
#define MIYOO_KEY_LEFT   276
#define MIYOO_KEY_A      32   /* SDLK_SPACE */
#define MIYOO_KEY_B      306  /* SDLK_LCTRL */
#define MIYOO_KEY_X      304  /* SDLK_LSHIFT */
#define MIYOO_KEY_Y      308  /* SDLK_LALT */
#define MIYOO_KEY_L1     101  /* 'e' */
#define MIYOO_KEY_R1     116  /* 't' */
#define MIYOO_KEY_L2     9    /* SDLK_TAB */
#define MIYOO_KEY_R2     8    /* SDLK_BACKSPACE */
#define MIYOO_KEY_SELECT 305  /* SDLK_RCTRL */
#define MIYOO_KEY_START  13   /* SDLK_RETURN */
#define MIYOO_KEY_MENU   27   /* SDLK_ESCAPE: the menu (the handheld's 'q') */

/* The panel area (first column and row, width, height; unturned) that present shows a w x h frame in: the
 * Mini v4 / Flip full-height fill (<GAME>_FLIPFILL) or the whole-number scale, centred. */
void miyoo_panel_area(int w, int h, int *ox, int *oy, int *pw, int *ph);

#endif
