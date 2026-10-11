"""Procedural upbeat ukulele/marimba track (120 BPM, C major, I-V-vi-IV) + paper SFX.
Usage: python music.py out/mobilegl_intro.json out/music.wav"""
import sys, json, functools
import numpy as np
from scipy import signal
from scipy.io import wavfile

SR = 44100
BPM = 120
BEAT = 60 / BPM
BAR = 4 * BEAT
rng_global = np.random.default_rng(42)


def hz(m):
    return 440.0 * 2 ** ((m - 69) / 12)


def N(s):
    names = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}
    return 12 * (int(s[-1]) + 1) + names[s[0]] + (1 if '#' in s else 0)


def env_ad(n, a=0.002, release=0.02):
    e = np.ones(n)
    na, nr = max(1, int(a * SR)), max(1, int(release * SR))
    e[:na] = np.linspace(0, 1, na)
    e[-nr:] *= np.linspace(1, 0, nr)
    return e


# ---------------------------------------------------------------- instruments
@functools.lru_cache(None)
def pluck(m, dur=1.4):
    f = hz(m)
    P = int(SR / f)
    total = int(dur * SR)
    r = np.random.default_rng(m)
    y = np.zeros(total + 1)
    burst = r.uniform(-1, 1, P)
    burst = np.convolve(burst, [0.3, 0.4, 0.3], 'same')
    burst -= burst.mean()
    y[1:P + 1] = burst
    k = P + 1
    while k < total + 1:
        e = min(k + P, total + 1)
        y[k:e] = 0.4985 * (y[k - P:e - P] + y[k - P - 1:e - P - 1])
        k = e
    y = y[1:]
    y *= env_ad(total, 0.001, 0.08)
    b, a = signal.butter(2, min(0.99, 5200 / (SR / 2)))
    return signal.lfilter(b, a, y) / (np.abs(y).max() + 1e-9)


@functools.lru_cache(None)
def mallet(m, dur=0.7):
    f = hz(m)
    t = np.arange(int(dur * SR)) / SR
    y = (np.sin(2 * np.pi * f * t) * np.exp(-t * 5.5)
         + 0.32 * np.sin(2 * np.pi * 3.93 * f * t) * np.exp(-t * 22)
         + 0.10 * np.sin(2 * np.pi * 9.2 * f * t) * np.exp(-t * 55))
    return y * env_ad(len(t), 0.0015, 0.03)


@functools.lru_cache(None)
def bass(m, dur):
    f = hz(m)
    t = np.arange(int(dur * SR)) / SR
    y = np.sin(2 * np.pi * f * t) + 0.35 * np.sin(2 * np.pi * 2 * f * t) + 0.15 * np.sin(2 * np.pi * 3 * f * t)
    e = (1 - np.exp(-t * 300)) * np.exp(-t * 3.0)
    return y * e * env_ad(len(t), 0.001, 0.03)


@functools.lru_cache(None)
def kick():
    t = np.arange(int(0.35 * SR)) / SR
    f = 48 + 110 * np.exp(-t * 32)
    ph = 2 * np.pi * np.cumsum(f) / SR
    y = np.sin(ph) * np.exp(-t * 11)
    y[:int(0.004 * SR)] += np.random.default_rng(1).uniform(-0.4, 0.4, int(0.004 * SR))
    return y


def _band(x, lo, hi, order=2):
    b, a = signal.butter(order, [lo / (SR / 2), min(0.99, hi / (SR / 2))], 'band')
    return signal.lfilter(b, a, x)


def _hp(x, fc):
    b, a = signal.butter(2, fc / (SR / 2), 'high')
    return signal.lfilter(b, a, x)


@functools.lru_cache(None)
def clap():
    n = int(0.25 * SR)
    t = np.arange(n) / SR
    r = np.random.default_rng(2)
    y = np.zeros(n)
    for k, d in enumerate([0, 0.009, 0.018, 0.027]):
        s = int(d * SR)
        dec = 120 if k < 3 else 22
        y[s:] += r.uniform(-1, 1, n - s) * np.exp(-(t[:n - s]) * dec)
    y = _band(y, 900, 3200)
    return y / np.abs(y).max()


@functools.lru_cache(None)
def hat(open_=False):
    n = int((0.18 if open_ else 0.06) * SR)
    t = np.arange(n) / SR
    y = _hp(np.random.default_rng(3).uniform(-1, 1, n), 7000) * np.exp(-t * (18 if open_ else 70))
    return y / np.abs(y).max()


@functools.lru_cache(None)
def shaker():
    n = int(0.09 * SR)
    t = np.arange(n) / SR
    e = np.minimum(1, t / 0.02) * np.exp(-t * 40)
    y = _hp(np.random.default_rng(4).uniform(-1, 1, n), 5000) * e
    return y / np.abs(y).max()


def swoosh(seed):
    n = int(0.75 * SR)
    t = np.arange(n) / SR
    r = np.random.default_rng(seed)
    x = r.uniform(-1, 1, n)
    e = np.sin(np.pi * np.clip(t / 0.75, 0, 1)) ** 2.2
    y = _band(x, 500, 5000) * e
    crinkle = np.zeros(n)
    for _ in range(26):
        s = int(r.uniform(0.05, 0.6) * SR)
        L = int(0.004 * SR)
        crinkle[s:s + L] += r.uniform(-1, 1, L) * r.uniform(0.3, 1)
    y += 0.5 * _hp(crinkle, 2500)
    return y / np.abs(y).max()


@functools.lru_cache(None)
def tok():
    t = np.arange(int(0.08 * SR)) / SR
    f = 700 * np.exp(-t * 12) + 260
    return np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t * 45)


# ---------------------------------------------------------------- score
CHORDS = [  # name, uke voicing (G C E A strings), bass root, chord tones for arps
    ('C', [N('G4'), N('C4'), N('E4'), N('C5')], N('C2'), [N('C5'), N('E5'), N('G5'), N('C6')]),
    ('G', [N('G4'), N('D4'), N('G4'), N('B4')], N('G1'), [N('B4'), N('D5'), N('G5'), N('B5')]),
    ('Am', [N('A4'), N('C4'), N('E4'), N('A4')], N('A1'), [N('C5'), N('E5'), N('A5'), N('C6')]),
    ('F', [N('A4'), N('C4'), N('F4'), N('A4')], N('F1'), [N('C5'), N('F5'), N('A5'), N('C6')]),
]
MEL_A = [
    [(0, 'E5', .5), (1, 'G5', .5), (2, 'A5', .5), (2.5, 'G5', .5), (3, 'E5', 1)],
    [(0, 'D5', .5), (1, 'G5', .5), (1.5, 'B5', .5), (2, 'D6', 1), (3, 'B5', .5), (3.5, 'G5', .5)],
    [(0, 'C6', .5), (0.5, 'B5', .5), (1, 'A5', 1), (2, 'E5', .5), (3, 'A5', .5), (3.5, 'C6', .5)],
    [(0, 'A5', 1.5), (1.5, 'G5', .5), (2, 'F5', .5), (2.5, 'E5', .5), (3, 'D5', 1)],
]
MEL_B = [
    [(0, 'G5', .5), (0.5, 'G5', .5), (1, 'C6', 1), (2, 'E6', .5), (2.5, 'D6', .5), (3, 'C6', 1)],
    [(0, 'B5', .5), (1, 'D6', .5), (1.5, 'B5', .5), (2, 'G5', 1), (3, 'A5', .5), (3.5, 'B5', .5)],
    [(0, 'C6', 1), (1, 'E6', .5), (1.5, 'D6', .5), (2, 'C6', .5), (2.5, 'A5', .5), (3, 'E5', 1)],
    [(0, 'F5', .5), (0.5, 'A5', .5), (1, 'C6', 1), (2, 'A5', .5), (2.5, 'G5', .5), (3, 'G5', 1)],
]
STRUM = [(0, 'D'), (1, 'D'), (1.5, 'U'), (2.5, 'U'), (3, 'D'), (3.5, 'U')]
BASS = [(0, 0, .45), (1, 0, .2), (1.5, 12, .2), (2, 0, .45), (3, 7, .3), (3.5, 12, .2)]


class Mix:
    def __init__(self, seconds):
        self.n = int(seconds * SR) + SR * 3
        self.buses = {k: np.zeros((self.n, 2)) for k in ('dry', 'wet')}

    def add(self, x, t, gain=1.0, pan=0.0, bus='dry'):
        s = int(t * SR)
        if s >= self.n:
            return
        x = x[:self.n - s]
        l, r = np.cos((pan + 1) * np.pi / 4), np.sin((pan + 1) * np.pi / 4)
        self.buses[bus][s:s + len(x), 0] += x * gain * l * 1.414
        self.buses[bus][s:s + len(x), 1] += x * gain * r * 1.414


def section(bar, last):
    if bar >= last:
        return 'end'
    if bar < 2:
        return 'intro0'
    if bar < 8:
        return 'intro'
    if 64 <= bar < 68 or 112 <= bar < 116:
        return 'break'
    return 'mel' if ((bar - 8) // 16) % 2 == 0 else 'arp'


def build(meta, out):
    total = meta['total']
    mix = Mix(total)
    last = int((total - 3.6) // BAR)            # final hit bar
    r = np.random.default_rng(9)
    for bar in range(last + 1):
        t0 = bar * BAR
        sec = section(bar, last)
        name, uke, root, tones = CHORDS[bar % 4]
        if sec == 'end':
            for k, m in enumerate(uke):
                mix.add(pluck(m, 3.0), t0 + 0.012 * k, 0.42, -0.25, 'wet')
            mix.add(mallet(N('C6'), 2.0), t0, 0.45, 0.2, 'wet')
            mix.add(mallet(N('G5'), 2.0), t0 + 0.06, 0.3, 0.2, 'wet')
            mix.add(bass(N('C2'), 2.0), t0, 0.55)
            mix.add(kick(), t0, 0.7)
            mix.add(hat(True), t0, 0.25, 0.3)
            break
        drums = sec in ('intro', 'mel', 'arp')
        # ukulele strums
        for b, d in STRUM:
            order = uke if d == 'D' else uke[::-1]
            vel = (0.36 if d == 'D' else 0.24) * (0.8 if sec == 'break' else 1)
            for k, m in enumerate(order):
                mix.add(pluck(m), t0 + b * BEAT + 0.011 * k + r.uniform(0, 0.004), vel * r.uniform(0.85, 1.05),
                        -0.25, 'wet')
        # bass
        if sec != 'intro0':
            for b, off, d in BASS:
                g = 0.42 if sec != 'break' else 0.3
                mix.add(bass(root + off, round(d * BEAT * 2, 3)), t0 + b * BEAT, g)
        # drums
        if drums:
            for b in (0, 2):
                mix.add(kick(), t0 + b * BEAT, 0.62)
            if sec != 'intro' or bar >= 4:
                for b in (1, 3):
                    mix.add(clap(), t0 + b * BEAT, 0.26, 0.05, 'wet')
            for e in range(8):
                mix.add(hat(open_=(e == 7 and bar % 4 == 3)), t0 + e * BEAT / 2,
                        0.045 if e % 2 == 0 else 0.075, 0.3)
            if sec in ('mel', 'arp'):
                for s in range(16):
                    mix.add(shaker(), t0 + s * BEAT / 4, 0.045 if s % 2 else 0.025, -0.35)
                if bar % 8 == 7:   # little fill
                    mix.add(kick(), t0 + 3.5 * BEAT, 0.45)
                    mix.add(clap(), t0 + 3.75 * BEAT, 0.16, 0.05, 'wet')
        if sec == 'break':
            for s in range(8):
                mix.add(shaker(), t0 + s * BEAT / 2, 0.05, -0.35)
        # melody / arps
        if sec == 'mel' or sec == 'break' and bar % 4 >= 2:
            phrase = MEL_A if ((bar - 8) // 4) % 2 == 0 else MEL_B
            for b, note, d in phrase[bar % 4]:
                g = 0.3 if sec == 'mel' else 0.2
                mix.add(mallet(N(note), max(0.4, d * BEAT * 1.6)), t0 + b * BEAT, g, 0.2, 'wet')
                if sec == 'mel' and ((bar - 8) // 16) >= 2:      # later: octave sparkle doubling
                    mix.add(mallet(N(note) + 12, 0.4), t0 + b * BEAT, 0.08, 0.35, 'wet')
        if sec == 'arp':
            pat = [0, 1, 2, 3, 2, 1, 2, 3, 0, 1, 2, 3, 2, 3, 1, 2]
            for s, i in enumerate(pat):
                mix.add(mallet(tones[i], 0.35), t0 + s * BEAT / 4, 0.13 if s % 4 else 0.18, 0.35, 'wet')
            if bar % 2 == 0:   # simple call-back motif
                for b, note in [(0, tones[3] + 0), (0.5, tones[2]), (1, tones[3])]:
                    mix.add(mallet(note + 12 if note < N('C6') else note, 0.5), t0 + b * BEAT, 0.12, -0.1, 'wet')
    # SFX
    for k, w in enumerate(meta['wipes']):
        mix.add(swoosh(100 + k), w - 0.05, 0.2, 0.4 - 0.8 * (k % 2))
    for k, p in enumerate(meta['pops']):
        mix.add(tok(), p + 0.35, 0.14, -0.6 + 0.15 * (k % 8))
    # reverb on the wet bus
    ir_n = int(1.3 * SR)
    t = np.arange(ir_n) / SR
    ir = np.random.default_rng(7).standard_normal((ir_n, 2)) * np.exp(-t * 4.2)[:, None]
    ir[:, 0] = _band(ir[:, 0], 200, 6000)
    ir[:, 1] = _band(ir[:, 1], 200, 6000)
    ir /= np.sqrt((ir ** 2).sum(axis=0))
    wet = mix.buses['wet']
    rev = np.stack([signal.fftconvolve(wet[:, c], ir[:, c])[:mix.n] for c in range(2)], axis=1)
    y = mix.buses['dry'] + wet + 0.22 * rev
    y = y[:int(total * SR)]
    bh, ah = signal.butter(2, 30 / (SR / 2), 'high')
    y = signal.filtfilt(bh, ah, y, axis=0)
    y /= np.abs(y).max() / 0.95
    y = np.tanh(1.4 * y) / np.tanh(1.4)
    n_in, n_out = int(0.25 * SR), int(1.6 * SR)
    y[:n_in] *= np.linspace(0, 1, n_in)[:, None]
    y[-n_out:] *= np.linspace(1, 0, n_out)[:, None] ** 1.5
    y *= 0.89 / np.abs(y).max()
    wavfile.write(out, SR, (y * 32767).astype(np.int16))
    print('wrote', out, len(y) / SR, 's; final hit bar', last, 'at', last * BAR)


if __name__ == '__main__':
    build(json.load(open(sys.argv[1])), sys.argv[2])
