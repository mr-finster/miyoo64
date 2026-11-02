/* aspref.c (test only, not shipped): lockstep check of the audio mixer (port/src/pc/mixer.c) against the classic
 * aspMain semantics as mupen64plus-rsp-hle's alist_audio.c / alist.c implement them (ADPCM: alist_adpcm, ENVMIXER:
 * alist_envmix_exp, RESAMPLE: alist_resample), transcribed; after the Pilotwings 64 port's tools/aspref.c.
 * This file includes mixer.c with its entry points renamed, so each aXxxImpl the game calls runs the reference on a
 * copy of the mixer's DMEM, then the port's on the real one, then compares the two. State blocks: the reference keeps
 * its own, keyed by address. Build: make PLAT=pc in port/ (the desktop proxy runs the C loops the NEON ones are
 * checked against), compile this file with build/us_pc/src/pc/mixer.o's flags (make PLAT=pc -n shows them) plus
 * -Isrc/pc, and relink build/us_pc/sm64 with it in place of mixer.o. Run headless (tools/run.sh's variables,
 * SM64_AUTO=2 SM64_EXIT_AT=3000). ASPREF_OUT=<file> gets the per-command counts every 600 audio frames and at exit.
 * The envelope: ASPREF_ENV=pj64 takes the ramp's first 8 samples to vol x rate, as Project64's and Azimer's HLE and
 * mupen64plus's own before its refactor 7ed91e5 (2014) do; by default it is today's mupen64plus, whose first step goes
 * to vol x rate^2, a lead of 8 samples over the whole ramp. sm64-port's lanes are the former.
 * Results (3000 frames, title and Castle Grounds): ADPCM 138,705 commands, none differ; RESAMPLE: 1-2 LSB (the
 * rounding of each tap); ENVMIXER with ASPREF_ENV=pj64: 1-2 LSB once the right channel's ramp starts from its own
 * volume (it started from the left's: 287,000 samples off by 4 to 501). */
#define aClearBufferImpl port_aClearBufferImpl
#define aLoadADPCMImpl port_aLoadADPCMImpl
#define aSetBufferImpl port_aSetBufferImpl
#define aDMEMMoveImpl port_aDMEMMoveImpl
#define aSetLoopImpl port_aSetLoopImpl
#define aADPCMdecImpl port_aADPCMdecImpl
#define aResampleImpl port_aResampleImpl
#define aSetVolumeImpl port_aSetVolumeImpl
#define aLoadBufferImpl port_aLoadBufferImpl
#define aSaveBufferImpl port_aSaveBufferImpl
#define aInterleaveImpl port_aInterleaveImpl
#define aMixImpl port_aMixImpl
#define aEnvMixerImpl port_aEnvMixerImpl
#include "../port/src/pc/mixer.c"
#undef aClearBufferImpl
#undef aLoadADPCMImpl
#undef aSetBufferImpl
#undef aDMEMMoveImpl
#undef aSetLoopImpl
#undef aADPCMdecImpl
#undef aResampleImpl
#undef aSetVolumeImpl
#undef aLoadBufferImpl
#undef aSaveBufferImpl
#undef aInterleaveImpl
#undef aMixImpl
#undef aEnvMixerImpl
#include <stdio.h>
#include <stdlib.h>

static uint8_t rd[BUF_SIZE + 64];
static struct { uint16_t in, out, count, dry_right, wet_left, wet_right; int16_t vol[2], target[2]; int32_t rate[2];
    int16_t dry, wet; int16_t table[16 * 8]; const int16_t *loop; } Rg;
static int16_t *rs(unsigned a) { return (int16_t *)(rd + (a & ~1u)); }
static int16_t clamp_s16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
static unsigned align(unsigned x, unsigned a) { return (x + a - 1) & ~(a - 1); }

#define NS 4096
static struct { const void *a; uint8_t d[80]; int used; } sts[NS];
static uint8_t *state(const void *a, int len) {
    unsigned h = (unsigned)(((uintptr_t)a * 2654435761u) % NS);
    while (sts[h].used && sts[h].a != a) h = (h + 1) % NS;
    if (!sts[h].used) { sts[h].used = 1; sts[h].a = a; memcpy(sts[h].d, a, len); }
    return sts[h].d;
}

static unsigned long nbig_scale;
static void ref_adpcm(uint8_t flags, const void *addr) {
    unsigned dmemo = Rg.out, dmemi = Rg.in, count = align(Rg.count, 32);
    int16_t last[16]; int i, j;
    int16_t *st = (int16_t *)state(addr, 32);
    if (flags & A_INIT) memset(last, 0, 32);
    else if (flags & A_LOOP) memcpy(last, Rg.loop, 32);
    else memcpy(last, st, 32);
    for (i = 0; i < 16; i++, dmemo += 2) *rs(dmemo) = last[i];
    while (count) {
        int16_t frame[16]; uint8_t code = rd[dmemi++]; unsigned scale = code >> 4;
        const int16_t *cb = Rg.table + ((code & 0xf) << 4);
        unsigned rshift = scale < 12 ? 12 - scale : 0;
        if (scale > 12) nbig_scale++;
        for (i = 0; i < 8; i++) { uint8_t b = rd[dmemi++];
            frame[2*i] = (int16_t)((uint16_t)(b & 0xf0) << 8) >> rshift;
            frame[2*i+1] = (int16_t)((uint16_t)(b & 0x0f) << 12) >> rshift; }
        for (j = 0; j < 2; j++) {
            const int16_t *src = frame + 8 * j;
            int16_t l1 = j ? last[6] : last[14], l2 = j ? last[7] : last[15], tmp[8];
            for (i = 0; i < 8; i++) { int k; int32_t acc = (int32_t)src[i] << 11;
                acc += cb[i] * l1 + cb[8 + i] * l2;
                for (k = 0; k < i; k++) acc += cb[8 + i - 1 - k] * src[k];
                tmp[i] = clamp_s16(acc >> 11); }
            memcpy(last + 8 * j, tmp, 16);
        }
        for (i = 0; i < 16; i++, dmemo += 2) *rs(dmemo) = last[i];
        count -= 32;
    }
    memcpy(st, last, 32);
}
static int env_pj64;
static void ref_envmix(uint8_t flags, const void *addr) {
    int aux = flags & A_AUX, n = aux ? 4 : 2;
    int16_t *in = rs(Rg.in), *dl = rs(Rg.out), *dr = rs(Rg.dry_right), *wl = rs(Rg.wet_left), *wr = rs(Rg.wet_right);
    int32_t val[2], tgt[2], step[2], seq[2], rate[2]; int16_t dry = Rg.dry, wet = Rg.wet; int32_t *sv = (int32_t *)state(addr, 80);
    int x, y, c, ptr = 0, count = Rg.count;
    if (flags & A_INIT) for (c = 0; c < 2; c++) { val[c] = Rg.vol[c] << 16; tgt[c] = Rg.target[c] << 16; rate[c] = Rg.rate[c]; seq[c] = (int32_t)((int64_t)Rg.vol[c] * Rg.rate[c]); }
    else { for (c = 0; c < 2; c++) { val[c] = sv[c]; tgt[c] = sv[2+c]; rate[c] = sv[4+c]; seq[c] = sv[6+c]; } dry = sv[8]; wet = sv[9]; }
    for (c = 0; c < 2; c++) step[c] = tgt[c] - val[c];
    for (y = 0; y < count; y += 16) {
        /* ASPREF_ENV=pj64: the ramp's first block ends at vol x rate (Project64 / Azimer, and mupen64plus before its
         * 2014 refactor 7ed91e5); by default the current mupen64plus, whose first block ends at vol x rate^2 */
        for (c = 0; c < 2; c++) if (step[c]) {
            if (env_pj64) { step[c] = (seq[c] - val[c]) >> 3; seq[c] = ((int64_t)seq[c] * rate[c]) >> 16; }
            else { seq[c] = ((int64_t)seq[c] * rate[c]) >> 16; step[c] = (seq[c] - val[c]) >> 3; } }
        for (x = 0; x < 8; x++) {
            int16_t v[2], g[4]; int16_t *b[4] = { dl + ptr, dr + ptr, wl + ptr, wr + ptr }; int i;
            for (c = 0; c < 2; c++) { val[c] += step[c];
                if (step[c] <= 0 ? val[c] <= tgt[c] : val[c] >= tgt[c]) { val[c] = tgt[c]; step[c] = 0; }
                v[c] = (int16_t)(val[c] >> 16); }
            g[0] = clamp_s16((v[0] * dry + 0x4000) >> 15); g[1] = clamp_s16((v[1] * dry + 0x4000) >> 15);
            g[2] = clamp_s16((v[0] * wet + 0x4000) >> 15); g[3] = clamp_s16((v[1] * wet + 0x4000) >> 15);
            for (i = 0; i < n; i++) *b[i] = clamp_s16(*b[i] + ((in[ptr] * g[i]) >> 15));
            ptr++;
        }
    }
    for (c = 0; c < 2; c++) { sv[c] = val[c]; sv[2+c] = tgt[c]; sv[4+c] = rate[c]; sv[6+c] = seq[c]; } sv[8] = dry; sv[9] = wet;
}
static void ref_resample(uint8_t flags, uint16_t pitch, const void *addr) {
    unsigned ipos = Rg.in >> 1, opos = Rg.out >> 1, count = align(Rg.count, 16) >> 1; uint32_t acc = 0, p = (uint32_t)pitch << 1; int k;
    int16_t *sv = (int16_t *)state(addr, 32);
    ipos -= 4;
    if (flags & A_INIT) { for (k = 0; k < 4; k++) *rs((ipos + k) << 1) = 0; }
    else { for (k = 0; k < 4; k++) *rs((ipos + k) << 1) = sv[k]; acc = (uint16_t)sv[4]; }
    while (count--) {
        const int16_t *lut = resample_table[(acc >> 10) & 63];
        *rs((opos++) << 1) = clamp_s16((*rs(ipos << 1) * lut[0] + *rs((ipos + 1) << 1) * lut[1] + *rs((ipos + 2) << 1) * lut[2] + *rs((ipos + 3) << 1) * lut[3]) >> 15);
        acc += p; ipos += acc >> 16; acc &= 0xffff;
    }
    for (k = 0; k < 4; k++) sv[k] = *rs((ipos + k) << 1);
    sv[4] = acc;
}

enum { C_ADPCM, C_ENVMIX, C_RESAMPLE, C_RESAMPLE_F2, C_N };
static const char *names[C_N] = { "ADPCM", "ENVMIXER", "RESAMPLE", "RESAMPLE+2" };
static unsigned long ncmd[C_N], nbad[C_N], nsamp[C_N], hist[C_N][4]; static int maxd[C_N]; static unsigned long frames;
static void report(void) {
    int i; FILE *o = fopen(getenv("ASPREF_OUT") ? getenv("ASPREF_OUT") : "/dev/stderr", "w"); if (!o) return;
    fprintf(o, "[aspref] %lu ADPCM frames with a scale over 12\n", nbig_scale);
    for (i = 0; i < C_N; i++) if (ncmd[i]) fprintf(o, "[aspref] %-10s %8lu cmds %8lu differ (%lu samples, max %d; diff 1: %lu, 2-3: %lu, 4-63: %lu, 64+: %lu)\n",
        names[i], ncmd[i], nbad[i], nsamp[i], maxd[i], hist[i][0], hist[i][1], hist[i][2], hist[i][3]);
    fclose(o);
}
static void before(void) {
    static int init;
    if (!init) { init = 1; atexit(report); env_pj64 = getenv("ASPREF_ENV") && !strcmp(getenv("ASPREF_ENV"), "pj64"); }
    memcpy(rd, rspa.buf.as_u8, BUF_SIZE);
}
static void after(int c) {
    unsigned a; int bad = 0, md = 0;
    for (a = 0; a + 1 < BUF_SIZE; a += 2) {
        int16_t p = *(int16_t *)(rspa.buf.as_u8 + a), r = *(int16_t *)(rd + a);
        if (p != r) { int d = abs(p - r); bad = 1; nsamp[c]++; if (d > md) md = d;
            hist[c][d == 1 ? 0 : d < 4 ? 1 : d < 64 ? 2 : 3]++; }
    }
    ncmd[c]++; if (bad) { nbad[c]++; if (md > maxd[c]) maxd[c] = md; }
}

void aClearBufferImpl(uint16_t addr, int nbytes) { port_aClearBufferImpl(addr, nbytes); }
void aLoadADPCMImpl(int n, const int16_t *book) { memcpy(Rg.table, book, n > (int)sizeof Rg.table ? (int)sizeof Rg.table : n); port_aLoadADPCMImpl(n, book); }
void aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes) {
    if (flags & A_AUX) { Rg.dry_right = in; Rg.wet_left = out; Rg.wet_right = nbytes; } else { Rg.in = in; Rg.out = out; Rg.count = nbytes; }
    port_aSetBufferImpl(flags, in, out, nbytes);
}
void aDMEMMoveImpl(uint16_t i, uint16_t o, int n) { port_aDMEMMoveImpl(i, o, n); }
void aSetLoopImpl(ADPCM_STATE *s) { Rg.loop = (const int16_t *)s; port_aSetLoopImpl(s); }
void aSetVolumeImpl(uint8_t flags, int16_t v, int16_t t, int16_t r) {
    if (flags & A_AUX) { Rg.dry = v; Rg.wet = r; }
    else { int lr = (flags & A_LEFT) ? 0 : 1; if (flags & A_VOL) Rg.vol[lr] = v; else { Rg.target[lr] = v; Rg.rate[lr] = (int32_t)((uint32_t)(uint16_t)t << 16 | (uint16_t)r); } }
    port_aSetVolumeImpl(flags, v, t, r);
}
void aLoadBufferImpl(const void *s) { port_aLoadBufferImpl(s); }
void aSaveBufferImpl(int16_t *d) { port_aSaveBufferImpl(d); }
void aInterleaveImpl(uint16_t l, uint16_t r) { frames++; if (frames % 600 == 0) report(); port_aInterleaveImpl(l, r); }
void aMixImpl(int16_t g, uint16_t i, uint16_t o) { port_aMixImpl(g, i, o); }
void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state) { before(); ref_adpcm(flags, state); port_aADPCMdecImpl(flags, state); after(C_ADPCM); }
/* ASPREF_DEBUG=<n>: the first n envelope mixes off by 64 or more, sample by sample (what each side added) */
void aEnvMixerImpl(uint8_t flags, ENVMIX_STATE state) {
    static int dbg = -1; static uint8_t pre[BUF_SIZE]; int md = maxd[C_ENVMIX];
    if (dbg < 0) dbg = getenv("ASPREF_DEBUG") ? atoi(getenv("ASPREF_DEBUG")) : 0;
    before(); memcpy(pre, rd, BUF_SIZE); ref_envmix(flags, state); port_aEnvMixerImpl(flags, state);
    maxd[C_ENVMIX] = 0; after(C_ENVMIX);
    if (dbg > 0 && maxd[C_ENVMIX] >= 64) {
        int i; const int16_t *p = (const int16_t *)rspa.buf.as_u8, *q = (const int16_t *)pre;
        dbg--;
        fprintf(stderr, "ENV flags %x vol %d %d tgt %d %d rate %08x %08x dry %d wet %d count %d\n", flags, Rg.vol[0], Rg.vol[1],
                Rg.target[0], Rg.target[1], Rg.rate[0], Rg.rate[1], Rg.dry, Rg.wet, Rg.count);
        for (i = 0; i < Rg.count / 2; i++) {
            int l = Rg.out / 2 + i, r = Rg.dry_right / 2 + i;
            fprintf(stderr, " %3d in %6d L port %6d ref %6d | R port %6d ref %6d\n", i, *rs(Rg.in + 2 * i), p[l] - q[l],
                    *rs(2 * l) - q[l], p[r] - q[r], *rs(2 * r) - q[r]);
        }
    }
    if (md > maxd[C_ENVMIX]) maxd[C_ENVMIX] = md;
}
void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state) {
    before(); ref_resample(flags, pitch, state); port_aResampleImpl(flags, pitch, state); after(flags & 2 ? C_RESAMPLE_F2 : C_RESAMPLE);
}
