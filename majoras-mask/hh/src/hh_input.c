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

/* Fine aim (issue #8): the d-pad in first person and aiming -- the C-up look, the bow (magic arrows too),
 * the hookshot, Deku bubbles, Zora fins: the camera modes Play_UpdateMain copies to gHhAimCam, never while
 * paused. The game turns the aim by (1 - cos(stick * 200)) * 1500 units a frame (z_player.c): 7.9 degrees
 * a frame at full tilt (stick 80), so a one-frame tap moved it 7.9 degrees. Each d-pad axis now starts at
 * MM_FINEAIMPCT percent of full tilt (default 20: stick 16, 0.38 degrees a frame), stays there for the
 * first 250 ms, then rises in a straight line to full tilt at MM_FINEAIMMS (default 1000 ms). Walking,
 * menus and every other view: full tilt as before. MM_FINEAIM=0: full tilt everywhere (OOT_AIMRAMP=0, the
 * old name, too). */
u8 gHhAimCam;
#define HH_AIM_HOLD_US 250000u
static int sFineAim = -1;
static int sFineMin = 16;
static uint64_t sFineHoldUs = HH_AIM_HOLD_US, sFineFullUs = 1000000u;
static uint64_t sAimSinceX, sAimSinceY; /* when that axis' d-pad went down (0: up) */
extern uint64_t port_time_us64(void);

static int HhInput_EnvInt(const char* name, int def, int lo, int hi) {
    const char* e = getenv(name);
    int v;

    if (e == NULL || e[0] == '\0') {
        return def;
    }
    v = atoi(e);
    return v < lo ? lo : (v > hi ? hi : v);
}

static void HhInput_FineAimInit(void) {
    const char* e = getenv("MM_FINEAIM");
    int pct, ms;

    if (e == NULL) {
        e = getenv("OOT_AIMRAMP"); /* the old name */
    }
    sFineAim = !(e != NULL && e[0] == '0');
    pct = HhInput_EnvInt("MM_FINEAIMPCT", 20, 5, 100);
    ms = HhInput_EnvInt("MM_FINEAIMMS", 1000, 0, 5000);
    sFineMin = (HH_STICK_MAX * pct + 50) / 100;
    sFineFullUs = (uint64_t)ms * 1000u;
    sFineHoldUs = sFineFullUs < HH_AIM_HOLD_US ? sFineFullUs : HH_AIM_HOLD_US;
}

/* MENU > OPTIONS (hh_options.c): the fine aim's variables are read again at the next d-pad read. */
void HhInput_FineAimReload(void) {
    sFineAim = -1;
}

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
    if (dt >= sFineFullUs) {
        return HH_STICK_MAX;
    }
    if (dt < sFineHoldUs) {
        return sFineMin;
    }
    return sFineMin + (int)((uint64_t)(HH_STICK_MAX - sFineMin) * (dt - sFineHoldUs) / (sFineFullUs - sFineHoldUs));
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
 * the keys -- u d l r (stick), a b z s R U H J K Y (A, B, Z, START, R, C-up, C-left, C-down, C-right, L), 8 2 4 6
 * (d-pad up, down, left, right), < > ^ v (the handheld's d-pad: left, right, up, down) -- for that many input
 * reads, then the next step; nothing is held after the last. Walks Link into
 * walls and counters to check the collision against the prerendered rooms. */
static const char* sInputScript;
static int sInputLeft = -1;
static char sInputKeys[12];
/* < > ^ v in OOT_INPUT: the handheld's d-pad itself, as a player presses it (the fine aim applies), on a clock of
 * 50 ms a read (the game's 20 fps) so a test's numbers do not depend on the emulator's speed */
static bool sScriptPad[4];
static bool sScriptClockOn;
static uint64_t sScriptClockUs;

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
    memset(sScriptPad, 0, sizeof(sScriptPad));
    if (sInputLeft == 0) {
        return;
    }
    sInputLeft--;
    for (k = sInputKeys; *k != '\0'; k++) {
        switch (*k) {
            case '^': sScriptPad[0] = true; sScriptClockOn = true; break;
            case 'v': sScriptPad[1] = true; sScriptClockOn = true; break;
            case '<': sScriptPad[2] = true; sScriptClockOn = true; break;
            case '>': sScriptPad[3] = true; sScriptClockOn = true; break;
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
            case 'Y': *btn |= BTN_L; break;
            case '8': *btn |= BTN_DUP; break; /* the d-pad: the map select's rows (the soak's scripts) */
            case '2': *btn |= BTN_DDOWN; break;
            case '4': *btn |= BTN_DLEFT; break;
            case '6': *btn |= BTN_DRIGHT; break;
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

    HhInput_Script(&btn, &sx, &sy);
    sScriptClockUs += 50000u;

    const bool up = sKeys[HH_KEY_UP] || sScriptPad[0];
    const bool down = sKeys[HH_KEY_DOWN] || sScriptPad[1];
    const bool left = sKeys[HH_KEY_LEFT] || sScriptPad[2];
    const bool right = sKeys[HH_KEY_RIGHT] || sScriptPad[3];
    const bool chords = HhInput_StartChordOn();
    const bool chord = chords && sKeys[HH_KEY_START];

    if (ocarina || chord) {
        /* Ocarina mode: the d-pad plays the four C notes (notes/controls.md); with START held
         * (chords) the d-pad is the C buttons too. */
        if (up) btn |= BTN_CUP;
        if (down) btn |= BTN_CDOWN;
        if (left) btn |= BTN_CLEFT;
        if (right) btn |= BTN_CRIGHT;
    } else {
        int tx = HH_STICK_MAX, ty = HH_STICK_MAX;

        if (sFineAim < 0) {
            HhInput_FineAimInit();
        }
        if (sFineAim && gHhAimCam) {
            const uint64_t now = sScriptClockOn ? sScriptClockUs : port_time_us64();

            tx = HhInput_AimTilt(now, &sAimSinceX, left || right);
            ty = HhInput_AimTilt(now, &sAimSinceY, up || down);
        } else {
            sAimSinceX = sAimSinceY = 0;
        }
        if (up) sy += ty;
        if (down) sy -= ty;
        if (left) sx -= tx;
        if (right) sx += tx;
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
    { MR_CUP, MB_L2, 0, 0, "FIRST PERSON, TATL" },
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
