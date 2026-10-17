/* pd_fixed.c -- Perfect Dark's memory at fixed addresses, so a save state loads in any later run.
 *
 * A state holds .data/.bss and the game's heap, and those hold pointers into the ROM file mapping and
 * into the buffers romdataInit keeps (the inflated data segment, the fonts, the audio banks). Left to
 * mmap/malloc, those move from run to run: on the Mini a thread's first malloc makes a 1 MB-aligned
 * glibc arena, and when that races ahead of the heap's calloc the heap lands 1.7 MB lower (TEST1:
 * 0xb15ff008 instead of 0xb17a2008), the state no longer matches and RESUME is not offered. Turning
 * address randomisation off (main.c) does not help with that.
 *
 * So the three live at fixed addresses, far below the Mini's mmap area (top-down from 0xb6f.....) and
 * far above the binary and its brk heap (0x10000-0x430000 + a few MB):
 *   0x60000000  the ROM file (up to 64 MB)
 *   0x64000000  the startup pool: every sysMemAlloc while romdataInit runs (bump allocator, never freed)
 *   0x68000000  the game's heap (memp/mema), only when the ROM and the pool are fixed as well, so the
 *               state header's heap address also vouches for the other two
 * Each is mapped with MAP_FIXED_NOREPLACE (4.17+; the Mini's 4.9 kernel ignores the flag and takes the
 * address as a hint) and the result is checked. If one fails, that piece falls back to mmap/malloc as
 * before and the log says so. PD_FIXEDMEM=0 turns it off (tests).
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "kit/savestate.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define PD_FIXED_ROM       0x60000000u
#define PD_FIXED_ROM_MAX   0x04000000u
#define PD_FIXED_POOL      0x64000000u
#define PD_FIXED_POOL_SIZE 0x02000000u /* 32 MB of address space; only what is used is backed */
#define PD_FIXED_HEAP      0x68000000u
#define PD_FIXED_HEAP_MAX  0x40000000u /* to 0xa8000000, still below the mmap area */

static int sOn SAVESTATE_EXCLUDE = -1;
static int sRomFixed SAVESTATE_EXCLUDE, sPoolFixed SAVESTATE_EXCLUDE, sHeapFixed SAVESTATE_EXCLUDE;
static int sPoolActive SAVESTATE_EXCLUDE;
static pthread_t sPoolThread SAVESTATE_EXCLUDE;
static size_t sPoolUsed SAVESTATE_EXCLUDE;
static char sWhy[200] SAVESTATE_EXCLUDE;

static int on(void)
{
	if (sOn < 0) {
		const char *e = getenv("PD_FIXEDMEM");
		sOn = !(e && e[0] == '0');
	}
	return sOn;
}

static void why(const char *what, uintptr_t at, const void *got, int err)
{
	if (sWhy[0]) return; /* the first failure */
	if (got) snprintf(sWhy, sizeof(sWhy), "%s: 0x%08lx is taken (the kernel offered %p)", what, (unsigned long)at, got);
	else snprintf(sWhy, sizeof(sWhy), "%s at 0x%08lx: %s", what, (unsigned long)at, strerror(err));
}

static void *map_at(uintptr_t at, size_t size, int prot, int flags, int fd, const char *what)
{
	void *p = mmap((void *)at, size, prot, flags | MAP_FIXED_NOREPLACE, fd, 0);
	if (p == (void *)at) return p;
	if (p != MAP_FAILED) {
		munmap(p, size);
		why(what, at, p, 0);
	} else {
		why(what, at, NULL, errno);
	}
	return NULL;
}

/* fs.c fsFileMap (the ROM): mapped at PD_FIXED_ROM, or NULL (the caller maps it anywhere). */
void *pdFixedMapRom(int fd, size_t size)
{
	if (!on() || sRomFixed) return NULL;
	if (size > PD_FIXED_ROM_MAX) {
		if (!sWhy[0]) snprintf(sWhy, sizeof(sWhy), "rom: %zu bytes, more than the %u MB room", size, PD_FIXED_ROM_MAX >> 20);
		return NULL;
	}
	void *p = map_at(PD_FIXED_ROM, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, "rom");
	if (p) sRomFixed = 1;
	return p;
}

/* romdataInit: from here to pdFixedPoolEnd, the main thread's sysMemAlloc comes from the pool. */
void pdFixedPoolBegin(void)
{
	if (!on() || sPoolFixed) return;
	if (!map_at(PD_FIXED_POOL, PD_FIXED_POOL_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, "pool")) return;
	sPoolFixed = 1;
	sPoolThread = pthread_self();
	sPoolActive = 1;
}

void pdFixedPoolEnd(void)
{
	sPoolActive = 0;
}

int pdFixedPoolOwns(const void *p)
{
	return sPoolFixed && (uintptr_t)p >= PD_FIXED_POOL && (uintptr_t)p < PD_FIXED_POOL + PD_FIXED_POOL_SIZE;
}

/* 16-byte header (the size) + 16-byte aligned data. NULL when the pool isn't taking allocations. */
void *pdFixedPoolAlloc(size_t size)
{
	if (!sPoolActive || !pthread_equal(pthread_self(), sPoolThread)) return NULL;
	const size_t need = 16 + ((size + 15) & ~(size_t)15);
	if (sPoolUsed + need > PD_FIXED_POOL_SIZE) {
		fprintf(stderr, "fixed memory: the startup pool is full (%zu KB used, %zu more asked)\n", sPoolUsed >> 10, size);
		return NULL;
	}
	unsigned char *h = (unsigned char *)PD_FIXED_POOL + sPoolUsed;
	sPoolUsed += need;
	*(size_t *)h = size;
	memset(h + 16, 0, size); /* fresh anonymous pages are zero already; this also backs them */
	return h + 16;
}

size_t pdFixedPoolSize(const void *p)
{
	return *(const size_t *)((const unsigned char *)p - 16);
}

/* sysMemFree of a pool block: the newest one is given back (a preprocessor's scratch buffer), any other
 * stays (bump allocator). */
void pdFixedPoolFree(void *p)
{
	unsigned char *h = (unsigned char *)p - 16;
	const size_t need = 16 + ((pdFixedPoolSize(p) + 15) & ~(size_t)15);
	if (sPoolActive && h + need == (unsigned char *)PD_FIXED_POOL + sPoolUsed) sPoolUsed = (size_t)(h - (unsigned char *)PD_FIXED_POOL);
}

/* main.c: the game's heap at PD_FIXED_HEAP, or NULL (main.c callocs it as before). */
void *pdFixedHeap(size_t size)
{
	if (!on()) return NULL;
	if (!sRomFixed || !sPoolFixed) return NULL; /* the heap's address in a state stands for all three */
	if (size > PD_FIXED_HEAP_MAX) {
		if (!sWhy[0]) snprintf(sWhy, sizeof(sWhy), "heap: %zu MB, more than the %u MB room", size >> 20, PD_FIXED_HEAP_MAX >> 20);
		return NULL;
	}
	const size_t len = (size + 4095) & ~(size_t)4095;
	void *p = map_at(PD_FIXED_HEAP, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, "heap");
	if (p) sHeapFixed = 1;
	return p;
}

void pdFixedLog(void)
{
	if (!on()) {
		fprintf(stderr, "fixed memory: off (PD_FIXEDMEM=0): ROM, startup buffers and heap where mmap/malloc put them\n");
	} else if (sRomFixed && sPoolFixed && sHeapFixed) {
		fprintf(stderr, "fixed memory: rom 0x%08x, startup pool 0x%08x (%zu KB used), heap 0x%08x\n",
		        PD_FIXED_ROM, PD_FIXED_POOL, sPoolUsed >> 10, PD_FIXED_HEAP);
	} else {
		fprintf(stderr, "fixed memory: FAILED (%s); rom %s, pool %s, heap %s -- falling back to mmap/malloc: "
		        "a save state may not load in a later run\n", sWhy[0] ? sWhy : "?",
		        sRomFixed ? "fixed" : "anywhere", sPoolFixed ? "fixed" : "anywhere", sHeapFixed ? "fixed" : "anywhere");
	}
}
