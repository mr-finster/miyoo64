/**
 * Save states. A state is the game's writable memory, in the layout the port
 * picks in hh_game.h:
 *  - hole (default): .data + .bss between the linker symbols __data_start and
 *    the end of the game's writable data, minus the "dontsave" section. State
 *    that must NOT be clobbered on load -- live SDL/TTF handles, malloc'd
 *    framebuffers, the audio ring the callback thread is draining, the big
 *    texture arena -- is tagged SAVESTATE_EXCLUDE into that section, which a
 *    load preserves across the restore.
 *  - include list (HH_SS_INCLUDE_LIST): only the .game_data/.game_bss ranges the
 *    port's linker script gathers; SAVESTATE_EXCLUDE is then a no-op.
 * Works because the binary is non-PIE at a fixed address: every pointer inside
 * the snapshot is valid again on any later boot, so states survive power cycles
 * (that is what makes RESUME GAME possible). The texture cache is
 * flushed on load so textures re-import from the restored game data.
 */
#ifndef PORT_SAVESTATE_H
#define PORT_SAVESTATE_H

#include "hh_game.h"

#if defined(HH_SS_INCLUDE_LIST)
#define SAVESTATE_EXCLUDE /* port-side state lies outside the include list by construction */
#else
#define SAVESTATE_EXCLUDE __attribute__((section("dontsave")))
#endif
#define QUICKSAVE_SLOT 0xFFFF

/* Queue a save/load; applied at the next iteration boundary by
 * savestate_check() (never mid-display-list). */
void savestate_request_save(int slot);
void savestate_request_load(int slot);
void savestate_check(void);

/* True if slot has a state file / one this exact build can load (the menu
 * shows "Used" only for the latter: states don't survive a rebuild). */
int  savestate_slot_exists(int slot);
int  savestate_slot_valid(int slot);

/* At boot main() passes argv to savestate_parse_args() (--loadStateFile <state path>, for
 * tests); the state is restored a few frames in (after game init) by savestate_check(). */
void savestate_set_progname(const char *argv0);
void savestate_parse_args(int argc, char **argv);
int  savestate_boot_load_pending(void); /* 1 if launched to resume a quicksave */
/* After gfx_init: RESUME GAME / NEW GAME when an auto save exists (handheld). */
void savestate_boot_prompt(void);
/* EXIT GAME: quicksave (the auto save RESUME GAME offers), then quit. */
void savestate_request_exit_save(void);
/* After the game loop. */
void savestate_shutdown(void);

#endif
