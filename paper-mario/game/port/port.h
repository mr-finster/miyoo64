/* The native port's own interface (M2): what game/port/*.c share. Two kinds of file use it: the game side includes
 * the decomp's common.h and no system headers (the decomp declares its own libc, and its bool is a 4-byte enum, which
 * <stdbool.h> would break), the host side the other way round. So this header takes only <stdint.h>, and a flag is
 * an int. */
#ifndef PM64_PORT_H
#define PM64_PORT_H
#if __has_include(<stdint.h>)
#include <stdint.h>
#else /* the game side is built with the decomp's headers only (-nostdinc) */
typedef __INT8_TYPE__ int8_t;
typedef __INT16_TYPE__ int16_t;
typedef __INT32_TYPE__ int32_t;
typedef __UINT8_TYPE__ uint8_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __UINT64_TYPE__ uint64_t;
typedef __UINTPTR_TYPE__ uintptr_t;
#endif

/* the save chip (128 KB flash) as a file: port_rom.c */
void port_flash_open(void);
void port_flash_read(uint32_t offset, void *dst, uint32_t len);
void port_flash_write(uint32_t offset, const void *src, uint32_t len);
void port_flash_erase(uint32_t offset, uint32_t len);

/* the device's screen and buttons (port_sdl.c; the desktop proxy has port_input.c's pad script) */
void port_video_init(void);
void port_video_present(void);
void port_video_crash(void);
int port_video_shutdown(void); /* the way out: SDL_Quit (port_host.c); 1 once SDL is down */
/* the panel's picture (port_gfx.c): the frame squeezed 4 -> 3; during the intro logos the middle 240 columns 1:1
 * and the widening after them; in the field the scene stretched over its black frame. port_panel_begin once a frame,
 * then port_panel_plain (1: the plain squeeze, row by row) or port_panel_row for each of the 240 rows. */
void port_panel_begin(const uint32_t *fb, int w, int h);
int port_panel_plain(void);
int port_panel_rect(void);       /* 1: a resampled rectangle, which the device draws with port_panel_565 (NEON) */
void port_panel_565(uint16_t *dst, int stride);
void port_panel_row(uint32_t *dst, int y);
int port_logos_view(void);         /* game side (port_main.c) */
int port_letterbox_standard(void); /* game side (port_main.c) */

/* the save dir and friends (port_host.c), the ROM finder and the asset region (port_assets_hh.c) */
const char *port_save_dir(void);
const char *port_save_path(const char *name);
const char *port_eboot_dir(void);
void port_log_always(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int port_find_rom(char *out, uint32_t outlen, char *why, uint32_t whylen);
int port_pkg_rom(char *out, uint32_t outlen, char *why, uint32_t whylen); /* rom.z64 next to the executable, else 0 */
void port_assets_load(const char *argv0);
void port_assets_rom_hint(const char *path); /* the ROM already found: the region's build scans no more */
void port_assets_done(void);                  /* ends the first-start screen: Done, a beat, black (no-op when none was shown) */
void port_assets_no_rom(const char *why);     /* the no-ROM screen, then exit (why: the last file rejected, or "") */
extern int port_assets_cache_current; /* assets.bin holds this run's region byte for byte (loaded or just written) */

/* sound: the game's audio manager (port_audio.c, game side), the mixer (port_mixer.c) and the output (port_audio_out.c) */
void port_audio_boot(void);
void port_audio_retrace(void);
void port_mixer_set_heap(void *base, uint32_t size);
void port_mixer_run(const uint32_t *cmds, int count);
void port_audio_out_init(uint32_t rate);
int32_t port_audio_out_want(int32_t frame, int32_t min_frame);
void port_audio_out_mark(void); /* once per retrace's fill: the queue's aim follows the gaps */
extern uint32_t port_audio_out_underruns;
void port_audio_out_retrace(void);
void port_audio_out_push(const int16_t *samples, int32_t frames);

/* the log (stderr and, on the device, the log file) */
void port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint32_t port_time_us(void);
extern uint32_t port_perf_gfx_us, port_perf_audio_us; /* time in drawing and in audio, for the PERF line */

/* The player's ROM, read like the cartridge (port_rom.c). The .z64 order; a .v64 or .n64 dump is normalised when read. */
int port_rom_open(const char *path);
void port_rom_read(uint32_t rom_addr, void *dst, uint32_t size);
uint32_t port_rom_size(void);

/* A DMA from the ROM (nuPiReadRom and the game's dma_copy): code segments are linked in natively, so a DMA of one
 * is not a copy (port_segments.c); everything else reads the ROM. */
void port_pi_read(uint32_t rom_addr, void *dst, uint32_t size);
/* 1 = [rom_start, rom_end) lies inside a code segment (its descriptor, if any, in *seg_index) */
int port_code_segment(uint32_t rom_start, uint32_t rom_end, int *seg_index);

/* The frame: the game's display lists go to the renderer (port_gfx.c). */
void port_gfx_init(void);
void port_gfx_task(void *gfx_list, int swap);
void port_prev_frame_done(uint16_t *framebuffer); /* port_gfx.c: the effects' frame before */
void *port_seg_to_ptr(uintptr_t addr);

/* controller 1, from the window's keys or the input script (port_input.c) */
void port_input_poll(void);
void port_input_read(uint16_t *button, int8_t *stick_x, int8_t *stick_y);

/* save states (port_state.c; the kit's savestate.c does the files) */
void port_state_init(void);
void port_state_stage(void);
void port_state_frame(uint32_t frame);
void port_audio_out_set_muted(int muted);

extern int port_quit; /* set by the window or the frame limit: the main loop ends */
extern uint32_t port_frame; /* game frames run */
void port_debug_frame(void); /* the test switches' per-frame side (port_debug.c) */
void port_flash_commit(void); /* the save image to disk when the frame changed it (port_rom.c) */
uint32_t port_debug_seed(uint32_t seed); /* PM64_SEED (port_debug.c) */
/* where the main loop is (port_host.c; port_sdl.c marks the flip), for a diagnostic log line */
enum { PORT_STAGE_BOOT, PORT_STAGE_INPUT, PORT_STAGE_CHECK, PORT_STAGE_GAME, PORT_STAGE_FLIP, PORT_STAGE_SAVE,
       PORT_STAGE_SLEEP };
extern volatile int port_loop_stage;

#endif
