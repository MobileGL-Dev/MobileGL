"""Per-frame comparison of two Mali counter captures.

usage: python compare.py A.pftrace FPS_A B.pftrace FPS_B
Needs `pip install perfetto`.
"""
import sys

from perfetto.trace_processor import TraceProcessor


def per_second(path):
    tp = TraceProcessor(trace=path)
    rows = tp.query(
        "select t.name, sum(c.value) s, (max(c.ts) - min(c.ts)) / 1e9 dur "
        "from gpu_counter_track t join counter c on c.track_id = t.id group by t.id")
    out = {r.name: r.s / r.dur for r in rows if r.dur > 0}
    tp.close()
    return out


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    a, fps_a, b, fps_b = sys.argv[1], float(sys.argv[2]), sys.argv[3], float(sys.argv[4])
    A, B = per_second(a), per_second(b)
    print(f"{'counter':52s} {'A/frame':>14s} {'B/frame':>14s} {'B/A':>6s}")
    for name in sorted(A, key=lambda k: -A[k] / fps_a):
        pa, pb = A[name] / fps_a, B.get(name, 0.0) / fps_b
        ratio = f"{pb / pa:6.2f}" if pa else "   n/a"
        print(f"{name:52s} {pa:14.0f} {pb:14.0f} {ratio}")


if __name__ == "__main__":
    main()
