"""Bars 56-72: the second drop. Same process / another process / another machine, run-ahead, many clients."""
import math
from anim import *
from kit import *
from devices import *
from engine import LW, LH, hexc


def label_chip(E, x, y, s, col=INK, bgc=BONE, size=26, sc=1.0):
    chip(E, x, y, s, size, col, bgc, sc=sc, font='mono', wght=800)


def blob_pair(E, f, sep, strand_r, k_goo=90, glow=0.15, y=560, r=200, packets=False, lb=0.0):
    """Two blobs separated by sep px, joined by a strand (r=0 -> no strand)."""
    cx = 960
    wob = 10 * f.kick
    xa, xb = cx - sep / 2, cx + sep / 2
    balls = [(xa, y + wob, r + 8 * f.kick), (xb, y - wob, r + 8 * f.kick)]
    for k in range(5):   # surface jiggle
        ang = f.lt * 2 + k * 1.3
        balls.append((xa + math.cos(ang) * r * 0.5, y + math.sin(ang) * r * 0.5, r * 0.45))
        balls.append((xb + math.cos(-ang) * r * 0.5, y + math.sin(-ang) * r * 0.5, r * 0.45))
    caps = [(xa, y, xb, y, strand_r, strand_r)] if strand_r > 0 else []
    goo(E, balls, caps, k=k_goo, glow=glow)
    if packets and strand_r > 0:
        for k in range(16):
            ph = (f.lbeat * 2 + k / 4) % 4 / 4
            x = lerp(xa + r * 0.7, xb - r * 0.7, ph)
            E.circle(x, y, strand_r * 0.45, scale_rgb(PALE, 1.6))
    return xa, xb


def modes_v1(E, f):
    lb = f.lbeat
    P = {'bloom': 0.6, 'grain': 0.032, 'ca': 0.45 * f.kick}
    tb_ = lambda b: f.t - (lb - b) * S.BEAT
    if lb < 8:   # SAME PROCESS
        plate(E, f, 'ink')
        screen_cam(E, f, zoom=1 + 0.015 * f.kick, shake=18 * math.exp(-lb * 2.5))
        if lb < 0.25:
            P['flash'] = (*GLUE[:3], 0.8 * (1 - lb / 0.25))
        slam(E, f, 'SAME PROCESS.', 960, 160, tb_(0), fit_size(E, 'SAME PROCESS.', 1500, cap=170), BONE)
        blob_pair(E, f, 0, 0, r=260, glow=0.25)
        label_chip(E, 960, 500, 'FRONTEND', sc=out_back(lb / 0.4))
        label_chip(E, 960, 620, 'BACKEND', sc=out_back((lb - 0.5) / 0.4))
        E.text('monolith · inproc', 960, 920, 34, GLUE, font='mono', wght=700)
    elif lb < 16:  # ANOTHER PROCESS
        plate(E, f, 'ink')
        screen_cam(E, f, zoom=1 + 0.015 * f.kick, shake=14 * math.exp(-(lb - 8) * 2.5))
        slam(E, f, 'ANOTHER PROCESS.', 960, 160, tb_(8), fit_size(E, 'ANOTHER PROCESS.', 1600, cap=170), BONE)
        u = in_out_cubic((lb - 8) / 1.8)
        sep = 900 * u
        sr = lerp(150, 26, u)
        xa, xb = blob_pair(E, f, sep, sr, r=lerp(260, 190, u), packets=lb > 10, lb=lb)
        label_chip(E, xa, 560, 'CLIENT', sc=1.0)
        label_chip(E, xb, 560, 'SERVER', sc=1.0)
        if lb >= 10:
            uu = clamp01((lb - 10) / 0.5)
            E.text('your app · frontend', xa, 820, 26, a(BONE, 0.8 * uu), font='mono')
            E.text('owns the GPU · backend', xb, 820, 26, a(BONE, 0.8 * uu), font='mono')
            E.text('spawn + shared memory', 960, 470, 30, a(GLUE, uu), font='mono', wght=700)
    elif lb < 24:  # ANOTHER MACHINE
        plate(E, f, 'ink', drift=(0, 0))
        z = lerp(1.25, 1.0, out_expo((lb - 16) / 1.5))
        screen_cam(E, f, zoom=z + 0.012 * f.kick, shake=14 * math.exp(-(lb - 16) * 2.5))
        slam(E, f, 'ANOTHER MACHINE.', 960, 150, tb_(16), fit_size(E, 'ANOTHER MACHINE.', 1600, cap=170), BONE)
        il = Iso(360, 820, 58)
        laptop(E, il, f, y=2.0, screen=screen_layer(E, 'm_lap', 600, 380, paint_mac, f))
        it = Iso(1580, 860, 62)
        tower(E, it, f)
        # long strand with packets, sagging like glue
        pts = []
        for k in range(13):
            u = k / 12
            pts.append((lerp(560, 1420, u), 560 + 120 * math.sin(math.pi * u) + 14 * math.sin(f.lt * 3 + u * 6)))
        caps = [(a_[0], a_[1], b[0], b[1], 16, 16) for a_, b in zip(pts, pts[1:])]
        goo(E, [(560, 560, 40), (1420, 560, 40)], caps, k=30, outline=5, bevel=18)
        for k in range(8):
            ph = ((lb - 16) * 4 / 8 + k / 8) % 1
            idx_ = ph * 12
            i0 = int(idx_)
            fr = idx_ - i0
            x = lerp(pts[i0][0], pts[min(i0 + 1, 12)][0], fr)
            y = lerp(pts[i0][1], pts[min(i0 + 1, 12)][1], fr)
            E.rect(x, y - 2, 30, 22, scale_rgb(PALE, 1.5), r=5)
        chip(E, 990, 440, 'TCP', 34, INK, PALE, font='mono', wght=800)
        E.text('client', 360, 960, 30, BONE, font='mono', wght=700)
        E.text('server', 1580, 960, 30, BONE, font='mono', wght=700)
        if lb >= 18:
            uu = clamp01((lb - 18) / 0.5)
            E.text('the client is the frontend · the server is the backend', 960, 1030, 26, a(GLUE, uu),
                   font='mono', wght=600)
    elif lb < 28:  # recap list
        plate(E, f, 'mint', grid=False)
        rows = ['SAME PROCESS', 'ANOTHER PROCESS', 'ANOTHER MACHINE']
        for i, s in enumerate(rows):
            if lb < 24 + i:
                continue
            y = 290 + i * 230
            slam(E, f, s, 260, y, tb_(24 + i), 150, INK, align='left', wdth=95)
            check(E, 170, y, 60, MOSS, 16, (lb - 24 - i) / 0.3)
        if lb >= 27:
            E.text('monolith · inproc · spawn + shm · tcp', 960, 990, 34, MOSS, font='mono', wght=800)
    elif lb < 32:  # YOUR CALL.
        plate(E, f, 'glue', grid=False)
        slam(E, f, 'YOUR CALL.', 960, 540, tb_(28), fit_size(E, 'YOUR CALL.', 1600), INK, punch=0.3)
        screen_cam(E, f, shake=16 * math.exp(-(lb - 28) * 3))
    elif lb < 40:  # RUN-AHEAD lanes
        plate(E, f, 'ink')
        slam(E, f, 'RUN-AHEAD', 120, 160, tb_(32), 150, BONE, align='left', wdth=110)
        E.text('the client keeps going while the server catches up', 130, 260, 28, a(GLUE, 0.9), font='mono',
               wght=600, align='left')
        scroll = (lb - 32) * 70
        for lane, (name, y, wait) in enumerate((('APP · monolith', 470, True), ('GPU', 560, True),
                                                 ('APP · split', 760, False), ('GPU', 850, False))):
            E.text(name, 130, y, 24, a(BONE, 0.8), font='mono', wght=700, align='left')
            for k in range(14):
                if lane in (0, 1):
                    x0 = 420 + k * 260 - scroll
                    if lane == 0:
                        E.rect(x0 + 60, y, 110, 54, GLUE, r=8)
                        E.rect(x0 + 180, y, 110, 54, a(BONE, 0.12), r=8)
                    else:
                        E.rect(x0 + 180, y, 110, 54, MINT, r=8)
                else:
                    x0 = 420 + k * 150 - scroll * 1.0
                    if lane == 2:
                        E.rect(x0 + 60, y, 130, 54, GLUE, r=8)
                    else:
                        E.rect(x0 + 120, y, 130, 54, MINT, r=8)
        E.rect(200, 660, 400, 520, INK)  # mask the lanes under the labels
        for lane, (name, y) in enumerate((('APP · monolith', 470), ('GPU', 560), ('APP · split', 760), ('GPU', 850))):
            E.text(name, 130, y, 24, a(BONE, 0.85), font='mono', wght=700, align='left')
        E.text('wait', 700 - (scroll % 260), 420, 20, a(BONE, 0.4), font='mono')
    elif lb < 48:  # ~1/3 faster
        plate(E, f, 'ink')
        slam(E, f, '≈33% FASTER', 960, 260, tb_(40), 210, NEON, punch=0.3)
        u = out_expo((lb - 41) / 1.5)
        for i, (name, val, col) in enumerate((('monolith', 1.0, a(BONE, 0.6)), ('inproc / spawn', 1.33, GLUE))):
            y = 560 + i * 170
            E.text(name, 520, y, 34, BONE, font='mono', wght=700, align='right')
            w = 900 * val / 1.33 * u
            E.rect(560 + w / 2, y, w, 110, col, r=14)
            E.text(f'{val:.2f}×', 560 + w + 30, y, 44, a(BONE, clamp01(u * 3)), wght=900, align='left')
        E.text('under heavy load, with the backend split off', 960, 960, 28, a(BONE, 0.7), font='mono')
    else:  # many clients orbiting one server
        plate(E, f, 'ink')
        zoom = 1 + 0.25 * in_expo((lb - 58) / 6)
        screen_cam(E, f, zoom=zoom + 0.01 * f.kick, rot=0.05 * in_expo((lb - 58) / 6))
        apps = ['browser', 'terminal', 'game', 'compositor', 'video', 'editor']
        n = min(6, max(0, int(lb - 48) + 1))
        balls = [(960, 560, 150 + 10 * f.kick)]
        caps = []
        pos = []
        for k in range(n):
            ang = k * 2 * math.pi / 6 + f.lt * 0.35
            u = out_back((lb - 48 - k) / 0.5)
            R = 400 * u
            x, y = 960 + math.cos(ang) * R * 1.25, 560 + math.sin(ang) * R * 0.8
            balls.append((x, y, 70))
            caps.append((960, 560, x, y, 14, 14))
            pos.append((x, y, apps[k]))
        goo(E, balls, caps, k=40, glow=0.2)
        label_chip(E, 960, 560, 'SERVER', size=30)
        for x, y, s in pos:
            label_chip(E, x, y, s, size=22)
        if lb >= 56:
            uu = clamp01((lb - 56) / 0.5)
            E.text('one server · many apps · a whole desktop', 960, 1000, 36, a(GLUE, uu), font='mono', wght=700)
    return P


def run_ahead(E, f, u):
    """6 beats. Time advances one slot per 8th, snapping: the monolith app alternates work and waiting, the split
    app never waits while the GPU follows one slot behind."""
    P = {'bloom': 0.55, 'grain': 0.032, 'ca': 0.4 * f.kick}
    plate(E, f, 'ink')
    tb0 = f.t - u * S.BEAT
    slam(E, f, 'RUN-AHEAD', 120, 150, tb0, 150, BONE, align='left', wdth=110)
    E.text('the client keeps going while the server catches up', 130, 250, 28, a(GLUE, 0.9), font='mono', wght=600,
           align='left')
    n = min(12, int(u * 2) + 1)               # slots shown so far
    frac = (u * 2) % 1
    pop = out_back(clamp01(frac / 0.35), 2.0)
    screen_cam(E, f, zoom=1 + 0.012 * f.kick + 0.006 * (1 - clamp01(frac / 0.3)))
    X0, SW = 440, 94
    lanes = [('APP · monolith', 430), ('GPU', 515), ('APP · split', 700), ('GPU', 785)]
    for name, y in lanes:
        E.text(name, 130, y, 26, a(BONE, 0.85), font='mono', wght=700, align='left')
        E.line(X0 - 20, y + 36, X0 + 12 * SW, y + 36, 2, a(MOSS, 0.8))
    waits = 0
    for k in range(n):
        sc_ = pop if k == n - 1 else 1.0
        x = X0 + k * SW + SW / 2
        # monolith: the app works, then waits while the GPU draws that frame
        if k % 2 == 0:
            E.rect(x, 430, (SW - 10) * sc_, 58 * sc_, GLUE, r=8)
        else:
            waits += 1
            E.rect(x, 430, (SW - 10) * sc_, 58 * sc_, a(BONE, 0.14), r=8)
            E.text('wait', x, 430, 20 * sc_, a(BONE, 0.6), font='mono', wght=700)
            E.rect(x, 515, (SW - 10) * sc_, 58 * sc_, MINT, r=8)
        # split: the app records every slot, the server draws one slot behind
        E.rect(x, 700, (SW - 10) * sc_, 58 * sc_, GLUE, r=8)
        if k >= 1:
            E.rect(x, 785, (SW - 10) * sc_, 58 * sc_, MINT, r=8)
    # playhead
    px = X0 + n * SW - (1 - pop) * 0.0
    E.line(px, 380, px, 830, 4, a(NEON, 0.9))
    E.text(f'app waited: {waits}', X0 + 12 * SW + 40, 472, 30, a(CORAL, 0.95), font='mono', wght=800, align='left')
    E.text('app waited: 0', X0 + 12 * SW + 40, 742, 30, GLUE, font='mono', wght=800, align='left')
    return P


def vf(f, vlb):
    """A copy of the frame whose local clock reads vlb beats (beat pulses stay on the real clock)."""
    import copy
    g = copy.copy(f)
    d = (vlb - f.lbeat) * S.BEAT
    g.lbeat, g.lt, g.t = vlb, f.lt + d, f.t + d
    return g


WORDS2 = [('MONOLITH', 'one process'), ('INPROC', 'same process, through the pipe'), ('SPAWN + SHM', 'a child process'),
          ('TCP', 'another machine'), ('ONE SERVER', 'owns the GPU'), ('MANY APPS', 'one per client'),
          ('A WHOLE DESKTOP', 'compositor included'), ('ANY MACHINE', 'over the network')]


def modes(E, f):
    """Second drop, accelerating: 4-6 beat ideas, then 1-beat words, then doubling clients, then the vortex."""
    lb = f.lbeat
    tb_ = lambda b: f.t - (lb - b) * S.BEAT
    if lb < 4:
        return modes_v1(E, f)
    if lb < 8:
        return modes_v1(E, vf(f, 8 + (lb - 4) * 1.5))
    if lb < 14:
        return modes_v1(E, vf(f, 16 + (lb - 8)))
    if lb < 17:
        return modes_v1(E, vf(f, 24 + (lb - 14)))
    if lb < 18:
        return modes_v1(E, vf(f, 28 + (lb - 17)))
    if lb < 24:
        return run_ahead(E, f, lb - 18)
    if lb < 28:
        return modes_v1(E, vf(f, 40 + (lb - 24) * 2))
    P = {'bloom': 0.6, 'grain': 0.032, 'ca': 0.45 * f.kick}
    if lb < 36:   # clients join on 8ths, the orbit speeds up
        plate(E, f, 'ink')
        c = climax(f, 28, 36)
        screen_cam(E, f, zoom=1.05 - 0.1 * c + 0.012 * f.kick)
        apps = ['browser', 'terminal', 'game', 'compositor', 'video', 'editor', 'launcher', 'IDE', 'player', 'chat',
                'office', 'shell']
        n = min(12, int((lb - 28) * 2) + 1)
        balls = [(960, 540, 150 + 12 * f.kick)]
        caps, pos = [], []
        spin = f.lt * 0.4 + 2.0 * c * c
        for k in range(n):
            ring = 0 if k < 6 else 1
            ang = (k % 6) * math.pi / 3 + spin * (1 if ring == 0 else -0.7) + ring * 0.5
            u = out_back((lb - 28 - k * 0.5) / 0.35)
            R = (330 + 190 * ring) * u
            x, y = 960 + math.cos(ang) * R * 1.35, 540 + math.sin(ang) * R * 0.8
            balls.append((x, y, 58 - 8 * ring))
            caps.append((960, 540, x, y, 12, 12))
            pos.append((x, y, apps[k]))
        goo(E, balls, caps, k=40, glow=0.2 + 0.2 * c)
        chip(E, 960, 540, 'SERVER', 30, INK, PALE)
        for x, y, s in pos:
            chip(E, x, y, s, 20, INK, BONE)
        E.text(f'{n} clients', 960, 1010, 34, GLUE, font='mono', wght=800)
    elif lb < 44:   # one word a beat
        kind, k = beat_plate(f, ['ink', 'glue', 'ink', 'mint'], 36)
        b, fg = plate(E, f, kind, grid=False)
        w, cap = WORDS2[k % 8]
        screen_cam(E, f, zoom=1 + 0.05 * (lb % 1) + 0.02 * f.kick, rot=(0.02 if k % 2 else -0.02))
        speedlines(E, f, 0.3 + 0.2 * climax(f, 36, 44), a(fg, 0.45), seed=k + 40)
        slam(E, f, w, 960, 500, tb_(36 + k), fit_size(E, w, 1600, cap=380), fg if kind != 'ink' else NEON, punch=0.22)
        E.text(cap, 960, 780, 40, a(fg, 0.8), font='mono', wght=700)
    elif lb < 56:   # clients doubling every beat
        plate(E, f, 'ink')
        k = int(lb - 44)
        fr = lb - 44 - k
        n_prev = 2 ** min(k, 6)
        split = out_back(clamp01(fr / 0.45))
        cols = 8
        n_draw = min(64, 2 ** min(k + 1, 6))
        balls = []
        for j in range(n_draw):
            parent = j // 2 if k < 6 else j
            if k < 6:
                # parent positions laid out on a grid sized for n_prev, children slide apart
                pc = min(n_prev, 64)
                gx = max(1, int(math.ceil(math.sqrt(pc * 16 / 9))))
                px = 960 + ((parent % gx) - (gx - 1) / 2) * 1500 / gx
                py = 540 + ((parent // gx) - (math.ceil(pc / gx) - 1) / 2) * 820 / max(1, math.ceil(pc / gx))
                r = min(140, 620 / gx)
                off = (j % 2 - 0.5) * r * 1.2 * split
                balls.append((px + off, py, r * (1 - 0.25 * split)))
            else:
                x = 960 + ((j % cols) - 3.5) * 200
                y = 540 + ((j // cols) - 3.5) * 110
                wave = math.sin(lb * math.pi * 2 - j * 0.4)
                balls.append((x, y + 10 * wave, 38 + 8 * f.kick))
        screen_cam(E, f, zoom=1.0 + 0.02 * f.kick + 0.08 * climax(f, 50, 56), shake=3 * climax(f, 50, 56))
        goo(E, balls, k=24 if k < 6 else 16, glow=0.25, outline=5, bevel=24)
        E.cam, E.shake = (LW / 2, LH / 2, 1.0, 0.0), (0.0, 0.0)
        E.rect(960, 1010, 640, 76, INK, r=38, stroke=a(GLUE, 0.7), sw=3)
        E.text('more clients · one server', 960, 1010, 36, NEON, font='mono', wght=800)
        P['bloom'] = 0.7
    else:   # vortex climax
        c = climax(f, 56, 63)
        plate(E, f, 'ink', grid=False)
        speedlines(E, f, 0.4 + 0.6 * c, a(GLUE, 0.8), n=110, seed=11)
        screen_cam(E, f, zoom=1 + 0.9 * c + 0.03 * f.kick, rot=-0.08 * c, shake=4 + 16 * c)
        balls = [(960, 540, 120 + 140 * c)]
        for j in range(48):
            ang = j * 2.399 + f.lt * (0.8 + 7 * c)
            R = (220 + 700 * rnd('vx', j)) * (1 - 0.85 * c)
            balls.append((960 + math.cos(ang) * R * 1.5, 540 + math.sin(ang) * R * 0.85, 26 + 10 * rnd('vr', j)))
        goo(E, balls, k=50 + 60 * c, glow=0.25 + 0.5 * c)
        E.cam = (LW / 2, LH / 2, 1.0 + 0.04 * f.kick + 0.1 * c, 0.0)
        E.text('ONE SERVER.', 960, 200, 150, BONE, wght=900, wdth=100 + 25 * c)
        E.text('EVERY APP.', 960, 900, 150, NEON, wght=900, wdth=100 + 25 * c)
        P['ca'] = 0.5 + 2.0 * c
        P['bloom'] = 0.6 + 0.4 * c
    return P
