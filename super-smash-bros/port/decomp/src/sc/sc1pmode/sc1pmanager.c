#include <gr/ground.h>
#include <ft/fighter.h>
#include <sc/scene.h>
#include <sys/dma.h>
#ifdef PORT
#include <mn/mncommon/mncongra.h>
#include <mn/mncommon/mnmessage.h>
#include <mn/mn1pmode/mn1pcontinue.h>
#include <mv/mvending/mvending.h>

/* Console reset (portSCManagerRequestReset in scmanager.c) fired inside a
 * 1P sub-scene: bail out to scManagerRunLoop instead of chaining the next
 * sub-scene against half-torn-down battle state (a mid-match reset crashed
 * in ftManagerSetupFilesAllKind otherwise). The consume re-points
 * scene_curr at the reset target, which this manager's own scene_curr
 * writes would otherwise clobber. Checked after every sub-scene call in
 * sc1PManagerUpdateScene. */
extern sb32 portSCManagerConsumeReset(void);
#define SC1P_PORT_RESET_BAIL()               \
	do                                       \
	{                                        \
		if (portSCManagerConsumeReset())     \
		{                                    \
			return;                          \
		}                                    \
	} while (0)
#else
#define SC1P_PORT_RESET_BAIL() do { } while (0)
#endif

// // // // // // // // // // // //
//                               //
//       INITIALIZED DATA        //
//                               //
// // // // // // // // // // // //

// 0x80116BF0
SYOverlay dSC1PManagerSubsysOverlay = SCMANAGER_OVERLAY_DEFINE(1);

// 0x80116C14
SYOverlay dSC1PManager1PIntroOverlay = SCMANAGER_OVERLAY_DEFINE(24);

// 0x80116C38
SYOverlay dSC1PManager1PStageClearOverlay = SCMANAGER_OVERLAY_DEFINE(56);

// 0x80116C5C
SYOverlay dSC1PManager1PChallengerOverlay = SCMANAGER_OVERLAY_DEFINE(23);

// 0x80116C80
SYOverlay dSC1PManagerEndingOverlay = SCMANAGER_OVERLAY_DEFINE(54);

// 0x80116CA4
SYOverlay dSC1PManagerMessageOverlay = SCMANAGER_OVERLAY_DEFINE(22);

#if defined(REGION_US)
// 0x80116CC8
SYOverlay dSC1PManagerCongraOverlay = SCMANAGER_OVERLAY_DEFINE(57);
#endif

// 0x80116CEC
SYOverlay dSC1PManagerStaffrollOverlay = SCMANAGER_OVERLAY_DEFINE(59);

// 0x80116D10
SYOverlay dSC1PManager1PGameOverlay = SCMANAGER_OVERLAY_DEFINE(65);

// 0x80116D34
SYOverlay dSC1PManagerObjectsOverlay = SCMANAGER_OVERLAY_DEFINE(3);

// 0x80116D58
SYOverlay dSC1PManager1PBonusStageOverlay = SCMANAGER_OVERLAY_DEFINE(6);

// 0x80116D7C
SYOverlay dSC1PManager1PContinueOverlay = SCMANAGER_OVERLAY_DEFINE(55);

// 0x80116DA0
u8 dSC1PManagerKirbyTeamModelPartIDs[/* */] =
{
    12,  7,  4,  8,
    11, 10,  5,  9,
     0,  6,  3, 13
};

// 0x80116DAC
s32 dSC1PManagerChallangerFighterKinds[/* */] = { nFTKindLuigi, nFTKindNess, nFTKindPurin, nFTKindCaptain };

// 0x80116DBC
u32 dSC1PManagerUnlockNewcomerKinds[/* */] = { nLBBackupUnlockLuigi, nLBBackupUnlockNess, nLBBackupUnlockPurin, nLBBackupUnlockCaptain };

// // // // // // // // // // // //
//                               //
//   GLOBAL / STATIC VARIABLES   //
//                               //
// // // // // // // // // // // //

// 0x80130D60
u8 sSC1PManagerScenePrev;

// 0x80130D64 - Total time (in frames) taken to complete 1P Game
u32 gSC1PManagerTotalTimeTics;

// 0x80130D68 - Total times fallen in 1P Game
s32 gSC1PManagerTotalFalls;

// 0x80130D6C - Total damage taken in 1P Game
s32 gSC1PManagerTotalDamage;

// 0x80130D70 - Starts at 0, increments by 1 each time a continue is used; lowers CP difficulty (on current stage?)
s32 gSC1PManagerLevelDrop;

// 0x80130D74 - Starts at 2, each time this reaches 0 it increments gSC1PManagerLevelDrop
u8 sSC1PManagerLevelGuard;

// 0x80130D75 - Copy ability of final Kirby on Kirby Team in 1P Game
u8 gSC1PManagerKirbyTeamFinalCopy;

// 0x80130D76
u8 gSC1PManagerKirbyTeamModelPartID;

// // // // // // // // // // // //
//                               //
//           FUNCTIONS           //
//                               //
// // // // // // // // // // // //

#ifdef PORT
/* Classic Co-op: a co-op run is active when the latched port toggle is on
 * and the classic-context CSS handed off a second human. Everything the
 * feature touches keys off this one predicate so that with the toggle off
 * (or a solo selection) every code path is byte-identical to stock. */
sb32 sc1PManagerIsCoopActive(void)
{
	extern int port_enhancement_classic_coop(void);

	return (port_enhancement_classic_coop() != 0) && (gSCManagerSceneData.coop_player2 != SCCOMMON_COOP_NO_PLAYER2);
}
#endif

// 0x800D6490
s32 sc1PManagerGetFighterKindsNum(u16 mask)
{
    s32 i, j;

    for (i = 0, j = 0; i < (sizeof(u16) * 8); i++, mask = mask >> 1)
    {
        if (mask & 1)
        {
            j++;
        }
    }
    return j;
}

// 0x800D6508
s32 sc1PManagerGetShuffledFighterKind(u16 this_mask, u16 prev_mask, s32 random)
{
    s32 fkind = -1;

    random++;

    do
    {
        fkind++;

        if ((this_mask & (1 << fkind)) && !(prev_mask & (1 << fkind)))
        {
            random--;
        }
    }
    while (random != 0);

    return fkind;
}

// 0x800D6554
s32 sc1PManagerGetShuffledKirbyCopy(u16 flags, s32 random)
{
    s32 ret = -1;

    random++;

    do
    {
        ret++;

        if (flags & (1 << ret))
        {
            random--;
        }
    }
    while (random != 0);

    return ret;
}

// 0x800D6590
void sc1PManagerTrySetChallengers(void)
{
    if
    (
        !(gSCManagerBackupData.unlock_mask & LBBACKUP_UNLOCK_MASK_NESS) && 
        (gSCManagerBackupData.spgame_difficulty >= nSC1PGameDifficultyNormal) && 
        (gSCManagerSceneData.continues_used == 0) &&
        (gSCManagerBackupData.spgame_stock_count < 3)
    )
    {
        gSCManagerSceneData.spgame_stage = nSC1PGameStageNess;
    }
#if defined(REGION_US)
    else if (!(gSCManagerBackupData.unlock_mask & LBBACKUP_UNLOCK_MASK_CAPTAIN) && (gSC1PManagerTotalTimeTics < I_MIN_TO_TICS(12)))
    {
        // Captain Falcon's unlock criteria is 12 minutes instead of the reported 20???
        gSCManagerSceneData.spgame_stage = nSC1PGameStageCaptain;
    }
#else
    else if (!(gSCManagerBackupData.unlock_mask & LBBACKUP_UNLOCK_MASK_CAPTAIN) && (gSC1PManagerTotalTimeTics < I_MIN_TO_TICS(20)))
    {
        gSCManagerSceneData.spgame_stage = nSC1PGameStageCaptain;
    }
#endif
    else if (!(gSCManagerBackupData.unlock_mask & LBBACKUP_UNLOCK_MASK_PURIN))
    {
        gSCManagerSceneData.spgame_stage = nSC1PGameStagePurin;
    }
}

// 0x800D6630
sb32 sc1PManagerCheckUnlockSoundTest(void)
{
    s32 fkind;
    u16 bonus_record_count;

    if (!(gSCManagerBackupData.unlock_mask & LBBACKUP_UNLOCK_MASK_SOUNDTEST))
    {
        for (fkind = 0, bonus_record_count = 0; fkind < ARRAY_COUNT(gSCManagerBackupData.spgame_records); fkind++)
        {
            if (gSCManagerBackupData.spgame_records[fkind].bonus1_task_count == SCBATTLE_BONUSGAME_TASK_MAX) // Check if fighter has broken all targets
            {
                bonus_record_count |= (1 << fkind);
            }
        }
        if ((bonus_record_count & LBBACKUP_CHARACTER_MASK_ALL) == LBBACKUP_CHARACTER_MASK_ALL)
        {
            for (fkind = 0, bonus_record_count = 0; fkind < ARRAY_COUNT(gSCManagerBackupData.spgame_records); fkind++)
            {
                if (gSCManagerBackupData.spgame_records[fkind].bonus2_task_count == SCBATTLE_BONUSGAME_TASK_MAX) // Check if fighter has boarded all platforms
                {
                    bonus_record_count |= (1 << fkind);
                }
            }
            if ((bonus_record_count & LBBACKUP_CHARACTER_MASK_ALL) == LBBACKUP_CHARACTER_MASK_ALL)
            {
                return TRUE;
            }
        }
    }
    return FALSE;
}

// 0x800D6738
void sc1PManagerTrySaveBackup(sb32 is_complete_spgame)
{
    sb32 is_write_data = FALSE;

    if (gSCManagerBackupData.spgame_records[gSCManagerSceneData.fkind].spgame_hiscore < gSCManagerSceneData.spgame_score)
    {
        gSCManagerBackupData.spgame_records[gSCManagerSceneData.fkind].spgame_hiscore   = gSCManagerSceneData.spgame_score;
        gSCManagerBackupData.spgame_records[gSCManagerSceneData.fkind].spgame_continues = gSCManagerSceneData.continues_used;
        gSCManagerBackupData.spgame_records[gSCManagerSceneData.fkind].spgame_total_bonuses   = gSCManagerSceneData.bonus_count;

        if (is_complete_spgame != FALSE)
        {
            gSCManagerBackupData.spgame_records[gSCManagerSceneData.fkind].spgame_best_difficulty = gSCManagerBackupData.spgame_difficulty + 1;
        }
        else gSCManagerBackupData.spgame_records[gSCManagerSceneData.fkind].spgame_best_difficulty = 0;

        is_write_data = TRUE;
    }
    if ((gSCManagerBackupData.spgame_records[gSCManagerSceneData.fkind].is_spgame_complete == FALSE) && (is_complete_spgame != 0))
    {
        gSCManagerBackupData.spgame_records[gSCManagerSceneData.fkind].is_spgame_complete = TRUE;

        is_write_data = TRUE;
    }
    if (is_write_data != FALSE)
    {
        lbBackupWrite();
    }
}

// 0x800D67DC
void sc1PManagerUpdateScene(void)
{
    s32 i, j;
    u16 this_mask;
    s32 bonus_stat_count;
    sb32 is_player_lose;
    u16 spgame_characters_complete;
    u32 bonus_stat_mask;
    s32 random;
    s32 player;
    s32 spgame_stage;

    sSC1PManagerScenePrev = gSCManagerSceneData.scene_prev;

    gSCManager1PGameBattleState.is_team_battle = TRUE;
    gSCManager1PGameBattleState.game_rules = (SCBATTLE_GAMERULE_1PGAME | SCBATTLE_GAMERULE_TIME);
    gSCManager1PGameBattleState.damage_ratio = 100;
    gSCManager1PGameBattleState.is_show_score = FALSE;
    gSCManager1PGameBattleState.is_not_teamshadows = TRUE;

    if (gSCManagerBackupData.error_flags & LBBACKUP_ERROR_1PGAMEMARIO)
    {
        gSCManagerSceneData.fkind = nFTKindMario;
        gSCManagerSceneData.costume = 0;
    }
    gSCManager1PGameBattleState.players[gSCManagerSceneData.player].handicap = FTCOMMON_HANDICAP_DEFAULT;
    gSCManager1PGameBattleState.players[gSCManagerSceneData.player].pkind = nFTPlayerKindMan;
    gSCManager1PGameBattleState.players[gSCManagerSceneData.player].team = 0;
    gSCManager1PGameBattleState.players[gSCManagerSceneData.player].shade = 0;
    gSCManager1PGameBattleState.players[gSCManagerSceneData.player].color = gSCManagerSceneData.player;
    gSCManager1PGameBattleState.players[gSCManagerSceneData.player].tag = gSCManagerSceneData.player;
    gSCManager1PGameBattleState.players[gSCManagerSceneData.player].fkind = gSCManagerSceneData.fkind;
    gSCManager1PGameBattleState.players[gSCManagerSceneData.player].costume = gSCManagerSceneData.costume;
    gSCManager1PGameBattleState.players[gSCManagerSceneData.player].stock_count = gSCManagerBackupData.spgame_stock_count;
    gSCManager1PGameBattleState.players[gSCManagerSceneData.player].is_spgame_enemy = FALSE;

    gSCManagerSceneData.spgame_score = 0;
    gSCManagerSceneData.continues_used = 0;
    gSCManagerSceneData.bonus_count = 0;

    gSC1PManagerTotalTimeTics = 0;

    gSC1PManagerTotalFalls = 0;
    gSC1PManagerTotalDamage = 0;
    gSC1PManagerLevelDrop = 0;
    sSC1PManagerLevelGuard = 2;

    player = gSCManagerSceneData.player;

    for (i = 0; i < ARRAY_COUNT(gSCManagerSceneData.ally_players); i++)
    {
        if (player == (GMCOMMON_PLAYERS_MAX - 1))
        {
            player = 0;
        }
        else player++;

        gSCManagerSceneData.ally_players[i] = player;
    }
#ifdef PORT
    if (sc1PManagerIsCoopActive())
    {
        /* Classic Co-op: P2 takes the first ally slot. Input routing is by
         * slot index (fighter descs bind &gSYControllerDevices[slot]), so
         * ally_players[0] must be re-pointed at P2's real controller port;
         * every downstream "ally slot" path then treats P2 as the ally.
         * ally_players[1] becomes the next port that is neither human, so
         * the Giant DK stage still gets its one CPU ally. */
        s32 p2_slot = gSCManagerSceneData.coop_player2;
        s32 free_port;

        gSCManagerSceneData.ally_players[0] = p2_slot;

        for (free_port = 0; free_port < GMCOMMON_PLAYERS_MAX; free_port++)
        {
            if ((free_port != gSCManagerSceneData.player) && (free_port != p2_slot))
            {
                gSCManagerSceneData.ally_players[1] = free_port;
                break;
            }
        }
        gSCManager1PGameBattleState.players[p2_slot].handicap = FTCOMMON_HANDICAP_DEFAULT;
        //gSCManager1PGameBattleState.players[p2_slot].pkind = nFTPlayerKindMan;
        // Remove the hardcoded nFTPlayerKindMan and replace it with our passed data:
        gSCManager1PGameBattleState.players[p2_slot].pkind = gSCManagerSceneData.coop_pkind2;
        // If it's a CPU, apply the difficulty level
        if (gSCManagerSceneData.coop_pkind2 == nFTPlayerKindCom) {
            gSCManager1PGameBattleState.players[p2_slot].level = gSCManagerSceneData.coop_level2;
        }

        gSCManager1PGameBattleState.players[p2_slot].team = 0;
        gSCManager1PGameBattleState.players[p2_slot].shade = gSCManagerSceneData.coop_shade2;

        // colors for teammate (human or CPU)
        if (gSCManagerSceneData.coop_pkind2 == nFTPlayerKindCom) {
            gSCManager1PGameBattleState.players[p2_slot].color = 4; // 4 = Grey CPU color
            gSCManager1PGameBattleState.players[p2_slot].tag = 4;   // 4 = CP tag
        } else {
            gSCManager1PGameBattleState.players[p2_slot].color = p2_slot;
            gSCManager1PGameBattleState.players[p2_slot].tag = p2_slot;
        }

        gSCManager1PGameBattleState.players[p2_slot].fkind = gSCManagerSceneData.coop_fkind2;
        gSCManager1PGameBattleState.players[p2_slot].costume = gSCManagerSceneData.coop_costume2;
        gSCManager1PGameBattleState.players[p2_slot].stock_count = gSCManagerBackupData.spgame_stock_count;
        gSCManager1PGameBattleState.players[p2_slot].is_spgame_enemy = FALSE;
    }
#endif
    if (gSCManagerSceneData.spgame_stage >= nSC1PGameStageChallengerStart)
    {
        goto skip_main_stages;
    }
    else
    {
        while (gSCManagerSceneData.spgame_stage <= nSC1PGameStageCommonEnd)
        {
#ifdef PORT
            /* Issue #103/#128: sc1pmanager dispatches scenes (sc1PIntro,
             * sc1PGame, sc1PStageClear, mn1pcontinue, ...) directly via
             * StartScene calls inside this loop, never returning to
             * scManagerRunLoop. That bypasses the cross-scene fighter_gobj
             * scrub at scmanager.c:943-958, so a stale gobj from the prior
             * round survives into the next round's spawn logic. Repeat the
             * same scrub here, once per round-iteration. */
            for (s32 _p = 0; _p < GMCOMMON_PLAYERS_MAX; _p++) {
                gSCManager1PGameBattleState.players[_p].fighter_gobj = NULL;
                gSCManagerVSBattleState.players[_p].fighter_gobj = NULL;
                gSCManagerTransferBattleState.players[_p].fighter_gobj = NULL;
            }
#endif
#ifdef PORT
            /* a synth fkind >= 32 makes this shift UB (wraps mod 32 on x86 and
             * wrongly excludes a vanilla ally); a synth is never in the u16 mask */
            this_mask = (gSCManagerBackupData.fighter_mask | LBBACKUP_CHARACTER_MASK_STARTER);

            if (gSCManagerSceneData.fkind < GMCOMMON_FIGHTERS_PLAYABLE_NUM)
            {
                this_mask &= ~(1 << gSCManagerSceneData.fkind);
            }
#else
            this_mask = (gSCManagerBackupData.fighter_mask | LBBACKUP_CHARACTER_MASK_STARTER) & ~(1 << gSCManagerSceneData.fkind);
#endif

            is_player_lose = FALSE;

            switch (gSCManagerSceneData.spgame_stage)
            {
            case nSC1PGameStageMario:
                this_mask &= ~1;

#ifdef PORT
                /* Co-op: P2 *is* the partner on the Mario Bros. stage —
                 * don't overwrite their slot with a random CPU ally. */
                if (sc1PManagerIsCoopActive())
                {
                    break;
                }
#endif
                gSCManager1PGameBattleState.players[gSCManagerSceneData.ally_players[0]].fkind = sc1PManagerGetShuffledFighterKind(this_mask, 0, syUtilsRandIntRange(sc1PManagerGetFighterKindsNum(this_mask)));

                if (gSCManager1PGameBattleState.players[gSCManagerSceneData.ally_players[0]].fkind == nFTKindLuigi)
                {
                    gSCManager1PGameBattleState.players[gSCManagerSceneData.ally_players[0]].costume = ftParamGetCostumeCommonID(nFTKindLuigi, 1);
                }
                else gSCManager1PGameBattleState.players[gSCManagerSceneData.ally_players[0]].costume = 0;

                gSCManager1PGameBattleState.players[gSCManagerSceneData.ally_players[0]].shade = 0;
                break;

            case nSC1PGameStageDonkey:
                random = sc1PManagerGetFighterKindsNum(this_mask);

#ifdef PORT
                /* Co-op: P2 holds ally slot 0; only the second (CPU) ally
                 * is randomized. The dup-exclusion mask reads slot 0's
                 * fkind, which is P2's character — exactly what we want. */
                if (sc1PManagerIsCoopActive())
                {
                    this_mask &= ~(1 << gSCManagerSceneData.coop_fkind2);
                    random = sc1PManagerGetFighterKindsNum(this_mask);

                    gSCManager1PGameBattleState.players[gSCManagerSceneData.ally_players[1]].fkind = sc1PManagerGetShuffledFighterKind(this_mask, 0, syUtilsRandIntRange(random));
                    gSCManager1PGameBattleState.players[gSCManagerSceneData.ally_players[1]].costume = 0;
                    gSCManager1PGameBattleState.players[gSCManagerSceneData.ally_players[1]].shade = 0;
                    break;
                }
#endif
                gSCManager1PGameBattleState.players[gSCManagerSceneData.ally_players[0]].fkind = sc1PManagerGetShuffledFighterKind(this_mask, 0, syUtilsRandIntRange(random));
                gSCManager1PGameBattleState.players[gSCManagerSceneData.ally_players[0]].costume = 0;
                gSCManager1PGameBattleState.players[gSCManagerSceneData.ally_players[0]].shade = 0;

                gSCManager1PGameBattleState.players[gSCManagerSceneData.ally_players[1]].fkind = sc1PManagerGetShuffledFighterKind(this_mask, (1 << gSCManager1PGameBattleState.players[gSCManagerSceneData.ally_players[0]].fkind), syUtilsRandIntRange(random - 1));
                gSCManager1PGameBattleState.players[gSCManagerSceneData.ally_players[1]].costume = 0;
                gSCManager1PGameBattleState.players[gSCManagerSceneData.ally_players[1]].shade = 0;
                break;

            case nSC1PGameStageKirby:
                this_mask = (gSCManagerBackupData.fighter_mask | LBBACKUP_MASK_FIGHTER(nFTKindKirby));

                gSC1PManagerKirbyTeamFinalCopy = sc1PManagerGetShuffledKirbyCopy(this_mask, syUtilsRandIntRange(sc1PManagerGetFighterKindsNum(this_mask)));

                gSC1PManagerKirbyTeamModelPartID = dSC1PManagerKirbyTeamModelPartIDs[gSC1PManagerKirbyTeamFinalCopy];
                break;
            }
            syDmaLoadOverlay(&dSC1PManagerSubsysOverlay);
            syDmaLoadOverlay(&dSC1PManager1PIntroOverlay);

            gSCManagerSceneData.scene_prev = nSCKind1PGame;
            gSCManagerSceneData.scene_curr = nSCKind1PIntro;

            sc1PIntroStartScene();
            SC1P_PORT_RESET_BAIL();

            switch (gSCManagerSceneData.spgame_stage)
            {
            case nSC1PGameStageBonus1:
            case nSC1PGameStageBonus2:
                syDmaLoadOverlay(&dSC1PManagerObjectsOverlay);
                syDmaLoadOverlay(&dSC1PManager1PBonusStageOverlay);

                gSCManagerSceneData.scene_prev = nSCKind1PGame;
                gSCManagerSceneData.scene_curr = nSCKind1PBonusStage;

                sc1PBonusStageStartScene();
                SC1P_PORT_RESET_BAIL();
                break;

            default:
                syDmaLoadOverlay(&dSC1PManagerObjectsOverlay);
                syDmaLoadOverlay(&dSC1PManager1PGameOverlay);

#ifdef PORT
                /* sc1PIntro on exit (sc1pintro.c:2035, 2053) clobbers
                 * scene_curr to nSCKindTitle before returning, so without
                 * this assignment the fight runs with scene_curr=1. The
                 * decomp's later code at sc1pmanager.c:489/546/606 writes
                 * `scene_prev = nSCKind1PGame` as if scene_curr had been
                 * nSCKind1PGame during the round — making scene_curr match
                 * that intent. Required for the C-Stick Smash / D-Pad Jump
                 * gameplay gate in controller.c (issue #97). */
                gSCManagerSceneData.scene_curr = nSCKind1PGame;
#endif
                sc1PGameStartScene();
                SC1P_PORT_RESET_BAIL();

                if (gSCManagerSceneData.spgame_stage != nSC1PGameStageBonus3)
                {
#ifdef PORT
                    /* Co-op: the run continues while either human still has
                     * stocks; only both-eliminated (or time out) loses. */
                    if (sc1PManagerIsCoopActive())
                    {
                        if (((gSCManager1PGameBattleState.players[gSCManagerSceneData.player].stock_count == -1) &&
                             (gSCManager1PGameBattleState.players[gSCManagerSceneData.coop_player2].stock_count == -1)) ||
                            (gSCManager1PGameBattleState.time_remain == 0))
                        {
                            is_player_lose = TRUE;
                        }
                    }
                    else
#endif
                    if ((gSCManager1PGameBattleState.players[gSCManagerSceneData.player].stock_count == -1) || (gSCManager1PGameBattleState.time_remain == 0))
                    {
                        is_player_lose = TRUE;
                    }
                }
                break;
            }
            if (gSCManagerSceneData.is_reset != FALSE)
            {
                gSCManagerSceneData.scene_prev = nSCKind1PGame;
                gSCManagerSceneData.scene_curr = nSCKind1PMode;

                return;
            }
            if (is_player_lose != FALSE)
            {
                syDmaLoadOverlay(&dSC1PManagerSubsysOverlay);
                syDmaLoadOverlay(&dSC1PManager1PContinueOverlay);

                mnPlayers1PGameContinueStartScene();
                SC1P_PORT_RESET_BAIL();

                if (gSCManagerSceneData.is_continue != FALSE)
                {
                    gSCManagerSceneData.continues_used++;

                    gSCManager1PGameBattleState.players[gSCManagerSceneData.player].stock_count = gSCManagerBackupData.spgame_stock_count;
#ifdef PORT
                    /* Co-op: a continue revives both players. */
                    if (sc1PManagerIsCoopActive())
                    {
                        gSCManager1PGameBattleState.players[gSCManagerSceneData.coop_player2].stock_count = gSCManagerBackupData.spgame_stock_count;
                    }
#endif
                    gSCManagerSceneData.spgame_stage--;

                    if (--sSC1PManagerLevelGuard == 0)
                    {
                        sSC1PManagerLevelGuard = 2;

                        gSC1PManagerLevelDrop++;

                        if (gSC1PManagerLevelDrop > 9)
                        {
                            gSC1PManagerLevelDrop = 9;
                        }
                    }
                }
                else
                {
                    sc1PManagerTrySaveBackup(FALSE);

                    gSCManagerSceneData.scene_prev = nSCKind1PGame;
#if defined(REGION_US)        
                    gSCManagerSceneData.scene_curr = nSCKindStartup;
#else
                    gSCManagerSceneData.scene_curr = nSCKindOpeningRoom;
#endif

                    return;
                }
            }
            else
            {
                gSC1PManagerLevelDrop = 0;
                sSC1PManagerLevelGuard = 2;

                bonus_stat_count = 0;

                for (i = 0; i < ARRAY_COUNT(gSCManagerSceneData.bonus_get_mask); i++)
                {
                    bonus_stat_mask = gSCManagerSceneData.bonus_get_mask[i];

                    for (j = 0; j < (sizeof(u32) * 8); j++, bonus_stat_mask >>= 1)
                    {
                        if (bonus_stat_mask & 1)
                        {
                            bonus_stat_count++;
                        }
                    }
                }
                gSCManagerSceneData.bonus_count += bonus_stat_count;

                syDmaLoadOverlay(&dSC1PManagerSubsysOverlay);
                syDmaLoadOverlay(&dSC1PManager1PStageClearOverlay);

                gSCManagerSceneData.scene_prev = nSCKind1PGame;
                gSCManagerSceneData.scene_curr = nSCKind1PStageClear;

                sc1PStageClearStartScene();
                SC1P_PORT_RESET_BAIL();
            }
            gSCManagerSceneData.spgame_stage++;
        }
        sc1PManagerTrySaveBackup(TRUE);

        syDmaLoadOverlay(&dSC1PManagerSubsysOverlay);
        syDmaLoadOverlay(&dSC1PManagerEndingOverlay);

        gSCManagerSceneData.scene_prev = nSCKind1PGame;
        gSCManagerSceneData.scene_curr = nSCKindEnding;

        mvEndingStartScene();
        SC1P_PORT_RESET_BAIL();

        syDmaLoadOverlay(&dSC1PManagerStaffrollOverlay);

        gSCManagerSceneData.scene_prev = nSCKind1PGame;
        gSCManagerSceneData.scene_curr = nSCKindStaffroll;

        scStaffrollStartScene();
        SC1P_PORT_RESET_BAIL();

#if defined(REGION_US)
        syDmaLoadOverlay(&dSC1PManagerCongraOverlay);

        gSCManagerSceneData.scene_prev = nSCKind1PGame;
        gSCManagerSceneData.scene_curr = nSCKindCongra;

        mnCongraStartScene();
        SC1P_PORT_RESET_BAIL();
#endif

        gSCManagerSceneData.spgame_stage--;

        sc1PManagerTrySetChallengers();
    }
skip_main_stages:
#ifdef PORT
    /* Issue #103/#128: same per-iteration scrub as in the main-stages while
     * loop above. The challenger flow (Luigi → Ness → Purin → Captain Falcon)
     * dispatches sc1PChallenger and sc1PGame directly without returning to
     * scManagerRunLoop, so without this each challenger battle inherits the
     * prior battle's stale fighter_gobj pointers. */
    for (s32 _p = 0; _p < GMCOMMON_PLAYERS_MAX; _p++) {
        gSCManager1PGameBattleState.players[_p].fighter_gobj = NULL;
        gSCManagerVSBattleState.players[_p].fighter_gobj = NULL;
        gSCManagerTransferBattleState.players[_p].fighter_gobj = NULL;
    }
#endif
    if (gSCManagerSceneData.spgame_stage >= nSC1PGameStageChallengerStart)
    {
        gSCManagerSceneData.challenger_fkind = dSC1PManagerChallangerFighterKinds[gSCManagerSceneData.spgame_stage - nSC1PGameStageChallengerStart];

        syDmaLoadOverlay(&dSC1PManagerSubsysOverlay);
        syDmaLoadOverlay(&dSC1PManager1PChallengerOverlay);

        gSCManagerSceneData.scene_prev = nSCKind1PGame;
        gSCManagerSceneData.scene_curr = nSCKind1PChallenger;

        sc1PChallengerStartScene();
        SC1P_PORT_RESET_BAIL();

        gSCManager1PGameBattleState.players[gSCManagerSceneData.player].stock_count = 0;
#ifdef PORT
        /* Co-op: the challenger duel is a single-life fight — P1 is reset to
         * one stock above. P2 plays it too (2-player challenger duels), so it
         * needs the identical reset; otherwise P2 keeps the run's full stock
         * count and, with the single-stock-icon challenger HUD hiding the
         * number, reads as "never loses a stock / infinite lives". */
        if (sc1PManagerIsCoopActive())
        {
            gSCManager1PGameBattleState.players[gSCManagerSceneData.coop_player2].stock_count = 0;
        }
#endif

        syDmaLoadOverlay(&dSC1PManagerObjectsOverlay);
        syDmaLoadOverlay(&dSC1PManager1PGameOverlay);

#ifdef PORT
        /* sc1PChallenger on exit (sc1pchallenger.c:353) sets scene_curr to
         * nSCKindTitle; restore the 1P-Game scene so the controller-input
         * gameplay gate fires during challenger battles too. */
        gSCManagerSceneData.scene_curr = nSCKind1PGame;
#endif
        sc1PGameStartScene();
        SC1P_PORT_RESET_BAIL();

        if (gSCManagerSceneData.is_reset != FALSE)
        {
            gSCManagerSceneData.scene_prev = nSCKind1PGame;
            gSCManagerSceneData.scene_curr = nSCKind1PMode;

            return;
        }
#ifdef PORT
        /* Co-op: the duel is won (challenger unlocked) if EITHER human is
         * still alive. The fight only ends on challenger-KO or both humans
         * eliminated, so any survivor means the challenger fell — even if P2
         * landed the KO while P1 was already out. The P1-only solo test below
         * would mislabel that as a loss. */
        if (((gSCManager1PGameBattleState.players[gSCManagerSceneData.player].stock_count != -1) ||
             (sc1PManagerIsCoopActive() && (gSCManager1PGameBattleState.players[gSCManagerSceneData.coop_player2].stock_count != -1))) &&
            (gSCManager1PGameBattleState.time_remain != 0))
#else
        if ((gSCManager1PGameBattleState.players[gSCManagerSceneData.player].stock_count != -1) && (gSCManager1PGameBattleState.time_remain != 0))
#endif
        {
            gSCManagerSceneData.challenger_level_drop = dSCManagerDefaultSceneData.challenger_level_drop;
            gSCManagerSceneData.unlock_messages[0] = dSC1PManagerUnlockNewcomerKinds[gSCManagerSceneData.spgame_stage - nSC1PGameStageChallengerStart];

            syDmaLoadOverlay(&dSC1PManagerSubsysOverlay);
            syDmaLoadOverlay(&dSC1PManagerMessageOverlay);

            gSCManagerSceneData.scene_prev = nSCKind1PGame;
            gSCManagerSceneData.scene_curr = nSCKindMessage;

            mnMessageStartScene();
            SC1P_PORT_RESET_BAIL();
        }
        else if (gSCManagerSceneData.challenger_level_drop < 9)
        {
            gSCManagerSceneData.challenger_level_drop += 2;
        }
        if (gSCManagerSceneData.spgame_stage == nSC1PGameStageLuigi)
        {
            gSCManagerSceneData.scene_prev = nSCKind1PBonusStage;
            gSCManagerSceneData.scene_curr = nSCKind1PBonus1Players;
            return;
        }
    }
    if (!(gSCManagerBackupData.unlock_mask & LBBACKUP_UNLOCK_MASK_INISHIE))
    {
        if ((gSCManagerBackupData.ground_mask & LBBACKUP_GROUND_MASK_ALL) == LBBACKUP_GROUND_MASK_ALL)
        {
            for (i = 0, spgame_characters_complete = 0; i < ARRAY_COUNT(gSCManagerBackupData.spgame_records); i++)
            {
                if (gSCManagerBackupData.spgame_records[i].is_spgame_complete != FALSE)
                {
                    spgame_characters_complete |= (1 << i);
                }
            }
            if ((spgame_characters_complete & LBBACKUP_CHARACTER_MASK_STARTER) == LBBACKUP_CHARACTER_MASK_STARTER)
            {
                gSCManagerSceneData.unlock_messages[0] = nLBBackupUnlockInishie;

                syDmaLoadOverlay(&dSC1PManagerSubsysOverlay);
                syDmaLoadOverlay(&dSC1PManagerMessageOverlay);

                gSCManagerSceneData.scene_prev = nSCKind1PGame;
                gSCManagerSceneData.scene_curr = nSCKindMessage;

                mnMessageStartScene();
                SC1P_PORT_RESET_BAIL();
            }
        }
    }
    gSCManagerSceneData.scene_prev = nSCKind1PGame;
#if defined(REGION_US)        
    gSCManagerSceneData.scene_curr = nSCKindStartup;
#else
    gSCManagerSceneData.scene_curr = nSCKindOpeningRoom;
#endif
}
