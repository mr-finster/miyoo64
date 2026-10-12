#include "audio.h"
#include "audiomgr.h"
#include "assets/objects/gameplay_keep/gameplay_keep.h"
#include "assets/objects/object_link_boy/object_link_boy.h"
#include "assets/objects/object_link_child/object_link_child.h"
#include "assets/objects/gameplay_field_keep/gameplay_field_keep.h"
#include "assets/objects/object_bombiwa/object_bombiwa.h"
#include "assets/objects/object_horse/object_horse.h"
#include "assets/objects/object_kanban/object_kanban.h"
#include "assets/objects/object_kusa/object_kusa.h"
#include "assets/objects/object_mag/object_mag.h"
#include "assets/objects/object_owl/object_owl.h"
#include "assets/objects/object_peehat/object_peehat.h"
#include "assets/objects/object_skb/object_skb.h"
#include "assets/objects/object_spot00_objects/object_spot00_objects.h"
#include "assets/objects/object_wood02/object_wood02.h"
#include "assets/scenes/overworld/spot00/spot00_room_0.h"
#include "array_count.h"
#include "bgcheck.h"
#include "camera.h"
#include "controller.h"
#include "debug.h"
#include "dma.h"
#include "environment.h"
#include "fault.h"
#include "file_select_state.h"
#include "font.h"
#include "console_logo_state.h"
#include "gfx.h"
#include "irqmgr.h"
#include "jpeg.h"
#include "letterbox.h"
#include "libc64/malloc.h"
#include "libu64/runtime.h"
#include "main.h"
#include "map.h"
#include "map_select_state.h"
#include "message.h"
#include "one_point_cutscene.h"
#include "object.h"
#include "padmgr.h"
#include "play_state.h"
#include "player.h"
#include "prenmi_buff.h"
#include "region.h"
#include "regs.h"
#include "rumble.h"
#include "save.h"
#include "sched.h"
#include "segmented_address.h"
#include "scene.h"
#include "setup_state.h"
#include "speed_meter.h"
#include "sram.h"
#include "ss_sram.h"
#include "sys_matrix.h"
#include "sys_cfb.h"
#include "thread.h"
#include "title_setup_state.h"
#include "vi_mode.h"
#include "z_actor_dlftbls.h"
#include "z_game_dlftbls.h"
#include "z_lib.h"
#include "oot_psp_asset_loader.h"
#include "oot_psp_audio_backend.h"
#include "oot_psp_memory.h"
#include "hh_game.h"

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "handheld_menu.h" /* the OS caption on a failed game save (a no-op off the handheld) */

/* HH_GS_*: game state a save state holds (hh_game.h). Left untagged here: sPspSram (the save
 * file's image; a load must not roll back the other files on the card), the framebuffers
 * (addresses only: the renderer draws into its own), and the thread-stack cache. */
s32 gScreenWidth HH_GS_DATA = SCREEN_WIDTH;
s32 gScreenHeight HH_GS_DATA = SCREEN_HEIGHT;
u32 gSystemHeapSize HH_GS_BSS = 0;
Scheduler gScheduler HH_GS_BSS;
PadMgr gPadMgr HH_GS_BSS;
IrqMgr gIrqMgr HH_GS_BSS;
PreNmiBuff* gAppNmiBufferPtr HH_GS_BSS = NULL;
uintptr_t gSegments[NUM_SEGMENTS] HH_GS_BSS;
s32 gCurrentRegion HH_GS_DATA = REGION_US;

#define OOT_PSP_NATIVE_ADDR_START PORT_REAL_PTR_MIN
#define OOT_PSP_NATIVE_ADDR_END 0xFFFFFFFFU
#define OOT_PSP_SEGMENTED_COLLISION_OFFSET_MAX 0x00010000U
#define OOT_PSP_SAVE_PATH "oot-psp-save.bin"
#define OOT_PSP_SAVE_IO_CHUNK_SIZE 0x4000

volatile OSTime gAudioThreadUpdateTimeTotalPerGfxTask HH_GS_BSS = 0;
volatile OSTime gGfxTaskSentToNextReadyMinusAudioThreadUpdateTime HH_GS_BSS = 0;
volatile OSTime gRSPAudioTimeTotal HH_GS_BSS = 0;
volatile OSTime gRSPGfxTimeTotal HH_GS_BSS = 0;
volatile OSTime gRDPTimeTotal HH_GS_BSS = 0;
volatile OSTime gGraphUpdatePeriod HH_GS_BSS = 0;
volatile OSTime gAudioThreadUpdateTimeStart HH_GS_BSS = 0;
volatile OSTime gAudioThreadUpdateTimeAcc HH_GS_BSS = 0;
volatile OSTime gRSPAudioTimeAcc HH_GS_BSS = 0;
volatile OSTime gRSPGfxTimeAcc HH_GS_BSS = 0;
volatile OSTime gRSPOtherTimeAcc HH_GS_BSS = 0;
volatile OSTime D_8016A578 HH_GS_BSS = 0;
volatile OSTime gRDPTimeAcc HH_GS_BSS = 0;

u32 gDmaMgrVerbose HH_GS_BSS = 0;
size_t gDmaMgrDmaBuffSize HH_GS_DATA = DMAMGR_DEFAULT_BUFSIZE;
float qNaN0x10000 = __builtin_nanf("");
Mtx D_01000000 = gdSPDefMtx(1.0f, 0.0f, 0.0f, 0.0f,
                            0.0f, 1.0f, 0.0f, 0.0f,
                            0.0f, 0.0f, 1.0f, 0.0f,
                            0.0f, 0.0f, 0.0f, 1.0f);

#define OOT_PSP_ENTRANCE_FIELD(continueBgm, displayTitleCard, endTransType, startTransType)                  \
    (((continueBgm) ? ENTRANCE_INFO_CONTINUE_BGM_FLAG : 0) |                                                 \
     ((displayTitleCard) ? ENTRANCE_INFO_DISPLAY_TITLE_CARD_FLAG : 0) |                                      \
     (((endTransType) << ENTRANCE_INFO_END_TRANS_TYPE_SHIFT) & ENTRANCE_INFO_END_TRANS_TYPE_MASK) |          \
     (((startTransType) << ENTRANCE_INFO_START_TRANS_TYPE_SHIFT) & ENTRANCE_INFO_START_TRANS_TYPE_MASK))

#define DEFINE_ENTRANCE(_0, sceneId, spawn, continueBgm, displayTitleCard, endTransType, startTransType) \
    { sceneId, spawn,                                                                                    \
      OOT_PSP_ENTRANCE_FIELD(continueBgm, displayTitleCard, endTransType, startTransType) },

EntranceInfo gEntranceTable[ENTR_MAX] = {
#include "tables/entrance_table.h"
};

#undef DEFINE_ENTRANCE

GameStateOverlay gGameStateOverlayTable[GAMESTATE_ID_MAX] HH_GS_DATA = {
    [GAMESTATE_SETUP] = { NULL, ROM_FILE_UNSET, NULL, NULL, NULL, Setup_Init, Setup_Destroy, NULL, NULL, 0,
                          sizeof(SetupState) },
    [GAMESTATE_MAP_SELECT] = { NULL, ROM_FILE_UNSET, NULL, NULL, NULL, MapSelect_Init, NULL, NULL, NULL, 0,
                               sizeof(GameState) },
    [GAMESTATE_CONSOLE_LOGO] = { NULL, ROM_FILE_UNSET, NULL, NULL, NULL, ConsoleLogo_Init, ConsoleLogo_Destroy, NULL,
                                 NULL, 0, sizeof(ConsoleLogoState) },
    [GAMESTATE_PLAY] = { NULL, ROM_FILE_UNSET, NULL, NULL, NULL, Play_Init, Play_Destroy, NULL, NULL, 0,
                         sizeof(PlayState) },
    [GAMESTATE_TITLE_SETUP] = { NULL, ROM_FILE_UNSET, NULL, NULL, NULL, TitleSetup_Init, TitleSetup_Destroy, NULL,
                                NULL, 0, sizeof(TitleSetupState) },
    [GAMESTATE_FILE_SELECT] = { NULL, ROM_FILE_UNSET, NULL, NULL, NULL, FileSelect_Init, NULL, NULL, NULL, 0,
                                sizeof(GameState) },
};

static RegEditor sRegEditor HH_GS_BSS;
RegEditor* gRegEditor = &sRegEditor;

static PreNmiBuff sPspPreNmiBuffer HH_GS_BSS;
/* The game's memory: the system arena, the game states, the scene/room/object heap. */
u8 gOotPspSystemHeap[OOT_PSP_SYSTEM_HEAP_SIZE] __attribute__((aligned(64))) HH_GS_BSS;
static u8 sPspSram[SRAM_SIZE] __attribute__((aligned(64)));
static u16 sPspFramebuffers[2][SCREEN_HEIGHT][SCREEN_WIDTH] __attribute__((aligned(64)));
u16 D_0E000000[SCREEN_HEIGHT * SCREEN_WIDTH] __attribute__((aligned(64)));
u16 D_0F000000[SCREEN_HEIGHT * SCREEN_WIDTH] __attribute__((aligned(64)));
static uintptr_t sSysCfbFbPtr[2] HH_GS_BSS;
static uintptr_t sSysCfbEnd HH_GS_BSS;
static SceUID sPspStackThreadId = -1;
static uintptr_t sPspStackStart;
static uintptr_t sPspStackEnd;
static uintptr_t sPspStackAltStart;
static uintptr_t sPspStackAltEnd;

/* A test boot (OOT_ENTRANCE, debug.cfg entrance=) keeps its saves apart from the real ones. */
static const char* OotPsp_GetSavePath(char* buffer, size_t bufferSize) {
    return OotPsp_ResolveRootPath(getenv("OOT_ENTRANCE") != NULL ? "oot-psp-save-debug.bin" : OOT_PSP_SAVE_PATH,
                                  buffer, bufferSize);
}

/* oot-psp-save.bin is all three save files, so a write never truncates the only
 * copy. The PSP code rewrote it in place (O_TRUNC, no fsync), and a power cut
 * can come at any moment. The new image goes to <save>.tmp,
 * fsync, rename over the save, fsync the renamed file, fsync the directory, so
 * a cut mid-write leaves the old file whole. A cut that leaves only the .tmp,
 * fsynced whole, is recovered by the load, which finishes the rename (MK64
 * 65d7488).
 * The renamed file's own fsync (2026-10-01; super-smash-bros cd5128c, DKR 9c91d40):
 * the card's vfat (Linux 4.14 vfat_rename) moves the new file into the OLD
 * file's directory slot (a rename to a new name: a slot made with cluster 0)
 * and only marks the inode dirty, so the slot's start cluster and size reach
 * the card at that inode's writeback, ~30 s later. The directory fsync writes
 * the removed .tmp entry and the FAT with the old save's clusters freed, not
 * that slot: a hard power cut in those seconds left the save pointing at freed
 * clusters, with no .tmp left for the load (the save lost, the FAT
 * inconsistent). An fsync of the renamed file writes the slot. */
extern void port_log_always(const char* fmt, ...);

/* The directory entry of a file just renamed into place, then its folder, onto the card (above). */
static void OotPsp_SyncRenamed(int fd, const char* path) {
    char dir[400];
    char* slash;

    if (fd >= 0) {
        fsync(fd);
    }
    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (slash != NULL) {
        int dfd;

        *slash = '\0';
        dfd = open(dir[0] != '\0' ? dir : "/", O_RDONLY);
        if (dfd >= 0) {
            fsync(dfd);
            close(dfd);
        }
    }
}

/* A save file that is there but could not be read at the start (an I/O error, a broken cluster
 * chain) is never replaced this session (the 2026-10-01 consistency pass; PM64 742bdb4, SSB64's
 * S3). Read as missing, it gave the game a blank cartridge: Sram_InitSram wrote the default header
 * and the file select's check rewrote every slot, so within seconds of the title screen the
 * player's three files were replaced by empty ones on the card. */
static int sSramKeep;

/* A failed game save: the game ignores SsSram_ReadWrite's result (and says it saved), so a full or
 * read-only card lost the progress with only a log line. GAME SAVE FAILED in the OS bar, as the
 * EEPROM ports do (MK64 37c208f, GoldenEye 3b09a41, DKR 8938223). Once per burst: a save is two
 * writes (the slot and its backup), the file select's first check up to seven. */
static void OotPsp_SaveFailed(void) {
    extern uint64_t port_time_us64(void);
    static uint64_t sShownAt;
    const uint64_t now = port_time_us64();

    if (sShownAt == 0 || now - sShownAt >= 2000000u) {
        sShownAt = now;
        handheld_menu_notify("GAME SAVE FAILED");
    }
}

/* 1 if path held a whole image; a short one is left zero-padded in sPspSram. *bad (if given): the
 * errno when the file is there but could not be opened or read, 0 when it is missing or short. */
static int OotPsp_ReadSramFile(const char* path, int* bad) {
    FILE* f;
    size_t n = 0;

    memset(sPspSram, 0, sizeof(sPspSram));
    errno = 0;
    f = fopen(path, "rb");
    if (f != NULL) {
        n = fread(sPspSram, 1, sizeof(sPspSram), f);
        if (bad != NULL && ferror(f)) {
            *bad = errno != 0 ? errno : EIO;
        }
        fclose(f);
    } else if (bad != NULL && errno != ENOENT) {
        *bad = errno != 0 ? errno : EIO;
    }
    return n == sizeof(sPspSram);
}

static void OotPsp_LoadSram(void) {
    char pathBuffer[384];
    char tmp[400];
    const char* path = OotPsp_GetSavePath(pathBuffer, sizeof(pathBuffer));
    int bad = 0;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (OotPsp_ReadSramFile(path, &bad)) {
        printf("oot-psp save loaded path=%s size=%lu\n", path, (unsigned long)sizeof(sPspSram));
        return;
    }
    if (OotPsp_ReadSramFile(tmp, NULL)) {
        port_log_always("save: %s missing or short, using the .tmp\n", path);
        if (rename(tmp, path) == 0) {
            const int fd = open(path, O_RDONLY);

            OotPsp_SyncRenamed(fd, path);
            if (fd >= 0) {
                close(fd);
            }
        }
        return;
    }
    /* No save yet (zeros: the game formats it), or a short file from an older
     * in-place write: keep what is there, zero-padded -- the game checks each
     * file's checksum. A file that is there but can't be read is kept as it is
     * (sSramKeep). */
    OotPsp_ReadSramFile(path, NULL);
    if (bad != 0) {
        sSramKeep = 1;
        port_log_always("save: %s can't be read (%s): kept as it is, no game save replaces it this session\n", path,
                        strerror(bad));
        return;
    }
    printf("oot-psp save not found or short path=%s\n", path);
}

static s32 OotPsp_FlushSram(void) {
    char pathBuffer[384];
    char tmp[400];
    const char* path = OotPsp_GetSavePath(pathBuffer, sizeof(pathBuffer));
    FILE* f;
    int ok;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    const char* stage = NULL; /* the first failing stage, logged with its strerror (kit 88210e7's rule) */
    int err = 0;

    if (sSramKeep) {
        port_log_always("save: not written: %s could not be read at the start\n", path);
        OotPsp_SaveFailed();
        return false;
    }
    errno = 0;
    f = fopen(tmp, "wb");
    if (f == NULL) {
        port_log_always("save: cannot open %s (%s), %s kept\n", tmp, strerror(errno), path);
        OotPsp_SaveFailed();
        return false;
    }
    errno = 0;
    ok = fwrite(sPspSram, 1, sizeof(sPspSram), f) == sizeof(sPspSram);
    if (!ok) { stage = "write"; err = errno != 0 ? errno : ENOSPC; } /* a short write sets no errno */
    if (fflush(f) != 0 && ok) { ok = 0; stage = "flush"; err = errno; }
    if (ok && fsync(fileno(f)) != 0) { ok = 0; stage = "fsync"; err = errno; }
    /* The file stays open across the rename and is fsynced again after it (the vfat slot, above);
     * its close moves after the rename: its result never mattered after a successful fsync. */
    if (ok && rename(tmp, path) != 0) { ok = 0; stage = "rename"; err = errno; }
    if (!ok) {
        fclose(f);
        port_log_always("save: failed at %s (%s), %s kept\n", stage, strerror(err), path);
        remove(tmp);
        OotPsp_SaveFailed();
        return false;
    }
    OotPsp_SyncRenamed(fileno(f), path);
    fclose(f);
    return true;
}

/* statefix A (handheld-kit k1.8; the kit's savestate.c calls this after a load, mix lock held, before the texture
 * fixup). sPspSram, the card's save (the file as last read or written), is not in a state; the RAM copies the
 * game keeps of it are. So a state older than the card's last write restored older copies: the file select's
 * readBuff (the whole save, written back whole by a copy or a new file) and the header's options in
 * gSaveContext. Sram_HhReapply (z_sram.c) puts the card's back where they differ and says there what it leaves:
 * gSaveContext.save, the adventure in play (written only when the player saves, to its own slot and backup), is
 * the state's. A card that equals the state's copies changes nothing (the load stays exact). Nothing is put back
 * when the save could not be read at the start (sSramKeep) or the state is from before the game read it (the
 * Setup state: ConsoleLogo_Init reads it). One line per load. OOT_SAVEREAPPLY=0: as before (tests). */
void hh_game_save_reapply(void) {
    extern GameState* HhMainLoop_Current(GameStateFunc * init);
    static int sOn = -1;
    GameStateFunc init = NULL;
    GameState* state = HhMainLoop_Current(&init);
    s32 options = 0;
    s32 diff;

    if (sOn < 0) {
        const char* e = getenv("OOT_SAVEREAPPLY");
        sOn = !(e != NULL && e[0] == '0');
    }
    if (!sOn) {
        port_log_always("sram: OOT_SAVEREAPPLY=0: the state's copies of the save kept\n");
        return;
    }
    if (sSramKeep) {
        port_log_always("sram: the card's save could not be read at the start: the state's copies kept\n");
        return;
    }
    if (state == NULL || init == NULL || init == Setup_Init) {
        port_log_always("sram: the state is from before the game read its save: nothing to put back\n");
        return;
    }
    diff = Sram_HhReapply(init == FileSelect_Init ? (FileSelectState*)state : NULL, sPspSram, &options);
    if (diff == 0 && !options) {
        port_log_always("sram: the state's save is the card's\n");
    } else {
        port_log_always("sram: the card's save is newer than the state's (file select copy: %d bytes differ; options: "
                        "%s, sound %d z-target %d): put back\n",
                        (int)diff, options ? "changed" : "same", gSaveContext.soundSetting,
                        gSaveContext.zTargetSetting);
    }
}

/* OOT_SAVEWRITE_AT (hh_main.c, tests, statefix A): in the file select, the whole save written from its readBuff
 * as Sram_CopySave and Sram_InitSave write it (the next copy or new file), and what the file select shows. */
void OotPsp_TestSaveWrite(FileSelectState* fileSelect) {
    u32 h = 2166136261u;
    s32 i;

    SsSram_ReadWrite(OS_K1_TO_PHYSICAL(0xA8000000), fileSelect->sramCtx.readBuff, SRAM_SIZE, OS_WRITE);
    for (i = 0; i < (s32)sizeof(fileSelect->fileNames); i++) {
        h = (h ^ ((u8*)fileSelect->fileNames)[i]) * 16777619u;
    }
    port_log_always("savewrite: the file select's copy written whole; it shows deaths %u %u %u, hearts %u %u %u, "
                    "names %08x; options sound %d z-target %d\n",
                    fileSelect->deaths[0], fileSelect->deaths[1], fileSelect->deaths[2],
                    fileSelect->healthCapacities[0], fileSelect->healthCapacities[1],
                    fileSelect->healthCapacities[2], (unsigned)h, gSaveContext.soundSetting,
                    gSaveContext.zTargetSetting);
}

#ifndef HANDHELD
/* OOT_SAVETEST=1 (desktop): load, flush, check the file and no .tmp left, then
 * a torn write (only the .tmp present) recovers on load. Exits. */
void OotPsp_SaveSelfTest(void) {
    char pathBuffer[384];
    char tmp[400];
    const char* path = OotPsp_GetSavePath(pathBuffer, sizeof(pathBuffer));
    static u8 want[SRAM_SIZE];
    int ok = 1;
    FILE* f;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    OotPsp_LoadSram();
    memcpy(want, sPspSram, sizeof(want));
    want[123] ^= 0x5A; /* a change to write */
    memcpy(sPspSram, want, sizeof(want));
    ok = ok && OotPsp_FlushSram();
    ok = ok && access(tmp, F_OK) != 0;
    ok = ok && OotPsp_ReadSramFile(path, NULL) && memcmp(sPspSram, want, sizeof(want)) == 0;
    /* torn write: the save gone, only the .tmp there */
    rename(path, tmp);
    OotPsp_LoadSram();
    ok = ok && memcmp(sPspSram, want, sizeof(want)) == 0 && access(path, F_OK) == 0 && access(tmp, F_OK) != 0;
    /* restore the original byte */
    want[123] ^= 0x5A;
    memcpy(sPspSram, want, sizeof(want));
    ok = ok && OotPsp_FlushSram();
    f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
    }
    printf("savetest: %s\n", ok ? "PASS" : "FAIL");
    exit(ok ? 0 : 1);
}
#endif

extern u8 __bss_start[];
extern u8 _end[];

static int OotPsp_IsContainedRange(const void* ptr, size_t size, uintptr_t rangeStart, uintptr_t rangeEnd) {
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end;

    if ((ptr == NULL) || (size == 0) || (start > UINTPTR_MAX - size)) {
        return false;
    }

    end = start + size;
    return (start >= rangeStart) && (end <= rangeEnd);
}

static void OotPsp_UpdateCurrentThreadStackRange(void) {
    SceUID threadId = sceKernelGetThreadId();
    SceKernelThreadInfo threadInfo;
    uintptr_t stack;
    uintptr_t stackSize;

    if (threadId == sPspStackThreadId) {
        return;
    }

    sPspStackThreadId = threadId;
    sPspStackStart = 0;
    sPspStackEnd = 0;
    sPspStackAltStart = 0;
    sPspStackAltEnd = 0;

    memset(&threadInfo, 0, sizeof(threadInfo));
    threadInfo.size = sizeof(threadInfo);
    if (sceKernelReferThreadStatus(threadId, &threadInfo) < 0) {
        return;
    }

    stack = (uintptr_t)threadInfo.stack;
    stackSize = (uintptr_t)threadInfo.stackSize;
    if ((stack == 0) || (stackSize == 0)) {
        return;
    }

    if (stack <= (UINTPTR_MAX - stackSize)) {
        sPspStackStart = stack;
        sPspStackEnd = stack + stackSize;
    }
    if (stack >= stackSize) {
        sPspStackAltStart = stack - stackSize;
        sPspStackAltEnd = stack;
    }
}

int OotPsp_IsRuntimeByteRangeSlow(uintptr_t start, uintptr_t end) {
    OotPsp_UpdateCurrentThreadStackRange();

    return ((start >= sPspStackStart) && (end <= sPspStackEnd)) ||
           ((start >= sPspStackAltStart) && (end <= sPspStackAltEnd));
}

static s32 OotPsp_IsNativeUserRange(uintptr_t addr, size_t size) {
    if (size == 0) {
        return true;
    }

    if ((addr < OOT_PSP_NATIVE_ADDR_START) || (addr >= OOT_PSP_NATIVE_ADDR_END)) {
        return false;
    }

    return size <= (OOT_PSP_NATIVE_ADDR_END - addr);
}

/* handheld: there are no KSEG aliases in this address map. gSegments holds
 * OS_K0_TO_PHYSICAL(ptr) = ptr - K0BASE (mod 2^32); translation adds K0BASE back
 * (OotPsp_TranslateSegmentedAddress), so any other value passes through as is. */
static uintptr_t OotPsp_StripKernelAlias(uintptr_t addr) {
    return addr;
}

static s32 OotPsp_IsStaticStorageRange(const void* ptr, size_t size) {
    if (OotPsp_IsContainedRange(ptr, size, (uintptr_t)gOotPspSystemHeap,
                                (uintptr_t)gOotPspSystemHeap + sizeof(gOotPspSystemHeap))) {
        return true;
    }

    if (OotPsp_IsContainedRange(ptr, size, (uintptr_t)sPspSram,
                                (uintptr_t)sPspSram + sizeof(sPspSram))) {
        return true;
    }

    if (OotPsp_IsContainedRange(ptr, size, (uintptr_t)sPspFramebuffers,
                                (uintptr_t)sPspFramebuffers + sizeof(sPspFramebuffers))) {
        return true;
    }

    if (OotPsp_IsContainedRange(ptr, size, (uintptr_t)D_0E000000,
                                (uintptr_t)D_0E000000 + sizeof(D_0E000000))) {
        return true;
    }

    if (OotPsp_IsContainedRange(ptr, size, (uintptr_t)D_0F000000,
                                (uintptr_t)D_0F000000 + sizeof(D_0F000000))) {
        return true;
    }

    if (OotPsp_IsContainedRange(ptr, size, (uintptr_t)&sPspPreNmiBuffer,
                                (uintptr_t)&sPspPreNmiBuffer + sizeof(sPspPreNmiBuffer))) {
        return true;
    }

    return false;
}

static s32 OotPsp_IsKnownNativePointer(uintptr_t addr, size_t size) {
    u32 loadedFlags;

    addr = OotPsp_StripKernelAlias(addr);

    if (!OotPsp_IsNativeUserRange(addr, size)) {
        return false;
    }

    /*
     * Keep code/rodata/data before .bss automatically native, but do not use
     * addr < _end.  _end includes large .bss buffers and can overlap raw OoT
     * segment-looking values such as 0x09001234 on real PSP hardware.
     */
    if (addr < (uintptr_t)__bss_start) {
        return true;
    }

    if (OotPsp_IsStaticStorageRange((const void*)addr, size)) {
        return true;
    }

    if (OotPsp_IsRuntimeByteRange((void*)addr, size)) {
        return true;
    }

    if (OotPsp_GetLoadedExternalAssetRangeFlags((void*)addr, size, &loadedFlags)) {
        return true;
    }

    return false;
}

static s32 OotPsp_AddressLooksSegmented(uintptr_t addr, u32 segment) {
    uintptr_t offset;

    addr = OotPsp_StripKernelAlias(addr);
    offset = SEGMENT_OFFSET(addr);

    if ((segment == 0) || (segment >= NUM_SEGMENTS)) {
        return false;
    }

    if ((addr & 0xF0000000U) != 0) {
        return false;
    }

    if (OotPsp_IsKnownNativePointer(addr, 1)) {
        return false;
    }

    if ((addr < OOT_PSP_NATIVE_ADDR_START) || (addr >= OOT_PSP_NATIVE_ADDR_END)) {
        return true;
    }

    if (offset < OOT_PSP_SEGMENTED_COLLISION_OFFSET_MAX) {
        return true;
    }

    /*
     * Segments 8-B overlap the PSP user-memory window.  If the address is in
     * that window but is not a known live PSP allocation, treat it as segmented
     * instead of passing a fake native pointer to the renderer.
     */
    return true;
}

static void* OotPsp_TranslateSegmentedAddress(uintptr_t addr, u32 segment) {
    uintptr_t base = (uintptr_t)gSegments[segment] + K0BASE;
    uintptr_t offset = SEGMENT_OFFSET(addr);

    /*
     * Do NOT add K0BASE here.  On real PSP that turns a valid user pointer such
     * as 0x0901A590 into 0x8901A590, matching the crash BadVAddr 0x8901BD97.
     */
    return (void*)(base + offset);
}

static void OotPsp_LogUnmappedSegment(uintptr_t addr, u32 segment) {
    static s32 sUnmappedSegmentLogCount = 0;

    addr = OotPsp_StripKernelAlias(addr);

    if (sUnmappedSegmentLogCount < 16) {
        printf("oot-psp cpu unmapped segment addr=%08lx segment=%lu offset=%06lx\n", (unsigned long)addr,
               (unsigned long)segment, (unsigned long)SEGMENT_OFFSET(addr));
    } else if (sUnmappedSegmentLogCount == 16) {
        printf("oot-psp cpu unmapped segment logs suppressed\n");
    }

    sUnmappedSegmentLogCount++;
}

void* SegmentedToVirtualCompat(uintptr_t addr) {
    u32 segment;
    s32 looksSegmented;

    if (addr == 0) {
        return NULL;
    }

    addr = OotPsp_StripKernelAlias(addr);
    segment = SEGMENT_NUMBER(addr);
    looksSegmented = OotPsp_AddressLooksSegmented(addr, segment);

    if (looksSegmented) {
        if (gSegments[segment] != 0) {
            return OotPsp_TranslateSegmentedAddress(addr, segment);
        }

        OotPsp_LogUnmappedSegment(addr, segment);
        return NULL;
    }

    return (void*)addr;
}

void* SegmentedToVirtualExplicit(uintptr_t addr) {
    u32 segment;

    if (addr == 0) {
        return NULL;
    }

    addr = OotPsp_StripKernelAlias(addr);
    segment = SEGMENT_NUMBER(addr);

    if ((segment == 0) || (segment >= NUM_SEGMENTS) || ((addr & 0xF0000000U) != 0)) {
        return (void*)addr;
    }

    if (gSegments[segment] != 0) {
        return OotPsp_TranslateSegmentedAddress(addr, segment);
    }

    OotPsp_LogUnmappedSegment(addr, segment);
    return NULL;
}

void Regs_Init(void) {
    memset(&sRegEditor, 0, sizeof(sRegEditor));
    R_UPDATE_RATE = 1;
    R_PAUSE_BG_PRERENDER_STATE = PAUSE_BG_PRERENDER_OFF;
    R_GRAPH_TASKSET00_FLAGS = 0;
    R_ENABLE_FB_FILTER = 0;
}

void SysCfb_Init(UNUSED s32 n64dd) {
    memset(sPspFramebuffers, 0, sizeof(sPspFramebuffers));
    sSysCfbFbPtr[0] = (uintptr_t)&sPspFramebuffers[0][0][0];
    sSysCfbFbPtr[1] = (uintptr_t)&sPspFramebuffers[1][0][0];
    sSysCfbEnd = (uintptr_t)&sPspFramebuffers[ARRAY_COUNT(sPspFramebuffers)];
}

void SysCfb_Reset(void) {
    sSysCfbFbPtr[0] = 0;
    sSysCfbFbPtr[1] = 0;
    sSysCfbEnd = 0;
}

void* SysCfb_GetFbPtr(s32 idx) {
    if (idx < 0 || idx >= ARRAY_COUNT(sSysCfbFbPtr)) {
        return NULL;
    }
    return (void*)sSysCfbFbPtr[idx];
}

void* SysCfb_GetFbEnd(void) {
    return (void*)sSysCfbEnd;
}

void DmaMgr_Init(void) {
}

static s32 OotPsp_IsNativeRange(uintptr_t addr, size_t size) {
    if (size == 0) {
        return true;
    }

    if ((addr < OOT_PSP_NATIVE_ADDR_START) || (addr >= OOT_PSP_NATIVE_ADDR_END)) {
        return false;
    }

    return size <= (OOT_PSP_NATIVE_ADDR_END - addr);
}

static s32 OotPsp_DmaReadInternal(void* ram, uintptr_t vrom, size_t size, s32 isAudioRead) {
    s32 status;

    if (isAudioRead) {
        status = (OotPspAudioBackend_NeedsRefillUrgently() || OotPspAudioBackend_NeedsRefillDuringIo())
                     ? OotPsp_AssetReadAudioUrgent(ram, vrom, size)
                     : OotPsp_AssetReadAudio(ram, vrom, size);
    } else {
        status = OotPsp_AssetRead(ram, vrom, size);
    }

    if (status == OOT_PSP_ASSET_READ_OK) {
        return 0;
    }

    if (status == OOT_PSP_ASSET_READ_FAILED) {
        Fault_AddHungupAndCrash(__FILE__, __LINE__);
    }

    if (!OotPsp_IsNativeRange(vrom, size)) {
        uintptr_t normalizedVrom = OotPsp_NormalizeVrom(vrom);

        printf("oot-psp dma invalid fallback ram=%p vrom=%08lx normalized=%08lx size=%lu status=%ld\n", ram,
               (unsigned long)vrom, (unsigned long)normalizedVrom, (unsigned long)size, (long)status);
        Fault_AddHungupAndCrash(__FILE__, __LINE__);
    }

    if (isAudioRead) {
        memcpy(ram, (const void*)vrom, size);
    } else {
        OotPsp_MemcpyVfpu(ram, (const void*)vrom, size);
    }
    return 0;
}

static s32 OotPsp_DmaRead(void* ram, uintptr_t vrom, size_t size) {
    return OotPsp_DmaReadInternal(ram, vrom, size, false);
}

s32 DmaMgr_RequestAsync(DmaRequest* req, void* ram, uintptr_t vrom, size_t size, u32 unk5, OSMesgQueue* queue, OSMesg msg) {
    (void)unk5;
    OotPsp_DmaRead(ram, vrom, size);

    if (req != NULL) {
        req->dramAddr = ram;
        req->vromAddr = vrom;
        req->size = size;
        req->notifyQueue = queue;
        req->notifyMsg = msg;
    }

    if (queue != NULL) {
        osSendMesg(queue, msg, OS_MESG_NOBLOCK);
    }

    return 0;
}

s32 DmaMgr_RequestSync(void* ram, uintptr_t vrom, size_t size) {
    return OotPsp_DmaRead(ram, vrom, size);
}

s32 DmaMgr_DmaRomToRam(uintptr_t rom, void* ram, size_t size) {
    return OotPsp_DmaRead(ram, rom, size);
}

void DmaMgr_DmaFromDriveRom(void* ram, uintptr_t rom, size_t size) {
    OotPsp_DmaRead(ram, rom, size);
}

s32 DmaMgr_AudioDmaHandler(UNUSED OSPiHandle* pihandle, OSIoMesg* mb, UNUSED s32 direction) {
    if (mb == NULL) {
        return -1;
    }

    OotPsp_DmaReadInternal(mb->dramAddr, mb->devAddr, mb->size, true);
    if (mb->hdr.retQueue != NULL) {
        osSendMesg(mb->hdr.retQueue, mb, OS_MESG_NOBLOCK);
    }
    return 0;
}

void PadMgr_RequestPadData(PadMgr* padMgr, Input* inputs, UNUSED s32 gameRequest) {
    OSContPad pads[MAXCONTROLLERS];
    Input* input = &inputs[0];

    osContStartReadData(NULL);
    osContGetReadData(pads);

    input->prev = padMgr->inputs[0].cur;
    input->cur = pads[0];
    input->press.button = (input->cur.button ^ input->prev.button) & input->cur.button;
    input->press.stick_x = input->cur.stick_x - input->prev.stick_x;
    input->press.stick_y = input->cur.stick_y - input->prev.stick_y;
    input->rel.button = (input->cur.button ^ input->prev.button) & input->prev.button;
    input->rel.stick_x = input->cur.stick_x;
    input->rel.stick_y = input->cur.stick_y;

    memset(&inputs[1], 0, sizeof(Input) * (MAXCONTROLLERS - 1));
    padMgr->inputs[0] = *input;
    padMgr->validCtrlrsMask = 1;
    padMgr->ctrlrIsConnected[0] = true;
    padMgr->nControllers = 1;
}

void PadMgr_RumbleStop(UNUSED PadMgr* padMgr) {
}

void PadMgr_RumbleReset(PadMgr* padMgr) {
    s32 i;

    for (i = 0; i < ARRAY_COUNT(padMgr->rumbleEnable); i++) {
        padMgr->rumbleEnable[i] = 0;
        padMgr->rumbleTimer[i] = 0;
    }
}

void PadMgr_RumbleSetSingle(PadMgr* padMgr, u32 port, u32 rumble) {
    if (port < ARRAY_COUNT(padMgr->rumbleEnable)) {
        padMgr->rumbleEnable[port] = rumble != 0;
    }
}

void PadMgr_RumbleSet(PadMgr* padMgr, u8* enable) {
    s32 i;

    if (enable != NULL) {
        for (i = 0; i < ARRAY_COUNT(padMgr->rumbleEnable); i++) {
            padMgr->rumbleEnable[i] = enable[i];
        }
    }
}

void SpeedMeter_Init(SpeedMeter* this) {
    memset(this, 0, sizeof(*this));
}

void SpeedMeter_Destroy(UNUSED SpeedMeter* this) {
}

void SpeedMeter_DrawTimeEntries(UNUSED SpeedMeter* this, UNUSED GraphicsContext* gfxCtx) {
}

void SpeedMeter_DrawAllocEntries(UNUSED SpeedMeter* meter, UNUSED GraphicsContext* gfxCtx, UNUSED GameState* state) {
}

void Debug_DrawText(UNUSED GraphicsContext* gfxCtx) {
}

void SsSram_ReadWrite(s32 addr, void* dramAddr, size_t size, s32 direction) {
    uintptr_t base = OS_K1_TO_PHYSICAL(0xA8000000);
    uintptr_t offset;

    if (dramAddr == NULL) {
        return;
    }

    if ((uintptr_t)addr < base) {
        return;
    }

    offset = (uintptr_t)addr - base;
    if (offset >= sizeof(sPspSram)) {
        return;
    }

    if (size > (sizeof(sPspSram) - offset)) {
        size = sizeof(sPspSram) - offset;
    }

    if (direction == OS_READ) {
        memcpy(dramAddr, &sPspSram[offset], size);
    } else {
        memcpy(&sPspSram[offset], dramAddr, size);
        OotPsp_FlushSram();
    }
}

void LogUtils_HungupThread(const char* name, int line) {
    Fault_AddHungupAndCrash(name, line);
}

void LogUtils_ResetHungup(void) {
}

void Fault_Init(void) {
}

void Fault_AddClient(FaultClient* client, void* callback, void* arg0, void* arg1) {
    if (client != NULL) {
        client->callback = callback;
        client->arg0 = arg0;
        client->arg1 = arg1;
    }
}

void Fault_RemoveClient(FaultClient* client) {
    if (client != NULL) {
        client->callback = NULL;
        client->arg0 = NULL;
        client->arg1 = NULL;
    }
}

static void OotPsp_FaultWrite(const char* msg) {
    osSyncPrintf("%s", msg);
    sceIoWrite(1, msg, strlen(msg));
}

NORETURN void oot_psp_assert(const char* assertion, const char* file, int line) {
    char msg[256];

    snprintf(msg, sizeof(msg), "oot-psp assert failed: %s (%s:%d)\n", assertion != NULL ? assertion : "(null)",
             file != NULL ? file : "(null)", line);
    OotPsp_FaultWrite(msg);
    Fault_AddHungupAndCrash(file, line);
}

s32 Fault_Printf(const char* fmt, ...) {
    char buf[512];
    va_list args;
    s32 written;

    va_start(args, fmt);
    written = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (written < 0) {
        return written;
    }

    osSyncPrintf("%s", buf);
    return written;
}

void Fault_SetFontColor(UNUSED u16 color) {
}

void Fault_SetCursor(UNUSED s32 x, UNUSED s32 y) {
}

void Fault_SetCharPad(UNUSED s8 padW, UNUSED s8 padH) {
}

NORETURN void Fault_AddHungupAndCrashImpl(const char* exp1, const char* exp2) {
    char msg[256];

    snprintf(msg, sizeof(msg), "oot-psp fault: %s %s\n", exp1 != NULL ? exp1 : "(null)",
             exp2 != NULL ? exp2 : "(null)");
    OotPsp_FaultWrite(msg);
    sceKernelExitGame();
    while (true) {
        sceKernelDelayThread(1000000);
    }
}

NORETURN void Fault_AddHungupAndCrash(const char* file, int line) {
    char msg[256];

    snprintf(msg, sizeof(msg), "oot-psp fault at %s:%d\n", file != NULL ? file : "(null)", line);
    OotPsp_FaultWrite(msg);
    sceKernelExitGame();
    while (true) {
        sceKernelDelayThread(1000000);
    }
}

void OotPspGame_Init(void) {
    memset(&gScheduler, 0, sizeof(gScheduler));
    memset(&gPadMgr, 0, sizeof(gPadMgr));
    memset(&gIrqMgr, 0, sizeof(gIrqMgr));
    memset(gSegments, 0, sizeof(gSegments));

    gCurrentRegion = REGION_US;
    SaveContext_Init();
#ifndef HANDHELD
    if (getenv("OOT_SAVETEST") != NULL) {
        OotPsp_SaveSelfTest();
    }
#endif
    OotPsp_LoadSram();
    gAppNmiBufferPtr = &sPspPreNmiBuffer;
    PreNmiBuff_Init(gAppNmiBufferPtr);

    SysCfb_Init(0);
    osViSwapBuffer(SysCfb_GetFbPtr(0));

    gSystemHeapSize = sizeof(gOotPspSystemHeap);
    Runtime_Init(gOotPspSystemHeap, gSystemHeapSize);
    Regs_Init();
    Letterbox_Init();

    gPadMgr.validCtrlrsMask = 1;
    gPadMgr.ctrlrIsConnected[0] = true;
    gPadMgr.nControllers = 1;

    Sched_Init(&gScheduler, NULL, THREAD_PRI_SCHED, 0, 0, &gIrqMgr);
    OotPspAudio_Init();
}
