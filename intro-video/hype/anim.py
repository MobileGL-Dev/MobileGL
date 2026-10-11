"""Easing, palette and per-frame timing helpers shared by the scenes."""
import math
from engine import hexc, mix, with_alpha, scale_rgb
import score as S

# --- palette (sampled from the MobileGlues icon, pushed for contrast) --------------------------------
INK = hexc('#06110a')      # near-black green: dark plates
INK2 = hexc('#0d2213')     # panels on dark
MOSS = hexc('#1d4a25')     # deep forest
LEAF = hexc('#4e9a3e')     # icon outline green
GLUE = hexc('#6edb55')     # icon highlight lime: the signal colour
NEON = hexc('#8cff62', 2.6)  # emissive lime (blooms)
MINT = hexc('#a0e28c')     # icon background: light plates
PALE = hexc('#d4f0c9')     # pale top face
BONE = hexc('#f1f7ec')     # type on dark
CORAL = hexc('#ff6a55')    # rare accent: errors / device lost
CORAL_HOT = hexc('#ff6a55', 2.2)
AMBER = hexc('#e7a94f')    # speckle tan, tiny accents
CLEAR = (0.0, 0.0, 0.0, 0.0)


def a(c, al):
    return with_alpha(c, al)


# --- easing ----------------------------------------------------------------------------------------
def clamp01(x):
    return 0.0 if x < 0 else 1.0 if x > 1 else x


def lin(t, t0, t1):
    return clamp01((t - t0) / (t1 - t0)) if t1 != t0 else float(t >= t0)


def out_expo(u):
    u = clamp01(u)
    return 1.0 if u >= 1 else 1 - 2 ** (-10 * u)


def in_expo(u):
    u = clamp01(u)
    return 0.0 if u <= 0 else 2 ** (10 * u - 10)


def out_cubic(u):
    u = clamp01(u)
    return 1 - (1 - u) ** 3


def in_cubic(u):
    u = clamp01(u)
    return u ** 3


def in_out_cubic(u):
    u = clamp01(u)
    return 4 * u ** 3 if u < 0.5 else 1 - (-2 * u + 2) ** 3 / 2


def out_back(u, k=1.7):
    u = clamp01(u)
    return 1 + (k + 1) * (u - 1) ** 3 + k * (u - 1) ** 2


def spring(u, freq=3.0, damp=6.0):
    """0 -> 1 with overshoot; u in seconds-ish units (unclamped above)."""
    if u <= 0:
        return 0.0
    return 1 - math.exp(-damp * u) * math.cos(2 * math.pi * freq * u)


def bump(u):
    """0 -> 1 -> 0 over u in [0, 1]."""
    u = clamp01(u)
    return math.sin(math.pi * u)


def lerp(a_, b, u):
    return a_ + (b - a_) * u


def rnd(*k):
    """Deterministic hash -> [0, 1)."""
    h = 2166136261
    for v in k:
        for ch in repr(v):
            h = ((h ^ ord(ch)) * 16777619) & 0xffffffff
    h ^= h >> 13
    h = (h * 1274126177) & 0xffffffff
    return (h & 0xffffff) / float(0x1000000)


# --- frame timing ----------------------------------------------------------------------------------
class Frame:
    def __init__(self, t, entry_start_bar=0):
        self.t = t
        self.bar = t / S.BAR
        self.beat = t / S.BEAT
        self.b0 = entry_start_bar
        self.lt = t - entry_start_bar * S.BAR          # local seconds in entry
        self.lbeat = self.lt / S.BEAT                  # local beats
        self.lbar = self.lt / S.BAR
        self.kick = S.pulse(t, 'kick', 0.11)
        self.clap = S.pulse(t, 'clap', 0.10)
        self.hat = S.pulse(t, 'hat', 0.05)
        self.crash = S.pulse(t, 'crash', 0.6)
        self.impact = S.pulse(t, 'impact', 0.35)
        self.roll = S.pulse(t, 'roll', 0.08)

    def at(self, beat):
        """Seconds since local beat `beat` (negative before)."""
        return self.lt - beat * S.BEAT

    def u(self, b0, b1):
        """Progress through local beats [b0, b1]."""
        return clamp01((self.lbeat - b0) / (b1 - b0))

    def step(self, div=1.0):
        """Index of the current local beat subdivision (div=2 -> 8ths)."""
        return int(math.floor(self.lbeat * div + 1e-6))


def frame_idx(t, fps=60):
    return int(math.floor(t * fps + 1e-6))
