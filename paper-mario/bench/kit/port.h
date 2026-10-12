/**
 * Mario Kart 64 -> PSP port: shared declarations for the port layer.
 *
 * The N64 game addresses much of its data through RSP "segments"
 * (0xSSOOOOOO = segment SS, offset OOOOOO).  On the PSP the same data is
 * linked straight into the executable, so a segmented address is resolved
 * either linearly (segment base + offset, for buffers the game fills at run
 * time: vertices, decompressed textures, the gfx pool) or through a table of
 * {offset, symbol} pairs generated from the decomp's own naming (course
 * display lists, torch assets), see tools/psp/gen_seg_tables.py.
 */
#ifndef PORT_H
#define PORT_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>

typedef struct {
    u32 offset;      // offset inside the segment
    u32 size;        // bytes covered from `offset` (up to the next entry)
    const void* ptr; // where that data lives on the PSP
} PortSegEntry;

typedef struct {
    const PortSegEntry* entries; // sorted by offset
    s32 count;
} PortSegTable;

typedef struct {
    PortSegTable seg6; // course_data.c      (0x06xxxxxx)
    PortSegTable seg7; // course_displaylists (0x07xxxxxx)
} PortCourseSegTables;

extern const PortCourseSegTables gPortCourseSegTables[]; // indexed by course id
extern const PortSegTable gPortCommonDataSegTable;       // segment 0x0D
extern const PortSegTable gPortCeremonyDataSegTable;     // segment 0x0B (ending)
extern const PortSegTable gPortStartupLogoSegTable;      // segment 0x06 (boot logo)

extern uintptr_t gSegmentTable[16];

void port_set_segment_table(s32 segment, const PortSegTable* table);
void port_clear_segment_table(s32 segment);
/** Segmented address (or real pointer) -> real pointer.  NULL if unmapped. */
void* port_seg_to_ptr(uintptr_t addr);
/** Real pointers pass straight through, everything else is treated as segmented. */
/* Real pointers live above the N64 segment window (segments 0..0xF span
 * 0x00000000..0x0FFFFFFF). On the PSP that is user RAM at 0x08800000; on
 * desktop/handheld the binary + heap are placed at >= 0x10000000 (Makefile.handheld
 * links with -Ttext-segment=0x10000000) so real addresses never collide with a
 * segmented one. */
#if defined(TARGET_PSP)
#define PORT_REAL_PTR_MIN 0x08800000u
#else
#define PORT_REAL_PTR_MIN 0x10000000u
#endif
static inline int port_is_real_ptr(uintptr_t addr) {
    return addr >= PORT_REAL_PTR_MIN;
}

/* floorf/ceilf without the libm call ARMv7 needs (it has no round-to-int
 * instruction, so GCC emits a real call even with -fno-math-errno): bit-exact
 * for finite |x| < 2^31 -- (int) truncates toward zero, then a one-count
 * correction toward -inf / +inf; int->float is exact below 2^24. Callers keep
 * inputs in range: screen coords are bounded by the W_MIN near clip, texture
 * offsets by GFX_MAX_UV_REPEATS, and the one unbounded user (the span-solve
 * quotient in gfx_soft.c) clamps first. Shared here so gfx_pc.c and
 * gfx_soft.c cannot drift apart on a primitive the bit-exact sweep relies on. */
static inline int   port_ifloorf(float x) { int i = (int)x; return i - (x < (float)i); }
static inline int   port_iceilf (float x) { int i = (int)x; return i + (x > (float)i); }
static inline float port_floorf (float x) { return (float)port_ifloorf(x); }

/* Memory pool the game allocates course/texture data from (replaces the N64's
 * fixed RAM regions between the code segments). */
#define PORT_MEMORY_POOL_SIZE 0x300000
extern u8 gPortMemoryPool[PORT_MEMORY_POOL_SIZE];

/* Where the port keeps its files (save, log, screenshots). */
/* The port keeps everything it writes (save, cache, log, debug dumps) in a
 * data/ folder next to the EBOOT.  port_eboot_dir() is set from argv[0] at
 * boot; port_save_dir() is its data/ subfolder; port_save_path("x") joins. */
const char* port_eboot_dir(void);
const char* port_save_dir(void);
const char* port_save_path(const char* name);
void port_set_save_dir(const char* argv0);
void port_fs_init(void);

/* Debug builds only (psp_debug.c; no-ops otherwise): dump the displayed frame
 * / z-buffer to data/shotNNN.ppm / depthNNN.ppm, backend self-test at boot,
 * per-frame probes of the scripted run. */
void port_screenshot(int index);
void port_depthshot(int index);
void port_debug_selftest(void);
void port_debug_frame_begin(u32 frame);
void port_debug_frame_end(u32 frame);

/* Logging (goes to stdout / psplink / a file depending on the backend). */
void port_log(const char* fmt, ...);
void port_log_always(const char* fmt, ...); /* PERF/STALL: ignores MK64_QUIET */
#define PORT_LOG(...) port_log(__VA_ARGS__)

/* Stadium TV screens (Luigi Raceway, Wario Stadium, the award ceremony): the
 * game copies a patch of the previous frame's framebuffer into RGBA16 course
 * textures.  The port has no CPU-readable framebuffer, so copy_framebuffer()
 * files a request instead and the GE backend fills the tile from its own frame
 * at the end of the frame.  x/y/w/h are in the N64's 320x240 frame; `target`
 * is the tile in the course texture segment, written in N64 texel order. */
void port_fb_copy_request(int x, int y, int w, int h, uint16_t* target); /* PM64: plain types (PM ultratypes s32 = long) */
/* The course's texture block (memory.c: decompressed once at the end of the
 * heap, segment 5) does not change while the course is up, so the renderer
 * need not hash those texels on every material change (beckerd 57790fa: 4 ms
 * a picture on PSP hardware). NULL/0 when the block is gone (any gamestate
 * change). The stadium-screen tiles inside it are rewritten every frame and
 * stay hashed. */
void port_course_textures_loaded(void* start, unsigned int size);
void port_fb_tile_note(void* target, unsigned int bytes);

/* Frame hooks implemented by the platform backend. */
void port_gfx_run(Gfx* dl);        // execute a display list (F3DEX -> sceGu)
void port_gfx_start_frame(void);
void port_gfx_end_frame(void);     // present + vsync
void port_audio_frame(void);       // mix one game frame worth of audio
void port_audio_keepalive(void);   // throttled FIFO top-up during long loads

/* Frame profile: wall-clock buckets in microseconds. Each layer adds into
 * gPortProfFrame[] during the iteration; port main folds it into the running
 * totals at the iteration end (gPortProfLast = the finished iteration, read by
 * the dynamic-res ladder) and prints the PERF line every 150 iterations. */
enum { PROF_LOGIC, PROF_AUDIO, PROF_GFX, PROF_R3D, PROF_R2D, PROF_TEX, PROF_COMP, PROF_CLR, PROF_PRES, PROF_SLEEP, PROF_ITER, PROF_COUNT };
/* PROF_SLEEP: frame pacing sleep, the in-game menu, save/load -- time inside
 * the iteration that is not work (the ladder uses ITER - SLEEP). */
extern u32 gPortProfFrame[PROF_COUNT];
extern u32 gPortProfLast[PROF_COUNT];
extern volatile u32 gPortProfFlipUs;   /* SDL_Flip time on the present thread (not iteration time) */
#define PORT_PROF_ADD(slot, us) (gPortProfFrame[(slot)] += (u32)(us))
void port_input_poll(void);        // fill controller state

/* Display-list markers (gDPNoOpTag) bracketing the race HUD so the renderer
 * can anchor its 2D elements to the screen edges on the wide display. */
#define PORT_HUD_TAG_ON  0x48554431u /* 'HUD1' */
#define PORT_HUD_TAG_OFF 0x48554430u /* 'HUD0' */
#define PORT_HUD_TAG_CENTRE 0x48554432u /* 'HUD2': menus -- uniform scale, centred */

#endif /* PORT_H */
