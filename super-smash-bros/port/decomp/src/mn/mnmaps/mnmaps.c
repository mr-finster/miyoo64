#include <mn/menu.h>
#include <ft/fighter.h>
#include <gr/ground.h>
// #include <gm/gmsound.h> // temporary, until this finds a proper place (included from fighter.h)
#include <sc/scene.h>
#include <sys/video.h>
#include <sys/controller.h>
#include <sys/rdp.h>
#include <reloc_data.h>
#ifdef PORT
extern void *func_800269C0_275C0(u16 id);
// Port-built CSS sprites (baked into the binary at compile time). Declared as
// `extern` rather than a header include because the port symbols live outside
// the decomp include path; the linker resolves them from port/css_icons/*.cpp.
// Generic per-stage CSS asset getters (port/css_icons/port_css_stage_assets.cpp).
// Backgrounds and icons load a PNG override from <app-data>/assets/css_icons/
// when present (dev builds stage these at build time via
// tools/derive_stage_assets.py) and otherwise derive from the wallpaper Sprite
// in the user's extracted .o2r archive. Nameplates/emblems are PNG-only.
// Return NULL when nothing is available; the caller falls back to ROM data.
extern Sprite *portCSSGetStageIconSprite(int gkind);
extern Sprite *portCSSGetStageBackgroundSprite(int gkind);
extern Sprite *portCSSGetStageNameSprite(int gkind);
extern Sprite *portCSSGetStageEmblemSprite(int gkind);
extern Sprite *portCSSGetScrollArrowSprite(void);
extern Sprite *portCSSGetScrollArrowLeftSprite(void);
extern float port_widescreen_clip_x_scale(void);
// LUS menu toggle (port/enhancements/BonusStages.cpp, issue #267): non-zero
// when the port-added bonus stage page should be available. Read ONLY by the
// mnMapsInitVars snapshot (sMNMapsBonusStagesEnabled) so the lock state is
// consistent for the whole screen session; a menu change applies next open.
extern int port_enhancement_bonus_stages_enabled(void);
#endif

#define nMNMapsSlotRandom	9
#define nMNMapsSlotCount	10
#define nMNMapsRandomGKind	0xDE
#define nMNMapsFileMasterHandIcon	5

#ifdef PORT
// PORT: empty-slot sentinel for pages that don't fill all 10 grid positions.
// mnMapsCheckLocked treats it as locked, so mnMapsMakeIcons skips it and
// cursor-movement helpers skip past it.
#define nMNMapsEmptyGKind	0xDD

#define nMNMapsPageOriginal	0
#define nMNMapsPagePort		1
#define nMNMapsPageCount	2

// Foreground-slide page-transition animation. Background bitmap (stone tiles)
// stays put; the icons GObj and the cursor slide as a unit, GBC-style.
//
//   nMNMapsScrollFrames: total frames the slide runs. 320 px / 16 frames = 20 px/frame.
//   The shorter the slide, the snappier; too short and it just looks like a jump.
#define nMNMapsScrollFrames		16
#define nMNMapsScrollPageWidth	320.0F

#define nMNMapsScrollDirNone	0
#define nMNMapsScrollDirRight	1   // jumping to page+1, contents slide LEFT
#define nMNMapsScrollDirLeft	2   // jumping to page-1, contents slide RIGHT
#endif


// // // // // // // // // // // //
//                               //
//       INITIALIZED DATA        //
//                               //
// // // // // // // // // // // //

// 0x801344D0
u32 dMNMapsFileIDs[/* */] =
{
#ifdef PORT
	llFTEmblemSpritesFileID,
	llMNSelectCommonFileID,
	llMNMapsFileID,
	llMNCommonFontsFileID,
	llGRWallpaperTrainingBlackFileID,
	llMasterHandIconFileID
#else
	&llFTEmblemSpritesFileID,
	&llMNSelectCommonFileID,
	&llMNMapsFileID,
	&llMNCommonFontsFileID,
	&llGRWallpaperTrainingBlackFileID,
	&llMasterHandIconFileID
#endif
};

// 0x801344E4
GRFileInfo dMNMapsFileInfos[/* */] =
{
#ifdef PORT
	{ llGRCastleMapFileID,   llGRCastleMapMapHeader },
	{ llGRSectorMapFileID,   llGRSectorMapMapHeader },
	{ llGRJungleMapFileID,   llGRJungleMapMapHeader },
	{ llGRZebesMapFileID,    llGRZebesMapMapHeader },
	{ llGRHyruleMapFileID,   llGRHyruleMapMapHeader },
	{ llGRYosterMapFileID,   llGRYosterMapMapHeader },
	{ llGRPupupuMapFileID,   llGRPupupuMapMapHeader },
	{ llGRYamabukiMapFileID, llGRYamabukiMapMapHeader },
	{ llGRInishieMapFileID,  llGRInishieMapMapHeader },
	{ llGRPupupuSmallMapFileID, llGRPupupuSmallMapMapHeader },
	{ llGRPupupuTestMapFileID, llGRPupupuTestMapMapHeader },
	{ llGRExplainMapFileID, llGRExplainMapMapHeader },
	{ llGRYosterSmallMapFileID, llGRYosterSmallMapMapHeader },
	{ llGRMetalMapFileID, llGRMetalMapMapHeader },
	{ llGRZakoMapFileID, llGRZakoMapMapHeader },
	{ llGRBonus3MapFileID, llGRBonus3MapMapHeader },
	{ llGRLastMapFileID, llGRLastMapMapHeader }
#else
	{ &llGRCastleMapFileID,   &llGRCastleMapMapHeader },
	{ &llGRSectorMapFileID,   &llGRSectorMapMapHeader },
	{ &llGRJungleMapFileID,   &llGRJungleMapMapHeader },
	{ &llGRZebesMapFileID,    &llGRZebesMapMapHeader },
	{ &llGRHyruleMapFileID,   &llGRHyruleMapMapHeader },
	{ &llGRYosterMapFileID,   &llGRYosterMapMapHeader },
	{ &llGRPupupuMapFileID,   &llGRPupupuMapMapHeader },
	{ &llGRYamabukiMapFileID, &llGRYamabukiMapMapHeader },
	{ &llGRInishieMapFileID,  &llGRInishieMapMapHeader },
	{ &llGRPupupuSmallMapFileID, &llGRPupupuSmallMapMapHeader },
	{ &llGRPupupuTestMapFileID, &llGRPupupuTestMapMapHeader },
	{ &llGRExplainMapFileID, &llGRExplainMapMapHeader },
	{ &llGRYosterSmallMapFileID, &llGRYosterSmallMapMapHeader },
	{ &llGRMetalMapFileID, &llGRMetalMapMapHeader },
	{ &llGRZakoMapFileID, &llGRZakoMapMapHeader },
	{ &llGRBonus3MapFileID, &llGRBonus3MapMapHeader },
	{ &llGRLastMapFileID, &llGRLastMapMapHeader }
#endif
};

// 0x8013452C
intptr_t dMNMapsWallpaperOffsets[/* */] =
{
	0x00026C88, 0x00026C88, 0x00026C88,
	0x00026C88, 0x00026C88, 0x00026C88,
	0x00026C88, 0x00026C88, 0x00026C88,
	0x00026C88, 0x00026C88, 0x00026C88,
	0x00026C88, 0x00026C88, 0x00026C88,
	0x00026C88, 0x00026C88
};

// 0x80134550
GRFileInfo dMNMapsTrainingModeFileInfos[/* */] =
{
#ifdef PORT
	{ llGRWallpaperTrainingBlackFileID, 0x00000000 },
	{ llGRWallpaperTrainingYellowFileID,0xEE9E0600 },
	{ llGRWallpaperTrainingBlueFileID, 	0xAFF5FF00 }
#else
	{ &llGRWallpaperTrainingBlackFileID, 0x00000000 },
	{ &llGRWallpaperTrainingYellowFileID,0xEE9E0600 },
	{ &llGRWallpaperTrainingBlueFileID, 	0xAFF5FF00 }
#endif
};

// 0x80134568
s32 dMNMapsTrainingModeWallpaperIDs[/* */] = { 2, 0, 0, 0, 2, 1, 2, 2, 2, 0, 2, 2, 1, 2, 2, 2, 2 };

// 0x80134590
Lights1 dMNMapsLights1 = gdSPDefLights1(0x20, 0x20, 0x20, 0xFF, 0xFF, 0xFF, 0x14, 0x14, 0x14);

// 0x801345A8
Gfx dMNMapsDisplayList[/* */] =
{
	gsSPSetGeometryMode(G_LIGHTING),
	gsSPSetLights1(dMNMapsLights1),
	gsSPEndDisplayList()
};

// // // // // // // // // // // //
//                               //
//   GLOBAL / STATIC VARIABLES   //
//                               //
// // // // // // // // // // // //

// 0x80134BD0
s32 sMNMapsPad0x80134BD0[2];

// 0x80134BD8
s32 sMNMapsCursorSlot;

// 0x80134BDC
GObj *sMNMapsCursorGObj;

// 0x80134BE0
GObj *sMNMapsNameLogoGObj;

// 0x80134BE4
GObj *sMNMapsHeap0WallpaperGObj;

// 0x80134BE8
GObj *sMNMapsHeap1WallpaperGObj;

// 0x80134BF0
GObj *sMNMapsHeap0LayerGObjs[4];

// 0x80134C00
GObj *sMNMapsHeap1LayerGObjs[4];

// 0x80134C10
MPGroundData *sMNMapsGroundInfo;

// 0x80134C14;
CObj *sMNMapsPreviewCObj;

// 0x80134C18
sb32 sMNMapsIsTrainingMode;

// 0x80134C1C - // flag indicating which bonus features are available
u8 sMNMapsUnlockedMask;

// 0x80134C20
s32 sMNMapsHeapID;

// 0x80134C24 - Frames elapsed on SSS
s32 sMNMapsTotalTimeTics;

// 0x80134C28 - Frames until cursor can be moved again
s32 sMNMapsScrollWait;

// 0x80134C2C - Frames to wait until exiting Stage Select
s32 sMNMapsReturnTic;

// 0x80134C30
LBFileNode sMNMapsStatusBuffer[30];

// 0x80134D20
LBFileNode sMNMapsForceStatusBuffer[30];

// 0x80134E10
void *sMNMapsFiles[ARRAY_COUNT(dMNMapsFileIDs)];

// 0x80134E24
void *sMNMapsModelHeap0;

// 0x80134E28
void *sMNMapsModelHeap1;

#ifdef PORT
// PORT: current CSS page (0 = original 9-stage layout, 1+ = port expansion pages).
// Cursor moving right past page 0's rightmost slot transitions to page 1 etc.
s32 sMNMapsCursorPage;

// Tracks the icons-layer GObj so a page transition can rebuild it without leaking.
GObj *sMNMapsIconsGObj;

// Tracks the page-scroll arrow GObj. Re-built whenever the page changes so the
// arrow visibility (left and/or right side) reflects which neighbor pages exist.
// Arrows do NOT slide with the icons during a page transition — they're page-state
// indicators, not page content.
GObj *sMNMapsArrowsGObj;

// Page-slide animation state (foreground-only slide, background stays).
//   sMNMapsScrollDir          : 0 (idle) / Right / Left.
//   sMNMapsScrollTicks        : frames remaining in current slide (counts down).
//   sMNMapsScrollOldIconsGObj : outgoing page's icons GObj (sliding off-screen).
//   sMNMapsScrollOldArrowsGObj: outgoing page's arrows GObj (sliding off-screen).
//                                The "new" arrows live in sMNMapsArrowsGObj and
//                                are built off-screen at slide start, same as icons.
//   sMNMapsScrollIconsDelta   : per-frame x-delta applied to icons + arrows GObjs.
//   sMNMapsScrollCursorDelta  : per-frame x-delta applied to the cursor.
s32 sMNMapsScrollDir;
s32 sMNMapsScrollTicks;
GObj *sMNMapsScrollOldIconsGObj;
GObj *sMNMapsScrollOldArrowsGObj;
f32 sMNMapsScrollIconsDelta;
f32 sMNMapsScrollCursorDelta;

// Base "look-at" Y the preview camera oscillates around. The original ROM
// implementation captured this value once on thread start (f32 y = cobj->vec.at.y
// in mnMapsPreviewCameraThreadUpdate); switching stages writes the new gkind's
// at.y but the thread immediately overwrites it next frame with sin(deg)*40 + y,
// so the camera kept orbiting whichever stage's y was loaded when the thread
// was first created. The visible regression: enter VS from a low-y stage like
// Hyrule (positions[].y=1500), return to CSS, and every stage now renders with
// camera at.y≈1500 — Castle (expects 1800) appears noticeably higher on screen.
// SetPreviewCameraPosition writes this global; the thread reads it each frame.
f32 sMNMapsPreviewBaseAtY;

// Per-page gkind tables. Each row is exactly 10 slots (5 row 1 + 5 row 2, with the
// last entry being the random/page-specific terminator). nMNMapsEmptyGKind marks a
// slot with no stage on this page — mnMapsCheckLocked treats it as locked so the
// icon isn't drawn and the cursor skips past it.
s32 dMNMapsPageGkinds[nMNMapsPageCount][nMNMapsSlotCount] =
{
	// Page 0 — original ROM-shipping layout (unchanged from N64 release).
	{
		nGRKindCastle, nGRKindJungle, nGRKindHyrule, nGRKindZebes, nGRKindInishie,
		nGRKindYoster, nGRKindPupupu, nGRKindSector, nGRKindYamabuki, nMNMapsRandomGKind,
	},
	// Page 1 — port expansion. Three playable stages from the 1P/Master-Hand pool:
	//   row 1 (slots 0..4): Final Destination, Metal Cavern, Battlefield
	//   row 2 (slots 5..9): empty (page-jump fallback finds row-1 entries instead)
	{
		nGRKindLast,       nGRKindMetal,      nGRKindZako,      nMNMapsEmptyGKind, nMNMapsEmptyGKind,
		nMNMapsEmptyGKind, nMNMapsEmptyGKind, nMNMapsEmptyGKind, nMNMapsEmptyGKind, nMNMapsEmptyGKind,
	},
};

static SObj *sMNMapsStageSelectTextSObj = NULL; // Tracks the cursive image
static GObj *sMNMapsMusicSelectTextGObj = NULL; // Tracks our new text

// Snapshot of the LUS "Bonus Stages" toggle, latched by mnMapsInitVars each
// time this screen opens. All in-scene lock checks read the snapshot, never
// the live CVar — otherwise a mid-screen toggle from the overlay menu
// half-applies (icons and arrows are built only at open/page-slide, but the
// lock checks gate cursor movement and stage confirm every frame, so a live
// read could freeze the cursor on a suddenly-all-locked page or let A select
// a stage whose icon just vanished). The toggle lands on the next rebuild.
static sb32 sMNMapsBonusStagesEnabled = TRUE;

s32 mnMapsGetPageForGkind(s32 gkind); // defined below; used by mnMapsCheckLocked
#endif

// // // // // // // // // // // //
//                               //
//           FUNCTIONS           //
//                               //
// // // // // // // // // // // //

// music selection screen hookup
#ifdef PORT
extern void port_enhancement_music_select_reset(void);
extern s32 port_enhancement_music_select_handle_a(u32 bgm_id);
extern s32 port_enhancement_music_select_handle_b(void);

// Helper to recolor the cursor. Native C handles the SObj pointer easily.
void mnMapsSetCursorColor(u8 r, u8 g, u8 b)
{
	if (sMNMapsCursorGObj != NULL)
	{
		SObjGetStruct(sMNMapsCursorGObj)->sprite.red = r;
		SObjGetStruct(sMNMapsCursorGObj)->sprite.green = g;
		SObjGetStruct(sMNMapsCursorGObj)->sprite.blue = b;
	}
}

// Maps the Stage ID directly to the raw integer BGM ID to avoid header conflicts
u32 mnMapsGetBGMForGKind(s32 gkind)
{
	switch (gkind)
	{
		case nGRKindCastle:   return 6;
		case nGRKindSector:   return 4;
		case nGRKindJungle:   return 5;
		case nGRKindZebes:    return 1;
		case nGRKindHyrule:   return 9;
		case nGRKindYoster:   return 8;
		case nGRKindPupupu:   return 0;
		case nGRKindYamabuki: return 7;
		case nGRKindInishie:  return 2;
		case nGRKindMetal:    return 37;
		case nGRKindZako:     return 36;
		case nGRKindLast:     return 25;
		case nMNMapsRandomGKind: return 0xFFFF; // Special random flag
		default:              return 0;
	}
}

// Swaps the text between "Stage Select" and "MUSIC SELECT"
void mnMapsSetLabelState(sb32 is_music_select)
{
	if (sMNMapsStageSelectTextSObj != NULL)
	{
		if (is_music_select)
		{
			// Move the cursive "Stage Select" off-screen
			sMNMapsStageSelectTextSObj->pos.x = -1000.0F;

			// Generate the blocky "MUSIC SELECT" text
			if (sMNMapsMusicSelectTextGObj == NULL)
			{
				u32 color[3] = { 0x00, 0xAA, 0xFF }; // Match the blue cursor!
				sMNMapsMusicSelectTextGObj = gcMakeGObjSPAfter(0, NULL, 6, GOBJ_PRIORITY_DEFAULT);
				gcAddGObjDisplay(sMNMapsMusicSelectTextGObj, mnMapsLabelsProcDisplay, 4, GOBJ_PRIORITY_DEFAULT, ~0);

				// Draw the text exactly where the cursive image used to be
				mnMapsMakeString(sMNMapsMusicSelectTextGObj, "MUSIC SELECT", 205.0F, 128.0F, color);
			}
		}
		else
		{
			// Bring the cursive "Stage Select" back to its original position
			sMNMapsStageSelectTextSObj->pos.x = 172.0F;

			// Destroy the "MUSIC SELECT" text
			if (sMNMapsMusicSelectTextGObj != NULL)
			{
				gcEjectGObj(sMNMapsMusicSelectTextGObj);
				sMNMapsMusicSelectTextGObj = NULL;
			}
		}
	}
}
#endif

// 0x80131B00
void mnMapsAllocModelHeaps(void)
{
	size_t size, max = 0;
	s32 i;

	for (i = 0; i < ARRAY_COUNT(dMNMapsFileInfos); i++)
	{
		size = lbRelocGetFileSize(dMNMapsFileInfos[i].file_id);

		if (max < size)
		{
			max = size;
		}
	}
	sMNMapsModelHeap0 = syTaskmanMalloc(max, 0x10);
	sMNMapsModelHeap1 = syTaskmanMalloc(max, 0x10);
}

// 0x80131B88
void mnMapsFuncLights(Gfx **dls)
{
	gSPDisplayList(dls[0]++, dMNMapsDisplayList);
}

// 0x80131BAC
sb32 mnMapsCheckLocked(s32 gkind)
{
#ifdef PORT
	if (gkind == nMNMapsEmptyGKind)
	{
		// Empty page slot — no stage to show. Treated as locked so mnMapsMakeIcons
		// skips it and cursor-movement helpers walk past it.
		return TRUE;
	}
	if (!sMNMapsBonusStagesEnabled && mnMapsGetPageForGkind(gkind) > nMNMapsPageOriginal)
	{
		// Port-added bonus stages, hideable via the LUS "Bonus Stages" toggle
		// (issue #267 — vanilla stage roster). Membership is derived from the
		// page table — every stage beyond page 0 is a bonus stage — so a stage
		// added to dMNMapsPageGkinds later automatically honors the toggle.
		// Locked here covers everything at once: no icons, no page-jump landing
		// slot (so the page is unreachable), no random-selection candidacy, and
		// mnMapsInitVars reroutes a restored cursor back to page 0. Reads the
		// per-screen snapshot, not the live CVar — see sMNMapsBonusStagesEnabled.
		return TRUE;
	}
#endif
	if (gkind == nGRKindInishie)
	{
		if (sMNMapsUnlockedMask & LBBACKUP_UNLOCK_MASK_INISHIE)
		{
			return FALSE;
		}
		else return TRUE;
	}
	else return FALSE;
}

// 0x80131BB0
void mnMapsGetSlotPosition(s32 slot, f32 *x, f32 *y)
{
	// Original ROM layout: 5 icons per row, step 50, row 1 y=30, row 2 y=68.
	// The original mnMapsMakeIcons inlined this as `x + 30` (row 1) and `x - 220`
	// (row 2 with `x = i * 50`); we share the same math through a helper so the
	// cursor and per-page rebuild paths stay in sync.
	if (slot < 5)
	{
		*x = (slot * 50) + 30;
		*y = 30.0F;
	}
	else
	{
		*x = ((slot - 5) * 50) + 30;
		*y = 68.0F;
	}
}

// 0x80131BE4
s32 mnMapsGetCharacterID(const char c)
{
	switch (c)
	{
	case '\'':
		return 0x1A;

	case '%':
		return 0x1B;

	case '.':
		return 0x1C;

	case ' ':
		return 0x1D;

	default:
		if ((c < 'A') || (c > 'Z'))
		{
			return 0x1D;
		}
		else return c - 'A';
	}
}

// 0x80131C5C
f32 mnMapsGetCharacterSpacing(const char *str, s32 c)
{
	switch (str[c])
	{
	case 'A':
		switch (str[c + 1])
		{
		case 'F':
		case 'P':
		case 'T':
		case 'V':
		case 'Y':
			return 0.0F;

		default:
			return 1.0F;
		}
		break;

	case 'F':
	case 'P':
	case 'V':
	case 'Y':
		switch(str[c + 1])
		{
		case 'A':
		case 'T':
			return 0.0F;

		default:
			return 1.0F;
		}
		break;

	case 'Q':
	case 'T':
		switch(str[c + 1])
		{
		case '\'':
		case '.':
			return 1.0F;

		default:
			return 0.0F;
		}
		break;

	case '\'':
		return 1.0F;

	case '.':
		return 1.0F;

	default:
		switch(str[c + 1])
		{
		case 'T':
			return 0.0F;

		default:
			return 1.0F;
		}
		break;
	}
}

// 0x80131D80 - Unused?
void mnMapsMakeString(GObj *gobj, const char *str, f32 x, f32 y, u32 *color)
{
	intptr_t chars[/* */] =
	{
#ifdef PORT
		llMNCommonFontsLetterASprite, llMNCommonFontsLetterBSprite,
		llMNCommonFontsLetterCSprite, llMNCommonFontsLetterDSprite,
		llMNCommonFontsLetterESprite, llMNCommonFontsLetterFSprite,
		llMNCommonFontsLetterGSprite, llMNCommonFontsLetterHSprite,
		llMNCommonFontsLetterISprite, llMNCommonFontsLetterJSprite,
		llMNCommonFontsLetterKSprite, llMNCommonFontsLetterLSprite,
		llMNCommonFontsLetterMSprite, llMNCommonFontsLetterNSprite,
		llMNCommonFontsLetterOSprite, llMNCommonFontsLetterPSprite,
		llMNCommonFontsLetterQSprite, llMNCommonFontsLetterRSprite,
		llMNCommonFontsLetterSSprite, llMNCommonFontsLetterTSprite,
		llMNCommonFontsLetterUSprite, llMNCommonFontsLetterVSprite,
		llMNCommonFontsLetterWSprite, llMNCommonFontsLetterXSprite,
		llMNCommonFontsLetterYSprite, llMNCommonFontsLetterZSprite,
#else
		&llMNCommonFontsLetterASprite, &llMNCommonFontsLetterBSprite,
		&llMNCommonFontsLetterCSprite, &llMNCommonFontsLetterDSprite,
		&llMNCommonFontsLetterESprite, &llMNCommonFontsLetterFSprite,
		&llMNCommonFontsLetterGSprite, &llMNCommonFontsLetterHSprite,
		&llMNCommonFontsLetterISprite, &llMNCommonFontsLetterJSprite,
		&llMNCommonFontsLetterKSprite, &llMNCommonFontsLetterLSprite,
		&llMNCommonFontsLetterMSprite, &llMNCommonFontsLetterNSprite,
		&llMNCommonFontsLetterOSprite, &llMNCommonFontsLetterPSprite,
		&llMNCommonFontsLetterQSprite, &llMNCommonFontsLetterRSprite,
		&llMNCommonFontsLetterSSprite, &llMNCommonFontsLetterTSprite,
		&llMNCommonFontsLetterUSprite, &llMNCommonFontsLetterVSprite,
		&llMNCommonFontsLetterWSprite, &llMNCommonFontsLetterXSprite,
		&llMNCommonFontsLetterYSprite, &llMNCommonFontsLetterZSprite,
#endif

#ifdef PORT
		llMNCommonFontsSymbolApostropheSprite,
		llMNCommonFontsSymbolPercentSprite,
		llMNCommonFontsSymbolPeriodSprite
#else
		&llMNCommonFontsSymbolApostropheSprite,
		&llMNCommonFontsSymbolPercentSprite,
		&llMNCommonFontsSymbolPeriodSprite
#endif
	};
	SObj *sobj;
	f32 start_x = x;
	s32 i;

	for (i = 0; str[i] != 0; i++)
	{
		if (((((str[i] >= '0') && (str[i] <= '9')) ? TRUE : FALSE)) || (str[i] == ' '))
		{
			if (str[i] == ' ')
			{
				start_x += 4.0F;
			}
			else start_x += str[i] - '0';
		}
		else
		{
			sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[3], chars[mnMapsGetCharacterID(str[i])]));
			sobj->pos.x = start_x;

			start_x += sobj->sprite.width + mnMapsGetCharacterSpacing(str, i);

			switch (str[i])
			{
			case '\'':
				sobj->pos.y = y - 1.0F;
				break;
			
			case '.':
				sobj->pos.y = y + 4.0F;
				break;
				
			default:
				sobj->pos.y = y;
				break;
			}
			sobj->sprite.attr &= ~SP_FASTCOPY;
			sobj->sprite.attr |= SP_TRANSPARENT;

			sobj->sprite.red = color[0];
			sobj->sprite.green = color[1];
			sobj->sprite.blue = color[2];
		}
	}
}

// 0x80131FA4
void mnMapsMakeWallpaper(void)
{
	GObj *gobj;
	SObj *sobj;

	gobj = gcMakeGObjSPAfter(0, NULL, 2, GOBJ_PRIORITY_DEFAULT);
	gcAddGObjDisplay(gobj, lbCommonDrawSObjAttr, 0, GOBJ_PRIORITY_DEFAULT, ~0);
	
#ifdef PORT
	sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[1], llMNSelectCommonStoneBackgroundSprite));
#else
	sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[1], &llMNSelectCommonStoneBackgroundSprite));
#endif

	sobj->cms = G_TX_WRAP;
	sobj->cmt = G_TX_WRAP;

	sobj->masks = 6;
	sobj->maskt = 5;

	sobj->lrs = 300;
	sobj->lrt = 220;

	sobj->pos.x = 10.0F;
	sobj->pos.y = 10.0F;
}

// 0x80132048
void mnMapsMakePlaque(void)
{
	GObj *gobj;
	SObj *sobj;

	gobj = gcMakeGObjSPAfter(0, NULL, 8, GOBJ_PRIORITY_DEFAULT);
	gcAddGObjDisplay(gobj, lbCommonDrawSObjAttr, 6, GOBJ_PRIORITY_DEFAULT, ~0);

#ifdef PORT
	sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], llMNMapsWoodenCircleSprite));
#else
	sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], &llMNMapsWoodenCircleSprite));
#endif
	sobj->sprite.attr &= ~SP_FASTCOPY;
	sobj->sprite.attr |= SP_TRANSPARENT;

	sobj->pos.x = 189.0F;
	sobj->pos.y = 124.0F;
}

// 0x801320E0
void mnMapsLabelsProcDisplay(GObj *gobj)
{
	gDPPipeSync(gSYTaskmanDLHeads[0]++);
	gDPSetCycleType(gSYTaskmanDLHeads[0]++, G_CYC_1CYCLE);
	gDPSetPrimColor(gSYTaskmanDLHeads[0]++, 0, 0, 0x57, 0x60, 0x88, 0xFF);
	gDPSetCombineMode(gSYTaskmanDLHeads[0]++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
	gDPSetRenderMode(gSYTaskmanDLHeads[0]++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
	gDPSetPrimColor(gSYTaskmanDLHeads[0]++, 0, 0, 0x57, 0x60, 0x88, 0xFF);
	gDPFillRectangle(gSYTaskmanDLHeads[0]++, 160, 128, 320, 134);
	gDPSetPrimColor(gSYTaskmanDLHeads[0]++, 0, 0, 0x00, 0x00, 0x00, 0x33);
	gDPFillRectangle(gSYTaskmanDLHeads[0]++, 194, 189, 268, 193);
	gDPPipeSync(gSYTaskmanDLHeads[0]++);
	gDPSetRenderMode(gSYTaskmanDLHeads[0]++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
	gDPSetCycleType(gSYTaskmanDLHeads[0]++, G_CYC_1CYCLE);

	lbCommonClearExternSpriteParams();
	lbCommonDrawSObjAttr(gobj);
}

// 0x80132288
void mnMapsMakeLabels(void)
{
	GObj *gobj;
	SObj *sobj;
	s32 x;

	gobj = gcMakeGObjSPAfter(0, NULL, 6, GOBJ_PRIORITY_DEFAULT);
	gcAddGObjDisplay(gobj, mnMapsLabelsProcDisplay, 4, GOBJ_PRIORITY_DEFAULT, ~0);

#ifdef PORT
	sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], llMNMapsStageSelectTextSprite));
	sMNMapsStageSelectTextSObj = sobj; // save the pointer so we can move it off-screen later
#else
	sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], &llMNMapsStageSelectTextSprite));
#endif
	sobj->sprite.attr &= ~SP_FASTCOPY;
	sobj->sprite.attr |= SP_TRANSPARENT;

	sobj->envcolor.r = 0x00;
	sobj->envcolor.g = 0x00;
	sobj->envcolor.b = 0x00;

	sobj->sprite.red = 0xAF;
	sobj->sprite.green = 0xB1;
	sobj->sprite.blue = 0xCC;

	sobj->pos.x = 172.0F;
	sobj->pos.y = 122.0F;

#ifdef PORT
	sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], llMNMapsPlateLeftSprite));
#else
	sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], &llMNMapsPlateLeftSprite));
#endif
	sobj->sprite.attr &= ~SP_FASTCOPY;
	sobj->sprite.attr |= SP_TRANSPARENT;

	sobj->pos.x = 174.0F;
	sobj->pos.y = 191.0F;

	for (x = 186; x < 262; x += 4)
	{
#ifdef PORT
		sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], llMNMapsPlateMiddleSprite));
#else
		sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], &llMNMapsPlateMiddleSprite));
#endif
		sobj->pos.x = x;
		sobj->pos.y = 191.0F;
	}
#ifdef PORT
	sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], llMNMapsPlateRightSprite));
#else
	sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], &llMNMapsPlateRightSprite));
#endif
	sobj->sprite.attr &= ~SP_FASTCOPY;
	sobj->sprite.attr |= SP_TRANSPARENT;

	sobj->pos.x = 262.0F;
	sobj->pos.y = 191.0F;
}

// 0x80132430
s32 mnMapsGetGroundKind(s32 slot)
{
#ifdef PORT
	// Page table drives this directly — Final Destination lives on page 1, the
	// original 9 stages + random on page 0, and any future expansion stages drop
	// into dMNMapsPageGkinds without churning this function.
	return dMNMapsPageGkinds[sMNMapsCursorPage][slot];
#else
	s32 gkinds[/* */] =
	{
		nGRKindCastle, nGRKindJungle, nGRKindHyrule, nGRKindZebes, nGRKindInishie,
		nGRKindYoster, nGRKindPupupu, nGRKindSector, nGRKindYamabuki
	};

	if (slot == nMNMapsSlotRandom)
	{
		return nMNMapsRandomGKind;
	}
	return gkinds[slot];
#endif
}

#ifdef PORT
// PORT: locate which page a gkind lives on. Returns -1 if not in any page table.
// Used by mnMapsInitVars to restore the cursor across sessions.
s32 mnMapsGetPageForGkind(s32 gkind)
{
	s32 page;
	s32 slot;

	for (page = 0; page < nMNMapsPageCount; page++)
	{
		for (slot = 0; slot < nMNMapsSlotCount; slot++)
		{
			if (dMNMapsPageGkinds[page][slot] == gkind)
			{
				return page;
			}
		}
	}
	return -1;
}
#endif

// 0x80132498
s32 mnMapsGetSlot(s32 gkind)
{
#ifdef PORT
	// Look up the slot within whichever page owns this gkind. Caller is
	// responsible for syncing sMNMapsCursorPage via mnMapsGetPageForGkind.
	s32 page;
	s32 slot;

	for (page = 0; page < nMNMapsPageCount; page++)
	{
		for (slot = 0; slot < nMNMapsSlotCount; slot++)
		{
			if (dMNMapsPageGkinds[page][slot] == gkind)
			{
				return slot;
			}
		}
	}
	return 0;
#else
	switch (gkind)
	{
	case nGRKindCastle:
		return 0;

	case nGRKindJungle:
		return 1;

	case nGRKindHyrule:
		return 2;

	case nGRKindZebes:
		return 3;

	case nGRKindInishie:
		return 4;

	case nGRKindYoster:
		return 5;

	case nGRKindPupupu:
		return 6;

	case nGRKindSector:
		return 7;

	case nGRKindYamabuki:
		return 8;

	case nMNMapsRandomGKind:
		return nMNMapsSlotRandom;
	}
#endif
}

// 0x80132528
void mnMapsMakeIcons(void)
{
	GObj *gobj;
	SObj *sobj;

	intptr_t offsets[/* */] =
	{
#ifdef PORT
		llMNMapsPeachsCastleSprite, 	llMNMapsSectorZSprite,
		llMNMapsCongoJungleSprite, 	llMNMapsPlanetZebesSprite,
		llMNMapsHyruleCastleSprite, 	llMNMapsYoshisIslandSprite,
		llMNMapsDreamLandSprite,	llMNMapsSaffronCitySprite,
		llMNMapsMushroomKingdomSprite,	llMNMapsRandomSmallSprite
#else
		&llMNMapsPeachsCastleSprite, 	&llMNMapsSectorZSprite,
		&llMNMapsCongoJungleSprite, 	&llMNMapsPlanetZebesSprite,
		&llMNMapsHyruleCastleSprite, 	&llMNMapsYoshisIslandSprite,
		&llMNMapsDreamLandSprite,	&llMNMapsSaffronCitySprite,
		&llMNMapsMushroomKingdomSprite,	&llMNMapsRandomSmallSprite
#endif
	};
	s32 gkind;
	f32 x;
	f32 y;
	s32 i;

	gobj = gcMakeGObjSPAfter(0, NULL, 3, GOBJ_PRIORITY_DEFAULT);
	gcAddGObjDisplay(gobj, lbCommonDrawSObjAttr, 1, GOBJ_PRIORITY_DEFAULT, ~0);
#ifdef PORT
	sMNMapsIconsGObj = gobj;
#endif

	for (i = 0; i < nMNMapsSlotCount; i++)
	{
		gkind = mnMapsGetGroundKind(i);

		if (mnMapsCheckLocked(gkind) == FALSE)
		{
			if (gkind == nMNMapsRandomGKind)
			{
#ifdef PORT
				sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], llMNMapsRandomSmallSprite));
#else
				sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], &llMNMapsRandomSmallSprite));
#endif
			}
			else
			{
				Sprite *port_icon = NULL;
#ifdef PORT
				// Generic per-stage port asset (Torch-derived PNG, runtime-loaded). Returns
				// NULL for ROM gkinds that don't have a port override — we fall through to
				// the legacy offsets[] table for those.
				port_icon = portCSSGetStageIconSprite(gkind);
#endif
				if (port_icon != NULL)
				{
					sobj = lbCommonMakeSObjForGObj(gobj, port_icon);
				}
				else if (gkind <= nGRKindBattleEnd)
				{
					// ROM VS-mode stage — index the legacy offsets table.
					sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], offsets[gkind]));
				}
#ifdef PORT
				else
				{
					// Port-introduced stage but its PNG is missing (user hasn't run a Torch
					// build, or the manifest entry was added without a matching extraction).
					// Show the question-mark sprite so the slot still renders.
					sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], llMNMapsQuestionMarkSprite));
				}
#endif
			}

			mnMapsGetSlotPosition(i, &x, &y);
			sobj->pos.x = x;
			sobj->pos.y = y;
		}
	}
}

// 0x801326DC
void mnMapsSetNamePosition(SObj *sobj, s32 gkind)
{
	Vec2f positions[/* */] =
	{
		{ 195.0F, 196.0F },
		{ 202.0F, 196.0F },
		{ 190.0F, 196.0F },
		{ 195.0F, 196.0F },
		{ 198.0F, 196.0F },
		{ 190.0F, 196.0F },
		{ 195.0F, 196.0F },
		{ 190.0F, 196.0F },
		{ 190.0F, 196.0F }
	};
#if defined(REGION_US)
	sobj->pos.x = 183.0F;
	sobj->pos.y = 196.0F;
#else
	sobj->pos.x = positions[gkind].x;
	sobj->pos.y = positions[gkind].y;
#endif
}


// 0x80132738
void mnMapsMakeName(GObj *gobj, s32 gkind)
{
	SObj* sobj;
	intptr_t offsets[/* */] =
	{
#ifdef PORT
		llMNMapsPeachsCastleTextSprite,
		llMNMapsSectorZTextSprite,
		llMNMapsCongoJungleTextSprite,
		llMNMapsPlanetZebesTextSprite,
		llMNMapsHyruleCastleTextSprite,
		llMNMapsYoshisIslandTextSprite,
		llMNMapsDreamLandTextSprite,
		llMNMapsSaffronCityTextSprite,
		llMNMapsMushroomKingdomTextSprite
#else
		&llMNMapsPeachsCastleTextSprite,
		&llMNMapsSectorZTextSprite,
		&llMNMapsCongoJungleTextSprite,
		&llMNMapsPlanetZebesTextSprite,
		&llMNMapsHyruleCastleTextSprite,
		&llMNMapsYoshisIslandTextSprite,
		&llMNMapsDreamLandTextSprite,
		&llMNMapsSaffronCityTextSprite,
		&llMNMapsMushroomKingdomTextSprite
#endif
	};

#ifdef PORT
	// Generic per-stage nameplate sprite (96x10 synthesized plate — PNG
	// override or the compiled-in baked fallback). Same color-override pattern
	// as ROM nameplates: red/green/blue forced to 0x00 so the texel's alpha
	// mask becomes a black silhouette. mnMapsSetNamePosition isn't safe for
	// out-of-range gkinds (positions[] is only 9 entries), so position
	// directly at the ROM's canonical nameplate anchor x=183: the PlateRight
	// cap sprite is 24 px wide (o2r-verified), so the pill spans 174..286
	// with center 230, and a 96px canvas at 183 centers its ink at 231 —
	// exactly where the ROM's own pre-rendered plates sit. (An earlier fix
	// assumed a 12px right cap and anchored at 176, ~6px left of center.)
	{
		Sprite *port_name = portCSSGetStageNameSprite(gkind);
		if (port_name != NULL)
		{
			sobj = lbCommonMakeSObjForGObj(gobj, port_name);
			sobj->pos.x = 183.0F;
			sobj->pos.y = 196.0F;
			sobj->sprite.attr &= ~SP_FASTCOPY;
			sobj->sprite.attr |= SP_TRANSPARENT;
			sobj->sprite.red   = 0x00;
			sobj->sprite.green = 0x00;
			sobj->sprite.blue  = 0x00;
			return;
		}
	}
	// Port-introduced stages always have a nameplate from the port side —
	// PNG override or the compiled-in baked plate (port_css_stage_assets.cpp)
	// — so a NULL here for gkind > nGRKindBattleEnd means the port build is
	// broken, not a runtime condition. Never index offsets[] (9 entries) with
	// an out-of-range gkind; leave the plate blank instead. (The old
	// runtime-rasterized subtitle-font fallback was deleted with the baked
	// plates — it rendered chewed, mis-kerned glyphs at fractional scales.)
	if (gkind > nGRKindBattleEnd)
	{
		return;
	}
#endif
	sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], offsets[gkind]));
	mnMapsSetNamePosition(sobj, gkind);

	sobj->sprite.attr &= ~SP_FASTCOPY;
	sobj->sprite.attr |= SP_TRANSPARENT;

	sobj->sprite.red = 0x00;
	sobj->sprite.green = 0x00;
	sobj->sprite.blue = 0x00;
}

#if defined(REGION_US)
// 0x80134700
char *dMNMapsSubtitles[/* */] =
{
	"IN THE SKY OF",
	"SECTOR Z",
	"CONGO JUNGLE",
	"PLANET ZEBES",
	"CASTLE OF HYRULE",
	"YOSHI'S ISLAND",
	"PUPUPU LAND",
	"YAMABUKI CITY",
	"CLASSIC MUSHROOM"
};
char *dMNMapsSubtitles2[/* */] =
{
	"CASTLE PEACH",
	"ABOARD A GREAT FOX",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"KINGDOM"
};

// 0x80134748
Vec2f dMNMapsSubtitlePositions[/* */] =
{
	{ 192.0F, 167.0F },
	{ 214.0F, 167.0F },
	{ 202.0F, 169.0F },
	{ 202.0F, 169.0F },
	{ 193.0F, 169.0F },
	{ 198.0F, 169.0F },
	{ 205.0F, 169.0F },
	{ 199.0F, 169.0F },
	{ 191.0F, 167.0F }
};
Vec2f dMNMapsSubtitlePositions2[/* */] =
{
	{ 209.0F, 174.0F },
	{ 188.0F, 174.0F },
	{   0.0F,   0.0F },
	{   0.0F,   0.0F },
	{   0.0F,   0.0F },
	{   0.0F,   0.0F },
	{   0.0F,   0.0F },
	{ 203.0F, 174.0F }, 
	{ 213.0F, 174.0 }
};

// 0x801347D8
u32 dMNMapsSubtitleColors[/* */] = { 255, 255, 255 };

// 0x801327E0 - Unused?
void mnMapsSubtitleHasExtraLine(void)
{
	return;
}

// 0x801327E8 - Unused?
void mnMapsMakeSubtitle(void)
{
	return;
}
#else
// 0x801327E0 - Unused?
sb32 mnMapsSubtitleHasExtraLine(s32 gkind)
{
	switch (gkind)
	{
		case nGRKindCastle:
		case nGRKindSector:
		case nGRKindInishie:
			return TRUE;
		default:
			return FALSE;
	}
}

// 0x801327E8 - Unused?
void mnMapsMakeSubtitle(GObj *gobj, s32 gkind) {
    char *dMNMapsSubtitles[/* */] =
    {
    	"IN THE SKY OF",
    	"SECTOR Z",
    	"CONGO JUNGLE",
    	"PLANET ZEBES",
    	"CASTLE OF HYRULE",
    	"YOSHI'S ISLAND",
    	"PUPUPU LAND",
    	"YAMABUKI CITY",
    	"CLASSIC MUSHROOM"
    };
    char *dMNMapsSubtitles2[/* */] =
    {
    	"CASTLE PEACH",
    	"ABOARD A GREAT FOX",
    	NULL,
    	NULL,
    	NULL,
    	NULL,
    	NULL,
    	NULL,
    	"KINGDOM"
    };

    Vec2f dMNMapsSubtitlePositions[/* */] =
    {
        { 192.0F, 167.0F },
        { 214.0F, 167.0F },
        { 202.0F, 169.0F },
        { 202.0F, 169.0F },
        { 193.0F, 169.0F },
        { 198.0F, 169.0F },
        { 205.0F, 169.0F },
        { 199.0F, 169.0F },
        { 191.0F, 167.0F }
    };
    Vec2f dMNMapsSubtitlePositions2[/* */] =
    {
        { 209.0F, 174.0F },
        { 188.0F, 174.0F },
        {   0.0F,   0.0F },
        {   0.0F,   0.0F },
        {   0.0F,   0.0F },
        {   0.0F,   0.0F },
        {   0.0F,   0.0F },
        { 203.0F, 174.0F }, 
        { 213.0F, 174.0 }
    };

    u32 dMNMapsSubtitleColors[/* */] = { 255, 255, 255 };

    mnMapsMakeString(gobj, dMNMapsSubtitles[gkind], dMNMapsSubtitlePositions[gkind].x, dMNMapsSubtitlePositions[gkind].y, dMNMapsSubtitleColors);
    
    if (mnMapsSubtitleHasExtraLine(gkind) != FALSE) 
    {
        mnMapsMakeString(gobj, dMNMapsSubtitles2[gkind], dMNMapsSubtitlePositions2[gkind].x, dMNMapsSubtitlePositions2[gkind].y, dMNMapsSubtitleColors);
    }
}
#endif

// 0x801327F0
void mnMapsSetLogoPosition(GObj *gobj, s32 gkind)
{
	Vec2f positions[/* */] =
	{
		{ 3.0F, 19.0F },
		{ 3.0F, 19.0F },
		{ 3.0F, 20.0F },
		{ 2.0F, 20.0F },
		{ 3.0F, 17.0F },
		{-1.0F, 19.0F },
		{ 1.0F, 20.0F },
		{ 1.0F, 20.0F },
		{ 3.0F, 19.0F },
		{34.0F, 20.0F }
	};

	if (gkind == nMNMapsRandomGKind)
	{
		SObjGetStruct(gobj)->pos.x = 223.0F;
		SObjGetStruct(gobj)->pos.y = 144.0F;
	}
	else if (gkind == nGRKindLast)
	{
		// FD emblem is the same 64x48 size as every ROM FT emblem (port_css_stage_assets
		// upscales the Master Hand sprite at build time). The other stages compute their
		// position as positions[gkind].x + 189, positions[gkind].y + 124, with per-stage
		// .x offsets clustered around 1..3 and .y around 19..20 — i.e. they all land at
		// roughly (190..192, 143..144). Match that so FD's emblem aligns visually with
		// the rest of the stage emblems on the wooden circle.
		SObjGetStruct(gobj)->pos.x = 191.0F;
		SObjGetStruct(gobj)->pos.y = 143.0F;
	}
	else
	{
		SObjGetStruct(gobj)->pos.x = positions[gkind].x + 189.0F;
		SObjGetStruct(gobj)->pos.y = positions[gkind].y + 124.0F;
	}
}

// 0x801328A8
void mnMapsMakeEmblem(GObj *gobj, s32 gkind)
{
	SObj *sobj;

	intptr_t offsets[/* */] =
	{
#ifdef PORT
		llFTEmblemSpritesMarioSprite,	llFTEmblemSpritesFoxSprite,
		llFTEmblemSpritesDonkeySprite, 	llFTEmblemSpritesMetroidSprite,
		llFTEmblemSpritesZeldaSprite, 	llFTEmblemSpritesYoshiSprite,
		llFTEmblemSpritesKirbySprite, 	llFTEmblemSpritesPMonstersSprite,
		llFTEmblemSpritesMarioSprite
#else
		&llFTEmblemSpritesMarioSprite,	&llFTEmblemSpritesFoxSprite,
		&llFTEmblemSpritesDonkeySprite, 	&llFTEmblemSpritesMetroidSprite,
		&llFTEmblemSpritesZeldaSprite, 	&llFTEmblemSpritesYoshiSprite,
		&llFTEmblemSpritesKirbySprite, 	&llFTEmblemSpritesPMonstersSprite,
		&llFTEmblemSpritesMarioSprite
#endif
	};

	if (gkind == nMNMapsRandomGKind)
	{
#ifdef PORT
		sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], llMNMapsQuestionMarkSprite));
#else
		sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], &llMNMapsQuestionMarkSprite));
#endif

		sobj->sprite.attr &= ~SP_FASTCOPY;
		sobj->sprite.attr |= SP_TRANSPARENT;

		sobj->sprite.red = 0x5C;
		sobj->sprite.green = 0x22;
		sobj->sprite.blue = 0x00;
	}
#ifdef PORT
	else if ((gkind == nGRKindLast) || (gkind == nGRKindMetal) || (gkind == nGRKindZako))
	{
		// Port-introduced stages: emblem deliberately blank for now — every
		// attempt to render either a port-derived sprite or a ROM fallback
		// has hit format-edge bugs (TMEM overflow, IA4 bit-layout mismatch,
		// multi-bitmap stride). Skip emblem creation entirely; the wooden
		// circle behind it still renders. mnMapsSetLogoPosition is also
		// skipped because there's no SObj to position.
		return;
	}
#endif
	else
	{
		sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[0], offsets[gkind]));
		sobj->sprite.attr &= ~SP_FASTCOPY;
		sobj->sprite.attr |= SP_TRANSPARENT;

		sobj->sprite.red = 0x5C;
		sobj->sprite.green = 0x22;
		sobj->sprite.blue = 0x00;
	}
	mnMapsSetLogoPosition(gobj, gkind);
}

// 0x801329AC
void mnMapsMakeNameAndEmblem(s32 slot)
{
	GObj *gobj;

	if (sMNMapsNameLogoGObj != NULL)
	{
		gcEjectGObj(sMNMapsNameLogoGObj);
	}
	gobj = gcMakeGObjSPAfter(0, NULL, 4, GOBJ_PRIORITY_DEFAULT);
	sMNMapsNameLogoGObj = gobj;
	gcAddGObjDisplay(gobj, lbCommonDrawSObjAttr, 2, GOBJ_PRIORITY_DEFAULT, ~0);
	mnMapsMakeEmblem(sMNMapsNameLogoGObj, mnMapsGetGroundKind(slot));

	if (slot != nMNMapsSlotRandom)
	{
		mnMapsMakeName(sMNMapsNameLogoGObj, mnMapsGetGroundKind(slot));
#if defined(REGION_JP)
		{
			s32 ngkind = mnMapsGetGroundKind(slot);
			// Port-introduced stages have no JP subtitle table entry; dMNMapsSubtitles[]
			// is only sized for the original 9 stages.
			if ((ngkind != nGRKindLast) && (ngkind != nGRKindMetal) && (ngkind != nGRKindZako))
			{
				mnMapsMakeSubtitle(sMNMapsNameLogoGObj, ngkind);
			}
		}
#endif
	}
}

// 0x80132A58
void mnMapsSetCursorPosition(GObj *gobj, s32 slot)
{
	f32 x;
	f32 y;

	mnMapsGetSlotPosition(slot, &x, &y);
	SObjGetStruct(gobj)->pos.x = x - 7.0F;
	SObjGetStruct(gobj)->pos.y = y - 7.0F;
}

// 0x80132ADC
void mnMapsMakeCursor(void)
{
	GObj *gobj;
	SObj *sobj;

	sMNMapsCursorGObj = gobj = gcMakeGObjSPAfter(0, NULL, 7, GOBJ_PRIORITY_DEFAULT);
	gcAddGObjDisplay(gobj, lbCommonDrawSObjAttr, 5, GOBJ_PRIORITY_DEFAULT, ~0);

#ifdef PORT
	sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], llMNMapsCursorSprite));
#else
	sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], &llMNMapsCursorSprite));
#endif
	sobj->sprite.attr &= ~SP_FASTCOPY;
	sobj->sprite.attr |= SP_TRANSPARENT;

	sobj->sprite.red = 0xFF;
	sobj->sprite.green = 0x00;
	sobj->sprite.blue = 0x00;

	mnMapsSetCursorPosition(gobj, sMNMapsCursorSlot);
}

#ifdef PORT
// TRUE when the page holds at least one unlocked slot, i.e. a page jump could
// land there. Keeps the right-arrow indicator honest when the whole bonus page
// is hidden by the "Bonus Stages" toggle — mnMapsTryPageJump would refuse the
// jump, so the arrow must not advertise it.
static sb32 mnMapsPageHasUnlockedSlot(s32 page)
{
	s32 slot;

	for (slot = 0; slot < nMNMapsSlotCount; slot++)
	{
		if (mnMapsCheckLocked(dMNMapsPageGkinds[page][slot]) == FALSE)
		{
			return TRUE;
		}
	}
	return FALSE;
}

// Build the page-scroll arrow indicators. One SObj per visible arrow side:
//   right (>) — present when sMNMapsCursorPage + 1 < nMNMapsPageCount
//   left  (<) — present when sMNMapsCursorPage > 0; mirrored via sprite.scalex = -1
// The arrow sprite is W=6, H=44 (port_css_arrow.cpp). It sits at the vertical
// center of the icons block (rows at y=30..62 and y=68..100 → center y≈65).
// Lives in its own GObj so it doesn't get caught up in the icons slide animation —
// the arrows are page-state indicators, not page content. mnMapsMakeArrows
// destroys any prior arrows GObj and rebuilds, so it's safe to call on every
// page change.
void mnMapsMakeArrows(void)
{
	GObj *gobj;
	SObj *sobj;
	Sprite *arrow_right;
	Sprite *arrow_left;

	if (sMNMapsArrowsGObj != NULL)
	{
		gcEjectGObj(sMNMapsArrowsGObj);
		sMNMapsArrowsGObj = NULL;
	}

	arrow_right = portCSSGetScrollArrowSprite();
	arrow_left  = portCSSGetScrollArrowLeftSprite();
	if (arrow_right == NULL && arrow_left == NULL) return;

	gobj = gcMakeGObjSPAfter(0, NULL, 4, GOBJ_PRIORITY_DEFAULT);
	gcAddGObjDisplay(gobj, lbCommonDrawSObjAttr, 1, GOBJ_PRIORITY_DEFAULT, ~0);
	sMNMapsArrowsGObj = gobj;

	// Arrow placement: symmetric clearance from the icon row's outer edges so the
	// CSS grid stays at the exact same layout as page 0. Row-1 spans x=30..278
	// (slot 0 leftmost, slot 4 rightmost icon right edge). Each arrow leaves a
	// 12 px gap to the nearest icon edge.
	//   Right arrow:  pos.x = 290 → arrow.left=290, icon.right=278, gap=12
	//   Left  arrow:  pos.x = 10  → arrow.right=18, icon.left=30,   gap=12
	if (((sMNMapsCursorPage + 1) < nMNMapsPageCount) && (arrow_right != NULL) &&
	    (mnMapsPageHasUnlockedSlot(sMNMapsCursorPage + 1) != FALSE))
	{
		sobj = lbCommonMakeSObjForGObj(gobj, arrow_right);
		sobj->sprite.attr &= ~SP_FASTCOPY;
		sobj->sprite.attr |= SP_TRANSPARENT;
		sobj->pos.x = 290.0F;
		sobj->pos.y = 43.0F;   // center y=65 with H=44
	}
	if ((sMNMapsCursorPage > 0) && (arrow_left != NULL))
	{
		sobj = lbCommonMakeSObjForGObj(gobj, arrow_left);
		sobj->sprite.attr &= ~SP_FASTCOPY;
		sobj->sprite.attr |= SP_TRANSPARENT;
		sobj->pos.x = 10.0F;
		sobj->pos.y = 43.0F;
	}
}
#endif

// 0x80132B84
void mnMapsLoadMapFile(s32 gkind, void *heap)
{
	sMNMapsGroundInfo = lbRelocGetFileData
	(
		MPGroundData*,
		lbRelocGetForceExternHeapFile
		(
			dMNMapsFileInfos[gkind].file_id,
			heap
		),
		dMNMapsFileInfos[gkind].offset
	);
#ifdef PORT
	mpCollisionFixGroundDataLayout(sMNMapsGroundInfo);
#endif
}

// 0x80132BC8
void mnMapsPreviewWallpaperProcDisplay(GObj *gobj)
{
	gDPPipeSync(gSYTaskmanDLHeads[0]++);
	gDPSetCycleType(gSYTaskmanDLHeads[0]++, G_CYC_1CYCLE);
	gDPSetPrimColor(gSYTaskmanDLHeads[0]++, 0, 0, 0x57, 0x60, 0x88, 0xFF);
	gDPSetCombineMode(gSYTaskmanDLHeads[0]++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
	gDPSetRenderMode(gSYTaskmanDLHeads[0]++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
	gDPSetPrimColor(gSYTaskmanDLHeads[0]++, 0, 0, 0x00, 0x00, 0x00, 0x73);
	gDPFillRectangle(gSYTaskmanDLHeads[0]++, 43, 130, 152, 211);
	gDPPipeSync(gSYTaskmanDLHeads[0]++);
	gDPSetRenderMode(gSYTaskmanDLHeads[0]++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
	gDPSetCycleType(gSYTaskmanDLHeads[0]++, G_CYC_1CYCLE);

	lbCommonClearExternSpriteParams();
	lbCommonDrawSObjAttr(gobj);
}

// 0x80132D2C
GObj* mnMapsMakePreviewWallpaper(s32 gkind)
{
	GObj *gobj;
	SObj *sobj;
	s32 x;

	gobj = gcMakeGObjSPAfter(0, NULL, 9, GOBJ_PRIORITY_DEFAULT);
	gcAddGObjDisplay(gobj, mnMapsPreviewWallpaperProcDisplay, 7, GOBJ_PRIORITY_DEFAULT, ~0);

	// draw patterned bg
	for (x = 43; x < 155; x += 16)
	{
#ifdef PORT
		sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], llMNMapsTilesSprite));
#else
		sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], &llMNMapsTilesSprite));
#endif
		sobj->pos.x = x;
		sobj->pos.y = 130.0F;

		continue;
	}
	// Check if Random
	if (gkind == nMNMapsRandomGKind)
	{
		// If Random, use Random image
#ifdef PORT
		sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], llMNMapsRandomBigSprite));
#else
		sobj = lbCommonMakeSObjForGObj(gobj, lbRelocGetFileData(Sprite*, sMNMapsFiles[2], &llMNMapsRandomBigSprite));
#endif
		sobj->pos.x = 40.0F;
		sobj->pos.y = 127.0F;
	}
	else
	{
		// If not Random, check if Training Mode
		if (sMNMapsIsTrainingMode == TRUE)
		{
			/* PORT: hoist the #ifdef-PORT conditional values out of the
			 * lbRelocGetFileData macro argument list. MSVC's preprocessor
			 * (both legacy and /Zc:preprocessor) rejects `#` directives
			 * inside function-like macro arg lists with C2059 (the C
			 * standard says it's undefined behavior; clang/gcc accept
			 * it leniently). */
#ifdef PORT
			void *training_wallpaper_arg =
				(void*) ((uintptr_t)PORT_RESOLVE(sMNMapsGroundInfo->wallpaper) - dMNMapsWallpaperOffsets[gkind]);
			intptr_t training_blue_offset = llGRWallpaperTrainingBlueSprite;
#else
			void *training_wallpaper_arg =
				(void*) ((uintptr_t)sMNMapsGroundInfo->wallpaper - dMNMapsWallpaperOffsets[gkind]);
			intptr_t training_blue_offset = (intptr_t)&llGRWallpaperTrainingBlueSprite;
#endif
			// If Training Mode, use Smash logo bg
			sobj = lbCommonMakeSObjForGObj
			(
				gobj,
				lbRelocGetFileData
				(
					Sprite*,
					lbRelocGetForceExternHeapFile
					(
						dMNMapsTrainingModeFileInfos[dMNMapsTrainingModeWallpaperIDs[gkind]].file_id,
						training_wallpaper_arg
					),
					training_blue_offset
				)
			);
		}
#ifdef PORT
		else
		{
			// Generic per-stage port asset (Torch-derived from user's ROM at build
			// time, loaded as PNG at runtime). Returns NULL for stages without a
			// port-side entry — falls back to the reloc-data wallpaper from
			// sMNMapsGroundInfo, which is the original ROM-shipped sprite for that
			// stage. Either way, no Nintendo bytes are baked into the binary.
			Sprite *port_wp = portCSSGetStageBackgroundSprite(gkind);
			if (port_wp != NULL)
			{
				sobj = lbCommonMakeSObjForGObj(gobj, port_wp);
			}
			else
			{
				sobj = lbCommonMakeSObjForGObj(gobj, (Sprite*)PORT_RESOLVE(sMNMapsGroundInfo->wallpaper));
			}
		}
#else
		else sobj = lbCommonMakeSObjForGObj(gobj, sMNMapsGroundInfo->wallpaper); // Use stage bg
#endif
		
		sobj->sprite.attr &= ~SP_FASTCOPY;

		sobj->sprite.scalex = 0.37F;
		sobj->sprite.scaley = 0.37F;

		sobj->pos.x = 40.0F;
		sobj->pos.y = 127.0F;
	}
	return gobj;
}

// 0x80132EF0
void mnMapsModelPriProcDisplay(GObj *gobj)
{
#ifdef PORT
	// Mirror grDisplayLayer0PriProcDisplay (grdisplay.c): clear Z and render in
	// painter order. On N64 the original Z-buffered path leaned on AA-coverage
	// to mask coplanar surfaces; Fast3D's GL_LESS depth-compare exposes those
	// ties as per-pixel z-fight (visible on Dream Land's water-over-hill).
	gDPPipeSync(gSYTaskmanDLHeads[0]++);
	gSPClearGeometryMode(gSYTaskmanDLHeads[0]++, G_ZBUFFER);
	gDPSetRenderMode(gSYTaskmanDLHeads[0]++, G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2);

	gcDrawDObjTreeForGObj(gobj);

	gDPPipeSync(gSYTaskmanDLHeads[0]++);
	gSPSetGeometryMode(gSYTaskmanDLHeads[0]++, G_ZBUFFER);
	gDPSetRenderMode(gSYTaskmanDLHeads[0]++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
#else
	gDPPipeSync(gSYTaskmanDLHeads[0]++);
	gSPSetGeometryMode(gSYTaskmanDLHeads[0]++, G_ZBUFFER);
	gDPSetRenderMode(gSYTaskmanDLHeads[0]++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);

	gcDrawDObjTreeForGObj(gobj);
#endif
}

// 0x80132F70
void mnMapsModelSecProcDisplay(GObj *gobj)
{
#ifdef PORT
	// Mirror grDisplayLayer0SecProcDisplay (grdisplay.c): opaque pass in painter
	// order, XLU pass keeps Z (XLU surfaces don't author back-to-front; Z is
	// what prevents them from punching through later opaque draws).
	gDPPipeSync(gSYTaskmanDLHeads[0]++);
	gSPClearGeometryMode(gSYTaskmanDLHeads[0]++, G_ZBUFFER);
	gDPSetRenderMode(gSYTaskmanDLHeads[0]++, G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2);
	gDPPipeSync(gSYTaskmanDLHeads[1]++);
	gSPClearGeometryMode(gSYTaskmanDLHeads[1]++, G_ZBUFFER);
	gDPSetRenderMode(gSYTaskmanDLHeads[1]++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);

	gcDrawDObjTreeDLLinksForGObj(gobj);

	gDPPipeSync(gSYTaskmanDLHeads[0]++);
	gSPSetGeometryMode(gSYTaskmanDLHeads[0]++, G_ZBUFFER);
	gDPSetRenderMode(gSYTaskmanDLHeads[0]++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
	gDPPipeSync(gSYTaskmanDLHeads[1]++);
	gSPSetGeometryMode(gSYTaskmanDLHeads[1]++, G_ZBUFFER);
	gDPSetRenderMode(gSYTaskmanDLHeads[1]++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);
#else
	gDPPipeSync(gSYTaskmanDLHeads[0]++);
	gSPSetGeometryMode(gSYTaskmanDLHeads[0]++, G_ZBUFFER);
	gDPSetRenderMode(gSYTaskmanDLHeads[0]++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
	gDPPipeSync(gSYTaskmanDLHeads[1]++);
	gSPSetGeometryMode(gSYTaskmanDLHeads[1]++, G_ZBUFFER);
	gDPSetRenderMode(gSYTaskmanDLHeads[1]++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);

	gcDrawDObjTreeDLLinksForGObj(gobj);
#endif
}

// 0x8013303C
GObj* mnMapsMakeLayer(s32 gkind, MPGroundData *ground_data, MPGroundDesc *ground_desc, s32 id)
{
	GObj *gobj;
	f32 scales[/* */] =
	{
		0.5F, 0.2F, 0.6F,
		0.5F, 0.3F, 0.6F,
		0.5F, 0.4F, 0.2F
	};

	if (ground_desc->dobjdesc == NULL)
	{
		return NULL;
	}
	gobj = gcMakeGObjSPAfter(0, NULL, 5, GOBJ_PRIORITY_DEFAULT);
	gcAddGObjDisplay(gobj, (ground_data->layer_mask & (1 << id)) ? mnMapsModelSecProcDisplay : mnMapsModelPriProcDisplay, 3, GOBJ_PRIORITY_DEFAULT, ~0);
#ifdef PORT
	gcSetupCustomDObjs(gobj, (DObjDesc*)PORT_RESOLVE(ground_desc->dobjdesc), NULL, nGCMatrixKindTraRotRpyRSca, nGCMatrixKindNull, nGCMatrixKindNull);
#else
	gcSetupCustomDObjs(gobj, ground_desc->dobjdesc, NULL, nGCMatrixKindTraRotRpyRSca, nGCMatrixKindNull, nGCMatrixKindNull);
#endif

	if (ground_desc->p_mobjsubs != NULL)
	{
#ifdef PORT
		gcAddMObjAll(gobj, (MObjSub***)PORT_RESOLVE(ground_desc->p_mobjsubs));
#else
		gcAddMObjAll(gobj, ground_desc->p_mobjsubs);
#endif
	}
	if ((ground_desc->anim_joints != NULL) || (ground_desc->p_matanim_joints != NULL))
	{
#ifdef PORT
		gcAddAnimAll(gobj, (AObjEvent32**)PORT_RESOLVE(ground_desc->anim_joints), (AObjEvent32***)PORT_RESOLVE(ground_desc->p_matanim_joints), 0.0F);
#else
		gcAddAnimAll(gobj, ground_desc->anim_joints, ground_desc->p_matanim_joints, 0.0F);
#endif
		gcPlayAnimAll(gobj);
	}
	if (gkind == nGRKindLast)
	{
		// 0.35F: Final Destination's worldspace model is larger than the standard stages,
		// so it needs a lower scale than e.g. Hyrule (0.6) or Castle (0.5).  0.22F was
		// visibly too small; 0.35F brings it roughly in line with the other previews.
		DObjGetStruct(gobj)->scale.vec.f.x = 0.35F;
		DObjGetStruct(gobj)->scale.vec.f.y = 0.35F;
		DObjGetStruct(gobj)->scale.vec.f.z = 0.35F;
	}
#ifdef PORT
	else if ((gkind == nGRKindMetal) || (gkind == nGRKindZako))
	{
		// Metal Cavern / Battlefield are mid-sized boss arenas. 0.5F matches
		// the FD-class compact preview without clipping; tune up/down later
		// after seeing the preview camera framing in practice.
		DObjGetStruct(gobj)->scale.vec.f.x = 0.5F;
		DObjGetStruct(gobj)->scale.vec.f.y = 0.5F;
		DObjGetStruct(gobj)->scale.vec.f.z = 0.5F;
	}
#endif
	else
	{
		DObjGetStruct(gobj)->scale.vec.f.x = scales[gkind];
		DObjGetStruct(gobj)->scale.vec.f.y = scales[gkind];
		DObjGetStruct(gobj)->scale.vec.f.z = scales[gkind];
	}

	return gobj;
}

// 0x801331AC
void mnMapsMakeModel(s32 gkind, MPGroundData *ground_data, s32 heap_id)
{
	DObj *root_dobj, *next_dobj;
	GObj **gobjs = (heap_id == 0) ? sMNMapsHeap1LayerGObjs : sMNMapsHeap0LayerGObjs;
	s32 i;

	gobjs[0] = mnMapsMakeLayer(gkind, ground_data, &ground_data->gr_desc[0], 0);
	gobjs[1] = mnMapsMakeLayer(gkind, ground_data, &ground_data->gr_desc[1], 1);
	gobjs[2] = mnMapsMakeLayer(gkind, ground_data, &ground_data->gr_desc[2], 2);
	gobjs[3] = mnMapsMakeLayer(gkind, ground_data, &ground_data->gr_desc[3], 3);

	if (gkind == nGRKindYamabuki)
	{
		DObjGetChild(DObjGetChild(DObjGetStruct(gobjs[3])))->flags = DOBJ_FLAG_HIDDEN;
	}
	if (gkind == nGRKindYoster)
	{
		for
		(
			next_dobj = root_dobj = DObjGetStruct(gobjs[0]), i = 1;
			next_dobj != NULL;
			next_dobj = lbCommonGetTreeDObjNextFromRoot(next_dobj, root_dobj), i++
		)
		{
			if ((i == 0xF) || (i == 0x11))
			{
				next_dobj->flags = DOBJ_FLAG_HIDDEN;
			}
		}
	}
}

// 0x801332DC
void mnMapsDestroyPreview(s32 heap_id)
{
	s32 i;

	if (heap_id == 0)
	{
		if (sMNMapsHeap0WallpaperGObj != NULL)
		{
			gcEjectGObj(sMNMapsHeap0WallpaperGObj);
			sMNMapsHeap0WallpaperGObj = NULL;
		}
		for (i = 0; i < ARRAY_COUNT(sMNMapsHeap0LayerGObjs); i++)
		{
			if (sMNMapsHeap0LayerGObjs[i] != NULL)
			{
				gcEjectGObj(sMNMapsHeap0LayerGObjs[i]);
				sMNMapsHeap0LayerGObjs[i] = NULL;
			}
		}
	}
	else
	{
		if (sMNMapsHeap1WallpaperGObj != NULL)
		{
			gcEjectGObj(sMNMapsHeap1WallpaperGObj);
			sMNMapsHeap1WallpaperGObj = NULL;
		}
		for (i = 0; i < ARRAY_COUNT(sMNMapsHeap1LayerGObjs); i++)
		{
			if (sMNMapsHeap1LayerGObjs[i] != NULL)
			{
				gcEjectGObj(sMNMapsHeap1LayerGObjs[i]);
				sMNMapsHeap1LayerGObjs[i] = NULL;
			}
		}
	}
}

// 0x801333B4
void mnMapsMakePreview(s32 gkind)
{
	if (gkind != nMNMapsRandomGKind)
	{
		if (sMNMapsHeapID == 0)
		{
			mnMapsLoadMapFile(gkind, sMNMapsModelHeap1);
		}
		else mnMapsLoadMapFile(gkind, sMNMapsModelHeap0);
	}
	if (sMNMapsHeapID == 0)
	{
		sMNMapsHeap1WallpaperGObj = mnMapsMakePreviewWallpaper(gkind);
	}
	else sMNMapsHeap0WallpaperGObj = mnMapsMakePreviewWallpaper(gkind);

	if (gkind != nMNMapsRandomGKind)
	{
		mnMapsMakeModel(gkind, sMNMapsGroundInfo, sMNMapsHeapID);
		mnMapsSetPreviewCameraPosition(sMNMapsPreviewCObj, gkind);
	}
	mnMapsDestroyPreview(sMNMapsHeapID);

	sMNMapsHeapID = (sMNMapsHeapID == 0) ? 1 : 0;
}

// 0x801334AC
void mnMapsMakeWallpaperCamera(void)
{
	GObj *gobj = gcMakeCameraGObj
	(
		1,
		NULL,
		1,
		GOBJ_PRIORITY_DEFAULT,
		lbCommonDrawSprite,
		80,
		COBJ_MASK_DLLINK(0),
		~0,
		FALSE,
		nGCProcessKindFunc,
		NULL,
		1,
		FALSE
	);
	CObj *cobj = CObjGetStruct(gobj);
	syRdpSetViewport(&cobj->viewport, 10.0F, 10.0F, 310.0F, 230.0F);
}

// 0x8013354C
void mnMapsMakePlaqueCamera(void)
{
	GObj *gobj = gcMakeCameraGObj
	(
		1,
		NULL,
		1,
		GOBJ_PRIORITY_DEFAULT,
		lbCommonDrawSprite,
		40,
		COBJ_MASK_DLLINK(6),
		~0,
		FALSE,
		nGCProcessKindFunc,
		NULL,
		1,
		FALSE
	);
	CObj *cobj = CObjGetStruct(gobj);
	syRdpSetViewport(&cobj->viewport, 10.0F, 10.0F, 310.0F, 230.0F);
}

// 0x801335EC
void mnMapsMakePreviewWallpaperCamera(void)
{
	GObj *gobj = gcMakeCameraGObj
	(
		1,
		NULL,
		1,
		GOBJ_PRIORITY_DEFAULT,
		lbCommonDrawSprite,
		70,
		COBJ_MASK_DLLINK(7),
		~0,
		FALSE,
		nGCProcessKindFunc,
		NULL,
		1,
		FALSE
	);
	CObj *cobj = CObjGetStruct(gobj);
	syRdpSetViewport(&cobj->viewport, 10.0F, 10.0F, 310.0F, 230.0F);
}

// 0x8013368C
void mnMapsMakeLabelsViewport(void)
{
	GObj *gobj = gcMakeCameraGObj
	(
		1,
		NULL,
		1,
		GOBJ_PRIORITY_DEFAULT,
		lbCommonDrawSprite,
		30,
		COBJ_MASK_DLLINK(4),
		~0,
		FALSE,
		nGCProcessKindFunc,
		NULL,
		1,
		FALSE
	);
	CObj *cobj = CObjGetStruct(gobj);
	syRdpSetViewport(&cobj->viewport, 10.0F, 10.0F, 310.0F, 230.0F);
}

// 0x8013372C
void mnMapsMakeIconsCamera(void)
{
	GObj *gobj = gcMakeCameraGObj
	(
		1,
		NULL,
		1,
		GOBJ_PRIORITY_DEFAULT,
		lbCommonDrawSprite,
		60,
		COBJ_MASK_DLLINK(1),
		~0,
		FALSE,
		nGCProcessKindFunc,
		NULL,
		1,
		FALSE
	);
	CObj *cobj = CObjGetStruct(gobj);
	syRdpSetViewport(&cobj->viewport, 10.0F, 10.0F, 310.0F, 230.0F);
}

// 0x801337CC
void mnMapsMakeNameAndEmblemCamera(void)
{
	GObj *gobj = gcMakeCameraGObj
	(
		1,
		NULL,
		1,
		GOBJ_PRIORITY_DEFAULT,
		lbCommonDrawSprite,
		20,
		COBJ_MASK_DLLINK(2),
		~0,
		FALSE,
		nGCProcessKindFunc,
		NULL,
		1,
		FALSE
	);
	CObj *cobj = CObjGetStruct(gobj);
	syRdpSetViewport(&cobj->viewport, 10.0F, 10.0F, 310.0F, 230.0F);
}

// 0x8013386C
void mnMapsMakeCursorCamera(void)
{
	GObj *gobj = gcMakeCameraGObj
	(
		1,
		NULL,
		1,
		GOBJ_PRIORITY_DEFAULT,
		lbCommonDrawSprite,
		50,
		COBJ_MASK_DLLINK(5),
		~0,
		FALSE,
		nGCProcessKindFunc,
		NULL,
		1,
		FALSE
	);
	CObj *cobj = CObjGetStruct(gobj);
	syRdpSetViewport(&cobj->viewport, 10.0F, 10.0F, 310.0F, 230.0F);
}

// 0x8013390C
void mnMapsSetPreviewCameraPosition(CObj *cobj, s32 gkind)
{
	Vec3f positions[/* */] =
	{
		{ 1700.0F, 1800.0F, 0.0F },
		{ 1600.0F, 1600.0F, 0.0F },
		{ 1600.0F, 1600.0F, 0.0F },
		{ 1600.0F, 1600.0F, 0.0F },
		{ 1600.0F, 1500.0F, 0.0F },
		{ 1600.0F, 1600.0F, 0.0F },
		{ 1600.0F, 1500.0F, 0.0F },
		{ 1600.0F, 1600.0F, 0.0F },
		{ 1200.0F, 1600.0F, 0.0F },
#ifdef PORT
		// Port-introduced playable stages live well past the starter+unlock block;
		// extend the array via designated initializers so positions[<gkind>] is
		// well-defined. Intervening 1P/bonus gkinds never reach this VS-CSS path.
		[nGRKindMetal] = { 1600.0F, 1800.0F, 0.0F },
		[nGRKindZako]  = { 1600.0F, 1800.0F, 0.0F },
		[nGRKindLast]  = { 1600.0F, 1800.0F, 0.0F },
#endif
	};

	if (gkind == nMNMapsRandomGKind)
	{
		gkind = nGRKindCastle;
	}
	cobj->vec.eye.x = -3000.0F;
	cobj->vec.eye.y = 3000.0F;
	cobj->vec.eye.z = 9000.0F;
	cobj->vec.up.x = 0.0F;
	cobj->vec.up.y = 1.0F;
	cobj->vec.up.z = 0.0F;
	cobj->vec.at.x = positions[gkind].x;
	cobj->vec.at.y = positions[gkind].y;
	cobj->vec.at.z = positions[gkind].z;
#ifdef PORT
	/* Widescreen: the 3D stage preview renders through libultraship's
	 * AdjXForAspectRatio (clip-x compressed by (4/3)/window_aspect) while
	 * the surrounding CSS panel UI draws as 2D TextureRectangle ops that
	 * skip AdjX. Without compensation the rendered stage drifts inward
	 * relative to the 2D preview-window panel in 16:9. The model is
	 * centered at translate.x=0 so the fighter-style `translate.x /= scale`
	 * pattern is a no-op here; instead push the camera's look-at point
	 * outward along +x so the model projects further along -clip-x,
	 * shifting the rendered stage left to recenter it inside the 2D
	 * panel frame. */
	{
		f32 scale = port_widescreen_clip_x_scale();
		if (scale > 0.0F && scale < 1.0F)
		{
			cobj->vec.at.x /= scale;
		}
	}
	// Track the base at.y the camera thread oscillates around. Without this,
	// the thread (mnMapsPreviewCameraThreadUpdate) captures vec.at.y once at
	// start and ignores subsequent SetPosition writes — switching stages or
	// re-entering the CSS after a VS match would leave every stage rendered
	// around whatever y was loaded first.
	sMNMapsPreviewBaseAtY = positions[gkind].y;
#endif
}

// 0x801339C4
void mnMapsPreviewCameraThreadUpdate(GObj *gobj)
{
	CObj* cobj = CObjGetStruct(gobj);
#ifdef PORT
	// Re-read sMNMapsPreviewBaseAtY each frame so stage switches and scene
	// re-entries take effect. The base is updated by mnMapsSetPreviewCameraPosition.
	f32 deg = 0.0F;

	while (TRUE)
	{
		cobj->vec.at.y = __sinf(F_CLC_DTOR32(deg)) * 40.0F + sMNMapsPreviewBaseAtY;

		deg = (deg + 2.0F > 360.0F) ? deg + 2.0F - 360.0F : deg + 2.0F;

		gcSleepCurrentGObjThread(1);
	}
#else
	f32 y = cobj->vec.at.y;
	f32 deg = 0.0F;

	while (TRUE)
	{
		cobj->vec.at.y = __sinf(F_CLC_DTOR32(deg)) * 40.0F + y;

		deg = (deg + 2.0F > 360.0F) ? deg + 2.0F - 360.0F : deg + 2.0F;

		gcSleepCurrentGObjThread(1);
	}
#endif
}

// 0x80133A00
// In-page-only vertical move (row 1 ↔ row 2). Page transitions stay horizontal.
s32 mnMapsGetVerticalSlot(s32 slot, sb32 is_down)
{
	s32 next_slot;

	if (is_down != FALSE)
	{
		if (slot >= 5) return slot;
		next_slot = slot + 5;
	}
	else
	{
		if (slot < 5) return slot;
		next_slot = slot - 5;
	}
	if (mnMapsCheckLocked(mnMapsGetGroundKind(next_slot)) == FALSE)
	{
		return next_slot;
	}
	return slot;
}

// 0x80133A40
// In-page-only horizontal walk. Wraps within the current row (slot 4↔0, slot 9↔5).
// Page transitions are handled separately in mnMapsFuncRun via mnMapsTryPageJump.
s32 mnMapsGetHorizontalSlot(s32 slot, sb32 is_right)
{
	s32 next_slot = slot;
	s32 i;

	for (i = 0; i < nMNMapsSlotCount; i++)
	{
		if (is_right != FALSE)
		{
			if (next_slot == 4)        next_slot = 0;
			else if (next_slot == 9)   next_slot = 5;
			else                       next_slot++;
		}
		else
		{
			if (next_slot == 0)        next_slot = 4;
			else if (next_slot == 5)   next_slot = 9;
			else                       next_slot--;
		}
		if (mnMapsCheckLocked(mnMapsGetGroundKind(next_slot)) == FALSE)
		{
			return next_slot;
		}
	}
	return slot;
}

#ifdef PORT
// PORT: shift every SObj on a GObj's list by (dx, dy). Used to slide the icons
// GObj as a unit during a page transition. NULL-safe.
static void mnMapsShiftSObjs(GObj *gobj, f32 dx, f32 dy)
{
	SObj *s;

	if (gobj == NULL) return;
	for (s = SObjGetStruct(gobj); s != NULL; s = SObjGetNext(s))
	{
		s->pos.x += dx;
		s->pos.y += dy;
	}
}

// PORT: advance the page-slide animation by one frame. Returns TRUE if a slide
// is in progress (the caller — mnMapsFuncRun — should skip input handling this
// frame); FALSE means idle.
sb32 mnMapsTickScrollAnimation(void)
{
	if (sMNMapsScrollDir == nMNMapsScrollDirNone)
	{
		return FALSE;
	}

	mnMapsShiftSObjs(sMNMapsScrollOldIconsGObj,  sMNMapsScrollIconsDelta, 0.0F);
	mnMapsShiftSObjs(sMNMapsIconsGObj,           sMNMapsScrollIconsDelta, 0.0F);
	mnMapsShiftSObjs(sMNMapsScrollOldArrowsGObj, sMNMapsScrollIconsDelta, 0.0F);
	mnMapsShiftSObjs(sMNMapsArrowsGObj,          sMNMapsScrollIconsDelta, 0.0F);
	if (sMNMapsCursorGObj != NULL)
	{
		SObjGetStruct(sMNMapsCursorGObj)->pos.x += sMNMapsScrollCursorDelta;
	}

	sMNMapsScrollTicks--;
	if (sMNMapsScrollTicks <= 0)
	{
		// Slide finished — destroy outgoing icons + arrows, snap cursor onto the
		// exact slot, and refresh the per-stage UI (name plate, emblem, preview
		// camera + model). The new arrows GObj is already in place; just trust it.
		if (sMNMapsScrollOldIconsGObj != NULL)
		{
			gcEjectGObj(sMNMapsScrollOldIconsGObj);
			sMNMapsScrollOldIconsGObj = NULL;
		}
		if (sMNMapsScrollOldArrowsGObj != NULL)
		{
			gcEjectGObj(sMNMapsScrollOldArrowsGObj);
			sMNMapsScrollOldArrowsGObj = NULL;
		}
		mnMapsSetCursorPosition(sMNMapsCursorGObj, sMNMapsCursorSlot);
		mnMapsMakeNameAndEmblem(sMNMapsCursorSlot);
		mnMapsMakePreview(mnMapsGetGroundKind(sMNMapsCursorSlot));
		sMNMapsScrollDir = nMNMapsScrollDirNone;
	}
	return TRUE;
}

// PORT: kick off a page-slide animation. Caller has already validated that
// target_page / target_slot are a legitimate edge transition. The outgoing
// icons GObj is captured into sMNMapsScrollOldIconsGObj; a fresh icons GObj
// for the target page is built off-screen (offset ±320 px) and slides in
// while the old one slides out.
static void mnMapsBeginPageSlide(s32 target_page, s32 target_slot, sb32 is_right)
{
	f32 page_offset;
	f32 old_cursor_x;
	f32 new_cursor_x;
	f32 new_slot_x;
	f32 new_slot_y;

	// Snapshot current cursor position before we change pages.
	old_cursor_x = SObjGetStruct(sMNMapsCursorGObj)->pos.x;

	// Hand the current icons + arrows off to the "old / sliding off-screen" pointers.
	sMNMapsScrollOldIconsGObj  = sMNMapsIconsGObj;
	sMNMapsScrollOldArrowsGObj = sMNMapsArrowsGObj;
	sMNMapsIconsGObj           = NULL;
	sMNMapsArrowsGObj          = NULL;

	sMNMapsCursorPage = target_page;
	sMNMapsCursorSlot = target_slot;

	// mnMapsMakeIcons / mnMapsMakeArrows read sMNMapsCursorPage and assign
	// sMNMapsIconsGObj / sMNMapsArrowsGObj for the *new* page.
	mnMapsMakeIcons();
	mnMapsMakeArrows();

	// Position the new icons + arrows off-screen in the direction the slide
	// reveals them from: right-jump → enter from +320; left-jump → enter from -320.
	page_offset = (is_right != FALSE) ? nMNMapsScrollPageWidth : -nMNMapsScrollPageWidth;
	mnMapsShiftSObjs(sMNMapsIconsGObj,  page_offset, 0.0F);
	mnMapsShiftSObjs(sMNMapsArrowsGObj, page_offset, 0.0F);

	// Cursor lerps from its current screen position to the new slot's natural
	// cursor position over the slide duration.
	mnMapsGetSlotPosition(target_slot, &new_slot_x, &new_slot_y);
	new_cursor_x = new_slot_x - 7.0F;

	sMNMapsScrollDir          = (is_right != FALSE) ? nMNMapsScrollDirRight : nMNMapsScrollDirLeft;
	sMNMapsScrollTicks        = nMNMapsScrollFrames;
	sMNMapsScrollIconsDelta   = -page_offset / (f32)nMNMapsScrollFrames;
	sMNMapsScrollCursorDelta  = (new_cursor_x - old_cursor_x) / (f32)nMNMapsScrollFrames;
}

// PORT: attempt a page transition when the cursor is at the visible edge of
// its row in the requested direction. Returns TRUE if a slide was kicked off
// (caller must SKIP its own snap-updates while sMNMapsScrollDir != None);
// FALSE means caller should fall back to in-page horizontal movement.
//
// Trigger condition is "no unlocked slot exists in this row in the move
// direction", NOT "cursor sits on a hard-coded edge slot". That's important:
// when Mushroom Kingdom is locked, slot 4 is unreachable — without this the
// page jump from row 1 never fires (slot 3 is the de-facto right edge).
//
// Landing-slot search: try the symmetric row on the target page first
// (row 1 ↔ row 1, row 2 ↔ row 2). If that row is entirely empty on the
// target page (current page-1 case — only FD in row 1, no row 2 content),
// fall back to the other row. Without that fallback, pressing right from
// Random on page 0 would refuse to scroll because page 1's row 2 is empty.
sb32 mnMapsTryPageJump(sb32 is_right)
{
	s32 target_page;
	s32 row_left;
	s32 row_right;
	s32 other_left;
	s32 other_right;
	s32 step;
	s32 probe;

	// Current row range.
	if (sMNMapsCursorSlot < 5)
	{
		row_left = 0;  row_right = 4;
		other_left = 5; other_right = 9;
	}
	else
	{
		row_left = 5;  row_right = 9;
		other_left = 0; other_right = 4;
	}

	// At-edge check: is there any unlocked slot beyond the cursor in this row?
	if (is_right != FALSE)
	{
		for (probe = sMNMapsCursorSlot + 1; probe <= row_right; probe++)
		{
			if (mnMapsCheckLocked(mnMapsGetGroundKind(probe)) == FALSE) return FALSE;
		}
		target_page = sMNMapsCursorPage + 1;
		step = 1;
	}
	else
	{
		for (probe = sMNMapsCursorSlot - 1; probe >= row_left; probe--)
		{
			if (mnMapsCheckLocked(mnMapsGetGroundKind(probe)) == FALSE) return FALSE;
		}
		target_page = sMNMapsCursorPage - 1;
		step = -1;
	}

	if (target_page < 0 || target_page >= nMNMapsPageCount) return FALSE;

	// Find a landing slot on target_page. Walk the symmetric row from the
	// entry side first, then fall back to the other row if symmetric is empty.
	{
		s32 probe_start = (is_right != FALSE) ? row_left : row_right;
		s32 fallback_start = (is_right != FALSE) ? other_left : other_right;
		s32 i;

		for (i = 0, probe = probe_start; i < 5 && probe >= row_left && probe <= row_right; i++, probe += step)
		{
			if (mnMapsCheckLocked(dMNMapsPageGkinds[target_page][probe]) == FALSE)
			{
				mnMapsBeginPageSlide(target_page, probe, is_right);
				return TRUE;
			}
		}
		for (i = 0, probe = fallback_start; i < 5 && probe >= other_left && probe <= other_right; i++, probe += step)
		{
			if (mnMapsCheckLocked(dMNMapsPageGkinds[target_page][probe]) == FALSE)
			{
				mnMapsBeginPageSlide(target_page, probe, is_right);
				return TRUE;
			}
		}
	}
	return FALSE;
}
#endif

// 0x80133A88
void mnMapsMakePreviewCamera(void)
{
	s32 unused;
	GObj *gobj = gcMakeCameraGObj
	(
		1,
		NULL,
		1,
		GOBJ_PRIORITY_DEFAULT,
		func_80017DBC,
		65,
		COBJ_MASK_DLLINK(3),
		~0,
		TRUE,
		nGCProcessKindThread,
		NULL,
		1,
		FALSE
	);
	CObj *cobj = CObjGetStruct(gobj);

	sMNMapsPreviewCObj = cobj;

	syRdpSetViewport(&cobj->viewport, 10.0F, 10.0F, 310.0F, 230.0F);

	cobj->projection.persp.far = 16384.0F;

	mnMapsSetPreviewCameraPosition(cobj, mnMapsGetGroundKind(sMNMapsCursorSlot));

	gcAddGObjProcess(gobj, mnMapsPreviewCameraThreadUpdate, nGCProcessKindThread, 1);
}

// 0x80133B78
void mnMapsSaveSceneData(void)
{
	s32 gkind;
	s32 slot;

	if (sMNMapsCursorSlot == nMNMapsSlotRandom)
	{
#ifdef PORT
		// Random picks across all pages — collect every unlocked, non-random,
		// non-current gkind and pick uniformly. That way FD (and any future
		// expansion stages on page 1+) participate.
		s32 candidates[nMNMapsPageCount * nMNMapsSlotCount];
		s32 count = 0;
		s32 page;

		for (page = 0; page < nMNMapsPageCount; page++)
		{
			for (slot = 0; slot < nMNMapsSlotCount; slot++)
			{
				gkind = dMNMapsPageGkinds[page][slot];
				if (gkind == nMNMapsRandomGKind) continue;
				if (mnMapsCheckLocked(gkind) != FALSE) continue;
				if (gkind == gSCManagerSceneData.gkind) continue;
				candidates[count++] = gkind;
			}
		}
		if (count > 0)
		{
			gSCManagerSceneData.gkind = candidates[syUtilsRandTimeUCharRange(count)];
		}
		else
		{
			gSCManagerSceneData.gkind = mnMapsGetGroundKind(0);
		}
#else
		do
		{
			slot = syUtilsRandTimeUCharRange(nMNMapsSlotRandom);
			gkind = mnMapsGetGroundKind(slot);
		}
		while ((mnMapsCheckLocked(gkind) != FALSE) || (gkind == gSCManagerSceneData.gkind));

		gSCManagerSceneData.gkind = gkind;
#endif
	}
	else gSCManagerSceneData.gkind = mnMapsGetGroundKind(sMNMapsCursorSlot);

	if (sMNMapsIsTrainingMode == FALSE)
	{
		gSCManagerSceneData.maps_vsmode_gkind = mnMapsGetGroundKind(sMNMapsCursorSlot);
	}
	if (sMNMapsIsTrainingMode == TRUE)
	{
		gSCManagerSceneData.maps_training_gkind = mnMapsGetGroundKind(sMNMapsCursorSlot);
	}
}

// 0x80133C6C
void mnMapsInitVars(void)
{
	s32 i;
#ifdef PORT
	s32 saved_gkind;
	s32 saved_page;
	port_enhancement_music_select_reset(); // reset hook for music selection
	sMNMapsStageSelectTextSObj = NULL; // reset our text tracking pointers
	sMNMapsMusicSelectTextGObj = NULL;
	// Latch the Bonus Stages toggle for this screen session — must happen
	// before any mnMapsCheckLocked call below (cursor restore) and stays
	// fixed until the screen is rebuilt.
	sMNMapsBonusStagesEnabled = port_enhancement_bonus_stages_enabled();
#endif

	sMNMapsNameLogoGObj = NULL;
	sMNMapsHeap0WallpaperGObj = NULL;
	sMNMapsHeap1WallpaperGObj = NULL;
#ifdef PORT
	sMNMapsIconsGObj           = NULL;
	sMNMapsArrowsGObj          = NULL;
	sMNMapsCursorPage          = nMNMapsPageOriginal;
	sMNMapsScrollDir           = nMNMapsScrollDirNone;
	sMNMapsScrollTicks         = 0;
	sMNMapsScrollOldIconsGObj  = NULL;
	sMNMapsScrollOldArrowsGObj = NULL;
	sMNMapsScrollIconsDelta    = 0.0F;
	sMNMapsScrollCursorDelta   = 0.0F;
	// Reset before mnMapsMakePreviewCamera (which calls SetPreviewCameraPosition)
	// runs — guards against a stale value if something queries the global before
	// the camera is rebuilt.
	sMNMapsPreviewBaseAtY      = 0.0F;
#endif

	for (i = 0; i < ARRAY_COUNT(sMNMapsHeap0LayerGObjs); i++)
	{
		sMNMapsHeap0LayerGObjs[i] = NULL;
		sMNMapsHeap1LayerGObjs[i] = NULL;
	}
	switch (gSCManagerSceneData.scene_prev)
	{
	case nSCKindPlayers1PTraining:
		sMNMapsIsTrainingMode = TRUE;
#ifdef PORT
		saved_gkind = gSCManagerSceneData.maps_training_gkind;
		saved_page  = mnMapsGetPageForGkind(saved_gkind);
		sMNMapsCursorPage = (saved_page >= 0) ? saved_page : nMNMapsPageOriginal;
#endif
		sMNMapsCursorSlot = mnMapsGetSlot(gSCManagerSceneData.maps_training_gkind);
		break;

	case nSCKindPlayersVS:
		sMNMapsIsTrainingMode = FALSE;
#ifdef PORT
		saved_gkind = gSCManagerSceneData.maps_vsmode_gkind;
		saved_page  = mnMapsGetPageForGkind(saved_gkind);
		sMNMapsCursorPage = (saved_page >= 0) ? saved_page : nMNMapsPageOriginal;
#endif
		sMNMapsCursorSlot = mnMapsGetSlot(gSCManagerSceneData.maps_vsmode_gkind);
		break;
	}
	sMNMapsUnlockedMask = gSCManagerBackupData.unlock_mask;
	if (mnMapsCheckLocked(mnMapsGetGroundKind(sMNMapsCursorSlot)) != FALSE)
	{
#ifdef PORT
		// Fall back to page 0 slot 0 (always Castle) if the restored slot is unusable
		// on the resolved page (e.g. training mode hides FD on page 1).
		sMNMapsCursorPage = nMNMapsPageOriginal;
#endif
		sMNMapsCursorSlot = 0;
	}
	sMNMapsHeapID = 1;
	sMNMapsTotalTimeTics = 0;
	sMNMapsReturnTic = sMNMapsTotalTimeTics + I_MIN_TO_TICS(5);
}

// 0x80133D60
void mnMapsSaveSceneData2(void)
{
	mnMapsSaveSceneData();
}

// 0x80133D80
void mnMapsFuncRun(GObj *gobj)
{
	s32 unused;
	s32 stick_input;
	s32 button_input;
	s32 next_slot;

	sMNMapsTotalTimeTics++;

#ifdef PORT
	// Page-slide animation has the floor — while sliding, block all input and
	// don't run the rest of the per-frame logic. mnMapsTickScrollAnimation
	// finalizes (eject old icons, refresh name/emblem/preview) on the last tick.
	if (mnMapsTickScrollAnimation() != FALSE)
	{
		return;
	}
#endif

	if (sMNMapsTotalTimeTics >= 10)
	{
		if (sMNMapsTotalTimeTics == sMNMapsReturnTic)
		{
			gSCManagerSceneData.scene_prev = gSCManagerSceneData.scene_curr;
			gSCManagerSceneData.scene_curr = nSCKindTitle;

			mnMapsSaveSceneData2();
			syTaskmanSetLoadScene();
			return;
		}
		if (scSubsysControllerCheckNoInputAll() == FALSE)
		{
			sMNMapsReturnTic = sMNMapsTotalTimeTics + I_MIN_TO_TICS(5);
		}
		if (sMNMapsScrollWait != 0)
		{
			sMNMapsScrollWait--;
		}
		if
		(
			(scSubsysControllerGetPlayerStickInRangeLR(-20, 20)) &&
			(scSubsysControllerGetPlayerStickInRangeUD(-20, 20)) &&
			(scSubsysControllerGetPlayerHoldButtons(U_JPAD | R_JPAD | R_TRIG | U_CBUTTONS | R_CBUTTONS) == FALSE) && 
			(scSubsysControllerGetPlayerHoldButtons(D_JPAD | L_JPAD | L_TRIG | D_CBUTTONS | L_CBUTTONS) == FALSE)
		)
		{
			sMNMapsScrollWait = 0;
		}

		if (scSubsysControllerGetPlayerTapButtons(A_BUTTON | START_BUTTON))
		{
			#ifdef PORT
			s32 action = port_enhancement_music_select_handle_a(mnMapsGetBGMForGKind(mnMapsGetGroundKind(sMNMapsCursorSlot)));
			if (action == 1)
			{
				// Phase 1: Stage Locked
				mnMapsSaveSceneData2();
				func_800269C0_275C0(nSYAudioFGMStageSelect); // Confirm sound
				mnMapsSetCursorColor(0x00, 0xAA, 0xFF);      // Turn cursor Blue
				mnMapsSetLabelState(TRUE); // Change text to "Music Select"
				return;                                      // Halt scene transition
			}
			else if (action == 2)
			{
				// Phase 2: Music Locked (proceeds to transition)
				func_800269C0_275C0(nSYAudioFGMStageSelect);
			}
			else
			{
				// Original Behavior
				mnMapsSaveSceneData2();
				func_800269C0_275C0(nSYAudioFGMStageSelect);
			}
			#else
			mnMapsSaveSceneData2();
			func_800269C0_275C0(nSYAudioFGMStageSelect);
			#endif
			if (sMNMapsIsTrainingMode == TRUE)
			{
				gSCManagerSceneData.scene_prev = gSCManagerSceneData.scene_curr;
				gSCManagerSceneData.scene_curr = nSCKind1PTrainingMode;
			}
			else
			{
				gSCManagerSceneData.scene_prev = gSCManagerSceneData.scene_curr;
				gSCManagerSceneData.scene_curr = nSCKindVSBattle;
			}
			syTaskmanSetLoadScene();
		}

		if (scSubsysControllerGetPlayerTapButtons(B_BUTTON))
		{
			#ifdef PORT
			if (port_enhancement_music_select_handle_b() == 1)
			{
				// Cancel music selection and return to stage selection
				func_800269C0_275C0(nSYAudioFGMMenuDenied); // Back sound
				mnMapsSetCursorColor(0xFF, 0x00, 0x00);     // Turn cursor Red
				mnMapsSetLabelState(FALSE); // Restore "Stage Select" text
				return;
			}
			#endif
			mnMapsSaveSceneData2();

			if (sMNMapsIsTrainingMode == TRUE)
			{
				gSCManagerSceneData.scene_prev = gSCManagerSceneData.scene_curr;
				gSCManagerSceneData.scene_curr = nSCKindPlayers1PTraining;
			}
			else
			{
				gSCManagerSceneData.scene_prev = gSCManagerSceneData.scene_curr;
				gSCManagerSceneData.scene_curr = nSCKindPlayersVS;
			}
			syTaskmanSetLoadScene();
		}
		if (sMNMapsScrollWait == 0)
		{
			button_input = scSubsysControllerGetPlayerHoldButtons(U_JPAD | U_CBUTTONS);

			if ((button_input != 0) || (stick_input = scSubsysControllerGetPlayerStickUD(20, 1), (stick_input != 0)))
			{
				next_slot = mnMapsGetVerticalSlot(sMNMapsCursorSlot, FALSE);

				if (next_slot != sMNMapsCursorSlot)
				{
					func_800269C0_275C0(nSYAudioFGMMenuScroll2);

					sMNMapsCursorSlot = next_slot;

					mnMapsMakeNameAndEmblem(sMNMapsCursorSlot);
					mnMapsSetCursorPosition(sMNMapsCursorGObj, sMNMapsCursorSlot);
					mnMapsMakePreview(mnMapsGetGroundKind(sMNMapsCursorSlot));
				}
				if (button_input != 0)
				{
					sMNMapsScrollWait = 12;
				}
				else sMNMapsScrollWait = (160 - stick_input) / 7;

				return;
			}
			button_input = scSubsysControllerGetPlayerHoldButtons(D_JPAD | D_CBUTTONS);

			if ((button_input != 0) || (stick_input = scSubsysControllerGetPlayerStickUD(-20, 0), (stick_input != 0)))
			{
				next_slot = mnMapsGetVerticalSlot(sMNMapsCursorSlot, TRUE);

				if (next_slot != sMNMapsCursorSlot)
				{
					func_800269C0_275C0(nSYAudioFGMMenuScroll2);

					sMNMapsCursorSlot = next_slot;

					mnMapsMakeNameAndEmblem(sMNMapsCursorSlot);
					mnMapsSetCursorPosition(sMNMapsCursorGObj, sMNMapsCursorSlot);
					mnMapsMakePreview(mnMapsGetGroundKind(sMNMapsCursorSlot));
				}
				if (button_input != 0)
				{
					sMNMapsScrollWait = 12;
				}
				else sMNMapsScrollWait = (stick_input + 160) / 7;

				return;
			}
			button_input = scSubsysControllerGetPlayerHoldButtons(L_JPAD | L_TRIG | L_CBUTTONS);

			if ((button_input != 0) || (stick_input = scSubsysControllerGetPlayerStickLR(-20, 0), (stick_input)))
			{
#ifdef PORT
				// Page jump kicks off a slide; the slide's last frame finalizes name+emblem+preview.
				// We skip the per-stage snap-updates and cursor reposition in that case.
				if (mnMapsTryPageJump(FALSE) != FALSE)
				{
					func_800269C0_275C0(nSYAudioFGMMenuScroll2);
				}
				else
#endif
				{
					sMNMapsCursorSlot = mnMapsGetHorizontalSlot(sMNMapsCursorSlot, FALSE);
					func_800269C0_275C0(nSYAudioFGMMenuScroll2);
					mnMapsMakeNameAndEmblem(sMNMapsCursorSlot);
					mnMapsSetCursorPosition(sMNMapsCursorGObj, sMNMapsCursorSlot);
					mnMapsMakePreview(mnMapsGetGroundKind(sMNMapsCursorSlot));
				}

				if (button_input != 0)
				{
					sMNMapsScrollWait = 12;
				}
				else sMNMapsScrollWait = (stick_input + 160) / 7;

				return;
			}
			button_input = scSubsysControllerGetPlayerHoldButtons(R_JPAD | R_TRIG | R_CBUTTONS);

			if ((button_input != 0) || (stick_input = scSubsysControllerGetPlayerStickLR(20, 1), (stick_input)))
			{
#ifdef PORT
				if (mnMapsTryPageJump(TRUE) != FALSE)
				{
					func_800269C0_275C0(nSYAudioFGMMenuScroll2);
				}
				else
#endif
				{
					sMNMapsCursorSlot = mnMapsGetHorizontalSlot(sMNMapsCursorSlot, TRUE);
					func_800269C0_275C0(nSYAudioFGMMenuScroll2);
					mnMapsMakeNameAndEmblem(sMNMapsCursorSlot);
					mnMapsSetCursorPosition(sMNMapsCursorGObj, sMNMapsCursorSlot);
					mnMapsMakePreview(mnMapsGetGroundKind(sMNMapsCursorSlot));
				}

				if (button_input != 0)
				{
					sMNMapsScrollWait = 12;
				}
				else sMNMapsScrollWait = (160 - stick_input) / 7;
			}
		}
	}
}

// 0x80134304
void mnMapsFuncStart(void)
{
	s32 unused1[2];
	LBRelocSetup rl_setup;
	s32 unused2;

	rl_setup.table_addr = (uintptr_t)&lLBRelocTableAddr;
#ifdef PORT
	rl_setup.table_files_num = (u32)llRelocFileCount;
#else
	rl_setup.table_files_num = (u32)&llRelocFileCount;
#endif
	rl_setup.file_heap = NULL;
	rl_setup.file_heap_size = 0;
	rl_setup.status_buffer = sMNMapsStatusBuffer;
	rl_setup.status_buffer_size = ARRAY_COUNT(sMNMapsStatusBuffer);
	rl_setup.force_status_buffer = sMNMapsForceStatusBuffer;
	rl_setup.force_status_buffer_size = ARRAY_COUNT(sMNMapsForceStatusBuffer);

	lbRelocInitSetup(&rl_setup);
	lbRelocLoadFilesListed(dMNMapsFileIDs, sMNMapsFiles);
	mnMapsAllocModelHeaps();

	gcMakeGObjSPAfter(0, mnMapsFuncRun, 0, GOBJ_PRIORITY_DEFAULT);
	gcMakeDefaultCameraGObj(1, GOBJ_PRIORITY_DEFAULT, 100, COBJ_FLAG_ZBUFFER, GPACK_RGBA8888(0x00, 0x00, 0x00, 0x00));
	mnMapsInitVars();
	mnMapsMakeWallpaperCamera();
	mnMapsMakeLabelsViewport();
	mnMapsMakeIconsCamera();
	mnMapsMakeNameAndEmblemCamera();
	mnMapsMakeCursorCamera();
	mnMapsMakePreviewCamera();
	mnMapsMakePlaqueCamera();
	mnMapsMakePreviewWallpaperCamera();
	mnMapsMakeWallpaper();
	mnMapsMakePlaque();
	mnMapsMakeLabels();
	mnMapsMakeIcons();
#ifdef PORT
	mnMapsMakeArrows();
#endif
	mnMapsMakeNameAndEmblem(sMNMapsCursorSlot);
	mnMapsMakeCursor();
	mnMapsMakePreview(mnMapsGetGroundKind(sMNMapsCursorSlot));
}

// 0x8013490C
SYVideoSetup dMNMapsVideoSetup = SYVIDEO_SETUP_DEFAULT();

// 0x80134928
SYTaskmanSetup dMNMapsTaskmanSetup =
{
    // Task Manager Buffer Setup
    {
        0,                          // ???
        gcRunAll,              		// Update function
        scManagerFuncDraw,        	// Frame draw function
        &ovl30_BSS_END,             // Allocatable memory pool start
        0,                          // Allocatable memory pool size
        1,                          // ???
        2,                          // Number of contexts?
        sizeof(Gfx) * 2125,         // Display List Buffer 0 Size
        sizeof(Gfx) * 512,          // Display List Buffer 1 Size
        0,                          // Display List Buffer 2 Size
        0,                          // Display List Buffer 3 Size
        0x8000,                     // Graphics Heap Size
        2,                          // ???
        0x8000,                     // RDP Output Buffer Size
        mnMapsFuncLights,   		// Pre-render function
        syControllerFuncRead,       // Controller I/O function
    },

    0,                              // Number of GObjThreads
    sizeof(u64) * 64,              	// Thread stack size
    0,                              // Number of thread stacks
    0,                              // ???
    0,                              // Number of GObjProcesses
    0,                              // Number of GObjs
    sizeof(GObj),                   // GObj size
    0,                              // Number of XObjs
    NULL,        					// Matrix function list
    NULL,                           // DObjVec eject function
    0,                              // Number of AObjs
    0,                              // Number of MObjs
    0,                              // Number of DObjs
    sizeof(DObj),                   // DObj size
    0,                              // Number of SObjs
    sizeof(SObj),                   // SObj size
    0,                              // Number of CObjs
    sizeof(CObj),                 	// CObj size
    
    mnMapsFuncStart         		// Task start function
};

// 0x8013446C
void mnMapsStartScene(void)
{
	dMNMapsVideoSetup.zbuffer = SYVIDEO_ZBUFFER_START(320, 240, 0, 10, u16);
	syVideoInit(&dMNMapsVideoSetup);

	dMNMapsTaskmanSetup.scene_setup.arena_size = (size_t) ((uintptr_t)&ovl1_VRAM - (uintptr_t)&ovl30_BSS_END);
	scManagerFuncUpdate(&dMNMapsTaskmanSetup);
}
