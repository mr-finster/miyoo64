/**
 * The game's priority (the 2-second beat, 2026-09-27). an OS helper script forked every
 * ~2.05 s, ~15-25 ms of CPU on the single core, and at the same priority as the game they land inside
 * one frame, which then runs late: a hitch every 2 s in every port. A modest nice (not SCHED_RR/FIFO,
 * so nothing in the OS can starve) lets the script run in the game's per-frame gaps instead. main()
 * sets it once, before any thread exists: every thread (audio, present, the worker) and every
 * child we run inherits it, so audio and present stay level with the
 * game. DKR_PRIO=<nice> (default -10; 0 = off, the old behaviour). The desktop build needs root to go
 * below 0; it logs and keeps what it had.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

extern void port_log_always(const char *fmt, ...);

void game_priority(void) {
    const char *e = getenv("DKR_PRIO");
    const int want = e ? atoi(e) : -10;
    const int before = getpriority(PRIO_PROCESS, 0);
    if (want != 0 && setpriority(PRIO_PROCESS, 0, want) != 0)
        port_log_always("priority: nice %d refused (%s), kept %d\n", want, strerror(errno), before);
    port_log_always("priority: nice %d (asked %d)\n", getpriority(PRIO_PROCESS, 0), want);
}
