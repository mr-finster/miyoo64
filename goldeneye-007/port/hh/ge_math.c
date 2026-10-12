/* ge_math.c: C for the game's hand-written assembly that has no C in the decomp.
 *
 * Game and libultra code call these under their N64 names (sinf, cosf, randomGetNext...):
 * hh/include/math.h renames sinf/cosf to ge_sinf/ge_cosf in every game file, as the N64
 * link resolves every sinf/cosf call to Rare's versions (libultra's gu/sinf.c and cosf.c
 * are only weak and never used; they are not built).
 *
 * Every function is an instruction-for-instruction translation: single-precision
 * operations in the assembly's order, each rounded (the build never contracts to FMA),
 * round.w.s as hh_round_w_s (round-to-nearest-even, the N64 default mode), and 64-bit
 * register arithmetic as u64. */
#include <ultra64.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

static f32 hh_f32(u32 bits)
{
    union { u32 u; f32 f; } v;
    v.u = bits;
    return v.f;
}

/* ------------------------------------------------------------------------------------ */
/* math_sincos.s: Rare's sinf/cosf                                                      */

/* round.w.s in the N64's default mode (nearest, ties to even), without libm. The handheld's
 * musl 1.2.1 (the SDK sysroot; the device's is the same Buildroot's) returns the argument
 * from rintf, roundf and nearbyintf and truncates in lrintf/lroundf: the x + 2^23 - 2^23
 * they rely on is folded away (expf/powf and sin/cos/tan reductions beyond ~7 fail alike).
 * With rintf here, every argument whose n rounds up skipped the reduction: on the device
 * cos(pi/2) came out 0.0033 and the Dam's projection differed from the desktop proxy's in
 * the fifth digit (round 12, the lead's x86 = ARM check). |x| < 2^28 / pi here, so the
 * truncation fits and x - trunc(x) is exact. GE_ROUNDW=0: libm's lrintf, for A/B. */
u32 hh_roundw_fixed; /* reductions where libm's lrintf would have truncated (GE_TILECENSUS) */
static s32 hh_round_w_s(f32 x)
{
    static int libm = -1;
    s32 n = (s32)x;
    f32 d = x - (f32)n;
    if (libm < 0) {
        const char *e = getenv("GE_ROUNDW");
        libm = e != NULL && e[0] == '0';
    }
    if (libm)
        return (s32)lrintf(x);
    if (d > 0.5f || (d == 0.5f && (n & 1)))
        n++;
    else if (d < -0.5f || (d == -0.5f && (n & 1)))
        n--;
    if (n != (s32)x)
        hh_roundw_fixed++;
    return n;
}

f32 ge_sinf(f32 x)
{
    const f32 s4 = hh_f32(0x362edef8), s3 = hh_f32(0xb94fb7ff);
    const f32 s2 = hh_f32(0x3c08876a), s1 = hh_f32(0xbe2aaaa6);
    union { f32 f; s32 i; } u;
    s32 e, n;
    f32 x2, r, nf;

    u.f = x;
    e = (u.i >> 22) & 0x1ff; /* exponent and the top mantissa bit */
    if (e < 0xff) {          /* |x| < 1.5 */
        if (e < 0xe6)
            return x;
        x2 = x * x;
        r = s4 * x2;
        r = s3 + r;
        r = r * x2;
        r = s2 + r;
        r = r * x2;
        r = s1 + r;
        r = r * x2;
        r = r * x;
        return r + x;
    }
    if (e >= 0x136) /* |x| >= 2^28 */
        return 0.0f;
    nf = hh_f32(0x3ea2f983) * x; /* 1/pi */
    n = hh_round_w_s(nf);
    nf = (f32)n;
    r = hh_f32(0x40490fdb) * nf; /* pi */
    x = x - r;
    r = hh_f32(0x330885a3) * nf;
    x = x - r;
    x2 = x * x;
    r = s4 * x2;
    r = s3 + r;
    r = r * x2;
    r = s2 + r;
    r = r * x2;
    r = s1 + r;
    r = r * x2;
    r = r * x;
    r = r + x;
    if (n & 1)
        r = -r;
    return r;
}

f32 ge_cosf(f32 x)
{
    return ge_sinf(x + hh_f32(0x3fc90fda)); /* + pi/2, then falls into sinf */
}

/* ------------------------------------------------------------------------------------ */
/* random.s / chrObjRandom.s: 64-bit shift-register generators                          */

u64 g_randomSeed = 0xAB8D9F7781280783ull;
u64 g_chrObjRandomSeed = 0xAB8D9F7781280783ull;

/* GE_RANDTRACE: log each randomGetNext caller while armed (hh_random_trace = 1). */
int hh_random_trace;
static void hh_random_trace_call(u32 caller)
{
    fprintf(stderr, "[rand] call from %08x\n", caller);
}
static u32 hh_random_step(u64 *seed)
{
    u64 s = *seed;
    u64 a2 = ((s << 63) >> 31) | ((s << 31) >> 32);
    a2 ^= (s << 44) >> 32;
    s = ((a2 >> 20) & 0xfff) ^ a2;
    *seed = s;
    return (u32)s;
}

u32 hh_random_calls; /* randomGetNext calls (debug: the ramrom desync hunt) */
u32 hh_random_last_caller;
u32 randomGetNext(void)
{
    hh_random_calls++;
    hh_random_last_caller = (u32)(uintptr_t)__builtin_return_address(0);
    if (hh_random_trace)
        hh_random_trace_call(hh_random_last_caller);
    return hh_random_step(&g_randomSeed);
}
u32 randomGetNextFrom(u64 *seed) { return hh_random_step(seed); }
u32 chrObjRandomGetNext(void) { return hh_random_step(&g_chrObjRandomSeed); }

/* daddiu on the sign-extended 32-bit argument */
void randomSetSeed(u32 seed) { g_randomSeed = (u64)((s64)(s32)seed + 1); }
void chrObjRandomSetSeed(u32 seed) { g_chrObjRandomSeed = (u64)((s64)(s32)seed + 1); }
