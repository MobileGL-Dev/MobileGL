# P15 handoff (living document)

The plan and its measurements are in [PLAN-P15.md](PLAN-P15.md); the audit is in
[PARITY-AUDIT.md](PARITY-AUDIT.md).

**Target** (PLAN §0.1, revised by the user 2026-10-08):
- CPU scenes: FCL presenting-thread CPU at or below dev monolith (Magma <= 1.79, Espryt <= 2.14
  ms/frame), dev measured in the same session, ~5 % noise.
- GPU scene (Iris + BSL): fps within ~5 % of MobileGlues on both backends.

## Latest (2026-10-08 late): harness H2, dev reference, scoreboard

- **Every FCL vanilla number now goes in [SCOREBOARD.md](SCOREBOARD.md).** It has one row per
  session, as fps ratio to MobileGlues plus CPU ms/frame. It also holds the fixed dev reference, so
  "vs dev" no longer needs dev in every session. The per-function dev baseline is in
  [MONOLITH-DIFF.md](MONOLITH-DIFF.md).
- **The old probe's variance was core placement.** The scheduler gave MobileGL's monolith render
  thread the X4 core 72-90 % of the time and MobileGlues' 47-60 %. MobileGlues' fps tracked its X4
  share (fifi: 60 % -> 378, 47 % -> 344).
- **Harness H2 (scratchpad `p15/fcl_bench.sh`, `fclab2.sh`, `benchwin.py`):** the same for every
  arm.
  - After world entry, `taskset -a -p 7c` pins FCL to cpu2-6 (A720).
  - fps comes from the SurfaceView BLAST layer `frame=` in `dumpsys SurfaceFlinger`. It agrees
    with the MobileGL fps log over the same span (252.1 vs ~253).
  - Warmup: 20 s, then 5 s windows until two consecutive ones agree within 3 %.
  - Measurement: two 5 s windows with no tracer; CPU from schedstat.
  - atrace, then the optional simpleperf (`sp=1`), run only after the windows.
- **Frozen bench saves** (1.21.5 and 1.21.4-Fabric, scratchpad `p15/freeze_world.py`):
  - Rules: no daylight, weather or mob-spawn cycle, randomTickSpeed 0, no fire tick.
  - DayTime 6000, clear weather.
  - Before this, the vanilla save's day cycle was running.
  - The originals are on the device as `level.dat.p15-orig`.
- **Spread over 3 interleaved reps, (max - min) / mean:**
  - Old probe: Espryt 9.3 %, Magma 5.6 %, MobileGlues 1.3 %; MobileGlues spanned 322-376 across
    sessions.
  - H2: Espryt 1.2 %, Magma 2.9 %, MobileGlues 2.0 %, dev 6-7 %.
- **Dev reference (H2, safter, all VALID):** MobileGlues 321.5 fps / 1.70 ms. dev Magma is 0.950
  of that (2.01 ms) and dev Espryt 0.887 (2.38 ms).
- **On A720 the monolith gap is larger than the X4 hid:** p15s Magma is 0.768 of MobileGlues (0.81
  of dev, 2.91 ms) and Espryt 0.746 (0.84 of dev, 3.18 ms). That is +0.90 / +0.80 ms/frame of CPU
  over dev.
- **Inproc p15s Magma on H2:** 327.7 fps against MobileGlues ~321.
- **Emulated TLS:** `__emutls_get_address` costs ~0.06 ms/frame of self time on the p15 monolith
  render thread, spread over many callers. dev shows almost none. The structural fix is native TLS
  (minSdk), which is the user's call.

## Earlier (2026-10-08, after S0)

Every number below carries its profile and passed the validity check (`state.txt`). `cpuhunt` =
CPU about 1.5/1.25 GHz, GPU 903 MHz (cleanly CPU-bound); interval 0, anland off, daemons stopped.

- **The S0 milestone's absolute numbers are superseded.** The perf stack's `msm_performance` caps
  (787 / 1286 / 1075 / 1248 MHz) were clamping the CPU under the pin for some of that session. Only
  same-session deltas from it stand. Measurement now uses `bench_session.sh` and the sampler (PLAN
  §1), and the probe reports only clean fps windows taken before any tracing (`P15PROBE
  trace-start` marker, 25 s settle).
- **FCL vanilla 1.21.5, cpuhunt, presenting-thread CPU ms/frame:**

  | build | Espryt | Magma |
  |---|---|---|
  | dev | 1.76 | 1.60 |
  | p15k (S0 + VAO shadow) | 2.10 | 2.03 |
  | p15l (+ vertex-elements dedup) | 2.10 | 2.08 |

  MobileGlues: 1.58 in an earlier cpuhunt session (needs a same-session re-run with clean windows).
  Floor gap: Espryt +0.34, Magma +0.43 ms.
- **Driver-call redundancy:**
  - Measured with a diagnostic build that counts every host GLES/EGL call and redundant re-sets
    (`p15diag` worktree, never committed; HostCount.inl and VkCount.cpp).
  - Espryt sent 3717 host calls/frame against MC's ~874: 2118 `glDisableVertexAttribArray`, 378
    enable, 378 divisor, all re-sets.
  - Two fixes, each independently removes the walk churn:
    - b4e209e1: a shadow of each driver VAO (850 calls/frame);
    - c8a5abe5: the client no longer re-creates an identical vertex-elements record on every
      `glBindVertexBuffer`. With the shadow off it takes 3724 down to 1228.
  - CPU gain is small (about 0.1 ms): the calls were cheap in the driver.
  - Magma issues about 380 Vulkan calls/frame with essentially none redundant.
- **Where Espryt's gap to dev is now:** the driver time equals dev's (0.50 vs 0.48). The excess is
  libMobileGL itself, 0.75 vs 0.32: client half 0.51, backend 0.24, applier 0.02. The client half
  is spread thin (validate, tracker, emitters, residual copy): S1's draw-delta.
- **Inproc (cpuhunt):**
  - Espryt reaches 306 fps vs MobileGlues about 319; GL thread 1.68 ms, apply thread 1.83 ms of a
    3.27 ms frame.
  - Neither thread is saturated. The apply thread waits in `queueBuffer` on a GPU fence
    (present pacing), and the GL thread waits on the apply thread.
  - `MOBILEGL_IPC_PRESENT_CREDIT=2` changes nothing (300 fps).
  - Beating MobileGlues on inproc needs a present-path change (stage P territory, dropped by the
    user), not CPU work.
- **S0b:** the fixture `minecraft-1.21.5-vanilla-fcl-in-world` is landed (4e27342e) and is in the
  SSIM gate as `fcl` (32/32 on p15j).
- **Knobs on FCL:** `adb shell setprop debug.mobilegl.env "MOBILEGL_A=1;MOBILEGL_B=0"` (49c6394d);
  `fclab.sh` takes a 6th field `K=V,K=V`.
- **MC's AFK frame limit:** `inactivityFpsLimit:"afk"` (MC 1.21.2+) made later probe windows and
  profiles spin in `System.nanoTime`. `fcl_scene.sh` now sets `"minimized"` for bench scenes and
  restores the original options files (`/data/local/tmp/p15-opts-121{5,4}.txt`). Profiles taken
  before this are partly AFK-polluted.
- **BSL at `gpuhunt`** (CPU 2380/1689 MHz, GPU 578 MHz, all 100 % GPU busy):
  - MobileGlues 53.4 fps;
  - dev Magma 53.2;
  - p15l Magma 51.8 (3 reps, -3.0 % vs MobileGlues: target met, -2.6 % vs dev to bisect);
  - Espryt dev and p15l 48.3 (-9.6 %): the GPU gap, inherited from dev.
- **Same-session cpuhunt, apply thread:**

  | build | fps | apply ms/frame |
  |---|---|---|
  | MobileGlues | 329 | CPU 1.55 |
  | Magma inproc p15m | 300 | 1.6 |
  | Espryt inproc | 296 | 1.65-1.8 |

  - Inproc beats MobileGlues only when the apply thread's CPU is below MobileGlues' (both pay the
    same queueBuffer wait). About 0.3 ms per frame beyond the apply CPU is still unexplained.
  - 500b63ea: per-store reuse of renamed-away wire buffers (Magma apply -0.13 ms).
  - ONE_TIME_SUBMIT: no effect (A/B'd, knob dropped).
  - Magma's driver work is about 2x dev's: vkResetCommandBuffer 0.09 + vkEndCommandBuffer 0.11
    ms/frame.
- **Inproc serialization found (2026-10-08, p15l cpuhunt profiles):** every blit reached the
  client as `BlitNamedFramebuffer`, a verb with no `MGP_VERB_OP_LIST` row, so
  `ClientVerbIsBarriered` answered barriered and the GL thread quiesced the applier once per frame
  (1.65 ms/frame parked in `WaitForApplyToCatchUp`, plus 0.11 from the following `glBindTexture`).
  The apply thread then idled 0.24 ms parked plus 0.2 ms spinning in `Doorbell::Wait`. e00db8e6
  replaces the inverse and its barriered fallback with a verb -> record-op table
  (`MG_Pipe/VerbRecordOps.h`, one row per verb by static_assert; no knob, user decision).
  - Same-session blab (cpuhunt): Magma inproc 319.6 -> 334.9 fps (+4.8 %), Espryt inproc
    324.4 -> 338.1 (+4.2 %).
  - Wait census (`MOBILEGL_IPC_WAIT_STATS=1`, 7acf3ea8): FCL vanilla inproc waits only on the
    present credit (1.00/frame). BSL waits 7 times per frame on three kWaitApplied rows plus 22
    follow-on quiesces; see [WAIT-AUDIT.md](WAIT-AUDIT.md). User decision applied in 2fbcc224:
    fire-and-forget rows async, inproc creates and PBO readbacks no longer wait, and the dual block
    (`MOBILEGL_IPC_ROLE_SPLIT_STATE`, now default on) lets a barriered verb cost one wait. BSL
    census after it still owed (gpuhunt only; FCL's 1.21.4-Fabric profile exited with signal 34 at
    JVM start on both backends in the last attempt).
  - db915f76: a client parked on the present credit is rung only by the credit (apply-thread
    futex wakes 0.056 ms/frame before).
  - Frames in flight (0b96a3ef): `MOBILEGL_FRAMES_IN_FLIGHT` (default 3) drives Magma's frame
    contexts, Espryt's post-present fence wait and the split present credit max(1, N-2); the Magma
    name is an alias. Default latency unchanged; the FIF A/B is the user's input for the default.
  - Present credit 2 (cens, cpuhunt, all VALID): Magma 309.0 -> 319.9, Espryt 299.1 -> 319.1 fps
    vs MobileGlues 321.9 in the same session; apply CPU 1.40-1.43 vs MobileGlues' GL thread 1.57.
    Credit 2 costs a frame of latency: user decision. The remaining inproc gap is the queueBuffer
    fence wait (GPU-side), not CPU.
  - BSL at cpuhunt (GPU 903 MHz) reaches GPU 95 C and kgsl thermal_pwrlevel 1 (834 MHz cap): the
    census run was INVALID twice and the job was stopped. Run BSL only at gpuhunt.
  - Apply-thread frame (Magma inproc): on-CPU 1.75 (0.2 of it spin), queueBuffer fence wait 1.45,
    parked 0.24 ms.
  - Magma apply on-CPU by verb: draws 0.55 (SetupWireDraw 0.40, of it descriptor binding 0.13),
    present 0.37 (End CB 0.10, queueBuffer CPU 0.08, submit 0.07, wire-store sweep 0.035),
    clear 0.19 (the frame's recording begin 0.09 + pass begin 0.05), buffer writes 0.09, blit
    0.06 (two image views and a VkFramebuffer created and retired per blit).
- **Device contention, root cause (2026-10-08):** the anland app was relaunched by `fclab.sh`'s own
  end-of-run `monkey` (launchedFromUid=0); its activity takes the foreground back whenever paused,
  so FCL launched over it never reaches the world (RenderDoc job: 0 of 4 worlds). c41dfdda:
  `bench_session.sh start` force-stops it and `stop` relaunches it only if it ran before.
- **Open user decision:** Espryt's apply thread spends 0.06 ms/frame in `LatchIfGuiltyBeforeApply`
  (glGetGraphicsResetStatus is a kgsl ioctl, queried on every wake). Rate-limiting it to 2 ms
  breaks `ServerLoopEglLatchTest.AGuiltyResetEndsTheSessionBeforeItsNextRecord...`, the designed
  promptness guarantee, so it was reverted. Trading that guarantee for 0.06 ms is the user's call.
- **CI:** 4a8ecf9a green. 49c6394d: one job failed on a hosted runner's `apt-get update` timeout
  (45 min, infrastructure); the failed job is re-run.
- **Next:**
  - RenderDoc (rdc GLES layer, remote server stopped): Espryt vs MobileGlues BSL pass structure;
  - S1 (client validate 0.25 ms) and S3 (backend by-handle syncs, descriptor resolve) for the CPU
    floor and the inproc apply thread.

## Status (2026-10-08)

**Branch:** `p15impl` (worktree `.claude/worktrees/p15`, short path `W:/p15`), pushed to
`feat/disaggregated`.

**S0.1 parity audit:** written.

**S0.2, Magma wire non-draw ports:**
- **M1a, clears become part of the draw pass** (`ClearWireFramebufferInPass`; `PrepareWireDrawPass`
  split out of `SetupWireDraw`):
  - A whole-area clear that begins the pass begins it with a LOAD_OP_CLEAR variant of the pass's
    render pass (`RenderPassEntry::clearVariants`, compatible with the framebuffer).
  - Any other clear (into the open pass, or scissored) is `vkCmdClearAttachments` in the pass. A
    forced pass restart for depth clears (an LRZ hypothesis) was tried and reverted: it cost CPU
    and fixed nothing.
  - Kill switches: `MOBILEGL_MAGMA_INPASS_CLEAR=0`; `MOBILEGL_MAGMA_INPASS_CLEAR_DEFAULT=1` puts
    default-framebuffer clears in the pass too (off by default, see the open item).
- **Open item: default-framebuffer clears stay standalone.** With them in the pass, the openra
  retrace on Adreno 750 loses its palette-sampled sprites (SSIM 0.956378, deterministic). What was
  measured:
  - Both `vkCmdClearAttachments` and LOAD_OP_CLEAR fail; so does splitting the clear calls.
  - Colour-only or depth-only in the pass, with the other part standalone, passes.
  - Event traces: the first draw flushes the recording (the palette `glTexImage2D`'s upload flush,
    `FlushPendingCommands`) right after the clear. Command order otherwise matches the standalone
    path.
  - Not explained yet. Lavapipe and the InPassClear scenarios on FBOs are clean.
  - Ruled out by reading: a missing ordering edge. Default-framebuffer images get the same
    pass-begin barrier as FBO images (a full ALL_COMMANDS memory barrier, or their transition),
    with their layout tracked per swapchain index through the swapchain object.
  - Still open: the in-pass path commits the freshly acquired swapchain image
    (`DefaultFramebufferWriteIndex(true)` in `PrepareWireDrawPass`) at the clear; check whether
    the standalone path's clear and the in-pass clear address the same index on the trace route.
  - Cost of leaving it: small for FCL (MC renders into its own FBO and blits to the default one).
- **M2, draw-pass cache across frames:** one cache, entries pruned when their image epochs move,
  destroyed after their last submit. Kill switch: `MOBILEGL_MAGMA_PASS_CACHE=0`.
- **M3, blit descriptor sets:** one pool per frame slot.
- **M9, converted vertex streams** (wrongly marked "have" in the first audit). The wire arm
  re-converted the whole remaining stream on every draw: Sodium's multi-draws over one arena cost
  49 ms/frame of CPU on BSL (19.5 fps against dev's 64.7).
  - Now one conversion per (store content serial, layout), kept across frames in
    `VkBufferManager` (`LookupConvertedStream` / `StoreConvertedStream`).
  - Each entry owns a vertex buffer. Bounded at 192 MB (least recently used first); entries idle
    for 256 frames go. A replaced buffer is released when its frame slot comes round.
  - The content serial is process-wide monotonic and moves on every host write, respecify,
    GPU-write mark, shader-writable bind and donation.
  - Kill switch: `MOBILEGL_MAGMA_VERTEX_CONVERSION_CACHE=0`.
- **M4, write renaming.** A busy `glBufferSubData` used to take a staged copy, which ended the
  render pass with two full barriers (FCL: ~5.6 per frame, all on one 288-byte store). Now:
  - A store of 64 KB or less keeps a host shadow (64 MB budget), from respecify on.
  - A busy write into a shadowed store gives it a fresh VkBuffer filled from the shadow and
    releases the old one like a glBufferData orphan. No copy is recorded and the pass stays open.
  - GPU-written marks, shader-writable binds, donation and import drop the shadow.
  - `ReadWireBuffer` serves shadowed stores from the shadow, so no host wait.
  - Kill switch: `MOBILEGL_MAGMA_WRITE_RENAME=0`.
- **Tests:**
  - `InPassClearScenario`: 4 cases, monolith and split, both backends.
  - `ConvertedVertexStreamScenario`: 3 cases (reuse, same-frame sub-data, cross-frame sub-data).
    Red-once: a lookup that ignores the content serial turns both sub-data cases red.
  - `WriteRenameScenario`: 4 cases (order and carry, one-byte writes, uniform and index buffers,
    multi-frame). Red-once: a rename that skips the shadow fill turns all 4 red, which also
    proves the rename engages.
  - Red-once checks: the colour index and the scissor (2 cases red); the depth clear-load bit
    (depth/stencil case red).
  - Syncval gates green (cache 7/7, run-ahead 14/14), plus the InPassClear cases under syncval.
- **Gates:**
  - SSIM 24/24 (openra, rd12, bsl × 4 arms × 2 backends) on `p15e`.
  - Host unit and integration-gpu green, apart from the known environmental `ServerSpawn` /
    `FuzzArm2` / `PeerLatch` / spawn ColdStart failures.
  - `CompositorRecovery` flakes under -j16 and passes alone 3/3.

**S0 milestone (one device session, FCL, interval 0, 2 reps; `runs/ms-s0i`):**

| scene | build | Magma CPU ms/frame | Magma fps | Espryt CPU ms/frame | Espryt fps | GPU busy |
|---|---|---|---|---|---|---|
| vanilla 1.21.5 (CPU) | fin | 3.27 | 170 | 2.81 | 190 | 12-15 % |
| | p15h (S0 without M4) | 2.59 | 191 | - | - | 15 % |
| | p15i (S0) | 2.56 | 190 | 2.77 | 193 | 13-15 % |
| | dev | 1.79 (n=1) | 234 | 2.14 | 218 | 14 % |
| | MobileGlues | 1.59 | 256 | | | 15 % |
| BSL (GPU) | p15i | 7.26 | 77.2 | 7.61 | 70.6 | 99 % |
| | dev | 13.85 | 66.1 | 7.17 | 70.5 | 85-100 % |
| | MobileGlues | 6.25 | 78.2 | | | 98-100 % |

(BSL before the converted-stream cache: Magma 19.5 fps at 26 % GPU busy, 49 ms CPU/frame.)

- Espryt's `base` lib (p15plan, unchanged) read 2.84 in the same session: the earlier "p15e
  Espryt regression" was session variance. S0 does not touch Espryt.
- **Target status.** Magma BSL is met (within 1.3 % of MobileGlues, GPU-bound). Espryt BSL is
  GPU-bound at 99 % and 10 % below MobileGlues, same as dev: a GPU-side gap still to explain (the
  load/store and clear audit of PLAN §0.1). CPU scene: Magma 2.56 vs MobileGlues 1.59, Espryt 2.77.
- **Where Magma's CPU goes now** (p15i profile, `runs/prof-hi`, ms/frame inclusive):
  `VerbChannel::Port` 0.85, `OnDrawVbo` 0.62, `SetupWireDraw` 0.46, `Present` 0.32,
  `MGPipeValidateForVerb` 0.27, in-pass clears 0.18. The draw path is S1-S3's; S0's non-draw ports
  are done. Proof that M4 engages: `StagedWireRangeCopy` 0.067 is gone and `RenameBusyWireStore`
  costs 0.048 (a VMA allocation per rename; a recycle pool could take most of it).

**Stage P:** dropped for the target.

## Harness

| purpose | where |
|---|---|
| FCL probes and reducers | `tools/device_bench/disagg/` (README "P15 FCL probes") |
| FCL vanilla A/B, H2 (pinned, SF frame source, stability warmup, `sp=1` profiles after the windows) | scratchpad `p15/fclab2.sh "<name>:<lib>:<backend>[:sp[:transport[:K=V,...]]] ..." <reps>` -> `fcl_bench.sh`; `fclsum.py`, `residency.py`, `diffprof.py`, `prof_prep.sh` |
| FCL milestone (CPU + BSL scenes, fin / new / dev / MobileGlues) | scratchpad `p15/fcl_milestone.sh`, `fcl_scene.sh`, `fclab.sh`, `fclsum.py` |
| SSIM gate (openra, rd12, bsl × 4 arms × 2 backends) | scratchpad `p15/ssim_gate.sh`; `matrix_p14.sh` now takes `PREP_WLS` and a `bsl` workload |
| host CI-shaped build (WSL archlinux) | `~/mgl-p15-ci31`, scratchpad `p15/wsl_ci.sh`, `w_suite.sh`, `w_syncval.sh`, `w_sv_one.sh`, `w_one.sh` |
| trace APK builds | scratchpad `build_trace.sh W:/p15 .<tag>` (rerun once on the `IncrementalSplitterRunnable` failure) |

**Host-build trap:** editing a header while ninja is compiling leaves objects newer than the header
but built from its old text. This happened once and gave a `bad_alloc` in every scenario. Never
edit sources during a build; `touch` the edited headers if in doubt.

**Device facts:**
- Clocks: CPU 2035.2 MHz on policies 2/5/7 and 1574.4 MHz on policy0, GPU 680 MHz (new default since
  2026-10-08, pending the soak). Pinned per session by `bench_session.sh`, which every `devjob.sh`
  job runs.
- Frequency daemons are stopped only inside a bench session: `thermal-engine`, `perf2-hal-1-0`,
  `vendor.perfservice`, `performance` and `hyperschedule_hal_service`. Their prior state is in
  `/data/local/tmp/p15-bench-session` while a session is open, and they are restarted and verified
  on exit. If that file exists with no job running, a session was left open: run
  `bench_session.sh stop`.
- The perf stack's game-mode caps in `/sys/kernel/msm_performance/parameters/cpu_max_freq` survive
  its services and silently override the pin. The session releases them.
- FCL's original lib md5 is `00c09f0a…`.
- MC 1.21.5 options md5 is `93b3f708…`; it is backed up at `/data/local/tmp/p15-options.txt`.
- Iris properties are backed up at `/data/local/tmp/p15-iris.properties`.
- `fcl_scene.sh restore` restores Iris; `fcl_p14.sh restore` restores the lib and `config.json`.
- The anland desktop is stopped during runs and restarted afterwards.
