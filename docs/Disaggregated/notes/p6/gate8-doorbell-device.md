# Gate 8 item ① — the doorbell, measured on the device (server half included)

2026-09-22, branch `feat/disaggregated`, source head `94c130d6`. `CONTRACT-P6.md` §9 item 8-①.

The number this item asks for is the socket doorbell's cost against `inproc`'s condvar. The previous
session (`gate7-device-ab.md`) delivered the **client** half of that ledger and reported the
**server** half as structurally missing: `PipeStats::Init()` was called only from
`MobileGL::Initialize()`, which the spawn server process never runs, so every counter in it —
`ServerLoop`'s own `ServerWaits`/`ServerParks` publish included — was compiled in and permanently
false there. That gap is now closed in the tree (`MG_Remote/Server/ServerMain.cpp`, step 1.6 +
the matching `Shutdown`), and this session re-runs the focused protocol to collect the missing half.

**Verdict up front.** The server half is now real and reproducible: the spawn server counts
**10 415–10 416 server waits per frame**, against `inproc`'s **11 434–11 963**. The previous
session's judgement is unchanged — the pair remains a **tie** on client CPU per frame (7.992/8.009
vs 7.987/7.989, a 0.2 % difference with the arms' own spread larger than that), and both are ~30 %
cheaper than monolith. The doorbell hypothesis in §9's note — that a socket blocking read may be
*cheaper* than `inproc`'s spin+park — is now **answerable and answered: no measurable difference
either way on this workload.**

---

## 1. Protocol actually executed

Focused, not the full six-arm matrix: two transports, interleaved, best-of-2, one window.

| item | value |
|---|---|
| device | Redmi `M332BF`, serial `2f7cbe2e`, SM8750 / Adreno 830v2 |
| reboot | `adb reboot`, `sys.boot_completed=1`, boot id compared |
| boot id before | `107f24d3-b1f1-4a97-ab8c-9706d1901f32` |
| **boot id after** | **`7e6c6e9f-1894-4916-bf39-cce48c9abd37`** (changed ⇒ the reboot is a fact) |
| CPU pin | little `policy0` → 1555200, big `policy6` → 1958400 |
| GPU pin | `kgsl-3d0` `min/max_pwrlevel = 0` → **1050000000 Hz** (this unit cannot reach 1100 MHz) |
| fan | `target_level=2`, `real_speed=14404` rpm, `pwm_duty=65` |
| arm order | `inproc, spawn, spawn, inproc` — interleaved, one window |
| repeats | `--benchmark-repeats 2`, `--benchmark-tail-frames 200`, `--benchmark-no-finish`, best-of-2 |
| workload | `minecraft-1.21.4-rd12-odinlite-in-world`, DirectGLES, 251 frames, **1471.36 draws/frame** |
| stats | `MOBILEGL_PIPE_STATS=1`, `MOBILEGL_PIPE_STATS_PERIOD=120` |
| split knobs | `MOBILEGL_IPC_ROLE_SPLIT_STATE=1`, `MOBILEGL_IPC_STRICT_ERRORS=1`, `MOBILEGL_IPC_RUN_AHEAD=1` |
| **not set** | `MOBILEGL_PIPE_STATS_FILE` — deliberately absent this round (another agent is changing its role-suffix handling) |
| APK | `MobileGL-plugin-trace-release-p6gate8.apk`, sha256 `5eaf26c873c55fefb36f31f4a89364c71105d4cb8c0e6578f447d3a466378f86` |
| result | **4/4 runner exits 0, 8/8 pin checks `PINNED`, 0 `Fatal{`** |

Temperature across the 8 pin checks: **37.2 – 44.9 °C** on `cpuss-0-0`.

### 1.1 Build (from a snapshot, with the ServerMain fix)

```bash
# snapshot of the worktree, excluding .git / cmake-build-debug / build / .cxx / .trace-work /
# Testing / android-plugin/{app/build,.gradle,build}; the four nested 3rdparty/.../build dirs that
# name-matching also caught were restored afterwards
cd /c/Users/geekerwan/p6-snap2/MobileGL-disagg
export JAVA_HOME="C:/Program Files/Java/jdk-21"
export JAVA_TOOL_OPTIONS='-Djdk.net.unixdomain.tmpdir=C:\Users\geekerwan\.gradle\uds'
export ANDROID_HOME="C:/Users/geekerwan/AppData/Local/Android/Sdk"
"/c/Users/geekerwan/.gradle/wrapper/dists/gradle-8.13-bin/5xuhj0ry160q40clulazy9h7d/gradle-8.13/bin/gradle.bat" \
  --no-daemon -p android-plugin :app:assembleTraceRelease \
  -Pmobilegl.buildDisaggregated=ON -Pmobilegl.buildDisaggregatedInproc=ON -Pmobilegl.pipePush=ON \
  -Pmobilegl.apkSuffix=p6gate8 -Pmobilegl.debuggableRelease=true \
  -Pmobilegl.applicationIdSuffix=.p6gate8 -Pmobilegl.logLevel=MOBILEGL_LOG_LEVEL_INFO
# BUILD SUCCESSFUL in 2m 33s (cold, 53 tasks executed)

apksigner sign --ks ~/.android/debug.keystore --ks-pass pass:android \
  --ks-key-alias androiddebugkey --key-pass pass:android --out signed.apk unsigned.apk
```

The source under test is the tree's own fix; `grep -c "PipeStats::Init()"
MobileGL/MG_Remote/Server/ServerMain.cpp` = 2 (init + the comment), and the call sits **after**
`MG_ConfigLoader::Init()` (so `Features.PipeStats` is already parsed and the latch reads the real
value) and **after** `MGPipeSetServerProcessRole(true)` (so the log sink files the line as the
server's).

### 1.2 The run

```bash
cd /c/Users/geekerwan/p6-snap2/MobileGL-disagg
python tools/device_bench/p6/ab_session.py \
  --tree "C:/Users/geekerwan/p6-snap2/MobileGL-disagg" \
  --out  "C:/Users/geekerwan/p6-snap2/evidence" \
  --apk  'C:\Users\geekerwan\p6-snap2\artifacts\trace-p6gate8-debugsigned.apk' \
  --package top.mobilegl.plugin.p6gate8.trace \
  --reboot --fan-level 2 --repeats 2 --session gate8-doorbell \
  --arm inproc:DirectGLES:minecraft-1.21.4-rd12-odinlite-in-world:2 \
  --arm spawn:DirectGLES:minecraft-1.21.4-rd12-odinlite-in-world:2 \
  --arm spawn:DirectGLES:minecraft-1.21.4-rd12-odinlite-in-world:2 \
  --arm inproc:DirectGLES:minecraft-1.21.4-rd12-odinlite-in-world:2

python tools/device_bench/p6/render_ab.py <session>/session-summary.json
```

Evidence root: `C:/Users/geekerwan/p6-snap2/evidence/gate8-doorbell/`. Four arm directories, each
with `mobilegl.client.log` **and** `mobilegl.server.log`, `pin-before.txt`, `pin-after.txt`,
`benchmark-run{1,2}.json`, `arm-summary.json`; plus `session-summary.json`.

---

## 2. Arm proof (contract §9.5)

| arm | transport marker | `spawn ARMED - …pid N` | server log pulled | verdict |
|---|---|---|---|---|
| inproc (a) | `MOBILEGL_TRANSPORT=inproc` | n/a | yes | OK |
| spawn (a) | `MOBILEGL_TRANSPORT=spawn` | `pid 18331` | yes | OK |
| spawn (b) | `MOBILEGL_TRANSPORT=spawn` | `pid 21400` | yes | OK |
| inproc (b) | `MOBILEGL_TRANSPORT=inproc` | n/a | yes | OK |

```
[Android MobileGLTraceRe/INFO]: MG_Remote client: spawn ARMED - the server role runs in pid 18331,
  reached at "@mgl-13833-12970367463263226992"
[Android MobileGLTraceRe/INFO]: MG_Remote client: spawn ARMED - the server role runs in pid 21400,
  reached at "@mgl-17377-12970367463263231936"
```

The per-thread CPU sampler corroborates the same topology independently — this is what "two
processes" means on the device, read straight out of `/proc/<pid>/task/*`:

```
inproc, repeat 1:  pid 12188  tid 13614  client-gl  MobileGLTraceRe   8990 ms
                   pid 12188  tid 13618  apply      mgl-srv-apply     8590 ms   <- SAME pid
spawn,  repeat 1:  pid 13831  tid 17165  client-gl  MobileGLTraceRe   9000 ms
                   pid 17166  tid 17195  apply      mgl-srv-apply     8060 ms   <- different pid
spawn,  repeat 2:  pid 13833  tid 18329  client-gl  MobileGLTraceRe   8920 ms
                   pid 18331  tid 18342  apply      mgl-srv-apply     8570 ms   <- == the armed pid
```

`Config: IPC ring=8MiB stage=32MiB spin=50us … verb-barrier=1 run-ahead=1 present-credit=1 strict=1
role-split-state=1` present on every arm. Zero `Fatal{` in all 8 role logs.

---

## 3. The doorbell ledger — the complete table

The four counters are **RUN TOTALS**, so the rates divide by the run's `frames=`, never by the
`window=` on the same line. Two structural facts decide *which log* each pair comes from, and both
were confirmed by this session rather than assumed:

- **`srv`/`srvpark`** are published by the apply thread into **its own process's** PipeStats. Under
  `inproc` both roles are threads of one process, so the client's line carries them; under `spawn`
  they are in `mobilegl.server.log` and the client's line prints `srv=0 srvpark=0` — a zero that
  means "another process", not "never waited".
- **`frames=`** is bumped by `PipeStats::OnPresent`, which the backend calls on Present. Under
  `spawn` present is applied **in the server process**, so the *client's* `frames=0 window=0` — the
  same shape of structural zero. The rate denominators below therefore use the client's count under
  `inproc` and the server's under `spawn`, and the column says which.

| arm | transport | counters from | frames from | `cli frames=` | `srv frames=` | srv | srvpark | cli | clipark | **srv/f** | **srvpark/f** | **cli/f** | **clipark/f** |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 01 | inproc | client-log | client-log | 251 | 240 | 2 869 874 | 1 341 | 30 407 | 1 444 | **11433.8** | **5.343** | **121.1** | **5.753** |
| 02 | spawn | client(cli)+server(srv) | server-log | 0 | 251 | 2 614 177 | 1 229 | 30 401 | 1 524 | **10415.0** | **4.896** | **121.1** | **6.072** |
| 03 | spawn | client(cli)+server(srv) | server-log | 0 | 251 | 2 614 456 | 1 170 | 30 401 | 1 525 | **10416.2** | **4.661** | **121.1** | **6.076** |
| 04 | inproc | client-log | client-log | 251 | 240 | 3 002 771 | 1 265 | 30 407 | 1 406 | **11963.2** | **5.040** | **121.1** | **5.602** |

**Wait, and a reading of each column.**

- **`srv/f` — the server's rendezvous rate.** `inproc` 11 434 / 11 963; `spawn` 10 415 / 10 416.
  That is **7.1 per draw** on both spawn arms against **7.8 / 8.1** on the inproc arms
  (1471.36 draws/frame). The spawn arm is **8.9–12.9 % lower**, which is the one directional
  difference in this table; §3.1 says why it should not be read as a transport win.
- **`srvpark/f` — how often that rendezvous actually blocked.** 5.34 / 5.04 (`inproc`) against
  4.90 / 4.66 (`spawn`). **0.042–0.047 % of server waits park** — the spin budget covers essentially
  all of them on both transports. This is the P5d/P5e characterisation holding: the cost is the
  rendezvous, not the wakeup.
- **`cli/f` — the client's own waits.** **121.1 on all four arms**, identical to a tenth. The client
  does the same work either way; the transport does not change *how often* it waits.
- **`clipark/f` — the client's parks.** 5.60 / 5.75 (`inproc`) against 6.07 / 6.08 (`spawn`): the
  spawn arm parks **5.5–8.5 % more often**. **4.6–5.0 % of client waits park** — two orders of
  magnitude more than the server side, because the client's spin budget is the one being spent
  against a producer that is frequently not yet there.

### 3.1 Why `srv/f` is not a like-for-like comparison

The `inproc` server's wait count and the `spawn` server's wait count are **not counting the same
event**, so the 11 % difference must not be read as "the socket rendezvous happens less often".

Under `inproc` the apply thread's `Doorbell::Wait` is *also* the mechanism by which the client's
queue drain is published inside one address space, and the client is in lockstep behind a verb
barrier for a large part of the trace; under `spawn` the same loop is woken by descriptor readiness.
Both counters count entries into `Doorbell::Wait`, but the number of times the two roles have to
agree per frame is a property of the transport's handoff discipline, not of the doorbell's cost.
What is comparable between the two columns is the *park ratio*, and that is near-identical
(0.042–0.047 % both ways).

The honest form of the item-① answer is therefore: **the server does ~10 400–12 000 doorbell waits
per frame on both transports, ~0.045 % of which block, and replacing the shared-cache-line spin with
a socket read does not change the client's per-frame CPU (below).**

---

## 4. Client CPU per frame — the tie holds

Primary metric = the client GL thread's own `CLOCK_THREAD_CPUTIME_ID` deltas, summarised by the
device over the trailing 200 frames (`MEASUREMENTS.md` §9's rule).

| arm | transport | frames | tail | wall p50 ms | wall p95 ms | **client CPU p50 ms** | CPU p95 ms | fps |
|---|---|---|---|---|---|---|---|---|
| 01 | inproc | 251 | 200 | 8.105 | 18.474 | **7.987** | 18.044 | 105.72 |
| 02 | spawn | 251 | 200 | 8.116 | 18.586 | **7.992** | 17.928 | 104.43 |
| 03 | spawn | 251 | 200 | 8.113 | 18.003 | **8.009** | 17.483 | 104.19 |
| 04 | inproc | 251 | 200 | 8.089 | 19.295 | **7.989** | 18.030 | 103.72 |

Per-repeat (all 8 runs, client CPU p50 ms):

| arm | rep 1 | rep 2 | spread |
|---|---|---|---|
| inproc (a) | 7.976 | 7.987 | 0.14 % |
| spawn (a) | 8.019 | 7.992 | 0.34 % |
| spawn (b) | 8.030 | 8.009 | 0.26 % |
| inproc (b) | 7.989 | 8.044 | 0.69 % |

**Pair delta: inproc 7.9880 (mean of 7.987/7.989) vs spawn 8.0005 (mean of 7.992/8.009) = +0.0125 ms,
+0.16 %.** The arms' own run-to-run spread is 0.14–0.69 %, i.e. the same order as the difference.
**This reproduces the previous session's tie** (there: inproc 7.880/7.889 vs spawn 7.876/7.869) on a
different build and a different boot: the two transports are indistinguishable on the metric that
matters, and what moved between the two sessions is the absolute level (~0.1 ms), which is
run-to-run drift, not a transport effect.

---

## 5. Log excerpts, verbatim (with `frames=` / `window=`)

Full-window values are elided to `…`; the `wait[...]` bracket and the frame/window fields are
original.

### 5.1 `inproc` arm (a)

```
client: MGPipe stats: frames=251 window=11 draws=16185 draws/f=1471.36 acc=65496 acc/draw=4.05
        bytes/f[buf=10289.45 tex=342341.82 ubog=281585.45 … resid=161.45 csob-blob=281568.00 …]
        tex[emit=6 box=6 rect=0 jobs=6] cso[csom=0 csob=208 mpr=0]
        emit[fbe=68 sve=143 sse=0 sie=0 ctu=10 trp=0 rsp=0 vbs=16262 wrec=82523 wrec/f=7502.09
             maxrec=1808 maxcap=4194304 ringwraps=61 ringpads=53 ringwaits=0]
        wait[srv=2869874 srvpark=1341 cli=30407 clipark=1444]

server: MGPipe stats: frames=120 window=120 draws=1156748 draws/f=9639.57 …
        wait[srv=2437659 srvpark=1227 cli=28409 clipark=1355]
        MGPipe stats: frames=240 window=120 draws=176565 draws/f=1471.38 …
        wait[srv=2829654 srvpark=1315 cli=30239 clipark=1431]
```

Both pairs are in the **client** log, because both roles are one process. The server log's last line
is at its previous period boundary (`frames=240`) — the apply thread does not emit the session-final
line; that is why the frame count for this arm comes from the client's `frames=251`.

### 5.2 `spawn` arm (a)

```
client: MGPipe stats: frames=0 window=0 draws=0 draws/f=n/a acc=0 acc/draw=n/a
        bytes[buf=0 tex=0 … resid=174584 csob-blob=257348207 seg=2698318987]
        tex[emit=0 box=0 rect=0 jobs=0] cso[csom=27 csob=20330 mpr=0]
        emit[fbe=9142 sve=20821 sse=3 sie=0 ctu=1644 trp=0 rsp=0 vbs=0 wrec=6929844 wrec/f=6929844
             maxrec=1808 maxcap=4194304 ringwraps=61 ringpads=52 ringwaits=0]
        wait[srv=0 srvpark=0 cli=30401 clipark=1524]

server: MGPipe stats: frames=120 window=120 draws=1156748 draws/f=9639.57 …
        wait[srv=2219341 srvpark=1137 cli=0 clipark=0]
        MGPipe stats: frames=240 window=120 draws=176565 draws/f=1471.38 …
        wait[srv=2582289 srvpark=1222 cli=0 clipark=0]
        MGPipe stats: frames=251 window=11 draws=16185 draws/f=1471.36 …
        wait[srv=2614177 srvpark=1229 cli=0 clipark=0]
```

**The two structural zeros and why neither is a measurement.** The client's `frames=0 window=0` says
this process counted no frames — it never presents, present is applied in the server. Its
`srv=0 srvpark=0` says the server's counters live in another process. Symmetrically the server's
`cli=0 clipark=0` is the client's own producer, which is not in that process. Reading any of the
three as "zero waits" is the mistake this table exists to prevent.

Also visible: the spawn **client** does count the byte and record classes (`resid`, `csob-blob`,
`wrec=6929844`, `ringwraps=61`), because those are published by the emitter that runs in the client
process; the spawn **server** reports `0` for them because the server publishes only what its own
role produces. So gate 8 item ② (`SEG_STAGE` bytes/frame) is readable on the client under both
transports; item ③ (records/frame) likewise (`wrec/f`), with the caveat that `wrec` is a client-side
emission count rather than the server's applied count.

---

## 6. The fix this session verifies

`MG_Remote/Server/ServerMain.cpp`, step 1.6:

```
MobileGL::MG_Util::PipeStats::Init();          // after MG_ConfigLoader::Init and
                                               // MGPipeSetServerProcessRole(true)
```

plus the matching `PipeStats::Shutdown()` before `_exit(0)` (after `loop.Stop()`, so the apply
thread is not still publishing, and before `_exit`, which runs no atexit handler).

**Before/after on this protocol:**

| | previous session (`gate7-device-ab.md`) | this session |
|---|---|---|
| spawn server summary lines in `mobilegl.server.log` | **0** | **3** (frames=120, 240, 251) |
| `wait[]` in the spawn server log | absent | `srv=2614177 srvpark=1229` |
| gate 8 item ① server half | "structurally missing" | measured |

The client-side numbers are unchanged by the fix (`cli=30401 clipark=1524` here vs `30401 / 1538` in
the previous session), which is the expected shape: the fix arms counters in the server process and
touches nothing the client does.

**One thing it does not fix, reported rather than papered over:** the spawn **server** log's last
summary line lands at the session-final `frames=251 window=11` (the `Shutdown` call), but the spawn
**client** log's only line is the `frames=0 window=0` one — the client has no frame boundary of its
own to key a period on, so it emits exactly one line, at teardown. Downstream readers that index
"the last line" per role therefore get a well-formed line from each, but they must not assume the
two are the same window. The `frames from` column in §3 is what makes that unambiguous.

---

## 7. Judgement

**Gate 8 item ① — the measuring gap is closed.** Both halves of the doorbell ledger are on the
device, from both roles' own logs, on both transports: server waits **10 415–11 963 per frame**
(0.042–0.047 % parking) and client waits **121.1 per frame** on all four arms (4.6–5.0 % parking).

**On the contract's §9 note.** That note predicted a socket blocking read might be *cheaper* than
`inproc`'s ~1600 spins/frame on a shared cache line. The measurement says **neither cheaper nor more
expensive**: client CPU per frame is 7.992/8.009 (`spawn`) against 7.987/7.989 (`inproc`) — a +0.16 %
difference well inside the 0.14–0.69 % run-to-run spread, reproducing the previous session's tie at a
different absolute level. The server's own wait *count* is 8.9–12.9 % lower under `spawn`, but §3.1
explains why that is a property of the handoff discipline rather than of the doorbell, and the
comparable quantity (park ratio) is the same on both.

Stated plainly: **on this workload, at this resolution, the doorbell's implementation — futex-backed
condvar with a spin budget versus a one-byte socket write — is not a measurable cost.** Both are
~30 % cheaper than `monolith` on client CPU per frame. Per the campaign's standing rule these
numbers are recorded, not gated.

**Caveats, not hidden.** The exercise was one workload (rd12 in-world, DirectGLES) and one device;
the reference note's "1600 spins/frame" figure was for a heavier scene, so a workload with more
rendezvous per frame is where a difference would have to show up if it exists at all. The
`spawn` arm's `clipark/f` is 5.5–8.5 % higher than `inproc`'s (6.07 vs 5.60/5.75) — the only column
where the socket looks marginally worse, and it is ~0.4 extra parking events per frame out of 121.
