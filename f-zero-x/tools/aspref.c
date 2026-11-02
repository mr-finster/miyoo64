/* aspref.c (test only, not shipped): lockstep check of the audio microcode in C (game/src/port/audio/alist.c and
 * mixer.c) against F-Zero X's NEAD microcode as mupen64plus-rsp-hle's alist_nead.c / alist.c implement it
 * (alist_process_nead_fz: ADPCM alist_adpcm, RESAMPLE alist_resample, ENVSETUP1/2 + ENVMIXER alist_envmix_nead),
 * transcribed; after the Pilotwings 64 port's tools/aspref.c. This file includes alist.c and mixer.c, the command
 * runner renamed: each command of the game's list runs the reference on a copy of the mixer's DMEM, then the port's
 * on the real one, and the DMEM after the two is compared. The reference keeps its own ADPCM / resampler states,
 * keyed by address. Build: the desktop proxy (make -f Makefile.handheld PLAT=pc), then this file compiled with its
 * flags plus -DPORT_MIXER_SCALAR (the C loops the NEON ones are checked against) and linked in place of
 * build/pc/src/port/audio/{alist,mixer}.o. ASPREF_OUT=<file> gets the per-command counts every 600 audio tasks.
 * Results (FZX_AUTO=1 FZX_HEADLESS=1, 155,942 audio tasks of the autopilot): ADPCM 115,352 and ENVMIXER 115,203
 * commands, none differ; RESAMPLE 1-2 LSB on every command (the C loop rounds each tap, rsp-hle rounds the sum). */
#define port_audio_run_alist port_audio_run_alist_port
#include "../game/src/port/audio/mixer.c"
#include "../game/src/port/audio/alist.c"
#undef port_audio_run_alist
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
static struct { const void *a; uint8_t d[32]; int used; } sts[NS];
static uint8_t *state(const void *a, int len) {
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
static void ref_envmix(uint32_t w1, uint32_t w2) {
    int16_t xors[4];
    unsigned dmemi = (w1 >> 12) & 0xff0, count = align((w1 >> 8) & 0xff, 8), i;
    int16_t *in = rs(dmemi), *dl = rs((w2 >> 20) & 0xff0), *dr = rs((w2 >> 12) & 0xff0), *wl = rs((w2 >> 4) & 0xff0), *wr = rs((w2 << 4) & 0xff0);
    xors[2] = 0 - (int16_t)((w1 & 0x8) >> 1); xors[3] = 0 - (int16_t)((w1 & 0x4) >> 1);
    xors[0] = 0 - (int16_t)((w1 & 0x2) >> 1); xors[1] = 0 - (int16_t)(w1 & 0x1);
    if ((w1 >> 4) & 1) { int16_t *t = wl; wl = wr; wr = t; }
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
    fprintf(o, "[aspref] %lu tasks, %lu ADPCM frames with a scale over 12 (4-bit) / 14 (2-bit)\n", tasks, nbig_scale);
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

void port_audio_run_alist(const u64 *list, u32 bytes) {
    static int init; const u32 *w = (const u32 *)list; u32 k, n = bytes / 8;
    if (!init) { init = 1; atexit(report); }
    if (++tasks % 600 == 0) report();
    for (k = 0; k < n; k++, w += 2) {
        const u32 w0 = w[0], w1 = w[1]; int c = -1;
        memcpy(rd, rspa.buf.as_u8, 0x1000);
        switch (w0 >> 24) {
        case A_ADPCM: c = C_ADPCM; ref_adpcm((w0 >> 16) & 0xff, PTR(w1)); break;
        case A_RESAMPLE: c = C_RESAMPLE; ref_resample((w0 >> 16) & 0xff, w0 & 0xffff, PTR(w1)); break;
        case A_ENVMIXER: c = C_ENVMIX; ref_envmix(w0, w1); break;
        case A_SETBUFF: Rg.in = w0; Rg.out = w1 >> 16; Rg.count = w1; break;
        case A_LOADADPCM: { unsigned b = w0 & 0xffff; memcpy(Rg.table, PTR(w1), b > sizeof Rg.table ? sizeof Rg.table : b); break; }
        case A_SETLOOP: Rg.loop = (const int16_t *)PTR(w1); break;
        case A_ENVSETUP1: Rg.env_values[2] = (w0 >> 8) & 0xff00; Rg.env_steps[2] = w0; Rg.env_steps[0] = w1 >> 16; Rg.env_steps[1] = w1; break;
        case A_ENVSETUP2: Rg.env_values[0] = w1 >> 16; Rg.env_values[1] = w1; break;
        default: break;
        }
        port_audio_run_alist_port((const u64 *)w, 8);
        if (c >= 0) compare(c);
    }
}
