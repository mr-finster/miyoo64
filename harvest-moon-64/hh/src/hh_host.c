/* Harvest Moon 64 on the Miyoo Mini: the host side's main. System headers only.
 *
 * The N64's RAM (0x80000000-0x807FFFFF) is mapped at its own address: the game keeps its sprite, map, text and
 * bytecode buffers at fixed N64 addresses (src/buffers, buffers.h) and its bytecode holds N64 addresses, so the
 * native executable is linked at 0x40000000, clear of it. The data pack (hm64-pack.bin, tools/setup.sh: the ROM with
 * the bytecode and text re-assembled for ARM, hh/tools/hm64_pack.py) stands in for the cartridge: nuPiReadRom copies
 * from it. One thread runs the game; a retrace (hh_host_retrace) is paced to 60 Hz.
 *
 *   HM64_PACK=<file>      the data pack (default: data/hm64-pack.bin next to the executable)
 *   HM64_DATA=<dir>       the save folder (the battery save, the log)
 *   HM64_EXIT_AT=<n>      stop after n retraces (tests)
 *   HM64_NOPACE=1         no pacing (tests under qemu)
 *   HM64_HZ=<n>           game steps a second (default 20). The port runs one main-loop step and one drawn frame per
 *                         retrace call; the N64 (angrylion reference of the intro) advances about 3x slower than 60/s,
 *                         its main loop taking about three retraces a step, so the device paces a step to 1/20 s.
 *   HM64_KEYS=<script>    a pad script "<frame>:<key>+|-,..." by drawn frame (port_input.c)
 *   HM64_DUMP=<dir>       each drawn frame as PPM, HM64_DUMP_EVERY=<n> every nth (hh_gfx.c)
 *   HM64_WATCHDOG=<s>     the stall report after s seconds without a retrace (default 5; 0 = off; hh_watch.c)
 * Test hooks (all by retrace, applied at mainLoop's wait between two steps): HM64_POKE, HM64_SAVE_AT, HM64_LOAD_AT,
 * HM64_EXITSAVE_AT, HM64_STALL_AT (below). */
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include "port.h"
#include "savestate.h"
#include "debug_cfg.h"
#include "../game/hh_port.h"
#if defined(MIYOO)
#include "miyoo_panel.h"
#endif

#define N64_RAM_BASE 0x80000000u
#define N64_RAM_SIZE 0x00800000u

int port_quit;
uint32_t port_frame;          /* frames drawn (a swap task) */
uint32_t hh_retraces;         /* retraces run */
volatile int port_loop_stage;
uint32_t port_perf_gfx_us, port_perf_audio_us;

/* ---------------------------------------------------------------- the clock and the log */
uint32_t port_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) ((uint64_t) ts.tv_sec * 1000000u + (uint64_t) (ts.tv_nsec / 1000));
}

static char sSaveDir[512];
const char *port_save_dir(void) {
    if (!sSaveDir[0]) {
        const char *e = getenv("HM64_DATA");
#if defined(MIYOO)
        const char *d = e && e[0] ? e : miyoo_data_dir();
#else
        const char *d = e && e[0] ? e : "./";
#endif
        snprintf(sSaveDir, sizeof sSaveDir, "%s%s", d, d[strlen(d) - 1] == '/' ? "" : "/");
        mkdir(sSaveDir, 0755);
    }
    return sSaveDir;
}
const char *port_save_path(const char *name) {
    static char path[4][640];
    static int k;
    k = (k + 1) & 3;
    snprintf(path[k], sizeof path[k], "%s%s", port_save_dir(), name);
    return path[k];
}

static FILE *sLogFile;
static int sLogDup;     /* console.log is stderr (see vlog) */
static int sLogFd = -1; /* console.log's descriptor, for the crash line (write() only) */
static void vlog(const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    vfprintf(stderr, fmt, ap);
    if (!sLogFile && !sLogDup) {
        /* console.log; the two sessions before it stay as console.1.log and console.2.log (as Paper Mario's) */
        char a[700], b[700];
        snprintf(a, sizeof a, "%s", port_save_path("console.1.log"));
        snprintf(b, sizeof b, "%s", port_save_path("console.2.log"));
        rename(a, b);
        snprintf(b, sizeof b, "%s", port_save_path("console.log"));
        rename(b, a);
        sLogFile = fopen(b, "w");
        if (sLogFile) { setvbuf(sLogFile, NULL, _IOLBF, 0); sLogFd = fileno(sLogFile); }
        /* stderr goes to console.log too, so the kit's own lines -- the panel, the worker, the clock, the save states,
         * the sleep saves, the menu -- are in the log; HM64_LOG=0 keeps stderr as it is (a terminal). Then this
         * function writes stderr only. */
        if (sLogFile && !(getenv("HM64_LOG") && getenv("HM64_LOG")[0] == '0')) {
            fflush(stderr);
            if (dup2(sLogFd, 2) >= 0) {
                setvbuf(stderr, NULL, _IOLBF, 0);
                sLogFd = 2;
                fclose(sLogFile);
                sLogFile = NULL;
                sLogDup = 1;
                vfprintf(stderr, fmt, ap2); /* this first line went to the old stderr only */
            }
        }
    }
    if (sLogFile) vfprintf(sLogFile, fmt, ap2);
    va_end(ap2);
}
void port_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}
void port_log_always(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}
void hh_log_game(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}
int handheld_log_fd(void) { return sLogFd; }
unsigned handheld_loop_detail(char *buf, size_t n) {
    if (n > 0) snprintf(buf, n, "retrace %u, frame %u", (unsigned) hh_retraces, (unsigned) port_frame);
    return hh_retraces;
}

/* ---------------------------------------------------------------- the crash line */
static void crash_handler(int sig, siginfo_t *si, void *uc) {
    char buf[200];
    int n = snprintf(buf, sizeof buf, "FATAL: signal %d at retrace %u frame %u", sig, (unsigned) hh_retraces,
                     (unsigned) port_frame);
#if defined(__arm__)
    const mcontext_t *m = &((ucontext_t *) uc)->uc_mcontext;
    n += snprintf(buf + n, sizeof buf - n, ", pc %08lx lr %08lx sp %08lx r0 %08lx r1 %08lx r2 %08lx r3 %08lx",
                  (unsigned long) m->arm_pc, (unsigned long) m->arm_lr, (unsigned long) m->arm_sp,
                  (unsigned long) m->arm_r0, (unsigned long) m->arm_r1, (unsigned long) m->arm_r2,
                  (unsigned long) m->arm_r3);
#else
    (void) uc;
#endif
    if (si && si->si_code > 0) n += snprintf(buf + n, sizeof buf - n, " addr %p", si->si_addr);
    buf[n++] = '\n';
    (void) !write(2, buf, (size_t) n);
    if (sLogFd > 2) (void) !write(sLogFd, buf, (size_t) n);
#ifdef PM64_SDL12
    port_video_crash();
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ---------------------------------------------------------------- the N64's RAM and the data pack */
static uint8_t *sPack;
static uint32_t sPackSize;

static void map_n64_ram(void) {
    /* MAP_FIXED_NOREPLACE: never over something already mapped there (a library, the heap); a kernel older than 4.17
     * takes the address as a hint and answers elsewhere if it is taken, which is refused the same way. The save
     * states hold this RAM and the game's pointers into it: it must sit at the same address in every run. */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
    void *p = mmap((void *) (uintptr_t) N64_RAM_BASE, N64_RAM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *) (uintptr_t) N64_RAM_BASE) {
        port_log("ram: cannot map the N64 RAM at 0x%08X: %s (got %p)\n", N64_RAM_BASE, strerror(errno), p);
        if (p != MAP_FAILED) munmap(p, N64_RAM_SIZE);
        exit(1);
    }
    port_log("ram: N64 RAM mapped at 0x%08X (%u KB)\n", N64_RAM_BASE, N64_RAM_SIZE / 1024);
}

/* the save states take the N64's RAM with the game's .data/.bss (the kit's savestate.c, hh/gamestate.ld) */
int hh_game_extra_range(char **base, size_t *size) {
    *base = (char *) (uintptr_t) N64_RAM_BASE;
    *size = N64_RAM_SIZE;
    return 1;
}

static int load_pack(const char *argv0) {
    char path[600];
    const char *e = getenv("HM64_PACK");
    if (e && e[0]) snprintf(path, sizeof path, "%s", e);
    else snprintf(path, sizeof path, "%s", port_save_path("hm64-pack.bin"));
    (void) argv0;
    FILE *f = fopen(path, "rb");
    if (!f) { port_log("pack: %s: %s (the first start builds it from your ROM)\n", path, strerror(errno)); return 0; }
    fseek(f, 0, SEEK_END);
    sPackSize = (uint32_t) ftell(f);
    fseek(f, 0, SEEK_SET);
    sPack = malloc(sPackSize);
    if (!sPack || fread(sPack, 1, sPackSize, f) != sPackSize) { port_log("pack: cannot read %s\n", path); fclose(f); return 0; }
    fclose(f);
    port_log("pack: %s, %u KB\n", path, sPackSize / 1024);
    return 1;
}

void hh_rom_read(unsigned int rom_addr, void *dst, unsigned int size) {
    static int warned;
    rom_addr &= 0x0FFFFFFFu;
    if ((uint64_t) rom_addr + size > sPackSize) {
        if (warned++ < 20)
            port_log("rom: read 0x%08X + 0x%X past the pack's end (from %p)\n", rom_addr, size, __builtin_return_address(0));
        if (rom_addr >= sPackSize) return;
        size = sPackSize - rom_addr;
    }
    memcpy(dst, sPack + rom_addr, size);
}

/* ---------------------------------------------------------------- the battery save (SRAM, 32 KB) */
#define SRAM_SIZE 0x8000
static uint8_t sSram[SRAM_SIZE];
static int sSramLoaded;
static void sram_load(void) {
    if (sSramLoaded) return;
    sSramLoaded = 1;
    FILE *f = fopen(port_save_path("hm64.sram"), "rb");
    if (!f) return;
    (void) !fread(sSram, 1, SRAM_SIZE, f);
    fclose(f);
}
void hh_sram_read(unsigned int dev_addr, void *dst, unsigned int size) {
    const uint32_t off = (dev_addr - 0x08000000u) & (SRAM_SIZE - 1);
    sram_load();
    if (off + size > SRAM_SIZE) size = SRAM_SIZE - off;
    memcpy(dst, sSram + off, size);
}
void hh_sram_write(unsigned int dev_addr, const void *src, unsigned int size) {
    const uint32_t off = (dev_addr - 0x08000000u) & (SRAM_SIZE - 1);
    sram_load();
    if (off + size > SRAM_SIZE) size = SRAM_SIZE - off;
    memcpy(sSram + off, src, size);
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp", port_save_path("hm64.sram"));
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fwrite(sSram, 1, SRAM_SIZE, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename(tmp, port_save_path("hm64.sram"));
}

/* ---------------------------------------------------------------- the retrace */
static int sDisplayOn;
void hh_display_on(int on) { sDisplayOn = on; }

void port_state_stage(void) {}
void port_request_exit(void);

static void host_exit(void) {
    port_log("exit: %u retraces, %u frames drawn\n", (unsigned) hh_retraces, (unsigned) port_frame);
    savestate_shutdown();
#ifdef PM64_SDL12
    port_video_shutdown();
#endif
    exit(0);
}

/* PERF every 600 retraces: the game's work per retrace and the drawing's share */
static uint32_t sPerfT0, sPerfWork, sPerfGfx0, sPerfAud0, sPerfN;
extern uint32_t port_audio_out_underruns;
static uint32_t sNext;

/* ---------------------------------------------------------------- sound */
/* the game side's audio manager (decomp.patch nuaustlmgr.c): one frame of samples, a multiple of 184 */
int hh_au_render(int16_t *out, int samples);
void port_audio_out_advance_us(uint32_t us);
#define AU_FRAME (184 * 4) /* 23 ms at 32 kHz */
#define AU_MIN (184 * 3)
static void audio_pump(uint32_t step_us) {
    static int16_t buf[AU_FRAME * 2 + 64];
    const uint32_t t0 = port_time_us();
    port_audio_out_advance_us(step_us);
    port_audio_out_mark();
    for (int guard = 0; guard < 8; guard++) {
        const int32_t want = port_audio_out_want(AU_FRAME, AU_MIN);
        if (want <= 0) break;
        const int n = hh_au_render(buf, want);
        if (n <= 0) break;
        port_audio_out_push(buf, n);
    }
    port_perf_audio_us += port_time_us() - t0;
}

/* Whether <n> is in the env var's comma list ("120,900,4000") */
static int at_list(const char *name, uint32_t n) { /* (by retrace) */
    const char *e = getenv(name);
    while (e && *e) {
        char *end;
        const unsigned long v = strtoul(e, &end, 10);
        if (end == e) break;
        if (v == n) return 1;
        e = *end == ',' ? end + 1 : end;
    }
    return 0;
}

/* The test hooks' clock: the retraces, or (HM64_CLOCK=rand, the release review against the N64) the game's rand() calls
 * (lib/libkmc rand.c, decomp.patch), which the N64 reference counts too: one clock for both sides however many retraces a
 * step takes there. Events fire at the first wait at or past their time. */
extern unsigned int hh_rand_calls;
uint32_t hh_test_clock(void) {
    static int sRand = -1;
    if (sRand < 0) { const char *e = getenv("HM64_CLOCK"); sRand = e && !strcmp(e, "rand"); }
    return sRand ? hh_rand_calls : hh_retraces;
}

/* HM64_POKE=<retrace>:<addr hex>:<bytes 1|2|4>:<value hex>[:or][,...] (tests, the release review): a word of game memory
 * written at mainLoop's wait of that retrace, in the order given (retraces ascending). The address is the native one
 * (the executable's symbol, or the N64 RAM at its own address); the N64 reference gets the same variable poked at
 * its N64 address. */
static void test_pokes(void) {
    static const char *sNext = (const char *) 1;
    static unsigned long sAt;
    static unsigned sAddr, sLen, sVal;
    static int sHave, sOr;
    if (sNext == (const char *) 1) sNext = getenv("HM64_POKE");
    for (;;) {
        while (!sHave && sNext && *sNext) {
            const char *c = strchr(sNext, ',');
            char op[4] = "";
            sHave = sscanf(sNext, "%lu:%x:%u:%x:%3[a-z]", &sAt, &sAddr, &sLen, &sVal, op) >= 4;
            sOr = !strcmp(op, "or"); /* <...>:or sets those bits, keeping the others */
            sNext = c ? c + 1 : NULL;
        }
        if (!sHave || sAt > hh_test_clock()) return;
        sHave = 0;
        const uintptr_t a = sAddr;
        const int game = (a >= 0x40000000u && a < 0x48000000u) || (a >= N64_RAM_BASE && a < N64_RAM_BASE + N64_RAM_SIZE);
        if (!game || (sLen != 1 && sLen != 2 && sLen != 4)) { port_log("poke: %08X (%u) refused\n", sAddr, sLen); continue; }
        if (sLen == 1) *(volatile uint8_t *) a = (uint8_t) (sVal | (sOr ? *(volatile uint8_t *) a : 0));
        else if (sLen == 2) *(volatile uint16_t *) a = (uint16_t) (sVal | (sOr ? *(volatile uint16_t *) a : 0));
        else *(volatile uint32_t *) a = sVal | (sOr ? *(volatile uint32_t *) a : 0);
        port_log("poke: %08X (%u) = %X at retrace %u (clock %u)\n", sAddr, sLen, sVal, (unsigned) hh_retraces, (unsigned) hh_test_clock());
    }
}

/* HM64_PEEK=<every>:<addr hex>:<bytes>[:<addr>:<bytes>...] (tests): those words logged every <every> retraces */
static void test_peek(void) {
    static int sEvery = -1;
    static unsigned sAddr[8], sLen[8];
    static int sN;
    if (sEvery < 0) {
        const char *e = getenv("HM64_PEEK");
        sEvery = 0;
        if (e) {
            char *end;
            sEvery = (int) strtol(e, &end, 10);
            while (*end == ':' && sN < 8) {
                sAddr[sN] = (unsigned) strtoul(end + 1, &end, 16);
                if (*end != ':') break;
                sLen[sN++] = (unsigned) strtoul(end + 1, &end, 10);
            }
        }
    }
    if (sEvery <= 0 || hh_retraces % (uint32_t) sEvery) return;
    char line[200];
    int n = snprintf(line, sizeof line, "peek %u:", (unsigned) hh_retraces);
    for (int i = 0; i < sN; i++) {
        const uintptr_t a = sAddr[i];
        const unsigned v = sLen[i] == 1 ? *(volatile uint8_t *) a : sLen[i] == 2 ? *(volatile uint16_t *) a : *(volatile uint32_t *) a;
        n += snprintf(line + n, sizeof line - n, " %08X=%X", sAddr[i], v);
    }
    port_log("%s\n", line);
}

/* mainLoop's wait between two steps: save states, the test hooks */
uint32_t hh_main_waits; /* mainLoop's waits so far: the game's steps */
static void main_wait(void) {
    if (hh_main_waits++ == 0) port_log("main loop: first step at retrace %u (rand calls %u)\n", (unsigned) hh_retraces, hh_rand_calls);
    test_pokes();
    test_peek();
    if (at_list("HM64_SAVE_AT", hh_retraces)) savestate_request_save(1);
    if (at_list("HM64_LOAD_AT", hh_retraces)) savestate_request_load(1);
    if (at_list("HM64_EXITSAVE_AT", hh_retraces)) savestate_request_exit_save();
    {   /* HM64_STALL_AT=<retrace> (tests of the watchdog): the loop stops there for good */
        static long sStall = -2;
        if (sStall == -2) { const char *e = getenv("HM64_STALL_AT"); sStall = e ? atol(e) : -1; }
        if (sStall >= 0 && hh_retraces >= (uint32_t) sStall) {
            port_loop_stage = PORT_STAGE_SLEEP;
            port_log("HM64_STALL_AT: the game loop stops at retrace %u\n", (unsigned) hh_retraces);
            for (;;) sleep(60);
        }
    }
    port_loop_stage = PORT_STAGE_SAVE;
    savestate_check();
    port_loop_stage = PORT_STAGE_GAME;
}

void hh_watch_beat(void);
void hh_watch_start(void);
void hh_convert_ensure(void);
void hh_host_retrace(int main_wait_now) {
    static uint32_t sExitAt = 0xFFFFFFFFu;
    static int sInit, sNoPace;
    static uint32_t sStepUs = 50000;
    static uint32_t sT0;
    if (!sInit) {
        sInit = 1;
        const char *e = getenv("HM64_EXIT_AT");
        if (e && atoi(e) > 0) sExitAt = (uint32_t) atoi(e);
        e = getenv("HM64_NOPACE");
        sNoPace = e && e[0] == '1';
        e = getenv("HM64_HZ");
        if (e && atoi(e) > 0 && atoi(e) <= 60) sStepUs = 1000000u / (uint32_t) atoi(e);
        sNext = sPerfT0 = port_time_us();
    }
    const uint32_t now0 = port_time_us();
    if (hh_retraces > 0) sPerfWork += now0 - sT0;
    hh_retraces++;
    hh_watch_beat();
    port_loop_stage = PORT_STAGE_INPUT;
    port_input_poll();
    if (main_wait_now) main_wait();
    if (port_quit || hh_retraces >= sExitAt) host_exit();
    port_loop_stage = PORT_STAGE_GAME;
    if (++sPerfN == 600) {
        const uint32_t el = now0 - sPerfT0;
        port_log("PERF step work %.2f ms avg (gfx %.2f, audio %.2f) | %.1f steps/s | %u frames drawn | audio gaps %u\n",
                 sPerfWork / 1000.0 / sPerfN, (port_perf_gfx_us - sPerfGfx0) / 1000.0 / sPerfN,
                 (port_perf_audio_us - sPerfAud0) / 1000.0 / sPerfN, sPerfN * 1e6 / (el ? el : 1), (unsigned) port_frame,
                 (unsigned) port_audio_out_underruns);
        sPerfAud0 = port_perf_audio_us;
        port_audio_out_underruns = 0;
        sPerfN = 0;
        sPerfWork = 0;
        sPerfGfx0 = port_perf_gfx_us;
        sPerfT0 = now0;
    }
    if (!sNoPace) { /* HM64_HZ: sleep the part over 12 ms (the Mini's sleeps last a kernel tick), yield the rest */
        port_loop_stage = PORT_STAGE_SLEEP;
        sNext += sStepUs;
        for (;;) {
            const int32_t ahead = (int32_t) (sNext - port_time_us());
            if (ahead <= 0) { if (ahead < -100000) sNext = port_time_us(); break; }
            if (ahead > 12000) usleep((useconds_t) (ahead - 12000));
            else sched_yield();
        }
        port_loop_stage = PORT_STAGE_GAME;
    }
    audio_pump(sStepUs);
    sT0 = port_time_us();
}

void hh_input_read(unsigned short *button, signed char *stick_x, signed char *stick_y) {
    uint16_t b;
    int8_t x, y;
    port_input_read(&b, &x, &y);
    *button = b;
    *stick_x = x;
    *stick_y = y;
}

static void port_priority(void) {
    const char *e = getenv("HM64_PRIO");
    const int want = e ? atoi(e) : -10;
    if (want != 0) (void) setpriority(PRIO_PROCESS, 0, want);
}

int main(int argc, char **argv) {
    extern const char hm64_version[], hm64_release[];
    extern const uint32_t port_build_stamp;
    /* the first line of console.log: SESSION <game> <version> build <id> (the id a save state is tied to) */
    port_log("SESSION hm64 %s-%s build %08X\n", hm64_release, hm64_version, (unsigned) port_build_stamp);
    port_log("Harvest Moon 64 for the Miyoo Mini, %s, version %s\n", hm64_release, hm64_version);
    debug_cfg_load(port_save_path("debug.cfg"), port_log);
    port_priority();
    savestate_set_progname(argv[0]);
    savestate_parse_args(argc, argv);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    map_n64_ram();
#ifdef PM64_SDL12
    port_video_init();
#endif
    hh_watch_start();     /* the stall report (hh_watch.c) */
    hh_convert_ensure();  /* first start: the data pack from the player's ROM (hh_convert.c) */
    if (!load_pack(argv[0])) {
#ifdef PM64_SDL12
        port_video_shutdown();
#endif
        return 1;
    }
    savestate_boot_prompt(); /* an auto save on the card: RESUME GAME / NEW GAME (the kit's savestate.c) */
    port_audio_out_init(32000);
    hh_game_main(); /* never returns: the game's main loop; the way out is host_exit */
    return 0;
}
