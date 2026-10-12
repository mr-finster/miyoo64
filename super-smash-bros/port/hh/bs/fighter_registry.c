/* From BattleShip port/fighter_registry.cpp (MIT, see port/NOTICE.md), converted to C:
 * a fixed table replaces the std::vector of unique_ptr rows. Only the vanilla
 * fighters are ever registered here (seeded from the decomp arrays at boot). */
#include "fighter_registry.h"
#include <string.h>
#include <stddef.h>

#define PORT_FIGHTER_MAX 64
static FighterDescriptor sRows[PORT_FIGHTER_MAX];
static unsigned char sUsed[PORT_FIGHTER_MAX];

/* Returns the row for fkind, or NULL if out of range / unregistered. */
static const FighterDescriptor *get(int fkind)
{
    if (fkind < 0 || fkind >= PORT_FIGHTER_MAX || !sUsed[fkind]) return NULL;
    return &sRows[fkind];
}

void port_fighter_register(int fkind, const FighterDescriptor *src)
{
    if (fkind < 0 || fkind >= PORT_FIGHTER_MAX || src == NULL) return;
    memcpy(&sRows[fkind], src, sizeof(FighterDescriptor));
    sUsed[fkind] = 1;
}

const FighterDescriptor *port_fighter_descriptor(int fkind)
{
    return get(fkind);
}

struct FTData *port_fighter_data(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->ft_data;
    return NULL;
}

struct FTStatusDesc *port_fighter_special_descs(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->special_descs;
    return NULL;
}

int port_fighter_special_descs_count(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->special_descs_count;
    return 0;
}

PortFTSpecialEnterFn port_fighter_special_handler(int fkind, int kind)
{
    if (kind < 0 || kind >= PORT_FIGHTER_SPECIAL_COUNT) return NULL;

    const FighterDescriptor *r = get(fkind); if (r) return r->special_handler[kind];
    return NULL;
}

/* nFTCommonStatusEntryNull is the safe sentinel for "no entry animation"
 * (decomp value: 1). Hard-coded so the header stays decomp-agnostic. */
enum { PORT_ENTRY_NULL_STATUS = 1 };

int port_fighter_entry_appear(int fkind, int entry_id)
{
    if (entry_id != 0 && entry_id != 1) return PORT_ENTRY_NULL_STATUS;
    const FighterDescriptor *r = get(fkind); if (r) return r->entry_appear_status[entry_id];
    return PORT_ENTRY_NULL_STATUS;
}

void port_fighter_entry_make_effect(int fkind, struct FTStruct *fp)
{
    const FighterDescriptor *r = get(fkind); if (r) {
        if (r->entry_make_effect != NULL) r->entry_make_effect(fp);
    }
}

struct FTOpeningDesc *port_fighter_opening_descs(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->opening_descs;
    return NULL;
}

struct FTCostume *port_fighter_costume_row(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->costume_row;
    return NULL;
}

float port_fighter_scale(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->scale;
    return 1.0f;
}

int port_fighter_skeleton_col_anim_base(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->skeleton_col_anim_base;
    return 0;
}

int port_fighter_down_bounce_fgm(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->down_bounce_fgm;
    return 0;
}

int port_fighter_public_call_fgm(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->public_call_fgm;
    return 0;
}

void *port_fighter_yoshi_egg_damage_coll(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->yoshi_egg_damage_coll;
    return NULL;
}

void *port_fighter_computer_attack_list(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->computer_attack_list;
    return NULL;
}

void port_fighter_for_each(PortFighterForEachFn cb, void *user)
{
    if (cb == NULL) return;
    for (int i = 0; i < PORT_FIGHTER_MAX; ++i) {
        if (sUsed[i]) cb(i, &sRows[i], user);
    }
}

/* SR engine-extension accessors. Each looks up the fkind row and falls
 * back to a safe sentinel (0 / NULL / safe int) when no override is
 * registered. */

int port_fighter_shield_hitlag_skip(int fkind, struct GObj *fighter_gobj, int status_id)
{
    const FighterDescriptor *r = get(fkind); if (r) {
        if (r->shield_hitlag_skip != NULL) {
            return r->shield_hitlag_skip(fighter_gobj, status_id);
        }
    }
    return 0;
}

int port_fighter_ecb_override(int fkind, struct FTStruct *fp, int next_status_id,
                               float *out_upper, float *out_middle)
{
    if (fp == NULL || out_upper == NULL || out_middle == NULL) return 0;
    /* ECB resizing is per-fighter: only the descriptor registered for THIS
     * fighter's fkind may rewrite its diamond. The old form looped over every
     * registered fighter and called each one's callback with this fp, so a mod
     * that hooked one character could resize unrelated/vanilla fighters. */
    const FighterDescriptor *r = get(fkind); if (r) {
        if (r->ecb_override != NULL) {
            return r->ecb_override(fp, next_status_id, out_upper, out_middle);
        }
    }
    return 0;
}

int port_fighter_kirby_hat_id(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->kirby_hat_id;
    return 0;
}

/* Per-player pending custom hat id storage lives in KirbyHatEngine. These
 * forward through handlers the mod installs at MOD_INIT. */
static PortKirbySetPendingHatFn s_set_pending = NULL;
static PortKirbyGetPendingHatFn s_get_pending = NULL;

void port_kirby_register_pending_hat_handlers(PortKirbySetPendingHatFn set,
                                              PortKirbyGetPendingHatFn get)
{
    s_set_pending = set;
    s_get_pending = get;
}

void port_kirby_set_pending_hat(int player, int hat_id)
{
    if (s_set_pending) s_set_pending(player, hat_id);
}

int port_kirby_get_pending_hat(int player)
{
    return s_get_pending ? s_get_pending(player) : 0;
}

/* -1 = unset; ftMainSetStatus then uses the acting fighter's own fkind. Scoped
 * to a single synth-copied-special SetStatus call by the dispatch select fn. */
static int sKirbyCopySpecialFkind = -1;

void port_kirby_set_copy_special_fkind(int fkind)
{
    sKirbyCopySpecialFkind = fkind;
}

int port_kirby_get_copy_special_fkind(void)
{
    return sKirbyCopySpecialFkind;
}

int port_fighter_crowd_chant_fgm(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->crowd_chant_fgm;
    return 0;
}

const char *port_fighter_action_string(int fkind, int action_id)
{
    const FighterDescriptor *r = get(fkind);
    if (r == NULL) return NULL;
    if (r->action_string_table == NULL) return NULL;
    int idx = action_id - r->action_string_base_action_id;
    if (idx < 0 || idx >= r->action_string_table_count) return NULL;
    return r->action_string_table[idx];
}

void *port_fighter_ai_attack_prevent_routine(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->ai_attack_prevent_routine;
    return NULL;
}

void *port_fighter_ai_recovery_routine(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->ai_recovery_routine;
    return NULL;
}

void *port_fighter_ai_attack_weight_routine(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->ai_attack_weight_routine;
    return NULL;
}

int port_fighter_remix_1p_end_bgm(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->remix_1p_end_bgm;
    return 0;
}

int port_fighter_remix_1p_ending_image_file_id(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->remix_1p_ending_image_file_id;
    return 0;
}

const unsigned char *port_fighter_default_costumes(int fkind, int *out_count)
{
    const FighterDescriptor *r = get(fkind); if (r) {
        if (out_count != NULL) *out_count = r->default_costumes_count;
        return r->default_costumes;
    }
    if (out_count != NULL) *out_count = 0;
    return NULL;
}

int port_fighter_team_costume(int fkind, int team)
{
    if (team < 0 || team >= 4) return -1;
    const FighterDescriptor *r = get(fkind); if (r) {
        unsigned char c = r->team_costume[team];
        return (c == 0xFFu) ? -1 : (int)c;
    }
    return -1;
}

const unsigned char *port_fighter_charge_smash_frames(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->charge_smash_frames;
    return NULL;
}

int port_fighter_costume_count(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->costume_count;
    return 0;
}

float port_fighter_css_spotlight_scale(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) {
        if (r->css_spotlight_scale > 0.0f) return r->css_spotlight_scale;
    }
    return 1.5f;
}

int port_fighter_custom_capture_action(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->custom_capture_action;
    return 0;
}

int port_fighter_custom_capture_dk_interrupt(int fkind, struct FTStruct *grabber_fp)
{
    const FighterDescriptor *r = get(fkind); if (r) {
        if (r->custom_capture_dk_interrupt != NULL) {
            return r->custom_capture_dk_interrupt(grabber_fp);
        }
    }
    return 0;
}

int port_fighter_results_announce_fgm(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->results_announce_fgm;
    return 0;
}

const char *port_fighter_results_name(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->results_name;
    return NULL;
}

float port_fighter_results_name_lx(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->results_name_lx;
    return 0.0f;
}

float port_fighter_results_name_scale(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->results_name_scale;
    return 1.0f;
}

float port_fighter_results_wins_lx(int fkind)
{
    const FighterDescriptor *r = get(fkind); if (r) return r->results_wins_lx;
    return 0.0f;
}

/* CE-owned FTEmblemModels blob base, refreshed every scene reset. The emblem
 * model's internal Gfx/Vtx/texture pointer tokens are wiped + re-registered on
 * each lbRelocInitSetup, so the base buffer pointer is stable but the resolved
 * model is only valid after CE has reloaded for the current generation. CE
 * passes NULL here if the reload failed, which disables the emblem draw. */
static void *s_results_emblem_base = NULL;

void port_set_results_emblem_base(void *base)
{
    s_results_emblem_base = base;
}

int port_fighter_results_emblem(int fkind, void **out_dobjdesc,
                                void **out_mobjsub, void **out_matanim)
{
    const FighterDescriptor *r = get(fkind);
    if (r == NULL || r->results_emblem_valid == 0 ||
        s_results_emblem_base == NULL)
    {
        return 0;
    }
    unsigned char *base = (unsigned char *)(s_results_emblem_base);
    if (out_dobjdesc) *out_dobjdesc = base + r->results_emblem_dobjdesc;
    if (out_mobjsub)  *out_mobjsub  = base + r->results_emblem_mobjsub;
    if (out_matanim)  *out_matanim  = base + r->results_emblem_matanim;
    return 1;
}

