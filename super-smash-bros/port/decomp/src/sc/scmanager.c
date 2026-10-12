#ifdef PORT
#include <port_log.h>
#ifdef PORT
extern char *getenv(const char *name);
extern int atoi(const char *s);
extern void port_coroutine_yield(void);
#endif
#endif
#include <ft/fighter.h>
#include <wp/weapon.h>
#include <it/item.h>
#include <mv/movie.h>
#include <mn/menu.h>
#include <sc/scene.h>
#include <db/debug.h>
#include <sys/debug.h>
#include <sys/dma.h>
#include <sys/taskman.h>
#include <sys/audio.h>
#include <sys/video.h>
#include <sys/controller.h>
#include <sys/netpeer.h>
#include <sys/netreplay.h>
#ifdef PORT
#include <gr/ground.h>
#include <sys/objman.h>
#endif

extern void mnVSModeStartScene();

// // // // // // // // // // // //
//                               //
//   GLOBAL / STATIC VARIABLES   //
//                               //
// // // // // // // // // // // //

// 0x800A44D0
u8 sSCManagerPad0x800A44D0[16];

// 0x800A44E0
LBBackupData gSCManagerBackupData;

// 0x800A4AD0
SCCommonData gSCManagerSceneData;

// 0x800A4B18
SCBattleState gSCManager1PGameBattleState;

// 0x800A4D08
SCBattleState gSCManagerTransferBattleState;

// 0x800A4EF8
SCBattleState gSCManagerVSBattleState;

// 0x800A50E8
SCBattleState *gSCManagerBattleState;

// 0x800A50EC
u32 gSCManagerCIC;

// 0x800A50F0
s32 gSCManagerUnkown0x800A50F0;

// 0x800A50F8
FTFileSize gSCManagerFighterFileSizes[nFTKindEnumCount];

// 0x800A523C
s32 sSCManagerUnk0x800A523C;

// // // // // // // // // // // //
//                               //
//       INITIALIZED DATA        //
//                               //
// // // // // // // // // // // //

// 0x800A3070 (JP: 0x800A1090)
SYOverlay dSCManagerOverlays[/* */] =
{
	SCMANAGER_OVERLAY_DEFINE(0),
	SCMANAGER_OVERLAY_DEFINE(1),
	SCMANAGER_OVERLAY_DEFINE(2),
	SCMANAGER_OVERLAY_DEFINE(3),
	SCMANAGER_OVERLAY_DEFINE(4),
	SCMANAGER_OVERLAY_DEFINE(5),
	SCMANAGER_OVERLAY_DEFINE(6),
	SCMANAGER_OVERLAY_DEFINE(7),
	SCMANAGER_OVERLAY_DEFINE(8),
	SCMANAGER_OVERLAY_DEFINE(9),
	SCMANAGER_OVERLAY_DEFINE(10),
	SCMANAGER_OVERLAY_DEFINE(11),
	SCMANAGER_OVERLAY_DEFINE(12),
	SCMANAGER_OVERLAY_DEFINE(13),
	SCMANAGER_OVERLAY_DEFINE(14),
	SCMANAGER_OVERLAY_DEFINE(15),
	SCMANAGER_OVERLAY_DEFINE(16),
	SCMANAGER_OVERLAY_DEFINE(17),
	SCMANAGER_OVERLAY_DEFINE(18),
	SCMANAGER_OVERLAY_DEFINE(19),
	SCMANAGER_OVERLAY_DEFINE(20),
	SCMANAGER_OVERLAY_DEFINE(21),
	SCMANAGER_OVERLAY_DEFINE(22),
	SCMANAGER_OVERLAY_DEFINE(23),
	SCMANAGER_OVERLAY_DEFINE(24),
	SCMANAGER_OVERLAY_DEFINE(25),
	SCMANAGER_OVERLAY_DEFINE(26),
	SCMANAGER_OVERLAY_DEFINE(27),
	SCMANAGER_OVERLAY_DEFINE(28),
	SCMANAGER_OVERLAY_DEFINE(29),
	SCMANAGER_OVERLAY_DEFINE(30),
	SCMANAGER_OVERLAY_DEFINE(31),
	SCMANAGER_OVERLAY_DEFINE(32),
	SCMANAGER_OVERLAY_DEFINE(33),
	SCMANAGER_OVERLAY_DEFINE(34),
	SCMANAGER_OVERLAY_DEFINE(35),
	SCMANAGER_OVERLAY_DEFINE(36),
	SCMANAGER_OVERLAY_DEFINE(37),
	SCMANAGER_OVERLAY_DEFINE(38),
	SCMANAGER_OVERLAY_DEFINE(39),
	SCMANAGER_OVERLAY_DEFINE(40),
	SCMANAGER_OVERLAY_DEFINE(41),
	SCMANAGER_OVERLAY_DEFINE(42),
	SCMANAGER_OVERLAY_DEFINE(43),
	SCMANAGER_OVERLAY_DEFINE(44),
	SCMANAGER_OVERLAY_DEFINE(45),
	SCMANAGER_OVERLAY_DEFINE(46),
	SCMANAGER_OVERLAY_DEFINE(47),
	SCMANAGER_OVERLAY_DEFINE(48),
	SCMANAGER_OVERLAY_DEFINE(49),
	SCMANAGER_OVERLAY_DEFINE(50),
	SCMANAGER_OVERLAY_DEFINE(51),
	SCMANAGER_OVERLAY_DEFINE(52),
	SCMANAGER_OVERLAY_DEFINE(53),
	SCMANAGER_OVERLAY_DEFINE(54),
	SCMANAGER_OVERLAY_DEFINE(55),
	SCMANAGER_OVERLAY_DEFINE(56),
#if defined(REGION_US)
	SCMANAGER_OVERLAY_DEFINE(57),
	SCMANAGER_OVERLAY_DEFINE(58),
#endif
	SCMANAGER_OVERLAY_DEFINE(59),
	SCMANAGER_OVERLAY_DEFINE(60),
	SCMANAGER_OVERLAY_DEFINE(61),
	SCMANAGER_OVERLAY_DEFINE(62),
	SCMANAGER_OVERLAY_DEFINE(63),
	SCMANAGER_OVERLAY_DEFINE(64)
};

// 0x800A3994
LBBackupData dSCManagerDefaultBackupData =
{ 
	// VS Records
	{
		// Mario VS Records
		{
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// KO count on each character
			0,										// Time used
			0,										// Damage dealt
			0,										// Damage taken
			0,										// ???
			0,										// Self-destructs
			0,										// Games played with this character present
			0,										// Avg. player count of this character in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// Avg. player count with other characters in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }	// Number of times played against other characters
		},

		// Fox VS Records
		{
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// KO count on each character
			0,										// Time used
			0,										// Damage dealt
			0,										// Damage taken
			0,										// ???
			0,										// Self-destructs
			0,										// Games played with this character present
			0,										// Avg. player count of this character in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// Avg. player count with other characters in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }	// Number of times played against other characters
		},

		// Donkey Kong VS Records
		{
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// KO count on each character
			0,										// Time used
			0,										// Damage dealt
			0,										// Damage taken
			0,										// ???
			0,										// Self-destructs
			0,										// Games played with this character present
			0,										// Avg. player count of this character in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// Avg. player count with other characters in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }	// Number of times played against other characters
		},

		// Samus VS Records
		{
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// KO count on each character
			0,										// Time used
			0,										// Damage dealt
			0,										// Damage taken
			0,										// ???
			0,										// Self-destructs
			0,										// Games played with this character present
			0,										// Avg. player count of this character in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// Avg. player count with other characters in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }	// Number of times played against other characters
		},

		// Luigi VS Records
		{
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// KO count on each character
			0,										// Time used
			0,										// Damage dealt
			0,										// Damage taken
			0,										// ???
			0,										// Self-destructs
			0,										// Games played with this character present
			0,										// Avg. player count of this character in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// Avg. player count with other characters in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }	// Number of times played against other characters
		},

		// Link VS Records
		{
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// KO count on each character
			0,										// Time used
			0,										// Damage dealt
			0,										// Damage taken
			0,										// ???
			0,										// Self-destructs
			0,										// Games played with this character present
			0,										// Avg. player count of this character in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// Avg. player count with other characters in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }	// Number of times played against other characters
		},

		// Yoshi VS Records
		{
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// KO count on each character
			0,										// Time used
			0,										// Damage dealt
			0,										// Damage taken
			0,										// ???
			0,										// Self-destructs
			0,										// Games played with this character present
			0,										// Avg. player count of this character in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// Avg. player count with other characters in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }	// Number of times played against other characters
		},

		// Captain Falcon VS Records
		{
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// KO count on each character
			0,										// Time used
			0,										// Damage dealt
			0,										// Damage taken
			0,										// ???
			0,										// Self-destructs
			0,										// Games played with this character present
			0,										// Avg. player count of this character in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// Avg. player count with other characters in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }	// Number of times played against other characters
		},

		// Kirby VS Records
		{
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// KO count on each character
			0,										// Time used
			0,										// Damage dealt
			0,										// Damage taken
			0,										// ???
			0,										// Self-destructs
			0,										// Games played with this character present
			0,										// Avg. player count of this character in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// Avg. player count with other characters in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }	// Number of times played against other characters
		},

		// Pikachu VS Records
		{
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// KO count on each character
			0,										// Time used
			0,										// Damage dealt
			0,										// Damage taken
			0,										// ???
			0,										// Self-destructs
			0,										// Games played with this character present
			0,										// Avg. player count of this character in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// Avg. player count with other characters in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }	// Number of times played against other characters
		},

		// Jigglypuff VS Records
		{
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// KO count on each character
			0,										// Time used
			0,										// Damage dealt
			0,										// Damage taken
			0,										// ???
			0,										// Self-destructs
			0,										// Games played with this character present
			0,										// Avg. player count of this character in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// Avg. player count with other characters in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }	// Number of times played against other characters
		},

		// Ness VS Records
		{
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// KO count on each character
			0,										// Time used
			0,										// Damage dealt
			0,										// Damage taken
			0,										// ???
			0,										// Self-destructs
			0,										// Games played with this character present
			0,										// Avg. player count of this character in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },	// Avg. player count with other characters in-game
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }	// Number of times played against other characters
		}
	},

	TRUE,											// Are screen flashes allowed?
	1, 												// 0 = Mono, 1 = Stereo
	0,												// Vertical screen adjust offset
	0,												// Horizontal screen adjust offset
	nFTKindMario,									// Last character viewed on Character Data menu
	0,												// Mask of unlocked features
	0,												// Mask of available characters
	nSC1PGameDifficultyEasy,						// Last 1P Game difficulty setting
	2,												// Last 1P Game stocks setting

	// 1P Records
	{
		// Mario 1P Records
		{
			0,										// 1P Game High-Score
			0,										// 1P Game number of continues used
			0,										// 1P Game humber of bonuses earned
			0,										// 1P Game highest difficulty cleared
			I_MIN_TO_TICS(60),						// Break the Targets best time
			0,										// Break the Targets number of targets broken
			I_MIN_TO_TICS(60),						// Board the Platforms best time
			0,										// Board the Platforms number of platforms boarded
			FALSE									// Has this character cleared 1P Game?
		},

		// Fox 1P Records
		{
			0,										// 1P Game High-Score
			0,										// 1P Game number of continues used
			0,										// 1P Game humber of bonuses earned
			0,										// 1P Game highest difficulty cleared
			I_MIN_TO_TICS(60),						// Break the Targets best time
			0,										// Break the Targets number of targets broken
			I_MIN_TO_TICS(60),						// Board the Platforms best time
			0,										// Board the Platforms number of platforms boarded
			FALSE									// Has this character cleared 1P Game?
		},

		// Donkey Kong 1P Records
		{
			0,										// 1P Game High-Score
			0,										// 1P Game number of continues used
			0,										// 1P Game humber of bonuses earned
			0,										// 1P Game highest difficulty cleared
			I_MIN_TO_TICS(60),						// Break the Targets best time
			0,										// Break the Targets number of targets broken
			I_MIN_TO_TICS(60),						// Board the Platforms best time
			0,										// Board the Platforms number of platforms boarded
			FALSE									// Has this character cleared 1P Game?
		},

		// Samus 1P Records
		{
			0,										// 1P Game High-Score
			0,										// 1P Game number of continues used
			0,										// 1P Game humber of bonuses earned
			0,										// 1P Game highest difficulty cleared
			I_MIN_TO_TICS(60),						// Break the Targets best time
			0,										// Break the Targets number of targets broken
			I_MIN_TO_TICS(60),						// Board the Platforms best time
			0,										// Board the Platforms number of platforms boarded
			FALSE									// Has this character cleared 1P Game?
		},

		// Luigi 1P Records
		{
			0,										// 1P Game High-Score
			0,										// 1P Game number of continues used
			0,										// 1P Game humber of bonuses earned
			0,										// 1P Game highest difficulty cleared
			I_MIN_TO_TICS(60),						// Break the Targets best time
			0,										// Break the Targets number of targets broken
			I_MIN_TO_TICS(60),						// Board the Platforms best time
			0,										// Board the Platforms number of platforms boarded
			FALSE									// Has this character cleared 1P Game?
		},

		// Link 1P Records
		{
			0,										// 1P Game High-Score
			0,										// 1P Game number of continues used
			0,										// 1P Game humber of bonuses earned
			0,										// 1P Game highest difficulty cleared
			I_MIN_TO_TICS(60),						// Break the Targets best time
			0,										// Break the Targets number of targets broken
			I_MIN_TO_TICS(60),						// Board the Platforms best time
			0,										// Board the Platforms number of platforms boarded
			FALSE									// Has this character cleared 1P Game?
		},

		// Yoshi 1P Records
		{
			0,										// 1P Game High-Score
			0,										// 1P Game number of continues used
			0,										// 1P Game humber of bonuses earned
			0,										// 1P Game highest difficulty cleared
			I_MIN_TO_TICS(60),						// Break the Targets best time
			0,										// Break the Targets number of targets broken
			I_MIN_TO_TICS(60),						// Board the Platforms best time
			0,										// Board the Platforms number of platforms boarded
			FALSE									// Has this character cleared 1P Game?
		},

		// Captain Falcon 1P Records
		{
			0,										// 1P Game High-Score
			0,										// 1P Game number of continues used
			0,										// 1P Game humber of bonuses earned
			0,										// 1P Game highest difficulty cleared
			I_MIN_TO_TICS(60),						// Break the Targets best time
			0,										// Break the Targets number of targets broken
			I_MIN_TO_TICS(60),						// Board the Platforms best time
			0,										// Board the Platforms number of platforms boarded
			FALSE									// Has this character cleared 1P Game?
		},

		// Kirby 1P Records
		{
			0,										// 1P Game High-Score
			0,										// 1P Game number of continues used
			0,										// 1P Game humber of bonuses earned
			0,										// 1P Game highest difficulty cleared
			I_MIN_TO_TICS(60),						// Break the Targets best time
			0,										// Break the Targets number of targets broken
			I_MIN_TO_TICS(60),						// Board the Platforms best time
			0,										// Board the Platforms number of platforms boarded
			FALSE									// Has this character cleared 1P Game?
		},

		// Pikachu 1P Records
		{
			0,										// 1P Game High-Score
			0,										// 1P Game number of continues used
			0,										// 1P Game humber of bonuses earned
			0,										// 1P Game highest difficulty cleared
			I_MIN_TO_TICS(60),						// Break the Targets best time
			0,										// Break the Targets number of targets broken
			I_MIN_TO_TICS(60),						// Board the Platforms best time
			0,										// Board the Platforms number of platforms boarded
			FALSE									// Has this character cleared 1P Game?
		},

		// Jigglypuff 1P Records
		{
			0,										// 1P Game High-Score
			0,										// 1P Game number of continues used
			0,										// 1P Game humber of bonuses earned
			0,										// 1P Game highest difficulty cleared
			I_MIN_TO_TICS(60),						// Break the Targets best time
			0,										// Break the Targets number of targets broken
			I_MIN_TO_TICS(60),						// Board the Platforms best time
			0,										// Board the Platforms number of platforms boarded
			FALSE									// Has this character cleared 1P Game?
		},

		// Ness 1P Records
		{
			0,										// 1P Game High-Score
			0,										// 1P Game number of continues used
			0,										// 1P Game humber of bonuses earned
			0,										// 1P Game highest difficulty cleared
			I_MIN_TO_TICS(60),						// Break the Targets best time
			0,										// Break the Targets number of targets broken
			I_MIN_TO_TICS(60),						// Board the Platforms best time
			0,										// Board the Platforms number of platforms boarded
			FALSE									// Has this character cleared 1P Game?
		}
	},

	0,												// Mask of unique stages played in VS Mode (for Mushroom Kingdom)
	0,												// Number of games played in VS Mode to unlock Item Switch
	0,												// Total number of games played in VS Mode
	0,												// Anti-Piracy measures mask; this is where the "penalties" are stored
	0,												// Boot count
	0x29A,											// Signature?
	0												// Checksum of all previous save data struct members' values
};

// 0x800A3F80
SCCommonData dSCManagerDefaultSceneData =
{
#if defined(REGION_US)
	nSCKindStartup,										// Current scene
	nSCKindStartup,										// Previous scene
#else
	nSCKindOpeningRoom,									// Current scene
	nSCKindOpeningRoom,									// Previous scene
#endif
	// Queued unlock messages
	{
		nLBBackupUnlockEnumCount,
		nLBBackupUnlockEnumCount,
		nLBBackupUnlockEnumCount,
		nLBBackupUnlockEnumCount,
		nLBBackupUnlockEnumCount,
		nLBBackupUnlockEnumCount,
		nLBBackupUnlockEnumCount
	},

	nFTKindLuigi,									// Challanger approaching character
	0,												// Mask of previously demo'd characters
	nFTKindNull,									// First demo character?

	// Demo characters
	{
		nFTKindNull,
		nFTKindNull
	},

	nGRKindCastle,									// Stage selected

	FALSE,											// Is Sudden Death?
	FALSE,											// Has "continue" been selected?
	FALSE,											// Has A + B + Z + R been input?

	0,												// 1P Game player's port
	nFTKindNull,									// 1P Game player's character
	0,												// 1P Game player's costume
	5,												// 1P Game time limit (in seconds, 100 = infinite)
	0,												// 1P Game current stage
	
	// 1P Game ally characters
	{
		nFTKindMario,
		nFTKindMario
	},

	0,												// 1P Game time remaining (in seconds)
	0,												// 1P Game score
	0,												// 1P Game continues used
	0,												// 1P Game total bonuses acquired
	
	// Bonus masks
	{
		0,
		0,
		0
	},

	0,												// Bonus stage tasks completed
	nFTKindNull,									// Bonus stage character
	0,												// Bonus stage costume

	nFTKindNull,									// Training mode player's character
	0,												// Training mode player's costume
	nFTKindNull,									// Training mode player's character
	0,												// Training mode player's costume
	TRUE,											// Extend time until auto-demo starts?
	0,												// Auto-demo current stage order index
	nGRKindCastle,									// VS Mode stage selected
	nGRKindCastle,									// Training Mode stage selected
	0,												// Levels to subtract from challenger's CPU level
	FALSE											// Has the title screen animation been viewed?
};

// 0x800A3FC8
SCBattleState dSCManagerDefaultBattleState =
{
	nSCBattleGameTypeDemo,							// Game type
	nGRKindCastle,									// Stage
	FALSE,											// Is team battle?
	SCBATTLE_GAMERULE_TIME,							// Game rule
	0,												// Total players in-game
	0,												// Total CPUs in-game
	3,												// Time limit (in seconds)
	2,												// Stocks
	nSCBattleHandicapOff,							// Handicap setting
	FALSE,											// Is team attack enabled?
	TRUE,											// Is stage select enabled?
	100,											// Damage ratio
	~0,												// Item Switch mask
	TRUE,											// Reset players when first entering VS Mode character select screen?
	nSCBattleGameStatusWait,						// Status of current match
	0,												// Time remaining (in tics)
	0,												// Time passed (in tics)
	nSCBattleItemSwitchMiddle,						// Item appearance rate
	TRUE,											// Display score?
	0,												// Shadow colors based on team or use default black?

	// Player Data
	{
		// Player 1
		{
			3,										// CPU level
			9,										// Handicap
			nFTPlayerKindNot,						// Player type
			nFTKindNull,							// Character
			0,										// Team
			0,										// Port
			0,										// Costume
			0,										// Shade
			0,										// Color
			TRUE,									// Is permanent stock icon?
			0,										// Player tag
			0,										// Stock count
			FALSE,									// Is a "VS [Character] Team" member?
			0,										// Placement
			0,										// Falls
			0,										// Score

			// Total number of KOs scored on each player
			{
				0, 0, 0, 0
			},

			0,										// ???
			0,										// ???

			0,										// Total number of self-destructs
			0,										// Total damage dealt
			0,										// Total damage taken from all sources

			// Total damage taken from each player
			{
				0, 0, 0, 0
			},

			0,										// Total damage taken on current stock
			0,										// Combo damage taken from foes
			0,										// Combo hits landed by foes
			NULL,									// Pointer to fighter GObj
			0,										// Current position in stale moves queue
			
			// Stale moves info
			{
				{ 0, 0 },
				{ 0, 0 },
				{ 0, 0 },
				{ 0, 0 },
				{ 0, 0 }
			}
		},

		// Player 2
		{
			3,										// CPU level
			9,										// Handicap
			nFTPlayerKindNot,						// Player type
			nFTKindNull,							// Character
			0,										// Team
			0,										// Port
			0,										// Costume
			0,										// Shade
			0,										// Color
			TRUE,									// Permanent stock icon?
			1,										// Player tag
			0,										// Stock count
			FALSE,									// Is a "VS [Character] Team" member?
			0,										// Placement
			0,										// Falls
			0,										// Score

			// Total number of KOs scored on each player
			{
				0, 0, 0, 0
			},

			0,										// ???
			0,										// ???

			0,										// Total number of self-destructs
			0,										// Total damage dealt
			0,										// Total damage taken from all sources

			// Total damage taken from each player
			{
				0, 0, 0, 0
			},

			0,										// Total damage taken on current stock
			0,										// Combo damage taken from foes
			0,										// Combo hits landed by foes
			NULL,									// Pointer to fighter GObj
			0,										// Current position in stale moves queue
			
			// Stale moves info
			{
				{ 0, 0 },
				{ 0, 0 },
				{ 0, 0 },
				{ 0, 0 },
				{ 0, 0 }
			}
		},

		// Player 3
		{
			3,										// CPU level
			9,										// Handicap
			nFTPlayerKindNot,						// Player type
			nFTKindNull,							// Character
			1,										// Team
			0,										// Port
			0,										// Costume
			0,										// Shade
			0,										// Color
			TRUE,									// Permanent stock icon?
			2,										// Player tag
			0,										// Stock count
			FALSE,									// Is a "VS [Character] Team" member?
			0,										// Placement
			0,										// Falls
			0,										// Score

			// Total number of KOs scored on each player
			{
				0, 0, 0, 0
			},

			0,										// ???
			0,										// ???

			0,										// Total number of self-destructs
			0,										// Total damage dealt
			0,										// Total damage taken from all sources

			// Total damage taken from each player
			{
				0, 0, 0, 0
			},

			0,										// Total damage taken on current stock
			0,										// Combo damage taken from foes
			0,										// Combo hits landed by foes
			NULL,									// Pointer to fighter GObj
			0,										// Current position in stale moves queue
			
			// Stale moves info
			{
				{ 0, 0 },
				{ 0, 0 },
				{ 0, 0 },
				{ 0, 0 },
				{ 0, 0 }
			}
		},

		// Player 4
		{
			3,										// CPU level
			9,										// Handicap
			nFTPlayerKindNot,						// Player type
			nFTKindNull,							// Character
			1,										// Team
			0,										// Port
			0,										// Costume
			0,										// Shade
			0,										// Color
			TRUE,									// Permanent stock icon?
			3,										// Player tag
			0,										// Stock count
			FALSE,									// Is a "VS [Character] Team" member?
			0,										// Placement
			0,										// Falls
			0,										// Score

			// Total number of KOs scored on each player
			{
				0, 0, 0, 0
			},

			0,										// ???
			0,										// ???

			0,										// Total number of self-destructs
			0,										// Total damage dealt
			0,										// Total damage taken from all sources

			// Total damage taken from each player
			{
				0, 0, 0, 0
			},

			0,										// Total damage taken on current stock
			0,										// Combo damage taken from foes
			0,										// Combo hits landed by foes
			NULL,									// Pointer to fighter GObj
			0,										// Current position in stale moves queue
			
			// Stale moves info
			{
				{ 0, 0 },
				{ 0, 0 },
				{ 0, 0 },
				{ 0, 0 },
				{ 0, 0 }
			}
		}
	}
};

#if defined(REGION_US)
// 0x800A41B8
s32 dSCManagerPad0x800A41B8[/* */] = { 0, 0 };

// 0x800A41C0 (.rodata) - use { __DATE__ " " __TIME__ } in a real setting
char dSCManagerBuildDate[/* */] = { "Mar 16 1999 18:26:57" };
#else
char dSCManagerBuildDate[/* */] = { "Dec 23 1998 18:06:24" };
#endif

// // // // // // // // // // // //
//                               //
//           FUNCTIONS           //
//                               //
// // // // // // // // // // // //

// 0x800A1980
void scManagerRunLoop(sb32 arg)
{
#ifndef PORT
	u16 *framebuffer;
	uintptr_t end;
#endif

	syControllerSetStatusDelay(60);

	syDebugSetFuncPrint(scManagerFuncPrint);
	syDebugStartRmonThread5Hang();

	syDmaLoadOverlay(&dSCManagerOverlays[0]);
	syDmaLoadOverlay(&dSCManagerOverlays[2]);
	syDmaLoadOverlay(&dSCManagerOverlays[1]);

	gSCManagerBackupData = dSCManagerDefaultBackupData;
	gSCManagerSceneData = dSCManagerDefaultSceneData;

	gSCManager1PGameBattleState   = 
	gSCManagerTransferBattleState =
	gSCManagerVSBattleState       = dSCManagerDefaultBattleState;

	ftManagerSetupFileSize();
	dSYAudioPublicSettings.unk31 = 72;

	/* Wait for the audio thread to finish its settings-update restart
	 * (n_alClose -> portAudioLoadAssets -> syAudioMakeBGMPlayers -> clear
	 * dSYAudioIsSettingsUpdated) before dispatching the first scene.
	 * Skipping it lets syAudioPlayBGM race the restart: on JP the first
	 * scene is nSCKindOpeningRoom immediately (no ~3 s nSCKindStartup to
	 * mask it as on US), so syAudioStopBGMAll() wipes the queued BGM and
	 * the attract music is silent. See
	 * docs/bugs/jp_attract_music_settings_update_race_2026-05-18.md.
	 * PORT swaps the N64 busy-spin for a cooperative yield — the same
	 * idiom as the scautodemo / scvsbattle BGM waits. */
	syAudioSetSettingsUpdated();

	while (syAudioGetSettingsUpdated() != FALSE)
	{
#ifdef PORT
		port_coroutine_yield();
#endif
		continue;
	}
	syAudioSetFXType(AL_FX_CUSTOM);

	while (syAudioGetRestarting() != FALSE)
	{
#ifdef PORT
		port_coroutine_yield();
#endif
		continue;
	}
	lbBackupIsSramValid();
	lbBackupApplyOptions();

#ifndef PORT
	/* N64 only — no physical framebuffers on PC; Fast3D manages them. */
	framebuffer = (u16*) gSYFramebufferSets;
	end = 0x80400000;

	while ((uintptr_t)framebuffer < end)
	{
		*framebuffer++ = GPACK_RGBA5551(0x00, 0x00, 0x00, 0x01);
	}
#endif
	if (gSYControllerConnectedNum == 0)
	{
		gSCManagerSceneData.scene_curr = nSCKindNoController;
	}
#ifdef PORT_STAGE_CYCLE_DEMO
	gSCManagerSceneData.scene_curr    = nSCKindAutoDemo;
	gSCManagerSceneData.scene_prev    = nSCKindAutoDemo;
	gSCManagerSceneData.demo_fkind[0] = nFTKindMario;
	gSCManagerSceneData.demo_fkind[1] = nFTKindFox;
#endif
#ifdef PORT
	/* Classic Co-op handoff defaults to "no P2". BSS zero-init would read
	 * as port 0, which is a valid port — the sentinel must be explicit.
	 * The co-op CSS rewrites this on every Classic entry. */
	gSCManagerSceneData.coop_player2 = SCCOMMON_COOP_NO_PLAYER2;
	{
		const char *env = getenv("SSB64_START_SCENE");
		if (env != NULL)
		{
			int n = atoi(env);
			gSCManagerSceneData.scene_curr = n;
			gSCManagerSceneData.scene_prev = n;
			/* The override jumps the player into a scene that they may
			 * not have unlocked yet (e.g. nSCKindSoundTest requires
			 * LBBACKUP_UNLOCK_MASK_SOUNDTEST). Several menus assume
			 * scene_prev only ever names a reachable scene, and gate
			 * GObj creation on the matching unlock bit. Visiting Sound
			 * Test on a fresh save and pressing B otherwise drops back
			 * into the Data menu with sMNDataOption=SoundTest but
			 * sMNDataOptionSoundTestGObj=NULL → SIGSEGV. Granting all
			 * unlocks alongside the scene override keeps backup-mask
			 * state consistent with where the user told us to start. */
			gSCManagerBackupData.unlock_mask |= LBBACKUP_UNLOCK_MASK_ALL;
			port_log("SSB64: SSB64_START_SCENE override → scene=%d (unlock_mask |= ALL)\n", n);
		}
		const char *stage_env = getenv("SSB64_SPGAME_STAGE");
		if (stage_env != NULL)
		{
			int s = atoi(stage_env);
			gSCManagerSceneData.spgame_stage = s;
			port_log("SSB64: SSB64_SPGAME_STAGE override → spgame_stage=%d (13=Boss/MasterHand)\n", s);
		}
		const char *fkind_env = getenv("SSB64_SPGAME_FKIND");
		if (fkind_env != NULL)
		{
			int f = atoi(fkind_env);
			extern s32 ftParamGetCostumeCommonID(s32 fkind, s32 color);
			gSCManagerSceneData.fkind = f;
			/* The costume the character select gives on A (round 17): a raw 0 is not every
			 * fighter's own (Link's 0 is his white outfit). */
			gSCManagerSceneData.costume = (u8)ftParamGetCostumeCommonID(f, 0);
			if (getenv("SSB64_SPGAME_COSTUME") != NULL) gSCManagerSceneData.costume = (u8)atoi(getenv("SSB64_SPGAME_COSTUME"));
			port_log("SSB64: 1P costume %d (A gives %d)\n", (int)gSCManagerSceneData.costume, (int)ftParamGetCostumeCommonID(f, 0));
			port_log("SSB64: SSB64_SPGAME_FKIND override → fkind=%d\n", f);
		}
		/* Classic Co-op debug: inject a P2 handoff without going through
		 * the CSS, for direct-boot testing of the co-op game side, e.g.
		 *   SSB64_START_SCENE=52 SSB64_COOP_P2=1 SSB64_COOP_P2_FKIND=5 */
		const char *coop_env = getenv("SSB64_COOP_P2");
		if (coop_env != NULL)
		{
			const char *coop_fkind_env = getenv("SSB64_COOP_P2_FKIND");
			gSCManagerSceneData.coop_player2 = (u8)atoi(coop_env);
			gSCManagerSceneData.coop_fkind2 = (u8)((coop_fkind_env != NULL) ? atoi(coop_fkind_env) : 1);
			gSCManagerSceneData.coop_costume2 = 0;
			gSCManagerSceneData.coop_shade2 = 0;
			port_log("SSB64: SSB64_COOP_P2 override → P2 port=%d fkind=%d\n",
			         (int)gSCManagerSceneData.coop_player2, (int)gSCManagerSceneData.coop_fkind2);
		}
		syNetReplayInitDebugEnv();
#ifdef HANDHELD_PORT
		{
			extern void hh_bench_setup(void);
			hh_bench_setup();   /* SSB64_BENCH / bench.cfg: straight into the benchmark fight */
		}
#endif
		syNetPeerInitDebugEnv();
	}

	// boot straight to CSS if enabled
	extern int port_enhancement_boot_to_vs_css(void);
	if (port_enhancement_boot_to_vs_css())
	{
		// Overwrite the starting scene
		gSCManagerSceneData.scene_curr = nSCKindPlayersVS;
		gSCManagerSceneData.scene_prev = nSCKindPlayersVS;
	}
	port_log("SSB64: scManagerRunLoop — controllers=%d scene=%d\n",
	         (int)gSYControllerConnectedNum, (int)gSCManagerSceneData.scene_curr);
#endif
	while (TRUE)
	{
#ifdef PORT
		/* Console reset: consume a pending reset (re-applies the target
		 * scene and clears the flag) before dispatching. For top-level
		 * scenes this is where the reset lands; for 1P sub-scenes the
		 * sc1pmanager.c guards consumed it already and this is a no-op. */
		{
			extern sb32 portSCManagerConsumeReset(void);
			portSCManagerConsumeReset();
		}

		/* Issue #260: never show the N64 boot logo on PC. Every entry
		 * point into nSCKindStartup (cold boot via the US default scene
		 * data, and the attract-loop returns from title / auto-demo /
		 * staff roll / 1P ending / unlock message) is redirected here at
		 * the single dispatch point to the opening movie — the scene the
		 * logo itself proceeds to. scene_prev is left as the setter wrote
		 * it; nothing keys off scene_prev == nSCKindStartup. JP builds
		 * already boot straight to nSCKindOpeningRoom, and the BGM
		 * settings-update race that path once exposed is closed by the
		 * syAudioGetSettingsUpdated() wait above. */
#if defined(REGION_US)
		if (gSCManagerSceneData.scene_curr == nSCKindStartup)
		{
			gSCManagerSceneData.scene_curr = nSCKindOpeningRoom;
		}
#endif

		port_log("SSB64: scManagerRunLoop — entering scene %d\n",
		         (int)gSCManagerSceneData.scene_curr);

		/* Issue #103 defence: clear cross-scene GObj pointers in the three
		 * persistent battle-state structs before dispatching the next scene.
		 * `fighter_gobj` is the only field in SCPlayerData that holds a host
		 * pointer; it's set at fighter spawn (ftparam.c:197 in movie scenes,
		 * and via FTStruct linkage in the regular battle path), survives the
		 * battle scene's arena, and is read across scenes in callbacks like
		 * ftshadow.c:107 and ftcomputer.c:5623. With taskman.c's prev-arena
		 * free in place those reads now SIGSEGV at the read site instead of
		 * hitting plausible-looking stale data. NULL the slot at the scene
		 * boundary so the read just yields NULL (which the consumers already
		 * gate on, e.g. controller.c:208). */
		for (s32 _p = 0; _p < GMCOMMON_PLAYERS_MAX; _p++) {
			gSCManager1PGameBattleState.players[_p].fighter_gobj = NULL;
			gSCManagerVSBattleState.players[_p].fighter_gobj = NULL;
			gSCManagerTransferBattleState.players[_p].fighter_gobj = NULL;
		}

		/* Zebes acid + efManagerMakeEffect stale-dl_link family
		 * (docs/bugs/zebes_acid_effect_stale_dl_link_holder_2026-05-23.md):
		 * scene routines do not eject their own GObjs on exit — there are no
		 * per-stage teardown hooks, and the existing gcEjectAll() sweep only
		 * runs at task boundaries (taskman.c:1185, 1203), not between the
		 * intra-task scene transitions that the attract demo cycles through.
		 * The previous scene's stage / effect GObjs therefore stay linked in
		 * gGCCommonLinks[], their DObj subtrees keep pointing at dl_link
		 * memory in the recycled scene arena, and on the next frame the
		 * renderer walks freed bytes — the defensive guard in
		 * gcDrawDObjTreeDLLinks bails with "stale dl_link" and the entire
		 * subtree drops (visible: particle / stage geometry corruption that
		 * persists across the attract → title → match transition).
		 *
		 * Eject every linked GObj here, before the next scene's start routine
		 * runs. Per objdef.h::GObjLinkID, every link kind is scene-scoped: no
		 * GObj is documented to survive a scene transition. We mirror the
		 * body of gcEjectAll() (objhelper.c:465) inline so we can clamp any
		 * non-progress — gcEjectGObj refuses to drop gGCCurrentCommon, which
		 * shouldn't be set between scenes but is cheap to guard against. The
		 * existing GOBJ_PORT_EJECTED_SENTINEL guard in gcEjectGObj handles a
		 * stage routine that already self-ejected (e.g. grHyruleTwister at
		 * grhyrule.c:334) — that GObj is no longer in the link, so the sweep
		 * just doesn't see it. */
		{
			s32 _link;
			for (_link = 0; _link < ARRAY_COUNT(gGCCommonLinks); _link++) {
				GObj *_head;
				s32 _guard = 0;
				while ((_head = gGCCommonLinks[_link]) != NULL) {
					gcEjectGObj(_head);
					if (gGCCommonLinks[_link] == _head) {
						port_log("SSB64: scManagerRunLoop scene-boundary GObj "
						         "sweep — no progress link=%d head=%p, breaking\n",
						         (int)_link, (void*)_head);
						break;
					}
					if (++_guard > 4096) {
						port_log("SSB64: scManagerRunLoop scene-boundary GObj "
						         "sweep — runaway link=%d, breaking\n",
						         (int)_link);
						break;
					}
				}
			}
		}

		/* gGRCommonStruct is a union of per-stage var blobs; the cached GObj*
		 * slots (zebes.map_gobj, hyrule.twister_gobj, jungle.tarucann_gobj,
		 * castle.bumper_gobj, yamabuki.{gate,monster}_gobj, pupupu.map_gobj[],
		 * inishie.{pakkun,pblock}_gobj, ...) all overlay one another. The
		 * GObjs they cached were just ejected above; null the holders so
		 * nothing (e.g. a stale subsystem callback that survived eject)
		 * dereferences a freed slot before the next stage init repopulates
		 * the union from scratch. */
		memset(&gGRCommonStruct, 0, sizeof(gGRCommonStruct));
		{
			/* the opening movie is framed by the renderer's movie crop (gfx_pc gfx_set_movie_frame) */
			extern void gfx_set_movie_frame(int on);
			gfx_set_movie_frame(gSCManagerSceneData.scene_curr >= nSCKindOpeningRoom &&
			                    gSCManagerSceneData.scene_curr <= nSCKindOpeningNewcomers);
		}
#endif
		switch (gSCManagerSceneData.scene_curr)
		{
			case nSCKindNoController:
				syDmaLoadOverlay(&dSCManagerOverlays[11]);
				mnNoControllerStartScene();
				break;

			case nSCKindTitle:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[10]);
				syDmaLoadOverlay(&dSCManagerOverlays[8]);
				syDmaLoadOverlay(&dSCManagerOverlays[9]);
				mnTitleStartScene();
				break;

			case nSCKindDebugMaps:
				syDmaLoadOverlay(&dSCManagerOverlays[12]);
				syDmaLoadOverlay(&dSCManagerOverlays[8]);
				syDmaLoadOverlay(&dSCManagerOverlays[9]);
				dbMapsStartScene();
				break;

			case nSCKindDebugCube:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[13]);
				syDmaLoadOverlay(&dSCManagerOverlays[8]);
				syDmaLoadOverlay(&dSCManagerOverlays[9]);
				dbCubeStartScene();
				break;

			case nSCKindDebugBattle:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[14]);
				syDmaLoadOverlay(&dSCManagerOverlays[8]);
				syDmaLoadOverlay(&dSCManagerOverlays[9]);
				dbBattleStartScene();
				break;

			case nSCKindDebugFalls:
				syDmaLoadOverlay(&dSCManagerOverlays[15]);
				dbFallsStartScene();
				break;

			case nSCKindDebugUnknown:
				syDmaLoadOverlay(&dSCManagerOverlays[16]);
				mnUnusedFightersStartScene();
				break;

			case nSCKindModeSelect:
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[17]);
				mnModeSelectStartScene();
				break;

			case nSCKind1PMode:
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[18]);
				mn1PModeStartScene();
				break;

			case nSCKindOption:
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
#if defined(REGION_US)
				syDmaLoadOverlay(&dSCManagerOverlays[60]);
#else
				syDmaLoadOverlay(&dSCManagerOverlays[58]);
#endif
				mnOptionStartScene();
				break;

			case nSCKindData:
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
#if defined(REGION_US)
				syDmaLoadOverlay(&dSCManagerOverlays[61]);
#else
				syDmaLoadOverlay(&dSCManagerOverlays[59]);
#endif
				mnDataStartScene();
				break;

			case nSCKindVSMode:
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[19]);
				mnVSModeStartScene();
				break;

			case nSCKindVSOptions:
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[20]);
				mnVSOptionsStartScene();
				break;

			case nSCKindVSItemSwitch:
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[21]);
				mnVSItemSwitchStartScene();
				break;

			case nSCKindMessage:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[22]);
				mnMessageStartScene();
				break;

			case nSCKind1PChallenger:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[23]);
				sc1PChallengerStartScene();
				break;

			case nSCKind1PIntro:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[24]);
				sc1PIntroStartScene();
				break;

			case nSCKindScreenAdjust:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[25]);
				mnScreenAdjustStartScene();
				break;

			case nSCKindPlayersVS:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[26]);
				mnPlayersVSStartScene();

				// skip stage select
				{
					extern int port_get_comp_ruleset(void);
					if (port_get_comp_ruleset() && gSCManagerSceneData.scene_curr == nSCKindMaps)
					{
						gSCManagerSceneData.gkind = nGRKindPupupu;
						gSCManagerTransferBattleState.gkind = nGRKindPupupu;
						gSCManagerSceneData.scene_curr = nSCKindVSBattle;
					}
				}
				break;

			case nSCKindMaps:
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[30]);
				mnMapsStartScene();
				break;

			case nSCKindVSBattle:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[3]);
				syDmaLoadOverlay(&dSCManagerOverlays[4]);

				// apply competitive ruleset
				{
					extern int port_get_comp_ruleset(void);
					if (port_get_comp_ruleset())
					{
						int i;
						extern __typeof__(gSCManagerTransferBattleState) gSCManagerVSBattleState;

						gSCManagerTransferBattleState.game_rules = 0x2; // Stock Mode
						gSCManagerTransferBattleState.time_limit = 8; // 8 minutes
						gSCManagerTransferBattleState.stocks = 3; // 4 Stocks
						gSCManagerTransferBattleState.is_show_score = FALSE;
						gSCManagerTransferBattleState.item_toggles = 0; // No Items
						gSCManagerTransferBattleState.item_appearance_rate = 4; // None
						gSCManagerTransferBattleState.gkind = nGRKindPupupu; // Dream Land

						// set Team Attack to ON
						if (gSCManagerTransferBattleState.is_team_battle == TRUE)
						{
							gSCManagerTransferBattleState.is_team_attack = TRUE;
						}

						gSCManagerVSBattleState.game_rules = 0x2;
						gSCManagerVSBattleState.time_limit = 0;
						gSCManagerVSBattleState.stocks = 3;
						gSCManagerVSBattleState.gkind = nGRKindPupupu;

						for (i = 0; i < GMCOMMON_PLAYERS_MAX; i++)
						{
							if (gSCManagerTransferBattleState.players[i].pkind != 2)
							{
								gSCManagerTransferBattleState.players[i].stock_count = 3;
								gSCManagerTransferBattleState.players[i].is_single_stockicon = FALSE;
								gSCManagerTransferBattleState.players[i].falls = 0;
								gSCManagerTransferBattleState.players[i].score = 0;
							}
						}
					}
					else
					{
						// vanilla timer fix
						if (!(gSCManagerTransferBattleState.game_rules & 0x1))
						{
							gSCManagerTransferBattleState.time_limit = 0; // Infinite Time
						}
					}
				}

				// force default CPU level to 9 if checked
				{
					extern int port_enhancement_cpu_level_9(void);
					if (port_enhancement_cpu_level_9())
					{
						int i;
						for (i = 0; i < GMCOMMON_PLAYERS_MAX; i++)
						{
							// 1 = nFTPlayerKindCom (Slot is a CPU)
							if (gSCManagerTransferBattleState.players[i].pkind == 1)
							{
								gSCManagerTransferBattleState.players[i].level = 9;
							}
						}
					}
				}

				scVSBattleStartScene();

				// clean up timer
				{
					extern int port_get_comp_ruleset(void);
					if (port_get_comp_ruleset())
					{
						extern __typeof__(gSCManagerTransferBattleState) gSCManagerVSBattleState;
						gSCManagerVSBattleState.time_limit = SCBATTLE_TIMELIMIT_INFINITE;
					}
				}
				break;

			case nSCKindUnknownMario:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[3]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[5]);
				mvUnknownMarioStartScene();
				break;

			case nSCKind1PGame:
				sc1PManagerUpdateScene();
				break;

			case nSCKind1PBonusStage:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[3]);
				syDmaLoadOverlay(&dSCManagerOverlays[6]);
				sc1PBonusStageStartScene();
				break;

			case nSCKind1PTrainingMode:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[3]);
				syDmaLoadOverlay(&dSCManagerOverlays[7]);
				sc1PTrainingModeStartScene();
				break;

			case nSCKindVSResults:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[31]);
				mnVSResultsStartScene();
				break;

			case nSCKindVSRecord:
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[32]);
				mnVSRecordStartScene();
				break;

			case nSCKindCharacters:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[33]);
				mnCharactersStartScene();
				break;

#if defined(REGION_US)
				case nSCKindStartup:
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[58]);
				mnStartupStartScene();
				break;
#endif

			case nSCKindOpeningRoom:
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[34]);
				mvOpeningRoomStartScene();
				break;

			case nSCKindOpeningPortraits:
				syDmaLoadOverlay(&dSCManagerOverlays[35]);
				mvOpeningPortraitsStartScene();
				break;

			case nSCKindOpeningMario:
				syDmaLoadOverlay(&dSCManagerOverlays[3]);
				syDmaLoadOverlay(&dSCManagerOverlays[36]);
				mvOpeningMarioStartScene();
				break;

			case nSCKindOpeningDonkey:
				syDmaLoadOverlay(&dSCManagerOverlays[37]);
				mvOpeningDonkeyStartScene();
				break;

			case nSCKindOpeningSamus:
				syDmaLoadOverlay(&dSCManagerOverlays[38]);
				mvOpeningSamusStartScene();
				break;

			case nSCKindOpeningFox:
				syDmaLoadOverlay(&dSCManagerOverlays[39]);
				mvOpeningFoxStartScene();
				break;

			case nSCKindOpeningLink:
				syDmaLoadOverlay(&dSCManagerOverlays[40]);
				mvOpeningLinkStartScene();
				break;

			case nSCKindOpeningYoshi:
				syDmaLoadOverlay(&dSCManagerOverlays[41]);
				mvOpeningYoshiStartScene();
				break;

			case nSCKindOpeningPikachu:
				syDmaLoadOverlay(&dSCManagerOverlays[42]);
				mvOpeningPikachuStartScene();
				break;

			case nSCKindOpeningKirby:
				syDmaLoadOverlay(&dSCManagerOverlays[43]);
				mvOpeningKirbyStartScene();
				break;

			case nSCKindOpeningRun:
				syDmaLoadOverlay(&dSCManagerOverlays[44]);
				mvOpeningRunStartScene();
				break;
				
			case nSCKindOpeningYoster:
				syDmaLoadOverlay(&dSCManagerOverlays[45]);
				mvOpeningYosterStartScene();
				break;

			case nSCKindOpeningCliff:
				syDmaLoadOverlay(&dSCManagerOverlays[46]);
				mvOpeningCliffStartScene();
				break;

			case nSCKindOpeningStandoff:
				syDmaLoadOverlay(&dSCManagerOverlays[47]);
				mvOpeningStandoffStartScene();
				break;

			case nSCKindOpeningYamabuki:
				syDmaLoadOverlay(&dSCManagerOverlays[48]);
				mvOpeningYamabukiStartScene();
				break;

			case nSCKindOpeningClash:
				syDmaLoadOverlay(&dSCManagerOverlays[49]);
				mvOpeningClashStartScene();
				break;

			case nSCKindOpeningSector:
				syDmaLoadOverlay(&dSCManagerOverlays[50]);
				mvOpeningSectorStartScene();
				break;

			case nSCKindOpeningJungle:
				syDmaLoadOverlay(&dSCManagerOverlays[3]);
				syDmaLoadOverlay(&dSCManagerOverlays[51]);
				mvOpeningJungleStartScene();
				break;

			case nSCKindOpeningNewcomers:
				syDmaLoadOverlay(&dSCManagerOverlays[52]);
				mvOpeningNewcomersStartScene();
				break;

			case nSCKind1PGamePlayers:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[27]);
				mnPlayers1PGameStartScene();
				break;

			case nSCKindPlayers1PTraining:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[28]);
				mnPlayers1PTrainingStartScene();
				break;
				
			case nSCKind1PBonus1Players:
#ifdef DAIRANTOU_OPT0
			case nSCKind1PBonus2Players:
#endif
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[29]);
				mnPlayers1PBonusStartScene();
				break;

#ifndef DAIRANTOU_OPT0
			case nSCKind1PBonus2Players:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[29]);
				mnPlayers1PBonusStartScene();
				break;
#endif
			case nSCKindBackupClear:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[53]);
				mnBackupClearStartScene();
				break;

			case nSCKindEnding:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[54]);
				mvEndingStartScene();
				break;

			case nSCKind1PContinue:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[55]);
				mnPlayers1PGameContinueStartScene();
				break;

			case nSCKind1PScoreUnk:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[56]);
				sc1PStageClearStartScene();
				break;

			case nSCKind1PStageClear:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
				syDmaLoadOverlay(&dSCManagerOverlays[56]);
				sc1PStageClearStartScene();
				break;

			case nSCKindStaffroll:
#if defined(REGION_US)
				syDmaLoadOverlay(&dSCManagerOverlays[59]);
#else
				syDmaLoadOverlay(&dSCManagerOverlays[57]);
#endif
				scStaffrollStartScene();
				break;

#if defined(REGION_US)
			case nSCKindCongra:
				syDmaLoadOverlay(&dSCManagerOverlays[57]);
				mnCongraStartScene();
				break;
#endif

			case nSCKindSoundTest:
				syDmaLoadOverlay(&dSCManagerOverlays[1]);
#if defined(REGION_US)
				syDmaLoadOverlay(&dSCManagerOverlays[62]);
#else
				syDmaLoadOverlay(&dSCManagerOverlays[60]);
#endif
				mnSoundTestStartScene();
				break;

			case nSCKindExplain:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[3]);
#if defined(REGION_US)
				syDmaLoadOverlay(&dSCManagerOverlays[63]);
#else
				syDmaLoadOverlay(&dSCManagerOverlays[61]);
#endif
				scExplainStartScene();
				break;

			case nSCKindAutoDemo:
				syDmaLoadOverlay(&dSCManagerOverlays[2]);
				syDmaLoadOverlay(&dSCManagerOverlays[3]);
#if defined(REGION_US)
				syDmaLoadOverlay(&dSCManagerOverlays[64]);
#else
				syDmaLoadOverlay(&dSCManagerOverlays[62]);
#endif
				scAutoDemoStartScene();
				break;
		}
	}
}

// 0x800A2698
void scManagerFuncUpdate(SYTaskmanSetup *arg)
{
	syTaskmanStartTask(arg);
}

// 0x800A26B8
void scManagerFuncDraw(void)
{
	gcDrawAll();
}

// 0x800A26D8
void scManagerMeterProcDisplay(GObj *gobj)
{
	s32 width;
	s32 unused;
	s32 pos_y = 203;

	func_80016338(gSYTaskmanDLHeads, CObjGetStruct(gobj), 0);

	gDPPipeSync(gSYTaskmanDLHeads[0]++);
	gDPSetCycleType(gSYTaskmanDLHeads[0]++, G_CYC_FILL);
	gDPSetRenderMode(gSYTaskmanDLHeads[0]++, G_RM_NOOP, G_RM_NOOP2);

	width = ((gLBParticleStructsUsedNum / 112.0F) * 256.0F);

	if (width < 0)
	{
		width = 0;
	}
	if (width > 256)
	{
		width = 256;
	}
	gDPSetFillColor(gSYTaskmanDLHeads[0]++, syVideoGetFillColor(GPACK_RGBA8888(0x00, 0x00, 0xFF, 0xFF)));
	gDPFillRectangle(gSYTaskmanDLHeads[0]++, 30, pos_y, width + 30, pos_y + 1);

	pos_y += 2;

	gDPPipeSync(gSYTaskmanDLHeads[0]++);

	width = ((gLBParticleGeneratorsUsedNum / 24.0F) * 256.0F);

	if (width < 0)
	{
		width = 0;
	}
	if (width > 256)
	{
		width = 256;
	}
	gDPSetFillColor(gSYTaskmanDLHeads[0]++, syVideoGetFillColor(GPACK_RGBA8888(0xFF, 0x40, 0x00, 0xFF)));
	gDPFillRectangle(gSYTaskmanDLHeads[0]++, 30, pos_y, width + 30, pos_y + 1);

	pos_y += 2;

	gDPPipeSync(gSYTaskmanDLHeads[0]++);

	width = ((gLBParticleTransformsUsedNum / 80.0F) * 256.0F);

	if (width < 0)
	{
		width = 0;
	}
	if (width > 256)
	{
		width = 256;
	}
	gDPSetFillColor(gSYTaskmanDLHeads[0]++, syVideoGetFillColor(GPACK_RGBA8888(0xFF, 0xFF, 0xFF, 0xFF)));
	gDPFillRectangle(gSYTaskmanDLHeads[0]++, 30, pos_y, width + 30, pos_y + 1);
	gDPPipeSync(gSYTaskmanDLHeads[0]++);

	// this needs to be in its own block to match. macro?
	// could explain the double sync
	{
		size_t mem_free = (size_t) ((uintptr_t)gSYTaskmanGeneralHeap.end - (uintptr_t)gSYTaskmanGeneralHeap.ptr);

		gDPSetFillColor(gSYTaskmanDLHeads[0]++, syVideoGetFillColor(GPACK_RGBA8888(0xFF, 0xFF, 0xFF, 0xFF)));
		func_800218E0(0x14, 0x14, mem_free, 7, 1);
		gDPPipeSync(gSYTaskmanDLHeads[0]++);
	}
	gDPPipeSync(gSYTaskmanDLHeads[0]++);
	gDPSetCycleType(gSYTaskmanDLHeads[0]++, G_CYC_1CYCLE);
	gDPSetRenderMode(gSYTaskmanDLHeads[0]++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
}

// 0x800A2B18
GObj* scManagerMakeMeterCamera(s32 link, u32 link_priority, u32 dl_link_priority)
{
	if (gcFindGObjByID(~0x10000000) != NULL)
	{
		return NULL;
	}
	else return gcMakeCameraGObj
	(
		~0x10000000,
		NULL,
		link,
		link_priority,
		scManagerMeterProcDisplay,
		dl_link_priority,
		0,
		0,
		FALSE,
		nGCProcessKindThread,
		NULL,
		0,
		FALSE
	);
}

// 0x800A2BA8
void scManagerMakeDebugCameras(s32 link, u32 link_priority, s32 dl_link_priority)
{
	GObj *gobj = gcFindGObjByID(~0x1);

	if (gobj != NULL)
	{
		gcEjectGObj(gobj);
	}
	else syDebugMakeMeterCamera(link, link_priority, dl_link_priority);

	gobj = gcFindGObjByID(~0x10000000);

	if (gobj != NULL)
	{
		gcEjectGObj(gobj);
	}
	else scManagerMakeMeterCamera(link, link_priority, dl_link_priority);
}

// 0x800A2C30
void scManagerInspectGObj(GObj *gobj)
{
    FTStruct *fp;
    WPStruct *wp;
    ITStruct *ip;
    EFStruct *ep;

    syDebugDebugPrintf("gobj id:%d:", gobj->id);

    switch (gobj->id)
    {
    case nGCCommonKindFighter:
        fp = ftGetStruct(gobj);

        syDebugDebugPrintf("fighter\n");
        syDebugDebugPrintf("kind:%d, player:%d, pkind:%d\n", fp->fkind, fp->player, fp->pkind);
        syDebugDebugPrintf("stat:%d, mstat:%d\n", fp->status_id, fp->motion_id);
        syDebugDebugPrintf("ga:%d\n", fp->ga);
        break;

    case nGCCommonKindWeapon:
        wp = wpGetStruct(gobj);

        syDebugDebugPrintf("weapon\n");
        syDebugDebugPrintf("kind:%d, player:%d\n", wp->kind, wp->player);
        syDebugDebugPrintf("atk stat:%d\n", wp->attack_coll.attack_state);
        syDebugDebugPrintf("ga:%d\n", wp->ga);
        break;

    case nGCCommonKindItem:
        ip = itGetStruct(gobj);

        syDebugDebugPrintf("item\n");
        syDebugDebugPrintf("kind:%d, player:%d\n", ip->kind, ip->player);
        syDebugDebugPrintf("atk stat:%d\n", ip->attack_coll.attack_state);
        syDebugDebugPrintf("ga:%d\n", ip->ga);
        syDebugDebugPrintf("proc update:%x\n", ip->proc_update);
        syDebugDebugPrintf("proc map:%x\n", ip->proc_map);
        syDebugDebugPrintf("proc hit:%x\n", ip->proc_hit);
        syDebugDebugPrintf("proc shield:%x\n", ip->proc_shield);
        syDebugDebugPrintf("proc hop:%x\n", ip->proc_hop);
        syDebugDebugPrintf("proc setoff:%x\n", ip->proc_setoff);
        syDebugDebugPrintf("proc reflector:%x\n", ip->proc_reflector);
        syDebugDebugPrintf("proc damage:%x\n", ip->proc_damage);
        break;

    case nGCCommonKindEffect:
        ep = efGetStruct(gobj);

        // Check if address is within base RDRAM + expansion pak bounds (why though!?)
#ifdef PORT
        if (ep != NULL)
#else
        if (((uintptr_t)ep >= 0x80000000) && ((uintptr_t)ep < 0x80800000))
#endif
        {
            syDebugDebugPrintf("effect\n");
            syDebugDebugPrintf("fgobj:%x", ep->fighter_gobj);
            syDebugDebugPrintf("proc func:%x\n", ep->proc_update);
        }
        else syDebugDebugPrintf("\n");
        break;

    default:
        syDebugDebugPrintf("\n");
        break;
    }
}

// 0x800A2E84
void scManagerFuncPrint(void)
{
	switch (dGCCurrentStatus)
	{
	case nGCStatusSystem:
		syDebugDebugPrintf("SYS\n");
		break;

	case nGCStatusRunning:
		syDebugDebugPrintf("BF\n");

		if (gGCCurrentCommon != NULL)
		{
			syDebugDebugPrintf("addr:%x\n", gGCCurrentCommon->func_run);
			scManagerInspectGObj(gGCCurrentCommon);
		}
		break;

	case nGCStatusProcessing:
		syDebugDebugPrintf("GP\n");

		if (gGCCurrentCommon != NULL)
		{
			if (gGCCurrentProcess != NULL)
			{
				switch (gGCCurrentProcess->kind)
				{
				case nGCProcessKindThread:
					syDebugDebugPrintf("thread:%x\n", gGCCurrentProcess->exec.gobjthread->thread.context.pc);
					break;

				case nGCProcessKindFunc:
					syDebugDebugPrintf("func:%x\n", gGCCurrentProcess->exec.func);
					break;
				}
			}
			scManagerInspectGObj(gGCCurrentCommon);
		}
		break;

	case nGCStatusCapturing:
		syDebugDebugPrintf("DFC\n");

		if (gGCCurrentCamera != NULL)
		{
			syDebugDebugPrintf("addr:%x\n", gGCCurrentCamera->proc_display);
			scManagerInspectGObj(gGCCurrentCamera);
		}
		break;
		
	case nGCStatusDisplaying:
		syDebugDebugPrintf("DFO\n");

		if (gGCCurrentCamera != NULL)
		{
			syDebugDebugPrintf("cam addr:%x\n", gGCCurrentCamera->proc_display);
		}
		if (gGCCurrentDisplay != NULL)
		{
			syDebugDebugPrintf("disp addr:%x\n", gGCCurrentDisplay->proc_display);
			scManagerInspectGObj(gGCCurrentDisplay);
		}
		break;
	}
}

// 0x800A3040
void scManagerRunPrintGObjStatus(void)
{
	syDebugRunFuncPrint(scManagerFuncPrint);
}

#ifdef PORT
/* In-game reset for the port's ESC-menu Reset button / Ctrl+Cmd-R console
 * "reset" command (registered in port/port.cpp). Mirrors what every scene's
 * own exit path does (see sc1pintro.c, sc1pstageclear.c, scvsbattle.c):
 * wind down audio, point scene_curr at the target scene, and ask the task
 * manager to end the current scene at the frame boundary. scManagerRunLoop
 * then runs its normal scene-boundary cleanup (GObj sweep, stale-pointer
 * nulling) before entering the target — the same code path as any regular
 * scene transition.
 *
 * The reset is STICKY (sPortResetPending + target) because 1P mode runs a
 * nested scene chain: sc1PManagerUpdateScene dispatches sub-scenes back to
 * back and rewrites scene_curr itself between them, so a plain scene_curr
 * write gets clobbered and the manager chains into the next stage intro
 * against half-torn-down battle state (crashed in
 * ftManagerSetupFilesAllKind). sc1pmanager.c checks
 * portSCManagerConsumeReset() after every sub-scene call and bails out to
 * scManagerRunLoop; scManagerRunLoop consumes any remaining pending reset
 * at the top of its dispatch loop (the top-level-scene case).
 *
 * The target mirrors the port's boot flow: nSCKindOpeningRoom (the port
 * skips the ~3 s nSCKindStartup — see scManagerRunLoop), overridden to the
 * VS character select when the Boot-to-CSS enhancement is active, exactly
 * like boot. Safe to call from the ImGui/menu context: the port runs game
 * and UI cooperatively on one coroutine, and the writes here are plain
 * stores that taskman/scmanager consume at the next frame boundary. */
static ub32 sPortResetPending = FALSE;
static s32 sPortResetTarget = nSCKindOpeningRoom;

void portSCManagerRequestReset(void)
{
	extern void func_800266A0_272A0(void); /* audio wind-down used by every scene exit */
	extern int port_enhancement_boot_to_vs_css(void);

	sPortResetTarget = port_enhancement_boot_to_vs_css() ? nSCKindPlayersVS : nSCKindOpeningRoom;
	sPortResetPending = TRUE;

	/* Speak the game's native match-abort language: is_reset is what the
	 * original A+B+R+Z combo sets (ifcommon.c), and every battle-end flow
	 * already honors it — scvsbattle.c:684 skips sudden death, the VS
	 * results and 1P bonus/challenger flows short-circuit. Without it, a
	 * forced mid-match exit reads as "time up, tied score" and VS battles
	 * chain into sudden death. Each battle start clears it, so no staleness. */
	gSCManagerSceneData.is_reset = TRUE;

	func_800266A0_272A0();

	/* Boot-like state: prev == curr == target (same as the default scene
	 * data and the Boot-to-CSS override at the top of scManagerRunLoop). */
	gSCManagerSceneData.scene_prev = sPortResetTarget;
	gSCManagerSceneData.scene_curr = sPortResetTarget;

	syTaskmanSetLoadScene();
}

/* If a reset is pending: re-apply the target scene (undoing any scene_curr
 * writes a nested manager made since the request), clear the pending flag,
 * and return TRUE so nested managers know to unwind to scManagerRunLoop. */
sb32 portSCManagerConsumeReset(void)
{
	if (!sPortResetPending)
	{
		return FALSE;
	}
	sPortResetPending = FALSE;

	gSCManagerSceneData.scene_prev = sPortResetTarget;
	gSCManagerSceneData.scene_curr = sPortResetTarget;

	return TRUE;
}
#endif

#ifdef HANDHELD_PORT
/* handheld save states (M7): after a load, the renderer state the scene start sets from the game's
 * scene (the opening movie's crop, above) follows the restored scene, not the one this process was
 * in. Called by gfx_pc.c's gfx_savestate_fixup. */
void port_scene_renderer_sync(void)
{
	extern void gfx_set_movie_frame(int on);
	gfx_set_movie_frame(gSCManagerSceneData.scene_curr >= nSCKindOpeningRoom &&
	                    gSCManagerSceneData.scene_curr <= nSCKindOpeningNewcomers);
}
#endif
