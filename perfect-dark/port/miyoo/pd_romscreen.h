/* pd_romscreen.h: the ROM screens (pd_romscreen.c), drawn while pd_card.c finds and checks the
 * player's ROM, and sysFatalError's message on the panel. The game's screen (gfx_sdl12.c) is up
 * already (videoInit runs before romdataInit); these draw on it directly. */
#ifndef PD_ROMSCREEN_H
#define PD_ROMSCREEN_H

/* sub: the ROM's file name (or ""), l1/l2: the job's two lines, stage: the third; pct 0..99 */
void pd_romscreen_progress(const char *sub, const char *l1, const char *l2, const char *stage, int pct, const char *foot);
/* the work is over: "Done" 100%, a beat to read it, then black (nothing if no screen was shown) */
void pd_romscreen_finish(void);
/* the error on the screen until a button, the log line, the panel and clock given back, exit(1) */
void pd_romscreen_fail(const char *sub, const char *l1, const char *l2, const char *l3) __attribute__((noreturn));
/* sysFatalError's text, word-wrapped on the screen until a button; returns (the caller exits) */
void pd_romscreen_fatal(const char *msg);

#endif
