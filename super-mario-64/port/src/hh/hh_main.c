/**
 * Super Mario 64 -> handheld / desktop port: process entry point and the port_*
 * host hooks that don't belong to a specific subsystem (time, logging, save
 * paths, frame driving). This is the Mario Kart 64 port's main.c with
 * SM64's own loop in it (sm64-port's pc_main.c: one game_loop_one_iteration()
 * per paced frame, then one frame of audio).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* dl_iterate_phdr (SM64_CODELOCK) */
#endif
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/resource.h>

#include <ultra64.h>
#include "port.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "savestate.h"
#if defined(MIYOO)
#include "miyoo_clock.h"
#endif

/* Backends (defined in the sibling src/port/sdl/*.c files). */
extern struct GfxWindowManagerAPI gfx_sdl;
extern struct GfxRenderingAPI     gfx_soft_api;
extern void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *window_title, bool start_in_fullscreen);
extern void gfx_run(Gfx *commands);
extern void gfx_start_frame(void);
extern void gfx_end_frame(void);

/* Game entry (sm64-port's split of thread5_game_loop, src/game/game_init.c). */
#include "sm64.h"
#include "game/memory.h"
#include "game/game_init.h"
#include "audio/external.h"
#include "audio/data.h"
extern void thread5_game_loop(void *arg);
extern void game_loop_one_iteration(void);
extern void create_next_audio_buffer(s16 *samples, u32 num_samples);
extern void port_audio_out_init(void);
extern void port_audio_out_push(const s16 *samples, u32 bytes);
extern u32 port_audio_out_queued_bytes(void);
extern u32 port_audio_out_cushion_samples(void);
extern void port_audio_mix_lock(void);
extern void port_audio_mix_unlock(void);

/* src/game/main.c isn't built off the N64; sm64-port's pc_main.c defined these. */
OSMesg gMainReceivedMesg;
OSMesgQueue gSIEventMesgQueue;
s8 gResetTimer;
s8 gNmiResetBarsTimer;
s8 gDebugLevelSelect;
s8 gShowProfiler;
s8 gShowDebugText;

static const char *sProgName SAVESTATE_EXCLUDE; /* argv[0] */
static volatile int sRunning SAVESTATE_EXCLUDE = 1; /* process lifetime, not game state */
void port_request_exit(void) { sRunning = 0; }

/* -------------------------------------------------------------- logging --- */
/* Excluded from save states: a FILE* is a heap address in THIS process. A state
 * loaded in a later session would otherwise restore the old session's pointer,
 * and the next log line would write through it (MK64 round 19). */
static FILE *sLogFile SAVESTATE_EXCLUDE;

static void log_write(const char *buf) {
    fputs(buf, stdout);
    fflush(stdout);
    if (sLogFile == NULL) {
        /* Own copy of the log, independent of where stdout points; rotate the
         * previous session so a short later launch never wipes a long one. */
        rename(port_save_path("log.1.txt"), port_save_path("log.2.txt"));
        rename(port_save_path("log.txt"), port_save_path("log.1.txt"));
        sLogFile = fopen(port_save_path("log.txt"), "w");
    }
    if (sLogFile != NULL) {
        fputs(buf, sLogFile);
        fflush(sLogFile);
    }
}

void port_log(const char *fmt, ...) {
    /* SM64_QUIET=1 drops the chatty logging: per-frame traces (with an
     * fflushed log file) dominate wall-clock time on slow/emulated hosts. */
    static int sQuiet SAVESTATE_EXCLUDE = -1;
    if (sQuiet < 0) { const char *e = getenv("SM64_QUIET"); sQuiet = (e && e[0] == '1') ? 1 : 0; }
    if (sQuiet) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_write(buf);
}

/* PERF / STALL / session markers: never silenced. */
void port_log_always(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_write(buf);
}

/* ------------------------------------------------------------ addresses --- */
/* NO_SEGMENTED_MEMORY: display lists carry real pointers (all >= 0x10000000,
 * where the executable is linked). Anything lower would be an N64 segmented
 * address the port never set up: report the first few, pass it through. */
void *port_seg_to_ptr(uintptr_t addr) {
    if (addr != 0 && !port_is_real_ptr(addr)) {
        static int sReported SAVESTATE_EXCLUDE;
        if (sReported < 8) { sReported++; port_log_always("gfx: segmented address %08X in a display list\n", (unsigned) addr); }
    }
    return (void *) addr;
}

u32 port_time_us(void); /* below, with the other time helpers */

/* ------------------------------------------------------ SM64 host hooks --- */
/* sm64-port's pc_main.c provided these. The RSP tasks run inline: the display
 * list through the kit's interpreter, audio in port_sm64_audio_frame(). */
void dispatch_audio_sptask(UNUSED struct SPTask *spTask) {
}

void set_vblank_handler(UNUSED s32 index, UNUSED struct VblankHandler *handler, UNUSED OSMesgQueue *queue, UNUSED OSMesg *msg) {
}

static int sGameInited SAVESTATE_EXCLUDE; /* thread5_game_loop's setup has run */
#if defined(MIYOO)
/* SM64_AUDIOJOB (2026-10-12, default 1; the Miyoo with the worker on): the frame's audio (port_sm64_audio_frame: the
 * sequence players and the synthesis, 0.15-0.65M instructions a frame under qemu) runs on the second core while this
 * core draws the frame, instead of after it on this core. It starts once the game's logic for the frame is done (the
 * display list is about to run: exec_display_list), and nothing the game does until the frame's end touches the audio
 * state (the display list is the renderer's alone); it holds the mix lock, as the backstop thread's frames always have.
 * The renderer's first job for the worker waits for it (gfx_miyoo_job_fence), and so does the iteration's end, before
 * any save state or the next logic. The same calls in the same order: the same sound. 0: as before. */
void port_sm64_audio_frame(void);
int gMiyooAudioJob SAVESTATE_EXCLUDE = -1;
static int sAudioThisFrame SAVESTATE_EXCLUDE;        /* the frame's audio went to the worker */
static volatile int sAudioJobOut SAVESTATE_EXCLUDE;
static void audio_job(void *unused) {
    (void)unused;
    port_audio_mix_lock();
    port_sm64_audio_frame();
    port_audio_mix_unlock();
    __atomic_store_n(&sAudioJobOut, 0, __ATOMIC_RELEASE); /* (seen 0 = everything the job wrote is seen) */
    { extern volatile int gMiyooJobBusy; __atomic_store_n(&gMiyooJobBusy, 0, __ATOMIC_RELEASE); }
}
#endif
void exec_display_list(struct SPTask *spTask) {
    if (!sGameInited) {
        return;
    }
#if defined(MIYOO)
    {
        extern int miyoo_worker_ready(void);
        extern void miyoo_worker_run(void (*fn)(void *), void *arg);
        extern void gfx_miyoo_job_fence(void);
        extern volatile int gMiyooJobOut;
        extern int gMiyooMt;
        if (gMiyooAudioJob < 0) { const char *e = getenv("SM64_AUDIOJOB"); gMiyooAudioJob = !(e && e[0] == '0'); }
        if (gMiyooAudioJob > 0 && gMiyooMt && !sAudioThisFrame && miyoo_worker_ready()) {
            gfx_miyoo_job_fence(); /* the worker takes one job at a time (the last frame's present) */
            extern volatile int gMiyooJobBusy;
            sAudioThisFrame = 1; sAudioJobOut = 1; gMiyooJobOut = 1; gMiyooJobBusy = 1;
            miyoo_worker_run(audio_job, NULL);
        }
    }
#endif
    const u32 t = port_time_us();
    gfx_run((Gfx *) spTask->task.t.data_ptr);
    PORT_PROF_ADD(PROF_GFX, port_time_us() - t);
}

/* One game frame of audio: two AI buffers of 528 or 544 samples at 32 kHz (the
 * US timing sm64-port uses), the long ones while the ring is under its cushion.
 * Called by the game thread each frame and by the backstop thread when the ring
 * runs low (both under port_audio_mix_lock). */
#define SAMPLES_HIGH 544
#define SAMPLES_LOW 528
extern int port_audio_sync(void);
/* Samples for the next 1/60 s buffer: long while the ring is under its
 * cushion, else short -- or, under SM64_AUDIO_SYNC, exactly 32000/60 on
 * average (533 or 534), whatever the ring holds. */
static u32 audio_buffer_samples(void) {
    if (port_audio_sync()) {
        static u32 sAcc SAVESTATE_EXCLUDE;
        sAcc += 32000;
        const u32 n = sAcc / 60;
        sAcc %= 60;
        return n;
    }
    const u32 queued = port_audio_out_queued_bytes() / 4;
    return queued < port_audio_out_cushion_samples() ? SAMPLES_HIGH : SAMPLES_LOW;
}
void port_sm64_audio_frame(void) {
    s16 buf[SAMPLES_HIGH * 2 * 2];
    const u32 n0 = audio_buffer_samples();
    const u32 n1 = port_audio_sync() ? audio_buffer_samples() : n0;
    create_next_audio_buffer(buf, n0);
    create_next_audio_buffer(buf + n0 * 2, n1);
    port_audio_out_push(buf, (n0 + n1) * 4);
}

/* One N64 audio frame (1/60 s) for wait_for_audio_frames, which the N64's game
 * thread spins in while its audio thread renders: audio_reset_session waits
 * there for the notes' release and the reverb tail, then for the load lock.
 * We have no audio thread to wait for, so render the frame here -- paced by
 * the output, as the N64's is by the AI -- or silence while the heap is being
 * rebuilt (the N64 skips synthesis under AUDIO_LOCK_LOADING). sm64-port
 * returned at once, so every reset cut the sound off hard. Runs with the mix
 * lock held (sound_reset).
 *
 * SM64_RESETSILENT (default 1; 0 = every frame waited for and played, as before; 2026-10-05, the
 * user: the wipes "have a slight pause when they expand"). A frame that comes out all zero is
 * silence: it is neither played nor waited for. The reset's three frames under the load lock are
 * always silent, and a deferred reset (sound_init.c defer_tick) comes when nothing would be cut, so
 * most of its release and reverb frames are silent too (desktop, BoB/JRB's act select: 5-7 of 7-8).
 * Waiting for each to play held the game 1/60 s a frame, and the deferred reset lands in the wipe's
 * first opening frames: on the device 70-130 ms of a 90-170 ms stall (qemu: the reset's own work
 * is 0.25M instructions, the rest is that wait). Every sample that sounds is still played, in the
 * same order, paced as before; game and sound both go on sooner by the silence taken out, so the
 * game frames meet the same sound as before. The frame is still made (the sequence players, the
 * sample counts and gAudioFrameCount advance as before): no game frame changes. */
int port_reset_silent SAVESTATE_EXCLUDE = -1; /* the device bench's "resetsilent-off" windows set 0 */
static void audio_wait_frame(void);
void port_sm64_audio_wait_frame(void) { /* STALLWHY: the reset's frames, made and waited for (port.h) */
    PORT_WHY_ADD(WHY_RFRAMES, 1);
    audio_wait_frame();
}
/* SM64_RESETNOWAIT (default 1; 0 = as before. 2026-10-06, the user: "a small hitch" walking into the castle and
 * as a course's iris opens). Device logs and the desktop's STALLWHY: those hitches are the audio reset's sounding
 * release and reverb frames (4-6 of them, peaks -24 to -36 dBFS: the reverb tail, audible, so not droppable),
 * each waiting for room under the output ring's cushion, about 1/60 s apiece, all inside one game frame: 60-110 ms
 * of a frozen picture. The deferred reset (sound_init.c) lands in the iris opening, so the freeze shows.
 * Now the reset's frames (only those in audio_reset_session, port_reset_session) go into the ring at once while it
 * has room for them (the cushion plus 8 frames; the ring holds 32 blocks), in the same order, the same samples. The
 * time they would have waited becomes a debt the frame pacer (gfx_sdl12.c) pays in the frames after: at most 4 ms a
 * frame on top of an on-time frame's sleep, and a late frame pays with its lateness. So every sample is the same and
 * plays in the same order; the ring is back to its cushion (sound to picture as before) within about a second; the
 * game logic is untouched; the picture runs those frames a little slower instead of freezing. The sound-effect
 * hold of sound_reset (a fade or a voice at a reset, the screen held black) keeps its waits. */
static int port_reset_session SAVESTATE_EXCLUDE;   /* inside audio_reset_session's frames */
volatile u32 gPortResetDebtUs SAVESTATE_EXCLUDE;  /* the waits not taken, for the pacer */
#define RESET_NOWAIT_EXTRA (SAMPLES_HIGH * 8u)
/* heap.c's wait_for_audio_frames, around audio_reset_session's frames.
 * SM64_RINGFILL=1 (test hook, with SM64_AUDIO_SYNC=1): set the output ring to its cushion (silence added, or
 * the excess dropped) as the device's ring stands when a reset comes, and let the reset's waits run as on the
 * device; the game itself stays frame-exact (sync), so a desktop run measures the device's waits repeatably. */
static int sRingFill SAVESTATE_EXCLUDE = -1;
void port_reset_session_frames(int on) {
    port_reset_session = on;
    if (sRingFill < 0) { const char *e = getenv("SM64_RINGFILL"); sRingFill = e && e[0] == '1'; }
    if (on && sRingFill) {
        extern void port_audio_out_test_trim(u32 samples);
        port_audio_out_test_trim(port_audio_out_cushion_samples());
        static s16 z[SAMPLES_HIGH * 2] SAVESTATE_EXCLUDE;
        u32 q;
        while ((q = port_audio_out_queued_bytes() / 4) < port_audio_out_cushion_samples()) {
            const u32 k = port_audio_out_cushion_samples() - q;
            port_audio_out_push(z, (k < SAMPLES_HIGH ? k : SAMPLES_HIGH) * 4);
        }
    }
}
static int reset_nowait(void) {
    static int s SAVESTATE_EXCLUDE = -1;
    if (s < 0) { const char *e = getenv("SM64_RESETNOWAIT"); s = !(e && e[0] == '0'); }
    return s;
}
/* the wait for room in the output ring (at most 100 ms) before n samples go in */
static void audio_wait_room(u32 n) {
    const int sync = port_audio_sync() && !(port_reset_session && sRingFill > 0);
    if (port_reset_session && reset_nowait() && !sync) {
        const u32 q = port_audio_out_queued_bytes() / 4, c = port_audio_out_cushion_samples();
        if (q + n <= c + RESET_NOWAIT_EXTRA) {
            if (q >= c) { /* what the wait below would have let drain (to under the cushion, at most this frame's worth
                           * beyond what earlier frames already owe): the debt */
                const u32 over = q - c, extra = over < n ? over : n;
                extern u32 port_audio_out_rate(void);
                gPortResetDebtUs += (u32)((u64)extra * 1000000u / port_audio_out_rate());
                PORT_WHY_ADD(WHY_REARLY, 1);
            }
            return;
        }
    }
    const u32 t = port_time_us();
    for (int i = 0; i < 50 && !sync && port_audio_out_queued_bytes() / 4 >= port_audio_out_cushion_samples(); i++) {
        usleep(2000); /* at most 100 ms: a stalled output can't hang a reset */
    }
    PORT_WHY_ADD(WHY_RWAIT, port_time_us() - t);
}
static void audio_wait_frame(void) {
    s16 buf[SAMPLES_HIGH * 2];
    if (port_reset_silent < 0) { const char *e = getenv("SM64_RESETSILENT"); port_reset_silent = !(e && e[0] == '0'); }
    if (!port_reset_silent) { /* as before */
        audio_wait_room(SAMPLES_HIGH);
        const u32 n = audio_buffer_samples();
        if (gAudioLoadLock == AUDIO_LOCK_NOT_LOADING) {
            const u32 t = port_time_us();
            create_next_audio_buffer(buf, n); /* counts gAudioFrameCount */
            PORT_WHY_ADD(WHY_RSYNTH, port_time_us() - t);
        } else {
            memset(buf, 0, n * 4);
            gAudioFrameCount++;
        }
        port_audio_out_push(buf, n * 4);
        return;
    }
    /* The frame first, then the wait if it sounds. Its length is what audio_buffer_samples() picks
     * after the wait: the ring is under its cushion then, so the long one (only an output stalled for
     * the whole 100 ms left it over, and the short one); under SM64_AUDIO_SYNC its own count. */
    const u32 n = port_audio_sync() ? audio_buffer_samples() : SAMPLES_HIGH;
    if (gAudioLoadLock != AUDIO_LOCK_NOT_LOADING) {
        gAudioFrameCount++; /* silence while the heap is rebuilt */
        return;
    }
    { const u32 t = port_time_us();
      create_next_audio_buffer(buf, n); /* counts gAudioFrameCount */
      PORT_WHY_ADD(WHY_RSYNTH, port_time_us() - t); }
    u32 any = 0;
    for (u32 i = 0; i < n * 2; i++) any |= (u16)buf[i];
    if (any == 0) return; /* silence */
    PORT_WHY_ADD(WHY_RSOUND, 1);
    audio_wait_room(n);
    port_audio_out_push(buf, n * 4);
}

/* ----------------------------------------------------------------- time --- */
static u32 port_clock_us_sys(void) {
#if defined(SYS_clock_gettime32)
    /* musl 1.2's clock_gettime tries the time64 syscall first, and the handheld's
     * 4.14 kernel lacks it, so every read costs two kernel entries. The
     * backend's profile buckets read this clock ~500 times a frame. */
    struct { long s, ns; } t;
    if (syscall(SYS_clock_gettime32, CLOCK_MONOTONIC, &t) == 0) {
        return (u32)((u64)t.s * 1000000u + (u32)(t.ns / 1000));
    }
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32)((u64)ts.tv_sec * 1000000u + (u32)(ts.tv_nsec / 1000));
}

/* SM64_VDSOCLOCK (default 1; 0 = the syscall always, as before; 2026-10-05): the kernel's own
 * __vdso_clock_gettime, the same CLOCK_MONOTONIC read in user space instead of a syscall. The
 * the handheld's musl 1.2.1 never looks the vDSO up. GoldenEye's device Bench (X37): 157-167 ns a read
 * against 598-611 ns for the syscall; taken as mario-kart-64's MK64_VDSOCLOCK / diddy-kong-racing's
 * DKR_VDSOCLOCK. The vDSO is found through AT_SYSINFO_EHDR's symbol table and used only if, best
 * of three trials of 256 reads each, its readings lie between the syscall's around them and cost
 * under half as much; one CLOCK line in the log says which. Only the cost of a read changes, never
 * the clock read. The device bench (bench_window_end) turns it off in its "vdsoclock-off" windows. */
typedef int (*PortCgt32)(int, void *);
static PortCgt32 sVdsoCgt SAVESTATE_EXCLUDE;    /* this process's address, never a saved one */
static int sClockMode SAVESTATE_EXCLUDE = -1;   /* -1 not picked, 0 syscall, 1 vDSO */
int port_clock_vdso_off SAVESTATE_EXCLUDE;      /* the bench's OFF windows: the syscall */

static u32 port_clock_us_vdso(void) {
    struct { long s, ns; } t;
    if (sVdsoCgt(CLOCK_MONOTONIC, &t) == 0) return (u32)((u64)t.s * 1000000u + (u32)(t.ns / 1000));
    return port_clock_us_sys();
}

#if defined(__linux__) && __SIZEOF_POINTER__ == 4
#include <elf.h>
#include <sys/auxv.h>
/* A symbol of the vDSO image (32-bit ELF), as musl's __vdsosym; NULL if none. */
static void *port_vdso_sym(const char *want) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)(uintptr_t)getauxval(AT_SYSINFO_EHDR);
    uintptr_t bias = 0;
    int have = 0;
    if (eh == NULL || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS32) return NULL;
    const Elf32_Phdr *ph = (const Elf32_Phdr *)((const char *)eh + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) { bias = (uintptr_t)eh + ph[i].p_offset - ph[i].p_vaddr; have = 1; break; }
    if (!have || eh->e_shoff == 0) return NULL;
    const Elf32_Shdr *sh = (const Elf32_Shdr *)((const char *)eh + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum) continue;
        const Elf32_Sym *sym = (const Elf32_Sym *)((const char *)eh + sh[i].sh_offset);
        const char *str = (const char *)eh + sh[sh[i].sh_link].sh_offset;
        const unsigned n = sh[i].sh_size / sizeof(Elf32_Sym);
        for (unsigned k = 0; k < n; k++)
            if (ELF32_ST_TYPE(sym[k].st_info) == STT_FUNC && sym[k].st_shndx != SHN_UNDEF && strcmp(str + sym[k].st_name, want) == 0)
                return (void *)(bias + sym[k].st_value);
    }
    return NULL;
}
#else
static void *port_vdso_sym(const char *want) { (void)want; return NULL; }
#endif

/* 256 reads of each, the syscall's around the vDSO's: 0 if the vDSO's readings fall outside them
 * (microseconds; the costs in ns a read) */
static int port_clock_trial(u32 *vdso_ns, u32 *sys_ns) {
    const u32 t0 = port_clock_us_sys();
    for (int i = 0; i < 256; i++) (void)port_clock_us_sys();
    const u32 t1 = port_clock_us_sys();
    const u32 a = port_clock_us_vdso();
    for (int i = 0; i < 256; i++) (void)port_clock_us_vdso();
    const u32 b = port_clock_us_vdso();
    const u32 t2 = port_clock_us_sys();
    *vdso_ns = (u32)((u64)(b - a) * 1000u / 257u);
    *sys_ns = (u32)((u64)(t1 - t0) * 1000u / 257u);
    return (s32)(a - t1) >= 0 && (s32)(t2 - b) >= 0;
}

/* Once, at start-up before any other thread (port_time_us picks on its first read otherwise). */
void port_clock_pick(void) {
    if (sClockMode >= 0) return;
    const char *e = getenv("SM64_VDSOCLOCK");
    const int on = !(e && e[0] == '0');
    sVdsoCgt = on ? (PortCgt32)port_vdso_sym("__vdso_clock_gettime") : NULL;
    if (sVdsoCgt == NULL) {
        sClockMode = 0;
        port_log_always("CLOCK syscall (%s)\n", on ? "no vDSO clock_gettime" : "SM64_VDSOCLOCK=0");
        return;
    }
    u32 v = ~0u, s = ~0u;
    int same = 1;
    for (int i = 0; i < 3; i++) { /* the best of three (a GoldenEye device run once caught an interrupt inside one) */
        u32 tv, ts;
        same &= port_clock_trial(&tv, &ts);
        if (tv < v) v = tv;
        if (ts < s) s = ts;
    }
    sClockMode = same && (u64)v * 2 < s;
    port_log_always("CLOCK %s: vDSO %u ns/read vs syscall %u ns/read\n", sClockMode ? "vdso" : "syscall", v, s);
    if (!sClockMode) sVdsoCgt = NULL;
}

int port_clock_is_vdso(void) { return sClockMode > 0; }

u32 port_time_us(void) {
    if (sClockMode > 0 && !port_clock_vdso_off) return port_clock_us_vdso();
    if (sClockMode < 0) { port_clock_pick(); if (sClockMode > 0) return port_clock_us_vdso(); }
    return port_clock_us_sys();
}

/* Frame profile (see port.h). The game's PORT_PROFILE hooks report through
 * port_profile_add: slot 0 = game logic, 1 = gfx run, 2 = gfx end, 3 = audio. */
u32 gPortProfFrame[PROF_COUNT] SAVESTATE_EXCLUDE;
u32 gPortProfLast[PROF_COUNT] SAVESTATE_EXCLUDE;
volatile u32 gPortProfFlipUs SAVESTATE_EXCLUDE;
u32 gPortWhy[WHY_COUNT] SAVESTATE_EXCLUDE;
u32 gPortWhyCmd[64] SAVESTATE_EXCLUDE;
/* STALLWHY (2026-10-06): a frame whose game logic took over 40 ms (SM64_WHYUS=<us>: another threshold) gets
 * a line with its named parts, in us (port.h). script = the level script's commands (the top three by
 * time: "c<hex opcode>"; 12 = the level's update call, 11 = a one-off call); objs, music, reset and
 * eeprom are inside it; reset's frames = the release and reverb frames it made, sounding = those not
 * silent, early = those put in the ring at once (SM64_RESETNOWAIT), wait = the time spent waiting for room in the output ring, synth = making them. */
static void why_frame_end(u32 logic_us) {
    static long sWhyUs SAVESTATE_EXCLUDE = -2;
    if (sWhyUs == -2) { const char *e = getenv("SM64_WHYUS"); sWhyUs = e ? atol(e) : 40000; }
    if (logic_us > (u32)sWhyUs) {
        u32 top[3] = { 0, 0, 0 }; int topc[3] = { -1, -1, -1 };
        for (int c = 0; c < 64; c++) {
            const u32 v = gPortWhyCmd[c];
            for (int k = 0; k < 3; k++) if (v > top[k]) {
                for (int j = 2; j > k; j--) { top[j] = top[j - 1]; topc[j] = topc[j - 1]; }
                top[k] = v; topc[k] = c; break;
            }
        }
        const u32 named = gPortWhy[WHY_TICK] + gPortWhy[WHY_SCRIPT] + gPortWhy[WHY_RENDER];
        extern u32 gGlobalTimer;
        port_log_always("STALLWHY t%u logic %u us: tick %u script %u (c%02x %u, c%02x %u, c%02x %u) render %u other %u"
                        " | in script: objs %u music %u reset %u (frames %u sounding %u early %u wait %u synth %u) eeprom %u (fsync %u)\n",
                        gGlobalTimer, logic_us, gPortWhy[WHY_TICK], gPortWhy[WHY_SCRIPT],
                        topc[0] & 0xff, top[0], topc[1] & 0xff, top[1], topc[2] & 0xff, top[2],
                        gPortWhy[WHY_RENDER], logic_us > named ? logic_us - named : 0,
                        gPortWhy[WHY_OBJS], gPortWhy[WHY_MUSIC], gPortWhy[WHY_RESET], gPortWhy[WHY_RFRAMES],
                        gPortWhy[WHY_RSOUND], gPortWhy[WHY_REARLY], gPortWhy[WHY_RWAIT], gPortWhy[WHY_RSYNTH], gPortWhy[WHY_EEPROM],
                        gPortWhy[WHY_EEFSYNC]);
    }
    memset(gPortWhy, 0, sizeof(gPortWhy));
    memset(gPortWhyCmd, 0, sizeof(gPortWhyCmd));
}
void port_profile_add(int slot, u32 us) {
    static const int map[4] = { PROF_LOGIC, PROF_GFX, PROF_GFX, PROF_AUDIO };
    if (slot < 0 || slot >= 4) return;
    gPortProfFrame[map[slot]] += us;
}

/* Every 150 iterations: averages of every bucket, the worst iteration, how
 * many iterations actually rendered (the rest were frameskip), the 3D
 * internal size the ladder settled on, and audio underruns in the window.
 * interp = gfx minus the backend buckets = display-list interpretation,
 * vertex transform/clip, batching. All wall-clock on the game thread; flip is
 * the present thread's SDL_Flip time (blocking there costs the game nothing). */
static int bench_on(void);
static void bench_window_end(void);
static const char *sBenchName SAVESTATE_EXCLUDE; /* the bench's variant this window (see bench_on) */
/* SM64_LOGVERBOSE=1 (or logverbose=1 in sm64_bench.cfg): the full logging of before 2026-10-05, a PERF line
 * every 150 iterations instead of every 750 in normal play. A bench run always prints every window. */
static int port_logverbose(void) {
    static int sV SAVESTATE_EXCLUDE = -1;
    if (sV < 0) {
        const char *e = getenv("SM64_LOGVERBOSE");
        sV = e && e[0] == '1';
        if (!(e && e[0])) {
            extern const char *port_save_path(const char *name);
            FILE *f = fopen(port_save_path("sm64_bench.cfg"), "r");
            if (f) {
                char line[64];
                while (fgets(line, sizeof(line), f)) if (!strncmp(line, "logverbose=1", 12)) sV = 1;
                fclose(f);
            }
        }
    }
    return sV;
}
static void prof_iteration_end(u32 iter_us, int rendered) {
    static u32 sIt SAVESTATE_EXCLUDE, sSub SAVESTATE_EXCLUDE;
    static u32 sSum[PROF_COUNT] SAVESTATE_EXCLUDE, sMax SAVESTATE_EXCLUDE, sN SAVESTATE_EXCLUDE, sRendered SAVESTATE_EXCLUDE;
    static u32 sFlipSum SAVESTATE_EXCLUDE, sUnderrunsPrev SAVESTATE_EXCLUDE;
    gPortProfFrame[PROF_ITER] = iter_us;
    for (int i = 0; i < PROF_COUNT; i++) { sSum[i] += gPortProfFrame[i]; gPortProfLast[i] = gPortProfFrame[i]; gPortProfFrame[i] = 0; }
    why_frame_end(gPortProfLast[PROF_LOGIC]);
    { extern void port_bench2_frame(u32, u32, u32, u32, u32, u32); /* the level bench (hh_bench.c) */
      port_bench2_frame(iter_us - gPortProfLast[PROF_SLEEP], gPortProfLast[PROF_LOGIC], gPortProfLast[PROF_AUDIO],
                        gPortProfLast[PROF_GFX] > gPortProfLast[PROF_SLEEP] ? gPortProfLast[PROF_GFX] - gPortProfLast[PROF_SLEEP] : 0,
                        gPortProfLast[PROF_R3D], gPortProfLast[PROF_R2D]); }
    if (iter_us > sMax) sMax = iter_us;
    sRendered += rendered ? 1 : 0;
    static long sStallEnv SAVESTATE_EXCLUDE = -2; /* SM64_STALLUS=<us> (test hook): the STALL line's threshold */
    if (sStallEnv == -2) { const char *e = getenv("SM64_STALLUS"); sStallEnv = e ? atol(e) : -1; }
    extern int port_bench2_on(void);
    const u32 stall_us = sStallEnv >= 0 ? (u32)sStallEnv : (bench_on() || port_bench2_on() || port_logverbose()) ? 80000 : 100000; /* normal play: over 100 ms only (2026-10-06, the slow-frame lines across the ports); the Bench and SM64_LOGVERBOSE=1 keep 80 */
    /* page faults in this iteration (2026-10-06, the wipe stall): "pf" = pages read from the card (major) and
     * pages first touched (minor), in the STALL lines. One getrusage a frame. */
    static long sMaj SAVESTATE_EXCLUDE = -1, sMin SAVESTATE_EXCLUDE;
    long dMaj = 0, dMin = 0;
    { struct rusage ru; if (getrusage(RUSAGE_SELF, &ru) == 0) { if (sMaj >= 0) { dMaj = ru.ru_majflt - sMaj; dMin = ru.ru_minflt - sMin; } sMaj = ru.ru_majflt; sMin = ru.ru_minflt; } }
    if (sStallEnv >= 0 && iter_us > stall_us + gPortProfLast[PROF_SLEEP]) {
        extern u32 gGlobalTimer;
        port_log_always("STALLUS t%u: iter %u logic %u audio %u gfx %u r3d %u pf %ld/%ld\n", gGlobalTimer, iter_us - gPortProfLast[PROF_SLEEP],
                        gPortProfLast[PROF_LOGIC], gPortProfLast[PROF_AUDIO], gPortProfLast[PROF_GFX], gPortProfLast[PROF_R3D], dMaj, dMin);
    } else if (iter_us > stall_us + gPortProfLast[PROF_SLEEP]) { /* a stall the audio backstop had to cover: say where */
        extern u32 port_audio_out_underruns(void);
        port_log_always("STALL %u ms: logic %u audio %u gfx %u (tex %u r2d %u r3d %u) pf %ld/%ld underruns %u%s%s\n",
                 iter_us / 1000, gPortProfLast[PROF_LOGIC] / 1000, gPortProfLast[PROF_AUDIO] / 1000,
                 gPortProfLast[PROF_GFX] / 1000, gPortProfLast[PROF_TEX] / 1000,
                 gPortProfLast[PROF_R2D] / 1000, gPortProfLast[PROF_R3D] / 1000, dMaj, dMin, port_audio_out_underruns(),
                 bench_on() ? " | bench " : "", bench_on() ? sBenchName : "");
    }
    { u32 f = gPortProfFlipUs; gPortProfFlipUs = 0; sFlipSum += f; }
    sN++;
    if (++sIt < 150) return;
    sIt = 0;
    if (!bench_on() && !port_logverbose() && ++sSub < 5) return; /* normal play: one PERF line per 5 windows, the sums keep adding up */
    sSub = 0;
    extern u32 port_audio_out_underruns(void);
    extern void gfx_soft_get_internal(int *w, int *h);
    int iw = 0, ih = 0; gfx_soft_get_internal(&iw, &ih);
    /* The pacing sleep (and the menu) run inside gfx start_frame, so they sit
     * inside the GFX bucket: take them out before deriving the interpreter. */
    const u32 back = sSum[PROF_R3D] + sSum[PROF_R2D] + sSum[PROF_TEX] + sSum[PROF_COMP] + sSum[PROF_CLR] + sSum[PROF_PRES] + sSum[PROF_SLEEP];
    const u32 interp = sSum[PROF_GFX] > back ? sSum[PROF_GFX] - back : 0;
    extern u32 port_audio_thread_mixes(void);
    static u32 sMixPrev SAVESTATE_EXCLUDE;
    const u32 ur = port_audio_out_underruns();
    const u32 mixes = port_audio_thread_mixes();
#define AVG(i) ((float)sSum[i] / (1000.0f * sN))
    char bench[48] = "";
    if (bench_on()) snprintf(bench, sizeof(bench), " | bench %s", sBenchName);
#if defined(MIYOO) /* the second core: split triangles, worker jobs and sleeps (gfx_soft.c) */
    extern int gfx_soft_mt_line(char *buf, int n);
    char mt[96];
    gfx_soft_mt_line(mt, sizeof(mt));
#define PERF_MT "%s"
#define PERF_MT_ARG , mt
#else
#define PERF_MT ""
#define PERF_MT_ARG
#endif
    port_log_always("PERF work %.1f ms avg (iter %.1f, max %.1f, sleep %.1f) rendered %u/%u | logic %.1f audio %.1f gfx %.1f = interp %.1f + r3d %.1f + r2d %.1f + tex %.1f + comp %.1f + clr %.1f + pres %.1f | flip(thread) %.1f | 3d-res %dx%d | underruns +%u backstop-mixes +%u%s" PERF_MT "\n",
             AVG(PROF_ITER) - AVG(PROF_SLEEP), AVG(PROF_ITER), sMax / 1000.0f, AVG(PROF_SLEEP), sRendered, sN,
             AVG(PROF_LOGIC), AVG(PROF_AUDIO), AVG(PROF_GFX) - AVG(PROF_SLEEP), (float)interp / (1000.0f * sN),
             AVG(PROF_R3D), AVG(PROF_R2D), AVG(PROF_TEX), AVG(PROF_COMP), AVG(PROF_CLR), AVG(PROF_PRES),
             (float)sFlipSum / (1000.0f * sN), iw, ih, ur - sUnderrunsPrev, mixes - sMixPrev, bench PERF_MT_ARG);
#undef PERF_MT
#undef PERF_MT_ARG
#undef AVG
    /* Raster shape (device round 10: compute vs memory bound). The span-pixel
     * count needs no PMU and already separates the two cases: a big overdraw
     * factor with low ns/px = overdraw-bound (cut pixels); a small overdraw
     * factor with high ns/px = per-pixel/memory-bound (cut per-pixel cost).
     * The PMU line, when available, confirms it (IPC + L1D-misses/px). */
    {
        extern int pmu_ok(void);
        extern uint64_t gfx_soft_span_px, gfx_soft_tris;
        static uint64_t sCyc SAVESTATE_EXCLUDE, sIns SAVESTATE_EXCLUDE, sMiss SAVESTATE_EXCLUDE, sPx SAVESTATE_EXCLUDE;
        extern uint64_t gPmuCyc, gPmuIns, gPmuMiss; /* accumulated in run_one_iteration */
        const uint64_t dc = gPmuCyc - sCyc, di = gPmuIns - sIns, dm = gPmuMiss - sMiss;
        const uint64_t dpx = gfx_soft_span_px - sPx;
        static uint64_t sTri SAVESTATE_EXCLUDE; const uint64_t dtri = gfx_soft_tris - sTri; sTri = gfx_soft_tris;
        sCyc = gPmuCyc; sIns = gPmuIns; sMiss = gPmuMiss; sPx = gfx_soft_span_px;
        if (dpx > 0 && sRendered > 0) {
            const double px_per_frame = (double)dpx / sRendered;
            const double r3d_us = (double)sSum[PROF_R3D] / sRendered;
            port_log_always("GFX3D %.0fk span-px/frame (overdraw %.1fx) | %.0f tris (%.0f px/tri) | %.1f ns/px | r3d %.1f ms\n",
                     px_per_frame / 1000.0, px_per_frame / ((iw > 0 && ih > 0) ? (double)iw * ih : 240.0 * 240.0),
                     (double)dtri / sRendered, dtri ? (double)dpx / dtri : 0.0,
                     dpx ? r3d_us * 1000.0 / px_per_frame : 0.0, r3d_us / 1000.0);
        }
        if (pmu_ok() && dpx > 0) {
            port_log_always("PMU   IPC %.2f | %.0f cyc/px | %.2f L1Dmiss/px (cyc %.0fM insn %.0fM miss %.0fM)\n",
                     di ? (double)di / (double)dc : 0.0, (double)dc / (double)dpx, (double)dm / (double)dpx,
                     (double)dc / 1e6, (double)di / 1e6, (double)dm / 1e6);
        }
    }
    sUnderrunsPrev = ur; sMixPrev = mixes;
    memset(sSum, 0, sizeof(sSum)); sMax = 0; sN = 0; sRendered = 0; sFlipSum = 0;
    bench_window_end();
}

/* The device bench: SM64_BENCH=1, or bench=1 in <data>/sm64_bench.cfg, runs every speed switch of
 * the 2026-10-05 and 2026-10-06 rounds against the build's defaults, one PERF window each: the windows alternate
 * "all-on" and one switch OFF, the OFF switch taking the next turn each time (all-on, vdsoclock-off,
 * all-on, alphaskip-off, ...), so each OFF window has an all-on window on either side to pair with.
 * Each PERF line ends "| bench <variant>". Every switch here leaves the picture as it is: only the
 * time changes. (Earlier rounds flipped the crack fill, measured +2.56 ms and kept; the row
 * prefetch; the round-1 NEON paths and the direct present.) The PERF line of the window just
 * measured is printed by prof_iteration_end before bench_window_end sets up the next one. */
#ifndef SM64_BENCH_DEFAULT
#define SM64_BENCH_DEFAULT 0 /* 1 in bd58f684 for the crack fill's device A/B (measured: +2.6 ms) */
#endif
extern int gfx_soft_alphaskip, gfx_soft_neonnoza, gfx_soft_crackbox, gfx_soft_cracklean;
extern int gfx_soft_fastrows, gfx_soft_spanvec, gfx_soft_shadepx, gfx_soft_zskip, gfx_soft_neonnoto, gfx_soft_wraprr;
extern int gfx_soft_skyclr, gfx_soft_lean, gfx_soft_fullst;
typedef struct { const char *name; int *var; int off; } BenchSwitch;
#if defined(MIYOO)
/* The Miyoo's bench (bench=1 in data/sm64_bench.cfg): its own switches only -- the second core's work at all (mt-off:
 * everything on one core, the worker idle, as threads=1) and each job of the 2026-10-12 round (the other switches
 * stay at their defaults). */
extern int gMiyooMt, gMiyooPresJob, gMiyooAudioJob, gMiyooCrackSplit, gMiyooSplitRef;
#define BENCH_ROUND0 1
static BenchSwitch sBenchSw[] = {
    { "mt-off", &gMiyooMt, 0 },                    /* the second core (SM64_THREADS=1 at start does the same) */
    { "presjob-off", &gMiyooPresJob, 0 },          /* SM64_PRESJOB (the copy and doubling on the worker) */
    { "audiojob-off", &gMiyooAudioJob, 0 },        /* SM64_AUDIOJOB (the frame's audio on the worker) */
    { "cracksplit-off", &gMiyooCrackSplit, 0 },    /* SM64_CRACKSPLIT (the crack fill's scan on both cores) */
    { "splitref-off", &gMiyooSplitRef, 0 },        /* SM64_SPLITREF (handed-back rows noted, the share goes on) */
};
#else
#define BENCH_ROUND0 12 /* sBenchSw[BENCH_ROUND0..]: the round the level bench turns off in turn (2026-10-07: speed round 2) */
static BenchSwitch sBenchSw[] = {
    { "vdsoclock-off", &port_clock_vdso_off, 1 },  /* SM64_VDSOCLOCK (the syscall clock) */
    { "alphaskip-off", &gfx_soft_alphaskip, 0 },   /* SM64_ALPHASKIP */
    { "neonnoza-off", &gfx_soft_neonnoza, 0 },     /* SM64_NEONNOZA (the wipes' shape quad) */
    { "resetsilent-off", &port_reset_silent, 0 },  /* SM64_RESETSILENT (shows in a STALL line, not the averages) */
    { "crackbox-off", &gfx_soft_crackbox, 0 },   /* SM64_CRACKBOX (2026-10-06: the fill's scan and clears inside the covered box) */
    { "cracklean-off", &gfx_soft_cracklean, 0 },   /* SM64_CRACKLEAN (2026-10-06: the fill's lean NEON test) */
    { "fastrows-off", &gfx_soft_fastrows, 0 },     /* SM64_FASTROWS (gfx_soft.c tri_rows) */
    { "spanvec-off", &gfx_soft_spanvec, 0 },       /* SM64_SPANVEC (span_row_v; ZSKIP, SHADEPX and WRAPRR live in its rows) */
    { "shadepx-off", &gfx_soft_shadepx, 0 },       /* SM64_SHADEPX (smooth shade one vector a pixel) */
    { "zskip-off", &gfx_soft_zskip, 0 },           /* SM64_ZSKIP (spans behind the depth buffer skipped) */
    { "neonnoto-off", &gfx_soft_neonnoto, 0 },     /* SM64_NEONNOTO (the power meter's path on NEON) */
    { "wraprr-off", &gfx_soft_wraprr, 0 },         /* SM64_WRAPRR (repeating textures' wrap by AND) */
    /* speed round 2 (2026-10-07) */
    { "skyclr-off", &gfx_soft_skyclr, 0 },         /* SM64_SKYCLR (no alpha-clear pass over a target just cleared) */
    { "lean-off", &gfx_soft_lean, 0 },             /* SM64_LEAN (the rows' usual settings compiled in; FULLST lives in its rows) */
    { "fullst-off", &gfx_soft_fullst, 0 },         /* SM64_FULLST (full groups stored without loading the old pixels) */
};
#endif
#define BENCH_N ((int) (sizeof(sBenchSw) / sizeof(sBenchSw[0])))
static int sBenchOn[BENCH_N] SAVESTATE_EXCLUDE;
void port_bench_switches_init(void);
extern int port_clock_is_vdso(void);  /* each switch's own value (its default or env) */
static int sBench SAVESTATE_EXCLUDE = -1;
static u32 sBenchWindow SAVESTATE_EXCLUDE;
static const char *sBenchName SAVESTATE_EXCLUDE = "all-on";
static int bench_on(void) {
    if (sBench < 0) {
        const char *e = getenv("SM64_BENCH");
        sBench = e ? e[0] == '1' : SM64_BENCH_DEFAULT;
        /* (until 2026-10-06 the bench build, sm64_bench, ran these windows; it now runs the level bench, hh_bench.c) */
        if (!sBench) {
            extern const char *port_save_path(const char *name);
            FILE *f = fopen(port_save_path("sm64_bench.cfg"), "r");
            if (f) {
                char line[64];
                while (fgets(line, sizeof(line), f)) if (!strncmp(line, "bench=1", 7)) sBench = 1;
                fclose(f);
            }
        }
        if (sBench) {
            port_bench_switches_init();
            port_log_always("bench: windows alternate all-on and one switch off (%d switches; clock %s)\n", BENCH_N,
                            port_clock_is_vdso() ? "vdso" : "syscall");
        }
    }
    return sBench;
}
/* The switches' own values (their defaults or env), read once; the level bench (hh_bench.c) uses them too. */
static int sBenchInit SAVESTATE_EXCLUDE;
void port_bench_switches_init(void) {
    if (sBenchInit) return;
    sBenchInit = 1;
    {
        {
            if (gfx_soft_alphaskip < 0) { const char *a = getenv("SM64_ALPHASKIP"); gfx_soft_alphaskip = !(a && a[0] == '0'); }
            if (gfx_soft_neonnoza < 0) { const char *a = getenv("SM64_NEONNOZA"); gfx_soft_neonnoza = !(a && a[0] == '0'); }
            if (port_reset_silent < 0) { const char *a = getenv("SM64_RESETSILENT"); port_reset_silent = !(a && a[0] == '0'); }
            if (gfx_soft_crackbox < 0) { const char *a = getenv("SM64_CRACKBOX"); gfx_soft_crackbox = !(a && a[0] == '0'); }
            if (gfx_soft_cracklean < 0) { const char *a = getenv("SM64_CRACKLEAN"); gfx_soft_cracklean = !(a && a[0] == '0'); }
            int *const rv[] = { &gfx_soft_fastrows, &gfx_soft_spanvec, &gfx_soft_shadepx, &gfx_soft_zskip, &gfx_soft_neonnoto, &gfx_soft_wraprr,
                                &gfx_soft_skyclr, &gfx_soft_lean, &gfx_soft_fullst };
            static const char *const re[] = { "SM64_FASTROWS", "SM64_SPANVEC", "SM64_SHADEPX", "SM64_ZSKIP", "SM64_NEONNOTO", "SM64_WRAPRR",
                                              "SM64_SKYCLR", "SM64_LEAN", "SM64_FULLST" };
            for (int i = 0; i < (int) (sizeof(rv) / sizeof(rv[0])); i++) if (*rv[i] < 0) { const char *a = getenv(re[i]); *rv[i] = !(a && a[0] == '0'); }
            for (int i = 0; i < BENCH_N; i++) sBenchOn[i] = *sBenchSw[i].var;
        }
    }
}
int port_bench_switch_count(void) { return BENCH_N; }
int port_bench_round_count(void) { return BENCH_N - BENCH_ROUND0; }
const char *port_bench_round_name(int i) { return i >= 0 && BENCH_ROUND0 + i < BENCH_N ? sBenchSw[BENCH_ROUND0 + i].name : "all-on"; }
/* this round's switches in mask off (bit i: sBenchSw[BENCH_ROUND0 + i]), every other switch at its own value */
void port_bench_round_off(unsigned mask) {
    port_bench_switches_init();
    for (int k = 0; k < BENCH_N; k++) *sBenchSw[k].var = sBenchOn[k];
    for (int i = 0; BENCH_ROUND0 + i < BENCH_N; i++) if (mask & (1u << i)) *sBenchSw[BENCH_ROUND0 + i].var = sBenchSw[BENCH_ROUND0 + i].off;
}
const char *port_bench_switch_name(int i) { return i >= 0 && i < BENCH_N ? sBenchSw[i].name : "all-on"; }
/* -1: every switch at its own value; else switch i off, the others at theirs */
void port_bench_switch_off(int i) {
    port_bench_switches_init();
    for (int k = 0; k < BENCH_N; k++) *sBenchSw[k].var = sBenchOn[k];
    if (i >= 0 && i < BENCH_N) *sBenchSw[i].var = sBenchSw[i].off;
}
static void bench_window_end(void) {
    if (!bench_on()) return;
    sBenchWindow++;
    for (int i = 0; i < BENCH_N; i++) *sBenchSw[i].var = sBenchOn[i];
    if (sBenchWindow & 1) {
        const int i = (int) ((sBenchWindow >> 1) % BENCH_N);
        *sBenchSw[i].var = sBenchSw[i].off;
        sBenchName = sBenchSw[i].name;
    } else {
        sBenchName = "all-on";
    }
}

/* PMU accumulators, summed per rendered iteration in run_one_iteration. */
uint64_t gPmuCyc SAVESTATE_EXCLUDE, gPmuIns SAVESTATE_EXCLUDE, gPmuMiss SAVESTATE_EXCLUDE;

/* port_log() / port_log_always() are provided by src/port/segments.c. */
extern void port_log_always(const char *fmt, ...);

/* ------------------------------------------------------------ save paths --- */
/* Everything the port writes (save, config, log, debug dumps) goes under a
 * writable data dir: $SM64_DATA if set, else the executable's dir + /data
 * (desktop), which on the handheld resolves under the OS's home folder. */
static char sEbootDir[256] = "./";
static char sSaveDir[300]  = "./data/";

/* The test builds (2026-10-06): the executables named sm64_test and
 * sm64_bench keep everything in the test data folder, as GoldenEye's test builds use .goldeneye-test: their
 * own data, eeprom (a 120-star save on the first start, hh_eeprom.c), states and logs. The player's handheld/.sm64
 * is never opened. SM64_TESTMODE=1 does the same on the desktop (the save part; the dir is SM64_DATA there). */
static int prog_is(const char *prefix) {
    if (!sProgName) return 0;
    const char *b = strrchr(sProgName, '/');
    return strncmp(b ? b + 1 : sProgName, prefix, strlen(prefix)) == 0;
}
int port_prog_is(const char *prefix) { return prog_is(prefix); }
int port_test_mode(void) {
    static int sT SAVESTATE_EXCLUDE = -1;
    if (sT < 0) {
        const char *e = getenv("SM64_TESTMODE");
        sT = (e && e[0]) ? e[0] == '1' : (prog_is("sm64_test") || prog_is("sm64_bench"));
    }
    return sT;
}

const char *port_eboot_dir(void) { return sEbootDir; }
const char *port_save_dir(void)  { return sSaveDir; }

const char *port_save_path(const char *name) {
    static char buf[4][512];
    static int i;
    char *b = buf[i++ & 3];
    snprintf(b, sizeof(buf[0]), "%s%s", sSaveDir, name);
    return b;
}

void port_fs_mkdir(const char *path) { mkdir(path, 0777); }

void port_set_save_dir(const char *argv0) {
    const char *env = getenv("SM64_DATA");
    if (env && env[0]) {
        snprintf(sEbootDir, sizeof(sEbootDir), "%s", env);
        size_t n = strlen(sEbootDir);
        if (n && sEbootDir[n - 1] != '/' && n + 1 < sizeof(sEbootDir)) { sEbootDir[n] = '/'; sEbootDir[n + 1] = 0; }
    } else {
        const char *slash = argv0 ? strrchr(argv0, '/') : NULL;
        if (slash && (size_t)(slash + 1 - argv0) < sizeof(sEbootDir)) {
            memcpy(sEbootDir, argv0, slash + 1 - argv0);
            sEbootDir[slash + 1 - argv0] = 0;
        } else {
            snprintf(sEbootDir, sizeof(sEbootDir), "./");
        }
    }
    char dir[300];
    mkdir(sEbootDir, 0777); /* a fresh SM64_DATA (the desktop runs): its data/ needs it */
    snprintf(dir, sizeof(dir), "%sdata", sEbootDir);
    mkdir(dir, 0777);
    snprintf(sSaveDir, sizeof(sSaveDir), "%s/", dir);
}

void port_fs_init(void) { /* data/ created in port_set_save_dir(); nothing else. */ }

/* ---------------------------------------------------------- debug stubs --- */
void port_screenshot(int index)          { (void)index; }
void port_depthshot(int index)           { (void)index; }
void port_debug_selftest(void)           { }
void port_debug_frame_begin(u32 frame)   { (void)frame; }
void port_debug_frame_end(u32 frame)     { (void)frame; }
void port_gfx_overlay(void)              { } /* FPS overlay: off by default */

/* Referenced by gfx_pc.c's gfx_sp_reset (was defined in the PSP main). */
float identity_matrix[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};

/* --------------------------------------------------------- frame driving --- */
void port_gfx_run(Gfx *dl)        { gfx_run(dl); }
void port_gfx_start_frame(void)   { gfx_start_frame(); }
void port_gfx_end_frame(void)     { gfx_end_frame(); }

static u32 sFrame SAVESTATE_EXCLUDE; /* port-side frame counter: restoring it re-fired the SM64_LOAD_AT hook in a loop */
/* Headless save/load verification: SM64_SAVE_AT / SM64_LOAD_AT frame numbers
 * queue a slot-1 save/load, so a scripted run can save mid-race, keep playing,
 * load, and confirm the game rewound to the saved state. */
static void savestate_test_hook(long frame) {
    static long save_at SAVESTATE_EXCLUDE = -2, load_at SAVESTATE_EXCLUDE = -2;
    if (save_at == -2) {
        const char *s = getenv("SM64_SAVE_AT"); save_at = s ? atol(s) : -1;
        const char *l = getenv("SM64_LOAD_AT"); load_at = l ? atol(l) : -1;
    }
    if (frame == save_at) savestate_request_save(1);
    if (frame == load_at) savestate_request_load(1);
    { /* SM64_EXITSAVE_AT=<frame>: EXIT GAME from the menu (quicksave, then quit). */
        static long exitsave_at SAVESTATE_EXCLUDE = -2;
        if (exitsave_at == -2) { const char *e = getenv("SM64_EXITSAVE_AT"); exitsave_at = e ? atol(e) : -1; }
        if (frame == exitsave_at) savestate_request_exit_save();
    }
    { /* SM64_ASSETDUMP_AT=<f1>,<f2>,...: write the asset region to
       * data/assetdump_<frame>.bin -- debug, finds runtime writes into ROM data. */
        static char spec[160] SAVESTATE_EXCLUDE; static int init SAVESTATE_EXCLUDE;
        if (!init) { const char *e = getenv("SM64_ASSETDUMP_AT"); if (e) snprintf(spec, sizeof(spec), ",%s,", e); init = 1; }
        if (spec[0]) {
            char key[24]; snprintf(key, sizeof(key), ",%ld,", frame);
            if (strstr(spec, key)) {
                extern char __assets_start[], __assets_end[];
                char name[48]; snprintf(name, sizeof(name), "assetdump_%ld.bin", frame);
                FILE *f = fopen(port_save_path(name), "wb");
                if (f) { fwrite(__assets_start, 1, (size_t) (__assets_end - __assets_start), f); fclose(f); }
            }
        }
    }
    { /* SM64_EXIT_AT=<frame>: clean exit (gprof needs exit(), not a kill). */
        static long exit_at SAVESTATE_EXCLUDE = -2;
        if (exit_at == -2) { const char *f = getenv("SM64_EXIT_AT"); exit_at = f ? atol(f) : -1; }
        if (frame == exit_at) exit(0);
    }
}

static void run_one_iteration(void) {
    extern u32 gfx_soft_frames_rendered;
    extern void pmu_read(uint64_t out[3]);
    extern int pmu_ok(void);
    uint64_t p0[3] = {0,0,0};
    const u32 t0 = port_time_us();
    const u32 rendered0 = gfx_soft_frames_rendered;
    if (pmu_ok()) pmu_read(p0);
    port_debug_frame_begin(sFrame);
    { /* one SM64 frame: pacing and frameskip decide in gfx_start_frame (the window
       * backend), the display list runs inside the game loop (exec_display_list) */
        u32 t = port_time_us();
        gfx_start_frame();
        const u32 g0 = gPortProfFrame[PROF_GFX];
        const u32 tl = port_time_us();
        gPortProfFrame[PROF_GFX] += tl - t;
        game_loop_one_iteration();
        const u32 ta = port_time_us();
        const u32 gfx_in_loop = gPortProfFrame[PROF_GFX] - g0 - (tl - t);
        PORT_PROF_ADD(PROF_LOGIC, (ta - tl) - gfx_in_loop);
#if defined(MIYOO)
        if (sAudioThisFrame) { /* SM64_AUDIOJOB: made on the worker; done before anything else touches it */
            extern void gfx_miyoo_job_fence(void);
            sAudioThisFrame = 0;
            if (__atomic_load_n(&sAudioJobOut, __ATOMIC_ACQUIRE)) gfx_miyoo_job_fence();
        } else
#endif
        {
        port_audio_mix_lock();
        port_sm64_audio_frame();
        port_audio_mix_unlock();
        }
        t = port_time_us();
        PORT_PROF_ADD(PROF_AUDIO, t - ta);
        gfx_end_frame();
        PORT_PROF_ADD(PROF_GFX, port_time_us() - t);
    }
    savestate_test_hook(sFrame);
    { const u32 ts = port_time_us();
      savestate_check(); /* apply any queued save/load at the iteration boundary */
      PORT_PROF_ADD(PROF_SLEEP, port_time_us() - ts); }
    port_debug_frame_end(sFrame);
    const int did_render = gfx_soft_frames_rendered != rendered0;
    if (pmu_ok() && did_render) { /* only rendered frames feed the raster PMU stats */
        uint64_t p1[3]; pmu_read(p1);
        gPmuCyc += p1[0] - p0[0]; gPmuIns += p1[1] - p0[1]; gPmuMiss += p1[2] - p0[2];
    }
    prof_iteration_end(port_time_us() - t0, did_render);
    sFrame++;
}

#if defined(MIYOO)
/* Miyoo Mini: the game's folder (launch.sh sets SM64_DATA to App/SuperMario64; else the executable's own
 * folder) holds the settings, saves, save states, assets.bin and the logs, all in its data/ (port_set_save_dir).
 * The console log is data/console.log, rotated like the handheld's. SM64_LOG=0 leaves stdout/stderr alone. */
static void miyoo_console_setup(void) {
    char dir[256], d[300], a[320], b[320];
    const char *env = getenv("SM64_DATA");
    if (env && env[0]) snprintf(dir, sizeof(dir), "%s", env);
    else {
        const ssize_t n = readlink("/proc/self/exe", dir, sizeof(dir) - 1);
        char *slash;
        dir[n > 0 ? n : 0] = 0;
        slash = strrchr(dir, '/');
        if (n <= 0 || !slash) snprintf(dir, sizeof(dir), ".");
        else *slash = 0;
        setenv("SM64_DATA", dir, 1);
    }
    snprintf(d, sizeof(d), "%s/data", dir);
    mkdir(dir, 0777); mkdir(d, 0777);
    const char *lg = getenv("SM64_LOG");
    if (lg && lg[0] == '0') return;
    snprintf(a, sizeof(a), "%s/console.1.log", d); snprintf(b, sizeof(b), "%s/console.2.log", d); rename(a, b);
    snprintf(b, sizeof(b), "%s/console.log", d); rename(b, a);
    const int fd = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
}
#endif

/* SM64_CODELOCK (default 1; 0 = as before; 2026-10-06, the wipe stall). The game's code is read from
 * the SD card: a page of code is read from the card the first time
 * it runs (a 128 KB block per miss), and the kernel may drop clean pages again later. A level's first load runs
 * code nothing ran before (its objects' behaviours, its geo functions): on the device that frame's game logic
 * took 100-185 ms, while its instructions are ~1.3M under qemu (about 3 ms). So the file-backed part of the
 * executable and of its libraries (code, read-only data, initialised data: ~2-3 MB) is read in and locked in memory once, at start-up,
 * on the black screen before the title. No instruction of the game changes: only when the reads happen. The log
 * says how long it took and how many pages came from the card. */
#include <sys/mman.h>
#include <sys/resource.h>
#include <link.h>
static int code_lock_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    size_t *locked = (size_t *)data;
    /* the executable and the libraries it loaded (SDL, libc: on the OS's own squashfs) */
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || ph->p_filesz == 0) continue;
        const uintptr_t a = (uintptr_t)(info->dlpi_addr + ph->p_vaddr) & ~(uintptr_t)4095;
        const uintptr_t e = ((uintptr_t)(info->dlpi_addr + ph->p_vaddr + ph->p_filesz) + 4095) & ~(uintptr_t)4095;
        if (mlock((void *)a, e - a) == 0) locked[0] += e - a;
        else { locked[1]++; locked[2] = (size_t)errno; }
    }
    return 0;
}
static void port_code_lock(void) {
    const char *e = getenv("SM64_CODELOCK");
    if (e && e[0] == '0') { port_log_always("CODELOCK off (SM64_CODELOCK=0)\n"); return; }
    struct rusage r0, r1;
    getrusage(RUSAGE_SELF, &r0);
    const u32 t = port_time_us();
    size_t locked[3] = { 0, 0, 0 }; /* bytes locked, segments refused, the last refusal's errno */
    { struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY }; (void)setrlimit(RLIMIT_MEMLOCK, &rl); } /* root on the handheld: no cap */
    dl_iterate_phdr(code_lock_cb, locked);
    getrusage(RUSAGE_SELF, &r1);
    port_log_always("CODELOCK %u KB locked in %u ms (%ld pages read in, %ld already in memory); %u segments refused (errno %u)\n",
                    (unsigned)(locked[0] >> 10), (port_time_us() - t) / 1000, r1.ru_majflt - r0.ru_majflt,
                    r1.ru_minflt - r0.ru_minflt, (unsigned)locked[1], (unsigned)locked[2]);
}

int main(int argc, char **argv) {
    sProgName = argc > 0 ? argv[0] : NULL;
#if defined(MIYOO)
    miyoo_console_setup(); /* the console log first, so the priority line lands in it */
#endif
    { /* Kit k1.6: nice -10 (SM64_PRIO=<n>, 0 = off) before any thread or lasting child, so all inherit
       * it: an OS helper script forks every ~2 s and otherwise steals a frame. Here, after the
       * console setup (its "pid record" child has already exited), so the line lands in console.log. */
        extern void hh_game_priority(void);
        hh_game_priority();
    }
    { /* SM64_HEAPSHIFT=<bytes>: debug -- start the heap somewhere else, as the
       * device's address randomisation does every boot, so the desktop can
       * catch a heap pointer leaking into a save state. */
        const char *hs = getenv("SM64_HEAPSHIFT");
        if (hs && atol(hs) > 0) { volatile char *p = malloc((size_t) atol(hs)); if (p) p[0] = 1; }
    }
    setvbuf(stdout, NULL, _IONBF, 0); /* unbuffered: progress is visible up to any crash */
    setvbuf(stderr, NULL, _IONBF, 0);
    port_set_save_dir(argc > 0 ? argv[0] : NULL);
#if defined(MIYOO)
    {   /* The CPU clock (kit miyoo_clock.c): 1500 MHz on the Mini, 1800 on the Mini Plus by default, cpuclock=<MHz> in
         * data/sm64_display.cfg (0: as the OS set it), SM64_CPUCLOCK=<MHz> wins. Put back at every way out (the exit
         * below, atexit, the crash handler). The clock goes in the log either way. */
        extern int gfx_miyoo_cfg(const char *key, int def);
        if (!miyoo_clock_boost_to(gfx_miyoo_cfg("cpuclock=", -1))) { /* left alone: what cpufreq says (never /dev/mem off a Mini) */
            FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
            long khz = 0;
            if (f) { if (fscanf(f, "%ld", &khz) != 1) khz = 0; fclose(f); }
            if (khz > 0) port_log_always("clock: %ld MHz (cpufreq)\n", khz / 1000);
        }
    }
#endif
    savestate_parse_args(argc, argv); /* a relaunch: --loadStateFile <path> */
    { extern void port_clock_pick(void); port_clock_pick(); } /* SM64_VDSOCLOCK: before any other thread reads the clock */
    /* The game's data is not in the executable: the asset region is filled
     * from the cache, or on the first start from the player's ROM (M3). */
    { extern void port_assets_load(const char *argv0); port_assets_load(argc > 0 ? argv[0] : NULL); }
    if (getenv("SM64_ASSETS_ONLY")) return 0; /* build/verify the asset cache and stop (sweep prewarm) */
    port_code_lock(); /* SM64_CODELOCK: the code read in from the card now, not at a level's first load */
    port_fs_init();
    port_audio_out_init();
    PORT_LOG("boot\n");

    /* The game's own memory pool, a static array as on the N64 (not
     * USE_SYSTEM_MALLOC): its address is fixed by the link, so game state never
     * holds a heap pointer and a save state loads in any later session. */
    static u64 sPool[0x165000 / 8 / 4 * sizeof(void *)];
    main_pool_init(sPool, sPool + sizeof(sPool) / sizeof(sPool[0]));
    gEffectsMemoryPool = mem_pool_init(0x4000, MEMORY_POOL_LEFT);

    { extern void port_assets_finish(void); port_assets_finish(); } /* the first-start screen: "Done" after the file system, audio device and pools, then black (SDL stays up) */
    gfx_init(&gfx_sdl, &gfx_soft_api, "Super Mario 64", false);
    { extern int port_bench2_on(void); if (!port_bench2_on()) savestate_boot_prompt(); } /* resume an auto save, or start new (handheld); the level bench neither asks nor removes it */
    port_debug_selftest();

    { extern int port_debug_level(void); if (port_debug_level() > 0) gDebugLevelSelect = TRUE; } /* SM64_LEVEL */
    audio_init();
    sound_init();
    thread5_game_loop(NULL); /* the game's setup; the loop body is game_loop_one_iteration */
    sGameInited = 1;
    PORT_LOG("game init done\n");
    { extern void port_audio_backstop_enable(int on); port_audio_backstop_enable(1); }
    { extern void pmu_init(void); pmu_init(); } /* Cortex-A7 counters (compute vs memory bound) */

    while (sRunning) {
        run_one_iteration();
    }
    savestate_shutdown();
    port_log_always("SESSION END (clean exit)\n");
#if defined(MIYOO)
    miyoo_clock_restore(); /* the clock the OS had, before anything that could end the process */
#endif
    fflush(NULL);
    sync();
#if defined(HANDHELD)
    { /* SDL down before the launcher comes back (X50, gfx_sdl12.c); the line after it says the shutdown finished
       * rather than hung. */
        extern int gfx_wm_shutdown(void);
        port_log_always(gfx_wm_shutdown() ? "exit: SDL shut down\n" : "exit: SDL left up (a flip still running)\n");
    }
#endif
    return 0;
}
