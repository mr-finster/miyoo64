/* Controller 1 for the native port (M2). Host side. On the desktop proxy a pad script drives it, keyed by game
 * frame: PM64_KEYS="120:start+,124:start-,300:a+,302:a-" (keys a b z start l r cu cd cl cr du dd dl dr, and the
 * stick up down left right). The device's buttons come with the device build. */
#include <stdlib.h>
#include <string.h>
#include "port.h"

/* N64 button bits (PR/os_cont.h) */
enum { B_A = 0x8000, B_B = 0x4000, B_Z = 0x2000, B_START = 0x1000, B_DU = 0x0800, B_DD = 0x0400, B_DL = 0x0200,
       B_DR = 0x0100, B_L = 0x0020, B_R = 0x0010, B_CU = 0x0008, B_CD = 0x0004, B_CL = 0x0002, B_CR = 0x0001 };

static uint16_t sButtons;
static int8_t sStickX, sStickY;
static const char *sScript;

static int key_bit(const char *k, int n, int *stick) {
    static const struct { const char *name; int bit, stick; } keys[] = {
        { "a", B_A, 0 }, { "b", B_B, 0 }, { "z", B_Z, 0 }, { "start", B_START, 0 }, { "l", B_L, 0 }, { "r", B_R, 0 },
        { "cu", B_CU, 0 }, { "cd", B_CD, 0 }, { "cl", B_CL, 0 }, { "cr", B_CR, 0 }, { "du", B_DU, 0 }, { "dd", B_DD, 0 },
        { "dl", B_DL, 0 }, { "dr", B_DR, 0 }, { "up", 0, 1 }, { "down", 0, 2 }, { "left", 0, 3 }, { "right", 0, 4 } };
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++)
        if ((int) strlen(keys[i].name) == n && !strncmp(keys[i].name, k, (size_t) n)) { *stick = keys[i].stick; return keys[i].bit; }
    *stick = 0;
    return 0;
}

void port_script_poll(void) {
    if (!sScript) { sScript = getenv("PM64_KEYS"); if (!sScript) sScript = ""; }
    for (const char *p = sScript; *p; ) { /* apply every event of this frame */
        char *end;
        const unsigned long f = strtoul(p, &end, 10);
        if (end == p || *end != ':') break;
        const char *k = end + 1;
        int n = 0;
        while (k[n] && k[n] != '+' && k[n] != '-') n++;
        const int down = k[n] == '+';
        if (f == port_frame) {
            int stick;
            const int bit = key_bit(k, n, &stick);
            if (bit) sButtons = down ? (uint16_t) (sButtons | bit) : (uint16_t) (sButtons & ~bit);
            if (stick == 1) sStickY = down ? 80 : 0;
            if (stick == 2) sStickY = down ? -80 : 0;
            if (stick == 3) sStickX = down ? -80 : 0;
            if (stick == 4) sStickX = down ? 80 : 0;
        }
        p = k + n + (k[n] ? 1 : 0);
        if (*p == ',') p++;
    }
}

void port_script_read(uint16_t *button, int8_t *stick_x, int8_t *stick_y) {
    *button = sButtons;
    *stick_x = sStickX;
    *stick_y = sStickY;
}

#ifndef PM64_SDL12 /* the desktop proxy has only the script; the device ORs it into its buttons (port_sdl.c) */
void port_input_poll(void) { port_script_poll(); }
void port_input_read(uint16_t *button, int8_t *stick_x, int8_t *stick_y) { port_script_read(button, stick_x, stick_y); }
#endif
