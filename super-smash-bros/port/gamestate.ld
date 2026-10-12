/*
 * Save-state include list (port/kit/platform/savestate.c, HH_SS_INCLUDE_LIST in hh/hh_game.h).
 *
 * Collected by object path: the .data/.bss of every decomp object (the game under
 * decomp/src/<dir>, its libultra audio and gu), minus BattleShip's netplay objects (a socket
 * fd and 2 MB of replay buffers the handheld build never uses); the coroutines (hh/coroutine.o:
 * the game's threads, their stacks and contexts); the BattleShip bridge (hh/bs: the reloc
 * tables that point into the scene arena, the audio bridge and mixer, the fighter registry, the
 * N64 OS stubs); and the port variables tagged HH_GS_DATA / HH_GS_BSS (.data.hhgs / .bss.hhgs:
 * the SRAM mirror, main.c's load stall). A -T script's input rules are matched before the
 * default script's, so these sections land here rather than in .data/.bss. A save state is
 * exactly the two ranges between the symbols; everything else -- the rest of the port layer
 * (hh/n64/os.o's clock base, the pacer, input, the audio ring), the renderer, SDL and libc --
 * lies outside them.
 */
SECTIONS
{
  .game_data : ALIGN(64) {
    __gamedata_start = .;
    EXCLUDE_FILE(*/sys/netpeer.o */sys/netsync.o */sys/netinput.o */sys/netreplay.o) */decomp/src/*(.data .data.*)
    */hh/coroutine.o(.data .data.*)
    */hh/bs/*(.data .data.*)
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
    EXCLUDE_FILE(*/sys/netpeer.o */sys/netsync.o */sys/netinput.o */sys/netreplay.o) */decomp/src/*(.bss .bss.*)
    */hh/coroutine.o(.bss .bss.*)
    */hh/bs/*(.bss .bss.*)
    *(.bss.hhgs)
    . = ALIGN(64);
    __gamebss_end = .;
  }
}
INSERT AFTER .bss;
