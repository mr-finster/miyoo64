/**
 * The game's priority (handheld-kit k1.6 hh_game_priority, from DKR b37c2ef; the lead's round,
 * 2026-09-27). an OS battery check that forks helpers every ~2 s, ~15-25 ms of CPU, at the game's
 * priority lands inside one frame, which runs late: a hitch every 2 s (with nice -10: frames losing >= 15 ms 0.40 -> 0.05 a second, underruns
 * 1 -> 0). A modest nice, not a real-time class, so nothing in the OS starves. port_main.c calls
 * port_game_priority() first thing in main, before any thread or child exists, so all of them
 * (audio, present) inherit it.
 * FZX_PRIO=<nice> (default -10; 0: as before). port_game_priority_log() says what it got once the
 * log is open (the game binary never writes stderr: gate 2b). A desktop build or a container
 * needs root or CAP_SYS_NICE to go below 0. Its own file: <errno.h> defines errno, a field name in
 * the N64 headers.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

void port_log(const char* fmt, ...);

static int sWant, sGot, sErr;

void port_game_priority(void) {
    const char* e = getenv("FZX_PRIO");
    sWant = e ? atoi(e) : -10;
    if (sWant != 0 && setpriority(PRIO_PROCESS, 0, sWant) != 0) {
        sErr = errno;
    }
    sGot = getpriority(PRIO_PROCESS, 0);
}

void port_game_priority_log(void) {
    if (sErr != 0) {
        port_log("priority: nice %d refused (%s), kept %d\n", sWant, strerror(sErr), sGot);
    }
    port_log("priority: nice %d (asked %d)\n", sGot, sWant);
}
