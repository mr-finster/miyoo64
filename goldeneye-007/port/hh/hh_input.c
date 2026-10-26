/* hh_input.c: the handheld keys (the Miyoo Mini's, and the desktop proxy's keyboard) -> controller 1.
 *
 * Layout (notes/controls.md, agreed 2026-09-24), fed to GoldenEye's default control style
 * 1.1 "Honey":
 *   d-pad   walk forward/back and turn, through a virtual stick: a tap turns a little,
 *           holding ramps to full speed
 *   L / R   strafe (C-left / C-right; the game leans while aiming)
 *   B       fire (Z)            Y (hold)  aim (R): the d-pad moves the crosshair, which
 *   A       action (N64 B)                stays where it is left; pushing past the edge
 *   X       next weapon (N64 A)           turns or pitches the view
 *   L2      C-down (look down, crouch while aiming) while held on its own
 *   R2      C-up (look up)
 *   START   pause (the watch)
 * Outside gameplay (front-end menus, the watch) A and B are the N64's A and B, so menus
 * select and go back the way the N64 labels say. In the front end the d-pad is a plain stick;
 * on the watch it is the N64's d-pad (one item a press, the game's own hold repeat). */
#include <ultra64.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "bondconstants.h"
#include "hh_hw.h"
#include "hh_os.h"
#include "hh_plat.h"
#include "savestate.h" /* SAVESTATE_EXCLUDE: host state a state load must not overwrite (port/kit) */
#if defined(TARGET_MIYOO)
#include "miyoo_remap.h"
#endif

#define STICK_FULL 80  /* GoldenEye's full deflection (dead zone +-5) */
#define AIM_EDGE 60    /* aiming: the crosshair's travel; past it the view turns */

/* tunables for the device (GE_TURN_START, GE_TURN_RAMP_MS, GE_AIM_RATE) */
static int sTurnStart SAVESTATE_EXCLUDE = 24;      /* stick x the moment a turn key goes down */
static int sTurnRampMs SAVESTATE_EXCLUDE = 250;    /* ... reaching full deflection after this long */
static float sAimRate SAVESTATE_EXCLUDE = 110.0f;  /* aiming: crosshair stick units per second */

enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_X, K_Y, K_L, K_R, K_START, K_LOOKDOWN, K_LOOKUP, K_COUNT };

static bool sKey[K_COUNT] SAVESTATE_EXCLUDE;
static u64 sDownNs[K_COUNT] SAVESTATE_EXCLUDE;
static float sAimX SAVESTATE_EXCLUDE, sAimY SAVESTATE_EXCLUDE;
static u64 sLastPollNs SAVESTATE_EXCLUDE;
static bool sTunablesRead SAVESTATE_EXCLUDE;

extern struct player *g_CurrentPlayer; /* player.c; NULL before the first stage */
s32 get_BONDdata_outside_watch_menu_flag(void);
LEVELID bossGetStageNum(void);

static int sScriptN SAVESTATE_EXCLUDE = -1; /* GE_PADSCRIPT entries (below); -1 until read */

/* The clock of the turn ramp and the aim rate: the host's, except under GE_PADSCRIPT, where
 * it is the VI count (16.7 ms a tick), so a scripted run turns the same however fast the host
 * renders -- two runs of a script stay frame-comparable (tools/sweep.sh). */
static u64 hh_input_ns(void)
{
    if (sScriptN > 0)
        return (u64)hh_vicount * 16683333ull;
    return hh_host_ns();
}

static void hh_input_key(int key, bool down)
{
    if (key < 0 || key >= K_COUNT || sKey[key] == down)
        return;
    sKey[key] = down;
    if (down)
        sDownNs[key] = hh_input_ns();
}

/* The keys: the letters are GE_PADSCRIPT's (below): d-pad u/d/l/r, A/B/X/Y = a/b/x/y, L = m,
 * R = n, START = s, L2 = k (look down), R2 = o (look up). MENU is handled by the window backend. */
void controller_handheld_key(int sym, int down)
{
    int key = -1;
#if defined(TARGET_MIYOO)
    sym = miyoo_remap_key(sym); /* MENU > CONTROLS (kit miyoo_remap.c); the letters above pass unchanged */
#endif
    switch (sym) {
    case 'u': key = K_UP; break;
    case 'd': key = K_DOWN; break;
    case 'l': key = K_LEFT; break;
    case 'r': key = K_RIGHT; break;
    case 'a': key = K_A; break;
    case 'b': key = K_B; break;
    case 'x': key = K_X; break;
    case 'y': key = K_Y; break;
    case 'm': key = K_L; break;
    case 'n': key = K_R; break;
    case 's': key = K_START; break;
    case 'k': key = K_LOOKDOWN; break;
    case 'o': key = K_LOOKUP; break;
#if defined(TARGET_MIYOO)
    /* Miyoo Mini SDL 1.2 keys (the stock / Onion SDL; none of them is one of the letters above,
     * which GE_PADSCRIPT keeps using): the handheld layout on the same face buttons, L2 to
     * look down (crouch while aiming) and R2 to look up. MENU (Escape) is the window
     * backend's; SELECT is free. */
    case 273: key = K_UP; break;    /* SDLK_UP */
    case 274: key = K_DOWN; break;  /* SDLK_DOWN */
    case 276: key = K_LEFT; break;  /* SDLK_LEFT */
    case 275: key = K_RIGHT; break; /* SDLK_RIGHT */
    case 32: key = K_A; break;      /* SDLK_SPACE: A */
    case 306: key = K_B; break;     /* SDLK_LCTRL: B */
    case 304: key = K_X; break;     /* SDLK_LSHIFT: X */
    case 308: key = K_Y; break;     /* SDLK_LALT: Y */
    case 'e': key = K_L; break;     /* L1 */
    case 't': key = K_R; break;     /* R1 */
    case 9: key = K_LOOKDOWN; break; /* SDLK_TAB: L2 */
    case 8: key = K_LOOKUP; break;   /* SDLK_BACKSPACE: R2 */
    case 13: key = K_START; break;  /* SDLK_RETURN: START */
#endif
    }
    hh_input_key(key, down != 0);
}

#if defined(TARGET_MIYOO)
/* MENU > CONTROLS: the layout above, in play (the watch and the menus move with it). */
const struct miyoo_remap_row miyoo_remap_rows[] = {
    { MR_A, MB_X, 0, 0, "NEXT WEAPON" },
    { MR_B, MB_A, 0, 0, "ACTION, RELOAD" },
    { MR_Z, MB_B, 0, 0, "FIRE" },
    { MR_R, MB_Y, 0, 0, "AIM (HOLD)" },
    { MR_START, MB_START, 0, 0, "PAUSE (THE WATCH)" },
    { MR_CUP, MB_R2, 0, 0, "LOOK UP" },
    { MR_CDOWN, MB_L2, 0, 0, "LOOK DOWN" },
    { MR_CLEFT, MB_L1, 0, 0, "STRAFE LEFT" },
    { MR_CRIGHT, MB_R1, 0, 0, "STRAFE RIGHT" },
    { MR_L, MB_NONE, 0, 0, "MENUS: L1" },
    { MR_STICK, MB_DPAD, 0, 0, "WALK, TURN" },
    { MR_DPAD, MB_NONE, 0, 0, "THE WATCH: D-PAD" },
};
const int miyoo_remap_nrows = (int)(sizeof(miyoo_remap_rows) / sizeof(miyoo_remap_rows[0]));
#endif

/* GE_PADSCRIPT (tests: menus, the watch, ... without hands): "<vi>:<keys>,<vi>:<keys>,..."
 * holds the handheld keys <keys> -- the OS letters above: u d l r a b x y, m = L, n = R,
 * s = START, k = L2, o = R2 -- from VI tick <vi> (GE_EXIT_AT's clock) until the next entry;
 * "<vi>:" lets go of everything. Example: "400:s,410:,700:a,710:" presses START, then A. */
#define HH_SCRIPT_MAX 128
static struct {
    u32 vi;
    char keys[16];
} sScript[HH_SCRIPT_MAX];
static int sScriptAt SAVESTATE_EXCLUDE;
static char sScriptHeld[16] SAVESTATE_EXCLUDE;

static void hh_input_script(void)
{
    if (sScriptN < 0) {
        const char *e = getenv("GE_PADSCRIPT");
        sScriptN = 0;
        while (e != NULL && *e && sScriptN < HH_SCRIPT_MAX) {
            char *end;
            int k = 0;
            sScript[sScriptN].vi = (u32)strtoul(e, &end, 10);
            if (end == e || *end != ':')
                break;
            for (e = end + 1; *e && *e != ','; e++)
                if (k < 15)
                    sScript[sScriptN].keys[k++] = *e;
            sScript[sScriptN++].keys[k] = 0;
            if (*e == ',')
                e++;
        }
    }
    while (sScriptAt < sScriptN && hh_vicount >= sScript[sScriptAt].vi) {
        const char *keys = sScript[sScriptAt++].keys, *p;
        for (p = sScriptHeld; *p; p++)
            if (strchr(keys, *p) == NULL)
                controller_handheld_key(*p, 0);
        for (p = keys; *p; p++)
            if (strchr(sScriptHeld, *p) == NULL)
                controller_handheld_key(*p, 1);
        strcpy(sScriptHeld, keys);
    }
}

/* Desktop proxy (gfx_sdl.c, SDL2 scancodes): the same layout on a keyboard.
 * Arrows = d-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R, Return = START, Tab = L2,
 * E = R2. */
void controller_sdl_key(int sc, bool down)
{
    int key = -1;
    switch (sc) {
    case 82: key = K_UP; break;    /* up */
    case 81: key = K_DOWN; break;  /* down */
    case 80: key = K_LEFT; break;  /* left */
    case 79: key = K_RIGHT; break; /* right */
    case 27: key = K_A; break;     /* X */
    case 29: key = K_B; break;     /* Z */
    case 22: key = K_X; break;     /* S */
    case 4: key = K_Y; break;      /* A */
    case 20: key = K_L; break;     /* Q */
    case 26: key = K_R; break;     /* W */
    case 40: key = K_START; break; /* Return */
    case 43: key = K_LOOKDOWN; break; /* Tab */
    case 8: key = K_LOOKUP; break;    /* E */
    }
    hh_input_key(key, down);
}

/* In a stage (not the title stage: menus, intro, cast) with the watch closed: the gameplay
 * layout. (Not current_menu: a GE_ARGS stage boot never passes the front end.) */
static bool hh_input_gameplay(void)
{
    return bossGetStageNum() != LEVELID_TITLE && g_CurrentPlayer != NULL && get_BONDdata_outside_watch_menu_flag();
}

/* In a stage with the watch (pause menu) open. */
static bool hh_input_watch(void)
{
    return bossGetStageNum() != LEVELID_TITLE && g_CurrentPlayer != NULL && !get_BONDdata_outside_watch_menu_flag();
}

/* Outside gameplay: A and B are the N64's, the rest as the menus expect. */
static void hh_input_menu_buttons(u16 *btn, bool look_down)
{
    if (sKey[K_A]) *btn |= A_BUTTON;
    if (sKey[K_B]) *btn |= B_BUTTON;
    if (sKey[K_X]) *btn |= Z_TRIG;
    if (sKey[K_Y]) *btn |= R_TRIG;
    if (sKey[K_L]) *btn |= L_TRIG;
    if (sKey[K_R]) *btn |= R_TRIG;
    if (look_down) *btn |= D_CBUTTONS;
    if (sKey[K_LOOKUP]) *btn |= U_CBUTTONS;
}

/* A turn direction held for this long: its deflection. */
static int hh_turn(int key, u64 now)
{
    u64 since = sDownNs[key];
    u64 ms = (now - since) / 1000000u;
    if (sTurnRampMs <= 0 || ms >= (u64)sTurnRampMs)
        return STICK_FULL;
    return sTurnStart + (int)((STICK_FULL - sTurnStart) * (s64)ms / sTurnRampMs);
}

/* Aiming: move the crosshair stick toward the held direction; it stays at the edge. */
static int hh_aim_axis(float *pos, bool neg, bool pos_key, float step)
{
    if (neg && !pos_key) {
        *pos -= step;
        if (*pos <= -AIM_EDGE) { *pos = -AIM_EDGE; return -STICK_FULL; } /* pushing past: turn */
    } else if (pos_key && !neg) {
        *pos += step;
        if (*pos >= AIM_EDGE) { *pos = AIM_EDGE; return STICK_FULL; }
    }
    return (int)*pos;
}

/* osContGetReadData: the pad the game sees this poll. */
void hh_input_poll(void)
{
    u64 now;
    float dt;
    u16 btn = 0;
    int sx = 0, sy = 0;

    hh_input_script();
    now = hh_input_ns();
    dt = sLastPollNs ? (float)(now - sLastPollNs) / 1e9f : 0.0f;
    if (!sTunablesRead) {
        const char *e;
        sTunablesRead = true;
        if ((e = getenv("GE_TURN_START")) != NULL) sTurnStart = atoi(e);
        if ((e = getenv("GE_TURN_RAMP_MS")) != NULL) sTurnRampMs = atoi(e);
        if ((e = getenv("GE_AIM_RATE")) != NULL) sAimRate = (float)atof(e);
    }
    if (dt > 0.1f) dt = 0.1f; /* after a pause or a load */
    sLastPollNs = now;
    const bool left = sKey[K_LEFT], down = sKey[K_DOWN];
    const bool right = sKey[K_RIGHT], up = sKey[K_UP];
    /* L2 on its own is C-down; with R2 it is not a look-down. */
    const bool look_down = sKey[K_LOOKDOWN] && !sKey[K_LOOKUP];

    if (hh_input_gameplay()) {
        if (sKey[K_Y]) {
            float step = sAimRate * dt;
            btn |= R_TRIG;
            sx = hh_aim_axis(&sAimX, left, right, step);
            sy = hh_aim_axis(&sAimY, down, up, step);
        } else {
            sAimX = sAimY = 0.0f;
            if (left && !right) sx = -hh_turn(K_LEFT, now);
            if (right && !left) sx = hh_turn(K_RIGHT, now);
            if (up && !down) sy = STICK_FULL;
            if (down && !up) sy = -STICK_FULL;
        }
        if (sKey[K_B]) btn |= Z_TRIG;
        if (sKey[K_A]) btn |= B_BUTTON;
        if (sKey[K_X]) btn |= A_BUTTON;
        if (sKey[K_L]) btn |= L_CBUTTONS;
        if (sKey[K_R]) btn |= R_CBUTTONS;
        if (look_down) btn |= D_CBUTTONS;
        if (sKey[K_LOOKUP]) btn |= U_CBUTTONS;
    } else if (hh_input_watch()) {
        /* The watch: the d-pad is the N64's d-pad. Its inventory list moves one item a press
         * and then 0.1 item a frame while held (options.c game_options_inventory_navigation),
         * the N64's own repeat; a full-tilt stick there (>= 0x47) moves an item every frame,
         * which made the list unusable from a d-pad. Every watch page takes the d-pad. */
        sAimX = sAimY = 0.0f;
        if (left && !right) btn |= L_JPAD;
        if (right && !left) btn |= R_JPAD;
        if (up && !down) btn |= U_JPAD;
        if (down && !up) btn |= D_JPAD;
        hh_input_menu_buttons(&btn, look_down);
    } else {
        sAimX = sAimY = 0.0f;
        if (left && !right) sx = -STICK_FULL;
        if (right && !left) sx = STICK_FULL;
        if (up && !down) sy = STICK_FULL;
        if (down && !up) sy = -STICK_FULL;
        hh_input_menu_buttons(&btn, look_down);
    }
    if (sKey[K_START]) btn |= START_BUTTON;

    hh_pad_buttons = btn;
    hh_pad_stick_x = (s8)sx;
    hh_pad_stick_y = (s8)sy;
}
