/* romtables.ld: the read-only bytes hh_romtables.c fills from the player's ROM, in pages of their own.
 *
 * The tables romtables.py scrubs are in .data (written as any variable) except the strings they point
 * at: string literals of the game code, in .rodata.str*. Those go here, in an output section that
 * starts and ends on a 4 KiB page, after the code (.fini, etext) and before .rodata, so the fill's
 * mprotect (read+write, then read-only) touches no code and no other data (v1.0.1's first build had
 * .rodata's first strings on .text's last page). romtables.py refuses a build with a filled byte
 * outside .romtables and the writable sections; hh_romtables.c checks the pages at every start. */
SECTIONS
{
  .romtables : ALIGN(4096)
  {
    __romtables_start = .;
    */src/*.o(.rodata.str*)
    . = ALIGN(4096);
    __romtables_end = .;
  }
}
INSERT BEFORE .rodata;
