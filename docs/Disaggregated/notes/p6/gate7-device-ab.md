# Gate 7 — Redmi `2f7cbe2e` paired A/B, and gate 8 item 1 (the doorbell)

2026-09-22, branch `feat/disaggregated`, source head `98d0b96c`. `CONTRACT-P6.md` §9 items 7 and 8-①.

Everything below was measured on one reboot-clean thermal window on Redmi `M332BF` (`2f7cbe2e`,
SM8750, Adreno 830v2) from a **snapshot of the worktree** (`C:/Users/geekerwan/p6-snap/MobileGL-disagg`),
never from the shared tree: two other agents were editing `MobileGL/MG_Util/Metrics/PipeStats.*`
and the submodule pointers at the same time.

**Verdict up front.** The pair is a **tie**. `spawn` is *not* slower than `inproc` on this workload —
it is 0.15 % faster on the primary metric (mean of the two interleaved orders), deep inside the
run-to-run spread — and both are ~30 % *cheaper* than the `monolith` control on client CPU per frame
(+23 % in fps). The socket doorbell did **not** deliver the cheaper handoff the contract's §9 note
anticipated, but it also did not cost anything measurable, which is a different and more useful answer
than the note's.

Gate 8 item ① is **partially** delivered: the client half of the ledger is real on both arms and
**the server half is unmeasurable under `spawn` on this build**, for a structural reason found while
collecting it (§6). That is a finding, not a collection failure, and it is reported rather than
substituted with a number from the other arm.

---

## 1. Protocol actually executed

| item | value |
|---|---|
| device | Redmi `M332BF`, serial `2f7cbe2e`, SM8750 / Adreno 830v2 |
| reboot | `adb reboot`, `sys.boot_completed=1`, then boot id compared |
| boot id before | `e69d0329-2264-4a2d-9b00-e1a4ffb6154d` |
| **boot id after** | **`107f24d3-b1f1-4a97-ab8c-9706d1901f32`** (changed ⇒ the reboot is a fact, not a claim) |
| CPU pin | little `policy0` → 1555200, big `policy6` → 1958400 (`pin_device.sh 2f7cbe2e pin`) |
| GPU pin | `kgsl-3d0` `min/max_pwrlevel = 0` → **1050000000 Hz** (see §5.1 — this unit cannot reach 1100 MHz) |
| fan | `/sys/class/xm_power/hw_monitor/pwm_fan` `target_level=2`, `real_speed=14848` rpm, `pwm_duty=65` |
| arm order | `monolith, inproc, spawn, inproc, monolith, spawn` — interleaved, one window |
| repeats | `--benchmark-repeats 3`, `--benchmark-tail-frames 200`, `--benchmark-no-finish`, best-of-3 |
| workload | `minecraft-1.21.4-rd12-odinlite-in-world`, DirectGLES, 251 frames, **1471.36 draws/frame** |
| stats | `MOBILEGL_PIPE_STATS=1`, `MOBILEGL_PIPE_STATS_PERIOD=120` |
| split knobs | `MOBILEGL_IPC_ROLE_SPLIT_STATE=1`, `MOBILEGL_IPC_STRICT_ERRORS=1`, `MOBILEGL_IPC_RUN_AHEAD=1` |
| APK | `MobileGL-plugin-trace-release-p6gate7.apk`, sha256 `8882683b063db13e16236530007c9b364de55fe7a11fba968169ebe2942fcfaf` |
| 6 arms × 3 repeats | **18/18 runner exits 0, 36/36 pin checks `PINNED`, 0 `Fatal{`** |

Per-arm pin evidence is in each arm directory's `pin-before.txt` / `pin-after.txt`; both exit 0
(`PINNED`) for all six arms. The sampled temperature on `cpuss-0-0` across all 12 checks was
**37.6–46.5 °C** (min 37.6, max 46.5), i.e. no arm ran cold and none approached throttling.

### 1.1 The build

```bash
# from the SNAPSHOT, never the shared tree
cd /c/Users/geekerwan/p6-snap/MobileGL-disagg
export JAVA_HOME="C:/Program Files/Java/jdk-21"
export JAVA_TOOL_OPTIONS='-Djdk.net.unixdomain.tmpdir=C:\Users\geekerwan\.gradle\uds'
export ANDROID_HOME="C:/Users/geekerwan/AppData/Local/Android/Sdk"
"/c/Users/geekerwan/.gradle/wrapper/dists/gradle-8.13-bin/5xuhj0ry160q40clulazy9h7d/gradle-8.13/bin/gradle.bat" \
  --no-daemon -p android-plugin :app:assembleTraceRelease \
  -Pmobilegl.buildDisaggregated=ON -Pmobilegl.buildDisaggregatedInproc=ON -Pmobilegl.pipePush=ON \
  -Pmobilegl.apkSuffix=p6gate7 -Pmobilegl.debuggableRelease=true \
  -Pmobilegl.applicationIdSuffix=.p6gate7 -Pmobilegl.logLevel=MOBILEGL_LOG_LEVEL_INFO
# BUILD SUCCESSFUL in 10s (incremental; 2m09s cold)
```

Two build facts worth carrying forward, both stated by the CI workflow and both required:

- `-Pmobilegl.debuggableRelease=true` is what makes the release APK `run-as`-able. Without it
  `trace-replay-ci.sh` cannot pull the private logs at all and every arm comes back with no library
  log — indistinguishable from a run that produced none.
- The APK is **not** signed by Gradle here (`SIGNING_*` env is a CI secret). It was signed with the
  standard debug key (`~/.android/debug.keystore`, `androiddebugkey`) so `run-as` works:
  ```bash
  apksigner sign --ks ~/.android/debug.keystore --ks-pass pass:android \
    --ks-key-alias androiddebugkey --key-pass pass:android \
    --out trace-p6gate7-debugsigned.apk unsigned.apk
  ```
- `-Pmobilegl.applicationIdSuffix=.p6gate7` puts the build under
  `top.mobilegl.plugin.p6gate7.trace`, **beside** an unrelated trace install under the canonical id
  rather than replacing it. The runner's hard-coded package name is overridden by
  `MOBILEGL_TRACE_PACKAGE` (snapshot-local; see §5.4).

### 1.2 The run

```bash
cd /c/Users/geekerwan/p6-snap/MobileGL-disagg
python tools/device_bench/p6/gate7_ab.py \
  --tree "C:/Users/geekerwan/p6-snap/MobileGL-disagg" \
  --out  "C:/Users/geekerwan/p6-snap/evidence" \
  --apk  'C:\Users\geekerwan\p6-snap\artifacts\trace-p6gate7-debugsigned.apk' \
  --reboot --fan-level 2 --session gate7-rd12 \
  --arm monolith:DirectGLES:minecraft-1.21.4-rd12-odinlite-in-world:3 \
  --arm inproc:DirectGLES:minecraft-1.21.4-rd12-odinlite-in-world:3 \
  --arm spawn:DirectGLES:minecraft-1.21.4-rd12-odinlite-in-world:3 \
  --arm inproc:DirectGLES:minecraft-1.21.4-rd12-odinlite-in-world:3 \
  --arm monolith:DirectGLES:minecraft-1.21.4-rd12-odinlite-in-world:3 \
  --arm spawn:DirectGLES:minecraft-1.21.4-rd12-odinlite-in-world:3
```

Evidence root: `C:/Users/geekerwan/p6-snap/evidence/gate7-rd12/`. Six arm directories, each with
`arm-summary.json`, `benchmark-run{1,2,3}.json`, `mobilegl.client.log`, `mobilegl.server.log`,
`pin-before.txt`, `pin-after.txt`, `runner.stdout.txt`, `logcat.txt`; plus `session-summary.json`.

`tools/device_bench/p6/gate7_ab.py` and `render_gate7.py` are new in this snapshot (§7).

---

## 2. The paired A/B

Primary metric = **client thread CPU ms/frame**, the client GL thread's own
`CLOCK_THREAD_CPUTIME_ID` deltas summarised by the device over the trailing 200 frames
(`MEASUREMENTS.md` §9's rule; the kernel ignores this app's affinity requests, so a process total
mixes two clock domains). `fps` and wall p50/p95 are recorded beside it.

### 2.1 Best-of-3, per arm

| arm | order | frames | tail | wall p50 ms | wall p95 ms | **client CPU p50 ms** | CPU p95 ms | fps | client total ms | server total ms |
|---|---|---|---|---|---|---|---|---|---|---|
| monolith (a) | 1 | 251 | 200 | 11.320 | 12.369 | **11.231** | 12.267 | 86.32 | 11060 | — |
| inproc (a) | 2 | 251 | 200 | 7.971 | 18.196 | **7.880** | 17.466 | 106.04 | 8230 | 7800 |
| spawn (a) | 3 | 251 | 200 | 7.989 | 18.369 | **7.876** | 17.790 | 106.12 | 8930 | 8640 |
| inproc (b) | 4 | 251 | 200 | 8.001 | 18.072 | **7.889** | 17.489 | 105.78 | 8900 | 8390 |
| monolith (b) | 5 | 251 | 200 | 11.287 | 12.388 | **11.240** | 12.300 | 86.26 | 10860 | — |
| spawn (b) | 6 | 251 | 200 | 7.979 | 18.120 | **7.869** | 17.283 | 105.95 | 8960 | 8660 |

`client total` / `server total` are the per-role CPU **totals** the sampler accumulated over that
repeat. They include the single shader bring-up frame, so they are *not* per-frame rates and must not
be divided by 251 — see §5.2, where that mistake was made and caught.

### 2.2 The pair: `spawn` − `inproc`

Both orders shown rather than a cherry-picked one. The per-order column pairs are the two
repetitions of each transport; the Δ column uses their means.

| metric | inproc (a / b) | spawn (a / b) | Δ (mean) | Δ % |
|---|---|---|---|---|
| **client CPU p50 ms/frame** | 7.880 / 7.889 | 7.876 / 7.869 | **−0.012** | **−0.15 %** |
| client CPU p95 ms/frame | 17.466 / 17.489 | 17.790 / 17.283 | +0.059 | +0.34 % |
| wall p50 ms | 7.971 / 8.001 | 7.989 / 7.979 | −0.002 | −0.03 % |
| wall p95 ms | 18.196 / 18.072 | 18.369 / 18.120 | +0.111 | +0.61 % |
| fps | 106.04 / 105.78 | 106.12 / 105.95 | +0.125 | +0.12 % |
| client CPU total ms | 8230 / 8900 | 8930 / 8960 | +380 | +4.5 % |
| server CPU total ms | 7800 / 8390 | 8640 / 8660 | +555 | +6.7 % |

**The pair is a tie on every steady-state metric.** The per-order sign *flips* on the wall columns —
inproc is 0.03 % ahead in order (a) and spawn 0.03 % ahead in order (b) — which is the signature of
noise rather than of an effect. The client CPU p50 column is the one to read, and it is the primary
metric: 7.880 vs 7.876 in order (a) and 7.889 vs 7.869 in order (b), i.e. −0.15 % on the means.

The `* total ms` rows differ by 4.5–6.7 % and are **not** a contradiction: those totals include the
frame-1 shader compile, whose cost varies run to run by more than the transport's whole effect. The
per-frame columns exclude it by construction (the device summarises the trailing 200 frames), which
is exactly why the primary metric is the per-frame one.

### 2.3 Run-to-run spread, and why the tie is credible

All 18 repeats, client CPU p50 ms/frame:

| arm | rep 1 | rep 2 | rep 3 | spread |
|---|---|---|---|---|
| monolith (a) | 11.297 | 11.346 | 11.231 | 1.0 % |
| inproc (a) | 7.885 | 7.880 | 7.901 | 0.3 % |
| spawn (a) | 7.876 | 8.049 | 7.952 | 2.2 % |
| inproc (b) | 7.881 | 7.889 | 7.872 | 0.2 % |
| monolith (b) | 11.388 | 11.300 | 11.240 | 1.3 % |
| spawn (b) | 7.861 | 7.869 | 8.191 | 4.2 % |

Within-arm spread is 0.2–4.2 %, and the four `inproc`/`spawn` arms land in 7.87–7.95. The
between-transport difference (−0.006 ms) is an order of magnitude below the spread of the arms it is
drawn from. **This is a tie, stated as a tie.**

### 2.4 Against the `monolith` control

Not part of the pair, recorded per the protocol as the baseline row:

| comparison | client CPU p50 ms | Δ | fps | Δ |
|---|---|---|---|---|
| monolith → inproc | 11.236 → 7.885 | **−29.8 %** | 86.29 → 105.91 | +22.7 % |
| monolith → spawn | 11.236 → 7.873 | **−29.9 %** | 86.29 → 106.04 | +22.9 % |

Both split transports are ~30 % cheaper on client CPU than monolith on this workload, and the
`spawn` arm reaches it through a socket rather than a shared address space. **The doorbell's cost is
not visible at this workload's resolution.**

---

## 3. Arm proof (contract §9.5)

Every spawn entry recorded the child pid **and** `transport=spawn` in its own private log. The
runner also ran `--require-spawn`'s checks:

| arm | `Config: MOBILEGL_TRANSPORT=spawn` | `spawn ARMED - the server role runs in pid N` | server log pulled | verdict |
|---|---|---|---|---|
| spawn (a) | present | `pid 29990` | yes | OK |
| spawn (b) | present | `pid 22130` | yes | OK |

```
[Android MobileGLTraceRe/INFO]: MG_Remote client: spawn ARMED - the server role runs in pid 29990,
  reached at "@mgl-23197-12970367462759574816"
```

Both `inproc` arms carry `Config: MOBILEGL_TRANSPORT=inproc - the MGPipe record stream crosses a
real ring to an apply thread` and a `Config: IPC` line reading
`ring=8MiB stage=32MiB spin=50us … verb-barrier=1 run-ahead=1 present-credit=1 strict=1
role-split-state=1`. Both `monolith` arms carry **neither** marker and **no** `Config: IPC` line —
the control stayed a control. Zero `Fatal{` records in any of the 12 role logs.

Logs are per-role as P6 requires: `mobilegl.client.log` and `mobilegl.server.log` both pulled for
every arm that has a server role. The monolith arms legitimately produce only the client file.

---

## 4. Gate 8 item ① — the doorbell ledger, and the arms measured together

### 4.1 The four counters

`srv` / `srvpark` = entries into the apply thread's `Doorbell::Wait` and the subset that stopped
spinning and blocked. `cli` / `clipark` = the same for the client producer. **Both pairs are RUN
TOTALS**, not window values — `PipeStats.h` says so beside the Gauge enum, and the rates below
therefore divide by the run's `frames=251`, **not** by the `window=` on the same line. Dividing by
`window=` inflates the rate by run/window (here ~21×) into a number that looks plausible and is not.

### 4.2 `inproc` (both arms agree)

| arm | srv | srvpark | cli | clipark | srv/f | srvpark/f | cli/f | clipark/f |
|---|---|---|---|---|---|---|---|---|
| inproc (a) | 2 738 247 | 1 543 | 30 407 | 1 548 | 10909.4 | 6.15 | 121.1 | 6.17 |
| inproc (b) | 2 727 377 | 1 281 | 30 407 | 1 399 | 10866.0 | 5.10 | 121.1 | 5.57 |

Read: **~10 900 server waits per frame** at 1471 draws/frame — about 7.4 waits per draw, on a shared
cache line, with only **0.06 % of them parking** (1 543 of 2.74 M). The client's own pair is far
smaller: **121 waits/frame, 0.06 % parking**. The spin budget is covering the handoff almost
completely, which matches the P5d/P5e characterisation (`SPIN_US=50`, parks near zero, the cost is
the rendezvous itself) and confirms that the contract's "1600 spins/frame on a shared cache line"
note is about the *right* quantity — the server side of the rendezvous, not the client's.

That ratio is the number that makes the socket comparison interesting: `inproc` pays 10 900
cache-line rendezvous per frame, and each one is a spin on memory both threads share.

### 4.3 `spawn` — the client half, and why the server half is missing

| arm | cli | clipark | cli/f | clipark/f | srv | srvpark |
|---|---|---|---|---|---|---|
| spawn (a) | 30 401 | 1 538 | 121.1 | 6.13 | **unavailable** | **unavailable** |
| spawn (b) | 30 401 | 1 530 | 121.1 | 6.10 | **unavailable** | **unavailable** |

The client-side numbers are **byte-for-byte the `inproc` client numbers** (30 407 vs 30 401, 121.1/f
both, parks within 1 %). That is the expected shape — the client does the same work either way — and
it is the first half of item ①: **the client's wait count and park rate are transport-invariant**, so
whatever the socket changes, it is not the client's wait frequency.

The server half is not collected because **the spawn server process never enables PipeStats at all**
(§6). Its client log prints `srv=0 srvpark=0` for the whole run, which is the "another process"
zero `PipeStats.h` warns about, not a measurement of zero waits. An earlier OpenRA spawn run showed
the same: no `MGPipe stats:` line in `mobilegl.server.log` and no `counters ON` line either.

### 4.4 What item ① can and cannot say

- **Client side**: measured on both arms. 121.1 waits/frame, 6.1 parks/frame, transport-invariant.
- **Server side under `inproc`**: measured. 10 866–10 909 waits/frame, 5.1–6.2 parks/frame, 0.06 % park rate.
- **Server side under `spawn`**: **not measureable on this build.** The relative cost of a socket
  `read()`/`write()` handoff against a shared-cache-line spin cannot be quoted as a ratio from this
  session, because one of its two halves does not exist.

The honest statement is therefore the one §2 already supports: at this workload the two transports
produce the **same steady-state client CPU per frame to within 0.2 %**, so a socket handoff replacing
a spin+park handoff costs *nothing detectable* — even though the doorbell's own accounting on the
spawn side is invisible. The contract's hypothesis (§9's note: "converting ~1600 spins/frame into
~4 blocking reads/frame may be cheaper") is **not falsified and not confirmed**; the observable it
should have moved, client CPU ms/frame, did not move in either direction.

---

## 5. Known issues and traps found while collecting

All five are reported because each one produced a plausible-looking wrong number before it was
caught, and three of them are in the harness rather than in MobileGL.

### 5.1 This unit cannot reach the GPU's top pwrlevel (device fact, hardcoded)

`pin_device.sh`'s new `2f7cbe2e` row pins kgsl `min/max_pwrlevel = 0`, but on **this** unit that
yields `gpuclk = 1050000000`, **not** 1100000000:

```
thermal_pwrlevel = 1        # latches: writing 0 returns success and reads back 1
max_gpuclk       = 1050000000   # read-only; a write is accepted and ignored
devfreq/max_freq = 1050000000   # clamps a 1100000000 write down
```

So the row records `GPU_PINNED_FREQ=1050000000` — *the fastest frequency this board will actually
run*, not the top OPP — and `check` compares against the truth. Consequence for the campaign,
stated where the number is: **absolute times from this device are not comparable with the
`35d0befa` rows taken while it could reach 1100 MHz.** Only same-session pairs are.

### 5.2 A per-frame rate divided by the whole run (harness, fixed)

The sampler's steady-state reporting originally divided a replay's total per-role CPU by the frame
count. Frame 1 of this fixture costs **7332 ms** of client CPU (one shader/pipeline bring-up) against
7.7–8.5 ms for each of the other 250, so that column read ~32 ms/frame beside a device median of
7.9. The device's own two series (`frameCpuTimesMs[]`, and the `medianFrameCpuMs` it summarises over
the tail) are the correct source for per-frame figures; the sampler contributes **roles and totals**,
which is what the doorbell comparison needs and what the client-only series cannot express. Fixed in
`render_gate7.py` (`steady_state_cpu`, `role_totals`), and the reversal is documented there.

### 5.3 `--transport` is silently ignored in `--benchmark` mode (harness trap)

`run_android_retrace_local.py` applies `--transport` through `run_case()`, but `run_benchmark_case()`
passes `args.env` alone. **`--benchmark --transport spawn` runs monolith while its label says
spawn** — measured: a `--benchmark` run with `--transport inproc` wrote no `Config: IPC` line at all.
Every arm in §1 therefore spells its transport as an explicit `--env MOBILEGL_TRANSPORT=…` and every
arm is verified against the library's own log afterwards. Not changed in the tree (out of scope);
worth a fix in the runner or a guard in the arm-proof.

### 5.4 `MOBILEGL_RETRACE_USE_PBUFFER` must be exported, not passed as `--env` (harness trap)

`trace-replay-ci.sh` reads that variable **itself**, to add `--ez use_pbuffer true` to the intent. A
`--env` override only reaches the in-process applier of env knobs, which is a different mechanism, so
the retrace still asks for a window surface. That is fatal **on the spawn arm and only there**: an
`ANativeWindow*` is a pointer into the client's process and the wire refuses it by name (Rule H),
measured as a SessionFault carrying `Fatal{UnmigratedSurface, "AndroidNativeWindow@P12"}`. The
script's own environment is what makes it work.

### 5.5 The spawn arm needs a distinct package id (environment fact)

The machine carries a pre-existing trace install under `top.mobilegl.plugin.trace` signed with a key
that no longer exists in this session's reach, and two APKs signed by different keys cannot share an
id. Replacing it would have destroyed state this campaign does not own, so the gate-7 build was
installed as `top.mobilegl.plugin.p6gate7.trace` and the runner's package constant was overridden via
`MOBILEGL_TRACE_PACKAGE` in the **snapshot's** copy of `run_android_retrace_local.py` only (the tree
copy is untouched). The existing install was left exactly as found.

---

## 6. Finding: the spawn server process never enables PipeStats

**`MG_Util::PipeStats::Init()` is called from exactly one place, `MobileGL::Initialize()`
(`MobileGL/Init.cpp:123`). The spawn server process does not call `MobileGL::Initialize()`** — it
calls `MG_ConfigLoader::Init()` and then `MG_Backend::InitServerRoleForSpawn()`
(`MG_Remote/Server/ServerMain.cpp:181`, `:198`). Neither of those reaches `PipeStats::Init()`.

Consequences, all observed rather than inferred:

- `g_pipeStatsEnabled` stays `false` in the server process, so **none** of the 68 `PipeStats::Enabled()`
  gates fire there — the four `wait[]` gauges, the ring gauges, the byte-class and call-class counters
  and the summary line itself.
- `mobilegl.server.log` contains no `MGPipe stats:` line and no `counters ON` line, on either
  workload (rd12, 251 frames; OpenRA, 128 frames), on both spawn arms and on the exploratory runs.
  The client log's `wait[srv=0 srvpark=0]` is therefore structural, exactly as `PipeStats.h`
  describes for the spawn case.
- The server-side **byte** and **record** classes (`buf/f`, `tex/f`, `maxrec`) are likewise
  unavailable under spawn. Under `inproc` the same server role writes them into
  `mobilegl.server.log` and they are collected normally.

This is **not** repaired here: it is a MobileGL source change, and this task is explicitly barred from
touching MobileGL. It is reported as a finding with its exact cause so the next stage can decide
whether `PipeStats::Init()` belongs in `InitServerRoleForSpawn()`/`ServerMain` (which would make the
gate-8 numbers collectable on both split transports) or whether the server's counters are meant to
stay a spawn-blind spot.

**It does not affect the §2 pair**: the primary metric is the client thread's own per-frame CPU clock,
taken from `benchmark.json` in the client process, where PipeStats is enabled and irrelevant.

---

## 7. What was added

| path | what |
|---|---|
| `tools/device_bench/pin_device.sh` (+28 lines) | the `2f7cbe2e` device row: policies/stock/pins, kgsl nodes, `thermal_pwrlevel=1` / 1050 MHz ceiling, thermal type `cpuss-0-0`. All three actions verified (§8) |
| `tools/device_bench/p6/gate7_ab.py` (snapshot only) | the reboot-clean / pin / interleave / arm-proof orchestrator and per-thread CPU sampler |
| `tools/device_bench/p6/render_gate7.py` (snapshot only) | evidence → markdown tables |
| `tools/trace_replay/run_android_retrace_local.py` (snapshot only) | `MOBILEGL_TRACE_PACKAGE` override (§5.5) |

Only `pin_device.sh` is in the shared tree. The other three exist in
`C:/Users/geekerwan/p6-snap/MobileGL-disagg` and should be promoted deliberately rather than as a
side effect of this run.

---

## 8. `pin_device.sh 2f7cbe2e` verification

Every value measured on this unit on 2026-09-22, then exercised through all three actions:

```
$ bash tools/device_bench/pin_device.sh 2f7cbe2e check      # at stock
  big     policy6  gov=walt  cur=3072000  min=1017600  max=3072000   (pin 1958400 / stock 1017600-3072000)
  little  policy0  gov=walt  cur=2745600  min=384000   max=2745600   (pin 1555200 / stock 556800-2745600)
  gpu     kgsl-3d0 pwrlevel=0..12  freq=443000000 Hz  busy=0%   (pin lvl 0 = 1050000000 Hz / stock lvl 0..12)
  thermal cpuss-0-0 35200 mC = 35.2 C   (/sys/class/thermal/thermal_zone13)
  VERDICT: UNPINNED                                                     rc=2

$ bash tools/device_bench/pin_device.sh 2f7cbe2e pin
  ... after pin ...
  big  cur=1958400 min=1958400 max=1958400 | little cur=1555200 min=1555200 max=1555200
  gpu  pwrlevel=0..0 freq=1050000000 Hz
  VERDICT: PINNED - all 3 pinned nodes at their pins, live frequencies match.   rc=0

$ bash tools/device_bench/pin_device.sh 2f7cbe2e unpin
  little min=556800 max=2745600 | big min=1017600 max=3072000 | gpu pwrlevel=0..12
  VERDICT: UNPINNED                                                     rc=0
```

Also verified: the pin **holds under load** (8 concurrent busy loops → still `PINNED`; a
partially-pinned state was correctly reported as `DRIFT` mid-experiment rather than silently
accepted) and is **stable at idle** over 90 s. Stock values are hardcoded, not sampled at pin time,
so a restore cannot read "stock" off an already-pinned device and make the pin permanent.

---

## 9. Judgement

**Gate 7 (paired A/B, reboot-clean, one thermal window) — met.** Six arms, three repeats each,
`spawn` interleaved against `inproc` in both orders within one window, every arm's pin checked before
and after, one boot id for the whole window with the reboot proven by the id change.

**Gate 8 item ① — client half met, server half blocked by §6.** Not dressed up: the ratio the item
asks for cannot be computed from this session, and the reason is a named, located code path rather
than a missed collection step.

**The performance reading.** `spawn` and `inproc` are the same transport as far as the client's CPU
per frame is concerned (−0.15 % with the sign flipping between orders), and both are ~30 % cheaper
than `monolith`. Per the campaign's standing rule these numbers are **recorded, not gated**. The
targeted question — does a socket doorbell beat a shared-cache-line spin+park — came back
**inconclusive rather than negative**: the client-side wait accounting is identical across
transports, and the server-side accounting that would have shown the difference is not being
collected under spawn at all.

---

## 10. Raw evidence, quoted

### 10.1 Boot id (the reboot, before and after)

```
$ adb -s 2f7cbe2e shell cat /proc/sys/kernel/random/boot_id     # before `adb reboot`
e69d0329-2264-4a2d-9b00-e1a4ffb6154d
$ adb -s 2f7cbe2e shell cat /proc/sys/kernel/random/boot_id     # after boot_completed=1
107f24d3-b1f1-4a97-ab8c-9706d1901f32
```

One boot id covers all six arms; the temperature range across the 12 pin checks was 37.6–46.5 °C on
`cpuss-0-0`.

### 10.2 Pin evidence, `inproc` arm (a) — representative of all six

```
== before pin ==
  device : Redmi M332BF / SM8750 / Adreno 830v2 (2f7cbe2e)
  big     policy6  gov=walt  cur=1958400  min=1958400  max=1958400   (pin 1958400 / stock 1017600-3072000)
  little  policy0  gov=walt  cur=1555200  min=1555200  max=1555200   (pin 1555200 / stock 556800-2745600)
  gpu     kgsl-3d0 pwrlevel=0..0  freq=1050000000 Hz  busy=4%   (pin lvl 0 = 1050000000 Hz / stock lvl 0..12)
  thermal cpuss-0-0 39500 mC = 39.5 C   (/sys/class/thermal/thermal_zone13)

== after pin ==
  ... identical node values ...
  VERDICT: PINNED - all 3 pinned nodes at their pins, live frequencies match.
```

…and, after the arm's three repeats:

```
== check ==
  big     policy6  gov=walt  cur=1958400  min=1958400  max=1958400
  little  policy0  gov=walt  cur=1555200  min=1555200  max=1555200
  gpu     kgsl-3d0 pwrlevel=0..0  freq=1050000000 Hz  busy=22%
  thermal cpuss-0-0 45300 mC = 45.3 C
  VERDICT: PINNED - all 3 pinned nodes at their pins, live frequencies match.
```

Both exits are 0 (`PINNED`) for all six arms; `session-summary.json` records `pin_before_rc` and
`pin_after_rc` per arm.

### 10.3 Arm proof, verbatim

```
01 client: Config: MOBILEGL_TRANSPORT=inproc - the MGPipe record stream crosses a real ring to an apply thread
03 client: Config: MOBILEGL_TRANSPORT=spawn - the MGPipe record stream crosses a real ring to an
           apply thread in ANOTHER PROCESS
03 client: MG_Remote client: spawn ARMED - the server role runs in pid 29990,
           reached at "@mgl-23197-12970367462759574816"
06 client: MG_Remote client: spawn ARMED - the server role runs in pid 22130,
           reached at "@mgl-16187-12970367462759644032"

02 client: Config: IPC ring=8MiB stage=32MiB spin=50us persistent-block=64KiB adopt-tier=2
           verb-barrier=1 run-ahead=1 present-credit=1 strict=1 audit=0 role-split-state=1 affinity='auto'
01 client: (no `Config: IPC` line, no transport marker)   <- the control stayed a control
```

### 10.4 The ledger lines, verbatim (`inproc` arm (b), client and server logs)

```
client: ... wait[srv=2727377 srvpark=1281 cli=30407 clipark=1399] gates[ers=...]
server: ... wait[srv=2694923 srvpark=1273 cli=30239 clipark=1392] gates[ers=...]

spawn arm (a), client log only — the server file has no summary line at all:
client: ... wait[srv=0 srvpark=0 cli=30401 clipark=1538] gates[ers=0/0 etl=0/0 eub=0/0 ...]
```

### 10.5 Byte classes and record size, `inproc` (gate 8 items ② and ③ input)

From the trailing window of the `inproc` arms (`window=120` / client `window=11`, `frames=251`):

| role | window | run frames | buf/f | tex/f | ubog/f | ubon/f | csob-blob/f | resid/f | maxrec | maxcap |
|---|---|---|---|---|---|---|---|---|---|---|
| client | 11 | 251 | 10 289.45 | 342 341.82 | 281 585.45 | 0 | 281 568.00 | 161.45 | 1808 | 4 194 304 |
| server | 120 | 251 | 10 290.00 | 357 348.93 | 281 586.00 | 0 | 281 586.00 | 163.00 | 1808 | 4 194 304 |

`maxrec=1808` against a 4 MiB cap (0.04 %) — **content chunking is not exercised by this workload**,
and `ringwaits=0`. These are recorded here as input to gate 8 items ②/③, not as the item's answer:
item ③ ("records/frame post-chunking") needs the per-record distribution, which this summary line
does not carry — `MEASUREMENTS.md:342` already says in as many words that no post-chunking
distribution exists, and this session does not create one. Item ② (`SEG_STAGE` bytes/frame) is
readable from `buf/f + tex/f + ubog/f + vtxc/f + idxc/f + icmd/f`, but only on the **`inproc`
server role**; under `spawn` the server-side byte classes are unavailable for §6's reason.

