/* The game's sine/cosine and arctangent tables (include/trig_tables.inc.c, data
 * from the ROM, restored untracked with the decomp's data) compiled as an ASSET
 * object: on the handheld their bytes live in the asset region, rebuilt from the
 * player's ROM at first boot, so the executable carries none of them.
 * math_util.c includes the file itself on every other target. */
#include <PR/ultratypes.h>

#include "trig_tables.inc.c"
