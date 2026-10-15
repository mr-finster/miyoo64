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

#endif
