/**
 * SDL keyboard -> N64 pad. ultra_shim.c's libultra controller shim calls
 * controller_psp_init/read once per frame.
 *
 * Desktop keys: arrows = stick, X/Space = A, Z = B, Enter = Start,
 * LShift = Z-trigger, Q/W = L/R, I/J/K/L = C-up/left/down/right.
 *
 * Headless autopilot (MK64_AUTO=1): taps START and A on a cadence so the game
 * advances title -> menus -> race without a keyboard. Deterministic enough for
 * smoke-driving screenshots; real play uses the keyboard.
 */
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ultra64.h>

#if defined(TARGET_PC)
#include <SDL2/SDL_scancode.h>
#elif defined(MIYOO)
#include <SDL/SDL_keysym.h>
#endif

#include "savestate.h"
#if defined(MIYOO)
#include "miyoo_remap.h"
#endif
/* live hardware state, not game state: a load must not restore keys held at save time */
static bool sKeys[512] SAVESTATE_EXCLUDE;
static int sAuto = -1;      /* MK64_AUTO: -1 unread, 0 off, 1 on, 2 on without pausing races */
static unsigned sFrame;

void controller_sdl_key(int scancode, bool down) {
    if (scancode >= 0 && scancode < 512) sKeys[scancode] = down;
}

/* Handheld: the buttons arrive as keyboard SDLK syms (miyoo_video.h). Store by sym; MENU is
 * the overlay's, handled in gfx_sdl12.c before it reaches here. */
#if defined(MIYOO)
/* MENU > CONTROLS (kit miyoo_remap.c): the layout below, in play, as the screen shows it; a remap moves the
 * pressed buttons, so every mode and chord here follows. */
const struct miyoo_remap_row miyoo_remap_rows[] = {
    { MR_A, MB_A, 0, 0, "ACCELERATE" },
    { MR_B, MB_B, 0, 0, "BRAKE" },
    { MR_Z, MB_L1, MB_Y, 0, "USE ITEM" },
    { MR_R, MB_R1, MB_X, 0, "HOP, DRIFT" },
    { MR_START, MB_START, 0, 0, "PAUSE" },
    { MR_L, MB_L2, 0, 0, NULL },
    { MR_CRIGHT, MB_R2, MB_SELECT, 0, "THE HUD" },
    { MR_CUP, MB_NONE, 0, 0, NULL },
    { MR_CDOWN, MB_NONE, 0, 0, NULL },
    { MR_CLEFT, MB_NONE, 0, 0, NULL },
    { MR_STICK, MB_DPAD, 0, 0, "STEER" },
    { MR_DPAD, MB_NONE, 0, 0, NULL },
};
const int miyoo_remap_nrows = (int)(sizeof(miyoo_remap_rows) / sizeof(miyoo_remap_rows[0]));
#endif

void controller_handheld_key(int sym, int down) {
#if defined(MIYOO)
    sym = miyoo_remap_key(sym); /* MENU > CONTROLS */
#endif
    if (sym >= 0 && sym < 512) sKeys[sym] = (bool)down;
}

void controller_psp_init(void) { }

void controller_psp_read(OSContPad *pad) {
    if (!pad) return;
    memset(pad, 0, sizeof(*pad));
    u16 b = 0;
    s8 sx = 0, sy = 0;

#if defined(MIYOO)
    /* Miyoo Mini (the device's SDL keysyms, see miyoo_video.h): the handheld layout, with the
     * Miyoo's extra buttons. A accel, B brake, R1/X hop-drift, L1/Y item (Z), L2 = N64 L,
     * R2 or SELECT = C-right, the HUD switch. MENU (ESCAPE) opens
     * the menu in gfx_sdl12.c and never reaches here. */
    if (sKeys[SDLK_SPACE])  b |= CONT_A;
    if (sKeys[SDLK_LCTRL])  b |= CONT_B;
    if (sKeys[SDLK_RETURN]) b |= CONT_START;
    if (sKeys[SDLK_t] || sKeys[SDLK_LSHIFT]) b |= CONT_R;
    if (sKeys[SDLK_e] || sKeys[SDLK_LALT])   b |= CONT_G;
    if (sKeys[SDLK_TAB]) b |= CONT_L;
    if (sKeys[SDLK_BACKSPACE] || sKeys[SDLK_RCTRL]) b |= CONT_F;
    if (sKeys[SDLK_UP])    sy =  80;
    if (sKeys[SDLK_DOWN])  sy = -80;
    if (sKeys[SDLK_LEFT])  sx = -80;
    if (sKeys[SDLK_RIGHT]) sx =  80;
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
    if (sKeys[SDL_SCANCODE_UP])    sy =  80;
    if (sKeys[SDL_SCANCODE_DOWN])  sy = -80;
    if (sKeys[SDL_SCANCODE_LEFT])  sx = -80;
    if (sKeys[SDL_SCANCODE_RIGHT]) sx =  80;
#endif

    if (sAuto < 0) { const char *e = getenv("MK64_AUTO"); sAuto = (e && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0; }
    if (sAuto) {
        /* Tap START, then A, ~once a second each; hold A while "racing" so the
         * kart accelerates if a race is reached. */
        /* MK64_AUTO=2: the same, minus START while racing (no pause menu over
         * test dumps). 1 keeps the exact input the recorded baselines use. */
        extern s32 gGamestate;
        unsigned t = sFrame % 120;
        if (t < 4 && !(sAuto == 2 && gGamestate == 4 /* RACING */)) b |= CONT_START;
        else if (t >= 60 && t < 64) b |= CONT_A;
        if (sFrame > 1800) b |= CONT_A; /* after ~60 s of menus, just hold A */
        { /* MK64_AUTO_HOLDA=1 (save-state tests): while racing, A held and nothing else, so the input
           * never depends on the process's frame count -- a state loaded at any frame, in-process or
           * by a fresh process, plays on exactly as the run that saved it. */
            static int sHold SAVESTATE_EXCLUDE = -1;
            extern s32 gGamestate;
            if (sHold < 0) { const char *e = getenv("MK64_AUTO_HOLDA"); sHold = e && e[0] == '1'; }
            if (sHold && gGamestate == 4 /* RACING */) b = CONT_A;
        }
        { /* MK64_AUTO_DOWN_AT=<frame>: hold down for 4 frames there, e.g. 250 =
           * T.TRIALS instead of MARIO GP on the game select (desktop tests). */
            static long sDownAt = -2;
            if (sDownAt == -2) { const char *e = getenv("MK64_AUTO_DOWN_AT"); sDownAt = e ? atol(e) : -1; }
            if (sDownAt >= 0 && (long) sFrame >= sDownAt && (long) sFrame < sDownAt + 4) { b |= CONT_DOWN; sy = -80; }
        }
        { /* MK64_AUTO_CRIGHT_AT=<frame>[,<frame>...] (up to 8): one frame of
           * C-right at each -- the HUD switch. */
            static long sAt[8]; static int sN = -1;
            if (sN < 0) { const char *e = getenv("MK64_AUTO_CRIGHT_AT"); sN = 0;
                while (e && *e && sN < 8) { sAt[sN++] = strtol(e, (char **)&e, 10); if (*e == ',') e++; else break; } }
            for (int i = 0; i < sN; i++) if ((long) sFrame == sAt[i]) b |= CONT_F;
        }
    }
    sFrame++;

    pad->button = b;
    pad->stick_x = sx;
    pad->stick_y = sy;
}

/* Some callers use the generic name. */
void port_input_poll(void) { }
