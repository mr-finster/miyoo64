/**
 * SDL keyboard -> N64 pad. ultra_shim.c's libultra controller shim calls
 * controller_psp_init/read once per frame.
 *
 * Desktop keys: arrows = stick, X/Space = A, Z = B, Enter = Start,
 * LShift = Z-trigger, Q/W = L/R, I/J/K/L = C-up/left/down/right.
 *
 * Headless autopilot (DKR_AUTO=1): taps START and A on a cadence so the game
 * advances title -> menus -> race without a keyboard. Deterministic enough for
 * smoke-driving screenshots; real play uses the keyboard.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ultra64.h>

#if defined(TARGET_PC)
#include <SDL2/SDL_scancode.h>
#elif defined(MIYOO)
#include <SDL/SDL_keysym.h>
#endif

#include "savestate.h"
/* live hardware state, not game state: a load must not restore keys held at save time */
static bool sKeys[512] SAVESTATE_EXCLUDE;
static int sAuto = -1;      /* DKR_AUTO: -1 unread, 0 off, 1 on, 2 on without pausing races */
static unsigned sFrame;

void controller_sdl_key(int scancode, bool down) {
    if (scancode >= 0 && scancode < 512) sKeys[scancode] = down;
}

/* Handheld: the buttons arrive as keyboard SDLK syms (miyoo_video.h). Store by sym; MENU is
 * the overlay's, handled in gfx_sdl12.c before it reaches here. */
void controller_handheld_key(int sym, int down) {
    if (sym >= 0 && sym < 512) sKeys[sym] = (bool)down;
}


static int getenv_ramp(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("DKR_RAMP"); s = e && e[0] == '1'; }
    return s;
}
void port_input_read(OSContPad *pad) {
    if (!pad) return;
    memset(pad, 0, sizeof(*pad));
    u16 b = 0;
    s8 sx = 0, sy = 0;

    /* Digital directions (d-pad or arrow keys), ramped to a stick below. */
    int dl = 0, dr = 0, du = 0, dd = 0;
#if defined(MIYOO)
    /* Miyoo Mini (the device's SDL keysyms, see miyoo_video.h): the handheld layout, with the
     * Miyoo's extra buttons. A accelerate, B brake, R1 or X hop / powerslide / plane tricks
     * (N64 R), L1 or Y fire item (N64 Z), START pause, L2 = N64 L, R2 or SELECT = C-up (the
     * camera). MENU
     * (ESCAPE) opens the menu in gfx_sdl12.c and never reaches here. */
    if (sKeys[SDLK_SPACE])  b |= CONT_A;
    if (sKeys[SDLK_LCTRL])  b |= CONT_B;
    if (sKeys[SDLK_RETURN]) b |= CONT_START;
    if (sKeys[SDLK_t] || sKeys[SDLK_LSHIFT]) b |= CONT_R;
    if (sKeys[SDLK_e] || sKeys[SDLK_LALT])   b |= CONT_G;
    if (sKeys[SDLK_TAB]) b |= CONT_L;
    if (sKeys[SDLK_BACKSPACE] || sKeys[SDLK_RCTRL]) b |= CONT_E;
    du = sKeys[SDLK_UP]; dd = sKeys[SDLK_DOWN]; dl = sKeys[SDLK_LEFT]; dr = sKeys[SDLK_RIGHT];
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
    du = sKeys[SDL_SCANCODE_UP]; dd = sKeys[SDL_SCANCODE_DOWN];
    dl = sKeys[SDL_SCANCODE_LEFT]; dr = sKeys[SDL_SCANCODE_RIGHT];
#endif
    {
        /* Digital -> analog. Default: full tilt at once, as in the MK64 port.
         * DKR_RAMP=1 restores the ramp below for A/B. Racing with the ramp: the stick ramps to full over ~120 ms for
         * steering (X) and ~200 ms for plane pitch (Y); release snaps to
         * centre and reversing restarts from centre, so a tap is a small
         * correction and a hold is full lock. Menus read thresholds, so
         * outside a race the stick is full at once. The N64 raw range used
         * here is +-80 (the game clamps to 70 past an 8 deadzone). */
        extern s32 gGameMode; /* thread3_main.c: 2 = GAMEMODE_INGAME */
        extern u64 port_clock_us(void);
        static float sStickX, sStickY;
        static u64 sLastUs;
        const u64 now = port_clock_us();
        float dt = sLastUs ? (float) (now - sLastUs) / 1000.0f : 0.0f;
        const int wantX = dr - dl, wantY = du - dd;
        sLastUs = now;
        if (dt > 100.0f) dt = 100.0f;
        if (gGameMode != 2 || !getenv_ramp()) {
            sStickX = 80.0f * wantX; sStickY = 80.0f * wantY;
        } else {
            if (wantX == 0 || (wantX > 0) != (sStickX > 0)) sStickX = 0.0f;
            if (wantY == 0 || (wantY > 0) != (sStickY > 0)) sStickY = 0.0f;
            sStickX += wantX * 80.0f * dt / 120.0f;
            sStickY += wantY * 80.0f * dt / 200.0f;
            if (sStickX > 80.0f) sStickX = 80.0f;
            if (sStickX < -80.0f) sStickX = -80.0f;
            if (sStickY > 80.0f) sStickY = 80.0f;
            if (sStickY < -80.0f) sStickY = -80.0f;
            /* never below the game's deadzone while held */
            if (wantX && sStickX * wantX < 10.0f) sStickX = 10.0f * wantX;
            if (wantY && sStickY * wantY < 10.0f) sStickY = 10.0f * wantY;
        }
        sx = (s8) sStickX;
        sy = (s8) sStickY;
    }

    /* DKR_INPUT="<frame>:<button>[+<button>...],..." (headless tests): each
     * entry presses its buttons for 4 frames from <frame>. Buttons: A B Z L R
     * START UP DOWN LEFT RIGHT (stick) CU CD CL CR. Overrides DKR_AUTO. */
    {
        static char sScript[1024]; static int sScriptInit;
        if (!sScriptInit) { const char *e = getenv("DKR_INPUT"); if (e) snprintf(sScript, sizeof(sScript), "%s", e); sScriptInit = 1; }
        if (sScript[0]) {
            const char *p = sScript;
            while (*p) {
                long f = strtol(p, (char **) &p, 10);
                if (*p == ':') p++;
                while (*p && *p != ',') {
                    char name[8]; int n = 0;
                    while (*p && *p != ',' && *p != '+' && n < 7) name[n++] = *p++;
                    name[n] = 0;
                    if (*p == '+') p++;
                    if (sFrame >= (unsigned) f && sFrame < (unsigned) f + 4) {
                        if (!strcmp(name, "A")) b |= CONT_A;
                        else if (!strcmp(name, "B")) b |= CONT_B;
                        else if (!strcmp(name, "Z")) b |= CONT_G;
                        else if (!strcmp(name, "L")) b |= CONT_L;
                        else if (!strcmp(name, "R")) b |= CONT_R;
                        else if (!strcmp(name, "START")) b |= CONT_START;
                        else if (!strcmp(name, "CU")) b |= CONT_E;
                        else if (!strcmp(name, "CD")) b |= CONT_D;
                        else if (!strcmp(name, "CL")) b |= CONT_C;
                        else if (!strcmp(name, "CR")) b |= CONT_F;
                        else if (!strcmp(name, "UP")) sy = 80;
                        else if (!strcmp(name, "DOWN")) sy = -80;
                        else if (!strcmp(name, "LEFT")) sx = -80;
                        else if (!strcmp(name, "RIGHT")) sx = 80;
                        else if (!strcmp(name, "HOLDA")) b |= CONT_A;
                    }
                    /* HOLDA: hold A from this frame on (accelerate in a race) */
                    if (!strcmp(name, "HOLDA") && sFrame >= (unsigned) f) b |= CONT_A;
                }
                if (*p == ',') p++;
            }
            sFrame++;
            pad->button = b; pad->stick_x = sx; pad->stick_y = sy;
            return;
        }
    }
    /* DKR_AUTO=race: a menu-aware autopilot that goes Title -> Tracks mode ->
     * the first track -> race, then holds A. It reads the game's current menu
     * (menu.c gCurrentMenuId: 0 title, 19 game select, 15 the track grid) and
     * mode (2 = racing). DKR_AUTO=track:<row>,<column> (developer, round 18's
     * z64 sweep) picks that cell of Tracks mode's grid instead (stick down
     * <row> times, right <column> times, from the first cell) and leaves the
     * kart idle on the grid, as the real game's scripted runs do. */
    {
        static int sRace = -1, sRow, sCol, sIdle;
        if (sRace < 0) {
            const char *e = getenv("DKR_AUTO"); extern int port_bench_active(void);
            sRace = (e && !strcmp(e, "race")) || port_bench_active();
            if (e && !strncmp(e, "track:", 6)) { sRace = 1; sIdle = 1; sscanf(e + 6, "%d,%d", &sRow, &sCol); }
        }
        if (sRace) {
            extern s32 gCurrentMenuId, gGameMode;
            static s32 sMenu = -1; static unsigned sOnMenu, sStep;
            if (gCurrentMenuId != sMenu) { sMenu = gCurrentMenuId; sOnMenu = 0; sStep = 0; }
            sOnMenu++;
            extern int port_bench_active(void), port_bench_autodrive(void);
            if (port_bench_active() && !port_bench_autodrive()) {
                /* benchmark hub sample: hands off, the camera stays on the open
                 * hub (holding A drove the hovercraft into the nearest wall) */
            } else if (sIdle && gGameMode == 0) {
                /* the grid cell's race (GAMEMODE_INGAME is 0): hands off. The race branch below tests
                 * gGameMode == 2, which is never the race: the A/B sweeps' races have driven on its
                 * menu branch's pulsed A since round 2, so it stays as it is (their recorded input). */
            } else if (sIdle && sMenu == 15) {
                /* the grid, in the real game's scripted rhythm (refs: the z64 runs' keys, one mupen
                 * frame ~ two port frames): a stick push every 57 frames from frame 150, rows
                 * first, then A 57 frames after the last push and every 188 after (vehicle,
                 * T.T., OK?, go) */
                const unsigned nav = (unsigned) (sRow + sCol), t0 = 150u + 57u * nav + 57u;
                if (sOnMenu >= 150u && sOnMenu < 150u + 57u * nav) {
                    const unsigned k = (sOnMenu - 150u) / 57u, t = (sOnMenu - 150u) % 57u;
                    if (t < 8) { if (k < (unsigned) sRow) sy = -80; else sx = 80; }
                } else if (sOnMenu >= t0 && (sOnMenu - t0) % 188u < 8u) {
                    b |= CONT_A;
                }
            } else if (gGameMode == 2 || port_bench_active()) {
                b |= CONT_A; /* never START during a benchmark: no pause menu */
            } else if (sMenu == 19) { /* game select: TRACKS is the second entry */
                if (sOnMenu >= 40 && sOnMenu < 44) sy = -80;
                else if (sOnMenu >= 70 && sOnMenu < 74) b |= CONT_A;
            } else if ((sOnMenu % 40) >= 30 && (sOnMenu % 40) < 34) {
                b |= sMenu == 0 ? CONT_START : CONT_A;
            }
            sFrame++;
            pad->button = b; pad->stick_x = sx; pad->stick_y = sy;
            return;
        }
    }
    if (sAuto < 0) { const char *e = getenv("DKR_AUTO"); sAuto = (e && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0; }
    if (sAuto) {
        /* Tap START, then A, ~once a second each; hold A while "racing" so the
         * kart accelerates if a race is reached. */
        /* DKR_AUTO=2: the same, minus START while racing (no pause menu over
         * test dumps). 1 keeps the exact input the recorded baselines use. */
        extern s32 gGameMode; /* thread3_main.c */
#define gGamestate gGameMode
        unsigned t = sFrame % 120;
        if (t < 4 && !(sAuto == 2 && gGamestate == 2 /* GAMEMODE_INGAME */)) b |= CONT_START;
        else if (t >= 60 && t < 64) b |= CONT_A;
        if (sFrame > 1800) b |= CONT_A; /* after ~60 s of menus, just hold A */
    }
    sFrame++;

    pad->button = b;
    pad->stick_x = sx;
    pad->stick_y = sy;
}

/* Some callers use the generic name. */
void port_input_poll(void) { }
