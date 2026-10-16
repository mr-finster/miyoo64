/* miyoo_clock: the Mini's CPU clock for the handheld ports (the shared miyoo/kit, KM3).
 *
 * The SSD202D runs at 1.2 GHz under the stock cpufreq table. Onion runs its heavy RetroArch cores
 * faster by programming the CPU PLL directly (its `cpuclock` tool; its screen recorder runs at
 * 1600 MHz, its Beetle PCE launcher at 1600 on the Mini and 1900 on the Mini Plus). The ports are CPU-bound software renderers on
 * both cores, so the clock is the most direct speed lever there is.
 *
 *     miyoo_clock_boost();     // at start, after the console log is set up: logs one line
 *     ...                      // the clock goes back by itself at exit (atexit), and the port's
 *     miyoo_clock_restore();   //   SIGTERM path calls this before it quits (safe to call twice)
 *
 * Target: <GAME>_CPUCLOCK=<MHz> (HH_ENV, or -DMIYOO_CLOCK_ENV), else the port's cfg value passed to
 * miyoo_clock_boost_to(), else a default with margin: 1500 MHz (Mini), 1800 MHz (Mini Plus) -- below
 * Onion's ceilings for its heaviest core (1600 / 1900), on the user's word: not to the limit.
 * 0 leaves the clock alone. Only on the Mini and Mini Plus (Onion's /tmp/deviceModel 283 / 354, or the
 * stock firmware's files); anything else is left untouched. Needs root (/dev/mem), as every Miyoo OS
 * runs its apps. Built only with -DMIYOO. */
#ifndef MIYOO_CLOCK_H
#define MIYOO_CLOCK_H

/* Raise the clock to the target (see above). Returns the MHz now set, or 0 if left alone. */
int miyoo_clock_boost(void);
/* The same with the port's own setting (its cfg file's value): -1 = the model's default above,
 * 0 = leave the clock alone, else MHz. The environment still wins. */
int miyoo_clock_boost_to(int mhz);
/* Put back the clock and governor found at boost time. Idempotent; a no-op if nothing was changed. */
void miyoo_clock_restore(void);
/* The CPU clock now, in MHz, read from the PLL (0 if it can't be read). */
int miyoo_clock_mhz(void);


/* The watch, for a sleep and wake (Onion's keymon sets the governor to powersave and back, and runs its
 * cpuclock): about once a second from the main loop, or at once after a wake. It checks that the governor is
 * still the one kept (the boost's userspace, else what launch.sh set: performance), the PLL still at the
 * boost's clock, and cpu1 online. mode 0 off, 1 report a drift (once per change), 2 re-apply what drifted.
 * Writes the line (e.g. "wake: governor powersave->userspace, clock 1200->1500 MHz, cpu1 0->1") to line
 * (or stderr if line is NULL); returns the drift seen: 1 governor, 2 clock, 4 cpu1. Only on a Mini / Mini
 * Plus, or with MIYOO_SYSFS_ROOT=<dir> (every sysfs path under <dir>: tests). miyoo_clock_restore stops it. */
int miyoo_clock_watch(int mode, char *line, int n);
typedef struct {
    char gov[32], online[32];
    long cur_khz, min_khz, max_khz, setspeed_khz; /* -1: unreadable */
    int cpu1;                                      /* cpu1/online, -1 unreadable */
    int pll_mhz;                                   /* 0: unreadable */
} MiyooClockState;
void miyoo_clock_read_state(MiyooClockState *st);

#endif
