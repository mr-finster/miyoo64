/**
 * The game's priority (kit k1.6, from DKR b37c2ef; the 2-second beat, 2026-09-27). An OS battery
 * check that forks helpers every ~2 s, ~15-25 ms of CPU, at the same priority as the game lands inside one
 * frame, which runs late: a hitch every 2 s (0.40 -> 0.05 frames/s losing >= 15 ms, worst frame per window
 * 55.7 -> 43.1 ms, underruns 1 -> 0). A modest nice (not SCHED_RR/FIFO, so nothing in the OS can starve) lets the script run in the
 * game's per-frame gaps. Call hh_game_priority() once at the top of main(), before any thread or child
 * exists: every thread (audio, present) and child
 * inherits it. <PORT>_PRIO=<nice> (HH_ENV("PRIO"); default -10; 0 = off, the old behaviour). Desktop
 * builds need root (or CAP_SYS_NICE in a container: docker run --cap-add SYS_NICE) to go below 0; it
 * logs and keeps what it had.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include "hh_game.h"

void hh_game_priority(void) {
    const char *e = getenv(HH_ENV("PRIO"));
    const int want = e ? atoi(e) : -10;
    const int before = getpriority(PRIO_PROCESS, 0);
    if (want != 0 && setpriority(PRIO_PROCESS, 0, want) != 0)
        fprintf(stderr, "priority: nice %d refused (%s), kept %d\n", want, strerror(errno), before);
    fprintf(stderr, "priority: nice %d (asked %d)\n", getpriority(PRIO_PROCESS, 0), want);
    fflush(stderr);
}
