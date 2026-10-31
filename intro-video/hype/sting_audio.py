"""Sound for the glue-drop sting: a falling whoosh, a wet thump, splash plinks, tongues popping, a soft chime."""
import sys
import numpy as np
import soundfile as sf
from dsp import SR, sine, svf, saw, compress, limit
from music import reverb_ir, conv
import music as M
import sting

N = int(sting.END * SR)
rng = np.random.default_rng(7)
L = np.zeros(N)
R = np.zeros(N)


def put(t, x, pan=0.0, g=1.0):
    i0 = int(t * SR)
    n = min(len(x), N - i0)
    a = (pan + 1) * np.pi / 4
    L[i0:i0 + n] += x[:n] * np.cos(a) * 1.414 * g
    R[i0:i0 + n] += x[:n] * np.sin(a) * 1.414 * g


def tt(n):
    return np.arange(n) / SR


def bloop(f0, f1, dur, v):
    n = int(dur * SR)
    t = tt(n)
    f = f1 + (f0 - f1) * np.exp(-t / (dur * 0.25))
    return sine(f, 0.0) * np.minimum(t / 0.004, 1) * np.exp(-t / (dur * 0.35)) * v


hit = sting.T_HIT
# whoosh while it falls
n = int(0.5 * SR)
t = tt(n)
nz = svf(rng.standard_normal(n), 400 + 3000 * (t / 0.5) ** 2, 0.5, 1)
put(hit - 0.5, nz * (t / 0.5) ** 2 * 0.25)
# impact: low thump + wet splat
n = int(0.9 * SR)
t = tt(n)
thump = sine(45 + 120 * np.exp(-t / 0.05), 0.0) * np.exp(-t / 0.25)
splat = svf(rng.standard_normal(n), 2500 * np.exp(-t / 0.08) + 200, 0.3, 0) * np.exp(-t / 0.12)
put(hit, np.tanh(1.5 * thump) * 0.9 + splat * 0.7)
put(hit, bloop(520, 140, 0.35, 0.45))
# droplets landing around
for k in range(9):
    tk = hit + 0.25 + 0.06 * k + 0.05 * rng.random()
    put(tk, bloop(1400 + 600 * rng.random(), 700, 0.09, 0.14), pan=rng.uniform(-0.8, 0.8))
# tongues popping up, rising
notes = [392, 440, 523.25, 587.33, 659.25]
for k, fz in enumerate(notes):
    put(hit + 0.12 + 0.07 * k, bloop(fz * 1.5, fz, 0.2, 0.28), pan=-0.5 + 0.25 * k)
# settle chime: soft FM bells, C add9
for k, m in enumerate([60, 64, 67, 74, 79]):
    put(hit + 0.75 + 0.02 * k, M.bell(m, 1.4, 0.16), pan=-0.3 + 0.15 * k)
ir = reverb_ir(1.8, 6000, 3)
M.N = N
rl, rr = conv(L, R, ir)
L2, R2 = L + rl * 0.35, R + rr * 0.35
L2, R2 = limit(L2, R2, 0.89, 0.004, 0.05)
peak = max(abs(L2).max(), abs(R2).max())
g = 0.85 / peak
sf.write(sys.argv[1], np.stack([L2 * g, R2 * g], 1).astype(np.float32), SR, subtype='PCM_24')
print('ok', peak)
