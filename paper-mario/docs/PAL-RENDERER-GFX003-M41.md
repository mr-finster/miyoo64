# Paper Mario PAL: GFX003 and M4.1

## GFX003: star pieces and TEXEL1 UV coordinates

Star pieces were sometimes invisible. In the generic TEXEL1-only path, the UV repeat drop used the TEXEL0 coordinate frame even though TEXEL1 uses its own sampler and coordinates. The guard in `gfx_emit_triangle` disables that drop for this state; the existing linked TEXEL0/TEXEL1 handling remains in place.

Commit `b87fb593ec26107a687b89cea08424200268922e` preserves the tested guard on baseline `64bc67c0974f7fe05c15bba29a4fcbd2a0bb1ddf`. On Miyoo Mini Plus, build `A2A0B224` displayed the star piece correctly according to the hardware observation. Original tile-0 state was not fully measured and no extra instrumented A/B trace of the fix build was captured. This is a hardware-confirmed fix candidate, not a claim that every UV state has been verified.

## M4.1: texture-edge blend

A leaf fragment measured on hardware had RGBA `(11,36,7,90)` over framebuffer RGB `(132,181,222)` and the existing software path produced `(89,129,146)`. A conspicuous blue-green fringe was visible along foliage contours.

M4.1 is active for the PAL build and disabled for the US build to preserve the existing US edge appearance. `pm_m41_edge()` matches a one-cycle draw whose complete OtherModeL equals `0x00553078` (`G_RM_AA_ZB_TEX_EDGE | G_RM_AA_ZB_TEX_EDGE2`). This is a render-state predicate, not a foliage, texture, scene, or material-name list: other materials with the same state can also match. In the software triangle rasterizer, a matching texture-edge draw writes passing texels without alpha blending into the framebuffer. Alpha rejection, depth testing, and texture sampling remain separate. The 2D texture-rectangle path does not use this triangle blend setting.

The old `PM64_EDGE_AB` A/B option belonged to the historical experiment described below; the current source does not read it and has no runtime switch for this exact M4.1 predicate. The M4.1 predicate is guarded for the PAL region (`PM64_ROM_REGION_PAL`); the other renderer fixes remain shared. `PM64_EDGEOPAQUE=1` is a separate, broader optional texture-edge setting; setting it to `0` does not disable the matching M4.1 path.

M2.9 is a separate approximation in this same one-cycle edge state: the triangle path uses an alpha rejection threshold of 32 rather than `0x55` when full coverage is selected. Neither change implements full N64 sample coverage or VI anti-aliasing. Some edges can look darker or harder, and isolated texture-edge/coverage artefacts can remain.

The examined upstream main (`9b0da5a1b034d915fb618d6ed6d712d744a63605`) contains newer TEXEL1 UV, cache-dimension, mirror/clamp and depth-compare fixes, but not the same M4.1 predicate or M2.9 threshold. These operate at different state/sampling stages, but static inspection does not establish that their combination is regression-free. The changes have been applied to that upstream basis and checked with focused host tests; their combined behavior still requires hardware checks.

In a Miyoo A/B comparison of build `635A2249`, a narrow blue-green color threshold counted 52 pixels in A and zero in B in the 72×50 region x=92–163, y=56–105. The counts describe all pixels in that region, not a semantic edge mask. The images were well aligned but came from different game frames; animation can explain some changes. B produced darker, sometimes harder edges, and other scenes, transparent objects and motion still need regression testing. A differently scaled and compressed RetroArch/ParaLLEl-RDP image also lacked similarly conspicuous blue fringes; it is a visual reference, not proof of exact RDP or original hardware behavior.

The reduction in colour fringes remains desirable. A more technically grounded coverage and edge treatment needs further investigation and may be proposed separately; no solution or timeline is claimed.
