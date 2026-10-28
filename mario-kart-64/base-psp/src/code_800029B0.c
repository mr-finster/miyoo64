#include <ultra64.h>
#ifndef TARGET_N64
#include <stdlib.h>
#include "port/port.h"
#endif
#include <macros.h>
#include <mk64.h>

#include "code_800029B0.h"
#include "memory.h"
#include "path.h"
#include "actors.h"
#include "actor_types.h"
#include "math_util.h"
#include "audio/external.h"
#include <defines.h>
#include "collision.h"
#include "memory.h"
#include "menu_items.h"
#include "skybox_and_splitscreen.h"
#include "code_8006E9C0.h"
#include "spawn_players.h"
#include "replays.h"
#include "render_courses.h"
#include "main.h"
#include "courses/all_course_data.h"
#include "courses/all_course_packed.h"
#include "menus.h"
#include "data/other_textures.h"

extern s32 gDemoTimer;
extern s16 D_802BA048;
#if !ENABLE_CUSTOM_COURSE_ENGINE
s16 gCurrentCourseId = 0;
#endif
s16 gCurrentlyLoadedCourseId = 0xFF;
u16 D_800DC5A8 = 0;
s32 D_800DC5AC = 0;
u16 D_800DC5B0 = 1;
u16 D_800DC5B4 = 0;
u16 D_800DC5B8 = 0;
u16 D_800DC5BC = 0;
u16 gIsInQuitToMenuTransition = 0;
u16 gQuitToMenuTransitionCounter = 0;
u16 D_800DC5C8 = 0;
UNUSED u16 D_800DC5CC = 0;
s32 D_800DC5D0 = 0;
s32 D_800DC5D4 = 0;
s32 D_800DC5D8 = 0;
s32 D_800DC5DC = 64;

s32 D_800DC5E0 = 32;

// This is tracking which credit "state" we're in, decides which credits are shown (and probably other stuff)
u16 D_800DC5E4 = 0;

//! @todo gPlayerWinningIndex (D_800DC5E8) accessed as word, D_800DC5EB as u8
s32 gPlayerWinningIndex = 0;

ALIGNED16 struct UnkStruct_800DC5EC D_8015F480[4];
struct UnkStruct_800DC5EC* D_800DC5EC = &D_8015F480[0];
struct UnkStruct_800DC5EC* D_800DC5F0 = &D_8015F480[1];
struct UnkStruct_800DC5EC* D_800DC5F4 = &D_8015F480[2];
struct UnkStruct_800DC5EC* D_800DC5F8 = &D_8015F480[3];
u16 gIsGamePaused = 0; // 1 if the game is paused and 0 if the game is not paused
u8* pAppNmiBuffer = (u8*) &osAppNmiBuffer;

s32 gIsMirrorMode = 0;
f32 vtxStretchY = 1.0f;
Lights1 D_800DC610[] = {
    gdSPDefLights1(175, 175, 175, 255, 255, 255, 0, 0, 120),
    gdSPDefLights1(115, 115, 115, 255, 255, 255, 0, 0, 120),
};
UNUSED s32 pad_800029B0 = 0x80000000;
s16 gCreditsCourseId = COURSE_LUIGI_RACEWAY;
s16 gPlaceItemBoxes = 1;

// Technically a pointer to an array, but declaring it so creates regalloc issues.
CollisionTriangle* gCollisionMesh;
u16* gCollisionIndices;
u16 gCollisionMeshCount; // Number of entries in gCollisionMesh
u16 gNumCollisionTriangles;
u32 D_8015F58C;

Vec3f D_8015F590;
s32 D_8015F59C;
s32 D_8015F5A0;
s32 D_8015F5A4;
s32 code_800029B0_bss_pad[48];
Vtx* vtxBuffer[32];

s16 gCourseMaxX;
s16 gCourseMinX;

s16 gCourseMaxY; // s16 or u16?
s16 gCourseMinY;

s16 gCourseMaxZ;
s16 gCourseMinZ;

s16 D_8015F6F4;
s16 D_8015F6F6;
u16 D_8015F6F8;
s16 D_8015F6FA;
s16 D_8015F6FC;
/**
 * Incremented by one every time the player spawns a new shell actor.
 * A cleaning routine runs once twenty-one shells have spawned.
 */
u16 gNumSpawnedShells;

u16 D_8015F700;
u16 D_8015F702;
f32 D_8015F704;
Vec3f D_8015F708;
UNUSED u32 D_8015F718[3]; // Likely held ptrs to segmented data.
size_t gFreeMemorySize;
uintptr_t gNextFreeMemoryAddress;
uintptr_t gHeapEndPtr;
u32 D_8015F730;
uintptr_t gFreeMemoryResetAnchor;
Vec3f D_8015F738;
Vec3f D_8015F748;
Vec3f D_8015F758;
Vec3f D_8015F768;
Vec3f D_8015F778;

f32 gCourseDirection; // Extra mode, flips vertices.
s32 D_8015F788;
s32 D_8015F790[64]; // Unknown data, potentially not used.
u16 D_8015F890;
u16 D_8015F892;
u16 D_8015F894;
// Indexed by Player ID. Track time in seconds since player has last crossed the finish line
f32 gTimePlayerLastTouchedFinishLine[8];

u8* nmi_gVersusResults2P;
u8* nmi_gVersusResults3P;
u8* nmi_gVersusResults4P;
u8* gNmiUnknown4;
u8* gNmiUnknown5;
u8* gNmiUnknown6;

Vec3f D_8015F8D0;
s32 D_8015F8DC;

s32 D_8015F8E0;
f32 D_8015F8E4;
f32 D_8015F8E8;
s16 gPlayerPositionLUT[8]; // Player index at each position
u16 gNumPermanentActors;
s32 code_800029B0_bss_pad2[44];

struct Actor gActorList[ACTOR_LIST_SIZE];
//! @warning todo: Is this apart of the actor array?
UNUSED u8 D_80162578[sizeof(struct Actor)];

s16 gDebugPathCount;
s16 sIsController1Unplugged;
s32 D_801625EC;
s32 D_801625F0;
s32 D_801625F4;
s32 D_801625F8;
f32 D_801625FC;

void func_800029B0(void) {
    switch (D_800DC5A8) {
        case 0:
            func_800C8F44(127);
            break;
        case 1:
            func_800C8F44(75);
            break;
        case 2:
            func_800C8F44(0);
            break;
    }
}

void setup_race(void) {
    struct Controller* controller;
    int i;

    gPlayerCountSelection1 = gPlayerCount;
    if (gGamestate != RACING) {
        gIsMirrorMode = 0;
    }
    if (gIsMirrorMode) {
        gCourseDirection = -1.0f;
    } else {
        gCourseDirection = 1.0f;
    }
    if (gModeSelection == GRAND_PRIX) {
        gCurrentCourseId = gCupCourseOrder[gCupSelection][gCourseIndexInCup];
    }
    gActiveScreenMode = gScreenModeSelection;
#ifdef PORT_FORCE_COURSE
    gCurrentCourseId = (PORT_FORCE_COURSE);
#endif
#ifdef PORT_COURSE_TEST
    {
        extern s32 gPortForceCourse;
        if (gPortForceCourse >= 0) {
            gCurrentCourseId = gPortForceCourse;
            // Battle arenas (Block Fort/Skyscraper/Double Deck/Big Donut -- NOT
            // DK Jungle 0x12, a GP course) only make sense in Battle mode; force
            // the state the battle menu would set so the load path is valid.
            if (gPortForceCourse == COURSE_BLOCK_FORT || gPortForceCourse == COURSE_SKYSCRAPER ||
                gPortForceCourse == COURSE_DOUBLE_DECK || gPortForceCourse == COURSE_BIG_DONUT) {
                gModeSelection = BATTLE;
                gPlayerCount = gPlayerCountSelection1 = 2;
                gScreenModeSelection = gActiveScreenMode = SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL;
            }
        }
    }
#endif
#ifndef TARGET_N64
    { /* MK64_COURSE=<id>: force a course (desktop testing, e.g. fog tracks). */
        static int sForceCourse = -2;
        if (sForceCourse == -2) {
            const char *e = getenv("MK64_COURSE");
            sForceCourse = e ? atoi(e) : -1;
        }
        if (sForceCourse >= 0) gCurrentCourseId = sForceCourse;
    }
    { /* MK64_MIRROR=1: EXTRA (mirror) mode without the unlock (desktop testing): what
       * the class OK sets for EXTRA, here after the clear above for a fresh race. */
        static int sForceMirror = -2;
        if (sForceMirror == -2) {
            const char *e = getenv("MK64_MIRROR");
            sForceMirror = e ? atoi(e) : 0;
        }
        if (sForceMirror == 1) {
            gIsMirrorMode = 1;
            gCourseDirection = -1.0f;
            gCCSelection = CC_EXTRA;
        }
    }
    { /* MK64_CHAR=<0-7>: force player 1's character (Mario, Luigi, Yoshi,
       * Toad, DK, Wario, Peach, Bowser) -- desktop testing / screenshots. */
        static int sForceChar = -2;
        if (sForceChar == -2) {
            const char *e = getenv("MK64_CHAR");
            sForceChar = e ? atoi(e) : -1;
        }
        if (sForceChar >= 0 && sForceChar <= 7) gCharacterSelections[0] = (s8) sForceChar;
    }
    { /* MK64_PLAYERS=4: a 4-player VS race whatever the menus chose (players 2-4 sit idle); 40 the same in BATTLE --
       * desktop screenshots of the split-screen HUD (MK64_HUD). */
        static int sForcePlayers = -2;
        if (sForcePlayers == -2) {
            const char *e = getenv("MK64_PLAYERS");
            sForcePlayers = e ? atoi(e) : -1;
        }
        if (sForcePlayers == 4 || sForcePlayers == 40) { /* 40: a 4-player BATTLE (with MK64_COURSE=15/16/17/19) */
            gModeSelection = sForcePlayers == 40 ? BATTLE : VERSUS;
            if (sForcePlayers == 40) gCCSelection = CC_BATTLE;
            gPlayerCount = gPlayerCountSelection1 = 4;
            gScreenModeSelection = gActiveScreenMode = SCREEN_MODE_3P_4P_SPLITSCREEN;
            gCharacterSelections[1] = 1; gCharacterSelections[2] = 2; gCharacterSelections[3] = 3;
        }
    }
    /* The N64 mixed audio on its own thread, so course loads never starved
     * it. port_audio_loading(1) hands the mixer to a helper thread for the
     * whole load (audio_sdl.c) -- per-stage pumps (round 3/4) still left gaps
     * because the stall has no fixed hot spot. Nothing in these stages
     * touches the audio engine, and the mixer is mutex-serialised anyway. */
    extern void port_audio_loading(int on);
#define AUDIO_PUMP() ((void)0)
    port_audio_loading(1);
#else
#define AUDIO_PUMP()
#endif
    if (gCurrentCourseId != gCurrentlyLoadedCourseId) {
        D_80150120 = 0;
        gCurrentlyLoadedCourseId = gCurrentCourseId;
        gNextFreeMemoryAddress = gFreeMemoryResetAnchor;
        load_course(gCurrentCourseId);
        AUDIO_PUMP();
        PORT_LOGV(" collision mesh\n");
        course_generate_collision_mesh();
        PORT_LOGV(" collision mesh done\n");
        AUDIO_PUMP();
        D_8015F730 = gNextFreeMemoryAddress;
    } else {
        gNextFreeMemoryAddress = D_8015F730;
    }
    func_802969F8();
    PORT_LOGV(" func_802969F8 done\n");
    AUDIO_PUMP();
    func_80005310();
    PORT_LOGV(" func_80005310 done\n");
    AUDIO_PUMP();
    func_8003D080();
    PORT_LOGV(" func_8003D080 done\n");
    init_hud();
    PORT_LOGV(" init_hud done\n");
    AUDIO_PUMP();
    gRaceState = RACE_NONE;
    gNumSpawnedShells = 0;
    D_800DC5B8 = 0;
    D_80152308 = 0;
    gDemoTimer = -1;
    D_802BA048 = 0;
    func_802A74BC();
    PORT_LOGV(" func_802A74BC done\n");
    AUDIO_PUMP();
    set_perspective_and_aspect_ratio();
    func_80091FA4();
    PORT_LOGV(" func_80091FA4 done\n");
    AUDIO_PUMP();
    init_actors_and_load_textures();
    PORT_LOGV(" init_actors_and_load_textures done\n");
#ifndef TARGET_N64
    port_audio_loading(0);
#endif

    if (gModeSelection != BATTLE) {
        D_8015F8D0[1] = (f32) (gCurrentTrackPath->posY - 15);
        D_8015F8D0[2] = gCurrentTrackPath->posZ;
        if (gCurrentCourseId == COURSE_TOADS_TURNPIKE) {
            D_8015F8D0[0] = (gIsMirrorMode != 0) ? gCurrentTrackPath->posX + 138.0f : gCurrentTrackPath->posX - 138.0f;
        } else if (gCurrentCourseId == COURSE_WARIO_STADIUM) {
            D_8015F8D0[0] = (gIsMirrorMode != 0) ? gCurrentTrackPath->posX + 12.0f : gCurrentTrackPath->posX - 12.0f;
        } else {
            D_8015F8D0[0] = gCurrentTrackPath->posX;
        }
    }
    if (!gDemoMode) {
        func_800CA008(gPlayerCountSelection1 - 1, gCurrentCourseId + 4);
        func_800CB2C4();
    }

    controller = gControllerOne;

    for (i = 0; i < 7; i++, controller++) {
        controller->rawStickX = 0;
        controller->rawStickY = 0;
        controller->buttonPressed = 0;
        controller->buttonDepressed = 0;
        controller->button = 0;
    }
}

// sound related
void func_80002DAC(void) {
#if !ENABLE_CUSTOM_COURSE_ENGINE
    switch (gCurrentCourseId) {
        case COURSE_MARIO_RACEWAY:
            vec3f_set(D_8015F748, -223.0f, 94.0f, -155.0f);
            func_800C9D80(D_8015F748, D_802B91C8, 0x5103700B);
            break;
        case COURSE_ROYAL_RACEWAY:
            vec3f_set(D_8015F748, 177.0f, 87.0f, -393.0f);
            func_800C9D80(D_8015F748, D_802B91C8, 0x5103700B);
            break;
        case COURSE_LUIGI_RACEWAY:
            vec3f_set(D_8015F748, 85.0f, 21.0f, -219.0f);
            func_800C9D80(D_8015F748, D_802B91C8, 0x5103700B);
            break;
        case COURSE_WARIO_STADIUM:
            vec3f_set(D_8015F748, 298.0f, 202.0f, -850.0f);
            func_800C9D80(D_8015F748, D_802B91C8, 0x5103700B);
            vec3f_set(D_8015F758, -1600.0f, 202.0f, -2430.0f);
            func_800C9D80(D_8015F758, D_802B91C8, 0x5103700B);
            vec3f_set(D_8015F768, -2708.0f, 202.0f, 1762.0f);
            func_800C9D80(D_8015F768, D_802B91C8, 0x5103700B);
            vec3f_set(D_8015F778, -775.0f, 202.0f, 1930.0f);
            func_800C9D80(D_8015F778, D_802B91C8, 0x5103700B);
            break;
        case COURSE_KOOPA_BEACH:
            vec3f_set(D_8015F738, 153.0f, 0.0f, 2319.0f);
            func_800C9D80(D_8015F738, D_802B91C8, 0x51028001);
            break;
        case COURSE_DK_JUNGLE:
            vec3f_set(D_8015F738, -790.0f, -255.0f, -447.0f);
            func_800C9D80(D_8015F738, D_802B91C8, 0x51028001);
            break;
        default:
            break;
    }
#else

#endif
}

/**
 * The nmi buffer allows developers to save 64 bytes of data through a soft reset.
 */
void clear_nmi_buffer(void) {
    s32 i;
    for (i = 0; i < 16; i++) {
        osAppNmiBuffer[i] = 0;
    }
}

void func_80003040(void) {
    Vec3f position;
    Vec3f velocity = { 0, 0, 0 };
    Vec3s rotation = { 0, 0, 0 };

    D_800DC5BC = 0;
    D_800DC5C8 = 0;
    gNumActors = 0;
    gIsMirrorMode = 0;
    gCourseDirection = 1.0f;

    gPlayerCountSelection1 = 1;

    //! @warning Grand Prix loads some textures that credits does not
    // Therefore, this calculation allows future texture allocations to skip 0x9000
    // this allows hard-coded data addresses to sync with memory data.
    // In other words, the first texture in segment 3 for credits should start at 0x9000
    // This is only required for moo moo farm.
    // This is also bad memory management practice as this could result in overwriting the wrong memory.
    set_segment_base_addr(0x3, (void*) (gNextFreeMemoryAddress - 0x9000));
    destroy_all_actors();
#if !ENABLE_CUSTOM_COURSE_ENGINE
    switch (gCurrentCourseId) {
        case COURSE_MARIO_RACEWAY:
            dma_textures(gTextureTrees1, 0x35B, 0x800);
            spawn_foliage(d_course_mario_raceway_tree_spawns);
            break;
        case COURSE_BOWSER_CASTLE:
            // d_course_bowsers_castle_packed_dl_1350
            find_vtx_and_set_colours(0x07001350, 0x32, 0, 0, 0);
            break;
        case COURSE_BANSHEE_BOARDWALK:
            // d_course_banshee_boardwalk_packed_dl_878
            find_vtx_and_set_colours(0x07000878, 128, 0, 0, 0);
            break;
        case COURSE_YOSHI_VALLEY:
            vec3f_set(position, -2300.0f, 0.0f, 634.0f);
            position[0] *= gCourseDirection;
            add_actor_to_empty_slot(position, rotation, velocity, ACTOR_YOSHI_EGG);
            break;
        case COURSE_MOO_MOO_FARM:
            dma_textures(gTextureTrees4Left, 0x3E8, 0x800);
            dma_textures(gTextureTrees4Right, 0x3E8, 0x800);
            dma_textures(gTextureCow01Left, 0x400, 0x800);
            dma_textures(gTextureCow01Right, 0x400, 0x800);
            dma_textures(gTextureCow02Left, 0x400, 0x800);
            dma_textures(gTextureCow02Right, 0x400, 0x800);
            dma_textures(gTextureCow03Left, 0x400, 0x800);
            dma_textures(gTextureCow03Right, 0x400, 0x800);
            dma_textures(gTextureCow04Left, 0x400, 0x800);
            dma_textures(gTextureCow04Right, 0x400, 0x800);
            dma_textures(gTextureCow05Left, 0x400, 0x800);
            dma_textures(gTextureCow05Right, 0x400, 0x800);
            spawn_foliage(d_course_moo_moo_farm_tree_spawn);
            break;
        case COURSE_SHERBET_LAND:
            // d_course_sherbet_land_packed_dl_1EB8
            find_vtx_and_set_colours(0x07001EB8, 180, 0xFF, 0xFF, 0xFF);
            // d_course_sherbet_land_packed_dl_2308
            find_vtx_and_set_colours(0x07002308, 150, 0xFF, 0xFF, 0xFF);
            break;
        case COURSE_RAINBOW_ROAD:
            // d_course_rainbow_road_packed_dl_2068
            find_vtx_and_set_colours(0x07002068, 150, 0xFF, 0xFF, 0xFF);
            // d_course_rainbow_road_packed_dl_1E18
            find_vtx_and_set_colours(0x07001E18, 150, 0xFF, 0xFF, 0xFF);
            // d_course_rainbow_road_packed_dl_1318
            find_vtx_and_set_colours(0x07001318, 255, 0xFF, 0xFF, 0);
            break;
        case COURSE_WARIO_STADIUM:
            vec3f_set(position, -131.0f, 83.0f, 286.0f);
            add_actor_to_empty_slot(position, rotation, velocity, ACTOR_WARIO_SIGN);
            vec3f_set(position, -2353.0f, 72.0f, -1608.0f);
            add_actor_to_empty_slot(position, rotation, velocity, ACTOR_WARIO_SIGN);
            vec3f_set(position, -2622.0f, 79.0f, 739.0f);
            add_actor_to_empty_slot(position, rotation, velocity, ACTOR_WARIO_SIGN);
            // d_course_wario_stadium_packed_dl_C50
            find_vtx_and_set_colours(0x07000C50, 0x64, 0xFF, 0xFF, 0xFF);
            // d_course_wario_stadium_packed_dl_BD8
            find_vtx_and_set_colours(0x07000BD8, 0x64, 0xFF, 0xFF, 0xFF);
            // d_course_wario_stadium_packed_dl_B60
            find_vtx_and_set_colours(0x07000B60, 0x64, 0xFF, 0xFF, 0xFF);
            // d_course_wario_stadium_packed_dl_AE8
            find_vtx_and_set_colours(0x07000AE8, 0x64, 0xFF, 0xFF, 0xFF);
            // d_course_wario_stadium_packed_dl_CC8
            find_vtx_and_set_colours(0x07000CC8, 0x64, 0xFF, 0xFF, 0xFF);
            // d_course_wario_stadium_packed_dl_D50
            find_vtx_and_set_colours(0x07000D50, 0x64, 0xFF, 0xFF, 0xFF);
            // d_course_wario_stadium_packed_dl_DD0
            find_vtx_and_set_colours(0x07000DD0, 0x64, 0xFF, 0xFF, 0xFF);
            // d_course_wario_stadium_packed_dl_E48
            find_vtx_and_set_colours(0x07000E48, 0x64, 0xFF, 0xFF, 0xFF);
            break;
        case COURSE_DK_JUNGLE:
            // d_course_dks_jungle_parkway_packed_dl_3FA8
            find_vtx_and_set_colours(0x07003FA8, 0x78, 0xFF, 0xFF, 0xFF);
            break;
        default:
            break;
    }
#else

#endif
    gNumPermanentActors = gNumActors;
}

#ifndef TARGET_N64
#include <objects.h>
#ifdef TARGET_HANDHELD
#include "port/sdl/savestate.h" /* SAVESTATE_EXCLUDE */
#else
#define SAVESTATE_EXCLUDE
#endif
/* MK64_PENGUIN_CAM=<frame>: desktop test (backlog 2, the pupils) -- from that
 * frame on, park player 1 near Sherbet Land's big penguin, facing it, circling
 * it one eighth of a turn every 120 frames so one of the dumps shows its face. */
void debug_penguin_cam(long frame) {
    static long sAt SAVESTATE_EXCLUDE = -2;
    Object* o;
    Player* p = &gPlayers[0];
    u16 phi;
    f32 dx, dz, d;
    if (sAt == -2) {
        const char* e = getenv("MK64_PENGUIN_CAM");
        sAt = e ? atol(e) : -1;
    }
    if (sAt < 0 || frame < sAt || gGamestate != RACING || gCurrentCourseId != COURSE_SHERBET_LAND) {
        return;
    }
    o = &gObjectList[indexObjectList1[0]];
    d = getenv("MK64_PENGUIN_DIST") ? (f32) atof(getenv("MK64_PENGUIN_DIST")) : 70.0f;
    phi = (u16) (((frame - sAt) / 120) * 0x2000);
    dx = sins(phi) * d;
    dz = coss(phi) * d;
    p->pos[0] = o->pos[0] + dx;
    p->pos[2] = o->pos[2] + dz;
    p->velocity[0] = p->velocity[1] = p->velocity[2] = 0.0f;
    p->speed = 0.0f;
    p->rotation[1] = (s16) -atan2s(-dx, -dz); /* the chase camera looks along -rotation */
    if (frame % 120 == 100) {
        port_log_always("penguin cam: frame %ld phi %04X penguin %.0f,%.0f,%.0f dir %04X kart %.0f,%.0f yaw %04X cam %.0f,%.0f -> %.0f,%.0f rot %04X\n", frame, phi,
                        o->pos[0], o->pos[1], o->pos[2], o->direction_angle[1], p->pos[0], p->pos[2], (u16) p->rotation[1],
                        camera1->pos[0], camera1->pos[2], camera1->lookAt[0], camera1->lookAt[2], (u16) camera1->rot[1]);
    }
}
#endif

#ifndef TARGET_N64
#include "camera.h"
#include "render_player.h"
/* MK64_BEHIND_TEST=<frame>: desktop test (backlog 4, near-camera culling) --
 * from that frame on, park CPU kart 1 on the line from the camera to player 1,
 * S units in front of the camera, S falling 2 units every 10 frames from 40 to
 * -10 (behind the camera); log S, the kart's eye distance and whether the game
 * put it in this frame's render list. */
void debug_behind_test(long frame) {
    static long sAt SAVESTATE_EXCLUDE = -2;
    Player* me = &gPlayers[0];
    Player* k = &gPlayers[1];
    f32 dx, dz, len, s;
    s32 i, listed = 0;
    if (sAt == -2) {
        const char* e = getenv("MK64_BEHIND_TEST");
        sAt = e ? atol(e) : -1;
    }
    if (sAt < 0 || frame < sAt || gGamestate != RACING) {
        return;
    }
    s = 40.0f - (f32) (((frame - sAt) / 10) * 2);
    if (s < -10.0f) {
        return;
    }
    dx = me->pos[0] - camera1->pos[0];
    dz = me->pos[2] - camera1->pos[2];
    len = sqrtf(dx * dx + dz * dz);
    if (len < 1.0f) {
        return;
    }
    {
        extern s32 gDebugKartRendered[8];
        listed = gDebugKartRendered[1];
        gDebugKartRendered[1] = 0;
    }
    (void) i;
    if (frame % 10 == 9) {
        port_log_always("behind: frame %ld S %.0f (cam->me %.1f) render_kart %d unk_002 %04X kart y %.1f cam y %.1f\n", frame, s, len,
                        listed, (unsigned) k->unk_002, k->pos[1], camera1->pos[1]);
    }
    k->pos[0] = camera1->pos[0] + dx / len * s;
    k->pos[2] = camera1->pos[2] + dz / len * s;
    k->pos[1] = me->pos[1];
    k->rotation[1] = me->rotation[1];
    k->velocity[0] = k->velocity[1] = k->velocity[2] = 0.0f;
    k->speed = 0.0f;
}
#endif

#ifdef TARGET_PC
#include <stdio.h>
/* gfx_sdl.c's MK64_POSLOG (desktop testing): player 1's position and the mirror flag */
void port_log_p1_pos(long frame) {
    fprintf(stderr, "POS f%ld: gamestate %d mirror %d x %.1f y %.1f z %.1f speed %.2f\n", frame, (int) gGamestate,
            (int) gIsMirrorMode, gPlayerOne->pos[0], gPlayerOne->pos[1], gPlayerOne->pos[2], gPlayerOne->speed);
}
#endif
