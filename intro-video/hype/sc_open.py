"""Bars 0-16: the cold open (a prompt that the screen obeys) and the first build-up."""
import math
from anim import *
from kit import *
from devices import *
from engine import LW, LH, hexc

CLEAR_C = hexc('#0b2412')


def cold_open(E, f):
    """Bars 0-8. GL calls typed at a prompt; each one is executed by the frame itself."""
    lb = f.lbeat
    P = {'bloom': 0.7, 'grain': 0.032, 'vignette': 0.45}
    bg(E, INK)
    # glClear: flood from the prompt
    if lb >= 8:
        u = out_expo((lb - 8) / 1.2)
        E.circle(150, 930, 2400 * u, CLEAR_C)
        dot_grid(E, f, a(MOSS, 0.55 * u), 60, 2.2)
    cmds = [  # text, type start, type end, execute beat
        ('glClearColor(0.04, 0.14, 0.07, 1.0);', 1.0, 3.6, 4.0),
        ('glClear(GL_COLOR_BUFFER_BIT);', 4.5, 7.0, 8.0),
        ('glDrawArrays(GL_TRIANGLES, 0, 3);', 9.0, 11.6, 12.0),
        ('glUseProgram(glue);', 16.0, 18.0, 20.0),
        ('// ...and now: run it on everything', 24.0, 27.5, 99.0),
    ]
    # centre visual: triangle -> glue drop
    cx, cy = 960, 470
    if 12 <= lb < 22:
        u = out_back((lb - 12) / 0.9, 2.2)
        melt = in_out_cubic((lb - 20) / 2.0)
        R = 300 * u * (1 - 0.85 * melt)
        rot = 0.05 * (lb - 12) + 0.25 * melt
        pts = [(cx + R * math.cos(rot - math.pi / 2 + k * 2 * math.pi / 3),
                cy + 30 + R * math.sin(rot - math.pi / 2 + k * 2 * math.pi / 3)) for k in range(3)]
        hp = 1 + 0.25 * f.hat
        if melt < 1:
            tri_rgb(E, pts, [scale_rgb(GLUE, 2.2 * hp), scale_rgb(PALE, 1.0), scale_rgb(MOSS, 1.3)])
        if melt > 0:   # vertices turn into glue and run to the centre
            balls = [(lerp(px, cx, melt), lerp(py, cy + 30, melt), 30 + 120 * melt) for px, py in pts]
            goo(E, balls, k=90, outline=6)
    elif 22 <= lb:
        bob = math.sin(f.lt * 3.0) * 8
        stretch = 0.0
        r = 150 + 10 * f.hat
        if lb >= 24:
            stretch = 260 * in_cubic((lb - 24) / 7.5)
        balls = [(cx, cy + 30 + bob, r)]
        caps = [(cx, cy + 30 + bob - stretch - 40, cx, cy + 30 + bob, r * 0.25, r * 0.95)] if stretch > 1 else []
        z = 1 + 9 * in_expo((lb - 27) / 4.6)
        E.cam = (cx, cy + 30 + bob, z, 0.0)
        goo(E, balls, caps, k=80, outline=7, glow=0.2 * clamp01((lb - 24) / 8))
        E.cam = (LW / 2, LH / 2, 1.0, 0.0)
        if lb > 30.6:
            bg(E, a(GLUE, clamp01((lb - 30.6) / 0.6)))
    # prompt + log
    py = 930
    done = [c for c in cmds if lb >= c[2]]
    cur = [c for c in cmds if c[1] <= lb < c[2] or (lb >= c[2] and lb < c[3])]
    alpha_ui = 1.0 - clamp01((lb - 29) / 1.5)
    for i, c in enumerate(reversed([c for c in cmds if lb >= c[3]])):
        code_line(E, c[0], 150, py - 56 * (i + 1) - 20, 28, alpha=(0.55 - 0.1 * i) * alpha_ui)
    active = None
    for c in cmds:
        if c[1] <= lb < c[3] and lb < 99:
            active = c
    E.text('>', 112, py, 30, a(GLUE, alpha_ui), font='mono', wght=700, align='left')
    if active:
        n = len(typed(f, active[0], active[1], active[2]))
        code_line(E, active[0], 150, py, 30, chars=n, cursor=True, f=f, alpha=alpha_ui)
    else:
        code_line(E, '', 150, py, 30, chars=0, cursor=True, f=f, alpha=alpha_ui)
    # execute flashes
    for c in cmds:
        u = lb - c[3]
        if 0 <= u < 0.5:
            E.rect(LW / 2, LH / 2, LW, LH, a(GLUE, 0.025 * (1 - u / 0.5)))
    hud(E, f, 'MOBILEGL // REEL', 'OPENGL 4.6 CORE', alpha=clamp01(lb / 4) * alpha_ui)
    if lb > 30.6:
        P['exposure'] = 1.0 + 0.6 * clamp01((lb - 30.6) / 1.4)
    return P


def your_app(E, f):
    """Bars 8-16: your app speaks GL 4.6 -> your device doesn't -> so we made the glue -> countdown."""
    lb = f.lbeat
    P = {'bloom': 0.5, 'grain': 0.035}
    tb_ = lambda b: f.t - (lb - b) * S.BEAT          # absolute time of local beat b
    if lb < 8:
        plate(E, f, 'mint', drift=(0, -12))
        E.cam = (LW / 2, LH / 2, 1.0 + 0.004 * lb, -0.004 * lb)
        x0 = 130
        w = slam(E, f, 'YOUR', x0, 270, tb_(0), 210, INK, align='left')
        slam(E, f, 'APP', x0 + w + 50, 270, tb_(1), 210, INK, align='left')
        slam(E, f, 'SPEAKS', x0, 500, tb_(2), 210, INK, align='left')
        w = slam(E, f, 'OPENGL', x0, 730, tb_(3), 210, INK, align='left')
        slam(E, f, '4.6', x0 + w + 50, 730, tb_(3) + 0.06, 210, MOSS, align='left', wdth=80)
        chips_ = ['#version 460 core', 'glBindVertexArray', 'glDrawElementsInstanced', 'GL_ARB_direct_state_access',
                  'glMultiDrawElementsIndirect', 'compute shaders', 'SSBOs', 'tessellation']
        pos = [(1460, 190), (1500, 330), (1420, 470), (900, 900), (1430, 610), (420, 905), (1650, 905), (1300, 905)]
        for i, s in enumerate(chips_):
            bi = 4 + i * 0.5
            if lb >= bi:
                u = out_back((lb - bi) / 0.35)
                chip(E, pos[i][0], pos[i][1], s, 30, BONE, INK, sc=u, rot=0.03 * math.sin(i * 2.3))
        E.cam = (LW / 2, LH / 2, 1.0, 0.0)
    elif lb < 16:
        plate(E, f, 'ink')
        w = slam(E, f, 'YOUR', 130, 190, tb_(8), 150, BONE, align='left')
        w2 = slam(E, f, 'DEVICE', 130 + w + 40, 190, tb_(8.5), 150, BONE, align='left')
        if lb >= 15:
            stamp(E, f, "DOESN'T.", 960, 560, tb_(15), 210, CORAL, rot=-0.09, w_pad=50)
        if lb >= 15:
            P['shake'] = 1
            screen_cam(E, f, shake=10 * math.exp(-(lb - 15) * 3))
        devs = [('phone', 400, 'Android', 'OpenGL ES + Vulkan only'), ('laptop', 960, 'macOS', 'OpenGL 4.1, deprecated'),
                ('board', 1520, 'ARM Linux', 'GLES-only vendor drivers')]
        for i, (kind, x, name, sub) in enumerate(devs):
            bi = 9 + i
            if lb < bi:
                continue
            u = out_back((lb - bi) / 0.4)
            iso = Iso(x, 760 + 40 * (1 - u), 34 * u)
            if kind == 'phone':
                phone(E, iso, f)
            elif kind == 'laptop':
                laptop(E, iso, f, y=2.0)
            else:
                board(E, iso, f)
            E.text(name, x, 880, 44, a(BONE, u), wght=800)
            E.text(sub, x, 935, 24, a(BONE, 0.65 * u), font='mono', wght=500)
            ci = 12 + i
            if lb >= ci:
                cross(E, x, 600, 120, CORAL, 26, (lb - ci) / 0.25)
        E.cam = (LW / 2, LH / 2, 1.0, 0.0)
        E.shake = (0.0, 0.0)
    elif lb < 24:
        plate(E, f, 'mint', drift=(10, 0))
        slam(E, f, 'SO WE MADE', 960, 230, tb_(16), 120, INK, wdth=110)
        size = fit_size(E, 'THE GLUE.', 1640)
        if lb >= 18:
            slam(E, f, 'THE GLUE.', 960, 470, tb_(18), size, INK, punch=0.25)
        top = 470 - size * 0.36   # cap top
        # a drop falls onto the words at beat 19 and drips over them
        if lb >= 18.2:
            fall = clamp01((lb - 18.2) / 0.8)
            X = 1290
            y = lerp(-150, top - 30, in_cubic(fall))
            if fall < 1:
                b, c = drop_shape(X, y, 56, 90 * fall)
                goo(E, b, c, k=50)
            else:
                spread = out_expo((lb - 19) / 0.6)
                balls = [(X, top - 18, 46 * (1 - 0.4 * spread))]
                caps = [(X - 250 * spread, top - 6, X + 230 * spread, top - 6, 14, 14)]
                for k, (dx, ln) in enumerate(((-230, 70), (-120, 150), (-20, 60), (80, 190), (190, 90))):
                    L = ln * out_cubic((lb - 19 - 0.12 * k) / 3.0)
                    x = X + dx * spread
                    if L > 2:
                        caps.append((x, top - 6, x, top - 6 + L, 10, 13))
                        balls.append((x, top - 6 + L, 17))
                goo(E, balls, caps, k=34, outline=5, bevel=22)
        # the crossed devices come back with checks
        for i, (name, x) in enumerate((('Android', 560), ('macOS', 960), ('ARM Linux', 1360))):
            bi = 20 + i
            if lb < bi:
                continue
            u = out_back((lb - bi) / 0.3)
            E.circle(x, 780, 80 * u, INK)
            check(E, x, 780, 70 * u, GLUE, 14, (lb - bi - 0.1) / 0.3)
            E.text(name, x, 910, 40, a(INK, u), wght=800)
            E.line(x, 690, x, 600, 8, a(LEAF, u))
    else:
        # bars 14-15: loading bar, then 3-2-1, then one beat of silence
        if lb < 28:
            plate(E, f, 'ink')
            u = clamp01((lb - 24) / 3.9)
            k = int(u * 32)
            E.text('LOADING DESKTOP OPENGL', 960, 420, 64, BONE, wght=800, wdth=110)
            for i in range(32):
                c = NEON if i < k else a(MOSS, 0.8)
                E.rect(960 - 31 * 26 / 2 + i * 26, 540, 18, 54, c, r=3)
            E.text(f'{int(u * 100):3d}%', 960, 640, 40, GLUE, font='mono', wght=700)
            E.text('frontend · pipe · backend · window system', 960, 720, 24, a(BONE, 0.5), font='mono')
        elif lb < 31:
            i = int(lb - 28)
            kind = ['glue', 'ink', 'mint'][i]
            b, fg = plate(E, f, kind, grid=False)
            slam(E, f, str(3 - i), 960, 560, tb_(28 + i), 760, fg if kind != 'ink' else NEON, punch=0.35, wght=900,
                 wdth=110)
            P['shake'] = 1
            screen_cam(E, f, shake=14 * math.exp(-(lb - 28 - i) * 5), seed=i)
        else:   # the silent beat: the block falls into the glue on the downbeat
            pre_drop(E, f, 960, 330, 165, tb_(32))
            P['grain'] = 0.05
    return P
