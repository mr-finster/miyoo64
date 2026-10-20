/* hh_mm.c, included: the frame boundary, game-state notes and the device watchdog.
 *
 * The watchdog (a thread, on unless MM_WATCHDOG=0; MM_WATCHDOG=<s> sets the stall time, default 5):
 *  - the game loop beats at every frame boundary and every presented frame (HhMm_Beat). No beat for <s>
 *    seconds (the MENU overlay, which holds the loop by design, and a stopped process -- Onion's sleep -- do
 *    not count) and console.log gets one report: the loop's stage, the game state and its phase, the frame,
 *    memory (VmRSS, MemAvailable, swap), the main thread's pc/lr/sp and the code addresses found on its stack
 *    (`addr2line -f -e mm <addr>...` with the unstripped build), and every thread's state and wchan. Again
 *    every 30 s while the stall lasts; one line when frames come back.
 *  - MENU (read from the input device as Onion's keymon does, /dev/input/event0; MIYOO_INPUT_DEV for tests)
 *    with the loop stalled for 2 s or more, or the OS's SIGTERM while it is stalled: the process quits at once
 *    (the clock put back, SDL shut down with a 5 s alarm behind it), so a black screen never needs a reset.
 *    With the loop running, MENU is the game's own (gfx_sdl12.c: the overlay; EXIT GAME quits).
 * Every line goes through port_log_always (flushed per line). */
#include <dirent.h>
#include <sys/syscall.h>
#include <linux/input.h>
#include <poll.h>
#include <time.h>
#include "z64game.h"
#include "z64play.h"
#include "z64save.h"
#include "z64environment.h"
#include "sequence.h"
#include "savestate.h"

extern char __executable_start[], etext[];
extern GameStateOverlay* volatile gHhOvl;
extern GameState* volatile gHhGameState;

static volatile uint64_t sBeatUs;
static volatile int sMenuDepth;
static pthread_t sMainThread;
static uintptr_t sMainStackHi;
static const char* volatile sStateName = "none";
static const char* volatile sStatePhase = "";
static volatile int sSnapReady;
static volatile unsigned long sSnapPc, sSnapLr, sSnapSp;
static uint32_t sSnapWords[40];
static volatile int sSnapN;
static int sStallS = 5;

static uint64_t HhW_Us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
}

/* seconds since the process started (console.log's timing of the start and of each change) */
static double HhW_Since(void) {
    static uint64_t sT0;
    const uint64_t now = HhW_Us();
    if (sT0 == 0) {
        sT0 = now;
    }
    return (now - sT0) / 1e6;
}

void HhMm_Beat(void) {
    sBeatUs = HhW_Us();
}

/* gfx_sdl12.c around the MENU overlay (modal: no frames while it is up) */
void HhMm_MenuDepth(int d) {
    sMenuDepth += d;
    HhMm_Beat();
}

static long HhW_Field(const char* path, const char* key) {
    char line[160];
    long v = -1;
    const size_t kl = strlen(key);
    FILE* f = fopen(path, "r");

    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, key, kl) == 0) {
            v = strtol(line + kl, NULL, 10);
            break;
        }
    }
    fclose(f);
    return v;
}

static void HhW_Mem(char* buf, size_t n) {
    snprintf(buf, n, "rss %ldk vsz %ldk, system avail %ldk free %ldk swapfree %ldk",
             HhW_Field("/proc/self/status", "VmRSS:"), HhW_Field("/proc/self/status", "VmSize:"),
             HhW_Field("/proc/meminfo", "MemAvailable:"), HhW_Field("/proc/meminfo", "MemFree:"),
             HhW_Field("/proc/meminfo", "SwapFree:"));
}

void HhMm_LogMem(const char* why) {
    char m[200];
    HhW_Mem(m, sizeof(m));
    port_log_always("mem: %s: %s\n", why, m);
}

static const char* HhMm_StateName(GameStateOverlay* ovl) {
    static const char* const kNames[] = { "Setup", "MapSelect", "ConsoleLogo", "Play", "TitleSetup", "FileSelect",
                                          "DayTelop" };
    const ptrdiff_t i = ovl - gGameStateOverlayTable;
    return (i >= 0 && i < (ptrdiff_t)ARRAY_COUNT(kNames)) ? kNames[i] : "?";
}

/* Test hooks: whether <frame> is in the env var's comma list ("120,900,4000") */
static int HhW_AtList(const char* name, long frame) {
    const char* e = getenv(name);
    while (e != NULL && *e != '\0') {
        char* end;
        const long v = strtol(e, &end, 10);
        if (end == e) {
            break;
        }
        if (v == frame) {
            return 1;
        }
        e = *end == ',' ? end + 1 : end;
    }
    return 0;
}

/* graph.c (the handheld loop): a game state starts, runs, ends */
void HhMm_StateEvent(const char* what, GameStateOverlay* ovl) {
    char m[200];
    sStateName = HhMm_StateName(ovl);
    sStatePhase = what;
    HhMm_Beat();
    HhW_Mem(m, sizeof(m));
    port_log_always("mm: game state %s %s at frame %ld, %.1f s; %s\n", sStateName, what, sFrames, HhW_Since(), m);
}

/* graph.c, before each frame: 1 = quit */
s32 HhMm_Boundary(GameStateOverlay* ovl) {
    static s16 sScene = -2;
    static s32 sRoom = -2;

    HhMm_Beat();
    {   /* MM_MAPSELECT=1 (tests, the soak): the title's game state entry becomes the map select's, so the N64 logo
         * leads to the map select (the N64 reference gets the same patch as RAM pokes: the same scripted menu) */
        static int sDone;
        if (!sDone) {
            sDone = 1;
            if (getenv("MM_MAPSELECT") != NULL && atoi(getenv("MM_MAPSELECT")) != 0) {
                gGameStateOverlayTable[GAMESTATE_TITLE_SETUP] = gGameStateOverlayTable[GAMESTATE_MAP_SELECT];
                port_log_always("mm: MM_MAPSELECT: the title leads to the map select\n");
            }
        }
    }
    {   /* MM_STALL_AT=<frame> (tests): the loop stops there for good, as the device's black screen did */
        static long sStallAt = -2;
        if (sStallAt == -2) {
            const char* e = getenv("MM_STALL_AT");
            sStallAt = e != NULL ? atol(e) : -1;
        }
        if (sStallAt >= 0 && sFrames >= sStallAt) {
            gHhLoopStage = "MM_STALL_AT";
            for (;;) {
                sleep(60);
            }
        }
    }
    {   /* MM_MENU_EXIT_AT=<frame> (tests): MENU -> EXIT GAME -> confirm, as the player does */
        static long sAt = -2;
        if (sAt == -2) {
            const char* e = getenv("MM_MENU_EXIT_AT");
            sAt = e != NULL ? atol(e) : -1;
        }
        if (sAt >= 0 && sFrames == sAt) {
            extern void port_test_menu_exit(void);
            sAt = -1;
            port_test_menu_exit();
        }
    }
    if (ovl == &gGameStateOverlayTable[3] && gHhGameState != NULL) {
        PlayState* play = (PlayState*)gHhGameState;
        if (play->sceneId != sScene || play->roomCtx.curRoom.num != sRoom) {
            sScene = play->sceneId;
            sRoom = play->roomCtx.curRoom.num;
            port_log_always("mm: scene %d room %d at frame %ld, %.1f s\n", sScene, (int)sRoom, sFrames, HhW_Since());
        }
        {   /* MM_CSLOG=<n> (debug): every n frames, the cutscene's state */
            static int sCsLog = -1;
            if (sCsLog < 0) {
                sCsLog = getenv("MM_CSLOG") != NULL ? atoi(getenv("MM_CSLOG")) : 0;
            }
            if (sCsLog > 0 && sFrames % sCsLog == 0) {
                port_log_always("cs: f%ld layer %d csIndex %04x state %d frame %u script %p idx %u/%u flags %08x\n", sFrames,
                                gSaveContext.sceneLayer, gSaveContext.save.cutsceneIndex, play->csCtx.state,
                                play->csCtx.curFrame, (void*)play->csCtx.script, play->csCtx.scriptIndex,
                                play->csCtx.scriptListCount, (unsigned)play->cutsceneFlags[0]);
                char mem[160];
                HhW_Mem(mem, sizeof(mem));
                port_log_always("mem: f%ld %s\n", sFrames, mem);
                port_log_always("hud: f%ld vis %d next %d alpha health %d a %d b %d minimap %d gameMode %d pause %d "
                                "msgMode %d health %d\n", sFrames, gSaveContext.hudVisibility,
                                gSaveContext.nextHudVisibility, play->interfaceCtx.healthAlpha,
                                play->interfaceCtx.aAlpha, play->interfaceCtx.bAlpha, play->interfaceCtx.minimapAlpha,
                                gSaveContext.gameMode, play->pauseCtx.state, play->msgCtx.msgMode,
                                gSaveContext.save.saveInfo.playerData.health);
            }
        }
    }
    {   /* MM_SAVE_AT / MM_LOAD_AT / MM_EXITSAVE_AT=<frame> (tests): slot 1 saved, slot 1 loaded, EXIT GAME's
         * auto save and quit, at that loop iteration (this process's frames: a load never rewinds them) */
        if (HhW_AtList("MM_SAVE_AT", sFrames)) {
            extern void hh_mm_ptraudit(void);
            hh_mm_ptraudit();
            savestate_request_save(1);
        }
        if (HhW_AtList("MM_LOAD_AT", sFrames)) {
            savestate_request_load(1);
        }
        if (HhW_AtList("MM_EXITSAVE_AT", sFrames)) {
            savestate_request_exit_save();
        }
    }
    {   /* MM_FILENUM_AT=<frame>[:<n>] (tests): the map select's debug save becomes file n (0), in the normal game mode:
         * its save points (Song of Time, Clock Town's) index flash pages by the file number, and 0xFF reads past the
         * page tables; the logo left GAMEMODE_TITLE_SCREEN (no HUD, no pause). The N64 reference gets the same
         * two words poked (gSaveContext + 0x3CA0, + 0x3CA8). */
        static long sAt = -2;
        static int sNum;
        if (sAt == -2) {
            const char* e = getenv("MM_FILENUM_AT");
            sAt = -1;
            if (e != NULL && sscanf(e, "%ld:%d", &sAt, &sNum) < 1) {
                sAt = -1;
            }
        }
        if (sAt >= 0 && sFrames == sAt) {
            sAt = -1;
            port_log_always("mm: MM_FILENUM_AT: file %d (was %d) at frame %ld\n", sNum, (int)gSaveContext.fileNum, sFrames);
            gSaveContext.fileNum = sNum;
            gSaveContext.gameMode = GAMEMODE_NORMAL;
        }
    }
    {   /* MM_ENVLOG=<every> (tests, the release review): the light setting and fog in Play every <every> frames */
        static int sEvery = -1;
        if (sEvery < 0) {
            const char* e = getenv("MM_ENVLOG");
            sEvery = (e != NULL) ? atoi(e) : 0;
        }
        if (sEvery > 0 && sFrames % sEvery == 0 && gHhGameState != NULL && ovl == &gGameStateOverlayTable[3]) {
            PlayState* play = (PlayState*)gHhGameState;
            EnvironmentContext* env = &play->envCtx;
            port_log_always("mm: env f%ld mode %u cfg %u set %u prev %u blend %.2f ovr %u fog %u %u %u near %d far %d | "
                            "lightCtx fog %u %u %u near %d far %d time %04x\n",
                            sFrames, env->lightMode, env->lightConfig, env->lightSetting, env->prevLightSetting,
                            env->lightBlend, env->lightSettingOverride, env->lightSettings.fogColor[0],
                            env->lightSettings.fogColor[1], env->lightSettings.fogColor[2], env->lightSettings.fogNear,
                            env->lightSettings.zFar, play->lightCtx.fogColor[0], play->lightCtx.fogColor[1],
                            play->lightCtx.fogColor[2], play->lightCtx.fogNear, play->lightCtx.zFar,
                            (unsigned)gSaveContext.save.time);
        }
    }
    {   /* MM_SAVEPOKE=<frame>:<offset hex>:<bytes 1|2|4>:<value hex>[,...] (tests, the release review): a field of
         * gSaveContext written at that frame (time +0x0C u16, isNight +0x10, day +0x18); the N64 reference gets the
         * same field poked at 0x801EF670 + offset. Frames ascending. */
        static long sSpAt = -2;
        static const char* sSpNext;
        static unsigned sSpOff, sSpLen, sSpVal;
        if (sSpAt == -2) {
            sSpNext = getenv("MM_SAVEPOKE");
            sSpAt = -1;
        }
        for (;;) {
            while (sSpAt < 0 && sSpNext != NULL && *sSpNext != '\0') {
                const char* c = strchr(sSpNext, ',');
                if (sscanf(sSpNext, "%ld:%x:%u:%x", &sSpAt, &sSpOff, &sSpLen, &sSpVal) < 4 || sSpAt < sFrames) {
                    sSpAt = -1;
                }
                sSpNext = (c != NULL) ? c + 1 : NULL;
            }
            if (sSpAt < 0 || sFrames != sSpAt) {
                break;
            }
            sSpAt = -1;
            if (sSpOff + sSpLen <= sizeof(gSaveContext)) {
                u8* b = (u8*)&gSaveContext + sSpOff;
                if (sSpLen == 1) {
                    *b = (u8)sSpVal;
                } else if (sSpLen == 2) {
                    *(u16*)b = (u16)sSpVal;
                } else {
                    *(u32*)b = sSpVal;
                }
                port_log_always("mm: MM_SAVEPOKE gSaveContext+%#x (%u) = %#x at frame %ld\n", sSpOff, sSpLen, sSpVal, sFrames);
            }
        }
    }
    {   /* MM_WARP=<frame>:<entrance hex> (tests): in Play, a scene change to that entrance at that frame, as a door or a
         * map select entry would (ENTRANCE(scene, spawn) = scene << 9 | spawn << 4) */
        static long sWAt = -2;
        static unsigned sWEnt;
        if (sWAt == -2) {
            const char* e = getenv("MM_WARP");
            sWAt = -1;
            if (e != NULL && sscanf(e, "%ld:%x", &sWAt, &sWEnt) < 2) {
                sWAt = -1;
            }
        }
        if (sWAt >= 0 && sFrames >= sWAt && gHhGameState != NULL && ovl == &gGameStateOverlayTable[3]) {
            PlayState* play = (PlayState*)gHhGameState;
            sWAt = -1;
            play->nextEntrance = (u16)sWEnt;
            play->transitionTrigger = TRANS_TRIGGER_START;
            play->transitionType = TRANS_TYPE_FADE_BLACK;
            port_log_always("mm: MM_WARP to entrance %#x at frame %ld\n", sWEnt, sFrames);
        }
    }
    {   /* MM_SETPOS=<frame>:<x>:<y>:<z>[:<yaw>][,...] (tests): put the player there (world, previous and home
         * position; facing yaw in binary angle units) at that frame, in Play. Frames ascending. */
        static long sPosAt = -2;
        static const char* sPosNext;
        static float sPx, sPy, sPz;
        static int sPyaw;
        if (sPosAt == -2) {
            sPosNext = getenv("MM_SETPOS");
            sPosAt = -1;
        }
        while (sPosAt < 0 && sPosNext != NULL && *sPosNext != '\0') {
            const char* c = strchr(sPosNext, ',');
            sPyaw = -1;
            if (sscanf(sPosNext, "%ld:%f:%f:%f:%d", &sPosAt, &sPx, &sPy, &sPz, &sPyaw) < 4) {
                sPosAt = -1;
            }
            sPosNext = (c != NULL) ? c + 1 : NULL;
            if (sPosAt >= 0 && sPosAt < sFrames) {
                sPosAt = -1;
            }
        }
        if (sPosAt >= 0 && sFrames == sPosAt && gHhGameState != NULL && ovl == &gGameStateOverlayTable[3]) {
            PlayState* play = (PlayState*)gHhGameState;
            Actor* a = (Actor*)GET_PLAYER(play);
            sPosAt = -1;
            if (a != NULL) {
                a->world.pos.x = a->prevPos.x = a->home.pos.x = sPx;
                a->world.pos.y = a->prevPos.y = a->home.pos.y = sPy;
                a->world.pos.z = a->prevPos.z = a->home.pos.z = sPz;
                if (sPyaw >= 0) {
                    a->world.rot.y = a->shape.rot.y = (s16)sPyaw;
                }
                port_log_always("mm: MM_SETPOS player to %.0f %.0f %.0f (yaw %d) at frame %ld\n", sPx, sPy, sPz, sPyaw,
                                sFrames);
            }
        }
    }
    {   /* MM_WARP=<frame>:<entrance hex>[:<day>:<time hex>][,<frame>:<entrance hex>...] (tests, the soak): at that
         * frame, the map select's "load game" (the debug save: every item, mask and song, as file 1) into that
         * entrance from any game state (the title's demo too); in a file's game, the area exit instead (day and
         * time unused). 0xD800 South Clock Town. Frames ascending. */
        static long sWarpAt = -2;
        static const char* sWarpNext;
        static unsigned sWarpEntr, sWarpDay = 1, sWarpTime = 0x4000;
        if (sWarpAt == -2) {
            sWarpNext = getenv("MM_WARP");
            sWarpAt = -1;
        }
        while (sWarpAt < 0 && sWarpNext != NULL && *sWarpNext != '\0') {
            const char* c = strchr(sWarpNext, ',');
            if (sscanf(sWarpNext, "%ld:%x:%u:%x", &sWarpAt, &sWarpEntr, &sWarpDay, &sWarpTime) < 2) {
                sWarpAt = -1;
            }
            sWarpNext = (c != NULL) ? c + 1 : NULL;
            if (sWarpAt >= 0 && sWarpAt < sFrames) {
                sWarpAt = -1; /* already past */
            }
        }
        if (sWarpAt >= 0 && sFrames == sWarpAt && gHhGameState != NULL) {
            GameState* gs = (GameState*)gHhGameState;
            sWarpAt = -1;
            port_log_always("mm: MM_WARP to entrance %04x (day %u time %04x) from %s at frame %ld\n", sWarpEntr,
                            sWarpDay, sWarpTime, sStateName, sFrames);
            if (ovl == &gGameStateOverlayTable[3] && gSaveContext.fileNum != 0xFF) { /* a file's game: exit there */
                PlayState* play = (PlayState*)gs;
                play->nextEntrance = sWarpEntr;
                play->transitionTrigger = TRANS_TRIGGER_START;
                play->transitionType = TRANS_TYPE_FADE_BLACK;
            } else {
                extern void Sram_InitDebugSave(void);
                gSaveContext.fileNum = 0xFF;
                Sram_InitDebugSave();
                gSaveContext.fileNum = 0; /* file 1: the save points (entering Clock Town's) index their pages by it */
                gSaveContext.gameMode = GAMEMODE_NORMAL; /* from the title's demo: GAMEMODE_TITLE_SCREEN hides the HUD */
                gSaveContext.buttonStatus[EQUIP_SLOT_B] = BTN_ENABLED;
                gSaveContext.buttonStatus[EQUIP_SLOT_C_LEFT] = BTN_ENABLED;
                gSaveContext.buttonStatus[EQUIP_SLOT_C_DOWN] = BTN_ENABLED;
                gSaveContext.buttonStatus[EQUIP_SLOT_C_RIGHT] = BTN_ENABLED;
                gSaveContext.buttonStatus[EQUIP_SLOT_A] = BTN_ENABLED;
                gSaveContext.hudVisibilityForceButtonAlphasByStatus = false;
                gSaveContext.nextHudVisibility = HUD_VISIBILITY_IDLE;
                gSaveContext.hudVisibility = HUD_VISIBILITY_IDLE;
                gSaveContext.hudVisibilityTimer = 0;
                gSaveContext.save.entrance = sWarpEntr;
                gSaveContext.save.day = sWarpDay;
                gSaveContext.save.time = sWarpTime;
                gSaveContext.respawn[RESPAWN_MODE_DOWN].entrance = 0xFFFF;
                gSaveContext.seqId = (u8)NA_BGM_DISABLED;
                gSaveContext.ambienceId = AMBIENCE_ID_DISABLED;
                gSaveContext.showTitleCard = true;
                gSaveContext.respawnFlag = 0;
                gSaveContext.respawn[RESPAWN_MODE_GORON].entrance = 0xFF;
                gSaveContext.respawn[RESPAWN_MODE_ZORA].entrance = 0xFF;
                gSaveContext.respawn[RESPAWN_MODE_DEKU].entrance = 0xFF;
                gSaveContext.respawn[RESPAWN_MODE_HUMAN].entrance = 0xFF;
                gWeatherMode = WEATHER_MODE_CLEAR;
                STOP_GAMESTATE(gs);
                SET_NEXT_GAMESTATE(gs, Play_Init, sizeof(PlayState));
            }
        }
    }
    if (sExitRequested && !sExitLegit) {
        port_log_always("mm: exit flag set without a request: value %08x at %p (frame %ld); ignored\n",
                        (unsigned)sExitRequested, (void*)&sExitRequested, sFrames);
        sExitRequested = 0;
    }
    {
        extern void HhFlash_Flush(int now);
        HhFlash_Flush(0);
    }
    gHhLoopStage = "save state check";
    savestate_check();
    gHhLoopStage = "game update";
    return sExitRequested;
}

static void HhW_Snap(int sig, siginfo_t* si, void* ucv) {
    ucontext_t* u = ucv;

    sSnapPc = u->uc_mcontext.arm_pc;
    sSnapLr = u->uc_mcontext.arm_lr;
    sSnapSp = u->uc_mcontext.arm_sp;
    __sync_synchronize();
    sSnapReady = 1;
}

/* One thread's pc/lr/sp (SIGUSR1 to it) and the code addresses on its stack (read through /proc/self/mem, so an
 * unmapped page ends the scan instead of faulting). tid 0: the main thread. */
static void HhW_SnapThread(pid_t tid, const char* what) {
    int i;
    char w[480];
    size_t o = 0;

    sSnapReady = 0;
    if (tid == 0) {
        pthread_kill(sMainThread, SIGUSR1);
    } else {
        syscall(SYS_tgkill, getpid(), tid, SIGUSR1);
    }
    for (i = 0; i < 50 && !sSnapReady; i++) {
        usleep(10000);
    }
    if (!sSnapReady) {
        port_log_always("watchdog: %s did not take the signal\n", what);
        return;
    }
    {
        int fd = open("/proc/self/mem", O_RDONLY);
        uint32_t buf[512];
        uintptr_t at = sSnapSp, end = sSnapSp + 8192;
        int n = 0;
        if (tid == 0 && sMainStackHi != 0 && end > sMainStackHi) {
            end = sMainStackHi;
        }
        w[0] = '\0';
        while (fd >= 0 && at < end && n < 40) {
            const size_t want = (end - at) < sizeof(buf) ? (end - at) & ~3u : sizeof(buf);
            const ssize_t r = pread(fd, buf, want, (off_t)at);
            int k;
            if (r <= 0) {
                break;
            }
            for (k = 0; k < (int)(r / 4) && n < 40; k++) {
                if (buf[k] >= (uintptr_t)__executable_start && buf[k] < (uintptr_t)etext) {
                    o += snprintf(w + o, sizeof(w) - o, " %08x", (unsigned)buf[k]);
                    n++;
                }
            }
            at += (uintptr_t)r;
        }
        if (fd >= 0) {
            close(fd);
        }
    }
    port_log_always("watchdog: %s pc %08lx lr %08lx sp %08lx, code on its stack:%s\n", what, sSnapPc, sSnapLr, sSnapSp,
                    w);
}

static void HhW_Threads(void) {
    char out[1024];
    size_t o = 0;
    DIR* d = opendir("/proc/self/task");
    struct dirent* e;

    out[0] = '\0';
    if (d == NULL) {
        return;
    }
    while ((e = readdir(d)) != NULL && o < sizeof(out) - 80) {
        char path[96], buf[256], wchan[64] = "";
        FILE* f;
        char* rp;
        if (e->d_name[0] == '.') {
            continue;
        }
        snprintf(path, sizeof(path), "/proc/self/task/%s/stat", e->d_name);
        f = fopen(path, "r");
        if (f == NULL) {
            continue;
        }
        buf[0] = '\0';
        if (fgets(buf, sizeof(buf), f) == NULL) {
            buf[0] = '\0';
        }
        fclose(f);
        snprintf(path, sizeof(path), "/proc/self/task/%s/wchan", e->d_name);
        f = fopen(path, "r");
        if (f != NULL) {
            if (fgets(wchan, sizeof(wchan), f) == NULL) {
                wchan[0] = '\0';
            }
            fclose(f);
        }
        rp = strrchr(buf, ')');
        {
            char* lp = strchr(buf, '(');
            char comm[24] = "?";
            size_t wl = strlen(wchan);
            if (wl > 0 && wchan[wl - 1] == '\n') {
                wchan[wl - 1] = '\0';
            }
            if (lp != NULL && rp != NULL && rp > lp) {
                snprintf(comm, sizeof(comm), "%.*s", (int)(rp - lp - 1), lp + 1);
            }
            o += snprintf(out + o, sizeof(out) - o, " %s %s %c %s;", e->d_name, comm,
                          rp != NULL && rp[1] == ' ' ? rp[2] : '?', wchan[0] ? wchan : "-");
        }
    }
    closedir(d);
    port_log_always("watchdog: threads:%s\n", out);
}

static void HhW_Report(uint64_t gap) {
    char m[200];
    int i;

    HhW_Mem(m, sizeof(m));
    port_log_always("watchdog: no frame for %.1f s: stage %s, game state %s (%s), frame %ld; %s\n", gap / 1e6,
                    gHhLoopStage, sStateName, sStatePhase, sFrames, m);
    HhW_SnapThread(0, "main");
    {   /* every other thread but this one */
        DIR* d = opendir("/proc/self/task");
        struct dirent* e;
        const pid_t self = (pid_t)syscall(SYS_gettid), mainTid = getpid();
        while (d != NULL && (e = readdir(d)) != NULL) {
            const pid_t t = (pid_t)atoi(e->d_name);
            char what[24];
            if (t <= 0 || t == self || t == mainTid) {
                continue;
            }
            snprintf(what, sizeof(what), "thread %d", (int)t);
            HhW_SnapThread(t, what);
        }
        if (d != NULL) {
            closedir(d);
        }
    }
    HhW_Threads();
}

static void HhW_Quit(const char* why, uint64_t gap) {
    extern int gfx_wm_shutdown(void);
    port_log_always("watchdog: %s with the game stuck %.1f s: quitting\n", why, gap / 1e6);
    {
        extern void HhFlash_Flush(int now);
        HhFlash_Flush(1);
    }
    port_log_always(gfx_wm_shutdown() ? "exit: SDL shut down\n" : "exit: SDL left up (a flip still running)\n");
    fflush(NULL);
    _exit(0);
}

static void* HhW_Main(void* arg) {
    extern int gfx_miyoo_term_pending(void) __attribute__((weak));
    const char* dev = getenv("MIYOO_INPUT_DEV");
    int fd;
    uint64_t prev = HhW_Us(), stallFrom = 0, lastReport = 0;

    if (dev == NULL || dev[0] == '\0') {
        dev = "/dev/input/event0";
    }
    fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    port_log_always("watchdog: on (a report after %d s with no frame; MENU quits a stuck game; buttons: %s%s)\n", sStallS,
                    dev, fd >= 0 ? "" : " not readable");
    for (;;) {
        struct pollfd pf;
        uint64_t now, gap;
        int menu = 0;

        pf.fd = fd;
        pf.events = POLLIN;
        pf.revents = 0;
        if (fd >= 0) {
            poll(&pf, 1, 250);
        } else {
            usleep(250000);
        }
        now = HhW_Us();
        if (now - prev > 1500000) {
            HhMm_Beat(); /* this thread was stopped too (a sleep): not a stall */
        }
        prev = now;
        if (fd >= 0 && (pf.revents & POLLIN)) {
            struct input_event ev[16];
            ssize_t r;
            while ((r = read(fd, ev, sizeof(ev))) > 0) {
                int k;
                for (k = 0; k < (int)(r / (ssize_t)sizeof(ev[0])); k++) {
                    if (ev[k].type == EV_KEY && ev[k].code == KEY_ESC && ev[k].value == 1) {
                        menu = 1;
                    }
                }
            }
            if (r == 0) {
                usleep(100000); /* a test FIFO with no writer */
            }
        }
        {   /* MM_THREADDUMP=<s> (tests): one full report that many seconds in, stalled or not */
            static long sDumpAt = -2;
            static uint64_t sStart;
            if (sDumpAt == -2) {
                const char* e = getenv("MM_THREADDUMP");
                sDumpAt = e != NULL ? atol(e) : -1;
                sStart = now;
            }
            if (sDumpAt >= 0 && now - sStart >= (uint64_t)sDumpAt * 1000000ull) {
                sDumpAt = -1;
                HhW_Report(sBeatUs < now ? now - sBeatUs : 0);
            }
        }
        if (sMenuDepth > 0) {
            HhMm_Beat();
            continue;
        }
        {
            const uint64_t beat = sBeatUs; /* one read: the game thread may move it past `now` meanwhile */
            gap = beat < now ? now - beat : 0;
        }
        if (sBeatUs != 0 && gap >= (uint64_t)sStallS * 1000000ull) {
            if (stallFrom == 0 || now - lastReport >= 30000000ull) {
                HhW_Report(gap);
                lastReport = now;
            }
            if (stallFrom == 0) {
                stallFrom = sBeatUs;
            }
        } else if (stallFrom != 0 && gap < 1000000ull) {
            port_log_always("watchdog: frames again after %.1f s\n", (sBeatUs - stallFrom) / 1e6);
            stallFrom = 0;
        }
        if (sBeatUs != 0 && gap >= 2000000ull) {
            if (menu) {
                if (stallFrom == 0) {
                    HhW_Report(gap);
                }
                HhW_Quit("MENU", gap);
            }
            if (gfx_miyoo_term_pending != NULL && gfx_miyoo_term_pending()) {
                HhW_Quit("SIGTERM", gap);
            }
        }
    }
    return NULL;
}

static void HhMm_WatchStart(void) {
    const char* e = getenv("MM_WATCHDOG");
    struct sigaction sa;
    pthread_attr_t a;
    pthread_t t;
    FILE* f;
    char line[256];

    if (e != NULL && e[0] == '0') {
        port_log_always("watchdog: off (MM_WATCHDOG=0)\n");
        return;
    }
    if (e != NULL && atoi(e) > 0) {
        sStallS = atoi(e);
    }
    sMainThread = pthread_self();
    f = fopen("/proc/self/maps", "r"); /* the main stack's top: the stack scan stops there */
    if (f != NULL) {
        while (fgets(line, sizeof(line), f) != NULL) {
            unsigned long lo, hi;
            if (strstr(line, "[stack]") != NULL && sscanf(line, "%lx-%lx", &lo, &hi) == 2) {
                sMainStackHi = hi;
            }
        }
        fclose(f);
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = HhW_Snap;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64 * 1024);
    if (pthread_create(&t, &a, HhW_Main, NULL) == 0) {
        pthread_detach(t);
    } else {
        port_log_always("watchdog: off (no thread)\n");
    }
    pthread_attr_destroy(&a);
}

/* MM_PTRAUDIT=1 (tests): at each save, the words of the saved game memory that point into memory a later run
 * maps elsewhere (the brk heap, anonymous maps, the data pack, libraries, stacks; anything but the executable):
 * counted per mapping, with the first few addresses. A state is only good for another process if these are none
 * (or only harmless ones). */
void hh_mm_ptraudit(void) {
    extern char __gamedata_start[], __gamedata_end[], __gamebss_start[], __gamebss_end[], _end[];
    static struct { unsigned long lo, hi; char name[80]; unsigned n; uintptr_t first[4]; } m[128];
    int nm = 0, i;
    FILE* f;
    char line[256];
    const char* e = getenv("MM_PTRAUDIT");

    FILE* sDump;
    if (e == NULL || e[0] != '1') {
        return;
    }
    sDump = getenv("MM_PTRAUDIT_DUMP") ? fopen(getenv("MM_PTRAUDIT_DUMP"), "w") : NULL;
    f = fopen("/proc/self/maps", "r");
    while (f != NULL && nm < 128 && fgets(line, sizeof(line), f) != NULL) {
        unsigned long lo, hi;
        char perms[8], path[200] = "";
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %199[^\n]", &lo, &hi, perms, path) < 3) {
            continue;
        }
        if (lo >= (uintptr_t)__executable_start && hi <= (((uintptr_t)_end + 0xFFF) & ~0xFFFul)) {
            continue; /* the executable's own image: fixed */
        }
        if (lo >= (uintptr_t)__gamedata_start && hi <= (uintptr_t)__gamebss_end + 0x1000) {
            continue;
        }
        m[nm].lo = lo;
        m[nm].hi = hi;
        snprintf(m[nm].name, sizeof(m[nm].name), "%s", path[0] ? path : "[anon]");
        m[nm].n = 0;
        nm++;
    }
    if (f != NULL) {
        fclose(f);
    }
    {
        const struct { char* a; char* b; } r[2] = { { __gamedata_start, __gamedata_end }, { __gamebss_start, __gamebss_end } };
        int k;
        for (k = 0; k < 2; k++) {
            const uint32_t* w;
            for (w = (const uint32_t*)r[k].a; w < (const uint32_t*)r[k].b; w++) {
                const uint32_t v = *w;
                if (v < 0x10000u || (v >= (uintptr_t)__executable_start && v < (uintptr_t)_end)) {
                    continue;
                }
                for (i = 0; i < nm; i++) {
                    if (v >= m[i].lo && v < m[i].hi) {
                        if (sDump != NULL && m[i].hi - m[i].lo < 0x10000000ul) {
                            fprintf(sDump, "%08x %08x %s\n", (unsigned)(uintptr_t)w, (unsigned)v, m[i].name);
                        }
                        if (m[i].n < 4) {
                            m[i].first[m[i].n] = (uintptr_t)w;
                        }
                        m[i].n++;
                        break;
                    }
                }
            }
        }
    }
    for (i = 0; i < nm; i++) {
        if (m[i].n) {
            port_log_always("ptraudit: %u words -> %08lx-%08lx %s (at %08x %08x %08x %08x)\n", m[i].n, m[i].lo, m[i].hi,
                            m[i].name, (unsigned)m[i].first[0], (unsigned)m[i].first[1], (unsigned)m[i].first[2],
                            (unsigned)m[i].first[3]);
        }
    }
    if (sDump != NULL) {
        fclose(sDump);
    }
    port_log_always("ptraudit: done (%d maps checked)\n", nm);
}

void HhMm_TimeStart(void) {
    (void)HhW_Since();
}
