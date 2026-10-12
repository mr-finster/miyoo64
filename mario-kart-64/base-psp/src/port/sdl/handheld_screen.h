/* The handheld output size (the port kit K8): 320x240, doubled to the Miyoo Mini's panel. A
 * screen= line in mk64_display.cfg in the data dir (the app folder) can ask for the square size
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
/* The 3D at its true shape (v49, gfx_pc.c mp_update): on by default; a line of its own
 *     unsquish3d=0
 * brings back the 4:3 view squeezed to 240 wide, as before v49. Returns 0 or 1. */
int handheld_cfg_unsquish3d(void);
/* Owned edges (2026-10-03, gfx_pc.c / gfx_soft.c MK64_FILLRULE): on by default; a line of its own
 *     fillrule=0
 * brings back the inclusive row solve for see-through draws without AA (a pixel on an edge two of their triangles
 * share drawn twice). -1: no line. */
int handheld_cfg_fillrule(void);
int handheld_cfg_logverbose(void); /* logverbose=1: MK64_LOGVERBOSE, the full logging of before 2026-10-05 (-1 absent) */
int handheld_cfg_nearw(void);  /* nearw=0/1: MK64_NEARW, the w plane at a near plane nearer than 2 (-1 absent) */
/* The exact speed switches (2026-10-04; the same pixels either way, so a device A/B decides them): lines of their own
 *     vtxfast=0|1|ab    vdsoclock=0|1|ab    spancvt=0|1|ab
 * (MK64_VTXFAST, MK64_VDSOCLOCK, MK64_SPANCVT; the environment wins). ab alternates off and on every PERF window and logs "AB ..."
 * under each PERF line. Returns 0, 1, 2 (ab) or -1 (no line: vdsoclock on; vtxfast and spancvt off since v1.5.0). */
int handheld_cfg_vtxfast(void);
int handheld_cfg_vdsoclock(void);
int handheld_cfg_spancvt(void);
/* The cross-port speed round (2026-10-07, gfx_soft.c; exact, default on): lines of their own
 *     fastrows=0|1|ab    spanvec=0|1|ab    shadepx=0|1|ab    zskip=0|1|ab    wraprr=0|1|ab    speedbench=0|1
 * (MK64_FASTROWS, MK64_SPANVEC, MK64_SHADEPX, MK64_ZSKIP, MK64_WRAPRR, MK64_SPEEDBENCH; the environment wins).
 * k: 3 fastrows .. 7 wraprr, 8 speedbench (0..2 are the three above). Returns 0, 1, 2 (ab) or -1 (no line). */
int handheld_cfg_speed(int k);
#if defined(MIYOO)
/* Miyoo Mini (miyoo_video.c): a line of its own
 *     rotate180=0
 * draws the picture unturned. 1 (default): turned 180 degrees, as the Mini's panel needs. */
int handheld_cfg_rotate180(void);
#endif
#if defined(MIYOO)
/* Miyoo Mini (miyoo_worker.c, the second core): a line of its own
 *     threads=1
 * draws on one core, as before (2: both, the default). -1: no line. MK64_THREADS=1/2 wins. */
int handheld_cfg_threads(void);
/* Miyoo Mini (miyoo_clock.c, the CPU clock): a line of its own
 *     cpuclock=1200
 * runs the CPU at that many MHz (400..2000); 0 leaves the clock as the OS set it. -1: no line, the
 * default: 1500 MHz on the Mini, 1800 on the Mini Plus. MK64_CPUCLOCK=<MHz> wins. */
int handheld_cfg_cpuclock(void);
#endif
