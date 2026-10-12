/**
 * Star Fox 64 on the handheld kit: the game side of the frame loop.
 *
 * On the N64, sys_main.c runs five threads (idle, main, graphics, audio,
 * serial, timer) that meet through message queues: the graphics thread builds
 * a display list per frame, the main thread hands it to the RSP, the VI
 * retrace paces everything at gVIsPerFrame x 1/60 s. The port runs the same
 * steps in order on one thread, once per game frame:
 *
 *   input (Controller_ReadData + Controller_UpdateInput)
 *   Game_Update into the gfx pool's display lists     (Graphics_ThreadEntry)
 *   gfx_run on the master list, present               (the RSP/RDP + VI)
 *   Audio_Update                                      (end of the gfx loop)
 *   the audio thread's ticks, one per VI              (port_audio.c)
 *
 * Pacing is the window backend's (gfx_sdl12.c / gfx_sdl.c deadline pacer),
 * told the frame length through port_set_frame_vis() from gVIsPerFrame.
 */
#include "global.h"
#include "sf64audio_external.h"
#include "port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void port_gfx_run(Gfx* dl);
extern void port_gfx_start_frame(void);
extern void port_gfx_end_frame(void);
extern void port_set_frame_vis(int vis);
extern u32 port_time_us(void);
extern void Main_Initialize(void);
extern void Main_SetVIMode(void);
extern void Main_InitMesgQueues(void);
extern void Graphics_InitializeTask(u32 frameCount);
extern void Controller_Init(void);
extern void Controller_ReadData(void);
extern void Controller_UpdateInput(void);
extern void Graphics_NMIWipe(void);
extern void port_audio_init(void);
extern void port_audio_frame(void);
extern void port_audio_render(int on);

static void build_frame(void) {
    gSPSegment(gUnkDisp1++, 0, 0);
    gSPDisplayList(gMasterDisp++, gGfxPool->unkDL1);
    { /* debug: SF64_BLUR_TEST=<alpha> forces the motion blur (gBlurAlpha, which
       * Game_Update reads first thing) on every frame -- a repro for the
       * renderer's frame history without flying to a warp zone (the title's
       * space scene shows it). */
        static int sBlur = -1; /* debug setting, never in a save */
        if (sBlur < 0) { const char* e = getenv("SF64_BLUR_TEST"); sBlur = e ? atoi(e) & 0xFF : 0; }
        if (sBlur > 0) {
            gBlurAlpha = (u8) sBlur;
        }
    }
    {
        const u32 t0 = port_time_us();
        Game_Update();
        PORT_PROF_ADD(PROF_LOGIC, port_time_us() - t0);
    }
    if (gStartNMI == 1) {
        Graphics_NMIWipe();
    }
    gSPEndDisplayList(gUnkDisp1++);
    gSPEndDisplayList(gUnkDisp2++);
    gSPDisplayList(gMasterDisp++, gGfxPool->unkDL2);
    gDPFullSync(gMasterDisp++);
    gSPEndDisplayList(gMasterDisp++);
}

void port_game_init(void) {
    /* Idle_ThreadEntry's VI setup: gamma on (the N64 VI curve; the art is
     * made for it -- without it everything shows too dark). */
    Main_SetVIMode();
    Main_Initialize();
    Main_InitMesgQueues();
    port_audio_init();
    Controller_Init();
    Game_Initialize();
    gSysFrameCount = 0;
}

void port_game_loop_one_iteration(void) {
    static s32 sLastState = -1, sLastTitle = -1;
    PORT_LOOP_STAGE(PORT_STAGE_TIMERS); /* where the loop is, for the diagnostic line (port.h) */
    if (gGameState != sLastState) { /* boot/menu progress in the log */
        port_log("gamestate %d at frame %u\n", (int) gGameState, (unsigned) gSysFrameCount);
        sLastState = gGameState;
    }
    if (gTitleState != sLastTitle) {
        port_log("titlestate %d at frame %u (next-state timer %d)\n", (int) gTitleState, (unsigned) gSysFrameCount,
                 (int) gNextGameStateTimer);
        sLastTitle = gTitleState;
    }
    gSysFrameCount++;
    { /* the timer thread (Timer_ThreadEntry): run every task whose timer fired */
        extern int port_timers_poll(void);
        extern void Timer_CompleteTask(TimerTask* task);
        OSMesg m;
        port_timers_poll();
        while (osRecvMesg(&gTimerTaskMesgQueue, &m, OS_MESG_NOBLOCK) == 0) {
            Timer_CompleteTask((TimerTask*) m);
        }
    }
    { /* debug: SF64_SMOKE_TEST=1 spawns explosion smoke ahead of the ship
       * every 20 frames in a level (a deterministic repro for effect bugs) */
        static int sSmoke = -1; /* debug setting, never in a save */
        if (sSmoke < 0) { const char* e = getenv("SF64_SMOKE_TEST"); sSmoke = e && e[0] == '1'; }
        if (sSmoke && gGameState == GSTATE_PLAY && gPlayer != NULL && (gSysFrameCount % 20) == 0) {
            extern void Effect_FireSmoke1_Spawn3(f32 xPos, f32 yPos, f32 zPos, f32 scale2);
            Effect_FireSmoke1_Spawn3(gPlayer[0].pos.x, gPlayer[0].pos.y, gPlayer[0].trueZpos - 400.0f, 4.0f);
        }
    }
    { /* test: SF64_STATELOG=<n> logs the game state every n frames (exact hex floats), so a
       * desktop run and a qemu run of the device binary can be diffed line by line: the
       * first differing line is where their game logic parted (the lead's B8 check). */
        static int sStateLog = -1; /* debug setting, never in a save */
        if (sStateLog < 0) { const char* e = getenv("SF64_STATELOG"); sStateLog = e ? atoi(e) : 0; }
        if (sStateLog > 0 && (gSysFrameCount % (u32) sStateLog) == 0) {
            extern s32 sRandSeed1, sRandSeed2, sRandSeed3;
            if (gPlayer != NULL && gGameState == GSTATE_PLAY) {
                port_log("state %u gs %d rng %d %d %d hits %d pos %a %a %a tz %a\n", (unsigned) gSysFrameCount,
                         (int) gGameState, (int) sRandSeed1, (int) sRandSeed2, (int) sRandSeed3, (int) gHitCount,
                         (double) gPlayer[0].pos.x, (double) gPlayer[0].pos.y, (double) gPlayer[0].pos.z,
                         (double) gPlayer[0].trueZpos);
            } else {
                port_log("state %u gs %d rng %d %d %d\n", (unsigned) gSysFrameCount, (int) gGameState,
                         (int) sRandSeed1, (int) sRandSeed2, (int) sRandSeed3);
            }
        }
    }
    PORT_LOOP_STAGE(PORT_STAGE_INPUT);
    Graphics_InitializeTask(gSysFrameCount);
    Controller_ReadData();
    Controller_UpdateInput();

    port_set_frame_vis(gVIsPerFrame);
    /* Pacing and rendering touch no sound state: the audio backstop thread may
     * run the engine meanwhile if the output runs low (sdl/audio_sdl.c). */
    port_audio_render(1);
    PORT_LOOP_STAGE(PORT_STAGE_EVENTS);
    port_gfx_start_frame(); /* pacing (may sleep) and frameskip decision */
    port_audio_render(0);
    PORT_LOOP_STAGE(PORT_STAGE_GAME);
    build_frame();
    { /* gfx buckets as MK64's main.c: 1 = display-list run, 2 = frame end/present */
        extern void port_profile_add(int slot, u32 us);
        PORT_LOOP_STAGE(PORT_STAGE_DRAW);
        port_audio_render(1);
        const u32 t0 = port_time_us();
        port_gfx_run(gGfxPool->masterDL);
        const u32 t1 = port_time_us();
        port_gfx_end_frame();
        port_profile_add(1, t1 - t0);
        port_profile_add(2, port_time_us() - t1);
        port_audio_render(0);
    }
    { /* TRIS: where the frame's triangles went (bring-up diagnostic, gfx_pc.c) */
        extern uint32_t gfx_tri_in, gfx_tri_clipall, gfx_tri_far, gfx_tri_cull, gfx_tri_pass;
        if (gSysFrameCount % 150 == 0) {
            port_log("TRIS/150f in %u clipall %u far %u cull %u pass %u\n", gfx_tri_in, gfx_tri_clipall, gfx_tri_far,
                     gfx_tri_cull, gfx_tri_pass);
            gfx_tri_in = gfx_tri_clipall = gfx_tri_far = gfx_tri_cull = gfx_tri_pass = 0;
        }
    }

    PORT_LOOP_STAGE(PORT_STAGE_AUDIO);
    {
        const u32 t0 = port_time_us();
        Audio_Update();
        port_audio_frame();
        PORT_PROF_ADD(PROF_AUDIO, port_time_us() - t0);
    }
}

/* SF64_GOD=1 with SF64_AUTO=2 (tests, controller_sdl.c): shields back to full every poll, so the autopilot
 * lives through Corneria to the boss (round 19: the boss's death explosion was the device run's worst lag). */
/* SF64_RADIODMG=1 (tests, with SF64_AUTO=2): the radio portrait's red damage flash held on while a message
 * shows -- the translucent quad of round 21's red-square bug (IMG_0192) */
void port_test_radio_damage(void) {
    extern s32 D_80161788;
    static int on = -1;
    if (on < 0) { const char *e = getenv("SF64_RADIODMG"); on = e && e[0] == '1'; }
    if (on && gRadioState >= 4) D_80161788 = 10;
}
void port_test_refill_shields(void) {
    static int sGrangaPolls = -1; /* SF64_GODKILL=n: n polls into the Corneria boss fight, one hit that fells it */
    if (gPlayer != NULL) gPlayer[0].shields = Play_GetMaxShields();
    if (sGrangaPolls < 0) { const char *e = getenv("SF64_GODKILL"); sGrangaPolls = e ? atoi(e) : 0; if (sGrangaPolls <= 0) sGrangaPolls = 0; else sGrangaPolls++; }
    if (sGrangaPolls > 1 && gBosses[0].obj.status == OBJ_ACTIVE && gBosses[0].obj.id == OBJ_BOSS_CO_GRANGA && --sGrangaPolls == 1) {
        gBosses[0].dmgType = DMG_BEAM; gBosses[0].dmgPart = 0; /* GRANGA_DMG_BACKPACK */
        gBosses[0].damage = 1000;
        port_log_always("test: SF64_GODKILL hit the Corneria boss (game frame %d)\n", (int)gGameFrameCount);
    }
    /* the other route's boss, the Attack Carrier: its launch bays take hits only while open, and the carrier
     * only once all three are down, so keep hitting every poll until it is dying (state 8) */
    if (sGrangaPolls > 1 && gBosses[0].obj.status == OBJ_ACTIVE && gBosses[0].obj.id == OBJ_BOSS_CO_CARRIER) sGrangaPolls--;
    if (sGrangaPolls == 1 && gBosses[0].obj.status == OBJ_ACTIVE && gBosses[0].obj.id == OBJ_BOSS_CO_CARRIER && gBosses[0].state < 8) {
        static int said;
        for (int i = 1; i <= 3; i++)
            if (gBosses[i].state == 0 && gBosses[i].health != 0) {
                gBosses[i].dmgType = DMG_BEAM; gBosses[i].dmgPart = i == 1 ? 0 : 2; gBosses[i].damage = 1000;
            }
        gBosses[0].dmgType = DMG_BEAM; gBosses[0].damage = 1000;
        if (!said) { said = 1; port_log_always("test: SF64_GODKILL hitting the Attack Carrier (game frame %d)\n", (int)gGameFrameCount); }
    }
}

/* Round 26, the smart bomb (the owner: "the bomb is still unplayable ... change how it looks"): the blast
 * sphere draws both its sides, two blended bilinear layers over most of the screen. With front faces culled
 * it is one layer from outside and from inside alike (the far / inner surface). SF64_BOMB=0 or bomb=0 in
 * sf64_display.cfg: both layers, as on the N64. */
int port_bomb_one_layer(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SF64_BOMB");
        on = !(e && e[0] == '0');
        if (!e) {
            const char *dir = getenv("SF64_DATA");
            char path[512], line[64];
            snprintf(path, sizeof(path), "%s/sf64_display.cfg", dir ? dir : ".");
            FILE *f = fopen(path, "r");
            if (f) {
                while (fgets(line, sizeof(line), f))
                    if (strncmp(line, "bomb=", 5) == 0 && (line[5] == '0' || line[5] == '1')) on = line[5] - '0';
                fclose(f);
            }
        }
    }
    return on;
}
