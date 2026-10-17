#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>
#include <SDL.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "system.h"

#ifdef PLATFORM_WIN32

#include <windows.h>

// on win32 we use waitable timers instead of nanosleep
typedef HANDLE WINAPI (*CREATEWAITABLETIMEREXAFN)(LPSECURITY_ATTRIBUTES, LPCSTR, DWORD, DWORD);
static HANDLE timer;
static CREATEWAITABLETIMEREXAFN pfnCreateWaitableTimerExA;

// winapi also provides a yield macro
#define DO_YIELD() YieldProcessor()

// ask system for high performance GPU, if any
__attribute__((dllexport)) u32 NvOptimusEnablement = 1;
__attribute__((dllexport)) u32 AmdPowerXpressRequestHighPerformance = 1;

#else

#include <unistd.h>

// figure out how to yield
#if defined(PLATFORM_X86) || defined(PLATFORM_X86_64)
// this should work even if the code is not built with SSE enabled, at least on gcc and clang,
// but if it doesn't we'll have to use  __builtin_ia32_pause() or something
#include <immintrin.h>
#define DO_YIELD() _mm_pause()
#elif defined(PLATFORM_ARM) && (defined(PLATFORM_64BIT) || PLATFORM_ARM == 7 || PLATFORM_ARM == 8)
// same as YieldProcessor() on ARM Windows
#define DO_YIELD() __asm__ volatile("dmb ishst\n\tyield":::"memory")
#else
// fuck it
#define DO_YIELD() do { } while (0)
#endif

#endif

#define LOG_FNAME "pd.log"
#define CRASHLOG_FNAME "pd.crash.log"
#define USEC_IN_SEC 1000000ULL

static u64 startTick = 0;
static char logPath[2048];

static s32 sysArgc;
static const char **sysArgv;

static inline void sysLogSetPath(const char *fname)
{
	// figure out where the log is and clear it
	// try working dir first
	snprintf(logPath, sizeof(logPath), "./%s", fname);
	FILE *f = fopen(logPath, "wb");
	if (!f) {
		// try home dir
		sysGetHomePath(logPath, sizeof(logPath) - 1);
		strncat(logPath, "/", sizeof(logPath) - 1);
		strncat(logPath, fname, sizeof(logPath) - 1);
		f = fopen(logPath, "wb");
	}
	if (f) {
		fclose(f);
	}
}

void sysInitArgs(s32 argc, const char **argv)
{
	sysArgc = argc;
	sysArgv = argv;
}

void sysInit(void)
{
	startTick = sysGetMicroseconds();

	if (sysArgCheck("--log")) {
		sysLogSetPath(LOG_FNAME);
	}

#ifdef VERSION_HASH
	sysLogPrintf(LOG_NOTE, "version: " VERSION_BRANCH " " VERSION_HASH " (" VERSION_TARGET ")");
#endif

	char timestr[256];
	const time_t curtime = time(NULL);
	strftime(timestr, sizeof(timestr), "%d %b %Y %H:%M:%S", localtime(&curtime));
	sysLogPrintf(LOG_NOTE, "startup date: %s", timestr);

#ifdef PLATFORM_WIN32
	// this function is only present on Vista+, so try to import it from kernel32 by hand
	pfnCreateWaitableTimerExA = (CREATEWAITABLETIMEREXAFN)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateWaitableTimerExA");
	if (pfnCreateWaitableTimerExA) {
		// function exists, try to create a hires timer
		timer = pfnCreateWaitableTimerExA(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	}
	if (!timer) {
		// no function or hires timers not supported, fallback to lower resolution timer
		sysLogPrintf(LOG_WARNING, "SYS: hires waitable timers not available");
		timer = CreateWaitableTimerA(NULL, FALSE, NULL);
	}
#endif
}

s32 sysArgCheck(const char *arg)
{
	for (s32 i = 1; i < sysArgc; ++i) {
		if (!strcasecmp(sysArgv[i], arg)) {
			return 1;
		}
	}
	return 0;
}

const char *sysArgGetString(const char *arg)
{
	for (s32 i = 1; i < sysArgc; ++i) {
		if (!strcasecmp(sysArgv[i], arg)) {
			if (i < sysArgc - 1) {
				return sysArgv[i + 1];
			}
		}
	}
	return NULL;
}

s32 sysArgGetInt(const char *arg, s32 defval)
{
	for (s32 i = 1; i < sysArgc; ++i) {
		if (!strcasecmp(sysArgv[i], arg)) {
			if (i < sysArgc - 1) {
				return strtol(sysArgv[i + 1], NULL, 0);
			}
		}
	}
	return defval;
}

/* PD_FIXCLOCK=1 (tests): every frame counts as exactly one tick-rate step and the RNG seed is
 * fixed, so qemu runs repeat frame for frame and builds can be compared byte for byte. */
s32 sysFixClock(void)
{
	static s32 on = -1;
	if (on < 0) { const char *e = getenv("PD_FIXCLOCK"); on = e && e[0] && e[0] != '0'; }
	return on;
}

u64 sysGetMicroseconds(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return ((u64)tv.tv_sec * USEC_IN_SEC + (u64)tv.tv_usec) - startTick;
}

s32 sysLogIsOpen(void)
{
	return (logPath[0] != '\0');
}

void sysLogPrintf(s32 level, const char *fmt, ...)
{
	static const char *prefix[3] = {
		"", "WARNING: ", "ERROR: "
	};

	char logmsg[2048];

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(logmsg, sizeof(logmsg), fmt, ap);
	va_end(ap);

	if (logPath[0]) {
		FILE *f = fopen(logPath, "ab");
		if (f) {
			fprintf(f, "%s%s\n", prefix[level], logmsg);
			fclose(f);
		}
	}

	FILE *fout = (level == LOG_NOTE) ? stdout : stderr;
	fprintf(fout, "%s%s\n", prefix[level], logmsg);
}

void sysFatalError(const char *fmt, ...)
{
	static s32 alreadyCrashed = 0;

	if (alreadyCrashed) {
		abort();
	}

	char errmsg[2048] = { 0 };

	alreadyCrashed = 1;

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(errmsg, sizeof(errmsg), fmt, ap);
	va_end(ap);

	sysLogPrintf(LOG_ERROR, "FATAL: %s", errmsg);

	fflush(stdout);
	fflush(stderr);

#ifdef PLATFORM_MIYOO
	{ extern void miyooFatalScreen(const char *msg); miyooFatalScreen(errmsg); }
#else
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Fatal error", errmsg, NULL);
#endif

	exit(1);
}

void sysGetExecutablePath(char *outPath, const u32 outLen)
{
	// try asking SDL
#ifdef PLATFORM_MIYOO
	char *sdlPath = NULL;
#else
	char *sdlPath = SDL_GetBasePath();
#endif

	if (sdlPath && *sdlPath) {
		// -1 to trim trailing slash
		const u32 len = strlen(sdlPath) - 1;
		if (len < outLen) {
			memcpy(outPath, sdlPath, len);
			outPath[len] = '\0';
		}
	} else if (sysArgc && sysArgv[0] && sysArgv[0][0]) {
		// get exe path from argv[0]
		strncpy(outPath, sysArgv[0], outLen - 1);
		outPath[outLen - 1] = '\0';
	} else if (outLen > 1) {
		// give up, use working directory instead
		outPath[0] = '.';
		outPath[1] = '\0';
	}

#ifdef PLATFORM_WIN32
	// replace all backslashes with forward slashes, windows supports both
	for (u32 i = 0; i < outLen && outPath[i]; ++i) {
		if (outPath[i] == '\\') {
			outPath[i] = '/';
		}
	}
#endif

#ifndef PLATFORM_MIYOO
	SDL_free(sdlPath);
#endif
}

void sysGetHomePath(char *outPath, const u32 outLen)
{
	// try asking SDL
#ifdef PLATFORM_MIYOO
	char *sdlPath = NULL;
#else
	char *sdlPath = SDL_GetPrefPath("", "perfectdark");
#endif

	if (sdlPath && *sdlPath) {
		// -1 to trim trailing slash
		const u32 len = strlen(sdlPath) - 1;
		if (len < outLen) {
			memcpy(outPath, sdlPath, len);
			outPath[len] = '\0';
		}
	} else if (outLen > 1) {
		// give up, use working directory instead
		outPath[0] = '.';
		outPath[1] = '\0';
	}

#ifdef PLATFORM_WIN32
	// replace all backslashes with forward slashes, windows supports both
	for (u32 i = 0; i < outLen && outPath[i]; ++i) {
		if (outPath[i] == '\\') {
			outPath[i] = '/';
		}
	}
#endif

#ifndef PLATFORM_MIYOO
	SDL_free(sdlPath);
#endif
}

#ifdef PLATFORM_MIYOO
// port/miyoo/pd_fixed.c: while romdataInit runs, the buffers it keeps come from a pool at a fixed address
// (save states point into them); pool blocks are never freed.
extern void *pdFixedPoolAlloc(size_t size);
extern int pdFixedPoolOwns(const void *p);
extern size_t pdFixedPoolSize(const void *p);
extern void pdFixedPoolFree(void *p);
#endif

void *sysMemAlloc(const u32 size)
{
#ifdef PLATFORM_MIYOO
	void *p = pdFixedPoolAlloc(size);
	if (p) return p;
#endif
	return malloc(size);
}

void *sysMemZeroAlloc(const u32 size)
{
#ifdef PLATFORM_MIYOO
	void *p = pdFixedPoolAlloc(size); // zeroed
	if (p) return p;
#endif
	return calloc(1, size);
}

void *sysMemRealloc(void *ptr, const u32 newSize)
{
#ifdef PLATFORM_MIYOO
	if (ptr && pdFixedPoolOwns(ptr)) {
		const size_t old = pdFixedPoolSize(ptr);
		void *p = pdFixedPoolAlloc(newSize);
		if (!p) p = malloc(newSize);
		if (p) memcpy(p, ptr, old < newSize ? old : newSize);
		return p;
	}
	if (!ptr) return sysMemAlloc(newSize);
#endif
	return realloc(ptr, newSize);
}

void sysMemFree(void *ptr)
{
#ifdef PLATFORM_MIYOO
	if (pdFixedPoolOwns(ptr)) {
		pdFixedPoolFree(ptr);
		return;
	}
#endif
	free(ptr);
}

void sysSleep(const s64 hns)
{
#ifdef PLATFORM_WIN32
	static LARGE_INTEGER li;
	li.QuadPart = -hns;
	SetWaitableTimer(timer, &li, 0, NULL, NULL, FALSE);
	WaitForSingleObject(timer, INFINITE);
#else
	const struct timespec spec = { 0, hns * 100 };
	nanosleep(&spec, NULL);
#endif
}

void sysCpuRelax(void)
{
	DO_YIELD();
}
