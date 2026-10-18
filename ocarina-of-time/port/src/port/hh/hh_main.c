/* Entry point: oot-PSP's probe main (src/port/psp/oot_psp_probe.c) without the PSP
 * specifics (clock, DVE, home menu, runtime patches -- M3 brings the device-side
 * asset rebuild). The main thread runs the gamestate loop; Graph_Update renders
 * each frame synchronously through Sched_Notify (hh_sched.c). */
#include <signal.h>
#include <unistd.h>
#ifndef HANDHELD
#include <execinfo.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "console_logo_state.h"
#include "file_select_state.h"
#include "fault.h"
#include "game.h"
#include "gfx.h"
#include "libc64/malloc.h"
#include "oot_psp_asset_loader.h"
#include "oot_psp_controls.h"
#include "oot_psp_renderer.h"
#include "play_state.h"
#include "player.h"
#include "setup_state.h"
#include "title_setup_state.h"
#include "message.h"
#include "save.h"
#include "inventory.h"
#include "z_lib.h"
#include "room.h"
#include "skin_matrix.h"
#include "sys_math.h"
#include "camera.h"
#include "bgcheck.h"
#include "scene.h"
void port_log_always(const char* fmt, ...);
#include "z_en_item00.h"
#include "item.h"
#include "sram.h"
#include "environment.h"
#include "seqcmd.h"
#include "sequence.h"
#include "controller.h"
#include "hh_game.h"
#include "savestate.h"

void OotPspGame_Init(void);
void Graph_Init(GraphicsContext* gfxCtx);
void Graph_Destroy(GraphicsContext* gfxCtx);
void Graph_Update(GraphicsContext* gfxCtx, GameState* gameState);

static volatile int sExitRequested;
static long sExitAt = -1;
static int sExitAtReached; /* the loop ended at OOT_EXIT_AT, not on a quit (the bench tells them apart) */
static long sFrames; /* this process's loop iterations: port-side, never restored by a load */
/* For gfx_sdl12.c's crash line ("FATAL: signal N at iteration I"): a plain load, signal-safe. */
u32 port_frame_count(void) {
    return (u32)sFrames;
}

/* The game loop's own state. The N64's Graph thread kept it on its stack; here it is game
 * state (hh_game.h), so a save state holds it and a load resumes the loop exactly there.
 * External linkage: the loop re-reads it after savestate_check(). */
typedef struct HhMainLoop {
    GraphicsContext gfxCtx;
    GameState* gameState;
    GameStateFunc nextInit;
    GameStateFunc curInit;
    size_t nextSize;
} HhMainLoop;
HhMainLoop gHhMainLoop HH_GS_BSS;

/* Where the loop is, for a diagnostic line in console.log: whether it still turns (iterations since
 * the last call: 0 = stuck, more = turning without drawing) and, if not, in which stage, game state
 * and scene it stopped. The stage is a
 * store of a string literal by the main thread (here, hh_renderer.c, hh_sched.c), read by another
 * thread; both are port-side, outside the save states. No handheld_log_fd: on the device stderr is
 * console.log (hh_port.c dup2), so the weak default's write(2) already lands there. */
const char* volatile gHhLoopStage = "boot";
void FileSelect_Init(GameState* thisx);
void MapSelect_Init(GameState* thisx);
unsigned handheld_loop_detail(char* buf, size_t n) {
    extern int gHhSceneId;
    if (n > 0) {
        const GameStateFunc init = gHhMainLoop.curInit; /* compared only: the state may be mid-change */
        const char* name = init == Setup_Init         ? "Setup"
                           : init == ConsoleLogo_Init ? "ConsoleLogo"
                           : init == TitleSetup_Init  ? "TitleSetup"
                           : init == FileSelect_Init  ? "FileSelect"
                           : init == Play_Init        ? "Play"
                           : init == MapSelect_Init   ? "MapSelect"
                                                      : NULL;
        char other[24];
        if (name == NULL) {
            snprintf(other, sizeof(other), "init %p", (void*)init);
            name = other;
        }
        snprintf(buf, n, "stage %s, game state %s, scene %d, iteration %u", gHhLoopStage, name, gHhSceneId,
                 (unsigned)sFrames);
    }
    return (unsigned)sFrames;
}

/* Save-state test hooks (desktop and qemu runs; the MK64 names): OOT_SAVE_AT=<frame> saves
 * slot 1, OOT_LOAD_AT=<frame> loads it, OOT_EXITSAVE_AT=<frame> takes EXIT GAME's path (auto
 * save, then quit). Frames are this process's loop iterations, which a load never restores,
 * so a hook can't fire again from its own state (MK64's MK64_LOAD_AT loop). */
/* OOT_SPAWN=<actor id>,<params>,<frame> (debug, headless tests): spawn that actor at Link's feet
 * on that loop iteration of a debug boot -- a song's effect (0x18B Oceff_Storm, params 0) without
 * an ocarina script. */
void func_8001E304(EnItem00* this, PlayState* play);

static void HhDebugSpawn(void) {
    static long sAt = -2;
    static int sId, sParams, sDist, sAngle;

    if (sAt == -2) {
        /* <actor>,<params>,<frame>[,<distance from Link>[,<angle from his facing, binary angle units>]].
         * params | 0x10000 on En_Item00 (0x15): launch it the way cut grass or a pot drops one
         * (Item_DropCollectibleRandom's no-0x8000 path), and take a heart off Link so hearts stay.
         * params | 0x20000 on any actor: Link down to 3 hearts first, so a cut grass tuft's random
         * "flexible" drop turns into a recovery heart.
         * params | 0x40000: no Deku Nuts first, so an ITEM00_NUTS (0x0C) at his feet plays the
         * get-item pose (the first nuts of a file do; later ones are just added). */
        const char* e = getenv("OOT_SPAWN");
        sAt = -1;
        if (e != NULL && sscanf(e, "%i,%i,%ld,%i,%i", &sId, &sParams, &sAt, &sDist, &sAngle) < 3) {
            sAt = -1;
        }
    }
    if (sAt >= 0 && sFrames == sAt && gHhMainLoop.curInit == Play_Init) {
        PlayState* play = (PlayState*)gHhMainLoop.gameState;
        Player* player = GET_PLAYER(play);

        if (player != NULL) {
            const s16 yaw = player->actor.shape.rot.y + (s16)sAngle;
            if (sParams & 0x20000) {
                gSaveContext.save.info.playerData.health = 0x30;
            }
            if (sParams & 0x40000) {
                gSaveContext.save.info.inventory.items[SLOT_DEKU_NUT] = ITEM_NONE;
                AMMO(ITEM_DEKU_NUT) = 0;
            }
            Actor* a = Actor_Spawn(&play->actorCtx, play, (s16)sId, player->actor.world.pos.x + Math_SinS(yaw) * sDist,
                                   player->actor.world.pos.y, player->actor.world.pos.z + Math_CosS(yaw) * sDist, 0, 0,
                                   0, (s16)(sParams & 0xFFFF));
            if (a != NULL && sId == ACTOR_EN_ITEM00 && (sParams & 0x10000)) {
                EnItem00* item = (EnItem00*)a;
                a->velocity.y = 8.0f;
                a->speed = 2.0f;
                a->gravity = -0.9f;
                a->world.rot.y = 0;
                Actor_SetScale(a, 0.0f);
                item->actionFunc = func_8001E304;
                a->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;
                a->room = -1;
                item->despawnTimer = 220;
                gSaveContext.save.info.playerData.health = gSaveContext.save.info.playerData.healthCapacity - 0x10;
            }
            printf("hh: spawned actor 0x%X params %d at frame %ld: %p\n", sId, sParams, sFrames, (void*)a);
        }
    }
}

/* OOT_WARPROOM=<room>,<x>,<y>,<z>,<yaw>,<frame>[/<next warp>...] (debug, device-state checks and
 * boss-intro tours on the desktop): at that frame, request the room (Play_Update loads it), put Link
 * there, and finish the change three frames later -- a room and position read out of a device quick
 * save. Up to 4 warps; one into the room already loaded only moves Link (a boss's trigger box). */
static void HhDebugWarpRoom(void) {
    static int sN = -1;
    static long sAt[4];
    static int sRoom[4], sYaw[4];
    static float sX[4], sY[4], sZ[4];

    if (sN < 0) {
        const char* e = getenv("OOT_WARPROOM");
        sN = 0;
        while (e != NULL && sN < 4 &&
               sscanf(e, "%i,%f,%f,%f,%i,%ld", &sRoom[sN], &sX[sN], &sY[sN], &sZ[sN], &sYaw[sN], &sAt[sN]) == 6) {
            sN++;
            e = strchr(e, '/');
            if (e != NULL) {
                e++;
            }
        }
    }
    for (int i = 0; i < sN; i++) {
        if ((sFrames == sAt[i] || sFrames == sAt[i] + 3) && gHhMainLoop.curInit == Play_Init) {
            PlayState* play = (PlayState*)gHhMainLoop.gameState;
            Player* player = GET_PLAYER(play);
            const bool move = play->roomCtx.curRoom.num != sRoom[i] || play->roomCtx.status != 0;

            if (sFrames == sAt[i]) {
                if (move) {
                    Room_RequestNewRoom(play, &play->roomCtx, sRoom[i]);
                }
            } else if (play->roomCtx.status != 0) {
                Room_FinishRoomChange(play, &play->roomCtx);
            }
            if (player != NULL) {
                player->actor.world.pos.x = player->actor.home.pos.x = player->actor.prevPos.x = sX[i];
                player->actor.world.pos.y = player->actor.home.pos.y = player->actor.prevPos.y = sY[i];
                player->actor.world.pos.z = player->actor.home.pos.z = player->actor.prevPos.z = sZ[i];
                player->actor.shape.rot.y = player->actor.world.rot.y = (s16)sYaw[i];
                player->actor.room = sRoom[i];
            }
            printf("hh: warp %d room %d at frame %ld\n", i, sRoom[i], sFrames);
        }
    }
}

/* OOT_MSG=<textId>,<frame> / OOT_OCARINA=<action>,<frame> (debug, text and HUD checks): open that
 * message, or start that ocarina action (2 = Minuet taught: the staff with its notes), in play. */
static void HhDebugMessage(void) {
    static long sMsgAt = -2, sOcaAt;
    static int sMsgId, sOcaAction;

    if (sMsgAt == -2) {
        const char* e = getenv("OOT_MSG");
        sMsgAt = sOcaAt = -1;
        if (e != NULL && sscanf(e, "%i,%ld", &sMsgId, &sMsgAt) != 2) {
            sMsgAt = -1;
        }
        e = getenv("OOT_OCARINA");
        if (e != NULL && sscanf(e, "%i,%ld", &sOcaAction, &sOcaAt) != 2) {
            sOcaAt = -1;
        }
    }
    if (gHhMainLoop.curInit != Play_Init) {
        return;
    }
    if (sMsgAt >= 0 && sFrames == sMsgAt) {
        Message_StartTextbox((PlayState*)gHhMainLoop.gameState, (u16)sMsgId, NULL);
    }
    if (sOcaAt >= 0 && sFrames == sOcaAt) {
        Message_StartOcarina((PlayState*)gHhMainLoop.gameState, (u16)sOcaAction);
    }
}

/* The game state running now and its init (hh_game.c's hh_game_save_reapply, right after a load put them back). */
GameState* HhMainLoop_Current(GameStateFunc* init) {
    *init = gHhMainLoop.curInit;
    return gHhMainLoop.gameState;
}

/* Statefix test hooks: OOT_SAVEWRITE_AT=<frame>, in the file select, writes the whole save from its copy as a
 * copy or a new file would (hh_game.c OotPsp_TestSaveWrite); OOT_MENU_AT=<frame> (handheld build) presses MENU
 * twice, so the menu opens and closes at once and a headless run goes through its mute and unmute. */
static void HhSaveStateHooks(void) {
    static long sSaveAt = -2, sLoadAt, sExitSaveAt, sSaveWriteAt, sMenuAt;

    if (sSaveAt == -2) {
        const char* e = getenv("OOT_SAVE_AT");
        sSaveAt = e != NULL ? atol(e) : -1;
        e = getenv("OOT_LOAD_AT");
        sLoadAt = e != NULL ? atol(e) : -1;
        e = getenv("OOT_EXITSAVE_AT");
        sExitSaveAt = e != NULL ? atol(e) : -1;
        e = getenv("OOT_SAVEWRITE_AT");
        sSaveWriteAt = e != NULL ? atol(e) : -1;
        e = getenv("OOT_MENU_AT");
        sMenuAt = e != NULL ? atol(e) : -1;
    }
    if (sFrames == sSaveWriteAt && gHhMainLoop.curInit == FileSelect_Init) {
        extern void OotPsp_TestSaveWrite(FileSelectState * fileSelect);
        OotPsp_TestSaveWrite((FileSelectState*)gHhMainLoop.gameState);
    }
#if defined(HANDHELD)
    if (sFrames == sMenuAt) {
        extern void port_test_menu_press(void);
        port_test_menu_press();
    }
#endif
    if (sFrames == sSaveAt) {
        savestate_request_save(1);
    }
    if (sFrames == sLoadAt) {
        savestate_request_load(1);
    }
    if (sFrames == sExitSaveAt) {
        savestate_request_exit_save();
    }
}

#ifndef HANDHELD
/* OOT_WATCHDOG=<s> (desktop): print a backtrace if the process is still running
 * after <s> seconds, and again on SIGSEGV -- gdb cannot ptrace under Rosetta. */
static void HhBacktrace(int sig) {
    void* frames[64];
    int n = backtrace(frames, 64);

    fprintf(stderr, "hh: signal %d, backtrace:\n", sig);
    backtrace_symbols_fd(frames, n, 2);
    if (sig != SIGALRM) {
        _exit(128 + sig);
    }
    alarm(5);
}
#endif

void port_request_exit(void) {
    sExitRequested = 1;
}

/* OOT_FULLSAVE=1 (debug.cfg full=1, test boots only): the debug save filled out for device
 * tests -- the user, 2026-09-25: "spawn me in hyrule field ... with all items and the game
 * 100% so i can go anywhere and use all items". Every item at its best (Ocarina of Time,
 * Longshot, bottles with a fairy, a blue potion, bugs and a Poe, the Mask of Truth, the
 * Claim Check), full ammo on every upgrade, all equipment and Biggoron's Sword, the 6
 * medallions, 3 stones, 12 songs, Stone of Agony, Gerudo's Card, 100 tokens, 20 hearts, double
 * magic and defense, every dungeon's map, compass, boss key and 9 small keys, 500 rupees,
 * Epona, the Door of Time open (age swaps at the pedestal) and the rainbow bridge. The story
 * events stay as the debug save has them. */
static void HhDebugFullSave(void) {
    Inventory* inv = &gSaveContext.save.info.inventory;
    SavePlayerData* pd = &gSaveContext.save.info.playerData;
    int i;

    inv->items[SLOT_OCARINA] = ITEM_OCARINA_OF_TIME;
    inv->items[SLOT_HOOKSHOT] = ITEM_LONGSHOT;
    inv->items[SLOT_BOTTLE_1] = ITEM_BOTTLE_FAIRY;
    inv->items[SLOT_BOTTLE_2] = ITEM_BOTTLE_POTION_BLUE;
    inv->items[SLOT_BOTTLE_3] = ITEM_BOTTLE_BUG;
    inv->items[SLOT_BOTTLE_4] = ITEM_BOTTLE_POE;
    inv->items[SLOT_TRADE_ADULT] = ITEM_CLAIM_CHECK;
    inv->items[SLOT_TRADE_CHILD] = ITEM_MASK_TRUTH;
    inv->items[SLOT_MAGIC_BEAN] = ITEM_MAGIC_BEAN;
    Inventory_ChangeUpgrade(UPG_QUIVER, 3);
    Inventory_ChangeUpgrade(UPG_BOMB_BAG, 3);
    Inventory_ChangeUpgrade(UPG_STRENGTH, 3);
    Inventory_ChangeUpgrade(UPG_SCALE, 2);
    Inventory_ChangeUpgrade(UPG_WALLET, 2);
    Inventory_ChangeUpgrade(UPG_BULLET_BAG, 3);
    Inventory_ChangeUpgrade(UPG_DEKU_STICKS, 3);
    Inventory_ChangeUpgrade(UPG_DEKU_NUTS, 3);
    inv->ammo[SLOT_DEKU_STICK] = 30;
    inv->ammo[SLOT_DEKU_NUT] = 40;
    inv->ammo[SLOT_BOMB] = 40;
    inv->ammo[SLOT_BOW] = 50;
    inv->ammo[SLOT_SLINGSHOT] = 50;
    inv->ammo[SLOT_BOMBCHU] = 50;
    inv->ammo[SLOT_MAGIC_BEAN] = 10;
    for (i = QUEST_MEDALLION_FOREST; i <= QUEST_SKULL_TOKEN; i++) {
        inv->questItems |= gBitFlags[i];
    }
    inv->gsTokens = 100;
    for (i = 0; i < (int)(sizeof(inv->dungeonItems) / sizeof(inv->dungeonItems[0])); i++) {
        inv->dungeonItems[i] = 7; /* boss key, compass, map */
    }
    for (i = 0; i < (int)(sizeof(inv->dungeonKeys) / sizeof(inv->dungeonKeys[0])); i++) {
        inv->dungeonKeys[i] = 9;
    }
    inv->defenseHearts = 20;
    pd->healthCapacity = pd->health = 20 * 0x10;
    pd->isMagicAcquired = true;
    pd->isDoubleMagicAcquired = true;
    /* As FileSelect_LoadGame starts every file: the fill target is the saved magic and level
     * and magic are 0, so Interface_Update's "Magic Start" sets the level, steps
     * magicCapacity up to 96 and fills. Setting level 2 directly skipped that: the capacity
     * stayed 0 and the meter drew only its left cap under the fill (the user's v32 photo). */
    gSaveContext.magicFillTarget = MAGIC_DOUBLE_METER;
    gSaveContext.magicCapacity = 0;
    pd->magicLevel = 0;
    pd->magic = 0;
    pd->isDoubleDefenseAcquired = true;
    pd->bgsFlag = 1;
    pd->swordHealth = 8;
    pd->rupees = 500;
    SET_EVENTCHKINF(EVENTCHKINF_EPONA_OBTAINED);
    SET_EVENTCHKINF(EVENTCHKINF_OPENED_DOOR_OF_TIME);
    SET_EVENTCHKINF(EVENTCHKINF_REVEALED_MASTER_SWORD);
    SET_EVENTCHKINF(EVENTCHKINF_CREATED_RAINBOW_BRIDGE);
}

/* OOT_ENTRANCE=<id> (debug): after Setup, start straight in that entrance with the
 * debug save, exactly as the map select's MapSelect_LoadGame does. Kokiri Forest
 * is 0xEE, Hyrule Field 0xCD, Link's house 0xBB, Market (day) 0xB1.
 * A test boot saves to its own file, oot-psp-save-debug.bin (hh_game.c), in slot 1:
 * the debug save's file number 0xFF indexed gSramSlotOffsets out of bounds, so a
 * save from the pause menu wrote the SRAM image at a garbage offset. */
static int HhDebugBootEntrance(GameStateFunc* nextInit, size_t* nextSize) {
    const char* env = getenv("OOT_ENTRANCE");

    if (env == NULL) {
        return 0;
    }
    gSaveContext.fileNum = 0xFF;
    /* Age first, as the map select does (MapSelect_Init starts child and
     * MapSelect_LoadGame calls Sram_InitDebugSave with the chosen age): the
     * debug save leaves linkAge alone and hands out the Kokiri Sword, Deku
     * Shield and slingshot only to a child. Never setting it booted ADULT Link
     * (linkAge 0) into Kokiri Forest: white sleeves and tights, gauntlets, a
     * giant in the Saria cutscene. OOT_ADULT=1 / debug.cfg adult=1 for adult. */
    env = getenv("OOT_ADULT");
    gSaveContext.save.linkAge = (env != NULL && env[0] == '1') ? LINK_AGE_ADULT : LINK_AGE_CHILD;
    env = getenv("OOT_ENTRANCE");
    Sram_InitDebugSave();
    gSaveContext.fileNum = 0; /* after the debug save's child items (they test 0xFF) */
    {
        const char* full = getenv("OOT_FULLSAVE");
        if (full != NULL && full[0] == '1') {
            HhDebugFullSave();
        }
    }
    {   /* OOT_CBUTTON=<1..3>,<item>,<slot> (debug, first-person tests): put that item on C-left,
         * C-down or C-right, e.g. 2,0x0A,9 = the hookshot on C-down. */
        const char* cb = getenv("OOT_CBUTTON");
        int b, item, slot;
        if (cb != NULL && sscanf(cb, "%i,%i,%i", &b, &item, &slot) == 3 && b >= 1 && b <= 3) {
            gSaveContext.save.info.equips.buttonItems[b] = (u8)item;
            gSaveContext.save.info.equips.cButtonSlots[b - 1] = (u8)slot;
            gSaveContext.save.info.inventory.items[slot] = (u8)item;
        }
    }
    {   /* OOT_EVENTCHK=<flag>[,<flag>...] (debug, tours): set those event flags first, e.g. a boss's
         * BEGAN_*_BATTLE, which skips Gohma's look-at-her puzzle straight to her title card. */
        const char* ev = getenv("OOT_EVENTCHK");
        while (ev != NULL && *ev != '\0') {
            char* end;
            const long f = strtol(ev, &end, 0);
            if (end == ev) {
                break;
            }
            SET_EVENTCHKINF(f);
            ev = (*end == ',') ? end + 1 : end;
        }
    }
    {   /* OOT_DAYTIME=<0..0xFFFF> (debug.cfg daytime=): the clock; the debug save
         * leaves it at 0, midnight. 0x8000 is noon, 0x4555 about 06:30. */
        const char* t = getenv("OOT_DAYTIME");
        if (t != NULL) {
            gSaveContext.save.dayTime = (u16)strtol(t, NULL, 0);
        }
    }
    {   /* OOT_CHAMBERCS=<0..5> (debug, reference checks): which sage's medallion cutscene the Chamber of Sages
         * (entrance 0x6B, adult) plays, gSaveContext.chamberCutsceneNum: 2 = Ruto's, the Water Medallion. The
         * same byte as a GameShark write on the N64 (0x8011B9E5), so both run the same scripted cutscene. */
        const char* c = getenv("OOT_CHAMBERCS");
        if (c != NULL) {
            gSaveContext.chamberCutsceneNum = (u8)strtol(c, NULL, 0);
        }
    }
    if (gSaveContext.language != LANGUAGE_JPN) {
        /* The NTSC debug save's name is katakana (リンク), which the English text
         * path decodes to garbage glyphs ("<junk>, it's so hot in here!"); a file
         * made on the file select is fine. Name it "Link" in the filename charset. */
        static const u8 kName[] = { FILENAME_UPPERCASE('L'), FILENAME_LOWERCASE('i'), FILENAME_LOWERCASE('n'),
                                    FILENAME_LOWERCASE('k'), FILENAME_SPACE,          FILENAME_SPACE,
                                    FILENAME_SPACE,          FILENAME_SPACE };

        memcpy(gSaveContext.save.info.playerData.playerName, kName, sizeof(kName));
    }
    gSaveContext.buttonStatus[0] = gSaveContext.buttonStatus[1] = gSaveContext.buttonStatus[2] =
        gSaveContext.buttonStatus[3] = gSaveContext.buttonStatus[4] = BTN_ENABLED;
    gSaveContext.forceRisingButtonAlphas = gSaveContext.nextHudVisibilityMode = gSaveContext.hudVisibilityMode =
        gSaveContext.hudVisibilityModeTimer = 0;
    gSaveContext.save.entranceIndex = (s32)strtol(env, NULL, 0);
    {   /* OOT_CUTSCENE=<0xFFF0..0xFFFA> (debug.cfg cutscene=): start the entrance with that scene
         * cutscene layer, as the map select's cutscene options do (the visual tour's story moments). */
        const char* cs = getenv("OOT_CUTSCENE");
        if (cs != NULL) {
            gSaveContext.save.cutsceneIndex = (u16)strtol(cs, NULL, 0);
        }
    }
    gSaveContext.respawnFlag = 0;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex = ENTR_LOAD_OPENING;
    gSaveContext.seqId = (u8)NA_BGM_DISABLED;
    gSaveContext.natureAmbienceId = 0xFF;
    gSaveContext.showTitleCard = true;
    gWeatherMode = WEATHER_MODE_CLEAR;
    *nextInit = Play_Init;
    *nextSize = sizeof(PlayState);
    printf("hh: debug boot to entrance 0x%X\n", (unsigned)gSaveContext.save.entranceIndex);
    return 1;
}

static size_t HhStateSize(GameStateFunc init) {
    if (init == Setup_Init) {
        return sizeof(SetupState);
    }
    if (init == ConsoleLogo_Init) {
        return sizeof(ConsoleLogoState);
    }
    if (init == TitleSetup_Init) {
        return sizeof(TitleSetupState);
    }
    if (init == Play_Init) {
        return sizeof(PlayState);
    }
    return sizeof(GameState);
}

/* The packed asset file is <root>data/segments/oot_psp_assets.bin; the loader
 * derives <root> from the "executable path" it is given. */
const char* port_save_dir(void);
/* hh_romscreen.c: "no game data" screen (device) / message (desktop), then exit(1).
 * The pack's id file goes first, so the next start rebuilds the data (hh_convert.c). */
void HhRomScreen_NoData(void);
static void HhNoData(void) {
    char path[600];

    snprintf(path, sizeof(path), "%s/data/segments/oot_psp_assets.id", getenv("OOT_ASSETS") ? getenv("OOT_ASSETS") : port_save_dir());
    unlink(path);
    HhRomScreen_NoData();
}

/* Game data root: $OOT_ASSETS, else the save dir (port_save_dir),
 * holding data/segments/oot_psp_assets.bin and embedded.bin. */
static const char* HhAssetRoot(void) {
    const char* root = getenv("OOT_ASSETS");

    return (root != NULL) ? root : port_save_dir();
}

static const char* HhAssetRootExe(char* buf, size_t size) {
    snprintf(buf, size, "%s/oot", HhAssetRoot());
    return buf;
}

/* OOT_AUTOPATH="x,z;x,z;..." (debug; recordings): from OOT_AUTOPATH_FROM (default 30) on, the stick points Link at
 * each waypoint in turn, relative to the active camera as Player reads it (world yaw = the camera's input yaw +
 * atan2(-stick x, stick y)); a waypoint within 30 units is passed; after the last the stick rests. */
static void HhAutoPath(PlayState* play, long frame) {
    extern int gHhAutoStickOn;
    extern float gHhAutoStickX, gHhAutoStickY;
    static float sWp[64][2];
    static int sN = -1, sI;
    static long sFrom;
    Player* player;
    float dx, dz, th;

    if (sN < 0) {
        const char* e = getenv("OOT_AUTOPATH");
        sN = 0;
        sFrom = getenv("OOT_AUTOPATH_FROM") ? atol(getenv("OOT_AUTOPATH_FROM")) : 30;
        while (e != NULL && *e != '\0' && sN < 64) {
            char* end;
            sWp[sN][0] = strtof(e, &end);
            if (*end != ',') break;
            sWp[sN][1] = strtof(end + 1, &end);
            sN++;
            e = (*end == ';') ? end + 1 : end;
            if (*end != ';') break;
        }
    }
    gHhAutoStickOn = 0;
    if (sN == 0 || play == NULL || frame < sFrom || sI >= sN || (player = GET_PLAYER(play)) == NULL) {
        return;
    }
    dx = sWp[sI][0] - player->actor.world.pos.x;
    dz = sWp[sI][1] - player->actor.world.pos.z;
    if (dx * dx + dz * dz < 30.0f * 30.0f) {
        port_log_always("AUTOPATH f%ld waypoint %d reached (%.0f, %.0f)\n", frame, sI, player->actor.world.pos.x,
                        player->actor.world.pos.z);
        sI++;
        return;
    }
    /* yaw toward the waypoint (binang: 0 = +z, as Math_Vec3f_Yaw), minus the camera's input yaw */
    th = (float)(s16)(Math_Atan2S(dz, dx) - Camera_GetInputDirYaw(GET_ACTIVE_CAM(play))) * (M_PI / 32768.0f);
    gHhAutoStickX = -sinf(th);
    gHhAutoStickY = cosf(th);
    gHhAutoStickOn = 1;
}

/* OOT_NOCATCH=1 (debug; recordings of the castle courtyard): the guards never catch Link (En_Heishi1's
 * search and night check, En_Heishi3's sight) */
int HhDebugNoCatch(void) {
    static int sOn = -1;

    if (sOn < 0) {
        sOn = getenv("OOT_NOCATCH") != NULL && getenv("OOT_NOCATCH")[0] == '1';
    }
    return sOn;
}

/* OOT_CROP3D=2 (gfx_pc.c): in a prerendered room, the 240-wide window over the game's 320-wide picture
 * follows Link. Its left edge stays in 0..2*margin and keeps Link at least 60 N64 pixels inside it (a dead
 * zone: the view holds still while he walks about the middle); a new room or camera starts with him
 * centred. gHhCropPan = the window's centre - 160, in N64 pixels. */
static void HhCropPanUpdate(PlayState* play) {
    extern float gHhCropPan;
    extern int gHhCropPan3D;
    extern float gfx_current_dimensions_aspect(void);
    extern int gfx_crop3d(void);
    static float sLeft;
    static int sActive, sScene = -1, sRoom = -1, sBgCam = -1;
    const float winW = 240.0f * gfx_current_dimensions_aspect(), margin = (320.0f - winW) * 0.5f;
    Player* player;
    float x, w;

    int image, sideScroll;
    float dead;

    gHhCropPan3D = 0;
    image = play != NULL && play->roomCtx.curRoom.roomShape != NULL &&
            play->roomCtx.curRoom.roomShape->base.type == ROOM_SHAPE_TYPE_IMAGE;
    /* the castle courtyard (CAM_SET_PREREND_SIDE_SCROLL, real 3D): the same slide over the 3D alone, with a
     * wider dead zone so Link gets up to his N64 distance from the edge he runs toward (the user, 2026-09-29) */
    sideScroll = play != NULL && !image && play->cameraPtrs[CAM_ID_MAIN] != NULL &&
                 play->cameraPtrs[CAM_ID_MAIN]->setting == CAM_SET_PREREND_SIDE_SCROLL;
    if (gfx_crop3d() != 2 || (!image && !sideScroll) || margin <= 0.0f || (player = GET_PLAYER(play)) == NULL) {
        sActive = 0;
        gHhCropPan = 0.0f;
        return;
    }
    dead = sideScroll ? 100.0f : 60.0f;
    w = player->actor.projectedW != 0.0f ? player->actor.projectedW : 1.0f;
    x = 160.0f + 160.0f * player->actor.projectedPos.x / w;
    if (!sActive || play->sceneId != sScene || play->roomCtx.curRoom.num != sRoom ||
        play->cameraPtrs[CAM_ID_MAIN]->bgCamIndex != sBgCam) {
        sLeft = x - winW * 0.5f; /* a new room, or a new picture of it (the back alley's cameras): Link centred */
        sActive = 1;
        sScene = play->sceneId;
        sRoom = play->roomCtx.curRoom.num;
        sBgCam = play->cameraPtrs[CAM_ID_MAIN]->bgCamIndex;
    } else if (x < sLeft + dead) {
        sLeft = x - dead;
    } else if (x > sLeft + winW - dead) {
        sLeft = x - winW + dead;
    }
    if (sLeft < 0.0f) sLeft = 0.0f;
    if (sLeft > 2.0f * margin) sLeft = 2.0f * margin;
    gHhCropPan = sLeft + winW * 0.5f - 160.0f;
    gHhCropPan3D = sideScroll;
}

/* OOT_EXITSCAN=1 (debug; the OOT_CROP3D question, 2026-09-29): in a prerendered room, every 30 frames,
 * where each exit lands across the game's 320-wide picture, through the game's own view-projection:
 * the doors (transition actors, their position and 60 units above) and the exit polygons of the scene
 * collision (min/max of their vertices, per exit index). A crop to the middle 240 keeps x 40..280. */
static void HhExitScan(PlayState* play, long frame) {
    const RoomShape* shape = play->roomCtx.curRoom.roomShape;
    CollisionHeader* col = play->colCtx.colHeader;
    float lo[64], hi[64];
    int behind[64];
    int i;

    if (frame % 30 != 0 || shape == NULL || shape->base.type != ROOM_SHAPE_TYPE_IMAGE) {
        return;
    }
    for (i = 0; i < play->transitionActors.count; i++) {
        TransitionActorEntry* t = &play->transitionActors.list[i];
        float x0 = 1e9f, x1 = -1e9f;
        int k, back = 0;

        for (k = 0; k < 2; k++) {
            Vec3f v = { t->pos.x, t->pos.y + k * 60.0f, t->pos.z }, o;
            f32 w;

            SkinMatrix_Vec3fMtxFMultXYZW(&play->viewProjectionMtxF, &v, &o, &w);
            if (w <= 0.0f) { back = 1; continue; }
            if (160.0f + o.x / w * 160.0f < x0) x0 = 160.0f + o.x / w * 160.0f;
            if (160.0f + o.x / w * 160.0f > x1) x1 = 160.0f + o.x / w * 160.0f;
        }
        port_log_always("EXITSCAN f%ld scene %d room %d cam %d door %d (actor 0x%X rooms %d/%d) x %.0f..%.0f%s\n", frame,
                        play->sceneId, play->roomCtx.curRoom.num, play->cameraPtrs[CAM_ID_MAIN]->setting, i, t->id,
                        t->sides[0].room, t->sides[1].room, x0, x1, back ? " (partly behind the camera)" : "");
    }
    { /* every actor: where it is drawn (its projected position, as the game computes it) */
        int cat;
        for (cat = 0; cat < ACTORCAT_MAX; cat++) {
            Actor* a;
            for (a = play->actorCtx.actorLists[cat].head; a != NULL; a = a->next) {
                const f32 w = a->projectedW;
                if (w <= 0.0f) continue;
                port_log_always("EXITSCAN f%ld scene %d room %d cam %d actor 0x%03X cat %d params 0x%04X x %.0f..%.0f\n",
                                frame, play->sceneId, play->roomCtx.curRoom.num, play->cameraPtrs[CAM_ID_MAIN]->setting,
                                a->id, cat, (u16)a->params, 160.0f + 160.0f * a->projectedPos.x / w,
                                160.0f + 160.0f * a->projectedPos.x / w);
            }
        }
    }
    for (i = 0; i < 64; i++) { lo[i] = 1e9f; hi[i] = -1e9f; behind[i] = 0; }
    for (i = 0; col != NULL && i < col->numPolygons; i++) {
        CollisionPoly* poly = &col->polyList[i];
        const u32 ex = SurfaceType_GetExitIndex(&play->colCtx, poly, BGCHECK_SCENE);
        int k;

        if (ex == 0 || ex >= 64) continue;
        for (k = 0; k < 3; k++) {
            Vec3s* s3 = &col->vtxList[COLPOLY_VTX_INDEX(poly->vtxData[k])];
            Vec3f v = { s3->x, s3->y, s3->z }, o;
            f32 w;

            SkinMatrix_Vec3fMtxFMultXYZW(&play->viewProjectionMtxF, &v, &o, &w);
            if (w <= 0.0f) { behind[ex] = 1; continue; }
            if (160.0f + o.x / w * 160.0f < lo[ex]) lo[ex] = 160.0f + o.x / w * 160.0f;
            if (160.0f + o.x / w * 160.0f > hi[ex]) hi[ex] = 160.0f + o.x / w * 160.0f;
        }
    }
    for (i = 1; i < 64; i++) {
        if (lo[i] <= hi[i] || behind[i]) {
            port_log_always("EXITSCAN f%ld scene %d room %d cam %d exit %d x %.0f..%.0f%s\n", frame, play->sceneId,
                            play->roomCtx.curRoom.num, play->cameraPtrs[CAM_ID_MAIN]->setting, i, lo[i], hi[i],
                            behind[i] ? " (partly behind the camera)" : "");
        }
    }
}

/* <root>/debug.cfg (test builds): "entrance=0xCD" / "adult=1" -> the same debug
 * boot as the OOT_ENTRANCE / OOT_ADULT environment variables (a launcher start has
 * no environment of its own). */
static void HhLoadDebugCfg(void) {
    char path[600];
    char line[128];
    FILE* f;

    snprintf(path, sizeof(path), "%s/debug.cfg", HhAssetRoot());
    f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char* eq = strchr(line, '=');

        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        eq[strcspn(eq + 1, "\r\n") + 1] = '\0';
        if (strcmp(line, "entrance") == 0) {
            setenv("OOT_ENTRANCE", eq + 1, 1);
        } else if (strcmp(line, "adult") == 0) {
            setenv("OOT_ADULT", eq + 1, 1);
        } else if (strcmp(line, "prof") == 0 && eq[1] == '1') {
            setenv("OOT_PROF", "1", 1); /* on-device sampling profiler -> prof.txt */
        } else if (line[0] != '#' && strlen(line) < 24) {
            /* any other key=value is the OOT_<KEY> switch of the same name, so a
             * device A/B needs no rebuild: tex32=1, hudfilter=0, boxcache=0,
             * tilemask=0, ... */
            char name[32] = "OOT_";
            size_t i, n = 4;
            int ok = line[0] != '\0';

            for (i = 0; line[i] != '\0'; i++) {
                const char c = line[i];

                ok = ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_');
                name[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
            }
            name[n] = '\0';
            if (ok) {
                setenv(name, eq + 1, 1);
            }
        }
    }
    fclose(f);
}

int main(int argc, char** argv) {
    HhMainLoop* const L = &gHhMainLoop;
    char exeBuf[512];
    const char* env;

    /* Kit k1.6: nice -10 before any thread or child exists (all inherit it), so an OS helper
     * script that runs every 2 s fits in the frame gaps instead of taking a frame (the HITCH every 41 frames at
     * 20 fps). OOT_PRIO=0: the old priority. */
    {
        extern void hh_game_priority(void);
        hh_game_priority();
    }
    {   /* The device bench (hh_bench.c): launched as oot_bench, run the views as children. */
        extern int hh_bench_wanted(const char* argv0);
        extern int hh_bench_main(void);
        if (hh_bench_wanted(argv[0])) {
            return hh_bench_main();
        }
    }

    {   /* OOT_HEAPSHIFT=<bytes> (tests): start the host heap and the mmap area somewhere else,
         * as the device's address randomisation does on every boot, so a host pointer that
         * leaks into a save state breaks a load on the desktop too (MK64_HEAPSHIFT). */
        const char* hs = getenv("OOT_HEAPSHIFT");

        if (hs != NULL && atol(hs) > 0) {
            const size_t n = (size_t)atol(hs);
            volatile char* p = malloc(n);
            void* m = mmap(NULL, (n + 0xFFFF) & ~(size_t)0xFFFF, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                           -1, 0);

            if (p != NULL) {
                p[0] = 1;
            }
            (void)m;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    savestate_parse_args(argc, argv); /* --loadStateFile <path> (tests) */
    if ((env = getenv("OOT_EXIT_AT")) != NULL) {
        sExitAt = atol(env);
    }
#ifndef HANDHELD
    signal(SIGSEGV, HhBacktrace);
    signal(SIGALRM, HhBacktrace);
    if ((env = getenv("OOT_WATCHDOG")) != NULL) {
        alarm((unsigned)atoi(env));
    }
#endif

    HhLoadDebugCfg();
    {
        extern void HhProf_Init(void);
        HhProf_Init();
    }
    osInitialize();
    {
        /* hh_convert.c: first start builds the game data from the player's ROM */
        extern void HhConvert_Ensure(const char* root);
        HhConvert_Ensure(HhAssetRoot());
    }
    OotPspRenderer_Init();
    if (!OotPsp_AssetInit(HhAssetRootExe(exeBuf, sizeof(exeBuf)))) {
        printf("hh: asset initialisation failed (set OOT_ASSETS to the dir holding data/segments/)\n");
        HhNoData();
    }
    {
        extern int HhEmbedded_Load(const char* dir);

        if (!HhEmbedded_Load(HhAssetRoot())) {
            HhNoData();
        }
    }
    savestate_boot_prompt(); /* an auto save on the card: RESUME GAME / NEW GAME (handheld) */
    OotPspControls_Load();
    OotPspGame_Init();
    Graph_Init(&L->gfxCtx);
    {
        extern void HhOvl_LogInit(void);
        HhOvl_LogInit(); /* hh_ovl.c: overlay reloads, as the N64 does them */
    }
    L->nextInit = Setup_Init;
    L->nextSize = sizeof(SetupState);

    while (L->nextInit != NULL && !sExitRequested) {
        L->gameState = SYSTEM_ARENA_MALLOC(L->nextSize, __FILE__, __LINE__);
        if (L->gameState == NULL) {
            Fault_AddHungupAndCrash(__FILE__, __LINE__);
        }
        memset(L->gameState, 0, L->nextSize);
        printf("hh: gamestate init=%p size=%u\n", (void*)L->nextInit, (unsigned)L->nextSize);
        L->curInit = L->nextInit;
        gHhLoopStage = "game state init"; /* a scene load: no frame drawn until it ends */
        {   /* the N64 loads a game state's overlay as the state starts and frees it as it ends (graph.c): its
             * .data/.bss as that fresh load would leave them (hh_ovl.c) */
            extern void HhOvl_GameStateLoad(void* init);
            HhOvl_GameStateLoad((void*)L->nextInit);
        }
        GameState_Init(L->gameState, L->nextInit, &L->gfxCtx);

        while (GameState_IsRunning(L->gameState)) {
            if (sExitRequested || (sExitAt >= 0 && sFrames >= sExitAt)) {
                if (!sExitRequested) {
                    sExitAtReached = 1;
                }
                sExitRequested = 1;
                break;
            }
            /* The iteration boundary: a queued save or load (menu, power-off, boot resume) happens
             * here, between two whole frames, never inside one. A load replaces L's contents. */
            gHhLoopStage = "save state check";
            HhSaveStateHooks();
            HhDebugSpawn();
            HhDebugWarpRoom();
            HhDebugMessage();
            {   /* OOT_TIMELOCK=<dayTime> (debug): hold the clock there every frame, as a GameShark
                 * "8111A5DC xxxx" does in the z64 reference runs, so sky checks compare one moment. */
                static const char* sTimeLock;
                static int sTimeLockInit;
                if (!sTimeLockInit) {
                    sTimeLockInit = 1;
                    sTimeLock = getenv("OOT_TIMELOCK");
                }
                if (sTimeLock != NULL) {
                    gSaveContext.save.dayTime = (u16)strtol(sTimeLock, NULL, 0);
                }
            }
            savestate_check();
            if (sExitRequested) {
                break; /* EXIT GAME's auto save is done */
            }
            gHhLoopStage = "game update"; /* then hh_renderer.c / hh_sched.c: drawing, pacer sleep, audio update */
            Graph_Update(&L->gfxCtx, L->gameState);
            sFrames++;
            gHhLoopStage = "loop end";
            {   /* the renderer's per-scene rules (gfx_soft.c OOT_WATERGRAIN): the scene being drawn */
                extern int gHhSceneId, gHhRoomNum;
                gHhSceneId = L->curInit == Play_Init ? ((PlayState*)L->gameState)->sceneId : -1;
                gHhRoomNum = L->curInit == Play_Init ? ((PlayState*)L->gameState)->roomCtx.curRoom.num : -1; /* the SCENE lines */
            }
            HhCropPanUpdate(L->curInit == Play_Init ? (PlayState*)L->gameState : NULL);
            /* the debug switches below, read once (the environment is set before the loop) */
            static int sDbgInit, sDbgAutoPath, sDbgExitScan, sDbgTraceLink;
            if (!sDbgInit) {
                sDbgInit = 1;
                sDbgAutoPath = getenv("OOT_AUTOPATH") != NULL;
                sDbgExitScan = getenv("OOT_EXITSCAN") != NULL;
                sDbgTraceLink = getenv("OOT_TRACE_LINK") != NULL;
            }
            if (sDbgAutoPath) {
                HhAutoPath(L->curInit == Play_Init ? (PlayState*)L->gameState : NULL, sFrames);
            }
            if (L->curInit == Play_Init && sDbgExitScan) {
                HhExitScan((PlayState*)L->gameState, sFrames);
            }
            if (L->curInit == Play_Init && sDbgTraceLink) {
                /* debug: Link's position each frame (collision tests with OOT_INPUT) */
                Player* player = GET_PLAYER((PlayState*)L->gameState);

                if (player != NULL) {
                    /* screen: the game's 320x240 from the projected feet position */
                    const f32 w = player->actor.projectedW != 0.0f ? player->actor.projectedW : 1.0f;
                    printf("LINK %ld %.1f %.1f %.1f speed %.2f screen %.1f %.1f rupees %d\n", sFrames,
                           player->actor.world.pos.x, player->actor.world.pos.y, player->actor.world.pos.z,
                           player->actor.speed, 160.0f + 160.0f * player->actor.projectedPos.x / w,
                           120.0f - 120.0f * player->actor.projectedPos.y / w,
                           (int)gSaveContext.save.info.playerData.rupees);
                }
            }
            {
                extern void HhProf_Frame(void);
                extern void HhAudio_SyncFrame(void);
                HhProf_Frame();
                HhAudio_SyncFrame(); /* OOT_AUDIO_SYNC=1 only: the audio updates of this frame */
            }
        }

        if (sExitRequested) {
            extern void HhAudio_BeginExit(void);
            gHhLoopStage = "exit";
            HhAudio_BeginExit(); /* a queued audio reset in the destroy waits on the audio thread */
            GameState_Destroy(L->gameState);
            SYSTEM_ARENA_FREE(L->gameState, __FILE__, __LINE__);
            break;
        }

        gHhLoopStage = "game state destroy";
        L->nextInit = GameState_GetInit(L->gameState);
        L->nextSize = L->gameState->size != 0 ? L->gameState->size : HhStateSize(L->nextInit);
        if (L->curInit == Setup_Init) {
            HhDebugBootEntrance(&L->nextInit, &L->nextSize);
        }
        printf("hh: gamestate done frames=%u next=%p\n", L->gameState->frames, (void*)L->nextInit);
        GameState_Destroy(L->gameState);
        SYSTEM_ARENA_FREE(L->gameState, __FILE__, __LINE__);
    }

    Graph_Destroy(&L->gfxCtx);
    savestate_shutdown();
    {
        extern void HhProf_Exit(void);
        HhProf_Exit();
    }
    port_log_always("hh: exit after %ld frames\n", sFrames); /* console.log: the game got out */
    {   /* SDL down on every exit, not just the bench's: SDL 1.2's fbcon leaves the console, keyboard
         * and video modes set up otherwise, and the next SDL program (the launcher after EXIT GAME)
         * can hang at its first flip, as GoldenEye's second bench run did. */
        extern void HhAudio_Shutdown(void);
        extern int gfx_wm_shutdown(void);
        HhAudio_Shutdown();
        if (getenv("OOT_BENCH_CHILD") != NULL && sExitAtReached) {
            extern void HhPerf_BenchReport(void);
            HhPerf_BenchReport();
        }
        port_log_always(gfx_wm_shutdown() ? "exit: SDL shut down\n" : "exit: SDL left up (a flip still running)\n");
        if (getenv("OOT_BENCH_CHILD") != NULL && !sExitAtReached) {
            return 3; /* HH_BENCH_QUIT: the player quit (EXIT GAME); the bench stops (hh_bench.c) */
        }
    }
    return 0;
}
