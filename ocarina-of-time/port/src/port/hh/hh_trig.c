/* The N64's float sine and cosine: libultra's sinf/cosf (src/libultra/gu/sinf.c, cosf.c),
 * the same algorithm, operation order and double-precision constants, so the game's float
 * trig gives the console's results, and the same results on x86 and ARM (the host libms
 * differ in the last bit, which the round-2 sweeps showed in particles).
 *
 * Those two files can't be built as they are: gu.h's du union is {hi, lo} words, which only
 * lays out as a double on a big-endian CPU (on x86/ARM every constant became a denormal).
 * The constants here are the same bits written as hex floats.
 *
 * -fno-builtin-sinf -fno-builtin-cosf (Makefile.handheld) keep GCC from folding these calls
 * with the host's maths or merging a sinf/cosf pair into the host libm's sincosf.
 * OOT_N64TRIG=0 (debug.cfg n64trig=0): the host libm's again, the A/B. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>

static float (*sHostSinf)(float), (*sHostCosf)(float);
static volatile int sN64Trig = -1;
static int n64trig(void) {
    if (sN64Trig < 0) { /* the first call; racing threads resolve the same answer */
        const char *e = getenv("OOT_N64TRIG");
        int on = !(e && e[0] == '0');
        if (!on) {
            sHostSinf = (float (*)(float))dlsym(RTLD_NEXT, "sinf");
            sHostCosf = (float (*)(float))dlsym(RTLD_NEXT, "cosf");
            if (!sHostSinf || !sHostCosf) on = 1;
        }
        sN64Trig = on;
    }
    return sN64Trig;
}

#define SQ(x) ((x) * (x))
#define ROUND(x) (int32_t)(((x) >= 0.0) ? ((x) + 0.5) : ((x) - 0.5))

static const double P[] = {
    0x1.0000000000000p+0,  -0x1.55554bc83656dp-3, 0x1.110ed3804c2a0p-7,
    -0x1.9f6ffeea56814p-13, 0x1.5dbdf0e314bfep-19,
};
static const double rpi = 0x1.45f306dc9c883p-2;  /* 1/pi */
static const double pihi = 0x1.921fb50000000p+1; /* pi, split in two */
static const double pilo = 0x1.110b4611a6263p-25;

/* libm_vals.s: the NaN returned for a NaN angle (0x7F810000). */
static float qnan(void) {
    union { uint32_t u; float f; } v = { 0x7F810000u };
    return v.f;
}

static int32_t float_bits(float f) {
    union { float f; int32_t i; } v = { f };
    return v.i;
}

float sinf(float angle) {
    double dx, xSq, polyApprox, dn, result;
    int32_t n;
    int32_t xpt = (float_bits(angle) >> 22) & 0x1FF;

    if (!n64trig()) {
        return sHostSinf(angle);
    }

    if (xpt < 0xFF) {
        dx = angle;
        if (xpt >= 0xE6) {
            xSq = SQ(dx);
            polyApprox = ((P[4] * xSq + P[3]) * xSq + P[2]) * xSq + P[1];
            result = dx + (dx * xSq) * polyApprox;
            return (float)result;
        }
        return angle;
    }

    if (xpt < 0x136) {
        dx = angle;
        dn = dx * rpi;
        n = ROUND(dn);
        dn = n;

        dx -= dn * pihi;
        dx -= dn * pilo;
        xSq = SQ(dx);

        polyApprox = ((P[4] * xSq + P[3]) * xSq + P[2]) * xSq + P[1];
        result = dx + (dx * xSq) * polyApprox;

        if (!(n & 1)) {
            return (float)result;
        }
        return -(float)result;
    }

    if (angle != angle) {
        return qnan();
    }
    return 0.0f;
}

float cosf(float angle) {
    float absx;
    double dx, xSq, polyApprox, dn, result;
    int32_t n;
    int32_t xpt = (float_bits(angle) >> 22) & 0x1FF;

    if (!n64trig()) {
        return sHostCosf(angle);
    }

    if (xpt < 0x136) {
        absx = (angle > 0) ? angle : -angle;
        dx = absx;

        dn = dx * rpi + 0.5;
        n = ROUND(dn);
        dn = n;

        dn -= 0.5;

        dx -= dn * pihi;
        dx -= dn * pilo;

        xSq = SQ(dx);

        polyApprox = ((P[4] * xSq + P[3]) * xSq + P[2]) * xSq + P[1];

        result = dx + (dx * xSq) * polyApprox;

        if (!(n & 1)) {
            return (float)result;
        }
        return -(float)result;
    }
    if (angle != angle) {
        return qnan();
    }
    return 0.0f;
}

/* acos for the renderer's linear texgen (gfx_pc.c), which called the host acosf: glibc's
 * and musl's differ in the last bit (an exhaustive-stride check: glibc 80% of [-1,1], and the
 * SDK's musl, built with fused multiply-adds, 7.6% against this strict-float version). The
 * fdlibm algorithm in float-only operations, the same bits on x86 and ARM; acos(0) and
 * acos(-1) round to the nearest float of pi/2 and pi as both libms do (texgen's most common
 * value, cx = 0, lands exactly on scale/4). From FreeBSD's e_acosf.c:
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 * Conversion to float by Ian Lance Taylor, Cygnus Support, ian@cygnus.com. */
static const float pio2_hi = 1.5707962513e+00f, /* 0x3fc90fda */
    pio2_lo = 7.5497894159e-08f,                /* 0x33a22168 */
    pS0 = 1.6666586697e-01f, pS1 = -4.2743422091e-02f, pS2 = -8.6563630030e-03f,
    qS1 = -7.0662963390e-01f;

static float acos_r(float z) {
    const float p = z * (pS0 + z * (pS1 + z * pS2));
    const float q = 1.0f + z * qS1;
    return p / q;
}

float hh_acosf(float x) {
    float z, w, s, c, df;
    const uint32_t hx = (uint32_t)float_bits(x);
    const uint32_t ix = hx & 0x7fffffff;

    if (ix >= 0x3f800000) { /* |x| >= 1 or NaN */
        if (ix == 0x3f800000) {
            return (hx >> 31) ? 2 * pio2_hi + 2 * pio2_lo : 0;
        }
        return 0 / (x - x);
    }
    if (ix < 0x3f000000) { /* |x| < 0.5 */
        if (ix <= 0x32800000) { /* |x| < 2**-26 */
            return pio2_hi + pio2_lo;
        }
        return pio2_hi - (x - (pio2_lo - x * acos_r(x * x)));
    }
    if (hx >> 31) { /* x < -0.5 */
        z = (1 + x) * 0.5f;
        s = __builtin_sqrtf(z);
        w = acos_r(z) * s - pio2_lo;
        return 2 * (pio2_hi - (s + w));
    }
    /* x > 0.5 */
    z = (1 - x) * 0.5f;
    s = __builtin_sqrtf(z);
    {
        union { float f; uint32_t u; } v = { s };
        v.u &= 0xfffff000;
        df = v.f;
    }
    c = (z - df * df) / (s + df);
    w = acos_r(z) * s + c;
    return 2 * (df + w);
}
