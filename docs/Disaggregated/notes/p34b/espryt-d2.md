# P3b/P4b wave 2-D package D2 — the Espryt stream, part 2

The verification gates P3b/P4b owes, plus the texture / program split-scenario census.
Base `7ed5da52` (`feat/disaggregated`), worktree `~/w7/p7-espryt-d2`, branch `p7/espryt-d2`.

Package D1 owns `MG_Backend/DirectGLES/{DirectGLES.cpp,Managers.cpp}`, the XFB/readback
scenarios and their split-arm registrations. Nothing here touches those; every
`MG_IntegrationTest/CMakeLists.txt` edit sits in a block headed `package D2`.

---

## Slice 1 — `TextureViewAliasScenario`

The verification half of "按存储属主键控的发射游标与 view 索引重映射". The code half landed in
P4a/P5e: the drain cursor is owner-keyed (`MG_Impl/Pipe/TextureEmit.h`, `NoteLevelDirty` +
`DrainTextureSubData`), the index remap is `TextureObjectView::ToOwnerLevel` /
`ToOwnerUploadTarget` / `ToOwnerRegionOffset`, the wire half is `MGPSamplerView` +
`MGPResourceDesc::ViewOf`.

Four cases, each asserting on TWO channels at once — sampled texels AND
`MGPipeTextureEmitter::SubDataCount()` / `RefusedSubDataCount()` / `DrainListSize()`, read through
the new `Harness/TextureEmitPeek.{h,cpp}`:

| case | direction | drains |
|---|---|---|
| `SubImageThroughAViewIsSampledThroughTheOwner` | view upload → owner sample | 1 |
| `SubImageThroughTheOwnerIsSampledThroughTheView` | owner upload → view sample | 1 |
| `TheViewUploadDrainRunsAgainAfterADrawBoundary` | view upload → owner sample | 3 |
| `TheOwnerUploadDrainRunsAgainAfterADrawBoundary` | owner upload → view sample | 3 |

### Red-once (R-16)

`TextureObjectView::MarkStorageDirtyRegion` patched to call `PipeNoteLevelDirty` on ITSELF instead
of forwarding to the storage owner — the two-cursor shape the design rejects.

```
DirectGLES.TextureViewAliasScenario.TheViewUploadDrainRunsAgainAfterADrawBoundary  FAILED
  the first view write, through the owner: 11408 of 11408 pixels disagree;
    first at (2, 2) is rgba(50,160,30,255), expected rgba(255,0,255,255)
  Expected: (window.SubDataDelta()) > (0u), actual: 0 vs 0
    the SECOND drain (first view write after a draw boundary): the client texture emitter
    described 0 texture(s) across this draw and put NO resource_subdata on the wire.
  ... and the same pair again for the THIRD drain.
```

**Both channels red simultaneously**, which is the property the file was written for: stale texels
AND a drain that emitted nothing.

**Deviation from the plan's prescription, recorded rather than papered over.** The plan says
"case (a) must go red". It does not — case (c) does. Case (a) creates the texture, writes through
the view and drains ONCE, and that first drain of a texture's life carries the whole level anyway
(the storage definition already marked it dirty), so a cursor keyed on the wrong object still
ships the right texels by accident. It is the SECOND and THIRD drains, where only the sub-region
written since the last validate point is owed, that can lose a record. Cases (c)/(d) exist for
exactly that and are where the red lands. The scenario header states this; case (a) is kept
because it is the cheapest statement of the remap itself.

### A defect this slice FOUND: owner writes are invisible through a sampled view under split

`TheOwnerUploadDrainRunsAgainAfterADrawBoundary` is **red on inproc, spawn and tcp, green under
monolith**, on the unmodified tree.

Once a view has been SAMPLED, a later `glTexSubImage` through the OWNER's own name never becomes
visible through the view. The client half is provably correct — the case's own counter assertions
pass, so the records were emitted and the applier accepted them.

* The server's per-texture clean gate `BackendTextureObject::IsDrawSyncCleanByRecord`
  (`Managers.cpp:15433`) compares the VIEW's `record.Serial` and `record.PendingUploads`.
* An owner-side `resource_subdata` moves only the OWNER's (`MG_Pipe/PipeApply.cpp:1175`).
* Nothing propagates a serial bump from a storage owner to the records that view it.
  `MGPResourceDesc::ViewOf` is a forward edge with no reverse index, and the applier reads it only
  in the descriptor-equality helper (`PipeApply.cpp:666`).
* Under monolith the same gate reads `GetContentVersion()` THROUGH the view, which
  `TextureObjectView` forwards to the owner (`TextureObjectView.cpp:100-102`). The split gate has
  no equivalent of that forwarding.
* `MG_Remote/CONTRACT-P5E.md:387-395` states the split gate in full and has the same hole, so the
  implementation is faithful to the contract. **The contract is what is wrong.**

`KHR-GL43.texture_view.coherency` is exactly this test and reads Pass in the CTS baseline — which
is a **monolith** reading (`notes/p7/device-window-1/CTS-base/README.md:16`).

**Not fixed here.** The fix is either at the gate (`Managers.cpp` — package D1's file) or at the
publisher (`MG_Pipe/PipeApply.cpp`'s `ApplyTextureUpload`, outside D2's declared footprint), and
the publisher-side form needs either an O(#texture records) scan per `resource_subdata` or a new
reverse index on `MGPipeResourceRecord` — a design decision with a contract amendment attached.
The case is **excluded by name from the three split arms** with the cause stated in the CMake
comment, kept in the monolith lanes where it is the regression net for the forwarding that works,
and listed in `split-scenario-census.md`. G14: the name still exists in every lane.

### The tcp arm was green-skipping everything

`MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1` on `MGL_ITEST_GLES_TCP_ENVIRONMENT` is not enough: the tcp
lane's server is **pre-started by the `TcpServer.Start` fixture**, not spawned from a client that
carries the lane environment, so the server withheld `GL_ARB_texture_view` and all four cases
SKIPPED while the identical Split and Spawn arms ran. Fixed by putting the knob on the fixture
entry's own `ENVIRONMENT` property (`tcp_server_fixture.py` builds the supervisor's environment
from `os.environ` and strips only the transport keys). A feature knob that reaches two arms of
three is the worst of the three states: the lane is green and tests nothing.

---

## Slice 2 — `TextureUploadShapeScenario` promoted from record to gate (R-11 / D-D4)

`RecordProperty` → `EXPECT_EQ` against a gold row measured on this cursor, plus a third texture
uploaded through a `glTextureView` so the view/owner remap is inside the counted workload.

### The gold row

Workload constants `kScatteredRects=40`, `kFrames=3`, one contiguous band, three textures.

| | emit | box | rect | jobs | client emitter |
|---|---|---|---|---|---|
| **gold (this cursor)** | **9** | **9** | **0** | **9** | **9** |
| P4a's record (two textures, no client emitter) | 6 | 6 | 0 | 6 | ctu=0 |

Identical on all four arms — `DirectGLES.TextureUploadShape.` (monolith), `.Split.`, `.Spawn.`,
`.Tcp.`.

### The client half is read from the emitter, not from `ctu=`

Measured, and it is a correction rather than a preference:

| arm | where `tex[]` is | where `ctu=` is |
|---|---|---|
| monolith | the one log | same line |
| inproc | the SERVER's log (two files, one process, one counter set) | same line |
| spawn / tcp | the SERVER's log | the CLIENT process's own counters, on a **different Present cadence** — measured `ctu=13` against `emit=9`, and usually not yet written at all when the case reads |

So `ctu == emit` is not a well-posed comparison under spawn/tcp from the summary lines. The log
field is kept as a RECORD and asserted only where the two are window-aligned (decided by asking
`PeekSplitRuntime()` which transport resolved — **not** by testing whether the number is >= 0,
which was this case's first mistake: the server publishes a perfectly readable `ctu=0` under
spawn and the comparison silently became `0 == 9`). The ASSERTION reads
`MGPipeTextureEmitter::SubDataCount()` in-process, which is the same counter's source and is the
client's true count in every arm, because the test process is the client under all four.

`Harness/PipeStatsWindow.h` gains `LastFromClientLog()` / `LastFromServerLog()` for this.

### Red-once (R-16)

**The prescribed flip does not reach this workload, and that is a finding.** Flipping
`summedArea * 4 >= unionArea * 3` in `MipmapStorage::GetDirtyRects` leaves the gate GREEN. Four
attempts, in order, all green:

1. flip `summedArea*4 >= unionArea*3` → green
2. `rects.size() < 2` → `rects.empty()` → green
3. remove the union-box re-seed in `MarkDirtyRegion` → green
4. `MOBILEGL_ESPRYT_DISABLE_UNPACK_RING=1` alone → green

The reason is two gates downstream of the predicate, either of which alone forces the box arm:

* `Managers.cpp:8764` `rectShape = subRectEligible && dirtyRectCount >= 2` — the SERVER needs at
  least two rects, and the client's list collapses to one for this workload; and
* `Managers.cpp:8652` `if (BufferImpl::UnpackRingAvailable()) dirtyRectCount = 0;` — **by design**:
  "through the unpack ring every glTexSubImage is a GPU copy job (Mali), so ~100 sprite rects
  become ~100 jobs whose fixed cost dwarfs the union box's extra bytes… One box, one job."

So `box=9 rect=0` is not an accident of the scatter pattern: it is the unpack-ring policy's
intended output, and no client-side predicate can move it while the ring is available. The gold
row pins the policy's output, which is the right thing to pin.

The red-once that DOES bite makes `GetDirtyRects` hand back the union box cut in two AND disables
the unpack ring:

```
DirectGLES.TextureUploadShape. ... FAILED
  the BOX/RECT split moved: 6 box emissions against a gold of 9
  the BOX/RECT split moved: 3 rect-list emissions against a gold of 0
  the JOB count moved: 12 against a gold of 9
  tex[emit=9 box=6 rect=3 jobs=12]
```

and all 26 pixel cases of `TextureView` / `TextureViewAlias` / `SampledSetStaleness` /
`ImageSizeAfterRespec` / `CopyImageLevelRange` / `ClearThenReadPixels` stay GREEN under the same
patch — the required property, and the point of the gate: SSIM and every pixel assertion are blind
to a shape that inverted.

### The Mali frame-time delta is NOT published

D-D4 asks for the +6 ms/frame reading beside the gold. **Not schedulable**: this phase has one
device, a Redmi with an Adreno, and no Mali part. Recorded as a declared deviation in the scenario
header and in `docs/Disaggregated/MEASUREMENTS.md` as "Adreno-only; Mali delta deferred" rather
than modelled or copied from the 2026 measurement's conditions. What the gate pins is the SHAPE,
on the reasoning that the shape is the cliff's cause and the frame time is its consequence on one
vendor's hardware.

### Stale prose corrected

All three said the client emitter had not landed. `MG_Impl/Pipe/TextureEmit.h`'s `EmitOneLevel`
increments `CallClass::ClientTextureUploadEmissions` on every piece it emits, and the build's own
probe hits (`MGITEST_PIPE_CLIENT_TEXTURE_UPLOAD_EMITTER_PRESENT=1` is in every lane's environment).

* `Scenarios/TextureUploadShapeScenario.cpp` header and the client-half comment block
* `MG_IntegrationTest/CMakeLists.txt`, the probe's comment ("Once package B's texture emitter
  lands"). The `else()` arm is KEPT — it is what makes the arming decision falsifiable in both
  directions — but it is unreachable on this tree and now says so.
* `notes/p4a/p4a-results/gates-v1.md`'s re-run table

---

## Slice 3 — the memo purity gate

`scripts/ci/espryt_memo_purity.py`, a sibling of `link_seam_purity.py`, wired into `test.yml`
beside it. Scoped to the BRACES of the families the plan names (`ResolvedTextureBindingMemo`,
`SamplerPassMemo`, `UnitSamplerLookupMemo`, `TwinLookupMemo`, `StateBackendObjectRegistry`) plus
the image sweep's seven file statics, because these two files pass frontend pointers around
legitimately in hundreds of places and a file-wide grep would be a list of exceptions.

Guard-awareness is a small conditional walker, not a preprocessor: it recognises
`#if MOBILEGL_PIPE_LEGACY_MEMOS`, `#if !MOBILEGL_BUILD_DISAGGREGATED` and the `#else` of
`#if MOBILEGL_BUILD_DISAGGREGATED`, which is every spelling these files use.

**Six allow-list entries, each with its reason.** The two that matter:
`StateBackendObjectRegistry`'s `UnorderedMap<StateObject*, Entry>` (`Managers.h:330`, and the
`m_entries` member behind it) and `ResolvedTextureBindingMemo`'s type-erased `const void* program`
are the PULL build's only arms — deleting them moves pull `.text` and fails G1, so they retire
with the pull path (P13). The other four are memo VALUES rather than keys.

**Red-once (R-16)**: 7 negative controls in `--self-test`, 4 positive controls (a
`LEGACY_MEMOS` arm, the `#else` of a `DISAGGREGATED` arm, the allow-listed type-erased key, and a
frontend pointer outside every memo family) — plus one taken against the real tree: injecting
`MG_State::GLState::ProgramObject* regressionKey` into the actual `ResolvedTextureBindingMemo`
reddens the gate and names `DirectGLES.cpp:7266`.

**What the sweep found that is NOT in scope**: seven unguarded frontend-pointer-keyed containers
under `MG_Backend/DirectVulkan` (`VkTextureManager`'s five `TextureIdentity`-keyed containers,
`VkClearManager`'s two, `VkRenderPassManager`'s two raw `RenderbufferObject*` maps, and
`VulkanRenderer::VaoDrawMemo`'s `vaoKey`+`vaoLifetimeId` pair sitting beside the correctly fenced
handle key). Magma's key inventory is P7 wave 2's, so the gate does not look there; recorded here
so the next pass meets a known list.

---

## Slice 4 — R-5 (D-K2's dependency rule in one place) and R-3

### R-5

The rule had **six** statements and nothing compared them, and **two of the four prose ones were
wrong in the same way**: `MG_Pipe/MGPipe.h:115-125` and `MG_Backend/DirectGLES/Managers.h:690-695`
both said "THREE OF THEM HAVE A DEPENDENCY" (there are six rows) and both said "the mirror pairs
(10 without 11, ...) are all fine" — when 10-without-11 is precisely D-K2's FOURTH row
(ID-14/ID-15), refused by the server and withheld by the client, with `PipeFill.cpp` saying so out
loud: "The brief's original 'bit 10 without 11 is fine' is WITHDRAWN for P4a as built." Neither
wrong comment could fail anything. `Managers.h`'s copy is in the header of the file that
implements the refusal.

**The rows, once**, in `MG_Pipe/SubsystemDeps.def` (X-macro, `FatalFamilies.def`'s convention),
expanded by `MGPipe.h` into `kMGPipeSubsystemDependencies[]` / `MGPipeSubsystemRequires()` /
`MGPipeSubsystemDependenciesAreSet()`, with four `static_assert`s over it:

| family | requires |
|---|---|
| bit 8 VertexInput | bit 7 |
| bit 9 Framebuffer | bit 10 |
| bit 10 TextureResources | bit 7 **and bit 11** |
| bit 11 Samplers | bit 10 |
| bit 12 Programs | — (a ROW, not an absence) |
| bit 13 BufferBindings | bit 7 |

Bits 8 and 13 were the two the old comment did not know about; bit 13's row lived fifteen lines
further down in `MGPipe.h`, which is how "THREE OF THEM" survived two phases.

**The two readers do NOT yet read it, deliberately.** `MG_Impl/Pipe/PipeFill.cpp` is the contract
package's file for the whole phase (`TextureEmit.h` states that rule) and
`MG_Backend/DirectGLES/Managers.cpp` belongs to package D1 while it is in flight. Switching them
is two one-line changes for the integrator. **The anti-drift property does not wait for it**:
`MG_Test/Backend/DirectGLES/SubsystemDepsTest.cpp` is the first thing in the tree that calls BOTH
readers at the same mask, sweeping the phase constants, the two documented refusal lanes and one
mask per row, and compares each against the `.def`. It is in that binary because that binary is
the one that links both sides.

Both comparisons are **one-directional**, and each for a measured reason:

* the server resolvers also fold in the family's own bit and (for the framebuffer one) D-C3's
  separate wire-caps refusal, so a resolver may say NO for a reason of its own but may never say
  YES at a mask the table refuses;
* `MGPipeP4aFamilyEmits` is a QUADRUPLE — the operator's bit, the wired constant, a registered
  consumer, and the dependency rule — and the first version of this case asserted equality and
  reddened at `0x1fff` on the sampler and program families because a bare unit binary registers no
  consumer. That was the assertion being wrong, not the product; it is recorded because the
  temptation to assert equality here is strong and wrong.

**Red-once (R-16), twice:**

1. drop D-K2's fourth row from the `.def` → **compile-time** failure, `MGPipe.h:225` and `:230`
   ("D-K2's fourth row (bit 10 requires bit 11) has gone missing again").
2. make the SERVER's `ResolveSamplerSubsystemArm` stop checking bit 10 (throwaway patch to
   `Managers.cpp`, reverted) → `TheServerConsumerGateNeverArmsAFamilyTheTableRefuses` reds and
   names the mask: "at MOBILEGL_PIPE_PUSH=0x9ff the SERVER's ResolveSamplerSubsystemArm ARMED the
   handle arm for family 0x800 while MG_Pipe/SubsystemDeps.def says its dependency bits are not
   all set." `0x9ff` is exactly the lane `CMakeLists.txt` already pins for G12.

Stale prose corrected in `MGPipe.h` (both blocks), `MG_Impl/Pipe/PipeFill.h` and
`MG_Impl/Pipe/TextureEmit.h`. **`MG_Backend/DirectGLES/Managers.h:690-695` is NOT corrected here**
— it is package D1's file. Its two defects are named above so the integrator can take them in one
line.

### R-3 (the cheap half)

`MG_Remote/CONTRACT-P5.md` already claimed the image-access, sampler-view-target and draw-buffer
encodings as contract, but did so **by pointing at package-header line ranges, and all three had
drifted** — `ImageEmit.h:146-159` no longer holds the encoding at all, `SamplerEmit.h:900` is off
by six lines, `FramebufferEmit.h:146-163` by one. That is the exact failure the table's own
`MGPSubData::Target` row warns about.

All three rows now state the encoding **by enumerator and function name** rather than by line, and
the two confusable siblings are named out loud: `MGPImageBind::Access` carries the raw GL token
(not this encoding), and `SamplerEmit.h`'s `SamplerTarget[]` is a different, biased encoding of the
same enum living 200 lines from the wire one.

**The remaining half of R-3 is stated, not closed**: `MGPSamplerView::Target` has no `Count`
bound, no `...IsValid` predicate and no `Fatal{ProtocolCorruption}` for an out-of-range value,
unlike `MGPipeResourceTarget`. Giving it the `MGPipeImageAccess` treatment in
`MGPipeValueTypes.h` is a `MG_Pipe` change outside this package's declared footprint.

---

## Slice 5 — the raw-depth-fetch sampler, step 1 only

Step 1 done: `ContextEpochTest` gains a case that drives `GetRawDepthFetchSampler()` on BOTH arms
in one process and asserts they are two different `BackendSamplerObject`s — the observable form of
`DirectGLES.cpp:334-336`'s prose claim that the legacy pair "is not constructed or consulted by
the server".

**Step 2 NOT done, and the package's own condition is why.** Every deletion candidate — the two
file-static `SharedPtr`s (`DirectGLES.cpp:82-83`), the monolith arm (`:361-371`),
`NeedsRawDepthFetchSampler` (`:393-399`) and the pre-handle sampler-pass branch (`:8015-8024`) —
is UNCONDITIONAL code. None sits behind `#if !MOBILEGL_BUILD_DISAGGREGATED`; the pull build
(`DISAGGREGATED=OFF`, `PIPE_PUSH=OFF`) compiles and links all of it, and the monolith arm is the
ENTIRE body `GetRawDepthFetchSampler` has there. Deleting any of it moves pull `.text` and fails
G1's 0/0/0/0 — `ARCHITECTURE.md:340` states that rule for exactly this class of
retired-but-compiled code. Filed as **P13**, beside the existing S7 entry in
`notes/p5f/f0-statics.md`.

---

## Slice 6 — the census

`notes/p34b/split-scenario-census.md` carries the table. Twelve scenarios newly gating in the
three GLES split arms; `ProgramPipelineScenario`'s nine non-storage-block cases deliberately not
registered (ScenarioFixture's armed-lane rule); DirectVulkan arms deliberately none, with the
tier-2 informational replay named as the reason it is not a coverage hole.
