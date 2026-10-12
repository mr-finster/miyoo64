#include "global.h"

SaveFile gSaveFile;

// clang-format off

Save gDefaultSave = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // PlanetData

    0x01, 0x02, 0x03, 0x04, // pad10

    SOUNDMODE_STEREO,
    99, // musicVolume
    99, // voiceVolume
    99, // sfxVolume
    
    // rankNameEntry
    'F', 'O', 'X', 
    'P', 'E', 'P', 
    'S', 'L', 'P', 
    'F', 'A', 'L', 
    'W', 'L', 'F', 
    'L', 'O', 'N', 
    'P', 'I', 'G', 
    'A', 'N', 'R', 
    'B', 'I', 'L', 
    'K', 'A', 'T', 

    // rankingRoute
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,

    // rankingLives
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

    // rankingMedal
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

    // stats
    PLANET_STATS(100, PLANET_CORNERIA, false, false, false), 
    PLANET_STATS(9, PLANET_SECTOR_Y, true, true, true), 
    PLANET_STATS(9, PLANET_AQUAS, true, true, true), 
    PLANET_STATS(9, PLANET_ZONESS, true, true, true), 
    PLANET_STATS(9, PLANET_SECTOR_Z, true, true, true), 
    PLANET_STATS(9, PLANET_AREA_6, true, true, true), 
    PLANET_STATS(9, SAVE_SLOT_VENOM_2, true, true, true), 

    PLANET_STATS(90, PLANET_CORNERIA, false, false, false), 
    PLANET_STATS(9, PLANET_SECTOR_Y, true, false, false), 
    PLANET_STATS(9, PLANET_KATINA, true, false, false), 
    PLANET_STATS(9, PLANET_VENOM, true, false, false), 
    PLANET_STATS(9, PLANET_MACBETH, true, false, false), 
    PLANET_STATS(9, PLANET_AREA_6, true, false, false), 
    PLANET_STATS(9, SAVE_SLOT_VENOM_2, true, false, false), 

    PLANET_STATS(80, PLANET_CORNERIA, false, false, false),
    PLANET_STATS(9, PLANET_METEO, false, true, false),
    PLANET_STATS(9, PLANET_FORTUNA, false, true, false),
    PLANET_STATS(9, PLANET_SECTOR_X, false, true, false),
    PLANET_STATS(9, PLANET_TITANIA, false, true, false),
    PLANET_STATS(9, PLANET_BOLSE, false, true, false),
    PLANET_STATS(9, SAVE_SLOT_VENOM_1, false, true, false),

    PLANET_STATS(70, PLANET_CORNERIA, false, false, false), 
    PLANET_STATS(9, PLANET_METEO, false, false, true),
    PLANET_STATS(9, PLANET_FORTUNA, false, false, true),
    PLANET_STATS(9, PLANET_SECTOR_X, false, false, true),
    PLANET_STATS(9, PLANET_TITANIA, false, false, true),
    PLANET_STATS(9, PLANET_BOLSE, false, false, true),
    PLANET_STATS(9, SAVE_SLOT_VENOM_1, false, false, true),

    PLANET_STATS(60, PLANET_CORNERIA, false, false, false), 
    PLANET_STATS(9, PLANET_METEO, true, false, true),
    PLANET_STATS(9, PLANET_FORTUNA, true, false, true),
    PLANET_STATS(9, PLANET_SECTOR_X, true, false, true),
    PLANET_STATS(9, PLANET_TITANIA, true, false, true),
    PLANET_STATS(9, PLANET_BOLSE, true, false, true),
    PLANET_STATS(9, SAVE_SLOT_VENOM_1, true, false, true),

    PLANET_STATS(50, PLANET_CORNERIA, false, false, false), 
    PLANET_STATS(4, PLANET_METEO, false, true, true),
    PLANET_STATS(9, PLANET_FORTUNA, false, true, true),
    PLANET_STATS(9, PLANET_SECTOR_X, false, true, true),
    PLANET_STATS(9, PLANET_TITANIA, false, true, true),
    PLANET_STATS(9, PLANET_BOLSE, false, true, true),
    PLANET_STATS(9, SAVE_SLOT_VENOM_1, false, true, true),

    PLANET_STATS(40, PLANET_CORNERIA, false, false, false), 
    PLANET_STATS(1, PLANET_METEO, true, true, false),
    PLANET_STATS(2, PLANET_FORTUNA, true, true, false),
    PLANET_STATS(9, PLANET_SECTOR_X, true, true, false),
    PLANET_STATS(9, PLANET_TITANIA, true, true, false),
    PLANET_STATS(9, PLANET_BOLSE, true, true, false),
    PLANET_STATS(9, SAVE_SLOT_VENOM_1, true, true, false),

    PLANET_STATS(30, PLANET_CORNERIA, false, false, false),
    PLANET_STATS(1, PLANET_METEO, false, true, false),
    PLANET_STATS(1, PLANET_FORTUNA, true, false, false),
    PLANET_STATS(1, PLANET_SECTOR_X, true, true, true),
    PLANET_STATS(8, PLANET_TITANIA, true, false, true),
    PLANET_STATS(9, PLANET_BOLSE, false, true, true),
    PLANET_STATS(9, SAVE_SLOT_VENOM_1, true, true, false),

    PLANET_STATS(20, PLANET_CORNERIA, false, false, false),
    PLANET_STATS(1, PLANET_METEO, false, true, true),
    PLANET_STATS(1, PLANET_FORTUNA, true, false, true),
    PLANET_STATS(1, PLANET_SECTOR_X, true, true, true),
    PLANET_STATS(1, PLANET_TITANIA, true, false, false),
    PLANET_STATS(6, PLANET_BOLSE, false, true, false),
    PLANET_STATS(9, SAVE_SLOT_VENOM_1, false, false, true),

    PLANET_STATS(10, PLANET_CORNERIA, false, false, false),
    PLANET_STATS(1, PLANET_METEO, true, true, true),
    PLANET_STATS(1, PLANET_FORTUNA, true, false, false),
    PLANET_STATS(1, PLANET_SECTOR_X, true, false, false),
    PLANET_STATS(1, PLANET_TITANIA, false, true, false),
    PLANET_STATS(1, PLANET_BOLSE, false, false, true),
    PLANET_STATS(4, SAVE_SLOT_VENOM_1, true, false, true),
    
    // unk_EA
    0xFF,

    // padding
    0x09, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
};
// clang-format on

u16 Save_Checksum(Save* save) {
    u16 checksum;
    s32 i;

    for (i = 0, checksum = 0; i < sizeof(SaveData); i++) {
        checksum ^= save->raw[i];
        checksum <<= 1;
        checksum = (checksum & 0xFE) | ((checksum >> 8) & 1);
    }

    checksum = (checksum & 0xFF) | 0x9500;

    return checksum;
}

s32 Save_Write(void) {
    OSMesg msg;

    gSaveFile.save.checksum = Save_Checksum(&gSaveFile.save);
    gSaveFile.backup = gSaveFile.save;
    gSaveIOBuffer = gSaveFile;
#if defined(TARGET_HANDHELD)
    /* Port: no serial thread runs (sf64_main.c runs the threads' steps in turn on one thread),
     * so nothing would read the request and the wait below would return at once with msg unset:
     * no save ever reached eeprom.bin. Do the serial thread's job here; it posts the result to
     * gSaveMesgQueue as on the N64. */
    Save_WriteData();
#else
    osSendMesg(&gSerialThreadMesgQueue, (OSMesg) SI_WRITE_SAVE, OS_MESG_NOBLOCK);
#endif
    MQ_WAIT_FOR_MESG(&gSaveMesgQueue, &msg);
    if (msg != (OSMesg) SI_SAVE_SUCCESS) {
        return -1;
    }
    return 0;
}

s32 Save_Read(void) {
    OSMesg msg;
    s32 i;

#if defined(TARGET_HANDHELD)
    Save_ReadData(); /* the serial thread's job, as in Save_Write */
#else
    osSendMesg(&gSerialThreadMesgQueue, (OSMesg) SI_READ_SAVE, OS_MESG_NOBLOCK);
#endif
    MQ_WAIT_FOR_MESG(&gSaveMesgQueue, &msg);
    if ((s32) msg != SI_SAVE_SUCCESS) {
        return -1;
    }

    gSaveFile = gSaveIOBuffer;

    if (gSaveFile.save.checksum == Save_Checksum(&gSaveFile.save)) {
        PRINTF("ＥＥＰＲＯＭ ＲＯＭ［０］ 正常\n"); // EEPROM ROM[0] Normal
        return 0;
    }
#ifdef AVOID_UB
    for (i = 0; i < sizeof(SaveData); i++) {
#else
    for (i = 0; i <= sizeof(SaveData); i++) { // should be <, but gets overwritten immediately.
#endif
        gSaveFile.save.raw[i] = gSaveFile.backup.raw[i];
    }
    gSaveFile.save.checksum = gSaveFile.backup.checksum;

    if (gSaveFile.save.checksum == Save_Checksum(&gSaveFile.save)) {
        PRINTF("ＥＥＰＲＯＭ ＲＯＭ［1］ 正常\n"); // EEPROM ROM[1] Normal
        return 0;
    } else {
        PRINTF("ＥＥＰＲＯＭ ＲＯＭ［０］ ＆ ＲＯＭ［1］ 異常\n"); // EEPROM ROM[0] & ROM[1] Error
        return -1;
    }
}

#if defined(TARGET_HANDHELD)
/* statefix A (handheld-kit k1.8; ultra_shim.c hh_game_save_reapply calls this after a load that happened, mixer
 * parked, mix lock held, when eeprom.bin differs from the state's EEPROM image). card: eeprom.bin, the whole
 * 512-byte image (a SaveFile's own bytes, as Save_ReadEeprom reads them). The state's copies of the save become
 * the card's: gSaveFile as Save_Read makes it (the first copy if its checksum holds, else the second), and
 * sPrevSaveData and gSaveIOBuffer the card's raw image (Save_WriteEeprom writes only the blocks that differ from
 * sPrevSaveData, so it must describe the image the shim holds, which the caller sets to the card's too). Every
 * progress write in Star Fox (a planet's flags and medals, the ranking, Venom, the data clear) sets gSaveFile and
 * calls Save_Write in the same frame, so a state never holds progress the card lacks: the card wins, no merge.
 * Only the sound screen leaves an edit unwritten (the sound mode and the three volumes change in gSaveFile as the
 * player moves; B writes them): a setting the state's gSaveFile holds unwritten (it differs from the state's last
 * written save) is the player's pick on screen and stays; the others are the card's.
 * Values the game takes from the save once (GSTATE_INIT, the sound screen's setup) follow a card setting that
 * differs from the state's: the sound mode now (the sound screen's own command), a volume in gVolumeSettings and
 * the audio's sVolumeSettings, which the audio applies at its next reset (a scene change): Audio_SetVolume now
 * would bring back a BGM the game has faded. Screens that copied the save at their setup (the title's ranking,
 * the options' expert unlock, the sound screen's sliders) show it again when next entered.
 * Returns -1 (nothing changed) when neither copy on the card passes the game's checksum; else 0, | 1 when a sound
 * setting changed, | 2 when an unwritten pick was kept. */
extern u8 sVolumeSettings[3];
s32 Save_PortReapply(const SaveFile* card) {
    static const u8 kSoundMode[OPTIONSOUND_MAX] = { SOUNDMODE_STEREO, SOUNDMODE_MONO, SOUNDMODE_HEADSET };
    const SaveFile img = *card;
    const Save state = gSaveFile.save;
    Save fromCard;
    Save prev;
    u8* const set[4] = { &gSaveFile.save.data.soundMode, &gSaveFile.save.data.musicVolume,
                         &gSaveFile.save.data.voiceVolume, &gSaveFile.save.data.sfxVolume };
    const u8 stateSet[4] = { state.data.soundMode, state.data.musicVolume, state.data.voiceVolume, state.data.sfxVolume };
    u8 prevSet[4];
    s32 ret = 0;
    s32 i;

    fromCard = img.save;
    if (fromCard.checksum != Save_Checksum(&fromCard)) {
        fromCard = img.backup;
        if (fromCard.checksum != Save_Checksum(&fromCard)) {
            return -1;
        }
    }
    /* The state's save as the game last read or wrote it: a setting of the state's gSaveFile beyond it is unwritten. */
    prev = sPrevSaveData.save;
    if (prev.checksum != Save_Checksum(&prev)) {
        prev = sPrevSaveData.backup;
    }
    prevSet[0] = prev.data.soundMode;
    prevSet[1] = prev.data.musicVolume;
    prevSet[2] = prev.data.voiceVolume;
    prevSet[3] = prev.data.sfxVolume;

    gSaveFile.save = fromCard;
    gSaveFile.backup = img.backup; /* as Save_Read leaves it; Save_Write copies the first over it */
    for (i = 0; i < 4; i++) {
        if (stateSet[i] != prevSet[i]) { /* the sound screen's unwritten pick */
            *set[i] = stateSet[i];
            ret |= 2;
            continue;
        }
        if (*set[i] == prevSet[i]) {
            continue;
        }
        ret |= 1;
        if (i == 0) {
            u8 mode = *set[0];
            if (mode >= OPTIONSOUND_MAX) {
                mode = OPTIONSOUND_STEREO; /* GSTATE_INIT's rule */
            }
            if (mode != gOptionSoundMode) {
                gOptionSoundMode = mode;
                SEQCMD_SET_SOUND_MODE(kSoundMode[mode]);
            }
        } else {
            /* musicVolume, voiceVolume, sfxVolume = AUDIO_TYPE_MUSIC, _VOICE, _SFX; GSTATE_INIT's clamp */
            gVolumeSettings[i - 1] = *set[i] > 99 ? 99 : *set[i];
            sVolumeSettings[i - 1] = gVolumeSettings[i - 1];
        }
    }
    sPrevSaveData = img;
    gSaveIOBuffer = img;
    return ret;
}

/* SF64_SAVEWRITE_AT (sdl/main.c, statefix tests): the game's own save write when a level starts from the map
 * (Map_PlanetSaveSlot_Setup's SAVETYPE_PLAYED, then Save_Write), here for Katina: a headless run shows what the
 * game's next write after a load puts on the card. */
void Save_PortTestWrite(void) {
    gSaveFile.save.data.planet[PLANET_KATINA].played = 1;
    Save_Write();
}

/* SF64_SOUNDPICK_AT (sdl/main.c, statefix tests): what the sound screen does when the music slider moves
 * (Option_Sound_SetVolumeLevels: music volume 10 in gSaveFile and the audio, not written until B). */
void Save_PortTestSoundPick(void) {
    gVolumeSettings[AUDIO_TYPE_MUSIC] = 10;
    gSaveFile.save.data.musicVolume = 10;
    Audio_SetVolume(AUDIO_TYPE_MUSIC, 10);
}
#endif
