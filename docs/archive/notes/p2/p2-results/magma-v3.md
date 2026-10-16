# P2 package D — `p2/magma` result (v3, rework round 3)

Tree: `~/w7/p2-magma`, branch `p2/magma`, from the contract commit `9c6a8a25` (tag
`p2/contract`). HEAD `93cf2249`. Not pushed. Build dirs: `build-linux` (pull, the G1 build),
`build-push`, `build-nolegacy` (`-DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF`)
and — new this round, review v2 minor 8 — `build-verify` (`-DMOBILEGL_PIPE_VERIFY=ON
-DMOBILEGL_ITEST_REQUIRE_GPU=ON`, which forces `MOBILEGL_PIPE_PUSH=ON`).

Verification scaffolding, **not deliverables**: `~/w7/p2-magma-armed` (`tmp/magma-armed` =
`p2/magma` merged with `p2/tracker`, re-merged this round at `840f8026`, clean) is where the
m1 arming evidence and the three red probes were taken. Its source was restored after every
probe and re-verified green; `git status` on both trees is clean apart from the untracked
`build-nolegacy/` directory.

## 1. Commits

| sha | subject |
|---|---|
| `d66f400f` | `[Refactor] (Magma): key the pipeline memo on the render-state CSO handle and stop recomputing a hash the client already computed` |
| `553065dc` | `[Refactor] (Magma): drive the dynamic tail from the pushed dynamic version and make the chunk table check DynamicTailKey's inventory` |
| `391e1230` | `[Refactor] (Magma): key the vertex-input cache and the VAO draw memo on {slot, gen} instead of a lifetime id and a heap address` |
| `fb410351` | `[Refactor] (State, Magma): take the backend's raw pointers out of the frontend VAO - the hash and state memos become the factory's own per-slot fields` |
| `a0e7c93d` | `[Fix] (Magma): make the legacy-memo lever a startup gate for Magma's own bit, bound the {slot, gen} mint, and keep the all-pull arm free of push-only cost` |
| `1e89fd90` | `[Fix] (Magma): assert rather than assume that a handle indexed into a per-slot table is non-null` |
| **`38a76e5d`** | **`[Fix] (Magma): size the {slot, gen} mint by the live working set instead of by a capacity, and give it to the renderer that uses it`** |
| **`93cf2249`** | **`[Fix] (Magma): log the mint's high-water at a level a shipped build keeps`** |

The last two are this round. Files touched by the branch are still exactly the six C.3 names
(`git diff --name-only 9c6a8a25..HEAD`), all this package's by C.5:
`MG_Backend/DirectVulkan/Renderer/{MagmaPipeArms.h, VulkanRenderer.h, VulkanRenderer.cpp,
VertexInputStateFactory.h, VertexInputStateFactory.cpp}` and
`MG_State/GLState/VertexArrayState/VertexArrayObject.h`.

---

## 2. MAJOR M1 — what changed, and the evidence

The review's finding was correct and I take all of it: the r2 `MagmaPipeIdentityTable` was a
**fixed** 2048/8192-entry 2-way LRU, it **introduced** eviction where the content-hash memo and
the resolved-state memo previously had none (they were unbounded `mutable` fields on
`VertexArrayObject`), and every green gate in v2 §3 was green below both capacities.

### 2.1 The fix: no capacity at all, reclamation by age

`MagmaPipeIdentityTable` (`MagmaPipeArms.h`) is now a grow-on-demand mint:

* `UnorderedMap<lifetimeId, slotIndex>` + a dense `Vector<Entry>` + a free list, with a
  **one-entry front memo** in front of the map probe;
* `OnFrameBoundary()` retires slots whose object has not been drawn for
  `kRetireAgeBoundaries = 1024` boundaries, swept every `kSweepInterval = 256` — the same two
  numbers `VertexInputStateFactory::OnFrameBoundary` uses for the entries those slots key —
  and returns them to the free list. `Gen` moves on **reuse**, so reclamation is exactly as
  ABA-proof as the LRU was;
* driven from `VulkanRenderer`'s two existing frame-boundary sites, beside the factory's own.

Age-based reclamation is the stand-in for the frontend death notification P2 has no hook for
(v2 MAJOR 3's reason for not using `MG_Impl/Pipe/SlotAllocator` is unchanged and still stated
in the header). Footprint therefore tracks the **live drawn working set**, not objects ever
created — which is what makes "no capacity" affordable.

Consumer tables:

* `VertexInputStateFactory::m_vaoMemos` — the two memos that had **no** capacity before this
  package — now has none again. It follows the mint through `MagmaPipeSlotTable<T>`, a chunked
  table (256 entries/chunk) whose **entry addresses never move**, which is D12.4's
  grow-on-demand ask without a relocating `Vector`. 48 B per live VAO.
* `VulkanRenderer::m_vaoDrawMemoTable` — the one memo that **did** have a capacity before this
  package — deliberately keeps the base ref's 2048 entries and the base ref's
  older-`frameSerial` victim rule, and changes only its **key** (`{slot, gen}` instead of a
  Fibonacci-mixed heap address plus a lifetime-id compare). A `VaoDrawMemo` is ~450 B
  (`ResolvedVertexBindings` dominates), so growing it with the live VAO set is megabytes on an
  LMK platform; losing one costs one vertex-binding re-resolve, exactly what it cost on the
  base ref.

Ownership: the two tables are a `MagmaPipeIdentityTables` **member of `VulkanRenderer`**,
handed to its `VertexInputStateFactory` at construction, instead of two function-local statics
(review v2 minor 4).

### 2.2 Evidence 1 — handle churn, the review's own workloads

`scratchpad/wf5/p2-magma-r3/churn.py` is a verbatim transcription of the landed `Acquire` /
`ClaimSlot` / `OnFrameBoundary`, driven with exactly the workloads the review used against r2.
Output at `~/w7/p2-magma-r3-churn.txt`:

| live objects | 512 | 1024 | 2048 | 2500 | 3000 | 4000 | 8192 | 10000 | 16000 |
|---|---|---|---|---|---|---|---|---|---|
| r2 (review, sparse ids) | 3.1 % | 20.0 % | 58.1 % | — | — | — | — | — | — |
| r2 (review, consecutive) | 0 % | 0 % | 0 % | 54.2 % | 95.2 % | 100 % | — | 54.2 %¹ | 100 %¹ |
| **r3, consecutive** | **0.0 %** | **0.0 %** | **0.0 %** | **0.0 %** | **0.0 %** | **0.0 %** | **0.0 %** | **0.0 %** | **0.0 %** |
| **r3, sparse** | **0.0 %** | **0.0 %** | **0.0 %** | **0.0 %** | **0.0 %** | **0.0 %** | **0.0 %** | **0.0 %** | **0.0 %** |

¹ the review's buffer-table row (8192 entries). Identical at 1 and 5 acquisitions per use.

Reclamation, same script: 8 waves × 500 VAOs × 400 frames = **4000 objects ever created, 500
live at a time → 2500 slots minted, 1500 live, 1000 on the free list**. The table converges on
the working set plus the retirement lag rather than on objects ever created.

### 2.3 Evidence 2 — the desktop corpus already reaches 69 % of r2's VAO capacity

The mint logs its high-water at power-of-two milestones (see §4, minor 1/D-16). On the armed
tree's 40-trace retrace, `minecraft-1.21.4-rd12-odinlite-in-world.DirectVulkan` emits

```
MagmaPipeIdentityTable(VertexElementsCso): high-water 1024 slots minted, 1023 live
MagmaPipeIdentityTable(Buffer):            high-water 1024 slots minted, 1023 live
```

A throwaway 64-slot-granularity probe on the scaffolding tree (`peak.sh`, source restored,
`~/w7/p2-magma-r3-peak-*.log`) puts the peak at **1408 live VAOs and 1408 live buffers**
(±64) for that one trace.

That is **69 % of r2's 2048-entry VAO table on the existing desktop corpus, at render distance
12**, and the review's own sparse-id measurement puts r2 at 20 % handle churn from 1024 live
objects. So v2 §2 MAJOR 4's "nothing on desktop reaches those working sets" was false against
this very corpus, and a device case at a higher render distance or with Sodium would have been
past the capacity, not near it. No device time was needed to establish this.

### 2.4 Evidence 3 — the draw memo, the one table that keeps a capacity

`drawmemo.py` (`~/w7/p2-magma-r3-drawmemo.txt`) drives a 2048-entry, 2-way, older-serial-victim
table with (a) the base ref's mixed heap addresses and (b) r3's dense `{slot, gen}`, and
measures the **steady-state** miss rate (first frame discarded):

| live VAOs | 64 | 256 | 512 | 1024 | 2048 | 2500 | 3000 | 4096 | 8192 |
|---|---|---|---|---|---|---|---|---|---|
| base ref (address hash) | 0.0 % | 1.2 % | 6.4 % | 24.0 % | 60.0 % | 69.5 % | 79.1 % | 91.0 % | 99.6 % |
| **r3 (`{slot, gen}` masked)** | **0.0 %** | **0.0 %** | **0.0 %** | **0.0 %** | **0.0 %** | **36.2 %** | **63.5 %** | 100 % | 100 % |

r3 is better everywhere up to 3000 live VAOs and equal-and-hopeless above 4096 (2 × capacity,
where LRU on a cyclic pattern cannot win for any indexing — I also measured a skewed
mask+mixed-alternate variant, which is no better; the numbers are in the same file). **At no
working set below 4096 is this table worse than what it replaces**, and the two memos that
used to be unbounded are no longer in this table's fate at all.

### 2.5 Evidence 4 — three red probes on the armed tree

All on `~/w7/p2-magma-armed`, source `git checkout`-restored and re-verified 432/432 after each.

| probe | what | expected | observed |
|---|---|---|---|
| **A** | `kSweepInterval = 1`, `kRetireAgeBoundaries = 0` — every slot retired at every frame boundary, maximum reclamation churn | green (correctness must not depend on the reclamation policy) | **432/432** |
| **B** | probe A **plus** the `++Gen` on slot reuse removed | red, hoped | **432/432 — NOT red.** Reported honestly below |
| **C** | every VAO's handle collapsed to a constant `{1,1}` (buffers untouched), so every VAO shares one entry of both consumer tables and inherits its predecessor's memos | red | **6 of 432 fail** — `DirectVulkan[.AsyncCompile].XfbAfterClipDistanceScenario.{SkipComponentsCaptureAfterClipDistanceWorkloadLines8, …Points1, SkipComponentsCaptureSurvivesEveryClipWorkloadStopPoint}`, four of them SEGFAULT. Restored: **432/432** |

Probe A is the one that matters for M1: correctness is independent of when slots are reclaimed,
so the only thing the retirement age buys or costs is memo recomputes.

Probe C is m3's `ROADMAP.md:7` half: the suite **can** see a broken vertex-input identity.

**Probe B is a negative result and I am not going to dress it up.** Removing the generation
bump does not turn the 432 red, because an inherited `VaoBackendMemos` still has to survive
`HashConfigVersion != vao.GetConfigVersion()` and an inherited `VaoDrawMemo` still has to
survive the per-draw `bindings` revalidation (frame serial, content hash, live buffer pointers,
slice epochs). Only a deliberately constructed ABA — same address, byte-identical attribute
configuration, different buffer — defeats all of those at once, and that construction is
**D18's `HandleRecycleScenario`, which is package E's file and is not on this tree**. So: the
`Gen` discipline is load-bearing by construction and is *not* covered by any test this package
can reach. G8 is the gate; the integrator must not read this package's green 432 as covering it.

---

## 3. Verification — every command and its actual result

All from `~/w7/p2-magma` at `93cf2249` unless stated. Driver scripts under
`scratchpad/wf5/p2-magma-r3/`, logs under `~/w7/p2-magma-r3*`.

| # | command | result |
|---|---|---|
| V1 | `cmake --build build-{linux,push,nolegacy,verify} -j 12` | rc 0 in all four |
| V2 | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; `.text +160`. The four are the **contract's** (`RenderState::RenderState()`, `SetCapability`, `IsCapabilityEnabled`, `_GLOBAL__sub_I_DirectGLES.cpp`) (G1) |
| V3 | same, `--before ~/w7/p2-contract/build-linux/libMobileGL.so` | `0 added, 0 removed, **0 resized**, 0 renamed`, `.text +0`, `27060 of 27060 normalised names unchanged`. **This package's pull build is byte-identical to the contract's** (D-13 still holds after the rewrite) |
| V4 | `awk '/^    namespace RenderStateImpl \{/,…/' DirectGLES.cpp \| sha256sum` | `d8fd1c48…0efe27`, equal to `~/w7/p2-before-syncrenderstate.sha` (G5) |
| V5 | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty (G13) |
| V6 | `python3 scripts/check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)` (G13) |
| V7 | `python3 scripts/gen_pipe.py --check` / `--self-test` | rc 0 / `7 negative-control trip(s), positive control OK` (G13) |
| V8 | stdio grep over `MG_Backend/DirectVulkan` + `MG_State/.../VertexArrayState` | no stdio (three hits are `Vector<…> inputs(` matching the `puts(` pattern) (G13) |
| V9 | `ctest -N` names, working grep, four build dirs | `build-linux` / `build-push` / `build-nolegacy` = **2367**, byte-identical to each other; `build-verify` = **3185**, a strict superset (818 verify-only, 0 missing). vs the 2363 baseline: **0 removed, 4 added**, all four the contract's `MG_Test/Pipe` placeholders (G2 name half, G14) |
| V10 | `ctest -L unit -j 8` in all four dirs | **1489/1489** in each |
| V11 | `ctest -L integration-gpu -j 4 -R DirectVulkan`, `build-push`, default mask | **432/432** |
| V12 | same, `MOBILEGL_PIPE_PUSH=0` (the all-pull control) | **432/432** |
| V13 | same, `MOBILEGL_PIPE_LEGACY_MEMOS=0` at the default mask | **432/432** |
| V14 | same, `MOBILEGL_PIPE_PUSH=0x60 MOBILEGL_PIPE_LEGACY_MEMOS=0` (v1 MAJOR 1's repro) | **432/432** |
| V15 | same, `build-nolegacy` | **432/432** |
| V16 | same, `build-linux` (pull) | **432/432** |
| V17 | `ctest --test-dir build-verify -L integration-verify -j 4 -R DirectVulkan` | **409/409** (G4's DirectVulkan half — new this round, review v2 minor 8) |
| V18 | `retrace_gate.py --tree ~/w7/p2-magma --lib build-push/libMobileGL.so --out ~/w7/retrace-out/p2-magma-r3f -j 4 --only 'DirectVulkan$'` | **40/40 PASS**, min SSIM **0.993475** (`minecraft-1.21.11-main-menu`), gate 0.99 (G3, DirectVulkan half) |
| V19 | `git diff --name-only 9c6a8a25..HEAD` | the six files C.3 names, nothing else (C.5) |
| V20 | `grep -rn 'BackendStateMemo\|BackendAuxMemo\|m_backendHashMemo' MobileGL/MG_State MobileGL/MG_Backend` | 15 hits outside `ProgramObject`, every one inside a `MOBILEGL_PIPE_LEGACY_MEMOS` arm (D-8) |
| V21 | `grep -rn "MagmaPipeVaoIdentity\|MagmaPipeBufferIdentity\|MagmaPipeHandleOf\|kMagmaVaoIdentityEntries\|kVaoMemoSlotCount" MobileGL/` | **none** — the process-global accessors and the two capacity constants are gone (minor 4) |
| V22 | `MOBILEGL_PIPE_PUSH=0x20 MOBILEGL_PIPE_LEGACY_MEMOS=0`, 13 DirectVulkan cases, `MOBILEGL_LOG_FILE_PATH` | one `Fatal{PipeLegacyMemosDisabled}` naming **bit 6 at startup**; ctest reports a visible **SKIP**, not a mid-frame abort. D14 semantics unchanged |
| V23 | `MOBILEGL_PIPE_PUSH=0x7e MOBILEGL_PIPE_LEGACY_MEMOS=0`, same 13 cases | **13/13 green** *and* one startup `WARN` naming `kMGPipeSubsystemRenderState (bit 0 …)` — the silent case review v2 minor 2 found. Control at the default mask: **0** such warnings |
| V24 | armed tree (`840f8026`): `ctest -L integration-gpu -j 4 -R DirectVulkan` | **432/432** |
| V25 | armed tree: 13 `CrossFrameBuffer` cases with `MOBILEGL_LOG_FILE_PATH` | **0** no-CSO fallback warnings (delivered tree, same cases: **1**) |
| V26 | armed tree: `retrace_gate.py … --only 'DirectVulkan$'` | **40/40 PASS**; **0** of 40 logs carry the fallback warning (delivered tree: **80** files) |
| V27 | red probes A / B / C (§2.5), each restored and re-verified | 432/432 · 432/432 · **6 failures**, restored 432/432 |
| V28 | `~/w7/p2-magma-armed`, `~/w7/p2-magma` `git status --short` | clean apart from the untracked `build-nolegacy/` |

Nothing flaked this round: the two `…IsActuallyArmedWhenTheEnvironmentPinsItOn` cases that
failed intermittently in v2 passed in all 13 `-j 4` integration runs here.

---

## 4. The minors

| # | disposition |
|---|---|
| **1 — `MGLOG_W_ONCE` on the per-draw path, cost misdocumented** | **Fixed.** The latch is now a `mutable Bool m_pipelineCsoFallbackWarned` on the renderer and the log is `MGLOG_W`. The comment says what `MOBILEGL_LOG_ONCE_INTERNAL` actually is (an unconditional `std::atomic_flag::test_and_set`, i.e. a locked `xchg` per evaluation) and why this site may not pay it. Per renderer rather than per process is also the right scope. |
| **2 — `MOBILEGL_PIPE_LEGACY_MEMOS=0` with bit 0 clear is silent** | **Fixed as a diagnostic, declared as D-15.** `MagmaPipeValidateSubsystemConfiguration` now names the combination at startup with `MGLOG_W`. It is **not** made fatal: bit 0 is not Track H (D14 labels only bits 5/6 that) and unlike bits 5/6 there is a correct answer — the state hash. Reproduced green with the warning at V23. |
| **3 — §4's inference over-stated** | **Fixed.** The comment now states exactly what the warning's absence proves ("no draw took the fallback **while bit 0 was set**") and that with bit 0 clear it proves nothing, because the function returns before the latch and no draw is keyed on a handle either. |
| **4 — process-global identity tables** | **Fixed.** `MagmaPipeIdentityTables` is a `VulkanRenderer` member handed to its factory; the two function-local statics are gone (V21). Both consumer tables were already per-instance, so all four now share one lifetime and one reclamation clock per context. |
| **5 — `553065dc`'s subject overclaims** | **Not fixed; argued.** The instruction for this round is "as new commits on top", and rewriting that message means rebasing the four commits the review cites by sha, invalidating every citation in `magma-review-v2.md` and `magma-v2.md` for a message. The finding is correct and recorded here instead: **`553065dc` contains only comment and `static_assert`s; the dynamic tail's re-sourcing is the contract's, not this commit's** (D-2 says so). Integrator: if you squash at merge, the honest subject is `[Refactor] (Magma): make the chunk table check DynamicTailKey's inventory`. My two new commits describe only what they change. |
| **6 — D19's `DynamicChunksCoverMagmasDynamicTailKey` exists nowhere as a ctest entry** | **Cannot be fixed here; made greppable and routed.** `MG_Test/Pipe/RenderStateSpansTest.cpp` is package A's file (C.5) and `MG_IntegrationTest/**` is package E's, so this package has no file it may put a ctest entry in. The `static_assert` block in `VulkanRenderer.cpp` now names `DynamicChunksCoverMagmasDynamicTailKey` in its comment so `grep` finds it, and says explicitly that the integrator must make sure the outcome is not "neither". **Blocking-for-integrator item, §8.3.** |
| **7 — D17's `AccessorCalls` re-audit not done** | **Done. Result below.** |
| **8 — `build-verify` never configured** | **Fixed.** `build-verify` is configured and built on this tree; unit 1489/1489 and `integration-verify -R DirectVulkan` 409/409 (V1, V10, V17). The verify-only assertions are now exercised here, not just proven to compile. |
| **9 — both assertions are compiled out of every build P2 runs** | **Fixed for the one that could be, and the reason the other cannot is now on record.** The `Gen`-wrap defence is on the **release path**: a slot that reaches generation `2^32-1` is permanently retired (and warned) rather than wrapped. The `MagmaPipeSlotIndex` null check keeps its release-path ternary. **And a new fact the reviewer should have:** a DEBUG-log-level build of this tree — the only configuration in which `MOBILEGL_ASSERT` is live — **does not compile at all**. `MG_Util/Types.h:153,159,170,174` uses `MOBILEGL_ASSERT` where `MGLOG_F` is not declared; the file is untouched since the base ref `48268068` and belongs to no package. Log: `~/w7/p2-magma-r3-build-dbgassert.log` (configure `-DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_DEBUG`, rc 1, first failures in `MG_Util/Converters/MGToGL/ErrorCodeConverter.cpp` and `MG_State/GLState/ErrorState/Error.cpp`, neither of which includes anything this package touches). So **no assertion anywhere in MobileGL is exercised by anything today**. Routed to the integrator, §8.7. |
| **10 — brief-vs-tree items** | Re-confirmed and re-recorded in §6, unchanged. |
| **11 — `MGPipeApplier()` is one process-global** | **Cannot be fixed here; routed and documented at the read site.** `MG_Pipe/PipeApply.cpp` is package A's. A comment at `ResolveBoundRenderStateCso` now names the hazard (a multi-context process reads whatever CSO another context last bound) so neither review can assume the other caught it. **Blocking-for-integrator item, §8.2.** |

### D17 re-audit of the `AccessorCalls` tally constants (minor 7)

D17 names eight sites; all eight have moved and are re-audited at their current lines. The
tallies are unchanged and the audit says why:

| site (now) | tally | what it counts | verdict after m1/m2/m3 |
|---|---|---|---|
| `VulkanRenderer.cpp:5220`, `:5227` | 1 | the single `MGB_CTX->GetPipelineStateVersion()` read at `:5173`, which **both** arms make | **still correct.** The reviewer's suspicion was that the handle arm no longer performs `ComputePipelineStateHash`'s bulk `GetRenderStateParameters()` fetch — it does not, but **`ComputePipelineStateHash` has never carried an `AddCalls` of its own** (`grep -c AddCalls` over its body = 0). So m1's saving is **invisible** to `acc/draw`: the counter *under*-reports the win rather than over-reporting it |
| `:5485` | 15 | the payload-builder walk on a pipeline-memo miss | untouched by this package |
| `:6156`, `:6163` | 1 / 2 | `GetRenderStateParametersVersion()`, and that plus the bulk parameter fetch that builds `DynamicTailKey` | untouched: m2 re-sourced the *value*, not the number of reads |
| `:6685` | 6 | the fast path's six unconditional accessor reads | untouched |
| `:6718` | 1 | the draw-program read, beside the `Draws` denominator | untouched |
| `:6731` | 3 | the full path's three immediate reads | untouched |

**Rule for the report (D17's precondition):** `acc/draw` may be quoted for P2, but it must be
quoted with the note that Magma's largest single removal — one bulk `GetRenderStateParameters()`
per draw whose pipeline-state version moved — is **not in the tally**, so the `acc/draw`
delta is a lower bound on Magma's side. Lead with the gate hit/miss pairs
(`MagmaPipelineMemo`, `MagmaDynamicTail`, `MagmaDrawFastPath`) and the CPU-time series.

---

## 5. Deviations from the brief, with reasons

Carried from v2, still true and unchanged: **D-1** (the enumeration comment did not move to
`MGPipeRenderStateSpans.cpp`, package A's file), **D-2** (the dynamic tail's version gate is
re-sourced but not finer-grained; the tree is right against D12.3 and the reviewer confirmed
it), **D-3** (D12.3's coverage check is `static_assert`s here, not a ctest entry; see minor 6),
**D-4** (`ScissorTestEnabledMask` is pipeline state, so D12.3's blanket claim is false as
written and the assertion is inverted), **D-6** (a VAO's `MGPipeKind` is `VertexElementsCso`),
**D-8** (the three `VertexArrayObject` memo triples are `#if MOBILEGL_PIPE_LEGACY_MEMOS`, not
textually deleted, because a literal deletion changes the pull build), **D-9** (the eviction
epoch is de-globalised, not removed), **D-10** (negative control C lives here although C.4
gives `HandleRecycleScenario` to package E), **D-11** (`MG_Backend/DirectVulkan` no longer
includes `MG_Impl/Pipe/SlotAllocator.h`), **D-12** (D12.1's version→hash gate and cached-hash
fields survive as the no-CSO fallback's cache), **D-13** (the pull build's two pipeline-memo
sites keep the base ref's text under `#else`).

Rewritten or new this round:

* **D-5 (rewritten)** — D12.4 asks for a grow-on-demand `Vector`. **Two of the three tables now
  are grow-on-demand** (the mint and `m_vaoMemos`), which is what the brief asked for; the
  third, `m_vaoDrawMemoTable`, deliberately stays fixed at the base ref's 2048 entries with the
  base ref's victim rule, for the memory and no-regression reasons in §2.1/§2.4. `m_vaoMemos`
  grows through a **chunked** table rather than a `Vector` because a growing `Vector` relocates
  entries the draw path holds references into.
* **D-7 (rewritten)** — Magma still mints its own `{slot, gen}` rather than using
  `MG_Impl/Pipe/SlotAllocator`, for v2 MAJOR 3's unchanged reason (nothing in P2 can call its
  `Free`). What changed is that the mint is now **unbounded-but-reclaimed** instead of
  **bounded-with-eviction**: it has a `Free` of its own, driven by age instead of by a death
  notification. *Integrator: when object deletion reaches the client allocator (P3a/P3b) the
  right end state is still `MGPipeSlots()` with a live `Free` and `MagmaPipeIdentityTable`
  deleted; the age-based sweep is what to delete with it.*
* **D-14 (new)** — an acquisition is no longer "a mask plus at most two `Uint64` compares". It
  is a one-entry front-memo compare for every acquisition after a draw's first (five or six of
  the six a draw makes), and a hash-map probe otherwise. That is still less than the address
  multiply plus two-way probe the pre-handle arm ran, and the front memo lives inside `Acquire`
  so no call site can disagree with another about it.
* **D-15 (new)** — D14's runtime row ("false: the legacy arm is never entered") is not
  literally true for bit 0 with `MOBILEGL_PIPE_LEGACY_MEMOS=0`, because D14's compile row names
  `ComputePipelineStateHash` as part of the pre-handle arm and a no-CSO draw has to key on
  something. Magma warns at startup instead of aborting. See minor 2.
* **D-16 (new)** — D20 says `MGLOG_D` for anything non-critical. The mint's high-water line is
  `MGLOG_I`, at power-of-two milestones from 1024, on the allocate-a-new-slot branch only.
  Reason: `MGLOG_D` is compiled out of every shipped build **and of every build P2 measures**,
  and this line *is* the live-object measurement review v2's M1 asks for (§2.3 is that line
  doing its job on the existing corpus). At most a handful of lines per session; never one on
  a draw, so `ROADMAP.md:7` holds.

---

## 6. Tree-versus-brief contradictions

Unchanged from v2 and independently re-confirmed by the reviewer; repeated so the integrator
has them in one place.

1. **D12.3's premise about `bind_render_state`** — the applier writes both versions and has to
   (Espryt's `SyncRenderState` uses that counter and G5 forbids touching it). Tree wins.
2. **D12.3's claim that every `DynamicTailKey` input is dynamic** — `ScissorTestEnabledMask` is
   pipeline state. Tree wins.
3. **D12.1 lists three cached-hash fields; there are five.**
4. **D12.1 does not mention the second memo probe** in `TrySetupDrawFastPath`.
5. **Section A's G2/G14 grep is wrong.** `grep -E '^\s+Test #'` matches only 4-digit ids
   (`ctest -N` right-aligns) and returns 1368 of 2367 names here. The working form is
   `grep -E '^\s+Test\s+#[0-9]+:' | sed -E 's/^ *Test *#[0-9]+: //'`. **Fix `BRIEF-P2.md`
   before `wsl_integrate_p2.sh` uses it.**
6. **G1/D15's admitted resize set is short by one.** `_GLOBAL__sub_I_DirectGLES.cpp` (−9) is a
   fourth resized symbol and it is the **contract's** (V3 proves this package adds none). D15
   admits three, so G1 is red by its own text after every merge until D15 is amended.
7. **C.3's verification block** prints `grep -rn 'BackendStateMemo…' # only ProgramObject's`.
   With D-8 in force the honest check is "every hit is inside a `MOBILEGL_PIPE_LEGACY_MEMOS`
   arm, plus `ProgramObject`'s" (V20).
8. **`MGLOG_F_ONCE` does not exist** (`MG_Util/Debug/Log.h` has `_ONCE` for D/I/W/E only).
9. **G3's command passes `--ssim 0.99` and C.3's passes `--backend`; `~/w7/retrace_gate.py`
   has neither option** (`--tree --lib --out -j --only` only) and exits 2. Its built-in
   threshold ran instead. Fix the brief or the script before D.3.
10. **`MOBILEGL_PIPE_PUSH=0x20` with `MOBILEGL_PIPE_LEGACY_MEMOS=0` is fatal to a DirectVulkan
    run at startup, naming bit 6.** That is D14's own semantics, not a defect: a per-subsystem
    A/B with the legacy lever off must set the bit belonging to the backend under test.
11. **New this round:** a DEBUG-log-level build of `feat/disaggregated` does not compile
    (`MG_Util/Types.h`, untouched since the base ref). Every `MOBILEGL_ASSERT` in the tree is
    therefore dead code in every buildable configuration. See minor 9 and §8.7.

---

## 7. Flakes

None this round. Thirteen `-j 4` integration runs (six configurations on the delivered tree,
one verify lane, four probe runs on the armed tree, two restores) produced no intermittent
failure. v2's two `…IsActuallyArmedWhenTheEnvironmentPinsItOn` flakes did not recur.

---

## 8. Unfinished, and what the integrator must carry

1. **G11 / D.4.2 is still not run.** Nothing in this package can run it. What this round adds
   is that M1 no longer *depends* on it: the structure has no capacity to be sized, and the
   two workloads' live-object counts are now readable from a shipped INFO build's log
   (`MagmaPipeIdentityTable(...): high-water N slots minted, M live`). Please grep for that
   line in the device run and put the two numbers in `MEASUREMENTS.md`.
2. **Blocking, package A: `MG_Pipe::MGPipeApplier()` is one process-global `g_applier`**
   (`PipeApply.cpp:99,112`), not the per-context CSO store D2 specifies. Magma reads it
   directly and is its only P2 consumer, so in a multi-context process one renderer's pipeline
   memo keys on whatever CSO another context last bound. Named in a comment at
   `ResolveBoundRenderStateCso`; the fix belongs in package A.
3. **Blocking, D19 outcome:** `DynamicChunksCoverMagmasDynamicTailKey` is `static_assert`s in
   `VulkanRenderer.cpp` here and a named ctest case in package A's `RenderStateSpansTest.cpp`
   there. Make sure the outcome is not "neither"; both is fine and intended.
4. **G8 / `HandleRecycleScenario` is the only thing that covers the `Gen` discipline.** Red
   probe B (§2.5) shows the 432 integration cases do not. Package E owns it.
5. **D-5 / D-7:** revisit the mint when object deletion reaches the client allocator (P3a/P3b).
6. **D-2:** decide whether to add a dynamic-only version to the wire. **D-1:** the enumeration
   comment's move into `MGPipeRenderStateSpans.cpp`.
7. **`MOBILEGL_ASSERT` is dead everywhere.** A DEBUG build of the tree does not compile
   (`MG_Util/Types.h` uses it before `MGLOG_F` is declared). Either fix that include order or
   stop pretending the assertions are a defence. Not this package's file; one-line fix.
8. **Brief fixes:** §6.5 (the G2/G14 grep), §6.9 (`--ssim` / `--backend`), §6.6 (amend D15's
   admitted resize set with `_GLOBAL__sub_I_DirectGLES.cpp`).
9. **`553065dc`'s subject** overclaims — see minor 5 for the honest one if you squash.
10. **A `check_include_closure.py` probe rooted at an `MG_Backend` header** would actually gate
    `ARCHITECTURE.md:9`; today G13 says nothing about it either way. The script is not this
    package's file.
11. **`MEASUREMENTS.md` input from this package.** The pipeline memo compares an 8-byte handle
    instead of an 8-byte hash and skips `ComputePipelineStateHash` entirely (≈50
    `CombinePipelineStateWord` rounds **plus one bulk `GetRenderStateParameters()` fetch that
    no `AccessorCalls` tally counts** — see the D17 re-audit) on every draw whose
    pipeline-state version moved. `LookupVaoDrawMemo` drops a multiply and one of two compares
    and, below 2048 live VAOs, never collides at all (0.0 % vs the base ref's 6.4 % at 512 and
    24.0 % at 1024 — §2.4). New steady-state cost: one hash-map probe per draw per kind behind
    a one-entry memo, and ~100 B per live VAO plus ~50 B per live buffer of mint and memo
    storage, reclaimed when the object goes idle. None of it is measured on hardware here; that
    is D.4.
12. **The armed tree** (`~/w7/p2-magma-armed`, `tmp/magma-armed` @ `840f8026`) is scaffolding
    and can be deleted after the real integration.
