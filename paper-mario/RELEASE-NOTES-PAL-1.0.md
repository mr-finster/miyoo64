# Paper Mario PAL 1.0.0 — release notes

The release supports the European English, German, French and Spanish
game and starts through Onion Apps or Ports. It uses `App/PaperMarioPAL/` and
`App/PaperMarioPAL/data/`; future releases are intended to retain these paths.
The user's own verified PAL ROM is required and is not distributed.

PAL development includes hardware-observed improvements to star pieces, texture
sampling and caching, foliage edges, depth comparison, wall-fire combining and
several gameplay sequences. The current M4.1 edge path is active for its exact
one-cycle render state and can produce darker or harder edges. Isolated
texture-edge/coverage artefacts and other graphical differences can remain.
See `docs/PAL-RENDERER-GFX003-M41.md` for the scope and evidence.

A clean ARM32 build and release gates passed, including binary/recipes pairing,
build stamp and ROM/asset leak checks. Hardware testing on Miyoo Mini Plus
confirmed Apps and Ports launch, their icons, languages, menus, regular save/load,
savestates and many renderer and gameplay scenes. Texture-edge/coverage issues
and some visual differences in Dry Dry Ruins remain. The overlay runtime host
test remains blocked by eight unlinked asset symbols; test coverage is not complete.

No automatic save migration is provided; users may manually copy a backed-up
regular in-game save without overwriting existing data. Migration from older
alpha versions has not been separately hardware verified. Savestates are tied to
their build stamp; compatibility between builds is not promised.

Paper Mario PAL 1.0 does not include the Universal Guide.
