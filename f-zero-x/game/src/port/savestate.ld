/* Save states (src/port/sdl/savestate.c). The port's own objects -- renderer, SDL,
 * audio ring, logs, ROM mapping, menu, everything in src/port outside src/port/ultra --
 * have their .data / .bss renamed to .port_data / .port_bss at build time
 * (Makefile.handheld) and live here, outside the two ranges a state holds:
 *   game: .data (the game's objects, the OS layer in src/port/ultra, anything tagged
 *         SAVESTATE_INCLUDE) and the overlays' .data.ovl.*, then .bss;
 *   ram:  .n64_rdram (the N64's 4 MB) and the overlays' .bss.ovl.*.
 * So no heap, FILE* or SDL pointer is ever part of a state (MK64 v25). Gate 5 in
 * tools/build_miyoo.sh (tools/check_savestate.py) checks what lands in the ranges. */
SECTIONS {
  .port_data : ALIGN(16) { __port_data_start = .; *(.port_data) __port_data_end = .; }
} INSERT BEFORE .data;
SECTIONS {
  .port_bss (NOLOAD) : ALIGN(16) { __port_bss_start = .; *(.port_bss) __port_bss_end = .; }
} INSERT AFTER .bss;
__ss_game_start = ADDR(.data);
__ss_game_end = ADDR(.bss) + SIZEOF(.bss);
__ss_ram_start = ADDR(.n64_rdram);
__ss_ram_end = _end;
