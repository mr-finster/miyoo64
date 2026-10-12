/* pmcap video plugin: no rendering; dumps RDRAM (big-endian, as on the N64) plus the gfx
 * OSTask header for each captured gfx task. Output: $PMCAP_OUT/<name>_NNN.pmc
 * Captures contain ROM-derived data: local files only, never committed or shipped. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#define M64P_PLUGIN_PROTOTYPES 1
#include <mupen64plus/m64p_types.h>
#include <mupen64plus/m64p_plugin.h>

extern volatile unsigned pmcap_poll;
extern volatile int pmcap_cap_left, pmcap_cap_index;
extern char pmcap_cap_name[64];
static GFX_INFO gi;
static unsigned ntasks;

static uint32_t dmem32(unsigned off) { return *(uint32_t *)(gi.DMEM + off); } /* DMEM is host-order words */

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle core, void *ctx, void (*dbg)(void *, int, const char *)) {
    (void)core; (void)ctx; (void)dbg; return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL PluginShutdown(void) { fprintf(stderr, "pmcap-video: %u gfx tasks\n", ntasks); return M64ERR_SUCCESS; }
EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *t, int *v, int *api, const char **name, int *caps) {
    if (t) *t = M64PLUGIN_GFX; if (v) *v = 0x010000; if (api) *api = 0x020200;
    if (name) *name = "pmcap capture"; if (caps) *caps = 0; return M64ERR_SUCCESS;
}
EXPORT int CALL InitiateGFX(GFX_INFO g) { gi = g; return 1; }
EXPORT int CALL RomOpen(void) { return 1; }
EXPORT void CALL RomClosed(void) {}
EXPORT void CALL ProcessDList(void) {
    uint32_t type = dmem32(0xFC0), ucode = dmem32(0xFD0), dl = dmem32(0xFF0), dlsize = dmem32(0xFF4);
    if (type == 1) {
        ntasks++;
        if (pmcap_cap_left > 0) {
            const char *dir = getenv("PMCAP_OUT"); char path[512];
            snprintf(path, sizeof path, "%s/%s_%03d.pmc", dir ? dir : ".", pmcap_cap_name, pmcap_cap_index);
            FILE *f = fopen(path, "wb");
            if (f) {
                uint32_t size = 4 << 20, hdr[8] = { 0x31434D50 /* "PMC1" */, dl, dlsize, ucode, size, pmcap_poll, ntasks, 0 };
                uint32_t *be = malloc(size);
                for (uint32_t i = 0; i < size / 4; i++) be[i] = __builtin_bswap32(((uint32_t *)gi.RDRAM)[i]);
                fwrite(hdr, sizeof hdr, 1, f); fwrite(be, size, 1, f); fclose(f); free(be);
                fprintf(stderr, "pmcap-video: %s dl=%08x poll=%u\n", path, dl, pmcap_poll);
            }
            pmcap_cap_index++; pmcap_cap_left--;
        }
    }
    *gi.MI_INTR_REG |= 0x20; /* DP interrupt: the RDP "finished" */
    gi.CheckInterrupts();
}
EXPORT void CALL ProcessRDPList(void) {}
EXPORT void CALL ShowCFB(void) {}
EXPORT void CALL UpdateScreen(void) {}
EXPORT void CALL ViStatusChanged(void) {}
EXPORT void CALL ViWidthChanged(void) {}
EXPORT void CALL ChangeWindow(void) {}
EXPORT void CALL MoveScreen(int x, int y) { (void)x; (void)y; }
EXPORT void CALL ReadScreen2(void *d, int *w, int *h, int front) { (void)d; (void)front; *w = 0; *h = 0; }
EXPORT void CALL SetRenderingCallback(void (*cb)(int)) { (void)cb; }
EXPORT void CALL ResizeVideoOutput(int w, int h) { (void)w; (void)h; }
EXPORT void CALL FBRead(unsigned a) { (void)a; }
EXPORT void CALL FBWrite(unsigned a, unsigned s) { (void)a; (void)s; }
EXPORT void CALL FBGetFrameBufferInfo(void *p) { (void)p; }
