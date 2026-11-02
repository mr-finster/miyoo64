/* aspref.c (test only, not shipped): lockstep check of the audio microcode in C (port/hh/audio/acmd_mixer.c) against
 * GoldenEye's aspMain as mupen64plus-rsp-hle's alist_audio.c / alist.c implement it (alist_process_audio_ge: ADPCM
 * alist_adpcm, RESAMPLE alist_resample, ENVMIXER_GE alist_envmix_ge -- the linear ramp), transcribed; after the
 * Pilotwings 64 port's tools/aspref.c. Diddy Kong Racing's acmd_mixer.c is the same code (its microcode,
 * alist_process_audio_bc, has the same three commands). Each command of the game's list runs the reference on a copy
 * of the mixer's DMEM, then the port's on the real one, and the DMEM after the two is compared. The reference keeps
 * its own ADPCM / resampler / envelope states, keyed by address. Build (the desktop proxy runs the C loops the NEON
 * ones are checked against): the game checks its asset recipes against the link layout, so this goes into the build
 * itself, for the test only: at the top of acmd_mixer.c `#define port_audio_run_cmds port_audio_run_cmds_port`, at
 * its end `#include "../../../tools/aspref.c"`, then make -f Makefile.handheld PLAT=pc in port/.
 * ASPREF_OUT=<file> gets the per-command counts every 600 audio tasks and at exit.
 * Results (GE_EXIT_AT=6000, 2,959 audio tasks: logos, intro, menus): ADPCM 99,555 commands, none differ; ENVMIXER
 * 1-2 LSB (the mix rounds dry * 0x7fff + in * gain once, rsp-hle adds (in * gain) >> 15); RESAMPLE 1-2 LSB. */
#undef port_audio_run_cmds
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint8_t rd[AUDIO_DMEM_SIZE];
static struct { uint16_t in, out, count, dry_right, wet_left, wet_right; int16_t vol[2], target[2]; int32_t rate[2];
    int16_t dry, wet; int16_t table[16 * 8]; const int16_t *loop; } Rg;
static int16_t *rs(unsigned a) { return (int16_t *)(rd + (a & 0xffe)); }
static uint8_t *ru8(unsigned a) { return rd + (a & 0xfff); }
static int16_t clamp_s16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
static unsigned align(unsigned x, unsigned a) { return (x + a - 1) & ~(a - 1); }

#define NS 4096
static struct { const void *a; uint8_t d[80]; int used; } *sts; /* on the heap: the link layout stays the game's */
static uint8_t *state(const void *a, int len) {
    unsigned h = (unsigned)(((uintptr_t)a * 2654435761u) % NS);
    if (!sts) sts = calloc(NS, sizeof *sts);
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
        int16_t frame[16]; uint8_t code = *ru8(dmemi++); unsigned scale = code >> 4;
        const int16_t *cb = Rg.table + ((code & 0xf) << 4);
        unsigned rshift = scale < 12 ? 12 - scale : 0;
        if (scale > 12) nbig_scale++;
        for (i = 0; i < 8; i++) { uint8_t b = *ru8(dmemi++);
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
static void ref_resample(uint8_t flags, uint16_t pitch, const void *addr) {
    unsigned ipos = Rg.in >> 1, opos = Rg.out >> 1, count = align(Rg.count, 16) >> 1; uint32_t acc = 0, p = (uint32_t)pitch << 1; int k;
    int16_t *sv = (int16_t *)state(addr, 32);
    ipos -= 4;
    if (flags & A_INIT) { for (k = 0; k < 4; k++) *rs((ipos + k) << 1) = 0; }
    else { for (k = 0; k < 4; k++) *rs((ipos + k) << 1) = sv[k]; acc = (uint16_t)sv[4]; }
    while (count--) {
        const int16_t *lut = sResampleTable[(acc >> 10) & 63];
        *rs((opos++) << 1) = clamp_s16((*rs(ipos << 1) * lut[0] + *rs((ipos + 1) << 1) * lut[1] + *rs((ipos + 2) << 1) * lut[2] + *rs((ipos + 3) << 1) * lut[3]) >> 15);
        acc += p; ipos += acc >> 16; acc &= 0xffff;
    }
    for (k = 0; k < 4; k++) sv[k] = *rs((ipos + k) << 1);
    sv[4] = acc;
}
/* alist_envmix_ge: the volume steps by rate / 8 each sample to its target */
static void ref_envmix(uint8_t flags, const void *addr) {
    int n = (flags & A_AUX) ? 4 : 2;
    int16_t *in = rs(Rg.in), *dl = rs(Rg.out), *dr = rs(Rg.dry_right), *wl = rs(Rg.wet_left), *wr = rs(Rg.wet_right);
    int32_t val[2], tgt[2], step[2]; int16_t dry = Rg.dry, wet = Rg.wet; int32_t *sv = (int32_t *)state(addr, 80);
    unsigned k, count = Rg.count >> 1; int c;
    if (flags & A_INIT) for (c = 0; c < 2; c++) { val[c] = Rg.vol[c] << 16; tgt[c] = Rg.target[c] << 16; step[c] = Rg.rate[c] / 8; }
    else { for (c = 0; c < 2; c++) { val[c] = sv[c]; tgt[c] = sv[2+c]; step[c] = sv[4+c]; } dry = sv[6]; wet = sv[7]; }
    for (k = 0; k < count; k++) {
        int16_t v[2], g[4]; int16_t *b[4] = { dl + k, dr + k, wl + k, wr + k }; int i;
        for (c = 0; c < 2; c++) { val[c] += step[c];
            if (step[c] <= 0 ? val[c] <= tgt[c] : val[c] >= tgt[c]) { val[c] = tgt[c]; step[c] = 0; }
            v[c] = (int16_t)(val[c] >> 16); }
        g[0] = clamp_s16((v[0] * dry + 0x4000) >> 15); g[1] = clamp_s16((v[1] * dry + 0x4000) >> 15);
        g[2] = clamp_s16((v[0] * wet + 0x4000) >> 15); g[3] = clamp_s16((v[1] * wet + 0x4000) >> 15);
        for (i = 0; i < n; i++) *b[i] = clamp_s16(*b[i] + ((in[k] * g[i]) >> 15));
    }
    for (c = 0; c < 2; c++) { sv[c] = val[c]; sv[2+c] = tgt[c]; sv[4+c] = step[c]; } sv[6] = dry; sv[7] = wet;
}

enum { C_ADPCM, C_ENVMIX, C_RESAMPLE, C_N };
static const char *names[C_N] = { "ADPCM", "ENVMIXER", "RESAMPLE" };
static unsigned long ncmd[C_N], nbad[C_N], nsamp[C_N], hist[C_N][4]; static int maxd[C_N]; static unsigned long tasks;
static void report(void) {
    int i; FILE *o = fopen(getenv("ASPREF_OUT") ? getenv("ASPREF_OUT") : "/dev/stderr", "w"); if (!o) return;
    fprintf(o, "[aspref] %lu tasks, %lu ADPCM frames with a scale over 12\n", tasks, nbig_scale);
    for (i = 0; i < C_N; i++) if (ncmd[i]) fprintf(o, "[aspref] %-10s %8lu cmds %8lu differ (%lu samples, max %d; diff 1: %lu, 2-3: %lu, 4-63: %lu, 64+: %lu)\n",
        names[i], ncmd[i], nbad[i], nsamp[i], maxd[i], hist[i][0], hist[i][1], hist[i][2], hist[i][3]);
    fclose(o);
}
static void compare(int c) {
    unsigned a; int bad = 0, md = 0;
    for (a = 0; a + 1 < AUDIO_DMEM_SIZE; a += 2) {
        int16_t p = *DMEM_S16(a), r = *(int16_t *)(rd + a);
        if (p != r) { int d = abs(p - r); bad = 1; nsamp[c]++; if (d > md) md = d;
            hist[c][d == 1 ? 0 : d < 4 ? 1 : d < 64 ? 2 : 3]++; }
    }
    ncmd[c]++; if (bad) { nbad[c]++; if (md > maxd[c]) maxd[c] = md; }
}

void port_audio_run_cmds(const void *cmdList, unsigned int sizeBytes) {
    static int init; const u32 *w = (const u32 *)cmdList; unsigned k, n = sizeBytes / 8;
    if (!init) { init = 1; atexit(report); }
    if (++tasks % 600 == 0) report();
    for (k = 0; k < n; k++, w += 2) {
        const u32 w0 = w[0], w1 = w[1]; int c = -1;
        memcpy(rd, DMEM_U8(0), AUDIO_DMEM_SIZE);
        switch (w0 >> 24) {
        case A_ADPCM: c = C_ADPCM; ref_adpcm((w0 >> 16) & 0xff, dram(w1)); break;
        case A_RESAMPLE: c = C_RESAMPLE; ref_resample((w0 >> 16) & 0xff, w0 & 0xffff, dram(w1)); break;
        case A_ENVMIXER: c = C_ENVMIX; ref_envmix((w0 >> 16) & 0xff, dram(w1)); break;
        case A_SETBUFF: if ((w0 >> 16) & A_AUX) { Rg.dry_right = w0; Rg.wet_left = w1 >> 16; Rg.wet_right = w1; } else { Rg.in = w0; Rg.out = w1 >> 16; Rg.count = w1; } break;
        case A_SETVOL: { uint8_t f = w0 >> 16;
            if (f & A_AUX) { Rg.dry = w0; Rg.wet = w1; }
            else { int lr = (f & A_LEFT) ? 0 : 1; if (f & A_VOL) Rg.vol[lr] = w0; else { Rg.target[lr] = w0; Rg.rate[lr] = w1; } }
            break; }
        case A_LOADADPCM: { unsigned b = w0 & 0xffff; memcpy(Rg.table, dram(w1), b > sizeof Rg.table ? sizeof Rg.table : b); break; }
        case A_SETLOOP: Rg.loop = (const int16_t *)dram(w1); break;
        default: break;
        }
        port_audio_run_cmds_port(w, 8);
        if (c >= 0) compare(c);
    }
}
