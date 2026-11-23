# Ocarina of Time PAL 1.0 — v0.1 alpha

This experimental native PAL build requires your own unmodified European PAL
1.0 ROM in z64 byte order. Its SHA-256 is
`f29d21e10f77c44d933f07ff6d0f593d4267925f1e6834ce55e239b6037549e7`
(SHA-1 `328a1f1beba30ce5e178f031662019eb32c5f3b5`). ROMs and extracted
game assets are not distributed with the port.

The validated build used the existing `oot-pal-m1-build:local` Docker image,
an existing populated `port/.venv`, and the existing IDO and EGCS tools. In a
clean checkout, make those tools available and place your ROM at
`port/baseroms/pal-1.0/baserom.z64`. With `ocarina-of-time` mounted as `/work`
in that container, run the following from `/work/port`:

```sh
make -o venv setup VERSION=pal-1.0 -j4
make VERSION=pal-1.0 REGION=EU -j4 asset_files \
  build/pal-1.0/assets/text/message_data.enc.nes.h \
  build/pal-1.0/assets/text/message_data_staff.enc.nes.h
make VERSION=pal-1.0 REGION=EU -j4 build/pal-1.0/assets/audio/sequence_sizes.h
make -f Makefile.handheld PLAT=miyoo VERSION=pal-1.0 -j4
cd /work
VERSION=pal-1.0 tools/build_miyoo.sh
```

`-o venv` reuses the populated environment and skips package installation; the
tools must already be available. The previous broad N64 make invocation also
started N64 compilation, which produced IDO errors. That compilation is not
needed for the handheld build: the explicit targets above generate its asset,
text and sequence-size inputs. A clean isolated ARM32 build of commit
`5b26c72590cdffdc6e09e7538b0b6fa14d35665f` succeeded with this sequence.

That previous candidate’s release gates passed: binary/recipe stamp match, independently derived
recipes and roundtrip, source provenance, `.hhassets` NOBITS, save-state COPY
relocations, embedded and whole-ROM leak budgets, host-path/metadata checks,
and package contents. The package ZIP integrity check also passed. The leak
checks reported 6 matching windows (budget 16) and 61 KiB attributed to existing
decomp source tables (budget 80 KiB); they are not a claim of zero matches.
That previous candidate has stamp `926C0645` and has **not** been tested on hardware.

The output is `OcarinaOfTimePAL-v0.1.0-alpha.zip`. Set `RELEASE_VERSION` to a version such as `v0.1.1-alpha` for subsequent packages; the app folder remains `OcarinaOfTimePAL`. PAL and US currently share
handheld build and generated directories: use a clean separate checkout when
switching versions. Follow the README installation instructions for Onion OS;
install the app-only package and place your own matching ROM at
`App/OcarinaOfTimePAL/rom.z64` on the card.
Keep its data directory separate from other builds and back up your saves.

On a Miyoo Mini Plus with Onion OS, the owner reports successful startup,
German text, graphics, controls, music and sound effects, completing the first
Deku Tree including its boss and reaching the exit of Kokiri Forest. Normal
in-game saving and loading worked within this PAL port. In a hardware A/B test,
the 50 Hz PAL audio thread improved audible synchronization: ring snapshots at
48 kHz fell from 32416, 31568 and 31744 to 2233, 1682 and 2506 output frames.
These are snapshots, not end-to-end latency measurements. Occasional underruns
remain. The video scheduler and NTSC audio timing are unchanged.

Later regions and dungeons, a complete playthrough, PAL/US save compatibility,
save-state compatibility between builds, and long-term stability beyond the
tested route remain unverified. No compatibility or stability guarantee is made.
The hardware-tested build had stamp `00DE7880`; a rebuild uses its own stamp.
