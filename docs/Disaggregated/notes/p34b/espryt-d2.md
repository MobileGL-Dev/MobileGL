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
