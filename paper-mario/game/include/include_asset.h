#ifndef _H_INCLUDE_ASSET
#define _H_INCLUDE_ASSET
/* The native port's copy of the decomp's include/include_asset.h (M2): the same .incbin of the decomp's built asset
 * files (the compiler runs in refs/papermario, so the relative paths hold), with directives both native assemblers
 * take. ".align 3" is 8 bytes on MIPS but 3 bytes on i386 ELF, so .balign 8; ARM gas reads '@' as a comment, so the
 * symbol type is %object. The data stays in N64 byte order (textures and palettes are bytes and big-endian shorts,
 * as the renderer reads them). They go to .assetdata, the region the device rebuilds from the player's ROM at first
 * boot (tools/assets, recipes); the desktop proxy links the bytes in. */
#define ASTRINGIFY_(x) #x
#define ASTRINGIFY(x) ASTRINGIFY_(x)

#define PM_INCBIN_(TYPE, SYMBOLNAME, PATH) \
    extern TYPE SYMBOLNAME[]; \
    __asm__( \
        ".pushsection .assetdata,\"aw\"\n" \
        ".globl " #SYMBOLNAME "\n" \
        ".balign 8\n" \
        ".type " #SYMBOLNAME ", %object\n" \
        #SYMBOLNAME ":\n" \
        ".incbin \"" PATH "\"\n" \
        ".popsection\n" \
    )

#define _INCLUDE_IMG(FILENAME, SYMBOLNAME) PM_INCBIN_(unsigned char, SYMBOLNAME, "ver/" ASTRINGIFY(VERSION) "/build/" FILENAME ".bin")
#define INCLUDE_IMG(FILENAME, SYMBOLNAME) _INCLUDE_IMG(FILENAME, SYMBOLNAME)
#define INCLUDE_PAL(FILENAME, SYMBOLNAME) PM_INCBIN_(unsigned short, SYMBOLNAME, "ver/" ASTRINGIFY(VERSION) "/build/" FILENAME ".bin")
#define INCLUDE_RAW(FILENAME, SYMBOLNAME) \
    PM_INCBIN_(unsigned char, SYMBOLNAME, "ver/" ASTRINGIFY(VERSION) "/build/assets/" ASTRINGIFY(VERSION) "/" FILENAME)

#endif // _H_INCLUDE_ASSET
