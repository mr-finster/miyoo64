#include <ft/fighter.h>
#include <reloc_data.h>

#ifdef PORT
/* The rapid-jab (Attack100) entry + per-character status/input switches key on
 * fp->fkind against a vanilla whitelist; a synthetic fighter's fkind is not in
 * it, so a Captain/Fox/Link-parented synth with jab_3 never enters its parent's
 * rapid jab. Resolve the synth's PARENT kind first via the
 * FighterParentKindResolveEvent query (no-op for vanilla fighters / with no
 * listener — the payload comes back unchanged). */
#include "hooks/Events.h"
static s32 ftCommonAttack100ResolveParentKind(s32 fkind)
{
    CALL_EVENT(FighterParentKindResolveEvent, fkind, fkind);
    return FighterParentKindResolveEvent_.resolved_fkind;
}
#define FT_A100_KIND(fp) (ftCommonAttack100ResolveParentKind((fp)->fkind))
#else
#define FT_A100_KIND(fp) ((fp)->fkind)
#endif

// // // // // // // // // // // //
//                               //
//             MACROS            //
//                               //
// // // // // // // // // // // //

#define ftCommonAttack100CheckFighterKind(fp) \
(                                             \
    (FT_A100_KIND(fp) == nFTKindFox)        ||  \
    (FT_A100_KIND(fp) == nFTKindNFox)    ||  \
    (FT_A100_KIND(fp) == nFTKindLink)       ||  \
    (FT_A100_KIND(fp) == nFTKindNLink)   ||  \
    (FT_A100_KIND(fp) == nFTKindKirby)      ||  \
    (FT_A100_KIND(fp) == nFTKindNKirby)  ||  \
    (FT_A100_KIND(fp) == nFTKindPurin)      ||  \
    (FT_A100_KIND(fp) == nFTKindNPurin)  ||  \
    (FT_A100_KIND(fp) == nFTKindCaptain)    ||  \
    (FT_A100_KIND(fp) == nFTKindNCaptain)    \
)

// // // // // // // // // // // //
//                               //
//           FUNCTIONS           //
//                               //
// // // // // // // // // // // //

// 0x8014F0D0
void ftCommonAttack100StartProcUpdate(GObj *fighter_gobj)
{
    ftAnimEndCheckSetStatus(fighter_gobj, ftCommonAttack100LoopSetStatus);
}

// 0x8014F0F4
void ftCommonAttack100StartSetStatus(GObj *fighter_gobj)
{
    FTStruct *fp = ftGetStruct(fighter_gobj);
    s32 status_id;

    if (ftCommonGetCheckInterruptCommon(fighter_gobj) == FALSE)
    {
#ifdef PORT
        /* SR rapid_jab_begin_action table: a synth's begin status may differ
         * from its parent's (Sheik = Fox-aligned 0xDC, not Captain's 0xDD).
         * status_id stays -1 (vanilla switch below) with no listener. */
        {
            CALL_EVENT(FighterRapidJabStatusQueryEvent, fp->fkind, /* phase */ 0, -1);
            status_id = FighterRapidJabStatusQueryEvent_.status_id;
        }
        if (status_id < 0)
#endif
        switch (FT_A100_KIND(fp))
        {
        case nFTKindFox:
        case nFTKindNFox:
            status_id = nFTFoxStatusAttack100Start;
            break;

        case nFTKindLink:
        case nFTKindNLink:
            status_id = nFTLinkStatusAttack100Start;
            break;

        case nFTKindKirby:
        case nFTKindNKirby:
            status_id = nFTKirbyStatusAttack100Start;
            break;

        case nFTKindPurin:
        case nFTKindNPurin:
            status_id = nFTPurinStatusAttack100Start;
            break;

        case nFTKindCaptain:
        case nFTKindNCaptain:
            status_id = nFTCaptainStatusAttack100Start;
            break;
        }
        ftMainSetStatus(fighter_gobj, status_id, 0.0F, 1.0F, FTSTATUS_PRESERVE_NONE);
        ftMainPlayAnimEventsAll(fighter_gobj);

        fp->status_vars.common.attack100.is_anim_end = FALSE;
        fp->status_vars.common.attack100.is_goto_loop = FALSE;

        fp->motion_vars.flags.flag1 = 0;
        fp->motion_vars.flags.flag2 = 0;
    }
}

// 0x8014F1BC
void ftCommonAttack100LoopKirbyUpdateEffect(FTStruct *fp)
{
    Vec3f pos;

    if (fp->fkind == nFTKindKirby)
    {
        if (fp->motion_vars.flags.flag2 != 0)
        {
#ifdef PORT
            ftKirbyAttack100Effect *effect = (ftKirbyAttack100Effect*) ((uintptr_t)gFTDataKirbyMainMotion + (intptr_t)llKirbyMainMotionftKirbyAttack100Effect);
#else
            ftKirbyAttack100Effect *effect = (ftKirbyAttack100Effect*) ((uintptr_t)gFTDataKirbyMainMotion + (intptr_t)&llKirbyMainMotionftKirbyAttack100Effect);
#endif

            pos.x = effect[fp->motion_vars.flags.flag2 - 1].offset.x;
            pos.y = effect[fp->motion_vars.flags.flag2 - 1].offset.y;
            pos.z = effect[fp->motion_vars.flags.flag2 - 1].offset.z;

            gmCollisionGetFighterPartsWorldPosition(fp->joints[nFTPartsJointTopN], &pos);

            efManagerKirbyVulcanJabMakeEffect(&pos, fp->lr, effect[fp->motion_vars.flags.flag2 - 1].rotate, effect[fp->motion_vars.flags.flag2 - 1].vel, effect[fp->motion_vars.flags.flag2 - 1].add);

            fp->motion_vars.flags.flag2 = 0;
        }
    }
}

// 0x8014F2A8
void ftCommonAttack100LoopProcUpdate(GObj *fighter_gobj)
{
    FTStruct *fp = ftGetStruct(fighter_gobj);

    if ((fighter_gobj->anim_frame >= 0.0F) && (fighter_gobj->anim_frame < DObjGetStruct(fighter_gobj)->anim_speed))
    {
        fp->status_vars.common.attack100.is_anim_end = TRUE;

        ftParamSetMotionID(fp, nFTMotionAttackIDAttack100);
        ftParamSetStatUpdate(fp, fp->stat_flags.halfword);
        ftParamUpdate1PGameAttackStats(fp, 0);
    }
    if (fp->motion_vars.flags.flag1 != 0)
    {
        fp->motion_vars.flags.flag1 = 0;

        if ((fp->status_vars.common.attack100.is_anim_end != FALSE) && (fp->status_vars.common.attack100.is_goto_loop == FALSE))
        {
            ftCommonAttack100EndSetStatus(fighter_gobj);

            return;
        }
        else if (ftCommonGetCheckInterruptCommon(fighter_gobj) == FALSE)
        {
            fp->status_vars.common.attack100.is_goto_loop = FALSE;
        }
        else return;
    }
    ftCommonAttack100LoopKirbyUpdateEffect(fp);
}

// 0x8014F388
void ftCommonAttack100LoopProcInterrupt(GObj *fighter_gobj)
{
    FTStruct *fp = ftGetStruct(fighter_gobj);

    if ((fp->input.pl.button_tap & fp->input.button_mask_a) || (fp->input.pl.button_release & fp->input.button_mask_a))
    {
        fp->status_vars.common.attack100.is_goto_loop = TRUE;
    }
}

// 0x8014F3C0
void ftCommonAttack100LoopSetStatus(GObj *fighter_gobj)
{
    FTStruct *fp = ftGetStruct(fighter_gobj);
    s32 status_id;

#ifdef PORT
    /* SR rapid_jab_loop_action table (Sheik = Fox-aligned 0xDD). */
    {
        CALL_EVENT(FighterRapidJabStatusQueryEvent, fp->fkind, /* phase */ 1, -1);
        status_id = FighterRapidJabStatusQueryEvent_.status_id;
    }
    if (status_id < 0)
#endif
    switch (FT_A100_KIND(fp))
    {
    case nFTKindFox:
    case nFTKindNFox:
        status_id = nFTFoxStatusAttack100Loop;
        break;

    case nFTKindLink:
    case nFTKindNLink:
        status_id = nFTLinkStatusAttack100Loop;
        break;

    case nFTKindKirby:
    case nFTKindNKirby:
        status_id = nFTKirbyStatusAttack100Loop;
        break;

    case nFTKindPurin:
    case nFTKindNPurin:
        status_id = nFTPurinStatusAttack100Loop;
        break;

    case nFTKindCaptain:
    case nFTKindNCaptain:
        status_id = nFTCaptainStatusAttack100Loop;
        break;
    }
    ftMainSetStatus(fighter_gobj, status_id, 0.0F, 1.0F, FTSTATUS_PRESERVE_NONE);
    ftCommonAttack100LoopKirbyUpdateEffect(fp);
}

// 0x8014F45C
void ftCommonAttack100EndSetStatus(GObj *fighter_gobj)
{
    FTStruct *fp = ftGetStruct(fighter_gobj);
    s32 status_id;

#ifdef PORT
    /* SR rapid_jab_ending_action table (Sheik = Fox-aligned 0xDE). */
    {
        CALL_EVENT(FighterRapidJabStatusQueryEvent, fp->fkind, /* phase */ 2, -1);
        status_id = FighterRapidJabStatusQueryEvent_.status_id;
    }
    if (status_id < 0)
#endif
    switch (FT_A100_KIND(fp))
    {
    case nFTKindFox:
    case nFTKindNFox:
        status_id = nFTFoxStatusAttack100End;
        break;

    case nFTKindLink:
    case nFTKindNLink:
        status_id = nFTLinkStatusAttack100End;
        break;

    case nFTKindKirby:
    case nFTKindNKirby:
        status_id = nFTKirbyStatusAttack100End;
        break;

    case nFTKindPurin:
    case nFTKindNPurin:
        status_id = nFTPurinStatusAttack100End;
        break;

    case nFTKindCaptain:
    case nFTKindNCaptain:
        status_id = nFTCaptainStatusAttack100End;
        break;
    }
    ftMainSetStatus(fighter_gobj, status_id, 0.0F, 1.0F, FTSTATUS_PRESERVE_NONE);
}

// 0x8014F4EC
sb32 ftCommonAttack100StartCheckInterruptCommon(GObj *fighter_gobj)
{
    FTStruct *fp = ftGetStruct(fighter_gobj);
    s32 status_id;
    s32 inputs_min;

    if (!(ftCommonAttack100CheckFighterKind(fp)))
    {
        return FALSE;
    }
    if ((fp->input.pl.button_tap & fp->input.button_mask_a) || (fp->input.pl.button_release & fp->input.button_mask_a))
    {
        /*
         * A frame-perfect A press (tapped on one frame and immediately released on the next)
         * registers twice due to the way button_release works. 
         */ 

        fp->attack1_input_count++;

        switch (FT_A100_KIND(fp))
        {
        case nFTKindFox:
        case nFTKindNFox:
            inputs_min = 4;
            status_id = nFTCommonStatusAttack12;
            break;

        case nFTKindLink:
        case nFTKindNLink:
            inputs_min = 5;
            status_id = nFTCommonStatusAttack12;
            break;

        case nFTKindKirby:
        case nFTKindNKirby:
            inputs_min = 4;
            status_id = nFTCommonStatusAttack12;
            break;

        case nFTKindPurin:
        case nFTKindNPurin:
            inputs_min = 4;
            status_id = nFTCommonStatusAttack12;
            break;

        case nFTKindCaptain:
        case nFTKindNCaptain:
            inputs_min = 6;
            status_id = nFTCaptainStatusAttack13;
            break;
        }
        if (fp->attack1_input_count >= inputs_min)
        {
            if ((status_id == fp->status_id) && (fp->motion_vars.flags.flag1 != 0))
            {
                ftCommonAttack100StartSetStatus(fighter_gobj);

                return TRUE;
            }
            else fp->is_goto_attack100 = TRUE;
        }
    }
    return FALSE;
}
