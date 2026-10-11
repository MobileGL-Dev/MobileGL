"""Bars 48-56: MGPipe over the snare roll. Only what changed, no round trips, deduplicated."""
import math
from anim import *
from kit import *
from devices import *
from engine import LW, LH, hexc

ROLLS = [t for t, _ in S.events('roll') if S.tb(48) <= t < S.tb(56)]
LABELS = ['BIND', 'STATE Δ', 'DRAW', 'UNIFORM', 'CLEAR', 'DRAW', 'STATE Δ', 'BLIT', 'DRAW', 'FENCE']


def tube(E, f, y, x0=330, x1=1590, rate_scale=1.0):
    E.rect((x0 + x1) / 2, y, x1 - x0, 96, INK2, r=48, stroke=a(GLUE, 0.6), sw=4)
    speed = 900.0
    for k, ts in enumerate(ROLLS):
        if ts > f.t:
            break
        x = x0 + 40 + speed * (f.t - ts)
        if x > x1 - 40:
            continue
        lab = LABELS[k % len(LABELS)]
        w = E.measure(lab, 'mono', 20, 800) + 26
        E.rect(x, y, w, 46, GLUE if 'Δ' not in lab else NEON, r=23)
        E.text(lab, x, y, 20, INK, font='mono', wght=800)


def card(E, x, y, title, sub, col=GLUE, fg=INK, w=300, h=200):
    E.rect(x, y, w, h, col, r=26)
    E.text(title, x, y - 18, 44, fg, wght=900)
    E.text(sub, x, y + 40, 20, a(fg, 0.7), font='mono', wght=600)


ROWS = [('BLEND', 'off', 'on'), ('DEPTH_TEST', 'on', 'on'), ('VIEWPORT', '1920×1080', '1920×1080'),
        ('PROGRAM', '12', '12'), ('TEX0', '2D:7', '2D:9'), ('CULL_FACE', 'back', 'back'), ('VAO', '3', '3'),
        ('SCISSOR', 'off', 'off'), ('UBO0', '5', '5'), ('STENCIL', 'off', 'off')]


def pipe(E, f):
    lb = f.lbeat
    P = {'bloom': 0.55, 'grain': 0.035, 'ca': 0.2 + 0.6 * f.roll * clamp01(lb / 24)}
    tb_ = lambda b: f.t - (lb - b) * S.BEAT
    zoom = 1 + 0.06 * clamp01(lb / 31) + 0.01 * f.roll
    if lb < 8:
        plate(E, f, 'ink', drift=(-60, 0))
        screen_cam(E, f, zoom=zoom)
        slam(E, f, 'MGPIPE', 960, 300, tb_(0), 260, BONE, wdth=120)
        if lb >= 1:
            E.text('the stream between frontend and backend', 960, 450, 32, a(GLUE, clamp01(lb - 1)), font='mono',
                   wght=600)
        card(E, 200, 720, 'FRONT', 'your process', GLUE)
        card(E, 1720, 720, 'BACK', 'owns the GPU', MINT)
        tube(E, f, 720)
    elif lb < 16:
        plate(E, f, 'mint')
        screen_cam(E, f, zoom=zoom)
        slam(E, f, 'ONLY WHAT CHANGED.', 960, 150, tb_(8), 120, INK, wdth=105)
        beat = int(lb - 8)
        changed = {0: [0], 1: [4], 2: [0, 6], 3: [9], 4: [2], 5: [4, 7], 6: [1], 7: [3, 0]}
        live = changed.get(beat, [])
        for i, (name, v0, v1) in enumerate(ROWS):
            y = 290 + i * 70
            hot = i in live
            E.rect(470, y, 640, 58, INK if hot else a(PALE, 0.9), r=10)
            E.text(name, 170, y, 26, GLUE if hot else INK, font='mono', wght=700, align='left')
            val = (v1 if (beat + i) % 2 else v0) if hot else v0
            E.text(val, 770, y, 26, NEON if hot else a(INK, 0.6), font='mono', wght=700, align='right')
            if hot:
                u = (lb % 1) / 0.8
                x = lerp(800, 1500, in_out_cubic(u))
                yy = lerp(y, 620, in_out_cubic(u))
                if u < 1:
                    chip(E, x, yy, f'{name}={val}', 22, INK, GLUE)
        E.rect(1600, 620, 420, 300, INK, r=26)
        E.text('SENT', 1600, 520, 30, GLUE, font='mono', wght=800)
        E.text(f'{len(live)}', 1600, 630, 150, NEON, wght=900)
        E.text(f'of {len(ROWS)} fields', 1600, 730, 26, a(BONE, 0.7), font='mono')
    elif lb < 24:
        plate(E, f, 'ink')
        screen_cam(E, f, zoom=zoom)
        slam(E, f, 'NO ROUND TRIPS.', 960, 160, tb_(16), 130, BONE, wdth=105)
        card(E, 380, 560, 'CLIENT', 'frontend', GLUE, w=420, h=260)
        card(E, 1540, 560, 'SERVER', 'backend', MINT, w=420, h=260)
        code_line(E, 'glGenBuffers(1, &buf);', 170, 780, 32, chars=int(clamp01((lb - 16.3) / 1.2) * 22))
        if lb >= 18:
            u = out_back((lb - 18) / 0.35)
            chip(E, 380, 880, 'buf = { slot: 17, gen: 3 }', 30, INK, PALE, sc=u)
            E.text('minted on the client, instantly', 380, 960, 24, a(BONE, 0.7 * u), font='mono')
        if lb >= 19:
            arrow(E, 610, 520, 1310, 520, 10, GLUE, u=(lb - 19) / 0.5)
            E.text('records →', 960, 470, 26, GLUE, font='mono', wght=700)
        if lb >= 20:
            u = clamp01((lb - 20) / 0.5)
            for k in range(12):
                x0 = 1310 - k * 60
                if (1310 - x0) / 700 < u:
                    E.line(x0, 620, x0 - 30, 620, 8, a(BONE, 0.4))
            E.poly([(600, 620), (640, 600), (640, 640)], a(BONE, 0.4))
            if lb >= 21:
                cross(E, 960, 620, 80, CORAL, 18, (lb - 21) / 0.3)
                E.text('waiting for a reply', 960, 700, 26, a(CORAL, 0.9), font='mono', wght=700)
    elif lb < 28:
        plate(E, f, 'glue', grid=False)
        screen_cam(E, f, zoom=zoom)
        slam(E, f, 'DEDUPLICATED.', 960, 180, tb_(24), 150, INK, wdth=105)
        u = in_out_cubic((lb - 25) / 1.6)
        for k in range(6):
            ang = k * math.pi / 3 + 0.4
            x = 960 + math.cos(ang) * 520 * (1 - u)
            y = 640 + math.sin(ang) * 260 * (1 - u)
            E.rect(x, y, 360, 220, INK, r=24, rot=0.1 * math.sin(k) * (1 - u))
            for L in range(3):
                E.rect(x - 60 + 20 * (L % 2), y - 50 + L * 50, 220 - 40 * (L % 2), 22, a(GLUE, 0.8), r=8,
                       rot=0.1 * math.sin(k) * (1 - u))
        if lb >= 26.6:
            chip(E, 1140, 520, '×6 → 1', 40, INK, PALE, sc=out_back((lb - 26.6) / 0.3), font='archivo', wght=900)
    elif lb < 31:
        words = ['RECORD', 'DIFF', 'DEDUPE', 'SEND', 'RECORD', 'DIFF']
        k = min(int((lb - 28) * 2), 5)
        kind = ['ink', 'ink', 'glue', 'glue', 'mint', 'mint'][k]
        b, fg = plate(E, f, kind, grid=False)
        slam(E, f, words[k], 960, 560, tb_(28 + k * 0.5), fit_size(E, words[k], 1600, cap=520), fg if kind != 'ink' else NEON,
             punch=0.2)
        P['ca'] = 1.0 + 0.3 * k
    else:
        bg(E, hexc('#000000'))
    return P
