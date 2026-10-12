/* priority.c -- the game's scheduling priority (kit k1.6 hh_priority.c, from DKR b37c2ef).
 *
 * an OS helper script forks every ~2.05 s, ~15-25 ms of CPU on the single
 * core; at the game's priority they land inside one frame, which runs late: a hitch every 2 s in every
 * port (DKR on the device: frames losing >= 15 ms 0.40/s -> 0.05/s, worst frame per window 55.7 -> 43.1 ms,
 * underruns 1 -> 0). A modest nice (not SCHED_RR/FIFO, so nothing in the OS can starve) lets the script
 * run in the game's per-frame gaps. hh_game_priority() runs first thing in main(), before any thread or
 * child exists, so every thread (audio, present) and child inherits it. SSB64_PRIO=<nice> (default -10;
 * 0 = off, the old behaviour). Desktop builds need root, or CAP_SYS_NICE in a container (docker run
 * --cap-add SYS_NICE), to go below 0; otherwise it keeps what it had. The result is logged by
 * hh_game_priority_log() once console.log may be opened (after the bench arguments). */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

extern void port_log_always(const char *fmt, ...);

static int sWant, sBefore, sAfter, sErr;

void hh_game_priority(void)
{
    const char *e = getenv("SSB64_PRIO");
    sWant = e ? atoi(e) : -10;
    sBefore = getpriority(PRIO_PROCESS, 0);
    sErr = 0;
    if (sWant != 0 && setpriority(PRIO_PROCESS, 0, sWant) != 0) sErr = errno;
    sAfter = getpriority(PRIO_PROCESS, 0);
}

void hh_game_priority_log(void)
{
    if (sErr != 0) port_log_always("priority: nice %d refused (%s), kept %d\n", sWant, strerror(sErr), sBefore);
    port_log_always("priority: nice %d (asked %d)\n", sAfter, sWant);
}
