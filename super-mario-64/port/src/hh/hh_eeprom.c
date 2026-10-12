/**
 * The game's 4 Kbit EEPROM as a 512-byte file in the data dir (the MK64 handheld
 * port's writer, 65d7488). It is all of a player's progress, so a write never
 * truncates the only copy: the new image goes to eeprom.bin.tmp, fsync, rename
 * over eeprom.bin, fsync the dir. A power cut (flat battery) mid-write
 * leaves the old file whole. vfat renames over an existing file in two steps,
 * so a cut between them leaves only the .tmp -- fsynced whole, so load takes it
 * and finishes the rename.
 *
 * sEeprom mirrors the file and is game state (in save states), as on MK64: a
 * state restores the EEPROM the game saw then ("emulator-style", the user's
 * call there).
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <PR/ultratypes.h>
#include <PR/os_eeprom.h>
#include "macros.h"
#include "port.h"

#define EEPROM_FILE port_save_path("eeprom.bin")
#define EEPROM_TMP  port_save_path("eeprom.bin.tmp")
#define EEPROM_SIZE 512

static u8 sEeprom[EEPROM_SIZE];
static u8 sEepromLoaded;

/* 1 if path held a whole image; a short one is left zero-padded in sEeprom. */
static int eeprom_read(const char *path) {
    FILE *fp = fopen(path, "rb");
    size_t n = 0;
    memset(sEeprom, 0, sizeof(sEeprom));
    if (fp != NULL) {
        n = fread(sEeprom, 1, sizeof(sEeprom), fp);
        fclose(fp);
    }
    return n == sizeof(sEeprom);
}

static s32 eeprom_save(void);

static void eeprom_load(void) {
    if (sEepromLoaded) {
        return;
    }
    sEepromLoaded = 1;
    if (eeprom_read(EEPROM_FILE)) {
        return;
    }
    if (eeprom_read(EEPROM_TMP)) {
        port_log_always("eeprom: eeprom.bin missing or short, using eeprom.bin.tmp\n");
        rename(EEPROM_TMP, EEPROM_FILE);
        return;
    }
    /* The test builds (the test data folder, hh_main.c port_test_mode) with no save yet: start with
     * everything unlocked, a 120-star file A built from the game's own save structures (save_file.c
     * port_test_save_image), written as the game writes a save. Never in the player's data folder. */
    {
        extern int port_test_mode(void);
        extern void port_test_save_image(u8 *out);
        extern s32 port_test_save_stars(const u8 *image);
        FILE *fp = fopen(EEPROM_FILE, "rb");
        if (fp == NULL && port_test_mode()) {
            port_test_save_image(sEeprom);
            const int ok = eeprom_save() == 0;
            port_log_always("eeprom: test save: file A with %d stars, everything unlocked (%s)\n",
                            (int) port_test_save_stars(sEeprom), ok ? "written" : "write failed");
            return;
        }
        if (fp != NULL) fclose(fp);
    }
    /* no save yet (zeros: the game formats it), or a short file: keep what's
     * there, the game checks each save file's checksum */
    eeprom_read(EEPROM_FILE);
}

static s32 eeprom_save(void) {
    FILE *fp = fopen(EEPROM_TMP, "wb");
    int ok, dir;
    if (fp == NULL) {
        port_log_always("eeprom: cannot write %s (errno %d)\n", EEPROM_TMP, errno);
        return -1;
    }
    ok = fwrite(sEeprom, 1, sizeof(sEeprom), fp) == sizeof(sEeprom);
    ok = fflush(fp) == 0 && ok;
    { const u32 t = port_time_us(); ok = ok && fsync(fileno(fp)) == 0; PORT_WHY_ADD(WHY_EEFSYNC, port_time_us() - t); }
    ok = fclose(fp) == 0 && ok;
    if (!ok || rename(EEPROM_TMP, EEPROM_FILE) != 0) {
        port_log_always("eeprom: write failed, eeprom.bin kept\n");
        remove(EEPROM_TMP);
        return -1;
    }
    dir = open(port_save_dir(), O_RDONLY);
    if (dir >= 0) {
        const u32 t = port_time_us();
        fsync(dir);
        PORT_WHY_ADD(WHY_EEFSYNC, port_time_us() - t);
        close(dir);
    }
    return 0;
}

s32 osEepromProbe(UNUSED OSMesgQueue *mq) {
    return 1; /* EEPROM_TYPE_4K */
}

s32 osEepromLongRead(UNUSED OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes) {
    eeprom_load();
    if (address * 8 + nbytes > EEPROM_SIZE) {
        return -1;
    }
    memcpy(buffer, sEeprom + address * 8, nbytes);
    return 0;
}

s32 osEepromLongWrite(UNUSED OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes) {
    eeprom_load();
    { extern int port_bench2_on(void); if (port_bench2_on()) return 0; } /* the level bench (hh_bench.c) leaves the test save as it is */
    if (address * 8 + nbytes > EEPROM_SIZE) {
        return -1;
    }
    memcpy(sEeprom + address * 8, buffer, nbytes);
    const u32 t = port_time_us();
    const s32 r = eeprom_save();
    PORT_WHY_ADD(WHY_EEPROM, port_time_us() - t);
    return r;
}
