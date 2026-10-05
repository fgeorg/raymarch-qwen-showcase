#ifdef DIAG
// --- DIAG: floor reflection validation ---------------------------------------
// Isolates the floor's specular math from the SDF machinery and validates it
// against analytic ground truth:
//   - floor hit is analytic: N = (0,1,0) (no sphere tracing, no calcNormal,
//     so the blob cannot participate in the normal)
//   - the image half containing the analytic highlight uses the exact mirror
//     direction (crisp truth); the other half uses the production fuzzReflect
//     BSDF with 48 fixed samples (the smear)
//   - post-tonemap overlays: magenta band = sun direction; yellow segment
//     camera-floor-point -> highlight; green cross = analytic highlight;
//     black divider at image center
// Pixel<->direction projection is the exact pinhole mapping: the pixel whose
// ray direction equals d is (focal*dot(d,rt), focal*dot(d,up)) / dot(d,fw).
vec2 pixUV(vec3 d, vec3 f, vec3 r, vec3 u) {
  float fden = max(dot(d, f), 1e-3);
  return vec2(cam.focal * dot(d, r) / fden, cam.focal * dot(d, u) / fden);
}
float segDist2D(vec2 p, vec2 a, vec2 b) {
  vec2 pa = p - a, ba = b - a;
  float t = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-12), 0.0, 1.0);
  return length(pa - ba * t);
}

void diagMain() {
  ivec2 px = ivec2(gl_GlobalInvocationID.xy);

  vec3 ro = cam.ro, ta = cam.ta;
  vec3 fw = normalize(ta - ro);
  vec3 rt = normalize(cross(fw, vec3(0, 1, 0)));
  vec3 up = cross(rt, fw);
  float res = cam.res;
  int IMW = int(cam.res + 0.5);  // img.c row width; cam.res isn't a constant expr

  vec3 sd = sunDir();
  vec3 mdir = normalize(vec3(sd.x, -sd.y, sd.z));  // primary dir mirroring to sd
  vec3 hp = ro + mdir * (ro.y / (-mdir.y));        // analytic highlight on floor
  vec3 cf = vec3(ro.x, 0.0, ro.z);                 // camera's floor point
  vec2 sunUV = pixUV(sd, fw, rt, up);
  vec2 hpUV  = pixUV(mdir, fw, rt, up);
  vec2 cfUV  = pixUV(normalize(cf - ro), fw, rt, up);
  bool crispPx = hpUV.x > 0.0 ? (int(px.x) >= 512) : (int(px.x) < 512);

  float baseRnd = cam.seed + hash21i(int(px.x), int(px.y)) * 43758.5453;
  const int AAc = (AA_SAMPLES <= 4) ? 2 : 3;
  const int AAr = (AA_SAMPLES + AAc - 1) / AAc;
  vec3 acc = vec3(0.0);
  for (int s = 0; s < AA_SAMPLES; s++) {
    vec2 sub = (vec2(float(s % AAc), float(s / AAc)) + 0.5) / vec2(float(AAc), float(AAr));
    float jx = hash21(baseRnd + float(s) * 0.618,  baseRnd * 1.31 + float(s) * 0.371) - 0.5;
    float jy = hash21(baseRnd * 1.71 + float(s) * 0.537, baseRnd * 2.13 + float(s) * 0.917) - 0.5;
    float pxu = (2.0 * (float(px.x) + sub.x + jx - 0.5) - res) / res;
    float pyu = (res - 2.0 * (float(px.y) + sub.y + jy - 0.5)) / res;
    vec3 rd = normalize(fw * cam.focal + rt * pxu + up * pyu);

    // only rays toward the floor take part; everything else keeps the sky so
    // the sun disc stays visible for the checker
    if (rd.y >= -1e-4) { acc += skyRay(rd); continue; }
    vec3 N = vec3(0.0, 1.0, 0.0);
    vec3 Rm = reflectDir(rd, N);
    if (crispPx) {
      acc += skyRay(Rm);   // exact mirror: analytic ground truth
    } else {
      const int NS = 48;
      vec3 sm = vec3(0.0);
      for (int i = 0; i < NS; i++) {
        float fi = float(i);
        float u1 = (fi + 0.5) / float(NS);
        float u2 = (mod(7.0 * fi + 3.0, float(NS)) + 0.5) / float(NS);
        float u3 = (mod(5.0 * fi + 1.0, float(NS)) + 0.5) / float(NS);
        sm += skyRay(fuzzReflect(Rm, N, getRough(4.0), u1, u2, u3));
      }
      acc += sm * (1.0 / float(NS));
    }
  }

  // same tonemap as main, but NO vignette (keeps pixel<->world undistorted)
  vec3 col = acc * (1.0 / float(AA_SAMPLES)) * 0.96;
  col = vec3(softKneeC(col.x, 2.5), softKneeC(col.y, 2.5), softKneeC(col.z, 2.5));
  col = vec3(acesComp(col.x * 1.02), acesComp(col.y * 1.02), acesComp(col.z * 1.02));
  col = pow(col, vec3(0.92));
  float dith = (hash21i(int(px.x) * 3 + 11, int(px.y) * 7 + 5) - 0.5) * (1.0 / 65535.0);

  // --- post-tonemap overlays (positions in exact pixel-center coords) --------
  float pxu0 = (2.0 * (float(px.x) + 0.5) - res) / res;
  float pyu0 = (res - 2.0 * (float(px.y) + 0.5)) / res;
  vec2 pPix   = vec2(512.0 * (1.0 + pxu0), 512.0 * (1.0 - pyu0));
  vec2 sunPix = vec2(512.0 * (1.0 + sunUV.x), 512.0 * (1.0 - sunUV.y));
  vec2 hpPix  = vec2(512.0 * (1.0 + hpUV.x),  512.0 * (1.0 - hpUV.y));
  vec2 cfPix  = vec2(512.0 * (1.0 + cfUV.x),  512.0 * (1.0 - cfUV.y));

  if (abs(pxu0 - sunUV.x) < 0.005) col = mix(col, vec3(1.0, 0.0, 1.0), 0.6);          // magenta: sun dir
  if (segDist2D(pPix, cfPix, hpPix) < 7.5) col = mix(col, vec3(1.0, 0.85, 0.2), 0.7); // yellow: cam->highlight
  if (abs(pxu0 - hpUV.x) < 0.02 && abs(pyu0 - hpUV.y) < 0.35)                          // green: analytic highlight
    col = mix(col, vec3(0.1, 1.0, 0.25), 0.8);
  else if (abs(pxu0 - hpUV.x) < 0.35 && abs(pyu0 - hpUV.y) < 0.02)
    col = mix(col, vec3(0.1, 1.0, 0.25), 0.8);
  if (abs(pxu0 - cfUV.x) < 0.09 && abs(pyu0 - cfUV.y) < 0.09) col = mix(col, vec3(0.0, 1.0, 1.0), 0.5); // cyan: cam floor pt
  if (abs(pxu0) < 0.003) col = vec3(0.0);                                              // black divider at center

  vec4 outc;
  outc.x = clamp(col.x + dith, 0.0, 1.0);
  outc.y = clamp(col.y + dith, 0.0, 1.0);
  outc.z = clamp(col.z + dith, 0.0, 1.0);
  outc.w = 1.0;
  img.c[px.y * IMW + px.x] = outc;
}
#endif
