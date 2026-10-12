#include <ft/fttypes.h>
#include <sc/scsubsys/scsubsys.h>
#include <reloc_data.h>

FTMotionDesc dFTNPurinSubMotionDescs[] =
{
#ifdef PORT
	ll_1492_FileID, 0x80000000, 0x00000000
#else
	&llFTPurinAnimEggLayFileID, 0x80000000, 0x00000000
#endif
};
s32 dFTNPurinSubMotionDescsCount = sizeof(dFTNPurinSubMotionDescs)/sizeof(FTMotionDesc); // 0x00000001
