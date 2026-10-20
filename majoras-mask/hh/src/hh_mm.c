/* Majora's Mask: the game-side platform glue the N64 boot, idle, DMA, IRQ, pad, fault and flash code
 * provided (src/boot, src/code/main.c, padmgr.c, osFlash.c are left out of the build), and the entry
 * point. The Ocarina of Time port's hh_main.c / hh_game.c, for MM's boot sequence (code/main.c's Main):
 * the main thread runs Graph_ThreadEntry (code/graph.c), which loops the gamestates; each frame's display
 * list is drawn synchronously when the game hands the task to the scheduler (hh_sched.c Sched_Notify).
 *
 * Game data: the data pack (hh/tools/mm_pack.py, from the player's ROM), mapped read-only. It keeps the
 * ROM's vrom layout, so DMA requests are copies from pack + vrom. */
#include "ultra64.h"
#include "PR/os_internal_flash.h"
#include "audiomgr.h"
#include "buffers.h"
#include "fault.h"
#include "irqmgr.h"
#include "padmgr.h"
#include "regs.h"
#include "scheduler.h"
#include "segmented_address.h"
#include "stack.h"
#include "sys_cfb.h"
#include "viconfig.h"
#include "z64dma.h"
#include "z64thread.h"
#include "z64save.h"
#include "libu64/system_heap.h"
#include "libc64/malloc.h"
#include "hh_game.h"
#include "savestate.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <ucontext.h>

void port_log(const char* fmt, ...);
void port_log_always(const char* fmt, ...);
const char* port_save_path(const char* name);
uint32_t port_time_us(void);
void OotPspRenderer_Init(void);
void Graph_ThreadEntry(void* arg);

/* ---- what code/main.c and boot/ defined ---------------------------------------------------------- */
uintptr_t gSegments[NUM_SEGMENTS] HH_GS_BSS;
Scheduler gScheduler HH_GS_BSS;
PadMgr gPadMgr HH_GS_BSS;
IrqMgr gIrqMgr HH_GS_BSS;
s32 gScreenWidth HH_GS_DATA = SCREEN_WIDTH;
s32 gScreenHeight HH_GS_DATA = SCREEN_HEIGHT;
size_t gSystemHeapSize HH_GS_BSS;
vs32 gIrqMgrResetStatus HH_GS_BSS;
volatile OSTime gIrqMgrRetraceTime HH_GS_BSS;
volatile OSTime sIrqMgrResetTime HH_GS_BSS;
s32 sIrqMgrRetraceCount HH_GS_BSS;
u32 gViConfigFeatures HH_GS_DATA = OS_VI_DITHER_FILTER_ON | OS_VI_GAMMA_OFF;
f32 gViConfigXScale HH_GS_DATA = 1.0f;
f32 gViConfigYScale HH_GS_DATA = 1.0f;
u8 gViConfigModeType HH_GS_BSS;
OSViMode gViConfigMode;
OSViMode osViModeNtscLan1;
OSViMode osViModeNtscHpf1;
OSViMode osViModeNtscHpn1;
OSViMode osViModeNtscLpn1;
OSViMode osViModeMpalLan1;
OSViMode osViModeFpalLan1;
OSViMode osViModePalLan1;
f32 __libm_qnan_f = __builtin_nanf("");
f32 qNaN0x10000 = __builtin_nanf("");
size_t gDmaMgrDmaBuffSize = DMAMGR_DEFAULT_BUFSIZE;
u64* gAudioSPDataPtr;
/* the RSP microcode: never run (the renderer and the mixer are the RSP), only their addresses taken */
u64 rspbootTextStart[1], rspbootTextEnd[1];
u64 gspF3DZEX2_NoN_PosLight_fifoTextStart[1], gspF3DZEX2_NoN_PosLight_fifoDataStart[1];
u64 gspS2DEX2_fifoTextStart[1], gspS2DEX2_fifoDataStart[1];
u64 aspMainStack[0x400 / 8];

#ifndef HH_RELEASE
#define HH_RELEASE "dev"
#endif
const char* volatile gHhLoopStage = "boot";
static volatile int sExitRequested;
static volatile int sExitLegit;
void port_request_exit(void) {
    sExitLegit = 1;
    if (!sExitRequested) {
        port_log_always("mm: exit requested (from %p)\n", __builtin_return_address(0));
    }
    sExitRequested = 1;
}
int HhMm_ExitRequested(void) {
    return sExitRequested;
}
void port_audio_out_set_muted(int muted) __attribute__((weak));
void port_audio_out_set_muted(int muted) {
}

/* ---- the data pack --------------------------------------------------------------------------------- */
static const u8* sPack;
static size_t sPackSize;

static int HhPack_Open(void) {
    const char* cands[6];
    int n = 0;
    const char* env = getenv("MM_PACK");
    int i;

    if (env != NULL) {
        cands[n++] = env;
    }
    cands[n++] = port_save_path("mm_pack.bin");
    {   /* next to the executable (a PERSONAL zip's App/MajorasMask/mm_pack.bin) */
        static char exeDir[600];
        const ssize_t k = readlink("/proc/self/exe", exeDir, sizeof(exeDir) - 16);
        char* sl;
        exeDir[k > 0 ? k : 0] = '\0';
        sl = strrchr(exeDir, '/');
        if (k > 0 && sl != NULL) {
            strcpy(sl + 1, "mm_pack.bin");
            cands[n++] = exeDir;
        }
    }
    cands[n++] = "build/hh-pack.bin";
    for (i = 0; i < n; i++) {
        struct stat st;
        int fd = open(cands[i], O_RDONLY);

        if (fd < 0) {
            continue;
        }
        if (fstat(fd, &st) == 0 && st.st_size > 0) {
            void* p = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

            if (p != MAP_FAILED) {
                sPack = p;
                sPackSize = st.st_size;
                close(fd);
                port_log_always("mm: data pack %s (%u bytes) at %p\n", cands[i], (unsigned)sPackSize, p);
                {   /* 1.0.1: the whole pack read ahead now, in big sequential reads, off the game thread (the kernel's
                     * readahead). 1.0.0 faulted it in one 4 KB page at a time from the SD card as each scene, object
                     * and sound bank was first used: on a Mini Plus the first visits (the intro, the first minutes of
                     * a session) stalled the game and the audio. The pages are the file's: the kernel can drop them
                     * again under memory pressure. madvise as a raw system call (no GOT room for an import). */
                    register long r0 __asm__("r0") = (long)p;
                    register long r1 __asm__("r1") = (long)sPackSize;
                    register long r2 __asm__("r2") = 3; /* MADV_WILLNEED */
                    register long r7 __asm__("r7") = 220;
                    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
                    port_log_always("mm: data pack read ahead (madvise %ld)\n", r0);
                }
                return 1;
            }
        }
        close(fd);
    }
    port_log_always("FATAL: no data pack (MM_PACK, data/mm_pack.bin or build/hh-pack.bin)\n");
    return 0;
}

extern char __gamedata_start[], __gamedata_end[], __gamebss_start[], __gamebss_end[];
/* the game's memory (its .data and .bss: every heap, buffer and save buffer is in there) */
static int HhMm_InGameMem(const void* p, size_t size) {
    const uintptr_t a = (uintptr_t)p;
    return (a >= (uintptr_t)__gamebss_start && a + size >= a && a + size <= (uintptr_t)__gamebss_end) ||
           (a >= (uintptr_t)__gamedata_start && a + size >= a && a + size <= (uintptr_t)__gamedata_end);
}
static void HhPack_Read(void* ram, uintptr_t vrom, size_t size) {
    {   /* every DMA lands in game memory: anything else is a bad pointer (logged, the first few, and dropped) */
        static int sBad;
        const uintptr_t a = (uintptr_t)ram;
        if (!((a >= (uintptr_t)__gamebss_start && a + size <= (uintptr_t)__gamebss_end) ||
              (a >= (uintptr_t)__gamedata_start && a + size <= (uintptr_t)__gamedata_end))) {
            if (sBad++ < 20) {
                port_log_always("mm: DMA to %p (vrom %08x size %x) outside game memory, from %p; dropped\n", ram,
                                (unsigned)vrom, (unsigned)size, __builtin_return_address(0));
            }
            return;
        }
    }
    if (vrom + size > sPackSize || vrom + size < vrom) {
        port_log_always("mm: DMA past the pack: vrom %08x size %x\n", (unsigned)vrom, (unsigned)size);
        memset(ram, 0, size);
        return;
    }
    memcpy(ram, sPack + vrom, size);
}

const void* HhPack_Ptr(uintptr_t vrom) {
    return sPack + vrom;
}

/* ---- DMA (boot/z_std_dma.c) ---------------------------------------------------------------------- */
void DmaMgr_Init(void) {
}

void DmaMgr_Stop(void) {
}

s32 DmaMgr_RequestSync(void* ram, uintptr_t vrom, size_t size) {
    HhPack_Read(ram, vrom, size);
    return 0;
}

s32 DmaMgr_RequestAsync(DmaRequest* req, void* ram, uintptr_t vrom, size_t size, UNK_TYPE unused, OSMesgQueue* queue,
                        void* msg) {
    HhPack_Read(ram, vrom, size);
    if (req != NULL) {
        req->vromAddr = vrom;
        req->dramAddr = ram;
        req->size = size;
        req->notifyQueue = queue;
        req->notifyMsg = msg;
    }
    if (queue != NULL) {
        osSendMesg(queue, msg, OS_MESG_NOBLOCK);
    }
    return 0;
}

s32 DmaMgr_DmaRomToRam(uintptr_t rom, void* ram, size_t size) {
    HhPack_Read(ram, rom, size);
    return 0;
}

s32 DmaMgr_TranslateVromToRom(uintptr_t vrom) {
    return (s32)vrom;
}

const char* func_800809F4(uintptr_t vrom) {
    return "";
}

s32 DmaMgr_AudioDmaHandler(OSPiHandle* pihandle, OSIoMesg* mb, s32 direction) {
    if (mb == NULL) {
        return -1;
    }
    HhPack_Read(mb->dramAddr, mb->devAddr, mb->size);
    if (mb->hdr.retQueue != NULL) {
        osSendMesg(mb->hdr.retQueue, (OSMesg)mb, OS_MESG_NOBLOCK);
    }
    return 0;
}

void Audio_InvalDCache(void* buf, size_t size) {
}

void Audio_WritebackDCache(void* buf, size_t size) {
}

/* ---- the scheduler's notify (code/sched.c): the frame is drawn right here -------------------------- */
void Sched_Notify(Scheduler* sc);

void Sched_SendNotifyMsg(Scheduler* sched) {
    Sched_Notify(sched);
}

void Sched_SendAudioCancelMsg(Scheduler* sched) {
}

void Sched_SendGfxCancelMsg(Scheduler* sched) {
}

/* ---- IRQ manager, VI config, CIC ----------------------------------------------------------------- */
void IrqMgr_AddClient(IrqMgr* irqMgr, IrqMgrClient* client, OSMesgQueue* msgQueue) {
}

void IrqMgr_RemoveClient(IrqMgr* irqMgr, IrqMgrClient* client) {
}

void ViConfig_UpdateVi(u32 black) {
}

void CIC6105_Noop2(void) {
}

u32 __osSpGetStatus(void) {
    return 0;
}

void __osSpSetStatus(u32 data) {
}

/* ---- pad manager (code/padmgr.c) ----------------------------------------------------------------- */
static void HhPad_Read(Input* inputs) {
    OSContPad pads[MAXCONTROLLERS];
    Input* input = &inputs[0];

    memset(pads, 0, sizeof(pads));
    osContStartReadData(NULL);
    osContGetReadData(pads);

    input->prev = gPadMgr.inputs[0].cur;
    input->cur = pads[0];
    input->press.button = (input->cur.button ^ input->prev.button) & input->cur.button;
    input->press.stick_x = input->cur.stick_x - input->prev.stick_x;
    input->press.stick_y = input->cur.stick_y - input->prev.stick_y;
    input->rel.button = (input->cur.button ^ input->prev.button) & input->prev.button;
    input->rel.stick_x = input->cur.stick_x;
    input->rel.stick_y = input->cur.stick_y;
    memset(&inputs[1], 0, sizeof(Input) * (MAXCONTROLLERS - 1));
    gPadMgr.inputs[0] = *input;
    gPadMgr.validCtrlrsMask = 1;
    gPadMgr.nControllers = 1;
}

void PadMgr_GetInput(Input* inputs, s32 gameRequest) {
    HhPad_Read(inputs);
}

void PadMgr_GetInput2(Input* inputs, s32 gameRequest) {
    HhPad_Read(inputs);
}

void PadMgr_GetInputNoLock(Input* inputs, s32 gameRequest) {
    HhPad_Read(inputs);
}

u8 PadMgr_GetValidControllersMask(void) {
    return 1;
}

s32 PadMgr_ControllerHasRumblePak(s32 port) {
    return false;
}

void PadMgr_RumblePause(void) {
}

void PadMgr_RumbleSet(u8 enable[MAXCONTROLLERS]) {
}

void PadMgr_RumbleSetSingle(s32 port, s32 enable) {
}

void PadMgr_SetInputRetraceCallback(void (*callback)(void*), void* arg) {
    gPadMgr.inputRetraceCallback = callback;
    gPadMgr.inputRetraceArg = arg;
}

void PadMgr_UnsetInputRetraceCallback(void (*callback)(void*), void* arg) {
    gPadMgr.inputRetraceCallback = NULL;
    gPadMgr.inputRetraceArg = NULL;
}

void PadMgr_SetRumbleRetraceCallback(void (*callback)(void*), void* arg) {
}

void PadMgr_UnsetRumbleRetraceCallback(void (*callback)(void*), void* arg) {
}

static OSMesgQueue sHhSerialQueue;
OSMesgQueue* PadMgr_VoiceAcquireSerialEventQueue(void) {
    return &sHhSerialQueue;
}

void PadMgr_VoiceReleaseSerialEventQueue(OSMesgQueue* serialEventQueue) {
}

/* the Voice Recognition Unit (Japanese only): never present */
s32 osVoiceCheckWord(u8* word) {
    return 0;
}
s32 osVoiceClearDictionary(OSVoiceHandle* hd, u8 numWords) {
    return CONT_ERR_NO_CONTROLLER;
}
s32 osVoiceControlGain(OSVoiceHandle* hd, s32 analog, s32 digital) {
    return CONT_ERR_NO_CONTROLLER;
}
s32 osVoiceGetReadData(OSVoiceHandle* hd, OSVoiceData* result) {
    return CONT_ERR_NO_CONTROLLER;
}
s32 osVoiceMaskDictionary(OSVoiceHandle* hd, u8* maskPattern, int size) {
    return CONT_ERR_NO_CONTROLLER;
}
s32 osVoiceSetWord(OSVoiceHandle* hd, u8* word) {
    return CONT_ERR_NO_CONTROLLER;
}
s32 osVoiceStartReadData(OSVoiceHandle* hd) {
    return CONT_ERR_NO_CONTROLLER;
}
s32 osVoiceStopReadData(OSVoiceHandle* hd) {
    return CONT_ERR_NO_CONTROLLER;
}

/* ---- the fault screen (boot/fault.c): a log line and an exit ------------------------------------- */
void Fault_Init(void) {
}

void Fault_AddClient(FaultClient* client, FaultClientCallback callback, void* arg0, void* arg1) {
    if (client != NULL) {
        client->callback = callback;
        client->arg0 = arg0;
        client->arg1 = arg1;
    }
}

void Fault_RemoveClient(FaultClient* client) {
}

void Fault_AddAddrConvClient(FaultAddrConvClient* client, FaultAddrConvClientCallback callback, void* arg) {
}

void Fault_RemoveAddrConvClient(FaultAddrConvClient* client) {
}

void Fault_SetFrameBuffer(void* fb, u16 w, u16 h) {
}

void Fault_WaitForInput(void) {
}

void FaultDrawer_SetCharPad(s8 padW, s8 padH) {
}

s32 FaultDrawer_Printf(const char* fmt, ...) {
    char buf[512];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    port_log_always("fault: %s", buf);
    return 0;
}

void FaultDrawer_DrawText(s32 x, s32 y, const char* fmt, ...) {
    char buf[512];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    port_log_always("fault: %s\n", buf);
}

NORETURN void Fault_AddHungupAndCrashImpl(const char* exp1, const char* exp2) {
    port_log_always("FATAL: game fault: %s %s\n", exp1 != NULL ? exp1 : "", exp2 != NULL ? exp2 : "");
    fflush(NULL);
    _exit(2);
}

NORETURN void Fault_AddHungupAndCrash(const char* file, s32 line) {
    char msg[64];

    snprintf(msg, sizeof(msg), "line %d", (int)line);
    Fault_AddHungupAndCrashImpl(file, msg);
}

/* ---- the flash chip (code/osFlash.c): a 128 KB file in the data folder --------------------------- */
#define HH_FLASH_SIZE 0x20000
#define HH_FLASH_PAGE 128
static u8 sFlash[HH_FLASH_SIZE];
static int sFlashLoaded;
static u8 sFlashWriteBuf[HH_FLASH_PAGE];
static volatile uint64_t sFlashDirtyUs;
uint64_t port_time_us64(void);

static int HhFlash_Write(const u8* data);
void HhMm_LogAlignment(const char* when);

/* The save's own sanity check at load (the audit): every file copy that says it is a save ("ZELDA3") must carry
 * the game's checksum (Sram_CalcChecksum: the byte sum with the checksum field zero). A copy that fails is a broken
 * file (a crash mid-write, a bad card): the whole file is kept as mm-flash.bin.bad-<time>, that copy is erased (the
 * game then takes the other copy, or an empty file) and the log says which. */
static void HhFlash_Check(void) {
    static const struct {
        u32 page;
        u32 size;
        const char* what;
    } kCopies[] = {
        { 0x000, sizeof(Save), "file 1" },
        { 0x040, sizeof(Save), "file 1 backup" },
        { 0x080, sizeof(Save), "file 2" },
        { 0x0C0, sizeof(Save), "file 2 backup" },
        { 0x100, offsetof(SaveContext, fileNum), "file 1 owl save" },
        { 0x180, offsetof(SaveContext, fileNum), "file 1 owl save backup" },
        { 0x200, offsetof(SaveContext, fileNum), "file 2 owl save" },
        { 0x280, offsetof(SaveContext, fileNum), "file 2 owl save backup" },
    };
    const size_t ckOff = offsetof(SaveContext, save.saveInfo.checksum);
    const size_t newfOff = offsetof(SaveContext, save.saveInfo.playerData.newf);
    int bad = 0;
    size_t i;
    char backup[512];

    for (i = 0; i < ARRAY_COUNT(kCopies); i++) {
        const u8* c = sFlash + kCopies[i].page * HH_FLASH_PAGE;
        u16 stored, sum = 0;
        size_t j;
        if (memcmp(c + newfOff, "ZELDA3", 6) != 0) {
            continue; /* empty */
        }
        memcpy(&stored, c + ckOff, sizeof(stored));
        for (j = 0; j < kCopies[i].size; j++) {
            sum += (j == ckOff || j == ckOff + 1) ? 0 : c[j];
        }
        if (sum == stored) {
            continue;
        }
        if (bad++ == 0) {
            const time_t now = time(NULL);
            struct tm tmv;
            FILE* f;
            localtime_r(&now, &tmv);
            snprintf(backup, sizeof(backup), "%s.bad-%04d%02d%02d-%02d%02d%02d", port_save_path("mm-flash.bin"),
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            f = fopen(backup, "wb");
            if (f != NULL) {
                fwrite(sFlash, 1, HH_FLASH_SIZE, f);
                fclose(f);
            }
        }
        port_log_always("mm: SAVE CHECK: %s fails its checksum (stored %04x, computed %04x): erased; the whole file "
                        "is kept as %s\n", kCopies[i].what, stored, sum, backup);
        memset(sFlash + kCopies[i].page * HH_FLASH_PAGE, 0xFF, kCopies[i].size);
    }
    if (bad) {
        HhFlash_Write(sFlash);
    } else {
        port_log_always("mm: save check: every file copy's checksum is right\n");
    }
}

static void HhFlash_Load(void) {
    FILE* f;

    if (sFlashLoaded) {
        return;
    }
    sFlashLoaded = 1;
    memset(sFlash, 0xFF, sizeof(sFlash));
    f = fopen(port_save_path("mm-flash.bin"), "rb");
    if (f != NULL) {
        size_t n = fread(sFlash, 1, sizeof(sFlash), f);

        fclose(f);
        port_log_always("mm: flash save loaded (%u bytes)\n", (unsigned)n);
        HhFlash_Check();
    }
}

static int HhFlash_Write(const u8* data) {
    char tmp[512];
    const char* path = port_save_path("mm-flash.bin");
    FILE* f;
    int ok;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (f == NULL) {
        port_log_always("mm: flash save failed: %s\n", strerror(errno));
        return 0;
    }
    ok = fwrite(data, 1, HH_FLASH_SIZE, f) == HH_FLASH_SIZE;
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        port_log_always("mm: flash save failed writing %s: %s (the old file is kept)\n", tmp, strerror(errno));
        remove(tmp);
        return 0;
    }
    rename(tmp, path);
    return 1;
}

/* The card write (128 KB + fsync: tens to hundreds of ms on the SD card) runs on its own thread, from a snapshot
 * taken at the frame boundary, so a save never stalls a frame (the audit's I/O check). At exit the caller waits. */
static u8 sFlashSnap[HH_FLASH_SIZE];
static pthread_mutex_t sFlashMx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sFlashCv = PTHREAD_COND_INITIALIZER;
static int sFlashPending, sFlashBusy, sFlashThreadOn;

static void* HhFlash_Thread(void* arg) {
    (void)arg;
    pthread_mutex_lock(&sFlashMx);
    for (;;) {
        while (!sFlashPending) {
            pthread_cond_wait(&sFlashCv, &sFlashMx);
        }
        sFlashPending = 0;
        sFlashBusy = 1;
        pthread_mutex_unlock(&sFlashMx);
        {
            const uint64_t t0 = port_time_us64();
            const int ok = HhFlash_Write(sFlashSnap);
            if (ok) {
                port_log_always("mm: flash save written (%.0f ms, writer thread)\n", (port_time_us64() - t0) / 1000.0);
            }
        }
        pthread_mutex_lock(&sFlashMx);
        sFlashBusy = 0;
        pthread_cond_broadcast(&sFlashCv);
    }
    return NULL;
}

/* At the frame boundary (now = 0): if pages changed and none for 200 ms, a snapshot goes to the writer thread.
 * At exit (now = 1): the same at once, and the caller waits (up to 3 s) until it is on the card. */
void HhFlash_Flush(int now) {
    const uint64_t d = sFlashDirtyUs;
    if (d != 0 && (now || port_time_us64() - d > 200000)) {
        sFlashDirtyUs = 0;
        pthread_mutex_lock(&sFlashMx);
        if (!sFlashThreadOn) {
            pthread_t t;
            sFlashThreadOn = pthread_create(&t, NULL, HhFlash_Thread, NULL) == 0;
            if (sFlashThreadOn) {
                pthread_detach(t);
            }
        }
        if (sFlashThreadOn) {
            /* (a write in progress reads sFlashSnap: wait for it first, rare and short) */
            while (sFlashBusy) {
                pthread_cond_wait(&sFlashCv, &sFlashMx);
            }
            memcpy(sFlashSnap, sFlash, HH_FLASH_SIZE);
            sFlashPending = 1;
            pthread_cond_broadcast(&sFlashCv);
        }
        pthread_mutex_unlock(&sFlashMx);
        if (!sFlashThreadOn) {
            HhFlash_Write(sFlash);
        }
    }
    if (now && sFlashThreadOn) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 3;
        pthread_mutex_lock(&sFlashMx);
        while ((sFlashPending || sFlashBusy) && pthread_cond_timedwait(&sFlashCv, &sFlashMx, &ts) == 0) {
        }
        pthread_mutex_unlock(&sFlashMx);
    }
}

OSPiHandle* osFlashInit(void) {
    static OSPiHandle sHandle;

    HhFlash_Load();
    return &sHandle;
}

void osFlashReadId(u32* flashType, u32* flashVendor) {
    *flashType = 0x11118001;
    *flashVendor = 0x00C2001E; /* MX29L1100 */
}

s32 osFlashSectorErase(u32 pageNum) {
    const uint64_t off = ((uint64_t)pageNum * HH_FLASH_PAGE) & ~(uint64_t)0x3FFF; /* 16 KB sectors */

    HhFlash_Load();
    if (off < HH_FLASH_SIZE) {
        memset(sFlash + off, 0xFF, 0x4000);
        sFlashDirtyUs = port_time_us64();
    }
    return 0;
}

s32 osFlashWriteBuffer(OSIoMesg* mb, s32 priority, void* dramAddr, OSMesgQueue* mq) {
    if (HhMm_InGameMem(dramAddr, HH_FLASH_PAGE)) {
        memcpy(sFlashWriteBuf, dramAddr, HH_FLASH_PAGE);
    } else { /* a bad save request (e.g. a file number out of range): logged and dropped, never a crash */
        static int sBad;
        if (sBad++ < 10) {
            port_log_always("mm: flash write from %p outside game memory; dropped\n", dramAddr);
        }
        memset(sFlashWriteBuf, 0xFF, HH_FLASH_PAGE);
    }
    if (mq != NULL) {
        osSendMesg(mq, (OSMesg)mb, OS_MESG_NOBLOCK);
    }
    return 0;
}

s32 osFlashWriteArray(u32 pageNum) {
    const uint64_t off = (uint64_t)pageNum * HH_FLASH_PAGE;

    HhFlash_Load();
    if (off + HH_FLASH_PAGE <= HH_FLASH_SIZE) {
        memcpy(sFlash + off, sFlashWriteBuf, HH_FLASH_PAGE);
        /* the file is written once the burst of page writes is over (HhFlash_Flush at the frame boundary): a new
         * file's save is ~200 pages, and a 128 KB write + fsync for each took minutes on the card (the name entry
         * "hang") */
        sFlashDirtyUs = port_time_us64();
    }
    return 0;
}

s32 osFlashReadArray(OSIoMesg* mb, s32 priority, u32 pageNum, void* dramAddr, u32 pageCount, OSMesgQueue* mq) {
    const uint64_t off = (uint64_t)pageNum * HH_FLASH_PAGE;
    const uint64_t n = (uint64_t)pageCount * HH_FLASH_PAGE;

    HhFlash_Load();
    if (off + n <= HH_FLASH_SIZE && HhMm_InGameMem(dramAddr, (size_t)n)) {
        memcpy(dramAddr, sFlash + off, (size_t)n);
    } else {
        port_log_always("mm: flash read of pages %u+%u to %p out of range; dropped\n", (unsigned)pageNum,
                        (unsigned)pageCount, dramAddr);
    }
    if (mq != NULL) {
        osSendMesg(mq, (OSMesg)mb, OS_MESG_NOBLOCK);
    }
    return 0;
}

/* ---- the audio interface (the mixer replaces it; silent until hh_audio is ported) ---------------- */
s32 OotPspAudioBackend_SetFrequency(u32 frequency) __attribute__((weak));
s32 OotPspAudioBackend_SetFrequency(u32 frequency) {
    return frequency;
}
u32 OotPspAudioBackend_GetLength(void) __attribute__((weak));
u32 OotPspAudioBackend_GetLength(void) {
    return 0;
}
s32 OotPspAudioBackend_Queue(const void* buf, u32 size);
s32 osAiSetNextBuffer(void* buf, u32 size) {
    return OotPspAudioBackend_Queue(buf, size);
}

/* ---- segments: a real pointer stays a pointer ---------------------------------------------------- */
void* HhSegmentedToVirtual(uintptr_t addr) {
    u32 seg;

    if (addr >= PORT_REAL_PTR_MIN || addr == 0) {
        return (void*)addr;
    }
    seg = SEGMENT_NUMBER(addr);
    return (void*)(gSegments[seg] + K0BASE + SEGMENT_OFFSET(addr));
}

/* ---- the system heap (main.c: the RAM between the code and the framebuffers) --------------------- */
#define HH_SYSTEM_HEAP_SIZE (12 * 1024 * 1024)
static u8 sSystemHeap[HH_SYSTEM_HEAP_SIZE] __attribute__((aligned(64))) HH_GS_BSS;

/* ---- the entry point ------------------------------------------------------------------------------ */
static void HhMm_Crash(int sig) {
    extern u32 port_frame_count(void);
    port_log_always("FATAL: signal %d\n", sig);
    fflush(NULL);
    _exit(3);
}

static long sFrames;
u32 port_frame_count(void) {
    return (u32)sFrames;
}
#include "functions.h"
#include "audio/load.h"
void HhMm_AudioFrame(void) __attribute__((weak));
/* the audio engine's updates without the mixer: one per VI of the frame (hh_mm_audio.c replaces this) */
void HhMm_AudioFrame(void) {
    extern void HhAudio_SyncFrame(void);
    HhAudio_SyncFrame(); /* OOT_AUDIO_SYNC=1 only (deterministic runs); otherwise the audio thread (hh_audio.c) */
}
void HhMm_AudioInit(void) __attribute__((weak));
void HhMm_AudioInit(void) {
    extern void OotPspAudio_Init(void);
    OotPspAudio_Init(); /* hh_audio.c: the engine's init, the SDL output, the 60 Hz audio thread */
}

void HhMm_Beat(void);
void HhMm_FrameDone(void) {
    sFrames++;
    HhMm_Beat();
    {   /* MM_TRACE_AT=<frame>: the renderer's per-command trace for the next frame (console.log) */
        static long sTraceAt = -2;
        extern int gfx_trace_frames;
        if (sTraceAt == -2) {
            const char* e = getenv("MM_TRACE_AT");
            sTraceAt = e != NULL ? atol(e) : -1;
        }
        if (sTraceAt >= 0 && sFrames == sTraceAt) {
            gfx_trace_frames = 1;
        }
    }
    HhMm_AudioFrame();
    {
        static long sExitAt = -2;
        if (sExitAt == -2) {
            const char* e = getenv("MM_EXIT_AT");
            sExitAt = e != NULL ? atol(e) : -1;
        }
        if (sExitAt >= 0 && sFrames >= sExitAt) {
            port_log_always("mm: exit at frame %ld\n", sFrames);
            HhMm_LogAlignment("exit");
            HhFlash_Flush(1);
            fflush(NULL);
            _exit(0);
        }
    }
}

#include "hh_mm_watch.inc"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
static void HhMm_HighMem(void) {
    const char* e = getenv("MM_HIGHMEM");
    size_t sz;
    unsigned n = 0, mb = 0;

    if (e == NULL || e[0] != '1') {
        return;
    }
    { /* every free 64 KB below 0x80000000 */
        uintptr_t a;
        for (a = 0x00100000u; a < 0x80000000u; a += 0x10000) {
            void* p = mmap((void*)a, 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                           -1, 0);
            if (p == MAP_FAILED) {
                continue;
            }
            if (p != (void*)a) { /* a kernel without MAP_FIXED_NOREPLACE: a hint only */
                munmap(p, 0x10000);
                continue;
            }
            n++;
        }
        mb = n / 16;
        (void)sz;
    }
    {
        void* t = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        port_log_always("mm: MM_HIGHMEM: %u maps (%u MB) reserved below 0x80000000; next map at %p, brk %p\n", n, mb,
                        t, sbrk(0));
        munmap(t, 4096);
    }
}

/* The kernel's count of unaligned accesses it fixed up for user programs (/proc/cpu/alignment, "User:"): each
 * costs a trap, so a rise across a session means unaligned loads in the hot path (the audit's alignment check:
 * qemu does not show them). -1 where the file is absent (qemu, desktop). */
static long HhMm_AlignFixups(void) {
    FILE* f = fopen("/proc/cpu/alignment", "r");
    char line[128];
    long n = -1;
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "User:", 5) == 0) {
            n = atol(line + 5);
            break;
        }
    }
    fclose(f);
    return n;
}
static long sAlignAtStart = -2;
void HhMm_LogAlignment(const char* when) {
    const long n = HhMm_AlignFixups();
    if (sAlignAtStart == -2) {
        sAlignAtStart = n;
    }
    if (n >= 0) {
        port_log_always("align: %s: %ld user alignment fixups by the kernel (%ld this session)\n", when, n,
                        n - sAlignAtStart);
    }
}

char** gHhMmArgv; /* hh_mm_convert.c: the first start starts the game again in a fresh process */
int main(int argc, char** argv) {
    gHhMmArgv = argv;
    {   /* kit k1.6: nice -10 before any thread exists (all inherit it): the OS's helpers fit in the frame gaps */
        extern void hh_game_priority(void);
        hh_game_priority();
    }
    HhMm_HighMem();
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGSEGV, HhMm_Crash);
    signal(SIGBUS, HhMm_Crash);
    signal(SIGILL, HhMm_Crash);
    signal(SIGFPE, HhMm_Crash);
    port_log_always("mm: Majora's Mask (Miyoo Mini port) %s\n", HH_RELEASE);
    HhMm_LogAlignment("start");
    {
        extern void HhOvl_LogInit(void);
        HhOvl_LogInit();
    }
    {
        extern void HhMm_TimeStart(void);
        HhMm_TimeStart();
    }
    HhMm_WatchStart();

    {   /* the Mini's CPU clock (kit miyoo_clock), as the OoT port: the model's default boost, put back at exit */
        extern int miyoo_clock_boost_to(int mhz);
        extern void miyoo_clock_restore(void);
        const int mhz = miyoo_clock_boost_to(-1);
        if (mhz > 0) {
            port_log_always("clock: %d MHz\n", mhz);
            atexit(miyoo_clock_restore);
        } else {
            port_log_always("clock: left as it was\n");
        }
    }
    osInitialize();
    {   /* hh_mm_convert.c: first start builds the data pack from the player's ROM (mm_recipes.bin next to mm) */
        extern void HhMm_ConvertEnsure(void);
        HhMm_ConvertEnsure();
    }
    if (!HhPack_Open()) {
        return 1;
    }
    {   /* the arrays of ROM data the shipped executable leaves out (hh_mm_convert.c, hh/tools/mm_embed.py) */
        extern int HhEmbed_Fill(const unsigned char* pack, size_t packSize);
        extern void HhOvl_Refresh(void);
        if (!HhEmbed_Fill(sPack, sPackSize)) {
            return 1;
        }
        HhOvl_Refresh(); /* hh_ovl.c's overlay .data snapshots: taken before the copy */
    }
    OotPspRenderer_Init();
    savestate_parse_args(argc, argv); /* --loadStateFile <path> (tests) */
    savestate_boot_prompt();          /* an auto save on the card: RESUME GAME / NEW GAME */

    /* code/main.c Main(), without the threads the port does not need */
    gScreenWidth = SCREEN_WIDTH;
    gScreenHeight = SCREEN_HEIGHT;
    gSystemHeapSize = sizeof(sSystemHeap);
    SystemHeap_Init(sSystemHeap, gSystemHeapSize);
    Regs_Init();
    Sched_Init(&gScheduler, NULL, Z_PRIORITY_SCHED, gViConfigModeType, 1, &gIrqMgr);
    gPadMgr.validCtrlrsMask = 1;
    gPadMgr.nControllers = 1;
    HhMm_AudioInit();
    port_log_always("mm: audio initialised\n");

    Graph_ThreadEntry(NULL);
    port_log_always("mm: game loop ended after %ld frames\n", sFrames);
    HhMm_LogAlignment("exit");
    HhFlash_Flush(1);
    {   /* SDL down on every exit (the next SDL program, the launcher, can hang at its first flip otherwise) */
        extern void HhAudio_Shutdown(void);
        extern int gfx_wm_shutdown(void);
        HhAudio_Shutdown();
        port_log_always(gfx_wm_shutdown() ? "exit: SDL shut down\n" : "exit: SDL left up (a flip still running)\n");
    }
    return 0;
}
