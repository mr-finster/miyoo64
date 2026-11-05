/* hh_libm.c: libultra's sinf/cosf (gu/sinf.c, gu/cosf.c: Cody and Waite in double precision) for a little-endian
 * host. The originals spell their constants as {high word, low word} unions, which on this host build different
 * doubles (the N64 logo's camera came out infinite); the same constants are given here as doubles, so the game gets
 * the N64's results bit for bit. */
#include <stdint.h>

typedef union { uint64_t u; double d; } hh_du;
#define DU(hi, lo) { ((uint64_t)(hi) << 32) | (uint32_t)(lo) }
static const hh_du P[] = { DU(0x3ff00000, 0x00000000), DU(0xbfc55554, 0xbc83656d), DU(0x3f8110ed, 0x3804c2a0),
                           DU(0xbf29f6ff, 0xeea56814), DU(0x3ec5dbdf, 0x0e314bfe) };
static const hh_du rpi = DU(0x3fd45f30, 0x6dc9c883), pihi = DU(0x400921fb, 0x50000000),
                   pilo = DU(0x3e6110b4, 0x611a6263);
#define ROUND(d) (int)(((d) >= 0.0) ? ((d) + 0.5) : ((d) - 0.5))
extern const float __libm_qnan_f;

static inline int bits(float x) { union { float f; int i; } u; u.f = x; return u.i; }

float sinf(float x)
{
    double dx, xsq, poly, dn, result;
    int n, xpt = (bits(x) >> 22) & 0x1ff;
    if (xpt < 0xff) {
        dx = x;
        if (xpt >= 0xe6) {
            xsq = dx * dx;
            poly = ((P[4].d * xsq + P[3].d) * xsq + P[2].d) * xsq + P[1].d;
            result = dx + (dx * xsq) * poly;
            return (float)result;
        }
        return x;
    }
    if (xpt < 0x136) {
        dx = x;
        dn = dx * rpi.d;
        n = ROUND(dn);
        dn = n;
        dx = dx - dn * pihi.d;
        dx = dx - dn * pilo.d;
        xsq = dx * dx;
        poly = ((P[4].d * xsq + P[3].d) * xsq + P[2].d) * xsq + P[1].d;
        result = dx + (dx * xsq) * poly;
        if ((n & 1) == 0)
            return (float)result;
        return -(float)result;
    }
    if (x != x)
        return __libm_qnan_f;
    return 0.0f;
}

float cosf(float x)
{
    float absx;
    double dx, xsq, poly, dn, result;
    int n, xpt = (bits(x) >> 22) & 0x1ff;
    if (xpt < 0x136) {
        absx = x > 0 ? x : -x;
        dx = absx;
        dn = dx * rpi.d + 0.5;
        n = ROUND(dn);
        dn = n;
        dn -= 0.5;
        dx = dx - dn * pihi.d;
        dx = dx - dn * pilo.d;
        xsq = dx * dx;
        poly = ((P[4].d * xsq + P[3].d) * xsq + P[2].d) * xsq + P[1].d;
        result = dx + (dx * xsq) * poly;
        if ((n & 1) == 0)
            return (float)result;
        return -(float)result;
    }
    if (x != x)
        return __libm_qnan_f;
    return 0.0f;
}
