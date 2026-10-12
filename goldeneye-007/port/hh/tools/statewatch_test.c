/* statewatch_test.c: the frame watchdog (hh/hh_plat.c's hh_plat_vi, the real file) across a state load
 * (2026-10-01 device log: "[plat] no new frame since vi 51 (vi 8827, frames 3 ...)" and a thread dump right
 * after "savestate: resuming", while the game ran on at full rate). Replays that log's sequence: a fresh
 * boot shows 3 frames by vi 51, the state (taken at vi ~8776) loads, then frames every 3-4 retraces.
 *   ok: no stall report after the load; a real stall (600 retraces without a frame) still reported once.
 * Run: sh hh/tools/statewatch_test.sh */
#include <stdio.h>
#include <string.h>

static int sDumps;
void hh_os_dump(const char *why) { (void)why; sDumps++; }
void hh_gfx_init(void) {}
void hh_gfx_run_task(void *t) { (void)t; }
void hh_bench_report(void) {}
void hh_audio_push(const void *p, unsigned b, unsigned r) { (void)p; (void)b; (void)r; }
void port_audio_run_cmds(const void *c, unsigned n) { (void)c; (void)n; }
unsigned long long hh_ai_samples;
int hh_vi_black, hh_frame_boundary;
int g_StageNum;
unsigned hh_os_where(int *w, int *t) { *w = 0; *t = -1; return 0; }

void hh_plat_vi(unsigned vicount, int new_frame);
void hh_plat_state_loaded(void);

int main(int argc, char **argv) {
    const int with_hook = !(argc > 1 && strcmp(argv[1], "nohook") == 0);
    unsigned vi;
    int fail = 0;
    for (vi = 1; vi <= 51; vi++) hh_plat_vi(vi, vi == 20 || vi == 35 || vi == 51); /* the fresh boot */
    if (with_hook) hh_plat_state_loaded(); /* hh_os_state_loaded (gfx_savestate_fixup) */
    for (vi = 8777; vi < 9200; vi++) hh_plat_vi(vi, vi % 4 == 0); /* the restored game, ~15 fps */
    printf("after the load: %d stall reports%s\n", sDumps, with_hook ? "" : " (without the hook: the old code)");
    if (with_hook && sDumps != 0) fail = 1;
    const int before = sDumps;
    for (; vi < 9200 + 700; vi++) hh_plat_vi(vi, 0); /* a real stall */
    printf("a real 700-retrace stall: %d report(s)\n", sDumps - before);
    if (sDumps - before != 1) fail = 1;
    printf("%s\n", fail ? "statewatch_test: FAIL" : "statewatch_test: PASS");
    return with_hook ? fail : 0;
}
