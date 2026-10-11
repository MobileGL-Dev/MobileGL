"""Bars 72-80: the breakdown. Device lost (not a crash), memory stays yours, zero-copy."""
import math
from anim import *
from kit import *
from devices import *
from engine import LW, LH, hexc


def soft_words(E, f, words, b0, step, x, y, size, col, align='center', font='archivo-i', wght=300):
    """Words fade/slide in one per `step` beats, laid out on one line."""
    total = sum(E.measure(w + ' ', font, size, wght) for w in words)
    cx = x - total / 2 if align == 'center' else x
    for i, w in enumerate(words):
        bi = b0 + i * step
        u = out_cubic((f.lbeat - bi) / 0.8)
        adv = E.measure(w + ' ', font, size, wght)
        if u > 0:
            E.text(w, cx, y + 24 * (1 - u), size, a(col, u), font=font, wght=wght, align='left')
        cx += adv


def breakdown(E, f):
    lb = f.lbeat
    heart = S.pulse(f.t, 'heart', 0.15)
    P = {'bloom': 0.65, 'grain': 0.032, 'vignette': 0.5}
    plate(E, f, 'ink', drift=(4, 2))
    screen_cam(E, f, zoom=1.0 + 0.02 * math.sin(f.lt * 0.4) + 0.015 * heart)
    if lb < 16:
        # server + clients orbiting slowly; client 2 fails
        apps = ['browser', 'terminal', 'game', 'compositor', 'video', 'editor']
        balls = [(960, 600, 120)]
        caps = []
        bad = None
        for k in range(6):
            ang = k * 2 * math.pi / 6 + f.t * 0.25
            x, y = 960 + math.cos(ang) * 470, 600 + math.sin(ang) * 260
            if k == 2 and lb >= 8:
                bad = (x, y)
                fall = clamp01((lb - 10) / 4)
                x += 60 * fall
                y += 500 * in_cubic(fall)
                continue
            balls.append((x, y, 56))
            caps.append((960, 600, x, y, 10, 10))
        goo(E, balls, caps, k=36, glow=0.15)
        if bad is not None:
            fall = clamp01((lb - 10) / 4)
            jit = 6 * math.sin(f.t * 60) * (lb < 10)
            bx, by = bad[0] + 60 * fall + jit, bad[1] + 500 * in_cubic(fall)
            if lb < 10:   # strand still attached, straining
                goo(E, [(bx, by, 56)], [(960, 600, bx, by, 6, 6)], k=20, col=CORAL, rim=hexc('#9c2f22'), glow=0.2)
            else:
                goo(E, [(bx, by, 56)], k=20, col=a(CORAL, 1 - 0.6 * fall), rim=hexc('#9c2f22'))
            if lb >= 11:
                stamp(E, f, 'DEVICE LOST', bx + 40, by - 110, f.t - (lb - 11) * S.BEAT, 46, CORAL, rot=0.1)
        for k in range(6):
            if k == 2 and lb >= 8:
                continue
            ang = k * 2 * math.pi / 6 + f.t * 0.25
            chip(E, 960 + math.cos(ang) * 470, 600 + math.sin(ang) * 260, apps[k], 20, INK, BONE)
        chip(E, 960, 600, 'SERVER', 26, INK, PALE)
        if lb < 8:
            soft_words(E, f, ['and', 'when', 'things', 'go', 'wrong?'], 0.5, 1.0, 960, 170, 84, BONE)
        else:
            if lb >= 12:
                soft_words(E, f, ['a', 'clean', 'device-lost.', 'not', 'a', 'crash.'], 12, 0.5, 960, 150, 72, BONE)
            if lb >= 14:
                E.text('everyone else keeps rendering', 960, 1010, 30, a(GLUE, clamp01((lb - 14) / 0.6)), font='mono',
                       wght=600)
    elif lb < 24:
        soft_words(E, f, ['your', 'memory', 'stays', 'yours.'], 16, 0.75, 960, 160, 90, BONE)
        # client memory grid
        E.rect(480, 620, 560, 520, INK2, r=26, stroke=a(GLUE, 0.6), sw=3)
        E.text('CLIENT MEMORY', 480, 400, 26, GLUE, font='mono', wght=800)
        for r in range(7):
            for c in range(8):
                on = rnd('mem', r, c, int(f.t * 4)) > 0.6
                E.rect(270 + c * 60, 470 + r * 56, 46, 42, a(GLUE, 0.85 if on else 0.18), r=6)
        E.rect(1440, 620, 460, 360, INK2, r=26, stroke=a(MINT, 0.6), sw=3)
        E.text('SERVER', 1440, 560, 54, MINT, wght=900)
        E.text('backend', 1440, 620, 24, a(MINT, 0.7), font='mono')
        # only records travel; reaching in is walled off
        for k in range(5):
            ph = ((lb - 16) / 2 + k / 5) % 1
            E.rect(lerp(800, 1180, ph), 560, 54, 26, GLUE, r=8)
        if lb >= 18:
            u = clamp01((lb - 18) / 0.6)
            arrow(E, 1200, 720, lerp(1200, 860, u), 720, 8, a(CORAL, 0.9), u=1.0)
            E.line(830, 640, 830, 800, 12, a(BONE, u))
            E.text('the backend never reads frontend memory', 960, 1000, 30, a(BONE, 0.8 * u), font='mono', wght=600)
    else:
        soft_words(E, f, ['zero-copy', 'where', 'it', 'counts.'], 24, 0.75, 960, 150, 90, BONE)
        # one window buffer in GPU memory, shared by the app (writes) and the compositor (reads)
        k = max(0, int(lb - 25) + 1) if lb >= 25 else 0          # frames rendered so far
        bx, by, bw, bh = 960, 560, 420, 280

        def frame_art(cx, cy, w, h, n, alpha=1.0):
            E.rect(cx, cy, w, h, a(PALE, alpha), r=12)
            E.rect(cx, cy - h / 2 + 18 * h / 280, w, 36 * h / 280, a(MINT, alpha), r=10)
            ang = n * 0.9
            E.circle(cx + math.cos(ang) * w * 0.25, cy + 20 * h / 280 + math.sin(ang) * h * 0.18, 34 * h / 280,
                     a(GLUE, alpha))
            E.text(f'frame {n}', cx + w * 0.3, cy + h * 0.36, 20 * h / 280, a(INK, 0.7 * alpha), font='mono', wght=700)
        # the shared buffer
        E.rect(bx, by, bw + 40, bh + 90, INK2, r=20, stroke=a(GLUE, 0.8), sw=4)
        E.text('one window buffer · GPU memory', bx, by - bh / 2 - 22, 22, GLUE, font='mono', wght=700)
        frame_art(bx, by + 10, bw, bh, max(k, 1) if lb >= 25 else 0)
        # app (left) and compositor (right)
        E.rect(300, 560, 320, 200, INK2, r=20, stroke=a(BONE, 0.5), sw=3)
        E.text('APP', 300, 520, 50, BONE, wght=900)
        E.text('draws into it', 300, 585, 22, a(BONE, 0.7), font='mono')
        E.rect(1620, 560, 320, 260, INK2, r=20, stroke=a(BONE, 0.5), sw=3)
        E.text('COMPOSITOR', 1620, 470, 38, BONE, wght=900)
        if lb >= 25.5:
            frame_art(1620, 590, 240, 160, max(k, 1), 0.95)    # what it shows IS the shared buffer
        # both point at the same memory
        E.line(460, 560, bx - bw / 2 - 30, 560, 5, a(GLUE, 0.7))
        E.line(bx + bw / 2 + 30, 560, 1460, 560, 5, a(GLUE, 0.7))
        # each beat: only a small handle travels across, the pixels stay where they are
        if lb >= 25:
            u = clamp01(((lb - 25) % 1) / 0.6)
            hx = lerp(460, 1460, in_out_cubic(u))
            hy = 470 - 90 * bump(u)
            if u < 1:
                chip(E, hx, hy, 'handle', 22, INK, GLUE)
            pulse = math.exp(-((lb - 25) % 1) * 5)
            E.rect(bx, by, bw + 40 + 30 * pulse, bh + 90 + 30 * pulse, (0, 0, 0, 0), r=24, stroke=a(NEON, 0.6 * pulse),
                   sw=4)
        # counters
        E.text(f'frames shown: {k}', 760, 900, 34, BONE, font='mono', wght=800)
        E.text('copies made: 0', 1180, 900, 34, NEON, font='mono', wght=800)
        if lb >= 27:
            E.text('the app and the compositor share one buffer: handed over, never copied', 960, 1000, 28,
                   a(GLUE, clamp01((lb - 27) / 0.6)), font='mono', wght=600)
    return P
