/* The few MK64 macros.h helpers the MK64 renderer files use. */
#ifndef HH_GFX_MACROS_H
#define HH_GFX_MACROS_H
#ifndef UNUSED
#define UNUSED __attribute__((unused))
#endif
#ifndef ALIGN
#define ALIGN(val, alignment) (((val) + ((alignment) - 1)) & ~((alignment) - 1))
#endif
#ifndef CLAMP
#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))
#endif
#endif
