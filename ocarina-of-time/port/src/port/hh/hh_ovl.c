/* Overlay reloads, as the N64 does them.
 *
 * The N64 copies an overlay from ROM each time it loads it (Overlay_Load): its .data comes back with its initial
 * values and its .bss is zeroed. Game code relies on that. The diving-game Zora in Zora's Domain (EnDivingGame)
 * keeps a static "already here" flag that only that reload clears: with every overlay linked in, the flag stayed
 * set after the first visit, so on every later visit the Zora removed himself and the Silver Scale could no longer
 * be won. Other overlays keep spawn counters and once-only flags the same way (Ganon's Tower collapse void-out,
 * EnEg; the Arwing effects' "initialised" flag, EnClearTag; ...).
 *
 * The port links every overlay in. tools/handheld/ovl_ld.py gathers each overlay's .data/.bss between symbols;
 * here the .data is copied at start-up (before any game code runs) and put back, with the .bss zeroed, at the
 * points where the N64 would load the overlay afresh:
 *   actors      the first instance after the overlay was freed (no instances left, unless
 *               ACTOROVL_ALLOC_PERSISTENT) or after the actor context was reset (a new scene);
 *   effects     the first spawn after EffectSs_InitInfo / EffectSs_ClearAll;
 *   kaleido     every KaleidoManager_LoadOvl (the pause menu and the player swap in and out of one area);
 *   game states every game state start (the N64 loads the state's overlay and frees it when the state ends).
 * The "loaded" flags and instance counts are game state (saved in save states). OOT_OVLRESET=0: off (A/B).
 * The same mechanism as the Majora's Mask port's hh_ovl.c.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "hh_game.h"
#include "ultra64.h"

extern void port_log_always(const char* fmt, ...);

#define WEAK __attribute__((weak))
#define OVL_DECL(dir) extern char __hhovl_##dir##_ds[] WEAK, __hhovl_##dir##_de[] WEAK, \
                                  __hhovl_##dir##_bs[] WEAK, __hhovl_##dir##_be[] WEAK;
#define OVL_ENTRY(dir) { #dir, __hhovl_##dir##_ds, __hhovl_##dir##_de, __hhovl_##dir##_bs, __hhovl_##dir##_be, NULL },
#define OVL_NONE { NULL, NULL, NULL, NULL, NULL, NULL },

typedef struct {
    const char* name;
    char *ds, *de, *bs, *be;
    uint8_t* snap;
} HhOvl;

/* declarations */
#define DEFINE_ACTOR(name, _e, _a, _d) OVL_DECL(ovl_##name)
#define DEFINE_ACTOR_INTERNAL(name, _e, _a, _d)
#define DEFINE_ACTOR_UNSET(_e)
#include "tables/actor_table.h"
#undef DEFINE_ACTOR
#undef DEFINE_ACTOR_INTERNAL
#undef DEFINE_ACTOR_UNSET
#define DEFINE_EFFECT_SS(name, _e) OVL_DECL(ovl_##name)
#define DEFINE_EFFECT_SS_UNSET(_e)
#include "tables/effect_ss_table.h"
#undef DEFINE_EFFECT_SS
#undef DEFINE_EFFECT_SS_UNSET
#define DEFINE_GAMESTATE(type, _e, seg) OVL_DECL(ovl_##seg) extern void type##_Init(void*);
#define DEFINE_GAMESTATE_INTERNAL(type, _e)
#include "tables/gamestate_table.h"
#undef DEFINE_GAMESTATE
#undef DEFINE_GAMESTATE_INTERNAL
OVL_DECL(ovl_kaleido_scope)
OVL_DECL(ovl_player_actor)

/* tables: actors and effects by id, the kaleido manager's two by index (KALEIDO_OVL_*), game states with their Init */
static HhOvl sActorOvl[] = {
#define DEFINE_ACTOR(name, _e, _a, _d) OVL_ENTRY(ovl_##name)
#define DEFINE_ACTOR_INTERNAL(name, _e, _a, _d) OVL_NONE
#define DEFINE_ACTOR_UNSET(_e) OVL_NONE
#include "tables/actor_table.h"
#undef DEFINE_ACTOR
#undef DEFINE_ACTOR_INTERNAL
#undef DEFINE_ACTOR_UNSET
};
static HhOvl sEffectOvl[] = {
#define DEFINE_EFFECT_SS(name, _e) OVL_ENTRY(ovl_##name)
#define DEFINE_EFFECT_SS_UNSET(_e) OVL_NONE
#include "tables/effect_ss_table.h"
#undef DEFINE_EFFECT_SS
#undef DEFINE_EFFECT_SS_UNSET
};
static HhOvl sKaleidoOvl[] = {
    OVL_ENTRY(ovl_kaleido_scope)
    OVL_ENTRY(ovl_player_actor)
};
static HhOvl sStateOvl[] = {
#define DEFINE_GAMESTATE(type, _e, seg) OVL_ENTRY(ovl_##seg)
#define DEFINE_GAMESTATE_INTERNAL(type, _e) OVL_NONE
#include "tables/gamestate_table.h"
#undef DEFINE_GAMESTATE
#undef DEFINE_GAMESTATE_INTERNAL
};
static void (*const sStateInit[])(void*) = {
#define DEFINE_GAMESTATE(type, _e, seg) type##_Init,
#define DEFINE_GAMESTATE_INTERNAL(type, _e) NULL,
#include "tables/gamestate_table.h"
#undef DEFINE_GAMESTATE
#undef DEFINE_GAMESTATE_INTERNAL
};

#define N_ACTORS (sizeof(sActorOvl) / sizeof(sActorOvl[0]))
#define N_EFFECTS (sizeof(sEffectOvl) / sizeof(sEffectOvl[0]))
#define N_KALEIDO (sizeof(sKaleidoOvl) / sizeof(sKaleidoOvl[0]))
#define N_STATES (sizeof(sStateOvl) / sizeof(sStateOvl[0]))
/* the port's numLoaded only ever counts up (every actor takes the "not an overlay" path): the N64's own count */
static u16 sActorCount[N_ACTORS] HH_GS_BSS;
static u8 sActorLoaded[N_ACTORS] HH_GS_BSS;
static u8 sEffectLoaded[N_EFFECTS] HH_GS_BSS;
static int sOn = -1;

static void snap(HhOvl* t, size_t n) {
    for (size_t i = 0; i < n; i++) {
        HhOvl* o = &t[i];
        if (o->ds == NULL || o->de <= o->ds) continue;
        o->snap = malloc((size_t)(o->de - o->ds));
        if (o->snap != NULL) memcpy(o->snap, o->ds, (size_t)(o->de - o->ds));
    }
}

/* before main: every overlay's .data still holds its initial values */
__attribute__((constructor)) static void HhOvl_Snapshot(void) {
    const char* e = getenv("OOT_OVLRESET");
    sOn = !(e != NULL && e[0] == '0');
    if (!sOn) return;
    snap(sActorOvl, N_ACTORS);
    snap(sEffectOvl, N_EFFECTS);
    snap(sKaleidoOvl, N_KALEIDO);
    snap(sStateOvl, N_STATES);
}

void HhOvl_LogInit(void) {
    size_t bytes = 0, count = 0, bss = 0, found = 0, total = 0;
    HhOvl* tabs[4] = { sActorOvl, sEffectOvl, sKaleidoOvl, sStateOvl };
    size_t ns[4] = { N_ACTORS, N_EFFECTS, N_KALEIDO, N_STATES };
    for (int t = 0; t < 4; t++) {
        for (size_t i = 0; i < ns[t]; i++) {
            const HhOvl* o = &tabs[t][i];
            if (o->name == NULL) continue;
            total++;
            if (o->ds != NULL || o->bs != NULL) found++;
            if (o->snap != NULL) { bytes += (size_t)(o->de - o->ds); count++; }
            if (o->bs != NULL && o->be > o->bs) bss += (size_t)(o->be - o->bs);
        }
    }
    port_log_always("oot: overlay reloads %s: %zu of %zu overlays found, %zu overlays' .data kept (%zu KB), .bss %zu KB\n",
                    sOn ? "on" : "off (OOT_OVLRESET=0)", found, total, count, bytes / 1024, bss / 1024);
}

static void reset(const HhOvl* o) {
    if (!sOn || o->name == NULL) return;
    if (o->snap != NULL) memcpy(o->ds, o->snap, (size_t)(o->de - o->ds));
    if (o->bs != NULL && o->be > o->bs) memset(o->bs, 0, (size_t)(o->be - o->bs));
}

/* Actor_Spawn, before the actor's profile is read: the overlay as Overlay_Load leaves it, unless still loaded */
void HhOvl_ActorLoad(s32 id) {
    if (id < 0 || (size_t)id >= N_ACTORS || sActorLoaded[id]) return;
    reset(&sActorOvl[id]);
    sActorLoaded[id] = 1;
}
/* Actor_Spawn's numLoaded++: one more instance */
void HhOvl_ActorRef(s32 id) {
    if (id >= 0 && (size_t)id < N_ACTORS && sActorCount[id] < 0xFFFF) sActorCount[id]++;
}
/* Actor_Delete (drop = 1: one instance fewer) and Actor_FreeOverlay (drop = 0, a failed spawn): with no instances
 * left the N64 frees the overlay, unless it is ACTOROVL_ALLOC_PERSISTENT (kept until the scene ends) */
void HhOvl_ActorRelease(s32 id, s32 persistent, s32 drop) {
    if (id < 0 || (size_t)id >= N_ACTORS) return;
    if (drop && sActorCount[id] > 0) sActorCount[id]--;
    if (sActorCount[id] == 0 && !persistent) sActorLoaded[id] = 0;
}
/* Actor_InitContext: a new arena, every actor overlay is loaded afresh */
void HhOvl_ActorFreeAll(void) {
    memset(sActorLoaded, 0, sizeof(sActorLoaded));
    memset(sActorCount, 0, sizeof(sActorCount));
}
void HhOvl_EffectLoad(s32 type) {
    if (type < 0 || (size_t)type >= N_EFFECTS || sEffectLoaded[type]) return;
    reset(&sEffectOvl[type]);
    sEffectLoaded[type] = 1;
}
void HhOvl_EffectFreeAll(void) {
    memset(sEffectLoaded, 0, sizeof(sEffectLoaded));
}
void HhOvl_KaleidoLoad(s32 index) {
    if (index >= 0 && (size_t)index < N_KALEIDO) reset(&sKaleidoOvl[index]);
}
void HhOvl_GameStateLoad(void* init) {
    for (size_t i = 0; i < N_STATES; i++) {
        if (sStateInit[i] != NULL && (void*)sStateInit[i] == init) {
            reset(&sStateOvl[i]);
            return;
        }
    }
}
