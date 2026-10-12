/* Recipe blob: how the port rebuilds the asset region from the player's ROM.
 * Written by tools/assets/emit_recipes.py (numbers only -- no game data) as
 * recipes.bin in the zip; parsed in scratch memory by assets_handheld.c.
 * From the MK64 port (after beckerd's PSP asset split).
 *
 * Layout: SM64RCP1 header | recipes | literals | patterns | runs (u16, padded
 * to 4) | blocks | relocs | trailer "BLID" + cache id, "STMP" + the
 * executable's build stamp. */
#ifndef PORT_ASSETS_H
#define PORT_ASSETS_H

enum { PA_RAW = 1, PA_MIO0 = 2, PA_LITERAL = 3, PA_RELOCS = 4 };
enum { PA_XF_ID = 0, PA_XF_SW16 = 1, PA_XF_SW32 = 2, PA_XF_PATTERN = 16 }; /* >= 16: pattern index + 16 */
/* Field widths inside one big-endian source word, each reversed to little-endian */
enum { PA_SHAPE_4 = 0, PA_SHAPE_22 = 1, PA_SHAPE_211 = 2, PA_SHAPE_112 = 3, PA_SHAPE_1111 = 4 };

typedef struct {
    char magic[8]; /* SM64RCP1 */
    unsigned int region_size, data_crc, recipe_count, literal_size, pattern_count, run_count, block_count, reloc_count,
        max_block, max_pattern_words;
} PortAssetBlobHeader;

typedef struct {
    unsigned int dst, size;
    unsigned short kind, xform;
    unsigned int src;   /* RAW: ROM offset. MIO0: ROM offset of the block. LITERAL: offset in the literal blob */
    unsigned int extra; /* MIO0: offset in the decompressed block */
} PortAssetRecipe;

/* A pattern: `run_count` runs from run `first` (each u16 = shape << 13 | words),
 * `nwords` words in all, repeating from its start every `nwords` words. A
 * short one is a struct's fields (a vertex: 22 22 22 1111); a long one is one
 * symbol's own mixed fields. */
typedef struct { unsigned int nwords, first, run_count; } PortAssetPattern;
typedef struct { unsigned int rom_off, rom_len, decomp_len; } PortAssetBlock;
typedef struct { unsigned int off, target; } PortAssetReloc; /* off bit 31 set: target is a module-relative link address; else region-relative */

#endif
