/* The handheld output size (the port kit K8): 320x240, doubled to the Miyoo Mini's panel. A
 * screen= line in dkr_display.cfg in the data dir (the app folder) can ask for the square size
 * instead, boxed in the middle. Read once, before the first SDL_SetVideoMode, and fixed for the
 * run. */
#pragma once

int handheld_screen_w(void);
int handheld_screen_h(void);
int handheld_screen_wide(void); /* 1 = 320x240 */
/* After SDL_SetVideoMode: the size the video surface really came up at wins (a panel that can't
 * do 320x240 falls back to the square size, and the render, present and menu follow it). */
void handheld_screen_confirm(int w, int h);
/* A device-bench switch in the same file (round 2): a line of its own
 *     neon=0
 * runs the scalar code where NEON has a runtime switch -- the raster spans (gfx_soft, as
 * MK64_NEON=0), the audio envelope mixer (MK64_ENVNEON=0) and the present conversion -- so a
 * bench can compare both on the A7 (F-Zero X's NEON paths were slower than scalar there).
 * -1: no line (the build's default); 0, 1 or 2: gfx_soft's MK64_NEON modes. */
int handheld_cfg_neon(void);
/* The race HUD (v38, the user's pick): unsquished by default -- every element at its true shape
 * around the place the squeeze gives it, TIME and the lap counter squeezed (gfx_pc.c
 * hud_fit_mode). A line of its own
 *     hud=0
 * brings back the squeeze everywhere, as before v38. -1: no line. */
int handheld_cfg_hud(void);
/* fillrule=<0|1> in the settings file: -1 absent (gfx_pc: DKR_FILLRULE, 1 = owned edges for blended draws without AA) */
int handheld_cfg_fillrule(void);
/* vdsoclock=0|1: DKR_VDSOCLOCK, the clock read through the kernel's vDSO (main.c; the environment wins). -1: no line (on). */
int handheld_cfg_vdsoclock(void);
/* The frame pace (decomp/src/video.c fb_update): a line of its own
 *     fps=60
 * shows every frame as soon as it is drawn (the game advances by the 60 Hz fields that passed), where
 * the default holds a frame that took 16.7..33 ms to 30 fps. -1: no line (fps=30, the hold). DKR_FPS=30/60 wins. */
int handheld_cfg_fps(void);
/* logverbose=1: DKR_LOGVERBOSE, the full logging of before 2026-10-05 (a PERF line every 150 iterations, not every 750). -1: no line. */
int handheld_cfg_logverbose(void);
#if defined(MIYOO)
/* Miyoo Mini (miyoo_video.c): a line of its own
 *     rotate180=0
 * draws the picture unturned. 1 (default): turned 180 degrees, as the Mini's panel needs.
 * DKR_ROTATE=0/1 in the environment wins. */
int handheld_cfg_rotate180(void);
/* Miyoo Mini (miyoo_worker.c, the second core): a line of its own
 *     threads=1
 * draws on one core, as before (2: both, the default). -1: no line. DKR_THREADS=1/2 wins. */
int handheld_cfg_threads(void);
/* Miyoo Mini (miyoo_clock.c, the CPU clock): a line of its own
 *     cpuclock=1200
 * runs the CPU at that many MHz (400..2000); 0 leaves the clock as the OS set it. -1: no line, the
 * default: 1500 MHz on the Mini, 1800 on the Mini Plus. DKR_CPUCLOCK=<MHz> wins. */
int handheld_cfg_cpuclock(void);
#endif
