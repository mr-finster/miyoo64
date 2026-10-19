/* Included first in every game-side file (Makefile.handheld). */
#ifndef HH_COMPAT_H
#define HH_COMPAT_H
/* mainLoop.c's busy waits for the retrace: one thread here, so the wait runs the retrace (hh_nusys.c) */
void hh_wait_retrace(void);
#define HH_WAIT_RETRACE() hh_wait_retrace()
/* the main loop's own wait between two steps: the one place a save state is taken or restored (hh_host.c) */
void hh_wait_retrace_main(void);
#define HH_WAIT_RETRACE_MAIN() hh_wait_retrace_main()
#endif
