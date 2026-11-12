/*
 * hh_controls.h -- MENU > OPTIONS > CONTROLS: MODERN (hh_controls.c)
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
 */
#ifndef HH_CONTROLS_H
#define HH_CONTROLS_H

/* read by z_player.c each frame; set by HhCtl_Reload (at start and from the options page) */
extern int gHhCtlJumpButton;   /* OOT_JUMPBUTTON: the jump button jumps */
extern int gHhCtlAutoJump;     /* OOT_AUTOJUMP: running off an edge jumps by itself (only off with a jump button) */
extern int gHhCtlLedgeGrab;    /* OOT_LEDGEGRAB: walking off an edge, Link hangs on to it */
extern int gHhCtlSprint;       /* OOT_SPRINT: the run's speed after a roll with A held, in %; 100 = off */
extern int gHhCtlBetterRoll;   /* OOT_BETTERROLL: rolls chained and steered */
extern int gHhCtlAirSteer;     /* OOT_AIRSTEER: a little steering in the air */
extern int gHhCtlJumpStrength; /* OOT_JUMPSTRENGTH: the button jump's least speed, x10 */
extern int gHhCtlJumpHeld;     /* hh_input.c: the jump button is down */
extern int gHhCtlJumpOk;       /* z_player.c: Link can jump now (6, counted down by each read of the buttons) */

void HhCtl_Reload(void); /* hh_main.c after debug.cfg; hh_options.c's CONTROLS row */

#endif
