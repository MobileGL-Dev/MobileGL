# inproc vs monolith at `7c6f6886` — measured, and why

Redmi `2f7cbe2e`, FCL fordebug + Minecraft 26.3-rc-3, world `test`, VD12, DirectGLES, ~848 draws per
frame. GPU pinned (`min_pwrlevel = max_pwrlevel = 0`, re-asserted per arm because the pin is lost
across app restarts), fan on, 30 s window taken 20 s after the world settles. Arms interleaved so
thermal drift shows in both. Harness: `~/w7/notes/tools/p5d_bench_fcl4.sh` (P5d's, unchanged).

## 1 The answer: yes, inproc is still slower

| arm | fps (window) | fps p50 | client GL thread ms/frame | apply thread ms/frame |
|---|---|---|---|---|
| mono1 | 254.9 | 267.9 | 3.898 | — |
| inproc1 | 161.7 | 166.4 | 6.098 | 5.305 |
| mono2 | 201.3 | 195.1 | 4.781 | — |
| inproc2 | 141.8 | 143.5 | 6.958 | 5.653 |
| mono3 | 192.4 | 254.2 | 4.694 | — |

Thermal drift is real (monolith's own GL thread moved 3.898 → 4.781 across the session), so read the
PAIRED ratios, not the absolute numbers:

- **fps**: inproc is **0.62 – 0.74×** monolith.
- **client thread CPU per frame**: inproc's client is **1.46 – 1.56×** monolith's single thread.
- **total CPU per frame**: inproc 11.4 – 12.6 ms across two threads, monolith 3.9 – 4.8 ms on one.
  The split arm spends roughly **2.6×** the total CPU.

Monolith is cleanly single-thread CPU-bound: its GL thread reports **993.7 ms of CPU per second of
wall clock**, i.e. 99.4% of one core. inproc is bound by its client thread, which is also ~99%.

**So parity means `max(client, apply) <= monolith's single-thread ms/frame`** — about 3.9 – 4.7 ms.
It does NOT require the split arm to spend less total CPU than monolith; it requires neither thread
to exceed what monolith's one thread spends. That is a question of balance and overhead.

## 2 Where the extra CPU goes: a rendezvous per draw

The renderer's own stats line counts entries into `Doorbell::Wait` as `srv` / `cli` and actual
futex parks as `srvpark` / `clipark` (`MG_Util/Metrics/PipeStats.cpp:537`). A 60 s inproc sample
(8,280 frames):

| counter | total | per frame |
|---|---|---|
| `cli` (client enters the wait) | 7,588,093 | **916** |
| `clipark` (client actually parks) | 13,025 | 1.6 |
| `srv` | 15,836,497 | 1,913 |
| `srvpark` | 37,456 | 4.5 |
| `rsp` (residual pulls) | 102,408 per 120-frame window | **853, i.e. one per draw** |

Against ~848 draws per frame, the client blocks **about once per draw** and 99.8% of those blocks
resolve by spinning rather than parking. The monolith arm reports `rsp=0` and
`wait[srv=0 srvpark=0 cli=0 clipark=0]` — it has no rendezvous at all.

`rsp` equalling the draw count is the same fact the strict lane reports as
`GetProgramForDraw@DrawArrays`: one barrier-pulled field read per draw. A record that reads client
memory cannot be unbarriered, so the draw path stays in lockstep and the client waits.

## 3 The spin is the cheap version of the rendezvous, not the problem

Control arm `MOBILEGL_IPC_SPIN_US=0` (park immediately instead of spinning up to 50 µs):

| arm | fps p50 | client ms/frame | apply ms/frame | `clipark` / `cli` |
|---|---|---|---|---|
| inproc (default, 50 µs) | 143.5 – 166.4 | 6.098 – 6.958 | 5.305 – 5.653 | 0.2% |
| inproc `SPIN_US=0` | **20.9** | **20.167** | **14.681** | **99.6%** |

Making every rendezvous a futex park costs about 14 ms/frame more on the client and collapses the
frame rate by 7×. So the 50 µs spin is not waste to be tuned away — it is what keeps the current
design viable. **The cost is the rendezvous itself, and the fix is to stop having one per draw, not
to make each one cheaper.** Tuning `SPIN_US` is a dead end in both directions.

## 4 The barrier-off control cannot answer the performance question

`MOBILEGL_IPC_VERB_BARRIER=0` aborts a few seconds into the run:

```
F/MobileGL: MGPipe: Fatal{UnmigratedPipeInput, "IsCapabilityEnabled@ClientWaitSync"}
```

which is exactly what `Config.h` documents it to do ("EXPECTED to be red, because 31 of the 63
PipeInputs fields are still pulled from a live GLContext by the client's residual fill"). It is a
correctness control and gives no usable CPU number. The ceiling that run-ahead buys therefore
cannot be measured before run-ahead lands; §5 is an estimate, and it is labelled as one.

## 5 What run-ahead should buy, and what it will not

Let `F` be the client's frontend work, `B` the backend GL work, `E` the record encode, `D` the
decode plus resolve, and `S` the rendezvous cost on each side. Then

- monolith ≈ `F + B` = 3.9 – 4.8 ms/frame (measured)
- inproc client = `F + E + S_c` = 6.1 – 7.0 ms/frame (measured)
- inproc apply = `D + B + S_a` = 5.3 – 5.7 ms/frame (measured)

so `E + D + S_c + S_a` ≈ **7 ms/frame**. Run-ahead removes `S_c` and `S_a` for unbarriered records
and nothing else: **the encode and decode stay**. The acceptance signal is therefore not fps first
but the counters:

- `rsp` must go to **0** on unbarriered records (BRIEF §3.3 already pins this),
- `cli` must fall from ~916 per frame to the handful of records that stay barriered by ruling
  ID-84 (readback, XFB, CopyTex, `set_storage_block_binding`), which in this workload is a few per
  frame, not hundreds.

If after that the arm is still short of parity, the remaining gap is `E` and `D` and it is an
encode/decode problem, to be attributed by symbolized call graph and not by self-percent — the trap
P5d recorded after mis-diagnosing an idle poll as backend lock contention.

## 6 Measurement discipline for the next round

- Interleave arms in one session; report the paired ratio. A single-arm fps number drifted 27% in
  this very session.
- Per-thread CPU ms/frame is the primary metric, fps the secondary — fps alone confounds vsync
  capping (this panel is 120 Hz and a capped monolith run reads 115) and thermal state.
- Re-assert the GPU pin per arm; it is lost across app restarts.
- Confirm the library under test by symbol probe on the APK's `lib/arm64-v8a/libMobileGL.so`, never
  by the in-game `GIT@` stamp, which is stale under incremental builds.
