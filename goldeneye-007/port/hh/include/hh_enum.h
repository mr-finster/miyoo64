/* hh_enum.h: IDO's enum semantics under GCC (force-included by Makefile.handheld).
 *
 * IDO gives every enum type int. GCC gives an enum with no negative enumerators
 * `unsigned int`, which silently flips `head >= 0` (HEAD_FIXED = 0xFFFFFFFF),
 * `(chrflags << 7) >= 0` (CHRFLAG words), e < 0, e / n and (float)e. hh/tools/enum_int.py
 * puts HH_ENUM_INT(<unique>) right after every enum's `{` (a -1 enumerator: the type
 * becomes int and the next enumerator without an initializer is still 0) and wraps
 * enumerator literals above INT_MAX in HH_I32 (their int value, as on the N64). */
#ifndef HH_ENUM_H
#define HH_ENUM_H
#define HH_ENUM_INT(n) HH_ENUM_INT_##n = -1,
#define HH_I32(x) ((int)(x))
#endif
