#include "sys.h"

#if !defined(TARGET_HANDHELD)
/* The port builds only the atan/asin/acos chain below: the game calls Math_FAtanF and
 * Math_FAcosF and nothing else from this file. The rest wrap the N64's FPU rounding
 * instructions, which libc/math.h maps to libm, and two of those (roundf, nearbyintf) are
 * broken in some libcs (musl 1.2.1); round.w.s also rounds halves to even,
 * unlike roundf. Left out so they can't be picked up by accident. */
f32 Math_TanF(f32 x) {
    return __sinf(x) / __cosf(x);
}

f32 Math_FloorF(f32 x) {
    return __floorf(x);
}

f32 Math_CeilF(f32 x) {
    return __ceilf(x);
}

f64 Math_Fabs(f64 x) {
    return (x < 0.0) ? -x : x;
}

f32 Math_FabsF(f32 x) {
    return (x < 0.0f) ? -x : x;
}

f32 Math_NearbyIntF(f32 x) {
    return __nearbyintf(x);
}

f32 Math_TruncF(f32 x) {
    return __truncf(x);
}

f32 Math_RoundF(f32 x) {
    return __roundf(x);
}
#endif

/* The N64's own atan (a continued fraction) and the asin/acos built on it: single-precision
 * adds, multiplies and divides only, so with -ffp-contract=off the x86 proxy and the device
 * get the N64's bits (x86 = ARM on all 598,017 inputs of a sweep). The
 * port used host libm here until round 12; glibc and musl disagree in the last bits on about
 * a third of inputs, and both differ from the N64's (acos by up to ~500 ulp near +-1). */
f32 Math_FAtanF(f32 x) {
    s32 sector;
    s32 i;
    f32 sq;
    f32 conv = 0.0f;
    f32 z;

    if (x > 1.0f) {
        sector = 1;
        x = 1.0f / x;
    } else if (x < -1.0f) {
        sector = -1;
        x = 1.0f / x;
    } else {
        sector = 0;
    }

    sq = SQ(x);

    for (z = i = 24; i != 0; i--) {
        conv = SQ(z) * sq / (2.0f * z + 1.0f + conv);
        z -= 1.0f;
    }

    if (sector > 0) {
        return M_PI / 2.0f - (x / (1.0f + conv));
    } else if (sector < 0) {
        return -M_PI / 2.0f - (x / (1.0f + conv));
    } else {
        return x / (1.0f + conv);
    }
}

f32 Math_FAtan2F(f32 y, f32 x) {
    if ((y == 0.0f) && (x == 0.0f)) {
        return 0.0f;
    }
    if (x == 0.0f) {
        if (y < 0.0f) {
            return -M_PI / 2.0f;
        }
        return M_PI / 2.0f;
    }
    if (x < 0.0f) {
        if (y < 0.0f) {
            return -(M_PI - Math_FAtanF(fabs(y / x)));
        }
        return M_PI - Math_FAtanF(fabs(y / x));
    }
    return Math_FAtanF(y / x);
}

f32 Math_FAsinF(f32 x) {
    return Math_FAtan2F(x, sqrtf(1 - SQ(x)));
}

f32 Math_FAcosF(f32 x) {
    return M_PI / 2.0f - Math_FAsinF(x);
}
