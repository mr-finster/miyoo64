/* The native port's stdarg (M2): the decomp's include/stdlib/stdarg.h walks the stack as MIPS o32 does, which is
 * wrong on ARM (arguments in registers) and not what clang accepts; the compiler's builtins are right everywhere. */
#ifndef _STDARG_H
#define _STDARG_H
#define _ANSI_STDARG_H_
#define __GNUC_VA_LIST
typedef __builtin_va_list __gnuc_va_list;
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap) __builtin_va_end(ap)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_copy(dst, src) __builtin_va_copy(dst, src)
#endif
