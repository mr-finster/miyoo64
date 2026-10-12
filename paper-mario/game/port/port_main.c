/* The game's side of the native port's main loop (M2). Game side: the decomp's headers. boot_main (main.c) ends in
 * an endless loop waiting for nusys's retrace thread, so the port does its work here instead: the same setup calls in
 * the same order, then one retrace at a time. Paper Mario steps and draws on every second retrace (30 fps,
 * gfxRetrace_Callback toggles D_80073E0A), so a game frame is two retraces. */
#include "common.h"
#include "nu/nusys.h"
#include "port.h"
#include "game_modes.h"

void gfxRetrace_Callback(s32 gfxTaskNum);
void gfxPreNMI_Callback(void);
void create_audio_system(void);
void load_engine_data(void);
void crash_screen_init(void);
void is_debug_init(void);

void port_game_boot(void) {
    osViSetMode(&osViModeNtscLan1);
    osViSetSpecialFeatures(OS_VI_GAMMA_OFF | OS_VI_GAMMA_DITHER_OFF | OS_VI_DIVOT_ON | OS_VI_DITHER_FILTER_ON);
    nuGfxDisplayOff();
    nuGfxInit();
    gGameStatusPtr->contBitPattern = nuContInit();
    nusched.retraceCount = 1; /* nuScCreateScheduler's: the audio frame size is worked out from it */
    create_audio_system();
    port_audio_boot();
    load_engine_data();
    nuGfxFuncSet((NUGfxFunc) gfxRetrace_Callback);
    nuGfxPreNMIFuncSet(gfxPreNMI_Callback);
    gRandSeed = port_debug_seed(gRandSeed + osGetCount());
    nuGfxDisplayOn();
    port_log("game: booted\n");
}

/* one retrace: nusys's graphics thread calls the game's callback with the number of queued tasks (always 0 here) */
void port_game_retrace(void) {
    if (nuGfxFunc != NULL) nuGfxFunc(nuGfxTaskSpool);
    port_audio_out_retrace();
    port_audio_retrace();
}

/* The intro logos (the N64 cube, Nintendo, Intelligent Systems): state_logos.c draws them in the middle of the
 * 320-wide screen on a flat fill, the widest (Nintendo) 163 px, so the panel shows the middle 240 columns 1:1 while
 * they are up instead of squeezing the frame (port_gfx.c's port_panel_row). The curtains and the stage come in
 * behind the Intelligent Systems logo as it fades (startup state 9, LOGOS_STATE_IS_FADE_OUT, its fade 0..255): the
 * view widens with that fade to the whole squeezed frame, so the stage's edges come into view as a pull-back rather
 * than a jump. 0 = 1:1, 255 = the whole frame squeezed. */
int port_logos_view(void) {
    if (get_game_mode() != GAME_MODE_LOGOS || gGameStatusPtr->startupState > 9) return 255;
    if (gGameStatusPtr->startupState < 9) return 0;
    const s32 a = gGameStatusPtr->screenColorA;
    return a < 0 ? 0 : a > 255 ? 255 : a;
}

/* The scene's black frame. background_gfx.c clears everything outside the camera's viewport to black: 12 px at the
 * sides, 20 at the top and bottom in the field (296x200 of 320x240), the N64's overscan margin, which the real game
 * shows too on an emulator (z64) and a TV mostly hid. Battles, the chapter cards and the menus that draw no such
 * frame fill the screen. The panel stretches the scene over the standard frame (port_gfx.c, "always stretch", the
 * user's pick for every game, 2026-09-29) when this frame drew it; the status bar (top at y 18) and the speech bubble
 * (y 16), which reach into the top margin, move down 2 and 4 px to stay inside. PM64_STRETCH=0: the black frame as
 * before. */
char *getenv(const char *);
static s32 sStretch = -1;
static s32 sLbX0, sLbY0, sLbX1, sLbY1;
static u32 sLbFrame = 0xFFFFFFFF;
static s32 stretch_on(void) {
    if (sStretch < 0) { const char *e = getenv("PM64_STRETCH"); sStretch = !(e != NULL && e[0] == '0'); }
    return sStretch;
}
void port_letterbox_set(s32 x0, s32 y0, s32 x1, s32 y1) {
    sLbX0 = x0; sLbY0 = y0; sLbX1 = x1; sLbY1 = y1;
    sLbFrame = port_frame;
}
void port_letterbox_keep(void) { sLbFrame = port_frame; }
s32 port_hud_shift(s32 px) { return stretch_on() ? px : 0; }
extern f32 gCurtainScale; /* curtains.c: the theatre curtains (the logos' end, the chapter cards) cover the whole
                          * screen, frame and all, while it is below 1.9 */
int port_letterbox_standard(void) {
    return stretch_on() && sLbFrame == port_frame && sLbX0 == 12 && sLbY0 == 20 && sLbX1 == 308 && sLbY1 == 220 &&
           gCurtainScale >= 1.9f;
}
