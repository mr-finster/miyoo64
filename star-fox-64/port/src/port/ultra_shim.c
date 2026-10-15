/**
 * libultra replacement (from the MK64 handheld kit; DMAs read the SF64 port ROM image).
 *
 * The game runs single-threaded: the port's main loop calls the game loop
 * directly, so message queues never block (osRecvMesg returns -1 when empty),
 * DMAs read the port ROM image (port_rom.c), the VI/AI/SP
 * calls are no-ops or hooks into the port backends, and EEPROM is a file.
 */
#include <stdint.h>
#include <ultra64.h>
#include <PR/os.h>
#include <macros.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include "port.h"
#include "sdl/savestate.h"
#include "sf64save.h" /* statefix A: Save_PortReapply */

#if defined(TARGET_PSP)
#include <pspkernel.h>
#include <psprtc.h>
static inline u64 port_micros_u64(void) { return port_micros_u64(); }
#else
#include <time.h>
extern u64 port_game_time_us64(void); /* sdl/main.c: one syscall per read on the device; virtual under SF64_VCLOCK */
static inline u64 port_micros_u64(void) { return port_game_time_us64(); }
#endif

/* ------------------------------------------------------------------------- */
/* Globals libultra normally provides                                          */
/* ------------------------------------------------------------------------- */

u64 osClockRate = 62500000;
s32 osTvType = 1; // OS_TV_TYPE_NTSC
s32 osResetType = 0; // cold boot
s32 osAppNmiBuffer[16];
OSViMode osViModeTable[32];

/* Referenced by src/os/math (sinf/cosf). */
typedef union {
    int i;
    float f;
} port_fu;
#undef NAN
const port_fu NAN = { 0x7f810000 };

/* ------------------------------------------------------------------------- */
/* Init / threads / timers                                                      */
/* ------------------------------------------------------------------------- */

void osInitialize(void) {
}

void osCreateThread(UNUSED OSThread* thread, UNUSED OSId id, UNUSED void (*entry)(void*), UNUSED void* arg,
                    UNUSED void* sp, UNUSED OSPri pri) {
}
void osStartThread(UNUSED OSThread* thread) {
}
void osDestroyThread(UNUSED OSThread* thread) {
}
void osYieldThread(void) {
}
void osSetThreadPri(UNUSED OSThread* thread, UNUSED OSPri pri) {
}
OSPri osGetThreadPri(UNUSED OSThread* thread) {
    return 0;
}
OSThread* __osGetCurrFaultedThread(void) {
    return NULL;
}

/* osGetTime counts at osClockRate (62.5 MHz); derive it from the system
 * microsecond clock so time deltas stay meaningful. */
static u64 sTimeBase;
/* The game clock goes on from a save state's own time after a load. A state holds the pending
 * timers' deadlines (sTimers, below) on the clock of the boot that saved it, and that clock is
 * the device's uptime: a resume after a power cycle reads a smaller one, so a timer
 * saved pending (the Nintendo logo's 1 s wait, Timer_CreateTask at GSTATE_CHECK_SAVE) only came
 * due once the new uptime caught up with the old -- minutes or hours on the logo. sClockLast is
 * in the state, sClockLastLive never restored: they differ only after a load, and the first
 * reading then moves the base so the clock continues from the state's last reading. */
static OSTime sClockLast;
static OSTime sClockLastLive SAVESTATE_EXCLUDE;

OSTime osGetTime(void) {
    u64 now = port_micros_u64(); // microseconds
    OSTime t;
    if (sClockLast != sClockLastLive) {
        sTimeBase = now - (u64) (sClockLast / 62.5); /* a state was loaded: continue its clock */
    }
    t = (OSTime) ((now - sTimeBase) * 62.5);
    sClockLast = sClockLastLive = t;
    return t;
}

void osSetTime(OSTime time) {
    sTimeBase = port_micros_u64() - (u64) (time / 62.5);
}

u32 osGetCount(void) {
    // 46.875 MHz on the N64; system clock is 1 MHz, scale to keep ratios sane.
    return (u32) (port_micros_u64() * 46);
}

/* Timers (SF64: Timer_CreateTask / Timer_Wait). The N64's timer interrupt
 * sends the message when the countdown expires; here pending timers are
 * checked by port_timers_poll() (once per frame, and from any blocking
 * receive that would otherwise find its queue empty). */
#define PORT_MAX_TIMERS 32
static struct {
    OSTimer* timer;
    OSTime deadline, interval;
    OSMesgQueue* mq;
    OSMesg msg;
} sTimers[PORT_MAX_TIMERS];

int osSetTimer(OSTimer* timer, OSTime countdown, OSTime interval, OSMesgQueue* mq, OSMesg msg) {
    int i;
    for (i = 0; i < PORT_MAX_TIMERS; i++) {
        if (sTimers[i].timer == NULL || sTimers[i].timer == timer) {
            sTimers[i].timer = timer;
            sTimers[i].deadline = osGetTime() + (countdown ? countdown : interval);
            sTimers[i].interval = interval;
            sTimers[i].mq = mq;
            sTimers[i].msg = msg;
            return 0;
        }
    }
    port_log_always("osSetTimer: no free timer slot\n");
    return -1;
}

int osStopTimer(OSTimer* timer) {
    int i;
    for (i = 0; i < PORT_MAX_TIMERS; i++) {
        if (sTimers[i].timer == timer) {
            sTimers[i].timer = NULL;
            return 0;
        }
    }
    return -1;
}

/* Fire every due timer; returns how many fired. */
int port_timers_poll(void) {
    const OSTime now = osGetTime();
    int i, fired = 0;
    for (i = 0; i < PORT_MAX_TIMERS; i++) {
        if (sTimers[i].timer != NULL && now >= sTimers[i].deadline) {
            OSMesgQueue* mq = sTimers[i].mq;
            OSMesg msg = sTimers[i].msg;
            if (sTimers[i].interval != 0) {
                sTimers[i].deadline += sTimers[i].interval;
            } else {
                sTimers[i].timer = NULL;
            }
            if (mq != NULL) osSendMesg(mq, msg, OS_MESG_NOBLOCK);
            fired++;
        }
    }
    return fired;
}

/* A pacing wait. Any sleep on the Miyoo Mini costs a whole kernel tick, 9.2-9.6 ms however short the request
 * (measured on the Mini Plus), so a frame's remainder under ~10 ms slept was a frame late. Only the part of a wait
 * above 12 ms is slept; the rest is a sched_yield loop on the monotonic clock, to the same deadline. On a kernel whose
 * sleeps are exact the timing is the same. SF64_SPINWAIT=0: one sleep, as before. Returns the microseconds waited. */
extern int sched_yield(void); /* not <sched.h>: the game has a sched.h of its own on the include path */
static unsigned int spin_wait_us(unsigned int us) {
    static int spin = -1;
    struct timespec ts;
    unsigned long long t0, now, until;
    if (spin < 0) { const char *e = getenv("SF64_SPINWAIT"); spin = !(e && e[0] == '0'); }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t0 = (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
    until = t0 + (unsigned long long)us * 1000ull;
    for (now = t0; now < until;) {
        const unsigned long long left = until - now;
        if (!spin || left > 12000000ull) {
            const unsigned long long s = spin ? left - 12000000ull : left;
            struct timespec d = { (time_t)(s / 1000000000ull), (long)(s % 1000000000ull) };
            nanosleep(&d, NULL); /* a signal cutting it short: the loop goes on to the deadline */
        } else {
            sched_yield();
        }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
    }
    return (unsigned int)((now - t0) / 1000u);
}

/* A blocking receive on a queue a pending timer feeds: sleep until it is due. */
static int port_timer_wait_for(OSMesgQueue* mq) {
    int i, best = -1;
    for (i = 0; i < PORT_MAX_TIMERS; i++) {
        if (sTimers[i].timer != NULL && sTimers[i].mq == mq &&
            (best < 0 || sTimers[i].deadline < sTimers[best].deadline)) {
            best = i;
        }
    }
    if (best < 0) return 0;
    {
        const OSTime now = osGetTime();
        if (sTimers[best].deadline > now) {
            const u64 us = (u64) ((sTimers[best].deadline - now) / 62.5);
            (void) spin_wait_us((unsigned int) us);
        }
    }
    return port_timers_poll();
}

void osSetEventMesg(UNUSED OSEvent e, UNUSED OSMesgQueue* mq, UNUSED OSMesg msg) {
}

/* ------------------------------------------------------------------------- */
/* Message queues (never block)                                               */
/* ------------------------------------------------------------------------- */

void osCreateMesgQueue(OSMesgQueue* mq, OSMesg* msgBuf, s32 count) {
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msgBuf;
}

s32 osSendMesg(OSMesgQueue* mq, OSMesg msg, UNUSED s32 flag) {
    s32 index;
    if (mq->validCount >= mq->msgCount) {
        return -1;
    }
    index = (mq->first + mq->validCount) % mq->msgCount;
    mq->msg[index] = msg;
    mq->validCount++;
    return 0;
}

s32 osJamMesg(OSMesgQueue* mq, OSMesg msg, UNUSED s32 flag) {
    if (mq->validCount >= mq->msgCount) {
        return -1;
    }
    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    mq->msg[mq->first] = msg;
    mq->validCount++;
    return 0;
}

s32 osRecvMesg(OSMesgQueue* mq, OSMesg* msg, s32 flag) {
    if (mq->validCount == 0) {
        port_timers_poll();
        if (mq->validCount == 0 && flag == OS_MESG_BLOCK) {
            port_timer_wait_for(mq);
        }
        if (mq->validCount == 0) {
            return -1;
        }
    }
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Cache / memory                                                             */
/* ------------------------------------------------------------------------- */

void osInvalDCache(UNUSED void* a, UNUSED s32 b) {
}
void osInvalICache(UNUSED void* a, UNUSED s32 b) {
}
void osWritebackDCache(UNUSED void* a, UNUSED s32 b) {
}
void osWritebackDCacheAll(void) {
}
u32 osVirtualToPhysical(void* addr) {
    return (uintptr_t) addr;
}

/* ------------------------------------------------------------------------- */
/* "PI" DMA: the cartridge is in memory                                       */
/* ------------------------------------------------------------------------- */

void osCreatePiManager(UNUSED OSPri pri, UNUSED OSMesgQueue* cmdQ, UNUSED OSMesg* cmdBuf, UNUSED s32 cmdMsgCnt) {
}

s32 osPiStartDma(UNUSED OSIoMesg* mb, UNUSED s32 priority, UNUSED s32 direction, uintptr_t devAddr, void* vAddr,
                 size_t nbytes, OSMesgQueue* mq) {
    if (nbytes != 0) {
        /* SF64: devAddr is an offset into the port ROM image (port_rom.c). */
        extern void port_rom_read(uintptr_t romAddr, void* dst, size_t size);
        port_rom_read(devAddr, vAddr, nbytes);
    }
    if (mq != NULL) {
        osSendMesg(mq, NULL, OS_MESG_NOBLOCK); // completion message
    }
    return 0;
}

s32 osEPiStartDma(UNUSED OSPiHandle* pihandle, OSIoMesg* mb, s32 direction) {
    return osPiStartDma(mb, 0, direction, mb->devAddr, mb->dramAddr, mb->size, mb->hdr.retQueue);
}

OSPiHandle* osCartRomInit(void) {
    static OSPiHandle handle;
    return &handle;
}

/* ------------------------------------------------------------------------- */
/* VI                                                                         */
/* ------------------------------------------------------------------------- */

void osCreateViManager(UNUSED OSPri pri) {
}
void osViSetMode(UNUSED OSViMode* mode) {
}
void osViSetEvent(UNUSED OSMesgQueue* mq, UNUSED OSMesg msg, UNUSED u32 retraceCount) {
}
void osViBlack(UNUSED u8 active) {
}
int gPortViGamma; /* game state: the game sets it at boot (and its debug combo toggles it) */
void osViSetSpecialFeatures(u32 func) {
    if (func & OS_VI_GAMMA_ON) gPortViGamma = 1;
    if (func & OS_VI_GAMMA_OFF) gPortViGamma = 0;
}
/* The VI gamma curve, out = sqrt(in) on 0..1, as tables (built once). */
static unsigned char sGamma8[256] SAVESTATE_EXCLUDE;
static unsigned short* sGamma565 SAVESTATE_EXCLUDE;
static int sGammaBuilt SAVESTATE_EXCLUDE;
/* SF64_GAMMADITHER: the game turns the VI's gamma dither on with the gamma (sys_main.c): the VI puts
 * 6 random bits under each 8-bit colour before the square root ((c << 6) | rand, 0..16383), so black
 * comes out as noise of 0..16 (about 10 on average) and the darkest tones a little lighter -- every
 * black of the game (space, the map, the briefings, fades) is a dark grey on the N64 (angrylion: a
 * median of 10-11 where the port showed 0). The table takes the dither's average (no grain); from
 * c = 16 up it changes nothing by more than 1. SF64_GAMMADITHER=0: the plain curve, as before. */
static void gamma_build(void) {
    int i;
    const char* e = getenv("SF64_GAMMADITHER");
    const int dither = !(e && e[0] == '0');
    for (i = 0; i < 256; i++) {
        int v;
        if (dither) {
            float sum = 0.0f;
            int d;
            for (d = 0; d < 64; d++) sum += sqrtf((float) ((i << 6) | d) / 16383.0f) * 255.0f;
            v = (int) (sum / 64.0f + 0.5f);
        } else {
            v = (int) (sqrtf((float) i / 255.0f) * 255.0f + 0.5f);
        }
        sGamma8[i] = (unsigned char) (v > 255 ? 255 : v);
    }
    sGammaBuilt = 1;
}
const unsigned char* port_vi_gamma8(void) {
    if (!gPortViGamma) return NULL;
    if (!sGammaBuilt) gamma_build();
    return sGamma8;
}
/* RGB565 -> RGB565 through the curve (128 KB, for the handheld's 565 present). */
const unsigned short* port_vi_gamma565(void) {
    if (!gPortViGamma) return NULL;
    if (!sGammaBuilt) gamma_build();
    if (sGamma565 == NULL) {
        unsigned int p;
        sGamma565 = malloc(65536 * sizeof(*sGamma565));
        if (sGamma565 == NULL) return NULL;
        for (p = 0; p < 65536; p++) {
            const int r = sGamma8[((p >> 11) & 31) * 255 / 31], g = sGamma8[((p >> 5) & 63) * 255 / 63],
                      b = sGamma8[(p & 31) * 255 / 31];
            sGamma565[p] = (unsigned short) (((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 |
                                             ((b * 31 + 127) / 255));
        }
    }
    return sGamma565;
}
void osViSwapBuffer(UNUSED void* vaddr) {
}

/* ------------------------------------------------------------------------- */
/* SP tasks: graphics go through port_gfx_run(), audio through the mixer      */
/* ------------------------------------------------------------------------- */

void osSpTaskLoad(UNUSED OSTask* task) {
}
void osSpTaskStartGo(UNUSED OSTask* task) {
}
void osSpTaskYield(void) {
}
OSYieldResult osSpTaskYielded(UNUSED OSTask* task) {
    return 0;
}

/* ------------------------------------------------------------------------- */
/* AI                                                                         */
/* ------------------------------------------------------------------------- */

extern void port_audio_out_push(const s16* samples, u32 bytes);
extern u32 port_audio_out_queued_bytes(void);

s32 osAiSetFrequency(u32 freq) {
    extern void port_audio_out_set_rate(u32 freq);
    port_audio_out_set_rate(freq); // the output stage resamples to the PSP rate
    return (s32) freq;
}
s32 osAiSetNextBuffer(void* buf, u32 size) {
    port_audio_out_push((const s16*) buf, size);
    return 0;
}
u32 osAiGetLength(void) {
    extern u32 port_audio_out_ai_length(void);
    return port_audio_out_ai_length();
}

/* ------------------------------------------------------------------------- */
/* Controllers                                                                */
/* ------------------------------------------------------------------------- */

extern void controller_psp_init(void);
extern void controller_psp_read(OSContPad* pad);

s32 osContInit(UNUSED OSMesgQueue* mq, u8* bitpattern, OSContStatus* status) {
    controller_psp_init();
    *bitpattern = 1; // one controller plugged in
    status[0].type = CONT_TYPE_NORMAL;
    status[0].status = 0;
    status[0].errno = 0;
    status[1].errno = status[2].errno = status[3].errno = CONT_NO_RESPONSE_ERROR;
    return 0;
}

s32 osContStartReadData(OSMesgQueue* mq) {
    osSendMesg(mq, NULL, OS_MESG_NOBLOCK);
    return 0;
}

void osContGetReadData(OSContPad* pad) {
    controller_psp_read(&pad[0]);
#ifdef PORT_INPUT_SCRIPT
    {
        extern void port_input_script(OSContPad* pad);
        port_input_script(&pad[0]);
    }
#endif
    pad[1].button = pad[2].button = pad[3].button = 0;
    pad[1].stick_x = pad[2].stick_x = pad[3].stick_x = 0;
    pad[1].stick_y = pad[2].stick_y = pad[3].stick_y = 0;
    pad[1].errno = pad[2].errno = pad[3].errno = CONT_NO_RESPONSE_ERROR;
}

/* ------------------------------------------------------------------------- */
/* EEPROM: a 512 byte file next to the EBOOT                                  */
/* ------------------------------------------------------------------------- */

#define EEPROM_FILE port_save_path("eeprom.bin")
#define EEPROM_SIZE 512

/* The cartridge EEPROM in memory. A block write (osEepromWrite, 8 bytes) changes only this image;
 * port_eeprom_flush writes the whole image to eeprom.bin once, at the end of a save (sys_save.c
 * Save_WriteEeprom). Star Fox writes a save block by block, up to 64 of them, and one file write
 * + fsync per block would cost seconds on the SD card after a press in the menus.
 * These stay IN the save states, beside the game's own copies (gSaveFile, sPrevSaveData):
 * Save_WriteEeprom writes only the blocks that differ from sPrevSaveData, so after a load the
 * image must be the one sPrevSaveData describes, or the next save would mix two saves' blocks
 * and break both copies' checksums (the next boot would then start from the default save). After
 * a load, hh_game_save_reapply (below, statefix A) puts eeprom.bin back over the image and the
 * game's copies together when the card's is newer, so a load never rolls the save back. The flush
 * runs inside the game's frame, so a state holds a dirty image only after a failed write (the next
 * flush retries it). */
static u8 sEeprom[EEPROM_SIZE];
static u8 sEepromLoaded;
static u8 sEepromDirty;
/* The image was built without eeprom.bin: the file was there but could not be read (a card read
 * error, EIO). The game then took its default save in memory, and the first save (the boot's own,
 * at GSTATE_CHECK_SAVE) would have replaced the player's progress with it. No write replaces the
 * file while this is set; each refused save says GAME SAVE FAILED. In the save states like the image
 * it describes: a state from such a session, resumed later, still holds an image built on the
 * default save (the next cold start without a resume reads eeprom.bin again). */
static u8 sEepromKeep;

#if defined(TARGET_PSP)
static void eeprom_load(void) {
    FILE* fp;
    if (sEepromLoaded) {
        return;
    }
    sEepromLoaded = 1;
    memset(sEeprom, 0, sizeof(sEeprom));
    fp = fopen(EEPROM_FILE, "rb");
    if (fp != NULL) {
        fread(sEeprom, 1, sizeof(sEeprom), fp);
        fclose(fp);
    }
}

static s32 eeprom_save(void) {
    FILE* fp = fopen(EEPROM_FILE, "wb");
    if (fp == NULL) {
        return -1;
    }
    fwrite(sEeprom, 1, sizeof(sEeprom), fp);
    fclose(fp);
    return 0;
}
#else
/* eeprom.bin is all of the player's progress (cups, records, options), so a
 * write never truncates the only copy: the new image goes to eeprom.bin.tmp,
 * fsync, rename over eeprom.bin, fsync the renamed file, fsync the dir. A power
 * cut (flat battery) mid-write leaves the old file whole. vfat renames over
 * an existing file in two steps (drop the old entry, rename the new one), so a
 * cut between them leaves only the .tmp -- fsynced whole, so load takes it and
 * finishes the rename. */
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>           /* here, after the controller code: libultra's OSContPad has a field named errno */
#include "sdl/handheld_menu.h" /* GAME SAVE FAILED in the OS bar (a no-op off the handheld) */
#define EEPROM_TMP port_save_path("eeprom.bin.tmp")

/* After a rename onto eeprom.bin: the renamed file first, then the folder. On the card's vfat (Linux
 * 4.14 vfat_rename) a rename over an existing file moves the new file into the OLD file's directory
 * slot (to a new name: a slot made with cluster 0) and only marks its inode dirty, so the slot's start
 * cluster and size reach the card at that inode's writeback (~30 s later) or its own fsync; the
 * folder's fsync writes the removed .tmp entry and the FAT with the old file's clusters freed. A hard
 * power cut in that window left eeprom.bin pointing at the freed clusters: the progress lost, the FAT
 * inconsistent (super-smash-bros cd5128c, diddy-kong-racing 9c91d40). fp: the renamed file, still open (closed
 * here), or NULL to open it again. The data is already in place under its name: a failure here only
 * leaves the slot to the kernel's writeback, as before. */
static void eeprom_sync_rename(FILE* fp) {
    const int fd = fp != NULL ? fileno(fp) : open(EEPROM_FILE, O_RDONLY);
    int dir;
    if (fd >= 0) {
        fsync(fd);
    }
    if (fp != NULL) {
        fclose(fp);
    } else if (fd >= 0) {
        close(fd);
    }
    dir = open(port_save_dir(), O_RDONLY);
    if (dir >= 0) {
        fsync(dir);
        close(dir);
    }
}

/* The bytes path held (at most a whole image); img (EEPROM_SIZE) gets them, zero-padded. *err: 0, or
 * why a file that is there could not be opened or read (anything but "no such file"). */
static size_t eeprom_read_into(u8* img, const char* path, int* err) {
    FILE* fp;
    size_t n = 0;
    memset(img, 0, EEPROM_SIZE);
    *err = 0;
    errno = 0;
    fp = fopen(path, "rb");
    if (fp == NULL) {
        if (errno != ENOENT) {
            *err = errno ? errno : EIO;
        }
        return 0;
    }
    n = fread(img, 1, EEPROM_SIZE, fp);
    if (ferror(fp)) {
        *err = errno ? errno : EIO;
    }
    fclose(fp);
    return n;
}
static size_t eeprom_read(const char* path, int* err) {
    return eeprom_read_into(sEeprom, path, err);
}

static void eeprom_load(void) {
    size_t n;
    int err;
    if (sEepromLoaded) {
        return;
    }
    sEepromLoaded = 1;
    if (eeprom_read(EEPROM_FILE, &err) == sizeof(sEeprom)) {
        port_log_always("eeprom: eeprom.bin read\n");
        return;
    }
    if (eeprom_read(EEPROM_TMP, &err) == sizeof(sEeprom)) {
        port_log_always("eeprom: eeprom.bin missing or short, using eeprom.bin.tmp\n");
        if (rename(EEPROM_TMP, EEPROM_FILE) == 0) {
            eeprom_sync_rename(NULL);
        }
        return;
    }
    /* no save yet (zeros: the game writes its default save), or a short file: keep what's
     * there, zero-padded; the game checks both copies' checksums and falls back on its own */
    n = eeprom_read(EEPROM_FILE, &err);
    if (err != 0) {
        /* There, but unreadable: never formatted over (paper-mario 742bdb4). The game sees a blank
         * EEPROM and plays from its default save; no save this session replaces the file. */
        memset(sEeprom, 0, sizeof(sEeprom));
        sEepromKeep = 1;
        port_log_always("eeprom: eeprom.bin cannot be read (%s): kept as it is, no save replaces it this session\n",
                        strerror(err));
    } else if (n == sizeof(sEeprom)) {
        port_log_always("eeprom: eeprom.bin read (second try)\n");
    } else if (n == 0) {
        port_log_always("eeprom: no eeprom.bin yet (or an empty one): the game writes a new save\n");
    } else {
        port_log_always("eeprom: eeprom.bin short (%u bytes), the rest read as zeros\n", (unsigned) n);
    }
}

/* A failed write: the game ignores Save_Write's result, so a full or read-only card lost a medal, a
 * record or a setting with only "write failed" (no reason) in the log. The stage and the reason go
 * to the log and GAME SAVE FAILED to the OS bar (mario-kart-64 37c208f, goldeneye-007 3b09a41,
 * diddy-kong-racing 8938223); eeprom.bin keeps the previous image, and the image stays dirty, so the next
 * save writes it whole. */
static s32 eeprom_failed(const char* stage, int err) {
    port_log_always("eeprom: %s failed (%s), eeprom.bin kept\n", stage, strerror(err));
    handheld_menu_notify("GAME SAVE FAILED");
    return -1;
}

extern u64 port_time_us64(void); /* sdl/main.c: the real clock (the timing in the log line) */
static s32 eeprom_save(void) {
    const u64 t0 = port_time_us64();
    const char* stage = NULL;
    int err = 0;
    FILE* fp;
    if (sEepromKeep) {
        port_log_always("eeprom: not written: eeprom.bin could not be read at the start (kept as it is)\n");
        handheld_menu_notify("GAME SAVE FAILED");
        return -1;
    }
    errno = 0;
    fp = fopen(EEPROM_TMP, "wb");
    if (fp == NULL) {
        return eeprom_failed("open", errno ? errno : EIO);
    }
    errno = 0;
    if (fwrite(sEeprom, 1, sizeof(sEeprom), fp) != sizeof(sEeprom) || fflush(fp) != 0) {
        stage = "write";
        err = errno ? errno : ENOSPC; /* a short write may set none */
    } else if (fsync(fileno(fp)) != 0) {
        stage = "fsync";
        err = errno;
    } else if (rename(EEPROM_TMP, EEPROM_FILE) != 0) {
        stage = "rename";
        err = errno;
    }
    if (stage != NULL) {
        fclose(fp);
        remove(EEPROM_TMP);
        return eeprom_failed(stage, err);
    }
    /* still open across the rename: fsynced again (the vfat slot), closed, then the folder. The close
     * moved after the rename: its result never mattered after a successful fflush and fsync. */
    eeprom_sync_rename(fp);
    port_log_always("eeprom: eeprom.bin written (%u ms)\n", (unsigned) ((port_time_us64() - t0) / 1000));
    return 0;
}

/* statefix A (handheld-kit k1.8; the kit's savestate.c calls it after a load that happened, mixer parked, mix
 * lock held, before the texture fixup). sEeprom and the game's copies of the save (gSaveFile, sPrevSaveData,
 * gSaveIOBuffer: fox_save.c, sys_save.c) are in the state, so a LOAD gave back the save as it was when the
 * state was made: a medal, a planet cleared, a ranking or a sound setting saved since then was gone in the
 * game, and the game's next save (Save_WriteEeprom: the blocks that differ from sPrevSaveData, into this
 * image, then the whole image to the card) wrote the old image back over eeprom.bin. Now eeprom.bin is put
 * back over all of them at once:
 *  - The card's image is the state's sEeprom (nothing written since the state): nothing changes, the load
 *    is exact.
 *  - It differs: sEeprom = the card's image (clean), and fox_save.c Save_PortReapply makes gSaveFile,
 *    sPrevSaveData and gSaveIOBuffer the card's, with the sound settings the game took from the save at
 *    boot (it says what it keeps of the state: the sound screen's unwritten pick). A level or a menu in the
 *    state goes on as it was; its next save writes into the newer save, as a save does on the console.
 *  - Not changed (one log line): the state is from before the game's first EEPROM read (the boot: the game
 *    reads the card itself at GSTATE_CHECK_SAVE, sEepromLoaded being in the state); eeprom.bin could not be
 *    read when the state's session started (sEepromKeep); it is missing or short now; neither of its copies
 *    passes the game's checksum (the game would start from its default save). A read error now also sets
 *    sEepromKeep, as eeprom_load does: a file that is there but unreadable is never written over.
 * SF64_SAVEREAPPLY=0: as before (the state's copies stand; tests). */
_Static_assert(sizeof(SaveFile) == EEPROM_SIZE, "a SaveFile is the whole EEPROM image");
void hh_game_save_reapply(void) {
    static SaveFile card; /* a SaveFile's bytes: the whole EEPROM image */
    u8* const img = (u8*) &card;
    size_t n;
    int err = 0, diff = 0, i, r;
    {
        const char* e = getenv("SF64_SAVEREAPPLY");
        if (e && e[0] == '0') {
            return;
        }
    }
    if (!sEepromLoaded) {
        port_log_always("eeprom: the state is from before the game's first read; the game reads eeprom.bin itself\n");
        return;
    }
    if (sEepromKeep) {
        port_log_always("eeprom: eeprom.bin could not be read when the state's session started; the state's copy stands, no write replaces the file\n");
        return;
    }
    n = eeprom_read_into(img, EEPROM_FILE, &err);
    if (n != EEPROM_SIZE && !err) {
        n = eeprom_read_into(img, EEPROM_TMP, &err); /* a rename cut short: the whole .tmp, as eeprom_load */
    }
    if (n != EEPROM_SIZE) {
        if (err) {
            sEepromKeep = 1;
        }
        port_log_always("eeprom: eeprom.bin %s (%u bytes); the state's copy stands%s\n", err ? strerror(err) : "missing or short",
                        (unsigned) n, err ? ", no write replaces the file" : "");
        return;
    }
    for (i = 0; i < EEPROM_SIZE; i++) {
        diff += img[i] != sEeprom[i];
    }
    if (diff == 0) {
        port_log_always("eeprom: the state's save is the card's\n");
        return;
    }
    r = Save_PortReapply(&card);
    if (r < 0) {
        port_log_always("eeprom: eeprom.bin differs from the state's (%d bytes) but neither copy passes the game's check; the state's copy stands\n",
                        diff);
        return;
    }
    memcpy(sEeprom, img, EEPROM_SIZE);
    sEepromDirty = 0;
    port_log_always("eeprom: the card's save is newer than the state's (%d bytes): put back%s%s\n", diff,
                    (r & 1) ? "; sound settings" : "", (r & 2) ? "; the sound screen's unsaved pick kept" : "");
}
#endif

/* The image to eeprom.bin, if a block changed since the last flush: one atomic file write per
 * game save. 0 = on the card (or nothing to write), -1 = the write failed (eeprom.bin as it was;
 * the image stays dirty, so the next save writes it again). */
s32 port_eeprom_flush(void) {
    if (!sEepromDirty) {
        return 0;
    }
    if (eeprom_save() != 0) {
        return -1;
    }
    sEepromDirty = 0;
    return 0;
}

s32 osEepromProbe(UNUSED OSMesgQueue* mq) {
    return EEPROM_TYPE_4K;
}

s32 osEepromLongRead(UNUSED OSMesgQueue* mq, u8 address, u8* buffer, int nbytes) {
    eeprom_load();
    if (address * 8 + nbytes > EEPROM_SIZE) {
        return -1;
    }
    memcpy(buffer, sEeprom + address * 8, nbytes);
    return 0;
}

/* Into the in-memory image only; port_eeprom_flush puts it on the card. */
s32 osEepromLongWrite(UNUSED OSMesgQueue* mq, u8 address, u8* buffer, int nbytes) {
    eeprom_load();
    if (nbytes < 0 || address * 8 + nbytes > EEPROM_SIZE) {
        return -1;
    }
    if (memcmp(sEeprom + address * 8, buffer, nbytes) != 0) {
        memcpy(sEeprom + address * 8, buffer, nbytes);
        sEepromDirty = 1;
    }
    return 0;
}

s32 osEepromRead(OSMesgQueue* mq, u8 address, u8* buffer) {
    return osEepromLongRead(mq, address, buffer, 8);
}

s32 osEepromWrite(OSMesgQueue* mq, u8 address, u8* buffer) {
    return osEepromLongWrite(mq, address, buffer, 8);
}

/* ------------------------------------------------------------------------- */
/* Controller Pak (ghost data): not present                                   */
/* ------------------------------------------------------------------------- */

s32 osPfsInit(UNUSED OSMesgQueue* mq, UNUSED OSPfs* pfs, UNUSED int channel) {
    return PFS_ERR_NOPACK;
}
s32 osPfsIsPlug(UNUSED OSMesgQueue* mq, u8* pattern) {
    *pattern = 0;
    return 0;
}
s32 osPfsFindFile(UNUSED OSPfs* pfs, UNUSED u16 company, UNUSED u32 game, UNUSED u8* gameName, UNUSED u8* extName,
                  UNUSED s32* fileNo) {
    return PFS_ERR_NOPACK;
}
s32 osPfsAllocateFile(UNUSED OSPfs* pfs, UNUSED u16 company, UNUSED u32 game, UNUSED u8* gameName,
                      UNUSED u8* extName, UNUSED int length, UNUSED s32* fileNo) {
    return PFS_ERR_NOPACK;
}
s32 osPfsDeleteFile(UNUSED OSPfs* pfs, UNUSED u16 company, UNUSED u32 game, UNUSED u8* gameName,
                    UNUSED u8* extName) {
    return PFS_ERR_NOPACK;
}
s32 osPfsReadWriteFile(UNUSED OSPfs* pfs, UNUSED s32 fileNo, UNUSED u8 flag, UNUSED int offset, UNUSED int nbytes,
                       UNUSED u8* data) {
    return PFS_ERR_NOPACK;
}
s32 osPfsFileState(UNUSED OSPfs* pfs, UNUSED s32 fileNo, UNUSED OSPfsState* state) {
    return PFS_ERR_NOPACK;
}
s32 osPfsFreeBlocks(UNUSED OSPfs* pfs, s32* bytes) {
    *bytes = 0;
    return PFS_ERR_NOPACK;
}
s32 osPfsNumFiles(UNUSED OSPfs* pfs, s32* max, s32* used) {
    *max = 0;
    *used = 0;
    return PFS_ERR_NOPACK;
}

/* ------------------------------------------------------------------------- */
/* Debug output                                                               */
/* ------------------------------------------------------------------------- */

void rmonPrintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void osSyncPrintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
