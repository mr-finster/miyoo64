/* clock_test.c: PM64_VDSOCLOCK (goldeneye-007's X37, as mario-kart-64's clock_test) -- port_time_us through the vDSO
 * against the CLOCK_MONOTONIC syscall. Includes game/port/port_host.c (its statics reachable; the game it drives is
 * left unresolved and never called). Reports which read port_clock_pick chose, checks that every port_time_us read
 * lies between the syscall reads around it (to the microsecond) and never goes backwards, and times a read of each.
 *   clock_test [reads]       (tools/port/clock_test.sh builds it for x86 -m32 and ARM)
 * Exit 1 on a disagreement. */
#define main pm64_main
#include "port_host.c"
#undef main

int main(int argc, char **argv) {
    const int n = argc > 1 ? atoi(argv[1]) : 200000;
    int bad = 0;
    port_clock_pick();
    if (getenv("CT_FORCE") && sClockMode == 0 && !(getenv("PM64_VDSOCLOCK") && getenv("PM64_VDSOCLOCK")[0] == '0')) {
        sVdsoCgt = (PortCgt32) port_vdso_sym("__vdso_clock_gettime"); /* check the vDSO's readings even where it is not faster */
        if (sVdsoCgt) { sClockMode = 1; printf("clock_test: vDSO forced on for the agreement check\n"); }
    }
    printf("clock_test: PM64_VDSOCLOCK=%s, mode %s\n", getenv("PM64_VDSOCLOCK") ? getenv("PM64_VDSOCLOCK") : "(unset)",
           sClockMode > 0 ? "vdso" : "syscall");
    uint64_t prev = 0;
    for (int i = 0; i < n; i++) {
        const uint64_t a = port_clock_us_sys();
        const uint32_t b = port_time_us();
        const uint64_t c = port_clock_us_sys();
        if (b < (uint32_t) a || b > (uint32_t) c || b < (uint32_t) prev) { /* no wrap within a run: 71 minutes */
            if (bad++ < 5) printf("disagree: syscall %llu, port %u, syscall %llu, previous %llu\n", (unsigned long long) a, b,
                                  (unsigned long long) c, (unsigned long long) prev);
        }
        prev = b;
    }
    const uint64_t t0 = port_clock_us_sys();
    for (int i = 0; i < n; i++) (void) port_time_us();
    const uint64_t t1 = port_clock_us_sys();
    for (int i = 0; i < n; i++) (void) port_clock_us_sys();
    const uint64_t t2 = port_clock_us_sys();
    printf("%d reads: port_time_us %.3f us each, syscall %.3f us each\n", n, (double) (t1 - t0) / n, (double) (t2 - t1) / n);
    printf("clock_test: %s (%d disagreements)\n", bad ? "FAIL" : "PASS", bad);
    return bad != 0;
}
