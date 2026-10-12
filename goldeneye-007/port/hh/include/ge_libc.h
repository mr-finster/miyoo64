/* Port: GE's own libc-alikes (src/str.c, src/sprintf.c) get game-private names, so the
 * game keeps IDO-era semantics while SDL/libc keep theirs. Included by the shims above
 * and force-included into game TUs; the port layer (hh/) never includes it. */
#ifndef HH_GE_LIBC_H
#define HH_GE_LIBC_H
#include <stddef.h>
#include <math.h> /* its renames must precede every game TU */
#include "hh_port.h"
#if defined(PORT) && !defined(HH_PORT_LAYER)
#define strcpy   ge_strcpy
#define strncpy  ge_strncpy
#define strcat   ge_strcat
#define strcmp   ge_strcmp
#define strncmp  ge_strncmp
#define toupper  ge_toupper
#define isdigit  ge_isdigit
#define isalpha  ge_isalpha
#define isspace  ge_isspace
#define strtol   ge_strtol
#define sprintf  ge_sprintf
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
long int strtol(const char *str, char **endptr, int base);
int sprintf(char *dst, const char *fmt, ...);
#endif
#endif
