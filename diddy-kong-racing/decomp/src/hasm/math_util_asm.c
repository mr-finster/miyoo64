/**
 * The trigonometry and data words from src/hasm/ido/math_util.s (port: the two
 * tables are generated at build time by tools/gen_trig_tables.py).
 *
 * Transcribed by the DKR-GC project (RyoSaeba89/DKR-GC, CC0).
 *
 * That file is hand-written MIPS with no C counterpart in the decomp, so a
 * non-MIPS build has to supply its data words and its sine/cosine entry points.
 *
 * The functions are transcribed from the same file. Two details of the original
 * matter and are preserved here:
 *
 *  - Everything is unsigned 32-bit arithmetic on the low 16 bits of the angle.
 *    The MIPS uses srl and lhu throughout, and the interpolation step relies on
 *    the wraparound, so the C uses u32 and reads the table through a u16 * even
 *    though gSineTable is declared s16 (its 0x8000 entry does not fit in one).
 *  - The file is assembled in the default reorder mode -- it only asks for
 *    .set noreorder further down, around the CP0 code -- so the instruction
 *    after each branch is not a delay slot and does not run when the branch is
 *    taken. The conditionals below are therefore ordinary C conditionals.
 *
 * coss_* are the sins_* entry points offset by a quarter turn; in the assembly
 * they literally are, via addiu + fall-through into an XLEAF.
 */
#include "macros.h"
#include "math_util.h"
#include "types.h"

extern s16 gSineTable[1025];
extern s16 gArcTanTable[1025];

u8 gIntDisFlag = 0x00;
s32 gCurrentRNGSeed = 0x5141564D; /* 'QAVM' */
s32 gPrevRNGSeed = 0x5141564D;    /* 'QAVM' */

/* Official name: mathSinInterp */
s32 sins_s16(s16 angle) {
    const u16 *tbl;
    u32 a = (u32) (u16) angle;
    u32 cur, next, frac, delta, v;

    /* bgez (a << 17): bit 14 clear means the first half of the quadrant pair */
    if (a & 0x4000) {
        a ^= 0x7FFF;
    }

    tbl = (const u16 *) ((const u8 *) gSineTable + ((a >> 3) & 0x7FE));
    cur = tbl[0];
    next = tbl[1];

    frac = a & 0xF;
    delta = ((next - cur) * frac) >> 3;
    v = (cur << 1) + delta;

    /* bgez (a << 16): bit 15 set means the second half of the circle */
    if (a & 0x8000) {
        return -(s32) v;
    }
    return (s32) v;
}

/* Official name: mathCosInterp */
s32 coss_s16(s16 angle) {
    return sins_s16((s16) (angle + 0x4000));
}

/* Official name: mathSin -- same table, without the interpolation step. */
s32 sins_2(s16 angle) {
    const u16 *tbl;
    u32 a = (u32) (u16) angle;
    u32 v;

    if (a & 0x4000) {
        a ^= 0x7FFF;
    }

    tbl = (const u16 *) ((const u8 *) gSineTable + ((a >> 3) & 0x7FE));
    v = (u32) tbl[0] << 1;

    if (a & 0x8000) {
        return -(s32) v;
    }
    return (s32) v;
}

/* Official name: mathCos */
s32 coss_2(s16 angle) {
    return sins_2((s16) (angle + 0x4000));
}

f32 sins_f(s16 angle) {
    return (f32) sins_s16(angle) * (1.0f / 65536.0f);
}

f32 coss_f(s16 angle) {
    return (f32) coss_s16(angle) * (1.0f / 65536.0f);
}
