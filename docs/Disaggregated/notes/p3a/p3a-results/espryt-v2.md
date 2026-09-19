# P3a package C — Espryt rework (`espryt-v2`)

Branch `p3a/espryt`, worktree `/home/swung/w7/p3a-espryt`, **rebased onto `refs/heads/feat/disaggregated` (`5cb826b0`)**
and then ten new commits. **Not pushed.** Every file touched is in C.2's list
(`MG_Backend/DirectGLES/{SlotTables.h, Managers.h, Managers.cpp, DirectGLES.cpp}`, `MG_Test/SanityTest.cpp`);
`MultiDraw.cpp` and `Utils.cpp` are still untouched, and nothing outside the list was edited.

---

## 1. The rebase (ID-9), and where `d7655247`'s fix sits in each arm

`git rebase refs/heads/feat/disaggregated` applied **cleanly, with no conflict** — the contract commit `39722687`
dropped out as a duplicate of `e01c0ccc`, and the six package commits replayed with new hashes:

| # | before | after | message (unchanged) |
|---|---|---|---|
| e1 | `9ae4ec09` | `4db6bf1e` | give the buffer resource its own {slot, gen} table … |
| e2 | `de13c588` | `4d157a44` | take the buffer ops by handle and payload … |
| e3 | `855272e5` | `b4687922` | answer a readback through the reverse channel … |
| e4 | `469f78b8` | `6926efb1` | drive the driver VAO from the pushed vertex-elements record … |
| e5 | `a9a1d0eb` | `568005fb` | key the narrowed fp64 vertex stream on the buffer's handle … |
| e6 | `203120fa` | `cdbd5535` | extend the slot-table suite to the buffer kind … |

**A clean rebase was the trap, not the relief.** `d7655247` adds `++g_bufferBackendIdGeneration` inside
`Ops_Respecify`'s immediate immutable-store retire; e2 had *copied* that retire path into `Ops_H_Respecify` before the
merge existed, so three-way merge put the new line into the original body and left the copy alone. There was no
textual conflict and no diagnostic — the fix was simply absent from the arm the default mask runs.

After the rework the file has **six** `++g_bufferBackendIdGeneration` sites, three per arm, and the two arms are
symmetric:

| what re-mints the driver id | legacy arm | handle arm |
|---|---|---|
| persistent-map adoption | `Managers.cpp:1484` (`Ops_AcquirePersistentMap`) | `:2134` (`Ops_H_MapPersistent`) |
| **respecify's immediate immutable retire — `d7655247`** | **`:1548`** (`Ops_Respecify`) | **`:1881`** (`Ops_H_Respecify`, added by `8589bb5a`) |
| the ensure path's deferred immutable retire | `:2798` (`EnsureBufferResource`) | `:2659` (`EnsureBufferResourceForHandle`) |

`LargeArenaAdoptionScenario.RespecifiedVertexArenaKeepsVaoBindings` and the two other cases `d7655247` added are the
gate; they are among the 463 the two legacy lanes run green, and among the 204 the default mask still fails on the
stub applier (§5.4).

`build-linux` / `build-push` were reconfigured against `5cb826b0` before anything was judged, and `~/w7/p3a-before-*`
is ID-9's re-captured baseline (2502 ctest names, `p3a-before-head.txt` = `5cb826b0`).

---

## 2. The rework, finding by finding

Ten commits, oldest first. Every message is a single line, no body, no attribution.

| # | hash | finding | message |
|---|---|---|---|
| r1 | `8589bb5a` | ID-9 | `[Fix] (Espryt): carry the adopted-store respecify's buffer-id generation bump into the handle arm as well` |
| r2 | `5f3f82c1` | ID-11 | `[Fix] (Espryt): leave FlushPendingRangesNow as this file's only definition and call the one shared range ladder from both arms` |
| r3 | `7c8e2ba3` | m2, m3, m5, m6, m8 | `[Fix] (Espryt): clamp the handle readback to the backend store past its offset, memoise the buffer handle lookup, and write down the two contracts the ops owed their callers` |
| r4 | `7b950575` | **C-1** | `[Fix] (Espryt): keep the live-map question in the handle arm's draw-clean probe instead of a record field P3a pins false` |
| r5 | `b3bd5d27` | **C-2** | `[Fix] (Espryt): re-read the client's shadow base at every use and forget it at the two events that free the allocation it names` |
| r6 | `329cbae5` | **M-1** | `[Fix] (Espryt): refuse a backwards generation and an out-of-range slot in the handle-keyed GetOrCreate instead of adopting them` |
| r7 | `88ff762b` | **M-2** | `[Fix] (Espryt): walk all 32 attribute slots so a vertex-elements record that shrinks still disables the arrays it dropped` |
| r8 | `ea5a6089` | **M-3, M-4** | `[Fix] (Espryt): make the vertex-input bit require the resource bit and give both new subsystems a named verdict when the knobs leave no arm` |
| r9 | `cbd60fe9` | **M-5** | `[Fix] (Espryt): treat an unbound vertex-elements record as a memo miss and record the applier-reset serial rule the twin's gate depends on` |
| r10 | `6a1a9bc7` | C-1 / M-1 gates | `[Fix, Test] (Espryt): pin that a live host map keeps the handle arm's draw probe dirty and that a backwards generation is refused` |

### C-1 — the live-map question is back, and it is asked of the only thing that can answer it

`Managers.cpp:2556` (the signature), `:2577` (the test), `:2588` / `DirectGLES.cpp:631` / `:810` (the three call
sites), `Managers.h:748` (the declaration and its contract).

`IsBufferDrawCleanByHandle` now takes `const BufferObject* frontend` and asks `frontend->IsMapped()` **at exactly the
point in the order the legacy probe asks it** — after the adopted-store early-out, beside the `HasLiveHostWrites`
test, which is kept together with its `MOBILEGL_PIPE_VERIFY` assertion. The review's reasoning is reproduced in the
function: `HasLiveHostWrites` is pinned false and written by nobody in P3a, so it *cannot* stand in for the frontend's
map state; an emulated (non-adopted) persistent map mutates the client's shadow with no call, no serial and no epoch,
which is the whole reason a map question exists in this probe at all. The comment says the frontend read retires the
moment P5 gives the field a producer, and that it is the last frontend read in the function.

The three call sites all already held the object: the dispatcher has `frontend`; the VBO memo entry has
`entry.frontend` (the same pointer the legacy arm probes, kept alive by the VAO attribute's `SharedPtr` for as long as
the memo is valid); the IBO half is handed `possibleIBO.get()`, the live bound object, which is what the legacy
comment there says the probe re-validates against. A null `frontend` means "no live map", stated at the declaration.

**The test that would have caught it** — `DirectGLESBufferDrawProbe.ALiveHostMapKeepsTheHandleArmProbeDirtyBetweenTwoDraws`
(`SanityTest.cpp:4180`, push-only, skipping visibly in a pull build at `:4375`). It builds an applier record and a twin
that answer **clean to every one of the probe's other questions** (asserted first, so the case cannot pass vacuously),
takes a `GL_MAP_PERSISTENT_BIT | GL_MAP_WRITE_BIT` map that nothing adopts, writes one byte through the pointer,
asserts the record's `Serial` did **not** move (the point of the whole finding), and requires the probe to answer
dirty; then unmaps and requires it to answer clean again, so the case is about the map and not about the fixture.
**Negative control run:** with the `IsMapped()` test disabled again the case fails at `SanityTest.cpp:4238`
("the handle arm called a persistently mapped, non-adopted buffer draw-CLEAN…").

A unit-level case is what C.2's file list allows — the itest the review asks for lives in `MG_IntegrationTest`, which
is package D's — so §6 carries it forward as an integrator item.

### C-2 — no cached host pointer survives the two events that free it

`Managers.h:656` (the member's lifetime rule), `Managers.cpp:2637` (`liveHostBase`), `:2676` / `:2689` (the pool
reseed), `:2726` (the ensure path's base), `:1865` (the orphan clear), `:2164` (the adoption clear), `:4419` (the fp64
source).

Four changes, and the first is the one that matters:

1. **`EnsureBufferResourceForHandle` re-reads `bufferObject->MappedData()` at every use**, through a `liveHostBase()`
   lambda declared at the top of the function with the reason beside it — exactly what the legacy arm did, and
   available because D-N/D6 keep the frontend object on this path for `SyncPersistentMappedRange` anyway. The pool
   reseed, the respecify's `initialData` and the pending-range flush all read it; none of them touches the cached
   member any more.
2. **The pool decision is back to the legacy one** — `(poolSize > 0 && !resource->persistentMapped)`. The undeclared
   `&& resource->hostBytes != nullptr` term is gone, with a comment saying why it was wrong: it changed the *pool*
   behaviour D-F freezes rather than "the source of bytes" the conversion may move, and it moves `AcquireFromPool`
   call counts that `StreamedArenaScenario`'s two recycle cases observe.
3. **`hostBytes` is cleared at the two events that invalidate it** — a `HasDefinedContent == 0` respecify
   (`Ops_H_Respecify`, which is also the call that resizes the client's shadow and brings no replacement base) and a
   successful `Ops_H_MapPersistent` (the client adopts the coherent pointer and does `clear() + shrink_to_fit()` on
   the shadow the instant we return). The member's comment now states the rule as three bullets and says that every
   path holding the frontend object must re-read instead of reading it.
4. **`SyncFloat64AttributeAsFloat32ByHandle` reads `persistentPtr` for an adopted source.** This was the reachable
   use-after-free: the memo is deliberately never trusted for a persistently mapped source, so for an adopted buffer
   the narrowing re-read the freed shadow **on every draw**. With (3) in place the alternative branch is a null and a
   refusal rather than a stale read.

**Declared deviation (new, D11):** `Ops_H_MapPersistent` gains one statement that `Ops_AcquirePersistentMap` does not
have (`resource->hostBytes = nullptr`). It is about a push-only member the legacy arm does not own, not about the map,
and it is the statement that makes (4) read the right memory. D-E's "two permitted changes" is therefore three, and
this is the third; the legacy `Ops_AcquirePersistentMap` is still byte-untouched.

### M-1 — `GetOrCreate(MGPipeHandle)` refuses what `FindByHandle` refuses

`SlotTables.h:148` (`kMaxHandleSlot`), `:283-296` (the bound), `:301-315` (the direction test), `:331` (`LiveGenAt`),
`:502` (`EntryAt`'s contract), `Managers.cpp:2359-2372` (the release-build diagnostic).

The `!=` becomes a **direction** test: forward is a recycle and resets the twin, backward returns `m_nullTwin`. The
comment says which arm covers which direction and why the minting overload's symmetric form is provably safe where
this one's is not. `handle.Slot` is bounded by `kMaxHandleSlot` (2^20, documented as a sanity cap rather than a
policy — the allocator is the client's and there is no constant to check against on this side) **before** `EntryAt`
resizes on it, and `EntryAt` now carries the sentence that every caller bounds its slot first.

`MOBILEGL_ASSERT` is inert at INFO, so `GetOrCreateBufferResourceForHandle` asks the same two questions first and
answers them with `MGLOG_E_ONCE` — a refused resource is loud in the builds that ship. `LiveGenAt(slot)` exists for
that and for nothing else.

**Gate:** `DirectGLESSlotTable.AGenerationBehindTheLiveTwinIsRefusedRatherThanAdopted` (`SanityTest.cpp:4258`) stamps a
live twin at `{7, 2}`, calls `GetOrCreate({7, 1})` and asserts the incumbent's twin survived **with its marker**, then
checks that `{7, 3}` still resets it (the direction that was always right) and that a slot at `kMaxHandleSlot` is
refused rather than resized to. **Negative control run:** with the direction test disabled the case fails at
`SanityTest.cpp:4277`.

Note for the integrator: `wire-review-v1` M-D reports the identical unbounded-resize shape in the applier's
`RecordAt`. This package bounds the **backend** side; the applier side is still A's.

### M-2 — the attribute walk covers all 32 slots

`Managers.cpp:4008-4021`. The loop bound is `MG_Pipe::kMGPipeMaxVertexAttribs`, not `rec->AttributeCount`, so the
disable arm is restored for an attribute the configuration has dropped. The comment records why it costs nothing:
`MGPipeApplyCreateVertexElements` zeroes both arrays before it unpacks, so an entry past `AttributeCount` reads
`Enabled = 0`, `Type = 0` (`DataType::Int8`, **not** `Float64`, which is 8) and `IsLong = 0` — i.e. exactly "disable
this array", with no requirement placed on B. `VertexArrayObject::MAX_VERTEX_ATTRIBS` is also 32, so the legacy walk
issued the same `glDisableVertexAttribArray` calls and no new GL traffic appears on either arm.

### M-3 — bit 8 requires bit 7, and the header says so

`Managers.cpp:2316-2331`, `Managers.h:700-711`.

`ResolveVertexInputSubsystemArm()` refuses `bit 8 set, bit 7 clear` with an `MGLOG_E` that names both bits and the
reason (the vertex-input handle arm resolves every attribute's driver buffer id out of the resource slot table, and
only bit 7 puts twins there), and runs the legacy vertex-input arm instead. The header's claim that "the A/B has to be
able to run either one alone" is corrected in place: `0x0ff` (bit 7 on, bit 8 off) **is** a supported A/B, because the
legacy VAO walk reaches the handle arm through `EnsureBufferResource`'s own dispatch; `0x17f` is not, and is now
refused loudly rather than half-run with every attribute binding through whatever pointer the driver VAO last held.

### M-4 — both new subsystems answer `PipeLegacyMemos`

`Managers.cpp:2283-2290` (`PipeSubsystemArmVerdict` + `ClassifyPipeSubsystemArm`), `:2292-2310` and `:2312-2354` (the
two resolvers).

The three-way shape `ClassifyEsprytSlotArm` established for bit 5, with the third verdict's **reachability** as a
parameter, because the two families genuinely differ:

* **bit 7** — the buffer family's legacy arm is the `Ops_*` table and `g_glesBufferBackendOps`, compiled
  *unconditionally* (only the VAO twin memos and the pre-handle VAO body sit under `MOBILEGL_PIPE_LEGACY_MEMOS`), so
  `NoArm` is unreachable for it. The knob is still answered: bit clear **and** `PipeLegacyMemos` false logs an
  `MGLOG_W` saying which arm actually ran.
* **bit 8** — the legacy arm *is* conditional, so `NoArm` is real and is named with `MGLOG_E` at arm resolution, not
  only at the first draw (the pre-existing `SyncToBackend` guard still says it again there).

**It diagnoses and does not stop, deliberately, and this is a decision the integrator should confirm.**
`ResolveEsprytSlotTablesArm`'s `Fatal{}` is safe because no shipped lane pins bit 5 clear. Bits 7 and 8 are clear in
**every `.Handles` itest lane today** (`MG_IntegrationTest/CMakeLists.txt` pins `MOBILEGL_PIPE_PUSH=0x7f` beside
`MOBILEGL_PIPE_LEGACY_MEMOS=0`), which is contract-review item 11 and package D's to re-pin: a stop here would turn
that lane red for a mis-pinned control rather than for a defect. The reason is written at the classifier, and it says
the stop can be promoted the moment those lanes carry an explicit `0x1ff` arm.

### M-5 — the reset dependency is declared, and an unbound record is a memo miss

`Managers.cpp:3974-3990` (the twin's gate), `DirectGLES.cpp:613-622` and `:677-679` (the memo key).

The gate carries a paragraph naming `MGPipeApplierReset()`, the fact that it runs on **every** change of the current
`GLContext` (`Tracker.h`'s `if (m_context != &ctx) Reset();`), that the twin survives the excursion because
`BackendVertexArrayObject` has no context-generation member, and that this design is correct **only if the reset
ADVANCES `VertexBuffersSerial` / `IndexBufferSerial` instead of zeroing them** — wire's C2. It also records the second
half the review derived: that the `m_syncedVertexBuffersSerial` term is what forces the whole AND dirty after a reset,
which is what rescues the per-record `ContentSerial` half. `wire-v2` reports C2 fixed (`PipeApply.cpp:628-646`), so
the precondition is expected to hold at merge; §6 item 12 keeps it as a hard gate anyway.

Separately, `SyncVaoAttributeBuffersByHandle` now treats "no live vertex-elements record" as a **miss**: the key
`{null, 0, buffersSerial}` describes no configuration and never changes while the state persists, so a memo stamped
with it would hit on every later draw of a VAO whose attributes have moved. The walk still runs and the buffers are
still ensured; only `memo->valid` is withheld.

### The minors

| # | disposition |
|---|---|
| m1 | **Report fix.** §3.1 of `espryt-v1` rendered `GetBackendResource()/SetBackendResource()` → "the slot table's `GetOrCreate(res)`". The substituted call is `GetOrCreateBufferResourceForHandle` (`Managers.cpp:2357`), which additionally seeds `pendingRespecify = true` on a freshly minted twin where `Ops_AcquirePersistentMap` does not. Outcome identical (a twin with no `storageInitialized` respecifies either way); the claim is corrected here rather than in the code. |
| m2 | **Fixed** (`Managers.cpp:2036-2043`). `Ops_H_Readback` maps at `record.Offset`, so the size is clamped against `storageSize - Offset` and a `readOffset >= storageSize` returns; the applier's `BufferRangeFault` checks the range against `Desc.Width`, which a not-yet-applied respecify can leave larger than the store. |
| m3 | **Fixed** (`Managers.cpp:1997`). The `ZoneScopedC` the legacy map arm carries is back in `Ops_H_FlushRange`'s kill-switch map arm. |
| m4 | **Declared, kept.** `FlushPendingRangesFrom`'s `if (hostBase == nullptr) return;` after the queue is drained is a text change the legacy arm also runs inside a push build. It is unreachable there (`MappedData()` is non-null whenever ranges are queued) and it is the honest answer on the handle arm (the queue is dropped, and the next full re-upload puts the store right, which the comment says in place). It is the one place D1's "one body, so the arms cannot drift" is not literally true, and it is recorded here rather than removed. |
| m5 | **Fixed** (comment, `Managers.cpp:4123-4131`). The VAO index bind resolves without ensuring; the comment says why that is noisy rather than wrong (nothing is stamped on the miss, so the next indexed draw repairs it) and why ensuring here would need the object this arm does not hold. |
| m6 | **Fixed** (`Managers.cpp:2400`). `HandleOfBuffer` goes through `BackendSlotTable::HandleOf`, which carries the table's single-entry front memo and populates it — previously only the minting overload wrote it, so nothing absorbed the per-draw `FindByLifetimeId` hash lookups. Same answer, same allocator probe on a miss. D.4 should still record the cost (§6). |
| m7 | **Report fix.** D6's "no `MG_State` type in any new handle-arm signature" holds for the nine `MGPipeResourceOps` bodies, `SyncToBackendFromApplier`, `SyncFloat64AttributeAsFloat32ByHandle`, `BindAttributeBufferByHandle`, `SyncZeroStrideAttributeByHandle`, `ResourceWidthForHandle`, `ResourceSerialForHandle`, `GetOrCreateBufferResourceForHandle` and `FindBufferResourceForHandle`. It does **not** hold for `EnsureBufferResourceForHandle` (D-N's `SyncPersistentMappedRange`), `MarkBufferGpuWritten`, or — now — `IsBufferDrawCleanByHandle` (C-1). All three are named at their declarations; G13 greps `PipeApply.h`, which is unaffected. |
| m8 | **Fixed** (comment, `Managers.cpp:2431-2437`) **and stated for B**: `BufferObject::MarkGpuWritten` sets **both** `m_hasDefinedContent` and `m_gpuWritePending`. The client's `OnGpuWritten` must set both, or the next `ResourceRespecify` carries `HasDefinedContent = 0`, `Ops_H_Respecify` orphans instead of uploading, and the shader-written contents are dropped. |
| m9 | **Fixed by running the real thing.** §5.5: a full `MOBILEGL_PIPE_LEGACY_MEMOS=OFF` + `MOBILEGL_PIPE_PUSH=ON` configure and build of the library, the test targets and the integration test, not `-fsyntax-only`. |

### ID-11 — `FlushPendingRangesNow` is the file's only definition again, and it is byte-identical

`Managers.cpp:1127-1139` (where the forwarder was, and why it is not there), `:1316` (the one definition),
`:1718-1723` and `:2869-2871` (the two legacy call sites).

The v1 tree had **two** definitions of the name — a two-line forwarder in the push arm and the untouched body in the
`#else` — and package D's `scripts/p3a_untouched_regions.sh` exits **2 ("could not run")** on exactly that, because it
requires each of the ten names to be defined exactly once. The forwarder is gone. The push arm's two legacy call
sites (`Ops_ReadbackFromGpu` and `EnsureBufferResource`) call `FlushPendingRangesFrom(*resource,
bufferObject.MappedData(), bufferObject.GetSize())` under `#if MOBILEGL_PIPE_PUSH`, with the untouched
`FlushPendingRangesNow(*resource, bufferObject)` in the `#else`.

**What this delivers versus what ID-11 asked for, stated plainly.** ID-11 says "restore it byte-identical and make the
handle arm CALL it". The first half is met exactly (the gate now runs and reports the sha equal — §5.3). The second
half cannot be met literally: `FlushPendingRangesNow`'s signature is `(GLESBufferResource&, BufferObject&)`, and a
byte-identical body must keep it; having no frontend object to offer is the whole point of the conversion, and
`Ops_H_Readback` has none. What is delivered instead is the property the gate's own comment says the row exists for,
in its strongest available form: **a push build contains exactly ONE three-tier ladder, `FlushPendingRangesFrom`, and
BOTH arms call it**; a pull build contains exactly one, `FlushPendingRangesNow`, byte-identical to `5cb826b0`'s. There
is no build in which two ladders are compiled, so there is nothing for a tier to drift into. The alternative reading —
keep the byte-identical function compiled in both builds and let the handle arm carry `FlushPendingRangesFrom` beside
it — would pass the same gate while putting two ladders in every push build, which is what the row forbids.

---

## 3. Track H census and re-keyed memos

Unchanged from `espryt-v1` §2, with two additions and one correction:

* `GLESBufferResource::hostBytes` keeps its place in the census as a push-only member, but its **lifetime rule** is now
  part of the contract (`Managers.h:656`): refreshed by a content-carrying call, cleared by an orphaning respecify and
  by a successful persistent-map adoption, and never read by a path that holds the frontend object.
* `BackendSlotTable::kMaxHandleSlot` and `LiveGenAt` are new, push-only, and add no member to any object.
* `IsBufferDrawCleanByHandle` reads **five** things again, not four: the four applier reads plus the frontend's
  `IsMapped()`, which retires at P5. The v1 report's "five reads become four applier reads with identical semantics"
  was the inverse of what the code did and is corrected here.

---

## 4. Deviations

D1-D10 stand as `espryt-v1` §5 records them, with the judgements `espryt-review-v1` gave them, plus:

* **D7 is narrower than it was.** The declared `SyncGpuWrites` gap is unchanged and is still P8's. The *second* defect
  on that function — the freed shadow — is fixed (C-2), so `DoublePrecisionScenario` with an adopted source now
  exercises correct memory rather than a use-after-free, and the remaining difference is only the declared one.
* **D11 (new)** — `Ops_H_MapPersistent` clears `hostBytes` on success. See C-2 above.
* **D12 (new)** — `MOBILEGL_PIPE_PUSH` builds contain no `FlushPendingRangesNow`. See ID-11 above.
* **D13 (new)** — bit 8 without bit 7 is refused rather than supported. See M-3 above.
* **D14 (new)** — the armless verdict for bits 7 and 8 diagnoses and does **not** stop. See M-4 above; this one wants
  an explicit integrator decision, because the alternative depends on package D re-pinning the `.Handles` lanes.

---

## 5. Verification transcript

All in `/home/swung/w7/p3a-espryt` at `6a1a9bc7`, `CCACHE_BASEDIR=/home/swung/w7`. Every build directory was
rebuilt before anything was judged.

### 5.1 Builds and unit

```
cmake --build build-push  --parallel 28      rc 0
cmake --build build-linux --parallel 28      rc 0

ctest --test-dir build-push  -L unit --no-tests=error
    100% tests passed, 0 tests failed out of 1584
ctest --test-dir build-linux -L unit --no-tests=error
    100% tests passed, 0 tests failed out of 1584
```

`ctest -N`: **2504** in `build-push`, **2504** in `build-linux`, `diff` empty. Against
`~/w7/p3a-before-ctest-names.txt` (2502): **0 removed, 2 added**, and both are this rework's gates —
`DirectGLESBufferDrawProbe.ALiveHostMapKeepsTheHandleArmProbeDirtyBetweenTwoDraws` and
`DirectGLESSlotTable.AGenerationBehindTheLiveTwinIsRefusedRatherThanAdopted`. Both **run** in `build-push` and both
**skip visibly** in `build-linux` (G2/G14).

### 5.2 G1 — the pull build is still inert

```
python3 scripts/symbol_report.py --before ~/w7/p3a-before-libMobileGL.so \
    --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0
    rc 0
    Removed (0) _none_   Added (0) _none_   Resized (0) _none_   Renamed only (0) _none_
```

`~/w7/p3a-before-libMobileGL.so` is ID-9's re-captured baseline, i.e. the **pull build of `5cb826b0`**.

### 5.3 G5 — the ten, by the real script

`scripts/p3a_untouched_regions.sh` is package D's and is not in this tree; a copy of `p3a-gates`' current version
(`/home/swung/w7/p3a-gates/scripts/p3a_untouched_regions.sh`, which already carries ID-11's tenth row) was placed in
the tree for the run and **removed again** — the working tree is clean and nothing of D's was committed.

```
bash scripts/p3a_untouched_regions.sh --self-test
    positive control: 10 bodies extracted from the working tree
    positive control: an untouched copy compares equal
    positive control: an edit outside the ten bodies is invisible
    negative control: a perturbed ClearBufferPool body is reported, and named
    negative control: a perturbed FlushPendingRangesNow body is reported, and named
    self-test passed                                             rc 0

bash scripts/p3a_untouched_regions.sh 5cb826b0 HEAD              rc 0
    the 10 pool / deferred-release / ring / flush-drain functions are
    byte-identical between 5cb826b0 and HEAD
```

**Verdict: all TEN, including `FlushPendingRangesNow`.** Their shas at both refs:

```
ba79b92dc6ebf2cb2af6aa79f23ba1941818d131812965e9f96a89caffc8e90a  IsPoolable
6563286e007650a5abb0a1bbdd7acddd901776f3e55d3c0087c8fee60aa6cc4c  EnrollIntoPool
dc9d3066b2429208e18adc27c639d16260e74b7f17f7c3f290d54f58534f089b  AcquireFromPool
12656cfd716dec50a4a4893459a7a6aaa0a635f79dda8fa51b88a819da27e5e0  TrimBufferPool
c44a12746bcd82e55d8a635adf766e1fe2cf961ded5d9d745896fd65535bde16  ClearBufferPool
15259e547fb514dcdcf087bbde1d048fc2d9e7901ea6c6c94a1109fb7987073a  ProcessDeferredBufferReleases
2daaa5254c38db63742ef8d6dacd25f8868c7fcd3d9745956862134cf76911d5  CreateRingStorage
6221df2e4c0b185fbac061e956253dacd8f8d886bb3f3341cd76a26b5e1f84b1  RingAvailable
709b6f4190c735517034b9c5d62967a9ae61f2198d2eb441dfe4e7af437cb784  RingAllocate
c65570022a5856c1afccfd0cb42fefac707689f5bb3154c3a8e17d2495ccdf8e  FlushPendingRangesNow
```

At `203120fa` (before this rework) the same invocation **could not run**: two definitions of
`FlushPendingRangesNow` is the script's exit 2.

### 5.4 The three integration lanes

```
MOBILEGL_PIPE_PUSH=0    ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES
    rc 0   100% tests passed, 0 tests failed out of 463
MOBILEGL_PIPE_PUSH=0x7f ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES
    rc 0   100% tests passed, 0 tests failed out of 463
                        ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES   (0x1ff)
    rc 8   56% tests passed, 204 tests failed out of 463
```

463, not 461: `d7655247` added three `LargeArenaAdoptionScenario` cases and the rebase brought them in.

**Classification of the 204, and it is stronger than a classification.** The lane was run **twice against freshly
built binaries**: once at `cdbd5535` (this package before the rework) and once at `6a1a9bc7`. Both fail **204 of 463**,
and the failing **name sets are identical** — `comm` reports nothing on either side:

```
--- failures ONLY at HEAD (new, would be mine)      : (empty)
--- failures ONLY before the rework (fixed by it)   : (empty)
```

So no failure in that lane is attributable to this rework. They remain the stub applier's, as `espryt-v1` §4.4
established and as the review confirmed: with nothing emitting `resource_create` every buffer resolves to the null
handle, and with nothing emitting `create_vertex_elements`/`bind_vertex_elements` there is no bound record. Running one
of them directly under `MOBILEGL_LOG_FILE_PATH` confirms the two, and only the two, MGPipe diagnostics:

```
MOBILEGL_LOG_FILE_PATH=… MOBILEGL_PIPE_PUSH=0x1ff MobileGLIntegrationTest \
    --gtest_filter='*OrientationScenario.OffscreenPassRendersUpright*'          rc 1

[ERROR]: MGPipe: buffer 1 has no resource handle - the resource family is switched over but
         nothing emitted resource_create for it
[ERROR]: MGPipe: no vertex-elements record is bound, so the driver VAO cannot be configured -
         nothing emitted create_vertex_elements/bind_vertex_elements
```

No other `MGPipe` error line appears in that log. (The two `NormalizePixelFormat: unhandled internalFormat:
GL_STENCIL_INDEX8` errors in the same log are pre-existing and unrelated to the pipe.)

Failures by scenario, whole list this time rather than a head:
`ImageTargetKind` 24, `VertexAttribBinding` 13, `CrossFrameBuffer` 13, `Orientation` 11, `MultiDraw` 9, `GuiBatch` 9,
`DrawParameters` 8, `DoublePrecision` 8, `SsboDeclarationForm` 7, `PrimitiveRestart` 7, `TessellationXfbCapture` 6,
`XfbRepeatedCapture` 5, `XfbAfterClipDistance` 5, `ResidentIndex` 5, `ProgramPipeline` 5, `PointSizeDemotion` 5+3,
`XfbCaptureBufferReuse` 4, `UnwrittenPositionOutput` 4, `UnboundImageDescriptor` 4, `LargeArenaAdoption` 4,
`VertexArrayEnableDisable` 3, `UniformInitializer` 3, `SsboArrayLength` 3, `ClearThenReadPixels` 3, `AtomicCounter` 3,
then 2s and 1s — "every scenario that draws or touches a buffer", unchanged in shape from v1.

### 5.5 `MOBILEGL_PIPE_LEGACY_MEMOS=0`, really built (m9)

A dedicated `build-nolegacy` was configured with `-DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF` and the
rest of `build-push`'s options, and **built to completion** — the shared library, `MG_Test`'s targets and
`MobileGLIntegrationTest`, so `MultiDraw.cpp`, `Utils.cpp` and `SanityTest.cpp` are compiled **and linked**, which
`-fsyntax-only` never did:

```
cmake -S . -B build-nolegacy -G Ninja -DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF …
    configure rc=0
cmake --build build-nolegacy --parallel 28
    build rc=0
MOBILEGL_PIPE_LEGACY_MEMOS:BOOL=OFF   MOBILEGL_PIPE_PUSH:BOOL=ON        (from CMakeCache.txt)
    build-nolegacy/libMobileGL.so
    build-nolegacy/MobileGL/MG_Test/SanityTest
    build-nolegacy/MobileGL/MG_IntegrationTest/MobileGLIntegrationTest
ctest --test-dir build-nolegacy -L unit --no-tests=error
    100% tests passed, 0 tests failed out of 1584
```

That path also exercises `ResolveVertexInputSubsystemArm`'s `kLegacyVaoArmCompiled == false` arm (M-4). The directory
is deleted afterwards; it is not one of the three gate builds. Note that `MOBILEGL_BUILD_BENCHMARK` must be forced
`OFF` for a fresh configure in a worktree — the vendored google-benchmark does not build with this tree's
`-Werror` set, which is unrelated to the knob under test.

### 5.6 Falsifiability of the two new cases

Both were run with their fix reverted in the working tree, rebuilt, and observed to fail — then restored, rebuilt and
observed to pass:

```
C-1  disable the IsMapped() test        -> FAILED at SanityTest.cpp:4238
M-1  disable the Gen direction test     -> FAILED at SanityTest.cpp:4277
restore both                            -> [  PASSED  ] 2 tests
```

### 5.7 Purity and shape, re-checked after the rebase

```
grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'      -> empty
grep -n  'MG_State' MobileGL/MG_Pipe/PipeApply.h               -> no hit
git status --porcelain                                          -> clean
git diff --stat 5cb826b0 HEAD                                   -> SlotTables.h, Managers.h, Managers.cpp,
                                                                   DirectGLES.cpp, SanityTest.cpp only
```

`g_pendingFetchBaseInstance` / `ScopedFetchBaseInstance` stay under `MOBILEGL_PIPE_LEGACY_MEMOS` as ID-10 directs, and
the handle arm reads `MGPipeApplierState::VertexFetchBaseInstance`; nothing here changed that.

---

## 6. The exact post-rebase verification list for the integrator

This package has been rebased onto `feat/disaggregated` at `5cb826b0` — i.e. onto **contract only**. `wire` and
`client` are not underneath it yet. After `git rebase refs/heads/feat/disaggregated` once those are merged, in this
order:

1. **Rebuild `build-linux`, `build-push` and `build-verify` before judging anything.** A verdict from a stale
   `build-push` has already cost this package a round.
2. `python3 scripts/symbol_report.py --before ~/w7/p3a-before-libMobileGL.so --after build-linux/libMobileGL.so
   --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` → `0/0/0/0`.
3. `ctest -L unit` in all three build directories; `ctest -N` name sets equal between `build-linux` and `build-push`,
   nothing removed against `~/w7/p3a-before-ctest-names.txt`, and **exactly the two additions named in §5.1**.
4. **`bash scripts/p3a_untouched_regions.sh 5cb826b0 HEAD` and `--self-test`** — the real script, now that D has
   landed it. It must report **ten** functions, not nine. An exit **2** means a second definition of one of the ten
   has reappeared; that is a different failure from an exit 1 and must not be read as a pass.
5. **The default-mask lane is the first execution of most of this code.** `ctest --test-dir build-push -L
   integration-gpu --no-tests=error -j 8 -R DirectGLES` with no env pin must be green. Treat a green there as
   bring-up, not as verification. The A/B in §5.4 says only that this rework introduces no failure the stub applier
   did not already produce.
6. **Both legacy arms again**: `MOBILEGL_PIPE_PUSH=0` and `=0x7f`, same lane, both green.
7. **The named case for C-1, in an integration lane.** `SanityTest`'s new case pins the probe's semantics but issues no
   GL. The itest the review asks for — map a sub-adoption-threshold buffer `PERSISTENT|WRITE`, write through the
   pointer with **no GL call at all**, draw, read the pixels back — is package D's to add
   (`MG_IntegrationTest` is not in C.2's list). Until it exists, `MOBILEGL_COHERENT_AS_FLUSH=1` over the two
   `coherent_as_flush: true` Create fixtures (G3b) is the nearest proxy and must be run under push **and** pull with a
   byte-for-byte path comparison, not only an SSIM pass.
8. **The named cases for C-2**: `DoublePrecisionScenario` with an **adopted** source buffer (≥ 16 MiB, adoption
   enabled), and `StorageBufferRegrowScenario` under ASan or with pool tracing, since the reseed is the second
   dereference. A clean run of today's `DoublePrecisionScenario` does not discharge this.
9. **`VertexArrayEnableDisableScenario` under the default mask, with a configuration that actually shrinks the enabled
   set** — the only gate that can see M-2.
10. **Three subsystem combinations, and the third now has a defined answer**: `0x1ff`, `0x7f`, and `0x17f`. `0x17f`
    must **refuse** — an `MGLOG_E` naming bits 7 and 8 — and run the legacy vertex-input arm. G12's scenario should
    assert the refusal rather than a behaviour. `0x0ff` (bit 7 on, bit 8 off) is the supported one-family A/B.
11. **`MOBILEGL_PIPE_LEGACY_MEMOS=0` with `MOBILEGL_PIPE_PUSH=0x7f`** — the `.Handles` itest pin — now produces a named
    verdict rather than a silent legacy run (M-4). **Integrator decision wanted (D14):** it diagnoses and does not
    stop, because a stop would turn every `.Handles` lane red before package D re-pins them (contract-review item 11).
    If D lands the `0x1ff` arm and re-pins those lanes, promote it to the `Fatal{}` that bit 5 raises.
12. **Confirm wire's C2 is in the merged tree** — the applier reset must **advance** `VertexBuffersSerial` and
    `IndexBufferSerial`, not zero them. `Managers.cpp:3974` says so at the gate. `wire-v2` records the fix at
    `PipeApply.cpp:628-646` (`++` at every reset, with mutation M15 going red when it is reverted), so this
    precondition is expected to hold — but it is a **hard merge gate**: if a later rebase or a revert puts the `= 0`
    back, the twin's attribute gate can read clean over state the applier has just cleared, and this package must not
    be merged. (`wire`'s C1 — the records now survive a make-current — only narrows how often the state this gate
    guards against arises; the serial rule is what the gate depends on.)
13. `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1 ctest --test-dir build-push -L integration-gpu -j 8` — D.3's named
    step; this package rewrote that kill switch's caller.
14. `ctest --test-dir build-verify -L integration-verify` and the verify retrace sweep (`grep -l 'Fatal{'` empty,
    `grep -L 'MGPipe verify:'` empty), plus `ctest --test-dir build-verify -R HandleRecycle` (G8) including package D's
    buffer arm.
15. **The retrace sweep**: `python3 ~/w7/retrace_gate.py --tree ~/w7/p3a-espryt --lib build-push/libMobileGL.so --out
    ~/w7/retrace-out/p3a-espryt -j 4`, plus G3b's named `--only` set.
16. **The buffer/VAO family by name** under the default mask (C.2's last regex), watching `DoublePrecisionScenario`
    (D7), the two `VertexAttribBindingScenario` base-instance cases and all eight `DrawParametersScenario` cases
    (D2/D-H2 — and ID-10's three client call sites, which must be present for them to go green),
    `PrimitiveRestart`/`ResidentIndex`/`MultiDraw` (the element-array restores against `m_syncedIndexSerial`),
    `CrossFrameBuffer`/`StreamedArena` (the Mali WAR queue-only `SubData` and the tier ladder),
    `LargeArenaAdoption`/`StorageBufferRegrow` (`Ops_H_MapPersistent`, `mpr`, `InvalidateIndexedBufferBindingShadowsForId`,
    **and `d7655247`'s three new cases** — the handle-arm half of that fix has never run), and
    `AtomicCounter`/`BufferTexture`/`PackedWordReadback`/`XfbCaptureBufferReuse` (the reverse channel and the epoch
    order).
17. **Two contract points to confirm against the merged `client`**, both now written into this package's code:
    `OnGpuWritten`'s whole-resource range shape **and** that the client's implementation sets `m_hasDefinedContent`
    as well as `m_gpuWritePending` (m8); and that `MGPVertexBuffer::BindingIndex` is the attribute index for every
    entry (D-H3), which `VertexBufferForBindingIndex`'s fast path assumes.
18. **Record the per-draw handle-resolution cost in D.4** (m6). It is now memoised through the slot table's front
    memo, so the number to publish is the memo's hit rate, not a raw `FindByLifetimeId` count.
19. `bash scripts/p3a_vertex_input_negative_control.sh` and package D's `ResourceSubsystemControlScenario`, which are
    what turn M-3's refusal and M-4's verdict from log lines into assertions.

**Still open and NOT this package's to close**, recorded so it is not lost: `wire-review-v1` M-D (the applier's
`RecordAt` resizes on an unbounded client slot — the backend half is bounded here, the applier half is A's); the
dangling-`ResolvedDrawBuffers::Entry::resource` invariant after `ReleaseByHandle` (both consumers check identity
through `FindByHandle` first, so nothing is dereferenced today, but the invariant is the only thing between that memo
and a use-after-free and deserves a sentence at the member in a later pass).

---

*Intermediate logs (`~/w7/p3a-espryt2-{lanes,ab,nolegacy}.log`, `build-nolegacy/`, the scratchpad's WSL scripts and
the copied `scripts/p3a_untouched_regions.sh`) are deleted; `git status` is clean.*
