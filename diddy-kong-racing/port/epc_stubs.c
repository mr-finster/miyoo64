/**
 * The N64 build's crash handler (thread0_epc.c) and stack probes, which the
 * port does not have: a crash is a host signal with a backtrace in the log.
 */
#include <ultra64.h>
#include "memory.h"
#include "thread0_epc.h"
#include "port.h"
#include <stdlib.h>

/* libm_vals.s: the quiet NaN libultra's sinf/cosf return for bad input. */
const float __libm_qnan_f = __builtin_nanf("0x10000");

void update_object_stack_trace(s32 index, s32 value) { }
s32 get_lockup_status(void) { return 0; }
void mode_lockup(s32 updateRate) { }
void render_epc_lock_up_display(void) { }
void thread3_verify_stack(void) { }

void dump_memory_to_cpak(s32 epc, s32 size, u32 colourTag) {
    /* The N64 build dumps the stack to a Controller Pak and crashes; carrying
     * on with a NULL block only hides where things went wrong. */
    port_log_always("mempool: allocation failed (size %d, tag %08X) -- fatal\n", (int) size, (unsigned) colourTag);
    abort();
}

StackInfo *stack_pointer(void) {
    static StackInfo sInfo;
    sInfo.sp = (u32) (uintptr_t) __builtin_frame_address(0);
    return &sInfo;
}

/* IO_READ (PR/rcp.h): the answers a genuine cartridge and console give to the
 * game's anti-tamper probes, which otherwise sabotage physics and the camera. */
unsigned int port_io_read(unsigned int addr) {
    switch (addr) {
        case 0x200: return 0xAC290000; /* tracks.c: IPL3 word */
        case 0x284: return 0x240B17D7; /* object_functions.c: IPL3 word */
        case SP_DMEM_START: return 0xFFFFFFFFu;
        case SP_IMEM_START: return CIC_ID;
        default: return 0; /* PI status etc: idle */
    }
}

/* libultra's debug assert (libultra/src/debug/assert.h). */
void __assert(const char *exp, const char *filename, int line) {
    port_log_always("assert: %s (%s:%d)\n", exp, filename, line);
}
