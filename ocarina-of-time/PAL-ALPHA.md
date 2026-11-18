# Ocarina of Time PAL 1.0 — v0.1 alpha

This experimental native PAL build requires your own unmodified European PAL
1.0 ROM in z64 byte order. Its SHA-256 is
`f29d21e10f77c44d933f07ff6d0f593d4267925f1e6834ce55e239b6037549e7`
(SHA-1 `328a1f1beba30ce5e178f031662019eb32c5f3b5`). ROMs and extracted
game assets are not distributed with the port.

Use the Docker prerequisites in the main README, place the ROM at
`port/baseroms/pal-1.0/baserom.z64`, and from `ocarina-of-time` run:

```sh
./dev.sh 'cd port && make setup VERSION=pal-1.0'
./dev.sh 'cd port && make -k CC_CHECK=true VERSION=pal-1.0 REGION=EU -j4'
./dev.sh 'cd port && make -f Makefile.handheld PLAT=miyoo VERSION=pal-1.0 -j4'
./dev.sh 'VERSION=pal-1.0 PAL_AUDIO_M22=1 tools/build_miyoo.sh'
```

The intermediate N64 header-generation command has produced compiler errors
after generating the required headers in the tested environment; it was stopped
and the handheld build then succeeded. A clean end-to-end build has not been
verified. The output is `OcarinaOfTime-PAL-AudioM22.zip`. PAL and US currently share
handheld build and generated directories: use a clean separate checkout when
switching versions. Follow the README installation instructions for Onion OS;
install the app-only package and place your own matching ROM at
`App/OcarinaOfTime-PAL-AudioM22/rom.z64` on the card.
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
