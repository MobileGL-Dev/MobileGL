#!/usr/bin/env python3
"""freqcheck.py <freq.txt> [pin.txt] - P15 measurement-validity verdict for one run.

freq.txt is freq_sampler.sh output; pin.txt is the `pin_clocks.sh show` output taken when the
session pinned (bench_session.sh writes it). A run is VALID only if every sample shows each CPU
policy at its pinned frequency, with its scaling_max still at the pin (no cap), and - whenever the
GPU is busy - the GPU at its pinned frequency, with no kgsl thermal power level or throttling.
One OPP step of tolerance is allowed: the next available frequency below the pin.

Prints one line: VALID|INVALID, per-policy min/max cur freq, GPU min/max (busy samples), peak
temperatures, and the reasons when invalid. Exit code 0 = valid, 1 = invalid, 2 = no samples.

CLOCKS ARE JUDGED OVER THE MEASUREMENT WINDOWS, THERMAL SIGNALS OVER THE WHOLE RUN (P15). When a
measure_raw.txt sits beside freq.txt (one window per line: <frame0> <frame1> <t_start> <t_end>, in
the sampler's uptime clock), the clock checks (CPU cur / max, GPU) only read the samples taken inside
a window, because the rule is "clocks pinned while benchmarking". The sampler starts during warmup,
and its first sample can catch an idle GPU parked at its lowest OPP. kgsl thermal_pwrlevel and
throttling are still checked over every sample. The line says which span each check covered.
"""
import re
import sys


def parse(path):
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        kv = dict(re.findall(r"(\w+)=([^\s]+)", line))
        if "t" in kv:
            rows.append(kv)
    return rows


def pins(path):
    """The INTENDED pin, from pin_clocks.sh's targets (env PIN_CPU_KHZ / PIN_LITTLE_KHZ / PIN_GPU_HZ,
    same defaults) mapped onto each policy's available frequencies - never the caps read back from
    the device, which a frequency daemon may already have lowered."""
    import os
    big = int(os.environ.get("PIN_CPU_KHZ", "2035200"))
    little = int(os.environ.get("PIN_LITTLE_KHZ", "1574400"))
    gpu = int(os.environ.get("PIN_GPU_HZ", "680000000"))
    # The session's profile (pin_clocks.sh writes it on the device; probes save it as profile.txt).
    prof = os.path.join(os.path.dirname(path), "profile.txt") if path else None
    if prof and os.path.exists(prof):
        kv = dict(re.findall(r"(\w+)=(\S+)", open(prof, encoding="utf-8", errors="replace").read()))
        big = int(kv.get("cpu", big)); little = int(kv.get("little", little)); gpu = int(kv.get("gpu", gpu))
    cpu, steps = {}, {}
    if not path:
        return cpu, gpu, steps
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"gpu cur=\d+ min=\d+ max=\d+ avail=([\d ]+)", line)
        if m:
            # kgsl power levels index this table from the top (level 0 = fastest).
            steps["gpu"] = sorted((int(x) for x in m.group(1).split()), reverse=True)
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"policy(\d+) cur=\d+ min=(\d+) max=(\d+) avail=([\d ]+)", line)
        if m:
            avail = sorted(int(x) for x in m.group(4).split())
            want = little if m.group(1) == "0" else big
            ok = [a for a in avail if a <= want]
            cpu[m.group(1)] = ok[-1] if ok else avail[0]
            steps[m.group(1)] = avail
    return cpu, gpu, steps


def windows(freq_path):
    import os
    path = os.path.join(os.path.dirname(os.path.abspath(freq_path)), "measure_raw.txt")
    spans = []
    if os.path.exists(path):
        for line in open(path, encoding="utf-8", errors="replace"):
            parts = line.split()
            if len(parts) >= 4:
                try:
                    spans.append((float(parts[2]), float(parts[3])))
                except ValueError:
                    pass
    return spans


def main():
    all_rows = parse(sys.argv[1])
    cpu_pin, gpu_pin, steps = pins(sys.argv[2] if len(sys.argv) > 2 else None)
    if not all_rows:
        print("NO-SAMPLES")
        sys.exit(2)
    spans = windows(sys.argv[1])
    in_window = [r for r in all_rows if any(a <= float(r["t"]) <= b for a, b in spans)]
    rows = in_window if in_window else all_rows
    reasons = []
    out = [f"clocks over {'window' if in_window else 'all'} samples n={len(rows)}/{len(all_rows)}"]
    pols = sorted({k[1:] for k in rows[0] if re.fullmatch(r"c\d+", k)}, key=int)
    for p in pols:
        cur = [int(r["c" + p].split("/")[0]) for r in rows]
        mx = [int(r["c" + p].split("/")[1]) for r in rows]
        out.append(f"c{p}={min(cur) // 1000}-{max(cur) // 1000}MHz")
        pin = cpu_pin.get(p)
        if pin:
            lower = [s for s in steps.get(p, []) if s < pin]
            floor = lower[-1] if lower else pin
            if min(cur) < floor:
                reasons.append(f"c{p} cur {min(cur) // 1000} < pin {pin // 1000}")
            if min(mx) < pin:
                reasons.append(f"c{p} max capped to {min(mx) // 1000}")
    busy = [r for r in rows if int(r.get("busy", "0") or 0) > 0]
    gpus = [int(r["gpu"]) for r in (busy or rows)]
    out.append(f"gpu={min(gpus) // 1000000}-{max(gpus) // 1000000}MHz(busy samples {len(busy)}/{len(rows)})")
    if gpu_pin and busy and min(gpus) < gpu_pin * 0.85:
        reasons.append(f"gpu {min(gpus) // 1000000} < pin {gpu_pin // 1000000}")
    # Thermal signals over EVERY sample, windows or not.
    tpl = max(int(r.get("tpl", 0)) for r in all_rows)
    thr = max(int(r.get("thr", 0)) for r in all_rows)
    if thr:
        reasons.append(f"kgsl throttling {thr}")
    # A devfreq max_freq write (the pin itself) shows up as kgsl's thermal_pwrlevel; only a level
    # whose frequency is below the pin is a cap.
    table = steps.get("gpu", [])
    if tpl and table and tpl < len(table):
        if gpu_pin and table[tpl] < gpu_pin:
            reasons.append(f"gpu thermal_pwrlevel {tpl} = {table[tpl] // 1000000} MHz < pin")
    elif tpl and not table:
        reasons.append(f"thermal_pwrlevel {tpl} (no OPP table to map it)")
    out.append(f"tpl={tpl} thr={thr}")
    peak = lambda k: max(int(r.get(k, 0)) for r in all_rows) / 1000.0
    out.append(f"peak cpu {peak('cpuT'):.1f}C gpu {peak('gpuT'):.1f}C skin {peak('skinT'):.1f}C n={len(all_rows)}")
    verdict = "VALID" if not reasons else "INVALID"
    print(verdict, " ".join(out), ("| " + "; ".join(reasons)) if reasons else "")
    sys.exit(0 if not reasons else 1)


if __name__ == "__main__":
    main()
