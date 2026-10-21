/* The native port's main (M2). Host side: system headers only. It finds the player's ROM, boots the game's code
 * (port_main.c) and runs one game frame (two retraces) per loop.
 *   PM64_ROM=<file>      the ROM (default: "Paper Mario (USA).z64" in the working directory, then the repo root)
 *   PM64_FRAMES=<n>      stop after n game frames (the desktop proxy's test runs)
 *   PM64_SHOTS=<dir>     write each shown frame as a square PPM (port_gfx.c)
 *   PM64_KEYS=<script>   a pad script "<frame>:<key>+|-,..." (port_input.c) */
#define _GNU_SOURCE 1 /* REG_EIP in the crash report */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <errno.h>
#include <signal.h>
#include <ucontext.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#include "port.h"
#include "savestate.h"
#include "debug_cfg.h"
#if defined(MIYOO)
#include "miyoo_panel.h"
#include "miyoo_worker.h"
#endif

int port_quit;
uint32_t port_frame;
uint32_t gPortProfFrame[16];

void port_game_boot(void);
void port_game_retrace(void);

uint32_t port_perf_gfx_us, port_perf_audio_us; /* the PERF line's parts (port_gfx.c, port_audio.c) */
/* PM64_VDSOCLOCK (default 1; 0 = the syscall always, as before; PM64_VDSOCLOCK=0 in debug.cfg too): the kernel's own
 * __vdso_clock_gettime, the same CLOCK_MONOTONIC read in user space, instead of a syscall. The handheld's musl 1.2.1 never
 * looks the vDSO up. GoldenEye's device Bench (goldeneye-007 X37, hh_os.c): 157-167 ns a read against 598-611 ns for
 * the syscall. Taken from it (as mario-kart-64's MK64_VDSOCLOCK): the vDSO is found through AT_SYSINFO_EHDR's symbol table
 * and used only if, best of three trials of 256 reads each, its readings lie between the syscall's around them and cost
 * under half as much; one CLOCK line says which. Only the cost of a read changes, never the clock read. */
typedef int (*PortCgt32)(int, void *);
static PortCgt32 sVdsoCgt SAVESTATE_EXCLUDE;    /* the address is this process's, never a saved one */
static int sClockMode SAVESTATE_EXCLUDE = -1;   /* -1 not picked, 0 syscall, 1 vDSO */

static uint64_t port_clock_us_sys(void) {
#if defined(SYS_clock_gettime32)
    struct { long s, ns; } t; /* musl on the handheld's 4.14 kernel: the time64 syscall is missing */
    if (syscall(SYS_clock_gettime32, CLOCK_MONOTONIC, &t) == 0) return (uint64_t) t.s * 1000000u + (uint64_t) (t.ns / 1000);
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000u + (uint64_t) (ts.tv_nsec / 1000);
}

static uint64_t port_clock_us_vdso(void) {
    struct { long s, ns; } t;
    if (sVdsoCgt(CLOCK_MONOTONIC, &t) == 0) return (uint64_t) t.s * 1000000u + (uint64_t) (t.ns / 1000);
    return port_clock_us_sys();
}

#if defined(__linux__) && __SIZEOF_POINTER__ == 4
#include <elf.h>
#include <sys/auxv.h>
/* A symbol of the vDSO image (32-bit ELF), as musl's __vdsosym; NULL if none. */
static void *port_vdso_sym(const char *want) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *) (uintptr_t) getauxval(AT_SYSINFO_EHDR);
    uintptr_t bias = 0;
    int i, have = 0;
    if (eh == NULL || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS32) return NULL;
    const Elf32_Phdr *ph = (const Elf32_Phdr *) ((const char *) eh + eh->e_phoff);
    for (i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) { bias = (uintptr_t) eh + ph[i].p_offset - ph[i].p_vaddr; have = 1; break; }
    if (!have || eh->e_shoff == 0) return NULL;
    const Elf32_Shdr *sh = (const Elf32_Shdr *) ((const char *) eh + eh->e_shoff);
    for (i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum) continue;
        const Elf32_Sym *sym = (const Elf32_Sym *) ((const char *) eh + sh[i].sh_offset);
        const char *str = (const char *) eh + sh[sh[i].sh_link].sh_offset;
        const unsigned n = sh[i].sh_size / sizeof(Elf32_Sym);
        for (unsigned k = 0; k < n; k++)
            if (ELF32_ST_TYPE(sym[k].st_info) == STT_FUNC && sym[k].st_shndx != SHN_UNDEF && strcmp(str + sym[k].st_name, want) == 0)
                return (void *) (bias + sym[k].st_value);
    }
    return NULL;
}
#else
static void *port_vdso_sym(const char *want) { (void) want; return NULL; }
#endif

/* 256 reads of each, the syscall's around the vDSO's: 0 if the vDSO's readings fall outside them */
static int port_clock_trial(uint64_t *vdso_ns, uint64_t *sys_ns) {
    uint64_t t0, t1, t2, a, b;
    int i;
    t0 = port_clock_us_sys();
    for (i = 0; i < 256; i++) (void) port_clock_us_sys();
    t1 = port_clock_us_sys();
    a = port_clock_us_vdso();
    for (i = 0; i < 256; i++) (void) port_clock_us_vdso();
    b = port_clock_us_vdso();
    t2 = port_clock_us_sys();
    *vdso_ns = (b - a) * 1000u / 257u;
    *sys_ns = (t1 - t0) * 1000u / 257u;
    return a >= t1 && b <= t2;
}

/* Once, from main() after debug.cfg and before any other thread (port_time_us picks on its first read otherwise). */
static void port_clock_pick(void) {
    uint64_t v = ~0ull, s = ~0ull, tv, ts;
    int i, same = 1;
    if (sClockMode >= 0) return;
    const char *e = getenv("PM64_VDSOCLOCK");
    const int on = !(e && e[0] == '0');
    sVdsoCgt = on ? (PortCgt32) port_vdso_sym("__vdso_clock_gettime") : NULL;
    if (sVdsoCgt == NULL) {
        sClockMode = 0;
        port_log("CLOCK syscall (%s)\n", on ? "no vDSO clock_gettime" : "PM64_VDSOCLOCK=0");
        return;
    }
    for (i = 0; i < 3; i++) { /* the best of three (a GoldenEye device run once caught an interrupt inside one) */
        same &= port_clock_trial(&tv, &ts);
        if (tv < v) v = tv;
        if (ts < s) s = ts;
    }
    sClockMode = same && v * 2 < s;
    port_log("CLOCK %s: vDSO %u ns/read vs syscall %u ns/read\n", sClockMode ? "vdso" : "syscall", (unsigned) v, (unsigned) s);
    if (!sClockMode) sVdsoCgt = NULL;
}

uint32_t port_time_us(void) {
    if (sClockMode > 0) return (uint32_t) port_clock_us_vdso();
    if (sClockMode < 0) port_clock_pick();
    return (uint32_t) (sClockMode > 0 ? port_clock_us_vdso() : port_clock_us_sys());
}

/* ---------------------------------------------------------------- the save dir and the log */
/* The Miyoo keeps its files in its data folder (the save, the asset cache, the log, the ROM's path); the desktop
 * proxy in the working directory. PM64_DATA=<dir> overrides both. */
static char sSaveDir[512];
const char *port_save_dir(void) {
    if (!sSaveDir[0]) {
        const char *e = getenv("PM64_DATA");
#if defined(MIYOO)
        const char *d = e && e[0] ? e : miyoo_data_dir(); /* data/ next to the executable (launch.sh sets PM64_DATA to it) */
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
/* the executable's folder (the game folder on the device), with a trailing slash */
const char *port_eboot_dir(void) {
    static char dir[512];
    if (!dir[0]) {
        char self[512];
        const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n > 0) {
            self[n] = 0;
            char *slash = strrchr(self, '/');
            if (slash) { slash[1] = 0; snprintf(dir, sizeof dir, "%s", self); }
        }
        if (!dir[0]) snprintf(dir, sizeof dir, "./");
    }
    return dir;
}

/* stderr and, on the device, console.log in the save dir (read it when the card comes back) */
static FILE *sLogFile;
#if defined(MIYOO)
static int sLogDup; /* console.log is stderr (see vlog) */
#endif
static volatile int sLogFd = -1; /* console.log's descriptor, for the crash line (write() only, crash_handler) */
static void vlog(const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    vfprintf(stderr, fmt, ap);
#ifdef PM64_SDL12
    if (!sLogFile && !sLogDup) {
        /* console.log, as in every port; the two sessions before it stay as console.1.log and
         * console.2.log: a problem survives the next starts (round 3) */
        char a[512], b[512];
        snprintf(a, sizeof a, "%s", port_save_path("console.1.log"));
        snprintf(b, sizeof b, "%s", port_save_path("console.2.log"));
        rename(a, b);
        snprintf(b, sizeof b, "%s", port_save_path("console.log"));
        rename(b, a);
        sLogFile = fopen(b, "w");
        if (sLogFile) { setvbuf(sLogFile, NULL, _IOLBF, 0); sLogFd = fileno(sLogFile); }
#if defined(MIYOO)
        /* The Mini: stderr goes to console.log too (no terminal there), so the kit's own lines -- the panel, the worker,
         * the clock, the save states, the menu -- are in the log; PM64_LOG=0 keeps stderr as it is (a terminal). Then
         * this function writes stderr only. */
        if (sLogFile && !(getenv("PM64_LOG") && getenv("PM64_LOG")[0] == '0')) {
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
#endif
    }
    if (sLogFile) vfprintf(sLogFile, fmt, ap2);
#endif
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

/* Where the main loop is, for a diagnostic log line: how many iterations it has run and in which stage of which game
 * frame it is. Written by the main thread only (port_sdl.c marks the flip), read by other threads. */
volatile int port_loop_stage;
static volatile uint32_t sLoopIter;
#ifdef PM64_SDL12
int handheld_log_fd(void) { return sLogFd; } /* console.log (port_hh.c's weak default: stderr only) */
unsigned handheld_loop_detail(char *buf, size_t n) {
    static const char *const kStage[] = { "boot", "input and menu", "save state check", "game frame", "flip", "game save",
                                          "pacer sleep" };
    const int st = port_loop_stage;
    if (n > 0)
        snprintf(buf, n, "stage %s, game frame %u", kStage[st >= 0 && st <= PORT_STAGE_SLEEP ? st : 0], (unsigned) port_frame);
    return sLoopIter;
}
#endif

/* A crash prints where it happened (addresses for addr2line -f -e build/game/pc/pm64) and the game frame. First one
 * line, the same in every port (diddy-kong-racing cd3649a): "FATAL: signal N at game frame F", on ARM the pc and lr
 * (addr2line -f -e build/game/miyoo/pm64 names them), and for a fault the kernel raised (not abort()) the address.
 * Only write() and arithmetic for it: this runs in signal context, and a crash inside stdio or malloc could leave the
 * log's FILE half way through a line. Then the screen unpanned and the details through port_log, as before. */
static char *crash_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *crash_hex(char *p, unsigned long v) {
    for (int s = (int) sizeof v * 8 - 4; s >= 0; s -= 4) *p++ = "0123456789abcdef"[(v >> s) & 15];
    return p;
}
static char *crash_dec(char *p, unsigned long v) {
    char t[24];
    int n = 0;
    do { t[n++] = (char) ('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}
static void crash_handler(int sig, siginfo_t *si, void *uc) {
    {
        char buf[160], *p = buf;
        p = crash_str(p, "FATAL: signal "); p = crash_dec(p, (unsigned long) sig);
        p = crash_str(p, " at game frame "); p = crash_dec(p, port_frame);
#if defined(__arm__)
        const mcontext_t *m = &((ucontext_t *) uc)->uc_mcontext;
        p = crash_str(p, ", pc "); p = crash_hex(p, m->arm_pc);
        p = crash_str(p, " lr "); p = crash_hex(p, m->arm_lr);
#endif
        if (si && si->si_code > 0) { p = crash_str(p, " addr "); p = crash_hex(p, (unsigned long) (uintptr_t) si->si_addr); }
        *p++ = '\n';
        (void) !write(2, buf, (size_t) (p - buf));
#if defined(MIYOO)
        if (sLogFd >= 0 && sLogFd != 2) (void) !write(sLogFd, buf, (size_t) (p - buf)); /* console.log may be stderr */
#else
        if (sLogFd >= 0) (void) !write(sLogFd, buf, (size_t) (p - buf));
#endif
    }
    (void) uc;
#ifdef PM64_SDL12
    port_video_crash();
#endif
#if defined(__GLIBC__)
    void *bt[48];
    const int n = backtrace(bt, 48);
    for (int i = 0; i < n; i++) port_log("  #%d %p\n", i, bt[i]);
#endif
#if defined(__i386__) && defined(REG_EIP)
    /* a crash inside libc (memcpy on a bad pointer) does not unwind: the registers and the code addresses on the
     * stack name the caller */
    const greg_t *r = ((ucontext_t *) uc)->uc_mcontext.gregs;
    port_log("  eip %08x esp %08x ebp %08x eax %08x ecx %08x edx %08x esi %08x edi %08x\n", (unsigned) r[REG_EIP],
             (unsigned) r[REG_ESP], (unsigned) r[REG_EBP], (unsigned) r[REG_EAX], (unsigned) r[REG_ECX],
             (unsigned) r[REG_EDX], (unsigned) r[REG_ESI], (unsigned) r[REG_EDI]);
    extern char __executable_start[], etext[];
    const uint32_t *sp = (const uint32_t *) r[REG_ESP];
    for (int i = 0, k = 0; i < 256 && k < 12; i++)
        if (sp[i] >= (uint32_t) (uintptr_t) __executable_start && sp[i] < (uint32_t) (uintptr_t) etext)
            port_log("  #%d 0x%08x (stack +%d)\n", 100 + k++, sp[i], i * 4);
#endif
#if defined(__arm__)
    { /* the device: no unwinder, so the faulting instruction and its caller (addr2line -f -e build/game/miyoo/pm64) */
        const mcontext_t *m = &((ucontext_t *) uc)->uc_mcontext;
        port_log("  pc %08lx lr %08lx sp %08lx r0 %08lx r1 %08lx r2 %08lx r3 %08lx\n", (unsigned long) m->arm_pc,
                 (unsigned long) m->arm_lr, (unsigned long) m->arm_sp, (unsigned long) m->arm_r0, (unsigned long) m->arm_r1,
                 (unsigned long) m->arm_r2, (unsigned long) m->arm_r3);
    }
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}

/* The game's priority (the kit's k1.6, hh_priority.c, from DKR b37c2ef; the 2-second beat). an OS helper script
 * forks every ~2.05 s, 15-25 ms of CPU on the one core; at the game's priority they land inside
 * a frame, which runs late: a hitch every 2 s in every port (DKR on the device: frames losing >= 15 ms 0.40 -> 0.05
 * a second). A modest nice (not a real-time class: nothing in the OS can starve) lets them run in the frame's gaps.
 * First thing in main, so SDL's audio thread inherits it. PM64_PRIO=<nice> (default -10; 0: off, as before). The
 * desktop needs root or CAP_SYS_NICE (docker run --cap-add SYS_NICE) to go below 0; it says so and keeps its own. */
static void port_priority(void) {
    const char *e = getenv("PM64_PRIO");
    const int want = e ? atoi(e) : -10, before = getpriority(PRIO_PROCESS, 0);
    if (want != 0 && setpriority(PRIO_PROCESS, 0, want) != 0)
        port_log("priority: nice %d refused (%s), kept %d\n", want, strerror(errno), before);
    port_log("priority: nice %d (asked %d)\n", getpriority(PRIO_PROCESS, 0), want);
}

/* A pacing wait. Any sleep on the Miyoo Mini costs a whole kernel tick, 9.2-9.6 ms however short the request
 * (measured on the Mini Plus), so a frame's remainder under ~10 ms slept was a frame late. Only the part of a wait
 * above 12 ms is slept; the rest is a sched_yield loop on the monotonic clock, to the same deadline. On a kernel whose
 * sleeps are exact the timing is the same. PM64_SPINWAIT=0: one sleep, as before. Returns the microseconds waited. */
extern int sched_yield(void); /* not <sched.h>: the game has a sched.h of its own on the include path */
static unsigned int spin_wait_us(unsigned int us) {
    static int spin = -1;
    struct timespec ts;
    unsigned long long t0, now, until;
    if (spin < 0) { const char *e = getenv("PM64_SPINWAIT"); spin = !(e && e[0] == '0'); }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t0 = (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
    until = t0 + (unsigned long long)us * 1000ull;
    for (now = t0; now < until;) {
        const unsigned long long left = until - now;
        if (!spin || left > 12000000ull) {
            const unsigned long long s = spin ? left - 12000000ull : left;
            struct timespec d = { (time_t)(s / 1000000000ull), (long)(s % 1000000000ull) };
            nanosleep(&d, NULL); /* a signal cutting it short: the loop goes on to the deadline */
        } else {
            sched_yield();
        }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
    }
    return (unsigned int)((now - t0) / 1000u);
}

int main(int argc, char **argv) {
    debug_cfg_load(port_save_path("debug.cfg"), port_log); /* the settings file (debug_cfg.h): before any switch is read */
    port_clock_pick(); /* PM64_VDSOCLOCK: before any other thread reads the clock */
    port_priority();
    savestate_set_progname(argv[0]);
    savestate_parse_args(argc, argv); /* a relaunch: --loadStateFile <path> */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    /* the player's ROM: the game streams from it (port_rom.c) and the asset region is built from it. A normal start
     * opens the ROM found and checked at an earlier start (data/rom_path.txt, the same size and time); else the search
     * (port_find_rom: the game's folder, Roms/, the card; header and SHA-1), which remembers what it finds.
     * PM64_ROM=<file> (tests) wins over both. */
    int opened = 0;
    char why[160] = "";
    {
        char rom[512] = "";
        const char *env = getenv("PM64_ROM");
        if (!env && port_known_rom(rom, sizeof rom)) opened = port_rom_open(rom);
        if (!opened && port_find_rom(rom, sizeof rom, why, sizeof why)) opened = port_rom_open(rom);
        if (!opened) port_log("port: no Paper Mario (USA) ROM found%s%s (set PM64_ROM)\n", why[0] ? ": " : "", why);
        else port_assets_rom_hint(rom); /* the region's build uses it: one scan of the card, not two */
    }
    port_assets_load(argv[0]); /* shows its own screen and exits when there is no ROM and no cache */
    if (!opened) { port_assets_no_rom(why); return 1; } /* with the cache alone: the game reads the ROM as it runs */
    port_state_init(); /* the region's write tracking, for save states */
    port_assets_done(); /* the first-start screen ends here, after the last of its work: Done, then black */
    const char *fe = getenv("PM64_FRAMES");
    const uint32_t frames = fe ? (uint32_t) strtoul(fe, NULL, 10) : 0;

#ifdef PM64_SDL12
    port_video_init();
    savestate_boot_prompt(); /* an auto save on the card: RESUME GAME / NEW GAME */
#endif
    port_game_boot();
    uint32_t next = port_time_us();
    /* PERF every 300 game frames (10 s at 30 fps), in the other ports' shape: work = a game frame's input, logic,
     * audio and drawing (gfx and audio are parts of it), iter = work + sleep, late = frames whose work passed 33.3 ms
     * (a playtest's speed, round 3) */
    uint32_t perfT0 = port_time_us(), perfWork = 0, perfMax = 0, perfSleep = 0, perfLate = 0, perfN = 0;
    while (!port_quit) {
        const uint32_t t0 = port_time_us();
        sLoopIter++;
        port_loop_stage = PORT_STAGE_INPUT;
        port_input_poll(); /* the menu runs in here, and the power-off is taken */
        port_loop_stage = PORT_STAGE_CHECK;
        port_state_frame(port_frame);
        savestate_check(); /* a save, load, resume or exit asked for: between two game frames */
        port_loop_stage = PORT_STAGE_GAME;
        port_game_retrace();
        port_game_retrace();
        port_frame++;
        port_debug_frame();
        port_loop_stage = PORT_STAGE_SAVE;
        port_flash_commit(); /* the game's save of this frame, whole, as one replace (port_rom.c) */
        port_loop_stage = PORT_STAGE_SLEEP;
        { /* PM64_FRAMEDELAY=<ms> (test): each game frame that much slower, as a heavy scene on the device */
            static int sDelay = -1;
            if (sDelay < 0) { const char *e = getenv("PM64_FRAMEDELAY"); sDelay = e ? atoi(e) : 0; }
            if (sDelay > 0) usleep((useconds_t) sDelay * 1000u);
        }
        const uint32_t work = port_time_us() - t0;
        perfWork += work;
        if (work > perfMax) perfMax = work;
        if (work > 33333) perfLate++;
        perfN++;
        if (frames && port_frame >= frames) break;
#ifdef PM64_SDL12
        /* the game steps at 30 fps (every second retrace); a late frame is not made up. No frame's remainder is longer
         * than a frame: a longer "wait" is the 32-bit clock wrapping after a stall of over 2^31 us (35.8 min: the
         * handheld menu runs inside a frame and can be left open that long), which slept the difference, up to 35
         * minutes on the last frame (ocarina-of-time 1138e29, the 2026-10-01 sweep). That is a late frame too. */
        next += 33333;
        const uint32_t now = port_time_us();
        const int32_t ahead = (int32_t) (next - now);
        if (ahead > 0 && ahead <= 33333) { perfSleep += spin_wait_us((unsigned int) ahead); } /* the time actually waited */
        else if (ahead > 33333 || ahead < -100000) next = now;
#endif
        if (perfN == 300) {
            const uint32_t el = port_time_us() - perfT0;
#if defined(MIYOO)
            char mt[160] = ""; /* the second core: split triangles, the split share of the 3D pixels, handoffs (gfx_soft.c) */
            extern int gfx_soft_mt_line(char *buf, int n);
            gfx_soft_mt_line(mt, (int) sizeof mt);
            port_log("PERF work %.1f ms avg (iter %.1f, max %.1f, sleep %.1f) | gfx %.1f audio %.1f | fps %.1f | late %u/%u"
                     " | audio gaps %u%s\n",
                     perfWork / 1000.0 / perfN, el / 1000.0 / perfN, perfMax / 1000.0, perfSleep / 1000.0 / perfN,
                     port_perf_gfx_us / 1000.0 / perfN, port_perf_audio_us / 1000.0 / perfN, perfN * 1e6 / (el ? el : 1),
                     perfLate, perfN, port_audio_out_underruns, mt);
#else
            port_log("PERF work %.1f ms avg (iter %.1f, max %.1f, sleep %.1f) | gfx %.1f audio %.1f | fps %.1f | late %u/%u"
                     " | audio gaps %u\n",
                     perfWork / 1000.0 / perfN, el / 1000.0 / perfN, perfMax / 1000.0, perfSleep / 1000.0 / perfN,
                     port_perf_gfx_us / 1000.0 / perfN, port_perf_audio_us / 1000.0 / perfN, perfN * 1e6 / (el ? el : 1),
                     perfLate, perfN, port_audio_out_underruns);
#endif
            port_audio_out_underruns = 0;
            perfT0 = port_time_us();
            perfWork = perfMax = perfSleep = perfLate = perfN = 0;
            port_perf_gfx_us = port_perf_audio_us = 0;
        }
    }
    port_log("port: %u game frames\n", port_frame);
    savestate_shutdown();
#ifdef PM64_SDL12
    /* the launcher after us gets the console back (port_sdl.c); the last line of console.log, as in every port */
    port_log(port_video_shutdown() ? "exit: SDL shut down\n" : "exit: SDL left up (a flip still running)\n");
#else
    port_log("exit\n"); /* console.log: the game got out */
#endif
    return 0;
}
