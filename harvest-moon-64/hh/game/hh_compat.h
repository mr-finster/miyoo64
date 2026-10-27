/* Included first in every game-side file (Makefile.handheld). */
#ifndef HH_COMPAT_H
#define HH_COMPAT_H
/* mainLoop.c's busy waits for the retrace: one thread here, so the wait runs the retrace (hh_nusys.c) */
void hh_wait_retrace(void);
#define HH_WAIT_RETRACE() hh_wait_retrace()
/* the main loop's own wait between two steps: the one place a save state is taken or restored (hh_host.c) */
void hh_wait_retrace_main(void);
#define HH_WAIT_RETRACE_MAIN() hh_wait_retrace_main()
#ifdef TARGET_HANDHELD
/* A float converted to an unsigned type, as the N64 converts it. On the N64 (MIPS trunc.w) and on x86 a negative
 * float wraps: (u8)-1.0f is 255. ARM's vcvt.u32 saturates it to 0 instead, so a negative tile coordinate, colour or
 * screen position comes out as 0 here. HM64_F2U(v) is the N64's result on both sides of zero; common.h has the
 * N64 build's (v). */
#define HM64_F2U(v) __extension__ ({ __typeof__(v) hm64_f2u_ = (v); \
    hm64_f2u_ < 0 ? (unsigned int) (int) hm64_f2u_ : (unsigned int) hm64_f2u_; })
#endif
#endif
