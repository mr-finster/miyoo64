/* Input: keyboard (desktop proxy) / handheld keys -> one N64 pad.
 *
 * The oot-PSP controller path is kept: osContStartReadData() calls
 * sceCtrlPeekBufferPositive() (-> HhInput_Read here) and then
 * OotPspControls_MapButtons/MapStick. Here HhInput_Read already produces N64
 * button bits in pad->Buttons and a centred 0..255 stick in Lx/Ly, so the two
 * map functions are pass-throughs. The handheld layout (notes/controls.md,
 * including the ocarina mode) lives in this file.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "shim/hh_psp_shim.h"
#include "controller.h"
#include "ultra64/ultratypes.h"
#if defined(MIYOO)
#include "miyoo_remap.h"
#endif

#define HH_STICK_MAX 80

/* The d-pad in first person and aiming (the C-up look, bow, hookshot, slingshot, boomerang: the
 * camera modes Play_Update copies to gHhAimCam). OoT turns the view by (1 - cos(stick * 200)) *
 * 1500 units a frame (z_player.c): 4.9 degrees at full tilt, 1.3 at half, 0.6 at a third, so full
 * tilt alone made small targets one-frame taps. There each d-pad axis starts at a third (20 past
 * PadUtils' dead zone of 7) and reaches full tilt after 0.4 s held. OOT_AIMRAMP=0: full tilt. */
u8 gHhAimCam;
#define HH_AIM_MIN 20
#define HH_AIM_RAMP_US 400000u
static int sAimRamp = -1;
static uint64_t sAimSinceX, sAimSinceY; /* when that axis' d-pad went down (0: up) */
extern uint64_t port_time_us64(void);

static int HhInput_AimTilt(uint64_t now, uint64_t* since, bool held) {
    uint64_t dt;

    if (!held) {
        *since = 0;
        return HH_STICK_MAX;
    }
    if (*since == 0) {
        *since = now;
    }
    dt = now - *since;
    if (dt >= HH_AIM_RAMP_US) {
        return HH_STICK_MAX;
    }
    return 7 + HH_AIM_MIN + (int)((60 - HH_AIM_MIN) * dt / HH_AIM_RAMP_US);
}

typedef enum {
    HH_KEY_UP, HH_KEY_DOWN, HH_KEY_LEFT, HH_KEY_RIGHT,
    HH_KEY_A, HH_KEY_B, HH_KEY_X, HH_KEY_Y, HH_KEY_L, HH_KEY_R, HH_KEY_START,
    /* the C buttons and Z (the desktop's keys; R2 and L2 on the Miyoo) */
    HH_KEY_CUP, HH_KEY_CLEFT, HH_KEY_CDOWN, HH_KEY_CRIGHT, HH_KEY_Z,
    HH_KEY_COUNT
} HhKey;

static volatile bool sKeys[HH_KEY_COUNT];
static int sAutoFrame;

/* OOT_INPUT="<reads>:<keys>,<reads>:<keys>,..." (debug, headless tests): hold
 * the keys -- u d l r (stick), a b z s R U H J K (A, B, Z, START, R, C-up, C-left, C-down, C-right) -- for that many input
 * reads, then the next step; nothing is held after the last. Walks Link into
 * walls and counters to check the collision against the prerendered rooms. */
static const char* sInputScript;
static int sInputLeft = -1;
static char sInputKeys[12];

/* OOT_AUTOPATH (debug, hh_main.c HhAutoPath): the stick toward the next waypoint, [-1, 1] each way */
int gHhAutoStickOn;
float gHhAutoStickX, gHhAutoStickY;

static void HhInput_Script(u16* btn, int* sx, int* sy) {
    const char* k;

    if (gHhAutoStickOn) {
        *sx += (int)(gHhAutoStickX * HH_STICK_MAX);
        *sy += (int)(gHhAutoStickY * HH_STICK_MAX);
    }

    if (sInputLeft < 0) {
        sInputScript = getenv("OOT_INPUT");
        sInputLeft = 0;
        sInputKeys[0] = '\0';
    }
    while (sInputLeft == 0 && sInputScript != NULL && *sInputScript != '\0') {
        char* end;
        size_t n = 0;

        sInputLeft = (int)strtol(sInputScript, &end, 10);
        sInputScript = end;
        if (*sInputScript == ':') {
            sInputScript++;
        }
        while (*sInputScript != '\0' && *sInputScript != ',' && n < sizeof(sInputKeys) - 1) {
            sInputKeys[n++] = *sInputScript++;
        }
        sInputKeys[n] = '\0';
        if (*sInputScript == ',') {
            sInputScript++;
        }
    }
    if (sInputLeft == 0) {
        return;
    }
    sInputLeft--;
    for (k = sInputKeys; *k != '\0'; k++) {
        switch (*k) {
            case 'u': *sy += HH_STICK_MAX; break;
            case 'd': *sy -= HH_STICK_MAX; break;
            case 'l': *sx -= HH_STICK_MAX; break;
            case 'r': *sx += HH_STICK_MAX; break;
            case 'a': *btn |= BTN_A; break;
            case 'b': *btn |= BTN_B; break;
            case 'z': *btn |= BTN_Z; break;
            case 's': *btn |= BTN_START; break;
            case 'R': *btn |= BTN_R; break; /* pause pages: R next, z previous */
            case 'U': *btn |= BTN_CUP; break; /* first-person view: the horizon, for sky checks */
            case 'H': *btn |= BTN_CLEFT; break;
            case 'J': *btn |= BTN_CDOWN; break;
            case 'K': *btn |= BTN_CRIGHT; break;
            default: break;
        }
    }
}

/* Set from the audio code: the ocarina is out (src/audio/game/general.c). */
extern u8 sIsOcarinaInputEnabled;

/* START is a modifier (the controls review, 2026-09-25; F-Zero X's chords). Held with another
 * button it sends a chord, in either order, and START itself goes out on release, for one read,
 * only when no chord went out -- so the N64 buttons the handheld lacks are START + a button:
 *   START + L        N64 L: the minimap on/off (z_map_exp.c), missing before
 *   START + R        C-up: first person / Navi (R then START as before; now START then R too)
 *   START + d-pad    C-up / C-down / C-left / C-right (no stick while START is held)
 * The press accumulates in PadMgr until the game reads it, so a one-read pulse is never lost.
 * OOT_STARTCHORD=0: the old layout (START at once, R + START = C-up). */
static int sStartChord = -1;
static volatile bool sStartUsed, sStartPulse;

static bool HhInput_StartChordOn(void) {
    if (sStartChord < 0) {
        const char* e = getenv("OOT_STARTCHORD");
        sStartChord = !(e != NULL && e[0] == '0');
    }
    return sStartChord != 0;
}

static bool HhInput_IsChordKey(int key) {
    return key == HH_KEY_L || key == HH_KEY_R || key == HH_KEY_UP || key == HH_KEY_DOWN || key == HH_KEY_LEFT ||
           key == HH_KEY_RIGHT;
}

void HhInput_SetKey(int key, bool down) {
    if (key >= 0 && key < HH_KEY_COUNT) {
        if (key == HH_KEY_START) {
            if (down && !sKeys[HH_KEY_START]) {
                /* R already held (the old R + START order): that is a chord */
                sStartUsed = sKeys[HH_KEY_R];
            } else if (!down && sKeys[HH_KEY_START] && !sStartUsed) {
                sStartPulse = true;
            }
        } else if (down && sKeys[HH_KEY_START] && HhInput_IsChordKey(key)) {
            sStartUsed = true;
        }
        sKeys[key] = down;
    }
}

/* SDL2 scancodes (desktop proxy, gfx_sdl.c). */
void controller_sdl_key(int sc, bool down) {
    int key = -1;

    switch (sc) {
        case 82: key = HH_KEY_UP; break;    /* arrows */
        case 81: key = HH_KEY_DOWN; break;
        case 80: key = HH_KEY_LEFT; break;
        case 79: key = HH_KEY_RIGHT; break;
        case 27: key = HH_KEY_A; break;     /* X */
        case 29: key = HH_KEY_B; break;     /* Z */
        case 40: key = HH_KEY_START; break; /* Enter */
        case 225: key = HH_KEY_Z; break;    /* LShift */
        case 20: key = HH_KEY_L; break;     /* Q */
        case 26: key = HH_KEY_R; break;     /* W */
        case 12: key = HH_KEY_CUP; break;   /* I */
        case 13: key = HH_KEY_CLEFT; break; /* J */
        case 14: key = HH_KEY_CDOWN; break; /* K */
        case 15: key = HH_KEY_CRIGHT; break; /* L */
        default: break;
    }
    HhInput_SetKey(key, down);
}

void HhInput_Read(SceCtrlData* pad) {
    u16 btn = 0;
    int sx = 0;
    int sy = 0;
    bool ocarina = sIsOcarinaInputEnabled;

    memset(pad, 0, sizeof(*pad));

    const bool chords = HhInput_StartChordOn();
    const bool chord = chords && sKeys[HH_KEY_START];

    if (ocarina || chord) {
        /* Ocarina mode: the d-pad plays the four C notes (notes/controls.md); with START held
         * (chords) the d-pad is the C buttons too. */
        if (sKeys[HH_KEY_UP]) btn |= BTN_CUP;
        if (sKeys[HH_KEY_DOWN]) btn |= BTN_CDOWN;
        if (sKeys[HH_KEY_LEFT]) btn |= BTN_CLEFT;
        if (sKeys[HH_KEY_RIGHT]) btn |= BTN_CRIGHT;
    } else {
        int tx = HH_STICK_MAX, ty = HH_STICK_MAX;

        if (sAimRamp < 0) {
            const char* e = getenv("OOT_AIMRAMP");
            sAimRamp = !(e != NULL && e[0] == '0');
        }
        if (sAimRamp && gHhAimCam) {
            const uint64_t now = port_time_us64();

            tx = HhInput_AimTilt(now, &sAimSinceX, sKeys[HH_KEY_LEFT] || sKeys[HH_KEY_RIGHT]);
            ty = HhInput_AimTilt(now, &sAimSinceY, sKeys[HH_KEY_UP] || sKeys[HH_KEY_DOWN]);
        } else {
            sAimSinceX = sAimSinceY = 0;
        }
        if (sKeys[HH_KEY_UP]) sy += ty;
        if (sKeys[HH_KEY_DOWN]) sy -= ty;
        if (sKeys[HH_KEY_LEFT]) sx -= tx;
        if (sKeys[HH_KEY_RIGHT]) sx += tx;
    }

    if (sKeys[HH_KEY_A]) btn |= BTN_A;
    if (sKeys[HH_KEY_B]) btn |= BTN_B;
    if (sKeys[HH_KEY_Z]) btn |= BTN_Z;
    if (chords) {
        if (sKeys[HH_KEY_L]) btn |= chord ? BTN_L : BTN_Z; /* L = Z-target; START + L = N64 L */
        if (sKeys[HH_KEY_R]) btn |= chord ? BTN_CUP : BTN_R; /* START + R = C-up */
        if (sStartPulse) {
            sStartPulse = false;
            btn |= BTN_START; /* START alone, on release */
        }
    } else {
        if (sKeys[HH_KEY_L]) btn |= BTN_Z; /* L = Z-target */
        if (sKeys[HH_KEY_R] && sKeys[HH_KEY_START]) {
            btn |= BTN_CUP; /* R + START = C-up (first person / Navi) */
        } else {
            if (sKeys[HH_KEY_R]) btn |= BTN_R;
            if (sKeys[HH_KEY_START]) btn |= BTN_START;
        }
    }
    if (sKeys[HH_KEY_X]) btn |= BTN_CLEFT;
    if (sKeys[HH_KEY_Y]) btn |= BTN_CDOWN;
    if (sKeys[HH_KEY_CUP]) btn |= BTN_CUP;
    if (sKeys[HH_KEY_CLEFT]) btn |= BTN_CLEFT;
    if (sKeys[HH_KEY_CDOWN]) btn |= BTN_CDOWN;
    if (sKeys[HH_KEY_CRIGHT]) btn |= BTN_CRIGHT;

    HhInput_Script(&btn, &sx, &sy);

    /* OOT_AUTO=1: headless autopilot -- tap START then A through the title and
     * menus (the desktop smoke test). */
    static int sAutoOn = -1; /* read once: the environment is set before the game starts */
    if (sAutoOn < 0) {
        sAutoOn = getenv("OOT_AUTO") != NULL;
    }
    if (sAutoOn) {
        int f = sAutoFrame++;

        if ((f % 60) < 4) {
            btn |= ((f / 60) % 2 == 0) ? BTN_START : BTN_A;
        }
    }

    pad->Buttons = btn;
    pad->Lx = (unsigned char)(128 + sx);
    pad->Ly = (unsigned char)(128 - sy);
}

/* The pad already carries N64 bits; the stick is centred on 128. */
u16 OotPspControls_MapButtons(u32 buttons) {
    return (u16)buttons;
}

s8 OotPspControls_MapStick(u8 raw) {
    return (s8)((int)raw - 128);
}

s32 OotPspControls_Load(void) {
    return 0;
}

/* The Miyoo Mini's SDL 1.2 keys: X/Y C-left/C-down, L1 Z-target, R1 shield, START with its chords,
 * R2 C-right and L2 C-up (first person / Navi). MENU (Escape) is the window backend's. */
#if defined(MIYOO)
/* MENU > CONTROLS (kit miyoo_remap.c): the layout below, in play, as the screen shows it; a remap moves the
 * pressed buttons, so every mode and chord here follows. */
const struct miyoo_remap_row miyoo_remap_rows[] = {
    { MR_A, MB_A, 0, 0, "ACTION" },
    { MR_B, MB_B, 0, 0, "SWORD" },
    { MR_Z, MB_L1, 0, 0, "Z-TARGET" },
    { MR_R, MB_R1, 0, 0, "SHIELD" },
    { MR_START, MB_START, 0, 0, "PAUSE (ON RELEASE)" },
    { MR_CUP, MB_L2, 0, 0, "FIRST PERSON, NAVI" },
    { MR_CDOWN, MB_Y, 0, 0, "ITEM" },
    { MR_CLEFT, MB_X, 0, 0, "ITEM" },
    { MR_CRIGHT, MB_R2, 0, 0, "ITEM" },
    { MR_L, MB_L1, 0, MB_START, "MAP" },
    { MR_STICK, MB_DPAD, 0, 0, "MOVE" },
    { MR_DPAD, MB_NONE, 0, 0, "OCARINA: D-PAD" },
};
const int miyoo_remap_nrows = (int)(sizeof(miyoo_remap_rows) / sizeof(miyoo_remap_rows[0]));
#endif

void controller_handheld_key(int sym, int down) {
#if defined(MIYOO)
    sym = miyoo_remap_key(sym); /* MENU > CONTROLS */
#endif
    int key = -1;

    switch (sym) {
#if defined(MIYOO)
        case 273: key = HH_KEY_UP; break;    /* SDLK_UP */
        case 274: key = HH_KEY_DOWN; break;  /* SDLK_DOWN */
        case 276: key = HH_KEY_LEFT; break;  /* SDLK_LEFT */
        case 275: key = HH_KEY_RIGHT; break; /* SDLK_RIGHT */
        case 32: key = HH_KEY_A; break;      /* SDLK_SPACE: A */
        case 306: key = HH_KEY_B; break;     /* SDLK_LCTRL: B */
        case 304: key = HH_KEY_X; break;     /* SDLK_LSHIFT: X */
        case 308: key = HH_KEY_Y; break;     /* SDLK_LALT: Y */
        case 'e': key = HH_KEY_L; break;     /* L1 */
        case 't': key = HH_KEY_R; break;     /* R1 */
        case 8: key = HH_KEY_CRIGHT; break;  /* SDLK_BACKSPACE: R2 = C-right */
        case 9: key = HH_KEY_CUP; break;     /* SDLK_TAB: L2 = C-up */
        case 13: key = HH_KEY_START; break;  /* SDLK_RETURN: START */
#endif
        default: break;
    }
    HhInput_SetKey(key, down != 0);
}
