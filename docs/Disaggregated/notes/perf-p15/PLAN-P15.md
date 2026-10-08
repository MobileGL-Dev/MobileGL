# P15 plan: the FCL gap first, then a draw-delta fast path

Written 2026-10-07 on `feat/disaggregated` at `a0f4aa83`. Device: Lenovo Y700 (Adreno 750,
`HA27Q3LQ`), rooted, clocks pinned. Baseline: `dev` at `08124c99` (the pull monolith).

This is the plan, plus the measurements it rests on (section 2). Nothing here is implemented yet.

## 0. Summary

1. **FCL's "unexplained ~3 ms/frame wait" is display pacing, and dev has it too.**
   - On every build, the presenting thread spends about 2.0 ms sleeping and 0.4 ms runnable per
     frame. That holds for dev and feat, on both backends.
   - The sleep is in `queueBuffer`. There the render thread waits for the `BLASTBufferQueue`
     mutex, which the app's binder thread holds across a synchronous `setTransactionState`. That
     call is phase-locked to the 165 Hz display.
   - The whole FCL fps gap vs dev is presenting-thread CPU:
     - Magma: 3.25 vs 1.85 ms/frame;
     - Espryt: 2.7 vs 2.05 ms/frame.
2. **FCL's Magma CPU gap is mostly non-draw verbs on the wire arm.** These are mechanisms dev's
   frontend arm had and the wire arm lost:
   - every `glClear` builds and destroys its own image view, render pass and framebuffer;
   - every blit creates a descriptor pool;
   - `glBufferSubData` takes a slow staged path;
   - the retired-object garbage collection and command-buffer reset grow to match.
   This explains about 1.0 of the 1.4 ms. rd12 Magma pays the clear and churn part too, about
   0.45 ms/frame.
3. **The (VAO, program) draw-packet table as sketched would never hit on rd12.**
   - rd12 draws 1471 times a frame, with 1466 distinct (VAO, program) pairs per frame. A pair
     repeats only across frames, and something outside the whitelist always runs between frames.
     So a packet stamped with a context epoch hits 0.0 % of the time.
   - What does hold on rd12 is the **draw-to-draw delta**: before 99.0 % of draws, the only calls
     since the previous draw are `glBindVertexArray` and a float uniform.
   - P15 therefore builds a draw-delta fast path:
     - "nothing context-global moved since the last draw", checked once per draw against two
       epochs (a client one and a server one);
     - per-VAO and per-program slices that persist across frames.
   It does not build a pair-keyed packet table.

Stage order: 0 (parity audit + wire non-draw fast path), 0b (an FCL 1.21.5 trace fixture), 1
(client draw-delta prototype), 2 (per-VAO / per-program slices), 3 (applier + backend
draw-delta), 4 (split arms, verify, default-on).

## 1. Measurement rules for P15

- **FCL:**
  - The primary metric is the presenting-thread CPU in ms/frame, which MobileGL's fps log already
    prints. Report fps beside it, never alone.
  - Above ~165 fps, frame time ≈ CPU + ~2.4 ms of display-coupled wait (section 2.1). So fps
    ratios understate CPU ratios and are not linear in CPU saved.
  - Take readings only from in-world steady windows. The probe waits for MC's `Time elapsed:`
    line and then four fps-log windows of more than 150 frames each.
- **Trace (rd12, openra, and the FCL 1.21.5 fixture from stage 0b):**
  - fps, best of 3.
  - Per-layer CPU in ms/frame from `profile_p14.sh` / `layers.py`. Remember that rd12 carries
    ~6.9 ms/frame of harness parse.
- **Method:**
  - `FINISH=false`, pinned clocks, best of 3.
  - Re-run any surprising result before believing it.
  - Every probe needs proof it engaged: a marker line, a counter, or a knob that visibly changes
    behaviour.
  - Every optimisation needs a negative control that can go red.
- **Device hygiene:**
  - One device job at a time, under the `devjob.sh` lock.
  - Restore FCL's library, config and properties afterwards, md5-checked (`fcl_p14.sh restore`).
  - Stop the anland desktop during runs.
  - FCL's SIGRTMIN+2 start-up death is known (DEBTS). The probe relaunches up to 4 times.
- **Gates for each landing:**
  - SSIM 16/16 (`matrix_p14.sh ssim`);
  - the retrace CI;
  - the run-ahead and syncval gates on any Magma change that touches barriers or pass structure;
  - an FCL soak before default-on.

## 2. Measurements behind the plan (2026-10-07)

Tools are in the session scratchpad `p15/`, to be landed under `tools/device_bench/disagg/`
(open question 4):
- `fcl_probe.sh`: waits for in-world frames, then runs 4 s of ftrace (sched, kgsl cmdbatch, atrace
  gfx) and 6 s of `simpleperf --trace-offcpu -p`;
- `offwait.py`: off-CPU time by open atrace slice and waker;
- `sfalign.py`: SurfaceFlinger transaction phase;
- `fclsplit.py` / `incl.py` / `drvcaller.py`: layer split, inclusive functions, and driver time
  by caller;
- `P15Count.inl` (measurement-only patch): per-draw pair and gap statistics.

### 2.1 Item 1: where FCL's wait is

FCL MC 1.21.5, in world, view distance 2, monolith. The figures for the presenting thread
(`Thread-14`) come from the ftrace window. The off-CPU breakdown comes from the simpleperf window.

| build | frame ms | CPU ms | sleep ms | runnable ms | biggest sleep |
|---|---|---|---|---|---|
| dev Magma (×2) | 4.27 / 4.46 | 1.77 | 2.05 / 2.07 | 0.35 / 0.41 | QueuePresentKHR > queueBuffer, woken by the app's binder thread |
| feat Magma (×2) | 5.67 / 5.79 | 3.43 | 1.98 / 2.10 | 0.44 / 0.47 | same |
| dev Espryt | 4.54 | 2.10 | 1.92 | 0.40 | eglSwapBuffers > queueBuffer, same waker |
| feat Espryt | 5.03 | 2.73 | 1.81 | 0.39 | same |

- **The off-CPU stack is the same on all four builds:**
  - The render thread: `BLASTBufferQueue::onFrameAvailable` → `std::mutex::lock`, 1.4-1.7
    ms/frame. A further 0.2-0.34 ms/frame goes to `Fence::waitForever` in `queueBuffer`.
  - The lock holder is the app binder thread: `releaseBufferCallback` →
    `acquireNextBufferLocked` → `Transaction::apply` → `BpSurfaceComposer::setTransactionState`,
    synchronous and under the BBQ mutex.
- **Inside SurfaceFlinger, `setTransactionState` has a median of 3.4 ms** (p10-p90 2.7-3.8). It
  returns **4.95 ms after SF's `flushTransactions`** (p10-p90 4.78-5.2). The flush period is 6.1
  ms (165 Hz, `NextFrameInterval 165_Hz`), with about one call per refresh. So the BBQ binder thread is
  paced by the display. Above refresh rate, the render thread's present waits for it through the
  mutex.
- **Other checks:**
  - GPU busy is ~14 % (union of kgsl `adreno_cmdbatch_retired`).
  - The swapchain is MAILBOX with 6 images, so present mode and image count are not the cause.
- **Consequence:** this is not a MobileGL defect and not a feat regression. Making the present
  asynchronous would raise every build's FCL fps above 165, but only by producing frames SF drops.
  It is not proposed as a perf item; open question 2 covers it.

### 2.2 FCL CPU by function (render thread, on-CPU ms/frame, inclusive)

**Magma monolith: feat 3.43 vs dev 1.77.** Repeat run: 3.25 vs 1.85 by the fps log.

| item | feat | dev | note |
|---|---|---|---|
| glClear → ClearWireFramebuffer | 0.42-0.46 | ~0.05 (Clear + ClearAttachmentsOnActiveRenderPass) | per clear: RetireWireDrawPass, vkCreateImageView + vkCreateRenderPass + vkCreateFramebuffer, its own pass, retire. 6 clears/frame, ~70 µs each |
| glBufferSubData → WriteWireBuffer / StagedWireRangeCopy | 0.28 | ~0 | 11 calls/frame, ~25 µs each; check whether it also ends the pass |
| CollectWireObjects (vkDestroyFramebuffer / DescriptorPool / ImageView) | 0.18 | none | the GC of the objects above |
| glBlitNamedFramebuffer → BlitWireColorImage | 0.13 | 0.02 | vkCreateDescriptorPool per blit (0.08) |
| TransitionWireImage | 0.14 | small | |
| BeginCommandRecording (vkResetCommandBuffer) + EndCommandRecording | 0.24 | 0.08 | larger, fragmented command buffers |
| glDrawElements (whole draw) | 0.93 | 0.56 | +0.37: client validate/emit ~0.27; SetupWireDraw 0.55 vs dev SetupDraw 0.54 |
| Present | 0.37 | 0.25 | |
| driver layer total | 1.46 | 0.49 | almost all the create/destroy above; not cache pressure |

**Espryt monolith: feat 2.73 vs dev 2.10.** The +0.62 splits as:
- client half +0.39;
- backend +0.09;
- driver +0.04;
- applier +0.035.

The clear/blit/sub-data path costs about the same as dev's: clear 0.16 vs 0.13, blit 0.125 vs
0.11, sub-data 0.024 vs 0.007. So Espryt has no churn of this kind (at most 0.07 ms/frame). Its
gap is the draw path: glDrawElements 1.01 vs 0.60, which is +4.7 µs per draw over 87 draws.

**rd12 Magma** (P14 `prof-fin` profiles, scaled to ms/frame):
- ClearWireFramebuffer ≈ 0.27, CollectWireObjects ≈ 0.18, BeginCommandRecording 0.12 (dev 0.02),
  blit 0.09 (dev 0.06).
- So stage 0 also pays on rd12: about 0.4-0.5 ms/frame.
- Dev's deferred-clear lookups cost about 0.6 ms/frame on rd12: `VkClearManager::HasPendingClear`
  is probed per draw, plus the key find. A naive port would be a regression there. Pending clears
  must be resolved once per pass begin, never per draw.

### 2.3 Draw-pair and gap statistics (P15Count, frontend entry level)

What each column measures:
- **pairs:** distinct (VAO lifetime id, program lifetime id) pairs per frame.
- **same as prev:** the previous draw had the same pair.
- **pair epoch hit:** the pair has drawn before, and no non-whitelisted GL entry has run since its
  last draw. This is the packet-table hit rate.
- **prev-draw gap clean:** no non-whitelisted entry since the previous draw, whichever pair it
  had. This is the draw-delta hit rate.

The whitelist is draws, `glBindVertexArray`, float uniforms and `glUseProgram`. "+buffer" adds
buffer binds and vertex-buffer binds.

| workload | draws/frame | GL calls/frame | pairs/frame (ever) | same as prev | pair epoch hit, whitelist / +buffer | prev-draw gap clean, whitelist / +buffer |
|---|---|---|---|---|---|---|
| rd12 (steady window) | 1471 | 4856 | 1466 (1473) | 0.3 % | 0.0 % / 0.0 % | **99.0 %** / 99.0 % |
| FCL MC 1.21.5 (10 windows, both backends) | 88 | 873 | 15.1 (27) | 82.9 % | 0.0 % / 80.6 % | 0.0 % / **80.6 %** |
| openra | 29 | 593 | 2 (2) | 93 % | 0 % / 0 % | 0 % / 0 % |

Engagement and controls:
- Allowing every category gives 99.9-100 %, which is the positive control.
- The FCL numbers are identical on Espryt and Magma, as expected for frontend counts.

What sits between draws:
- **rd12:** per draw, just BindVertexArray + Uniform3fv + DrawElements. The ~1 % of other gaps
  carry Enable/Disable, BlendFunc, BindTexture, Uniform1i and similar.
- **FCL:**
  - `BindBuffer` comes before 98.9 % of draws and `BindVertexBuffer` before 88.5 %. Both are
    VAO-scoped state: the element buffer and the vertex binding.
  - 19 % of draws also follow `BindFramebuffer` / `Viewport` / `PolygonMode`.
  - MC issues 96 `BindFramebuffer` calls per frame, mostly redundant.
- **openra:** every draw changes blend, texture, attrib pointers and sub-data. No per-draw
  shortcut applies, and none is needed: openra is not where the gap is.

## 3. Stages

### Stage 0: parity audit, then the wire non-draw fast path (Magma first)

**0.1 Parity audit (first step, judgement work).**
- **What to list:** every performance mechanism of dev's frontend arm that the record and wire
  arms lost when P13 W4 moved monolith onto the wire arm and W6e / `de6ab167` deleted the old
  code as dead. The split arm (the anland desktop) has always paid for them.
- **References:** `git show origin/dev:<path>` and the pre-W6 feat tree `88a11b81`.
- **Seed inventory:** [PARITY-SEED.md](PARITY-SEED.md) lists the definitions in OLD but not NEW
  for 17 backend files (regex-extracted, approximate). The audit's verdicts go in
  `perf-p15/PARITY-AUDIT.md`.
- **For each mechanism, record:**
  - what it did and its key;
  - whether the wire/record arm has an equivalent, and where;
  - whether FCL or rd12 shows its absence (section 2.2 numbers);
  - porting cost and correctness risk;
  - a verdict.

Seed rows (to be verified by the audit):

| mechanism (dev / 88a11b81) | wire/record arm today | shows in profiles | first estimate |
|---|---|---|---|
| `VkClearManager`: glClear queues a pending clear per attachment (texture + lifetime id + level/layer); the next pass on that attachment starts with LOAD_OP_CLEAR; materialised before reads, blits and copies | none: ClearWireFramebuffer runs a standalone pass with fresh objects | FCL 0.42, rd12 0.27 ms | **port, per pass begin**: dev's per-draw probe cost 0.6 ms on rd12 |
| `VkRenderPassManager::GetOrCreateRenderPass` hash cache of render pass + framebuffer, with OnPresent GC | partial: wire pass continuation, RenderPassEntry; framebuffers still created in SetupWireDraw (0.04) and in clears | FCL 0.04-0.19 (create) + 0.12 (destroy) | port a cache keyed on attachment lifetime ids + image epoch |
| shader blit to default FB (`TryBlitToDefaultFramebufferWithShader`), persistent blit resources (`InitializeBlitResources`) | BlitWireColorImage creates a descriptor pool per blit | FCL 0.08 + GC | persistent pool / set cache per (pipeline, source view) |
| `VkBufferManager::OnSubData` / `OnResidentSubData` (writes into resident or streamed slices) | WriteWireBuffer + StagedWireRangeCopy | FCL 0.28 | find out why it is ~25 µs per call; port the resident fast path; check for pass breaks |
| `TrySetupDrawFastPath` / `SetupDrawSnapshot` (4 snapshots keyed on program lifetime id) | P14 memos (pass continuation, unit samplers, pipeline/vertex/index bind skips) | SetupWireDraw ≈ dev SetupDraw on FCL | audit for leftovers only |
| `UniformManager` `FastRebindMemo`, `SampledBindingsUnchanged` | P14 descriptor-set rebind, walk-skip | small | audit |
| `VertexInputStateFactory` per-VAO memos (`VaoBackendMemos`) | vertex-input layout kept on its record (P14) | small | audit |
| `VkTextureManager` `SyncedTextureMemoEntry` / `DrawSyncScope` | P14 direct-mapped texture fronts | small | audit |
| Espryt `StateBackendObjectRegistry` (pointer-keyed twins), `SyncCurrentProgram`, `SyncVaoAttributeBuffersByHandle`, `BackendFramebufferObject::SyncToBackend` | twins keyed on applier serials (P14) | Espryt backend +0.09 on FCL | audit; low priority |

**0.2 Port, in order of measured value:**

1. **Deferred clears on the wire arm.**
   - Key: image lifetime id + level/layer range + aspect, never a VkImage or a GL name.
   - A clear records a pending entry. The next wire pass that targets the image takes it as
     LOAD_OP_CLEAR.
   - A clear inside an open pass on the same attachments becomes `vkCmdClearAttachments`, without
     ending the pass.
   - Materialise pending clears before any read: sampling, readback, blit source, copy, mipmap
     generation, shared-image export and present. The cost is one check per pass begin and one
     per read verb; no per-draw probe.
   - The clear-mask cases (partial colour mask, stencil write mask, scissor) keep the current path.
2. **A render-pass + framebuffer cache** for wire passes and clears.
   - Keyed on attachment lifetime ids and the image epoch.
   - Retired when an attachment's lifetime ends: the store destroy epoch.
3. **Blit descriptor reuse:** a persistent pool, sets keyed on (source view lifetime id, sampler).
4. **The `glBufferSubData` path:** find the ~25 µs per call, then port the resident-slice write
   and check whether it ends the pass.
5. **Command-buffer churn:** once 1-4 land, re-measure `vkResetCommandBuffer` /
   `EndCommandRecording`. They should fall with the pass count.

**Espryt:** no churn of this kind measured (≤0.07 ms/frame on FCL). The audit should still cover
Espryt's deleted mechanisms. Port only those that show in profiles.

**Correctness:**
- Deferred clears are the e8e1521 class of risk: a read that skips materialisation renders stale
  content.
- Tests:
  - an integration scenario per read path (sample after clear; readback; blit source; copy; mipmap;
    present), with negative controls that drop materialisation for one read kind and must go red;
  - SSIM 16/16;
  - the syncval gates (a LOAD_OP_CLEAR changes the pass's first access).
- Runtime knob, e.g. `MOBILEGL_MAGMA_DEFERRED_CLEARS=0`, kept for one release.

**Expected gain:**
- FCL Magma: 0.8-1.0 of the 1.4 ms presenting-thread CPU gap. Medium-high confidence: the items
  are measured, the removal fraction is estimated.
- rd12 Magma: 0.3-0.5 ms/frame, about +3-5 % fps. Medium confidence.

### Stage 0b: an FCL MC 1.21.5 trace fixture

- **Why:** rd12 (MC 1.21.4) never exercised the per-frame clear, blit and BufferSubData pattern
  that FCL's MC 1.21.5 runs. The trace matrix must cover what FCL runs.
- **Capture:**
  - with the on-device FCL capture wrapper (`tools/trace_replay/skills/trace-fixture-authoring-on-android-fcl`);
  - the same world and view distance as the FCL bench, from the in-world steady state;
  - trace mode forces 854×480.
- **Trim:** a multi-frame window, like rd12 (`gltrim -f`, about 250 frames), so the benchmark
  measures steady frames. The golden is the final frame.
- **Golden:**
  - follow `trace-fixture-authoring/SKILL.md`: generate it on a real GPU (Windows `glretrace` on
    the RTX 3080, or a known-good Android replay); never on llvmpipe for anything heavy;
  - check its content against a live-run screenshot;
  - remove alpha; `repack --brotli`; register with `ci: false` like rd12.
- **Use:**
  - add to `matrix_p14.sh` `wl_args`;
  - A/B every stage on rd12, openra and this fixture;
  - check that P15Count on the fixture reproduces section 2.3's FCL row (88 draws, 15 pairs); if
    not, the fixture does not represent FCL.
- **Order:** best done before stage 0.2 lands, so stage 0 can be A/B'd on a trace as well as on FCL.

### Stage 1: client draw-delta prototype (Espryt monolith)

This is the client half of the old sketch, re-keyed on what section 2.3 shows.

- **Client context epoch:**
  - one `Uint64` per context;
  - bumped by every GL/EGL entry except a whitelist;
  - share-group-scoped objects (textures, buffers, programs, renderbuffers, samplers) bump a
    share-group epoch that every context in the group checks.
- **Whitelist classes,** decided per entry in a checked-in table (section 4.4):
  - **free:** draws, `glBindVertexArray`, float `glUniform*` / `glProgramUniform*`, `glUseProgram`
    (the program is a key, not context state);
  - **VAO-scoped:** `glBindVertexBuffer`, `glVertexAttrib*Pointer`, `glEnable/DisableVertexAttribArray`,
    `glBindBuffer(GL_ELEMENT_ARRAY_BUFFER)` and the VAO DSA calls. These bump the bound VAO's own
    versions, which the per-VAO slice checks. `glBindBuffer(GL_ARRAY_BUFFER)` changes nothing a draw
    reads;
  - **program-scoped:** integer uniforms and block bindings. These bump the program's backend-state
    / block-binding version;
  - **everything else** bumps the epoch.
  - **Redundant binds** (same object, same value) may skip the bump only where the entry already
    compares. MC issues 96 BindFramebuffer calls per frame and 19 % of draws follow one.
- **At the validate point:**
  - if (epoch, share-group epoch, context serial, slot reset epoch) are unchanged since the
    previous draw, skip `Tracker::Update`'s context-global shutters, the residual copy and the
    context-value emit;
  - run only the VAO-derived shutters (through the per-VAO slice once stage 2 lands), the
    program-derived ones, and the uniform span;
  - also skip `ReadDrawBindings` / `MarkGpuWritesForDraw` when no persistent map is live and no
    image or SSBO write is bound. The persistent-map re-sync stays a per-draw input otherwise.
- **Kill switch:** `MOBILEGL_PIPE_DRAW_DELTA=0`, default off until stage 4.
- **Measure** on rd12, FCL and the stage 0b fixture against P14's per-item numbers: Tracker::Update
  0.12, CopyField 0.10, validate self 0.10, ReadDrawBindings 0.06, MarkGpuWritesForDraw 0.05,
  thread-local/atomic 0.07 ms/frame.
- **Expected:** 0.3-0.5 ms/frame on rd12 Espryt monolith (91 → ~94-96 fps), and 0.1-0.25 ms on FCL
  Espryt. Medium confidence.
- **Decide** on stages 2-3 from this.

### Stage 2: per-VAO and per-program slices

- **Per-VAO slice.** This is what the old "packet" becomes. It extends `VaoMemo`, which is already
  keyed on VAO lifetime id + slot reset epoch.
  - It holds the vertex-elements handle + published config version, the buffer set + index
    handles + hash, the stored-on-server mirrors, and the backend's per-VAO answers (stage 3).
  - It is reached by an index stored on the frontend VAO object, not a hash probe.
  - rd12 rebinds a different VAO for every draw, but each VAO is seen every frame and its config
    hardly moves. So the slice turns EmitVertexElements (0.13) / EmitVertexBuffers (0.07) into
    a version compare plus a handle reuse.
- **Per-program slice:** the shader CSO handle, link and backend-state versions, the
  uniform-span layout and the UBO block size.
- **Expected:**
  - rd12: 0.1-0.2 ms on the client half;
  - FCL: small, because only 27 pairs exist. But each FCL draw also changes the vertex buffer
    binding, which is now a VAO-version bump that the slice re-derives cheaply.

### Stage 3: applier and backend draw-delta

- **Server epoch, generated from `PipeCalls.def`:**
  - The applier bumps an epoch for every applied record except a whitelist: draw,
    bind_vertex_elements, the uniform span, and the per-VAO / per-program record kinds.
  - A constexpr table is generated from the record opcode list. A test fails if a record kind is
    missing from it. This is the server-side twin of section 4.4.
- **Backend-internal mutations also bump a backend epoch.** These are:
  - texture `Serial` / `ParamsSerial` / `PendingUploads`;
  - twin force-resync flags;
  - the sampler CSO serial;
  - shared-image state;
  - GPU-written marks;
  - on Magma: the image epoch, the resource-erase epoch, an image layout leaving GENERAL, and the
    store-destroy epoch.
  The rule from the content-version lesson applies: every place that changes a twin's synced
  state bumps it, including `glGenerateMipmap` growing mips. P14's Magma memos already check most of these.
- **When both epochs are unchanged since the previous draw,** the backend skips the
  context-global walks:
  - Espryt: `SyncNeccessaryTextures` / `SyncNeccessaryBuffers`, `AdoptUnchangedBufferSets`,
    `BindCurrentProgramWithResources`;
  - Magma: whatever of SetupWireDraw is not VAO- or program-derived.
- **Expected:**
  - rd12 Espryt backend: 0.15-0.3 ms of the +0.45;
  - rd12 Magma: 0.1-0.3 ms (its backend is already below dev);
  - driver: 0-0.3 ms, if the cache-pressure reading is right. Measure L1D refills before and
    after with `stat_p14.sh`.

### Stage 4: split arms, verify, default-on

- **Inproc / spawn / tcp:**
  - the client epoch and the per-VAO/program client slices live in the client;
  - the server epoch and the backend slices live where the applier runs: the same thread on
    monolith, the apply thread on inproc, the server process on spawn and tcp.
  - Nothing new crosses the wire.
  - Under run-ahead, client validity depends only on client epochs. The server computes its own
    epoch from the records it applies, so the two cannot disagree about what moved.
- **Default-on** after:
  - CI and SSIM green on all arms;
  - verify lanes green with the shadow check (section 4.5);
  - an FCL soak on both backends.
  Keep the knob for one release.
- **Expected inproc:**
  - Espryt rd12 is client-bound, so stages 1-2 give 112 → ~116-119, above dev's 115.
  - Magma inproc is apply-bound, so it gains from stages 0 and 3.

## 4. Design details

### 4.1 Scope and non-goals

**In scope:**
- the stage 0 wire non-draw fast path (Magma);
- the client and server draw-delta epochs;
- per-VAO and per-program slices;
- the backend draw-delta.

**Non-goals:**
- a (VAO, program) pair table: 0 % hit on rd12, and FCL's hits are covered by the draw delta;
- record batching (measured and dropped in P14);
- protocol changes;
- asynchronous present (open question 2);
- native TLS (open question 3);
- openra, whose gaps change state on every draw.

### 4.2 Per-layer state for one draw on the fast path

| layer | read per draw on a hit | kept in |
|---|---|---|
| tracker | client epoch + share-group epoch + context serial + slot reset epoch (4 compares); VAO identity (lifetime id + config + attribute generation); program identity (lifetime id + link + backend-state + block-binding versions) | context, per-VAO slice, per-program slice |
| emitter | per-VAO slice: elements / buffers / index handles + versions; uniform span; draw record | per-VAO slice |
| applier | server epoch; vertex-elements record by slot + generation (bind by reference) | applier context |
| backend | backend epoch; Espryt VAO twin + program twin pointers; Magma vertex-input id, pipeline handle, descriptor-set layout, pass-compatibility id | per-VAO / per-program backend slices |

### 4.3 Keys

- Use monotonic lifetime ids only: VAO, program, buffer and texture lifetime ids; slot +
  generation on the server side. Never a GL name or a pointer. This is the ABA rule, broken three
  times so far (sampler, texture, program).
- Slices are invalidated by:
  - the context serial;
  - `MGPipeSlots().ResetEpoch()`;
  - the store-destroy epoch.
- A slice index stored on a frontend object is valid only together with the object's lifetime id.

### 4.4 Invalidation sources, and how to prove none is missed

**Client entry-point table** (`MG_Pipe/DrawDeltaClass.def`, new):
- one row per exported GL entry, giving its class (free / VAO-scoped / program-scoped /
  share-group / context);
- `DECLARE_GL_FUNCTION_END*` passes `#name` to `GLStreamScope`, which looks up the class at
  compile time;
- the hand-written `MOBILEGL_GL_API` definitions (~170 in `Definitions.cpp`, mostly EXT/ARB
  aliases plus `glFinish`/`glFlush`) and the EGL entries use the same scope helper.

**Completeness tests:**
1. A CTest scans `Definitions.cpp` and the EGL exports. It fails if:
   - any exported entry is missing from the table;
   - a table row names a non-exported entry;
   - any `MOBILEGL_GL_API` body neither constructs the scope nor forwards to a macro entry.
2. A runtime self-test: in a debug build, every entry asserts it consulted the table. A fuzz
   driver walks all entries.
3. **Negative controls:** a test-only knob `MOBILEGL_PIPE_DRAW_DELTA_DROP=<entry>` removes one
   entry's bump. Run for a representative entry per class:
   - `glBindTexture`;
   - `glTexParameteri`;
   - `glBindFramebuffer`;
   - `glEnable`;
   - `glBindBufferBase`;
   - `glUniform1i` (program-scoped);
   - `glBindVertexBuffer` (VAO-scoped);
   - an EXT alias;
   - an EGL entry.
   Each must turn the shadow check (4.5) and a scenario red.

**Mutations that bypass GL entries:**
- persistent-mapped writes stay a per-draw input;
- async link completion (link version);
- `eglMakeCurrent` (context serial);
- another context in the share group (share-group epoch);
- server-side backend mutations (backend epoch, 3.3).

**Server side:**
- the generated table from `PipeCalls.def`, with the same three tests;
- for the backend epoch, a list of every writer of the synced-state fields, enforced by making
  those fields private behind a setter that bumps. This is the same chokepoint design as the
  Magma layout setter in the whole-draw fast-path note.

### 4.5 The verify comparator

`MOBILEGL_PIPE_VERIFY` compares the residual block with a live snapshot at the verb.

Under verify:
- the fast path is armed but always takes the slow path;
- it asserts that the fast path would have hit with byte-identical residual, record and
  vertex-element output (a **shadow check**).
So every verify lane cross-checks epoch coverage for free, and the negative controls in 4.4
redden it.

A non-verify build has a sampled shadow mode, `MOBILEGL_PIPE_DRAW_DELTA_SHADOW=N`, which checks
every Nth hit. It is for FCL soaks.

POISON stamps (trace builds): on a hit, the stamp set recorded at the last slow draw is applied in
one bulk write. Freshness checks then keep working.

### 4.6 Tests

- Unit tests for the epoch classes; the table tests (4.4).
- Integration scenarios:
  - draw-delta hit/miss per class;
  - share-group mutation from a second context;
  - persistent-map write between two hit draws;
  - async link completing between draws;
  - VAO rebind with the same lifetime id after delete + gen (ABA);
  - context switch;
  - slot reset.
  Each runs with a negative control.
- Stage 0: deferred-clear read-path scenarios with drop-materialisation controls; syncval.
- All: SSIM 16/16, the retrace CI, FCL soak.

## 5. Expected gains

ms/frame of CPU on the bound thread.

| stage | rd12 Espryt mono | rd12 Magma mono | FCL Espryt mono (presenting CPU) | FCL Magma mono (presenting CPU) | confidence |
|---|---|---|---|---|---|
| 0 wire non-draw | 0-0.07 | 0.3-0.5 | ≤0.07 | 0.8-1.0 | medium-high (FCL), medium (rd12) |
| 1 client draw-delta | 0.3-0.5 | 0.3-0.5 | 0.1-0.25 | 0.1-0.25 | medium |
| 2 slices | 0.1-0.2 | 0.1-0.2 | ~0.05 | ~0.05 | low-medium |
| 3 backend draw-delta | 0.15-0.3 (+ driver 0-0.3) | 0.1-0.3 | ~0.05 | 0.05-0.1 | low-medium; driver unproven |
| total | 0.55-1.0 (+driver), gap 2.5 | 0.8-1.5, gap 0.76 | 0.25-0.4, gap 0.62 | 1.0-1.4, gap 1.4 | |

What the totals mean:
- **rd12 Espryt monolith:** 91 → roughly 96-101 fps. Short of dev's 115 unless the driver term
  moves.
- **rd12 Magma monolith:** 84 → about dev's 87 or above.
- **FCL Magma:** CPU gap mostly closed. Espryt about halved.
- **Inproc:** above dev on Espryt rd12. Magma inproc gains stages 0 + 3.
- **Revision:** these are below the old sketch's 1.0-1.6 ms because the pair table does not hit on
  rd12.

## 6. Risks

- **A missed epoch bump renders stale state.** This is the dominant risk. Mitigations:
  - the generated tables and their completeness tests;
  - the verify shadow check;
  - per-class negative controls;
  - SSIM and the retrace CI;
  - default-off until stage 4.
- **Deferred clears skipping a materialisation.** Same class as the reverted e8e1521. Mitigations:
  per-read-path scenarios with drop controls, syncval, SSIM.
- **ABA on slice keys.** Mitigations: lifetime ids, slot + generation, the reset and destroy
  epochs.
- **Share groups and multiple sessions:** per-context epochs miss a mutation from another context
  in the group. Mitigation: the share-group epoch. The multi-session server hazards also apply on
  the split arms.
- **Gains below estimate:** the stage 1 prototype is the go/no-go for stages 2-3.
- **FCL fixture fidelity:** stage 0b's P15Count check.

## 7. Open questions for the user

1. **The FCL metric.** Do you agree to make presenting-thread CPU ms/frame the primary FCL metric?
   fps above refresh rate is display-coupled (section 2.1).
2. **Asynchronous present.** Presenting from a separate thread would cut the ~2 ms/frame BBQ wait
   from the render thread on every build, dev included. It would raise FCL fps above refresh, but
   the extra frames are ones SF drops; at or below refresh nothing changes. Leave it out (my
   recommendation), or plan it as a separate item?
3. **Native TLS** (minSdk 29, +1.7 % Espryt): still your call from P14.
4. **The P15 measurement tools.** May I commit `fcl_probe.sh`, `offwait.py`, `sfalign.py`,
   `fclsplit.py`, `drvcaller.py` and `incl.py` under `tools/device_bench/disagg/` with the plan?
   They are tooling, not product code. This assignment was docs-only, so they are not in this
   commit.
5. **Magma deferred clears, Espryt side:** the wire arm's Espryt clears cost the same as dev's, so
   no port is planned there. Confirm.
