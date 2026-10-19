/* Overlay reloads, as the N64 does them.
 *
 * The N64 copies an overlay from ROM each time it loads it: its .data comes back with its initial values and its
 * .bss zeroed. Game code relies on that. EnSlime (Chuchu) keeps `sSlimeTexAnim = Lib_SegmentedToVirtual(...)`, a
 * pointer into the object bank, behind a static "already done" flag; the next scene loads the object elsewhere, and
 * the stale pointer's material list jumped through garbage (the audit's multi-scene soak: Southern Swamp -> Road to
 * Southern Swamp, SIGSEGV in AnimatedMat_DrawMain). Fifteen more overlays cache desegmented pointers the same way,
 * and others keep spawn counters and once-only flags.
 *
 * The port links every overlay in. hh/tools/mm_ovl_ld.py gathers each overlay's .data/.bss between symbols; here
 * the .data is copied at start-up (before any game code runs) and put back, with the .bss zeroed, at the points
 * where the N64 would load the overlay afresh:
 *   actors      the first instance after the overlay was freed (Actor_FreeOverlay: no instances left, unless
 *               ALLOCTYPE_PERMANENT) or after the actor context was reset (a new scene);
 *   effects     the first spawn after EffectSs_InitInfo / EffectSs_ClearAll;
 *   kaleido     every KaleidoManager_LoadOvl (the pause menu and the player swap in and out of one area);
 *   game states every Overlay_LoadGameState.
 * The "loaded" flags are game state (saved in save states). MM_OVLRESET=0: off (A/B).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "hh_game.h"
#include "ultra64.h"

extern void port_log_always(const char *fmt, ...);

#define WEAK __attribute__((weak))
#define OVL_DECL(dir) extern char __hhovl_##dir##_ds[] WEAK, __hhovl_##dir##_de[] WEAK, \
                                  __hhovl_##dir##_bs[] WEAK, __hhovl_##dir##_be[] WEAK;
#define OVL_ENTRY(dir) { #dir, __hhovl_##dir##_ds, __hhovl_##dir##_de, __hhovl_##dir##_bs, __hhovl_##dir##_be, NULL },

typedef struct {
    const char *name;
    char *ds, *de, *bs, *be;
    uint8_t *snap;
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
#define DEFINE_GAMESTATE(type, _e, seg) OVL_DECL(ovl_##seg) extern void type##_Init(void *);
#define DEFINE_GAMESTATE_INTERNAL(type, _e)
#include "tables/gamestate_table.h"
#undef DEFINE_GAMESTATE
#undef DEFINE_GAMESTATE_INTERNAL
OVL_DECL(ovl_kaleido_scope)
OVL_DECL(ovl_player_actor)

/* tables: actors and effects by id, the kaleido manager's two by index, game states with their Init */
static HhOvl sActorOvl[] = {
#define DEFINE_ACTOR(name, _e, _a, _d) OVL_ENTRY(ovl_##name)
#define DEFINE_ACTOR_INTERNAL(name, _e, _a, _d) { NULL, NULL, NULL, NULL, NULL, NULL },
#define DEFINE_ACTOR_UNSET(_e) { NULL, NULL, NULL, NULL, NULL, NULL },
#include "tables/actor_table.h"
#undef DEFINE_ACTOR
#undef DEFINE_ACTOR_INTERNAL
#undef DEFINE_ACTOR_UNSET
};
static HhOvl sEffectOvl[] = {
#define DEFINE_EFFECT_SS(name, _e) OVL_ENTRY(ovl_##name)
#define DEFINE_EFFECT_SS_UNSET(_e) { NULL, NULL, NULL, NULL, NULL, NULL },
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
#define DEFINE_GAMESTATE_INTERNAL(type, _e) { NULL, NULL, NULL, NULL, NULL, NULL },
#include "tables/gamestate_table.h"
#undef DEFINE_GAMESTATE
#undef DEFINE_GAMESTATE_INTERNAL
};
static void (*const sStateInit[])(void *) = {
#define DEFINE_GAMESTATE(type, _e, seg) type##_Init,
#define DEFINE_GAMESTATE_INTERNAL(type, _e) NULL,
#include "tables/gamestate_table.h"
#undef DEFINE_GAMESTATE
#undef DEFINE_GAMESTATE_INTERNAL
};

#define N_ACTORS (sizeof(sActorOvl) / sizeof(sActorOvl[0]))
#define N_EFFECTS (sizeof(sEffectOvl) / sizeof(sEffectOvl[0]))
static u8 sActorLoaded[N_ACTORS] HH_GS_BSS;
static u8 sEffectLoaded[N_EFFECTS] HH_GS_BSS;
static int sOn = -1;
static unsigned long sResets;

static void snap(HhOvl *t, size_t n, size_t *bytes, size_t *count) {
    for (size_t i = 0; i < n; i++) {
        HhOvl *o = &t[i];
        if (o->ds == NULL || o->de <= o->ds) continue;
        o->snap = malloc((size_t) (o->de - o->ds));
        if (o->snap == NULL) continue;
        memcpy(o->snap, o->ds, (size_t) (o->de - o->ds));
        *bytes += (size_t) (o->de - o->ds);
        (*count)++;
    }
}

/* before main: every overlay's .data still holds its initial values */
__attribute__((constructor)) static void HhOvl_Snapshot(void) {
    const char *e = getenv("MM_OVLRESET");
    size_t bytes = 0, count = 0;
    sOn = !(e != NULL && e[0] == '0');
    if (!sOn) return;
    snap(sActorOvl, N_ACTORS, &bytes, &count);
    snap(sEffectOvl, N_EFFECTS, &bytes, &count);
    snap(sKaleidoOvl, 2, &bytes, &count);
    snap(sStateOvl, sizeof(sStateOvl) / sizeof(sStateOvl[0]), &bytes, &count);
}

void HhOvl_LogInit(void) {
    size_t bytes = 0, count = 0, bss = 0;
    HhOvl *tabs[4] = { sActorOvl, sEffectOvl, sKaleidoOvl, sStateOvl };
    size_t ns[4] = { N_ACTORS, N_EFFECTS, 2, sizeof(sStateOvl) / sizeof(sStateOvl[0]) };
    for (int t = 0; t < 4; t++) {
        for (size_t i = 0; i < ns[t]; i++) {
            if (tabs[t][i].snap != NULL) { bytes += (size_t) (tabs[t][i].de - tabs[t][i].ds); count++; }
            if (tabs[t][i].bs != NULL && tabs[t][i].be > tabs[t][i].bs) bss += (size_t) (tabs[t][i].be - tabs[t][i].bs);
        }
    }
    port_log_always("mm: overlay reloads %s: %zu overlays' .data kept (%zu KB), .bss %zu KB\n", sOn ? "on" : "off (MM_OVLRESET=0)",
                    count, bytes / 1024, bss / 1024);
}

static void reset(const HhOvl *o) {
    if (!sOn || o->name == NULL) return;
    if (o->snap != NULL) memcpy(o->ds, o->snap, (size_t) (o->de - o->ds));
    if (o->bs != NULL && o->be > o->bs) memset(o->bs, 0, (size_t) (o->be - o->bs));
    sResets++;
}

void HhOvl_ActorLoad(s32 id) {
    if (id < 0 || (size_t) id >= N_ACTORS || sActorLoaded[id]) return;
    reset(&sActorOvl[id]);
    sActorLoaded[id] = 1;
}
void HhOvl_ActorFree(s32 id) {
    if (id >= 0 && (size_t) id < N_ACTORS) sActorLoaded[id] = 0;
}
void HhOvl_ActorFreeAll(void) {
    memset(sActorLoaded, 0, sizeof(sActorLoaded));
}
void HhOvl_EffectLoad(s32 type) {
    if (type < 0 || (size_t) type >= N_EFFECTS || sEffectLoaded[type]) return;
    reset(&sEffectOvl[type]);
    sEffectLoaded[type] = 1;
}
void HhOvl_EffectFreeAll(void) {
    memset(sEffectLoaded, 0, sizeof(sEffectLoaded));
}
void HhOvl_KaleidoLoad(s32 index) {
    if (index >= 0 && index < 2) reset(&sKaleidoOvl[index]);
}
void HhOvl_GameStateLoad(void *init) {
    for (size_t i = 0; i < sizeof(sStateInit) / sizeof(sStateInit[0]); i++) {
        if (sStateInit[i] != NULL && (void *) sStateInit[i] == init) {
            reset(&sStateOvl[i]);
            return;
        }
    }
}
unsigned long HhOvl_Resets(void) { return sResets; }
