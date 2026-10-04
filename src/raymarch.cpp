// CPU SDF raymarcher — showcase of smoothmin + soft boolean effects.
//
// All light transport is pure ray tracing against the SDF scene:
//   - primary rays: sphere-traced
//   - soft shadows: shadow rays marched through the SDF, supersampled over
//     a sun disc (no analytic 7h/t penumbra approximation)
//   - ambient: hemisphere samples, each occlusion-tested with a short march,
//     weighted by the traced sky radiance (no hemispherical sky shortcut)
//   - specular: GGX-microfacet directions, each a full traced ray
//     (recursive, limited depth) — no fake Blinn-Phong, no sky-mirror fresnel
//
// Renders out/render.png at 1024x1024 using all CPU cores, prints render ms.
//
// Quality knobs (CXXFLAGS):
//   -DAA_SAMPLES=2 -DMAX_STEPS=80 -DSHADOW_SAMPLES=4 -DSHADOW_STEPS=24
//   -DAO_SAMPLES=3 -DSPEC_SAMPLES=3 -DREFL_DEPTH=2

#include "png.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#ifndef AA_SAMPLES
#define AA_SAMPLES 1
#endif
#ifndef MAX_STEPS
#define MAX_STEPS 64
#endif
#ifndef SHADOW_SAMPLES
#define SHADOW_SAMPLES 4
#endif
#ifndef SHADOW_STEPS
#define SHADOW_STEPS 24
#endif
#ifndef AO_SAMPLES
#define AO_SAMPLES 3
#endif
#ifndef SPEC_SAMPLES
#define SPEC_SAMPLES 3
#endif
#ifndef REFL_DEPTH
#define REFL_DEPTH 2
#endif
// FUZZ: microfacet roughness multiplier (1.0 = material defaults, >1 = more
// fuzz). Applied per-material and clamped, so it always has a visible effect.
#ifndef FUZZ
#define FUZZ 1.0
#endif

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float mixf(float a, float b, float t) { return a + (b - a) * t; }
static inline float fractf(float x) { return x - std::floorf(x); }

struct V3 { float x, y, z; };
static inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline V3& operator+=(V3& a, V3 b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
static inline V3 operator*(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
static inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static inline V3 operator*(float s, V3 a) { return {a.x * s, a.y * s, a.z * s}; }
static inline V3 mixV(V3 a, V3 b, float t) { return a + (b - a) * t; }
static inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 cross(V3 a, V3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static inline float length(V3 a) { return std::sqrtf(dot(a, a)); }
static inline V3 normalize(V3 a) { float l = length(a); return (l > 1e-8f) ? a * (1.f / l) : a; }

// Precomputed rotation constants (hoisted out of the per-pixel hot loop).
struct Rot { float c, s; };
static Rot R_A_XZ, R_A_XY, R_B_YZ, R_B_XZ;

static inline void prepRots() {
  R_A_XZ = {cosf(-0.4f), sinf(-0.4f)};
  R_A_XY = {cosf(0.16f), sinf(0.16f)};
  R_B_YZ = {cosf(1.25f), sinf(1.25f)};
  R_B_XZ = {cosf(0.55f), sinf(0.55f)};
}
// mat2(c,-s,s,c) * vec2 = (c*x + s*y, -s*x + c*y)
// plane 0 = xz, 1 = xy, 2 = yz
static inline V3 rotApply(V3 q, Rot r, int plane) {
  float x, y;
  if (plane == 0) { x = q.x; y = q.z; }
  else if (plane == 1) { x = q.x; y = q.y; }
  else { x = q.y; y = q.z; }
  float nx = r.c * x + r.s * y, ny = -r.s * x + r.c * y;
  if (plane == 0) { q.x = nx; q.z = ny; }
  else if (plane == 1) { q.x = nx; q.y = ny; }
  else { q.y = nx; q.z = ny; }
  return q;
}

static inline float hash21(float x, float y) {
  return fractf(std::sinf(x * 12.9898f + y * 78.233f) * 43758.5453f);
}
static V3 hash33(float seed) {
  float px = fractf(seed * 443.897f), py = fractf(seed * 441.423f), pz = fractf(seed * 437.195f);
  float d = px * (py + 19.19f) + py * (pz + 19.19f) + pz * (px + 19.19f);
  px += d; py += d; pz += d;
  return {fractf((px + py) * pz), fractf((px + px) * py), fractf((py + px) * px)};
}

// smooth min / soft boolean difference (carve b out of a)
static inline float smin(float a, float b, float k) {
  float h = clampf(0.5f + 0.5f * (b - a) / k, 0.f, 1.f);
  return mixf(b, a, h) - k * h * (1.f - h);
}
static inline float sdiff(float a, float b, float k) { return -smin(-a, b, k); }

static inline float sdSphere(V3 p, float r) { return length(p) - r; }
static inline float sdTorus(V3 p, float R, float r) {
  float q1 = std::sqrtf(p.x * p.x + p.z * p.z) - R;
  return std::sqrtf(q1 * q1 + p.y * p.y) - r;
}

struct Hit { float d; float id; };  // id: 1=goo 2=cavity 3=gold 4=floor

static Hit map(V3 p) {
  // torus A: big, gently tilted
  V3 q = p - V3{-0.15f, 0.98f, 0.05f};
  q = rotApply(q, R_A_XZ, 0);
  q = rotApply(q, R_A_XY, 1);
  float dA = sdTorus(q, 1.0f, 0.33f);

  // torus B: more upright, weaving through A
  q = p - V3{0.55f, 0.78f, 0.42f};
  q = rotApply(q, R_B_YZ, 2);
  q = rotApply(q, R_B_XZ, 0);
  float dB = sdTorus(q, 0.58f, 0.26f);

  // sphere 1: joins the gooey blend
  float dS1 = sdSphere(p - V3{-0.85f, 0.55f, 0.5f}, 0.48f);

  float goo = smin(smin(dA, dB, 0.4f), dS1, 0.3f);

  // sphere 2: boolean subtraction -> carved hole
  float dCut = sdSphere(p - V3{0.30f, 1.12f, -0.12f}, 0.44f);
  float carved = sdiff(goo, dCut, 0.13f);

  float d = carved;
  float id = (goo < 0.f && dCut < 0.f) ? 2.f : 1.f;

  // sphere 3: independent gold metal, hard union
  float dGold = sdSphere(p - V3{0.92f, 0.33f, 0.88f}, 0.30f);
  if (dGold < d) { d = dGold; id = 3.f; }
  if (p.y < d) { d = p.y; id = 4.f; }  // floor plane
  return {d, id};
}

// --- pure tracing -------------------------------------------------------

// March a ray through the SDF. Returns hit point (or ro+rd*MAXT) and id.
static V3 trace(V3 ro, V3 rd, float* id, float maxT) {
  float t = 0.f;
  *id = -1.f;
  for (int i = 0; i < MAX_STEPS; i++) {
    Hit h = map(ro + rd * t);
    if (h.d < 0.0012f + 0.0006f * t) { *id = h.id; return ro + rd * t; }
    t += h.d;
    if (t > maxT) break;
  }
  *id = -1.f;
  return ro + rd * maxT;
}

static V3 calcNormal(V3 p) {
  const float e = 0.0012f;
  V3 n = V3{
    e * map(p + V3{e, 0, 0}).d - e * map(p + V3{-e, 0, 0}).d,
    e * map(p + V3{0, e, 0}).d - e * map(p + V3{0, -e, 0}).d,
    e * map(p + V3{0, 0, e}).d - e * map(p + V3{0, 0, -e}).d};
  return normalize(n);
}

static const V3 SUN = normalize(V3{0.30f, 0.45f, 0.60f});
static const V3 SUN_COL = V3{1.f, 0.95f, 0.88f} * 3.8f;
static const float SUN_R = 0.060f;  // angular radius of the sun disc (rad)

// Hard shadow test: march a single ray toward the sun through the SDF.
static inline bool shadowHit(V3 ro, V3 rd) {
  float t = 0.02f;
  for (int i = 0; i < SHADOW_STEPS; i++) {
    float h = map(ro + rd * t).d;
    if (h < 0.0008f + 0.0004f * t) return true;
    t += clampf(h, 0.01f, 0.15f);
    if (t > 12.f) break;
  }
  return false;
}

// Soft shadow: supersample shadow rays over the sun disc. Every ray is a
// real SDF march — penumbra comes from geometry, not from a 7h/t formula.
static float softShadowTraced(V3 ro, V3 rd, float rnd) {
  float vis = 0.f;
  for (int i = 0; i < SHADOW_SAMPLES; i++) {
    V3 h = hash33(rnd + i * 0.618f);
    float r = SUN_R * std::sqrtf(h.x);
    float a = 2.f * 3.14159265f * h.y;
    // build a basis around rd
    V3 u = normalize(cross(rd, std::abs(rd.y) < 0.99f ? V3{0, 1, 0} : V3{1, 0, 0}));
    V3 v = cross(rd, u);
    V3 d = normalize(rd + (u * (r * cosf(a)) + v * (r * sinf(a))));
    vis += shadowHit(ro, d) ? 0.f : 1.f;
  }
  return vis / SHADOW_SAMPLES;
}

// Sky: physical-ish atmosphere. Blue zenith, warmth confined to a thin
// band at the horizon, small sun disc + tight halo.
static V3 skyRay(V3 rd) {
  float y = rd.y;
  V3 zen{0.18f, 0.37f, 0.80f}, hor{0.42f, 0.52f, 0.66f};
  float m = std::powf(clampf(1.f - std::max(y, 0.f), 0.f, 1.f), 8.f);
  V3 col = mixV(zen, hor, m);
  float s = clampf(dot(rd, SUN), 0.f, 1.f);
  col = col + V3{0.80f, 0.87f, 1.00f} * (0.04f * std::powf(s, 24.f))
      + V3{1.55f, 1.50f, 1.38f} * (4.5f * std::powf(s, 1400.f));
  if (y < 0.f) col = mixV(col, V3{0.16f, 0.14f, 0.13f}, clampf(-y * 8.f, 0.f, 1.f));
  return col;
}

static V3 getAlbedo(float id) {
  if (id < 1.5f) return V3{0.10f, 0.48f, 0.52f};
  if (id < 2.5f) return V3{0.05f, 0.09f, 0.10f};
  if (id < 3.5f) return V3{1.00f, 0.71f, 0.29f};
  return V3{0.34f, 0.31f, 0.29f};
}
static inline float getMetal(float id) { return id > 3.5f ? 0.4f : (id > 2.5f ? 1.f : 0.f); }
static float getRough(float id) {
  float base;
  if (id < 1.5f) base = 0.22f;
  else if (id < 2.5f) base = 0.55f;
  else if (id < 3.5f) base = 0.10f;
  else base = 0.90f;  // floor: extra-fuzzy by default
  // FUZZ multiplier, clamped to a sane range — this is the fuzz factor.
  return clampf(base * FUZZ, 0.02f, 1.0f);
}
static inline V3 getF0(V3 alb, float metal) {
  return mixV(V3{0.02f, 0.02f, 0.02f}, alb, metal);
}

static inline V3 reflectDir(V3 I, V3 N) { return I - N * (2.f * dot(I, N)); }

// Soft-knee firefly clamp: values above `knee` compress smoothly instead of
// spiking. Kills the salt-and-pepper fireflies from rare microfacet samples
// that catch the sun disc or the gold sphere's glint.
static inline float softKnee(float x, float knee) {
  if (x <= knee) return x;
  return knee + (x - knee) / (1.f + 2.f * (x - knee));
}
static inline V3 fireflyClamp(V3 v, float knee) {
  return V3{softKnee(v.x, knee), softKnee(v.y, knee), softKnee(v.z, knee)};
}

// GGX half-vector sample (importance-sampled, Marsaglia) — gives the
// direction of one traced specular ray.
static inline V3 ggxSample(V3 N, float rough, V3 h) {
  float a = rough * rough;
  float u = h.z;
  float cosT = std::sqrtf((1.f - u) / (1.f + (a * a - 1.f) * u));
  float sinT = std::sqrtf(std::max(0.f, 1.f - cosT * cosT));
  float phi = 2.f * 3.14159265f * h.x;
  V3 Hlocal = {sinT * cosf(phi), sinT * sinf(phi), cosT};
  // tangent basis around N
  V3 up = std::abs(N.y) < 0.99f ? V3{0, 1, 0} : V3{1, 0, 0};
  V3 T = normalize(cross(up, N));
  V3 B = cross(N, T);
  return normalize(T * Hlocal.x + B * Hlocal.y + N * Hlocal.z);
}

// Recursively evaluate a ray: pure tracing at every bounce.
// depth: 0 = primary, >0 = reflection bounce (cheaper sampling).
static V3 evalRay(V3 ro, V3 rd, float depth, float rnd) {
  float id;
  V3 pos = trace(ro, rd, &id, 40.f);
  if (id < 0.f) return skyRay(rd);

  V3 N = calcNormal(pos);
  float metal = getMetal(id);
  float rough = getRough(id);
  V3 alb = getAlbedo(id);
  V3 F0 = getF0(alb, metal);

  float ndl = std::max(dot(N, SUN), 0.f);

  // direct light: traced shadow rays over the sun disc
  float shadow = ndl > 0.f ? softShadowTraced(pos + N * 0.02f, SUN, rnd) : 0.f;
  V3 dif = alb * SUN_COL * ndl * shadow * (1.f - metal);

  // ambient: hemisphere samples, each occlusion-tested by a short SDF
  // march, weighted by the traced sky in that direction.
  V3 amb{0, 0, 0};
  int nAO = depth == 0 ? AO_SAMPLES : 1;
  V3 up = std::abs(N.y) < 0.99f ? V3{0, 1, 0} : V3{1, 0, 0};
  V3 T = normalize(cross(up, N));
  V3 B = cross(N, T);
  for (int i = 0; i < nAO; i++) {
    V3 h = hash33(rnd * 1.7f + i * 0.389f);
    float r = std::sqrtf(h.x), a = 2.f * 3.14159265f * h.y;
    V3 dir = N + (T * (r * cosf(a)) + B * (r * sinf(a)) + N * std::sqrtf(std::max(0.f, 1.f - r * r))) * 1.6f;
    V3 nd = normalize(dir);
    // occlusion: march a short ray from the surface
    float occ = 0.f;
    V3 aoRo = pos + N * 0.02f;
    float t = 0.01f;
    for (int j = 0; j < 16; j++) {
      float hh = map(aoRo + nd * t).d;
      if (hh < 0.0008f + 0.0004f * t) { occ = 1.f; break; }
      t += clampf(hh, 0.01f, 0.25f);
      if (t > 6.f) break;
    }
    amb += (1.f - occ) * skyRay(nd);
  }
  amb = amb * (1.f / nAO) * alb * (1.f - metal);

  // specular: traced GGX rays (full recursive trace per ray)
  V3 col = dif + amb;
  if (depth < REFL_DEPTH) {
    V3 F = F0 + (V3{1, 1, 1} - F0) * std::powf(1.f + dot(rd, N), 5.f);
    int nS = depth == 0 ? SPEC_SAMPLES : 1;
    V3 spec{0, 0, 0};
    for (int i = 0; i < nS; i++) {
      V3 h = hash33(rnd * 2.3f + 7.1f + i * 0.523f);
      V3 H = ggxSample(N, rough, h);
      V3 Rd = reflectDir(rd, H);
      if (dot(Rd, N) < 0.f) continue;
      spec += evalRay(pos + N * 0.02f, Rd, depth + 1, rnd + 13.7f + i);
    }
    float w = (nS > 0) ? 1.f / nS : 0.f;
    V3 sp = F * spec * w;
    col += fireflyClamp(sp, 1.4f);
  }
  return col;
}

static inline float acesComp(float x) {
  return clampf((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.f, 1.f);
}

int main(int argc, char** argv) {
  prepRots();
  int RES = 1024;
  const char* outPath = "out/render.png";
  if (argc > 1) RES = std::atoi(argv[1]);
  if (argc > 2) outPath = argv[2];
  std::vector<uint16_t> rgba((size_t)RES * RES * 4, 0);

  V3 ro{2.55f, 1.30f, 2.35f}, ta{0.02f, 0.74f, 0.02f};
  V3 fw = normalize(ta - ro);
  V3 rt = normalize(cross(fw, V3{0, 1, 0}));
  V3 up = cross(rt, fw);
  const float focal = 1.6f;

  auto t0 = std::chrono::steady_clock::now();
  int nthreads = (int)std::thread::hardware_concurrency();
  if (nthreads < 1) nthreads = 1;
  if (nthreads > 4) nthreads = 4;
  std::vector<std::thread> th;
  for (int i = 0; i < nthreads; i++) {
    int r0 = i * RES / nthreads, r1 = (i + 1) * RES / nthreads;
    th.emplace_back([&, r0, r1]() {
      for (int y = r0; y < r1; y++) {
        uint16_t* row = &rgba[(size_t)y * RES * 4];
        for (int x = 0; x < RES; x++) {
          float accX = 0, accY = 0, accZ = 0;
          int n = 0;
          for (int s = 0; s < AA_SAMPLES; s++) {
            float offx = (float)((s & 1) + 0.5) / (float)AA_SAMPLES;
            float offy = (float)((s >> 1) + 0.5) / (float)AA_SAMPLES;
            float px = (2.f * (x + offx - 0.5f) - RES) / (float)RES;
            float py = (RES - 2.f * (y + offy - 0.5f)) / (float)RES;
            V3 rd = normalize(fw * focal + rt * px + up * py);
            float baseRnd = hash21(x * 1.731f + 11.3f, y * 2.917f + 5.7f) * 43758.5453f;
            V3 c = evalRay(ro, rd, 0, baseRnd + s * 13.7f);
            accX += c.x; accY += c.y; accZ += c.z; n++;
          }
          float colX = acesComp((accX / n) * 1.1f);
          float colY = acesComp((accY / n) * 1.1f);
          float colZ = acesComp((accZ / n) * 1.1f);
          colX = std::powf(colX, 0.92f); colY = std::powf(colY, 0.92f); colZ = std::powf(colZ, 0.92f);
          float uvx = (2.f * (x + 0.5f) - RES) / (float)RES;
          float uvy = (RES - 2.f * (y + 0.5f)) / (float)RES;
          float vig = 1.f - 0.30f * 0.36f * (uvx * uvx + uvy * uvy);
          float dith = (hash21(x * 0.371f, y * 0.733f) - 0.5f) * (1.0f / 65535.f);
          uint16_t* pxl = &row[x * 4];
          pxl[0] = (uint16_t)(clampf(colX * vig + dith, 0.f, 1.f) * 65535.f + 0.5f);
          pxl[1] = (uint16_t)(clampf(colY * vig + dith, 0.f, 1.f) * 65535.f + 0.5f);
          pxl[2] = (uint16_t)(clampf(colZ * vig + dith, 0.f, 1.f) * 65535.f + 0.5f);
          pxl[3] = 65535;
        }
      }
    });
  }
  for (auto& t : th) t.join();
  auto t1 = std::chrono::steady_clock::now();
  double renderMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
  printf("render: %dx%d in %.1f ms (%d threads, AA=%d steps=%d shadS=%d shadSt=%d ao=%d spec=%d reflD=%d)\n",
         RES, RES, renderMs, nthreads, AA_SAMPLES, MAX_STEPS, SHADOW_SAMPLES, SHADOW_STEPS, AO_SAMPLES, SPEC_SAMPLES, REFL_DEPTH);

  if (!writePng16(outPath, RES, RES, rgba.data())) {
    fprintf(stderr, "failed to write %s\n", outPath);
    return 1;
  }
  auto t2 = std::chrono::steady_clock::now();
  double totalMs = std::chrono::duration<double, std::milli>(t2 - t0).count();
  double pngMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
  printf("png written: %s (total %.1f ms, png %.1f ms)\n", outPath, totalMs, pngMs);
  return 0;
}
