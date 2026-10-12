/* clock_test.c: MK64_VDSOCLOCK (2026-10-04, the final speed sweep; the GoldenEye port's X37) -- port_time_us through
 * the vDSO against the CLOCK_MONOTONIC syscall. Includes src/port/sdl/main.c (its statics reachable; the game it
 * drives is left unresolved and never called). Reports which read port_clock_pick chose, checks that every
 * port_time_us read lies between the syscall reads around it (to the microsecond) and never goes backwards, with
 * the switch on, off and ab (the A/B phase flipped every 1000 reads), and times a read of each.
 *   clock_test [reads]       (tools/handheld/clock_test.sh builds it for x86 -m32 and ARM)
 * Exit 1 on a disagreement. */
#define main mk64_main
#include "sdl/main.c" /* src/port/sdl/main.c (src/main.c is the game's) */
#undef main
#if defined(HANDHELD)
int handheld_cfg_vdsoclock(void) { return -1; } /* no settings file: MK64_VDSOCLOCK or on */
#endif

int main(int argc, char **argv) {
    const int n = argc > 1 ? atoi(argv[1]) : 200000;
    int bad = 0;
    port_clock_pick();
    if (getenv("CT_FORCE") && sClockSwitch && sClockMode == 0) { /* check the vDSO's readings even where it is not faster */
        sVdsoCgt = (PortCgt32)port_vdso_sym("__vdso_clock_gettime");
        if (sVdsoCgt) { sClockMode = 1; printf("clock_test: vDSO forced on for the agreement check\n"); }
    }
    printf("clock_test: MK64_VDSOCLOCK=%s, mode %s\n", getenv("MK64_VDSOCLOCK") ? getenv("MK64_VDSOCLOCK") : "(unset)",
           sClockMode > 0 ? "vdso" : "syscall");
    u64 prev = 0;
    for (int i = 0; i < n; i++) {
        if (i % 1000 == 0) gPortAbPhase ^= 1;
        const u64 a = port_clock_us_sys();
        const u32 b = port_time_us();
        const u64 c = port_clock_us_sys();
        if (b < (u32)a || b > (u32)c || b < (u32)prev) { /* (no wrap within a run: a u32 of microseconds is 71 minutes) */
            if (bad++ < 5) printf("disagree: syscall %llu, port %u, syscall %llu, previous %llu\n", (unsigned long long)a, b,
                                  (unsigned long long)c, (unsigned long long)prev);
        }
        prev = b;
    }
    gPortAbPhase = 1;
    {
        const u64 t0 = port_clock_us_sys();
        for (int i = 0; i < n; i++) (void)port_time_us();
        const u64 t1 = port_clock_us_sys();
        for (int i = 0; i < n; i++) (void)port_clock_us_sys();
        const u64 t2 = port_clock_us_sys();
        printf("%d reads: port_time_us %.3f us each, syscall %.3f us each\n", n, (double)(t1 - t0) / n, (double)(t2 - t1) / n);
    }
    printf("clock_test: %s (%d disagreements)\n", bad ? "FAIL" : "PASS", bad);
    return bad != 0;
}
