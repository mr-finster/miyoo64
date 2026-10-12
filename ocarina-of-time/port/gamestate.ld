/*
 * Save-state include list (port/kit/platform/savestate.c, HH_SS_INCLUDE_LIST in
 * src/port/hh/hh_game.h).
 *
 * Collected by object path: the .data/.bss of every decomp object (src/audio, buffers,
 * code, elf_message, libc64, libu64, libultra, overlays), of the generated object/scene
 * and audio tables (gen/), and the port variables tagged HH_GS_DATA / HH_GS_BSS
 * (.data.hhgs / .bss.hhgs). A -T script's input rules are matched before the default
 * script's, so these sections land here rather than in .data/.bss. A save state is
 * exactly the two ranges between the symbols; everything else -- the port layer, SDL,
 * libc and the linker's copies of libc variables (.dynbss) -- lies outside them.
 */
SECTIONS
{
  .game_data : ALIGN(64) {
    __gamedata_start = .;
    */src/audio/*(.data .data.*)
    */src/buffers/*(.data .data.*)
    */src/code/*(.data .data.*)
    */src/elf_message/*(.data .data.*)
    */src/libc64/*(.data .data.*)
    */src/libu64/*(.data .data.*)
    */src/libultra/*(.data .data.*)
    */src/overlays/*(.data .data.*)
    */gen/oot_psp_asset_tables.o(.data .data.*)
    */gen/oot_psp_audio_tables.o(.data .data.*)
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
    */src/buffers/*(.bss .bss.*)
    */src/code/*(.bss .bss.*)
    */src/elf_message/*(.bss .bss.*)
    */src/libc64/*(.bss .bss.*)
    */src/libu64/*(.bss .bss.*)
    */src/libultra/*(.bss .bss.*)
    */src/overlays/*(.bss .bss.*)
    */gen/oot_psp_asset_tables.o(.bss .bss.*)
    */gen/oot_psp_audio_tables.o(.bss .bss.*)
    *(.bss.hhgs)
    . = ALIGN(64);
    __gamebss_end = .;
  }
}
INSERT AFTER .bss;
