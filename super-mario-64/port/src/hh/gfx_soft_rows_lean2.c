/* SM64_LEAN's NEON row variants (gfx_soft_span_neon.h span_grp PV 8, 16, 32), part 2 of 3 (1: the blended kinds and
 * the picker, 2: the opaque kinds, 3: the blended kinds that write depth), compiled apart from gfx_soft.c: in one file
 * they needed more memory than the build machine had. The device build only. */
#if defined(__ARM_NEON)
#include <stdint.h>
#include <string.h>
#include "../port.h"
#define SPAN_ROWS_LEAN_TU 2
static inline int clamp16_sh8(int32_t q) { int r; __asm__("usat %0, #16, %1, asr #8" : "=r"(r) : "r"(q)); return r; }
#include "gfx_soft_span_neon.h"
#endif
