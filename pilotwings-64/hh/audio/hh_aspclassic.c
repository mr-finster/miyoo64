/* hh_aspclassic.c: the classic audio microcode (aspMain, libultra abi.h without N_MICRO) in C, for Pilotwings 64's
 * libultra al* synthesizer. Commands keep the buffer state the microcode keeps (SETBUFF's in/out/count and its aux
 * outputs, SETVOL's volumes and ramps, the ADPCM book and loop). DMEM is a 4 KB byte array; samples in it are host
 * s16, and so are the state blocks in N64 RAM (only this file reads them). The audio heap's addresses come as K0 or
 * physical addresses, both reached through the RAM mapped at 0x80000000. Host side. */
#include <stdint.h>
#include <string.h>

typedef int16_t s16_t;
static uint8_t dmem[0x1000 + 64];
static struct {
    uint16_t in, out, count, dry_right, wet_left, wet_right;
    int16_t vol[2], target[2];
    int32_t rate[2];
    int16_t dry, wet;
    int16_t book[16][8 * 2];
    uint32_t loop;
} st;

static int16_t resample_table[64][4] = {
    {0x0c39, 0x66ad, 0x0d46, 0xffdf}, {0x0b39, 0x6696, 0x0e5f, 0xffd8},
    {0x0a44, 0x6669, 0x0f83, 0xffd0}, {0x095a, 0x6626, 0x10b4, 0xffc8},
    {0x087d, 0x65cd, 0x11f0, 0xffbf}, {0x07ab, 0x655e, 0x1338, 0xffb6},
    {0x06e4, 0x64d9, 0x148c, 0xffac}, {0x0628, 0x643f, 0x15eb, 0xffa1},
    {0x0577, 0x638f, 0x1756, 0xff96}, {0x04d1, 0x62cb, 0x18cb, 0xff8a},
    {0x0435, 0x61f3, 0x1a4c, 0xff7e}, {0x03a4, 0x6106, 0x1bd7, 0xff71},
    {0x031c, 0x6007, 0x1d6c, 0xff64}, {0x029f, 0x5ef5, 0x1f0b, 0xff56},
    {0x022a, 0x5dd0, 0x20b3, 0xff48}, {0x01be, 0x5c9a, 0x2264, 0xff3a},
    {0x015b, 0x5b53, 0x241e, 0xff2c}, {0x0101, 0x59fc, 0x25e0, 0xff1e},
    {0x00ae, 0x5896, 0x27a9, 0xff10}, {0x0063, 0x5720, 0x297a, 0xff02},
    {0x001f, 0x559d, 0x2b50, 0xfef4}, {0xffe2, 0x540d, 0x2d2c, 0xfee8},
    {0xffac, 0x5270, 0x2f0d, 0xfedb}, {0xff7c, 0x50c7, 0x30f3, 0xfed0},
    {0xff53, 0x4f14, 0x32dc, 0xfec6}, {0xff2e, 0x4d57, 0x34c8, 0xfebd},
    {0xff0f, 0x4b91, 0x36b6, 0xfeb6}, {0xfef5, 0x49c2, 0x38a5, 0xfeb0},
    {0xfedf, 0x47ed, 0x3a95, 0xfeac}, {0xfece, 0x4611, 0x3c85, 0xfeab},
    {0xfec0, 0x4430, 0x3e74, 0xfeac}, {0xfeb6, 0x424a, 0x4060, 0xfeaf},
    {0xfeaf, 0x4060, 0x424a, 0xfeb6}, {0xfeac, 0x3e74, 0x4430, 0xfec0},
    {0xfeab, 0x3c85, 0x4611, 0xfece}, {0xfeac, 0x3a95, 0x47ed, 0xfedf},
    {0xfeb0, 0x38a5, 0x49c2, 0xfef5}, {0xfeb6, 0x36b6, 0x4b91, 0xff0f},
    {0xfebd, 0x34c8, 0x4d57, 0xff2e}, {0xfec6, 0x32dc, 0x4f14, 0xff53},
    {0xfed0, 0x30f3, 0x50c7, 0xff7c}, {0xfedb, 0x2f0d, 0x5270, 0xffac},
    {0xfee8, 0x2d2c, 0x540d, 0xffe2}, {0xfef4, 0x2b50, 0x559d, 0x001f},
    {0xff02, 0x297a, 0x5720, 0x0063}, {0xff10, 0x27a9, 0x5896, 0x00ae},
    {0xff1e, 0x25e0, 0x59fc, 0x0101}, {0xff2c, 0x241e, 0x5b53, 0x015b},
    {0xff3a, 0x2264, 0x5c9a, 0x01be}, {0xff48, 0x20b3, 0x5dd0, 0x022a},
    {0xff56, 0x1f0b, 0x5ef5, 0x029f}, {0xff64, 0x1d6c, 0x6007, 0x031c},
    {0xff71, 0x1bd7, 0x6106, 0x03a4}, {0xff7e, 0x1a4c, 0x61f3, 0x0435},
    {0xff8a, 0x18cb, 0x62cb, 0x04d1}, {0xff96, 0x1756, 0x638f, 0x0577},
    {0xffa1, 0x15eb, 0x643f, 0x0628}, {0xffac, 0x148c, 0x64d9, 0x06e4},
    {0xffb6, 0x1338, 0x655e, 0x07ab}, {0xffbf, 0x11f0, 0x65cd, 0x087d},
    {0xffc8, 0x10b4, 0x6626, 0x095a}, {0xffd0, 0x0f83, 0x6669, 0x0a44},
    {0xffd8, 0x0e5f, 0x6696, 0x0b39}, {0xffdf, 0x0d46, 0x66ad, 0x0c39}
};

static inline void *ram(uint32_t a)
{
    if (a < 0x20000000u) a |= 0x80000000u;
    return (void *)(uintptr_t)a;
}
static inline int16_t clamp16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }
static inline int16_t *S(uint32_t a) { return (int16_t *)(dmem + (a & 0xFFE)); }

/* ADPCMDEC: the output starts with the 16 samples of the frame before (the state, or the loop's state), then the
 * decoded frames: libultra's alAdpcmPull steps over that frame (*outp += lastsam or ADPCMFSIZE samples) and resumes
 * mid-frame from it. Before ALPHA11 the frames went at the output address itself, so every pull of every voice read
 * its sound 16 samples early and ended on up to 16 stale DMEM samples: a click per voice per audio frame (60+ a
 * second), heard as a buzzing, "distorted" low end. A header scale over 12 saturates there (the microcode's shift). */
static void adpcm(uint32_t flags, uint32_t state)
{
    int16_t *hist = ram(state), last[16];
    uint32_t in = st.in, out = st.out;
    int n = (st.count + 31) / 32, i, j, k;
    int16_t *o = S(out);
    if (flags & 1) memset(last, 0, sizeof last);
    else if (flags & 2) memcpy(last, ram(st.loop), sizeof last);
    else memcpy(last, hist, sizeof last);
    for (k = 0; k < 16; k++) S(out + 2 * k)[0] = last[k];
    o = S(out + 32);
    int16_t p2 = last[14], p1 = last[15];
    for (i = 0; i < n; i++) {
        uint8_t hdr = dmem[in++ & 0xFFF];
        int scale = 1 << ((hdr >> 4) > 12 ? 12 : (hdr >> 4));
        const int16_t *bk = st.book[hdr & 0xF];
        for (j = 0; j < 2; j++) {
            int32_t ins[8], acc;
            for (k = 0; k < 8; k += 2) {
                uint8_t b = dmem[in++ & 0xFFF];
                int a0 = (b >> 4) & 0xF, a1 = b & 0xF;
                if (a0 >= 8) a0 -= 16;
                if (a1 >= 8) a1 -= 16;
                ins[k] = a0 * scale;
                ins[k + 1] = a1 * scale;
            }
            for (k = 0; k < 8; k++) {
                int l;
                acc = bk[k] * p2 + bk[8 + k] * p1 + (ins[k] << 11);
                for (l = 0; l < k; l++) acc += bk[8 + k - 1 - l] * ins[l];
                o[k] = clamp16(acc >> 11);
            }
            p2 = o[6];
            p1 = o[7];
            for (k = 0; k < 8; k++) last[8 * j + k] = o[k];
            out += 16;
            o = S(out + 32);
        }
    }
    memcpy(hist, last, 32);
}

static void resample(uint32_t flags, uint32_t pitch, uint32_t state)
{
    int16_t *sv = ram(state);
    uint32_t ipos = (st.in >> 1) - 4, opos = st.out >> 1, accu = 0;
    int n = ((st.count + 15) & ~15) >> 1, i, k;
    int16_t *d = (int16_t *)dmem;
    pitch <<= 1;
    if (flags & 1) {
        for (k = 0; k < 4; k++) d[(ipos + k) & 0x7FF] = 0;
    } else {
        for (k = 0; k < 4; k++) d[(ipos + k) & 0x7FF] = sv[k];
        accu = (uint16_t)sv[5];
    }
    for (i = 0; i < n; i++) {
        const int16_t *lut = resample_table[(accu >> 10) & 0x3F];
        int32_t v = 0;
        for (k = 0; k < 4; k++) v += d[(ipos + k) & 0x7FF] * lut[k];
        d[(opos++) & 0x7FF] = clamp16(v >> 15);
        accu += pitch;
        ipos += accu >> 16;
        accu &= 0xFFFF;
    }
    for (k = 0; k < 4; k++) sv[k] = d[(ipos + k) & 0x7FF];
    sv[5] = (int16_t)accu;
}

/* the envelope mixer (the classic microcode's exponential ramp): every 8 samples the ramp's next point is the last one
 * times the 16.16 rate, and the volume moves to it in 8 equal steps (stopping at the target); the dry and wet gains
 * are the volume times the send, rounded. Until ALPHA11 the volume jumped once per 8 samples (a 2.7 kHz zipper on
 * every attack and release). State (in the voice's ENVMIX_STATE, 80 bytes): value, target, rate, point per side, then
 * dry and wet. */
static void envmixer(uint32_t flags, uint32_t state)
{
    int32_t *sv = ram(state);
    int32_t val[2], tgt[2], rate[2], seq[2], step[2], dry, wet;
    int n = st.count >> 1, i, c;
    int16_t *in = S(st.in), *dl = S(st.out), *dr = S(st.dry_right), *wl = S(st.wet_left), *wr = S(st.wet_right);
    if (flags & 1) {
        for (c = 0; c < 2; c++) {
            val[c] = st.vol[c] * 65536;
            tgt[c] = st.target[c] * 65536;
            rate[c] = st.rate[c];
            seq[c] = (int32_t)((int64_t)st.vol[c] * rate[c]);
        }
        dry = st.dry;
        wet = st.wet;
    } else {
        for (c = 0; c < 2; c++) { val[c] = sv[c]; tgt[c] = sv[2 + c]; rate[c] = sv[4 + c]; seq[c] = sv[6 + c]; }
        dry = (int16_t)sv[8];
        wet = (int16_t)sv[9];
    }
    for (c = 0; c < 2; c++) step[c] = tgt[c] - val[c];
    for (i = 0; i < n; i++) {
        int32_t s = in[i], g[2];
        if ((i & 7) == 0) {
            for (c = 0; c < 2; c++) {
                if (step[c] != 0) {
                    seq[c] = (int32_t)(((int64_t)seq[c] * rate[c]) >> 16);
                    step[c] = (int32_t)(((int64_t)seq[c] - val[c]) >> 3);
                }
            }
        }
        for (c = 0; c < 2; c++) {
            val[c] += step[c];
            if (step[c] <= 0 ? val[c] <= tgt[c] : val[c] >= tgt[c]) { val[c] = tgt[c]; step[c] = 0; }
            g[c] = (int16_t)(val[c] >> 16);
        }
        {
            const int32_t gl = clamp16((g[0] * dry + 0x4000) >> 15), gr = clamp16((g[1] * dry + 0x4000) >> 15);
            dl[i] = clamp16(dl[i] + ((s * gl) >> 15));
            dr[i] = clamp16(dr[i] + ((s * gr) >> 15));
        }
        if (flags & 8) {
            const int32_t gl = clamp16((g[0] * wet + 0x4000) >> 15), gr = clamp16((g[1] * wet + 0x4000) >> 15);
            wl[i] = clamp16(wl[i] + ((s * gl) >> 15));
            wr[i] = clamp16(wr[i] + ((s * gr) >> 15));
        }
    }
    for (c = 0; c < 2; c++) { sv[c] = val[c]; sv[2 + c] = tgt[c]; sv[4 + c] = rate[c]; sv[6 + c] = seq[c]; }
    sv[8] = dry;
    sv[9] = wet;
}

/* the pole filter (libultra reverb's low-pass sections): a 2-pole IIR over 8-sample frames with the table loaded by
 * LOADADPCM (h1 = first 8 coefficients, h2 = next 8); the last samples are kept at the state address */
static void polef(uint32_t flags, int16_t gain, uint32_t state)
{
    const int16_t *h1 = st.book[0];
    int16_t h2[8], h2g[8], *sv = ram(state);
    int16_t *src = S(st.in), *dst = S(st.out);
    int n = ((st.count + 15) & ~15) / 16, i, k;
    int16_t l1 = 0, l2 = 0;
    for (i = 0; i < 8; i++) { h2[i] = st.book[0][8 + i]; h2g[i] = (int16_t)(((int32_t)h2[i] * gain) >> 14); }
    if (!(flags & 1)) { l1 = sv[2]; l2 = sv[3]; }
    while (n-- > 0) {
        int16_t frame[8];
        memcpy(frame, src, sizeof frame);
        for (i = 0; i < 8; i++) {
            int32_t acc = frame[i] * gain + h1[i] * l1 + h2[i] * l2;
            for (k = 0; k < i; k++) acc += h2g[k] * frame[i - 1 - k];
            dst[i] = clamp16(acc >> 14);
        }
        l1 = dst[6];
        l2 = dst[7];
        src += 8;
        dst += 8;
    }
    memcpy(sv, dst - 4, 8);
}

void hh_aspclassic_run(const uint32_t *cmd, int count)
{
    int k, i;
    for (k = 0; k < count; k++, cmd += 2) {
        const uint32_t w0 = cmd[0], w1 = cmd[1];
        const uint32_t flags = (w0 >> 16) & 0xFF;
        switch (w0 >> 24) {
        case 1: adpcm(flags, w1); break;
        case 2: memset(dmem + (w0 & 0xFFF), 0, ((w1 & 0xFFFF) + 15) & ~15); break;
        case 3: envmixer(flags, w1); break;
        case 4: memcpy(dmem + (st.in & 0xFFF), ram(w1), (st.count + 7) & ~7); break;
        case 5: resample(flags, w0 & 0xFFFF, w1); break;
        case 6: memcpy(ram(w1), dmem + (st.out & 0xFFF), (st.count + 7) & ~7); break;
        case 8:
            if (flags & 8) { st.dry_right = w0 & 0xFFFF; st.wet_left = w1 >> 16; st.wet_right = w1 & 0xFFFF; }
            else { st.in = w0 & 0xFFFF; st.out = w1 >> 16; st.count = w1 & 0xFFFF; }
            break;
        case 9: {
            int c = (flags & 2) ? 0 : 1; /* A_LEFT */
            if (flags & 8) { st.dry = (int16_t)w0; st.wet = (int16_t)w1; }
            else if (flags & 4) st.vol[c] = (int16_t)w0;
            else { st.target[c] = (int16_t)w0; st.rate[c] = (int32_t)w1; }
            break;
        }
        case 10: memmove(dmem + ((w1 >> 16) & 0xFFF), dmem + (w0 & 0xFFF), (((w1 & 0xFFFF) + 15) & ~15)); break;
        case 11: memcpy(st.book, ram(w1), (w0 & 0xFFFF) > sizeof st.book ? sizeof st.book : (w0 & 0xFFFF)); break;
        case 12: {
            int16_t g = (int16_t)w0, *in = S(w1 >> 16), *out = S(w1 & 0xFFFF);
            for (i = 0; i < st.count / 2; i++) out[i] = clamp16(out[i] + ((in[i] * g) >> 15));
            break;
        }
        case 13: {
            int16_t *l = S(w1 >> 16), *r = S(w1 & 0xFFFF), tmp[0x800];
            int n = st.count / 2;
            for (i = 0; i < n && 2 * i + 1 < 0x800; i++) { tmp[2 * i] = l[i]; tmp[2 * i + 1] = r[i]; }
            memcpy(S(st.out), tmp, (size_t)n * 4);
            break;
        }
        case 15: st.loop = w1; break;
        case 14: polef(flags, (int16_t)w0, w1); break;
        default: break; /* SPNOOP, SEGMENT */
        }
    }
}
