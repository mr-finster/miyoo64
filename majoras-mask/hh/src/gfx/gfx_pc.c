#if defined(TARGET_PSP) || defined(TARGET_HANDHELD)
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

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
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
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
/* F3DEX2: up to 7 lights + ambient (OoT uses them all; sm64-port's 2 made the
 * ambient index run off the array -> garbage-coloured lighting). */
#define MAX_LIGHTS 7
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
	short u, v; /* signed S10.5: a texrect can start left of / above texel 0 (SSB64 4843c2c) */
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
    uint16_t view_w, view_h; // the tile view imported (a LoadTile sub-rectangle or a scrolled
    uint32_t view_stride;    // block can share an address with a different view: keep both)
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
    Light_t current_lookat[2]; // gSPLookAt: [0] = LOOKATY, [1] = LOOKATX (F3DEX2 G_MV_LIGHT offsets 24, 0)
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

#define GFX_LT_SLOTS 4
static struct RDP {
    const uint8_t *palette; // the render tile's TLUT (gfx_rdp_palette_update): its 16-colour bank for CI4
    const uint8_t *tlut_last; // the last TLUT loaded
    const uint8_t *tlut_bank[16]; // TMEM's TLUT half by 16-colour bank (tmem 256 + 16 * bank): what each load put there
    uint8_t tile_palette; // the render tile's palette (G_SETTILE): a CI4 texel's colour is bank[palette][texel]
    struct {
        const uint8_t *addr;
        uint8_t siz;
        uint8_t tile_number;
        uint16_t width; // texels per row of the image (G_SETTIMG)
    } texture_to_load;
    uint16_t tile_tmem[8]; // TMEM address (64-bit words) of every tile descriptor
    // Texture loads by TMEM address (64-bit words). MK64 kept two fixed halves
    // (tmem / 256); OoT's sky loads its second texture at 0x80, which landed in
    // the first texture's half and overwrote it.
    struct {
        const uint8_t *addr;
        uint32_t size_bytes;
        uint32_t stride_bytes; // DRAM row pitch (0 = contiguous texels)
        uint16_t tmem;
        bool valid;
    } loaded_texture[GFX_LT_SLOTS];
    uint8_t lt_next; // round-robin victim
    struct {
        uint8_t fmt;
        uint8_t siz;
        uint8_t cms, cmt;         // effective (gfx_tile_cm): what the sampler does
        uint8_t cms_raw, cmt_raw; // as the display list set them
        uint8_t masks, maskt;     // wrap period 1 << mask texels (0 = none)
        uint16_t tmem; // TMEM address of the render tile (selects its loaded_texture[])
        uint16_t uls, ult, lrs, lrt; // U10.2
        uint32_t line_size_bytes;
    } texture_tile;
    bool textures_changed[2];
    // Every tile descriptor (OoT's two-texture combiners read tile 1 as TEXEL1).
    struct {
        uint8_t fmt, siz, cms, cmt, shifts, shiftt, masks, maskt, palette;
        uint16_t tmem;
        uint16_t uls, ult, lrs, lrt; // U10.2
        uint32_t line_size_bytes;
    } tiles[8];
    uint8_t cc1[8]; // cycle-1 muxes: rgb a b c d, alpha a b c d
    
    uint32_t other_mode_l, other_mode_h;
    uint32_t combine_mode;
    uint8_t cc2[8];          // cycle-2 combiner muxes (raw G_CCMUX/G_ACMUX): rgb a b c d, alpha a b c d
    bool cc2_pass;           // cycle 2 is a plain pass of COMBINED (the 1-cycle model already covers it)
    uint8_t prim_lod_frac;
    
    struct RGBA env_color, prim_color, fog_color, fill_color;
    struct XYWidthHeight viewport, scissor;
    bool viewport_or_scissor_changed;
    void *z_buf_address;
    void *color_image_address;
} rdp  __attribute__((aligned(4)));

static struct RenderingState {
    struct XYWidthHeight viewport, scissor;
    struct ShaderProgram *shader_program;
    struct TextureHashmapNode *textures[2];
    bool depth_test;
    bool depth_mask;
    bool decal_mode;
    bool alpha_blend;
    bool tex_add; // backend texture function is ADD (texel + vertex colour)
    bool force_in; // backend always modulates by the vertex colour (cycle 2 lives there)
    bool tex_lerp; struct RGBA lerp_k; // backend lerps K -> vertex colour by the texel
    bool lerp_v;      // OOT_SHADELERP: ... the other way, vertex colour (the shade) -> K
    bool fog_px; struct RGBA fog_c; int fog_a; // backend fogs per pixel: fog colour, constant alpha (gfx_fog_plan)
    bool edge_opaque; // TEX_EDGE without FORCE_BL: the soft backend writes it unblended
    bool own_edges;   // OOT_FILLRULE: blended / alpha-tested without AA_EN, each pixel to one triangle of an edge
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
    bool cc2;                              // 2-cycle with a real cycle 2: vertex colour evaluates both cycles
    bool alpha_255;                        // copy mode: alpha forced to 255
    bool needs_lod;
    uint8_t mirror;                        // the bound texture was imported doubled (see gfx_mirror_flags)
    bool use_fog;                          // cycle-1 blender is CLR_FOG * A_SHADE: lerp the colour to the fog colour
    bool fog_px;                           // software backend: the fog factor rides in the vertex alpha, fogged per pixel
    bool fog_raw;                          // ... the raw factor replaces the evaluated vertex alpha
    uint8_t fog_class;                     // census: 0 folded, 1 per pixel, 2 per pixel with a constant alpha; +4 decal
    bool tex_add;                          // GE ADD: texel + const_color (see the tint combiner in gfx_tri_rebuild_state)
    struct RGBA const_color;
    float drop_u, drop_v;                  // the per-triangle rebase step, in tile-0 periods (see gfx_detail_drop)
} tri_state;

static uint32_t sRdpHalf1; /* G_RDPHALF_1's word: G_BRANCH_Z's target */
static size_t buf_vbo_len;
static size_t buf_num_vert;
static size_t buf_vbo_num_tris;
/* round 69: per buffered triangle, drawn twice (a G_TRI2 of one triangle twice; gfx_soft.c) */
static uint8_t buf_tri_twice[MAX_BUFFERED];
static bool gfx_tri_twice_next;
extern const uint8_t *gfx_soft_tri_twice;
static bool gfx_blend_twice_on(void) {
    static int sOn = -1;
    if (sOn < 0) { const char *e = getenv("OOT_BLENDTWICE"); sOn = !(e && e[0] == '0'); }
    return sOn;
}

static struct GfxWindowManagerAPI *gfx_wapi;
static struct GfxRenderingAPI *gfx_rapi;

#if defined(TARGET_PSP)
#include <pspthreadman.h>
static unsigned long get_time(void) {
    return sceKernelGetSystemTimeWide();
}
#else
#include <time.h>
static unsigned long get_time(void) {
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
/* Exact side planes (x > w etc., homogeneous: exact for any sign of w). Kept
 * apart from the guard-band bits above, which gate the early reject BEFORE
 * the phase bookkeeping in gfx_sp_tri1; these reject after it. */
#define X_OUT_L (0x100)
#define X_OUT_R (0x200)
#define Y_OUT_B (0x400)
#define Y_OUT_T (0x800)
#define XY_OUT (X_OUT_L | X_OUT_R | Y_OUT_B | Y_OUT_T)
/* Beyond the RSP's guard band, |x| or |y| > ratio * |w| (gSPClipRatio): sends the triangle to
 * the clipper (R33). One bit for all four sides, so never part of a shared-flag reject. */
#define GB_OUT (0x1000)

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
#define FAR_GUARD  2.0f
#define W_NEAR 24.0f

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
	{  0.f,  0.f, -1.f, -0.97f }	// z >= -0.97 w: the near plane, pulled in a little so the GE never sees
	                            // a vertex right on its own near plane (it handles those badly)
};
/* (NDCPlane[0], labelled "near", is the FAR plane z <= w; [5] is the near plane.)
 * OOT_NEARCLIP (default on; =0 the kit's old behaviour, for A/B) -- SM64 5cd6699,
 * the cross-port lead's item 3: clip at the game's exact near plane z = -w (the
 * RSP's), fan the polygon the LAST clip pass produced (the kit fanned the one
 * before it, so its near pass was lost), reject past the far plane at z > w
 * (z > 2|w| never fired), trigger the clipper by those flags (the w < 2 / w < 24
 * thresholds let triangles between the eye and OoT's near plane of 10 through),
 * and map z/w (-1..1) onto the 0..1 depth range (z/w passed straight through put
 * everything within ~20 units of the camera at depth 0). */
static const float NDCPlaneNear[4] = { 0.f, 0.f, -1.f, -1.f };
bool gfx_depth_full_range(void);
static int sNearClip = -1;
static inline bool gfx_nearclip_on(void) {
    if (sNearClip < 0) {
        const char *e = getenv("OOT_NEARCLIP");
        sNearClip = !(e != NULL && e[0] == '0');
    }
    return sNearClip != 0;
}
bool gfx_depth_full_range(void) { return gfx_nearclip_on(); }

/* OOT_NON (default on; =0 the exact near clip above, for A/B): OoT's microcode is F3DZEX2 NoN,
 * "no near clipping". The RSP never cuts at the near plane z = -w: only the guard-band side
 * planes (|x|,|y| <= ratio * w, which also remove everything behind the eye) and the far
 * plane; whatever lies between the eye and the near plane is drawn and the RDP clamps its
 * depth to the nearest value. The Redead grave's exit light (a yellow box over the hole,
 * the camera 7.5 units behind its face, near = 10) washes the whole view on the N64 and was
 * cut away here. Needs the exact near clip's plumbing, so only with OOT_NEARCLIP on. */
static int sNoN = -1;
static inline bool gfx_non_on(void) {
    if (sNoN < 0) {
        const char *e = getenv("OOT_NON");
        sNoN = !(e != NULL && e[0] == '0');
    }
    return sNoN != 0 && gfx_nearclip_on();
}

/* The side planes (the lead's R33, DKR 467ad1e: the floor flashed under the kart). The shade is
 * interpolated on screen, so where a triangle is cut changes its shading. The console cuts where
 * a plane sends a triangle to the clipper: the near and far planes and the guard band, |x|,|y| =
 * ratio * w from gSPClipRatio (OoT: FRUSTRATIO_2 in z_rcp.c's setup, 1 in z_room.c and for some
 * of Link's parts). The kit sent a triangle only for its near or far plane, then also cut it at
 * the exact screen edges: the moment a big foreground triangle's vertex crossed the near plane,
 * the cut at the edges began at once and its whole shading jumped. Now a vertex beyond the guard
 * band sends its triangle (GB_OUT) and the clipper cuts there, never at the screen edges (the
 * rasterizer's box clamps to the screen). OOT_CLIPEDGES=1: the kit's screen edges (A/B). */
static int gfx_clip_ratio = 2; /* the RSP's default; gSPClipRatio's G_MW_CLIP words set it */
static int sClipEdges = -1;
static inline int gfx_clipedges(void) {
    if (sClipEdges < 0) {
        const char *e = getenv("OOT_CLIPEDGES");
        sClipEdges = (e != NULL && e[0] == '1') ? 1 : 2;
    }
    return sClipEdges;
}

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
/* OOT_NEARW (default on; gfx_soft.c, with the depth path it needs; 2026-10-04, goldeneye-007 c20b9ee): with NoN
 * (OOT_NON, the default) the N64's only plane on the eye's side is w >= 0 (the guard band's |x|, |y| <= ratio * w),
 * kept here as w >= 1/64 -- the sliver nearer than 1/64 of a unit is the only difference, and the guard band keeps the
 * projection of anything near w = 0 inside +-ratio screens. 1/64 bounds 1/w at 64: a perspective-shaded colour times
 * 1/w stays inside 16.16 (255 * 64 * 65536 < 2^31). The kit's 2.0 (MK64's units) cut whatever came within 2 units of
 * the eye, which the N64 draws (round 49's NoN kept it). OOT_NEARW=0, or the near plane on (OOT_NON=0 or
 * OOT_NEARCLIP=0): 2.0, as before. */
extern int gfx_soft_nearw_on(void);
static int sNearWpc SAVESTATE_EXCLUDE = -1;
static inline float gfx_wmin(void) {
    if (sNearWpc < 0) sNearWpc = gfx_soft_nearw_on();
    return sNearWpc && gfx_non_on() ? 1.0f / 64.0f : 2.0f;
}
#define W_MIN gfx_wmin()
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

uint32_t clip_to_frustum( struct LoadedVertex * v0, struct LoadedVertex * v1, uint32_t vIn )
{
	uint32_t vOut;

	vOut = vIn;

	// Near plane (w-based) first: removes behind-camera parts so the NDC-plane
	// clips below see only w>0 vertices.  Bounces v0<->v1 like the others.
	vOut = clipToNearW( v1, v0, vOut );
	{ struct LoadedVertex *t = v0; v0 = v1; v1 = t; }

	if (gfx_nearclip_on() && gfx_clipedges() == 2) { /* the guard band (R33); still four passes */
		const float r = (float) gfx_clip_ratio;
		const float gp[4][4] = { { 1.f, 0.f, 0.f, -r }, { -1.f, 0.f, 0.f, -r }, { 0.f, 1.f, 0.f, -r }, { 0.f, -1.f, 0.f, -r } };
		vOut = clipToHyperPlane( v1, v0, vOut, gp[0] );
		vOut = clipToHyperPlane( v0, v1, vOut, gp[1] );
		vOut = clipToHyperPlane( v1, v0, vOut, gp[2] );
		vOut = clipToHyperPlane( v0, v1, vOut, gp[3] );
	} else {
	vOut = clipToHyperPlane( v1, v0, vOut, NDCPlane[2] );		// right
	vOut = clipToHyperPlane( v0, v1, vOut, NDCPlane[1] );		// left
	vOut = clipToHyperPlane( v1, v0, vOut, NDCPlane[4] );		// top
	vOut = clipToHyperPlane( v0, v1, vOut, NDCPlane[3] );		// bottom
	}
	vOut = clipToHyperPlane( v1, v0, vOut, NDCPlane[0] );		// far (z <= w; the kit's label said near)
	if (!gfx_non_on()) /* NoN: no near pass (the w clip above keeps the eye's side) */
	vOut = clipToHyperPlane( v0, v1, vOut, gfx_nearclip_on() ? NDCPlaneNear : NDCPlane[5] );	// near

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

    uint32_t out = clip_to_frustum( temp_a, temp_b, 3 );
    /* The passes bounce temp_a <-> temp_b and the last (near) one lands in
     * temp_b; the kit fanned temp_a, the polygon before it, with the last
     * pass's vertex count (OOT_NEARCLIP). */
    const struct LoadedVertex *poly = gfx_nearclip_on() && !gfx_non_on() ? temp_b : temp_a;
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
/* OoT UI (HUD, pause screen, file select), bracketed by HH_TAG_UI_* tags the
 * game emits: its perspective views (the A button, the pause pages) map like
 * the 2D HUD -- the 320-wide layout squeezed uniformly onto the screen, not the
 * world's aspect-correct crop -- and are never bilinear-filtered. */
static bool gfx_ui_mode;
/* The HUD unsquish (v37, the user's pick from the 2026-09-27 preview sheet): between the game's
 * HH_HUD_EL(id, cx) tags an element's texrects are drawn around the centre the 320->240 squeeze gives
 * it (cx * 0.75) at 1:1, instead of squeezed to 0.75 in x -- MK64's rule (39258d3), one centre per
 * element so rows of digits and letters keep their spacing. Default: 2 rupees+keys, 4 minimap and
 * 6 the area title at 1:1 (7 the ammo counts and 8 the action labels since round 56, the user's ask), 5 the message box (item-get included) at 0.92 -- 256 of the 320 layout wide,
 * so 1:1 would be wider than the screen; 0.92 fits it, its text ~23% larger than the squeeze. The
 * hearts, magic meter and B/C buttons (ids 1, 3) stay squeezed: at 1:1 the B button lands on the last
 * hearts; the A button and its label are a perspective quad and always squeezed. OOT_HUD=0: all
 * squeezed (the old HUD); OOT_HUD=1 or all: every element; OOT_HUD=<ids>, e.g. 2456: those. */
static int gfx_hud_el_cx SAVESTATE_EXCLUDE = -1; /* the open element's centre (320 layout), -1 none */
/* The B/C buttons' group (element 3, cx = the A button's centre): each texrect's centre, and the centres of
 * the ammo counts and B's label inside it (7, 8), sit at A + (x - A) x the squeeze x OOT_HUD_BTNSPREAD
 * (default 1.15) -- wider gaps between B, A and the C buttons so A's 1:1 label (8) overlaps them less
 * (the user, 2026-09-29); sizes stay squeezed. 1 = the plain squeeze. -1 when no group is open. */
static int gfx_hud_group_ax SAVESTATE_EXCLUDE = -1;
/* C-left's centre (tag id 15, inside the group): everything right of A moves as one block by C-left's move,
 * so the C buttons keep their own spacing (the user's pick: "1.15 but don't space out yellow buttons") */
static int gfx_hud_group_cref SAVESTATE_EXCLUDE = -1;
/* tag id 14 (paused, cx = B's left edge): START, left of B and only drawn when paused, keeps the plain
 * squeeze -- spread, its "Return" reached the tenth heart. B, A and C keep the play spacing: the pause
 * background is a snapshot of the last play frame, and a different layout showed both. -1 none. */
static int gfx_hud_group_leftcut SAVESTATE_EXCLUDE = -1;
static float gfx_hud_btn_spread(void) {
    static float m = -1.0f;
    if (m < 0.0f) { const char *e = getenv("OOT_HUD_BTNSPREAD"); m = e ? (float) atof(e) : 1.15f; if (!(m >= 1.0f && m <= 1.34f)) m = 1.15f; }
    return m;
}
/* x (320 layout) -> output pixels: the squeeze, spread about the open group's anchor */
static float gfx_hud_group_x(float x, float sx) {
    const float ax = (float) gfx_hud_group_ax, m = gfx_hud_btn_spread();
    if (gfx_hud_group_ax < 0 || (gfx_hud_group_leftcut >= 0 && x < (float) gfx_hud_group_leftcut)) return x * sx;
    if (x > ax && gfx_hud_group_cref >= 0) return (x + ((float) gfx_hud_group_cref - ax) * (m - 1.0f)) * sx;
    return (ax + (x - ax) * m) * sx;
}
static float gfx_hud_el_k SAVESTATE_EXCLUDE = 1.0f; /* its x scale against y: 1 = 1:1 */
/* The message box's x scale against y (id 5, 256 of the 320 layout wide): 0.92 fits it on the 240 screen
 * with a margin (1:1 would be 256 px; 0.75 is the squeeze). OOT_HUD_MSGSCALE=<k> for A/B. */
static float gfx_hud_msg_k(void) {
    static float k = -1.0f;
    if (k < 0.0f) { const char *e = getenv("OOT_HUD_MSGSCALE"); k = e ? (float) atof(e) : 0.92f; if (!(k > 0.5f && k <= 1.0f)) k = 0.92f; }
    return k;
}
/* OOT_MSGBOX1TO1 (default on; 0 = the 0.92 layout above, the user's pick of 2026-10-04, "option E"): the message
 * box's text at the N64's size. Rects wider than 40 px (the box, the backgrounds, the ocarina staff) at 240/256 about
 * the screen's centre -- the 256-wide box fills the 240 screen; the small ones (glyphs and their shadows, icons, the
 * choice arrow, button glyphs, the next/end marker, the ocarina notes) drawn 1:1 wide around their centre, which sits
 * at 1:1 from the centre, so the text has the N64's letter shapes and spacing. A box with an item icon or the ocarina
 * staff (HH_HUD_MSG_COMPACT: its lines run to the box's edge, 287 of 290 at the widest) places them at 240/256 like
 * the box; the credits (HH_HUD_MSG_CREDITS, 32..280 wide) at 0.92. A glyph keeps exactly 1 px to its drop shadow.
 * Every English line fits the 240 screen (BUILDLOG 2026-10-04). */
#ifndef HH_HUD_MSG_COMPACT
#define HH_HUD_MSG_COMPACT 0x800 /* as hh_compat.h */
#define HH_HUD_MSG_CREDITS 0x400
#endif
static int gfx_hud_msg_mode SAVESTATE_EXCLUDE = -1; /* -1 off or not the message box; 0 1:1 text; 1 compact; 2 credits */
static int32_t sMsgShUlx SAVESTATE_EXCLUDE, sMsgShUly SAVESTATE_EXCLUDE, sMsgShW SAVESTATE_EXCLUDE, sMsgShH SAVESTATE_EXCLUDE;
static float sMsgShOut SAVESTATE_EXCLUDE;
static bool gfx_msgbox_1to1(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("OOT_MSGBOX1TO1"); on = !(e && e[0] == '0'); }
    return on != 0;
}
static unsigned gfx_hud_el_mask SAVESTATE_EXCLUDE = 0xFFFFFFFFu; /* bit id: enabled; resolved lazily */
static bool gfx_hud_el_enabled(int id) {
    if (gfx_hud_el_mask == 0xFFFFFFFFu) {
        const char *e = getenv("OOT_HUD");
        gfx_hud_el_mask = e ? 0u : (1u << 2) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9); /* the user's pick */
        if (e && (!strcmp(e, "1") || !strcmp(e, "all"))) gfx_hud_el_mask = 0x3FEu;
        else if (e) for (; *e; e++) if (*e >= '1' && *e <= '9') gfx_hud_el_mask |= 1u << (*e - '0');
    }
    return (gfx_hud_el_mask >> id) & 1u;
}
/* HH_HUD_KX (hh_compat.h): the x scale that undoes the squeeze for element id's model-space quads. */
float gfx_hud_el_kx(int id) {
    const float sx = (float) gfx_current_dimensions.width / (2.0f * HALF_SCREEN_WIDTH);
    const float sy = (float) gfx_current_dimensions.height / (2.0f * HALF_SCREEN_HEIGHT);
    return (gfx_hud_el_enabled(id) && sx > 0.0f && sx < sy) ? sy / sx : 1.0f;
}
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
        gfx_soft_tri_twice = buf_tri_twice; /* round 69: which of these to draw twice */
        { /* round 74: per-pixel fog from depth (gfx_soft.c raster_tri); SoftVtx z is z_ndc * 0.5 + 0.5 with the near clip */
            static int sFogZOn = -1;
            extern void gfx_soft_set_fogz(bool on, float a, float b);
            if (sFogZOn < 0) { const char *e = getenv("OOT_FOGZ"); sFogZOn = e && e[0] == '1'; } /* off: see BUILDLOG round 74 */
            const float m = rsp.fog_mul, o = rsp.fog_offset;
            if (gfx_nearclip_on()) gfx_soft_set_fogz(sFogZOn, 2.0f * m, o - m);
            else gfx_soft_set_fogz(sFogZOn, m, o);
        }
#ifdef PORT_PROFILE_DL
        { uint32_t _f0 = port_time_us(); gfx_rapi->draw_triangles((float *)buf_vbo, buf_vbo_len, buf_vbo_num_tris); gfx_prof_flush += port_time_us() - _f0; }
#else
        gfx_rapi->draw_triangles((float *)buf_vbo, buf_vbo_len, buf_vbo_num_tris);
#endif
        gfx_soft_tri_twice = NULL;
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
 * wiring). Desktop tuning only: OOT_DRAWDIST=<units>, validated and parsed
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

/* Pre-blended two-texture combiners (gfx_blend_tiles): texture ids from the
 * same pool as the cache, so a cache reset drops them too. */
#define GFX_BLEND_CACHE 32
static struct {
    uint32_t key[16];
    uint32_t texture_id;
    uint32_t last_used_frame;
    bool valid;
} gfx_blend_cache[GFX_BLEND_CACHE];

/* The rasterizer's tile-0 texture as queued triangles will sample it (gfx_select_tex0), and
 * its detail layer (gfx_detail_set). A texture cache reset recycles ids: both are forgotten
 * (texman_clear turns the rasterizer's detail layer off to match). */
static uint32_t gfx_tex0_bound SAVESTATE_EXCLUDE;
static struct { uint32_t id; int es, et; int32_t offs, offt; uint16_t w[4]; int mul, clamp; } gfx_detail_cur SAVESTATE_EXCLUDE;

/* Bumped by every texture cache reset. A reset wipes every texture, including the one the OTHER tile
 * of the draw being set up already holds: importing tile 1 could reset the cache and leave
 * rendering_state.textures[0] on a zeroed pool node (id 0), and the draw went out untextured -- a flat
 * block of the combiner's colour (the user's v35 photo: the sunset as a red bar, after a long session
 * had filled the arena). The import loops run again when it changes under them. */
static uint32_t gfx_cache_epoch SAVESTATE_EXCLUDE;
static int gfx_epochfix SAVESTATE_EXCLUDE = -1; /* OOT_EPOCHFIX=0: the old behaviour (A/B) */
static bool gfx_epochfix_on(void) {
    if (gfx_epochfix < 0) { const char *e = getenv("OOT_EPOCHFIX"); gfx_epochfix = !(e && e[0] == '0'); }
    return gfx_epochfix != 0;
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
    gfx_tex0_bound = 0;
    memset(&gfx_detail_cur, 0, sizeof(gfx_detail_cur));
    memset(gfx_blend_cache, 0, sizeof(gfx_blend_cache));
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

/* The TLUT's type (G_SETOTHERMODE_H TEXTLUT): IA16 for GfxPrint's font and the VisMono effect */
static inline bool gfx_tlut_ia16(void) {
    return (rdp.other_mode_h & (3U << G_MDSFT_TEXTLUT)) == G_TT_IA16;
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

#ifndef HANDHELD
/* The cross-port lead's texture rules R24-R28, counted (desktop census, OOT_CCCENSUS=1). */
static uint32_t sCenR24Tris, sCenR25Tris, sCenR26Tris, sCenR27Tris, sCenR27Loads, sCenR28Sc, sCenR28Inv, sCenR28Tris;
static bool sCenR26Dirty; /* the render tile's corners were last written by a LoadTile of another tile */
static bool sCenR28Now;   /* the current scissor is inverted */
static void cen_r27_overlap(int slot) { /* a load that overwrites part of another recorded load */
    const uint32_t lo = rdp.loaded_texture[slot].tmem, hi = lo + (rdp.loaded_texture[slot].size_bytes + 7) / 8;
    for (int j = 0; j < GFX_LT_SLOTS; j++) {
        if (j == slot || !rdp.loaded_texture[j].valid) continue;
        const uint32_t l2 = rdp.loaded_texture[j].tmem, h2 = l2 + (rdp.loaded_texture[j].size_bytes + 7) / 8;
        if (l2 < hi && lo < h2) { sCenR27Loads++; return; }
    }
}
#endif

/* The loaded_texture[] slot holding TMEM address `tmem`: the load at that
 * address, else the load whose extent covers it, else slot 0. */
static int gfx_lt_find(uint16_t tmem, bool exact_only) {
    int i;
    for (i = 0; i < GFX_LT_SLOTS; i++) {
        if (rdp.loaded_texture[i].valid && rdp.loaded_texture[i].tmem == tmem) return i;
    }
    if (exact_only) return -1;
    for (i = 0; i < GFX_LT_SLOTS; i++) {
        const uint32_t lo = rdp.loaded_texture[i].tmem, hi = lo + (rdp.loaded_texture[i].size_bytes + 7) / 8;
        if (rdp.loaded_texture[i].valid && tmem > lo && tmem < hi) return i;
    }
    return 0;
}

/* The slot a load to `tmem` writes: the same address again, or a slot neither
 * texture unit is reading. */
static int gfx_lt_slot_for_load(uint16_t tmem) {
    int i = gfx_lt_find(tmem, true), n;
    if (i >= 0) return i;
    for (i = 0; i < GFX_LT_SLOTS; i++) {
        if (!rdp.loaded_texture[i].valid) goto found;
    }
    for (n = 0; n < GFX_LT_SLOTS; n++) {
        i = (rdp.lt_next + n) % GFX_LT_SLOTS;
        if (rdp.loaded_texture[i].tmem != rdp.texture_tile.tmem && rdp.loaded_texture[i].tmem != rdp.tiles[1].tmem) break;
    }
    if (n == GFX_LT_SLOTS) i = rdp.lt_next % GFX_LT_SLOTS;
    rdp.lt_next = (uint8_t) ((i + 1) % GFX_LT_SLOTS);
found:
    rdp.loaded_texture[i].tmem = tmem;
    rdp.loaded_texture[i].valid = true;
    return i;
}

/* The loaded_texture[] slot a texture unit reads: unit 0 is the render tile,
 * unit 1 is tile 1 (TEXEL1). */
static inline int gfx_tex_slot(int tile) {
    return gfx_lt_find(tile == 0 ? rdp.texture_tile.tmem : rdp.tiles[1].tmem, false);
}

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
/* The RDP masks texel coordinates to 1 << mask (wrap or mirror, after any
 * clamp to the tile): a tile never reads past one period of its mask, however
 * large it is. OoT's adult Link sizes the gauntlet tile 128 texels wide over a
 * 16-wide texture (mask 4) to repeat it round the forearm, and his sleeve and
 * tights tiles the same way; importing the whole tile read the bytes after the
 * texture as texels -- speckled gauntlets, blotched tights. */
/* Tile shifts. The RDP scales S/T by the tile's shift before subtracting its corner:
 * 1..10 = >> n, 11..15 = << 16 - n. The render tile's shift goes into the UV scale
 * (tri_state.u_scale); the pre-blends build in render-tile texels, so they sample tile
 * 1 at its shift RELATIVE to the render tile's.
 * OFF by default for now (v24, the user: the ground looked too big and flat). The base
 * layer comes out at the console's size; a tile 1 much finer than tile 0 (x16 on the field
 * ground) collapsed in the pre-blend, and the detail layer (gfx_detail_tiles, v25) now
 * draws it per pixel. Still off in v25: the rock wall's two-scale lit layer comes out darker
 * than z64, and a clamped tile 1 and the TPE combiners still collapse (BUILDLOG round 26).
 * ON by default since round 31 (2026-09-26): the user chose the grain after the 1:1 sheet ("i do
 * like how much crisper the textures look in the ON screenshots"), and the device cost is about
 * none with NEON on (49/53 windows at 20 fps). OOT_TILESHIFT=0 (debug.cfg tileshift=0): off, A/B. */
static int gfx_tileshift SAVESTATE_EXCLUDE = -1;
static inline bool gfx_tileshift_on(void) {
    if (gfx_tileshift < 0) { const char *e = getenv("OOT_TILESHIFT"); gfx_tileshift = !(e && e[0] == '0'); }
    return gfx_tileshift != 0;
}
static inline int gfx_shift_exp(uint8_t sh) { /* the power of two a shift scales by */
    return (sh == 0 || sh > 15) ? 0 : sh <= 10 ? -(int) sh : 16 - (int) sh;
}
static inline uint8_t gfx_tile1_shift(bool t) { /* tile 1's shift for the pre-blends */
    const uint8_t sh1 = t ? rdp.tiles[1].shiftt : rdp.tiles[1].shifts;
    if (!gfx_tileshift_on()) return sh1;
    int e = gfx_shift_exp(sh1) - gfx_shift_exp(t ? rdp.tiles[0].shiftt : rdp.tiles[0].shifts);
    if (e < -10) e = -10;
    if (e > 5) e = 5;
    return (uint8_t) (e == 0 ? 0 : e < 0 ? -e : 16 - e);
}

static int gfx_tilemask SAVESTATE_EXCLUDE = -1; /* OOT_TILEMASK=0: the old whole-tile import (A/B) */
static inline bool gfx_tilemask_on(void) {
    if (gfx_tilemask < 0) { const char *e = getenv("OOT_TILEMASK"); gfx_tilemask = !(e && e[0] == '0'); }
    return gfx_tilemask != 0;
}
static inline uint32_t gfx_mask_period(uint32_t n, uint8_t mask) {
    return (mask != 0 && mask < 16 && n > (1u << mask) && gfx_tilemask_on()) ? (1u << mask) : n;
}
/* The sampler's wrap for one axis: a clamped tile wider than its mask period
 * repeats inside the tile (the RDP clamps to the tile, then masks), so it wraps. */
static inline uint8_t gfx_tile_cm(uint8_t cm, uint8_t mask, uint16_t lo, uint16_t hi) {
    const int extent = ((int) hi - (int) lo + 4) / 4;
    if (mask != 0 && mask < 16 && (cm & G_TX_CLAMP) && extent > (1 << mask) && gfx_tilemask_on()) cm &= (uint8_t) ~G_TX_CLAMP;
    return cm;
}
static inline void gfx_render_tile_cm(void) {
    rdp.texture_tile.cms = gfx_tile_cm(rdp.texture_tile.cms_raw, rdp.texture_tile.masks, rdp.texture_tile.uls, rdp.texture_tile.lrs);
    rdp.texture_tile.cmt = gfx_tile_cm(rdp.texture_tile.cmt_raw, rdp.texture_tile.maskt, rdp.texture_tile.ult, rdp.texture_tile.lrt);
}

static inline void gfx_tile_extent(int tile, uint32_t *width, uint32_t *height);
static inline void gfx_tile_size(int tile, uint32_t *width, uint32_t *height) {
    gfx_tile_extent(tile, width, height);
    *width = gfx_mask_period(*width, rdp.texture_tile.masks);
    *height = gfx_mask_period(*height, rdp.texture_tile.maskt);
}

/* The render tile corner to corner, before the mask. */
static inline void gfx_tile_extent(int tile, uint32_t *width, uint32_t *height) {
    if (rdp.loaded_texture[gfx_tex_slot(tile)].stride_bytes != 0) {
        // LoadTile: TMEM holds exactly the sub-rectangle (uls,ult)-(lrs,lrt) of
        // a larger image (menu atlases, menu_items.c) and the corner is that
        // sub-rectangle's origin: size it corner to corner.
        int w = ((int) rdp.texture_tile.lrs - (int) rdp.texture_tile.uls + 4) / 4;
        int h = ((int) rdp.texture_tile.lrt - (int) rdp.texture_tile.ult + 4) / 4;
        *width = (uint32_t) (w < 1 ? 1 : w);
        *height = (uint32_t) (h < 1 ? 1 : h);
    } else {
        // LoadBlock: the whole texture is loaded and the corner is a scroll
        // offset. OoT's Gfx_TwoTexScroll moves BOTH corners (lrs = uls + (w-1)*4),
        // so the size is corner to corner; MK64's extent-from-origin rule made a
        // scrolling 32-wide texture 32 + uls/4 wide.
        int w = ((int) rdp.texture_tile.lrs - (int) rdp.texture_tile.uls + 4) / 4;
        int h = ((int) rdp.texture_tile.lrt - (int) rdp.texture_tile.ult + 4) / 4;
        /* A wrapping axis with a mask repeats every 2^mask texels whatever the tile's corners say: the corners are
         * only the scroll offset. MM's title logo flames (En_Mag) scroll a 32x32 flame through a 4x4 tile size
         * (gDPSetTileSize(1, s, t, s + 15, t + 15) in 10.2): imported 4x4, the flames drew as a grid of stripes. */
        if (rdp.texture_tile.masks != 0 && rdp.texture_tile.masks < 11 && !(rdp.texture_tile.cms_raw & G_TX_CLAMP) &&
            w < (1 << rdp.texture_tile.masks)) {
            w = 1 << rdp.texture_tile.masks;
        }
        if (rdp.texture_tile.maskt != 0 && rdp.texture_tile.maskt < 11 && !(rdp.texture_tile.cmt_raw & G_TX_CLAMP) &&
            h < (1 << rdp.texture_tile.maskt)) {
            h = 1 << rdp.texture_tile.maskt;
        }
        *width = (uint32_t) (w < 1 ? 1 : w);
        *height = (uint32_t) (h < 1 ? 1 : h);
    }
}

static uint32_t gfx_texture_content_hash(int tile, uint32_t fmt, uint32_t siz) {
    const uint8_t *src = rdp.loaded_texture[gfx_tex_slot(tile)].addr;
    uint32_t stride = rdp.loaded_texture[gfx_tex_slot(tile)].stride_bytes;
    uint32_t h = 2166136261u;
    if (src == NULL) {
        return 0;
    }
    // A source below real RAM is an unresolved/low segment address (e.g. a
    // course texture at seg 5 whose base was not mapped): reading it faults on
    // real hardware (the emulator tolerates its mapped low memory).  Skip.
    if ((uintptr_t) src < PORT_REAL_PTR_MIN) {
        return 0;
    }
    // Texels inside the executable's text/data (ROM blobs, torch assets) never
    // change; only buffers in BSS / the game's pools need hashing.  A CI
    // texture still hashes a palette that lives in writable memory.
    if (gfx_is_static_memory(src)) {
        if (fmt == G_IM_FMT_CI && rdp.palette != NULL) {
            if (!gfx_is_static_memory(rdp.palette)) {
                return hash_bytes(h, rdp.palette, siz == G_IM_SIZ_4b ? 32 : 512) ^ (gfx_tlut_ia16() ? 0x1A160000u : 0u);
            }
            // Static texels with a static palette: the palette's ADDRESS still
            // has to take part (Rainbow Road animates by cycling through
            // several TLUTs over the same CI texels).
            return ((uint32_t) (uintptr_t) rdp.palette | 1u) ^ (gfx_tlut_ia16() ? 2u : 0u);
        }
        return 1;
    }
    if (stride == 0) {
        uint32_t size = rdp.loaded_texture[gfx_tex_slot(tile)].size_bytes;
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
    if (fmt == G_IM_FMT_CI && rdp.palette != NULL) {
        h = hash_bytes(h, rdp.palette, siz == G_IM_SIZ_4b ? 32 : 512) ^ (gfx_tlut_ia16() ? 0x1A160000u : 0u);
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

/* OOT_TEXSTRESS=<n> (debug): reset the texture cache every n new imports, as a long session's full
 * arena does -- to test that draws survive a reset under their own imports (gfx_cache_epoch). */
static bool gfx_tex_stress(void) {
    static int sEvery = -1, sCount;
    static bool sJustReset;
    if (sEvery < 0) { const char *e = getenv("OOT_TEXSTRESS"); sEvery = e ? atoi(e) : 0; }
    /* Never two in a row: a reset empties the arena, so the import after one always fits (1 = every
     * other import). Resetting on every import could never leave a two-tile draw with both. */
    if (sEvery <= 0) return false;
    if (sJustReset) { sJustReset = false; return false; }
    sJustReset = ++sCount % sEvery == 0;
    return sJustReset;
}

static bool gfx_sampfix_on(void) {
    static int sOn = -1;
    if (sOn < 0) { const char *e = getenv("OOT_SAMPFIX"); sOn = !(e && e[0] == '0'); }
    return sOn != 0;
}

static bool gfx_texture_cache_lookup(int tile, struct TextureHashmapNode **n, const uint8_t *orig_addr, uint32_t fmt, uint32_t siz, uint8_t mirror) {
    size_t hash = (uintptr_t)orig_addr;
    uint32_t content_hash = gfx_texture_content_hash(tile, fmt, siz);
    uint32_t upload_bytes = gfx_texture_upload_bytes(tile, fmt, siz, mirror);
    struct TextureHashmapNode *stale = NULL;
    uint32_t view_w, view_h, view_stride = rdp.loaded_texture[gfx_tex_slot(tile)].stride_bytes;
    gfx_tile_size(tile, &view_w, &view_h);
    hash = (hash >> 5) & 0x3ff;
    struct TextureHashmapNode **node = &gfx_texture_cache.hashmap[hash];
    while (*node != NULL && *node - gfx_texture_cache.pool < (int)gfx_texture_cache.pool_pos) {
        if ((*node)->texture_addr == orig_addr && (*node)->fmt == fmt && (*node)->siz == siz && (*node)->mirror == mirror &&
            (*node)->view_w == view_w && (*node)->view_h == view_h && (*node)->view_stride == view_stride) {
            if ((*node)->content_hash == content_hash) {
                if (gfx_debug_frame) port_log("  texhit %u addr %p fmt %u siz %u %ux%u hash %08X tile %d\n", (*node)->texture_id, orig_addr, fmt, siz, view_w, view_h, content_hash, tile);
                gfx_rapi->select_texture(tile, (*node)->texture_id);
                /* The hit's own tile: passing 0 for any tile made a tile-1 hit set tile 0's texture's
                 * wrap/clamp/filter (Paper Mario's 853ed45 find; OoT's census: Zora's Domain, 242 of
                 * 1,456 tile-1 hits had another sampler on tile 0). OOT_SAMPFIX=0: the old 0 (A/B). */
                gfx_rapi->set_sampler_parameters(gfx_sampfix_on() ? tile : 0, (*node)->linear_filter, (*node)->cms, (*node)->cmt);
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
    (*node)->view_w = (uint16_t) view_w;
    (*node)->view_h = (uint16_t) view_h;
    (*node)->view_stride = view_stride;
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
    const uint8_t* src = rdp.loaded_texture[gfx_tex_slot(tile)].addr;
    uint32_t stride = rdp.loaded_texture[gfx_tex_slot(tile)].stride_bytes;
    uint32_t x, y;

    if (width == 0 || height == 0) {
        return;
    }
    // Reject an unresolved/low segment source (see gfx_texture_content_hash):
    // reading it hard-faults on device.  Leave the previously-bound texture.
    if (src == NULL || (uintptr_t) src < PORT_REAL_PTR_MIN) {
        return;
    }
    if (stride == 0) {
        /* LoadBlock copies DRAM into TMEM linearly and the RDP reads rows at
         * the render tile's line size (SetTile `line`, 64-bit words), not at
         * width * bpp: a texture narrower than its rows (padding) skews every
         * row otherwise. 32-bit texels split across the TMEM halves, so `line`
         * counts half a row. Never tighter than the width. (super-smash-bros
         * a55820b: Smash's HUD sprites 27-30 px wide in 32 px rows.) */
        stride = rdp.texture_tile.line_size_bytes;
        if (siz == G_IM_SIZ_32b) {
            stride *= 2;
        }
        if (stride == 0 || stride < tex_bytes_per_row(width, siz)) {
            stride = tex_bytes_per_row(width, siz);
        }
    }
    if (width * height > GFX_MAX_IMPORT_TEXELS) {
        SUPPORT_CHECK(width * height <= GFX_MAX_IMPORT_TEXELS);
        height = GFX_MAX_IMPORT_TEXELS / width;
    }
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
        const bool ia16 = gfx_tlut_ia16();
        if (gfx_debug_frame) port_log("  ci import palette %p tex %p idx0 %02X %02X %02X %02X\n", rdp.palette, src, src[0], src[1], src[2], src[3]);
        for (y = 0; y < height; y++) {
            const uint8_t* row = TEXEL_ROW(y);
            for (x = 0; x < width; x++) {
                uint8_t idx = (siz == G_IM_SIZ_4b) ? nib(row, x) : row[x];
                uint16_t e = be16(rdp.palette + idx * 2);
                if (ia16) { /* G_TT_IA16: the colour is intensity (high byte) and alpha (low byte) */
                    const uint16_t i5 = (uint16_t) (e >> 11);
                    e = (uint16_t) ((i5 << 11) | (i5 << 6) | (i5 << 1) | ((e & 0xFF) >= 0x80));
                }
                out16[y * width + x] = n64_rgba16_to_psp5551(e);
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

static void import_texture(int tile) {
    uint8_t fmt = rdp.texture_tile.fmt;
    uint8_t siz = rdp.texture_tile.siz;
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
    if (gfx_texture_cache_lookup(tile, &rendering_state.textures[tile], rdp.loaded_texture[gfx_tex_slot(tile)].addr, fmt, siz, mirror)) {
        return;
    }
    import_texture_any(tile, mirror);
}

/* Tile 1 (TEXEL1) through the tile-0 import path: swap its descriptor in. Only
 * when it reads a different TMEM load than tile 0 (OoT: tile 1 at 0x100 for
 * room materials, 0x80 for the sky). */
static bool gfx_tile1_importable(void) {
    int s1 = gfx_lt_find(rdp.tiles[1].tmem, false);
    return s1 != gfx_lt_find(rdp.texture_tile.tmem, false) && rdp.loaded_texture[s1].valid;
}

static void import_texture_tile1(void) {
    __typeof__(rdp.texture_tile) save = rdp.texture_tile;
    rdp.texture_tile.fmt = rdp.tiles[1].fmt;
    rdp.texture_tile.siz = rdp.tiles[1].siz;
    rdp.texture_tile.cms_raw = rdp.tiles[1].cms;
    rdp.texture_tile.cmt_raw = rdp.tiles[1].cmt;
    rdp.texture_tile.masks = rdp.tiles[1].masks;
    rdp.texture_tile.maskt = rdp.tiles[1].maskt;
    rdp.texture_tile.uls = rdp.tiles[1].uls;
    rdp.texture_tile.ult = rdp.tiles[1].ult;
    rdp.texture_tile.lrs = rdp.tiles[1].lrs;
    rdp.texture_tile.lrt = rdp.tiles[1].lrt;
    gfx_render_tile_cm();
    rdp.texture_tile.line_size_bytes = rdp.tiles[1].line_size_bytes;
    rdp.texture_tile.tmem = rdp.tiles[1].tmem;
    import_texture(1);
    rdp.texture_tile = save;
}

static bool gfx_is_pow2(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

/* The lerp factor in the rgb C slot: constant(s) into k[] (returns 0), or a
 * texel alpha (1 = tile 0, 2 = tile 1). -1 = not a per-draw constant.
 * LOD_FRACTION is taken as 0 (the near level, what the port drew before). */
static int gfx_blend_k_rgb(uint8_t c, uint8_t k[3]) {
    const uint8_t *prim = (const uint8_t *) &rdp.prim_color, *env = (const uint8_t *) &rdp.env_color;
    int ch;
    switch (c) {
        case G_CCMUX_TEXEL0_ALPHA: return 1;
        case G_CCMUX_TEXEL1_ALPHA: return 2;
        case G_CCMUX_PRIMITIVE: for (ch = 0; ch < 3; ch++) k[ch] = prim[ch]; return 0;
        case G_CCMUX_ENVIRONMENT: for (ch = 0; ch < 3; ch++) k[ch] = env[ch]; return 0;
        case G_CCMUX_PRIMITIVE_ALPHA: k[0] = k[1] = k[2] = rdp.prim_color.a; return 0;
        case G_CCMUX_ENV_ALPHA: k[0] = k[1] = k[2] = rdp.env_color.a; return 0;
        case G_CCMUX_PRIM_LOD_FRAC: k[0] = k[1] = k[2] = rdp.prim_lod_frac; return 0;
        case G_CCMUX_LOD_FRACTION: k[0] = k[1] = k[2] = 0; return 0;
        default: if (c >= 15) { k[0] = k[1] = k[2] = 0; return 0; } return -1;
    }
}

static int gfx_blend_k_alpha(uint8_t c, uint8_t *k) {
    switch (c) {
        case G_ACMUX_TEXEL0: return 1;
        case G_ACMUX_TEXEL1: return 2;
        case G_ACMUX_PRIMITIVE: *k = rdp.prim_color.a; return 0;
        case G_ACMUX_ENVIRONMENT: *k = rdp.env_color.a; return 0;
        case G_ACMUX_PRIM_LOD_FRAC: *k = rdp.prim_lod_frac; return 0;
        case G_ACMUX_LOD_FRACTION: case G_ACMUX_0: *k = 0; return 0;
        default: return -1;
    }
}

/* One slot set (a, b, c, d) -> 0 = tile 0 only, 1 = tile 1 only, 2 = t0 + (t1 - t0) * c,
 * 3 = t1 + (t0 - t1) * c, -1 = mixes the tiles in a form we cannot pre-blend. */
static int gfx_blend_mode(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t t0, uint8_t t1, uint8_t t0a, uint8_t t1a) {
    bool uses0 = a == t0 || b == t0 || c == t0 || d == t0 || (t0a != 0xff && c == t0a);
    bool uses1 = a == t1 || b == t1 || c == t1 || d == t1 || (t1a != 0xff && c == t1a);
    if (!uses1) return 0;
    if (a == t1 && b == t0 && d == t0) return 2;
    if (a == t0 && b == t1 && d == t1) return 3;
    if (!uses0) return 1;
    return -1;
}

/* Cycle 1 reads TEXEL1 anywhere (the compiled cc_id can hide it: a lerp by
 * ENV_ALPHA compiles to plain TEXEL0 because that C input has no slot). */
static bool gfx_cc1_uses_texel1(void) {
    const uint8_t *m = rdp.cc1;
    return m[0] == G_CCMUX_TEXEL1 || m[1] == G_CCMUX_TEXEL1 || m[2] == G_CCMUX_TEXEL1 || m[2] == G_CCMUX_TEXEL1_ALPHA ||
           m[3] == G_CCMUX_TEXEL1 || m[4] == G_ACMUX_TEXEL1 || m[5] == G_ACMUX_TEXEL1 || m[6] == G_ACMUX_TEXEL1 ||
           m[7] == G_ACMUX_TEXEL1;
}

extern bool gfx_soft_blend_textures(uint32_t dst, uint32_t id0, uint32_t id1, int W, int H, int base_s, int base_t,
                                    int uls1, int ult1, int shs1, int sht1, int rgb_mode, int a_mode,
                                    int krgb_src, const uint8_t krgb[3], int ka_src, uint8_t ka, bool filt, uint32_t qfrac);

/* Cycle 1 mixes TEXEL0 and TEXEL1 (OoT's room/water/effect combiners): bind a
 * cached pre-blend of both tiles as the one texture. Returns its id (0 = draw
 * tile 0 as before) and the blended texture size in *bw, *bh. */
static int sBlendMirror1 SAVESTATE_EXCLUDE = 1; /* OOT_BLENDMIRROR=0: refuse a mirrored tile 1 (A/B) */
static int sBlendFrac SAVESTATE_EXCLUDE = -1; /* OOT_BLENDFRAC=0: whole-texel corners in the bilinear bake (A/B) */
static uint32_t gfx_blend_tiles(uint32_t *bw, uint32_t *bh) {
    const uint8_t *m = rdp.cc1;
    struct TextureHashmapNode *n0 = rendering_state.textures[0], *n1 = rendering_state.textures[1];
    uint8_t krgb[3] = { 0, 0, 0 }, ka = 0;
    int rgb_mode, a_mode, krgb_src = 0, ka_src = 0, i, pick = -1;
    uint32_t w0, h0, w1, h1, W, H, key[16] = { 0 };
    int iw1, ih1;
    bool filt;
    uint32_t qfrac;

    if (sBlendFrac < 0) {
        const char *e = getenv("OOT_BLENDFRAC"), *m = getenv("OOT_BLENDMIRROR");
        sBlendFrac = !(e && e[0] == '0');
        sBlendMirror1 = !(m && m[0] == '0');
    }
    /* A mirrored tile 1 is fine: it was imported doubled with its mirror image (import_texture),
     * so wrapping over the doubled size mirrors it (its period doubles below). Refusing it drew
     * such draws as tile 0 alone: Lake Hylia's night water lerps fully to a still tile 1 over a
     * scrolling tile 0, and the port showed the scrolling layer (the user, v33: "the texture
     * moves"). */
    if (n0 == NULL || n1 == NULL || n0->mirror != 0 || !gfx_tile1_importable() ||
        (n1->mirror != 0 && !sBlendMirror1)) {
        if (gfx_debug_frame) port_log("  blend skip n0 %p n1 %p tmem %u/%u\n", (void *) n0, (void *) n1, rdp.texture_tile.tmem, rdp.tiles[1].tmem);
        return 0;
    }
    rgb_mode = gfx_blend_mode(m[0], m[1], m[2], m[3], G_CCMUX_TEXEL0, G_CCMUX_TEXEL1, G_CCMUX_TEXEL0_ALPHA, G_CCMUX_TEXEL1_ALPHA);
    a_mode = gfx_blend_mode(m[4], m[5], m[6], m[7], G_ACMUX_TEXEL0, G_ACMUX_TEXEL1, 0xff, 0xff);
    if (rgb_mode < 0 || a_mode < 0 || (rgb_mode == 0 && a_mode == 0)) {
        if (gfx_debug_frame) port_log("  blend no-form %d/%d cc1 %u %u %u %u / %u %u %u %u\n", rgb_mode, a_mode, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
        return 0;
    }
    if (rgb_mode >= 2 && (krgb_src = gfx_blend_k_rgb(m[2], krgb)) < 0) return 0;
    if (a_mode >= 2 && (ka_src = gfx_blend_k_alpha(m[6], &ka)) < 0) return 0;

    gfx_tile_size(0, &w0, &h0);
    iw1 = ((int) rdp.tiles[1].lrs - (int) rdp.tiles[1].uls + 4) / 4;
    ih1 = ((int) rdp.tiles[1].lrt - (int) rdp.tiles[1].ult + 4) / 4;
    w1 = gfx_mask_period((uint32_t) (iw1 < 1 ? 1 : iw1), rdp.tiles[1].masks) << (n1->mirror & 1);
    h1 = gfx_mask_period((uint32_t) (ih1 < 1 ? 1 : ih1), rdp.tiles[1].maskt) << ((n1->mirror >> 1) & 1);
    W = w0;
    H = h0;
    // Tile 1 repeats with its own period in tile-0 texels (its shift scales
    // it): cover both periods so the pre-blend tiles seamlessly.
    if (gfx_is_pow2(w0) && gfx_is_pow2(w1) && !(rdp.texture_tile.cms & G_TX_CLAMP)) {
        uint32_t sh = gfx_tile1_shift(false);
        uint32_t p = sh == 0 ? w1 : sh <= 10 ? w1 << sh : (w1 >> (16 - sh));
        if (p > W) W = p;
    }
    if (gfx_is_pow2(h0) && gfx_is_pow2(h1) && !(rdp.texture_tile.cmt & G_TX_CLAMP)) {
        uint32_t sh = gfx_tile1_shift(true);
        uint32_t p = sh == 0 ? h1 : sh <= 10 ? h1 << sh : (h1 >> (16 - sh));
        if (p > H) H = p;
    }
    if (W * H > 256 * 256) {
        return 0;
    }
    filt = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;

    key[0] = (uint32_t) (uintptr_t) n0;
    key[1] = n0->content_hash;
    key[2] = (uint32_t) (uintptr_t) n1;
    key[3] = n1->content_hash;
    key[4] = W | (H << 16);
    key[5] = (uint32_t) (rdp.texture_tile.uls >> 2) | ((uint32_t) (rdp.texture_tile.ult >> 2) << 16);
    key[6] = (uint32_t) (rdp.tiles[1].uls >> 2) | ((uint32_t) (rdp.tiles[1].ult >> 2) << 16);
    key[7] = gfx_tile1_shift(false) | (gfx_tile1_shift(true) << 8) | (rgb_mode << 16) | (a_mode << 20) | (krgb_src << 24) | (ka_src << 28);
    key[8] = krgb[0] | (krgb[1] << 8) | (krgb[2] << 16) | ((uint32_t) ka << 24);
    key[9] = n0->texture_id;
    key[10] = n1->texture_id;
    key[11] = 0x424c4e44;
    key[12] = filt; /* a coarser tile 1 is baked bilinear when the draw filters */
    /* ... at both corners' quarter texels (the only reader of them): tiles 0/1, S then T */
    qfrac = filt && sBlendFrac ? ((rdp.texture_tile.uls & 3) | ((rdp.texture_tile.ult & 3) << 2) | ((rdp.tiles[1].uls & 3) << 4) |
                    ((rdp.tiles[1].ult & 3) << 6)) : 0;
    key[13] = qfrac;

    for (i = 0; i < GFX_BLEND_CACHE; i++) {
        if (gfx_blend_cache[i].valid && memcmp(gfx_blend_cache[i].key, key, sizeof(key)) == 0) {
            gfx_blend_cache[i].last_used_frame = gfx_frame_counter;
            if (gfx_debug_frame) port_log("  blend hit %u %ux%u\n", gfx_blend_cache[i].texture_id, W, H);
            *bw = W;
            *bh = H;
            return gfx_blend_cache[i].texture_id;
        }
    }
    for (i = 0; i < GFX_BLEND_CACHE; i++) {
        if (!gfx_blend_cache[i].valid) { pick = i; break; }
        if (pick < 0 || gfx_blend_cache[i].last_used_frame < gfx_blend_cache[pick].last_used_frame) pick = i;
    }
    gfx_flush(); // queued triangles may still sample the entry being rewritten
    if (gfx_blend_cache[pick].texture_id == 0) {
        gfx_blend_cache[pick].texture_id = gfx_rapi->new_texture();
    }
    if (!gfx_soft_blend_textures(gfx_blend_cache[pick].texture_id, n0->texture_id, n1->texture_id, (int) W, (int) H,
                                 rdp.texture_tile.uls >> 2, rdp.texture_tile.ult >> 2, rdp.tiles[1].uls >> 2,
                                 rdp.tiles[1].ult >> 2, gfx_tile1_shift(false), gfx_tile1_shift(true), rgb_mode, a_mode,
                                 krgb_src, krgb, ka_src, ka, filt, qfrac)) {
        gfx_blend_cache[pick].valid = false;
        return 0;
    }
    memcpy(gfx_blend_cache[pick].key, key, sizeof(key));
    gfx_blend_cache[pick].valid = true;
    gfx_blend_cache[pick].last_used_frame = gfx_frame_counter;
    if (gfx_debug_frame) port_log("  blend tex %u %ux%u modes %d/%d k %d:%02X%02X%02X %d:%02X\n", gfx_blend_cache[pick].texture_id, W, H, rgb_mode, a_mode, krgb_src, krgb[0], krgb[1], krgb[2], ka_src, ka);
    *bw = W;
    *bh = H;
    return gfx_blend_cache[pick].texture_id;
}

/* The detail layer (with OOT_TILESHIFT=1; OOT_DETAIL=0 turns it off, for A/B). Cycle 1 lerps
 * tile 0 and tile 1 by a constant, and tile 1 repeats at a finer scale than tile 0 on some
 * axis, or reads tile 0's own TMEM at another scale or corner (so there's no second texture
 * to import). Hyrule Field's ground is the case: a base texture at shift 3 (8x magnified)
 * under a grass grain at shift 15 (2x minified), 16x finer. A pre-blend over tile 0's
 * texels holds one tile-1 texel per tile-0 texel, so the grain came out as one flat colour;
 * the rasterizer samples tile 1 per pixel instead (gfx_soft_set_detail). A coarser tile 1
 * stays in the pre-blend, which holds it at one fetch a pixel. */
extern bool gfx_soft_set_detail(uint32_t id1, int es, int et, int32_t offs, int32_t offt, const uint16_t w[4], int mul,
                                int clamps, int clampt, int sizes, int sizet);
static int sDetailMode SAVESTATE_EXCLUDE = -1;
/* Census (OOT_CCCENSUS=1): why the last state has no detail layer while its tile 1 is at
 * another scale than tile 0 (gfx_detail_cand); 0 = it has one. */
static int gfx_detail_why SAVESTATE_EXCLUDE;
static bool gfx_detail_cand SAVESTATE_EXCLUDE;

/* The rasterizer's detail layer for the triangles that follow (id1 = 0: off). Queued
 * triangles are drawn first when it changes. False if it's off. */
/* clamp: bit 0 tile 1 clamped in s, bit 1 in t (the multiply form only); sizes in tile-1 texels */
static bool gfx_detail_set(uint32_t id1, int es, int et, int32_t offs, int32_t offt, const uint16_t *w, int mul, int clamp,
                           int sizes, int sizet) {
    static const uint16_t zero[4];
    if (id1 == 0) {
        es = et = 0;
        offs = offt = 0;
        w = zero;
    }
    if (gfx_detail_cur.id == id1 && gfx_detail_cur.es == es && gfx_detail_cur.et == et && gfx_detail_cur.offs == offs &&
        gfx_detail_cur.offt == offt && memcmp(gfx_detail_cur.w, w, sizeof(gfx_detail_cur.w)) == 0 &&
        gfx_detail_cur.mul == mul && gfx_detail_cur.clamp == (clamp | (sizes << 2) | (sizet << 16))) {
        return id1 != 0;
    }
    gfx_flush();
    if (!gfx_soft_set_detail(id1, es, et, offs, offt, w, mul, clamp & 1, (clamp >> 1) & 1, sizes, sizet)) {
        memset(&gfx_detail_cur, 0, sizeof(gfx_detail_cur)); /* the rasterizer turned it off */
        return false;
    }
    gfx_detail_cur.id = id1;
    gfx_detail_cur.es = es;
    gfx_detail_cur.et = et;
    gfx_detail_cur.offs = offs;
    gfx_detail_cur.offt = offt;
    memcpy(gfx_detail_cur.w, w, sizeof(gfx_detail_cur.w));
    gfx_detail_cur.mul = mul;
    gfx_detail_cur.clamp = clamp | (sizes << 2) | (sizet << 16);
    return id1 != 0;
}

/* The render tile's texel coordinate s0 = S * 2^e0 - uls0 (+ 1/2 for bilinear, which the
 * rasterizer takes off again) gives tile 1's: s1 = S * 2^e1 - uls1 = (s0 + uls0) * 2^(e1 - e0)
 * - uls1. Returns the 16.16 offset of s1 against s0 * 2^e, modulo 2^32 (the rasterizer only
 * keeps it modulo tile 1's power-of-two period). Corners are 10.2: * 16384 is 16.16. */
static int32_t gfx_detail_offset(uint16_t lo0, uint16_t lo1, int e, int64_t half) {
    const int64_t a = (int64_t) lo0 * 16384 - half; /* a multiple of 2^14: exact under >> 10 */
    const int64_t o = (e >= 0 ? a * ((int64_t) 1 << e) : a / ((int64_t) 1 << -e)) - (int64_t) lo1 * 16384 + half;
    return (int32_t) (uint32_t) ((uint64_t) o & 0xffffffffu);
}

/* Cycle 1 is a detail-layer draw (above): set the rasterizer's detail layer. False = no
 * (the pre-blend as before); gfx_detail_why says why. */
/* Cycle 1's colour is TEXEL0 x TEXEL1 (no add): MM's painted walls (Clock Town: brick x mural), whose common period
 * is too big for a per-texel bake (gfx_tpe_tiles' 256x256) */
static bool gfx_cc1_tex_multiply(void) {
    const uint8_t *m = rdp.cc1;
    return ((m[0] == G_CCMUX_TEXEL1 && m[2] == G_CCMUX_TEXEL0) || (m[0] == G_CCMUX_TEXEL0 && m[2] == G_CCMUX_TEXEL1)) &&
           m[1] >= 8 && m[3] == 7;
}

static bool gfx_detail_tiles_mode(bool mul);
static bool gfx_detail_tiles(void) {
    return gfx_detail_tiles_mode(false);
}
static bool gfx_detail_tiles_mode(bool mul) {
    const uint8_t *m = rdp.cc1;
    struct TextureHashmapNode *n0 = rendering_state.textures[0], *n1;
    uint8_t krgb[3] = { 0, 0, 0 }, ka = 0, cms1, cmt1;
    uint16_t w[4];
    int rgb_mode, a_mode, es, et, ch;
    bool shared;
    int64_t half;

    if (sDetailMode < 0) {
        const char *e = getenv("OOT_DETAIL");
        sDetailMode = !(e != NULL && e[0] == '0');
    }
    gfx_detail_cand = false;
    gfx_detail_why = 1;
    if (!gfx_tileshift_on() || n0 == NULL) return false;
    es = gfx_shift_exp(rdp.tiles[1].shifts) - gfx_shift_exp(rdp.tiles[G_TX_RENDERTILE].shifts);
    et = gfx_shift_exp(rdp.tiles[1].shiftt) - gfx_shift_exp(rdp.tiles[G_TX_RENDERTILE].shiftt);
    shared = !gfx_tile1_importable() && gfx_lt_find(rdp.tiles[1].tmem, false) == gfx_lt_find(rdp.texture_tile.tmem, false);
    if (!mul && !shared && es <= 0 && et <= 0) return false; /* the pre-blend holds a tile 1 no finer than tile 0 */
    gfx_detail_cand = true;
    gfx_detail_why = 2;
    if (!sDetailMode) return false;
    gfx_detail_why = 3;
    rgb_mode = mul ? 1 : gfx_blend_mode(m[0], m[1], m[2], m[3], G_CCMUX_TEXEL0, G_CCMUX_TEXEL1, G_CCMUX_TEXEL0_ALPHA, G_CCMUX_TEXEL1_ALPHA);
    a_mode = gfx_blend_mode(m[4], m[5], m[6], m[7], G_ACMUX_TEXEL0, G_ACMUX_TEXEL1, 0xff, 0xff);
    if (rgb_mode < 0 || a_mode < 0 || (rgb_mode == 0 && a_mode == 0)) return false;
    gfx_detail_why = 4; /* a lerp factor that isn't a per-draw constant (a texel alpha) */
    if ((rgb_mode >= 2 && gfx_blend_k_rgb(m[2], krgb) != 0) || (a_mode >= 2 && gfx_blend_k_alpha(m[6], &ka) != 0)) {
        return false;
    }
    gfx_detail_why = 5;
    if (es < -10 || es > 8 || et < -10 || et > 8) return false;
    gfx_detail_why = 6; /* tile 1 must repeat: a mask, not clamped inside it */
    cms1 = gfx_tile_cm(rdp.tiles[1].cms, rdp.tiles[1].masks, rdp.tiles[1].uls, rdp.tiles[1].lrs);
    cmt1 = gfx_tile_cm(rdp.tiles[1].cmt, rdp.tiles[1].maskt, rdp.tiles[1].ult, rdp.tiles[1].lrt);
    /* the multiply form takes a clamped tile 1 too (MM's painted walls: the brick tile clamped at its 32x32 size) */
    int dclamp = 0;
    if (mul && rdp.tiles[1].masks != 0 && rdp.tiles[1].maskt != 0) {
        dclamp = ((cms1 & G_TX_CLAMP) ? 1 : 0) | ((cmt1 & G_TX_CLAMP) ? 2 : 0);
        cms1 &= ~G_TX_CLAMP;
        cmt1 &= ~G_TX_CLAMP;
    }
    if (rdp.tiles[1].masks == 0 || rdp.tiles[1].maskt == 0 || ((cms1 | cmt1) & G_TX_CLAMP)) return false;
    gfx_detail_why = 7;
    if (shared) {
        /* Tile 1 reads tile 0's texels if the format, the TMEM address, the row pitch and
         * the period match (the field's second ground material: one texture, two scales). */
        uint32_t w0, h0;
        const int iw1 = ((int) rdp.tiles[1].lrs - (int) rdp.tiles[1].uls + 4) / 4;
        const int ih1 = ((int) rdp.tiles[1].lrt - (int) rdp.tiles[1].ult + 4) / 4;
        gfx_tile_size(0, &w0, &h0);
        if (rdp.tiles[1].fmt != rdp.texture_tile.fmt || rdp.tiles[1].siz != rdp.texture_tile.siz ||
            rdp.tiles[1].tmem != rdp.texture_tile.tmem || rdp.tiles[1].line_size_bytes != rdp.texture_tile.line_size_bytes ||
            n0->mirror != 0 || ((cms1 | cmt1) & G_TX_MIRROR) ||
            gfx_mask_period((uint32_t) (iw1 < 1 ? 1 : iw1), rdp.tiles[1].masks) != w0 ||
            gfx_mask_period((uint32_t) (ih1 < 1 ? 1 : ih1), rdp.tiles[1].maskt) != h0) {
            return false;
        }
        n1 = n0;
    } else {
        /* A mirrored tile 1 was imported doubled, mirror image included (import_texture): it
         * repeats over twice the period, which the rasterizer's wrap takes as it is. */
        n1 = rendering_state.textures[1];
        if (!gfx_tile1_importable() || n1 == NULL) return false;
        gfx_detail_why = 10;
        if (((cms1 & G_TX_MIRROR) != 0) != ((n1->mirror & 1) != 0) || ((cmt1 & G_TX_MIRROR) != 0) != ((n1->mirror & 2) != 0)) {
            return false;
        }
        gfx_detail_why = 7;
    }
    for (ch = 0; ch < 3; ch++) { /* lanes B G R; krgb is r g b */
        const int k = krgb[2 - ch];
        w[ch] = (uint16_t) (rgb_mode == 0 ? 0 : rgb_mode == 1 ? 256 : rgb_mode == 2 ? k : 256 - k);
    }
    w[3] = (uint16_t) (a_mode == 0 ? 0 : a_mode == 1 ? 256 : a_mode == 2 ? ka : 256 - ka);
    half = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT ? 32768 : 0; /* tri_state.u_off's filt */
    gfx_detail_why = 8;
    if (!gfx_detail_set(n1->texture_id, es, et, gfx_detail_offset(rdp.texture_tile.uls, rdp.tiles[1].uls, es, half),
                        gfx_detail_offset(rdp.texture_tile.ult, rdp.tiles[1].ult, et, half), w, mul, dclamp,
                        ((int) rdp.tiles[1].lrs - (int) rdp.tiles[1].uls + 4) / 4,
                        ((int) rdp.tiles[1].lrt - (int) rdp.tiles[1].ult + 4) / 4)) {
        return false;
    }
    if (gfx_debug_frame) port_log("  detail tex %u (tile 0 %u%s) e %d/%d w %u %u %u %u\n", n1->texture_id, n0->texture_id, shared ? ", shared" : "", es, et, w[0], w[1], w[2], w[3]);
    gfx_detail_why = 0;
    return true;
}

/* The per-triangle rebase (gfx_emit_tri drops the whole tile-0 periods its three vertices share)
 * must not move the detail layer: tile 1's coordinate is tile 0's * 2^e + offset, modulo tile 1's
 * period P1, so a drop of k tile-0 periods moves tile 1 by k * W0 * 2^e texels -- invisible only
 * when that is a multiple of P1. The Deku Tree basement water (tile 1 coarser than tile 0 on one
 * axis) had k * W0 * 2^e = half a period: triangles with different k showed seams, and the layer
 * jumped whenever a triangle's k changed with the scroll (the user's device shots, 2026-09-27).
 * The step: the smallest power of two L with L * W0 * 2^e a multiple of P1. OOT_DETAILREBASE=0:
 * one period, as before. */
extern bool gfx_soft_tex_size(uint32_t id, int *w, int *h);
static float gfx_detail_drop(float w0, int e, int p1) {
    static int sOn SAVESTATE_EXCLUDE = -1;
    const uint32_t iw = (uint32_t) w0;
    int a = 0, lp = 0, k;
    if (sOn < 0) { const char *v = getenv("OOT_DETAILREBASE"); sOn = !(v && v[0] == '0'); }
    if (!sOn || iw == 0 || p1 <= 0) return 1.0f;
    while (!((iw >> a) & 1u)) a++;          /* W0 = odd * 2^a */
    while ((1 << lp) < p1) lp++;            /* P1 = 2^lp (gfx_soft_set_detail wants powers of two) */
    k = lp - a - e;
    return k <= 0 ? 1.0f : (float) (1u << (k > 20 ? 20 : k));
}

/* The rasterizer samples its tile-0 binding when the queue is drawn: triangles queued under
 * one binding are drawn before it changes. (A pre-blend cache hit used to rebind with
 * triangles of the previous blend still queued, when the shader stayed the same.) */
static void gfx_select_tex0(uint32_t id) {
    if (id != gfx_tex0_bound) {
        gfx_flush();
        gfx_tex0_bound = id;
    }
    gfx_rapi->select_texture(0, id);
}

/* Cycle 1 evaluated per texel (gfx_soft_eval_texels) when it reads only the
 * two tiles and constants and the cc_id model cannot hold it: an alpha or
 * PRIM_LOD_FRAC input in C drops to 0 there, and gfx_blend_tiles only knows
 * t0/t1 lerps. Torch flames ((T1 - PRIM) * PRIM_LOD_FRAC + T0, alpha
 * (T1 - 1) * PRIM_LOD_FRAC + T0), get-item and shop models, fire and light
 * effects drew as plain tile 0 (GoldenEye's kit review, 2026-09-24; OoT
 * census: ~75k of 19M triangles over 41 scenes). The triangle then draws the
 * result as TEXEL0: *rgb / *alpha say which halves of the cc_id to replace.
 * OOT_TPE=0 turns it off. */
extern bool gfx_soft_eval_texels(uint32_t dst, uint32_t id0, uint32_t id1, int W, int H, int base_s, int base_t,
                                 int uls1, int ult1, int shs1, int sht1, const uint8_t m[8], const uint8_t k[9],
                                 bool keep_alpha, bool filt1);
static int sTpeMode = -1;
static bool tpe_rgb_in(uint8_t mux, int slot) { /* reads only tiles / constants (or is a literal) */
    switch (slot) {
        case 0: return mux == G_CCMUX_TEXEL0 || mux == G_CCMUX_TEXEL1 || mux == G_CCMUX_PRIMITIVE ||
                       mux == G_CCMUX_ENVIRONMENT || mux == G_CCMUX_1 || mux >= 8;
        case 1: return mux == G_CCMUX_TEXEL0 || mux == G_CCMUX_TEXEL1 || mux == G_CCMUX_PRIMITIVE ||
                       mux == G_CCMUX_ENVIRONMENT || mux >= 8;
        case 2: return mux == G_CCMUX_TEXEL0 || mux == G_CCMUX_TEXEL1 || mux == G_CCMUX_PRIMITIVE ||
                       mux == G_CCMUX_ENVIRONMENT || mux == G_CCMUX_TEXEL0_ALPHA || mux == G_CCMUX_TEXEL1_ALPHA ||
                       mux == G_CCMUX_PRIMITIVE_ALPHA || mux == G_CCMUX_ENV_ALPHA || mux == G_CCMUX_PRIM_LOD_FRAC ||
                       mux >= 16;
        default: return mux != G_CCMUX_COMBINED && mux != G_CCMUX_SHADE;
    }
}
static bool tpe_a_in(uint8_t mux) { /* 0 = COMBINED (A/B/D) or LOD_FRACTION (C) */
    return mux != 0 && mux != G_ACMUX_SHADE;
}
static bool tpe_rgb_evaluable(const uint8_t *m) {
    const bool zero_c = m[2] >= 16;
    return tpe_rgb_in(m[3], 3) && (zero_c || m[0] == m[1] || (tpe_rgb_in(m[0], 0) && tpe_rgb_in(m[1], 1) && tpe_rgb_in(m[2], 2)));
}
static bool tpe_a_evaluable(const uint8_t *m) {
    return tpe_a_in(m[7]) && (m[6] == G_ACMUX_0 || m[4] == m[5] || (tpe_a_in(m[4]) && tpe_a_in(m[5]) && tpe_a_in(m[6])));
}
/* The cc_id (color_comb_component) drops these inputs. */
static bool tpe_rgb_lossy(const uint8_t *m) {
    const uint8_t c = m[2];
    if (c >= 16) return false;
    if (m[0] != m[1] && !(c <= G_CCMUX_ENVIRONMENT || c == G_CCMUX_TEXEL0_ALPHA || c == G_CCMUX_LOD_FRACTION)) return true;
    return m[0] == G_CCMUX_NOISE || m[1] == G_CCMUX_CENTER || m[1] == G_CCMUX_K4;
}
static bool tpe_a_lossy(const uint8_t *m) {
    if (m[6] == G_ACMUX_0) return false;
    return m[4] != m[5] && (m[6] == G_ACMUX_PRIM_LOD_FRAC || m[5] == G_ACMUX_1);
}
static bool tpe_uses_t1(const uint8_t *m, int base) {
    return base == 0 ? (m[0] == G_CCMUX_TEXEL1 || m[1] == G_CCMUX_TEXEL1 || m[2] == G_CCMUX_TEXEL1 ||
                        m[2] == G_CCMUX_TEXEL1_ALPHA || m[3] == G_CCMUX_TEXEL1)
                     : (m[4] == G_ACMUX_TEXEL1 || m[5] == G_ACMUX_TEXEL1 || m[6] == G_ACMUX_TEXEL1 ||
                        m[7] == G_ACMUX_TEXEL1);
}
/* Decide: 0 = leave cycle 1 to the cc_id / gfx_blend_tiles; else bit 0 = rgb
 * and bit 1 = alpha are evaluated per texel (bit 1 clear: tile 0's alpha is
 * kept for the cc_id's alpha combiner). */
/* Cycle 1 = (A - K) * TEXEL + K (K = PRIM or ENV) under a real cycle 2: the texel lerp (gfx_update_tex_lerp)
 * only takes it with no cycle 2, so the cc_id kept A * TEXEL and lost K -- the get-item Deku Nut
 * ((PRIM - ENV) * TEXEL0 + ENV, then COMBINED * SHADE) drew near-black with a yellow band where the N64 has a
 * red-brown shell (the Kokiri shop shelf vs mupen z64, 2026-09-28). Evaluated per texel instead; cycle 2 then
 * folds into the vertex colour as for any pre-evaluated cycle 1. OOT_TPEKLERP=0: as before. */
static bool gfx_tpe_klerp_cc2(const uint8_t *m) {
    static int sOn SAVESTATE_EXCLUDE = -1;
    if (sOn < 0) { const char *e = getenv("OOT_TPEKLERP"); sOn = !(e && e[0] == '0'); }
    if (!sOn) return false;
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) != G_CYC_2CYCLE || rdp.cc2_pass) return false;
    return (m[2] == G_CCMUX_TEXEL0 || m[2] == G_CCMUX_TEXEL1) && m[1] == m[3] &&
           (m[1] == G_CCMUX_PRIMITIVE || m[1] == G_CCMUX_ENVIRONMENT) && m[0] != m[1];
}

static int gfx_tpe_plan(bool use_alpha) {
    const uint8_t *m = rdp.cc1;
    int rgb_mode, a_mode;
    bool blend_ok, need;

    if (sTpeMode < 0) {
        const char *e = getenv("OOT_TPE");
        sTpeMode = !(e != NULL && e[0] == '0');
    }
    if (!sTpeMode || !tpe_rgb_evaluable(m)) return 0;
    rgb_mode = gfx_blend_mode(m[0], m[1], m[2], m[3], G_CCMUX_TEXEL0, G_CCMUX_TEXEL1, G_CCMUX_TEXEL0_ALPHA, G_CCMUX_TEXEL1_ALPHA);
    a_mode = gfx_blend_mode(m[4], m[5], m[6], m[7], G_ACMUX_TEXEL0, G_ACMUX_TEXEL1, 0xff, 0xff);
    {
        uint8_t kr[3], ka;
        blend_ok = rgb_mode >= 0 && a_mode >= 0 && !(rgb_mode == 0 && a_mode == 0) &&
                   !(rgb_mode >= 2 && gfx_blend_k_rgb(m[2], kr) < 0) && !(a_mode >= 2 && gfx_blend_k_alpha(m[6], &ka) < 0);
    }
    if (!gfx_cc1_uses_texel1()) blend_ok = true; /* nothing for gfx_blend_tiles to do */
    need = !blend_ok || (tpe_rgb_lossy(m) && !(rgb_mode >= 2)) || (use_alpha && tpe_a_lossy(m) && !(a_mode >= 2));
    if (!need && gfx_tpe_klerp_cc2(m)) need = true;
    if (!need) return 0;
    if (use_alpha && tpe_a_evaluable(m)) return 3;
    if (!use_alpha || !tpe_uses_t1(m, 1)) return 1; /* keep tile 0's alpha for the cc_id */
    return 0;
}

static uint32_t gfx_tpe_tiles(int plan, uint32_t *bw, uint32_t *bh) {
    const uint8_t *m = rdp.cc1;
    struct TextureHashmapNode *n0 = rendering_state.textures[0];
    struct TextureHashmapNode *n1 = gfx_cc1_uses_texel1() ? rendering_state.textures[1] : NULL;
    uint32_t w0, h0, W, H, key[16] = { 0 };
    uint8_t k[9];
    int i, pick = -1;

    /* Mirrored tiles (round 76, the creation story's gold vs z64 + Glide64): a mirrored texture is imported
     * doubled with its mirror image (import_texture), so evaluating over the doubled period and wrapping
     * mirrors it, as gfx_blend_tiles does for tile 1. Refused, the goddess aura's torch-flame combiner
     * (alpha (T1 - 1) * PRIM_LOD_FRAC + T0 over mirrored textures) fell back to alpha = T0: the aura drew
     * far too opaque (cyan-white where the N64 is a faint lavender) and the gold under it looked dark.
     * OOT_TPEMIRROR=0: refuse them, as before. */
    static int sTpeMirror SAVESTATE_EXCLUDE = -1, sTpeShared SAVESTATE_EXCLUDE = -1;
    bool shared1 = false;
    if (sTpeMirror < 0) { const char *e = getenv("OOT_TPEMIRROR"); sTpeMirror = !(e && e[0] == '0'); }
    /* Tile 1 on tile 0's own load (the same TMEM, format, pitch, masks and clamp/mirror; only its
     * corner differs -- a second scroll of one texture, as the goddess aura's Gfx_TwoTexScroll):
     * evaluate with tile 0's texture as tile 1. Refused, the aura fell back as above.
     * OOT_TPESHARED=0: refuse it, as before. */
    if (sTpeShared < 0) { const char *e = getenv("OOT_TPESHARED"); sTpeShared = !(e && e[0] == '0'); }
    if (sTpeShared && n0 != NULL && gfx_cc1_uses_texel1() && !gfx_tile1_importable() &&
        gfx_lt_find(rdp.tiles[1].tmem, false) == gfx_lt_find(rdp.texture_tile.tmem, false) &&
        rdp.tiles[1].tmem == rdp.texture_tile.tmem && rdp.tiles[1].fmt == rdp.texture_tile.fmt &&
        rdp.tiles[1].siz == rdp.texture_tile.siz && rdp.tiles[1].line_size_bytes == rdp.texture_tile.line_size_bytes &&
        rdp.tiles[1].masks == rdp.texture_tile.masks && rdp.tiles[1].maskt == rdp.texture_tile.maskt &&
        rdp.tiles[1].cms == rdp.texture_tile.cms_raw && rdp.tiles[1].cmt == rdp.texture_tile.cmt_raw &&
        rdp.tiles[1].masks != 0 && rdp.tiles[1].maskt != 0) {
        n1 = n0;
        shared1 = true;
    }
    if (n0 == NULL || (n0->mirror != 0 && !sTpeMirror) ||
        (gfx_cc1_uses_texel1() && !shared1 && (n1 == NULL || (n1->mirror != 0 && !sTpeMirror) || !gfx_tile1_importable()))) {
        if (gfx_debug_frame) port_log("  tpe skip n0 %p n1 %p importable %d\n", (void *) n0, (void *) n1, (int) gfx_tile1_importable());
        return 0;
    }
    gfx_tile_size(0, &w0, &h0);
    w0 <<= n0->mirror & 1;
    h0 <<= (n0->mirror >> 1) & 1;
    W = w0;
    H = h0;
    if (n1 != NULL) { /* cover both tile periods, as gfx_blend_tiles does */
        int iw1 = ((int) rdp.tiles[1].lrs - (int) rdp.tiles[1].uls + 4) / 4;
        int ih1 = ((int) rdp.tiles[1].lrt - (int) rdp.tiles[1].ult + 4) / 4;
        if (rdp.tiles[1].masks != 0 && rdp.tiles[1].masks < 11 && !(rdp.tiles[1].cms & G_TX_CLAMP) && iw1 < (1 << rdp.tiles[1].masks))
            iw1 = 1 << rdp.tiles[1].masks; /* a wrapping axis repeats at its mask (gfx_tile_extent) */
        if (rdp.tiles[1].maskt != 0 && rdp.tiles[1].maskt < 11 && !(rdp.tiles[1].cmt & G_TX_CLAMP) && ih1 < (1 << rdp.tiles[1].maskt))
            ih1 = 1 << rdp.tiles[1].maskt;
        uint32_t w1 = gfx_mask_period((uint32_t) (iw1 < 1 ? 1 : iw1), rdp.tiles[1].masks) << (n1->mirror & 1);
        uint32_t h1 = gfx_mask_period((uint32_t) (ih1 < 1 ? 1 : ih1), rdp.tiles[1].maskt) << ((n1->mirror >> 1) & 1);
        if (gfx_is_pow2(w0) && gfx_is_pow2(w1) && !(rdp.texture_tile.cms & G_TX_CLAMP)) {
            uint32_t sh = gfx_tile1_shift(false);
            uint32_t p = sh == 0 ? w1 : sh <= 10 ? w1 << sh : (w1 >> (16 - sh));
            if (p > W) W = p;
        }
        if (gfx_is_pow2(h0) && gfx_is_pow2(h1) && !(rdp.texture_tile.cmt & G_TX_CLAMP)) {
            uint32_t sh = gfx_tile1_shift(true);
            uint32_t p = sh == 0 ? h1 : sh <= 10 ? h1 << sh : (h1 >> (16 - sh));
            if (p > H) H = p;
        }
    }
    /* Known gap (MM audit, 2026-10-18): some of Clock Town's painted walls multiply a brick tile 0 by a mural tile
     * 1 whose common period is 512x512 tile-0 texels; they are skipped here and draw as tile 0 alone. A 512x512
     * cap baked them, but the 1 MB bakes reset the 12 MB texture arena every frame there. The fix is per-pixel
     * (the detail layer with a multiply), not a bigger bake. */
    if (W * H > 256 * 256) {
        if (gfx_debug_frame) port_log("  tpe skip size %ux%u\n", W, H);
        return 0;
    }
    memcpy(&k[0], &rdp.prim_color, 4);
    memcpy(&k[4], &rdp.env_color, 4);
    k[8] = rdp.prim_lod_frac;

    key[0] = (uint32_t) (uintptr_t) n0;
    key[1] = n0->content_hash;
    key[2] = (uint32_t) (uintptr_t) n1;
    key[3] = n1 != NULL ? n1->content_hash : 0;
    key[4] = W | (H << 16);
    key[5] = (uint32_t) (rdp.texture_tile.uls >> 2) | ((uint32_t) (rdp.texture_tile.ult >> 2) << 16);
    key[6] = n1 != NULL ? (uint32_t) (rdp.tiles[1].uls >> 2) | ((uint32_t) (rdp.tiles[1].ult >> 2) << 16) : 0;
    key[7] = (n1 != NULL ? gfx_tile1_shift(false) | (gfx_tile1_shift(true) << 8) : 0) | ((uint32_t) plan << 16) | ((uint32_t) k[8] << 24);
    key[8] = m[0] | (m[1] << 8) | (m[2] << 16) | ((uint32_t) m[3] << 24);
    key[9] = m[4] | (m[5] << 8) | (m[6] << 16) | ((uint32_t) m[7] << 24);
    memcpy(&key[10], &k[0], 4);
    memcpy(&key[11], &k[4], 4);
    key[12] = n0->texture_id;
    key[13] = n1 != NULL ? n1->texture_id : 0;
    /* tile 1 bilinear in the bake when the game filters and tile 1 is magnified (gfx_soft_eval_texels) */
    const bool filt1 = n1 != NULL && (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT &&
                       W * H <= 64 * 64; /* small bakes only: a scrolling one is redone every frame */
    key[14] = filt1;
    key[15] = 0x54504531; /* 'TPE1' */

    for (i = 0; i < GFX_BLEND_CACHE; i++) {
        if (gfx_blend_cache[i].valid && memcmp(gfx_blend_cache[i].key, key, sizeof(key)) == 0) {
            gfx_blend_cache[i].last_used_frame = gfx_frame_counter;
            if (gfx_debug_frame) port_log("  tpe hit %u %ux%u\n", gfx_blend_cache[i].texture_id, W, H);
            *bw = W;
            *bh = H;
            return gfx_blend_cache[i].texture_id;
        }
    }
    for (i = 0; i < GFX_BLEND_CACHE; i++) {
        if (!gfx_blend_cache[i].valid) { pick = i; break; }
        if (pick < 0 || gfx_blend_cache[i].last_used_frame < gfx_blend_cache[pick].last_used_frame) pick = i;
    }
    gfx_flush(); // queued triangles may still sample the entry being rewritten
    if (gfx_blend_cache[pick].texture_id == 0) {
        gfx_blend_cache[pick].texture_id = gfx_rapi->new_texture();
    }
    if (!gfx_soft_eval_texels(gfx_blend_cache[pick].texture_id, n0->texture_id, n1 != NULL ? n1->texture_id : 0,
                              (int) W, (int) H, rdp.texture_tile.uls >> 2, rdp.texture_tile.ult >> 2,
                              rdp.tiles[1].uls >> 2, rdp.tiles[1].ult >> 2, gfx_tile1_shift(false), gfx_tile1_shift(true), m,
                              k, !(plan & 2), filt1)) {
        gfx_blend_cache[pick].valid = false;
        return 0;
    }
    memcpy(gfx_blend_cache[pick].key, key, sizeof(key));
    gfx_blend_cache[pick].valid = true;
    gfx_blend_cache[pick].last_used_frame = gfx_frame_counter;
    if (gfx_debug_frame) port_log("  tpe tex %u %ux%u plan %d cc1 %u %u %u %u / %u %u %u %u\n", gfx_blend_cache[pick].texture_id, W, H, plan, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
    *bw = W;
    *bh = H;
    return gfx_blend_cache[pick].texture_id;
}

/* (A - K) * TEXEL0 + K, K = PRIM or ENV: OoT's everyday tint (HUD icons, text,
 * file select, pause cursor: G_CC_PRIMITIVE-ENV style). Evaluated with the
 * texel as 1 the vertex colour is A and a modulate loses K; the backend lerps
 * K -> vertex colour by the texel instead. Not with cycle 2 folded in. */
/* Cycle 1 = TEXEL0 * K1 (K1 = PRIM, ENV or SHADE; either order) under a cycle 2 (X - K) * COMBINED + K:
 * the vertex colour takes the texel as 1.0, so it is K + (X - K) * K1, and the texel lerp
 * K + (vertex - K) * TEXEL0 is K + (X - K) * K1 * TEXEL0 -- the RDP's result. Barinade's Bari sparks
 * ((TEXEL0 * PRIM), then (PRIM - ENV) * COMBINED + ENV) lost the golden ENV and drew black-and-white
 * (the visual tour, 2026-09-28). OOT_TEXLERP2M=0: only the plain pass-through, as before. */
static bool gfx_cc1_texel_times_k(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint32_t cc_id) {
    static int sOn SAVESTATE_EXCLUDE = -1;
    if (sOn < 0) { const char *e = getenv("OOT_TEXLERP2M"); sOn = !(e && e[0] == '0'); }
    if (!sOn || b != CC_0 || d != CC_0 || (cc_id & CC_FLAG_RGB_A_ONE)) return false;
    return (a == CC_TEXEL0 && (c == CC_PRIM || c == CC_ENV || c == CC_SHADE)) ||
           (c == CC_TEXEL0 && (a == CC_PRIM || a == CC_ENV || a == CC_SHADE));
}

/* OOT_SHADELERP (2026-10-03, the Deku Tree's vision: the light beams under the Triforce dark grey where the N64 has them
 * light cyan): cycle 2 = (X - SHADE) * COMBINED + SHADE (X = PRIM, ENV or 1) over a cycle 1 that passes the texel --
 * DemoEffect_DrawCrystalLight's gCrystalLightDL: blue shade (0,100,255 .. 0,200,255) lerped to PRIM (255,255,170) by
 * the scrolled two-tile texture. Only K = PRIM or ENV was taken below; this one fell back to the vertex colour at
 * COMBINED = 1, i.e. PRIM, times the texel: the blue gone, black where the texture is dark. Now the vertex colour is
 * the shade (gfx_eval_cycle2, gfx_vc_compile) and the rasterizer lerps it toward X by the texel (gfx_soft.c
 * texlerpv): SHADE + (X - SHADE) * TEXEL, the RDP's result. OOT_SHADELERP=0: as before. */
static bool gfx_shade_lerp_on(void) {
    static int sOn SAVESTATE_EXCLUDE = -1;
    if (sOn < 0) { const char *e = getenv("OOT_SHADELERP"); sOn = !(e && e[0] == '0'); }
    return sOn;
}
static void gfx_update_tex_lerp(uint32_t cc_id, bool cc2) {
    extern void gfx_scegu_set_tex_lerp(bool on, uint8_t r, uint8_t g, uint8_t b);
    extern void gfx_scegu_set_tex_lerpv(bool on);
    uint32_t bits = cc_id & 0xfff;
    uint8_t a = bits & 7, b = (bits >> 3) & 7, c = (bits >> 6) & 7, d = (bits >> 9) & 7;
    bool on = false, lv = false;
    struct RGBA k = { 0, 0, 0, 0 };
    if (!cc2 && c == CC_TEXEL0 && b == d && (b == CC_PRIM || b == CC_ENV) && (a != b || (cc_id & CC_FLAG_RGB_A_ONE)) &&
        !(cc_id & CC_FLAG_RGB_D_ONE)) {
        k = b == CC_PRIM ? rdp.prim_color : rdp.env_color;
        on = (k.r | k.g | k.b) != 0;
    } else if (cc2 && !(cc_id & CC_FLAG_RGB_D_ONE) &&
               ((c == CC_0 && d == CC_TEXEL0) || gfx_cc1_texel_times_k(a, b, c, d, cc_id))) {
        /* Cycle 1 passes the texel through (plain, pre-blended or pre-evaluated)
         * and cycle 2 is (X - K) * COMBINED + K: the same lerp with COMBINED as
         * the texel -- torch flames' (PRIM - ENV) * COMBINED + ENV, the
         * (SHADE - PRIM) * COMBINED + PRIM tints. The vertex colour (cycle 2 at
         * COMBINED = 1) is X. OOT_TEXLERP2=0 turns it off. */
        static int sTexLerp2 = -1;
        const uint8_t *c2 = rdp.cc2;
        if (sTexLerp2 < 0) {
            const char *e = getenv("OOT_TEXLERP2");
            sTexLerp2 = !(e != NULL && e[0] == '0');
        }
        if (sTexLerp2 && c2[2] == G_CCMUX_COMBINED && c2[1] == c2[3] &&
            (c2[1] == G_CCMUX_PRIMITIVE || c2[1] == G_CCMUX_ENVIRONMENT) && c2[0] != c2[1] &&
            (c2[0] == G_CCMUX_PRIMITIVE || c2[0] == G_CCMUX_SHADE || c2[0] == G_CCMUX_ENVIRONMENT || c2[0] == G_CCMUX_1)) {
            k = c2[1] == G_CCMUX_PRIMITIVE ? rdp.prim_color : rdp.env_color;
            on = (k.r | k.g | k.b) != 0;
        } else if (c == CC_0 && d == CC_TEXEL0 && gfx_shade_lerp_on() && c2[2] == G_CCMUX_COMBINED &&
                   c2[1] == G_CCMUX_SHADE && c2[3] == G_CCMUX_SHADE &&
                   (c2[0] == G_CCMUX_PRIMITIVE || c2[0] == G_CCMUX_ENVIRONMENT || c2[0] == G_CCMUX_1)) {
            /* OOT_SHADELERP: (X - SHADE) * COMBINED + SHADE, the texel passed through cycle 1 */
            static const struct RGBA white = { 255, 255, 255, 255 };
            k = c2[0] == G_CCMUX_PRIMITIVE ? rdp.prim_color : c2[0] == G_CCMUX_ENVIRONMENT ? rdp.env_color : white;
            on = lv = true;
        }
    }
    if (on != rendering_state.tex_lerp || lv != rendering_state.lerp_v ||
        (on && (k.r != rendering_state.lerp_k.r || k.g != rendering_state.lerp_k.g || k.b != rendering_state.lerp_k.b))) {
        gfx_flush();
        gfx_scegu_set_tex_lerpv(lv);
        gfx_scegu_set_tex_lerp(on, k.r, k.g, k.b);
        rendering_state.tex_lerp = on;
        rendering_state.lerp_v = lv;
        rendering_state.lerp_k = k;
    }
}

static inline float dot(const float a[3], const float b[3])
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

/* Round 57 (the user's creation-story shots, IMG_0188-0190): any non-zero length is normalised, as the
 * RSP does. The old floor (a squared length over 1e-5) skipped models drawn at a small scale -- the
 * goddesses (a light/lookat direction ~0.003 long through their matrix) -- so their texgen and lighting
 * read n.l ~ 0: one flat colour instead of the gold shine. OOT_NORMFIX=0: the old floor (A/B). */
/* Round 78 (the release hold, against angrylion): the texgen (lookat) axes of a model drawn at a tiny scale
 * do not reach unit length on the console. Fitted on the creation-story goddesses (lookat 0.0030 long
 * through their 0.03 scale): 1 / max(length, 0.015) puts their body within a few levels of angrylion (mean
 * 202-212 vs 198-210, the same spread); unit length drew the texgen sweep too wide (dark olive edges, ~12%
 * darker), no normalising at all one flat khaki. Lookat axes only: the lights stay at unit length (the floor
 * on lights darkened every 0.01-scale actor, Link included, away from angrylion). The mechanism in the
 * microcode is not pinned down; this is the fit. OOT_NORMFLOOR=<length> (0 = unit length, round 57). */
/* Set while the texgen (lookat) axes are normalised: the floor below applies to them only. */
static int gfx_norm_lookat SAVESTATE_EXCLUDE;
static void gfx_normalize_vector(float v[3]) {
    static int sNormFix SAVESTATE_EXCLUDE = -1;
    static float sNormFloor SAVESTATE_EXCLUDE = -1.0f;
    if (sNormFix < 0) { const char *e = getenv("OOT_NORMFIX"); sNormFix = !(e && e[0] == '0'); }
    if (sNormFloor < 0.0f) { const char *e = getenv("OOT_NORMFLOOR"); sNormFloor = e ? (float) atof(e) : 0.015f; }
    float dot = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if(dot > (sNormFix ? 1e-30f : 0.00001f)){
        const float len = sqrtf(dot);
        const float scale = 1.0f / (sNormFix && gfx_norm_lookat && len < sNormFloor ? sNormFloor : len);
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

/* The factor a tile shift applies to texture coordinates. */
static float gfx_tile_shift_scale(uint8_t shift) {
    if (!gfx_tileshift_on() || shift == 0 || shift > 15) return 1.0f;
    return shift <= 10 ? 1.0f / (float) (1u << shift) : (float) (1u << (16 - shift));
}

/* OOT_TEXGENLIN=0: G_TEXTURE_GEN_LINEAR drawn as plain texgen (A/B). */
static bool gfx_texgen_linear_on(void) {
    static int sOn = -1;
    if (sOn < 0) { const char *e = getenv("OOT_TEXGENLIN"); sOn = !(e && e[0] == '0'); }
    return sOn != 0;
}

/* OOT_LOOKAT=0: texgen from the fixed axes, ignoring gSPLookAt (A/B). */
static bool gfx_lookat_on(void) {
    static int sOn = -1;
    if (sOn < 0) { const char *e = getenv("OOT_LOOKAT"); sOn = !(e && e[0] == '0'); }
    return sOn != 0;
}

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
    if (gfx_debug_frame)
        port_log("    m (%.3f %.3f %.3f %.3f) (%.3f %.3f %.3f %.3f) (%.3f %.3f %.3f %.3f) (%.1f %.1f %.1f %.3f)\n",
                 matrix[0][0], matrix[0][1], matrix[0][2], matrix[0][3], matrix[1][0], matrix[1][1], matrix[1][2],
                 matrix[1][3], matrix[2][0], matrix[2][1], matrix[2][2], matrix[2][3], matrix[3][0], matrix[3][1],
                 matrix[3][2], matrix[3][3]);

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
    /* The RSP never pops its last matrix (F3DEX2: a pop at the bottom of the stack does nothing). Popping to 0
     * made the next G_MTX load write modelview_matrix_stack[-1], the 64 bytes before rsp: rendering_state, whose
     * texture pointers became matrix floats and crashed the next textured triangle (1.0.0: the Astral
     * Observatory, entered from Termina Field or the telescope). */
    while (count--) {
        if (rsp.modelview_matrix_stack_size > 1) {
            --rsp.modelview_matrix_stack_size;
        }
    }
    if (rsp.modelview_matrix_stack_size > 0) {
        gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
    }
    /* MM_POPLIGHTS (ALPHA7, Clock Town's thatch flipping dark brown <-> bright tan as Link moved): the lights are
     * taken into model space with the modelview of the moment, and a pop changes the modelview as much as a load.
     * Without this the geometry after a gsSPPopMatrix was lit through the matrix that was popped: East Clock
     * Town's sign billboards (gsSPMatrix of the 0x01000040 cylindrical billboard, then gsSPPopMatrix) turned the
     * sun with the camera's yaw for whatever room entries came next, and Room_DrawCullable sorts its entries by
     * depth every frame, so a small step changed which roofs came after a sign. F3DEX2 relights after a pop too
     * (angrylion: the same roof keeps its shade at both positions). */
    rsp.lights_changed = 1;
}

/* Prerendered (JPEG) rooms: the background image (gfx_s2dex_bg) is the game's
 * whole 4:3 view squashed into the narrow screen, so from the moment a frame
 * draws one, its 3D is squashed the same way instead of cropped -- else Link,
 * the actors and the collision they follow no longer line up with the walls,
 * counters and doors in the picture (device, v15: Link walked into things that
 * were not drawn there). Per frame, not per room: a room only draws its image
 * under the fixed prerendered camera (Link's house has one but a free camera).
 * z_room.c tags the display list (HH_TAG_PRERENDER) before the room's depth-only
 * occluders, which it draws BEFORE the image: squashing only from the image on
 * left them cropped and hid Link in the wrong places (the Kokiri Shop nook).
 * The room draws before the actors, so everything after the tag follows it. */
static bool gfx_jpeg_room_squash;
/* OOT_CROP3D (the MK64_CROP3D question, 2026-09-29; the user picked 2, the default: "follow but ensure it
 * works"): prerendered rooms keep their true shape too. 0 = the old squeeze (the image fitted to the screen,
 * the 3D squeezed with it). Everything else is already cropped, not squeezed (gfx_sp_vertex).
 *   1: the image at the 3D's scale about the screen centre (its middle 240 columns on a square output), the 3D
 *      cropped with it, so both still line up -- the room's left and right 40 N64 pixels are off screen,
 *      and some rooms have exits there (OOT_EXITSCAN, BUILDLOG round 64).
 *   2: the same, but the 240-wide window slides across the 320-wide room with Link (hh_main.c sets
 *      gHhCropPan, N64 pixels from the centre, +-40 on a square output), so an exit at an edge comes into view
 *      as he walks to it. The image and the 3D shift together, so they still line up. */
float gfx_current_dimensions_aspect(void) { /* the output's width / height (hh_main.c's crop window) */
    return (float) gfx_current_dimensions.width / (float) gfx_current_dimensions.height;
}
float gHhCropPan; /* hh_main.c, per frame, in a prerendered room with OOT_CROP3D=2 */
int gHhCropPan3D;  /* hh_main.c: the castle courtyard's side-scroll camera -- the pan applies to the whole frame's 3D */
int gfx_crop3d(void) {
    static int sCrop = -1;
    if (sCrop < 0) { const char *e = getenv("OOT_CROP3D"); sCrop = (e != NULL && (e[0] == '0' || e[0] == '1')) ? e[0] - '0' : 2; }
    return sCrop;
}
static float gfx_jpeg_room_pan; /* this frame's pan (N64 px), 0 outside prerendered frames */
static void gfx_jpeg_room_squash_on(void) { /* OOT_JPEGSQUASH=0: the old crop, for A/B */
    static int sSquashOn = -1;
    if (sSquashOn < 0) { const char *e = getenv("OOT_JPEGSQUASH"); sSquashOn = !(e != NULL && e[0] == '0'); }
    gfx_jpeg_room_squash = sSquashOn != 0 && !gfx_crop3d();
    gfx_jpeg_room_pan = gfx_crop3d() == 2 ? gHhCropPan : 0.0f;
}
/* The game's depth reads (the sun's lens flare, z_kankyo.c; light glows, z_lights.c; hh_read_depth in
 * gfx_soft.c) name a pixel of the N64's 320x240 frame. The 3D here is that frame's middle at its true shape
 * (gfx_sp_vertex), so N64 x sits at the target's centre + (x + 0.5 - 160 - pan) * h / 240. Reading at
 * x * w / 320 squeezed every read toward the centre (on a square output by a quarter of the sun's offset, up to 30 px
 * at the edges) and answered for the 40-px bands off screen at the window's edges, so the flare came and went
 * by whatever was drawn beside the sun, and showed for a sun out of view. A frame squeezed to fit
 * (gfx_jpeg_room_squash, OOT_CROP3D=0) keeps x * w / 320. -1: outside the 3D. The same pixels at 320x240.
 * OOT_FLARE=0: the old read, for A/B. */
int gfx_depth_read_x(int x, int w, int h) {
    static int sOn = -1;
    if (sOn < 0) { const char *e = getenv("OOT_FLARE"); sOn = !(e != NULL && e[0] == '0'); }
    if (!sOn || gfx_jpeg_room_squash || h <= 0) return x * w / 320;
    const float bx = (float) w * 0.5f + ((float) x + 0.5f - 160.0f - gfx_jpeg_room_pan) * (float) h / 240.0f;
    return bx >= 0.0f && bx < (float) w ? (int) bx : -1;
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
     * ((v0*M0 + v1*M1) + v2*M2) + v3*M3, each product rounded before its add
     * (NEON VMLA is not fused) -- the scalar loop's order and rounding, 8
     * instructions instead of ~80. */
    const float (*const nMP)[4] = rsp.MP_matrix;
    const float (*const nMV)[4] = rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1];
    const float32x4_t mp0 = vld1q_f32(nMP[0]), mp1 = vld1q_f32(nMP[1]), mp2 = vld1q_f32(nMP[2]), mp3 = vld1q_f32(nMP[3]);
    const float32x4_t mv0 = vld1q_f32(nMV[0]), mv1 = vld1q_f32(nMV[1]), mv2 = vld1q_f32(nMV[2]), mv3 = vld1q_f32(nMV[3]);
#endif
    /* The aspect quotient of gfx_adjust_x_for_aspect_ratio, divided once a
     * batch instead of every vertex (same operands: the same float). */
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
        float x = (rsp.is_persp && !gfx_ui_mode && !gfx_jpeg_room_squash) ? proj_vec[0] * (4.0f / 3.0f) / aspect_q
                                                                          : proj_vec[0];
        /* OOT_CROP3D=2: the window's pan in a prerendered room, as clip x (N64 px -> output NDC, times w);
         * only when there is one, so the default path is the same arithmetic as before */
        if (gfx_jpeg_room_pan != 0.0f && rsp.is_persp && !gfx_ui_mode) {
            x -= gfx_jpeg_room_pan * (2.0f / (240.0f * aspect_q)) * proj_vec[3];
        }
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
                /* Texgen axes: the task's gSPLookAt (guLookAtHilite before every hilite: swords,
                 * shields, metal), s from LOOKATX and t from LOOKATY, as the console's microcode
                 * does (GoldenEye, against the z64 LLE reference). Each task starts from the fixed
                 * axes the port always used (gfx_sp_reset); a zero vector keeps the last axis.
                 * OOT_LOOKAT=0: the fixed axes always, for A/B. */
                const Light_t *ls = &lookat_x, *lt = &lookat_y;
                if (gfx_lookat_on()) { ls = &rsp.current_lookat[1]; lt = &rsp.current_lookat[0]; }
                /* A zero vector gives n.l = 0 on every vertex, as on the RSP: the texture's centre.
                 * (OoT's N64 logo points its gsSPLookAt one byte past the end of its file, at
                 * the arena's next node, NULL there: every face samples the centre texel, the
                 * pale "N" of the console. Normalising zero would give NaNs.) */
                gfx_norm_lookat = 1;
                if (ls->dir[0] | ls->dir[1] | ls->dir[2]) calculate_normal_dir(ls, rsp.current_lookat_coeffs[0]);
                else memset(rsp.current_lookat_coeffs[0], 0, sizeof(rsp.current_lookat_coeffs[0]));
                if (lt->dir[0] | lt->dir[1] | lt->dir[2]) calculate_normal_dir(lt, rsp.current_lookat_coeffs[1]);
                else memset(rsp.current_lookat_coeffs[1], 0, sizeof(rsp.current_lookat_coeffs[1]));
                gfx_norm_lookat = 0;
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
                
                if ((rsp.geometry_mode & G_TEXTURE_GEN_LINEAR) && gfx_texgen_linear_on()) {
                    /* G_TEXTURE_GEN_LINEAR: the microcode maps the angle, not the cosine, so a
                     * flat face sweeps the texture evenly (GLideN64: acos(n.l) * 1024 / pi before
                     * the texture scale): s = acos(n.l) / pi * scale / 2 in S10.5, the same range
                     * as the plain mapping. OOT_TEXGENLIN=0: the plain mapping (A/B). */
                    const float cx = dotx / 127.0f < -1.0f ? -1.0f : dotx / 127.0f > 1.0f ? 1.0f : dotx / 127.0f;
                    const float cy = doty / 127.0f < -1.0f ? -1.0f : doty / 127.0f > 1.0f ? 1.0f : doty / 127.0f;
                    extern float hh_acosf(float); /* hh_trig.c: the same bits on x86 and ARM */
                    /* acos(-n.l) (GLideN64): n.l = -1 -> 0, +1 -> the far edge, the plain mapping's
                     * orientation. acos(+n.l) mirrored every linear-texgen texture (the N64 boot
                     * logo's shine vs mupen64plus z64, 2026-09-27). OOT_TEXGENSIGN=0: the old sign. */
                    static int sSign = -1;
                    if (sSign < 0) { const char *e = getenv("OOT_TEXGENSIGN"); sSign = !(e && e[0] == '0'); }
                    const float sx = sSign ? -cx : cx, sy = sSign ? -cy : cy;
                    U = (int32_t)(hh_acosf(sx) * (1.0f / (float)M_PI) * 0.5f * rsp.texture_scaling_factor.s);
                    V = (int32_t)(hh_acosf(sy) * (1.0f / (float)M_PI) * 0.5f * rsp.texture_scaling_factor.t);
                } else {
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
            {   /* the guard band (R33): the clip ratio times |w| */
                const float rw = (float) gfx_clip_ratio * aw;
                if (x < -rw || x > rw || y < -rw || y > rw) rej |= GB_OUT;
            }
            // In front of the game's near plane (z_ndc < -1) or behind the eye (MK64
            // 4f64001): was w < W_NEAR, which the trivial reject treated as
            // "outside" and so deleted every triangle within 24 units of the eye.
            if (gfx_non_on() ? (w < W_MIN) : (z < -w)) rej |= Z_POS; /* NoN: only the eye's side */
            // Beyond the far plane. z > 2|w| (the kit's) never fires: a perspective
            // z/w stays below (f+n)/(f-n) ~ 1.002. z > w is only true in front of
            // the eye (behind it z < w < 0). OOT_NEARCLIP=0: the old test.
            if (gfx_nearclip_on() ? (z > w) : (z > FAR_GUARD * aw)) rej |= Z_NEG;
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

#if !defined(TARGET_PSP) && !defined(PORT_GE_CULL) && !defined(PORT_GE_TL)
/* OOT_VTXFAST (default on; =0 the loop above): gSPVertex as two passes, the same arithmetic.
 * The device profile (prof_basement.txt, build 9d432f7e) had the inlined loop above at ~3.7% of
 * the frame: inside gfx_run_dl's register pressure GCC kept the eight matrix rows on the stack
 * (16 reloads a vertex), built each lane broadcast through a VFP store and a NEON lane load, read
 * the NEON products back through the stack, and reloaded every rsp / rdp field after each store
 * through d (d lies inside rsp). Here pass 1 transforms the whole load with the rows held in
 * registers (the same VMUL/VMLA by scalar, the same order: ((v0*M0 + v1*M1) + v2*M2) + 1*M3; the
 * int16 coordinates widened in NEON, exact), and pass 2 is the old per-vertex body, statement for
 * statement, on locals hoisted out of the loop (the flags, the fog and texture factors, the
 * aspect quotient and the pan factor: the same float operations on the same operands). The
 * lights are recomputed before the loop instead of at its first vertex (the old loop did it
 * there once, when lights_changed and lighting is on and the load is not empty).
 * tools/handheld/dl_test.c checks the loaded vertices and every draw byte for byte against the
 * old loop, on x86 and ARM. */
static int sVtxFast SAVESTATE_EXCLUDE = -1;
static inline bool gfx_vtxfast_on(void) {
    if (sVtxFast < 0) { const char *e = getenv("OOT_VTXFAST"); sVtxFast = !(e && e[0] == '0'); }
    return sVtxFast != 0;
}

/* OOT_VTXFLAGS=1 (with OOT_VTXFAST; default off, see below): the clip flags compared as integers. For floats
 * that are not NaN, a < b exactly when vtx_key(a) < vtx_key(b), and vtx_key(-a) = -vtx_key(a): the key is the bit
 * pattern for a sign bit of 0 and minus the magnitude bits for a sign bit of 1, so +0 and -0 share key 0, denormals
 * and infinities keep their order, and |key| <= 0x7F800000 never overflows when negated. A vertex with a NaN among
 * x y z w (the others derive from w) takes the float compares, which are false for NaN. The VFP compares cost a VMRS
 * each (14 a vertex); here the four coordinates and three bounds cross to the core registers once. */
/* Default off: the A7's VMRS cost is not known here (qemu counts the integer path as more instructions, ~65 against
 * ~48 inlined, with no VMRS); the Bench view "Kokiri Forest, integer clip flags" decides. */
static int sVtxFlags SAVESTATE_EXCLUDE = -1;
static inline int32_t vtx_key(float f) {
    int32_t i;
    memcpy(&i, &f, sizeof(i));
    return i >= 0 ? i : (int32_t)(0x80000000u - (uint32_t)i);
}
static inline bool vtx_isnan(float f) {
    uint32_t i;
    memcpy(&i, &f, sizeof(i));
    return (i & 0x7FFFFFFFu) > 0x7F800000u;
}

/* A perspective vertex's clip flags (the old loop's block); keys: OOT_VTXFLAGS's integer compares. */
static inline uint32_t vtx_clip_flags(float x, float y, float z, float w, float ratio, bool non, bool nearclip, int keys) {
    uint32_t rej = 0;
    if (keys && !vtx_isnan(x) && !vtx_isnan(y) && !vtx_isnan(z) && !vtx_isnan(w)) {
        /* |w| by its bits: for w = -0 the old aw is -0 and this +0, which share key 0 (as do gw, rw and fg) */
        const float aw = fabsf(w);
        const float gw = GUARD_BAND * aw, rw = ratio * aw, fg = FAR_GUARD * aw;
        const int32_t kx = vtx_key(x), ky = vtx_key(y), kz = vtx_key(z), kw = vtx_key(w);
        const int32_t kg = vtx_key(gw), kr = vtx_key(rw);
        rej = (kx < -kg ? X_POS : 0u) | (kx > kg ? X_NEG : 0u) | (ky < -kg ? Y_POS : 0u) | (ky > kg ? Y_NEG : 0u) |
              (kx < -kw ? X_OUT_L : 0u) | (kx > kw ? X_OUT_R : 0u) | (ky < -kw ? Y_OUT_B : 0u) | (ky > kw ? Y_OUT_T : 0u) |
              ((kx < -kr || kx > kr || ky < -kr || ky > kr) ? GB_OUT : 0u) |
              ((non ? kw < vtx_key(W_MIN) : kz < -kw) ? Z_POS : 0u) |
              ((nearclip ? kz > kw : kz > vtx_key(fg)) ? Z_NEG : 0u);
        return rej;
    }
    {   /* the old loop's compares, verbatim */
        float aw = w < 0.0f ? -w : w;
        float gw = GUARD_BAND * aw;
        if (x < -gw) rej |= X_POS;
        if (x > gw) rej |= X_NEG;
        if (y < -gw) rej |= Y_POS;
        if (y > gw) rej |= Y_NEG;
        if (x < -w) rej |= X_OUT_L;
        if (x > w) rej |= X_OUT_R;
        if (y < -w) rej |= Y_OUT_B;
        if (y > w) rej |= Y_OUT_T;
        {
            const float rw = ratio * aw;
            if (x < -rw || x > rw || y < -rw || y > rw) rej |= GB_OUT;
        }
        if (non ? (w < W_MIN) : (z < -w)) rej |= Z_POS;
        if (nearclip ? (z > w) : (z > FAR_GUARD * aw)) rej |= Z_NEG;
    }
    return rej;
}

static __attribute__((noinline)) void gfx_sp_vertex_v2(size_t n_vertices, size_t dest_index, const Vtx *vertices) {
    float pv[MAX_VERTICES][8] __attribute__((aligned(16))); /* proj (clip) x y z w, view x y z w */
    if (n_vertices == 0) return;
    if (n_vertices > MAX_VERTICES) n_vertices = MAX_VERTICES; /* pv's size; the old loop wrote past
                                                             * loaded_vertices too: F3DEX2 decodes <= 127 */
    {
        const float (*const MP)[4] = rsp.MP_matrix;
        const float (*const MV)[4] = rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1];
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        const float32x4_t mp0 = vld1q_f32(MP[0]), mp1 = vld1q_f32(MP[1]), mp2 = vld1q_f32(MP[2]), mp3 = vld1q_f32(MP[3]);
        const float32x4_t mv0 = vld1q_f32(MV[0]), mv1 = vld1q_f32(MV[1]), mv2 = vld1q_f32(MV[2]), mv3 = vld1q_f32(MV[3]);
        for (size_t i = 0; i < n_vertices; i++) {
            /* ob[0..2] and the flag word: the flag lane is never used */
            const float32x4_t t = vcvtq_f32_s32(vmovl_s16(vld1_s16(vertices[i].v.ob)));
            const float32x2_t t01 = vget_low_f32(t), t23 = vget_high_f32(t);
            float32x4_t p = vmulq_lane_f32(mp0, t01, 0), q = vmulq_lane_f32(mv0, t01, 0);
            p = vmlaq_lane_f32(p, mp1, t01, 1); q = vmlaq_lane_f32(q, mv1, t01, 1);
            p = vmlaq_lane_f32(p, mp2, t23, 0); q = vmlaq_lane_f32(q, mv2, t23, 0);
            p = vmlaq_n_f32(p, mp3, 1.0f);      q = vmlaq_n_f32(q, mv3, 1.0f);
            vst1q_f32(pv[i], p);
            vst1q_f32(pv[i] + 4, q);
        }
#else
        for (size_t i = 0; i < n_vertices; i++) {
            const float t0 = vertices[i].v.ob[0], t1 = vertices[i].v.ob[1], t2 = vertices[i].v.ob[2], t3 = 1.0f;
            for (int c = 0; c < 4; c++) {
                pv[i][c] = t0 * MP[0][c] + t1 * MP[1][c] + t2 * MP[2][c] + t3 * MP[3][c];
                pv[i][4 + c] = t0 * MV[0][c] + t1 * MV[1][c] + t2 * MV[2][c] + t3 * MV[3][c];
            }
        }
#endif
    }

    const uint32_t gm = rsp.geometry_mode;
    const bool persp = rsp.is_persp;
    const float aspect_q = (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
    const bool widen = persp && !gfx_ui_mode && !gfx_jpeg_room_squash;
    const bool pan_on = gfx_jpeg_room_pan != 0.0f && persp && !gfx_ui_mode;
    const float pan_k = gfx_jpeg_room_pan * (2.0f / (240.0f * aspect_q)); /* the old (pan * (2 / (240 aspect))) */
    const float fog_mul = rsp.fog_mul, fog_offset = rsp.fog_offset;
    const int tsf_s = rsp.texture_scaling_factor.s, tsf_t = rsp.texture_scaling_factor.t;
    const bool lighting = (gm & G_LIGHTING) != 0;
    const bool texgen = lighting && (gm & G_TEXTURE_GEN) != 0;
    const bool texgen_lin = texgen && (gm & G_TEXTURE_GEN_LINEAR) && gfx_texgen_linear_on();
    const float ratio = (float) gfx_clip_ratio;
    const bool non = gfx_non_on(), nearclip = gfx_nearclip_on();
    const int dbg = gfx_debug_frame;
    if (sVtxFlags < 0) { const char *e = getenv("OOT_VTXFLAGS"); sVtxFlags = e != NULL && e[0] == '1'; }
    const int flagkeys = sVtxFlags;
    int nl = 0;
    unsigned int amb_r = 0, amb_g = 0, amb_b = 0;
    if (lighting) {
        if (rsp.lights_changed) {
            for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                calculate_normal_dir(&rsp.current_lights[i], rsp.current_lights_coeffs[i]);
            }
            static const Light_t lookat_x = {{0, 0, 0}, 0, {0, 0, 0}, 0, {127, 0, 0}, 0};
            static const Light_t lookat_y = {{0, 0, 0}, 0, {0, 0, 0}, 0, {0, 127, 0}, 0};
            const Light_t *ls = &lookat_x, *lt = &lookat_y;
            if (gfx_lookat_on()) { ls = &rsp.current_lookat[1]; lt = &rsp.current_lookat[0]; }
            gfx_norm_lookat = 1;
            if (ls->dir[0] | ls->dir[1] | ls->dir[2]) calculate_normal_dir(ls, rsp.current_lookat_coeffs[0]);
            else memset(rsp.current_lookat_coeffs[0], 0, sizeof(rsp.current_lookat_coeffs[0]));
            if (lt->dir[0] | lt->dir[1] | lt->dir[2]) calculate_normal_dir(lt, rsp.current_lookat_coeffs[1]);
            else memset(rsp.current_lookat_coeffs[1], 0, sizeof(rsp.current_lookat_coeffs[1]));
            gfx_norm_lookat = 0;
            rsp.lights_changed = false;
        }
        nl = rsp.current_num_lights - 1;
        amb_r = rsp.current_lights[nl].col[0];
        amb_g = rsp.current_lights[nl].col[1];
        amb_b = rsp.current_lights[nl].col[2];
    }
    int sign = 1; /* OOT_TEXGENSIGN, read where the old loop reads it */
    if (texgen_lin) {
        static int sSign = -1;
        if (sSign < 0) { const char *e = getenv("OOT_TEXGENSIGN"); sSign = !(e && e[0] == '0'); }
        sign = sSign;
    }

    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const Vtx_t *v = &vertices[i].v;
        const Vtx_tn *vn = &vertices[i].n;
        struct LoadedVertex *d = &rsp.loaded_vertices[dest_index];
        const float *proj_vec = pv[i], *view_vec = pv[i] + 4;
        int fog_alpha = -1;

        float x = widen ? proj_vec[0] * (4.0f / 3.0f) / aspect_q : proj_vec[0];
        if (pan_on) {
            x -= pan_k * proj_vec[3];
        }
        const float y = proj_vec[1];
        const float z = proj_vec[2];
        float w = proj_vec[3];
        if (gm & G_FOG) {
            float winv = (w > -0.001f && w < 0.001f) ? 32767.0f : 1.0f / w;
            float fog_z;
            if (winv < 0.0f) winv = 32767.0f;
            fog_z = z * winv * fog_mul + fog_offset;
            if (fog_z < 0.0f) fog_z = 0.0f;
            if (fog_z > 255.0f) fog_z = 255.0f;
            fog_alpha = (int) fog_z;
        }

        short U = v->tc[0] * tsf_s >> 16;
        short V = v->tc[1] * tsf_t >> 16;

        uint8_t cr, cg, cb;
        if (lighting) {
            unsigned int r = amb_r, g = amb_g, b = amb_b;
            for (int l = 0; l < nl; l++) {
                float intensity = 0;
                intensity += vn->n[0] * rsp.current_lights_coeffs[l][0];
                intensity += vn->n[1] * rsp.current_lights_coeffs[l][1];
                intensity += vn->n[2] * rsp.current_lights_coeffs[l][2];
                intensity /= 127.0f;
                if (intensity > 0.0f) {
                    r += intensity * rsp.current_lights[l].col[0];
                    g += intensity * rsp.current_lights[l].col[1];
                    b += intensity * rsp.current_lights[l].col[2];
                }
            }
            cr = r > 255 ? 255 : r;
            cg = g > 255 ? 255 : g;
            cb = b > 255 ? 255 : b;
            if (texgen) {
                float dotx = 0, doty = 0;
                dotx += vn->n[0] * rsp.current_lookat_coeffs[0][0];
                dotx += vn->n[1] * rsp.current_lookat_coeffs[0][1];
                dotx += vn->n[2] * rsp.current_lookat_coeffs[0][2];
                doty += vn->n[0] * rsp.current_lookat_coeffs[1][0];
                doty += vn->n[1] * rsp.current_lookat_coeffs[1][1];
                doty += vn->n[2] * rsp.current_lookat_coeffs[1][2];
                if (texgen_lin) {
                    const float cx = dotx / 127.0f < -1.0f ? -1.0f : dotx / 127.0f > 1.0f ? 1.0f : dotx / 127.0f;
                    const float cy = doty / 127.0f < -1.0f ? -1.0f : doty / 127.0f > 1.0f ? 1.0f : doty / 127.0f;
                    extern float hh_acosf(float);
                    const float sx = sign ? -cx : cx, sy = sign ? -cy : cy;
                    U = (int32_t)(hh_acosf(sx) * (1.0f / (float)M_PI) * 0.5f * tsf_s);
                    V = (int32_t)(hh_acosf(sy) * (1.0f / (float)M_PI) * 0.5f * tsf_t);
                } else {
                    U = (int32_t)((dotx / 127.0f + 1.0f) / 4.0f * tsf_s);
                    V = (int32_t)((doty / 127.0f + 1.0f) / 4.0f * tsf_t);
                }
            }
        } else {
            cr = v->cn[0];
            cg = v->cn[1];
            cb = v->cn[2];
        }

        const uint32_t rej = persp ? vtx_clip_flags(x, y, z, w, ratio, non, nearclip, flagkeys) : 0;

        if (dbg) {
            static int n;
            if (proj_vec[3] != 1.0f && n++ < 6) {
                const float temp_vec[3] = { v->ob[0], v->ob[1], v->ob[2] };
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

        /* the fields the old loop leaves, in one pass over d */
        d->x = view_vec[0];
        d->y = view_vec[1];
        d->z = view_vec[2];
        d->w = -view_vec[2];
        d->_x = x;
        d->_y = y;
        d->_z = z;
        d->_w = w;
        d->u = U;
        d->v = V;
        d->color.r = cr;
        d->color.g = cg;
        d->color.b = cb;
        d->color.a = fog_alpha >= 0 ? (uint8_t) fog_alpha : v->cn[3];
        d->clip_rej = rej;
    }
}
#endif


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

/* Cycle 2 of a 2-cycle combiner, per vertex, with COMBINED = the cycle-1 result in
 * `out` and the texels taken as 1.0 like cycle 1 (the rasterizer multiplies the
 * texel in per pixel). Exact for the X * COMBINED family OoT uses everywhere. */
static inline int cc2_rgb_input(uint8_t mux, int slot, const struct RGBA *shade, int lod, const uint8_t *comb, int ch) {
    const uint8_t *prim = (const uint8_t *) &rdp.prim_color, *env = (const uint8_t *) &rdp.env_color;
    if (mux == G_CCMUX_COMBINED) return comb[ch];
    if (mux == G_CCMUX_TEXEL0 || mux == G_CCMUX_TEXEL1) return 255;
    if (mux == G_CCMUX_PRIMITIVE) return prim[ch];
    if (mux == G_CCMUX_SHADE) return ((const uint8_t *) shade)[ch];
    if (mux == G_CCMUX_ENVIRONMENT) return env[ch];
    if (slot == 2) { /* the C (multiplier) slot has its own alpha inputs */
        switch (mux) {
            case G_CCMUX_COMBINED_ALPHA: return comb[3];
            case G_CCMUX_TEXEL0_ALPHA: case G_CCMUX_TEXEL1_ALPHA: return 255;
            case G_CCMUX_PRIMITIVE_ALPHA: return prim[3];
            case G_CCMUX_SHADE_ALPHA: return shade->a;
            case G_CCMUX_ENV_ALPHA: return env[3];
            case G_CCMUX_LOD_FRACTION: return lod;
            case G_CCMUX_PRIM_LOD_FRAC: return rdp.prim_lod_frac;
            default: return 0;
        }
    }
    if (slot == 0 && mux == G_CCMUX_1) return 255;  /* A: 6 = 1, 7 = NOISE */
    if (slot == 0 && mux == G_CCMUX_NOISE) return 128;
    if (slot == 3 && mux == G_CCMUX_1) return 255;  /* D: 6 = 1 */
    return 0;
}

static inline int cc2_a_input(uint8_t mux, int slot, const struct RGBA *shade, int lod, int comb_a) {
    switch (mux) {
        case G_ACMUX_COMBINED: return slot == 2 ? lod : comb_a; /* 0 in C = LOD_FRACTION */
        case G_ACMUX_TEXEL0: case G_ACMUX_TEXEL1: return 255;
        case G_ACMUX_PRIMITIVE: return rdp.prim_color.a;
        case G_ACMUX_SHADE: return shade->a;
        case G_ACMUX_ENVIRONMENT: return rdp.env_color.a;
        case G_ACMUX_1: return slot == 2 ? rdp.prim_lod_frac : 255; /* 6 in C = PRIM_LOD_FRAC */
        default: return 0;
    }
}

static void gfx_eval_cycle2(const struct RGBA *shade, int lod, struct RGBA *out) {
    uint8_t comb[4] = { out->r, out->g, out->b, out->a };
    uint8_t *o = (uint8_t *) out;
    int ch;
    for (ch = 0; ch < 3; ch++) {
        if (rendering_state.lerp_v) { o[ch] = ((const uint8_t *) shade)[ch]; continue; } /* OOT_SHADELERP: the shade */
        int a = cc2_rgb_input(rdp.cc2[0], 0, shade, lod, comb, ch);
        int b = cc2_rgb_input(rdp.cc2[1], 1, shade, lod, comb, ch);
        int c = cc2_rgb_input(rdp.cc2[2], 2, shade, lod, comb, ch);
        int d = cc2_rgb_input(rdp.cc2[3], 3, shade, lod, comb, ch);
        int v = ((a - b) * c) / 255 + d;
        o[ch] = (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
    }
    {
        int a = cc2_a_input(rdp.cc2[4], 0, shade, lod, comb[3]);
        int b = cc2_a_input(rdp.cc2[5], 1, shade, lod, comb[3]);
        int c = cc2_a_input(rdp.cc2[6], 2, shade, lod, comb[3]);
        int d = cc2_a_input(rdp.cc2[7], 3, shade, lod, comb[3]);
        int v = ((a - b) * c) / 255 + d;
        o[3] = (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

static void gfx_eval_vertex_color(uint32_t cc_id, const struct RGBA *shade, int lod, struct RGBA *out) {
    uint8_t *o = (uint8_t *) out;
    int ch;

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
        o[ch] = (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
    }
    // An empty alpha combiner (alpha unused: cleared by the caller) means opaque.
    if ((cc_id & 0xfff000) == 0 && !(cc_id & (CC_FLAG_ALPHA_A_ONE | CC_FLAG_ALPHA_D_ONE))) {
        out->a = 255;
    }
    if (tri_state.cc2) {
        gfx_eval_cycle2(shade, lod, out);
    }
}

/* The same evaluation compiled per triangle state (gfx_tri_rebuild_state): each
 * combiner input of both cycles becomes an index into one byte table -- shade,
 * LOD and the cycle-1 result filled per vertex, the rest constant until the next
 * state command -- so a vertex costs four (eight with cycle 2) table-driven
 * (a - b) * c / 255 + d instead of the switch walk above (~500 instructions a
 * vertex under qemu). Same inputs, same integer arithmetic: bit-identical.
 * OOT_VCPROG=0 evaluates with gfx_eval_vertex_color, =2 runs both and logs any
 * difference. */
enum { VC_SHADE = 0, VC_LOD = 4, VC_COMB = 5, VC_ZERO = 9, VC_ONE = 10, VC_HALF = 11,
       VC_PRIM = 12, VC_ENV = 16, VC_PLF = 20, VC_N = 24 };
static struct {
    uint8_t src[VC_N];
    uint8_t c1[4][4], c2[4][4]; /* per output channel: the a b c d indices */
    bool a255, cc2;
    uint8_t n1[4][8], n2[4][8]; /* OOT_VCNEON: the same indices per slot, one lane a channel (lanes 4-7: 0) */
} sVc;
static int sVcMode = -1;

static uint8_t vc_slot1(uint8_t code, int one, int ch) { /* cc_eval_slot */
    switch (code) {
        case CC_0: return one ? VC_ONE : VC_ZERO;
        case CC_TEXEL0: case CC_TEXEL1: case CC_TEXEL0A: return VC_ONE;
        case CC_PRIM: return (uint8_t) (VC_PRIM + ch);
        case CC_SHADE: return (uint8_t) (VC_SHADE + ch);
        case CC_ENV: return (uint8_t) (VC_ENV + ch);
        case CC_LOD: return VC_LOD;
    }
    return VC_ZERO;
}
static uint8_t vc_rgb2(uint8_t mux, int slot, int ch) { /* cc2_rgb_input */
    if (mux == G_CCMUX_COMBINED) return (uint8_t) (VC_COMB + ch);
    if (mux == G_CCMUX_TEXEL0 || mux == G_CCMUX_TEXEL1) return VC_ONE;
    if (mux == G_CCMUX_PRIMITIVE) return (uint8_t) (VC_PRIM + ch);
    if (mux == G_CCMUX_SHADE) return (uint8_t) (VC_SHADE + ch);
    if (mux == G_CCMUX_ENVIRONMENT) return (uint8_t) (VC_ENV + ch);
    if (slot == 2) {
        switch (mux) {
            case G_CCMUX_COMBINED_ALPHA: return VC_COMB + 3;
            case G_CCMUX_TEXEL0_ALPHA: case G_CCMUX_TEXEL1_ALPHA: return VC_ONE;
            case G_CCMUX_PRIMITIVE_ALPHA: return VC_PRIM + 3;
            case G_CCMUX_SHADE_ALPHA: return VC_SHADE + 3;
            case G_CCMUX_ENV_ALPHA: return VC_ENV + 3;
            case G_CCMUX_LOD_FRACTION: return VC_LOD;
            case G_CCMUX_PRIM_LOD_FRAC: return VC_PLF;
            default: return VC_ZERO;
        }
    }
    if (slot == 0 && mux == G_CCMUX_1) return VC_ONE;
    if (slot == 0 && mux == G_CCMUX_NOISE) return VC_HALF;
    if (slot == 3 && mux == G_CCMUX_1) return VC_ONE;
    return VC_ZERO;
}
static uint8_t vc_a2(uint8_t mux, int slot) { /* cc2_a_input */
    switch (mux) {
        case G_ACMUX_COMBINED: return slot == 2 ? VC_LOD : VC_COMB + 3;
        case G_ACMUX_TEXEL0: case G_ACMUX_TEXEL1: return VC_ONE;
        case G_ACMUX_PRIMITIVE: return VC_PRIM + 3;
        case G_ACMUX_SHADE: return VC_SHADE + 3;
        case G_ACMUX_ENVIRONMENT: return VC_ENV + 3;
        case G_ACMUX_1: return slot == 2 ? VC_PLF : VC_ONE;
        default: return VC_ZERO;
    }
}
static void gfx_vc_compile(uint32_t cc_id) {
    int ch, s;
    memset(sVc.src, 0, sizeof(sVc.src));
    sVc.src[VC_ONE] = 255;
    sVc.src[VC_HALF] = 128;
    memcpy(&sVc.src[VC_PRIM], &rdp.prim_color, 4);
    memcpy(&sVc.src[VC_ENV], &rdp.env_color, 4);
    sVc.src[VC_PLF] = rdp.prim_lod_frac;
    for (ch = 0; ch < 4; ch++) {
        const int k = (ch == 3) ? 1 : 0;
        const uint32_t bits = cc_id >> (k * 12);
        sVc.c1[ch][0] = vc_slot1(bits & 7, (cc_id >> (28 + k)) & 1, ch);
        sVc.c1[ch][1] = vc_slot1((bits >> 3) & 7, 0, ch);
        sVc.c1[ch][2] = vc_slot1((bits >> 6) & 7, 0, ch);
        sVc.c1[ch][3] = vc_slot1((bits >> 9) & 7, (cc_id >> (30 + k)) & 1, ch);
    }
    sVc.a255 = (cc_id & 0xfff000) == 0 && !(cc_id & (CC_FLAG_ALPHA_A_ONE | CC_FLAG_ALPHA_D_ONE));
    sVc.cc2 = tri_state.cc2;
    for (ch = 0; ch < 3; ch++) {
        for (s = 0; s < 4; s++) sVc.c2[ch][s] = vc_rgb2(rdp.cc2[s], s, ch);
        if (rendering_state.lerp_v) { /* OOT_SHADELERP: (0 - 0) * 0 + SHADE, as gfx_eval_cycle2 */
            sVc.c2[ch][0] = sVc.c2[ch][1] = sVc.c2[ch][2] = VC_ZERO;
            sVc.c2[ch][3] = (uint8_t) (VC_SHADE + ch);
        }
    }
    for (s = 0; s < 4; s++) sVc.c2[3][s] = vc_a2(rdp.cc2[4 + s], s);
    memset(sVc.n1, 0, sizeof(sVc.n1));
    memset(sVc.n2, 0, sizeof(sVc.n2));
    for (ch = 0; ch < 4; ch++) {
        for (s = 0; s < 4; s++) {
            sVc.n1[s][ch] = sVc.c1[ch][s];
            sVc.n2[s][ch] = sVc.c2[ch][s];
        }
    }
}
static inline uint8_t vc_abcd(const uint8_t *src, const uint8_t *p) {
    const int v = ((src[p[0]] - src[p[1]]) * src[p[2]]) / 255 + src[p[3]];
    return (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
}
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
/* OOT_VCNEON (default on; =0 the scalar vc_abcd per channel): the four channels of one cycle at once, the
 * same integer arithmetic. The device profile had gfx_vc_eval at ~0.9% of the basement frame, nearly all in
 * vc_abcd's byte gathers and signed divides. Each slot's four bytes come from the 24-byte source table by
 * VTBL (indices < VC_N); (a - b) * c widened to 32 bits as in C (|.| <= 65025); C's division by 255 (toward
 * zero) as sign * ((|x| * 0x8081) >> 23), which equals |x| / 255 for every |x| <= 65025 (dl_test vcdiv checks
 * all of them); + d; the clamp to 0..255 by saturating narrows. The source table is written exactly as the
 * scalar code writes it (shade, lod, the cycle-1 result before cycle 2). */
static int sVcNeon SAVESTATE_EXCLUDE = -1;
static inline uint8x8_t vc_cycle_neon(const uint8_t *src, const uint8_t (*n)[8]) {
    uint8x8x3_t t;
    t.val[0] = vld1_u8(src);
    t.val[1] = vld1_u8(src + 8);
    t.val[2] = vld1_u8(src + 16);
    const uint8x8_t a = vtbl3_u8(t, vld1_u8(n[0])), b = vtbl3_u8(t, vld1_u8(n[1]));
    const uint8x8_t c = vtbl3_u8(t, vld1_u8(n[2])), d = vtbl3_u8(t, vld1_u8(n[3]));
    const int16x4_t ab = vget_low_s16(vreinterpretq_s16_u16(vsubl_u8(a, b)));
    const int16x4_t c16 = vget_low_s16(vreinterpretq_s16_u16(vmovl_u8(c)));
    const int32x4_t x = vmull_s16(ab, c16);
    const int32x4_t sgn = vshrq_n_s32(x, 31); /* 0 or -1 */
    const uint32x4_t q = vshrq_n_u32(vmulq_u32(vreinterpretq_u32_s32(vabsq_s32(x)), vdupq_n_u32(0x8081u)), 23);
    const int32x4_t qs = vsubq_s32(veorq_s32(vreinterpretq_s32_u32(q), sgn), sgn); /* x / 255, toward zero */
    const int32x4_t v = vaddq_s32(qs, vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(d)))));
    const uint16x4_t v16 = vqmovun_s32(v); /* < 0 -> 0 */
    return vqmovn_u16(vcombine_u16(v16, v16)); /* > 255 -> 255 */
}
#endif
static void gfx_vc_eval(const struct RGBA *shade, int lod, struct RGBA *out) {
    uint8_t *const src = sVc.src, *const o = (uint8_t *) out;
    memcpy(&src[VC_SHADE], shade, 4);
    src[VC_LOD] = (uint8_t) lod; /* 0..255 */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (sVcNeon < 0) { const char *e = getenv("OOT_VCNEON"); sVcNeon = !(e && e[0] == '0'); }
    if (sVcNeon) {
        uint8_t r[8];
        vst1_u8(r, vc_cycle_neon(src, (const uint8_t (*)[8]) sVc.n1));
        o[0] = r[0];
        o[1] = r[1];
        o[2] = r[2];
        o[3] = sVc.a255 ? 255 : r[3];
        if (sVc.cc2) {
            memcpy(&src[VC_COMB], o, 4);
            vst1_u8(r, vc_cycle_neon(src, (const uint8_t (*)[8]) sVc.n2));
            o[0] = r[0];
            o[1] = r[1];
            o[2] = r[2];
            o[3] = r[3];
        }
        return;
    }
#endif
    o[0] = vc_abcd(src, sVc.c1[0]);
    o[1] = vc_abcd(src, sVc.c1[1]);
    o[2] = vc_abcd(src, sVc.c1[2]);
    o[3] = sVc.a255 ? 255 : vc_abcd(src, sVc.c1[3]);
    if (sVc.cc2) {
        memcpy(&src[VC_COMB], o, 4);
        o[0] = vc_abcd(src, sVc.c2[0]);
        o[1] = vc_abcd(src, sVc.c2[1]);
        o[2] = vc_abcd(src, sVc.c2[2]);
        o[3] = vc_abcd(src, sVc.c2[3]);
    }
}
static void gfx_vc_eval_checked(uint32_t cc_id, const struct RGBA *shade, int lod, struct RGBA *out) {
    struct RGBA ref;
    gfx_eval_vertex_color(cc_id, shade, lod, &ref);
    gfx_vc_eval(shade, lod, out);
    if (memcmp(&ref, out, 4) != 0) {
        static int sLogs;
        if (sLogs < 20) {
            sLogs++;
            port_log("vcprog: cc %08X cc2 %d shade %02X%02X%02X%02X lod %d: ref %02X%02X%02X%02X prog %02X%02X%02X%02X\n",
                     (unsigned) cc_id, (int) tri_state.cc2, shade->r, shade->g, shade->b, shade->a, lod,
                     ref.r, ref.g, ref.b, ref.a, out->r, out->g, out->b, out->a);
        }
        *out = ref;
    }
}



/* The vertex alpha gfx_emit_vertex hands the rasterizer for shade alpha a (texels count as 1). */
static uint8_t gfx_fog_probe_alpha(uint32_t cc_id, uint8_t a, int lod) {
    struct RGBA shade = { 90, 140, 200, a }, out;
    switch (tri_state.color_mode) {
        case 0: return tri_state.alpha_255 ? 255 : a;
        case 1: return tri_state.const_color.a;
        default:
            if (sVcMode == 1) gfx_vc_eval(&shade, lod, &out);
            else gfx_eval_vertex_color(cc_id, &shade, lod, &out);
            return out.a;
    }
}

/* Fog: ONE reference for every port (MK64 7d23507: SM64 de0b5af's rules on MK64 c400d1b's fast
 * paths; the ports lead, round 2). The RDP blends the fog colour in after the combiner,
 * lerp(combined, fog, shade alpha); the fold in gfx_emit_vertex (the vertex colour lerped toward
 * the fog before the texture multiply) is exact only for black fog on a modulate. OoT's fog is
 * coloured in 43 of 45 scenes (the R35 census), and a combiner that never reads the vertex colour
 * (a decal) got no fog from the fold at all. Everywhere else the vertex alpha carries the fog
 * factor and gfx_soft fogs each pixel, when the vertex alpha is free for it: an opaque draw, an
 * alpha from the texel alone, from the shade alone (on the N64 the shade alpha IS the factor, so
 * the evaluated alpha is kept) or a constant, which the backend gets on its own (fog_a). Texel x
 * constant and shade x constant alphas keep the fold. OoT evaluates cycle 2 into the vertex
 * colour (texels as 1), so its alpha is classified by probing that evaluation -- identity,
 * constant or mixed -- and the combine flags are the rasterizer's own (gfx_soft_combine_flags).
 * OOT_FOGPX=0: always fold; 1: MK64 c400d1b's rule (coloured fog, the alpha unused or the
 * texel's alone); 2 (default): this rule. */
/* OOT_FOGTEXA=0: a texel x constant alpha keeps the fold, as before. */
static bool gfx_fog_texa_on(void) {
    static int sOn SAVESTATE_EXCLUDE = -1;
    if (sOn < 0) { const char *e = getenv("OOT_FOGTEXA"); sOn = !(e && e[0] == '0'); }
    return sOn != 0;
}
static void gfx_fog_plan(uint32_t cc_id, bool use_alpha, bool use_fog) {
    static int sFogPx SAVESTATE_EXCLUDE = -1;
    extern void gfx_soft_combine_flags(bool *rgb_in, bool *a_tex, bool *a_in);
    extern void gfx_soft_set_fog(bool on, int r, int g, int b, int a);
    if (sFogPx < 0) { const char *e = getenv("OOT_FOGPX"); sFogPx = (e && e[0] >= '0' && e[0] <= '2') ? e[0] - '0' : 2; }
    bool fog_px = false, fog_raw = false, decal = false;
    int fog_a = -1;
    if (use_fog) {
        const bool black = (rdp.fog_color.r | rdp.fog_color.g | rdp.fog_color.b) == 0;
        bool rgb_in, a_tex, a_in;
        gfx_soft_combine_flags(&rgb_in, &a_tex, &a_in);
        a_tex = a_tex && tri_state.use_texture;
        decal = !rgb_in; /* the fold never reaches the pixel */
        if (sFogPx == 1) {
            fog_px = !black && (!use_alpha || (a_tex && !a_in));
            fog_raw = fog_px;
        } else if (sFogPx == 2) {
            /* black fog on the vertex colour or texture x vertex colour: the fold is exact */
            const bool fold_exact = black && !decal && (!tri_state.use_texture || !(tri_state.tex_add || rendering_state.tex_lerp));
            if (!fold_exact) {
                if (!use_alpha || (a_tex && !a_in)) {
                    fog_px = fog_raw = true;   /* the vertex alpha is unused */
                } else if (a_in) {
                    /* the vertex alpha as a function of the shade alpha: identity, constant or mixed
                     * (probed only here: ~1% of the instructions when every fogged rebuild did it) */
                    const uint8_t g0 = gfx_fog_probe_alpha(cc_id, 0, 0), g1 = gfx_fog_probe_alpha(cc_id, 77, 0);
                    const uint8_t g2 = gfx_fog_probe_alpha(cc_id, 255, 0);
                    const bool lod_free = !tri_state.needs_lod || (gfx_fog_probe_alpha(cc_id, 77, 128) == g1 &&
                                                                     gfx_fog_probe_alpha(cc_id, 0, 255) == g0);
                    if (lod_free && g0 == 0 && g1 == 77 && g2 == 255) {
                        fog_px = true;         /* the shade alpha, i.e. the factor, kept */
                    } else if (lod_free && g0 == g1 && g1 == g2 && (!a_tex || gfx_fog_texa_on())) {
                        /* a constant alpha, sent on its own; with a texel alpha the rasterizer multiplies
                         * the texel in (Goron City's wall mural: TEXEL0 * PRIM alpha under light-brown fog
                         * kept the fold, which only fogs a black fog right -- the dark mural stayed dark
                         * where the N64 fades it into the haze; the tour vs mupen z64, 2026-09-28) */
                        fog_px = fog_raw = true;
                        fog_a = g0;
                    }
                }
            }
        }
    }
    tri_state.fog_px = fog_px;
    tri_state.fog_raw = fog_raw;
    tri_state.fog_class = (uint8_t) ((fog_px ? (fog_a >= 0 ? 2 : 1) : 0) + (decal ? 4 : 0));
    if (fog_px != rendering_state.fog_px ||
        (fog_px && (rdp.fog_color.r != rendering_state.fog_c.r || rdp.fog_color.g != rendering_state.fog_c.g ||
                    rdp.fog_color.b != rendering_state.fog_c.b || fog_a != rendering_state.fog_a))) {
        gfx_flush();
        gfx_soft_set_fog(fog_px, rdp.fog_color.r, rdp.fog_color.g, rdp.fog_color.b, fog_a);
        rendering_state.fog_px = fog_px;
        rendering_state.fog_c = rdp.fog_color;
        rendering_state.fog_a = fog_a;
    }
}

uint32_t gfx_prof_rebuilds;
static void gfx_tri_rebuild_state(struct LoadedVertex *v1) {
    gfx_prof_rebuilds++;
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
    uint32_t cc_id = rdp.combine_mode;
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
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    bool use_noise = (rdp.other_mode_l & G_AC_DITHER) == G_AC_DITHER;
    if (texture_edge) {
        use_alpha = true;
    }
    {   /* TEX_EDGE without FORCE_BL: written unblended (gfx_soft_set_edge_opaque). OOT_EDGEOPAQUE=0: old. */
        static int sEdgeOp = -1;
        extern void gfx_soft_set_edge_opaque(bool on);
        if (sEdgeOp < 0) { const char *e = getenv("OOT_EDGEOPAQUE"); sEdgeOp = !(e != NULL && e[0] == '0'); }
        const bool eo = sEdgeOp && texture_edge && !(rdp.other_mode_l & FORCE_BL);
        if (eo != rendering_state.edge_opaque) {
            gfx_flush();
            gfx_soft_set_edge_opaque(eo);
            rendering_state.edge_opaque = eo;
        }
    }
    {   /* OOT_FILLRULE (gfx_soft.c own_edge): a blended or alpha-tested draw without AA_EN -- the RDP writes such a
         * pixel only when one fixed sample of it is covered, so each pixel along an edge two triangles share is drawn
         * once (the flames over Ganondorf in the Deku Tree's vision showed the clip fan's diagonal, blended twice).
         * With AA_EN both triangles write the pixel they share (partial coverage each): left as it was.
         * OOT_FILLRULE=0: every draw takes the inclusive row solve, as before. */
        static int sFillRule = -1;
        extern void gfx_soft_set_own_edges(bool on);
        if (sFillRule < 0) { const char *e = getenv("OOT_FILLRULE"); sFillRule = !(e != NULL && e[0] == '0'); }
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
    const int tpe_plan = gfx_tpe_plan(use_alpha);
    if (tpe_plan & 1) { /* cycle 1's rgb comes pre-evaluated in the texel */
        cc_id = (cc_id & ~(0xfffu | CC_FLAG_RGB_A_ONE | CC_FLAG_RGB_D_ONE)) | ((uint32_t) CC_TEXEL0 << 9);
    }
    if (tpe_plan & 2) { /* and its alpha */
        cc_id = (cc_id & ~(0xfff000u | CC_FLAG_ALPHA_A_ONE | CC_FLAG_ALPHA_D_ONE)) | ((uint32_t) CC_TEXEL0 << 21);
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
            port_log("  tri state cc %08X cc2 %d [%u %u %u %u] othermode_l %08X h %08X prim %02X%02X%02X%02X env %02X%02X%02X%02X\n", (unsigned) cc_id, (int) tri_state.cc2, rdp.cc2[0], rdp.cc2[1], rdp.cc2[2], rdp.cc2[3], (unsigned) rdp.other_mode_l, (unsigned) rdp.other_mode_h, rdp.prim_color.r, rdp.prim_color.g, rdp.prim_color.b, rdp.prim_color.a, rdp.env_color.r, rdp.env_color.g, rdp.env_color.b, rdp.env_color.a);
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
    bool uses_t1 = (used_textures[0] || used_textures[1]) && gfx_cc1_uses_texel1();
    for (int pass = 0; pass < 2; pass++) {
    const uint32_t epoch = gfx_cache_epoch;
    for (int i = 0; i < 2; i++) {
        if (used_textures[i] || uses_t1) {
            if (rdp.textures_changed[i]) {
                gfx_flush();
                if (i == 1 && gfx_tile1_importable()) {
                    import_texture_tile1();
                } else {
                    import_texture(i);
                }
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
    if (epoch == gfx_cache_epoch || !gfx_epochfix_on()) break; /* no reset under the imports */
    }
    // The GE has one texture unit: whatever was bound last (possibly tile 1's
    // mip level, or a texture just uploaded) must give way to tile 0's texture.
    uint32_t blend_w = 0, blend_h = 0, blend_id = 0;
    bool detail = false;
    gfx_detail_cand = false;
    if (tpe_plan != 0) {
        blend_id = gfx_tpe_tiles(tpe_plan, &blend_w, &blend_h);
        if (blend_id == 0 && (tpe_plan & 1) && uses_t1 && gfx_cc1_tex_multiply()) {
            /* TEXEL0 x TEXEL1 too big to bake (MM's painted walls): sampled per pixel; the cc_id's cycle-1 colour is
             * already TEXEL0 (tpe_plan). A tile 1 clamped inside its mask (gfx_detail_why 6) still draws as tile 0. */
            detail = gfx_detail_tiles_mode(true);
            if (gfx_debug_frame && !detail) port_log("  multiply detail refused: why %d\n", gfx_detail_why);
        }
        if (gfx_cc1_uses_texel1() && gfx_tileshift_on() && (gfx_tile1_shift(false) != 0 || gfx_tile1_shift(true) != 0)) {
            gfx_detail_cand = true; /* census: a per-texel combiner over a tile 1 at another scale */
            gfx_detail_why = 9;
        }
    } else if (uses_t1) {
        detail = gfx_detail_tiles();
        if (!detail) blend_id = gfx_blend_tiles(&blend_w, &blend_h);
    }
    if (!detail) gfx_detail_set(0, 0, 0, 0, 0, NULL, 0, 0, 0, 0);
    if (blend_id != 0) {
        gfx_select_tex0(blend_id);
        gfx_rapi->set_sampler_parameters(0, (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT,
                                         rdp.texture_tile.cms, rdp.texture_tile.cmt);
    } else if ((used_textures[0] || used_textures[1]) && rendering_state.textures[0] != NULL) {
        gfx_select_tex0(rendering_state.textures[0]->texture_id);
    }

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
        if (blend_id != 0) {
            tex_width = (float) blend_w;
            tex_height = (float) blend_h;
            tri_state.mirror = 0;
        }
        if (tri_state.mirror & 1) tex_width *= 2.0f;
        if (tri_state.mirror & 2) tex_height *= 2.0f;
        tri_state.drop_u = tri_state.drop_v = 1.0f;
        if (detail && gfx_detail_cur.id != 0) {
            int w1, h1;
            if (gfx_soft_tex_size(gfx_detail_cur.id, &w1, &h1)) {
                tri_state.drop_u = gfx_detail_drop(tex_width, gfx_detail_cur.es, w1);
                tri_state.drop_v = gfx_detail_drop(tex_height, gfx_detail_cur.et, h1);
                /* a clamped tile 1 reads absolute coordinates: no rebase on that axis */
                if (gfx_detail_cur.clamp & 1) tri_state.drop_u = 1e9f;
                if (gfx_detail_cur.clamp & 2) tri_state.drop_v = 1e9f;
            }
        }
        float filt = ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) ? 0.5f : 0.0f;
        float inv_w = 1.0f / tex_width, inv_h = 1.0f / tex_height;
        /* The render tile's shift scales S/T before the tile corner is subtracted
         * (the RDP: 1..10 = >> n, 11..15 = << 16 - n). OoT's N64 logo reflection
         * tile shifts S by 2 and T by 1: unshifted, every face sampled 4x / 2x too
         * far into the texture. Off by default for now: see gfx_tileshift_on. */
        tri_state.u_scale = inv_w / 32.0f * gfx_tile_shift_scale(rdp.tiles[G_TX_RENDERTILE].shifts);
        tri_state.v_scale = inv_h / 32.0f * gfx_tile_shift_scale(rdp.tiles[G_TX_RENDERTILE].shiftt);
        tri_state.u_off = (filt - rdp.texture_tile.uls * 8 / 32.0f) * inv_w;
        tri_state.v_off = (filt - rdp.texture_tile.ult * 8 / 32.0f) * inv_h;
    }
    // Classify the combiner once: does the vertex colour depend on the shade?
    tri_state.cc2 = false; /* classify cycle 1 alone, not with the last triangle's cycle 2 */
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
    tri_state.cc2 = two_cycle && !rdp.cc2_pass;
    // Cycle 2 is folded into the vertex colour, so the rasterizer must modulate
    // by it even when cycle 1 alone has no vertex input (OoT's two-texture room
    // combiners: lerp(TEXEL0, TEXEL1) in cycle 1, * SHADE in cycle 2).
    if (tri_state.cc2 != rendering_state.force_in) {
        extern void gfx_scegu_set_force_in(bool on);
        gfx_flush();
        gfx_scegu_set_force_in(tri_state.cc2);
        rendering_state.force_in = tri_state.cc2;
    }
    gfx_update_tex_lerp(cc_id, tri_state.cc2);
    tri_state.color_mode = tri_state.cc2 ? 2 : comb->color_mode;
    tri_state.alpha_255 = comb->alpha_255;
    tri_state.needs_lod = comb->needs_lod;
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
            if ((y->r | y->g | y->b) != 0 && !rendering_state.tex_lerp) {
                add = true;
            }
        }
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
    if (sVcMode < 0) {
        const char *e = getenv("OOT_VCPROG");
        sVcMode = e ? atoi(e) : 1;
    }
    if (tri_state.color_mode == 2 && sVcMode) {
        gfx_vc_compile(tri_state.cc_id);
    }
    gfx_fog_plan(cc_id, use_alpha, use_fog);
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
        /* z/w is -1 (near) .. 1 (far), as the RSP's viewport maps it; the
         * rasterizer's z-buffer is 0..1. Passed straight through (OOT_NEARCLIP=0),
         * everything nearer than z/w = 0 -- ~2x OoT's near plane of 10 units --
         * clamped to depth 0 and lost its ordering. */
        out->z = gfx_nearclip_on() ? cv->_z * inv * 0.5f + 0.5f : cv->_z * inv;
        /* NoN: nearer than the near plane z goes below 0; the rasterizer clamps it per pixel
         * (clamp16_sh8), as the RDP does, so the depth still interpolates exactly. */
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
            if (sVcMode == 1) gfx_vc_eval(&cv->color, lod, &out->color);
            else if (sVcMode == 0) gfx_eval_vertex_color(cc_id, &cv->color, lod, &out->color);
            else gfx_vc_eval_checked(cc_id, &cv->color, lod, &out->color);
            break;
    }
    if (tri_state.fog_px) {
        // The fog factor: gfx_soft fogs each pixel (gfx_fog_plan). With a shade
        // alpha the evaluated vertex alpha already is it.
        if (tri_state.fog_raw) out->color.a = cv->color.a;
    } else if (tri_state.use_fog) {
        // Blender cycle 1 of the fog render modes: colour = lerp(colour, fog
        // colour, shade alpha) -- the RSP fog factor with G_FOG on, otherwise
        // the model's vertex alpha (Moo Moo Farm darkens its dirt that way).
        // Folded into the vertex colour before the texture multiply: exact
        // for dark fog colours, slightly darker than the N64 for bright ones.
        int a = cv->color.a, ia = 255 - a;
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
            // (a doubled mirrored texture has period 1 in normalised units, like a plain repeat;
            // with a detail layer the step is gfx_detail_drop's, 1 otherwise -- bit-identical)
            const float su = ((rdp.texture_tile.cms & G_TX_MIRROR) && !(tri_state.mirror & 1) ? 2.0f : 1.0f) * tri_state.drop_u;
            const float sv = ((rdp.texture_tile.cmt & G_TX_MIRROR) && !(tri_state.mirror & 2) ? 2.0f : 1.0f) * tri_state.drop_v;
            float offu = ((rdp.texture_tile.cms & G_TX_CLAMP) || su >= 1e8f) ? 0.0f : su == 1.0f ? port_floorf(minu) : su * port_floorf(minu / su);
            float offv = ((rdp.texture_tile.cmt & G_TX_CLAMP) || sv >= 1e8f) ? 0.0f : sv == 1.0f ? port_floorf(minv) : sv * port_floorf(minv / sv);
            if (offu != 0.0f || offv != 0.0f) {
                for (i = 0; i < 3; i++) {
                    first[i].u -= offu;
                    first[i].v -= offv;
                }
            }
        }
#endif
    }
    buf_tri_twice[buf_vbo_num_tris] = gfx_tri_twice_next;
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

#ifndef HANDHELD
/* OOT_CCCENSUS=1 (desktop debug): count the triangles whose combiner uses an
 * input the cycle-1 model (color_comb_component) drops -- RGB C = an alpha
 * input (COMBINED/TEXEL1/PRIM/SHADE/ENV alpha), PRIM_LOD_FRAC or K5, RGB B =
 * CENTER/K4, RGB A = NOISE, alpha C = LOD_FRACTION / PRIM_LOD_FRAC, each only
 * where it matters (A != B) -- plus CI textures drawn under an IA16 TLUT and
 * G_LOADTLUTs with a non-zero start texel. Printed per combiner at exit.
 * (GoldenEye's kit fixes, 2026-09-24: which of them OoT's display lists hit.) */
static int sCensus = -1;
static struct { uint8_t m[16]; uint8_t cyc; uint32_t tris; } sCen[96];
static int sCenN;
static uint32_t sCenIa16Tris, sCenTlutLoads, sCenTlutOffset, sCenTris;
/* Blender B = 1-A in the last cycle without FORCE_BL (GoldenEye cb5c863: the
 * RDP then blends only partly covered edge pixels; the kit blends every
 * pixel by alpha), per othermode_l. */
static struct { uint32_t oml, tris; } sCenNfb[32];
static int sCenNfbN;
static uint32_t sCenNfbTris;
static uint32_t sCenRects, sCenRectWrap; /* texrects, and those gfx_sp_tri1_2d shifts by whole mask periods */
static uint32_t sCenTexrects, sCenTexrectFrac; /* 1/2-cycle texrects, and those with a fractional edge (GE 9e9b6ed) */
static uint32_t sCenLoadBlocks, sCenLoadBlockDxt0[4]; /* LoadBlocks, and those with dxt 0 by texel size (DKR be3afca) */
static uint32_t sCenDetail[11]; /* gfx_detail_cand triangles by gfx_detail_why (9 = the TPE's) */
static uint32_t sCenFog[2]; /* R35: fogged triangles with black / coloured fog (the vertex fold is exact only for black) */
static uint32_t sCenFogDecal[2]; /* ... decals (the combiner never reads the vertex colour): black / coloured fog */
static uint32_t sCenFogPlan[3], sCenFogSome;
static uint32_t sCenZimgRects; /* R34: texrects drawn while the colour image is the Z image */
static uint32_t sCenR23Moved;  /* R23 (b): scissors whose edges moved to pixel centres */ /* gfx_fog_plan: folded / per pixel / per pixel + constant alpha; tris with fog > 0 */
static void cc_census_print(void) {
    int i, j;
    fprintf(stderr, "cccensus: R24 %u R25 %u R26 %u R27 %u tris (of %u); R27 loads overwriting a load %u; R28 inverted scissors %u of %u, %u tris under one\n",
            (unsigned) sCenR24Tris, (unsigned) sCenR25Tris, (unsigned) sCenR26Tris, (unsigned) sCenR27Tris, (unsigned) sCenTris,
            (unsigned) sCenR27Loads, (unsigned) sCenR28Inv, (unsigned) sCenR28Sc, (unsigned) sCenR28Tris);
    fprintf(stderr, "cccensus: %u tris, %u IA16-TLUT CI tris, %u of %u LOADTLUTs with a start texel\n",
            (unsigned) sCenTris, (unsigned) sCenIa16Tris, (unsigned) sCenTlutOffset, (unsigned) sCenTlutLoads);
    fprintf(stderr, "cccensus: %u tris blend 1-A without FORCE_BL\n", (unsigned) sCenNfbTris);
    fprintf(stderr, "cccensus: fog: %u tris black, %u tris coloured\n", (unsigned) sCenFog[0], (unsigned) sCenFog[1]);
    fprintf(stderr, "cccensus: fog decals: %u tris black, %u tris coloured; plan: %u folded, %u per pixel, %u per pixel + constant alpha; %u with fog > 0\n",
            (unsigned) sCenFogDecal[0], (unsigned) sCenFogDecal[1], (unsigned) sCenFogPlan[0], (unsigned) sCenFogPlan[1],
            (unsigned) sCenFogPlan[2], (unsigned) sCenFogSome);
    fprintf(stderr, "cccensus: zimg texrects (R34) %u; scissors moved to pixel centres (R23) %u\n", (unsigned) sCenZimgRects,
            (unsigned) sCenR23Moved);
    fprintf(stderr, "cccensus: detail layer: %u tris on; off: %u no switch, %u combiner form, %u texel-alpha lerp, "
            "%u scale range, %u tile 1 clamps, %u texture, %u mirror import, %u rasterizer, %u per-texel (TPE)\n",
            (unsigned) sCenDetail[0], (unsigned) sCenDetail[2], (unsigned) sCenDetail[3], (unsigned) sCenDetail[4],
            (unsigned) sCenDetail[5], (unsigned) sCenDetail[6], (unsigned) sCenDetail[7], (unsigned) sCenDetail[10],
            (unsigned) sCenDetail[8], (unsigned) sCenDetail[9]);
    fprintf(stderr, "cccensus: %u of %u texrects shifted by whole mask periods\n", (unsigned) sCenRectWrap, (unsigned) sCenRects);
    fprintf(stderr, "cccensus: %u of %u 1/2-cycle texrects with a fractional edge\n", (unsigned) sCenTexrectFrac, (unsigned) sCenTexrects);
    fprintf(stderr, "cccensus: %u of %u LoadBlocks with dxt 0 (4b %u 8b %u 16b %u 32b %u)\n",
            (unsigned) (sCenLoadBlockDxt0[0] + sCenLoadBlockDxt0[1] + sCenLoadBlockDxt0[2] + sCenLoadBlockDxt0[3]),
            (unsigned) sCenLoadBlocks, (unsigned) sCenLoadBlockDxt0[0], (unsigned) sCenLoadBlockDxt0[1],
            (unsigned) sCenLoadBlockDxt0[2], (unsigned) sCenLoadBlockDxt0[3]);
    for (i = 0; i < sCenNfbN; i++) {
        fprintf(stderr, "cccensus: nofbl %08X %u tris\n", (unsigned) sCenNfb[i].oml, (unsigned) sCenNfb[i].tris);
    }
    for (i = 0; i < sCenN; i++) {
        for (j = i + 1; j < sCenN; j++) {
            if (sCen[j].tris > sCen[i].tris) {
                const __typeof__(sCen[0]) t = sCen[i];
                sCen[i] = sCen[j];
                sCen[j] = t;
            }
        }
        fprintf(stderr, "cccensus: %7u tris cyc%u c1 rgb %u %u %u %u a %u %u %u %u | c2 rgb %u %u %u %u a %u %u %u %u\n",
                (unsigned) sCen[i].tris, (unsigned) sCen[i].cyc + 1, sCen[i].m[0], sCen[i].m[1], sCen[i].m[2],
                sCen[i].m[3], sCen[i].m[4], sCen[i].m[5], sCen[i].m[6], sCen[i].m[7], sCen[i].m[8], sCen[i].m[9],
                sCen[i].m[10], sCen[i].m[11], sCen[i].m[12], sCen[i].m[13], sCen[i].m[14], sCen[i].m[15]);
    }
}
static bool cc_census_on(void) {
    if (sCensus < 0) {
        const char *e = getenv("OOT_CCCENSUS");
        sCensus = e != NULL && e[0] == '1';
        if (sCensus) atexit(cc_census_print);
    }
    return sCensus != 0;
}
static void cc_census_tri(void) {
    const uint8_t *m = rdp.cc1;
    const uint8_t cyc = (uint8_t) ((rdp.other_mode_h >> G_MDSFT_CYCLETYPE) & 3);
    bool odd;
    int i;

    if (!cc_census_on()) return;
    sCenTris++;
    {   /* R24-R28 per triangle */
        const uint8_t *c2 = rdp.cc2;
#define CEN_TEX(m, u) ((m)[0] == (u) || (m)[1] == (u) || (m)[2] == (u) || (m)[2] == (u) + 7 || (m)[3] == (u) || \
                       (m)[4] == (u) || (m)[5] == (u) || (m)[6] == (u) || (m)[7] == (u))
        const bool t0 = CEN_TEX(m, 1) || (cyc == 1 && CEN_TEX(c2, 1));
        if (cyc == 1 && (CEN_TEX(c2, 1) || CEN_TEX(c2, 2))) sCenR25Tris++;
#undef CEN_TEX
        if (t0) {
            const int slot = gfx_tex_slot(0);
            if (rdp.loaded_texture[slot].stride_bytes == 0) {
                const int ew = ((int) rdp.texture_tile.lrs - (int) rdp.texture_tile.uls + 4) / 4;
                const int eh = ((int) rdp.texture_tile.lrt - (int) rdp.texture_tile.ult + 4) / 4;
                const uint8_t ms = rdp.texture_tile.masks, mt = rdp.texture_tile.maskt;
                if ((ms && ms < 16 && !(rdp.texture_tile.cms_raw & G_TX_CLAMP) && ew < (1 << ms)) ||
                    (mt && mt < 16 && !(rdp.texture_tile.cmt_raw & G_TX_CLAMP) && eh < (1 << mt))) sCenR24Tris++;
            }
            if (sCenR26Dirty) sCenR26Tris++;
            for (int j = 0; j < GFX_LT_SLOTS; j++) { /* R27: the render tile starts inside a load */
                const uint32_t lo = rdp.loaded_texture[j].tmem, hi = lo + (rdp.loaded_texture[j].size_bytes + 7) / 8;
                if (rdp.loaded_texture[j].valid && rdp.loaded_texture[j].tmem == rdp.texture_tile.tmem) break;
                if (rdp.loaded_texture[j].valid && rdp.texture_tile.tmem > lo && rdp.texture_tile.tmem < hi) { sCenR27Tris++; break; }
            }
        }
        if (sCenR28Now) sCenR28Tris++;
    }
    if (rdp.texture_tile.fmt == G_IM_FMT_CI && (rdp.other_mode_h & (3U << G_MDSFT_TEXTLUT)) == G_TT_IA16) {
        sCenIa16Tris++;
    }
    {
        const uint32_t oml = rdp.other_mode_l;
        const uint32_t b = cyc == 1 ? (oml >> 16) & 3 : (oml >> 18) & 3;
        if (b == G_BL_1MA && !(oml & FORCE_BL) && !(oml & CVG_X_ALPHA)) {
            sCenNfbTris++;
            for (i = 0; i < sCenNfbN && sCenNfb[i].oml != oml; i++) {
            }
            if (i == sCenNfbN && sCenNfbN < 32) {
                sCenNfb[sCenNfbN].oml = oml;
                sCenNfb[sCenNfbN++].tris = 0;
            }
            if (i < sCenNfbN) sCenNfb[i].tris++;
        }
    }
    odd = (m[0] != m[1] && (m[2] == G_CCMUX_COMBINED_ALPHA || m[2] == G_CCMUX_TEXEL1_ALPHA ||
                            m[2] == G_CCMUX_PRIMITIVE_ALPHA || m[2] == G_CCMUX_SHADE_ALPHA ||
                            m[2] == G_CCMUX_ENV_ALPHA || m[2] == G_CCMUX_PRIM_LOD_FRAC || m[2] == G_CCMUX_K5)) ||
          (m[2] != G_CCMUX_0 && (m[1] == G_CCMUX_CENTER || m[1] == G_CCMUX_K4 || m[0] == G_CCMUX_NOISE)) ||
          (m[4] != m[5] && (m[6] == G_ACMUX_LOD_FRACTION || m[6] == G_ACMUX_PRIM_LOD_FRAC));
    if (!odd) return;
    for (i = 0; i < sCenN; i++) {
        if (sCen[i].cyc == cyc && memcmp(sCen[i].m, m, 8) == 0 && memcmp(sCen[i].m + 8, rdp.cc2, 8) == 0) break;
    }
    if (i == sCenN) {
        if (sCenN == (int) (sizeof(sCen) / sizeof(sCen[0]))) return;
        memcpy(sCen[i].m, m, 8);
        memcpy(sCen[i].m + 8, rdp.cc2, 8);
        sCen[i].cyc = cyc;
        sCenN++;
    }
    sCen[i].tris++;
}
#endif

static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
#ifdef PORT_EXP_NOTRI
    return;
#endif
#ifndef HANDHELD
    cc_census_tri();
#endif
#ifdef PORT_PROFILE_DL
    uint32_t _pt0 = port_time_us();
#endif
    struct LoadedVertex *v1 = &rsp.loaded_vertices[vtx1_idx];
    struct LoadedVertex *v2 = &rsp.loaded_vertices[vtx2_idx];
    struct LoadedVertex *v3 = &rsp.loaded_vertices[vtx3_idx];
    struct LoadedVertex *v_arr[3] = {v1, v2, v3};

    if (v1->clip_rej & v2->clip_rej & v3->clip_rej & ~(XY_OUT | GB_OUT)) {
        // The whole triangle lies outside the guard band (the exact side
        // planes reject below, after the phase bookkeeping)
        return;
    }
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
    /* Wholly beyond one side of the view (exact planes, the same x the
     * rasterizer projects): it would rasterize no pixel -- raster_tri clamps
     * to the viewport -- so skip the emit, state rebuild and setup. The
     * guard-band reject above only catches triangles 3 half-screens out.
     * After the phase bookkeeping, so target selection is unchanged.
     * (MK64 perf round 24: 44% of emitted triangles on DK Jungle.) */
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
            float wmin = v1->_w; if (v2->_w < wmin) wmin = v2->_w; if (v3->_w < wmin) wmin = v3->_w;
            // Clip when a vertex is near/behind the eye (w < W_MIN: the
            // behind-camera road/checkered garbage) or the polygon spans to
            // beyond the far plane (the stretched wedge during turns).
            if (gfx_nearclip_on()) {
                // In front of the game's near plane or behind the eye (both set
                // Z_POS), or beyond the far plane (Z_NEG): clip, as the RSP does.
                if ((v1->clip_rej | v2->clip_rej | v3->clip_rej) & (Z_POS | Z_NEG | (gfx_clipedges() == 2 ? GB_OUT : 0))) {
                    span_clip = true;
                }
            } else if (wmin < W_MIN ||
                (wmin < W_NEAR && (v1->_z > v1->_w || v2->_z > v2->_w || v3->_z > v3->_w))) {
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
#ifndef HANDHELD
    if (gfx_detail_cand && cc_census_on()) sCenDetail[gfx_detail_why]++;
    if (tri_state.use_fog && cc_census_on()) {
        const int col = (rdp.fog_color.r | rdp.fog_color.g | rdp.fog_color.b) != 0;
        sCenFog[col]++;
        if (tri_state.fog_class & 4) sCenFogDecal[col]++;
        sCenFogPlan[tri_state.fog_class & 3]++;
        if ((v1->color.a | v2->color.a | v3->color.a) != 0) sCenFogSome++;
    }
#endif
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

/* OOT_RECT2T (2026-10-04, the title logo's flames and the Haunted Wasteland's sandstorm against angrylion and z64):
 * a texture rectangle in 2-cycle mode whose cycle 1 reads TEXEL1 drew tile 0 alone, with cycle 2 left out of its
 * colour -- the 2D path never took the triangles' two-tile pre-blend (gfx_blend_tiles), per-texel cycle 1 (the TPE,
 * gfx_tpe_tiles) or the cycle-2 lerp (PRIM - ENV) * COMBINED + ENV (gfx_update_tex_lerp). EnMag's flames
 * ((T1 - PRIM) * PRIM_LOD_FRAC + T0, the scrolled flame tile over each 64x64 logo mask) came out a white glow with
 * no orange; gFieldSandstormDL ((T1 - T0) * PRIM_LOD_FRAC + T0, alpha by ENV_A) a pale cream with gaps where tile 0
 * alone is thin (the bottom rows). Such a rectangle now binds the same pre-blend the 3D path would and folds
 * cycle 2 into its colour as the 3D path does (the vertex colour = cycle 2 at COMBINED = 1, the texel lerp or
 * modulate on). Its corners wrap by the pre-blend's size. Every other rectangle is drawn as before.
 * OOT_RECT2T=0: tile 0 alone, as before. */
static bool gfx_rect2t_on(void) {
    static int sOn SAVESTATE_EXCLUDE = -1;
    if (sOn < 0) { const char *e = getenv("OOT_RECT2T"); sOn = !(e && e[0] == '0'); }
    return sOn != 0;
}

/* This will be going away possibly, it all depends on how we end up treating hw sprites */
static void gfx_sp_tri1_2d(uint8_t vtx1_idx, uint8_t vtx2_idx, UNUSED uint8_t vtx3_idx) {
    struct VertexColor *v1 = &rsp.loaded_vertices_2D[vtx1_idx];
    struct VertexColor *v2 = &rsp.loaded_vertices_2D[vtx2_idx];
    struct VertexColor *v_arr[2] = {v1, v2};

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
    
    uint32_t cc_id = rdp.combine_mode;
    
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
    /* OOT_RECT2T (above): both tiles and cycle 2, as a triangle would draw them */
    const bool rect2t = two_cycle && !rdp.cc2_pass && gfx_cc1_uses_texel1() && gfx_rect2t_on();
    if (rendering_state.force_in != rect2t) { // rectangles take their colour from the 1-cycle model (OOT_RECT2T: + cycle 2)
        extern void gfx_scegu_set_force_in(bool on);
        gfx_flush();
        gfx_scegu_set_force_in(rect2t);
        rendering_state.force_in = rect2t;
    }
    if (rendering_state.tex_add) { // the 3D path's MK64 kart-tint ADD must not leak into rectangles
        extern void gfx_scegu_set_texfunc_add(bool on);
        gfx_flush();
        gfx_scegu_set_texfunc_add(false);
        rendering_state.tex_add = false;
    }
    if (rendering_state.fog_px) { // nor per-pixel fog (gfx_fog_plan)
        extern void gfx_soft_set_fog(bool on, int r, int g, int b, int a);
        gfx_flush();
        gfx_soft_set_fog(false, 0, 0, 0, -1);
        rendering_state.fog_px = false;
    }
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    /* COPY mode has no blender: an alpha-compare threshold keeps or drops each
     * texel whole, so texels with a clear alpha bit are skipped, not painted
     * black (f-zero-x's fix, same code base). */
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY &&
        (rdp.other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_THRESHOLD) {
        texture_edge = true;
    }
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
    
    const int tpe_plan = rect2t ? gfx_tpe_plan(use_alpha) : 0;
    if (tpe_plan & 1) { /* cycle 1's rgb comes pre-evaluated in the texel (as gfx_tri_rebuild_state) */
        cc_id = (cc_id & ~(0xfffu | CC_FLAG_RGB_A_ONE | CC_FLAG_RGB_D_ONE)) | ((uint32_t) CC_TEXEL0 << 9);
    }
    if (tpe_plan & 2) { /* and its alpha */
        cc_id = (cc_id & ~(0xfff000u | CC_FLAG_ALPHA_A_ONE | CC_FLAG_ALPHA_D_ONE)) | ((uint32_t) CC_TEXEL0 << 21);
    }
    if (gfx_trace_frames > 0) port_log("  2d: cc %08X\n", (unsigned) cc_id);
    gfx_update_tex_lerp(cc_id, rect2t);
    struct ColorCombiner *comb = gfx_lookup_or_create_color_combiner(cc_id);
    struct ShaderProgram *prg = comb->prg;
    if (gfx_trace_frames > 0) port_log("  2d: prg %p\n", prg);
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
    
    for (int pass = 0; pass < 2; pass++) {
    const uint32_t epoch = gfx_cache_epoch; /* a reset under tile 1's import: tile 0 again (gfx_cache_epoch) */
    for (int i = 0; i < 2; i++) {
        if (used_textures[i] || rect2t) {
            if (rdp.textures_changed[i]) {
                gfx_flush();
                if (rect2t && i == 1 && gfx_tile1_importable()) { /* tile 1's own descriptor (OOT_RECT2T) */
                    import_texture_tile1();
                } else {
                    import_texture(i);
                }
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
    if (epoch == gfx_cache_epoch || !gfx_epochfix_on()) break;
    }
    // The GE has one texture unit: whatever was bound last (possibly tile 1's
    // mip level, or a texture just uploaded) must give way to tile 0's texture.
    uint32_t blend_w = 0, blend_h = 0, blend_id = 0;
    if (rect2t && (used_textures[0] || used_textures[1])) { /* OOT_RECT2T: the pre-blend a triangle would bind */
        blend_id = tpe_plan != 0 ? gfx_tpe_tiles(tpe_plan, &blend_w, &blend_h) : gfx_blend_tiles(&blend_w, &blend_h);
    }
    if (blend_id != 0) {
        gfx_select_tex0(blend_id);
        gfx_rapi->set_sampler_parameters(0, (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT,
                                         rdp.texture_tile.cms, rdp.texture_tile.cmt);
    } else if (used_textures[0] && rendering_state.textures[0] != NULL) {
        gfx_rapi->select_texture(0, rendering_state.textures[0]->texture_id);
    }
    
    bool use_texture = used_textures[0] || used_textures[1];
    //uint32_t tex_width = (rdp.texture_tile.lrs - rdp.texture_tile.uls + 4) / 4;
    //uint32_t tex_height = (rdp.texture_tile.lrt - rdp.texture_tile.ult + 4) / 4;

    VertexColor tri_buf[2] = {{0}};
    int tri_num_vert = 0;
    /* A wrapped tile reads texel (S - origin) & (period - 1), period = 2^mask
     * (twice that mirrored), but the sampler wraps by the imported size: a
     * texrect starting at S = 0 on a tile whose origin is 64 texels in read
     * column (-64 mod size). Shift both corners by whole periods so the first
     * lands in [0, period) (pm64 525cb47: PM's 64x32 piece grids). Clamped
     * tiles are left alone. S10.5 units. */
    int32_t wrap_s = 0, wrap_t = 0;
    if (use_texture && blend_id != 0) { /* OOT_RECT2T: whole periods of the pre-blend (both tiles' phases kept) */
        if (!(rdp.texture_tile.cms & G_TX_CLAMP)) {
            const int32_t p = 32 * (int32_t) blend_w, s0 = v_arr[0]->u - rdp.texture_tile.uls * 8;
            wrap_s = (s0 >= 0 ? s0 / p : -((-s0 + p - 1) / p)) * p;
        }
        if (!(rdp.texture_tile.cmt & G_TX_CLAMP)) {
            const int32_t p = 32 * (int32_t) blend_h, t0 = v_arr[0]->v - rdp.texture_tile.ult * 8;
            wrap_t = (t0 >= 0 ? t0 / p : -((-t0 + p - 1) / p)) * p;
        }
    } else if (use_texture) {
        if (!(rdp.texture_tile.cms & G_TX_CLAMP) && rdp.texture_tile.masks != 0) {
            const int32_t p = 32 << (rdp.texture_tile.masks + ((rdp.texture_tile.cms & G_TX_MIRROR) ? 1 : 0));
            const int32_t s0 = v_arr[0]->u - rdp.texture_tile.uls * 8;
            wrap_s = (s0 >= 0 ? s0 / p : -((-s0 + p - 1) / p)) * p;
        }
        if (!(rdp.texture_tile.cmt & G_TX_CLAMP) && rdp.texture_tile.maskt != 0) {
            const int32_t p = 32 << (rdp.texture_tile.maskt + ((rdp.texture_tile.cmt & G_TX_MIRROR) ? 1 : 0));
            const int32_t t0 = v_arr[0]->v - rdp.texture_tile.ult * 8;
            wrap_t = (t0 >= 0 ? t0 / p : -((-t0 + p - 1) / p)) * p;
        }
    }
    
#ifndef HANDHELD
    if (use_texture && cc_census_on()) {
        sCenRects++;
        sCenRectWrap += (wrap_s | wrap_t) != 0;
    }
#endif
    /* A rectangle the game filters bilinearly and magnifies more than 2x: the song effects
     * (Oceff_Storm's full-screen cloud layer is a 64x64 texture read at a tile shift of 3, ~58
     * pixels a texel). The RDP scales S/T by the render tile's shift before subtracting its origin
     * and filters between texels; the whole-texel corners below lose the shift and the scroll's
     * fraction, and the 2D path samples them nearest -- huge flickering blocks (the user's
     * device screenshot, 2026-09-26). These corners go to gfx_soft in S10.5 with z = 5, which
     * it samples bilinearly. Every other rectangle is unchanged. OOT_RECTBILIN=0: off (A/B). */
    int32_t fs[2], ft[2];
    bool magbil = false;
    if (use_texture && (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT &&
        (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) != G_CYC_COPY) {
        static int sRectBilin SAVESTATE_EXCLUDE = -1;
        if (sRectBilin < 0) { const char *e = getenv("OOT_RECTBILIN"); sRectBilin = !(e && e[0] == '0'); }
        if (sRectBilin) {
            const float scs = gfx_tile_shift_scale(rdp.tiles[G_TX_RENDERTILE].shifts);
            const float sct = gfx_tile_shift_scale(rdp.tiles[G_TX_RENDERTILE].shiftt);
            int32_t ws = 0, wt = 0;
            for (int i = 0; i < 2; i++) {
                fs[i] = (int32_t) lroundf(v_arr[i]->u * scs) - rdp.texture_tile.uls * 8;
                ft[i] = (int32_t) lroundf(v_arr[i]->v * sct) - rdp.texture_tile.ult * 8;
            }
            if (!(rdp.texture_tile.cms & G_TX_CLAMP) && (rdp.texture_tile.masks != 0 || blend_id != 0)) {
                const int32_t p = blend_id != 0 ? 32 * (int32_t) blend_w /* OOT_RECT2T: the pre-blend's period */
                                                : 32 << (rdp.texture_tile.masks + ((rdp.texture_tile.cms & G_TX_MIRROR) ? 1 : 0));
                ws = (fs[0] >= 0 ? fs[0] / p : -((-fs[0] + p - 1) / p)) * p;
            }
            if (!(rdp.texture_tile.cmt & G_TX_CLAMP) && (rdp.texture_tile.maskt != 0 || blend_id != 0)) {
                const int32_t p = blend_id != 0 ? 32 * (int32_t) blend_h
                                                : 32 << (rdp.texture_tile.maskt + ((rdp.texture_tile.cmt & G_TX_MIRROR) ? 1 : 0));
                wt = (ft[0] >= 0 ? ft[0] / p : -((-ft[0] + p - 1) / p)) * p;
            }
            const int w = abs((int) v_arr[1]->x - (int) v_arr[0]->x), h = abs((int) v_arr[1]->y - (int) v_arr[0]->y);
            magbil = true;
            for (int i = 0; i < 2; i++) {
                fs[i] -= ws;
                ft[i] -= wt;
                if (fs[i] < -32768 || fs[i] > 32767 || ft[i] < -32768 || ft[i] > 32767) magbil = false;
            }
            /* texels per pixel under 1/2 on both axes (S10.5: 32 a texel) */
            magbil = magbil && abs(fs[1] - fs[0]) * 2 < w * 32 && abs(ft[1] - ft[0]) * 2 < h * 32;
        }
    }
    
    for (int i = 0; i < 2; i++) {
        tri_buf[tri_num_vert].x = v_arr[i]->x;
        tri_buf[tri_num_vert].y = v_arr[i]->y;
        tri_buf[tri_num_vert].z = 0;
        
        if (use_texture) {
            short u = (v_arr[i]->u - rdp.texture_tile.uls * 8 - wrap_s) / 32;
            short v = (v_arr[i]->v - rdp.texture_tile.ult * 8 - wrap_t) / 32;
            /*
            if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
                // Linear filter adds 0.5f to the coordinates
                u += 0.5f;
                v += 0.5f;
            }
            */
            tri_buf[tri_num_vert].u = u;
            tri_buf[tri_num_vert].v = v;
            if (magbil) { /* S10.5 corners, bilinear (see above) */
                tri_buf[tri_num_vert].u = (short) fs[i];
                tri_buf[tri_num_vert].v = (short) ft[i];
                tri_buf[tri_num_vert].z = 5;
            }
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
        if (rect2t) { /* OOT_RECT2T: cycle 2 folded in (gfx_eval_vertex_color reads tri_state.cc2) */
            const bool cc2_saved = tri_state.cc2;
            tri_state.cc2 = true;
            gfx_eval_vertex_color(cc_id, &v_arr[i]->color, 0, &tri_buf[tri_num_vert].color);
            tri_state.cc2 = cc2_saved;
        } else {
            gfx_eval_vertex_color(cc_id, &v_arr[i]->color, 0, &tri_buf[tri_num_vert].color);
        }
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
#if 0
        case G_MV_LOOKATY:
        case G_MV_LOOKATX:
            memcpy(rsp.current_lookat + (index - G_MV_LOOKATY) / 2, data, sizeof(Light_t));
            //rsp.lights_changed = 1;
            break;
#endif
#ifdef F3DEX_GBI_2
        case G_MV_LIGHT: {
            int lightidx = offset / 24 - 2;
            if (lightidx >= 0 && lightidx <= MAX_LIGHTS) {
                // NOTE: reads out of bounds if it is an ambient light
                memcpy(rsp.current_lights + lightidx, data, sizeof(Light_t));
                rsp.lights_changed = true; /* a new direction is taken into model space at the next vertex load */
            } else if (lightidx == -2 || lightidx == -1) {
                /* gSPLookAt: G_MVO_LOOKATX (offset 0) and G_MVO_LOOKATY (24), the texgen axes */
                memcpy(&rsp.current_lookat[lightidx == -2 ? 1 : 0], data, sizeof(Light_t));
                rsp.lights_changed = true;
            }
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

static uintptr_t rsp_segments[16]; /* see seg_addr() */

static void gfx_sp_moveword(uint8_t index, uint16_t offset, uint32_t data) {
    _UNUSED(offset);

    switch (index) {
        case G_MW_NUMLIGHT:
#ifdef F3DEX_GBI_2
            rsp.current_num_lights = data / 24 + 1; // add ambient light
            if (rsp.current_num_lights > MAX_LIGHTS + 1) rsp.current_num_lights = MAX_LIGHTS + 1;
            if (rsp.current_num_lights < 1) rsp.current_num_lights = 1;
#else
            // Ambient light is included
            // The 31th bit is a flag that lights should be recalculated
            rsp.current_num_lights = (data - 0x80000000U) / 32;
#endif
            rsp.lights_changed = 1;
            break;
        case G_MW_FOG:
            if ((int16_t)(data >> 16) != rsp.fog_mul || (int16_t)data != rsp.fog_offset) gfx_flush(); /* round 74: the batch keeps its own */
            rsp.fog_mul = (int16_t)(data >> 16);
            rsp.fog_offset = (int16_t)data;
            break;
        case G_MW_SEGMENT:
            /* A physical-form base of a host pointer (OS_K0_TO_PHYSICAL: ptr - 0x80000000) lies at or above
             * 0xC0000000, where no host pointer does (ARM32 user space ends there): back to the pointer. */
            if (data >= 0xC0000000u) {
                static int sWarned;
                if (sWarned++ < 8) port_log("gfx: segment %u base %08X is a physical form: %08X used\n", (offset >> 2) & 0xF,
                                            (unsigned) data, (unsigned) (data + 0x80000000u));
                data += 0x80000000u;
            }
            rsp_segments[(offset >> 2) & 0xF] = data;
            break;
        case G_MW_CLIP: /* gSPClipRatio: RNX carries the ratio (FR_NEG_FRUSTRATIO_n = n) */
            if (offset == G_MWO_CLIP_RNX && (data & 0xFFFF) >= 1 && (data & 0xFFFF) <= 6) {
                gfx_clip_ratio = (int) (data & 0xFFFF);
            }
            break;
    }
}

static void gfx_sp_texture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    _UNUSED(level);
    _UNUSED(tile);
    _UNUSED(on);

    rsp.texture_scaling_factor.s = sc;
    rsp.texture_scaling_factor.t = tc;
}

/* R23, the ports lead's final rule (round 2), on the scissor: (a) an edge within one N64 pixel of
 * a screen edge goes to it, all four sides (the one-short band below is the right and bottom);
 * (b) interior edges by pixel centres, column c inside when x0 <= c + 0.5 < x1, rows alike
 * (F-Zero X's form). OOT_R23=0: off. The 2D rectangles on pixel centres would be their own
 * switch (FZX_RECTCENTER): not taken here, the lead's call. */
static int gfx_r23 SAVESTATE_EXCLUDE = -1;
static inline bool gfx_r23_on(void) {
    if (gfx_r23 < 0) { const char *e = getenv("OOT_R23"); gfx_r23 = !(e && e[0] == '0'); }
    return gfx_r23 != 0;
}
static void gfx_dp_set_scissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    _UNUSED(mode);
#ifndef HANDHELD
    sCenR28Sc++;
    sCenR28Now = lrx < ulx || lry < uly;
    sCenR28Inv += sCenR28Now;
#endif
    /* A scissor one pixel short of the N64 screen (0,0,319,239: DKR every frame)
     * would lose the handheld's last column and row everywhere: cover the edge,
     * as the fill-rect and texrect rules do (cross-port lead, 2026-09-24).
     * Only the one-short band: a larger scissor (Star Fox's 0,0,320,720) stays. */
    if (lrx >= (SCREEN_WIDTH - 1) << 2 && lrx < SCREEN_WIDTH << 2) lrx = SCREEN_WIDTH << 2;
    if (lry >= (SCREEN_HEIGHT - 1) << 2 && lry < SCREEN_HEIGHT << 2) lry = SCREEN_HEIGHT << 2;
    const bool r23 = gfx_r23_on();
    if (r23) { /* (a) the left and top edges */
        if (ulx <= 4) ulx = 0;
        if (uly <= 4) uly = 0;
    }

    float x = ulx / 4.0f * RATIO_X;
#if defined(TARGET_HANDHELD)
    float y = uly / 4.0f * RATIO_Y;                 /* top-down rasterizer: see viewport note */
#else
    float y = (SCREEN_HEIGHT - lry / 4.0f) * RATIO_Y;
#endif
    float width = (lrx - ulx) / 4.0f * RATIO_X;
    float height = (lry - uly) / 4.0f * RATIO_Y;
    if (r23 && lrx >= ulx && lry >= uly) { /* (b) pixel centres on the target */
        const float cx0 = ceilf(x - 0.5f), cx1 = ceilf(x + width - 0.5f), cy0 = ceilf(y - 0.5f), cy1 = ceilf(y + height - 0.5f);
#ifndef HANDHELD
        if (cc_census_on() && (cx0 != x || cx1 - cx0 != width || cy0 != y || cy1 - cy0 != height)) sCenR23Moved++;
#endif
        x = cx0; width = cx1 > cx0 ? cx1 - cx0 : 0.0f;
        y = cy0; height = cy1 > cy0 ? cy1 - cy0 : 0.0f;
    }

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

/* The render tile's palette: a CI4 tile reads the 16 colours its G_SETTILE palette names (GfxPrint's font picks one
 * of four 1-bit glyph planes that way, and was drawn as solid blocks when every CI4 tile took the last TLUT);
 * CI8 reads the 256 from bank 0. Banks no load has filled yet fall back to the last TLUT, as before. */
static void gfx_rdp_palette_update(void) {
    const uint8_t *p = NULL;
    if (rdp.texture_tile.siz == G_IM_SIZ_4b) {
        p = rdp.tlut_bank[rdp.tile_palette & 15];
    } else {
        p = rdp.tlut_bank[0];
    }
    rdp.palette = p != NULL ? p : rdp.tlut_last;
}

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, UNUSED uint32_t palette, uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts) {
    // A zero mask means no wrapping: the RDP clamps that axis to the tile
    // whatever the clamp bit says (and mirroring needs a mask). OoT's sun and
    // moon quads run their UVs past a 64x64 texture with mask 0 -- two moons.
    if (masks == 0) cms = (cms & ~G_TX_MIRROR) | G_TX_CLAMP;
    if (maskt == 0) cmt = (cmt & ~G_TX_MIRROR) | G_TX_CLAMP;
    if (tile == G_TX_RENDERTILE) {
        tri_state.valid = false;
    }
    _UNUSED(maskt);
    _UNUSED(masks);

    if (tile == G_TX_RENDERTILE) {
        rdp.texture_tile.fmt = fmt;
        rdp.texture_tile.siz = siz;
        rdp.tile_palette = (uint8_t) (palette & 15);
        gfx_rdp_palette_update();
        rdp.texture_tile.cms_raw = cms;
        rdp.texture_tile.cmt_raw = cmt;
        rdp.texture_tile.masks = (uint8_t) masks;
        rdp.texture_tile.maskt = (uint8_t) maskt;
        gfx_render_tile_cm();
        // MK64's packed course display lists keep two textures in TMEM (one
        // per half) and point the render tile at either: tile config G is a
        // 32x32 tile at tmem 256.  Draw from the block loaded into that half,
        // not always from the first (Moo Moo Farm's dirt road showed the grass
        // texture on those polygons, issue #1).
        rdp.texture_tile.tmem = (uint16_t) tmem;
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
    rdp.tiles[tile & 7].fmt = fmt;
    rdp.tiles[tile & 7].siz = siz;
    rdp.tiles[tile & 7].cms = cms;
    rdp.tiles[tile & 7].cmt = cmt;
    rdp.tiles[tile & 7].shifts = shifts;
    rdp.tiles[tile & 7].shiftt = shiftt;
    rdp.tiles[tile & 7].masks = (uint8_t) masks;
    rdp.tiles[tile & 7].maskt = (uint8_t) maskt;
    rdp.tiles[tile & 7].tmem = (uint16_t) tmem;
    rdp.tiles[tile & 7].palette = (uint8_t) (palette & 15);
    rdp.tiles[tile & 7].line_size_bytes = line * 8;
    if (tile == 1) {
        rdp.textures_changed[1] = true;
        tri_state.valid = false;
    }

}

static void gfx_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    rdp.tiles[tile & 7].uls = uls;
    rdp.tiles[tile & 7].ult = ult;
    rdp.tiles[tile & 7].lrs = lrs;
    rdp.tiles[tile & 7].lrt = lrt;
    if (tile == 1) {
        rdp.textures_changed[1] = true;
        tri_state.valid = false;
    }
    if (tile == G_TX_RENDERTILE) {
        tri_state.valid = false;
    }
    if (tile == G_TX_RENDERTILE) {
        rdp.texture_tile.uls = uls;
        rdp.texture_tile.ult = ult;
        rdp.texture_tile.lrs = lrs;
        rdp.texture_tile.lrt = lrt;
        gfx_render_tile_cm();
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
#ifndef HANDHELD
        sCenR26Dirty = false;
#endif
    }
}

static void gfx_dp_load_tlut(UNUSED uint8_t tile, uint32_t high_index) {
    tri_state.valid = false;

    SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(rdp.texture_to_load.siz == G_IM_SIZ_16b);
    rdp.tlut_last = rdp.texture_to_load.addr;
    if (rdp.texture_to_load.addr != NULL) { /* gDPLoadTLUT: count - 1 colours into TMEM at the load tile's address */
        const uint32_t tmem = rdp.tile_tmem[tile & 7];
        const uint32_t bank0 = tmem >= 256 ? (tmem - 256) / 16 : 0;
        const uint32_t banks = ((high_index & 0x3FF) + 1 + 15) / 16;
        for (uint32_t i = 0; i < banks && bank0 + i < 16; i++) {
            rdp.tlut_bank[bank0 + i] = rdp.texture_to_load.addr + i * 32;
        }
    }
    gfx_rdp_palette_update();
    if (gfx_debug_frame && rdp.palette != NULL) port_log("  loadtlut addr %p first %02X%02X %02X%02X %02X%02X\n", rdp.palette, rdp.palette[0], rdp.palette[1], rdp.palette[2], rdp.palette[3], rdp.palette[4], rdp.palette[5]);
}

static void gfx_dp_load_block(uint8_t tile, UNUSED uint32_t uls, UNUSED uint32_t ult, uint32_t lrs, uint32_t dxt) {
    tri_state.valid = false;
    _UNUSED(dxt);
#ifndef HANDHELD
    if (cc_census_on()) {
        sCenLoadBlocks++;
        if (dxt == 0 && lrs != 0) sCenLoadBlockDxt0[rdp.texture_to_load.siz & 3]++;
    }
#endif

    SUPPORT_CHECK(uls == 0);
    SUPPORT_CHECK(ult == 0);
    rdp.texture_to_load.tile_number = (uint8_t) gfx_lt_slot_for_load(rdp.tile_tmem[tile & 7]);
    
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
    rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes = size_bytes;
#ifndef HANDHELD
    cen_r27_overlap(rdp.texture_to_load.tile_number);
#endif
    SUPPORT_CHECK(size_bytes <= 4096);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].addr = rdp.texture_to_load.addr;
    rdp.loaded_texture[rdp.texture_to_load.tile_number].stride_bytes = 0;
    
    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    tri_state.valid = false;
    rdp.texture_to_load.tile_number = (uint8_t) gfx_lt_slot_for_load(rdp.tile_tmem[tile & 7]);
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
    rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes = size_bytes;

    SUPPORT_CHECK(size_bytes <= 4096);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].addr =
        rdp.texture_to_load.addr + gfx_texture_image_offset(uls >> G_TEXTURE_IMAGE_FRAC, ult >> G_TEXTURE_IMAGE_FRAC);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].stride_bytes = gfx_texture_image_offset(0, 1);
    rdp.texture_tile.uls = uls;
    rdp.texture_tile.ult = ult;
    rdp.texture_tile.lrs = lrs;
    rdp.texture_tile.lrt = lrt;
    gfx_render_tile_cm();
#ifndef HANDHELD
    sCenR26Dirty = (tile & 7) != G_TX_RENDERTILE;
    cen_r27_overlap(rdp.texture_to_load.tile_number);
#endif

    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
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

/* X27, pixel-exact HUD and text (the user, 2026-09-28; SM64 eb0a164): a texrect stepping one texel a
 * pixel (dsdx or dtdy exactly 1<<10, not flipped) that lands at 1:1 on the screen on that axis -- y always
 * at 240 high, x in the unsquished elements (rupees, minimap, area title) -- steps exactly one texel a
 * pixel from its first whole pixel, as the RDP does, instead of its truncated texel span stretched over
 * the rounded pixel span. The area title card (144x24, drawn y .. y + 23.75: TitleCard_Draw's lry - 1)
 * read 23 texel rows over 24 pixels, one row doubled; now rows 0..23 as on the N64. The 2D path samples
 * these point already (magbil needs > 2x). OOT_PIXEXACT=0: the old span (A/B). */
static int sRectStep1 SAVESTATE_EXCLUDE, sRectStep1T SAVESTATE_EXCLUDE; /* texture_rectangle -> draw_rectangle */
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
        if (gfx_hud_el_cx >= 0) { /* OOT_HUD: 1:1 around the element's squeezed centre */
            const float cx = (float) gfx_hud_el_cx;
            ulxf = gfx_hud_group_x(cx, sx) + (ulx / 4.0f - cx) * sy * gfx_hud_el_k;
            lrxf = gfx_hud_group_x(cx, sx) + (lrx / 4.0f - cx) * sy * gfx_hud_el_k;
            if (gfx_hud_msg_mode >= 0) { /* OOT_MSGBOX1TO1 (above) */
                const float x0 = 0.5f * (float) gfx_current_dimensions.width;
                const float kbox = fminf(1.0f, (float) gfx_current_dimensions.width / (256.0f * sy));
                const float w = (lrx - ulx) / 4.0f;
                if (w > 40.0f) {
                    ulxf = x0 + (ulx / 4.0f - cx) * sy * kbox;
                    lrxf = x0 + (lrx / 4.0f - cx) * sy * kbox;
                } else {
                    const float k = gfx_hud_msg_mode == 0 ? 1.0f : gfx_hud_msg_mode == 1 ? kbox : gfx_hud_msg_k();
                    const float c = x0 + ((ulx + lrx) / 8.0f - cx) * sy * k;
                    if (lrx - ulx == sMsgShW && lry - uly == sMsgShH && ulx == sMsgShUlx - 4 && uly == sMsgShUly - 4)
                        ulxf = sMsgShOut - 1.0f; /* the glyph right after its drop shadow (1 px right and down) */
                    else
                        ulxf = floorf(c - 0.5f * w * sy + 0.5f);
                    lrxf = ulxf + w * sy;
                    sMsgShUlx = ulx; sMsgShUly = uly; sMsgShW = lrx - ulx; sMsgShH = lry - uly; sMsgShOut = ulxf;
                }
            }
        } else if (gfx_hud_group_ax >= 0) { /* a squeezed rect in the button group: its centre spread */
            const float c = (ulx + lrx) / 8.0f;
            ulxf = gfx_hud_group_x(c, sx) + (ulx / 4.0f - c) * sx;
            lrxf = gfx_hud_group_x(c, sx) + (lrx / 4.0f - c) * sx;
        }
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
    if (sRectStep1 | sRectStep1T) { /* X27 (above) */
        static int sPixExact SAVESTATE_EXCLUDE = -1;
        if (sPixExact < 0) { const char *e = getenv("OOT_PIXEXACT"); sPixExact = !(e && e[0] == '0'); }
        if (sPixExact) {
            if (sRectStep1 && lrx > ulx && fabsf((lrxf - ulxf) * 4.0f - (float) (lrx - ulx)) < 0.02f)
                lr->u = (short) (ul->u + sRectStep1 * 32 * ((int) lr->x - (int) ul->x));
            if (sRectStep1T && lry > uly && fabsf((lryf - ulyf) * 4.0f - (float) (lry - uly)) < 0.02f)
                lr->v = (short) (ul->v + sRectStep1T * 32 * ((int) lr->y - (int) ul->y));
        }
    }

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

static void gfx_dp_texture_rectangle_impl(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip);

/* Framebuffer effects' state (see gfx_s2dex_bg): the screen's colour image for the task being drawn, and the current
 * colour image's pixel size and width (gfx_dp_set_color_image). */
static const void *sScreenCI SAVESTATE_EXCLUDE;
static int sFbFx SAVESTATE_EXCLUDE = -1;
static uint32_t sCiSiz SAVESTATE_EXCLUDE, sCiWidth SAVESTATE_EXCLUDE;
void gfx_set_screen_fb(const void *fb) { sScreenCI = fb; }
static bool gfx_fbfx_on(void) {
    if (sFbFx < 0) { const char *e = getenv("MM_FBFX"); sFbFx = !(e && e[0] == '0'); }
    return sFbFx && sScreenCI != NULL;
}
/* A rectangle into a colour image that is neither the screen nor the depth buffer (PreRender's coverage image for the
 * pause background's filters: the screen's coverage, as I8, rows of texrects). The soft renderer has no coverage: an
 * 8-bit image gets full coverage (0xFF), so the CPU's anti-alias filter leaves those pixels alone, as it does inside
 * the N64's polygons; any other offscreen rectangle is dropped. Drawn onto the screen, they were stray blocks.
 * Returns true when the rectangle was handled here. */
static bool gfx_fbfx_offscreen_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    const void *ci = rdp.color_image_address;
    if (!gfx_fbfx_on() || ci == NULL || ci == sScreenCI || ci == rdp.z_buf_address) return false;
    if (sCiSiz == G_IM_SIZ_8b && sCiWidth > 0 && sCiWidth <= 640 && (uintptr_t) ci >= PORT_REAL_PTR_MIN) {
        int x0 = ulx >> 2, y0 = uly >> 2, x1 = (lrx + 3) >> 2, y1 = (lry + 3) >> 2;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int) sCiWidth) x1 = (int) sCiWidth;
        if (y1 > 480) y1 = 480;
        for (int y = y0; y < y1 && x1 > x0; y++) memset((uint8_t *) ci + (size_t) y * sCiWidth + x0, 0xFF, (size_t) (x1 - x0));
    }
    if (gfx_debug_frame) port_log("  fbfx: a rectangle into colour image %p (siz %u w %u) %s\n", ci, sCiSiz, sCiWidth,
                                  sCiSiz == G_IM_SIZ_8b ? "filled: full coverage" : "dropped");
    return true;
}

/* A rectangle on another tile of the same CI4 texture with its own palette (GfxPrint: tiles 2, 4, 6 name
 * palettes 1-3 over the font at TMEM 0, one glyph plane each): drawn with that tile's palette. */
static void gfx_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    if (gfx_fbfx_offscreen_rect(ulx, uly, lrx, lry)) return;
    const uint8_t t = tile & 7, saved = rdp.tile_palette;
    const bool swap = t != G_TX_RENDERTILE && rdp.texture_tile.fmt == G_IM_FMT_CI &&
                      rdp.tiles[t].tmem == rdp.texture_tile.tmem && rdp.tiles[t].palette != saved;
    if (swap) {
        gfx_flush();
        rdp.tile_palette = rdp.tiles[t].palette;
        gfx_rdp_palette_update();
        rdp.textures_changed[0] = true;
        tri_state.valid = false;
    }
    gfx_dp_texture_rectangle_impl(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, flip);
    if (swap) {
        gfx_flush();
        rdp.tile_palette = saved;
        gfx_rdp_palette_update();
        rdp.textures_changed[0] = true;
        tri_state.valid = false;
    }
}

static void gfx_dp_texture_rectangle_impl(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    if (gfx_debug_frame) port_log("  texrect othermode_l %08X h %08X combine %08X\n", (unsigned) rdp.other_mode_l, (unsigned) rdp.other_mode_h, (unsigned) rdp.combine_mode);
    _UNUSED(tile);
    /* R34 (Smash 3b03a66): with the colour image set to the depth buffer, the RDP writes this
     * rectangle into depth, not onto the screen, like the depth clears gfx_dp_fill_rectangle
     * skips. Drawn to the screen it showed as a stray textured block (Smash: a black square
     * behind every off-screen bubble). OOT_ZIMGRECT=1 draws it anyway (A/B). */
    {
        static int sZimgRect SAVESTATE_EXCLUDE = -1;
        if (sZimgRect < 0) { const char *e = getenv("OOT_ZIMGRECT"); sZimgRect = e && e[0] == '1'; }
        if (rdp.color_image_address == rdp.z_buf_address) {
#ifndef HANDHELD
            if (cc_census_on()) sCenZimgRects++;
#endif
            if (!sZimgRect) return;
        }
    }
#ifndef HANDHELD
    if (cc_census_on()) {
        const uint32_t cyc = rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE);
        if (cyc == G_CYC_1CYCLE || cyc == G_CYC_2CYCLE) {
            sCenTexrects++;
            sCenTexrectFrac += ((ulx | uly | lrx | lry) & 3) != 0;
        }
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
        // 1/2-cycle: the lower-right edge is exclusive, so a rectangle reaching
        // the last N64 pixel (319, 239) leaves a 1-px seam on the handheld's
        // right/bottom edge that a CRT's overscan hid: cover the screen edge
        // (MK64 99921c7's fill-rect rule; DKR 9df6228, Star Fox). Before the UV
        // span below, so the last column samples on instead of stretching.
        // Only the one-short band, as for the scissor.
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
    #if 0
    if (!flip) {
        ll->u = uls;
        ll->v = lrt;
        ur->u = lrs;
        ur->v = ult;
    } else {
        ll->u = lrs;
        ll->v = ult;
        ur->u = uls;
        ur->v = lrt;
    }
    #endif
    
    /* An inverted rectangle (lower-right left of or above upper-left, after the copy-mode edge)
     * covers no pixel on the RDP. The 2D path swapped the corners and drew it stretched across the
     * screen with a negative S/T step (DKR's name entry: a glyph ending at x = 0 keeps a negative
     * ulx, which the 12-bit field wraps to ~1016 px; diddy-kong-racing 8131e79, GoldenEye d9c6f6f).
     * OOT_TEXRECTINV=1: drawn as before. */
    if (lrx < ulx || lry < uly) {
        static int draw SAVESTATE_EXCLUDE = -1;
        if (draw < 0) { const char *e = getenv("OOT_TEXRECTINV"); draw = e && e[0] == '1'; }
        if (!draw) { rdp.combine_mode = saved_combine_mode; return; }
    }
    sRectStep1 = !flip && (dsdx == 1024 || dsdx == -1024) ? (dsdx > 0 ? 1 : -1) : 0;
    sRectStep1T = !flip && (dtdy == 1024 || dtdy == -1024) ? (dtdy > 0 ? 1 : -1) : 0;
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    sRectStep1 = sRectStep1T = 0;
    rdp.combine_mode = saved_combine_mode;
}

/* uObjBg_t / uObjScaleBg_t (include/ultra64/gs2dex.h), written natively by
 * z_room.c's Room_DrawBackground2D. */
typedef struct {
    uint16_t imageX, imageW; /* u10.5 / u10.2 */
    int16_t frameX;          /* s10.2 */
    uint16_t frameW;         /* u10.2 */
    uint16_t imageY, imageH;
    int16_t frameY;
    uint16_t frameH;
    const void *imagePtr;
    uint16_t imageLoad;
    uint8_t imageFmt, imageSiz;
    uint16_t imagePal, imageFlip;
    uint16_t scaleW, scaleH; /* u5.10 (scalable form only) */
    int32_t imageYorig;
} HhObjBg;

static void gfx_log_unresolved(const char *what, uintptr_t w1);
static void *seg_addr(uintptr_t w1);
extern void gfx_soft_draw_bg(const void *img, int img_w, int img_h, int fmt, const uint8_t *tlut, int x0, int y0, int x1,
                             int y1, float u0, float v0, float dudx, float dvdy);

/* gSPBgRectCopy / gSPBgRect1Cyc: blit the (JPEG-decoded) room image. RGBA16
 * backgrounds are the prerendered rooms, decoded in place by the CPU (host
 * order, hh_jpeg.c); CI8 ones read their TLUT like any CI texture. */
/* Framebuffer effects (MM's motion blur, PreRender's saves of the frame for the pause background): the screen's
 * colour image for the task being drawn (hh_sched.c), so a draw that reads the screen as an image, or writes another
 * colour image, is recognised. The soft renderer never writes the N64's framebuffer in RAM: a copy of the screen into
 * another colour image is made here from the frame drawn so far (RGBA5551, as the RDP would have), and a draw of
 * the screen onto itself is dropped. MM_FBFX=0: as before (drawn as plain images). */
extern void gfx_soft_read_rgba16(uint16_t *dst, int dw, int dh);
extern void gfx_soft_blend_rgba16(const uint16_t *src, int sw, int sh, int alpha);

static void gfx_s2dex_bg(bool copy, const void *ptr) {
    const HhObjBg *bg = (const HhObjBg *) ptr;
    const void *img;
    float sx = RATIO_X, sy = RATIO_Y, scw = 1.0f, sch = 1.0f;
    int x0, y0, x1, y1;

    if (bg == NULL || (uintptr_t) bg < PORT_REAL_PTR_MIN) return;
    img = seg_addr((uintptr_t) bg->imagePtr);
    if (img == NULL || (uintptr_t) img < PORT_REAL_PTR_MIN) { gfx_log_unresolved("G_BG", (uintptr_t) bg->imagePtr); return; }
    if (gfx_fbfx_on()) {
        const void *ci = rdp.color_image_address;
        /* the depth buffer counts: PreRender saves the screen for the pause background into it (fbufSave = zbuffer);
         * drawn as the screen onto itself, the save was lost and the pause showed black behind the menu */
        const bool offscreen = ci != NULL && ci != sScreenCI;
        if (offscreen) {
            if (img == sScreenCI && bg->imageSiz == G_IM_SIZ_16b && (uintptr_t) ci >= PORT_REAL_PTR_MIN) {
                const int w = bg->imageW / 4, h = bg->imageH / 4;
                gfx_flush();
                if (w > 0 && h > 0 && w <= 640 && h <= 480) gfx_soft_read_rgba16((uint16_t *) ci, w, h);
                if (gfx_debug_frame) port_log("  fbfx: the screen saved to %p (%dx%d)\n", ci, w, h);
            } else if (gfx_debug_frame) {
                port_log("  fbfx: a bg into colour image %p dropped\n", ci);
            }
            return;
        }
        if (img == sScreenCI) {
            if (gfx_debug_frame) port_log("  fbfx: the screen onto itself dropped\n");
            return;
        }
        if (!copy && bg->imageFmt == G_IM_FMT_RGBA && bg->imageSiz == G_IM_SIZ_16b &&
            (rdp.env_color.a < 255 || rdp.cc1[3] != 7 || rdp.cc1[0] == G_CCMUX_PRIMITIVE)) {
            /* a saved screen drawn back through the combiner (motion blur, the screenshot fades): evaluated per pixel.
             * (The old rule blended by env alpha alone: the fades' alpha-1 copy came out as nothing over a black fill,
             * minutes of black screen after a Song of Time in the audit's soak.) */
            extern void gfx_soft_screen_cc(const uint16_t *src, int sw, int sh, const uint8_t m[8], const uint8_t prim[4],
                                           const uint8_t env[4], uint8_t lod, int blend, float fx0, float fy0, float fx1,
                                           float fy1, float u0, float v0, float dudx, float dvdy);
            const bool blend = (rdp.other_mode_l & FORCE_BL) != 0;
            /* the frame and scale (VisFbuf's end-of-day shrink draws the saved screen scaled about the centre) */
            const float bsw = (bg->scaleW != 0) ? bg->scaleW / 1024.0f : 1.0f, bsh = (bg->scaleH != 0) ? bg->scaleH / 1024.0f : 1.0f;
            gfx_flush();
            gfx_soft_screen_cc((const uint16_t *) img, bg->imageW / 4, bg->imageH / 4, rdp.cc1,
                               (const uint8_t *) &rdp.prim_color, (const uint8_t *) &rdp.env_color, rdp.prim_lod_frac, blend,
                               bg->frameX / 4.0f, bg->frameY / 4.0f, (bg->frameX + bg->frameW) / 4.0f,
                               (bg->frameY + bg->frameH) / 4.0f, bg->imageX / 32.0f, bg->imageY / 32.0f, bsw, bsh);
            if (gfx_debug_frame) port_log("  fbfx: %p drawn over the screen: cc1 %u %u %u %u / %u %u %u %u blend %d\n", img, rdp.cc1[0],
                                          rdp.cc1[1], rdp.cc1[2], rdp.cc1[3], rdp.cc1[4], rdp.cc1[5], rdp.cc1[6], rdp.cc1[7], blend);
            return;
        }
    }
    if (!copy && bg->scaleW != 0 && bg->scaleH != 0) { scw = bg->scaleW / 1024.0f; sch = bg->scaleH / 1024.0f; }
    gfx_flush();
    gfx_set_phase(false);
    x0 = (int) (bg->frameX / 4.0f * sx + 0.5f);
    y0 = (int) (bg->frameY / 4.0f * sy + 0.5f);
    x1 = (int) ((bg->frameX + bg->frameW) / 4.0f * sx + 0.5f);
    y1 = (int) ((bg->frameY + bg->frameH) / 4.0f * sy + 0.5f);
    if (gfx_debug_frame) port_log("  bg %s img %p %ux%u fmt %u siz %u frame (%d,%d)-(%d,%d) scale %.3f\n", copy ? "copy" : "1cyc", img, bg->imageW / 4, bg->imageH / 4, bg->imageFmt, bg->imageSiz, x0, y0, x1, y1, scw);
    {
        float u0 = bg->imageX / 32.0f, dudx = scw / sx;
        if (gfx_crop3d()) { /* true shape about the centre, like the cropped 3D; clipped here with u0 moved */
            const float hw = 0.5f * (float) gfx_current_dimensions.width;
            const float pan = gfx_crop3d() == 2 ? gHhCropPan : 0.0f;
            x0 = (int) (hw + (bg->frameX / 4.0f - HALF_SCREEN_WIDTH - pan) * sy + 0.5f);
            x1 = (int) (hw + ((bg->frameX + bg->frameW) / 4.0f - HALF_SCREEN_WIDTH - pan) * sy + 0.5f);
            dudx = scw / sy;
            if (x0 < 0) { u0 += (float) -x0 * dudx; x0 = 0; }
            if (x1 > (int) gfx_current_dimensions.width) x1 = (int) gfx_current_dimensions.width;
        }
        gfx_soft_draw_bg(img, bg->imageW / 4, bg->imageH / 4,
                         bg->imageFmt == G_IM_FMT_CI ? 1 : 0, rdp.palette, x0, y0, x1, y1,
                         u0, bg->imageY / 32.0f, dudx, sch / sy);
    }
    gfx_jpeg_room_squash_on(); /* the rest of this frame's 3D matches the image */
}

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    if (rdp.color_image_address == rdp.z_buf_address) {
        // Don't clear Z buffer here since we already did it with glClear
        return;
    }
    if (gfx_fbfx_on()) {
        /* PreRender_FetchFbufCoverage: the screen's coverage written over the screen (G_RM_VISCVG); there is none */
        if ((rdp.other_mode_l & ~7u) == ((uint32_t) (G_RM_VISCVG | G_RM_VISCVG2) & ~7u)) {
            if (gfx_debug_frame) port_log("  fbfx: the coverage fill (VISCVG) dropped\n");
            return;
        }
        if (gfx_fbfx_offscreen_rect(ulx, uly, lrx, lry)) return;
    }
    uint32_t mode = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));
    
    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    } else {
        // 1/2-cycle: the lower-right edge is exclusive, so a full-screen dim to
        // (319, 239) leaves a 1px seam a CRT's overscan hid (MK64 99921c7).
        // The plain form, not the one-short band of the texrect and scissor
        // rules: it also turns a fill larger than the screen into an exact
        // full-screen fill for the deferred-fill path (Smash's title).
        if (lrx >= (SCREEN_WIDTH - 1) << 2) lrx = SCREEN_WIDTH << 2;
        if (lry >= (SCREEN_HEIGHT - 1) << 2) lry = SCREEN_HEIGHT << 2;
    }
    /* The same for a fill: an inverted rectangle covers no pixel on the RDP (texrect above).
     * OOT_TEXRECTINV=1: drawn as before. */
    if (lrx < ulx || lry < uly) {
        static int draw SAVESTATE_EXCLUDE = -1;
        if (draw < 0) { const char *e = getenv("OOT_TEXRECTINV"); draw = e && e[0] == '1'; }
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
    // 1/2-cycle mode: the rectangle goes through the current combiner and
    // blender like any primitive.  MK64's draw_box (pause dim, menu fades,
    // the lightning flash, the fall-off blackout) does exactly that: XLU_SURF
    // + G_CC_PRIMITIVE with the box colour and an 8-bit alpha in the prim
    // colour.  Forcing the fill colour here (what sm64-port did) drew those
    // boxes opaque black or not at all, depending on the fill colour's 1-bit
    // alpha.
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_set_z_image(void *z_buf_address) {
    rdp.z_buf_address = z_buf_address;
}

static void gfx_dp_set_color_image(uint32_t format, uint32_t size, uint32_t width, void* address) {
    _UNUSED(format);
    sCiSiz = size;
    sCiWidth = width + 1; /* the command holds width - 1 */
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

/* The RSP's segment table (OoT sets it with gSPSegment: objects on 0x06, eye and
 * mouth textures on 0x08/0x09, the z-buffer on 0x0E ...), reset every task like
 * the microcode's DMEM copy. An RSP segment wins; an unset one falls back to the
 * CPU's gSegments (port_seg_to_ptr), as oot-PSP's interpreter does. Bases are real
 * pointers, segmented addresses or NULL: the master segment sync (z_rcp.c) and the
 * call sites that converted to a physical address (the bubble and splash effects,
 * Link's underwater bubbles, En_Fish2) pass host pointers (K0_TO_DL_ADDR). */
extern void* port_seg_to_ptr(uintptr_t addr);

static void *seg_addr(uintptr_t w1) {
    int depth;

    for (depth = 0; depth < 4; depth++) {
        uint32_t seg;
        uintptr_t base;

        if (w1 >= PORT_REAL_PTR_MIN || w1 == 0) {
            return (void *) w1;
        }
        seg = (uint32_t) (w1 >> 24);
        if (seg >= 16) {
            return (void *) w1;
        }
        base = rsp_segments[seg];
        if (base == 0) {
            return port_seg_to_ptr(w1);
        }
        /* A segment base may itself be segmented (chained): resolve again. */
        w1 = base + (w1 & 0x00FFFFFF);
    }
    return (void *) w1;
}

/* The billboard. The game names segment 1's matrix (the billboard Play_Draw builds each frame) as
 * &D_01000000: 0x01000000 on the N64, but here a real variable (hh_game.c, an identity matrix), so
 * seg_addr returned the identity and every gSPMatrix(&D_01000000) draw lost its billboard: the sun's
 * lens flare circles and ring (z_kankyo.c), the rain and the other z_kankyo.c effects, Object_Kankyo,
 * Demo_Kankyo, En_Viewer and Effect_Ss_Dead_Dd. Their quads stayed in the world's xy plane, so seen
 * from the side they narrowed to slivers (the user's "squashed" flares). The PSP renderer maps it
 * (gfx_fast3d.c); this maps it to segment 1 the same way, and keeps the identity while segment 1 is
 * unset. Weak: the host test harnesses have no hh_game.c. OOT_BILLBOARD=0: the identity, as before. */
extern char D_01000000[] __attribute__((weak));
static void *gfx_mtx_addr(uintptr_t w1) {
    static int sOn = -1;
    if (w1 != 0 && w1 == (uintptr_t) D_01000000 && rsp_segments[1] != 0) {
        if (sOn < 0) { const char *e = getenv("OOT_BILLBOARD"); sOn = !(e != NULL && e[0] == '0'); }
        if (sOn) return seg_addr(0x01000000u);
    }
    return seg_addr(w1);
}

static void gfx_log_unresolved(const char *what, uintptr_t w1) {
    static int sCount;

    if (sCount++ < 32) {
        port_log("gfx: %s to unresolved %08X (rsp seg %08X)\n", what, (unsigned) w1,
                 (unsigned) rsp_segments[(w1 >> 24) & 0xF]);
    }
}


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

#ifndef HANDHELD
/* OOT_TRITRACE=<frame> (desktop debug): every G_TRI1/G_TRI2 triangle of that frame -- the command's address, its
 * vertex indices and their clip x,y,w -- to tell a display list drawn twice from a mesh that overlaps itself */
static void gfx_tri_trace(const Gfx *cmd, int a, int b, int c) {
    static long sAt = -2;
    extern long gfx_trace_frame_no;
    if (sAt == -2) { const char *e = getenv("OOT_TRITRACE"); sAt = e ? atol(e) : -1; }
    if (sAt < 0 || gfx_trace_frame_no != sAt) return;
    const struct LoadedVertex *v = rsp.loaded_vertices;
    port_log_always("TRI cmd %p [%d %d %d] (%.1f %.1f %.1f) (%.1f %.1f %.1f) (%.1f %.1f %.1f)\n", (const void *) cmd, a, b, c,
             v[a].x, v[a].y, v[a].w, v[b].x, v[b].y, v[b].w, v[c].x, v[c].y, v[c].w);
}
long gfx_trace_frame_no;
#endif

#if !defined(TARGET_PSP) && !defined(PORT_GE_CULL) && !defined(PORT_GE_TL)
/* gSPModifyVertex (F3DEX2 G_MODIFYVTX, 0x02): one word of a vertex already in the vertex buffer rewritten in place,
 * after its transform and lighting, as the microcode does in DMEM. gfx_run_dl had no case for it, so it was skipped.
 * OoT sends only G_MWO_POINT_ST (657 commands in the extracted object display lists, none from game code): a limb's
 * triangles reuse vertices the previous limb loaded (the seam) and give them this limb's texture coordinates first.
 * Skipped, those triangles sampled the seam at the previous limb's coordinates in this limb's texture: adult and
 * child Ruto, the Bazaar's shopkeeper, the roof man, Barinade, Volvagia, the Like Like, Dead Hand, the Shell Blade,
 * Jabu-Jabu's muscle blocks, the fish (gameplay_keep). The four kinds, as Smash's gfx_sp_modify_vertex:
 *   G_MWO_POINT_RGBA      the shade colour and alpha (the fog factor's byte too, as the RSP keeps it there);
 *   G_MWO_POINT_ST        s and t, s10.5 after the gSPTexture scale: what gfx_sp_vertex stores in u, v;
 *   G_MWO_POINT_XYSCREEN  x and y in s13.2 N64 pixels, taken back through the N64 viewport to clip space at the
 *                         vertex's own w, then widened and panned as gfx_sp_vertex does its x;
 *   G_MWO_POINT_ZSCREEN   the screen depth, s15.16 in the RDP's 0..0x7FFF range (the viewport's z times 32), back
 *                         to clip z the same way.
 * After XYSCREEN / ZSCREEN the clip flags are redone (vtx_clip_flags). OOT_MODIFYVTX (debug.cfg modifyvtx=): 0 =
 * skipped, as before; 1 = applied (default); ab = skipped and applied in turn every 100 frames (~5 s), for an A/B
 * on the device. OOT_PROBE=1: a "PROBE modifyvtx" line for each frame that sent any. */
static int sModVtx SAVESTATE_EXCLUDE = -1;
static int sModVtxProbe SAVESTATE_EXCLUDE = -1;
static uint32_t gfx_modvtx_count[4] SAVESTATE_EXCLUDE; /* RGBA, ST, XYSCREEN, ZSCREEN this frame (OOT_PROBE=1) */
static inline bool gfx_modifyvtx_on(void) {
    if (sModVtx < 0) {
        const char *e = getenv("OOT_MODIFYVTX");
        sModVtx = (e && e[0] == 'a' && e[1] == 'b') ? 2 : !(e && e[0] == '0');
        if (sModVtx != 1) port_log_always("gfx: OOT_MODIFYVTX %s\n", sModVtx == 2 ? "ab (off, then on, every 100 frames)" : "off");
    }
    return sModVtx == 2 ? ((gfx_frame_counter / 100) & 1) != 0 : sModVtx != 0;
}
static __attribute__((noinline)) void gfx_sp_modify_vertex(uint32_t idx, uint32_t where, uint32_t val) {
    if (idx >= MAX_VERTICES || !gfx_modifyvtx_on()) return;
    struct LoadedVertex *d = &rsp.loaded_vertices[idx];
    switch (where) {
        case G_MWO_POINT_RGBA:
            d->color.r = (uint8_t) (val >> 24); d->color.g = (uint8_t) (val >> 16);
            d->color.b = (uint8_t) (val >> 8); d->color.a = (uint8_t) val;
            gfx_modvtx_count[0]++;
            break;
        case G_MWO_POINT_ST:
            d->u = (float) (int16_t) (val >> 16);
            d->v = (float) (int16_t) val;
            gfx_modvtx_count[1]++;
            break;
        case G_MWO_POINT_XYSCREEN:
        case G_MWO_POINT_ZSCREEN: {
            gfx_modvtx_count[where == G_MWO_POINT_XYSCREEN ? 2 : 3]++;
            if (!gfx_raw_vp_valid) break;
            const float w = d->_w != 0.0f ? d->_w : 1.0f;
            if (where == G_MWO_POINT_XYSCREEN) {
                const float vsx = gfx_raw_vp.vscale[0], vsy = gfx_raw_vp.vscale[1]; /* both in 1/4 px, as s13.2 */
                if (vsx == 0.0f || vsy == 0.0f) break;
                const float xn = ((float) (int16_t) (val >> 16) - gfx_raw_vp.vtrans[0]) / vsx;
                const float yn = (gfx_raw_vp.vtrans[1] - (float) (int16_t) val) / vsy; /* N64 rows run down */
                const float aspect_q = (float) gfx_current_dimensions.width / (float) gfx_current_dimensions.height;
                float x = xn * w;
                if (rsp.is_persp && !gfx_ui_mode && !gfx_jpeg_room_squash) x = x * (4.0f / 3.0f) / aspect_q;
                if (gfx_jpeg_room_pan != 0.0f && rsp.is_persp && !gfx_ui_mode) {
                    x -= gfx_jpeg_room_pan * (2.0f / (240.0f * aspect_q)) * w;
                }
                d->_x = x;
                d->_y = yn * w;
            } else {
                const float vsz = gfx_raw_vp.vscale[2];
                if (vsz == 0.0f) break;
                const float zn = ((float) (int32_t) val / (65536.0f * 32.0f) - gfx_raw_vp.vtrans[2]) / vsz;
                d->_z = zn * w;
            }
            d->clip_rej = rsp.is_persp ? vtx_clip_flags(d->_x, d->_y, d->_z, d->_w, (float) gfx_clip_ratio, gfx_non_on(),
                                                        gfx_nearclip_on(), sVtxFlags > 0)
                                       : 0;
            break;
        }
    }
}
static void gfx_modvtx_probe_frame(void) {
    if (sModVtxProbe < 0) { const char *e = getenv("OOT_PROBE"); sModVtxProbe = e && e[0] == '1'; }
    if (sModVtxProbe && (gfx_modvtx_count[0] | gfx_modvtx_count[1] | gfx_modvtx_count[2] | gfx_modvtx_count[3])) {
        port_log_always("PROBE modifyvtx frame %u rgba %u st %u xyscreen %u zscreen %u\n", (unsigned) gfx_frame_counter,
                        (unsigned) gfx_modvtx_count[0], (unsigned) gfx_modvtx_count[1], (unsigned) gfx_modvtx_count[2],
                        (unsigned) gfx_modvtx_count[3]);
    }
    memset(gfx_modvtx_count, 0, sizeof(gfx_modvtx_count));
}
#endif

static void gfx_run_dl(Gfx* cmd) {
    for (;;) {
        uint32_t opcode = cmd->words.w0 >> 24;
        static long sTraceMax SAVESTATE_EXCLUDE = -1; /* OOT_TRACEMAX: display-list lines traced per frame (4000) */
        if (sTraceMax < 0) { const char *e = getenv("OOT_TRACEMAX"); sTraceMax = e ? atol(e) : 4000; }
        if (gfx_trace_frames > 0 && gfx_cmd_budget < sTraceMax) {
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
                if (cmd->words.w1 == HH_TAG_PRERENDER) {
                    gfx_jpeg_room_squash_on(); /* before the room's occluders, not just at the image */
                }
                if (cmd->words.w1 == HH_TAG_UI_BEGIN || cmd->words.w1 == HH_TAG_UI_END) {
                    extern void gfx_scegu_set_ui_mode(bool on);
                    bool on = cmd->words.w1 == HH_TAG_UI_BEGIN;
                    if (on != gfx_ui_mode) {
                        gfx_flush();
                        gfx_ui_mode = on;
                        gfx_scegu_set_ui_mode(on);
                    }
                    break;
                }
                if (cmd->words.w1 == HH_TAG_VISMONO) {
                    extern void gfx_soft_vismono(const uint8_t prim[4], const uint8_t env[4]);
                    gfx_flush();
                    gfx_soft_vismono((const uint8_t *) &rdp.prim_color, (const uint8_t *) &rdp.env_color);
                    break;
                }
                if (cmd->words.w1 == HH_TAG_SKY_BEGIN || cmd->words.w1 == HH_TAG_SKY_END) {
                    extern void gfx_scegu_set_sky_pass(bool on);
                    gfx_flush();
                    gfx_scegu_set_sky_pass(cmd->words.w1 == HH_TAG_SKY_BEGIN);
                    break;
                }
                if ((cmd->words.w1 & 0xFFFF0000u) == 0x48560000u) { /* HH_HUD_EL / HH_HUD_EL_END (OOT_HUD) */
                    const uint32_t id = (cmd->words.w1 >> 12) & 0xFu;
                    const uint32_t cx = cmd->words.w1 & (id == 5 ? 0x3FFu : 0xFFFu); /* 5: the message box's flags above */
                    if (id == 15 && cmd->words.w1 != 0x4856FFFFu) { gfx_hud_group_cref = (int) cx; break; } /* C-left's centre */
                    if (id == 14 && cmd->words.w1 != 0x4856FFFFu) { gfx_hud_group_leftcut = (int) cx; break; } /* paused: START unspread */
                    const int want = (cmd->words.w1 != 0x4856FFFFu && gfx_hud_el_enabled((int) id)) ? (int) cx : -1;
                    const float k = (want >= 0 && id == 5) ? gfx_hud_msg_k() : 1.0f;
                    if (cmd->words.w1 == 0x4856FFFFu) { gfx_hud_group_ax = gfx_hud_group_cref = gfx_hud_group_leftcut = -1; }
                    else if (id == 3) gfx_hud_group_ax = gfx_hud_el_enabled(8) ? (int) cx : -1; /* the spread goes with A's 1:1 label */
                    const int msg = (want >= 0 && id == 5 && gfx_msgbox_1to1())
                                        ? ((cmd->words.w1 & HH_HUD_MSG_CREDITS) ? 2 : (cmd->words.w1 & HH_HUD_MSG_COMPACT) ? 1 : 0)
                                        : -1;
                    if (want != gfx_hud_el_cx || k != gfx_hud_el_k || msg != gfx_hud_msg_mode) {
                        gfx_flush();
                        gfx_hud_el_cx = want;
                        gfx_hud_el_k = k;
                        gfx_hud_msg_mode = msg;
                        sMsgShW = -1;
                    }
                    break;
                }
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
                gfx_sp_matrix(C0(0, 8) ^ G_MTX_PUSH, (const int32_t *) gfx_mtx_addr(cmd->words.w1));
#else
                gfx_sp_matrix(C0(16, 8), (const int32_t *) gfx_mtx_addr(cmd->words.w1));
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
#if defined(F3DEX_GBI_2) && !defined(TARGET_PSP) && !defined(PORT_GE_CULL) && !defined(PORT_GE_TL)
            case (uint8_t)G_MODIFYVTX: /* gSPModifyVertex: w0 = 02 | where << 16 | vtx * 2 */
                gfx_sp_modify_vertex(C0(0, 16) / 2, C0(16, 8), cmd->words.w1);
                break;
#endif
            case G_VTX:
#ifdef F3DEX_GBI_2
#if !defined(TARGET_PSP) && !defined(PORT_GE_CULL) && !defined(PORT_GE_TL)
                if (gfx_vtxfast_on() && C0(12, 8) <= MAX_VERTICES) {
                    gfx_sp_vertex_v2(C0(12, 8), C0(1, 7) - C0(12, 8), seg_addr(cmd->words.w1));
                    break;
                }
#endif
                gfx_sp_vertex(C0(12, 8), C0(1, 7) - C0(12, 8), seg_addr(cmd->words.w1));
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_vertex(C0(10, 6), C0(16, 8) / 2, seg_addr(cmd->words.w1));
#else
                gfx_sp_vertex((C0(0, 16)) / sizeof(Vtx), C0(16, 4), seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t) G_CULLDL:
            {
                // gSPCullDisplayList(first, last): end this list when every one of
                // those vertices is outside the same plane (OoT opens room/object
                // lists and each sky face with one; oot-PSP gfx_fast3d.c).
                // Exact frustum planes (the guard-band clip_rej is too loose to
                // ever cull); far is left out, as OoT's microcode does not clip it.
                uint32_t first = C0(0, 16) / 2, last = C1(0, 16) / 2, rej = rsp.is_persp ? 0x1fu : 0u;
                if (first < MAX_VERTICES && last < MAX_VERTICES && first <= last) {
                    for (uint32_t i = first; i <= last && rej != 0; i++) {
                        const struct LoadedVertex *cv = &rsp.loaded_vertices[i];
                        uint32_t out = 0;
                        if (cv->_x < -cv->_w) out |= 1;
                        if (cv->_x > cv->_w) out |= 2;
                        if (cv->_y < -cv->_w) out |= 4;
                        if (cv->_y > cv->_w) out |= 8;
                        if (cv->_z < -cv->_w) out |= 16;
                        rej &= out;
                    }
                    if (gfx_debug_frame) {
                        port_log("  culldl %u..%u %s\n", first, last, rej != 0 ? "CULLED" : "kept");
                        for (uint32_t i = first; i <= last; i++) {
                            const struct LoadedVertex *cv = &rsp.loaded_vertices[i];
                            port_log("    v%u clip %.1f %.1f %.1f w %.1f\n", i, cv->_x, cv->_y, cv->_z, cv->_w);
                        }
                    }
                    if (rej != 0) {
                        return;
                    }
                }
                break;
            }
            case G_DL:
                if ((uintptr_t)seg_addr(cmd->words.w1) < PORT_REAL_PTR_MIN) {
                    gfx_log_unresolved("G_DL", cmd->words.w1);
                    if (C0(16, 1) != 0) {
                        return; /* a branch to nowhere ends this list */
                    }
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
            case (uint8_t)G_RDPHALF_1:
                sRdpHalf1 = cmd->words.w1; /* the branch target for G_BRANCH_Z (texrects consume their own) */
                break;
            case (uint8_t)G_BRANCH_Z:
                // gSPBranchLessZraw: jump (no return) to the list G_RDPHALF_1 named
                // when a vertex's screen z is below a threshold -- the scene's
                // level-of-detail switch, whose near branch LOADS the vertices the
                // caller's triangles then use (Death Mountain Trail: without it the
                // trail near Link vanished, Link walking on sky). Always taken, as
                // oot-PSP and oot-dc do (neither keeps the RSP's fixed-point
                // screen z) -- and GLideN64's test, z/w * 1023 <= zval, is always
                // true for OoT's zvals of 0x400 and more (DMT: 0xAF0).
                if (sRdpHalf1 != 0) {
                    if ((uintptr_t)seg_addr(sRdpHalf1) < PORT_REAL_PTR_MIN) {
                        gfx_log_unresolved("G_BRANCH_Z", sRdpHalf1);
                        return;
                    }
                    cmd = (Gfx *)seg_addr(sRdpHalf1);
                    --cmd; // increase after break
                }
                break;
            case (uint8_t)G_ENDDL:
                return;
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
#ifndef HANDHELD
                gfx_tri_trace(cmd, C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
#endif
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
#ifndef HANDHELD
                gfx_tri_trace(cmd, C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
                gfx_tri_trace(cmd, C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
#endif
                /* Round 69: both halves the same triangle (the Deku Tree basement's water, for a denser blend):
                 * one triangle flagged to be drawn twice (gfx_soft.c shades it once and blends it twice, or
                 * draws it again). OOT_BLENDTWICE=0: two triangles, as before. */
                if ((cmd->words.w0 & 0xFFFFFFu) == (cmd->words.w1 & 0xFFFFFFu) && gfx_blend_twice_on()) {
#ifndef HANDHELD
                    { static int sOn = -1; static unsigned long sN; if (sOn < 0) sOn = getenv("OOT_TWICECOUNT") != NULL;
                      if (sOn && (++sN % 10) == 0) port_log_always("TWICECOUNT %lu\n", sN); }
#endif
                    gfx_tri_twice_next = true;
                    gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
                    gfx_tri_twice_next = false;
                    break;
                }
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
                break;
#endif
#ifdef F3DEX_GBI_2
            // S2DEX background rectangles (OoT's prerendered rooms load S2DEX just
            // for these; F3DEX2 leaves 0x09/0x0A unused, so decode them always).
            case 0x0A: /* G_BG_COPY */
            case 0x09: /* G_BG_1CYC */
                gfx_s2dex_bg(opcode == 0x0A, seg_addr(cmd->words.w1));
                break;
#endif
#ifdef F3DEX_GBI_2
            // gSP1Quadrangle: two triangles laid out like G_TRI2 (OoT's sky faces,
            // file-select panels, pause/menu quads). Was only decoded for F3D_OLD.
            case (uint8_t)G_QUAD:
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
            case (uint8_t)G_RDPSETOTHERMODE:
                /* gsDPSetOtherMode(h, l): the whole other mode at once (OoT's setup DLs
                 * use it everywhere: render mode, cycle type, filter, z mode). MK64 never
                 * emitted it, so the interpreter silently skipped it. */
                if (rdp.other_mode_h != (cmd->words.w0 & 0x00FFFFFF) || rdp.other_mode_l != cmd->words.w1) {
                    rdp.other_mode_h = cmd->words.w0 & 0x00FFFFFF;
                    rdp.other_mode_l = cmd->words.w1;
                    tri_state.valid = false;
                }
                break;
            case (uint8_t)G_SETOTHERMODE_H:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(63 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, (uint64_t) cmd->words.w1 << 32);
#else
                gfx_sp_set_other_mode(C0(8, 8) + 32, C0(0, 8), (uint64_t) cmd->words.w1 << 32);
#endif
                break;
            
            // RDP Commands:
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
#ifndef HANDHELD
                if (cc_census_on()) {
                    sCenTlutLoads++;
                    sCenTlutOffset += (C0(12, 12) | C0(0, 12)) != 0;
                }
#endif
                gfx_dp_load_tlut(C1(24, 3), C1(14, 10));
                break;
            case G_SETENVCOLOR:
                gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMCOLOR:
                if (rdp.prim_lod_frac != (uint8_t) C0(0, 8)) {
                    rdp.prim_lod_frac = (uint8_t) C0(0, 8);
                    tri_state.valid = false;
                }
                gfx_dp_set_prim_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFOGCOLOR:
                gfx_dp_set_fog_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFILLCOLOR:
                gfx_dp_set_fill_color(cmd->words.w1);
                break;
            case G_SETCOMBINE:
                {
                    /* Cycle 2 (OoT's 2-cycle modes: X * COMBINED tints -- Link's tunic is
                     * (TEXEL0 * SHADE) then * PRIM -- fog passes, ...). The MK64 model only
                     * decoded cycle 1. */
                    uint8_t c2[8] = { (uint8_t) C0(5, 4), (uint8_t) C1(24, 4), (uint8_t) C0(0, 5), (uint8_t) C1(6, 3),
                                      (uint8_t) C1(21, 3), (uint8_t) C1(3, 3), (uint8_t) C1(18, 3), (uint8_t) C1(0, 3) };
                    /* pass = (x - x)*y + COMBINED or (COMBINED - 0) * 1 + 0, for rgb and alpha */
                    bool rgb_pass = (c2[3] == G_CCMUX_COMBINED && (c2[0] == c2[1] || c2[2] == G_CCMUX_0)) ||
                                    (c2[0] == G_CCMUX_COMBINED && c2[1] == G_CCMUX_0 && c2[2] == G_CCMUX_1 && c2[3] == G_CCMUX_0);
                    bool a_pass = (c2[7] == G_ACMUX_COMBINED && (c2[4] == c2[5] || c2[6] == G_ACMUX_0)) ||
                                  (c2[4] == G_ACMUX_COMBINED && c2[5] == G_ACMUX_0 && c2[6] == G_ACMUX_1 && c2[7] == G_ACMUX_0);
                    {
                        uint8_t c1[8] = { (uint8_t) C0(20, 4), (uint8_t) C1(28, 4), (uint8_t) C0(15, 5), (uint8_t) C1(15, 3),
                                          (uint8_t) C0(12, 3), (uint8_t) C1(12, 3), (uint8_t) C0(9, 3), (uint8_t) C1(9, 3) };
                        if (memcmp(c1, rdp.cc1, 8) != 0) {
                            memcpy(rdp.cc1, c1, 8);
                            tri_state.valid = false;
                        }
                    }
                    if (memcmp(c2, rdp.cc2, 8) != 0 || rdp.cc2_pass != (rgb_pass && a_pass)) {
                        memcpy(rdp.cc2, c2, 8);
                        rdp.cc2_pass = rgb_pass && a_pass;
                        tri_state.valid = false;
                    }
                }
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
    gfx_clip_ratio = 2; /* each task starts from the microcode's default ratio */
    rsp.current_num_lights = 2;
    rsp.lights_changed = true;
    /* Each task starts with the fixed texgen axes the port always used (LOOKATX = x,
     * LOOKATY = y), so a frame without gSPLookAt draws as before. OoT sets a lookat
     * before every hilite (z_actor.c func_8002E830). */
    memset(rsp.current_lookat, 0, sizeof(rsp.current_lookat));
    rsp.current_lookat[1].dir[0] = 127;
    rsp.current_lookat[0].dir[1] = 127;
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
    { /* OOT_BOOTSYNC (goldeneye-007 round 17, "Bond missing from the gun barrel"): the soft backend starts with
       * depth test and depth write on, gfx_pc's cached rendering_state with them off. A draw sends a flag only when
       * it differs from the cache, so until the first depth-tested draw every draw was depth-tested and wrote depth.
       * Start the backend as the cache says. OOT_BOOTSYNC=0: as before. */
        const char *e = getenv("OOT_BOOTSYNC");
        if (!(e && e[0] == '0')) {
            gfx_rapi->set_depth_test(rendering_state.depth_test);
            gfx_rapi->set_depth_mask(rendering_state.depth_mask);
            gfx_rapi->set_zmode_decal(rendering_state.decal_mode);
            gfx_rapi->set_use_alpha(rendering_state.alpha_blend);
        }
    }
    { /* OOT_DRAWDIST=<clip.w units>: far cull, desktop tuning, off by default.
       * Validated: non-numeric, trailing junk, <= 0 or absurd stays OFF -- the
       * round-12 bare atof() turned "abc" into 0.0f and culled the whole 3D
       * scene with no log line. Read here, once, not in the per-frame gfx_run. */
        const char *e = getenv("OOT_DRAWDIST"); char *end = NULL;
        gfx_draw_dist_w = 1e30f;
        if (e && e[0]) {
            const double d = strtod(e, &end);
            if (end && *end == '\0' && d > 0.0 && d < 1e9) { gfx_draw_dist_w = (float)d; port_log("gfx: far cull at clip.w %.0f (OOT_DRAWDIST)\n", d); }
            else port_log("gfx: ignoring OOT_DRAWDIST='%s' (want a positive number)\n", e);
        }
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
    rendering_state.force_in = false;
    rendering_state.tex_lerp = false;
    rendering_state.lerp_v = false;
    rendering_state.own_edges = false;
    rendering_state.fog_px = false;
    {
        extern void gfx_soft_set_fog(bool on, int r, int g, int b, int a);
        gfx_soft_set_fog(false, 0, 0, 0, -1);
    }
    {
        extern void gfx_scegu_set_force_in(bool on);
        extern void gfx_scegu_set_tex_lerp(bool on, uint8_t r, uint8_t g, uint8_t b);
        extern void gfx_scegu_set_tex_lerpv(bool on);
        extern void gfx_soft_set_own_edges(bool on);
        gfx_scegu_set_force_in(false);
        gfx_scegu_set_tex_lerpv(false);
        gfx_scegu_set_tex_lerp(false, 0, 0, 0);
        gfx_soft_set_own_edges(false);
    }
    {
        extern void gfx_scegu_reset_texfunc_add(void);
        gfx_scegu_reset_texfunc_add(); // no GE call: the shader reload re-sends the texture function
    }
    rendering_state.textures[0] = rendering_state.textures[1] = NULL;
    memset(&rendering_state.viewport, 0, sizeof(rendering_state.viewport));
    memset(&rendering_state.scissor, 0, sizeof(rendering_state.scissor));
    rdp.viewport_or_scissor_changed = true;
    tri_state.valid = false;
    /* OOT_LOADSYNC (super-smash-bros 9e9bb19): the four flags above were reset in the cache only; the soft backend
     * kept what the last draw before the load had set (the state holds the game's memory, not the renderer's).
     * A draw sends a flag only when it differs from the cache, so a draw wanting the reset value (blend on,
     * depth off) was drawn with the backend's old one until some draw flipped it. Set the backend to what the
     * cache says. Runs only here (after a load). OOT_LOADSYNC=0: as before. */
    {
        static int on SAVESTATE_EXCLUDE = -1;
        if (on < 0) { const char *e = getenv("OOT_LOADSYNC"); on = !(e && e[0] == '0'); }
        if (on) {
            gfx_rapi->set_depth_test(rendering_state.depth_test);
            gfx_rapi->set_depth_mask(rendering_state.depth_mask);
            gfx_rapi->set_zmode_decal(rendering_state.decal_mode);
            gfx_rapi->set_use_alpha(rendering_state.alpha_blend);
        }
    }
}

/* Called right after a savestate load restores .data/.bss. The texture cache
 * (saved) now points into the arena (excluded/flushed), so drop the cache and
 * clear the backend arena; textures re-import from the restored course data on
 * the next frame (same coordinated flush the arena-full path uses each frame).
 * Then forget cached GE state so the whole pipeline is re-sent. */
void gfx_savestate_fixup(void) {
    gfx_texture_cache_reset(false);
    gfx_overlay_state_dirty();
}

void gfx_start_frame(void) {
    gfx_frame_counter++;
    memset(rsp_segments, 0, sizeof(rsp_segments));
#ifdef PORT_GE_TL
    memset(ge_last_mp, 0, sizeof(ge_last_mp)); // GE list reset each frame -> re-push GU_PROJECTION on the first batch
    ge_list_used = 0;
#endif
    gfx_hud_anchor = 0;
    hud_prev_class = -1;
    gfx_hud_el_cx = -1;
    gfx_hud_msg_mode = -1;
    gfx_hud_group_ax = gfx_hud_group_cref = -1;
    gfx_hud_group_leftcut = -1;
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
    //sceIoWrite(1, "----START FRAME!\n", 18);
    total_t0 = sceKernelLibcClock();
    gfx_wapi->handle_events();
}

/* A task the game submits without OS_SC_SWAPBUFFER (hh_sched.c) is drawn but never shown: the N64 keeps scanning out
 * the last swapped frame. MM's pause opens with two such frames (z_play.c, SREG(33) |= 1): the one that saves the
 * scene for the pause background, then one that only starts PreRender's filters and draws nothing -- shown, that
 * one was a black frame on every START. Not presented, not dumped (OOT_DUMP counts shown frames). MM_SWAPHOLD=0: as
 * before (every task shown). */
static bool sPresentHold SAVESTATE_EXCLUDE;
void gfx_set_present_hold(bool hold) {
    static int on SAVESTATE_EXCLUDE = -1;
    if (on < 0) { const char *e = getenv("MM_SWAPHOLD"); on = !(e && e[0] == '0'); }
    sPresentHold = on && hold;
}
static bool gfx_present_held(void) { return sPresentHold; }

void gfx_run(Gfx *commands) {
    gfx_sp_reset();
    gfx_cmd_budget = 0;
    gfx_debug_frame = gfx_trace_frames > 0; /* OOT_TRACE_AT: per-draw state too */
    
    //INFO_MSG("New frame");
    
    { /* OOT_LOADPIC (f-zero-x 8102b5d): a process with a state to load at boot (--loadStateFile, RESUME
       * GAME) drew and presented its own boot frames before the load, and that picture (the start of the boot logo)
       * stayed on screen through the load until the first frame after it. Those frames are dropped now: nothing is
       * shown until the resumed game's first picture. OOT_LOADPIC=0: as before. */
        static int on SAVESTATE_EXCLUDE = -1;
        extern int savestate_boot_load_pending(void);
        if (on < 0) { const char *e = getenv("OOT_LOADPIC"); on = !(e && e[0] == '0'); }
        if (on && savestate_boot_load_pending()) { dropped_frame = true; return; }
    }
    if (!gfx_wapi->start_frame()) {
        dropped_frame = true;
        return;
    }
    dropped_frame = false;
    //double t0 = gfx_wapi->get_time();
    unsigned int t0 = sceKernelLibcClock();
    gfx_rapi->start_frame();
    gfx_in_frame = true;
#ifndef HANDHELD
    gfx_trace_frame_no++;
#endif
    gfx_jpeg_room_squash = false;
    gfx_jpeg_room_pan = gHhCropPan3D ? gHhCropPan : 0.0f; /* the courtyard: from the frame's start, no picture */
#if defined(TARGET_HANDHELD)
    gfx_phase3d = true; /* the backend opens every frame on the internal target */
    gfx_seen_persp = false;
    if (gfx_ui_mode) { /* an unmatched UI tag must not leak into the next frame */
        extern void gfx_scegu_set_ui_mode(bool on);
        gfx_ui_mode = false;
        gfx_scegu_set_ui_mode(false);
    }
    memset(&rendering_state.viewport, 0, sizeof(rendering_state.viewport));
    memset(&rendering_state.scissor, 0, sizeof(rendering_state.scissor));
    rdp.viewport_or_scissor_changed = true;
    tri_state.valid = false;
#endif
    gfx_run_dl(commands);
#if !defined(TARGET_PSP) && !defined(PORT_GE_CULL) && !defined(PORT_GE_TL)
    gfx_modvtx_probe_frame();
#endif
    if (gfx_trace_frames > 0) {
        gfx_trace_frames--;
        port_log("dl done\n");
    }
    gfx_debug_frame = 0; // a one-frame switch (set by the input script / debug hooks before the frame)
    gfx_flush();
    {
        extern void port_gfx_overlay(void);
#ifndef PORT_NO_FPS
        if (gPortShowFps) port_gfx_overlay(); // FPS counter, off by default; hold SELECT 3 s to toggle
#endif
    }
    gfx_rapi->end_frame();
    gfx_in_frame = false;
    if (!gfx_present_held()) gfx_wapi->swap_buffers_begin();
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
     * kicking in. */
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
