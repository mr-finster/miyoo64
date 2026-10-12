/* acmd_trace.h -- BattleShip's audio-command tracer compiled out (no-ops). */
#ifndef ACMD_TRACE_H
#define ACMD_TRACE_H
#include <stdint.h>
static inline void acmd_trace_init(void) {}
static inline void acmd_trace_begin_task(void) {}
static inline void acmd_trace_end_task(void) {}
static inline void acmd_trace_log_cmd(uint32_t w0, uint32_t w1) { (void)w0; (void)w1; }
#endif
