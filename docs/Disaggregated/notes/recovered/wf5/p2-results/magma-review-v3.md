# Adversarial review — package `p2/magma`, round 3 (`magma-v3.md`, HEAD `93cf2249`)

Verdict: **APPROVED — 0 majors, 12 minors.**

Everything below was re-run by me, in `~/w7/p2-magma` (and, where stated, `~/w7/p2-magma-armed`),
from scripts under
`C:/Users/GEEKER~1/.../scratchpad/wf5/p2-magma-review3/`, with logs under `~/w7/rv3-magma/`.
Both trees were left exactly as found (`git status --short` on `~/w7/p2-magma` shows only the
untracked `build-nolegacy/`; the armed tree is clean at `840f8026`). No source file was edited
and no probe was applied.

---

## 1. What I re-ran, and what it produced

| # | command | my result | v3's claim | agrees |
|---|---|---|---|---|
| R1 | `cmake --build build-{linux,push,nolegacy,verify} -j 12` | rc 0 ×4 | rc 0 ×4 (V1) | yes |
| R2 | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; `.text +160`; resized = `RenderState::RenderState()` (+148), `RenderState::SetCapability` (+77), `RenderState::IsCapabilityEnabled` (+29), `_GLOBAL__sub_I_DirectGLES.cpp` (−9) | V2 | yes — **and nothing in `MG_Backend/DirectVulkan` or `VertexArrayObject` is resized at all, which independently corroborates V3 without needing the contract `.so`** |
| R3 | `awk '/^    namespace RenderStateImpl \{/,/^    \} \/\/ namespace RenderStateImpl/' DirectGLES.cpp \| sha256sum`, for `9c6a8a25` and `HEAD`, against `~/w7/p2-before-syncrenderstate.sha` | all three `d8fd1c48716056c536752fde09db3a9e5aa8494b70404798bfc15d11220efe27` | V4 (G5) | yes |
| R4 | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty | V5 (G13) | yes |
| R5 | `python3 scripts/check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)` | V6 | yes |
| R6 | `python3 scripts/gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 | V7 | yes |
| R7 | `python3 scripts/gen_pipe_dirty_surface.py --check` / `--self-test` | **rc 2 both**, `error: unrecognized arguments: --check` | not claimed | G9 is unreachable on this tree — see minor 10 |
| R8 | `ctest -N` name sets, four dirs, working grep | `build-linux` = `build-push` = `build-nolegacy` = **2367**, byte-identical; `build-verify` = **3185**; vs `~/w7/p2-before-ctest-names.txt`: **0 removed, 4 added** (`{CsoCache,RenderStateSpans,SlotAllocator,Tracker}.PlaceholderUntilTheOwningPackageFillsThisIn` — all the contract's) | V9 (G2 name half, G14) | yes |
| R9 | `ctest -L unit --no-tests=error -j 8`, four dirs | **1489/1489** in each | V10 | yes |
| R10 | `ctest -L integration-gpu -j 4 -R DirectVulkan`, `build-push`, default mask | **432/432** | V11 | yes |
| R11 | same, `MOBILEGL_PIPE_PUSH=0` | **432/432** | V12 | yes |
| R12 | same, `build-nolegacy` | **432/432** | V15 | yes |
| R13 | `ctest --test-dir build-verify -L integration-verify -j 4 -R DirectVulkan` | **409/409** | V17 (G4, DirectVulkan half) | yes |
| R14 | `python3 ~/w7/retrace_gate.py --tree ~/w7/p2-magma --lib build-push/libMobileGL.so --out ~/w7/retrace-out/rv3-magma -j 4 --only 'DirectVulkan$'` | **40/40 PASS**, min ssim **0.993475** (`minecraft-1.21.11-main-menu`) | V18 (G3, DirectVulkan half) | yes, to the digit |
| R15 | `~/w7/p2-magma-armed` `build-push`: `ctest -L integration-gpu -j 4 -R DirectVulkan` | **432/432** | V24 | yes |
| R16 | armed tree, 13 `CrossFrameBuffer` cases with `MOBILEGL_LOG_FILE_PATH` | **0** occurrences of `no render-state CSO is bound` | V25 | yes |
| R17 | **negative control I added**: the same 13 cases on the *delivered* tree with `MOBILEGL_LOG_FILE_PATH` | **1** occurrence (`grep -rc` → `mgl.log:1`) | — | the warning is not vacuous: it fires exactly where the CSO arm is absent and never where it runs |
| R18 | `MOBILEGL_PIPE_PUSH=0x20 MOBILEGL_PIPE_LEGACY_MEMOS=0`, 13 cases | ctest reports **Skipped**, log carries 1 × `Fatal{PipeLegacyMemosDisabled}` naming `bit 6 of MOBILEGL_PIPE_PUSH` | V22 | yes |
| R19 | `MOBILEGL_PIPE_PUSH=0x7e MOBILEGL_PIPE_LEGACY_MEMOS=0`, 13 cases | **13/13 green** + 1 startup `WARN` naming `kMGPipeSubsystemRenderState (bit 0` | V23 | yes |
| R20 | grep `MagmaPipeIdentityTable` across my own 40-trace retrace output | exactly two INFO lines, in `minecraft-1.21.4-rd12-odinlite-in-world` only: `(Buffer): high-water 1024 slots minted, 1023 live` and `(VertexElementsCso): high-water 1024 slots minted, 1023 live`; **no `2048` milestone in any of the 40** | §2.3 | reproduces; see minor 11 for what it does and does not establish |

Nothing in v3's §3 table failed to reproduce. Nothing flaked (no intermittent failure in
any of the 12 ctest runs above).

---

## 2. The refutation attempts that failed (i.e. the code survived them)

I went after each of the classes the charter names. These are the checks, not padding — each
one was a candidate major that did not survive scrutiny, and the integrator should not have to
redo them.

### 2.1 Chunk-table partition and the subset hash vs the pipeline version

The contract's table (`MobileGL/MG_Pipe/MGPipeRenderStateSpans.h:59-100`) is 16 `offsetof`
boundaries with alternating halves and `static_assert`s for ascent, completeness,
`7 + 8 == 15`, and `396 + 772 == 1168`. I re-derived the halves from `RenderState.cpp` for
**25 setters**, not five, by extracting every `BumpVersions()` / `++m_version` /
`++m_pipelineStateVersion` site with its enclosing function:

| setter (`RenderState.cpp`) | bump | member(s) written | chunk | rule holds |
|---|---|---|---|---|
| `SetViewport` :78, `SetViewportIndexed` :98 | `++m_version` | `Viewports` | D0 | ✓ |
| `SetLineWidth` :113 | `++m_version` | `LineWidth` | D0 | ✓ |
| `SetPointSize` :203 | `++m_version` | `PointSize` | D0 | ✓ |
| `SetPatchVertices` :214, `SetPatchDefault{Outer,Inner}Level` :233,:244 | `BumpVersions()` | patch trio | **P0** | ✓ |
| `SetPolygonOffsetClamped` :276 | `++m_version` | `PolygonOffset{Factor,Units,Clamp}` | D1 | ✓ |
| `SetClipControl` :288 | `++m_version` | `ClipOrigin`, `ClipDepthMode` | D1 | ✓ |
| `SetBlendFunc(Indexed)` :520,:546, `SetBlendEquation(Indexed)` :572,:591 | `BumpVersions()` | `BlendStates[]` | **P1** | ✓ |
| `SetLogicOp` :607, `SetDepthFunc` :619, `SetDepthMask` :630 | `BumpVersions()` | `LogicOp`/`DepthFunc`/`DepthMask` | **P1** | ✓ |
| `SetColorMask(Indexed)` :688,:699 | `BumpVersions()` | `ColorMasks[]` | **P1** | ✓ |
| `SetClear{Color,Depth,Stencil}` :711,:722,:733 | `++m_version` | `Clear*` | D2 | ✓ |
| `SetBlendColor` :744 | `++m_version` | `BlendColor` | D2 | ✓ |
| `SetDepthRange(Indexed)` :760,:775 | `++m_version` | `DepthRanges` | D2 | ✓ |
| `SetSampleCoverage` :791, `SetSampleMaskValue` :806, `SetMinSampleShadingValue` :820 | `BumpVersions()` | `SampleCoverage{Value,Invert}`, `SampleMaskValue`, `MinSampleShadingValue` | **P2** | ✓ (this is the brief's declared deviation against `MGPipeTypes.h`'s "sample coverage is dynamic" comment — the tree is right) |
| `SetStencilFunc` :648-649 | `++m_version`, **conditional** `++m_pipelineStateVersion` | `Ref`,`ValueMask` → D3/D4; `Func` → P2/P3 | split | ✓ — the conditional bump is exactly the sub-member split |
| `SetStencilMask` :657 | `++m_version` | `WriteMask` | D3/D4 | ✓ |
| `SetStencilOp` :671 | `BumpVersions()` | `FailOp`/`PassDepth*Op` | P3/P4 | ✓ |
| `SetCullFaceMode` :894, `SetFrontFaceMode` :905, `SetProvokingVertexMode` :916 | `BumpVersions()` | the three enums | **P4** | ✓ |
| `SetHint` :131, `SetPointFadeThresholdSize` :147, `SetPointSpriteCoordOrigin` :157, `SetClampReadColor` :167 | `++m_version` | the hints + three | D5 | ✓ |
| `SetPolygonMode` :178 | `BumpVersions()` | `PolygonMode{Front,Back}` | **P5** | ✓ |
| `SetPrimitiveRestartIndex` :192 | `++m_version` | `PrimitiveRestartIndex` | D6 | ✓ |
| `SetCapability` :313 (the `SET_CAPABILITY` macro), :349, :360 | `BumpVersions()` | the 20 bools `ColorLogicOpEnabled..ProgramPointSizeEnabled` + `ScissorTestEnabledMask` | **P6** | ✓ |
| `SetCapability` :381 (`ClipDistance0..7`) | `++m_version` **only** | `ClipDistanceEnabledMask` | D7 | ✓ — and this is the case that would break if the mask were pipeline |
| `SetCapabilityIndexed` :460,:476 | `BumpVersions()` | `BlendStates[i].Enabled` (P1), `ScissorTestEnabledMask` (P6) | pipeline | ✓ |
| `SetScissorBox(Indexed)` :948,:969 | `++m_version` | `ScissorBoxes`, `ScissorBoxWrittenMask` | D7 | ✓ |

No disagreement in either direction. The declaration order in
`MGPipeValueTypes.h`'s `RenderStateParameters` confirms that every bool
`ComputePipelineStateHash` reads lies inside `[ColorLogicOpEnabled, ScissorBoxes)` = chunk P6,
so the P6 comment is accurate.

### 2.2 Did anything silently change meaning moving to the CSO key? (m1)

I enumerated **every** input of `ComputePipelineStateHash`
(`VulkanRenderer.cpp:4992-5079`) and located each one in the chunk table:
`CullFaceEnabled` P4; `DepthTestEnabled`/`DepthMask`/`DepthFunc`/`LogicOp`/`BlendStates[]`/`ColorMasks[]` P1;
`PolygonOffsetFillEnabled`, `RasterizerDiscardEnabled`, `ColorLogicOpEnabled`, `StencilTestEnabled`,
`PrimitiveRestart{,FixedIndex}Enabled`, `SampleShadingEnabled`, `MultisampleEnabled`, `SampleMaskEnabled` P6;
`SampleMaskValue`/`MinSampleShadingValue` P2; the patch quintet P0; `PolygonModeFront` P5;
`CullFaceModeSetting` P4; the four stencil ops + `Func` per face P2/P3/P4.
**All pipeline.** The only non-`RenderStateParameters` inputs are `colorAttachmentCount`,
`rasterizationSamples` (both folded through `ResolveEffectiveSampleMask`) and
`m_independentBlendFeatureEnabled` (a constant of the renderer). The first two are carried by
`entry.renderPassHash`: `renderPassHash` is `renderPassEntry.hash`
(`VulkanRenderer.cpp:5166`), the key of the render-pass cache entry that *owns*
`colorAttachmentCount` and `sampleCount` as fields (`VkRenderPassManager.h:68-80`), so two
draws with equal `renderPassHash` have equal values for both. The CSO's 396-byte subset is a
strict superset of the hashed fields (all 8 `BlendStates`/`ColorMasks` rather than the first
`colorAttachmentCount`), so the handle discriminates at least as finely. **m1's key is sound.**

`ComputePipelineSubsetStateHashFallback` (the `-DMOBILEGL_PIPE_LEGACY_MEMOS=OFF` fallback,
`VulkanRenderer.cpp:5091-5096`) is `MGPipeComputePipelineSubsetHash` over the same 396 bytes —
also a refinement, with the render-pass facts still separated. Sound.

### 2.3 Is the D19 coverage assertion a gate that can fail? (m2)

Yes. `DynamicTailKey`'s hand-written input inventory (`VulkanRenderer.cpp:344-354`) is exactly
`Viewports[0]`, `DepthRanges[0]`, `BlendColor`, `PolygonOffsetUnits`, `PolygonOffsetFactor`,
`LineWidth`, `StencilStates[0..1].{ValueMask,WriteMask,Ref}`, `ScissorTestEnabledMask bit 0`,
`ScissorBoxes[0]`, plus the four backend facts D12.3 exempts. The `static_assert` block at
`VulkanRenderer.cpp:400-492` covers **every one of them** — seven `MAGMA_TAIL_INPUT_IS_DYNAMIC`,
three × two `MAGMA_TAIL_STENCIL_IS_DYNAMIC`, and the inverted assert for
`ScissorTestEnabledMask`. `MagmaRenderStateRangeIsDynamic` walks the real
`kMGPipeRenderStateChunkBoundaries` and returns false the moment any overlapping chunk is
pipeline, so demoting `ColorMasks` (D19's negative control) or promoting `LineWidth` is a
build break here. Not decorative; the inventory is complete against it.

### 2.4 Stale answers from a memo or suppressor

- **Pipeline memo.** The handle arm stores `(pipelineStateHash = 0, renderStateCso = H)`, the
  pre-handle arm `(hash = h, cso = null)`, and the probe compares **both**
  (`VulkanRenderer.cpp:5209-5212`, `:6606-6610`); the two arms' entries can never match each
  other. `InvalidatePipelineMemo()` still clears `m_pipelineStateHashValid`
  (`VulkanRenderer.h:1030-1034`), so the surviving fallback cache cannot outlive a context.
  The fallback cache is keyed on a monotonic version plus both render-pass facts, so it cannot
  serve a stale hash.
- **The CSO handle as an identity.** `MGPipeApplyCreateRenderState` writes
  `record.Gen = desc.Cso.Gen` and never mutates a live record under an unchanged handle
  (`PipeApply.cpp:120-153`); `MGPipeApplyDeleteRenderState` clears the binding without bumping
  `Gen` and leaves the client to bump it on reuse (`:172-183`). So "same handle ⟹ same 396
  bytes" holds, which is what m1 relies on.
- **`VaoBackendMemos`.** Guarded by `Owner == handle` (Gen included) **and** the VAO's config
  version **and** the eviction epoch (`VertexInputStateFactory.cpp:87-99`, `:124-141`,
  `:152-172`) — the same three facts the legacy arm's `GetBackendStateMemo` guarded, no fewer.
  A slot that changed owner is cleared, never inherited.
- **The mint's one-entry front memo.** `Acquire` returns `m_lastHandle` only on an exact
  `lifetimeId` match (`MagmaPipeArms.h:215-218`), and `OnFrameBoundary` drops it whenever any
  slot is retired (`:255-263`) — which is the only way a slot can reach the free list and so
  the only way a memoised handle can go stale. I could not construct a stale hit.
- **`m_vaoDrawMemoTable`.** The handle arm's victim reset (`VulkanRenderer.cpp:3691-3702`) is
  field-for-field the legacy arm's (`:3739-3748`): `contentHash = 0`, `layoutFactsValid = false`,
  `bindings.{frameSerial, indexFrameSerial, indexBuffer}` cleared. I also traced the
  "prefer the empty way" victim rule through retire-and-reuse sequences; it degrades to at
  worst one extra collision per set and never returns another object's entry.

### 2.5 A handle re-key that breaks on deletion, reuse, or a share group

- `Gen` is `Uint32` on both sides (`MGPipeHandles.h:52-53` and `MagmaPipeArms.h:279`) — no
  truncation, so the `Gen == ~Uint32{0}` permanent-retire guard (`:292-297`) is the real bound.
- A `{slot, gen}` is never re-issued: `ClaimSlot` bumps `Gen` on every free-list pop
  (`:298`) and starts a fresh slot at 1 (`:303`). Two live objects can therefore never share a
  handle, and a *memoised* hash naming `{7,3}` can never be matched by a later object, because
  slot 7's generation only ever increases.
- The `lifetimeId == 0` path would be dangerous — `MagmaPipeSlotIndex(kMGPipeNullHandle)`
  returns 0 (`MagmaPipeArms.h:366-372`) and `VaoBackendMemos::Owner` defaults to the null
  handle, so a zero-id object would match slot 0's entry forever. It is unreachable:
  `BufferObject.cpp:19` seeds `g_nextBufferLifetimeId{1}` and both `m_lifetimeId` members are
  `const` and initialised from the allocator (`BufferObject.h:254`,
  `VertexArrayObject.h:193`). Verified, not assumed.
- Share groups: VAOs are not shared; buffers are, but the mint is a `VulkanRenderer` member
  (`VulkanRenderer.h:1457`) and the identity hash it feeds is only ever the *factory's own map
  key*. The pipeline's `vertexInputHash` is the content-derived `vis.layoutHash`
  (`VulkanRenderer.cpp:5164-5165`), not the identity hash, so nothing crosses a renderer.

### 2.6 Instrumentation on the hot path

`ResolveBoundRenderStateCso` (`VulkanRenderer.h:1288-1301`) adds one non-atomic member load and
a predicted branch per draw; the `MGLOG_I` high-water line fires only on the allocate-a-new-slot
branch at powers of two ≥ 1024 (`MagmaPipeArms.h:316-320`) — R20 shows exactly two such lines
across the whole 40-trace corpus; the retire line is `MGLOG_D`. Nothing on the draw path.

### 2.7 Undeclared deviations

I diffed §5's list against the code and found no deviation from D1–D20 that is not declared
there. In particular D-8 (guard, don't delete, the frontend VAO memos) is the only reading that
satisfies both D12.5 and D14/G1 — the brief contradicts itself here and the tree resolves it in
the direction the gate demands.

---

## 3. Minors

1. **The Buffer mint is aged on "last hashed", not "last drawn", and the header claims
   otherwise.** `HandleOf(MGPipeKind::Buffer, …)` has exactly **one** call site in the tree —
   `VertexInputStateFactory.cpp:64`, inside `ComputeHash` — and `ComputeHash` runs only on a
   hash-memo *miss* (`VertexInputStateFactory.cpp:127-133`, `:137-141`;
   `grep -rn "HandleOf(" MobileGL/` returns three lines, two of them the VAO kind). So a buffer
   drawn every frame through a VAO whose config version has not moved is never re-`Acquire`d,
   its `Entry::LastUse` never advances, and `OnFrameBoundary` retires its slot after
   ~1280 boundaries — while `MagmaPipeArms.h:173-176` says "retires slots whose object has not
   been **drawn**" and `:267-272` says the two ages are "deliberately the same numbers … a slot
   retired earlier than its cache entry would … be a pure waste". For the VAO table that is
   true (`MemosFor`/`ResolveVaoHandle` stamp on every draw); for the Buffer table it is false,
   and the "pure waste" it claims to avoid is exactly what happens. Consequences, both bounded:
   (a) the `MagmaPipeIdentityTable(Buffer)` high-water/live numbers §8.1 asks the integrator to
   record in `MEASUREMENTS.md` are *not* the live buffer working set; (b) when a long-stable
   VAO is eventually reconfigured its buffers get fresh `{slot, gen}`, so an unchanged
   (layout, buffer-set) pair produces a *different* vertex-input identity hash than before and
   misses a `m_cache` entry the base ref's lifetime-id key would have hit — one extra
   `BackendVertexInputState` build per reconfiguration, not per draw. **No correctness effect**
   (§2.5 above: a `{slot, gen}` is never re-issued, so no two objects can ever alias). Fix or
   downgrade the comment; if the behaviour is intended, say so and stop calling the number a
   working-set measurement.
2. **`churn.py`'s workload model does not match the landed call pattern** for the same reason:
   it drives `Acquire` per use, which is right for VAOs and wrong for buffers (minor 1). §2.2's
   conclusion survives untouched — a mint with no capacity cannot evict, so 0.0 % churn holds
   for any call pattern — but the buffer row is modelling something the code does not do.
3. **Member-destruction order is a latent dangling pointer.**
   `UniquePtr<VertexInputStateFactory> m_vertexInputStateFactory` is declared at
   `VulkanRenderer.h:1039`, `MagmaPipeIdentityTables m_pipeIdentity` at `:1457`. Members destruct
   in reverse declaration order, so `m_pipeIdentity` dies **first** and the factory — whose
   `m_identity` points at it (`VertexInputStateFactory.h:665`) — is destroyed afterwards holding
   a dangling pointer. Harmless today (`~VertexInputStateFactory() = default`, and nothing in
   `~VulkanRenderer`'s body touches it), but there is no guard and no comment. Declaring
   `m_pipeIdentity` above `m_vertexInputStateFactory` removes the hazard for free.
4. **`MG_Pipe::MGPipeApplier()` is one process-global** (`PipeApply.cpp:99`, `:112`), and this
   package's pipeline memo now keys on `BoundRenderStateCso` read out of it. Declared and routed
   (§8.2, and a comment at `ResolveBoundRenderStateCso`). Carried to package A / the integrator,
   unchanged from review v2.
5. **`DynamicChunksCoverMagmasDynamicTailKey` is `static_assert`s here, not a ctest entry.**
   Declared (D-3, minor 6, §8.3). I verified in §2.3 that the assertion set is *complete* against
   `DynamicTailKey`'s inventory and *can* fail, so this is a real gate — it is only in the wrong
   file. Integrator: make sure the outcome is not "neither".
6. **`553065dc`'s subject still overclaims** ("drive the dynamic tail from the pushed dynamic
   version") when the commit carries only comments and `static_assert`s. Argued rather than
   fixed (minor 5). Fair given "new commits on top", but the honest subject belongs in the squash.
7. **`MGLOG_I` for the high-water line is a declared deviation from D20** (D-16). Justified by
   `MGLOG_D` being compiled out of every measured build, and R20 confirms it is two lines across
   40 traces. Accept, but it should not become the precedent for other packages.
8. **`snap.vao` / `snap.vaoLifetimeId` are still stamped unconditionally** at
   `VulkanRenderer.cpp:6644-6645` and `:7235-7236`, including in the
   `-DMOBILEGL_PIPE_LEGACY_MEMOS=OFF` build where `MagmaPipeTrackHArmIsHandles` is a compile-time
   `true` and the pair is provably dead. Two dead stores per snapshot. C.3 asked for the pair to
   be collapsed into the handle; under the legacy arm it cannot be, but under `nolegacy` it can.
9. **`VaoDrawMemo` gains 8 bytes inside an `alignas(64)` struct** under push
   (`VulkanRenderer.h:1401-1408`). If that crosses a 64-byte multiple, the fixed 2048-entry table
   grows by 128 KB in every push build. Not measured anywhere in v3; worth one `sizeof` line in
   `MEASUREMENTS.md`.
10. **G9 is not reachable on this tree and this package's green must not be read as covering it**:
    `gen_pipe_dirty_surface.py --check` / `--self-test` both exit 2 with
    `unrecognized arguments` (R7), because `scripts/gen_pipe_dirty_surface.py` and
    `DirtySurface.def` are package B's. Expected; recorded so nobody double-counts.
11. **§2.3's "peak 1408 live VAOs and 1408 live buffers (±64)" is not reproducible from the
    shipped logging**, and what *is* reproducible (R20) is weaker than the sentence built on it:
    exactly one of the 40 traces crosses 1024 minted slots for either kind, and **none** crosses
    2048. So the corpus establishes ≥1024 and <2048 minted slots — enough to say r2's fixed
    2048-entry VAO table was being approached, not enough to say a device case "would have been
    past the capacity". M1's fix does not depend on the stronger claim (an unbounded mint has no
    capacity to exceed), so this is a wording issue in the evidence, not a hole in the fix.
12. **m1's handle arm is only ever executed on the scaffolding tree.** On the delivered tree
    `MGPipeApplier().BoundRenderStateCso` is always null, so every one of R10–R14's lanes runs
    m1's *fallback*, not its handle arm — I confirmed this directly (R17: the delivered tree emits
    the `no render-state CSO is bound` warning; R16: the armed tree emits none, and is 432/432).
    v3 says this, and the armed tree is scheduled for deletion (§8.12). **Integrator: after
    `tracker` lands in `~/w7/pipe`, R10/R13/R14 must be re-run there**, or m1 ships with its only
    coverage in a worktree that no longer exists.

---

## 4. Things v3 reports honestly that I confirmed, and that are not findings against it

- **Red probe B is a negative result** (§2.5): removing the `++Gen` does not turn the 432 red.
  I did not re-run the probes (they require editing the armed tree, which I was told not to do),
  but the mechanism v3 gives is correct — an inherited `VaoBackendMemos` still has to survive
  `HashConfigVersion != GetConfigVersion()` and an inherited `VaoDrawMemo` still has to survive
  the per-draw `bindings` revalidation, so only a constructed ABA defeats all of it. That
  construction is `HandleRecycleScenario` (D18, package E). The `Gen` discipline is therefore
  load-bearing and uncovered on this tree, and v3 says so in as many words. G8 must be real.
- **Brief-vs-tree items §6.5, §6.9** are correct and I verified both:
  `~/w7/retrace_gate.py`'s `add_argument` list is `--tree --lib --out -j --only` only (no
  `--ssim`, no `--backend`), and `grep -E '^\s+Test #'` under-matches because `ctest -N`
  right-aligns the id. Both need fixing in `BRIEF-P2.md` before `wsl_integrate_p2.sh` runs.
- **§6.6** (G1/D15's admitted resize set is short by `_GLOBAL__sub_I_DirectGLES.cpp`) is
  confirmed by R2: the fourth resized symbol is real and is the contract's. D15 must be amended
  or G1 is red by its own text after every merge.
- **§6.11 / minor 9** (a DEBUG-log-level build does not compile, so no `MOBILEGL_ASSERT` in the
  tree is live in any buildable configuration) is a tree-wide fact about a file this package does
  not own. It is the reason the `Gen`-wrap defence was moved to the release path, which is the
  right call.

---

## 5. Bottom line

Every gate this package can reach reproduces exactly as claimed, on my own runs, from the
baselines the brief pins. The pull build gains nothing (0 added / 0 removed / 0 renamed, and no
resize outside the contract's three `RenderState` symbols plus `_GLOBAL__sub_I_DirectGLES.cpp`),
`SyncRenderState` is byte-identical, the test-name set only grows by the contract's four
placeholders, and the two re-keys survived the aliasing, deletion, reuse and share-group attacks
I could construct. The M1 rework is sound: the mint has no capacity to overflow, and its
generation discipline makes a stale handle unmatchable by construction.

The one substantive thing I found — the Buffer mint aging on the wrong clock (minor 1) — is a
cost and documentation defect with no correctness consequence and no gate that it fails, so it
does not block. **Approved.**
