/* miyoo_clock: the Mini's CPU clock (see miyoo_clock.h). The PLL programming follows the register
 * sequence Onion's cpuclock tool uses (OnionUI src/cpuclock): the CPU clock comes from the MPLL's
 * low-pass-filter (LPF) frequency synthesiser, LPF = 432 MHz * 2^19 / (clk / 16 * post_div / 2). */
#if defined(MIYOO)
#include "miyoo_clock.h"
#if defined(__has_include)
#if __has_include("savestate.h")
#include "savestate.h"
#endif
#endif
#ifndef SAVESTATE_EXCLUDE
#define SAVESTATE_EXCLUDE
#endif
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MIYOO_CLOCK_ENV
#if defined(HH_ENV)
#define MIYOO_CLOCK_ENV HH_ENV("CPUCLOCK")
#else
#define MIYOO_CLOCK_ENV "MIYOO_CPUCLOCK"
#endif
#endif

#define RIU_BASE 0x1F000000u
#define MPLL_BASE (RIU_BASE + 0x103000u * 2)
#define MPLL_SIZE 0x1000u
#define GOV "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"
#define SETSPEED "/sys/devices/system/cpu/cpufreq/policy0/scaling_setspeed"
static const uint64_t kDivSrc = 432000000ull * 524288;
/* Defaults with margin, not Onion's ceilings (the user, 2026-10-10: "don't overclock too much to the
 * limit"; then 1800 on the Plus, the user's pick): Onion asks for up to 1900 (Plus) / 1600 (Mini) for its
 * heaviest core. */
#define MIYOO_CLOCK_PLUS 1800
#define MIYOO_CLOCK_MINI 1500

static int sSavedMHz SAVESTATE_EXCLUDE;         /* 0: nothing changed */
static char sSavedGov[32] SAVESTATE_EXCLUDE;

/* 283 Mini, 354 Mini Plus, 0 unknown (left alone). */
static int model(void) {
    int id = 0;
    FILE *f = fopen("/tmp/deviceModel", "r");   /* Onion writes it at boot */
    if (f) { if (fscanf(f, "%d", &id) != 1) id = 0; fclose(f); }
    if (id == 283 || id == 354) return id;
    struct stat st;
    if (stat("/customer/app/axp_test", &st) == 0) return 354;     /* the Plus's PMIC tool */
    if (stat("/customer/app/MainUI", &st) == 0) return 283;       /* a stock Mini without it */
    return 0;
}

static volatile uint16_t *map_pll(int *fd) {
    *fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (*fd < 0) return NULL;
    void *p = mmap(NULL, MPLL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, MPLL_BASE);
    if (p == MAP_FAILED) { close(*fd); return NULL; }
    return (volatile uint16_t *) p;
}
static void unmap_pll(volatile uint16_t *p, int fd) { munmap((void *) p, MPLL_SIZE); close(fd); }

static int read_mhz(volatile uint16_t *p16) {
    volatile uint8_t *p8 = (volatile uint8_t *) p16;
    uint32_t lpf = p16[0x2A4] + ((uint32_t) p16[0x2A6] << 16);
    const uint32_t post_div = (p16[0x232] & 0x0F) + 1;
    if (!lpf) lpf = ((uint32_t) p8[0x2C2 << 1] << 16) + ((uint32_t) p8[0x2C1 << 1] << 8) + p8[0x2C0 << 1];
    if (!lpf) return 0;
    return (int) (kDivSrc / lpf * 2 / post_div * 16 / 1000000);
}

static void set_khz(volatile uint16_t *p16, uint32_t khz) {
    const uint32_t post_div = khz >= 800000 ? 2 : khz >= 400000 ? 4 : khz >= 200000 ? 8 : 16;
    const uint32_t rate = khz * 1000 / 16 * post_div / 2;
    const uint32_t lpf = (uint32_t) (kDivSrc / rate);
    uint32_t cur = (p16[0x232] & 0x0F) + 1, d = cur;
    while (post_div > d) { d <<= 1; p16[0x232] = (p16[0x232] & 0xF0) | ((d - 1) & 0x0F); }  /* slower first */
    p16[0x2A8] = 0x0000;              /* LPF off */
    p16[0x2AE] = 0x000F;              /* update count */
    p16[0x2A4] = lpf & 0xFFFF;        /* target, high slot */
    p16[0x2A6] = lpf >> 16;
    p16[0x2B0] = 0x0001;              /* LPF control */
    p16[0x2B2] |= 0x1000;             /* low -> high */
    p16[0x2A8] = 0x0001;              /* LPF on */
    for (int i = 0; i < 10000000 && !(p16[0x2BA] & 1); i++) { }   /* done (bounded) */
    p16[0x2A0] = lpf & 0xFFFF;        /* and the low slot */
    p16[0x2A2] = lpf >> 16;
    while (post_div < d) { d >>= 1; p16[0x232] = (p16[0x232] & 0xF0) | ((d - 1) & 0x0F); }  /* faster last */
}

static void put(const char *path, const char *s) {
    const int fd = open(path, O_WRONLY);
    if (fd >= 0) { (void) !write(fd, s, strlen(s)); close(fd); }
}

int miyoo_clock_mhz(void) {
    int fd;
    volatile uint16_t *p = map_pll(&fd);
    if (!p) return 0;
    const int m = read_mhz(p);
    unmap_pll(p, fd);
    return m;
}

int miyoo_clock_boost_to(int mhz) {
    const int id = model();
    const char *e = getenv(MIYOO_CLOCK_ENV);
    if (e && e[0]) mhz = atoi(e);
    else if (mhz < 0) mhz = id == 354 ? MIYOO_CLOCK_PLUS : MIYOO_CLOCK_MINI;
    if (mhz == 0) { fprintf(stderr, "clock: left alone (%s=0)\n", MIYOO_CLOCK_ENV); return 0; }
    if (!id) { fprintf(stderr, "clock: left alone (not a Mini / Mini Plus)\n"); return 0; }
    if (mhz < 400) mhz = 400;
    if (mhz > 2000) mhz = 2000;
    int fd;
    volatile uint16_t *p = map_pll(&fd);
    if (!p) { fprintf(stderr, "clock: left alone (no /dev/mem)\n"); return 0; }
    const int was = read_mhz(p);
    if (!sSavedMHz) {
        sSavedMHz = was > 0 ? was : 1200;
        FILE *g = fopen(GOV, "r");
        if (g) { if (fscanf(g, "%31s", sSavedGov) != 1) sSavedGov[0] = 0; fclose(g); }
        atexit(miyoo_clock_restore);
    }
    char khz[16];
    snprintf(khz, sizeof(khz), "%d", mhz * 1000);
    put(GOV, "userspace");            /* cpufreq keeps its hands off the PLL */
    put(SETSPEED, khz);
    set_khz(p, (uint32_t) mhz * 1000);
    const int now = read_mhz(p);
    unmap_pll(p, fd);
    fprintf(stderr, "clock: %d -> %d MHz (model %d; %s=<MHz> to change, 0 to leave it)\n", was, now, id, MIYOO_CLOCK_ENV);
    return now;
}

int miyoo_clock_boost(void) { return miyoo_clock_boost_to(-1); }

void miyoo_clock_restore(void) {
    if (!sSavedMHz) return;
    const int mhz = sSavedMHz;
    sSavedMHz = 0;
    int fd;
    volatile uint16_t *p = map_pll(&fd);
    if (p) { set_khz(p, (uint32_t) mhz * 1000); unmap_pll(p, fd); }
    char khz[16];
    snprintf(khz, sizeof(khz), "%d", mhz * 1000);
    put(SETSPEED, khz);
    if (sSavedGov[0]) put(GOV, sSavedGov);
}
#endif
