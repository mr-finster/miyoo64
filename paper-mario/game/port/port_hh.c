/* What the kit's menu and save states ask of the port (the kit's menu round). Host side.
 * The menu (MENU: RESUME, SAVE, LOAD, VOLUME, BRIGHTNESS, EXIT GAME) and the power-off's auto save and
 * resume run on the device only; the save states themselves run on the desktop proxy too (the cross-process tests).
 * Paper Mario draws one screen format (320x240, squeezed to the panel), so the display and 3D-size settings the
 * menu's older versions set are no-ops here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include "port.h"
#include "savestate.h"
#include "handheld_menu.h"

/* EXIT GAME, and the window closing */
void port_request_exit(void) { port_quit = 1; }

/* The kit parks a mixer thread around a save or load (MK64's backstop). Paper Mario mixes on the game thread, and
 * the SDL audio callback only reads the output ring, which is not game memory: nothing to park or lock. */
void port_audio_backstop_enable(int on) { (void) on; }
int port_audio_backstop_swap(int on) { (void) on; return 0; }
void port_audio_mix_lock(void) {}
void port_audio_mix_unlock(void) {}

#ifdef PM64_SDL12
#include <SDL/SDL.h>
#include <spawn.h>
#include <sys/wait.h>
#include <poll.h>
#include <time.h>

int gHandheldDisplayMode SAVESTATE_EXCLUDE = HH_DISPLAY_CONVERT;
void gfx_sdl12_set_display_mode(int mode) { gHandheldDisplayMode = mode; }
void gfx_sdl12_set_res_dynamic(int on) { (void) on; }
int gfx_sdl12_get_res_dynamic(void) { return 0; }
void gfx_sdl12_set_res_pct(int pct) { (void) pct; }

/* The Miyoo Mini (README_MIYOO.md): the OS ends the game with SIGTERM (Onion, the stock OS and MinUI power off that way; Allium sends it and kills 5 s later;
 * any `kill`): the handler only sets the flag, the loop then takes EXIT GAME's path (port_sdl.c: the asset region's pages
 * staged, the auto save, quit), and the next start offers RESUME GAME. The menu and the resume prompt step aside while it
 * is set (handheld_power_pending). */
#include "miyoo_panel.h"
static volatile sig_atomic_t sTerm;
int handheld_power_pending(void) { return sTerm; }

void port_hh_init(SDL_Surface *screen) {
    miyoo_term_install(&sTerm);
    extern int gfx_soft_magbilin; /* as below: PM64_MAGBILIN is the port's own, not the menu's */
    const int magbilin = gfx_soft_magbilin;
    handheld_menu_init(screen); /* miyoo_menu.c */
    gfx_soft_magbilin = magbilin;
    const char *td = getenv("PM64_MENU_TEST");
    if (td && td[0]) { handheld_menu_test_dump(td); exit(0); }
}

/* Once a frame (port_sdl.c's poll): SIGTERM seen -> 1, once (the flag stays up for the menu and the prompt until then) */
int port_hh_take_power(void) {
    if (!sTerm) return 0;
    sTerm = 0;
    return 1;
}
#endif /* PM64_SDL12 */
