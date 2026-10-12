/* spancvt_test.c: MK64_SPANCVT (2026-10-04, the final speed sweep) -- gfx_soft.c's integer-clamped conversions
 * (to_fix16_i, z_fix_i) against the float-clamped ones raster_tri used before (to_fix16, and the z clamp), run on the
 * ARM build (the Miyoo toolchain's gcc, under qemu-arm: the VFP conversion it relies on saturates there as on the A7).
 * Every float bit pattern is tried when STEP=1; the default steps through all 2^32 by STEP and adds every pattern
 * within 2^16 ulps of each clamp bound and the specials (+-0, +-inf, NaNs, denormals). Each input is also scaled by
 * the texture sizes raster_tri multiplies in (1 .. 256). Exit 1 on any difference. */
#include "gfx_soft.c"

static int32_t old_fix16(float f) { return to_fix16(f); }
static int32_t old_z(float zf) { return (int32_t)((zf < -1.0f ? -1.0f : zf > 2.0f ? 2.0f : zf) * (65535.0f * 256.0f)); }
static uint64_t sN, sBad;
static void one(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    if (to_fix16_i(f) != old_fix16(f) || z_fix_i(f) != old_z(f)) {
        if (sBad++ < 10) printf("differ at %08x (%g): fix16 %d / %d, z %d / %d\n", bits, f, to_fix16_i(f), old_fix16(f), z_fix_i(f), old_z(f));
    }
    sN++;
}
static void around(float c) {
    uint32_t b;
    memcpy(&b, &c, 4);
    for (int64_t d = -65536; d <= 65536; d++) one((uint32_t)((int64_t)b + d));
}
int main(int argc, char **argv) {
    const uint32_t step = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 13;
    for (uint64_t b = 0; b <= 0xFFFFFFFFull; b += step) one((uint32_t)b);
    const float k[] = { 16000.0f, -16000.0f, 16000.0f / 65536.0f, 1048576000.0f, -1.0f, 2.0f, 1.0f / 16776960.0f, 0.0f, 1.0f,
                        2147483648.0f / 65536.0f, -2147483648.0f / 65536.0f, 2147483648.0f / 16776960.0f };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) { around(k[i]); around(-k[i]); }
    const uint32_t sp[] = { 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0x00000001u, 0x80000001u, 0x007FFFFFu };
    for (size_t i = 0; i < sizeof(sp) / sizeof(sp[0]); i++) one(sp[i]);
    /* products with the texture sizes, as raster_tri forms them (u_a * (float)tw) */
    for (int t = 1; t <= 256; t <<= 1)
        for (uint32_t b = 0x30000000u; b < 0x50000000u; b += 4099) { float f; memcpy(&f, &b, 4); float g = f * (float)t; uint32_t gb; memcpy(&gb, &g, 4); one(gb); one(gb ^ 0x80000000u); }
    printf("spancvt_test: %llu inputs, %llu differ: %s\n", (unsigned long long)sN, (unsigned long long)sBad, sBad ? "FAIL" : "PASS");
    return sBad != 0;
}
