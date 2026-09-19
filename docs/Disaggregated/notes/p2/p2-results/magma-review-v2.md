# P2 package D — `p2/magma` adversarial review (v2)

Tree reviewed: `~/w7/p2-magma` @ `1e89fd90`, six commits on top of the contract `9c6a8a25`
(tag `p2/contract`). Build dirs as found: `build-linux` (pull), `build-push`,
`build-nolegacy`. Auxiliary tree `~/w7/p2-magma-armed` @ `88a8e661` (`p2/magma` + `p2/tracker`).
The tree was left exactly as found: no source edit, no new build dir inside either worktree
(the two out-of-tree compile probes wrote to `~/w7/rev-magma/` and `/tmp`).

**Verdict: NOT APPROVED — 1 major, 11 minors.**

The three v1 majors that were code defects (MAJOR 1 per-draw abort, MAJOR 2 all-pull arm
contamination, MAJOR 5 "m1 executed by no test") are genuinely fixed and I reproduced the
evidence for each. The remaining major is the *replacement* introduced by v2's fix for v1
MAJOR 3/4: the backend-local identity table. Its declared cost in `magma-v2.md` §2 MAJOR 4 is
materially understated, and the understatement hides a new unbounded-memory failure mode in
exactly the workload P2 exists to speed up.

---

## 0. What I re-ran, and what it said

Everything below was executed by me on `~/w7/p2-magma` @ `1e89fd90` (builds were already
current — `ninja: no work to do` in all three dirs).

| gate | command | observed |
|---|---|---|
| **G1** | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; the four are `RenderState::RenderState()` +148, `RenderState::SetCapability` +77, `RenderState::IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9 |
| **G1, isolated to this package** | same script, `--before ~/w7/p2-contract/build-linux/libMobileGL.so` | `0 added, 0 removed, **0 resized**, 0 renamed`, `.text +0`. This package's pull build is byte-identical to the contract's. D-13's `#else` duplication does what it claims. |
| **G13** | `check_include_closure.py` / `gen_pipe.py --check` / `--self-test` / `grep -rc pGLContext MobileGL/MG_Backend` / stdio grep | `4 probes, 0 skipped, 0 problem(s)`; rc 0; `7 negative-control trip(s), positive control OK`; grep empty; no stdio (only the word "puts" inside prose comments) |
| **G5** | `awk '/^    namespace RenderStateImpl \{/,/^    \} \/\/ namespace RenderStateImpl/' DirectGLES.cpp \| sha256sum` | `d8fd1c48…0efe27` — equal to `~/w7/p2-before-syncrenderstate.sha` |
| **G2 (name half), G14** | `ctest -N` with the *working* grep in all three dirs | 2367 / 2367 / 2367, all three byte-identical; vs the 2363-name baseline: **0 removed**, 4 added, all four the contract's `MG_Test/Pipe` placeholders |
| unit | `ctest -L unit -j 8` in `build-linux` / `build-push` / `build-nolegacy` | **1489/1489** in each |
| integration | `ctest -L integration-gpu -j 4 -R DirectVulkan` in six configurations | **432/432** in every one: `build-linux` (pull); `build-push` default mask; `MOBILEGL_PIPE_PUSH=0`; `MOBILEGL_PIPE_PUSH=0x60 MOBILEGL_PIPE_LEGACY_MEMOS=0` (v1 MAJOR 1's repro); `MOBILEGL_PIPE_LEGACY_MEMOS=0` at the default mask; `build-nolegacy`. **No flakes in any of the six** (the result file's §7 flakes did not recur here). |
| **G3, DirectVulkan half** | `retrace_gate.py --tree ~/w7/p2-magma --lib build-push/libMobileGL.so --out ~/w7/retrace-out/rev-magma -j 4 --only 'DirectVulkan$'` | `passed 40 / 40; failed: []` |
| arming (v1 MAJOR 5) | 13 `DirectVulkan.*CrossFrameBuffer` cases with `MOBILEGL_LOG_FILE_PATH`, armed tree vs delivered tree | armed: 13/13, **0** occurrences of the fallback warning; delivered: 13/13, **1**. Across the 40 retrace logs on the delivered tree the warning appears in **80** files (`grep -rl "no render-state CSO" ~/w7/retrace-out/rev-magma`). The claim in §4 reproduces exactly and its control half is real. |
| ownership (C.5) | `git diff --name-only 9c6a8a25..HEAD` | the six files C.3 names, nothing else |
| `MOBILEGL_PIPE_VERIFY` build | recompiled `VulkanRenderer.cpp` and `VertexInputStateFactory.cpp` out-of-tree from `build-push/compile_commands.json` with `-DMOBILEGL_PIPE_VERIFY=1` | rc 0, no warnings — the never-built verify configuration is not a compile risk for this package |

**Chunk-table spot check (the review's "re-derive at least five setters" item).** I re-derived
eight setters from `MG_State/GLState/RenderState/RenderState.cpp` against the landed boundaries
in `MG_Pipe/MGPipeRenderStateSpans.h:60-100` and the declaration order in
`MG_Pipe/MGPipeValueTypes.h`. All consistent with the rule "pipeline ⟺ a `BumpVersions()`
setter writes it":

| setter | `RenderState.cpp` | writes | chunk | half | verdict |
|---|---|---|---|---|---|
| `SetStencilFunc` | `:637-649` | `Func` (pipeline version conditional at `:649`), `Ref`, `ValueMask` (`++m_version` only) | `Func` → P2/P3, `Ref`/`ValueMask` → D3/D4 | split | ✔ the sub-member split is exactly what the conditional bump needs |
| `SetStencilMask` | `:652-657` | `WriteMask`, `++m_version` | D3/D4 | dynamic | ✔ |
| `SetSampleCoverage` | `:786-791` | `SampleCoverageValue`, `SampleCoverageInvert`, `BumpVersions()` | P2 | pipeline | ✔ (the D6 deviation against `MGPipeTypes.h:232-235` is real and correctly resolved) |
| `SetCapability(ClipDistanceN)` | `:373-381` | `ClipDistanceEnabledMask`, `++m_version` only, explicitly *not* `BumpVersions` | D7 | dynamic | ✔ |
| `SetCapabilityIndexed(ScissorTest)` | `:456-460` | `ScissorTestEnabledMask`, `BumpVersions()` | P6 | pipeline | ✔ — and it is why `magma-v2.md` D-4 inverts D12.3's claim correctly |
| `SetScissorBox` | `:926-948` | `ScissorBoxes`, `ScissorBoxWrittenMask`, `++m_version` | D7 | dynamic | ✔ |
| `SetPolygonMode` | `:174-178` | `PolygonModeFront`, `PolygonModeBack`, `BumpVersions()` | P5 | pipeline | ✔ |
| `SetMinSampleShadingValue` | `:813-820` | `MinSampleShadingValue`, `BumpVersions()` | P2 | pipeline | ✔ |

**The m1 discrimination argument holds.** I read `ComputePipelineStateHash`
(`VulkanRenderer.cpp:4963-5051`) field by field: every one of its `RenderStateParameters`
inputs — the 11 capability bools, `MinSampleShadingValue`, `SampleMaskValue`, `PatchVertices`,
the six patch levels, `PolygonModeFront`, `CullFaceModeSetting`, `DepthFunc`, `LogicOp`, both
faces' `{FailOp, PassDepthPassOp, PassDepthFailOp, Func}`, `BlendStates[i]`, `ColorMasks[i]` —
lands in an odd (pipeline) chunk of the landed table. Its two remaining inputs,
`colorAttachmentCount` and `rasterizationSamples` (also reached through
`ResolveEffectiveSampleMask`, `:4954-4960`), are pinned by `entry.renderPassHash`:
`VkRenderPassManager.cpp:606-765` folds the draw-buffer list, the valid draw-buffer count and
each attachment's `sampleCount` into that hash. So collapsing `pipelineStateHash` to the CSO
handle loses no discrimination, and `m_independentBlendFeatureEnabled` is a per-renderer
device constant while the memo is per renderer. I could not construct an aliasing case.

**The m2 `static_assert`s are a real gate, not decoration.** I compiled an out-of-tree probe
(`~/w7/rev-magma/probe/probe.cpp`) that reuses the landed
`MagmaRenderStateRangeIsDynamic` shape against `MGPipeRenderStateSpans.h`: the positive
controls (`BlendColor`, `LineWidth`) hold, the inverted controls (`DepthFunc`,
`ScissorTestEnabledMask`) hold, and a deliberately wrong assertion produces exactly one
`static assertion failed`. I also checked the assertion set against `DynamicTailKey`'s own
inventory (`VulkanRenderer.cpp:344-350`): every listed reader is covered, none is missing.

**Handle identity is sound for deletion, reuse and share groups.** `MGPipeHandles.h:52-58`'s
contract is met by `MagmaPipeIdentityTable::Acquire`: `Gen` moves on every owner change, and
lifetime ids are process-global monotonic from 1
(`VertexArrayObject.cpp:17-21`, `BufferObject.h:254`), so two live objects can never share a
`{slot, gen}`, a recycled heap address cannot reproduce one, and a buffer shared across a share
group keys identically in both contexts. The ABA control is wired the way D18 specifies
(`VertexInputStateFactory.cpp:293-301` hashes `attr.Buffer.get()` on the pre-handle arm;
`VulkanRenderer.cpp:3692-3710` drops the lifetime-id half of the compare), and both guards sit
on the arm D18's `AbaControl` run selects (`MOBILEGL_PIPE_PUSH=0`).

---

## MAJOR

### M1 — the re-key replaces two *unbounded, per-object* VAO memos with a 2048-entry 2-way cache, and above capacity turns the vertex-input content hash into a per-draw value that inserts an entry into an unbounded map on every draw

**Where.** `MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h:143-241` (the identity table,
`kMagmaVaoIdentityEntries = 2048`, `kMagmaBufferIdentityEntries = 8192`, 2 ways per set);
`VertexInputStateFactory.h:487-508` (`VaoBackendMemos`, `kVaoMemoSlotCount = 2048`);
`VertexInputStateFactory.cpp:110-152` (`TryGetMemoizedHash`, `GetOrComputeHash`);
`VertexInputStateFactory.cpp:158-183` (`GetOrCreateVertexInputState`'s state memo);
`VertexInputStateFactory.cpp:210-216` + `.h:170` (`m_cache`, an unbounded
`UnorderedMap<HashType, UniquePtr<BackendVertexInputState>>`);
`VertexInputStateFactory.cpp:420-452` (`OnFrameBoundary`: swept every **256** boundaries,
retire age **1024** boundaries).

**What changed, precisely.** Before this package, the two facts the draw path memoises per VAO
lived as `mutable` fields **on the `VertexArrayObject` itself** —
`m_backendHashMemo` / `m_backendHashMemoVersion` and
`m_backendStateMemo` / `…Epoch` / `…Version`
(`MG_State/GLState/VertexArrayState/VertexArrayObject.h:204-216`). One memo per live VAO, no
capacity, no eviction, guarded only by the VAO's own config version. After this package they
live in `VertexInputStateFactory::m_vaoMemos`: 2048 entries, indexed by a slot minted from a
**2-way set-associative LRU** table with 1024 sets.

`magma-v2.md` §2 MAJOR 4 describes this as "the mitigations are not gone, they moved one level
down", "both tables are caches", and "with more live objects than the table holds, an LRU
eviction changes a live object's handle … For a VAO that costs a memo recompute". Two of those
three statements are wrong for the memos in question, and the third understates the cliff.

**Reproduction.** `MagmaPipeIdentityTable::Acquire` (`MagmaPipeArms.h:157-188`) is 20 lines and
fully deterministic. I transcribed it verbatim into
`…/scratchpad/wf5/magma-review/s29.sh` / `s37.sh` and measured the fraction of *uses* whose
first `Acquire` misses — each such miss clears the VAO's `VaoBackendMemos` (`MemosFor`,
`VertexInputStateFactory.cpp:100-106`) **and** resets its `VaoDrawMemo`
(`VulkanRenderer.cpp:3662-3671`):

```
VAO table (2048 entries, 1024 sets), live lifetime ids CONSECUTIVE (best case), round-robin draw order:
  live VAOs= 2048   handle-CHANGED=     0  (  0.0% of uses)
  live VAOs= 2500   handle-CHANGED=  4068  ( 54.2% of uses)
  live VAOs= 3000   handle-CHANGED=  8568  ( 95.2% of uses)
  live VAOs= 4000   handle-CHANGED= 12000  (100.0% of uses)

same, lifetime ids SPARSE (a long-running app that creates and destroys VAOs):
  live VAOs=  512   handle-CHANGED=    48  (  3.1%)
  live VAOs= 1024   handle-CHANGED=   615  ( 20.0%)
  live VAOs= 2048   handle-CHANGED=  3570  ( 58.1%)

Buffer table (8192 entries, 4096 sets), consecutive ids:
  live buffers= 8192  key-CHANGED=      0  (  0.0%)
  live buffers=10000  key-CHANGED=  16272  ( 54.2%)
  live buffers=16000  key-CHANGED=  48000  (100.0%)
```

The per-use miss rate is independent of how many times a draw re-acquires the same VAO (I
re-ran at 1 and 5 acquires per use — `s37.sh` — identical numbers), because the extra acquires
are hits that only refresh `LastUse`.

The second table is the one the code says is "sized four times the VAO one anyway … to keep it
rare" (`MagmaPipeArms.h:227-232`). At 20 % on the first table with only **half** its nominal
capacity in live objects, "rare" is not what the structure delivers.

**Failure scenario, tier 1 (VAO table alone over capacity — certain).** Minecraft in-world with
one `VertexBuffer`-backed VAO per chunk section is the `minecraft-1.21.4-in-world` /
`…-sodium-in-world` shape and is one of D.4.2's four device cases; the tree's own comment at
`VertexArrayObject.h:150-155` describes the target as "an app cycles hundreds of VAOs per
frame", and `VulkanRenderer.h:1385-1390` describes VAO create/destroy churn heavy enough that
"a deleted VAO's heap address is handed straight back". For every draw whose VAO's set is
over-subscribed:

1. `MemosFor` clears the entry, so `TryGetMemoizedHash` returns false and `GetOrComputeHash`
   runs `ComputeHash(vao)` — a walk of all 16 attributes with ~10 `XXH64_update` calls each
   **plus 16 more `MagmaPipeBufferIdentity().Acquire()` calls**
   (`VertexInputStateFactory.cpp:20-56, 284-303`) — **once per draw** where it previously ran
   once per VAO reconfiguration;
2. the state memo misses too, so `GetOrCreateVertexInputState(vao, hash)` does the `m_cache`
   probe the memo exists to skip;
3. `LookupVaoDrawMemo` resets `contentHash`, `layoutFactsValid` and `bindings.frameSerial`
   (`VulkanRenderer.cpp:3666-3671`), so `TryBindResolvedVertexBindings` declines and the draw
   pays a full vertex-binding re-resolve.

That is three memos lost per draw in the workload whose per-draw cost is the entire point of
P2, and G11's acceptance criterion is "per-thread CPU p50 and p99 deltas are **not negative**
on either device".

**Failure scenario, tier 2 (both tables over capacity — memory).** Once the buffer table also
thrashes, each per-draw `ComputeHash` mixes a **different** `{slot, gen}` for the same buffer
(`VertexInputStateFactory.cpp:289-292`), so the content hash differs from the previous draw's.
`GetOrCreateVertexInputState(vao, hash)` then misses `m_cache` and **heap-allocates and inserts
a new `BackendVertexInputState`** (`VertexInputStateFactory.cpp:210-216`, `.h:170`). `m_cache`
has no capacity bound; `OnFrameBoundary` sweeps only when
`m_frameBoundaryCounter % 256 == 0` and retires only entries older than 1024 boundaries
(`:428-433`), so growth is bounded solely by ~1024 frames × draws-per-frame of insertions.
Every erase in that sweep also bumps `m_evictionEpoch`, invalidating **every** VAO's state
memo (`:436-441`), so the sweep itself is a second cliff. Nothing in `magma-v2.md` mentions
this path; §2 MAJOR 4 stops at "costs a rebuilt `BackendVertexInputState` … per VAO
reconfiguration, not per draw", which is exactly the premise tier 1 removes.

This is the same class of problem the design change was made to avoid — the header's own
justification for not using `MG_Impl/Pipe/SlotAllocator` is that an allocator with a dead
`Free` "grows … for the life of the process, on a platform with an LMK"
(`MagmaPipeArms.h:129-139`). The replacement trades a bounded-rate leak of one `SlotState`
plus one map node per object ever created for an unbounded-rate insertion into `m_cache` of a
full `BackendVertexInputState` per draw.

**Why this is a major and not a declared risk.** The deviation is declared (D-5/D-7), but the
declaration rests on three claims I can show are false or unsupported:

* "both tables are caches" — false for `VaoBackendMemos`. Its predecessors were per-object
  fields with **no** capacity. The package does not move eviction one level down; it
  *introduces* eviction where there was none.
* "2048 entries is what the address-hashed `VaoDrawMemo` table it replaces held, so the working
  set this covers without eviction is unchanged" (`MagmaPipeArms.h:224-226`) — true of the
  draw memo, false of the hash and state memos, which are the ones the P1→P2 per-draw win
  depends on.
* "nothing on desktop reaches those working sets" (§2 MAJOR 4) — true, and that is the problem:
  the 432 integration cases, the 40 retraces and the 1489 unit tests are all below both
  capacities, so **every green gate in §3 is green in the regime where the structure cannot
  fail**, and the regime where it does fail is the device A/B that has not been run.

**What would close it.** Either size both tables off a measured live-object high-water mark for
`minecraft-1.21.4-in-world` and `…-sodium-in-world` and pin the number with an assertion, or
keep the hash/state memo per-object (unbounded, as today) and let only the draw memo be
slot-indexed, or run D.4.2 on one device first and publish the p50/p99 before this lands. The
minimum acceptable outcome is a measurement, because no gate this package can reach can see it.

---

## MINORS

1. **`MGLOG_W_ONCE` is on the per-draw pipeline path and its documented cost is wrong by an
   order of magnitude.** `VulkanRenderer.h:1038-1041` and `magma-v2.md` §2 both state "`_ONCE`
   costs one static bool test". `MOBILEGL_LOG_ONCE_INTERNAL` (`MG_Util/Debug/Log.h:51-58`) is
   `static std::atomic_flag latch; if (!latch.test_and_set(relaxed))` — an **unconditional**
   read-modify-write on every evaluation. `objdump -dC` of
   `build-push/CMakeFiles/MobileGL_s.dir/…/VulkanRenderer.cpp.o` shows ten
   `xchg %al,0x0(%rip)` (implicitly locked) inside `VulkanRenderer::GetOrCreatePipeline`. It is
   branch-guarded on `MGPipeHandleIsNull(boundCso)`, so it is inert once a tracker is present
   (verified: 0 occurrences on the armed tree) — but it fires on **every draw** in the
   configuration this package ships today (bit 0 set, no tracker: 80 of 80 retrace logs carry
   it). `ROADMAP.md:7` forbids committing hot-path instrumentation, and §4 uses this precisely
   as instrumentation. Fix the comment at minimum; consider `MGLOG_D_ONCE` plus a `PipeStats`
   counter, which is what D17 exists for.
2. **`MOBILEGL_PIPE_LEGACY_MEMOS=0` does not stop Magma entering the pre-handle arm, and does
   not say so.** With bit 0 (`kMGPipeSubsystemRenderState`) clear,
   `ResolveBoundRenderStateCso` returns null at `VulkanRenderer.h:1046-1048` — *before* the
   warning — and `ResolveFallbackPipelineStateHash` (`:924-935`) calls
   `ComputePipelineStateHash`, which D14's compile-switch row names explicitly as part of "the
   pre-handle arm". D14's runtime row says "false: the legacy arm is never entered". Repro:
   `MOBILEGL_PIPE_PUSH=0x7e MOBILEGL_PIPE_LEGACY_MEMOS=0` runs green while executing the legacy
   hash on every draw whose pipeline version moved, silently. The behaviour is arguably the
   only sensible one, but it is a D14 deviation and it is **not** in `magma-v2.md` §5.
3. **§4's inference is scoped more narrowly than it is stated.** "Its ABSENCE from a run's log
   is the positive evidence that every draw keyed on a handle"
   (`VulkanRenderer.h:1038-1041`, `magma-v2.md` §2) holds only while bit 0 is set. With bit 0
   clear the warning can never appear *and* no draw is keyed on a handle, so absence proves
   nothing. Worth one clause in the comment, because the integrator will grep these logs.
4. **The identity tables are process-global and never reset, while both consumer tables are
   per-instance.** `MagmaPipeVaoIdentity()` / `MagmaPipeBufferIdentity()`
   (`MagmaPipeArms.h:234-241`) are function-local statics; `m_vaoDrawMemoTable` and
   `m_vaoMemos` belong to a `VulkanRenderer` / `VertexInputStateFactory`. Two live contexts, or
   a context recreation inside one process, share and drive one LRU: no aliasing (lifetime ids
   are globally unique) but halved effective capacity and interleaved eviction, which feeds M1.
   The tables also carry a `Vector::resize` and per-call LRU writes with no synchronisation;
   `s_evictionEpochSource` and the shared `static inline XXH64_state_t* m_hashState`
   (`VertexInputStateFactory.h:184-190`) set that precedent, so this is consistent with the
   file rather than new — but a global that outlives every context is new.
5. **Commit `553065dc`'s subject describes a change it does not make.** "drive the dynamic tail
   from the pushed dynamic version" — `git show --stat 553065dc` is `1 file changed, 98
   insertions(+)`, all of it comment and `static_assert`; `ApplyDynamicDrawStateTail`'s
   `const Uint paramsVersion = MGB_CTX->GetRenderStateParametersVersion();` is untouched. The
   re-sourcing is the contract's (D-2 says so honestly in §5, and the tree is right against
   D12.3), but the commit message claims the deliverable.
6. **D19's `DynamicChunksCoverMagmasDynamicTailKey` ctest entry exists nowhere.** It is
   `static_assert`s in `VulkanRenderer.cpp:399-425` here and a named test in package A's
   `RenderStateSpansTest.cpp` there. `magma-v2.md` §8.7 flags "make sure the outcome is not
   neither"; the integrator must actually close it. (The `static_assert`s themselves are a real
   gate — proved above.)
7. **D17's re-audit of the `AccessorCalls` tally constants was not done.** D17 names
   `VulkanRenderer.cpp:5009,5016,5274,5927,5934,6416,6449,6462`; all eight line numbers have
   moved, and the tallies at the pipeline-memo gate (`:5191`, `:5198`) still say
   `AccessorCalls, 1` on both arms although the handle arm no longer performs
   `ComputePipelineStateHash`'s bulk `GetRenderStateParameters()` fetch. §8.11 lists this
   package's `MEASUREMENTS.md` input but not the re-audit D17 makes a precondition for quoting
   `acc/draw`.
8. **`build-verify` was never configured for this package.** C.3 does not ask for one, and D.3
   part 2 is the authority — but nothing here proves the package compiles under
   `MOBILEGL_PIPE_VERIFY`. I closed that specific hole myself (both changed TUs recompiled with
   `-DMOBILEGL_PIPE_VERIFY=1` from `build-push/compile_commands.json`: rc 0, no warnings), so
   G4 is not at build risk; the applier's verify-only assertions remain unexercised here.
9. **Both assertions this package adds are compiled out in every build P2 runs.**
   `MOBILEGL_ASSERT` is live only when `MOBILEGL_LOG_ACTIVE_LEVEL <= DEBUG`
   (`MobileGL/Defines.h:97-115`); all three build dirs are `MOBILEGL_LOG_LEVEL_INFO`. The
   `Gen`-wrap assert (`MagmaPipeArms.h:175-178`) never runs — consistent with
   `MGPipeHandles.h:52-58`, which says the wrap is defended only in a debug allocator — and
   `1e89fd90`'s null-handle assert (`:214-215`) never runs either; only its release-path
   ternary has effect. The commit's value is the ternary, not the assertion.
10. **Brief-vs-tree items I independently confirmed** (all already listed in `magma-v2.md` §6,
    recorded here so the integrator has a second source): the A-section G2/G14 grep
    `grep -E '^\s+Test #'` returns **1368** of 2367 names, the working form returns 2367;
    `~/w7/retrace_gate.py --help` shows only `--tree --lib --out -j --only`, so G3's `--ssim`
    and C.3's `--backend` do not exist; `MGLOG_F_ONCE` is absent from `MG_Util/Debug/Log.h`;
    and G1/D15's admitted resize set is short of `_GLOBAL__sub_I_DirectGLES.cpp` (−9), which is
    the contract's and appears after every merge.
11. **`MG_Pipe::MGPipeApplier()` is one process-global `g_applier` (`PipeApply.cpp:99, 112`),
    not the per-context CSO store D2 specifies.** `ResolveBoundRenderStateCso`
    (`VulkanRenderer.h:1045-1057`) reads it directly, so in a multi-context process one
    renderer's pipeline memo would key on whatever CSO another context last bound. The defect
    is package A's, but Magma is its only consumer in P2 and the integrator should route it
    rather than let both reviews assume the other caught it.

---

## Things I tried to break and could not

Recorded so the integrator does not re-spend the time.

* **Pipeline-memo aliasing across the two arms.** Handle-arm entries carry
  `pipelineStateHash == 0` and a non-null `renderStateCso`; fallback entries carry a real hash
  and `kMGPipeNullHandle`. The probe tests both components (`VulkanRenderer.cpp:5180-5184`,
  `:6577-6582`) and the insert writes both (`:5851-5854`). No cross-arm match is constructible.
* **Fast path vs full path keying differently.** Both call `ResolveBoundRenderStateCso()` and
  both fall back through `ResolveFallbackPipelineStateHash` with the render-pass facts from
  their own source; the key tuples are identical.
* **`vaoMoved` weakening.** The handle-arm form
  (`VulkanRenderer.cpp:6313-6315`) is strictly *stronger* than the address+lifetime-id form:
  identical VAO ⇒ identical handle unless the identity table evicted it, in which case the
  handle differs and the compare is conservative.
* **Mid-draw handle drift.** Every `Acquire` inside one draw is for the same VAO lifetime id
  (the buffer acquisitions in `ComputeHash` hit a different table), so `MemosFor`,
  `LookupVaoDrawMemo` and `ResolveVaoHandle` cannot disagree within a draw.
* **A stale `VaoBackendMemos` served across an owner change.** `MemosFor` clears on
  `Owner != handle` and `Gen` moves on every owner change, so contents are never inherited.
* **A reference into `m_vaoMemos` invalidated across the nested
  `GetOrCreateVertexInputState`.** The table is fixed-size after its first `resize`, and the
  code re-takes the reference anyway.
* **`GetBackendAuxMemo` losing a reader.** `grep -rn` across `MobileGL/` confirms the getter has
  no caller anywhere, so retiring rather than moving it is correct (D12.5 agrees).
* **Slot-band collisions.** `MGPipeKind::Buffer` and `VertexElementsCso` are distinct kinds;
  `kMGPipeShaderCsoCompositeSlotBase` is a `ShaderCso`-only band; `kMGPipeDefaultFramebuffer`
  is `{0,1}` of kind `Framebuffer`. Slots 1…8192 of these two kinds collide with nothing.
* **`MOBILEGL_PIPE_PUSH=OFF -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF` failing to compile**
  (`VaoContentHashIfKnown`'s `#else` calls `vao.GetBackendHashMemo`, which only exists under
  `MOBILEGL_PIPE_LEGACY_MEMOS`). `CMakeLists.txt:476-479` forces the option ON in that
  combination, so the configuration is unreachable.
