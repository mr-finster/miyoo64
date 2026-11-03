# Native PAL support (current-main integration)

This branch adds PAL ROM identity, region-isolated generators, PAL message/title/language/HUD handling and the sprite return correction to current upstream. The general overlay implementation is retained; old per-ability resets are not added.

Supply your own canonical Paper Mario (Europe, PAL) ROM, SHA-1 2111d39265a317414d359e35a7d971c4dfa5f9e1. After configuring/splitting the pinned decomp for PAL, run tools/port/pal_build.py --rom <own-pal.z64> from paper-mario mounted at /work in the documented pm64hh:decomp environment (ARM toolchain plus PyYAML). --plan prints the region-specific pipeline without ROM or compiler access. Every build derives fresh matching recipes under ignored build/game/pal; no PAL recipes are committed.

The package definition miyoo/game-pal.json selects App/PaperMarioPAL with its own data directory. Run the fatal pal_release_gates.py checks on the final packaged app. Never substitute the historical alpha recipes for a newly linked overlay build.

This branch has host/regression checks only: no ARM rebuild or hardware validation is claimed. It differs from the hardware-tested historical release snapshot. A new ARM binary/recipe pair and hardware regression are required. Preserve the pinned upstream source notices; upstream MIT covers port code only, not all decompiled/third-party code.

## Source build prerequisites

Use the pinned pmret/papermario commit ee321921df2debdaae4c9c00426b6ea84f26adb5 and apply game/patches/decomp.patch. Do not version the materialized refs tree. The historical build used the offline pm64hh:decomp image sha256:0c42a871ebcd90da2a7cde0ce981fd02dbd53683f9ff771c86af7b21ad1b0289 (extends the validated ARM build image and supplies PyYAML/Splat).

Mount paper-mario at /work and the user's canonical PAL ROM read-only at /work/refs/papermario/ver/pal/baserom.z64. Before ./configure pal, run `python3 -B tools/port/prepare_splat_provider.py`. It checks the installed splat64 0.31.0 provider and reproduces the exact validated Makefile adapter; it never downloads packages or replaces a different provider. Then run ./configure pal in refs/papermario, and its generated_code_pal / inc_img_bins_pal Ninja targets. Only after that run `python3 -B tools/port/pal_build.py --rom refs/papermario/ver/pal/baserom.z64` from /work.

The build forces game-object recompilation, emits a new nonzero buildstamp, and derives fresh recipes into ignored build/game/pal. No inherited US recipe or runtime cache is reused. Package with miyoo/game-pal.json, run pal_release_gates.py and perform an independent hardware test. The provided historical alpha ZIP is a repackaging of the validated binary, not a fresh build from the new release metadata. Newly built binaries are not claimed to have its exact checksum or hardware validation.
