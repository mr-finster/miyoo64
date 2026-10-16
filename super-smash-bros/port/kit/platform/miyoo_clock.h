/* miyoo_clock: the Mini's CPU clock for the ports (the shared Miyoo kit, KM3).
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

/* The watch: miyoo_clock_boost* starts a thread that keeps what was set through a sleep and wake. Onion's
 * sleep sets the governor to powersave and back, and a Mini came back from it at 400 MHz (scaling_setspeed
 * 400000, the PLL at 400) with the game at a third of its speed and the sound starved. Once a second, and at
 * once after a wake (the thread's 250 ms tick over 1 s late: the process was stopped; or CLOCK_BOOTTIME over
 * 1 s ahead of CLOCK_MONOTONIC: the system slept), it checks the governor (the boost's userspace, else what
 * launch.sh set), scaling_setspeed and the PLL (the boost's), and cpu1/online, and re-applies what drifted
 * (governor, setspeed, then the PLL, as the boost does), logging one line:
 *     wake: setspeed 400000->1200000 kHz, clock 400->1500 MHz
 * <GAME>_WATCH (the clock's env name with WATCH for CPUCLOCK): unset = re-apply, 1 = report only, 0 = off.
 * Only on a Mini / Mini Plus. MIYOO_SYSFS_ROOT=<dir> (tests): every sysfs path under <dir>, and the boost
 * and the watch run on that tree without the PLL. miyoo_clock_restore stops it. */
/* Wakes the watch has seen, and the CLOCK_MONOTONIC ns of the last (for a stall watchdog: a stop is no stall). */
unsigned miyoo_clock_wakes(void);
long long miyoo_clock_last_wake_ns(void);


#endif
