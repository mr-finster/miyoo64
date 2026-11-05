/* hh_soak.c: a test driver for long runs (tests only; off unless PW64_SOAK is set). It walks a list of tests --
 * every vehicle, class, test, island and pilot -- in one process: it puts the next test in place of the vehicle, pilot
 * and test menus (as the game's own demo does: pilot, vehicle, class and test, then the test's set-up state), flies it
 * with one of a few behaviours, and lets the game's own results, retry and class-complete flows run, pressing A
 * through them.
 *
 *   PW64_SOAK=all | tests | extras | <veh>.<cls>.<test>.<pilot>[,...]   the list ("all": the 51 tests below and the
 *       extras: the options' photo album, class complete for every class and vehicle, the ending); veh 0 hang glider,
 *       1 rocket belt, 2 gyrocopter, 3 cannonball, 4 sky diving, 5 jumble hopper, 6 birdman (cls = island 0-3,
 *       test = time of day 0-2), 7 options, 8 class complete (test = the vehicle), 9 the ending; cls 0 beginner, 1 A,
 *       2 B, 3 pilot (bonus games: level 0-2); pilot 0-5
 *   PW64_SOAK_FRAMES=<n>   game frames a test flies before the results are forced (default 900)
 *   PW64_SOAK_TEST=<n>     game frames a list entry may take with its retries before the next one (default 2400)
 *   PW64_SOAK_SS=<n>       save state (slot 3) every n game frames and load it back n/2 frames later (0: none)
 *   PW64_SOAK_LOOPS=<n>    times through the list (default 1); then the process exits
 *   PW64_SOAK_FROM=<n>     start at entry n
 *   PW64_SOAK_SHOT=<dir>   (PC build) a picture of every flight 150 frames in
 * The behaviour of entry i: i%4 == 0 no input (the craft drifts down: a crash or a landing), 1 flies (stick and
 * thrust), 2 quits through the pause menu after 300 frames, 3 opens the pause menu's second item (retry / replay)
 * after 200 frames and then flies. The results screen's first item is Retry: entries are retried until their time
 * is up. "SOAK" lines go to the log with the process's memory (VmRSS, VmHWM) at every new entry and every 600
 * frames. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "savestate.h"

extern unsigned int hh_vicount;
extern void savestate_request_save(int slot), savestate_request_load(int slot);

#define SOAK_MAX 256
static struct { unsigned short pilot, veh, cls, test; } sList[SOAK_MAX] SAVESTATE_EXCLUDE;
static int sN SAVESTATE_EXCLUDE = -1, sAt SAVESTATE_EXCLUDE = -1, sLoops SAVESTATE_EXCLUDE = 1, sLoop SAVESTATE_EXCLUDE;
static unsigned sFlyFrames SAVESTATE_EXCLUDE = 900, sTestFrames SAVESTATE_EXCLUDE = 2400, sSS SAVESTATE_EXCLUDE;
static unsigned sFrame SAVESTATE_EXCLUDE;     /* game frames since the process started (gameUpdate calls) */
static unsigned sEntryFrame SAVESTATE_EXCLUDE; /* sFrame at the start of this list entry */
static unsigned sFlyFrame SAVESTATE_EXCLUDE;   /* game frames in TEST_UPDATE since this test (or retry) started */
static unsigned sSSAt SAVESTATE_EXCLUDE;       /* next save-state frame */
static int sSSLoadPending SAVESTATE_EXCLUDE;
static int sState SAVESTATE_EXCLUDE;           /* the game state at the last call */
static unsigned sPauseVi SAVESTATE_EXCLUDE;    /* the pause sequence started at this VI (0: none) */
static int sPauseDone SAVESTATE_EXCLUDE;
static int sMenuA SAVESTATE_EXCLUDE;
static unsigned sMenuVi SAVESTATE_EXCLUDE;       /* the press sMenuA last counted (vi / 40) */
static int sLastBefore SAVESTATE_EXCLUDE = -1;
char hh_soak_shot[256] SAVESTATE_EXCLUDE;       /* gfx_null.c dumps the frame to this path, then clears it */
static unsigned sShots SAVESTATE_EXCLUDE;  /* the state at the last hh_soak_before */            /* A presses in this results stay */
static unsigned sTests SAVESTATE_EXCLUDE, sRetries SAVESTATE_EXCLUDE, sSaves SAVESTATE_EXCLUDE, sLoads SAVESTATE_EXCLUDE;
static unsigned sForced SAVESTATE_EXCLUDE, sQuits SAVESTATE_EXCLUDE, sCongrats SAVESTATE_EXCLUDE, sCredits SAVESTATE_EXCLUDE;

enum { ST_TITLE = 0, ST_TEST_DETAILS = 2, ST_PILOT = 3, ST_SETUP = 4, ST_UPDATE = 5, ST_RESULTS = 6, ST_OPTIONS = 7, ST_FILE = 10,
       ST_VEHCLASS = 11, ST_OVERVIEW = 12, ST_RESULTS_CB = 13, ST_CONGRATS = 14, ST_CREDITS = 15 };

static void add(int veh, int cls, int test)
{
    if (sN < SOAK_MAX) {
        sList[sN].veh = (unsigned short)veh;
        sList[sN].cls = (unsigned short)cls;
        sList[sN].test = (unsigned short)test;
        sList[sN].pilot = (unsigned short)(sN % 6);
        sN++;
    }
}

static void soak_init(void)
{
    const char *e = getenv("PW64_SOAK");
    sN = 0;
    if (e == NULL || !e[0])
        return;
    if (!strcmp(e, "all") || !strcmp(e, "tests")) {
        static const int tests[4] = { 1, 2, 3, 3 }; /* taskGetTestCount: beginner 1, A 2, B 3, pilot 3 */
        int v, c, t;
        for (c = 0; c < 4; c++)
            for (v = 0; v < 3; v++)
                for (t = 0; t < tests[c]; t++)
                    add(v, c, t);
        for (c = 0; c < 3; c++) {
            add(3, c, c);  /* cannonball level c, target c */
            add(3, c, 3);
            add(4, c, 0);  /* sky diving */
            add(5, c, 0);  /* jumble hopper */
        }
        for (c = 0; c < 4; c++)
            for (t = 0; t < 3; t++)
                add(6, c, t); /* birdman: island c, time of day t */
    }
    if (!strcmp(e, "all") || !strcmp(e, "extras")) {
        int v, c;
        add(7, 0, 0); /* options: the photo album (file 1), back to the title */
        for (c = 0; c < 4; c++)
            for (v = 0; v < 3; v++)
                add(8, c, v); /* class complete (the congratulations sequence) for class c, vehicle v */
        add(9, 0, 0); /* the ending (credits) */
    } else if (strcmp(e, "all") && strcmp(e, "tests")) {
        while (*e) {
            int v = 0, c = 0, t = 0, p = 0;
            if (sscanf(e, "%d.%d.%d.%d", &v, &c, &t, &p) >= 3 && sN < SOAK_MAX) {
                add(v, c, t);
                sList[sN - 1].pilot = (unsigned short)p;
            }
            e = strchr(e, ',');
            if (e == NULL)
                break;
            e++;
        }
    }
    if ((e = getenv("PW64_SOAK_FRAMES")) != NULL && atoi(e) > 0) sFlyFrames = (unsigned)atoi(e);
    if ((e = getenv("PW64_SOAK_TEST")) != NULL && atoi(e) > 0) sTestFrames = (unsigned)atoi(e);
    if ((e = getenv("PW64_SOAK_SS")) != NULL && atoi(e) > 0) sSS = (unsigned)atoi(e);
    if ((e = getenv("PW64_SOAK_LOOPS")) != NULL && atoi(e) > 0) sLoops = atoi(e);
    if ((e = getenv("PW64_SOAK_FROM")) != NULL && atoi(e) > 0 && atoi(e) < sN) { sAt = atoi(e) - 1; sEntryFrame = 0u - sTestFrames; } /* start at entry n */
    sSSAt = sSS;
    fprintf(stderr, "SOAK: %d tests, %u frames a flight, %u an entry, save state every %u, %d loop(s)\n", sN,
            sFlyFrames, sTestFrames, sSS, sLoops);
}

int hh_soak_on(void)
{
    if (sN < 0)
        soak_init();
    return sN > 0;
}

static void mem_kb(long *rss, long *hwm)
{
    char line[128];
    FILE *f = fopen("/proc/self/status", "r");
    *rss = *hwm = -1;
    if (f == NULL)
        return;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "VmRSS:", 6)) *rss = atol(line + 6);
        else if (!strncmp(line, "VmHWM:", 6)) *hwm = atol(line + 6);
    }
    fclose(f);
}

static void soak_log(const char *what)
{
    long rss, hwm;
    mem_kb(&rss, &hwm);
    fprintf(stderr, "SOAK %s vi %u frame %u entry %d/%d loop %d (veh %d cls %d test %d pilot %d) state %d | tests %u retries %u "
            "forced %u quits %u congrats %u credits %u saves %u loads %u | rss %ld kB hwm %ld kB\n", what, hh_vicount, sFrame,
            sAt, sN, sLoop, sAt >= 0 ? sList[sAt].veh : -1, sAt >= 0 ? sList[sAt].cls : -1, sAt >= 0 ? sList[sAt].test : -1,
            sAt >= 0 ? sList[sAt].pilot : -1, sState, sTests, sRetries, sForced, sQuits, sCongrats, sCredits, sSaves,
            sLoads, rss, hwm);
}

/* gameUpdate, before the state's function: a state to go to instead (the next test's set-up, with *spec = pilot,
 * vehicle, class, test filled in), or -1. */
int hh_soak_before(int state, unsigned short *spec)
{
    int next = 0;
    if (!hh_soak_on())
        return -1;
    sFrame++;
    if (sFrame % 600 == 0)
        soak_log("mem");
    if (sSS) { /* save, then load back half a period later: a round trip in whatever state the game is in */
        if (sFrame >= sSSAt && !sSSLoadPending) {
            savestate_request_save(3);
            sSaves++;
            sSSLoadPending = 1;
            sSSAt = sFrame + sSS / 2;
        } else if (sSSLoadPending && sFrame >= sSSAt) {
            savestate_request_load(3);
            sLoads++;
            sSSLoadPending = 0;
            sSSAt = sFrame + sSS / 2;
        }
    }
    if (state == ST_CONGRATS && sState != ST_CONGRATS) sCongrats++;
    if (state == ST_CREDITS && sState != ST_CREDITS) sCredits++;
    {
        const int last = sLastBefore;
        sLastBefore = state;
        if (state != ST_SETUP || last == ST_SETUP)
            goto not_setup;
    }
    {
        if (sAt >= 0 && sFrame - sEntryFrame < sTestFrames) {
            sRetries++; /* the results' Retry (or the pause menu's): the same entry again */
            sFlyFrame = 0;
            sPauseVi = 0;
            sPauseDone = 0;
            sState = state;
            return -1;
        }
        next = 1;
    }
not_setup:
    if (state == ST_VEHCLASS || state == ST_OVERVIEW || state == ST_TEST_DETAILS || state == ST_PILOT)
        next = 1;
    if (!next) {
        sState = state;
        return -1;
    }
    if (sAt < 0 || sFrame - sEntryFrame >= sTestFrames) { /* the next entry */
        sAt++;
        if (sAt >= sN) {
            sLoop++;
            if (sLoop >= sLoops) {
                soak_log("done");
                fflush(stdout);
                fflush(stderr);
                exit(0);
            }
            sAt = 0;
        }
        sEntryFrame = sFrame;
        sTests++;
        sState = state; /* for the log: the state it replaces */
        soak_log("test");
    } else { /* time left: this entry again (after a quit or the results' Next / Another test / Quit) */
        sRetries++;
        sState = state;
        soak_log("again");
    }
    spec[0] = sList[sAt].pilot;
    spec[1] = sList[sAt].veh;
    spec[2] = sList[sAt].cls;
    spec[3] = sList[sAt].test;
    sMenuA = 0;
    if (sList[sAt].veh >= 7) { /* the extras: options/album, class complete, the ending */
        const int to = sList[sAt].veh == 7 ? ST_OPTIONS : sList[sAt].veh == 8 ? ST_CONGRATS : ST_CREDITS;
        if (sList[sAt].veh == 8) {
            spec[1] = sList[sAt].test; /* the vehicle */
            spec[3] = 0;
        } else {
            spec[1] = 0;
            spec[2] = 0;
            spec[3] = 0;
        }
        sEntryFrame = sFrame - sTestFrames; /* once: the next menu moves on */
        sState = to;
        sLastBefore = to;
        return to;
    }
    sFlyFrame = 0;
    sPauseVi = 0;
    sPauseDone = 0;
    sState = ST_SETUP;
    sLastBefore = ST_SETUP;
    return ST_SETUP;
}

/* gameUpdate, after the state's function: a test past its time goes to its results. */
int hh_soak_after(int prev, int next)
{
    if (!hh_soak_on())
        return next;
    if (prev == ST_UPDATE && next == ST_UPDATE && sAt >= 0) {
        sFlyFrame++;
        if (sFlyFrame == 150 && getenv("PW64_SOAK_SHOT") != NULL) /* PW64_SOAK_SHOT=<dir>: a picture 150 frames in */
            snprintf(hh_soak_shot, sizeof hh_soak_shot, "%s/s%04u_e%02d_v%dc%dt%dp%d.ppm", getenv("PW64_SOAK_SHOT"),
                     sShots++, sAt, sList[sAt].veh, sList[sAt].cls, sList[sAt].test, sList[sAt].pilot);
        if (sFlyFrame >= sFlyFrames && sList[sAt].veh != 3 /* cannonball: its own results (RESULTS_CB) */) {
            sForced++;
            sState = ST_RESULTS;
            return ST_RESULTS;
        }
    }
    if (prev == ST_UPDATE && next != ST_UPDATE && next != ST_RESULTS && next != ST_RESULTS_CB)
        sQuits++;
    if (next != prev)
        fprintf(stderr, "SOAK state %d -> %d vi %u frame %u\n", prev, next, hh_vicount, sFrame);
    if (next != sState)
        sMenuA = 0;
    sState = next;
    return next;
}

/* hh_input_poll: the keys while a soak runs ('a' A, 's' START, 'd'/'u'/'l'/'r' stick, 'b' B, 'm' Z). */
void hh_soak_keys(char *keys)
{
    const unsigned vi = hh_vicount;
    int beh;
    keys[0] = 0;
    if (!hh_soak_on())
        return;
    {   /* unpaced, time stands still while a thread polls the pad in a loop (the pause menu waits for A to be let
         * go: replay_screen.c func_8032CF28): after 200 polls in one VI, let go of everything */
        static unsigned lastVi SAVESTATE_EXCLUDE, polls SAVESTATE_EXCLUDE;
        if (vi != lastVi) { lastVi = vi; polls = 0; }
        if (++polls > 200)
            return;
    }
    if (sState != ST_UPDATE || sAt < 0) {
        /* menus, class complete, credits: START and A in turn (START only at the title). The results' menus
         * (Check photo / Replay / Next; Retry / Another test / Quit): down, then A -- except an entry with i%3 == 1
         * takes the first item once (a replay, or a retry) */
        /* the pulses' phase drifts (17 VIs every 160) so they do not lock to a menu's own rhythm: the results'
         * "Replay" loop, where a fixed 'down' always fell inside the replay and the next 'A' chose Replay again */
        const unsigned t = vi + (vi / 160) * 17, ph = t % 40;
        if (sState == ST_RESULTS || sState == ST_RESULTS_CB) {
            /* the results' menus (Check photo / Replay / Next; Retry / Another test / Quit): steer the cursor to the
             * last item (Next, Quit) and take it -- or, for an entry with i%3 == 1, to the first item once (a photo
             * check, a retry). Every 4th pulse is A whatever the cursor (the tallies, a replay: no menu up), every
             * 6th B (out of the photo grid). */
            extern void hh_soak_menu_state(int *cur, int *count);
            int cur, count, target;
            const unsigned pulse = t / 20;
            hh_soak_menu_state(&cur, &count);
            target = (sAt >= 0 && sAt % 3 == 1 && sMenuA == 0) ? 0 : count - 1;
            if (t % 20 < 4) {
                if (pulse % 6 == 5)
                    strcpy(keys, "b");
                else if (pulse % 4 == 3 || cur == target || count <= 0)
                    strcpy(keys, "a");
                else
                    strcpy(keys, cur < target ? "d" : "u");
                if (keys[0] == 'a' && pulse != sMenuVi) {
                    sMenuVi = pulse;
                    sMenuA++;
                }
            }
            return;
        }
        if (sState == ST_OPTIONS) { /* Photo Album, File 1, then a few pictures, then back out */
            static const char *const seq[] = { "a", "a", "a", "r", "r", "a", "l", "b", "b", "b", "b" };
            if (ph >= 20 && ph < 24) {
                strcpy(keys, seq[sMenuA < 11 ? sMenuA : 10]);
                if (ph == 23 && t / 40 != sMenuVi) {
                    sMenuVi = t / 40;
                    sMenuA++;
                }
            }
            return;
        }
        if (ph < 4)
            strcpy(keys, sState == ST_TITLE && (t / 40) % 2 ? "s" : "a");
        return;
    }
    beh = sAt % 4;
    if (sList[sAt].veh == 3 && sFrame - sEntryFrame >= sTestFrames)
        beh = 2; /* the cannonball's results go straight on to the next shot: when time is up, quit from its pause menu */
    else if (sList[sAt].veh == 3) { /* cannonball: aim a little, then fire */
        if (vi % 90 < 30) strcpy(keys, (vi / 90) % 2 ? "l" : "u");
        else if (vi % 90 >= 60 && vi % 90 < 64) strcpy(keys, "a");
        return;
    }
    if (sList[sAt].veh == 3 && sPauseDone) { /* no quit taken (a shot in flight): try again */
        sPauseDone = 0;
        sPauseVi = 0;
    }
    if ((beh == 2 && sFlyFrame >= 300) || (beh == 3 && sFlyFrame >= 200)) {
        /* pause: START, then (2) down, down, A = quit, or (3) down, A = the second item */
        static const char *const quit[] = { "s", "", "d", "", "d", "", "a", "" };
        static const char *const second[] = { "s", "", "d", "", "a", "", "", "" };
        const char *const *seq = beh == 2 ? quit : second;
        unsigned step;
        if (!sPauseDone) {
            if (sPauseVi == 0)
                sPauseVi = vi;
            step = (vi - sPauseVi) / 12;
            if (step < 8) {
                strcpy(keys, seq[step]);
                return;
            }
            sPauseDone = 1;
        }
    }
    if (beh == 0)
        return; /* hands off */
    {   /* fly: thrust (A) most of the time, the stick round a slow pattern, B now and then */
        static const char *const pat[] = { "u", "ur", "r", "dr", "d", "dl", "l", "ul", "", "a" };
        const char *p = pat[(vi / 45) % 10];
        char *k = keys;
        if (vi % 120 < 90) *k++ = 'a';
        while (*p) *k++ = *p++;
        if (vi % 300 < 10) *k++ = 'b';
        if (vi % 240 >= 100 && vi % 240 < 106) *k++ = 'm'; /* Z: a photo in the photo tests */
        *k = 0;
    }
}
