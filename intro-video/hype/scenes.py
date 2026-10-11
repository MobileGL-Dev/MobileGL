"""Timeline: which scene draws which bars, plus the global beat-driven post and the stutter glitch."""
import math
import importlib
from anim import *
from engine import LW, LH

# (first bar, end bar, module, function)
TIMELINE = [
    (0, 8, 'sc_open', 'cold_open'),
    (8, 16, 'sc_open', 'your_app'),
    (16, 32, 'sc_drop1', 'drop1'),
    (32, 48, 'sc_how', 'how'),
    (48, 56, 'sc_pipe', 'pipe'),
    (56, 72, 'sc_modes', 'modes'),
    (72, 80, 'sc_break', 'breakdown'),
    (80, 88, 'sc_build3', 'checklist'),
    (88, 104, 'sc_final', 'final_drop'),
    (104, 200, 'sc_final', 'outro'),
]
_fns = {}


def _fn(mod, name):
    key = (mod, name)
    if key not in _fns:
        try:
            _fns[key] = getattr(importlib.import_module(mod), name)
        except (ImportError, AttributeError):
            _fns[key] = None
    return _fns[key]


STUTTER = [t for t, _ in S.events('stutter')]
PATTERN = [2, 2, 1, 1, 0.5, 0.5, 0.5, 0.5, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25]


def stutter_remap(t):
    """Same slicing as the music's stutter: inside the beat, show the first 8th again and again."""
    for ts in STUTTER:
        if ts <= t < ts + S.BEAT:
            u = t - ts
            p = 0.0
            for k, s in enumerate(PATTERN):
                d = s * S.S16
                if u < p + d:
                    return ts + (u - p), k
                p += d
    return t, -1


def draw(E, t):
    t_src, sk = stutter_remap(t)
    bar = t_src / S.BAR
    entry = None
    for e in TIMELINE:
        if e[0] <= bar < e[1]:
            entry = e
            break
    E.begin_sub(INK)
    P = {}
    if entry:
        fn = _fn(entry[2], entry[3])
        f = Frame(t_src, entry[0])
        if fn:
            P = fn(E, f) or {}
        else:
            E.rect(LW / 2, LH / 2, LW, LH, INK)
            E.text(f'{entry[2]}.{entry[3]}  bar {bar:.2f}', LW / 2, LH / 2, 60, BONE, font='mono')
    E.flush()
    if sk >= 0:   # glitch dressing on the repeats
        P = dict(P)
        P['ca'] = P.get('ca', 0) + 3.0 + sk * 0.3
        P['scan'] = 0.25
    return P
