/* hh_hw.c: the N64 hardware behind libultra's device calls -- cartridge (PI), controllers
 * (SI), EEPROM, audio interface, video interface, RDP registers, caches and TLB.
 * Everything completes immediately; completions are delivered as the same messages the
 * N64 would send (hh_os.c). */
#include <ultra64.h>
#include <PR/os.h>
#include <PR/os_internal.h>
#include <PR/rcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "hh_os.h"
#include "hh_hw.h"
#include "hh_plat.h"
#include "savestate.h" /* SAVESTATE_EXCLUDE: host state a state load must not overwrite (port/kit) */
#include "handheld_menu.h" /* handheld_menu_notify: the OS caption (a no-op off the handheld) */

/* ------------------------------------------------------------------------------------ */
/* system                                                                               */

u32 osTvType = OS_TV_NTSC;
u32 osResetType = 0; /* cold reset */
u32 osMemSize = 0x400000;
u64 osClockRate = 62500000ull;

void osInitialize(void) {}

u32 __osGetFpcCsr(void) { return 0; }
u32 __osSetFpcCsr(u32 value) { (void)value; return 0; }

/* Everything the game can hand to the RCP sits in KSEG0: the 4 MB DRAM block at
 * 0x80000000 and this binary above it (notes/m2-design.md). */
u32 osVirtualToPhysical(void *addr)
{
    u32 a = (u32)(uintptr_t)addr;
    if (a >= 0x80000000u && a < 0xC0000000u)
        return a & 0x1FFFFFFFu;
    if (a != 0) {
        static int warned;
        if (warned++ < 8)
            fprintf(stderr, "[hh_hw] osVirtualToPhysical(%08x): outside KSEG0\n", a);
    }
    return a;
}

void osWritebackDCache(void *p, s32 n) { (void)p; (void)n; }
void osWritebackDCacheAll(void) {}
void osInvalDCache(void *p, s32 n) { (void)p; (void)n; }
void osInvalICache(void *p, s32 n) { (void)p; (void)n; }
void osUnmapTLB(s32 i) { (void)i; }
void osUnmapTLBAll(void) {}


/* ------------------------------------------------------------------------------------ */
/* cartridge                                                                            */

const u8 *hh_rom SAVESTATE_EXCLUDE;   /* big-endian (z64) image, set up by hh_boot.c */
u32 hh_rom_size SAVESTATE_EXCLUDE;

/* The ROM pages (4 KB) read for the first time this session, a running total (the STALL line, hh_gfx.c): the ROM
 * is mapped from the card (hh_boot.c), so a first read of a page waits for the card unless it was read ahead. */
u32 hh_rom_cold_pages SAVESTATE_EXCLUDE;
static u32 sRomSeen[(16u << 20) / 4096 / 32] SAVESTATE_EXCLUDE;
static void hh_rom_touch(u32 off, u32 len)
{
    u32 pg, last;
    if (len == 0)
        return;
    last = (off + len - 1) >> 12;
    for (pg = off >> 12; pg <= last && pg < sizeof(sRomSeen) * 8; pg++)
        if (!(sRomSeen[pg >> 5] & (1u << (pg & 31)))) {
            sRomSeen[pg >> 5] |= 1u << (pg & 31);
            hh_rom_cold_pages++;
        }
}

void hh_rom_read(void *dst, u32 romaddr, u32 len)
{
    u32 off = romaddr & 0x0FFFFFFFu;
    if (off >= hh_rom_size || len > hh_rom_size - off) {
        fprintf(stderr, "[hh_hw] ROM read %08x+%x past the end (%x)\n", romaddr, len, hh_rom_size);
        memset(dst, 0, len);
        if (off < hh_rom_size)
            memcpy(dst, hh_rom + off, hh_rom_size - off);
        return;
    }
    hh_rom_touch(off, len);
    memcpy(dst, hh_rom + off, len);
}

void osCreatePiManager(OSPri pri, OSMesgQueue *cmdQ, OSMesg *cmdBuf, s32 cmdMsgCnt)
{
    (void)pri; (void)cmdQ; (void)cmdBuf; (void)cmdMsgCnt;
}

s32 osPiStartDma(OSIoMesg *mb, s32 priority, s32 direction, u32 devAddr, void *dramAddr, u32 size,
                 OSMesgQueue *mq)
{
    (void)priority;
    if (direction != OS_READ)
        hh_fatal("PI write to the cartridge (%08x)", devAddr);
    hh_rom_read(dramAddr, devAddr, size);
    mb->hdr.type = OS_MESG_TYPE_DMAREAD;
    mb->hdr.retQueue = mq;
    mb->dramAddr = dramAddr;
    mb->devAddr = devAddr;
    mb->size = size;
    if (mq != NULL)
        osSendMesg(mq, (OSMesg)mb, OS_MESG_NOBLOCK);
    return 0;
}

s32 osPiRawStartDma(s32 direction, u32 devAddr, void *dramAddr, u32 size)
{
    if (direction != OS_READ)
        hh_fatal("PI raw write to the cartridge (%08x)", devAddr);
    hh_rom_read(dramAddr, devAddr, size);
    return 0;
}

u32 osPiGetStatus(void) { return 0; }

/* The development command line (token.c tokenReadIo: 160 words at 0xFFB000, past the end
 * of the cartridge). GE_ARGS supplies it, e.g. "-level_33" (Dam) or "-level_34 -hard2".
 * The game stores the words and then reads them as a byte string, so the bytes are packed
 * in host order here. Without GE_ARGS the buffer reads as empty. */
#define HH_TOKEN_IO 0xFFB000u
#define HH_TOKEN_IO_LEN (160 * 4)

s32 osPiReadIo(u32 devAddr, u32 *data)
{
    u8 b[4];
    if (devAddr >= HH_TOKEN_IO && devAddr < HH_TOKEN_IO + HH_TOKEN_IO_LEN) {
        const char *a = getenv("GE_ARGS");
        u32 off = devAddr - HH_TOKEN_IO, n = a ? (u32)strlen(a) : 0, k;
        for (k = 0; k < 4; k++)
            b[k] = (off + k < n && off + k + 1 < HH_TOKEN_IO_LEN) ? (u8)a[off + k] : 0;
        memcpy(data, b, 4);
        return 0;
    }
    hh_rom_read(b, devAddr, 4);
    *data = ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* controllers: one standard controller in port 1, no paks                              */

u16 hh_pad_buttons;
s8 hh_pad_stick_x, hh_pad_stick_y;

static void hh_cont_status(OSContStatus *status)
{
    int i;
    for (i = 0; i < MAXCONTROLLERS; i++) {
        status[i].type = i == 0 ? CONT_TYPE_NORMAL : 0;
        status[i].status = 0;
        status[i].errno = i == 0 ? 0 : CONT_NO_RESPONSE_ERROR;
    }
}

s32 osContInit(OSMesgQueue *mq, u8 *bitpattern, OSContStatus *status)
{
    (void)mq;
    *bitpattern = 1;
    hh_cont_status(status);
    return 0;
}

s32 osContStartQuery(OSMesgQueue *mq)
{
    (void)mq;
    hh_os_post_event(OS_EVENT_SI);
    return 0;
}

void osContGetQuery(OSContStatus *status) { hh_cont_status(status); }

s32 osContStartReadData(OSMesgQueue *mq)
{
    (void)mq;
    hh_os_post_event(OS_EVENT_SI);
    return 0;
}

void osContGetReadData(OSContPad *pad)
{
    int i;
    hh_input_poll();
    for (i = 0; i < MAXCONTROLLERS; i++) {
        pad[i].button = i == 0 ? hh_pad_buttons : 0;
        pad[i].stick_x = i == 0 ? hh_pad_stick_x : 0;
        pad[i].stick_y = i == 0 ? hh_pad_stick_y : 0;
        pad[i].errno = i == 0 ? 0 : CONT_NO_RESPONSE_ERROR;
    }
}

s32 osPfsInit(OSMesgQueue *mq, OSPfs *pfs, int channel)
{
    (void)mq; (void)pfs; (void)channel;
    return PFS_ERR_NOPACK;
}

s32 osMotorInit(OSMesgQueue *mq, OSPfs *pfs, int channel)
{
    (void)mq; (void)pfs; (void)channel;
    return PFS_ERR_NOPACK;
}

s32 osMotorStart(OSPfs *pfs) { (void)pfs; return PFS_ERR_NOPACK; }
s32 osMotorStop(OSPfs *pfs) { (void)pfs; return PFS_ERR_NOPACK; }

/* ------------------------------------------------------------------------------------ */
/* EEPROM: 4 Kbit (64 blocks of 8 bytes), kept in <save dir>/eeprom.bin                   */

/* Here, after the controller code: <errno.h>'s errno macro breaks libultra's OSContPad.errno
 * member (hh_plat.h), and nothing below uses that member. */
#include <errno.h>

#define HH_EEP_BLOCKS 64
static u8 g_eeprom[HH_EEP_BLOCKS * EEPROM_BLOCK_SIZE] SAVESTATE_EXCLUDE; /* the save file: never rolled back by a state */
static char g_eeprom_path[512] SAVESTATE_EXCLUDE;
int hh_eeprom_write_failed SAVESTATE_EXCLUDE; /* the platform shows a warning while set */
/* eeprom.bin was there at the start but could not be opened or read (a card read error): the image in
 * memory is not what it holds, so no write may replace it this session (the 2026-10-01 consistency
 * pass, the Paper Mario port 742bdb4). The game then sees an erased EEPROM and formats it: every write shows
 * GAME SAVE FAILED, and the file waits for a start that can read it. */
static int g_eeprom_keep SAVESTATE_EXCLUDE; /* that read's errno; 0: none */

/* 1 if path held a whole image (a short one is left in g_eeprom, the rest erased). *err: 0, or why
 * a file that is there could not be opened or read (a missing file is 0). */
static int hh_eeprom_read(const char *path, int *err)
{
    FILE *f;
    size_t n = 0;
    memset(g_eeprom, 0xFF, sizeof(g_eeprom)); /* erased */
    *err = 0;
    errno = 0;
    f = fopen(path, "rb");
    if (f != NULL) {
        n = fread(g_eeprom, 1, sizeof(g_eeprom), f);
        if (n != sizeof(g_eeprom) && ferror(f))
            *err = errno != 0 ? errno : EIO;
        fclose(f);
    } else if (errno != ENOENT) {
        *err = errno != 0 ? errno : EIO;
    }
    return n == sizeof(g_eeprom);
}

/* A rename onto eeprom.bin, on the card: the renamed file itself (fd, still open; -1: opened
 * here), then its folder. On the card's vfat (Linux 4.14 vfat_rename) a rename over an existing
 * file moves the new file into the OLD file's directory slot (a new name: a slot made with cluster 0
 * and size 0) and only marks its inode dirty: the slot's start cluster and size reach the card at
 * that inode's writeback (~30 s) or its own fsync. The folder's fsync writes the removed .tmp entry
 * and the FAT with the old file's clusters freed, not that slot, so a hard power cut in those ~30 s
 * (a hang and a forced power-off, a flat battery) left eeprom.bin pointing at the freed clusters of
 * the save before: the progress lost, the FAT inconsistent. The 2026-10-01 consistency pass, as
 * the SSB64 port cd5128c (sram.bin), the DKR port 9c91d40 and the Paper Mario port 20d93fc. */
static void hh_eeprom_sync_rename(int fd)
{
    char dir[512], *slash;
    int d;
    if (fd >= 0) {
        fsync(fd);
    } else if ((d = open(g_eeprom_path, O_RDONLY)) >= 0) {
        fsync(d);
        close(d);
    }
    snprintf(dir, sizeof(dir), "%s", g_eeprom_path);
    slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = 0;
        d = open(dir[0] ? dir : "/", O_RDONLY);
        if (d >= 0) {
            fsync(d);
            close(d);
        }
    }
}

/* eeprom.bin, else a whole eeprom.bin.tmp: vfat renames over a file in two steps (drop the
 * old entry, rename the new one), so a power cut between them leaves only the .tmp -- written
 * and fsynced whole -- and the rename is finished here (MK64 65d7488), then synced as a save's. */
void hh_eeprom_load(const char *path)
{
    char tmp[520];
    int err;
    snprintf(g_eeprom_path, sizeof(g_eeprom_path), "%s", path);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    g_eeprom_keep = 0;
    if (hh_eeprom_read(path, &err))
        return;
    if (hh_eeprom_read(tmp, &err)) {
        fprintf(stderr, "[hh_hw] %s missing or short: using %s\n", path, tmp);
        if (rename(tmp, path) == 0)
            hh_eeprom_sync_rename(-1);
        return;
    }
    /* no save yet (erased: the game formats it), or a short file: keep what is there, the
     * game checks each record. One that is there but can't be read is never replaced. */
    if (!hh_eeprom_read(path, &err)) {
        if (err != 0) {
            g_eeprom_keep = err;
            fprintf(stderr, "[hh_hw] %s cannot be read (%s): kept as it is, no game save replaces it this session\n",
                    path, strerror(err));
        } else {
            fprintf(stderr, "[hh_hw] %s: no complete EEPROM image, starting from what is there\n", path);
        }
    }
}

/* Write the whole image to a temporary file and rename it over the old one, so a power
 * cut never leaves a torn save. Failures are reported, never silent (MK64 round 24): in the log,
 * and on the screen (the 2026-10-01 sweep: until then only the log said so, and the game ignores
 * the write's result, so a full or read-only card lost a mission's progress without a word). The
 * the handheld OS caption, as the kit shows SAVE FAILED for a state; a later write that works makes the
 * file whole again (it holds the whole image). The log line names the stage and the reason (the
 * consistency pass, as MK64 37c208f / DKR 8938223); a .tmp not known whole on the card (open, write
 * or fsync failed) is removed, so the load's .tmp fallback can never take it. One whose rename failed
 * is whole and fsynced: it stays, and is what that fallback takes if eeprom.bin is missing. */
static s32 hh_eeprom_flush(void)
{
    char tmp[520];
    const char *stage = NULL;
    int err = 0;
    FILE *f;
    if (g_eeprom_path[0] == 0)
        return 0;
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_eeprom_path);
    if (g_eeprom_keep != 0) {
        fprintf(stderr, "[hh_hw] not saving %s: it could not be read at the start (%s), and this image would "
                "replace a save it never saw; eeprom.bin kept\n", g_eeprom_path, strerror(g_eeprom_keep));
        stage = ""; /* logged */
    } else {
        errno = 0;
        f = fopen(tmp, "wb");
        if (f == NULL) {
            stage = "open";
            err = errno;
        } else {
            if (fwrite(g_eeprom, 1, sizeof(g_eeprom), f) != sizeof(g_eeprom) || fflush(f) != 0) {
                stage = "write";
                err = errno != 0 ? errno : ENOSPC; /* a short write may set none */
            } else if (fsync(fileno(f)) != 0) { /* whole on the card before the rename */
                stage = "fsync";
                err = errno;
            } else if (rename(tmp, g_eeprom_path) != 0) {
                stage = "rename";
                err = errno;
            } else { /* and the rename itself: the file, still open, then its folder */
                hh_eeprom_sync_rename(fileno(f));
            }
            fclose(f); /* after the rename: its result never mattered after a good fsync */
            if (stage != NULL && strcmp(stage, "rename") != 0)
                remove(tmp);
        }
    }
    if (stage != NULL) {
        if (stage[0] != 0)
            fprintf(stderr, "[hh_hw] saving %s failed at %s: %s; eeprom.bin kept\n", g_eeprom_path, stage,
                    strerror(err != 0 ? err : EIO));
        handheld_menu_notify("GAME SAVE FAILED");
        hh_eeprom_write_failed = 1;
        return CONT_NO_RESPONSE_ERROR;
    }
    hh_eeprom_write_failed = 0;
    return 0;
}

/* Every game save on the card, timed (it runs in the game's thread: a slow card's fsyncs stall the frame) */
static s32 hh_eeprom_flush_timed(u8 address, int nbytes)
{
    extern s32 g_StageNum;
    const u64 t0 = hh_host_ns();
    const s32 r = hh_eeprom_flush();
    fprintf(stderr, "[hh_hw] game save: blocks %u-%u written to the card in %.1f ms (stage %d)\n", (unsigned)address,
            (unsigned)(address + (nbytes + EEPROM_BLOCK_SIZE - 1) / EEPROM_BLOCK_SIZE - 1),
            (hh_host_ns() - t0) / 1e6, (int)g_StageNum);
    return r;
}

s32 osEepromProbe(OSMesgQueue *mq) { (void)mq; return EEPROM_TYPE_4K; }

/* In a state, on purpose (not SAVESTATE_EXCLUDE): whether the game had read its save when the state
 * was made (hh_game_save_reapply). */
int hh_eeprom_game_read;

s32 osEepromLongRead(OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes)
{
    (void)mq;
    if (address >= HH_EEP_BLOCKS || nbytes < 0 || (u32)address * EEPROM_BLOCK_SIZE + nbytes > sizeof(g_eeprom))
        return -1;
    memcpy(buffer, g_eeprom + address * EEPROM_BLOCK_SIZE, nbytes);
    hh_eeprom_game_read = 1;
    return 0;
}

s32 osEepromRead(OSMesgQueue *mq, u8 address, u8 *buffer)
{
    return osEepromLongRead(mq, address, buffer, EEPROM_BLOCK_SIZE);
}

s32 osEepromLongWrite(OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes)
{
    (void)mq;
    if (address >= HH_EEP_BLOCKS || nbytes < 0 || (u32)address * EEPROM_BLOCK_SIZE + nbytes > sizeof(g_eeprom))
        return -1;
    memcpy(g_eeprom + address * EEPROM_BLOCK_SIZE, buffer, nbytes);
    return hh_eeprom_flush_timed(address, nbytes);
}

s32 osEepromWrite(OSMesgQueue *mq, u8 address, u8 *buffer)
{
    return osEepromLongWrite(mq, address, buffer, EEPROM_BLOCK_SIZE);
}

/* statefix A (the port kit k1.8: the kit's savestate.c calls this after a load that happened, every game
 * thread parked, before the texture fixup). g_eeprom -- eeprom.bin as this session holds it: read at the
 * start, then every game write -- is never in a state, so the file itself was never rolled back; but the
 * game's RAM copy of it is, and the game writes from that copy. GoldenEye reads the EEPROM once
 * (fileValidateSaves, at the legal screen) into saves[0..4], the five 96-byte folder slots of bytes
 * 32-511 (four folders and a free slot that moves at each write: per mission and difficulty best times
 * and completion, cheats unlocked, 007 mode, the folder's options word, look and volumes), and from then
 * on every change to a folder is written at once: built from saves[] (fileOverwriteSaveSlotWithNewSave
 * writes the free slot and clears the old one), so after any write saves[0..4] are byte for byte
 * g_eeprom's bytes 32-511. A state from before a later write restored the old saves[]: the folder select
 * and mission select showed the old progress, and the next write (a mission end, a cheat, the watch's
 * options, a copy or a delete) built the folder from the old copy, into the old free slot, and cleared the
 * slot the newer folder lived in: the newer progress gone from the card too.
 *  - The card's bytes 32-511 are the state's saves[0..4]: nothing was written since the state; nothing
 *    changes (the load is exact).
 *  - They differ: the card's five slots replace saves[0..4] whole (file2.c fileReapplyCardSave), and
 *    save_selected_bond[] is derived from them again as the boot read does. No merge: GoldenEye never
 *    holds unwritten progress in saves[] (each change is written in the same call), so the card holds
 *    all of it; a mission in play when the state was made writes into the card's folder at its end
 *    (fileUnlockStageInFolderAtDifficulty keeps the better time), as on the console. A write in flight
 *    is never in a state (hh_game_save_busy, hh_plat.c: states wait for the write to finish).
 *  - Left as the state's: the live game -- the folder in play and its difficulty (selected_folder_num,
 *    selected_difficulty), and the settings the mission started with (fileLoadSettingsForFolder at each
 *    mission start: look, aim, control style, screen, music and effects volume). They are what the watch
 *    shows; the watch writes them into this folder's options word when it closes with them different, the
 *    same as changing them there (only those bytes: the progress comes from the card). The RAMROM demo
 *    slot saves[5] is not on the card.
 *  - The state is from before the game first read the EEPROM (the boot logos): the game reads g_eeprom
 *    itself later. eeprom.bin could not be read at the start (g_eeprom_keep): the state's copy stands, no
 *    write replaces the file this session. A card image whose slots fail their checksum is not one the
 *    game wrote (eeprom.bin gone or damaged since the start): the state's copy stands.
 * GE_SAVEREAPPLY=0: as before (the state's copy stands; tests). */
void hh_game_save_reapply(void)
{
    extern s32 fileReapplyCardSave(const u8 *card);
    static int sOn SAVESTATE_EXCLUDE = -1;
    s32 r;
    if (sOn < 0) {
        const char *e = getenv("GE_SAVEREAPPLY");
        sOn = !(e && e[0] == '0');
    }
    if (!sOn)
        return;
    if (!hh_eeprom_game_read) {
        fprintf(stderr, "eeprom: the state is from before the game read its save; it reads eeprom.bin itself\n");
        return;
    }
    if (g_eeprom_keep != 0) {
        fprintf(stderr, "eeprom: eeprom.bin could not be read at the start; the state's copy stands, no write replaces the file\n");
        return;
    }
    r = fileReapplyCardSave(g_eeprom);
    if (r == 0)
        fprintf(stderr, "eeprom: the state's save is the card's\n");
    else if (r < 0)
        fprintf(stderr, "eeprom: %d of the card's folder slots fail their checksum; the state's copy stands\n", (int)-r);
    else
        fprintf(stderr, "eeprom: the card's save is newer than the state's (%d bytes): put back\n", (int)r);
}

/* tests (statefix): GE_SAVEWRITE_AT=<vi>: at the first game frame at or after that VI, the game's own
 * mission-end save (file.c hhTestMissionEndSave -> end_of_mission_briefing: the stage in play completed at
 * its difficulty in the folder in play -- GE_SAVEWRITE_FOLDER=<0-3> picks another -- with the mission time
 * so far; a cheat too if that time beats the target), on the game thread, as the debrief runs it. Called
 * from bossMainloop's frame (boss.c, PORT). */
void hh_test_savewrite(void)
{
    extern unsigned int hh_vicount;
    extern s32 hhTestMissionEndSave(s32 folder);
    extern s32 getMissiontimer(void);
    extern s32 selected_folder_num, selected_difficulty;
    extern void *fileGetSaveForFoldernum(u32 folder);
    extern s32 fileGetSaveStageDifficultyTime(void *save, s32 levelid, s32 difficulty);
    static int sAt SAVESTATE_EXCLUDE = -1;
    if (sAt < 0)
        sAt = getenv("GE_SAVEWRITE_AT") ? atoi(getenv("GE_SAVEWRITE_AT")) : 0;
    if (sAt > 0 && hh_vicount >= (unsigned)sAt) {
        const char *f = getenv("GE_SAVEWRITE_FOLDER");
        const s32 t = getMissiontimer();
        void *save;
        s32 m;
        sAt = 0;
        m = hhTestMissionEndSave(f ? atoi(f) : -1);
        save = fileGetSaveForFoldernum((u32)selected_folder_num);
        fprintf(stderr, "[hh_hw] test: the mission-end save at vi %u: folder %d, mission %d, difficulty %d, mission time %d "
                "ticks; its time now %d s\n", hh_vicount, (int)selected_folder_num, (int)m, (int)selected_difficulty, (int)t,
                m >= 0 && save ? (int)fileGetSaveStageDifficultyTime(save, m, selected_difficulty) : -1);
    }
}

/* ------------------------------------------------------------------------------------ */
/* audio interface: a 2-deep DMA FIFO playing at the DAC rate, in game time               */

#define HH_VI_NTSC_CLOCK 48681812u
static u32 g_ai_dacrate = 2208;
/* The N64's FIFO is 2 deep; here up to HH_AI_FIFO (round 19). Game time moves in event steps,
 * so after a 35-45 ms frame the audio thread mixes its 2-3 waiting frames at one instant and the
 * third found the FIFO full: osAiSetNextBuffer returned -1 and the frame never reached the card
 * (about 7% of the sound at 13 fps). The audio manager's frame sizing (osAiGetLength, the
 * current buffer's remainder) keeps production at the DAC rate either way. GE_AIFIFO=2: before. */
#define HH_AI_FIFO 8
static struct { u32 len; u64 start; } g_ai[HH_AI_FIFO];
static int g_ai_n, g_ai_depth SAVESTATE_EXCLUDE;
/* The end of the last buffer queued (game time), kept when the FIFO runs empty (round 19, the
 * user: sound "still skips every few seconds"). After a 35-70 ms frame the FIFO often ran dry in
 * game time; the next buffer started at `now`, so the idle gap was never made up and a level
 * produced 97-99% of the sound the card plays -- the card's cushion drained and refilled every
 * few seconds. A buffer that finds the FIFO empty now starts where the last one ended, up to
 * HH_AI_CATCHUP behind: osAiGetLength then reads it as (partly) played and the audio manager's
 * frame sizing mixes longer frames until the sound is back in step. A longer gap (a load, a
 * menu) starts at `now` as before. GE_AICATCHUP=0: always `now`. */
static u64 g_ai_end;
static int g_ai_catchup SAVESTATE_EXCLUDE = -1;
#define HH_AI_CATCHUP (HH_COUNT_HZ / 4)
u32 hh_ai_drops SAVESTATE_EXCLUDE; /* buffers refused (the FIFO full): each is a skip */
u64 hh_ai_samples SAVESTATE_EXCLUDE; /* stereo samples accepted, running total (the [plat] line) */

static u64 hh_ai_duration(u32 len)
{
    /* len/4 stereo samples, each dacrate VI-clock ticks, in 46.875 MHz counts */
    return (u64)(len / 4) * g_ai_dacrate * HH_COUNT_HZ / HH_VI_NTSC_CLOCK;
}

static void hh_ai_update(u64 now)
{
    while (g_ai_n > 0 && now >= g_ai[0].start + hh_ai_duration(g_ai[0].len)) {
        u64 end = g_ai[0].start + hh_ai_duration(g_ai[0].len);
        int i;
        for (i = 1; i < g_ai_n; i++)
            g_ai[i - 1] = g_ai[i];
        g_ai_n--;
        if (g_ai_n > 0 && g_ai[0].start < end)
            g_ai[0].start = end;
    }
}

s32 osAiSetFrequency(u32 frequency)
{
    f32 f = HH_VI_NTSC_CLOCK / (f32)frequency + .5f;
    u32 dacRate = (u32)f;
    if (dacRate < AI_MIN_DAC_RATE)
        return -1;
    g_ai_dacrate = dacRate;
    return HH_VI_NTSC_CLOCK / (s32)dacRate;
}

u32 osAiGetLength(void)
{
    u64 now = hh_os_now(), played;
    hh_ai_update(now);
    if (g_ai_n == 0 || now < g_ai[0].start)
        return g_ai_n ? g_ai[0].len : 0;
    played = (now - g_ai[0].start) * HH_VI_NTSC_CLOCK / ((u64)g_ai_dacrate * HH_COUNT_HZ) * 4;
    played &= ~7ull; /* the AI DMA moves 8-byte units */
    return played >= g_ai[0].len ? 0 : g_ai[0].len - (u32)played;
}

s32 osAiSetNextBuffer(void *buf, u32 len)
{
    u64 now = hh_os_now();
    hh_ai_update(now);
    if (g_ai_depth == 0) {
        const char *e = getenv("GE_AIFIFO");
        g_ai_depth = e && atoi(e) >= 2 && atoi(e) <= HH_AI_FIFO ? atoi(e) : HH_AI_FIFO;
    }
    if (g_ai_n == g_ai_depth) {
        hh_ai_drops++; /* the PERF line's "aud ... drop" */
        return -1;
    }
    if (g_ai_catchup < 0)
        g_ai_catchup = !(getenv("GE_AICATCHUP") && getenv("GE_AICATCHUP")[0] == '0');
    g_ai[g_ai_n].len = len;
    if (g_ai_n > 0)
        g_ai[g_ai_n].start = g_ai[g_ai_n - 1].start + hh_ai_duration(g_ai[g_ai_n - 1].len);
    else if (g_ai_catchup && g_ai_end != 0 && g_ai_end < now && now - g_ai_end <= HH_AI_CATCHUP)
        g_ai[g_ai_n].start = g_ai_end; /* the FIFO ran dry: continue from where the sound left off */
    else
        g_ai[g_ai_n].start = now;
    g_ai_end = g_ai[g_ai_n].start + hh_ai_duration(len);
    g_ai_n++;
    hh_ai_samples += len / 4;
    hh_plat_audio_out(buf, len, HH_VI_NTSC_CLOCK / g_ai_dacrate);
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* video interface                                                                      */

static void *g_vi_cur_fb, *g_vi_next_fb;
OSViMode *hh_vi_mode;
int hh_vi_black = 1;
f32 hh_vi_xscale = 1.0f, hh_vi_yscale = 1.0f;
u32 hh_vi_features;

void osViSetMode(OSViMode *mode) { hh_vi_mode = mode; }
void osViBlack(u8 active) { hh_vi_black = active; }
void osViSetSpecialFeatures(u32 func) { hh_vi_features = func; }
void osViSetXScale(f32 value) { hh_vi_xscale = value; }
void osViSetYScale(f32 value) { hh_vi_yscale = value; }
void osViRepeatLine(u8 active) { (void)active; }
void osViSwapBuffer(void *frameBufPtr) { g_vi_next_fb = frameBufPtr; }
void *osViGetCurrentFramebuffer(void) { return g_vi_cur_fb; }
void *osViGetNextFramebuffer(void) { return g_vi_next_fb; }

void hh_vi_retrace(u32 vicount)
{
    int new_frame = g_vi_next_fb != g_vi_cur_fb;
    g_vi_cur_fb = g_vi_next_fb;
    hh_plat_vi(vicount, new_frame);
}

/* ------------------------------------------------------------------------------------ */
/* RDP registers                                                                        */

void osDpSetStatus(u32 data) { (void)data; }

s32 osDpSetNextBuffer(void *buf, u64 size)
{
    /* RDP-only (FIFO) tasks: the command list was already drawn with its RSP task */
    (void)buf; (void)size;
    hh_os_post_event(OS_EVENT_DP);
    return 0;
}

void osDpGetCounters(u32 *counters) { memset(counters, 0, 4 * sizeof(u32)); }
