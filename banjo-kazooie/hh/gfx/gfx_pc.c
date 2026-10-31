#if defined(TARGET_PSP) || defined(TARGET_HANDHELD)
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
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

/* BK_VHASH=<file> (debug; the lead's x86 = ARM check): one line per frame of FNV-1a hashes --
 * the vertex input (Vtx), the matrices (raw Mtx), the raw RDP triangles, the transformed
 * vertices and the rasterizer's triangle buffer -- to find where the desktop proxy and the
 * device part. */
static FILE *gfx_vh_file SAVESTATE_EXCLUDE;
static int gfx_vh_on SAVESTATE_EXCLUDE = -1;
static uint32_t gfx_vh[5], gfx_vh_frame;
static uint32_t gfx_vh_fnv(uint32_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *) p;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}
static int gfx_vh_dumpframe(void) { /* BK_VHASH_DUMP=<n>: frame n's matrices and vertices word by word */
    static int f = -2;
    if (f == -2) { const char *e = getenv("BK_VHASH_DUMP"); f = e ? atoi(e) : -1; }
    return f;
}
static inline bool gfx_vh_enabled(void) {
    if (gfx_vh_on < 0) {
        const char *e = getenv("BK_VHASH");
        gfx_vh_file = (e && e[0]) ? fopen(e, "w") : NULL;
        gfx_vh_on = gfx_vh_file != NULL;
        for (int i = 0; i < 5; i++) gfx_vh[i] = 2166136261u;
    }
    return gfx_vh_on;
}

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

/* GoldenEye: the size of the frame the game draws -- 320x240 in play, 440x330 in the
 * menus -- from gDPSetColorImage's width (4:3). Every N64 screen coordinate (viewport,
 * scissor, rectangles) is in this space. */
static int gfx_n64_w = 320, gfx_n64_h = 240;
int gfx_pc_frame_w(void) { return gfx_n64_w; } /* gfx_soft.c fill_edge_strip */
#define SCREEN_WIDTH gfx_n64_w
#define SCREEN_HEIGHT gfx_n64_h
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
	short u, v; /* S10.5: texrects may start left of / above texel 0 (ssb64 4843c2c) or step backwards
	               to it: GE draws its upside-down images (the file select's "SELECT FILE", Copy and
	               Erase icons, the cursor) from t = height - 1 down to -1, which wrapped to 2047 */
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
    Light_t current_lookat[2]; // [0] = G_MV_LOOKATY, [1] = G_MV_LOOKATX, as the F3D DMEM holds them (BK_LOOKAT)
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
    const uint8_t *palette;
    const uint8_t *tlut_base; /* the last TLUT load: its source and TMEM word address (palette = base + entry) */
    uint16_t tlut_tmem;
    uint8_t tile_pal;         /* the render tile's palette number (CI4: entries 16 * pal on) */
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
        uint8_t fmt, siz, cms, cmt, shifts, shiftt, masks, maskt, pal;
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
    uint8_t prim_min_level;  // gDPSetPrimColor's m: the LOD's floor (the detail grain's weight)
    
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
    bool own_edges; // BK_FILLRULE: blended / alpha-tested without AA_EN, each pixel to one triangle of an edge
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
  struct RGBA k; /* the combiner at texel 0 (PORT_GE_RAWCC; gfx_soft.c SoftVtx) */
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
    uint8_t cms, cmt;                      // the render tile's wrap at the rebuild (tile 1 in detail mode): the UV drop's
    bool use_fog;                          // cycle-1 blender is CLR_FOG * A_SHADE: lerp the colour to the fog colour
    uint8_t fog_a;                         // its A input: G_BL_A_IN / A_FOG / A_SHADE / 0 (PORT_GE_RAWCC)
    bool tex_add;                          // GE ADD: texel + const_color (see the tint combiner in gfx_tri_rebuild_state)
    struct RGBA const_color;
} tri_state;

static size_t buf_vbo_len;
static size_t buf_num_vert;
static size_t buf_vbo_num_tris;

static struct GfxWindowManagerAPI *gfx_wapi SAVESTATE_EXCLUDE;
static struct GfxRenderingAPI *gfx_rapi SAVESTATE_EXCLUDE;

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
/* In front of the game's near plane (z < -w) but not behind the eye (w >= W_MIN), when the
 * near plane is not a clip plane (gfx_nearclip_on, below): clipped to the side planes to
 * keep the projection bounded, never rejected. */
#define Z_NEAR (0x1000)
/* Beyond the RSP's guard band, |x| or |y| > ratio * |w| (GoldenEye sets gSPClipRatio(
 * FRUSTRATIO_2) every frame, lv.c): sends the triangle to the clipper (R33, below). One bit
 * for all four sides, so never part of the shared-flag early reject. */
#define GB_OUT (0x2000)
/* R33 (DKR 467ad1e, the user's floor flash): the shade is stepped on screen, so where a
 * triangle is cut changes its shading. F3D 2.0G clips at its guard band, the far plane and
 * w < 0 (gmain.s clip mask 0x4343; round 9) and a triangle goes to the clipper when a vertex
 * crosses one of those. The kit also cut at the exact screen edges, but only once a near or
 * far test had fired, so a big floor triangle's shading jumped the frame that started.
 * BK_CLIPEDGES: 2 = the RSP's (default), 1 = the kit's screen edges on its old trigger,
 * 0 = no side planes (A/B). */
static float gfx_clip_ratio SAVESTATE_EXCLUDE = 2.0f;
static int gfx_clipedges SAVESTATE_EXCLUDE = -1;
static inline int gfx_clipedges_mode(void) {
    if (gfx_clipedges < 0) { const char *e = getenv("BK_CLIPEDGES"); gfx_clipedges = (e && (e[0] == '0' || e[0] == '1')) ? e[0] - '0' : 2; }
    return gfx_clipedges;
}

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
	{  0.f,  0.f, -1.f, -1.f }	// z >= -w: the near plane, where the RSP clips (the kit pulled it in to
	                            // -0.97 w for the PSP GE; the SM64 port 5cd6699)
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
/* BK_NEARW (default on; gfx_soft.c, with the depth path it needs): the N64's plane, w >= 0, as w >=
 * 1/64 -- the sliver nearer than 1/64 of a unit is the only difference, and the guard band below
 * keeps the projection of anything near w = 0 inside +-2 screens. 1/64 bounds 1/w at 64: a
 * perspective-shaded colour times 1/w stays inside 16.16 (255 * 64 * 65536 < 2^31). The kit's 2.0
 * (MK64's units) cut whatever came within 2 units of the eye: the Dam tower's block by the outer
 * stairs, explosions' smoke at the camera. BK_NEARW=0: 2.0. */
extern int gfx_soft_nearw_on(void);
static float sWMin SAVESTATE_EXCLUDE = -1.0f;
static inline float gfx_wmin(void) {
    if (sWMin < 0.0f) sWMin = gfx_soft_nearw_on() ? 1.0f / 64.0f : 2.0f;
    return sWMin;
}
#define W_MIN gfx_wmin()
/* The GE does no polygon clipping: with GU_CLIP_PLANES on it DISCARDS any
 * triangle that has a vertex outside the depth range (clip.z outside +-clip.w).
 * The RSP instead clips such triangles at the near/far planes and draws the
 * rest.  So the port must clip at the game's real near plane (z_ndc = -1,
 * gCourseNearPersp = 9 units on most courses), not merely "behind the eye" --
 * otherwise a big road triangle with one vertex closer than 9 units vanishes
 * whole and the backdrop shows through (Turnpike / DK Jungle foreground gap).
 * BK_DEPTH_EPS pulls the planes fractionally inward so a CPU-clipped vertex
 * re-transformed by the GE never rounds to just outside the range. */
#ifndef BK_DEPTH_EPS
#define BK_DEPTH_EPS 0.001f
#endif
#ifndef BK_TL_NEAR
#define BK_TL_NEAR 0.05f   /* BK_TL near-clip plane (clip.w); as small as possible to keep road under the kart, above the clip.w->0 blow-up */
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

/* Seven passes bounce between the two buffers; *result gets the one holding the output
 * (round-24 bug f: gfx_clip_single_vert fanned from v0, which after the last pass holds the
 * near-plane output with the far pass's count -- geometry past the far plane, e.g. every
 * big outdoor floor, came out as garbage or not at all). */
/* The near plane. GoldenEye's RSP microcode (F3D, "RSP SW Version: 2.0G, 09-30-96") never
 * clips at it: the vertex clip codes it tests (gmain.s: trivial reject 0x7030, clip 0x4343)
 * are the four side planes, the guard band and the far plane, plus w < 0. Geometry between
 * the eye and the near plane is drawn, and the RDP clamps its negative depth to 0 (in front
 * of everything) -- as raster_tri's clamp16_sh8 does. Clipping there (the kit's rule, right
 * for other microcodes) cut holes in whatever passed close to the camera: the Dam intro's
 * truck (its cab, bumper and wheels at 11-34 units, the near plane at 30) showed the cliff
 * through it where the z64 LLE reference draws it whole. So GoldenEye clips only behind the
 * eye (clipToNearW, w >= W_MIN). BK_NEARCLIP=1 restores the z = -w plane (A/B). */
static int gfx_nearclip SAVESTATE_EXCLUDE = -1;
static inline bool gfx_nearclip_on(void) {
    if (gfx_nearclip < 0) { const char *e = getenv("BK_NEARCLIP"); gfx_nearclip = (e != NULL && e[0] == '1'); }
    return gfx_nearclip != 0;
}

uint32_t clip_to_frustum( struct LoadedVertex * v0, struct LoadedVertex * v1, uint32_t vIn, struct LoadedVertex **result )
{
	uint32_t vOut = vIn;
	struct LoadedVertex *src = v0, *dst = v1, *t;
	/* each pass writes the other buffer; *result is where the last one ended */
#define CLIP_PASS(call) do { vOut = (call); t = src; src = dst; dst = t; } while (0)

	// Near plane (w-based) first: removes behind-camera parts so the NDC-plane
	// clips below see only w>0 vertices.
	CLIP_PASS( clipToNearW( dst, src, vOut ) );
	if (gfx_clipedges_mode() == 2) {	// the RSP's guard band (R33)
		const float r = gfx_clip_ratio;
		const float gp[4][4] = { { 1.f, 0.f, 0.f, -r }, { -1.f, 0.f, 0.f, -r }, { 0.f, 1.f, 0.f, -r }, { 0.f, -1.f, 0.f, -r } };
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, gp[0] ) );
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, gp[1] ) );
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, gp[2] ) );
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, gp[3] ) );
	} else if (gfx_clipedges_mode() == 1) {	// the kit's exact screen edges
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[2] ) );		// right
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[1] ) );		// left
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[4] ) );		// top
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[3] ) );		// bottom
	}
	CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[0] ) );		// z <= w: the far plane
	if (gfx_nearclip_on())	// z >= -w: the near plane, not a clip plane for GE's F3D
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[5] ) );
#undef CLIP_PASS

	*result = src;
	return vOut;
}

/* Fog split (round 41, the user: Surface 2's snow "flashes" walking; "add the fix and add it to the
 * bench"). The RSP writes each vertex's fog factor clamped to 0..255, and the fog blend then steps that
 * clamped value across the triangle. The unclamped factor, z/w * mul + offset, is affine on screen, so a
 * ground triangle whose near corner sits far below 0 (under Bond's feet) had its whole near half
 * pulled toward that corner's clamp: the snow's fog swung with every step and head bob (the z64 and
 * Glide64 references show the same waves: this is the game's, and the fix is an enhancement the user
 * asked for). A fogged triangle whose corners straddle 0 or 255 is cut there -- the planes
 * z * mul + (offset - k) * w = 0 in clip space -- and each piece's corners get the exact factor, so the
 * per-vertex fold is the per-pixel fog: clear, the ramp, or full fog. Edges are cut from a canonical
 * endpoint so neighbours share bit-identical cut points. BK_FOGPX: unset = Surface 1 and 2 only, 1 = every
 * level, 0 = none (the game's own look). */
static int sFogPx = -1;
static inline float fog_raw(const struct LoadedVertex *v) {
    return v->_z / v->_w * rsp.fog_mul + rsp.fog_offset;
}
static inline void fog_set_a(struct LoadedVertex *v) {
    float f = fog_raw(v);
    if (f < 0.0f) f = 0.0f;
    if (f > 255.0f) f = 255.0f;
    v->color.a = (uint8_t)(int)f;
}
static inline bool fog_vless(const struct LoadedVertex *a, const struct LoadedVertex *b) {
    if (a->_x != b->_x) return a->_x < b->_x;
    if (a->_y != b->_y) return a->_y < b->_y;
    return a->_z < b->_z;
}
/* keep the part with fog_raw * sg <= k * sg (sg = +1: f <= k, -1: f >= k); pcut: the triangle's shade is stepped
 * perspective-correct (BK_PSHADE), so a cut corner takes the clip-space colour */
static int fog_clip(struct LoadedVertex *out, const struct LoadedVertex *in, int n, float k, float sg, bool pcut) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const struct LoadedVertex *a = &in[i], *b = &in[(i + 1) % n];
        const float da = (a->_z * rsp.fog_mul + (rsp.fog_offset - k) * a->_w) * sg;
        const float db = (b->_z * rsp.fog_mul + (rsp.fog_offset - k) * b->_w) * sg;
        if (da <= 0.0f) out[m++] = *a;
        if ((da < 0.0f && db > 0.0f) || (da > 0.0f && db < 0.0f)) {
            /* the cut from the canonical endpoint: the same bits from either neighbour */
            const struct LoadedVertex *p = a, *q = b;
            float dp = da, dq = db;
            if (fog_vless(b, a)) { p = b; q = a; dp = db; dq = da; }
            const float t = dp / (dp - dq);
            gfx_clip_interpolate_vert(&out[m], p, q, t);
            /* The cut corner takes the colour the unsplit triangle has there, so each piece carries its
             * gradient and no crease shows along the cut (round 43, the user: "the hard seams of each lit
             * triangle"). Where the shade is stepped linearly on screen (the RDP's rule: BK_PSHADE=0, or an
             * untextured or no-depth draw) that is the colour at its screen position (s); where gfx_soft
             * steps it perspective-correct (BK_PSHADE, textured depth-tested: the snow) it is the
             * clip-space one (t) -- the screen-position colour there bent each piece's gradient toward its
             * far corner, and the error moved with every step: Surface 2's snow waved in brightness with
             * the split on (2026-10-04, BK_FOGPXSHADE=0: the old rule). Position and u/v stay clip-space. */
            const float den = (1.0f - t) * p->_w + t * q->_w;
            const float s = pcut ? t : den > 0.0f ? t * q->_w / den : t;
            out[m].color.r = (uint8_t)(p->color.r + (q->color.r - p->color.r) * s + 0.5f);
            out[m].color.g = (uint8_t)(p->color.g + (q->color.g - p->color.g) * s + 0.5f);
            out[m].color.b = (uint8_t)(p->color.b + (q->color.b - p->color.b) * s + 0.5f);
            out[m].clip_rej = 0;
            m++;
        }
    }
    return m;
}
static struct LoadedVertex sFogOut[288];
static struct LoadedVertex *sFogPtr[288];
static unsigned long sFogSplitN;
static void fog_split_report(void) { fprintf(stderr, "[gfx] BK_FOGPX: %lu triangles split\n", sFogSplitN); }
/* tris: n vertices, 3 a triangle. Returns the new count (sFogPtr), or 0 when nothing was split. */
static size_t fog_split(struct LoadedVertex **tris, size_t n, bool pcut) {
    size_t o = 0;
    bool any = false;
    for (size_t t = 0; t + 3 <= n; t += 3) {
        struct LoadedVertex *v[3] = { tris[t], tris[t + 1], tris[t + 2] };
        bool split = false;
        if (v[0]->_w > 0.0f && v[1]->_w > 0.0f && v[2]->_w > 0.0f) {
            /* only triangles big on screen: on a small one the clamp's error covers a few pixels, and
             * the Cradle's thousands of small fogged triangles cost +13% to split; 32 px: +3.6% (BK_FOGPXMIN=<px>) */
            static float sMinPx = -1.0f;
            if (sMinPx < 0.0f) { const char *e = getenv("BK_FOGPXMIN"); sMinPx = e ? (float)atof(e) : 32.0f; }
            const float sx = 0.5f * (float)rdp.viewport.width, sy = 0.5f * (float)rdp.viewport.height;
            const float x0 = v[0]->_x / v[0]->_w * sx, x1 = v[1]->_x / v[1]->_w * sx, x2 = v[2]->_x / v[2]->_w * sx;
            const float y0 = v[0]->_y / v[0]->_w * sy, y1 = v[1]->_y / v[1]->_w * sy, y2 = v[2]->_y / v[2]->_w * sy;
            const float bw = fmaxf(x0, fmaxf(x1, x2)) - fminf(x0, fminf(x1, x2));
            const float bh = fmaxf(y0, fmaxf(y1, y2)) - fminf(y0, fminf(y1, y2));
            if (bw < sMinPx && bh < sMinPx) goto nosplit;
            const float f0 = fog_raw(v[0]), f1 = fog_raw(v[1]), f2 = fog_raw(v[2]);
            const float lo = fminf(f0, fminf(f1, f2)), hi = fmaxf(f0, fmaxf(f1, f2));
            split = (lo < 0.0f && hi > 0.0f) || (lo < 255.0f && hi > 255.0f);
        }
    nosplit:
        if (!split) {
            if (o + 3 > 288) return 0;
            for (int k = 0; k < 3; k++) { sFogOut[o] = *v[k]; o++; }
            continue;
        }
        any = true;
        sFogSplitN++;
        struct LoadedVertex tri[3] = { *v[0], *v[1], *v[2] }, a[8], b[8];
        struct { float k, sg; int two; float k2, sg2; } reg[3] = {
            { 0.0f, 1.0f, 0, 0, 0 },         /* f <= 0 */
            { 0.0f, -1.0f, 1, 255.0f, 1.0f }, /* 0 <= f <= 255 */
            { 255.0f, -1.0f, 0, 0, 0 },      /* f >= 255 */
        };
        for (int r = 0; r < 3; r++) {
            int m = fog_clip(a, tri, 3, reg[r].k, reg[r].sg, pcut);
            struct LoadedVertex *poly = a;
            if (reg[r].two && m >= 3) { m = fog_clip(b, a, m, reg[r].k2, reg[r].sg2, pcut); poly = b; }
            if (m < 3) continue;
            for (int i = 0; i < m; i++) fog_set_a(&poly[i]);
            for (int i = 1; i + 1 < m; i++) {
                if (o + 3 > 288) return 0;
                sFogOut[o++] = poly[0]; sFogOut[o++] = poly[i]; sFogOut[o++] = poly[i + 1];
            }
        }
    }
    if (!any) return 0;
    for (size_t i = 0; i < o; i++) sFogPtr[i] = &sFogOut[i];
    return o;
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

    struct LoadedVertex *res;
    uint32_t out = clip_to_frustum( temp_a, temp_b, 3, &res );
    if( out < 3 ){
        *p_num_vertices = 0;
        return;
    }

    // Retesselate.  out <= 10 (3 verts + 7 clip planes), so this emits at most
    // 3*(10-2)=24 vertices; cap defensively at the caller's buffer size (32) so a
    // miscount can never overflow _clipped_vertices/ptr_clipped_vertices again.
    for( uint32_t j = 0; j <= out - 3 && clipped_vertices_num + 3 <= 32; ++j )
    {
        p_p_vertices[clipped_vertices_num++] = ( res[ 0 ] );
        p_p_vertices[clipped_vertices_num++] = ( res[ j + 1 ] );
        p_p_vertices[clipped_vertices_num++] = ( res[ j + 2 ] );
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
#ifndef BK_LIST_RECYCLE
#define BK_LIST_RECYCLE (400u * 1024u) /* recycle before the 512KB list fills (max batch ~73KB) */
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
        if (ge_list_used + buf_vbo_len + 4096u > BK_LIST_RECYCLE) {
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
        if (gfx_vh_enabled()) gfx_vh[4] = gfx_vh_fnv(gfx_vh[4], buf_vbo, buf_vbo_len);
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

/* Pre-blended two-texture combiners (gfx_blend_tiles): texture ids from the
 * same pool as the cache, so a cache reset drops them too. */
#define GFX_BLEND_CACHE 32
static struct {
    uint32_t key[12];
    uint32_t texture_id;
    uint32_t last_used_frame;
    bool valid;
} gfx_blend_cache[GFX_BLEND_CACHE];

/* Bumped by every texture cache reset (round 20, OoT 2b69f7b: the v35 sunset drawn as a red bar after
 * a long session). A reset wipes every texture, including the one the OTHER tile of the draw being set
 * up already holds: importing tile 1 could reset the cache and leave rendering_state.textures[0] on a
 * zeroed pool node -- or, the soft backend's ids restarting at 1, on an id a new texture now owns: the
 * draw went out untextured or with the wrong texture. The reset drops both bindings, and the import
 * loops run again when the epoch changes under them. BK_EPOCHFIX=0: before (A/B). */
static uint32_t gfx_cache_epoch SAVESTATE_EXCLUDE;
static int gfx_epochfix SAVESTATE_EXCLUDE = -1;
static bool gfx_epochfix_on(void) {
    if (gfx_epochfix < 0) { const char *e = getenv("BK_EPOCHFIX"); gfx_epochfix = !(e && e[0] == '0'); }
    return gfx_epochfix != 0;
}
static int gfx_tex_stress_hold; /* the re-run pass: BK_TEXSTRESS forces no reset (a real one can't fire on an empty arena) */

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
static void gfx_palette_update(uint32_t siz);
static uint8_t gfx_rtile; /* the render tile: G_TEXTURE's tile (0 unless a display list names another) */

static inline bool gfx_is_static_memory(const void *p) {
    const char *c = (const char *) p;
    if (c >= course_tex_lo && c < course_tex_hi) {
        return !(c >= fb_tile_lo && c < fb_tile_hi);
    }
    return (c >= _ftext && c < _fbss) || (c >= __assets_start && c < __assets_end);
}

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
 * (tri_state.u_scale); the pre-blends build in render-tile texels, so they sample tile 1
 * at its shift RELATIVE to the render tile's (the OoT port 5c60b2c, R29). The Dam draws 9%
 * of its textured triangles at shift 15/15 (x2), and with R29 its cliff rock came out twice
 * as fine: the user preferred the cliffs without it (2026-09-24), so GoldenEye keeps it OFF
 * -- shifts ignored except tile 1's own, as before. BK_TILESHIFT=1 applies R29 (A/B). */
static int gfx_tileshift SAVESTATE_EXCLUDE = -1;
static inline bool gfx_tileshift_on(void) {
    if (gfx_tileshift < 0) { const char *e = getenv("BK_TILESHIFT"); gfx_tileshift = (e && e[0] == '1'); }
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
/* The factor a tile shift applies to texture coordinates. */
static float gfx_tile_shift_scale(uint8_t shift) {
    if (!gfx_tileshift_on() || shift == 0 || shift > 15) return 1.0f;
    return shift <= 10 ? 1.0f / (float) (1u << shift) : (float) (1u << (16 - shift));
}

static int gfx_tilemask SAVESTATE_EXCLUDE = -1; /* OOT_TILEMASK=0: the old whole-tile import (A/B) */
static inline bool gfx_tilemask_on(void) {
    if (gfx_tilemask < 0) { const char *e = getenv("BK_TILEMASK"); gfx_tilemask = !(e && e[0] == '0'); }
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
    /* MIRROR|CLAMP with the tile no wider than the mask period: the RDP clamps first, the mirror bit is never
     * reached: a plain clamp (miyoo64 MM 1.0.4, "extra eyes") */
    if ((cm & G_TX_CLAMP) && (cm & G_TX_MIRROR) && (mask == 0 || mask >= 16 || extent <= (1 << mask))) cm &= (uint8_t) ~G_TX_MIRROR;
    return cm;
}
static inline void gfx_render_tile_cm(void) {
    rdp.texture_tile.cms = gfx_tile_cm(rdp.texture_tile.cms_raw, rdp.texture_tile.masks, rdp.texture_tile.uls, rdp.texture_tile.lrs);
    rdp.texture_tile.cmt = gfx_tile_cm(rdp.texture_tile.cmt_raw, rdp.texture_tile.maskt, rdp.texture_tile.ult, rdp.texture_tile.lrt);
}

static inline void gfx_tile_extent(int tile, uint32_t *width, uint32_t *height);
/* BK_TILECENSUS=1: how often gfx_tile_size's scroll rule below changes a size, and on
 * which textures (printed at exit). */
static int gfx_tilecensus SAVESTATE_EXCLUDE = -1;
static uint32_t gfx_tilecensus_n SAVESTATE_EXCLUDE;
static uint32_t gfx_tilecensus_seen[32][6] SAVESTATE_EXCLUDE; /* addr, fmt|siz, masks|maskt, w0, h0, count */
static int gfx_tilecensus_used SAVESTATE_EXCLUDE;
static uint32_t gfx_shiftcensus_tris SAVESTATE_EXCLUDE, gfx_shiftcensus_all SAVESTATE_EXCLUDE; /* R29 */
static uint32_t gfx_shiftcensus_pairs[16][2] SAVESTATE_EXCLUDE; /* shifts|shiftt << 8, triangles */
static int gfx_shiftcensus_used SAVESTATE_EXCLUDE;
static uint32_t gfx_zcensus_nocmp SAVESTATE_EXCLUDE, gfx_zcensus_updonly SAVESTATE_EXCLUDE; /* gfx_depth_test_on */
static uint32_t gfx_fogcensus_v[2][2] SAVESTATE_EXCLUDE; /* R35: fogged vertices [exact fold][fog colour not black] */
static uint32_t gfx_clipcensus[3] SAVESTATE_EXCLUDE; /* R33: triangles clipped, sent by GB_OUT alone, by Z_NEAR alone */
static uint32_t gfx_ztexrect_n SAVESTATE_EXCLUDE; /* R34: texrects drawn while the colour image is the Z image */
static uint32_t gfx_blend_census[3] SAVESTATE_EXCLUDE; /* pre-blends built: all, tile 1 = tile 0's image, fractional offset */
static void gfx_tilecensus_report(void) {
    fprintf(stderr, "[fog] %u triangles fogged: exact fold (colour mode 3) %u with a coloured fog, %u black; approximate fold %u coloured, %u black\n",
            (unsigned) ((gfx_fogcensus_v[0][0] + gfx_fogcensus_v[0][1] + gfx_fogcensus_v[1][0] + gfx_fogcensus_v[1][1]) / 3),
            (unsigned) (gfx_fogcensus_v[1][1] / 3), (unsigned) (gfx_fogcensus_v[1][0] / 3), (unsigned) (gfx_fogcensus_v[0][1] / 3),
            (unsigned) (gfx_fogcensus_v[0][0] / 3));
    fprintf(stderr, "[clip] %u triangles clipped; %u sent by the guard band alone, %u by the near plane alone (R33)\n",
            (unsigned) gfx_clipcensus[0], (unsigned) gfx_clipcensus[1], (unsigned) gfx_clipcensus[2]);
    fprintf(stderr, "[ztexrect] %u texrects into the Z image (R34)\n", (unsigned) gfx_ztexrect_n);
    fprintf(stderr, "[blend] %u two-tile pre-blends built; %u of tile 0's own image at tile 1's corner (BK_BLENDSAME), %u at a fractional tile-1 offset (BK_BLENDFRAC)\n",
            (unsigned) gfx_blend_census[0], (unsigned) gfx_blend_census[1], (unsigned) gfx_blend_census[2]);
    { extern uint32_t gfx_soft_spanskip_spans, gfx_soft_spanskip_px;
      fprintf(stderr, "[spanskip] %u sprite spans skipped, all taps alpha 0 (%u px; gfx_soft.c span_clear)\n",
              (unsigned) gfx_soft_spanskip_spans, (unsigned) gfx_soft_spanskip_px); }
    fprintf(stderr, "[tileshift] %u of %u textured triangles drawn with a render-tile shift\n", (unsigned) gfx_shiftcensus_tris, (unsigned) gfx_shiftcensus_all);
    for (int i = 0; i < gfx_shiftcensus_used; i++)
        fprintf(stderr, "[tileshift]   shifts %u shiftt %u: %u triangles\n", (unsigned) (gfx_shiftcensus_pairs[i][0] & 0xff),
                (unsigned) (gfx_shiftcensus_pairs[i][0] >> 8), (unsigned) gfx_shiftcensus_pairs[i][1]);
    fprintf(stderr, "[zcmp] %u state rebuilds under G_ZBUFFER without Z_CMP (not depth-tested), %u of them with Z_UPD\n",
            (unsigned) gfx_zcensus_nocmp, (unsigned) gfx_zcensus_updonly);
    fprintf(stderr, "[tilescroll] rule changed %u tile sizes, %d distinct textures\n", (unsigned) gfx_tilecensus_n, gfx_tilecensus_used);
    for (int i = 0; i < gfx_tilecensus_used; i++)
        fprintf(stderr, "[tilescroll]   addr %08X fmt/siz %02X masks/maskt %04X corner-to-corner %ux%u, %u times\n",
                (unsigned) gfx_tilecensus_seen[i][0], (unsigned) gfx_tilecensus_seen[i][1], (unsigned) gfx_tilecensus_seen[i][2],
                (unsigned) gfx_tilecensus_seen[i][3], (unsigned) gfx_tilecensus_seen[i][4], (unsigned) gfx_tilecensus_seen[i][5]);
}

/* Set by gfx_tile_size when the render tile's wrap period is wider than its TMEM line: the
 * RDP then reads each row on past the line's end into the next one (import keeps the line
 * as the row pitch). */
static bool gfx_tile_rows_overlap SAVESTATE_EXCLUDE;

static inline void gfx_tile_size(int tile, uint32_t *width, uint32_t *height) {
    gfx_tile_extent(tile, width, height);
    *width = gfx_mask_period(*width, rdp.texture_tile.masks);
    *height = gfx_mask_period(*height, rdp.texture_tile.maskt);
    gfx_tile_rows_overlap = false;
    /* A LoadBlock tile that wraps (mask set, clamp off) reads one whole period of its mask
     * from its corner, however far apart its corners are: the RDP subtracts the corner, then
     * masks, and the extent only bounds a clamp. Star Fox scrolls its ground and sea by
     * moving the upper-left corner alone, so the corner-to-corner size shrank the texture as
     * it scrolled (the Star Fox 64 port 7725ecc). Only while the loaded block holds the whole period.
     * Parked 2026-09-24; on since round 12 for GoldenEye's sea (the Frigate's two water
     * tiles have zero-size corners and 32x32 masks: the port drew one texel of it, green).
     * BK_TILESCROLL=0: off. */
    static int sTileScroll SAVESTATE_EXCLUDE = -1;
    if (sTileScroll < 0) { const char *e = getenv("BK_TILESCROLL"); sTileScroll = !(e && e[0] == '0'); }
    if (gfx_tilecensus < 0) {
        const char *e = getenv("BK_TILECENSUS");
        gfx_tilecensus = e && e[0] == '1';
        if (gfx_tilecensus) atexit(gfx_tilecensus_report);
    }
    if ((sTileScroll || gfx_tilecensus) && rdp.loaded_texture[gfx_tex_slot(tile)].stride_bytes == 0 && gfx_tilemask_on()) {
        const uint8_t ms = rdp.texture_tile.masks, mt = rdp.texture_tile.maskt;
        const uint32_t line = rdp.texture_tile.line_size_bytes * (rdp.texture_tile.siz == G_IM_SIZ_32b ? 2u : 1u);
        const uint32_t loaded = rdp.loaded_texture[gfx_tex_slot(tile)].size_bytes;
        const uint32_t w0 = *width, h0 = *height;
        uint32_t w = w0, h = h0;
        bool overlap = false;
        if (mt != 0 && mt < 16 && !(rdp.texture_tile.cmt_raw & G_TX_CLAMP) && h < (1u << mt) && line != 0 &&
            (1u << mt) * line <= loaded) {
            h = 1u << mt;
        }
        /* S: a row is read from its start at the line's pitch -- on past the line's end into
         * the next row when the period is wider (the sea: 32 RGBA16 texels, 16-texel lines),
         * as long as that whole read lies inside the loaded block */
        if (ms != 0 && ms < 16 && !(rdp.texture_tile.cms_raw & G_TX_CLAMP) && w < (1u << ms) && line != 0 &&
            (h - 1) * line + tex_bytes_per_row(1u << ms, rdp.texture_tile.siz) <= loaded) {
            w = 1u << ms;
            overlap = tex_bytes_per_row(w, rdp.texture_tile.siz) > line;
        }
        if ((w != w0 || h != h0) && gfx_tilecensus) {
            const uint32_t a = (uint32_t) (uintptr_t) rdp.loaded_texture[gfx_tex_slot(tile)].addr;
            int i;
            gfx_tilecensus_n++;
            for (i = 0; i < gfx_tilecensus_used && gfx_tilecensus_seen[i][0] != a; i++) {}
            if (i == gfx_tilecensus_used && i < 32) {
                gfx_tilecensus_used++;
                gfx_tilecensus_seen[i][0] = a;
                gfx_tilecensus_seen[i][1] = (uint32_t) (rdp.texture_tile.fmt << 4 | rdp.texture_tile.siz);
                gfx_tilecensus_seen[i][2] = (uint32_t) (ms << 8 | mt);
                gfx_tilecensus_seen[i][3] = w0;
                gfx_tilecensus_seen[i][4] = h0;
            }
            if (i < 32) gfx_tilecensus_seen[i][5]++;
        }
        if (sTileScroll) { *width = w; *height = h; gfx_tile_rows_overlap = overlap; }
    }
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
        *width = (uint32_t) (w < 1 ? 1 : w);
        *height = (uint32_t) (h < 1 ? 1 : h);
    }
}

/* With the TLUT on, the RDP looks every texel up in it whatever the tile's format says; a
 * 16-bit texel by its high byte (MAME n64 fetch_rgba16_tlut0/1). GoldenEye's sea reads its
 * CI8 water through two RGBA16 tiles that way: the palette's dark blues -- decoded as colour
 * the raw index bytes drew it green (the user's Frigate report, round 12). BK_TLUT16=0: off. */
static inline bool gfx_tlut16(uint32_t siz) {
    static int on SAVESTATE_EXCLUDE = -1;
    if (on < 0) { const char *e = getenv("BK_TLUT16"); on = !(e && e[0] == '0'); }
    return on && siz == G_IM_SIZ_16b && (rdp.other_mode_h & (3U << G_MDSFT_TEXTLUT)) != G_TT_NONE && rdp.palette != NULL;
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
        if ((fmt == G_IM_FMT_CI || gfx_tlut16(siz)) && rdp.palette != NULL) {
            if (!gfx_is_static_memory(rdp.palette)) {
                /* (the TLUT type too: the same CI texels decode differently as IA16) */
                return hash_bytes(h, rdp.palette, siz == G_IM_SIZ_4b ? 32 : 512) ^ ((rdp.other_mode_h >> G_MDSFT_TEXTLUT) & 3);
            }
            // Static texels with a static palette: the palette's ADDRESS still
            // has to take part (Rainbow Road animates by cycling through
            // several TLUTs over the same CI texels).
            return ((uint32_t) (uintptr_t) rdp.palette | 1u) ^ (((rdp.other_mode_h >> G_MDSFT_TEXTLUT) & 3) << 1);
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
    if ((fmt == G_IM_FMT_CI || gfx_tlut16(siz)) && rdp.palette != NULL) {
        h = hash_bytes(h, rdp.palette, siz == G_IM_SIZ_4b ? 32 : 512) ^ ((rdp.other_mode_h >> G_MDSFT_TEXTLUT) & 3);
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

/* BK_TEXSTRESS=<n> (tests): reset the texture cache every n new imports, as a long session's full
 * arena does (OoT 2b69f7b's gfx_tex_stress) -- a reset only forces re-imports, so a stressed sweep
 * must draw the same frames as a normal one. */
static bool gfx_tex_stress(void) {
    static int sEvery = -1, sCount;
    if (sEvery < 0) { const char *e = getenv("BK_TEXSTRESS"); sEvery = e ? atoi(e) : 0; }
    return sEvery > 0 && !gfx_tex_stress_hold && ++sCount % sEvery == 0;
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
                gfx_rapi->set_sampler_parameters(0, (*node)->linear_filter, (*node)->cms, (*node)->cmt);
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
/* (No odd-row swap for dxt-0 block loads: GoldenEye's texSwapAltRowBytes, which pre-swaps
 * them for the RDP, is off under PORT, so textures stay linear in memory. DKR be3afca's
 * sampler swap would scramble them: the file select's photos.) */

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
    { static long want = -2; /* BK_TEXDUMP=<hex source address>: that texture's imports (tests) */
      if (want == -2) { const char *e = getenv("BK_TEXDUMP"); want = e ? (long) strtoul(e, NULL, 16) : -1; }
      if (want >= 0 && (uintptr_t) src == (uintptr_t) want) gfx_dump_textures = 1; else if (want >= 0) return; }
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
         * counts half a row. Never tighter than the width. (the SSB64 port
         * a55820b: Smash's HUD sprites 27-30 px wide in 32 px rows.) */
        stride = rdp.texture_tile.line_size_bytes;
        if (siz == G_IM_SIZ_32b) {
            stride *= 2;
        }
        /* (except where the wrap period is wider than the line on purpose: the RDP reads on
         * into the next row -- gfx_tile_size, GoldenEye's sea) */
        if (stride == 0 || (stride < tex_bytes_per_row(width, siz) && !gfx_tile_rows_overlap)) {
            stride = tex_bytes_per_row(width, siz);
        }
    }
    if (width * height > GFX_MAX_IMPORT_TEXELS) {
        SUPPORT_CHECK(width * height <= GFX_MAX_IMPORT_TEXELS);
        height = GFX_MAX_IMPORT_TEXELS / width;
    }
    if (gfx_debug_frame) port_log("  import fmt %d siz %d %ux%u stride %u addr %p\n", fmt, siz, width, height, stride, src);

    if (gfx_tlut16(siz)) { /* palette index = the texel's high byte (gfx_tlut16) */
        if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTLUT)) == G_TT_IA16) {
            for (y = 0; y < height; y++) {
                const uint8_t* row = TEXEL_ROW(y);
                for (x = 0; x < width; x++) {
                    const uint8_t *e = rdp.palette + row[x * 2] * 2;
                    out32[y * width + x] = (uint32_t) e[0] | ((uint32_t) e[0] << 8) | ((uint32_t) e[0] << 16) | ((uint32_t) e[1] << 24);
                }
            }
            gfx_upload_maybe_mirrored(out32, width, height, 4, GU_PSM_8888, mirror);
            gfx_debug_dump_texture(NULL, out32, width, height, fmt, siz, src);
            return;
        }
        for (y = 0; y < height; y++) {
            const uint8_t* row = TEXEL_ROW(y);
            for (x = 0; x < width; x++) {
                out16[y * width + x] = n64_rgba16_to_psp5551(be16(rdp.palette + row[x * 2] * 2));
            }
        }
        gfx_upload_maybe_mirrored(out16, width, height, 2, GU_PSM_5551, mirror);
        gfx_debug_dump_texture(out16, NULL, width, height, fmt, siz, src);
        return;
    }
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
        if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTLUT)) == G_TT_IA16) {
            /* IA16 palette (GoldenEye's guns): big-endian intensity then 8-bit alpha. */
            for (y = 0; y < height; y++) {
                const uint8_t* row = TEXEL_ROW(y);
                for (x = 0; x < width; x++) {
                    uint8_t idx = (siz == G_IM_SIZ_4b) ? nib(row, x) : row[x];
                    const uint8_t *e = rdp.palette + idx * 2;
                    out32[y * width + x] = (uint32_t) e[0] | ((uint32_t) e[0] << 8) | ((uint32_t) e[0] << 16) | ((uint32_t) e[1] << 24);
                }
            }
            gfx_upload_maybe_mirrored(out32, width, height, 4, GU_PSM_8888, mirror);
            gfx_debug_dump_texture(NULL, out32, width, height, fmt, siz, src);
            return;
        }
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

static void import_texture_fmt(int tile);
/* With the TLUT on, the RDP reads a 4- or 8-bit texel as a palette index whatever the tile's format says
 * (angrylion fetch_texel: tlut_en takes the CI path). BK's pause menu draws its item icons (egg, feathers,
 * jinjo, skull, the B button) from RGBA4/I8-tagged tiles with a TLUT loaded: decoded as RGBA4 they came out
 * as stripes or clear. BK_TLUTANY=0: off. */
static void import_texture(int tile) {
    static int on SAVESTATE_EXCLUDE = -1;
    if (on < 0) { const char *e = getenv("BK_TLUTANY"); on = !(e && e[0] == '0'); }
    const uint8_t f0 = rdp.texture_tile.fmt, z0 = rdp.texture_tile.siz;
    if (on && f0 != G_IM_FMT_CI && (z0 == G_IM_SIZ_4b || z0 == G_IM_SIZ_8b) && rdp.palette != NULL
        && (rdp.other_mode_h & (3U << G_MDSFT_TEXTLUT)) != G_TT_NONE) {
        rdp.texture_tile.fmt = G_IM_FMT_CI;
        import_texture_fmt(tile);
        rdp.texture_tile.fmt = f0;
        return;
    }
    import_texture_fmt(tile);
}

static void import_texture_fmt(int tile) {
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

/* Tile 1 on tile 0's own load, decoded the same way (format, size, line, TMEM address,
 * masks, no mirror): the same image, read from another corner. GoldenEye draws its water so
 * (unk_092E50.c): one image on both tiles, tile 1 (22.5, 37.5) texels past tile 0, lerped
 * by the prim LOD fraction, sin(t) * 127 + 128 (a 2.6 s swing at 30 fps): the N64's shimmer.
 * The Frigate's sea (the sky code, fixed corners), the Dam's and Complex's water (texture
 * 1511) and the Caverns' (1508), both corners scrolling. Tile 1 could not be imported apart,
 * so the port skipped the pre-blend and drew tile 0 alone: still water (round 14, the
 * Frigate). The palette is not compared (the port keeps one). BK_BLENDSAME=0: skip it (A/B). */
static int gfx_blendsame SAVESTATE_EXCLUDE = -1;
static bool gfx_tile1_same_image(void) {
    const __typeof__(rdp.tiles[1]) *t1 = &rdp.tiles[1];
    if (gfx_blendsame < 0) { const char *e = getenv("BK_BLENDSAME"); gfx_blendsame = !(e && e[0] == '0'); }
    return gfx_blendsame && t1->tmem == rdp.texture_tile.tmem && t1->fmt == rdp.texture_tile.fmt &&
           t1->siz == rdp.texture_tile.siz && t1->line_size_bytes == rdp.texture_tile.line_size_bytes &&
           t1->masks == rdp.texture_tile.masks && t1->maskt == rdp.texture_tile.maskt &&
           !((t1->cms | t1->cmt | rdp.texture_tile.cms_raw | rdp.texture_tile.cmt_raw) & G_TX_MIRROR);
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
 * 3 = t1 + (t0 - t1) * c, 4 = t0 * t1 (b and d zero: GoldenEye's explosions are an IA8 flame
 * times a 16x14 RGBA16 colour map on tile 1 -- without it they drew grey), -1 = mixes the
 * tiles in a form we cannot pre-blend. b0/d0: the b and d slots read zero. */
static int gfx_blend_mode(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t t0, uint8_t t1, uint8_t t0a, uint8_t t1a,
                          bool b0, bool d0) {
    bool uses0 = a == t0 || b == t0 || c == t0 || d == t0 || (t0a != 0xff && c == t0a);
    bool uses1 = a == t1 || b == t1 || c == t1 || d == t1 || (t1a != 0xff && c == t1a);
    if (!uses1) return 0;
    if (a == t1 && b == t0 && d == t0) return 2;
    if (a == t0 && b == t1 && d == t1) return 3;
    if (!uses0) return 1;
    if (b0 && d0 && ((a == t0 && c == t1) || (a == t1 && c == t0))) return 4;
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
                                    int krgb_src, const uint8_t krgb[3], int ka_src, uint8_t ka, int filt1,
                                    int dq_s, int dq_t, int mips);

/* Tile 1's corner past tile 0's in quarter texels beyond the whole ones (the corners are
 * U10.2). The pre-blend is built on tile 0's texels, so tile 1 was read at the whole-texel
 * offset: the sea's tile 1 sits half a texel off in both axes. Used where it is exact to
 * sample tile 1 bilinearly there: filtered and unshifted. BK_BLENDFRAC=0: whole texels (A/B). */
static int gfx_blendfrac SAVESTATE_EXCLUDE = -1;

/* Cycle 1 mixes TEXEL0 and TEXEL1 (OoT's room/water/effect combiners): bind a
 * cached pre-blend of both tiles as the one texture. Returns its id (0 = draw
 * tile 0 as before) and the blended texture size in *bw, *bh. */
static uint32_t gfx_blend_tiles(uint32_t *bw, uint32_t *bh) {
    const uint8_t *m = rdp.cc1;
    struct TextureHashmapNode *n0 = rendering_state.textures[0], *n1 = rendering_state.textures[1];
    uint8_t krgb[3] = { 0, 0, 0 }, ka = 0;
    int rgb_mode, a_mode, krgb_src = 0, ka_src = 0, i, pick = -1;
    uint32_t w0, h0, w1, h1, W, H, key[12];
    int iw1, ih1, dq_s = 0, dq_t = 0;
    /* same: tile 1 is tile 0's image from another corner (the waters). From the same corner
     * and unshifted it reads tile 0's texels exactly -- GE's LOD combiner with LOD off
     * (Streets, Runway, Jungle...): every mode but t0 * t1 gives tile 0, and drawing tile 0
     * keeps its mip levels, which a pre-blend has not got. So those stay tile 0 alone. */
    const bool importable = gfx_tile1_importable(),
               same = !importable && gfx_tile1_same_image() &&
                      !(rdp.tiles[1].uls == rdp.texture_tile.uls && rdp.tiles[1].ult == rdp.texture_tile.ult &&
                        gfx_tile1_shift(false) == 0 && gfx_tile1_shift(true) == 0);

    if (same) n1 = n0; /* tile 1's unit imported tile 0's descriptor: the same node */
    if (n0 == NULL || n1 == NULL || n0->mirror != 0 || n1->mirror != 0 || !(importable || same)) {
        if (gfx_debug_frame) port_log("  blend skip n0 %p n1 %p tmem %u/%u\n", (void *) n0, (void *) n1, rdp.texture_tile.tmem, rdp.tiles[1].tmem);
        return 0;
    }
    rgb_mode = gfx_blend_mode(m[0], m[1], m[2], m[3], G_CCMUX_TEXEL0, G_CCMUX_TEXEL1, G_CCMUX_TEXEL0_ALPHA, G_CCMUX_TEXEL1_ALPHA,
                              m[1] >= 8 /* B: 8-15 = 0 */, m[3] == 7 /* D: 7 = 0 */);
    a_mode = gfx_blend_mode(m[4], m[5], m[6], m[7], G_ACMUX_TEXEL0, G_ACMUX_TEXEL1, 0xff, 0xff, m[5] == 7, m[7] == 7);
    if (rgb_mode < 0 || a_mode < 0 || (rgb_mode == 0 && a_mode == 0)) {
        if (gfx_debug_frame) port_log("  blend no-form %d/%d cc1 %u %u %u %u / %u %u %u %u\n", rgb_mode, a_mode, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
        return 0;
    }
    if ((rgb_mode == 2 || rgb_mode == 3) && (krgb_src = gfx_blend_k_rgb(m[2], krgb)) < 0) return 0;
    if ((a_mode == 2 || a_mode == 3) && (ka_src = gfx_blend_k_alpha(m[6], &ka)) < 0) return 0;

    gfx_tile_size(0, &w0, &h0);
    iw1 = ((int) rdp.tiles[1].lrs - (int) rdp.tiles[1].uls + 4) / 4;
    ih1 = ((int) rdp.tiles[1].lrt - (int) rdp.tiles[1].ult + 4) / 4;
    w1 = gfx_mask_period((uint32_t) (iw1 < 1 ? 1 : iw1), rdp.tiles[1].masks);
    h1 = gfx_mask_period((uint32_t) (ih1 < 1 ? 1 : ih1), rdp.tiles[1].maskt);
    if (same) { /* one image: tile 0's period (the sea's tile 1 has zero-size corners too) */
        w1 = w0;
        h1 = h0;
    }
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
    /* tile 1 filtered as the RDP filters it (bilinear unless G_TF_POINT), clamped as it says */
    const int filt1 = ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT ? 1 : 0) |
                      ((rdp.tiles[1].cms & G_TX_CLAMP) ? 2 : 0) | ((rdp.tiles[1].cmt & G_TX_CLAMP) ? 4 : 0);
    if (gfx_blendfrac < 0) { const char *e = getenv("BK_BLENDFRAC"); gfx_blendfrac = !(e && e[0] == '0'); }
    if (gfx_blendfrac && (filt1 & 1) && gfx_tile1_shift(false) == 0 && gfx_tile1_shift(true) == 0) {
        dq_s = (int) (rdp.texture_tile.uls & 3) - (int) (rdp.tiles[1].uls & 3);
        dq_t = (int) (rdp.texture_tile.ult & 3) - (int) (rdp.tiles[1].ult & 3);
    }
    key[11] = 0x424c4e44 ^ (uint32_t) filt1 ^ ((uint32_t) (dq_s + 4) << 8) ^ ((uint32_t) (dq_t + 4) << 12);

    for (i = 0; i < GFX_BLEND_CACHE; i++) {
        if (gfx_blend_cache[i].valid && memcmp(gfx_blend_cache[i].key, key, sizeof(key)) == 0) {
            gfx_blend_cache[i].last_used_frame = gfx_frame_counter;
            if (gfx_debug_frame) port_log("  blend hit %u %ux%u\n", gfx_blend_cache[i].texture_id, W, H);
            *bw = W;
            *bh = H;
            return gfx_blend_cache[i].texture_id;
        }
    }
    /* the least recently used entry -- of this size if one was not used this frame: the
     * waters re-blend every frame (their LOD fraction moves), and a slot of another size
     * would take new arena space each time instead of being rewritten in place */
    {
        int same_sz = -1;
        for (i = 0; i < GFX_BLEND_CACHE; i++) {
            if (!gfx_blend_cache[i].valid) { pick = i; break; }
            if (pick < 0 || gfx_blend_cache[i].last_used_frame < gfx_blend_cache[pick].last_used_frame) pick = i;
            if (gfx_blend_cache[i].key[4] == key[4] &&
                (same_sz < 0 || gfx_blend_cache[i].last_used_frame < gfx_blend_cache[same_sz].last_used_frame)) same_sz = i;
        }
        if (i == GFX_BLEND_CACHE && same_sz >= 0 && gfx_blend_cache[same_sz].last_used_frame != gfx_frame_counter) pick = same_sz;
    }
    gfx_flush(); // queued triangles may still sample the entry being rewritten
    if (gfx_blend_cache[pick].texture_id == 0) {
        gfx_blend_cache[pick].texture_id = gfx_rapi->new_texture();
    }
    if (!gfx_soft_blend_textures(gfx_blend_cache[pick].texture_id, n0->texture_id, n1->texture_id, (int) W, (int) H,
                                 rdp.texture_tile.uls >> 2, rdp.texture_tile.ult >> 2, rdp.tiles[1].uls >> 2,
                                 rdp.tiles[1].ult >> 2, gfx_tile1_shift(false), gfx_tile1_shift(true), rgb_mode, a_mode,
                                 krgb_src, krgb, ka_src, ka, filt1, dq_s, dq_t, (int) same)) {
        gfx_blend_cache[pick].valid = false;
        return 0;
    }
    memcpy(gfx_blend_cache[pick].key, key, sizeof(key));
    gfx_blend_cache[pick].valid = true;
    gfx_blend_cache[pick].last_used_frame = gfx_frame_counter;
    gfx_blend_census[0]++;
    if (same) gfx_blend_census[1]++;
    if (dq_s != 0 || dq_t != 0) gfx_blend_census[2]++;
    if (gfx_debug_frame) port_log("  blend tex %u %ux%u modes %d/%d k %d:%02X%02X%02X %d:%02X same %d dq %d/%d\n", gfx_blend_cache[pick].texture_id, W, H, rgb_mode, a_mode, krgb_src, krgb[0], krgb[1], krgb[2], ka_src, ka, (int) same, dq_s, dq_t);
    *bw = W;
    *bh = H;
    return gfx_blend_cache[pick].texture_id;
}

/* (A - K) * TEXEL0 + K, K = PRIM or ENV: OoT's everyday tint (HUD icons, text,
 * file select, pause cursor: G_CC_PRIMITIVE-ENV style). Evaluated with the
 * texel as 1 the vertex colour is A and a modulate loses K; the backend lerps
 * K -> vertex colour by the texel instead. Not with cycle 2 folded in. */
static void gfx_update_tex_lerp(uint32_t cc_id, bool cc2) {
    extern void gfx_scegu_set_tex_lerp(bool on, uint8_t r, uint8_t g, uint8_t b);
    uint32_t bits = cc_id & 0xfff;
    uint8_t a = bits & 7, b = (bits >> 3) & 7, c = (bits >> 6) & 7, d = (bits >> 9) & 7;
    bool on = false;
    struct RGBA k = { 0, 0, 0, 0 };
    if (!cc2 && c == CC_TEXEL0 && b == d && (b == CC_PRIM || b == CC_ENV) && (a != b || (cc_id & CC_FLAG_RGB_A_ONE)) &&
        !(cc_id & CC_FLAG_RGB_D_ONE)) {
        k = b == CC_PRIM ? rdp.prim_color : rdp.env_color;
        on = (k.r | k.g | k.b) != 0;
    }
    if (on != rendering_state.tex_lerp ||
        (on && (k.r != rendering_state.lerp_k.r || k.g != rendering_state.lerp_k.g || k.b != rendering_state.lerp_k.b))) {
        gfx_flush();
        gfx_scegu_set_tex_lerp(on, k.r, k.g, k.b);
        rendering_state.tex_lerp = on;
        rendering_state.lerp_k = k;
    }
}

static inline float dot(const float a[3], const float b[3])
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

/* The RSP normalizes a light or lookat direction at any length. The directions are taken into model
 * space through the modelview, so for a model drawn at a small scale they come out ~0.003 long; the
 * old 1e-5 floor on the squared length then skipped the normalize and n.l came out ~0: no directional
 * light and no texgen shine, one flat colour (OoT 6f1ac0e, the lead's X31). BK_NORMALIZE=0: the old floor. */
static int gfx_norm_any SAVESTATE_EXCLUDE = -1;
static void gfx_normalize_vector(float v[3]) {
    float dot = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (gfx_norm_any < 0) { const char *e = getenv("BK_NORMALIZE"); gfx_norm_any = !(e && e[0] == '0'); }
    if (dot > (gfx_norm_any ? 1e-30f : 0.00001f)) {
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
    if (gfx_vh_enabled()) {
        gfx_vh[1] = gfx_vh_fnv(gfx_vh_fnv(gfx_vh[1], &parameters, 1), addr, 64);
        if ((int) gfx_vh_frame == gfx_vh_dumpframe()) {
            fprintf(gfx_vh_file, "M %02X %p:", parameters, (const void *) addr);
            for (int i = 0; i < 16; i++) fprintf(gfx_vh_file, " %08X", (unsigned) addr[i]);
            fprintf(gfx_vh_file, "\n");
        }
    }
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
    // Under BK_TL the GE applies MP as GU_PROJECTION on object-space vertices;
    // it is (re)set per batch in gfx_flush from the current rsp.MP_matrix.
}

static void gfx_sp_pop_matrix(uint32_t count) {
    if (gfx_debug_frame) port_log("  popmtx %u stack %d\n", (unsigned) count, (int) rsp.modelview_matrix_stack_size);
#ifdef PORT_GE_TL
    gfx_flush(); // draw pending verts with the current modelview before popping
#endif
    /* The RSP never pops its last matrix (F3DEX2: a pop at the bottom of the stack does nothing). Popping to 0
     * made the next G_MTX load write modelview_matrix_stack[-1], the 64 bytes before it in rsp, and a pop at 0
     * left MP_matrix stale (Majora's Mask 1.0.0: the renderer's texture pointers became matrix floats and the
     * next textured triangle crashed; majoras-mask 1.0.1). */
    while (count--) {
        if (rsp.modelview_matrix_stack_size > 1) {
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
    /* A load that runs past the vertex buffer keeps the vertices that fit (F3DEX2's v0 - n can also wrap below 0):
     * the RSP would write past its buffer into DMEM, this wrote past loaded_vertices over the rest of rsp. */
    if (dest_index >= MAX_VERTICES) return;
    if (n_vertices > MAX_VERTICES - dest_index) n_vertices = MAX_VERTICES - dest_index;
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
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(PORT_GE_CULL)
    /* the batch's two matrices, row by row in NEON registers (see the transform below) */
    const float (*const _MPn)[4] = rsp.MP_matrix;
    const float (*const _MVn)[4] = rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1];
    const float32x4_t mp0 = vld1q_f32(_MPn[0]), mp1 = vld1q_f32(_MPn[1]), mp2 = vld1q_f32(_MPn[2]), mp3 = vld1q_f32(_MPn[3]);
    const float32x4_t mv0 = vld1q_f32(_MVn[0]), mv1 = vld1q_f32(_MVn[1]), mv2 = vld1q_f32(_MVn[2]), mv3 = vld1q_f32(_MVn[3]);
    static int sVtxNeon = -1; /* BK_VTXNEON=0: the scalar transform and clip flags (the device bench) */
    if (sVtxNeon < 0) { const char *e = getenv("BK_VTXNEON"); sVtxNeon = !(e && e[0] == '0'); }
#endif
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
        /* NEON (round 19, FZX 1b73772 / the OoT port c935b73): ((t0*M0 + t1*M1) + t2*M2) + t3*M3 in
         * lanes, the scalar order -- VMLA is not fused, and the build has -ffp-contract=off */
        if (sVtxNeon) {
            const float t0 = temp_vec[0], t1 = temp_vec[1], t2 = temp_vec[2], t3 = temp_vec[3];
            vst1q_f32(proj_vec, vmlaq_n_f32(vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(mp0, t0), mp1, t1), mp2, t2), mp3, t3));
            vst1q_f32(view_vec, vmlaq_n_f32(vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(mv0, t0), mv1, t1), mv2, t2), mv3, t3));
        } else
#endif
        {
            const float (*_MP)[4] = rsp.MP_matrix;
            const float (*_MV)[4] = rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1];
            for (int _c = 0; _c < 4; _c++) {
                proj_vec[_c] = temp_vec[0]*_MP[0][_c] + temp_vec[1]*_MP[1][_c] + temp_vec[2]*_MP[2][_c] + temp_vec[3]*_MP[3][_c];
                view_vec[_c] = temp_vec[0]*_MV[0][_c] + temp_vec[1]*_MV[1][_c] + temp_vec[2]*_MV[2][_c] + temp_vec[3]*_MV[3][_c];
            }
        }
#endif
        /* Aspect widening is only meaningful for the perspective 3D world
         * (it trades a wider FOV for the square screen). Ortho draws -- the
         * whole MK64 HUD is ortho quads via set_matrix_hud_screen() -- already
         * map their own coordinate space exactly onto the viewport, so
         * widening them just overspills the screen edges: on a square output the item
         * box, position number and corner portraits clipped off. No-op at 4:3
         * (desktop 320x240), so it only changes the narrow CONVERT screen. */
#ifdef PORT_GE_RAWCC
        const float x = proj_vec[0]; /* GE builds its perspectives for the output (hh_screen.c) */
#else
        const float x = (rsp.is_persp && !gfx_ui_mode) ? gfx_adjust_x_for_aspect_ratio(proj_vec[0]) : proj_vec[0];
#endif
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
        if (gfx_debug_frame) port_log("  vtx %p ob %d %d %d tc %d %d sc %d %d\n", (const void *)v, v->ob[0], v->ob[1], v->ob[2], v->tc[0], v->tc[1], rsp.texture_scaling_factor.s, rsp.texture_scaling_factor.t);
        
        if (rsp.geometry_mode & G_LIGHTING) {
            if (rsp.lights_changed) {
                for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                    calculate_normal_dir(&rsp.current_lights[i], rsp.current_lights_coeffs[i]);
                }
                static const Light_t lookat_x = {{0, 0, 0}, 0, {0, 0, 0}, 0, {127, 0, 0}, 0};
                static const Light_t lookat_y = {{0, 0, 0}, 0, {0, 0, 0}, 0, {0, 127, 0}, 0};
                /* Texgen axes: the task's lookat (the DMEM default, or gSPLookAt), s from LOOKATX
                 * and t from LOOKATY -- what the console's own F3D microcode does (checked against
                 * the z64 LLE reference running GE's microcode: the Nintendo logo's chrome ends
                 * light up; the fixed axes left them dark). BK_LOOKAT=0: the SM64 port's fixed
                 * eye-space axes, s from x (transposed against its own guLookAtReflect(up = x));
                 * 2: s from LOOKATY. Texgen lives inside the lighting path, as in F3D: the RSP
                 * generates s/t only with G_LIGHTING on, so the coefficients update with the lights. */
                static int sLookatMode = -1;
                if (sLookatMode < 0) { const char *e = getenv("BK_LOOKAT"); sLookatMode = e ? atoi(e) : 1; }
                const Light_t *ls = &lookat_x, *lt = &lookat_y;
                if (sLookatMode == 1) { ls = &rsp.current_lookat[1]; lt = &rsp.current_lookat[0]; }
                if (sLookatMode == 2) { ls = &rsp.current_lookat[0]; lt = &rsp.current_lookat[1]; }
                if (ls->dir[0] | ls->dir[1] | ls->dir[2]) calculate_normal_dir(ls, rsp.current_lookat_coeffs[0]);
                if (lt->dir[0] | lt->dir[1] | lt->dir[2]) calculate_normal_dir(lt, rsp.current_lookat_coeffs[1]);
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
            if (gfx_debug_frame && i == 0) port_log("  lit vtx0 shade %02X%02X%02X (ambient %02X%02X%02X, lights %d)\n", d->color.r, d->color.g, d->color.b, rsp.current_lights[rsp.current_num_lights - 1].col[0], rsp.current_lights[rsp.current_num_lights - 1].col[1], rsp.current_lights[rsp.current_num_lights - 1].col[2], rsp.current_num_lights);
            
            if (rsp.geometry_mode & G_TEXTURE_GEN) {
                float dotx = 0, doty = 0;
                dotx += vn->n[0] * rsp.current_lookat_coeffs[0][0];
                dotx += vn->n[1] * rsp.current_lookat_coeffs[0][1];
                dotx += vn->n[2] * rsp.current_lookat_coeffs[0][2];
                doty += vn->n[0] * rsp.current_lookat_coeffs[1][0];
                doty += vn->n[1] * rsp.current_lookat_coeffs[1][1];
                doty += vn->n[2] * rsp.current_lookat_coeffs[1][2];
                
                U = (int32_t)((dotx / 127.0f + 1.0f) / 4.0f * rsp.texture_scaling_factor.s);
                V = (int32_t)((doty / 127.0f + 1.0f) / 4.0f * rsp.texture_scaling_factor.t);
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
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            /* The x/y tests in NEON lanes (round 19, FZX da6798e): each VFP compare moved its
             * flags to the core (vmrs), twelve a vertex. x > b is tested as -x < -b (negation is
             * exact), |w| * c is the same product either way round, NaN fails both forms. */
            uint32_t rej;
            if (!sVtxNeon) {
                const float aw = w < 0.0f ? -w : w, gw = GUARD_BAND * aw, rw = gfx_clip_ratio * aw;
                rej = 0;
                if (x < -gw) rej |= X_POS;
                if (x > gw) rej |= X_NEG;
                if (y < -gw) rej |= Y_POS;
                if (y > gw) rej |= Y_NEG;
                if (x < -w) rej |= X_OUT_L;
                if (x > w) rej |= X_OUT_R;
                if (y < -w) rej |= Y_OUT_B;
                if (y > w) rej |= Y_OUT_T;
                if (x < -rw || x > rw || y < -rw || y > rw) rej |= GB_OUT;
            } else {
                const float32x4_t AW = vabsq_f32(vdupq_n_f32(w)), XY = { x, -x, y, -y };
                static const uint32_t kGuard[4] = { X_POS, X_NEG, Y_POS, Y_NEG }, kOut[4] = { X_OUT_L, X_OUT_R, Y_OUT_B, Y_OUT_T };
                uint32x4_t r = vandq_u32(vcltq_f32(XY, vnegq_f32(vmulq_n_f32(AW, GUARD_BAND))), vld1q_u32(kGuard));
                r = vorrq_u32(r, vandq_u32(vcltq_f32(XY, vdupq_n_f32(-w)), vld1q_u32(kOut)));
                r = vorrq_u32(r, vandq_u32(vcltq_f32(XY, vnegq_f32(vmulq_n_f32(AW, gfx_clip_ratio))), vdupq_n_u32(GB_OUT)));
                const uint32x2_t r2 = vorr_u32(vget_low_u32(r), vget_high_u32(r));
                rej = vget_lane_u32(r2, 0) | vget_lane_u32(r2, 1);
            }
#else
            float aw = w < 0.0f ? -w : w;
            float gw = GUARD_BAND * aw;
            uint32_t rej = 0; /* gathered, stored once (FZX 1b73772: each |= was a load/orr/store) */
            if (x < -gw) rej |= X_POS;
            if (x > gw) rej |= X_NEG;
            if (y < -gw) rej |= Y_POS;
            if (y > gw) rej |= Y_NEG;
            if (x < -w) rej |= X_OUT_L;
            if (x > w) rej |= X_OUT_R;
            if (y < -w) rej |= Y_OUT_B;
            if (y > w) rej |= Y_OUT_T;
            {   /* beyond the RSP's guard band (R33) */
                const float rw = gfx_clip_ratio * aw;
                if (x < -rw || x > rw || y < -rw || y > rw) rej |= GB_OUT;
            }
#endif
            // In front of the game's near plane (z_ndc < -1) or behind the eye (MK64
            // 4f64001): was w < W_NEAR, which the trivial reject treated as
            // "outside" and so deleted every triangle within 24 units of the eye.
            if (gfx_nearclip_on()) {
                if (z < -w) rej |= Z_POS;
            } else if (w < W_MIN) {
                rej |= Z_POS; /* behind the eye: clipped, or rejected when all three are */
            } else if (z < -w) {
                rej |= Z_NEAR; /* in front of the near plane: drawn (F3D 2.0G) */
            }
            // Beyond the far plane (the RSP clips there; GE's per-level far plane is a real
            // cull). Was z > 2|w|, which a perspective never reaches (z/w < (f+n)/(f-n)), so
            // the far reject never fired (round-24 bug l; the SM64 port 5cd6699).
            if (z > w) rej |= Z_NEG;
            d->clip_rej = rej;
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
                float wz = (1.0f - BK_DEPTH_EPS) * proj_vec[3];
                // Behind the eye, or nearer than the game's near plane (z_ndc < -1).
                if (proj_vec[3] < BK_TL_NEAR || proj_vec[2] + wz < 0.0f) d->clip_rej = Z_POS;
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
                port_log("  vtx cn %02X %02X %02X %02X flag %04X tc %d,%d\n", v->cn[0], v->cn[1], v->cn[2], v->cn[3],
                         (unsigned) v->flag, v->tc[0], v->tc[1]);
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
    if (gfx_vh_enabled()) {
        gfx_vh[0] = gfx_vh_fnv(gfx_vh[0], vertices, n_vertices * sizeof(Vtx));
        if ((int) gfx_vh_frame == gfx_vh_dumpframe())
            for (size_t k = 0; k < n_vertices; k++) {
                const Vtx_t *t = &vertices[k].v;
                fprintf(gfx_vh_file, "V %u: ob %d %d %d tc %d %d cn %02X%02X%02X%02X\n", (unsigned) (dest_index - n_vertices + k),
                        t->ob[0], t->ob[1], t->ob[2], t->tc[0], t->tc[1], t->cn[0], t->cn[1], t->cn[2], t->cn[3]);
            }
        for (size_t k = dest_index - n_vertices; k < dest_index; k++) {
            const struct LoadedVertex *d = &rsp.loaded_vertices[k];
            gfx_vh[3] = gfx_vh_fnv(gfx_vh[3], &d->_x, 4 * sizeof(float));
            gfx_vh[3] = gfx_vh_fnv(gfx_vh[3], &d->u, 2 * sizeof(float));
            gfx_vh[3] = gfx_vh_fnv(gfx_vh[3], &d->color, sizeof(d->color));
            gfx_vh[3] = gfx_vh_fnv(gfx_vh[3], &d->clip_rej, sizeof(d->clip_rej));
        }
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

#ifdef PORT_GE_RAWCC
/* GoldenEye: the whole RDP combiner, both cycles, per vertex from the raw muxes. The compiled
 * cc_id has 3-bit slots (0, TEXEL0/1, PRIM, SHADE, ENV, TEXEL0_ALPHA, LOD) and read GE's
 * gun/character combiner, lerp(ENV, TEXEL0, SHADE_ALPHA) * SHADE, as plain ENV: black
 * characters and no texture bound. Here every input is evaluated with the texels at a probe
 * value (tc for TEXEL0/1, ta for their alphas) and the pixel is lerp(K, C, texel), K = the
 * combiner at texel 0 and C at texel 255: exact for combiners affine in the texel, which
 * all of GE's are (TRILERP between two mip levels of one image reads as one texel). */
static inline int raw_rgb_in(uint8_t mux, int slot, const struct RGBA *shade, int lod, const uint8_t *comb,
                             int tc, int ta, int ch) {
    const uint8_t *prim = (const uint8_t *) &rdp.prim_color, *env = (const uint8_t *) &rdp.env_color;
    switch (mux) {
        case G_CCMUX_COMBINED: return comb[ch];
        case G_CCMUX_TEXEL0: case G_CCMUX_TEXEL1: return tc;
        case G_CCMUX_PRIMITIVE: return prim[ch];
        case G_CCMUX_SHADE: return ((const uint8_t *) shade)[ch];
        case G_CCMUX_ENVIRONMENT: return env[ch];
        default: break;
    }
    if (slot == 2) { /* C: 5 bits, with the alpha inputs */
        switch (mux) {
            case G_CCMUX_COMBINED_ALPHA: return comb[3];
            case G_CCMUX_TEXEL0_ALPHA: case G_CCMUX_TEXEL1_ALPHA: return ta;
            case G_CCMUX_PRIMITIVE_ALPHA: return prim[3];
            case G_CCMUX_SHADE_ALPHA: return shade->a;
            case G_CCMUX_ENV_ALPHA: return env[3];
            case G_CCMUX_LOD_FRACTION: return lod;
            case G_CCMUX_PRIM_LOD_FRAC: return rdp.prim_lod_frac;
            default: return 0; /* SCALE, K5, 0 */
        }
    }
    if (slot == 0) return mux == G_CCMUX_1 ? 255 : mux == G_CCMUX_NOISE ? 128 : 0;
    if (slot == 3) return mux == G_CCMUX_1 ? 255 : 0;
    return 0; /* B: CENTER, K4, 0 */
}

static inline int raw_a_in(uint8_t mux, int slot, const struct RGBA *shade, int lod, int comb_a, int ta) {
    switch (mux) {
        case G_ACMUX_COMBINED: return slot == 2 ? lod : comb_a; /* C: LOD_FRACTION */
        case G_ACMUX_TEXEL0: case G_ACMUX_TEXEL1: return ta;
        case G_ACMUX_PRIMITIVE: return rdp.prim_color.a;
        case G_ACMUX_SHADE: return shade->a;
        case G_ACMUX_ENVIRONMENT: return rdp.env_color.a;
        case G_ACMUX_1: return slot == 2 ? rdp.prim_lod_frac : 255; /* C: PRIM_LOD_FRAC */
        default: return 0;
    }
}

static void raw_cycle_ref(const uint8_t *m, const struct RGBA *shade, int lod, const uint8_t *comb, int tc, int ta, uint8_t *o) {
    int ch, a, b, c, d, v;
    for (ch = 0; ch < 3; ch++) {
        a = raw_rgb_in(m[0], 0, shade, lod, comb, tc, ta, ch);
        b = raw_rgb_in(m[1], 1, shade, lod, comb, tc, ta, ch);
        c = raw_rgb_in(m[2], 2, shade, lod, comb, tc, ta, ch);
        d = raw_rgb_in(m[3], 3, shade, lod, comb, tc, ta, ch);
        v = ((a - b) * c) / 255 + d;
        o[ch] = (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
    }
    a = raw_a_in(m[4], 0, shade, lod, comb[3], ta);
    b = raw_a_in(m[5], 1, shade, lod, comb[3], ta);
    c = raw_a_in(m[6], 2, shade, lod, comb[3], ta);
    d = raw_a_in(m[7], 3, shade, lod, comb[3], ta);
    v = ((a - b) * c) / 255 + d;
    o[3] = (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
}

/* One RGB input as three bytes: raw_rgb_in's cases once per slot instead of once per channel (round 34:
 * raw_cycle was 2% of the device frame). A per-channel input points at its colour, a single-value one
 * at bc[slot], filled with that value. The same values as raw_rgb_in, so the same results. BK_RAWFAST=0:
 * raw_cycle_ref. */
static inline const uint8_t *raw_rgb_src(uint8_t mux, int slot, const uint8_t *shade, int lod, const uint8_t *comb,
                                         int tc, int ta, uint8_t *bc) {
    int v;
    switch (mux) {
        case G_CCMUX_COMBINED: return comb;
        case G_CCMUX_PRIMITIVE: return (const uint8_t *) &rdp.prim_color;
        case G_CCMUX_SHADE: return shade;
        case G_CCMUX_ENVIRONMENT: return (const uint8_t *) &rdp.env_color;
        case G_CCMUX_TEXEL0: case G_CCMUX_TEXEL1: v = tc; break;
        default:
            if (slot == 2) {
                switch (mux) {
                    case G_CCMUX_COMBINED_ALPHA: v = comb[3]; break;
                    case G_CCMUX_TEXEL0_ALPHA: case G_CCMUX_TEXEL1_ALPHA: v = ta; break;
                    case G_CCMUX_PRIMITIVE_ALPHA: v = rdp.prim_color.a; break;
                    case G_CCMUX_SHADE_ALPHA: v = shade[3]; break;
                    case G_CCMUX_ENV_ALPHA: v = rdp.env_color.a; break;
                    case G_CCMUX_LOD_FRACTION: v = lod; break;
                    case G_CCMUX_PRIM_LOD_FRAC: v = rdp.prim_lod_frac; break;
                    default: v = 0; break;
                }
            } else if (slot == 0) v = mux == G_CCMUX_1 ? 255 : mux == G_CCMUX_NOISE ? 128 : 0;
            else if (slot == 3) v = mux == G_CCMUX_1 ? 255 : 0;
            else v = 0;
            break;
    }
    bc[0] = bc[1] = bc[2] = (uint8_t) v;
    return bc;
}

static int sRawFast SAVESTATE_EXCLUDE = -1;
static void raw_cycle(const uint8_t *m, const struct RGBA *shade, int lod, const uint8_t *comb, int tc, int ta, uint8_t *o) {
    if (sRawFast < 0) { const char *e = getenv("BK_RAWFAST"); sRawFast = e && e[0] >= '0' && e[0] <= '2' ? e[0] - '0' : 2; }
    if (!sRawFast) { raw_cycle_ref(m, shade, lod, comb, tc, ta, o); return; }
    uint8_t bc[4][4];
    const uint8_t *sh = (const uint8_t *) shade;
    const uint8_t *pa = raw_rgb_src(m[0], 0, sh, lod, comb, tc, ta, bc[0]);
    const uint8_t *pb = raw_rgb_src(m[1], 1, sh, lod, comb, tc, ta, bc[1]);
    const uint8_t *pc = raw_rgb_src(m[2], 2, sh, lod, comb, tc, ta, bc[2]);
    const uint8_t *pd = raw_rgb_src(m[3], 3, sh, lod, comb, tc, ta, bc[3]);
    int ch, a, b, c, d, v;
    for (ch = 0; ch < 3; ch++) {
        v = ((pa[ch] - pb[ch]) * pc[ch]) / 255 + pd[ch];
        o[ch] = (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
    }
    a = raw_a_in(m[4], 0, shade, lod, comb[3], ta);
    b = raw_a_in(m[5], 1, shade, lod, comb[3], ta);
    c = raw_a_in(m[6], 2, shade, lod, comb[3], ta);
    d = raw_a_in(m[7], 3, shade, lod, comb[3], ta);
    v = ((a - b) * c) / 255 + d;
    o[3] = (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
}

/* Both cycles (the second only in 2-cycle mode and when it is not a plain pass). */
static void gfx_eval_raw_ref(const struct RGBA *shade, int lod, int tc, int ta, struct RGBA *out) {
    static const uint8_t zero[4];
    uint8_t c1[4];
    raw_cycle(rdp.cc1, shade, lod, zero, tc, ta, c1);
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE && !rdp.cc2_pass) {
        raw_cycle(rdp.cc2, shade, lod, c1, tc, ta, (uint8_t *) out);
    } else {
        memcpy(out, c1, 4);
    }
}

/* The combiner compiled per state (round 37, the other ports' vc_* form): raw_rgb_in / raw_a_in turned
 * into byte offsets into one input array, once per cc1/cc2/prim/env/lod-frac/cycle state; a vertex then
 * runs 8 (a - b) * c / 255 + d lines per cycle straight from the array. The same inputs and the same
 * arithmetic as raw_cycle_ref, so the same bytes. BK_RAWFAST=2 (default) this, 1 the per-slot sources
 * (round 34), 0 raw_cycle_ref; BK_RAWCHK=1: every result also from the reference, mismatches counted. */
enum { RI_SHADE = 0, RI_COMB = 4, RI_TC = 8, RI_TA = 9, RI_LOD = 10, RI_ZERO = 11, RI_ONE = 12, RI_HALF = 13,
       RI_PRIM = 14, RI_ENV = 18, RI_PLF = 22, RI_N = 24 };
static uint8_t raw_rgb_idx(uint8_t mux, int slot, int ch) { /* raw_rgb_in, as an offset */
    switch (mux) {
        case G_CCMUX_COMBINED: return (uint8_t) (RI_COMB + ch);
        case G_CCMUX_TEXEL0: case G_CCMUX_TEXEL1: return RI_TC;
        case G_CCMUX_PRIMITIVE: return (uint8_t) (RI_PRIM + ch);
        case G_CCMUX_SHADE: return (uint8_t) (RI_SHADE + ch);
        case G_CCMUX_ENVIRONMENT: return (uint8_t) (RI_ENV + ch);
        default: break;
    }
    if (slot == 2) {
        switch (mux) {
            case G_CCMUX_COMBINED_ALPHA: return RI_COMB + 3;
            case G_CCMUX_TEXEL0_ALPHA: case G_CCMUX_TEXEL1_ALPHA: return RI_TA;
            case G_CCMUX_PRIMITIVE_ALPHA: return RI_PRIM + 3;
            case G_CCMUX_SHADE_ALPHA: return RI_SHADE + 3;
            case G_CCMUX_ENV_ALPHA: return RI_ENV + 3;
            case G_CCMUX_LOD_FRACTION: return RI_LOD;
            case G_CCMUX_PRIM_LOD_FRAC: return RI_PLF;
            default: return RI_ZERO;
        }
    }
    if (slot == 0) return mux == G_CCMUX_1 ? RI_ONE : mux == G_CCMUX_NOISE ? RI_HALF : RI_ZERO;
    if (slot == 3) return mux == G_CCMUX_1 ? RI_ONE : RI_ZERO;
    return RI_ZERO;
}
static uint8_t raw_a_idx(uint8_t mux, int slot) { /* raw_a_in, as an offset */
    switch (mux) {
        case G_ACMUX_COMBINED: return slot == 2 ? RI_LOD : RI_COMB + 3;
        case G_ACMUX_TEXEL0: case G_ACMUX_TEXEL1: return RI_TA;
        case G_ACMUX_PRIMITIVE: return RI_PRIM + 3;
        case G_ACMUX_SHADE: return RI_SHADE + 3;
        case G_ACMUX_ENVIRONMENT: return RI_ENV + 3;
        case G_ACMUX_1: return slot == 2 ? RI_PLF : RI_ONE;
        default: return RI_ZERO;
    }
}
static struct {
    uint32_t st[7];
    bool valid, two;
    uint8_t in[RI_N];    /* the constant inputs: 0, 255, 128, prim, env, the prim lod fraction */
    uint8_t op[2][4][4]; /* [cycle][r, g, b, a][a, b, c, d] */
} sRawProg SAVESTATE_EXCLUDE;
static int sRawChk SAVESTATE_EXCLUDE = -1;
static unsigned long sRawChkN SAVESTATE_EXCLUDE, sRawChkBad SAVESTATE_EXCLUDE;
static void raw_chk_report(void) {
    fprintf(stderr, "[gfx] BK_RAWCHK: %lu combiner results checked, %lu differ\n", sRawChkN, sRawChkBad);
}
static void raw_prog_build(const uint32_t *st) {
    const uint8_t *m[2] = { rdp.cc1, rdp.cc2 };
    memcpy(sRawProg.st, st, sizeof(sRawProg.st));
    sRawProg.valid = true;
    sRawProg.two = (st[6] >> 8) & 1;
    memset(sRawProg.in, 0, sizeof(sRawProg.in));
    sRawProg.in[RI_ONE] = 255;
    sRawProg.in[RI_HALF] = 128;
    memcpy(&sRawProg.in[RI_PRIM], &rdp.prim_color, 4);
    memcpy(&sRawProg.in[RI_ENV], &rdp.env_color, 4);
    sRawProg.in[RI_PLF] = (uint8_t) rdp.prim_lod_frac;
    for (int cy = 0; cy < 2; cy++) {
        for (int ch = 0; ch < 3; ch++)
            for (int sl = 0; sl < 4; sl++) sRawProg.op[cy][ch][sl] = raw_rgb_idx(m[cy][sl], sl, ch);
        for (int sl = 0; sl < 4; sl++) sRawProg.op[cy][3][sl] = raw_a_idx(m[cy][4 + sl], sl);
    }
}
static inline void raw_prog_cycle(const uint8_t (*op)[4], const uint8_t *in, uint8_t *o) {
    for (int ch = 0; ch < 4; ch++) {
        const uint8_t *q = op[ch];
        const int v = ((in[q[0]] - in[q[1]]) * in[q[2]]) / 255 + in[q[3]];
        o[ch] = (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
    }
}
static void gfx_eval_raw(const struct RGBA *shade, int lod, int tc, int ta, struct RGBA *out) {
    if (sRawFast < 0) { const char *e = getenv("BK_RAWFAST"); sRawFast = e && e[0] >= '0' && e[0] <= '2' ? e[0] - '0' : 2; }
    if (sRawFast < 2) { gfx_eval_raw_ref(shade, lod, tc, ta, out); return; }
    uint32_t st[7];
    memcpy(&st[0], rdp.cc1, 8);
    memcpy(&st[2], rdp.cc2, 8);
    memcpy(&st[4], &rdp.prim_color, 4);
    memcpy(&st[5], &rdp.env_color, 4);
    st[6] = (uint32_t) rdp.prim_lod_frac |
            ((uint32_t) ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE && !rdp.cc2_pass) << 8);
    if (!sRawProg.valid || ((st[0] ^ sRawProg.st[0]) | (st[1] ^ sRawProg.st[1]) | (st[2] ^ sRawProg.st[2]) |
                            (st[3] ^ sRawProg.st[3]) | (st[4] ^ sRawProg.st[4]) | (st[5] ^ sRawProg.st[5]) |
                            (st[6] ^ sRawProg.st[6])))
        raw_prog_build(st);
    uint8_t *in = sRawProg.in; /* the per-call inputs go into the same array: one vertex at a time */
    memcpy(&in[RI_SHADE], shade, 4);
    in[RI_COMB] = in[RI_COMB + 1] = in[RI_COMB + 2] = in[RI_COMB + 3] = 0;
    in[RI_TC] = (uint8_t) tc;
    in[RI_TA] = (uint8_t) ta;
    in[RI_LOD] = (uint8_t) lod;
    if (sRawProg.two) {
        raw_prog_cycle(sRawProg.op[0], in, &in[RI_COMB]);
        raw_prog_cycle(sRawProg.op[1], in, (uint8_t *) out);
    } else {
        raw_prog_cycle(sRawProg.op[0], in, (uint8_t *) out);
    }
    if (sRawChk < 0) { const char *e = getenv("BK_RAWCHK"); sRawChk = e && e[0] == '1'; if (sRawChk) atexit(raw_chk_report); }
    if (sRawChk) {
        struct RGBA r;
        gfx_eval_raw_ref(shade, lod, tc, ta, &r);
        sRawChkN++;
        if (memcmp(&r, out, 4) != 0) sRawChkBad++;
    }
}

/* gfx_eval_raw for an emitted vertex, remembered (round 16, the user: "cradle is a bit slow":
 * raw_cycle was 12% of the Cradle's instructions, ~770 a vertex, and a vertex is emitted once
 * for every triangle it is in). Both results -- C (texel 255) and K (texel 0) -- depend only on
 * the shade, the lod and the combiner state snapshotted here, so an entry is reused while that
 * state is unchanged: exact by construction, whichever code path wrote the state. BK_RAWMEMO=0:
 * off (A/B). */
static uint32_t sRawMemoState[7] SAVESTATE_EXCLUDE; /* cc1, cc2, prim, env, lod frac + 2-cycle: word compares, no libc */
static struct { uint32_t shade, gen; int lod; bool tex; struct RGBA c, k; } sRawMemo[256] SAVESTATE_EXCLUDE;
static uint32_t sRawMemoGen SAVESTATE_EXCLUDE = 1;
static int sRawMemoOn SAVESTATE_EXCLUDE = -1;
static void gfx_eval_raw_vertex(const struct RGBA *shade, int lod, bool tex, struct RGBA *c, struct RGBA *k) {
    uint32_t st[7], sh;
    if (sRawMemoOn < 0) { const char *e = getenv("BK_RAWMEMO"); sRawMemoOn = !(e && e[0] == '0'); }
    if (!sRawMemoOn) {
        gfx_eval_raw(shade, lod, 255, 255, c);
        if (tex) gfx_eval_raw(shade, lod, 0, 0, k); else *k = *c;
        return;
    }
    memcpy(&st[0], rdp.cc1, 8); /* constant sizes: plain loads */
    memcpy(&st[2], rdp.cc2, 8);
    memcpy(&st[4], &rdp.prim_color, 4);
    memcpy(&st[5], &rdp.env_color, 4);
    st[6] = (uint32_t) rdp.prim_lod_frac |
            ((uint32_t) ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE && !rdp.cc2_pass) << 8);
    if ((st[0] ^ sRawMemoState[0]) | (st[1] ^ sRawMemoState[1]) | (st[2] ^ sRawMemoState[2]) | (st[3] ^ sRawMemoState[3]) |
        (st[4] ^ sRawMemoState[4]) | (st[5] ^ sRawMemoState[5]) | (st[6] ^ sRawMemoState[6])) {
        for (int i = 0; i < 7; i++) sRawMemoState[i] = st[i];
        sRawMemoGen++;
    }
    memcpy(&sh, shade, 4);
    {
        const unsigned h = ((sh * 2654435761u) ^ ((uint32_t) lod * 40503u)) >> 24;
        __typeof__(sRawMemo[0]) *m = &sRawMemo[h];
        if (m->gen == sRawMemoGen && m->shade == sh && m->lod == lod && m->tex == tex) {
            *c = m->c;
            *k = m->k;
            return;
        }
        gfx_eval_raw(shade, lod, 255, 255, c);
        if (tex) gfx_eval_raw(shade, lod, 0, 0, k); else *k = *c;
        m->gen = sRawMemoGen; m->shade = sh; m->lod = lod; m->tex = tex; m->c = *c; m->k = *k;
    }
}

/* Which texel components the output reads: probe two shades with each texel input at 0/255. */
static void gfx_raw_classify(bool *rgb_t, bool *rgb_ta, bool *a_t) {
    static const struct RGBA probes[2] = { { 10, 20, 30, 40 }, { 200, 100, 50, 150 } };
    int i;
    *rgb_t = *rgb_ta = *a_t = false;
    for (i = 0; i < 2; i++) {
        struct RGBA base, t1, t2;
        gfx_eval_raw(&probes[i], 128, 0, 0, &base);
        gfx_eval_raw(&probes[i], 128, 255, 0, &t1);
        gfx_eval_raw(&probes[i], 128, 0, 255, &t2);
        if (t1.r != base.r || t1.g != base.g || t1.b != base.b) *rgb_t = true;
        if (t2.r != base.r || t2.g != base.g || t2.b != base.b) *rgb_ta = true;
        if (t2.a != base.a) *a_t = true;
    }
}
#endif

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



uint32_t gfx_prof_rebuilds;

static int gfx_seamfix(void) { /* the scissor and texrect edge rules; BK_SEAMFIX=0 turns them off (A/B) */
    static int s = -1;
    if (s < 0) { const char *e = getenv("BK_SEAMFIX"); s = !(e && e[0] == '0'); }
    return s;
}


/* The RDP compares depth only when the render mode asks for it (Z_CMP) on a primitive the RSP
 * sent with depth (G_ZBUFFER); the kit took G_ZBUFFER alone. GoldenEye draws its first-person
 * gun with the z-buffer off -- G_RM_AA_OPA_SURF under the level's G_ZBUFFER (model.c,
 * zbufferenabled = 0) -- so on the N64 it is always on top, while here it was depth-tested
 * against the world and went behind walls when walking into them (device round 10; more often
 * once the near plane stopped clipping close walls, which now reach depth 0 as on the RDP).
 * Writes stay Z_UPD under a passed test (the rasterizer writes only where it tests; GoldenEye
 * sets no Z_UPD without Z_CMP -- BK_TILECENSUS counts it). BK_ZCMP=0: the old rule (A/B). */
static int gfx_zcmp SAVESTATE_EXCLUDE = -1;
static inline bool gfx_depth_test_on(void) {
    if (gfx_zcmp < 0) { const char *e = getenv("BK_ZCMP"); gfx_zcmp = !(e && e[0] == '0'); }
    if ((rsp.geometry_mode & G_ZBUFFER) != G_ZBUFFER)
        return false;
    if ((rdp.other_mode_l & Z_CMP) != Z_CMP) {
        gfx_zcensus_nocmp++;
        if (rdp.other_mode_l & Z_UPD) gfx_zcensus_updonly++;
        return !gfx_zcmp;
    }
    return true;
}

/* Set by the game each task (hh_gfx.c): this frame is a front-end screen of 2D-like images on
 * 3D models, drawn with the N64's own texture filter (gfx_soft sN64Filter). */
int gfx_front_n64filter;
/* gSPTexture's level (the mip levels the draw may read, minus 1): the RDP's last LOD tile (BK_MIPLOD) */
static uint8_t gfx_tex_level;

static void gfx_tri_rebuild_state(struct LoadedVertex *v1) {
    gfx_prof_rebuilds++;
    {
        extern void gfx_scegu_set_n64_filter(bool on, bool tex_lod);
        extern void gfx_scegu_set_lod_level(int max_level);
        static int sOn = -1, sLod = -1, sLevel = -1;
        const int on = gfx_front_n64filter != 0, lod = (rdp.other_mode_h & G_TL_LOD) != 0;
        const int level = lod ? gfx_tex_level & 7 : 0;
        if (on != sOn || lod != sLod) {
            gfx_flush();
            gfx_scegu_set_n64_filter(on, lod);
            sOn = on; sLod = lod;
        }
        if (level != sLevel) {
            gfx_flush();
            gfx_scegu_set_lod_level(level);
            sLevel = level;
        }
    }
    bool depth_test = gfx_depth_test_on();
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
#ifdef PORT_GE_RAWCC
    /* The shader only picks the per-pixel texel use (texel x vertex, texel alpha as colour,
     * none); the colours come from gfx_eval_raw per vertex (colour mode 3). */
    {
        bool rgb_t, rgb_ta, a_t;
        uint32_t rgb, alpha;
        gfx_raw_classify(&rgb_t, &rgb_ta, &a_t);
        rgb = (rgb_t || rgb_ta) ? ((rgb_ta && !rgb_t ? CC_TEXEL0A : CC_TEXEL0) | (CC_SHADE << 6)) : (CC_SHADE << 9);
        alpha = a_t ? (CC_TEXEL0 | (CC_SHADE << 6)) : (CC_SHADE << 9);
        cc_id = rgb | (alpha << 12);
    }
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
#ifdef PORT_GE_RAWCC
    /* Without FORCE_BL the RDP blends only partly covered (edge) pixels: a fully covered
     * pixel takes the blender's first input as is (angrylion z_compare: blend_en =
     * force_blend || (!overflow && antialias_en && farther)). GE's room geometry uses
     * FOG_SHADE_A + an AA opaque cycle 2 whose B mux is 1-A with no FORCE_BL; blending
     * it by the fog factor made everything near the camera (fog 0) invisible. */
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
    {   /* BK_FILLRULE (gfx_soft.c own_edge): a blended or alpha-tested draw without AA_EN -- the RDP writes such a
         * pixel only when one fixed sample of it is covered, so each pixel along an edge two triangles share is drawn
         * once (a see-through quad's diagonal through pixel centres was blended twice: a one-pixel line across it).
         * With AA_EN both triangles write the pixel they share (partial coverage each): left as it was.
         * BK_FILLRULE=0: every draw takes the inclusive row solve, as before. */
        static int sFillRule = -1;
        extern void gfx_soft_set_own_edges(bool on);
        if (sFillRule < 0) { const char *e = getenv("BK_FILLRULE"); sFillRule = !(e != NULL && e[0] == '0'); }
        const bool own = sFillRule && use_alpha && !(rdp.other_mode_l & AA_EN);
        if (own != rendering_state.own_edges) {
            gfx_flush();
            gfx_soft_set_own_edges(own);
            rendering_state.own_edges = own;
        }
    }
    uint8_t num_inputs;
    bool used_textures[2];
    gfx_rapi->shader_get_info(prg, &num_inputs, used_textures);
#ifdef PORT_GE_RAWCC
    /* With the RDP's LOD on (G_TL_LOD), GoldenEye's TEXEL1 is the next mip level of TEXEL0's
     * image (drawn from level 0 and gfx_soft's own mips). With it off, TEXEL1 is a texture of
     * its own: the explosions multiply their IA8 flame by a 16x14 RGBA16 colour map on tile 1
     * -- without it they drew grey (device round 10). BK_TEXEL1=0: never (A/B). */
    static int sTexel1 = -1;
    if (sTexel1 < 0) { const char *e = getenv("BK_TEXEL1"); sTexel1 = !(e && e[0] == '0'); }
    bool uses_t1 = sTexel1 && !(rdp.other_mode_h & G_TL_LOD) && (used_textures[0] || used_textures[1]) && gfx_cc1_uses_texel1();
#else
    bool uses_t1 = (used_textures[0] || used_textures[1]) && gfx_cc1_uses_texel1();
#endif
    for (int pass = 0; pass < 2; pass++) {
    const uint32_t epoch = gfx_cache_epoch; /* a reset under tile 1's import: tile 0 again (gfx_cache_epoch) */
    gfx_tex_stress_hold = pass;
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
    gfx_tex_stress_hold = 0;
    // The GE has one texture unit: whatever was bound last (possibly tile 1's
    // mip level, or a texture just uploaded) must give way to tile 0's texture.
    uint32_t blend_w = 0, blend_h = 0, blend_id = 0;
    if (uses_t1) {
        blend_id = gfx_blend_tiles(&blend_w, &blend_h);
    }
    if (blend_id != 0) {
        gfx_rapi->select_texture(0, blend_id);
        gfx_rapi->set_sampler_parameters(0, (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT,
                                         rdp.texture_tile.cms, rdp.texture_tile.cmt);
    } else if ((used_textures[0] || used_textures[1]) && rendering_state.textures[0] != NULL) {
        gfx_rapi->select_texture(0, rendering_state.textures[0]->texture_id);
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
        tri_state.cms = (uint8_t)rdp.texture_tile.cms; tri_state.cmt = (uint8_t)rdp.texture_tile.cmt;
        if (blend_id != 0) {
            tex_width = (float) blend_w;
            tex_height = (float) blend_h;
            tri_state.mirror = 0;
        }
        if (tri_state.mirror & 1) tex_width *= 2.0f;
        if (tri_state.mirror & 2) tex_height *= 2.0f;
        float filt = ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) ? 0.5f : 0.0f;
        float inv_w = 1.0f / tex_width, inv_h = 1.0f / tex_height;
        /* the render tile's shift scales S/T before its corner is subtracted (R29) */
        tri_state.u_scale = inv_w / 32.0f * gfx_tile_shift_scale(rdp.tiles[gfx_rtile].shifts);
        tri_state.v_scale = inv_h / 32.0f * gfx_tile_shift_scale(rdp.tiles[gfx_rtile].shiftt);
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
#ifdef PORT_GE_RAWCC
    tri_state.cc2 = false; /* gfx_eval_raw evaluates both cycles */
#endif
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
#ifdef PORT_GE_RAWCC
    tri_state.color_mode = 3;
    tri_state.fog_a = (uint8_t) ((rdp.other_mode_l >> 26) & 3);
#endif
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
        /* z/w runs -1 (near) .. 1 (far), as the RSP's viewport maps it; the z-buffer is 0..1.
         * The kit passed z/w through, so everything nearer than z/w = 0 (about twice the near
         * plane) clamped to depth 0 and lost its order: the gun against walls, decals (round-24
         * bug h; the SM64 port 5cd6699). */
        out->z = cv->_z * inv * 0.5f + 0.5f;
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
#ifdef PORT_GE_RAWCC
        case 3:
            gfx_eval_raw_vertex(&cv->color, lod, tri_state.use_texture, &out->color, &out->k);
            break;
#endif
        default:
            gfx_eval_vertex_color(cc_id, &cv->color, lod, &out->color);
            break;
    }
#if !defined(TARGET_PSP)
    if (tri_state.color_mode != 3) {
        out->k.r = out->k.g = out->k.b = out->k.a = 0;
    }
#endif
#ifdef PORT_GE_RAWCC
    if (tri_state.use_fog && gfx_tilecensus > 0)
        gfx_fogcensus_v[tri_state.color_mode == 3][(rdp.fog_color.r | rdp.fog_color.g | rdp.fog_color.b) != 0]++;
    if (tri_state.use_fog && tri_state.color_mode == 3) {
        /* Blender cycle 1 = FOG * A + IN * (1 - A): lerp both colours to the fog colour, so
         * lerp(K, C, texel) comes out exactly as lerp(pixel, FOG, A). A = shade alpha (the
         * RSP fog factor, FOG_SHADE_A), the fog colour's alpha (FOG_PRIM_A: GE's per-object
         * fog/darkness) or the combined alpha (taken at the vertex). */
        int a = tri_state.fog_a == G_BL_A_SHADE ? cv->color.a : tri_state.fog_a == G_BL_A_FOG ? rdp.fog_color.a :
                tri_state.fog_a == G_BL_A_IN ? out->color.a : 0;
        int ia = 255 - a;
        out->color.r = (uint8_t) ((out->color.r * ia + rdp.fog_color.r * a) / 255);
        out->color.g = (uint8_t) ((out->color.g * ia + rdp.fog_color.g * a) / 255);
        out->color.b = (uint8_t) ((out->color.b * ia + rdp.fog_color.b * a) / 255);
        out->k.r = (uint8_t) ((out->k.r * ia + rdp.fog_color.r * a) / 255);
        out->k.g = (uint8_t) ((out->k.g * ia + rdp.fog_color.g * a) / 255);
        out->k.b = (uint8_t) ((out->k.b * ia + rdp.fog_color.b * a) / 255);
    } else
#endif
    if (tri_state.use_fog) {
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
            // (a doubled mirrored texture has period 1 in normalised units, like a plain repeat)
            /* the wrap of the tile the draw samples, as recorded at its state rebuild: in the RDP's detail
             * mode that is tile 1 (clamped in the Depot), and rdp.texture_tile is tile 0 again by now (round 25,
             * OoT's two-tile rebase check: the drop had used tile 0's repeat on tile 1's clamp) */
            static int sDropTile SAVESTATE_EXCLUDE = -1; /* BK_DROPTILE=0: tile 0's wrap as before (A/B) */
            if (sDropTile < 0) { const char *e = getenv("BK_DROPTILE"); sDropTile = !(e && e[0] == '0'); }
            const unsigned cms = sDropTile ? tri_state.cms : rdp.texture_tile.cms, cmt = sDropTile ? tri_state.cmt : rdp.texture_tile.cmt;
            float offu = (cms & G_TX_CLAMP) ? 0.0f : ((cms & G_TX_MIRROR) && !(tri_state.mirror & 1)) ? 2.0f * port_floorf(minu * 0.5f) : port_floorf(minu);
            float offv = (cmt & G_TX_CLAMP) ? 0.0f : ((cmt & G_TX_MIRROR) && !(tri_state.mirror & 2)) ? 2.0f * port_floorf(minv * 0.5f) : port_floorf(minv);
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
#ifndef BK_GUARD_NDC
#define BK_GUARD_NDC 1.0f
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

    // Near plane: keep the portion with clip.w >= BK_TL_NEAR.
    {
        int nout = 0;
        for (i = 0; i < n; i++) {
            const struct LoadedVertex *cur = &bufA[i];
            const struct LoadedVertex *nxt = &bufA[(i + 1) % n];
            int cin = cur->_w >= BK_TL_NEAR, nin = nxt->_w >= BK_TL_NEAR;
            if (cin && nout < 10) bufB[nout++] = *cur;
            if (cin != nin && nout < 10) {
                float t = (BK_TL_NEAR - cur->_w) / (nxt->_w - cur->_w);
                nclip_lerp(&bufB[nout++], cur, nxt, t);
            }
        }
        n = nout;
    }
    if (n < 3) return; // fully behind the eye

    // The game's real near and far planes (what the RSP clips against), pulled
    // in by BK_DEPTH_EPS: the GE rejects a whole triangle if any vertex lands
    // outside its depth range, so every emitted vertex must satisfy
    //   clip.z >= -(1-eps)*w   and   clip.z <= (1-eps)*w.
    const float D = 1.0f - BK_DEPTH_EPS;
    n = nclip_plane(bufB, n, bufA, 0.0f, 0.0f,  1.0f, D, 10); if (n < 3) return;
    n = nclip_plane(bufA, n, bufB, 0.0f, 0.0f, -1.0f, D, 10); if (n < 3) return;

    // Four guard-band side planes (all verts now have clip.w >= BK_TL_NEAR > 0):
    //   clip.x <=  G*w,  clip.x >= -G*w,  clip.y <=  G*w,  clip.y >= -G*w
    const float G = BK_GUARD_NDC;
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

#ifdef PORT_GE_RAWCC
/* The RDP's detail mode (G_TD_DETAIL with the LOD on; the Depot's ground): tile 0 holds a detail
 * texture and the image is tile 1 up -- TEXEL0 reads the detail tile only while the texture is
 * magnified past level 0, blended in by the LOD fraction. The port draws level 0 of every
 * mipmapped texture, so here tile 1 is the render tile for the draw and the detail grain is
 * left out. Drawn from tile 0, the Depot's gravel came out as its 16x16 detail strip (masks
 * 128x4) through a palette left over from the draw before: a blue and purple hatch (round 12).
 * BK_DETAIL=0: tile 0 as before. */
static uint32_t gfx_detail_census SAVESTATE_EXCLUDE; /* state rebuilds in detail mode (BK_TILECENSUS) */
/* The detail grain (round 32; gfx_soft.c sGrain): with the TRILERP combiner in detail mode, tile 0 is
 * imported into unit 1 and the rasterizer lerps it into the base where the base is magnified.
 * Off by default (the user, 2026-09-30, after the 240 before/after: barely visible, ~3 ms on the Dam);
 * BK_DETAILGRAIN=1: on. */
extern void gfx_soft_set_grain(uint32_t id, float ks, float kt, float os, float ot, float minf);
static int gfx_grain SAVESTATE_EXCLUDE = -1;
static uint32_t gfx_grain_bound SAVESTATE_EXCLUDE;
static void gfx_grain_off(void) {
    if (gfx_grain_bound != 0) {
        gfx_flush();
        gfx_soft_set_grain(0, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f);
        gfx_grain_bound = 0;
    }
}
static void gfx_grain_bind(void) {
    const uint8_t *m = rdp.cc1;
    if (gfx_grain < 0) { const char *e = getenv("BK_DETAILGRAIN"); gfx_grain = (e && e[0] == '1'); }
    /* TRILERP: (TEXEL1 - TEXEL0) * LOD_FRACTION + TEXEL0 */
    if (!gfx_grain || !(m[0] == G_CCMUX_TEXEL1 && m[1] == G_CCMUX_TEXEL0 && m[2] == G_CCMUX_LOD_FRACTION && m[3] == G_CCMUX_TEXEL0)) {
        gfx_grain_off();
        return;
    }
    /* tile 0 (the texture_tile descriptor again) into unit 1: gfx_tex_slot(1) reads tiles[1]'s TMEM */
    __typeof__(rdp.tiles[1]) save1 = rdp.tiles[1];
    const uint8_t save_fmt = rdp.texture_tile.fmt;
    rdp.tiles[1] = rdp.tiles[0];
    /* a colour-indexed grain with the TLUT off (the Depot's CI8 strip) is read as intensity (round 12) */
    if (rdp.texture_tile.fmt == G_IM_FMT_CI && (rdp.other_mode_h & (3U << G_MDSFT_TEXTLUT)) == G_TT_NONE) {
        rdp.texture_tile.fmt = G_IM_FMT_I;
        rdp.tiles[1].fmt = G_IM_FMT_I;
    }
    rendering_state.textures[1] = NULL;
    import_texture(1);
    rdp.tiles[1] = save1;
    rdp.texture_tile.fmt = save_fmt;
    rdp.textures_changed[1] = true;
    struct TextureHashmapNode *n = rendering_state.textures[1];
    if (n == NULL) { gfx_grain_off(); return; }
    const float k0s = (float) (1 << (gfx_shift_exp(rdp.tiles[0].shifts) + 10)) / 1024.0f;
    const float k0t = (float) (1 << (gfx_shift_exp(rdp.tiles[0].shiftt) + 10)) / 1024.0f;
    const float k1s = (float) (1 << (gfx_shift_exp(rdp.tiles[1].shifts) + 10)) / 1024.0f;
    const float k1t = (float) (1 << (gfx_shift_exp(rdp.tiles[1].shiftt) + 10)) / 1024.0f;
    const float ks = k0s / k1s, kt = k0t / k1t;
    /* base texel u (tile 1, + 0.5 for the filter) = S * k1 - uls1; grain = S * k0 - uls0 (+ 0.5) */
    const float filt = ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) ? 0.5f : 0.0f;
    const float os = ((float) rdp.tiles[1].uls / 4.0f - filt) * ks - (float) rdp.tiles[0].uls / 4.0f + filt;
    const float ot = ((float) rdp.tiles[1].ult / 4.0f - filt) * kt - (float) rdp.tiles[0].ult / 4.0f + filt;
    gfx_flush();
    gfx_soft_set_grain(n->texture_id, ks, kt, os, ot, (float) rdp.prim_min_level / 32.0f);
    gfx_grain_bound = n->texture_id;
}
uint32_t gfx_detail_census_get(void) { return gfx_detail_census; }
static void gfx_tri_rebuild_state_detail(struct LoadedVertex *v1) {
    static int sDetail SAVESTATE_EXCLUDE = -1;
    if (sDetail < 0) { const char *e = getenv("BK_DETAIL"); sDetail = !(e && e[0] == '0'); }
    const bool detail = (rdp.other_mode_h & (3U << G_MDSFT_TEXTDETAIL)) == G_TD_DETAIL && (rdp.other_mode_h & G_TL_LOD) != 0;
    if (detail && gfx_tilecensus > 0) gfx_detail_census++;
    if (!detail || !sDetail) {
        gfx_grain_off();
        gfx_tri_rebuild_state(v1);
        return;
    }
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
    rdp.textures_changed[0] = true; /* the bound image follows the swapped render tile ... */
    gfx_tri_rebuild_state(v1);
    rdp.texture_tile = save;
    rdp.textures_changed[0] = true; /* ... and the next draw's is checked again (a cache hit) */
    gfx_grain_bind();
}
#endif

static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
    /* A tri command's index (index * 2 in a byte) reaches 127; the vertex buffer holds MAX_VERTICES. The RSP would
     * read whatever DMEM follows its buffer: here the triangle is dropped rather than read past loaded_vertices. */
    if (vtx1_idx >= MAX_VERTICES || vtx2_idx >= MAX_VERTICES || vtx3_idx >= MAX_VERTICES) return;
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

    if (gfx_trace_frames > 0 && rsp.is_persp) /* BK_TRACE_AT: every perspective triangle, clip space */
        port_log("  tri1 (%.2f %.2f %.2f %.2f) (%.2f %.2f %.2f %.2f) (%.2f %.2f %.2f %.2f) rej %X %X %X geo %08X\n",
                 v1->_x, v1->_y, v1->_z, v1->_w, v2->_x, v2->_y, v2->_z, v2->_w, v3->_x, v3->_y, v3->_z, v3->_w,
                 (unsigned) v1->clip_rej, (unsigned) v2->clip_rej, (unsigned) v3->clip_rej, (unsigned) rsp.geometry_mode);
    if (v1->clip_rej & v2->clip_rej & v3->clip_rej & ~(XY_OUT | Z_NEAR | GB_OUT)) {
        // The whole triangle lies outside the guard band (the exact side
        // planes reject below, after the phase bookkeeping). GB_OUT is one bit
        // for all four sides: three vertices sharing it may be on opposite sides.
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
            // Clip when a vertex is in front of the game's near plane (or behind the eye:
            // both set Z_POS) or beyond its far plane (Z_NEG): the RSP's planes, not MK64's
            // distances (w < 2, or w < 24 and past far). In an FPS the camera is always
            // against a wall (round-24 bug i; the SM64 port 5cd6699). R33: or beyond the
            // guard band (GB_OUT), the planes the clipper then cuts at; a vertex merely in
            // front of the near plane (Z_NEAR) no longer sends it -- the guard band keeps
            // its projection bounded.
            const uint32_t trig = gfx_clipedges_mode() == 2 ? (Z_POS | Z_NEG | GB_OUT) : (Z_POS | Z_NEG | Z_NEAR);
            const uint32_t any = v1->clip_rej | v2->clip_rej | v3->clip_rej;
            if (any & trig) {
                span_clip = true;
            }
            if (gfx_tilecensus > 0) {
                if (any & trig) gfx_clipcensus[0]++;
                if ((any & (Z_POS | Z_NEG | GB_OUT | Z_NEAR)) == GB_OUT) gfx_clipcensus[1]++;
                if ((any & (Z_POS | Z_NEG | GB_OUT | Z_NEAR)) == Z_NEAR) gfx_clipcensus[2]++;
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
    // A vertex whose |clip.x| or |clip.y| exceeds BK_GUARD_NDC*clip.w projects
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
    // A vertex whose |clip.x| or |clip.y| exceeds BK_GUARD_NDC*clip.w projects
    // outside the GE guard band and WRAPS on real hardware -- route the whole
    // triangle through the guard-band clipper.  Perspective only.
    if (!ge_needs_clip && (rsp.is_persp)) {
        const struct LoadedVertex *gv[3] = { v1, v2, v3 };
        int gk;
        for (gk = 0; gk < 3; gk++) {
            float gw = BK_GUARD_NDC * gv[gk]->_w;
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
#ifdef PORT_GE_RAWCC
        gfx_tri_rebuild_state_detail(v1);
#else
        gfx_tri_rebuild_state(v1);
#endif
    }
    if (sFogPx < 0) {
        const char *e = getenv("BK_FOGPX");
        /* round 43: OFF by default (the user, 2026-09-30: seams on the lit snow triangles, and "slower but
         * correct visuals"): the split is an enhancement over the game's own per-vertex fog, not its look */
        sFogPx = !e || !e[0] || e[0] == '0' ? 0 : e[0] == '2' ? 2 : 1;
        if (getenv("BK_FOGPXCOUNT")) atexit(fog_split_report);
    }
    /* 2 (BK_FOGPX=2): only the levels whose fog flickered (the user: "only on levels that have had flicker
     * like surface") -- Surface 1 and 2, the snowfields; 1: every level; 0: none */
    
    const bool fogpx_here = sFogPx == 1 || (sFogPx == 2 && ((-1) == 36 || (-1) == 43));
    if (fogpx_here && rsp.is_persp && (rsp.geometry_mode & G_FOG) && (rdp.other_mode_l >> 30) == G_BL_CLR_FOG &&
        ((rdp.other_mode_l >> 26) & 3) == G_BL_A_SHADE && rsp.fog_mul != 0.0f) {
        /* BK_FOGPXSHADE (default on; 0 = round 43's screen-position colour at every cut): the cut corners
         * follow the shade rule gfx_soft will step this triangle with (raster_tri's pshade: textured, depth
         * tested; the split runs here, after the state rebuild, to know) */
        static int sFogPxShade = -1;
        if (sFogPxShade < 0) { const char *e = getenv("BK_FOGPXSHADE"); sFogPxShade = !(e && e[0] == '0'); }
        extern int gfx_soft_pshade_on(void);
        const bool pcut = sFogPxShade && gfx_soft_pshade_on() && tri_state.use_texture && gfx_depth_test_on();
        const size_t n = fog_split(clipped_vertices, clipped_vertices_num, pcut);
        if (n) { clipped_vertices = sFogPtr; clipped_vertices_num = n; }
    }
#ifdef PORT_PROFILE_DL
    gfx_prof_state += port_time_us() - _pt0;
    _pt0 = port_time_us();
#endif
    uint32_t cc_id = tri_state.cc_id;
    bool use_texture = tri_state.use_texture;
    if (gfx_tilecensus > 0 && use_texture && rsp.is_persp) { /* R29 census: the render tile's shift */
        const uint32_t key = rdp.tiles[0].shifts | (rdp.tiles[0].shiftt << 8);
        gfx_shiftcensus_all++;
        if (key) {
            int i;
            gfx_shiftcensus_tris++;
            for (i = 0; i < gfx_shiftcensus_used && gfx_shiftcensus_pairs[i][0] != key; i++) {}
            if (i == gfx_shiftcensus_used && i < 16) { gfx_shiftcensus_used++; gfx_shiftcensus_pairs[i][0] = key; }
            if (i < 16) gfx_shiftcensus_pairs[i][1]++;
        }
    }
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

#ifdef PORT_GE_RAWCC
/* GE's sky (sky.c skyRenderTri) builds RDP triangle commands on the CPU and streams them
 * through the RSP 64 bits at a time: G_RDPHALF_1 carries the high word, G_RDPHALF_CONT
 * (G_RDPHALF_2 for the last one) the low word. A complete G_TRI_* command is turned back
 * into screen-space vertices -- the edge walker's corner points, with the shade and texture
 * planes evaluated there -- and drawn through gfx_sp_tri1 like RSP triangles. */
static uint32_t gfx_rdp_raw_hi;
static uint32_t gfx_rdp_raw[44]; /* up to 22 64-bit words: edges 4, shade 8, texture 8, z 2 */
static int gfx_rdp_raw_n;

static int gfx_rdp_raw_words(uint32_t w0) {
    uint32_t c = (w0 >> 24) & 0x3F;
    if (c < 0x08 || c > 0x0F) return 0;
    return 2 * (4 + ((c & 4) ? 8 : 0) + ((c & 2) ? 8 : 0) + ((c & 1) ? 2 : 0));
}

/* 16-bit field i (0..3, high first) of 64-bit word k of a coefficient block */
static inline uint32_t gfx_rdp_field(const uint32_t *blk, int k, int i) {
    uint32_t w = blk[2 * k + (i >> 1)];
    return (i & 1) ? (w & 0xFFFF) : (w >> 16);
}

/* s15.16 coefficient i of a block, its integer half in word k_int, fraction in k_frac */
static inline float gfx_rdp_coef(const uint32_t *blk, int k_int, int k_frac, int i) {
    return (float) (int32_t) ((gfx_rdp_field(blk, k_int, i) << 16) | gfx_rdp_field(blk, k_frac, i)) / 65536.0f;
}

/* Attribute i of a block at screen point (x, y): the value is given where the major edge
 * crosses the triangle's first scanline y0, stepped by d/de down that edge and by d/dx
 * across the span. */
static float gfx_rdp_attr(const uint32_t *blk, int i, float x, float y, float y0, float xh, float dxhdy) {
    float v = gfx_rdp_coef(blk, 0, 2, i), ddx = gfx_rdp_coef(blk, 1, 3, i), dde = gfx_rdp_coef(blk, 4, 6, i);
    return v + dde * (y - y0) + ddx * (x - (xh + dxhdy * (y - y0)));
}

/* One corner of a decoded RDP primitive as an RSP vertex in clip space. */
static void gfx_rdp_raw_vertex(struct LoadedVertex *d, const uint32_t *w, float x, float y, float y0, float xh, float dxhdy) {
    const uint32_t c = (w[0] >> 24) & 0x3F;
    const bool shade = (c & 4) != 0, tex = (c & 2) != 0;
    const bool persp = (rdp.other_mode_h & (1U << G_MDSFT_TEXTPERSP)) != 0;
    const uint32_t *sblk = w + 8, *tblk = w + 8 + (shade ? 16 : 0);
    float cw = 100.0f; /* clip w: far beyond the near-clip thresholds */
    memset(d, 0, sizeof(*d));
    if (shade) {
        float ch[4];
        int i;
        for (i = 0; i < 4; i++) {
            ch[i] = gfx_rdp_attr(sblk, i, x, y, y0, xh, dxhdy);
            if (ch[i] < 0.0f) ch[i] = 0.0f;
            if (ch[i] > 255.0f) ch[i] = 255.0f;
        }
        d->color.r = (uint8_t) ch[0];
        d->color.g = (uint8_t) ch[1];
        d->color.b = (uint8_t) ch[2];
        d->color.a = (uint8_t) ch[3];
    }
    if (tex) {
        float s = gfx_rdp_attr(tblk, 0, x, y, y0, xh, dxhdy);
        float t = gfx_rdp_attr(tblk, 1, x, y, y0, xh, dxhdy);
        if (persp) {
            /* the RDP divides by W (0x7FFF = 1.0) per pixel: W is 1/w */
            float wv = gfx_rdp_attr(tblk, 2, x, y, y0, xh, dxhdy);
            if (wv < 1.0f) wv = 1.0f;
            s = s / wv * 32768.0f;
            t = t / wv * 32768.0f;
            cw = 100.0f * 32768.0f / wv;
        }
        d->u = s; /* s10.5 texels, like an RSP vertex's */
        d->v = t;
    }
    {
        float ndc_x = (x * RATIO_X - rdp.viewport.x) / rdp.viewport.width * 2.0f - 1.0f;
        float ndc_y = 1.0f - (y * RATIO_Y - rdp.viewport.y) / rdp.viewport.height * 2.0f;
        d->_x = ndc_x * cw;
        d->_y = ndc_y * cw;
        d->_z = 0.999f * cw;
        d->_w = cw;
    }
    if (gfx_debug_frame)
        port_log("  rdp tri %02X (%.2f,%.2f) rgba %d %d %d %d uv (%.1f,%.1f) w %.1f\n", (unsigned) c, x, y,
                 d->color.r, d->color.g, d->color.b, d->color.a, d->u / 32.0f, d->v / 32.0f, d->_w);
}

/* The RDP "triangle" walks a major edge H (YH..YL) against a minor edge M (YH..YM) then
 * L (YM..YL); H and M start apart when the top is flat (skyRenderFull draws the whole
 * sky that way), and L need not meet H at the bottom. So it is drawn as two trapezoids,
 * YH..YM and YM..YL, two triangles each; one set of attribute planes covers all. */
static void gfx_rdp_raw_triangle(const uint32_t *w) {
    if (gfx_vh_enabled()) gfx_vh[2] = gfx_vh_fnv(gfx_vh[2], w, gfx_rdp_raw_words(w[0]) * 4);
    const float yl = (float) ((int32_t) (w[0] << 18) >> 18) / 4.0f;
    const float ym = (float) ((int32_t) (w[1] << 2) >> 18) / 4.0f;
    const float yh = (float) ((int32_t) (w[1] << 18) >> 18) / 4.0f;
    const float xl = (float) (int32_t) w[2] / 65536.0f, dxldy = (float) (int32_t) w[3] / 65536.0f;
    const float xh = (float) (int32_t) w[4] / 65536.0f, dxhdy = (float) (int32_t) w[5] / 65536.0f;
    const float xm = (float) (int32_t) w[6] / 65536.0f, dxmdy = (float) (int32_t) w[7] / 65536.0f;
    const float y0 = floorf(yh); /* XH, XM and the attributes are given at the first whole scanline */
    /* corners: 0 H(yh) 1 M(yh) 2 M(ym) 3 H(ym) 4 L(ym) 5 L(yl) 6 H(yl) */
    const float cx[7] = { xh + dxhdy * (yh - y0), xm + dxmdy * (yh - y0), xm + dxmdy * (ym - y0), xh + dxhdy * (ym - y0),
                          xl, xl + dxldy * (yl - ym), xh + dxhdy * (yl - y0) };
    const float cy[7] = { yh, yh, ym, ym, ym, yl, yl };
    static const uint8_t tris[4][3] = { { 0, 1, 2 }, { 0, 2, 3 }, { 3, 4, 5 }, { 3, 5, 6 } };
    enum { BASE = MAX_VERTICES - 7 };
    struct LoadedVertex save[7];
    struct LoadedVertex *d = &rsp.loaded_vertices[BASE];
    uint32_t gm = rsp.geometry_mode;
    bool is_persp = rsp.is_persp;
    int k;

    memcpy(save, d, sizeof(save));
    for (k = 0; k < 7; k++)
        gfx_rdp_raw_vertex(&d[k], w, cx[k], cy[k], y0, xh, dxhdy);
    rsp.geometry_mode &= ~G_CULL_BOTH; /* the RDP draws either winding */
    rsp.is_persp = true;
    for (k = 0; k < 4; k++) {
        const float *a = &cx[tris[k][0]], *b = &cx[tris[k][1]], *e = &cx[tris[k][2]];
        const float area = (*b - *a) * (cy[tris[k][2]] - cy[tris[k][0]]) - (*e - *a) * (cy[tris[k][1]] - cy[tris[k][0]]);
        if (area > 0.01f || area < -0.01f)
            gfx_sp_tri1(BASE + tris[k][0], BASE + tris[k][1], BASE + tris[k][2]);
    }
    rsp.geometry_mode = gm;
    rsp.is_persp = is_persp;
    memcpy(d, save, sizeof(save));
}

static void gfx_rdp_raw_push(uint32_t lo) {
    if (gfx_rdp_raw_n == 0 && gfx_rdp_raw_words(gfx_rdp_raw_hi) == 0)
        return; /* not the start of a triangle */
    gfx_rdp_raw[gfx_rdp_raw_n++] = gfx_rdp_raw_hi;
    gfx_rdp_raw[gfx_rdp_raw_n++] = lo;
    if (gfx_rdp_raw_n >= gfx_rdp_raw_words(gfx_rdp_raw[0])) {
        gfx_rdp_raw_triangle(gfx_rdp_raw);
        gfx_rdp_raw_n = 0;
    }
}
#endif

/* This will be going away possibly, it all depends on how we end up treating hw sprites */
static void gfx_sp_tri1_2d(uint8_t vtx1_idx, uint8_t vtx2_idx, UNUSED uint8_t vtx3_idx) {
    struct VertexColor *v1 = &rsp.loaded_vertices_2D[vtx1_idx];
    struct VertexColor *v2 = &rsp.loaded_vertices_2D[vtx2_idx];
    struct VertexColor *v_arr[2] = {v1, v2};

    bool depth_test = gfx_depth_test_on();
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
#ifdef PORT_GE_RAWCC
    /* Without FORCE_BL the RDP blends only partly covered (edge) pixels: a fully covered
     * pixel takes the blender's first input as is (angrylion z_compare: blend_en =
     * force_blend || (!overflow && antialias_en && farther)). GE's room geometry uses
     * FOG_SHADE_A + an AA opaque cycle 2 whose B mux is 1-A with no FORCE_BL; blending
     * it by the fog factor made everything near the camera (fog 0) invisible. */
    if (!(rdp.other_mode_l & FORCE_BL)) use_alpha = false;
#endif
    if (rendering_state.force_in) { // rectangles take their colour from the 1-cycle model
        extern void gfx_scegu_set_force_in(bool on);
        gfx_flush();
        gfx_scegu_set_force_in(false);
        rendering_state.force_in = false;
    }
    if (rendering_state.tex_add) { // the 3D path's MK64 kart-tint ADD must not leak into rectangles
        extern void gfx_scegu_set_texfunc_add(bool on);
        gfx_flush();
        gfx_scegu_set_texfunc_add(false);
        rendering_state.tex_add = false;
    }
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    /* COPY mode has no blender: an alpha-compare threshold keeps or drops each
     * texel whole, so texels with a clear alpha bit are skipped, not painted
     * black (the F-Zero X port's fix, same code base). */
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
    
#ifdef PORT_GE_RAWCC
    /* BK_2DCOMB (2026-10-03, the reference sweep: SELECT FILE's gun-barrel swirl missing). The compiled cc_id has
     * no slot for a constant alpha in the RGB C input (PRIM_ALPHA, SHADE_ALPHA, ENV_ALPHA, PRIM_LOD_FRAC), so
     * color_comb_component reads it as 0: the folder screen's background, 300 one-line texture rectangles under
     * (TEXEL0 - PRIM) * ENV_ALPHA + PRIM with env alpha 20 (front.c constructor_menu05_fileselect), drew as flat PRIM
     * -- the grey gradient without the swirl. With such a C (1-cycle, so the result is affine in the texel) the
     * rectangle's colour comes from the raw combiner as the triangles' does (gfx_eval_raw): C at texel 255 as the
     * vertex colour, K at texel 0 as the tex lerp's K (out = K + (C - K) * texel), the alpha as before.
     * BK_2DCOMB=0: the compiled form (flat PRIM). */
    static int s2dComb = -1;
    if (s2dComb < 0) { const char *e = getenv("BK_2DCOMB"); s2dComb = !(e && e[0] == '0'); }
    const uint8_t c2d = rdp.cc1[2];
    const bool raw2d = s2dComb && !two_cycle && (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_1CYCLE &&
                       (c2d == G_CCMUX_PRIMITIVE_ALPHA || c2d == G_CCMUX_SHADE_ALPHA || c2d == G_CCMUX_ENV_ALPHA ||
                        c2d == G_CCMUX_PRIM_LOD_FRAC) &&
                       (rdp.cc1[0] == G_CCMUX_TEXEL0 || rdp.cc1[1] == G_CCMUX_TEXEL0 || rdp.cc1[3] == G_CCMUX_TEXEL0);
    struct RGBA raw2d_k = { 0, 0, 0, 0 };
    if (raw2d) {
        gfx_eval_raw(&v_arr[0]->color, 0, 0, 0, &raw2d_k);
        cc_id = (cc_id & ~0xfffu) | CC_TEXEL0 | (CC_SHADE << 6); /* texel x vertex colour, lerped from K (below) */
    }
#endif
    if (gfx_trace_frames > 0) port_log("  2d: cc %08X\n", (unsigned) cc_id);
    gfx_update_tex_lerp(cc_id, false);
#ifdef PORT_GE_RAWCC
    if (raw2d) {
        extern void gfx_scegu_set_tex_lerp(bool on, uint8_t r, uint8_t g, uint8_t b);
        gfx_flush();
        gfx_scegu_set_tex_lerp(true, raw2d_k.r, raw2d_k.g, raw2d_k.b);
        rendering_state.tex_lerp = true;
        rendering_state.lerp_k = raw2d_k;
    }
#endif
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
    if (epoch == gfx_cache_epoch || !gfx_epochfix_on()) break;
    }
    gfx_tex_stress_hold = 0;
    // The GE has one texture unit: whatever was bound last (possibly tile 1's
    // mip level, or a texture just uploaded) must give way to tile 0's texture.
    if (used_textures[0] && rendering_state.textures[0] != NULL) {
        gfx_rapi->select_texture(0, rendering_state.textures[0]->texture_id);
    }
    
    bool use_texture = used_textures[0] || used_textures[1];
    //uint32_t tex_width = (rdp.texture_tile.lrs - rdp.texture_tile.uls + 4) / 4;
    //uint32_t tex_height = (rdp.texture_tile.lrt - rdp.texture_tile.ult + 4) / 4;

    VertexColor tri_buf[2] = {{0}};
    int tri_num_vert = 0;
    /* A wrapping tile (no clamp bit, mask m) reads coordinate & (2^m - 1), so a texrect may
     * start whole periods before its tile's origin, while the samplers wrap by the imported
     * size, which is smaller on edge pieces. Shift both corners by the same whole number of
     * periods (two when mirrored) so the upper-left one lands in [0, period) (pm64 525cb47). */
    int pm_su = 0, pm_sv = 0;
    
    for (int i = 0; i < 2; i++) {
        tri_buf[tri_num_vert].x = v_arr[i]->x;
        tri_buf[tri_num_vert].y = v_arr[i]->y;
        tri_buf[tri_num_vert].z = 0;
        
        if (use_texture) {
            short u = (v_arr[i]->u - rdp.texture_tile.uls * 8)/ 32;
            short v = (v_arr[i]->v - rdp.texture_tile.ult * 8) / 32;
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
#ifdef PORT_GE_RAWCC
        if (raw2d) { /* the raw combiner's RGB at texel 255 (BK_2DCOMB) */
            struct RGBA c;
            gfx_eval_raw(&v_arr[i]->color, 0, 255, 255, &c);
            tri_buf[tri_num_vert].color.r = c.r; tri_buf[tri_num_vert].color.g = c.g; tri_buf[tri_num_vert].color.b = c.b;
        }
#endif
        tri_num_vert++;
    }
    if (gfx_trace_frames > 0) port_log("  2d: draw (%d,%d)-(%d,%d)\n", tri_buf[0].x, tri_buf[0].y, tri_buf[1].x, tri_buf[1].y);
    {   extern bool gfx_soft_2d_bilin; /* this sprite's filter: a magnified one is bilinear (gfx_soft.c) */
        gfx_soft_2d_bilin = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
    }
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
#ifndef F3DEX_GBI_2
        case G_MV_LOOKATY:
        case G_MV_LOOKATX:
            memcpy(rsp.current_lookat + (index - G_MV_LOOKATY) / 2, data, sizeof(Light_t));
            rsp.lights_changed = 1;
            break;
#endif
#ifdef F3DEX_GBI_2
        case G_MV_LIGHT: {
            int lightidx = offset / 24 - 2;
            if (lightidx >= 0 && lightidx <= MAX_LIGHTS) { // skip lookat
                // NOTE: reads out of bounds if it is an ambient light
                memcpy(rsp.current_lights + lightidx, data, sizeof(Light_t));
            }
            break;
        }
#else
        case G_MV_L0:
        case G_MV_L1:
        case G_MV_L2:
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(rsp.current_lights + (index - G_MV_L0) / 2, data, sizeof(Light_t));
            if (gfx_debug_frame) {
                const Light_t *l = &rsp.current_lights[(index - G_MV_L0) / 2];
                port_log("  light %d col %02X%02X%02X dir %d,%d,%d (num %d)\n", (index - G_MV_L0) / 2, l->col[0], l->col[1], l->col[2], l->dir[0], l->dir[1], l->dir[2], rsp.current_num_lights);
            }
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
            rsp.current_num_lights = (data - 0x80000000U) / 32; /* clamped as the GBI2 count: past MAX_LIGHTS read past current_lights */
            if (rsp.current_num_lights > MAX_LIGHTS + 1) rsp.current_num_lights = MAX_LIGHTS + 1;
            if (rsp.current_num_lights < 1) rsp.current_num_lights = 1;
#endif
            rsp.lights_changed = 1;
            break;
        case G_MW_FOG:
            rsp.fog_mul = (int16_t)(data >> 16);
            rsp.fog_offset = (int16_t)data;
            if (gfx_debug_frame) port_log("  fog mul %d offset %d\n", rsp.fog_mul, rsp.fog_offset);
            break;
        case G_MW_SEGMENT:
            rsp_segments[(offset >> 2) & 0xF] = data;
            break;
        case G_MW_CLIP: /* gSPClipRatio: RNX holds the ratio (FR_NEG_FRUSTRATIO_n = n); R33 */
            if (offset == G_MWO_CLIP_RNX) {
                const int r = (int16_t) data;
                gfx_clip_ratio = (float) (r < 0 ? -r : r < 1 ? 1 : r);
            }
            break;
    }
}

static void gfx_select_rtile(uint8_t tile) {
    tile &= 7;
    if (tile != gfx_rtile) {
        gfx_rtile = tile;
        rdp.texture_tile.fmt = rdp.tiles[tile].fmt;
        rdp.texture_tile.siz = rdp.tiles[tile].siz;
        rdp.texture_tile.cms_raw = rdp.tiles[tile].cms;
        rdp.texture_tile.cmt_raw = rdp.tiles[tile].cmt;
        rdp.texture_tile.masks = rdp.tiles[tile].masks;
        rdp.texture_tile.maskt = rdp.tiles[tile].maskt;
        rdp.texture_tile.uls = rdp.tiles[tile].uls;
        rdp.texture_tile.ult = rdp.tiles[tile].ult;
        rdp.texture_tile.lrs = rdp.tiles[tile].lrs;
        rdp.texture_tile.lrt = rdp.tiles[tile].lrt;
        rdp.texture_tile.tmem = rdp.tiles[tile].tmem;
        rdp.texture_tile.line_size_bytes = rdp.tiles[tile].line_size_bytes;
        rdp.tile_pal = rdp.tiles[tile].pal;
        gfx_render_tile_cm();
        gfx_palette_update(rdp.texture_tile.siz);
        rdp.textures_changed[0] = rdp.textures_changed[1] = true;
        tri_state.valid = false;
    }
}

static void gfx_sp_texture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    if (level != gfx_tex_level) {
        gfx_tex_level = level;
        tri_state.valid = false; /* the next triangle passes it on (gfx_tri_rebuild_state) */
    }
    _UNUSED(on);
    /* The tile the RSP's G_TEXTURE names is the render tile (BK's map models load mipmapped textures through
     * tile 7 and draw through tile 2, set up once per frame): its descriptor becomes the render tile's. */
    gfx_select_rtile(tile);

    rsp.texture_scaling_factor.s = sc;
    rsp.texture_scaling_factor.t = tc;
}


static void gfx_dp_set_scissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    _UNUSED(mode);

    // The lower-right edge is exclusive: a scissor to (319, 239) loses the last column and
    // row, a seam a CRT's overscan hid (DKR clips to 0,0,319,239 every frame). Only the
    // one-short band: a scissor larger than the screen stays as it is. (BK_SEAMFIX=0: off, A/B)
    if (gfx_seamfix()) {
        if (lrx >= (SCREEN_WIDTH - 1) << 2 && lrx < SCREEN_WIDTH << 2) lrx = SCREEN_WIDTH << 2;
        if (lry >= (SCREEN_HEIGHT - 1) << 2 && lry < SCREEN_HEIGHT << 2) lry = SCREEN_HEIGHT << 2;
    }

    float x = ulx / 4.0f * RATIO_X;
#if defined(TARGET_HANDHELD)
    float y = uly / 4.0f * RATIO_Y;                 /* top-down rasterizer: see viewport note */
#else
    float y = (SCREEN_HEIGHT - lry / 4.0f) * RATIO_Y;
#endif
    /* Signed: an inverted scissor is empty, not (unsigned) ~1e9 rows (fzerox cf242ea, R28). */
    float width = (float) ((int32_t) lrx - (int32_t) ulx) / 4.0f * RATIO_X;
    float height = (float) ((int32_t) lry - (int32_t) uly) / 4.0f * RATIO_Y;
    if (width < 0.0f) width = 0.0f;
    if (height < 0.0f) height = 0.0f;
    /* BK_SCISSOR (R23 final, the lead's round 2):
     * 0: x and width truncated apart into the int16 fields, as before round 12;
     * 1: whole target pixels with the right/bottom edges rounded outward (c68da63 R23) --
     *    wrong for edges inside the screen (F-Zero X's 4P divider, a title banner);
     * 2 (default): an edge within one N64 pixel of a screen edge goes to it, on all four
     *    sides; the others by pixel centres, first ceil(x0 - 0.5), end ceil(x1 - 0.5), from
     *    the edges (0 seams on the Dam and the menus at 100% and 85%, round 9). */
    {
        static int sScMode SAVESTATE_EXCLUDE = -1;
        if (sScMode < 0) { const char *e = getenv("BK_SCISSOR"); sScMode = e ? atoi(e) : 2; }
        if (sScMode == 1 && gfx_seamfix()) {
            const float x1 = x + width, y1 = y + height;
            x = floorf(x);
            y = floorf(y);
            width = ceilf(x1 - 0.001f) - x;
            height = ceilf(y1 - 0.001f) - y;
        } else if (sScMode == 2) {
            const float tw = (float) gfx_current_dimensions.width, th = (float) gfx_current_dimensions.height;
            float x0 = x, y0 = y, x1 = x + width, y1 = y + height;
            x0 = ulx <= 4 ? 0.0f : ceilf(x0 - 0.5f);
            y0 = uly <= 4 ? 0.0f : ceilf(y0 - 0.5f);
            x1 = lrx >= (uint32_t) (SCREEN_WIDTH - 1) << 2 ? tw : ceilf(x1 - 0.5f);
            y1 = lry >= (uint32_t) (SCREEN_HEIGHT - 1) << 2 ? th : ceilf(y1 - 0.5f);
            x = x0; y = y0;
            width = x1 > x0 ? x1 - x0 : 0.0f;
            height = y1 > y0 ? y1 - y0 : 0.0f;
        }
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

/* The palette the render tile reads: TLUT entries start at TMEM word 256; a TLUT loaded elsewhere (BK loads 16-colour
 * TLUTs at 256 + 16p) and the render tile's palette number select where the colours come from. */
static void gfx_palette_update(uint32_t siz) {
    int entry;
    if (rdp.tlut_base == NULL) return;
    entry = 256 + (siz == G_IM_SIZ_4b ? 16 * (int) rdp.tile_pal : 0) - (int) rdp.tlut_tmem;
    if (entry < 0 || entry > 255) entry = 0;
    rdp.palette = rdp.tlut_base + entry * 2;
}

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, UNUSED uint32_t palette, uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts) {
    // A zero mask means no wrapping: the RDP clamps that axis to the tile
    // whatever the clamp bit says (and mirroring needs a mask). OoT's sun and
    // moon quads run their UVs past a 64x64 texture with mask 0 -- two moons.
    if (masks == 0) cms = (cms & ~G_TX_MIRROR) | G_TX_CLAMP;
    if (maskt == 0) cmt = (cmt & ~G_TX_MIRROR) | G_TX_CLAMP;
    if (tile == gfx_rtile) {
        tri_state.valid = false;
    }
    _UNUSED(maskt);
    _UNUSED(masks);

    if (tile == gfx_rtile) {
        /* the render tile's palette (CI4 palette p = TLUT entries 16p..16p+15, TMEM words 256 + 16p on) */
        rdp.tile_pal = (uint8_t) palette;
        gfx_palette_update(siz);
        rdp.texture_tile.fmt = fmt;
        rdp.texture_tile.siz = siz;
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
    rdp.tiles[tile & 7].line_size_bytes = line * 8;
    rdp.tiles[tile & 7].pal = (uint8_t) palette;
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
    if (tile == gfx_rtile) {
        tri_state.valid = false;
    }
    if (tile == gfx_rtile) {
        rdp.texture_tile.uls = uls;
        rdp.texture_tile.ult = ult;
        rdp.texture_tile.lrs = lrs;
        rdp.texture_tile.lrt = lrt;
        gfx_render_tile_cm();
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
}

static void gfx_dp_load_tlut(UNUSED uint8_t tile, uint32_t high_index, uint32_t uls, uint32_t ult) {
    tri_state.valid = false;
    _UNUSED(high_index);

    SUPPORT_CHECK(rdp.texture_to_load.siz == G_IM_SIZ_16b);
    /* The TLUT starts (uls, ult) texels into the image (10.2 coordinates): GoldenEye keeps
     * each CI texture's palette right after its pixels and loads it from there. */
    rdp.tlut_base = rdp.texture_to_load.addr + ((uls >> 2) + (ult >> 2) * rdp.texture_to_load.width) * 2;
    rdp.tlut_tmem = rdp.tile_tmem[tile & 7];
    gfx_palette_update(rdp.texture_tile.siz);
    if (gfx_debug_frame) port_log("  loadtlut addr %p first %02X%02X %02X%02X %02X%02X\n", rdp.palette, rdp.palette[0], rdp.palette[1], rdp.palette[2], rdp.palette[3], rdp.palette[4], rdp.palette[5]);
}

/* TMEM is 4 KB: a load fills at most what lies past the load tile's TMEM address (the RDP wraps the rest to
 * the start of TMEM). An oversized LoadBlock / LoadTile (or LoadTile's lrs < uls, which wrapped the size to
 * gigabytes) is cut there, so no import or hash reads more than TMEM could hold. */
static inline uint32_t gfx_tmem_room(uint8_t tile, uint32_t size_bytes) {
    const uint32_t room = 4096 - ((uint32_t) rdp.tile_tmem[tile & 7] & 0x1FF) * 8;
    return size_bytes > room ? room : size_bytes;
}

static void gfx_dp_load_block(uint8_t tile, UNUSED uint32_t uls, UNUSED uint32_t ult, uint32_t lrs, uint32_t dxt) {
    tri_state.valid = false;
    _UNUSED(dxt);

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
    size_bytes = gfx_tmem_room(tile, size_bytes);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes = size_bytes;
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
    size_bytes = gfx_tmem_room(tile, size_bytes);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes = size_bytes;

    SUPPORT_CHECK(size_bytes <= 4096);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].addr =
        rdp.texture_to_load.addr + gfx_texture_image_offset(uls >> G_TEXTURE_IMAGE_FRAC, ult >> G_TEXTURE_IMAGE_FRAC);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].stride_bytes = gfx_texture_image_offset(0, 1);
    /* only a load into the render tile's TMEM sizes the render tile: BK's dialog letters load a 1-row
     * gradient for tile 1 after the glyph, and that row made every glyph 1 texel tall ("barcodes") */
    if (rdp.tile_tmem[tile & 7] == rdp.tile_tmem[gfx_rtile]) {
    rdp.texture_tile.uls = uls;
    rdp.texture_tile.ult = ult;
    rdp.texture_tile.lrs = lrs;
    rdp.texture_tile.lrt = lrt;
    gfx_render_tile_cm();
    }

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

/* GoldenEye's 1:1 HUD (hh_screen.h HH_TAG_GEHUD_*; gfx_draw_rectangle): 0 squeezed, 1 LEFT, 2 SIDE, 3 INTRO, 4 TOP.
 * BK_HUD1TO1=0: every HUD element squeezed, as before. */
static int gfx_ge_hud SAVESTATE_EXCLUDE, gfx_ge_hud_placed SAVESTATE_EXCLUDE;
static float gfx_ge_hud_left SAVESTATE_EXCLUDE, gfx_ge_hud_x0 SAVESTATE_EXCLUDE, gfx_ge_hud_scale SAVESTATE_EXCLUDE;
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
        if (gfx_ge_hud != 0 && sx < sy) {
            /* Tagged HUD (hh_screen.h HH_TAG_GEHUD_*): the layout's x at the height's scale, 1:1 on
             * a square output, instead of squeezed. LEFT / INTRO: the block's first rectangle (the message box)
             * fixes one left edge for all of it, and a box too wide to fit from there to the screen's
             * right margin is narrowed to fit (the Dam's "Byelomorye Dam, Arkangelsk, USSR": 236 px) --
             * so every message starts at the same x, long or short. SIDE: each rectangle keeps the
             * pixel margin of the edge it sits nearer.
             * Pulled into the corners (the user, 2026-09-29: "can the ammo move more into the corner/edge,
             * same with the message text (opposite edge)"): the counters ~8 px from their side and the
             * bottom, the message ~8 px from the left, both 8 px lower. The intro's captions take the
             * message's left edge (the user: "move the intro text to the same x as the message text ...
             * the dam text almost goes off screen") but keep the game's height (the user, earlier: "this
             * text shouldnt carry the other notification text move"); a block too wide to fit keeps 8 px
             * on the right as on the left. BK_HUDCORNER=0: the N64's margins. */
            static int sCorner SAVESTATE_EXCLUDE = -1;
            if (sCorner < 0) { const char *e = getenv("BK_HUDCORNER"); sCorner = !(e && e[0] == '0'); }
            const float w = (float) gfx_current_dimensions.width, x0 = ulx / 4.0f, x1 = lrx / 4.0f;
            if (gfx_ge_hud == 4) {
                /* TOP (the top message, bondview2.c): the game has placed and wrapped its text for 1:1
                 * (hh_screen.c hhHudMsgTopLeft / hhHudMsgTopRoom), so its x at the height's scale; the
                 * box behind it spans the game's whole width, so it spans the output's (as squeezed). */
                if (!(x0 <= 0.0f && x1 >= 2.0f * HALF_SCREEN_WIDTH)) {
                    ulxf = x0 * sy;
                    lrxf = x1 * sy;
                }
            } else if (gfx_ge_hud == 2) {
                float shift = 0.0f;
                if (0.5f * (x0 + x1) > HALF_SCREEN_WIDTH) shift = w - 2.0f * HALF_SCREEN_WIDTH * sy;
                if (sCorner) {
                    shift += (0.5f * (x0 + x1) > HALF_SCREEN_WIDTH ? 27.0f : -27.0f) * sy; /* a counter: to its side */
                    ulyf += 8.0f * sy;
                    lryf += 8.0f * sy;
                }
                ulxf = x0 * sy + shift;
                lrxf = x1 * sy + shift;
            } else {
                if (!gfx_ge_hud_placed) {
                    const float margin = sCorner ? 8.0f : 4.0f; /* corners: as far from the right as from the left */
                    float room;
                    gfx_ge_hud_placed = 1;
                    gfx_ge_hud_x0 = x0;
                    if (sCorner) gfx_ge_hud_left = (x0 - 22.0f) * sy;                  /* message and intro caption: to the left */
                    else if (gfx_ge_hud == 3) gfx_ge_hud_left = floorf(x0 * sx);       /* the intro: the game's place */
                    else gfx_ge_hud_left = x0 * sy;
                    room = w - margin - gfx_ge_hud_left;
                    gfx_ge_hud_scale = (x1 - x0) * sy > room ? room / ((x1 - x0) * sy) : 1.0f;
                }
                if (gfx_ge_hud == 1 && sCorner) {
                    ulyf += 8.0f * sy;
                    lryf += 8.0f * sy;
                }
                ulxf = gfx_ge_hud_left + (x0 - gfx_ge_hud_x0) * sy * gfx_ge_hud_scale;
                lrxf = gfx_ge_hud_left + (x1 - gfx_ge_hud_x0) * sy * gfx_ge_hud_scale;
            }
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
        // Under BK_TL (no PORT_GE_TL_ASPECT) the 3D fills the wide screen, so 2D
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
    gfx_select_rtile(tile); /* a texture rectangle names its tile itself (the HUD after a map drawn through tile 2) */
    /* An inverted rectangle (lower-right above or left of upper-left) covers no scanline on the
     * RDP. GoldenEye's watch list clips its text by moving a glyph's edges and emits such rects as
     * rows scroll out of the list window (uly 198, lry 50); drawn from 50 to 198 with a negative T
     * step, each glyph smeared up the whole watch screen (the user's "text streaks", round 17).
     * BK_TEXRECTINV=1: drawn as before. */
    if (lry < uly || lrx < ulx) {
        static int draw = -1;
        if (draw < 0) { const char *e = getenv("BK_TEXRECTINV"); draw = e && e[0] == '1'; }
        if (!draw) return;
    }
    if (rdp.color_image_address == rdp.z_buf_address && rdp.z_buf_address != NULL) {
        /* R34 (Smash 3b03a66): drawn into the depth image, never onto the screen, like the fill
         * rects above; counted by the census (BK_TILECENSUS=1), skipped unless BK_ZTEXRECT=1 */
        static int draw = -1;
        if (draw < 0) { const char *e = getenv("BK_ZTEXRECT"); draw = e && e[0] == '1'; }
        gfx_ztexrect_n++;
        if (!draw) return;
    }

    uint32_t saved_combine_mode = rdp.combine_mode;
    /* A blender that paints the FOG colour at FOG alpha over memory (P = CLR_FOG, A = A_FOG, M = CLR_MEM, B = 1-A):
     * BK's fades to white (gctransition: set fog colour, full-screen rectangle) ignore the combiner's output. The
     * compiled shaders have no blender stage, so the rectangle is drawn as the fog colour with its alpha.
     * Without this the Rare logo's fade to white showed black frames. */
    struct RGBA saved_prim = rdp.prim_color;
    uint8_t saved_cc1_2 = rdp.cc1[2];
    bool fog_blend = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_1CYCLE &&
                     ((rdp.other_mode_l >> 30) & 3) == G_BL_CLR_FOG && ((rdp.other_mode_l >> 26) & 3) == G_BL_A_FOG &&
                     ((rdp.other_mode_l >> 22) & 3) == G_BL_CLR_MEM;
    if (fog_blend) {
        rdp.prim_color = rdp.fog_color;
        rdp.cc1[2] = G_CCMUX_0;
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_PRIMITIVE), color_comb(0, 0, 0, G_ACMUX_PRIMITIVE), 0);
    }
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1 instead
        dsdx >>= 2;
        
        // Color combiner is turned off in copy mode
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_TEXEL0), color_comb(0, 0, 0, G_ACMUX_TEXEL0), 0);
        
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    else {
#ifdef PORT_GE_RAWCC
        /* 1/2-cycle: the RDP draws every pixel a sub-scanline of the rectangle touches, so
         * it covers whole N64 pixels, [floor(ul), ceil(lr)). Snap before scaling to the
         * target: GE's gun barrel (440x330 intro, one 0.75-px-tall texrect per image row)
         * scaled to 0.55 px per row, and a quarter of the target rows got no rectangle.
         * S and T stay the first pixel's, as on the RDP. */
        ulx &= ~3;
        uly &= ~3;
        lrx = (lrx + 3) & ~3;
        lry = (lry + 3) & ~3;
#endif
        // The lower-right edge is exclusive, so a rectangle to (319, 239) leaves a 1px
        // seam a CRT's overscan hid (DKR 9df6228). Only the one-short band.
        if (gfx_seamfix()) {
            if (lrx >= (SCREEN_WIDTH - 1) << 2 && lrx < SCREEN_WIDTH << 2) lrx = SCREEN_WIDTH << 2;
            if (lry >= (SCREEN_HEIGHT - 1) << 2 && lry < SCREEN_HEIGHT << 2) lry = SCREEN_HEIGHT << 2;
        }
    }
    
    // uls and ult are S10.5
    // dsdx and dtdy are S5.10
    // lrx, lry, ulx, uly are U10.2
    // lrs, lrt are S10.5
    /* G_TEXRECTFLIP (GE's sideways folder-tab labels, START / NEXT / PREVIOUS): S steps by
     * DsDx down the rectangle and T by DtDy across it, the texture turned about its diagonal
     * (gfx_soft draws it so while gfx_soft_rect_flip is set). The RDP samples each pixel at its
     * corner, a pixel-centre sampler half a step on: with a negative step that is the next
     * texel over, so those corners move one step. The old code negated both steps and kept S
     * across: every glyph read texels outside itself, and the labels came out as faint olive
     * smudges (the user, device IMG_0119). BK_RECTFLIP=0: the old mapping (A/B). */
    static int sRectFlip SAVESTATE_EXCLUDE = -1;
    if (sRectFlip < 0) { const char *e = getenv("BK_RECTFLIP"); sRectFlip = !(e && e[0] == '0'); }
    const bool flip_rdp = flip && sRectFlip;
    if (flip && !flip_rdp) {
        dsdx = -dsdx;
        dtdy = -dtdy;
    }
    int16_t width = !flip ? lrx - ulx : lry - uly;
    int16_t height = !flip ? lry - uly : lrx - ulx;
    float lrs = ((uls << 7) + dsdx * width) >> 7;
    float lrt = ((ult << 7) + dtdy * height) >> 7;
    if (flip_rdp && dsdx < 0) { /* one pixel's step in S10.5: dsdx / 32 */
        uls = (int16_t) (uls - dsdx / 32);
        lrs -= (float) (dsdx / 32);
    }
    if (flip_rdp && dtdy < 0) {
        ult = (int16_t) (ult - dtdy / 32);
        lrt -= (float) (dtdy / 32);
    }

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

    {
        extern bool gfx_soft_rect_flip;
        gfx_soft_rect_flip = flip_rdp;
        gfx_draw_rectangle(ulx, uly, lrx, lry);
        gfx_soft_rect_flip = false;
    }
    rdp.combine_mode = saved_combine_mode;
    if (fog_blend) {
        rdp.prim_color = saved_prim;
        rdp.cc1[2] = saved_cc1_2;
        tri_state.valid = false;
    }
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
static void gfx_s2dex_bg(bool copy, const void *ptr) {
    const HhObjBg *bg = (const HhObjBg *) ptr;
    const void *img;
    float sx = RATIO_X, sy = RATIO_Y, scw = 1.0f, sch = 1.0f;
    int x0, y0, x1, y1;

    if (bg == NULL || (uintptr_t) bg < PORT_REAL_PTR_MIN) return;
    img = seg_addr((uintptr_t) bg->imagePtr);
    if (img == NULL || (uintptr_t) img < PORT_REAL_PTR_MIN) { gfx_log_unresolved("G_BG", (uintptr_t) bg->imagePtr); return; }
    if (!copy && bg->scaleW != 0 && bg->scaleH != 0) { scw = bg->scaleW / 1024.0f; sch = bg->scaleH / 1024.0f; }
    gfx_flush();
    gfx_set_phase(false);
    x0 = (int) (bg->frameX / 4.0f * sx + 0.5f);
    y0 = (int) (bg->frameY / 4.0f * sy + 0.5f);
    x1 = (int) ((bg->frameX + bg->frameW) / 4.0f * sx + 0.5f);
    y1 = (int) ((bg->frameY + bg->frameH) / 4.0f * sy + 0.5f);
    if (gfx_debug_frame) port_log("  bg %s img %p %ux%u fmt %u siz %u frame (%d,%d)-(%d,%d) scale %.3f\n", copy ? "copy" : "1cyc", img, bg->imageW / 4, bg->imageH / 4, bg->imageFmt, bg->imageSiz, x0, y0, x1, y1, scw);
    gfx_soft_draw_bg(img, bg->imageW / 4, bg->imageH / 4,
                     bg->imageFmt == G_IM_FMT_CI ? 1 : 0, rdp.palette, x0, y0, x1, y1,
                     bg->imageX / 32.0f, bg->imageY / 32.0f, scw / sx, sch / sy);
}

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    if (rdp.color_image_address == rdp.z_buf_address) {
        // Don't clear Z buffer here since we already did it with glClear
        return;
    }
    uint32_t mode = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));
    
    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    } else {
        // 1/2-cycle: the lower-right edge is exclusive, so a full-screen dim to
        // (319, 239) leaves a 1px seam a CRT's overscan hid (MK64 99921c7).
        if (lrx >= (SCREEN_WIDTH - 1) << 2) lrx = SCREEN_WIDTH << 2;
        if (lry >= (SCREEN_HEIGHT - 1) << 2) lry = SCREEN_HEIGHT << 2;
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

void *gfx_main_color_image SAVESTATE_EXCLUDE; /* the last 320-wide colour image of the frame (hh_gfx.c writes the
                                                * picture back there: the game reads its frame buffers) */
static void gfx_dp_set_color_image(uint32_t format, uint32_t size, uint32_t width, void* address) {
    _UNUSED(format);
    _UNUSED(size);

    rdp.color_image_address = address;
    { /* which N64 frame buffer this frame draws into (hh_gfx_before_swap writes the picture back to that one) */
      extern int hh_gfx_cimg_fb; const uintptr_t a = (uintptr_t) address, fb0 = 0x803A5D00u, fbsz = 292u * 216u * 2u;
      if (a >= fb0 && a < fb0 + 2 * fbsz) hh_gfx_cimg_fb = (int) ((a - fb0) / fbsz); }
    if (width + 1 == 320 && address != rdp.z_buf_address) gfx_main_color_image = address;
    width += 1; /* the command holds width - 1 */
    if (width >= 256 && width <= 640 && (int)width != gfx_n64_w && address != rdp.z_buf_address) {
        gfx_flush();
        gfx_n64_w = (int)width;
        gfx_n64_h = (int)width * 3 / 4;
        if (gfx_raw_vp_valid) gfx_calc_and_set_viewport(&gfx_raw_vp);
        if (gfx_raw_sc_valid) gfx_dp_set_scissor(0, gfx_raw_sc[0], gfx_raw_sc[1], gfx_raw_sc[2], gfx_raw_sc[3]);
        rdp.viewport_or_scissor_changed = true;
        tri_state.valid = false;
    }
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
 * CPU's gSegments (port_seg_to_ptr), as oot-PSP's interpreter does. Game code only
 * ever passes real pointers, segmented addresses or NULL as bases (checked:
 * no gSPSegment call site converts to a physical address). */
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

extern uint32_t hh_rsp_poll(void) __attribute__((weak));
/* gSPDisplayList calls nest as deep as the microcode's display-list stack: 18 in F3DEX2, 10 in F3D/F3DEX.
 * A deeper call is skipped, once logged (the RSP would push its return address past the stack, over DMEM;
 * here unbounded recursion ran the C stack out before the command budget could end the frame). */
#ifdef F3DEX_GBI_2
#define GFX_DL_STACK_DEPTH 18
#else
#define GFX_DL_STACK_DEPTH 10
#endif
static int gfx_dl_depth;
static int gfx_dl_call_ok(uintptr_t w1) {
    static int logged;
    if (gfx_dl_depth < GFX_DL_STACK_DEPTH) return 1;
    if (!logged) { logged = 1; port_log("gfx: display list call %08X past the %d-deep call stack skipped\n", (unsigned) w1, GFX_DL_STACK_DEPTH); }
    return 0;
}

static void gfx_run_dl(Gfx* cmd) {
    for (;;) {
        uint32_t opcode = cmd->words.w0 >> 24;
        if (gfx_trace_frames > 0 && gfx_cmd_budget < 60000) {
            port_log("dl %p: %08X %08X\n", cmd, (unsigned) cmd->words.w0, (unsigned) cmd->words.w1);
        }
        if (++gfx_cmd_budget > 400000) {
            port_log("gfx_run_dl: runaway display list at %p (op %02X)\n", cmd, opcode);
            return;
        }
        if ((gfx_cmd_budget & 63) == 0 && hh_rsp_poll != NULL)
            (void)hh_rsp_poll(); /* GoldenEye's BK_RSPYIELD (hh_os.c): a long frame lets the audio thread run */
#ifdef PORT_PROFILE_DL
        gfx_prof_opcount[opcode]++;
        gfx_prof_cmds++;
        int prof_slot = (opcode == 0x04) ? 4 : (opcode == 0xBF || opcode == 0xB1 || opcode == 0xB5) ? 5 : (opcode == 0x06 || opcode == 0xB8 || opcode == 0x00) ? 0 : 6;
        uint32_t prof_t0 = prof_slot ? port_time_us() : 0;
#endif
        switch (opcode) {
            case G_NOOP:
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
                if (cmd->words.w1 == HH_TAG_SKY_BEGIN || cmd->words.w1 == HH_TAG_SKY_END) {
                    extern void gfx_scegu_set_sky_pass(bool on);
                    gfx_flush();
                    gfx_scegu_set_sky_pass(cmd->words.w1 == HH_TAG_SKY_BEGIN);
                    break;
                }
                if (cmd->words.w1 == HH_TAG_FPMODEL_ON || cmd->words.w1 == HH_TAG_FPMODEL_OFF) {
                    extern void gfx_soft_set_fpmodel(bool on);
                    gfx_flush();
                    gfx_soft_set_fpmodel(cmd->words.w1 == HH_TAG_FPMODEL_ON);
                    break;
                }
                if (cmd->words.w1 >= HH_TAG_GEHUD_OFF && cmd->words.w1 <= HH_TAG_GEHUD_TOP) {
                    static int sOn SAVESTATE_EXCLUDE = -1;
                    if (sOn < 0) { const char *e = getenv("BK_HUD1TO1"); sOn = !(e && e[0] == '0'); }
                    gfx_ge_hud = sOn ? (int) (cmd->words.w1 - HH_TAG_GEHUD_OFF) : 0;
                    gfx_ge_hud_placed = 0;
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
#if !defined(F3DEX_GBI) && !defined(F3DEX_GBI_2)
            case 0xB1:
            {
                /* GoldenEye (Rare's F3D): gSP4Triangles -- up to four triangles with
                 * direct 4-bit vertex indices; triangle i = (x, y, z) with x, y in w1
                 * bits 8i..8i+7 and z in w0 bits 4i..4i+3. Unused slots are (0,0,0);
                 * a triangle with a repeated index has no area on the N64 either. */
                uint32_t w0 = (uint32_t) cmd->words.w0, w1 = (uint32_t) cmd->words.w1;
                int t;
                for (t = 0; t < 4; t++) {
                    uint8_t x = (w1 >> (8 * t)) & 0xF, y = (w1 >> (8 * t + 4)) & 0xF, z = (w0 >> (4 * t)) & 0xF;
                    if (x == y || y == z || x == z) continue;
                    gfx_sp_tri1(x, y, z);
                }
                break;
            }
#endif
            case (uint8_t) G_CULLDL:
            {
                // gSPCullDisplayList(first, last): end this list when every one of
                // those vertices is outside the same plane (OoT opens room/object
                // lists and each sky face with one; oot-PSP gfx_fast3d.c).
                // Exact frustum planes (the guard-band clip_rej is too loose to
                // ever cull); far is left out, as OoT's microcode does not clip it.
#if defined(F3DEX_GBI) || defined(F3DEX_GBI_2)
                uint32_t first = C0(0, 16) / 2, last = C1(0, 16) / 2, rej = rsp.is_persp ? 0x1fu : 0u;
#else
                /* F3D: vstart*40 and ((vend+1) & 15)*40, 40 = the RSP's vertex stride */
                uint32_t first = C0(0, 16) / 40, end = C1(0, 16) / 40, last = (end == 0 ? 16 : end) - 1;
                uint32_t rej = rsp.is_persp ? 0x1fu : 0u;
#endif
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
                    if (gfx_debug_frame) port_log("  culldl %u..%u %s\n", first, last, rej != 0 ? "CULLED" : "kept");
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
                    if (gfx_dl_call_ok(cmd->words.w1)) {
                        gfx_dl_depth++;
                        gfx_run_dl((Gfx *)seg_addr(cmd->words.w1));
                        gfx_dl_depth--;
                    }
                } else {
                    cmd = (Gfx *)seg_addr(cmd->words.w1);
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
#ifdef PORT_GE_RAWCC
            case (uint8_t)G_RDPHALF_1:
                gfx_rdp_raw_hi = cmd->words.w1;
                break;
#ifdef G_RDPHALF_CONT
            case (uint8_t)G_RDPHALF_CONT:
#endif
            case (uint8_t)G_RDPHALF_2:
                gfx_rdp_raw_push(cmd->words.w1);
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
                gfx_dp_load_tlut(C1(24, 3), C1(14, 10), C0(12, 12), C0(0, 12));
                break;
            case G_SETENVCOLOR:
                gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMCOLOR:
                if (rdp.prim_lod_frac != (uint8_t) C0(0, 8) || rdp.prim_min_level != (uint8_t) C0(8, 5)) {
                    rdp.prim_lod_frac = (uint8_t) C0(0, 8);
                    rdp.prim_min_level = (uint8_t) C0(8, 5);
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
                gfx_dp_set_color_image(C0(21, 3), C0(19, 2), C0(0, 12), seg_addr(cmd->words.w1));
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
    /* Each task starts from the microcode's DMEM image: GoldenEye's F3D 2.0G has lookat Y
     * dir (127, 0, 0) and lookat X (0, 127, 0) -- the values SM64 loads itself with
     * guLookAtReflect(up = x). The Nintendo and Rare logos texgen from these. */
    memset(rsp.current_lookat, 0, sizeof(rsp.current_lookat));
    rsp.current_lookat[0].dir[0] = 127;
    rsp.current_lookat[1].dir[1] = 127;
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
        const char *e = getenv("BK_DRAWDIST"); char *end = NULL;
        gfx_draw_dist_w = 1e30f;
        if (e && e[0]) {
            const double d = strtod(e, &end);
            if (end && *end == '\0' && d > 0.0 && d < 1e9) { gfx_draw_dist_w = (float)d; port_log("gfx: far cull at clip.w %.0f (MK64_DRAWDIST)\n", d); }
            else port_log("gfx: ignoring MK64_DRAWDIST='%s' (want a positive number)\n", e);
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
    gfx_rapi->set_depth_test(false); /* the backend must match the cache: only changes are sent */
    gfx_rapi->set_depth_mask(false);
    gfx_rapi->set_zmode_decal(false);
    rendering_state.alpha_blend = true;
    {   /* BK_LOADSYNC (2026-10-10, the cross-port check after Smash's title, ssb64-rc 9e9bb19): the depth flags above go
         * to the backend too, but blend was reset in the cache only, and the soft backend kept what the last draw
         * before a state load set. A draw sends blend only when it differs from the cache, so with the cache saying
         * "blend" and the backend not blending, the blended draws after the load came out opaque until an opaque one
         * came between. Now the backend is set to what the cache says. BK_LOADSYNC=0 (debug.cfg loadsync=0): as before. */
        static int sLoadSync SAVESTATE_EXCLUDE = -1;
        if (sLoadSync < 0) { const char *e = getenv("BK_LOADSYNC"); sLoadSync = !(e && e[0] == '0'); }
        if (sLoadSync)
            gfx_rapi->set_use_alpha(rendering_state.alpha_blend);
    }
    rendering_state.tex_add = false;
    rendering_state.force_in = false;
    rendering_state.tex_lerp = false;
    rendering_state.own_edges = false;
    {
        extern void gfx_scegu_set_force_in(bool on);
        extern void gfx_scegu_set_tex_lerp(bool on, uint8_t r, uint8_t g, uint8_t b);
        extern void gfx_soft_set_own_edges(bool on);
        gfx_scegu_set_force_in(false);
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
}

/* Called right after a savestate load restores .data/.bss. The texture cache
 * (saved) now points into the arena (excluded/flushed), so drop the cache and
 * clear the backend arena; textures re-import from the restored course data on
 * the next frame (same coordinated flush the arena-full path uses each frame).
 * Then forget cached GE state so the whole pipeline is re-sent. */
void gfx_savestate_fixup(void) {
    extern void hh_os_state_loaded(void) __attribute__((weak));
    gfx_texture_cache_reset(false);
    gfx_overlay_state_dirty();
    if (hh_os_state_loaded != NULL) hh_os_state_loaded(); /* GoldenEye: the game clock against the host's */
}

void gfx_start_frame(void) {
#ifdef PORT_GE_RAWCC
    gfx_rdp_raw_n = 0; /* a command cut off by the last frame's end */
#endif
    gfx_frame_counter++;
    memset(rsp_segments, 0, sizeof(rsp_segments));
#ifdef PORT_GE_TL
    memset(ge_last_mp, 0, sizeof(ge_last_mp)); // GE list reset each frame -> re-push GU_PROJECTION on the first batch
    ge_list_used = 0;
#endif
    gfx_hud_anchor = 0;
    gfx_ge_hud = 0;
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
    //sceIoWrite(1, "----START FRAME!\n", 18);
    total_t0 = sceKernelLibcClock();
    gfx_wapi->handle_events();
}

void gfx_run(Gfx *commands) {
    gfx_sp_reset();
    gfx_cmd_budget = 0;
    gfx_debug_frame = gfx_trace_frames > 0; /* OOT_TRACE_AT: per-draw state too */
    
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
    if (gfx_trace_frames > 0) {
        gfx_trace_frames--;
        port_log("dl done\n");
    }
    gfx_debug_frame = 0; // a one-frame switch (set by the input script / debug hooks before the frame)
    gfx_flush();
    if (gfx_vh_enabled()) {
        fprintf(gfx_vh_file, "%u vtxin %08X mtx %08X rdptri %08X vtxout %08X tribuf %08X\n", (unsigned) gfx_vh_frame++,
                (unsigned) gfx_vh[0], (unsigned) gfx_vh[1], (unsigned) gfx_vh[2], (unsigned) gfx_vh[3], (unsigned) gfx_vh[4]);
        fflush(gfx_vh_file);
        for (int i = 0; i < 5; i++) gfx_vh[i] = 2166136261u;
    }
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
     * kicking in. Grab console.log from the data dir after a session. */
    gfx_last_render_ms = (t1 - t0) / 1000.0f; /* interpreter+raster+present, this frame */
}

void gfx_end_frame(void) {
    
    //sceIoWrite(1, "----END FRAME!\n", 16);
    if (!dropped_frame) {
        gfx_rapi->finish_render();
        { extern void hh_gfx_before_swap(void); hh_gfx_before_swap(); }
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
