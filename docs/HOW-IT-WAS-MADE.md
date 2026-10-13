# How it was made

## The approach

Each game starts from its community decompilation: C source that the decomp project has matched against the original game, function by function. That code is what runs on the Miyoo. It is compiled for the Mini's ARM CPU, not emulated.

What this project adds around it:

- **A port layer per game.** It stands in for the N64 hardware and OS calls the game expects (threads, timers, controller, cartridge reads, audio), on Linux and the Mini's SDL 1.2.
- **A software renderer.** It descends from the Super Mario 64 PC port's Fast3D and reads the game's own display lists. Its 3D rasteriser is written for the Mini, and it splits each frame's drawing across both cores.
- **A shared kit** for the panel, the in-game menu, save states, the CPU clock and the second-core worker. It is copied into each game.
- **Asset rebuilding on the device.** A game can't ship its data, so each zip carries "recipes": offsets, sizes and conversions, numbers only. On first start the game reads the player's ROM and rebuilds what it needs.
- **Packaging.** One packer builds the same zip layout for every game and every Miyoo OS.

## How it's checked against the original game

- **The decomp code is the reference.** Game logic is the decomp's, so gameplay matches the original. Port changes stay in the port layer, the renderer, or clearly marked handheld fixes.
- **Frame comparisons.** Each game has deterministic test modes (scripted input, fixed timing). Under qemu-arm, a new build's frames are compared byte for byte against the previous build's. A renderer change that's meant to alter the picture is checked against reference frames. Fixes listed on the site are checked against the original game's look.
- **Release gates.** Before a zip is made, scripts check:
  - provenance: every source file is tracked;
  - no ROM-derived data in the binary or zip (a leak scan matches every file against the ROM);
  - no host paths or metadata;
  - the exact file list.
- **Device runs.** Builds are played on a Miyoo Mini Plus. Each game writes a console log with frame timings, memory use and, on a crash, where it happened.

## Working with AI

The ports were written with Claude Code, Anthropic's coding agent.

- **Agents run headless.** They build, run games under qemu, dump frames and compare them, and repeat until the result checks out.
- **Device testing is human.** Builds go on a real Miyoo. Logs, photos and "this feels wrong" reports come back and drive the next round.
- **Long memory lives in the repo.** Commit messages and code comments are detailed on purpose. They record what was measured, what was tried and why a change is shaped the way it is, so a new session doesn't have to rediscover it.
- **A person does QA and makes the decisions:** what to work on, what's good enough, what ships.

## Problems worth knowing about

| Symptom | Cause | Fix |
|---|---|---|
| Frames late by up to ~10 ms | Any short sleep on the Mini's kernel lasts a full ~10 ms tick | Pacers sleep only waits longer than 12 ms, then yield on the monotonic clock up to the deadline |
| Crash (bus error) on ARM only | Converted data written back to back left some structs misaligned | Converters align every item; crash handlers log the fault address |
| Black screen in the launcher after quitting | SDL 1.2 left the framebuffer console in graphics mode | Every game shuts SDL down on exit, with a watchdog |
| Picture boxed in with a border | The game's own viewport was smaller than the frame | The renderer scales the game's viewport to fill the panel |
| Text drawn as solid blocks | IA16 palette entries read in the wrong byte order | Palette reads match the game's layout |
| Gun drawn through walls | A mid-frame depth clear was a no-op in the renderer | Depth is cleared when the game asks for it |
| Game won't start with a ROM whose SHA-1 is right | The copy on the SD card was damaged in transfer | The log shows the data check failing; recopy the file |

## Where to look in the code

| Path | What it does |
|---|---|
| `<game>/miyoo/game.json` | The game's packaging description: folder, files, ROM check |
| `<game>/miyoo/tools/miyoo_pack.py` | Builds the zip for every Miyoo OS (Apps, Onion Ports, MinUI, Allium) |
| `<game>/miyoo/templates/` | `launch.sh` and the per-OS launchers |
| `goldeneye-007/port/hh/gfx/gfx_pc.c` | Display-list interpreter (the Fast3D descendant) |
| `goldeneye-007/port/hh/gfx/gfx_soft.c` | Software rasteriser, with the two-core row split |
| `goldeneye-007/port/hh/gfx/gfx_sdl12.c` | Panel output, input and pacing on SDL 1.2 |
| `goldeneye-007/port/kit/platform/` | Shared kit: menu, save states, CPU clock, worker thread |
| `goldeneye-007/port/hh/hh_os.c` | The N64 OS layer: threads, timers, retrace |
| `goldeneye-007/tools/build_miyoo.sh` | Build plus release gates |
| `goldeneye-007/tools/leak_scan.py` | Refuses a zip that contains anything matching the ROM |

The other games follow the same shape; each game's README lists its own files.
