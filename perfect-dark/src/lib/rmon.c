#ifdef PLATFORM_N64
#include <ultra64.h>
#include <stdarg.h>
#include "constants.h"
#include "bss.h"
#include "lib/crash.h"
#include "lib/libc/xprintf.h"
#include "data.h"
#include "types.h"
#else
#include <stdarg.h>
#include <stdio.h>
#include "system.h"
#include "types.h"
#include "lib/crash.h"
#endif

void rmonproc()
{
	// empty
}

bool rmonIsDisabled(void)
{
	return true;
}

s32 rmon0002fa10(void)
{
	return -1;
}

void rmon0002fa18(void)
{
	// empty
}

void rmon0002fa20(void)
{
	// empty
}

void rmon0002fa28(void)
{
	// empty
}

void rmon0002fa30(s32 *arg0, s32 arg1)
{
	if (arg0);
	if (arg1);
}

void rmon0002fa38(u32 arg0)
{
	if (arg0);
}

void rmon0002fa40(void)
{
	// empty
}

char *rmonProut(char *dst, const char *src, size_t count)
{
	s32 i = 0;

	while (i != count) {
		crashAppendChar(src[i++]);
	}

	return (char *) 1;
}

void rmonPrintf(const char *format, ...)
{
#ifndef PLATFORM_N64
	char msg[2048];
#endif
	va_list ap;
	va_start(ap, format);

#ifdef PLATFORM_N64
	_Printf(rmonProut, NULL, format, ap);
#else
	vsnprintf(msg, sizeof(msg), format, ap);
	va_end(ap);
	sysLogPrintf(LOG_NOTE, "rmonPrintf: %s", msg);
#endif
}
