/*====================================================================
 * bnkf.c
 *
 * Copyright 1993, Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * This is UNPUBLISHED PROPRIETARY SOURCE CODE of Silicon Graphics,
 * Inc.; the contents of this file may not be disclosed to third
 * parties, copied or duplicated in any form, in whole or in part,
 * without the prior written permission of Silicon Graphics, Inc.
 *
 * RESTRICTED RIGHTS LEGEND:
 * Use, duplication or disclosure by the Government is subject to
 * restrictions as set forth in subdivision (c)(1)(ii) of the Rights
 * in Technical Data and Computer Software clause at DFARS
 * 252.227-7013, and/or in similar or successor clauses in the FAR,
 * DOD or NASA FAR Supplement. Unpublished - rights reserved under the
 * Copyright Laws of the United States.
 *====================================================================*/

#include <libaudio.h>
#include <os_internal.h>
#include <ultraerror.h>

/*
 * ### when the file format settles down a little, I'll remove these
 * ### for efficiency.
 */
static  void _bnkfPatchBank(ALBank *bank, s32 offset, s32 table);
static  void _bnkfPatchInst(ALInstrument *i, s32 offset, s32 table);
static  void _bnkfPatchSound(ALSound *s, s32 offset, s32 table);
static  void _bnkfPatchWaveTable(ALWaveTable *w, s32 offset, s32 table);

#if defined(PORT)
/*
 * Port: bank files come from the ROM big-endian. Each structure is swapped to
 * host order in the same single pass that turns its offsets into pointers
 * (the flags byte already marks bank/instrument/sound/wavetable as visited).
 * Envelopes, ADPCM books and loops carry no flag and can be shared between
 * sounds, so they are swapped once through a small visited set.
 */
static u16 bnkf_sw16(u16 v) { return (u16) ((v >> 8) | (v << 8)); }
static u32 bnkf_sw32(u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }
#define SW16(x) ((x) = (__typeof__(x)) bnkf_sw16((u16) (x)))
#define SW32(x) ((x) = (__typeof__(x)) bnkf_sw32((u32) (x)))
#define BNKF_SEEN_MAX 4096
static void *sBnkfSeen[BNKF_SEEN_MAX];
static int sBnkfSeenCount;
static int bnkf_first_visit(void *p) {
    int i;
    for (i = 0; i < sBnkfSeenCount; i++) {
        if (sBnkfSeen[i] == p) return 0;
    }
    if (sBnkfSeenCount < BNKF_SEEN_MAX) sBnkfSeen[sBnkfSeenCount++] = p;
    return 1;
}
#endif

void alSeqFileNew(ALSeqFile *file, u8 *base)
{
    s32 offset = (s32) base;
    s32 i;

#if defined(PORT)
    /* Port: the ALSeqFile header arrives big-endian from the ROM. */
    SW16(file->revision);
    SW16(file->seqCount);
    for (i = 0; i < file->seqCount; i++) {
        SW32(file->seqArray[i].offset);
        SW32(file->seqArray[i].len);
    }
#endif
    
    /*
     * patch the file so that offsets are pointers
     */
    for (i = 0; i < file->seqCount; i++) {
        file->seqArray[i].offset = (u8 *)((u8 *)file->seqArray[i].offset + offset);
    }
}

void alBnkfNew(ALBankFile *file, u8 *table)
{
    s32 offset = (s32) file;
    s32 woffset = (s32) table;
    
    s32 i;
    
    /*
     * check the file format revision in debug libraries
     */
#if defined(PORT)
    sBnkfSeenCount = 0;
    SW16(file->revision);
    SW16(file->bankCount);
    for (i = 0; i < file->bankCount; i++) {
        SW32(file->bankArray[i]);
    }
#endif
    ALFailIf(file->revision != AL_BANK_VERSION, ERR_ALBNKFNEW);
    
    /*
     * patch the file so that offsets are pointers
     */
    for (i = 0; i < file->bankCount; i++) {
        file->bankArray[i] = (ALBank *)((u8 *)file->bankArray[i] + offset);
        if(file->bankArray[i])
            _bnkfPatchBank(file->bankArray[i], offset, woffset);
    }
}

void _bnkfPatchBank(ALBank *bank, s32 offset, s32 table) 
{
    s32 i;
    
    if (bank->flags)
        return;

    bank->flags = 1;
#if defined(PORT)
    SW16(bank->instCount);
    SW32(bank->sampleRate);
    SW32(bank->percussion);
    for (i = 0; i < bank->instCount; i++) {
        SW32(bank->instArray[i]);
    }
#endif

    if (bank->percussion) {
        bank->percussion = (ALInstrument *)((u8 *)bank->percussion + offset);
        _bnkfPatchInst(bank->percussion, offset, table);
    }
    
    for (i = 0; i < bank->instCount; i++) {
        bank->instArray[i] = (ALInstrument *)((u8 *)bank->instArray[i] +
                                              offset);
        if(bank->instArray[i])
            _bnkfPatchInst(bank->instArray[i], offset, table);
    }
}

void _bnkfPatchInst(ALInstrument *inst, s32 offset, s32 table)
{
    s32 i;

    if (inst->flags)
        return;

    inst->flags = 1;
#if defined(PORT)
    SW16(inst->bendRange);
    SW16(inst->soundCount);
    for (i = 0; i < inst->soundCount; i++) {
        SW32(inst->soundArray[i]);
    }
#endif
    
    for (i = 0; i < inst->soundCount; i++) {
        inst->soundArray[i] = (ALSound *)((u8 *)inst->soundArray[i] +
                                          offset);
        _bnkfPatchSound(inst->soundArray[i], offset, table);

    }
}

void _bnkfPatchSound(ALSound *s, s32 offset, s32 table)
{
    if (s->flags)
        return;

    s->flags = 1;
#if defined(PORT)
    SW32(s->envelope);
    SW32(s->keyMap);
    SW32(s->wavetable);
#endif
    
    s->envelope  = (ALEnvelope *)((u8 *)s->envelope + offset);
    s->keyMap    = (ALKeyMap *)((u8 *)s->keyMap + offset);
#if defined(PORT)
    if (bnkf_first_visit(s->envelope)) {
        SW32(s->envelope->attackTime);
        SW32(s->envelope->decayTime);
        SW32(s->envelope->releaseTime);
    }
#endif

    s->wavetable = (ALWaveTable *)((u8 *)s->wavetable + offset);
    _bnkfPatchWaveTable(s->wavetable, offset, table);
}

void _bnkfPatchWaveTable(ALWaveTable *w, s32 offset, s32 table)
{
    if (w->flags)
        return;

    w->flags = 1;
#if defined(PORT)
    SW32(w->base);
    SW32(w->len);
    SW32(w->waveInfo.adpcmWave.loop); /* rawWave.loop aliases the same word */
    if (w->type == AL_ADPCM_WAVE) {
        SW32(w->waveInfo.adpcmWave.book);
    }
#endif
    
    w->base += table;

    /* sct 2/14/96 - patch wavetable loop info based on type. */
    if (w->type == AL_ADPCM_WAVE)
    {
	w->waveInfo.adpcmWave.book  = (ALADPCMBook *)((u8 *)w->waveInfo.adpcmWave.book + offset);
	if (w->waveInfo.adpcmWave.loop)
	    w->waveInfo.adpcmWave.loop = (ALADPCMloop *)((u8 *)w->waveInfo.adpcmWave.loop + offset);
#if defined(PORT)
        if (bnkf_first_visit(w->waveInfo.adpcmWave.book)) {
            ALADPCMBook *b = w->waveInfo.adpcmWave.book;
            s32 n, k;
            SW32(b->order);
            SW32(b->npredictors);
            n = b->order * b->npredictors * 8;
            for (k = 0; k < n; k++) SW16(b->book[k]);
        }
        if (w->waveInfo.adpcmWave.loop && bnkf_first_visit(w->waveInfo.adpcmWave.loop)) {
            ALADPCMloop *l = w->waveInfo.adpcmWave.loop;
            s32 k;
            SW32(l->start);
            SW32(l->end);
            SW32(l->count);
            for (k = 0; k < 16; k++) SW16(l->state[k]);
        }
#endif
    }
    else if (w->type == AL_RAW16_WAVE)
    {
	if (w->waveInfo.rawWave.loop)
	    w->waveInfo.rawWave.loop = (ALRawLoop *)((u8 *)w->waveInfo.rawWave.loop + offset);
#if defined(PORT)
        if (w->waveInfo.rawWave.loop && bnkf_first_visit(w->waveInfo.rawWave.loop)) {
            SW32(w->waveInfo.rawWave.loop->start);
            SW32(w->waveInfo.rawWave.loop->end);
            SW32(w->waveInfo.rawWave.loop->count);
        }
#endif
    }	
}
