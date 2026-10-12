# Settings file

- Optional. Put it at `App/OcarinaOfTime/data/debug.cfg`, one `key=value` per line.
- A missing key keeps its default.

| Key | Default | What it does |
|---|---|---|
| `watergrain` | `1` | The Deku Tree basement water's fine grain; `0` = off, about 1 fps faster there |
| `hud` | `2456789` | Which HUD parts are drawn at 1:1 (`1` or `all` = every part) |
| `hud_btnspread` | `1.15` | Spacing of the A/B/C button group (1.0 to 1.34) |
| `hud_msgscale` | `0.92` | Width of the dialog text when `msgbox1to1=0` |
| `msgbox1to1` | `1` | Dialog text at the N64's size: the letters and their spacing as on the N64, the box widened to fill the screen; boxes with an item icon or the ocarina staff and the credits a little narrower so every line fits; `0` = the old layout (box and text squeezed to 0.92) |
| `hudfilter` | `1` | Smoothed HUD, sprites and text at 240; `0` = plain scaling |
| `crop3d` | `2` | Prerendered rooms: `2` = true shape, the view follows Link; `1` = true shape, centred; `0` = squeezed to fit (the old look) |
| `fastwater` | `0` | The Deku Tree basement water as the N64 draws it (two layers); `1` = the old cheaper water (its grain layer skipped, not the N64's look) |
| `fwtint` | `64` | With fastwater: a teal glass over the basement water (0 = off, up to 255); `fwglass=0` = a solid tint instead |
| `fwblends` | `2` | With fastwater: how opaque the basement water is (2 = see-through, 4 = nearly solid) |
| `waterlean` | `1` | A faster way to draw the basement water, the same picture; `0` = the older, slower way |
| `opaquelean` | `1` | A faster way to draw most solid textured surfaces, the same picture; `0` = the older, slower way |
| `alphalean` | `1` | A faster way to draw see-through and cut-out surfaces (leaves, fences, water) and solid surfaces on clamped textures, the same picture; `0` = the older, slower way |
| `skylean` | `1` | A faster way to draw the sky, the same picture; `0` = the older, slower way |
| `leanz` | `1` | A faster way to draw solid surfaces (the common case), the same picture; `0` = the older, slower way |
| `psvcvt` | `1` | A faster shading setup, the same picture; `0` = the older, slower way |
| `uiloop` | `1` | A faster way to draw the HUD's buttons and labels, the same picture; `0` = the older, slower way |
| `nearin` | `1` | A faster way to draw the HUD's buttons and labels (texels read without wrapping when none is needed), the same picture; `0` = the older way |
| `uineon` | `1` | The HUD's tinted buttons and labels drawn with the CPU's vector unit, the same picture; `0` = the older, slower way |
| `2dgen` | `1` | A faster way to draw tinted HUD sprites, the same picture; `0` = the older, slower way |
| `2dlerp` | `1` | Tinted HUD sprites 8 pixels at a time, the same picture; `0` = the older, slower way |
| `skydefer` | `1` | Draws the full-resolution sky last, only where nothing covers it, the same picture and faster outdoors; `0` = the older, slower way |
| `lerplean` | `1` | A faster way to draw tinted surfaces (the colour pulled toward a constant by the texture), the same picture; `0` = the older, slower way |
| `detclamp` | `1` | A faster way to draw two-layer ground and walls on clamped textures (Zora's Domain), the same picture; `0` = the older, slower way |
| `detlerp` | `1` | A faster way to draw two-layer tinted effects (the Song of Storms' clouds), the same picture; `0` = the older, slower way |
| `nozlean` | `1` | A faster way to draw see-through layers that ignore depth (inside Ganon's Castle), the same picture; `0` = the older, slower way |
| `magbil2d` | `1` | A faster way to draw the full-screen smoothed sprites (the Haunted Wasteland's sandstorm, the Song of Storms' cloud cover), the same picture; `0` = the older, slower way |
| `clamptap` | `1` | A faster way to read textures that do not repeat (the big flames, Barinade's room, the sky), the same picture; `0` = the older, slower way |
| `alphaskip` | `1` | Skips the fully see-through parts of flames and cut-out layers sooner, the same picture; `0` = the older, slower way |
| `fastrows` | `1` | A faster way to step from one row of a surface to the next, the same picture; `0` = the older, slower way |
| `zskip` | `1` | Skips runs of 16 pixels that lie wholly behind what is already drawn, the same picture; `0` = the older, slower way |
| `tperows` | `1` | A faster way to rebuild the scrolling flame textures each frame (Volvagia's fire bursts), the same picture; `0` = the older, slower way |
| `blendneon` | `1` | A faster way to rebuild scrolling two-texture layers each frame (lava, lake and fountain water), the same picture; `0` = the older, slower way |
| `tpefast` | `1` | A faster way to rebuild torch flames and other two-texture effects each frame, the same picture; `0` = the older, slower way |
| `pld` | `0` | `1` = fetch the next row's pixels from memory ahead of drawing it, the same picture (speed being measured) |
| `edgetint` | `1` | Cut-out edges (flowers, grass, leaves) drawn in front of the sky take the colours around them, as the N64 shows them; `0` = the old blend with the sky (a blue outline where ground is drawn behind them later) |
| `crackfill` | `1` | Fills one-pixel cracks between pieces of the scenery that the N64 never shows; `0` = off |
| `crackscan` | `1` | Faster scan for those cracks (the same pixels); `0` = the old scan |
| `nearw` | `1` | Whatever comes right up to the camera (a wall, a bush, smoke) stays solid, as on the N64; `0` = the old clip, which cut away anything within 2 units of the eye |
| `fillrule` | `1` | See-through effects (flames, smoke, glows) without a bright seam: where two of their triangles meet, each pixel is drawn once, as on the N64; `0` = the old rule (a thin bright line along the shared edge, e.g. corner to corner across the flames in the Deku Tree's vision) |
| `shadelerp` | `1` | Effects whose colour runs from the vertex colour to a constant by the texture drawn in the N64's colours (the light beams under the Triforce in the Deku Tree's vision: light cyan); `0` = the old colours (those beams dark grey) |
| `magflame` | `1` | The title screen's logo flames as the N64 draws them (a yellow-green glow as the logo fades in, then the orange flames around the shield, sword and ZELDA); `0` = the old flat version (a grey speckled ghost, then a faint grey haze) |
| `rect2t` | `1` | Full-screen and 2D effects made of two textures drawn with both, in the N64's colours (the Haunted Wasteland's sandstorm orange over the whole view, the title logo's flames); `0` = the first texture only (the sandstorm pale cream with clear sand along the bottom) |
| `modifyvtx` | `1` | Model joints the N64 re-maps (gSPModifyVertex) drawn with the right texture there, as on the N64: Barinade's ceiling tentacles, the Bazaar shopkeeper's shoulders, adult Ruto's arm fins (also child Ruto, the roof man, Volvagia, the Like Like, Dead Hand, the Shell Blade, Jabu-Jabu's muscle blocks, the fish); `0` = the old look (the previous joint's texture stretched across the seam); `ab` = off and on in turn every 100 frames, for an A/B |
| `halfwater` | `1` | The Deku Tree basement water (and other see-through surfaces of one colour strength) drawn at half resolution: faster there, softer, the grain mostly lost; `0` = full resolution |
| `fullsky` | `1` | The sky at full resolution; `0` = drawn at half resolution and scaled up (faster outdoors, slightly softer clouds) |
| `pshade` | `1` | Floor and wall shading stays put as the view turns; `0` = the N64's own way (the shading slides on big near triangles) |
| `pacecarry` | `17` | Pacing: a frame up to this many ms late keeps the 20 fps beat, so the next quick frame catches up; `0` = every late frame restarts the beat (the old pacing). Only when frames are shown changes, never what they show |
| `vtxfast` | `1` | A faster vertex transform, the same picture; `0` = the older, slower way |
| `vtxflags` | `0` | `1` = another way to test vertices against the screen edges, the same picture (speed being measured) |
| `vcneon` | `1` | A faster vertex colour combiner, the same picture; `0` = the older, slower way |
| `vdsoclock` | `1` | Reads the clock without a system call; `0` = the system call (the CLOCK line in console.log says which is used) |
| `declick` | `1` | The sound fades out and back in around the menu, a save-state load and a moment the sound runs short, instead of stopping dead; `0` = the old hard cuts |
| `savereapply` | `1` | Loading a save state keeps the saves made since (the file select's files, the sound and Z-targeting options); `0` = the state's older copy comes back, as before |
| `mixprof` | off | `mixprof=1` writes the audio mixer's time per command to `console.log` (MIXPROF lines) |
| `prof` | off | `prof=1` writes a speed profile to `prof.txt` next to `debug.cfg` |

A game log is written to `console.log` in the same folder. The Miyoo-only keys (`rotate`, `threads`, `splitpx`, `cpuclock`, `audiofill`) are in README_MIYOO.md.
