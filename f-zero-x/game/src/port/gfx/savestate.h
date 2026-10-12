/* Save states (src/port/sdl/savestate.c): the API of mario-kart-64's
 * savestate.h. A state holds the game's own memory only -- its .data/.bss, the OS layer
 * in src/port/ultra (fibers and their stacks, queues, clock, SRAM) and the N64's 4 MB --
 * never the port's (src/port/savestate.ld). SAVESTATE_EXCLUDE stays a no-op: the port's
 * objects are outside the saved ranges already. SAVESTATE_INCLUDE puts a port variable
 * the game depends on into them (rom.c's DMA notes, which MIO0 decoding reads). */
#ifndef SAVESTATE_H
#define SAVESTATE_H
#define SAVESTATE_EXCLUDE
#define SAVESTATE_INCLUDE __attribute__((section(".data.savestate")))
#define QUICKSAVE_SLOT 0xFFFF

int  savestate_available(void);            /* 0 would hide SAVE / LOAD in the menu */
/* Queue a save/load; applied between retraces by savestate_check(). */
void savestate_request_save(int slot);
void savestate_request_load(int slot);
void savestate_check(void);
/* A state file for this slot / one this exact build can load (the menu shows "Used"
 * only for the latter: states never survive a rebuild). */
int  savestate_slot_exists(int slot);
int  savestate_slot_valid(int slot);

/* At launch main() hands argv to savestate_parse_args() (--loadStateFile <state>, for tests);
 * savestate_boot_prompt() (after the menu is up) resumes that state at once, or asks
 * RESUME GAME / NEW GAME on a cold start with an auto save. */
void savestate_parse_args(int argc, char** argv);
void savestate_boot_prompt(void);
void savestate_request_exit_save(void);    /* EXIT GAME: quicksave (the auto save), then quit */
void savestate_shutdown(void);             /* after the loop */
/* bench.c: the same state kept in RAM (no card writes), restored as a load is. */
int  savestate_mem_snapshot(void);
int  savestate_mem_restore(void);
#endif
