#ifdef PORT

/* PORT: re-link Subroutine/Goto targets in the compiled-C demo motion
 * scripts (issue #240 — Jigglypuff's eyes never blink on the character
 * select / victory screens).
 *
 * The scsubsysdata*.c demo scripts (CSS idle, win/lose poses, opening) are
 * s32 arrays whose Subroutine/Goto operands held N64 overlay addresses. On
 * LP64 a pointer can't live in an s32 slot (ADDR64 relocation spill), so
 * ftMotionCommandSubroutineS2/GotoS2 compile to a 0 placeholder — which
 * PORT_RESOLVE() in ftMainParseMotionEvent silently resolves to NULL,
 * killing the script at its first jump. Eight fighters' blink/mouth
 * texture-swap subroutines were dead on every demo pose (the in-match
 * copies of the same scripts load from the relocData binary with real
 * tokens and were never affected).
 *
 * Same pattern as gmColScriptsLinkRelocTargets (gmcolscripts.c): a table
 * of { script, operand word index, target } links, each patched to a
 * 32-bit reloc token at scene init. The table is generated from the
 * ftMotionCommandSubroutine/Goto call sites in scsubsysdata*.c — each
 * such macro occupies two s32 words, opcode then operand — and every
 * entry is validated against the compiled opcode word before patching so
 * source drift shows up as a log line, not a mispatched script. */

#include <ft/fttypes.h>
#include <ft/ftdef.h>

/* decomp/include shadows the system stdlib.h; declare getenv directly. */
extern char *getenv(const char *name);

extern unsigned int portRelocRegisterPointer(void *ptr);
extern void port_log(const char *fmt, ...);

extern s32 D_ovl1_80390DC0[]; /* mario blink subroutine */
extern s32 D_ovl1_80390DE4[];
extern s32 D_ovl1_80390E14[];
extern s32 D_ovl1_80390E34[];
extern s32 D_ovl1_80390E50[];
extern s32 D_ovl1_80390E7C[];
extern s32 D_ovl1_80391700[]; /* luigi blink subroutine */
extern s32 D_ovl1_80391724[];
extern s32 D_ovl1_80391890[]; /* link blink subroutine */
extern s32 D_ovl1_803918A4[];
extern s32 D_ovl1_80391B90[]; /* yoshi mouth subroutine */
extern s32 D_ovl1_80391BC4[];
extern s32 D_ovl1_80392010[]; /* kirby blink subroutine */
extern s32 D_ovl1_80392020[];
extern s32 D_ovl1_80392088[];
extern s32 D_ovl1_80392240[]; /* pikachu blink subroutine */
extern s32 D_ovl1_80392250[];
extern s32 D_ovl1_803922AC[];
extern s32 D_ovl1_80392480[]; /* purin blink subroutine */
extern s32 D_ovl1_803924B4[];
extern s32 D_ovl1_803924E0[];
extern s32 D_ovl1_80392508[];
extern s32 D_ovl1_80392548[];
extern s32 D_ovl1_80392584[];
extern s32 D_ovl1_80392670[]; /* ness blink subroutine */
extern s32 D_ovl1_80392694[];

typedef struct SCSubsysMotionLink
{
	s32 *script;        /* demo motion script array */
	s32 operand_index;  /* word index of the Subroutine/Goto operand slot */
	s32 *target;        /* script the operand jumps to */
} SCSubsysMotionLink;

static SCSubsysMotionLink sSCSubsysMotionLinks[] =
{
	/* link */
	{ D_ovl1_803918A4, 1, D_ovl1_80391890 },
	{ D_ovl1_803918A4, 4, D_ovl1_80391890 },
	{ D_ovl1_803918A4, 7, D_ovl1_80391890 },
	{ D_ovl1_803918A4, 10, D_ovl1_803918A4 },
	/* mario */
	{ D_ovl1_80390DE4, 2, D_ovl1_80390DC0 },
	{ D_ovl1_80390DE4, 5, D_ovl1_80390DC0 },
	{ D_ovl1_80390DE4, 8, D_ovl1_80390DC0 },
	{ D_ovl1_80390DE4, 11, D_ovl1_80390DE4 },
	{ D_ovl1_80390E14, 2, D_ovl1_80390DC0 },
	{ D_ovl1_80390E14, 6, D_ovl1_80390DC0 },
	{ D_ovl1_80390E34, 2, D_ovl1_80390DC0 },
	{ D_ovl1_80390E34, 5, D_ovl1_80390DC0 },
	{ D_ovl1_80390E50, 2, D_ovl1_80390DC0 },
	{ D_ovl1_80390E50, 6, D_ovl1_80390DC0 },
	{ D_ovl1_80390E7C, 2, D_ovl1_80390DC0 },
	{ D_ovl1_80390E7C, 5, D_ovl1_80390DC0 },
	{ D_ovl1_80390E7C, 8, D_ovl1_80390E7C },
	/* luigi */
	{ D_ovl1_80391724, 2, D_ovl1_80391700 },
	{ D_ovl1_80391724, 5, D_ovl1_80391700 },
	{ D_ovl1_80391724, 8, D_ovl1_80391700 },
	{ D_ovl1_80391724, 11, D_ovl1_80391724 },
	/* ness */
	{ D_ovl1_80392694, 2, D_ovl1_80392670 },
	{ D_ovl1_80392694, 6, D_ovl1_80392670 },
	{ D_ovl1_80392694, 9, D_ovl1_80392670 },
	{ D_ovl1_80392694, 12, D_ovl1_80392694 },
	/* kirby */
	{ D_ovl1_80392020, 1, D_ovl1_80392010 },
	{ D_ovl1_80392020, 4, D_ovl1_80392010 },
	{ D_ovl1_80392020, 7, D_ovl1_80392010 },
	{ D_ovl1_80392020, 14, D_ovl1_80392020 },
	{ D_ovl1_80392088, 1, D_ovl1_80392010 },
	{ D_ovl1_80392088, 4, D_ovl1_80392010 },
	{ D_ovl1_80392088, 7, D_ovl1_80392010 },
	{ D_ovl1_80392088, 10, D_ovl1_80392088 },
	/* pikachu */
	{ D_ovl1_80392250, 1, D_ovl1_80392240 },
	{ D_ovl1_80392250, 4, D_ovl1_80392240 },
	{ D_ovl1_80392250, 7, D_ovl1_80392240 },
	{ D_ovl1_80392250, 10, D_ovl1_80392240 },
	{ D_ovl1_80392250, 13, D_ovl1_80392240 },
	{ D_ovl1_80392250, 16, D_ovl1_80392250 },
	{ D_ovl1_803922AC, 3, D_ovl1_80392240 },
	{ D_ovl1_803922AC, 8, D_ovl1_80392240 },
	{ D_ovl1_803922AC, 13, D_ovl1_80392240 },
	/* purin */
	{ D_ovl1_803924B4, 1, D_ovl1_80392480 },
	{ D_ovl1_803924B4, 4, D_ovl1_80392480 },
	{ D_ovl1_803924B4, 7, D_ovl1_80392480 },
	{ D_ovl1_803924B4, 10, D_ovl1_803924B4 },
	{ D_ovl1_803924E0, 2, D_ovl1_80392480 },
	{ D_ovl1_803924E0, 5, D_ovl1_80392480 },
	{ D_ovl1_803924E0, 8, D_ovl1_80392480 },
	{ D_ovl1_80392508, 2, D_ovl1_80392480 },
	{ D_ovl1_80392508, 5, D_ovl1_80392480 },
	{ D_ovl1_80392508, 8, D_ovl1_80392480 },
	{ D_ovl1_80392508, 11, D_ovl1_80392480 },
	{ D_ovl1_80392508, 14, D_ovl1_80392480 },
	{ D_ovl1_80392548, 10, D_ovl1_80392480 },
	{ D_ovl1_80392548, 13, D_ovl1_80392480 },
	{ D_ovl1_80392584, 1, D_ovl1_80392480 },
	{ D_ovl1_80392584, 4, D_ovl1_80392480 },
	/* yoshi */
	{ D_ovl1_80391BC4, 1, D_ovl1_80391B90 },
	{ D_ovl1_80391BC4, 4, D_ovl1_80391B90 },
	{ D_ovl1_80391BC4, 7, D_ovl1_80391B90 },
	{ D_ovl1_80391BC4, 10, D_ovl1_80391BC4 },
};

void scSubsysMotionLinkRelocTargets(void)
{
	static sb32 validated = FALSE;
	s32 i;

	/* Diagnostic kill switch: SSB64_NO_MOTIONFIX=1 leaves the demo-script
	 * operands stubbed (the pre-fix behavior — scripts die at their first
	 * Subroutine/Goto). Lets a bisect separate "the scripts now run" from
	 * everything else without a rebuild. */
	if (getenv("SSB64_NO_MOTIONFIX") != NULL)
	{
		return;
	}

	for (i = 0; i < ARRAY_COUNT(sSCSubsysMotionLinks); i++)
	{
		SCSubsysMotionLink *link = &sSCSubsysMotionLinks[i];

		/* The word before the operand slot must be the Subroutine/Goto
		 * opcode the table was generated against; anything else means
		 * the scsubsysdata*.c source drifted and the index is stale. */
		u32 opcode = ((u32)link->script[link->operand_index - 1] >> 26) & 0x3F;

		if (opcode != nFTMotionEventSubroutine && opcode != nFTMotionEventGoto)
		{
			if (!validated)
			{
				port_log("scSubsysMotionLinkRelocTargets: link %d — word %d "
				         "precedes opcode %u, not Subroutine/Goto; table is "
				         "stale, skipping (regenerate from scsubsysdata*.c)\n",
				         (int)i, (int)link->operand_index, (unsigned)opcode);
			}
			continue;
		}
		link->script[link->operand_index] = portRelocRegisterPointer(link->target);
	}
	if (!validated)
	{
		port_log("scSubsysMotionLinkRelocTargets: %d demo-script Subroutine/Goto "
		         "targets linked\n", (int)ARRAY_COUNT(sSCSubsysMotionLinks));
	}
	validated = TRUE;
}

#endif /* PORT */
