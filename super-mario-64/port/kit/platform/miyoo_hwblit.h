/* miyoo_hwblit: the game frame onto the panel by the SSD202D's 2D engine (SigmaStar MI_GFX) instead of the CPU
 * (experimental, off unless asked for: <GAME>_HWBLIT=1|2 or MIYOO_HWBLIT=1|2 in launch.sh, or hwblit=1|2 in the
 * port's settings file).
 *
 *  1 (nearest): the same picture as the CPU's: each game pixel a 2x2 block, turned 180 degrees. Used only when a
 *    check at start finds the engine's output identical to the CPU's, pixel for pixel; otherwise the CPU goes on.
 *  2 (smooth): the engine's own scaler (it blends neighbouring pixels), also for the Mini v4 / Flip full-height fill.
 *
 * libmi_sys.so / libmi_gfx.so are opened at run time (dlopen): a firmware without them, a failed allocation, a
 * check that does not match or a blit that fails all leave the CPU drawing, with one line in the log. Nothing in
 * here touches the picture unless the switch is on. tools/hwblit_test (built from this file) checks and times it
 * on the device. */
#ifndef MIYOO_HWBLIT_H
#define MIYOO_HWBLIT_H
#include <stdint.h>

/* The panel's back buffer as the port's SDL hands it out. phys_hint: SDL_Surface.unused1 (some Miyoo SDL builds
 * keep the buffer's physical address there; 0 if not). */
typedef struct {
    void *pixels;
    uint32_t phys_hint;
    int pitch, w, h;
} MiyooHwPanel;

/* The asked mode: 0 off, 1 nearest, 2 smooth. env: the port's own variable name ("SM64_HWBLIT"); cfg: the
 * settings file's value (-1 when none). The environment wins (the port's, then MIYOO_HWBLIT). */
int miyoo_hwblit_mode(const char *env, int cfg);

/* A w x h frame (0x??RRGGBB, alpha forced to amask) onto the panel rectangle (ox, oy, pw, ph) as the CPU would place
 * it (unturned coordinates; rot: turned 180 degrees). mode: miyoo_hwblit_mode(). 1: handed to the engine (src is
 * free again; the engine may still be drawing: miyoo_hwblit_sync before the flip or anything else that reads or writes
 * the panel); 0: not drawn, the caller draws with the CPU (the switch is off, the engine is not usable, or this frame's
 * size or scale is one the asked mode does not do). The first call with mode != 0 sets the engine up. */
int miyoo_hwblit_frame(int mode, const MiyooHwPanel *p, const uint32_t *src, int w, int h, int ox, int oy, int pw,
                       int ph, int rot, uint32_t amask);
/* Waits until the engine has finished the last frame handed to it (nothing to do when none is out). */
void miyoo_hwblit_sync(void);

/* 1 while the engine draws the frames (after the first frame's set-up) */
int miyoo_hwblit_active(void);

/* ---- for tools/hwblit_test ---- */
/* Opens the libraries and the engine (once). 1 ok. why: a short reason on failure. */
int miyoo_hwblit_open(const char **why);
/* Physically contiguous memory (cached), freed with miyoo_hwblit_free. 0 on failure. */
void *miyoo_hwblit_alloc(uint32_t bytes, uint64_t *phys);
void miyoo_hwblit_free(void *va, uint64_t phys, uint32_t bytes);
void miyoo_hwblit_flush(void *va, uint32_t bytes);
/* One blit. Surfaces are (phys, width, height, stride in bytes), ARGB8888; rects (x, y, w, h); rotate 0..3 (x 90
 * degrees), mirror 0 none 1 horizontal 2 vertical 3 both. 0 ok. */
int miyoo_hwblit_blit(uint64_t sp, int sw, int sh, int sstride, int sx, int sy, int srw, int srh, uint64_t dp, int dw,
                      int dh, int dstride, int dx, int dy, int drw, int drh, int rotate, int mirror);
/* The nearest 2x ways, each checked against the CPU at w x h (0: exact, -1: the engine failed, >0: pixels off);
 * how: 0 one scaled blit, 1 rows by stride + columns scaled, 2 through two quarter turns (columns by stride too). */
long miyoo_hwblit_check_x2(int how, int w, int h, int rot, int rotmirror, double *ms);
/* Which ways of turning 180 degrees the engine gets right at 1:1: bit 0 rotate 180, bit 1 mirror both; -1 no memory,
 * -2 every blit refused. */
int miyoo_hwblit_check_turn(void);
/* Where the panel's buffer is for the engine: 0 none; 1 the SDL surface's unused1; 2 /dev/fb0 mapped. desc: text. */
int miyoo_hwblit_locate(const MiyooHwPanel *p, uint64_t *phys, char *desc, int desclen);

/* Back to "not set up" (the next frame checks everything again); how >= 0: nearest uses that way at w x h, unchecked. */
void miyoo_hwblit_reset(int how, int w, int h);

#endif
