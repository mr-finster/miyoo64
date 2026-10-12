/**
 * F-Zero X -> handheld: the host side of the libultra replacement.
 *
 * The game keeps its N64 structure: bootproc creates the idle thread, which
 * creates the main (scheduler), game, audio and reset threads, and they talk
 * through message queues driven by VI / SP / DP / SI events. The port runs every
 * N64 thread as a cooperative fiber (src/port/ultra/ultra_os.c) whose whole state
 * lives in game memory: the saved stack pointer in OSThread.context and the stack
 * in a static pool. The host loop (src/port/port_main.c) plays the hardware: it
 * runs the fibers until they are all blocked, executes the SP tasks they queued
 * (display lists -> renderer, audio command lists -> mixer), raises the SP/DP
 * events, and raises a VI at 60 Hz.
 */
#ifndef PORT_OS_H
#define PORT_OS_H

#include "libultra/ultra64.h"

/* Virtual N64 clock: 46.875 MHz count, advanced per VI and per osGetTime call. */
#define PORT_COUNT_HZ 46875000u
#define PORT_VI_HZ 60
#define PORT_COUNT_PER_VI (PORT_COUNT_HZ / PORT_VI_HZ)

/* --- host -> OS ----------------------------------------------------------- */
void port_os_init(void);
/** Run N64 threads until every one is blocked (or the idle thread is reached). */
void port_os_run(void);
/** One vertical retrace: advance the clock, swap the VI framebuffer, post VI messages. */
void port_os_vi(void);
/** Post an OS event (OS_EVENT_SP / OS_EVENT_DP / ...) to whoever registered for it. */
void port_os_event(s32 event);
/** The SP task the scheduler started and nobody executed yet (NULL if none). */
OSTask* port_os_take_sp_task(void);
u32 port_os_vi_count(void);
void* port_os_vi_current_fb(void);
/** The framebuffer the VI shows now (NULL while blanked). */
void* port_os_vi_framebuffer(void);
/** A thread busy-waits: hand control back to the host so time can pass. */
void port_os_block_on_host(void);
/** The running game thread's id, -1 on the host (the watchdog's log line, any thread). */
int port_os_running_id(void);

/* --- OS -> host (implemented by the platform layer) ------------------------ */
/** Controller state for port 0 (N64 button bits, stick -80..80). */
void port_input_read(OSContPad* pad);
/** Audio samples the game produced (interleaved s16 stereo, native endian). */
void port_audio_push(const s16* samples, u32 bytes);
u32 port_audio_queued_bytes(void);
u32 port_audio_take_ring_min(void); /* PERF: the least the ring held at a callback that played (sdl/audio_sdl.c) */
u32 port_audio_gaps(void);          /* PERF: the times the sound came back after a gap (hh_declick.h) */
void port_audio_wav_mark(const char* what);
void port_audio_set_frequency(u32 hz);
void port_audio_init(int headless);
void port_audio_quit(void);
void port_audio_vi(void);
u32 port_audio_underruns(void);
/** Run one aspMain command list (the game's Acmd words) on the C mixer. */
void port_audio_run_alist(const u64* list, u32 bytes);
/** The 3D pass's size in % of the screen (the HUD stays native): always 100 in the game;
 *  smaller sizes are developer A/B switches (FZX_INTRES, bench.c). */
int port_res3d_pct(void);
void port_res3d_set(int pct); /* bench.c: an exact 3D size */
/** The 3D pass's texture filter: 1 = bilinear (SMOOTH, always in the game), 0 = nearest (A/B). */
int port_filter3d(void);

/* --- ROM / DMA (src/port/ultra/rom.c) ------------------------------------- */
int port_rom_open(const char* path, char* err, int errlen);
/** Copy [romOffset, +size) of the player's ROM to dst and fix the byte order
 *  of any asset data in it (tools/gen_swapmap.py). */
void port_rom_dma(u32 romOffset, void* dst, u32 size);
const u8* port_rom_bytes(void);

/* --- misc ----------------------------------------------------------------- */
void port_log(const char* fmt, ...);
const char* port_data_path(const char* name);
void port_fatal(const char* fmt, ...);

#endif
