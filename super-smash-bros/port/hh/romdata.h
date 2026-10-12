/* romdata.h -- the player's ROM data, as the port sees it.
 *
 * The zip carries no game data. On first boot the port copies one slice of
 * the player's own ROM (normalised to .z64 byte order and SHA-1 checked) to
 * the save directory: ROM 0x001AC870..0x00F5F4E0, the relocData file table
 * and files plus the audio banks. Everything the game ever DMA'd from the
 * cartridge at runtime lives in that range. The slice is mmap'd read-only, so
 * it costs page cache, not process memory.
 *
 * All offsets below are ROM addresses (as the N64 code used them), not
 * offsets into the slice. Data is returned exactly as stored in the ROM:
 * big-endian, reloc files still vpk0-compressed on the cartridge. */
#ifndef HH_ROMDATA_H
#define HH_ROMDATA_H

#include <stddef.h>
#include <stdint.h>

#define ROMDATA_SLICE_START   0x001AC870u   /* US: relocData table (lLBRelocTableAddr) */
#define ROMDATA_SLICE_END     0x00F5F4E0u   /* US: end of fgm.ucd, start of rom_pad */
#define ROMDATA_RELOC_TABLE   0x001AC870u
#define ROMDATA_RELOC_COUNT   2132u
#define ROMDATA_RELOC_ENTRY   12u
#define ROMDATA_MAP_ADDR      0x60000000u   /* fixed mmap address of the slice (save states) */
#define ROMDATA_US_SHA1       "e2929e10fccc0aa84e5776227e798abc07cedabf"

/* ROM regions of the audio banks (splat yaml, US). */
#define ROMDATA_S1_MUSIC_SBK      0x00B277B0u
#define ROMDATA_B1_SOUNDS1_CTL    0x00B4E5C0u
#define ROMDATA_B1_SOUNDS1_TBL    0x00B54CE0u
#define ROMDATA_B1_SOUNDS2_CTL    0x00C6B650u
#define ROMDATA_B1_SOUNDS2_TBL    0x00C7B1F0u
#define ROMDATA_FGM_UNK           0x00F573D0u
#define ROMDATA_FGM_TBL           0x00F57BF0u
#define ROMDATA_FGM_UCD           0x00F5A9C0u

/* Particle banks: script (.scb) and texture (.txb) ROM ranges (splat yaml, US).
 * The decomp names them l<Owner>Particle{Script,Texture}Bank{Lo,Hi}. */
#define ROMDATA_EFCOMMON_SCB      0x00AC7340u
#define ROMDATA_EFCOMMON_TXB      0x00AC9DE0u
#define ROMDATA_PARTICLES0_SCB    0x00B16C80u
#define ROMDATA_PARTICLES0_TXB    0x00B17060u
#define ROMDATA_PARTICLES1_SCB    0x00B174A0u
#define ROMDATA_PARTICLES1_TXB    0x00B176A0u
#define ROMDATA_PARTICLES2_SCB    0x00B19700u
#define ROMDATA_PARTICLES2_TXB    0x00B19850u
#define ROMDATA_ITCOMMON_SCB      0x00B1BCA0u
#define ROMDATA_ITCOMMON_TXB      0x00B1BDE0u
#define ROMDATA_GRPUPUPU_SCB      0x00B1E640u
#define ROMDATA_GRPUPUPU_TXB      0x00B1E7E0u
#define ROMDATA_GRHYRULE_SCB      0x00B1F960u
#define ROMDATA_GRHYRULE_TXB      0x00B1FC80u
#define ROMDATA_GRYOSTER_SCB      0x00B22980u
#define ROMDATA_GRYOSTER_TXB      0x00B22A00u
#define ROMDATA_MNTITLE_SCB       0x00B22C30u
#define ROMDATA_MNTITLE_TXB       0x00B22D40u
#define ROMDATA_PARTICLES_END     0x00B277B0u   /* = S1_music_sbk */

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the extracted slice (building it from the ROM first if needed).
 * Returns 0 on success; on failure writes a reason for the error screen. */
int romdata_init(const char *save_dir, char *err, size_t err_size);

/* 1 if the slice is mapped at ROMDATA_MAP_ADDR (required for save states). */
int romdata_at_fixed_address(void);

/* Pointer to `size` bytes at ROM address `rom`, or NULL if out of the slice. */
const uint8_t *romdata_ptr(uint32_t rom, uint32_t size);

/* memcpy from ROM; returns 0 on success, -1 if out of range. */
int romdata_read(uint32_t rom, void *dst, uint32_t size);

/* One reloc file, as the N64 table describes it. */
typedef struct RomRelocInfo {
    uint32_t file_id;
    uint16_t reloc_intern;      /* word offset of the internal pointer chain, 0xFFFF = none */
    uint16_t reloc_extern;      /* word offset of the external pointer chain, 0xFFFF = none */
    uint32_t size;              /* decompressed size in bytes */
    uint32_t extern_count;      /* number of external file ids */
    const uint8_t *extern_ids;  /* big-endian u16[extern_count], points into the ROM slice */
} RomRelocInfo;

/* Fills `info` for `file_id`; returns 0, or -1 for a bad id. */
int romdata_reloc_info(uint32_t file_id, RomRelocInfo *info);

/* Reads file `file_id` into dst (info.size bytes), decompressing if needed.
 * The bytes are the original big-endian file; byte-swapping is the caller's
 * job (lbreloc bridge). Returns 0 on success. */
int romdata_reloc_read(uint32_t file_id, void *dst, uint32_t dst_size);

/* Extern file id `i` of a file (host order). */
static inline uint16_t romdata_extern_id(const RomRelocInfo *info, uint32_t i)
{
    return (uint16_t)((info->extern_ids[i * 2] << 8) | info->extern_ids[i * 2 + 1]);
}

#ifdef __cplusplus
}
#endif
#endif
