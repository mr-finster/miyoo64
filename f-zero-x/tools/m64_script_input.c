/* mupen64plus input plugin: plays FZX_KEYS-style scripts (<poll>:<key>+|-,...) on controller 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { int Present, RawData, Plugin; } CONTROL;
typedef struct { CONTROL *Controls; } CONTROL_INFO;
typedef union { unsigned int Value; } BUTTONS;
static unsigned long sPoll; static int sKeys[12];
static const char *names[] = {"up","down","left","right","a","b","x","y","l","r","start","fn"};
#define EXPORT __attribute__((visibility("default")))
EXPORT int PluginStartup(void *a, void *b, void *c) { return 0; }
EXPORT int PluginShutdown(void) { return 0; }
EXPORT int PluginGetVersion(int *type, int *ver, int *api, const char **name, int *caps) {
    if (type) *type = 4; if (ver) *ver = 0x020500; if (api) *api = 0x020100; if (name) *name = "script"; if (caps) *caps = 0; return 0; }
EXPORT void InitiateControllers(CONTROL_INFO info) { info.Controls[0].Present = 1; info.Controls[0].Plugin = 1; }
EXPORT void ControllerCommand(int c, unsigned char *cmd) {}
EXPORT void ReadController(int c, unsigned char *cmd) {}
EXPORT int RomOpen(void) { sPoll = 0; return 1; }
EXPORT void RomClosed(void) {}
EXPORT void SDL_KeyDown(int m, int k) {}
EXPORT void SDL_KeyUp(int m, int k) {}
EXPORT void GetKeys(int c, BUTTONS *keys) {
    const char *p = getenv("FZX_KEYS"); unsigned v = 0; int x = 0, y = 0;
    if (c != 0) { keys->Value = 0; return; }
    sPoll++;
    for (; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
        char *e; unsigned long at = strtoul(p, &e, 10);
        if (at != sPoll || *e != ':') continue;
        for (int k = 0; k < 12; k++) { size_t n = strlen(names[k]);
            if (!strncmp(e + 1, names[k], n) && (e[1+n] == '+' || e[1+n] == '-')) sKeys[k] = e[1+n] == '+'; }
    }
    if (sKeys[0]) y = 80; if (sKeys[1]) y = -80; if (sKeys[2]) x = -80; if (sKeys[3]) x = 80;
    if (sKeys[4]) v |= 1u << 7; if (sKeys[5]) v |= 1u << 6; if (sKeys[10]) v |= 1u << 4;
    if (sKeys[8]) v |= 1u << 5; if (sKeys[9]) v |= 1u << 12;
    if (sKeys[0]) v |= 1u << 3; if (sKeys[1]) v |= 1u << 2; if (sKeys[2]) v |= 1u << 1; if (sKeys[3]) v |= 1u << 0;
    keys->Value = v | ((unsigned)(x & 0xff) << 16) | ((unsigned)(y & 0xff) << 24);
    if (getenv("SCRIPT_LOG") && (sPoll % 600) == 0) fprintf(stderr, "script: poll %lu\n", sPoll);
}
