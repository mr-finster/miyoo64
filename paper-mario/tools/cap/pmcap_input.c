/* pmcap input plugin: scripted controller for mupen64plus.
 * PMCAP_SCRIPT=file, one event per line, keyed by controller-poll count:
 *   <poll> press <btn[+btn..]|-> <polls> [x y]   hold buttons (and stick) for <polls> polls
 *   <poll> shot                                  take the next screenshot (video plugin permitting)
 *   <poll> cap <n> <name>                        capture the next <n> gfx tasks as <name>_NNN
 *   <poll> save <path>                           save the emulator state (mupen64plus format) -- the same instant
 *                                                for the real game in m64ref (--savestate), for fix sheets
 *   <poll> quit
 * Buttons: A B Z S L R CU CD CL CR DU DD DL DR. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#define M64P_PLUGIN_PROTOTYPES 1
#include <mupen64plus/m64p_types.h>
#include <mupen64plus/m64p_plugin.h>

extern volatile unsigned pmcap_poll;
extern volatile int pmcap_cap_left, pmcap_cap_index;
extern char pmcap_cap_name[64];

typedef m64p_error (*core_cmd_t)(m64p_command, int, void *);
static core_cmd_t core_cmd;
enum { EV_PRESS, EV_SHOT, EV_CAP, EV_QUIT, EV_SAVE };
typedef struct { unsigned at, len; int type; unsigned btn; int x, y, n; char name[256]; } Ev;
static Ev evs[4096];
static int nev;

static unsigned parse_btn(const char *s) {
    static const struct { const char *n; unsigned b; } T[] = {
        {"DR",1<<0},{"DL",1<<1},{"DD",1<<2},{"DU",1<<3},{"S",1<<4},{"Z",1<<5},{"B",1<<6},{"A",1<<7},
        {"CR",1<<8},{"CL",1<<9},{"CD",1<<10},{"CU",1<<11},{"R",1<<12},{"L",1<<13}};
    unsigned b = 0; char buf[128]; strncpy(buf, s, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (char *t = strtok(buf, "+"); t; t = strtok(NULL, "+"))
        for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(t, T[i].n)) b |= T[i].b;
    return b;
}

static void load_script(void) {
    const char *p = getenv("PMCAP_SCRIPT"); FILE *f; char line[256];
    if (!p || !(f = fopen(p, "r"))) { fprintf(stderr, "pmcap-input: no PMCAP_SCRIPT\n"); return; }
    while (fgets(line, sizeof line, f) && nev < 4096) {
        Ev e = {0}; char kw[16] = "", a1[64] = "", a2[64] = "";
        if (line[0] == '#' || sscanf(line, "%u %15s", &e.at, kw) < 2) continue;
        if (!strcmp(kw, "press")) { e.type = EV_PRESS; sscanf(line, "%*u %*s %63s %u %d %d", a1, &e.len, &e.x, &e.y); e.btn = parse_btn(a1); }
        else if (!strcmp(kw, "shot")) e.type = EV_SHOT;
        else if (!strcmp(kw, "cap")) { e.type = EV_CAP; sscanf(line, "%*u %*s %d %63s", &e.n, e.name); }
        else if (!strcmp(kw, "save")) { e.type = EV_SAVE; sscanf(line, "%*u %*s %255s", e.name); }
        else if (!strcmp(kw, "quit")) e.type = EV_QUIT;
        else continue;
        (void)a2; evs[nev++] = e;
    }
    fclose(f);
    fprintf(stderr, "pmcap-input: %d events\n", nev);
}

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle core, void *ctx, void (*dbg)(void *, int, const char *)) {
    (void)ctx; (void)dbg; core_cmd = (core_cmd_t)dlsym(core, "CoreDoCommand"); load_script(); return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL PluginShutdown(void) { return M64ERR_SUCCESS; }
EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *t, int *v, int *api, const char **name, int *caps) {
    if (t) *t = M64PLUGIN_INPUT; if (v) *v = 0x010000; if (api) *api = 0x020100;
    if (name) *name = "pmcap scripted input"; if (caps) *caps = 0; return M64ERR_SUCCESS;
}
EXPORT void CALL InitiateControllers(CONTROL_INFO ci) {
    for (int i = 0; i < 4; i++) { ci.Controls[i].Present = i == 0; ci.Controls[i].RawData = 0; ci.Controls[i].Plugin = PLUGIN_NONE; }
}
EXPORT void CALL GetKeys(int c, BUTTONS *k) {
    k->Value = 0;
    if (c != 0) return;
    unsigned p = pmcap_poll++;
    for (int i = 0; i < nev; i++) {
        Ev *e = &evs[i];
        if (e->type == EV_PRESS && p >= e->at && p < e->at + (e->len ? e->len : 1)) {
            k->Value |= e->btn; if (e->x || e->y) { k->X_AXIS = e->x; k->Y_AXIS = e->y; }
        } else if (p == e->at) {
            if (e->type == EV_SHOT && core_cmd) core_cmd(M64CMD_TAKE_NEXT_SCREENSHOT, 0, NULL);
            else if (e->type == EV_CAP) { pmcap_cap_left = e->n; pmcap_cap_index = 0; strncpy(pmcap_cap_name, e->name, 63); pmcap_cap_name[63] = 0; }
            else if (e->type == EV_SAVE && core_cmd) { fprintf(stderr, "pmcap-input: save %s at poll %u\n", e->name, p); core_cmd(M64CMD_STATE_SAVE, 1, e->name); }
            else if (e->type == EV_QUIT && core_cmd) { fprintf(stderr, "pmcap-input: quit at poll %u\n", p); core_cmd(M64CMD_STOP, 0, NULL); }
        }
    }
}
EXPORT void CALL ControllerCommand(int c, unsigned char *cmd) { (void)c; (void)cmd; }
EXPORT void CALL ReadController(int c, unsigned char *cmd) { (void)c; (void)cmd; }
EXPORT int CALL RomOpen(void) { return 1; }
EXPORT void CALL RomClosed(void) {}
EXPORT void CALL SDL_KeyDown(int m, int s) { (void)m; (void)s; }
EXPORT void CALL SDL_KeyUp(int m, int s) { (void)m; (void)s; }
