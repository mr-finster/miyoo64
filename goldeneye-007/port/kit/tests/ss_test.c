/* ss_test.c -- save states on the desktop, both layouts (tests/run.sh builds it
 * twice: the hole layout, and HH_SS_INCLUDE_LIST). Built with HANDHELD defined so
 * the player-facing paths run, against stubs for the port and the menu:
 *  1. a menu SAVE writes the slot, is valid, and is reported as SAVED SLOT n;
 *  2. a LOAD brings game memory back, keeps SAVESTATE_EXCLUDE (hole) or anything
 *     outside the include list live, flushes the texture cache once, and is
 *     reported as LOADED SLOT n;
 *  3. a truncated or foreign state is rejected before any memory is touched, and
 *     reported as LOAD FAILED;
 *  4. a save that can't be written leaves the old state and says SAVE FAILED;
 *  5. EXIT GAME's failed save is shown too, and the quit still happens;
 *  6. the backstop comes back as it was (swap present) or on (enable only). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include "savestate.h"

/* ---- game memory under test ---- */
#if defined(HH_SS_INCLUDE_LIST)
#define GAME_DATA __attribute__((section("game_data"), used))
#define GAME_BSS  __attribute__((section("game_bss"), used))
#else
#define GAME_DATA
#define GAME_BSS
#endif
int gGameData[64] GAME_DATA = { 1, 2, 3 };
int gGameBss[4096] GAME_BSS;
/* Host-side state: kept live across a load (dontsave hole, or outside the list). */
static int sHostLive SAVESTATE_EXCLUDE = 7;

#if defined(HH_SS_INCLUDE_LIST)
/* ld provides __start_/__stop_ for these sections only when something refers to
 * them; run.sh maps the kit's include-list symbols (__gamedata_start and so on)
 * onto them. */
extern char __start_game_data[], __stop_game_data[], __start_game_bss[], __stop_game_bss[];
void *volatile gKeepSections[4] = { __start_game_data, __stop_game_data, __start_game_bss, __stop_game_bss };
#endif

/* ---- port stubs (their bookkeeping is host-side state too) ---- */
static char sDir[256] SAVESTATE_EXCLUDE;
const char *port_save_dir(void) { return sDir; }
const char *port_save_path(const char *name) {
    static char p[512];
    snprintf(p, sizeof(p), "%s/%s", sDir, name);
    return p;
}
static int sExitRequested SAVESTATE_EXCLUDE, sFixups SAVESTATE_EXCLUDE, sBackstop SAVESTATE_EXCLUDE = 1, sLocked SAVESTATE_EXCLUDE;
void port_request_exit(void) { sExitRequested++; }
void gfx_savestate_fixup(void) { sFixups++; }
void port_audio_backstop_enable(int on) { sBackstop = on; }
#if defined(TEST_BACKSTOP_SWAP)
int port_audio_backstop_swap(int on) { const int was = sBackstop; sBackstop = on; return was; }
#endif
void port_audio_mix_lock(void) { sLocked++; }
void port_audio_mix_unlock(void) { sLocked--; }
const uint32_t port_build_stamp = 0x12345678u;

/* ---- menu stubs (HANDHELD is defined: savestate.c calls the real names) ---- */
static char sLastNotif[64] SAVESTATE_EXCLUDE;
void handheld_menu_notify(const char *msg) { snprintf(sLastNotif, sizeof(sLastNotif), "%s", msg); }
void handheld_menu_notify_wait(const char *msg) { snprintf(sLastNotif, sizeof(sLastNotif), "%s", msg); }
int handheld_menu_resume_prompt(void) { return 1; }
int handheld_power_pending(void) { return 0; }

static int sFails SAVESTATE_EXCLUDE;
#define CHECK(c, ...) do { if (!(c)) { sFails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void fill(int base) {
    for (int i = 0; i < 64; i++) gGameData[i] = base + i;
    for (int i = 0; i < 4096; i++) gGameBss[i] = base * 3 + i;
}
static int same(int base) {
    for (int i = 0; i < 64; i++) if (gGameData[i] != base + i) return 0;
    for (int i = 0; i < 4096; i++) if (gGameBss[i] != base * 3 + i) return 0;
    return 1;
}

int main(void) {
    snprintf(sDir, sizeof(sDir), "/tmp/hhkit_ss_%d", (int) getpid());
    mkdir(sDir, 0755);

    /* 1. save */
    fill(100); sHostLive = 7;
    savestate_request_save(1);
    savestate_check();
    CHECK(savestate_slot_exists(1), "slot 1 not written");
    CHECK(savestate_slot_valid(1), "slot 1 not valid");
    CHECK(strcmp(sLastNotif, "SAVED SLOT 1") == 0, "save notif '%s'", sLastNotif);
    CHECK(sLocked == 0, "mix lock left held");
    CHECK(sBackstop == 1, "backstop not back on after the save");

    /* 2. load */
    fill(900); sHostLive = 8; sFixups = 0;
    savestate_request_load(1);
    savestate_check();
    CHECK(same(100), "game memory not restored");
    CHECK(sHostLive == 8, "host-side state clobbered by the load (%d)", sHostLive);
    CHECK(sFixups == 1, "texture fixup ran %d times", sFixups);
    CHECK(strcmp(sLastNotif, "LOADED SLOT 1") == 0, "load notif '%s'", sLastNotif);

    /* 3. truncated state: rejected before memory is touched */
    {
        const char *p = port_save_path("state_01.sav");
        FILE *f = fopen(p, "r+b");
        fseek(f, 0, SEEK_END);
        const long len = ftell(f);
        fclose(f);
        CHECK(truncate(p, len - 4) == 0, "truncate");
        CHECK(!savestate_slot_valid(1), "truncated slot still valid");
        fill(500); sFixups = 0;
        savestate_request_load(1);
        savestate_check();
        CHECK(same(500), "a rejected load touched game memory");
        CHECK(sFixups == 0, "fixup ran on a rejected load");
        CHECK(strcmp(sLastNotif, "LOAD FAILED") == 0, "rejected-load notif '%s'", sLastNotif);
        savestate_request_load(7); /* empty slot */
        savestate_check();
        CHECK(strcmp(sLastNotif, "LOAD FAILED") == 0, "empty-slot notif '%s'", sLastNotif);
    }

    /* 4. a save that can't be written keeps the old state and says so */
    {
        fill(300);
        savestate_request_save(2);
        savestate_check();
        CHECK(savestate_slot_valid(2), "slot 2 not valid");
        char dir[256]; strcpy(dir, sDir);
        snprintf(sDir, sizeof(sDir), "%.200s/missing", dir); /* no such directory: fopen fails */
        fill(301);
        savestate_request_save(2);
        savestate_check();
        CHECK(strcmp(sLastNotif, "SAVE FAILED") == 0, "failed-save notif '%s'", sLastNotif);
        strcpy(sDir, dir);
        CHECK(savestate_slot_valid(2), "old slot 2 lost after a failed save");
        fill(0);
        savestate_request_load(2);
        savestate_check();
        CHECK(same(300), "old slot 2 does not hold the old state");

        /* 5. EXIT GAME: the failed save is shown, and the quit still happens */
        snprintf(sDir, sizeof(sDir), "%.200s/missing", dir);
        sLastNotif[0] = 0; sExitRequested = 0;
        savestate_request_exit_save();
        savestate_check();
        CHECK(strcmp(sLastNotif, "SAVE FAILED") == 0, "exit-save notif '%s'", sLastNotif);
        CHECK(sExitRequested == 1, "exit not requested");
        strcpy(sDir, dir);
        sLastNotif[0] = 0; sExitRequested = 0;
        savestate_request_exit_save();
        savestate_check();
        CHECK(strcmp(sLastNotif, "SAVING") == 0, "a good exit save notified '%s'", sLastNotif); /* k1.7 */
        CHECK(sExitRequested == 1 && savestate_slot_valid(QUICKSAVE_SLOT), "exit save missing");
    }

    /* 6. the backstop comes back as it was */
#if defined(TEST_BACKSTOP_SWAP)
    sBackstop = 0;
    savestate_request_save(3);
    savestate_check();
    CHECK(sBackstop == 0, "an off backstop was turned on by the save");
#endif

    char cmd[300]; snprintf(cmd, sizeof(cmd), "rm -rf %s", sDir); (void) !system(cmd);
    printf("%s: %s\n", TEST_NAME, sFails ? "FAILED" : "ok");
    return sFails ? 1 : 0;
}
