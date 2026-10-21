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
#include <PR/ultratypes.h>
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
	short u, v; /* S10.5: a texrect may start left of / above texel 0 (super-smash-bros 4843c2c) */
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
    uint16_t view_w, view_h; // the tile view imported: one address can be drawn through different
    uint32_t view_stride;    // views (a LoadTile sub-rectangle, another tile size) -- fzerox's fix
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
    Light_t current_lookat[2]; // [0] = G_MV_LOOKATY, [1] = G_MV_LOOKATX, as the F3D DMEM holds them (SF64_LOOKAT)
    uint8_t current_num_lights; // includes ambient light
    bool lights_changed;
    
    uint32_t geometry_mode;
    int16_t fog_mul, fog_offset;
    float clip_ratio; // gSPClipRatio (G_MW_CLIP): the guard band is +-clip_ratio * w (Star Fox: FRUSTRATIO_1)
    
    struct {
        // U0.16
        uint16_t s, t;
    } texture_scaling_factor;
    
    struct VertexColor loaded_vertices_2D[4];
    struct LoadedVertex loaded_vertices[MAX_VERTICES];
} rsp  __attribute__((aligned(16)));

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
    } loaded_texture[2];
    struct {
        uint8_t fmt;
        uint8_t siz;
        uint8_t cms, cmt;         // effective (gfx_tile_cm): what the sampler does
        uint8_t cms_raw, cmt_raw; // as the display list set them (after the mask-0 clamp)
        uint8_t masks, maskt;     // wrap period 1 << mask texels (0 = none)
        uint8_t tmem_slot; // which loaded_texture[] the render tile reads: its TMEM half (tmem / 256)
        uint8_t palette;   // CI4: which 16-entry bank of the loaded TLUT (SF64 uses 1-15; MK64 only 0)
        uint16_t uls, ult, lrs, lrt; // U10.2
        uint32_t line_size_bytes;
    } texture_tile;
    bool textures_changed[2];
    // Every tile descriptor (OoT's two-texture combiners read tile 1 as TEXEL1).
    struct {
        uint8_t fmt, siz, cms, cmt, shifts, shiftt, masks, maskt;
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
    int edge_opaque; // TEX_EDGE without FORCE_BL: the soft backend's X12 mode (0 blend, 1 unblended, 2 unblended over empty pixels)
    bool own_edges;  // SF64_FILLRULE: blended / alpha-tested without AA_EN, each pixel to one triangle of an edge
    bool fog_px; struct RGBA fog_c; int fog_a; // backend per-pixel fog on, its colour, the constant alpha the factor displaced (-1: none)
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
    bool fog_px;                           // fogged per pixel by the backend, not folded (see gfx_tri_fog)
    bool fog_raw_alpha;                    // fog_px: the raw factor replaces the combined vertex alpha (not for a shade alpha)
    uint8_t fog_path;                      // SF64_CC_CENSUS: which fog rule the draw took (gfx_tri_fog)
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
static unsigned long get_time(void) {
    extern uint64_t port_time_us64(void); /* sdl/main.c */
    return (unsigned long)port_time_us64();
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
/* Exact side planes (x > w etc., homogeneous). Kept apart from the guard-band
 * bits above, which gate the early reject BEFORE the phase bookkeeping in
 * gfx_sp_tri1; these reject after it (MK64 perf round 24, via ocarina-of-time
 * d267ca3). */
#define X_OUT_L (0x100)
#define X_OUT_R (0x200)
#define Y_OUT_B (0x400)
#define Y_OUT_T (0x800)
#define XY_OUT (X_OUT_L | X_OUT_R | Y_OUT_B | Y_OUT_T)
/* Beyond the RSP's guard band, |x| or |y| > clip_ratio * |w| in clip space (gSPClipRatio; Star Fox
 * sets FRUSTRATIO_1 every frame, fox_rcp_init.c, so its band is the screen edge): the triangle goes
 * to the clipper, which cuts it there (R33, below). One bit for all four sides, so three vertices
 * sharing it may be on opposite sides of the screen: it never joins the shared-flag reject. */
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
	{  0.f,  0.f, -1.f, -1.f }	// z >= -w: the near plane, where the RSP clips. (The kit pulled it in to
	                            // -0.97 w for the PSP GE, which mishandles a vertex on its own near plane.)
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
#define W_MIN 2.0f
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

/* The sides (R33, diddy-kong-racing 467ad1e; the lead's round 2). The shade colour is interpolated on
 * screen, so where a triangle is cut changes its shading. The console sends a triangle to its
 * clipper when a vertex is in front of the near plane, beyond the far one or beyond the guard band
 * (+-clip_ratio * w), and cuts it at those planes, so a cut begins where a vertex crosses one and
 * the shading can't jump. The kit cut at the exact screen edges only once a triangle had been sent
 * for its near or far plane, so a big foreground triangle's shading jumped the frame a vertex
 * crossed the near plane (DKR's and GoldenEye's floor "flashing" on the device). Star Fox's band IS
 * the screen edge (FRUSTRATIO_1), so the planes are the ones it always had; what changes is the
 * trigger: a vertex beyond them (GB_OUT). SF64_CLIPEDGES: 2 the RSP's guard band and trigger;
 * 1 (default again, round 12) the kit's (the screen edges, cut only for a near/far triangle): R33
 * cost +1.6% ARM instructions and changed nothing visible on a square output, so the user's rule keeps it
 * off; 0 no side planes. */
static int gfx_clipedges SAVESTATE_EXCLUDE = 1;

/* The polygon starts in v0; each pass writes the other buffer. Returns the vertex count and sets
 * *res to the buffer holding the result (DKR's form: the number of passes no longer matters). */
static uint32_t clip_to_frustum( struct LoadedVertex * v0, struct LoadedVertex * v1, uint32_t vIn, struct LoadedVertex **res )
{
	uint32_t vOut = vIn;
	struct LoadedVertex *src = v0, *dst = v1, *t;
#define CLIP_PASS(call) do { vOut = (call); t = src; src = dst; dst = t; } while (0)
	// Near plane (w-based) first: removes behind-camera parts so the NDC-plane
	// clips below see only w>0 vertices.
	CLIP_PASS( clipToNearW( dst, src, vOut ) );
	if (gfx_clipedges) {
		const float r = gfx_clipedges == 2 ? rsp.clip_ratio : 1.0f;
		const float side[4][4] = { { -1.f, 0.f, 0.f, -r }, { 1.f, 0.f, 0.f, -r }, { 0.f, -1.f, 0.f, -r }, { 0.f, 1.f, 0.f, -r } };
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, side[0] ) );	// x >= -r w
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, side[1] ) );	// x <=  r w
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, side[2] ) );	// y >= -r w
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, side[3] ) );	// y <=  r w
	}
	CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[0] ) );	// the far plane, z <= w (labelled "near")
	CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[5] ) );	// the near plane, z >= -w
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

    /* The passes bounce temp_a <-> temp_b; poly is where they ended. The kit
     * fanned temp_a -- the polygon before the last (near) pass, with the last
     * pass's vertex count (super-mario-64 5cd6699, the cross-port lead's B1). */
    struct LoadedVertex *poly;
    uint32_t out = clip_to_frustum( temp_a, temp_b, 3, &poly );
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
 * wiring). Desktop tuning only: SF64_DRAWDIST=<units>, validated and parsed
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

/* Bumped by every texture cache reset (ocarina-of-time 2b69f7b, the lead's cross-port check). A reset wipes
 * every texture, including the one the OTHER tile of the draw being set up already holds: importing
 * tile 1 could reset the cache and leave rendering_state.textures[0] on a zeroed pool node, and the draw
 * went out untextured -- a flat block of the combiner's colour (OoT's red sunset bar after a long
 * session had filled the arena). The import loops run again when it changes under them.
 * SF64_EPOCHFIX=0: the old behaviour (A/B). */
static uint32_t gfx_cache_epoch SAVESTATE_EXCLUDE;
static bool gfx_stress_hold SAVESTATE_EXCLUDE; /* the epoch re-run pass: no forced reset (a real one can't fire on an emptied arena) */
/* SF64_CC_CENSUS: mid-frame resets, draws importing both tiles, resets under a draw's own imports,
 * and tile-1 cache hits whose sampler call lands on tile 0 with other clamp bits (pm64 853ed45's note) */
static uint32_t gfx_census_midreset SAVESTATE_EXCLUDE, gfx_census_t1draw SAVESTATE_EXCLUDE, gfx_census_underreset SAVESTATE_EXCLUDE,
                gfx_census_t1samp SAVESTATE_EXCLUDE;
static int gfx_epochfix SAVESTATE_EXCLUDE = -1;
static bool gfx_epochfix_on(void) {
    if (gfx_epochfix < 0) { const char *e = getenv("SF64_EPOCHFIX"); gfx_epochfix = !(e && e[0] == '0'); }
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
        gfx_census_midreset++;
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
static inline bool gfx_is_static_memory(const void *p) {
    const char *c = (const char *) p;
    if (c >= course_tex_lo && c < course_tex_hi) {
        return !(c >= fb_tile_lo && c < fb_tile_hi);
    }
    return (c >= _ftext && c < _fbss) || (c >= __assets_start && c < __assets_end);
}

/* The loaded_texture[] slot a texture unit reads: unit 0 is the render tile,
 * which addresses either TMEM half; unit 1 keeps its own slot. */
static inline int gfx_tex_slot(int tile) {
    return tile == 0 ? rdp.texture_tile.tmem_slot : tile;
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
 * large it is. A tile bigger than its texture repeats that texture; importing
 * the whole tile read the bytes after it as texels (ocarina-of-time ab65ddd: adult
 * Link's gauntlets and tights; ssb64 stage grounds). SF64_TILEMASK=0 restores
 * the old whole-tile import for A/B. */
static int gfx_tilemask SAVESTATE_EXCLUDE = -1;
static inline bool gfx_tilemask_on(void) {
    if (gfx_tilemask < 0) { const char *e = getenv("SF64_TILEMASK"); gfx_tilemask = !(e && e[0] == '0'); }
    return gfx_tilemask != 0;
}
static inline uint32_t gfx_mask_period(uint32_t n, uint8_t mask) {
    return (mask != 0 && mask < 16 && n > (1u << mask) && gfx_tilemask_on()) ? (1u << mask) : n;
}
static int gfx_census_mode(void);
static void gfx_census_log(uint32_t *n, const char *what, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
static uint32_t gfx_census_mirmask SAVESTATE_EXCLUDE; /* pm64 fix 1: MIRROR|CLAMP, period < extent <= 2 periods */
/* The sampler's wrap for one axis: a clamped tile wider than its mask period
 * repeats inside the tile (the RDP clamps to the tile, then masks), so it wraps. */
static inline uint8_t gfx_tile_cm(uint8_t cm, uint8_t mask, uint16_t lo, uint16_t hi) {
    const int extent = ((int) hi - (int) lo + 4) / 4;
    if (gfx_census_mode() && mask != 0 && mask < 16 && (cm & G_TX_CLAMP) && (cm & G_TX_MIRROR) &&
        extent > (1 << mask) && extent <= (2 << mask))
        gfx_census_log(&gfx_census_mirmask, "MIRROR|CLAMP tile, period < extent <= 2 periods (mask extent)", mask, (uint32_t) extent, 0, 0);
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
    /* A LoadBlock tile that wraps (mask set, clamp off) reads one whole period of
     * its mask from its corner, however far apart its corners are: the RDP
     * subtracts the corner, then masks, and the extent only bounds a clamp. Star
     * Fox scrolls its ground and sea by moving the upper-left corner alone
     * (gDPSetupTile: dw, dh to w-1, h-1; Background_DrawGround), so the
     * corner-to-corner size shrank the 32x32 texture to 32 - scroll rows,
     * repeating that strip at a period that changed every frame. Only while the
     * loaded block holds the whole period. SF64_TILESCROLL=0 keeps the
     * corner-to-corner size (A/B); SF64_TILEMASK=0 turns it off with the rest of
     * the mask handling. */
    static int sTileScroll SAVESTATE_EXCLUDE = -1;
    if (sTileScroll < 0) { const char *e = getenv("SF64_TILESCROLL"); sTileScroll = !(e && e[0] == '0'); }
    if (sTileScroll && rdp.loaded_texture[gfx_tex_slot(tile)].stride_bytes == 0 && gfx_tilemask_on()) {
        const uint8_t ms = rdp.texture_tile.masks, mt = rdp.texture_tile.maskt;
        const uint32_t line = rdp.texture_tile.line_size_bytes * (rdp.texture_tile.siz == G_IM_SIZ_32b ? 2u : 1u);
        const uint32_t loaded = rdp.loaded_texture[gfx_tex_slot(tile)].size_bytes;
        if (ms != 0 && ms < 16 && !(rdp.texture_tile.cms_raw & G_TX_CLAMP) && *width < (1u << ms) &&
            tex_bytes_per_row(1u << ms, rdp.texture_tile.siz) <= line) {
            *width = 1u << ms;
        }
        if (mt != 0 && mt < 16 && !(rdp.texture_tile.cmt_raw & G_TX_CLAMP) && *height < (1u << mt) &&
            (1u << mt) * line <= loaded) {
            *height = 1u << mt;
        }
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

/* The palette a CI texture reads: a CI4 tile picks one 16-entry bank of the
 * loaded TLUT with its palette field (32 bytes per bank); CI8 uses all 256. */
static inline const uint8_t *gfx_tile_palette(uint32_t siz) {
    return rdp.palette + (siz == G_IM_SIZ_4b ? rdp.texture_tile.palette * 32u : 0u);
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
                return hash_bytes(h, gfx_tile_palette(siz), siz == G_IM_SIZ_4b ? 32 : 512);
            }
            // Static texels with a static palette: the palette's ADDRESS still
            // has to take part (Rainbow Road animates by cycling through
            // several TLUTs over the same CI texels).
            return (uint32_t) (uintptr_t) gfx_tile_palette(siz) | 1u;
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
        h = hash_bytes(h, gfx_tile_palette(siz), siz == G_IM_SIZ_4b ? 32 : 512);
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
    uint32_t bpp = 4; /* gfx_soft stores every texture as ARGB8888 (5551 unpacked at upload) */
    _UNUSED(fmt); _UNUSED(siz);
    uint32_t pw = 1, ph = 1;
    if (width * height > GFX_MAX_IMPORT_TEXELS) height = GFX_MAX_IMPORT_TEXELS / (width ? width : 1);
    width <<= mirror & 1;
    height <<= (mirror >> 1) & 1;
    while (pw < width) pw <<= 1;
    while (ph < height) ph <<= 1;
    return pw * ph * bpp;
}

/* SF64_CC_CENSUS (see gfx_cc_census): lookups whose texels, format and address
 * match a cached import made through another tile view -- the draws the view in
 * the key sends to their own import. */
static int gfx_census_views SAVESTATE_EXCLUDE = -1; /* -1: SF64_CC_CENSUS not read yet */
static uint32_t gfx_census_splits SAVESTATE_EXCLUDE;
static int gfx_census_mode(void);
static void gfx_census_view_split(const uint8_t *addr, uint32_t w0, uint32_t h0, uint32_t s0, uint32_t w1, uint32_t h1,
                                  uint32_t s1) {
    if (gfx_census_splits++ < 8) {
        port_log("cc census: frame %u tile view split at %p: %ux%u stride %u vs %ux%u stride %u\n", gfx_frame_counter,
                 (const void *) addr, w0, h0, s0, w1, h1, s1);
    }
}

/* SF64_TEXSTRESS=<n> (test): reset the texture cache every n new imports, as a long session's full
 * arena does, so a sweep can show draws survive a reset under their own imports (gfx_cache_epoch):
 * a reset only forces re-imports, so the frames must come out bit-identical. */
static bool gfx_tex_stress(void) {
    static int sEvery SAVESTATE_EXCLUDE = -1, sCount SAVESTATE_EXCLUDE;
    if (sEvery < 0) { const char *e = getenv("SF64_TEXSTRESS"); sEvery = e ? atoi(e) : 0; }
    return sEvery > 0 && !gfx_stress_hold && ++sCount % sEvery == 0;
}

static bool gfx_texture_cache_lookup(int tile, struct TextureHashmapNode **n, const uint8_t *orig_addr, uint32_t fmt, uint32_t siz, uint8_t mirror) {
    size_t hash = (uintptr_t)orig_addr;
    uint32_t content_hash = gfx_texture_content_hash(tile, fmt, siz);
    uint32_t upload_bytes = gfx_texture_upload_bytes(tile, fmt, siz, mirror);
    struct TextureHashmapNode *stale = NULL;
    /* The tile view is part of the key (super-mario-64 1e163a0, from ocarina-of-time
     * 0a3cadd / f-zero-x 09f55f3; the cross-port lead's R3): static texels
     * hash to a constant, and a LoadBlock's hash covers the block, not the view,
     * so a second view of the same address (another tile size, a LoadTile
     * sub-rectangle) would reuse the first view's import. */
    uint32_t view_w, view_h;
    const uint32_t view_stride = rdp.loaded_texture[gfx_tex_slot(tile)].stride_bytes;
    gfx_tile_size(tile, &view_w, &view_h);
    if (gfx_census_views < 0) gfx_census_mode();
    hash = (hash >> 5) & 0x3ff;
    struct TextureHashmapNode **node = &gfx_texture_cache.hashmap[hash];
    while (*node != NULL && *node - gfx_texture_cache.pool < (int)gfx_texture_cache.pool_pos) {
        if (gfx_census_views > 0 && (*node)->texture_addr == orig_addr && (*node)->fmt == fmt && (*node)->siz == siz &&
            (*node)->mirror == mirror && (*node)->content_hash == content_hash &&
            ((*node)->view_w != view_w || (*node)->view_h != view_h || (*node)->view_stride != view_stride)) {
            gfx_census_view_split(orig_addr, (*node)->view_w, (*node)->view_h, (*node)->view_stride, view_w, view_h,
                                  view_stride);
        }
        if ((*node)->texture_addr == orig_addr && (*node)->fmt == fmt && (*node)->siz == siz && (*node)->mirror == mirror &&
            (*node)->view_w == view_w && (*node)->view_h == view_h && (*node)->view_stride == view_stride) {
            if ((*node)->content_hash == content_hash) {
                if (tile == 1 && rendering_state.textures[0] != NULL &&
                    (((*node)->cms ^ rendering_state.textures[0]->cms) & G_TX_CLAMP || ((*node)->cmt ^ rendering_state.textures[0]->cmt) & G_TX_CLAMP))
                    gfx_census_t1samp++;
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
    { extern const void *gfx_soft_tex_src; gfx_soft_tex_src = rdp.loaded_texture[gfx_tex_slot(tile)].addr; } /* SF64_FRAMESTAT / drawprof names */
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
        /* LoadBlock copies DRAM into TMEM linearly and the RDP samples rows at
         * the render tile's line size (SetTile `line`, 64-bit words), not at
         * width * bpp (super-smash-bros a55820b: 27-30 px sprites in 32 px rows
         * skewed into noise). 32-bit texels split across the two TMEM halves,
         * so `line` counts half a row there. Never tighter than the texels. */
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
        const uint8_t *pal = gfx_tile_palette(siz);
        if (gfx_debug_frame) port_log("  ci import palette %p tex %p idx0 %02X %02X %02X %02X\n", rdp.palette, src, src[0], src[1], src[2], src[3]);
        for (y = 0; y < height; y++) {
            const uint8_t* row = TEXEL_ROW(y);
            for (x = 0; x < width; x++) {
                uint8_t idx = (siz == G_IM_SIZ_4b) ? nib(row, x) : row[x];
                out16[y * width + x] = n64_rgba16_to_psp5551(be16(pal + idx * 2));
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
 * when it reads the other TMEM half (loaded_texture[1]), OoT's usual layout
 * (tile 0 at TMEM 0, tile 1 at 0x100). */
static bool gfx_tile1_importable(void) {
    return rdp.texture_tile.tmem_slot == 0 && ((rdp.tiles[1].tmem / 256) & 1) == 1;
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
    rdp.texture_tile.tmem_slot = 1;
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
                                    int krgb_src, const uint8_t krgb[3], int ka_src, uint8_t ka);

/* Cycle 1 mixes TEXEL0 and TEXEL1 (OoT's room/water/effect combiners): bind a
 * cached pre-blend of both tiles as the one texture. Returns its id (0 = draw
 * tile 0 as before) and the blended texture size in *bw, *bh. */
static uint32_t gfx_blend_tiles(uint32_t *bw, uint32_t *bh) {
    const uint8_t *m = rdp.cc1;
    struct TextureHashmapNode *n0 = rendering_state.textures[0], *n1 = rendering_state.textures[1];
    uint8_t krgb[3] = { 0, 0, 0 }, ka = 0;
    int rgb_mode, a_mode, krgb_src = 0, ka_src = 0, i, pick = -1;
    uint32_t w0, h0, w1, h1, W, H, key[12];
    int iw1, ih1;

    if (n0 == NULL || n1 == NULL || n0->mirror != 0 || n1->mirror != 0 || !gfx_tile1_importable()) {
        if (gfx_debug_frame) port_log("  blend skip n0 %p n1 %p tmem %u/%u\n", (void *) n0, (void *) n1, rdp.texture_tile.tmem_slot, rdp.tiles[1].tmem);
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
    w1 = gfx_mask_period((uint32_t) (iw1 < 1 ? 1 : iw1), rdp.tiles[1].masks);
    h1 = gfx_mask_period((uint32_t) (ih1 < 1 ? 1 : ih1), rdp.tiles[1].maskt);
    W = w0;
    H = h0;
    // Tile 1 repeats with its own period in tile-0 texels (its shift scales
    // it): cover both periods so the pre-blend tiles seamlessly.
    if (gfx_is_pow2(w0) && gfx_is_pow2(w1) && !(rdp.texture_tile.cms & G_TX_CLAMP)) {
        uint32_t sh = rdp.tiles[1].shifts;
        uint32_t p = sh == 0 ? w1 : sh <= 10 ? w1 << sh : (w1 >> (16 - sh));
        if (p > W) W = p;
    }
    if (gfx_is_pow2(h0) && gfx_is_pow2(h1) && !(rdp.texture_tile.cmt & G_TX_CLAMP)) {
        uint32_t sh = rdp.tiles[1].shiftt;
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
    key[7] = rdp.tiles[1].shifts | (rdp.tiles[1].shiftt << 8) | (rgb_mode << 16) | (a_mode << 20) | (krgb_src << 24) | (ka_src << 28);
    key[8] = krgb[0] | (krgb[1] << 8) | (krgb[2] << 16) | ((uint32_t) ka << 24);
    key[9] = n0->texture_id;
    key[10] = n1->texture_id;
    key[11] = 0x424c4e44;

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
                                 rdp.tiles[1].ult >> 2, rdp.tiles[1].shifts, rdp.tiles[1].shiftt, rgb_mode, a_mode,
                                 krgb_src, krgb, ka_src, ka)) {
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

/* X31 (ocarina-of-time 6f1ac0e): the light and lookat directions reach model space through the modelview, so for a
 * model drawn at a small scale they come out ~0.003 long; the old 1e-5 floor skipped normalizing them and n.l
 * went to ~0 -- no directional light, no env-map shine, one flat colour. The RSP normalizes at any length.
 * SF64_NORMFIX=0: the old floor. */
static void gfx_normalize_vector(float v[3]) {
    static int fix SAVESTATE_EXCLUDE = -1;
    if (fix < 0) { const char *e = getenv("SF64_NORMFIX"); fix = !(e && e[0] == '0'); }
    float dot = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (dot > (fix ? 1e-30f : 0.00001f)) {
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

/* The aspect ratio 3D is fitted to, when it differs from the frame's: the
 * handheld's default screen fit renders the N64's 320x240 frame (so 2D and text
 * keep every column) and squeezes it to the 240 panel at present; 3D is drawn
 * pre-stretched for a 1:1 panel so it comes out at true aspect. 0 = frame. */
float gfx_persp_aspect SAVESTATE_EXCLUDE;
static float gfx_adjust_x_for_aspect_ratio(float x) {
    const float ar = gfx_persp_aspect > 0.0f ? gfx_persp_aspect
                                             : (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
    return x * (4.0f / 3.0f) / ar;
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
#if !defined(TARGET_PSP) && !defined(PORT_GE_CULL) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
    /* Device: both matrices' rows held for the batch; per vertex and column
     * ((v0*M0 + v1*M1) + v2*M2) + v3*M3, each product rounded before its add
     * (NEON VMLA is not fused) -- the scalar loop's order and rounding, 8
     * instructions instead of ~80 (ocarina-of-time c935b73). */
    const float (*const nMP)[4] = rsp.MP_matrix;
    const float (*const nMV)[4] = rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1];
    const float32x4_t mp0 = vld1q_f32(nMP[0]), mp1 = vld1q_f32(nMP[1]), mp2 = vld1q_f32(nMP[2]), mp3 = vld1q_f32(nMP[3]);
    const float32x4_t mv0 = vld1q_f32(nMV[0]), mv1 = vld1q_f32(nMV[1]), mv2 = vld1q_f32(nMV[2]), mv3 = vld1q_f32(nMV[3]);
#endif
    /* gfx_adjust_x_for_aspect_ratio's quotient, once a batch instead of every
     * vertex (same operands: the same float). Between the HUD tags the game's
     * perspective HUD pieces (gold-ring slots, team icons) keep the frame's
     * 4:3 like the HUD's 2D, so the squeeze at present treats them alike. */
    /* Between the tags, the frame's 4:3 exactly (round 12): width/height of the CURRENT target was the
     * internal square size whenever the 3D phase was on (after any perspective triangle), so a tagged piece
     * loaded then was pre-stretched after all -- the planet select's and General Pepper's side windows
     * ran off the panel (the user's report), and the HUD's own perspective pieces depended on what drew
     * before them. SF64_HUDASPECT=0: as before. */
    static int sHudAspect SAVESTATE_EXCLUDE = -1;
    if (sHudAspect < 0) { const char *e = getenv("SF64_HUDASPECT"); sHudAspect = !(e && e[0] == '0'); }
    const float aspect_q = (gfx_hud_anchor && sHudAspect) ? (4.0f / 3.0f)
                           : (gfx_persp_aspect > 0.0f && !gfx_hud_anchor)
                               ? gfx_persp_aspect
                               : (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
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
        const float x = rsp.is_persp ? proj_vec[0] * (4.0f / 3.0f) / aspect_q : proj_vec[0];
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
                /* Texgen axes: the task's lookat (the microcode's DMEM default, or
                 * gSPLookAt), s from LOOKATX and t from LOOKATY -- what the console's
                 * own F3D microcode does (goldeneye-007 c68da63, checked there
                 * against m64ref's z64 LLE reference; the lead's R22). Star Fox never
                 * sets a lookat, so the defaults decide. SF64_LOOKAT=0: the kit's
                 * fixed eye-space axes, s from x; 2: s from LOOKATY. */
                static int sLookatMode SAVESTATE_EXCLUDE = -1;
                if (sLookatMode < 0) { const char *e = getenv("SF64_LOOKAT"); sLookatMode = e ? atoi(e) : 1; }
                const Light_t *ls = &lookat_x, *lt = &lookat_y;
                if (sLookatMode == 1) { ls = &rsp.current_lookat[1]; lt = &rsp.current_lookat[0]; }
                if (sLookatMode == 2) { ls = &rsp.current_lookat[0]; lt = &rsp.current_lookat[1]; }
                /* A zero vector gives n.l = 0 on every vertex, as on the RSP: the texture's
                 * centre (ocarina-of-time 5c60b2c; normalising zero would give NaNs). */
                if (ls->dir[0] | ls->dir[1] | ls->dir[2]) calculate_normal_dir(ls, rsp.current_lookat_coeffs[0]);
                else memset(rsp.current_lookat_coeffs[0], 0, sizeof(rsp.current_lookat_coeffs[0]));
                if (lt->dir[0] | lt->dir[1] | lt->dir[2]) calculate_normal_dir(lt, rsp.current_lookat_coeffs[1]);
                else memset(rsp.current_lookat_coeffs[1], 0, sizeof(rsp.current_lookat_coeffs[1]));
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
                
                static int sTexgenLin SAVESTATE_EXCLUDE = -1;
                if (sTexgenLin < 0) { const char *e = getenv("SF64_TEXGENLIN"); sTexgenLin = !(e && e[0] == '0'); }
                if ((rsp.geometry_mode & G_TEXTURE_GEN_LINEAR) && sTexgenLin) {
                    /* G_TEXTURE_GEN_LINEAR: the microcode maps the angle, not the cosine, so a
                     * flat face sweeps the texture evenly (GLideN64: acos(n.l) * 1024 / pi before
                     * the texture scale): s = acos(n.l) / pi * scale / 2 in S10.5, the same range
                     * as the plain mapping (ocarina-of-time 5c60b2c, the lead's R30; Star Fox's gold
                     * and silver rings and stars). SF64_TEXGENLIN=0: the plain mapping (A/B).
                     * port_acosf: the same bits on the device and the desktop (round 12). */
                    const float cx = dotx / 127.0f < -1.0f ? -1.0f : dotx / 127.0f > 1.0f ? 1.0f : dotx / 127.0f;
                    const float cy = doty / 127.0f < -1.0f ? -1.0f : doty / 127.0f > 1.0f ? 1.0f : doty / 127.0f;
                    U = (int32_t)(port_acosf(cx) * (1.0f / (float)M_PI) * 0.5f * rsp.texture_scaling_factor.s);
                    V = (int32_t)(port_acosf(cy) * (1.0f / (float)M_PI) * 0.5f * rsp.texture_scaling_factor.t);
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
            { /* the RSP's guard band in its own clip space (the clipper's planes act on _x, _y) */
                const float gb = rsp.clip_ratio * aw;
                if (proj_vec[0] < -gb || proj_vec[0] > gb || proj_vec[1] < -gb || proj_vec[1] > gb) rej |= GB_OUT;
            }
            // In front of the game's near plane (z_ndc < -1), or behind the eye:
            // a triangle with all three vertices here is invisible (the RSP
            // clips it away), so gfx_sp_tri1's trivial reject may drop it. This
            // was w < W_NEAR (24) -- a clip hint, but the shared-bit reject
            // treated it as "outside", deleting every triangle within 24 units
            // of the camera (MK64 device report after v29).
            if (z < -w) rej |= Z_POS;
            // Beyond the far plane (the RSP clips there). Was z > 2|w|, which a
            // standard perspective never reaches (z/w < (f+n)/(f-n) ~ 1.002 here),
            // so the far reject never fired (super-mario-64 5cd6699, B3). z > w is
            // only ever true in front of the eye: behind it z < w < 0.
            if (z > w) rej |= Z_NEG;
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

static void gfx_eval_vertex_color(uint32_t cc_id, const struct RGBA *shade, int lod, struct RGBA *out) {
    uint8_t *o = (uint8_t *) out;
    int ch;
#if !defined(TARGET_PSP)
    // A decal of the texture over an input by the texel's own alpha,
    // (TEXEL0 - X) * TEXEL0_ALPHA + X (G_CC_BLENDRGBA). With texels as 1.0 it
    // folds to white; the software backend does the per-pixel lerp itself
    // (gfx_soft rgb_decal) and needs X.
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
    if (tri_state.cc2) {
        gfx_eval_cycle2(shade, lod, out);
    }
}

/* The same evaluation compiled per triangle state (gfx_tri_rebuild_state):
 * each combiner input of both cycles becomes an index into one byte table --
 * shade, LOD and the cycle-1 result filled per vertex, the rest constant until
 * the next state command -- so a vertex costs four (eight with cycle 2)
 * table-driven (a - b) * c / 255 + d instead of the switch walk above
 * (ocarina-of-time c935b73; mirrors this file's evaluator, including the decal
 * rule). Same inputs, same integer arithmetic: bit-identical.
 * SF64_VCPROG=0 evaluates with gfx_eval_vertex_color, =2 runs both and logs
 * any difference. */
enum { VC_SHADE = 0, VC_LOD = 4, VC_COMB = 5, VC_ZERO = 9, VC_ONE = 10, VC_HALF = 11,
       VC_PRIM = 12, VC_ENV = 16, VC_PLF = 20, VC_N = 24 };
static struct {
    uint8_t src[VC_N];
    uint8_t c1[4][4], c2[4][4]; /* per output channel: the a b c d indices */
    bool a255, cc2;
} sVc SAVESTATE_EXCLUDE;
static int sVcMode SAVESTATE_EXCLUDE = -1;

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
#if !defined(TARGET_PSP)
    /* gfx_eval_vertex_color's decal rule: rgb = d */
    const uint32_t xb = (cc_id >> 3) & 7;
    const bool decal = (cc_id & 7) == CC_TEXEL0 && ((cc_id >> 6) & 7) == CC_TEXEL0A && xb == ((cc_id >> 9) & 7) &&
                       (xb == CC_PRIM || xb == CC_SHADE || xb == CC_ENV || xb == CC_LOD);
#else
    const bool decal = false;
#endif
    for (ch = 0; ch < 4; ch++) {
        const int k = (ch == 3) ? 1 : 0;
        const uint32_t bits = cc_id >> (k * 12);
        sVc.c1[ch][0] = vc_slot1(bits & 7, (cc_id >> (28 + k)) & 1, ch);
        sVc.c1[ch][1] = vc_slot1((bits >> 3) & 7, 0, ch);
        sVc.c1[ch][2] = vc_slot1((bits >> 6) & 7, 0, ch);
        sVc.c1[ch][3] = vc_slot1((bits >> 9) & 7, (cc_id >> (30 + k)) & 1, ch);
        if (decal && ch < 3) { sVc.c1[ch][0] = sVc.c1[ch][1] = sVc.c1[ch][2] = VC_ZERO; } /* (0 - 0) * 0 / 255 + d */
    }
    sVc.a255 = (cc_id & 0xfff000) == 0 && !(cc_id & (CC_FLAG_ALPHA_A_ONE | CC_FLAG_ALPHA_D_ONE));
    sVc.cc2 = tri_state.cc2;
    for (ch = 0; ch < 3; ch++) {
        for (s = 0; s < 4; s++) sVc.c2[ch][s] = vc_rgb2(rdp.cc2[s], s, ch);
    }
    for (s = 0; s < 4; s++) sVc.c2[3][s] = vc_a2(rdp.cc2[4 + s], s);
}
static inline uint8_t vc_abcd(const uint8_t *src, const uint8_t *p) {
    const int v = ((src[p[0]] - src[p[1]]) * src[p[2]]) / 255 + src[p[3]];
    return (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
}
static void gfx_vc_eval(const struct RGBA *shade, int lod, struct RGBA *out) {
    uint8_t *const src = sVc.src, *const o = (uint8_t *) out;
    memcpy(&src[VC_SHADE], shade, 4);
    src[VC_LOD] = (uint8_t) lod; /* 0..255 */
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



/* Fog: ONE reference for every port (mario-kart-64 7d23507: SM64 de0b5af's rules on MK64 c400d1b's
 * fast paths), taken in round 11 for the pop-in. The RDP blends the fog colour in after the combiner,
 * lerp(combined, fog, shade alpha); the fold (gfx_emit_vertex: the vertex colour lerped toward the
 * fog before the texture multiply) is exact only for black fog on texture x input or an untextured
 * combiner. Under Corneria's navy fog (0x192338) a far hill came out a texel-dark copy of itself
 * where the N64 fades it in from the fog -- the reported pop-in (z64 at the same instant, BUILDLOG
 * round 11). Everywhere the fold is inexact the vertex alpha carries the fog factor and gfx_soft
 * fogs each pixel: coloured fog, and decals under any fog (their combiner never reads the vertex
 * colour, so the fold dropped the fog). That needs the vertex alpha free for the factor: an opaque
 * draw (the alpha is unused), an alpha from the texel alone, from the shade alone (on the N64 the
 * shade alpha IS the factor) or a constant, which goes to the backend on its own (fog_a). Texel x
 * constant or shade x constant alphas keep the fold. Star Fox's own guard: a second cycle folds
 * into the vertex colour too (tri_state.cc2), so its alpha is not cycle 1's -- SETUPDL_45's
 * translucent MODULATEIA_PRIM2 would lose the PRIM alpha -- and a blended 2-cycle draw keeps the
 * fold. SF64_FOGPX=0: always fold; 1: c400d1b's rule; 2 (default): the reference. */
static int gfx_fogpx_mode SAVESTATE_EXCLUDE = -1;
/* SF64_CC_CENSUS (ocarina-of-time round 47 #3, OOT_FOGTEXA): fogged blended draws whose alpha is a texel x a
 * constant under NON-black fog, which keep the vertex-colour fold ([0] the one fog rule, [1] Star Fox's
 * 2-cycle guard) -- exact only for black fog. */
static uint32_t gfx_census_fogtexa[2] SAVESTATE_EXCLUDE;
/* SF64_FOGTEXA=1: a texel x constant alpha fogged per pixel (round 17). OFF by default: +0.20% qemu
 * instructions in Corneria (frames 2800-3300) for a two-row line near the horizon that can't be seen on a
 * square output -- the owner's rule keeps such a fix off. */
static bool gfx_fogtexa_on(void) {
    static int sOn SAVESTATE_EXCLUDE = -1;
    if (sOn < 0) { const char *e = getenv("SF64_FOGTEXA"); sOn = e && e[0] == '1'; }
    return sOn != 0;
}
/* The alpha combiner is exactly TEXEL0 x PRIM or ENV (either order): (TEXEL0 - 0) * K + 0. */
static bool fog_tex_x_const_alpha(uint32_t cc_id) {
    const uint32_t a = (cc_id >> 12) & 7, b = (cc_id >> 15) & 7, c = (cc_id >> 18) & 7, d = (cc_id >> 21) & 7;
    const bool k_a = a == CC_PRIM || a == CC_ENV, k_c = c == CC_PRIM || c == CC_ENV;
    return b == CC_0 && d == CC_0 && !(cc_id & (CC_FLAG_ALPHA_A_ONE | CC_FLAG_ALPHA_D_ONE)) &&
           ((a == CC_TEXEL0 && k_c) || (c == CC_TEXEL0 && k_a));
}
static void gfx_tri_fog(uint32_t cc_id, bool use_alpha) {
    bool fog_px = false;
    int fog_a = -1; // >= 0: the combiner's constant alpha, displaced from the vertex by the fog factor
    uint8_t path = 0;
    if (gfx_fogpx_mode < 0) {
        const char *e = getenv("SF64_FOGPX");
        gfx_fogpx_mode = (e && e[0] >= '0' && e[0] <= '2') ? e[0] - '0' : 2;
    }
    tri_state.fog_raw_alpha = false;
    if (tri_state.use_fog) {
        const bool black = (rdp.fog_color.r | rdp.fog_color.g | rdp.fog_color.b) == 0;
        bool a_shade = false, a_tex = false, a_other = false; // the alpha combiner's inputs (cleared when opaque)
        for (int k = 0; k < 4; k++) {
            const uint32_t code = (cc_id >> (12 + 3 * k)) & 7;
            if (code == CC_SHADE) a_shade = true;
            else if (code == CC_TEXEL0 || code == CC_TEXEL1 || code == CC_TEXEL0A) a_tex = true;
            else if (code != CC_0) a_other = true;
        }
        path = 5; /* the fold: exact (1) or kept inexact (5), below */
        if (!black && use_alpha && !a_shade && a_tex && a_other) gfx_census_fogtexa[tri_state.cc2 ? 1 : 0]++;
        if (gfx_fogpx_mode == 0) {
            path = 6;
        } else if (tri_state.cc2 && use_alpha) {
            /* Star Fox's guard (above): keep the fold */
        } else if (gfx_fogpx_mode == 1) {
            fog_px = !black && !a_shade && !a_other && (!use_alpha || a_tex);
        } else {
            const uint32_t ra = cc_id & 7, rb = (cc_id >> 3) & 7, rc = (cc_id >> 6) & 7, rd = (cc_id >> 9) & 7;
            const bool in_a = ra == CC_PRIM || ra == CC_SHADE || ra == CC_ENV;
            const bool in_c = rc == CC_PRIM || rc == CC_SHADE || rc == CC_ENV;
            const bool tex_x_input = rb == CC_0 && rd == CC_0 && !(cc_id & (CC_FLAG_RGB_A_ONE | CC_FLAG_RGB_D_ONE)) &&
                                     ((ra == CC_TEXEL0 && in_c) || (rc == CC_TEXEL0 && in_a));
            if (black && (!tri_state.use_texture || tex_x_input)) {
                path = 1;
            } else if (!use_alpha) {
                fog_px = true;
            } else if (!a_shade && a_tex && a_other && gfx_fogtexa_on() && fog_tex_x_const_alpha(cc_id)) {
                /* ocarina-of-time c7ebfea (OOT_FOGTEXA): a texel x constant alpha (TEXEL0 * PRIM) kept the fold,
                 * which fogs right only under black fog: under Corneria's navy fog a blended decal stays
                 * dark where the RDP lerps it into the haze. Per pixel now: the constant goes on its own
                 * (fog_a, the texel taken as 1) and the rasterizer multiplies the texel alpha back in. */
                fog_px = true;
                tri_state.fog_raw_alpha = true;
                struct RGBA dummy = { 0, 0, 0, 0 }, o;
                gfx_eval_vertex_color(cc_id, &dummy, 0, &o);
                fog_a = o.a;
            } else if (a_shade ? !a_other : !(a_tex && a_other)) {
                fog_px = true;
                tri_state.fog_raw_alpha = !a_shade; // a shade alpha: the combined vertex alpha, as SM64
                if (!a_shade && !a_tex) { // constant: PRIM/ENV alpha, or 0/1
                    struct RGBA dummy = { 0, 0, 0, 0 }, o;
                    gfx_eval_vertex_color(cc_id, &dummy, 0, &o);
                    fog_a = o.a;
                }
            }
        }
        if (fog_px) path = !use_alpha ? 3 : fog_a >= 0 ? 4 : tri_state.fog_raw_alpha ? 3 : 2;
    }
    tri_state.fog_px = fog_px;
    if (fog_px && (gfx_fogpx_mode == 1 || !use_alpha)) tri_state.fog_raw_alpha = true;
    tri_state.fog_path = path;
    if (fog_px != rendering_state.fog_px || (fog_px && (rdp.fog_color.r != rendering_state.fog_c.r ||
                                                       rdp.fog_color.g != rendering_state.fog_c.g ||
                                                       rdp.fog_color.b != rendering_state.fog_c.b ||
                                                       fog_a != rendering_state.fog_a))) {
        extern void gfx_soft_set_fog(bool on, int r, int g, int b, int a);
        gfx_flush();
        gfx_soft_set_fog(fog_px, rdp.fog_color.r, rdp.fog_color.g, rdp.fog_color.b, fog_a);
        rendering_state.fog_px = fog_px;
        rendering_state.fog_c = rdp.fog_color;
        rendering_state.fog_a = fog_a;
    }
}

uint32_t gfx_prof_rebuilds;

/* SF64_CC_CENSUS=1 (test only): logs the first draw with each combiner whose rgb
 * multiplier (C) is an input the cc_id has no code for -- ENV_ALPHA,
 * PRIMITIVE_ALPHA, PRIM_LOD_FRAC and the other alpha inputs, which cycle 1 folds
 * to 0 -- and at exit how often each was seen (triangle state rebuilds, rects).
 * "tex" marks a texel in A, B or D: (ENV - TEXEL0) * ENV_ALPHA + TEXEL0 is DKR's
 * menu highlight, which drew as the plain texel until diddy-kong-racing ac7de18
 * (cross-port lead, round 7). Cycle 2 of a 2-cycle mode is logged when it reads
 * a texel in A, B or D with any C. SF64_CC_CENSUS=2 logs every form (a check
 * that the census sees the draws). Either also counts the G_LOADBLOCKs with
 * dxt 0, whose odd rows reach TMEM unswapped (diddy-kong-racing be3afca, a85c65c):
 * Star Fox's data and code have none (round 7). */
#define CC_CENSUS_MAX 128
static int gfx_cc_census_on SAVESTATE_EXCLUDE = -1;
static int gfx_cc_census_n SAVESTATE_EXCLUDE;
static uint32_t gfx_census_blocks SAVESTATE_EXCLUDE, gfx_census_dxt0 SAVESTATE_EXCLUDE;
static uint32_t gfx_census_sc SAVESTATE_EXCLUDE, gfx_census_sc_n64 SAVESTATE_EXCLUDE, gfx_census_sc_target SAVESTATE_EXCLUDE;
/* The lead's texture rules from paper-mario aa512fd and f-zero-x cf242ea: draws whose
 * second cycle reads a texel (R25), draws whose render-tile corners came from a LoadTile
 * rectangle other than the last SetTileSize(0) (R26), draws whose tile starts inside a
 * load at a non-zero TMEM offset (R27), wrapping LoadTile tiles smaller than their mask
 * period (R24 for LoadTile), inverted scissors (R28). */
static uint32_t gfx_census_shift SAVESTATE_EXCLUDE; /* R29: textured draws whose render tile shifts S or T */
static uint32_t gfx_census_zimgrect SAVESTATE_EXCLUDE; /* R34: texrects into the Z image */
static uint32_t gfx_census_lbwide SAVESTATE_EXCLUDE; /* X18 */
static uint32_t gfx_census_edgeop SAVESTATE_EXCLUDE; /* X12: triangle state rebuilds with TEX_EDGE and no FORCE_BL */
static uint32_t gfx_census_texrect1, gfx_census_texrectfrac SAVESTATE_EXCLUDE; /* R11: 1/2-cycle texrects, off whole N64 pixels */
static uint32_t gfx_census_c2tex SAVESTATE_EXCLUDE, gfx_census_ltcorner SAVESTATE_EXCLUDE,
    gfx_census_tmemoff SAVESTATE_EXCLUDE, gfx_census_ltwrap SAVESTATE_EXCLUDE, gfx_census_scinv SAVESTATE_EXCLUDE;
static uint16_t gfx_census_load_tmem[2] SAVESTATE_EXCLUDE;
static uint16_t gfx_census_sts0[4] SAVESTATE_EXCLUDE;
/* The pop-in (round 11): triangles drawn with the fog blender (CLR_FOG * A_SHADE). The fold in
 * gfx_emit_vertex lerps the vertex colour to the fog colour before the texture multiply, so a
 * textured triangle at fog factor a comes out texel * lerp(shade, fog, a) where the N64 draws
 * lerp(texel * shade, fog, a): the same for black fog, darker by fog * a * (1 - texel) for any
 * other, and a fully fogged object is a texel-dark shape instead of the fog colour.
 * [0] fog-blender triangles, [1] with a fog colour other than black, [2] of those textured,
 * [3] textured with a vertex at fog >= 128, [4] textured with all three at 255, [5] G_FOG on. */
static uint32_t gfx_census_fog[6] SAVESTATE_EXCLUDE;
/* ...and which rule each took (tri_state.fog_path, gfx_tri_fog): the fold exact, per pixel with the shade
 * alpha, with the raw factor (opaque or texel alpha), with a constant alpha, the fold kept (inexact), the
 * fold by SF64_FOGPX=0. */
static uint32_t gfx_census_fogpath[7] SAVESTATE_EXCLUDE;
/* ...and per combiner (cc_id, cycle 2 folded in, blended, rule): the lead's two classes -- non-black
 * fog, and decals under any fog (an rgb combiner that never reads the vertex colour) -- and what keeps
 * the fold. */
static struct { uint32_t cc_id, tris; uint8_t cc2, blend, path, decal; } gfx_census_fogcc[24] SAVESTATE_EXCLUDE;
static int gfx_census_fogcc_n SAVESTATE_EXCLUDE;
static uint32_t gfx_census_fogdecal SAVESTATE_EXCLUDE;
static uint32_t gfx_census_fogrgb[12] SAVESTATE_EXCLUDE;
static int gfx_census_fogrgb_n SAVESTATE_EXCLUDE;
static bool gfx_census_ltflag SAVESTATE_EXCLUDE;
static struct {
    uint8_t m[9]; /* cycle-1 rgb a b c d, cycle-2 rgb a b c d (0xFF: 1-cycle), 0 tri / 1 rect */
    uint32_t draws;
} gfx_cc_census_tab[CC_CENSUS_MAX] SAVESTATE_EXCLUDE;

static bool cc_census_texel(const uint8_t *m) {
    return m[0] == G_CCMUX_TEXEL0 || m[0] == G_CCMUX_TEXEL1 || m[1] == G_CCMUX_TEXEL0 || m[1] == G_CCMUX_TEXEL1 ||
           m[3] == G_CCMUX_TEXEL0 || m[3] == G_CCMUX_TEXEL1;
}

static void gfx_cc_census_report(void) {
    int i;
    for (i = 0; i < gfx_cc_census_n; i++) {
        const uint8_t *m = gfx_cc_census_tab[i].m;
        port_log("cc census: %s c1 [%u %u %u %u]%s c2 [%u %u %u %u] seen %u\n", m[8] ? "rect" : "tri", m[0], m[1],
                 m[2], m[3], cc_census_texel(m) ? " tex" : "", m[4], m[5], m[6], m[7], gfx_cc_census_tab[i].draws);
    }
    port_log("cc census: %d forms\n", gfx_cc_census_n);
    port_log("cc census: %u LoadBlocks, %u with dxt 0\n", gfx_census_blocks, gfx_census_dxt0);
    port_log("cc census: %u texture lookups split by the tile view\n", gfx_census_splits);
    port_log("cc census: %u scissors, %u off whole N64 pixels, %u off whole target pixels, %u inverted\n",
             gfx_census_sc, gfx_census_sc_n64, gfx_census_sc_target, gfx_census_scinv);
    port_log("cc census: draws with a texel in cycle 2 %u, with LoadTile corners %u, starting inside a load %u, "
             "wrapping LoadTile tiles under their period %u, with a render-tile shift %u\n", gfx_census_c2tex,
             gfx_census_ltcorner, gfx_census_tmemoff, gfx_census_ltwrap, gfx_census_shift);
    port_log("cc census: texrects into the Z image (R34) %u\n", gfx_census_zimgrect);
    port_log("cc census: 1/2-cycle texrects %u, off whole N64 pixels (R11) %u\n", gfx_census_texrect1, gfx_census_texrectfrac);
    port_log("cc census: mirrored clamped tiles the mask-period rule misreads (pm64 fix 1) %u\n", gfx_census_mirmask);
    port_log("cc census: TEX_EDGE triangles without FORCE_BL (X12) %u state rebuilds\n", gfx_census_edgeop);
    port_log("cc census: wrapping LoadBlock tiles with an S period wider than the tile line (X18) %u\n", gfx_census_lbwide);
    port_log("cc census: fogged texel x constant alpha under coloured fog (OOT_FOGTEXA) %u (2-cycle guard %u)\n", gfx_census_fogtexa[0], gfx_census_fogtexa[1]);
    port_log("cc census: texture cache: mid-frame resets %u, two-tile draws %u, resets under a draw's imports %u, tile-1 hits setting tile 0's clamp %u\n",
             gfx_census_midreset, gfx_census_t1draw, gfx_census_underreset, gfx_census_t1samp);
    port_log("cc census: fog-blender triangles %u (G_FOG on %u), fog colour not black %u, of those textured %u, "
             "textured with a vertex at fog >= 128 %u, textured and fully fogged %u\n", gfx_census_fog[0],
             gfx_census_fog[5], gfx_census_fog[1], gfx_census_fog[2], gfx_census_fog[3], gfx_census_fog[4]);
    for (int i = 0; i < gfx_census_fogcc_n; i++) {
        port_log("cc census: fog combiner %08X cc2 %u blended %u decal %u rule %u: %u triangles\n",
                 (unsigned) gfx_census_fogcc[i].cc_id, gfx_census_fogcc[i].cc2, gfx_census_fogcc[i].blend,
                 gfx_census_fogcc[i].decal, gfx_census_fogcc[i].path, gfx_census_fogcc[i].tris);
    }
    port_log("cc census: fogged decal triangles (any fog colour) %u\n", gfx_census_fogdecal);
    port_log("cc census: fog rules: fold exact %u, per pixel (shade alpha) %u, per pixel (raw factor) %u, per pixel "
             "(raw, constant alpha) %u, fold kept (inexact) %u, fold (SF64_FOGPX=0) %u\n", gfx_census_fogpath[1],
             gfx_census_fogpath[2], gfx_census_fogpath[3], gfx_census_fogpath[4], gfx_census_fogpath[5], gfx_census_fogpath[6]);
}

static void gfx_census_log(uint32_t *n, const char *what, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if ((*n)++ < 6) {
        port_log("cc census: frame %u %s %u %u %u %u\n", gfx_frame_counter, what, a, b, c, d);
    }
}

/* Per fog-blender triangle: the counts above (gfx_census_fog). */
static void gfx_census_fogtri(const struct LoadedVertex *a, const struct LoadedVertex *b, const struct LoadedVertex *c) {
    uint32_t rgb = (uint32_t) rdp.fog_color.r << 16 | rdp.fog_color.g << 8 | rdp.fog_color.b;
    int hi = a->color.a > b->color.a ? a->color.a : b->color.a, i;
    if (c->color.a > hi) hi = c->color.a;
    gfx_census_fog[0]++;
    gfx_census_fogpath[tri_state.fog_path < 7 ? tri_state.fog_path : 0]++;
    {
        const uint32_t id = tri_state.cc_id;
        const uint8_t blend = rendering_state.alpha_blend, path = tri_state.fog_path, cc2 = tri_state.cc2;
        bool reads_in = false;
        for (int k = 0; k < 4; k++) {
            const uint32_t code = (id >> (3 * k)) & 7;
            reads_in |= code == CC_PRIM || code == CC_SHADE || code == CC_ENV || code == CC_LOD;
        }
        const uint8_t decal = tri_state.use_texture && !reads_in && !cc2;
        if (decal) gfx_census_fogdecal++;
        for (i = 0; i < gfx_census_fogcc_n; i++) {
            if (gfx_census_fogcc[i].cc_id == id && gfx_census_fogcc[i].cc2 == cc2 && gfx_census_fogcc[i].blend == blend &&
                gfx_census_fogcc[i].path == path) break;
        }
        if (i < gfx_census_fogcc_n) gfx_census_fogcc[i].tris++;
        else if (i < 24) {
            gfx_census_fogcc[i].cc_id = id; gfx_census_fogcc[i].cc2 = cc2; gfx_census_fogcc[i].blend = blend;
            gfx_census_fogcc[i].path = path; gfx_census_fogcc[i].decal = decal; gfx_census_fogcc[i].tris = 1;
            gfx_census_fogcc_n++;
        }
    }
    if (rsp.geometry_mode & G_FOG) gfx_census_fog[5]++;
    if (rgb == 0) return;
    gfx_census_fog[1]++;
    for (i = 0; i < gfx_census_fogrgb_n && gfx_census_fogrgb[i] != rgb; i++) {}
    if (i == gfx_census_fogrgb_n && i < 12) {
        gfx_census_fogrgb[gfx_census_fogrgb_n++] = rgb;
        port_log("cc census: frame %u new fog colour %06X (fog mul %d offset %d, G_FOG %d)\n", gfx_frame_counter,
                 (unsigned) rgb, rsp.fog_mul, rsp.fog_offset, (rsp.geometry_mode & G_FOG) != 0);
    }
    if (!tri_state.use_texture) return;
    gfx_census_fog[2]++;
    if (hi >= 128) gfx_census_fog[3]++;
    if (a->color.a == 255 && b->color.a == 255 && c->color.a == 255) gfx_census_fog[4]++;
}

/* Per draw state (triangle rebuild or rectangle): the texture rules above. */
static void gfx_census_tex(bool two_cycle) {
    const uint8_t *m = rdp.cc2;
    int slot = rdp.texture_tile.tmem_slot;
    if (two_cycle && !rdp.cc2_pass) {
        bool t = false;
        int i;
        for (i = 0; i < 4; i++) {
            if (m[i] == G_CCMUX_TEXEL0 || m[i] == G_CCMUX_TEXEL1) t = true;
            if (i == 2 && (m[i] == G_CCMUX_TEXEL0_ALPHA || m[i] == G_CCMUX_TEXEL1_ALPHA)) t = true;
            if (m[4 + i] == G_ACMUX_TEXEL0 || m[4 + i] == G_ACMUX_TEXEL1) t = true;
        }
        if (t) gfx_census_log(&gfx_census_c2tex, "cycle 2 reads a texel: c2", m[0] | m[1] << 8 | m[2] << 16 | m[3] << 24,
                              m[4] | m[5] << 8 | m[6] << 16 | m[7] << 24, rdp.cc1[0] | rdp.cc1[1] << 8 | rdp.cc1[2] << 16 | rdp.cc1[3] << 24, 0);
    }
    {
        const uint8_t *c = rdp.cc1;
        bool tex = false;
        int i;
        for (i = 0; i < 8; i++) {
            if (c[i] == G_CCMUX_TEXEL0 || c[i] == G_CCMUX_TEXEL1) tex = true; /* same codes for alpha (1, 2) */
            if (two_cycle && !rdp.cc2_pass && (m[i] == G_CCMUX_TEXEL0 || m[i] == G_CCMUX_TEXEL1)) tex = true;
        }
        if (c[2] == G_CCMUX_TEXEL0_ALPHA || c[2] == G_CCMUX_TEXEL1_ALPHA) tex = true;
        if (!tex) return; /* the tile rules below only matter to draws that read a texture */
    }
    if (rdp.tiles[G_TX_RENDERTILE].shifts || rdp.tiles[G_TX_RENDERTILE].shiftt) {
        gfx_census_log(&gfx_census_shift, "render tile shift s t (tmem, fmt)", rdp.tiles[G_TX_RENDERTILE].shifts,
                       rdp.tiles[G_TX_RENDERTILE].shiftt, rdp.tile_tmem[G_TX_RENDERTILE], rdp.texture_tile.fmt);
    }
    if (gfx_census_ltflag) {
        gfx_census_log(&gfx_census_ltcorner, "render tile has LoadTile corners (uls ult lrs lrt)", rdp.texture_tile.uls,
                       rdp.texture_tile.ult, rdp.texture_tile.lrs, rdp.texture_tile.lrt);
    }
    if (rdp.tile_tmem[G_TX_RENDERTILE] != gfx_census_load_tmem[slot]) {
        gfx_census_log(&gfx_census_tmemoff, "render tile tmem vs its load's tmem", rdp.tile_tmem[G_TX_RENDERTILE],
                       gfx_census_load_tmem[slot], (unsigned) slot, 0);
    }
    if (rdp.loaded_texture[slot].stride_bytes == 0 && rdp.texture_tile.masks && !(rdp.texture_tile.cms & G_TX_CLAMP)) {
        /* X18 (count only): a wrapping LoadBlock tile whose S period is wider than one tile line */
        static const uint32_t bits[4] = { 4, 8, 16, 32 };
        const uint32_t line = rdp.texture_tile.line_size_bytes * (rdp.texture_tile.siz == G_IM_SIZ_32b ? 2u : 1u);
        const uint32_t line_texels = line * 8u / bits[rdp.texture_tile.siz & 3];
        if (line_texels && (1u << rdp.texture_tile.masks) > line_texels)
            gfx_census_log(&gfx_census_lbwide, "wrapping LoadBlock tile, S period wider than its line (period line fmt siz)",
                           1u << rdp.texture_tile.masks, line_texels, rdp.texture_tile.fmt, rdp.texture_tile.siz);
    }
    if (rdp.loaded_texture[slot].stride_bytes != 0) {
        const int w = ((int) rdp.texture_tile.lrs - (int) rdp.texture_tile.uls + 4) / 4;
        const int h = ((int) rdp.texture_tile.lrt - (int) rdp.texture_tile.ult + 4) / 4;
        const uint8_t ms = rdp.texture_tile.masks, mt = rdp.texture_tile.maskt;
        if ((ms && ms < 16 && !(rdp.texture_tile.cms_raw & G_TX_CLAMP) && w < (1 << ms)) ||
            (mt && mt < 16 && !(rdp.texture_tile.cmt_raw & G_TX_CLAMP) && h < (1 << mt))) {
            gfx_census_log(&gfx_census_ltwrap, "wrapping LoadTile tile under its period (w h masks maskt)", (unsigned) w,
                           (unsigned) h, ms, mt);
        }
    }
}

static int gfx_census_mode(void) {
    if (gfx_cc_census_on < 0) {
        const char *e = getenv("SF64_CC_CENSUS");
        gfx_cc_census_on = e && (e[0] == '1' || e[0] == '2') ? e[0] - '0' : 0;
        gfx_census_views = gfx_cc_census_on > 0;
        if (gfx_cc_census_on) atexit(gfx_cc_census_report);
    }
    return gfx_cc_census_on;
}

static void gfx_cc_census(bool two_cycle, int rect) {
    uint8_t key[9];
    bool c1, c2;
    int i;
    if (!gfx_census_mode()) return;
    switch (rdp.cc1[2]) {
        case G_CCMUX_SCALE: case G_CCMUX_COMBINED_ALPHA: case G_CCMUX_TEXEL1_ALPHA: case G_CCMUX_PRIMITIVE_ALPHA:
        case G_CCMUX_SHADE_ALPHA: case G_CCMUX_ENV_ALPHA: case G_CCMUX_PRIM_LOD_FRAC: case G_CCMUX_K5:
            c1 = true;
            break;
        default:
            c1 = false;
    }
    c2 = two_cycle && !rdp.cc2_pass && rdp.cc2[2] < 16 && cc_census_texel(rdp.cc2);
    if (!c1 && !c2 && gfx_cc_census_on != 2) return;
    memcpy(key, rdp.cc1, 4);
    if (two_cycle && !rdp.cc2_pass) memcpy(key + 4, rdp.cc2, 4);
    else memset(key + 4, 0xFF, 4);
    key[8] = (uint8_t) rect;
    for (i = 0; i < gfx_cc_census_n; i++) {
        if (memcmp(gfx_cc_census_tab[i].m, key, 9) == 0) {
            gfx_cc_census_tab[i].draws++;
            return;
        }
    }
    if (gfx_cc_census_n == CC_CENSUS_MAX) return;
    memcpy(gfx_cc_census_tab[gfx_cc_census_n].m, key, 9);
    gfx_cc_census_tab[gfx_cc_census_n++].draws = 1;
    port_log("cc census: frame %u new %s c1 [%u %u %u %u]%s c2 [%u %u %u %u] prim %02X%02X%02X%02X env %02X%02X%02X%02X "
             "plf %u\n", gfx_frame_counter, rect ? "rect" : "tri", key[0], key[1], key[2], key[3],
             cc_census_texel(key) ? " tex" : "", key[4], key[5], key[6], key[7], rdp.prim_color.r, rdp.prim_color.g,
             rdp.prim_color.b, rdp.prim_color.a, rdp.env_color.r, rdp.env_color.g, rdp.env_color.b, rdp.env_color.a,
             rdp.prim_lod_frac);
}

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
    gfx_cc_census(two_cycle, 0);
    if (gfx_cc_census_on > 0) gfx_census_tex(two_cycle);
    bool use_alpha = two_cycle ? ((rdp.other_mode_l >> 16) & 3) == G_BL_1MA : ((rdp.other_mode_l >> 18) & 3) == G_BL_1MA;
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
    {   /* X12 (ocarina-of-time 7120396): a texture edge without FORCE_BL (G_RM_AA_ZB_TEX_EDGE) never blends on
         * the RDP -- coverage comes from the texel's alpha and the pixel is written as its own colour or
         * not at all -- so a cutout's filtered rim must not mix with what's behind. Triangles only; texture
         * rectangles keep blending. SF64_EDGEOPAQUE=0: as before. */
        static int sEdgeOp SAVESTATE_EXCLUDE = -1;
        extern void gfx_soft_set_edge_opaque(int mode);
        if (sEdgeOp < 0) { const char *e = getenv("SF64_EDGEOPAQUE"); sEdgeOp = (e && e[0] >= '0' && e[0] <= '2') ? e[0] - '0' : 1; }
        const int eo = (texture_edge && !(rdp.other_mode_l & FORCE_BL)) ? sEdgeOp : 0;
        if (gfx_census_mode() && texture_edge && !(rdp.other_mode_l & FORCE_BL)) gfx_census_edgeop++;
        if (eo != rendering_state.edge_opaque) {
            gfx_flush();
            gfx_soft_set_edge_opaque(eo);
            rendering_state.edge_opaque = eo;
        }
    }
    {   /* SF64_FILLRULE (gfx_soft.c own_edge; ocarina-of-time a7c4909): a blended or alpha-tested draw without AA_EN --
         * the RDP writes such a pixel only when one fixed sample of it is covered, so each pixel along an edge two
         * triangles share is drawn once (the inclusive row solve blended it twice: a bright one-pixel line along
         * the diagonal of a see-through quad). With AA_EN both triangles write the pixel they share (partial
         * coverage each): left as it was. SF64_FILLRULE=0 or fillrule=0 in the display cfg: every draw takes the
         * inclusive row solve, as before. */
        static int sFillRule SAVESTATE_EXCLUDE = -1;
        extern void gfx_soft_set_own_edges(bool on);
        if (sFillRule < 0) {
            const char *e = getenv("SF64_FILLRULE");
            sFillRule = !(e && e[0] == '0');
            if (!e) {
                const char *dir = getenv("SF64_DATA");
                char path[512], line[64];
                snprintf(path, sizeof(path), "%s/sf64_display.cfg", dir ? dir : ".");
                FILE *f = fopen(path, "r");
                if (f) {
                    while (fgets(line, sizeof(line), f))
                        if (strncmp(line, "fillrule=", 9) == 0 && (line[9] == '0' || line[9] == '1')) sFillRule = line[9] - '0';
                    fclose(f);
                }
            }
        }
        const bool own = sFillRule && use_alpha && !(rdp.other_mode_l & AA_EN);
        if (own != rendering_state.own_edges) {
            gfx_flush();
            gfx_soft_set_own_edges(own);
            rendering_state.own_edges = own;
        }
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
    if (uses_t1 || (used_textures[0] && used_textures[1])) gfx_census_t1draw++;
    for (int pass = 0; pass < 2; pass++) {
    const uint32_t epoch = gfx_cache_epoch; /* a reset under tile 1's import: tile 0 again (gfx_cache_epoch) */
    gfx_stress_hold = pass > 0;
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
    if (pass == 0 && epoch != gfx_cache_epoch) gfx_census_underreset++;
    if (epoch == gfx_cache_epoch || !gfx_epochfix_on()) break; /* no reset under the imports */
    }
    gfx_stress_hold = false;
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
        if (blend_id != 0) {
            tex_width = (float) blend_w;
            tex_height = (float) blend_h;
            tri_state.mirror = 0;
        }
        if (tri_state.mirror & 1) tex_width *= 2.0f;
        if (tri_state.mirror & 2) tex_height *= 2.0f;
        float filt = ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) ? 0.5f : 0.0f;
        float inv_w = 1.0f / tex_width, inv_h = 1.0f / tex_height;
        tri_state.u_scale = inv_w / 32.0f;
        tri_state.v_scale = inv_h / 32.0f;
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
        const char *e = getenv("SF64_VCPROG");
        sVcMode = e ? atoi(e) : 1;
    }
    if (tri_state.color_mode == 2 && sVcMode) {
        gfx_vc_compile(tri_state.cc_id);
    }
    gfx_tri_fog(cc_id, use_alpha);
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
        /* z/w is -1 (near) .. 1 (far), as the RSP's viewport maps it (vscale =
         * vtrans = G_MAXZ/2); the rasterizer's z-buffer is 0..1. The kit passed
         * z/w straight through: everything nearer than z/w = 0 (2*near*far/(far
         * + near), about 20 units in Star Fox) clamped to depth 0 and lost its
         * ordering (super-mario-64 5cd6699, B2). */
        out->z = cv->_z * inv * 0.5f + 0.5f;
#ifdef HH_CLIPCHECK
        { /* debug (super-mario-64 5cd6699): vertices reaching the rasterizer outside the depth range */
            static unsigned n_all, n_near, n_far; static int reg;
            if (!reg) { reg = 1; void hh_clipcheck_report(void); atexit(hh_clipcheck_report); }
            n_all++;
            if (rsp.is_persp && (w <= 0.0f || cv->_z < -w * 1.0001f)) n_near++;
            if (rsp.is_persp && w > 0.0f && cv->_z > w * 1.0001f) n_far++;
            extern unsigned hh_cc_all, hh_cc_near, hh_cc_far;
            hh_cc_all = n_all; hh_cc_near = n_near; hh_cc_far = n_far;
        }
#endif
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
        // Fogged per pixel by the backend (gfx_tri_fog): it reads the fog factor from the vertex alpha.
        if (tri_state.fog_raw_alpha) out->color.a = cv->color.a;
    } else if (tri_state.use_fog) {
        // Blender cycle 1 of the fog render modes: colour = lerp(colour, fog
        // colour, shade alpha) -- the RSP fog factor with G_FOG on, otherwise
        // the model's vertex alpha (Moo Moo Farm darkens its dirt that way).
        // Folded into the vertex colour before the texture multiply: exact
        // for an untextured combiner, and for black fog times the texel.
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
            float offu = (rdp.texture_tile.cms & G_TX_CLAMP) ? 0.0f : ((rdp.texture_tile.cms & G_TX_MIRROR) && !(tri_state.mirror & 1)) ? 2.0f * port_floorf(minu * 0.5f) : port_floorf(minu);
            float offv = (rdp.texture_tile.cmt & G_TX_CLAMP) ? 0.0f : ((rdp.texture_tile.cmt & G_TX_MIRROR) && !(tri_state.mirror & 2)) ? 2.0f * port_floorf(minv * 0.5f) : port_floorf(minv);
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

/* Triangle fate counters (SF64 bring-up diagnostic, logged every 150 frames
 * as TRIS): in = submitted, clipall = trivially outside the frustum, far =
 * beyond gfx_draw_dist_w, cull = back/front-face culled, pass = on to clipping
 * and the backend. */
uint32_t gfx_tri_in, gfx_tri_clipall, gfx_tri_far, gfx_tri_cull, gfx_tri_pass;

#if defined(TARGET_HANDHELD)
/* Round 25, the crosshair (the owner: "can the crosshair sprites be pixel perfect?"; option A): between
 * PORT_RETICLE_TAG_ON/OFF (fox_display.c Display_Reticle) the quad is not rasterized. Its on-screen box and
 * its texels, each through the current 1-cycle combiner, go to gfx_soft_reticle_lineart, which draws the
 * texture's line art on the 1:1 HUD layer: every opaque texel one panel pixel, forward-mapped into the box,
 * so the 1-px lines stay 1 px at the game's own sizes (the N64 smooths them; so did we). Only when the HUD
 * layer is on (CONVERT, hud=1), the texture is RGBA16 and at most 64x64, and the combiner uses nothing a
 * constant quad can't give; otherwise the quad draws as before. SF64_RETICLE=0 or reticle=0 in the display
 * cfg: as before. */
static int gfx_reticle_tag SAVESTATE_EXCLUDE, gfx_reticle_on SAVESTATE_EXCLUDE = -1;
static bool gfx_reticle_have SAVESTATE_EXCLUDE; /* a box captured */
static float gfx_reticle_bb[4] SAVESTATE_EXCLUDE;
static uint32_t gfx_reticle_px[64 * 64] SAVESTATE_EXCLUDE; /* the combined texels, ARGB */
static int gfx_reticle_tw SAVESTATE_EXCLUDE, gfx_reticle_th SAVESTATE_EXCLUDE;
static int gfx_reticle_enabled(void) {
    if (gfx_reticle_on < 0) {
        const char *e = getenv("SF64_RETICLE");
        gfx_reticle_on = !(e && e[0] == '0');
        if (!e) {
            const char *dir = getenv("SF64_DATA");
            char path[512], line[64];
            snprintf(path, sizeof(path), "%s/sf64_display.cfg", dir ? dir : ".");
            FILE *f = fopen(path, "r");
            if (f) {
                while (fgets(line, sizeof(line), f))
                    if (strncmp(line, "reticle=", 8) == 0 && (line[8] == '0' || line[8] == '1')) gfx_reticle_on = line[8] - '0';
                fclose(f);
            }
        }
    }
    extern int gfx_soft_hud_unsquish;
    return gfx_reticle_on && gfx_soft_hud_unsquish;
}
/* One combiner input for a constant quad: -1 when it isn't constant (texel 1, LOD, noise, ...). */
static int gfx_reticle_cc_rgb(int mux, int ch, const uint8_t *tex, const uint8_t *shade, int is_c) {
    const uint8_t *prim = (const uint8_t *) &rdp.prim_color, *env = (const uint8_t *) &rdp.env_color;
    switch (mux) {
        case G_CCMUX_TEXEL0: return tex[ch];
        case G_CCMUX_PRIMITIVE: return prim[ch];
        case G_CCMUX_SHADE: return shade[ch];
        case G_CCMUX_ENVIRONMENT: return env[ch];
    }
    if (is_c) {
        switch (mux) {
            case G_CCMUX_TEXEL0_ALPHA: return tex[3];
            case G_CCMUX_PRIMITIVE_ALPHA: return prim[3];
            case G_CCMUX_SHADE_ALPHA: return shade[3];
            case G_CCMUX_ENV_ALPHA: return env[3];
            case G_CCMUX_0: return 0;
        }
        return -1;
    }
    if (mux == G_CCMUX_1) return 255;
    if (mux == G_CCMUX_0 || mux >= 7) return 0;
    return -1;
}
static int gfx_reticle_cc_a(int mux, const uint8_t *tex, const uint8_t *shade, int is_c) {
    const uint8_t *prim = (const uint8_t *) &rdp.prim_color, *env = (const uint8_t *) &rdp.env_color;
    switch (mux) {
        case G_ACMUX_TEXEL0: return tex[3];
        case G_ACMUX_PRIMITIVE: return prim[3];
        case G_ACMUX_SHADE: return shade[3];
        case G_ACMUX_ENVIRONMENT: return env[3];
        case G_ACMUX_0: return 0;
    }
    if (!is_c && mux == G_ACMUX_1) return 255;
    return -1;
}
/* The texels through the combiner, into gfx_reticle_px; false: not a case this handles. */
static bool gfx_reticle_texels(const struct LoadedVertex *v) {
    const int slot = gfx_tex_slot(G_TX_RENDERTILE);
    const uint8_t *src = rdp.loaded_texture[slot].addr;
    const int tw = (int) ((rdp.texture_tile.lrs - rdp.texture_tile.uls + 4) >> 2);
    const int th = (int) ((rdp.texture_tile.lrt - rdp.texture_tile.ult + 4) >> 2);
    if (src == NULL || (uintptr_t) src < PORT_REAL_PTR_MIN || rdp.texture_tile.fmt != G_IM_FMT_RGBA ||
        rdp.texture_tile.siz != G_IM_SIZ_16b || tw < 1 || th < 1 || tw > 64 || th > 64 ||
        rdp.loaded_texture[slot].stride_bytes != 0 || rdp.loaded_texture[slot].size_bytes < (uint32_t) (tw * th * 2))
        return false;
    const uint8_t shade[4] = { v->color.r, v->color.g, v->color.b, v->color.a };
    /* The same texture, colours and combiner as last time: the texels are already combined (~0.4M ARM
     * instructions a frame otherwise, two crosshairs x 1024 texels). */
    static const uint8_t *k_src; static struct RGBA k_prim, k_env; static uint8_t k_cc[8], k_shade[4]; static int k_w, k_h;
    if (k_w == tw && k_h == th && k_src == src && !memcmp(&k_prim, &rdp.prim_color, sizeof(k_prim)) &&
        !memcmp(&k_env, &rdp.env_color, sizeof(k_env)) && !memcmp(k_cc, rdp.cc1, 8) && !memcmp(k_shade, shade, 4))
        return true;
    k_w = 0; /* invalid until the loop below completes */
    for (int i = 0; i < tw * th; i++) {
        const uint16_t c = be16(src + i * 2);
        const int r5 = (c >> 11) & 31, g5 = (c >> 6) & 31, b5 = (c >> 1) & 31;
        const uint8_t tex[4] = { (uint8_t) ((r5 << 3) | (r5 >> 2)), (uint8_t) ((g5 << 3) | (g5 >> 2)),
                                 (uint8_t) ((b5 << 3) | (b5 >> 2)), (uint8_t) ((c & 1) ? 255 : 0) };
        int out[4];
        for (int ch = 0; ch < 3; ch++) {
            const int a = gfx_reticle_cc_rgb(rdp.cc1[0], ch, tex, shade, 0), b = gfx_reticle_cc_rgb(rdp.cc1[1], ch, tex, shade, 0);
            const int cc = gfx_reticle_cc_rgb(rdp.cc1[2], ch, tex, shade, 1), d = gfx_reticle_cc_rgb(rdp.cc1[3], ch, tex, shade, 0);
            if (a < 0 || b < 0 || cc < 0 || d < 0) return false;
            int x = (a - b) * cc / 255 + d;
            out[ch] = x < 0 ? 0 : x > 255 ? 255 : x;
        }
        {
            const int a = gfx_reticle_cc_a(rdp.cc1[4], tex, shade, 0), b = gfx_reticle_cc_a(rdp.cc1[5], tex, shade, 0);
            const int cc = gfx_reticle_cc_a(rdp.cc1[6], tex, shade, 1), d = gfx_reticle_cc_a(rdp.cc1[7], tex, shade, 0);
            if (a < 0 || b < 0 || cc < 0 || d < 0) return false;
            int x = (a - b) * cc / 255 + d;
            out[3] = x < 0 ? 0 : x > 255 ? 255 : x;
        }
        gfx_reticle_px[i] = (uint32_t) out[3] << 24 | (uint32_t) out[0] << 16 | (uint32_t) out[1] << 8 | (uint32_t) out[2];
    }
    gfx_reticle_tw = tw; gfx_reticle_th = th;
    k_src = src; k_prim = rdp.prim_color; k_env = rdp.env_color; memcpy(k_cc, rdp.cc1, 8); memcpy(k_shade, shade, 4);
    k_w = tw; k_h = th;
    return true;
}
/* A triangle between the tags: true = taken (its box added), false = draw it as usual. */
static bool gfx_reticle_capture(struct LoadedVertex *const *va) {
#if !defined(HANDHELD)
    (void) va; return false; /* the desktop proxy has no panel layer */
#endif
    if (!gfx_reticle_have) {
        if (!gfx_reticle_enabled() || !gfx_reticle_texels(va[0])) return false;
    }
    float bb[4] = { gfx_reticle_bb[0], gfx_reticle_bb[1], gfx_reticle_bb[2], gfx_reticle_bb[3] };
    for (int i = 0; i < 3; i++) {
        const float w = va[i]->_w;
        if (!(w > 1e-6f)) return false; /* behind the eye: as before */
        const float x = (float) rdp.viewport.x + (va[i]->_x / w * 0.5f + 0.5f) * (float) rdp.viewport.width;
        const float y = (float) rdp.viewport.y + (0.5f - va[i]->_y / w * 0.5f) * (float) rdp.viewport.height;
        if (!gfx_reticle_have && i == 0) { bb[0] = bb[2] = x; bb[1] = bb[3] = y; }
        if (x < bb[0]) bb[0] = x;
        if (x > bb[2]) bb[2] = x;
        if (y < bb[1]) bb[1] = y;
        if (y > bb[3]) bb[3] = y;
    }
    memcpy(gfx_reticle_bb, bb, sizeof(bb));
    gfx_reticle_have = true;
    return true;
}
static void gfx_reticle_end(void) {
#if defined(HANDHELD)
    if (gfx_reticle_have) {
        extern void gfx_soft_reticle_lineart(const float bb[4], const uint32_t *px, int tw, int th);
        gfx_soft_reticle_lineart(gfx_reticle_bb, gfx_reticle_px, gfx_reticle_tw, gfx_reticle_th);
    }
#endif
    gfx_reticle_have = false;
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

    gfx_tri_in++;
    if (v1->clip_rej & v2->clip_rej & v3->clip_rej & ~(XY_OUT | GB_OUT)) {
        // The whole triangle lies outside the guard band (the exact side
        // planes reject below, after the phase bookkeeping; GB_OUT is one bit
        // for all four sides, so it never rejects)
        gfx_tri_clipall++;
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
    if (gfx_reticle_tag && gfx_reticle_capture(v_arr)) return; /* round 25: the crosshair, as line art */
    /* Wholly beyond one side of the view (exact planes, the same x the
     * rasterizer projects): it would rasterize no pixel, so skip the emit,
     * state rebuild and setup. After the phase bookkeeping, so target
     * selection is unchanged. SF64_SIDEREJECT=0 turns it off (A/B). */
    {
        static int on SAVESTATE_EXCLUDE = -1;
        if (on < 0) { const char *e = getenv("SF64_SIDEREJECT"); on = !(e && e[0] == '0'); }
        if (on && (v1->clip_rej & v2->clip_rej & v3->clip_rej & XY_OUT)) {
            gfx_tri_clipall++;
            return;
        }
    }
#endif
    /* Software-path far cull (device round 12; the PSP-GE PORT_DRAW_DIST block
     * that used to sit here never compiled for this build and is gone). Skip
     * triangles wholly beyond gfx_draw_dist_w (perspective only; _w grows with
     * distance). OFF by default (1e30): on DK Jungle it only bought ~17% and
     * the user rejected the pop-in; kept as a desktop tuning knob. */
    if (rsp.is_persp && v1->_w > gfx_draw_dist_w && v2->_w > gfx_draw_dist_w && v3->_w > gfx_draw_dist_w) {
        gfx_tri_far++;
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
                    gfx_tri_cull++;
                    return; }
                break;
            case G_CULL_BACK:
                if (cross >= 0) {
                    gfx_tri_cull++;
                    return; }
                break;
            case G_CULL_BOTH:
                gfx_tri_cull++;
                return;
        }
        culled_early = true;
    }
    gfx_tri_pass++;
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
            // Clip when a vertex is in front of the game's near plane (or
            // behind the eye: both set Z_POS) or beyond its far plane (Z_NEG),
            // as the RSP does. The kit triggered on w < 2, or w < 24 and past
            // far: MK64 distances (near plane 9). Star Fox's near plane is 10,
            // so a triangle 2..10 units from the camera drew at depth 0 over
            // everything instead of being cut away (super-mario-64 5cd6699, B4).
            // A vertex beyond the guard band too (R33, gfx_clipedges).
            if ((v1->clip_rej | v2->clip_rej | v3->clip_rej) & (Z_POS | Z_NEG | (gfx_clipedges == 2 ? GB_OUT : 0))) {
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
#ifdef HH_CLIPCHECK
        { extern unsigned hh_cc_clipped, hh_cc_changed; hh_cc_clipped++; if (clipped_vertices_num != 3) hh_cc_changed++; }
#endif

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
    if (gfx_cc_census_on > 0 && tri_state.use_fog) gfx_census_fogtri(v1, v2, v3);
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
    gfx_cc_census(two_cycle, 1);
    if (gfx_cc_census_on > 0) gfx_census_tex(two_cycle);
    bool use_alpha = two_cycle ? ((rdp.other_mode_l >> 16) & 3) == G_BL_1MA : ((rdp.other_mode_l >> 18) & 3) == G_BL_1MA;
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
    gfx_update_tex_lerp(cc_id, false);
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
    
    if (used_textures[0] && used_textures[1]) gfx_census_t1draw++;
    for (int pass = 0; pass < 2; pass++) {
    const uint32_t epoch = gfx_cache_epoch; /* a reset under tile 1's import: tile 0 again (gfx_cache_epoch) */
    gfx_stress_hold = pass > 0;
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
    if (pass == 0 && epoch != gfx_cache_epoch) gfx_census_underreset++;
    if (epoch == gfx_cache_epoch || !gfx_epochfix_on()) break;
    }
    gfx_stress_hold = false;
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
    /* A wrapping tile (no clamp, mask m) reads coordinate & (2^m - 1), so a texrect may start
     * whole periods before its tile's origin; the samplers wrap by the imported size instead,
     * which is smaller for edge pieces. Shift both corners by the same whole number of periods
     * (two when mirrored) so the first lands in [0, period) (paper-mario 525cb47). */
    int shift_u = 0, shift_v = 0;

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
                    shift_u = (u >= 0 ? u / p : -((p - 1 - u) / p)) * p;
                }
                if (!(rdp.texture_tile.cmt & G_TX_CLAMP) && rdp.texture_tile.maskt) {
                    const int p = (1 << rdp.texture_tile.maskt) << ((rdp.texture_tile.cmt & G_TX_MIRROR) ? 1 : 0);
                    shift_v = (v >= 0 ? v / p : -((p - 1 - v) / p)) * p;
                }
            }
            u -= shift_u;
            v -= shift_v;
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
    {   extern bool gfx_soft_2d_bilin; /* X14: this sprite's filter; a magnified one is bilinear (gfx_soft.c) */
        gfx_soft_2d_bilin = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
    }
    gfx_scegu_draw_triangles_2d((float*)&tri_buf[0],0,1);
    if (gfx_trace_frames > 0) port_log("  2d: drawn\n");
}

static void gfx_sp_geometry_mode(uint32_t clear, uint32_t set) {
    uint32_t old = rsp.geometry_mode;
    rsp.geometry_mode &= ~clear;
    rsp.geometry_mode |= set;
    if ((set & G_TEXTURE_GEN) && gfx_census_mode()) { /* test only: the frames that texgen (see gfx_cc_census) */
        static uint32_t sLast SAVESTATE_EXCLUDE = ~0u, sKey SAVESTATE_EXCLUDE, sLines SAVESTATE_EXCLUDE;
        const uint32_t key = (uint32_t) rsp.texture_scaling_factor.s << 16 ^ rsp.texture_scaling_factor.t ^
                             ((rsp.geometry_mode & G_TEXTURE_GEN_LINEAR) ? 0x80000000u : 0);
        /* a new kind, or the same one after a pause of more than a second */
        if ((key != sKey || gfx_frame_counter > sLast + 30) && sLines < 64) {
            sKey = key;
            sLines++;
            port_log("cc census: frame %u texgen%s scale %u,%u\n", gfx_frame_counter,
                     (rsp.geometry_mode & G_TEXTURE_GEN_LINEAR) ? " linear" : "", rsp.texture_scaling_factor.s,
                     rsp.texture_scaling_factor.t);
        }
        sLast = gfx_frame_counter;
    }
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
        // All eight slots: Star Fox 64 sets seven directional lights plus the
        // ambient (gSPSetLights7, sys_lights.c), which lands in G_MV_L7.
        // Taking only L0-L2 (all MK64 needs) left the ambient at zero: every
        // lit surface lost its ambient term and the game rendered too dark.
        case G_MV_L0:
        case G_MV_L1:
        case G_MV_L2:
        case G_MV_L3:
        case G_MV_L4:
        case G_MV_L5:
        case G_MV_L6:
        case G_MV_L7:
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(rsp.current_lights + (index - G_MV_L0) / 2, data, sizeof(Light_t));
            rsp.lights_changed = 1;
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
            break;
        case G_MW_CLIP: /* gSPClipRatio: RNX carries the ratio (FR_NEG_FRUSTRATIO_n = n) */
            if (offset == G_MWO_CLIP_RNX && (data & 0xffff) >= 1 && (data & 0xffff) <= 6) {
                rsp.clip_ratio = (float)(data & 0xffff);
            }
            break;
        case G_MW_SEGMENT:
            rsp_segments[(offset >> 2) & 0xF] = data;
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

static void gfx_dp_set_scissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    _UNUSED(mode);
#if defined(MIYOO)
    { /* The Miyoo Mini fills its 4:3 panel (SF64_FILL, default on; gfx_sdl12.c gfx_miyoo_fill): the scissor edges on
       * the N64's TV-safe box (SCREEN_MARGIN, 8 px) go to the frame's edges, so the scene draws its own pixels there
       * (the viewport and projection already cover the whole 320x240: nothing is stretched). SF64_FILL=0: the border. */
        extern int gfx_miyoo_fill(void);
        if (gfx_miyoo_fill()) {
            if (ulx == 8u << 2) ulx = 0;
            if (uly == 8u << 2) uly = 0;
            if (lrx == (SCREEN_WIDTH - 8u) << 2) lrx = SCREEN_WIDTH << 2;
            if (lry == (SCREEN_HEIGHT - 8u) << 2) lry = SCREEN_HEIGHT << 2;
        }
    }
#endif
#line 4403 /* (the line numbers SUPPORT_CHECK prints stay the handheld build's) */

    /* Edge seams (cross-port lead): a scissor ending one pixel short of the screen
     * (DKR sets 0,0,319,239 every frame) clips the last column and row off
     * everything, as fill rects and texrects reaching the edge are already
     * extended. Only that case: Star Fox's standby list scissors 320x720 for its
     * three stacked framebuffers, which must stay as it is. */
    if (lrx >= (SCREEN_WIDTH - 1) << 2 && lrx < SCREEN_WIDTH << 2) {
        lrx = SCREEN_WIDTH << 2;
    }
    if (lry >= (SCREEN_HEIGHT - 1) << 2 && lry < SCREEN_HEIGHT << 2) {
        lry = SCREEN_HEIGHT << 2;
    }

    float x = ulx / 4.0f * RATIO_X;
#if defined(TARGET_HANDHELD)
    float y = uly / 4.0f * RATIO_Y;                 /* top-down rasterizer: see viewport note */
#else
    float y = (SCREEN_HEIGHT - lry / 4.0f) * RATIO_Y;
#endif
    float width = ((int) lrx - (int) ulx) / 4.0f * RATIO_X;
    float height = ((int) lry - (int) uly) / 4.0f * RATIO_Y;
    if (gfx_census_mode()) { /* test only: see gfx_cc_census (ge-handheld c68da63's check) */
        gfx_census_sc++;
        if ((ulx | uly | lrx | lry) & 3) gfx_census_sc_n64++;
        if (lrx < ulx || lry < uly) gfx_census_log(&gfx_census_scinv, "inverted scissor", ulx, uly, lrx, lry);
        if (x != floorf(x) || y != floorf(y) || width != floorf(width) || height != floorf(height)) {
            if (gfx_census_sc_target++ < 8) {
                port_log("cc census: frame %u scissor %u,%u-%u,%u -> %.2f,%.2f %.2fx%.2f\n", gfx_frame_counter, ulx, uly,
                         lrx, lry, x, y, width, height);
            }
        }
    }

    /* A corner past the other one (lrx < ulx or lry < uly) leaves the RDP nothing to draw;
     * the unsigned difference made such a scissor about a billion rows tall, clipping from
     * its top edge to the bottom of the screen (f-zero-x cf242ea, the lead's R28;
     * Star Fox sets none in 2x6000 autopilot frames, so this is a guard). */
    if (width < 0.0f) width = 0.0f;
    if (height < 0.0f) height = 0.0f;
#if defined(TARGET_HANDHELD)
    {
        /* R23, the ports lead's final rule (round 2; mario-kart-64 94a67cf, checked against z64 by
         * GoldenEye, F-Zero X, Smash and SM64): (a) an edge within one N64 pixel of a screen edge goes
         * to it (the band rule above does the lower right); (b) the others on pixel centres -- a
         * column or row is inside when its centre is: first ceil(x0 - 0.5), end ceil(x1 - 0.5).
         * A guard here: Star Fox's scissors land on whole pixels at 100% (the census: 18,136, none
         * fractional), so no frame changes. SF64_SCCENTRE=0: as before. */
        static int sCentre SAVESTATE_EXCLUDE = -1;
        if (sCentre < 0) { const char *e = getenv("SF64_SCCENTRE"); sCentre = !(e && e[0] == '0'); }
        if (sCentre) {
            const uint32_t ux = ulx <= 4 ? 0 : ulx, uy = uly <= 4 ? 0 : uly;
            const int x0 = (int) ceilf(ux / 4.0f * RATIO_X - 0.5f), x1 = (int) ceilf(lrx / 4.0f * RATIO_X - 0.5f);
            const int y0 = (int) ceilf(uy / 4.0f * RATIO_Y - 0.5f), y1 = (int) ceilf(lry / 4.0f * RATIO_Y - 0.5f);
            x = (float) x0; width = (float) (x1 > x0 ? x1 - x0 : 0);
            y = (float) y0; height = (float) (y1 > y0 ? y1 - y0 : 0);
        }
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
static void gfx_set_phase(bool is3d);
/* HUD unsquish (round 21): a PORT_HUD_TAG_ANCHOR piece tag from the game (fox_hud.c). Each piece's draws go
 * to gfx_soft's 1:1 HUD layer at its anchor; a perspective piece needs a 3D pass of its own, so the phase is
 * closed at every change of piece. Only when the squeeze is on (gfx_soft_hud_unsquish): otherwise the tags
 * change nothing, not even the batching. */
static void gfx_hud_piece(int anchor) {
    extern int gfx_soft_hud_unsquish, gfx_soft_hud_anchor_now(void);
    extern void gfx_soft_set_hud_anchor(int a);
    if (!gfx_soft_hud_unsquish || anchor == gfx_soft_hud_anchor_now()) return;
    gfx_flush();
    if (gfx_phase3d) gfx_set_phase(false);
    gfx_soft_set_hud_anchor(anchor);
}
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

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, UNUSED uint32_t palette, uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts) {
    // A zero mask means no wrapping: the RDP clamps that axis to the tile
    // whatever the clamp bit says, and mirroring needs a mask (ocarina-of-time
    // 8674501: UVs run past a texture with mask 0 repeated it).
    if (masks == 0) cms = (cms & ~G_TX_MIRROR) | G_TX_CLAMP;
    if (maskt == 0) cmt = (cmt & ~G_TX_MIRROR) | G_TX_CLAMP;
    if (tile == G_TX_RENDERTILE) {
        tri_state.valid = false;
    }

    if (tile == G_TX_RENDERTILE) {
        rdp.texture_tile.palette = (uint8_t) (palette & 0xF); // CI4: upper 4 bits of the colour index
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
        rdp.texture_tile.tmem_slot = (tmem / 256) & 1;
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
    if (tile == 1) {
        rdp.textures_changed[1] = true;
        tri_state.valid = false;
    }
    if (tile == G_TX_LOADTILE) {
        rdp.texture_to_load.tile_number = tmem / 256;
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
        gfx_census_sts0[0] = uls; gfx_census_sts0[1] = ult; gfx_census_sts0[2] = lrs; gfx_census_sts0[3] = lrt;
        gfx_census_ltflag = false; /* test only: see gfx_census_tex */
    }
}

static void gfx_dp_load_tlut(UNUSED uint8_t tile, uint32_t high_index) {
    tri_state.valid = false;
    _UNUSED(high_index);

    SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(rdp.texture_to_load.siz == G_IM_SIZ_16b);
    rdp.palette = rdp.texture_to_load.addr;
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
    if (gfx_census_mode()) { /* test only: see gfx_cc_census */
        gfx_census_load_tmem[(rdp.tile_tmem[tile & 7] / 256) & 1] = rdp.tile_tmem[tile & 7];
        gfx_census_blocks++;
        if (dxt == 0 && gfx_census_dxt0++ < 8) {
            port_log("cc census: frame %u LoadBlock dxt 0, siz %u lrs %u\n", gfx_frame_counter,
                     (unsigned) rdp.texture_to_load.siz, (unsigned) lrs);
        }
    }

    SUPPORT_CHECK(uls == 0);
    SUPPORT_CHECK(ult == 0);
    rdp.texture_to_load.tile_number = (rdp.tile_tmem[tile & 7] / 256) & 1; // the TMEM half this tile loads into
    
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
    
    rdp.textures_changed[rdp.texture_to_load.tile_number] = true;
}

static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    tri_state.valid = false;
    if (gfx_census_mode()) { /* test only: see gfx_census_tex */
        gfx_census_load_tmem[(rdp.tile_tmem[tile & 7] / 256) & 1] = rdp.tile_tmem[tile & 7];
        gfx_census_ltflag = uls != gfx_census_sts0[0] || ult != gfx_census_sts0[1] || lrs != gfx_census_sts0[2] ||
                            lrt != gfx_census_sts0[3];
    }
    rdp.texture_to_load.tile_number = (rdp.tile_tmem[tile & 7] / 256) & 1; // the TMEM half this tile loads into
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
    rdp.texture_tile.uls = uls;
    rdp.texture_tile.ult = ult;
    rdp.texture_tile.lrs = lrs;
    rdp.texture_tile.lrt = lrt;
    gfx_render_tile_cm();

    rdp.textures_changed[rdp.texture_to_load.tile_number] = true;
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
    if (rdp.fog_color.r != r || rdp.fog_color.g != g || rdp.fog_color.b != b) {
        tri_state.valid = false; // gfx_tri_fog latches the colour (per-pixel fog, and which rule is exact)
    }
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

#if defined(TARGET_HANDHELD)
        /* 2D keeps the frame's own mapping: the handheld fit squeezes the whole
         * 320x240 frame to the panel at present (area-filtered), and only the
         * perspective 3D is pre-stretched (gfx_persp_aspect) to come out at
         * true aspect. gfx_adjust_x_for_aspect_ratio stretched 2D too, by 4/3
         * about the centre: the HUD's shield gauge, hit counter, lives, bombs
         * and radio portraits ran off the panel's edges (device screenshot,
         * 24 Sep). Uniform squeeze = the N64 layout exactly, as MK64 settled on. */
        ulxf = ulxf * (4.0f / 3.0f) / out_aspect;
        lrxf = lrxf * (4.0f / 3.0f) / out_aspect;
#elif !defined(PORT_GE_TL) || defined(PORT_GE_TL_ASPECT)
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
    /* R34 (super-smash-bros 3b03a66): with the colour image set to the Z image the game is writing
     * depth, not pixels -- the fill-rect path already skips those; a texrect there showed Smash's
     * depth stamp as a black square. Counted by SF64_CC_CENSUS; SF64_ZIMGRECT=1 draws them anyway. */
    if (rdp.color_image_address == rdp.z_buf_address) {
        static int sZimg SAVESTATE_EXCLUDE = -1;
        if (sZimg < 0) { const char *e = getenv("SF64_ZIMGRECT"); sZimg = e && e[0] == '1'; }
        if (gfx_census_mode()) gfx_census_log(&gfx_census_zimgrect, "texrect into the Z image (ulx uly lrx lry)", (uint32_t) ulx, (uint32_t) uly, (uint32_t) lrx, (uint32_t) lry);
        if (!sZimg) return;
    }

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
        if (gfx_census_mode()) {
            gfx_census_texrect1++;
            if ((ulx | uly | lrx | lry) & 3) gfx_census_log(&gfx_census_texrectfrac, "1/2-cycle texrect off whole N64 pixels (ulx uly lrx lry)", (uint32_t) ulx, (uint32_t) uly, (uint32_t) lrx, (uint32_t) lry);
        }
        /* R11 (goldeneye-007 9e9b6ed): in 1/2-cycle mode the RDP draws every pixel a sub-scanline
         * of the rectangle touches, so it covers whole N64 pixels, [floor(ul), ceil(lr)); S and T
         * stay the first pixel's. Star Fox: 4,542 of 94,386 texrects in 4000 autopilot frames end on
         * a quarter pixel (the teammates' shield gauges, the radio and mission panels); taking it
         * moves one edge pixel in 27 of 81 sweep frames. Against z64 at the same instant (Peppy's
         * gauge, port 2000 = z64 2021) the right ends match better and the left end worse, and it
         * is invisible on a square output, so it stays OFF (round 13). SF64_TEXRECTPX=1: the RDP rule. */
        static int sTexrectPx SAVESTATE_EXCLUDE = -1;
        if (sTexrectPx < 0) { const char *e = getenv("SF64_TEXRECTPX"); sTexrectPx = e && e[0] == '1'; }
        if (sTexrectPx) { ulx &= ~3; uly &= ~3; lrx = (lrx + 3) & ~3; lry = (lry + 3) & ~3; }
        // 1/2-cycle: the lower-right edge is exclusive, so a full-screen dim to
        // (319, 239) leaves a 1px seam a CRT's overscan hid (MK64 99921c7).
        if (lrx >= (SCREEN_WIDTH - 1) << 2) lrx = SCREEN_WIDTH << 2;
        if (lry >= (SCREEN_HEIGHT - 1) << 2) lry = SCREEN_HEIGHT << 2;
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
     * SF64_TEXRECTINV=1: drawn as before. */
    if (lrx < ulx || lry < uly) {
        static int draw SAVESTATE_EXCLUDE = -1;
        if (draw < 0) { const char *e = getenv("SF64_TEXRECTINV"); draw = e && e[0] == '1'; }
        if (!draw) { rdp.combine_mode = saved_combine_mode; return; }
    }
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    if (rdp.color_image_address == rdp.z_buf_address) {
        // Don't clear Z buffer here since we already did it with glClear
        return;
    }
#if defined(MIYOO)
    { /* SF64_FILL (gfx_dp_set_scissor): a fill whose edges lie on the TV-safe box (the frame's clear, the wipes, the
       * split-screen bars) reaches the frame's edges too (fill and copy modes' lower-right is inclusive: 311 / 231) */
        extern int gfx_miyoo_fill(void);
        /* only a rect 16 px or more along one side: the menu's starfield plots 1-px fill rects, and one sitting on
         * the box's edge became an 8-px dash */
        if (gfx_miyoo_fill() && (lrx - ulx >= 16 << 2 || lry - uly >= 16 << 2)) {
            if (ulx == 8 << 2) ulx = 0;
            if (uly == 8 << 2) uly = 0;
            if (lrx == (SCREEN_WIDTH - 9) << 2 || lrx == (SCREEN_WIDTH - 8) << 2) lrx += 8 << 2;
            if (lry == (SCREEN_HEIGHT - 9) << 2 || lry == (SCREEN_HEIGHT - 8) << 2) lry += 8 << 2;
        }
    }
#endif
#line 5013
    uint32_t mode = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));
    
    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    } else {
        // 1/2-cycle: the lower-right edge is exclusive, so a full-screen box
        // drawn to (319, 239) leaves the last column and row uncovered -- a
        // bright 1px seam on the handheld panel (MK64 device report after v29).
        if (lrx >= (SCREEN_WIDTH - 1) << 2) lrx = SCREEN_WIDTH << 2;
        if (lry >= (SCREEN_HEIGHT - 1) << 2) lry = SCREEN_HEIGHT << 2;
    }
    /* The same for a fill: an inverted rectangle covers no pixel on the RDP (texrect above).
     * SF64_TEXRECTINV=1: drawn as before. */
    if (lrx < ulx || lry < uly) {
        static int draw SAVESTATE_EXCLUDE = -1;
        if (draw < 0) { const char *e = getenv("SF64_TEXRECTINV"); draw = e && e[0] == '1'; }
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
    _UNUSED(size);
    _UNUSED(width);

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
#if defined(TARGET_HANDHELD)
                    gfx_hud_piece(0); /* a HUD section opens and closes on the squeezed frame */
#endif
                }
#if defined(TARGET_HANDHELD)
                else if ((cmd->words.w1 & ~0xFu) == PORT_HUD_TAG_ANCHOR) gfx_hud_piece((int)(cmd->words.w1 & 0xFu));
                else if (cmd->words.w1 == PORT_RETICLE_TAG_ON) { gfx_reticle_tag = 1; gfx_reticle_have = false; }
                else if (cmd->words.w1 == PORT_RETICLE_TAG_OFF) { gfx_reticle_tag = 0; gfx_reticle_end(); }
#endif
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
    rsp.current_num_lights = 2;
    rsp.lights_changed = true;
    /* Each task starts from the microcode's DMEM image: F3D has lookat Y dir
     * (127, 0, 0) and lookat X (0, 127, 0) (goldeneye-007 c68da63, GoldenEye's
     * F3D 2.0G; Star Fox's F3DEX checked against m64ref z64, BUILDLOG round 9). */
    memset(rsp.current_lookat, 0, sizeof(rsp.current_lookat));
    rsp.current_lookat[0].dir[0] = 127;
    rsp.current_lookat[1].dir[1] = 127;
    rsp.clip_ratio = 2.0f; /* the microcode's default until the game's gSPClipRatio (Star Fox: 1, first thing each frame) */
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
    { /* SF64_BOOTSYNC (goldeneye-007 round 17, "Bond missing from the gun barrel"): the soft backend starts with
       * depth test and depth write on, gfx_pc's cached rendering_state with them off. A draw sends a flag only when
       * it differs from the cache, so until the first depth-tested draw every draw was depth-tested and wrote depth.
       * Start the backend as the cache says. SF64_BOOTSYNC=0: as before. */
        const char *e = getenv("SF64_BOOTSYNC");
        if (!(e && e[0] == '0')) {
            gfx_rapi->set_depth_test(rendering_state.depth_test);
            gfx_rapi->set_depth_mask(rendering_state.depth_mask);
            gfx_rapi->set_zmode_decal(rendering_state.decal_mode);
            gfx_rapi->set_use_alpha(rendering_state.alpha_blend);
        }
    }
    { const char *e = getenv("SF64_CLIPEDGES"); if (e && e[0] >= '0' && e[0] <= '2') gfx_clipedges = e[0] - '0'; }
    { /* SF64_DRAWDIST=<clip.w units>: far cull, desktop tuning, off by default.
       * Validated: non-numeric, trailing junk, <= 0 or absurd stays OFF -- the
       * round-12 bare atof() turned "abc" into 0.0f and culled the whole 3D
       * scene with no log line. Read here, once, not in the per-frame gfx_run. */
        const char *e = getenv("SF64_DRAWDIST"); char *end = NULL;
        gfx_draw_dist_w = 1e30f;
        if (e && e[0]) {
            const double d = strtod(e, &end);
            if (end && *end == '\0' && d > 0.0 && d < 1e9) { gfx_draw_dist_w = (float)d; port_log("gfx: far cull at clip.w %.0f (SF64_DRAWDIST)\n", d); }
            else port_log("gfx: ignoring SF64_DRAWDIST='%s' (want a positive number)\n", e);
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
    rendering_state.fog_px = false; // gfx_scegu_reset_texfunc_add turns the backend's fog off too
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
    /* SF64_LOADSYNC (super-smash-bros 9e9bb19): the four flags above were reset in the cache only; the soft backend
     * kept what the last draw before the save had set. A draw sends a flag only when it differs from the cache,
     * so a draw wanting the reset value (blend on, depth off) was drawn with the backend's old one until some
     * draw flipped it. The edge-opaque mode comes back with the state in the cache, while the backend's is kept
     * live (SAVESTATE_EXCLUDE): the same mismatch. Set the backend to what the cache says. Runs only here
     * (after a load). SF64_LOADSYNC=0: as before. */
    {
        static int on SAVESTATE_EXCLUDE = -1;
        if (on < 0) { const char *e = getenv("SF64_LOADSYNC"); on = !(e && e[0] == '0'); }
        if (on) {
            extern void gfx_soft_set_edge_opaque(int mode);
            gfx_rapi->set_depth_test(rendering_state.depth_test);
            gfx_rapi->set_depth_mask(rendering_state.depth_mask);
            gfx_rapi->set_zmode_decal(rendering_state.decal_mode);
            gfx_rapi->set_use_alpha(rendering_state.alpha_blend);
            gfx_soft_set_edge_opaque(rendering_state.edge_opaque);
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
    gfx_debug_frame = gfx_trace_frames > 0; /* SF64_TRACE_AT: per-draw state too */
    
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

#ifdef HH_CLIPCHECK
unsigned hh_cc_all, hh_cc_near, hh_cc_far, hh_cc_clipped, hh_cc_changed;
void hh_clipcheck_report(void) {
    fprintf(stderr, "CLIPCHECK vertices %u, in front of near %u, beyond far %u | clipped tris %u (shape changed %u)\n",
            hh_cc_all, hh_cc_near, hh_cc_far, hh_cc_clipped, hh_cc_changed);
}
#endif
