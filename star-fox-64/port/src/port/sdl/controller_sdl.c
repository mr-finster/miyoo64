/**
 * Keys -> N64 pad for Star Fox 64 (layout: notes/controls.md). ultra_shim.c's
 * libultra controller shim calls controller_psp_init/read once per game frame.
 *
 * Miyoo Mini (MIYOO, the device's SDL keysyms, miyoo_panel.h): d-pad stick, A fire, B bomb, L1 Z, R1 R, X C-left (boost), Y C-down (brake), START,
 * SELECT C-right (answer ROB, radio branches; held while held), L2 C-up (the camera), R2 L (menus only).
 * MENU (Escape) opens the port menu in gfx_sdl12.c and never reaches here.
 *
 * Desktop: arrows = stick, X/Space = A, Z = B, LShift = Z, W = R, Q = L,
 * J = C-left, K = C-down, L = C-right, I = C-up, Enter = START.
 *
 * D-pad to stick: the game applies a +-16 deadzone and clamps to +-60 after it
 * (Controller_AddDeadZone). In play, a held direction ramps from just past the
 * deadzone to full deflection over RAMP_FRAMES game frames so a tap makes a
 * small correction; outside play (menus, map) it is full deflection at once.
 * SF64_INVERT=1 flips the vertical axis (the port menu will own this setting).
 *
 * Headless autopilot (SF64_AUTO=1): taps START and A on a cadence so the game
 * walks title -> menus -> a level without a keyboard (desktop smoke runs).
 *
 * Replay (SF64_KEYS, tests): plays a key script in m64ref's M64REF_KEYS format,
 * "<poll>:<key>+|-,..." (miyoo64 tools/m64ref.sh), so a run here and the
 * reference emulator get the same presses: <poll> counts the N64's controller
 * reads, SF64_KEYS_RATE of them per read here (default 2: the N64 reads once per
 * VI, twice per 30 fps game frame), starting SF64_KEYS_OFFSET polls in.
 */
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ultra64.h>

#if defined(TARGET_PC)
#include <SDL2/SDL_scancode.h>
#elif defined(MIYOO)
#include "miyoo_panel.h" /* MIYOO_KEY_*: the Mini's SDL keysyms */
#endif

#include "savestate.h"
/* live hardware state, not game state: a load must not restore keys held at save time */
static bool sKeys[512] SAVESTATE_EXCLUDE;
static int sAuto SAVESTATE_EXCLUDE = -1; /* SF64_AUTO: -1 unread, 0 off, 1 on */
static int sInvert SAVESTATE_EXCLUDE = -1;
static unsigned sFrame SAVESTATE_EXCLUDE;
static int sAutoBomb SAVESTATE_EXCLUDE = -1, sAutoBombDet SAVESTATE_EXCLUDE = -1; /* SF64_AUTOBOMB, SF64_AUTOBOMB_DET (-1: unread) */
/* The Bench (main.c, 2026-10-11): the autopilot's poll count goes back with each load of its segment state, so
 * every window replays the same presses; it turns the smart bombs on for its bomb phase. */
unsigned port_auto_polls(void) { return sFrame; }
void port_auto_polls_set(unsigned n) { sFrame = n; }
static unsigned sAutoBombPhase SAVESTATE_EXCLUDE; /* the poll the cadence counts from */
void port_auto_bomb_set(int every, int det, unsigned phase) { sAutoBomb = every < 0 ? 0 : every; sAutoBombDet = det < 0 ? 0 : det; sAutoBombPhase = phase; }

/* SF64_KEYS replay (see the header). Keys in m64ref_input.c's order. */
static const char *const sReplayNames[18] = { "a", "b", "z", "start", "l", "r", "cu", "cd", "cl", "cr",
                                              "du", "dd", "dl", "dr", "up", "down", "left", "right" };
static const char *sReplay SAVESTATE_EXCLUDE;   /* the script, NULL: off */
static int sReplayOn SAVESTATE_EXCLUDE = -1;
static unsigned long sReplayPoll SAVESTATE_EXCLUDE; /* polls applied so far */
static long sReplayRate SAVESTATE_EXCLUDE = 2, sReplayOffset SAVESTATE_EXCLUDE;
static bool sReplayHeld[18] SAVESTATE_EXCLUDE;

static void replay_apply(unsigned long poll) {
    const char *p;
    for (p = sReplay; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
        char *e;
        unsigned long at = strtoul(p, &e, 10);
        int k;
        if (at != poll || *e != ':') continue;
        for (k = 0; k < 18; k++) {
            size_t n = strlen(sReplayNames[k]);
            if (!strncmp(e + 1, sReplayNames[k], n) && (e[1 + n] == '+' || e[1 + n] == '-')) {
                sReplayHeld[k] = e[1 + n] == '+';
            }
        }
    }
}

/* One read: advance the N64 poll clock by the rate, then the pad as m64ref_input builds it. */
static bool replay_read(OSContPad *pad) {
    static const u16 bits[14] = { CONT_A, CONT_B, CONT_G, CONT_START, CONT_L, CONT_R, CONT_E,
                                  CONT_D, CONT_C, CONT_F, CONT_UP, CONT_DOWN, CONT_LEFT, CONT_RIGHT };
    unsigned long target;
    int k;
    u16 b = 0;
    if (sReplayOn < 0) {
        const char *r = getenv("SF64_KEYS_RATE"), *o = getenv("SF64_KEYS_OFFSET");
        sReplay = getenv("SF64_KEYS");
        sReplayOn = sReplay != NULL && sReplay[0] != 0;
        if (r) sReplayRate = strcmp(r, "vi") == 0 ? 0 : atol(r);
        if (o) sReplayOffset = atol(o);
    }
    if (!sReplayOn) return false;
    if (sReplayRate == 0) {
        /* SF64_KEYS_RATE=vi (tests): the N64 clock of mupen64plus's m64ref input plugin, two polls a
         * retrace, whatever the game's frame rate: 2 x gVIsPerFrame polls a read (the menus run at
         * one retrace a frame, play at two), so menu presses and in-play presses both land where a
         * reference run put them. */
        extern u8 gVIsPerFrame;
        static long sClock SAVESTATE_EXCLUDE;
        sClock += 2 * (gVIsPerFrame ? gVIsPerFrame : 1);
        const long t = sClock + sReplayOffset;
        target = t > 0 ? (unsigned long) t : 0;
    } else {
        const long t = (long) sFrame * sReplayRate + sReplayOffset; /* sFrame: reads so far, this one included */
        target = t > 0 ? (unsigned long) t : 0;                  /* a negative offset starts the script later */
    }
    while (sReplayPoll < target) replay_apply(++sReplayPoll);
    for (k = 0; k < 14; k++) {
        if (sReplayHeld[k]) b |= bits[k];
    }
    pad->button = b;
    pad->stick_y = (s8) (sReplayHeld[14] ? 80 : sReplayHeld[15] ? -80 : 0);
    pad->stick_x = (s8) (sReplayHeld[16] ? -80 : sReplayHeld[17] ? 80 : 0);
    return true;
}

#define STICK_MIN 17    /* first raw value past the game's +-16 deadzone */
#define STICK_MAX 76    /* raw value the game clamps to its +-60 maximum */
#define RAMP_FRAMES 6   /* ~200 ms at 30 Hz from STICK_MIN to STICK_MAX */

void controller_sdl_key(int scancode, bool down) {
    if (scancode >= 0 && scancode < 512) sKeys[scancode] = down;
}

void controller_handheld_key(int sym, int down) {
    if (sym >= 0 && sym < 512) sKeys[sym] = (bool) down;
}

void controller_psp_init(void) { }

/* In a level (the only state where fine aim matters): ramp; elsewhere instant. */
static int in_play(void) {
    extern s32 gGameState;
    return gGameState == 7; /* GSTATE_PLAY */
}

/* One axis: -1/0/+1 held -> raw stick value, ramping while held in play. */
static s8 axis(int dir, int *held) {
    if (dir == 0) { *held = 0; return 0; }
    if (*held * dir < 0) *held = 0; /* reversed: restart the ramp */
    if (*held * dir < RAMP_FRAMES) *held += dir;
    int mag = in_play() ? STICK_MIN + (STICK_MAX - STICK_MIN) * (*held * dir) / RAMP_FRAMES : STICK_MAX;
    return (s8) (dir * mag);
}

void controller_psp_read(OSContPad *pad) {
    static int sHeldX SAVESTATE_EXCLUDE, sHeldY SAVESTATE_EXCLUDE;
    if (!pad) return;
    memset(pad, 0, sizeof(*pad));
    u16 b = 0;
    int dx = 0, dy = 0;

    if (sInvert < 0) { const char *e = getenv("SF64_INVERT"); sInvert = e && e[0] == '1'; }
#if defined(MIYOO)
    if (sKeys[MIYOO_KEY_A])      b |= CONT_A;
    if (sKeys[MIYOO_KEY_B])      b |= CONT_B;
    if (sKeys[MIYOO_KEY_START])  b |= CONT_START;
    if (sKeys[MIYOO_KEY_L1])     b |= CONT_G;   /* Z: bank left, twice = roll */
    if (sKeys[MIYOO_KEY_R1])     b |= CONT_R;
    if (sKeys[MIYOO_KEY_X])      b |= CONT_C;   /* C-left: boost */
    if (sKeys[MIYOO_KEY_Y])      b |= CONT_D;   /* C-down: brake */
    if (sKeys[MIYOO_KEY_SELECT]) b |= CONT_F;   /* C-right: answer ROB / radio branches */
    if (sKeys[MIYOO_KEY_L2])     b |= CONT_E;   /* C-up: the camera */
    if (sKeys[MIYOO_KEY_R2])     b |= CONT_L;   /* L: menus only */
    if (sKeys[MIYOO_KEY_UP])    dy += 1;
    if (sKeys[MIYOO_KEY_DOWN])  dy -= 1;
    if (sKeys[MIYOO_KEY_LEFT])  dx -= 1;
    if (sKeys[MIYOO_KEY_RIGHT]) dx += 1;
#elif defined(TARGET_PC)
    if (sKeys[SDL_SCANCODE_X] || sKeys[SDL_SCANCODE_SPACE]) b |= CONT_A;
    if (sKeys[SDL_SCANCODE_Z])      b |= CONT_B;
    if (sKeys[SDL_SCANCODE_RETURN]) b |= CONT_START;
    if (sKeys[SDL_SCANCODE_LSHIFT]) b |= CONT_G;   /* Z trigger */
    if (sKeys[SDL_SCANCODE_Q])      b |= CONT_L;
    if (sKeys[SDL_SCANCODE_W])      b |= CONT_R;
    if (sKeys[SDL_SCANCODE_I])      b |= CONT_E;   /* C-up */
    if (sKeys[SDL_SCANCODE_K])      b |= CONT_D;   /* C-down */
    if (sKeys[SDL_SCANCODE_J])      b |= CONT_C;   /* C-left */
    if (sKeys[SDL_SCANCODE_L])      b |= CONT_F;   /* C-right */
    if (sKeys[SDL_SCANCODE_UP])    dy += 1;
    if (sKeys[SDL_SCANCODE_DOWN])  dy -= 1;
    if (sKeys[SDL_SCANCODE_LEFT])  dx -= 1;
    if (sKeys[SDL_SCANCODE_RIGHT]) dx += 1;
#endif
    if (sAuto < 0) { const char *e = getenv("SF64_AUTO"); sAuto = (e && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0; }
    if (sAuto == 2 && in_play()) {
        /* SF64_AUTO=2: in a level, fire (tap A) and never pause -- explosions */
        if ((sFrame & 3) < 2) b |= CONT_A;
        { /* SF64_AUTOBOMB=n (tests): a smart bomb every n polls in play (round 22: "shooting a bomb lags the game") */
            int bomb = sAutoBomb; if (bomb < 0) { const char *e = getenv("SF64_AUTOBOMB"); bomb = sAutoBomb = e ? atoi(e) : 0; if (bomb < 0) bomb = sAutoBomb = 0; }
            const unsigned bf = sFrame - sAutoBombPhase; /* (0 unless the Bench set a phase) */
            if (bomb > 0 && sFrame >= sAutoBombPhase && (bf % (unsigned)bomb) < 2) b |= CONT_B;
            /* SF64_AUTOBOMB_DET=k (tests, round 27): B again k reads after each bomb -- detonated close, as a
             * player does, so the blast wraps the camera */
            int det = sAutoBombDet; if (det < 0) { const char *e = getenv("SF64_AUTOBOMB_DET"); det = sAutoBombDet = e ? atoi(e) : 0; }
            if (bomb > 0 && det > 0 && sFrame >= sAutoBombPhase + (unsigned)det && ((bf + (unsigned)bomb - (unsigned)det) % (unsigned)bomb) < 2) b |= CONT_B;
        }
        { extern void port_test_radio_damage(void); port_test_radio_damage(); }
        { /* SF64_AUTOPAUSE=n (tests): tap START at poll n and stay paused (the pause screen's portraits, round 22) */
            static long pause = -2; if (pause == -2) { const char *e = getenv("SF64_AUTOPAUSE"); pause = e ? atol(e) : -1; }
            if (pause >= 0 && sFrame >= (unsigned)pause && sFrame < (unsigned)pause + 4) {
                b |= CONT_START;
                if (sFrame == (unsigned)pause) { extern void port_log_always(const char *fmt, ...); port_log_always("test: SF64_AUTOPAUSE: START at poll %u\n", sFrame); }
            }
            if (pause >= 0 && sFrame >= (unsigned)pause) b &= ~(u16)(CONT_A | CONT_B);
        }
        { /* SF64_GOD=1 (tests): shields kept full, so the autopilot lives to reach the boss (round 19: the
           * boss's death explosion, IMG_0184) */
            static int god = -1; if (god < 0) { const char *e = getenv("SF64_GOD"); god = e && e[0] == '1'; }
            if (god) { extern void port_test_refill_shields(void); port_test_refill_shields(); }
        }
    } else if (sAuto) {
        unsigned t = sFrame % 90;
        if (t < 4) b |= CONT_START;
        else if (t >= 45 && t < 49) b |= CONT_A;
    }
    sFrame++;

    if (sInvert) dy = -dy;
    pad->button = b;
    pad->stick_x = axis(dx, &sHeldX);
    pad->stick_y = axis(dy, &sHeldY);
    replay_read(pad); /* SF64_KEYS: the script's pad instead */
    { /* SF64_LEVEL=<n> (tests): gCurrentLevel held at n on every read, as mupen64plus's "Level Select" cheat
       * (80178237) holds it on the N64, so the first level started is that one on both (reference shots) */
        static int lvl SAVESTATE_EXCLUDE = -2;
        if (lvl == -2) { const char *e = getenv("SF64_LEVEL"); lvl = e && *e ? atoi(e) : -1; }
        if (lvl >= 0) { extern s32 gCurrentLevel; gCurrentLevel = lvl; }
    }
}

/* Some callers use the generic name. */
void port_input_poll(void) { }
