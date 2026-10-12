#ifndef __STDDEF_H__
#define __STDDEF_H__

#include <PR/ultratypes.h>

#ifdef _MSC_VER
/* MSVC: offsetof is a compiler intrinsic */
#ifndef offsetof
#define offsetof(type, member) ((size_t)&(((type*)0)->member))
#endif
#elif !defined(__sgi)
/* GCC/Clang: use built-in offsetof macro */
#define offsetof(type, member) __builtin_offsetof(type, member)
#else
/* IDO: use macro from Indy headers */
#define offsetof(s, m) (size_t)(&(((s*)0)->m))
#endif

/* ptrdiff_t. Decomp source never uses it, but this shim shadows the real
 * <stddef.h> on the include path, so any system header pulled in by a port
 * TU that transitively references ptrdiff_t breaks if we don't supply it.
 * NDK r29 / bionic <unistd.h> surfaces this; macOS/glibc happen to declare
 * it elsewhere on the include chain, which masked the issue on desktop.
 * Provided via the compiler's built-in __PTRDIFF_TYPE__ macro (GCC/Clang). */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(_PTRDIFF_T_DEFINED_)
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#define _PTRDIFF_T_DEFINED_
#endif

#endif /* __STDDEF_H__ */
