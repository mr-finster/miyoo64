#ifndef LIBC_STDDEF_H
#define LIBC_STDDEF_H

#if defined(TARGET_HANDHELD)
/* handheld/desktop port: the host C library (layouts that matter to the game
 * -- bool, the typedefs -- are unchanged). */
#include <stddef.h>
#include "PR/ultratypes.h"
#else

#include "PR/ultratypes.h"

typedef s32 ptrdiff_t;

#ifdef __GNUC__
#define offsetof(structure, member) __builtin_offsetof (structure, member)
#else
#define offsetof(structure, member) ((size_t)&(((structure*)0)->member))
#endif

#endif /* TARGET_HANDHELD */
#endif /* STDDEF_H */
