/* hh_screen.h: GoldenEye's perspectives for the output's shape (hh_screen.c). */
#ifndef HH_SCREEN_H
#define HH_SCREEN_H
#include <ultra64.h>

float hhScreenSqueeze(void);
int hhScreenFillWanted(void);
int hhBondviewFillActive(void); /* bondview2.c */
Gfx *hhGunProjectionBegin(Gfx *gdl);
Gfx *hhGunProjectionEnd(Gfx *gdl);
void hhScreenPerspective(float *fovy, float *aspect, int world);
void hhGuPerspective(Mtx *m, u16 *perspNorm, float fovy, float aspect, float near, float far, float scale, int world);
void hhGuPerspectiveF(float mf[4][4], u16 *perspNorm, float fovy, float aspect, float near, float far, float scale, int world);

/* HUD elements drawn 1:1 on a narrower-than-4:3 output instead of squeezed (gfx_pc.c
 * gfx_draw_rectangle; the user, 2026-09-29: "unsquash ammo and objective complete text box"):
 * LEFT = one block kept at its left margin (the bottom message), SIDE = each rectangle kept to
 * the screen edge it sits nearer (the ammo counters), INTRO = LEFT without the corner move (the
 * level intro's captions stay where the game puts them), TOP = the top message (GE_HUDMSG1TO1, 2026-10-04).
 * Also in hh/gfx/port.h. */
#define HH_TAG_GEHUD_OFF 0x47454830u  /* 'GEH0' */
#define HH_TAG_GEHUD_LEFT 0x47454831u /* 'GEH1' */
#define HH_TAG_GEHUD_SIDE 0x47454832u /* 'GEH2' */
#define HH_TAG_GEHUD_INTRO 0x47454833u /* 'GEH3': LEFT for the intro's captions, at the game's own margin */
#define HH_TAG_GEHUD_TOP 0x47454834u   /* 'GEH4': the top message (bondview2.c): its text 1:1 as placed and broken there, its full-width box full width */
/* The first-person models (gun, hands, the pause's watch arm): never taken for the sky (round 38). */
#define HH_TAG_FPMODEL_OFF 0x47454630u /* 'GEF0' */
#define HH_TAG_FPMODEL_ON 0x47454631u  /* 'GEF1' */
int hhHud1to1(void);
int hhHudMsgTop1to1(void);
int hhHudMsgTopLeft(void);
int hhHudMsgTopRoom(void);
int hhIntroTextHold(void);

#endif
