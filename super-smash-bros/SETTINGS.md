# Settings and switches

Every switch is an environment variable (`SSB64_<NAME>`, set it in `launch.sh` on the Miyoo). Many can also go in the
settings file, one lowercase `key=value` per line: `App/SuperSmashBros/data/bench.cfg` on the Miyoo. The defaults are what the release plays with; most switches are for testing and A/B comparisons (the
picture is the same either way unless noted).

| Area | Switches |
|---|---|
| Size and resolution | `SSB64_W`/`SSB64_H`, `SSB64_INTRES` (3D %), `SSB64_DYNRES` |
| Run control | `SSB64_FREERUN`, `SSB64_FRAMES`, `SSB64_BENCH`, `SSB64_BENCH_MINUTES` |
| Frame dumps | `SSB64_DUMP`/`SSB64_DUMP_EVERY`/`SSB64_DUMP_FROM` |
| A/B toggles | `SSB64_HUDFILTER=0`, `SSB64_BOXCACHE=0`, `SSB64_CMNEON=0`, `SSB64_VCPROG=0`/`2` (2 = run both, log differences), `SSB64_TILEMASK=0`, `SSB64_TEX32=0/1/2` |
| Renderer toggles | `SSB64_FOGPX=0`, `SSB64_MAGBILIN=1`, `SSB64_CLIPFIX=0`, `SSB64_NEARCLIP=0`, `SSB64_CLIPEDGES=0/1/2`, `SSB64_ZRANGE=1`, `SSB64_SCISSOR=0`, `SSB64_RECTCENTER=0`, `SSB64_LTDROP=0`; `SSB64_NEON=0` (or `neon=0` in `<save dir>/bench.cfg` on the device) turns off the renderer's NEON |
| Texel lerp | `SSB64_LERPROWS=0` (or `lerprows=0` in bench.cfg): the texel-lerp NEON rows off (same pixels; the bench A/B'd it every 5 s until the 2026-10-07 speed round's A/B replaced it) |
| Row speedups | `SSB64_FASTROWS=0`, `SSB64_SPANVEC=0` (takes `SSB64_ZSKIP` with it), `SSB64_ZSKIP=0` (or `fastrows=0` / `spanvec=0` / `zskip=0` in bench.cfg): the Super Mario 64 port's exact row speedups off, one at a time (same pixels; the bench A/Bs them every 5 s) |
| Fill rule | `SSB64_FILLRULE=0` (or `fillrule=0` in bench.cfg): blended / alpha-tested draws without AA_EN take the old inclusive row solve again (a pixel on an edge two triangles share drawn twice); default 1 = owned edges, as the RDP |
| Picture fixes | `SSB64_TLUTINVAL`, `SSB64_MODIFYVTX`, `SSB64_FBCAPTURE`, `SSB64_ZIMAGE`, `SSB64_ZCMP`, `SSB64_PRIMDEPTH`, `SSB64_FRAMEMAP`, `SSB64_ACTHRESH`, `SSB64_MWMATRIX`, and 2026-10-10's `SSB64_LOADSYNC` (the backend's blend/depth flags re-set after a state load): each `=0`, `=1` (the default) or `=ab` (off, then on, every 5 s), or `tlutinval=` ... in bench.cfg; `SSB64_PROBE=1` prints their counters at exit; `SSB64_TRACE_CMDS=<n>` lengthens a traced frame |
| Save states | `SSB64_SAVE_AT`/`SSB64_LOAD_AT`/`SSB64_EXITSAVE_AT`/`SSB64_POWER_AT`/`SSB64_FIXUP_AT=<tick>`, `--loadStateFile <file>`, `SSB64_RESUME_ANSWER=resume` or `new`, `SSB64_MENU_TEST=<dir>`, `SSB64_TESTCLOCK=1` (the game clock = ticks, for title/menu round trips); `tools/qstate.sh`. Statefix (kit k1.8): `SSB64_SAVEREAPPLY=0` (a load keeps the state's save, as before), `SSB64_DECLICK=0` (no fades, as before), `SSB64_SAVEWRITE_AT=<tick>` (the title's save write), `SSB64_MENU_AT=<tick>` (device build: the menu opens and closes) |
| Debug and tracing | `SSB64_PICK=x,y` + `SSB64_TRACE_AT`, `SSB64_DUMPTEX`, `SSB64_PCM` |
| Paths | `SSB64_ROM`, `SSB64_DATA`, `SSB64_CARD` |
| Logging | `SSB64_LOG_APPEND`, `SSB64_QUIET` |


Miyoo only: `threads=1` / `SSB64_THREADS=1` draws on one core, `SSB64_SPLIT=<px>` is the smallest triangle both cores
share, `SSB64_ROTATE=0` draws the picture unturned (see README_MIYOO.md).
