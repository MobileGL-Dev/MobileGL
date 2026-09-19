# b1 — adversarial review, v1

Reviewed `p5/b1` `5175f9cc..5c45ec2a` (8 commits) in `~/w7/p5-b1`, against
`~/w7/notes/p5/BRIEF-P5.md` §5 b1 / R-6 / R-11 / E3, `MobileGL/MG_Remote/CONTRACT-P5.md`
(which outranks the brief), and `~/w7/notes/p5/scout-gpu-writes-and-persistent-map-push.md`.
All four builds re-configured and re-built by me; all gate numbers below are my own re-runs,
not b1's (`~/w7/p5-b1rev-gate.log`, `~/w7/p5-b1rev-g2.log`).

**Counts: 1 BLOCKER, 7 MAJOR, 6 MINOR, 2 QUESTION.**

---

## BLOCKER

### B-1 — retiring `frontend->IsMapped()` re-commits the exact regression `SanityTest.cpp:4583` exists to remember

`Managers.cpp:2695-2707` makes the last frontend read conditional:

```cpp
const Bool askTheObjectWhetherItIsMapped =
    MG_Config::Transport == MG_Config::TransportMode::Monolith;
if (askTheObjectWhetherItIsMapped && frontend != nullptr && frontend->IsMapped()) return false;
```

The pre-existing comment two screens above it (`Managers.cpp:2661-2664`, **not** b1's text) states the
precondition for doing this: *"The frontend read retires the moment P5 gives `HasLiveHostWrites` a
producer."* b1's producer does not satisfy that precondition, because it is unreachable for the buffer
class the read was protecting.

The chain, every link verified:

1. **The rising edge deliberately emits no record.** `BufferObject.cpp:436-446`
   (`NotePersistentMapStateChanged`): `if (live) { /* RISING EDGE COSTS NOTHING */ return; }`.
   So at `glMapBufferRange(WRITE|PERSISTENT|COHERENT)` the server's
   `MGPipeResourceRecord::HasLiveHostWrites` stays **false**.
2. **The only producer of the bit is a content record.** `PipeFill.cpp:709`, inside
   `MGPipeEmitResourceSubData`. `MGPipeEmitBufferSubDataResident` does not set it (see M-2), and
   `MGPipeEmitResourceFlushRange` does not go through `ApplyBufferWrite` at all.
3. **The only content record a coherent persistent map produces is the push**, and the push runs from
   `BufferObject::SyncPersistentMappedRange` (`BufferObject.cpp:369-381` → `PushBlocksFor`).
4. **For a vertex / uniform / SSBO persistent map that call lives inside the ensure path** —
   `Managers.cpp:2841` (`EnsureBufferResourceForHandle`) and `Managers.cpp:3018`
   (`EnsureBufferResource`). `SanityTest.cpp:4578-4582` says so verbatim: *"…and with it
   SyncPersistentMappedRange, the ONLY per-draw push for a vertex/uniform/SSBO persistent map"*.
   The other seven Espryt sites are index / indirect / parameter buffers
   (`DirectGLES.cpp:361, 6185, 6439, 6440, 6541, 6542`, `MultiDraw.cpp:511`).
5. **A "clean" answer skips the ensure.** `DirectGLES.cpp:691`:
   `if (IsBufferDrawCleanByHandle(entry.handle, entry.resource, entry.frontend)) continue;`
6. **And then it latches.** `DirectGLES.cpp:697`: `memo->vboCleanEpoch = allClean ? bufferEpoch : 0;`.
   A coherent persistent map bumps no mutation epoch (that is the whole point), so once every entry
   probes clean the probe loop itself is skipped on every later draw.
7. **`PushPersistentMapsBeforeVerb()` cannot break the cycle**: it has no caller anywhere in the tree.
   ```
   $ grep -rn "PushPersistentMapsBeforeVerb\|MarkGpuWritesForDraw\|MarkGpuWritesForDispatch" MobileGL \
       --include=*.cpp --include=*.h | grep -v "MG_Remote/Client\|MG_Test"
   MobileGL/MG_State/GLState/BufferState/BufferObject.cpp:375:  // ... retire into ... at P8.
   ```
   (one comment, zero calls)

**Failure scenario.** `glBufferStorage(GL_ARRAY_BUFFER, n, nullptr, MAP_WRITE|MAP_PERSISTENT|MAP_COHERENT)`,
`glMapBufferRange` (declined by R-6, so the shadow is emulated), attributes pointed at it, then per frame:
memcpy through the pointer, `glDrawArrays`. Draw 1 rebuilds the memo and calls `EnsureBufferResource`
unconditionally (`DirectGLES.cpp:721`), so the first frame pushes. Draw 2 probes: record bit false,
`record->Serial == syncedChangeSerial`, `pendingRanges` empty, size matches → **clean** → `allClean` →
`vboCleanEpoch` stamped. Draw 3 and every frame after it skip the probe entirely, never reach
`SyncPersistentMappedRange`, never emit a block, never move the serial. **The frame draws frame 1's
bytes, for ever, with no diagnostic** — the C-1 shape, at the transport layer, for the single idiom
(a persistently mapped streaming arena behind a static VAO) the whole feature exists to serve.

b1's new case does **not** catch it: `SanityTest.cpp:4696-4735`
(`UnderSplitTheRecordAloneAnswersTheLiveHostMapQuestion`) writes `record.HasLiveHostWrites = true` by
hand. Delete `PipeFill.cpp:709` entirely and that test still passes.

**Fix.** One of: (a) do not retire the frontend read in the same package that leaves the producer
unreachable — keep it until the push is wired ahead of the probe; (b) publish the bit on the **rising**
edge with a real record (the falling edge already proves a one-block state record is affordable);
(c) have `IsBufferDrawCleanByHandle` under split ask
`MG_Remote::Client::PersistentMapTracker::IsLivePersistentMap` for the handle — but that re-introduces a
client read on the server side and is only legal in inproc; (d) wire
`PushPersistentMapsBeforeVerb()` at the validate point now rather than deferring it to c1, so the push
no longer depends on the probe it feeds. Whichever is chosen, the test that pins it must drive the
**producer chain** (map → push → apply → probe), not a hand-written record.

---

## MAJOR

### M-2 — `buffer_subdata_resident` silently clears the bit

`PipeApply.cpp:921` assigns rather than merges:

```cpp
stored->HasLiveHostWrites = record.HasLiveHostWrites != 0;
```

`ApplyBufferWrite` has two callers — `resource_subdata` (`PipeApply.cpp:1787`) and
`buffer_subdata_resident` (`:1823`) — and only the first emitter sets the field.
`MGPipeEmitBufferSubDataResident` (`PipeFill.cpp:725-745`) builds `MGPSubData record{}` and never
touches `HasLiveHostWrites`, so every `glBufferSubData` against a live-write-mapped buffer writes
**false** into the server's state. b1's own comment at `PipeApply.cpp:923-928` argues the assignment is
right because "a content record emitted while nothing maps the buffer is exactly how the state goes back
to false" — `buffer_subdata_resident` is a content record emitted while something *does* map it.

Failure: a persistently mapped arena that also takes `glBufferSubData` (legal, and the ordinary
Flywheel/Create shape) ends every such call with the server believing no host writer is live. With B-1
fixed, this is the second door to the same draw-clean answer.

Fix: set the bit in `MGPipeEmitBufferSubDataResident` too (one line beside `PipeFill.cpp:709`), or add a
`record.DeclaresLiveHostWrites` presence bit so the applier only overwrites from records that speak.

### M-3 — `pmap` **does** double-count against `stage-buffer`, and the inventory now asserts it does not

`PipeStats.cpp:33-35` (unedited, pre-existing) says of `stage-buffer`:
*"NOT covered: bytes an app writes THROUGH a persistent map. Those never pass through either backend
(D4/D-B4)"* — true only while adoption survives. R-6 kills adoption, so a pushed block becomes an
ordinary `resource_subdata`, reaches `Ops_H_SubData` (`Managers.cpp:1960-2000`), is queued into
`pendingRanges`, and is staged with `AddBytes(ByteClass::StageBuffer, …)` at `Managers.cpp:1121`
(upload ring), `:1224`, `:1282`. The same bytes are already counted as `PersistentMapPush` at
`BufferObject.cpp:404-412`.

b1's new text at `PipeStats.cpp:78-80` states the opposite: *"NOT double-counted with stage-buffer"*.
The inventory is contract; it is now wrong in exactly the configuration the counter was wired for, and
§8's "`pmap` 与 `mpr` 的对照" measurement inherits the error.

Fix: amend the `stage-buffer` entry to say it *does* cover the pushed bytes under split (and drop the
"NOT double-counted" sentence), or subtract the push at the staging site.

### M-4 — `MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0` does not turn the push off, so E3(a) can go green

`BufferObject.cpp:456-459`, the falling edge:

```cpp
const Uint64 blockBytes = MG_Remote::Client::PersistentMapTracker::BlockBytes();
const SizeT length = blockBytes == 0 || blockBytes >= static_cast<Uint64>(m_size)
                         ? m_size : static_cast<SizeT>(blockBytes);
NotifySubData(0, length);
```

With the knob at 0, `PushBlocksFor` correctly ships nothing (`PersistentMapTracker.cpp:88`) — but the
unmap ships the **whole buffer**. `Config.h:483-485` and `CONTRACT-P5.md` §5 both say 0 means the push
is off and `PersistentCoherentMapScenario` *must go red*; if t1's scenario unmaps before it reads back,
it goes green and the negative control is dead. The unit case that claims to pin this
(`SplitBufferSet.AZeroBlockSizeTurnsThePushOffRatherThanMakingItUnlimited`,
`SplitBufferTest.cpp:299-315`) only drives `PushBlocksFor` and never touches the falling edge.

Fix: `if (blockBytes == 0) return;` before the falling-edge emission, and extend the unit case to
`ReleaseMemory` under the knob.

### M-5 — the one new site that is **not** gated on `Transport != Monolith` (D-J)

Every other new site pairs `#if MOBILEGL_BUILD_DISAGGREGATED` with a transport test. `GL_Drawing.cpp:1321-1327`
does not:

```cpp
#if MOBILEGL_BUILD_DISAGGREGATED
            buffer->SyncGpuWrites();
#endif
```

inside `FixupGsStripCaptureOrder`. In `build-split` running `MOBILEGL_TRANSPORT=monolith` on Magma —
whose `VulkanRenderer.cpp:11618` marks the capture targets — `m_gpuWritePending` is set, so this now
takes the monolith arm of `SyncGpuWrites`, emits a whole-buffer `MGPipeEmitResourceReadback`
(`BufferObject.cpp:508-518`) per capture buffer and bumps `m_changeSerial` through
`WritebackFromBackend`, forcing a re-upload on the next draw. The fence at `:1390` still runs in that
arm, so the readback is pure new work. D-J forbids new behaviour on the monolith path, and
`integration-gpu` does not observe it (build-split monolith is 1117/1117 either way).

Fix: `if (MG_Config::Transport != MG_Config::TransportMode::Monolith) buffer->SyncGpuWrites();`

### M-6 — the tier-1 widening refusal cannot fire from production code, and R-11's actual deliverable did not land

`Managers.cpp:1137-1139`:

```cpp
const GLbitfield access = mapUsable ? InvalidateFlushAccessFor(start, end, start, end,
                                                              limit, resource.storageSize) : 0u;
```

The map extent and the queued extent are the **same two variables**, so the refusal branch at
`Managers.h:927` (`if (mapStart != queuedStart || mapEnd != queuedEnd) return 0u;`) is unreachable at
the only call site; it can only be exercised by the unit cases calling the function directly
(`SplitBufferTest.cpp:425-436`). Meanwhile the charter's real item — *"`hostBase` 的『每次使用重求值』
在 split 下变成『发射时快照进 `SEG_STAGE`』"* — is **not implemented**: `Managers.cpp:2752-2758`'s
`liveHostBase` lambda is byte-for-byte unchanged, and `Ops_H_SubData` still parks a client pointer in
`GLESBufferResource::hostBytes` (`Managers.cpp:1980-1983`). (That last one is v1's per BRIEF:551, fine.)

So: the answer to "is b1's snapshot narrower-or-equal to the range it claims?" is *there is no
snapshot*. What landed is a refactor with identical behaviour plus a dead guard and five unit cases.
Nothing in the tree forces w1 to pass a real snapshot extent when it writes one — no `static_assert`,
no TODO trip, no failing test. b1 discloses this in its §5, but the brief listed it as a deliverable and
no integrator ruling moved it.

Fix: either land the snapshot and pass its real extent as `mapStart/mapEnd`, or record an integrator
ruling moving the item to w1 **and** leave a build-breaking marker (e.g. an
`#error`-guarded `MOBILEGL_IPC_STAGE_SNAPSHOT` capability constant) so w1 cannot land the snapshot
without wiring the extent.

### M-7 — `AwaitBufferWriteback` is an unconditional no-op with no tripwire

`GpuWritePending.cpp:158-170`:

```cpp
void AwaitBufferWriteback(BufferObject& buffer) {
    (void)buffer;
    ...
    if (ClientSession::Active() == nullptr) return;
}
```

`ClientSession::Active()` is `{ return nullptr; }` (`ClientSession.cpp:30`), and the function has no
non-null branch — so it is a no-op today *and* a no-op the day a session exists. `SyncGpuWrites`
(`BufferObject.cpp:497-529`) then falls to
`if (m_gpuWritePending && !BufferWritebackIsReachable(*this)) m_gpuWritePending = false;`, which leaves
the flag **set** whenever a readback route exists, and returns to a caller that proceeds to read the
un-refreshed shadow. Every consumer of the "third state" — including M-5's XFB fixup, which is the one
place the removed `ClientWaitSync` guarantee had to be replaced — therefore reads stale bytes as soon as
the apply side stops being synchronous. Nothing fails if the wait is never written.

Fix: make the empty case loud — an `MGLOG_F`/`MOBILEGL_ASSERT` on "session active and the answer has not
landed", or a `PipeStats` counter that a named test asserts stays zero, so the hole has a red spelling
before s1/c1 land.

### M-8 — the pin is lifted only in a configuration nobody builds, so the new bit is never compared or tripped anywhere

`PipeApply.cpp:845` and `Managers.cpp:2677` are now
`#if MOBILEGL_PIPE_VERIFY && !MOBILEGL_BUILD_DISAGGREGATED`. The build matrix
(PACKAGE-PREAMBLE §"构建配方") has **no VERIFY+DISAGGREGATED directory**: `build-verify` is
`MOBILEGL_PIPE_VERIFY=ON` without the transport, `build-split` is PUSH+DISAGG without VERIFY. So:

* in `build-verify` nothing can set the bit — `PipeFill.cpp:709` is inside
  `#if MOBILEGL_BUILD_DISAGGREGATED` — and the G4 field comparator (which *does* see the new field:
  `MGP_FIELDS_MGPSubData` at `PipeFields.def:176`, `MGPSubData` at `MGP_VERIFY_PAYLOAD_LIST`
  `PipeFields.def:352`, expanded at `gen_pipe.py:876`) compares 0 against 0, vacuously;
* in `build-split` neither the pin nor the assert is compiled;
* `ResourceEmit.TheLiveHostWritesWireFiresOnTheCallAPersistentMapProducerWouldSetItOn`
  (`ResourceEmitTest.cpp:1253`) skips in both push and split.

The wire tripwire for the field is therefore **effectively deleted**, not lifted, in every build where
the field can be non-zero. This is the *mechanism* that lets B-1 and M-2 be invisible.

Fix: add a `build-verify-split` lane (VERIFY+DISAGGREGATED) to the gate skeleton and keep the pin
meaningful by inverting it there — assert the bit is set **iff** the emitting object said so — or move
the invariant into a split-buildable assertion.

---

## MINOR

### m-9 — the "same commit" claim is false
The brief (§5 b1) requires the `AddBytes` wiring and the `PipeStats.cpp:68-70` inventory rewrite in the
**same** commit. They are three commits apart: the `AddBytes` call is in `52d2d5dd`
(`BufferObject.cpp` only — `git show --stat 52d2d5dd`), the inventory in `d9fb5273`
(`PipeStats.cpp` + `MEASUREMENTS.md`). b1's report §1 row 5 states "rewritten **in the same commit**".
The tree at HEAD is consistent; the report is not.

### m-10 — two comments describe a carrier that does not exist
`BufferObject.cpp:424-429`: *"it rides MGPResourceDesc's last pad byte as a METADATA field (ID-18 M4),
so a respecify that moves only it allocates nothing, acks nothing and clears no pending upload"*, and
`BufferObject.h:217-219`: *"What MGPipeBuildResourceDesc writes into
MGPResourceDesc::HasLiveHostWrites"*. Commit `450e21cc` moved the bit to `MGPSubData`;
`MGPResourceDesc` has no such member and `MGPipeBuildResourceDesc` writes nothing. Both survived.

### m-11 — "the adoption path notes the change itself" — it does not
`BufferObject.cpp:815-820` justifies publishing before the adoption attempt by saying the adoption path
withdraws the entry. `NotePersistentMapStateChanged` has exactly four call sites (`:188`, `:331`,
`:735`, `:820`); `AdoptPersistentMap` is not one of them. Self-healing today only because
`PushBlocksFor` re-checks the predicate (`PersistentMapTracker.cpp:76-83`); the published bit does not
self-heal and will stay `true` across an adoption until unmap. Latent for P11.

### m-12 — R-6's other half was neither done nor handed off
R-6: *"`LargeArenaAdoptionScenario`（断言发生了采纳）在 split 车道下改成断言 **decline** + `mpr` 不变"*.
Not done (b1 touched no `MG_IntegrationTest` file — correctly, it is t1's) and **not mentioned** in
b1's §4 "what the other packages need from me", which lists only `PersistentCoherentMapScenario`.
`LargeArenaAdoptionScenario.cpp:275/299/344/389/407/450` all depend on an adoption that R-6 now makes
impossible; they will go red the first time a split lane runs inproc.

### m-13 — unsynchronised process-wide state
`g_producerMarks` (`GpuWritePending.cpp:28`) is a plain `Array<Uint64,…>` incremented from
`MarkBufferForProducer` on whatever thread calls a GL entry point, and
`PersistentMapTracker::m_livePersistentMaps` (`PersistentMapTracker.h:106`) is an unsynchronised
`UnorderedMap` mutated from `~BufferObject` (`BufferObject.cpp:58-64`), which is not necessarily the GL
thread. The counters are diagnostics; the map is load-bearing. Table 3 calls these client-role
singletons, so this may be intended — flagging it as a thing v1's thread work must confirm rather than
inherit.

### m-14 — `MOBILEGL_IPC_ADOPT_TIER` refuses late and refuses everything
`AdoptTierIsEmulate()` (`PersistentMapTracker.cpp:124-136`) aborts for any value != 2, so `=7` dies
naming P11 as though it were a real tier, and the abort lands at the first `map_persistent` rather than
at parse — a mis-set run gets through EGL bring-up and a frame of setup before it dies. Contract §5 only
promises 0 and 1 parse-and-Fatal.

---

## QUESTION

### Q-15 — R-6's "always" has a second, ungated door
The decline lives in `MGPipeApplyMapPersistent` (`PipeApply.cpp:1958-1975`), but
`AcquireMemoryRange` falls through to the legacy op when the MGPipe resource subsystem is off
(`BufferObject.cpp:840-847`: `g_bufferBackendOps->AcquirePersistentMap(*this)`), and that arm has no
transport test. In a split build with a backend that registered no MGPipe resource ops, a real pointer
is minted and adopted. Is that configuration in scope for P5? If yes, R-6 needs the gate at the
frontend too; if no, say so in the contract rather than leaving it to E3(d) to notice.

### Q-16 — the seam, stated rather than left open
Row 0/1 walk **bindings** where their backend twins (`DirectGLES.cpp:570/618`,
`UniformManager.cpp:1231`) walk a program-resolved subset — wider, and safe, and b1 argues it well
(§3.3). Row 2 sweeps 192 units against the backend's bitset — wider, safe. Rows 3/5 resolve the XFB
program on the client, which the client genuinely owns. What no row covers is the backend's own
buffers: Espryt's converted-vertex-stream and primitive-restart scratch, Magma's UBO ring. **Those have
no `MarkGpuWritten` today either**, so the client set is no narrower than monolith's — but that is worth
one sentence in the header's inventory, because the next reader will ask the same question and the
answer ("not a regression, still a hole") is not currently written down anywhere.

---

## What I verified, and it held

* **The pad decision's premise is true at the source.** `RespecifyRedefinesNoStorage`
  (`PipeApply.cpp:565-566`) refuses `kMGPipeResourceTargetBuffer` as its **first line**, so
  `MGPResourceDesc` genuinely cannot carry a metadata-only buffer update and the descriptor carrier
  would have cost a re-upload per map. The ruling is sound; the contract row should be corrected as b1
  asks.
* **The pad move is clean.** `MGPipeTypes.h:1098` `Uint8 Pad0[2]`, `MGP_ASSERT_POD(MGPSubData, 72)` at
  `:1104` unmoved. Nothing in the tree zeroes, normalises or assert-zeroes `MGPSubData`'s pads
  (`grep -rn Pad0 MobileGL/MG_Pipe MobileGL/MG_Remote MobileGL/MG_Impl/Pipe scripts/gen_pipe.py`).
* **The bit is registered where it must be.** `F(HasLiveHostWrites)` in `MGP_FIELDS_MGPSubData`
  (`PipeFields.def:176`); `MGPSubData` is in `MGP_VERIFY_PAYLOAD_LIST` (`:352`); `gen_pipe.py`'s cover
  check (`:416-446`, `PADDING_MEMBER_RE = ^Pad\d*$`) would have **failed** `--check` had it been
  omitted, so this is enforced and not merely tidy. The verify comparator expands the macro
  (`gen_pipe.py:876`), so no regeneration is needed and `--check` is honest. (What is *not* honest is
  that no lane ever gives it a non-zero value — M-8.)
  `gen_pipe.py --check` rc=0, `--self-test` rc=0.
* **`PinNoLiveHostWrites` was lifted, not deleted** — `#else` empty body at `PipeApply.cpp:865`;
  the `Managers.cpp` assert keeps the same guard; `SanityTest.cpp`'s original
  `ALiveHostMapKeepsTheHandleArmProbeDirtyBetweenTwoDraws` is untouched and a split twin was added
  beside it. The *scope* of the lift is the problem (M-8), not the mechanic.
* **G1 held, re-run by me at `--threshold 0`:**
  ```
  .text 10806323 -> 10806323 (+0, +0.000%);  file bytes identical
  27811 -> 27811 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
  ```
  It holds for a real reason: `FlushPendingRangesFrom` (the only caller of `InvalidateFlushAccessFor`)
  is inside `#if MOBILEGL_PIPE_PUSH` (`Managers.cpp:1046..`), `m_publishedLiveHostWrites` is behind
  `#if MOBILEGL_BUILD_DISAGGREGATED`, `MG_Config::Transport` is an
  `inline constexpr TransportMode::Monolith` in any non-DISAGG build (`Config.h:512-514`), and
  `ByteClass::PersistentMapPush` already existed **above** the `#if MOBILEGL_PIPE_PUSH` block
  (`PipeStats.h:72` vs `:76`) — so wiring it resized nothing. Confirmed.
* **Test lanes, re-run:** `ctest -L unit` = **1806 / 1806 / 1806** (pull / push / verify) and
  **1863** (split), 100% passed, 0 failed. G2 pull-vs-push name diff = **0 lines**; G14 split
  removed = **0**, added = **57**. Numbers match b1's report exactly.
* **`integration-gpu` under `MOBILEGL_TRANSPORT=monolith`:** build-push **1117/1117**, build-split
  **1117/1117**, and the name set is identical to `~/w7/p5-before-ctest-names.txt` (0 names added,
  0 missing). Necessary but not sufficient — M-5 is a monolith-path behaviour change this suite does
  not observe.
* **`mpr` really is counted before the decline** (`PipeApply.cpp:1944`,
  `++g_applier.MapPersistentRoundtrips;` is the first statement), so E3(c)'s "`mpr` equal between the
  arms" is structurally true and not a coincidence.
* **T0/T1 are a named refusal, not a fallthrough** — `MGLOG_F` + `std::abort()`
  (`PersistentMapTracker.cpp:124-136`), with a death test
  (`SplitBufferSet.OnlyAdoptTierTwoIsImplemented`).
* **The 21-site census is right.** I counted them: Espryt 9 (`DirectGLES.cpp:361, 6185, 6439, 6440,
  6541, 6542`, `MultiDraw.cpp:511`, `Managers.cpp:2841, 3018`) + Magma 12 (`DirectVulkan.cpp:281, 472,
  796`, `UniformManager.cpp:2024`, `VulkanRenderer.cpp:3542, 3621, 4013, 7428, 12423, 12424`,
  `VkBufferManager.cpp:628, 679`) = 21. `MEASUREMENTS.md:111`'s 20 → 21 correction and the naming of
  `DirectGLES.cpp:361` as the missing one are both correct.
* **The membership predicate really is `SyncPersistentMappedRange`'s chain**, line for line and in
  order (`PersistentMapTracker.cpp:40-59` vs `BufferObject.cpp:382-395`), including the
  `FlushExplicit` early-out, and a unit case drives them against each other.
* **Block arithmetic and the falling-edge byte source are sound.** `MappedData()` is the shadow base
  (`BufferObject.cpp:873`, `return m_resource.Bytes();`), so block offsets are buffer-relative and
  correct; the remainder tail is right; and `ReleaseMemory` does land the staged writes and call
  `NotifyFlushMappedRange` **before** `NotePersistentMapStateChanged` (`BufferObject.cpp:290-331`), so
  the falling-edge record's bytes are current when `landStagedWrites` is true.
* **`MarkEndTransformFeedbackCaptureTargets()` is in the only correct place** — before
  `GLContext::EndTransformFeedback()` clears the bindings (`GL_Drawing.cpp:1369-1372`) — and the
  fence removal's one in-function consumer (`FixupGsStripCaptureOrder`, which reads back through
  `MappedData()`) was spotted and given a `SyncGpuWrites()`. That the `SyncGpuWrites()` currently
  waits for nothing is M-7, and that it is ungated is M-5; the *diagnosis* was right and nobody else
  had named it.
* **The whole module is otherwise correctly gated.** Every other new site pairs
  `#if MOBILEGL_BUILD_DISAGGREGATED` with `Transport != Monolith`, and the two entry points that are
  not textually gated (`~BufferObject`'s `Forget`, `NotePersistentMapStateChanged`) check
  `PushIsArmed()` or are provably inert (the set is empty and `m_publishedLiveHostWrites` is never
  written) when the transport is monolith.
