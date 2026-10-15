#ifndef _IN_UTILS_H
#define _IN_UTILS_H

#define UTIL_MAX_TOKEN 1024

#include <PR/ultratypes.h>

char *strRightTrim(char *str);
char *strTrim(char *str);
char *strUnquote(char *str);
char *strParseToken(char *str, char *out, s32 *outCount);
char *strFmt(const char *fmt, ...);
char *strDuplicate(const char *str);

#endif
