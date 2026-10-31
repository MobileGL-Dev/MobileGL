"""Bars 80-88: rapid-fire checklist over the last build."""
import math
from anim import *
from kit import *
from engine import LW, LH, hexc

ITEMS = ['OpenGL 4.6 Core', 'GLSL → SPIR-V', 'Vulkan backend', 'OpenGL ES backend',
         'Android', 'Linux', 'Windows', 'macOS',
         'EGL · GLX', 'WGL', 'CGL · NSOpenGL', 'glvnd vendor',
         'same process', 'another process', 'another machine', 'open source']


def checklist(E, f):
    lb = f.lbeat
    P = {'bloom': 0.55, 'grain': 0.035, 'ca': 0.3 * f.roll}
    if lb >= 31:   # the silent beat: the block falls into the glue, the last drop lands on the downbeat
        pre_drop(E, f, 960, 247, 181.5, f.t - (lb - 32) * S.BEAT)
        return P
    plate(E, f, 'ink')
    c = climax(f, 24, 31)
    screen_cam(E, f, zoom=1 + 0.35 * c + 0.01 * f.roll, rot=0.04 * c, shake=2 + 14 * c)
    n = min(16, max(0, int(lb) + 1))
    sweep = int(lb * 2) % 16 if lb >= 16 else -1
    if lb >= 24:
        speedlines(E, f, 0.3 + 0.7 * c, a(GLUE, 0.7), n=100, seed=31)
    for k in range(16):
        r, cc = k // 4, k % 4
        x = 270 + cc * 460
        y = 260 + r * 190
        if k >= n:
            E.rect(x, y, 420, 150, a(INK2, 0.6), r=20)
            continue
        # from beat 24 the cards fly out at the camera, one per 8th, accelerating
        fly = 0.0
        tf = 24 + 0.44 * k * (1 - 0.35 * k / 16)
        if lb >= tf:
            fly = in_cubic(clamp01((lb - tf) / 0.6))
        if fly >= 1:
            continue
        sc_ = 1 + 3.5 * fly
        x = 960 + (x - 960) * (1 + 2.8 * fly)
        y = 540 + (y - 540) * (1 + 2.8 * fly)
        u = out_back((lb - k) / 0.35)
        hot = k == sweep or (lb < 16 and int(lb) == k) or fly > 0
        col = scale_rgb(GLUE, 1.5) if hot else INK2
        E.alpha = 1 - fly
        E.rect(x, y, 420 * u * sc_, 150 * u * sc_, col, r=20 * sc_, stroke=a(GLUE, 0.6), sw=3 * sc_)
        E.text(ITEMS[k], x - 180 * u * sc_, y, 30 * u * sc_, INK if hot else BONE, wght=800, align='left', wdth=90)
        check(E, x + 165 * u * sc_, y, 30 * u * sc_, INK if hot else GLUE, 9 * sc_, (lb - k - 0.1) / 0.3)
        E.flush()
        E.alpha = 1.0
    E.cam, E.shake = (LW / 2, LH / 2, 1.0, 0.0), (0.0, 0.0)
    if lb < 24:
        E.rect(960, 1000, 380, 80, INK, r=40, stroke=a(GLUE, 0.6), sw=3)
        E.text(f'{n:2d} / 16', 960, 1000, 44, NEON, font='mono', wght=800)
    P['ca'] = 0.3 * f.roll + 1.5 * c
    P['bloom'] = 0.55 + 0.4 * c
    return P
