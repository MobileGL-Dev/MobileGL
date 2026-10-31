#version 430
#include "common.glsl"
// The MobileGlues mark, alive: a crafting-table / command-block block floating in a blob of glue
// (a glue pool around its lower half, tongues of glue rising behind it, a few loose drops), raymarched
// under an orthographic isometric camera.
in vec2 v_uv;
out vec4 o;
uniform vec2 u_res;
uniform vec4 u_cubes[16];   // screen x, y, size (half-edge px), yaw
uniform vec4 u_cubes2[16];  // squash, wobble, tint mix (0 green .. 1 alt), alpha
uniform vec4 u_cubes3[16];  // seconds since the block hit the glue (big = settled)
uniform int u_n;
uniform float u_time;
uniform vec3 u_body;        // glue colour (linear)
uniform vec3 u_alt;         // alternative block tint
uniform vec3 u_deep;        // deep glue colour
uniform float u_speckle;    // visibility of the items on the block top
uniform float u_pitch;

vec4 C1, C2, C3;
float seed;
float sd_rbox(vec3 p, vec3 b, float r){ vec3 q = abs(p) - b + r; return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0) - r; }
float sd_cap(vec3 p, vec3 a, vec3 b, float r){ vec3 pa = p - a, ba = b - a; float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0); return length(pa - ba * h) - r; }
float smin(float a, float b, float k){ float h = max(k - abs(a - b), 0.0) / k; return min(a, b) - h * h * k * 0.25; }
float smax(float a, float b, float k){ float h = max(k - abs(a - b), 0.0) / k; return max(a, b) + h * h * k * 0.25; }

// C3.x = seconds since the block hit the glue (negative: still falling, large: settled)
float since(){ return C3.x; }
float splash(){ return since() < 0.0 ? 0.0 : exp(-since() * 2.6); }
float bob(){
  float u = since();
  float rest = 0.06 * sin(u_time * 2.2 + seed * 3.0) - C2.x * 0.5;
  if (u < 0.0) return 0.25 + 22.0 * u * u;                          // falling in
  return rest * (1.0 - exp(-u * 2.0)) - 0.42 * exp(-u * 4.5) * cos(u * 11.0);   // plunge, bounce, settle
}

vec3 deform(vec3 p){
  float wob = C2.y;
  return p + wob * 0.04 * vec3(sin(3.1 * p.y + u_time * 9.0), 0.0, sin(2.9 * p.x + u_time * 8.0));
}
float d_block(vec3 p){ return sd_rbox(p - vec3(0.0, bob(), 0.0), vec3(0.78), 0.07); }

// glue level around the block: a wavy line about half-way up its sides
float level(vec3 p){
  float a = atan(p.z, p.x);
  return -0.12 + 0.13 * sin(a * 5.0 + u_time * 1.4 + seed) + 0.07 * sin(a * 9.0 - u_time * 1.1) + 0.25 * splash();
}
float d_glue(vec3 p){
  float sq = C2.x, u = since(), sp = splash();
  // the pool: spreads and ripples outward from the impact
  float r = length(p.xz);
  float ripple = sp * 0.09 * sin(9.0 * r - 16.0 * u) * smoothstep(1.9, 0.6, r);
  float wide = 1.5 * (1.0 + 0.12 * sp * sin(u * 12.0));
  float th = atan(p.z, p.x);
  float R = wide * (1.0 + 0.08 * sin(3.0 * th + seed + u_time * 0.7) + 0.05 * sin(5.0 * th - u_time * 0.9 + seed * 2.0));
  float hh = 0.2 * (1.0 - 0.5 * sq) * (1.0 - 0.35 * sp);
  vec2 dq = vec2(r - (R - 0.3), abs(p.y + 0.78 - ripple) - hh + 0.0);   // a pillow: round in plan, soft rim
  float pool = length(max(dq, 0.0)) + min(max(dq.x, dq.y), 0.0) - 0.3;
  float g = pool;
  // before the hit: a fat, wobbling blob of glue waiting in the middle; the block squashes it flat
  float mound = u < 0.0 ? 1.0 : exp(-u * 10.0);
  if (mound > 0.01) {
    vec3 m = p - vec3(0.0, -0.55, 0.0);
    float w = 1.0 + 0.06 * sin(u_time * 5.0);
    vec3 rad = vec3(1.2 * w, 0.85 * mound / w + 0.01, 1.2 * w);
    float e = (length(m / rad) - 1.0) * min(min(rad.x, rad.y), rad.z);
    g = smin(g, e, 0.3);
  }
  // a coat that hugs the block up to the wavy level (only once the block is in)
  if (u >= 0.0) {
    vec3 pb = p - vec3(0.0, bob(), 0.0);
    float coat = smax(sd_rbox(pb, vec3(0.86), 0.32), pb.y - level(pb), 0.18);
    coat += (1.0 - smoothstep(0.0, 0.12, u)) * 0.6;
    g = smin(g, coat, 0.35);
  }
  // tongues erupt behind the block on impact, overshoot, then sway; their drops are flung up and hover
  float erupt = u < 0.0 ? 0.0 : max(0.0, 1.0 - exp(-u * 5.0) + 0.55 * exp(-u * 3.0) * sin(u * 10.0));
  if (erupt > 0.0) {
    for (int i = 0; i < 5; i++){
      float fi = float(i);
      float ang = 3.93 - C1.w + (fi - 2.0) * 0.48;            // spread around the back (relative to the view)
      vec2 xz = vec2(cos(ang), sin(ang)) * (1.18 + 0.12 * sin(fi * 2.1));
      float lag = clamp(u * 6.0 - fi * 0.35, 0.0, 1.0);
      float top = ((0.55 + 0.45 * fract(sin(fi * 17.3 + seed) * 4375.5)) * (1.0 - 0.6 * sq)
                + 0.08 * sin(u_time * 2.0 + fi * 1.7) + 0.7) * erupt * lag - 0.7;
      float rr = (0.24 + 0.08 * fract(sin(fi * 9.1 + seed) * 1735.3)) * min(1.0, 0.3 + lag);
      g = smin(g, sd_cap(p, vec3(xz.x, -0.7, xz.y), vec3(xz.x, top, xz.y), rr), 0.25);
      float fly = 1.6 * exp(-u * 1.7) * max(sin(u * 4.0), -0.2);
      float dy = top + 0.45 + fly + 0.12 * sin(u_time * 2.6 + fi * 2.3);
      g = min(g, length(p - vec3(xz.x * 1.05, dy, xz.y * 1.05)) - rr * 0.45 * min(1.0, u * 4.0));
    }
  }
  // the splash itself: droplets thrown out radially, falling back
  if (u >= 0.0 && u < 0.9) {
    for (int i = 0; i < 10; i++){
      float fi = float(i);
      float a = fi * 0.628 + 0.3;
      float sp2 = 2.6 + 1.2 * fract(sin(fi * 31.7) * 943.1);
      vec3 c = vec3(cos(a) * (1.0 + sp2 * u), -0.35 + 4.6 * u - 8.5 * u * u, sin(a) * (1.0 + sp2 * u));
      g = min(g, length(p - c) - 0.13 * (1.0 - u / 0.9));
    }
  }
  return g;
}
float map(vec3 p){ p = deform(p); return min(d_block(p), d_glue(p)) * 0.7; }
float map_block(vec3 p){ return d_block(deform(p)) * 0.8; }
float map_glue(vec3 p){ return d_glue(deform(p)) * 0.7; }
vec3 nrm(vec3 p){ vec2 e = vec2(1e-3, 0); return normalize(vec3(map(p + e.xyy) - map(p - e.xyy), map(p + e.yxy) - map(p - e.yxy), map(p + e.yyx) - map(p - e.yyx))); }

float h2(vec2 p){ return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

vec3 block_albedo(vec3 q, vec3 n){
  vec3 base = mix(vec3(0.16, 0.5, 0.1), u_alt, C2.z);
  vec3 an = abs(n);
  vec2 uv; float top = 0.0;
  if (an.y > max(an.x, an.z)) { uv = q.xz; top = 1.0; }
  else if (an.x > an.z) uv = vec2(q.z, q.y); else uv = vec2(q.x, q.y);
  uv /= 0.78;
  vec2 tx = floor((uv * 0.5 + 0.5) * 16.0);            // 16x16 texels
  float grain = 0.92 + 0.16 * h2(tx + top * 31.0);
  vec2 c = (tx + 0.5) / 16.0 * 2.0 - 1.0;
  vec3 col = base * grain;
  if (top > 0.5) {
    // crafting grid: pale panel, dark frame, 3x3 slots, some with items
    if (max(abs(c.x), abs(c.y)) < 0.6) {
      col = vec3(0.47, 0.75, 0.42) * (0.95 + 0.1 * h2(tx));
      vec2 slot = floor((c + 0.6) / 0.4);
      vec2 sc = (slot + 0.5) * 0.4 - 0.6;
      if (max(abs(c.x - sc.x), abs(c.y - sc.y)) > 0.15) col *= 0.82;
      float r = h2(slot + 7.0);
      vec3 item = r < 0.2 ? vec3(0.62, 0.11, 0.08) : r < 0.4 ? vec3(0.21, 0.09, 0.01) : r < 0.55 ? vec3(0.55, 0.3, 0.08)
                : r < 0.7 ? vec3(0.13, 0.09, 0.06) : r < 0.85 ? vec3(0.66, 0.42, 0.25) : vec3(0.1, 0.3, 0.08);
      if (h2(slot + 3.0) < 0.75 && max(abs(c.x - sc.x), abs(c.y - sc.y)) < 0.1) col = mix(col, item, u_speckle);
    } else if (max(abs(c.x), abs(c.y)) < 0.72) col = base * 0.55;
  } else {
    // command-block side: framed square with an up chevron
    float m = max(abs(c.x), abs(c.y + 0.05));
    if (m > 0.5 && m < 0.66) col = base * 0.55;
    else if (m <= 0.5) {
      col = base * 1.1 * grain;
      float chev = abs(abs(c.x) - (0.25 - (c.y + 0.05)));
      if (chev < 0.1 && abs(c.x) < 0.4 && c.y > -0.35 && c.y < 0.3) col = base * 1.8;
    }
  }
  return col;
}

vec4 shade_cube(vec2 s, int i){
  C1 = u_cubes[i]; C2 = u_cubes2[i]; C3 = u_cubes3[i];
  seed = float(i) * 1.37;
  float sz = C1.z;
  const float FIT = 1.4;                                   // the glue blob is wider than the old cube
  vec2 q = (s - C1.xy) / sz * FIT;
  if (dot(q, q) > (C3.x < 0.0 || C3.x < 1.0 ? 30.0 : 9.0)) return vec4(0);
  float yaw = C1.w + 0.7853982, pit = u_pitch;
  vec3 fw = normalize(vec3(-cos(pit) * sin(yaw), -sin(pit), -cos(pit) * cos(yaw)));
  vec3 rt = normalize(cross(fw, vec3(0, 1, 0)));
  vec3 up = cross(rt, fw);
  vec3 ro = -fw * 5.0 + rt * q.x - up * q.y;
  float t = 0.0, d, dmin = 1e5;
  bool hit = false;
  for (int k = 0; k < 180; k++){
    d = map(ro + fw * t);
    dmin = min(dmin, d);
    if (d < 6e-4){ hit = true; break; }
    t += d;
    if (t > 10.0) break;
  }
  float pxw = FIT / sz * u_logical.x / u_res.x;
  float cov = hit ? 1.0 : clamp(1.0 - dmin / pxw, 0.0, 1.0) * step(dmin, pxw);
  if (cov <= 0.0) return vec4(0);
  vec3 p = ro + fw * t;
  vec3 n = nrm(p);
  vec3 qd = deform(p);
  bool glue = hit && d_glue(qd) < d_block(qd);
  vec3 L = normalize(vec3(-0.45, 0.85, 0.35));
  vec3 H = normalize(L - fw);
  vec3 c;
  float alpha = 1.0;
  if (glue) {
    // slime: a glossy skin over translucent green; march through it, see the block inside
    float fres = 0.04 + 0.96 * pow(1.0 - clamp(dot(n, -fw), 0.0, 1.0), 4.0);
    vec3 rd = refract(fw, n, 0.75);
    if (dot(rd, rd) < 0.5) rd = fw;
    vec3 q2 = p - n * 0.01;
    float path = 0.0;
    bool inner = false;
    vec3 pin;
    for (int k = 0; k < 64; k++){
      float db = map_block(q2);
      if (db < 1e-3) { inner = true; pin = q2; break; }
      float dg = map_glue(q2);
      if (dg > 0.004) {      // left the glue: is the block behind this bit of glue?
        float t2 = 0.0;
        for (int j = 0; j < 40; j++){ float d2 = map_block(q2 + rd * t2); if (d2 < 1e-3) { inner = true; pin = q2 + rd * t2; break; } t2 += d2; if (t2 > 4.0) break; }
        break;
      }
      float st = clamp(min(-dg, db), 0.015, 0.2);
      q2 += rd * st;
      path += st;
    }
    vec3 sigma = vec3(2.3, 0.55, 2.7);                 // green passes, red/blue are absorbed
    vec3 T = exp(-sigma * path * 1.2);
    vec3 scatter = u_body * (0.55 + 0.45 * clamp(dot(n, L), 0.0, 1.0));
    vec3 behind = vec3(0.0);
    if (inner) {
      vec3 nb = normalize(vec3(map_block(pin + vec3(1e-3, 0, 0)) - map_block(pin - vec3(1e-3, 0, 0)),
                               map_block(pin + vec3(0, 1e-3, 0)) - map_block(pin - vec3(0, 1e-3, 0)),
                               map_block(pin + vec3(0, 0, 1e-3)) - map_block(pin - vec3(0, 0, 1e-3))));
      vec3 qb = deform(pin) - vec3(0.0, bob(), 0.0);
      behind = block_albedo(qb, nb) * (0.6 + 0.35 * clamp(dot(nb, L), 0.0, 1.0));
    }
    float Tavg = dot(T, vec3(0.3, 0.5, 0.2));
    float body = 1.0 - Tavg;                              // how much the slime itself is seen
    c = behind * T + scatter * body;
    c = mix(c, vec3(0.75, 1.0, 0.65), fres * 0.35);
    c += vec3(1.0) * pow(clamp(dot(n, H), 0.0, 1.0), 120.0) * 1.4;
    c += vec3(0.85, 1.0, 0.75) * pow(clamp(dot(n, H), 0.0, 1.0), 16.0) * 0.12;
    // nothing solid behind: the background shows through (premultiplied, alpha < 1)
    alpha = inner ? 1.0 : clamp(0.35 + 0.65 * body + 0.5 * fres, 0.0, 1.0);
    if (!inner) c = c * 1.0;
  } else {
    float dif = clamp(dot(n, L), 0.0, 1.0);
    vec3 qb = qd - vec3(0.0, bob(), 0.0);
    vec3 an = abs(n);
    vec3 alb = block_albedo(qb, n);
    float face = an.y > max(an.x, an.z) ? 1.25 : (n.x * rt.x + n.z * rt.z < 0.0 ? 1.0 : 0.78);
    c = alb * face * (0.7 + 0.4 * dif);
    vec3 e = abs(qb) - 0.78;
    float edge = smoothstep(-0.06, -0.02, max(min(e.x, e.y), max(min(e.y, e.z), min(e.x, e.z))));
    c = mix(c, vec3(0.3, 0.9, 0.18), edge * 0.7);       // the icon's bright lime edges
  }
  float a = cov * C2.w * alpha;
  return vec4(c * cov * C2.w, a);
}

void main(){
  vec2 s = uncam(vec2(v_uv.x, 1.0 - v_uv.y) * u_logical);
  vec4 acc = vec4(0);
  for (int i = 0; i < u_n; i++){
    vec4 c = shade_cube(s, i);
    acc = c + acc * (1.0 - c.a);
  }
  o = acc * u_alpha;
}
