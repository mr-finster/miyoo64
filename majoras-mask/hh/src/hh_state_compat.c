/* Save states of earlier releases (the owner's ALPHA4 progress lives in its states: early in the first cycle the
 * game itself cannot save).
 *
 * A state is the raw .game_data/.game_bss of the build that wrote it, so it only means the same thing in a build
 * that keeps every game address: functions (actor update pointers), constant tables and strings, data, the heap.
 * A pinned build (PIN_BASE=<dir> in hh/Makefile.handheld: hh/tools/mm_pin_ld.py) links every input section of the
 * base release at the base release's address; what changed moves to overflow sections above the game's .bss.
 * hh/tools/mm_pin_check.py checks the result symbol by symbol.
 *
 * What still differs: a few static pointers in .game_data to strings the linker tail-merged with port strings, which
 * it lays out a few bytes differently. hh/tools/mm_compat_fix.py lists every .game_data word whose initial value
 * differs between the two builds (build/hh-gen/mm_compat_fix.c); a loaded state of the base release gets the new
 * value wherever it still holds the old one.
 *
 * kHhCompatBuilds: the base releases whose states this build loads (their build ids, as savestate.c writes them).
 * Only a build linked against that release's map (PIN_BASE) may list it. */
#include <stdint.h>
#include <string.h>
#include "hh_game.h"

extern void port_log_always(const char *fmt, ...);

typedef struct {
    uint32_t addr, from, to;
} HhCompatFix;

/* build/hh-gen/mm_compat_fix.c (generated; empty when the build is not pinned) */
extern const char *const kHhCompatBuilds[];
extern const HhCompatFix kHhCompatFix[];
extern const uint32_t kHhCompatFixCount;

int hh_state_compat_build(const char *build) {
    for (int i = 0; kHhCompatBuilds[i] != NULL; i++) {
        if (strncmp(build, kHhCompatBuilds[i], 32) == 0) {
            return 1;
        }
    }
    return 0;
}

void hh_state_compat_fixup(const char *build) {
    uint32_t fixed = 0, kept = 0;
    for (uint32_t i = 0; i < kHhCompatFixCount; i++) {
        uint32_t *w = (uint32_t *) (uintptr_t) kHhCompatFix[i].addr;
        if (*w == kHhCompatFix[i].from) {
            *w = kHhCompatFix[i].to;
            fixed++;
        } else {
            kept++; /* the game changed it since: its value is the game's */
        }
    }
    port_log_always("savestate: a state of \"%.31s\" (an earlier release, same layout): %u words updated, %u left as the "
                    "game set them\n", build, (unsigned) fixed, (unsigned) kept);
}
