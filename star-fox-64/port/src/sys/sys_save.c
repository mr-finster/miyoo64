

#include "sys.h"

SaveFile gSaveIOBuffer;
SaveFile sPrevSaveData;

s32 Save_ReadBlock(s32 arg0, u8* arg1) {
    if (osEepromRead(&gSerialEventQueue, arg0, arg1)) {
        PRINTF("ＥＥＰＲＯＭ インターフェース回路反応なし (ＲＥＡＤ)\n");
        return -1;
    } else {
        PRINTF("EEPROM READ  %02X: %02X %02X %02X %02X %02X %02X %02X %02X\n");
        return 0;
    }
}

s32 Save_WriteBlock(s32 arg0, u8* arg1) {
    if (osEepromWrite(&gSerialEventQueue, arg0, arg1)) {
        PRINTF("ＥＥＰＲＯＭ インターフェース回路反応なし (ＷＲＩＴＥ)\n");
        return -1;
    } else {
#if !defined(TARGET_HANDHELD)
        Timer_Wait(MSEC_TO_CYCLES(15));
#endif
        PRINTF("EEPROM WRITE %02X: %02X %02X %02X %02X %02X %02X %02X %02X\n");
        return 0;
    }
}

s32 Save_WriteEeprom(SaveFile* arg0) {
    s32 var_a2;
    s32 i;
    s32 j;

    if (osEepromProbe(&gSerialEventQueue) != 1) {
        PRINTF("ＥＥＰＲＯＭ が ありません\n");
        return -1;
    }

    for (i = 0; i < EEPROM_MAXBLOCKS; i++) {
        var_a2 = 0;
        for (j = 0; j < EEPROM_BLOCK_SIZE; j++) {
            if (((char*) &sPrevSaveData)[EEPROM_BLOCK_SIZE * i + j] != ((char*) arg0)[EEPROM_BLOCK_SIZE * i + j]) {
                ((char*) &sPrevSaveData)[EEPROM_BLOCK_SIZE * i + j] = ((char*) arg0)[EEPROM_BLOCK_SIZE * i + j];
                var_a2 = 1;
            }
        }
        if ((var_a2 == 1) && Save_WriteBlock(i, &((char*) arg0)[EEPROM_BLOCK_SIZE * i])) {
            return -1;
        }
    }
#if defined(TARGET_HANDHELD)
    {
        /* Port: the blocks went to the EEPROM image in memory (ultra_shim.c); the save reaches
         * eeprom.bin here, in one atomic file write. The N64's 15 ms write cycle per block
         * (Timer_Wait above) is not waited for: no chip to wait on. */
        extern s32 port_eeprom_flush(void);
        return port_eeprom_flush();
    }
#endif
    return 0;
}

s32 Save_ReadEeprom(SaveFile* arg0) {
    s32 i;

    if (osEepromProbe(&gSerialEventQueue) != 1) {
        PRINTF("ＥＥＰＲＯＭ が ありません\n");
        return -1;
    }
    for (i = 0; i < EEPROM_MAXBLOCKS; i++) {
        if (Save_ReadBlock(i, &((char*) arg0)[EEPROM_BLOCK_SIZE * i]) != 0) {
            return -1;
        }
    }
    sPrevSaveData = *arg0;
    return 0;
}
