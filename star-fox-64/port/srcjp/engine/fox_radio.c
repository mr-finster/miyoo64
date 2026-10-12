#include "common.h"

extern u16** gRadioMsgList;
extern s32 gRadioMsgListIndex;
extern s32 gRadioPrintPosX;
extern s32 gRadioPrintPosY;
extern f32 gRadioTextBoxPosX;
extern f32 gRadioTextBoxPosY;
extern f32 gRadioTextBoxScaleX;
extern f32 gRadioPortraitPosX;
extern f32 gRadioPortraitPosY;
extern f32 gRadioTextBoxScaleY;
extern s32 gMsgCharIsPrinting;
extern s32 gRadioMsgCharIndex;
extern u16* gRadioMsg;

bool Message_IsPrintingChar(u16* msgPtr, s32 charPos);
void RCP_SetupDL_36(void);

void Radio_Hide(void) {
    RCP_SetupDL_36();
    if (gRadioTextBoxScaleY == 1.3f) {
        gMsgCharIsPrinting = Message_IsPrintingChar(gRadioMsg, gRadioMsgCharIndex);
    }
}

#pragma GLOBAL_ASM("asm/jp/rev0/nonmatchings/engine/fox_radio/Radio_CheckMesgPriority.s")

#pragma GLOBAL_ASM("asm/jp/rev0/nonmatchings/engine/fox_radio/Radio_PlayMessage.s")

#pragma GLOBAL_ASM("asm/jp/rev0/nonmatchings/engine/fox_radio/Radio_Portrait_Draw.s")

#pragma GLOBAL_ASM("asm/jp/rev0/nonmatchings/engine/fox_radio/Radio_TextBox_Draw.s")

// Matched
// https://decomp.me/scratch/6ASda
#pragma GLOBAL_ASM("asm/jp/rev0/nonmatchings/engine/fox_radio/Radio_Draw.s")

#pragma GLOBAL_ASM("asm/jp/rev0/nonmatchings/engine/fox_radio/Radio_Draw_Alt.s")
