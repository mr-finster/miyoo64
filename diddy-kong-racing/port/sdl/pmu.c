/**
 * Cortex-A7 performance counters via perf_event_open (device round 9 -> 10:
 * is the software rasterizer compute-bound or memory-bound?). Opens three
 * userspace counters -- CPU cycles, retired instructions, L1 data-cache read
 * misses -- and reads them per frame. From these the PERF log derives IPC and
 * misses-per-pixel: low IPC + high misses => memory-bound (NEON won't help,
 * texture/access layout will); high IPC => compute-bound (SIMD helps).
 *
 * Counting mode (not sampling), exclude_kernel: works unprivileged when
 * /proc/sys/kernel/perf_event_paranoid <= 2 (the handheld runs games as root,
 * so this is available). Any failure disables PMU reporting silently.
 */
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <linux/perf_event.h>
#endif

extern void port_log_always(const char *fmt, ...);

enum { PMU_CYCLES, PMU_INSNS, PMU_L1DMISS, PMU_N };
static int sFd[PMU_N];
static int sOk;

#if defined(__linux__)
static int pmu_open_one(uint32_t type, uint64_t config) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.type = type;
    a.size = sizeof(a);
    a.config = config;
    a.disabled = 0;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    long fd = syscall(SYS_perf_event_open, &a, 0 /* this thread */, -1 /* any cpu */, -1 /* no group */, 0);
    return (int)fd;
}
#endif

void pmu_init(void) {
#if defined(__linux__)
    sFd[PMU_CYCLES]  = pmu_open_one(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);
    sFd[PMU_INSNS]   = pmu_open_one(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
    sFd[PMU_L1DMISS] = pmu_open_one(PERF_TYPE_HW_CACHE,
        PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
        (PERF_COUNT_HW_CACHE_RESULT_MISS << 16));
    sOk = (sFd[PMU_CYCLES] >= 0 && sFd[PMU_INSNS] >= 0 && sFd[PMU_L1DMISS] >= 0);
    if (!sOk) {
        port_log_always("PMU: perf_event_open unavailable (errno %d) -- no counter data\n", errno);
        for (int i = 0; i < PMU_N; i++) if (sFd[i] >= 0) close(sFd[i]);
    } else {
        port_log_always("PMU: counters open (cycles, insns, L1D-miss)\n");
    }
#else
    sOk = 0;
#endif
}

int pmu_ok(void) { return sOk; }

/* Read the three raw counters into out[3] (cycles, insns, L1D-miss). */
void pmu_read(uint64_t out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!sOk) return;
    for (int i = 0; i < PMU_N; i++) {
        uint64_t v = 0;
        if (read(sFd[i], &v, sizeof(v)) == (ssize_t)sizeof(v)) out[i] = v;
    }
}
