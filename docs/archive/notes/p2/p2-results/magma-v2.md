# P2 package D — `p2/magma` result (v2, rework round 2)

Tree: `~/w7/p2-magma`, branch `p2/magma`, from the contract commit `9c6a8a25` (tag
`p2/contract`). Not pushed. Build dirs `build-linux` (pull, the G1 build), `build-push`,
`build-nolegacy` (`-DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF`).

One extra tree exists for verification only: `~/w7/p2-magma-armed`, branch
`tmp/magma-armed` = `p2/magma` merged with `p2/tracker` (clean merge, no conflicts — which
is itself evidence for C.5's ownership split). **It is not a deliverable**; it exists because
m1 cannot be exercised on a tree with no tracker to bind a render-state CSO (review MAJOR 5),
and it is where the arming evidence in §4 was taken. No file in any other package's tree was
edited.

## 1. Commits

| sha | subject |
|---|---|
| `d66f400f` | `[Refactor] (Magma): key the pipeline memo on the render-state CSO handle and stop recomputing a hash the client already computed` |
| `553065dc` | `[Refactor] (Magma): drive the dynamic tail from the pushed dynamic version and make the chunk table check DynamicTailKey's inventory` |
| `391e1230` | `[Refactor] (Magma): key the vertex-input cache and the VAO draw memo on {slot, gen} instead of a lifetime id and a heap address` |
| `fb410351` | `[Refactor] (State, Magma): take the backend's raw pointers out of the frontend VAO - the hash and state memos become the factory's own per-slot fields` |
| **`a0e7c93d`** | **`[Fix] (Magma): make the legacy-memo lever a startup gate for Magma's own bit, bound the {slot, gen} mint, and keep the all-pull arm free of push-only cost`** |
| **`1e89fd90`** | **`[Fix] (Magma): assert rather than assume that a handle indexed into a per-slot table is non-null`** |

The first four are v1's, unchanged. The last two are this round. Files touched are the same
six as v1 — `MG_Backend/DirectVulkan/Renderer/{MagmaPipeArms.h, VulkanRenderer.h,
VulkanRenderer.cpp, VertexInputStateFactory.h, VertexInputStateFactory.cpp}` and
`MG_State/GLState/VertexArrayState/VertexArrayObject.h` — all this package's by C.5.

## 2. How each major was fixed

### MAJOR 1 — `MOBILEGL_PIPE_LEGACY_MEMOS=0` aborted every draw through a non-Track-H bit

`MagmaPipeRequireLegacyArm` (per-draw `MGLOG_F` + `std::abort()`) is **deleted**. In its place
`MagmaPipeValidateSubsystemConfiguration()` runs **once, from `VulkanRenderer::Initialize()`**,
which is what D14 actually specifies ("a Track-H subsystem whose bit is clear is a **startup**
`Fatal{PipeLegacyMemosDisabled}`"). Three boundaries, each of which the per-draw shape got
wrong:

* it checks **only bit 6** (`kMGPipeSubsystemMagmaVertexInput`), and only when Magma is the
  backend being brought up, so Espryt's bit 5 cannot kill a DirectVulkan run and vice versa;
* **bit 0 is not consulted at all.** It is not Track H (D14 labels only bits 5/6 that) and it
  is not a memo re-key — it decides where the pipeline memo's *state key* comes from. A clear
  bit 0 now silently selects the state hash;
* the "no CSO bound" case is a **fallback, not an abort**, in every build (see below).

Every re-keyed Track-H site now asks one helper, `MagmaPipeTrackHArmIsHandles(bit)`, which is
`MagmaPipeSubsystemOn(bit)` where the legacy arm is compiled and a compile-time `true` where it
is not (the startup gate has already made a clear bit fatal there). That also removes minor 3:
`VertexInputStateFactory::ComputeHash` was the one site deciding for itself.

Reproduction of the review's exact commands, on the final commit:

| command | v1 | v2 |
|---|---|---|
| `MOBILEGL_PIPE_PUSH=0x60 MOBILEGL_PIPE_LEGACY_MEMOS=0 ctest -L integration-gpu -j 4 -R DirectVulkan` | 9/9 "Subprocess aborted" on the CrossFrameBuffer subset | **432/432** |
| `MOBILEGL_PIPE_LEGACY_MEMOS=0` at the default mask | aborted | **432/432** |
| `MOBILEGL_PIPE_PUSH=0x20 MOBILEGL_PIPE_LEGACY_MEMOS=0` (Espryt's bit alone) | aborted mid-draw | startup Fatal naming bit 6, the harness's pre-flight child catches it and the case **SKIPS visibly** with "no usable GPU/display/ICD for backend DirectVulkan" — the D14 semantics, loud, before any frame |

**The no-CSO fallback.** `delete_render_state` clears `BoundRenderStateCso`
(`MG_Pipe/PipeApply.cpp`), so the null handle is reachable on *any* tree, integrated or not.
The fallback is the pre-handle `ComputePipelineStateHash` where one is compiled, and the
client's own `MGPipeComputePipelineSubsetHash` over the same 396 pipeline bytes where it is
not (`ComputePipelineSubsetStateHashFallback`). That is what makes
`-DMOBILEGL_PIPE_LEGACY_MEMOS=OFF` a **runnable** configuration: v1's `build-nolegacy` was
180/432 with `Fatal{PipeLegacyMemosDisabled}` on every draw, v2's is **432/432**.

The fallback logs `MGLOG_W_ONCE` rather than `MGLOG_D_ONCE`. W is compiled in at every shipped
log level and `_ONCE` costs one static bool test, so a run whose pipeline memo never keys on a
CSO handle now says so in its log — which is the thing that made MAJOR 5 invisible, and which
§4 turns into the arming evidence.

### MAJOR 2 — the all-pull control arm was contaminated

`snap.vaoHandle = ResolveVaoHandle(vao)` at both stamping sites is now inside
`if (MagmaPipeTrackHArmIsHandles(kMGPipeSubsystemMagmaVertexInput))`, so
`MOBILEGL_PIPE_PUSH=0` mints nothing and compares nothing for a field that arm never reads.
D14's "reproduces P1's behaviour exactly" holds again, and D.4.3's T2 is back to meaning "P1's
residual fill alone".

### MAJOR 3 — unbounded, never-reclaimed growth of the client slot allocator

Magma no longer uses `MG_Impl/Pipe/SlotAllocator` at all. `grep -rn "MGPipeSlots()" MobileGL
--include=*.cpp --include=*.h | grep -v MG_Impl/Pipe/SlotAllocator` now returns **one comment
line and no code**. The reason the allocator was wrong here is not a defect in the allocator:
nothing in P2 can call its `Free`. There is no frontend death notification Magma can hook —
`BufferBackendOps::OnDestroy` is handed a `BackendBufferResource`, not the `BufferObject`, and
only fires for a buffer that ever had one; `VertexArrayObject` has no hook at all, and adding
one is D13's explicit-destroy work, which covers Espryt's six kinds and not
`VertexElementsCso`.

In its place, `MagmaPipeIdentityTable` (in `MagmaPipeArms.h`): fixed capacity, 2-way
set-associative, indexed by lifetime id, LRU victim inside the set, `Gen` incremented whenever
a slot changes owner. Two instances — 2048 entries for `VertexElementsCso` (32 KB), 8192 for
`Buffer` (128 KB). Bounded, nothing to reclaim, and exactly as ABA-proof as the allocator's
`{slot, gen}`: a recycled slot always carries a moved generation, so a stale handle cannot
match. It also takes `MG_Backend`'s only `MG_Impl` include back out (minor 4) and turns the
per-attribute hash-map probe into an array index (minor 2).

### MAJOR 4 — the density premise was false and the mitigations had been deleted

Both per-slot tables are now a **bijection** with the mint rather than a masked direct map:
`kVaoDrawMemoSlotCount == kMagmaVaoIdentityEntries` and
`VertexInputStateFactory::kVaoMemoSlotCount == kMagmaVaoIdentityEntries` are `static_assert`s,
entry *i* is slot *i + kMGPipeFirstAllocatableSlot*, and the index is
`MagmaPipeSlotIndex(handle)` with no mask. Two live VAOs cannot land on one entry at all, so
the birthday-collision scenario the review constructed cannot happen. The mitigations are not
gone, they moved one level down and are now written once: the second candidate is the
identity table's second way, and the frame-serial victim choice is its LRU. The comments that
argued the false density claim are rewritten.

The residual cost this design does carry, stated plainly: with more live objects than the
table holds, an LRU eviction changes a live object's handle. For a VAO that costs a memo
recompute (both tables are caches; correctness is the exact `{slot, gen}` compare). For a
**buffer** it changes the vertex-input *content* hash, which costs a rebuilt
`BackendVertexInputState` — but `ComputeHash` is memoised per VAO on its config version, so
that is paid per VAO reconfiguration, not per draw, and the buffer table is sized 4× the VAO
one for it. It is not measured here; nothing on desktop reaches those working sets.

### MAJOR 5 — m1 was executed by no test

Fixed by verification, not by code: `~/w7/p2-magma-armed` = `p2/magma` + `p2/tracker` (D.1's
own order), where the tracker emits `create_render_state`/`bind_render_state` and the applier
publishes `BoundRenderStateCso`. See §4 for the four runs; the short version is that on that
tree **432/432 integration cases and all 40 DirectVulkan retraces run with zero fallback
warnings**, i.e. every draw's pipeline memo was keyed on the CSO handle, and a red probe that
collapses the handle to a constant turns 4 of 59 render-state-sensitive scenarios red.

## 3. Verification — every command and its actual result

All from `~/w7/p2-magma` at `1e89fd90` unless stated.

| # | command | result |
|---|---|---|
| V1 | `cmake --build build-linux -j 12` | rc 0 |
| V2 | `cmake --build build-push -j 12` | rc 0 |
| V3 | `cmake --build build-nolegacy -j 12` | rc 0 |
| V4 | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; the four are the **contract's** (`RenderState::RenderState()` +148, `SetCapability` +77, `IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9). **This package adds none.** (G1) |
| V5 | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty (G13) |
| V6 | `python3 scripts/check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)` (G13) |
| V7 | `python3 scripts/gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 (G13) |
| V8 | stdio grep over `MG_Backend/DirectVulkan` + `MG_State/.../VertexArrayState` | none (G13) |
| V9 | `awk '/namespace RenderStateImpl {/,/} \/\/ namespace RenderStateImpl/' DirectGLES.cpp \| sha256sum` | `d8fd1c48…0efe27`, equal to `~/w7/p2-before-syncrenderstate.sha` (G5) |
| V10 | `ctest -N` names, corrected grep (§6.5), `build-{linux,push,nolegacy}` | 2367 / 2367 / 2367, **all three identical**; vs the baseline: **0 removed**, 4 added and all four the contract's `MG_Test/Pipe` placeholders (G2 name half, G14) |
| V11 | `ctest -L unit` in `build-linux` / `build-push` / `build-nolegacy` | **1489/1489** in each |
| V12 | `ctest -L integration-gpu -j 4 -R DirectVulkan`, `build-push`, default mask | **432/432** |
| V13 | same, `MOBILEGL_PIPE_PUSH=0` | **432/432** (one flake on the first run, see §7; clean on rerun) |
| V14 | same, `MOBILEGL_PIPE_PUSH=0x60 MOBILEGL_PIPE_LEGACY_MEMOS=0` — **the review's MAJOR 1 repro** | **432/432** (was 9/9 aborted) |
| V15 | same, `MOBILEGL_PIPE_LEGACY_MEMOS=0` at the default mask | **432/432** |
| V16 | same, `build-nolegacy` | **432/432** (was 180/432, every failure a per-draw Fatal) |
| V17 | same, `build-linux` (pull) | **432/432** (one flake on the first run; clean on rerun) |
| V18 | `retrace_gate.py --tree ~/w7/p2-magma --lib build-push/libMobileGL.so --out ~/w7/retrace-out/p2-magma-r2f -j 4 --only 'DirectVulkan$'` | **40/40 PASS**, min SSIM **0.993475** (`minecraft-1.21.11-main-menu`), gate 0.99 (G3, DirectVulkan half) |
| V19 | `grep -rn "MGPipeSlots()" MobileGL … \| grep -v MG_Impl/Pipe/SlotAllocator` | one comment line, **no code** (MAJOR 3) |
| V20 | `grep -rn "include <MG_Impl" MobileGL/MG_Backend` | only the pre-existing `DirectGLES.cpp:22`; **this package's is gone** (minor 4) |
| V21 | `grep -rn 'BackendStateMemo\|BackendAuxMemo\|m_backendHashMemo' MobileGL/MG_State MobileGL/MG_Backend` | 15 hits outside `ProgramObject`, every one inside a `MOBILEGL_PIPE_LEGACY_MEMOS` arm (D-8) |
| V22 | `MOBILEGL_PIPE_PUSH=0x20 MOBILEGL_PIPE_LEGACY_MEMOS=0`, one DirectVulkan case, `MOBILEGL_LOG_FILE_PATH` | one `Fatal{PipeLegacyMemosDisabled}` naming bit 6 **at startup**; ctest reports a visible SKIP, not a mid-frame abort |

`retrace_gate.py` has no `--ssim` option on this tree (the brief's G3 line passes one); the
script's built-in threshold is what ran. Recorded as a brief-vs-tree difference, not a
deviation.

## 4. The m1 arming evidence (`~/w7/p2-magma-armed`, `tmp/magma-armed`)

`p2/magma` merged with `p2/tracker` — the merge is clean, which is the first datum: C.5's
file split holds between D and B.

| # | run | result |
|---|---|---|
| A1 | `ctest -L integration-gpu -j 4 -R DirectVulkan`, default mask | **432/432** |
| A2 | 13 `CrossFrameBufferScenario` cases with `MOBILEGL_LOG_FILE_PATH` | 13/13; occurrences of the fallback warning: **0** |
| A3 | the same 13 on `~/w7/p2-magma` (no tracker) — the control | 13/13; fallback warning: **1** |
| A4 | `retrace_gate.py … --only 'DirectVulkan$'` on the armed tree | **40/40 PASS**, min SSIM 0.993475; fallback warning across all 40 logs: **0** (on `~/w7/p2-magma` the same grep hits **80** files) |
| A5 | RED PROBE: `ResolveBoundRenderStateCso` patched to return a constant `{1,1}`, rebuilt, `-R 'DirectVulkan.*(Blend\|Stencil\|Depth\|CrossFrameBuffer\|Clear)'` | **4 of 59 fail** — `LayeredCubeMapArrayDepthStencilAttachmentGatesEveryLayerFace`, both `RenderbufferBlendFormatScenario` cases, and one more. Patch reverted, rebuilt: **59/59**. |

A2+A3+A4 say the CSO handle arm ran for every draw of 432 integration cases and 40 retraces,
and that the same binary shape makes the *absence* of the arm observable. A5 is the
`ROADMAP.md:7` half: the gate can go red for m1's reason — if the CSO key stops
discriminating render states, the memo hands back the wrong pipeline and the pixels move.

What this still does **not** cover: `build-verify` (no verify build on either tree; C.3 asks
for none), the DirectGLES half of G3, and G6–G12 / G11 / all of D.4.

## 5. Deviations from the brief, with reasons

Carried from v1, still true:

* **D-1** — `ComputePipelineStateHash`'s 20-line enumeration comment did **not** move to
  `MGPipeRenderStateSpans.cpp` (D12.2). That file is package A's.
* **D-2** — the dynamic tail's version gate is re-sourced but not finer-grained. D12.3 says
  `GetRenderStateParametersVersion()` stops moving on a pipeline-only change; it does not,
  because `MGPipeApplyBindRenderState` writes both versions and has to (Espryt's
  `SyncRenderState` uses that counter as its all-state detector and G5 forbids touching it).
  A dynamic-only version on the wire is a contract change. The reviewer independently
  confirmed the tree is right here.
* **D-3** — D12.3's coverage check is `static_assert`s in `VulkanRenderer.cpp`, not the
  `RenderStateSpansTest.cpp` entry D19 names; that file is package A's. **Integrator: make
  sure the outcome is not "neither"** (review minor 5).
* **D-4** — `ScissorTestEnabledMask` is pipeline state, so D12.3's "every field
  `DynamicTailKey` reads is dynamic" is false as written; pinned with the assertion inverted.
  Independently confirmed by the reviewer.
* **D-6** — a VAO's `MGPipeKind` is `VertexElementsCso`; there is no `VertexArray` kind.
* **D-8** — the three `VertexArrayObject` memo triples are `#if MOBILEGL_PIPE_LEGACY_MEMOS`,
  not textually deleted (D12.5 says "deleted outright"): a literal deletion changes the pull
  build, which G1 admits no resize of. `build-nolegacy` is where the deletion is real.
* **D-9** — the eviction epoch is de-globalised, not removed.
* **D-10** — negative control C (`MOBILEGL_PIPE_HANDLE_ABA_CONTROL`) is implemented here
  although C.4 gives `HandleRecycleScenario` to package E: D18 defines the knob in terms of
  two guards that live in this package's files.

New or changed this round:

* **D-5 (rewritten)** — the two per-slot tables stay **fixed** where D12.4 asks for a
  grow-on-demand `Vector`, and they are now a *bijection* with a fixed-capacity mint rather
  than a masked map. Grow-on-demand only makes sense against an allocator that frees, and
  nothing in P2 frees a `VertexElementsCso` slot. *Integrator: revisit when object deletion
  reaches the client allocator (P3a/P3b) — at that point the mint should become
  `MGPipeSlots()` again and both tables should be sized off `HighWater()`.*
* **D-7 (rewritten)** — Magma mints its own `{slot, gen}` from a **backend-local, bounded,
  self-recycling** identity table rather than from `MG_Impl/Pipe/SlotAllocator`. It was
  already a P2 stand-in (the tracker emits no object-class state, so no client handle reaches
  the backend); what changed is that the stand-in is now bounded and does not leak. The
  earlier claim that "both acquisition sites sit behind a one-entry memo" was wrong — there
  are three sites and one had no memo (review minor 2). There is now no memo at any of them
  and none is needed: an acquisition is a mask plus at most two `Uint64` compares, which is
  less than the address multiply and two-way probe the pre-handle arm ran.
* **D-11 (retired)** — `MG_Backend/DirectVulkan` no longer includes `MG_Impl/Pipe/SlotAllocator.h`.
  The conflict the reviewer named (`ARCHITECTURE.md:9` — the server process links `MG_Backend`
  + the MGPipe object table and **not** `MG_State`, `MG_Impl`, glslang) is therefore gone. The
  remaining push-only include from this package is `MG_Pipe/PipeApply.h`, which is the server
  half of the calls and is on the right side of that line. Note `check_include_closure.py`
  still has no probe rooted at an `MG_Backend` header, so G13 says nothing about this either
  way — worth a probe, and recorded as a suggestion, not done here (the script is not this
  package's file).
* **D-12 (new)** — D12.1 says the version→hash gate and the cached-hash fields "are deleted".
  They are not: they are the no-CSO fallback's cache, and that fallback is structurally
  required because `delete_render_state` clears the binding. On a tree whose tracker binds a
  CSO the five words are written once and never read again; they retire with the pull path at
  P13. This is a direct consequence of fixing MAJOR 1's second guard.
* **D-13 (new)** — the pull build's two pipeline-memo sites keep the base ref's text
  statement for statement under `#else`. Routing the pull build through the new inline helper
  instead resized `GetOrCreatePipeline` (+104) and `TrySetupDrawFastPath` (−112) — a real G1
  red that this structure removes. Cost: ten duplicated lines, twice.

## 6. Tree-versus-brief contradictions

1. **D12.3's premise about `bind_render_state`** — see D-2. The brief's model of the applier
   does not match `PipeApply.cpp` as landed. (Reviewer's minor 7 agrees; the tree wins.)
2. **D12.3's claim about `DynamicTailKey`'s inputs** — see D-4. (Reviewer's minor 6 agrees.)
3. **D12.1 lists three cached-hash fields; there are five** (`m_pipelineStateHash{,Valid,
   Version,ColorCount,SampleCount}`).
4. **D12.1 does not mention the second memo probe** in `TrySetupDrawFastPath`. It exists and
   is re-keyed identically; leaving it on the hash would have been a silent arm mismatch.
5. **G2/G14's shell command in section A is wrong.** `grep -E '^\s+Test #'` matches only
   4-digit ids (`ctest -N` right-aligns), returning 1368 of 2367 names here. The working form
   is `grep -E '^\s+Test\s+#[0-9]+:' | sed -E 's/^ *Test *#[0-9]+: //'`. **Fix `BRIEF-P2.md`
   before `wsl_integrate_p2.sh` uses it.** (Reviewer's minor 8 agrees.)
6. **G1's admitted resize set is short by one.** `_GLOBAL__sub_I_DirectGLES.cpp −9` is a
   fourth resized symbol and it is the **contract's** — identical against
   `~/w7/p2-contract/build-linux/libMobileGL.so`. D15 admits three, so G1 is red by its own
   text after every merge until D15 is amended. Integrator's call, not this package's defect.
   (Reviewer's minor 1.)
7. **C.3's verification block** prints `grep -rn 'BackendStateMemo…' # only ProgramObject's`.
   With D-8 in force the honest check is "every hit is inside a `MOBILEGL_PIPE_LEGACY_MEMOS`
   arm, plus `ProgramObject`'s" (V21).
8. **`MGLOG_F_ONCE` does not exist** (`MG_Util/Debug/Log.h` has `_ONCE` for D/I/W/E only).
9. **G3's command passes `--ssim 0.99`; `~/w7/retrace_gate.py` has no such option** and exits
   2. Its own threshold ran instead. Fix the brief or the script before D.3.
10. **`MOBILEGL_PIPE_PUSH=0x20` (Espryt's bit alone) with `MOBILEGL_PIPE_LEGACY_MEMOS=0` is
    still fatal to a DirectVulkan run** — but now at startup, naming bit 6, because with bit 6
    clear and the legacy arm forbidden Magma's vertex-input site genuinely has no arm. That is
    D14's own semantics, not a defect; it means a per-subsystem A/B with the legacy lever off
    must set the bit belonging to the backend under test.

## 7. Flakes

`DirectVulkan.PrimGenReroute…TheRerouteIsActuallyArmedWhenTheEnvironmentPinsItOn` and
`…PointSizeDemotion…TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn` failed once each in
four of this round's `-j 4` runs (once in `build-nolegacy`, once in `build-push
MOBILEGL_PIPE_PUSH=0`, once in `build-linux`, twice in the armed tree's first run) and passed
every time in isolation (3× each) and on every full rerun. `build-linux` is byte-identical to
the baseline apart from the contract's four resizes, so they cannot be this package's.
Load-sensitive lavapipe flake. *Integrator: worth a serial label or a retry-on-failure in
`integration-gpu` generally.* (The reviewer could not reproduce it in v1; this round did, four
times, which supports "load-sensitive" rather than "does not exist".)

## 8. Unfinished, and follow-ups for the integrator

1. **`build-verify` was never run for this package** (C.3 asks for none, and neither tree has
   one). D.3 part 2 remains the authority for G4.
2. **The armed tree is verification scaffolding.** `tmp/magma-armed` / `~/w7/p2-magma-armed`
   can be deleted after the real integration; keep it only if the D.3 evidence is wanted
   before B and D are both merged.
3. **D-5 / D-7**: revisit the mint when object deletion reaches the client allocator. The
   right end state is `MGPipeSlots()` with a live `Free`, both tables sized off `HighWater()`,
   and `MagmaPipeIdentityTable` deleted. Nothing else in Magma changes.
4. **D-12**: once the tracker is landed and the fallback is provably unreachable in a shipped
   configuration, the five cached-hash fields can go — but not before, and the
   `MGLOG_W_ONCE` is the thing that tells you.
5. **D-2**: decide whether to add a dynamic-only version to the wire.
6. **D-1**: the enumeration comment's move into `MGPipeRenderStateSpans.cpp`.
7. **D-3 / D19**: `DynamicChunksCoverMagmasDynamicTailKey` — `static_assert`s here, no ctest
   entry; make sure package A's file does not also drop it.
8. Fix `BRIEF-P2.md`'s G2/G14 grep (§6.5) and G3's `--ssim` argument (§6.9).
9. Amend D15's admitted resize set with `_GLOBAL__sub_I_DirectGLES.cpp` (§6.6).
10. Consider a `check_include_closure.py` probe rooted at an `MG_Backend` header, so
    `ARCHITECTURE.md:9` is actually gated (§5, D-11).
11. **`MEASUREMENTS.md` input from this package**: the pipeline memo compares an 8-byte handle
    instead of an 8-byte hash and skips `ComputePipelineStateHash` entirely (~50
    `CombinePipelineStateWord` rounds plus one bulk parameter fetch) on every draw whose
    pipeline-state version moved; `LookupVaoDrawMemo` drops a multiply, a two-way probe and one
    of two compares; `VertexInputStateFactory` drops one `mutable` write per VAO
    reconfiguration. New fixed cost: 160 KB of identity tables in a push build. None of it is
    measured here — that is D.4.
12. **Not measured, and it cannot be measured on desktop**: the identity tables' eviction
    behaviour above 2048 live VAOs / 8192 live buffers (§2, MAJOR 4). If the device A/B shows
    an unexplained per-draw cost in a chunk-cycling world, that is the first thing to size up.
