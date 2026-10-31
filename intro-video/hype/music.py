"""Hype electro-pop instrumental for the MobileGL showreel, synthesised from score.py.
usage: python music.py out.wav"""
import sys
import numpy as np
from scipy.signal import fftconvolve, butter, sosfiltfilt
from dsp import SR, saw, pulse, sine, svf, onepole_hp, pingpong, compress, limit, mtof, adsr
import score as S

N = int(S.END * SR)
rng = np.random.default_rng(1234)


def idx(t):
    return int(round(t * SR))


class Bus:
    def __init__(self, name):
        self.name = name
        self.l = np.zeros(N)
        self.r = np.zeros(N)

    def add(self, t, sig, pan=0.0, gain=1.0):
        """sig: mono array or (l, r) tuple; pan -1..1 (equal-power)."""
        i0 = idx(t)
        if isinstance(sig, tuple):
            l, r = sig
        else:
            a = (pan + 1) * np.pi / 4
            l, r = sig * np.cos(a) * 1.4142, sig * np.sin(a) * 1.4142
        if i0 < 0:
            l, r, i0 = l[-i0:], r[-i0:], 0
        n = min(len(l), N - i0)
        if n <= 0:
            return
        self.l[i0:i0 + n] += gain * l[:n]
        self.r[i0:i0 + n] += gain * r[:n]

    def apply(self, g):
        self.l *= g
        self.r *= g


def noise(n):
    return rng.standard_normal(n)


def tarr(n):
    return np.arange(n) / SR


# --------------------------------------------------------------------------------------------------
# automation helpers
def curve(points):
    """Piecewise-exponential-ish automation over the whole song from [(time, value)] (log interp)."""
    ts = np.array([p[0] for p in points])
    vs = np.log(np.array([p[1] for p in points]))
    return np.exp(np.interp(np.arange(N) / SR, ts, vs))


def lin_curve(points):
    ts = np.array([p[0] for p in points])
    vs = np.array([p[1] for p in points])
    return np.interp(np.arange(N) / SR, ts, vs)


def sidechain(depth, rel=0.16):
    """Gain curve ducking on every kick."""
    g = np.ones(N)
    kn = int(0.6 * SR)
    t = tarr(kn)
    shape = 1 - depth * np.where(t < 0.004, t / 0.004, np.exp(-(np.maximum(t - 0.004, 0) / rel) ** 1.6))
    for tk, v in S.events('kick'):
        i0 = idx(tk)
        n = min(kn, N - i0)
        g[i0:i0 + n] = np.minimum(g[i0:i0 + n], 1 - (1 - shape[:n]) * min(v, 1.0))
    return g


def gaps():
    """Mute window (one beat of silence) before each drop."""
    g = np.ones(N)
    for b in (16, 56, 88):
        a0, a1 = idx(S.tb(b - 1, 3)), idx(S.tb(b))
        f = int(0.006 * SR)
        g[a0:a1] = 0
        g[a0 - f:a0] = np.linspace(1, 0, f)
        g[a1 - f // 2:a1] = 0
    return g


# --------------------------------------------------------------------------------------------------
# drums
def kick(v):
    n = int(0.5 * SR)
    t = tarr(n)
    f = 46 + 130 * np.exp(-t / 0.032) + 40 * np.exp(-t / 0.006)
    body = sine(f, 0.0) * (np.exp(-t / 0.32) * np.minimum(t / 0.002, 1))
    click = svf(noise(n), np.full(n, 3500.0), 0.2, 1) * np.exp(-t / 0.004) * 0.6
    x = np.tanh(1.6 * (body + click)) * 0.95
    return x * v


def clap(v):
    n = int(0.35 * SR)
    t = tarr(n)
    nz = svf(noise(n), np.full(n, 1300.0), 0.35, 1) * 2.2
    env = np.zeros(n)
    for k, d in enumerate((0.0, 0.009, 0.018, 0.027)):
        m = t >= d
        env[m] += np.exp(-(t[m] - d) / (0.006 if k < 3 else 0.11)) * (0.8 if k < 3 else 1.0)
    body = sine(np.full(n, 190.0) + 40 * np.exp(-t / 0.01), 0.25) * np.exp(-t / 0.05) * 0.35
    return (nz * env + body) * v * 0.8


def snare(v, pitch=1.0):
    n = int(0.25 * SR)
    t = tarr(n)
    nz = svf(noise(n), np.full(n, 4200.0 * pitch), 0.1, 1) * 1.8 * np.exp(-t / 0.07)
    body = sine(np.full(n, 200.0 * pitch) + 60 * np.exp(-t / 0.008), 0.0) * np.exp(-t / 0.06)
    return (nz * 0.7 + body * 0.6) * v


METAL = np.array([205.3, 304.4, 369.6, 522.7, 540.0, 800.0]) * 1.7


def metal(n):
    x = np.zeros(n)
    for f in METAL:
        x += pulse(np.full(n, f), 0.5, rng.random())
    return x / len(METAL)


def hat(v, decay=0.035, n_s=0.12):
    n = int(n_s * SR)
    t = tarr(n)
    x = 0.55 * metal(n) + 0.6 * noise(n)
    x = svf(x, np.full(n, 8500.0), 0.2, 2)
    return x * np.exp(-t / decay) * v * 0.55


def shaker(v):
    n = int(0.09 * SR)
    t = tarr(n)
    x = svf(noise(n), np.full(n, 6500.0), 0.3, 1)
    env = np.minimum(t / 0.012, 1) * np.exp(-t / 0.03)
    return x * env * v * 0.5


def crash(v):
    n = int(2.6 * SR)
    t = tarr(n)
    x = 0.5 * metal(n) + noise(n)
    x = svf(x, np.full(n, 5200.0), 0.1, 2)
    xl = x * np.exp(-t / 0.75)
    xr = np.roll(x, 37) * np.exp(-t / 0.7)
    return (xl * v * 0.3, xr * v * 0.3)


def impact(v):
    n = int(3.0 * SR)
    t = tarr(n)
    boom = sine(30 + 45 * np.exp(-t / 0.35), 0.0) * np.exp(-t / 0.9) * np.minimum(t / 0.003, 1)
    crack = svf(noise(n), 6000 * np.exp(-t / 0.25) + 120, 0.2, 0) * np.exp(-t / 0.5)
    x = np.tanh(1.3 * boom) * 0.9 + crack * 0.35
    return x * v


def riser(dur, v):
    n = int(dur * SR)
    t = tarr(n)
    u = t / dur
    nz = svf(noise(n), 300 * (9000 / 300) ** (u ** 1.4), 0.55, 1)
    tone = saw(110 * 2 ** (u ** 1.3 * 2), 0.0) * 0.15
    tone = svf(tone, 400 + 5000 * u ** 2, 0.3, 0)
    env = u ** 2.2
    x = (nz * 0.9 + tone) * env * v * 0.55
    return (x, np.roll(x, 211))


def reverse_swell(v, dur):
    """Reversed crash-ish swell that ends exactly on the drop."""
    l, r = crash(1.0)
    n = int(dur * SR)
    return (l[:n][::-1] * v * 1.4, r[:n][::-1] * v * 1.4)


# --------------------------------------------------------------------------------------------------
# tonal voices
DETUNE = np.array([-0.24, -0.15, -0.06, 0.0, 0.06, 0.15, 0.24])   # semitones
PANS = np.array([-0.9, -0.6, -0.3, 0.0, 0.3, 0.6, 0.9])


def supersaw(midi, dur, v, att=0.006, rel=0.25, spread=1.0):
    n = int((dur + rel) * SR)
    l = np.zeros(n)
    r = np.zeros(n)
    for d, p in zip(DETUNE * spread, PANS):
        x = saw(np.full(n, float(mtof(midi + d))), rng.random())
        a = (p + 1) * np.pi / 4
        l += x * np.cos(a)
        r += x * np.sin(a)
    env = adsr(n, att, 0.3, 0.75, int(dur * SR), rel)
    return (l * env * v / 3.2, r * env * v / 3.2)


def pluck(midi, dur, v, bright=1.0):
    n = int((dur + 0.35) * SR)
    t = tarr(n)
    f = np.full(n, float(mtof(midi)))
    x = 0.6 * saw(f, rng.random()) + 0.4 * pulse(f * 1.003, 0.35, rng.random())
    cut = 250 + bright * 4200 * np.exp(-t / 0.09)
    x = svf(x, cut, 0.25, 0)
    return x * np.exp(-t / 0.22) * np.minimum(t / 0.002, 1) * v


def keys(midi, dur, v):
    """FM electric piano stab."""
    n = int((dur + 0.4) * SR)
    t = tarr(n)
    f = float(mtof(midi))
    idx_env = 2.2 * np.exp(-t / 0.15) + 0.4
    mod = sine(np.full(n, f), 0.0) * idx_env
    car = np.sin(2 * np.pi * f * t + mod)
    tine = sine(np.full(n, f * 14.0), 0.0) * np.exp(-t / 0.03) * 0.15
    env = np.exp(-t / 0.6) * np.minimum(t / 0.002, 1)
    rs = int(dur * SR)
    if rs < n:
        env[rs:] *= np.exp(-(t[rs:] - t[rs]) / 0.06)
    return (car + tine) * env * v


def bell(midi, dur, v):
    n = int((dur + 1.6) * SR)
    t = tarr(n)
    f = float(mtof(midi))
    mod = sine(np.full(n, f * 3.5), 0.0) * (1.4 * np.exp(-t / 0.4) + 0.1)
    car = np.sin(2 * np.pi * f * t + mod)
    sub = np.sin(2 * np.pi * f * t) * 0.5
    env = np.exp(-t / 1.1) * np.minimum(t / 0.004, 1)
    return (car * 0.6 + sub) * env * v


def mono_line(notes, kind, glide=0.035, vib=0.12):
    """Monophonic lead over a whole note list [(t, dur, midi, vel)] -> (l, r) full-length arrays (sparse
    regions zero). Glides between notes that touch; vibrato fades in on held notes."""
    if not notes:
        return np.zeros(N), np.zeros(N)
    notes = sorted(notes)
    t0 = notes[0][0] - 0.05
    t1 = notes[-1][0] + notes[-1][1] + 0.6
    i0, i1 = idx(t0), min(idx(t1), N)
    n = i1 - i0
    tt = (np.arange(n) + i0) / SR
    pitch = np.full(n, float(notes[0][2]))
    gate = np.zeros(n)
    vel = np.zeros(n)
    since = np.full(n, 10.0)
    for k, (ts, d, m, v) in enumerate(notes):
        a, b = idx(ts) - i0, min(idx(ts + d) - i0, n)
        nxt = idx(notes[k + 1][0]) - i0 if k + 1 < len(notes) else n
        seg = slice(a, min(nxt, n))
        prev = pitch[a - 1] if a > 0 else m
        u = (tt[seg] - ts)
        pitch[seg] = m + (prev - m) * np.exp(-u / glide) if k > 0 and notes[k - 1][0] + notes[k - 1][1] >= ts - 0.02 else m
        gate[a:b] = 1
        vel[a:min(nxt, n)] = v
        since[seg] = u
    vibr = vib * np.sin(2 * np.pi * 5.6 * tt) * np.clip((since - 0.18) / 0.25, 0, 1)
    f = mtof(pitch + vibr)
    # amp: attack 4ms, release 70ms (one-pole on gate)
    up, dn = np.exp(-1 / (0.004 * SR)), np.exp(-1 / (0.07 * SR))
    from scipy.signal import lfilter
    att = lfilter([1 - up], [1, -up], gate)
    rel = lfilter([1 - dn], [1, -dn], gate)
    env = np.where(gate > 0, att, rel) * vel
    if kind == 'lead':
        x = 0.45 * saw(f * 2 ** (0.08 / 12), 0.1) + 0.45 * saw(f * 2 ** (-0.08 / 12), 0.6) + 0.35 * pulse(f, 0.3, 0.3)
        cut = 900 + 5200 * np.clip(env, 0, 1) * (0.55 + 0.45 * np.exp(-np.minimum(since, 5) / 0.15))
        x = svf(x, cut, 0.3, 0)
        sub = sine(f / 2, 0.0) * 0.25
        x = (x + sub) * env
        l = x + 0.0
        r = 0.45 * saw(f * 2 ** (0.12 / 12), 0.9)
        r = svf(r, cut, 0.3, 0) * env * 0.6 + x * 0.7
        l = l * 1.0
        out = (l, r)
    else:  # 'chop': vowel-ish formant voice
        src = saw(f, 0.2) * 0.7 + pulse(f, 0.5, 0.0) * 0.3
        vowel = 0.5 + 0.5 * np.sin(2 * np.pi * tt * S.BPM / 60 / 2)
        f1 = 700 - 300 * vowel
        f2 = 1220 - 400 * vowel
        x = svf(src, f1, 0.85, 1) + 0.7 * svf(src, f2, 0.85, 1) + 0.25 * svf(src, np.full(n, 2600.0), 0.8, 1)
        x = x * env * 1.6
        out = (x, x)
    L, R = np.zeros(N), np.zeros(N)
    L[i0:i1] = out[0][:n]
    R[i0:i1] = out[1][:n]
    return L, R


# --------------------------------------------------------------------------------------------------
def build():
    B = {k: Bus(k) for k in ('kick', 'drums', 'bass', 'chords', 'pad', 'arp', 'lead', 'keys', 'fx')}

    # --- drums / fx from the shared event list
    for t, k, v in S.EVENTS:
        if k == 'kick':
            B['kick'].add(t, kick(v))
        elif k == 'clap':
            B['drums'].add(t, clap(v), 0.0, 0.7)
        elif k == 'snare':
            B['drums'].add(t, snare(v), 0.1, 0.6)
        elif k == 'roll':
            B['drums'].add(t, snare(v, 1.0 + 0.5 * v), 0.0, 0.38)
        elif k == 'hat':
            B['drums'].add(t, hat(v), 0.25, 0.75)
        elif k == 'ohat':
            B['drums'].add(t, hat(v, 0.12, 0.35), -0.2, 0.6)
        elif k == 'shaker':
            B['drums'].add(t, shaker(v), -0.35, 0.8)
        elif k == 'crash':
            B['fx'].add(t, crash(v), gain=0.9)
        elif k == 'impact':
            B['fx'].add(t, impact(v), gain=0.8)
        elif k == 'heart':
            B['kick'].add(t, kick(v) * 0.6)
            B['kick'].add(t + 0.16, kick(v) * 0.35)
        elif k == 'riser':
            B['fx'].add(t, riser(v, 1.0), gain=0.42)
        elif k == 'drop':
            B['fx'].add(t - S.BAR, reverse_swell(0.5, S.BAR), gain=0.6)

    # --- harmony
    for bar in range(S.NBARS):
        name, a, b = S.section(bar)
        i = bar - a
        voicing, root = S.chord(bar)
        t = S.tb(bar)
        drop = name.startswith('drop')
        # pads
        if name in ('intro', 'build1', 'verse', 'break', 'build2', 'build3', 'outro') or drop:
            pv = {'intro': 1.2, 'build1': 0.45, 'verse': 0.3, 'break': 0.6, 'build2': 0.4, 'build3': 0.4,
                  'outro': 0.5}.get(name, 0.3)
            for m in voicing:
                B['pad'].add(t, supersaw(m - 12 if m > 60 else m, S.BAR, pv * 0.35, att=0.35, rel=0.8,
                                         spread=0.6))
        # big chords
        if drop or name in ('build2', 'build3') or (name == 'build1' and i >= 4) or (name == 'outro'):
            vv = 1.0 if drop else 0.45
            if name == 'outro' and i >= 4:
                if i == 4:
                    for m in voicing + (voicing[0] + 12, voicing[1] + 12):
                        B['chords'].add(t, supersaw(m, 4 * S.BAR - 0.2, 0.9, rel=1.5))
            else:
                if drop:
                    # rhythmic stabs: the classic pumping offbeat-driven pattern
                    for s, l in ((0, 3), (3, 3), (6, 4), (10, 2), (12, 4)):
                        for m in voicing + (voicing[2] + 12,):
                            B['chords'].add(S.tb(bar, 0, s), supersaw(m, l * S.S16 - 0.01, vv, rel=0.12))
                else:
                    for m in voicing + (voicing[2] + 12,):
                        B['chords'].add(t, supersaw(m, S.BAR - 0.02, vv, rel=0.2))
        # bass
        if name not in ('intro', 'break') and not (name == 'outro' and i >= 4):
            if drop or name == 'outro':
                for s in range(0, 16, 2):
                    m = root + (12 if s in (6, 14) and drop else 0)
                    B['bass'].add(S.tb(bar, 0, s + 1), bassnote(m, S.S16 * 1.6, 0.9), 0.0)
                B['bass'].add(t, subnote(root, S.BAR - 0.03, 0.9))
            elif name == 'verse':
                for s, l, o in ((0, 3, 0), (3, 2, 12), (6, 2, 0), (8, 3, 0), (11, 1, 12), (12, 2, 7), (14, 2, 12)):
                    B['bass'].add(S.tb(bar, 0, s), bassnote(root + o, l * S.S16, 0.6))
                    B['bass'].add(S.tb(bar, 0, s), subnote(root + o if o != 12 else root, l * S.S16, 0.5))
            else:  # builds: plain quarter-note roots, saving the 8th-note pump for the drops
                bv = 0.7 if name == 'build1' else 0.55 + 0.25 * (i / 8)
                for q in range(4):
                    B['bass'].add(S.tb(bar, q), bassnote(root, S.BEAT * 0.8, bv))
                    B['bass'].add(S.tb(bar, q), subnote(root, S.BEAT * 0.8, bv * 0.85))
        elif name == 'outro' and i == 4:
            B['bass'].add(t, subnote(root, 3 * S.BAR, 0.9))
        # arp
        if name in ('intro', 'build1', 'verse', 'build2', 'build3', 'break') or drop:
            if name == 'intro' and i < 2:
                continue
            tones = [voicing[0] + 12, voicing[1] + 12, voicing[2] + 12, voicing[0] + 24]
            order = [0, 1, 2, 3, 2, 1, 2, 3, 0, 1, 2, 3, 2, 3, 1, 2]
            step = 2 if name == 'break' else 1
            av = {'intro': 0.5, 'build1': 0.4, 'verse': 0.42, 'break': 0.32}.get(name, 0.3)
            for s in range(0, 16, step):
                if name in ('intro', 'build1', 'build2', 'build3'):
                    bright = 0.25 + 0.75 * (i + s / 16) / (b - a)
                else:
                    bright = 0.7
                B['arp'].add(S.tb(bar, 0, s), pluck(tones[order[s]], S.S16, av * (1.0 if s % 4 == 0 else 0.75),
                                                    bright), pan=0.35 * np.sin(s * 0.9))
        # keys (verse stabs, break bells)
        if name == 'verse':
            for s in (2, 6, 10, 14) if i % 2 else (2, 6, 7, 10, 14):
                for m in voicing:
                    B['keys'].add(S.tb(bar, 0, s), keys(m + 12, S.S16 * 1.5, 0.22), pan=-0.2)
        if name == 'break':
            BREAK = [[(0, 4, 72), (4, 4, 69), (8, 8, 72)], [(0, 6, 77), (6, 2, 76), (8, 8, 72)],
                     [(0, 4, 74), (4, 4, 71), (8, 8, 74)], [(0, 6, 79), (6, 2, 77), (8, 8, 74)],
                     [(0, 4, 76), (4, 4, 72), (8, 8, 76)], [(0, 6, 81), (6, 2, 79), (8, 8, 76)],
                     [(0, 8, 77), (8, 8, 76)], [(0, 8, 74), (8, 4, 71), (12, 4, 74)]]
            for ts, d, m in S.notes_in(bar, BREAK[i]):
                B['keys'].add(ts, bell(m, d, 0.3), pan=0.1)

    # --- lead lines
    lead = []
    chop = []
    lead_hi = []
    for bar in range(S.NBARS):
        name, a, b = S.section(bar)
        i = bar - a
        if name.startswith('drop'):
            for ts, d, m in S.notes_in(bar, S.hook_bar(i)):
                lead.append((ts, d * 0.92, m, 1.0))
                if name != 'drop1' or i >= 8:
                    chop.append((ts, d * 0.92, m + 12, (0.6 if name == 'drop1' else 0.7 if name == 'drop2' else 0.85)
                                 * (1.0 if i >= 8 else 0.7)))
                if i >= 8:
                    lead_hi.append((ts, d * 0.92, m + 12, 0.5 + 0.03 * (i - 8)))
        elif name == 'outro' and i < 4:
            for ts, d, m in S.notes_in(bar, S.hook_bar(i if i < 3 else 7)):
                lead.append((ts, d * 0.92, m, 1.0 - 0.12 * i))
        elif name == 'build3' and i >= 4:
            for ts, d, m in S.notes_in(bar, S.hook_bar(i - 4)):
                lead.append((ts, d * 0.9, m, 0.45 + 0.1 * (i - 4)))
    lead.append((S.tb(108), S.BAR * 3, 76 + 2, 0.9))
    ll, lr = mono_line(lead, 'lead')
    B['lead'].add(0, (ll, lr), gain=0.42)
    hl, hr = mono_line(lead_hi, 'lead', vib=0.08)
    B['lead'].add(0, (hr, hl), gain=0.16)
    cl, cr = mono_line(chop, 'chop', glide=0.02, vib=0.05)
    B['lead'].add(0, (cl, cr), gain=0.2)
    return B


def bassnote(m, dur, v):
    n = int((dur + 0.05) * SR)
    t = tarr(n)
    f = np.full(n, float(mtof(m)))
    x = saw(f, 0.0) * 0.7 + saw(f * 1.004, 0.5) * 0.5
    x = svf(x, 180 + 900 * np.exp(-t / 0.07), 0.35, 0)
    env = np.minimum(t / 0.003, 1) * np.where(t < dur, 1.0, np.exp(-(t - dur) / 0.012))
    return np.tanh(x * 1.5) * env * v * 0.55


def subnote(m, dur, v):
    n = int((dur + 0.05) * SR)
    t = tarr(n)
    x = sine(np.full(n, float(mtof(m))), 0.0)
    env = np.minimum(t / 0.004, 1) * np.where(t < dur, 1.0, np.exp(-(t - dur) / 0.015))
    return x * env * v * 0.6


# --------------------------------------------------------------------------------------------------
def reverb_ir(seconds, damp, seed):
    r = np.random.default_rng(seed)
    n = int(seconds * SR)
    t = tarr(n)
    out = []
    for ch in range(2):
        x = r.standard_normal(n) * np.exp(-t / (seconds / 6.9))
        x = svf(x, damp * np.exp(-t / seconds * 1.5) + 400, 0.0, 0)
        x[: int(0.012 * SR)] *= np.linspace(0, 1, int(0.012 * SR))
        out.append(x / np.sqrt(np.sum(x ** 2)))
    return out


def conv(l, r, ir):
    return fftconvolve(l, ir[0])[:N], fftconvolve(r, ir[1])[:N]


def mix(B):
    sc_hard = sidechain(0.85)
    sc_mid = sidechain(0.55)
    sc_soft = sidechain(0.3)
    # section automation of the chord / pad / arp low-pass
    T = S.tb
    chord_cut = curve([(0, 300), (T(12), 500), (T(16) - 0.01, 9000), (T(16), 11000), (T(32), 11000),
                       (T(48), 600), (T(55, 3), 8000), (T(56), 12000), (T(72), 12000), (T(80), 500),
                       (T(87, 3), 8000), (T(88), 13000), (T(104), 13000), (T(108), 6000), (T(112) + 3, 300)])
    pad_cut = curve([(0, 450), (T(8), 2400), (T(16), 2600), (T(72), 2600), (T(73), 1600), (T(80), 2600),
                     (T(108), 3000), (T(112) + 3, 300)])
    arp_cut = curve([(0, 900), (T(8), 3000), (T(16), 7000), (T(112), 7000)])
    for k in ('chords',):
        B[k].l = svf(B[k].l, chord_cut, 0.15, 0)
        B[k].r = svf(B[k].r, chord_cut, 0.15, 0)
    B['pad'].l = svf(B['pad'].l, pad_cut, 0.1, 0)
    B['pad'].r = svf(B['pad'].r, pad_cut, 0.1, 0)
    B['arp'].l = svf(B['arp'].l, arp_cut, 0.1, 0)
    B['arp'].r = svf(B['arp'].r, arp_cut, 0.1, 0)

    B['chords'].apply(sc_hard)
    B['pad'].apply(sc_hard)
    B['bass'].apply(sc_hard)
    B['arp'].apply(sc_mid)
    B['keys'].apply(sc_soft)
    B['lead'].apply(sc_soft)
    g = gaps()
    for k in ('chords', 'pad', 'bass', 'arp', 'keys', 'lead'):
        B[k].apply(g)

    gains = {'kick': 0.95, 'drums': 1.0, 'bass': 0.68, 'chords': 0.3, 'pad': 0.4, 'arp': 0.28, 'lead': 1.4,
             'keys': 0.42, 'fx': 0.45}
    L = np.zeros(N)
    R = np.zeros(N)
    for k, b in B.items():
        L += b.l * gains[k]
        R += b.r * gains[k]

    # sends
    hall = reverb_ir(2.8, 5000, 7)
    room = reverb_ir(0.7, 7000, 9)
    send_hall_l = B['lead'].l * 0.35 + B['keys'].l * 0.35 + B['pad'].l * 0.3 + B['arp'].l * 0.25 + B['fx'].l * 0.3
    send_hall_r = B['lead'].r * 0.35 + B['keys'].r * 0.35 + B['pad'].r * 0.3 + B['arp'].r * 0.25 + B['fx'].r * 0.3
    send_room_l = B['drums'].l * 0.4
    send_room_r = B['drums'].r * 0.4
    hl, hr = conv(send_hall_l, send_hall_r, hall)
    rl, rr = conv(send_room_l, send_room_r, room)
    dl, dr = pingpong(B['lead'].l * 0.5 + B['arp'].l * 0.3 + B['keys'].l * 0.3,
                      B['lead'].r * 0.5 + B['arp'].r * 0.3 + B['keys'].r * 0.3, S.S16 * 3, 0.38, 3500.0)
    L += hl * 0.4 + rl * 0.3 + dl * 0.2
    R += hr * 0.4 + rr * 0.3 + dr * 0.2

    # stutters (glitch repeats of the first 16th of the beat)
    for t0, _ in S.events('stutter'):
        a = idx(t0)
        pattern = [2, 2, 1, 1, 0.5, 0.5, 0.5, 0.5, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25]  # in 16ths
        src_l, src_r = L[a:a + idx(S.S16 * 2)].copy(), R[a:a + idx(S.S16 * 2)].copy()
        p = a
        for k, s in enumerate(pattern):
            n = idx(S.S16 * s)
            f = np.ones(n)
            fd = min(64, n // 4)
            f[:fd] = np.linspace(0, 1, fd)
            f[-fd:] = np.linspace(1, 0, fd)
            gain = 1.0 - 0.02 * k
            L[p:p + n] = src_l[:n] * f * gain
            R[p:p + n] = src_r[:n] * f * gain
            p += n

    # master
    L = onepole_hp(L, 22.0)
    R = onepole_hp(R, 22.0)
    L, R = compress(L, R, -9.0, 2.0, 0.01, 0.15, 1.5)
    L, R = np.tanh(L * 0.9) / 0.9, np.tanh(R * 0.9) / 0.9
    return L, R


def master(L, R, target_lufs=-10.5):
    import pyloudnorm as pyln
    meter = pyln.Meter(SR)
    loud = meter.integrated_loudness(np.stack([L, R], 1))
    g = 10 ** ((target_lufs - loud) / 20)
    L, R = L * g, R * g
    L, R = limit(L, R, 0.89, 0.005, 0.08)
    fade = int(1.5 * SR)
    L[-fade:] *= np.linspace(1, 0, fade) ** 2
    R[-fade:] *= np.linspace(1, 0, fade) ** 2
    return L, R, loud


if __name__ == '__main__':
    import soundfile as sf
    B = build()
    L, R = mix(B)
    L, R, pre = master(L, R)
    import pyloudnorm as pyln
    after = pyln.Meter(SR).integrated_loudness(np.stack([L, R], 1))
    print(f'pre {pre:.1f} LUFS -> {after:.1f} LUFS, peak {max(abs(L).max(), abs(R).max()):.3f}, '
          f'dc {L.mean():.4f}')
    sf.write(sys.argv[1], np.stack([L, R], 1).astype(np.float32), SR, subtype='PCM_24')
