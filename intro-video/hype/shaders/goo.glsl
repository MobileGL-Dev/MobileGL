#version 430
#include "common.glsl"
// 2D "glue" metaballs: smooth union of circles and tapered capsules, shaded as a glossy bead.
in vec2 v_uv;
out vec4 o;
uniform vec2 u_res;
uniform vec4 u_balls[64];   // x, y, r, unused
uniform int u_nb;
uniform vec4 u_caps[24];    // ax, ay, bx, by
uniform vec4 u_capr[24];    // r0, r1
uniform int u_nc;
uniform float u_k;          // smooth-union radius (px)
uniform vec4 u_col;         // body colour (linear)
uniform vec4 u_rim;         // outline colour
uniform vec4 u_hi;          // specular colour
uniform float u_outline;    // outline width (px)
uniform float u_bevel;      // bevel width (px)
uniform float u_glow;       // emissive inner glow amount

float smin(float a, float b, float k){
  float h = max(k - abs(a - b), 0.0) / k;
  return min(a, b) - h * h * k * 0.25;
}
float field(vec2 p){
  float d = 1e5;
  for (int i = 0; i < u_nb; i++) d = smin(d, length(p - u_balls[i].xy) - u_balls[i].z, u_k);
  for (int i = 0; i < u_nc; i++){
    vec2 a = u_caps[i].xy, b = u_caps[i].zw;
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-4), 0.0, 1.0);
    d = smin(d, length(pa - ba * h) - mix(u_capr[i].x, u_capr[i].y, h), u_k);
  }
  return d;
}
float height(float d){ float u = 1.0 - clamp(-d / u_bevel, 0.0, 1.0); return sqrt(max(1.0 - u * u, 0.0)); }   // dome
void main(){
  vec2 s = vec2(v_uv.x, 1.0 - v_uv.y) * u_logical;
  vec2 p = uncam(s);
  float d = field(p);
  float px = u_logical.x / u_res.x / u_cam.z;
  if (d > 2.0 * px) { o = vec4(0); return; }
  // wide central differences: smooths the creases a distance field has along its medial axes
  float e = max(1.5, 0.14 * u_bevel);
  float hx = 0.0, hy = 0.0;
  for (int k = 0; k < 2; k++){
    float ee = e * (k == 0 ? 1.0 : 0.5);
    hx += (height(field(p + vec2(ee, 0))) - height(field(p - vec2(ee, 0)))) / ee;
    hy += (height(field(p + vec2(0, ee))) - height(field(p - vec2(0, ee)))) / ee;
  }
  vec3 n = normalize(vec3(-hx * u_bevel * 0.175, -hy * u_bevel * 0.175, 1.0));
  vec3 L = normalize(vec3(-0.55, -0.75, 0.75));
  float dif = clamp(dot(n, L), 0.0, 1.0);
  vec3 H = normalize(L + vec3(0, 0, 1));
  float spec = pow(clamp(dot(n, H), 0.0, 1.0), 70.0);
  vec3 c = u_col.rgb * (0.45 + 0.65 * dif);
  c *= 0.8 + 0.3 * clamp(0.5 - 0.5 * (n.x + n.y) * 1.6, 0.0, 1.0) + 0.0;   // lit from the top-left, thick body
  c += u_col.rgb * u_glow * height(d);
  float rim = 1.0 - smoothstep(u_outline - px, u_outline + px, -d);
  c = mix(c, u_rim.rgb, rim * u_rim.a);
  c += u_hi.rgb * spec * 1.4;
  float a = clamp(0.5 - d / px, 0.0, 1.0) * u_col.a;
  o = vec4(c * a, a) * u_alpha;
}
