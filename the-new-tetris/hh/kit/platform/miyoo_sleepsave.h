/* miyoo_sleepsave: the resume state written as the Miyoo goes to sleep (the shared miyoo/kit; the same file in
 * all ten games).
 *
 * Onion's sleep (keymon) freezes the game with SIGSTOP when POWER is released, and turns the screen off. If the
 * Mini then powers off while asleep (Onion's timer, not charging), the game gets SIGCONT, then SIGTERM, and is
 * SIGKILLed about 1.4 s later: the exit save had that long, and nothing at all with the MENU overlay open
 * (handover/sleep-poweroff-REPORT.md). Onion does not grab the buttons until it sleeps, so the game sees POWER go
 * down first: that is when the resume state (the slot RESUME GAME offers) is written, before the freeze.
 *
 *  - A thread reads the buttons from the input device (/dev/input/event0, as keymon does; SDL 1.2 on the Mini
 *    gives POWER no key of its own). It only notes the time of the last button event and of a POWER press.
 *  - At the game's iteration boundary, savestate_check() asks miyoo_sleepsave_due():
 *      MIYOO_SLEEPSAVE_POWER: POWER went down (not with MENU held: that is Onion's screenshot), at most 3 s ago;
 *      MIYOO_SLEEPSAVE_IDLE: no button for <GAME>_SLEEPSAVE_IDLE seconds (default 50: before Onion's shortest
 *      idle sleep, 1 min), once per idle period (no more until a button is pressed). A gap in the boundaries
 *      over 1.5 s (the game was stopped, or loading) restarts the count, so a wake never saves at once.
 *    and writes the state: the game pauses only while the state goes into the kernel's page cache; the fsync
 *    and the rename run on a helper thread (miyoo_sleepsave_async) while the game goes on. A SIGSTOP in the
 *    middle is fine: the old state is kept until the new one is whole, and the save goes on at SIGCONT.
 *  - The MENU overlay asks miyoo_sleepsave_menu_should_close() each pass: on a POWER press it steps aside (as
 *    it does for SIGTERM), the boundary saves, and the overlay is opened again (an SDL MENU key event).
 *
 * <GAME>_SLEEPSAVE=0 turns it all off. <GAME>_SLEEPSAVE_IDLE=<s> (0: no idle save). MIYOO_INPUT_DEV=<path>
 * (tests): read the button events from there (a FIFO of struct input_event). One log line per save:
 *     sleepsave: power press, resume state written (N ms)
 *     sleepsave: idle 50 s, resume state written (N ms)
 * Built only with -DMIYOO; elsewhere the calls are no-ops. */
#ifndef MIYOO_SLEEPSAVE_H
#define MIYOO_SLEEPSAVE_H

enum { MIYOO_SLEEPSAVE_NONE = 0, MIYOO_SLEEPSAVE_POWER = 1, MIYOO_SLEEPSAVE_IDLE = 2 };

/* Start the button thread (once; savestate_check calls it). env: the switch's name, e.g. "PD_SLEEPSAVE". */
void miyoo_sleepsave_start(const char *env);
/* At the iteration boundary, with the game loop running and nothing else to save: the save due now, if any
 * (consumed). */
int miyoo_sleepsave_due(void);
/* After that save (any thread): its kind and 1 = on the card. Logs the line, with the ms from the press (or
 * the boundary, for idle) to the state on the card. */
void miyoo_sleepsave_done(int kind, int ok);
/* The MENU overlay, each pass: 1 = a POWER press is waiting; close, the game loop saves and opens it again. */
int miyoo_sleepsave_menu_should_close(void);
/* Run fn(arg) on a helper thread (the fsync and rename of a sleep save). Joins any earlier one first; runs it
 * here if no thread can be made. */
void miyoo_sleepsave_async(void (*fn)(void *), void *arg);
/* Wait for the helper thread, if one runs (before any other save or load, and at exit). */
void miyoo_sleepsave_join(void);
/* 1 while the helper thread writes. */
int miyoo_sleepsave_busy(void);
/* CLOCK_MONOTONIC ms. */
double miyoo_sleepsave_ms(void);

#endif
