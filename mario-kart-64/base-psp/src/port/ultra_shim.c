/**
 * libultra replacement for the PSP port.
 *
 * The game runs single-threaded: the port's main loop calls the game loop
 * directly, so message queues never block (osRecvMesg returns -1 when empty),
 * DMAs are memcpys (the "ROM" is linked into the executable), the VI/AI/SP
 * calls are no-ops or hooks into the port backends, and EEPROM is a file.
 */
#include <ultra64.h>
#include <PR/os.h>
#include <macros.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "port.h"

#if defined(TARGET_PSP)
#include <pspkernel.h>
#include <psprtc.h>
static inline u64 port_micros_u64(void) { return port_micros_u64(); }
#else
#include <time.h>
static inline u64 port_micros_u64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000ull + (u64)ts.tv_nsec / 1000;
}
#endif

/* ------------------------------------------------------------------------- */
/* Globals libultra normally provides                                          */
/* ------------------------------------------------------------------------- */

u64 osClockRate = 62500000;
u32 osTvType = 1; // OS_TV_TYPE_NTSC
u32 osResetType = 0; // cold boot
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

OSTime osGetTime(void) {
    u64 now = port_micros_u64(); // microseconds
    return (OSTime) ((now - sTimeBase) * 62.5);
}

void osSetTime(OSTime time) {
    sTimeBase = port_micros_u64() - (u64) (time / 62.5);
}

u32 osGetCount(void) {
    // 46.875 MHz on the N64; system clock is 1 MHz, scale to keep ratios sane.
    return (u32) (port_micros_u64() * 46);
}

u32 osSetTimer(UNUSED OSTimer* timer, UNUSED OSTime countdown, UNUSED OSTime interval, UNUSED OSMesgQueue* mq,
               UNUSED OSMesg msg) {
    return 0;
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

s32 osRecvMesg(OSMesgQueue* mq, OSMesg* msg, UNUSED s32 flag) {
    if (mq->validCount == 0) {
        return -1;
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

void osInvalDCache(UNUSED void* a, UNUSED size_t b) {
}
void osInvalICache(UNUSED void* a, UNUSED size_t b) {
}
void osWritebackDCache(UNUSED void* a, UNUSED size_t b) {
}
void osWritebackDCacheAll(void) {
}
uintptr_t osVirtualToPhysical(void* addr) {
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
        memcpy(vAddr, (const void*) devAddr, nbytes);
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
void osViSetSpecialFeatures(UNUSED u32 func) {
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
    return port_audio_out_queued_bytes();
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
    status[0].errnum = 0;
    status[1].errnum = status[2].errnum = status[3].errnum = CONT_NO_RESPONSE_ERROR;
    return 0;
}

s32 osContStartReadData(OSMesgQueue* mq) {
    osSendMesg(mq, NULL, OS_MESG_NOBLOCK);
    return 0;
}

/* MK64_KEYS="<poll>:<key>+|-,...": the pad script of the shared tools/m64ref.sh (M64REF_KEYS), so
 * the port and mupen64plus get the same input at the same controller read and reach the same
 * moment. <poll> counts these reads (the game's own clock); key+ holds a key from that read on,
 * key- releases it. Keys: a b z start l r cu cd cl cr du dd dl dr, and up down left right (stick
 * at 80). The script replaces the pad (desktop tests; unset = the real pad). */
u32 gPortPolls; /* controller reads so far */
static void port_keys_script(OSContPad* pad) {
    static const char* sKeys = (const char*) -1;
    static const char* const names[18] = { "a", "b", "z", "start", "l", "r", "cu", "cd", "cl", "cr",
                                           "du", "dd", "dl", "dr", "up", "down", "left", "right" };
    static const u16 bits[14] = { A_BUTTON, B_BUTTON, Z_TRIG, START_BUTTON, L_TRIG, R_TRIG, U_CBUTTONS,
                                  D_CBUTTONS, L_CBUTTONS, R_CBUTTONS, U_JPAD, D_JPAD, L_JPAD, R_JPAD };
    static int held[18];
    if (sKeys == (const char*) -1) sKeys = getenv("MK64_KEYS");
    if (!sKeys) return;
    for (const char* p = sKeys; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
        char* e;
        const unsigned long at = strtoul(p, &e, 10);
        if (at != gPortPolls || *e != ':') continue;
        for (int k = 0; k < 18; k++) {
            const size_t n = strlen(names[k]);
            if (!strncmp(e + 1, names[k], n) && (e[1 + n] == '+' || e[1 + n] == '-')) held[k] = e[1 + n] == '+';
        }
    }
    u16 b = 0;
    for (int k = 0; k < 14; k++) if (held[k]) b |= bits[k];
    pad->button = b;
    pad->stick_y = held[14] ? 80 : held[15] ? -80 : 0;
    pad->stick_x = held[16] ? -80 : held[17] ? 80 : 0;
}

void osContGetReadData(OSContPad* pad) {
    controller_psp_read(&pad[0]);
    gPortPolls++;
    port_keys_script(&pad[0]);
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

static u8 sEeprom[EEPROM_SIZE];
static u8 sEepromLoaded;

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
 * fsync, rename over eeprom.bin, fsync eeprom.bin, fsync the dir. A power
 * cut (flat battery) mid-write leaves the old file whole. vfat renames over an existing
 * file in two steps (drop the old entry, rename the new one), so a cut
 * between them leaves only the .tmp -- fsynced whole, so load takes it and
 * finishes the rename. */
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "handheld_menu.h" /* the OS caption on a failed write (a no-op off the handheld) */
#define EEPROM_TMP port_save_path("eeprom.bin.tmp")

/* After a rename onto eeprom.bin: the rename itself reaches the card. The file first, which writes its
 * directory slot (on the card's vfat a rename only marks the moved inode dirty: until its writeback,
 * ~30 s, the slot keeps the old file's clusters -- or, under a new name, cluster 0 and size 0 -- while
 * the folder's fsync already frees the old clusters in the FAT; the SSB64 port cd5128c, the DKR port
 * 9c91d40), then the folder. A power cut in that window lost the save that had just been written. */
static void eeprom_sync_renamed(void) {
    int fd = open(EEPROM_FILE, O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    fd = open(port_save_dir(), O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

#ifdef TARGET_HANDHELD
#include "savestate.h" /* SAVESTATE_EXCLUDE */
#else
#define SAVESTATE_EXCLUDE
#endif
/* eeprom.bin is there but could not be opened or read (a card read error: EIO): the game then saw a
 * blank EEPROM and formatted it at boot (load_save_data rewrites every record whose checksum fails),
 * and the first of those writes renamed the blank image over the player's file. Such a file is kept
 * as it is for the session: every write is refused, logged and shown (the Paper Mario port 742bdb4, the DKR port
 * 4bbc6fe; the SSB64 port S3 is the same shape). A missing file is a new save, as before. Out of save
 * states: a LOAD of a state from a session that could read the file must not clear it. */
static int sEepromKeep SAVESTATE_EXCLUDE;

/* GAME SAVE FAILED in the OS bar, at most once a second: the game writes record by record (a blank or
 * unread image is formatted at boot in ~40 writes within one frame), and each caption is a thread and
 * a shell. Every failure is still logged. */
static void eeprom_failed_notify(void) {
    extern u32 port_time_us(void);
    static u32 sShownUs SAVESTATE_EXCLUDE;
    static int sShown SAVESTATE_EXCLUDE;
    const u32 now = port_time_us();
    if (sShown && now - sShownUs < 1000000u) {
        return;
    }
    sShown = 1;
    sShownUs = now;
    handheld_menu_notify("GAME SAVE FAILED");
}

/* The bytes read into img (EEPROM_SIZE, zero-padded): EEPROM_SIZE = a whole image. *err: 0, or why a
 * file that is there could not be opened or read whole (a missing file is 0: no save yet). */
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
    if (n < EEPROM_SIZE && ferror(fp)) {
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
    int err, err2;
    if (sEepromLoaded) {
        return;
    }
    sEepromLoaded = 1;
    if (eeprom_read(EEPROM_FILE, &err) == EEPROM_SIZE) {
        return;
    }
    if (eeprom_read(EEPROM_TMP, &err2) == EEPROM_SIZE) {
        port_log_always("eeprom: eeprom.bin missing or short, using eeprom.bin.tmp\n");
        if (rename(EEPROM_TMP, EEPROM_FILE) == 0) {
            eeprom_sync_renamed();
        }
        return;
    }
    n = eeprom_read(EEPROM_FILE, &err2);
    if (n == EEPROM_SIZE) {
        return; /* the first read's error was passing */
    }
    if (err || err2) {
        sEepromKeep = 1;
        port_log_always("eeprom: eeprom.bin cannot be read (%s, %u bytes): kept as it is, no write replaces it this session\n",
                        strerror(err2 ? err2 : err), (unsigned) n);
        return;
    }
    /* no save yet (zeros: the game formats it), or a short file from an old
     * build's in-place write: keep what's there, the game checks each record */
}

static s32 eeprom_save(void) {
    FILE* fp;
    int ok;
    if (sEepromKeep) {
        port_log_always("eeprom: not written: eeprom.bin could not be read at the start\n");
        eeprom_failed_notify();
        return -1;
    }
    fp = fopen(EEPROM_TMP, "wb");
    ok = fp != NULL && fwrite(sEeprom, 1, sizeof(sEeprom), fp) == sizeof(sEeprom);
    if (fp != NULL) {
        ok = fflush(fp) == 0 && ok;
        ok = ok && fsync(fileno(fp)) == 0;
        ok = fclose(fp) == 0 && ok;
    }
    if (!ok || rename(EEPROM_TMP, EEPROM_FILE) != 0) {
        /* The game ignores the write's result, so a full or read-only card lost a record or a cup
         * without a word (a failed open not even logged). Now logged with the reason and shown, as
         * GoldenEye's sweep does (hh_hw.c, the kit's SAVE FAILED for a state); a later write that
         * works makes the file whole again (it holds the whole image). */
        port_log_always("eeprom: write failed (%s), eeprom.bin kept\n", strerror(errno));
        eeprom_failed_notify();
        remove(EEPROM_TMP);
        return -1;
    }
    eeprom_sync_renamed();
    return 0;
}

/* statefix A (the port kit k1.8; the kit's savestate.c calls it after a load that happened, mixer parked,
 * mix lock held). sEeprom and the game's RAM copy of it, gSaveData (save.c: the whole 512-byte image,
 * read at the first menu, every record and cup written from it at once), are in the state, so a LOAD
 * gave back the save as it was when the state was made: a record or a cup won since then was gone in
 * the game, and the next race's write put the old image back over eeprom.bin. Now eeprom.bin is put
 * back over both: the save on the card is always the newest, in the game too.
 *  - The card's image is the state's sEeprom (nothing written since): nothing changes, the load is
 *    exact (gSaveData too: the game writes each change to its copy at once).
 *  - It differs: sEeprom = gSaveData = the card's image, and the sound mode it holds is the game's
 *    again (set_sound_mode only queues the audio command, as the options menu does). A race or a menu in
 *    the state goes on as it was; its next write merges into the newer records, as a write does on the
 *    console.
 *  - The state is from before the game first read the EEPROM (the boot logos): the game reads the card
 *    itself later. The card can't be read (sEepromKeep, or an error now): the state's copy stands, and
 *    no write replaces the file this session (sEepromKeep) or the next write is what the game knows.
 * MK64_SAVEREAPPLY=0: as before (the state's copy stands; tests). */
#include <defines.h>   /* NUM_SOUND_MODES, SOUND_MONO */
#include <save_data.h> /* SaveData gSaveData */
extern u8 gSoundMode;
extern void set_sound_mode(void);
_Static_assert(sizeof(SaveData) == EEPROM_SIZE, "gSaveData is the whole EEPROM image");
void hh_game_save_reapply(void) {
    static u8 card[EEPROM_SIZE];
    size_t n;
    int err = 0, diff = 0, i;
    {
        const char* e = getenv("MK64_SAVEREAPPLY");
        if (e && e[0] == '0') {
            return;
        }
    }
    if (!sEepromLoaded) {
        port_log_always("eeprom: the state is from before the first read; the game reads eeprom.bin itself\n");
        return;
    }
    if (sEepromKeep) {
        port_log_always("eeprom: eeprom.bin could not be read at the start; the state's copy stands, no write replaces the file\n");
        return;
    }
    n = eeprom_read_into(card, EEPROM_FILE, &err);
    if (n != EEPROM_SIZE && !err) {
        n = eeprom_read_into(card, EEPROM_TMP, &err); /* a rename cut short: the whole .tmp, as eeprom_load */
    }
    if (n != EEPROM_SIZE) {
        port_log_always("eeprom: eeprom.bin %s (%u bytes); the state's copy stands\n", err ? strerror(err) : "missing or short",
                        (unsigned) n);
        return;
    }
    for (i = 0; i < EEPROM_SIZE; i++) {
        diff += card[i] != sEeprom[i];
    }
    if (diff == 0) {
        port_log_always("eeprom: the state's save is the card's\n");
        return;
    }
    memcpy(sEeprom, card, EEPROM_SIZE);
    memcpy(&gSaveData, card, EEPROM_SIZE);
    {
        u8 mode = gSaveData.main.saveInfo.soundMode;
        if (mode >= NUM_SOUND_MODES) {
            mode = SOUND_MONO; /* load_save_data's rule */
        }
        if (mode != gSoundMode) {
            gSoundMode = mode;
            set_sound_mode();
        }
    }
    port_log_always("eeprom: the card's save is newer than the state's (%d bytes): put back\n", diff);
}
#endif

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

s32 osEepromLongWrite(UNUSED OSMesgQueue* mq, u8 address, u8* buffer, int nbytes) {
    eeprom_load();
    if (address * 8 + nbytes > EEPROM_SIZE) {
        return -1;
    }
    memcpy(sEeprom + address * 8, buffer, nbytes);
    return eeprom_save();
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
