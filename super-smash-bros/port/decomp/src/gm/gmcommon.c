#include <if/interface.h>
#include <reloc_data.h>

// // // // // // // // // // // //
//                               //
//       INITIALIZED DATA        //
//                               //
// // // // // // // // // // // //

// 0x80116BD0 - These will be linker variables in the future
u32 dGMCommonFileIDs[/* */] = 
{ 
#ifdef PORT
    llIFCommonPlayerFileID,
    llIFCommonGameStatusFileID,
    llIFCommonPlayerDamageFileID,
    llIFCommonTimerFileID,
    llIFCommonDigitsFileID,
    llIFCommonBattlePauseFileID,
    llIFCommonPlayerTagsFileID,
    llIFCommonAnnounceCommonFileID
#else
    &llIFCommonPlayerFileID,
    &llIFCommonGameStatusFileID,
    &llIFCommonPlayerDamageFileID,
    &llIFCommonTimerFileID,
    &llIFCommonDigitsFileID,
    &llIFCommonBattlePauseFileID,
    &llIFCommonPlayerTagsFileID,
    &llIFCommonAnnounceCommonFileID
#endif
};

// // // // // // // // // // // //
//                               //
//   GLOBAL / STATIC VARIABLES   //
//                               //
// // // // // // // // // // // //

// 0x80130D40
void *gGMCommonFiles[ARRAY_COUNT(dGMCommonFileIDs)];
