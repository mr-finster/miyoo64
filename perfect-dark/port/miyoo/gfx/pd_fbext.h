/* pd_fbext.h -- the Perfect Dark PC port's framebuffer GBI extensions on the soft renderer.
 * Included once, inside gfx_pc.c (it reaches the interpreter's static state).
 *
 * Stage 1: videoFramebuffersSupported() returns 0 (port/miyoo/video_miyoo.c), so the game takes
 * its no-framebuffer paths (no blur captures, no fisheye/night-vision copies) and these see no
 * targets. TODO: offscreen targets in gfx_soft (RGBA16 surfaces) for the menu blur and the
 * eyespy / night vision copies. */
#ifndef PD_FBEXT_H
#define PD_FBEXT_H

/* Offscreen framebuffers (video_miyoo.c): only small fixed-size ones exist (chr.c's 16x16 shield-hit
 * shimmer copies); screen-sized ones stay unsupported, so the game skips its full-screen effects. */
extern uint8_t *videoFbPixels(int fb, int *w, int *h);

/* G_SETTIMG_FB_EXT: sample an offscreen target as the texture. The game loads the "texture" first
 * with the framebuffer's id as its address (chr.c gDPLoadTextureBlock(var8009ccc0[i]...)): those
 * loads are pointed at the framebuffer's pixels. An unknown id leaves them at the id, which the
 * importer rejects (never a read of address 0xFFFFFFFF, TEST2's crash with simulants). */
static void pd_fb_texture(int fb) {
    int w, h, i;
    uint8_t *px = videoFbPixels(fb, &w, &h);
    if (!px) return;
    for (i = 0; i < GFX_LT_SLOTS; i++) {
        if (rdp.loaded_texture[i].addr == (const uint8_t *) (uintptr_t) (uint32_t) fb) {
            rdp.loaded_texture[i].addr = px;
            rdp.loaded_texture[i].size_bytes = (uint32_t) (w * h * 2);
            rdp.loaded_texture[i].stride_bytes = 0;
        }
    }
    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

/* A screen copy as the texture (video_miyoo.c ids 100, 101: bondview.c's motion blur when Joanna is
 * punched or drugged). The game draws it full-view with TEXEL0 x ENV, alpha ENV, CLD_SURF (a blend):
 * done here directly on the soft target, the saved frame over the current one. */
static int pd_scr_bound SAVESTATE_EXCLUDE = -1;
static void pd_scr_rectangle(int slot, int iw, int ih, int x0, int y0, int s0, int t0, int x1, int y1, int s1, int t1) {
    extern void gfx_soft_scr_blend(int slot, float x0, float y0, float x1, float y1, float s0, float t0, float s1,
                                   float t1, int iw, int ih, int r, int g, int b, int a);
    static int on = -1; /* PD_SCRBLUR=0: not drawn (A/B) */
    if (on < 0) { const char *e = getenv("PD_SCRBLUR"); on = !(e && e[0] == '0'); }
    if (!on) return;
    gfx_soft_scr_blend(slot, x0 / 4.0f, y0 / 4.0f, x1 / 4.0f, y1 / 4.0f, (float) s0, (float) t0, (float) s1, (float) t1,
                       iw, ih, rdp.env_color.r, rdp.env_color.g, rdp.env_color.b, rdp.env_color.a);
}

/* G_SETFB_EXT: draw into an offscreen target (0 = the screen). */
static void pd_set_framebuffer(int fb) { (void) fb; }

/* G_COPYFB_EXT: copy the screen (src 0), what is drawn so far, into a target at (left, top). */
static void pd_copy_framebuffer(int dst, int src, int left, int top) {
    extern void gfx_soft_read_rgba16(int x, int y, int w, int h, uint8_t *dst);
    int w, h;
    uint8_t *px;
    if (dst >= 100 && dst < 102 && src == 0) { /* camspy's lens: what is drawn so far, mid-frame */
        extern void gfx_soft_scr_capture_now(int slot);
        gfx_soft_scr_capture_now(dst - 100);
        return;
    }
    px = videoFbPixels(dst, &w, &h);
    if (!px || src != 0) return;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    gfx_soft_read_rgba16(left, top, w, h, px);
}

/* G_CLEAR_DEPTH_EXT: PD clears depth before the world and again before the first-person gun
 * (bondgun.c -> viPrepareZbuf); the second keeps the gun in front of walls (TEST1: "the weapon
 * clips through walls"). */
static void pd_clear_depth(void) {
    static int on = -1; /* PD_GUNDEPTH=0: off (A/B) */
    if (on < 0) { const char *e = getenv("PD_GUNDEPTH"); on = !(e && e[0] == '0'); }
    if (on) { extern void gfx_soft_clear_depth_mid(void); gfx_soft_clear_depth_mid(); }
}

/* G_IMAGERECT_EXT: a textured rectangle whose texture is iw x ih, screen (x0,y0)-(x1,y1) in 10.2,
 * texels (s0,t0)-(s1,t1). Drawn as a G_TEXRECT with the matching steps. */
static void pd_image_rectangle(int tile, int iw, int ih, int x0, int y0, int s0, int t0, int x1, int y1, int s1,
                               int t1) {
    int dx = x1 - x0, dy = y1 - y0;
    int16_t dsdx, dtdy;
    (void) ih;
    if (dx <= 0 || dy <= 0) return;
    if (tile == 0) rdp.texture_tile.line_size_bytes = iw << rdp.texture_tile.siz >> 1;
    dsdx = (int16_t) (((s1 - s0) * 1024 * 4) / dx); /* 5.10 per pixel; dx is 10.2 */
    dtdy = (int16_t) (((t1 - t0) * 1024 * 4) / dy);
    gfx_dp_texture_rectangle(x0, y0, x1, y1, tile, (int16_t) (s0 * 32), (int16_t) (t0 * 32), dsdx, dtdy, false);
}

#endif
