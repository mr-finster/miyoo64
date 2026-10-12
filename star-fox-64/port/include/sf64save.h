#ifndef SF64_SAVE
#define SF64_SAVE

#include "PR/ultratypes.h"

#define RANKING_MAX (10) // Maximum number of entries in the ranking
#define ROUTE_MAX (7)    // Maximum number of planets in a route
#define ENTRY_MAX (3)    // Maximum number of letters for name entry

#define TEAMSTATUS_PEPPY (0x00FF0000)
#define TEAMSTATUS_SLIPPY (0x0000FF00)
#define TEAMSTATUS_FALCO (0x000000FF)
#define TEAMSTATUS_ALIVE (0x00FFFFFF)

typedef struct {
    /* bit 0 */ u8 unk_0 : 3; // unused
    /* bit 3 */ u8 expertMedal : 1;
    /* bit 4 */ u8 expertClear : 1;
    /* bit 5 */ u8 played : 1;
    /* bit 6 */ u8 normalMedal : 1;
    /* bit 7 */ u8 normalClear : 1;
} PlanetData; // size = 0x1

typedef struct PlanetStats {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    /* Port (little-endian GCC fills a bitfield from bit 0): this order puts every field in the
     * same byte and bits as IDO's big-endian layout does, which PLANET_STATS (gDefaultSave's
     * ranking table) writes as two raw bytes: byte 0 hitCount, byte 1 planetId << 4 |
     * hitCountOver256 << 3 | peppy << 2 | falco << 1 | slippy. In the N64 order the port read
     * the default table as hit counts +256, the wrong planets and teammates. */
    u16 hitCount : 8;
    u16 slippyAlive : 1;
    u16 falcoAlive : 1;
    u16 peppyAlive : 1;
    u16 hitCountOver256 : 1;
    u16 planetId : 4;
#else
    /* bit 0 */ u16 hitCount : 8;
    /* bit 8 */ u16 planetId : 4;
    /* bit C */ u16 hitCountOver256 : 1;
    /* bit D */ u16 peppyAlive : 1;
    /* bit E */ u16 falcoAlive : 1;
    /* bit F */ u16 slippyAlive : 1;
#endif
} PlanetStats; // size = 0x2

#define PLANET_STATS(hitCount, planetId, peppyAlive, falcoAlive, slippyAlive) \
    (hitCount > 255 ? hitCount - 256 : hitCount),                             \
        ((planetId << 4) | ((hitCount > 255 ? 1 : 0) << 3) | (peppyAlive << 2) | (falcoAlive << 1) | (slippyAlive))

typedef struct SaveData {
    /* 0x00 */ PlanetData planet[16];
    /* 0x10 */ char pad10[0x4];
    /* 0x14 */ u8 soundMode;
    /* 0x15 */ u8 musicVolume;
    /* 0x16 */ u8 voiceVolume;
    /* 0x17 */ u8 sfxVolume;
    /* 0x18 */ u8 rankNameEntry[RANKING_MAX][ENTRY_MAX]; // Name entries in the ranking
    /* 0x36 */ u8 rankingRoute[RANKING_MAX];             // Maximum number of planets played in the current record
    /* 0x40 */ u8 rankingLives[RANKING_MAX];             // Player lives left in the current record
    /* 0x4A */ u16 rankingMedal[RANKING_MAX];            // Medals obtained in the current record
    /* 0x5E */ PlanetStats stats[RANKING_MAX][ROUTE_MAX];
    /* 0xEA */ u8 unk_EA;
    /* 0xEB */ u8 textLanguage;  // EU Only text language selection
    /* 0xEC */ u8 voiceLanguage; // EU Only voice language selection
    /* 0xED */ char padEE[0x11];
} SaveData; // size = 0xFE

typedef struct {
    /* 0x00 */ union {
        u8 raw[sizeof(SaveData)];
        SaveData data;
    };
    /* 0xFE */ u16 checksum;
} Save; // size = 0x100

typedef struct {
    /* 0x000 */ Save save;
    /* 0x100 */ Save backup;
} SaveFile; // size = 0x200

s32 Save_Write(void);
s32 Save_Read(void);

void Save_ReadData(void);
void Save_WriteData(void);

s32 Save_WriteEeprom(SaveFile*);
s32 Save_ReadEeprom(SaveFile*);

#if defined(TARGET_HANDHELD)
s32 Save_PortReapply(const SaveFile* card); /* statefix A: a state load's card save over the state's copies */
void Save_PortTestWrite(void);              /* SF64_SAVEWRITE_AT: a map-style save write (tests) */
void Save_PortTestSoundPick(void);          /* SF64_SOUNDPICK_AT: a sound screen slider move (tests) */
#endif

extern SaveFile gSaveIOBuffer;
extern SaveFile sPrevSaveData;
extern Save gDefaultSave;
extern SaveFile gSaveFile;

#endif
