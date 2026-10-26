/**
 * Input: handheld keys (device, gfx_sdl12.c) or desktop keys (gfx_sdl.c) -> one
 * N64 pad, laid out as notes/controls.md:
 *
 *   d-pad        stick +-80 (the game clamps to 63) and the d-pad bits (menus)
 *   A / B        A (accelerate) / B (boost)
 *   X / Y        C-down (air brake) / C-up (look back)
 *   L / R        Z / R (lean, side attacks, spin)
 *   Start        Start -- sent on release, and only if no chord was used
 *   SELECT / R2  C-right (camera)
 *   Start+d-pad  C-up / C-down / C-left / C-right (no stick while Start is held)
 *   Start+L      N64 L
 *   (no 3D resolution or filter chords: the 3D is always 100% SMOOTH -- the user,
 *   2026-09-24; Start+R and Start+X of rounds 4-8 are gone)
 *   MENU         the handheld menu (gfx_sdl12.c)
 *
 * The desktop keys feed the same logical keys so the chords can be tested:
 * arrows, X = A, Z = B, C = X, V = Y, A = L, S = R, Enter = Start;
 * I/J/K/L are the four C buttons directly.
 *
 * FZX_KEYS=<vi>:<key>+|-,... scripts presses for headless tests, e.g.
 * FZX_KEYS=900:start+,902:r+,906:r-,908:start- (keys: up down left right a b
 * x y l r start).
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "libultra/ultra64.h"
#include "port_os.h"
#if defined(MIYOO)
#include "miyoo_remap.h"
#endif

extern uint32_t port_time_us(void);

#define STICK_MAX 80
#define START_PULSE_TICKS 2 /* a released Start is held this many polls */

enum {
    K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_X, K_Y, K_L, K_R, K_START,
    K_CUP, K_CDOWN, K_CLEFT, K_CRIGHT, /* desktop extras */
    K_COUNT
};

static volatile bool sKeys[K_COUNT];
static bool sStartChord;   /* a chord was used during this Start hold */
static bool sStartWasDown;
static int sStartPulse;    /* polls of START still to send after a release */
/* Every key edge (key < 0: MENU, an unmapped desktop key, is ignored). */
static void set_key(int key, bool down) {
    if (key >= 0 && key < K_COUNT) {
        sKeys[key] = down;
    }
}

#if defined(MIYOO)
/* The Miyoo Mini's keys (its SDL 1.2: miyoo_panel.h's MIYOO_KEY_*), on the handheld layout: A, B, X, Y,
 * L1, R1 and Start as the handheld's; SELECT and R2 are the camera (C-right), L2 is C-left (machine select's Novice). MENU (Escape) never reaches here: gfx_sdl12.c
 * opens the menu, which passes key releases through (controller_handheld_key with down 0, which comes here). */
static void controller_miyoo_key(int sym, int down) {
    int key = -1;
    switch (sym) {
        case 273: key = K_UP; break;     /* MIYOO_KEY_UP */
        case 274: key = K_DOWN; break;   /* MIYOO_KEY_DOWN */
        case 276: key = K_LEFT; break;   /* MIYOO_KEY_LEFT */
        case 275: key = K_RIGHT; break;  /* MIYOO_KEY_RIGHT */
        case 32: key = K_A; break;       /* MIYOO_KEY_A (Space) */
        case 306: key = K_B; break;      /* MIYOO_KEY_B (Left Ctrl) */
        case 304: key = K_X; break;      /* MIYOO_KEY_X (Left Shift) */
        case 308: key = K_Y; break;      /* MIYOO_KEY_Y (Left Alt) */
        case 101: key = K_L; break;      /* MIYOO_KEY_L1 ('e') */
        case 116: key = K_R; break;      /* MIYOO_KEY_R1 ('t') */
        case 13: key = K_START; break;   /* MIYOO_KEY_START (Return) */
        case 305: key = K_CRIGHT; break; /* MIYOO_KEY_SELECT (Right Ctrl): camera */
        case 8: key = K_CRIGHT; break;   /* MIYOO_KEY_R2 (Backspace): camera */
        case 9: key = K_CLEFT; break;    /* MIYOO_KEY_L2 (Tab) */
        default: break;
    }
    if (sym == 27 && down && sKeys[K_START]) {
        sStartChord = true; /* the menu counts as a chord, as the handheld's MENU (B6) */
    }
    set_key(key, down != 0);
}
#endif

/* gfx_sdl12.c and the menu hand over the Mini's keys. The menu sends MENU (down, up) as it opens, so
 * it counts as a chord (B6): a Start held into it doesn't pause. */
#if defined(MIYOO)
/* MENU > CONTROLS (kit miyoo_remap.c): the layout below, in play, as the screen shows it; a remap moves the
 * pressed buttons, so every mode and chord here follows. */
const struct miyoo_remap_row miyoo_remap_rows[] = {
    { MR_A, MB_A, 0, 0, "ACCELERATE" },
    { MR_B, MB_B, 0, 0, "BOOST" },
    { MR_Z, MB_L1, 0, 0, "LEAN LEFT" },
    { MR_R, MB_R1, 0, 0, "LEAN RIGHT" },
    { MR_START, MB_START, 0, 0, "PAUSE (ON RELEASE)" },
    { MR_CUP, MB_Y, 0, 0, "LOOK BACK" },
    { MR_CDOWN, MB_X, 0, 0, "AIR BRAKE" },
    { MR_CLEFT, MB_L2, 0, 0, NULL },
    { MR_CRIGHT, MB_R2, MB_SELECT, 0, "CAMERA" },
    { MR_L, MB_L1, 0, MB_START, NULL },
    { MR_STICK, MB_DPAD, 0, 0, "STEER" },
    { MR_DPAD, MB_DPAD, 0, 0, "MENUS" },
};
const int miyoo_remap_nrows = (int)(sizeof(miyoo_remap_rows) / sizeof(miyoo_remap_rows[0]));
#endif

void controller_handheld_key(int sym, int down) {
#if defined(MIYOO)
    sym = miyoo_remap_key(sym); /* MENU > CONTROLS */
#endif
#if defined(MIYOO)
    controller_miyoo_key(sym, down);
#else
    (void) sym;
    (void) down;
#endif
}

/* SDL2 scancodes (desktop proxy). */
void controller_sdl_key(int sc, bool down) {
    int key = -1;
    switch (sc) {
        case 82: key = K_UP; break;     /* arrows */
        case 81: key = K_DOWN; break;
        case 80: key = K_LEFT; break;
        case 79: key = K_RIGHT; break;
        case 27: key = K_A; break;      /* X */
        case 29: key = K_B; break;      /* Z */
        case 6: key = K_X; break;       /* C */
        case 25: key = K_Y; break;      /* V */
        case 4: key = K_L; break;       /* A */
        case 22: key = K_R; break;      /* S */
        case 40: key = K_START; break;  /* Enter */
        case 12: key = K_CUP; break;    /* I */
        case 14: key = K_CDOWN; break;  /* K */
        case 13: key = K_CLEFT; break;  /* J */
        case 15: key = K_CRIGHT; break; /* L */
        default: break;
    }
    set_key(key, down);
}

/* T13 (MK64 259e562): FZX_PKEYS="<poll>:<key>+|-,..." is the pad script of miyoo64
 * tools/m64ref.sh (M64REF_KEYS) on the same clock: <poll> counts the game's controller reads
 * (gPortPolls; the first read is 1), so the port and mupen64plus get the same input at the same
 * read and reach the same moment. FZX_DUMP_POLLS dumps those frames (port_main.c) and
 * tools/bestmatch.py pairs them with the reference. key+ holds a key from that read on, key-
 * releases it. Keys: a b z start l r cu cd cl cr du dd dl dr, and the stick up down left right
 * (at 80). The script replaces the pad (desktop tests; unset = the real pad and FZX_KEYS). */
u32 gPortPolls; /* controller reads so far (a port object: outside the save states) */
static bool poll_script(OSContPad* pad) {
    static const char* sPKeys = (const char*) -1;
    static const char* const names[18] = { "a", "b", "z", "start", "l", "r", "cu", "cd", "cl", "cr",
                                           "du", "dd", "dl", "dr", "up", "down", "left", "right" };
    static const u16 bits[14] = { A_BUTTON, B_BUTTON, Z_TRIG, START_BUTTON, L_TRIG, R_TRIG, U_CBUTTONS,
                                  D_CBUTTONS, L_CBUTTONS, R_CBUTTONS, U_JPAD, D_JPAD, L_JPAD, R_JPAD };
    static bool held[18];
    const char* p;
    u16 b = 0;
    int k;
    if (sPKeys == (const char*) -1) {
        sPKeys = getenv("FZX_PKEYS");
    }
    if (sPKeys == NULL) {
        return false;
    }
    for (p = sPKeys; p != NULL && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
        char* e;
        const unsigned long at = strtoul(p, &e, 10);
        if (at != gPortPolls || *e != ':') {
            continue;
        }
        for (k = 0; k < 18; k++) {
            const size_t n = strlen(names[k]);
            if (strncmp(e + 1, names[k], n) == 0 && (e[1 + n] == '+' || e[1 + n] == '-')) {
                held[k] = e[1 + n] == '+';
            }
        }
    }
    for (k = 0; k < 14; k++) {
        if (held[k]) b |= bits[k];
    }
    pad->button = b;
    pad->stick_y = held[14] ? 80 : held[15] ? -80 : 0;
    pad->stick_x = held[16] ? -80 : held[17] ? 80 : 0;
    return true;
}

/* FZX_KEYS: apply the presses scripted for this retrace. */
static void scripted_keys(void) {
    static const char* sScript;
    static int sInit;
    static const char* const names[] = { "up", "down", "left", "right", "a", "b", "x", "y", "l", "r", "start" };
    const char* p;
    const u32 vi = port_os_vi_count();
    if (!sInit) {
        sScript = getenv("FZX_KEYS");
        sInit = 1;
    }
    for (p = sScript; p != NULL && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
        char* end;
        const unsigned long at = strtoul(p, &end, 10);
        int k;
        if (at != vi || *end != ':') {
            continue;
        }
        for (k = 0; k < (int) (sizeof(names) / sizeof(names[0])); k++) {
            const size_t n = strlen(names[k]);
            if (strncmp(end + 1, names[k], n) == 0 && (end[1 + n] == '+' || end[1 + n] == '-')) {
                set_key(k, end[1 + n] == '+');
            }
        }
    }
}

void port_input_read(OSContPad* pad) {
    u16 b = 0;
    s32 x = 0, y = 0;
    bool start;

    scripted_keys();
    start = sKeys[K_START];

    if (start) {
        /* Start held: the d-pad and L are chords, nothing reaches the stick. */
        if (sKeys[K_UP]) { b |= U_CBUTTONS; sStartChord = true; }
        if (sKeys[K_DOWN]) { b |= D_CBUTTONS; sStartChord = true; }
        if (sKeys[K_LEFT]) { b |= L_CBUTTONS; sStartChord = true; }
        if (sKeys[K_RIGHT]) { b |= R_CBUTTONS; sStartChord = true; }
        if (sKeys[K_L]) { b |= L_TRIG; sStartChord = true; }
    } else {
        if (sKeys[K_UP]) { y += STICK_MAX; b |= U_JPAD; }
        if (sKeys[K_DOWN]) { y -= STICK_MAX; b |= D_JPAD; }
        if (sKeys[K_LEFT]) { x -= STICK_MAX; b |= L_JPAD; }
        if (sKeys[K_RIGHT]) { x += STICK_MAX; b |= R_JPAD; }
        if (sKeys[K_L]) b |= Z_TRIG;
        if (sKeys[K_R]) b |= R_TRIG;
        if (sStartWasDown && !sStartChord) {
            sStartPulse = START_PULSE_TICKS; /* a plain Start tap */
        }
        sStartChord = false;
    }
    sStartWasDown = start;
    if (sStartPulse > 0) {
        sStartPulse--;
        b |= START_BUTTON;
    }
    if (sKeys[K_A]) b |= A_BUTTON;
    if (sKeys[K_B]) b |= B_BUTTON;
    if (sKeys[K_X] && !start) b |= D_CBUTTONS;
    if (sKeys[K_Y]) b |= U_CBUTTONS;
    if (sKeys[K_CUP]) b |= U_CBUTTONS;
    if (sKeys[K_CDOWN]) b |= D_CBUTTONS;
    if (sKeys[K_CLEFT]) b |= L_CBUTTONS;
    if (sKeys[K_CRIGHT]) b |= R_CBUTTONS;

    {
        /* FZX_AUTO=1: a headless driver -- taps START and A about once a second
         * so the game walks from the title through the menus into a race, and
         * holds the accelerator in races. */
        static int sAuto = -1;
        if (sAuto < 0) {
            const char* e = getenv("FZX_AUTO");
            extern int port_bench_active(void);
            sAuto = (e && e[0] == '1') || port_bench_active();
        }
        if (sAuto) {
            extern s32 gGameMode;
            const u32 t = port_os_vi_count() % 90;
            if ((gGameMode & 0x1F) == 1 || (gGameMode & 0x1F) == 2 || (gGameMode & 0x1F) == 0xE) {
                b |= A_BUTTON;
            } else if (t < 4) {
                b |= START_BUTTON;
            } else if (t >= 45 && t < 49) {
                b |= A_BUTTON;
            }
        }
    }
    pad->button = b;
    pad->stick_x = (s8) x;
    pad->stick_y = (s8) y;
    gPortPolls++;
    poll_script(pad);
    {
        /* FZX_COURSE=<0..23> (a test switch): gCourseIndex held at that course on every read, as
         * mupen64plus's "Track Modifier" cheat (800F8517) holds it on the N64, so a GP race or a
         * time attack loads that course on both for the reference shots. */
        static int sCourse = -2;
        if (sCourse == -2) {
            const char* e = getenv("FZX_COURSE");
            sCourse = e != NULL && *e ? atoi(e) : -1;
        }
        if (sCourse >= 0) {
            extern s32 gCourseIndex;
            gCourseIndex = sCourse;
        }
    }
    {
        /* FZX_SHIELD=1 (a test switch): player 1's energy float gets its upper half 0x4326 on every
         * read, as mupen64plus's "Player 1\Infinite Shield" cheat (812C4B48 4326) does on the N64:
         * Racer.energy is gRacers + 0x228 (fzx_racer.h), so the wall-grinding test drive lives to
         * the pit lane (the pit force field). */
        static int sShield = -1;
        if (sShield < 0) {
            const char* e = getenv("FZX_SHIELD");
            sShield = e != NULL && e[0] == '1';
        }
        if (sShield) {
            extern u8 gRacers[];
            u32 bits;
            memcpy(&bits, gRacers + 0x228, 4);
            bits = (bits & 0xFFFF) | 0x43260000u;
            memcpy(gRacers + 0x228, &bits, 4);
        }
    }
}

/* FZX_CPUPLAYER=1 (racer.c, a test switch): the player's machine is CPU-driven from the start,
 * so the autopilot finishes races and reaches the results screens (the seam scans). */
int port_test_cpu_player(void) {
    static int s = -1;
    if (s < 0) {
        const char* e = getenv("FZX_CPUPLAYER");
        s = e != NULL && e[0] == '1';
    }
    return s;
}

/* FZX_TRANSITION=<1..7> (transition.c, a test switch): force every tile/fade screen transition
 * to one type (1 small tiles, 2 large tiles, 3 whirl, 4 spiral, 5 fade, 6 static fade, 7 strips). */
int port_test_transition(void) {
    static int s = -1;
    if (s < 0) {
        const char* e = getenv("FZX_TRANSITION");
        s = e != NULL && e[0] >= '1' && e[0] <= '7' ? e[0] - '0' : 0;
    }
    return s;
}
