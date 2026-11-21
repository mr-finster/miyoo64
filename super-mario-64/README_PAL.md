# Super Mario 64 PAL for Miyoo Mini Plus

This experimental port adds native European PAL support alongside the existing
US port. It uses the European game's English, German and French text, a separate
PAL asset recipe, and a separate `SuperMario64PAL` Onion application and data
directory. PAL and US save states are build-specific and must not be exchanged.

## Installation

Unzip `SuperMario64PAL-onion.zip` to the root of the Onion SD card. Supply your
own Super Mario 64 (Europe) ROM on the card. The ROM must have SHA-1
`4ac5721683d0e0b6bbb561b58a71740845dceea9` in canonical big-endian order.
The game finds and verifies the ROM, reconstructs its assets on first launch,
and stores the generated cache and saves locally. The package contains no ROM
or extracted game assets. Start **Super Mario 64 PAL** from Apps.

## Implementation and validation

The EU build selects its own ROM region and SHA-1 and `recipes_miyoo_eu.json`;
the US build keeps its existing ROM and recipe. The software renderer handles
`G_TEXRECTFLIP` for EU glyphs. PAL pacing targets 25 game iterations per second,
with two 50 Hz audio buffers per iteration.

On a real Miyoo Mini Plus, the game launched, music played, language selection
and controls worked, and a regular save worked. Under ARM QEMU, English,
German and French were legible in the language menu and Peach's letter; ROM
recognition and asset reconstruction worked. Asset and leak checks passed.
One QEMU run reported 66 audio underruns and 98 backstop mixes in its first
750 iterations; this observation does not establish their effect on hardware.

Longer gameplay and a complete playthrough, hardware save states, long-term
audio stability and a full US build regression remain unconfirmed. Changed
handheld objects were compiled with US defines, but a complete US build and run
was not performed. See [README_MIYOO.md](README_MIYOO.md) for general build
and asset handling information. Never distribute the ROM or generated cache.
