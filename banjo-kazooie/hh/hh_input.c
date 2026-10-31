/* hh_input.c: the handheld keys (the Miyoo Mini's, and BK_PADSCRIPT's letters) -> controller 1.
 *
 * Layout:
 *   d-pad   control stick (full deflection: Banjo runs); SELECT held: half deflection (walk, tiptoe)
 *   A       A (jump)              B       B (attack)
 *   L1      Z (crouch; with A/B the Z moves)      R1      R (camera behind Banjo)
 *   X       C-up (first person; with Z: eggs forward)
 *   Y       C-down (eggs backward; camera zoom)
 *   L2 / R2 C-left / C-right (camera; with Z: Talon Trot / Wonderwing)
 *   START   START (pause)         MENU    the kit's game menu (the window backend) */
#include <ultra64.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "hh_hw.h"
#include "hh_os.h"
#include "hh_plat.h"
#include "savestate.h" /* SAVESTATE_EXCLUDE: host state a state load must not overwrite (kit) */
#if defined(TARGET_MIYOO)
#include "miyoo_remap.h"
#endif

#define STICK_FULL 80 /* a full push of the N64 stick (Banjo-Kazooie reads +-80 as full) */
#define STICK_WALK 36 /* SELECT held */

enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_X, K_Y, K_L, K_R, K_START, K_L2, K_R2, K_SELECT, K_COUNT };

static bool sKey[K_COUNT] SAVESTATE_EXCLUDE;
static int sScriptN SAVESTATE_EXCLUDE = -1; /* BK_PADSCRIPT entries (below); -1 until read */

static void hh_input_key(int key, bool down)
{
    if (key < 0 || key >= K_COUNT)
        return;
    sKey[key] = down;
}

/* BK_PADSCRIPT's letters: d-pad u/d/l/r, A/B/X/Y = a/b/x/y, L1 = m, R1 = n, START = s, L2 = k, R2 = o, SELECT = w. */
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
    case 'k': key = K_L2; break;
    case 'o': key = K_R2; break;
    case 'w': key = K_SELECT; break;
#if defined(TARGET_MIYOO)
    /* the Miyoo Mini's SDL 1.2 keys (stock / Onion SDL); MENU (Escape) is the window backend's */
    case 273: key = K_UP; break;     /* SDLK_UP */
    case 274: key = K_DOWN; break;   /* SDLK_DOWN */
    case 276: key = K_LEFT; break;   /* SDLK_LEFT */
    case 275: key = K_RIGHT; break;  /* SDLK_RIGHT */
    case 32: key = K_A; break;       /* SDLK_SPACE: A */
    case 306: key = K_B; break;      /* SDLK_LCTRL: B */
    case 304: key = K_X; break;      /* SDLK_LSHIFT: X */
    case 308: key = K_Y; break;      /* SDLK_LALT: Y */
    case 'e': key = K_L; break;      /* L1 */
    case 't': key = K_R; break;      /* R1 */
    case 9: key = K_L2; break;       /* SDLK_TAB: L2 */
    case 8: key = K_R2; break;       /* SDLK_BACKSPACE: R2 */
    case 13: key = K_START; break;   /* SDLK_RETURN: START */
    case 305: key = K_SELECT; break; /* SDLK_RCTRL: SELECT */
#endif
    }
    hh_input_key(key, down != 0);
}

#if defined(TARGET_MIYOO)
/* MENU > CONTROLS: the layout above. */
const struct miyoo_remap_row miyoo_remap_rows[] = {
    { MR_A, MB_A, 0, 0, "JUMP" },
    { MR_B, MB_B, 0, 0, "ATTACK" },
    { MR_Z, MB_L1, 0, 0, "CROUCH, Z MOVES" },
    { MR_R, MB_R1, 0, 0, "CAMERA BEHIND" },
    { MR_CUP, MB_X, 0, 0, "FIRST PERSON" },
    { MR_CDOWN, MB_Y, 0, 0, "EGGS, ZOOM" },
    { MR_CLEFT, MB_L2, 0, 0, "CAMERA LEFT" },
    { MR_CRIGHT, MB_R2, 0, 0, "CAMERA RIGHT" },
    { MR_START, MB_START, 0, 0, "PAUSE" },
    { MR_TEXT, MB_SELECT, 0, 0, "WALK (HOLD)" },
    { MR_STICK, MB_DPAD, 0, 0, "MOVE" },
};
const int miyoo_remap_nrows = (int)(sizeof(miyoo_remap_rows) / sizeof(miyoo_remap_rows[0]));
#endif

/* BK_PADSCRIPT (tests): "<vi>:<keys>,<vi>:<keys>,..." holds <keys> (the letters above) from VI tick <vi> until the
 * next entry; "<vi>:" lets go of everything. Example: "400:s,410:,700:a,710:" presses START, then A. */
#define HH_SCRIPT_MAX 512
static struct {
    u32 vi;
    char keys[16];
} sScript[HH_SCRIPT_MAX];
static int sScriptAt SAVESTATE_EXCLUDE;
static char sScriptHeld[16] SAVESTATE_EXCLUDE;

static void hh_input_script(void)
{
    if (sScriptN < 0) {
        const char *e = getenv("BK_PADSCRIPT");
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

/* osContGetReadData: the pad the game sees this poll. */
void hh_input_poll(void)
{
    u16 btn = 0;
    int sx = 0, sy = 0, full;

    hh_input_script();
    full = sKey[K_SELECT] ? STICK_WALK : STICK_FULL;
    if (sKey[K_LEFT] && !sKey[K_RIGHT]) sx = -full;
    if (sKey[K_RIGHT] && !sKey[K_LEFT]) sx = full;
    if (sKey[K_UP] && !sKey[K_DOWN]) sy = full;
    if (sKey[K_DOWN] && !sKey[K_UP]) sy = -full;
    if (sx != 0 && sy != 0) { /* a diagonal: the stick's round gate, not the square's corner */
        sx = sx * 7 / 10;
        sy = sy * 7 / 10;
    }
    if (sKey[K_A]) btn |= A_BUTTON;
    if (sKey[K_B]) btn |= B_BUTTON;
    if (sKey[K_L]) btn |= Z_TRIG;
    if (sKey[K_R]) btn |= R_TRIG;
    if (sKey[K_X]) btn |= U_CBUTTONS;
    if (sKey[K_Y]) btn |= D_CBUTTONS;
    if (sKey[K_L2]) btn |= L_CBUTTONS;
    if (sKey[K_R2]) btn |= R_CBUTTONS;
    if (sKey[K_START]) btn |= START_BUTTON;

    hh_pad_buttons = btn;
    hh_pad_stick_x = (s8)sx;
    hh_pad_stick_y = (s8)sy;
}
