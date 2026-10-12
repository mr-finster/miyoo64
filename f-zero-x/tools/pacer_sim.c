/* Round 11: the pacer's own code (cut from port_main.c by tools/pacer_sim.sh) on a simulated
 * clock, so a hitch A/B does not depend on how busy the machine is. One line per VI:
 * vi drawn(0/1) div (ticks per picture, "4/3" = 3 pictures in 4 ticks) picture_us late_us; a summary
 * per second on stderr.
 * Usage: pacer_sim <pic_us> <hitch_from> <hitch_to> <hitch_us> <jitter_pct> <vis>
 * (a negative jitter alternates: one picture +|j|%, the next -|j|%) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t s32;
#include "pacer_snip.c"

int main(int argc, char **argv) {
    const u32 pic = (u32) atol(argv[1]), hf = (u32) atol(argv[2]), ht = (u32) atol(argv[3]), hus = (u32) atol(argv[4]);
    const int jit = atoi(argv[5]), vis = atoi(argv[6]);
    const u32 tick = 1500; /* logic + audio a tick, as on the device */
    u32 t = 0, nextVi = 0, seed = 12345;
    int hitchDone = 0, pics = 0, slips = 0;
    for (u32 vi = 1; vi <= (u32) vis; vi++) {
        u32 us = 0, late = 0;
        int drawn = 0;
        t += tick;
        if (pacer_should_draw()) {
            seed = seed * 1103515245u + 12345u;
            if (jit >= 0) us = pic + (u32) ((int) pic * jit / 100 * (int) ((seed >> 16) % 201 - 100) / 100);
            else us = pic + (u32) ((int) pic * -jit / 100 * (pics & 1 ? 1 : -1)); /* alternating */
            if (vi >= hf && (vi <= ht || !hitchDone) && hus) { us = hus; hitchDone = 1; }
            t += us;
            sSkipsInRow = 0;
            pacer_note_picture(us);
            drawn = 1;
            pics++;
        } else {
            sSkipsInRow++;
        }
        nextVi += 16667u;
        s32 wait = (s32) (nextVi - t);
        if (wait > 0) t = nextVi;
        else if (wait < -100000) { nextVi = t; slips++; }
        else late = (u32) -wait;
        sSlotLateUs = late;
        sTickEmaUs = (sTickEmaUs * 15 + tick) / 16;
        printf("%u %d %s %u %u\n", vi, drawn, kCad[sCad].name, us, late);
        if (vi % 60 == 0) { fprintf(stderr, "vi %u pics %d div %s slips %d\n", vi, pics, kCad[sCad].name, slips); pics = 0; }
    }
    return 0;
}
