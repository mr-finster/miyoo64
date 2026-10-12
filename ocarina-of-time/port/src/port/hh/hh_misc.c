/* Small leftovers: microcode placeholders, first-boot hook (kanji index: hh_kanread.c). */
#include <stdio.h>

#include "ultra64.h"

/* The game only passes the RSP microcode addresses along in OSTask structs; the
 * port interprets display lists and audio command lists itself. */
u64 aspMainTextStart[1];
u64 aspMainDataStart[1];
u64 aspMainDataEnd[1];
u64 njpgdspMainTextStart[1];
u64 njpgdspMainDataStart[1];

/* M2 bakes the packed asset file on the host (Makefile.handheld); the device-side
 * first-boot builder arrives with M3. */
s32 OotPspAssetBuilder_Ensure(void) {
    return 1;
}
