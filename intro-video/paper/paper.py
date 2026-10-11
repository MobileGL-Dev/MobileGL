"""Tiny paper-cutout rendering engine: hand-cut shapes, paper grain, layered shadows,
stop-motion jitter, torn-paper wipes. Everything is deterministic so worker processes
can rebuild identical sprite caches independently."""
import os, re, math, zlib, random, functools
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 1920, 1080
FPS = 12          # stop-motion animation rate (encoded at 24 with doubled frames)
SS = 3            # supersampling for shape masks

# ---------------------------------------------------------------- palette
INK = (45, 42, 40); WHITE = (253, 250, 243); PAPER = (250, 244, 230)
CORAL = (231, 111, 81); TEAL = (42, 157, 143); MUSTARD = (236, 190, 90)
NAVY = (38, 70, 83); ORANGE = (244, 162, 97); SKY = (142, 202, 230)
BLUE = (52, 140, 200); BRAND = (0, 89, 156); PLUM = (146, 102, 150)
GREEN = (118, 178, 92); DIRT = (150, 100, 66); DARK = (58, 62, 72)
GREY = (170, 168, 160); PINK = (240, 150, 160); RED = (200, 50, 50)
BG_CREAM = (243, 234, 214); BG_SKY = (206, 228, 236); BG_MINT = (214, 234, 218)
BG_PEACH = (248, 222, 203); BG_LAV = (226, 219, 238); BG_BUTTER = (246, 233, 192)
BG_NAVY = (40, 58, 82)


def shade(c, f):
    if f >= 1:
        return tuple(int(v + (255 - v) * (f - 1)) for v in c)
    return tuple(int(v * f) for v in c)


def crc(*parts):
    return zlib.crc32('|'.join(map(str, parts)).encode())


# ---------------------------------------------------------------- easing
def clamp01(x): return 0.0 if x < 0 else 1.0 if x > 1 else x
def lerp(a, b, t): return a + (b - a) * t
def prog(t, t0, d): return clamp01((t - t0) / d) if d > 0 else float(t >= t0)
def ease_out(p): return 1 - (1 - p) ** 3
def ease_in(p): return p ** 3
def ease_io(p): return p * p * (3 - 2 * p)


def ease_back(p, s=1.70158):
    p -= 1
    return p * p * ((s + 1) * p + s) + 1


def bump(t, t0, d):
    return math.sin(math.pi * prog(t, t0, d))


# ---------------------------------------------------------------- fonts
FONT_FILES = {
    'fredoka': os.path.join(HERE, 'fonts', 'Fredoka.ttf'),
    'hand': os.path.join(HERE, 'fonts', 'PatrickHand.ttf'),
    'mono': next((p for p in (os.path.join(HERE, 'fonts', 'CascadiaMono.ttf'), r'C:\Windows\Fonts\CascadiaMono.ttf')
                  if os.path.exists(p)), os.path.join(HERE, 'fonts', 'CascadiaMono.ttf')),
    'kuaile': os.path.join(HERE, 'fonts', 'ZCOOLKuaiLe.ttf'),
    'wenkai': os.path.join(HERE, 'fonts', 'LXGWWenKai.ttf'),
}

# ---------------------------------------------------------------- localisation
LANG = os.environ.get('MGL_LANG', 'en')
TR = {}
if LANG == 'zh':
    from zh import TR_ZH as TR
CJK_SUB = {'hand': ('wenkai', 0.9), 'mono': ('wenkai', 0.95)}
CJK_RICH = {'fredoka': ('kuaile', 0.96)}   # Latin runs keep the original face, CJK runs use this


def T(s):
    return TR.get(s, s) if TR else s


def has_cjk(s):
    return any('\u3000' <= ch <= '\u9fff' or '\uff00' <= ch <= '\uffef' for ch in s)


def resolve(spec, s):
    """Text containing CJK is drawn with a CJK font standing in for the Latin family."""
    if not has_cjk(s):
        return spec
    if spec[0] in CJK_RICH:
        name, k = CJK_RICH[spec[0]]
        return ('rich', spec, (name, max(8, int(round(spec[1] * k)))))
    if spec[0] in CJK_SUB:
        name, k = CJK_SUB[spec[0]]
        return (name, max(8, int(round(spec[1] * k))))
    return spec


class RichFont:
    """Two faces sharing a baseline: one for Latin runs, one for CJK runs."""

    def __init__(self, lat, cjk, scale):
        self.fl, self.fc = get_font(lat, scale), get_font(cjk, scale)
        (al, dl), (ac, dc) = self.fl.getmetrics(), self.fc.getmetrics()
        self.asc, self.desc = max(al, ac), max(dl, dc)

    @staticmethod
    def _is_c(ch):
        return has_cjk(ch) or '\u2010' <= ch <= '\u206f'

    def runs(self, s):
        out, cur, curc = [], '', None
        for ch in s:
            c = curc if (ch == ' ' and curc is not None) else self._is_c(ch)
            if curc is None or c == curc:
                cur, curc = cur + ch, c
            else:
                out.append((cur, curc))
                cur, curc = ch, c
        if cur:
            out.append((cur, curc))
        return [(r, self.fc if c else self.fl) for r, c in out]

    def getlength(self, s):
        return sum(f.getlength(r) for r, f in self.runs(s))

    def bbox(self, s, spacing, h):
        lines = s.split('\n')
        w = max(self.getlength(l) for l in lines)
        hh = len(lines) * (self.asc + self.desc) + (len(lines) - 1) * spacing
        return (-w / 2, 0, w / 2, hh) if h == 'm' else (0, 0, w, hh)

    def draw(self, d, x, y, s, spacing, h, v, fill):
        lines = s.split('\n')
        lh = self.asc + self.desc
        top = y - ((len(lines) * lh + (len(lines) - 1) * spacing) / 2 if v == 'm' else 0)
        for i, ln in enumerate(lines):
            xs = x - (self.getlength(ln) / 2 if h == 'm' else 0)
            base = top + self.asc + i * (lh + spacing)
            for r, f in self.runs(ln):
                d.text((xs, base), r, font=f, fill=fill, anchor='ls')
                xs += f.getlength(r)


@functools.lru_cache(None)
def get_font(spec, scale=1):
    if spec[0] == 'rich':
        return RichFont(spec[1], spec[2], scale)
    f = ImageFont.truetype(FONT_FILES[spec[0]], int(round(spec[1] * scale)))
    if spec[0] == 'fredoka':
        f.set_variation_by_name(spec[2] if len(spec) > 2 else 'SemiBold')
    return f


_D = ImageDraw.Draw(Image.new('L', (8, 8)))


def text_bbox(s, font, spacing=6, align='center', h='m'):
    s = T(s)
    font = resolve(font, s)
    if font[0] == 'rich':
        return get_font(font).bbox(s, spacing, h)
    return _D.multiline_textbbox((0, 0), s, font=get_font(font), anchor=h + 'a',
                                 spacing=spacing, align=align)


def measure(s, font, spacing=6):
    l, t, r, b = text_bbox(s, font, spacing)
    return r - l, b - t


_TOK = re.compile(r'[A-Za-z0-9_.+#/×{}\-\u00a0]+|\s+|.', re.S)
_NO_START = set('，。、：；！？）》」』”’…—,.;:!?)')
_NO_END = set('（《「『“‘(')


_BREAK_AFTER = set('，。、：；！？')


def _cjk_greedy(toks, f, w):
    out, cur = [], []
    for tk in toks:
        line = ''.join(cur)
        if not line.strip() or f.getlength((line + tk).rstrip()) <= w:
            cur.append(tk)
            continue
        j = None   # prefer breaking right after punctuation if the line stays reasonably full
        for k in range(len(cur) - 1, 0, -1):
            if cur[k - 1][-1] in _BREAK_AFTER or cur[k - 1].isspace():
                if f.getlength(''.join(cur[:k]).rstrip()) >= 0.7 * w:
                    j = k
                break
        if j:
            out.append(''.join(cur[:j]).rstrip())
            cur = cur[j:]
        else:
            out.append(line.rstrip())
            cur = []
        while cur and cur[0].isspace():
            cur.pop(0)
        if cur or not tk.isspace():
            cur.append(tk)
    out.append(''.join(cur).rstrip())
    return out


def wrap_cjk(s, f, maxw):
    out = []
    for para in s.split('\n'):
        toks = []
        for tk in _TOK.findall(para):
            if toks and (tk[0] in _NO_START or toks[-1][-1] in _NO_END):
                toks[-1] += tk
            else:
                toks.append(tk)
        n = len(_cjk_greedy(toks, f, maxw))
        w = maxw
        if n > 1:   # balance: narrowest width that keeps the same line count
            lo = maxw * 0.3
            while w - lo > 4:
                mid = (lo + w) / 2
                if len(_cjk_greedy(toks, f, mid)) <= n:
                    w = mid
                else:
                    lo = mid
        out += _cjk_greedy(toks, f, w)
    return '\n'.join(out)


def wrap(s, font, maxw):
    s = T(s)
    font = resolve(font, s)
    f = get_font(font)
    if has_cjk(s):
        return wrap_cjk(s, f, maxw)
    out = []
    for para in s.split('\n'):
        line = ''
        for w in para.split(' '):
            cand = (line + ' ' + w).strip()
            if not line or f.getlength(cand) <= maxw:
                line = cand
            else:
                out.append(line)
                line = w
        out.append(line)
    return '\n'.join(out)


# ---------------------------------------------------------------- paper grain
_GRAIN = None


def grain():
    global _GRAIN
    if _GRAIN is None:
        size = 1024
        rng = np.random.default_rng(7)
        ky = np.fft.fftfreq(size)[:, None]
        kx = np.fft.fftfreq(size)[None, :]
        k2 = kx ** 2 + ky ** 2

        def periodic(sigma):   # gaussian-filtered white noise, seamlessly tileable
            f = np.fft.fft2(rng.standard_normal((size, size)))
            o = np.real(np.fft.ifft2(f * np.exp(-k2 * (2 * np.pi * sigma) ** 2 / 2)))
            return (o / o.std()).astype(np.float32)
        g = 0.45 * periodic(90) + 0.35 * periodic(24) + 0.3 * periodic(5)
        g += 0.4 * rng.standard_normal((size, size)).astype(np.float32)
        fib = Image.new('L', (size, size), 0)
        d = ImageDraw.Draw(fib)
        for _ in range(1400):
            x, y = rng.uniform(0, size, 2)
            ang = rng.uniform(0, math.pi)
            L = rng.uniform(5, 26)
            col = int(rng.uniform(70, 170))
            for ox in (-size, 0, size):
                for oy in (-size, 0, size):
                    d.line([(x + ox, y + oy), (x + ox + L * math.cos(ang), y + oy + L * math.sin(ang))],
                           fill=col, width=1)
        fib = np.asarray(fib.filter(ImageFilter.GaussianBlur(0.5)), np.float32) / 255
        g = (g - g.mean()) / g.std()
        _GRAIN = g + fib * 1.4
    return _GRAIN


def textured(color, w, h, seed, k):
    c = np.array(color, np.float32)
    if k <= 0:
        return np.ascontiguousarray(np.broadcast_to(c.astype(np.uint8), (h, w, 3)))
    g = grain()
    r = random.Random(seed)
    ys = (np.arange(h) + r.randrange(1024)) % 1024
    xs = (np.arange(w) + r.randrange(1024)) % 1024
    out = c[None, None, :] * (1 + k * g[np.ix_(ys, xs)][..., None])
    return np.clip(out, 0, 255).astype(np.uint8)


# ---------------------------------------------------------------- shapes
def rrect_pts(x, y, w, h, r):
    r = max(0.0, min(r, w / 2, h / 2))
    pts = []
    for cx, cy, a0 in [(x + w - r, y + r, -90), (x + w - r, y + h - r, 0),
                       (x + r, y + h - r, 90), (x + r, y + r, 180)]:
        for k in range(9):
            a = math.radians(a0 + 90 * k / 8)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def ellipse_pts(cx, cy, rx, ry, n=None):
    n = n or max(28, int((rx + ry) / 2.5))
    return [(cx + rx * math.cos(2 * math.pi * k / n), cy + ry * math.sin(2 * math.pi * k / n))
            for k in range(n)]


def star_pts(cx, cy, r1, r2, n=5, rot=-90):
    pts = []
    for k in range(2 * n):
        r = r1 if k % 2 == 0 else r2
        a = math.radians(rot + 180 * k / n)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def gear_pts(cx, cy, r1, r2, teeth):
    pts = []
    for k in range(teeth * 4):
        a = 2 * math.pi * k / (teeth * 4)
        r = r2 if (k % 4) in (1, 2) else r1
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def rot_rect_pts(cx, cy, w, h, ang):
    a = math.radians(ang)
    ca, sa = math.cos(a), math.sin(a)
    return [(cx + x * ca - y * sa, cy + x * sa + y * ca)
            for x, y in [(-w / 2, -h / 2), (w / 2, -h / 2), (w / 2, h / 2), (-w / 2, h / 2)]]


def roughen(pts, amp, seed, step=7):
    if amp <= 0 or len(pts) < 3:
        return pts
    p = []
    n0 = len(pts)
    for i in range(n0):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % n0]
        n = max(1, int(math.hypot(x1 - x0, y1 - y0) / step))
        for k in range(n):
            p.append((x0 + (x1 - x0) * k / n, y0 + (y1 - y0) * k / n))
    p = np.array(p, np.float32)
    rng = np.random.default_rng(seed)
    a = rng.standard_normal(p.shape).astype(np.float32)
    a = sum(np.roll(a, s, axis=0) for s in range(-1, 2))
    b = rng.standard_normal(p.shape).astype(np.float32)
    b = sum(np.roll(b, s, axis=0) for s in range(-6, 7))
    a *= 0.6 * amp / (a.std() + 1e-6)
    b *= 0.9 * amp / (b.std() + 1e-6)
    return [tuple(v) for v in (p + a + b)]


# ---------------------------------------------------------------- art builder
class Art:
    """Stack of paper layers. Each layer gets grain and casts a soft shadow on layers below."""

    def __init__(self, w, h, key):
        self.w, self.h, self.key = int(math.ceil(w)), int(math.ceil(h)), key
        self.ops = []

    def _seed(self):
        return crc(self.key, len(self.ops))

    def poly(self, pts, color, rough=1.0, shade=True, tex=0.035, opacity=1.0):
        self.ops.append(dict(kind='poly', pts=list(pts), color=color, rough=rough, shade=shade,
                             tex=tex, opacity=opacity, seed=self._seed()))
        return self

    def rrect(self, x, y, w, h, r, color, **kw):
        return self.poly(rrect_pts(x, y, w, h, r), color, **kw)

    def ellipse(self, cx, cy, rx, ry, color, **kw):
        return self.poly(ellipse_pts(cx, cy, rx, ry), color, **kw)

    def circle(self, cx, cy, r, color, **kw):
        return self.ellipse(cx, cy, r, r, color, **kw)

    def text(self, cx, cy, s, font, color, h='m', v='m', spacing=6, align='center',
             shade=False, tex=0.0, opacity=1.0):
        s = T(s)
        font = resolve(font, s)
        self.ops.append(dict(kind='text', x=cx, y=cy, s=s, font=font, color=color, h=h, v=v,
                             spacing=spacing, align=align, shade=shade, tex=tex,
                             opacity=opacity, seed=self._seed()))
        return self

    def lines(self, pls, width, color, shade=False, tex=0.0, caps=True, opacity=1.0):
        self.ops.append(dict(kind='lines', pls=pls, width=width, color=color, shade=shade,
                             tex=tex, caps=caps, opacity=opacity, seed=self._seed()))
        return self

    def build(self, pad=30):
        P = pad
        w2, h2 = self.w + 2 * P, self.h + 2 * P
        acc = Image.new('RGBA', (w2, h2), (0, 0, 0, 0))

        def T(x, y):
            return ((x + P) * SS, (y + P) * SS)

        for op in self.ops:
            m = Image.new('L', (w2 * SS, h2 * SS), 0)
            d = ImageDraw.Draw(m)
            if op['kind'] == 'poly':
                pts = roughen(op['pts'], op['rough'], op['seed'])
                d.polygon([T(x, y) for x, y in pts], fill=255)
            elif op['kind'] == 'text' and op['font'][0] == 'rich':
                get_font(op['font'], SS).draw(d, *T(op['x'], op['y']), op['s'], op['spacing'] * SS,
                                              op['h'], op['v'], 255)
            elif op['kind'] == 'text':
                l, t, r, b = text_bbox(op['s'], op['font'], op['spacing'], op['align'], op['h'])
                x = op['x'] - ((l + r) / 2 if op['h'] == 'm' else l)
                y = op['y'] - ((t + b) / 2 if op['v'] == 'm' else t)
                d.multiline_text(T(x, y), op['s'], font=get_font(op['font'], SS), fill=255,
                                 anchor=op['h'] + 'a', spacing=op['spacing'] * SS,
                                 align=op['align'])
            else:
                wd = max(1, int(op['width'] * SS))
                for pl in op['pls']:
                    q = [T(x, y) for x, y in pl]
                    d.line(q, fill=255, width=wd, joint='curve')
                    if op['caps']:
                        for qx, qy in (q[0], q[-1]):
                            d.ellipse([qx - wd / 2, qy - wd / 2, qx + wd / 2, qy + wd / 2], fill=255)
            m = m.resize((w2, h2), Image.BOX)
            if op['opacity'] < 1:
                o = op['opacity']
                m = m.point(lambda v: int(v * o))
            if op['shade']:
                sh = ImageChops.offset(m.filter(ImageFilter.GaussianBlur(2.4)), 2, 3)
                sh = ImageChops.multiply(sh, acc.getchannel('A')).point(lambda v: int(v * 0.40))
                sl = Image.new('RGBA', (w2, h2), (30, 20, 10, 0))
                sl.putalpha(sh)
                acc = Image.alpha_composite(acc, sl)
            layer = Image.fromarray(textured(op['color'], w2, h2, op['seed'], op['tex']), 'RGB')
            layer = layer.convert('RGBA')
            layer.putalpha(m)
            acc = Image.alpha_composite(acc, layer)
        return Sprite(acc, self.key)


class Sprite:
    def __init__(self, img, key, shadow_alpha=0.40, blur=6):
        arr = np.array(img)
        a = arr[..., 3]
        hole = a == 0
        if hole.any() and not hole.all():
            iy, ix = ndimage.distance_transform_edt(hole, return_distances=False, return_indices=True)
            for ch in range(3):
                c = arr[..., ch]
                c[hole] = c[iy[hole], ix[hole]]
        self.img = Image.fromarray(arr, 'RGBA')
        self.key = key
        sa = img.getchannel('A').filter(ImageFilter.GaussianBlur(blur)).point(
            lambda v: int(v * shadow_alpha))
        self.shadow = Image.new('RGBA', img.size, (35, 25, 15, 0))
        self.shadow.putalpha(sa)
        self.w, self.h = img.size


def comp(dst, src, x, y):
    sx0, sy0 = max(0, -x), max(0, -y)
    dx0, dy0 = max(0, x), max(0, y)
    w = min(src.width - sx0, dst.width - dx0)
    h = min(src.height - sy0, dst.height - dy0)
    if w <= 0 or h <= 0:
        return
    if sx0 or sy0 or w < src.width or h < src.height:
        src = src.crop((sx0, sy0, sx0 + w, sy0 + h))
    dst.alpha_composite(src, (dx0, dy0))


def _fade(img, alpha):
    img = img.copy()
    img.putalpha(img.getchannel('A').point(lambda v: int(v * alpha)))
    return img


def place(frame, spr, x, y, rot=0.0, sx=1.0, sy=None, alpha=1.0, elev=1.0, fi=0, boil=1.0,
          shadow=True):
    sy = sx if sy is None else sy
    if alpha <= 0.01 or sx <= 0.02 or sy <= 0.02:
        return
    if boil:
        r = random.Random(crc(spr.key, fi // 2))
        x += r.uniform(-1.1, 1.1) * boil
        y += r.uniform(-1.1, 1.1) * boil
        rot += r.uniform(-0.4, 0.4) * boil
    img, sh = spr.img, spr.shadow
    if abs(sx - 1) > 1e-3 or abs(sy - 1) > 1e-3:
        size = (max(1, int(img.width * sx)), max(1, int(img.height * sy)))
        img = img.resize(size, Image.BILINEAR)
        sh = sh.resize(size, Image.BILINEAR) if shadow else sh
    if abs(rot) > 0.05:
        img = img.rotate(rot, Image.BICUBIC, expand=True)
        if shadow:
            sh = sh.rotate(rot, Image.BILINEAR, expand=True)
    if alpha < 0.999:
        img = _fade(img, alpha)
        if shadow:
            sh = _fade(sh, alpha)
    if shadow and elev > 0:
        comp(frame, sh, int(round(x - sh.width / 2 + 6 * elev)), int(round(y - sh.height / 2 + 9 * elev)))
    comp(frame, img, int(round(x - img.width / 2)), int(round(y - img.height / 2)))


class Ctx:
    def __init__(self, frame, t, fi, dur):
        self.frame, self.t, self.fi, self.dur = frame, t, fi, dur

    def put(self, spr, x, y, rot=0.0, s=1.0, sy=None, alpha=1.0, elev=1.0, boil=1.0, shadow=True):
        place(self.frame, spr, x, y, rot, s, sy, alpha, elev, self.fi, boil, shadow)

    def _out(self, out, od):
        if out is None or self.t <= out:
            return 1.0, 1.0
        q = prog(self.t, out, od)
        return 1 - 0.85 * ease_in(q), 1 - q

    def pop(self, spr, x, y, t0, d=0.55, rot=0.0, s=1.0, elev=1.0, out=None, od=0.35, alpha=1.0,
            shadow=True):
        if self.t < t0:
            return False
        p = prog(self.t, t0, d)
        e = ease_out(p)
        sc = s * (0.2 + 0.8 * ease_back(p))
        sg = 1 if crc(spr.key) % 2 else -1
        so, ao = self._out(out, od)
        if ao <= 0.01:
            return False
        self.put(spr, x, y, rot + (1 - e) * 14 * sg, sc * so, None, alpha * min(1, p * 4) * ao,
                 elev + 3 * (1 - e), shadow=shadow)
        return True

    def slide(self, spr, p0, p1, t0, d=0.8, rot=0.0, s=1.0, elev=1.0, arc=0.0, out=None, od=0.35,
              rot0=None, s0=None, before=False, lift=2.5):
        if self.t < t0 and not before:
            return False
        p = prog(self.t, t0, d)
        e = ease_io(p)
        x = lerp(p0[0], p1[0], e)
        y = lerp(p0[1], p1[1], e) - arc * math.sin(math.pi * e)
        r = rot if rot0 is None else lerp(rot0, rot, e)
        sc = s if s0 is None else lerp(s0, s, e)
        so, ao = self._out(out, od)
        if ao <= 0.01:
            return False
        self.put(spr, x, y, r, sc * so, None, ao, elev + lift * math.sin(math.pi * p))
        return True

    def unfold(self, spr, x, y, t0, d=0.6, rot=0.0, s=1.0, elev=1.0, out=None, od=0.35):
        if self.t < t0:
            return False
        p = prog(self.t, t0, d)
        so, ao = self._out(out, od)
        if ao <= 0.01:
            return False
        self.put(spr, x, y, rot, s * max(0.03, ease_back(p, 1.3)) * so, s * so, ao * min(1, p * 5),
                 elev + 2 * (1 - p))
        return True


# ---------------------------------------------------------------- backgrounds / wipe
@functools.lru_cache(None)
def background(color):
    rgb = textured(color, W, H, crc('bg', color), 0.04).astype(np.float32)
    yy, xx = np.mgrid[0:H, 0:W]
    v = ((xx - W / 2) / (W * 0.72)) ** 2 + ((yy - H / 2) / (H * 0.72)) ** 2
    rgb *= (1 - 0.10 * np.clip(v, 0, 1.6))[..., None]
    img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)).convert('RGBA')
    ov = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(ov)
    for (a, b) in [((0, H * 0.18), (W, H * 0.92)), ((W * 0.64, 0), (W * 0.30, H))]:
        d.line([a, b], fill=(40, 30, 20, 16), width=3)
        d.line([(a[0] + 3, a[1] - 3), (b[0] + 3, b[1] - 3)], fill=(255, 255, 255, 26), width=3)
    return Image.alpha_composite(img, ov.filter(ImageFilter.GaussianBlur(1.2)))


SHEET_M = 110


@functools.lru_cache(None)
def sheet(color):
    bg = background(color)
    M = SHEET_M
    base = Image.new('RGBA', (W + M, H))
    base.paste(bg, (M, 0))
    base.paste(bg.crop((0, 0, M, H)).transpose(Image.FLIP_LEFT_RIGHT), (0, 0))
    rng = random.Random(crc('tear', color))
    edge, core = [], []
    for y in range(0, H + 15, 12):
        x = M * 0.62 + rng.uniform(-14, 14) + 12 * math.sin(y / 85.0)
        edge.append((x, y))
        core.append((x - rng.uniform(4, 11), y))
    mask = Image.new('L', (W + M, H), 0)
    ImageDraw.Draw(mask).polygon(edge + [(W + M, H), (W + M, 0)], fill=255)
    cmask = Image.new('L', (W + M, H), 0)
    ImageDraw.Draw(cmask).polygon(core + [(W + M, H), (W + M, 0)], fill=255)
    out = Image.new('RGBA', (W + M, H), (0, 0, 0, 0))
    sh = ImageChops.offset(cmask.filter(ImageFilter.GaussianBlur(9)), -12, 0).point(lambda v: int(v * 0.35))
    sl = Image.new('RGBA', out.size, (30, 20, 10, 0))
    sl.putalpha(sh)
    out = Image.alpha_composite(out, sl)
    cl = Image.new('RGBA', out.size, shade(color, 1.6) + (255,))
    cl.putalpha(cmask)
    out = Image.alpha_composite(out, cl)
    b2 = base.copy()
    b2.putalpha(mask)
    return Image.alpha_composite(out, b2)


def draw_wipe(frame, color, p):
    x = int(round(lerp(W, -SHEET_M, ease_io(p))))
    comp(frame, sheet(color), x, 0)
