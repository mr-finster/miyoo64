/* hh_port.c -- the port_* services of gfx/port.h (logging, clock, save dir,
 * segments), plus small hooks the fork's PORT code calls that have nothing
 * to do on this target. */
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "port.h"

uint32_t gPortProfFrame[PROF_COUNT];
uint64_t gPortAudioUsTotal; /* synthesis time over the whole run (BENCH line) */

/* n_alAudioFrame time, called from the decomp audio thread (sys/audio.c). */
void hh_prof_audio(uint32_t us)
{
    PORT_PROF_ADD(PROF_AUDIO, us);
    gPortAudioUsTotal += us;
}
uint32_t gPortProfLast[PROF_COUNT];
volatile uint32_t gPortProfFlipUs;
/* PERF2 line (main.c): reloc file loads (read, vpk0, byte swap, relocation)
 * and texture-cache range evictions inside the logic tick, per PERF window. */
uint32_t gPortRelocUs, gPortRelocLoads, gPortEvictUs, gPortEvictCalls;

static int sQuiet = -1;
int hh_cfg_switch(const char *env, const char *key, int dflt);
static FILE *sLogFile;
static int sLogClosed; /* port_exit closed console.log: never reopen it (that would rotate it away) */
/* console.log's descriptor while it is open, for the fatal-signal line (gfx_sdl12.c fatal_handler, which
 * may only write()); -1 before it opens and once port_exit closes it. */
static volatile sig_atomic_t sLogFd = -1;

/* Diagnostic chatter (the decomp's per-fighter / per-object traces: thousands of lines a minute, and a printf + fflush
 * each): off unless SSB64_LOGVERBOSE=1 (or logverbose=1 in bench.cfg), and SSB64_QUIET still silences it. It never
 * reached console.log, so a bug report loses nothing. */
void port_log(const char *fmt, ...)
{
    va_list ap;
    if (sQuiet < 0) sQuiet = getenv("SSB64_QUIET") != NULL || hh_cfg_switch("SSB64_LOGVERBOSE", "logverbose", 0) <= 0;
    if (sQuiet) return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* PERF lines and errors: stdout and <save dir>/console.log (a launcher start has
 * no terminal; the file comes back on the card). */
void port_log_always(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (sLogClosed) return;
    /* The bench chain re-execs per stage and keeps one log (--append-log). */
    if (sLogFile == NULL) {
        int append = getenv("SSB64_LOG_APPEND") != NULL;
        if (!append) {
            /* keep the previous session's log (MK64 keeps two) */
            char cur[700], old[700];
            snprintf(cur, sizeof cur, "%s", port_save_path("console.log"));
            snprintf(old, sizeof old, "%s", port_save_path("console.1.log"));
            rename(cur, old);
        }
        sLogFile = fopen(port_save_path("console.log"), append ? "a" : "w");
        if (sLogFile != NULL) sLogFd = fileno(sLogFile);
    }
    if (sLogFile != NULL) {
        va_start(ap, fmt);
        vfprintf(sLogFile, fmt, ap);
        va_end(ap);
        fflush(sLogFile);
    }
}

/* port_log.h: the log's raw descriptor for an async-signal-safe crash handler, or -1. Every
 * port_log_always line is flushed whole, so a write() here lands after the last full line. */
int port_log_get_fd(void)
{
    return sLogFd;
}

/* Monotonic microseconds. */
/* SSB64_VDSOCLOCK (default 1; 0 = the syscall always, as before; or vdsoclock=0 in bench.cfg): the kernel's own __vdso_clock_gettime, the
 * same CLOCK_MONOTONIC read in user space, instead of a syscall. The handheld's musl 1.2.1 never looks the vDSO up.
 * GoldenEye's device Bench (goldeneye-007 X37, hh_os.c): 157-167 ns a read against 598-611 ns for the syscall.
 * Taken from it (as mario-kart-64's MK64_VDSOCLOCK): the vDSO is found through AT_SYSINFO_EHDR's symbol table and used
 * only if, best of three trials of 256 reads each, its readings lie between the syscall's around them and cost under
 * half as much; one CLOCK line says which. Only the cost of a read changes, never the clock read. */
#include "savestate.h" /* SAVESTATE_EXCLUDE (a no-op with the include list) */
int hh_cfg_switch(const char *env, const char *key, int dflt);
typedef int (*PortCgt32)(int, void *);
static PortCgt32 sVdsoCgt SAVESTATE_EXCLUDE;    /* the address is this process's, never a saved one */
static int sClockMode SAVESTATE_EXCLUDE = -1;   /* -1 not picked, 0 syscall, 1 vDSO */

static uint64_t port_clock_us_sys(void)
{
#if defined(SYS_clock_gettime32)
    /* musl 1.2's clock_gettime tries the time64 syscall first and the handheld's 4.14 kernel lacks it, so every read
     * cost two kernel entries. SYS_clock_gettime32 goes straight to the one that exists (MK64 perf round 24). */
    struct { long s, ns; } t;
    if (syscall(SYS_clock_gettime32, CLOCK_MONOTONIC, &t) == 0)
        return (uint64_t)t.s * 1000000u + (uint64_t)(t.ns / 1000);
#endif
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    }
}

static uint64_t port_clock_us_vdso(void)
{
    struct { long s, ns; } t;
    if (sVdsoCgt(CLOCK_MONOTONIC, &t) == 0) return (uint64_t)t.s * 1000000u + (uint64_t)(t.ns / 1000);
    return port_clock_us_sys();
}

#if defined(__linux__) && __SIZEOF_POINTER__ == 4
#include <elf.h>
#include <sys/auxv.h>
/* A symbol of the vDSO image (32-bit ELF), as musl's __vdsosym; NULL if none. */
static void *port_vdso_sym(const char *want)
{
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)(uintptr_t)getauxval(AT_SYSINFO_EHDR);
    const Elf32_Phdr *ph;
    const Elf32_Shdr *sh;
    uintptr_t bias = 0;
    int i, have = 0;
    if (eh == NULL || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS32) return NULL;
    ph = (const Elf32_Phdr *)((const char *)eh + eh->e_phoff);
    for (i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) { bias = (uintptr_t)eh + ph[i].p_offset - ph[i].p_vaddr; have = 1; break; }
    if (!have || eh->e_shoff == 0) return NULL;
    sh = (const Elf32_Shdr *)((const char *)eh + eh->e_shoff);
    for (i = 0; i < eh->e_shnum; i++) {
        const Elf32_Sym *sym;
        const char *str;
        unsigned n, k;
        if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum) continue;
        sym = (const Elf32_Sym *)((const char *)eh + sh[i].sh_offset);
        str = (const char *)eh + sh[sh[i].sh_link].sh_offset;
        n = sh[i].sh_size / sizeof(Elf32_Sym);
        for (k = 0; k < n; k++)
            if (ELF32_ST_TYPE(sym[k].st_info) == STT_FUNC && sym[k].st_shndx != SHN_UNDEF && strcmp(str + sym[k].st_name, want) == 0)
                return (void *)(bias + sym[k].st_value);
    }
    return NULL;
}
#else
static void *port_vdso_sym(const char *want) { (void)want; return NULL; }
#endif

/* 256 reads of each, the syscall's around the vDSO's: 0 if the vDSO's readings fall outside them */
static int port_clock_trial(uint64_t *vdso_ns, uint64_t *sys_ns)
{
    uint64_t t0, t1, t2, a, b;
    int i;
    t0 = port_clock_us_sys();
    for (i = 0; i < 256; i++) (void)port_clock_us_sys();
    t1 = port_clock_us_sys();
    a = port_clock_us_vdso();
    for (i = 0; i < 256; i++) (void)port_clock_us_vdso();
    b = port_clock_us_vdso();
    t2 = port_clock_us_sys();
    *vdso_ns = (b - a) * 1000u / 257u;
    *sys_ns = (t1 - t0) * 1000u / 257u;
    return a >= t1 && b <= t2;
}

/* Once, at start-up before any other thread (port_time_us picks on its first read otherwise). */
void port_clock_pick(void)
{
    uint64_t v = ~0ull, s = ~0ull, tv, ts;
    int i, on, same = 1;
    if (sClockMode >= 0) return;
    on = hh_cfg_switch("SSB64_VDSOCLOCK", "vdsoclock", 1) != 0;
    sVdsoCgt = on ? (PortCgt32)port_vdso_sym("__vdso_clock_gettime") : NULL;
    if (sVdsoCgt == NULL) {
        sClockMode = 0;
        port_log_always("CLOCK syscall (%s)\n", on ? "no vDSO clock_gettime" : "SSB64_VDSOCLOCK=0");
        return;
    }
    for (i = 0; i < 3; i++) { /* the best of three (a GoldenEye device run once caught an interrupt inside one) */
        same &= port_clock_trial(&tv, &ts);
        if (tv < v) v = tv;
        if (ts < s) s = ts;
    }
    sClockMode = same && v * 2 < s;
    port_log_always("CLOCK %s: vDSO %u ns/read vs syscall %u ns/read\n", sClockMode ? "vdso" : "syscall", (unsigned)v, (unsigned)s);
    if (!sClockMode) sVdsoCgt = NULL;
}

uint32_t port_time_us(void)
{
    if (sClockMode > 0) return (uint32_t)port_clock_us_vdso();
    if (sClockMode < 0) port_clock_pick();
    return (uint32_t)(sClockMode > 0 ? port_clock_us_vdso() : port_clock_us_sys());
}

/* Saves, config and the ROM slice: $SSB64_DATA (the Miyoo: miyoo_data_dir) on
 * the device, else ./ssb64_data. */
const char *port_save_dir(void)
{
    static char sDir[512];
    if (sDir[0] == '\0') {
        const char *env = getenv("SSB64_DATA");
#if defined(MIYOO)
        /* The Miyoo Mini: $SSB64_DATA (launch.sh sets it to App/SuperSmashBros/data), else data/ next to
         * the executable (kit miyoo_panel.c: wherever the launcher keeps the game). */
        extern const char *miyoo_data_dir(void);
        snprintf(sDir, sizeof sDir, "%s", miyoo_data_dir());
        (void)env;
#else
        snprintf(sDir, sizeof sDir, "%s", env ? env : "ssb64_data");
#endif
        port_fs_mkdir(sDir);
    }
    return sDir;
}

const char *port_save_path(const char *name)
{
    static char sPath[640];
    snprintf(sPath, sizeof sPath, "%s/%s", port_save_dir(), name);
    return sPath;
}

/* The renderer's NEON paths on or off (round 15, the lead's round-2 item 8: a NEON-off
 * bench run on the device). SSB64_NEON=0, or a line "neon=0" in <save dir>/bench.cfg (a
 * launcher start has no environment of its own), turns off the span rows (cm_row_neon) and the
 * vertex transform; both fall back to their scalar loops, which draw the same frames. The
 * audio mixer's NEON stays on. Read once; the bench's first log line says which. */
int hh_neon_enabled(void)
{
    static int sNeon = -1;
    if (sNeon < 0) {
        const char *e = getenv("SSB64_NEON");
        sNeon = 1;
        if (e && e[0]) {
            sNeon = e[0] != '0';
        } else {
            char line[64];
            FILE *f = fopen(port_save_path("bench.cfg"), "r");
            while (f != NULL && fgets(line, sizeof line, f) != NULL) {
                if (strncmp(line, "neon=", 5) == 0) sNeon = line[5] != '0';
            }
            if (f != NULL) fclose(f);
        }
    }
    return sNeon;
}

/* A speed switch's value: the environment variable, else a line "<key>=<n>" in <save dir>/bench.cfg (a launcher
 * start has no environment of its own), else dflt; -1 when neither sets it and dflt is -1 (2026-10-03: the
 * texel-lerp rows, lerprows=). */
int hh_cfg_switch(const char *env, const char *key, int dflt)
{
    const char *e = getenv(env);
    if (e && e[0]) return atoi(e);
    char line[64];
    const size_t n = strlen(key);
    int v = dflt;
    FILE *f = fopen(port_save_path("bench.cfg"), "r");
    while (f != NULL && fgets(line, sizeof line, f) != NULL)
        if (strncmp(line, key, n) == 0 && line[n] == '=') v = atoi(line + n + 1);
    if (f != NULL) fclose(f);
    return v;
}

/* The 2026-10-09 picture fixes' switches (SSB64_<NAME>=0|1|ab, or "<key>=0|1|ab" in bench.cfg): 0 off, 1 on,
 * ab = off and on in turn every 5 s of play (gfx_pc.c gfx_fix_on), for an A/B on the device; dflt when neither
 * sets it. Returns 0, 1 or 2 (ab), and logs a switch that isn't at its default. */
int hh_fix_switch(const char *env, const char *key, int dflt)
{
    char val[16] = "", line[64];
    const char *e = getenv(env);
    if (e && e[0]) snprintf(val, sizeof val, "%s", e);
    else {
        const size_t n = strlen(key);
        FILE *f = fopen(port_save_path("bench.cfg"), "r");
        while (f != NULL && fgets(line, sizeof line, f) != NULL)
            if (strncmp(line, key, n) == 0 && line[n] == '=') sscanf(line + n + 1, "%15s", val);
        if (f != NULL) fclose(f);
    }
    const int v = val[0] == '\0' ? dflt : (val[0] == 'a' && val[1] == 'b') ? 2 : atoi(val) != 0;
    if (v != dflt) port_log_always("SSB64: %s %s\n", env, v == 2 ? "ab (off, then on, every 5 s)" : v ? "on" : "off");
    return v;
}

/* Leave for the launcher (round 15b, the user's report on bench v12: "it quit but didn't exit and
 * I had to hard reboot (black screen)"). The watchdog comes first: SIGALRM's default action ends
 * the process, so nothing on the way out -- the present thread, the audio device, an exit
 * handler -- can leave the device on a black screen. Then the platform stops drawing (the
 * present thread idle, so fb_unpan has the last word on the pan offset) and shuts SDL down
 * (X50: the launcher after us could hang at its first flip otherwise), console.log is
 * flushed and closed and the card synced before exit. */
void port_exit(int code)
{
    sigset_t s;
    signal(SIGALRM, SIG_DFL);
    sigemptyset(&s);
    sigaddset(&s, SIGALRM);
    sigprocmask(SIG_UNBLOCK, &s, NULL);
    alarm(5);
    port_log_always(port_platform_quiesce() ? "exit: SDL shut down\n" /* X50: the same last line in every port */
                                            : "exit: SDL left up (a flip still running)\n");
    fflush(NULL);
    sLogFd = -1;
    if (sLogFile != NULL) { fclose(sLogFile); sLogFile = NULL; }
    sLogClosed = 1;
    sync(); /* console.log and sram.bin reach the card even if the power goes next */
    exit(code);
}

int port_fs_mkdir(const char *path)
{
    return mkdir(path, 0755);
}

/* Smash sets every segment it uses through gSPSegment in its display lists,
 * which gfx_pc tracks itself; there is no CPU-side segment table to consult. */
void *port_seg_to_ptr(uintptr_t addr)
{
    return (void *)addr;
}

void port_gfx_overlay(void) {}

/* gfx_pc.c resets the projection and model-view stacks to this. */
float identity_matrix[4][4] = {
    { 1.0f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 1.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 1.0f, 0.0f },
    { 0.0f, 0.0f, 0.0f, 1.0f },
};

/* ---- fork hooks with nothing to do here ---- */
/* BattleShip's widescreen and 4:3 scissor modes (intro scenes). */
void GfxSetNativeDimensions(unsigned int width, unsigned int height) { (void)width; (void)height; }
void GfxSetTight4_3ScissorWindow(int active) { (void)active; }
/* GfxSetWidescreenFramebufferPersistence: gfx_pc.c (B4, the opening's burst). */
/* Frame interpolation (BattleShip's 120 Hz mode). */
void portInterpRecordMtx(void *mtx, void *owner, int ordinal, int tag) { (void)mtx; (void)owner; (void)ordinal; (void)tag; }
/* Diagnostics. */
void portDiagArmImportCapture(int n) { (void)n; }
void port_dump_backtrace(void) {}
