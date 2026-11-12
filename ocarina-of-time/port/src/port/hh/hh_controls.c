/*
 * hh_controls.c -- MENU > OPTIONS > CONTROLS: ORIGINAL or MODERN (debug.cfg controls=0 / 1).
 * Written for the Miyoo Mini port (cacuracaptors/oot-miyoomini, MIT); this code is the port's own.
 *
 * Copyright (c) 2026 cacuracaptors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * MODERN turns on, together (z_player.c, each behind TARGET_HANDHELD):
 *   X jumps (C-left moves to SELECT, and stays on START + left; MENU > CONTROLS can move them), with the automatic
 *   jump off an edge and the automatic ledge grab when walking off one turned off;
 *   running faster with A still held after a roll, and going straight from a roll or a landing into the run;
 *   rolls chained (A again near a roll's end) and steered with the d-pad;
 *   a little steering in the air (at most 45 degrees either side).
 * ORIGINAL is the N64's controls. Each change also has its own key, which wins over the row either way, so each one
 * can be tried alone: jumpbutton, autojump, ledgegrab, sprint (percent; 100 = off), betterroll, airsteer, and
 * jumpstrength (the button jump's least speed x10, default 70).
 */
#include <stdlib.h>
#include "hh_controls.h"

int gHhCtlJumpButton, gHhCtlAutoJump = 1, gHhCtlLedgeGrab = 1, gHhCtlSprint = 100, gHhCtlBetterRoll, gHhCtlAirSteer,
    gHhCtlJumpStrength = 70, gHhCtlJumpHeld, gHhCtlJumpOk;

/* OOT_<key> if set, else the value for this style */
static int key_or(const char* env, int modern, int original, int isModern) {
    const char* e = getenv(env);

    if (e != NULL && e[0] != '\0') {
        return atoi(e);
    }
    return isModern ? modern : original;
}

void HhCtl_Reload(void) {
    const char* e = getenv("OOT_CONTROLS");
    const int m = e != NULL && e[0] == '1';

    gHhCtlJumpButton = key_or("OOT_JUMPBUTTON", 1, 0, m) != 0;
    gHhCtlAutoJump = key_or("OOT_AUTOJUMP", 0, 1, m) != 0;
    gHhCtlLedgeGrab = key_or("OOT_LEDGEGRAB", 0, 1, m) != 0;
    gHhCtlSprint = key_or("OOT_SPRINT", 150, 100, m);
    gHhCtlBetterRoll = key_or("OOT_BETTERROLL", 1, 0, m) != 0;
    gHhCtlAirSteer = key_or("OOT_AIRSTEER", 1, 0, m) != 0;
    gHhCtlJumpStrength = key_or("OOT_JUMPSTRENGTH", 70, 70, m);
    if (gHhCtlSprint < 100) {
        gHhCtlSprint = 100;
    }
    if (gHhCtlSprint > 200) {
        gHhCtlSprint = 200;
    }
}
