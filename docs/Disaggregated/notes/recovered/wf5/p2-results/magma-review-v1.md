# P2 package D — `p2/magma` adversarial review (v1)

Reviewer ran everything below in `~/w7/p2-magma` (branch `p2/magma`, 4 commits on top of
`9c6a8a25` = tag `p2/contract`). Tree left exactly as found: no file edited, no commit, no
`git` state change. Incremental rebuilds only (`cmake --build`, rc 0, no source changed);
`ctest` wrote its usual `Testing/Temporary` output; scratch artefacts went to `/tmp` and to
`scratchpad/wf5/rev-magma/`.

**Verdict: NOT APPROVED — 4 majors.**

---

## 0. What I re-ran, and what actually passed

Independent re-runs (not quotations of the result file):

| gate | command | observed |
|---|---|---|
| G1 | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; the 4 resized are `RenderState::RenderState() +148`, `RenderState::SetCapability +77`, `RenderState::IsCapabilityEnabled +29`, `_GLOBAL__sub_I_DirectGLES.cpp -9`. Same four, byte-for-byte, against `~/w7/p2-contract/build-linux/libMobileGL.so` → **this package adds none**. |
| G5 | `awk '/namespace RenderStateImpl \{/,/\} \/\/ namespace RenderStateImpl/' MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp \| sha256sum` | `d8fd1c48…0efe27`, equal to `~/w7/p2-before-syncrenderstate.sha` |
| G14 | `ctest -N` (corrected grep) vs `~/w7/p2-before-ctest-names.txt` | 2367 names; **0 removed**; 4 added, all four the contract's `MG_Test/Pipe` placeholders |
| G2 (name half) | `diff` of `ctest -N` names across `build-linux` / `build-push` / `build-nolegacy` | identical in all three |
| unit | `ctest -L unit` in `build-linux`, `build-push`, `build-nolegacy` | **1489/1489** in each |
| integration | `ctest -L integration-gpu -j 4 -R DirectVulkan` on `build-push` | **432/432**; with `MOBILEGL_PIPE_PUSH=0` **432/432**; with `MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_HANDLE_ABA_CONTROL=1` **432/432** |
| G3 (DirectVulkan half) | `python3 ~/w7/retrace_gate.py --tree ~/w7/p2-magma --lib build-push/libMobileGL.so --out … -j 4 --only 'DirectVulkan$'` | **40/40 PASS**, min ssim 0.997009 (`minecraft-1.21.4-main-menu`) |
| G13 | `grep -rc pGLContext MobileGL/MG_Backend \| grep -v ':0$'` empty; `check_include_closure.py` → `4 probes, 0 skipped, 0 problem(s)`; `gen_pipe.py --check` / `--self-test` rc 0/0; the CI stdio alternation over `MG_Backend`+`MG_State` → clean | all green |
| G9 | `gen_pipe_dirty_surface.py --check` / `--self-test` | rc **2** / rc **2** — package B's deliverable, not this one's; recorded so it is not read as green here |

I also verified two load-bearing claims the package makes but does not prove, and both hold:

- **m1's "the CSO subset is a strict superset of `ComputePipelineStateHash`'s inputs".** I built
  an out-of-tree `offsetof` probe against `MG_Pipe/MGPipeValueTypes.h` +
  `MG_Pipe/MGPipeRenderStateSpans.h`
  (`scratchpad/wf5/rev-magma/probe.cpp`, compiled with `build-push`'s own include/define set).
  Every field `ComputePipelineStateHash` reads (`VulkanRenderer.cpp:4952-5041`) lands inside a
  **pipeline** chunk — `CullFaceEnabled@824`, `DepthTestEnabled@540`, `DepthMask@548`,
  `ColorLogicOpEnabled@880`, `MultisampleEnabled@885`, `PolygonOffsetFillEnabled@886`,
  `PrimitiveRestartEnabled@890`, `PrimitiveRestartFixedIndexEnabled@891`,
  `RasterizerDiscardEnabled@892`, `SampleMaskEnabled@896`, `SampleShadingEnabled@897`,
  `StencilTestEnabled@898`, `SampleMaskValue@760`, `MinSampleShadingValue@764`,
  `PatchVertices@264`, `PatchDefaultOuterLevel@268`, `PatchDefaultInnerLevel@284`,
  `PolygonModeFront@868`, `CullFaceModeSetting@828`, `DepthFunc@544`, `LogicOp@536`,
  `BlendStates@312`, `ColorMasks@549` — with `StencilStates@768` straddling at exactly the
  documented sub-member granularity (`Func`/ops pipeline, `Ref`/masks dynamic).
- **"`renderPassHash` keeps `colorAttachmentCount` / `rasterizationSamples` in the key".** True:
  `VkRenderPassManager.cpp:617,626` fold the draw-buffer set and the valid draw-buffer count,
  `:661,728` fold each attachment's `sampleCount`. So collapsing the state hash to the CSO
  handle loses no discrimination.
- **The chunk table itself.** I re-derived all 45 `RenderState::Set*` definitions
  (brace-balanced body extraction, `scratchpad/wf5/rev-magma/s38.sh`) and cross-checked each
  against the probe's pipeline/dynamic verdict. **No G7 violation found.** Spot-verified by
  hand: `SetSampleCoverage` (`RenderState.cpp:786`, `BumpVersions`) → `SampleCoverageValue@752`
  pipeline ✓; `SetPolygonMode` (`:174`, `BumpVersions`) → `PolygonModeFront@868` /
  `PolygonModeBack@872` pipeline ✓; `SetStencilFunc` (`:637`) bumps `m_pipelineStateVersion`
  only when `Func` moves and `Func` is the pipeline sub-member ✓; `SetPrimitiveRestartIndex`
  (`:189`) is `++m_version` only and `PrimitiveRestartIndex@876` is dynamic ✓;
  `SetCapability(ClipDistance0..7)` (`:363-381`) deliberately avoids `BumpVersions` and
  `ClipDistanceEnabledMask` is dynamic ✓; `SetClearColor` (`:707`) / `SetLineWidth` (`:109`) /
  `SetBlendColor` (`:740`) / `SetViewport` (`:69`) / `SetScissorBox` (`:926`) no bump, all
  dynamic ✓.

So the package's arithmetic is right. What it does not survive is the four findings below.

---

## MAJOR 1 — `MOBILEGL_PIPE_LEGACY_MEMOS=0` is now unusable at bit granularity: a clear **bit 0** aborts every DirectVulkan draw, from a per-draw site, and that is the exact lever G8's `.Handles` arm pulls

`ResolveBoundRenderStateCso()` (`MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.h:891-903`):

```cpp
        MG_Pipe::MGPipeHandle ResolveBoundRenderStateCso() const {
            if (!MagmaPipeSubsystemOn(MG_Pipe::kMGPipeSubsystemRenderState)) {
                MagmaPipeRequireLegacyArm("GetOrCreatePipeline");      // :893
                return MG_Pipe::kMGPipeNullHandle;
            }
```

`MagmaPipeRequireLegacyArm` (`MagmaPipeArms.h:50-62`) is `MGLOG_F` + `std::abort()` when
`MG_Config::Features.PipeLegacyMemos` is false.

Two things are wrong with routing bit 0 through that lever.

1. **Bit 0 is not a Track-H subsystem.** D14's runtime row says: *"false: the legacy arm is
   never entered, and **a Track-H subsystem** whose bit is clear is a **startup**
   `Fatal{PipeLegacyMemosDisabled}`."* `MG_Pipe/MGPipe.h:77-78` labels only bits 5 and 6
   `// Track H, Espryt 0b` and `// Track H, Magma subsystem 4`; bit 0
   (`kMGPipeSubsystemRenderState`, `:72`) carries no such label and is the render-state CSO,
   not a memo re-key. The implementation makes the lever global, and makes the Fatal
   **per-draw inside `GetOrCreatePipeline`** rather than at startup.
2. **It breaks the per-subsystem A/B the lever exists for.** Reproduced:

```
$ MOBILEGL_PIPE_PUSH=0x60 MOBILEGL_PIPE_LEGACY_MEMOS=0 \
    ctest --test-dir build-push -L integration-gpu -j 4 -R 'DirectVulkan.*CrossFrameBuffer'
    1911 - DirectVulkan.CrossFrameBufferScenario.VertexOrphanAndReupload (Subprocess aborted)
    … 9 of 9 aborted …

$ grep 'Fatal{PipeLegacyMemosDisabled}' /tmp/rev-magma-0x60.log
[FATAL]: MGPipe: Fatal{PipeLegacyMemosDisabled} GetOrCreatePipeline wanted the pre-handle arm
         but MOBILEGL_PIPE_LEGACY_MEMOS=0 forbids entering it

$ MOBILEGL_PIPE_PUSH=0x20 MOBILEGL_PIPE_LEGACY_MEMOS=0 \
    ctest --test-dir build-push -L integration-gpu -R '…IndexBufferSubData'
    0% tests passed, 1 tests failed out of 1      (Subprocess aborted)
```

`0x60` is D18's `.Handles` environment written literally ("bits 5 and 6 set"), and `0x20` is
**Espryt's** Track-H bit alone — with bit 6 clear Magma's own vertex-input site is on the
legacy arm legitimately, yet the run still dies, from an unrelated subsystem's site. So
package C cannot run its own Track-H A/B with the legacy lever either.

Failure scenario on the integrated tree: `HandleRecycleScenario.Handles` (G8, an always-on
ctest entry the acceptance gate names) runs with `MOBILEGL_PIPE_LEGACY_MEMOS=0`. It is green
only if bit 0 happens to be set *and* a render-state CSO is bound on **every** draw of the
scenario. `ResolveBoundRenderStateCso`'s second guard (`VulkanRenderer.h:897-901`) turns the
"no CSO bound" case into the same hard abort rather than the documented fallback — and
`MGPipeApplyDeleteRenderState` clears `BoundRenderStateCso` (`MG_Pipe/PipeApply.cpp:179-180`),
so the null state is reachable, not hypothetical.

Not declared: §4 of `magma-v1.md` lists ten deviations and none of them is this; §7 mentions
only the *compile-time* `build-nolegacy` case.

---

## MAJOR 2 — the all-pull control arm is contaminated: `MOBILEGL_PIPE_PUSH=0` still mints client handles on every draw, so D.4.3's T2 no longer means what the brief defines it to mean

`MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:6603` and `:7187`:

```cpp
#if MOBILEGL_PIPE_PUSH
        snap.vaoHandle = ResolveVaoHandle(vao);
#endif
```

Guarded by the **compile-time** switch only — not by
`MagmaPipeSubsystemOn(kMGPipeSubsystemMagmaVertexInput)`, unlike every other re-keyed site in
the package (`:3638`, `:6290`, `VertexInputStateFactory.cpp:59,108,128,157`). `ResolveVaoHandle`
(`VulkanRenderer.h:1377-1389`) calls `MagmaPipeHandleOf` → `MGPipeSlots().Acquire`
(`MagmaPipeArms.h:75-77`) on first sight of each VAO and a `Uint64` compare on every later
draw. The stored value is **dead** in that arm: `vaoMoved` at `:6288-6296` takes the
`(address, lifetimeId, configVersion)` branch when the bit is clear, and `snap.vaoHandle` is
read nowhere else.

D14 states the contract this breaks verbatim: *"`MOBILEGL_PIPE_PUSH=0` in the environment is
the all-subsystems-pull control that **reproduces P1's behaviour exactly**."* It does not: the
control arm now pays a hash-map insert per VAO and a memoised compare per draw, and grows the
allocator (see Major 3).

Failure scenario: D.4.3 defines **T2 = `ns_per_op(push, MOBILEGL_PIPE_PUSH=0) − ns_per_op(pull)`**
as "P1's residual fill alone", so that **T1 − T2** "isolates exactly what P2 added and what it
removed". With this code, T2 also carries part of what P2 added, so T1 − T2 under-reports P2's
cost by an unknown amount — on the number the day-43 GO/NO-GO reads (G11, `ROADMAP.md:49,56`).
Two lines (`&& MagmaPipeSubsystemOn(...)`) would fix it. Not declared.

---

## MAJOR 3 — Magma mints handles from the client-side allocator and nothing ever frees them: permanent, unbounded growth per VAO and per vertex/index buffer on the **default** push path

`MagmaPipeArms.h:75-77` is the **only** caller of `MGPipeSlots()` in the whole tree, and
`MGPipeSlotAllocator::Free` / `::Reset` have **no caller at all**:

```
$ grep -rn "MGPipeSlots()" MobileGL --include=*.cpp --include=*.h | grep -v MG_Impl/Pipe/SlotAllocator
MobileGL/MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h:76:  return MG_Pipe::MGPipeSlots().Acquire(kind, lifetimeId);
$ grep -rn "MGPipeSlots().Reset\|\.Free(MGPipeKind\|Slots().Free" MobileGL   # (empty)
```

`MGPipeSlotAllocator::AllocateFor` (`MG_Impl/Pipe/SlotAllocator.cpp:83-95`) appends a
`SlotState` to `KindState::Slots` **and** inserts into `ByLifetimeId` for every distinct
lifetime id; lifetime ids are process-global monotonic atomics that are never reused
(`MG_State/GLState/VertexArrayState/VertexArrayObject.cpp:17-21`,
`MG_State/GLState/BufferState/BufferObject.cpp:18-24`). Magma acquires:

- `MGPipeKind::Buffer`, once **per enabled vertex attribute** inside `ComputeHash`
  (`VertexInputStateFactory.cpp:59-62`),
- `MGPipeKind::VertexElementsCso`, per VAO (`VertexInputStateFactory.cpp:88`,
  `VulkanRenderer.h:1379-1385`).

Failure scenario: a Minecraft session cycles chunk VAOs and their vertex/index buffers
continuously. Every one of them permanently costs a `SlotState` plus an `UnorderedMap` node
in a process-lifetime singleton that has no reclamation path — on Android, in front of the
LMK. Nothing in the package's evidence can see this: the retrace fixtures and integration
scenarios are seconds long. Note this is not "a P2-wide fact" as §8.4 frames it: on this tree
Magma is the *only* consumer of the allocator, and `MGPipeKind::VertexElementsCso` is not one
of the kinds any package's explicit-destroy work covers (D13's death notification is Espryt's
six twin kinds).

---

## MAJOR 4 — the density premise the two new direct-mapped tables are justified by is false, and the package's own D-5 says so; the result is a hot-path memo that degrades in exactly the workload the code comment names

`LookupVaoDrawMemo`'s handle arm (`VulkanRenderer.cpp:3630-3661`) argues:

> "slots are dense by construction (the allocator has a free list plus a high-water mark), so
> consecutive VAOs land in consecutive entries and the collision the address hash existed to
> spread does not arise **below the table size**."

`VertexInputStateFactory::MemosFor` (`VertexInputStateFactory.cpp:81-97`) repeats the argument
for its own 2048-entry table. But by Major 3 no `VertexElementsCso` slot is ever freed — which
`magma-v1.md` D-5 asserts itself ("Nothing in P2 frees a `VertexElementsCso` slot"). So
`handle.Slot` grows monotonically with the number of VAOs **ever created**, and "below the
table size" holds only for the first 2048 VAOs of the process. After that the *live* working
set is scattered over an unbounded slot range and `handle.Slot & 2047`
(`VulkanRenderer.cpp:3646`, `VertexInputStateFactory.cpp:93`) collides at birthday rates.

What a collision costs is worse than what it replaced, because the handle arm deleted the two
mitigations the address-hashed table had:

- no second candidate (`m_vaoDrawMemoTable[index ^ 1u]`, `:3688`) — one entry, unconditionally
  claimed;
- no victim choice by `bindings.frameSerial` (`:3694-3697`) — the incumbent is always evicted.

On a claim the arm resets `contentHash`, `layoutFactsValid` and the three `bindings` serials
(`:3650-3658`), and `MemosFor` clears the whole `VaoBackendMemos` (`:90-94`). So a colliding
pair of live VAOs re-hashes its configuration, re-resolves its vertex bindings and re-derives
its layout facts on **every draw** — in the Minecraft chunk-cycling stream the comment at
`:6283-6287` names as the reason the VAO memo exists at all. The old table was keyed on live
VAOs and could not do this.

Failure scenario, concretely: two live VAOs whose slots are 2048 apart, alternating draws;
each `LookupVaoDrawMemo` / `MemosFor` call wipes the other's memo and returns a cold entry.
Correctness is preserved (the `{slot, gen}` compare is exact); the entire per-draw win m3/m4
is claimed for is not. The package publishes no measurement that could detect it (§8.6 is
explicit that nothing here is measured), and the desktop fixtures never exceed 2048 VAOs.
D-5 declares the *fixed size*; it does not declare that the density argument its sibling
comments rest on is contradicted by its own premise.

---

## MAJOR 5 (folded into the verdict, listed separately for the integrator) — m1, the package's principal deliverable, is exercised by nothing on this tree, so every green it reports for the CSO re-key is vacuous

The package's own §3 probe is the proof: with `MOBILEGL_PIPE_LEGACY_MEMOS=0` at runtime "the
first Fatal names `GetOrCreatePipeline`". That is `ResolveBoundRenderStateCso`'s
`MGPipeHandleIsNull(boundCso)` branch (`VulkanRenderer.h:897-901`) — i.e. **no CSO is ever
bound on this tree**, so `pipelineStateHash` is never 0, `entry.renderStateCso` is always the
null handle, and the pipeline memo runs the legacy `ComputePipelineStateHash` arm on every one
of the 432 integration cases and all 40 retraces I re-ran. The only m1 code path any test
executes is its abort path — which is the path Major 1 shows is wrong.

The brief's own criterion is `ROADMAP.md:7`, *每个门必须能因它存在的理由变红*: G2/G3 cannot go
red for m1's reason, because m1 never runs. This was avoidable inside the package's own
scope — branching or rebasing onto `p2/tracker` (D.1 orders tracker **before** magma anyway),
or driving one `MGPipeApplyCreateRenderState`/`…BindRenderState` pair through the applier in a
unit test, would have armed it. §7 and §8.1 disclose the gap honestly; disclosure does not
make the package verified.

---

## Minors

1. **G1's admitted resize set is short by one.** `_GLOBAL__sub_I_DirectGLES.cpp −9` is a fourth
   resized symbol; brief §A G1 says "empty or exactly the one mangled `RenderState::RenderState()`"
   and D15 admits three. Confirmed present on `~/w7/p2-contract` too, so it is the contract's,
   not this package's — but the integrator must amend D15's set or explain it, or G1 is red by
   its own text after every merge.
2. **D-7's claim is inaccurate.** "Both acquisition sites sit behind a one-entry
   `lifetimeId -> handle` memo" — there are **three** acquisition sites, and the third,
   `VertexInputStateFactory.cpp:59-62` (`MGPipeKind::Buffer`), has no memo: it probes the
   allocator's hash map once per enabled attribute, up to 32 times per `ComputeHash`.
3. **`ComputeHash` is the one re-keyed site with no arm assertion.** `VertexInputStateFactory.cpp:57-72`
   has no `MagmaPipeRequireLegacyArm` call, so with `Features.PipeLegacyMemos=0` and bit 6
   clear it silently falls back to the pre-handle `GetLifetimeId()` key instead of declining —
   inconsistent with the other five sites and with D14's "the legacy arm is never entered".
4. **First `MG_Backend → MG_Impl` include, and the architectural conflict is not named.**
   `MagmaPipeArms.h:22` includes `<MG_Impl/Pipe/SlotAllocator.h>`; `ARCHITECTURE.md:9` states
   the server process links `MG_Backend` + the MGPipe object table and **not** `MG_State`,
   `MG_Impl`, glslang. `check_include_closure.py` has no probe rooted at any `MG_Backend`
   header (its four probes are the value / artifacts / mutation / wire headers), so G13's green
   says nothing about this. D-11 declares the include; it does not name `ARCHITECTURE.md:9`.
5. **D-3's `static_assert`s are sound but do not discharge D19's test.** I checked the
   coverage list at `VulkanRenderer.cpp:404-448` against `DynamicTailKey`'s inventory
   (`:344-350`): complete (`Viewports`, `DepthRanges`, `BlendColor`, `PolygonOffsetFactor`,
   `PolygonOffsetUnits`, `LineWidth`, `ScissorBoxes`, and `Ref`/`ValueMask`/`WriteMask` per
   face), and **not vacuous** — the inverted `ScissorTestEnabledMask` assert at `:445-448`
   proves `MagmaRenderStateRangeIsDynamic` can return false. But D19 still lists
   `DynamicChunksCoverMagmasDynamicTailKey` among the four tests package A fills into
   `RenderStateSpansTest.cpp`; the integrator must ensure the outcome is not "neither".
6. **D-4 verified.** `ScissorTestEnabledMask@900` is in pipeline chunk P6 and
   `SetCapability(ScissorTest)` (`RenderState.cpp:356-362`) does call `BumpVersions()`, so the
   tree is right and D12.3 is wrong as written. The tail stays correct because `BumpVersions`
   moves both counters.
7. **D-2 verified.** `MGPipeApplyBindRenderState` writes both versions
   (`MG_Pipe/PipeApply.cpp:165`), so `GetRenderStateParametersVersion()` does still move on a
   pipeline-only change. The tree wins over D12.3; the integrator's call whether to add a
   dynamic-only version to the wire.
8. **§5.5's correction to the brief is right and load-bearing.** The brief's G2/G14
   `grep -E '^\s+Test #'` matches only 4-digit ids (`ctest -N` right-aligns); I used the
   corrected `grep -E '^\s+Test\s+#[0-9]+:'` throughout and got 2367 vs the brief's form's
   1368. Fix `BRIEF-P2.md` §A before `wsl_integrate_p2.sh` uses it.
9. **The reported flake did not reproduce.** `PrimGenReroute…TheRerouteIsActuallyArmed…` and
   `PointSizeDemotion…TheDemotionIsActuallyArmed…` passed in every one of my four
   `-L integration-gpu -j 4 -R DirectVulkan` runs. Plausible as a load-sensitive lavapipe
   flake, but unconfirmed here.
10. `MAGMA_TAIL_INPUT_IS_DYNAMIC(Viewports)` / `(DepthRanges)` / `(ScissorBoxes)` assert over
    the whole array while the tail reads only index 0. Stricter than needed and harmless, but
    a future per-viewport chunk split would break the build with a message that does not say
    why.
11. `MGLOG_D_ONCE` at `VulkanRenderer.h:898` sits on a per-draw path. Compiled out at
    `MOBILEGL_LOG_ACTIVE_LEVEL=INFO` (every build here), and `_ONCE` elsewhere, so it is not a
    `ROADMAP.md:7` violation — recorded because §5.8 is right that the fallback leaves no trace
    by default, which is what made Major 5 easy to miss.

---

## What I could not reach

- G4 (verify): no `build-verify` on this tree, and C.3 asks for none.
- G6/G7/G8/G10/G12: packages A, B and E own the artefacts.
- G3's DirectGLES half and the 79-case total: this package is DirectVulkan-only by construction.
- G11 / all of D.4: device work.
- Any behavioural check of m1 (Major 5): impossible on this tree by construction.
