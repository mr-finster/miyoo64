/* hh_boot.c: process start. Recreates the N64 memory map, loads the cartridge, starts the platform, then runs
 * Banjo-Kazooie's own boot (core1_main and the init thread's work: the PI manager, the main thread) and hands the CPU to
 * the kernel loop (hh_os.c).
 *
 * Memory: the 4 MB of N64 DRAM are an anonymous mapping at 0x80000000 and this binary is linked at 0x80400000, so
 * every address the game sees is a KSEG0 address and the N64's address macros work unmodified. The game's fixed
 * buffers (heap, frame buffers, depth buffer) sit at their N64 addresses in that block (hh_bk.c). */
#define _GNU_SOURCE 1 /* REG_EIP */
#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/stat.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#include "hh_os.h"
#include "hh_hw.h"
#include "hh_plat.h"
#include "hh_romscreen.h"
#include "savestate.h"

#define HH_DRAM_BASE 0x80000000u
#define HH_DRAM_SIZE 0x00400000u
#define HH_WINDOW_END 0x81000000u /* segment 0 addresses reach 16 MB */

extern char _end[];
extern char etext[]; /* end of .text */
#define __executable_end_guess etext[0]
const char *hh_card_setup(void); /* hh_card.c: the handheld's log, knobs and ROM */
void hh_card_allow_rebuild(void), hh_card_rebuild_done(void); /* hh_card.c: <data dir>/rebuild */
void hh_ovl_init(void); /* hh_bk.c */

/* The N64's DRAM (4 MB, mapped at a fixed address, outside the binary): the game's heap, its loaded
 * segments and frame buffers -- the third range of a save state (port/kit/platform/savestate.c). */
int hh_game_extra_range(char **base, size_t *size)
{
    *base = (char *)(uintptr_t)HH_DRAM_BASE;
    *size = HH_DRAM_SIZE;
    return 1;
}

static void hh_map_dram(void)
{
    void *p = mmap((void *)(uintptr_t)HH_DRAM_BASE, HH_DRAM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != (void *)(uintptr_t)HH_DRAM_BASE) {
        fprintf(stderr, "FATAL: cannot map N64 DRAM at 0x%08x (got %p: %s)\n", HH_DRAM_BASE, p,
                p == MAP_FAILED ? strerror(errno) : "address taken");
        exit(1);
    }
    /* KSEG1 (uncached) and the RCP's registers: Banjo-Kazooie reads a few words there (IPL3's leftovers in low RAM and
     * the RSP's memories, checked by the anti-tamper code; anctrl.c's animation speed depends on RAM 0x238). Plain
     * memory, with the values the N64 has after its boot. */
    if (mmap((void *)(uintptr_t)0xA0000000u, HH_DRAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
             -1, 0) != (void *)(uintptr_t)0xA0000000u ||
        mmap((void *)(uintptr_t)0xA4000000u, 0x00900000u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
             -1, 0) != (void *)(uintptr_t)0xA4000000u) {
        fprintf(stderr, "FATAL: cannot map the N64's KSEG1 window: %s\n", strerror(errno));
        exit(1);
    }
    *(volatile u32 *)(uintptr_t)0xA0000238u = 0x10000003u; /* anctrl.c, anctrl_setDuration */
    *(volatile u32 *)(uintptr_t)0xA000022Cu = 0x12600004u; /* code_4C020.c, the world exit pads */
    *(volatile u32 *)(uintptr_t)0xA0000200u = 0xAC290000u; /* SM code_2900.c (anti-tamper builds) */
    *(volatile u32 *)(uintptr_t)0xA00001D0u = 0x356BAAAEu; /* MMM tumblar.c */
    *(volatile u32 *)(uintptr_t)0xA00001D8u = 0xAC2B0010u; /* viewport.c */
    *(volatile u32 *)(uintptr_t)0xA0000284u = 0x240B17D7u; /* GV gvspawnqueue.c */
    *(volatile u32 *)(uintptr_t)0xA4000000u = 0xFFFFFFFFu; /* SP DMEM word 0 (ucode.c) */
    *(volatile u32 *)(uintptr_t)0xA4001000u = 0x000017D7u; /* SP IMEM word 0 */
    if ((uintptr_t)_end > HH_WINDOW_END) {
        fprintf(stderr, "FATAL: the binary ends at %p, past the 0x%08x window\n", (void *)_end, HH_WINDOW_END);
        exit(1);
    }
}

/* The cartridge in big-endian (z64) order. A z64 dump is mapped as is; v64 (byte-swapped)
 * and n64 (word-swapped) dumps are read and put in order. */
static void hh_load_rom(const char *path)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    u8 *p;
    u32 i, n;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 0x1000) {
        fprintf(stderr, "FATAL: cannot open the ROM %s: %s\n", path, strerror(errno));
        exit(1);
    }
    n = (u32)st.st_size;
    p = mmap(NULL, n, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "FATAL: cannot map the ROM %s: %s\n", path, strerror(errno));
        exit(1);
    }
    if (p[0] == 0x80 && p[1] == 0x37) {
        hh_rom = p;
    } else if ((p[0] == 0x37 && p[1] == 0x80) || (p[0] == 0x40 && p[1] == 0x12)) {
        u8 *q = malloc(n);
        int v64 = p[0] == 0x37;
        if (q == NULL) {
            fprintf(stderr, "FATAL: out of memory for the ROM\n");
            exit(1);
        }
        for (i = 0; i + 3 < n; i += 4) {
            if (v64) {
                q[i] = p[i + 1]; q[i + 1] = p[i]; q[i + 2] = p[i + 3]; q[i + 3] = p[i + 2];
            } else {
                q[i] = p[i + 3]; q[i + 1] = p[i + 2]; q[i + 2] = p[i + 1]; q[i + 3] = p[i];
            }
        }
        munmap(p, n);
        hh_rom = q;
    } else {
        fprintf(stderr, "FATAL: %s is not an N64 ROM\n", path);
        exit(1);
    }
    close(fd);
    hh_rom_size = n;
    if (memcmp(hh_rom + 0x20, "Banjo-Kazooie", 13) != 0 || hh_rom[0x3E] != 'E' || hh_rom[0x3F] != 0) {
        fprintf(stderr, "FATAL: %s is not Banjo-Kazooie (USA 1.0)\n", path);
        exit(1);
    }
}

/* gdb cannot ptrace under Rosetta: say where it died, for addr2line. */
static void hh_crash_handler(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = ucv;
    unsigned long pc = 0, lr = 0, sp = 0;
#if defined(__i386__)
    pc = uc->uc_mcontext.gregs[REG_EIP];
    sp = uc->uc_mcontext.gregs[REG_ESP];
#elif defined(__arm__)
    pc = uc->uc_mcontext.arm_pc;
    lr = uc->uc_mcontext.arm_lr;
    sp = uc->uc_mcontext.arm_sp;
#endif
    fprintf(stderr, "CRASH: signal %d at pc %08lx lr %08lx sp %08lx, address %p\n", sig, pc, lr, sp,
            si->si_addr);
#if defined(__i386__) || defined(__arm__)
    /* a call through a bad pointer: the return address is on top of the stack (i386);
     * on ARM the saved lr values further up the frames */
    if (sp >= 0x80000000ul && sp < 0x81000000ul) {
        int i;
        fprintf(stderr, "CRASH: stack");
        for (i = 0; i < 4; i++)
            fprintf(stderr, " [%08x]", ((u32 *)sp)[i]);
        fprintf(stderr, "\n");
        /* code addresses further up (return addresses of the callers) */
        for (i = 4; i < 256; i++) {
            u32 w = ((u32 *)sp)[i];
            if (w >= 0x80400000u && w < (u32)(uintptr_t)&__executable_end_guess)
                fprintf(stderr, "CRASH: stack code [%08x] at +%x\n", w, i * 4);
        }
    }
#endif
    hh_os_dump("crash");
#if defined(TARGET_MIYOO)
    { extern void miyoo_clock_restore(void); miyoo_clock_restore(); } /* a crash skips atexit: the clock back first */
#endif
#if defined(__GLIBC__)
    {
        void *bt[48];
        int n = backtrace(bt, 48);
        backtrace_symbols_fd(bt, n, 2);
    }
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}

/* The device backend's fatal handler (gfx_sdl12.c) unpans the panel, then reports here. */
void hh_crash_handler_chain(int sig, siginfo_t *si, void *uc) { hh_crash_handler(sig, si, uc); }

/* With -fstack-protector (debug builds): name the function whose frame was smashed --
 * the unwinder cannot walk a smashed stack. */
void __stack_chk_fail(void)
{
    fprintf(stderr, "CRASH: stack smashed in the function at %p\n", __builtin_return_address(0));
    hh_os_dump("stack smash");
    _exit(134);
}

static void hh_install_crash_handler(void)
{
    static u8 altstack[64 << 10];
    stack_t ss;
    struct sigaction sa;
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    unsigned i;
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = hh_crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);
    /* BK_WATCHDOG=<seconds>: report where the process is after that long (hangs) */
    if (getenv("BK_WATCHDOG") != NULL) {
        sigaction(SIGALRM, &sa, NULL);
        alarm((unsigned)atoi(getenv("BK_WATCHDOG")));
    }
}

int main(int argc, char **argv)
{
    const char *rom = getenv("BK_ROM");
    char path[512];
    (void)argc;
    setvbuf(stdout, NULL, _IOLBF, 0);
    savestate_parse_args(argc, argv); /* --loadStateFile <path>: an resume relaunch (port/kit) */
#ifdef TARGET_HANDHELD
    hh_card_allow_rebuild(); /* the game's own start (not the bench's parent) may redo the first start's job */
    rom = hh_card_setup(); /* the log, debug.cfg, the ROM (hh_card.c); its first-start screen
                            * (hh_romscreen.c) stays up through the tables' fill, below */
#endif
    {
        extern const uint32_t port_build_stamp;
        extern const char port_build_version[], port_build_release[];
        printf("SESSION banjo %s version %s build %08X\n", port_build_release, port_build_version, (unsigned)port_build_stamp);
        fflush(stdout);
    }
    { /* kit k1.6: nice -10 before any thread or child exists, so all inherit it (an OS helper script
       * stole a frame every 2 s); after the card setup so the log has the line and
       * debug.cfg's BK_PRIO counts. BK_PRIO=0: off. */
        extern void hh_game_priority(void);
        hh_game_priority();
    }
#if defined(TARGET_MIYOO)
    { /* the Mini's CPU clock (kit miyoo_clock.c): 1500 MHz (Mini) / 1800 MHz (Mini Plus, the user's pick), below Onion's
       * own ceilings, unless debug.cfg's cpuclock= (BK_CPUCLOCK) says otherwise, 0 = leave it; put back at exit
       * (port_request_exit, atexit, the crash handler) */
        extern int miyoo_clock_boost_to(int mhz);
        (void)miyoo_clock_boost_to(-1);
    }
#endif
    hh_install_crash_handler();
    hh_map_dram();
    hh_load_rom(rom ? rom : HH_DEFAULT_ROM);
    hh_romscreen_finish(); /* the first-start screen: "Done", then black; SDL stays up for hh_plat_init */
#ifdef TARGET_HANDHELD
    hh_card_rebuild_done(); /* a rebuild request worked: its file is removed */
#endif
    snprintf(path, sizeof(path), "%s/eeprom.bin", hh_plat_save_dir());
    hh_eeprom_load(path);
    hh_plat_init();
    savestate_boot_prompt(); /* RESUME GAME / NEW GAME when an auto save is on the card (the screen is up) */
    hh_os_init(getenv("BK_NOPACE") == NULL);
    hh_ovl_init();
    { /* core1_main (code_0.c) and initThread_entry (initthread.c), whose thread runs at idle priority on the N64 */
        extern void sns_find_and_parse_payload(void), parallel_init(void), mainThread_create(void);
        extern OSThread *mainThread_get(void);
        sns_find_and_parse_payload();
        osInitialize();
        parallel_init();
        mainThread_create();
        osStartThread(mainThread_get());
    }
    hh_os_run();
}
