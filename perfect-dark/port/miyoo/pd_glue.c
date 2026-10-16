/* pd_glue.c -- the port.h services the GoldenEye kit renderer and the Miyoo kit expect
 * (port/miyoo/gfx/port.h, kit/savestate.c, kit/miyoo_menu.c), for Perfect Dark. */
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <SDL.h>
#include <PR/ultratypes.h>
#include "gfx/port.h"
#include "kit/savestate.h"

uint32_t gPortProfFrame[PROF_COUNT];
uint32_t gPortProfLast[PROF_COUNT];
volatile uint32_t gPortProfFlipUs SAVESTATE_EXCLUDE;
uint32_t hh_menu_us SAVESTATE_EXCLUDE;
uint32_t hh_roundw_fixed;

float identity_matrix[4][4] = {
	{ 1.0f, 0.0f, 0.0f, 0.0f },
	{ 0.0f, 1.0f, 0.0f, 0.0f },
	{ 0.0f, 0.0f, 1.0f, 0.0f },
	{ 0.0f, 0.0f, 0.0f, 1.0f },
};

/* savestate.c: a state loads only into the binary that wrote it. TODO: per-link stamp; and PD's
 * heap (memp/mema, g_MempHeap) is malloc'd, outside .data/.bss, so states need it registered. */
extern const uint32_t port_build_stamp; /* build/miyoo/build_stamp.c, random per link */

/* The game's heap (memp, with mema inside it: everything a stage allocates) is malloc'd by main.c:
 * the kit saves and restores it beside .data/.bss. Addresses are the same on every start because
 * main.c turns address randomisation off (re-exec), so a state also loads after a power cycle. */
extern u8 *g_MempHeap;
extern u32 g_MempHeapSize;
int hh_game_extra_range(char **base, size_t *size)
{
	if (!g_MempHeap || !g_MempHeapSize) return 0;
	*base = (char *)g_MempHeap;
	*size = g_MempHeapSize;
	return 1;
}

static int sQuiet = -1;

void port_log(const char *fmt, ...)
{
	va_list ap;
	if (sQuiet < 0) {
		sQuiet = getenv("PD_QUIET") != NULL;
	}
	if (sQuiet) {
		return;
	}
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

void port_log_always(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

uint32_t port_time_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u);
}

const char *port_save_dir(void)
{
	static char dir[512];
	if (!dir[0]) {
		const char *e = getenv("PD_DATA");
		snprintf(dir, sizeof(dir), "%s", e && e[0] ? e : "./data");
		mkdir(dir, 0755);
	}
	return dir;
}

const char *port_save_path(const char *name)
{
	static char path[640];
	snprintf(path, sizeof(path), "%s/%s", port_save_dir(), name);
	return path;
}

int port_fs_mkdir(const char *path) { return mkdir(path, 0755); }

void *port_seg_to_ptr(uintptr_t addr) { return (void *)addr; }
void port_gfx_overlay(void) {}

void port_request_exit(void)
{
	extern int gfx_wm_shutdown(void);
	static int sExiting;
	if (sExiting++ == 0) {
		savestate_shutdown();
	}
	gfx_wm_shutdown();
	{ extern void miyoo_clock_restore(void); miyoo_clock_restore(); }
	fflush(stdout);
	exit(0);
}

/* The game's clock runs on the host clock (port/src/libultra.c osGetCount) and each frame's step is
 * the real time since the last one (timing.c). After the menu (gfx_sdl12.c) and a state load
 * (gfx_pc.c), the next step starts from now: the whole pause in one step banked the view (TEST12). */
extern void frametimeRebase(void);
void hh_os_rebase_clock(void) { frametimeRebase(); }
void hh_os_state_loaded(void) { frametimeRebase(); }
/* A crash: the signal, the faulting address, PC and LR, and the stack words that point into the
 * game's code (a raw backtrace). The binary is non-PIE, so these are link addresses: resolve them
 * with the build's map or addr2line -e pd.arm (build/miyoo/pd.map). */
#include <ucontext.h>
extern char __executable_start[], etext[];
static int inText(uintptr_t a) { return a >= (uintptr_t)__executable_start && a < (uintptr_t)etext; }
void hh_crash_handler_chain(int sig, siginfo_t *si, void *ucv)
{
	char buf[1024];
	int n;
	ucontext_t *uc = (ucontext_t *)ucv;
	n = snprintf(buf, sizeof(buf), "CRASH: signal %d (%s) addr %p", sig, strsignal(sig), si ? si->si_addr : NULL);
	if (uc) {
		const mcontext_t *m = &uc->uc_mcontext;
		n += snprintf(buf + n, sizeof(buf) - n, " pc %08lx lr %08lx sp %08lx fault %08lx | r0 %08lx r1 %08lx r2 %08lx r3 %08lx",
			(unsigned long)m->arm_pc, (unsigned long)m->arm_lr, (unsigned long)m->arm_sp, (unsigned long)m->fault_address,
			(unsigned long)m->arm_r0, (unsigned long)m->arm_r1, (unsigned long)m->arm_r2, (unsigned long)m->arm_r3);
		n += snprintf(buf + n, sizeof(buf) - n, "\nCRASH: stack code refs:");
		const uintptr_t *sp = (const uintptr_t *)m->arm_sp;
		int found = 0;
		for (int i = 0; i < 256 && found < 24 && n < (int)sizeof(buf) - 12; i++) {
			if (inText(sp[i])) { n += snprintf(buf + n, sizeof(buf) - n, " %08lx", (unsigned long)sp[i]); found++; }
		}
	}
	fprintf(stderr, "%s\nCRASH: build %08X; resolve with addr2line -f -e pd.arm <pc>\n", buf, (unsigned)port_build_stamp);
	{ extern void gfx_dump_recent(FILE *f); gfx_dump_recent(stderr); }
	fflush(stderr);
	{ extern void miyoo_clock_restore(void); miyoo_clock_restore(); }
	signal(sig, SIG_DFL);
	raise(sig);
}

/* stdout goes to App/PerfectDark/data/console.log (launch.sh): one line at a time, so a crash
 * loses nothing. */
__attribute__((constructor)) static void pdLineBuffered(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
}

/* The kit's audio hooks (savestate.c, the menu's mute): the SDL 1.2 lock around the callback. */
void port_audio_backstop_enable(int on) { (void)on; }
void port_audio_mix_lock(void) { SDL_LockAudio(); }
void port_audio_mix_unlock(void) { SDL_UnlockAudio(); }
void port_audio_out_set_muted(int muted) { SDL_PauseAudio(muted ? 1 : 0); }

extern int g_StageNum;
int bossGetStageNum(void) { return g_StageNum; }

/* sysFatalError's screen: the message on the panel until a button (pd_romscreen.c; before 1.0.1 the
 * log only), then back to the OS. */
void miyooFatalScreen(const char *msg)
{
	extern int gfx_wm_shutdown(void) __attribute__((weak));
	extern void pd_romscreen_fatal(const char *msg);
	fprintf(stderr, "FATAL: %s\n", msg);
	fflush(stderr);
	fflush(stdout);
	pd_romscreen_fatal(msg);
	/* back to the OS's menu in a usable state: SDL 1.2's fbcon down, the clock as found
	 * (sysFatalError exits right after) */
	if (gfx_wm_shutdown) gfx_wm_shutdown();
	{ extern void miyoo_clock_restore(void); miyoo_clock_restore(); }
}
