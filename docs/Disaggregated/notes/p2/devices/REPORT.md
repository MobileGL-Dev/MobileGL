# P2 frequency-pin profiles: what was read, what was written, what was proven

Date: 2026-09-07. Devices: `35d0befa` (Xiaomi 24129PN74C, Snapdragon 8 Elite / SM8750, Adreno
830v2) and `3B159D009VZ00000` (Oppo PLG110 / ColorOS, MediaTek MT6993 "Dimensity 9500", Mali).
Both rooted via Magisk (`su` returns uid 0 in context `u:r:magisk:s0`). Host: Windows, Git Bash,
`MSYS_NO_PATHCONV=1` on every adb invocation.

Outputs, all in this directory:

| file | what it is |
|---|---|
| `xiaomi-adreno830.env` | completed profile, `PROFILE_VERIFIED=1` |
| `oppo-mali.env` | completed profile, `PROFILE_VERIFIED=1` |
| `pin_device.sh` | standalone `<serial> pin\|unpin\|check` helper, pure adb + su |
| `REPORT.md` | this file |

Both devices were left **unpinned**, confirmed by `pin_device.sh <serial> check` at the end
(transcripts at the bottom). Every node written was restored.

---

## Headline: bench.sh cannot pin either of these devices

This is the finding that matters most, because it inverts an assumption written into the
existing profiles.

The `pin_freqs()` / `unpin_freqs()` pair in bench.sh writes
`/proc/ppm/policy/hard_userlimit_{min,max}_cpu_freq` and `/proc/gpufreq/gpufreq_opp_freq`.
On these devices:

| path | `35d0befa` (Qualcomm) | `3B159D009VZ00000` (MediaTek) |
|---|---|---|
| `/proc/ppm/policy/` | absent | **absent** |
| `/proc/gpufreq/` | absent | **absent** (superseded by `/proc/gpufreqv2/`) |

The Qualcomm case was already anticipated - `xiaomi-adreno830.env` said so. The MediaTek case
was **not**: the old `oppo-mali.env` reasoned "this part is a MediaTek SoC, so unlike the Adreno
profile the harness's existing /proc/ppm + /proc/gpufreq pin path is probably the right one" and
set `PIN_STYLE=ppm`. It is not right. MT6993 is new enough to have dropped both legacy
interfaces. So the exact failure mode the `PROFILE_VERIFIED` guard was written to prevent - a
root `echo` redirect into a missing /proc path exiting 0, and the run then reporting itself as
pinned - was waiting on the device the guard's own comment guessed was safe.

Consequence for P2: run bench.sh / session.sh with **`--no-pin`**, drive the pin with
`pin_device.sh`, and use `pin_device.sh check` in place of the `big_cur`/`little_cur`/`gpu_cur_khz`
integrity fields. `PROFILE_VERIFIED=1` in the two profiles certifies *the nodes and pins in those
files*, not that bench.sh can drive them; both files say so in a header block. Teaching bench.sh
a `PIN_STYLE` switch is the real fix and is not done here.

---

## Device 1 - `35d0befa`, Xiaomi 24129PN74C / SM8750 / Adreno 830v2

### CPU: which policy is "big", which is "little"

`ro.board.platform=sun`, `ro.soc.model=SM8750`. Two policies, and **no true little cluster** -
SM8750 is a 2+6 part:

```
policy0  cpus 0 1 2 3 4 5   gov=walt  cpuinfo 384000..3532800   (6x performance)
  avail: 384000 556800 748800 960000 1152000 1363200 1555200 1785600 1996800 2227200
         2400000 2745600 2918400 3072000 3321600 3532800
policy6  cpus 6 7           gov=walt  cpuinfo 1017600..4320000  (2x prime)
  avail: 1017600 1209600 1401600 1689600 1958400 2246400 2438400 2649600 2841600 3072000
         3283200 3513600 3801600 4089600 4204800 4320000
```

So **big = policy6, little = policy0**, and both protocol targets are *exact* members of
`scaling_available_frequencies` - no rounding:

| role | policy | target | chosen | rounding |
|---|---|---|---|---|
| big | policy6 | 1.96 GHz | **1958400** | exact |
| little | policy0 | 1.55 GHz | **1555200** | exact |

### CPU: how to pin, and does it hold

Method: `scaling_min_freq = scaling_max_freq = target`, **stock `walt` governor left alone**. No
performance or userspace governor is needed - once min == max the governor has no room, and not
switching it means nothing extra to restore. (`migov walt conservative powersave performance
schedutil` are all offered.)

Writes returned exit 0 and took effect immediately. Held for the whole window under load
(8 concurrent `yes > /dev/null`), sampled every 5 s:

```
t=1 p0_cur=1555200 p0_min=1555200 p0_max=1555200  p6_cur=1958400 p6_min=1958400 p6_max=1958400  gpuclk=1100000000  cpuss-0-0=47100
t=2 p0_cur=1555200 (min/max same)                 p6_cur=1958400 (min/max same)                 gpuclk=1100000000  cpuss-0-0=49400
t=3 p0_cur=1555200 (min/max same)                 p6_cur=1958400 (min/max same)                 gpuclk=1100000000  cpuss-0-0=49800
t=4 p0_cur=1555200 (min/max same)                 p6_cur=1958400 (min/max same)                 gpuclk=1100000000  cpuss-0-0=50200
t=5 p0_cur=1555200 (min/max same)                 p6_cur=1958400 (min/max same)                 gpuclk=1100000000  cpuss-0-0=51300
t=6 p0_cur=1555200 (min/max same)                 p6_cur=1958400 (min/max same)                 gpuclk=1100000000  cpuss-0-0=52100
t=7 p0_cur=1555200 (min/max same)                 p6_cur=1958400 (min/max same)                 gpuclk=1100000000  cpuss-0-0=52100
```

7 samples over 30 s, **zero drift** on either cluster or the GPU, while the SoC heated 47 -> 52 C.

Restore sequence (verified back to the stock snapshot):

```
policy6: scaling_min_freq=1017600  scaling_max_freq=2841600
policy0: scaling_min_freq=556800   scaling_max_freq=2745600
```

`pin_device.sh` writes min -> `cpuinfo_min_freq` first, then max, then min, so the order works
regardless of where stock sits relative to the target (writing min above the current max is
clamped by cpufreq).

### GPU: kgsl pwrlevels

```
gpu_model=Adreno830v2   num_pwrlevels=14   default_pwrlevel=12
gpu_available_frequencies: 1100000000 1050000000 967000000 900000000 832000000 734000000
                           660000000 607000000 525000000 443000000 389000000 342000000
                           222000000 160000000
stock: min_pwrlevel=12  max_pwrlevel=0  gpuclk=222000000
       devfreq/governor=msm-adreno-tz  devfreq/min_freq=160000000  devfreq/max_freq=1050000000
```

Pin: `echo 0 > /sys/class/kgsl/kgsl-3d0/min_pwrlevel` and the same to `max_pwrlevel`, collapsing
the range onto level 0. `gpuclk` read `1100000000` immediately and for the whole 30 s window.

Worth noting: **the devfreq route cannot reach the top step.** `devfreq/max_freq` is 1050000000,
one OPP below level 0, so a `devfreq/min_freq` + `max_freq` pin tops out at 1050 MHz. The
pwrlevel pin unlocks 1100 MHz. Restore: `min_pwrlevel=12`, `max_pwrlevel=0`.

**Unit trap.** bench.sh reports `gpu_cur_khz` from `$GPU_CURFREQ_NODE` via `awk` taking the last
field. The kgsl `gpuclk` node is in **Hz**; the MediaTek `current_freqency` node is in **kHz**.
So on this device the field bench.sh calls `gpu_cur_khz` is actually Hz. The profile carries
`GPU_PIN_HZ=1100000000` and `GPU_CURFREQ_UNIT=hz` alongside a schema-compatible
`GPU_PIN_KHZ=1100000`; compare against the former.

**GPU util node corrected.** The draft profile had `GPU_UTIL_NODE=/sys/class/kgsl/kgsl-3d0/gpubusy`.
Reading `gpubusy` prints `263821 1008410` - busy and total *cycles* - and bench.sh takes the
first field, so it would have logged raw cycle counts as a busy percentage. Changed to
`gpu_busy_percentage`, which prints `26 %` and whose first field is what bench.sh means.

### Thermal

84 zones; type strings are unique, so the match-first-zone-by-type loop in bench.sh is
unambiguous. Chosen: **`cpuss-0-0`** (`thermal_zone13`), the CPU-subsystem sensor for the 6-core
cluster that carries most of the load, and the hottest of the candidates under the load test:

| zone | idle | end of 30 s load |
|---|---|---|
| `cpuss-0-0` (13) | 34700 | 52100 |
| `cpuss-1-0` (20) | - | 49000 |
| `quiet_therm` (71) | 29835 | 32668 (skin sensor, not SoC) |
| `gpuss-0` (23) | 31200 | - |

The 40 C gate is reachable: it idles at 34.7 C, and was observed between 36.6 and 40.1 C across
the session.

---

## Device 2 - `3B159D009VZ00000`, Oppo PLG110 / MT6993 / Mali

### CPU: three policies, and the one that would have silently ruined the pin

`ro.soc.model=MT6993`. MT6993 is a **4+3+1** part:

```
policy0  cpus 0 1 2 3   gov=sugov_ext  cpuinfo 300000..2700000   stock max 2100000
policy4  cpus 4 5 6     gov=sugov_ext  cpuinfo 300000..3500000   stock max 3500000
policy7  cpu  7         gov=sugov_ext  cpuinfo 300000..4210000   stock max 3200000
```

Decision: **big = policy4, little = policy0**, *and policy7 is pinned to the big target as well*.
policy4 rather than policy7 is the structural analogue of the 2-core policy6 on the Xiaomi - a
multi-core cluster the render and worker threads can share, rather than a single core they would
contend for. But policy7 cannot be left alone: an unpinned 4.21 GHz core defeats the entire pin
the moment the scheduler lands a hot thread on it, and nothing in the result JSON would show it.
Pinned, the device reads as 4 little + 4 big.

Neither target is an exact OPP here - both are the **nearest available step**:

| role | policy | target | candidates | chosen | offset |
|---|---|---|---|---|---|
| big | policy4 | 1958400 | 1900000 (-58400), 2000000 (+41600) | **2000000** | +2.1% |
| little | policy0 | 1555200 | 1500000 (-55200), 1600000 (+44800) | **1600000** | +2.9% |
| extra | policy7 | 1958400 | 2000000 is an exact member | **2000000** | +2.1% |

Carry that 2-3% offset when comparing absolute per-thread CPU cost against the Xiaomi.

### CPU: how to pin, and does it hold

Same method (min = max = target, stock `sugov_ext` left alone). All six writes returned 0.
Held under the same 8-way load, sampled every 5 s, in `cur/min-max` form:

```
t=1 p0=1600000/1600000-1600000  p4=2000000/2000000-2000000  p7=2000000/2000000-2000000  gpu="0 1716000"  soc_max=52833
t=2 p0=1600000/1600000-1600000  p4=2000000/2000000-2000000  p7=2000000/2000000-2000000  gpu="0 1716000"  soc_max=56859
t=3 p0=1600000/1600000-1600000  p4=2000000/2000000-2000000  p7=2000000/2000000-2000000  gpu="0 1716000"  soc_max=58063
t=4 p0=1600000/1600000-1600000  p4=2000000/2000000-2000000  p7=2000000/2000000-2000000  gpu="0 1716000"  soc_max=59661
t=5 p0=1600000/1600000-1600000  p4=2000000/2000000-2000000  p7=2000000/2000000-2000000  gpu="0 1716000"  soc_max=60353
t=6 p0=1600000/1600000-1600000  p4=2000000/2000000-2000000  p7=2000000/2000000-2000000  gpu="0 1716000"  soc_max=61354
t=7 p0=1600000/1600000-1600000  p4=2000000/2000000-2000000  p7=2000000/2000000-2000000  gpu="0 1716000"  soc_max=62025
```

7 samples over 30 s, **zero drift** on all three policies, SoC 52.8 -> 62.0 C. The ColorOS
performance daemons did not contend for the cpufreq nodes during the window.

**Read permissions.** `scaling_governor`, `scaling_min_freq` and `scaling_max_freq` are `0660
system:system` here. A plain `adb shell cat .../scaling_governor` answers `Permission denied`,
which a careless parser reads as an empty governor rather than a failure. Every read goes through
su. Note `scaling_cur_freq` and `scaling_available_frequencies` *are* world-readable, which makes
the trap worse - a script can look like it is working.

### GPU: gpufreqv2, not legacy gpufreq

57 working OPPs, index 0 fastest:

```
/proc/gpufreqv2/gpu_working_opp_table
  [00] freq: 1716000  [01] 1690000  [02] 1664000 ... [56] 390000     (kHz)
```

Pin: `echo 0 > /proc/gpufreqv2/fix_target_opp_index`; restore: `echo -1`. Evidence:

```
before  : [GPUFREQ-DEBUG] fix GPU/STACK OPP index is disabled
after 0 : [GPUFREQ-DEBUG] fix GPU/STACK OPP index: 0/0
          /sys/kernel/ged/hal/current_freqency = "0 1716000"      (index 0, 1716000 kHz)
after -1: [GPUFREQ-DEBUG] fix GPU/STACK OPP index is disabled
```

**Deliberately not the ged `custom_boost_gpu_freq` / `custom_upbound_gpu_freq` pair**, the usual
MTK route, for two reasons both visible in the nodes:

1. On this kernel they take an **OPP index, not kHz**. Stock reads `boost=56`, `upbound=0` - a
   floor at the slowest OPP and a ceiling at the fastest. Porting the kHz-valued
   `GPU_PIN_KHZ=902000` from odinlite.env into them would write garbage.
2. Reading them prints a request log showing
   `/odm/bin/hw/vendor.oplus.hardware.urcc-service` (pid 1534) writing the same nodes. A pin
   there is one voter among several. `fix_target_opp_index` has no such contention.

### Thermal

87 zones, unique types. Chosen: **`soc_max`** (`thermal_zone14`) - the SoC-wide max aggregate,
which is what "SoC temperature" means for a gate, and the strictest candidate.

Cooldown series after the load test (screen awake, idle), 15 s apart:

```
soc_max  39094  37177  36806  35745  36954  36312
soc-top0 36361  35082  34641  34226  34079  34031
ap_ntc   36414  35034  34342  33965  33746  33566
```

It settles to **36-37 C**, so `THERMAL_START_MAX_MC=40000` is reachable - but only just, and it
was still reading 42.4 C several minutes after ordinary use. Expect this gate to actually wait,
unlike the one on the Xiaomi. Under load it went 52.8 -> 62.0 C in 30 s.

---

## Every node read or written

Read-only (both devices): `/sys/devices/system/cpu/cpufreq/policy*/{affected_cpus,
scaling_governor,scaling_available_governors,scaling_available_frequencies,cpuinfo_min_freq,
cpuinfo_max_freq,scaling_cur_freq}`, `/sys/class/thermal/thermal_zone*/{type,temp}`,
`getprop`, `id`.

Read-only, Xiaomi: `/sys/class/kgsl/kgsl-3d0/{gpu_model,num_pwrlevels,default_pwrlevel,
thermal_pwrlevel,max_gpuclk,gpuclk,clock_mhz,max_clock_mhz,min_clock_mhz,
gpu_available_frequencies,freq_table_mhz,gpubusy,gpu_busy_percentage,throttling,temp}`,
`/sys/class/kgsl/kgsl-3d0/devfreq/{name,governor,available_governors,available_frequencies,
cur_freq,min_freq,max_freq,target_freq,gpu_load,mod_percent}`, `/sys/class/devfreq/`.

Read-only, Oppo: `/proc/gpufreqv2/{gpufreq_status,gpu_working_opp_table,fix_target_opp_index}`,
`/sys/kernel/ged/hal/{current_freqency,custom_boost_gpu_freq,custom_upbound_gpu_freq,
gpu_utilization,total_gpu_freq_level_count}`.

**Written** (all restored):

| device | node | stock | pinned to | restored to |
|---|---|---|---|---|
| Xiaomi | `policy6/scaling_min_freq` | 1017600 | 1958400 | 1017600 |
| Xiaomi | `policy6/scaling_max_freq` | 2841600 | 1958400 | 2841600 |
| Xiaomi | `policy0/scaling_min_freq` | 556800 | 1555200 | 556800 |
| Xiaomi | `policy0/scaling_max_freq` | 2745600 | 1555200 | 2745600 |
| Xiaomi | `kgsl-3d0/min_pwrlevel` | 12 | 0 | 12 |
| Xiaomi | `kgsl-3d0/max_pwrlevel` | 0 | 0 | 0 |
| Oppo | `policy0/scaling_min_freq` | 300000 | 1600000 | 300000 |
| Oppo | `policy0/scaling_max_freq` | 2100000 | 1600000 | 2100000 |
| Oppo | `policy4/scaling_min_freq` | 300000 | 2000000 | 300000 |
| Oppo | `policy4/scaling_max_freq` | 3500000 | 2000000 | 3500000, then 3200000 by the vendor daemon - see below |
| Oppo | `policy7/scaling_min_freq` | 300000 | 2000000 | 300000 |
| Oppo | `policy7/scaling_max_freq` | 3200000 | 2000000 | 3200000 |
| Oppo | `/proc/gpufreqv2/fix_target_opp_index` | disabled | 0 | -1 (disabled) |

No governor was ever written on either device; both stayed on their vendor governor (`walt`,
`sugov_ext`) throughout. Nothing else was written - no `settings put`, no thermal-engine
tampering, no app installs.

### The one restore that does not settle at its stock value

On the Oppo, `policy4/scaling_max_freq` was restored to its snapshotted 3500000 and a ColorOS
daemon lowered it to **3200000** by itself within seconds. This is correct behaviour - the range
is handed back and the vendor takes over - but it means **stock maxima on this device are not
constants**.

It caught a real bug in the first version of `pin_device.sh check`, which classified each node as
at-pin / at-stock / neither and reported DRIFT for anything else: it declared DRIFT on a device
that had been released perfectly. The classifier now asserts only against *our* pins (pinned
means `min == max == pin`; anything else is not pinned, with a `min == max` at some other value
flagged as an external clamp, which is equally fatal to comparability). The hardcoded stock
values still drive the restore path, where handing the range back is all they have to do.

---

## `pin_device.sh`

Usage: `pin_device.sh <serial> pin|unpin|check`. Pure adb + su; no profile sourcing, no bench.sh.
The device table (policies, pins, stock values, GPU style, thermal zone type) is embedded so it
has no inputs to get wrong. All three actions print the live big/little/GPU frequencies,
governors and gate temperature, so the output of `pin` and of `unpin` is itself the before/after
evidence.

Exit codes: **0** PINNED (every pinned node at its pin and every live frequency equal to it),
**1** DRIFT (partly pinned, externally clamped, or a live frequency has left its pin - discard any
overlapping run), **2** UNPINNED (no node is at a pin this script set). UNPINNED is non-zero on
purpose, so that `pin_device.sh X check && measure` cannot silently measure an unpinned device.

Three things it does that are load-bearing:

- **Write order** is min -> `cpuinfo_min_freq`, then max -> target, then min -> target. Setting
  min above the current max is clamped by cpufreq, so a naive two-write pin half-applies
  depending on where stock sits relative to the target.
- **Stock values are hardcoded, not sampled at pin time.** A restore that read "stock" off an
  already-pinned device would make the pin permanent.
- **The device-side read emits raw node contents and parses locally.** The read is delivered as a
  single-quoted `su -c` argument, so a single quote inside it (an awk program, a sed expression)
  closes that quoting and the whole read returns nothing. That bug was hit during this work: the
  Oppo `check` printed `<unread>` for every field and reported DRIFT on a correctly pinned
  device. A zero-key read is now a hard error (exit 66), not a verdict.

### Final `check` on both devices, unpinned state

```
$ pin_device.sh 35d0befa check
== check ==
  device : Xiaomi 24129PN74C / SM8750 / Adreno 830v2 (35d0befa)
  big     policy6  gov=walt       cur=2841600   min=1017600   max=2841600    (pin 1958400 / stock 1017600-2841600)
  little  policy0  gov=walt       cur=2400000   min=556800    max=2745600    (pin 1555200 / stock 556800-2745600)
  gpu     kgsl-3d0 pwrlevel=0..12  freq=222000000 Hz  busy=7%   (pin lvl 0 = 1100000000 Hz / stock lvl 0..12)
  thermal cpuss-0-0 38200 mC = 38.2 C   (/sys/class/thermal/thermal_zone13)

  VERDICT: UNPINNED - none of the 3 nodes is at a pin this script set; the vendor
           governors have their range back and nothing this script writes is in effect.
rc=2
```

```
$ pin_device.sh 3B159D009VZ00000 check
== check ==
  device : Oppo PLG110 / MT6993 / Mali (gpufreqv2) (3B159D009VZ00000)
  big     policy4  gov=sugov_ext  cur=300000    min=300000    max=3200000    (pin 2000000 / stock 300000-3500000)
  little  policy0  gov=sugov_ext  cur=2100000   min=300000    max=2100000    (pin 1600000 / stock 300000-2100000)
  extra   policy7  gov=sugov_ext  cur=2750000   min=300000    max=3200000    (pin 2000000 / stock 300000-3200000)
  gpu     gpufreqv2 fix_opp=off  freq=1508000 kHz  busy=0%   (pin idx 0 = 1716000 kHz / stock off)
  thermal soc_max  45292 mC = 45.3 C   (/sys/class/thermal/thermal_zone14)

  VERDICT: UNPINNED - none of the 4 nodes is at a pin this script set; the vendor
           governors have their range back and nothing this script writes is in effect.
    note: big(policy4) unpinned, range 300000-3200000 (recorded stock 300000-3500000, which vendor daemons move)
rc=2
```

Both report their stock governors (`walt`, `sugov_ext`), the GPU back on DVFS
(pwrlevel 0..12 / `fix_opp=off`), and cpufreq ranges wide open. Nothing written is still in
effect.

For the record, the pinned half of the same cycle on each device:

```
$ pin_device.sh 35d0befa pin      (after pin)
  big     policy6  gov=walt       cur=1958400   min=1958400   max=1958400
  little  policy0  gov=walt       cur=1555200   min=1555200   max=1555200
  gpu     kgsl-3d0 pwrlevel=0..0  freq=1100000000 Hz
  VERDICT: PINNED - all 3 pinned nodes at their pins, live frequencies match.   rc=0

$ pin_device.sh 3B159D009VZ00000 pin      (after pin)
  big     policy4  gov=sugov_ext  cur=2000000   min=2000000   max=2000000
  little  policy0  gov=sugov_ext  cur=1600000   min=1600000   max=1600000
  extra   policy7  gov=sugov_ext  cur=2000000   min=2000000   max=2000000
  gpu     gpufreqv2 fix_opp=0     freq=1716000 kHz
  VERDICT: PINNED - all 4 pinned nodes at their pins, live frequencies match.   rc=0
```

---

## What is NOT verified

1. **bench.sh cannot drive these pins.** Not fixed here. The profiles are verified; the harness
   integration is not. Use `--no-pin` plus `pin_device.sh` until a `PIN_STYLE` switch exists.
2. **The Oppo GPU pin was confirmed at the DVFS-request level, not under graphics load.** The
   pinned window was a CPU load, so the GPU was power-collapsed throughout (`gpufreq_status`
   showed `PowerCount: 0`). `current_freqency` reported OPP index 0 / 1716000 kHz for the whole
   window - which is exactly the field bench.sh samples as `gpu_cur_khz`, so the check bench.sh
   would perform does pass - but nobody has watched this GPU actually clock 1716 MHz while
   rendering. Related readout quirk: while powered down, `/proc/gpufreqv2/gpufreq_status` keeps
   printing `Freq: 26000` for the fixed OPP entry. That is a parked rail, not the pin failing;
   `current_freqency` is the node to believe.
3. **Pin durability over a full bench window is untested.** The verification window is 30 s of
   synthetic CPU load, which is what the README defines. Neither device was held pinned through a
   180 s warmup plus a 30-sample measurement with the game running, where a vendor game-boost
   daemon has far more reason to intervene than it did here. Running `pin_device.sh check` after
   each run is what closes that gap; treat `rc=1` as "discard the run".
4. **Thermal gate behaviour under the real protocol is untested.** 40 C is reachable on both, but
   on the Oppo it idles only 3-4 C below the gate, so back-to-back paired runs will spend real
   time waiting. Nobody has measured how long.
5. **No reboot test.** All pins were applied and released within one session. Nothing here
   persists across a reboot by construction (every node written is volatile sysfs or procfs), but
   that was not demonstrated.
6. **The `walt` governor and the Xiaomi thermal daemon were never provoked.** 30 s at 52 C is
   well short of the thermal-limit regime where `thermal_pwrlevel` (read 0 throughout) would
   start capping the GPU.
