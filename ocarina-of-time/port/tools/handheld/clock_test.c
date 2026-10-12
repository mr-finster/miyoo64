/* clock_test.c: port_time_us64 (src/port/hh/hh_port.c) through the vDSO against the CLOCK_MONOTONIC syscall
 * (2026-10-01, the FPS hunt; OOT_VDSOCLOCK). Includes hh_port.c. Reports whether the vDSO was found and taken,
 * checks that the two clocks agree (every vDSO read lies between the syscall reads around it, to the microsecond)
 * and never go backwards, and times a read of each.
 *   clock_test [reads]       (tools/handheld/clock_test.sh builds it for x86 -m32 and ARM)
 * Exit 1 on a disagreement. */
#include "hh_port.c"

uintptr_t gSegments[16];
int gPortShowFps;
const uint32_t hh_build_stamp = 0;

int main(int argc, char** argv) {
    const int n = argc > 1 ? atoi(argv[1]) : 200000;
    HhTs32 t;
    int bad = 0;
    uint64_t prev = 0;
    (void)port_time_us64();
    printf("vdso %s\n", sVdsoState > 0 ? "in use" : "not used (syscall)");
    for (int i = 0; i < n; i++) {
        uint64_t a, b, c;
        port_clock_syscall(&t);
        a = (uint64_t)t.s * 1000000u + (uint64_t)(t.ns / 1000);
        b = port_time_us64();
        port_clock_syscall(&t);
        c = (uint64_t)t.s * 1000000u + (uint64_t)(t.ns / 1000);
        if (b < a || b > c || b < prev) {
            if (bad++ < 5) printf("disagree: syscall %llu, port %llu, syscall %llu, previous %llu\n", (unsigned long long)a,
                                  (unsigned long long)b, (unsigned long long)c, (unsigned long long)prev);
        }
        prev = b;
    }
    {
        const uint64_t t0 = port_time_us64();
        for (int i = 0; i < n; i++) (void)port_time_us64();
        const uint64_t t1 = port_time_us64();
        for (int i = 0; i < n; i++) port_clock_syscall(&t);
        const uint64_t t2 = port_time_us64();
        printf("%d reads: port_time_us64 %.3f us each, syscall %.3f us each\n", n, (double)(t1 - t0) / n, (double)(t2 - t1) / n);
    }
    printf("clock_test: %s (%d disagreements)\n", bad ? "FAIL" : "PASS", bad);
    return bad != 0;
}
