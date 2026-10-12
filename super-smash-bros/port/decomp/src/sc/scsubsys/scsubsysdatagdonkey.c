#include <ft/fttypes.h>
#include <sc/scsubsys/scsubsys.h>
#include <reloc_data.h>

FTMotionDesc dFTGDonkeySubMotionDescs[] =
{
#ifdef PORT
	ll_800_FileID, 0x80000000, 0x00000000
#else
	&llFTDonkeyAnimEggLayFileID, 0x80000000, 0x00000000
#endif
};
s32 dFTGDonkeySubMotionDescsCount = sizeof(dFTGDonkeySubMotionDescs)/sizeof(FTMotionDesc); // 0x00000001
