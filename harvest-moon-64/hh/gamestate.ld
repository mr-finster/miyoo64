/*
 * Save-state include list (kit/savestate.c, HH_SS_INCLUDE_LIST in kit/hh_game.h), as the Majora's Mask and Ocarina
 * of Time ports' gamestate.ld: the .data/.bss of every game object (the decomp's src/ and lib/, and the port's
 * game-side hh/game). A save state is exactly these two ranges plus the N64 RAM at 0x80000000 (hh_host.c,
 * hh_game_extra_range). The host side (the renderer, SDL, the kit, the mixer, the audio ring, the pack) lies outside
 * by construction, so a load never overwrites a live host pointer.
 */
SECTIONS
{
  .game_data : ALIGN(64) {
    __gamedata_start = .;
    build/hh/src/*(.data .data.*)
    build/hh/lib/*(.data .data.*)
    build/hh/hh/game/*(.data .data.*)
    . = ALIGN(64);
    __gamedata_end = .;
  }
}
INSERT AFTER .data;

SECTIONS
{
  .game_bss (NOLOAD) : ALIGN(64) {
    __gamebss_start = .;
    build/hh/src/*(.bss .bss.* COMMON)
    build/hh/lib/*(.bss .bss.* COMMON)
    build/hh/hh/game/*(.bss .bss.* COMMON)
    . = ALIGN(64);
    __gamebss_end = .;
  }
}
INSERT AFTER .bss;
