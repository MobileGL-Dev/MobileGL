"""Small numba DSP kit: band-limited oscillators, a TPT state-variable filter, a ping-pong delay, a
compressor and a look-ahead limiter. Everything runs on float64 mono arrays."""
import math
import numpy as np
from numba import njit

SR = 48000


@njit(cache=True)
def _blep(t, dt):
    if t < dt:
        t /= dt
        return t + t - t * t - 1.0
    if t > 1.0 - dt:
        t = (t - 1.0) / dt
        return t * t + t + t + 1.0
    return 0.0


@njit(cache=True)
def saw(freq, phase0):
    n = freq.shape[0]
    out = np.empty(n)
    p = phase0
    for i in range(n):
        dt = freq[i] / SR
        out[i] = 2.0 * p - 1.0 - _blep(p, dt)
        p += dt
        if p >= 1.0:
            p -= 1.0
    return out


@njit(cache=True)
def pulse(freq, width, phase0):
    n = freq.shape[0]
    out = np.empty(n)
    p = phase0
    for i in range(n):
        dt = freq[i] / SR
        v = 1.0 if p < width else -1.0
        v += _blep(p, dt)
        q = p - width
        if q < 0.0:
            q += 1.0
        v -= _blep(q, dt)
        out[i] = v
        p += dt
        if p >= 1.0:
            p -= 1.0
    return out


@njit(cache=True)
def sine(freq, phase0):
    n = freq.shape[0]
    out = np.empty(n)
    p = phase0
    for i in range(n):
        out[i] = math.sin(2.0 * math.pi * p)
        p += freq[i] / SR
        if p >= 1.0:
            p -= 1.0
    return out


@njit(cache=True)
def svf(x, cutoff, res, mode):
    """Zavalishin TPT SVF. cutoff: per-sample Hz array; res 0..1; mode 0=lp 1=bp 2=hp."""
    n = x.shape[0]
    out = np.empty(n)
    ic1 = 0.0
    ic2 = 0.0
    k = 2.0 - 2.0 * min(res, 0.98)
    for i in range(n):
        fc = min(max(cutoff[i], 10.0), SR * 0.45)
        g = math.tan(math.pi * fc / SR)
        a1 = 1.0 / (1.0 + g * (g + k))
        a2 = g * a1
        a3 = g * a2
        v3 = x[i] - ic2
        v1 = a1 * ic1 + a2 * v3
        v2 = ic2 + a2 * ic1 + a3 * v3
        ic1 = 2.0 * v1 - ic1
        ic2 = 2.0 * v2 - ic2
        if mode == 0:
            out[i] = v2
        elif mode == 1:
            out[i] = v1
        else:
            out[i] = x[i] - k * v1 - v2
    return out


@njit(cache=True)
def onepole_hp(x, fc):
    n = x.shape[0]
    out = np.empty(n)
    a = math.exp(-2.0 * math.pi * fc / SR)
    yp = 0.0
    xp = 0.0
    for i in range(n):
        yp = a * (yp + x[i] - xp)
        xp = x[i]
        out[i] = yp
    return out


@njit(cache=True)
def pingpong(l, r, delay_s, fb, damp_hz):
    n = l.shape[0]
    d = int(delay_s * SR)
    bl = np.zeros(d)
    br = np.zeros(d)
    ol = np.empty(n)
    orr = np.empty(n)
    a = math.exp(-2.0 * math.pi * damp_hz / SR)
    zl = 0.0
    zr = 0.0
    j = 0
    for i in range(n):
        yl = bl[j]
        yr = br[j]
        ol[i] = yl
        orr[i] = yr
        zl = (1 - a) * yl + a * zl
        zr = (1 - a) * yr + a * zr
        bl[j] = 0.5 * (l[i] + r[i]) + fb * zr     # mono in on the left tap, cross-feed
        br[j] = fb * zl
        j += 1
        if j >= d:
            j = 0
    return ol, orr


@njit(cache=True)
def compress(l, r, thresh_db, ratio, attack, release, makeup_db):
    n = l.shape[0]
    ol = np.empty(n)
    orr = np.empty(n)
    aa = math.exp(-1.0 / (attack * SR))
    ar = math.exp(-1.0 / (release * SR))
    env = 0.0
    mk = 10 ** (makeup_db / 20)
    for i in range(n):
        x = max(abs(l[i]), abs(r[i]))
        if x > env:
            env = aa * env + (1 - aa) * x
        else:
            env = ar * env + (1 - ar) * x
        db = 20 * math.log10(env + 1e-9)
        over = db - thresh_db
        gr = 0.0
        if over > 0:
            gr = over * (1 - 1 / ratio)
        g = 10 ** (-gr / 20) * mk
        ol[i] = l[i] * g
        orr[i] = r[i] * g
    return ol, orr


@njit(cache=True)
def limit(l, r, ceiling, lookahead_s, release_s):
    """Look-ahead peak limiter: gain = min over the window ahead, smoothed on release."""
    n = l.shape[0]
    la = int(lookahead_s * SR)
    need = np.ones(n)
    for i in range(n):
        p = max(abs(l[i]), abs(r[i]))
        if p > ceiling:
            need[i] = ceiling / p
    # running minimum over the next `la` samples (simple deque-free two-pass block min)
    gmin = np.ones(n)
    for i in range(n - 1, -1, -1):
        m = need[i]
        if i + 1 < n:
            # decay toward the earlier future minimum with a linear attack ramp over `la`
            nxt = gmin[i + 1]
            ramp = nxt + (1.0 - nxt) / la
            if ramp < m:
                m = ramp
        gmin[i] = m
    ar = math.exp(-1.0 / (release_s * SR))
    g = 1.0
    ol = np.empty(n)
    orr = np.empty(n)
    for i in range(n):
        t = gmin[i]
        if t < g:
            g = t
        else:
            g = ar * g + (1 - ar) * t
        ol[i] = l[i] * g
        orr[i] = r[i] * g
    return ol, orr


def mtof(m):
    return 440.0 * 2.0 ** ((np.asarray(m, dtype=np.float64) - 69.0) / 12.0)


def adsr(n, a, d, s, r_start, r, sr=SR):
    """Envelope of n samples; release begins at sample r_start and lasts r seconds."""
    t = np.arange(n) / sr
    e = np.where(t < a, t / max(a, 1e-6), s + (1 - s) * np.exp(-(t - a) / max(d, 1e-6)))
    rs = r_start / sr
    if r_start < n:
        lvl = e[min(r_start, n - 1)]
        tail = t >= rs
        e[tail] = lvl * np.exp(-(t[tail] - rs) / max(r / 4.0, 1e-6))
    return e
