/* Events.h -- C stand-in for BattleShip's libultraship event hooks.
 * The fork's PORT code fires a few query/notify events for mods. This port
 * has no mods, so every event is a plain payload struct that CALL_EVENT fills
 * and nothing listens to: queries come back unchanged, i.e. vanilla. */
#pragma once
#include <stdint.h>

#define DEFINE_EVENT(name, fields) \
    typedef struct name##_t { fields } name##_t; \
    static name##_t name##_ __attribute__((unused));
#define CALL_EVENT(name, ...) (name##_ = (name##_t){ __VA_ARGS__ })

DEFINE_EVENT(FighterEnvColorQueryEvent, int32_t player; uint32_t rgba;)
DEFINE_EVENT(FighterDamageDirectionApplyEvent, void *this_fp; void *attacker_fp; void *attack_coll;)
DEFINE_EVENT(FighterHitboxSlotResetEvent, int32_t player; int32_t slot;)
DEFINE_EVENT(FighterParentKindResolveEvent, int32_t fkind; int32_t resolved_fkind;)
DEFINE_EVENT(FighterRapidJabStatusQueryEvent, int32_t fkind; int32_t phase; int32_t status_id;)
