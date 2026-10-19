/* libultra's sinf and cosf (lib/libultra/src/gu/sinf.c, cosf.c), the N64's own, for the native port. Game side.
 *
 * The decomp's copies keep their double constants as pairs of 32-bit words in the N64's (big-endian) order, so they
 * cannot be compiled as they are for a little-endian CPU; until now the host's libm sinf/cosf stood in. Those round
 * differently from the N64's polynomial in the last bit, and the map's tile culling (map.c checkTileVisible, its
 * frustum corners from rotateVector3D) compares against exact thresholds: the port drew a row of map tiles more at
 * the left and bottom edges than the N64, where the N64 leaves black jagged gaps. These are the same computations in
 * double precision as on the N64 (MIPS FPU), the constants as exact hex literals of the same bits, no FMA
 * (-ffp-contract=off): bit-identical results. They replace libm's for the whole executable. */

typedef union { unsigned int i; float f; } hh_fu;

#define ROUND(d) (int) (((d) >= 0.0) ? ((d) + 0.5) : ((d) - 0.5))

static const double P1 = -0x1.55554bc83656dp-3, P2 = 0x1.110ed3804c2a0p-7, P3 = -0x1.9f6ffeea56814p-13,
                    P4 = 0x1.5dbdf0e314bfep-19;
static const double rpi = 0x1.45f306dc9c883p-2, pihi = 0x1.921fb50000000p+1, pilo = 0x1.110b4611a6263p-25;

extern float __libm_qnan_f;

float sinf(float x) {
    double dx, xsq, poly, dn, result;
    int n, ix, xpt;
    hh_fu u;
    u.f = x;
    ix = (int) u.i;
    xpt = (ix >> 22) & 0x1ff; /* exponent(x) + 1 bit of mantissa */
    if (xpt < 0xff) {         /* |x| < 1.5 */
        dx = x;
        if (xpt >= 0xe6) {    /* |x| >= 2^(-12) */
            xsq = dx * dx;
            poly = ((P4 * xsq + P3) * xsq + P2) * xsq + P1;
            result = dx + (dx * xsq) * poly;
            return (float) result;
        }
        return x;
    }
    if (xpt < 0x136) { /* |x| < 2^28: reduce to +/- pi/2 */
        dx = x;
        dn = dx * rpi;
        n = ROUND(dn);
        dn = n;
        dx = dx - dn * pihi;
        dx = dx - dn * pilo;
        xsq = dx * dx;
        poly = ((P4 * xsq + P3) * xsq + P2) * xsq + P1;
        result = dx + (dx * xsq) * poly;
        if ((n & 1) == 0) return (float) result;
        return -(float) result;
    }
    if (x != x) return __libm_qnan_f;
    return 0.0f;
}

float cosf(float x) {
    float absx;
    double dx, xsq, poly, dn, result;
    int n, ix, xpt;
    hh_fu u;
    u.f = x;
    ix = (int) u.i;
    xpt = (ix >> 22) & 0x1ff;
    if (xpt < 0x136) { /* |x| < 2^28: Cody and Waite, in double precision */
        absx = x > 0 ? x : -x;
        dx = absx;
        dn = dx * rpi + 0.5;
        n = ROUND(dn);
        dn = n;
        dn -= 0.5;
        dx = dx - dn * pihi;
        dx = dx - dn * pilo; /* dx = x - (n - 0.5) * pi */
        xsq = dx * dx;
        poly = ((P4 * xsq + P3) * xsq + P2) * xsq + P1;
        result = dx + (dx * xsq) * poly;
        if ((n & 1) == 0) return (float) result;
        return -(float) result;
    }
    if (x != x) return __libm_qnan_f;
    return 0.0f;
}
