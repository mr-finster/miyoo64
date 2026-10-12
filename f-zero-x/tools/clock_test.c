/* clock_test.c: FZX_VDSOCLOCK (goldeneye-007's X37, as mario-kart-64's clock_test) -- {port_time_us} through the vDSO
 * against the CLOCK_MONOTONIC syscall. Includes game/src/port/port_main.c (its statics reachable; the game it drives is left
 * unresolved and never called). Reports which read port_clock_pick chose, checks that every read lies between the
 * syscall reads around it (to the microsecond) and never goes backwards, and times a read of each.
 *   clock_test [reads]       (clock_test.sh builds it for x86 -m32 and ARM)
 * Exit 1 on a disagreement. */
#define main fzx_port_main
#include "port_main.c"
#undef main
#define CT_READ port_time_us
#include <stdarg.h>
/* port_misc.c's logger, not linked here */
void port_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }

int main(int argc, char **argv)
{
    const int n = argc > 1 ? atoi(argv[1]) : 200000;
    const char *sw = getenv("FZX_VDSOCLOCK");
    int bad = 0, i;
    uint64_t prev = 0, t0, t1, t2;
    port_clock_pick();
    if (getenv("CT_FORCE") && sClockMode == 0 && !(sw && sw[0] == '0')) {
        sVdsoCgt = (PortCgt32)port_vdso_sym("__vdso_clock_gettime"); /* check the vDSO's readings even where it is not faster */
        if (sVdsoCgt) { sClockMode = 1; printf("clock_test: vDSO forced on for the agreement check\n"); }
    }
    printf("clock_test: FZX_VDSOCLOCK=%s, mode %s\n", sw ? sw : "(unset)", sClockMode > 0 ? "vdso" : "syscall");
    for (i = 0; i < n; i++) {
        const uint64_t a = port_clock_us_sys();
        const uint32_t b = (uint32_t)CT_READ();
        const uint64_t c = port_clock_us_sys();
        if (b < (uint32_t)a || b > (uint32_t)c || b < (uint32_t)prev) { /* no wrap within a run: 71 minutes */
            if (bad++ < 5) printf("disagree: syscall %llu, port %u, syscall %llu, previous %llu\n", (unsigned long long)a, b,
                                  (unsigned long long)c, (unsigned long long)prev);
        }
        prev = b;
    }
    t0 = port_clock_us_sys();
    for (i = 0; i < n; i++) (void)CT_READ();
    t1 = port_clock_us_sys();
    for (i = 0; i < n; i++) (void)port_clock_us_sys();
    t2 = port_clock_us_sys();
    printf("%d reads: port %.3f us each, syscall %.3f us each\n", n, (double)(t1 - t0) / n, (double)(t2 - t1) / n);
    printf("clock_test: %s (%d disagreements)\n", bad ? "FAIL" : "PASS", bad);
    return bad != 0;
}
