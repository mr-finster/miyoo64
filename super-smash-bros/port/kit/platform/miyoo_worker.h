/* miyoo_worker: the Mini's second core for the ports (the shared Miyoo kit, KM2).
 *
 * The Miyoo Mini family has two Cortex-A7 cores (SSD202D). Every port's renderer is
 * single-threaded, so on the Mini one core draws while the other mostly waits. This is one
 * persistent worker thread on the second core and a handoff fast enough to split work as small as a
 * single triangle's rows: the caller hands over a job, does its own share, then waits.
 *
 *     if (miyoo_worker_ready() && rows >= MIN_ROWS) {
 *         miyoo_worker_run(draw_rows, &bottom_half);   // the second core: rows ym..y1
 *         draw_rows(&top_half);                          // this core:       rows y0..ym
 *         miyoo_worker_wait();
 *     } else draw_rows(&all_rows);
 *
 * Rules for a job, so the picture stays the same pixel for pixel ("correct visuals over speed"):
 *  - the two shares write disjoint pixels (disjoint rows of the colour and depth buffers);
 *  - everything both read is fixed before miyoo_worker_run (state, texture decodes, LUTs) and nothing
 *    either writes is read by the other until miyoo_worker_wait returns;
 *  - per-pixel results may not depend on the order rows are visited (no running RNG or counters:
 *    keep those per share and merge after the wait).
 *
 * The handoff spins (up to ~2 ms by the clock) before sleeping on a futex, both ways, so a job of a
 * few microseconds costs well under a microsecond to hand over while a frame draws, and the worker
 * sleeps between frames. Its state is SAVESTATE_EXCLUDE (the port's savestate.h): a state load never
 * rolls it back.
 * Built only with -DMIYOO; the handheld build never sees it. <GAME>_THREADS=1 (HH_ENV) turns it off,
 * for A/B runs; with one core online it stays off by itself. */
#ifndef MIYOO_WORKER_H
#define MIYOO_WORKER_H

/* Start the worker (once; later calls return the same answer). 1: running on the second core;
 * 0: one core online, <GAME>_THREADS=1, or the thread couldn't start -- callers then do all the
 * work themselves. Logs one line either way. */
int miyoo_worker_start(void);
int miyoo_worker_ready(void);

/* Hand fn(arg) to the second core. One job at a time: call miyoo_worker_wait before the next run.
 * Without a worker (one core, <GAME>_THREADS=1) the job simply runs here, before run returns. */
void miyoo_worker_run(void (*fn)(void *), void *arg);
/* Return once the job handed over last has finished (at once if none is pending). */
void miyoo_worker_wait(void);

/* Pin the calling thread to one core (0 or 1); for the game thread, so the two never share a core. */
void miyoo_worker_pin_self(int cpu);

/* For the port's PERF line, reset by reading: jobs handed over, waits that had to sleep (the job
 * outlasted the caller's share by more than the spin), and the caller's spin count while waiting
 * (no clock reads per job: two vDSO reads a job would cost more than the handoff). */
void miyoo_worker_stats(unsigned *jobs, unsigned *sleeps, unsigned long long *spins);

#endif
