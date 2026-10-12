#ifndef LIBC_STRING_H
#define LIBC_STRING_H

#if defined(TARGET_HANDHELD)
/* handheld/desktop port: the host C library (layouts that matter to the game
 * -- bool, the typedefs -- are unchanged). */
#include <string.h>
#include "PR/ultratypes.h"
#else

#include "libc/stddef.h"


const char* strchr(const char* s, int c);
size_t strlen(const char* s);
void* memcpy(void* s1, const void* s2, size_t n);


#endif /* TARGET_HANDHELD */
#endif
