/**
 * libultra replacement for Diddy Kong Racing on the handheld / desktop proxy.
 *
 * The game runs single-threaded. On the N64, thread 3 (the game) runs
 * main_game_loop() once per frame while the scheduler, the audio manager and
 * the background loader run as their own threads. Here port main calls
 * main_game_loop() directly, and the few places that hand work to another
 * thread are patched (#ifdef PORT) to do it inline:
 *   - gfxtask_run_xbus (rcp_dkr.c): runs the display list through gfx_pc now;
 *   - fb_update (video.c): counts elapsed 60 Hz fields off the wall clock
 *     (port_vi_fields) instead of VI retrace messages;
 *   - the audio manager's frame handler is called from the main loop;
 *   - bgload_tick (thread30_bgload.c) loads the menu level inline.
 * So message queues never block: osRecvMesg on an empty queue returns -1.
 *
 * PI DMA reads the asset region of the player's ROM (port/sdl/assets_dkr.c),
 * EEPROM is a file written atomically, and the Controller Pak reports "no
 * pak" until M4.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#include <ultra64.h>
#include <PR/os.h>
#include <PR/sched.h>
#include "port.h"
#include "handheld_menu.h" /* the OS caption on a failed EEPROM write (a no-op off the handheld) */

extern int port_rom_read(u32 rom, void *dst, u32 len);
extern u32 port_time_us(void);

/* ------------------------------------------------------------------ globals */
s32 osTvType = 1; /* OS_TV_TYPE_NTSC */
s32 osResetType = 0;
u64 osClockRate = 62500000;
OSViMode osViModeTable[56];
OSViMode osViModeNtscLpn1, osViModePalLpn1, osViModeMpalLpn1, osViModePalLan1, osViModeNtscLan1, osViModeMpalLan1;
/* RSP ucode symbols: only their addresses are taken. */
long long int rspbootTextStart[1], rspbootTextEnd[1];
u64 rspF3DDKRXbusStart[1], rspF3DDKRDataXbusStart[1], rspF3DDKRFifoStart[1], rspF3DDKRDataFifoStart[1];
long long int aspMainTextStart[1], aspMainDataStart[1];
u8 gGfxTaskYieldData[OS_YIELD_DATA_SIZE];
/* camera.c's anti-piracy probe reads the CIC's boot word at 0xB0000578. */
s32 D_B0000578 = 0x8965;
/* menu.c prints the ROM end as the free-space figure on a debug screen. */
u8 __ROM_END[1];

/* ------------------------------------------------------------------ time */
extern u64 port_time_us64(void); /* port/sdl/main.c: one syscall, not two */
static u64 now_us(void) { return port_time_us64(); }
/* The game's own clock reads go through port_clock_us(): wall time, except
 * under DKR_FAST, where it is the virtual clock -- the Player Select dancers
 * follow music_animation_fraction(), an osGetCount() stopwatch, and differed
 * from run to run on the wall clock. */
u64 port_clock_us(void);
static u64 sTimeBase;
OSTime osGetTime(void) { return (OSTime) ((port_clock_us() - sTimeBase) * 46875ull / 1000ull); } /* 46.875 MHz count */
void osSetTime(OSTime t) { sTimeBase = port_clock_us() - (u64) t * 1000ull / 46875ull; }
u32 osGetCount(void) { return (u32) ((port_clock_us() * 46875ull) / 1000ull); }

/* ------------------------------------------------------------------ video fields
 * The game paces itself by counting 60 Hz VI fields: fb_update() returns how
 * many passed since the last frame (2 = 30 fps, 3 = 20 fps ...) and the whole
 * simulation scales by that. port_vi_fields(min) sleeps until at least `min`
 * fields have passed since the previous call, then returns how many did. */
#define FIELD_US 16683u /* 59.94 Hz */
static u64 sFieldClock;
/* DKR_FAST=1 (headless tests): no sleeping; exactly `minFields` fields pass
 * per call on a virtual clock, so a run is fast and frame-for-frame
 * deterministic (the game always sees the same updateRate sequence). */
static int sFast = -1;
static u64 sVirtualUs;
static int fast_mode(void) {
    if (sFast < 0) { const char *e = getenv("DKR_FAST"); sFast = e && e[0] == '1'; }
    return sFast;
}
u64 port_clock_us(void) { return fast_mode() ? sVirtualUs : now_us(); }
s32 port_vi_fields(s32 minFields) {
    u64 t = now_us();
    s32 n;
    if (fast_mode()) {
        sVirtualUs += (u64) minFields * FIELD_US;
        return minFields;
    }
    if (sFieldClock == 0) sFieldClock = t;
    if (t < sFieldClock + (u64) minFields * FIELD_US) {
        u64 wake = sFieldClock + (u64) minFields * FIELD_US;
        u32 t0 = port_time_us();
        while ((t = now_us()) < wake) {
            u64 left = wake - t;
            if (left > 1500) usleep((useconds_t) (left - 1000));
        }
        PORT_PROF_ADD(PROF_SLEEP, port_time_us() - t0);
    }
    n = (s32) ((t - sFieldClock) / FIELD_US);
    if (n < 1) n = 1;
    if (n > 12) {
        sFieldClock = t; /* a long stall (load, menu): don't try to catch up */
    } else {
        sFieldClock += (u64) n * FIELD_US;
    }
    return n;
}

/* ------------------------------------------------------------------ threads (inline) */
void osCreateThread(OSThread *t, OSId id, void (*entry)(void *), void *arg, void *sp, OSPri pri) { }
void osStartThread(OSThread *t) { }
void osStopThread(OSThread *t) { }
void osSetEventMesg(OSEvent e, OSMesgQueue *mq, OSMesg m) { }
OSIntMask osSetIntMask(OSIntMask m) { return 0; }
u32 __osDisableInt(void) { return 0; }
void __osRestoreInt(u32 m) { }

/* ------------------------------------------------------------------ message queues */
void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *buf, s32 count) {
    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = buf;
}
s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flag) {
    if (mq->validCount >= mq->msgCount) return -1;
    mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    mq->validCount++;
    return 0;
}
s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag) {
    if (mq->validCount == 0) return -1;
    if (msg) *msg = mq->msg[mq->first];
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    return 0;
}

/* ------------------------------------------------------------------ scheduler (none) */
static OSMesgQueue sSchedQ;
static OSMesg sSchedBuf[8];
void osCreateScheduler(OSSched *s, void *stack, OSPri pri, u8 mode, u8 numFields) {
    osCreateMesgQueue(&sSchedQ, sSchedBuf, 8);
}
void osScAddClient(OSSched *s, OSScClient *c, OSMesgQueue *msgQ, u8 id) { }
OSMesgQueue *osScGetCmdQ(OSSched *s) { return &sSchedQ; }
OSMesgQueue *osScGetInterruptQ(OSSched *s) { return &sSchedQ; }

/* ------------------------------------------------------------------ caches, RCP */
void osInvalDCache(void *a, s32 n) { }
void osWritebackDCacheAll(void) { }
u32 osVirtualToPhysical(void *a) { return (u32) (uintptr_t) a; }
void osDpSetStatus(u32 s) { }
void __osSpSetStatus(u32 s) { }
u32 osDpGetStatus(void) { return 0; }
s32 osDpSetNextBuffer(void *b, u64 n) { return 0; }

/* ------------------------------------------------------------------ VI */
void osViSetMode(OSViMode *m) { }
void osViSetSpecialFeatures(u32 f) { }
void osViBlack(u8 on) { }
void osViSwapBuffer(void *fb) { }

/* ------------------------------------------------------------------ PI */
void osCreatePiManager(OSPri pri, OSMesgQueue *q, OSMesg *buf, s32 n) { }
s32 osPiStartDma(OSIoMesg *mb, s32 pri, s32 dir, u32 devAddr, void *vAddr, u32 nbytes, OSMesgQueue *mq) {
    if (dir == OS_READ) {
        port_rom_read(devAddr, vAddr, nbytes);
    }
    if (mq) osSendMesg(mq, (OSMesg) mb, OS_MESG_NOBLOCK);
    return 0;
}

/* ------------------------------------------------------------------ AI (port/sdl/audio_sdl.c) */
extern void port_audio_out_push(const s16 *samples, u32 bytes);
extern u32 port_audio_out_queued_bytes(void);
static u32 sAiFreq = 22050;
extern void port_audio_out_set_rate(u32 freq);
s32 osAiSetFrequency(u32 f) { sAiFreq = f; port_audio_out_set_rate(f); return (s32) f; }
/* DKR_FAST: the queue drains on the virtual clock instead of the sound card,
 * so the frame sizes -- and with them every "has this sound finished" test in
 * the game (the countdown's engine revs, voice lines) -- come out the same on
 * every machine, fast or slow (the x86 proxy and qemu-arm stay in step). */
static u64 sVirtQueue, sVirtLastUs, sVirtFrac;
static void virt_drain(void) {
    const u64 num = (sVirtualUs - sVirtLastUs) * sAiFreq + sVirtFrac;
    const u64 played = num / 1000000ull;
    sVirtFrac = num % 1000000ull;
    sVirtLastUs = sVirtualUs;
    sVirtQueue = sVirtQueue > played ? sVirtQueue - played : 0;
}
/* port_music_tail() (main.c) waits for the output to go quiet: the loudest
 * sample of the last frame pushed while it watches (above s16 range until one is). */
static int sAiWatch;
static u32 sAiPeak;
u32 port_ai_peak(int watch) {
    if (watch && !sAiWatch) sAiPeak = 0x10000;
    sAiWatch = watch;
    return sAiPeak;
}
/* An adaptive aim (PM64 fb7ae20, the matrix's X19), OFF by default: the longest recent gap between top-ups,
 * a quarter more and one 16 ms output buffer, 50-150 ms, the longest gap decaying by 1/64 a top-up. It
 * helped Paper Mario, whose queue ran dry on 60-160 ms frames; DKR refills to its cushion every frame in
 * proportion to the fields that passed, and the load mixer backstops a stall, so under qemu in real time
 * with every frame slowed it didn't help: 19 -> 20 underruns at ~42 ms frames, 15 -> 20 at ~57 ms
 * (DKR_FRAMEDELAY 20/35, 2026-09-28). DKR_AUDIOAIM=0: adaptive; <ms>: a fixed aim; default 67 (as
 * before). Real time only: under DKR_FAST the cushion stays AI_CUSHION_SAMPLES. */
static u64 sAiMarkUs, sAiGapMaxUs;
#define AI_CUSHION_SAMPLES 1470u /* the fixed cushion, ~67 ms at 22050 Hz (osAiGetLength) */
static u32 ai_cushion_samples(void) {
    static int sFixedMs = -2; /* -1: AI_CUSHION_SAMPLES (the default); 0: adaptive; > 0: that many ms */
    if (sFixedMs == -2) { const char *e = getenv("DKR_AUDIOAIM"); sFixedMs = e && e[0] ? atoi(e) : -1; }
    if (sFixedMs < 0) return AI_CUSHION_SAMPLES;
    u64 aim_us = sAiGapMaxUs + sAiGapMaxUs / 4 + 16000u;
    aim_us = aim_us < 50000u ? 50000u : aim_us > 150000u ? 150000u : aim_us;
    if (sFixedMs > 0) aim_us = (u64) sFixedMs * 1000u;
    return (u32) (aim_us * sAiFreq / 1000000u);
}
s32 osAiSetNextBuffer(void *buf, u32 size) {
    port_audio_out_push((const s16 *) buf, size);
    if (!fast_mode()) {
        const u64 now = now_us(), gap = sAiMarkUs ? now - sAiMarkUs : 0;
        sAiMarkUs = now;
        sAiGapMaxUs -= sAiGapMaxUs >> 6;
        if (gap > sAiGapMaxUs && gap < 1000000u) sAiGapMaxUs = gap;
    }
    if (fast_mode()) { virt_drain(); sVirtQueue += size / 4; }
    if (sAiWatch) {
        const s16 *s = (const s16 *) buf;
        u32 i, p = 0;
        for (i = 0; i < size / 2; i++) {
            const u32 a = (u32) (s[i] < 0 ? -s[i] : s[i]);
            if (a > p) p = a;
        }
        sAiPeak = p;
    }
    return 0;
}
/* The N64 AI holds at most the playing buffer plus one queued, and the audio
 * manager sizes each frame as frameSize - (samples left) + a few, which only
 * makes sense for "left" within one frame (a larger value goes negative and
 * wraps). The port's output queue is deeper, so report the part of it above a
 * fixed cushion (~67 ms at 22050 Hz), clamped to one frame: the game then
 * steers the queue to the cushion exactly as it steered the N64 DAC. */
#define AI_FRAME_SAMPLES 736u
u32 osAiGetLength(void) {
    u32 q;
    if (fast_mode()) {
        virt_drain();
        q = (u32) sVirtQueue;
    } else {
        q = port_audio_out_queued_bytes() / 4;
    }
    const u32 cushion = fast_mode() ? AI_CUSHION_SAMPLES : ai_cushion_samples();
    q = q > cushion ? q - cushion : 0;
    if (q > AI_FRAME_SAMPLES) q = AI_FRAME_SAMPLES;
    return q * 4;
}

/* ------------------------------------------------------------------ controllers */
extern void port_input_read(OSContPad *pad); /* port/sdl/controller_sdl.c */
s32 osContInit(OSMesgQueue *mq, u8 *bitpattern, OSContStatus *status) {
    *bitpattern = 1;
    memset(status, 0, sizeof(OSContStatus) * MAXCONTROLLERS);
    status[0].type = CONT_TYPE_NORMAL;
    status[0].status = 0; /* no pak: CONT_CARD_ON clear */
    for (int i = 1; i < MAXCONTROLLERS; i++) status[i].errno = CONT_NO_RESPONSE_ERROR;
    return 0;
}
s32 osContStartReadData(OSMesgQueue *mq) {
    if (mq) osSendMesg(mq, (OSMesg) 0, OS_MESG_NOBLOCK);
    return 0;
}
void osContGetReadData(OSContPad *pad) {
    memset(pad, 0, sizeof(OSContPad) * MAXCONTROLLERS);
    port_input_read(&pad[0]);
    for (int i = 1; i < MAXCONTROLLERS; i++) pad[i].errno = CONT_NO_RESPONSE_ERROR;
}

/* ------------------------------------------------------------------ EEPROM (4 Kbit)
 * eeprom.bin in the save dir. Writes arrive 8 bytes at a time (a save file is
 * 5 blocks, a full settings write 40+); each changed block marks the image
 * dirty and port_eeprom_flush(), called once per frame, writes the whole 512
 * bytes atomically: tmp, fsync, rename, fsync the dir.
 *
 * eeprom.bin is all of the player's progress, so a write never truncates the
 * only copy, and a power cut (flat battery) mid-write leaves the old file
 * whole. vfat renames over an existing file in two steps (drop the old entry,
 * rename the new one), so a cut between them leaves only the .tmp -- fsynced
 * whole, so the load takes it and finishes the rename. (the MK64 port 65d7488) */
#include <errno.h> /* here, after the controller code: libultra's structs have a field named errno */
#define EEP_SIZE 512
static u8 sEeprom[EEP_SIZE];
static int sEepromLoaded, sEepromDirty;
/* eeprom.bin is there but could not be opened or read (a card read error: EIO): the game then saw a
 * blank EEPROM and formatted it, and the next flush renamed that blank image over the player's file.
 * Such a file is kept as it is for the session: every write is refused, logged and shown
 * (the Paper Mario port 742bdb4; the SSB64 port S3 is the same shape). A missing file is a new save, as before. */
static int sEepromKeep;
/* The bytes read into sEeprom (zero-padded): EEP_SIZE = a whole image. *err: 0, or why a file that is
 * there could not be opened or read whole (a missing file is 0: no save yet). */
static size_t eeprom_read(const char *path, int *err) {
    FILE *f;
    size_t n = 0;
    memset(sEeprom, 0, sizeof(sEeprom));
    *err = 0;
    errno = 0;
    f = fopen(path, "rb");
    if (!f) { if (errno != ENOENT) *err = errno ? errno : EIO; return 0; }
    n = fread(sEeprom, 1, EEP_SIZE, f);
    if (n < EEP_SIZE && ferror(f)) *err = errno ? errno : EIO;
    fclose(f);
    return n;
}
/* A rename onto eeprom.bin reaches the card: the file first, which writes its directory slot (on the
 * card's vfat a rename only marks the moved inode dirty: until its writeback, ~30 s, the slot keeps
 * the old file's clusters, which the folder's fsync already frees in the FAT; the SSB64 port cd5128c),
 * then the folder. */
static void eeprom_sync_renamed(const char *dst) {
    const int f2 = open(dst, O_RDONLY);
    if (f2 >= 0) { fsync(f2); close(f2); }
    const int dir = open(port_save_dir(), O_RDONLY);
    if (dir >= 0) { fsync(dir); close(dir); }
}
static void eeprom_load(void) {
    char tmp[600];
    const char *dst;
    size_t n;
    int err, err2;
    if (sEepromLoaded) return;
    sEepromLoaded = 1;
    dst = port_save_path("eeprom.bin");
    if (eeprom_read(dst, &err) == EEP_SIZE) return;
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    if (eeprom_read(tmp, &err2) == EEP_SIZE) {
        port_log_always("eeprom: eeprom.bin missing or short, using eeprom.bin.tmp\n");
        /* synced as a save's rename is: unsynced, a power cut in the next ~30 s could leave eeprom.bin's
         * slot empty or on the short file's freed clusters with the .tmp's entry already gone, and the
         * progress the .tmp held was lost at the boot after */
        if (rename(tmp, dst) == 0) eeprom_sync_renamed(dst);
        return;
    }
    n = eeprom_read(dst, &err2);
    if (n == EEP_SIZE) return; /* the first read's error was passing */
    if (err || err2) {
        sEepromKeep = 1;
        port_log_always("eeprom: eeprom.bin cannot be read (%s, %u bytes): kept as it is, no write replaces it this session\n",
                        strerror(err2 ? err2 : err), (unsigned) n);
        return;
    }
    /* No save yet (zeros: the game formats it), or a short file from an
     * interrupted in-place write: keep what's there; the game checks each record. */
    if (n > 0) port_log_always("eeprom: short file, rest zeroed\n");
}
/* A failed write: the game ignores the result, so a full or read-only card lost a record or a
 * cup without a word on the screen. The reason goes to the log and GAME SAVE FAILED to the OS bar,
 * as MK64 (37c208f) and GoldenEye (3b09a41) do; eeprom.bin keeps the previous image, and the next
 * write that works makes it whole again (it holds the whole image). */
static void eeprom_failed(const char *stage, int err) {
    port_log_always("eeprom: %s failed (%s), eeprom.bin kept\n", stage, strerror(err));
    handheld_menu_notify("GAME SAVE FAILED");
}
void port_eeprom_flush(void) {
    char tmp[600];
    const char *dst;
    int fd;
    ssize_t n;
    if (!sEepromDirty) return;
    sEepromDirty = 0;
    if (sEepromKeep) {
        port_log_always("eeprom: not written: eeprom.bin could not be read at the start\n");
        handheld_menu_notify("GAME SAVE FAILED");
        return;
    }
    dst = port_save_path("eeprom.bin");
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { eeprom_failed("open", errno); return; }
    n = write(fd, sEeprom, EEP_SIZE);
    if (n != EEP_SIZE || fsync(fd) != 0) {
        const int err = n < 0 || n == EEP_SIZE ? errno : ENOSPC; /* a short write sets no errno */
        close(fd); remove(tmp); eeprom_failed(n != EEP_SIZE ? "write" : "fsync", err); return;
    }
    if (close(fd) != 0) { const int err = errno; remove(tmp); eeprom_failed("close", err); return; }
    if (rename(tmp, dst) != 0) { const int err = errno; remove(tmp); eeprom_failed("rename", err); return; }
    eeprom_sync_renamed(dst);
    { extern u32 port_frame_count(void); port_log("eeprom: saved (iteration %u)\n", (unsigned) port_frame_count()); }
}
/* statefix A (the port kit k1.8; savestate.c calls it after a load that happened, the audio parked):
 * sEeprom is outside the state, so it already holds the card's save after a load; what the load rolled
 * back is the game's RAM copies of it, which the game writes back whole later. save_data.c's
 * port_save_reapply puts the card's save back over them (its comment says which and why); a state with
 * nothing saved since it is left exactly as it was. sEeprom is the source: eeprom.bin plus any write
 * still to reach it (one whose flush failed), read now if this process hasn't yet (a resume at boot).
 * eeprom.bin unreadable at the start (sEepromKeep): the state's copies stand. One line per load.
 * DKR_SAVEREAPPLY=0: as before (the state's copies stand; tests). */
extern s32 port_save_reapply(u8 *card, s32 *records);
void hh_game_save_reapply(void) {
    s32 r, records = 0;
    {
        const char *e = getenv("DKR_SAVEREAPPLY");
        if (e && e[0] == '0') return;
    }
    eeprom_load();
    if (sEepromKeep) {
        port_log_always("eeprom: eeprom.bin could not be read at the start; the state's copies of the save stand\n");
        return;
    }
    r = port_save_reapply(sEeprom, &records);
    if (r & 0x100) {
        port_log_always("eeprom: the state is from before the game first read its save; it reads the card's itself\n");
    } else if (r & 0x200) {
        port_log_always("eeprom: the level table is larger than the scratch; the state's copies of the save stand\n");
    } else if (!(r & 0xF)) {
        port_log_always("eeprom: the state's save is the card's (nothing on the card is newer)%s%s\n", (r & 0x400) ? " (the card's settings block is bad: the state's stands)" : "",
                        (r & 0x800) ? " (the card's slot for the file in play is empty: the state's file stands)" : "");
    } else {
        char what[128] = "";
        size_t n = 0;
        if (r & 1) n += snprintf(what + n, sizeof(what) - n, ", %d record%s", (int) records, records == 1 ? "" : "s");
        if ((r & 2) && n < sizeof(what)) n += snprintf(what + n, sizeof(what) - n, ", the settings block");
        if ((r & 4) && n < sizeof(what)) n += snprintf(what + n, sizeof(what) - n, ", adventure file %d", (int) ((r >> 12) & 3) + 1);
        if ((r & 8) && n < sizeof(what)) snprintf(what + n, sizeof(what) - n, ", the file-select copies");
        port_log_always("eeprom: the card's save is newer than the state's: put back %s\n", what + 2);
    }
}

s32 osEepromProbe(OSMesgQueue *mq) { return EEPROM_TYPE_4K; }
s32 osEepromRead(OSMesgQueue *mq, u8 block, u8 *buf) {
    eeprom_load();
    if (block >= EEP_SIZE / 8) return -1;
    memcpy(buf, sEeprom + block * 8, 8);
    return 0;
}
s32 osEepromWrite(OSMesgQueue *mq, u8 block, u8 *buf) {
    eeprom_load();
    if (block >= EEP_SIZE / 8) return -1;
    if (memcmp(sEeprom + block * 8, buf, 8) != 0) {
        memcpy(sEeprom + block * 8, buf, 8);
        sEepromDirty = 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ Controller Pak / Rumble (none yet) */
s32 osPfsIsPlug(OSMesgQueue *mq, u8 *pattern) { *pattern = 0; return 0; }
s32 osPfsInit(OSMesgQueue *mq, OSPfs *pfs, int ch) { return PFS_ERR_NOPACK; }
s32 osPfsReFormat(OSPfs *pfs, OSMesgQueue *mq, int ch) { return PFS_ERR_NOPACK; }
s32 osPfsChecker(OSPfs *pfs) { return PFS_ERR_NOPACK; }
s32 osPfsFindFile(OSPfs *pfs, u16 c, u32 g, u8 *n, u8 *e, s32 *f) { return PFS_ERR_NOPACK; }
s32 osPfsAllocateFile(OSPfs *pfs, u16 c, u32 g, u8 *n, u8 *e, int len, s32 *f) { return PFS_ERR_NOPACK; }
s32 osPfsDeleteFile(OSPfs *pfs, u16 c, u32 g, u8 *n, u8 *e) { return PFS_ERR_NOPACK; }
s32 osPfsReadWriteFile(OSPfs *pfs, s32 f, u8 flag, int off, int n, u8 *buf) { return PFS_ERR_NOPACK; }
s32 osPfsFileState(OSPfs *pfs, s32 f, OSPfsState *st) { return PFS_ERR_NOPACK; }
s32 osPfsNumFiles(OSPfs *pfs, s32 *max, s32 *used) { return PFS_ERR_NOPACK; }
s32 osPfsFreeBlocks(OSPfs *pfs, s32 *bytes) { return PFS_ERR_NOPACK; }
s32 osMotorInit(OSMesgQueue *mq, OSPfs *pfs, int ch) { return PFS_ERR_NOPACK; }
s32 osMotorStart(OSPfs *pfs) { return 0; }
s32 osMotorStop(OSPfs *pfs) { return 0; }

/* ------------------------------------------------------------------ debug */
void rmonPrintf(const char *fmt, ...) { }
