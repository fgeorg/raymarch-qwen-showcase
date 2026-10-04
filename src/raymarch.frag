#version 450

layout(location = 0) out vec4 outColor;

// quality knobs (set at compile time by build.sh)
#define AA_SAMPLES 4
#define MAX_STEPS 72
#define SHADOW_STEPS 20
#define AMB_SAMPLES 4

const vec3 SUN = normalize(vec3(-0.42, 0.26, -0.86));

float hash21(vec2 p) {
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}
vec3 hash33(vec3 p) {
  p = fract(p * vec3(443.897, 441.423, 437.195));
  p += dot(p, p.yxz + 19.19);
  return fract((p.xxy + p.yxx) * p.zyx);
}

mat2 rot(float a) { float c = cos(a), s = sin(a); return mat2(c, -s, s, c); }

// smooth min / soft boolean difference (carve b out of a)
float smin(float a, float b, float k) {
  float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
  return mix(b, a, h) - k * h * (1.0 - h);
}
float sdiff(float a, float b, float k) { return -smin(-a, b, k); }

float sdSphere(vec3 p, float r) { return length(p) - r; }
float sdTorus(vec3 p, vec2 t) { return length(vec2(length(p.xz) - t.x, p.y)) - t.y; }

// scene: (distance, material id)  1=goo 2=cavity 3=gold 4=floor
vec2 map(vec3 p) {
  // torus A: big, gently tilted
  vec3 q = p - vec3(-0.15, 0.98, 0.05);
  q.xz = rot(-0.4) * q.xz;
  q.xy = rot(0.16) * q.xy;
  float dA = sdTorus(q, vec2(1.0, 0.33));

  // torus B: more upright, weaving through A
  q = p - vec3(0.55, 0.78, 0.42);
  q.yz = rot(1.25) * q.yz;
  q.xz = rot(0.55) * q.xz;
  float dB = sdTorus(q, vec2(0.58, 0.26));

  // sphere 1: joins the gooey blend
  float dS1 = sdSphere(p - vec3(-0.85, 0.55, 0.5), 0.48);

  float goo = smin(smin(dA, dB, 0.4), dS1, 0.3);

  // sphere 2: boolean subtraction -> carved hole
  float dCut = sdSphere(p - vec3(0.30, 1.12, -0.12), 0.44);
  float carved = sdiff(goo, dCut, 0.13);

  // sphere 3: independent gold metal, hard union
  float dGold = sdSphere(p - vec3(0.92, 0.33, 0.88), 0.30);

  float id = (goo < 0.0 && dCut < 0.0) ? 2.0 : 1.0;
  vec2 res = vec2(carved, id);
  res = min(res, vec2(dGold, 3.0));
  res = min(res, vec2(p.y, 4.0)); // floor plane
  return res;
}

vec4 trace(vec3 ro, vec3 rd) {
  float t = 0.0, id = -1.0;
  for (int i = 0; i < MAX_STEPS; i++) {
    vec2 h = map(ro + rd * t);
    if (h.x < 0.0015 + 0.0008 * t) { id = h.y; break; }
    t += h.x;
    if (t > 40.0) break;
  }
  if (t > 40.0) id = -1.0;
  return vec4(ro + rd * t, id);
}

vec3 calcNormal(vec3 p) {
  const vec2 e = vec2(1.0, -1.0) * 0.0012;
  return normalize(e.xyy * map(p + e.xyy).x + e.yyx * map(p + e.yyx).x
                 + e.yxy * map(p + e.yxy).x + e.xxx * map(p + e.xxx).x);
}

float softShadow(vec3 ro, vec3 rd) {
  float res = 1.0, t = 0.02;
  for (int i = 0; i < SHADOW_STEPS; i++) {
    float h = map(ro + rd * t).x;
    res = min(res, 7.0 * h / t);
    if (res < 0.004) break;
    t += clamp(h, 0.015, 0.2);
    if (t > 10.0) break;
  }
  return clamp(res, 0.0, 1.0);
}

// Ray Wenderlich style analytic sky: gradient + sun glow + disc
vec3 skyRay(vec3 rd) {
  float y = rd.y;
  vec3 zen = vec3(0.10, 0.28, 0.72);
  vec3 hor = vec3(1.00, 0.60, 0.38);
  vec3 col = mix(zen, hor, pow(clamp(1.0 - max(y, 0.0), 0.0, 1.0), 0.65));
  col = mix(col, vec3(0.10, 0.09, 0.10), smoothstep(0.0, -0.3, y));
  float s = clamp(dot(rd, SUN), 0.0, 1.0);
  col += vec3(1.0, 0.72, 0.45) * 0.14 * pow(s, 2.0);
  col += vec3(1.0, 0.78, 0.55) * 0.40 * pow(s, 10.0);
  col += vec3(1.3, 1.15, 1.0) * 14.0 * pow(s, 350.0);
  return col;
}

vec3 getAlbedo(float id) {
  if (id < 1.5) return vec3(0.12, 0.52, 0.55);
  if (id < 2.5) return vec3(0.05, 0.09, 0.10);
  if (id < 3.5) return vec3(1.00, 0.70, 0.32);
  return vec3(0.30, 0.26, 0.24);
}
float getMetal(float id) { return id > 2.5 ? 1.0 : 0.0; }
float getRough(float id) {
  if (id < 1.5) return 0.18;
  if (id < 2.5) return 0.55;
  if (id < 3.5) return 0.12;
  return 0.38;
}

vec3 shade(vec3 pos, vec3 N, vec3 rd, float id, vec2 rnd) {
  vec3 alb = getAlbedo(id);
  float metal = getMetal(id);
  float rough = getRough(id);

  // raytraced sun light with soft penumbra
  float sh = softShadow(pos + N * 0.004, SUN);
  float ndl = max(dot(N, SUN), 0.0);
  vec3 sunCol = vec3(1.0, 0.82, 0.60) * 3.4;

  // hemisphere ambient: sample the sky
  vec3 amb = vec3(0.0);
  for (int i = 0; i < AMB_SAMPLES; i++) {
    vec3 h = hash33(vec3(rnd * 17.3, float(i)));
    vec3 dir = N + normalize(h * 2.0 - 1.0) * 1.4;
    if (dot(dir, N) < 0.0) dir = N;
    amb += skyRay(normalize(dir));
  }
  amb /= float(AMB_SAMPLES);

  // metallic reflection: fuzzed direction, sample the sky
  vec3 R = reflect(rd, N);
  vec3 h2 = hash33(vec3(rnd * 31.7, 5.2));
  R = normalize(R + (h2 - 0.5) * rough * 1.6);
  vec3 refl = skyRay(R);

  vec3 F0 = mix(vec3(0.045), alb, metal);
  vec3 F = F0 + (1.0 - F0) * pow(1.0 + dot(rd, N), 5.0);

  vec3 dif = alb * (sunCol * sh * ndl * 0.9 + amb * (0.35 + 0.65 * max(N.y, 0.0)));
  vec3 col = dif * (1.0 - F) + refl * F;
  if (metal < 0.5) {
    vec3 Hh = normalize(SUN - rd);
    col += sunCol * sh * pow(max(dot(N, Hh), 0.0), 90.0) * (1.0 - rough) * 0.6;
  }
  return col;
}

vec3 aces(vec3 x) {
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
  const float RES = 1024.0;
  vec2 frag = gl_FragCoord.xy;
  vec2 uv = (2.0 * frag - RES) / RES;

  vec3 ro = vec3(2.9, 1.42, 2.7);
  vec3 ta = vec3(0.02, 0.70, 0.02);
  vec3 fw = normalize(ta - ro);
  vec3 rt = normalize(cross(fw, vec3(0.0, 1.0, 0.0)));
  vec3 up = cross(rt, fw);
  const float focal = 1.6;

  vec3 col = vec3(0.0);
  int n = 0;
  for (int s = 0; s < 4; s++) {
    if (s >= AA_SAMPLES) break;
    vec2 off = (vec2(float(s & 1), float(s >> 1)) + 0.5) / float(AA_SAMPLES);
    vec2 pp = (2.0 * (frag + off - 0.5) - RES) / RES;
    vec3 rd = normalize(fw * focal + rt * pp.x + up * pp.y);
    vec2 rnd = frag + float(s) * 0.37;
    vec4 hit = trace(ro, rd);
    vec3 c;
    if (hit.w < 0.0) {
      c = skyRay(rd);
    } else {
      vec3 N = calcNormal(hit.xyz);
      c = shade(hit.xyz, N, rd, hit.w, rnd);
    }
    col += c;
    n++;
  }
  col /= float(max(n, 1));

  col = aces(col * 1.1);
  col = pow(col, vec3(0.92));
  float vig = 1.0 - 0.30 * dot(uv * 0.6, uv * 0.6);
  col *= vig;
  outColor = vec4(col, 1.0);
}
