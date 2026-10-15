/* input.c -- see hh_input.h. N64 button bits from PR/os.h. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "hh_input.h"

#define N64_A      0x8000
#define N64_B      0x4000
#define N64_Z      0x2000
#define N64_START  0x1000
#define N64_L      0x0020
#define N64_R      0x0010
#define N64_CUP    0x0008
#define N64_CDOWN  0x0004
#define N64_CLEFT  0x0002
#define N64_CRIGHT 0x0001

#define STICK_FULL 80   /* the game clamps to +-80 (macros.h I_CONTROLLER_RANGE_MAX) */
#define STICK_HALF 40   /* >= 20 tilts/walks, < 50 never runs or smashes */

enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_X, K_Y, K_L, K_R, K_START, K_SELECT,
       K_CUP, K_CDOWN, K_CLEFT, K_CRIGHT, K_Z,
#if defined(MIYOO)
       K_L2, K_R2, /* the Miyoo Mini's extra shoulder buttons */
#endif
       K_COUNT };
static volatile bool sKeys[K_COUNT];
static int sAutoFrame;
extern uint32_t port_time_us(void);

static void set_key(int k, bool down)
{
    if (k >= 0 && k < K_COUNT) sKeys[k] = down;
}

/* SDL2 scancodes (desktop proxy, gfx_sdl.c): arrows, X=A, Z=B, C=jump(C),
 * S=shield(Z), A=R trigger, LShift=half stick, Enter=Start, Q=taunt(L), IJKL=C. */
void controller_sdl_key(int sc, bool down)
{
    int k = -1;
    switch (sc) {
    case 82: k = K_UP; break;
    case 81: k = K_DOWN; break;
    case 80: k = K_LEFT; break;
    case 79: k = K_RIGHT; break;
    case 27: k = K_A; break;        /* X */
    case 29: k = K_B; break;        /* Z */
    case 6:  k = K_X; break;        /* C */
    case 22: k = K_Y; break;        /* S */
    case 4:  k = K_L; break;        /* A */
    case 225: k = K_R; break;       /* LShift */
    case 40: k = K_START; break;    /* Enter */
    case 20: k = K_SELECT; break;   /* Q */
    case 12: k = K_CUP; break;      /* I */
    case 13: k = K_CLEFT; break;    /* J */
    case 14: k = K_CDOWN; break;    /* K */
    case 15: k = K_CRIGHT; break;   /* L */
    default: break;
    }
    set_key(k, down);
}

/* The handheld's keys as the SDL 1.2 backend reports them. */
void controller_handheld_key(int sym, int down)
{
    int k = -1;
    (void) sym;
#if defined(MIYOO)
    /* The Miyoo Mini's SDL keys (kit miyoo_panel.h): L1 = N64 R, R1 = half stick, SELECT = N64 L (taunt), L2 = N64 Z
     * (shield, as Y) and R2 = N64 L (taunt). MENU (Escape) opens the menu in gfx_sdl12.c and never reaches here. */
    switch (sym) {
    case 273: k = K_UP; break;     /* MIYOO_KEY_UP */
    case 274: k = K_DOWN; break;   /* MIYOO_KEY_DOWN */
    case 276: k = K_LEFT; break;   /* MIYOO_KEY_LEFT */
    case 275: k = K_RIGHT; break;  /* MIYOO_KEY_RIGHT */
    case 32:  k = K_A; break;      /* MIYOO_KEY_A */
    case 306: k = K_B; break;      /* MIYOO_KEY_B */
    case 304: k = K_X; break;      /* MIYOO_KEY_X */
    case 308: k = K_Y; break;      /* MIYOO_KEY_Y */
    case 101: k = K_L; break;      /* MIYOO_KEY_L1 */
    case 116: k = K_R; break;      /* MIYOO_KEY_R1 */
    case 9:   k = K_L2; break;     /* MIYOO_KEY_L2 */
    case 8:   k = K_R2; break;     /* MIYOO_KEY_R2 */
    case 305: k = K_SELECT; break; /* MIYOO_KEY_SELECT */
    case 13:  k = K_START; break;  /* MIYOO_KEY_START */
    default: break;
    }
#endif
    set_key(k, down != 0);
}

/* Scheme 1 (notes/controls.md): d-pad = full stick, R held = half stick,
 * A/B as is, X = C (jump), Y = Z (shield), L = R trigger, Select = L (taunt). */
void hh_input_read(HhPad *pad)
{
    { extern void hh_demo_stage_poll(void); hh_demo_stage_poll(); } /* SSB64_DEMOSTAGE (n64/bench.c) */
    int mag = sKeys[K_R] ? STICK_HALF : STICK_FULL;
    int sx = 0, sy = 0;
    uint16_t b = 0;

    if (sKeys[K_UP]) sy += mag;
    if (sKeys[K_DOWN]) sy -= mag;
    if (sKeys[K_LEFT]) sx -= mag;
    if (sKeys[K_RIGHT]) sx += mag;
    if (sKeys[K_A]) b |= N64_A;
    if (sKeys[K_B]) b |= N64_B;
    if (sKeys[K_X]) b |= N64_CUP;
    if (sKeys[K_Y]) b |= N64_Z;
    if (sKeys[K_L]) b |= N64_R;
    if (sKeys[K_SELECT]) b |= N64_L;
#if defined(MIYOO)
    if (sKeys[K_L2]) b |= N64_Z;
    if (sKeys[K_R2]) b |= N64_L;
#endif
    if (sKeys[K_START]) b |= N64_START;
    if (sKeys[K_CUP]) b |= N64_CUP;
    if (sKeys[K_CDOWN]) b |= N64_CDOWN;
    if (sKeys[K_CLEFT]) b |= N64_CLEFT;
    if (sKeys[K_CRIGHT]) b |= N64_CRIGHT;

    /* SSB64_AUTO=1: headless autopilot for smoke tests -- tap START, then A,
     * alternating once a second, to walk through the title and menus. */
    if (getenv("SSB64_AUTO") != NULL) {
        int f = sAutoFrame++;
        if ((f % 60) < 4) b |= ((f / 60) % 2 == 0) ? N64_START : N64_A;
    }
    /* SSB64_KEYS="30:start+,34:start-,...": scripted presses for same-instant checks against z64,
     * in m64ref's M64REF_KEYS form. N counts the game's pad reads; keys a b z l r start up down left
     * right (the stick, full) cu cd cl cr; + presses, - releases. */
    {
        static int sScriptInit = 0, sScriptN = 0, sScriptPos = 0, sScriptTick = 0;
        static struct { int at; uint16_t btn; int8_t sx, sy; bool down; } sScript[256];
        static uint16_t sHeldB = 0; static int8_t sHeldX = 0, sHeldY = 0;
        if (!sScriptInit) {
            sScriptInit = 1;
            const char *e = getenv("SSB64_KEYS");
            while (e && *e && sScriptN < 256) {
                char name[16]; int at = 0, k = 0;
                at = atoi(e);
                const char *c = strchr(e, ':'); if (!c) break; c++;
                while (*c && *c != '+' && *c != '-' && k < 15) name[k++] = *c++;
                name[k] = 0;
                const bool down = *c == '+';
                uint16_t btn = 0; int8_t x = 0, y = 0;
                if (!strcmp(name, "a")) btn = N64_A; else if (!strcmp(name, "b")) btn = N64_B;
                else if (!strcmp(name, "z")) btn = N64_Z; else if (!strcmp(name, "l")) btn = N64_L;
                else if (!strcmp(name, "r")) btn = N64_R; else if (!strcmp(name, "start")) btn = N64_START;
                else if (!strcmp(name, "cu")) btn = N64_CUP; else if (!strcmp(name, "cd")) btn = N64_CDOWN;
                else if (!strcmp(name, "cl")) btn = N64_CLEFT; else if (!strcmp(name, "cr")) btn = N64_CRIGHT;
                else if (!strcmp(name, "up")) y = STICK_FULL; else if (!strcmp(name, "down")) y = -STICK_FULL;
                else if (!strcmp(name, "left")) x = -STICK_FULL; else if (!strcmp(name, "right")) x = STICK_FULL;
                sScript[sScriptN].at = at; sScript[sScriptN].btn = btn; sScript[sScriptN].sx = x;
                sScript[sScriptN].sy = y; sScript[sScriptN].down = down; sScriptN++;
                e = strchr(c, ','); if (e) e++;
            }
        }
        if (sScriptN) {
            const int t = sScriptTick++;
            while (sScriptPos < sScriptN && sScript[sScriptPos].at <= t) {
                const int i = sScriptPos++;
                if (sScript[i].btn) sHeldB = sScript[i].down ? (sHeldB | sScript[i].btn) : (sHeldB & ~sScript[i].btn);
                if (sScript[i].sx) sHeldX = sScript[i].down ? sScript[i].sx : 0;
                if (sScript[i].sy) sHeldY = sScript[i].down ? sScript[i].sy : 0;
            }
            b |= sHeldB;
            if (sHeldX) sx = sHeldX;
            if (sHeldY) sy = sHeldY;
        }
    }
    pad->button = b;
    pad->stick_x = (int8_t)sx;
    pad->stick_y = (int8_t)sy;
}
