#ifndef LIBC_STDLIB_H
#define LIBC_STDLIB_H

#if defined(TARGET_HANDHELD)
/* handheld/desktop port: the host C library (layouts that matter to the game
 * -- bool, the typedefs -- are unchanged). */
#include <stdlib.h>
#include <stddef.h>
#include <sys/types.h> /* ssize_t (musl only has it here; the N64 header typedef'd it) */
#include "PR/ultratypes.h"
#else

#include "libc/stddef.h"

typedef struct {
    /* 0x0 */ int quot;
    /* 0x4 */ int rem;
} div_t;

typedef struct {
    /* 0x0 */ long quot;
    /* 0x4 */ long rem;
} ldiv_t;

typedef struct {
    /* 0x0 */ long long quot;
    /* 0x8 */ long long rem;
} lldiv_t;

typedef int ssize_t;

typedef long wchar_t;

ldiv_t ldiv(long numer, long denom);
lldiv_t lldiv(long long numer, long long denom);

#endif /* TARGET_HANDHELD */
#endif /* STDLIB_H */
