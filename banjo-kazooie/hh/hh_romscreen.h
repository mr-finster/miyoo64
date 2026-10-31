/* hh_romscreen.h: the first-start screens (hh_romscreen.c), drawn while hh_card.c finds and checks
 * the ROM and hh_romtables.c fills the data tables. The first progress call opens the screen;
 * hh_romscreen_finish ends it (black, SDL left up) before hh_plat_init opens the game's. */
#ifndef HH_ROMSCREEN_H
#define HH_ROMSCREEN_H

#define HH_CARD_PCT 80 /* the card work's share of the bar; the tables' fill has 80..99, Done 100 */

/* sub: the ROM's file name (or ""), l1/l2: the job's two lines, stage: the third; pct 0..80 (the card work) */
void hh_romscreen_progress(const char *sub, const char *l1, const char *l2, const char *stage, int pct, const char *foot);
/* the card work is over: the screen stays up for the tables' fill (BK_ROMSCREEN=0: closed, the old order) */
void hh_romscreen_card_done(void);
/* hh_romtables_load: the fill's start, and its steps (p 0..100 of its share, the bar's 80..99) */
void hh_romscreen_fill_begin(void);
void hh_romscreen_fill(const char *stage, int p);
/* everything is built: "Done" 100%, then black; SDL stays up for the game's mode switch */
void hh_romscreen_finish(void);
/* the error on the screen until a button, the log line, then exit(1) */
void hh_romscreen_fail(const char *sub, const char *l1, const char *l2, const char *l3) __attribute__((noreturn));
void hh_romscreen_close(void);

#endif
