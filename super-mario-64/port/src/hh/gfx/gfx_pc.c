#if defined(TARGET_PSP) || defined(TARGET_HANDHELD)
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#if defined(__ARM_NEON) && !defined(TARGET_PSP)
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
#define MAX_LIGHTS 2
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
    Light_t current_lookat[2]; // [0] = G_MV_LOOKATY, [1] = G_MV_LOOKATX, as the F3D DMEM holds them
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
        uint8_t cms, cmt;
        uint8_t masks, maskt; // wrap periods (2^mask texels): texrects start whole periods before the origin
        uint8_t tmem_slot; // which loaded_texture[] the render tile reads: its TMEM half (tmem / 256)
        uint16_t uls, ult, lrs, lrt; // U10.2
        uint32_t line_size_bytes;
    } texture_tile;
    bool textures_changed[2];
    
    uint32_t other_mode_l, other_mode_h;
    uint32_t combine_mode;
    
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
    int hilite;   // backend texel lerp toward a constant (cc_texlerp_mode bits)
    uint8_t hilite_c[4];
    bool trilerp; // backend TRILERP on
    int trilerp_f;
    bool fog_pix; // backend per-pixel fog on
    bool edge_opaque; // TEX_EDGE without FORCE_BL: the soft backend writes it unblended
    uint8_t fog_c[3];
    int fog_a;
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
    bool fog_fold;                         // fog folded into the vertex colour (exact here); else the backend fogs per pixel
    bool fog_raw_alpha;                    // per-pixel fog: the vertex alpha carries the raw fog factor
#ifdef HH_CENSUS
    uint8_t fog_census;                    // the fog rule's class for this state (hh_census_print), 0 = not fogged
    bool fog_census_diff;                  // ... and the reference decides otherwise than de0b5af did
#endif
    bool trilerp;                          // G_CC_TRILERP: the backend blends tile 0/1 by a per-triangle LOD fraction
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
/* Beyond the RSP's guard band: |x| or |y| > k|w| in the N64's clip space, k the clip ratio
 * (gfx_clip_ratio). One bit for all four sides, so three vertices sharing it may be on
 * opposite sides of the screen: it stays out of the shared-bit reject. */
#define GB_OUT (0x1000)

/* The clipper (M4 round 2: the lead's R32/R33, DKR 467ad1e). SM64_CLIP, read in gfx_init:
 *   2 (default) the RSP's: a triangle is clipped when a vertex is in front of the near
 *     plane, beyond the far one or beyond the guard band, and clip_rsp cuts it the way
 *     gspFast3D does (below);
 *   1 the same triggers, the kit's clipper (clip_to_frustum);
 *   0 the kit's triggers, near or far only: the build before this round.
 * The shade colour is interpolated on screen, so where a triangle is cut changes its
 * shading: cut only where the console cuts, or a floor's shading jumps the frame a vertex
 * crosses the near plane and the kit starts cutting it at the screen edges too. */
static int gfx_clip_mode SAVESTATE_EXCLUDE = 2;
static uint32_t gfx_clip_trigger SAVESTATE_EXCLUDE = Z_POS | Z_NEG | GB_OUT;
/* gSPClipRatio's k: the guard band is x, y = +-k w. Each task starts from gspFast3D's
 * DMEM, where k is 1 (rsp/fast3d.s: the clip planes at 0x70, k at 0x76, which the vertex
 * code tests); SM64 US never sets it (game_init.c sends gSPClipRatio only for F3DEX2), so
 * its guard band is the N64 screen's own edges. */
static int gfx_clip_ratio = 1;

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

uint32_t clip_to_frustum( struct LoadedVertex * v0, struct LoadedVertex * v1, uint32_t vIn )
{
	uint32_t vOut;

	vOut = vIn;

	// Near plane (w-based) first: removes behind-camera parts so the NDC-plane
	// clips below see only w>0 vertices.  Bounces v0<->v1 like the others.
	vOut = clipToNearW( v1, v0, vOut );
	{ struct LoadedVertex *t = v0; v0 = v1; v1 = t; }

	vOut = clipToHyperPlane( v1, v0, vOut, NDCPlane[2] );		// right
	vOut = clipToHyperPlane( v0, v1, vOut, NDCPlane[1] );		// left
	vOut = clipToHyperPlane( v1, v0, vOut, NDCPlane[4] );		// top
	vOut = clipToHyperPlane( v0, v1, vOut, NDCPlane[3] );		// bottom
	vOut = clipToHyperPlane( v1, v0, vOut, NDCPlane[0] );		// near
	vOut = clipToHyperPlane( v0, v1, vOut, NDCPlane[5] );		// far

	return vOut;
}

/* The RSP's clipper, as gspFast3D runs it (rsp/fast3d.s, perform_clip; M4 round 2).
 * The polygon starts as the command's three vertices in reverse order and meets six
 * planes: near (z >= -w), far (z <= w), then the guard band's sides y <= kw, x <= kw,
 * y >= -kw, x >= -kw. A vertex on a plane counts as outside it (the RSP's clip codes
 * are >= and <=). A plane no vertex is outside is skipped, and the polygon keeps its
 * order; a plane every vertex is outside drops the triangle. Otherwise the new polygon
 * starts with the point where the old one enters the plane, then its inside vertices in
 * order, then the point where it leaves. A new point is interpolated from the inside
 * vertex toward the outside one, and its fog is computed again from its own depth, as
 * the RSP's vertex code does for it. Returns the vertex count (0: nothing to draw); the
 * caller draws a fan from res[0]. */
static struct LoadedVertex clip_pool[12]; /* two new points a plane, six planes */

static inline float clip_rsp_dist(const struct LoadedVertex *v, int plane, float k) {
    switch (plane) { /* > 0: inside */
        case 0: return v->_z + v->_w;
        case 1: return v->_w - v->_z;
        case 2: return k * v->_w - v->_y;
        case 3: return k * v->_w - v->_x;
        case 4: return v->_y + k * v->_w;
        default: return v->_x + k * v->_w;
    }
}

static void clip_rsp_point(struct LoadedVertex *o, const struct LoadedVertex *in, const struct LoadedVertex *out,
                           float din, float dout) {
    gfx_clip_interpolate_vert(o, in, out, din / (din - dout));
    o->w = in->w + (out->w - in->w) * (din / (din - dout));
    o->clip_rej = 0;
    if (rsp.geometry_mode & G_FOG) { /* as gfx_sp_vertex computes it */
        float winv = (o->_w > -0.001f && o->_w < 0.001f) ? 32767.0f : 1.0f / o->_w;
        if (winv < 0.0f) winv = 32767.0f;
        float fog_z = o->_z * winv * rsp.fog_mul + rsp.fog_offset;
        if (fog_z < 0.0f) fog_z = 0.0f;
        if (fog_z > 255.0f) fog_z = 255.0f;
        o->color.a = (uint8_t) (int) fog_z;
    }
}

static uint32_t clip_rsp(struct LoadedVertex *const v_arr[3], const struct LoadedVertex **res) {
    const struct LoadedVertex *la[10], *lb[10], **src = la, **dst = lb, **t;
    const float k = (float) gfx_clip_ratio;
    uint32_t n = 3, pool = 0;
    la[0] = v_arr[2]; la[1] = v_arr[1]; la[2] = v_arr[0];
    for (int plane = 0; plane < 6; plane++) {
        float d[10];
        uint32_t nout = 0;
        for (uint32_t i = 0; i < n; i++) { d[i] = clip_rsp_dist(src[i], plane, k); nout += d[i] <= 0.0f; }
        if (nout == 0) continue;
        if (nout == n) return 0;
        uint32_t i = 0, m = 0;
        while (d[i] > 0.0f) i++;                 /* the first vertex outside */
        while (d[i] <= 0.0f) i = (i + 1) % n;    /* the first inside after it */
        const uint32_t lo = (i + n - 1) % n;     /* ... and the last outside before that */
        clip_rsp_point(&clip_pool[pool], src[i], src[lo], d[i], d[lo]);
        dst[m++] = &clip_pool[pool++];
        do { dst[m++] = src[i]; i = (i + 1) % n; } while (d[i] > 0.0f);
        const uint32_t li = (i + n - 1) % n;     /* the last inside vertex */
        clip_rsp_point(&clip_pool[pool], src[li], src[i], d[li], d[i]);
        dst[m++] = &clip_pool[pool++];
        n = m;
        t = src; src = dst; dst = t;
    }
    for (uint32_t i = 0; i < n; i++) res[i] = src[i];
    return n;
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

    if (gfx_clip_mode == 2) {
        /* The RSP's fan: (P[j+2], P[j+1], P0), the original triangle's winding (its
         * list starts reversed), which the late winding cull in gfx_sp_tri1 reads. */
        const struct LoadedVertex *poly[10];
        const uint32_t n = clip_rsp(v_arr, poly);
        for (uint32_t j = 0; j + 2 < n && clipped_vertices_num + 3 <= 32; ++j) {
            p_p_vertices[clipped_vertices_num++] = *poly[j + 2];
            p_p_vertices[clipped_vertices_num++] = *poly[j + 1];
            p_p_vertices[clipped_vertices_num++] = *poly[0];
        }
        *p_num_vertices = clipped_vertices_num;
        return;
    }

    temp_a[ 0 ] = *v_arr[ 0 ];
    temp_a[ 1 ] = *v_arr[ 1 ];
    temp_a[ 2 ] = *v_arr[ 2 ];

    /* Seven passes (near-w, four sides, far, near) bounce temp_a <-> temp_b,
     * so the result is in temp_b. The kit fanned temp_a -- the polygon before
     * the last (near) pass, with the last pass's vertex count (round 24). */
    uint32_t out = clip_to_frustum( temp_a, temp_b, 3 );
    const struct LoadedVertex *poly = temp_b;
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
/* Menu screens (hh_game.h hh_ui_frame): the game flags the frame while it
 * builds it; gfx_run latches the flag for that frame's display list. */
int gfx_ui_frame;
static bool gfx_ui_active;
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
#if !defined(PORT_GE_TL)
/* The in-game HUD unsquished (the user, 2026-09-25, for every port: "any hud or icons or overlays
 * that squash to 1:1 could they be unsquished and moved to fit"; MK64 39258d3 is the template, the
 * user's pick there). On the square output the HUD pass -- between PORT_HUD_TAG_ON and _OFF,
 * area.c's render_hud and its text labels -- is unsquished in place: each element keeps the centre
 * the squeeze gives it today and is drawn at 1:1 around it (x scaled like y, not by 0.75), so it
 * grows by an eighth of its width on each side and nothing moves. Draws the game brackets with
 * PORT_HUD_TAG_KEEP stay squeezed. Texrects (gfx_draw_rectangle: the counters, the timer, the
 * camera icon) and ortho quads (the end of gfx_sp_vertex: the power meter) use the same rule; an
 * element drawn inside a recent larger one grows around that one's centre, so it stays put on it.
 * SM64's counters and timer are rows of 16-px glyphs, each its own texrect: hud.c names each
 * element's box (PORT_HUD_TAG_GROUP), and every draw inside it is laid out at 1:1 around the box's
 * centre -- the letters spaced at 1:1 too, not each glyph widened in place. At 1:1 the N64's top
 * row (lives, the POWER meter, coins, stars) doesn't fit 240 px with its spacing, so, as in
 * DrUm78's port (the user's pick, 2026-09-26), the counters drop their "x" glyphs and hud.c places
 * the elements: the lives against the left margin, the stars, the timer and the camera against
 * the right one, the coins just left of the stars (PORT_HUD_AT_*); unplaced ones keep the squeezed
 * centre and stay inside their squeezed outer edge.
 * Full-width fills, and anything wider than the screen at 1:1, keep the squeeze. The act select
 * (star_select.c) and the pause screen (PORT_HUD_TAG_CROP, ingame_menu.c) have passes of their
 * own; the other menus and the dialogs stay squeezed; 4:3 output is untouched.
 * SM64_HUD=0: the squeeze everywhere, as before. */
static int sHudMode SAVESTATE_EXCLUDE = -1;
static int sHudKeep SAVESTATE_EXCLUDE; /* inside a PORT_HUD_TAG_KEEP bracket */
static int hud_fit_mode(void) {
    if (sHudMode < 0) { const char *e = getenv("SM64_HUD"); sHudMode = !(e && e[0] == '0'); }
    return sHudMode && !sHudKeep && gfx_hud_anchor == 1 && gfx_current_dimensions.width * 3 < gfx_current_dimensions.height * 4;
}
/* A placed element: full (leave it squeezed), or the layout x it grows around (cx) and where that
 * lands (ox), in layout units (output px / RATIO_X): with a lowered 3D resolution a placement made
 * on the internal target (the tags and vertex loads come in the 3D phase) was used on the native
 * one, and on the device the counters slid to the middle and the power meter's pie out of its
 * frame (IMG_0132-0134, 9b6f60cc with the ladder on). RATIO_X and hud_scale() scale together on a
 * square target, so a layout-unit placement lands the same on either. */
struct HudPlace { int full; float cx, ox, k; }; /* k: the 1:1 scale times this (a group too wide for the screen) */
struct HudBox { float x0, x1, y0, y1; struct HudPlace p; };
static struct HudBox sHudBox[8] SAVESTATE_EXCLUDE; /* this pass's larger elements, newest last */
static int sHudBoxN SAVESTATE_EXCLUDE;
static struct HudBox sHudGroup[16] SAVESTATE_EXCLUDE; /* this pass's named elements (PORT_HUD_TAG_GROUP) */
static int sHudGroupN SAVESTATE_EXCLUDE;
static uint32_t sHudGroupX SAVESTATE_EXCLUDE; /* the pending group's x0 | x1 << 12 */
/* PORT_HUD_TAG_CROP's pass (the pause screen): the layout at 1:1 about the screen's centre, i.e.
 * a 240-wide crop of it, instead of each element at 1:1 about its squeezed centre. */
static int sHudCrop SAVESTATE_EXCLUDE;
#define HUD_CROP_CX(cx) (((cx) - (float) HALF_SCREEN_WIDTH) * hud_scale() + 0.5f * (float) gfx_current_dimensions.width)
static void hud_fit_reset(void) { sHudBoxN = 0; sHudKeep = 0; sHudGroupN = 0; sHudCrop = 0; }
/* A named element: 1:1 around its centre, then back inside its squeezed outer edge. */
/* The unsquish is on for this output (hud.c drops the counters' "x" glyphs then, as DrUm78's). */
int gfx_hud_unsquish(void) {
    if (sHudMode < 0) { const char *e = getenv("SM64_HUD"); sHudMode = !(e && e[0] == '0'); }
    return sHudMode && gfx_current_dimensions.width * 3 < gfx_current_dimensions.height * 4;
}
#define HUD_MARGIN 8.0f /* output px: the left and right margins of the placed elements */
/* A placement on whole output pixels: layout x lands on x * hud_scale() + a whole-pixel offset, so
 * at 1:1 each texel centre is a pixel centre (gfx_soft.c samples those quads point). */
static float hud_snap(float cx, float ox_px) {
    const float px = (float) gfx_current_dimensions.width / 240.0f, d = ox_px - cx * hud_scale();
    return cx * hud_scale() + floorf(d / px + 0.5f) * px;
}
static void hud_group_add(uint32_t xs, uint32_t ys) {
    struct HudBox *g;
    if (sHudGroupN == 16) return;
    g = &sHudGroup[sHudGroupN++];
    g->x0 = (float) (xs & 0xfff); g->x1 = (float) ((xs >> 12) & 0xfff);
    g->y0 = (float) (ys & 0x3ff); g->y1 = (float) ((ys >> 12) & 0x7ff);
    const int mode = (ys >> 10) & 3; /* PORT_HUD_AT_* */
    const float sl = g->x0 * RATIO_X, sr = g->x1 * RATIO_X, w = (float) gfx_current_dimensions.width;
    const float px = w / 240.0f; /* one pixel of the 240 output on this target */
    const float cx = 0.5f * (g->x0 + g->x1), avail = w - 2.0f * HUD_MARGIN * px;
    float half = 0.5f * (g->x1 - g->x0) * hud_scale(), k = 1.0f;
    if (2.0f * half > avail) { k = avail / (2.0f * half); half = 0.5f * avail; } /* wider than the screen at 1:1: fit it */
    float ox = sHudCrop ? HUD_CROP_CX(cx) : cx * RATIO_X;
    if (sHudCrop && mode == 0) {                                /* the crop: back inside the margins */
        if (ox - half < HUD_MARGIN * px) ox = HUD_MARGIN * px + half;
        else if (ox + half > w - HUD_MARGIN * px) ox = w - HUD_MARGIN * px - half;
    } else if (mode == 1) ox = HUD_MARGIN * px + half;                 /* against the left margin */
    else if (mode == 2) ox = w - HUD_MARGIN * px - half;        /* against the right margin */
    else if (mode == 3 && sHudGroupN >= 2) {                    /* just left of the element placed before it */
        const struct HudBox *q = &sHudGroup[sHudGroupN - 2];
        ox = q->p.ox * RATIO_X - 0.5f * (q->x1 - q->x0) * hud_scale() - 4.0f * px - half;
    } else if (cx < HALF_SCREEN_WIDTH - 24.0f && ox - half < sl) ox = sl + half; /* inside its squeezed outer edge */
    else if (cx > HALF_SCREEN_WIDTH + 24.0f && ox + half > sr) ox = sr - half;
    g->p.full = (ys >> 23) & 1; /* PORT_HUD_KEEP_Y1: the element keeps the squeeze */
    g->p.cx = cx; g->p.ox = (k < 1.0f ? ox : hud_snap(cx, ox)) / RATIO_X; /* layout units (see HudPlace) */
    g->p.k = k;
}
/* x0..x1, y0..y1: the element in the 320x240 layout. */
static struct HudPlace hud_fit_place(float x0, float x1, float y0, float y1) {
    const float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
    const float f0 = (float) rdp.viewport.x / RATIO_X, f1 = (float) (rdp.viewport.x + rdp.viewport.width) / RATIO_X;
    struct HudPlace p;
    int i;
    p.full = -1;
    p.cx = cx;
    p.ox = cx; /* layout units (see HudPlace) */
    p.k = 1.0f;
    for (i = 0; i < sHudGroupN; i++) { /* inside a named element: laid out with it */
        const struct HudBox *g = &sHudGroup[i];
        if (cx >= g->x0 - 1.0f && cx <= g->x1 + 1.0f && cy >= g->y0 - 1.0f && cy <= g->y1 + 1.0f) return g->p;
    }
    if (sHudCrop) { /* the crop: 1:1 about the screen's centre; full-width fills keep the squeeze */
        p.full = (x0 <= f0 + 1.0f && x1 >= f1 - 1.0f) || (x1 - x0) * hud_scale() > (float) gfx_current_dimensions.width;
        p.ox = hud_snap(cx, HUD_CROP_CX(cx)) / RATIO_X;
        return p;
    }
    for (i = sHudBoxN - 1; i >= 0; i--) { /* inside a recent larger element: go with it */
        const struct HudBox *b = &sHudBox[i];
        if (cx >= b->x0 - 1.0f && cx <= b->x1 + 1.0f && cy >= b->y0 - 1.0f && cy <= b->y1 + 1.0f) { p = b->p; break; }
    }
    if (p.full < 0) p.ox = hud_snap(cx, cx * RATIO_X) / RATIO_X; /* a new element: on whole pixels */
    if (p.full < 0)
        p.full = (x0 <= f0 + 1.0f && x1 >= f1 - 1.0f) || (x1 - x0) * hud_scale() > (float) gfx_current_dimensions.width;
    if (!p.full && x1 - x0 >= 12.0f && y1 - y0 >= 12.0f) {
        if (sHudBoxN == 8) { memmove(sHudBox, sHudBox + 1, 7 * sizeof(sHudBox[0])); sHudBoxN = 7; }
        sHudBox[sHudBoxN].x0 = x0; sHudBox[sHudBoxN].x1 = x1; sHudBox[sHudBoxN].y0 = y0; sHudBox[sHudBoxN].y1 = y1;
        sHudBox[sHudBoxN++].p = p;
    }
    return p;
}
/* Layout x to output pixels: the centre where the squeeze puts it, the rest at 1:1 around it. */
static float hud_fit_x(float x, const struct HudPlace *p) { return p->ox * RATIO_X + (x - p->cx) * hud_scale() * p->k; }
#endif
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
 * wiring). Desktop tuning only: SM64_DRAWDIST=<units>, validated and parsed
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

/* Bumped by every texture cache reset (OoT 2b69f7b). A reset wipes every texture, including the one
 * the OTHER tile of the draw being set up already holds: importing tile 1 could reset the cache and
 * leave rendering_state.textures[0] on a zeroed pool node, and the draw went out untextured (a flat
 * block of the combiner's colour) or on a recycled id (another texture). The import loops run again
 * when it changes under them. SM64_EPOCHFIX=0: the old behaviour (A/B). */
static uint32_t gfx_cache_epoch SAVESTATE_EXCLUDE;
static int gfx_epochfix SAVESTATE_EXCLUDE = -1;
static bool gfx_epochfix_on(void) {
    if (gfx_epochfix < 0) { const char *e = getenv("SM64_EPOCHFIX"); gfx_epochfix = !(e && e[0] == '0'); }
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
    if (gfx_epochfix_on()) { /* nothing may keep a node of the pool wiped below */
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
void port_course_textures_loaded(void *start, u32 size) {
    course_tex_lo = (const char *) start;
    course_tex_hi = course_tex_lo + size;
    fb_tile_lo = fb_tile_hi = NULL;
    tex_cache_flush_pending = 1; /* the same addresses are about to mean other texels */
}
void port_fb_tile_note(void *target, u32 bytes) {
    const char *t = (const char *) target;
    if (fb_tile_lo == NULL || t < fb_tile_lo) fb_tile_lo = t;
    if (fb_tile_hi == NULL || t + bytes > fb_tile_hi) fb_tile_hi = t + bytes;
    /* No cache flush when the range grows: gfx_is_static_memory is asked on
     * every lookup, so a tile imported as a static course texture before its
     * first capture hashes its content at its next lookup, misses the static
     * marker (odd; content hashes are even, see gfx_texture_content_hash) and
     * re-imports on its own. The whole-cache reset that used to sit here
     * re-imported every other texture too, a start-line spike at the stadium
     * screens (the "quadrant lost the buffer" symptom it was aimed at was
     * the uint16 viewport, round 7). */
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

/* Render-tile texture size in texels: the tile's window, corner to corner.
 * SM64 moves a tile's corner in one place only: Goddard's Mario head slides a
 * 32x32 window over its face-shine highlight (gDPSetHilite1Tile: uls/ult = the
 * light's offset, lrs/lrt = offset + 31), so the corner is a UV offset (applied
 * in gfx_tri_rebuild_state) and the size is the window. Every other tile in the
 * game has a zero corner (all 1,408 in the data), where this is the same size.
 * The kit sized LoadBlock textures from the origin to lrs instead, for MK64's
 * scrolling pads, which move only uls: that made the shine a 57x57 texture cut
 * from the 32x32 load. */
static inline void gfx_tile_size(int tile, uint32_t *width, uint32_t *height) {
    (void) tile;
    int w = ((int) rdp.texture_tile.lrs - (int) rdp.texture_tile.uls + 4) / 4;
    int h = ((int) rdp.texture_tile.lrt - (int) rdp.texture_tile.ult + 4) / 4;
    *width = (uint32_t) (w < 1 ? 1 : w);
    *height = (uint32_t) (h < 1 ? 1 : h);
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
    if ((uintptr_t) src < 0x08000000u) {
        return 0;
    }
    // Texels inside the executable's text/data (ROM blobs, torch assets) never
    // change; only buffers in BSS / the game's pools need hashing.  A CI
    // texture still hashes a palette that lives in writable memory.
    if (gfx_is_static_memory(src)) {
        if (fmt == G_IM_FMT_CI && rdp.palette != NULL) {
            if (!gfx_is_static_memory(rdp.palette)) {
                return hash_bytes(h, rdp.palette, siz == G_IM_SIZ_4b ? 32 : 512) & ~1u; /* content: even */
            }
            // Static texels with a static palette: the palette's ADDRESS still
            // has to take part (Rainbow Road animates by cycling through
            // several TLUTs over the same CI texels).
            return (uint32_t) (uintptr_t) rdp.palette | 1u;
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
        h = hash_bytes(h, rdp.palette, siz == G_IM_SIZ_4b ? 32 : 512);
    }
    /* Content hashes are even and the static markers above odd, so a texture
     * whose memory stops being static (port_fb_tile_note) can never match
     * the marker it was cached under. */
    return h & ~1u;
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

/* SM64_TEXSTRESS=<n> (debug): a cache reset every n new imports, as a long session's full arena does
 * (OoT 2b69f7b's gfx_tex_stress): a reset only forces re-imports, so the frames must not change. */
static bool gfx_tex_stress_hold SAVESTATE_EXCLUDE; /* the re-run pass (gfx_cache_epoch): a real reset
                                                     * can't fire again on the arena it just emptied */
static bool gfx_tex_stress(void) {
    static int sEvery = -1, sCount;
    if (sEvery < 0) { const char *e = getenv("SM64_TEXSTRESS"); sEvery = e ? atoi(e) : 0; }
    return sEvery > 0 && !gfx_tex_stress_hold && ++sCount % sEvery == 0;
}

static bool gfx_texture_cache_lookup(int tile, struct TextureHashmapNode **n, const uint8_t *orig_addr, uint32_t fmt, uint32_t siz, uint8_t mirror) {
    size_t hash = (uintptr_t)orig_addr;
    uint32_t content_hash = gfx_texture_content_hash(tile, fmt, siz);
    uint32_t upload_bytes = gfx_texture_upload_bytes(tile, fmt, siz, mirror);
    struct TextureHashmapNode *stale = NULL;
    uint32_t view_w, view_h;
    const uint32_t view_stride = rdp.loaded_texture[gfx_tex_slot(tile)].stride_bytes;
    gfx_tile_size(tile, &view_w, &view_h);
    hash = (hash >> 5) & 0x3ff;
    struct TextureHashmapNode **node = &gfx_texture_cache.hashmap[hash];
    while (*node != NULL && *node - gfx_texture_cache.pool < (int)gfx_texture_cache.pool_pos) {
        if ((*node)->texture_addr == orig_addr && (*node)->fmt == fmt && (*node)->siz == siz && (*node)->mirror == mirror &&
            (*node)->view_w == view_w && (*node)->view_h == view_h && (*node)->view_stride == view_stride) {
            if ((*node)->content_hash == content_hash) {
                gfx_rapi->select_texture(tile, (*node)->texture_id);
                { /* the hit's own tile (it said 0 for either: a tile-1 hit set tile 0's wrap and filter; the
                   * matrix's X2). SM64 imports tile 1 zero times in the sweep. SM64_SAMPLERFIX=0: as before. */
                    static int sFix SAVESTATE_EXCLUDE = -1;
                    if (sFix < 0) { const char *e = getenv("SM64_SAMPLERFIX"); sFix = !(e && e[0] == '0'); }
                    gfx_rapi->set_sampler_parameters(sFix ? tile : 0, (*node)->linear_filter, (*node)->cms, (*node)->cmt);
                }
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
    if (src == NULL || (uintptr_t) src < 0x08000000u) {
        return;
    }
    if (stride == 0) {
        stride = tex_bytes_per_row(width, siz);
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

static inline float dot(const float a[3], const float b[3])
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

/* X31 (OoT 6f1ac0e): the RSP normalizes the light and look-at directions at any length. sm64-port
 * skipped a squared length under 1e-5, so a model drawn at a small scale (its modelview shrinks the
 * direction to ~0.003) got an unnormalized, near-zero direction: no diffuse light, no env-map shine.
 * Any non-zero length is normalized now; SM64_NORMFIX=0: the old threshold (A/B). */
static int sNormFix SAVESTATE_EXCLUDE = -1;
static void gfx_normalize_vector(float v[3]) {
    float dot = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (sNormFix < 0) { const char *e = getenv("SM64_NORMFIX"); sNormFix = !(e && e[0] == '0'); }
    if (sNormFix ? dot > 1e-30f : dot > 0.00001f) {
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

/* hh_game.h: a 320-wide screen x mapped like the world (identity at 4:3). */
float hh_crop_x(float x) {
    const float aspect = (float) gfx_current_dimensions.width / (float) gfx_current_dimensions.height;
    return aspect < 4.0f / 3.0f ? 160.0f + (x - 160.0f) * (4.0f / 3.0f) / aspect : x;
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
#elif defined(__ARM_NEON)
    /* The same once per batch for the handheld's NEON: the vertex stores go into rsp too, so the
     * compiler reloaded all 32 matrix floats for every vertex (M5 round 1). */
    const float (*const nMP)[4] = rsp.MP_matrix;
    const float (*const nMV)[4] = rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1];
    const float32x4_t mp0 = vld1q_f32(nMP[0]), mp1 = vld1q_f32(nMP[1]), mp2 = vld1q_f32(nMP[2]), mp3 = vld1q_f32(nMP[3]);
    const float32x4_t mv0 = vld1q_f32(nMV[0]), mv1 = vld1q_f32(nMV[1]), mv2 = vld1q_f32(nMV[2]), mv3 = vld1q_f32(nMV[3]);
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
#elif defined(__ARM_NEON)
        /* The scalar sum below, four columns at once and in its order: ((v0*M0 + v1*M1) + v2*M2)
         * + M3 (v3 = 1), each product rounded before its add (NEON's vmla is not fused, and the
         * scalar build has -ffp-contract=off), so the same bits. */
        {
            float32x4_t pv = vmulq_n_f32(mp0, temp_vec[0]), vv = vmulq_n_f32(mv0, temp_vec[0]);
            pv = vmlaq_n_f32(pv, mp1, temp_vec[1]); vv = vmlaq_n_f32(vv, mv1, temp_vec[1]);
            pv = vmlaq_n_f32(pv, mp2, temp_vec[2]); vv = vmlaq_n_f32(vv, mv2, temp_vec[2]);
            vst1q_f32(proj_vec, vaddq_f32(pv, mp3)); vst1q_f32(view_vec, vaddq_f32(vv, mv3));
        }
#else
        /* proj = MP * v (clip), view = MV * v.  N64 row-vector convention:
         * out[c] = sum_j v[j] * M[j][c] (matches the C reference at gfx_debug_frame). */
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
        const float x = (rsp.is_persp && !gfx_ui_active) ? gfx_adjust_x_for_aspect_ratio(proj_vec[0]) : proj_vec[0];
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
                /* Texgen axes: s from the task's LOOKATX, t from LOOKATY (the DMEM
                 * default or gSPLookAt), brought to model space like the lights --
                 * what the console's F3D microcode does (GoldenEye c68da63).
                 * sm64-port's fixed axes (s from eye x, t from eye y) transposed
                 * every env map. SM64_LOOKAT=0: those fixed axes (A/B). A zero
                 * dir gives n.l = 0, the texture's centre (ocarina-of-time), rather
                 * than normalising to NaN. */
                static const Light_t lookat_x = {{0, 0, 0}, 0, {0, 0, 0}, 0, {127, 0, 0}, 0};
                static const Light_t lookat_y = {{0, 0, 0}, 0, {0, 0, 0}, 0, {0, 127, 0}, 0};
                static int sLookat SAVESTATE_EXCLUDE = -1;
                if (sLookat < 0) { const char *e = getenv("SM64_LOOKAT"); sLookat = e ? atoi(e) : 1; }
                const Light_t *ls = sLookat ? &rsp.current_lookat[1] : &lookat_x;
                const Light_t *lt = sLookat ? &rsp.current_lookat[0] : &lookat_y;
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
#if defined(__ARM_NEON)
        if (rsp.is_persp) {
            /* The flags below as vector compares of (x, y, z, clip x) against w, the guard band
             * and the RSP's: one transfer out of NEON instead of eleven float compares, each
             * a vmrs stall on the A7 (M5 round 1). Same compares, same bits. */
            const float aw = w < 0.0f ? -w : w, gw = GUARD_BAND * aw, kw = (float) gfx_clip_ratio * aw;
            const float32x4_t pt = { x, y, z, proj_vec[0] };
            static const uint32_t bLw[4] = { X_OUT_L, Y_OUT_B, Z_POS, 0 }, bGw[4] = { X_OUT_R, Y_OUT_T, Z_NEG, 0 };
            static const uint32_t bLg[4] = { X_POS, Y_POS, 0, 0 }, bGg[4] = { X_NEG, Y_NEG, 0, 0 };
            static const uint32_t bK[4] = { 0, GB_OUT, 0, GB_OUT };
            uint32x4_t r = vandq_u32(vcltq_f32(pt, vdupq_n_f32(-w)), vld1q_u32(bLw));
            r = vorrq_u32(r, vandq_u32(vcgtq_f32(pt, vdupq_n_f32(w)), vld1q_u32(bGw)));
            r = vorrq_u32(r, vandq_u32(vcltq_f32(pt, vdupq_n_f32(-gw)), vld1q_u32(bLg)));
            r = vorrq_u32(r, vandq_u32(vcgtq_f32(pt, vdupq_n_f32(gw)), vld1q_u32(bGg)));
            r = vorrq_u32(r, vandq_u32(vorrq_u32(vcltq_f32(pt, vdupq_n_f32(-kw)), vcgtq_f32(pt, vdupq_n_f32(kw))), vld1q_u32(bK)));
            const uint32x2_t r2 = vorr_u32(vget_low_u32(r), vget_high_u32(r));
            d->clip_rej = vget_lane_u32(r2, 0) | vget_lane_u32(r2, 1);
        }
#else
        if (rsp.is_persp) {
            float aw = w < 0.0f ? -w : w;
            float gw = GUARD_BAND * aw;
            if (x < -gw) d->clip_rej |= X_POS;
            if (x > gw) d->clip_rej |= X_NEG;
            if (y < -gw) d->clip_rej |= Y_POS;
            if (y > gw) d->clip_rej |= Y_NEG;
            if (x < -w) d->clip_rej |= X_OUT_L;
            if (x > w) d->clip_rej |= X_OUT_R;
            if (y < -w) d->clip_rej |= Y_OUT_B;
            if (y > w) d->clip_rej |= Y_OUT_T;
            // In front of the game's near plane (z_ndc < -1), or behind the eye:
            // a triangle with all three vertices here is invisible (the RSP
            // clips it away), so gfx_sp_tri1's trivial reject may drop it. This
            // was w < W_NEAR (24) -- a clip hint, but the shared-bit reject
            // treated it as "outside", deleting every triangle within 24 units
            // of the camera: kart sprites vanished at ~26 units instead of at
            // the game's own ~10-unit cull (device report after v29).
            if (z < -w) d->clip_rej |= Z_POS;
            // Beyond the far plane (the RSP clips there). Was z > 2|w|, which a
            // standard perspective never reaches (z/w < (f+n)/(f-n) ~ 1.01), so
            // the far reject never fired (round 24). z > w is only ever true in
            // front of the eye: behind it z < w < 0.
            if (z > w) d->clip_rej |= Z_NEG;
            // Beyond the guard band, in the N64's clip space (proj_vec, not the
            // aspect-adjusted x): the RSP sends such a vertex's triangle to its clipper.
            {
                const float kw = (float) gfx_clip_ratio * aw;
                if (proj_vec[0] < -kw || proj_vec[0] > kw || y < -kw || y > kw) d->clip_rej |= GB_OUT;
            }
        }
#endif
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
#if !defined(PORT_GE_TL)
    if (!rsp.is_persp && n_vertices > 0 && hud_fit_mode()) { /* the HUD unsquished (hud_fit_mode): one placement a load */
        struct LoadedVertex *lv = &rsp.loaded_vertices[dest_index - n_vertices];
        const float vx = (float) rdp.viewport.x, vw = (float) rdp.viewport.width;
        const float vy = (float) rdp.viewport.y, vh = (float) rdp.viewport.height;
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        size_t i;
        for (i = 0; i < n_vertices; i++) { /* where the squeeze puts it, back in the 320x240 layout */
            const float w = lv[i]._w, inv = (w > 1e-6f || w < -1e-6f) ? 1.0f / w : 1.0f;
            const float lx = (vx + (lv[i]._x * inv * 0.5f + 0.5f) * vw) / RATIO_X;
            const float ly = (vy + (0.5f - lv[i]._y * inv * 0.5f) * vh) / RATIO_Y;
            if (lx < x0) x0 = lx;
            if (lx > x1) x1 = lx;
            if (ly < y0) y0 = ly;
            if (ly > y1) y1 = ly;
        }
        const struct HudPlace p = hud_fit_place(x0, x1, y0, y1);
        if (!p.full && vw > 0.0f) {
            for (i = 0; i < n_vertices; i++) {
                const float w = lv[i]._w, ww = (w > 1e-6f || w < -1e-6f) ? w : 1.0f, inv = 1.0f / ww;
                const float lx = (vx + (lv[i]._x * inv * 0.5f + 0.5f) * vw) / RATIO_X;
                lv[i]._x = ((hud_fit_x(lx, &p) - vx) / vw * 2.0f - 1.0f) * ww;
            }
        }
    }
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

/* Which channel groups (bit 0 RGB, bit 1 alpha) have the form (A - V) *
 * TEXEL0 + V with A a constant colour: the texel lerps V toward A per channel.
 * G_CC_HILITERGBA, (PRIM - SHADE) * TEXEL0 + SHADE: Mario's face shine on the
 * title (PRIM = the light colour, the texture a highlight spot). Evaluated with
 * texels as 1.0 it folds to PRIM, and the backend's modulate then drew the
 * shine texture times the light: a black face wherever the spot isn't. The
 * software backend lerps per pixel instead (gfx_soft sHilite) and needs V. */
static int cc_texlerp_mode(uint32_t cc_id) {
    int m = 0;
#if !defined(TARGET_PSP)
    for (int k = 0; k < 2; k++) {
        const uint32_t bits = cc_id >> (k * 12);
        const uint32_t a = bits & 7, b = (bits >> 3) & 7, c = (bits >> 6) & 7, d = (bits >> 9) & 7;
        if (!((cc_id >> (28 + k)) & 1) && !((cc_id >> (30 + k)) & 1) && c == CC_TEXEL0 && b == d && a != b &&
            (a == CC_PRIM || a == CC_ENV) && (b == CC_SHADE || b == CC_PRIM || b == CC_ENV)) {
            m |= 1 << k;
        }
    }
#else
    (void) cc_id;
#endif
    return m;
}

static void gfx_eval_vertex_color(uint32_t cc_id, const struct RGBA *shade, int lod, struct RGBA *out) {
    uint8_t *o = (uint8_t *) out;
    int ch;
    const int texlerp = cc_texlerp_mode(cc_id);
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
        if (texlerp & (1 << k)) v = d;
        o[ch] = (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
    }
    // An empty alpha combiner (alpha unused: cleared by the caller) means opaque.
    if ((cc_id & 0xfff000) == 0 && !(cc_id & (CC_FLAG_ALPHA_A_ONE | CC_FLAG_ALPHA_D_ONE))) {
        out->a = 255;
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
    {   /* Cutout triangles (G_RM_AA_ZB_TEX_EDGE: CVG_X_ALPHA, no FORCE_BL; SM64's LAYER_ALPHA -- trees,
         * fences, grates) are written unblended, as the RDP does: coverage comes from the texel's alpha
         * and the pixel is the texel's own colour or nothing (OoT 7120396). Blending by alpha mixed the
         * edge texels with whatever was behind at draw time. Triangles only: rects keep blending.
         * OFF by default: against the real game (m64ref z64, 27 same-instant sweep frames, the changed
         * pixels only) the edges moved further from it, mean |error| 36.4 -> 41.4 -- the N64's VI
         * anti-aliases coverage edges afterwards, which we don't, so the blend is the better stand-in
         * (Paper Mario found the same). SM64_EDGEOPAQUE=1: on. */
        static int sEdgeOp SAVESTATE_EXCLUDE = -1;
        extern void gfx_soft_set_edge_opaque(bool on);
        if (sEdgeOp < 0) { const char *e = getenv("SM64_EDGEOPAQUE"); sEdgeOp = e != NULL && e[0] == '1'; }
        const bool eo = sEdgeOp && texture_edge && !(rdp.other_mode_l & FORCE_BL);
        if (eo != rendering_state.edge_opaque) {
            gfx_flush();
            gfx_soft_set_edge_opaque(eo);
            rendering_state.edge_opaque = eo;
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
    for (int pass = 0; pass < 2; pass++) { /* again if a reset ran under the imports (gfx_cache_epoch) */
    const uint32_t epoch = gfx_cache_epoch;
    gfx_tex_stress_hold = pass > 0;
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
    gfx_tex_stress_hold = false;
    // The GE has one texture unit: whatever was bound last (possibly tile 1's
    // mip level, or a texture just uploaded) must give way to tile 0's texture.
    if (used_textures[0] && rendering_state.textures[0] != NULL) {
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
        tri_state.tex_add = add;
        if (add != rendering_state.tex_add) {
            extern void gfx_scegu_set_texfunc_add(bool on);
            gfx_flush();
            gfx_scegu_set_texfunc_add(add);
            rendering_state.tex_add = add;
        }
    }
#if !defined(TARGET_PSP)
    /* Texel lerp toward a constant (cc_texlerp_mode): hand the backend the
     * constant A of each channel group; the vertex colour carries V. */
    {
        const int hil = tri_state.use_texture ? cc_texlerp_mode(cc_id) : 0;
        uint8_t c[4] = { 0, 0, 0, 0 };
        if (hil & 1) {
            const struct RGBA *a = (cc_id & 7) == CC_PRIM ? &rdp.prim_color : &rdp.env_color;
            c[0] = a->r; c[1] = a->g; c[2] = a->b;
        }
        if (hil & 2) {
            c[3] = ((cc_id >> 12) & 7) == CC_PRIM ? rdp.prim_color.a : rdp.env_color.a;
        }
        if (hil != rendering_state.hilite || memcmp(c, rendering_state.hilite_c, 4) != 0) {
            extern void gfx_scegu_set_texfunc_hilite(int mode, const uint8_t c[4]);
            gfx_flush();
            gfx_scegu_set_texfunc_hilite(hil, c);
            rendering_state.hilite = hil;
            memcpy(rendering_state.hilite_c, c, 4);
        }
    }
    /* Fog. The kit folds it into the vertex colour: colour = lerp(colour, fog,
     * factor) before the texture multiply. That is exact only when nothing
     * per-pixel comes after it -- an untextured combiner -- or when the fog is
     * black and the texel just multiplies the vertex colour. SM64 has coloured
     * fogs (JRB's teal, LLL's orange, grey and near-white), where the fold
     * tinted the texture instead of fading it out, and decal-textured fogged
     * surfaces (37 in the data), whose combiner never reads the vertex colour,
     * so the fold dropped their fog entirely. Those fog per pixel instead: the
     * vertex alpha carries the fog factor (the RSP's shade alpha), and a
     * constant ENV/PRIM alpha it displaces goes to the backend (de0b5af).
     * The ports' one reference (MK64 7d23507: these rules on MK64 c400d1b's fast
     * paths; M4 round 2, the lead's (b)) also fogs per pixel an untextured draw
     * under coloured fog, and any opaque draw, and hands the backend a constant
     * alpha of 0 or 1 too. SM64_FOGPX: 0 always folds, 1 c400d1b's rule (coloured
     * fog only, the alpha from the texel or unused), 2 the reference (default),
     * 3 de0b5af's rule (this port's before). */
    {
        static int sFogRule SAVESTATE_EXCLUDE = -1;
        if (sFogRule < 0) {
            const char *e = getenv("SM64_FOGPX");
            sFogRule = (e && e[0] >= '0' && e[0] <= '3' && !e[1]) ? e[0] - '0' : 2;
        }
        bool fold = true, raw_a = false;
        int const_a = -1;
#ifdef HH_CENSUS
        int cen = 0; bool cen_diff = false;
#endif
        if (tri_state.use_fog && sFogRule) {
            const uint32_t rgb = cc_id & 0xfff, al = (cc_id >> 12) & 0xfff;
            const uint32_t ra = rgb & 7, rb = (rgb >> 3) & 7, rc = (rgb >> 6) & 7, rd = (rgb >> 9) & 7;
            const bool in_a = ra == CC_PRIM || ra == CC_SHADE || ra == CC_ENV;
            const bool in_c = rc == CC_PRIM || rc == CC_SHADE || rc == CC_ENV;
            const bool tex_x_input = rb == CC_0 && rd == CC_0 && !((cc_id >> 28) & 1) && !((cc_id >> 30) & 1) &&
                                     ((ra == CC_TEXEL0 && in_c) || (rc == CC_TEXEL0 && in_a));
            const bool black = (rdp.fog_color.r | rdp.fog_color.g | rdp.fog_color.b) == 0;
            bool a_shade = false, a_tex = false, a_other = false; /* the alpha combiner's inputs (cleared when opaque) */
            for (int k = 0; k < 4; k++) {
                const uint32_t code = (al >> (k * 3)) & 7;
                if (code == CC_SHADE) a_shade = true;
                else if (code == CC_TEXEL0 || code == CC_TEXEL1 || code == CC_TEXEL0A) a_tex = true;
                else if (code != CC_0) a_other = true;
            }
            bool r3_fold = true, r3_raw = false, r3_const = false; /* de0b5af's rule */
            if (tri_state.use_texture && !(black && tex_x_input)) {
                if (a_shade && !a_other) {
                    r3_fold = false;               /* the vertex alpha is the fog factor already */
                } else if (!a_shade && !(a_tex && a_other)) {
                    r3_fold = false; r3_raw = true; /* texel-only or constant alpha: carry the factor raw */
                    r3_const = !a_tex && a_other;  /* constant alpha (ENV/PRIM): its value */
                }
            }
            bool r2_fold = true, r2_raw = false, r2_const = false; /* the reference */
            if (!(black && (!tri_state.use_texture || tex_x_input))) {
                if (!use_alpha) {
                    r2_fold = false; r2_raw = true;
                } else if (a_shade ? !a_other : !(a_tex && a_other)) {
                    r2_fold = false; r2_raw = !a_shade; /* a shade alpha: the combined vertex alpha is the factor */
                    r2_const = !a_shade && !a_tex;      /* PRIM/ENV alpha, or 0/1 */
                }
            }
            bool want_const = false;
            if (sFogRule == 1) {
                fold = !(!black && !a_shade && !a_other && (!use_alpha || a_tex)); raw_a = !fold;
            } else if (sFogRule == 3) {
                fold = r3_fold; raw_a = r3_raw; want_const = r3_const;
            } else {
                fold = r2_fold; raw_a = r2_raw; want_const = r2_const;
            }
            if (want_const) {
                struct RGBA dummy = { 0, 0, 0, 0 }, o;
                gfx_eval_vertex_color(cc_id, &dummy, 0, &o);
                const_a = o.a;
            }
#ifdef HH_CENSUS
            /* 1 fold, 2 opaque, 3 shade alpha, 4 texel alpha, 5 constant alpha; +8 untextured; +16 decal */
            cen = r2_fold ? 1 : !use_alpha ? 2 : a_shade ? 3 : a_tex ? 4 : 5;
            if (!tri_state.use_texture) cen += 8;
            if (ra == CC_0 && rb == CC_0 && rc == CC_0 && rd == CC_TEXEL0) cen += 16;
            cen_diff = r2_fold != r3_fold || r2_raw != r3_raw || r2_const != r3_const;
#endif
        }
#ifdef HH_CENSUS
        tri_state.fog_census = (uint8_t) cen; tri_state.fog_census_diff = cen_diff;
#endif
        tri_state.fog_fold = fold;
        tri_state.fog_raw_alpha = raw_a;
        const bool pix = tri_state.use_fog && !fold;
        if (pix != rendering_state.fog_pix || (pix && (memcmp(rendering_state.fog_c, &rdp.fog_color, 3) != 0 ||
                                                       const_a != rendering_state.fog_a))) {
            extern void gfx_scegu_set_fog(bool on, uint8_t r, uint8_t g, uint8_t b, int const_a);
            gfx_flush();
            gfx_scegu_set_fog(pix, rdp.fog_color.r, rdp.fog_color.g, rdp.fog_color.b, const_a);
            rendering_state.fog_pix = pix;
            memcpy(rendering_state.fog_c, &rdp.fog_color, 3);
            rendering_state.fog_a = const_a;
        }
    }
    {
        /* (TEXEL1 - TEXEL0) * LOD_FRACTION + TEXEL0 (G_CC_TRILERP); the fraction
         * is set per triangle in gfx_sp_tri1. */
        const uint32_t rgb = cc_id & 0xfff;
        tri_state.trilerp = tri_state.use_texture && !((cc_id >> 28) & 1) && !((cc_id >> 30) & 1) &&
                            (rgb & 7) == CC_TEXEL1 && ((rgb >> 3) & 7) == CC_TEXEL0 &&
                            ((rgb >> 6) & 7) == CC_LOD && ((rgb >> 9) & 7) == CC_TEXEL0;
        if (!tri_state.trilerp && rendering_state.trilerp) {
            extern void gfx_scegu_set_trilerp(bool on, int f);
            gfx_flush();
            gfx_scegu_set_trilerp(false, 0);
            rendering_state.trilerp = false;
        }
    }
#endif
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
        /* z/w is -1 (near) .. 1 (far), as the RSP's viewport maps it (vscale =
         * vtrans = G_MAXZ/2); the rasterizer's z-buffer is 0..1. Round 24 found
         * the kit passing z/w straight through: everything nearer than z/w = 0
         * (2*near*far/(far+near) ~ 2x the near plane: ~200 units in SM64, ~18
         * in MK64) clamped to depth 0 and lost its ordering. */
        out->z = cv->_z * inv * 0.5f + 0.5f;
#ifdef HH_CLIPCHECK
        { /* debug: vertices reaching the rasterizer outside the depth range */
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
            gfx_eval_vertex_color(cc_id, &cv->color, lod, &out->color);
            break;
    }
    if (tri_state.use_fog && !tri_state.fog_fold) {
        // Per-pixel fog in the backend (gfx_tri_rebuild_state): it reads the
        // fog factor from the vertex alpha.
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

/* The RDP's LOD fraction for a TRILERP triangle (G_TL_LOD, two levels, no
 * sharpen/detail): lod = the largest texture-coordinate step per N64 screen
 * pixel; magnified (lod < 1) -> 0, tile 0 alone; lod >= 2 (the last level) ->
 * 255, tile 1; between, lod - 1. The N64 works it out per pixel; one affine
 * estimate per triangle is enough for the one user, the castle's stained glass
 * (Bowser near, Peach far). Pixels are counted at the N64's scale, so the swap
 * happens at the N64's distance whatever the 3D resolution. */
static Vp_t gfx_raw_vp;
static bool gfx_raw_vp_valid;
static int gfx_trilerp_frac(struct LoadedVertex *const *v) {
    float px[3], py[3], s[3], t[3];
    const float kx = rsp.is_persp && !gfx_ui_active ? gfx_adjust_x_for_aspect_ratio(1.0f) : 1.0f;
    for (int i = 0; i < 3; i++) {
        const float w = v[i]->_w != 0.0f ? v[i]->_w : 1e-6f;
        px[i] = (v[i]->_x / w) * 0.5f * (float) rdp.viewport.width;
        py[i] = (v[i]->_y / w) * 0.5f * (float) rdp.viewport.height;
        s[i] = v[i]->u / 32.0f;
        t[i] = v[i]->v / 32.0f;
    }
    const float dx1 = px[1] - px[0], dy1 = py[1] - py[0], dx2 = px[2] - px[0], dy2 = py[2] - py[0];
    const float det = dx1 * dy2 - dx2 * dy1;
    if (det > -1e-6f && det < 1e-6f) return 255;
    const float ds1 = s[1] - s[0], ds2 = s[2] - s[0], dt1 = t[1] - t[0], dt2 = t[2] - t[0];
    /* our pixels per N64 pixel: our viewport over the game's own, times the
     * square screen's crop in x (_x carries it: 1:1 with the N64 on a square output) */
    const float n64w = gfx_raw_vp_valid ? 2.0f * gfx_raw_vp.vscale[0] / 4.0f : 320.0f;
    const float n64h = gfx_raw_vp_valid ? 2.0f * gfx_raw_vp.vscale[1] / 4.0f : 240.0f;
    const float sx = (float) rdp.viewport.width * kx / n64w, sy = (float) rdp.viewport.height / n64h;
    float m = 0.0f, d;
    d = fabsf((ds1 * dy2 - ds2 * dy1) / det) * sx; if (d > m) m = d; /* ds/dx per N64 px */
    d = fabsf((dt1 * dy2 - dt2 * dy1) / det) * sx; if (d > m) m = d;
    d = fabsf((ds2 * dx1 - ds1 * dx2) / det) * sy; if (d > m) m = d; /* ds/dy */
    d = fabsf((dt2 * dx1 - dt1 * dx2) / det) * sy; if (d > m) m = d;
    const float f = m - 1.0f;
    return f <= 0.0f ? 0 : f >= 1.0f ? 255 : (int) (f * 255.0f + 0.5f);
}

#ifdef HH_CENSUS
/* HH_CENSUS (a counter build, like HH_DXTCOUNT; tools/census.sh builds and runs
 * it): GoldenEye's renderer census, the cross-port lead's item 5 -- which of
 * GoldenEye's kit fixes SM64's display lists would hit. The same counts as OoT's
 * OOT_CCCENSUS, plus cycle 2 (this renderer decodes cycle 1 only). Printed at
 * exit (SM64_EXIT_AT). SM64's G_RDPHALF_1s are Fast3D's gSPPerspNormalize, not
 * raw RDP triangles; the first few are logged with their neighbours. */
static uint8_t sCenCc[16]; /* the raw SETCOMBINE muxes: cycle 1 rgb a b c d, alpha a b c d; cycle 2 the same */
static struct { uint8_t m[16]; uint8_t cyc, why; uint32_t tris, quads; } sCen[64];
static int sCenN;
static struct { uint32_t oml, omh, tris, quads; } sCenNfb[32];
static int sCenNfbN;
static uint32_t sCenTris, sCenQuads, sCenNfbTris, sCenNfbQuads, sCenCiTex, sCenIa16Tex;
static uint32_t sCenTlutLoads, sCenTlutOffset, sCenTexrects, sCenTexrectFrac, sCenRdpHalf, sCenRawTri;
/* R34 (Smash 3b03a66): draws while the colour image is the depth image (they never reach the screen) */
static uint32_t sCenZTexrects, sCenZFills, sCenZTris;
static uint32_t sCenScissors, sCenScissorFrac, sCenScissorInv, sCenBlockDraws, sCenBlockScroll;
/* Paper Mario aa512fd (R24-R27) and OoT's tile shift (R29): */
static uint32_t sCenTileDraws, sCenTileScroll;       /* R24 for LoadTile tiles */
static uint32_t sCenC2TexDraws;                      /* R25: 2-cycle draws whose cycle 2 reads a texel */
static uint32_t sCenLoadTiles, sCenLoadTileDiff, sCenR26Draws; /* R26 */
static bool sCenRtDirty;                             /* the render tile's corners came from a LoadTile */
static uint16_t sCenLoadTmem[2], sCenLoadWords[2];  /* R27: each slot's last load, TMEM words */
static bool sCenSlotStale[2];                        /* ... overwritten since by a load into the other slot */
static uint32_t sCenR27Offset, sCenR27Stale;
static uint8_t sCenShift[8];                         /* R29: shifts | shiftt << 4, per tile */
static uint32_t sCenShiftTris, sCenShift1Tris;
static uint32_t sCenFog[32], sCenFogDiff[32];        /* fogged 3D triangles by class (gfx_tri_rebuild_state) */
static void hh_census_print(void) {
    int i, j;
    port_log("census: %u tris, %u 2D quads\n", (unsigned) sCenTris, (unsigned) sCenQuads);
    port_log("census: blend 1-A without FORCE_BL: %u tris, %u quads\n", (unsigned) sCenNfbTris, (unsigned) sCenNfbQuads);
    for (i = 0; i < sCenNfbN; i++) {
        port_log("census:   nofbl othermode_l %08X h %08X: %u tris %u quads\n", (unsigned) sCenNfb[i].oml,
                 (unsigned) sCenNfb[i].omh, (unsigned) sCenNfb[i].tris, (unsigned) sCenNfb[i].quads);
    }
    port_log("census: 1/2-cycle texrects %u, with a fractional edge %u\n", (unsigned) sCenTexrects, (unsigned) sCenTexrectFrac);
    port_log("census: R34 into the depth image: texrects %u, fill rects %u, tris %u\n", (unsigned) sCenZTexrects, (unsigned) sCenZFills, (unsigned) sCenZTris);
    port_log("census: LOADTLUTs %u, with a start texel %u\n", (unsigned) sCenTlutLoads, (unsigned) sCenTlutOffset);
    port_log("census: textured draws with a CI texture %u, under an IA16 TLUT %u\n", (unsigned) sCenCiTex, (unsigned) sCenIa16Tex);
    port_log("census: G_RDPHALF_1/2/CONT outside texrects %u, raw RDP triangle commands %u\n", (unsigned) sCenRdpHalf, (unsigned) sCenRawTri);
    port_log("census: scissors %u, with a fractional edge on the target %u\n", (unsigned) sCenScissors, (unsigned) sCenScissorFrac);
    port_log("census: inverted scissors %u\n", (unsigned) sCenScissorInv);
    {
        static const char *const cls[8] = { "?", "folded", "opaque", "shade alpha", "texel alpha", "constant alpha", "?", "?" };
        for (i = 1; i < 32; i++) {
            if (sCenFog[i]) port_log("census: fog %s%s%s: %u tris, the reference decides otherwise than de0b5af on %u\n", cls[i & 7],
                                     (i & 8) ? ", untextured" : "", (i & 16) ? ", decal" : "", (unsigned) sCenFog[i], (unsigned) sCenFogDiff[i]);
        }
    }
    port_log("census: R24 textured draws from a LoadTile tile %u, wrapping under the mask period %u\n", (unsigned) sCenTileDraws, (unsigned) sCenTileScroll);
    port_log("census: R25 2-cycle draws whose cycle 2 reads TEXEL0/1 %u\n", (unsigned) sCenC2TexDraws);
    port_log("census: R26 LoadTiles %u, rectangle unlike the render tile's %u, textured draws with LoadTile corners %u\n",
             (unsigned) sCenLoadTiles, (unsigned) sCenLoadTileDiff, (unsigned) sCenR26Draws);
    port_log("census: R27 textured draws from a tile inside a load at an offset %u, from an overwritten slot %u\n",
             (unsigned) sCenR27Offset, (unsigned) sCenR27Stale);
    port_log("census: R29 3D triangles with a render-tile shift %u, with a tile-1 shift under a TEXEL1 combiner %u\n",
             (unsigned) sCenShiftTris, (unsigned) sCenShift1Tris);
    port_log("census: textured draws from a LoadBlock tile %u, wrapping with a corner-to-corner size under the mask period %u\n",
             (unsigned) sCenBlockDraws, (unsigned) sCenBlockScroll);
    for (i = 0; i < sCenN; i++) {
        for (j = i + 1; j < sCenN; j++) {
            if (sCen[j].tris + sCen[j].quads > sCen[i].tris + sCen[i].quads) {
                const __typeof__(sCen[0]) t = sCen[i];
                sCen[i] = sCen[j];
                sCen[j] = t;
            }
        }
        port_log("census: %7u tris %5u quads cyc%u %s%s c1 rgb %u %u %u %u a %u %u %u %u | c2 rgb %u %u %u %u a %u %u %u %u\n",
                 (unsigned) sCen[i].tris, (unsigned) sCen[i].quads, (unsigned) sCen[i].cyc + 1,
                 (sCen[i].why & 1) ? "c1-input " : "", (sCen[i].why & 2) ? "c2-not-pass " : "",
                 sCen[i].m[0], sCen[i].m[1], sCen[i].m[2], sCen[i].m[3], sCen[i].m[4], sCen[i].m[5], sCen[i].m[6], sCen[i].m[7],
                 sCen[i].m[8], sCen[i].m[9], sCen[i].m[10], sCen[i].m[11], sCen[i].m[12], sCen[i].m[13], sCen[i].m[14], sCen[i].m[15]);
    }
}
static void hh_census_combine(uint32_t w0, uint32_t w1) {
    static bool sInit;
    if (!sInit) { sInit = true; atexit(hh_census_print); }
    sCenCc[0] = (w0 >> 20) & 15; sCenCc[1] = (w1 >> 28) & 15; sCenCc[2] = (w0 >> 15) & 31; sCenCc[3] = (w1 >> 15) & 7;
    sCenCc[4] = (w0 >> 12) & 7;  sCenCc[5] = (w1 >> 12) & 7;  sCenCc[6] = (w0 >> 9) & 7;   sCenCc[7] = (w1 >> 9) & 7;
    sCenCc[8] = (w0 >> 5) & 15;  sCenCc[9] = (w1 >> 24) & 15; sCenCc[10] = w0 & 31;        sCenCc[11] = (w1 >> 6) & 7;
    sCenCc[12] = (w1 >> 21) & 7; sCenCc[13] = (w1 >> 3) & 7;  sCenCc[14] = (w1 >> 18) & 7; sCenCc[15] = w1 & 7;
}
/* One draw: a 3D triangle (quad = false) or a 2D rectangle's quad. */
static void hh_census_draw(bool quad) {
    const uint8_t *m = sCenCc;
    const uint32_t cyc = (rdp.other_mode_h >> G_MDSFT_CYCLETYPE) & 3;
    const uint32_t oml = rdp.other_mode_l;
    uint8_t why = 0;
    int i;
    if (quad) sCenQuads++; else sCenTris++;
    if (cyc > G_CYC_2CYCLE >> G_MDSFT_CYCLETYPE) return; /* COPY / FILL: no combiner, no blender */
    { /* the texture: CI under an IA16 TLUT (GE ac5edfd) -- only if the combiner reads a texel */
        const bool tex = m[0] == 1 || m[0] == 2 || m[1] == 1 || m[1] == 2 || m[2] == 1 || m[2] == 2 || m[2] == 8 ||
                         m[2] == 9 || m[3] == 1 || m[3] == 2 || m[4] == 1 || m[4] == 2 || m[5] == 1 || m[5] == 2 ||
                         m[6] == 1 || m[6] == 2 || m[7] == 1 || m[7] == 2;
        if (tex && rdp.texture_tile.fmt == G_IM_FMT_CI) {
            sCenCiTex++;
            if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTLUT)) == G_TT_IA16) sCenIa16Tex++;
        }
        if (tex) {
            /* a tile scrolled by its upper-left corner alone (Star Fox 7725ecc,
             * R24; LoadTile tiles too, PM64): wrapping, sized corner to corner
             * under its mask period */
            const bool block = rdp.loaded_texture[gfx_tex_slot(0)].stride_bytes == 0;
            const int w = ((int) rdp.texture_tile.lrs - (int) rdp.texture_tile.uls + 4) / 4;
            const int h = ((int) rdp.texture_tile.lrt - (int) rdp.texture_tile.ult + 4) / 4;
            const int ms = rdp.texture_tile.masks, mt = rdp.texture_tile.maskt;
            if (block) sCenBlockDraws++; else sCenTileDraws++;
            if ((ms && ms < 16 && !(rdp.texture_tile.cms & G_TX_CLAMP) && w < (1 << ms)) ||
                (mt && mt < 16 && !(rdp.texture_tile.cmt & G_TX_CLAMP) && h < (1 << mt))) {
                static uint16_t sLast[4];
                if (block) sCenBlockScroll++; else sCenTileScroll++;
                if (memcmp(sLast, &rdp.texture_tile.uls, sizeof(sLast)) != 0) {
                    memcpy(sLast, &rdp.texture_tile.uls, sizeof(sLast));
                    port_log("census: %s tile %d,%d - %d,%d (x4) masks %d/%d cm %d/%d: %dx%d under the period\n",
                             block ? "block" : "loadtile", rdp.texture_tile.uls, rdp.texture_tile.ult, rdp.texture_tile.lrs,
                             rdp.texture_tile.lrt, ms, mt, rdp.texture_tile.cms, rdp.texture_tile.cmt, w, h);
                }
            }
            if (sCenRtDirty) sCenR26Draws++; /* R26: the render tile sized by a LoadTile's rectangle */
            { /* R27: the render tile starts inside its slot's load at an offset, or reads
               * a slot a later load overwrote */
                const int slot = rdp.texture_tile.tmem_slot;
                const uint16_t t = rdp.tile_tmem[G_TX_RENDERTILE];
                if (t != sCenLoadTmem[slot] && t > sCenLoadTmem[slot] && t < sCenLoadTmem[slot] + sCenLoadWords[slot]) {
                    static uint16_t sLastT = 0xFFFF;
                    sCenR27Offset++;
                    if (t != sLastT) { sLastT = t; port_log("census: R27 render tile at TMEM %u inside a load at %u (%u words)\n", t, sCenLoadTmem[slot], sCenLoadWords[slot]); }
                }
                if (sCenSlotStale[slot]) sCenR27Stale++;
            }
            if (!quad && (sCenShift[G_TX_RENDERTILE] != 0)) { /* R29 */
                static uint8_t sLastS;
                sCenShiftTris++;
                if (sCenShift[G_TX_RENDERTILE] != sLastS) { sLastS = sCenShift[G_TX_RENDERTILE]; port_log("census: R29 render tile shift s %u t %u\n", sLastS & 15, sLastS >> 4); }
            }
            if (!quad && (m[0] == 2 || m[1] == 2 || m[2] == 2 || m[2] == 9 || m[3] == 2 || m[4] == 2 || m[5] == 2 ||
                          m[6] == 2 || m[7] == 2) && sCenShift[G_TX_RENDERTILE + 1] != sCenShift[G_TX_RENDERTILE]) {
                sCenShift1Tris++;
            }
        }
        if (cyc == 1) { /* R25: cycle 2 reads a texel (its TEXEL0 is this pixel's TEXEL1 on the RDP) */
            const bool t2 = m[8] == 1 || m[8] == 2 || m[9] == 1 || m[9] == 2 || m[10] == 1 || m[10] == 2 ||
                            m[10] == 8 || m[10] == 9 || m[11] == 1 || m[11] == 2 || m[12] == 1 || m[12] == 2 ||
                            m[13] == 1 || m[13] == 2 || m[14] == 1 || m[14] == 2 || m[15] == 1 || m[15] == 2;
            if (t2) {
                static uint8_t sLastC2[8];
                sCenC2TexDraws++;
                if (memcmp(sLastC2, m + 8, 8) != 0) {
                    memcpy(sLastC2, m + 8, 8);
                    port_log("census: R25 cycle 2 reads a texel: c1 %u %u %u %u %u %u %u %u c2 %u %u %u %u %u %u %u %u\n", m[0], m[1], m[2], m[3], m[4],
                             m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
                }
            }
        }
    }
    { /* the blender: B = 1-A in the last cycle without FORCE_BL (GE cb5c863); the
       * texture-edge modes keep their alpha either way */
        const uint32_t b = cyc == 1 ? (oml >> 16) & 3 : (oml >> 18) & 3;
        if (b == G_BL_1MA && !(oml & FORCE_BL) && !(oml & CVG_X_ALPHA)) {
            if (quad) sCenNfbQuads++; else sCenNfbTris++;
            for (i = 0; i < sCenNfbN && !(sCenNfb[i].oml == oml && sCenNfb[i].omh == rdp.other_mode_h); i++) {
            }
            if (i == sCenNfbN && sCenNfbN < 32) {
                sCenNfb[i].oml = oml; sCenNfb[i].omh = rdp.other_mode_h; sCenNfb[i].tris = sCenNfb[i].quads = 0;
                sCenNfbN++;
            }
            if (i < sCenNfbN) { if (quad) sCenNfb[i].quads++; else sCenNfb[i].tris++; }
        }
    }
    /* cycle 1: an input color_comb_component drops, where it matters (OoT's
     * rule, plus SCALE); cycle 2 (2-cycle only): anything but a pass-through */
    if ((m[0] != m[1] && (m[2] == G_CCMUX_COMBINED_ALPHA || m[2] == G_CCMUX_TEXEL1_ALPHA || m[2] == G_CCMUX_PRIMITIVE_ALPHA ||
                          m[2] == G_CCMUX_SHADE_ALPHA || m[2] == G_CCMUX_ENV_ALPHA || m[2] == G_CCMUX_PRIM_LOD_FRAC ||
                          m[2] == G_CCMUX_K5 || m[2] == G_CCMUX_SCALE)) ||
        (m[2] != (G_CCMUX_0 & 31) && (m[1] == G_CCMUX_CENTER || m[1] == G_CCMUX_K4 || m[0] == G_CCMUX_NOISE)) ||
        (m[4] != m[5] && (m[6] == G_ACMUX_LOD_FRACTION || m[6] == G_ACMUX_PRIM_LOD_FRAC))) {
        why |= 1;
    }
    if (cyc == 1 && !(m[11] == G_CCMUX_COMBINED && (m[10] == (G_CCMUX_0 & 31) || m[8] == m[9]) &&
                      m[15] == G_ACMUX_COMBINED && (m[14] == G_ACMUX_0 || m[12] == m[13]))) {
        why |= 2;
    }
    if (!why) return;
    for (i = 0; i < sCenN; i++) {
        if (sCen[i].cyc == cyc && memcmp(sCen[i].m, m, cyc == 1 ? 16 : 8) == 0) break;
    }
    if (i == sCenN) {
        if (sCenN == (int) (sizeof(sCen) / sizeof(sCen[0]))) return;
        memcpy(sCen[i].m, m, 16);
        if (cyc != 1) memset(sCen[i].m + 8, 0, 8);
        sCen[i].cyc = (uint8_t) cyc; sCen[i].why = why; sCen[i].tris = sCen[i].quads = 0;
        sCenN++;
    }
    if (quad) sCen[i].quads++; else sCen[i].tris++;
}
#endif

static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
#ifdef PORT_EXP_NOTRI
    return;
#endif
#ifdef HH_CENSUS
    hh_census_draw(false);
    if (rdp.color_image_address == rdp.z_buf_address) sCenZTris++;
#endif
#ifdef PORT_PROFILE_DL
    uint32_t _pt0 = port_time_us();
#endif
    struct LoadedVertex *v1 = &rsp.loaded_vertices[vtx1_idx];
    struct LoadedVertex *v2 = &rsp.loaded_vertices[vtx2_idx];
    struct LoadedVertex *v3 = &rsp.loaded_vertices[vtx3_idx];
    struct LoadedVertex *v_arr[3] = {v1, v2, v3};

    if (v1->clip_rej & v2->clip_rej & v3->clip_rej & ~GB_OUT) {
        // The whole triangle lies outside the visible area
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
     * After the phase bookkeeping, so target selection is unchanged. */
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
            // Clip when a vertex is in front of the game's near plane (or
            // behind the eye: both set Z_POS) or beyond its far plane (Z_NEG),
            // as the RSP does. MK64 triggered on w < 2, or w < 24 and past far:
            // MK64 distances (near plane 9); SM64's near plane is 100, so a
            // wall 2..100 units from the camera would draw at depth 0 over
            // everything instead of being cut away (round 24). The RSP also
            // clips a triangle with a vertex beyond its guard band (GB_OUT;
            // gfx_clip_trigger, M4 round 2).
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
#ifdef HH_CENSUS
    if (tri_state.fog_census) { sCenFog[tri_state.fog_census]++; if (tri_state.fog_census_diff) sCenFogDiff[tri_state.fog_census]++; }
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
#if !defined(TARGET_PSP)
    if (tri_state.trilerp) {
        const int f = gfx_trilerp_frac(clipped_vertices);
        if (!rendering_state.trilerp || f != rendering_state.trilerp_f) {
            extern void gfx_scegu_set_trilerp(bool on, int f);
            gfx_flush();
            gfx_scegu_set_trilerp(true, f);
            rendering_state.trilerp = true;
            rendering_state.trilerp_f = f;
        }
    }
#endif
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
#ifdef HH_CENSUS
    hh_census_draw(true);
#endif
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
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    /* COPY mode has no blender: an alpha-compare threshold keeps or drops each
     * texel whole, so texels with a clear alpha bit are skipped, not painted
     * (ocarina-of-time 0a3cadd, f-zero-x's fix). SM64's dl_hud_img_begin is
     * COPY + G_AC_THRESHOLD and, in the US version, sets no render mode: the
     * HUD's alpha depended on whatever the last draw left. Alpha on, not the
     * texture-edge cut: on the 240-wide screen the HUD is squeezed through the
     * area filter, and the edge cut (alpha < 0x55) thinned its antialiased
     * outlines; clear texels still drop (alpha < 8). */
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY &&
        (rdp.other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_THRESHOLD) {
        use_alpha = true;
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
    
    if (gfx_trace_frames > 0) port_log("  2d: cc %08X\n", (unsigned) cc_id);
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
    
    for (int pass = 0; pass < 2; pass++) { /* again if a reset ran under the imports (gfx_cache_epoch) */
    const uint32_t epoch = gfx_cache_epoch;
    gfx_tex_stress_hold = pass > 0;
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
    gfx_tex_stress_hold = false;
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
    /* A wrapping tile (no clamp, mask m) reads coordinate & (2^m - 1) (twice
     * that period mirrored), so a texrect may start whole periods before its
     * tile's origin, while the samplers wrap by the imported size -- smaller on
     * a tile narrower than its period. Both corners shift by the same whole
     * number of periods so the first lands in [0, period) (paper-mario 525cb47:
     * Paper Mario's letter pieces read the wrong column and row). */
    int wrap_u = 0, wrap_v = 0;
    
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
                    wrap_u = (u >= 0 ? u / p : -((p - 1 - u) / p)) * p;
                }
                if (!(rdp.texture_tile.cmt & G_TX_CLAMP) && rdp.texture_tile.maskt) {
                    const int p = (1 << rdp.texture_tile.maskt) << ((rdp.texture_tile.cmt & G_TX_MIRROR) ? 1 : 0);
                    wrap_v = (v >= 0 ? v / p : -((p - 1 - v) / p)) * p;
                }
            }
            u -= wrap_u;
            v -= wrap_v;
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
        /* gSPLookAt: the texgen axes (GoldenEye c68da63, checked against the
         * console's own microcode in mupen64plus's LLE RSP). */
        case G_MV_LOOKATY:
        case G_MV_LOOKATX:
            memcpy(rsp.current_lookat + (index - G_MV_LOOKATY) / 2, data, sizeof(Light_t));
            rsp.lights_changed = true;
            break;
#endif
#ifdef F3DEX_GBI_2
        case G_MV_LIGHT: {
            int lightidx = offset / 24 - 2;
            if (lightidx >= 0 && lightidx <= MAX_LIGHTS) {
                // NOTE: reads out of bounds if it is an ambient light
                memcpy(rsp.current_lights + lightidx, data, sizeof(Light_t));
            } else if (lightidx == -2 || lightidx == -1) { // G_MVO_LOOKATX, G_MVO_LOOKATY
                memcpy(rsp.current_lookat + (lightidx == -2 ? 1 : 0), data, sizeof(Light_t));
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

static void gfx_sp_moveword(uint8_t index, uint16_t offset, uint32_t data) {
    switch (index) {
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
        case G_MW_CLIP:
            /* gSPClipRatio: the word at RNX holds k (FR_NEG_FRUSTRATIO_k); the other
             * three hold the same ratio for the other sides. */
            if (offset == G_MWO_CLIP_RNX && (int16_t) data >= 1 && (int16_t) data <= 6) gfx_clip_ratio = (int16_t) data;
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
    // The same one-short rule for the scissor (DKR clips to 0,0,319,239 every
    // frame and lost column and row 239), before the raw copy the viewport
    // re-applies is kept.
    if (lrx >= (SCREEN_WIDTH - 1) << 2 && lrx < SCREEN_WIDTH << 2) lrx = SCREEN_WIDTH << 2;
    if (lry >= (SCREEN_HEIGHT - 1) << 2 && lry < SCREEN_HEIGHT << 2) lry = SCREEN_HEIGHT << 2;

    float x = ulx / 4.0f * RATIO_X;
#if defined(TARGET_HANDHELD)
    float y = uly / 4.0f * RATIO_Y;                 /* top-down rasterizer: see viewport note */
#else
    float y = (SCREEN_HEIGHT - lry / 4.0f) * RATIO_Y;
#endif
    /* The size from signed differences, clamped at 0: an inverted scissor (lr
     * left of or above ul) passes nothing on the RDP, but the unsigned difference
     * wrapped to ~1e9 and the soft backend drew from the scissor's top edge to
     * the bottom of the screen (f-zero-x cf242ea). */
    float width = ((int32_t) lrx - (int32_t) ulx) / 4.0f * RATIO_X;
    float height = ((int32_t) lry - (int32_t) uly) / 4.0f * RATIO_Y;
    if (width < 0.0f) width = 0.0f;
    if (height < 0.0f) height = 0.0f;

#ifdef HH_CENSUS
    /* a scissor edge between target pixels (GoldenEye c68da63: the int16 fields
     * truncate x and width apart, losing the last column); an inverted one */
    sCenScissors++;
    if (lrx < ulx || lry < uly) {
        if (++sCenScissorInv <= 16) port_log("census: inverted scissor %u,%u - %u,%u (x4)\n", (unsigned) ulx, (unsigned) uly, (unsigned) lrx, (unsigned) lry);
    }
    if (x != floorf(x) || y != floorf(y) || x + width != floorf(x + width) || y + height != floorf(y + height)) {
        static uint32_t sLast[4];
        sCenScissorFrac++;
        if (sLast[0] != ulx || sLast[1] != uly || sLast[2] != lrx || sLast[3] != lry) {
            sLast[0] = ulx; sLast[1] = uly; sLast[2] = lrx; sLast[3] = lry;
            port_log("census: fractional scissor %u,%u - %u,%u (x4) -> x %.2f y %.2f w %.2f h %.2f\n", (unsigned) ulx,
                     (unsigned) uly, (unsigned) lrx, (unsigned) lry, x, y, width, height);
        }
    }
#endif
    {   /* Whole target pixels, the right and bottom edges rounded outward: the
         * int16 fields truncated x and width apart, so a scissor starting between
         * target pixels lost its last column (GoldenEye c68da63). SM64's dialogs
         * set such scissors: (2,2)-(320,240) after each box and the text clip at
         * x 30..162, 1.5 and 22.5 at 0.75 (no visible change either way). HELD
         * (the lead, R23): rounding outward hands a partly covered column inside
         * the screen to the scissor (F-Zero X's 4P divider), so it stays off until
         * the revised rule is checked. SM64_SCISSORFIX=1: on (A/B). */
        static int sScissorFix SAVESTATE_EXCLUDE = -1;
        if (sScissorFix < 0) { const char *e = getenv("SM64_SCISSORFIX"); sScissorFix = e && e[0] == '1'; }
        if (sScissorFix) {
            const float x1 = x + width, y1 = y + height;
            x = floorf(x);
            y = floorf(y);
            width = ceilf(x1 - 0.001f) - x;
            height = ceilf(y1 - 0.001f) - y;
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

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, UNUSED uint32_t palette, uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts) {
    // A zero mask means no wrapping: the RDP clamps that axis to the tile
    // whatever the clamp bit says (mirroring needs a mask too). OoT 8674501.
    // SM64 has 15 such tiles with the wrap bit set (Hoot, the file select,
    // Yoshi's egg, a sparkle, an effect, Goddard).
    if (masks == 0) cms = (cms & ~G_TX_MIRROR) | G_TX_CLAMP;
    if (maskt == 0) cmt = (cmt & ~G_TX_MIRROR) | G_TX_CLAMP;
    if (tile == G_TX_RENDERTILE) {
        tri_state.valid = false;
    }
    _UNUSED(shiftt);
    _UNUSED(shifts);
#ifdef HH_CENSUS
    sCenShift[tile & 7] = (uint8_t) ((shifts & 15) | ((shiftt & 15) << 4));
#endif

    if (tile == G_TX_RENDERTILE) {
        SUPPORT_CHECK(palette == 0); // palette should set upper 4 bits of color index in 4b mode
        rdp.texture_tile.fmt = fmt;
        rdp.texture_tile.siz = siz;
        rdp.texture_tile.cms = cms;
        rdp.texture_tile.cmt = cmt;
        rdp.texture_tile.masks = masks;
        rdp.texture_tile.maskt = maskt;
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
    if (tile == G_TX_LOADTILE) {
        rdp.texture_to_load.tile_number = tmem / 256;
    }
}

static void gfx_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    if (tile == G_TX_RENDERTILE) {
        tri_state.valid = false;
    }
    if (tile == G_TX_RENDERTILE) {
        rdp.texture_tile.uls = uls;
        rdp.texture_tile.ult = ult;
        rdp.texture_tile.lrs = lrs;
        rdp.texture_tile.lrt = lrt;
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
#ifdef HH_CENSUS
        sCenRtDirty = false;
#endif
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

static void gfx_dp_load_block(uint8_t tile, UNUSED uint32_t uls, UNUSED uint32_t ult, uint32_t lrs, uint32_t dxt) {
    tri_state.valid = false;
    _UNUSED(dxt);
#ifdef HH_DXTCOUNT /* the DKR session's check: does SM64 ever load a block with dxt 0 (odd rows pre-swapped)? */
    { static unsigned n0, n; n++; if (dxt == 0 && lrs > 0) { n0++; port_log("DXTCOUNT dxt0 #%u of %u: siz %u lrs %u tile %u\n", n0, n, (unsigned) rdp.texture_to_load.siz, (unsigned) lrs, (unsigned) tile); } }
#endif

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
    rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes = size_bytes;
    SUPPORT_CHECK(size_bytes <= 4096);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].addr = rdp.texture_to_load.addr;
    rdp.loaded_texture[rdp.texture_to_load.tile_number].stride_bytes = 0;
#ifdef HH_CENSUS
    { /* R27 bookkeeping: this slot's load, and the other slot overwritten when the
       * load runs into its TMEM range */
        const int sl = rdp.texture_to_load.tile_number, other = sl ^ 1;
        const uint16_t t0 = rdp.tile_tmem[tile & 7], words = (uint16_t) ((size_bytes + 7) / 8);
        sCenLoadTmem[sl] = t0; sCenLoadWords[sl] = words; sCenSlotStale[sl] = false;
        if (sCenLoadWords[other] && t0 < sCenLoadTmem[other] + sCenLoadWords[other] && sCenLoadTmem[other] < t0 + words) sCenSlotStale[other] = true;
    }
#endif
    
    rdp.textures_changed[rdp.texture_to_load.tile_number] = true;
}

static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    tri_state.valid = false;
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
    rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes = size_bytes;

    SUPPORT_CHECK(size_bytes <= 4096);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].addr =
        rdp.texture_to_load.addr + gfx_texture_image_offset(uls >> G_TEXTURE_IMAGE_FRAC, ult >> G_TEXTURE_IMAGE_FRAC);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].stride_bytes = gfx_texture_image_offset(0, 1);
#ifdef HH_CENSUS
    { /* R27 bookkeeping: this slot's load, and the other slot overwritten when the
       * load runs into its TMEM range */
        const int sl = rdp.texture_to_load.tile_number, other = sl ^ 1;
        const uint16_t t0 = rdp.tile_tmem[tile & 7], words = (uint16_t) ((size_bytes + 7) / 8);
        sCenLoadTmem[sl] = t0; sCenLoadWords[sl] = words; sCenSlotStale[sl] = false;
        if (sCenLoadWords[other] && t0 < sCenLoadTmem[other] + sCenLoadWords[other] && sCenLoadTmem[other] < t0 + words) sCenSlotStale[other] = true;
    }
#endif
#ifdef HH_CENSUS
    sCenLoadTiles++;
    if (uls != rdp.texture_tile.uls || ult != rdp.texture_tile.ult || lrs != rdp.texture_tile.lrs || lrt != rdp.texture_tile.lrt) {
        sCenLoadTileDiff++;
        sCenRtDirty = true; /* R26: the render tile's corners are about to be the load's */
    }
#endif
    rdp.texture_tile.uls = uls;
    rdp.texture_tile.ult = ult;
    rdp.texture_tile.lrs = lrs;
    rdp.texture_tile.lrt = lrt;

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
    if (rdp.fog_color.r != r || rdp.fog_color.g != g || rdp.fog_color.b != b || rdp.fog_color.a != a) {
        tri_state.valid = false; // the fold / the backend's fog colour
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

/* Set by gfx_dp_fill_rectangle for the race frame's opening full-screen solid
 * rect (select_framebuffer): draw it on the internal target, like the sky quads
 * after it. Every later draw covers it or composites over it, and a solid
 * colour upscales to itself, so the image is identical -- but the frame no
 * longer bounces 3D->2D->3D (a native fill, then a full seed of the internal
 * target from native) before the sky. */
static bool gfx_rect_on_3d;

static void gfx_draw_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    gfx_set_phase(gfx_rect_on_3d);
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
        if (hud_fit_mode()) { /* the HUD unsquished in place (hud_fit_mode) */
            const float x0 = ulx / 4.0f, x1 = lrx / 4.0f;
            const struct HudPlace p = hud_fit_place(x0, x1, uly / 4.0f, lry / 4.0f);
            if (!p.full) { ulxf = hud_fit_x(x0, &p); lrxf = hud_fit_x(x1, &p); }
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

#ifdef HH_CENSUS
    if (rdp.color_image_address == rdp.z_buf_address) sCenZTexrects++;
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) < G_CYC_COPY) {
        sCenTexrects++;
        if ((ulx | uly | lrx | lry) & 3) {
            if (++sCenTexrectFrac <= 24) port_log("census: fractional texrect %d.%02d,%d.%02d - %d.%02d,%d.%02d tile %u dsdx %d dtdy %d cyc %u\n",
                ulx >> 2, (ulx & 3) * 25, uly >> 2, (uly & 3) * 25, lrx >> 2, (lrx & 3) * 25, lry >> 2, (lry & 3) * 25,
                (unsigned) tile, (int) dsdx, (int) dtdy, (unsigned) ((rdp.other_mode_h >> G_MDSFT_CYCLETYPE) & 3));
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
        // 1/2-cycle: the lower-right edge is exclusive, so a texrect meant to reach
        // the last column or row stops one short -- a 1px seam on the handheld's
        // panel that a CRT's overscan hid. The fill rects' rule (MK64 99921c7), for
        // texrects (diddy-kong-racing 9df6228, Star Fox), in the one-short band only.
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
     * SM64_TEXRECTINV=1: drawn as before. */
    if (lrx < ulx || lry < uly) {
        static int draw SAVESTATE_EXCLUDE = -1;
        if (draw < 0) { const char *e = getenv("SM64_TEXRECTINV"); draw = e && e[0] == '1'; }
        if (!draw) { rdp.combine_mode = saved_combine_mode; return; }
    }
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
#ifdef HH_CENSUS
    if (rdp.color_image_address == rdp.z_buf_address) sCenZFills++;
#endif
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
        // 1/2-cycle: the lower-right edge is exclusive, so the game's full-screen
        // dims -- draw_box(0, 0, 319, 239), every pause menu -- leave the last
        // column and row uncovered. A CRT's overscan hid that; on the handheld's
        // panel it is a bright 1px seam on the right and bottom (device report
        // after v29). A box reaching the last pixel covers the edge.
        // The plain clamp (MK64 99921c7), not the scissor's one-short band: a
        // fill larger than the screen becomes an exact full-screen fill (Smash's
        // title; the band form changed 4 of its frames by ~38k px).
        if (lrx >= (SCREEN_WIDTH - 1) << 2) lrx = SCREEN_WIDTH << 2;
        if (lry >= (SCREEN_HEIGHT - 1) << 2) lry = SCREEN_HEIGHT << 2;
    }
    /* The same for a fill: an inverted rectangle covers no pixel on the RDP (texrect above).
     * SM64_TEXRECTINV=1: drawn as before. */
    if (lrx < ulx || lry < uly) {
        static int draw SAVESTATE_EXCLUDE = -1;
        if (draw < 0) { const char *e = getenv("SM64_TEXRECTINV"); draw = e && e[0] == '1'; }
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
#if defined(TARGET_HANDHELD)
    {
        /* Solid (no texel in the combiner), full-screen, and the first draw of
         * a frame that follows a 3D frame: both targets are still uniformly
         * clear, so blending or alpha-testing it gives the same pixels on
         * either. (MK64 issues it in 1-cycle mode, not FILL.) */
        extern int gfx_soft_last_frame_had_3d;
        extern int gfx_soft_frame_blank(void);
        bool textured = false;
        for (int i = 0; i < 8; i++) {
            const uint32_t m = (rdp.combine_mode >> (3 * i)) & 7;
            textured |= m == CC_TEXEL0 || m == CC_TEXEL1 || m == CC_TEXEL0A;
        }
        gfx_rect_on_3d = !textured && !gfx_seen_persp && gfx_soft_last_frame_had_3d && gfx_soft_frame_blank() &&
                         ulx <= 0 && uly <= 0 && lrx >= SCREEN_WIDTH << 2 && lry >= SCREEN_HEIGHT << 2;
    }
#endif
    // 1/2-cycle mode: the rectangle goes through the current combiner and
    // blender like any primitive.  MK64's draw_box (pause dim, menu fades,
    // the lightning flash, the fall-off blackout) does exactly that: XLU_SURF
    // + G_CC_PRIMITIVE with the box colour and an 8-bit alpha in the prim
    // colour.  Forcing the fill colour here (what sm64-port did) drew those
    // boxes opaque black or not at all, depending on the fill colour's 1-bit
    // alpha.
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    gfx_rect_on_3d = false;
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

extern void* port_seg_to_ptr(uintptr_t addr);
static inline void *seg_addr(uintptr_t w1) {
    return port_seg_to_ptr(w1);
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
                if (cmd->words.w1 == PORT_HUD_TAG_ON || cmd->words.w1 == PORT_HUD_TAG_OFF || cmd->words.w1 == PORT_HUD_TAG_CENTRE ||
                    cmd->words.w1 == PORT_HUD_TAG_CROP) {
                    int mode = (cmd->words.w1 == PORT_HUD_TAG_ON || cmd->words.w1 == PORT_HUD_TAG_CROP) ? 1 : cmd->words.w1 == PORT_HUD_TAG_CENTRE ? 2 : 0;
                    if (mode != gfx_hud_anchor) gfx_flush();
                    gfx_hud_anchor = mode;
#if !defined(PORT_GE_TL)
                    hud_fit_reset(); /* each HUD pass places afresh */
                    sHudCrop = cmd->words.w1 == PORT_HUD_TAG_CROP;
#endif
                }
#if !defined(PORT_GE_TL)
                else if (cmd->words.w1 == PORT_HUD_TAG_KEEP || cmd->words.w1 == PORT_HUD_TAG_KEEP_END) {
                    sHudKeep = cmd->words.w1 == PORT_HUD_TAG_KEEP; /* kept squeezed (hud_fit_mode) */
                } else if (cmd->words.w1 == PORT_HUD_TAG_GROUP) {
                    sHudGroupX = (uint32_t) cmd->words.w0 & 0xffffff; /* a named HUD element (hud_group_add) */
                } else if (cmd->words.w1 == PORT_HUD_TAG_GROUP2) {
                    hud_group_add(sHudGroupX, (uint32_t) cmd->words.w0 & 0xffffff);
                }
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
            case G_DL:
                if (C0(16, 1) == 0) {
                    // Push return address
                    gfx_run_dl((Gfx *)seg_addr(cmd->words.w1));
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
#if defined(F3D_OLD) && defined(F3DEX_GBI)
            // Early F3DEX quadrangle (MK64 startup logo): w1 = v3<<24 | v0<<16 | v1<<8 | v2 (indices * 2).
            // (MK64's F3D_OLD is "old F3DEX". In SM64's plain Fast3D, F3D_OLD alone, 0xB5 is G_LINE3D.)
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
#ifdef HH_CENSUS
                sCenTlutLoads++;
                if (C0(12, 12) || C0(0, 12)) sCenTlutOffset++;
#endif
                gfx_dp_load_tlut(C1(24, 3), C1(14, 10));
                break;
            case G_SETENVCOLOR:
                gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMCOLOR:
                gfx_dp_set_prim_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFOGCOLOR:
                gfx_dp_set_fog_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFILLCOLOR:
                gfx_dp_set_fill_color(cmd->words.w1);
                break;
            case G_SETCOMBINE:
#ifdef HH_CENSUS
                hh_census_combine(cmd->words.w0, cmd->words.w1);
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
#ifdef HH_CENSUS
            case (uint8_t)G_RDPHALF_1:
            case (uint8_t)G_RDPHALF_2:
#ifdef G_RDPHALF_CONT
            case (uint8_t)G_RDPHALF_CONT:
#endif
                if (++sCenRdpHalf <= 12) {
                    port_log("census: rdphalf at %p: prev %08X %08X this %08X %08X next %08X %08X\n", (void *) cmd,
                             (unsigned) cmd[-1].words.w0, (unsigned) cmd[-1].words.w1, (unsigned) cmd->words.w0,
                             (unsigned) cmd->words.w1, (unsigned) cmd[1].words.w0, (unsigned) cmd[1].words.w1);
                }
                break;
            default:
                if (opcode >= G_TRI_FILL && opcode <= G_TRI_SHADE_TXTR_ZBUFF) sCenRawTri++;
                break;
#endif
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
    /* Each task starts from the microcode's DMEM image: lookat X dir (0, 127, 0),
     * lookat Y (127, 0, 0) -- what SM64's own F3DEX2 build loads with
     * guLookAtReflect(up = +x), since F3DEX2 doesn't start with them. The US
     * game never sends gSPLookAt (Goddard's Mario head does), so its env maps
     * (metal Mario, stars, the metal cap cave) texgen from these. */
    memset(rsp.current_lookat, 0, sizeof(rsp.current_lookat));
    rsp.current_lookat[0].dir[0] = 127;
    rsp.current_lookat[1].dir[1] = 127;
    gfx_clip_ratio = 1; /* gspFast3D's DMEM (gfx_clip_ratio) */
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
    { /* SM64_DRAWDIST=<clip.w units>: far cull, desktop tuning, off by default.
       * Validated: non-numeric, trailing junk, <= 0 or absurd stays OFF -- the
       * round-12 bare atof() turned "abc" into 0.0f and culled the whole 3D
       * scene with no log line. Read here, once, not in the per-frame gfx_run. */
        const char *e = getenv("SM64_DRAWDIST"); char *end = NULL;
        gfx_draw_dist_w = 1e30f;
        if (e && e[0]) {
            const double d = strtod(e, &end);
            if (end && *end == '\0' && d > 0.0 && d < 1e9) { gfx_draw_dist_w = (float)d; port_log("gfx: far cull at clip.w %.0f (SM64_DRAWDIST)\n", d); }
            else port_log("gfx: ignoring SM64_DRAWDIST='%s' (want a positive number)\n", e);
        }
    }
    { /* SM64_CLIP=0|1|2: the clipper (gfx_clip_mode) */
        const char *e = getenv("SM64_CLIP");
        if (e && e[0] >= '0' && e[0] <= '2' && !e[1]) gfx_clip_mode = e[0] - '0';
        gfx_clip_trigger = Z_POS | Z_NEG | (gfx_clip_mode ? GB_OUT : 0);
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
    rendering_state.hilite = 0;
    rendering_state.fog_pix = false;
    rendering_state.trilerp = false;
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
#if !defined(PORT_GE_TL)
    hud_fit_reset();
#endif
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
    
    //INFO_MSG("New frame");
    
    if (!gfx_wapi->start_frame()) {
        dropped_frame = true;
        return;
    }
    dropped_frame = false;
    //double t0 = gfx_wapi->get_time();
    unsigned int t0 = sceKernelLibcClock();
    gfx_rapi->start_frame();
#if defined(TARGET_HANDHELD)
    gfx_phase3d = true; /* the backend opens every frame on the internal target */
    gfx_seen_persp = false;
    gfx_ui_active = gfx_ui_frame != 0; /* a menu screen: its 3D maps like its text */
    gfx_ui_frame = 0;
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

#ifdef HH_CLIPCHECK
unsigned hh_cc_all, hh_cc_near, hh_cc_far, hh_cc_clipped, hh_cc_changed;
void hh_clipcheck_report(void) {
    fprintf(stderr, "CLIPCHECK vertices %u, in front of near %u, beyond far %u | clipped tris %u (shape changed %u)\n",
            hh_cc_all, hh_cc_near, hh_cc_far, hh_cc_clipped, hh_cc_changed);
}
#endif
