# Raymarch — a Qwen-vibecoded raytracer showcase

A showcase of a raymarcher / raytracer built entirely through vibecoding with a
local **Qwen 3.8** LLM.

The whole point of this project: no hand-written core. Prompts in, working
renderer out. It doubles as a live experiment in how far a local model gets you
on GPU-adjacent, math-heavy code — SDFs, sphere tracing, shading, the works.

## What it does

A headless **Vulkan compute** renderer (`src/scene.comp`) that sphere-traces a
procedural scene and shades it with **pure path tracing** — the procedural
dusk sky (with a soft sun lobe) is the *only* light: no NEE, no shadow rays,
no microfacet fakes.

- **Primary rays** — sphere tracing against the SDF scene, capped by an
  analytic floor plane.
- **At every hit** — one stratified BSDF sample: Lambert diffuse (uniform
  hemisphere), or a *uniform-fuzz* specular — the mirror direction perturbed by
  a uniform disc in the tangent plane (a wendelich-style blur, no GGX),
  weighted by a Schlick fresnel.
- **Path termination** — rays that escape into the sky deposit its radiance;
  paths are additionally pruned by Russian roulette after bounce 2.
- **AA** — 128 stratified subpixel rays with decorrelated jitter.

The scene is a showcase of smooth-minimum and soft-boolean effects: a core
sphere melted with a torus ring and three orbiting satellite spheres (all one
"fused goo"), a bowl soft-carved into the top (smooth difference), a gold
metal sphere sitting in the bowl, and a warm polished floor.

## Getting started

Requirements: Linux, `g++`, `glslangValidator`, `make`, a Vulkan device
(compute-capable; the host prefers a real GPU over llvmpipe), zlib, and
`ffmpeg` for the video.

```sh
make            # build the GPU (Vulkan compute) renderer
make run        # one 1024^2 frame -> out/render.png (16-bit), GPU-timed
make video      # 150-frame clip (10 s @ 15 fps) -> out/frames -> out/showcase.mp4
```

Runtime fuzz: `make run FZZ=0.5` (crisp) .. `FZZ=2.0` (fuzzy). Fuzz scales the
uniform-fuzz specular radius, so reflections sweep from mirror-crisp to a wide
soft smear.

Quality knobs are `#defines` passed via `GPU_FLAGS`, e.g.

```sh
make GPU_FLAGS="-DAA_SAMPLES=64 -DMAX_STEPS=160 -DMAX_BOUNCES=6"
```

Implementation notes: normals are central-difference SDF gradients (6 evals);
bounce origins are lifted off the surface with an SDF-adaptive normal offset
(a fixed lift overshoots thin carved features like the bowl rim); a scene
bounding sphere lets floor-only rays skip the SDF march entirely; the
tonemap chain is a soft-knee firefly clamp → ACES → gamma 0.92 → vignette,
written as 16-bit with dither.

Environment note: the stock system Mesa crashed in `vkCreateComputePipelines`;
a locally built Mesa 26.2 is used instead. The Makefile picks it up
automatically when present at `/tmp/mesa262` (sets `VK_ICD_FILENAMES`).

## Performance

1024² in **~2.4 s** on Intel Graphics (ADL-N) via the local Mesa build —
a single compute dispatch, timed with `VK_QUERY_TYPE_TIMESTAMP` (`frame 0: …ms`).

Speed measures with no quality loss:

- a scene **bounding-sphere early exit**: rays that miss the bound skip the SDF
  march entirely (only the analytic floor plane can still be hit);
- stratified low-discrepancy AA paths, re-rotated per bounce so each path
  explores a different cell of the sample space;
- Russian roulette after bounce 2 kills low-contribution paths early;
- the sky is procedural, so a path's terminus costs one gradient eval —
  the whole scene's lighting is a single `skyRay(rd)`.

## Files

- `src/scene.comp` — the whole renderer (SDF scene, tracing, shading).
- `src/vkmain.cpp` — headless Vulkan host: device selection, compute dispatch,
  GPU timing, 16-bit PNG writer, 150-frame camera pan + dolly.
- `Makefile` — build/run/video targets.

---

Built in conversation with Qwen 3.8, running locally. All bugs are the model's;
all fixes are the model's too.
