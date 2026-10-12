/**
 * ftport.c - decomp-side helpers for the port fighter registry.
 *
 * Lives in decomp/ because it needs access to the source-of-truth
 * vanilla arrays (dFTManagerDataFiles[] / dFTCommonSpecial*StatusList[] /
 * dFTCommonEntryAppearStatusIDs[] / ...) and to the per-character efManager
 * entry-effect functions called from the original ftCommonAppearSetStatus
 * switch. The port-side .cpp can't pull these headers in without dragging
 * the decomp shim headers across the C++ boundary.
 *
 * Two exports:
 *   port_fighter_seed_vanilla() - registers fkinds 0..26 from the vanilla
 *       decomp tables. Called once at PortInit.
 *   ftPortVanillaEntryMakeEffect() - the per-fkind appear-effect switch
 *       previously inlined in ftCommonAppearSetStatus. Registered as the
 *       entry_make_effect callback for every vanilla fkind during seeding.
 */

#ifdef PORT

#include <ft/fighter.h>
#include <ef/efmanager.h>
#include <sys/objman.h>          /* gGCCommonLinks */
#include <sys/objdef.h>          /* nGCCommonLinkIDFighter */
#include <sc/scsubsys/scsubsys.h>

#include "fighter_registry.h"

/* Per-character status descs (used as the ft_data row's special_descs). */
extern FTStatusDesc *dFTMainSpecialStatusDescs[];

/* Special-enter dispatch tables (one function per fkind). */
extern void (*dFTCommonSpecialNStatusList[])(GObj *);
extern void (*dFTCommonSpecialHiStatusList[])(GObj *);
extern void (*dFTCommonSpecialLwStatusList[])(GObj *);
extern void (*dFTCommonSpecialAirNStatusList[])(GObj *);
extern void (*dFTCommonSpecialAirHiStatusList[])(GObj *);
extern void (*dFTCommonSpecialAirLwStatusList[])(GObj *);

/* Match-entry status IDs (one [R,L] pair per fkind). */
extern s32 dFTCommonEntryAppearStatusIDs[][2];

/* Per-fkind costume rows. */
extern FTCostume dFTParamCostumeIDs[];

/* Per-fkind float scale. */
extern f32 dSCSubsysFighterScales[];

/* Per-fkind base color-anim ID (damage_level offset added at read). */
extern s32 dFTParamSkeletonColAnimIDs[];

/* Per-fkind yoshi-egg damage collision descriptors. */
extern ftCommonYoshiEggDesc dFTCommonYoshiEggDamageCollDescs[];

/* Per-fkind down-bounce SFX IDs (u16). */
extern u16 dFTCommonDataDownBounceSFX[];

/* Per-fkind public-call FGM IDs (u16). */
extern u16 dFTCommonDataPublicFighterCallFGMs[];

/* Per-fkind CPU attack-list pointers. */
extern FTComputerAttack *dFTComputerAttackList[];

/* CSS attack1 motion descs - per-fkind row of 8 entries. */
extern void *dMNCharactersAttack1MotionDescs[][8];

#define PORT_VANILLA_FKIND_COUNT  27

/* Mirrors the per-fkind appear-effect switch that used to live inline in
 * ftCommonAppearSetStatus (decomp/src/ft/ftcommon/ftcommonentry.c). */
void ftPortVanillaEntryMakeEffect(FTStruct *fp)
{
    GObj *boss_target_gobj;

    switch (fp->fkind)
    {
    case nFTKindMario:
    case nFTKindLuigi:
    case nFTKindMMario:
        efManagerMarioEntryDokanMakeEffect(&fp->entry_pos, fp->fkind);
        break;

    case nFTKindFox:
        efManagerFoxEntryArwingMakeEffect(&fp->entry_pos, fp->status_vars.common.entry.lr);
        break;

    case nFTKindDonkey:
    case nFTKindGDonkey:
        efManagerDonkeyEntryTaruMakeEffect(&fp->entry_pos);
        break;

    case nFTKindSamus:
        efManagerSamusEntryPointMakeEffect(&fp->entry_pos);
        break;

    case nFTKindLink:
        efManagerLinkEntryWaveMakeEffect(&fp->entry_pos);
        efManagerLinkEntryBeamMakeEffect(&fp->entry_pos);
        break;

    case nFTKindYoshi:
        efManagerYoshiEntryEggMakeEffect(&fp->entry_pos);
        break;

    case nFTKindKirby:
        efManagerKirbyEntryStarMakeEffect(&fp->entry_pos, fp->status_vars.common.entry.lr);
        break;

    case nFTKindPikachu:
    case nFTKindPurin:
        efManagerMBallThrownMakeEffect(&fp->entry_pos, fp->status_vars.common.entry.lr);
        break;

    case nFTKindCaptain:
        if (fp->status_vars.common.entry.lr == -1)
        {
            fp->status_vars.common.entry.is_rotate = TRUE;
        }
        efManagerCaptainEntryCarMakeEffect(&fp->entry_pos, fp->status_vars.common.entry.lr);
        break;

    case nFTKindBoss:
        /* Master Hand picks its target from the live fighter link list
         * (first non-self entry) and stashes it on the boss passive
         * state. No visual effect. */
        boss_target_gobj = gGCCommonLinks[nGCCommonLinkIDFighter];

        while (boss_target_gobj != NULL)
        {
            if (boss_target_gobj != fp->fighter_gobj)
            {
                break;
            }
            else boss_target_gobj = boss_target_gobj->link_next;
        }
        fp->passive_vars.boss.p->target_gobj = boss_target_gobj;

        break;

    default:
        break;
    }
}

/* Seed slots 0..26 of the registry from the vanilla decomp arrays.
 * Mods may overwrite individual rows later; for safety, Mario (fkind 0)
 * is the fallback row consulted by accessors when a synth row left a
 * field NULL. */
void port_fighter_seed_vanilla(void)
{
    s32 fkind;
    FighterDescriptor desc;

    for (fkind = 0; fkind < PORT_VANILLA_FKIND_COUNT; fkind++)
    {
        /* Zero-init every field so untracked fields stay deterministic. */
        FTData *zero_data = NULL;
        s32 i;
        u8 *p = (u8 *)&desc;
        for (i = 0; i < (s32)sizeof(desc); i++) p[i] = 0;
        (void)zero_data;

        desc.ft_data                            = dFTManagerDataFiles[fkind];
        desc.special_descs                      = dFTMainSpecialStatusDescs[fkind];
        desc.special_descs_count                = 0;  /* vanilla never bounds-checks; synth fills this */

        desc.special_handler[PORT_FIGHTER_SPECIAL_N]      = dFTCommonSpecialNStatusList[fkind];
        desc.special_handler[PORT_FIGHTER_SPECIAL_HI]     = dFTCommonSpecialHiStatusList[fkind];
        desc.special_handler[PORT_FIGHTER_SPECIAL_LW]     = dFTCommonSpecialLwStatusList[fkind];
        desc.special_handler[PORT_FIGHTER_SPECIAL_AIR_N]  = dFTCommonSpecialAirNStatusList[fkind];
        desc.special_handler[PORT_FIGHTER_SPECIAL_AIR_HI] = dFTCommonSpecialAirHiStatusList[fkind];
        desc.special_handler[PORT_FIGHTER_SPECIAL_AIR_LW] = dFTCommonSpecialAirLwStatusList[fkind];

        desc.entry_appear_status[0] = dFTCommonEntryAppearStatusIDs[fkind][0];
        desc.entry_appear_status[1] = dFTCommonEntryAppearStatusIDs[fkind][1];
        desc.entry_make_effect      = ftPortVanillaEntryMakeEffect;

        /* D_ovl1_80390D20 is `FTOpeningDesc *[]`; vanilla code reads
         * D_ovl1_80390D20[fkind] and gets a FTOpeningDesc *. Store that
         * pointer directly so the accessor's return matches the original
         * read site one-for-one. */
        desc.opening_descs          = D_ovl1_80390D20[fkind];

        desc.costume_row            = &dFTParamCostumeIDs[fkind];
        /* dSCSubsysFighterScales has rows for the 12 playable fkinds only;
         * vanilla read sites never index it beyond nFTKindPlayableEnd. Give
         * boss/metal/polygon/giant rows a neutral 1.0 instead of reading
         * past the table. */
        desc.scale                  = (fkind <= nFTKindPlayableEnd) ? dSCSubsysFighterScales[fkind] : 1.0f;
        desc.skeleton_col_anim_base = dFTParamSkeletonColAnimIDs[fkind];
        desc.yoshi_egg_damage_coll  = &dFTCommonYoshiEggDamageCollDescs[fkind];
        desc.down_bounce_fgm        = (s32)dFTCommonDataDownBounceSFX[fkind];
        desc.public_call_fgm        = (s32)dFTCommonDataPublicFighterCallFGMs[fkind];
        desc.computer_attack_list   = dFTComputerAttackList[fkind];

        /* CSS fields seeded from mncharacters tables; specific motion id
         * lives elsewhere and is set per-character by the menu code at
         * CSS time, so the registry row just carries the per-fkind row
         * pointer for now. */
        desc.css_motion_special     = 0;
        desc.css_attack1_motion_descs = &dMNCharactersAttack1MotionDescs[fkind][0];

        /* SR engine extensions stay at NULL/0 for vanilla fkinds so the
         * accessors return the "no override" sentinel. team_costume[] is
         * a special case: 0x00 is a valid costume index, so we explicitly
         * set 0xFF (= "no team-costume override") for vanilla rows. */
        desc.team_costume[0] = 0xFFu;
        desc.team_costume[1] = 0xFFu;
        desc.team_costume[2] = 0xFFu;
        desc.team_costume[3] = 0xFFu;

        port_fighter_register(fkind, &desc);
    }
}

#endif /* PORT */
