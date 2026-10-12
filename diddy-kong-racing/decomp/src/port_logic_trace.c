/**
 * Port debug (DKR_LOGIC_LOG=1): one line of game state per iteration, to find
 * the first frame where two builds (x86 proxy / ARM) diverge. Game-side file
 * so it sees the game's own structures; prints through the port's logger.
 */
#include "types.h"
#include "structs.h"
#include "objects.h"

extern s32 gCurrentRNGSeed;
extern s32 gObjectCount;
extern void port_log_always(const char *fmt, ...);

static u32 f2u(f32 f) {
    union { f32 f; u32 u; } c;
    c.f = f;
    return c.u;
}

void port_logic_trace(u32 frame, s32 updateRate) {
    Object *p = NULL;
    s32 n = 0;
    Object **racers = get_racer_objects(&n);
    if (racers != NULL && n > 0) {
        p = racers[0];
    }
    if (p != NULL) {
        port_log_always("LOGIC %u ur%d rng %08X obj %d p1 %08X %08X %08X rot %04X\n", (unsigned) frame, (int) updateRate,
                        (unsigned) gCurrentRNGSeed, (int) gObjectCount, (unsigned) f2u(p->trans.x_position),
                        (unsigned) f2u(p->trans.y_position), (unsigned) f2u(p->trans.z_position),
                        (unsigned) (u16) p->trans.rotation.y_rotation);
    } else {
        port_log_always("LOGIC %u ur%d rng %08X obj %d\n", (unsigned) frame, (int) updateRate, (unsigned) gCurrentRNGSeed,
                        (int) gObjectCount);
    }
}
