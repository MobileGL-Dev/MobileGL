"""Isometric pop illustrations (icon style: flat faces, chunky light outlines) and their screen contents."""
import math
from anim import *
from kit import Iso, COS30, SIN30, code_line, cubes, goo
from engine import LW, LH

from engine import hexc as _hex

FACE_DARK = (_hex('#2e6e2f'), _hex('#1d4a25'), _hex('#163a1d'))      # top, left, right on dark plates
FACE_LIGHT = (_hex('#d4f0c9'), _hex('#82cb70'), _hex('#55b441'))     # icon faces on light plates
OUTL = _hex('#62ba4b')


def screen_layer(E, name, w, h, painter, f):
    """Paint 2D content into an offscreen layer (logical w x h) and return the texture."""
    tex = E.push_target(name, w, h)
    painter(E, f, w, h)
    E.pop_target()
    return tex


def put_on_face(E, iso, tex, face, x, y, z, w, h, alpha=1.0):
    """Map a w x h (world units) texture onto an iso face whose content centre is (x, y, z)."""
    cx, cy = iso.p(x, y, z)
    s = iso.s
    if face == 'left':
        m = iso.mat_left(1.0)
        E.image(tex, cx, cy, w * s, h * s, mat=m, color=(alpha,) * 4)
    elif face == 'right':
        E.image(tex, cx, cy, w * s, h * s, mat=iso.mat_right(1.0), color=(alpha,) * 4)
    else:
        E.image(tex, cx, cy, w * s, h * s, mat=iso.mat_top(1.0), color=(alpha,) * 4)


# --- devices ---------------------------------------------------------------------------------------
def phone(E, iso, f, x=0.0, y=0.0, faces=FACE_DARK, screen=None, line=OUTL, sw=5.0):
    """Upright phone facing left-front; screen texture on the left face."""
    W_, T, H_ = 3.0, 0.35, 6.2
    iso.box(E, x - W_ / 2, y, 0, W_, T, H_, faces[0], faces[1], faces[2], line, sw)
    if screen is not None:
        put_on_face(E, iso, screen, 'left', x, y + T, H_ / 2, W_ - 0.35, H_ - 0.7)


def monitor(E, iso, f, x=0.0, y=0.0, faces=FACE_DARK, screen=None, line=OUTL, sw=5.0):
    W_, T, H_ = 8.0, 0.4, 4.8
    iso.box(E, x - 1.0, y - 1.0, 0, 2.0, 2.0, 0.25, faces[0], faces[1], faces[2], line, sw)    # foot
    iso.box(E, x - 0.25, y - 0.2, 0.25, 0.5, 0.35, 1.3, faces[0], faces[1], faces[2], line, sw)  # neck
    iso.box(E, x - W_ / 2, y, 1.5, W_, T, H_, faces[0], faces[1], faces[2], line, sw)
    if screen is not None:
        put_on_face(E, iso, screen, 'left', x, y + T, 1.5 + H_ / 2, W_ - 0.4, H_ - 0.4)


def laptop(E, iso, f, x=0.0, y=0.0, faces=FACE_DARK, screen=None, line=OUTL, sw=5.0, open_=1.0):
    W_, D, T = 6.4, 4.2, 0.3
    iso.box(E, x - W_ / 2, y - D, 0, W_, D, T, faces[0], faces[1], faces[2], line, sw)       # base
    # keyboard hint
    for r in range(4):
        for c in range(10):
            iso.quad_top(E, x - W_ / 2 + 0.4 + c * 0.56, y - D + 0.5 + r * 0.55, T + 0.001, 0.42, 0.4,
                         a(faces[2], 0.9))
    # lid (upright, back edge)
    H_ = 4.0 * open_
    iso.box(E, x - W_ / 2, y - D - T, T, W_, T, H_, faces[0], faces[1], faces[2], line, sw)
    if screen is not None and open_ > 0.6:
        put_on_face(E, iso, screen, 'left', x, y - D, T + H_ / 2, W_ - 0.4, H_ - 0.4)


def tower(E, iso, f, x=0.0, y=0.0, faces=FACE_DARK, line=OUTL, sw=5.0, leds=True):
    W_, D, H_ = 2.6, 4.0, 6.0
    iso.box(E, x - W_ / 2, y - D / 2, 0, W_, D, H_, faces[0], faces[1], faces[2], line, sw)
    if leds:
        for k in range(5):
            on = (int(f.t * 8) + k * 3) % 5 < 3
            px, py = iso.p(x - W_ / 2 + 0.4 + k * 0.4, y + D / 2, H_ - 0.6)
            E.circle(px, py, 5, NEON if on else a(GLUE, 0.3))
        for k in range(4):
            px0, py0 = iso.p(x - W_ / 2 + 0.3, y + D / 2, H_ - 1.4 - k * 0.9)
            px1, py1 = iso.p(x + W_ / 2 - 0.3, y + D / 2, H_ - 1.4 - k * 0.9)
            E.line(px0, py0, px1, py1, 5, a(line, 0.6))


def board(E, iso, f, x=0.0, y=0.0, faces=FACE_DARK, line=OUTL, sw=5.0):
    """Single-board computer (ARM Linux)."""
    iso.box(E, x - 3.0, y - 2.0, 0, 6.0, 4.0, 0.25, faces[1], faces[1], faces[2], line, sw)
    iso.box(E, x - 1.0, y - 1.0, 0.25, 2.0, 2.0, 0.35, _hex('#202a22'), _hex('#111611'), _hex('#0b0f0b'), line, 3)
    for k in range(10):
        iso.box(E, x - 2.7 + k * 0.5, y - 1.9, 0.25, 0.3, 0.3, 0.45, a(AMBER, 1), a(AMBER, 1), a(AMBER, 1))
    iso.box(E, x + 1.5, y + 0.2, 0.25, 1.3, 1.6, 0.9, PALE, faces[1], faces[2], line, 3)


def file_card(E, x, y, w, h, title, sub='', col=PALE, ink=INK, rot=0.0, s=1.0, fold=36):
    """Flat document icon with a folded corner."""
    c, sn = math.cos(rot), math.sin(rot)

    def P(px, py):
        px, py = px * s, py * s
        return (x + c * px - sn * py, y + sn * px + c * py)
    pts = [P(-w / 2, -h / 2), P(w / 2 - fold, -h / 2), P(w / 2, -h / 2 + fold), P(w / 2, h / 2), P(-w / 2, h / 2)]
    E.poly(pts, col, stroke=OUTL, sw=5 * s)
    E.poly([P(w / 2 - fold, -h / 2), P(w / 2 - fold, -h / 2 + fold), P(w / 2, -h / 2 + fold)], MINT, stroke=OUTL,
           sw=4 * s)
    tx, ty = P(0, h * 0.12)
    E.text(title, tx, ty, 34 * s, ink, font='mono', wght=800, rot=rot)
    if sub:
        sx, sy = P(0, h * 0.3)
        E.text(sub, sx, sy, 20 * s, a(ink, 0.6), font='mono', wght=500, rot=rot)


# --- screen painters -------------------------------------------------------------------------------
def paint_voxels(E, f, w, h):
    """A tiny blocky landscape (a sandbox game, generically)."""
    E.rect(w / 2, h / 2, w, h, _hex('#7fc8f0'))
    E.rect(w / 2, h * 0.86, w, h * 0.5, _hex('#5aa0d0'))
    iso = Iso(w / 2, h * 0.36, w / 15)
    hgt = {}
    for gx in range(-4, 5):
        for gy in range(-4, 5):
            hgt[(gx, gy)] = 1 + int(2.2 * (0.5 + 0.5 * math.sin(gx * 0.9 + 1.3) * math.cos(gy * 0.8 - 0.4)))
    order = sorted(hgt, key=lambda k: (k[0] + k[1]))
    for gx, gy in order:
        z = hgt[(gx, gy)]
        iso.box(E, gx, gy, 0, 1, 1, z, _hex('#6ccf4f'), _hex('#8a5a32'), _hex('#6b4426'), _hex('#3b6b2b'), 1.2)
    # a tree
    iso.box(E, 1, -2, hgt[(1, -2)], 1, 1, 2, _hex('#8a5a32'), _hex('#6b4426'), _hex('#5a3820'))
    iso.box(E, 0, -3, hgt[(1, -2)] + 2, 3, 3, 1.6, _hex('#3f9a3a'), _hex('#2e7a2a'), _hex('#24621f'))
    sun = 0.5 + 0.5 * math.sin(f.t * 0.7)
    E.circle(w * 0.82, h * 0.12, w * 0.07, _hex('#fff7c2', 1.4))
    E.text('60 FPS', w * 0.07, h * 0.07, w * 0.06, BONE, font='mono', wght=700, align='left')


def paint_desktop(E, f, w, h):
    E.rect(w / 2, h / 2, w, h, _hex('#1b3a5c'))
    for k in range(5):  # wallpaper bands
        E.circle(w * (0.2 + 0.18 * k), h * (0.7 - 0.05 * k), w * 0.25, a(_hex('#2c5d8a'), 0.35))
    E.rect(w / 2, h - h * 0.045, w, h * 0.09, a(_hex('#0e1a26'), 0.92))          # panel
    for k in range(6):
        E.rect(w * 0.06 + k * w * 0.05, h - h * 0.045, w * 0.032, h * 0.05, a(_hex('#5fa8e8'), 0.85), r=3)
    E.text('12:34', w * 0.95, h - h * 0.045, h * 0.035, BONE, font='mono', wght=600, align='right')
    wins = [(0.32, 0.36, 0.42, 0.42, 'Terminal'), (0.62, 0.42, 0.46, 0.48, 'Browser'), (0.47, 0.6, 0.34, 0.3, 'glmark2')]
    for i, (cx, cy, ww, hh, title) in enumerate(wins):
        jig = 0.01 * math.sin(f.t * 1.3 + i)
        X, Y, Wn, Hn = w * (cx + jig), h * cy, w * ww, h * hh
        E.rect(X + 6, Y + 8, Wn, Hn, a(INK, 0.35), r=8)
        E.rect(X, Y, Wn, Hn, _hex('#eef3f7'), r=8)
        E.rect(X, Y - Hn / 2 + h * 0.025, Wn, h * 0.05, _hex('#d6dee6'), r=8)
        E.text(title, X, Y - Hn / 2 + h * 0.025, h * 0.028, _hex('#30404e'), wght=700)
        if title == 'Terminal':
            E.rect(X, Y + h * 0.025, Wn - 10, Hn - h * 0.06, _hex('#0f1b14'))
            for L, s in enumerate(('$ glxinfo -B', 'OpenGL core profile', 'version: 4.6', '$ _')):
                E.text(s, X - Wn / 2 + 14, Y - Hn / 2 + h * 0.09 + L * h * 0.045, h * 0.03, GLUE, font='mono',
                       wght=500, align='left')
        elif title == 'glmark2':
            E.rect(X, Y + h * 0.025, Wn - 10, Hn - h * 0.06, _hex('#202020'))
            E.circle(X + math.sin(f.t * 2) * Wn * 0.2, Y + h * 0.03, h * 0.06, _hex('#e05050'))
        else:
            for L in range(5):
                E.rect(X - Wn * 0.1, Y - Hn / 2 + h * 0.1 + L * h * 0.06, Wn * 0.7 * (0.6 + 0.4 * ((L * 7) % 3) / 2),
                       h * 0.022, _hex('#b8c4cf'), r=4)


def paint_mac(E, f, w, h):
    E.rect(w / 2, h / 2, w, h, _hex('#2a2440'))
    for k in range(3):
        E.circle(w * (0.3 + 0.25 * k), h * 0.6, w * 0.3, a(_hex('#5a3f86'), 0.3))
    E.rect(w / 2, h * 0.03, w, h * 0.06, a(_hex('#ffffff'), 0.25))
    E.rect(w / 2, h * 0.9, w * 0.5, h * 0.1, a(_hex('#ffffff'), 0.25), r=12)
    X, Y, Wn, Hn = w * 0.5, h * 0.45, w * 0.6, h * 0.58
    E.rect(X, Y, Wn, Hn, _hex('#101010'), r=10)
    E.rect(X, Y - Hn / 2 + h * 0.03, Wn, h * 0.06, _hex('#2a2a2a'), r=10)
    for k, cc in enumerate(('#ff5f57', '#febc2e', '#28c840')):
        E.circle(X - Wn / 2 + 18 + k * 18, Y - Hn / 2 + h * 0.03, 5, _hex(cc))
    # a little lit scene: hills + sun (a game, generically)
    E.rect(X, Y + Hn * 0.2, Wn - 8, Hn * 0.5, _hex('#3b7d3a'))
    E.circle(X + Wn * 0.25, Y - Hn * 0.1, h * 0.06, _hex('#ffd36b', 1.3))


def paint_term(E, f, w, h, lines, typed=None):
    E.rect(w / 2, h / 2, w, h, _hex('#0b160e'))
    for i, s in enumerate(lines):
        code_line(E, s, 20, 40 + i * 44, 26, chars=None if typed is None else typed - sum(len(x) for x in lines[:i]))
