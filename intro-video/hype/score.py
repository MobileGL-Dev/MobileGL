"""The arrangement both the music and the picture are cut to: tempo grid, sections, harmony, melodies
and every drum / fx event. The video reads the same events for its beat pulses, so they never drift."""
import math

BPM = 128.0
BEAT = 60.0 / BPM            # 0.46875 s
BAR = 4 * BEAT               # 1.875 s
S16 = BEAT / 4
NBARS = 112
END = NBARS * BAR + 3.5      # music + reverb tail (213.5 s)

SECTIONS = [  # name, first bar, end bar
    ('intro', 0, 8), ('build1', 8, 16), ('drop1', 16, 32), ('verse', 32, 48), ('build2', 48, 56),
    ('drop2', 56, 72), ('break', 72, 80), ('build3', 80, 88), ('drop3', 88, 104), ('outro', 104, 112),
]


def tb(bar, beat=0.0, s16=0.0):
    """Time of bar + beat + sixteenth, in seconds."""
    return bar * BAR + beat * BEAT + s16 * S16


def section(bar):
    for name, a, b in SECTIONS:
        if a <= bar < b:
            return name, a, b
    return 'end', NBARS, NBARS + 4


def key_shift(bar):
    return 2 if bar >= 88 else 0          # truck-driver lift into the last drop


# --- harmony ---------------------------------------------------------------------------------------
A, F, C, G, Em = 'Am', 'F', 'C', 'G', 'Em'
CHORDS = {  # close voicings around middle C (midi), root for the bass
    'Am': ((57, 60, 64), 45), 'F': ((57, 60, 65), 41), 'C': ((55, 60, 64), 48),
    'G': ((55, 59, 62), 43), 'Em': ((55, 59, 64), 40), 'Dm': ((57, 62, 65), 38),
}
MAIN = [A, F, C, G]
VERSE = [F, G, Em, A]


def chord_name(bar):
    name, a, _ = section(bar)
    i = bar - a
    if name == 'verse':
        return VERSE[i % 4]
    if name == 'break':
        return [F, F, G, G, A, A, F, G][i]
    if name == 'outro':
        return [A, F, C, G, C, C, C, C][i]
    if name == 'end':
        return C
    return MAIN[i % 4]


def chord(bar):
    """(voicing, bass root) for the bar, transposed by the key lift."""
    v, r = CHORDS[chord_name(bar)]
    k = key_shift(bar)
    return tuple(n + k for n in v), r + k


# --- melodies (16th-step grid: (step, length, midi)) ------------------------------------------------
HOOK = [
    [(0, 2, 76), (3, 1, 76), (4, 2, 74), (6, 2, 76), (8, 3, 81), (11, 2, 79), (14, 2, 76)],      # Am
    [(0, 2, 77), (3, 1, 76), (4, 2, 74), (6, 4, 72), (10, 2, 74), (12, 4, 76)],                  # F
    [(0, 2, 79), (3, 1, 79), (4, 2, 77), (6, 2, 76), (8, 3, 76), (11, 2, 72), (14, 2, 74)],      # C
    [(0, 6, 74), (6, 2, 71), (8, 4, 74), (12, 2, 76), (14, 2, 79)],                              # G
]
HOOK_END = [(0, 3, 74), (3, 3, 71), (6, 2, 74), (8, 8, 69)]                                       # G, resolving


def hook_bar(i):
    """Bar i (0..7) of the 8-bar lead phrase."""
    return HOOK_END if i % 8 == 7 else HOOK[i % 4]


def notes_in(bar, pattern, shift=0):
    k = key_shift(bar)
    return [(tb(bar, 0, s), l * S16, m + k + shift) for s, l, m in pattern]


# --- events ----------------------------------------------------------------------------------------
def _events():
    ev = []  # (time, kind, vel)

    def add(t, kind, vel=1.0):
        ev.append((t, kind, vel))

    for bar in range(NBARS):
        name, a, b = section(bar)
        i = bar - a
        last = bar == b - 1
        if name == 'intro':
            if i >= 4:
                for s in range(2, 16, 4):
                    add(tb(bar, 0, s), 'hat', 0.5 + 0.1 * (i - 4))
            if i == 0:
                add(tb(bar), 'impact', 0.5)
        elif name == 'build1':
            for q in range(4):
                if not (last and q == 3):
                    add(tb(bar, q), 'kick', 0.7 if i < 6 else 0.78)
                add(tb(bar, q, 2), 'hat', 0.7)
            for q in (1, 3):
                if not (last and q == 3):
                    add(tb(bar, q), 'clap', 0.65)
        elif name in ('drop1', 'drop2', 'drop3'):
            hot = {'drop1': 1.0, 'drop2': 1.0, 'drop3': 1.0}[name]
            fill = last or i == 7
            for q in range(4):
                add(tb(bar, q), 'kick', hot)
                add(tb(bar, q, 2), 'ohat', 0.8)
            for q in (1, 3):
                add(tb(bar, q), 'clap', 1.0)
            for s in range(16):
                if s % 4 != 2:
                    add(tb(bar, 0, s), 'hat', 0.45 + 0.25 * (s % 2 == 0))
            if i % 8 == 0:
                add(tb(bar), 'crash', 1.0)
            if i == 14:      # climb: 8th-note snares, then 16ths in the last bar
                for s in range(0, 16, 2):
                    add(tb(bar, 0, s), 'snare', 0.35 + 0.02 * s)
            elif last:
                for s in range(12):
                    add(tb(bar, 0, s), 'snare', 0.55 + 0.035 * s)
            elif fill:
                for s in (12, 13, 14, 15):
                    add(tb(bar, 0, s), 'snare', 0.6 + 0.1 * (s - 12))
                if last:
                    add(tb(bar, 3), 'stutter', 1.0)
        elif name == 'verse':
            for q in range(4):
                add(tb(bar, q), 'kick', 0.8)
            for q in (1, 3):
                add(tb(bar, q), 'clap', 0.75)
            for s in range(16):
                add(tb(bar, 0, s), 'shaker', 0.5 + 0.3 * (s % 4 == 2))
            if i % 4 == 3:
                add(tb(bar, 3, 2), 'snare', 0.5)
                add(tb(bar, 3, 3), 'snare', 0.6)
            if i == 8:
                add(tb(bar), 'crash', 0.7)
        elif name in ('build2', 'build3'):
            n = 8
            for q in range(4):
                if i < 6:
                    add(tb(bar, q), 'kick', 0.7)
            # snare roll: quarters -> 8ths -> 16ths -> 32nds, rising
            div = 1 if i < 2 else 2 if i < 4 else 4 if i < 6 else 8
            steps = 4 * div
            for s in range(steps):
                if last and s >= steps * 3 // 4:
                    break                         # one beat of silence before the drop
                prog = (i + s / steps) / n
                add(tb(bar, s * 4 / steps), 'roll', 0.35 + 0.65 * prog)
            if i == 0:
                add(tb(bar), 'crash', 0.8)
        elif name == 'break':
            if i == 0:
                add(tb(bar), 'impact', 0.8)
            if i >= 4:
                for q in (0, 2):
                    add(tb(bar, q), 'heart', 0.5 + 0.1 * (i - 4))
        elif name == 'outro':
            if i < 4:
                for q in range(4):
                    add(tb(bar, q), 'kick', 1.0 - 0.15 * i)
                    add(tb(bar, q, 2), 'ohat', 0.7 - 0.1 * i)
                for q in (1, 3):
                    add(tb(bar, q), 'clap', 0.9 - 0.15 * i)
            if i == 0:
                add(tb(bar), 'crash', 1.0)
            if i == 4:
                add(tb(bar), 'impact', 1.0)
                add(tb(bar), 'crash', 1.0)
                add(tb(bar), 'kick', 1.0)
    for b in (16, 56, 88):                        # drops: impact + reverse swell into them
        add(tb(b), 'impact', 1.0)
        add(tb(b), 'drop', 1.0)
    for a, b in ((4, 8), (12, 16), (48, 56), (80, 88), (30, 32), (70, 72), (102, 104)):
        add(tb(a), 'riser', tb(b) - tb(a))
    return sorted(ev)


EVENTS = _events()


def events(kind):
    return [(t, v) for t, k, v in EVENTS if k == kind]


def pulse(t, kind, decay=0.12, kinds_cache={}):
    """Exponentially decaying pulse from the most recent event of kind (0..1 * vel)."""
    ts = kinds_cache.get(kind)
    if ts is None:
        ts = kinds_cache[kind] = events(kind)
    best = 0.0
    lo, hi = 0, len(ts)
    while lo < hi:                       # last event <= t
        mid = (lo + hi) // 2
        if ts[mid][0] <= t:
            lo = mid + 1
        else:
            hi = mid
    if lo:
        te, v = ts[lo - 1]
        best = v * math.exp(-(t - te) / decay)
    return best


if __name__ == '__main__':
    print(f'{NBARS} bars, {NBARS * BAR:.2f}s + tail -> {END:.2f}s, {len(EVENTS)} events')
    for name, a, b in SECTIONS:
        print(f'{name:8s} bars {a:3d}-{b:3d}  {tb(a):7.2f}s - {tb(b):7.2f}s')
