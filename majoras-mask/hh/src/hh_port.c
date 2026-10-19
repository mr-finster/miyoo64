/* The port_* services the MK64 handheld renderer expects (gfx/port.h), for OoT. */
#ifndef HH_RELEASE
#define HH_RELEASE "dev"
#endif
#ifndef HH_VERSION
#define HH_VERSION "dev"
#endif
#include <stdarg.h>
#include <stdio.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>

#include "port.h"
#include "segmented_address.h"

uint32_t gPortProfFrame[PROF_COUNT];
uint32_t gPortProfLast[PROF_COUNT];
volatile uint32_t gPortProfFlipUs;

static int sQuiet = -1;

void port_log(const char* fmt, ...) {
    va_list ap;

    if (sQuiet < 0) {
        sQuiet = getenv("OOT_QUIET") != NULL;
    }
    if (sQuiet) {
        return;
    }
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* PERF/STALL lines: stdout and <save dir>/console.log (the device's launcher start has
 * no terminal; the file comes back on the card). */
static FILE* sLogFile;

/* One console.log per session; the previous two are kept as console.1.log and
 * console.2.log (as MK64 does), and each starts with the build stamp so a log
 * can be matched to its build (BUILDLOG records each version's stamp). */
const char* port_save_path(const char* name);

static void HhLog_Open(void) {
    extern const uint32_t hh_build_stamp;
    char a[600], b[600];

    snprintf(a, sizeof(a), "%s", port_save_path("console.1.log"));
    snprintf(b, sizeof(b), "%s", port_save_path("console.2.log"));
    rename(a, b);
    snprintf(b, sizeof(b), "%s", port_save_path("console.log"));
    rename(b, a);
    sLogFile = fopen(b, "w");
    if (sLogFile != NULL) {
        fprintf(sLogFile, "SESSION mm %s version %s build %08x\n", HH_RELEASE, HH_VERSION, (unsigned)hh_build_stamp);
        /* kit k1.6's hh_game_priority reports on stderr, which the launcher start drops: repeat the
         * result here (process-wide, set at the top of main before any thread). */
        fprintf(sLogFile, "priority: nice %d\n", getpriority(PRIO_PROCESS, 0));
        fflush(sLogFile);
#ifdef HANDHELD /* the device build only (TARGET_HANDHELD is set for the desktop build too) */
        /* The launcher start drops stderr, and the kit reports there (save states: each stage's
         * strerror, kit 88210e7; the priority): on the device it lands in console.log too. */
        dup2(fileno(sLogFile), STDERR_FILENO);
#endif
        fflush(sLogFile);
    }
}

void port_log_always(const char* fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (sLogFile == NULL) {
        HhLog_Open();
    }
    if (sLogFile != NULL) {
        va_start(ap, fmt);
        vfprintf(sLogFile, fmt, ap);
        va_end(ap);
        fflush(sLogFile);
    }
}

/* Monotonic microseconds. musl 1.2's clock_gettime tries the time64 syscall
 * first and the handheld's 4.14 kernel lacks it, so every read cost two kernel
 * entries (profile: ~2% of the CPU in clock_gettime + the thread-pointer reads
 * around it). SYS_clock_gettime32 goes straight to the one that exists.
 * (MK64 perf round 24.)
 *
 * OOT_VDSOCLOCK (default on; =0 the syscall always): the kernel's own
 * __vdso_clock_gettime (the same CLOCK_MONOTONIC, read in user space from the
 * ARM generic timer when the clocksource allows it, else its own one syscall).
 * musl 1.2.1 never looks the vDSO up, and the renderer reads this
 * clock several times a draw (gfx_soft's PROF_BEGIN/END per batch, uploads,
 * clears): the 9d432f7e basement profile had ~1% of the CPU at the `syscall`
 * wrapper. The vDSO is found through AT_SYSINFO_EHDR (its ELF symbol table) and
 * only used after a check against the syscall at the first call: the same clock
 * within 2 ms and not going backwards, else the syscall as before. Only the
 * cost of a read changes, never what the game or the renderer computes. */
typedef struct {
    long s, ns;
} HhTs32;
typedef int (*HhVdsoCgt)(clockid_t, HhTs32*);
static HhVdsoCgt sVdsoCgt;
static volatile int sVdsoState; /* 0 not looked up, 1 in use, -1 not available */

static int port_clock_syscall(HhTs32* t) {
#if defined(SYS_clock_gettime32)
    return syscall(SYS_clock_gettime32, CLOCK_MONOTONIC, t) == 0 ? 0 : -1;
#else
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }
    t->s = (long)ts.tv_sec;
    t->ns = ts.tv_nsec;
    return 0;
#endif
}

#if defined(__linux__) && (defined(__arm__) || defined(__i386__))
#include <elf.h>
#include <sys/auxv.h>
/* __vdso_clock_gettime in the vDSO image the kernel maps (a 32-bit ELF), or NULL. */
static HhVdsoCgt port_vdso_lookup(void) {
    const uintptr_t base = (uintptr_t)getauxval(AT_SYSINFO_EHDR);
    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)base;
    const Elf32_Shdr* sh;
    uintptr_t bias = 0;
    int i, haveLoad = 0;

    if (base == 0 || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS32 ||
        eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf32_Shdr) || eh->e_phentsize != sizeof(Elf32_Phdr)) {
        return NULL;
    }
    for (i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr* ph = (const Elf32_Phdr*)(base + eh->e_phoff) + i;

        if (ph->p_type == PT_LOAD) {
            bias = base + ph->p_offset - ph->p_vaddr;
            haveLoad = 1;
            break;
        }
    }
    if (!haveLoad) {
        return NULL;
    }
    sh = (const Elf32_Shdr*)(base + eh->e_shoff);
    for (i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_DYNSYM && sh[i].sh_entsize == sizeof(Elf32_Sym) && sh[i].sh_link < eh->e_shnum) {
            const Elf32_Sym* sym = (const Elf32_Sym*)(base + sh[i].sh_offset);
            const char* str = (const char*)(base + sh[sh[i].sh_link].sh_offset);
            const size_t n = sh[i].sh_size / sizeof(Elf32_Sym);
            size_t k;

            for (k = 0; k < n; k++) {
                if (ELF32_ST_TYPE(sym[k].st_info) == STT_FUNC && sym[k].st_shndx != SHN_UNDEF &&
                    strcmp(str + sym[k].st_name, "__vdso_clock_gettime") == 0) {
                    return (HhVdsoCgt)(bias + sym[k].st_value);
                }
            }
        }
    }
    return NULL;
}
#else
static HhVdsoCgt port_vdso_lookup(void) {
    return NULL;
}
#endif

static int port_vdso_init(void) {
    const char* e = getenv("OOT_VDSOCLOCK");
    HhVdsoCgt f = (e != NULL && e[0] == '0') ? NULL : port_vdso_lookup();
    HhTs32 a, b, c;

    if (f != NULL && port_clock_syscall(&a) == 0 && f(CLOCK_MONOTONIC, &b) == 0 && port_clock_syscall(&c) == 0) {
        const int64_t ua = (int64_t)a.s * 1000000 + a.ns / 1000, ub = (int64_t)b.s * 1000000 + b.ns / 1000;
        const int64_t uc = (int64_t)c.s * 1000000 + c.ns / 1000;

        if (ub >= ua && uc >= ub && uc - ua < 2000) {
            sVdsoCgt = f;
            return 1;
        }
    }
    return -1;
}

uint64_t port_time_us64(void) {
    HhTs32 t;
    int st = sVdsoState;

    if (st == 0) { /* racing threads reach the same answer */
        st = port_vdso_init();
        sVdsoState = st;
    }
    if (st > 0 && sVdsoCgt(CLOCK_MONOTONIC, &t) == 0) {
        return (uint64_t)t.s * 1000000u + (uint64_t)(t.ns / 1000);
    }
    if (port_clock_syscall(&t) == 0) {
        return (uint64_t)t.s * 1000000u + (uint64_t)(t.ns / 1000);
    }
    {
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    }
}

/* console.log's CLOCK line (hh_renderer.c, the first frame): which read port_time_us64 uses. */
const char* port_clock_source(void) {
    (void)port_time_us64();
    return sVdsoState > 0 ? "vdso" : "syscall";
}

uint32_t port_time_us(void) {
    return (uint32_t)port_time_us64();
}

/* Saves/config: $OOT_DATA, else data/ next to the executable on the Miyoo, else ./oot_data. */
const char* port_save_dir(void) {
    static char sDir[512];

    if (sDir[0] == '\0') {
        const char* env = getenv("MM_DATA") != NULL ? getenv("MM_DATA") : getenv("OOT_DATA"); /* launch.sh: MM_DATA */
#if defined(MIYOO)
        /* Miyoo Mini: data/ next to the executable (App/OcarinaOfTime/data; launch.sh sets OOT_DATA) */
        if (env == NULL) {
            char exe[480];
            const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
            char* slash;
            exe[n > 0 ? n : 0] = '\0';
            slash = strrchr(exe, '/');
            if (n > 0 && slash != NULL) { *slash = '\0'; snprintf(sDir, sizeof(sDir), "%s/data", exe); }
            else snprintf(sDir, sizeof(sDir), "/mnt/SDCARD/App/MajorasMask/data");
        } else {
            snprintf(sDir, sizeof(sDir), "%s", env);
        }
#else
        snprintf(sDir, sizeof(sDir), "%s", (env != NULL) ? env : "oot_data");
#endif
        port_fs_mkdir(sDir);
    }
    return sDir;
}

const char* port_save_path(const char* name) {
    static char sPath[640];

    snprintf(sPath, sizeof(sPath), "%s/%s", port_save_dir(), name);
    return sPath;
}

int port_fs_mkdir(const char* path) {
    return mkdir(path, 0755);
}

/* A segmented address (segment 1-15 in the top byte, below the real-pointer
 * floor) through the game's segment table; anything else is already a pointer.
 * gSegments holds OS_K0_TO_PHYSICAL(ptr) = ptr - K0BASE (mod 2^32), except 0x0E/0x0F
 * (the master DL, the framebuffer), which hold the pointer: host pointers lie in
 * [0x40000000, 0xC0000000), a physical form outside it (z_rcp.c's segment sync). */
void* port_seg_to_ptr(uintptr_t addr) {
    uint32_t seg;

    if (addr >= PORT_REAL_PTR_MIN) {
        return (void*)addr;
    }
    seg = (uint32_t)(addr >> 24);
    if (seg >= 1 && seg < NUM_SEGMENTS && gSegments[seg] != 0) {
        uintptr_t base = gSegments[seg];
        if (base < 0x40000000u || base >= 0xC0000000u) {
            base += 0x80000000u;
        }
        return (void*)(uintptr_t)(base + (addr & 0x00FFFFFF));
    }
    return (void*)addr;
}

void port_gfx_overlay(void) {
}

/* gfx_pc.c resets the projection and model-view stacks to this. */
float identity_matrix[4][4] = {
    { 1.0f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 1.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 1.0f, 0.0f },
    { 0.0f, 0.0f, 0.0f, 1.0f },
};
