# raymarch-qwen-showcase

Headless Vulkan compute path tracer, built via vibecoding with a local Qwen 3.8
(no hand-written core). Scene + shading live in `src/scene.comp`; the host is
`src/vkmain.cpp` (device, dispatch, GPU timing, 16-bit PNG writer, 150-frame pan).

## Commands

    make            # build
    make run        # one 1024^2 frame -> out/render.png (16-bit, ~6.5 MB), GPU-timed
    make preview    # 512px JPEG -> out/preview.jpg; use THIS for visual iteration
    make video      # 150-frame clip -> out/showcase.mp4 (deletes out/frames after encode)
    make frames     # same 150 frames kept in out/frames (regenerable, ~957 MB — never let it accumulate)
    make diag       # floor-reflection validation: split-mirror render + CPU checker (~10 s)

- Iterate with `make preview` (or `make preview IMG=<png>`), never the full-size 16-bit PNGs.
- `make run FZZ=0.5` (crisp) .. `FZZ=2.0` (soft) scales the uniform-fuzz specular radius.
- Quality knobs are #defines: `make GPU_FLAGS="-DAA_SAMPLES=64 -DMAX_STEPS=160 -DMAX_BOUNCES=6"` (defaults: AA 128).
- `tools/*.py` are CPU checkers printing compact PASS/FAIL reports (untracked by design).
- ~2.4 s/frame at 1024^2 (Intel ADL-N); `make video` is ~11 min blocking.

## Environment

- Stock system Mesa crashes in `vkCreateComputePipelines`. The Makefile auto-picks the
  locally built Mesa 26.2 at `/tmp/mesa262` (sets `VK_ICD_FILENAMES`) when present.
- `/tmp` is a small tmpfs — don't stage large files there.

## Rendering notes

- Pure path tracing; the procedural dusk sky is the only light (no NEE, no shadow rays).
  One stratified BSDF sample per hit (Lambert or uniform-fuzz specular + Schlick fresnel);
  sky escape deposits radiance; Russian roulette after bounce 2.
- Normals are central-difference SDF gradients; bounce origins lifted with an SDF-adaptive
  normal offset (a fixed lift overshoots thin features like the bowl rim).
- A scene bounding sphere lets floor-only rays skip the SDF march entirely.
- Tonemap: firefly soft-knee clamp -> ACES -> gamma 0.92 -> vignette; 16-bit, dithered.
- The DIAG pass is `src/diag.inc`, included from scene.comp under `#ifdef DIAG`
  (Makefile passes `-Isrc` to glslangValidator).
