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
	short u, v; /* S10.5: a texrect may start left of or above texel 0 (ssb64 4843c2c) */
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
    uint32_t view;  // the tile's size and row stride (gfx_texture_view_key)
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

#define GFX_LT_SLOTS 16
static struct RDP {
    const uint8_t *palette; // the last TLUT loaded
    const uint8_t *tlut_bank[16]; // DRAM address of each 16-entry TLUT bank in TMEM (CI4 tiles pick one)
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
        uint32_t row_bytes;    // LoadTile: the bytes of one loaded row (0 = LoadBlock)
        uint16_t line_bytes;   // LoadTile: the TMEM line its rows sit apart
        uint16_t tmem;
        bool valid;
    } loaded_texture[GFX_LT_SLOTS];
    uint8_t lt_next; // round-robin victim
    struct {
        uint8_t fmt;
        uint8_t siz;
        uint8_t cms, cmt;         // effective (gfx_tile_cm): what the sampler does
        uint8_t masks, maskt;     // wrap period 1 << mask texels (0 = none)
        uint16_t tmem; // TMEM address of the render tile (selects its loaded_texture[])
        uint8_t palette; // CI4 TLUT bank
        uint16_t uls, ult, lrs, lrt; // U10.2
        uint32_t line_size_bytes;
    } texture_tile;
    bool textures_changed[2];
    // Every tile descriptor (OoT's two-texture combiners read tile 1 as TEXEL1).
    struct {
        uint8_t fmt, siz, cms, cmt, shifts, shiftt, palette, masks, maskt;
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
#if !defined(PORT_GE_TL)
/* The viewport the vertices are mapped with (gfx_emit_vertex, the HUD's ortho vertices), set with rdp.viewport by
 * gfx_calc_and_set_viewport: rdp.viewport's whole pixels, as before, except when the unified renderer fills the screen
 * (FZX_FILL100), where it keeps the box map's fractional edges (a 320x240 viewport lands at -9.73..249.73 x
 * -18.46..258.46; rounded, the 3D would be shifted and scaled by up to 0.7 px). */
static struct { float x, y, w, h; } gfx_vpf SAVESTATE_EXCLUDE;
#endif

/* The tile the RSP draws with (gSPTexture's tile): TEXEL0 reads it and TEXEL1
 * the next one.  F-Zero X loads up to 7 machine textures into TMEM at once
 * (gDPLoadMultiBlock into tiles 1-7) and picks one per body part this way;
 * rdp.texture_tile is a copy of rdp.tiles[gfx_render_tile]. */
static uint8_t gfx_render_tile;
#define TEX1_TILE ((gfx_render_tile + 1) & 7)

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
    bool tex_lerp2; uint8_t lerp2_a[3], lerp2_b[3]; // backend: vertex * lerp(B, A, texel)
    bool tex_fog; struct RGBA fog_k;                // backend lerps the texel -> K by the vertex colour (FZX_TEXFOG)
    bool post_fog; struct RGBA post_fog_color;      // backend blends the fog colour after the texture
    bool own_edges; // FZX_FILLRULE: blended / alpha-tested without AA_EN, each pixel to one triangle of an edge
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
    bool tex_add;                          // GE ADD: texel + const_color (see the tint combiner in gfx_tri_rebuild_state)
    bool tex_lerp2;                        // vertex colour = SHADE, the backend does lerp(B, A, texel) * it
    bool tex_fog;                          // vertex colour = SHADE, the backend does lerp(texel, K, it) (FZX_TEXFOG)
    bool post_fog;                         // vertex alpha = fog factor, blended by the backend after the texture
    struct RGBA const_color;
} tri_state;

/* F3DFLX2.Rej, F-Zero X's race microcode: racer.c DMAs a 256-byte table into
 * DMEM (G_DMA_IO) and then loads LookAtY with a 16-bit direction. For vertices
 * lit with G_TEXTURE_GEN the microcode then skips texgen and sets the shade
 * alpha to table[128 + n.L] (n the vertex normal, L that direction in model
 * space); the machines' FOG_SHADE_A render mode blends the fog colour -- the
 * course's machine highlight colour -- in by it. Model after G-Diffuser's
 * libultraship (Zorkats/libultraship f7f2f41, interpreter.cpp). Per task. */
static uint8_t gfx_flx_lut[256];
static bool gfx_flx_lut_loaded, gfx_flx_light_valid;
static float gfx_flx_light[3];       // unit vector, as the game gave it
static float gfx_flx_light_model[3]; // in model space (recomputed with the lights)

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
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
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
/* Exact side planes (x > w etc., homogeneous: a triangle with all three
 * vertices beyond one of them has no visible point). Kept apart from the
 * guard-band bits above, which reject BEFORE the phase bookkeeping in
 * gfx_sp_tri1; these reject after it. (MK64 round 24 via oot d267ca3.) */
#define X_OUT_L (0x100)
#define X_OUT_R (0x200)
#define Y_OUT_B (0x400)
#define Y_OUT_T (0x800)
#define XY_OUT (X_OUT_L | X_OUT_R | Y_OUT_B | Y_OUT_T)
/* Beyond the RSP's guard band, |x| or |y| > ratio * |w| (gSPClipRatio): the triangle goes
 * to the clipper (R33, FZX_CLIPGB). One bit for all four sides, so it never joins the
 * shared-flag reject. */
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

static int gfx_fix_clipgb = -1; /* FZX_CLIPGB, read in gfx_fix_init (below) */
static float gfx_clip_rx = 2.0f, gfx_clip_ry = 2.0f; /* gSPClipRatio; each task starts at the microcode's 2 */
static const float NDCNearExact[4] = { 0.f, 0.f, -1.f, -1.f }; /* z >= -w: the RSP's near plane */

/* The polygon starts in v0; each pass writes the other buffer. Returns the vertex count and
 * sets *res to the buffer holding the result. */
static uint32_t clip_to_frustum( struct LoadedVertex * v0, struct LoadedVertex * v1, uint32_t vIn, struct LoadedVertex **res )
{
	uint32_t vOut = vIn;
	struct LoadedVertex *src = v0, *dst = v1, *t;
#define CLIP_PASS(call) do { vOut = (call); t = src; src = dst; dst = t; } while (0)

	// Near plane (w-based) first: removes behind-camera parts so the NDC-plane
	// clips below see only w>0 vertices.
	CLIP_PASS( clipToNearW( dst, src, vOut ) );
	if (gfx_fix_clipgb) {
		/* R33: the RSP's planes -- the guard band at x, y = +-ratio * w, the far plane and
		 * the near plane z >= -w; the screen edges are the rasterizer's (viewport, scissor). */
		const float gb[4][4] = { {  1.f,  0.f, 0.f, -gfx_clip_rx }, { -1.f,  0.f, 0.f, -gfx_clip_rx },
		                         {  0.f,  1.f, 0.f, -gfx_clip_ry }, {  0.f, -1.f, 0.f, -gfx_clip_ry } };
		for (int p = 0; p < 4; p++) CLIP_PASS( clipToHyperPlane( dst, src, vOut, gb[p] ) );
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[0] ) );	// far: z <= w
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCNearExact ) );	// near: z >= -w
	} else {
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[2] ) );	// right
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[1] ) );	// left
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[4] ) );	// top
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[3] ) );	// bottom
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[0] ) );	// far (z <= w; labelled "near" above)
		CLIP_PASS( clipToHyperPlane( dst, src, vOut, NDCPlane[5] ) );	// near, pulled in to -0.97w
	}
#undef CLIP_PASS

	*res = src;
	return vOut;
}

static struct LoadedVertex temp_a[12];
static struct LoadedVertex temp_b[12];
/* MK64's round-24 review of the kit's depth and clipping (super-mario-64 5cd6699), as the RSP
 * does it. F-Zero X's race camera has its near plane at 16 units (512 in one mode), so
 * everything within ~32 units of the eye sat at z/w < 0. Each has an A/B switch (=0: as before):
 *   FZX_ZRANGE   z/w runs -1 (near) .. 1 (far); the z-buffer is 0..1: z*0.5+0.5, not z/w
 *                (nearer than z/w = 0 all clamped to depth 0 and lost its order)
 *   FZX_FARREJ   the far reject is z > w (z > 2|w| never fires with a standard perspective)
 *   FZX_CLIPTRIG clip a triangle with any vertex in front of the near plane or beyond the far,
 *                not only within MK64's distances (w < 2, or w < 24 and past far)
 *   FZX_CLIPOUT  the clipper's result is in temp_b (seven passes bounce between the two), not
 *                temp_a: the polygon before the far pass with the far pass's vertex count
 * R33 (DKR 467ad1e; the ports lead's round 2, floor flashing):
 *   FZX_CLIPGB   a vertex beyond the guard band (|x| or |y| > ratio * |w|, the ratio from
 *                gSPClipRatio: 3 in the race, 2 to 6 elsewhere) sends its triangle to the
 *                clipper too, and the clipper cuts at the RSP's planes: the guard band, the far
 *                plane and the near plane z >= -w, with no screen-edge planes (0: the exact
 *                screen edges, cut only once near or far had sent the triangle, and the near
 *                plane at -0.97w). The shade colour is interpolated on screen, so where a
 *                triangle is cut changes its shading: cut at other planes than the console's,
 *                a big floor triangle's shading jumped in the frame a vertex first crossed the
 *                near plane. Off in rounds 12-16 (it moves big triangles' shading by 1-3 levels
 *                and cured no visible flash, for +0.8% instructions); ON again since 2026-09-30,
 *                the user's rule "I'd rather slower but correct visuals": the console clips this
 *                way. FZX_CLIPGB=0 turns it off. */
static int gfx_fix_zrange = -1, gfx_fix_farrej = -1, gfx_fix_cliptrig = -1, gfx_fix_clipout = -1;
static void gfx_fix_init(void) {
    if (gfx_fix_zrange >= 0) return;
    const char *e;
    e = getenv("FZX_ZRANGE"); gfx_fix_zrange = !(e && e[0] == '0');
    e = getenv("FZX_FARREJ"); gfx_fix_farrej = !(e && e[0] == '0');
    e = getenv("FZX_CLIPTRIG"); gfx_fix_cliptrig = !(e && e[0] == '0');
    e = getenv("FZX_CLIPOUT"); gfx_fix_clipout = !(e && e[0] == '0');
    e = getenv("FZX_CLIPGB"); gfx_fix_clipgb = !(e && e[0] == '0');
}

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
    const struct LoadedVertex *poly = gfx_fix_clipout ? res : temp_a; /* res: temp_b, after seven passes */
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
#if !defined(PORT_GE_TL)
/* The screen filled (the user, 2026-09-29: "some games don't take up the full screen like fzero";
 * their pick: stretch to fill). A 1P race clips everything to the TV-safe box x 12..308, y 16..224
 * of the 320x240 frame (camera.c), and a TV's overscan hid the black around it; on the handheld it
 * showed as 8 px bars at the sides and 16 at the top and bottom. Race_Draw names the box at the
 * start of each frame (PORT_FRAME_CROP); for that frame the 2D phase maps the box onto the whole
 * screen (viewport, scissor, rectangles: lay2out_*), gfx_soft.c's composite stretches the same box
 * of the 3D image over it (the same 3D pixels, scaled up a little more), and the race HUD keeps its
 * 1:1 (or squeezed) size, its elements moved with their centres. The 3D phase keeps the normal map
 * (the composite does the crop). FZX_FILL=0: the bars, as before.
 * The 3D at 100% (the device's default since 2026-10-03; the user, 2026-10-09: "fix them if they exist"): the unified
 * renderer draws the 3D straight onto the native screen, with no composite to stretch it, so the fill was off there
 * and the device showed the bars, and FZX_MENU1TO1 was off with it. Now the unified renderer draws the 3D through the
 * box's map itself (crop_on in the 3D phase too: viewport and scissor), at the square screen's own size, so the same
 * picture as the 80% fill's at full resolution, the box's 296 columns on 240 pixels; the viewport keeps its fractional
 * edges (gfx_vpf: rdp.viewport is whole pixels and shifted the 3D up to 0.7 px). FZX_FILL100=0: the bars at 100% (as
 * before); ab: alternates every 5 s (300 VIs) with an "AB fill100" line in console.log; default on. */
static bool gfx_phase3d;
int gfx_fill_on SAVESTATE_EXCLUDE = -1;
int gfx_fill100 SAVESTATE_EXCLUDE = -1;       /* FZX_FILL100: 0 off, 1 on, 2 ab (bench.c sets it) */
static int sFill100Frame SAVESTATE_EXCLUDE = -1; /* this frame's pick (gfx_start_frame): ab switches between frames */
static int sCropOn SAVESTATE_EXCLUDE;         /* this frame names a box to fill the screen with */
static float sCrop[4] SAVESTATE_EXCLUDE;       /* x0, y0, x1, y1 in the 320x240 layout */
static uint32_t sCropX SAVESTATE_EXCLUDE;      /* the pending tag's x0 | x1 << 12 */
extern unsigned int port_os_vi_count(void);
static int sCropMode SAVESTATE_EXCLUDE; /* this frame's fill (crop_mode_set): 0 none, 1 the 2D through the box (a
                                          * composite stretches the 3D), 2 the 3D too (the unified renderer) */
#if defined(MIYOO)
/* The Miyoo Mini's fill (the user, 2026-10-11, after the first device run: "fill the 640x480 panel, no black
 * border"; FZX_FILL=0: the N64's bordered picture). The screen is 4:3, so the square screen's fill (the box
 * stretched over it) would stretch a race 6.7% taller. Instead the frame's box is widened to 4:3 around its
 * centre (a race's 296x208 to 296x222; the title and menus' 296x224 are within 2% of 4:3 and kept) and mapped
 * onto the screen at one scale (x1.081 in a race), and in the 3D the scissor and the viewport's clip edges that
 * lie on the named box are moved out to the screen's edges: the scene draws its own pixels there (7 rows at the
 * top and bottom of a race; the viewport's transform is unchanged, so nothing is stretched). The race HUD is laid
 * out at 1:1 around the centres the map gives it (hud_fit_mode), and menu text 1:1 where it fits, as on the
 * handheld. sMiyooFill: this frame's fill is that one; sCropBox: the box the frame named. */
static int sMiyooFill SAVESTATE_EXCLUDE;
static float sCropBox[4] SAVESTATE_EXCLUDE;
#endif
#line 777 /* the handheld build's line numbers (__LINE__ in SUPPORT_CHECK) stay as before the Miyoo blocks */
static void fill100_frame(void) {
    if (gfx_fill100 < 0) {
        const char *e = getenv("FZX_FILL100");
        gfx_fill100 = !e ? 1 : (e[0] == 'a' || e[0] == 'A') ? 2 : e[0] == '0' ? 0 : 1;
    }
    if (gfx_fill100 != 2) { sFill100Frame = gfx_fill100; return; }
    const int on = ((port_os_vi_count() / 300) & 1) == 0;
    if (on != sFill100Frame) port_log("AB fill100 %s vi %u\n", on ? "on" : "off", port_os_vi_count());
    sFill100Frame = on;
}
/* The frame's fill, worked out once when it names its box (PORT_FRAME_CROP; the targets' sizes are set for the frame
 * by then) and kept until the next frame starts, for the readbacks between frames too. */
static void crop_mode_set(void) {
    extern void gfx_soft_get_internal(int *w, int *h);
    extern uint32_t *gfx_soft_get_framebuffer(int *w, int *h);
    int iw = 0, ih = 0, nw = 0, nh = 0;
    sCropMode = 0;
    if (gfx_fill_on < 0) { const char *e = getenv("FZX_FILL"); gfx_fill_on = !(e && e[0] == '0'); }
#if defined(MIYOO)
    sMiyooFill = 0;
#endif
#line 795
    if (!gfx_fill_on || !sCropOn) return;
    gfx_soft_get_internal(&iw, &ih);
    (void) gfx_soft_get_framebuffer(&nw, &nh);
#if defined(MIYOO)
    if (nw * 3 == nh * 4) { /* the Miyoo's 4:3 screen: the box widened to 4:3, mapped at one scale (above) */
        const float w = sCrop[2] - sCrop[0], h = sCrop[3] - sCrop[1], a = w / h, k = 4.0f / 3.0f;
        sCropBox[0] = sCrop[0]; sCropBox[1] = sCrop[1]; sCropBox[2] = sCrop[2]; sCropBox[3] = sCrop[3];
        if (a > k * 1.02f) { const float c = 0.5f * (sCrop[1] + sCrop[3]), h2 = 0.5f * w / k; sCrop[1] = c - h2; sCrop[3] = c + h2; }
        else if (a < k / 1.02f) { const float c = 0.5f * (sCrop[0] + sCrop[2]), w2 = 0.5f * h * k; sCrop[0] = c - w2; sCrop[2] = c + w2; }
        sCropMode = iw == nw && ih == nh ? 2 : 1;
        sMiyooFill = 1;
        return;
    }
#endif
#line 798
    if (nw * 3 >= nh * 4) return; /* a 4:3 screen */
    if (iw == nw && ih == nh) { /* the unified renderer, no composite: the 3D is drawn through the box (FZX_FILL100) */
        if (sFill100Frame < 0) fill100_frame();
        sCropMode = sFill100Frame ? 2 : 0;
    } else {
        sCropMode = 1;
    }
}
/* The box for this frame, if the fill applies (the square screen; a composite, or the unified renderer with
 * FZX_FILL100): gfx_soft.c's and the readbacks' too. */
int gfx_frame_crop(float r[4]) {
    if (!sCropMode) return 0;
    if (r) { r[0] = sCrop[0]; r[1] = sCrop[1]; r[2] = sCrop[2]; r[3] = sCrop[3]; }
    return 1;
}
/* the 2D maps through the box; the 3D too when nothing composites it (the unified renderer) */
static int crop_on(void) { return sCropMode == 2 || (sCropMode == 1 && !gfx_phase3d); }
/* The box the 3D view is filled from (the 320x240 layout), whatever the 3D size: FZX_WALLLOD's column map
 * (gfx_soft.c wall_cols). 0 when the frame names none or the fill is off. */
int gfx_frame_box(float r[4]) {
    if (gfx_fill_on < 0) { const char *e = getenv("FZX_FILL"); gfx_fill_on = !(e && e[0] == '0'); }
    if (!gfx_fill_on || !sCropOn) return 0;
    r[0] = sCrop[0]; r[1] = sCrop[1]; r[2] = sCrop[2]; r[3] = sCrop[3];
    return 1;
}
/* The results screens at 1:1 (the user, 2026-10-09: the GP results and the total ranking "re-laid out so text and
 * sprites are 1:1 and fit the square view"). Their 260-column N64 box was squeezed into 240 columns (x 0.75, or
 * x 0.81 and y 1.15 under the fill). menus.c now lays them out for 240 columns itself (port_layout_1to1) and draws
 * them between PORT_LAYOUT_1TO1 and PORT_LAYOUT_END: there the 2D shows the layout's columns x0..x0+239 and rows
 * y0..y0+239 one to one, one layout pixel a screen pixel, whatever the frame's own map (the fill or the squeeze).
 * The 3D, and everything outside the bracket, keep theirs. FZX_RESULTS1TO1=0: as before. */
int gfx_results1to1 SAVESTATE_EXCLUDE = -1;
static int sOneOn SAVESTATE_EXCLUDE;    /* inside a PORT_LAYOUT_1TO1 bracket */
static float sOne[2] SAVESTATE_EXCLUDE; /* the layout's x, y at the screen's top-left */
static int square_screen(void) { return gfx_current_dimensions.width * 3 < gfx_current_dimensions.height * 4; }
/* menus.c: lay the results screens out for 240 columns (a square screen, the switch on) */
int port_layout_1to1(void) {
    if (gfx_results1to1 < 0) { const char *e = getenv("FZX_RESULTS1TO1"); gfx_results1to1 = !(e && e[0] == '0'); }
    return gfx_results1to1 && square_screen();
}
static int one_on(void) { return sOneOn && !gfx_phase3d; }
static float one_unit(void) { return (float) gfx_current_dimensions.height / 240.0f; } /* 1 at native */
/* Layout (320x240) to the current target, and back. */
static float lay2out_x(float x) {
    if (one_on()) return (x - sOne[0]) * one_unit();
    return crop_on() ? (x - sCrop[0]) * (float) gfx_current_dimensions.width / (sCrop[2] - sCrop[0]) : x * RATIO_X;
}
static float lay2out_y(float y) {
    if (one_on()) return (y - sOne[1]) * one_unit();
    return crop_on() ? (y - sCrop[1]) * (float) gfx_current_dimensions.height / (sCrop[3] - sCrop[1]) : y * RATIO_Y;
}
static float out2lay_x(float x) {
    if (one_on()) return sOne[0] + x / one_unit();
    return crop_on() ? sCrop[0] + x * (sCrop[2] - sCrop[0]) / (float) gfx_current_dimensions.width : x / RATIO_X;
}
static float out2lay_y(float y) {
    if (one_on()) return sOne[1] + y / one_unit();
    return crop_on() ? sCrop[1] + y * (sCrop[3] - sCrop[1]) / (float) gfx_current_dimensions.height : y / RATIO_Y;
}
#endif
#if !defined(PORT_GE_TL)
/* The race HUD unsquished (the user's standing ask for every port: HUD, icons and text at 1:1,
 * not squeezed 0.75 in x; the lead's X27, 2026-09-28: pixel-exact text; MK64 39258d3 and SM64
 * 80270dd / eb0a164 are the templates). On a square output the HUD pass -- between
 * PORT_HUD_TAG_ON and _OFF, menus.c around Hud_DrawHud, the portraits, the minimap, the position
 * and the speed -- is unsquished in place: each element keeps the centre the squeeze gives it and
 * is drawn at 1:1 around it (x scaled like y, 240/240, not 240/320). hud.c names each element's box
 * (PORT_HUD_GROUP) so a label and its digits, each glyph a texrect of its own, are laid out at 1:1
 * as one piece around one centre instead of each glyph widened in place. The placement is on whole
 * output pixels (hud_snap), so at 1:1 every texel centre is a pixel centre: the texrect path
 * samples it point (gfx_soft.c "plain"), the text comes out pixel-exact. Elements that would
 * overlap at 1:1 keep the squeeze (PORT_HUD_GROUP's keep, or PORT_HUD_KEEP_BEGIN/END): in 1P the
 * top row can't fit (portraits 32 + LAP 25 + position 77 + TIME 97 px at 1:1), so the position,
 * TIME and the spare machines stay squeezed, as MK64's TIME and lap counter did (the user's pick
 * there); in 3P/4P the speed and the energy share a quarter's row and stay squeezed. A draw in no
 * named element keeps the squeeze (hud_fit_place), as does anything wider than the screen at 1:1. Menus, the results and
 * the other screens stay squeezed (the user's scope rule after MK64 v38: only the in-game HUD
 * unless each screen is checked). FZX_HUDUNSQ=0: the squeeze everywhere, as before. */
int gfx_hud_unsq SAVESTATE_EXCLUDE = -1;
static int sHudKeep SAVESTATE_EXCLUDE; /* inside a PORT_HUD_TAG_KEEP bracket */
static int hud_fit_mode(void) {
    if (gfx_hud_unsq < 0) { const char *e = getenv("FZX_HUDUNSQ"); gfx_hud_unsq = !(e && e[0] == '0'); }
#if defined(MIYOO)
    if (sMiyooFill && crop_on()) return gfx_hud_unsq && !sHudKeep && gfx_hud_anchor == 1; /* 1:1 under the Miyoo's fill */
#endif
#line 880
    return gfx_hud_unsq && !sHudKeep && gfx_hud_anchor == 1 && gfx_current_dimensions.width * 3 < gfx_current_dimensions.height * 4;
}
static float hud_unit(void) { return (float) gfx_current_dimensions.height / (2.0f * HALF_SCREEN_HEIGHT); } /* 1:1 = y's scale */
/* A placed element: full (leave it squeezed), or the layout x it grows around (cx) and where that
 * lands (ox), in layout units (output px / RATIO_X), so a placement made while the 3D target (80%)
 * is bound lands the same on the native one (SM64 IMG_0132-0134). */
struct HudPlace { int full; float cx, ox, s, cy; }; /* s: x scale in units of hud_unit() (1 = 1:1; the squeeze when a kept element is moved), target-independent; cy: the y it keeps its size around */
struct HudBox { float x0, x1, y0, y1; struct HudPlace p; };
static struct HudBox sHudGroup[40] SAVESTATE_EXCLUDE; /* this pass's named elements (PORT_HUD_GROUP) */
static int sHudGroupN SAVESTATE_EXCLUDE;
static uint32_t sHudGroupX SAVESTATE_EXCLUDE; /* the pending group's x0 | x1 << 12 */
static void hud_fit_reset(void) { sHudKeep = 0; sHudGroupN = 0; }
/* On whole output pixels: layout x lands on x * hud_unit() + a whole-pixel offset. */
static float hud_snap(float cx, float ox_px) {
#if defined(MIYOO)
    const float px = sMiyooFill ? 1.0f : (float) gfx_current_dimensions.width / 240.0f, d = ox_px - cx * hud_unit(); /* whole pixels */
#else
#line 894
    const float px = (float) gfx_current_dimensions.width / 240.0f, d = ox_px - cx * hud_unit();
#endif
#line 895
    return cx * hud_unit() + floorf(d / px + 0.5f) * px;
}
/* A placed group's output edges. */
static float hud_group_left(const struct HudBox *g) { return g->p.full ? lay2out_x(g->x0) : lay2out_x(g->p.ox) - (g->p.cx - g->x0) * g->p.s * hud_unit(); }
static float hud_group_right(const struct HudBox *g) { return g->p.full ? lay2out_x(g->x1) : lay2out_x(g->p.ox) + (g->x1 - g->p.cx) * g->p.s * hud_unit(); }
static void hud_group_add(uint32_t xs, uint32_t ys) {
    struct HudBox *g;
    if (sHudGroupN == (int) (sizeof(sHudGroup) / sizeof(sHudGroup[0]))) return;
    g = &sHudGroup[sHudGroupN++];
    g->x0 = (float) (xs & 0xfff); g->x1 = (float) ((xs >> 12) & 0xfff);
    g->y0 = (float) (ys & 0x3ff); g->y1 = (float) ((ys >> 12) & 0x7ff);
    const int at = (ys >> 10) & 3; /* PORT_HUD_AT_LEFT / _RIGHT: line up with the group before */
    const float cx = 0.5f * (g->x0 + g->x1);
    g->p.full = (ys >> 23) & 1; /* PORT_HUD_KEEP_Y1: the element keeps the squeeze */
    if (!g->p.full && (g->x1 - g->x0) * hud_unit() > (float) gfx_current_dimensions.width) g->p.full = 1;
    g->p.cx = cx;
    g->p.cy = 0.5f * (g->y0 + g->y1);
    g->p.s = g->p.full ? RATIO_X / hud_unit() : 1.0f;
    float ox = lay2out_x(cx);
    if (at && sHudGroupN >= 2) { /* the user, 2026-09-28: LAP left-aligned with the spare machines, TIME right-aligned with the energy */
        const struct HudBox *q = &sHudGroup[sHudGroupN - 2];
        ox = at == 1 ? hud_group_left(q) + (cx - g->x0) * g->p.s * hud_unit() : hud_group_right(q) - (g->x1 - cx) * g->p.s * hud_unit();
        g->p.full = 0; /* a kept element moves at its squeeze (s) */
    }
    if (g->p.full && crop_on()) g->p.full = 0; /* the fill moves kept elements too (at their squeeze) */
    g->p.ox = out2lay_x(g->p.s == 1.0f ? hud_snap(cx, ox) : ox); /* layout units: lands at lay2out_x(ox) */
}
/* x0..x1, y0..y1: a draw in the 320x240 layout. Only named elements are unsquished: a draw in
 * none of them (a text hud.c doesn't name yet, the 2P player labels) keeps the squeeze, so no
 * glyph-by-glyph text can be widened in place and overlap its neighbours. */
static struct HudPlace hud_fit_place(float x0, float x1, float y0, float y1) {
    const float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
    struct HudPlace p = { 1, 0.0f, 0.0f, 1.0f, 0.0f };
    int i;
    for (i = 0; i < sHudGroupN; i++) { /* inside a named element: laid out with it */
        const struct HudBox *g = &sHudGroup[i];
        if (cx >= g->x0 - 1.0f && cx <= g->x1 + 1.0f && cy >= g->y0 - 1.0f && cy <= g->y1 + 1.0f) return g->p;
    }
    if (crop_on()) { /* the fill: an unnamed draw keeps its squeezed size, moved with its centre */
        p.full = 0; p.cx = cx; p.ox = cx; p.cy = cy; p.s = RATIO_X / hud_unit();
    }
    (void) x0; (void) x1; (void) y0; (void) y1;
    return p;
}
/* Layout x to output pixels: the centre where the squeeze puts it, the rest at 1:1 around it. */
static float hud_fit_x(float x, const struct HudPlace *p) { return lay2out_x(p->ox) + (x - p->cx) * p->s * hud_unit(); }
/* Layout y to output pixels: y's own scale (1:1) around the element's centre, which the fill moves. */
static float hud_fit_y(float y, const struct HudPlace *p) {
    const float px = (float) gfx_current_dimensions.height / 240.0f, d = lay2out_y(p->cy) - p->cy * RATIO_Y; /* on whole pixels */
    return p->cy * RATIO_Y + floorf(d / px + 0.5f) * px + (y - p->cy) * RATIO_Y;
}
#endif
#if !defined(PORT_GE_TL)
/* Menu text at 1:1 (the user, 2026-09-29: "show me before and after first on all", then picked
 * "1:1 where it fits"; FZX_MENU1TO1=0 keeps the stretch). On a filled screen (gfx_frame_crop),
 * outside the race HUD pass, every font string font.c names (PORT_TEXT_BOX: its box from
 * Font_GetStringWidth) is laid out at the N64's own size (1:1, as the race HUD), on whole pixels
 * (text_centre_out: from its column's edge or its own centre); everything else keeps the fill's
 * stretch. An unscaled font's glyphs then land texel on pixel. */
int gfx_menu1to1 SAVESTATE_EXCLUDE = -1;
static struct HudBox sTextBox[96] SAVESTATE_EXCLUDE; /* this frame's font strings */
static int sTextN SAVESTATE_EXCLUDE;
static struct HudBox sTextPrev[96] SAVESTATE_EXCLUDE; /* last frame's (a string drawn later this frame is there) */
static int sTextPrevN SAVESTATE_EXCLUDE;
static uint32_t sTextX SAVESTATE_EXCLUDE;
static int text_fit_mode(void) {
    if (gfx_menu1to1 < 0) { const char *e = getenv("FZX_MENU1TO1"); gfx_menu1to1 = !(e && e[0] == '0'); }
    return gfx_menu1to1 && sTextN > 0 && !hud_fit_mode() && crop_on() && !sOneOn; /* (a 1:1 bracket is 1:1 already) */
}
static void text_box_add(uint32_t xs, uint32_t ys) {
    if (sTextN == (int) (sizeof(sTextBox) / sizeof(sTextBox[0]))) return;
    struct HudBox *b = &sTextBox[sTextN++];
    b->x0 = (float) (xs & 0xfff); b->x1 = (float) ((xs >> 12) & 0xfff);
    b->y0 = (float) (ys & 0xfff); b->y1 = (float) ((ys >> 12) & 0xfff);
}
static int text_same(const struct HudBox *a, const struct HudBox *b);
/* where a string's 1:1 box hangs from: a column's strings (sharing a left edge: the rankings' names)
 * keep that edge where the fill puts it and grow right, a right-aligned column grows left, a lone
 * string (a title, a course name) grows both ways from its centre. The string's centre on the target. */
static float text_centre_out(const struct HudBox *b) {
    const float half = 0.5f * (b->x1 - b->x0) * hud_unit();
    const struct HudBox *lists[2] = { sTextBox, sTextPrev };
    const int ns[2] = { sTextN, sTextPrevN };
    int left = 0, right = 0;
    for (int l = 0; l < 2; l++)
        for (int i = 0; i < ns[l]; i++) {
            const struct HudBox *c = &lists[l][i];
            if (c == b || text_same(c, b)) continue;
            left |= fabsf(c->x0 - b->x0) < 0.5f;
            right |= fabsf(c->x1 - b->x1) < 0.5f;
        }
    if (left && !right) return lay2out_x(b->x0) + half;
    if (right && !left) return lay2out_x(b->x1) - half;
    return lay2out_x(0.5f * (b->x0 + b->x1));
}
/* a string's box at 1:1 on the target (text_centre_out): x0, y0, x1, y1 */
static void text_box_out(const struct HudBox *b, float o[4]) {
    const float u = hud_unit(), cx = text_centre_out(b), cy = lay2out_y(0.5f * (b->y0 + b->y1));
    o[0] = cx - 0.5f * (b->x1 - b->x0) * u; o[2] = cx + 0.5f * (b->x1 - b->x0) * u;
    o[1] = cy - 0.5f * (b->y1 - b->y0) * u; o[3] = cy + 0.5f * (b->y1 - b->y0) * u;
}
static int text_same(const struct HudBox *a, const struct HudBox *b) {
    return fabsf(a->x0 - b->x0) < 0.5f && fabsf(a->x1 - b->x1) < 0.5f && fabsf(a->y0 - b->y0) < 0.5f && fabsf(a->y1 - b->y1) < 0.5f;
}
/* 1: the string at 1:1 would run into another string (this frame's so far, or last frame's: those
 * drawn after it); it keeps the stretch then (the results rows: a name and its points) */
static int text_clashes_one(const struct HudBox *b) {
    float o[4], q[4];
    text_box_out(b, o);
    const struct HudBox *lists[2] = { sTextBox, sTextPrev };
    const int ns[2] = { sTextN, sTextPrevN };
    for (int l = 0; l < 2; l++)
        for (int i = 0; i < ns[l]; i++) {
            const struct HudBox *c = &lists[l][i];
            if (c == b || text_same(c, b)) continue;
            text_box_out(c, q);
            if (o[0] < q[2] - 0.5f && q[0] < o[2] - 0.5f && o[1] < q[3] - 0.5f && q[1] < o[3] - 0.5f) return 1;
        }
    return 0;
}
/* a column (strings sharing a left or a right edge: the results' names, their points) decides as
 * one, so a short name doesn't go 1:1 beside a long one kept stretched */
static int text_clashes(const struct HudBox *b) {
    if (text_clashes_one(b)) return 1;
    const struct HudBox *lists[2] = { sTextBox, sTextPrev };
    const int ns[2] = { sTextN, sTextPrevN };
    for (int l = 0; l < 2; l++)
        for (int i = 0; i < ns[l]; i++) {
            const struct HudBox *c = &lists[l][i];
            if (c == b || text_same(c, b)) continue;
            if ((fabsf(c->x0 - b->x0) < 0.5f || fabsf(c->x1 - b->x1) < 0.5f) && text_clashes_one(c)) return 1;
        }
    return 0;
}
/* the string a draw belongs to, placed now (the 2D target's map); 0 if none, or it stays stretched */
static int text_fit_place(float x0, float x1, float y0, float y1, struct HudPlace *p) {
    const float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
    for (int i = sTextN - 1; i >= 0; i--) { /* the latest string drawn there */
        const struct HudBox *b = &sTextBox[i];
        if (cx >= b->x0 - 1.0f && cx <= b->x1 + 1.0f && cy >= b->y0 - 1.0f && cy <= b->y1 + 1.0f) {
            if (text_clashes(b)) return 0;
            p->full = 0; p->s = 1.0f;
            p->cx = 0.5f * (b->x0 + b->x1); p->cy = 0.5f * (b->y0 + b->y1);
            float ox = text_centre_out(b);
            const float half = 0.5f * (b->x1 - b->x0) * hud_unit(), w = (float) gfx_current_dimensions.width, m = 2.0f * w / 240.0f;
            if (ox - half < m) ox = m + half; /* inside the screen, a 2-px margin */
            else if (ox + half > w - m) ox = w - m - half;
            p->ox = out2lay_x(hud_snap(p->cx, ox));
            return 1;
        }
    }
    return 0;
}
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
static inline int gfx_tex_slot(int tile);
/* FZX_PROBE=x,y (gfx_soft.c): describe a batch's texture state when it starts. */
int gfx_probe_on = -1;
char gfx_probe_note[256];
static void gfx_probe_note_batch(void) {
    snprintf(gfx_probe_note, sizeof(gfx_probe_note),
             "tile %u fmt %u siz %u st %u,%u-%u,%u tmem %u addr %p line %u cms %u cmt %u scale %04X/%04X cc %08X prim %02X%02X%02X env %02X%02X%02X",
             (unsigned) gfx_render_tile, rdp.texture_tile.fmt, rdp.texture_tile.siz, rdp.texture_tile.uls >> 2,
             rdp.texture_tile.ult >> 2, rdp.texture_tile.lrs >> 2, rdp.texture_tile.lrt >> 2, rdp.texture_tile.tmem,
             (void *) rdp.loaded_texture[gfx_tex_slot(0)].addr, (unsigned) rdp.texture_tile.line_size_bytes,
             rdp.texture_tile.cms, rdp.texture_tile.cmt, rsp.texture_scaling_factor.s, rsp.texture_scaling_factor.t,
             (unsigned) rdp.combine_mode, rdp.prim_color.r, rdp.prim_color.g, rdp.prim_color.b, rdp.env_color.r,
             rdp.env_color.g, rdp.env_color.b);
}

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

/* Pre-blended two-texture combiners (gfx_blend_tiles): texture ids from the
 * same pool as the cache, so a cache reset drops them too. */
#define GFX_BLEND_CACHE 32
static struct {
    uint32_t key[12];
    uint32_t texture_id;
    uint32_t last_used_frame;
    bool valid;
} gfx_blend_cache[GFX_BLEND_CACHE];

/* Bumped by every texture cache reset (ocarina-of-time 2b69f7b, the lead's cross-port round
 * 2026-09-27). A reset wipes every texture, including the one the OTHER tile of the draw being
 * set up already holds: importing tile 1 could reset the cache and leave
 * rendering_state.textures[0] on a zeroed pool node (id 0), and the draw went out untextured, a
 * flat block of the combiner's colour (OoT's red sunset bar after a long session filled the
 * arena). The import loops run again when it changes under them. FZX_EPOCHFIX=0: as before. */
static uint32_t gfx_cache_epoch SAVESTATE_EXCLUDE;
static int gfx_epochfix SAVESTATE_EXCLUDE = -1;
static bool gfx_epochfix_on(void) {
    if (gfx_epochfix < 0) { const char *e = getenv("FZX_EPOCHFIX"); gfx_epochfix = !(e && e[0] == '0'); }
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
        tri_state.valid = false; /* as everywhere else textures_changed is set */
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
static int gfx_lt_slot_for_load(uint16_t tmem, uint32_t size_bytes) {
    int i = gfx_lt_find(tmem, true), n;
    {
        /* The load overwrites the TMEM words it covers: other loads there are gone. */
        const uint32_t lo = tmem, hi = lo + (size_bytes + 7) / 8;
        for (n = 0; n < GFX_LT_SLOTS; n++) {
            const uint32_t l2 = rdp.loaded_texture[n].tmem, h2 = l2 + (rdp.loaded_texture[n].size_bytes + 7) / 8;
            if (n != i && rdp.loaded_texture[n].valid && l2 < hi && lo < h2) rdp.loaded_texture[n].valid = false;
        }
    }
    if (i >= 0) return i;
    for (i = 0; i < GFX_LT_SLOTS; i++) {
        if (!rdp.loaded_texture[i].valid) goto found;
    }
    for (n = 0; n < GFX_LT_SLOTS; n++) {
        i = (rdp.lt_next + n) % GFX_LT_SLOTS;
        if (rdp.loaded_texture[i].tmem != rdp.texture_tile.tmem && rdp.loaded_texture[i].tmem != rdp.tiles[TEX1_TILE].tmem) break;
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
    return gfx_lt_find(tile == 0 ? rdp.texture_tile.tmem : rdp.tiles[TEX1_TILE].tmem, false);
}

/* The texels a texture unit's tile reads: its load, from the tile's offset into it
 * (paper-mario aa512fd). F-Zero X packs textures behind each other in one load and points
 * tiles into it: the menus' I4 tiles 1-4 (TMEM 16-112 of a load at 0), the race's I4 tiles
 * 1-7 (TMEM 64-448), a pre-race RGBA16 tile at TMEM 256. Read from the load's start, each
 * drew the first texture of the load. LoadBlock data lies in TMEM as in DRAM (8 bytes a
 * word); LoadTile rows sit the load tile's line apart. FZX_LTOFS=0: from the load's start,
 * as before (A/B). */
static int gfx_ltofs SAVESTATE_EXCLUDE = -1;
static const uint8_t *gfx_tile_src(int tile, uint32_t *size_bytes, uint32_t *stride_bytes) {
    const int s = gfx_tex_slot(tile);
    const uint16_t tm = tile == 0 ? rdp.texture_tile.tmem : rdp.tiles[TEX1_TILE].tmem;
    const uint32_t lo = rdp.loaded_texture[s].tmem, hi = lo + (rdp.loaded_texture[s].size_bytes + 7) / 8;
    const uint8_t *p = rdp.loaded_texture[s].addr;
    uint32_t size = rdp.loaded_texture[s].size_bytes, stride = rdp.loaded_texture[s].stride_bytes;
    if (gfx_ltofs < 0) { const char *e = getenv("FZX_LTOFS"); gfx_ltofs = !(e && e[0] == '0'); }
    if (gfx_ltofs && p != NULL && rdp.loaded_texture[s].valid && tm > lo && tm < hi) {
        const uint32_t off = (uint32_t) (tm - lo) * 8u;
        if (stride == 0) {
            p += off;
            size = size > off ? size - off : 0;
        } else {
            const uint32_t lb = rdp.loaded_texture[s].line_bytes ? rdp.loaded_texture[s].line_bytes : 8u;
            const uint32_t rb = rdp.loaded_texture[s].row_bytes, row = off / lb, col = off % lb;
            const uint32_t rows = rb ? size / rb : 0;
            p += row * stride + col;
            size = rows > row && rb > col ? (rows - row) * (rb - col) : 0;
        }
    }
    if (size_bytes) *size_bytes = size;
    if (stride_bytes) *stride_bytes = stride;
    return p;
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
 * large it is. A tile sized wider than its texture to repeat it (OoT's adult
 * Link gauntlets: a 128-wide tile over a 16-wide texture, mask 4) otherwise
 * imports the bytes after the texture as texels. (ocarina-of-time ab65ddd,
 * super-smash-bros 9cd4f96.) FZX_TILEMASK=0 restores the whole-tile import. */
/* R23, the ports lead's final rule (round 2), on the scissor (gfx_dp_set_scissor): (a) an edge
 * within one N64 pixel of a screen edge goes to it, all four sides; (b) interior edges by pixel
 * centres, column c inside when x0 <= c + 0.5 < x1. On by default; it changes no pixel on the
 * autopilot route, the attract demos or the banner collapse (FZX_R23=0: off).
 * The 2D rectangles on pixel centres are their own switch, FZX_RECTCENTER=1 (gfx_draw_rectangle):
 * rectangles at exact half-pixel edges move one column left, 96/111 desktop frames change, and
 * z64 favours it on the differing pixels by a little (7,037 vs 6,753 of 13,877). Off until the
 * lead's call. */
static int gfx_r23 SAVESTATE_EXCLUDE = -1, gfx_rectcenter SAVESTATE_EXCLUDE = -1;
static inline bool gfx_r23_on(void) {
    if (gfx_r23 < 0) { const char *e = getenv("FZX_R23"); gfx_r23 = !(e && e[0] == '0'); }
    return gfx_r23 != 0;
}
static inline bool gfx_rectcenter_on(void) {
    if (gfx_rectcenter < 0) { const char *e = getenv("FZX_RECTCENTER"); gfx_rectcenter = e && e[0] == '1'; }
    return gfx_rectcenter != 0;
}
static int gfx_tilemask SAVESTATE_EXCLUDE = -1;
static inline bool gfx_tilemask_on(void) {
    if (gfx_tilemask < 0) { const char *e = getenv("FZX_TILEMASK"); gfx_tilemask = !(e && e[0] == '0'); }
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
#ifdef FZX_STAT
/* Census (make STAT=1): a tile that wraps (mask set, clamp off) yet is smaller than one
 * mask period corner to corner. The RDP subtracts the corner and then masks, so it reads
 * the whole period: a scroll that moves only the upper-left corner (star-fox-64 7725ecc:
 * Star Fox's ground and sea) shrinks the texture here, and so does a LoadTile window of a
 * wider image (paper-mario aa512fd). Logged once per texture, mask and lower-right corner;
 * "holds" = the loaded data and the tile's line hold the whole period (the tiles the
 * fix widens). */
extern unsigned int port_os_vi_count(void);
static void stat_tile_scroll(int tile, uint32_t w, uint32_t h) {
    static struct { const uint8_t *addr; uint16_t lrs, lrt; uint8_t ms, mt; } seen[256];
    static int nseen;
    const int slot = gfx_tex_slot(tile);
    const uint8_t ms = rdp.texture_tile.masks, mt = rdp.texture_tile.maskt;
    const bool s = ms != 0 && ms < 16 && !(rdp.texture_tile.cms & G_TX_CLAMP) && w < (1u << ms);
    const bool t = mt != 0 && mt < 16 && !(rdp.texture_tile.cmt & G_TX_CLAMP) && h < (1u << mt);
    if (!s && !t) return;
    const uint8_t *addr = rdp.loaded_texture[slot].addr;
    for (int i = 0; i < nseen; i++) {
        if (seen[i].addr == addr && seen[i].lrs == rdp.texture_tile.lrs && seen[i].lrt == rdp.texture_tile.lrt &&
            seen[i].ms == ms && seen[i].mt == mt) return;
    }
    if (nseen == 256) return;
    seen[nseen].addr = addr; seen[nseen].lrs = rdp.texture_tile.lrs; seen[nseen].lrt = rdp.texture_tile.lrt;
    seen[nseen].ms = ms; seen[nseen].mt = mt; nseen++;
    const uint32_t line = rdp.texture_tile.line_size_bytes * (rdp.texture_tile.siz == G_IM_SIZ_32b ? 2u : 1u);
    const uint32_t loaded = rdp.loaded_texture[slot].size_bytes;
    port_log("STATT vi %u: %s tile %d fmt %d siz %d corner %u,%u-%u,%u = %ux%u, mask %u,%u, cm %u,%u, line %u, "
             "loaded %u | s %s, t %s\n", port_os_vi_count(), rdp.loaded_texture[slot].stride_bytes ? "LoadTile" : "LoadBlock",
             tile, rdp.texture_tile.fmt, rdp.texture_tile.siz,
             rdp.texture_tile.uls, rdp.texture_tile.ult, rdp.texture_tile.lrs, rdp.texture_tile.lrt, w, h, ms, mt,
             rdp.texture_tile.cms, rdp.texture_tile.cmt, line, loaded,
             !s ? "-" : tex_bytes_per_row(1u << ms, rdp.texture_tile.siz) <= line ? "narrow, holds" : "narrow",
             !t ? "-" : (1u << mt) * line <= loaded ? "short, holds" : "short");
}
#endif
static inline void gfx_tile_extent(int tile, uint32_t *width, uint32_t *height);
static inline void gfx_tile_size(int tile, uint32_t *width, uint32_t *height) {
    gfx_tile_extent(tile, width, height);
    *width = gfx_mask_period(*width, rdp.texture_tile.masks);
    *height = gfx_mask_period(*height, rdp.texture_tile.maskt);
#ifdef FZX_STAT
    stat_tile_scroll(tile, *width, *height);
#endif
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

/* The TLUT the render tile reads: its bank for CI4, the table from entry 0 for CI8. */
static inline const uint8_t *gfx_tile_palette(void) {
    const uint8_t *p = rdp.tlut_bank[rdp.texture_tile.siz == G_IM_SIZ_4b ? (rdp.texture_tile.palette & 15) : 0];
    return p != NULL ? p : rdp.palette;
}

static uint32_t gfx_texture_content_hash(int tile, uint32_t fmt, uint32_t siz) {
    const uint8_t *palette = gfx_tile_palette();
    uint32_t size, stride;
    const uint8_t *src = gfx_tile_src(tile, &size, &stride);
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
        if (fmt == G_IM_FMT_CI && palette != NULL) {
            if (!gfx_is_static_memory(palette)) {
                return hash_bytes(h, palette, siz == G_IM_SIZ_4b ? 32 : 512);
            }
            // Static texels with a static palette: the palette's ADDRESS still
            // has to take part (Rainbow Road animates by cycling through
            // several TLUTs over the same CI texels).
            return (uint32_t) (uintptr_t) palette | 1u;
        }
        return 1;
    }
    if (stride == 0) {
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
    if (fmt == G_IM_FMT_CI && palette != NULL) {
        h = hash_bytes(h, palette, siz == G_IM_SIZ_4b ? 32 : 512);
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
/* FZX_MIRRORCLAMP: the RDP clamps a coordinate to the tile first and mirrors it by the mask
 * after, so a clamped tile no wider than its mask period never reaches the mirrored half: it
 * reads as a plain clamp at its own edge. The machines' decals (IA4 16x16, mirror|clamp, UVs
 * running to ~54 texels) show their edge column over most of the panel on the N64; doubled,
 * the clamp sat at texel 31, the mirrored copy of column 0, and painted the Blue Falcon's
 * shoulders and the other machines' panels in the decal colour. FZX_MIRRORCLAMP=0: doubled
 * as before. */
static int gfx_mirrorclamp SAVESTATE_EXCLUDE = -1;
static uint8_t gfx_mirror_flags(uint32_t width, uint32_t height) {
    uint8_t m = 0;
    if (rdp.texture_tile.cms & G_TX_MIRROR) m |= 1;
    if (rdp.texture_tile.cmt & G_TX_MIRROR) m |= 2;
    if (gfx_mirrorclamp < 0) { const char *e = getenv("FZX_MIRRORCLAMP"); gfx_mirrorclamp = !(e && e[0] == '0'); }
    if (m != 0 && gfx_mirrorclamp) {
        const int ew = ((int) rdp.texture_tile.lrs - (int) rdp.texture_tile.uls + 4) / 4;
        const int eh = ((int) rdp.texture_tile.lrt - (int) rdp.texture_tile.ult + 4) / 4;
        if ((rdp.texture_tile.cms & G_TX_CLAMP) && rdp.texture_tile.masks < 16 && ew <= (1 << rdp.texture_tile.masks)) m &= ~1;
        if ((rdp.texture_tile.cmt & G_TX_CLAMP) && rdp.texture_tile.maskt < 16 && eh <= (1 << rdp.texture_tile.maskt)) m &= ~2;
    }
    if (m != 0 && (width << (m & 1)) * (height << ((m >> 1) & 1)) > GFX_MAX_MIRROR_TEXELS) {
        m = 0;
    }
    return m;
}

static uint32_t gfx_texture_upload_bytes(int tile, uint32_t fmt, uint32_t siz, uint8_t mirror) {
    uint32_t width, height;
    gfx_tile_size(tile, &width, &height);
    uint32_t bpp = 4; /* gfx_soft stores every texture as ARGB8888 (5551 unpacked at upload) */
    uint32_t pw = 1, ph = 1;
    if (width * height > GFX_MAX_IMPORT_TEXELS) height = GFX_MAX_IMPORT_TEXELS / (width ? width : 1);
    width <<= mirror & 1;
    height <<= (mirror >> 1) & 1;
    while (pw < width) pw <<= 1;
    while (ph < height) ph <<= 1;
    return pw * ph * bpp;
}

/* The same loaded texels can be read through tiles of different sizes and row
 * strides: F-Zero X loads a 64x32 image as one block and draws Big Blue's start
 * line from a 16x16 tile of it. The texture built for one view is wrong for
 * another, so the view is part of a cached texture's identity. */
static uint32_t gfx_texture_view_key(int tile) {
    int w, h;
    uint32_t line;
    if (tile == 0) {
        uint32_t tw, th;
        gfx_tile_size(0, &tw, &th);
        w = (int) tw;
        h = (int) th;
        line = rdp.texture_tile.line_size_bytes;
    } else {
        w = ((int) rdp.tiles[TEX1_TILE].lrs - (int) rdp.tiles[TEX1_TILE].uls + 4) / 4;
        h = ((int) rdp.tiles[TEX1_TILE].lrt - (int) rdp.tiles[TEX1_TILE].ult + 4) / 4;
        if (w > 0) w = (int) gfx_mask_period((uint32_t) w, rdp.tiles[TEX1_TILE].masks);
        if (h > 0) h = (int) gfx_mask_period((uint32_t) h, rdp.tiles[TEX1_TILE].maskt);
        line = rdp.tiles[TEX1_TILE].line_size_bytes;
    }
    return ((uint32_t) w & 0x3ff) | ((uint32_t) h & 0x3ff) << 10 | (line & 0xfff) << 20;
}

/* FZX_TEXSTRESS=<n> (test): reset the texture cache every n new imports, as a long session's full
 * arena does. A reset only forces re-imports, so the frames must come out bit-identical. */
static bool gfx_tex_stress_hold SAVESTATE_EXCLUDE; /* the re-run pass after a reset: a real reset can't fire on the empty arena */
static bool gfx_tex_stress(void) {
    static int sEvery = -1, sCount;
    if (gfx_tex_stress_hold) return false;
    if (sEvery < 0) { const char *e = getenv("FZX_TEXSTRESS"); sEvery = e ? atoi(e) : 0; }
    if (sEvery <= 0 || ++sCount % sEvery != 0) return false;
    if ((sCount / sEvery) % 500 == 1) port_log("texstress: %d forced resets (every %d new imports)\n", sCount / sEvery, sEvery);
    return true;
}
static bool gfx_texture_cache_lookup(int tile, struct TextureHashmapNode **n, const uint8_t *orig_addr, uint32_t fmt, uint32_t siz, uint8_t mirror) {
    size_t hash = (uintptr_t)orig_addr;
    const uint32_t view = gfx_texture_view_key(tile);
    uint32_t content_hash = gfx_texture_content_hash(tile, fmt, siz);
    uint32_t upload_bytes = gfx_texture_upload_bytes(tile, fmt, siz, mirror);
    struct TextureHashmapNode *stale = NULL;
    hash = (hash >> 5) & 0x3ff;
    struct TextureHashmapNode **node = &gfx_texture_cache.hashmap[hash];
    while (*node != NULL && *node - gfx_texture_cache.pool < (int)gfx_texture_cache.pool_pos) {
        if ((*node)->texture_addr == orig_addr && (*node)->fmt == fmt && (*node)->siz == siz && (*node)->mirror == mirror &&
            (*node)->view == view) {
            if ((*node)->content_hash == content_hash) {
                gfx_rapi->select_texture(tile, (*node)->texture_id);
                {   /* FZX_SAMPCOUNT=1 (test): a hit on tile 1 sets TILE 0's sampler below (the lead's round,
                     * 2026-09-27, from PM64): count the hits where that changes tile 0's filter or wrap */
                    static int sCnt = -1; static unsigned sHits, sDiff;
                    if (sCnt < 0) sCnt = getenv("FZX_SAMPCOUNT") != NULL;
                    if (sCnt && tile == 1) {
                        const struct TextureHashmapNode *t0 = rendering_state.textures[0];
                        sHits++;
                        if (t0 && (t0->linear_filter != (*node)->linear_filter || t0->cms != (*node)->cms || t0->cmt != (*node)->cmt)) sDiff++;
                        if ((sHits & 1023) == 1) port_log("sampcount: %u tile-1 hits, %u change tile 0's sampler\n", sHits, sDiff);
                    }
                }
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
        gfx_texture_cache.pool_pos == sizeof(gfx_texture_cache.pool) / sizeof(struct TextureHashmapNode) || gfx_tex_stress()) {
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
    (*node)->view = view;
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

/* F-Zero X keeps 16/32-bit texels in native byte order (the ROM DMA's byte-order
 * maps swap them like every other u16/u32 asset, and the game's own CPU-made
 * textures are native too); MK64/OoT kept ROM order. */
#define FZX_NATIVE_TEXELS 1
static inline uint16_t be16(const uint8_t* p) {
#if FZX_NATIVE_TEXELS
    return *(const uint16_t*) p;
#else
    return (uint16_t) ((p[0] << 8) | p[1]);
#endif
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
    uint32_t stride;
    const uint8_t* src = gfx_tile_src(tile, NULL, &stride);
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
         * a55820b via ocarina-of-time 223b2af.) */
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
        const uint8_t *palette = gfx_tile_palette();
        if (palette == NULL) {
            return;
        }
        if (gfx_debug_frame) port_log("  ci import palette %p tex %p idx0 %02X %02X %02X %02X\n", palette, src, src[0], src[1], src[2], src[3]);
        for (y = 0; y < height; y++) {
            const uint8_t* row = TEXEL_ROW(y);
            for (x = 0; x < width; x++) {
                uint8_t idx = (siz == G_IM_SIZ_4b) ? nib(row, x) : row[x];
                out16[y * width + x] = n64_rgba16_to_psp5551(be16(palette + idx * 2));
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
#if FZX_NATIVE_TEXELS
                uint32_t w = *(const uint32_t*) (row + x * 4);
                r = w >> 24; g = (w >> 16) & 0xFF; b = (w >> 8) & 0xFF; a = w & 0xFF;
#else
                r = row[x * 4]; g = row[x * 4 + 1]; b = row[x * 4 + 2]; a = row[x * 4 + 3];
#endif
            } else if (fmt == G_IM_FMT_IA) {
                if (siz == G_IM_SIZ_4b) {
                    uint8_t v = nib(row, x);
                    r = g = b = SCALE_3_8(v >> 1);
                    a = (v & 1) ? 255 : 0;
                } else if (siz == G_IM_SIZ_8b) {
                    r = g = b = SCALE_4_8(row[x] >> 4);
                    a = SCALE_4_8(row[x] & 0xF);
                } else {
                    uint16_t v = be16(row + x * 2);
                    r = g = b = v >> 8;
                    a = v & 0xFF;
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
    if (gfx_texture_cache_lookup(tile, &rendering_state.textures[tile], gfx_tile_src(tile, NULL, NULL), fmt, siz, mirror)) {
        return;
    }
    import_texture_any(tile, mirror);
}

/* Tile 1 (TEXEL1) through the tile-0 import path: swap its descriptor in. Only
 * when it reads a different TMEM load than tile 0 (OoT: tile 1 at 0x100 for
 * room materials, 0x80 for the sky). */
static bool gfx_tile1_importable(void) {
    int s1 = gfx_lt_find(rdp.tiles[TEX1_TILE].tmem, false);
    return s1 != gfx_lt_find(rdp.texture_tile.tmem, false) && rdp.loaded_texture[s1].valid;
}

static void import_texture_tile1(void) {
    __typeof__(rdp.texture_tile) save = rdp.texture_tile;
    rdp.texture_tile.fmt = rdp.tiles[TEX1_TILE].fmt;
    rdp.texture_tile.siz = rdp.tiles[TEX1_TILE].siz;
    rdp.texture_tile.cms = gfx_tile_cm(rdp.tiles[TEX1_TILE].cms, rdp.tiles[TEX1_TILE].masks, rdp.tiles[TEX1_TILE].uls, rdp.tiles[TEX1_TILE].lrs);
    rdp.texture_tile.cmt = gfx_tile_cm(rdp.tiles[TEX1_TILE].cmt, rdp.tiles[TEX1_TILE].maskt, rdp.tiles[TEX1_TILE].ult, rdp.tiles[TEX1_TILE].lrt);
    rdp.texture_tile.masks = rdp.tiles[TEX1_TILE].masks;
    rdp.texture_tile.maskt = rdp.tiles[TEX1_TILE].maskt;
    rdp.texture_tile.uls = rdp.tiles[TEX1_TILE].uls;
    rdp.texture_tile.ult = rdp.tiles[TEX1_TILE].ult;
    rdp.texture_tile.lrs = rdp.tiles[TEX1_TILE].lrs;
    rdp.texture_tile.lrt = rdp.tiles[TEX1_TILE].lrt;
    rdp.texture_tile.line_size_bytes = rdp.tiles[TEX1_TILE].line_size_bytes;
    rdp.texture_tile.tmem = rdp.tiles[TEX1_TILE].tmem;
    rdp.texture_tile.palette = rdp.tiles[TEX1_TILE].palette;
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
        if (gfx_debug_frame) port_log("  blend skip n0 %p n1 %p tmem %u/%u\n", (void *) n0, (void *) n1, rdp.texture_tile.tmem, rdp.tiles[TEX1_TILE].tmem);
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
    iw1 = ((int) rdp.tiles[TEX1_TILE].lrs - (int) rdp.tiles[TEX1_TILE].uls + 4) / 4;
    ih1 = ((int) rdp.tiles[TEX1_TILE].lrt - (int) rdp.tiles[TEX1_TILE].ult + 4) / 4;
    w1 = gfx_mask_period((uint32_t) (iw1 < 1 ? 1 : iw1), rdp.tiles[TEX1_TILE].masks);
    h1 = gfx_mask_period((uint32_t) (ih1 < 1 ? 1 : ih1), rdp.tiles[TEX1_TILE].maskt);
    W = w0;
    H = h0;
    // Tile 1 repeats with its own period in tile-0 texels (its shift scales
    // it): cover both periods so the pre-blend tiles seamlessly.
    if (gfx_is_pow2(w0) && gfx_is_pow2(w1) && !(rdp.texture_tile.cms & G_TX_CLAMP)) {
        uint32_t sh = rdp.tiles[TEX1_TILE].shifts;
        uint32_t p = sh == 0 ? w1 : sh <= 10 ? w1 << sh : (w1 >> (16 - sh));
        if (p > W) W = p;
    }
    if (gfx_is_pow2(h0) && gfx_is_pow2(h1) && !(rdp.texture_tile.cmt & G_TX_CLAMP)) {
        uint32_t sh = rdp.tiles[TEX1_TILE].shiftt;
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
    key[6] = (uint32_t) (rdp.tiles[TEX1_TILE].uls >> 2) | ((uint32_t) (rdp.tiles[TEX1_TILE].ult >> 2) << 16);
    key[7] = rdp.tiles[TEX1_TILE].shifts | (rdp.tiles[TEX1_TILE].shiftt << 8) | (rgb_mode << 16) | (a_mode << 20) | (krgb_src << 24) | (ka_src << 28);
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
                                 rdp.texture_tile.uls >> 2, rdp.texture_tile.ult >> 2, rdp.tiles[TEX1_TILE].uls >> 2,
                                 rdp.tiles[TEX1_TILE].ult >> 2, rdp.tiles[TEX1_TILE].shifts, rdp.tiles[TEX1_TILE].shiftt, rgb_mode, a_mode,
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
/* The colour of a constant combiner input (PRIM, ENV, 0 or 1) into k; false if it is not one. */
static bool gfx_cc_const_rgb(uint8_t code, bool one, uint8_t k[3]) {
    const struct RGBA *c = code == CC_PRIM ? &rdp.prim_color : code == CC_ENV ? &rdp.env_color : NULL;
    if (c != NULL) {
        k[0] = c->r; k[1] = c->g; k[2] = c->b;
        return true;
    }
    if (code == CC_0) {
        k[0] = k[1] = k[2] = one ? 255 : 0;
        return true;
    }
    return false;
}

/* FZX_TEXFOG (2026-10-06): (K - TEXEL0) * SHADE + TEXEL0, K = PRIM or ENV, the texture faded
 * to a constant by the vertex colour. F-Zero X's venue floor (ovl_i3/background.c, D_303AA40)
 * draws it with K the course's fog colour and the vertex colour the haze
 * (Background_UpdateVenueFloorVtx: 0 on a line ahead of the camera, 255 at the floor's far and
 * near edges). Evaluated with the texel as 1, the vertex colour came out lerp(white, K, shade)
 * and the backend multiplied the texel by it, so the city under Mute City's track drew dark and
 * unhazed where the N64 (angrylion) fades it to the fog colour (BUILDLOG round 11). Now the
 * vertex colour is SHADE and the backend lerps per pixel. Cycle 1 alone, triangles only.
 * FZX_TEXFOG=0 restores the old fold. */
static int gfx_texfog_on = -1;
static bool gfx_cc_texfog(uint32_t cc_id) {
    if (gfx_texfog_on < 0) {
        const char *e = getenv("FZX_TEXFOG");
        gfx_texfog_on = !(e && e[0] == '0');
    }
    const uint32_t a = cc_id & 7, b = (cc_id >> 3) & 7, c = (cc_id >> 6) & 7, d = (cc_id >> 9) & 7;
    return gfx_texfog_on && b == CC_TEXEL0 && d == CC_TEXEL0 && c == CC_SHADE && (a == CC_PRIM || a == CC_ENV);
}

static void gfx_update_tex_lerp(uint32_t cc_id, bool cc2, bool tri) {
    extern void gfx_scegu_set_tex_lerp(bool on, uint8_t r, uint8_t g, uint8_t b);
    extern void gfx_scegu_set_tex_lerp2(bool on, const uint8_t a[3], const uint8_t b[3]);
    uint32_t bits = cc_id & 0xfff;
    uint8_t a = bits & 7, b = (bits >> 3) & 7, c = (bits >> 6) & 7, d = (bits >> 9) & 7;
    bool on = false;
    struct RGBA k = { 0, 0, 0, 0 };
    {
        /* (A - B) * TEXEL0 + B then COMBINED * SHADE: F-Zero X's machine bodies,
         * two body colours by an intensity panel, lit. The K-lerp below cannot
         * take the cycle-2 multiply (and was off for it, so texel 0 drew black). */
        bool on2 = false;
        uint8_t ka[3], kb[3];
        const uint8_t *c2 = rdp.cc2;
        if (cc2 && c2[0] == G_CCMUX_COMBINED && c2[1] >= 8 && c2[2] == G_CCMUX_SHADE && c2[3] == 7 &&
            c == CC_TEXEL0 && b == d && !(cc_id & CC_FLAG_RGB_D_ONE) &&
            gfx_cc_const_rgb(a, (cc_id & CC_FLAG_RGB_A_ONE) != 0, ka) && gfx_cc_const_rgb(b, false, kb)) {
            on2 = true;
        }
        if (on2 != rendering_state.tex_lerp2 ||
            (on2 && (memcmp(ka, rendering_state.lerp2_a, 3) != 0 || memcmp(kb, rendering_state.lerp2_b, 3) != 0))) {
            gfx_flush();
            gfx_scegu_set_tex_lerp2(on2, ka, kb);
            rendering_state.tex_lerp2 = on2;
            if (on2) {
                memcpy(rendering_state.lerp2_a, ka, 3);
                memcpy(rendering_state.lerp2_b, kb, 3);
            }
        }
        tri_state.tex_lerp2 = on2;
    }
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
    {
        extern void gfx_scegu_set_tex_fog(bool on, uint8_t r, uint8_t g, uint8_t b);
        const bool fog = tri && !cc2 && gfx_cc_texfog(cc_id);
        struct RGBA hh = { 0, 0, 0, 0 };
        if (fog) {
            hh = (cc_id & 7) == CC_PRIM ? rdp.prim_color : rdp.env_color;
        }
        if (fog != rendering_state.tex_fog ||
            (fog && (hh.r != rendering_state.fog_k.r || hh.g != rendering_state.fog_k.g || hh.b != rendering_state.fog_k.b))) {
            gfx_flush();
            gfx_scegu_set_tex_fog(fog, hh.r, hh.g, hh.b);
            rendering_state.tex_fog = fog;
            rendering_state.fog_k = hh;
        }
        tri_state.tex_fog = fog;
    }
}

static inline float dot(const float a[3], const float b[3])
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

/* X31 (the lead, 2026-09-29, from OoT 6f1ac0e): the light and lookat directions come into model space
 * through the modelview, so for a model drawn small they are ~0.003 long; a 1e-5 floor on the squared
 * length left them unnormalized (n.l ~ 0: no directional light, no env-map shine). The RSP normalizes
 * any length: so does this, unless FZX_NORMANY=0 (the old floor). */
static void gfx_normalize_vector(float v[3]) {
    static float sFloor = -1.0f;
    if (sFloor < 0.0f) { const char *e = getenv("FZX_NORMANY"); sFloor = (e && e[0] == '0') ? 0.00001f : 1e-30f; }
    float dot = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if(dot > sFloor){
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
#if FZX_NATIVE_TEXELS
    /* F-Zero X: Mtx is kept in its u16 i[4][4] / f[4][4] view (see guMtxF2L) */
    {
        const uint16_t *ip = (const uint16_t *) addr, *fp = (const uint16_t *) addr + 16;
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                matrix[i][j] = (int32_t) (((uint32_t) ip[i * 4 + j] << 16) | fp[i * 4 + j]) / 65536.0f;
            }
        }
    }
#else
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            int32_t int_part = addr[i * 2 + j / 2];
            uint32_t frac_part = addr[8 + i * 2 + j / 2];
            matrix[i][j] = (int32_t)((int_part & 0xffff0000) | (frac_part >> 16)) / 65536.0f;
            matrix[i][j + 1] = (int32_t)((int_part << 16) | (frac_part & 0xffff)) / 65536.0f;
        }
    }
#endif
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
    if (gfx_debug_frame) {
        for (size_t k = 0; k < n_vertices && k < 32; k++) {
            port_log("  vin %p[%u] ob %d %d %d flag %u tc %d %d cn %02X%02X%02X%02X\n", (void*) vertices, (unsigned) k,
                     vertices[k].v.ob[0], vertices[k].v.ob[1], vertices[k].v.ob[2], vertices[k].v.flag,
                     vertices[k].v.tc[0], vertices[k].v.tc[1], vertices[k].v.cn[0], vertices[k].v.cn[1], vertices[k].v.cn[2], vertices[k].v.cn[3]);
        }
    }

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
    /* gfx_adjust_x_for_aspect_ratio's quotient, once per batch (same operands,
     * same float; ocarina-of-time c935b73). */
    const float asp_q = (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(TARGET_PSP)
    /* The two matrices' rows in NEON registers for the batch: per vertex
     * ((t0*M0 + t1*M1) + t2*M2) + t3*M3 in lanes, the scalar order (VMLA is
     * not fused). */
    const float (*_MPn)[4] = rsp.MP_matrix;
    const float (*_MVn)[4] = rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1];
    const float32x4_t mp0 = vld1q_f32(_MPn[0]), mp1 = vld1q_f32(_MPn[1]), mp2 = vld1q_f32(_MPn[2]), mp3 = vld1q_f32(_MPn[3]);
    const float32x4_t mv0 = vld1q_f32(_MVn[0]), mv1 = vld1q_f32(_MVn[1]), mv2 = vld1q_f32(_MVn[2]), mv3 = vld1q_f32(_MVn[3]);
#endif
    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const Vtx_t *v = &vertices[i].v;
        const Vtx_tn *vn = &vertices[i].n;
        struct LoadedVertex *d = &rsp.loaded_vertices[dest_index];
        int fog_alpha = -1; // >= 0: the RSP fog factor replaces the vertex alpha
        int flx_alpha = -1; // >= 0: the F3DFLX2 reflection alpha replaces it

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
            const float t0 = temp_vec[0], t1 = temp_vec[1], t2 = temp_vec[2], t3 = temp_vec[3];
            vst1q_f32(proj_vec, vmlaq_n_f32(vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(mp0, t0), mp1, t1), mp2, t2), mp3, t3));
            vst1q_f32(view_vec, vmlaq_n_f32(vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(mv0, t0), mv1, t1), mv2, t2), mv3, t3));
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
        const float x = rsp.is_persp ? proj_vec[0] * (4.0f / 3.0f) / asp_q : proj_vec[0]; /* gfx_adjust_x_for_aspect_ratio */
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
                /* Texgen axes: the task's gSPLookAt, s from LOOKATX and t from LOOKATY, as the
                 * console's microcode does (goldeneye-007; ocarina-of-time 5c60b2c). F-Zero X sets
                 * one, from the camera, for the machines' chrome where F3DFLX2's reflection table
                 * isn't loaded: the GP ending (racer.c), machine.c's machines, machine creation.
                 * Each task starts from the fixed axes the port always used (gfx_sp_reset). A zero
                 * vector gives n.l = 0 on every vertex, the texture's centre, as on the RSP
                 * (normalising it would give NaNs). FZX_LOOKAT=0: the fixed axes always (A/B). */
                static const Light_t lookat_x = {{0, 0, 0}, 0, {0, 0, 0}, 0, {127, 0, 0}, 0};
                static const Light_t lookat_y = {{0, 0, 0}, 0, {0, 0, 0}, 0, {0, 127, 0}, 0};
                static int sLookAt = -1;
                if (sLookAt < 0) { const char *e = getenv("FZX_LOOKAT"); sLookAt = !(e && e[0] == '0'); }
                const Light_t *ls = sLookAt ? &rsp.current_lookat[1] : &lookat_x, *lt = sLookAt ? &rsp.current_lookat[0] : &lookat_y;
                if (ls->dir[0] | ls->dir[1] | ls->dir[2]) calculate_normal_dir(ls, rsp.current_lookat_coeffs[0]);
                else memset(rsp.current_lookat_coeffs[0], 0, sizeof(rsp.current_lookat_coeffs[0]));
                if (lt->dir[0] | lt->dir[1] | lt->dir[2]) calculate_normal_dir(lt, rsp.current_lookat_coeffs[1]);
                else memset(rsp.current_lookat_coeffs[1], 0, sizeof(rsp.current_lookat_coeffs[1]));
                if (gfx_flx_light_valid) {
                    gfx_transposed_matrix_mul(gfx_flx_light_model, gfx_flx_light, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
                    gfx_normalize_vector(gfx_flx_light_model);
                }
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
            if (gfx_debug_frame && i < 2)
                port_log("  lit n %d %d %d nl %d L0 col %02X%02X%02X dir %d %d %d coef %.2f %.2f %.2f amb %02X%02X%02X -> %02X%02X%02X\n",
                         vn->n[0], vn->n[1], vn->n[2], rsp.current_num_lights, rsp.current_lights[0].col[0], rsp.current_lights[0].col[1],
                         rsp.current_lights[0].col[2], rsp.current_lights[0].dir[0], rsp.current_lights[0].dir[1], rsp.current_lights[0].dir[2],
                         rsp.current_lights_coeffs[0][0], rsp.current_lights_coeffs[0][1], rsp.current_lights_coeffs[0][2],
                         rsp.current_lights[rsp.current_num_lights - 1].col[0], rsp.current_lights[rsp.current_num_lights - 1].col[1],
                         rsp.current_lights[rsp.current_num_lights - 1].col[2], d->color.r, d->color.g, d->color.b);
            
            if ((rsp.geometry_mode & G_TEXTURE_GEN) && gfx_flx_light_valid) {
                /* F3DFLX2: the reflection alpha instead of texgen (UVs stay the model's). */
                int li = (int) (vn->n[0] * gfx_flx_light_model[0] + vn->n[1] * gfx_flx_light_model[1] + vn->n[2] * gfx_flx_light_model[2]);
                li = li < -128 ? -128 : li > 127 ? 127 : li;
                flx_alpha = gfx_flx_lut[128 + li];
            } else if (rsp.geometry_mode & G_TEXTURE_GEN) {
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
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(TARGET_PSP)
            /* The tests below in NEON lanes (round 12): each VFP compare moved its flags to the
             * core (vmrs), fourteen a vertex. x > b is tested as -x < -b (negation is exact), and
             * every bound is |w| times the same constant, so the flags are the same bits. */
            const float32x4_t AW = vabsq_f32(vdupq_n_f32(w));
            const float32x4_t XY = { x, -x, y, -y }, P = { proj_vec[0], -proj_vec[0], proj_vec[1], -proj_vec[1] };
            const float32x4_t K = { -gfx_clip_rx, -gfx_clip_rx, -gfx_clip_ry, -gfx_clip_ry };
            static const uint32_t kGuard[4] = { X_POS, X_NEG, Y_POS, Y_NEG }, kOut[4] = { X_OUT_L, X_OUT_R, Y_OUT_B, Y_OUT_T };
            uint32x4_t r = vandq_u32(vcltq_f32(XY, vnegq_f32(vmulq_n_f32(AW, GUARD_BAND))), vld1q_u32(kGuard));
            r = vorrq_u32(r, vandq_u32(vcltq_f32(XY, vdupq_n_f32(-w)), vld1q_u32(kOut)));
            r = vorrq_u32(r, vandq_u32(vcltq_f32(P, vmulq_f32(K, AW)), vdupq_n_u32(GB_OUT)));
            const float32x2_t ZZ = { z, -z };
            const float32x2_t ZB = { -w, gfx_fix_farrej ? -w : -(vget_lane_f32(vget_low_f32(AW), 0) * FAR_GUARD) };
            const uint32x2_t zbits = { Z_POS, Z_NEG };
            const uint32x2_t r2 = vorr_u32(vorr_u32(vget_low_u32(r), vget_high_u32(r)), vand_u32(vclt_f32(ZZ, ZB), zbits));
            d->clip_rej = vget_lane_u32(r2, 0) | vget_lane_u32(r2, 1);
#else
            float aw = w < 0.0f ? -w : w;
            float gw = GUARD_BAND * aw;
            uint32_t rej = 0; /* gathered, stored once (each |= was a load/orr/store) */
            if (x < -gw) rej |= X_POS;
            if (x > gw) rej |= X_NEG;
            if (y < -gw) rej |= Y_POS;
            if (y > gw) rej |= Y_NEG;
            if (x < -w) rej |= X_OUT_L;
            if (x > w) rej |= X_OUT_R;
            if (y < -w) rej |= Y_OUT_B;
            if (y > w) rej |= Y_OUT_T;
            /* R33: the RSP's guard band, in the N64's clip space (x before the aspect crop) */
            if (proj_vec[0] < -gfx_clip_rx * aw || proj_vec[0] > gfx_clip_rx * aw ||
                proj_vec[1] < -gfx_clip_ry * aw || proj_vec[1] > gfx_clip_ry * aw) rej |= GB_OUT;
            // In front of the game's near plane (z_ndc < -1) or behind the eye (MK64
            // 4f64001): was w < W_NEAR, which the trivial reject treated as
            // "outside" and so deleted every triangle within 24 units of the eye.
            if (z < -w) rej |= Z_POS;
            if (gfx_fix_farrej ? z > w : z > FAR_GUARD * aw) rej |= Z_NEG;  // beyond the far plane
            d->clip_rej = rej;
#endif
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
        d->color.a = fog_alpha >= 0 ? (uint8_t) fog_alpha : flx_alpha >= 0 ? (uint8_t) flx_alpha : v->cn[3];
    }
#ifdef PORT_PROFILE_DL
    gfx_prof_vtx += port_time_us() - _pv0;
    gfx_prof_vtxcount += n_vertices;
#endif
#if !defined(PORT_GE_TL)
    if (!rsp.is_persp && n_vertices > 0 && hud_fit_mode()) { /* the race HUD unsquished (hud_fit_mode): one placement a load */
        struct LoadedVertex *lv = &rsp.loaded_vertices[dest_index - n_vertices];
        const float vx = gfx_vpf.x, vw = gfx_vpf.w; /* (rdp.viewport, or its fractional edges under the unified fill) */
        const float vy = gfx_vpf.y, vh = gfx_vpf.h;
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        size_t i;
        for (i = 0; i < n_vertices; i++) { /* where the squeeze puts it, back in the 320x240 layout */
            const float w = lv[i]._w, inv = (w > 1e-6f || w < -1e-6f) ? 1.0f / w : 1.0f;
            const float lx = out2lay_x(vx + (lv[i]._x * inv * 0.5f + 0.5f) * vw);
            const float ly = out2lay_y(vy + (0.5f - lv[i]._y * inv * 0.5f) * vh);
            if (lx < x0) x0 = lx;
            if (lx > x1) x1 = lx;
            if (ly < y0) y0 = ly;
            if (ly > y1) y1 = ly;
        }
        const struct HudPlace p = hud_fit_place(x0, x1, y0, y1);
        if (!p.full && vw > 0.0f) {
            for (i = 0; i < n_vertices; i++) {
                const float w = lv[i]._w, ww = (w > 1e-6f || w < -1e-6f) ? w : 1.0f, inv = 1.0f / ww;
                const float lx = out2lay_x(vx + (lv[i]._x * inv * 0.5f + 0.5f) * vw);
                const float ly = out2lay_y(vy + (0.5f - lv[i]._y * inv * 0.5f) * vh);
                lv[i]._x = ((hud_fit_x(lx, &p) - vx) / vw * 2.0f - 1.0f) * ww;
                if (vh > 0.0f) lv[i]._y = (1.0f - (hud_fit_y(ly, &p) - vy) / vh * 2.0f) * ww;
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

/* FZX_CCSHADEA: a texture-free cycle-1 colour whose multiplier (C) is the shade alpha, as the
 * machines' explosion debris, (1 - SHADE) * SHADE_ALPHA + SHADE (hud_gfx D_4007FD8): each shard
 * whitened by its own alpha. The packed combiner id has no code for an alpha in C and read it as
 * 0, so every shard came out in the plain machine colour (saturated blue) where the N64 shows
 * pale and white shards. Set on G_SETCOMBINE; the vertex colour is then evaluated from the raw
 * cycle-1 inputs. FZX_CCSHADEA=0: as before. */
static bool gfx_cc1_shadea;
static struct RGBA gfx_blend_color;  /* G_SETBLENDCOLOR (FZX_BLENDCOL) */
static bool gfx_blendcol_tri;        /* this triangle is drawn in the blend colour */
static int gfx_ccshadea_on SAVESTATE_EXCLUDE = -1;
static void gfx_cc1_shadea_set(void) {
    const uint8_t *c = rdp.cc1;
    if (gfx_ccshadea_on < 0) { const char *e = getenv("FZX_CCSHADEA"); gfx_ccshadea_on = !(e && e[0] == '0'); }
    gfx_cc1_shadea = gfx_ccshadea_on && c[2] == G_CCMUX_SHADE_ALPHA &&
                     c[0] != G_CCMUX_TEXEL0 && c[0] != G_CCMUX_TEXEL1 && c[0] != G_CCMUX_COMBINED &&
                     c[1] != G_CCMUX_TEXEL0 && c[1] != G_CCMUX_TEXEL1 && c[1] != G_CCMUX_COMBINED &&
                     c[3] != G_CCMUX_TEXEL0 && c[3] != G_CCMUX_TEXEL1 && c[3] != G_CCMUX_COMBINED;
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
    // A decal of the texture over an input by the texel's own alpha,
    // (TEXEL0 - X) * TEXEL0_ALPHA + X (MK64 v30 bea7db7; F-Zero X's track and
    // machines). With texels as 1.0 it folds to white; gfx_soft does the
    // per-pixel lerp itself (rgb_decal) and needs X as the vertex colour.
    const uint32_t xb = (cc_id >> 3) & 7;
    const bool decal = (cc_id & 7) == CC_TEXEL0 && ((cc_id >> 6) & 7) == CC_TEXEL0A && xb == ((cc_id >> 9) & 7) &&
                       (xb == CC_PRIM || xb == CC_SHADE || xb == CC_ENV || xb == CC_LOD);
    const bool texfog = tri_state.tex_fog && gfx_cc_texfog(cc_id);

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
        if (decal && ch < 3) v = d;
        if (texfog && ch < 3) v = c; // SHADE; the backend lerps the texel to K by it
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



#ifdef FZX_STAT
/* Census (make STAT=1) for the ports lead's texture rules, per triangle state. Each
 * distinct case is logged once (STATX) and the triangles drawn in it are counted
 * (totals at exit):
 *  R29: the render tile, or TEXEL1's tile, has a shift (the RDP scales S/T by it);
 *  R25: a real cycle 2 reads TEXEL0/TEXEL1 (which the second cycle swaps);
 *  R27: a texture unit's tile starts inside a load rather than at its start. */
enum { ST_SHIFT0, ST_SHIFT1, ST_CC2TEX, ST_LTOFS0, ST_LTOFS1, ST_N };
static uint32_t sStTexFlags;
static unsigned long long sStTexTris[ST_N];
static void stat_tex_report(void) {
    static const char *nm[ST_N] = { "R29 render tile with a shift", "R29 TEXEL1 tile with a shift",
                                    "R25 cycle 2 reads a texel", "R27 unit 0 tile inside a load",
                                    "R27 unit 1 tile inside a load" };
    for (int k = 0; k < ST_N; k++) port_log("STATX total %s: %llu triangles\n", nm[k], sStTexTris[k]);
}
static void stat_tex_note(int kind, uint32_t a, uint32_t b, uint32_t c, const char *what) {
    static struct { uint32_t kind, a, b, c; } seen[256];
    static int nseen;
    sStTexFlags |= 1u << kind;
    for (int i = 0; i < nseen; i++) {
        if (seen[i].kind == (uint32_t) kind && seen[i].a == a && seen[i].b == b && seen[i].c == c) return;
    }
    if (nseen == 256) return;
    seen[nseen].kind = (uint32_t) kind; seen[nseen].a = a; seen[nseen].b = b; seen[nseen].c = c; nseen++;
    port_log("STATX vi %u: %s\n", port_os_vi_count(), what);
}
/* TMEM words into the load that covers `tmem`, or -1 when a load starts there or none covers it */
static int stat_lt_offset(uint16_t tmem, uint16_t *start) {
    if (gfx_lt_find(tmem, true) >= 0) return -1;
    for (int i = 0; i < GFX_LT_SLOTS; i++) {
        const uint32_t lo = rdp.loaded_texture[i].tmem, hi = lo + (rdp.loaded_texture[i].size_bytes + 7) / 8;
        if (rdp.loaded_texture[i].valid && tmem > lo && tmem < hi) { *start = (uint16_t) lo; return (int) (tmem - lo); }
    }
    return -1;
}
static void stat_tex_state(bool use_texture, bool uses_t1, bool cc2) {
    static int reg;
    char buf[200];
    if (!reg) { reg = 1; atexit(stat_tex_report); }
    sStTexFlags = 0;
    if (use_texture) {
        const int ts[2] = { gfx_render_tile, TEX1_TILE };
        for (int u = 0; u < (uses_t1 ? 2 : 1); u++) {
            const int t = ts[u];
            uint16_t start;
            int ofs;
            if (rdp.tiles[t].shifts || rdp.tiles[t].shiftt) {
                snprintf(buf, sizeof(buf), "R29 %s tile %d: shift s %u t %u, tmem %u, fmt %u siz %u, cc %08X",
                         u ? "TEXEL1" : "render", t, rdp.tiles[t].shifts, rdp.tiles[t].shiftt, rdp.tiles[t].tmem,
                         rdp.tiles[t].fmt, rdp.tiles[t].siz, (unsigned) rdp.combine_mode);
                stat_tex_note(u ? ST_SHIFT1 : ST_SHIFT0, (uint32_t) t | rdp.tiles[t].shifts << 8 | rdp.tiles[t].shiftt << 16,
                              rdp.tiles[t].tmem, rdp.combine_mode, buf);
            }
            if ((ofs = stat_lt_offset(rdp.tiles[t].tmem, &start)) > 0) {
                snprintf(buf, sizeof(buf), "R27 unit %d tile %d at tmem %u: %d words into the load at %u, fmt %u siz %u",
                         u, t, rdp.tiles[t].tmem, ofs, start, rdp.tiles[t].fmt, rdp.tiles[t].siz);
                stat_tex_note(u ? ST_LTOFS1 : ST_LTOFS0, (uint32_t) t, rdp.tiles[t].tmem, start, buf);
            }
        }
    }
    if (cc2) {
        bool reads = false;
        for (int i = 0; i < 8; i++) {
            const uint8_t m = rdp.cc2[i];
            reads |= m == G_CCMUX_TEXEL0 || m == G_CCMUX_TEXEL1; /* 1, 2: the texels in every slot, colour or alpha */
            if (i == 2) reads |= m == G_CCMUX_TEXEL0_ALPHA || m == G_CCMUX_TEXEL1_ALPHA;
        }
        if (reads) {
            snprintf(buf, sizeof(buf), "R25 cycle 2 reads a texel: cc %08X, cc2 [%u %u %u %u | %u %u %u %u]",
                     (unsigned) rdp.combine_mode, rdp.cc2[0], rdp.cc2[1], rdp.cc2[2], rdp.cc2[3], rdp.cc2[4], rdp.cc2[5],
                     rdp.cc2[6], rdp.cc2[7]);
            stat_tex_note(ST_CC2TEX, rdp.combine_mode, (uint32_t) rdp.cc2[0] | rdp.cc2[1] << 8 | rdp.cc2[2] << 16 | (uint32_t) rdp.cc2[3] << 24,
                          (uint32_t) rdp.cc2[4] | rdp.cc2[5] << 8 | rdp.cc2[6] << 16 | (uint32_t) rdp.cc2[7] << 24, buf);
        }
    }
}
static void stat_tex_tri(void) {
    for (int k = 0; k < ST_N; k++) if (sStTexFlags & (1u << k)) sStTexTris[k]++;
}
/* R33: why perspective triangles go to the clipper, and the clip ratios the game sets. */
static unsigned long long sStClip[4]; /* tris, near/far, guard band only, old distance test only */
static void stat_clip_report(void) {
    port_log("STATX total clip: %llu perspective triangles, near/far %llu, guard band only %llu, old distance test only %llu\n",
             sStClip[0], sStClip[1], sStClip[2], sStClip[3]);
}
static void stat_clip(uint32_t any_rej, bool old_trig) {
    static int reg;
    if (!reg) { reg = 1; atexit(stat_clip_report); }
    sStClip[0]++;
    if (any_rej & (Z_POS | Z_NEG)) sStClip[1]++;
    else if (any_rej & GB_OUT) sStClip[2]++;
    else if (old_trig) sStClip[3]++;
}
/* R35: fogged triangles. Opaque fog modes blend the fog colour per pixel after the texture
 * (post_fog, as the RDP); translucent ones fold it into the vertex colour, which is exact only
 * for black fog on a combine that reads the vertex colour. A decal (no SHADE in the colour
 * combine) loses folded fog entirely (MK64 7d23507's new class). Each folded case logged once. */
static unsigned long long sStFog[6]; /* per pixel: shaded, decal; folded: black shaded, black decal,
                                        non-black shaded, non-black decal */
static void stat_fog_report(void) {
    port_log("STATX total fog: per pixel %llu (decals %llu); folded: black %llu, black decal %llu, non-black %llu, non-black decal %llu triangles\n",
             sStFog[0] + sStFog[1], sStFog[1], sStFog[2], sStFog[3], sStFog[4], sStFog[5]);
}
static bool stat_reads_shade(uint32_t cc_id, bool cc2) {
    for (int k = 0; k < 4; k++) if (((cc_id >> (k * 3)) & 7) == CC_SHADE) return true;
    if (cc2) for (int k = 0; k < 4; k++) if (rdp.cc2[k] == G_CCMUX_SHADE) return true;
    return false;
}
static void stat_fog_tri(void) {
    static int reg;
    static struct { uint32_t col, cc; } seen[64];
    static int nseen;
    if (!reg) { reg = 1; atexit(stat_fog_report); }
    if (!tri_state.use_fog) return;
    const bool decal = !stat_reads_shade(tri_state.cc_id, tri_state.cc2);
    if (tri_state.post_fog) { sStFog[decal ? 1 : 0]++; return; }
    const bool black = !rdp.fog_color.r && !rdp.fog_color.g && !rdp.fog_color.b;
    sStFog[2 + (black ? 0 : 2) + (decal ? 1 : 0)]++;
    const uint32_t col = (uint32_t) rdp.fog_color.r << 24 | rdp.fog_color.g << 16 | rdp.fog_color.b << 8 | rdp.fog_color.a;
    for (int i = 0; i < nseen; i++) if (seen[i].col == col && seen[i].cc == tri_state.cc_id) return;
    if (nseen == 64) return;
    seen[nseen].col = col; seen[nseen].cc = tri_state.cc_id; nseen++;
    port_log("STATX vi %u: R35 folded fog %08X, cc_id %08X, decal %d, textured %d, rm %08X\n", port_os_vi_count(), (unsigned) col,
             (unsigned) tri_state.cc_id, decal, tri_state.use_texture, (unsigned) rdp.other_mode_l);
}
static void stat_clip_ratio(int axis, int r) {
    static uint32_t seen;
    const uint32_t bit = 1u << ((axis ? 16 : 0) + (r & 15));
    if (seen & bit) return;
    seen |= bit;
    port_log("STATX vi %u: clip ratio %c %d\n", port_os_vi_count(), axis ? 'y' : 'x', r);
}
/* B5 (MK64 c400d1b, 259e562): the tint (1 - X) * TEXEL0 + Y and the lerp (X - Y) * TEXEL0 + Y
 * combines, by how this renderer draws them. Triangles and rectangles counted; each distinct
 * case logged once. */
enum { B5_TINT_ADD, B5_TINT_ADD_X, B5_TINT_Y0, B5_TINT_RECT, B5_TINT_OTHER, B5_LERP_PX, B5_LERP_Y0, B5_LERP_CC2, B5_N };
static const char *const sB5Name[B5_N] = {
    "tint as texel + Y, X = 0 (exact)", "tint as texel + Y, X != 0 (drops 1 - X)", "tint with Y = 0 (exact)",
    "tint on a 2D rect, Y != 0 (no add)", "tint with a shaded or LOD input", "lerp per pixel (exact)",
    "lerp with Y = 0 (exact)", "lerp in 2-cycle (folded)" };
static unsigned long long sStB5[B5_N];
static int sStB5Cls = -1;
static void stat_b5_report(void) {
    for (int k = 0; k < B5_N; k++) port_log("STATX total B5 %s: %llu\n", sB5Name[k], sStB5[k]);
}
static bool gfx_cc_const_rgb(uint8_t code, bool one, uint8_t k[3]);
static void stat_b5_state(uint32_t cc_id, bool cc2, bool rect, int color_mode, bool add) {
    static int reg;
    static struct { uint32_t cls, cc, x, y; } seen[128];
    static int nseen;
    const uint8_t a = cc_id & 7, b = (cc_id >> 3) & 7, c = (cc_id >> 6) & 7, d = (cc_id >> 9) & 7;
    const bool a_one = (cc_id & CC_FLAG_RGB_A_ONE) != 0, d_one = (cc_id & CC_FLAG_RGB_D_ONE) != 0;
    uint8_t kx[3] = { 0, 0, 0 }, ky[3] = { 0, 0, 0 };
    int cls = -1;
    if (!reg) { reg = 1; atexit(stat_b5_report); }
    sStB5Cls = -1;
    if (c != CC_TEXEL0 || d_one) return;
    if (a == CC_0 && a_one && (d == CC_PRIM || d == CC_ENV) && b != d) {
        gfx_cc_const_rgb(d, false, ky);
        const bool xk = gfx_cc_const_rgb(b, false, kx);
        if (!xk) cls = B5_TINT_OTHER;
        else if (!(ky[0] | ky[1] | ky[2])) cls = B5_TINT_Y0;
        else if (rect) cls = B5_TINT_RECT; /* the 2D path never adds (its color_mode may be unset) */
        else if (color_mode != 1) cls = B5_TINT_OTHER;
        else if (add) cls = (kx[0] | kx[1] | kx[2]) ? B5_TINT_ADD_X : B5_TINT_ADD;
        else cls = B5_TINT_OTHER;
    } else if (b == d && (b == CC_PRIM || b == CC_ENV) && (a != b || a_one)) {
        gfx_cc_const_rgb(b, false, ky);
        gfx_cc_const_rgb(a, a_one, kx);
        if (cc2 && tri_state.tex_lerp2) return; /* the machine bodies' two-colour lerp: its own path */
        cls = cc2 ? B5_LERP_CC2 : (ky[0] | ky[1] | ky[2]) ? B5_LERP_PX : B5_LERP_Y0;
    }
    if (cls < 0) return;
    sStB5Cls = cls;
    const uint32_t x = (uint32_t) kx[0] << 16 | kx[1] << 8 | kx[2], y = (uint32_t) ky[0] << 16 | ky[1] << 8 | ky[2];
    for (int i = 0; i < nseen; i++) {
        if (seen[i].cls == (uint32_t) cls && seen[i].cc == cc_id && seen[i].x == x && seen[i].y == y) return;
    }
    if (nseen == 128) return;
    seen[nseen].cls = (uint32_t) cls; seen[nseen].cc = cc_id; seen[nseen].x = x; seen[nseen].y = y; nseen++;
    port_log("STATX vi %u: B5 %s: cc %08X, X %06X, Y %06X%s\n", port_os_vi_count(), sB5Name[cls], (unsigned) cc_id,
             (unsigned) x, (unsigned) y, rect ? ", 2D rect" : "");
}
static void stat_b5_count(void) {
    if (sStB5Cls >= 0) sStB5[sStB5Cls]++;
}
/* R34 (Smash 3b03a66): texture rectangles drawn with the colour image set to the depth buffer,
 * which the RDP writes into depth, never onto the screen. The first few logged, all counted. */
static unsigned long long sStR34;
static void stat_r34_report(void) {
    port_log("STATX total R34 texrects into the Z image: %llu\n", sStR34);
}
static void stat_r34(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    static int reg;
    if (!reg) { reg = 1; atexit(stat_r34_report); }
    if (rdp.color_image_address != rdp.z_buf_address) return;
    if (sStR34++ < 8) {
        port_log("STATX vi %u: R34 texrect into the Z image %d,%d-%d,%d, cc %08X\n", port_os_vi_count(), (int) ulx,
                 (int) uly, (int) lrx, (int) lry, (unsigned) rdp.combine_mode);
    }
}
#endif
uint32_t gfx_prof_rebuilds;
static void gfx_tri_rebuild_state(struct LoadedVertex *v1) {
    gfx_prof_rebuilds++;
    /* N64: the RSP produces z with G_ZBUFFER; the RDP compares only with Z_CMP and
     * writes only with Z_UPD, independently (F-Zero X's track: Z_UPD alone). */
    bool depth_test = (rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER && (rdp.other_mode_l & Z_CMP) == Z_CMP;
    if (depth_test != rendering_state.depth_test) {
        gfx_flush();
        gfx_rapi->set_depth_test(depth_test);
        rendering_state.depth_test = depth_test;
    }
    bool z_upd = (rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER && (rdp.other_mode_l & Z_UPD) == Z_UPD;
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
    {
        /* ...and only if that cycle blends with the framebuffer at all (P or M =
         * G_BL_CLR_MEM). F-Zero X's track draws in 1-cycle G_RM_FOG_SHADE_A-like
         * mode (FOG*shadeA + IN*(1-shadeA), no memory): opaque, the shade alpha
         * being the fog factor - treating it as opacity made the road vanish. */
        uint32_t p = two_cycle ? (rdp.other_mode_l >> 28) & 3 : (rdp.other_mode_l >> 30) & 3;
        uint32_t m = two_cycle ? (rdp.other_mode_l >> 20) & 3 : (rdp.other_mode_l >> 22) & 3;
        if (p != G_BL_CLR_MEM && m != G_BL_CLR_MEM) use_alpha = false;
    }
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    bool use_noise = (rdp.other_mode_l & G_AC_DITHER) == G_AC_DITHER;
    if (texture_edge) {
        use_alpha = true;
    }
    {   /* FZX_FILLRULE (gfx_soft.c own_edge; ocarina-of-time a7c4909): a blended or alpha-tested draw without AA_EN --
         * the RDP writes such a pixel only when one fixed sample of it is covered, so each pixel along an edge two
         * triangles share is drawn once (the inclusive row solve blended it twice: a bright one-pixel line along
         * the diagonal of a see-through quad). With AA_EN both triangles write the pixel they share (partial
         * coverage each): left as it was. FZX_FILLRULE=0: every draw takes the inclusive row solve, as before. */
        static int sFillRule SAVESTATE_EXCLUDE = -1;
        extern void gfx_soft_set_own_edges(bool on);
        if (sFillRule < 0) { const char *e = getenv("FZX_FILLRULE"); sFillRule = !(e && e[0] == '0'); }
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
    const uint32_t epoch = gfx_cache_epoch; /* a reset under tile 1's import: tile 0 again (gfx_cache_epoch) */
    gfx_tex_stress_hold = pass > 0;
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
    gfx_tex_stress_hold = false;
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
    {
        /* Opaque fog modes: the fog factor rides in the vertex alpha (unused when
         * not blending) and the backend blends the fog colour after the texture,
         * as the RDP does. FZX_FOLDFOG=1 folds it into the vertex colour instead
         * (the MK64 kit's way: cheaper, darker than the N64 under bright fog). */
        static int fold = -1;
        if (fold < 0) {
            const char *e = getenv("FZX_FOLDFOG");
            fold = e != NULL && e[0] == '1';
        }
        bool pf = use_fog && !use_alpha && !fold;
        if (pf != rendering_state.post_fog ||
            (pf && memcmp(&rdp.fog_color, &rendering_state.post_fog_color, sizeof(rdp.fog_color)) != 0)) {
            extern void gfx_scegu_set_post_fog(bool on, uint8_t r, uint8_t g, uint8_t b);
            gfx_flush();
            gfx_scegu_set_post_fog(pf, rdp.fog_color.r, rdp.fog_color.g, rdp.fog_color.b);
            rendering_state.post_fog = pf;
            rendering_state.post_fog_color = rdp.fog_color;
        }
        tri_state.post_fog = pf;
    }
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
        } else if (!uses_lod && gfx_cc_texfog(cc_id)) {
            comb->color_mode = 2; // per triangle: SHADE when the backend lerps (tri_state.tex_fog), else the fold
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
    gfx_update_tex_lerp(cc_id, tri_state.cc2, true);
#ifdef FZX_STAT
    stat_tex_state(tri_state.use_texture, uses_t1, tri_state.cc2);
#endif
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
#ifdef FZX_STAT
        stat_b5_state(cc_id, tri_state.cc2, false, comb->color_mode, add);
#endif
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
#if !defined(PORT_GE_TL)
        /* rdp.viewport, or its fractional edges under the unified fill (gfx_vpf) */
        out->x = gfx_vpf.x + (cv->_x * inv * 0.5f + 0.5f) * gfx_vpf.w;
        out->y = gfx_vpf.y + (0.5f - cv->_y * inv * 0.5f) * gfx_vpf.h;
#else
        out->x = (float)rdp.viewport.x + (cv->_x * inv * 0.5f + 0.5f) * (float)rdp.viewport.width;
        out->y = (float)rdp.viewport.y + (0.5f - cv->_y * inv * 0.5f) * (float)rdp.viewport.height;
#endif
        out->z = gfx_fix_zrange ? cv->_z * inv * 0.5f + 0.5f : cv->_z * inv; // the rasterizer maps 0..1 to the z-buffer
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
            if (tri_state.tex_lerp2) {
                out->color = cv->color; // SHADE; the backend lerps the two body colours by the texel
            } else {
                gfx_eval_vertex_color(cc_id, &cv->color, lod, &out->color);
            }
            break;
    }
    if (gfx_blendcol_tri) {
        out->color = gfx_blend_color; /* FZX_BLENDCOL (gfx_sp_tri1) */
        out->color.a = 255;
    } else if (gfx_cc1_shadea && !tri_state.cc2) {
        /* FZX_CCSHADEA: the rgb multiplier is the shade alpha (see gfx_cc1_shadea_set): the
         * colour from cycle 1's own inputs, per vertex. */
        uint8_t *o = (uint8_t *) &out->color;
        const uint8_t sh[4] = { cv->color.r, cv->color.g, cv->color.b, cv->color.a };
        for (int ch = 0; ch < 3; ch++) {
            const int a = cc2_rgb_input(rdp.cc1[0], 0, &cv->color, lod, sh, ch);
            const int b = cc2_rgb_input(rdp.cc1[1], 1, &cv->color, lod, sh, ch);
            const int c = cc2_rgb_input(rdp.cc1[2], 2, &cv->color, lod, sh, ch);
            const int d = cc2_rgb_input(rdp.cc1[3], 3, &cv->color, lod, sh, ch);
            const int v = ((a - b) * c) / 255 + d;
            o[ch] = (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
    if (tri_state.post_fog) {
        out->color.a = cv->color.a; // the fog factor; the backend blends after the texture
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
    if (buf_vbo_num_tris == 0 && gfx_probe_on > 0) gfx_probe_note_batch();
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

static int gfx_flatshade SAVESTATE_EXCLUDE = -1;
static inline bool gfx_flatshade_on(void) {
    if (gfx_flatshade < 0) { const char *e = getenv("FZX_FLATSHADE"); gfx_flatshade = !(e && e[0] == '0'); }
    return gfx_flatshade != 0;
}
static void gfx_sp_tri1_raw(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
#ifdef PORT_EXP_NOTRI
    return;
#endif
#ifdef PORT_PROFILE_DL
    uint32_t _pt0 = port_time_us();
#endif
    struct LoadedVertex *v1 = &rsp.loaded_vertices[vtx1_idx];
    struct LoadedVertex *v2 = &rsp.loaded_vertices[vtx2_idx];
    struct LoadedVertex *v3 = &rsp.loaded_vertices[vtx3_idx];
    /* FZX_FLATSHADE: with G_SHADING_SMOOTH clear the microcode gives all three vertices the
     * first one's colour (flat shading, gbi.h). The machines' explosion debris (racer.c) sets
     * only vertex 0's colour and leaves the other two with whatever an earlier effect left in
     * the buffer: interpolated, the shards ran into yellow and white gradients, where the N64
     * draws each one in a single shade. FZX_FLATSHADE=0: always smooth, as before. */
    struct LoadedVertex flat2, flat3;
    if (!(rsp.geometry_mode & G_SHADING_SMOOTH) && gfx_flatshade_on()) {
        if (memcmp(&v2->color, &v1->color, sizeof(v1->color)) != 0) { flat2 = *v2; flat2.color = v1->color; v2 = &flat2; }
        if (memcmp(&v3->color, &v1->color, sizeof(v1->color)) != 0) { flat3 = *v3; flat3.color = v1->color; v3 = &flat3; }
    }
    struct LoadedVertex *v_arr[3] = {v1, v2, v3};

    {
        /* debug: FZX_SKIP_CC=<hex combiner id> drops triangles of one combiner */
        static long sSkipCC = -2;
        if (sSkipCC == -2) { const char* e = getenv("FZX_SKIP_CC"); sSkipCC = e ? strtol(e, NULL, 16) : -1; }
        if (sSkipCC >= 0 && (long) rdp.combine_mode == sSkipCC) return;
    }
    if (v1->clip_rej & v2->clip_rej & v3->clip_rej & ~(XY_OUT | GB_OUT)) {
        // The whole triangle lies outside the guard band (the exact side
        // planes reject below, after the phase bookkeeping). GB_OUT is one bit
        // for all four sides: three vertices sharing it may be on opposite sides.
        if (gfx_debug_frame) port_log("  reject rej %02X/%02X/%02X w %.1f/%.1f/%.1f z %.1f/%.1f/%.1f\n", v1->clip_rej, v2->clip_rej, v3->clip_rej, v1->_w, v2->_w, v3->_w, v1->_z, v2->_z, v3->_z);
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
    /* Wholly beyond one side of the view (exact planes): it would rasterize
     * no pixel -- raster_tri clamps to the viewport -- so skip the emit,
     * state rebuild and setup. After the phase bookkeeping, so target
     * selection is unchanged. */
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
            const bool old_trig = wmin < W_MIN ||
                                  (wmin < W_NEAR && (v1->_z > v1->_w || v2->_z > v2->_w || v3->_z > v3->_w));
            const uint32_t any_rej = v1->clip_rej | v2->clip_rej | v3->clip_rej;
            if (old_trig) span_clip = true;
            /* The RSP's triggers: a vertex in front of the near plane, beyond the far plane
             * or (R33) beyond the guard band. */
            if (gfx_fix_cliptrig && (any_rej & (Z_POS | Z_NEG | (gfx_fix_clipgb ? GB_OUT : 0)))) span_clip = true;
#ifdef FZX_STAT
            stat_clip(any_rej, old_trig);
#endif
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
#ifdef FZX_STAT
    stat_tex_tri();
    stat_fog_tri();
    stat_b5_count();
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

/* This will be going away possibly, it all depends on how we end up treating hw sprites */
/* FZX_BLENDCOL: a blender whose pixel input is the blend colour, P = CLR_BL with M = CLR_BL too or
 * no FORCE_BL (full coverage writes P): every pixel the triangle covers is the blend colour,
 * whatever the combiner says. F-Zero X's attack highlight (racer.c, D_303A7D8: the machine
 * drawn again, scaled up, Z_CMP without Z_UPD, blend colour = the highlight) is a solid red rim
 * round the machine on the N64; the port knew no blend colour and drew the scaled model with its
 * own textures, a thin blue-white edge. The draw goes through as an untextured SHADE triangle
 * with the blend colour as its vertex colour. FZX_BLENDCOL=0: as before. */
static int gfx_blendcol_on SAVESTATE_EXCLUDE = -1;
static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
static void gfx_dp_set_combine_mode(uint32_t rgb, uint32_t alpha, uint32_t flags);
static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
    /* A tri command's index (index * 2 in a byte) reaches 127; the vertex buffer holds MAX_VERTICES. The RSP would
     * read whatever DMEM follows its buffer: here the triangle is dropped rather than read past loaded_vertices. */
    if (vtx1_idx >= MAX_VERTICES || vtx2_idx >= MAX_VERTICES || vtx3_idx >= MAX_VERTICES) return;
    const bool two = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE;
    const uint32_t p = two ? (rdp.other_mode_l >> 28) & 3 : (rdp.other_mode_l >> 30) & 3;
    const uint32_t m = two ? (rdp.other_mode_l >> 20) & 3 : (rdp.other_mode_l >> 22) & 3;
    if (gfx_blendcol_on < 0) { const char *e = getenv("FZX_BLENDCOL"); gfx_blendcol_on = !(e && e[0] == '0'); }
    if (!gfx_blendcol_on || p != G_BL_CLR_BL || (m != G_BL_CLR_BL && (rdp.other_mode_l & FORCE_BL))) {
        gfx_sp_tri1_raw(vtx1_idx, vtx2_idx, vtx3_idx);
        return;
    }
    {
        const uint32_t saved = rdp.combine_mode;
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_SHADE), color_comb(0, 0, 0, G_ACMUX_SHADE), 0);
        gfx_blendcol_tri = true;
        gfx_sp_tri1_raw(vtx1_idx, vtx2_idx, vtx3_idx);
        gfx_blendcol_tri = false;
        gfx_dp_set_combine_mode(saved & 0x00FFFFFF, 0, saved & 0xFF000000u);
    }
}

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
    {
        /* ...and only if that cycle blends with the framebuffer at all (P or M =
         * G_BL_CLR_MEM). F-Zero X's track draws in 1-cycle G_RM_FOG_SHADE_A-like
         * mode (FOG*shadeA + IN*(1-shadeA), no memory): opaque, the shade alpha
         * being the fog factor - treating it as opacity made the road vanish. */
        uint32_t p = two_cycle ? (rdp.other_mode_l >> 28) & 3 : (rdp.other_mode_l >> 30) & 3;
        uint32_t m = two_cycle ? (rdp.other_mode_l >> 20) & 3 : (rdp.other_mode_l >> 22) & 3;
        if (p != G_BL_CLR_MEM && m != G_BL_CLR_MEM) use_alpha = false;
    }
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
    
    /* COPY mode skips the combiner and blender, but alpha compare still decides
     * which texels are written: with G_AC_THRESHOLD a 5551 texel whose alpha bit
     * is clear is not drawn. F-Zero X's ENERGY frame (and other copy-mode HUD
     * sprites) rely on it; ignoring it painted the transparent corners black. */
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY &&
        (rdp.other_mode_l & 3) == G_AC_THRESHOLD) {
        texture_edge = true;
    }

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
    gfx_update_tex_lerp(cc_id, false, false);
    struct ColorCombiner *comb = gfx_lookup_or_create_color_combiner(cc_id);
    struct ShaderProgram *prg = comb->prg;
#ifdef FZX_STAT
    stat_b5_state(cc_id, false, true, comb->color_mode, false);
    stat_b5_count();
#endif
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
    /* A wrapping tile (no clamp bit, mask m) reads coordinate & (2^m - 1), so a texrect may start
     * whole periods before its tile's origin. The samplers wrap by the imported size, which is
     * smaller on an edge piece: shift both corners by the same whole number of periods (two when
     * mirrored) so the upper-left one lands in [0, period) (paper-mario 525cb47). */
    int wrap_su = 0, wrap_sv = 0;
    
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
                    wrap_su = (u >= 0 ? u / p : -((p - 1 - u) / p)) * p;
                }
                if (!(rdp.texture_tile.cmt & G_TX_CLAMP) && rdp.texture_tile.maskt) {
                    const int p = (1 << rdp.texture_tile.maskt) << ((rdp.texture_tile.cmt & G_TX_MIRROR) ? 1 : 0);
                    wrap_sv = (v >= 0 ? v / p : -((p - 1 - v) / p)) * p;
                }
            }
            u -= wrap_su;
            v -= wrap_sv;
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
    {   extern bool gfx_soft_2d_bilin; /* this sprite's filter: a magnified one is bilinear (gfx_soft.c, X14) */
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

#if defined(MIYOO)
/* The Miyoo's fill (sMiyooFill), the 3D only: an edge of a scissor or of the viewport's clip rectangle that lies
 * on the box the frame named (or beyond it), in layout units, goes to the screen's edge. */
static void miyoo_fill_edges(float *x, float *y, float *w, float *h, float lx0, float ly0, float lx1, float ly1) {
    if (!sMiyooFill || sCropMode != 2 || !gfx_phase3d) return;
    float x1 = *x + *w, y1 = *y + *h;
    const float sw = (float) gfx_current_dimensions.width, sh = (float) gfx_current_dimensions.height;
    if (lx0 <= sCropBox[0] + 0.5f) *x = 0.0f;
    if (ly0 <= sCropBox[1] + 0.5f) *y = 0.0f;
    if (lx1 >= sCropBox[2] - 0.5f) x1 = sw;
    if (ly1 >= sCropBox[3] - 0.5f) y1 = sh;
    *w = x1 - *x; *h = y1 - *y;
}
#endif
#line 4427
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

#if defined(MIYOO)
    const float x0_lay = x, y0_lay = y, x1_lay = x + width, y1_lay = y + height; /* miyoo_fill_edges */
#endif
#line 4445
#if defined(TARGET_HANDHELD)
    { /* layout to the target (lay2out_*: the fill's box on the whole screen, else the plain ratio) */
        const float x1 = lay2out_x(x + width), y1 = lay2out_y(y + height);
        x = lay2out_x(x); y = lay2out_y(y);
        width = x1 - x; height = y1 - y;
    }
#else
    width *= RATIO_X;
    height *= RATIO_Y;
    x *= RATIO_X;
    y *= RATIO_Y;
#endif

    rdp.viewport.x = x;
    rdp.viewport.y = y;
    rdp.viewport.width = width;
    rdp.viewport.height = height;
#if !defined(PORT_GE_TL)
    if (sCropMode == 2) { /* the unified fill keeps the fractional edges (gfx_vpf) */
        gfx_vpf.x = x; gfx_vpf.y = y; gfx_vpf.w = width; gfx_vpf.h = height;
    } else {
        gfx_vpf.x = rdp.viewport.x; gfx_vpf.y = rdp.viewport.y; gfx_vpf.w = rdp.viewport.width; gfx_vpf.h = rdp.viewport.height;
    }
#endif
#if defined(MIYOO)
    { /* the clip rectangle only (rdp.viewport: whole pixels); the transform keeps gfx_vpf */
        float vx = x, vy = y, vw = width, vh = height;
        miyoo_fill_edges(&vx, &vy, &vw, &vh, x0_lay, y0_lay, x1_lay, y1_lay);
        rdp.viewport.x = vx; rdp.viewport.y = vy; rdp.viewport.width = vw; rdp.viewport.height = vh;
#line 4467
    }
#endif

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
            if (offset == 24 && gfx_flx_lut_loaded && data != NULL) {
                /* racer.c writes three s16 at +8 (native order: GfxPool data). */
                const int16_t *dir = (const int16_t *) ((const uint8_t *) data + 8);
                gfx_flx_light[0] = dir[0] / 256.0f;
                gfx_flx_light[1] = dir[1] / 256.0f;
                gfx_flx_light[2] = dir[2] / 256.0f;
                gfx_normalize_vector(gfx_flx_light);
                gfx_flx_light_valid = true;
                rsp.lights_changed = 1;
                break;
            }
            if ((lightidx == -2 || lightidx == -1) && data != NULL) {
                /* gSPLookAt: G_MVO_LOOKATX (offset 0) and G_MVO_LOOKATY (24), the texgen axes */
                memcpy(&rsp.current_lookat[lightidx == -2 ? 1 : 0], data, sizeof(Light_t));
                rsp.lights_changed = 1;
            }
            if (lightidx >= 0 && lightidx <= MAX_LIGHTS) {
                // NOTE: reads out of bounds if it is an ambient light
                if (memcmp(rsp.current_lights + lightidx, data, sizeof(Light_t)) != 0) {
                    memcpy(rsp.current_lights + lightidx, data, sizeof(Light_t));
                    /* F-Zero X swaps each machine's light without touching the light
                     * count or the matrix: re-derive the light directions. */
                    rsp.lights_changed = 1;
                }
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
        case G_MW_SEGMENT:
            rsp_segments[(offset >> 2) & 0xF] = data;
            break;
        case G_MW_CLIP:
            /* gSPClipRatio (R33): RNX/RNY hold the ratio, RPX/RPY its negative. */
            if (offset == G_MWO_CLIP_RNX || offset == G_MWO_CLIP_RNY) {
                int16_t r = (int16_t) data;
                if (r < 1) r = 1;
                if (offset == G_MWO_CLIP_RNX) gfx_clip_rx = (float) r;
                else gfx_clip_ry = (float) r;
#ifdef FZX_STAT
                stat_clip_ratio(offset == G_MWO_CLIP_RNY, r);
#endif
            }
            break;
    }
}

/* rdp.texture_tile <- rdp.tiles[gfx_render_tile], and both texture units re-import. */
static void gfx_sync_render_tile(void) {
    const uint8_t t = gfx_render_tile;
    rdp.texture_tile.fmt = rdp.tiles[t].fmt;
    rdp.texture_tile.siz = rdp.tiles[t].siz;
    rdp.texture_tile.cms = gfx_tile_cm(rdp.tiles[t].cms, rdp.tiles[t].masks, rdp.tiles[t].uls, rdp.tiles[t].lrs);
    rdp.texture_tile.cmt = gfx_tile_cm(rdp.tiles[t].cmt, rdp.tiles[t].maskt, rdp.tiles[t].ult, rdp.tiles[t].lrt);
    rdp.texture_tile.masks = rdp.tiles[t].masks;
    rdp.texture_tile.maskt = rdp.tiles[t].maskt;
    rdp.texture_tile.tmem = rdp.tiles[t].tmem;
    rdp.texture_tile.uls = rdp.tiles[t].uls;
    rdp.texture_tile.ult = rdp.tiles[t].ult;
    rdp.texture_tile.lrs = rdp.tiles[t].lrs;
    rdp.texture_tile.lrt = rdp.tiles[t].lrt;
    rdp.texture_tile.line_size_bytes = rdp.tiles[t].line_size_bytes;
    rdp.texture_tile.palette = rdp.tiles[t].palette;
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
    tri_state.valid = false;
}

static void gfx_set_render_tile(uint8_t tile) {
    tile &= 7;
    if (tile != gfx_render_tile) {
        gfx_render_tile = tile;
        gfx_sync_render_tile();
    }
}

/* G_MODIFYVTX on a transformed vertex. ST arrives as the final s10.5
 * coordinates (already scaled: the RSP writes the word over the scaled pair). */
static void gfx_sp_modify_vertex(uint32_t idx, uint32_t where, uint32_t val) {
    struct LoadedVertex *d;
    if (idx >= MAX_VERTICES) {
        return;
    }
    d = &rsp.loaded_vertices[idx];
    switch (where) {
        case G_MWO_POINT_ST:
            d->u = (int16_t) (val >> 16);
            d->v = (int16_t) (val & 0xFFFF);
            break;
        case G_MWO_POINT_RGBA:
            d->color.r = (uint8_t) (val >> 24);
            d->color.g = (uint8_t) (val >> 16);
            d->color.b = (uint8_t) (val >> 8);
            d->color.a = (uint8_t) val;
            break;
        default:
            break; /* screen X/Y/Z: unused by F-Zero X */
    }
}

static void gfx_sp_texture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    _UNUSED(level);
    _UNUSED(on);

    rsp.texture_scaling_factor.s = sc;
    rsp.texture_scaling_factor.t = tc;
    gfx_set_render_tile(tile);
}

static void gfx_dp_set_scissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    _UNUSED(mode);
    /* A scissor one short of the screen (0,0,319,239) would lose its last column and row:
     * the fill-rect rule, one-short band only (DKR clips so every frame). */
    if (lrx >= (SCREEN_WIDTH - 1) << 2 && lrx < SCREEN_WIDTH << 2) lrx = SCREEN_WIDTH << 2;
    if (lry >= (SCREEN_HEIGHT - 1) << 2 && lry < SCREEN_HEIGHT << 2) lry = SCREEN_HEIGHT << 2;
    /* R23 (FZX_R23=0 turns it off): (a) an edge within one N64 pixel of the screen's left or
     * top edge goes to it (the band above is the right and bottom); (b) below, the interior
     * edges by pixel centres. */
    const bool r23 = gfx_r23_on();
    if (r23) {
        if (ulx <= 4) ulx = 0;
        if (uly <= 4) uly = 0;
    }

#if defined(TARGET_HANDHELD)
    float x = lay2out_x(ulx / 4.0f);
    float y = lay2out_y(uly / 4.0f);                /* top-down rasterizer: see viewport note */
    float width = lay2out_x(lrx / 4.0f) - x;       /* (the fill's box on the whole screen: lay2out_*) */
    float height = lay2out_y(lry / 4.0f) - y;
#if !defined(PORT_GE_TL)
    if (hud_fit_mode() && sHudGroupN > 0) {
        /* a scissor around a named element (the pause menu's opening wipe) goes where the element
         * is laid out at 1:1 */
        const struct HudPlace p = hud_fit_place(ulx / 4.0f, lrx / 4.0f, uly / 4.0f, lry / 4.0f);
        if (!p.full && p.s == 1.0f) {
            x = hud_fit_x(ulx / 4.0f, &p); width = hud_fit_x(lrx / 4.0f, &p) - x;
            y = hud_fit_y(uly / 4.0f, &p); height = hud_fit_y(lry / 4.0f, &p) - y;
        }
    }
#endif
#else
    float x = ulx / 4.0f * RATIO_X;
    float y = (SCREEN_HEIGHT - lry / 4.0f) * RATIO_Y;
    float width = ((int) lrx - (int) ulx) / 4.0f * RATIO_X;
    float height = ((int) lry - (int) uly) / 4.0f * RATIO_Y;
#endif
#ifdef FZX_STAT
    { /* census: every distinct scissor once, with its edges on the target (a fractional
       * edge falls between pixels; goldeneye-007 c68da63 rounds those outward) */
        static uint32_t seen[512][4];
        static int nseen;
        int i = 0;
        while (i < nseen && !(seen[i][0] == ulx && seen[i][1] == uly && seen[i][2] == lrx && seen[i][3] == lry)) i++;
        if (i == nseen && nseen < 512) {
            seen[nseen][0] = ulx; seen[nseen][1] = uly; seen[nseen][2] = lrx; seen[nseen][3] = lry; nseen++;
            const bool frac = x != floorf(x) || width != floorf(width) || y != floorf(y) || height != floorf(height);
            port_log("STATS vi %u: scissor %u,%u-%u,%u = x %.2f-%.2f y %.2f-%.2f%s%s\n", port_os_vi_count(), ulx, uly, lrx,
                     lry, x, x + width, y, y + height, frac ? " fractional" : "",
                     width < 0.0f || height < 0.0f ? " inverted" : "");
        }
    }
#endif
    /* A corner past the other one (lrx < ulx or lry < uly) leaves the RDP nothing to draw;
     * the unsigned difference made such a scissor about a billion rows tall, clipping from
     * its top edge to the bottom of the screen. The pre-race banners shrink theirs through
     * zero height and on (VI 1095-1130 of the autopilot) with nothing visible under them. */
    if (width < 0.0f) width = 0.0f;
    if (height < 0.0f) height = 0.0f;
    if (r23) { /* (b) column c is inside when x0 <= c + 0.5 < x1; rows alike */
        const float cx0 = ceilf(x - 0.5f), cx1 = ceilf(x + width - 0.5f), cy0 = ceilf(y - 0.5f), cy1 = ceilf(y + height - 0.5f);
        x = cx0; width = cx1 > cx0 ? cx1 - cx0 : 0.0f;
        y = cy0; height = cy1 > cy0 ? cy1 - cy0 : 0.0f;
    }

#if defined(MIYOO)
    miyoo_fill_edges(&x, &y, &width, &height, ulx / 4.0f, uly / 4.0f, lrx / 4.0f, lry / 4.0f);
#endif
#line 4695
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
    // whatever the clamp bit says, and mirroring needs a mask (ocarina-of-time
    // 8674501: OoT's moon quad ran its UVs past a mask-0 texture).
    if (masks == 0) cms = (cms & ~G_TX_MIRROR) | G_TX_CLAMP;
    if (maskt == 0) cmt = (cmt & ~G_TX_MIRROR) | G_TX_CLAMP;

    // Every tile descriptor remembers its TMEM address.  MK64's packed course
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
    rdp.tiles[tile & 7].palette = (uint8_t) palette;
    if ((tile & 7) == gfx_render_tile) {
        gfx_sync_render_tile();
    } else if ((tile & 7) == TEX1_TILE) {
        rdp.textures_changed[1] = true;
        tri_state.valid = false;
    }
}

static void gfx_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    rdp.tiles[tile & 7].uls = uls;
    rdp.tiles[tile & 7].ult = ult;
    rdp.tiles[tile & 7].lrs = lrs;
    rdp.tiles[tile & 7].lrt = lrt;
    if ((tile & 7) == gfx_render_tile) {
        gfx_sync_render_tile();
    } else if ((tile & 7) == TEX1_TILE) {
        rdp.textures_changed[1] = true;
        tri_state.valid = false;
    }
}

static void gfx_dp_load_tlut(uint8_t tile, uint32_t high_index) {
    tri_state.valid = false;

    SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(rdp.texture_to_load.siz == G_IM_SIZ_16b);
    rdp.palette = rdp.texture_to_load.addr;
    {
        /* TLUT entries sit one per TMEM word from 256: record the banks this load fills. */
        const int first = (int) rdp.tile_tmem[tile & 7] - 256, count = (int) high_index + 1;
        int b;
        for (b = 0; b < 16; b++) {
            const int e = b * 16;
            if (e >= first && e < first + count) {
                rdp.tlut_bank[b] = rdp.texture_to_load.addr + (e - first) * 2;
            }
        }
        rdp.textures_changed[0] = rdp.textures_changed[1] = true;
    }
    if (gfx_debug_frame) port_log("  loadtlut addr %p first %02X%02X %02X%02X %02X%02X\n", rdp.palette, rdp.palette[0], rdp.palette[1], rdp.palette[2], rdp.palette[3], rdp.palette[4], rdp.palette[5]);
}

/* TMEM is 4 KB: a load fills at most what lies past the load tile's TMEM address (the RDP wraps the rest to
 * the start of TMEM). An oversized LoadBlock / LoadTile (or LoadTile's lrs < uls, which wrapped the size to
 * gigabytes) is cut there, so no import or hash reads more than TMEM could hold. */
static inline uint32_t gfx_tmem_room(uint8_t tile, uint32_t size_bytes) {
    const uint32_t room = 4096 - ((uint32_t) rdp.tile_tmem[tile & 7] & 0x1FF) * 8;
    return size_bytes > room ? room : size_bytes;
}

static void gfx_dp_load_block(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    tri_state.valid = false;
    _UNUSED(dxt);


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
    rdp.texture_to_load.tile_number = (uint8_t) gfx_lt_slot_for_load(rdp.tile_tmem[tile & 7], size_bytes);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes = size_bytes;
    SUPPORT_CHECK(size_bytes <= 4096);
    // The block starts (uls, ult) texels into the image (G_SETTIMG width): F-Zero X
    // streams its menu backgrounds as 5-row LoadBlocks at ult = 0, 5, 10, ...
    rdp.loaded_texture[rdp.texture_to_load.tile_number].addr = rdp.texture_to_load.addr + gfx_texture_image_offset(uls, ult);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].stride_bytes = 0;
    rdp.loaded_texture[rdp.texture_to_load.tile_number].row_bytes = 0;
    rdp.loaded_texture[rdp.texture_to_load.tile_number].line_bytes = 0;
    
    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    tri_state.valid = false;
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
    rdp.texture_to_load.tile_number = (uint8_t) gfx_lt_slot_for_load(rdp.tile_tmem[tile & 7], size_bytes);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes = size_bytes;

    SUPPORT_CHECK(size_bytes <= 4096);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].addr =
        rdp.texture_to_load.addr + gfx_texture_image_offset(uls >> G_TEXTURE_IMAGE_FRAC, ult >> G_TEXTURE_IMAGE_FRAC);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].stride_bytes = gfx_texture_image_offset(0, 1);
    rdp.loaded_texture[rdp.texture_to_load.tile_number].row_bytes = (((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1) << word_size_shift;
    rdp.loaded_texture[rdp.texture_to_load.tile_number].line_bytes = (uint16_t) rdp.tiles[tile & 7].line_size_bytes;
    // The RDP's LoadTile also sets the load tile's size to the loaded rectangle.
    rdp.tiles[tile & 7].uls = (uint16_t) uls;
    rdp.tiles[tile & 7].ult = (uint16_t) ult;
    rdp.tiles[tile & 7].lrs = (uint16_t) lrs;
    rdp.tiles[tile & 7].lrt = (uint16_t) lrt;
    if ((tile & 7) == gfx_render_tile) {
        gfx_sync_render_tile();
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
    if (rdp.fog_color.r != r || rdp.fog_color.g != g || rdp.fog_color.b != b) {
        tri_state.valid = false; // the backend holds the fog colour for post fog
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
#if defined(MIYOO)
    /* the Miyoo's 4:3 fill: rectangles through the box's map too, as the triangles (viewport) are */
    if (out_aspect < 4.0f / 3.0f - 0.01f || (sMiyooFill && crop_on())) {
#else
    if (out_aspect < 4.0f / 3.0f - 0.01f) {
#endif
#line 5007
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
        (void) sx; (void) sy;
        ulxf = lay2out_x(ulx / 4.0f); /* (x * W/320 and y * H/240, or the fill's box: lay2out_*) */
        lrxf = lay2out_x(lrx / 4.0f);
        ulyf = lay2out_y(uly / 4.0f);
        lryf = lay2out_y(lry / 4.0f);
        if (hud_fit_mode()) { /* the race HUD unsquished in place (hud_fit_mode) */
            const float x0 = ulx / 4.0f, x1 = lrx / 4.0f;
            const struct HudPlace p = hud_fit_place(x0, x1, uly / 4.0f, lry / 4.0f);
            if (!p.full) {
                ulxf = hud_fit_x(x0, &p); lrxf = hud_fit_x(x1, &p);
                ulyf = hud_fit_y(uly / 4.0f, &p); lryf = hud_fit_y(lry / 4.0f, &p);
            }
        } else if (text_fit_mode()) { /* menu text at 1:1 (FZX_MENU1TO1) */
            const float x0 = ulx / 4.0f, x1 = lrx / 4.0f;
            struct HudPlace p;
            if (text_fit_place(x0, x1, uly / 4.0f, lry / 4.0f, &p)) {
                ulxf = hud_fit_x(x0, &p); lrxf = hud_fit_x(x1, &p);
                ulyf = hud_fit_y(uly / 4.0f, &p); lryf = hud_fit_y(lry / 4.0f, &p);
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
    
    /* The fill (lay2out_*) maps the TV-safe box onto the screen, so a full-screen rectangle now
     * starts left of and above it: clip it at 0 -- a negative float to unsigned short is undefined,
     * and x86 and ARM differed in the race's fade-in -- moving its texture coordinates with the edge
     * (floorf: the device's lrintf truncates). */
    {   /* each axis: an edge left of / above the screen moves to 0 with its texture coordinate (either
         * edge: mirrored rectangles come with the corners swapped); both past it: nothing left to draw */
        float *e0[2] = { &ulxf, &ulyf }, *e1[2] = { &lrxf, &lryf };
        short *t0[2] = { &ul->u, &ul->v }, *t1[2] = { &lr->u, &lr->v };
        for (int k = 0; k < 2; k++) {
            float *pa = e0[k], *pb = e1[k];
            short *ta = t0[k], *tb = t1[k];
            if (*pa >= 0.0f && *pb >= 0.0f) continue;
            if (*pa <= 0.0f && *pb <= 0.0f) { *pa = *pb = 0.0f; continue; } /* all of it off-screen */
            if (*pa < 0.0f) { const float t = -*pa / (*pb - *pa); *ta = (short) floorf((float) *ta + (float) (*tb - *ta) * t + 0.5f); *pa = 0.0f; }
            else            { const float t = -*pb / (*pa - *pb); *tb = (short) floorf((float) *tb + (float) (*ta - *tb) * t + 0.5f); *pb = 0.0f; }
        }
    }
    /* Round, don't truncate: the normalize->scale round trip leaves coords at
     * e.g. 47.9999; truncation opened 1-px seams between adjacent 2D tiles. */
    ul->x = (unsigned short)(ulxf + 0.5f);
    ul->y = (unsigned short)(ulyf + 0.5f);

    lr->x = (unsigned short)(lrxf + 0.5f);
    lr->y = (unsigned short)(lryf + 0.5f);
    if (gfx_rectcenter_on()) { /* pixel centres: a half-pixel edge stays at its column instead of rounding up */
        const float c[4] = { ceilf(ulxf - 0.5f), ceilf(ulyf - 0.5f), ceilf(lrxf - 0.5f), ceilf(lryf - 0.5f) };
        ul->x = (unsigned short)(c[0] < 0.0f ? 0.0f : c[0]);
        ul->y = (unsigned short)(c[1] < 0.0f ? 0.0f : c[1]);
        lr->x = (unsigned short)(c[2] < 0.0f ? 0.0f : c[2]);
        lr->y = (unsigned short)(c[3] < 0.0f ? 0.0f : c[3]);
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

static void gfx_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    if (gfx_debug_frame) port_log("  texrect othermode_l %08X h %08X combine %08X\n", (unsigned) rdp.other_mode_l, (unsigned) rdp.other_mode_h, (unsigned) rdp.combine_mode);
#ifdef FZX_STAT
    stat_r34(ulx, uly, lrx, lry);
#endif
    // A texture rectangle names its own tile; the RSP's render tile is untouched.
    const uint8_t saved_render_tile = gfx_render_tile;
    gfx_set_render_tile(tile);

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
        // 1/2-cycle: the exclusive lower-right edge one short of the screen leaves a
        // 1px seam (the fill-rect rule, DKR 9df6228 / Star Fox); only that band moves.
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
     * FZX_TEXRECTINV=1: drawn as before. */
    if (lrx < ulx || lry < uly) {
        static int draw SAVESTATE_EXCLUDE = -1;
        if (draw < 0) { const char *e = getenv("FZX_TEXRECTINV"); draw = e && e[0] == '1'; }
        if (!draw) { rdp.combine_mode = saved_combine_mode; gfx_set_render_tile(saved_render_tile); return; }
    }
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
    gfx_set_render_tile(saved_render_tile);
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
        // (319, 239) leaves a 1px seam a CRT's overscan hid (MK64 99921c7). Only the
        // one-short band moves: a rect past the screen stays as it is (the kit's form).
        if (lrx >= (SCREEN_WIDTH - 1) << 2 && lrx < SCREEN_WIDTH << 2) lrx = SCREEN_WIDTH << 2;
        if (lry >= (SCREEN_HEIGHT - 1) << 2 && lry < SCREEN_HEIGHT << 2) lry = SCREEN_HEIGHT << 2;
    }
    /* The same for a fill: an inverted rectangle covers no pixel on the RDP (texrect above).
     * FZX_TEXRECTINV=1: drawn as before. */
    if (lrx < ulx || lry < uly) {
        static int draw SAVESTATE_EXCLUDE = -1;
        if (draw < 0) { const char *e = getenv("FZX_TEXRECTINV"); draw = e && e[0] == '1'; }
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
#if !defined(PORT_GE_TL)
                    hud_fit_reset(); /* each HUD pass places afresh */
#endif
                }
#if !defined(PORT_GE_TL)
                else if (cmd->words.w1 == PORT_HUD_TAG_KEEP || cmd->words.w1 == PORT_HUD_TAG_KEEP_END) {
                    sHudKeep = cmd->words.w1 == PORT_HUD_TAG_KEEP; /* kept squeezed (hud_fit_mode) */
                } else if (cmd->words.w1 == PORT_HUD_TAG_GROUP) {
                    sHudGroupX = (uint32_t) cmd->words.w0 & 0xffffff; /* a named HUD element (hud_group_add) */
                } else if (cmd->words.w1 == PORT_HUD_TAG_GROUP2) {
                    hud_group_add(sHudGroupX, (uint32_t) cmd->words.w0 & 0xffffff);
                } else if (cmd->words.w1 == PORT_TEXT_TAG) {
                    sTextX = (uint32_t) cmd->words.w0 & 0xffffff; /* a font string's box (text_box_add) */
                } else if (cmd->words.w1 == PORT_TEXT_TAG2) {
                    text_box_add(sTextX, (uint32_t) cmd->words.w0 & 0xffffff);
                } else if (cmd->words.w1 == PORT_FRAME_TAG_CROP) {
                    sCropX = (uint32_t) cmd->words.w0 & 0xffffff; /* the fill's box (gfx_frame_crop) */
                } else if (cmd->words.w1 == PORT_FRAME_TAG_CROP2) {
                    const uint32_t ys = (uint32_t) cmd->words.w0 & 0xffffff;
                    sCrop[0] = (float) (sCropX & 0xfff); sCrop[2] = (float) ((sCropX >> 12) & 0xfff);
                    sCrop[1] = (float) (ys & 0xfff); sCrop[3] = (float) ((ys >> 12) & 0xfff);
                    sCropOn = sCrop[2] > sCrop[0] + 8.0f && sCrop[3] > sCrop[1] + 8.0f;
                    crop_mode_set();
                    if (gfx_raw_vp_valid) gfx_calc_and_set_viewport(&gfx_raw_vp); /* re-derived with the box */
                    if (gfx_raw_sc_valid) gfx_dp_set_scissor(0, gfx_raw_sc[0], gfx_raw_sc[1], gfx_raw_sc[2], gfx_raw_sc[3]);
                } else if (cmd->words.w1 == PORT_LAYOUT_TAG_1TO1 || cmd->words.w1 == PORT_LAYOUT_TAG_END) {
                    const uint32_t v = (uint32_t) cmd->words.w0 & 0xffffff; /* the results screens at 1:1 (one_on) */
                    gfx_flush();
                    sOneOn = cmd->words.w1 == PORT_LAYOUT_TAG_1TO1 && square_screen();
                    sOne[0] = (float) (v & 0xfff); sOne[1] = (float) ((v >> 12) & 0xfff);
                    if (gfx_raw_vp_valid) gfx_calc_and_set_viewport(&gfx_raw_vp); /* re-derived with the map */
                    if (gfx_raw_sc_valid) gfx_dp_set_scissor(0, gfx_raw_sc[0], gfx_raw_sc[1], gfx_raw_sc[2], gfx_raw_sc[3]);
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
#ifdef F3DEX_GBI_2
            case G_DMA_IO:
                /* Only F3DFLX2's table load (DRAM -> DMEM 0x8B0) matters to the port. */
                if (C0(23, 1) == 0) {
                    const uint8_t *src = (const uint8_t *) seg_addr(cmd->words.w1);
                    uint32_t size = C0(0, 12) + 1;
                    if (src != NULL && (uintptr_t) src >= PORT_REAL_PTR_MIN) {
                        memcpy(gfx_flx_lut, src, size < sizeof(gfx_flx_lut) ? size : sizeof(gfx_flx_lut));
                        gfx_flx_lut_loaded = true;
                        gfx_flx_light_valid = false;
                    }
                }
                break;
#endif
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
#ifdef F3DEX_GBI_2
            case G_MODIFYVTX:
                // gSPModifyVertex(vtx, where, val): F-Zero X's course strips load
                // each cross-section once and rewrite the texture coordinates of
                // the shared vertices per section. Ignoring it left stale ST on
                // those vertices: road textures stretched or repeated dozens of
                // times (Big Blue's start line as moire, Fire Field's rough zone).
                gfx_sp_modify_vertex(C0(0, 16) / 2, C0(16, 8), cmd->words.w1);
                break;
#endif
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
            case G_SETBLENDCOLOR:
                gfx_blend_color.r = C1(24, 8); gfx_blend_color.g = C1(16, 8);
                gfx_blend_color.b = C1(8, 8); gfx_blend_color.a = C1(0, 8);
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
                        gfx_cc1_shadea_set();
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
    gfx_clip_rx = gfx_clip_ry = 2.0f; /* the microcode's clip ratio until the task's gSPClipRatio */
    rsp.current_num_lights = 2;
    rsp.lights_changed = true;
    /* the fixed texgen axes until the task's gSPLookAt: LOOKATX = x, LOOKATY = y */
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
    { /* FZX_BOOTSYNC (goldeneye-007 round 17, "Bond missing from the gun barrel"): the soft backend starts with
       * depth test and depth write on, gfx_pc's cached rendering_state with them off. A draw sends a flag only when
       * it differs from the cache, so until the first depth-tested draw every draw was depth-tested and wrote depth.
       * Start the backend as the cache says. FZX_BOOTSYNC=0: as before. */
        const char *e = getenv("FZX_BOOTSYNC");
        if (!(e && e[0] == '0')) {
            gfx_rapi->set_depth_test(rendering_state.depth_test);
            gfx_rapi->set_depth_mask(rendering_state.depth_mask);
            gfx_rapi->set_zmode_decal(rendering_state.decal_mode);
            gfx_rapi->set_use_alpha(rendering_state.alpha_blend);
        }
    }
    { /* MK64_DRAWDIST=<clip.w units>: far cull, desktop tuning, off by default.
       * Validated: non-numeric, trailing junk, <= 0 or absurd stays OFF -- the
       * round-12 bare atof() turned "abc" into 0.0f and culled the whole 3D
       * scene with no log line. Read here, once, not in the per-frame gfx_run. */
        const char *e = getenv("FZX_DRAWDIST"); char *end = NULL;
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
    rendering_state.alpha_blend = true;
    rendering_state.tex_add = false;
    rendering_state.force_in = false;
    rendering_state.tex_lerp = false;
    rendering_state.tex_fog = false;
    rendering_state.own_edges = false;
    {
        extern void gfx_scegu_set_force_in(bool on);
        extern void gfx_scegu_set_tex_lerp(bool on, uint8_t r, uint8_t g, uint8_t b);
        extern void gfx_soft_set_own_edges(bool on);
        gfx_scegu_set_force_in(false);
        gfx_scegu_set_tex_lerp(false, 0, 0, 0);
        {
            extern void gfx_scegu_set_tex_fog(bool on, uint8_t r, uint8_t g, uint8_t b);
            gfx_scegu_set_tex_fog(false, 0, 0, 0);
        }
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
    /* FZX_LOADSYNC (super-smash-bros 9e9bb19): the four flags above were reset in the cache only; the soft backend
     * kept what the last draw before the load had set (the renderer is outside the state). A draw sends a flag
     * only when it differs from the cache, so a draw wanting the reset value (blend on, depth off) was drawn
     * with the backend's old one until some draw flipped it. Set the backend to what the cache says. Runs only
     * here (after a load). FZX_LOADSYNC=0: as before. */
    {
        static int on SAVESTATE_EXCLUDE = -1;
        if (on < 0) { const char *e = getenv("FZX_LOADSYNC"); on = !(e && e[0] == '0'); }
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
#if !defined(PORT_GE_TL)
    sCropOn = 0; /* a frame fills the screen only if it names its box (PORT_FRAME_CROP) */
    sCropMode = 0;
    fill100_frame(); /* (FZX_FILL100's pick for this frame) */
    sOneOn = 0;  /* and draws at 1:1 only inside a PORT_LAYOUT_1TO1 bracket */
    memcpy(sTextPrev, sTextBox, sizeof(sTextBox[0]) * (size_t) sTextN); sTextPrevN = sTextN;
    sTextN = 0;  /* and its font strings (PORT_TEXT_BOX) */
#endif
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

/* 1: draw the display list into the renderer's picture but don't present it (port_gfx_readback's
 * catch-up of a skipped frame, round 13d) */
int gfx_no_present;
void gfx_run(Gfx *commands) {
    gfx_fix_init();
    gfx_sp_reset();
    gfx_set_render_tile(0);
    gfx_flx_lut_loaded = gfx_flx_light_valid = false;
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
    if (!gfx_no_present) gfx_wapi->swap_buffers_begin();
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

/* The N64 can show a picture that no display list drew: the CPU writes it into a
 * framebuffer and the VI scans that out (F-Zero X's boot N64 logo, sys_main.c).
 * Present one such RDRAM framebuffer (320x240 RGBA5551) as a frame. */
void gfx_present_rdram_framebuffer(const uint16_t *fb) {
    extern uint32_t *gfx_soft_get_framebuffer(int *w, int *h);
    if (!gfx_wapi->start_frame()) {
        return;
    }
    gfx_rapi->start_frame();
    int w, h;
    uint32_t *dst = gfx_soft_get_framebuffer(&w, &h);
    /* The user (2026-09-29): the N64 logo unsquished. On a screen narrower than 4:3 the picture is
     * shown at y's scale, a centre crop of the 320-wide framebuffer (on a square output: columns 40..279
     * at 1:1); the logo spans x 100..224, so nothing of it is cut. FZX_LOGOUNSQ=0: squeezed as before. */
    static int sLogoUnsq = -1;
    if (sLogoUnsq < 0) { const char *e = getenv("FZX_LOGOUNSQ"); sLogoUnsq = !(e && e[0] == '0'); }
    const int crop = sLogoUnsq && w * 3 < h * 4;
    for (int y = 0; y < h; y++) {
        const uint16_t *src = fb + (y * SCREEN_HEIGHT / h) * SCREEN_WIDTH;
        for (int x = 0; x < w; x++) {
            const int sx = crop ? (int) floorf(((float) x + 0.5f - 0.5f * (float) w) * (float) SCREEN_HEIGHT / (float) h + 0.5f * (float) SCREEN_WIDTH)
                                : x * SCREEN_WIDTH / w;
            const uint32_t p = (sx >= 0 && sx < SCREEN_WIDTH) ? src[sx] : 0u;
            const uint32_t r = (p >> 11) & 31, g = (p >> 6) & 31, b = (p >> 1) & 31;
            *dst++ = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
        }
    }
    { extern void gfx_soft_native_written(void); gfx_soft_native_written(); }
    gfx_rapi->end_frame();
    gfx_wapi->swap_buffers_begin();
    gfx_rapi->finish_render();
    gfx_wapi->swap_buffers_end();
}

void gfx_end_frame(void) {
    
    //sceIoWrite(1, "----END FRAME!\n", 16);
    if (!dropped_frame && !gfx_no_present) {
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
