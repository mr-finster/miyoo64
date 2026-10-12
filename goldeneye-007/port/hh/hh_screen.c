/* hh_screen.c: GoldenEye's picture on the output's shape (SPIKE.md Q4).
 *
 * The kit maps the N64 frame (320x240, or 440x330 in menus) onto the output proportionally,
 * so a 4:3 frame shows squeezed by k = (W / H) / (4 / 3): 0.75 on a square output, 1 in
 * a 4:3 window. Every perspective GE builds is corrected here, in the game's own terms --
 * aspect times k, the vertical FOV widened to keep the horizontal view the game asked for --
 * so everything GE draws in screen space from its own projection (the sky, the crosshair,
 * the watch) stays on the world. (The kit's own x stretch for narrow outputs is off for GE.)
 *
 * The front end's folder screens are the exception: their 2D text sits on their 3D pages,
 * so the 3D stays the N64's, squeezed with the 2D (hhFrontFolderMenu, front.c).
 *
 * Fill (the square output's default, and the Miyoo Mini's 4:3; GE_SCREEN=n64 turns it off): 1-player
 * gameplay at the default screen size also shows the 10-row bands above and below the N64's 320x220 view.
 * The viewport is the whole 320x240 (bondview2.c) and the projection keeps the N64 view's
 * 80 degree horizontal FOV across the square, 80 degrees high. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "hh_screen.h"
#include "gfx/gfx_pc.h"

int hhBondviewFillActive(void); /* bondview2.c */
int hhFrontFolderMenu(void);    /* front.c */

float hhScreenSqueeze(void)
{
    const float w = (float)gfx_current_dimensions.width, h = (float)gfx_current_dimensions.height;
    if (w <= 0.0f || h <= 0.0f)
        return 1.0f;
    return (w / h) * 0.75f;
}

/* The 1:1 HUD (hh_screen.h HH_TAG_GEHUD_*) is on: a narrower-than-4:3 output and GE_HUD1TO1 not 0. */
int hhHud1to1(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GE_HUD1TO1");
        on = !(e && e[0] == '0');
    }
    return on && hhScreenSqueeze() < 0.99f;
}

/* The top message (the mission's lines: "Guard: Drop that weapon NOW!", "Plane destroyed!"; bondview2.c
 * sub_GAME_7F08AAE8) at 1:1 too (the user, 2026-10-04: "unsquash the message box, move the text left so
 * the longest messages fit"): the 1:1 HUD on and GE_HUDMSG1TO1 not 0. GE_HUDMSG1TO1=0: squeezed into
 * the screen's width with the box, as before. */
int hhHudMsgTop1to1(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GE_HUDMSG1TO1");
        on = !(e && e[0] == '0');
    }
    return on && hhHud1to1();
}

/* The level intro's captions ("Byelomorye Dam, Arkangelsk, USSR", "Nine years ago") keep their place while
 * they fade out (the user, 2026-10-04: "if i skip goldeneye intro, the dam / 9 years text moves y axis"):
 * a skip -- or the last intro camera's end -- turns CAMERAMODE_INTRO into CAMERAMODE_FADESWIRL, the
 * captions still on screen, and bondview2.c tagged them as the bottom message from then on, 8 rows lower
 * with the corner move. On (GE_INTROTEXT not 0): FADESWIRL's captions keep the intro's tag. The N64 keeps
 * them in place. GE_INTROTEXT=0: as before. */
int hhIntroTextHold(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GE_INTROTEXT");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* GE_HUDCORNER (gfx_pc.c): the bottom message 8 px from the left edge, or the game's 30 */
static int hhHudCorner(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GE_HUDCORNER");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* The top message's left edge in the game's 1:1 pixels: the bottom message's (the corner's 8, or the
 * game's own 30 with GE_HUDCORNER=0) */
int hhHudMsgTopLeft(void)
{
    return hhHudCorner() ? 8 : 30;
}

/* The width a line of the top message may take at 1:1: the output's width in the game's pixels (320
 * squeezed by hhScreenSqueeze: 240 on the handheld) less the left edge and a right margin (as far from
 * the right as from the left with the corner, 4 px without, as gfx_pc.c's bottom message). */
int hhHudMsgTopRoom(void)
{
    return (int) (320.0f * hhScreenSqueeze() + 0.5f) - hhHudMsgTopLeft() - (hhHudCorner() ? 8 : 4);
}

int hhScreenFillWanted(void)
{
    static int wanted = -1;
    if (wanted < 0) {
        const char *e = getenv("GE_SCREEN");
        wanted = !(e != NULL && strcmp(e, "n64") == 0);
    }
#if defined(TARGET_MIYOO)
    /* the Miyoo Mini's 4:3 panel too: the N64's bands showed as black bars top and bottom there, the one
     * game on the Mini that didn't fill the screen (the user, 2026-10-09). The same fill as the square's:
     * the 320x240 viewport, the N64 view's horizontal FOV, 240/220 more of the world up and down. */
    return wanted;
#else
    return wanted && hhScreenSqueeze() < 0.99f;
#endif
}

/* tan and atan without libm: series in double, rounded to float. The desktop's glibc and the
 * Handheld's musl differ in the last bit of tanf/atanf (and of tan/atan), which moved every
 * projection between the desktop proxy and the device (round 12, the lead's x86 = ARM check;
 * the handheld's musl is wrong outright elsewhere, see ge_math.c). Only +, -, *, / and sqrt,
 * which both round exactly, so the two builds agree by construction; accurate to ~1e-16, so
 * the float results are the correctly rounded ones (the same as glibc's for every fovy from 1
 * to 179 degrees in 0.01 steps). tan: |x| < pi / 2 (x is half a FOV). */
static float hhTan(float xf)
{
    const double x = xf, x2 = x * x;
    double s = x, c = 1.0, ts = x, tc = 1.0;
    int k;
    for (k = 1; k <= 12; k++) { /* the 25th and 24th powers over their factorials: < 1e-20 */
        ts *= -x2 / (double)((2 * k) * (2 * k + 1));
        tc *= -x2 / (double)((2 * k - 1) * (2 * k));
        s += ts;
        c += tc;
    }
    return (float)(s / c);
}

static float hhAtan(float tf)
{
    double t = tf < 0.0f ? -(double)tf : (double)tf, r, t2, term;
    int k, big = t > 1.0;
    if (big)
        t = 1.0 / t;
    t = t / (1.0 + sqrt(1.0 + t * t)); /* atan(t) = 2 atan(t / (1 + sqrt(1 + t^2))), twice: */
    t = t / (1.0 + sqrt(1.0 + t * t)); /* |t| <= tan(pi / 16) < 0.2 */
    t2 = t * t;
    r = t;
    term = t;
    for (k = 1; k <= 20; k++) { /* 0.2^41 / 41: < 1e-29 */
        term *= -t2;
        r += term / (double)(2 * k + 1);
    }
    r *= 4.0;
    if (big)
        r = 1.5707963267948966 - r; /* pi / 2 */
    return (float)(tf < 0.0f ? -r : r);
}

/* world: the gameplay view (the world's projection and culling, the watch on the wrist) */
void hhScreenPerspective(float *fovy, float *aspect, int world)
{
    const float k = hhScreenSqueeze();
    float t;
    if (hhFrontFolderMenu())
        return; /* the folder screens: their 3D squeezed with their 2D (front.c) */
    if (world == 2 && hhBondviewFillActive()) {
        /* the watch (bondviewRenderWatch): its text is 2D at the N64's rows, 1:1 down the square,
         * and the N64 drew the face in a 320x220 view 10 rows down a 240-row screen. Stretched to
         * those rows -- the N64's vertical view over 220 of 240 rows, its horizontal across the
         * width -- the face and the text line up again; drawn round (world == 1), the face was 3/4
         * of its N64 height and the text sat over its top and bottom edges (the user, round 17). */
        t = hhTan(*fovy * (float)(M_PI / 360.0)) * (240.0f / 220.0f);
        *aspect = 320.0f / 240.0f;
    } else if (world && hhBondviewFillActive()) {
        /* the N64 view's horizontal FOV (320x220 at this fovy) across a 320x240 viewport */
        const float a = k * (320.0f / 240.0f);
        t = hhTan(*fovy * (float)(M_PI / 360.0)) * (320.0f / 220.0f) / a;
        *aspect = a;
    } else {
        if (k > 0.999f && k < 1.001f)
            return; /* a 4:3 output: exactly the N64's matrices */
        t = hhTan(*fovy * (float)(M_PI / 360.0)) / k;
        *aspect *= k;
    }
    *fovy = hhAtan(t) * (float)(360.0 / M_PI);
}

void hhGuPerspective(Mtx *m, u16 *perspNorm, float fovy, float aspect, float near, float far, float scale, int world)
{
    hhScreenPerspective(&fovy, &aspect, world);
    guPerspective(m, perspNorm, fovy, aspect, near, far, scale);
}

void hhGuPerspectiveF(float mf[4][4], u16 *perspNorm, float fovy, float aspect, float near, float far, float scale, int world)
{
    hhScreenPerspective(&fovy, &aspect, world);
    guPerspectiveF(mf, perspNorm, fovy, aspect, near, far, scale);
}
