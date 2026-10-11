"""MobileGL intro video — paper-cutout style. `python video.py stills 3.0 25.5 ...` or `python video.py render`."""
import sys, json, time, subprocess
from multiprocessing import Pool
from paper import *

cache = functools.lru_cache(None)


# ================================================================ generic sprites
@cache
def spr_label(text, font=('fredoka', 40, 'SemiBold'), bg=WHITE, fg=INK, padx=30, pady=16, r=10,
              maxw=None, rough=1.0, spacing=6, align='center'):
    if maxw:
        text = wrap(text, font, maxw)
    tw, th = measure(text, font, spacing)
    w, h = tw + 2 * padx, th + 2 * pady
    a = Art(w, h, f'label|{text}|{font}|{bg}|{fg}')
    a.rrect(0, 0, w, h, r, bg, rough=rough)
    a.text(w / 2, h / 2, text, font, fg, spacing=spacing, align=align)
    return a.build()


@cache
def spr_tile(ch, color):
    a = Art(160, 180, f'tile|{ch}|{color}')
    a.poly([(0, 14), (14, 0), (114, 0), (160, 46), (160, 166), (146, 180), (14, 180), (0, 166)],
           color, rough=0.8)
    a.poly([(114, 0), (114, 46), (160, 46)], shade(color, 1.3), rough=0.4)
    a.text(78, 94, ch, ('fredoka', 122, 'Bold'), WHITE, shade=True)
    return a.build()


@cache
def spr_confetti(kind, color, size):
    a = Art(size, size, f'conf|{kind}|{color}|{size}')
    if kind == 0:
        a.poly([(0, size), (size / 2, 0), (size, size)], color, rough=0.5)
    elif kind == 1:
        a.circle(size / 2, size / 2, size / 2, color, rough=0.5)
    elif kind == 2:
        a.rrect(0, 0, size, size, 3, color, rough=0.5)
    else:
        a.poly(star_pts(size / 2, size / 2, size / 2, size / 4.5), color, rough=0.3)
    return a.build(pad=16)


@cache
def spr_crane(color):
    lt, dk, dd = shade(color, 1.25), shade(color, 0.8), shade(color, 0.62)
    a = Art(330, 230, f'crane|{color}')
    a.poly([(178, 128), (252, 26), (206, 140)], dd, rough=0.5)              # far wing
    a.poly([(28, 66), (142, 150), (118, 164)], dk, rough=0.5)               # tail
    a.poly([(110, 150), (170, 116), (234, 150), (170, 182)], color, rough=0.5)  # body
    a.poly([(110, 150), (170, 182), (156, 150)], dk, rough=0.4, shade=False)
    a.poly([(214, 148), (292, 50), (230, 158)], dk, rough=0.4)              # neck
    a.poly([(292, 50), (324, 66), (286, 64)], dd, rough=0.3)                # head
    a.poly([(138, 142), (92, 10), (194, 134)], lt, rough=0.5)               # near wing
    a.poly([(138, 142), (92, 10), (150, 120)], color, rough=0.4, shade=False)
    return a.build()


@cache
def spr_plane(color):
    a = Art(170, 96, f'plane|{color}')
    a.poly([(0, 18), (170, 48), (52, 48)], shade(color, 1.25), rough=0.4)
    a.poly([(52, 48), (170, 48), (72, 64)], shade(color, 0.65), rough=0.3)
    a.poly([(52, 48), (170, 48), (22, 88)], color, rough=0.4)
    return a.build()


@cache
def spr_bubble(text, font=('mono', 32), bg=WHITE, fg=INK, tail='bl'):
    tw, th = measure(text, font)
    w, h = tw + 60, th + 40
    a = Art(w, h + 34, f'bubble|{text}|{bg}|{tail}')
    tp = [(40, h - 8), (92, h - 8), (28, h + 32)] if tail == 'bl' else \
        [(w - 92, h - 8), (w - 40, h - 8), (w - 28, h + 32)]
    a.poly(tp, bg, rough=0.6, shade=False)
    a.rrect(0, 0, w, h, 22, bg, shade=False)
    a.text(w / 2, h / 2, text, font, fg)
    return a.build()


@cache
def spr_appwin():
    a = Art(560, 400, 'appwin')
    a.rrect(0, 0, 560, 400, 22, DARK)
    a.circle(34, 28, 9, CORAL, shade=False, rough=0.3)
    a.circle(60, 28, 9, MUSTARD, shade=False, rough=0.3)
    a.circle(86, 28, 9, GREEN, shade=False, rough=0.3)
    a.text(300, 28, 'my_gl_game', ('hand', 30), (225, 222, 215))
    a.rrect(14, 52, 532, 334, 10, SKY, rough=0.6)
    a.circle(462, 118, 34, MUSTARD)
    a.ellipse(170, 112, 52, 22, WHITE, rough=0.7)
    a.ellipse(206, 98, 34, 22, WHITE, rough=0.7)
    heights = [3, 3, 4, 4, 3, 2, 2, 3, 4, 5, 5, 4]
    sky = [(14, 386)]
    for i, hh in enumerate(heights):
        top = 386 - hh * 34
        sky += [(14 + 44 * i, top), (14 + 44 * (i + 1), top)]
    sky += [(546, 386)]
    a.poly(sky, DIRT, rough=0.5)
    for i, hh in enumerate(heights):
        top = 386 - hh * 34
        a.rrect(14 + 44 * i, top, 44, 14, 2, GREEN, rough=0.4, shade=i % 2 == 0)
    a.rrect(152, 160, 18, 76, 2, DIRT, rough=0.4)            # voxel tree
    a.rrect(120, 104, 82, 66, 4, shade(GREEN, 0.8), rough=0.5)
    return a.build()


@cache
def spr_phone(screen=BG_SKY, w=230, h=440):
    a = Art(w, h, f'phone|{screen}|{w}')
    a.rrect(0, 0, w, h, 34, DARK)
    a.rrect(14, 46, w - 28, h - 100, 12, screen, rough=0.6)
    a.rrect(w / 2 - 30, 20, 60, 10, 5, (30, 32, 38), shade=False)
    a.circle(w / 2, h - 28, 14, (84, 88, 98), shade=False)
    return a.build()


@cache
def spr_tablet(screen=BG_MINT):
    a = Art(400, 300, f'tablet|{screen}')
    a.rrect(0, 0, 400, 300, 28, DARK)
    a.rrect(40, 18, 320, 264, 10, screen, rough=0.6)
    a.circle(20, 150, 8, (84, 88, 98), shade=False)
    return a.build()


@cache
def spr_laptop(screen=SKY, s=1.0):
    a = Art(360, 240, f'laptop|{screen}')
    a.rrect(30, 0, 300, 196, 14, DARK)
    a.rrect(44, 14, 272, 166, 6, screen, rough=0.5)
    a.poly([(0, 202), (360, 202), (338, 236), (22, 236)], GREY, rough=0.6)
    return a.build()


@cache
def spr_gpu(size=220, label='GPU'):
    a = Art(size, size, f'gpu|{size}|{label}')
    pins = []
    n = 5
    e0, e1 = size * 0.03, size * 0.2
    for k in range(n):
        o = size * (0.26 + 0.48 * k / (n - 1))
        pins += [[(o, e0), (o, e1)], [(o, size - e0), (o, size - e1)],
                 [(e0, o), (e1, o)], [(size - e0, o), (size - e1, o)]]
    a.lines(pins, size * 0.05, GREY, caps=False, tex=0.03, shade=False)
    m1, m2 = size * 0.14, size * 0.25
    a.rrect(m1, m1, size - 2 * m1, size - 2 * m1, size * 0.07, DARK)
    a.rrect(m2, m2, size - 2 * m2, size - 2 * m2, size * 0.04, (84, 92, 108))
    a.text(size / 2, size / 2, label, ('fredoka', int(size * 0.2), 'SemiBold'), WHITE)
    return a.build()


@cache
def spr_slab(text, w, color, h=110, size=52, fg=WHITE):
    a = Art(w, h, f'slab|{text}|{w}|{color}')
    a.rrect(0, 0, w, h, 18, color)
    a.rrect(10, 10, w - 20, h - 20, 12, shade(color, 1.08), shade=False, rough=0.6)
    a.text(w / 2, h / 2, text, ('fredoka', size, 'SemiBold'), fg)
    return a.build()


@cache
def spr_arrow(L=120, color=INK, wd=22, head=46):
    a = Art(L, head, f'arrow|{L}|{color}|{wd}|{head}')
    cy, hx = head / 2, L - head * 0.9
    a.poly([(0, cy - wd / 2), (hx, cy - wd / 2), (hx, 0), (L, cy), (hx, head), (hx, cy + wd / 2),
            (0, cy + wd / 2)], color, rough=0.6)
    return a.build()


@cache
def spr_arrow_half(side):
    rng = random.Random(5 if side == 'L' else 6)
    head, wd, cy = 70, 34, 35
    if side == 'L':
        L = 150
        jag = [(L + rng.uniform(-10, 10), cy - wd / 2 + wd * k / 6) for k in range(7)]
        pts = [(0, cy - wd / 2)] + jag + [(0, cy + wd / 2)]
    else:
        L = 150
        hx = L - head * 0.9
        jag = [(rng.uniform(-10, 10), cy + wd / 2 - wd * k / 6) for k in range(7)]
        pts = jag + [(hx, cy - wd / 2), (hx, 0), (L, cy), (hx, head), (hx, cy + wd / 2)]
    a = Art(L, head, f'arrowhalf|{side}')
    a.poly(pts, CORAL, rough=0.6)
    return a.build()


@cache
def spr_sticker(text, color, r=80, size=34, fg=WHITE):
    a = Art(2 * r, 2 * r, f'sticker|{text}|{color}|{r}')
    a.circle(r, r, r, color)
    a.circle(r, r, r - 10, shade(color, 1.1), shade=False, rough=0.5)
    a.text(r, r, text, ('fredoka', size, 'SemiBold'), fg, spacing=2)
    return a.build()


@cache
def spr_qmark():
    return spr_sticker('?', MUSTARD, r=62, size=96)


@cache
def spr_star(color=MUSTARD, r=40):
    a = Art(2 * r, 2 * r, f'star|{color}|{r}')
    a.poly(star_pts(r, r, r, r * 0.42), color, rough=0.4)
    return a.build(pad=18)


@cache
def spr_gear(r, color):
    a = Art(2 * r, 2 * r, f'gear|{r}|{color}')
    a.poly(gear_pts(r, r, r * 0.78, r, 10), color, rough=0.4)
    a.circle(r, r, r * 0.28, shade(color, 0.7), rough=0.3)
    return a.build(pad=16)


@cache
def spr_cloud(r=34):
    a = Art(3 * r, 2 * r, f'cloud|{r}')
    a.circle(r * 0.9, r * 1.15, r * 0.8, WHITE, shade=False, rough=0.6)
    a.circle(r * 1.6, r * 0.95, r * 0.95, WHITE, shade=False, rough=0.6)
    a.circle(r * 2.2, r * 1.25, r * 0.7, WHITE, shade=False, rough=0.6)
    return a.build(pad=16)


@cache
def spr_house(roof, w=360, h=330):
    a = Art(w, h, f'house|{roof}|{w}')
    a.rrect(w * 0.68, 22, 44, 96, 4, shade(roof, 0.7))
    a.rrect(32, 128, w - 64, h - 128, 8, PAPER)
    a.poly([(0, 142), (w / 2, 8), (w, 142)], roof)
    a.rrect(w / 2 - 34, h - 112, 68, 112, 10, shade(roof, 0.75))
    a.circle(w / 2 + 20, h - 56, 5, MUSTARD, shade=False, rough=0.2)
    a.rrect(64, 168, 64, 58, 6, SKY, rough=0.5)
    a.rrect(w - 128, 168, 64, 58, 6, SKY, rough=0.5)
    return a.build()


HOUSE_CHIMNEY = (360 * 0.68 + 22 - 180, 22 - 165)   # offset from house centre


@cache
def spr_ink(text, font, color):
    tw, th = measure(text, font)
    a = Art(tw + 12, th + 12, f'ink|{text}|{font}|{color}')
    a.text((tw + 12) / 2, (th + 12) / 2, text, font, color, shade=True)
    return a.build()


def wrap_balanced(s, font, maxw):
    n = wrap(s, font, maxw).count(chr(10))
    if n == 0:
        return s
    lo, hi = 200.0, float(maxw)
    while hi - lo > 6:
        mid = (lo + hi) / 2
        if wrap(s, font, mid).count(chr(10)) <= n:
            hi = mid
        else:
            lo = mid
    return wrap(s, font, hi)


# ================================================================ captions / tags
CAP_FONT = ('hand', 52)


@cache
def spr_caption(text):
    s = wrap_balanced(text, CAP_FONT, 1560)
    tw, th = measure(s, CAP_FONT, 4)
    w, h = max(tw + 90, 520), th + 38
    a = Art(w, h, 'cap|' + text)
    a.rrect(0, 0, w, h, 6, PAPER, rough=1.6)
    a.text(w / 2, h / 2 + 2, s, CAP_FONT, INK, spacing=4)
    a.poly(rot_rect_pts(26, 2, 96, 32, -10), (248, 232, 168), rough=0.5, opacity=0.78, tex=0.02, shade=False)
    a.poly(rot_rect_pts(w - 26, 2, 96, 32, 9), (248, 232, 168), rough=0.5, opacity=0.78, tex=0.02, shade=False)
    return a.build()


def draw_caption(c, t0, t1, text, idx):
    if c.t < t0 or c.t > t1 + 0.35:
        return
    spr = spr_caption(text)
    p = prog(c.t, t0, 0.45)
    q = prog(c.t, t1, 0.32)
    y = 1046 - (spr.h - 60) / 2
    y += (1 - ease_back(p, 1.2)) * 200 + ease_in(q) * 220
    rot = (0.8 if idx % 2 else -0.8)
    c.put(spr, 960, y, rot, 1, None, 1, 1.2)


@cache
def spr_tag(text, color):
    tw, th = measure(text, ('fredoka', 36, 'SemiBold'))
    w, h = tw + 100, 70
    a = Art(w, h, f'tag|{text}|{color}')
    a.poly([(0, 0), (w - 34, 0), (w, h / 2), (w - 34, h), (0, h)], color, rough=1.0)
    a.circle(w - 40, h / 2, 9, shade(color, 0.6), shade=False, rough=0.3)
    a.text(28, h / 2, text, ('fredoka', 36, 'SemiBold'), WHITE, h='l')
    return a.build()


def draw_tag(c, text, color):
    spr = spr_tag(text, color)
    p = prog(c.t, 0.15, 0.7)
    x = lerp(-spr.w / 2 - 20, spr.w / 2 - 20, ease_back(p, 1.1))
    c.put(spr, x, 82, -1.5, 1, None, 1, 1.4)


# ================================================================ shared bits
def confetti(c, seed, t0, n=16, box=(230, 220, 1690, 760), cols=(CORAL, MUSTARD, TEAL, BLUE, PLUM, ORANGE)):
    rng = random.Random(seed)
    k = 0
    while k < n:
        x, y = rng.uniform(70, 1850), rng.uniform(150, 880)
        kind, col, size, rot = rng.randrange(4), rng.choice(cols), rng.randrange(26, 56), rng.uniform(-40, 40)
        if box[0] < x < box[2] and box[1] < y < box[3]:
            continue
        bob = 5 * math.sin(c.t * 1.7 + k)
        c.pop(spr_confetti(kind, col, size), x, y + bob, t0 + k * 0.09, rot=rot, elev=0.6)
        k += 1


def crane_fly(c, spr, t0, t1, p0, p1, ctrl, s=1.0, flip=False):
    if c.t < t0:
        return
    p = ease_io(prog(c.t, t0, t1 - t0))
    x = (1 - p) ** 2 * p0[0] + 2 * (1 - p) * p * ctrl[0] + p * p * p1[0]
    y = (1 - p) ** 2 * p0[1] + 2 * (1 - p) * p * ctrl[1] + p * p * p1[1]
    dx = 2 * (1 - p) * (ctrl[0] - p0[0]) + 2 * p * (p1[0] - ctrl[0])
    dy = 2 * (1 - p) * (ctrl[1] - p0[1]) + 2 * p * (p1[1] - ctrl[1])
    rot = -math.degrees(math.atan2(dy, abs(dx) + 1e-6)) * 0.35 if p < 1 else 0
    flap = 1 + 0.08 * math.sin(c.t * 10) if p < 1 else 1 + 0.03 * math.sin(c.t * 3)
    y += 10 * math.sin(c.t * 2.1)
    c.put(spr, x, y, rot, s, s * flap, 1, 4.5)


TITLE_COLS = [CORAL, MUSTARD, TEAL, ORANGE, PLUM, CORAL, BRAND, BRAND]


def title_tiles(c, y, t0, pops=None):
    for i, ch in enumerate('MobileGL'):
        tt = t0 + 0.16 * i
        if c.t < tt:
            continue
        p = prog(c.t, tt, 0.65)
        yy = y - 560 * (1 - ease_back(p, 1.6)) + 3 * math.sin(c.t * 2 + i)
        rot = (3.5 if i % 2 else -3.5) + (1 - ease_out(p)) * (25 if i % 2 else -25)
        c.put(spr_tile(ch, TITLE_COLS[i]), 960 - 172 * 3.5 + 172 * i, yy, rot, 1, None, 1,
              1 + 3 * (1 - ease_out(p)))


# ================================================================ scene 1: title
def s_title(c):
    confetti(c, 11, 0.4)
    title_tiles(c, 400, 0.3)
    c.unfold(spr_label('a desktop OpenGL implementation', ('fredoka', 50, 'Medium'), WHITE, INK,
                       padx=40), 960, 612, 2.4, rot=-1)
    crane_fly(c, spr_crane(CORAL), 1.2, 4.4, (-260, 560), (1650, 190), (760, -40))


# ================================================================ scene 2: the problem
def s_problem(c):
    c.pop(spr_appwin(), 560, 460, 0.4, rot=-2)
    c.pop(spr_bubble('glDrawElements(…)'), 700, 178, 2.0, rot=2)
    c.pop(spr_bubble('glUniform4f(…)'), 360, 205, 3.0, rot=-3)
    c.pop(spr_phone(BG_PEACH), 1320, 480, 6.3, rot=3)
    c.pop(spr_tablet(BG_MINT), 1665, 510, 6.8, rot=-2)
    c.pop(spr_bubble('OpenGL ES', ('fredoka', 42, 'SemiBold'), TEAL, WHITE), 1360, 175, 8.0, rot=-2)
    c.pop(spr_bubble('Vulkan', ('fredoka', 42, 'SemiBold'), PLUM, WHITE, 'br'), 1680, 270, 8.6, rot=2)
    gap = 6 * math.sin(max(0, c.t - 12.6) * 2.4) if c.t > 12.6 else 0
    c.pop(spr_arrow_half('L'), 938 - gap, 470, 12.0, rot=2)
    c.pop(spr_arrow_half('R'), 1122 + gap, 470, 12.2, rot=-3)
    c.pop(spr_qmark(), 1030, 345, 13.0, rot=8)


# ================================================================ scene 3: meet MobileGL
def s_meet(c):
    c.pop(spr_slab('Your app', 760, CORAL), 960, 190, 0.5)
    c.pop(spr_slab('GPU', 760, DARK), 960, 790, 1.0)
    c.pop(spr_slab('OpenGL ES driver', 420, TEAL, size=40), 742, 625, 1.4)
    c.pop(spr_slab('Vulkan driver', 420, PLUM, size=40), 1178, 625, 1.6)
    c.slide(spr_slab('MobileGL', 900, BRAND, h=130, size=66), (2500, 405), (960, 405), 3.6, d=1.0,
            rot0=-8, rot=0, elev=1.2)
    if c.t > 4.6:
        cr = spr_crane(MUSTARD)
        c.pop(cr, 640, 395, 4.6, s=0.45, rot=-6, elev=1.5)
    for i, (x, y, t0) in enumerate([(960, 300, 5.2), (742, 522, 5.5), (1178, 522, 5.7),
                                    (742, 710, 5.9), (1178, 710, 6.1)]):
        c.pop(spr_arrow(70, INK, 16, 36), x, y, t0, rot=-90, elev=0.5)
    c.pop(spr_sticker('OpenGL\n4.6 Core', MUSTARD, r=92, size=32, fg=INK), 1480, 200, 6.4, rot=12)
    c.pop(spr_sticker('no app\nchanges', TEAL, r=82, size=30), 450, 405, 17.3, rot=-10)


# ================================================================ scene 4: why it works
KNOBS = [(110, 150, 'blend'), (250, 150, 'depth'), (390, 150, 'cull'), (530, 150, 'stencil')]
TOGGLES = [(110, 330, 'texture 0'), (250, 330, 'program'), (390, 330, 'VAO'), (530, 330, 'viewport')]
BOARD_STATES = [  # (time, knob angles, toggles on)
    (0.0, (30, -40, 90, 0), (1, 0, 1, 1)),
    (13.4, (-60, -40, 90, 0), (1, 0, 1, 1)),
    (14.4, (-60, 50, 90, 0), (1, 1, 1, 1)),
    (15.4, (-60, 50, 90, 0), (0, 1, 1, 1)),
    (19.6, (100, 50, 90, 0), (0, 1, 1, 1)),
    (21.6, (100, 50, -20, 120), (1, 1, 0, 1)),
]


@cache
def spr_board():
    a = Art(640, 440, 'board')
    a.rrect(0, 0, 640, 440, 24, NAVY)
    a.rrect(16, 16, 608, 408, 16, shade(NAVY, 1.18), shade=False, rough=0.6)
    for x, y, lab in KNOBS:
        a.circle(x, y, 48, shade(NAVY, 0.8), shade=False, rough=0.5)
        a.text(x, y + 72, lab, ('hand', 30), WHITE)
    for x, y, lab in TOGGLES:
        a.rrect(x - 20, y - 46, 40, 92, 20, shade(NAVY, 0.7), shade=False, rough=0.4)
        a.text(x, y + 72, lab, ('hand', 28), WHITE)
    return a.build()


@cache
def spr_knob():
    a = Art(80, 80, 'knob')
    a.circle(40, 40, 38, MUSTARD)
    a.rrect(35, 6, 10, 34, 5, DARK, shade=False, rough=0.3)
    return a.build(pad=18)


@cache
def spr_lever(on):
    a = Art(40, 40, f'lever{on}')
    a.circle(20, 20, 19, CORAL if on else WHITE)
    return a.build(pad=16)


@cache
def spr_recipe(i):
    items = [['shaders: A + B', 'blend: off', 'depth: less', 'vertex format #2'],
             ['shaders: A + B', 'blend: on', 'depth: always', 'vertex format #2'],
             ['shaders: C + B', 'blend: on', 'cull: off', 'vertex format #5']][i]
    a = Art(360, 250, f'recipe{i}')
    a.rrect(0, 0, 360, 250, 10, WHITE)
    a.rrect(0, 0, 360, 58, 10, [TEAL, PLUM, ORANGE][i], shade=False)
    a.text(180, 29, f'Draw recipe #{i + 1}', ('fredoka', 30, 'SemiBold'), WHITE)
    a.text(30, 76, '\n'.join('• ' + T(s) for s in items), ('hand', 34), INK, h='l', v='t',
           align='left', spacing=2)
    return a.build()


@cache
def spr_ingredient(kind):
    a = Art(250, 280, f'ingr|{kind}')
    a.rrect(0, 0, 250, 280, 16, WHITE)
    cx, cy = 125, 115
    if kind == 'buffers':
        for k, col in enumerate([shade(BLUE, 0.8), BLUE, shade(BLUE, 1.25)]):
            a.rrect(45 + k * 8, 60 + k * 34, 150, 52, 8, col)
        a.text(cx + 12, 160, '01101', ('mono', 26), WHITE)
    elif kind == 'textures':
        for i in range(4):
            for j in range(4):
                if (i + j) % 2 == 0:
                    a.rrect(53 + i * 36, 43 + j * 36, 36, 36, 2, CORAL, rough=0.4, shade=False)
                else:
                    a.rrect(53 + i * 36, 43 + j * 36, 36, 36, 2, MUSTARD, rough=0.4, shade=False)
    elif kind == 'shaders':
        a.rrect(50, 44, 150, 150, 8, PAPER)
        a.rrect(42, 36, 166, 22, 10, shade(PAPER, 0.85))
        a.rrect(42, 182, 166, 22, 10, shade(PAPER, 0.85))
        a.text(66, 72, 'void main()\n{\n  color =\n   …;\n}', ('mono', 18), INK, h='l', v='t',
               align='left', spacing=3)
    else:
        a.circle(85, cy, 46, NAVY)
        a.circle(85, cy, 32, MUSTARD)
        a.rrect(80, cy - 30, 10, 28, 4, DARK, shade=False)
        a.rrect(155, cy - 52, 40, 104, 20, NAVY)
        a.circle(175, cy - 28, 17, CORAL)
    a.text(cx, 238, kind, ('fredoka', 38, 'SemiBold'), INK)
    return a.build()


def board_state(t):
    st = BOARD_STATES[0]
    prev = st
    for s in BOARD_STATES:
        if t >= s[0]:
            prev, st = st, s
    p = ease_back(prog(t, st[0], 0.45), 2.0) if st is not BOARD_STATES[0] else 1
    knobs = [lerp(a, b, p) for a, b in zip(prev[1], st[1])]
    return knobs, st[2]


def s_why(c):
    t = c.t
    out1 = 11.0
    c.pop(spr_label('OpenGL', ('fredoka', 46, 'SemiBold'), BRAND, WHITE), 560, 250, 1.0, rot=-3, out=out1)
    c.pop(spr_label('OpenGL ES', ('fredoka', 46, 'SemiBold'), TEAL, WHITE), 960, 235, 1.6, rot=2, out=out1 + .1)
    c.pop(spr_label('Vulkan', ('fredoka', 46, 'SemiBold'), PLUM, WHITE), 1360, 250, 2.2, rot=-2, out=out1 + .2)
    for i, kind in enumerate(['buffers', 'textures', 'shaders', 'state']):
        c.pop(spr_ingredient(kind), 480 + 320 * i, 560, 6.3 + 0.45 * i, rot=(-3, 2, -2, 3)[i],
              out=out1 + 0.1 * i)
    if t < 11.3:
        return
    BX, BY = 470, 520
    RX, RY = 1450, 540
    c.pop(spr_label('OpenGL: one big switchboard', ('fredoka', 38, 'SemiBold'), WHITE, INK),
          BX, 215, 11.6, rot=-1.5)
    c.pop(spr_label('Explicit recipes', ('fredoka', 38, 'SemiBold'), WHITE, INK), RX, 215, 17.6, rot=1.5)
    if c.pop(spr_board(), BX, BY, 11.4, rot=-1):
        knobs, togg = board_state(t)
        bp = prog(t, 11.4, 0.55)
        sc = 0.2 + 0.8 * ease_back(bp)
        for (x, y, _), ang in zip(KNOBS, knobs):
            c.put(spr_knob(), BX + (x - 320) * sc, BY + (y - 220) * sc, -ang - 1, sc, None, 1, 0.5)
        for (x, y, _), on in zip(TOGGLES, togg):
            c.put(spr_lever(on), BX + (x - 320) * sc, BY + (y - 220 + (-24 if on else 24)) * sc, 0, sc,
                  None, 1, 0.5)
    snaps = [18.3, 20.3, 22.3]
    c.pop(spr_slab('MobileGL', 300, BRAND, h=80, size=44), 960, 330, 17.6, rot=-3)
    for i, ts in enumerate(snaps):
        if t >= ts:
            fp = prog(t, ts, 0.35)
            if fp < 1:
                c.put(spr_star(WHITE, 90), BX, BY, fp * 40, 0.4 + 1.6 * fp, None, 1 - fp, 0, shadow=False)
            dst = (RX + (-30, 20, -10)[i], RY + 70 - 70 * i)
            c.slide(spr_recipe(i), (BX, BY), dst, ts + 0.15, d=1.1, rot=(-4, 3, -1)[i], rot0=20,
                    s=1, s0=0.3, arc=220, elev=1 + i * 0.3)


# ================================================================ scene 5: frontend
NB = (1010, 470)
TABS = [('Textures', CORAL), ('Uniforms', MUSTARD), ('Blend', TEAL), ('Vertex arrays', PLUM)]
SLIPS = [(2.0, 'glBindTexture(GL_TEXTURE_2D, 7)', 0), (3.6, 'glUniform4f(tint, 1, .5, 0, 1)', 1),
         (5.2, 'glEnable(GL_BLEND)', 2), (6.8, 'glBindVertexArray(3)', 3),
         (8.4, 'glDrawElements(GL_TRIANGLES, …)', None)]


@cache
def spr_notebook():
    a = Art(640, 450, 'notebook')
    a.rrect(0, 0, 640, 450, 18, BRAND)
    a.rrect(22, 18, 292, 414, 8, PAPER, rough=0.8)
    a.rrect(326, 18, 292, 414, 8, PAPER, rough=0.8)
    ls = []
    for y in range(110, 420, 44):
        ls += [[(42, y), (296, y)], [(346, y), (598, y)]]
    a.lines(ls, 2, (170, 200, 228), caps=False)
    a.lines([[(320, 26), (320, 424)]], 5, shade(BRAND, 0.7), caps=False)
    a.text(168, 60, 'GL state', ('hand', 38), shade(BRAND, 1.0))
    a.text(472, 60, 'draws', ('hand', 38), shade(BRAND, 1.0))
    return a.build()


@cache
def spr_nbtab(text, color):
    a = Art(230, 62, f'nbtab|{text}')
    a.rrect(0, 0, 230, 62, 14, color)
    a.text(142, 31, text, ('fredoka', 26, 'SemiBold'), WHITE)
    return a.build()


@cache
def spr_slip(text):
    return spr_label(text, ('mono', 30), WHITE, INK, padx=22, pady=14, r=4)


@cache
def spr_stamp(text):
    font = ('fredoka', 44, 'Bold')
    tw, th = measure(text, font)
    w, h = tw + 56, th + 44
    a = Art(w, h, 'stamp|' + text)
    rp = rrect_pts(0, 0, w, h, 10)
    a.lines([rp + [rp[0]]], 7, RED, tex=0.25, opacity=0.9)
    a.text(w / 2, h / 2, text, font, RED, tex=0.25, opacity=0.9)
    return a.build()


def s_frontend(c):
    t = c.t
    APP = (190, 470)
    c.pop(spr_appwin(), APP[0], APP[1], 0.3, s=0.42, rot=-3)
    nb_on = t >= 0.6
    lifts = [0] * 4
    for ts, txt, tab in SLIPS:
        if tab is not None:
            lifts[tab] = 26 * bump(t, ts + 1.1, 1.3)
    if nb_on:
        for i, (name, col) in enumerate(TABS):
            c.pop(spr_nbtab(name, col), NB[0] + 388 + lifts[i], NB[1] - 150 + 92 * i, 0.9 + 0.12 * i,
                  elev=0.6 + lifts[i] / 20)
    c.pop(spr_notebook(), NB[0], NB[1], 0.6, rot=0)
    rows = [NB[1] - 225 + y for y in (132, 220, 308, 396)]
    for k, (ts, txt, tab) in enumerate(SLIPS):
        if tab is not None:
            dst = (NB[0] - 152, rows[tab] - 10)
        else:
            dst = (NB[0] + 152, NB[1] - 225 + 132)
        c.slide(spr_slip(txt), APP, dst, ts, d=1.1, s=0.45, s0=0.8, rot=(-2 if k % 2 else 2), rot0=-10,
                arc=160, elev=0.3)
    # the bad call
    bad = spr_slip('glBindTexture(GL_TEXTURE_2D, 999)')
    if t >= 12.8:
        if t < 15.6:
            c.slide(bad, APP, (520, 770), 12.8, d=1.0, s=0.62, s0=0.6, rot=4, rot0=-8, arc=160)
            sx, sy = 520, 770
        else:
            p = prog(t, 15.6, 1.2)
            sx, sy = lerp(520, 60, ease_in(p)), 770 + 420 * ease_in(p)
            c.put(bad, sx, sy, 4 + 40 * p, 0.62, None, 1 - p * 0.3, 2)
        if t >= 14.1:
            sp = prog(t, 14.1, 0.25)
            c.put(spr_stamp('GL_INVALID_OPERATION'), sx + 20, sy + 4, -8, 0.55 * (1.8 - 0.8 * ease_out(sp)),
                  None, min(1, sp * 3) * (1 - prog(t, 16.4, 0.4)), 0, shadow=False)
    if t >= 18.5:
        bob = 6 * math.sin(t * 2)
        c.pop(spr_gpu(170), 1700, 790 + bob, 18.7, rot=4)
        for k in range(3):
            ph = (t * 0.7 + k / 3) % 1
            c.put(spr_ink('z', ('fredoka', 40 + 12 * k, 'Bold'), BRAND),
                  1780 + 50 * ph + 10 * k, 700 - 110 * ph - 10 * k, 10, 0.6 + 0.6 * ph, None,
                  min(1, (1 - ph) * 3), 0.5)


# ================================================================ scene 6: shaders
GLSL = ['#version 460 core', 'in vec3 pos;', 'uniform mat4 mvp;', '', 'void main() {',
        '  gl_Position =', '    mvp * vec4(pos, 1);', '}']
GLSLES = ['#version 320 es', 'precision highp', '        float;', 'in vec3 pos;', 'uniform …;',
          'void main() {', '  gl_Position = …;', '}']


@cache
def spr_scroll(title, lines, color=PAPER, tcol=BRAND):
    n = len(lines)
    w, h = 340, 130 + n * 31
    a = Art(w, h, f'scroll|{title}')
    a.rrect(12, 18, w - 24, h - 36, 4, color)
    a.rrect(0, 0, w, 36, 18, shade(color, 0.86))
    a.rrect(0, h - 36, w, 36, 18, shade(color, 0.86))
    a.text(w / 2, 66, title, ('fredoka', 34, 'SemiBold'), tcol)
    a.text(34, 96, '\n'.join(lines), ('mono', 21), INK, h='l', v='t', align='left', spacing=10)
    return a.build()


@cache
def spr_machine(name, color, w=360, h=260):
    a = Art(w, h + 70, f'machine|{name}|{color}')
    a.poly([(28, 0), (168, 0), (128, 76), (68, 76)], shade(color, 0.8))
    a.rrect(0, 64, w, h, 24, color)
    a.rrect(w - 10, 170, 26, 100, 6, DARK)
    a.rrect(30, h - 10, w - 60, 66, 12, WHITE)
    a.text(w / 2, h + 23, name, ('fredoka', 40, 'SemiBold'), INK)
    return a.build()


def s_shaders(c):
    t = c.t
    MX, MY = 900, 450
    if t < 3.0:
        c.pop(spr_scroll('GLSL', tuple(GLSL)), 300, 440, 0.4, rot=-3)
    else:
        p = prog(t, 3.0, 1.3)
        if p < 1:
            c.put(spr_scroll('GLSL', tuple(GLSL)), lerp(300, MX - 82, ease_io(p)),
                  lerp(440, MY - 205, ease_io(p)) - 120 * math.sin(math.pi * p), -3 + 20 * p,
                  1 - 0.85 * ease_in(p), None, 1, 1 + 2 * math.sin(math.pi * p))
    busy = 3.6 < t < 6.4
    spin = (t * (220 if busy else 30))
    if c.pop(spr_machine('glslang', MUSTARD), MX, MY, 1.0):
        shake = 2.5 * math.sin(t * 40) if busy else 0
        c.put(spr_gear(46, CORAL), MX - 60 + shake, MY - 30, spin, 1, None, 1, 0.5)
        c.put(spr_gear(34, TEAL), MX + 28 + shake, MY - 50, -spin * 1.35 + 18, 1, None, 1, 0.5)
    crane = spr_crane(BLUE)
    if t >= 5.8:
        if t < 12.6:
            c.slide(crane, (MX + 190, MY + 40), (1250, 420), 5.8, d=0.9, s=0.8, s0=0.2, arc=120, elev=1.5)
            c.pop(spr_label('SPIR-V', ('fredoka', 40, 'SemiBold'), BLUE, WHITE), 1250, 560, 6.4, rot=-2,
                  out=12.4)
        else:
            c.slide(crane, (1250, 420), (1530, 240), 12.6, d=1.0, s=0.7, s0=0.8, arc=60, elev=1.5)
            c.slide(crane, (1250, 420), (1380, 680), 12.6, d=1.0, s=0.3, s0=0.8, arc=40, elev=1.0,
                    out=13.6, od=0.3)
    if t >= 12.6:
        c.pop(spr_label('Magma (Vulkan): uses it as-is', ('fredoka', 36, 'SemiBold'), PLUM, WHITE), 1560, 380,
              13.4, rot=2)
        if c.pop(spr_machine('SPIRV-Cross', TEAL, w=330, h=200), 1380, 700, 12.4, s=0.62):
            c.put(spr_gear(34, MUSTARD), 1380 - 40, 700 - 34, t * 160, 0.62, None, 1, 0.5)
        c.pop(spr_scroll('GLSL ES', tuple(GLSLES), tcol=TEAL), 1730, 620, 14.6, s=0.78, rot=3)
        c.pop(spr_label('Espryt (OpenGL ES): GLSL ES', ('fredoka', 34, 'SemiBold'), TEAL, WHITE), 1560, 862,
              15.0, rot=-2)


# ================================================================ scene 7: records
BELT_Y = 560
BX0, BX1 = 330, 1590


@cache
def spr_belt():
    a = Art(1320, 64, 'belt')
    a.rrect(0, 0, 1320, 64, 32, DARK)
    a.rrect(14, 12, 1292, 16, 8, shade(DARK, 1.35), shade=False, rough=0.5)
    return a.build()


@cache
def spr_roller():
    a = Art(40, 40, 'roller')
    a.circle(20, 20, 19, GREY)
    a.lines([[(20, 4), (20, 36)]], 4, DARK, shade=False)
    return a.build(pad=14)


@cache
def spr_record(text, color):
    tw, th = measure(text, ('hand', 34))
    w, h = tw + 60, 66
    a = Art(w, h, f'record|{text}|{color}')
    a.rrect(0, 0, w, h, 8, WHITE)
    a.rrect(0, 0, 20, h, 6, color, shade=False)
    a.text(20 + (w - 20) / 2, h / 2, text, ('hand', 34), INK)
    return a.build()


@cache
def spr_box(name, color, w=230, h=170, size=40):
    a = Art(w, h, f'box|{name}|{color}|{w}')
    a.rrect(0, 0, w, h, 20, color)
    a.rrect(16, 16, w - 32, h - 32, 12, shade(color, 1.12), shade=False, rough=0.5)
    a.text(w / 2, h / 2, name, ('fredoka', size, 'SemiBold'), WHITE)
    return a.build()


def ride(c, spr, ts, y=BELT_Y - 62, v=270, extra=None):
    """Card spawned at ts from the frontend, rides the belt into the backend."""
    if c.t < ts:
        return None
    x = BX0 + v * (c.t - ts)
    if x > BX1:
        p = prog(x, BX1, 110)
        if p >= 1:
            return None
        c.put(spr, x, y - 30 * p, 0, 1 - 0.8 * p, None, 1 - p, 1)
        return None
    pp = prog(c.t, ts, 0.35)
    c.put(spr, x, y - 60 * (1 - ease_back(pp)), 0, 0.6 + 0.4 * ease_out(pp), None, 1, 1)
    return x


def s_records(c):
    t = c.t
    c.pop(spr_belt(), (BX0 + BX1) / 2, BELT_Y, 0.4, elev=0.6)
    if t >= 0.6:
        for k in range(11):
            c.put(spr_roller(), BX0 - 10 + k * 128, BELT_Y, -t * 260 + k * 30, 0.9, None, 1, 0.3)
    c.pop(spr_box('Frontend', BRAND), 190, 520, 0.6, rot=-2)
    c.pop(spr_house(TEAL, 300, 280), 1745, 470, 0.8)
    c.pop(spr_label('Backend', ('fredoka', 36, 'SemiBold'), TEAL, WHITE), 1745, 660, 1.0, rot=2)
    seq = [(1.6, spr_record('texture 7 on unit 0', CORAL)), (2.6, spr_record('blend: on', TEAL)),
           (3.6, spr_record('viewport 1920×1080', MUSTARD)),
           (4.7, spr_label('DRAW', ('fredoka', 44, 'Bold'), ORANGE, WHITE, padx=36)),
           (8.6, spr_record('uniform tint', MUSTARD)),
           (9.7, spr_label('DRAW', ('fredoka', 44, 'Bold'), ORANGE, WHITE, padx=36)),
           (24.0, spr_record('texture 12 on unit 1', CORAL)),
           (25.0, spr_label('DRAW', ('fredoka', 44, 'Bold'), ORANGE, WHITE, padx=36))]
    for ts, spr in seq:
        ride(c, spr, ts)
    c.pop(spr_sticker('only\nchanges', CORAL, r=78, size=30), 760, 300, 9.0, rot=-8, out=17.8)
    # handle tag riding with a new object
    x = ride(c, spr_record('new texture', BLUE), 12.4)
    if x is not None:
        c.put(spr_label('handle {slot 12, gen 3}', ('mono', 24), MUSTARD, INK, padx=14, pady=8, r=6),
              x + 10, BELT_Y + 70, -3, 1, None, 1, 0.8)
    c.pop(spr_sticker('no\nwaiting', BLUE, r=74, size=30), 1210, 300, 13.4, rot=8, out=17.8)
    # dedupe: two identical blocks merge
    blk = spr_record('blend on · depth less', PLUM)
    MP = (700, 360)
    if 18.4 <= t < 21.2:
        p = ease_io(prog(t, 18.5, 1.2))
        if p < 1:
            c.put(blk, lerp(450, MP[0], p), lerp(250, MP[1], p), lerp(-8, 0, p), 1, None, 1, 2)
            c.put(blk, lerp(980, MP[0], p), lerp(440, MP[1], p), lerp(7, 0, p), 1, None, 1, 2)
        else:
            q = ease_io(prog(t, 20.3, 0.9))
            c.put(blk, lerp(MP[0], BX0, q), lerp(MP[1], BELT_Y - 62, q) - 80 * math.sin(math.pi * q), 0, 1,
                  None, 1, 2)
    if 19.6 <= t < 20.4:
        sp = prog(t, 19.6, 0.8)
        c.put(spr_star(MUSTARD, 46), MP[0] + 150, MP[1] - 40, sp * 90, 0.4 + 0.9 * math.sin(math.pi * sp),
              None, 1, 1)
    if t >= 18.4:
        c.pop(spr_sticker('sent\nonce', MUSTARD, r=74, size=32, fg=INK), 1060, 280, 19.9, rot=10)
        ride(c, blk, 21.2)


# ================================================================ scene 8: backends
EH, MH = (560, 470), (1360, 470)


def s_backends(c):
    t = c.t
    c.pop(spr_house(CORAL), EH[0], EH[1], 0.4, rot=-1)
    c.pop(spr_house(TEAL), MH[0], MH[1], 0.7, rot=1)
    c.pop(spr_label('Espryt', ('fredoka', 52, 'SemiBold'), CORAL, WHITE), EH[0], 700, 6.2, rot=-2)
    c.pop(spr_label('speaks OpenGL ES', ('hand', 40), WHITE, INK, pady=8), EH[0], 790, 6.6, rot=1)
    c.pop(spr_label('Magma', ('fredoka', 52, 'SemiBold'), TEAL, WHITE), MH[0], 700, 7.6, rot=2)
    c.pop(spr_label('speaks Vulkan', ('hand', 40), WHITE, INK, pady=8), MH[0], 790, 8.0, rot=-1)
    c.pop(spr_gpu(150), 960, 540, 1.2, rot=-3)
    which = 0 if t < 9.5 or (13 < t < 16.5) else 1
    if t >= 9.0:
        val = ['DirectGLES', 'DirectVulkan'][which]
        k1 = spr_label('MOBILEGL_BACKEND_TYPE=', ('mono', 34), DARK, WHITE, padx=24, pady=14, r=6)
        k2 = spr_label(val, ('mono', 34), [CORAL, TEAL][which], WHITE, padx=20, pady=14, r=6)
        i1, i2 = k1.w - 60, k2.w - 60
        x0 = 960 - (i1 + i2) / 2
        c.pop(k1, x0 + i1 / 2, 175, 9.0)
        c.put(k2, x0 + i1 + i2 / 2 + 8, 175, -2 if which else 2, 1, None, min(1, (t - 9.0) * 4), 1.6)
    # records rain into the active house
    active = (EH, MH)[which] if t >= 9.0 else (EH if t < 4.8 else MH)
    for k in range(40):
        ts = 1.6 + k * 0.55
        if not (ts <= t < ts + 1.0):
            continue
        tgt = (EH, MH)[0 if t < 9.0 and ts < 4.8 else which] if t >= 9.0 or ts < 4.8 else MH
        p = prog(t, ts, 1.0)
        col = (CORAL, TEAL, MUSTARD, PLUM)[k % 4]
        c.put(spr_confetti(2, col, 34), lerp(960, tgt[0], ease_io(p)), lerp(260, tgt[1] + 40, ease_io(p))
              - 120 * math.sin(math.pi * p), p * 200, 1 - 0.4 * p, None, 1 - ease_in(p), 1.5)
    # chimney puffs of the active house
    for k in range(4):
        ph = (t * 0.6 + k / 4) % 1
        hx, hy = active[0] + HOUSE_CHIMNEY[0], active[1] + HOUSE_CHIMNEY[1]
        if t > 2:
            c.put(spr_cloud(26), hx + 40 * ph, hy - 20 - 140 * ph, 0, 0.5 + 0.8 * ph, None,
                  min(1, ph * 4) * (1 - ph), 0.4)
    if t >= 12.0:
        c.pop(spr_sticker('fully\nseparate', MUSTARD, r=80, size=30, fg=INK), 960, 330, 12.2, rot=-6)


# ================================================================ scene 9: anywhere
PANEL_X = [285, 735, 1185, 1635]
PANEL_Y = 470


@cache
def spr_fb(name, color, w=140):
    a = Art(w, 58, f'fb|{name}|{color}|{w}')
    a.rrect(0, 0, w, 58, 14, color)
    a.text(w / 2, 29, name, ('fredoka', 26, 'SemiBold'), WHITE)
    return a.build()


def _wavy(x, y0, y1, amp=7, ph=0.0):
    return [(x + amp * math.sin(y / 22 + ph), y) for y in range(int(y0), int(y1) + 1, 6)]


def _dashed(x, y, w, h, r=16, dash=14):
    pts = rrect_pts(x, y, w, h, r)
    pts.append(pts[0])
    out, acc, cur = [], 0.0, []
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        L = math.hypot(x1 - x0, y1 - y0)
        n = max(1, int(L / 3))
        for k in range(n):
            px, py = x0 + (x1 - x0) * k / n, y0 + (y1 - y0) * k / n
            on = int(acc / dash) % 2 == 0
            if on:
                cur.append((px, py))
            elif cur:
                out.append(cur)
                cur = []
            acc += L / n
    if cur:
        out.append(cur)
    return [s for s in out if len(s) > 1]


@cache
def spr_panel(i):
    titles = ['One thread', 'Two threads', 'Two processes', 'Two machines']
    notes = ['monolith', 'inproc', 'spawn + shared memory', 'tcp://host:port']
    a = Art(400, 440, f'panel{i}')
    a.rrect(0, 0, 400, 440, 18, WHITE)
    a.text(200, 44, titles[i], ('fredoka', 38, 'SemiBold'), INK)
    a.text(200, 410, notes[i], ('mono', 22) if i == 3 else ('hand', 30), GREY)
    if i == 0:
        a.lines([_wavy(200, 92, 370)], 6, CORAL, shade=True)
    elif i == 1:
        a.lines(_dashed(26, 90, 348, 290), 3, GREY, caps=False)
        a.lines([_wavy(115, 100, 368), _wavy(285, 100, 368, ph=2)], 6, CORAL, shade=True)
    elif i == 2:
        a.lines(_dashed(18, 90, 168, 200) + _dashed(214, 90, 168, 200), 3, GREY, caps=False)
    return a.build()


@cache
def spr_ring():
    a = Art(130, 130, 'ring')
    a.circle(65, 65, 62, DARK)
    for k in range(8):
        ang = 2 * math.pi * k / 8
        a.poly(rot_rect_pts(65 + 46 * math.cos(ang), 65 + 46 * math.sin(ang), 22, 18, math.degrees(ang)),
               (CORAL, MUSTARD, TEAL, BLUE)[k % 4], rough=0.3, shade=False)
    a.circle(65, 65, 30, WHITE)
    return a.build(pad=16)


def s_anywhere(c):
    t = c.t
    pts = [1.0, 6.0, 11.0, 16.0]
    F, B = spr_fb('frontend', BRAND), spr_fb('backend', TEAL)
    for i, (x, ts) in enumerate(zip(PANEL_X, pts)):
        if not c.pop(spr_panel(i), x, PANEL_Y, ts, rot=(-2, 1.5, -1, 2)[i]):
            continue
        ox, oy = x - 200, PANEL_Y - 220
        sc = 0.2 + 0.8 * ease_back(prog(t, ts, 0.55))
        if sc < 0.97:
            continue
        if i == 0:
            c.put(F, ox + 200, oy + 180, 0, 1, None, 1, 0.5)
            c.put(B, ox + 200, oy + 270, 0, 1, None, 1, 0.5)
        elif i == 1:
            c.put(spr_fb('frontend', BRAND, 130), ox + 115, oy + 200, 0, 1, None, 1, 0.5)
            c.put(spr_fb('backend', TEAL, 130), ox + 285, oy + 290, 0, 1, None, 1, 0.5)
            ph = (t * 0.9) % 1
            c.put(spr_confetti(2, MUSTARD, 28), lerp(ox + 130, ox + 270, ph), lerp(oy + 225, oy + 265, ph)
                  - 60 * math.sin(math.pi * ph), ph * 180, 1, None, min(1, (1 - ph) * 6, ph * 6), 1.2)
        elif i == 2:
            c.put(spr_fb('frontend', BRAND, 130), ox + 102, oy + 160, 0, 1, None, 1, 0.5)
            c.put(spr_fb('backend', TEAL, 130), ox + 298, oy + 230, 0, 1, None, 1, 0.5)
            c.put(spr_ring(), ox + 200, oy + 330, -t * 70, 0.85, None, 1, 0.8)
        else:
            c.put(spr_laptop(SKY), ox + 105, oy + 170, 0, 0.5, None, 1, 0.6)
            c.put(spr_phone(BG_PEACH, 230, 440), ox + 320, oy + 250, 4, 0.36, None, 1, 0.6)
            c.put(spr_label('client', ('hand', 30), BRAND, WHITE, padx=12, pady=4), ox + 105, oy + 250, -3,
                  1, None, 1, 0.5)
            c.put(spr_label('server', ('hand', 30), TEAL, WHITE, padx=12, pady=4), ox + 320, oy + 352, 3,
                  1, None, 1, 0.5)
            for k in range(2):
                ph = (t * 0.55 + k * 0.5) % 1
                px = lerp(ox + 140, ox + 300, ph)
                py = lerp(oy + 150, oy + 220, ph) - 70 * math.sin(math.pi * ph)
                c.put(spr_plane(MUSTARD if k else CORAL), px, py, -20 + 40 * ph, 0.45, None,
                      min(1, (1 - ph) * 6, ph * 6), 2.5)
    c.pop(spr_label('client = frontend, inside your app', ('fredoka', 34, 'SemiBold'), BRAND, WHITE),
          560, 790, 21.8, rot=-2)
    c.pop(spr_label('server = backend, next to the GPU', ('fredoka', 34, 'SemiBold'), TEAL, WHITE),
          1360, 800, 22.6, rot=2)


# ================================================================ scene 10: perks
@cache
def spr_perk(i):
    title, body, color = [
        ('Fast split', 'Two threads or processes can even beat one: both sides work at once.', MUSTARD),
        ('Run-ahead', 'Keep sending. Wait only for the answers you truly need.', CORAL),
        ('Zero-copy', 'Big buffers and window images are shared, not copied.', BLUE),
        ('Safe failure', "Server gone? Your app sees 'device lost', not a crash.", TEAL)][i]
    a = Art(410, 500, f'perk{i}')
    a.rrect(0, 0, 410, 500, 18, WHITE)
    a.rrect(0, 0, 410, 86, 18, color, shade=False)
    a.text(205, 44, title, ('fredoka', 42, 'SemiBold'), WHITE if i != 0 else INK)
    cx, cy = 205, 190
    if i == 0:
        a.lines([_wavy(cx - 50, cy - 70, cy + 70), _wavy(cx + 50, cy - 70, cy + 70, ph=2)], 7, CORAL, shade=True)
        a.rrect(cx - 50 - 60, cy - 22, 120, 44, 12, BRAND)
        a.rrect(cx + 50 - 60, cy + 4, 120, 44, 12, TEAL)
    elif i == 1:
        for k in range(3):
            px, py, sc = cx - 110 + 85 * k, cy + 40 - 32 * k, 0.55
            a.poly([(px, py), (px + 170 * sc, py + 30 * sc), (px + 52 * sc, py + 30 * sc)], shade(CORAL, 1.25), rough=0.3)
            a.poly([(px + 52 * sc, py + 30 * sc), (px + 170 * sc, py + 30 * sc), (px + 22 * sc, py + 70 * sc)], CORAL, rough=0.3)
    elif i == 2:
        a.rrect(cx - 150, cy - 70, 170, 130, 12, DARK)
        a.rrect(cx - 20, cy - 50, 170, 130, 12, DARK)
        a.rrect(cx - 60, cy - 34, 120, 90, 6, MUSTARD)
    else:
        a.poly([(cx - 70, cy - 80), (cx + 70, cy - 80), (cx + 70, cy), (cx, cy + 86), (cx - 70, cy)], TEAL)
        a.lines([[(cx - 32, cy - 6), (cx - 6, cy + 22), (cx + 38, cy - 34)]], 14, WHITE, shade=True)
    a.text(205, 300, wrap(body, ('hand', 36), 350), ('hand', 36), INK, v='t', spacing=4)
    return a.build()


def s_perks(c):
    for i, ts in enumerate([0.8, 6.8, 12.8, 18.8]):
        bob = 4 * math.sin(c.t * 1.5 + i)
        lift = 1 if c.t < ts + 0.6 or c.t > ts + 5.8 else 1.8
        c.pop(spr_perk(i), 255 + 470 * i, 480 + bob, ts, rot=(-2, 1.5, -1.5, 2)[i], elev=lift)


# ================================================================ scene 11: platforms
@cache
def spr_platform(i):
    name, body, color = [
        ('Android', 'Its first home: Minecraft: Java Edition via launchers like FoldCraftLauncher.', TEAL),
        ('Linux', 'EGL, GLX, Wayland and glvnd. Even a whole KDE Plasma desktop.', MUSTARD),
        ('Windows', 'A drop-in opengl32.dll that speaks WGL.', BLUE),
        ('macOS', 'CGL and NSOpenGL, on Vulkan via MoltenVK.', PLUM)][i]
    a = Art(400, 560, f'plat{i}')
    a.rrect(0, 0, 400, 560, 18, WHITE)
    a.rrect(0, 0, 400, 230, 18, color, shade=False)
    cx, cy = 200, 118
    if i == 0:
        a.rrect(cx - 58, cy - 96, 116, 196, 18, DARK)
        a.rrect(cx - 48, cy - 76, 96, 150, 6, BG_SKY, shade=False)
        a.poly([(cx, cy - 40), (cx + 34, cy - 22), (cx, cy - 4), (cx - 34, cy - 22)], GREEN, rough=0.3)
        a.poly([(cx - 34, cy - 22), (cx, cy - 4), (cx, cy + 36), (cx - 34, cy + 18)], DIRT, rough=0.3)
        a.poly([(cx + 34, cy - 22), (cx, cy - 4), (cx, cy + 36), (cx + 34, cy + 18)], shade(DIRT, 0.75), rough=0.3)
    elif i == 1:
        a.rrect(cx - 120, cy - 78, 240, 160, 14, DARK)
        a.text(cx - 92, cy - 30, '$ glmark2\n> _', ('mono', 26), (140, 220, 140), h='l', v='t', align='left', spacing=10)
    elif i == 2:
        a.rrect(cx - 120, cy - 80, 240, 164, 12, WHITE)
        a.rrect(cx - 120, cy - 80, 240, 38, 12, DARK, shade=False)
        a.text(cx, cy + 22, 'opengl32.dll', ('mono', 26), INK)
    else:
        a.rrect(cx - 110, cy - 82, 220, 144, 12, DARK)
        a.rrect(cx - 98, cy - 70, 196, 120, 6, SKY, shade=False)
        a.poly([(cx - 140, cy + 66), (cx + 140, cy + 66), (cx + 124, cy + 88), (cx - 124, cy + 88)], GREY)
    a.text(200, 280, name, ('fredoka', 50, 'SemiBold'), INK)
    a.text(200, 330, wrap(body, ('hand', 34), 340), ('hand', 34), INK, v='t', spacing=4)
    return a.build()


def s_platforms(c):
    t = c.t
    for i, ts in enumerate([0.8, 6.8, 7.3, 7.8]):
        hl = bump(t, 12.6, 6.6) if i == 1 else 0
        bob = 4 * math.sin(t * 1.5 + i)
        c.pop(spr_platform(i), 285 + 450 * i, 480 + bob - 30 * hl, ts, rot=(-2, 1.5, -1.5, 2)[i] * (1 - hl),
              s=1 + 0.06 * hl, elev=1 + 2 * hl)
    if 12.9 <= t:
        c.pop(spr_sticker('KDE Plasma\non a tablet', CORAL, r=96, size=30), 950, 180, 13.2, rot=10,
              out=19.2)


# ================================================================ scene 12: outro
def s_outro(c):
    confetti(c, 23, 0.6, n=18, box=(300, 200, 1620, 900))
    title_tiles(c, 330, 0.3)
    c.unfold(spr_label('free & open source  ·  LGPL-3.0', ('fredoka', 46, 'Medium'), WHITE, INK, padx=40),
             960, 530, 2.4, rot=-1)
    c.unfold(spr_label('github.com/MobileGL-Dev/MobileGL', ('mono', 44), BRAND, WHITE, padx=36, pady=18),
             960, 645, 3.1, rot=1)
    crane_fly(c, spr_crane(MUSTARD), 1.0, 4.6, (2200, 600), (300, 170), (1100, -60))
    c.pop(spr_label('Come fold with us!', ('fredoka', 64, 'SemiBold'), CORAL, WHITE, padx=50, pady=22),
          960, 820, 20.9, rot=-2)


# ================================================================ timeline
SCENES = [
    dict(fn=s_title, dur=16, bg=BG_CREAM, tag=None, caps=[
        (4.6, 9.6, 'Run desktop OpenGL apps where there is no desktop OpenGL driver.'),
        (9.9, 15.0, "Let's unfold how it works. No deep internals, promise!")]),
    dict(fn=s_problem, dur=20, bg=BG_SKY, tag=('The problem', CORAL), caps=[
        (0.6, 5.8, 'Lots of great software speaks desktop OpenGL: games, creative tools, whole desktops.'),
        (6.1, 11.6, 'But phones and tablets usually ship only OpenGL ES or Vulkan drivers.'),
        (11.9, 19.0, "Same GPU, different language: the app's calls have nobody to answer them.")]),
    dict(fn=s_meet, dur=22, bg=BG_BUTTER, tag=('Meet MobileGL', BRAND), caps=[
        (0.5, 5.8, 'Meet MobileGL: a complete desktop OpenGL library that sits between your app and the driver.'),
        (6.1, 11.4, 'Your app sees OpenGL 4.6 Core, with just a few corners still being finished.'),
        (11.7, 16.8, 'Underneath, MobileGL speaks whatever the device understands: OpenGL ES or Vulkan.'),
        (17.1, 21.2, "Your app doesn't change. It simply loads MobileGL as its OpenGL library.")]),
    dict(fn=s_why, dur=28, bg=BG_MINT, tag=('Why it works', TEAL), caps=[
        (0.5, 5.8, 'Why can this work at all? Every modern GPU API is built from the same ingredients:'),
        (6.1, 10.9, 'buffers, textures, shaders, and state like blending and depth testing.'),
        (11.4, 17.0, 'OpenGL keeps that state in one big global switchboard you flip one switch at a time.'),
        (17.3, 22.8, 'At each draw, MobileGL snapshots the switches that matter into an explicit recipe…'),
        (23.1, 27.2, '…that a backend can replay on OpenGL ES or Vulkan.')]),
    dict(fn=s_frontend, dur=26, bg=BG_PEACH, tag=('Step 1 · The frontend', CORAL), caps=[
        (0.5, 5.8, 'Step 1: the frontend. Every GL call your app makes lands here first.'),
        (6.1, 11.8, 'It tracks every bind, every uniform, every switch, just like a desktop driver would.'),
        (12.1, 17.8, 'It checks each call against the spec, so mistakes become proper GL errors, not crashes.'),
        (18.1, 25.2, 'Nothing has touched the GPU yet. The frontend simply knows what the world should look like.')]),
    dict(fn=s_shaders, dur=24, bg=BG_LAV, tag=('Shaders get folded', PLUM), caps=[
        (0.5, 5.8, 'Shaders get folded too. Your GLSL is compiled once, by glslang…'),
        (6.1, 11.8, '…into SPIR-V: a compact, portable shader format.'),
        (12.1, 17.8, 'Vulkan takes SPIR-V as-is. For OpenGL ES, SPIRV-Cross unfolds it into GLSL ES.'),
        (18.1, 23.2, 'One shader, folded two ways, and your app never notices.')]),
    dict(fn=s_records, dur=28, bg=BG_CREAM, tag=('Step 2 · Records', ORANGE), caps=[
        (0.5, 5.8, 'Step 2: before each draw, the frontend sends the backend only what changed.'),
        (6.1, 11.8, 'Each change is a small record. The draw itself is a verb that uses them.'),
        (12.1, 17.8, 'New objects get their handles up front, so creating things never waits for a reply.'),
        (18.1, 23.6, 'Identical state blocks are shared instead of resent. Less to carry, less to check.'),
        (23.9, 27.2, "The backend never peeks into the frontend's memory. Records are all it gets.")]),
    dict(fn=s_backends, dur=24, bg=BG_SKY, tag=('Step 3 · Backends', TEAL), caps=[
        (0.5, 5.8, 'Step 3: a backend turns records into real driver calls.'),
        (6.1, 11.8, 'Espryt speaks OpenGL ES. Magma speaks Vulkan. You pick one at startup.'),
        (12.1, 17.8, 'Each backend is fully independent, so one can improve without disturbing the other.'),
        (18.1, 23.2, 'Same app, same frontend. Choose whatever suits the device best.')]),
    dict(fn=s_anywhere, dur=28, bg=BG_BUTTER, tag=('Records can travel', BRAND), caps=[
        (0.5, 5.6, "Now the fun part: records are just data, so they don't care where the backend lives."),
        (5.9, 10.6, 'Run everything on one thread, or give frontend and backend a thread each…'),
        (10.9, 15.6, '…put them in two processes that share a ring of memory…'),
        (15.9, 21.2, "…or even on two machines: a PC can drive a phone's GPU over the network."),
        (21.5, 27.2, 'The frontend side is the client. The backend side, next to the GPU, is the server.')]),
    dict(fn=s_perks, dur=24, bg=BG_MINT, tag=('Built for the trip', ORANGE), caps=[
        (0.5, 6.3, "Splitting up isn't slow. Under heavy load it can even be faster than one thread."),
        (6.6, 12.4, 'The client runs ahead and only waits when it really needs an answer back.'),
        (12.7, 18.4, 'On the same machine, big buffers and window images are shared, not copied.'),
        (18.7, 23.4, "And if the server goes away, your app sees 'device lost' instead of crashing.")]),
    dict(fn=s_platforms, dur=26, bg=BG_LAV, tag=('Where it runs', PLUM), caps=[
        (0.5, 6.3, 'MobileGL grew up on Android: think Minecraft: Java Edition on a phone.'),
        (6.6, 12.4, 'It also runs on Linux, slips into Windows as opengl32.dll, and works on macOS.'),
        (12.7, 19.0, 'A favorite demo: a whole KDE Plasma desktop from a Linux container, rendered by MobileGL on an Android tablet.'),
        (19.3, 25.2, 'Same library everywhere. Only the window-system glue changes.')]),
    dict(fn=s_outro, dur=24, bg=BG_NAVY, tag=None, caps=[
        (4.2, 9.6, 'MobileGL is free, open source, and still growing.'),
        (9.9, 15.4, 'Build it, pick a backend, and point your favorite OpenGL app at it.'),
        (15.7, 20.6, "Something breaks? Tell us. That's the most useful thing you can do.")]),
]
TR = 0.8
STARTS = []
_acc = 0.0
for _s in SCENES:
    STARTS.append(_acc)
    _acc += _s['dur']
TOTAL = _acc
NFRAMES = int(TOTAL * FPS)


def render_frame(fi):
    t = fi / FPS
    i = max(k for k in range(len(SCENES)) if STARTS[k] <= t + 1e-9)
    sc = SCENES[i]
    lt = t - STARTS[i]
    frame = background(sc['bg']).copy()
    c = Ctx(frame, lt, fi, sc['dur'])
    sc['fn'](c)
    if sc['tag']:
        draw_tag(c, *sc['tag'])
    for k, (a, b, txt) in enumerate(sc['caps']):
        draw_caption(c, a, b, txt, k)
    if i + 1 < len(SCENES) and lt > sc['dur'] - TR:
        draw_wipe(frame, SCENES[i + 1]['bg'], prog(lt, sc['dur'] - TR, TR))
    out = frame.convert('RGB')
    fade = min(prog(t, 0, 0.7), 1 - prog(t, TOTAL - 1.6, 1.5))
    if fade < 1:
        out = Image.blend(Image.new('RGB', out.size, (0, 0, 0)), out, fade)
    return out


def _worker(fi):
    return render_frame(fi).tobytes()


def write_meta(path):
    caps, ev = [], []
    for st, sc in zip(STARTS, SCENES):
        for a, b, txt in sc['caps']:
            caps.append((st + a, st + b, T(txt)))
        ev.append(st + sc['dur'] - TR)
    json.dump(dict(total=TOTAL, starts=STARTS, wipes=ev[:-1],
                   pops=[0.3 + 0.16 * i for i in range(8)] + [STARTS[-1] + 0.3 + 0.16 * i for i in range(8)]),
              open(path + '.json', 'w'), indent=1)

    def ts(x):
        ms = int(round(x * 1000))
        return f'{ms // 3600000:02d}:{ms // 60000 % 60:02d}:{ms // 1000 % 60:02d},{ms % 1000:03d}'
    with open(path + '.srt', 'w', encoding='utf-8') as f:
        for n, (a, b, txt) in enumerate(caps, 1):
            f.write(f'{n}\n{ts(a)} --> {ts(b)}\n{txt}\n\n')


OUT = os.path.join(HERE, 'out' if LANG == 'en' else 'out_' + LANG)
SUFFIX = '' if LANG == 'en' else '_' + LANG

if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    if sys.argv[1] == 'stills':
        for s in sys.argv[2:]:
            t = float(s)
            render_frame(int(round(t * FPS))).save(os.path.join(OUT, f'still_{t:06.1f}.png'))
            print('still', t)
    elif sys.argv[1] == 'meta':
        write_meta(os.path.join(OUT, 'mobilegl_intro' + SUFFIX))
        print('total', TOTAL)
    elif sys.argv[1] == 'render':
        write_meta(os.path.join(OUT, 'mobilegl_intro' + SUFFIX))
        ff = sys.argv[2]
        dst = os.path.join(OUT, 'video_noaudio' + SUFFIX + '.mp4')
        p = subprocess.Popen([ff, '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                              '-s', f'{W}x{H}', '-r', str(FPS), '-i', '-', '-r', '24', '-c:v', 'libx264',
                              '-preset', 'slow', '-crf', '18', '-tune', 'animation', '-pix_fmt', 'yuv420p', dst],
                             stdin=subprocess.PIPE)
        t0 = time.time()
        with Pool(max(2, os.cpu_count() - 2)) as pool:
            for k, buf in enumerate(pool.imap(_worker, range(NFRAMES), chunksize=6)):
                p.stdin.write(buf)
                if k % 240 == 0:
                    print(f'{k}/{NFRAMES} {time.time() - t0:.0f}s', flush=True)
        p.stdin.close()
        p.wait()
        print('done', time.time() - t0)
