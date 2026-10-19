/* Minimal stand-in for mk64 include/macros.h: what the renderer kit uses. */
#ifndef PM64_BENCH_MACROS_H
#define PM64_BENCH_MACROS_H
#define UNUSED __attribute__((unused))
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#endif
