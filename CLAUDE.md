# raymarch-qwen-showcase

Headless Vulkan compute path tracer, built via vibecoding with a local Qwen 3.8
(no hand-written core). Scene + shading live in `src/scene.comp`; the host is
`src/vkmain.cpp` (device, dispatch, GPU timing, 16-bit PNG writer, 720-frame clip:
12 s @ 60 fps — 7 s pan + 5 s hold).

## Commands

    make            # build
    make run        # one 1024^2 frame -> out/render.png (16-bit, ~6.5 MB), GPU-timed
    make preview    # 512px JPEG -> out/preview.jpg; use THIS for visual iteration
    make video      # 720-frame clip (12 s @ 60 fps) -> out/showcase.mp4 (deletes out/frames after encode)
    make frames     # same 720 frames kept in out/frames (regenerable, ~4.6 GB — never let it accumulate)
    make diag       # floor-reflection validation: split-mirror render + CPU checker (~10 s)

- Iterate with `make preview` (or `make preview IMG=<png>`), never the full-size 16-bit PNGs.
- `make run FZZ=0.5` (crisp) .. `FZZ=2.0` (soft) scales the uniform-fuzz specular radius.
- Quality knobs are #defines: `make GPU_FLAGS="-DAA_SAMPLES=64 -DMAX_STEPS=160 -DMAX_BOUNCES=6"` (defaults: AA 128).
- Triage targets all render frame 420 and clobber `out/render.png` (restore with
  `make run` afterwards): flatsky, flatfloor, flatboth (flat sky/floor/both),
  hisamp (AA 512), diff, normview, mirrorview, frame420, plus banding triage:
  shadowmap (exact hard shadow from offset origin), shadowdist / shadowdist09 /
  shadowdistfix / shadowdisttrace (min-SDF shadow field, step variants),
  lobeflip (sun-lobe-only estimator). Run ONE target per
  invocation (FLAGS_STAMP pitfall), or build once and run
  `./build/vkmain --frame 420 --out out` directly.
- `tools/*.py` are CPU checkers printing compact PASS/FAIL reports (untracked by design).
- ~4.5 s/frame at 1024^2 (Intel ADL-N); `make video` is ~1 h (720 frames) — poll it, don't block on it.
- Penumbra banding (fixed): floor hits in `trace()` now refine to the exact
  y=0 crossing (the eps-band landing was per-pixel staircase-quantized and
  amplified by `offsetSurf`'s doubling lift); `offsetSurf` is a fixed 4-step
  no-break SDF walk — its old adaptive break was a hard threshold that jumped
  shadow-ray origins. Triage targets: shadowmap / shadowdist* / lobeflip.

## Environment

- Stock system Mesa crashes in `vkCreateComputePipelines`. The Makefile auto-picks the
  locally built Mesa 26.2 at `/tmp/mesa262` (sets `VK_ICD_FILENAMES`) when present.
- `/tmp` is a small tmpfs — don't stage large files there.

## Rendering notes

- Pure path tracing; the procedural dusk sky is the only light (no NEE, no shadow rays).
  One stratified BSDF sample per hit (Lambert or uniform-fuzz specular + Schlick fresnel);
  sky escape deposits radiance; Russian roulette after bounce 2.
- Normals are central-difference SDF gradients; bounce origins lifted with a fixed
  4-step no-break SDF walk along the normal (a hard SDF break there banded the
  penumbra; the fixed lift matches the old overshoot on thin features).
- A scene bounding sphere lets floor-only rays skip the SDF march entirely.
- Tonemap: firefly soft-knee clamp -> ACES -> gamma 0.92 -> vignette; 16-bit, dithered.
- The DIAG pass is `src/diag.inc`, included from scene.comp under `#ifdef DIAG`
  (Makefile passes `-Isrc` to glslangValidator).
