"""Bars 88-112: the last drop (key lift) and the outro."""
import math
import os
from anim import *
from kit import *
from devices import *
from engine import LW, LH, hexc
from sc_drop1 import logo, hook_word

# what the frames were drawn with; render.py fills this in from the GL renderer string
RENDERED_WITH = {'line': None}


def cube_rain(E, f, lb0, n=10, floor=1010, step=0.5):
    """A row of little glue blobs; a block drops into each in turn."""
    items = []
    for k in range(n):
        land = lb0 + k * step
        x = 140 + (k * 173) % 1640
        sz = 50 + 20 * rnd('cs', k)
        u = (f.lbeat - land) * S.BEAT
        items.append((x, floor - sz, sz, 0.0, 0.1 * f.kick, 0.4, rnd('tint', k) * 0.5, 1.0, u))
    cubes(E, f, items)


MONTAGE = [  # local beat, word, plate, painter
    (8, 'ANDROID', 'ink'), (12, 'LINUX', 'mint'), (16, 'WINDOWS', 'ink'), (20, 'MACOS', 'glue'),
    (24, 'VULKAN', 'ink'), (28, 'OPENGL ES', 'mint'), (32, 'ANY MACHINE', 'ink'), (36, 'OPEN SOURCE', 'glue'),
]


def montage(E, f, i, lb0):
    word, kind = MONTAGE[i][1], MONTAGE[i][2]
    u = f.lbeat - lb0
    b, fg = plate(E, f, kind, grid=True, drift=(40, 0))
    size = fit_size(E, word, 1700, cap=330)
    screen_cam(E, f, zoom=1 + 0.02 * f.kick + 0.01 * u, shake=12 * math.exp(-u * 4))
    faces = FACE_DARK if kind == 'ink' else FACE_LIGHT
    line = OUTL if kind == 'ink' else hexc('#2f7d2a')
    if word == 'ANDROID':
        iso = Iso(1450, 1000, 62)
        phone(E, iso, f, faces=faces, screen=screen_layer(E, 'f_phone', 265, 550, paint_voxels, f), line=line)
    elif word == 'LINUX':
        iso = Iso(1380, 1010, 56)
        monitor(E, iso, f, faces=faces, screen=screen_layer(E, 'f_desk', 760, 440, paint_desktop, f), line=line)
    elif word == 'WINDOWS':
        y = lerp(-200, 820, out_back(clamp01(u / 0.6), 1.2))
        file_card(E, 1520, y, 230, 280, 'opengl32', '.dll', rot=0.05)
    elif word == 'MACOS':
        iso = Iso(1420, 960, 58)
        laptop(E, iso, f, y=2.0, faces=FACE_DARK, screen=screen_layer(E, 'f_mac', 600, 380, paint_mac, f),
               line=hexc('#0d2213'))
    elif word == 'VULKAN':
        cubes(E, f, [(1500, 830, 150, f.lt * 1.2, 0.2 * f.kick, 0.4 * f.kick, 0.0, 1.0)], 
              deep=hexc('#0a2a10'))
        E.text('Magma', 1500, 1030, 34, GLUE, font='mono', wght=800)
    elif word == 'OPENGL ES':
        cubes(E, f, [(1500, 830, 150, -f.lt * 1.2, 0.2 * f.kick, 0.4 * f.kick, 0.0, 1.0)], 
              deep=hexc('#2e6e2f'))
        E.text('Espryt', 1500, 1030, 34, MOSS, font='mono', wght=800)
    elif word == 'ANY MACHINE':
        pts = [(260 + k * 120, 820 + 40 * math.sin(k * 0.8 + f.lt * 3)) for k in range(13)]
        caps = [(p[0], p[1], q[0], q[1], 14, 14) for p, q in zip(pts, pts[1:])]
        goo(E, [(pts[0][0], pts[0][1], 50), (pts[-1][0], pts[-1][1], 50)], caps, k=30, bevel=18)
        for k in range(6):
            ph = (u * 2 + k / 6) % 1
            x = lerp(pts[0][0], pts[-1][0], ph)
            E.rect(x, 820 + 40 * math.sin((x - 260) / 120 * 0.8 + f.lt * 3), 30, 22, scale_rgb(PALE, 1.5), r=5)
    else:  # OPEN SOURCE
        chip(E, 960, 820, 'LGPL-3.0', 54, GLUE, INK, font='archivo', wght=900, sc=out_back(u / 0.4))
        chip(E, 960, 940, 'github.com/MobileGL-Dev/MobileGL', 32, INK, BONE, sc=out_back((u - 0.5) / 0.4))
    slam(E, f, word, 960, 330, f.t - u * S.BEAT, size, fg if kind != 'ink' else BONE, punch=0.2)


QUICK = [('ANDROID', 'ink'), ('LINUX', 'glue'), ('WINDOWS', 'ink'), ('MACOS', 'mint'), ('VULKAN', 'ink'),
         ('OPENGL ES', 'glue'), ('LOCAL', 'ink'), ('REMOTE', 'mint')]
FEATS = ['4.6 CORE', 'SPIR-V', 'VULKAN', 'GLES', 'ANDROID', 'LINUX', 'WINDOWS', 'MACOS', 'EGL', 'GLX', 'WGL', 'CGL',
         'INPROC', 'SPAWN', 'TCP', 'OPEN SOURCE']


def final_drop(E, f):
    """Last drop, accelerating: logo, 2-beat montage, 1-beat words, 8th-note features, multiplying hook, collapse."""
    lb = f.lbeat
    P = {'bloom': 0.6, 'grain': 0.032, 'ca': 0.5 * f.kick}
    tb_ = lambda b: f.t - (lb - b) * S.BEAT
    if lb < 4:   # biggest logo hit + jelly rain
        plate(E, f, 'ink', grid=False)
        rot = -f.lt * 0.3
        for k in range(18):
            ang = rot + k * math.pi / 9
            E.poly([(960, 330), (960 + 2200 * math.cos(ang - 0.07), 330 + 2200 * math.sin(ang - 0.07)),
                    (960 + 2200 * math.cos(ang + 0.07), 330 + 2200 * math.sin(ang + 0.07))], a(MOSS, 0.45))
        screen_cam(E, f, zoom=1.0 + 0.015 * f.kick + 0.04 * lb / 4, shake=30 * math.exp(-lb * 2.0))
        cube_rain(E, f, 0.25, 10, step=0.3)
        logo(E, f, 0, 960, 500, scale=1.1)
        P['flash'] = (*GLUE[:3], 0.95 * math.exp(-lb * S.BEAT / 0.12))
        P['bloom'] = 0.75
    elif lb < 20:   # montage, 2 beats each
        i = int((lb - 4) // 2)
        montage(E, f, i, 4 + 2 * i)
    elif lb < 28:   # one word a beat
        k = int(lb - 20)
        word, kind = QUICK[k]
        b, fg = plate(E, f, kind, grid=False)
        screen_cam(E, f, zoom=1 + 0.06 * (lb % 1) + 0.02 * f.kick, rot=(0.02 if k % 2 else -0.02))
        speedlines(E, f, 0.35, a(fg, 0.45), seed=k + 80)
        slam(E, f, word, 960, 540, tb_(20 + k), fit_size(E, word, 1650, cap=460), fg if kind != 'ink' else NEON,
             punch=0.25)
    elif lb < 36:   # sixteen features on 8ths; the plate still changes only once a beat
        kind, k = beat_plate(f, ['glue', 'ink', 'mint', 'ink'], 28)
        b, fg = plate(E, f, kind, grid=False)
        j = int((lb - 28) * 2)
        w = FEATS[j % 16]
        c = climax(f, 28, 36)
        screen_cam(E, f, zoom=1 + 0.06 * ((lb * 2) % 1) + 0.03 * f.kick, shake=3 + 6 * c)
        speedlines(E, f, 0.4 + 0.3 * c, a(fg, 0.45), seed=j + 90)
        slam(E, f, w, 960, 500, tb_(28 + j * 0.5), fit_size(E, w, 1550, cap=440), fg if kind != 'ink' else NEON,
             punch=0.25)
        check(E, 960, 820, 60, fg if kind != 'ink' else GLUE, 16, ((lb * 2) % 1) / 0.4)
        E.text(f'{j + 1:2d} / 16', 1750, 1010, 30, a(fg, 0.8), font='mono', wght=800, align='right')
    elif lb < 52:   # hub: MobileGL in the middle, glue strands shoot out to everything it runs on
        plate(E, f, 'ink')
        SAT = ['ANDROID', 'LINUX', 'WINDOWS', 'MACOS', 'VULKAN', 'OPENGL ES', 'EGL · GLX · WGL · CGL', 'TCP']
        c = climax(f, 44, 52)
        spin = 0.15 * (lb - 36) + 2.2 * c * c
        screen_cam(E, f, zoom=1.0 + 0.03 * f.kick + 0.12 * c, rot=0.03 * c * math.sin(f.lt * 7), shake=2 + 10 * c)
        if lb >= 44:
            speedlines(E, f, 0.3 + 0.6 * c, a(GLUE, 0.5), n=80, seed=17)
        balls = [(960, 540, 130 + 14 * f.kick)]
        caps, pos = [], []
        for k, name in enumerate(SAT):
            bk = 36 + k
            if lb < bk:
                continue
            ang = k * 2 * math.pi / 8 + spin - math.pi / 2
            shoot = out_expo(clamp01((lb - bk) / 0.3))
            pull = in_cubic(clamp01((lb - 50) / 2))                 # reeled back in at the end
            R = (1 - pull) * shoot
            x, y = 960 + math.cos(ang) * 640 * R, 540 + math.sin(ang) * 360 * R
            caps.append((960, 540, x, y, 14, 10))
            balls.append((x, y, 26))
            pos.append((x, y, name, shoot * (1 - pull), k))
        goo(E, balls, caps, k=40, glow=0.2 + 0.3 * c)
        # packets racing out along the strands: 8ths, then 16ths
        rate = 2 if lb < 48 else 4
        for x, y, name, sh, k in pos:
            if sh < 0.95:
                continue
            ph = (lb * rate + k * 0.37) % 1
            E.rect(lerp(960, x, ph), lerp(540, y, ph), 22, 16, scale_rgb(PALE, 1.6), r=4)
        for x, y, name, sh, k in pos:
            if sh <= 0.05:
                continue
            fresh = math.exp(-max(0.0, lb - (36 + k)) * 4)
            chip(E, x, y, name, 40, INK, scale_rgb(GLUE, 1.0 + 0.8 * fresh), font='archivo', wght=900, sc=sh * (1 + 0.3 * fresh))
        E.text('MobileGL', 960, 540, 46 * (1 + 0.05 * f.kick), INK, wght=900)
        P['ca'] = 0.4 * f.kick + 1.2 * c
        P['bloom'] = 0.6 + 0.3 * c
    else:   # collapse into one glue drop, faster and faster
        plate(E, f, 'ink')
        u = in_cubic(clamp01((lb - 52) / 11.0))
        c = climax(f, 52, 63)
        speedlines(E, f, 0.3 + 0.7 * c, a(GLUE, 0.8), n=110, seed=13)
        screen_cam(E, f, zoom=1 + 0.5 * c + 0.03 * f.kick, rot=0.05 * c, shake=3 + 14 * c)
        balls = []
        for k in range(32):
            ang = k * 2 * math.pi / 32 + f.lt * (1.5 + 6 * c) + 3.0 * (lb - 52) / 11
            R = 900 * (1 - u) * (0.6 + 0.4 * rnd('cv', k))
            balls.append((960 + math.cos(ang) * R, 540 + math.sin(ang) * R * 0.6, 40 + 30 * u))
        balls.append((960, 540, 60 + 180 * u))
        goo(E, balls, k=60 + 60 * u, glow=0.2 + 0.6 * u)
        E.cam = (LW / 2, LH / 2, 1.0 + 0.04 * f.kick, 0.0)
        E.text('DESKTOP OPENGL.', 960, 160, 110, a(BONE, 1 - 0.3 * u), wght=900, wdth=100 + 25 * c)
        E.text('ANYWHERE.', 960, 930, 130, a(NEON, 1 - 0.3 * u), wght=900, wdth=100 + 25 * c)
        P['bloom'] = 0.6 + 0.5 * u
        P['ca'] = 0.5 + 1.8 * c
    return P


def outro(E, f):
    lb = f.lbeat
    P = {'bloom': 0.55, 'grain': 0.035}
    plate(E, f, 'ink', drift=(3, 0))
    fade_out = clamp01((lb - 26) / 5.0)
    if lb < 16:
        if lb < 0.3:
            P['flash'] = (*GLUE[:3], 0.7 * (1 - lb / 0.3))
        logo(E, f, 0, 960, 430, splat=True, scale=0.9)
        chips_ = [('OpenGL 4.6 Core', GLUE), ('Vulkan', MINT), ('OpenGL ES', MINT), ('LGPL-3.0', PALE)]
        total = sum(E.measure(s_, 'mono', 30, 700) + 36 for s_, _ in chips_) + 24 * (len(chips_) - 1)
        x = 960 - total / 2
        for k, (s, c) in enumerate(chips_):
            if lb >= 2 + k * 0.5:
                w = chip(E, x, 760, s, 30, INK, c, align='left', sc=out_back((lb - 2 - k * 0.5) / 0.35))
                x += w + 24
        if lb >= 5:
            u = clamp01((lb - 5) / 0.5)
            E.text('github.com/MobileGL-Dev/MobileGL', 960, 880, 44, a(BONE, u), font='mono', wght=700)
    else:
        u = clamp01((lb - 16) / 0.4)
        if lb < 16.3:
            P['flash'] = (*BONE[:3], 0.5 * (1 - (lb - 16) / 0.3))
        cubes(E, f, [(960, 360, 120, f.lt * 0.4, 0.0, 0.0, 0.0, 1.0 - fade_out)])
        line = RENDERED_WITH['line']
        if line:
            E.text('every frame you just saw', 960, 600, 70, a(BONE, u * (1 - fade_out)), wght=800)
            E.text('was rendered through MobileGL.', 960, 690, 70, a(NEON, u * (1 - fade_out)), wght=900)
            E.text(line, 960, 800, 28, a(BONE, 0.65 * u * (1 - fade_out)), font='mono', wght=600)
        else:
            E.text('desktop OpenGL. anywhere.', 960, 640, 80, a(BONE, u * (1 - fade_out)), wght=900)
        E.text('github.com/MobileGL-Dev/MobileGL', 960, 920, 34, a(GLUE, u * (1 - fade_out)), font='mono', wght=700)
        if lb >= 28:
            code_line(E, '', 900, 540, 34, chars=0, cursor=True, f=f, alpha=clamp01((lb - 28) / 0.5))
    P['fade'] = clamp01((lb - 32) / 1.2)
    return P
