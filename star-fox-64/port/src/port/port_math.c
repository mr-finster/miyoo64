/* Math the renderer needs from outside the game, built into the port so the -m32 desktop proxy
 * computes exactly what the device does. */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "port.h"

/* port_acosf: musl's acosf (FreeBSD's e_acosf.c) compiled with the port's -ffp-contract=off, for
 * the G_TEXTURE_GEN_LINEAR mapping in gfx_pc.c. A musl libc built with fused
 * multiply-adds (8 vfma/vfms in its acosf), so libc's acosf can't be matched on x86: it differs
 * from glibc's in the last 1-2 bits on a third of inputs. This one gives the same bits on both
 * builds (all 262,145 inputs of a sweep) and is at least as close to glibc (8.7% of
 * inputs 1 ulp apart). The N64's microcode has its own approximation; the game's own atan/acos
 * are the N64's (src/libc_math64.c). */
static const float pio2_hi = 1.5707962513e+00f, /* 0x3fc90fda */
                   pio2_lo = 7.5497894159e-08f, /* 0x33a22168 */
                   pS0 = 1.6666586697e-01f,
                   pS1 = -4.2743422091e-02f,
                   pS2 = -8.6563630030e-03f,
                   qS1 = -7.0662963390e-01f;

static float acos_r(float z) {
    float p = z * (pS0 + z * (pS1 + z * pS2));
    float q = 1.0f + z * qS1;
    return p / q;
}

float port_acosf(float x) {
    float z, w, s, c, df;
    uint32_t hx, ix;

    memcpy(&hx, &x, 4);
    ix = hx & 0x7fffffff;
    if (ix >= 0x3f800000) { /* |x| >= 1 or nan */
        if (ix == 0x3f800000) {
            if (hx >> 31) {
                return 2 * pio2_hi + 0x1p-120f;
            }
            return 0;
        }
        return 0 / (x - x);
    }
    if (ix < 0x3f000000) { /* |x| < 0.5 */
        if (ix <= 0x32800000) { /* |x| < 2**-26 */
            return pio2_hi + 0x1p-120f;
        }
        return pio2_hi - (x - (pio2_lo - x * acos_r(x * x)));
    }
    if (hx >> 31) { /* x < -0.5 */
        z = (1 + x) * 0.5f;
        s = sqrtf(z);
        w = acos_r(z) * s - pio2_lo;
        return 2 * (pio2_hi - (s + w));
    }
    z = (1 - x) * 0.5f; /* x > 0.5 */
    s = sqrtf(z);
    memcpy(&hx, &s, 4);
    hx &= 0xfffff000;
    memcpy(&df, &hx, 4);
    c = (z - df * df) / (s + df);
    w = acos_r(z) * s + c;
    return 2 * (df + w);
}
