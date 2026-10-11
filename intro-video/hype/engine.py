"""GPU motion-graphics engine for the MobileGL showreel (moderngl, OpenGL 4.3+ core).

Everything is laid out in logical 1920x1080 pixels, y down. A frame is the average of `samples`
sub-frames across a shutter (motion blur), then bloom / aberration / grain / vignette in post.
Set MGL_LIBGL to an opengl32.dll to render through another GL implementation (MobileGL)."""
import math
import os
import functools
from collections import OrderedDict

import numpy as np
import moderngl
import freetype
import uharfbuzz as hb

HERE = os.path.dirname(os.path.abspath(__file__))
LW, LH = 1920, 1080


# --- colour ----------------------------------------------------------------------------------------
def srgb2lin(c):
    c = c / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def hexc(h, k=1.0, a=1.0):
    """'#rrggbb' -> linear (r, g, b, a), rgb scaled by k (k > 1 for emissive glow)."""
    h = h.lstrip('#')
    return (srgb2lin(int(h[0:2], 16)) * k, srgb2lin(int(h[2:4], 16)) * k, srgb2lin(int(h[4:6], 16)) * k, a)


def mix(a, b, u):
    return tuple(x + (y - x) * u for x, y in zip(a, b))


def with_alpha(c, a):
    return (c[0], c[1], c[2], c[3] * a)


def scale_rgb(c, k):
    return (c[0] * k, c[1] * k, c[2] * k, c[3])


# --- fonts -----------------------------------------------------------------------------------------
class Face:
    def __init__(self, path):
        self.path = path
        self.hbface = hb.Face(hb.Blob.from_file_path(path))
        self.hbfont = hb.Font(self.hbface)
        self.upem = self.hbface.upem
        self.ft = freetype.Face(path)
        self.axes = [a.tag for a in self.ft.get_variation_info().axes] if self.ft.has_multiple_masters else []
        self.ft.set_char_size(1000 * 64)
        self.ft.load_char('H', freetype.FT_LOAD_NO_HINTING)
        self.cap = self.ft.glyph.metrics.horiBearingY / 64 / 1000      # cap height / em
        self.ft.load_char('x', freetype.FT_LOAD_NO_HINTING)
        self.xh = self.ft.glyph.metrics.horiBearingY / 64 / 1000

    def _vary(self, var):
        coords = tuple(var.get(a, None) for a in self.axes)
        if self.axes:
            info = {a.tag: a for a in self.ft.get_variation_info().axes}
            full = tuple(v if v is not None else info[a].default for a, v in zip(self.axes, coords))
            self.ft.set_var_design_coords(full)
            self.hbfont.set_variations({a: v for a, v in zip(self.axes, full)})

    def shape(self, s, size, var, tracking=0.0):
        """-> list of (gid, x, y, cluster) pen positions in px, and total advance."""
        self._vary(var)
        buf = hb.Buffer()
        buf.add_str(s)
        buf.guess_segment_properties()
        hb.shape(self.hbfont, buf, {'kern': True, 'liga': True})
        k = size / self.upem
        out = []
        x = 0.0
        for info, pos in zip(buf.glyph_infos, buf.glyph_positions):
            out.append((info.codepoint, x + pos.x_offset * k, -pos.y_offset * k, info.cluster))
            x += pos.x_advance * k + tracking
        return out, x - (tracking if out else 0)

    def raster(self, gid, size, var):
        self._vary(var)
        self.ft.set_char_size(int(round(size * 64)))
        self.ft.load_glyph(gid, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_NO_HINTING)
        g = self.ft.glyph
        b = g.bitmap
        arr = np.array(b.buffer, dtype=np.uint8).reshape(b.rows, b.width) if b.rows else np.zeros((0, 0), np.uint8)
        return arr, g.bitmap_left, g.bitmap_top


FONT_FILES = {
    'archivo': 'Archivo[wdth,wght].ttf',
    'archivo-i': 'Archivo-Italic[wdth,wght].ttf',
    'mono': 'JetBrainsMono[wght].ttf',
}


@functools.lru_cache(None)
def face(name):
    return Face(os.path.join(HERE, 'fonts', FONT_FILES[name]))


SIZE_LADDER = 2 ** (1 / 10)


def quant_size(px):
    """Raster size ladder (~7% steps) so animated sizes reuse textures."""
    k = math.ceil(math.log(max(px, 2)) / math.log(SIZE_LADDER) - 1e-6)
    return SIZE_LADDER ** k


# --- shaders ---------------------------------------------------------------------------------------
def _read(name):
    with open(os.path.join(HERE, 'shaders', name), encoding='utf-8') as f:
        return f.read()


def preprocess(src):
    out = []
    for line in src.splitlines():
        if line.strip().startswith('#include'):
            name = line.split('"')[1]
            out.append(preprocess(COMMON if name == 'common.glsl' else _read(name)))
        else:
            out.append(line)
    return '\n'.join(out)


VS_FULL = '''#version 430
out vec2 v_uv;
void main(){ vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); v_uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0, 1); }
'''

SHAPE_VS = '''#version 430
#include "common.glsl"
in vec2 a_corner;
in vec4 i_rect; in vec4 i_xf; in vec4 i_kind; in vec4 i_p0; in vec4 i_p1; in vec4 i_p2; in vec4 i_p3;
in vec4 i_fill; in vec4 i_stroke; in vec4 i_sw;
out vec2 v_lp;
flat out vec4 v_kind; flat out vec4 v_p0; flat out vec4 v_p1; flat out vec4 v_p2; flat out vec4 v_p3;
flat out vec4 v_fill; flat out vec4 v_stroke; flat out vec4 v_sw;
void main(){
  float pad = i_sw.x + 4.0 * i_sw.y + 2.0;
  vec2 lp = mix(i_rect.xy - pad, i_rect.zw + pad, a_corner);
  float c = cos(i_xf.z), s = sin(i_xf.z);
  vec2 wp = i_xf.xy + i_xf.w * vec2(c * lp.x - s * lp.y, s * lp.x + c * lp.y);
  gl_Position = vec4(to_ndc(cam(wp)), 0, 1);
  v_lp = lp; v_kind = i_kind; v_p0 = i_p0; v_p1 = i_p1; v_p2 = i_p2; v_p3 = i_p3;
  v_fill = i_fill; v_stroke = i_stroke; v_sw = i_sw;
}
'''

SHAPE_FS = '''#version 430
#include "common.glsl"
in vec2 v_lp;
flat in vec4 v_kind; flat in vec4 v_p0; flat in vec4 v_p1; flat in vec4 v_p2; flat in vec4 v_p3;
flat in vec4 v_fill; flat in vec4 v_stroke; flat in vec4 v_sw;
out vec4 o;
vec2 vtx(int i){
  vec4 q = i < 2 ? v_p0 : i < 4 ? v_p1 : i < 6 ? v_p2 : v_p3;
  return (i % 2 == 0) ? q.xy : q.zw;
}
float sd_poly(vec2 p, int n){
  vec2 v0 = vtx(0);
  float d = dot(p - v0, p - v0), sg = 1.0;
  for (int i = 0, j = n - 1; i < n; j = i, i++){
    vec2 vi = vtx(i), vj = vtx(j);
    vec2 e = vj - vi, w = p - vi;
    vec2 b = w - e * clamp(dot(w, e) / dot(e, e), 0.0, 1.0);
    d = min(d, dot(b, b));
    bvec3 cnd = bvec3(p.y >= vi.y, p.y < vj.y, e.x * w.y > e.y * w.x);
    if (all(cnd) || all(not(cnd))) sg *= -1.0;
  }
  return sg * sqrt(d);
}
float sd_arc(vec2 p, float r, float th, float a0, float a1){
  float a = atan(p.y, p.x);
  float mid = 0.5 * (a0 + a1), half_ = 0.5 * (a1 - a0);
  float da = mod(a - mid + PI, 2.0 * PI) - PI;
  if (abs(da) <= half_) return abs(length(p) - r) - th * 0.5;
  vec2 e0 = r * vec2(cos(a0), sin(a0)), e1 = r * vec2(cos(a1), sin(a1));
  return min(length(p - e0), length(p - e1)) - th * 0.5;
}
void main(){
  int kind = int(v_kind.x + 0.5);
  vec2 p = v_lp;
  float d;
  if (kind == 0) d = length(p) - v_p0.x;
  else if (kind == 1) { vec2 q = abs(p) - v_p0.xy + v_p0.z; d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - v_p0.z; }
  else if (kind == 2) { vec2 a = v_p0.xy, b = v_p0.zw; vec2 pa = p - a, ba = b - a;
                        float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
                        d = length(pa - ba * h) - mix(v_p1.x, v_p1.y, h); }
  else if (kind == 5) d = sd_arc(p, v_p0.x, v_p0.y, v_p0.z, v_p0.w);
  else d = sd_poly(p, int(v_kind.y + 0.5)) - v_kind.z;
  float aa = max(length(fwidth(p)) * 0.75, 1e-4);
  vec4 col = vec4(0);
  float af = clamp(0.5 - d / aa, 0.0, 1.0) * v_fill.a;
  col = vec4(v_fill.rgb * af, af);
  if (v_sw.x > 0.0) {
    float as_ = clamp(0.5 - (abs(d) - v_sw.x * 0.5) / aa, 0.0, 1.0) * v_stroke.a;
    col = vec4(v_stroke.rgb * as_, as_) + col * (1.0 - as_);
  }
  if (v_sw.y > 0.0) {             // emissive halo, additive (alpha untouched)
    float q = max(d, 0.0) / v_sw.y;
    float g = exp(-0.5 * q * q) * v_sw.z * (d > 0.0 ? 1.0 : 0.0);
    col.rgb += v_fill.rgb * g;
  }
  o = col * u_alpha;
}
'''

TEX_VS = '''#version 430
#include "common.glsl"
in vec2 a_corner;
uniform vec4 u_rect;      // local quad x0,y0,x1,y1
uniform vec2 u_t;         // translation
uniform vec4 u_m;         // 2x2 matrix (m00, m10, m01, m11)
out vec2 v_uv;
void main(){
  vec2 lp = mix(u_rect.xy, u_rect.zw, a_corner);
  vec2 wp = u_t + vec2(u_m.x * lp.x + u_m.z * lp.y, u_m.y * lp.x + u_m.w * lp.y);
  gl_Position = vec4(to_ndc(cam(wp)), 0, 1);
  v_uv = a_corner;
}
'''

TEX_FS = '''#version 430
#include "common.glsl"
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec4 u_color;
uniform vec4 u_reveal;    // u0, u1, softness, mode (0 = horizontal, 1 = vertical)
uniform int u_mode;       // 0 = mask (R channel) coloured, 1 = premultiplied rgba image
out vec4 o;
void main(){
  vec4 c;
  if (u_mode == 0) { float m = texture(u_tex, v_uv).r; c = vec4(u_color.rgb, 1.0) * m * u_color.a; }
  else c = texture(u_tex, v_uv) * u_color;
  float x = u_reveal.w > 0.5 ? v_uv.y : v_uv.x;
  float sft = max(u_reveal.z, 1e-4);
  float r = smoothstep(u_reveal.x - sft, u_reveal.x, x) * (1.0 - smoothstep(u_reveal.y, u_reveal.y + sft, x));
  o = c * r * u_alpha;
}
'''

COMMON = '''
uniform vec4 u_cam;       // centre x, y, zoom, rotation
uniform vec2 u_shake;
uniform vec2 u_logical;   // 1920, 1080
uniform float u_alpha;
const float PI = 3.14159265358979;
vec2 cam(vec2 p){
  vec2 c = u_logical * 0.5;
  vec2 q = p - u_cam.xy;
  float cs = cos(u_cam.w), sn = sin(u_cam.w);
  q = vec2(cs * q.x - sn * q.y, sn * q.x + cs * q.y) * u_cam.z;
  return q + c + u_shake;
}
vec2 uncam(vec2 s){
  vec2 c = u_logical * 0.5;
  vec2 q = (s - c - u_shake) / u_cam.z;
  float cs = cos(-u_cam.w), sn = sin(-u_cam.w);
  return vec2(cs * q.x - sn * q.y, sn * q.x + cs * q.y) + u_cam.xy;
}
vec2 to_ndc(vec2 p){ return vec2(p.x / u_logical.x * 2.0 - 1.0, 1.0 - p.y / u_logical.y * 2.0); }
'''


def _mat(xf, skew):
    """xf = (tx, ty, rot, scale) or (tx, ty, (m00, m01, m10, m11)); skew = (x shear, y scale)."""
    if len(xf) == 3:
        m00, m01, m10, m11 = xf[2]
    else:
        c, s_ = math.cos(xf[2]) * xf[3], math.sin(xf[2]) * xf[3]
        m00, m01, m10, m11 = c, -s_, s_, c
    sh, sy = skew
    # M * [[1, sh], [0, sy]]
    a, b, c2, d = m00, m00 * sh + m01 * sy, m10, m10 * sh + m11 * sy
    return (a, c2, b, d)


class Engine:
    def __init__(self, scale=1.0, samples=1, shutter=0.5, fps=60, libgl=None):
        libgl = libgl or os.environ.get('MGL_LIBGL') or 'opengl32.dll'
        self.ctx = moderngl.create_context(standalone=True, require=430, libgl=libgl)
        self.renderer = self.ctx.info['GL_RENDERER']
        self.scale = scale
        self.W, self.H = int(LW * scale), int(LH * scale)
        self.samples, self.shutter, self.fps = samples, shutter, fps
        ctx = self.ctx
        self.progs = {}
        self.p_shape = self.program(SHAPE_VS, SHAPE_FS)
        self.p_tex = self.program(TEX_VS, TEX_FS)
        self.quad = ctx.buffer(np.array([0, 0, 1, 0, 0, 1, 1, 1], dtype='f4').tobytes())
        self.inst = ctx.buffer(reserve=4096 * 40 * 4, dynamic=True)
        self.vao_shape = ctx.vertex_array(self.p_shape, [
            (self.quad, '2f', 'a_corner'),
            (self.inst, '4f 4f 4f 4f 4f 4f 4f 4f 4f 4f/i',
             'i_rect', 'i_xf', 'i_kind', 'i_p0', 'i_p1', 'i_p2', 'i_p3', 'i_fill', 'i_stroke', 'i_sw')])
        self.vao_tex = ctx.vertex_array(self.p_tex, [(self.quad, '2f', 'a_corner')])
        self.empty_vao = {}
        # targets
        self.t_scene = ctx.texture((self.W, self.H), 4, dtype='f2')
        self.f_scene = ctx.framebuffer([self.t_scene])
        self.t_acc = ctx.texture((self.W, self.H), 4, dtype='f4')
        self.f_acc = ctx.framebuffer([self.t_acc])
        self.t_out = ctx.texture((self.W, self.H), 4, dtype='f1')
        self.f_out = ctx.framebuffer([self.t_out])
        self.bloom = []
        w, h = self.W, self.H
        for i in range(7):
            w, h = max(w // 2, 1), max(h // 2, 1)
            t = ctx.texture((w, h), 4, dtype='f2')
            t.repeat_x = t.repeat_y = False
            self.bloom.append((t, ctx.framebuffer([t])))
        for t in (self.t_scene, self.t_acc):
            t.repeat_x = t.repeat_y = False
        self.p_acc = self.program(VS_FULL, ACC_FS)
        self.p_bright = self.program(VS_FULL, BRIGHT_FS)
        self.p_down = self.program(VS_FULL, DOWN_FS)
        self.p_up = self.program(VS_FULL, UP_FS)
        self.p_final = self.program(VS_FULL, FINAL_FS)
        self.text_cache = OrderedDict()
        self.img_cache = {}
        self.rt_cache = {}
        self.batch = []
        self.cam = (LW / 2, LH / 2, 1.0, 0.0)
        self.shake = (0.0, 0.0)
        self.alpha = 1.0
        self.logical = (LW, LH)
        self.target_stack = []

    # ---- programs
    def program(self, vs, fs):
        key = (vs, fs)
        if key not in self.progs:
            self.progs[key] = self.ctx.program(vertex_shader=preprocess(vs), fragment_shader=preprocess(fs))
        return self.progs[key]

    def fs_program(self, src):
        return self.program(VS_FULL, src)

    def _set(self, prog, **u):
        for k, v in u.items():
            if k in prog:
                prog[k].value = v

    def _common(self, prog):
        self._set(prog, u_cam=self.cam, u_shake=self.shake, u_logical=self.logical, u_alpha=self.alpha)

    # ---- render targets (offscreen layers in logical space)
    def layer(self, name, w=LW, h=LH):
        key = (name, w, h)
        if key not in self.rt_cache:
            t = self.ctx.texture((int(w * self.scale), int(h * self.scale)), 4, dtype='f2')
            t.build_mipmaps()
            t.filter = (moderngl.LINEAR_MIPMAP_LINEAR, moderngl.LINEAR)
            self.rt_cache[key] = (t, self.ctx.framebuffer([t]))
        return self.rt_cache[key]

    def push_target(self, name, w=LW, h=LH, clear=(0, 0, 0, 0)):
        """Draw into an offscreen layer of logical size w x h until pop_target(); returns its texture."""
        self.flush()
        t, fb = self.layer(name, w, h)
        self.target_stack.append((fb, t, self.cam, self.shake, self.logical, self.alpha))
        fb.use()
        fb.clear(*clear)
        self.logical = (w, h)
        self.cam = (w / 2, h / 2, 1.0, 0.0)
        self.shake = (0.0, 0.0)
        self.alpha = 1.0
        return t

    def pop_target(self):
        self.flush()
        fb, t, self.cam, self.shake, self.logical, self.alpha = self.target_stack.pop()
        t.build_mipmaps()
        prev = self.target_stack[-1][0] if self.target_stack else self.f_scene
        prev.use()
        return t

    # ---- shapes
    def _shape(self, kind, rect, xf, p, fill, stroke=(0, 0, 0, 0), sw=0.0, glow=0.0, glow_k=0.0, extra=(0, 0)):
        pp = list(p) + [0.0] * (16 - len(p))
        self.batch.append(('s', (rect, xf, (kind, extra[0], extra[1], 0), pp, fill, stroke, (sw, glow, glow_k, 0))))

    def circle(self, x, y, r, fill, stroke=(0, 0, 0, 0), sw=0.0, glow=0.0, glow_k=0.0):
        self._shape(0, (-r, -r, r, r), (x, y, 0, 1), (r,), fill, stroke, sw, glow, glow_k)

    def rect(self, x, y, w, h, fill, r=0.0, rot=0.0, stroke=(0, 0, 0, 0), sw=0.0, glow=0.0, glow_k=0.0, s=1.0):
        """Centred rectangle (x, y = centre) with corner radius r."""
        hw, hh = abs(w) / 2, abs(h) / 2
        r = min(r, hw, hh)
        self._shape(1, (-hw, -hh, hw, hh), (x, y, rot, s), (hw, hh, r), fill, stroke, sw, glow, glow_k)

    def line(self, x0, y0, x1, y1, width, fill, w1=None, glow=0.0, glow_k=0.0):
        r0 = width / 2
        r1 = (w1 if w1 is not None else width) / 2
        rr = max(r0, r1)
        self._shape(2, (min(x0, x1) - rr, min(y0, y1) - rr, max(x0, x1) + rr, max(y0, y1) + rr), (0, 0, 0, 1),
                    (x0, y0, x1, y1, r0, r1), fill, glow=glow, glow_k=glow_k)

    def poly(self, pts, fill, x=0.0, y=0.0, rot=0.0, s=1.0, stroke=(0, 0, 0, 0), sw=0.0, round_=0.0, glow=0.0,
             glow_k=0.0):
        assert 3 <= len(pts) <= 8
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        flat = [c for p in pts for c in p]
        self._shape(4, (min(xs) - round_, min(ys) - round_, max(xs) + round_, max(ys) + round_), (x, y, rot, s),
                    flat, fill, stroke, sw, glow, glow_k, extra=(len(pts), round_))

    def arc(self, x, y, r, th, a0, a1, fill, glow=0.0, glow_k=0.0):
        """Ring segment from angle a0 to a1 (radians, y down -> clockwise)."""
        if a1 - a0 >= 2 * math.pi - 1e-4:
            a0, a1 = -math.pi - 0.01, math.pi + 0.01
        R = r + th
        self._shape(5, (-R, -R, R, R), (x, y, 0, 1), (r, th, a0, a1), fill, glow=glow, glow_k=glow_k)

    def ring(self, x, y, r, th, fill, glow=0.0, glow_k=0.0):
        self.arc(x, y, r, th, -math.pi, math.pi, fill, glow, glow_k)

    def polyline(self, pts, width, fill, glow=0.0, glow_k=0.0):
        for a, b in zip(pts, pts[1:]):
            self.line(a[0], a[1], b[0], b[1], width, fill, glow=glow, glow_k=glow_k)

    # ---- text
    def _text_tex(self, s, font, size, var, tracking):
        fsize = quant_size(size * self.scale)
        k = fsize / (size * self.scale)
        var = {a: round(v / 2) * 2 for a, v in var.items()}
        key = (s, font, round(fsize, 3), tuple(sorted(var.items())), round(tracking * self.scale * k, 2))
        e = self.text_cache.get(key)
        if e is not None:
            self.text_cache.move_to_end(key)
            return e, fsize
        F = face(font)
        glyphs, adv = F.shape(s, fsize, var, tracking * self.scale * k)
        pad = int(fsize * 0.05) + 4
        asc = int(math.ceil(fsize * 1.05))
        desc = int(math.ceil(fsize * 0.35))
        wpx = int(math.ceil(adv)) + 2 * pad + int(fsize * 0.3)
        hpx = asc + desc + 2 * pad
        canvas = np.zeros((hpx, wpx), np.uint8)
        ox, oy = pad + int(fsize * 0.15), pad + asc
        for gid, gx, gy, _ in glyphs:
            bm, left, top = F.raster(gid, fsize, var)
            if bm.size == 0:
                continue
            x0 = int(round(ox + gx)) + left
            y0 = int(round(oy + gy)) - top
            h, w = bm.shape
            xa, ya = max(x0, 0), max(y0, 0)
            xb, yb = min(x0 + w, wpx), min(y0 + h, hpx)
            if xb <= xa or yb <= ya:
                continue
            sub = canvas[ya:yb, xa:xb]
            np.maximum(sub, bm[ya - y0:yb - y0, xa - x0:xb - x0], out=sub)
        tex = self.ctx.texture((wpx, hpx), 1, canvas.tobytes())
        tex.build_mipmaps()
        tex.filter = (moderngl.LINEAR_MIPMAP_LINEAR, moderngl.LINEAR)
        tex.repeat_x = tex.repeat_y = False
        e = (tex, wpx, hpx, ox, oy, adv, F.cap * fsize)
        self.text_cache[key] = e
        if len(self.text_cache) > 900:
            _, old = self.text_cache.popitem(last=False)
            old[0].release()
        return e, fsize

    def measure(self, s, font='archivo', size=100, wght=700, wdth=100, tracking=0.0):
        F = face(font)
        var = {'wght': wght, 'wdth': wdth} if font != 'mono' else {'wght': wght}
        return F.shape(s, size, var, tracking)[1]

    def text(self, s, x, y, size=100, color=(1, 1, 1, 1), font='archivo', wght=700, wdth=100, align='center',
             valign='cap', rot=0.0, s_=1.0, tracking=0.0, reveal=(0.0, 1.0, 0.0, 0.0), skew=0.0, sy=1.0, mat=None):
        """Draw a line of text. (x, y) is the anchor: align = left/center/right on the advance box; valign =
        'cap' (centre of cap height), 'base' (baseline), 'top' (cap top). Returns the advance width."""
        if not s:
            return 0.0
        var = {'wght': wght, 'wdth': wdth} if font != 'mono' else {'wght': wght}
        (tex, wpx, hpx, ox, oy, adv, cap), fsize = self._text_tex(s, font, size, var, tracking)
        k = size / (fsize / self.scale) / self.scale          # texture px -> logical px
        adv_l = adv * k
        ax = {'left': 0.0, 'center': adv_l / 2, 'right': adv_l}[align]
        ay = {'base': 0.0, 'cap': -cap * k / 2, 'top': -cap * k}[valign]
        x0 = -ox * k - ax
        y0 = -oy * k - ay
        xf = (x, y, mat) if mat is not None else (x, y, rot, s_)
        self.batch.append(('t', (tex, (x0, y0, x0 + wpx * k, y0 + hpx * k), xf, color, reveal, (skew, sy), 0)))
        return adv_l

    def image(self, tex, x, y, w, h, rot=0.0, color=(1, 1, 1, 1), s=1.0, reveal=(0.0, 1.0, 0.0, 0.0), mat=None):
        """Premultiplied RGBA texture (e.g. a pushed layer, which is stored bottom-up) centred at x, y."""
        xf = (x, y, mat) if mat is not None else (x, y, rot, s)
        self.batch.append(('t', (tex, (-w / 2, h / 2, w / 2, -h / 2), xf, color, reveal, (0, 1), 1)))

    def glyph_runs(self, s, font='archivo', size=100, wght=700, wdth=100, tracking=0.0):
        """Per-character x offsets (logical px, pen start = 0) and total advance, for per-letter animation."""
        F = face(font)
        var = {'wght': wght, 'wdth': wdth} if font != 'mono' else {'wght': wght}
        glyphs, adv = F.shape(s, size, var, tracking)
        xs = [0.0] * len(s)
        for gid, gx, gy, cl in glyphs:
            xs[cl] = gx
        ends = []
        for i in range(len(s)):
            ends.append(xs[i + 1] if i + 1 < len(s) else adv)
        return xs, ends, adv

    # ---- fullscreen shader layer
    def fullscreen(self, prog, **uniforms):
        self.flush()
        self._common(prog)
        self._set(prog, u_res=(self.W, self.H), **uniforms)
        vao = self.empty_vao.get(id(prog))
        if vao is None:
            vao = self.empty_vao[id(prog)] = self.ctx.vertex_array(prog, [])
        vao.render(moderngl.TRIANGLES, vertices=3)

    def bind_textures(self, prog, **tex):
        for i, (name, t) in enumerate(tex.items()):
            t.use(location=i + 1)
            if name in prog:
                prog[name].value = i + 1

    # ---- batching
    def flush(self):
        if not self.batch:
            return
        ctx = self.ctx
        ctx.enable(moderngl.BLEND)
        ctx.blend_func = (moderngl.ONE, moderngl.ONE_MINUS_SRC_ALPHA)
        i = 0
        while i < len(self.batch):
            kind = self.batch[i][0]
            j = i
            while j < len(self.batch) and self.batch[j][0] == kind and (kind == 's' or j == i):
                j += 1
            if kind == 's':
                rows = []
                for _, (rect, xf, kd, pp, fill, stroke, sw) in self.batch[i:j]:
                    rows.append(list(rect) + list(xf) + list(kd) + pp + list(fill) + list(stroke) + list(sw))
                data = np.array(rows, dtype='f4')
                if data.nbytes > self.inst.size:
                    self.inst.orphan(data.nbytes * 2)
                self.inst.write(data.tobytes())
                self._common(self.p_shape)
                self.vao_shape.render(moderngl.TRIANGLE_STRIP, vertices=4, instances=len(rows))
            else:
                tex, rect, xf, color, reveal, skew, mode = self.batch[i][1]
                p = self.p_tex
                self._common(p)
                tex.use(location=0)
                p['u_tex'].value = 0
                p['u_rect'].value = rect
                p['u_t'].value = xf[:2]
                p['u_m'].value = _mat(xf, skew)
                p['u_color'].value = color
                p['u_reveal'].value = reveal
                p['u_mode'].value = mode
                self.vao_tex.render(moderngl.TRIANGLE_STRIP, vertices=4)
            i = j
        self.batch = []

    # ---- frame
    def begin_sub(self, clear):
        self.f_scene.use()
        self.f_scene.clear(*clear)
        self.logical = (LW, LH)
        self.cam = (LW / 2, LH / 2, 1.0, 0.0)
        self.shake = (0.0, 0.0)
        self.alpha = 1.0

    def accumulate(self, weight, first):
        self.flush()
        ctx = self.ctx
        self.f_acc.use()
        if first:
            self.f_acc.clear(0, 0, 0, 0)
        ctx.enable(moderngl.BLEND)
        ctx.blend_func = (moderngl.ONE, moderngl.ONE)
        self.t_scene.use(location=0)
        self.p_acc['u_src'].value = 0
        self.p_acc['u_w'].value = weight
        self._fs(self.p_acc)

    def _fs(self, prog):
        vao = self.empty_vao.get(id(prog))
        if vao is None:
            vao = self.empty_vao[id(prog)] = self.ctx.vertex_array(prog, [])
        vao.render(moderngl.TRIANGLES, vertices=3)

    def post(self, P, frame_idx):
        ctx = self.ctx
        ctx.disable(moderngl.BLEND)
        # bloom
        t0, f0 = self.bloom[0]
        f0.use()
        self.t_acc.use(location=0)
        self.p_bright['u_src'].value = 0
        self.p_bright['u_thresh'].value = P.get('bloom_thresh', 0.9)
        self.p_bright['u_texel'].value = (1 / self.W, 1 / self.H)
        self._fs(self.p_bright)
        for i in range(1, len(self.bloom)):
            src, _ = self.bloom[i - 1]
            dst, fb = self.bloom[i]
            fb.use()
            src.use(location=0)
            self.p_down['u_src'].value = 0
            self.p_down['u_texel'].value = (1 / src.width, 1 / src.height)
            self._fs(self.p_down)
        ctx.enable(moderngl.BLEND)
        ctx.blend_func = (moderngl.ONE, moderngl.ONE)
        for i in range(len(self.bloom) - 1, 0, -1):
            src, _ = self.bloom[i]
            dst, fb = self.bloom[i - 1]
            fb.use()
            src.use(location=0)
            self.p_up['u_src'].value = 0
            self.p_up['u_texel'].value = (1 / src.width, 1 / src.height)
            self._fs(self.p_up)
        ctx.disable(moderngl.BLEND)
        self.f_out.use()
        p = self.p_final
        self.t_acc.use(location=0)
        self.bloom[0][0].use(location=1)
        p['u_src'].value = 0
        p['u_bloom'].value = 1
        self._set(p, u_res=(self.W, self.H), u_scale=self.scale,
                  u_bloom_k=P.get('bloom', 0.6), u_ca=P.get('ca', 0.0), u_vig=P.get('vignette', 0.35),
                  u_grain=P.get('grain', 0.045) * float(os.environ.get('GRAIN_K', '0.6')), u_flash=P.get('flash', (0, 0, 0, 0)), u_invert=P.get('invert', 0.0),
                  u_frame=float(frame_idx), u_exposure=P.get('exposure', 1.0), u_fade=P.get('fade', 0.0),
                  u_scan=P.get('scan', 0.0), u_grain_rate=float(os.environ.get('GRAIN_RATE', '0')))
        self._fs(p)

    def read(self):
        return self.t_out.read()


ACC_FS = '''#version 430
in vec2 v_uv; uniform sampler2D u_src; uniform float u_w; out vec4 o;
void main(){ o = texture(u_src, v_uv) * u_w; }
'''

BRIGHT_FS = '''#version 430
in vec2 v_uv; uniform sampler2D u_src; uniform float u_thresh; uniform vec2 u_texel; out vec4 o;
void main(){
  vec3 c = vec3(0);
  for (int i = 0; i < 4; i++){ vec2 d = vec2((i & 1) == 0 ? -0.5 : 0.5, (i & 2) == 0 ? -0.5 : 0.5) * u_texel;
    c += texture(u_src, v_uv + d).rgb; }
  c *= 0.25;
  float l = max(c.r, max(c.g, c.b));
  float knee = 0.25;
  float s = clamp(l - u_thresh + knee, 0.0, 2.0 * knee);
  s = s * s / (4.0 * knee + 1e-4);
  float w = max(s, l - u_thresh) / max(l, 1e-4);
  o = vec4(min(c * w, vec3(40.0)), 1);
}
'''

DOWN_FS = '''#version 430
in vec2 v_uv; uniform sampler2D u_src; uniform vec2 u_texel; out vec4 o;
void main(){
  vec2 t = u_texel;
  vec3 a = texture(u_src, v_uv + t * vec2(-2, -2)).rgb, b = texture(u_src, v_uv + t * vec2(0, -2)).rgb,
       c = texture(u_src, v_uv + t * vec2(2, -2)).rgb, d = texture(u_src, v_uv + t * vec2(-2, 0)).rgb,
       e = texture(u_src, v_uv).rgb, f = texture(u_src, v_uv + t * vec2(2, 0)).rgb,
       g = texture(u_src, v_uv + t * vec2(-2, 2)).rgb, h = texture(u_src, v_uv + t * vec2(0, 2)).rgb,
       i = texture(u_src, v_uv + t * vec2(2, 2)).rgb, j = texture(u_src, v_uv + t * vec2(-1, -1)).rgb,
       k = texture(u_src, v_uv + t * vec2(1, -1)).rgb, l = texture(u_src, v_uv + t * vec2(-1, 1)).rgb,
       m = texture(u_src, v_uv + t * vec2(1, 1)).rgb;
  vec3 r = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
  o = vec4(r, 1);
}
'''

UP_FS = '''#version 430
in vec2 v_uv; uniform sampler2D u_src; uniform vec2 u_texel; out vec4 o;
void main(){
  vec2 t = u_texel;
  vec3 r = texture(u_src, v_uv).rgb * 4.0;
  r += (texture(u_src, v_uv + vec2(-t.x, 0)).rgb + texture(u_src, v_uv + vec2(t.x, 0)).rgb +
        texture(u_src, v_uv + vec2(0, -t.y)).rgb + texture(u_src, v_uv + vec2(0, t.y)).rgb) * 2.0;
  r += texture(u_src, v_uv + vec2(-t.x, -t.y)).rgb + texture(u_src, v_uv + vec2(t.x, -t.y)).rgb +
       texture(u_src, v_uv + vec2(-t.x, t.y)).rgb + texture(u_src, v_uv + vec2(t.x, t.y)).rgb;
  o = vec4(r / 16.0 * 0.85, 1);
}
'''

FINAL_FS = '''#version 430
in vec2 v_uv; uniform sampler2D u_src; uniform sampler2D u_bloom; uniform vec2 u_res; uniform float u_scale;
uniform float u_grain_rate, u_bloom_k, u_ca, u_vig, u_grain, u_invert, u_frame, u_exposure, u_fade, u_scan; uniform vec4 u_flash;
out vec4 o;
uint pcg(uint v){ uint s = v * 747796405u + 2891336453u; uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
                  return (w >> 22u) ^ w; }
float hash(vec3 p){   // integer hash: bit-identical on every GL implementation
  uvec3 q = uvec3(ivec3(p) + 65536);
  return float(pcg(q.x + pcg(q.y + pcg(q.z))) & 0xffffffu) / 16777216.0; }
vec3 tonemap(vec3 c){
  // linear below 0.8, smooth shoulder to 1
  vec3 k = max(c - 0.8, 0.0);
  return min(c, 0.8) + 0.2 * (1.0 - exp(-k / 0.2));
}
void main(){
  vec2 uv = vec2(v_uv.x, 1.0 - v_uv.y);           // flip so the readback is top row first
  vec2 d = uv - 0.5;
  vec3 c;
  if (u_ca > 0.0) {
    vec2 off = d * u_ca * 0.012;
    c = vec3(texture(u_src, uv + off).r, texture(u_src, uv).g, texture(u_src, uv - off).b);
  } else c = texture(u_src, uv).rgb;
  c += texture(u_bloom, uv).rgb * u_bloom_k;
  c *= u_exposure;
  c = tonemap(c);
  c = mix(c, vec3(1.0) - c, u_invert);
  c = mix(c, u_flash.rgb, u_flash.a);
  float v = 1.0 - u_vig * pow(length(d * vec2(1.0, 0.75)) * 1.25, 2.4);
  c *= clamp(v, 0.0, 1.0);
  if (u_scan > 0.0) c *= 1.0 - u_scan * (0.5 + 0.5 * sin(uv.y * u_res.y * 3.14159 / max(u_scale * 2.0, 1.0)));
  c *= 1.0 - u_fade;
  vec2 px = floor(vec2(gl_FragCoord.x, u_res.y - gl_FragCoord.y) / max(u_scale, 0.5));
  float gf = u_grain_rate > 0.0 ? floor(u_frame / u_grain_rate) : 0.0;   // 0 = a fixed print texture
  float g = hash(vec3(px, mod(gf, 97.0))) + hash(vec3(px + 17.0, mod(gf, 89.0))) - 1.0;
  float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
  c += g * u_grain * (0.35 + 0.65 * (1.0 - lum)) * (0.6 + 0.4 * lum);
  c = clamp(c, 0.0, 1.0);
  vec3 s = mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
  s += (hash(vec3(px, mod(gf, 61.0) + 3.0)) - 0.5) / 255.0;
  o = vec4(s, 1);
}
'''
