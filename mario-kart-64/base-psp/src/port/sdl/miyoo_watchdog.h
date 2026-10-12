/* miyoo_watchdog: a stall report for device freezes (the Miyoo build only).
 *
 * A thread of its own wakes four times a second. Once the game has presented its first picture,
 * if no picture follows for MIYOO_WD_SECONDS (3 s; <GAME>_WATCHDOG=<s>, 0 = off) and no modal wait
 * (the menu) is on, it writes one block to stderr (console.log) with write() only -- the stuck
 * thread may hold stdio's lock -- and syncs it to the card:
 *   - what the game thread is doing (the port's loop stage, and the wait it is in: the worker, the
 *     present thread, ...), the worker's handoff state, the present thread's, the audio's;
 *   - every task of the process from /proc (its name, run state, CPU time, and the kernel function
 *     a sleeping one waits in);
 *   - the registered threads' program counters, sampled with a signal (pc and lr: addr2line -f -e
 *     <the binary> <pc> <lr> names them; the binary keeps its symbols).
 * Then it reports again every 10 s while the stall lasts (three blocks at most), so a freeze always
 * leaves evidence even when the player has to kill the game. */
#pragma once
#if defined(MIYOO)

/* What a thread is waiting on, for the report (gMiyooWdGame, gMiyooWdPresent). */
enum {
    WD_RUN = 0,        /* running its own code */
    WD_WORKER_WAIT,    /* the game thread waits for the worker's share of a triangle */
    WD_SKY_WAIT,       /* ... of the held sky's lines */
    WD_PRESENT_WAIT,   /* the game thread waits for the present thread to take the last picture */
    WD_PRESENT_IDLE,   /* the present thread waits for a picture */
    WD_PRESENT_DRAW,   /* the present thread draws a picture onto the panel (under the lock) */
    WD_PRESENT_FLIP,   /* the present thread flips the panel (outside the lock) */
    WD_MENU,           /* the game thread is in the menu */
    WD_COUNT
};
extern volatile int gMiyooWdGame, gMiyooWdPresent;

void miyoo_wd_start(void);                /* the watchdog thread (once); the caller is registered as "game" */
void miyoo_wd_register(const char *name); /* the calling thread, for the program counter sample */
void miyoo_wd_frame(void);                /* a picture went to the present thread (the first one arms it) */

/* The port's lines for the report (each fills buf, at most n bytes, and returns the length). */
int miyoo_wd_detail_game(char *buf, int n);
int miyoo_wd_detail_present(char *buf, int n);
int miyoo_wd_detail_audio(char *buf, int n);
#endif
