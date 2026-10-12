/* hh_os.h: the port layer's view of its own libultra OS (hh_os.c) and the hooks the
 * platform side (renderer, mixer, SDL) provides. Not seen by game code. */
#ifndef HH_OS_H
#define HH_OS_H

#include <stdint.h>
#include <ultra64.h>

#define HH_COUNT_HZ  46875000u   /* osGetCount() rate: half the 93.75 MHz CPU clock */
#define HH_VI_COUNTS 781250u     /* one 60 Hz retrace in osGetCount() units */

/* hh_os.c */
void hh_os_init(int paced);
void hh_os_run(void) __attribute__((noreturn)); /* the kernel loop (host main stack) */
u64 hh_os_now(void);                  /* osGetTime() */
void hh_os_sleep_until(u64 count);    /* block the calling game thread until then */
void hh_os_post_event(OSEvent e);     /* deliver a libultra event message */
void hh_os_rebase_clock(void);        /* forget host time spent outside the game (menus) */
void hh_os_dump(const char *why);     /* print every thread's state to stderr */
/* Any thread (the loop note): the kernel loop's turns; where it is and the running game
 * thread's id (-1 = the kernel). */
enum { HH_WHERE_BOOT, HH_WHERE_THREAD, HH_WHERE_CHECK, HH_WHERE_DRAW, HH_WHERE_AUDIO, HH_WHERE_EVENTS, HH_WHERE_SLEEP };
u32 hh_os_where(int *where, int *thread);
/* host time by where it went, running totals in us (hh_os.c hh_acct; the STALL line) */
enum { HH_ACCT_HOOK = 8, HH_ACCT_SERVICE, HH_ACCT_DRAW, HH_ACCT_SLEEP, HH_ACCT_N };
extern uint32_t hh_os_acct_us[HH_ACCT_N];
extern uint32_t hh_os_slice_max_us; /* the longest turn of one game thread since it was last zeroed */
extern int hh_os_slice_max_id;
extern int hh_os_trace;

/* hh_hw.c, called by the kernel once per retrace before the VI message is sent */
void hh_vi_retrace(u32 vicount);

/* Platform hooks. Kernel context: */
void hh_plat_rsp_task(OSTask *task);          /* execute a gfx or audio task, synchronously */
void hh_plat_vi(u32 vicount, int new_frame);  /* every retrace: input; present a new frame */
/* Game thread (osAiSetNextBuffer): queue interleaved s16 stereo PCM for output. */
void hh_plat_audio_out(const void *pcm, u32 len, u32 rate);

/* Die with a message, the thread states and the frame count. */
void hh_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
u64 hh_host_ns(void);

#endif
