"""MobileGlues mark sting: the block drops into a blob of glue, which splashes, erupts behind it and settles.
render:  python render.py video 0 4.5 --module sting --audio out/sting.wav --samples 8 --out out/glue_drop.mp4"""
import math
from anim import *
from kit import cubes, bg
from engine import LW, LH, hexc

END = 4.5
T_HIT = 1.1


def draw(E, t):
    E.begin_sub(MINT)
    f = Frame(t, 0)
    bg(E, MINT)
    u = t - T_HIT
    zoom = 1.0 + (0.03 * math.exp(-u * 3) * math.sin(u * 20) if u > 0 else 0.0)
    E.cam = (LW / 2, LH / 2, zoom, 0.0)
    cubes(E, f, [(960, 560, 330, 0.0, 0.0, 0.0, 0.0, 1.0, u)])
    fade = clamp01((t - (END - 0.4)) / 0.4)
    return {'bloom': 0.2, 'grain': 0.02, 'vignette': 0.22, 'flash': (*MINT[:3], fade)}
