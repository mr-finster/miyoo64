/**
 * The port's "cartridge": sf64.rom, an image with the decompressed ROM's
 * layout in which every asset bank has been replaced by the port's own build
 * of it (native byte order, same size and symbol addresses as the N64 bank --
 * tools/handheld/make_portrom.py on the desktop, the first-boot builder on the
 * device). Audio and everything else keep the ROM's bytes.
 *
 * Because the layout is the decompressed ROM's, the game's own DMA table and
 * its *_ROM_START symbols (rom_syms.ld) are file offsets here, and every PI
 * DMA (Lib_DmaRead, the audio loader's osEPiStartDma) is a memcpy out of a
 * read-only mapping: page cache the kernel can drop, never counted as heap.
 *
 * The image is never in the game folder: it is built from the player's ROM.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ultra64.h>
#include "port.h"
#include "savestate.h"

static const u8* sRom SAVESTATE_EXCLUDE;   /* mapping: process lifetime, never saved */
static size_t sRomSize SAVESTATE_EXCLUDE;

int port_rom_open(void) {
    const char* path = getenv("SF64_ROM");
    struct stat st;
    int fd;

    if (path == NULL || path[0] == '\0') {
        path = port_save_path("sf64.rom");
    }
    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 0x100000) {
        port_log_always("port_rom: cannot open %s\n", path);
        if (fd >= 0) close(fd);
        return 0;
    }
    sRom = mmap(NULL, (size_t) st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (sRom == MAP_FAILED) {
        sRom = NULL;
        port_log_always("port_rom: mmap %s failed\n", path);
        return 0;
    }
    sRomSize = (size_t) st.st_size;
    { /* a device-built image ends in a trailer (port_romimage.c): not ROM space */
        extern u32 port_romimage_payload_size(u32 file_size, const u8* tail);
        const u32 payload = port_romimage_payload_size((u32) sRomSize, sRom + sRomSize - 24);
        if (payload != 0) sRomSize = payload;
    }
    port_log("port_rom: %s, %u bytes\n", path, (unsigned) sRomSize);
    return 1;
}

void port_rom_read(uintptr_t romAddr, void* dst, size_t size) {
    if (sRom == NULL || romAddr >= sRomSize || size > sRomSize - romAddr) {
        port_log_always("port_rom: read %08X+%X outside the image (%X)\n", (unsigned) romAddr, (unsigned) size,
                        (unsigned) sRomSize);
        memset(dst, 0, size);
        return;
    }
    memcpy(dst, sRom + romAddr, size);
}
