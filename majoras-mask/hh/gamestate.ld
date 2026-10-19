/*
 * Save-state include list (hh/kit/platform/savestate.c, HH_SS_INCLUDE_LIST in hh/src/hh_game.h), as the
 * Ocarina of Time port's port/gamestate.ld: the .data/.bss of every decomp object and the port variables
 * tagged HH_GS_DATA / HH_GS_BSS. A save state is exactly the two ranges between the symbols.
 */
SECTIONS
{
  .game_data : ALIGN(64) {
    __gamedata_start = .;
    */src/audio/*(.data .data.*)
    */src/boot/*(.data .data.*)
    */src/buffers/*(.data .data.*)
    */src/code/*(.data .data.*)
    */src/libultra/*(.data .data.*)
    */src/overlays/*(.data .data.*)
    */hh/libultra/*(.data .data.*)
    *(.data.hhgs)
    . = ALIGN(64);
    __gamedata_end = .;
  }
}
INSERT AFTER .data;

SECTIONS
{
  .game_bss (NOLOAD) : ALIGN(64) {
    __gamebss_start = .;
    */src/audio/*(.bss .bss.*)
    */src/boot/*(.bss .bss.*)
    */src/buffers/*(.bss .bss.*)
    */src/code/*(.bss .bss.*)
    */src/libultra/*(.bss .bss.*)
    */src/overlays/*(.bss .bss.*)
    */hh/libultra/*(.bss .bss.*)
    *(.bss.hhgs)
    . = ALIGN(64);
    __gamebss_end = .;
  }
}
INSERT AFTER .bss;
