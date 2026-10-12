/* The game-state hash for proving speed switches (debug; see port_state_hash). */
#include <stdio.h>
#include <stdlib.h>
#include <ultra64.h>
#include <defines.h>
#include <common_structs.h>
#include <actor_types.h>
#include "camera.h"
#include "objects.h"
#include "savestate.h"

extern Player gPlayers[];
extern Camera cameras[];
extern struct Actor gActorList[];
extern Object gObjectList[];
extern u16 gRandomSeed16;
extern s32 gRaceFrameCounter;

/* MK64_STATEHASH=<file> (debug, 2026-10-04): after every iteration one line "<iteration> <state> <race>" -- FNV-1a
 * hashes of the game's state: <state> over the players, cameras, actors, objects, the RNG and the race frame counter
 * (compare within one build: the objects hold pointers); <race> over the players, cameras, the RNG and the counter
 * only (pointer-free here, comparable across builds). For proving a speed switch leaves the game as it was. */
void port_state_hash(u32 frame) {
    static FILE *sHashF SAVESTATE_EXCLUDE;
    static int sHashInit SAVESTATE_EXCLUDE;
    if (!sHashInit) {
        const char *e = getenv("MK64_STATEHASH");
        sHashInit = 1;
        if (e && e[0]) sHashF = fopen(e, "w");
    }
    if (!sHashF) return;
    struct { const void *p; size_t n; } r[] = {
        { gPlayers, sizeof(Player) * NUM_PLAYERS }, { cameras, sizeof(Camera) * 4 }, { &gRandomSeed16, sizeof(gRandomSeed16) },
        { &gRaceFrameCounter, sizeof(gRaceFrameCounter) }, { gActorList, sizeof(struct Actor) * ACTOR_LIST_SIZE },
        { gObjectList, sizeof(Object) * OBJECT_LIST_SIZE } };
    u32 h = 2166136261u, race = 0;
    for (size_t k = 0; k < sizeof(r) / sizeof(r[0]); k++) {
        const u8 *b = (const u8 *)r[k].p;
        for (size_t i = 0; i < r[k].n; i++) h = (h ^ b[i]) * 16777619u;
        if (k == 3) race = h;
    }
    fprintf(sHashF, "%u %08x %08x\n", (unsigned)frame, (unsigned)h, (unsigned)race);
}
