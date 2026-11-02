/* aspref.c (test only, not shipped): lockstep check of the C audio mixer (base-psp/src/port/audio/mixer.c, which the
 * game's synthesis calls directly) against Mario Kart 64's NEAD microcode as mupen64plus-rsp-hle's alist_nead.c /
 * alist.c implement it (alist_process_nead_mk: ADPCM alist_adpcm, RESAMPLE alist_resample, ENVSETUP1_MK / ENVSETUP2 +
 * ENVMIXER_MK alist_envmix_nead), transcribed; after the Pilotwings 64 port's tools/aspref.c. This file includes
 * mixer.c with its entry points renamed: each one the game calls runs the reference on a copy of the mixer's DMEM, then
 * the port's on the real one, and the DMEM after the two is compared. The reference keeps its own ADPCM / resampler
 * states, keyed by address. Build (the desktop proxy runs the C loops the NEON ones are checked against): standalone,
 * this file compiled with mixer.o's flags in place of mixer.o; where the game checks its link layout, in the build
 * itself for the test only: the eight #defines below at the top of mixer.c, and at its end `#define ASPREF_INTREE`
 * and `#include "../../../../tools/aspref.c"`, then make -f Makefile.handheld PLAT=pc in base-psp.
 * ASPREF_OUT=<file> gets the per-command counts every 30000 commands and at exit.
 * Results (MK64_AUTO=1 MK64_EXIT_AT=8000): ADPCM 535,827 and ENVMIXER 487,803 commands, none differ; RESAMPLE 1-2 LSB
 * on every command (the C loop rounds each tap, rsp-hle rounds the sum). */
#ifndef ASPREF_INTREE
#define aSetBufferImpl port_aSetBufferImpl
#define aLoadADPCMImpl port_aLoadADPCMImpl
#define aSetLoopImpl port_aSetLoopImpl
#define aADPCMdecImpl port_aADPCMdecImpl
#define aResampleImpl port_aResampleImpl
#define aEnvSetup1Impl port_aEnvSetup1Impl
#define aEnvSetup2Impl port_aEnvSetup2Impl
#define aEnvMixerImpl port_aEnvMixerImpl
#include "../base-psp/src/port/audio/mixer.c"
#endif
#undef aSetBufferImpl
#undef aLoadADPCMImpl
#undef aSetLoopImpl
#undef aADPCMdecImpl
#undef aResampleImpl
#undef aEnvSetup1Impl
#undef aEnvSetup2Impl
#undef aEnvMixerImpl
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t rd[BUF_SIZE + 64];
static struct { uint16_t in, out, count; int16_t table[16 * 8]; const int16_t *loop; uint16_t env_values[3], env_steps[3]; } Rg;
static int16_t *rs(unsigned a) { return (int16_t *)(rd + (a & 0xffe)); }
static uint8_t *ru8(unsigned a) { return rd + (a & 0xfff); }
static int16_t clamp_s16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
static unsigned align(unsigned x, unsigned a) { return (x + a - 1) & ~(a - 1); }

#define NS 4096
static struct { const void *a; uint8_t d[32]; int used; } *sts; /* on the heap: the link layout stays the game's */
static uint8_t *state(const void *a, int len) {
    if (!sts) sts = calloc(NS, sizeof *sts);
    unsigned h = (unsigned)(((uintptr_t)a * 2654435761u) % NS);
    while (sts[h].used && sts[h].a != a) h = (h + 1) % NS;
    if (!sts[h].used) { sts[h].used = 1; sts[h].a = a; memcpy(sts[h].d, a, len); }
    return sts[h].d;
}

static unsigned long nbig_scale;
static void ref_adpcm(uint8_t flags, const void *addr) {
    unsigned dmemo = Rg.out, dmemi = Rg.in, count = align(Rg.count, 32), two = flags & 4;
    int16_t last[16]; int i, j;
    int16_t *st = (int16_t *)state(addr, 32);
    if (flags & 1) memset(last, 0, 32);
    else if (flags & 2) memcpy(last, Rg.loop, 32);
    else memcpy(last, st, 32);
    for (i = 0; i < 16; i++, dmemo += 2) *rs(dmemo) = last[i];
    while (count) {
        int16_t frame[16]; uint8_t code = *ru8(dmemi++); unsigned scale = code >> 4;
        const int16_t *cb = Rg.table + ((code & 0xf) << 4);
        if (scale > (two ? 14u : 12u)) nbig_scale++;
        if (!two) {
            unsigned rshift = scale < 12 ? 12 - scale : 0;
            for (i = 0; i < 8; i++) { uint8_t b = *ru8(dmemi++);
                frame[2*i] = (int16_t)((uint16_t)(b & 0xf0) << 8) >> rshift;
                frame[2*i+1] = (int16_t)((uint16_t)(b & 0x0f) << 12) >> rshift; }
        } else {
            unsigned rshift = scale < 14 ? 14 - scale : 0;
            for (i = 0; i < 4; i++) { uint8_t b = *ru8(dmemi++);
                frame[4*i] = (int16_t)((uint16_t)(b & 0xc0) << 8) >> rshift;
                frame[4*i+1] = (int16_t)((uint16_t)(b & 0x30) << 10) >> rshift;
                frame[4*i+2] = (int16_t)((uint16_t)(b & 0x0c) << 12) >> rshift;
                frame[4*i+3] = (int16_t)((uint16_t)(b & 0x03) << 14) >> rshift; }
        }
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
static void ref_resample(uint8_t flags, uint16_t pitch, const void *addr) {
    unsigned ipos = Rg.in >> 1, opos = Rg.out >> 1, count = align(Rg.count, 16) >> 1; uint32_t acc = 0, p = (uint32_t)pitch << 1; int k;
    int16_t *sv = (int16_t *)state(addr, 32);
    ipos -= 4;
    if (flags & 1) { for (k = 0; k < 4; k++) *rs((ipos + k) << 1) = 0; }
    else { for (k = 0; k < 4; k++) *rs((ipos + k) << 1) = sv[k]; acc = (uint16_t)sv[4]; }
    while (count--) {
        const int16_t *lut = resample_table[(acc >> 10) & 63];
        *rs((opos++) << 1) = clamp_s16((*rs(ipos << 1) * lut[0] + *rs((ipos + 1) << 1) * lut[1] + *rs((ipos + 2) << 1) * lut[2] + *rs((ipos + 3) << 1) * lut[3]) >> 15);
        acc += p; ipos += acc >> 16; acc &= 0xffff;
    }
    for (k = 0; k < 4; k++) sv[k] = *rs((ipos + k) << 1);
    sv[4] = acc;
}
static void ref_envmix(unsigned dmemi, unsigned n, int negl, int negr, unsigned odl, unsigned odr, unsigned owl, unsigned owr) {
    int16_t xors[4] = { (int16_t)-negl, (int16_t)-negr, 0, 0 }; /* ENVMIXER_MK: no wet sign flags, no wet swap */
    unsigned count = align(n & 0xff, 8), i;
    int16_t *in = rs(dmemi), *dl = rs(odl), *dr = rs(odr), *wl = rs(owl), *wr = rs(owr);
    while (count) {
        for (i = 0; i < 8; i++) {
            int16_t l = (((int32_t)in[i] * (uint32_t)Rg.env_values[0]) >> 16) ^ xors[0];
            int16_t r = (((int32_t)in[i] * (uint32_t)Rg.env_values[1]) >> 16) ^ xors[1];
            int16_t l2 = (((int32_t)l * (uint32_t)Rg.env_values[2]) >> 16) ^ xors[2];
            int16_t r2 = (((int32_t)r * (uint32_t)Rg.env_values[2]) >> 16) ^ xors[3];
            dl[i] = clamp_s16(dl[i] + l); dr[i] = clamp_s16(dr[i] + r); wl[i] = clamp_s16(wl[i] + l2); wr[i] = clamp_s16(wr[i] + r2);
        }
        for (i = 0; i < 3; i++) Rg.env_values[i] += Rg.env_steps[i];
        dl += 8; dr += 8; wl += 8; wr += 8; in += 8; count -= 8;
    }
}

enum { C_ADPCM, C_ENVMIX, C_RESAMPLE, C_N };
static const char *names[C_N] = { "ADPCM", "ENVMIXER", "RESAMPLE" };
static unsigned long ncmd[C_N], nbad[C_N], nsamp[C_N], hist[C_N][4]; static int maxd[C_N]; static unsigned long tasks;
static void report(void) {
    int i; FILE *o = fopen(getenv("ASPREF_OUT") ? getenv("ASPREF_OUT") : "/dev/stderr", "w"); if (!o) return;
    fprintf(o, "[aspref] %lu ADPCM frames with a scale over 12 (4-bit) / 14 (2-bit)\n", nbig_scale);
    for (i = 0; i < C_N; i++) if (ncmd[i]) fprintf(o, "[aspref] %-10s %8lu cmds %8lu differ (%lu samples, max %d; diff 1: %lu, 2-3: %lu, 4-63: %lu, 64+: %lu)\n",
        names[i], ncmd[i], nbad[i], nsamp[i], maxd[i], hist[i][0], hist[i][1], hist[i][2], hist[i][3]);
    fclose(o);
}
static void compare(int c) {
    unsigned a; int bad = 0, md = 0;
    for (a = 0; a + 1 < 0x1000; a += 2) {
        int16_t p = *(int16_t *)(rspa.buf.as_u8 + a), r = *(int16_t *)(rd + a);
        if (p != r) { int d = abs(p - r); bad = 1; nsamp[c]++; if (d > md) md = d;
            hist[c][d == 1 ? 0 : d < 4 ? 1 : d < 64 ? 2 : 3]++; }
    }
    ncmd[c]++; if (bad) { nbad[c]++; if (md > maxd[c]) maxd[c] = md; }
}

static void before(void) { static int init; static unsigned long calls; if (!init) { init = 1; atexit(report); } if (++calls % 30000 == 0) report(); memcpy(rd, rspa.buf.as_u8, 0x1000); }
void aSetBufferImpl(uint8_t f, uint16_t in, uint16_t out, uint16_t n) { Rg.in = in; Rg.out = out; Rg.count = n; port_aSetBufferImpl(f, in, out, n); }
void aLoadADPCMImpl(int n, const int16_t *book) { memcpy(Rg.table, book, n > (int)sizeof Rg.table ? (int)sizeof Rg.table : n); port_aLoadADPCMImpl(n, book); }
void aSetLoopImpl(ADPCM_STATE *s) { Rg.loop = (const int16_t *)s; port_aSetLoopImpl(s); }
void aEnvSetup1Impl(uint8_t vw, uint16_t rw, uint16_t rl, uint16_t rr) {
    Rg.env_values[2] = (uint16_t)(vw << 8); Rg.env_steps[2] = 0; Rg.env_steps[0] = rl; Rg.env_steps[1] = rr; (void)rw; /* ENVSETUP1_MK */
    port_aEnvSetup1Impl(vw, rw, rl, rr);
}
void aEnvSetup2Impl(uint16_t l, uint16_t r) { Rg.env_values[0] = l; Rg.env_values[1] = r; port_aEnvSetup2Impl(l, r); }
void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state) { before(); ref_adpcm(flags, state); port_aADPCMdecImpl(flags, state); compare(C_ADPCM); }
void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state) { before(); ref_resample(flags, pitch, state); port_aResampleImpl(flags, pitch, state); compare(C_RESAMPLE); }
void aEnvMixerImpl(uint16_t in, uint16_t n, bool swap, bool negl, bool negr, uint16_t dl, uint16_t dr, uint16_t wl, uint16_t wr) {
    before(); ref_envmix(in, n, negl, negr, dl, dr, wl, wr); port_aEnvMixerImpl(in, n, swap, negl, negr, dl, dr, wl, wr); compare(C_ENVMIX);
}
