# P2 package D — `p2/magma` result (v1)

Tree: `~/w7/p2-magma`, branch `p2/magma`, branched from the contract commit
`9c6a8a25` (tag `p2/contract`). Not pushed.
Build dirs: `build-linux` (pull, the G1 build), `build-push`, plus a fourth I added,
`build-nolegacy` (`-DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF`), because
that is the only configuration in which D12.5's deletions are actually deletions.
No `build-verify` (the tree script was invoked without the third argument, per C.3, which
lists no verify command for this package).

## 1. Commits

| sha | subject |
|---|---|
| `d66f400f` | `[Refactor] (Magma): key the pipeline memo on the render-state CSO handle and stop recomputing a hash the client already computed` |
| `553065dc` | `[Refactor] (Magma): drive the dynamic tail from the pushed dynamic version and make the chunk table check DynamicTailKey's inventory` |
| `391e1230` | `[Refactor] (Magma): key the vertex-input cache and the VAO draw memo on {slot, gen} instead of a lifetime id and a heap address` |
| `fb410351` | `[Refactor] (State, Magma): take the backend's raw pointers out of the frontend VAO - the hash and state memos become the factory's own per-slot fields` |

677 insertions / 39 deletions over six files, all of them this package's by the C.5
ownership table: `MG_Backend/DirectVulkan/Renderer/{VulkanRenderer.h, VulkanRenderer.cpp,
VertexInputStateFactory.h, VertexInputStateFactory.cpp, MagmaPipeArms.h (new)}` and
`MG_State/GLState/VertexArrayState/VertexArrayObject.h`. **No file owned by another
package was touched.**

`MagmaPipeArms.h` is new and is inside `MG_Backend/DirectVulkan/**`, which C.5 gives to
this package; it holds the two-switch arm selector (compile-time `MOBILEGL_PIPE_PUSH` /
`MOBILEGL_PIPE_LEGACY_MEMOS`, runtime `Features.PipePush` bit / `Features.PipeLegacyMemos`)
plus the `lifetimeId -> {slot, gen}` acquisition, shared by m1, m3 and m4.

## 2. What landed, against C.3's four steps

**m1 (`d66f400f`)** — `GetOrCreatePipeline`'s memo is keyed on
`MGPipeApplier().BoundRenderStateCso` when `kMGPipeSubsystemRenderState` is on and a CSO is
bound; `entry.renderPassHash` stays in the key; `ComputePipelineStateHash` and all five
cached-hash members (`m_pipelineStateHash{,Valid,Version,ColorCount,SampleCount}`) are now
`#if MOBILEGL_PIPE_LEGACY_MEMOS` only, and `InvalidatePipelineMemo` loses them on the same
condition. `ResolveEffectiveSampleMask` is deliberately **not** retired (it is payload, not
key). **Both** memo probes are re-keyed: `TrySetupDrawFastPath` carries its own copy of the
probe (`VulkanRenderer.cpp` ~6330 at the base ref) which the brief does not mention; a fast
path that keyed differently from the full path would hand back a pipeline the full path
would not have matched.

**m2 (`553065dc`)** — the tail's version gate is re-sourced (see deviation D-2) and
D12.3's coverage check landed as `static_assert`s in `VulkanRenderer.cpp` instead of a unit
test (deviation D-3), including the three stencil members **per face**.

**m3 (`391e1230`)** — `VertexInputStateFactory::ComputeHash`'s `bufferKey` becomes
`slot | (gen << 32)`; `LookupVaoDrawMemo` becomes a direct slot index with a single handle
compare (no Fibonacci mix, no two-way probe, no frame-serial eviction choice);
`SetupDrawSnapshot`'s `vao`/`vaoLifetimeId` pair collapses to one handle compare. Negative
control C (`MOBILEGL_PIPE_HANDLE_ABA_CONTROL`) is implemented here — see §5.

**m4 (`fb410351`)** — the three `mutable` backend memos leave `VertexArrayObject`; the hash
and state memos become a slot-indexed table the factory owns, the aux memo is retired
outright (no live reader), and the process-wide `s_evictionEpochSource` shrinks to a
per-instance counter in the handle arm.

## 3. Verification — every command run and its actual result

All from `~/w7/p2-magma` unless stated.

| # | command | result |
|---|---|---|
| V1 | `cmake --build build-linux -j 12` | rc 0 |
| V2 | `cmake --build build-push -j 12` | rc 0 |
| V3 | `cmake -B build-nolegacy … -DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF && cmake --build build-nolegacy -j 12` | rc 0 (after one real fix it found: `OnFrameBoundary`'s `s_evictionEpochSource` bump) |
| V4 | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | **0 added, 0 removed, 0 renamed, 4 resized** — and all four resizes are the *contract* commit's: `RenderState::RenderState()` +148, `RenderState::SetCapability` +77, `RenderState::IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9. **This package adds none.** (G1) |
| V5 | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty (G13) |
| V6 | `python3 scripts/check_include_closure.py` | rc 0, `4 probes, 0 skipped, 0 problem(s)` (G13) |
| V7 | `python3 scripts/gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 (G13) |
| V8 | stdio grep over `MG_Backend/DirectVulkan` + `MG_State/.../VertexArrayState` | only the word "puts" inside comments; no stdio call (G13) |
| V9 | `awk '/namespace RenderStateImpl {/,/} \/\/ namespace RenderStateImpl/' …/DirectGLES.cpp \| sha256sum` | `d8fd1c48…0efe27`, **equal** to `~/w7/p2-before-syncrenderstate.sha` (G5) |
| V10 | `ctest --test-dir build-linux -N` vs `~/w7/p2-before-ctest-names.txt` | 2367 names; **0 removed**; 4 added, and all four are the contract's `MG_Test/Pipe` stubs (`{CsoCache,RenderStateSpans,SlotAllocator,Tracker}.PlaceholderUntilTheOwningPackageFillsThisIn`) (G14) |
| V11 | `diff` of the pull and push `ctest -N` name lists | empty — name-for-name identical (G2, the half this tree can answer) |
| V12 | `ctest --test-dir build-linux -L unit` | **1489/1489** |
| V13 | `ctest --test-dir build-push -L unit` | **1489/1489** |
| V14 | `ctest --test-dir build-push -L integration-gpu -j 4 -R DirectVulkan` | **432/432** (default bitmask `0x7f`) |
| V15 | `MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu -j 4 -R DirectVulkan` | **432/432** (the all-pull / legacy-arm control) |
| V16 | `ctest --test-dir build-linux -L integration-gpu -j 4 -R DirectVulkan` | 430/432 — the two failures are a pre-existing flake, see §6 |
| V17 | `python3 ~/w7/retrace_gate.py --tree ~/w7/p2-magma --lib build-push/libMobileGL.so --out ~/w7/retrace-out/p2-magma-push -j 4 --only 'DirectVulkan$'` | **40/40 PASS**, min SSIM **0.996284** (`…iris-sundial-lite-in-world`), max 1.0 — well over the 0.99 gate (G3, DirectVulkan half) |
| V18 | `ctest --test-dir build-nolegacy … -R DirectVulkan` | 180/432; every failure is `Fatal{PipeLegacyMemosDisabled}` — **expected on this tree**, see §5 |
| V19 | `grep -rn 'BackendStateMemo\|BackendAuxMemo\|m_backendHashMemo' MobileGL/MG_State MobileGL/MG_Backend` | every remaining hit is inside a `MOBILEGL_PIPE_LEGACY_MEMOS` arm (`VertexArrayObject.h`, `VertexInputStateFactory.cpp`) or a comment; `ProgramObject`'s is untouched and out of scope as C.3 requires |

**Proof the handle arms are actually taken** (not just compiled). Two probes with
`MOBILEGL_PIPE_LEGACY_MEMOS=0` at runtime on `build-push`, reading `/tmp/*.log` through
`MOBILEGL_LOG_FILE_PATH`:

- `MOBILEGL_PIPE_PUSH` unset (i.e. `0x7f`): the first Fatal names **`GetOrCreatePipeline`**.
- `MOBILEGL_PIPE_PUSH=0x3f` (bit 6, Magma vertex input, cleared): the first Fatal names
  **`VertexInputStateFactory::GetOrCreateVertexInputState`**.

The second probe establishes that the vertex-input site runs *before* `GetOrCreatePipeline`
in a draw. So the first probe naming `GetOrCreatePipeline` proves subsystem 4's handle arm
(m3 + m4) ran and did not need the legacy arm — i.e. the 432/432 and the 40/40 retrace above
exercised the re-keyed code, not the old code.

## 4. Deviations from the brief, with reasons

**D-1 — `ComputePipelineStateHash`'s 20-line enumeration comment did NOT move to
`MGPipeRenderStateSpans.cpp`.** D12.2 asks for that move. That file belongs to package A
(C.5). The comment stays with the function it documents, which is now legacy-arm-only.
*Integrator action: move it if A's file is still open, or leave it — it is provenance, not
contract.*

**D-2 — the tail's version gate is re-sourced but does not become finer-grained.** D12.3
says `GetRenderStateParametersVersion()` "no longer moves on a pipeline-only change". It
still does, and the tree is right against the brief: the contract's
`MGPipeApplyBindRenderState` (`MG_Pipe/PipeApply.cpp`) writes `bind.Version` into
`m_renderStateParametersVersion`, and it *has to* — Espryt's `SyncRenderState` uses that
same counter as its all-state change detector and G5 forbids changing one line of it, so a
bind that rewrote the pipeline half while leaving the counter still would make Espryt skip
re-syncing the state it just changed. Achieving D12.3's finer gate needs a **second,
dynamic-only version on the wire**, which is a contract change and therefore not mine to
make. The existing second-level `DynamicTailKey` value compare already absorbs a
pipeline-only change at the cost of one key build and no `vkCmd*`, exactly as before P2.
*Integrator action: either accept, or add `MGPDynamicState`-only version plumbing in a
follow-up contract commit and revisit this gate.*

**D-3 — D12.3's coverage check is `static_assert`s, not a ctest entry.** D19 puts
`DynamicChunksCoverMagmasDynamicTailKey` in `MG_Test/Pipe/RenderStateSpansTest.cpp`, which
C.5 gives to package A, and registering a new test file would need
`MG_Test/Pipe/CMakeLists.txt`, also A's. A `static_assert` in the file that owns the reader
is in-scope, stricter (build break, not a test failure) and cannot be skipped.

**D-4 — `ScissorTestEnabledMask` is in the PIPELINE half, so D12.3's "every field
`DynamicTailKey` reads lies inside `kMGPipeDynamicChunks`" is false as written.** Verified
from the tree: `MGPipeValueTypes.h:382` puts it between `ColorLogicOpEnabled` (357) and
`ScissorBoxes` (383), i.e. inside chunk P6; and it must be there, because D6's only rule is
"pipeline iff a `BumpVersions()` setter writes it" and `SetCapability(ScissorTest)` does.
The tail reads it only to choose between the scissor box and a full-extent rect, which stays
correct because `BumpVersions()` moves both counters. I pinned it with the assertion
**inverted**, so a later demotion (a real G7 violation) is also a build break. *The four
backend facts the brief excepts — `extentX`/`extentY`/`preTransform`/`isDefaultFbo` — are
excluded as the brief says.*

**D-5 — the `VaoDrawMemo` table stays FIXED at 2048 entries with a wrapping slot index,
where D12.4 asks for "a grow-on-demand `Vector`".** Nothing in P2 frees a
`VertexElementsCso` slot: the frontend death notification is Espryt 0b's `e2` and it covers
Espryt's six kinds; buffers are the only kind with an `OnDestroy` hook today. A
grow-on-demand table would therefore hold one ~1 KB `VaoDrawMemo` per VAO **ever created**,
which on a chunk-cycling Minecraft frame is tens of megabytes — a regression against
today's fixed 1.5 MB. With dense slots the direct index is exact below the table size and
degrades above it to a direct-mapped cache validated by the full `{slot, gen}`: never
wrong, only colder, and still strictly better than the address hash it replaces. The same
reasoning fixes the factory-side memo table at 2048 (96 KB). *Integrator action: revisit
once object deletion reaches the client allocator (P3a/P3b).*

**D-6 — a VAO's `MGPipeKind` is `VertexElementsCso`.** `MGPipeHandles.h` has no
`VertexArray` kind; `VertexElementsCso` is the only kind naming vertex-input state.

**D-7 — Magma acquires the handles itself rather than reading pushed ones.** The tracker
emits for dirty bits 0–4 only in P2 (D4), so no object-class handle reaches the backend
yet. `MGPipeSlots().Acquire(kind, lifetimeId)` is called from the backend, which in the
monolith is legal and in the split becomes a read of what the client already sent. Both
acquisition sites sit behind a one-entry `lifetimeId -> handle` memo so the arm does not
swap the address hash it deletes for a hash probe.

**D-8 — the three `VertexArrayObject` memo triples are `#if MOBILEGL_PIPE_LEGACY_MEMOS`,
not deleted from the file.** D12.5 says "deleted outright". A literal deletion changes the
**pull** build (the struct shrinks, `GetOrCreateVertexInputState` loses statements) and G1
admits no such resize; `MOBILEGL_PIPE_LEGACY_MEMOS` is forced ON in a pull build, so the
guard is exactly equivalent to "deleted, once the pull path retires at P13". The
`-DMOBILEGL_PIPE_LEGACY_MEMOS=OFF` build (V3) is what makes the deletion real, and it
compiles clean.

**D-9 — the eviction epoch is not removed, only de-globalised.** D12.5 reads as if a
slot-indexed table removes the need for an epoch. It removes the need for a **process-wide**
one (`s_evictionEpochSource` exists because the memos outlive the factory); the epoch itself
still guards the *pointee*, which is a cache entry `OnFrameBoundary` can still erase.

**D-10 — negative control C is implemented in this package**, though C.3 does not list it
and C.4 gives `HandleRecycleScenario` to package E. D18 specifies the knob's behaviour in
terms of two guards that live in *my* files (`VertexInputStateFactory::ComputeHash` and
`LookupVaoDrawMemo`), so E's `AbaControl` arm cannot work without it. It is wired to the
**pre-handle** arm, which is the arm that scenario runs (`MOBILEGL_PIPE_PUSH=0`).

**D-11 — `MG_Backend/DirectVulkan` now includes `MG_Pipe/PipeApply.h` and
`MG_Impl/Pipe/SlotAllocator.h`, both under `#if MOBILEGL_PIPE_PUSH`.** No probe in
`check_include_closure.py` forbids it (V6 is green) and D20 anticipates the applier being
reached from the server side, which in the monolith is the backend. Recorded because it is
the first `MG_Backend -> MG_Impl` include in the tree.

## 5. Tree-versus-brief contradictions (beyond the deviations above)

1. **D12.3's premise about `bind_render_state`** — see D-2. The brief's model of the
   applier does not match `PipeApply.cpp` as landed.
2. **D12.3's claim about `DynamicTailKey`'s inputs** — see D-4.
3. **D12.1 lists `VulkanRenderer.h:855, 856, 860, 861, 862` as "the cached-hash fields";
   there are five, not three** (`m_pipelineStateHashVersion`, `…ColorCount`, `…SampleCount`,
   `m_pipelineStateHash`, `m_pipelineStateHashValid`). All five are guarded.
4. **D12.1 does not mention the second memo probe** in `TrySetupDrawFastPath`. It exists and
   is re-keyed identically; leaving it on the hash would have been a silent arm mismatch.
5. **G2/G14's shell command in section A is wrong.** `grep -E '^\s+Test #'` only matches
   4-digit test ids, because `ctest -N` right-aligns the number (`Test   #7:`,
   `Test #1234:`). It silently returned 1368 of 2367 names here and would have reported ~999
   phantom removals. The working form is `grep -E '^\s+Test\s+#[0-9]+:' | sed -E 's/^ *Test
   *#[0-9]+: //'`. **The integrator must use the corrected form**, and should re-check how
   `~/w7/p2-before-ctest-names.txt` (2363 names) was produced — 2363 vs my 2367 is exactly
   the four contract stubs, so that baseline looks correctly captured.
6. **C.3's verification block prints `grep -rn 'BackendStateMemo…' … # only ProgramObject's`.**
   With D-8 in force, the legacy-arm copies also remain; the honest check is "every hit is
   inside a `MOBILEGL_PIPE_LEGACY_MEMOS` arm, plus `ProgramObject`'s" (V19).
7. **`MGLOG_F_ONCE` does not exist** (`MG_Util/Debug/Log.h` has `_ONCE` for D/I/W/E only).
   The Fatal path uses `MGLOG_F` + `std::abort()`, the same shape `PipeApply.cpp` uses.
8. **The `MGLOG_D_ONCE` on the "no CSO bound" fallback is compiled out** at
   `MOBILEGL_LOG_ACTIVE_LEVEL=INFO`, which is what every build here uses. The fallback is
   therefore silent in a normal build and only observable through the
   `MOBILEGL_PIPE_LEGACY_MEMOS=0` Fatal. That is deliberate (no hot-path logging,
   `ROADMAP.md:7`) but the integrator should know the fallback leaves no trace by default.

## 6. Observed flake, not caused by this package

`DirectVulkan.PrimGenReroute.PrimitivesGeneratedNoXfbScenario.TheRerouteIsActuallyArmedWhenTheEnvironmentPinsItOn`
and `DirectVulkan.PointSizeDemotion.PointSizeDemotionScenario.TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn`
failed once in `build-push -j 4` (V14's first run) and again in **`build-linux` -j 4** (V16).
`build-linux` is byte-identical to the baseline apart from the contract's four resizes
(V4), so these cannot be this package's. Both pass in isolation and passed three times in a
row under `-j 4` when run alone, and V14's rerun was 432/432. Reading: a load-sensitive
race in the two "arming-observable" lanes on lavapipe. *Integrator: worth a retry-on-failure
or a serial label, and it should be watched in `integration-gpu` generally, not just here.*

## 7. Gates this tree could not reach

- **G3's DirectGLES half and the 79-case total** — this package is `--only 'DirectVulkan$'`
  by construction; the 40 DirectVulkan cases are green (V17).
- **G4 (verify)** — no `build-verify` on this tree (C.3 asks for none). The re-keys are all
  behind `MOBILEGL_PIPE_PUSH` arms that do not change what any field *contains*, so the
  comparator has nothing new to compare, but the integrator's D.3 part 2 is still the
  authority.
- **G6/G7/G8/G9/G10/G12** — packages A, B and E own those artefacts.
- **G11 and all of D.4** — device work, integrator's.
- **`build-nolegacy` cannot RUN on this tree** (V18). With no legacy arm and no tracker to
  bind a render-state CSO, `GetOrCreatePipeline` has no key and
  `Fatal{PipeLegacyMemosDisabled}` fires at the first draw. That is the D14 gate working —
  the alternative would have been the memo silently aliasing every render state onto one
  entry — but it means **the integrator should re-run `build-nolegacy`'s
  `integration-gpu -R DirectVulkan` once `p2/tracker` has landed**; that run is the real
  end-to-end proof of the CSO key, and it is the strongest single check P2 can make of m1.

## 8. Unfinished / follow-ups for the integrator

1. Re-run `build-nolegacy` after `p2/tracker` lands (above). Until then m1's handle arm is
   compiled and arm-selected but never keyed, because nothing binds a CSO.
2. D-2: decide whether to add a dynamic-only version to the wire so the tail gate becomes
   what D12.3 describes.
3. D-1: the enumeration comment's move into `MGPipeRenderStateSpans.cpp`.
4. D-5: revisit the two fixed 2048-entry tables when object deletion reaches the client
   slot allocator; `MGPipeSlots()`'s `lifetimeId -> slot` map also grows unbounded for kinds
   nothing frees, which is a P2-wide fact, not a Magma one.
5. Correct `BRIEF-P2.md`'s G2/G14 command (§5.5) before the integration script uses it.
6. `MEASUREMENTS.md` input from this package: the pipeline memo now compares an 8-byte
   handle instead of an 8-byte hash and skips `ComputePipelineStateHash` entirely
   (~50 `CombinePipelineStateWord` rounds plus one bulk parameter fetch) on every draw whose
   pipeline-state version moved; `LookupVaoDrawMemo` drops a multiply, a two-way probe and
   one of two compares; `VertexInputStateFactory` drops one `mutable` write per VAO
   reconfiguration (the retired aux memo). None of it is measured here — that is D.4.
