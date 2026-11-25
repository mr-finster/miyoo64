# Paper Mario PAL 1.0.0 — release candidate notes

The planned release supports the European English, German, French and Spanish
game and starts through Onion Apps or Ports. It uses `App/PaperMarioPAL/` and
`App/PaperMarioPAL/data/`; future releases are intended to retain these paths.
The user's own verified PAL ROM is required and is not distributed.

PAL development includes hardware-observed improvements to star pieces, texture
sampling and caching, foliage edges, depth comparison, wall-fire combining and
several gameplay sequences. The current M4.1 edge path is active for its exact
one-cycle render state and can produce darker or harder edges. Isolated
texture-edge/coverage artefacts and other graphical differences can remain.
See `docs/PAL-RENDERER-GFX003-M41.md` for the scope and evidence.

The final guide-free release candidate still needs a reproducible clean build,
release gates and hardware acceptance. No automatic save migration is provided;
users may manually copy a backed-up regular in-game save, without automatically
overwriting existing data. Savestates are tied to their build stamp; compatibility
between builds is not promised. Manual in-game save migration still requires
hardware verification.

Paper Mario PAL 1.0 does not include the Universal Guide.
