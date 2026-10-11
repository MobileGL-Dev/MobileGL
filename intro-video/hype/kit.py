"""Reusable visual pieces: backgrounds, HUD, glue blobs, jelly cubes, isometric boxes, code panels, slams."""
import math
from anim import *
from engine import LW, LH, _read, hexc

COS30, SIN30 = math.cos(math.pi / 6), 0.5


# --- background ------------------------------------------------------------------------------------
def bg(E, c):
    E.rect(LW / 2, LH / 2, LW * 4, LH * 4, c)


def dot_grid(E, f, c, step=60, r=2.2, drift=(0.0, 0.0), pulse=0.0):
    ox = (drift[0] * f.lt) % step
    oy = (drift[1] * f.lt) % step
    rr = r * (1 + pulse * f.kick)
    y = -step + oy
    while y < LH + step:
        x = -step + ox
        while x < LW + step:
            E.circle(x, y, rr, c)
            x += step
        y += step


def line_grid(E, f, c, step=120, w=1.5, drift=(0.0, 0.0)):
    ox = (drift[0] * f.lt) % step
    oy = (drift[1] * f.lt) % step
    x = -step + ox
    while x < LW + step:
        E.line(x, -10, x, LH + 10, w, c)
        x += step
    y = -step + oy
    while y < LH + step:
        E.line(-10, y, LW + 10, y, w, c)
        y += step


# --- HUD -------------------------------------------------------------------------------------------
def hud(E, f, left='', right='', col=None, alpha=1.0):
    """Crop marks + tiny mono annotations; drawn in screen space."""
    col = col or a(BONE, 0.55)
    col = a(col, alpha)
    cam, sh = E.cam, E.shake
    E.cam, E.shake = (LW / 2, LH / 2, 1.0, 0.0), (0.0, 0.0)
    m, L = 44, 34
    for sx, sy in ((m, m), (LW - m, m), (m, LH - m), (LW - m, LH - m)):
        dx = 1 if sx < LW / 2 else -1
        dy = 1 if sy < LH / 2 else -1
        E.line(sx, sy, sx + dx * L, sy, 2.5, col)
        E.line(sx, sy, sx, sy + dy * L, 2.5, col)
    if left:
        E.text(left, m + 52, m + 6, 17, col, font='mono', wght=500, align='left', valign='cap')
    if right:
        E.text(right, LW - m - 52, m + 6, 17, col, font='mono', wght=500, align='right', valign='cap')
    E.text(f'{int(f.bar):03d}.{int(f.beat) % 4 + 1}  ', LW - m - 52, LH - m - 6, 17, col, font='mono', wght=500,
           align='right')
    E.text('MOBILEGL', m + 52, LH - m - 6, 17, col, font='mono', wght=700, align='left')
    E.flush()
    E.cam, E.shake = cam, sh


# --- glue ------------------------------------------------------------------------------------------
_goo = {}


def goo(E, balls, caps=(), k=40.0, col=GLUE, rim=hexc('#2f8a2b'), hi=(1, 1, 1, 1), outline=5.0, bevel=0.0, glow=0.0):
    """balls: [(x, y, r)], caps: [(ax, ay, bx, by, r0, r1)] in logical px (camera applies)."""
    prog = _goo.get('p')
    if prog is None:
        prog = _goo['p'] = E.fs_program(_read('goo.glsl'))
    bl = list(balls)[:64]
    if bevel <= 0:   # dome over the whole blob: bevel ~ the biggest radius
        bevel = max([r for _, _, r in bl] + [max(c[4], c[5]) * 1.5 for c in caps] + [10.0])
    cp = list(caps)[:24]
    bv = [(x, y, r, 0.0) for x, y, r in bl] + [(0, 0, 0, 0)] * (64 - len(bl))
    cv = [(c[0], c[1], c[2], c[3]) for c in cp] + [(0, 0, 0, 0)] * (24 - len(cp))
    cr = [(c[4], c[5], 0, 0) for c in cp] + [(0, 0, 0, 0)] * (24 - len(cp))
    prog['u_balls'].value = bv
    if 'u_caps' in prog:
        prog['u_caps'].value = cv
        prog['u_capr'].value = cr
    E.fullscreen(prog, u_nb=len(bl), u_nc=len(cp), u_k=k, u_col=col, u_rim=rim, u_hi=hi, u_outline=outline,
                 u_bevel=bevel, u_glow=glow)


def drop_shape(x, y, r, stretch=0.0):
    """A falling droplet: ball + tapered tail upward (stretch in px)."""
    if stretch <= 1:
        return [(x, y, r)], []
    return [(x, y, r)], [(x, y - stretch, x, y, r * 0.18, r * 0.85)]


# --- jelly cubes -----------------------------------------------------------------------------------
_cube = {}


GLUE_BODY = hexc('#5cb846')
GLUE_DEEP = hexc('#1f5e22')


def cubes(E, f, items, body=GLUE_BODY, alt=hexc('#2c6e30'), deep=GLUE_DEEP, speckle=1.0, pitch=0.6155):
    """The MobileGlues mark: a block floating in glue.
    items: [(x, y, half_size_px, yaw, squash, wobble, tint, alpha[, seconds_since_impact])]; a negative last value
    means the block is still falling towards the glue, 0 is the hit, large (default) is settled."""
    prog = _cube.get('p')
    if prog is None:
        prog = _cube['p'] = E.fs_program(_read('block.glsl'))
    it = list(items)[:16]
    c1 = [(i[0], i[1], i[2], i[3]) for i in it] + [(0, 0, 1, 0)] * (16 - len(it))
    c2 = [(i[4], i[5], i[6], i[7]) for i in it] + [(0, 0, 0, 0)] * (16 - len(it))
    c3 = [((i[8] if len(i) > 8 else 100.0), 0, 0, 0) for i in it] + [(100.0, 0, 0, 0)] * (16 - len(it))
    prog['u_cubes'].value = c1
    prog['u_cubes2'].value = c2
    prog['u_cubes3'].value = c3
    E.fullscreen(prog, u_n=len(it), u_time=f.t, u_body=body[:3], u_alt=alt[:3], u_deep=deep[:3],
                 u_speckle=speckle, u_pitch=pitch)


# --- isometric -------------------------------------------------------------------------------------
class Iso:
    def __init__(self, cx, cy, s):
        self.cx, self.cy, self.s = cx, cy, s

    def p(self, x, y, z):
        return (self.cx + (x - y) * COS30 * self.s, self.cy + (x + y) * SIN30 * self.s - z * self.s)

    def box(self, E, x0, y0, z0, dx, dy, dz, top, left, right, line=None, sw=0.0):
        x1, y1, z1 = x0 + dx, y0 + dy, z0 + dz
        P = self.p
        st = line or (0, 0, 0, 0)
        E.poly([P(x0, y1, z0), P(x1, y1, z0), P(x1, y1, z1), P(x0, y1, z1)], left, stroke=st, sw=sw)
        E.poly([P(x1, y0, z0), P(x1, y1, z0), P(x1, y1, z1), P(x1, y0, z1)], right, stroke=st, sw=sw)
        E.poly([P(x0, y0, z1), P(x1, y0, z1), P(x1, y1, z1), P(x0, y1, z1)], top, stroke=st, sw=sw)

    def quad_top(self, E, x0, y0, z, dx, dy, c, line=None, sw=0.0):
        P = self.p
        E.poly([P(x0, y0, z), P(x0 + dx, y0, z), P(x0 + dx, y0 + dy, z), P(x0, y0 + dy, z)], c,
               stroke=line or (0, 0, 0, 0), sw=sw)

    def mat_top(self, s=1.0):
        return (COS30 * s, -COS30 * s, SIN30 * s, SIN30 * s)

    def mat_left(self, s=1.0):   # face y = const, u along +x, v down
        return (COS30 * s, 0.0, SIN30 * s, s)

    def mat_right(self, s=1.0):  # face x = const, u along -y, v down
        return (COS30 * s, 0.0, -SIN30 * s, s)


# --- type pieces -----------------------------------------------------------------------------------
def chip(E, x, y, s, size=26, fg=INK, bg_=GLUE, font='mono', wght=700, align='center', pad=(18, 10), rot=0.0,
         alpha=1.0, sc=1.0):
    w = E.measure(s, font, size, wght)
    ax = {'left': w / 2 + pad[0], 'center': 0, 'right': -w / 2 - pad[0]}[align]
    c, sn = math.cos(rot), math.sin(rot)
    cx, cy = x + ax * c * sc, y + ax * sn * sc
    E.rect(cx, cy, (w + 2 * pad[0]) * sc, (size * 0.75 + 2 * pad[1]) * sc, a(bg_, alpha), r=10 * sc, rot=rot)
    E.text(s, cx, cy, size * sc, a(fg, alpha), font=font, wght=wght, rot=rot)
    return w + 2 * pad[0]


SYNTAX = {'kw': hexc('#8cff62'), 'fn': hexc('#f1f7ec'), 'num': hexc('#e7a94f'), 'str': hexc('#a0e28c'),
          'cm': hexc('#5f8f63'), 'p': hexc('#a9c4a6'), 'const': hexc('#d4f0c9')}


def tokenize(line):
    """Very small GL/GLSL-ish highlighter -> [(text, kind)]."""
    import re
    out = []
    for m in re.finditer(r'(//.*$)|("[^"]*")|(\b\d[\w.]*\b)|(\bGL_\w+|\bMOBILEGL_\w+)|(\bgl[A-Z]\w*|\bwgl\w*|\begl\w*)|'
                         r'(\b(?:void|in|out|uniform|vec[234]|float|int|return|layout|#version|core|export)\b)|(\w+)|(\s+)|(.)',
                         line):
        g = m.lastindex
        kind = {1: 'cm', 2: 'str', 3: 'num', 4: 'const', 5: 'kw', 6: 'kw', 7: 'fn', 8: 'p', 9: 'p'}[g]
        out.append((m.group(0), kind))
    return out


def code_line(E, s, x, y, size=30, alpha=1.0, chars=None, cursor=False, f=None):
    """Monospace syntax-coloured line, left-aligned at x (baseline-ish centre y). chars = typed so far."""
    cw = size * 0.6
    n = len(s) if chars is None else max(0, min(len(s), int(chars)))
    col = 0
    for tok, kind in tokenize(s):
        if col >= n:
            break
        part = tok[:n - col]
        if part.strip():
            E.text(part, x + col * cw, y, size, a(SYNTAX[kind], alpha), font='mono', wght=500, align='left')
        col += len(tok)
    if cursor and (f is None or (f.t * 2.2) % 1 < 0.6):
        E.rect(x + n * cw + cw * 0.5, y, cw * 0.9, size * 1.05, a(GLUE, alpha))
    return n * cw


def slam(E, f, word, x, y, t_hit, size=260, col=BONE, wght=900, wdth=100, rot=0.0, punch=0.18, align='center',
         font='archivo', tracking=0.0, hold=None):
    """A word that lands at t_hit: scales down from (1 + punch) with a quick fade-in, then holds."""
    u = f.t - t_hit
    if u < -0.02:
        return 0.0
    k = 1 + punch * math.exp(-max(u, 0) / 0.07)
    al = clamp01((u + 0.02) / 0.03)
    if hold is not None and u > hold:
        al *= 1 - clamp01((u - hold) / 0.06)
    return E.text(word, x, y, size, a(col, al * col[3]), font=font, wght=wght, wdth=wdth, rot=rot, s_=k, align=align,
                  tracking=tracking)


def stamp(E, f, text, x, y, t_hit, size=70, col=CORAL, rot=-0.12, w_pad=34):
    u = f.t - t_hit
    if u < 0:
        return
    k = 1 + 0.6 * math.exp(-u / 0.05)
    w = E.measure(text, 'archivo', size, 900, 80)
    E.rect(x, y, (w + 2 * w_pad) * k, size * 1.2 * k, (0, 0, 0, 0), r=10, rot=rot, stroke=col, sw=8 * k)
    E.text(text, x, y, size * k, col, wght=900, wdth=80, rot=rot)


def cross(E, x, y, r, col=CORAL, w=18, u=1.0):
    u = clamp01(u)
    if u <= 0:
        return
    d = r * 0.7071
    u1 = clamp01(u * 2)
    u2 = clamp01(u * 2 - 1)
    E.line(x - d, y - d, x - d + 2 * d * u1, y - d + 2 * d * u1, w, col)
    if u2 > 0:
        E.line(x + d, y - d, x + d - 2 * d * u2, y - d + 2 * d * u2, w, col)


def check(E, x, y, r, col=GLUE, w=12, u=1.0):
    u = clamp01(u)
    pts = [(x - r * 0.55, y), (x - r * 0.15, y + r * 0.42), (x + r * 0.6, y - r * 0.45)]
    l1 = math.dist(pts[0], pts[1])
    l2 = math.dist(pts[1], pts[2])
    L = (l1 + l2) * u
    if L <= 0:
        return
    if L <= l1:
        q = L / l1
        E.line(pts[0][0], pts[0][1], lerp(pts[0][0], pts[1][0], q), lerp(pts[0][1], pts[1][1], q), w, col)
    else:
        E.line(pts[0][0], pts[0][1], pts[1][0], pts[1][1], w, col)
        q = (L - l1) / l2
        E.line(pts[1][0], pts[1][1], lerp(pts[1][0], pts[2][0], q), lerp(pts[1][1], pts[2][1], q), w, col)


def arrow(E, x0, y0, x1, y1, w, col, head=None, u=1.0):
    u = clamp01(u)
    if u <= 0:
        return
    xe, ye = lerp(x0, x1, u), lerp(y0, y1, u)
    ang = math.atan2(ye - y0, xe - x0)
    head = head or w * 3.2
    bx, by = xe - math.cos(ang) * head * 0.8, ye - math.sin(ang) * head * 0.8
    E.line(x0, y0, bx, by, w, col)
    E.poly([(xe, ye), (xe - math.cos(ang - 0.5) * head, ye - math.sin(ang - 0.5) * head),
            (xe - math.cos(ang + 0.5) * head, ye - math.sin(ang + 0.5) * head)], col)


def screen_cam(E, f, zoom=1.0, rot=0.0, cx=LW / 2, cy=LH / 2, shake=0.0, seed=0):
    E.cam = (cx, cy, zoom, rot)
    if shake > 0:
        fi = int(f.t * 60)
        E.shake = ((rnd(seed, fi, 1) - 0.5) * 2 * shake, (rnd(seed, fi, 2) - 0.5) * 2 * shake)
    else:
        E.shake = (0.0, 0.0)


# --- plates / words --------------------------------------------------------------------------------
PLATES = {
    'ink': (INK, BONE, a(MOSS, 0.55)),
    'mint': (MINT, INK, a(LEAF, 0.35)),
    'glue': (GLUE, INK, a(LEAF, 0.45)),
    'pale': (PALE, INK, a(LEAF, 0.3)),
    'black': (hexc('#000000'), BONE, None),
}


def plate(E, f, kind='ink', grid=True, drift=(0.0, 0.0)):
    """Background colour + dot grid; returns (bg, fg)."""
    b, fg, g = PLATES[kind]
    bg(E, b)
    if grid and g is not None:
        dot_grid(E, f, g, 60, 2.2, drift)
    return b, fg


def fit_size(E, word, width, wght=900, wdth=100, font='archivo', cap=640):
    w = E.measure(word, font, 100, wght, wdth)
    return min(cap, 100 * width / max(w, 1))


TRI_FS = '''#version 430
#include "common.glsl"
in vec2 v_uv; out vec4 o;
uniform vec2 u_res; uniform vec2 u_p[3]; uniform vec4 u_c[3];
float cross2(vec2 a, vec2 b){ return a.x * b.y - a.y * b.x; }
void main(){
  vec2 p = uncam(vec2(v_uv.x, 1.0 - v_uv.y) * u_logical);
  vec2 a = u_p[0], b = u_p[1], c = u_p[2];
  float A = cross2(b - a, c - a);
  float w0 = cross2(b - p, c - p) / A, w1 = cross2(c - p, a - p) / A, w2 = 1.0 - w0 - w1;
  // signed distance to the triangle (negative inside)
  float d = 1e5;
  vec2 e[3] = vec2[3](b - a, c - b, a - c), v[3] = vec2[3](a, b, c);
  for (int i = 0; i < 3; i++){ vec2 pv = p - v[i]; vec2 q = pv - e[i] * clamp(dot(pv, e[i]) / dot(e[i], e[i]), 0.0, 1.0);
    d = min(d, length(q)); }
  bool inside = w0 >= 0.0 && w1 >= 0.0 && w2 >= 0.0;
  float sd = inside ? -d : d;
  float px = u_logical.x / u_res.x / u_cam.z;
  float al = clamp(0.5 - sd / px, 0.0, 1.0);
  vec4 col = u_c[0] * clamp(w0, 0.0, 1.0) + u_c[1] * clamp(w1, 0.0, 1.0) + u_c[2] * clamp(w2, 0.0, 1.0);
  o = vec4(col.rgb, 1.0) * al * col.a * u_alpha;
}
'''
_tri = {}


def tri_rgb(E, pts, cols):
    prog = _tri.get('p')
    if prog is None:
        prog = _tri['p'] = E.fs_program(TRI_FS)
    prog['u_p'].value = [tuple(p) for p in pts]
    prog['u_c'].value = [tuple(c) for c in cols]
    E.fullscreen(prog)


def typed(f, s, b0, b1):
    """Substring of s typed between local beats b0 and b1."""
    n = int(len(s) * clamp01((f.lbeat - b0) / (b1 - b0)) + 1e-6)
    return s[:n]


def word_seq(E, f, words, b0, step, x, y, size, col, **kw):
    """Words that replace each other, one per `step` beats from b0; returns the active index."""
    k = int(math.floor((f.lbeat - b0) / step + 1e-6))
    if 0 <= k < len(words):
        slam(E, f, words[k], x, y, f.t - (f.lbeat - (b0 + k * step)) * S.BEAT, size, col, **kw)
    return k


def speedlines(E, f, k, col=None, n=70, cx=LW / 2, cy=LH / 2, seed=0):
    """Radial streaks rushing outward; k = intensity 0..1 (speed, length and count all scale)."""
    if k <= 0.01:
        return
    col = col or BONE
    for i in range(int(n * min(1.0, 0.3 + k))):
        ang = rnd('sl', seed, i) * 2 * math.pi
        sp = 0.4 + 0.6 * rnd('sp', seed, i)
        ph = (rnd('ph', seed, i) + f.t * sp * (0.6 + 2.4 * k)) % 1.0
        r0 = 120 + ph * ph * 1400
        ln = (40 + 360 * k) * (0.3 + ph)
        x0, y0 = cx + math.cos(ang) * r0, cy + math.sin(ang) * r0
        x1, y1 = cx + math.cos(ang) * (r0 + ln), cy + math.sin(ang) * (r0 + ln)
        E.line(x0, y0, x1, y1, 2 + 5 * k * ph, a(col, (0.15 + 0.5 * k) * min(1, ph * 3)))


def beat_plate(f, kinds, b0=0.0):
    """Plate kind for the current beat (never changes faster than once a beat)."""
    k = int(math.floor(f.lbeat - b0 + 1e-6))
    return kinds[k % len(kinds)], k


def climax(f, b0, b1):
    """0 -> 1 ramp (accelerating) over local beats [b0, b1]."""
    return in_cubic(clamp01((f.lbeat - b0) / (b1 - b0)))


def pre_drop(E, f, cx, cy, size, t_hit, scale=1.0):
    """The silent beat before a drop: a spotlit blob of glue waits, the block falls into it on the downbeat."""
    bg(E, hexc('#000000'))
    E.circle(cx, cy + 40 * scale, 520 * scale, a(INK2, 0.9))
    cubes(E, f, [(cx, cy, size, 0.0, 0.0, 0.0, 0.0, 1.0, f.t - t_hit)])
