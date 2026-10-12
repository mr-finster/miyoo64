/**
 * Super Mario 64 -> handheld port: shared declarations for the port layer (the
 * Mario Kart 64 port's kit, src/hh). SM64 is built NO_SEGMENTED_MEMORY
 * (as sm64-port does): every address in a display list is a real pointer into
 * the executable, so the kit's segment lookup is a pass-through here.
 */
#ifndef PORT_H
#define PORT_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>

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
void port_log_always(const char* fmt, ...); /* PERF/STALL: ignores SM64_QUIET */
#define PORT_LOG(...) port_log(__VA_ARGS__)

/* Stadium TV screens (Luigi Raceway, Wario Stadium, the award ceremony): the
 * game copies a patch of the previous frame's framebuffer into RGBA16 course
 * textures.  The port has no CPU-readable framebuffer, so copy_framebuffer()
 * files a request instead and the GE backend fills the tile from its own frame
 * at the end of the frame.  x/y/w/h are in the N64's 320x240 frame; `target`
 * is the tile in the course texture segment, written in N64 texel order. */
void port_fb_copy_request(s32 x, s32 y, s32 w, s32 h, u16* target);
/* The course's texture block (memory.c: decompressed once at the end of the
 * heap, segment 5) does not change while the course is up, so the renderer
 * need not hash those texels on every material change (beckerd 57790fa: 4 ms
 * a picture on PSP hardware). NULL/0 when the block is gone (any gamestate
 * change). The stadium-screen tiles inside it are rewritten every frame and
 * stay hashed. */
void port_course_textures_loaded(void* start, u32 size);
void port_fb_tile_note(void* target, u32 bytes);

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
/* STALLWHY (2026-10-06, the transition hitches): named wall-clock parts of one frame's game logic, in us.
 * hh_main.c prints them in a STALLWHY line when the frame's logic is over 40 ms (SM64_WHYUS=<us>: another
 * threshold). Parts nest: the level script holds the update call, which holds the audio reset, which holds
 * its waits. */
enum { WHY_TICK, WHY_SCRIPT, WHY_RENDER, WHY_OBJS, WHY_MUSIC, WHY_RESET, WHY_RWAIT, WHY_RSYNTH, WHY_RFRAMES,
       WHY_RSOUND, WHY_REARLY, WHY_EEPROM, WHY_EEFSYNC, WHY_COUNT };
extern u32 gPortWhy[WHY_COUNT];
extern u32 gPortWhyCmd[64];       /* the level script's time by command */
#define PORT_WHY_ADD(slot, us) (gPortWhy[(slot)] += (u32)(us))
u32 port_time_us(void);
void port_input_poll(void);        // fill controller state

/* Display-list markers (gDPNoOpTag) bracketing the race HUD so the renderer
 * can anchor its 2D elements to the screen edges on the wide display. */
#define PORT_HUD_TAG_ON  0x48554431u /* 'HUD1' */
#define PORT_HUD_TAG_OFF 0x48554430u /* 'HUD0' */
#define PORT_HUD_TAG_CENTRE 0x48554432u /* 'HUD2': menus -- uniform scale, centred */
/* Inside the HUD pass, draws that keep the squeeze while the rest is unsquished (MK64 39258d3's
 * PORT_HUD_KEEP: the elements that would overlap at 1:1). */
#define PORT_HUD_TAG_KEEP     0x4855444Bu /* 'HUDK' */
#define PORT_HUD_TAG_KEEP_END 0x4855446Bu /* 'HUDk' */
/* A HUD pass laid out as a 1:1 crop of the 320x240 layout's centre (gfx_pc.c hud_fit_crop): every
 * draw keeps its place relative to the screen's centre, a named element (PORT_HUD_GROUP) is pulled
 * back inside the margins. The pause screen: its text is a block wider than a centred element. */
#define PORT_HUD_TAG_CROP 0x48554433u /* 'HUD3' */
#define PORT_HUD_CROP_BEGIN() gDPNoOpTag(gDisplayListHead++, PORT_HUD_TAG_CROP)
#define PORT_HUD_BEGIN()      gDPNoOpTag(gDisplayListHead++, PORT_HUD_TAG_ON)
#define PORT_HUD_END()        gDPNoOpTag(gDisplayListHead++, PORT_HUD_TAG_OFF)
#define PORT_HUD_KEEP_BEGIN() gDPNoOpTag(gDisplayListHead++, PORT_HUD_TAG_KEEP)
#define PORT_HUD_KEEP_END()   gDPNoOpTag(gDisplayListHead++, PORT_HUD_TAG_KEEP_END)
/* A HUD element made of several draws (a counter's icon, x and digits, each glyph its own texrect):
 * its box in the 320x240 layout (y down), so the unsquish lays the whole element out at 1:1 around
 * one centre instead of widening each glyph in place. Two no-ops, the box in their low 24 bits. */
#define PORT_HUD_TAG_GROUP    0x48554447u /* 'HUDG': x0 | x1 << 12 */
#define PORT_HUD_TAG_GROUP2   0x48554467u /* 'HUDg': y0 | y1 << 12 */
/* or'd into y0 (y < 1024): where the element goes at 1:1 (gfx_pc.c hud_group_add) */
#define PORT_HUD_AT_LEFT   0x400u /* against the left margin */
#define PORT_HUD_AT_RIGHT  0x800u /* against the right margin */
#define PORT_HUD_AT_BEFORE 0xC00u /* just left of the element placed before it */
#define PORT_HUD_KEEP_Y1   0x800u /* or'd into y1 (y < 2048): the element keeps the squeeze */
int gfx_hud_unsquish(void);   /* 1: the HUD is drawn at 1:1 (the counters drop their "x") */
#define PORT_HUD_GROUP(x0, x1, y0, y1) do {                                                               \
        gDisplayListHead->words.w0 = _SHIFTL(G_NOOP, 24, 8) | ((u32) (x0) & 0xfff) | (((u32) (x1) & 0xfff) << 12); \
        gDisplayListHead->words.w1 = PORT_HUD_TAG_GROUP; gDisplayListHead++;                              \
        gDisplayListHead->words.w0 = _SHIFTL(G_NOOP, 24, 8) | ((u32) (y0) & 0xfff) | (((u32) (y1) & 0xfff) << 12); \
        gDisplayListHead->words.w1 = PORT_HUD_TAG_GROUP2; gDisplayListHead++;                             \
    } while (0)

#endif /* PORT_H */
