/**
 * The game's main memory pool (N64: end of BSS to 0x80400000, ~2.8 MB). Here a
 * static array, renamed into the save-state region with the game's own .bss
 * (Makefile GAMESTATE_O), at a fixed address in the non-PIE binary.
 */
#include <ultra64.h>
#include "memory.h"

__attribute__((aligned(64))) u8 gPortMainPool[PORT_MAIN_POOL_SIZE];
