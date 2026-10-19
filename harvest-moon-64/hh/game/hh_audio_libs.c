/* The few libultra audio helpers n_audio uses from the old library (libultra/src/audio: sl.c, drvrnew.c, reverb.c),
 * without the rest of it. Game side. */
#include <n_synthInternals.h>

#define SCALE 16384
#define RANGE 2.0

void alLink(ALLink *ln, ALLink *to) {
    ln->next = to->next;
    ln->prev = to;
    if (to->next) to->next->prev = ln;
    to->next = ln;
}

void alUnlink(ALLink *ln) {
    if (ln->next) ln->next->prev = ln->prev;
    if (ln->prev) ln->prev->next = ln->next;
}

void init_lpfilter(ALLowPass *lp) {
    s32 i, temp;
    s16 fc;
    f64 ffc, fcoef;

    temp = lp->fc * SCALE;
    fc = temp >> 15;
    lp->fgain = SCALE - fc;
    lp->first = 1;
    for (i = 0; i < 8; i++) lp->fcvec.fccoef[i] = 0;
    lp->fcvec.fccoef[i++] = fc;
    fcoef = ffc = (f64) fc / SCALE;
    for (; i < 16; i++) {
        fcoef *= ffc;
        lp->fcvec.fccoef[i] = (s16) (fcoef * SCALE);
    }
}

f32 _doModFunc(ALDelay *d, s32 count) {
    f32 val;

    d->rsval += d->rsinc * count;
    d->rsval = (d->rsval > RANGE) ? d->rsval - (RANGE * 2) : d->rsval;
    val = d->rsval;
    val = (val < 0) ? -val : val;
    val -= RANGE / 2;
    return d->rsgain * val;
}

void osWritebackDCacheAll(void) {}
