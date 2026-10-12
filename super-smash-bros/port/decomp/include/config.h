#ifndef CONFIG_H
#define CONFIG_H

/**
 * @file config.h
 * A catch-all file for configuring various bugfixes and other options
 */

// Endianness
#ifdef PORT
#define IS_BIG_ENDIAN 0
#else
#define IS_BIG_ENDIAN 1
#endif

// Sign-extend a bitfield value declared as u32 for MSVC bitfield packing
// compatibility but representing a signed value in ROM data.
// Uses XOR-subtract trick (no UB, works for any width 1-31).
#define BITFIELD_SEXT(x, bits) ((s32)(((u32)(x) ^ (1u << ((bits) - 1))) - (1u << ((bits) - 1))))
#define BITFIELD_SEXT8(x)  BITFIELD_SEXT(x, 8)
#define BITFIELD_SEXT10(x) BITFIELD_SEXT(x, 10)
#define BITFIELD_SEXT13(x) BITFIELD_SEXT(x, 13)
#define BITFIELD_SEXT16(x) BITFIELD_SEXT(x, 16)

// Screen Size Defines
#define GS_SCREEN_WIDTH_DEFAULT 320
#define GS_SCREEN_HEIGHT_DEFAULT 240

#endif /* CONFIG_H */
