#if defined(TARGET_PSP) || defined(TARGET_HANDHELD)
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#if defined(TARGET_PSP)
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include "pspmath.h"
#else
/* GU pixel-format constants the N64->backend importer passes as upload_texture `type`
 * (same numeric values pspgu.h uses). The software rasterizer normalizes on upload. */
#define GU_PSM_5650 0
#define GU_PSM_5551 1
#define GU_PSM_4444 2
#define GU_PSM_8888 3
#define GU_PSM_T4   4
#define GU_PSM_T8   5
#define GU_PSM_T16  6
#define GU_PSM_T32  7
#endif

#include "gfx_pc.h"
#include "gfx_cc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "gfx_screen_config.h"
#include "macros.h"
#include "port.h"
#ifdef TARGET_HANDHELD
#include "savestate.h" /* SAVESTATE_EXCLUDE for the tuning knobs below */
#else
#define SAVESTATE_EXCLUDE
#endif

/* Full GE hardware transform+light+cull offload. Implies PORT_GE_CULL (no CPU
 * cull/clip): additionally feeds OBJECT-space vertices and sets GU_MODEL to the
 * N64 modelview, so the GE does modelview*projection*clip*cull like a native
 * PSP game. Frees the CPU of ~11ms transform + ~8ms cull per heavy frame. */
#if defined(PORT_GE_TL) && !defined(PORT_GE_CULL)
#define PORT_GE_CULL
#endif
extern int gfx_trace_frames;
extern int gfx_debug_frame;
extern int gfx_dump_textures;
int gfx_colorflush; /* debug frame: paint every GE batch in a unique flat colour (PORT_GFX_DEBUG) */
extern uint32_t port_time_us(void);
extern void port_profile_add(int slot, uint32_t us);
#if !defined(TARGET_PSP)
#define sceKernelLibcClock() port_time_us() /* microsecond profiling clock */
#endif
uint32_t gfx_prof_cmds, gfx_prof_tris;
uint32_t gfx_prof_vtx, gfx_prof_cullclip, gfx_prof_state, gfx_prof_emit, gfx_prof_flush;
uint32_t gfx_prof_neareye;
uint32_t gfx_prof_tri1calls, gfx_prof_clipcalls, gfx_prof_vtxcount;
uint32_t gfx_prof_opcount[256];
extern uint16_t gDebugKartTex[2][64 * 32];
extern int gDebugKartTexCount;
extern uint16_t gDebugTex32[32 * 32];
extern int gDebugTex32Valid;
extern void port_log(const char* fmt, ...);

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define INFO_MSG(x) printf("%s %s\n", __FILE__ ":" TOSTRING(__LINE__), x)
#define _UNUSED(x) (void)(x)

#define SUPPORT_CHECK(x) do { static int warned; if (!(x) && !warned) { warned = 1; port_log("gfx: unsupported: %s (%s:%d)\n", #x, __FILE__, __LINE__); } } while (0)

// align value to N-byte boundary
#define ALIGN(VAL_, ALIGNMENT_) (((VAL_) + ((ALIGNMENT_) - 1)) & ~((ALIGNMENT_) - 1))

// SCALE_M_N: upscale/downscale M-bit integer to N-bit
#define SCALE_5_8(VAL_) (((VAL_) * 0xFF) / 0x1F)
#define SCALE_8_5(VAL_) ((((VAL_) + 4) * 0x1F) / 0xFF)
#define SCALE_4_8(VAL_) ((VAL_) * 0x11)
#define SCALE_8_4(VAL_) ((VAL_) / 0x11)
#define SCALE_3_8(VAL_) ((VAL_) * 0x24)
#define SCALE_8_3(VAL_) ((VAL_) / 0x24)

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define HALF_SCREEN_WIDTH (SCREEN_WIDTH / 2)
#define HALF_SCREEN_HEIGHT (SCREEN_HEIGHT / 2)

#define RATIO_X (gfx_current_dimensions.width / (2.0f * HALF_SCREEN_WIDTH))
#define RATIO_Y (gfx_current_dimensions.height / (2.0f * HALF_SCREEN_HEIGHT))

#define MAX_BUFFERED (1024)
#ifdef PM64
#define MAX_LIGHTS 7 /* F3DZEX2: up to 7 directional lights + ambient */
#else
#define MAX_LIGHTS 2
#endif
#define MAX_VERTICES 64

/* Pixel Formats */
#define GU_PSM_5650		(0) /* Display, Texture, Palette */
#define GU_PSM_5551		(1) /* Display, Texture, Palette */
#define GU_PSM_4444		(2) /* Display, Texture, Palette */
#define GU_PSM_8888		(3) /* Display, Texture, Palette */
#define GU_PSM_T4		(4) /* Texture */
#define GU_PSM_T8		(5) /* Texture */
#define GU_PSM_T16		(6) /* Texture */
#define GU_PSM_T32		(7) /* Texture */
extern void* getStaticVramTexBuffer(unsigned int width, unsigned int height, unsigned int psm);
extern void gfx_scegu_draw_triangles_2d(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris);
extern float identity_matrix[4][4];

struct RGBA {
    uint8_t r, g, b, a;
} __attribute__((packed, aligned(4)));

/* SIGNED: MK64 animates viewports off the screen edge (the driver's-points
 * panels slide in from / out to negative x and y); as uint16 a viewport at
 * y=-4 became 65532, its vertices projected off-screen and the whole panel
 * vanished for the frames it straddled the edge (round 6b: "the quadrant
 * loses the buffer"). */
struct XYWidthHeight {
    int16_t x, y, width, height;
} __attribute__((packed, aligned(4)));

struct LoadedVertex {
    float x, y, z, w;
    float _x, _y, _z, _w;
    float u, v;
    struct RGBA color;
    uint32_t clip_rej;
} __attribute__((packed, aligned(16)));

typedef struct VertexColor {
	short u, v; /* S10.5: texrects may start left of / above texel 0 (ssb64 4843c2c) */
	struct RGBA color;
	unsigned short x, y, z;
} VertexColor __attribute__((aligned(16)));

struct TextureHashmapNode {
    struct TextureHashmapNode *next;
    
    const uint8_t *texture_addr;
    uint8_t fmt, siz;
    uint32_t content_hash; // texels (+ palette): MK64 rewrites texture buffers in place
    uint32_t last_used_frame;
    
    uint32_t texture_id;
    uint8_t cms, cmt;
    bool linear_filter;
    uint8_t mirror; // texels imported doubled with the mirrored copy: bit 0 along S, bit 1 along T
} __attribute__((packed, aligned(4)));
static struct {
    struct TextureHashmapNode *hashmap[1024];
    struct TextureHashmapNode pool[512];
    uint32_t pool_pos;
} gfx_texture_cache;

/* Bits 28..31 of the combiner id flag the a/d slots that hold the constant 1
 * (color_comb_component folds it into CC_0): rgb a, alpha a, rgb d, alpha d. */
#define CC_FLAG_RGB_A_ONE   (1u << 28)
#define CC_FLAG_ALPHA_A_ONE (1u << 29)
#define CC_FLAG_RGB_D_ONE   (1u << 30)
#define CC_FLAG_ALPHA_D_ONE (1u << 31)

struct ColorCombiner {
    uint32_t cc_id;
    struct ShaderProgram *prg;
    uint8_t shader_input_mapping[2][4];
    int8_t color_mode;   // -1 not classified yet, 0 copy shade, 1 constant, 2 general
    bool alpha_255;
    bool needs_lod;
} __attribute__((packed, aligned(4)));

static struct ColorCombiner color_combiner_pool[64];
int gfx_shader_pool_recycled;
static uint8_t color_combiner_pool_size;

static struct RSP {
    float modelview_matrix_stack[11][4][4]__attribute__((aligned(16)));

    float MP_matrix[4][4] __attribute__((aligned(16)));
    float P_matrix[4][4] __attribute__((aligned(16)));
    /* True when P_matrix is a perspective projection.  MK64 folds the camera
     * rotation into P, so the w-row (P[0][3],P[1][3],P[2][3]) is
     * -(view direction): P[2][3] alone is ~0 whenever the camera looks along
     * +-X (Turnpike/DK Jungle east-west stretches) -- testing only it turned
     * every perspective-only step (near clip, guard band, culling) off there. */
    bool is_persp;
    uint8_t modelview_matrix_stack_size;
    
    Light_t current_lights[MAX_LIGHTS + 1];
    float current_lights_coeffs[MAX_LIGHTS][3];
    float current_lookat_coeffs[2][3]; // lookat_x, lookat_y
#ifdef PM64
    Light_t current_lookat[2]; // gSPLookAt: [0] = LOOKATY, [1] = LOOKATX (F3DEX2 G_MV_LIGHT offsets 24, 0)
#endif
    uint8_t current_num_lights; // includes ambient light
    bool lights_changed;
    
    uint32_t geometry_mode;
    int16_t fog_mul, fog_offset;
    
    struct {
        // U0.16
        uint16_t s, t;
    } texture_scaling_factor;
    
    struct VertexColor loaded_vertices_2D[4];
    struct LoadedVertex loaded_vertices[MAX_VERTICES];
} rsp  __attribute__((aligned(16)));

#ifdef PM64
#define GFX_LT_SLOTS 8 /* texture loads, by TMEM address (gfx_lt_find) */
#else
#define GFX_LT_SLOTS 2
#endif
static struct RDP {
    const uint8_t *palette;
    struct {
        const uint8_t *addr;
        uint8_t siz;
        uint8_t tile_number;
        uint16_t width; // texels per row of the image (G_SETTIMG)
    } texture_to_load;
    uint16_t tile_tmem[8]; // TMEM address (64-bit words) of every tile descriptor
    struct {
        const uint8_t *addr;
        uint32_t size_bytes;
        uint32_t stride_bytes; // DRAM row pitch (0 = contiguous texels)
#ifdef PM64
        uint32_t row_bytes;  // LoadTile: bytes of each loaded row (0 = LoadBlock / TLUT)
        uint16_t line_bytes; // LoadTile: TMEM bytes between loaded rows (the load tile's line)
        uint16_t tmem;       // TMEM address (64-bit words) the load starts at
        bool valid;
#endif
    } loaded_texture[GFX_LT_SLOTS];
#ifdef PM64
    uint8_t lt_next; // round-robin victim
#endif
    struct {
        uint8_t fmt;
        uint8_t siz;
        uint8_t cms, cmt;
        uint8_t tmem_slot; // which loaded_texture[] the render tile reads: its TMEM half (tmem / 256)
        uint16_t uls, ult, lrs, lrt; // U10.2
        uint32_t line_size_bytes;
#ifdef PM64
        uint8_t palette; // CI4: which 16-entry TLUT bank (TMEM 0x100 + palette * 0x10)
        uint8_t cms_raw, cmt_raw; // as the display list set them (cms/cmt: what the sampler does, pm_render_tile_cm)
        uint8_t masks, maskt;     // wrap period 1 << mask texels (0 = none)
        uint16_t tmem;            // TMEM address (64-bit words): selects the load it reads (gfx_lt_find)
#endif
    } texture_tile;
    bool textures_changed[2];
    
    uint32_t other_mode_l, other_mode_h;
    uint32_t combine_mode;
    
    struct RGBA env_color, prim_color, fog_color, fill_color, blend_color;
    struct XYWidthHeight viewport, scissor;
    bool viewport_or_scissor_changed;
    void *z_buf_address;
    void *color_image_address;
#ifdef PM64
    uint32_t pm_cc_w0, pm_cc_w1; /* the raw G_SETCOMBINE words, both cycles (pm_cc_generic) */
    uint8_t pm_prim_lod;         /* G_SETPRIMCOLOR's LOD fraction */
#endif
} rdp  __attribute__((aligned(4)));
#ifdef PM64
static const uint8_t *pm_tlut[16]; /* DRAM address of each 16-entry TLUT bank last loaded (gfx_dp_load_tlut) */
#endif

static struct RenderingState {
    struct XYWidthHeight viewport, scissor;
    struct ShaderProgram *shader_program;
    struct TextureHashmapNode *textures[2];
    bool depth_test;
    bool depth_mask;
    bool decal_mode;
    bool alpha_blend;
    bool edge_opaque; // TEX_EDGE without FORCE_BL: the soft backend writes it unblended (X12)
    bool own_edges;   // PM64_FILLRULE: blended / alpha-tested without AA_EN, each pixel to one triangle of an edge
    bool tex_add; // backend texture function is ADD (texel + vertex colour)
} rendering_state __attribute__((aligned(16)));

struct GfxDimensions gfx_current_dimensions __attribute__((aligned(4)));

static bool dropped_frame;

#if defined(TARGET_PSP) || defined(TARGET_HANDHELD)
typedef struct psp_fast_t {
  float u,v;
  struct RGBA color;
  float x,y,z;
#if !defined(TARGET_PSP)
  float invw; /* 1/clip.w — the soft rasterizer does perspective-correct UVs
                 (the GE was affine; the N64 RDP was not, hence "PS1 wobble") */
#endif
} psp_fast_t;
static psp_fast_t buf_vbo[MAX_BUFFERED  * 3] __attribute__ ((aligned (32))); // 3 vertices in a triangle and 26 floats per vtx
#else
static float buf_vbo[MAX_BUFFERED * (26 * 3)] // 3 vertices in a triangle and 26 floats per vtx
#endif

/*
 * Per-triangle render state is only recomputed after a state command ran
 * (gfx_run_dl clears tri_state.valid); MK64 draws thousands of triangles per
 * frame between state changes.
 */
static struct {
    bool valid;
    uint32_t cc_id;
    struct ColorCombiner *comb;
    bool use_texture;
    float u_scale, v_scale, u_off, v_off; // buf.u = vtx.u * u_scale + u_off
    int color_mode;                        // 0 copy shade, 1 constant, 2 general
    bool alpha_255;                        // copy mode: alpha forced to 255
    bool needs_lod;
    uint8_t mirror;                        // the bound texture was imported doubled (see gfx_mirror_flags)
    bool use_fog;                          // cycle-1 blender is CLR_FOG * A_SHADE: lerp the colour to the fog colour
    bool tex_add;                          // GE ADD: texel + const_color (see the tint combiner in gfx_tri_rebuild_state)
    struct RGBA const_color;
} tri_state;

static size_t buf_vbo_len;
static size_t buf_num_vert;
static size_t buf_vbo_num_tris;

static struct GfxWindowManagerAPI *gfx_wapi;
static struct GfxRenderingAPI *gfx_rapi;

#if defined(TARGET_PSP)
#include <pspthreadman.h>
static unsigned long get_time(void) {
    return sceKernelGetSystemTimeWide();
}
#else
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
static unsigned long get_time(void) {
#if defined(SYS_clock_gettime32) /* one kernel entry, not two (see bench.c port_time_us) */
    struct { long s, ns; } t;
    if (syscall(SYS_clock_gettime32, CLOCK_MONOTONIC, &t) == 0) return (unsigned long)t.s * 1000000 + t.ns / 1000;
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif


//******************* Clipping things

// Bits for clipping
// +-+-+-
// xxyyzz
#define Z_NEG  (0x01)
#define Z_POS  (0x02)
#define Y_NEG  (0x04)
#define Y_POS  (0x08)
#define X_NEG  (0x10)
#define X_POS  (0x20)
/* Exact side planes (x > w etc., homogeneous: exact for any sign of w). Kept apart from the
 * guard-band bits above, which gate the early reject BEFORE the phase bookkeeping in
 * gfx_sp_tri1; these reject after it (MK64 perf round 24, via ocarina-of-time d267ca3). */
#define X_OUT_L (0x100)
#define X_OUT_R (0x200)
#define Y_OUT_B (0x400)
#define Y_OUT_T (0x800)
#define XY_OUT (X_OUT_L | X_OUT_R | Y_OUT_B | Y_OUT_T)
/* Beyond the RSP's guard band, |x| or |y| > ratio * |w| (the ratio from gSPClipRatio: Paper Mario sets FRUSTRATIO_2):
 * the triangle goes to the clipper, which cuts it at the guard band as the RSP does (the cross-port R33, DKR 467ad1e).
 * One bit for all four sides, so it never takes part in the shared-flag reject. */
#define GB_OUT (0x1000)
/* At or behind the eye (w < W_MIN): the clipper's w plane cuts it, whatever the near-plane rule. */
#define W_LOW (0x2000)
/* The clipper's planes (round 16). Each switch is read once, in gfx_init:
 *   PM64_CLIPEDGES  2 = the guard band x, y = +-ratio * w (default, R33): a vertex beyond it sends its triangle to
 *                   the clipper (GB_OUT). The shade colour is interpolated on screen, so where a triangle is cut
 *                   changes its shading; the console cuts where a vertex crosses a plane, so the shading can't jump.
 *                   1 = the kit's exact screen edges, cut only once a triangle was sent for its near or far plane;
 *                   0 = no side planes.
 *   PM64_NEARCLIP   0 = no near plane (default): Paper Mario's microcode is gspF3DZEX2_NoN, "no near clipping"
 *                   (gbi.h: the geometry mode's high byte turns near clipping on in the other F3DEX2 builds), so
 *                   what lies between the eye and the near plane draws uncut, its depth clamped to the nearest.
 *                   1 = round 11's near plane, z >= -w. Either way the w plane (w >= W_MIN) cuts what is at or
 *                   behind the eye; the RSP's guard-band planes do that in homogeneous space (x <= r*w and
 *                   x >= -r*w only hold for w >= 0). */
static int gfx_clipedges SAVESTATE_EXCLUDE = 2, gfx_nearclip SAVESTATE_EXCLUDE = 0;
static uint32_t gfx_clip_trigger SAVESTATE_EXCLUDE = Z_NEG | W_LOW | GB_OUT; /* set in gfx_init */
static uint32_t gfx_reject_keep SAVESTATE_EXCLUDE = XY_OUT | GB_OUT | Z_POS; /* bits the shared-flag reject ignores */
static float gfx_clip_ratio = 2.0f; /* gSPClipRatio (G_MW_CLIP) */
static int pm_zimgrect SAVESTATE_EXCLUDE; /* PM64_ZIMGRECT=1: texrects into the Z image draw (R34's A/B) */
#ifdef PM64
#define PM_CFB_BYTES (320u * 240u * 2u)
static const uint8_t *pm_cfb SAVESTATE_EXCLUDE;   /* the framebuffer being drawn: the last SETCIMG to one (pm_frame_texture_sync) */
static uint32_t pm_frame_gen SAVESTATE_EXCLUDE;   /* bumped when pm_frame_texture_sync changes that memory */
#endif
#ifdef PM64
/* PM64_CENSUS=1: counts for the cross-port "count first" rule, printed at exit (round 16). */
static int pm_census SAVESTATE_EXCLUDE = -1;
static struct {
    uint32_t tris, gb, near_cross, clips, clip_near, clip_far, clip_gb, clip_wlow, mwclip, fog_tris, fog_color,
             zimg_rects, zimg_tris, rects, scissors, scissors_r23, rects_centre, rects_texadd;
    float ratio_lo, ratio_hi;
    uint32_t fog_rgb[4];
} pm_cs SAVESTATE_EXCLUDE;
static void pm_census_print(void) {
    fprintf(stderr, "census: 3D triangles %u; with a vertex beyond the guard band %u; reaching in front of the near "
            "plane %u\n", pm_cs.tris, pm_cs.gb, pm_cs.near_cross);
    fprintf(stderr, "census: clipped %u (a vertex in front of near %u, beyond far %u, beyond the guard band %u, "
            "at or behind the eye %u)\n", pm_cs.clips, pm_cs.clip_near, pm_cs.clip_far, pm_cs.clip_gb, pm_cs.clip_wlow);
    fprintf(stderr, "census: gSPClipRatio %u times, ratio %.0f..%.0f\n", pm_cs.mwclip, pm_cs.ratio_lo, pm_cs.ratio_hi);
    fprintf(stderr, "census: fogged triangles %u, with a non-black fog colour %u (first colours %06X %06X %06X %06X)\n",
            pm_cs.fog_tris / 3, pm_cs.fog_color / 3, pm_cs.fog_rgb[0], pm_cs.fog_rgb[1], pm_cs.fog_rgb[2], pm_cs.fog_rgb[3]);
    fprintf(stderr, "census: texrects %u, into the Z image %u; triangles into the Z image %u\n", pm_cs.rects,
            pm_cs.zimg_rects, pm_cs.zimg_tris);
    fprintf(stderr, "census: scissors %u, moved by R23's final rule %u; 2D rectangles whose edges move on pixel "
            "centres %u\n", pm_cs.scissors, pm_cs.scissors_r23, pm_cs.rects_centre);
    fprintf(stderr, "census: 2D rects that found a 3D draw's texture-add state on %u\n", pm_cs.rects_texadd);
}
static inline bool pm_census_on(void) {
    if (pm_census < 0) {
        const char *e = getenv("PM64_CENSUS");
        pm_census = e && e[0] == '1';
        if (pm_census) { pm_cs.ratio_lo = 1e9f; pm_cs.ratio_hi = -1e9f; atexit(pm_census_print); }
    }
    return pm_census;
}
#endif

// Which frustum planes trigger the software clipper.  Default clips every
// plane (guard-banded below, so few triangles qualify) -- needed because a
// single near-camera / off-screen road polygon can straddle several planes at
// once and only clipping all of them removes the stretched-streak artifact.
// PORT_NOCLIP disables it; PORT_CLIP_MASK overrides the set for experiments.
#if defined(PORT_NOCLIP)
#define CLIP_TEST_FLAGS ( 0 )
#elif defined(PORT_CLIP_MASK)
#define CLIP_TEST_FLAGS ( PORT_CLIP_MASK )
#else
// Per-vertex flags are unused by default; a per-triangle test (below) clips only
// polygons that span from near the camera to beyond the far plane -- the ones
// that overflow the rasteriser into stretched streaks.  Everything else is left
// to the GE.
#define CLIP_TEST_FLAGS ( 0 )
#endif

// Software-clip only when a vertex reaches this far past a plane; small
// over-runs are left to the GE (guard band + depth clamp).  These keep the
// clip count low while still catching the pathological near/off-screen polys.
#define GUARD_BAND 3.0f   // X/Y: clip when |x|,|y| > GUARD_BAND * w

static inline float vec3_dot(const float *lhs, const float *rhs){
    return (lhs[0]*rhs[0]) + (lhs[1]*rhs[1]) + (lhs[2]*rhs[2]);
}

static inline float vec4_dot(const float *lhs, const float *rhs){
    return (lhs[0]*rhs[0]) + (lhs[1]*rhs[1]) + (lhs[2]*rhs[2])+ (lhs[3]*rhs[3]);
}

static inline void vec4_sub(float *out, const float* lhs, const float*rhs){
    out[0] = lhs[0]-rhs[0];
    out[1] = lhs[1]-rhs[1];
    out[2] = lhs[2]-rhs[2];
    out[3] = lhs[3]-rhs[3];
}

void gfx_clip_interpolate_vert(struct LoadedVertex* out, const struct  LoadedVertex* lhs, const struct LoadedVertex* rhs, const float factor )
{
    // projected pos
    out->x = lhs->x + (rhs->x - lhs->x) * factor;
    out->y = lhs->y + (rhs->y - lhs->y) * factor;
    out->z = lhs->z + (rhs->z - lhs->z) * factor;
    //out->w = lhs->w + (rhs->w - lhs->w) * factor;
    // transfomed pos
    out->_x = lhs->_x + (rhs->_x - lhs->_x) * factor;
    out->_y = lhs->_y + (rhs->_y - lhs->_y) * factor;
    out->_z = lhs->_z + (rhs->_z - lhs->_z) * factor;
    out->_w = lhs->_w + (rhs->_w - lhs->_w) * factor;
    // color
    out->color.r = lhs->color.r + (rhs->color.r - lhs->color.r) * factor;
    out->color.g = lhs->color.g + (rhs->color.g - lhs->color.g) * factor;
    out->color.b = lhs->color.b + (rhs->color.b - lhs->color.b) * factor;
    out->color.a = lhs->color.a + (rhs->color.a - lhs->color.a) * factor;
    // texture
    out->u = lhs->u + (rhs->u - lhs->u) * factor;
    out->v = lhs->v + (rhs->v - lhs->v) * factor;
}

//*****************************************************************************
//
//	The following clipping code was taken from The Irrlicht Engine.
//	See http://irrlicht.sourceforge.net/ for more information.
//	Copyright (C) 2002-2006 Nikolaus Gebhardt/Alten Thomas
//
//*****************************************************************************
static const float NDCPlane[6][4] =
{
	{  0.f,  0.f,  1.f, -1.f },	// near
	{  1.f,  0.f,  0.f, -1.f },	// left
	{ -1.f,  0.f,  0.f, -1.f },	// right
	{  0.f,  1.f,  0.f, -1.f },	// bottom
	{  0.f, -1.f,  0.f, -1.f },	// top
	{  0.f,  0.f, -1.f, -1.f }	// z >= -w: the near plane, where the RSP clips (SM64 5cd6699; the kit pulled it in to
	                            // -0.97 w for the PSP GE, which mishandles a vertex on its own near plane)
};

static uint32_t clipToHyperPlane( struct LoadedVertex *dest, const struct LoadedVertex *source, uint32_t inCount, const float plane[4] )
{
	uint32_t outCount;
	struct LoadedVertex *out;

	const struct LoadedVertex *a;
	const struct LoadedVertex *b;

	float aDotPlane;
	float bDotPlane;
    float temp_vec[4];

	out = dest;
	outCount = 0;
	b = source;
	bDotPlane = vec4_dot(&b->_x, plane);
    size_t i;

#define EPSILON 0.00000001
	for(i = 1; i < inCount + 1; ++i)
	{
		a = &source[i%inCount];
		aDotPlane = vec4_dot(&a->_x, plane);

		// current point inside
		if ( aDotPlane <= EPSILON )
		{
			// last point outside
			if ( bDotPlane > EPSILON )
			{
				// intersect line segment with plane
                // Next 2 lines are "(b->ProjectedPos - a->ProjectedPos).Dot( plane )"
                vec4_sub(temp_vec, &b->_x, &a->_x);
                const float dot_projected = vec4_dot(temp_vec, plane);
				gfx_clip_interpolate_vert(out, b, a, bDotPlane / dot_projected );
				out += 1;
				outCount += 1;
			}
			// copy current to out
			*out = *a;
			b = out;

			out += 1;
			outCount += 1;
		}
		else
		{
			// current point outside

			if ( bDotPlane <= EPSILON )
			{
				// previous was inside
				// intersect line segment with plane
                // Next 2 lines are "(b->ProjectedPos - a->ProjectedPos).Dot( plane )"
                vec4_sub(temp_vec, &b->_x, &a->_x);
                const float dot_projected = vec4_dot(temp_vec, plane);
				gfx_clip_interpolate_vert(out, b, a, bDotPlane / dot_projected );

				out += 1;
				outCount += 1;
			}
			b = a;
		}

        bDotPlane = vec4_dot(&b->_x, plane);
	}

	return outCount;
}

/* Homogeneous near-plane clip: keep only the part of the polygon with w >=
 * W_MIN (in front of the eye).  The projection-space (NDC) near plane cannot do
 * this -- a vertex behind the eye has w < 0 and z >= -w tests as "inside" -- so
 * behind-camera road/checkered polygons project to stretched garbage unless
 * clipped here first. */
/* PM64_NEARW (default on; gfx_soft.c, with the depth path it needs; 2026-10-04, goldeneye-007 c20b9ee, ocarina-of-time's
 * OOT_NEARW): with no near plane (PM64_NEARCLIP=0, the default: F3DZEX2 NoN) the N64's only plane on the eye's side is
 * w >= 0 (the guard band's |x|, |y| <= ratio * w), kept here as w >= 1/64 -- the sliver nearer than 1/64 of a unit is
 * the only difference, and the guard band keeps the projection of anything near w = 0 inside +-ratio screens. 1/64
 * bounds 1/w at 64: a perspective-shaded colour times 1/w stays inside 16.16 (255 * 64 * 65536 < 2^31). The kit's 2.0
 * (MK64's units) cut whatever came within 2 units of the eye, which the N64 draws. PM64_NEARW=0, or the near plane on
 * (PM64_NEARCLIP=1): 2.0, as before. Set in gfx_init. */
static float gfx_wmin SAVESTATE_EXCLUDE = 2.0f;
#define W_MIN gfx_wmin
/* The GE does no polygon clipping: with GU_CLIP_PLANES on it DISCARDS any
 * triangle that has a vertex outside the depth range (clip.z outside +-clip.w).
 * The RSP instead clips such triangles at the near/far planes and draws the
 * rest.  So the port must clip at the game's real near plane (z_ndc = -1,
 * gCourseNearPersp = 9 units on most courses), not merely "behind the eye" --
 * otherwise a big road triangle with one vertex closer than 9 units vanishes
 * whole and the backdrop shows through (Turnpike / DK Jungle foreground gap).
 * GE_DEPTH_EPS pulls the planes fractionally inward so a CPU-clipped vertex
 * re-transformed by the GE never rounds to just outside the range. */
#ifndef GE_DEPTH_EPS
#define GE_DEPTH_EPS 0.001f
#endif
#ifndef GE_TL_NEAR
#define GE_TL_NEAR 0.05f   /* GE_TL near-clip plane (clip.w); as small as possible to keep road under the kart, above the clip.w->0 blow-up */
#endif
static uint32_t clipToNearW( struct LoadedVertex *dest, const struct LoadedVertex *source, uint32_t inCount )
{
	uint32_t outCount = 0;
	struct LoadedVertex *out = dest;
	const struct LoadedVertex *a, *b = source;
	float aOut, bOut = W_MIN - b->_w;   // > 0 == outside (too near/behind)
	size_t i;
	for (i = 1; i < inCount + 1; ++i) {
		a = &source[i % inCount];
		aOut = W_MIN - a->_w;
		if (aOut <= EPSILON) {                 // a inside
			if (bOut > EPSILON) {              // b outside -> add intersection
				gfx_clip_interpolate_vert(out, b, a, bOut / (bOut - aOut));
				out++; outCount++;
			}
			*out = *a; b = out; out++; outCount++;
		} else {                                // a outside
			if (bOut <= EPSILON) {             // b inside -> add intersection
				gfx_clip_interpolate_vert(out, b, a, bOut / (bOut - aOut));
				out++; outCount++;
			}
			b = a;
		}
		bOut = W_MIN - b->_w;
	}
	return outCount;
}

/* A pass that would cut nothing is skipped (round 16, the guard-band clipper only): it would copy the polygon and
 * rotate it by one vertex, which changes the fan below but no coverage, and it costs a copy of every vertex. */
static bool clip_plane_cuts(const struct LoadedVertex *p, uint32_t n, const float plane[4]) {
	for (uint32_t i = 0; i < n; i++) if (vec4_dot(&p[i]._x, plane) > EPSILON) return true;
	return false;
}
static bool clip_nearw_cuts(const struct LoadedVertex *p, uint32_t n) {
	for (uint32_t i = 0; i < n; i++) if (W_MIN - p[i]._w > EPSILON) return true;
	return false;
}

/* The polygon starts in v0; each pass writes the other buffer. Returns the vertex count and sets *res to the buffer
 * holding the result (DKR 467ad1e's form). The planes follow PM64_CLIPEDGES and PM64_NEARCLIP (above); with both at
 * the kit's values (1, 1) the passes are round 15's. */
static uint32_t clip_to_frustum( struct LoadedVertex * v0, struct LoadedVertex * v1, uint32_t vIn, struct LoadedVertex **res )
{
	uint32_t vOut = vIn;
	struct LoadedVertex *src = v0, *dst = v1, *t;
#define CLIP_PASS(call) do { vOut = (call); t = src; src = dst; dst = t; } while (0)

	if (gfx_clipedges == 2) { /* the RSP's planes: the eye, the guard band, far (and near only with PM64_NEARCLIP=1) */
		const float r = gfx_clip_ratio;
		const float gp[4][4] = { { -1.f, 0.f, 0.f, -r }, { 1.f, 0.f, 0.f, -r }, { 0.f, -1.f, 0.f, -r }, { 0.f, 1.f, 0.f, -r } };
		if (clip_nearw_cuts( src, vOut )) CLIP_PASS( clipToNearW( dst, src, vOut ) );
		for (int k = 0; k < 4 && vOut >= 3; k++)	// x >= -r*w, x <= r*w, y >= -r*w, y <= r*w
			if (clip_plane_cuts( src, vOut, gp[k] )) CLIP_PASS( clipToHyperPlane( dst, src, vOut, gp[k] ) );
		if (vOut >= 3 && clip_plane_cuts( src, vOut, NDCPlane[0] )) CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[0] ) );	// far
		if (gfx_nearclip && vOut >= 3 && clip_plane_cuts( src, vOut, NDCPlane[5] )) CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[5] ) );	// near
		*res = src;
		return vOut;
	}
	// Near plane (w-based) first: removes behind-camera parts so the NDC-plane
	// clips below see only w>0 vertices.
	CLIP_PASS( clipToNearW( dst, src, vOut ) );
	if (gfx_clipedges) {
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[2] ) );	// right
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[1] ) );	// left
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[4] ) );	// top
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[3] ) );	// bottom
	}
	CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[0] ) );		// far (z <= w; the kit labelled it "near")
	if (gfx_nearclip)
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[5] ) );	// near (z >= -w; labelled "far")
#undef CLIP_PASS

	*res = src;
	return vOut;
}

static struct LoadedVertex temp_a[12];
static struct LoadedVertex temp_b[12];

void gfx_clip_single_vert( struct LoadedVertex *p_p_vertices, size_t *p_num_vertices, struct LoadedVertex *v_arr[3])
{
	//
	//	At this point all vertices are lit/projected and have both transformed and projected
	//	vertex positions. For the best results we clip against the projected vertex positions,
	//	but use the resulting intersections to interpolate the transformed positions. 
	//	The clipping is more efficient in normalised device coordinates, but rendering these
	//	directly prevents the PSP performing perspective correction. We could invert the projection
	//	matrix and use this to back-project the clip planes into world coordinates, but this
	//	suffers from various precision issues. Carrying around both sets of coordinates gives
	//	us the best of both worlds :)
	//
    size_t clipped_vertices_num = 0;

    temp_a[ 0 ] = *v_arr[ 0 ];
    temp_a[ 1 ] = *v_arr[ 1 ];
    temp_a[ 2 ] = *v_arr[ 2 ];

    /* The passes bounce temp_a <-> temp_b; res is where they ended. The kit fanned temp_a: with seven passes, the
     * polygon before the last pass, with that pass's vertex count (SM64 5cd6699). */
    struct LoadedVertex *res;
    uint32_t out = clip_to_frustum( temp_a, temp_b, 3, &res );
    const struct LoadedVertex *poly = res;
    if( out < 3 ){
        *p_num_vertices = 0;
        return;
    }

    // Retesselate.  out <= 10 (3 verts + 7 clip planes), so this emits at most
    // 3*(10-2)=24 vertices; cap defensively at the caller's buffer size (32) so a
    // miscount can never overflow _clipped_vertices/ptr_clipped_vertices again.
    for( uint32_t j = 0; j <= out - 3 && clipped_vertices_num + 3 <= 32; ++j )
    {
        p_p_vertices[clipped_vertices_num++] = ( poly[ 0 ] );
        p_p_vertices[clipped_vertices_num++] = ( poly[ j + 1 ] );
        p_p_vertices[clipped_vertices_num++] = ( poly[ j + 2 ] );
    }

	*p_num_vertices = clipped_vertices_num;
}

//******************* End Clipping things


static uint32_t gfx_flush_index;
/* HUD anchoring.  Between the HUD_ON/HUD_OFF display-list markers
 * (gDPNoOpTag from render_hud) every 2D element is scaled uniformly (the 4:3
 * layout keeps its proportions) and anchored to the screen edge it was
 * designed against: elements in the left third of the 320-wide layout stay
 * flush left, the right third flush right, the middle stays centred.
 * Full-width fills keep the plain stretch so they never leave side bars.
 * Built for PSP widescreen (spare width > 0); the handheld CONVERT square
 * screen reuses it with spare width < 0 -- right-anchored elements pull IN
 * to the 240-px edge instead of out. */
int gfx_hud_anchor;                 /* 0 stretch (legacy), 1 race HUD (anchor by side), 2 menus (centre) */
#define HUD_CLASS_LEFT 0
#define HUD_CLASS_CENTRE 1
#define HUD_CLASS_RIGHT 2
#define HUD_CLASS_FULL 3
static float hud_scale(void) { return (float) gfx_current_dimensions.height / (2.0f * HALF_SCREEN_HEIGHT); }
static float hud_shift(int cls) {
    float spare = (float) gfx_current_dimensions.width - (2.0f * HALF_SCREEN_WIDTH) * hud_scale();
    return cls == HUD_CLASS_LEFT ? 0.0f : cls == HUD_CLASS_RIGHT ? spare : spare * 0.5f;
}
/* The previous element's extent and class: an element drawn over or against
 * it belongs to it (a rank digit on a results-screen portrait, the dots on
 * the map) and must go to the same side, whatever side its own centre says.
 * Issue #14: Toad's "5" on the results screen was classed RIGHT while his
 * portrait stayed CENTRE and landed a slot away, next to Bowser. */
static float hud_prev_x0, hud_prev_x1;
static int hud_prev_class = -1;
/* x0/x1: horizontal extent in the game's 320-wide layout space. */
static int hud_class(float x0, float x1) {
    float c;
    int cls;
    if (x0 <= 1.0f && x1 >= 2.0f * HALF_SCREEN_WIDTH - 1.0f) {
        cls = HUD_CLASS_FULL;
    } else if (gfx_hud_anchor == 2) {
        cls = HUD_CLASS_CENTRE; /* menus: the 4:3 layout centred over the stretched background */
    } else if (hud_prev_class >= 0 && hud_prev_class != HUD_CLASS_FULL && x0 < hud_prev_x1 - 2.0f && x1 > hud_prev_x0 + 2.0f) {
        cls = hud_prev_class; /* drawn over the previous element: stay with it */
    } else {
        /* Only elements sitting on the centre line (item box, GO!/FINISH) stay
         * centred; everything else follows its side.  A narrow band keeps
         * multi-sprite elements (LAP + digits, TIME + digits) together and lets the
         * lap counter slide in from off-screen without changing class. */
        c = 0.5f * (x0 + x1) - HALF_SCREEN_WIDTH;
        cls = c < -24.0f ? HUD_CLASS_LEFT : c > 24.0f ? HUD_CLASS_RIGHT : HUD_CLASS_CENTRE;
    }
    hud_prev_x0 = x0;
    hud_prev_x1 = x1;
    hud_prev_class = cls;
    return cls;
}
#ifdef PORT_GE_TL
static int hud_batch_class = -1;    /* anchor class of the pending GE batch */
static int hud_load_class = -1;     /* anchor class of the last gSPVertex load */
static float ge_last_mp[4][4];
static uint32_t ge_list_used; /* bytes written to the GE list since the last (re)start */
#ifndef GE_LIST_RECYCLE
#define GE_LIST_RECYCLE (400u * 1024u) /* recycle before the 512KB list fills (max batch ~73KB) */
#endif /* recycle well before GU_LIST_BYTES (1MB); a per-batch max ~73KB fits the margin */
extern void gfx_scegu_sync_pending(void);
#endif
static void gfx_flush(void) {
    if (buf_vbo_len > 0) {
#ifdef PORT_GE_TL
        // GE-list overflow guard: a heavy intro (e.g. Toad's Turnpike GP) can
        // exceed the command list; overflowing corrupts memory -> white-screen
        // crash on real hardware (the emulator hides it).  Recycle the list
        // (execute what's queued, start fresh) before it fills.
        if (ge_list_used + buf_vbo_len + 4096u > GE_LIST_RECYCLE) {
            gfx_scegu_sync_pending();
            ge_list_used = 0;
            memset(ge_last_mp, 0, sizeof(ge_last_mp)); // force GU_PROJECTION re-push below
        }
        ge_list_used += buf_vbo_len + 128u; // vertex data + matrix/command overhead
        // The GE applies MP as GU_PROJECTION to object-space vertices.  Only push
        // it when it actually changed (the course keeps a constant MP for many
        // batches) -- redundant pushes bloat the GE list ~2x.
        float hud_mp[4][4] __attribute__((aligned(16)));
        const float (*use_mp)[4] = rsp.MP_matrix;
        if (gfx_hud_anchor && !rsp.is_persp && hud_batch_class >= 0 && hud_batch_class != HUD_CLASS_FULL) {
            /* Ortho HUD quads: fold the anchor into the projection's x column,
             * ndc.x' = a * ndc.x + b, so the GE lands them where a uniformly
             * scaled, edge-anchored 320x240 layout puts them. */
            float half_w = 0.5f * (float) gfx_current_dimensions.width;
            float a = hud_scale() * HALF_SCREEN_WIDTH / half_w;
            float b = (hud_scale() * HALF_SCREEN_WIDTH + hud_shift(hud_batch_class)) / half_w - 1.0f;
            int i;
            memcpy(hud_mp, rsp.MP_matrix, sizeof(hud_mp));
            for (i = 0; i < 4; i++) hud_mp[i][0] = a * rsp.MP_matrix[i][0] + b * rsp.MP_matrix[i][3];
            use_mp = hud_mp;
        }
        if (memcmp(ge_last_mp, use_mp, sizeof(rsp.MP_matrix)) != 0) {
            void *mp = (void *) ALIGN((unsigned int) sceGuGetMemory(sizeof(rsp.MP_matrix) + 15), 16);
            memcpy(mp, use_mp, sizeof(rsp.MP_matrix));
            sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4 *) mp);
            memcpy(ge_last_mp, use_mp, sizeof(rsp.MP_matrix));
            ge_list_used += sizeof(rsp.MP_matrix) + 32u;
        }
#endif

#ifdef PORT_GFX_DEBUG
        if (gfx_debug_frame && gfx_colorflush) {
            // Paint this batch in a unique flat colour so a screenshot identifies it.
            uint32_t idx = gfx_flush_index;
            struct RGBA col = { (uint8_t) (32 + (idx * 37) % 224), (uint8_t) (32 + (idx * 91) % 224), (uint8_t) (32 + (idx * 53) % 224), 255 };
            size_t k;
            for (k = 0; k < buf_num_vert; k++) buf_vbo[k].color = col;
            sceGuDisable(GU_TEXTURE_2D);
            sceGuDisable(GU_BLEND);
            sceGuDisable(GU_ALPHA_TEST);
            port_log("  flush %u colour %02X%02X%02X tris %u tex %d cc %08X\n", (unsigned) idx, col.r, col.g, col.b, (unsigned) buf_vbo_num_tris,
                     rendering_state.textures[0] ? (int) rendering_state.textures[0]->texture_id : -1, (unsigned) tri_state.cc_id);
        }
#endif
        gfx_flush_index++;
#ifdef PORT_PROFILE_DL
        { uint32_t _f0 = port_time_us(); gfx_rapi->draw_triangles((float *)buf_vbo, buf_vbo_len, buf_vbo_num_tris); gfx_prof_flush += port_time_us() - _f0; }
#else
        gfx_rapi->draw_triangles((float *)buf_vbo, buf_vbo_len, buf_vbo_num_tris);
#endif
#ifdef PORT_GFX_DEBUG
        if (gfx_debug_frame && gfx_colorflush) {
            sceGuEnable(GU_TEXTURE_2D);
            rendering_state.shader_program = NULL; // force state re-apply
            tri_state.valid = false;
        }
#endif
        buf_vbo_len = 0;
        buf_num_vert = 0;
        buf_vbo_num_tris = 0;
#ifdef PORT_GE_TL
        hud_batch_class = -1;
#endif
        //unsigned long t1 = get_time();
        /*if (t1 - t0 > 1000) {
            printf("f: %d %d\n", num, (int)(t1 - t0));
        }*/
    }
}

static struct ShaderProgram *gfx_lookup_or_create_shader_program(uint32_t shader_id) {
    struct ShaderProgram *prg = gfx_rapi->lookup_shader(shader_id);
    if (prg == NULL) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        prg = gfx_rapi->create_and_load_new_shader(shader_id);
        rendering_state.shader_program = prg;
    }
    return prg;
}

static void gfx_generate_cc(struct ColorCombiner *comb, uint32_t cc_id) {
    uint8_t c[2][4];
    uint32_t shader_id = ((cc_id >> 24) & 0xF) << 24; // SHADER_OPT_* only, not the CC_FLAG_* bits
    uint8_t shader_input_mapping[2][4] = {{0}};
    for (int i = 0; i < 4; i++) {
        c[0][i] = (cc_id >> (i * 3)) & 7;
        c[1][i] = (cc_id >> (12 + i * 3)) & 7;
    }
    for (int i = 0; i < 2; i++) {
        if (c[i][0] == c[i][1] || c[i][2] == CC_0) {
            c[i][0] = c[i][1] = c[i][2] = 0;
        }
        uint8_t input_number[8] = {0};
        int next_input_number = SHADER_INPUT_1;
        for (int j = 0; j < 4; j++) {
            int val = 0;
            switch (c[i][j]) {
                case CC_0:
                    break;
                case CC_TEXEL0:
                    val = SHADER_TEXEL0;
                    break;
                case CC_TEXEL1:
                    val = SHADER_TEXEL1;
                    break;
                case CC_TEXEL0A:
                    val = SHADER_TEXEL0A;
                    break;
                case CC_PRIM:
                case CC_SHADE:
                case CC_ENV:
                case CC_LOD:
                    if (input_number[c[i][j]] == 0) {
                        shader_input_mapping[i][next_input_number - 1] = c[i][j];
                        input_number[c[i][j]] = next_input_number++;
                    }
                    val = input_number[c[i][j]];
                    break;
            }
            shader_id |= val << (i * 12 + j * 3);
        }
    }
    comb->cc_id = cc_id;
    comb->prg = gfx_lookup_or_create_shader_program(shader_id);
    if (gfx_shader_pool_recycled) {
        // The backend restarted its shader pool: every other combiner's prg
        // pointer is stale.  Invalidate them (they regenerate on next use).
        size_t i;
        gfx_shader_pool_recycled = 0;
        for (i = 0; i < color_combiner_pool_size; i++) {
            if (&color_combiner_pool[i] != comb) {
                color_combiner_pool[i].cc_id = 0xFFFFFFFFu;
                color_combiner_pool[i].prg = NULL;
            }
        }
        rendering_state.shader_program = comb->prg;
        tri_state.valid = false;
    }
    memcpy(comb->shader_input_mapping, shader_input_mapping, sizeof(shader_input_mapping));
    comb->color_mode = -1;
}

static struct ColorCombiner *gfx_lookup_or_create_color_combiner(uint32_t cc_id) {
    static struct ColorCombiner *prev_combiner;
    if (prev_combiner != NULL && prev_combiner->cc_id == cc_id) {
        return prev_combiner;
    }
    
    for (size_t i = 0; i < color_combiner_pool_size; i++) {
        if (color_combiner_pool[i].cc_id == cc_id) {
            return prev_combiner = &color_combiner_pool[i];
        }
    }
    gfx_flush();
    if (color_combiner_pool_size == sizeof(color_combiner_pool) / sizeof(color_combiner_pool[0])) {
        // MK64 cycles through more combiner modes than the pool holds: start over.
        port_log("gfx: combiner pool full, recycling\n");
        color_combiner_pool_size = 0;
    }
    struct ColorCombiner *comb = &color_combiner_pool[color_combiner_pool_size++];
    gfx_generate_cc(comb, cc_id);
    return prev_combiner = comb;
}

extern int gfx_vram_space_available(void);
extern void texman_clear(void);
extern void texman_bind_tex(unsigned int num);
extern void gfx_scegu_sync_pending(void);
static void gfx_flush(void);
static void gfx_set_phase(bool is3d);
/* Far cull threshold in clip.w. 1e30 = off (the shipped default; no menu
 * wiring). Desktop tuning only: MK64_DRAWDIST=<units>, validated and parsed
 * once in gfx_init. SAVESTATE_EXCLUDE: a state saved with the env set must not
 * restore a cull distance into a session without it (round 13). */
float gfx_draw_dist_w SAVESTATE_EXCLUDE = 1e30f;
static bool gfx_phase3d = true;  /* current backend target: internal (3D) or native (2D) */
static bool gfx_seen_persp;      /* a perspective batch has been drawn this frame */
static uint32_t gfx_frame_counter;
int gPortShowFps = 0; /* FPS overlay; toggled by holding SELECT (controller_psp.c) */

/* Forget every cached texture and recycle the VRAM arena.  The GE is drained
 * first: the display list is only executed at the end of the frame and may
 * still reference the textures being freed. */
extern int texman_usage_percent(void);
static uint32_t gfx_midframe_resets;

/* Bumped by every texture cache reset (OoT 2b69f7b, the lead's texture-cache round). A reset wipes every texture,
 * including the one the OTHER tile of the draw being set up already holds: importing tile 1 could reset the cache
 * and leave rendering_state.textures[0] on a zeroed pool node -- or, as texman_clear restarts the ids at 1, on an
 * id tile 1's new texture then took -- so the draw went out untextured or with the wrong texels. The reset now drops
 * both bindings and marks both tiles changed, and the import loops run again when the epoch moves under them.
 * PM64_EPOCHFIX=0: the old behaviour (A/B). */
static uint32_t gfx_cache_epoch SAVESTATE_EXCLUDE;
static int gfx_epochfix SAVESTATE_EXCLUDE = -1;
static bool gfx_epochfix_on(void) {
    if (gfx_epochfix < 0) { const char *e = getenv("PM64_EPOCHFIX"); gfx_epochfix = !(e && e[0] == '0'); }
    return gfx_epochfix != 0;
}
/* PM64_TEXSTRESS=<n> (test): reset the cache every n new imports, as a long session's full arena does. Held off
 * while a draw imports again after a reset (gfx_tex_stress_hold): the arena is empty then, so a real one cannot
 * fire there, and with n = 1 a two-tile draw would otherwise never hold both. The stressed run must give the same
 * frames as a normal one: a reset only costs re-imports. */
static int gfx_tex_stress_hold SAVESTATE_EXCLUDE;
static bool gfx_tex_stress(void) {
    static int sEvery = -1, sCount;
    if (sEvery < 0) { const char *e = getenv("PM64_TEXSTRESS"); sEvery = e ? atoi(e) : 0; }
    return sEvery > 0 && !gfx_tex_stress_hold && ++sCount % sEvery == 0;
}
static void gfx_texture_cache_reset(bool midframe) {
    if (midframe) {
        gfx_flush();
        gfx_scegu_sync_pending();
#ifdef PORT_GE_TL
        memset(ge_last_mp, 0, sizeof(ge_last_mp)); // list restarted -> re-push GU_PROJECTION
        ge_list_used = 0;
#endif
        gfx_midframe_resets++;
    }
    texman_clear();
    gfx_cache_epoch++;
    if (gfx_epochfix_on()) {
        rendering_state.textures[0] = rendering_state.textures[1] = NULL;
        rdp.textures_changed[0] = rdp.textures_changed[1] = true;
    }
    gfx_texture_cache.pool_pos = 0;
    memset(gfx_texture_cache.pool, 0, sizeof(gfx_texture_cache.pool));
    memset(gfx_texture_cache.hashmap, 0, sizeof(gfx_texture_cache.hashmap));
}

static uint32_t tex_bytes_per_row(uint32_t width, uint32_t siz);

static inline uint32_t hash_bytes(uint32_t h, const uint8_t *p, uint32_t n) {
    // FNV-1a style over words.  Larger blocks are sampled (every 4th word plus
    // the last one): the buffers the game rewrites change broadly, and this
    // runs for every texture load of every frame.
    if (((uintptr_t) p & 3) == 0) {
        const uint32_t *w = (const uint32_t *) p;
        uint32_t nw = n >> 2;
        if (nw > 64) {
            const uint32_t *end = w + nw;
            h = (h ^ end[-1]) * 16777619u;
            for (; w < end; w += 4) {
                h = (h ^ *w) * 16777619u;
            }
            return h;
        }
        while (nw--) {
            h = (h ^ *w++) * 16777619u;
        }
        p = (const uint8_t *) w;
        n &= 3;
    }
    while (n--) {
        h = (h ^ *p++) * 16777619u;
    }
    return h;
}

/* Hash the texels the render tile will read (plus the palette for CI
 * formats).  The game reuses buffers (menu textures, kart sprites, player
 * palettes) so the address alone does not identify a texture. */
extern char _ftext[], _fbss[];             // linker: start of code, start of BSS
extern char __assets_start[], __assets_end[]; // linker: the ROM-derived asset region (after BSS)

/* Texels the game never rewrites: code/data of the executable, or the asset
 * region (ROM blobs, torch assets), which the linker places AFTER .bss. */
/* port.h: the course texture block and, inside it, the stadium-screen tiles. */
static const char *course_tex_lo, *course_tex_hi, *fb_tile_lo, *fb_tile_hi;
static int tex_cache_flush_pending;
void port_course_textures_loaded(void *start, unsigned int size) {
    course_tex_lo = (const char *) start;
    course_tex_hi = course_tex_lo + size;
    fb_tile_lo = fb_tile_hi = NULL;
    tex_cache_flush_pending = 1; /* the same addresses are about to mean other texels */
}
static bool gfx_in_frame; /* inside gfx_run: a cache reset must wait for start_frame */
void port_fb_tile_note(void *target, unsigned int bytes) {
    const char *t = (const char *) target;
    const char *lo = fb_tile_lo, *hi = fb_tile_hi;
    if (fb_tile_lo == NULL || t < fb_tile_lo) fb_tile_lo = t;
    if (fb_tile_hi == NULL || t + bytes > fb_tile_hi) fb_tile_hi = t + bytes;
    /* A tile drawn before its first capture was imported as a STATIC course
     * texture (address-keyed, never re-read): it kept showing its stale
     * contents until the noted range grew over it -- the quadrant that
     * "lost the buffer" for a few frames at a track load (round 6). Drop
     * the cache whenever the range grows so those entries re-import hashed. */
    if (fb_tile_lo != lo || fb_tile_hi != hi) {
        if (gfx_in_frame) tex_cache_flush_pending = 1;
        else gfx_texture_cache_reset(false);
    }
}
static inline bool gfx_is_static_memory(const void *p) {
    const char *c = (const char *) p;
    if (c >= course_tex_lo && c < course_tex_hi) {
        return !(c >= fb_tile_lo && c < fb_tile_hi);
    }
    return (c >= _ftext && c < _fbss) || (c >= __assets_start && c < __assets_end);
}

/* The loaded_texture[] slot a texture unit reads: unit 0 is the render tile,
 * which addresses either TMEM half; unit 1 keeps its own slot. */
#ifdef PM64
/* Paper Mario uses all eight tile descriptors (a texrect on tile 2 that reads the
 * TLUT out of TMEM, CI4 sprites choosing a palette bank per tile), so keep them all;
 * the kit's single render-tile state is loaded from them when a tile draws. */
static struct { uint8_t fmt, siz, cms, cmt, palette, masks, maskt, shifts, shiftt; uint16_t tmem, uls, ult, lrs, lrt; uint32_t line; } pm_tiles[8];
/* The RDP masks texel coordinates to 1 << mask (wrap or mirror, after any clamp to the tile), so
 * a tile never reads past one period of its mask however large it is: importing the whole tile
 * read the bytes after the texture as texels (ocarina-of-time ab65ddd: adult Link's gauntlets; ssb64
 * stage grounds). PM64_TILEMASK=0 restores the whole-tile import (A/B). */
static int pm_tilemask = -1;
static inline bool pm_tilemask_on(void) {
    if (pm_tilemask < 0) { const char *e = getenv("PM64_TILEMASK"); pm_tilemask = !(e && e[0] == '0'); }
    return pm_tilemask != 0;
}
/* PM64_LOADTILE=0: LoadTile resizes the render tile, as before round 15 (A/B). */
static int pm_loadtile = -1;
static inline bool pm_loadtile_own(void) {
    if (pm_loadtile < 0) { const char *e = getenv("PM64_LOADTILE"); pm_loadtile = !(e && e[0] == '0'); }
    return pm_loadtile != 0;
}
static inline uint32_t pm_mask_period(uint32_t n, uint8_t mask) {
    return (mask != 0 && mask < 16 && n > (1u << mask) && pm_tilemask_on()) ? (1u << mask) : n;
}
/* The sampler's wrap for one axis: a clamped tile wider than its mask period repeats inside the
 * tile (the RDP clamps to the tile, then masks), so it samples as a wrap. A mirrored mask's period is two masks: the
 * dark rooms' light stencil (a 64-texel quarter circle, MIRROR|CLAMP, a 128-texel tile) is one whole circle, clamped
 * outside it, not a sheet of circles (the native game, M2). */
static inline uint8_t pm_tile_cm(uint8_t cm, uint8_t mask, uint16_t lo, uint16_t hi) {
    const int extent = ((int) hi - (int) lo + 4) / 4;
    if (mask != 0 && mask < 16 && (cm & G_TX_CLAMP) && extent > ((1 << mask) << ((cm & G_TX_MIRROR) ? 1 : 0)) &&
        pm_tilemask_on())
        cm &= (uint8_t) ~G_TX_CLAMP;
    return cm;
}
static inline void pm_render_tile_cm(void) {
    rdp.texture_tile.cms = pm_tile_cm(rdp.texture_tile.cms_raw, rdp.texture_tile.masks, rdp.texture_tile.uls, rdp.texture_tile.lrs);
    rdp.texture_tile.cmt = pm_tile_cm(rdp.texture_tile.cmt_raw, rdp.texture_tile.maskt, rdp.texture_tile.ult, rdp.texture_tile.lrt);
}
static uint8_t pm_tmem_hi[2048];        /* TMEM upper half as the RDP holds a TLUT: each entry x4 */
static int pm_prim_tile;                /* the tile primitives draw from (texrect tile / G_TEXTURE tile) */
static void pm_select_tile(int t) {     /* make tile t the render tile */
    pm_prim_tile = t;
    rdp.texture_tile.fmt = pm_tiles[t].fmt;
    rdp.texture_tile.siz = pm_tiles[t].siz;
    rdp.texture_tile.cms_raw = pm_tiles[t].cms;
    rdp.texture_tile.cmt_raw = pm_tiles[t].cmt;
    rdp.texture_tile.masks = pm_tiles[t].masks;
    rdp.texture_tile.maskt = pm_tiles[t].maskt;
    rdp.texture_tile.tmem_slot = (pm_tiles[t].tmem / 256) & 1;
    rdp.texture_tile.tmem = pm_tiles[t].tmem;
    rdp.texture_tile.line_size_bytes = pm_tiles[t].line * 8;
    rdp.texture_tile.palette = pm_tiles[t].palette;
    rdp.texture_tile.uls = pm_tiles[t].uls; rdp.texture_tile.ult = pm_tiles[t].ult;
    rdp.texture_tile.lrs = pm_tiles[t].lrs; rdp.texture_tile.lrt = pm_tiles[t].lrt;
    pm_render_tile_cm();
    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
    tri_state.valid = false;
}
/* Texture loads by TMEM address (ocarina-of-time 5025c0d). MK64 kept one load per TMEM half (tmem / 256), and
 * Paper Mario packs textures inside a half: the castle floor's 32x32 CI4 loads at TMEM 0 and a 16x16 at
 * 0x40 right after it, which replaced it, so the floor drew with the other texture. A tile reads the load
 * at its address, else the load its address falls inside, from that offset (pm_tile_src). A load drops
 * the records it overwrites. PM64_LTSLOTS=0: the two halves, as before (A/B). */
static int pm_ltslots = -1;
static inline bool pm_ltslots_on(void) {
    if (pm_ltslots < 0) { const char *e = getenv("PM64_LTSLOTS"); pm_ltslots = !(e && e[0] == '0'); }
    return pm_ltslots != 0;
}
static int gfx_lt_find(uint16_t tmem, bool exact_only) {
    int i;
    if (!pm_ltslots_on()) return (tmem / 256) & 1;
    for (i = 0; i < GFX_LT_SLOTS; i++)
        if (rdp.loaded_texture[i].valid && rdp.loaded_texture[i].tmem == tmem) return i;
    if (exact_only) return -1;
    for (i = 0; i < GFX_LT_SLOTS; i++) {
        const uint32_t lo = rdp.loaded_texture[i].tmem, hi = lo + (rdp.loaded_texture[i].size_bytes + 7) / 8;
        if (rdp.loaded_texture[i].valid && tmem > lo && tmem < hi) return i;
    }
    return 0;
}
/* The record a load to [tmem, tmem + size) writes: the one at the same address, else a free one, else the
 * oldest that neither texture unit reads. Records the load overlaps are gone (TMEM was overwritten). */
static int gfx_lt_slot_for_load(uint16_t tmem, uint32_t size_bytes) {
    int i, n, keep = -1;
    if (!pm_ltslots_on()) return (tmem / 256) & 1;
    const uint32_t lo = tmem, hi = lo + (size_bytes + 7) / 8;
    for (i = 0; i < GFX_LT_SLOTS; i++) {
        if (!rdp.loaded_texture[i].valid) continue;
        const uint32_t l2 = rdp.loaded_texture[i].tmem, h2 = l2 + (rdp.loaded_texture[i].size_bytes + 7) / 8;
        if (l2 == lo && keep < 0) keep = i;
        else if (l2 < hi && lo < h2) rdp.loaded_texture[i].valid = false;
    }
    if (keep >= 0) { i = keep; goto found; }
    for (i = 0; i < GFX_LT_SLOTS; i++)
        if (!rdp.loaded_texture[i].valid) goto found;
    for (n = 0; n < GFX_LT_SLOTS; n++) {
        i = (rdp.lt_next + n) % GFX_LT_SLOTS;
        if (rdp.loaded_texture[i].tmem != pm_tiles[pm_prim_tile].tmem && rdp.loaded_texture[i].tmem != pm_tiles[(pm_prim_tile + 1) & 7].tmem) break;
    }
    if (n == GFX_LT_SLOTS) i = rdp.lt_next % GFX_LT_SLOTS;
    rdp.lt_next = (uint8_t) ((i + 1) % GFX_LT_SLOTS);
found:
    rdp.loaded_texture[i].tmem = tmem;
    rdp.loaded_texture[i].valid = true;
    return i;
}
#endif
#ifdef PM64
static int pm_tex1_slot = 1; /* TMEM half TEXEL1's tile reads (import_texture; PM64_LTSLOTS=0 only) */
#endif
static inline int gfx_tex_slot(int tile) {
#ifdef PM64
    if (pm_ltslots_on()) return gfx_lt_find(rdp.texture_tile.tmem, false); /* TEXEL1's import swaps its tile in */
    return tile == 0 ? rdp.texture_tile.tmem_slot : pm_tex1_slot;
#else
    return tile == 0 ? rdp.texture_tile.tmem_slot : tile;
#endif
}
#ifdef PM64
/* The texels tile `tile` reads: its load, from the tile's offset into it (a mip level or a texture packed
 * behind another in one load). LoadTile rows sit the load tile's line apart in TMEM. */
static const uint8_t *pm_tile_src(int tile, uint32_t *size_bytes, uint32_t *stride_bytes, uint32_t *row_bytes) {
    const int s = gfx_tex_slot(tile);
    const uint8_t *p = rdp.loaded_texture[s].addr;
    uint32_t size = rdp.loaded_texture[s].size_bytes, stride = rdp.loaded_texture[s].stride_bytes, rb = rdp.loaded_texture[s].row_bytes;
    if (pm_ltslots_on() && p != NULL && rdp.loaded_texture[s].valid && rdp.texture_tile.tmem > rdp.loaded_texture[s].tmem) {
        const uint32_t off = (uint32_t) (rdp.texture_tile.tmem - rdp.loaded_texture[s].tmem) * 8u;
        if (stride == 0) {
            p += off; size = size > off ? size - off : 0;
        } else {
            const uint32_t lb = rdp.loaded_texture[s].line_bytes ? rdp.loaded_texture[s].line_bytes : (rb ? rb : 1);
            const uint32_t row = off / lb, col = off % lb, rows = rb ? size / rb : 0;
            p += row * stride + col;
            rb = rb > col ? rb - col : 0;
            size = rows > row ? (rows - row) * rb : 0;
        }
    }
    if (size_bytes) *size_bytes = size;
    if (stride_bytes) *stride_bytes = stride;
    if (row_bytes) *row_bytes = rb;
    return p;
}
#endif

/* Render-tile texture size in texels.  MK64 scrolls textures (Royal Raceway
 * dash pads, DK Jungle river, Koopa Beach waterfall, Banshee Boardwalk) by
 * rewriting only the tile's upper-left corner every frame
 * (find_and_set_tile_size) and leaving lrs/lrt at the full extent; no display
 * list in the game sets a non-zero corner otherwise.  So the corner is purely
 * a UV offset (applied in gfx_sp_tri_update_state) and the texture size is the
 * extent from the origin.  Sizing from lrs-uls / lrt-ult shrank the texture as
 * it scrolled and went negative once the corner passed the extent, which
 * rejected the import and left the previously bound texture on the pad
 * (issue #8: flashing red and black, stretched). */
#ifdef PM64
/* A wrapping tile whose mask period is wider than its TMEM line (gfx_tile_size): the RDP reads on into the next
 * rows. The import copies the rows through pm_wide_buf, zero past the load (pm_wide_loaded bytes). */
static int pm_wide_tile, pm_wide_on_ = -1;
static uint32_t pm_wide_loaded;
static int pm_wide_on(void) {
    if (pm_wide_on_ < 0) { const char *e = getenv("PM64_WIDETILE"); pm_wide_on_ = !(e && e[0] == '0'); }
    return pm_wide_on_;
}
#endif
static inline void gfx_tile_size(int tile, uint32_t *width, uint32_t *height) {
#ifdef PM64
    /* Paper Mario: the hardware rule, corner to corner, for LoadBlock too. Its
     * sprites and HUD icons put the tile at texel 256 ((256,256)-(303,303) for a
     * 48x48 raster), which MK64's extent-from-origin sizing imported as 304x304
     * of the wrong memory: the smeared sprites of round 1. */
    if (1) {
#else
    if (rdp.loaded_texture[gfx_tex_slot(tile)].stride_bytes != 0) {
#endif
        // LoadTile: TMEM holds exactly the sub-rectangle (uls,ult)-(lrs,lrt) of
        // a larger image (menu atlases, menu_items.c) and the corner is that
        // sub-rectangle's origin: size it corner to corner.
        int w = ((int) rdp.texture_tile.lrs - (int) rdp.texture_tile.uls + 4) / 4;
        int h = ((int) rdp.texture_tile.lrt - (int) rdp.texture_tile.ult + 4) / 4;
        *width = (uint32_t) (w < 1 ? 1 : w);
        *height = (uint32_t) (h < 1 ? 1 : h);
#ifdef PM64
        *width = pm_mask_period(*width, rdp.texture_tile.masks);   /* never past one mask period */
        *height = pm_mask_period(*height, rdp.texture_tile.maskt);
        /* ...and a wrapping axis (mask set, clamp off) reads a whole period from its corner, however
         * close its corners are: the RDP subtracts the corner, then masks, and the extent only bounds
         * a clamp (star-fox-64 7725ecc, for LoadBlock; TMEM is TMEM, so LoadTile too). The light
         * rays' scrolling TEXEL1 is a 64-texel window of a 256-wide pattern: it wrapped at 64. Only
         * while TMEM holds the period (the tile's line and the load). PM64_TILESCROLL=0: off (A/B). */
        uint32_t loaded, rb, img_stride;
        (void) pm_tile_src(tile, &loaded, &img_stride, &rb);
        pm_wide_tile = 0;
        static int sTileScroll = -1;
        if (sTileScroll < 0) { const char *e = getenv("PM64_TILESCROLL"); sTileScroll = !(e && e[0] == '0'); }
        if (sTileScroll && pm_tilemask_on()) {
            const uint8_t ms = rdp.texture_tile.masks, mt = rdp.texture_tile.maskt;
            const uint32_t line = rdp.texture_tile.line_size_bytes * (rdp.texture_tile.siz == G_IM_SIZ_32b ? 2u : 1u);
            if (ms != 0 && ms < 16 && !(rdp.texture_tile.cms_raw & G_TX_CLAMP) && *width < (1u << ms) &&
                tex_bytes_per_row(1u << ms, rdp.texture_tile.siz) <= line)
                *width = 1u << ms;
            if (mt != 0 && mt < 16 && !(rdp.texture_tile.cmt_raw & G_TX_CLAMP) && *height < (1u << mt) &&
                (1u << mt) * line <= loaded)
                *height = 1u << mt;
            /* ...and a period wider than the line (the "!" emote: a 16-texel window of a 96-wide I4 strip that
             * wraps at 128, its pop-in walking the window's corner from 128 down to 48): the RDP's texel
             * (s - corner) & 127 reads on into the next TMEM row past texel 96. It wrapped at the 16-texel
             * window and stayed on its first, tiny frame, low and to the left (the user, 27 Sep). Taken when
             * the image's rows sit a line apart as in TMEM (a LoadTile of the whole width); read through
             * pm_wide_buf, zero past the load. PM64_WIDETILE=0: off. */
            if (pm_wide_on() && ms != 0 && ms < 10 && !(rdp.texture_tile.cms_raw & G_TX_CLAMP) && *width < (1u << ms) &&
                tex_bytes_per_row(1u << ms, rdp.texture_tile.siz) > line && line != 0 && img_stride == line) {
                *width = 1u << ms;
                pm_wide_tile = 1;
                pm_wide_loaded = loaded;
            }
        }
        /* ...and never past what a LoadTile put in TMEM: a tile larger than its load reads stale TMEM on
         * the RDP, and the import (from the image, not TMEM) would read past the texture. */
        if (rb != 0 && pm_loadtile_own() && !pm_wide_tile) {
            const uint32_t bits = rdp.texture_tile.siz == G_IM_SIZ_4b ? 4 : rdp.texture_tile.siz == G_IM_SIZ_8b ? 8 :
                                  rdp.texture_tile.siz == G_IM_SIZ_16b ? 16 : 32;
            const uint32_t maxw = rb * 8 / bits, maxh = loaded / rb;
            if (maxw > 0 && *width > maxw) *width = maxw;
            if (maxh > 0 && *height > maxh) *height = maxh;
        }
#endif
    } else {
        // LoadBlock: the whole texture is loaded; a corner is only ever a
        // scroll offset (see above), the size is the extent from the origin.
        *width = (rdp.texture_tile.lrs + 4u) / 4u;
        *height = (rdp.texture_tile.lrt + 4u) / 4u;
    }
}

static uint32_t gfx_texture_content_hash(int tile, uint32_t fmt, uint32_t siz) {
#ifdef PM64
    uint32_t pm_size, stride;
    const uint8_t *src = pm_tile_src(tile, &pm_size, &stride, NULL);
#else
    const uint8_t *src = rdp.loaded_texture[gfx_tex_slot(tile)].addr;
    uint32_t stride = rdp.loaded_texture[gfx_tex_slot(tile)].stride_bytes;
#endif
    uint32_t h = 2166136261u;
    if (src == NULL) {
        return 0;
    }
    // A source below real RAM is an unresolved/low segment address (e.g. a
    // course texture at seg 5 whose base was not mapped): reading it faults on
    // real hardware (the emulator tolerates its mapped low memory).  Skip.
    if ((uintptr_t) src < 0x08000000u) {
        return 0;
    }
    // Texels inside the executable's text/data (ROM blobs, torch assets) never
    // change; only buffers in BSS / the game's pools need hashing.  A CI
    // texture still hashes a palette that lives in writable memory.
    if (gfx_is_static_memory(src)) {
        if (fmt == G_IM_FMT_CI && rdp.palette != NULL) {
            if (!gfx_is_static_memory(rdp.palette)) {
                return hash_bytes(h, rdp.palette, siz == G_IM_SIZ_4b ? 32 : 512);
            }
            // Static texels with a static palette: the palette's ADDRESS still
            // has to take part (Rainbow Road animates by cycling through
            // several TLUTs over the same CI texels).
            return (uint32_t) (uintptr_t) rdp.palette | 1u;
        }
        return 1;
    }
    if (stride == 0) {
#ifdef PM64
        uint32_t size = pm_size;
#else
        uint32_t size = rdp.loaded_texture[gfx_tex_slot(tile)].size_bytes;
#endif
        h = hash_bytes(h, src, size > 4096 ? 4096 : size);
    } else {
        uint32_t width, height;
        gfx_tile_size(tile, &width, &height);
        uint32_t row_bytes = tex_bytes_per_row(width, siz);
        uint32_t y;
        if (width * height > 8192) {
            height = 8192 / (width ? width : 1);
        }
        for (y = 0; y < height; y++) {
            h = hash_bytes(h, src + y * stride, row_bytes);
        }
    }
#ifdef PM64
    /* the framebuffer being drawn, as a texture: its rows change in place every frame and a wide row's hash samples
     * every fourth word, so the sync's generation takes part (pm_frame_texture_sync) */
    if (pm_cfb != NULL && src >= pm_cfb && src < pm_cfb + PM_CFB_BYTES) h = (h ^ pm_frame_gen) * 16777619u;
#endif
    if (fmt == G_IM_FMT_CI && rdp.palette != NULL) {
        h = hash_bytes(h, rdp.palette, siz == G_IM_SIZ_4b ? 32 : 512);
    }
    return h;
}

extern uint32_t port_time_us(void);
extern void port_profile_add(int slot, uint32_t us);

extern int texman_can_hold(unsigned int num, unsigned int bytes);

/* Upper bound of the VRAM the backend needs for the render tile as uploaded
 * (gfx_scegu_upload_texture pads to a power of two when it cannot swizzle). */
/* Mirrored wrap (G_TX_MIRROR) is not a GE texture mode.  Such textures are
 * imported at twice the size with the mirrored copy baked in, and the GE then
 * repeats (MIRROR|WRAP) or clamps (MIRROR|CLAMP) the doubled image; the UV
 * scale accounts for the doubling.  Returns the doubling that fits the
 * import buffer (bit 0: S, bit 1: T); 0 falls back to plain repeat. */
#define GFX_MAX_IMPORT_TEXELS 8192
#define GFX_MAX_MIRROR_TEXELS 16384
static uint8_t gfx_mirror_flags(uint32_t width, uint32_t height) {
    uint8_t m = 0;
    if (rdp.texture_tile.cms & G_TX_MIRROR) m |= 1;
    if (rdp.texture_tile.cmt & G_TX_MIRROR) m |= 2;
    if (m != 0 && (width << (m & 1)) * (height << ((m >> 1) & 1)) > GFX_MAX_MIRROR_TEXELS) {
        m = 0;
    }
    return m;
}

static uint32_t gfx_texture_upload_bytes(int tile, uint32_t fmt, uint32_t siz, uint8_t mirror) {
    uint32_t width, height;
    gfx_tile_size(tile, &width, &height);
    uint32_t bpp = ((fmt == G_IM_FMT_RGBA && siz == G_IM_SIZ_16b) || fmt == G_IM_FMT_CI) ? 2 : 4;
    uint32_t pw = 1, ph = 1;
    if (width * height > GFX_MAX_IMPORT_TEXELS) height = GFX_MAX_IMPORT_TEXELS / (width ? width : 1);
    width <<= mirror & 1;
    height <<= (mirror >> 1) & 1;
    while (pw < width) pw <<= 1;
    while (ph < height) ph <<= 1;
    return pw * ph * bpp;
}

/* The cache hit's sampler goes to the tile being imported. It went to tile 0 for any tile (the kit's sm64-port
 * origin, one texture unit): a hit on tile 1 gave tile 0's texture tile 1's wrap and clamp, and tile 0's node kept
 * its old values, so the draw state never put it back. PM64_SAMPLERFIX=0: as before (the A/B). PM64_SAMPLERLOG=1:
 * each frame, how many tile-1 hits changed tile 0's texture (the count per scene). */
static int gfx_samplerfix SAVESTATE_EXCLUDE = -1, gfx_samplerlog SAVESTATE_EXCLUDE = -1;
static uint32_t gfx_sampler_clobbers SAVESTATE_EXCLUDE;
static int gfx_sampler_tile(int tile, const struct TextureHashmapNode *hit) {
    if (gfx_samplerfix < 0) {
        const char *e = getenv("PM64_SAMPLERFIX"); gfx_samplerfix = !(e && e[0] == '0');
        e = getenv("PM64_SAMPLERLOG"); gfx_samplerlog = e && e[0] == '1';
    }
    if (tile == 1 && gfx_samplerlog && rendering_state.textures[0] != NULL && rendering_state.textures[0]->texture_id != hit->texture_id &&
        ((rendering_state.textures[0]->cms ^ hit->cms) & G_TX_CLAMP || (rendering_state.textures[0]->cmt ^ hit->cmt) & G_TX_CLAMP))
        gfx_sampler_clobbers++;
    return gfx_samplerfix ? tile : 0;
}
static bool gfx_texture_cache_lookup(int tile, struct TextureHashmapNode **n, const uint8_t *orig_addr, uint32_t fmt, uint32_t siz, uint8_t mirror) {
    size_t hash = (uintptr_t)orig_addr;
    uint32_t content_hash = gfx_texture_content_hash(tile, fmt, siz);
    uint32_t upload_bytes = gfx_texture_upload_bytes(tile, fmt, siz, mirror);
    struct TextureHashmapNode *stale = NULL;
    hash = (hash >> 5) & 0x3ff;
    struct TextureHashmapNode **node = &gfx_texture_cache.hashmap[hash];
    while (*node != NULL && *node - gfx_texture_cache.pool < (int)gfx_texture_cache.pool_pos) {
        if ((*node)->texture_addr == orig_addr && (*node)->fmt == fmt && (*node)->siz == siz && (*node)->mirror == mirror) {
            if ((*node)->content_hash == content_hash) {
                gfx_rapi->select_texture(tile, (*node)->texture_id);
                gfx_rapi->set_sampler_parameters(gfx_sampler_tile(tile, *node), (*node)->linear_filter, (*node)->cms, (*node)->cmt);
                (*node)->last_used_frame = gfx_frame_counter;
                *n = *node;
                return true;
            }
            // Same buffer, new contents: its VRAM can be reused unless a draw
            // queued this frame still needs the old texels, or the new image is
            // bigger than the slot and the arena cannot grow it (then it goes
            // through the fresh-texture path below, which may reset the arena).
            if (stale == NULL && (*node)->last_used_frame != gfx_frame_counter &&
                texman_can_hold((*node)->texture_id, upload_bytes)) {
                stale = *node;
            }
        }
        node = &(*node)->next;
    }
    if (stale != NULL) {
        if (gfx_debug_frame) port_log("  reimport tex %u addr %p fmt %u siz %u hash %08X->%08X\n", stale->texture_id, orig_addr, fmt, siz, stale->content_hash, content_hash);
        stale->content_hash = content_hash;
        stale->last_used_frame = gfx_frame_counter;
        stale->cms = 0;
        stale->cmt = 0;
        stale->linear_filter = false;
        gfx_rapi->select_texture(tile, stale->texture_id);
        texman_bind_tex(stale->texture_id); // select_texture is a no-op if already selected; the upload targets the bound texture
        gfx_rapi->set_sampler_parameters(tile, false, 0, 0);
        *n = stale;
        return false; // caller re-imports into the bound texture's memory
    }
    if (!gfx_vram_space_available() || !texman_can_hold((unsigned int) -1, upload_bytes) ||
        gfx_texture_cache.pool_pos == sizeof(gfx_texture_cache.pool) / sizeof(struct TextureHashmapNode) ||
        gfx_tex_stress()) {
        gfx_texture_cache_reset(true);
        node = &gfx_texture_cache.hashmap[hash];
    }
    *node = &gfx_texture_cache.pool[gfx_texture_cache.pool_pos++];
    (*node)->texture_id = gfx_rapi->new_texture();
    if (gfx_debug_frame) port_log("  newtex %u addr %p fmt %u siz %u hash %08X tile %d\n", (*node)->texture_id, orig_addr, fmt, siz, content_hash, tile);
    gfx_rapi->select_texture(tile, (*node)->texture_id);
    texman_bind_tex((*node)->texture_id);
    gfx_rapi->set_sampler_parameters(tile, false, 0, 0);
    (*node)->cms = 0;
    (*node)->cmt = 0;
    (*node)->linear_filter = false;
    (*node)->next = NULL;
    (*node)->texture_addr = orig_addr;
    (*node)->fmt = fmt;
    (*node)->siz = siz;
    (*node)->mirror = mirror;
    (*node)->content_hash = content_hash;
    (*node)->last_used_frame = gfx_frame_counter;
    *n = *node;
    return false;
}

/*
 * Convert the texels of the render tile to a PSP texture.
 *
 * The tile is (lrs - uls + 1) x (lrt - ult + 1) texels of texture_tile.fmt /
 * siz.  Rows are stride_bytes apart in memory (a G_LOADTILE row strip of a
 * wider image), or contiguous after a G_LOADBLOCK.
 */
#define TEXEL_ROW(y) (src + (y) * stride)

#ifdef PORT_GFX_DEBUG
#include <stdio.h>
#include "port.h"
/* On a debug frame, write every imported texture (as converted for the GE)
 * to PORT_SAVE_DIR/tex/ so broken textures can be inspected by eye. */
static void gfx_debug_dump_texture(const uint16_t *p16, const uint32_t *p32, uint32_t width, uint32_t height, uint8_t fmt, uint8_t siz, const uint8_t *src) {
    static int sCount;
    char name[96];
    FILE *fp;
    uint32_t x, y;
    if (!gfx_dump_textures || sCount >= 600) {
        return;
    }
    if (sCount == 0) {
        extern void port_fs_mkdir(const char *path);
        port_fs_mkdir(port_save_path("tex"));
    }
    {
        extern unsigned int psp_tex_bound;
        snprintf(name, sizeof(name), "%s" "tex/t%03d_id%u_fmt%u_siz%u_%ux%u_%08x.ppm", port_save_dir(), sCount++, psp_tex_bound, fmt, siz, (unsigned) width, (unsigned) height, (unsigned) (uintptr_t) src);
    }
    fp = fopen(name, "wb");
    if (fp == NULL) {
        return;
    }
    fprintf(fp, "P6\n%u %u\n255\n", (unsigned) width, (unsigned) height);
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint8_t rgb[3];
            if (p16 != NULL) {
                uint16_t p = p16[y * width + x]; // PSP 5551: r low
                rgb[0] = (p & 0x1F) << 3;
                rgb[1] = ((p >> 5) & 0x1F) << 3;
                rgb[2] = ((p >> 10) & 0x1F) << 3;
                if (!(p & 0x8000)) { rgb[0] = 255; rgb[1] = 0; rgb[2] = 255; } // alpha 0 -> magenta
            } else {
                uint32_t p = p32[y * width + x];
                rgb[0] = p & 0xFF; rgb[1] = (p >> 8) & 0xFF; rgb[2] = (p >> 16) & 0xFF;
                if ((p >> 24) == 0) { rgb[0] = 255; rgb[1] = 0; rgb[2] = 255; }
            }
            fwrite(rgb, 1, 3, fp);
        }
    }
    fclose(fp);
    port_log("  dumped %s\n", name);
}
#else
#define gfx_debug_dump_texture(a, b, c, d, e, f, g) ((void) 0)
#endif

static uint32_t tex_bytes_per_row(uint32_t width, uint32_t siz) {
    switch (siz) {
        case G_IM_SIZ_4b: return (width + 1) / 2;
        case G_IM_SIZ_8b: return width;
        case G_IM_SIZ_16b: return width * 2;
        default: return width * 4;
    }
}

static inline uint8_t nib(const uint8_t* row, uint32_t x) {
    return (row[x / 2] >> (4 - (x % 2) * 4)) & 0xF;
}

static inline uint16_t be16(const uint8_t* p) {
    return (uint16_t) ((p[0] << 8) | p[1]);
}

static inline uint16_t n64_rgba16_to_psp5551(uint16_t col16) {
    uint16_t a = col16 & 1;
    uint16_t r = (col16 >> 11) & 0x1f;
    uint16_t g = (col16 >> 6) & 0x1f;
    uint16_t b = (col16 >> 1) & 0x1f;
    return (uint16_t) ((a << 15) | (b << 10) | (g << 5) | r);
}

/* Upload `buf` (w x h texels of `bpp` bytes), doubled along the mirrored
 * axes with the mirrored copy baked in when `mirror` asks for it. */
static void gfx_upload_maybe_mirrored(const void *buf, uint32_t w, uint32_t h, uint32_t bpp, unsigned int type, uint8_t mirror) {
    static uint32_t mirror32[GFX_MAX_MIRROR_TEXELS] __attribute__((aligned(16)));
    uint32_t w2, h2, x, y;
    if (mirror == 0) {
        gfx_rapi->upload_texture((const uint8_t *) buf, w, h, type);
        return;
    }
    w2 = w << (mirror & 1);
    h2 = h << ((mirror >> 1) & 1);
    for (y = 0; y < h2; y++) {
        uint32_t sy = y < h ? y : 2 * h - 1 - y;
        if (bpp == 2) {
            const uint16_t *src = (const uint16_t *) buf + sy * w;
            uint16_t *dst = (uint16_t *) mirror32 + y * w2;
            for (x = 0; x < w2; x++) dst[x] = src[x < w ? x : 2 * w - 1 - x];
        } else {
            const uint32_t *src = (const uint32_t *) buf + sy * w;
            uint32_t *dst = mirror32 + y * w2;
            for (x = 0; x < w2; x++) dst[x] = src[x < w ? x : 2 * w - 1 - x];
        }
    }
    gfx_rapi->upload_texture((const uint8_t *) mirror32, w2, h2, type);
}

static void import_texture_any(int tile, uint8_t mirror) {
    static uint32_t out32[GFX_MAX_IMPORT_TEXELS] __attribute__((aligned(16)));
    uint16_t* out16 = (uint16_t*) out32;
    uint8_t fmt = rdp.texture_tile.fmt;
    uint8_t siz = rdp.texture_tile.siz;
    uint32_t width, height;
    gfx_tile_size(tile, &width, &height);
#ifdef PM64
    uint32_t stride;
    const uint8_t* src = pm_tile_src(tile, NULL, &stride, NULL);
#else
    const uint8_t* src = rdp.loaded_texture[gfx_tex_slot(tile)].addr;
    uint32_t stride = rdp.loaded_texture[gfx_tex_slot(tile)].stride_bytes;
#endif
    uint32_t x, y;

    if (width == 0 || height == 0) {
        return;
    }
    // Reject an unresolved/low segment source (see gfx_texture_content_hash):
    // reading it hard-faults on device.  Leave the previously-bound texture.
    if (src == NULL || (uintptr_t) src < 0x08000000u) {
        return;
    }
    if (stride == 0) {
        stride = tex_bytes_per_row(width, siz);
#ifdef PM64
        /* LoadBlock: TMEM rows are the render tile's line apart (x2 for 32-bit texels, whose
         * RG and BA halves each take a line), never tighter than the texels themselves. A
         * padded row or a tile cut to its mask period read the padding as texels
         * (super-smash-bros a55820b: HUD sprites). */
        const uint32_t ls = rdp.texture_tile.line_size_bytes * (siz == G_IM_SIZ_32b ? 2u : 1u);
        if (ls > stride) stride = ls;
#endif
    }
    if (width * height > GFX_MAX_IMPORT_TEXELS) {
        SUPPORT_CHECK(width * height <= GFX_MAX_IMPORT_TEXELS);
        height = GFX_MAX_IMPORT_TEXELS / width;
    }
#ifdef PM64
    if (pm_wide_tile) { /* rows of the whole period, on into the next row, zero past the load (gfx_tile_size) */
        static uint8_t pm_wide_buf[GFX_MAX_IMPORT_TEXELS * 4];
        const uint32_t rowb = tex_bytes_per_row(width, siz);
        for (y = 0; y < height; y++) {
            const uint32_t at = y * stride, n = at >= pm_wide_loaded ? 0 : pm_wide_loaded - at < rowb ? pm_wide_loaded - at : rowb;
            if (n) memcpy(pm_wide_buf + y * rowb, src + at, n);
            if (n < rowb) memset(pm_wide_buf + y * rowb + n, 0, rowb - n);
        }
        src = pm_wide_buf;
        stride = rowb;
    }
#endif
    if (gfx_debug_frame) port_log("  import fmt %d siz %d %ux%u stride %u addr %p\n", fmt, siz, width, height, stride, src);

    if (fmt == G_IM_FMT_RGBA && siz == G_IM_SIZ_16b) {
        for (y = 0; y < height; y++) {
            const uint8_t* row = TEXEL_ROW(y);
            for (x = 0; x < width; x++) {
                out16[y * width + x] = n64_rgba16_to_psp5551(be16(row + x * 2));
            }
        }
        gfx_upload_maybe_mirrored(out16, width, height, 2, GU_PSM_5551, mirror);
        gfx_debug_dump_texture(out16, NULL, width, height, fmt, siz, src);
#ifdef PORT_INPUT_SCRIPT
        if (gfx_dump_textures && width == 32 && height == 32 && !gDebugTex32Valid) {
            memcpy(gDebugTex32, out16, 32 * 32 * 2);
            gDebugTex32Valid = 1;
            port_log("debug: captured 32x32 texture from %p\n", src);
        }
#endif
        return;
    }
    if (fmt == G_IM_FMT_CI) {
        if (gfx_debug_frame) port_log("  ci import palette %p tex %p idx0 %02X %02X %02X %02X\n", rdp.palette, src, src[0], src[1], src[2], src[3]);
        for (y = 0; y < height; y++) {
            const uint8_t* row = TEXEL_ROW(y);
            for (x = 0; x < width; x++) {
                uint8_t idx = (siz == G_IM_SIZ_4b) ? nib(row, x) : row[x];
                out16[y * width + x] = n64_rgba16_to_psp5551(be16(rdp.palette + idx * 2));
            }
        }
        gfx_upload_maybe_mirrored(out16, width, height, 2, GU_PSM_5551, mirror);
#ifdef PORT_INPUT_SCRIPT
        if (gfx_dump_textures && width == 64 && height == 32 && siz == G_IM_SIZ_8b && gDebugKartTexCount < 2) {
            memcpy(gDebugKartTex[gDebugKartTexCount++], out16, 64 * 32 * 2);
        }
#endif
        gfx_debug_dump_texture(out16, NULL, width, height, fmt, siz, src);
        return;
    }
    {
        // Debug: ASCII dump of the first few 4-bit intensity glyphs.
        static int glyphs;
        if (gfx_debug_frame && fmt == G_IM_FMT_I && siz == G_IM_SIZ_4b && glyphs++ < 3) {
            char line[80];
            port_log("  I4 glyph %ux%u stride %u:\n", width, height, stride);
            for (y = 0; y < height && y < 24; y++) {
                const uint8_t* row = TEXEL_ROW(y);
                for (x = 0; x < width && x < 60; x++) {
                    line[x] = ".:-=+*#%@"[nib(row, x) * 9 / 16];
                }
                line[x] = 0;
                port_log("  |%s|\n", line);
            }
        }
    }
    // Everything else expands to 8888.
    for (y = 0; y < height; y++) {
        const uint8_t* row = TEXEL_ROW(y);
        for (x = 0; x < width; x++) {
            uint8_t r, g, b, a;
            if (fmt == G_IM_FMT_RGBA) { // 32-bit
                r = row[x * 4]; g = row[x * 4 + 1]; b = row[x * 4 + 2]; a = row[x * 4 + 3];
            } else if (fmt == G_IM_FMT_IA) {
                if (siz == G_IM_SIZ_4b) {
                    uint8_t v = nib(row, x);
                    r = g = b = SCALE_3_8(v >> 1);
                    a = (v & 1) ? 255 : 0;
                } else if (siz == G_IM_SIZ_8b) {
                    r = g = b = SCALE_4_8(row[x] >> 4);
                    a = SCALE_4_8(row[x] & 0xF);
                } else {
                    r = g = b = row[x * 2];
                    a = row[x * 2 + 1];
                }
            } else { // G_IM_FMT_I
                if (siz == G_IM_SIZ_4b) {
                    r = g = b = a = SCALE_4_8(nib(row, x));
                } else {
                    r = g = b = a = row[x];
                }
            }
            out32[y * width + x] = (uint32_t) r | ((uint32_t) g << 8) | ((uint32_t) b << 16) | ((uint32_t) a << 24);
        }
    }
    gfx_upload_maybe_mirrored(out32, width, height, 4, GU_PSM_8888, mirror);
    gfx_debug_dump_texture(NULL, out32, width, height, fmt, siz, src);
}

static void import_texture_desc(int tile);
static void import_texture(int tile) {
#ifdef PM64
    /* TEXEL1 samples tile (prim tile + 1) with that tile's own format, TMEM half and
     * palette (lit sprites: the raster through its base palette next to the same
     * raster through the RDP-computed shading palette). The kit's model has one
     * render-tile descriptor: swap tile+1 in for this import, then restore. */
    if (tile == 1) {
        const int t1 = (pm_prim_tile + 1) & 7;
        __typeof__(rdp.texture_tile) saved = rdp.texture_tile;
        rdp.texture_tile.fmt = pm_tiles[t1].fmt; rdp.texture_tile.siz = pm_tiles[t1].siz;
        rdp.texture_tile.cms_raw = pm_tiles[t1].cms; rdp.texture_tile.cmt_raw = pm_tiles[t1].cmt;
        rdp.texture_tile.masks = pm_tiles[t1].masks; rdp.texture_tile.maskt = pm_tiles[t1].maskt;
        rdp.texture_tile.palette = pm_tiles[t1].palette; rdp.texture_tile.line_size_bytes = pm_tiles[t1].line * 8;
        rdp.texture_tile.uls = pm_tiles[t1].uls; rdp.texture_tile.ult = pm_tiles[t1].ult;
        rdp.texture_tile.lrs = pm_tiles[t1].lrs; rdp.texture_tile.lrt = pm_tiles[t1].lrt;
        pm_render_tile_cm();
        pm_tex1_slot = (pm_tiles[t1].tmem / 256) & 1;
        rdp.texture_tile.tmem = pm_tiles[t1].tmem;
        import_texture_desc(tile);
        rdp.texture_tile = saved;
        return;
    }
#endif
    import_texture_desc(tile);
}
static void import_texture_desc(int tile) {
    uint8_t fmt = rdp.texture_tile.fmt;
    uint8_t siz = rdp.texture_tile.siz;
#ifdef PM64
    if (fmt == G_IM_FMT_CI) {
        const uint8_t *bank = pm_tlut[siz == G_IM_SIZ_4b ? rdp.texture_tile.palette & 15 : 0];
        if (bank) rdp.palette = bank;
    }
#endif
    // Reject a degenerate render-tile size (e.g. lrt < ult -> height 0, seen on
    // a DK Jungle terrain texture): a 0-dimension texture gets a cache node and
    // GE binding but no valid upload, and the sceGu texture setup then feeds the
    // hardware GE bogus params (log2(0)) -> device-only fault (the emulator's
    // software GE tolerates it).  Leave the previously-bound texture instead.
    uint32_t width, height;
    gfx_tile_size(tile, &width, &height);
    if (width == 0 || height == 0 || width > 1024 || height > 1024) {
        return;
    }
    uint8_t mirror = gfx_mirror_flags(width, height);
#ifdef PM64
    if (gfx_texture_cache_lookup(tile, &rendering_state.textures[tile], pm_tile_src(tile, NULL, NULL, NULL), fmt, siz, mirror)) {
#else
    if (gfx_texture_cache_lookup(tile, &rendering_state.textures[tile], rdp.loaded_texture[gfx_tex_slot(tile)].addr, fmt, siz, mirror)) {
#endif
        return;
    }
    import_texture_any(tile, mirror);
}

static inline float dot(const float a[3], const float b[3])
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

/* X31 (ocarina-of-time 6f1ac0e): the RSP normalizes at any length. The lights' and the look-at's directions come into
 * model space through the modelview, so a model drawn at a small scale gets them ~0.003 long (dot ~1e-5): the old
 * 1e-5 threshold skipped those, n.l came out ~0 and the model lost its directional light and its texgen shine.
 * PM64_NORMFIX=0: the old threshold. */
static void gfx_normalize_vector(float v[3]) {
    static int fix = -1;
    if (fix < 0) { const char *e = getenv("PM64_NORMFIX"); fix = !(e && e[0] == '0'); }
    float dot = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (fix ? dot > 1e-30f : dot > 0.00001f) {
        const float scale = 1.0f / sqrtf(dot);
        v[0] *= scale;
        v[1] *= scale;
        v[2] *= scale;
    }
}

static void gfx_transposed_matrix_mul(float res[3], const float a[3], const float b[4][4]) {
    res[0] = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
    res[1] = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
    res[2] = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
}

#ifdef PM64
/* Tile shifts (ocarina-of-time 5c60b2c, R29): the RDP scales S/T by a tile's shift before subtracting its corner
 * (1..10 = >> n, 11..15 = << 16 - n). The render tile's shift goes into the UV scale. The generic combiner
 * still samples TEXEL1 at tile 0's scale (no m1 draw needs more: bench log "TEXEL1 at a relative shift").
 * PM64_TILESHIFT=0: shifts ignored (A/B). */
static bool pm_tileshift_on(void) {
    static int sOn = -1;
    if (sOn < 0) { const char *e = getenv("PM64_TILESHIFT"); sOn = !(e && e[0] == '0'); }
    return sOn != 0;
}
static float gfx_tile_shift_scale(uint8_t shift) {
    if (!pm_tileshift_on() || shift == 0 || shift > 15) return 1.0f;
    return shift <= 10 ? 1.0f / (float) (1u << shift) : (float) (1u << (16 - shift));
}
/* PM64_LOOKAT=0: texgen from the fixed axes, ignoring gSPLookAt (A/B). */
static bool pm_lookat_on(void) {
    static int sOn = -1;
    if (sOn < 0) { const char *e = getenv("PM64_LOOKAT"); sOn = !(e && e[0] == '0'); }
    return sOn != 0;
}
/* PM64_TEXGENLIN=0: G_TEXTURE_GEN_LINEAR drawn as plain texgen (A/B). */
static bool pm_texgenlin_on(void) {
    static int sOn = -1;
    if (sOn < 0) { const char *e = getenv("PM64_TEXGENLIN"); sOn = !(e && e[0] == '0'); }
    return sOn != 0;
}
#endif
static void calculate_normal_dir(const Light_t *light, float coeffs[3]) {
    float light_dir[3] = {
        light->dir[0] / 127.0f,
        light->dir[1] / 127.0f,
        light->dir[2] / 127.0f
    };
    gfx_transposed_matrix_mul(coeffs, light_dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}

#if !defined(TARGET_PSP)
static void gfx_matrix_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] +
                        a[i][1] * b[1][j] +
                        a[i][2] * b[2][j] +
                        a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}
#else 
static void gfx_matrix_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
  	__asm__ volatile (
        ".set			push\n"					// save assember option
        ".set			noreorder\n"			// suppress reordering
		"lv.q   R000, 0  + %1\n"
		"lv.q   R001, 16 + %1\n"
		"lv.q   R002, 32 + %1\n"
		"lv.q   R003, 48 + %1\n"

		"lv.q   R100, 0  + %2\n"
		"lv.q   R101, 16 + %2\n"
		"lv.q   R102, 32 + %2\n"
		"lv.q   R103, 48 + %2\n"

		"vmmul.q   M700, M000, M100\n"

		"sv.q   R700, 0  + %0\n"
		"sv.q   R701, 16 + %0\n"
		"sv.q   R702, 32 + %0\n"
		"sv.q   R703, 48 + %0\n"
        ".set			pop\n"					// restore assember option
		: "=m" (*res) : "m" (*a) ,"m" (*b) : "memory" );
}
#endif

static void gfx_sp_matrix(uint8_t parameters, const int32_t *addr) {
    float matrix[4][4] __attribute__((aligned(16)));
    if (gfx_debug_frame) port_log("  mtx params %02X addr %p stack %d\n", parameters, addr, (int) rsp.modelview_matrix_stack_size);
#ifndef GBI_FLOATS
    // Original GBI where fixed point matrices are used
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            int32_t int_part = addr[i * 2 + j / 2];
            uint32_t frac_part = addr[8 + i * 2 + j / 2];
            matrix[i][j] = (int32_t)((int_part & 0xffff0000) | (frac_part >> 16)) / 65536.0f;
            matrix[i][j + 1] = (int32_t)((int_part << 16) | (frac_part & 0xffff)) / 65536.0f;
        }
    }
#else
    // For a modified GBI where fixed point values are replaced with floats
    memcpy(matrix, addr, sizeof(matrix));
#endif

    if (parameters & G_MTX_PROJECTION) {
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.P_matrix, matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.P_matrix, matrix, rsp.P_matrix);
        }
        {
            float wx = rsp.P_matrix[0][3], wy = rsp.P_matrix[1][3], wz = rsp.P_matrix[2][3];
            rsp.is_persp = (wx * wx + wy * wy + wz * wz) > 0.01f;
        }
#if defined(TARGET_PSP)
        /* Allocate space in DL for current proj matrix */
        void *matrix_inline = (void *)ALIGN((unsigned int)sceGuGetMemory(sizeof(rsp.P_matrix)+15), 16);
        memcpy(matrix_inline, rsp.P_matrix, sizeof(rsp.P_matrix));
#if defined(PORT_GE_TL) && defined(PORT_GE_TL_ASPECT)
        {
            float k = (4.0f / 3.0f) / ((float) gfx_current_dimensions.width / (float) gfx_current_dimensions.height);
            float (*Pm)[4] = (float (*)[4]) matrix_inline;
            Pm[0][0] *= k; Pm[1][0] *= k; Pm[2][0] *= k; Pm[3][0] *= k;
        }
#endif
#ifdef PORT_GE_TL
        (void) matrix_inline;
        gfx_flush(); // triangles buffered so far belong to the previous matrix
#else
        gfx_flush(); // triangles buffered so far belong to the previous matrix
        sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4 *)matrix_inline);
#endif
#else  /* !TARGET_PSP: software rasterizer — CPU transform uses rsp.MP_matrix directly, no GE upload */
        gfx_flush(); // triangles buffered so far belong to the previous matrix
#endif
    } else { // G_MTX_MODELVIEW
#ifdef PORT_GE_TL
        gfx_flush(); // draw pending verts with the CURRENT modelview before it changes
#endif
        if ((parameters & G_MTX_PUSH) && rsp.modelview_matrix_stack_size < 11) {
            ++rsp.modelview_matrix_stack_size;
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 2], sizeof(matrix));
        }
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
        }
        rsp.lights_changed = 1;
    }
    gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
    // Under GE_TL the GE applies MP as GU_PROJECTION on object-space vertices;
    // it is (re)set per batch in gfx_flush from the current rsp.MP_matrix.
}

static void gfx_sp_pop_matrix(uint32_t count) {
    if (gfx_debug_frame) port_log("  popmtx %u stack %d\n", (unsigned) count, (int) rsp.modelview_matrix_stack_size);
#ifdef PORT_GE_TL
    gfx_flush(); // draw pending verts with the current modelview before popping
#endif
    while (count--) {
        if (rsp.modelview_matrix_stack_size > 0) {
            --rsp.modelview_matrix_stack_size;
        }
    }
    if (rsp.modelview_matrix_stack_size > 0) {
        gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
    }
}

static float gfx_adjust_x_for_aspect_ratio(float x) {
    return x * (4.0f / 3.0f) / ((float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height);
}

struct ShaderProgram {
    bool enabled;
    uint32_t shader_id;
    struct CCFeatures cc;
    int mix;
    bool texture_used[2];
    int texture_ord[2];
    int num_inputs;
};

static void gfx_sp_vertex(size_t n_vertices, size_t dest_index, const Vtx *vertices) {
#ifdef PORT_EXP_NOVTX
    return;
#endif
    float temp_vec[4] __attribute__((aligned(16)));
    float proj_vec[4] __attribute__((aligned(16)));
    float view_vec[4] __attribute__((aligned(16)));
#ifdef PORT_PROFILE_DL
    uint32_t _pv0 = port_time_us();
#endif
    // Load the two matrices into VFPU registers once for the whole batch (the
    // matrix is constant across a gSPVertex load); the per-vertex asm below
    // only loads the vertex and multiplies.  The lighting path calls helpers
    // that may touch the VFPU, so it reloads afterwards (see below).
#if defined(TARGET_PSP)
    __asm__ volatile (
        "lv.q  c700,  0 + %0\n" "lv.q  c710, 16 + %0\n" "lv.q  c720, 32 + %0\n" "lv.q  c730, 48 + %0\n"
        "lv.q  c400,  0 + %1\n" "lv.q  c410, 16 + %1\n" "lv.q  c420, 32 + %1\n" "lv.q  c430, 48 + %1\n"
        :: "m"(*rsp.MP_matrix), "m"(*rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1])
    );
#endif
#if !defined(TARGET_PSP) && !defined(PORT_GE_CULL) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
    /* Device: both matrices' rows held for the batch; per vertex and column
     * ((v0*M0 + v1*M1) + v2*M2) + v3*M3, each product rounded before its add (NEON VMLA is not
     * fused): the scalar loop's order and rounding, 8 instructions instead of ~80 (ocarina-of-time
     * c935b73). */
    const float (*const nMP)[4] = rsp.MP_matrix;
    const float (*const nMV)[4] = rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1];
    const float32x4_t mp0 = vld1q_f32(nMP[0]), mp1 = vld1q_f32(nMP[1]), mp2 = vld1q_f32(nMP[2]), mp3 = vld1q_f32(nMP[3]);
    const float32x4_t mv0 = vld1q_f32(nMV[0]), mv1 = vld1q_f32(nMV[1]), mv2 = vld1q_f32(nMV[2]), mv3 = vld1q_f32(nMV[3]);
#endif
    /* The aspect quotient of gfx_adjust_x_for_aspect_ratio, divided once a batch instead of every
     * vertex (same operands: the same float). */
    const float aspect_q = (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const Vtx_t *v = &vertices[i].v;
        const Vtx_tn *vn = &vertices[i].n;
        struct LoadedVertex *d = &rsp.loaded_vertices[dest_index];
        int fog_alpha = -1; // >= 0: the RSP fog factor replaces the vertex alpha

        temp_vec[0] = v->ob[0];
        temp_vec[1] = v->ob[1];
        temp_vec[2] = v->ob[2];
        temp_vec[3] = 1.0f;

#ifndef PORT_GE_CULL
#if defined(TARGET_PSP)
        __asm__ volatile (
            "lv.q  c200, %2\n"
            "vtfm4.q c000, e700, c200\n" "vtfm4.q c100, e400, c200\n"
            "sv.q  c000, %0\n" "sv.q  c100, %1\n"
            : "=m"(*proj_vec), "=m"(*view_vec) : "m"(*temp_vec)
        );
#else
        /* proj = MP * v (clip), view = MV * v.  N64 row-vector convention:
         * out[c] = sum_j v[j] * M[j][c] (matches the C reference at gfx_debug_frame). */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        {
            float32x4_t p = vmulq_n_f32(mp0, temp_vec[0]), q = vmulq_n_f32(mv0, temp_vec[0]);
            p = vmlaq_n_f32(p, mp1, temp_vec[1]); q = vmlaq_n_f32(q, mv1, temp_vec[1]);
            p = vmlaq_n_f32(p, mp2, temp_vec[2]); q = vmlaq_n_f32(q, mv2, temp_vec[2]);
            p = vmlaq_n_f32(p, mp3, temp_vec[3]); q = vmlaq_n_f32(q, mv3, temp_vec[3]);
            vst1q_f32(proj_vec, p);
            vst1q_f32(view_vec, q);
        }
#else
        {
            const float (*_MP)[4] = rsp.MP_matrix;
            const float (*_MV)[4] = rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1];
            for (int _c = 0; _c < 4; _c++) {
                proj_vec[_c] = temp_vec[0]*_MP[0][_c] + temp_vec[1]*_MP[1][_c] + temp_vec[2]*_MP[2][_c] + temp_vec[3]*_MP[3][_c];
                view_vec[_c] = temp_vec[0]*_MV[0][_c] + temp_vec[1]*_MV[1][_c] + temp_vec[2]*_MV[2][_c] + temp_vec[3]*_MV[3][_c];
            }
        }
#endif
#endif
        /* Aspect widening is only meaningful for the perspective 3D world
         * (it trades a wider FOV for the square screen). Ortho draws -- the
         * whole MK64 HUD is ortho quads via set_matrix_hud_screen() -- already
         * map their own coordinate space exactly onto the viewport, so
         * widening them just overspills the screen edges: on a square output the item
         * box, position number and corner portraits clipped off. No-op at 4:3
         * (desktop 320x240), so it only changes the narrow CONVERT screen. */
        const float x = rsp.is_persp ? proj_vec[0] * (4.0f / 3.0f) / aspect_q : proj_vec[0]; /* gfx_adjust_x_for_aspect_ratio */
        const float y = proj_vec[1];
        const float z = proj_vec[2];
        float w = proj_vec[3];
        if (rsp.geometry_mode & G_FOG) {
            /* RSP fog: replace the shade alpha with the distance fog factor
             * (fog_z = z/w * mul + offset, 0..255). Was only computed on the
             * PORT_GE_TL path -- without it the fog blend in gfx_emit_vertex
             * used the model's baked alpha as a constant fog amount, pulling
             * whole fogged courses toward the fog colour (Toad's Turnpike
             * rendered near-black, device round 3). */
            float winv = (w > -0.001f && w < 0.001f) ? 32767.0f : 1.0f / w;
            float fog_z;
            if (winv < 0.0f) winv = 32767.0f;
            fog_z = z * winv * rsp.fog_mul + rsp.fog_offset;
            if (fog_z < 0.0f) fog_z = 0.0f;
            if (fog_z > 255.0f) fog_z = 255.0f;
            fog_alpha = (int) fog_z;
        }
#else
#ifndef PORT_GE_TL
        __asm__ volatile (
            "lv.q  c200, %1\n"
            "vtfm4.q c100, e400, c200\n"
            "sv.q  c100, %0\n"
            : "=m"(*view_vec) : "m"(*temp_vec)
        );
#endif
#endif

        short U = v->tc[0] * rsp.texture_scaling_factor.s >> 16;
        short V = v->tc[1] * rsp.texture_scaling_factor.t >> 16;
        
        if (rsp.geometry_mode & G_LIGHTING) {
            if (rsp.lights_changed) {
                for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                    calculate_normal_dir(&rsp.current_lights[i], rsp.current_lights_coeffs[i]);
                }
                static const Light_t lookat_x = {{0, 0, 0}, 0, {0, 0, 0}, 0, {127, 0, 0}, 0};
                static const Light_t lookat_y = {{0, 0, 0}, 0, {0, 0, 0}, 0, {0, 127, 0}, 0};
#ifdef PM64
                /* Texgen axes: the task's gSPLookAt, s from LOOKATX and t from LOOKATY, as the console's
                 * microcode does (goldeneye / ocarina-of-time 5c60b2c, against the z64 reference). Each task starts
                 * from the fixed axes (gfx_sp_reset). A zero vector gives n.l = 0 on every vertex, as on the RSP
                 * (normalising it would give NaNs). PM64_LOOKAT=0: the fixed axes always (A/B). */
                const Light_t *ls = &lookat_x, *lt = &lookat_y;
                if (pm_lookat_on()) { ls = &rsp.current_lookat[1]; lt = &rsp.current_lookat[0]; }
                if (ls->dir[0] | ls->dir[1] | ls->dir[2]) calculate_normal_dir(ls, rsp.current_lookat_coeffs[0]);
                else memset(rsp.current_lookat_coeffs[0], 0, sizeof(rsp.current_lookat_coeffs[0]));
                if (lt->dir[0] | lt->dir[1] | lt->dir[2]) calculate_normal_dir(lt, rsp.current_lookat_coeffs[1]);
                else memset(rsp.current_lookat_coeffs[1], 0, sizeof(rsp.current_lookat_coeffs[1]));
#else
                calculate_normal_dir(&lookat_x, rsp.current_lookat_coeffs[0]);
                calculate_normal_dir(&lookat_y, rsp.current_lookat_coeffs[1]);
#endif
                rsp.lights_changed = false;
            }
            // calculate_normal_dir may have used the VFPU; reload the matrices.
#if defined(TARGET_PSP)
            __asm__ volatile (
                "lv.q  c700,  0 + %0\n" "lv.q  c710, 16 + %0\n" "lv.q  c720, 32 + %0\n" "lv.q  c730, 48 + %0\n"
                "lv.q  c400,  0 + %1\n" "lv.q  c410, 16 + %1\n" "lv.q  c420, 32 + %1\n" "lv.q  c430, 48 + %1\n"
                :: "m"(*rsp.MP_matrix), "m"(*rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1])
            );
#endif
            
            unsigned int r = rsp.current_lights[rsp.current_num_lights - 1].col[0];
            unsigned int g = rsp.current_lights[rsp.current_num_lights - 1].col[1];
            unsigned int b = rsp.current_lights[rsp.current_num_lights - 1].col[2];
            
            for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                float intensity = 0;
                intensity += vn->n[0] * rsp.current_lights_coeffs[i][0];
                intensity += vn->n[1] * rsp.current_lights_coeffs[i][1];
                intensity += vn->n[2] * rsp.current_lights_coeffs[i][2];
                intensity /= 127.0f;
                if (intensity > 0.0f) {
                    r += intensity * rsp.current_lights[i].col[0];
                    g += intensity * rsp.current_lights[i].col[1];
                    b += intensity * rsp.current_lights[i].col[2];
                }
            }
            
            d->color.r = r > 255 ? 255 : r;
            d->color.g = g > 255 ? 255 : g;
            d->color.b = b > 255 ? 255 : b;
            
            if (rsp.geometry_mode & G_TEXTURE_GEN) {
                float dotx = 0, doty = 0;
                dotx += vn->n[0] * rsp.current_lookat_coeffs[0][0];
                dotx += vn->n[1] * rsp.current_lookat_coeffs[0][1];
                dotx += vn->n[2] * rsp.current_lookat_coeffs[0][2];
                doty += vn->n[0] * rsp.current_lookat_coeffs[1][0];
                doty += vn->n[1] * rsp.current_lookat_coeffs[1][1];
                doty += vn->n[2] * rsp.current_lookat_coeffs[1][2];
                
#ifdef PM64
                if ((rsp.geometry_mode & G_TEXTURE_GEN_LINEAR) && pm_texgenlin_on()) {
                    /* G_TEXTURE_GEN_LINEAR: the microcode maps the angle, not the cosine (GLideN64: acos(n.l)
                     * before the texture scale), s = acos(n.l) / pi * scale / 2 in S10.5 (ocarina-of-time 5c60b2c). */
                    const float cx = dotx / 127.0f < -1.0f ? -1.0f : dotx / 127.0f > 1.0f ? 1.0f : dotx / 127.0f;
                    const float cy = doty / 127.0f < -1.0f ? -1.0f : doty / 127.0f > 1.0f ? 1.0f : doty / 127.0f;
                    U = (int32_t)(acosf(cx) * (1.0f / 3.14159265f) * 0.5f * rsp.texture_scaling_factor.s);
                    V = (int32_t)(acosf(cy) * (1.0f / 3.14159265f) * 0.5f * rsp.texture_scaling_factor.t);
                } else
#endif
                {
                    U = (int32_t)((dotx / 127.0f + 1.0f) / 4.0f * rsp.texture_scaling_factor.s);
                    V = (int32_t)((doty / 127.0f + 1.0f) / 4.0f * rsp.texture_scaling_factor.t);
                }
            }
        } else {
            d->color.r = v->cn[0];
            d->color.g = v->cn[1];
            d->color.b = v->cn[2];
        }
        
        d->u = U;
        d->v = V;
        
        // Trivial-reject flags -- only meaningful for a PERSPECTIVE projection.
        // An orthographic draw (screen-space skybox / HUD) has w==1 and
        // screen-sized coords that would spuriously flag as far off-screen and
        // get trivially rejected (deleting the whole skybox).
        d->clip_rej = 0;
#ifndef PORT_GE_CULL
        // Clip-space coords: the CPU winding cull (gfx_sp_tri1) and the homogeneous
        // clipper (clipToHyperPlane reads &_x as a vec4) both operate on these.
        // beckerd's shipping GE config sets them in the PORT_GE_TL branch; the
        // pure-CPU config (our software build) must set them here.
        d->_x = proj_vec[0];
        d->_y = proj_vec[1];
        d->_z = proj_vec[2];
        d->_w = proj_vec[3];
        if (rsp.is_persp) {
            float aw = w < 0.0f ? -w : w;
            float gw = GUARD_BAND * aw;
            uint32_t rej = 0; /* one store: each |= on d->clip_rej was a load + or + store */
            if (x < -gw) rej |= X_POS;
            if (x > gw) rej |= X_NEG;
            if (y < -gw) rej |= Y_POS;
            if (y > gw) rej |= Y_NEG;
            if (x < -w) rej |= X_OUT_L;
            if (x > w) rej |= X_OUT_R;
            if (y < -w) rej |= Y_OUT_B;
            if (y > w) rej |= Y_OUT_T;
            // In front of the game's near plane (z_ndc < -1), or behind the eye:
            // a triangle with all three vertices here is invisible (the RSP
            // clips it away), so gfx_sp_tri1's trivial reject may drop it. This
            // was w < W_NEAR (24), which deleted every triangle within 24 units
            // of the camera (mario-kart-64 4f64001: kart sprites vanished).
            if (z < -w) rej |= Z_POS;
            // Beyond the far plane (the RSP clips there). Was z > 2|w|, which a standard perspective never reaches
            // (z/w < (f+n)/(f-n) ~ 1.01), so the far reject never fired. z > w only happens in front of the eye:
            // behind it z < w < 0 (SM64 5cd6699, MK64 round-24 review).
            if (z > w) rej |= Z_NEG;
            {   /* the RSP's guard band and the eye (GB_OUT, W_LOW above; round 16) */
                const float gb = gfx_clip_ratio * aw;
                if (x < -gb || x > gb || y < -gb || y > gb) rej |= GB_OUT;
                if (w < W_MIN) rej |= W_LOW;
            }
            d->clip_rej |= rej;
        }
#endif

#ifdef PORT_GE_TL
        d->x = v->ob[0]; // object space; the GE does modelview*projection
        d->y = v->ob[1];
        d->z = v->ob[2];
        d->w = 1.0f;
        // The GE can't near-clip MK64's non-standard projection (its large w
        // offset keeps clip.w > 0 behind the eye), so behind-camera geometry
        // would project to stretched garbage.  Compute just the view-space Z
        // cheaply (one dot product, not a full transform) and flag verts at or
        // behind the near plane so tri1 can drop those triangles.
        d->clip_rej = 0;
        if (rsp.is_persp) {
            // MK64 folds the camera into the projection: clip = MP * obj, and the
            // modelview is identity for the static course.  One VFPU vtfm4 (c700
            // holds MP, loaded once per batch) computes clip.x/y/z/w -- much
            // cheaper than 3 scalar dot products.  x/y/w give the homogeneous
            // winding for CPU back-face culling; w is the near-plane distance.
            __asm__ volatile (
                "lv.q  c200, %1\n"
                "vtfm4.q c100, e700, c200\n"
                "sv.q  c100, %0\n"
                : "=m"(*proj_vec) : "m"(*temp_vec)
            );
            d->_x = proj_vec[0];
            d->_y = proj_vec[1];
            d->_z = proj_vec[2];
            d->_w = proj_vec[3]; // clip.w = signed near-plane distance
            if (rsp.geometry_mode & G_FOG) {
                // The RSP overwrites the shade alpha with the fog factor
                // (fog_z = z/w * mul + offset, 0..255 = none..full fog); the
                // fog render modes then blend toward the fog colour by it
                // (see tri_state.use_fog in gfx_emit_vertex).
                float w = proj_vec[3];
                float winv = (w > -0.001f && w < 0.001f) ? 32767.0f : 1.0f / w;
                float fog_z;
                if (winv < 0.0f) winv = 32767.0f;
                fog_z = proj_vec[2] * winv * rsp.fog_mul + rsp.fog_offset;
                if (fog_z < 0.0f) fog_z = 0.0f;
                if (fog_z > 255.0f) fog_z = 255.0f;
                fog_alpha = (int) fog_z;
            }
            {
                float wz = (1.0f - GE_DEPTH_EPS) * proj_vec[3];
                // Behind the eye, or nearer than the game's near plane (z_ndc < -1).
                if (proj_vec[3] < GE_TL_NEAR || proj_vec[2] + wz < 0.0f) d->clip_rej = Z_POS;
                // Beyond the far plane (z_ndc > 1): the GE would drop the whole triangle.
                if (wz - proj_vec[2] < 0.0f) d->clip_rej |= Z_NEG;
            }
        } else if (gfx_hud_anchor) {
            /* Ortho HUD element: clip.x (w == 1) tells which screen edge it belongs to. */
            __asm__ volatile (
                "lv.q  c200, %1\n"
                "vtfm4.q c100, e700, c200\n"
                "sv.q  c100, %0\n"
                : "=m"(*proj_vec) : "m"(*temp_vec)
            );
            d->_x = proj_vec[3] != 0.0f ? proj_vec[0] / proj_vec[3] : proj_vec[0];
            d->_w = proj_vec[3];
        }
#else
        d->x = view_vec[0]; // view space; the GE applies the projection
        d->y = view_vec[1];
        d->z = view_vec[2];
        d->w = -view_vec[2]; // camera distance, for LOD
#endif
        if (gfx_debug_frame) {
            static int n;
            if (proj_vec[3] != 1.0f && n++ < 6) {
                if (n == 1) {
                    const float (*P)[4] = rsp.P_matrix;
                    port_log("  P rows: (%.3f,%.3f,%.3f,%.3f) (%.3f,%.3f,%.3f,%.3f) (%.3f,%.3f,%.3f,%.3f) (%.3f,%.3f,%.3f,%.3f)\n",
                             P[0][0], P[0][1], P[0][2], P[0][3], P[1][0], P[1][1], P[1][2], P[1][3], P[2][0], P[2][1], P[2][2], P[2][3], P[3][0], P[3][1], P[3][2], P[3][3]);
                }
                const float (*M)[4] = rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1];
                float cv[4], cc[4];
                int c;
                for (c = 0; c < 4; c++) {
                    cv[c] = temp_vec[0] * M[0][c] + temp_vec[1] * M[1][c] + temp_vec[2] * M[2][c] + M[3][c];
                    cc[c] = temp_vec[0] * rsp.MP_matrix[0][c] + temp_vec[1] * rsp.MP_matrix[1][c] + temp_vec[2] * rsp.MP_matrix[2][c] + rsp.MP_matrix[3][c];
                }
                port_log("  vtx obj (%.1f,%.1f,%.1f) vfpu view (%.1f,%.1f,%.1f,%.1f) C view (%.1f,%.1f,%.1f,%.1f) vfpu clip (%.1f,%.1f,%.1f,%.1f) C clip (%.1f,%.1f,%.1f,%.1f) M3 (%.1f,%.1f,%.1f,%.1f)\n",
                         temp_vec[0], temp_vec[1], temp_vec[2], view_vec[0], view_vec[1], view_vec[2], view_vec[3], cv[0], cv[1], cv[2], cv[3],
                         proj_vec[0], proj_vec[1], proj_vec[2], proj_vec[3], cc[0], cc[1], cc[2], cc[3], M[3][0], M[3][1], M[3][2], M[3][3]);
            }
        }

#ifndef PORT_GE_CULL
        d->_x = x;
        d->_y = y;
        d->_z = z;
        d->_w = w;
#endif

        /*@Note: this is a trainwreck*/
        /*if (rsp.geometry_mode & G_FOG) {
            if (fabsf(w) < 0.001f) {
                // To avoid division by zero
                w = 0.001f;
            }
            
            float winv = 1.0f / w;
            if (winv < 0.0f) {
                winv = 32767.0f;
            }
            
            float fog_z = z * winv * rsp.fog_mul + rsp.fog_offset;
            if (fog_z < 0) fog_z = 0;
            if (fog_z > 255) fog_z = 255;
            d->color.a = fog_z; // Use alpha variable to store fog factor
            //d->color.r = d->color.r + (rdp.fog_color.r - d->color.r) * (fog_z/255);
            //d->color.g = d->color.g + (rdp.fog_color.g - d->color.g) * (fog_z/255);
            //d->color.b = d->color.b + (rdp.fog_color.b - d->color.b) * (fog_z/255);
            
            d->color.r = d->color.r + (255 - d->color.r) * (fog_z/255);
            d->color.g = d->color.g + (0 - d->color.g) * (fog_z/255);
            d->color.b = d->color.b + (0 - d->color.b) * (fog_z/255);
            //d->color.r = 255-fog_z;
            //d->color.g = 255-fog_z;
            //d->color.b = 255-fog_z;
            d->color.a = 255;
        } else {
            d->color.a = v->cn[3];
        }*/
        d->color.a = fog_alpha >= 0 ? (uint8_t) fog_alpha : v->cn[3];
    }
#ifdef PORT_PROFILE_DL
    gfx_prof_vtx += port_time_us() - _pv0;
    gfx_prof_vtxcount += n_vertices;
#endif
#ifdef PORT_GE_TL
    if (gfx_hud_anchor && !rsp.is_persp) {
        float x0 = 1e9f, x1 = -1e9f;
        size_t i;
        for (i = 0; i < n_vertices; i++) {
            float x = (rsp.loaded_vertices[dest_index - n_vertices + i]._x + 1.0f) * HALF_SCREEN_WIDTH;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
        }
        hud_load_class = hud_class(x0, x1);
    }
#endif
}


/*
 * The GE has one texture stage (texture x vertex color), so the RDP color
 * combiner is approximated by evaluating (a - b) * c + d per channel with the
 * texel samples taken as 1.0 and handing the result to the GE as the vertex
 * color.  This is exact for the common TEXEL0 * X forms and a reasonable
 * approximation for the rest; it also keeps the combiner's alpha input (e.g.
 * PRIMITIVE alpha on particles), which the GE multiplies with the texel alpha.
 */
static inline int cc_eval_slot(uint8_t code, int one, const struct RGBA *shade, int lod, int ch) {
    switch (code) {
        case CC_0:
            return one ? 255 : 0;
        case CC_TEXEL0:
        case CC_TEXEL1:
        case CC_TEXEL0A:
            return 255;
        case CC_PRIM:
            return ((const uint8_t *) &rdp.prim_color)[ch];
        case CC_SHADE:
            return ((const uint8_t *) shade)[ch];
        case CC_ENV:
            return ((const uint8_t *) &rdp.env_color)[ch];
        case CC_LOD:
            return lod;
    }
    return 0;
}

#ifdef PM64
static bool pm_gen_active; /* the current draw uses the generic combiner (pm_gen_state) */
#endif
static void gfx_eval_vertex_color(uint32_t cc_id, const struct RGBA *shade, int lod, struct RGBA *out) {
    uint8_t *o = (uint8_t *) out;
    int ch;
#ifdef PM64
    if (pm_gen_active) { *out = *shade; return; } /* the backend evaluates the whole combiner per pixel */
#endif
#if !defined(TARGET_PSP)
    // A decal of the texture over an input by the texel's own alpha,
    // (TEXEL0 - X) * TEXEL0_ALPHA + X (G_CC_BLENDRGBA: Sherbet Land's penguin
    // eyes). With texels as 1.0 it folds to white; the software backend does
    // the per-pixel lerp itself (gfx_soft rgb_decal) and needs X.
    const uint32_t xb = (cc_id >> 3) & 7;
    const bool decal = (cc_id & 7) == CC_TEXEL0 && ((cc_id >> 6) & 7) == CC_TEXEL0A && xb == ((cc_id >> 9) & 7) &&
                       (xb == CC_PRIM || xb == CC_SHADE || xb == CC_ENV || xb == CC_LOD);
#endif

    // Note: with fog on, the RSP stores the fog factor in the shade alpha and
    // MK64's fogged combiners route it into the output alpha; blending that
    // against the sky/ground gradient the game draws first is the port's fog.
    for (ch = 0; ch < 4; ch++) {
        int k = (ch == 3) ? 1 : 0;
        uint32_t bits = cc_id >> (k * 12);
        int a = cc_eval_slot(bits & 7, (cc_id >> (28 + k)) & 1, shade, lod, ch);
        int b = cc_eval_slot((bits >> 3) & 7, 0, shade, lod, ch);
        int c = cc_eval_slot((bits >> 6) & 7, 0, shade, lod, ch);
        int d = cc_eval_slot((bits >> 9) & 7, (cc_id >> (30 + k)) & 1, shade, lod, ch);
        int v = ((a - b) * c) / 255 + d;
#if !defined(TARGET_PSP)
        if (decal && ch < 3) v = d;
#endif
        o[ch] = (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
    }
    // An empty alpha combiner (alpha unused: cleared by the caller) means opaque.
    if ((cc_id & 0xfff000) == 0 && !(cc_id & (CC_FLAG_ALPHA_A_ONE | CC_FLAG_ALPHA_D_ONE))) {
        out->a = 255;
    }
}



uint32_t gfx_prof_rebuilds;
#ifdef PM64
/* Paper Mario's combiners (both cycles) against what the kit's fast path computes
 * exactly: per channel group, texel x input, input alone, texel alone, or the decal
 * (TEXEL0 - X) * TEXEL0_ALPHA + X; a second cycle that passes COMBINED through.
 * Anything else -- TEXEL1 (lit sprites, windows, candles, boxes), a working second
 * cycle, COMBINED_ALPHA / LOD / ONE-as-multiplier inputs -- runs the generic per-pixel
 * RDP combiner in gfx_soft (gfx_soft_pm_generic). */
extern void gfx_soft_pm_generic(bool on, uint32_t w0, uint32_t w1, bool two_cycle, const uint8_t prim[4], const uint8_t env[4],
                                uint8_t prim_lod, int ds, int dt, const int32_t t1map[6]);
enum { M_COMB, M_T0, M_T1, M_PRIM, M_SHADE, M_ENV, M_ONE, M_ZERO, M_OTHER };
static int pm_mux_rgb_ab(int v, bool is_b) { return v <= 5 ? v : v == 6 ? (is_b ? M_OTHER : M_ONE) : v == 7 ? M_OTHER : M_ZERO; }
static int pm_mux_rgb_c(int v) { return v <= 5 ? v : v == 8 ? 100 /* T0 alpha */ : v >= 16 ? M_ZERO : M_OTHER; }
static int pm_mux_rgb_d(int v) { return v <= 5 ? v : v == 6 ? M_ONE : M_ZERO; }
static int pm_mux_a_abd(int v) { return v <= 5 ? v : v == 6 ? M_ONE : M_ZERO; }
static int pm_mux_a_c(int v) { return (v >= 1 && v <= 5) ? v : v == 7 ? M_ZERO : M_OTHER; }
/* One (A-B)*C+D group of cycle 0: representable by the fast path? */
static bool pm_group_fast(int a, int b, int c, int d, bool rgb) {
    if (a == M_T1 || b == M_T1 || c == M_T1 || d == M_T1 || a == M_COMB || b == M_COMB || c == M_COMB || d == M_COMB) return false;
    if (a == M_OTHER || b == M_OTHER || c == M_OTHER || d == M_OTHER) return false;
    const bool ta = a == M_T0, tb = b == M_T0, tc = c == M_T0 || c == 100, td = d == M_T0;
    if (!ta && !tb && !tc && !td) return true;                 /* no texel: a vertex colour */
    if (tb) return false;
    if (td) return a == b || c == M_ZERO;                       /* texel alone */
    if (rgb && ta && c == 100 && b == d) return true;           /* decal (TEXEL0 - X) * TEXEL0_ALPHA + X */
    return b == M_ZERO && d == M_ZERO && (ta != tc);            /* texel x input */
}
/* The raw mux fields of one cycle: rgb a b c d, alpha a b c d. */
/* In 1-cycle mode the RDP has no separate TEXEL1: angrylion's reference gives TEXEL0 (of the next pixel), as do the
 * hardware-checked plugins. The port read whatever texture was left bound to tile 1: the "Paper Mario" title card
 * after the prologue (kmr_21, PM_CC_2E: colour TEXEL0, alpha TEXEL1 x PRIM) came out on a black box on the device
 * (the user's IMG_0180, 28 Sep) and invisible in a direct start -- whichever texture came before. PM64_1CYCT1=0: as
 * before. */
static int pm_1cyc_t1_on(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("PM64_1CYCT1"); s = !(e && e[0] == '0'); }
    return s;
}
static void pm_cycle_fields(int k, int f[8]) {
    const uint32_t w0 = rdp.pm_cc_w0, w1 = rdp.pm_cc_w1;
    if (k == 0) {
        f[0] = (w0 >> 20) & 15; f[1] = (w1 >> 28) & 15; f[2] = (w0 >> 15) & 31; f[3] = (w1 >> 15) & 7;
        f[4] = (w0 >> 12) & 7;  f[5] = (w1 >> 12) & 7;  f[6] = (w0 >> 9) & 7;   f[7] = (w1 >> 9) & 7;
    } else {
        f[0] = (w0 >> 5) & 15;  f[1] = (w1 >> 24) & 15; f[2] = w0 & 31;         f[3] = (w1 >> 6) & 7;
        f[4] = (w1 >> 21) & 7;  f[5] = (w1 >> 3) & 7;   f[6] = (w1 >> 18) & 7;  f[7] = w1 & 7;
    }
}
/* Decide how this draw combines. Returns the kit combine code for the fast path and
 * sets *gen when only the generic per-pixel combiner is exact. 2-cycle combines reduce
 * to one effective cycle when cycle 2 passes COMBINED through, or when cycle 1 is the
 * mipmap blend (TEXEL1 - TEXEL0) * LOD_FRACTION + TEXEL0 -- Paper Mario's world
 * geometry (G_CC_TRILERP then x SHADE): the LOD fraction is 0 at this resolution, so
 * COMBINED is TEXEL0. *t0 / *t1: the texels the generic path must sample. */
static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
/* Bench A/B (round 10): 1 = never take the generic combiner, so the device can time what it costs. The draws then
 * use the reduced single-cycle combine: same geometry and spans, wrong colours. Never set in a game build. */
int pm_gen_forbid;
/* Paper Mario's shroud (round 3, the lead's Smash A_FOG finding): the first blender cycle BLEND_COLOR * FOG_ALPHA +
 * IN * (1 - FOG_ALPHA) (PM_RM_SHROUD, macros.h) tints what the combiner drew toward the blend colour by the fog colour's
 * alpha. It darkens the battle stage for every Star Power, Star Beam and Shooting Star (mdl_set_shroud_tint_params) and
 * a window behind another (draw_box's darkening). The kit's blender reads only the second cycle, so the stage stayed
 * lit; now such a draw takes the generic combiner, which applies the tint per pixel (gfx_soft_pm_shroud). 0: none.
 * PM64_SHROUD (port_debug.c) turns it on in any scene for a test. */
static int pm_shroud(void) {
    static int sOn = -1; /* PM64_SHROUDFX=0: the shroud ignored, as before (the A/B) */
    if (sOn < 0) { const char *e = getenv("PM64_SHROUDFX"); sOn = !(e && e[0] == '0'); }
    const uint32_t l = rdp.other_mode_l;
    if (!sOn || (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) != G_CYC_2CYCLE) return 0;
    if (((l >> 30) & 3) != G_BL_CLR_BL || ((l >> 26) & 3) != G_BL_A_FOG || ((l >> 22) & 3) != G_BL_CLR_IN || ((l >> 18) & 3) != G_BL_1MA)
        return 0;
    return rdp.fog_color.a >> 3; /* the blender's 5-bit factor */
}
extern void gfx_soft_pm_shroud(int f5, uint8_t r, uint8_t g, uint8_t b);
static uint32_t pm_draw_combine(bool *gen, bool *t0, bool *t1) {
    const uint32_t cyc = rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE);
    if (cyc == G_CYC_FILL || cyc == G_CYC_COPY) { /* no combiner: the kit's fill / copy override stands */
        *gen = *t0 = *t1 = false;
        return rdp.combine_mode;
    }
    const bool two_cycle = cyc == G_CYC_2CYCLE;
    int c0[8], c1[8], e[8];
    bool reducible = true;
    pm_cycle_fields(0, c0);
    pm_cycle_fields(1, c1);
    if (!two_cycle && pm_1cyc_t1_on()) { /* 1-cycle: TEXEL1 is TEXEL0 (pm_1cyc_t1_on) */
        for (int i = 0; i < 8; i++) {
            if (c0[i] == 2) c0[i] = 1;          /* TEXEL1 -> TEXEL0 (every slot's code 2) */
            if (i == 2 && c0[i] == 9) c0[i] = 8; /* TEXEL1_ALPHA -> TEXEL0_ALPHA (colour C) */
        }
    }
    memcpy(e, c0, sizeof(e));
    if (two_cycle) {
        const bool rz = c1[2] >= 16 || c1[0] == c1[1], az = c1[6] == 7 || c1[4] == c1[5];
        const bool pass1 = rz && c1[3] == 0 && az && c1[7] == 0;
        /* (T1 - T0) * LOD + T0 reduces to TEXEL0 because the kit models no mipmapping (LOD_FRACTION = 0). PRIM_LOD_FRAC
         * is the game's own constant, so it reduces only when it is 0 (round 14: a cross-fade by a nonzero PRIM_LOD_FRAC
         * would have drawn TEXEL0 alone; 0 draws on m1). */
        const bool plod0 = rdp.pm_prim_lod == 0;
        const bool trilerp = c0[0] == 2 && c0[1] == 1 && (c0[2] == 13 || (c0[2] == 14 && plod0)) && c0[3] == 1 &&
                             c0[4] == 2 && c0[5] == 1 && (c0[6] == 0 || (c0[6] == 6 && plod0)) && c0[7] == 1;
        if (pass1) {
            /* cycle 0 alone */
        } else if (trilerp) {
            memcpy(e, c1, sizeof(e));
            for (int i = 0; i < 8; i++) if (i != 6 && e[i] == 0) e[i] = 1; /* COMBINED -> TEXEL0 (alpha C has no COMBINED) */
            if (e[2] == 7) e[2] = 8;                                          /* COMBINED_ALPHA -> TEXEL0_ALPHA */
        } else {
            reducible = false;
        }
    }
    const int ra = pm_mux_rgb_ab(e[0], false), rb = pm_mux_rgb_ab(e[1], true), rc = pm_mux_rgb_c(e[2]), rd = pm_mux_rgb_d(e[3]);
    const int aa = pm_mux_a_abd(e[4]), ab = pm_mux_a_abd(e[5]), ac = pm_mux_a_c(e[6]), ad = pm_mux_a_abd(e[7]);
    *gen = !reducible || !pm_group_fast(ra, rb, rc, rd, true) || !pm_group_fast(aa, ab, ac, ad, false) || pm_shroud() > 0;
    if (pm_gen_forbid) *gen = false;
    *t0 = *t1 = false;
    for (int k = 0; k < (two_cycle ? 2 : 1); k++) {
        const int *f = k == 0 ? c0 : c1;
        for (int i = 0; i < 8; i++) {
            const bool u0 = f[i] == 1 || (i == 2 && f[i] == 8), u1 = f[i] == 2 || (i == 2 && f[i] == 9);
            if (k == 0 ? u0 : u1) *t0 = true; /* SWAP in cycle 2 */
            if (k == 0 ? u1 : u0) *t1 = true;
        }
    }
    if (*gen) return rdp.combine_mode;
    return color_comb(e[0], e[1], e[2], e[3]) | (color_comb(e[4], e[5], e[6], e[7]) << 12) |
           (e[0] == G_CCMUX_1 ? CC_FLAG_RGB_A_ONE : 0) | (e[3] == G_CCMUX_1 ? CC_FLAG_RGB_D_ONE : 0) |
           (e[4] == G_ACMUX_1 ? CC_FLAG_ALPHA_A_ONE : 0) | (e[7] == G_ACMUX_1 ? CC_FLAG_ALPHA_D_ONE : 0);
}
/* PM64_T1SCALE=0: the generic combiner samples TEXEL1 at tile 0's scale with a whole-texel offset (round 15). */
static int pm_t1scale_on(void) {
    static int sOn = -1;
    if (sOn < 0) { const char *e = getenv("PM64_T1SCALE"); sOn = !(e && e[0] == '0'); }
    return sOn;
}
/* A tile shift as a power of two: 1..10 divide S/T by 2^shift, 11..15 multiply by 2^(16 - shift) (R29). */
static int pm_shift_log2(uint8_t shift) {
    if (!pm_tileshift_on() || shift == 0 || shift > 15) return 0;
    return shift <= 10 ? -(int) shift : 16 - (int) shift;
}
/* PM64_TEXADD=0: 2D rects keep the last 3D draw's texture-add state, and generic draws may set it (round 15). */
static int pm_texadd_on(void) {
    static int sOn = -1;
    if (sOn < 0) { const char *e = getenv("PM64_TEXADD"); sOn = !(e && e[0] == '0'); }
    return sOn;
}
static int pm_genshade_on(void) {
    static int sOn = -1;
    if (sOn < 0) { const char *e = getenv("PM64_GENSHADE"); sOn = !(e && e[0] == '0'); }
    return sOn;
}
/* Import + bind the textures the generic combine reads and hand the backend its state. */
/* The generic draw's TEXEL1 follows TEXEL0's S/T: (s + a) * 2^k + c (gen_t1). pm_gen_setup notes k and whether
 * both texels are read, for the per-triangle UV drop (gfx_emit_triangle, pm_uv_step). */
static int pm_t1_k[2], pm_t1_linked;
static void pm_gen_setup(bool two_cycle, bool t0, bool t1) {
    pm_t1_linked = t0 && t1;
    pm_t1_k[0] = pm_t1_k[1] = 0;
    for (int pass = 0; pass < 2; pass++) { /* a reset under tile 1's import: tile 0 again (gfx_cache_epoch) */
    const uint32_t epoch = gfx_cache_epoch;
    gfx_tex_stress_hold = pass;
    for (int i = 0; i < 2; i++) {
        if (!(i == 0 ? t0 : t1)) continue;
        if (rdp.textures_changed[i]) {
            gfx_flush();
            import_texture(i);
            rdp.textures_changed[i] = false;
        }
        if (rendering_state.textures[i] != NULL) {
            const int ti = i == 0 ? pm_prim_tile : (pm_prim_tile + 1) & 7;
            gfx_rapi->select_texture(i, rendering_state.textures[i]->texture_id);
            gfx_rapi->set_sampler_parameters(i, false, pm_tile_cm(pm_tiles[ti].cms, pm_tiles[ti].masks, pm_tiles[ti].uls, pm_tiles[ti].lrs),
                                             pm_tile_cm(pm_tiles[ti].cmt, pm_tiles[ti].maskt, pm_tiles[ti].ult, pm_tiles[ti].lrt));
        }
    }
    gfx_tex_stress_hold = 0;
    if (epoch == gfx_cache_epoch || !gfx_epochfix_on()) break;
    }
    if (t0 && rendering_state.textures[0] != NULL) gfx_rapi->select_texture(0, rendering_state.textures[0]->texture_id);
    const int t1i = (pm_prim_tile + 1) & 7;
    const int ds = ((int) rdp.texture_tile.uls - (int) pm_tiles[t1i].uls) / 4, dt = ((int) rdp.texture_tile.ult - (int) pm_tiles[t1i].ult) / 4;
    const uint8_t prim[4] = { rdp.prim_color.r, rdp.prim_color.g, rdp.prim_color.b, rdp.prim_color.a };
    const uint8_t env[4] = { rdp.env_color.r, rdp.env_color.g, rdp.env_color.b, rdp.env_color.a };
    gfx_flush();
    /* TEXEL1 from TEXEL0's S/T (gen_t1 in gfx_soft.c; round 16, R29): tile 1's own shift and corner, the corners
     * with their quarter texels. PM64_T1SCALE=0: tile 0's scale and round 15's whole-texel offset. */
    int32_t t1map[6] = { ds * 65536, 0, 0, dt * 65536, 0, 0 };
    if (pm_t1scale_on()) {
        const int32_t half = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT ? 32768 : 0; /* gfx_sp's filt */
        const int t0i = pm_prim_tile;
        t1map[0] = (int32_t) rdp.texture_tile.uls * 16384 - half; t1map[1] = half - (int32_t) pm_tiles[t1i].uls * 16384;
        t1map[2] = pm_shift_log2(pm_tiles[t1i].shifts) - pm_shift_log2(pm_tiles[t0i].shifts);
        t1map[3] = (int32_t) rdp.texture_tile.ult * 16384 - half; t1map[4] = half - (int32_t) pm_tiles[t1i].ult * 16384;
        t1map[5] = pm_shift_log2(pm_tiles[t1i].shiftt) - pm_shift_log2(pm_tiles[t0i].shiftt);
        pm_t1_k[0] = t1map[2]; pm_t1_k[1] = t1map[5];
    }
    gfx_soft_pm_shroud(pm_shroud(), rdp.blend_color.r, rdp.blend_color.g, rdp.blend_color.b);
    { extern bool gfx_soft_1cyc_t1; gfx_soft_1cyc_t1 = pm_1cyc_t1_on(); }
    gfx_soft_pm_generic(true, rdp.pm_cc_w0, rdp.pm_cc_w1, two_cycle, prim, env, rdp.pm_prim_lod, ds, dt, t1map);
    if (t1 && pm_tileshift_on() && !pm_t1scale_on() && (pm_tiles[t1i].shifts != pm_tiles[pm_prim_tile].shifts || pm_tiles[t1i].shiftt != pm_tiles[pm_prim_tile].shiftt)) {
        static uint32_t sSeen[8][2]; static int sLogged; /* the generic path samples TEXEL1 at tile 0's scale: */
        int i = 0;                                          /* say once per combine where a draw would want more */
        while (i < sLogged && !(sSeen[i][0] == rdp.pm_cc_w0 && sSeen[i][1] == rdp.pm_cc_w1)) i++;
        if (i == sLogged && sLogged < 8) { sSeen[sLogged][0] = rdp.pm_cc_w0; sSeen[sLogged++][1] = rdp.pm_cc_w1; port_log("generic: TEXEL1 at a relative shift (tile %d %u,%u vs %u,%u), FC%06X %08X: drawn at tile 0's scale\n", t1i,
                                                pm_tiles[t1i].shifts, pm_tiles[t1i].shiftt, pm_tiles[pm_prim_tile].shifts, pm_tiles[pm_prim_tile].shiftt,
                                                (unsigned) (rdp.pm_cc_w0 & 0xffffff), (unsigned) rdp.pm_cc_w1); }
    }
    static int sGenLog = -1;
    if (sGenLog < 0) sGenLog = getenv("PM64_GENLOG") != NULL;
    if (sGenLog) { /* debug: which combines take the generic path */
        static uint32_t seen[64][2]; static int nseen;
        int i; for (i = 0; i < nseen; i++) if (seen[i][0] == rdp.pm_cc_w0 && seen[i][1] == rdp.pm_cc_w1) break;
        if (i == nseen && nseen < 64) { seen[nseen][0] = rdp.pm_cc_w0; seen[nseen++][1] = rdp.pm_cc_w1;
            port_log("generic: FC%06X %08X %s t0 %d t1 %d\n", (unsigned) rdp.pm_cc_w0, (unsigned) rdp.pm_cc_w1, two_cycle ? "2cyc" : "1cyc", t0, t1); }
    }
}
static void pm_gen_off(void) {
    if (pm_gen_active) { gfx_flush(); gfx_soft_pm_generic(false, 0, 0, false, NULL, NULL, 0, 0, 0, NULL); }
    pm_gen_active = false;
}
#endif
#ifdef PM64
/* G_ZS_PRIM: the prim depth in place of the pixel's (the battle-start swirl, screen_overlays.c OVERLAY_START_BATTLE).
 * The swirl writes the prim depth 0 in a star's shape with a blender that keeps the colour (depth only), then fills
 * the screen black at prim depth 20 with a depth compare, so the black misses the star and the scene shows through
 * it. Without either the black covered everything: the black screen before the prologue's Bowser battle (the user,
 * 29 Sep). gfx_soft.c does the two cases it takes: 1, a triangle's depth-only write; 2, a rectangle's compare. */
static uint16_t pm_prim_depth;
static int pm_primz_mode, pm_primz_z;
extern void gfx_soft_set_primz(int mode, int z);
static void pm_primz_sync(bool rect) {
    static int on = -1; /* PM64_PRIMZ=0: the prim depth ignored, as before (the A/B) */
    if (on < 0) { const char *e = getenv("PM64_PRIMZ"); on = !(e && e[0] == '0'); }
    int mode = 0;
    const uint32_t l = rdp.other_mode_l;
    if (on && (l & G_ZS_PRIM)) {
        const bool keeps_mem = ((l >> 30) & 3) == 1 && ((l >> 22) & 3) == 1; /* cycle 1: P = M = the memory colour */
        if (!rect && (l & Z_UPD) && keeps_mem) mode = 1;
        else if (rect && (l & Z_CMP)) mode = 2;
    }
    const int z = (int) (pm_prim_depth & 0x7FFF) * 2; /* 15-bit N64 depth to the renderer's 16-bit, larger = farther */
    if (mode != pm_primz_mode || (mode && z != pm_primz_z)) {
        gfx_flush();
        gfx_soft_set_primz(mode, z);
        pm_primz_mode = mode;
        pm_primz_z = z;
    }
}
#endif
static void gfx_tri_rebuild_state(struct LoadedVertex *v1) {
    gfx_prof_rebuilds++;
#ifdef PM64
    pm_primz_sync(false);
#endif
    bool depth_test = (rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER;
    if (depth_test != rendering_state.depth_test) {
        gfx_flush();
        gfx_rapi->set_depth_test(depth_test);
        rendering_state.depth_test = depth_test;
    }
    bool z_upd = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    if (z_upd != rendering_state.depth_mask) {
        gfx_flush();
        gfx_rapi->set_depth_mask(z_upd);
        rendering_state.depth_mask = z_upd;
    }
    bool zmode_decal = (rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (zmode_decal != rendering_state.decal_mode) {
        gfx_flush();
        gfx_rapi->set_zmode_decal(zmode_decal);
        rendering_state.decal_mode = zmode_decal;
    }
    if (rdp.viewport_or_scissor_changed) {
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush();
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush();
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }
#ifdef PM64
    bool pmg, pmt0, pmt1;
    uint32_t cc_id = pm_draw_combine(&pmg, &pmt0, &pmt1);
#else
    uint32_t cc_id = rdp.combine_mode;
#endif
    // Whether the framebuffer blend uses the pixel alpha: the last blender
    // cycle's "b" mux is 1-A (G_BL_1MA).  In 2-cycle mode that is cycle 2
    // (bits 16-17); in 1-cycle mode bits 18-19.  sm64-port only tested bit 18
    // (G_BL_A_MEM), which misreads G_RM_OPA_SURF (B mux = G_BL_1, binary 10:
    // blender = IN*0 + IN*1, alpha ignored) as alpha-blended -- SM64 never
    // uses the non-AA OPA_SURF so it never showed, but MK64's menu setup DLs
    // (D_02007708, character-select cells) do: their art keeps black texels
    // with the RGBA16 alpha bit clear and the RDP still draws them opaque.
    // MK64's fogged render modes (FOG_SHADE_A + an opaque cycle 2) blended
    // with the fog amount as opacity before -- Moo Moo Farm's terrain let the
    // ground gradient show through as green patches (issue #1).
    bool two_cycle = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE;
    bool use_alpha = two_cycle ? ((rdp.other_mode_l >> 16) & 3) == G_BL_1MA : ((rdp.other_mode_l >> 18) & 3) == G_BL_1MA;
#ifdef PM64
    /* The blender only runs with FORCE_BL (or on coverage, which we do not model): Paper
     * Mario's background strips are TEXEL0 / SHADE-alpha texrects in render mode 0, where
     * alpha is 0 and the RDP writes the texel as is. Reading the B mux alone blended them
     * away (a black sky). */
    if (!(rdp.other_mode_l & FORCE_BL)) use_alpha = false;
#endif
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    bool use_noise = (rdp.other_mode_l & G_AC_DITHER) == G_AC_DITHER;
    if (texture_edge) {
        use_alpha = true;
    }
    {   /* X12 (ocarina-of-time 7120396): a texture edge without FORCE_BL (G_RM_AA_ZB_TEX_EDGE, Paper Mario's
         * RENDER_MODE_ALPHATEST: leaves, grass, fences, 101 uses) takes its coverage from the texel's alpha and the RDP
         * never blends it with memory: the texel's own colour or nothing. The kit blended it by alpha, which mixed the
         * soft edge with whatever was drawn behind it first. Triangles only; rectangles keep blending.
         * OFF in Paper Mario (round 4): the N64's VI anti-aliasing softens these edges and the port has none, so
         * the blend is the nearer stand-in -- unblended, the trees' and bushes' outlines came out hard and jagged,
         * 14% further from z64 at the same instant on the changed pixels (all 6 scenes with changes worse).
         * PM64_EDGEOPAQUE=1: on. */
        static int sEdgeOp = -1;
        extern void gfx_soft_set_edge_opaque(bool on);
        if (sEdgeOp < 0) { const char *e = getenv("PM64_EDGEOPAQUE"); sEdgeOp = e != NULL && e[0] == '1'; }
        const bool eo = sEdgeOp && texture_edge && !(rdp.other_mode_l & FORCE_BL);
        if (eo != rendering_state.edge_opaque) {
            gfx_flush();
            gfx_soft_set_edge_opaque(eo);
            rendering_state.edge_opaque = eo;
        }
    }
    {   /* PM64_FILLRULE (gfx_soft.c own_edge; ocarina-of-time a7c4909): a blended or alpha-tested draw without AA_EN --
         * the RDP writes such a pixel only when one fixed sample of it is covered, so each pixel along an edge two
         * triangles share is drawn once (the inclusive row solve blended it twice: a bright one-pixel line along
         * the diagonal of a see-through quad). With AA_EN both triangles write the pixel they share (partial
         * coverage each): left as it was. PM64_FILLRULE=0 (environment or debug.cfg): every draw takes the
         * inclusive row solve, as before. */
        static int sFillRule = -1;
        extern void gfx_soft_set_own_edges(bool on);
        if (sFillRule < 0) { const char *e = getenv("PM64_FILLRULE"); sFillRule = !(e != NULL && e[0] == '0'); }
        const bool own = sFillRule && use_alpha && !(rdp.other_mode_l & AA_EN);
        if (own != rendering_state.own_edges) {
            gfx_flush();
            gfx_soft_set_own_edges(own);
            rendering_state.own_edges = own;
        }
    }
    if (use_alpha) cc_id |= SHADER_OPT_ALPHA;
    if (use_fog) cc_id |= SHADER_OPT_FOG;
    if (texture_edge) cc_id |= SHADER_OPT_TEXTURE_EDGE;
    if (use_noise) cc_id |= SHADER_OPT_NOISE;
    if (!use_alpha) {
        cc_id &= ~0xfff000;
    }
    struct ColorCombiner *comb = gfx_lookup_or_create_color_combiner(cc_id);
    struct ShaderProgram *prg = comb->prg;
    if (prg != rendering_state.shader_program) {
        gfx_flush();
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(prg);
        rendering_state.shader_program = prg;
    }
    if (gfx_debug_frame) {
        static uint32_t last_cc, last_ol;
        if (cc_id != last_cc || rdp.other_mode_l != last_ol) {
            port_log("  tri state cc %08X othermode_l %08X h %08X prim %02X%02X%02X%02X env %02X%02X%02X%02X\n", (unsigned) cc_id, (unsigned) rdp.other_mode_l, (unsigned) rdp.other_mode_h, rdp.prim_color.r, rdp.prim_color.g, rdp.prim_color.b, rdp.prim_color.a, rdp.env_color.r, rdp.env_color.g, rdp.env_color.b, rdp.env_color.a);
            last_cc = cc_id; last_ol = rdp.other_mode_l;
        }
    }
    if (use_alpha != rendering_state.alpha_blend) {
        gfx_flush();
        gfx_rapi->set_use_alpha(use_alpha);
        rendering_state.alpha_blend = use_alpha;
    }
    uint8_t num_inputs;
    bool used_textures[2];
    gfx_rapi->shader_get_info(prg, &num_inputs, used_textures);
    for (int pass = 0; pass < 2; pass++) { /* a reset under tile 1's import: tile 0 again (gfx_cache_epoch) */
    const uint32_t epoch = gfx_cache_epoch;
    gfx_tex_stress_hold = pass;
    for (int i = 0; i < 2; i++) {
        if (used_textures[i]) {
            if (rdp.textures_changed[i]) {
                gfx_flush();
                import_texture(i);
                rdp.textures_changed[i] = false;
            }
            bool linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
            if (rendering_state.textures[i] == NULL) {
                // Texture unit flagged as used but never successfully imported
                // (skipped by the guards above): don't dereference a null node.
            } else if (linear_filter != rendering_state.textures[i]->linear_filter || rdp.texture_tile.cms != rendering_state.textures[i]->cms || rdp.texture_tile.cmt != rendering_state.textures[i]->cmt) {
                gfx_flush();
                gfx_rapi->set_sampler_parameters(i, linear_filter, rdp.texture_tile.cms, rdp.texture_tile.cmt);
                rendering_state.textures[i]->linear_filter = linear_filter;
                rendering_state.textures[i]->cms = rdp.texture_tile.cms;
                rendering_state.textures[i]->cmt = rdp.texture_tile.cmt;
            }
        }
    }
    gfx_tex_stress_hold = 0;
    if (epoch == gfx_cache_epoch || !gfx_epochfix_on()) break;
    }
    // The GE has one texture unit: whatever was bound last (possibly tile 1's
    // mip level, or a texture just uploaded) must give way to tile 0's texture.
    if (used_textures[0] && rendering_state.textures[0] != NULL) {
        gfx_rapi->select_texture(0, rendering_state.textures[0]->texture_id);
    }
#ifdef PM64
    if (pmg) {
        pm_gen_setup(two_cycle, pmt0, pmt1);
        pm_gen_active = true;
        used_textures[0] = pmt0 || pmt1; /* emit UVs */
    } else {
        pm_gen_off();
    }
#endif

#if defined(PORT_GE_CULL) && !defined(PORT_GE_TL)
    {
        extern void gfx_scegu_set_cull_mode(uint32_t cull);
        gfx_scegu_set_cull_mode(rsp.geometry_mode & G_CULL_BOTH);
    }
#endif
    tri_state.cc_id = cc_id;
    tri_state.comb = comb;
    tri_state.use_fog = use_fog;
    tri_state.use_texture = used_textures[0] || used_textures[1];
    if (tri_state.use_texture) {
        uint32_t tw, th;
        float tex_width, tex_height;
        gfx_tile_size(0, &tw, &th);
        tex_width = (float) tw;
        tex_height = (float) th;
        // A degenerate render-tile size (e.g. lrt < ult -> height 0 on a DK
        // Jungle terrain texture) would make 1/tex_height = Inf/NaN, which flows
        // into every vertex UV and faults the hardware GE (the host FPU/GE just
        // ignores it -- why the emulator never crashed).  Clamp to >= 1 texel.
        if (tex_width < 1.0f) tex_width = 1.0f;
        if (tex_height < 1.0f) tex_height = 1.0f;
        // A mirrored texture was imported at twice the size: one period of the
        // mirror pattern is the whole doubled image.
        tri_state.mirror = rendering_state.textures[0] != NULL ? rendering_state.textures[0]->mirror : 0;
        if (tri_state.mirror & 1) tex_width *= 2.0f;
        if (tri_state.mirror & 2) tex_height *= 2.0f;
        float filt = ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) ? 0.5f : 0.0f;
        float inv_w = 1.0f / tex_width, inv_h = 1.0f / tex_height;
#ifdef PM64
        /* The render tile's shift scales S/T before the corner is subtracted (R29, above). */
        tri_state.u_scale = inv_w / 32.0f * gfx_tile_shift_scale(pm_tiles[pm_prim_tile].shifts);
        tri_state.v_scale = inv_h / 32.0f * gfx_tile_shift_scale(pm_tiles[pm_prim_tile].shiftt);
#else
        tri_state.u_scale = inv_w / 32.0f;
        tri_state.v_scale = inv_h / 32.0f;
#endif
        tri_state.u_off = (filt - rdp.texture_tile.uls * 8 / 32.0f) * inv_w;
        tri_state.v_off = (filt - rdp.texture_tile.ult * 8 / 32.0f) * inv_h;
    }
    // Classify the combiner once: does the vertex colour depend on the shade?
    if (comb->color_mode < 0) {
        bool uses_shade = false, uses_lod = false;
        int k;
        for (k = 0; k < 8; k++) {
            uint8_t code = (cc_id >> (k * 3)) & 7;
            if (code == CC_SHADE) uses_shade = true;
            if (code == CC_LOD) uses_lod = true;
        }
        comb->needs_lod = uses_lod;
        comb->alpha_255 = false;
        if (!uses_shade && !uses_lod) {
            comb->color_mode = 1;
        } else if (!uses_lod) {
            // Identity on two probes => plain shade copy (alpha copied or 255).
            struct RGBA p1 = { 10, 20, 30, 40 }, p2 = { 200, 100, 50, 150 }, o1, o2;
            gfx_eval_vertex_color(cc_id, &p1, 0, &o1);
            gfx_eval_vertex_color(cc_id, &p2, 0, &o2);
            if (o1.r == p1.r && o1.g == p1.g && o1.b == p1.b && o2.r == p2.r && o2.g == p2.g && o2.b == p2.b &&
                ((o1.a == p1.a && o2.a == p2.a) || (o1.a == 255 && o2.a == 255))) {
                comb->color_mode = 0;
                comb->alpha_255 = (o1.a == 255 && o2.a == 255);
            } else {
                comb->color_mode = 2;
            }
        } else {
            comb->color_mode = 2;
        }
    }
    tri_state.color_mode = comb->color_mode;
    tri_state.alpha_255 = comb->alpha_255;
    tri_state.needs_lod = comb->needs_lod;
#ifdef PM64
    /* The generic combiner reads SHADE per pixel, so its vertices carry the raw shade, alpha included (round 16).
     * The classification above works on cc_id, the first cycle's reduction: a combine that reads SHADE only in its
     * second cycle came out as a constant colour, and one whose first cycle ignores the shade alpha got alpha 255.
     * The fountain outside Peach's castle, T1 x T0 then x SHADE with the water's edges at vertex alpha 0, drew at
     * alpha 0: invisible. PM64_GENSHADE=0 restores the classification (A/B). */
    if (pmg && pm_genshade_on()) { tri_state.color_mode = 0; tri_state.alpha_255 = false; }
#endif
    // "(1 - X) * TEXEL0 + Y", Y a constant colour input: MK64's kart tint,
    // (1 - ENV) * TEXEL0 + PRIM (func_8004B614).  The star and lightning cycle
    // PRIM; shadows and the Luigi Raceway tunnel raise ENV.  Evaluating the
    // combiner with TEXEL0 = 1 saturates the vertex colour to white and the
    // GE's modulate then drops the tint entirely (issue #15).  With a non-zero
    // Y draw texel + Y through the GE's ADD function with Y as the vertex
    // colour: exact for X = 0, the only case the game tints in.
    {
        bool add = false;
        uint32_t bits = cc_id & 0xfff;
        uint8_t a = bits & 7, c = (bits >> 6) & 7, d = (bits >> 9) & 7;
        if (comb->color_mode == 1 && a == CC_0 && ((cc_id >> 28) & 1) && c == CC_TEXEL0 && (d == CC_PRIM || d == CC_ENV) &&
            !((cc_id >> 30) & 1)) {
            const struct RGBA *y = d == CC_PRIM ? &rdp.prim_color : &rdp.env_color;
            if ((y->r | y->g | y->b) != 0) {
                add = true;
            }
        }
#ifdef PM64
        if (pmg && pm_texadd_on()) add = false; /* the generic combiner computes the tint itself (B5, above) */
#endif
        tri_state.tex_add = add;
        if (add != rendering_state.tex_add) {
            extern void gfx_scegu_set_texfunc_add(bool on);
            gfx_flush();
            gfx_scegu_set_texfunc_add(add);
            rendering_state.tex_add = add;
        }
    }
    if (comb->color_mode == 1) {
        struct RGBA dummy = { 0, 0, 0, 0 };
        gfx_eval_vertex_color(cc_id, &dummy, 0, &tri_state.const_color);
        if (tri_state.tex_add) {
            const struct RGBA *y = (((cc_id >> 9) & 7) == CC_PRIM) ? &rdp.prim_color : &rdp.env_color;
            tri_state.const_color.r = y->r;
            tri_state.const_color.g = y->g;
            tri_state.const_color.b = y->b; // alpha stays the combiner's (PRIM_A * TEXEL0_A -> GE: Af * At)
        }
    }
    tri_state.valid = true;
    (void) v1;
}


/* Emit one vertex of the current triangle into the GE buffer. */
static inline void gfx_emit_vertex(const struct LoadedVertex *cv, uint32_t cc_id, int lod) {
    psp_fast_t *out = &buf_vbo[buf_num_vert];
#if defined(TARGET_PSP)
    out->x = cv->x; // view space: the GE applies only the projection
    out->y = cv->y;
    out->z = cv->z;
#else
    /* Software rasterizer: the GE is gone, so project here. cv->_x.._w are clip
     * space (set in gfx_sp_vertex, interpolated by the clipper). Perspective
     * divide -> NDC, then the viewport maps NDC to screen pixels (y flipped). */
    {
        const float w = cv->_w;
        const float inv = (w > 1e-6f || w < -1e-6f) ? 1.0f / w : 0.0f;
        out->x = (float)rdp.viewport.x + (cv->_x * inv * 0.5f + 0.5f) * (float)rdp.viewport.width;
        out->y = (float)rdp.viewport.y + (0.5f - cv->_y * inv * 0.5f) * (float)rdp.viewport.height;
        /* z/w is -1 (near) .. 1 (far), as the RSP's viewport maps it (vscale = vtrans = G_MAXZ/2); the z-buffer is
         * 0..1. The kit passed z/w straight through, so everything nearer than z/w = 0 (about twice the near plane)
         * clamped to depth 0 and lost its order (SM64 5cd6699, MK64 round-24 review). */
        out->z = cv->_z * inv * 0.5f + 0.5f;
        /* Without near clipping (PM64_NEARCLIP=0, the NoN microcode) a vertex in front of the near plane has
         * z/w < -1, depth < 0. The RDP clamps depth per pixel, so the vertex keeps it and the rasterizer clamps each
         * pixel (ocarina-of-time OOT_NON, the lead's near-plane census): clamping the vertex bent the depth of the whole
         * triangle, in front of the plane too. PM64_ZVTXCLAMP=1: the vertex clamp, as before. */
        static int sZVtx = -1;
        if (sZVtx < 0) { const char *e = getenv("PM64_ZVTXCLAMP"); sZVtx = e && e[0] == '1'; }
        if (!gfx_nearclip && sZVtx && out->z < 0.0f) out->z = 0.0f;
        out->invw = (inv > 0.0f) ? inv : 0.0f; // <=0: rasterizer falls back to affine
    }
#endif
    if (tri_state.use_texture) {
        out->u = cv->u * tri_state.u_scale + tri_state.u_off;
        out->v = cv->v * tri_state.v_scale + tri_state.v_off;
    } else {
        out->u = 0;
        out->v = 0;
    }
    switch (tri_state.color_mode) {
        case 0:
            out->color = cv->color;
            if (tri_state.alpha_255) out->color.a = 255;
            break;
        case 1:
            out->color = tri_state.const_color;
            break;
        default:
            gfx_eval_vertex_color(cc_id, &cv->color, lod, &out->color);
            break;
    }
    if (tri_state.use_fog) {
        // Blender cycle 1 of the fog render modes: colour = lerp(colour, fog
        // colour, shade alpha) -- the RSP fog factor with G_FOG on, otherwise
        // the model's vertex alpha (Moo Moo Farm darkens its dirt that way).
        // Folded into the vertex colour before the texture multiply: exact
        // for dark fog colours, slightly darker than the N64 for bright ones.
        int a = cv->color.a, ia = 255 - a;
#ifdef PM64
        if (pm_census_on()) { /* the cross-port R35: is any fog colour not black? */
            const uint32_t rgb = (uint32_t)rdp.fog_color.r << 16 | (uint32_t)rdp.fog_color.g << 8 | rdp.fog_color.b;
            pm_cs.fog_tris++;
            if (rgb) {
                pm_cs.fog_color++;
                for (int k = 0; k < 4; k++) { if (pm_cs.fog_rgb[k] == rgb) break; if (!pm_cs.fog_rgb[k]) { pm_cs.fog_rgb[k] = rgb; break; } }
            }
        }
#endif
        out->color.r = (uint8_t) ((out->color.r * ia + rdp.fog_color.r * a) / 255);
        out->color.g = (uint8_t) ((out->color.g * ia + rdp.fog_color.g * a) / 255);
        out->color.b = (uint8_t) ((out->color.b * ia + rdp.fog_color.b * a) / 255);
    }
    buf_num_vert++;
    buf_vbo_len += sizeof(psp_fast_t);
}

/* Emit a triangle, subdividing it while its texture coordinates span more
 * repeats than the GE interpolates precisely (MK64 tiles its ground and hill
 * planes hundreds of times across single polygons). */
#define GFX_MAX_UV_REPEATS 16.0f
#ifdef PM64
extern void gfx_soft_tex_size(int tile, int *w, int *h);
static int pm_uvrebase_on(void) { static int s = -1; if (s < 0) { const char *e = getenv("PM64_UVREBASE"); s = !(e && e[0] == '0'); } return s; }
static int pm_uvdrop_on(void) { static int s = -1; if (s < 0) { const char *e = getenv("PM64_UVDROP"); s = !(e && e[0] == '0'); } return s; }
/* The drop for one axis in steps of base (1, or 2 for a doubled mirror) and of the smallest power of two L with
 * L * w0 * 2^k a multiple of p1 (both powers of two, so the larger); none within 2^16 repeats: no drop. */
static float pm_uv_step(float off, float minc, int base, int w0, int p1, int k) {
    if (off == 0.0f || w0 <= 0 || p1 <= 0) return off;
    int l = 1;
    for (; l <= 65536; l <<= 1) {
        const long long moved = (long long) l * w0, need = (long long) p1;
        if (k >= 0 ? ((moved << k) % need) == 0 : (moved % (need << -k)) == 0) break;
    }
    if (l > 65536) return 0.0f;
    const int step = l > base ? l : base;
    return (float) step * port_floorf(minc / (float) step);
}
#endif
static void gfx_emit_triangle(const struct LoadedVertex *a, const struct LoadedVertex *b, const struct LoadedVertex *c, uint32_t cc_id, int lod, int depth) {
    if (tri_state.use_texture && depth < 0) { // subdivision disabled: does not fix distant-texture aliasing (needs mipmaps)
        float umin = a->u, umax = a->u, vmin = a->v, vmax = a->v;
        if (b->u < umin) umin = b->u; if (b->u > umax) umax = b->u;
        if (c->u < umin) umin = c->u; if (c->u > umax) umax = c->u;
        if (b->v < vmin) vmin = b->v; if (b->v > vmax) vmax = b->v;
        if (c->v < vmin) vmin = c->v; if (c->v > vmax) vmax = c->v;
        if ((umax - umin) * tri_state.u_scale > GFX_MAX_UV_REPEATS || (vmax - vmin) * tri_state.v_scale > GFX_MAX_UV_REPEATS) {
            struct LoadedVertex ab, bc, ca;
            gfx_clip_interpolate_vert(&ab, a, b, 0.5f);
            gfx_clip_interpolate_vert(&bc, b, c, 0.5f);
            gfx_clip_interpolate_vert(&ca, c, a, 0.5f);
            gfx_emit_triangle(a, &ab, &ca, cc_id, lod, depth + 1);
            gfx_emit_triangle(&ab, b, &bc, cc_id, lod, depth + 1);
            gfx_emit_triangle(&ca, &bc, c, cc_id, lod, depth + 1);
            gfx_emit_triangle(&ab, &bc, &ca, cc_id, lod, depth + 1);
            return;
        }
    }
    if (buf_vbo_num_tris == MAX_BUFFERED) {
        gfx_flush();
    }
#ifdef PORT_GE_TL
    if (gfx_hud_anchor && !rsp.is_persp) {
        if (buf_vbo_num_tris > 0 && hud_batch_class != hud_load_class) gfx_flush();
        hud_batch_class = hud_load_class;
    }
#endif
#ifdef PORT_GFX_DEBUG
    if (gfx_debug_frame && (a->_w < 2.0f || b->_w < 2.0f || c->_w < 2.0f)) {
        extern uint32_t gfx_prof_neareye;
        gfx_prof_neareye++;
        if (gfx_prof_neareye <= 8)
            port_log("  near-eye tri: w %.2f/%.2f/%.2f  view z %.1f/%.1f/%.1f  cc %08X tex %d\n",
                     a->_w, b->_w, c->_w, a->z, b->z, c->z, (unsigned) cc_id,
                     rendering_state.textures[0] ? (int) rendering_state.textures[0]->texture_id : -1);
    }
#endif
    gfx_emit_vertex(a, cc_id, lod);
    gfx_emit_vertex(b, cc_id, lod);
    gfx_emit_vertex(c, cc_id, lod);
    if (tri_state.use_texture) {
#ifndef PORT_NO_UVDROP
        // Drop the whole texture repeats shared by the three vertices (periodic under REPEAT).
        psp_fast_t *first = &buf_vbo[buf_num_vert - 3];
        float minu = first[0].u, minv = first[0].v;
        int i;
        for (i = 1; i < 3; i++) {
            if (first[i].u < minu) minu = first[i].u;
            if (first[i].v < minv) minv = first[i].v;
        }
        {
            // (a doubled mirrored texture has period 1 in normalised units, like a plain repeat)
            float offu = (rdp.texture_tile.cms & G_TX_CLAMP) ? 0.0f : ((rdp.texture_tile.cms & G_TX_MIRROR) && !(tri_state.mirror & 1)) ? 2.0f * port_floorf(minu * 0.5f) : port_floorf(minu);
            float offv = (rdp.texture_tile.cmt & G_TX_CLAMP) ? 0.0f : ((rdp.texture_tile.cmt & G_TX_MIRROR) && !(tri_state.mirror & 2)) ? 2.0f * port_floorf(minv * 0.5f) : port_floorf(minv);
#ifdef PM64
            if (!pm_uvdrop_on()) offu = offv = 0.0f; /* PM64_UVDROP=0: no drop at all, the exact reference */
            else if (pm_gen_active && pm_t1_linked && (offu != 0.0f || offv != 0.0f) && pm_uvrebase_on()) {
                /* Dropping n repeats of TEXEL0 moves TEXEL1 by n * W0 * 2^k texels: whole repeats of TEXEL1 only when
                 * that is a multiple of its period P1. So drop in steps of the smallest power of two L that makes
                 * it one (ocarina-of-time 2cc4e3b, the water). It moved TEXEL1 by part of a repeat per triangle: seams
                 * and jumps in the two-layer surfaces -- the castle lift's dust (7,222 of 9,554 dropped triangles
                 * over 1,150 frames), the lava (kzn_08 8,372), the beach water (jan_00 4,516). PM64_UVREBASE=0: off. */
                int w0, h0, w1, h1;
                gfx_soft_tex_size(0, &w0, &h0);
                gfx_soft_tex_size(1, &w1, &h1);
                offu = pm_uv_step(offu, minu, (rdp.texture_tile.cms & G_TX_MIRROR) && !(tri_state.mirror & 1) ? 2 : 1, w0, w1, pm_t1_k[0]);
                offv = pm_uv_step(offv, minv, (rdp.texture_tile.cmt & G_TX_MIRROR) && !(tri_state.mirror & 2) ? 2 : 1, h0, h1, pm_t1_k[1]);
            }
#endif
            if (offu != 0.0f || offv != 0.0f) {
                for (i = 0; i < 3; i++) {
                    first[i].u -= offu;
                    first[i].v -= offv;
                }
            }
        }
#endif
    }
    buf_vbo_num_tris++;
}

#ifdef PORT_GE_TL
/* Clip a triangle against the near plane (clip.w >= W_MIN) in OBJECT space, so
 * the GE still transforms the result normally.  clip.w is linear in object
 * space (v->_w holds it), so the crossing point interpolates linearly.  Emits
 * the clipped polygon; the GE never sees a near-zero-w vertex (no blow-up), and
 * spanning road triangles keep their visible part (no gap/flicker behind the
 * kart). */
/* Guard-band NDC limit.  A vertex whose |clip.x/clip.w| or |clip.y/clip.w|
 * exceeds the PSP GE's guard band is WRAPPED (garbage / dropped) on real
 * hardware; PPSSPP silently clamps it, which is why the "road replaced by the
 * backdrop behind the kart" bug only appeared on-device.  So we clip the
 * near-straddling triangle against the near plane AND the four screen-side
 * guard planes -- every emitted vertex then projects on-screen. */
#ifndef GE_GUARD_NDC
#define GE_GUARD_NDC 1.0f
#endif

static inline void nclip_lerp(struct LoadedVertex *o, const struct LoadedVertex *a, const struct LoadedVertex *b, float t) {
    o->x = a->x + t * (b->x - a->x);
    o->y = a->y + t * (b->y - a->y);
    o->z = a->z + t * (b->z - a->z);
    o->_x = a->_x + t * (b->_x - a->_x);
    o->_y = a->_y + t * (b->_y - a->_y);
    o->_z = a->_z + t * (b->_z - a->_z);
    o->_w = a->_w + t * (b->_w - a->_w);
    o->u = a->u + t * (b->u - a->u);
    o->v = a->v + t * (b->v - a->v);
    o->color.r = (uint8_t) (a->color.r + t * (b->color.r - a->color.r));
    o->color.g = (uint8_t) (a->color.g + t * (b->color.g - a->color.g));
    o->color.b = (uint8_t) (a->color.b + t * (b->color.b - a->color.b));
    o->color.a = (uint8_t) (a->color.a + t * (b->color.a - a->color.a));
    o->clip_rej = 0;
}

/* Clip convex polygon `in` (n verts) against the half-space
 * ax*clip.x + ay*clip.y + az*clip.z + aw*clip.w >= 0.  Writes up to n+1 verts to `out`. */
static int nclip_plane(const struct LoadedVertex *in, int n, struct LoadedVertex *out,
                       float ax, float ay, float az, float aw, int cap) {
    int nout = 0, i;
    for (i = 0; i < n; i++) {
        const struct LoadedVertex *cur = &in[i];
        const struct LoadedVertex *nxt = &in[(i + 1) % n];
        float dc = ax * cur->_x + ay * cur->_y + az * cur->_z + aw * cur->_w;
        float dn = ax * nxt->_x + ay * nxt->_y + az * nxt->_z + aw * nxt->_w;
        int cin = dc >= 0.0f, nin = dn >= 0.0f;
        if (cin && nout < cap) out[nout++] = *cur;
        if (cin != nin && nout < cap) {
            float t = dc / (dc - dn);
            nclip_lerp(&out[nout++], cur, nxt, t);
        }
    }
    return nout;
}

static void gfx_ge_tl_near_clip(const struct LoadedVertex *a, const struct LoadedVertex *b, const struct LoadedVertex *c, uint32_t cc_id, int lod) {
    // Reject non-finite geometry up front (a bad matrix would feed garbage).
    if (!(a->_w == a->_w) || !(b->_w == b->_w) || !(c->_w == c->_w)) {
        return;
    }
    struct LoadedVertex bufA[10], bufB[10];
    int n = 3, i;
    bufA[0] = *a; bufA[1] = *b; bufA[2] = *c;

    // Near plane: keep the portion with clip.w >= GE_TL_NEAR.
    {
        int nout = 0;
        for (i = 0; i < n; i++) {
            const struct LoadedVertex *cur = &bufA[i];
            const struct LoadedVertex *nxt = &bufA[(i + 1) % n];
            int cin = cur->_w >= GE_TL_NEAR, nin = nxt->_w >= GE_TL_NEAR;
            if (cin && nout < 10) bufB[nout++] = *cur;
            if (cin != nin && nout < 10) {
                float t = (GE_TL_NEAR - cur->_w) / (nxt->_w - cur->_w);
                nclip_lerp(&bufB[nout++], cur, nxt, t);
            }
        }
        n = nout;
    }
    if (n < 3) return; // fully behind the eye

    // The game's real near and far planes (what the RSP clips against), pulled
    // in by GE_DEPTH_EPS: the GE rejects a whole triangle if any vertex lands
    // outside its depth range, so every emitted vertex must satisfy
    //   clip.z >= -(1-eps)*w   and   clip.z <= (1-eps)*w.
    const float D = 1.0f - GE_DEPTH_EPS;
    n = nclip_plane(bufB, n, bufA, 0.0f, 0.0f,  1.0f, D, 10); if (n < 3) return;
    n = nclip_plane(bufA, n, bufB, 0.0f, 0.0f, -1.0f, D, 10); if (n < 3) return;

    // Four guard-band side planes (all verts now have clip.w >= GE_TL_NEAR > 0):
    //   clip.x <=  G*w,  clip.x >= -G*w,  clip.y <=  G*w,  clip.y >= -G*w
    const float G = GE_GUARD_NDC;
    n = nclip_plane(bufB, n, bufA, -1.0f,  0.0f, 0.0f, G, 10); if (n < 3) return;
    n = nclip_plane(bufA, n, bufB,  1.0f,  0.0f, 0.0f, G, 10); if (n < 3) return;
    n = nclip_plane(bufB, n, bufA,  0.0f, -1.0f, 0.0f, G, 10); if (n < 3) return;
    n = nclip_plane(bufA, n, bufB,  0.0f,  1.0f, 0.0f, G, 10); if (n < 3) return;
    struct LoadedVertex *out = bufB;

    // Back-face cull the clipped polygon (every vertex has w > 0 => exact winding).
    uint32_t cull = rsp.geometry_mode & G_CULL_BOTH;
    if (cull) {
        float w1 = out[0]._w, w2 = out[1]._w, w3 = out[2]._w;
        float cross = (out[0]._x * w2 - out[1]._x * w1) * (out[2]._y * w2 - out[1]._y * w3)
                    - (out[0]._y * w2 - out[1]._y * w1) * (out[2]._x * w2 - out[1]._x * w3);
        if (cull == G_CULL_FRONT && cross <= 0.0f) return;
        if (cull == G_CULL_BACK && cross >= 0.0f) return;
        if ((cull & G_CULL_BOTH) == G_CULL_BOTH) return;
    }
    for (i = 1; i + 1 < n; i++) {
        gfx_emit_triangle(&out[0], &out[i], &out[i + 1], cc_id, lod, 0);
    }
}
#endif

#ifdef PM64
static bool pm64_zimg_pending; /* the depth image as a colour target, below (pm64_zimg_begin) */
static bool pm64_zimg_begin(void);
#endif
static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
#ifdef PM64
    if (pm64_zimg_pending) pm64_zimg_begin(); /* the first triangle into the depth image begins its target */
#endif
#ifdef PORT_EXP_NOTRI
    return;
#endif
#ifdef PORT_PROFILE_DL
    uint32_t _pt0 = port_time_us();
#endif
    struct LoadedVertex *v1 = &rsp.loaded_vertices[vtx1_idx];
    struct LoadedVertex *v2 = &rsp.loaded_vertices[vtx2_idx];
    struct LoadedVertex *v3 = &rsp.loaded_vertices[vtx3_idx];
    struct LoadedVertex *v_arr[3] = {v1, v2, v3};

    if (v1->clip_rej & v2->clip_rej & v3->clip_rej & ~gfx_reject_keep) {
        // The whole triangle lies outside the guard band (the exact side planes reject below,
        // after the phase bookkeeping). GB_OUT is one bit for all four sides, so three vertices
        // sharing it may be on opposite sides of the screen: never a reject. Without near clipping
        // (PM64_NEARCLIP=0) a triangle wholly in front of the near plane still draws.
        return;
    }
#ifdef PM64
    if (gfx_trace_frames > 0) { /* PM64_TRACE=N PM64_TRILOG=1: every triangle with its state (round 16, the fountain hunt) */
        static int sTriLog = -1;
        if (sTriLog < 0) sTriLog = getenv("PM64_TRILOG") != NULL;
        if (sTriLog) {
            float sx[3], sy[3];
            for (int k = 0; k < 3; k++) {
                const struct LoadedVertex *v = v_arr[k];
                const float iw = (v->_w > 1e-6f || v->_w < -1e-6f) ? 1.0f / v->_w : 0.0f;
                sx[k] = (v->_x * iw * 0.5f + 0.5f) * 320.0f; sy[k] = (0.5f - v->_y * iw * 0.5f) * 240.0f;
            }
            port_log("  tri: FC%06X %08X om %08X %08X geo %08X prim %02X%02X%02X%02X env %02X%02X%02X%02X cimg %p | "
                     "(%.0f,%.0f w%.0f a%u) (%.0f,%.0f w%.0f a%u) (%.0f,%.0f w%.0f a%u)\n",
                     (unsigned) rdp.pm_cc_w0, (unsigned) rdp.pm_cc_w1, (unsigned) rdp.other_mode_h, (unsigned) rdp.other_mode_l,
                     (unsigned) rsp.geometry_mode, rdp.prim_color.r, rdp.prim_color.g, rdp.prim_color.b, rdp.prim_color.a,
                     rdp.env_color.r, rdp.env_color.g, rdp.env_color.b, rdp.env_color.a, rdp.color_image_address,
                     sx[0], sy[0], v1->_w, v1->color.a, sx[1], sy[1], v2->_w, v2->color.a, sx[2], sy[2], v3->_w, v3->color.a);
        }
    }
    if (pm_census_on()) {
        if (rdp.color_image_address == rdp.z_buf_address) pm_cs.zimg_tris++;
        if (rsp.is_persp) {
            const uint32_t any = v1->clip_rej | v2->clip_rej | v3->clip_rej;
            pm_cs.tris++;
            if (any & GB_OUT) pm_cs.gb++;
            if ((v1->_z < -v1->_w && v1->_w >= W_MIN) || (v2->_z < -v2->_w && v2->_w >= W_MIN) ||
                (v3->_z < -v3->_w && v3->_w >= W_MIN)) pm_cs.near_cross++;
            if (any & gfx_clip_trigger) {
                pm_cs.clips++;
                if (any & Z_POS) pm_cs.clip_near++;
                if (any & Z_NEG) pm_cs.clip_far++;
                if (any & GB_OUT) pm_cs.clip_gb++;
                if (any & W_LOW) pm_cs.clip_wlow++;
            }
        }
    }
#endif
#if defined(TARGET_HANDHELD)
    /* Perspective -> internal target. Ortho geometry is HUD (native), EXCEPT
     * the sky/ground gradient quads render_skybox draws before any 3D each
     * race frame: two full-screen gouraud quads the track then covers, so
     * fill them at the internal size too (the "previous frame had 3D" test
     * keeps menus, which never draw perspective, on the native target). */
    {
        extern int gfx_soft_last_frame_had_3d;
        extern void gfx_scegu_note_persp(void);
        bool to3d = rsp.is_persp;
        if (rsp.is_persp) {
            if (!gfx_seen_persp) { gfx_seen_persp = true; gfx_scegu_note_persp(); }
        } else if (!gfx_seen_persp && gfx_soft_last_frame_had_3d) {
            to3d = true;
        }
        gfx_set_phase(to3d);
    }
    /* Wholly beyond one side of the view (exact planes, the same x the rasterizer projects): it
     * would rasterize no pixel, since raster_tri clamps to the viewport, so skip the emit, state
     * rebuild and setup. After the phase bookkeeping, so target selection is unchanged. */
    if (v1->clip_rej & v2->clip_rej & v3->clip_rej & XY_OUT) {
        return;
    }
#endif
    /* Software-path far cull (device round 12; the PSP-GE PORT_DRAW_DIST block
     * that used to sit here never compiled for this build and is gone). Skip
     * triangles wholly beyond gfx_draw_dist_w (perspective only; _w grows with
     * distance). OFF by default (1e30): on DK Jungle it only bought ~17% and
     * the user rejected the pop-in; kept as a desktop tuning knob. */
    if (rsp.is_persp && v1->_w > gfx_draw_dist_w && v2->_w > gfx_draw_dist_w && v3->_w > gfx_draw_dist_w) {
        return;
    }


    bool culled_early = false;
    bool is_persp_e = rsp.is_persp;
#ifndef PORT_GE_CULL
    if (is_persp_e && (rsp.geometry_mode & G_CULL_BOTH) != 0 && v1->_w > 0.0f && v2->_w > 0.0f && v3->_w > 0.0f) {
        // All in front of the eye: the winding test is exact before clipping,
        // and rejecting here saves clipping back faces.
        float w1 = v1->_w, w2 = v2->_w, w3 = v3->_w;
        float dx1 = v1->_x * w2 - v2->_x * w1;
        float dy1 = v1->_y * w2 - v2->_y * w1;
        float dx2 = v3->_x * w2 - v2->_x * w3;
        float dy2 = v3->_y * w2 - v2->_y * w3;
        float cross = dx1 * dy2 - dy1 * dx2;
        switch (rsp.geometry_mode & G_CULL_BOTH) {
            case G_CULL_FRONT:
                if (cross <= 0) {
                    return; }
                break;
            case G_CULL_BACK:
                if (cross >= 0) {
                    return; }
                break;
            case G_CULL_BOTH:
                return;
        }
        culled_early = true;
    }
#else
    if ((rsp.geometry_mode & G_CULL_BOTH) == G_CULL_BOTH) return; // GE culls front/back; both = nothing
#endif
    /* Clip only when a vertex is outside the frustum; otherwise pass the three
     * pointers straight through.  The clip scratch is static (single-threaded)
     * so the common path keeps a tiny stack frame. */
    struct LoadedVertex **clipped_vertices = v_arr;
    size_t clipped_vertices_num = 3;
    // clip_to_frustum can return up to 3 + 7 clip planes = 10 vertices, and the
    // retesselate in gfx_clip_single_vert emits 3*(out-2) = up to 24 vertices.
    // The old size of 18 overflowed on near-camera terrain that straddles many
    // frustum planes (DK Jungle intro) -> corrupted adjacent statics, which
    // faulted on the PSP's memory layout but not on the emulator's.  Size for
    // the true worst case (temp_a[12] bounds clip output -> 3*(12-2)=30).
    static struct LoadedVertex _clipped_vertices[32];
    static struct LoadedVertex *ptr_clipped_vertices[32];

#ifdef PORT_PROFILE_DL
    gfx_prof_tri1calls++;
#endif
    // A triangle that reaches from near the camera to beyond the far plane
    // projects to a degenerate stretched shape; clip it (near+far) so only the
    // visible span reaches the GE.  Cheap: true for only a few polygons per
    // frame (road wrapping behind the kart during turns), unlike flagging all
    // distant geometry.
    bool span_clip = false;
#ifndef PORT_GE_CULL
    /* CPU near/far span clip for the CPU-transform path.  Under GE offload the
     * near/far/guard planes are handled by gfx_ge_tl_near_clip below (the GE
     * itself never clips: with GU_CLIP_PLANES it drops whole triangles). */
    {
        // Orthographic draws (the screen-space skybox gradient, HUD) have w==1
        // for every vertex and never straddle the eye or far plane -- never
        // clip them, or the near-w test below deletes the whole skybox.
        bool is_perspective = rsp.is_persp;
        if (is_perspective) {
            // Clip when a vertex is in front of the game's near plane (or behind the eye: both set Z_POS) or beyond
            // its far plane (Z_NEG), as the RSP does. MK64 clipped only for w < 2, or w < 24 and past far: MK64
            // distances, so a Paper Mario wall or billboard between the near plane and 24 units drew unclipped
            // (SM64 5cd6699, MK64 round-24 review).
            /* Round 16: the triggers follow PM64_NEARCLIP and PM64_CLIPEDGES (gfx_clip_trigger, set in gfx_init):
             * by default a vertex beyond the far plane, beyond the guard band, or at or behind the eye. */
            if ((v1->clip_rej | v2->clip_rej | v3->clip_rej) & gfx_clip_trigger) {
                span_clip = true;
            }
        }
    }
#endif
    if (span_clip || ((v1->clip_rej || v2->clip_rej || v3->clip_rej) & CLIP_TEST_FLAGS)) {
#ifdef PORT_PROFILE_DL
        gfx_prof_clipcalls++;
#endif
        gfx_clip_single_vert(_clipped_vertices, &clipped_vertices_num, v_arr);

        if(!clipped_vertices_num){
            /* No idea if this is possible */
            return;
        }
        size_t i;
        for(i = 0;i < clipped_vertices_num;i++){
            ptr_clipped_vertices[i] = &_clipped_vertices[i];
        }
        clipped_vertices = ptr_clipped_vertices;
    }

#ifdef PORT_EXP_NOCACHE
    tri_state.valid = false;
#endif
#ifdef PORT_GE_TL
    // A vertex whose |clip.x| or |clip.y| exceeds GE_GUARD_NDC*clip.w projects
    // outside the GE guard band and WRAPS on real hardware (brief flashes as you
    // drive past near-camera walls/edges) -- route the whole triangle through the
    // guard-band clipper, same as near-plane straddlers.  Perspective only.
    // span_clip triangles (near-camera / far-spanning) are otherwise emitted via
    // the frustum clipper below, whose output can still carry a near-zero-w
    // vertex projecting far outside the GE guard band (measured NDC x up to 155
    // on Toad's Turnpike at speed -> wraps to black on real hardware).  Route
    // them through gfx_ge_tl_near_clip too, which clips the 4 guard planes and
    // guarantees every emitted vertex projects on-screen.
    bool ge_needs_clip = (v1->clip_rej | v2->clip_rej | v3->clip_rej) != 0;
    // A vertex whose |clip.x| or |clip.y| exceeds GE_GUARD_NDC*clip.w projects
    // outside the GE guard band and WRAPS on real hardware -- route the whole
    // triangle through the guard-band clipper.  Perspective only.
    if (!ge_needs_clip && (rsp.is_persp)) {
        const struct LoadedVertex *gv[3] = { v1, v2, v3 };
        int gk;
        for (gk = 0; gk < 3; gk++) {
            float gw = GE_GUARD_NDC * gv[gk]->_w;
            if (gv[gk]->_x > gw || gv[gk]->_x < -gw || gv[gk]->_y > gw || gv[gk]->_y < -gw) {
                ge_needs_clip = true;
                break;
            }
        }
    }

#endif
#if !defined(PORT_GE_CULL) || defined(PORT_GE_TL)
    bool is_persp_cull = rsp.is_persp;
    if (is_persp_cull && (rsp.geometry_mode & G_CULL_BOTH) != 0 && !culled_early
#ifdef PORT_GE_TL
        // Skip the winding cull for triangles straddling the near plane: with a
        // vertex behind the eye (mixed-sign w) the homogeneous winding test is
        // unreliable and would flicker the road behind the kart.  gfx_ge_tl_near_clip
        // handles them instead (also skip guard-band violators -> the clipper).
        && !ge_needs_clip
#endif
        ) {
        // Winding test on the (possibly clipped) polygon: every vertex now has
        // w > 0, so the sign of the homogeneous cross product is exact.  The
        // differences of x/w share the positive denominator w1*w2*w2*w3.
        const struct LoadedVertex *c1 = clipped_vertices[0], *c2 = clipped_vertices[1], *c3 = clipped_vertices[2];
        float w1 = c1->_w, w2 = c2->_w, w3 = c3->_w;
        float dx1 = c1->_x * w2 - c2->_x * w1;
        float dy1 = c1->_y * w2 - c2->_y * w1;
        float dx2 = c3->_x * w2 - c2->_x * w3;
        float dy2 = c3->_y * w2 - c2->_y * w3;
        float cross = dx1 * dy2 - dy1 * dx2;
        if (w1 * w3 < 0.0f) {
            cross = -cross;
        }
        switch (rsp.geometry_mode & G_CULL_BOTH) {
            case G_CULL_FRONT:
                if (cross <= 0) return;
                break;
            case G_CULL_BACK:
                if (cross >= 0) return;
                break;
            case G_CULL_BOTH:
                return;
        }
    }
#endif
#ifdef PORT_PROFILE_DL
    gfx_prof_cullclip += port_time_us() - _pt0;
    _pt0 = port_time_us();
#endif
    if (!tri_state.valid) {
        gfx_tri_rebuild_state(v1);
    }
#ifdef PORT_PROFILE_DL
    gfx_prof_state += port_time_us() - _pt0;
    _pt0 = port_time_us();
#endif
    uint32_t cc_id = tri_state.cc_id;
    bool use_texture = tri_state.use_texture;
    int lod = 0;
    if (tri_state.needs_lod) {
        float distance_frac = (v1->w - 3000.0f) / 3000.0f;
        if (distance_frac < 0.0f) distance_frac = 0.0f;
        if (distance_frac > 1.0f) distance_frac = 1.0f;
        lod = (int) (distance_frac * 255.0f);
    }
    size_t i;
#ifdef PORT_GE_TL
    if (ge_needs_clip) {
        // Triangle reaches at/behind the near plane, or a vertex falls outside
        // the GE guard band: clip it (near + 4 side planes) so the GE never sees
        // a near-zero-w or off-guard vertex -> no blow-up, no wrap-flash, no gap.
        gfx_ge_tl_near_clip(v1, v2, v3, cc_id, lod);
    } else
#endif
    for (i = 0; i + 2 < clipped_vertices_num; i += 3) {
        gfx_emit_triangle(clipped_vertices[i], clipped_vertices[i + 1], clipped_vertices[i + 2], cc_id, lod, 0);
    }
#ifdef PORT_PROFILE_DL
    gfx_prof_emit += port_time_us() - _pt0;
#endif
    if (gfx_debug_frame) {
        float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f, minz = 1e9f, maxz = -1e9f, minw = 1e9f, maxw = -1e9f;
        for (i = 0; i < clipped_vertices_num; i++) {
            const struct LoadedVertex *v = clipped_vertices[i];
            float w = v->_w != 0.0f ? v->_w : 1e-6f;
            float nx = v->_x / w, ny = v->_y / w, nz = v->_z / w;
            if (nx < minx) minx = nx; if (nx > maxx) maxx = nx;
            if (ny < miny) miny = ny; if (ny > maxy) maxy = ny;
            if (nz < minz) minz = nz; if (nz > maxz) maxz = nz;
            if (v->_w < minw) minw = v->_w; if (v->_w > maxw) maxw = v->_w;
        }
        float maxdev = 0.0f;
        for (i = 0; i < clipped_vertices_num; i++) {
            const struct LoadedVertex *v = clipped_vertices[i];
            int c;
            for (c = 0; c < 4; c++) {
                float g = v->x * rsp.P_matrix[0][c] + v->y * rsp.P_matrix[1][c] + v->z * rsp.P_matrix[2][c] + rsp.P_matrix[3][c];
                float d = g - (&v->_x)[c];
                if (d < 0) d = -d;
                if (d > maxdev) maxdev = d;
            }
        }
        if (clipped_vertices_num > 3 || (v1->clip_rej | v2->clip_rej | v3->clip_rej)) {
            float umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f;
            int amin = 255, amax = 0, rmin = 255, rmax = 0;
            for (i = 0; i < clipped_vertices_num; i++) {
                const struct LoadedVertex *v = clipped_vertices[i];
                if (v->u < umin) umin = v->u; if (v->u > umax) umax = v->u;
                if (v->v < vmin) vmin = v->v; if (v->v > vmax) vmax = v->v;
                if (v->color.a < amin) amin = v->color.a; if (v->color.a > amax) amax = v->color.a;
                if (v->color.r < rmin) rmin = v->color.r; if (v->color.r > rmax) rmax = v->color.r;
            }
            port_log("  clipped uv out [%.0f,%.0f]x[%.0f,%.0f] src uv (%.0f,%.0f)/(%.0f,%.0f)/(%.0f,%.0f) col r[%d,%d] a[%d,%d] src r %d/%d/%d a %d/%d/%d\n",
                     umin, umax, vmin, vmax, v1->u, v1->v, v2->u, v2->v, v3->u, v3->v, rmin, rmax, amin, amax,
                     v1->color.r, v2->color.r, v3->color.r, v1->color.a, v2->color.a, v3->color.a);
        }
        port_log("  bbox n=%d x[%.2f,%.2f] y[%.2f,%.2f] z[%.3f,%.3f] w[%.1f,%.1f] cc %08X tex %d src w %.1f/%.1f/%.1f rej %02X/%02X/%02X view z %.1f/%.1f/%.1f ge-dev %.3f\n", (int) clipped_vertices_num, minx, maxx, miny, maxy, minz, maxz, minw, maxw, (unsigned) cc_id, rendering_state.textures[0] ? (int) rendering_state.textures[0]->texture_id : -1, v1->_w, v2->_w, v3->_w, v1->clip_rej, v2->clip_rej, v3->clip_rej, v1->z, v2->z, v3->z, maxdev);
    }
    // The texture-repeat drop (large UV magnitudes lose GE precision) is done
    // per emitted triangle in gfx_emit_triangle.  It used to be repeated here
    // over buf_vbo[buf_num_vert - clipped_vertices_num], which is wrong for
    // the near-clip path: that path emits 0..8 triangles (and may flush in
    // between), so when it emitted nothing right after a flush the pointer
    // landed 72 bytes BEFORE buf_vbo and the float "-=" writes corrupted
    // buf_vbo_num_tris / buf_num_vert / buf_vbo_len / tri_state (crash in
    // gfx_sp_tri1 reading buf_vbo[-Inf]).
    (void) use_texture;
}

/* This will be going away possibly, it all depends on how we end up treating hw sprites */
static void gfx_sp_tri1_2d(uint8_t vtx1_idx, uint8_t vtx2_idx, UNUSED uint8_t vtx3_idx) {
    struct VertexColor *v1 = &rsp.loaded_vertices_2D[vtx1_idx];
    struct VertexColor *v2 = &rsp.loaded_vertices_2D[vtx2_idx];
    struct VertexColor *v_arr[2] = {v1, v2};
#ifdef PM64
    pm_primz_sync(true);
#endif

    bool depth_test = (rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER;
    if (depth_test != rendering_state.depth_test) {
        gfx_flush();
        gfx_rapi->set_depth_test(depth_test);
        rendering_state.depth_test = depth_test;
    }
    
    bool z_upd = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    if (z_upd != rendering_state.depth_mask) {
        gfx_flush();
        gfx_rapi->set_depth_mask(z_upd);
        rendering_state.depth_mask = z_upd;
    }
    
    bool zmode_decal = (rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (zmode_decal != rendering_state.decal_mode) {
        gfx_flush();
        gfx_rapi->set_zmode_decal(zmode_decal);
        rendering_state.decal_mode = zmode_decal;
    }
    
    if (rdp.viewport_or_scissor_changed) {
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush();
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush();
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }
    
#ifdef PM64
    bool pmg, pmt0, pmt1;
    uint32_t cc_id = pm_draw_combine(&pmg, &pmt0, &pmt1);
#else
    uint32_t cc_id = rdp.combine_mode;
#endif
    
    // Whether the framebuffer blend uses the pixel alpha: the last blender
    // cycle's "b" mux is 1-A (G_BL_1MA).  In 2-cycle mode that is cycle 2
    // (bits 16-17); in 1-cycle mode bits 18-19.  sm64-port only tested bit 18
    // (G_BL_A_MEM), which misreads G_RM_OPA_SURF (B mux = G_BL_1, binary 10:
    // blender = IN*0 + IN*1, alpha ignored) as alpha-blended -- SM64 never
    // uses the non-AA OPA_SURF so it never showed, but MK64's menu setup DLs
    // (D_02007708, character-select cells) do: their art keeps black texels
    // with the RGBA16 alpha bit clear and the RDP still draws them opaque.
    // MK64's fogged render modes (FOG_SHADE_A + an opaque cycle 2) blended
    // with the fog amount as opacity before -- Moo Moo Farm's terrain let the
    // ground gradient show through as green patches (issue #1).
    bool two_cycle = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE;
    bool use_alpha = two_cycle ? ((rdp.other_mode_l >> 16) & 3) == G_BL_1MA : ((rdp.other_mode_l >> 18) & 3) == G_BL_1MA;
#ifdef PM64
    /* The blender only runs with FORCE_BL (or on coverage, which we do not model): Paper
     * Mario's background strips are TEXEL0 / SHADE-alpha texrects in render mode 0, where
     * alpha is 0 and the RDP writes the texel as is. Reading the B mux alone blended them
     * away (a black sky). */
    if (!(rdp.other_mode_l & FORCE_BL)) use_alpha = false;
#endif
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    bool use_noise = (rdp.other_mode_l & G_AC_DITHER) == G_AC_DITHER;
    
    if (texture_edge) {
        use_alpha = true;
    }
    
    if (use_alpha) cc_id |= SHADER_OPT_ALPHA;
    if (use_fog) cc_id |= SHADER_OPT_FOG;
    if (texture_edge) cc_id |= SHADER_OPT_TEXTURE_EDGE;
    if (use_noise) cc_id |= SHADER_OPT_NOISE;
    
    if (!use_alpha) {
        cc_id &= ~0xfff000;
    }
    
    if (gfx_trace_frames > 0) port_log("  2d: cc %08X\n", (unsigned) cc_id);
    struct ColorCombiner *comb = gfx_lookup_or_create_color_combiner(cc_id);
    struct ShaderProgram *prg = comb->prg;
    if (gfx_trace_frames > 0) port_log("  2d: prg %p\n", prg);
#ifdef PM64
    /* A 2D rect sets its own texture-add state instead of keeping the last 3D draw's (the cross-port B5, MK64 c400d1b:
     * a tinted kart drawn last washed out MK64's HUD). Paper Mario's tints, (1 - X) x TEXEL0 + Y, go to the generic
     * combiner, so a rect never adds; PM64_TEXADD=0 keeps round 15's inherited state (A/B). */
    if (pm_texadd_on() && rendering_state.tex_add) {
        extern void gfx_scegu_set_texfunc_add(bool on);
        if (pm_census_on()) pm_cs.rects_texadd++;
        gfx_flush();
        gfx_scegu_set_texfunc_add(false);
        rendering_state.tex_add = false;
    }
#endif
    if (prg != rendering_state.shader_program) {
        gfx_flush();
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(prg);
        rendering_state.shader_program = prg;
    }
    if (use_alpha != rendering_state.alpha_blend) {
        gfx_flush();
        gfx_rapi->set_use_alpha(use_alpha);
        rendering_state.alpha_blend = use_alpha;
    }
    uint8_t num_inputs;
    bool used_textures[2];
    gfx_rapi->shader_get_info(prg, &num_inputs, used_textures);
    if (gfx_trace_frames > 0) port_log("  2d: inputs %d tex %d %d\n", num_inputs, used_textures[0], used_textures[1]);
    
    for (int pass = 0; pass < 2; pass++) { /* a reset under tile 1's import: tile 0 again (gfx_cache_epoch) */
    const uint32_t epoch = gfx_cache_epoch;
    gfx_tex_stress_hold = pass;
    for (int i = 0; i < 2; i++) {
        if (used_textures[i]) {
            if (rdp.textures_changed[i]) {
                gfx_flush();
                import_texture(i);
                rdp.textures_changed[i] = false;
            }
            bool linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
            if (rendering_state.textures[i] == NULL) {
                // Texture unit flagged as used but never successfully imported
                // (skipped by the guards above): don't dereference a null node.
            } else if (linear_filter != rendering_state.textures[i]->linear_filter || rdp.texture_tile.cms != rendering_state.textures[i]->cms || rdp.texture_tile.cmt != rendering_state.textures[i]->cmt) {
                gfx_flush();
                gfx_rapi->set_sampler_parameters(i, linear_filter, rdp.texture_tile.cms, rdp.texture_tile.cmt);
                rendering_state.textures[i]->linear_filter = linear_filter;
                rendering_state.textures[i]->cms = rdp.texture_tile.cms;
                rendering_state.textures[i]->cmt = rdp.texture_tile.cmt;
            }
        }
    }
    gfx_tex_stress_hold = 0;
    if (epoch == gfx_cache_epoch || !gfx_epochfix_on()) break;
    }
    // The GE has one texture unit: whatever was bound last (possibly tile 1's
    // mip level, or a texture just uploaded) must give way to tile 0's texture.
    if (used_textures[0] && rendering_state.textures[0] != NULL) {
        gfx_rapi->select_texture(0, rendering_state.textures[0]->texture_id);
    }
    
#ifdef PM64
    if (pmg) {
        pm_gen_setup(two_cycle, pmt0, pmt1);
        pm_gen_active = true;
        used_textures[0] = pmt0 || pmt1;
    } else {
        pm_gen_off();
    }
#endif
    bool use_texture = used_textures[0] || used_textures[1];
    //uint32_t tex_width = (rdp.texture_tile.lrs - rdp.texture_tile.uls + 4) / 4;
    //uint32_t tex_height = (rdp.texture_tile.lrt - rdp.texture_tile.ult + 4) / 4;

    VertexColor tri_buf[2] = {{0}};
    int tri_num_vert = 0;
#ifdef PM64
    /* A wrapping tile (no clamp bit, mask m) reads coordinate & (2^m - 1), so a texrect may start whole periods
     * before its tile's origin: Peach's letter draws each 64x32 piece from S = T = 0 on tiles whose origins are
     * 64/128 and 32/64/96 texels in. The samplers wrap by the imported size instead, which is smaller on the edge
     * pieces (22 columns, 9 rows): the right strip read from column 2 (4 with signed u) and the bottom strip from
     * row 1 (3). Shift both corners by the same whole number of periods (two when mirrored) so the upper-left one
     * lands in [0, period) (round 9). */
    int pm_su = 0, pm_sv = 0;
#endif
    
    for (int i = 0; i < 2; i++) {
        tri_buf[tri_num_vert].x = v_arr[i]->x;
        tri_buf[tri_num_vert].y = v_arr[i]->y;
        tri_buf[tri_num_vert].z = 0;
        
        if (use_texture) {
#ifdef PM64
            /* the render tile's shift scales S/T before its corner is subtracted, as for triangles (R29): the dark
             * rooms stretch their light stencil with shift 15 (x2), centred on Mario */
            short u = (short) (((float) v_arr[i]->u * gfx_tile_shift_scale(pm_tiles[pm_prim_tile].shifts) -
                                rdp.texture_tile.uls * 8) / 32);
            short v = (short) (((float) v_arr[i]->v * gfx_tile_shift_scale(pm_tiles[pm_prim_tile].shiftt) -
                                rdp.texture_tile.ult * 8) / 32);
#else
            short u = (v_arr[i]->u - rdp.texture_tile.uls * 8)/ 32;
            short v = (v_arr[i]->v - rdp.texture_tile.ult * 8) / 32;
#endif
#ifdef PM64
            if (i == 0) {
                if (!(rdp.texture_tile.cms & G_TX_CLAMP) && rdp.texture_tile.masks) {
                    const int p = (1 << rdp.texture_tile.masks) << ((rdp.texture_tile.cms & G_TX_MIRROR) ? 1 : 0);
                    pm_su = (u >= 0 ? u / p : -((p - 1 - u) / p)) * p;
                }
                if (!(rdp.texture_tile.cmt & G_TX_CLAMP) && rdp.texture_tile.maskt) {
                    const int p = (1 << rdp.texture_tile.maskt) << ((rdp.texture_tile.cmt & G_TX_MIRROR) ? 1 : 0);
                    pm_sv = (v >= 0 ? v / p : -((p - 1 - v) / p)) * p;
                }
            }
            u -= pm_su;
            v -= pm_sv;
#endif
            /*
            if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
                // Linear filter adds 0.5f to the coordinates
                u += 0.5f;
                v += 0.5f;
            }
            */
            tri_buf[tri_num_vert].u = u;
            tri_buf[tri_num_vert].v = v;
        } else {
            tri_buf[tri_num_vert].u = 0;
            tri_buf[tri_num_vert].v = 0;
        }
        
        /*
        //@Note no fog currently
        if (use_fog) {
            tri_buf[buf_vbo_len++] = rdp.fog_color.r / 255.0f;
            tri_buf[buf_vbo_len++] = rdp.fog_color.g / 255.0f;
            tri_buf[buf_vbo_len++] = rdp.fog_color.b / 255.0f;
            tri_buf[buf_vbo_len++] = v_arr[i]->color.a / 255.0f; // fog factor (not alpha)
        }
        */
        gfx_eval_vertex_color(cc_id, &v_arr[i]->color, 0, &tri_buf[tri_num_vert].color);
        tri_num_vert++;
    }
    if (gfx_trace_frames > 0) port_log("  2d: draw (%d,%d)-(%d,%d)\n", tri_buf[0].x, tri_buf[0].y, tri_buf[1].x, tri_buf[1].y);
    gfx_scegu_draw_triangles_2d((float*)&tri_buf[0],0,1);
    if (gfx_trace_frames > 0) port_log("  2d: drawn\n");
}

static void gfx_sp_geometry_mode(uint32_t clear, uint32_t set) {
    uint32_t old = rsp.geometry_mode;
    rsp.geometry_mode &= ~clear;
    rsp.geometry_mode |= set;
    if ((old ^ rsp.geometry_mode) & (G_ZBUFFER | G_CULL_BOTH)) {
        tri_state.valid = false;
    }
}

/* Raw (game-space) viewport/scissor, kept so they can be re-derived when the
 * output dimensions change mid-frame (3D-internal / 2D-native phases). */
static Vp_t gfx_raw_vp;
static bool gfx_raw_vp_valid;
static uint32_t gfx_raw_sc[4];
static bool gfx_raw_sc_valid;

static void gfx_calc_and_set_viewport(const Vp_t *viewport) {
    // 2 bits fraction
    float width = 2.0f * viewport->vscale[0] / 4.0f;
    float height = 2.0f * viewport->vscale[1] / 4.0f;
    float x = (viewport->vtrans[0] / 4.0f) - width / 2.0f;
#if defined(TARGET_HANDHELD)
    /* Our software rasterizer is top-down (row 0 = top) and gfx_emit_vertex
     * already flips NDC y inside the viewport rect, so viewport.y must be the
     * TOP edge in top-down coordinates. The OpenGL-style bottom-up flip below
     * is symmetric for a full-screen viewport (which is why 1P racing was
     * fine) but swaps the top and bottom halves for quadrant viewports: the
     * results / driver's-points scenes landed under the text instead of on
     * the opposite diagonal (device round 5), and split-screen would too. */
    float y = (viewport->vtrans[1] / 4.0f) - height / 2.0f;
#else
    float y = SCREEN_HEIGHT - ((viewport->vtrans[1] / 4.0f) + height / 2.0f);
#endif

    width *= RATIO_X;
    height *= RATIO_Y;
    x *= RATIO_X;
    y *= RATIO_Y;

    rdp.viewport.x = x;
    rdp.viewport.y = y;
    rdp.viewport.width = width;
    rdp.viewport.height = height;

    rdp.viewport_or_scissor_changed = true;
    tri_state.valid = false;
    if (viewport != &gfx_raw_vp) { gfx_raw_vp = *viewport; gfx_raw_vp_valid = true; }
}

static void gfx_sp_movemem(uint8_t index, uint8_t offset, const void* data) {
    switch (index) {
        case G_MV_VIEWPORT:
            gfx_calc_and_set_viewport((const Vp_t *) data);
            break;
#ifdef F3DEX_GBI_2
        case G_MV_LIGHT: {
            int lightidx = offset / 24 - 2;
            if (lightidx >= 0 && lightidx <= MAX_LIGHTS) { // skip lookat
                // NOTE: reads out of bounds if it is an ambient light
                memcpy(rsp.current_lights + lightidx, data, sizeof(Light_t));
            }
#ifdef PM64
            else if (lightidx == -2 || lightidx == -1) {
                /* gSPLookAt: G_MVO_LOOKATX (offset 0) and G_MVO_LOOKATY (24), the texgen axes */
                memcpy(&rsp.current_lookat[lightidx == -2 ? 1 : 0], data, sizeof(Light_t));
                rsp.lights_changed = true;
            }
#endif
            break;
        }
#else
        case G_MV_L0:
        case G_MV_L1:
        case G_MV_L2:
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(rsp.current_lights + (index - G_MV_L0) / 2, data, sizeof(Light_t));
            break;
#endif
    }
}

#ifdef PM64
static uint32_t rsp_segments[16]; /* gSPSegment: replay keeps the physical address, the native game a host pointer */
#endif
static void gfx_sp_moveword(uint8_t index, uint16_t offset, uint32_t data) {
    _UNUSED(offset);

    switch (index) {
#ifdef PM64
        case G_MW_SEGMENT:
#ifdef PM64_REPLAY
            rsp_segments[(offset >> 2) & 0xF] = data & 0x00FFFFFF;
#else
            rsp_segments[(offset >> 2) & 0xF] = data; /* the native game: a host pointer (osVirtualToPhysical is the identity) */
#endif
            break;
#endif
        case G_MW_NUMLIGHT:
#ifdef F3DEX_GBI_2
            rsp.current_num_lights = data / 24 + 1; // add ambient light
#else
            // Ambient light is included
            // The 31th bit is a flag that lights should be recalculated
            rsp.current_num_lights = (data - 0x80000000U) / 32;
#endif
            rsp.lights_changed = 1;
            break;
        case G_MW_FOG:
            rsp.fog_mul = (int16_t)(data >> 16);
            rsp.fog_offset = (int16_t)data;
            break;
        case G_MW_CLIP: /* gSPClipRatio: the guard band for GB_OUT and the clipper (round 16, R33). It writes RNX, RNY
                         * = r and RPX, RPY = -r (gbi.h: FR_NEG_FRUSTRATIO_2 is 2, FR_POS_FRUSTRATIO_2 0xfffe); RNX is
                         * taken for both axes, as the macro sets them all from one ratio. */
            if (offset == G_MWO_CLIP_RNX) {
                const int r = (int16_t)(data & 0xFFFF);
                if (r >= 1 && r <= 16) gfx_clip_ratio = (float)r;
#ifdef PM64
                if (pm_census_on()) {
                    pm_cs.mwclip++;
                    if ((float)r < pm_cs.ratio_lo) pm_cs.ratio_lo = (float)r;
                    if ((float)r > pm_cs.ratio_hi) pm_cs.ratio_hi = (float)r;
                }
#endif
            }
            break;
    }
}

#ifdef PM64
/* gSPModifyVertex (F3DEX2 G_MODIFYVTX, gbi.h): one word of a loaded vertex changed in place, after its transform, as
 * the RSP writes its vertex buffer. Paper Mario's one use is G_MWO_POINT_XYSCREEN: Sushie's Tidal Wave
 * (effects/underwater.c) moves the corners of a 16-pixel screen grid every frame to ripple the frame copied under it.
 * Its value is the screen position, x and y in S13.2 in the high and low halves; the RSP keeps the vertex's depth, w
 * and clip codes as transformed, so here only the clip-space x and y change, to what lands on that position through
 * gfx_emit_vertex's divide and the viewport in force. RGBA (the shade, r g b a from the high byte) and ST (S10.5,
 * already past gSPTexture's scale, as the vertex buffer holds them) as gbi.h gives them; the game never sends
 * ZSCREEN, which is logged once and skipped. PM64_MODVTX=0: the command ignored, as before (A/B). */
static void gfx_sp_modify_vertex(uint32_t vtx, uint32_t where, uint32_t val) {
    static int on SAVESTATE_EXCLUDE = -1;
    if (on < 0) { const char *e = getenv("PM64_MODVTX"); on = !(e && e[0] == '0'); }
    if (!on || vtx >= MAX_VERTICES) return;
    struct LoadedVertex *d = &rsp.loaded_vertices[vtx];
    switch (where) {
        case G_MWO_POINT_RGBA:
            d->color.r = (uint8_t) (val >> 24); d->color.g = (uint8_t) (val >> 16);
            d->color.b = (uint8_t) (val >> 8); d->color.a = (uint8_t) val;
            break;
        case G_MWO_POINT_ST:
            d->u = (float) (int16_t) (val >> 16);
            d->v = (float) (int16_t) val;
            break;
        case G_MWO_POINT_XYSCREEN: {
            const float vx = (float) rdp.viewport.x, vy = (float) rdp.viewport.y;
            const float vw = (float) rdp.viewport.width, vh = (float) rdp.viewport.height;
            if (vw == 0.0f || vh == 0.0f) break;
            const float sx = (float) (int16_t) (val >> 16) * 0.25f * RATIO_X, sy = (float) (int16_t) val * 0.25f * RATIO_Y;
            d->_x = ((sx - vx) / vw * 2.0f - 1.0f) * d->_w;
            d->_y = (1.0f - (sy - vy) / vh * 2.0f) * d->_w;
            break;
        }
        default: {
            static int logged SAVESTATE_EXCLUDE;
            if (!logged) { logged = 1; port_log("gfx: G_MODIFYVTX offset 0x%02X skipped (only RGBA, ST, XYSCREEN)\n", (unsigned) where); }
            break;
        }
    }
}
#endif

static void gfx_sp_texture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    _UNUSED(level);
    _UNUSED(tile);
    _UNUSED(on);

    rsp.texture_scaling_factor.s = sc;
    rsp.texture_scaling_factor.t = tc;
#ifdef PM64
    if ((tile & 7) != pm_prim_tile) pm_select_tile(tile & 7);
#endif
}

static void gfx_dp_set_scissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    _UNUSED(mode);
    // A scissor to (319, 239) drops the last column and row, which the CRT's overscan hid (DKR's game clips to it every
    // frame). Ending one pixel short keeps them; a scissor larger than the screen stays as it is (the cross-port lead,
    // 24 Sep; as MK64 99921c7 for fills).
    if (lrx >= (SCREEN_WIDTH - 1) << 2 && lrx < SCREEN_WIDTH << 2) lrx = SCREEN_WIDTH << 2;
    if (lry >= (SCREEN_HEIGHT - 1) << 2 && lry < SCREEN_HEIGHT << 2) lry = SCREEN_HEIGHT << 2;

    float x = ulx / 4.0f * RATIO_X;
#if defined(TARGET_HANDHELD)
    float y = uly / 4.0f * RATIO_Y;                 /* top-down rasterizer: see viewport note */
#else
    float y = (SCREEN_HEIGHT - lry / 4.0f) * RATIO_Y;
#endif
    float width = ((int) lrx - (int) ulx) / 4.0f * RATIO_X;
    float height = ((int) lry - (int) uly) / 4.0f * RATIO_Y;
    /* A corner past the other (lrx < ulx or lry < uly) leaves the RDP nothing to draw. The unsigned
     * difference made such a scissor about a billion rows tall, clipping from its top edge to the bottom
     * of the screen (f-zero-x cf242ea: its pre-race banners shrink their scissors through zero). */
    if (width < 0.0f) width = 0.0f;
    if (height < 0.0f) height = 0.0f;
    /* The scissor's edges, the cross-port R23 in its final form (round 16; GoldenEye's GE_SCISSOR=2). PM64_SCISSOR:
     *   2 (default): an edge within one N64 pixel of a screen edge goes to it, on all four sides (the band above
     *     did it for the right and bottom); the others by pixel centres, as the triangles and texrects count
     *     them: the first column is ceil(x0 - 0.5), the end ceil(x1 - 0.5);
     *   0: round 15's, the float edges as they are;
     *   1: whole target pixels with the right and bottom edges rounded outward (goldeneye c68da63; withdrawn,
     *     wrong for edges inside the screen: F-Zero X's 4P divider). PM64_SCISSOR_OUT=1 also selects it. */
    static int sScMode = -1;
    if (sScMode < 0) {
        const char *e = getenv("PM64_SCISSOR");
        sScMode = e && e[0] >= '0' && e[0] <= '2' ? e[0] - '0' : 2;
        if ((e = getenv("PM64_SCISSOR_OUT")) && e[0] == '1') sScMode = 1;
    }
#ifdef PM64
    const float x_r15 = x, y_r15 = y, w_r15 = width, h_r15 = height; /* the census compares against round 15 */
#endif
    if (sScMode == 1 && width > 0.0f && height > 0.0f) {
        const float x1 = x + width, y1 = y + height;
        x = (float) port_ifloorf(x);
        y = (float) port_ifloorf(y);
        width = (float) port_iceilf(x1 - 0.001f) - x;
        height = (float) port_iceilf(y1 - 0.001f) - y;
    } else if (sScMode == 2) {
        const float tw = (float) gfx_current_dimensions.width, th = (float) gfx_current_dimensions.height;
        float x0 = x, y0 = y, x1 = x + width, y1 = y + height;
        x0 = ulx <= 4 ? 0.0f : (float) port_iceilf(x0 - 0.5f);
        y0 = uly <= 4 ? 0.0f : (float) port_iceilf(y0 - 0.5f);
        x1 = lrx >= (uint32_t) (SCREEN_WIDTH - 1) << 2 ? tw : (float) port_iceilf(x1 - 0.5f);
        y1 = lry >= (uint32_t) (SCREEN_HEIGHT - 1) << 2 ? th : (float) port_iceilf(y1 - 0.5f);
        x = x0; y = y0;
        width = x1 > x0 ? x1 - x0 : 0.0f;
        height = y1 > y0 ? y1 - y0 : 0.0f;
    }
#ifdef PM64
    if (pm_census_on()) {
        pm_cs.scissors++;
        if (x != x_r15 || y != y_r15 || width != w_r15 || height != h_r15) pm_cs.scissors_r23++;
    }
#endif

    rdp.scissor.x = x;
    rdp.scissor.y = y;
    rdp.scissor.width = width;
    rdp.scissor.height = height;

    rdp.viewport_or_scissor_changed = true;
    tri_state.valid = false;
    gfx_raw_sc[0] = ulx; gfx_raw_sc[1] = uly; gfx_raw_sc[2] = lrx; gfx_raw_sc[3] = lry;
    gfx_raw_sc_valid = true;
}

/* 3D-internal / 2D-native phases (device round 5: "only downscale the 3D").
 * The soft backend keeps two targets; perspective triangles go to the
 * ladder-scaled internal one and everything 2D (texrects, ortho HUD quads,
 * menus) to the full-size native one, composited on each switch. Switching
 * changes gfx_current_dimensions, so the viewport and scissor are re-derived
 * from their raw N64 values for the new target and the cached GE state is
 * invalidated so the backend is re-sent both. */
#if defined(TARGET_HANDHELD)
extern void gfx_scegu_set_phase(int is3d);
extern void gfx_soft_get_internal(int *w, int *h);
static void gfx_set_phase(bool is3d) {
    if (is3d == gfx_phase3d) return;
    gfx_flush();
    gfx_scegu_set_phase(is3d ? 1 : 0);
    gfx_phase3d = is3d;
    if (gfx_raw_vp_valid) gfx_calc_and_set_viewport(&gfx_raw_vp);
    if (gfx_raw_sc_valid) gfx_dp_set_scissor(0, gfx_raw_sc[0], gfx_raw_sc[1], gfx_raw_sc[2], gfx_raw_sc[3]);
    memset(&rendering_state.viewport, 0, sizeof(rendering_state.viewport));
    memset(&rendering_state.scissor, 0, sizeof(rendering_state.scissor));
    rdp.viewport_or_scissor_changed = true;
    tri_state.valid = false;
}
#else
static void gfx_set_phase(bool is3d) { (void)is3d; }
#endif

static void gfx_dp_set_texture_image(uint32_t format, uint32_t size, uint32_t width, const void* addr) {
    _UNUSED(format);

    rdp.texture_to_load.addr = addr;
    rdp.texture_to_load.siz = size;
    rdp.texture_to_load.width = width + 1;
    {
        static int n;
        if (gfx_debug_frame) port_log("  settimg fmt %u siz %u width %u addr %p\n", (unsigned) format, (unsigned) size, (unsigned) width + 1, addr);
    }
}

#ifdef PM64
/* The framebuffer being drawn, loaded as a texture: Sushie's Tidal Wave (effects/underwater.c) copies the frame into
 * the depth image in 6-row strips and draws it back through a rippling grid; the crystal ball (mac_01) and flo_10's
 * water read it the same way. On the N64 the RDP draws into that memory, so a load reads what is drawn so far. This
 * renderer draws into its own buffer, so the memory held an older frame or nothing (the ripple drew nothing). A load
 * that reads any of it first writes the rows it reads there from the frame so far (gfx_soft_frame_rows: RGBA16 in
 * the N64's byte order, as nusys's framebuffers are), after drawing what is queued; the import sees the change through
 * pm_frame_gen. Loads from anywhere else are untouched. PM64_FRAMETEX=0: the memory as it was (A/B). */
static void pm_frame_texture_sync(const uint8_t *lo, const uint8_t *hi) {
    static int on SAVESTATE_EXCLUDE = -1;
    if (pm_cfb == NULL || lo == NULL || hi <= pm_cfb || lo >= pm_cfb + PM_CFB_BYTES) return;
    if (on < 0) { const char *e = getenv("PM64_FRAMETEX"); on = !(e && e[0] == '0'); }
    if (!on) return;
    if (lo < pm_cfb) lo = pm_cfb;
    if (hi > pm_cfb + PM_CFB_BYTES) hi = pm_cfb + PM_CFB_BYTES;
    extern int gfx_soft_frame_rows(uint8_t *fb, int y0, int y1);
    gfx_flush();
    if (gfx_soft_frame_rows((uint8_t *) pm_cfb, (int) ((lo - pm_cfb) / 640), (int) ((hi - 1 - pm_cfb) / 640))) pm_frame_gen++;
}
#endif

/* Byte offset of texel (s, t) inside the texture image set by G_SETTIMG. */
static uint32_t gfx_texture_image_offset(uint32_t s, uint32_t t) {
    uint32_t w = rdp.texture_to_load.width;
    switch (rdp.texture_to_load.siz) {
        case G_IM_SIZ_4b:
            return (t * w + s) / 2;
        case G_IM_SIZ_8b:
            return t * w + s;
        case G_IM_SIZ_16b:
            return (t * w + s) * 2;
        default:
            return (t * w + s) * 4;
    }
}

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, UNUSED uint32_t palette, uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts) {
    if (tile == G_TX_RENDERTILE) {
        tri_state.valid = false;
    }
#ifndef PM64
    _UNUSED(maskt);
    _UNUSED(masks);
#endif
#ifndef PM64
    _UNUSED(shiftt);
    _UNUSED(shifts);
#endif

#ifdef PM64
    /* A zero mask does not wrap on the RDP: it clamps (ocarina-of-time 8674501). Paper Mario's
     * text glyphs are G_TX_WRAP + G_TX_NOMASK; repeating them bled the opposite edge in. */
    if (masks == 0) cms |= G_TX_CLAMP;
    if (maskt == 0) cmt |= G_TX_CLAMP;
    pm_tiles[tile & 7].fmt = fmt; pm_tiles[tile & 7].siz = siz; pm_tiles[tile & 7].cms = cms; pm_tiles[tile & 7].cmt = cmt;
    pm_tiles[tile & 7].palette = palette; pm_tiles[tile & 7].tmem = tmem; pm_tiles[tile & 7].line = line;
    pm_tiles[tile & 7].masks = (uint8_t) masks; pm_tiles[tile & 7].maskt = (uint8_t) maskt;
    pm_tiles[tile & 7].shifts = (uint8_t) shifts; pm_tiles[tile & 7].shiftt = (uint8_t) shiftt;
    if (tile == G_TX_RENDERTILE) rdp.texture_tile.palette = palette;
#endif
    if (tile == G_TX_RENDERTILE) {
#ifndef PM64
        SUPPORT_CHECK(palette == 0); // palette should set upper 4 bits of color index in 4b mode
#endif
        rdp.texture_tile.fmt = fmt;
        rdp.texture_tile.siz = siz;
#ifdef PM64
        rdp.texture_tile.cms_raw = cms;
        rdp.texture_tile.cmt_raw = cmt;
        rdp.texture_tile.masks = (uint8_t) masks;
        rdp.texture_tile.maskt = (uint8_t) maskt;
        pm_render_tile_cm();
#else
        rdp.texture_tile.cms = cms;
        rdp.texture_tile.cmt = cmt;
#endif
        // MK64's packed course display lists keep two textures in TMEM (one
        // per half) and point the render tile at either: tile config G is a
        // 32x32 tile at tmem 256.  Draw from the block loaded into that half,
        // not always from the first (Moo Moo Farm's dirt road showed the grass
        // texture on those polygons, issue #1).
        rdp.texture_tile.tmem_slot = (tmem / 256) & 1;
#ifdef PM64
        rdp.texture_tile.tmem = (uint16_t) tmem;
#endif
        rdp.texture_tile.line_size_bytes = line * 8;
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    
    // Every tile descriptor remembers its TMEM half.  MK64's packed course
    // display lists load through tile 6 as well as tile 7 (plain dirt through
    // tile 7 into TMEM 0, the dirt-with-patches variant through tile 6 into
    // TMEM 256, then the render tile picks either half); recording the half
    // only for G_TX_LOADTILE sent the tile-6 load into slot 0 over the other
    // texture and left slot 1 stale -- Moo Moo Farm's road showed the grass
    // texture in patches (issue #1).
    rdp.tile_tmem[tile & 7] = (uint16_t) tmem;
    if (tile == G_TX_LOADTILE) {
        rdp.texture_to_load.tile_number = tmem / 256;
    }
}

static void gfx_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
#ifdef PM64
    pm_tiles[tile & 7].uls = uls; pm_tiles[tile & 7].ult = ult; pm_tiles[tile & 7].lrs = lrs; pm_tiles[tile & 7].lrt = lrt;
#endif
    if (tile == G_TX_RENDERTILE) {
        tri_state.valid = false;
    }
    if (tile == G_TX_RENDERTILE) {
        rdp.texture_tile.uls = uls;
        rdp.texture_tile.ult = ult;
        rdp.texture_tile.lrs = lrs;
        rdp.texture_tile.lrt = lrt;
#ifdef PM64
        pm_render_tile_cm();
#endif
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
}

static void gfx_dp_load_tlut(UNUSED uint8_t tile, uint32_t high_index) {
    tri_state.valid = false;
    _UNUSED(high_index);

    SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(rdp.texture_to_load.siz == G_IM_SIZ_16b);
    rdp.palette = rdp.texture_to_load.addr;
#ifdef PM64
    {   /* bank(s) by the load tile's TMEM address; mirror into the TMEM upper half */
        const int tmem = rdp.tile_tmem[tile & 7], count = (int) high_index + 1;
        if (tmem >= 0x100) {
            for (int b = (tmem - 0x100) / 0x10, i = 0; b < 16 && i < count; b++, i += 16) pm_tlut[b] = rdp.palette + i * 2;
            for (int i = 0; i < count && (tmem - 0x100 + i) < 256; i++)
                for (int k = 0; k < 4; k++) memcpy(pm_tmem_hi + ((tmem - 0x100 + i) * 4 + k) * 2, rdp.palette + i * 2, 2);
            const int s = pm_ltslots_on() ? gfx_lt_slot_for_load(0x100, sizeof(pm_tmem_hi)) : 1; /* the whole upper half */
            rdp.loaded_texture[s].addr = pm_tmem_hi;
            rdp.loaded_texture[s].size_bytes = sizeof(pm_tmem_hi);
            rdp.loaded_texture[s].stride_bytes = 0;
            rdp.loaded_texture[s].row_bytes = 0;
            rdp.loaded_texture[s].line_bytes = 0;
            rdp.textures_changed[0] = rdp.textures_changed[1] = true;
        }
    }
#endif
    if (gfx_debug_frame) port_log("  loadtlut addr %p first %02X%02X %02X%02X %02X%02X\n", rdp.palette, rdp.palette[0], rdp.palette[1], rdp.palette[2], rdp.palette[3], rdp.palette[4], rdp.palette[5]);
}

static void gfx_dp_load_block(uint8_t tile, UNUSED uint32_t uls, UNUSED uint32_t ult, uint32_t lrs, uint32_t dxt) {
    tri_state.valid = false;
    _UNUSED(dxt);

    SUPPORT_CHECK(uls == 0);
    SUPPORT_CHECK(ult == 0);
#ifndef PM64
    rdp.texture_to_load.tile_number = (rdp.tile_tmem[tile & 7] / 256) & 1; // the TMEM half this tile loads into
#endif
    
    // The lrs field rather seems to be number of pixels to load
    uint32_t word_size_shift;
    switch (rdp.texture_to_load.siz) {
        case G_IM_SIZ_4b:
            word_size_shift = 0; // Or -1? It's unused in SM64 anyway.
            break;
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
    }
    uint32_t size_bytes = (lrs + 1) << word_size_shift;
#ifdef PM64
    rdp.texture_to_load.tile_number = (uint8_t) gfx_lt_slot_for_load(rdp.tile_tmem[tile & 7], size_bytes);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].line_bytes = 0;
#endif
    rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes = size_bytes;
    SUPPORT_CHECK(size_bytes <= 4096);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].addr = rdp.texture_to_load.addr;
    rdp.loaded_texture[rdp.texture_to_load.tile_number].stride_bytes = 0;
#ifdef PM64
    rdp.loaded_texture[rdp.texture_to_load.tile_number].row_bytes = 0;
    rdp.textures_changed[0] = rdp.textures_changed[1] = true; /* either texture unit may read it */
    pm_frame_texture_sync(rdp.texture_to_load.addr, rdp.texture_to_load.addr + size_bytes);
#else
    rdp.textures_changed[rdp.texture_to_load.tile_number] = true;
#endif
}

static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    tri_state.valid = false;
#ifndef PM64
    rdp.texture_to_load.tile_number = (rdp.tile_tmem[tile & 7] / 256) & 1; // the TMEM half this tile loads into
#endif
    {
        static int n;
        if (gfx_debug_frame) port_log("  loadtile uls %u ult %u lrs %u lrt %u (u10.2) imgw %u\n", (unsigned) uls, (unsigned) ult, (unsigned) lrs, (unsigned) lrt, rdp.texture_to_load.width);
    }
    // MK64 streams large images (title background) in row strips: the loaded
    // rectangle starts (uls, ult) texels into the image.  Rows are contiguous
    // only when the strip spans the whole image width, which is the case here.

    uint32_t word_size_shift;
    switch (rdp.texture_to_load.siz) {
        case G_IM_SIZ_4b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
    }

    uint32_t size_bytes = ((((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1) * (((lrt - ult) >> G_TEXTURE_IMAGE_FRAC) + 1)) << word_size_shift;
#ifdef PM64
    rdp.texture_to_load.tile_number = (uint8_t) gfx_lt_slot_for_load(rdp.tile_tmem[tile & 7], size_bytes);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].line_bytes = (uint16_t) (pm_tiles[tile & 7].line * 8);
#endif
    rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes = size_bytes;

    SUPPORT_CHECK(size_bytes <= 4096);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].addr =
        rdp.texture_to_load.addr + gfx_texture_image_offset(uls >> G_TEXTURE_IMAGE_FRAC, ult >> G_TEXTURE_IMAGE_FRAC);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].stride_bytes = gfx_texture_image_offset(0, 1);
#ifdef PM64
    rdp.loaded_texture[rdp.texture_to_load.tile_number].row_bytes = (((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1) << word_size_shift;
    {
        const uint8_t *lo = rdp.loaded_texture[rdp.texture_to_load.tile_number].addr;
        const uint32_t rows = ((lrt - ult) >> G_TEXTURE_IMAGE_FRAC) + 1;
        pm_frame_texture_sync(lo, lo + (rows - 1) * rdp.loaded_texture[rdp.texture_to_load.tile_number].stride_bytes +
                                      rdp.loaded_texture[rdp.texture_to_load.tile_number].row_bytes);
    }
    /* LoadTile sets the corners of the tile it loads through, never the render tile's (unless it is the
     * one). Paper Mario loads TEXEL1's texture after sizing TEXEL0's tile (the light rays over Peach's
     * castle), and taking the load rectangle as the render tile's size imported TEXEL0's 16x64 mask as
     * 16x8. PM64_LOADTILE=0: the old behaviour (A/B). */
    const bool sLoadTileOwn = pm_loadtile_own();
    if (sLoadTileOwn) {
        pm_tiles[tile & 7].uls = (uint16_t) uls; pm_tiles[tile & 7].ult = (uint16_t) ult;
        pm_tiles[tile & 7].lrs = (uint16_t) lrs; pm_tiles[tile & 7].lrt = (uint16_t) lrt;
    }
    if (!sLoadTileOwn || (tile & 7) == pm_prim_tile) {
        rdp.texture_tile.uls = uls;
        rdp.texture_tile.ult = ult;
        rdp.texture_tile.lrs = lrs;
        rdp.texture_tile.lrt = lrt;
        pm_render_tile_cm();
    }
#else
    rdp.texture_tile.uls = uls;
    rdp.texture_tile.ult = ult;
    rdp.texture_tile.lrs = lrs;
    rdp.texture_tile.lrt = lrt;
#endif

#ifdef PM64
    rdp.textures_changed[0] = rdp.textures_changed[1] = true; /* either texture unit may read it */
#else
    rdp.textures_changed[rdp.texture_to_load.tile_number] = true;
#endif
}


static uint8_t color_comb_component(uint32_t v) {
    switch (v) {
        case G_CCMUX_TEXEL0:
            return CC_TEXEL0;
        case G_CCMUX_TEXEL1:
            return CC_TEXEL1;
        case G_CCMUX_PRIMITIVE:
            return CC_PRIM;
        case G_CCMUX_SHADE:
            return CC_SHADE;
        case G_CCMUX_ENVIRONMENT:
            return CC_ENV;
        case G_CCMUX_TEXEL0_ALPHA:
            return CC_TEXEL0A;
        case G_CCMUX_LOD_FRACTION:
            return CC_LOD;
        default:
            return CC_0;
    }
}

static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return color_comb_component(a) |
           (color_comb_component(b) << 3) |
           (color_comb_component(c) << 6) |
           (color_comb_component(d) << 9);
}

static void gfx_dp_set_combine_mode(uint32_t rgb, uint32_t alpha, uint32_t flags) {
    uint32_t mode = rgb | (alpha << 12) | flags;
    if (mode != rdp.combine_mode) {
        rdp.combine_mode = mode;
        tri_state.valid = false;
    }
}

static void gfx_dp_set_env_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (rdp.env_color.r != r || rdp.env_color.g != g || rdp.env_color.b != b || rdp.env_color.a != a) {
        tri_state.valid = false;
    }
    rdp.env_color.r = r;
    rdp.env_color.g = g;
    rdp.env_color.b = b;
    rdp.env_color.a = a;
}

static void gfx_dp_set_prim_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (rdp.prim_color.r != r || rdp.prim_color.g != g || rdp.prim_color.b != b || rdp.prim_color.a != a) {
        tri_state.valid = false;
    }
    rdp.prim_color.r = r;
    rdp.prim_color.g = g;
    rdp.prim_color.b = b;
    rdp.prim_color.a = a;
}

static void gfx_dp_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.fog_color.r = r;
    rdp.fog_color.g = g;
    rdp.fog_color.b = b;
    rdp.fog_color.a = a;
}

static void gfx_dp_set_fill_color(uint32_t packed_color) {
    uint16_t col16 = (uint16_t)packed_color;
    uint32_t r = col16 >> 11;
    uint32_t g = (col16 >> 6) & 0x1f;
    uint32_t b = (col16 >> 1) & 0x1f;
    uint32_t a = col16 & 1;
    rdp.fill_color.r = SCALE_5_8(r);
    rdp.fill_color.g = SCALE_5_8(g);
    rdp.fill_color.b = SCALE_5_8(b);
    rdp.fill_color.a = a * 255;
}

static void gfx_draw_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    gfx_set_phase(false);
    uint32_t saved_other_mode_h = rdp.other_mode_h;
    uint32_t cycle_type = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));
    
    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_POINT;
    }
    
    // U10.2 coordinates
    float ulxf = ulx;
    float ulyf = uly;
    float lrxf = lrx;
    float lryf = lry;

    const float out_aspect = (float) gfx_current_dimensions.width / (float) gfx_current_dimensions.height;
#ifndef PORT_GE_TL
    if (out_aspect < 4.0f / 3.0f - 0.01f) {
        /* Output narrower than the game's 4:3 (handheld CONVERT, a square output).
         * The legacy normalize->aspect-adjust->map chain works out to a
         * centre CROP here (net x' = x - 40 at 1:1), clipping edge HUD and
         * the OK/OPTION corners.  Map PROPORTIONALLY instead (x * W/320) --
         * and only that: the ortho-quad HUD (item window, position number,
         * standings) already maps proportionally through its projection, so
         * any per-element anchoring here draws texrect text (LAP/TIME) at a
         * different scale than the quads and they collide (device round 3:
         * LAP overlapping the item box). Uniform proportion = exactly the
         * N64 layout, squeezed; nothing can overlap that didn't on N64. */
        const float sy = (float) gfx_current_dimensions.height / (2.0f * HALF_SCREEN_HEIGHT);
        const float sx = (float) gfx_current_dimensions.width / (2.0f * HALF_SCREEN_WIDTH);
        ulxf = (ulx / 4.0f) * sx;
        lrxf = (lrx / 4.0f) * sx;
        ulyf = (uly / 4.0f) * sy;
        lryf = (lry / 4.0f) * sy;
    } else
#endif
    {
        ulxf = ulxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
        ulyf = (ulyf / (4.0f * HALF_SCREEN_HEIGHT)) - 1.0f;
        lrxf = lrxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
        lryf = (lryf / (4.0f * HALF_SCREEN_HEIGHT)) - 1.0f;

#if !defined(PORT_GE_TL) || defined(PORT_GE_TL_ASPECT)
        // Aspect-correct 2D to 4:3 only when the 3D path is also aspect-corrected.
        // Under GE_TL (no PORT_GE_TL_ASPECT) the 3D fills the wide screen, so 2D
        // must fill too -- otherwise menus/backgrounds pillarbox (~59px bars) while
        // the game is widescreen.
        ulxf = gfx_adjust_x_for_aspect_ratio(ulxf);
        lrxf = gfx_adjust_x_for_aspect_ratio(lrxf);
#endif

        /* Map to the real output size (the PSP build hardcoded its 480x272 halves,
         * 240/136; on a 320x240 target that oversized and offset every 2D rect). */
        {
            const float hw = 0.5f * (float) gfx_current_dimensions.width;
            const float hh = 0.5f * (float) gfx_current_dimensions.height;
            ulxf = (ulxf * hw) + hw;
            lrxf = (lrxf * hw) + hw;
#ifdef PORT_GE_TL
            if (gfx_hud_anchor) {
                float x0 = ulx / 4.0f, x1 = lrx / 4.0f;
                int cls = hud_class(x0, x1);
                if (cls != HUD_CLASS_FULL) {
                    ulxf = x0 * hud_scale() + hud_shift(cls);
                    lrxf = x1 * hud_scale() + hud_shift(cls);
                }
            }
#endif
            ulyf = (ulyf * hh) + hh;
            lryf = (lryf * hh) + hh;
        }
    }
    
    struct VertexColor* ul = &rsp.loaded_vertices_2D[0];
    struct VertexColor* lr = &rsp.loaded_vertices_2D[1];
    
    /* Round, don't truncate: the normalize->scale round trip leaves coords at
     * e.g. 47.9999; truncation opened 1-px seams between adjacent 2D tiles. */
    ul->x = (unsigned short)(ulxf + 0.5f);
    ul->y = (unsigned short)(ulyf + 0.5f);

    lr->x = (unsigned short)(lrxf + 0.5f);
    lr->y = (unsigned short)(lryf + 0.5f);
#ifdef PM64
    {   /* PM64_RECTCENTER=1: 2D rectangles on pixel centres, column c inside when x0 <= c + 0.5 < x1, as the scissor
         * (R23's separate switch, Smash's SSB64_RECTCENTER=2): ceil(x - 0.5), which still takes 47.9999 to 48.
         * Half-up differs only on an edge at exactly .5. Off by default until the real game says otherwise. */
        static int sRectCentre SAVESTATE_EXCLUDE = -1;
        if (sRectCentre < 0) { const char *e = getenv("PM64_RECTCENTER"); sRectCentre = e && e[0] == '1'; }
        const unsigned short cx0 = (unsigned short) port_iceilf(ulxf - 0.5f), cy0 = (unsigned short) port_iceilf(ulyf - 0.5f);
        const unsigned short cx1 = (unsigned short) port_iceilf(lrxf - 0.5f), cy1 = (unsigned short) port_iceilf(lryf - 0.5f);
        if (pm_census_on() && (cx0 != ul->x || cy0 != ul->y || cx1 != lr->x || cy1 != lr->y)) pm_cs.rects_centre++;
        if (sRectCentre) { ul->x = cx0; ul->y = cy0; lr->x = cx1; lr->y = cy1; }
    }
#endif

    // The coordinates for texture rectangle shall bypass the viewport setting
    struct XYWidthHeight default_viewport = {0, 0, gfx_current_dimensions.width, gfx_current_dimensions.height};
    struct XYWidthHeight viewport_saved = rdp.viewport;
    uint32_t geometry_mode_saved = rsp.geometry_mode;
    
    rdp.viewport = default_viewport;
    rdp.viewport_or_scissor_changed = true;
    tri_state.valid = false;
    rsp.geometry_mode = 0;
    
    gfx_sp_tri1_2d(0, 1, 2);
    
    rsp.geometry_mode = geometry_mode_saved;
    rdp.viewport = viewport_saved;
    rdp.viewport_or_scissor_changed = true;
    tri_state.valid = false;
    
    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = saved_other_mode_h;
    }
}

static void gfx_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    if (gfx_debug_frame) port_log("  texrect othermode_l %08X h %08X combine %08X\n", (unsigned) rdp.other_mode_l, (unsigned) rdp.other_mode_h, (unsigned) rdp.combine_mode);
    _UNUSED(tile);
#ifdef PM64
    /* A texture rectangle aimed at the depth buffer (the colour image set to the Z image) must not reach the screen,
     * as the depth clears already don't (the cross-port R34, Smash 3b03a66). PM64_ZIMGRECT=1 draws it (A/B). */
    if (rdp.z_buf_address && rdp.color_image_address == rdp.z_buf_address) {
        if (pm_census_on()) pm_cs.zimg_rects++;
        if (!pm_zimgrect && !pm64_zimg_begin()) return; /* into the target over the depth image, never the screen */
    }
    if (tile == G_TX_RENDERTILE && pm_census_on()) pm_cs.rects++;
    if (tile != G_TX_RENDERTILE) { /* draw from that tile, then put the render tile back */
        gfx_flush();
        pm_select_tile(tile & 7);
        gfx_dp_texture_rectangle(ulx, uly, lrx, lry, G_TX_RENDERTILE, uls, ult, dsdx, dtdy, flip);
        gfx_flush();
        pm_select_tile(G_TX_RENDERTILE);
        return;
    }
#endif

    uint32_t saved_combine_mode = rdp.combine_mode;
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1 instead
        dsdx >>= 2;
        
        // Color combiner is turned off in copy mode
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_TEXEL0), color_comb(0, 0, 0, G_ACMUX_TEXEL0), 0);
        
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    } else {
        // 1/2-cycle: the lower-right edge is exclusive, so rectangles drawn to (319, 239) leave a 1 px seam the CRT's
        // overscan hid. Ending one pixel short covers it; larger rectangles stay as they are (MK64 99921c7's fill rule;
        // DKR 9df6228; the one-short band from Star Fox, which clears 0,0,320,720).
        if (lrx >= (SCREEN_WIDTH - 1) << 2 && lrx < SCREEN_WIDTH << 2) lrx = SCREEN_WIDTH << 2;
        if (lry >= (SCREEN_HEIGHT - 1) << 2 && lry < SCREEN_HEIGHT << 2) lry = SCREEN_HEIGHT << 2;
    }
    
    // uls and ult are S10.5
    // dsdx and dtdy are S5.10
    // lrx, lry, ulx, uly are U10.2
    // lrs, lrt are S10.5
    if (flip) {
        dsdx = -dsdx;
        dtdy = -dtdy;
    }
    int16_t width = !flip ? lrx - ulx : lry - uly;
    int16_t height = !flip ? lry - uly : lrx - ulx;
    float lrs = ((uls << 7) + dsdx * width) >> 7;
    float lrt = ((ult << 7) + dtdy * height) >> 7;
    
    struct VertexColor* ul = &rsp.loaded_vertices_2D[0];
    struct VertexColor* lr = &rsp.loaded_vertices_2D[1];
    ul->u = uls;
    ul->v = ult;
    lr->u = lrs;
    lr->v = lrt;
    /*@Note: fix this */
    
    /* An inverted rectangle (lower-right left of or above upper-left, after the copy-mode edge)
     * covers no pixel on the RDP. The 2D path swapped the corners and drew it stretched across the
     * screen with a negative S/T step (DKR's name entry: a glyph ending at x = 0 keeps a negative
     * ulx, which the 12-bit field wraps to ~1016 px; diddy-kong-racing 8131e79, GoldenEye d9c6f6f).
     * PM64_TEXRECTINV=1: drawn as before. */
    if (lrx < ulx || lry < uly) {
        static int draw SAVESTATE_EXCLUDE = -1;
        if (draw < 0) { const char *e = getenv("PM64_TEXRECTINV"); draw = e && e[0] == '1'; }
        if (!draw) { rdp.combine_mode = saved_combine_mode; return; }
    }
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    if (rdp.color_image_address == rdp.z_buf_address) {
#ifdef PM64
        /* Paper Mario clears depth with a full-screen fill on the depth image
         * (the backend no longer clears per task, see gfx_soft_start_frame). */
        extern void gfx_soft_clear_depth(void);
        gfx_flush();
        gfx_soft_clear_depth();
#endif
        // Don't clear Z buffer here since we already did it with glClear
        return;
    }
    uint32_t mode = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));
    
    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    } else {
        // 1/2-cycle: the lower-right edge is exclusive, so full-screen boxes
        // (0, 0, 319, 239) leave the last column and row uncovered: a bright
        // 1 px seam on the panel (mario-kart-64 99921c7).
        if (lrx >= (SCREEN_WIDTH - 1) << 2) lrx = SCREEN_WIDTH << 2; /* plain form: a fill larger than the screen becomes an exact full-screen fill (the cross-port lead, from Smash) */
        if (lry >= (SCREEN_HEIGHT - 1) << 2) lry = SCREEN_HEIGHT << 2;
    }
    /* The same for a fill: an inverted rectangle covers no pixel on the RDP (texrect above).
     * PM64_TEXRECTINV=1: drawn as before. */
    if (lrx < ulx || lry < uly) {
        static int draw SAVESTATE_EXCLUDE = -1;
        if (draw < 0) { const char *e = getenv("PM64_TEXRECTINV"); draw = e && e[0] == '1'; }
        if (!draw) return;
    }
    
    for (int i = 0; i < 2; i++) {
        struct VertexColor* v = &rsp.loaded_vertices_2D[i];
        v->color = rdp.fill_color;
    }
    
    uint32_t saved_combine_mode = rdp.combine_mode;
    if (mode == G_CYC_FILL || mode == G_CYC_COPY) {
        // FILL mode: the RDP writes the fill colour straight to the framebuffer.
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_SHADE), color_comb(0, 0, 0, G_ACMUX_SHADE), 0);
    }
#ifdef PM64
    extern int gfx_soft_fill_raw_alpha; /* the fill's own alpha bit into a texture target (gfx_soft.c) */
    static int sFillAlpha = -1;
    if (sFillAlpha < 0) { const char *e = getenv("PM64_FILLALPHA"); sFillAlpha = !(e && e[0] == '0'); }
    gfx_soft_fill_raw_alpha = sFillAlpha && mode == G_CYC_FILL ? rdp.fill_color.a : -1;
#endif
    // 1/2-cycle mode: the rectangle goes through the current combiner and
    // blender like any primitive.  MK64's draw_box (pause dim, menu fades,
    // the lightning flash, the fall-off blackout) does exactly that: XLU_SURF
    // + G_CC_PRIMITIVE with the box colour and an 8-bit alpha in the prim
    // colour.  Forcing the fill colour here (what sm64-port did) drew those
    // boxes opaque black or not at all, depending on the fill colour's 1-bit
    // alpha.
    gfx_draw_rectangle(ulx, uly, lrx, lry);
#ifdef PM64
    gfx_soft_fill_raw_alpha = -1;
#endif
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_set_z_image(void *z_buf_address) {
    rdp.z_buf_address = z_buf_address;
}

#ifdef PM64
extern void gfx_soft_rt_begin(void *mem, int width, int siz, int rows);
extern void gfx_soft_rt_end(void);
static bool pm64_rt_active;
#ifdef PM64_REPLAY
extern uint8_t *pm64_rdram; /* replay: the captured RDRAM image */
/* Paper Mario's framebuffers are the three nusys cfbs at 0x8038F800..0x80400000; the
 * depth image is only ever a fill target (gfx_dp_fill_rectangle). Anything else is a
 * render-to-texture target (see gfx_soft_rt_begin). */
static bool pm64_is_framebuffer(const void *address) {
    const uint32_t phys = (uint32_t) ((const uint8_t *) address - pm64_rdram);
    return phys >= 0x38F800u && phys < 0x400000u;
}
static uint32_t pm64_addr_id(const void *address) { return (uint32_t) ((const uint8_t *) address - pm64_rdram); }
static int pm64_rt_rows(const void *address, int stride) { return (int) ((0x400000u - pm64_addr_id(address)) / (uint32_t) stride); }
#else
/* The native game (M2): the port knows its nusys framebuffers (nuGfxCfb[]); a target elsewhere holds at most a
 * screen's rows. */
extern bool port_is_framebuffer(const void *address);
static bool pm64_is_framebuffer(const void *address) { return port_is_framebuffer(address); }
static uint32_t pm64_addr_id(const void *address) { return (uint32_t) (uintptr_t) address; }
static int pm64_rt_rows(const void *address, int stride) { (void) address; (void) stride; return 240; }
#endif
/* The last nusys framebuffer a SETCIMG chose (RDRAM offset). Each game frame draws into the next of the three,
 * so the bench starts a game frame when it changes (round 9). */
uint32_t gfx_pm64_main_cimg;
static void pm64_rt_switch(void) {
    gfx_flush();
    memset(&rendering_state.viewport, 0, sizeof(rendering_state.viewport));
    memset(&rendering_state.scissor, 0, sizeof(rendering_state.scissor));
    rdp.viewport_or_scissor_changed = true;
    tri_state.valid = false;
}
/* The depth image as a colour image: a fill there is the depth clear (gfx_dp_fill_rectangle), but the dark rooms draw
 * their light stencil into that memory (RGBA32, then an I8 copy, screen_render_util.c appendGfx_darkness_stencil) and
 * texture the screen with it. So the first triangle or texture rectangle aimed at it begins a target over it; the
 * depth clears alone never do (they come every frame). */
static bool pm64_zimg_pending;
static uint32_t pm64_zimg_width, pm64_zimg_size;
static bool pm64_zimg_begin(void) {
    if (!pm64_zimg_pending) return pm64_rt_active;
    pm64_zimg_pending = false;
    const int bpp = pm64_zimg_size == G_IM_SIZ_32b ? 4 : pm64_zimg_size == G_IM_SIZ_16b ? 2 : 1;
    pm64_rt_switch();
    gfx_soft_rt_begin(rdp.z_buf_address, (int) pm64_zimg_width,
                      pm64_zimg_size == G_IM_SIZ_32b ? 3 : pm64_zimg_size == G_IM_SIZ_16b ? 2 : 1,
                      pm64_rt_rows(rdp.z_buf_address, (int) pm64_zimg_width * bpp));
    pm64_rt_active = true;
    return true;
}
#endif
static void gfx_dp_set_color_image(uint32_t format, uint32_t size, uint32_t width, void* address) {
    _UNUSED(format);
    _UNUSED(size);
    _UNUSED(width);

#ifdef PM64
    if (pm64_is_framebuffer(address)) { gfx_pm64_main_cimg = pm64_addr_id(address); pm_cfb = (const uint8_t *) address; }
    pm64_zimg_pending = false;
    if (address == rdp.z_buf_address && rdp.z_buf_address != NULL) {
        if (pm64_rt_active) { /* leave any target: what follows is the depth clear or the stencil */
            pm64_rt_switch();
            gfx_soft_rt_end();
            pm64_rt_active = false;
        }
        pm64_zimg_pending = true;
        pm64_zimg_width = width + 1; /* the command carries width - 1 */
        pm64_zimg_size = size;
    } else if (address != rdp.z_buf_address) {
        if (!pm64_is_framebuffer(address)) {
            width += 1; /* the command carries width - 1 */
            const int bpp = size == G_IM_SIZ_32b ? 4 : size == G_IM_SIZ_16b ? 2 : 1;
            const int rows = pm64_rt_rows(address, (int) width * bpp);
            pm64_rt_switch();
            gfx_soft_rt_begin(address, (int) width, size == G_IM_SIZ_32b ? 3 : size == G_IM_SIZ_16b ? 2 : 1, rows);
            pm64_rt_active = true;
        } else if (pm64_rt_active) {
            pm64_rt_switch();
            gfx_soft_rt_end();
            pm64_rt_active = false;
        }
    }
#endif
    rdp.color_image_address = address;
}

static void gfx_sp_set_other_mode(uint32_t shift, uint32_t num_bits, uint64_t mode) {
    uint32_t old_l = rdp.other_mode_l, old_h = rdp.other_mode_h;
    uint64_t mask = (((uint64_t)1 << num_bits) - 1) << shift;
    uint64_t om = rdp.other_mode_l | ((uint64_t)rdp.other_mode_h << 32);
    om = (om & ~mask) | mode;
    rdp.other_mode_l = (uint32_t)om;
    rdp.other_mode_h = (uint32_t)(om >> 32);
    if (old_l != rdp.other_mode_l || old_h != rdp.other_mode_h) {
        tri_state.valid = false;
    }
}

#ifdef PM64_REPLAY
/* M1 replay bench (paper-mario): display lists hold N64 addresses into a captured
 * RDRAM image. Resolve exactly as the RSP does: segment table (gSPSegment, reset
 * every task in gfx_run) + the low 24 bits; KSEG0 0x80xxxxxx lands on segment 0. */
extern uint8_t *pm64_rdram;
static inline void *seg_addr(uintptr_t w1) {
    uint32_t a = (uint32_t) w1;
    return pm64_rdram + ((rsp_segments[(a >> 24) & 0xF] + (a & 0x00FFFFFF)) & 0x003FFFFF);
}
#elif defined(PM64)
/* The native game (M2): its code and data sit above 0x40000000, so an address below 0x10000000 is segmented (the
 * segment number in its top byte, as the RSP reads it: effect gfx 0x09, entity models 0x0A) and anything else is a
 * host pointer. The port checks the result (port_seg_to_ptr logs what lands nowhere). */
extern void* port_seg_to_ptr(uintptr_t addr);
static inline void *seg_addr(uintptr_t w1) {
    const uint32_t a = (uint32_t) w1;
    if (a >= 0x10000000u) return (void *) (uintptr_t) a;
    return port_seg_to_ptr((uintptr_t) rsp_segments[(a >> 24) & 0xF] + (a & 0x00FFFFFF));
}
#else
extern void* port_seg_to_ptr(uintptr_t addr);
static inline void *seg_addr(uintptr_t w1) {
    return port_seg_to_ptr(w1);
}
#endif


int gfx_trace_frames = 0;
int gfx_debug_frame = 0;
float gfx_last_render_ms = 16.0f; // last frame's interpreter+raster cost (WM res ladder)
int gfx_dump_textures = 0;
uint16_t gDebugKartTex[2][64 * 32] __attribute__((aligned(16)));
int gDebugKartTexCount = 0;
uint16_t gDebugTex32[32 * 32] __attribute__((aligned(16)));
int gDebugTex32Valid = 0;

/* Debug: forget every cached texture so the next frame re-imports them all. */
void gfx_debug_flush_texture_cache(void) {
    gfx_texture_cache_reset(true);
}
#define C0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << width) - 1))
#define C1(pos, width) ((cmd->words.w1 >> (pos)) & ((1U << width) - 1))

extern void port_log(const char* fmt, ...);
static uint32_t gfx_cmd_budget;
static uint32_t sRdpHalf1; /* the last G_RDPHALF_1 word: G_BRANCH_Z's target */

/* A display-list call to no possible address (0, or unaligned filler like 0x55555505): logged with the list it is in
 * and skipped, instead of a crash in the renderer (the native game, M2: the map sweep found maps that draw one). */
static int gfx_bad_dl_target(const Gfx* list, const Gfx* cmd, uintptr_t target) {
    static int sLogged;
    if (target >= 0x10000 && (target & 3) == 0) return 0;
    if (sLogged < 20) {
        sLogged++;
        port_log("gfx: display list call to 0x%08X from %p (list %p, %08X %08X): skipped\n", (unsigned) target,
                 (const void*) cmd, (const void*) list, (unsigned) cmd->words.w0, (unsigned) cmd->words.w1);
        if (getenv("PM64_BADDL_CONTEXT")) /* the commands before it: which code emitted the call */
            for (const Gfx* c = cmd - 12 < list ? list : cmd - 12; c < cmd; c++)
                port_log("    %p: %08X %08X\n", (const void*) c, (unsigned) c->words.w0, (unsigned) c->words.w1);
    }
    return 1;
}

static void gfx_run_dl(Gfx* cmd) {
    Gfx* const list = cmd;
    for (;;) {
        uint32_t opcode = cmd->words.w0 >> 24;
        if (gfx_trace_frames > 0 && gfx_cmd_budget < 4000) {
            port_log("dl %p: %08X %08X\n", cmd, (unsigned) cmd->words.w0, (unsigned) cmd->words.w1);
        }
        if (++gfx_cmd_budget > 400000) {
            port_log("gfx_run_dl: runaway display list at %p (op %02X)\n", cmd, opcode);
            return;
        }
#ifdef PORT_PROFILE_DL
        gfx_prof_opcount[opcode]++;
        gfx_prof_cmds++;
        int prof_slot = (opcode == 0x04) ? 4 : (opcode == 0xBF || opcode == 0xB1 || opcode == 0xB5) ? 5 : (opcode == 0x06 || opcode == 0xB8 || opcode == 0x00) ? 0 : 6;
        uint32_t prof_t0 = prof_slot ? port_time_us() : 0;
#endif
        switch (opcode) {
            case G_NOOP:
                if (cmd->words.w1 == PORT_HUD_TAG_ON || cmd->words.w1 == PORT_HUD_TAG_OFF || cmd->words.w1 == PORT_HUD_TAG_CENTRE) {
                    int mode = cmd->words.w1 == PORT_HUD_TAG_ON ? 1 : cmd->words.w1 == PORT_HUD_TAG_CENTRE ? 2 : 0;
                    if (mode != gfx_hud_anchor) gfx_flush();
                    gfx_hud_anchor = mode;
                }
                break;
            // RSP commands:
            case G_MTX:
                gfx_flush();
#ifdef F3DEX_GBI_2
                gfx_sp_matrix(C0(0, 8) ^ G_MTX_PUSH, (const int32_t *) seg_addr(cmd->words.w1));
#else
                gfx_sp_matrix(C0(16, 8), (const int32_t *) seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_POPMTX:
#ifdef F3DEX_GBI_2
                gfx_sp_pop_matrix(cmd->words.w1 / 64);
#else
                gfx_sp_pop_matrix(1);
#endif
                break;
            case G_MOVEMEM:
#ifdef F3DEX_GBI_2
                gfx_sp_movemem(C0(0, 8), C0(8, 8) * 8, seg_addr(cmd->words.w1));
#else
                gfx_sp_movemem(C0(16, 8), 0, seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_MOVEWORD:
#ifdef F3DEX_GBI_2
                gfx_sp_moveword(C0(16, 8), C0(0, 16), cmd->words.w1);
#else
                gfx_sp_moveword(C0(0, 8), C0(8, 16), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_TEXTURE:
#ifdef F3DEX_GBI_2
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(1, 7));
#else
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(0, 8));
#endif
                break;
            case G_VTX:
#ifdef F3DEX_GBI_2
                gfx_sp_vertex(C0(12, 8), C0(1, 7) - C0(12, 8), seg_addr(cmd->words.w1));
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_vertex(C0(10, 6), C0(16, 8) / 2, seg_addr(cmd->words.w1));
#else
                gfx_sp_vertex((C0(0, 16)) / sizeof(Vtx), C0(16, 4), seg_addr(cmd->words.w1));
#endif
                break;
            case G_DL:
                if (gfx_bad_dl_target(list, cmd, (uintptr_t)seg_addr(cmd->words.w1))) {
                    if (C0(16, 1) != 0) return; /* a branch: this list ends here */
                    break;
                }
                if (C0(16, 1) == 0) {
                    // Push return address
                    gfx_run_dl((Gfx *)seg_addr(cmd->words.w1));
                } else {
                    cmd = (Gfx *)seg_addr(cmd->words.w1);
                    --cmd; // increase after break
                }
                break;
            case (uint8_t)G_RDPHALF_1: // its word is G_BRANCH_Z's target; texrects consume their own halves
                sRdpHalf1 = cmd->words.w1;
                break;
            case (uint8_t)G_BRANCH_Z:
                // gSPBranchLessZraw: jump (no push) when the vertex is nearer than zval. Always take it, the
                // high-detail branch, as ocarina-of-time v13 does (0 uses on the m1 scenes; map lists are ROM data).
                if (sRdpHalf1 != 0) {
                    cmd = (Gfx *)seg_addr(sRdpHalf1);
                    --cmd; // increase after break
                }
                break;
            case (uint8_t)G_ENDDL:
                return;
#if defined(PM64) && defined(F3DEX_GBI_2)
            case (uint8_t)G_MODIFYVTX: /* w0: where (bits 16-23), the vertex index x 2 (bits 0-15) */
                gfx_sp_modify_vertex(C0(0, 16) / 2, C0(16, 8), cmd->words.w1);
                break;
#endif
#ifdef F3DEX_GBI_2
            case G_GEOMETRYMODE:
                gfx_sp_geometry_mode(~C0(0, 24), cmd->words.w1);
                break;
#else
            case (uint8_t)G_SETGEOMETRYMODE:
                gfx_sp_geometry_mode(0, cmd->words.w1);
                break;
            case (uint8_t)G_CLEARGEOMETRYMODE:
                gfx_sp_geometry_mode(cmd->words.w1, 0);
                break;
#endif
            case (uint8_t)G_TRI1:
#ifdef F3DEX_GBI_2
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
#else
                gfx_sp_tri1(C1(16, 8) / 10, C1(8, 8) / 10, C1(0, 8) / 10);
#endif
                break;
#if defined(F3DEX_GBI) || defined(F3DLP_GBI)
            case (uint8_t)G_TRI2:
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
                break;
#endif
#ifdef F3D_OLD
            // Early F3DEX quadrangle (MK64 startup logo): w1 = v3<<24 | v0<<16 | v1<<8 | v2 (indices * 2).
            case (uint8_t)G_QUAD:
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
                gfx_sp_tri1(C1(16, 8) / 2, C1(0, 8) / 2, C1(24, 8) / 2);
                break;
#endif
            case (uint8_t)G_SETOTHERMODE_L:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(31 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, cmd->words.w1);
#else
                gfx_sp_set_other_mode(C0(8, 8), C0(0, 8), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_SETOTHERMODE_H:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(63 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, (uint64_t) cmd->words.w1 << 32);
#else
                gfx_sp_set_other_mode(C0(8, 8) + 32, C0(0, 8), (uint64_t) cmd->words.w1 << 32);
#endif
                break;
            
            // RDP Commands:
#ifdef PM64
            case (uint8_t)G_SETPRIMDEPTH:
                if (pm_prim_depth != (uint16_t) (cmd->words.w1 >> 16)) tri_state.valid = false;
                pm_prim_depth = (uint16_t) (cmd->words.w1 >> 16);
                break;
#endif
            case G_SETTIMG:
                gfx_dp_set_texture_image(C0(21, 3), C0(19, 2), C0(0, 10), seg_addr(cmd->words.w1));
                break;
            case G_LOADBLOCK:
                gfx_dp_load_block(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTILE:
                gfx_dp_load_tile(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETTILE:
                gfx_dp_set_tile(C0(21, 3), C0(19, 2), C0(9, 9), C0(0, 9), C1(24, 3), C1(20, 4), C1(18, 2), C1(14, 4), C1(10, 4), C1(8, 2), C1(4, 4), C1(0, 4));
                break;
            case G_SETTILESIZE:
                gfx_dp_set_tile_size(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTLUT:
                gfx_dp_load_tlut(C1(24, 3), C1(14, 10));
                break;
            case G_SETENVCOLOR:
                gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMCOLOR:
#ifdef PM64
                if (rdp.pm_prim_lod != C0(0, 8)) tri_state.valid = false;
                rdp.pm_prim_lod = C0(0, 8);
#endif
                gfx_dp_set_prim_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFOGCOLOR:
#ifdef PM64
                if (cmd->words.w1 != ((uint32_t) rdp.fog_color.r << 24 | rdp.fog_color.g << 16 | rdp.fog_color.b << 8 | rdp.fog_color.a))
                    tri_state.valid = false; /* the shroud's amount (pm_shroud) */
#endif
                gfx_dp_set_fog_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
#ifdef PM64
            case G_SETBLENDCOLOR: /* the shroud's tint (pm_shroud) */
                if (cmd->words.w1 != ((uint32_t) rdp.blend_color.r << 24 | rdp.blend_color.g << 16 | rdp.blend_color.b << 8 | rdp.blend_color.a))
                    tri_state.valid = false;
                rdp.blend_color.r = C1(24, 8); rdp.blend_color.g = C1(16, 8); rdp.blend_color.b = C1(8, 8); rdp.blend_color.a = C1(0, 8);
                break;
#endif
            case G_SETFILLCOLOR:
                gfx_dp_set_fill_color(cmd->words.w1);
                break;
            case G_SETCOMBINE:
#ifdef PM64
                rdp.pm_cc_w0 = cmd->words.w0 & 0x00FFFFFF;
                rdp.pm_cc_w1 = cmd->words.w1;
#endif
                gfx_dp_set_combine_mode(
                    color_comb(C0(20, 4), C1(28, 4), C0(15, 5), C1(15, 3)),
                    color_comb(C0(12, 3), C1(12, 3), C0(9, 3), C1(9, 3)),
                    (C0(20, 4) == G_CCMUX_1 ? CC_FLAG_RGB_A_ONE : 0) | (C1(15, 3) == G_CCMUX_1 ? CC_FLAG_RGB_D_ONE : 0) |
                    (C0(12, 3) == G_ACMUX_1 ? CC_FLAG_ALPHA_A_ONE : 0) | (C1(9, 3) == G_ACMUX_1 ? CC_FLAG_ALPHA_D_ONE : 0));
                    /*color_comb(C0(5, 4), C1(24, 4), C0(0, 5), C1(6, 3)),
                    color_comb(C1(21, 3), C1(3, 3), C1(18, 3), C1(0, 3)));*/
                break;
            // G_SETPRIMCOLOR, G_CCMUX_PRIMITIVE, G_ACMUX_PRIMITIVE, is used by Goddard
            // G_CCMUX_TEXEL1, LOD_FRACTION is used in Bowser room 1
            case G_TEXRECT:
            case G_TEXRECTFLIP:
            {
                int32_t lrx, lry, tile, ulx, uly;
                uint32_t uls, ult, dsdx, dtdy;
#ifdef F3DEX_GBI_2E
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                uls = C0(16, 16);
                ult = C0(0, 16);
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
#else
                lrx = C0(12, 12);
                lry = C0(0, 12);
                tile = C1(24, 3);
                ulx = C1(12, 12);
                uly = C1(0, 12);
                ++cmd;
                uls = C1(16, 16);
                ult = C1(0, 16);
                ++cmd;
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
#endif
                gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == G_TEXRECTFLIP);
                break;
            }
            case G_FILLRECT:
#ifdef F3DEX_GBI_2E
            {
                int32_t lrx, lry, ulx, uly;
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                gfx_dp_fill_rectangle(ulx, uly, lrx, lry);
                break;
            }
#else
                gfx_dp_fill_rectangle(C1(12, 12), C1(0, 12), C0(12, 12), C0(0, 12));
                break;
#endif
            case G_SETSCISSOR:
                gfx_dp_set_scissor(C1(24, 2), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETZIMG:
                gfx_dp_set_z_image(seg_addr(cmd->words.w1));
                break;
            case G_SETCIMG:
                gfx_dp_set_color_image(C0(21, 3), C0(19, 2), C0(0, 11), seg_addr(cmd->words.w1));
                break;
        }
#ifdef PORT_PROFILE_DL
        if (prof_slot) port_profile_add(prof_slot, port_time_us() - prof_t0);
#endif
        ++cmd;
    }
}

static void gfx_sp_reset() {
    rsp.modelview_matrix_stack_size = 1;
    rsp.current_num_lights = 2;
    rsp.lights_changed = true;
#ifdef PM64
    /* Each task starts with the fixed texgen axes the kit always used (LOOKATX = x, LOOKATY = y). */
    memset(rsp.current_lookat, 0, sizeof(rsp.current_lookat));
    rsp.current_lookat[1].dir[0] = 127;
    rsp.current_lookat[0].dir[1] = 127;
#endif
}

void gfx_get_dimensions(uint32_t *width, uint32_t *height) {
    gfx_wapi->get_dimensions(width, height);
}


float times[30];
float time_avg;
float time_first_200;
int total_frame_counter;
int frame_counter;

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *game_name, bool start_in_fullscreen) {
    gfx_wapi = wapi;
    gfx_rapi = rapi;
    gfx_wapi->init(game_name, start_in_fullscreen);
    gfx_rapi->init();
    { /* MK64_DRAWDIST=<clip.w units>: far cull, desktop tuning, off by default.
       * Validated: non-numeric, trailing junk, <= 0 or absurd stays OFF -- the
       * round-12 bare atof() turned "abc" into 0.0f and culled the whole 3D
       * scene with no log line. Read here, once, not in the per-frame gfx_run. */
        const char *e = getenv("MK64_DRAWDIST"); char *end = NULL;
        gfx_draw_dist_w = 1e30f;
        if (e && e[0]) {
            const double d = strtod(e, &end);
            if (end && *end == '\0' && d > 0.0 && d < 1e9) { gfx_draw_dist_w = (float)d; port_log("gfx: far cull at clip.w %.0f (MK64_DRAWDIST)\n", d); }
            else port_log("gfx: ignoring MK64_DRAWDIST='%s' (want a positive number)\n", e);
        }
    }

    { /* the clipper's planes (round 16; see GB_OUT) */
        const char *e;
        if ((e = getenv("PM64_CLIPEDGES")) && e[0] >= '0' && e[0] <= '2') gfx_clipedges = e[0] - '0';
        if ((e = getenv("PM64_NEARCLIP")) && e[0]) gfx_nearclip = e[0] != '0';
        gfx_clip_trigger = Z_NEG | W_LOW | (gfx_nearclip ? Z_POS : 0) | (gfx_clipedges == 2 ? GB_OUT : 0);
        gfx_reject_keep = XY_OUT | GB_OUT | (gfx_nearclip ? 0 : Z_POS);
        { extern int gfx_soft_nearw_on(void); gfx_wmin = gfx_soft_nearw_on() && !gfx_nearclip ? 1.0f / 64.0f : 2.0f; } /* PM64_NEARW */
        pm_zimgrect = (e = getenv("PM64_ZIMGRECT")) && e[0] == '1';
    }

    int i;
    for(i=0;i<30;i++){
        times[i] = 0.0f;
    }
    frame_counter = 0;
    time_avg = 0.0f;
    time_first_200 = 0;
    total_frame_counter = 0;

    // Used in the 120 star TAS
    static uint32_t precomp_shaders[] = {
        0x01200200,
        0x00000045,
        0x00000200,
        0x01200a00,
        0x00000a00,
        0x01a00045,
        0x00000551,
        0x01045045,
        0x05a00a00,
        0x01200045,
        0x05045045,
        0x01045a00,
        0x01a00a00,
        0x0000038d,
        0x01081081,
        0x0120038d,
        0x03200045,
        0x03200a00,
        0x01a00a6f,
        0x01141045,
        0x07a00a00,
        0x05200200,
        0x03200200,
        0x09200200,
        0x0920038d,
        0x09200045
    };
    for (size_t i = 0; i < sizeof(precomp_shaders) / sizeof(uint32_t); i++) {
        gfx_lookup_or_create_shader_program(precomp_shaders[i]);
    }

    memcpy(rsp.P_matrix, identity_matrix, sizeof(identity_matrix));
    memcpy(rsp.modelview_matrix_stack[0], identity_matrix, sizeof(identity_matrix));

    gfx_wapi->get_dimensions(&gfx_current_dimensions.width, &gfx_current_dimensions.height);
    if (gfx_current_dimensions.height == 0) {
        // Avoid division by zero
        gfx_current_dimensions.height = 1;
    }
    gfx_current_dimensions.aspect_ratio = (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
}

struct GfxRenderingAPI *gfx_get_current_rendering_api(void) {
    return gfx_rapi;
}

unsigned int total_t0, total_t1;

/* The overlay changed GE state behind the interpreter's back: forget the
 * cached state so it is re-sent on the next draw. */
void gfx_overlay_state_dirty(void) {
    rendering_state.shader_program = NULL;
    rendering_state.depth_test = false;
    rendering_state.depth_mask = false;
    rendering_state.decal_mode = false;
    rendering_state.alpha_blend = true;
    rendering_state.tex_add = false;
    rendering_state.own_edges = false;
    {
        extern void gfx_scegu_reset_texfunc_add(void);
        extern void gfx_soft_set_own_edges(bool on);
        gfx_scegu_reset_texfunc_add(); // no GE call: the shader reload re-sends the texture function
        gfx_soft_set_own_edges(false); // PM64_FILLRULE: the backend's flag and the cached one agree again
    }
    rendering_state.textures[0] = rendering_state.textures[1] = NULL;
    memset(&rendering_state.viewport, 0, sizeof(rendering_state.viewport));
    memset(&rendering_state.scissor, 0, sizeof(rendering_state.scissor));
    rdp.viewport_or_scissor_changed = true;
    tri_state.valid = false;
}

/* Called right after a savestate load restores .data/.bss. The texture cache
 * (saved) now points into the arena (excluded/flushed), so drop the cache and
 * clear the backend arena; textures re-import from the restored course data on
 * the next frame (same coordinated flush the arena-full path uses each frame).
 * Then forget cached GE state so the whole pipeline is re-sent. */
void gfx_savestate_fixup(void) {
    { extern void port_state_loaded(void) __attribute__((weak)); if (port_state_loaded) port_state_loaded(); } /* PM64: the asset region's pages */
    gfx_texture_cache_reset(false);
    gfx_overlay_state_dirty();
}

void gfx_start_frame(void) {
    gfx_frame_counter++;
#ifdef PORT_GE_TL
    memset(ge_last_mp, 0, sizeof(ge_last_mp)); // GE list reset each frame -> re-push GU_PROJECTION on the first batch
    ge_list_used = 0;
#endif
    gfx_hud_anchor = 0;
    hud_prev_class = -1;
    gfx_flush_index = 0;
    // Recycle the texture arena between frames (the previous frame's display
    // list has fully executed) rather than in the middle of one.
    if (tex_cache_flush_pending) {
        tex_cache_flush_pending = 0;
        gfx_texture_cache_reset(false); /* the course texture block changed hands (port_course_textures_loaded) */
    } else if (texman_usage_percent() > 85) {
        port_log("gfx: frame %u arena reset at %u%% (%u mid-frame resets last frame)\n", (unsigned) gfx_frame_counter, (unsigned) texman_usage_percent(), (unsigned) gfx_midframe_resets);
        gfx_texture_cache_reset(false);
    }
    gfx_midframe_resets = 0;
    if (gfx_sampler_clobbers) {
        port_log("sampler: frame %u: %u tile-1 hits changed tile 0's wrap\n", (unsigned) gfx_frame_counter, (unsigned) gfx_sampler_clobbers);
        gfx_sampler_clobbers = 0;
    }
    //sceIoWrite(1, "----START FRAME!\n", 18);
    total_t0 = sceKernelLibcClock();
    gfx_wapi->handle_events();
}

void gfx_run(Gfx *commands) {
    gfx_sp_reset();
#ifdef PM64
    memset(rsp_segments, 0, sizeof(rsp_segments)); /* the ucode starts every task with a zeroed table */
#endif
    gfx_cmd_budget = 0;
    
    //INFO_MSG("New frame");
    
    if (!gfx_wapi->start_frame()) {
        dropped_frame = true;
        return;
    }
    dropped_frame = false;
    //double t0 = gfx_wapi->get_time();
    unsigned int t0 = sceKernelLibcClock();
    gfx_rapi->start_frame();
    gfx_in_frame = true;
#if defined(TARGET_HANDHELD)
    gfx_phase3d = true; /* the backend opens every frame on the internal target */
    gfx_seen_persp = false;
    memset(&rendering_state.viewport, 0, sizeof(rendering_state.viewport));
    memset(&rendering_state.scissor, 0, sizeof(rendering_state.scissor));
    rdp.viewport_or_scissor_changed = true;
    tri_state.valid = false;
#endif
    gfx_run_dl(commands);
    if (gfx_trace_frames > 0) {
        gfx_trace_frames--;
        port_log("dl done\n");
    }
    gfx_debug_frame = 0; // a one-frame switch (set by the input script / debug hooks before the frame)
    gfx_flush();
#ifdef PM64
    if (pm64_rt_active) { gfx_soft_rt_end(); pm64_rt_active = false; }
    pm_gen_off();
#endif
    {
        extern void port_gfx_overlay(void);
#ifndef PORT_NO_FPS
        if (gPortShowFps) port_gfx_overlay(); // FPS counter, off by default; hold SELECT 3 s to toggle
#endif
    }
    gfx_rapi->end_frame();
    gfx_in_frame = false;
    gfx_wapi->swap_buffers_begin();
    //double t1 = gfx_wapi->get_time();
    unsigned int t1 = sceKernelLibcClock();
    //printf("Process %f %f\n", t1, t1 - t0);
    //printf("Process %d microsec, %f sec\n", t1 - t0, (t1 - t0)/1000000.0f);
    times[frame_counter] = (t1 - t0)/1000.0f;
    frame_counter++;
    time_first_200  += (t1 - t0)/1000.0f;
    total_frame_counter++;
    if(frame_counter>=30){
        frame_counter = 0;
        int i;
        for(i=0;i<30;i++)
            time_avg += times[i];
        time_avg /= 30;
        //printf("GFX AVG: %2.3f ms FPS %2.3f\n", time_avg, 1000/time_avg);
    }
    /* Periodic perf log (every ~5 s of rendered frames). Reports the render
     * cost of the interpreter+rasterizer -- the number that bounds in-game
     * FPS -- plus the current internal resolution, so console.log shows how
     * cost tracks per screen (menu vs each track) and whether dynamic-res is
     * kicking in. Grab the data folder's console.log after a session. */
    gfx_last_render_ms = (t1 - t0) / 1000.0f; /* interpreter+raster+present, this frame */
}

void gfx_end_frame(void) {
    
    //sceIoWrite(1, "----END FRAME!\n", 16);
    if (!dropped_frame) {
        gfx_rapi->finish_render();
        gfx_wapi->swap_buffers_end();
    }

    total_t1 = sceKernelLibcClock();
    float delta = (total_t1 - total_t0)/1000.0f;
    (void)delta;
    if(frame_counter>=29){
        //printf("TOTAL TIME FRAME: %2.3f ms FPS %2.3f\n", delta, 1000/delta);
    }
}
#endif
