"""Bars 16-32: the first drop. Logo hit, the hook, four platforms, the hook again."""
import math
from anim import *
from kit import *
from devices import *
from engine import LW, LH, hexc


def logo(E, f, lb0, cx=960, cy=560, splat=True, scale=1.0, cube_y=None):
    """The MobileGlues mark (block dropping into glue, hitting at local beat lb0) + MobileGL wordmark."""
    u = f.lbeat - lb0
    t_hit = f.t - u * S.BEAT
    s = scale
    cy_cube = cube_y if cube_y is not None else cy - 230 * s
    if splat and u >= 0:   # shockwave rings off the impact
        ts = u * S.BEAT
        for k in range(3):
            w = out_expo(clamp01((ts - 0.06 * k) / 0.9))
            if 0 < w < 1:
                E.ring(cx, cy_cube + 60 * s, (120 + 1100 * w) * s, 14 * (1 - w) + 1, a(GLUE, 0.7 * (1 - w)))
    cubes(E, f, [(cx, cy_cube, 165 * s, 0.0, 0.12 * f.kick, 0.4 * f.kick, 0.0, 1.0, u * S.BEAT)])
    k = 1 + 0.03 * f.kick
    w1 = E.measure('Mobile', 'archivo', 220 * s, 900, 100)
    w2 = E.measure('GL', 'archivo', 220 * s, 900, 100)
    x0 = cx - (w1 + w2) / 2
    E.text('Mobile', x0, cy + 150 * s, 220 * s * k, BONE, wght=900, align='left')
    E.text('GL', x0 + w1, cy + 150 * s, 220 * s * k, scale_rgb(GLUE, 1.6), wght=900, align='left')
    return t_hit


def hook_word(E, f, word, kind, t_hit, color=None, wdth=100):
    b, fg = plate(E, f, kind, grid=False)
    size = fit_size(E, word, 1720, 900, wdth, cap=560)
    slam(E, f, word, 960, 560, t_hit, size, color or fg, wdth=wdth, punch=0.12)


PLATFORMS = [
    ('ANDROID', 'ink', ['Minecraft: Java Edition', 'via Fold Craft Launcher', 'Vulkan or OpenGL ES underneath']),
    ('LINUX', 'mint', ['EGL · GLX · Wayland', 'a glvnd vendor', 'a whole KDE Plasma desktop']),
    ('WINDOWS', 'ink', ['WGL', 'drop-in opengl32.dll', 'Vulkan, or GLES via ANGLE']),
    ('MACOS', 'glue', ['CGL · NSOpenGL', 'Vulkan via MoltenVK', 'OpenGL 4.6 on a Mac']),
]



def draw_device(E, f, name, kind, u, cx, cy, sc, bounce=0.0):
    """The platform's hero device anchored at (cx, cy) with iso scale sc; u = beats since its cut."""
    faces = FACE_LIGHT if kind != 'ink' else FACE_DARK
    line = OUTL if kind == 'ink' else hexc('#2f7d2a')
    k = sc / 82
    if name == 'ANDROID':
        scr = screen_layer(E, 'scr_phone', 265, 550, paint_voxels, f)
        iso = Iso(cx, cy - bounce, sc)
        phone(E, iso, f, faces=faces, screen=scr, line=line)
        drip = 120 * out_cubic((u - 1.5) / 2)
        if drip > 2:
            px, py = iso.p(0, 0.2, 6.2)
            goo(E, [(px + 60 * k, py + 10, 26 * k), (px + 60 * k, py + 10 + drip * k, 18 * k)],
                [(px + 60 * k, py + 10, px + 60 * k, py + 10 + drip * k, 16 * k, 12 * k)], k=20, outline=4, bevel=16)
    elif name == 'LINUX':
        scr = screen_layer(E, 'scr_desk', 760, 440, paint_desktop, f)
        monitor(E, Iso(cx, cy - bounce, sc), f, faces=faces, screen=scr, line=line)
    elif name == 'WINDOWS':
        iso = Iso(cx, cy, sc)
        iso.box(E, -3.5, -2.5, 0, 7, 5, 2.4, faces[0], faces[1], faces[2], line, 5)          # folder body
        iso.box(E, -3.5, -2.5, 2.4, 3.0, 5, 0.4, faces[0], faces[1], faces[2], line, 5)       # tab
        x, y0 = iso.p(0, 0, 2.4)
        E.text('C:\\Games\\YourApp\\', x, y0 + 210 * k, 30 * k, a(BONE if kind == 'ink' else INK, 0.85),
               font='mono', wght=600)
        fall = clamp01((u - 0.4) / 0.6)
        land = u >= 1.0
        if not land:
            y = lerp(-200, y0 - 120 * k, in_cubic(fall))
        else:
            y = y0 - 120 * k + 26 * k * math.exp(-(u - 1.0) * 6) * math.sin((u - 1.0) * 20)
        if u >= 0.4:
            file_card(E, x, y - bounce * 0.5, 230, 280, 'opengl32', '.dll', rot=-0.08 if not land else 0.0, s=k)
        if land:
            ring = out_expo((u - 1.0) / 0.8)
            E.ring(x, y0 - 20, 60 + 300 * ring * k, 6 * (1 - ring) + 0.5, a(GLUE, 1 - ring))
    else:  # MACOS
        scr = screen_layer(E, 'scr_mac', 600, 380, paint_mac, f)
        laptop(E, Iso(cx, cy - bounce, sc), f, y=2.0, faces=FACE_DARK, screen=scr, line=hexc('#0d2213'),
               open_=out_back(clamp01(u / 0.5)))


DEV_POS = {'ANDROID': (1380, 1010, 92), 'LINUX': (1330, 1000, 78), 'WINDOWS': (1360, 880, 82), 'MACOS': (1330, 930, 86)}
DEV_CLOSE = {'ANDROID': (1450, 1180, 120), 'LINUX': (1400, 1200, 104), 'WINDOWS': (1450, 1060, 110),
             'MACOS': (1400, 1120, 116)}


def platform(E, f, i, lb0):
    """4-beat platform card: the name slams on the cut, its three facts pop on the next 8ths."""
    name, kind, chips_ = PLATFORMS[i]
    u = f.lbeat - lb0
    t0 = f.t - u * S.BEAT
    b, fg = plate(E, f, kind, drift=(-20, 0))
    screen_cam(E, f, zoom=1.0 + 0.02 * u + 0.012 * f.kick)
    E.text(f'0{i + 1} / 04', 130, 170, 26, a(fg, 0.7), font='mono', wght=700, align='left')
    slam(E, f, name, 120, 290, t0, 190, fg, align='left', wdth=110 if i != 2 else 95)
    for k, c in enumerate(chips_):
        bk = 0.5 + 0.5 * k
        if u >= bk:
            uu = out_back((u - bk) / 0.3)
            chip(E, 130, 430 + 82 * k, c, 30, b if kind != 'ink' else INK, fg if kind != 'ink' else GLUE,
                 align='left', sc=uu, font='mono', wght=700)
    draw_device(E, f, name, kind, u, *DEV_POS[name], bounce=18 * f.kick)
    return {}


def platform_close(E, f, i, lb0):
    """2-beat recap: a huge name, the device pushed in close."""
    name, kind, _ = PLATFORMS[i]
    u = f.lbeat - lb0
    b, fg = plate(E, f, kind, grid=True, drift=(-80, 0))
    screen_cam(E, f, zoom=1.0 + 0.12 * out_cubic(u / 2), rot=(0.02 if i % 2 else -0.02) * u,
               shake=10 * math.exp(-u * 5))
    draw_device(E, f, name, kind, 2.0 + u, *DEV_CLOSE[name], bounce=24 * f.kick)
    slam(E, f, name, 960, 300, f.t - u * S.BEAT, fit_size(E, name, 1650, cap=330), fg, punch=0.15)


API_WORDS = [('EGL', 'linux · android'), ('GLX', 'x11 apps'), ('WAYLAND', 'linux desktops'),
             ('GLVND', 'a system-wide vendor'), ('WGL', 'windows'), ('CGL', 'macos'), ('NSOPENGL', 'macos'),
             ('FCL', 'minecraft on android')]
TILES = ['ANDROID', 'LINUX', 'WINDOWS', 'MACOS', 'EGL', 'GLX', 'WAYLAND', 'GLVND', 'WGL', 'CGL', 'NSOPENGL', 'VULKAN',
         'OPENGL ES', '4.6 CORE', 'SPIR-V', 'LGPL-3.0']


def drop1(E, f):
    lb = f.lbeat
    P = {'bloom': 0.55, 'grain': 0.032, 'ca': 0.5 * f.kick}
    tb_ = lambda b: f.t - (lb - b) * S.BEAT
    if lb < 4:   # LOGO HIT (4 beats)
        plate(E, f, 'ink', grid=False)
        rot = f.lt * 0.25
        for k in range(18):   # rays
            ang = rot + k * math.pi / 9
            E.poly([(960, 400), (960 + 2200 * math.cos(ang - 0.06), 400 + 2200 * math.sin(ang - 0.06)),
                    (960 + 2200 * math.cos(ang + 0.06), 400 + 2200 * math.sin(ang + 0.06))], a(MOSS, 0.35))
        dot_grid(E, f, a(MOSS, 0.5), 60, 2.2, pulse=0.6)
        screen_cam(E, f, zoom=1.0 + 0.012 * f.kick + 0.03 * lb / 4, shake=26 * math.exp(-lb * 2.2))
        logo(E, f, 0, 960, 560)
        E.text('desktop OpenGL 4.6 Core  ·  on Vulkan & OpenGL ES  ·  anywhere', 960, 880, 30, a(BONE, 0.75),
               font='mono', wght=500)
        P['flash'] = (*GLUE[:3], 0.9 * math.exp(-lb * S.BEAT / 0.12))
        P['bloom'] = 0.7
    elif lb < 8:   # hook, one word per beat
        words = [('DESKTOP', 'glue', None), ('OPENGL', 'ink', None), ('ON', 'mint', None), ('ANYTHING', 'ink', NEON)]
        k = int(lb - 4)
        wd, kind, col = words[k]
        hook_word(E, f, wd, kind, tb_(4 + k), col, wdth=125 if k == 2 else 100)
        screen_cam(E, f, zoom=1 + 0.03 * f.kick + 0.02 * (lb % 1))
    elif lb < 24:   # platforms, 4 beats each
        i = int((lb - 8) // 4)
        platform(E, f, i, 8 + 4 * i)
        E.cam, E.shake = (LW / 2, LH / 2, 1.0, 0.0), (0.0, 0.0)
        hud(E, f, 'PLATFORMS', PLATFORMS[i][0], col=a(BONE, 0.5) if i in (0, 2) else a(INK, 0.5))
    elif lb < 32:   # recap, 2 beats each, pushed in
        i = int((lb - 24) // 2)
        platform_close(E, f, i, 24 + 2 * i)
    elif lb < 40:   # window-system APIs, one per beat
        kind, k = beat_plate(f, ['ink', 'glue', 'ink', 'mint'], 32)
        b, fg = plate(E, f, kind, grid=False)
        word, cap = API_WORDS[k % 8]
        screen_cam(E, f, zoom=1 + 0.04 * (lb % 1) + 0.02 * f.kick, rot=(0.015 if k % 2 else -0.015))
        speedlines(E, f, 0.25, a(fg, 0.5), n=40, seed=k)
        slam(E, f, word, 960, 500, tb_(32 + k), fit_size(E, word, 1500, cap=420), fg if kind != 'ink' else NEON,
             punch=0.2)
        E.text(cap, 960, 800, 40, a(fg, 0.8), font='mono', wght=700)
    elif lb < 48:   # hook in 8ths: the plate changes once a beat, the word twice
        kind, k = beat_plate(f, ['glue', 'ink', 'mint', 'ink'], 40)
        b, fg = plate(E, f, kind, grid=False)
        j = int((lb - 40) * 2)
        plat = ['ANDROID', 'LINUX', 'WINDOWS', 'MACOS'][(j // 4) % 4]
        word = ['DESKTOP', 'OPENGL', 'ON', plat][j % 4]
        screen_cam(E, f, zoom=1 + 0.05 * ((lb * 2) % 1) + 0.03 * f.kick, shake=4 + 4 * climax(f, 40, 48))
        speedlines(E, f, 0.35 + 0.3 * climax(f, 40, 48), a(fg, 0.45), seed=j)
        slam(E, f, word, 960, 540, tb_(40 + j * 0.5), fit_size(E, word, 1700, cap=520),
             fg if kind != 'ink' else (NEON if word == plat else BONE), punch=0.25)
        P['ca'] = 0.6 + 0.6 * f.kick
    else:   # 4x4 grid fills on 8ths, then the whole grid flashes and shakes up to the climax
        plate(E, f, 'ink')
        n = min(16, int((lb - 48) * 2) + 1)
        c = climax(f, 56, 63)
        z = 1.25 - 0.25 * out_cubic((lb - 48) / 4) + 0.08 * in_cubic(clamp01((lb - 52) / 4)) + 0.25 * c
        screen_cam(E, f, zoom=z + (0.02 + 0.05 * c) * f.kick, rot=0.03 * c * math.sin(f.lt * 9), shake=3 + 16 * c)
        if lb >= 56:
            speedlines(E, f, 0.3 + 0.7 * c, a(GLUE, 0.6), n=90, seed=5)
        # a highlight that races round the grid: one tile per 8th, then per 16th
        if lb >= 56:
            rate = 2 if lb < 60 else 4
            hot = int((lb - 56) * rate) % 16
        else:
            hot = -1
        order = [0, 1, 2, 3, 7, 11, 15, 14, 13, 12, 8, 4, 5, 6, 10, 9]
        all_on = lb >= 62.0
        for k in range(16):
            x = 270 + (k % 4) * 460
            y = 200 + (k // 4) * 225
            if k >= n:
                E.rect(x, y, 420, 190, a(INK2, 0.7), r=22)
                continue
            uu = out_back((lb - 48 - k * 0.5) / 0.3)
            jit = (8 + 20 * c) * f.kick
            x += (rnd('jx', k, int(f.t * 30)) - 0.5) * jit
            y += (rnd('jy', k, int(f.t * 30)) - 0.5) * jit
            lit = all_on or (hot >= 0 and order[hot] == k)
            col = scale_rgb(GLUE, 1.9) if lit else [GLUE, MINT, PALE, GLUE][(k + k // 4) % 4]
            sc_ = 1.0 + (0.12 if lit else 0.0) + 0.04 * f.kick
            E.rect(x, y, 420 * uu * sc_, 190 * uu * sc_, col, r=22)
            E.text(TILES[k], x - 10, y, fit_size(E, TILES[k], 330, cap=70) * uu * sc_, INK, wght=900)
            check(E, x + 170 * uu, y - 60 * uu, 22 * uu, INK, 7, (lb - 48 - k * 0.5 - 0.1) / 0.25)
        P['ca'] = 0.4 + 1.8 * c
        P['bloom'] = 0.55 + 0.35 * c
    return P
