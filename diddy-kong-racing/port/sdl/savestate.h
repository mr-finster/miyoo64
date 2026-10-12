/**
 * Save states. Snapshots the binary's writable game memory (.data + .bss)
 * between the linker symbols __data_start and _end. Works because the binary
 * is non-PIE at a fixed address (0x10000000): every pointer inside the
 * snapshot is valid again on any later boot, so states survive power cycles
 * (that is what makes a true resume possible).
 *
 * State that must NOT be clobbered on load -- live SDL/TTF handles, malloc'd
 * framebuffers, the audio ring the callback thread is draining, the big
 * texture arena -- is tagged SAVESTATE_EXCLUDE into the "dontsave" section,
 * which load preserves across the restore. The texture cache is flushed on
 * load (existing per-frame path) so textures re-import from restored course
 * data.
 */
#ifndef PORT_SAVESTATE_H
#define PORT_SAVESTATE_H

/* DKR: save states are an include-list (port/gamestate.ld); port-side state
 * is outside it by construction, so this tag is a no-op kept for the MK64 kit. */
#define SAVESTATE_EXCLUDE
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

/* Power-off: the OS's SIGTERM -> savestate_request_resume_save(): the next iteration boundary
 * saves the auto save and quits. At boot main() passes argv to savestate_parse_args()
 * (--loadStateFile <path> loads that state); the state is restored a few frames in (after game
 * init) by savestate_check(). */
void savestate_set_progname(const char *argv0);
void savestate_parse_args(int argc, char **argv);
void savestate_request_resume_save(void);
int  savestate_resume_save_pending(void); /* requested, the save not started yet (main.c's music tail hold) */
int  savestate_boot_load_pending(void); /* 1 if launched to resume a quicksave */
/* After gfx_init: RESUME GAME / NEW GAME when an auto save exists (handheld). */
void savestate_boot_prompt(void);
/* 1 while the game loop runs: a power-off is handled by the game (save + quit). */
int savestate_power_ready(void);
/* EXIT GAME: quicksave (the auto save RESUME GAME offers), then quit. */
void savestate_request_exit_save(void);
/* After the game loop. */
void savestate_shutdown(void);

#endif
