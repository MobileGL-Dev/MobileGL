# P3a package A, commits c1-c3 — the applier bodies (`wire-v1`)

Branch `p3a/wire`, worktree `/home/swung/w7/p3a-wire`, parent `39722687` (tag `p3a/contract`). **Not pushed.**
Three commits, three files, +1066 / -46. Only A's own files after the tag: `MG_Pipe/PipeApply.{h,cpp}` and the
applier-side half of `MG_Test/Pipe/ResourceEmitTest.cpp`.

| commit | message (single line, no body) |
|---|---|
| `db0c93bb95e166e9e184db720572a41af4abbf77` (`db0c93bb`) | `[Feat] (Pipe): apply the resource calls into a per-context slot-indexed record and dispatch them to the backend by handle` |
| `cede041deb190cde6eebd13e93c26a21fcee13b1` (`cede041d`) | `[Feat] (Pipe): apply vertex elements, vertex buffers and the index buffer into the server's own working state` |
| `e9b4a263c41b1d38eb88458a3b2f267e5651a6f8` (`e9b4a263`) | `[Test] (Pipe): pin the resource record's lifecycle and the buffer sub-data range encoding at both of its bounds` |

```
 MobileGL/MG_Pipe/PipeApply.cpp             | 549 ++++++++++++++++++++++++++--
 MobileGL/MG_Pipe/PipeApply.h               |  15 +-        (comments only, §3 A1)
 MobileGL/MG_Test/Pipe/ResourceEmitTest.cpp | 548 +++++++++++++++++++++++++++-
```

---

## 1. The shared machinery (`c1`, file-static, `PipeApply.cpp`'s anonymous namespace)

| helper | what it is |
|---|---|
| `RecordAt(Vector<R>&, slot)` | grows a slot-indexed table to hold `slot`. Slot spaces are dense per kind, so the server's table is an array a handle indexes, never a map. |
| `FindResource(MGPipeHandle)` / `FindVertexElements(MGPipeHandle)` | null unless the slot is in range **and** `Live` **and** `Gen` matches. Same three questions `FindCso` already asked for the CSO store. |
| `BufferRangeFault(offset, size, width)` | null when `[offset, offset+size)` is inside `width`. Written so nothing can overflow: `offset > width` is answered before the subtraction the second question needs. |
| `SubDataBoxFault(record)` | the buffer half's convention held to itself: offset `<= 0x7FFFFFFF` (a corrupt negative `UnionBox.X` read unsigned lands above it — that is what the decoder's own comment promises), `Level == 0`, `RegionCount == 0`. |
| `PinNoLiveHostWrites(...)` | `#if MOBILEGL_PIPE_VERIFY` only: `Fatal{PipeLiveHostWrites}` if a record ever arrives with `HasLiveHostWrites` set (D-A4's pin). Empty inline otherwise. |
| `ApplyBufferWrite(call, record, bytes, resident)` | the single gate + serial + dispatch path shared by `resource_subdata` and `buffer_subdata_resident`, so the bounds arithmetic exists once. |

**Two verdict classes, deliberately different** (written into the file beside the helpers):

- **a dead / stale / unknown handle** → `MOBILEGL_ASSERT` + a **defined no-op**: nothing stored, nothing dispatched,
  no serial moved. This is `MGPipeApplyBindRenderState`'s existing precedent for a dead CSO, and it is *not* a trip
  wire on purpose — see §3 D2, which names the legal sequence a wire here would abort on.
- **a record that does not describe its own bytes** → `Fatal{ProtocolCorruption}` with the record's identity in the
  line, and the call refused. That is the class `ARCHITECTURE.md:119` reserves the tag for: the fault would have the
  *backend* read or write outside a store.

---

## 2. Per entry point

### 2.1 The nine resource calls (`c1`)

| entry point | record | serial | bounds gate | dispatch |
|---|---|---|---|---|
| `ResourceCreate` | slot 0 refused; otherwise the record is **started over** (`record = {}`) and given `Gen`, `Live`, `Desc`. A recycled slot inherits nothing — not a `Width`, not a `Serial`, not an `Immutable`. | stays **0**: a create is not a mutation, it defines no storage, and a fresh backend twin starts its own synced serial at 0, so the two agree from the first instant. | none (no range) | `ops->Create(desc.Resource, desc)` |
| `ResourceRespecify` | descriptor replaced **whole** (not merged): a respecify restates extent, usage, storage flags, immutability and defined-content. | **++** | none | `ops->Respecify(res, desc, initialBytes)` |
| `ResourceSubData` | nothing (contents are the backend's) | **++** | `SubDataBoxFault` then `BufferRangeFault(offset, size, Desc.Width)` then `size != 0 && bytes == nullptr` | `ops->SubData(res, record, bytes)` |
| `BufferSubDataResident` | nothing | **++** | identical | `ops->SubDataResident` **if non-null** (`kOptional`, the frontend's own shape today) |
| `ResourceFlushRange` | nothing | **++** | `BufferRangeFault(record.Offset, record.Size, Desc.Width)` + null-bytes. `AccessFlags` are passed through **unnormalised** — the backend reads INVALIDATE_RANGE / INVALIDATE_BUFFER / UNSYNCHRONIZED per call. | `ops->FlushRange(res, record, bytes)` |
| `ResourceReadback` | nothing | **none** — a readback does not mutate the store; a bump would tell the twin its memo is stale and buy a re-upload of what it had just read out | `BufferRangeFault` | `ops->Readback(res, record)`, **synchronously**, so the writeback and the epoch bump that must follow it have both happened before this returns (D-D step 5) |
| `ResourceDestroy` | record dropped **whole**, `Gen` **kept** (the client allocator owns the bump and takes it on the next handout). Applier state is consistent **before** the backend is told. | reset to 0 with the record | none | `ops->Destroy(handle)` |
| `MapPersistent` | none | **none** (the donation re-mints a *driver* object; the backend's own id generation catches that) | none | `++MapPersistentRoundtrips` **first and unconditionally** (attempts, not acquisitions — mint *or* decline), then `ops->MapPersistent`; a missing table/member is a **decline**, i.e. `nullptr`, which is a real answer |
| `UnmapPersistent` | none | none | none | `ops->UnmapPersistent` |

`kNeedsAck`: `MGPipeResourceRespecifyNeedsAck(desc)` is **documented at the call site and deliberately not branched
on** — in monolith the acknowledgement *is* the return of `MGPipeApplyResourceRespecify`, so both arms of such a
branch would be identical and the transport would have to find and delete it. `PipeCatalogueTest`
(`ResourceRespecifyAcksOnlyImmutableStorage`) plus c3's `ARespecifyReplaces…` keep the predicate honest until the
doorbell reads it.

**Serial ordering, and C depends on it:** the serial is bumped **before** the backend hook is called, because Espryt
stamps its own `syncedChangeSerial` from `record.Serial` *inside* the hook. A bump afterwards would leave the twin
one mutation behind and the next draw would re-upload what it had just landed.

### 2.2 The five vertex-input calls (`c2`)

These **dispatch to nobody**, and that is not an omission: `MGPipeResourceOps` is the *resource* family's table.
The backend does not act on vertex input when it arrives — it reads this state at its own draw-time sync, which is
what the three serials are for.

| entry point | record | serial | bounds gate |
|---|---|---|---|
| `CreateVertexElements` | slot 0 refused. An existing record of the **same identity** keeps its serial (a re-create on the same handle is how a configuration change travels); a **different** `Gen` is a recycled slot and starts over. Both arrays are zeroed before the unpack, so a configuration that shrinks leaves nothing of the one before it. `memcpy` (a blob pointer carries no alignment guarantee). | `++ContentSerial`; the first create of an identity lands on **1**, so **0 means "never created"** and a twin that has synced nothing can never accidentally match. | counts `<= kMGPipeMaxVertexAttribs`; **`desc.Blob.Size` must equal `AttributeCount*24 + BindingPointCount*16` exactly**; non-empty blob must carry bytes. Any of the four → `Fatal{ProtocolCorruption}` naming `{slot, gen}`, both counts, both byte lengths. **Does not rebind.** |
| `BindVertexElements` | null handle is legal and clears the binding; a dead handle leaves the previous binding untouched (`bind_render_state`'s precedent) | — | kind assert |
| `DeleteVertexElements` | dropped whole, `Gen` kept; clears `BoundVertexElements` if it named this handle | — | — |
| `SetVertexBuffers` | entries copied into `[Start, Start+Count)` and **nowhere else** — entries outside the window are not cleared, because the record is "the last set as received" and a set that names four entries has said nothing about the rest. `VertexFetchBaseInstance = hdr.BaseInstance`, **raw** (§3 A1). | `++VertexBuffersSerial` | `Start + Count <= kMGPipeMaxVertexAttribs`, `Count != 0 => tail != nullptr` → `Fatal{ProtocolCorruption}` naming `{start, count, hash}` |
| `SetIndexBuffer` | stored verbatim | `++IndexBufferSerial` | **none, with three reasons in place**: `Res` may legitimately be null (no element-array buffer bound); `Offset`/`IndexSize` are the *draw's* and are 0/0 until one supplies them, so there is no extent to check against; and it is an independent call, not a subset of the configuration (D5) |

---

## 3. Deviations, each with its reason

**A1 — `VertexFetchBaseInstance` stores the RAW value; the applier does not resolve the shift.** D-H2.2 says
"Espryt's applier computes `fetchBaseInstance = UseNativeBaseInstance() ? 0 : hdr.BaseInstance`", and c0's header
comment said the member holds a value "resolved by the server's own decision". `UseNativeBaseInstance()` is a
**backend capability** living in `MG_Backend`, and `MG_Pipe` sits below it and may not ask (purity gate A; there is
no capability hook in `MGPipeResourceOps`, which is a resource table). Emulation is server-owned and the backend
*is* the server, so the raw value is stored and **package C's applier arm** turns it into a per-attribute byte
shift out of each attribute's own stride and divisor (`BaseInstanceByteShift`). The two comments in `PipeApply.h`
that said otherwise were corrected in `c2` — **comments only, no signature and no member changed**; this is the one
post-tag edit to a contract file and it is recorded here for the other three packages.

**A2 — a dead / unknown handle is a defined no-op with a debug assert, not `Fatal{ProtocolCorruption}`.** A resource
call is applied at the GL call that causes it, while `MGPipeApplierReset()` runs at the **first validate of a fresh
context** (`PipeFill.cpp:1190`, `tracker.FreshlyPrimed()`). A buffer that outlives a context switch — a shared store,
a worker context — therefore reaches this applier with its record already dropped, through no fault of any package.
A wire there would abort the verify lane on a *legal* sequence, which is the failure mode `ROADMAP.md:7`'s "每个门必须
能因它存在的理由变红" is the other half of. The refusal is loud in a debug build and silent-but-defined in the three
gate builds (INFO ⇒ `MOBILEGL_ASSERT` compiles out), and `ResourceEmit.ADestroyDropsTheRecord…` pins that it changes
nothing. **This is a real hole in the phase's design, not just in this package — see §5 item 1.**

**A3 — `MGPResourceDesc::Target` is not gated.** The field's documented value set ("Buffer | Tex1D.. | Renderbuffer |
TexBuffer") has **no minted enumerator** anywhere in the tree — `TextureTarget` has no `Buffer`. Gating on a value
this package would have to guess would hand package B a contract it could not read off any header. The buffer half is
identified by the **call**, which is what `PipeCalls.def` already says, and the box convention is what is checked.

**A4 — one new trip-wire tag beyond the specified one.** `Fatal{ProtocolCorruption}` is `ARCHITECTURE.md:119`'s and is
used exactly as specified. `Fatal{PipeLiveHostWrites}` is new and is D-A4's requirement spelled as this file spells
verdicts ("a `MOBILEGL_PIPE_VERIFY` assertion pins that it is false") — `MOBILEGL_ASSERT` would have been inert in
every build that ships, which is the mistake the file's own header comment forbids.

**A5 — `c3` is eight cases, and one of them is not push-gated.**
`ResourceEmit.TheSubDataRangeEncodingRefusesExactlyAtItsTwoBounds` exercises an **inline function of the payload
header**, which exists in a pull build too, so it runs in all three trees rather than skipping in one. The other
seven skip visibly in a pull build, as the file's header requires.

**A6 — the splitter's own case is not here.** `c3`'s brief line asks for "the emitter's splitter produces contiguous,
non-overlapping records that reassemble to the original range", but the splitter lives in package B's
`MG_Impl/Pipe/ResourceTracker.h`, which does not exist on this branch (C.5 gives B that file). What `c3` pins is what
the splitter is written **against** — the range encoding and both of its bounds, plus that a refused encoding leaves
the record untouched, which is what lets the emitter split against the very record it just tried. The split test is
**B's**, and the file's header comment now says so in those words.

**A7 — `bash scripts/p3a_untouched_regions.sh` (G5) could not run.** The script is package D's (ID-3) and is not in
the tree. This package touches no `Managers.cpp` region at all, so the nine functions are byte-identical by
construction.

**A8 — `check_include_closure.py --compiler clang++-20`** does not exist on this host (already recorded by c0);
`clang++` was used, and the no-argument form was run as well.

---

## 4. Verification transcript

All in `/home/swung/w7/p3a-wire` at `e9b4a263`, `CCACHE_BASEDIR=/home/swung/w7`.

**G1 — pull build symbol identity**, rc **0**:

```
python3 scripts/symbol_report.py --before /home/swung/w7/p3a-before-libMobileGL.so \
        --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0
symbol-report: Total 17209979 -> 17209979 (+0)
symbol-report: 27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
`.text` 10792739 -> 10792739 (+0, +0.000%). ... 27060 unchanged.
symbol-report: .data 76824 -> 76824 (+0)   .bss 1296872 -> 1296872 (+0)   .rodata 1534938 -> 1534938 (+0)
```

P3a's admitted-resize set is empty, as §A.2 G1 requires. (Everything c1/c2 add is inside `PipeApply.cpp`, which is
compiled only under `MOBILEGL_PIPE_PUSH`; c3 is a test executable.)

**Generators and closure**

```
python3 scripts/gen_pipe.py --check        rc 0  "gen_pipe: generated files are up to date"
                                                 "71 calls (11 screen, 60 context), 71 verify payloads,
                                                  63 PipeInputs fields (7 sticky, 34 emitted), 69 verbs, 9 classes"
                                                 "inventory 477 rows: ... 0 UNMAPPED"
python3 scripts/gen_pipe.py --self-test    rc 0  "7 negative-control trip(s), positive control OK"
git diff --exit-code -- MobileGL/MG_Pipe/generated                                            rc 0
python3 scripts/check_include_closure.py                                                      rc 0
        "4 probes, 0 skipped, 0 problem(s)"  (value 66 / artifacts 65 / mutation 84 / wire 2)
python3 scripts/check_include_closure.py --mode both --compiler clang++ --self-test --require-all
                                            rc 0  "6 negative-control trip(s), parser checks OK"
```

**Three builds** (`cmake --build <dir> -j 24`, at HEAD): `build-linux` rc **0**, `build-push` rc **0**,
`build-verify` rc **0**.

**Three unit runs** (`ctest --test-dir <dir> -L unit -j 12 --no-tests=error`):

```
build-linux   rc=0   100% tests passed, 0 tests failed out of 1579   (16.83 s)
build-push    rc=0   100% tests passed, 0 tests failed out of 1579   (14.16 s)
build-verify  rc=0   100% tests passed, 0 tests failed out of 1579   (12.85 s)
```

(1571 at `c0` + 8 new names.)

**Name sets (G2's name half, G14)** — ID-4's extractor:

```
ctest -N names: build-linux 2495, build-push 2495, build-verify 3323
diff <build-linux names> <build-push names>                    -> empty (rc 0)
comm -23 ~/w7/p3a-before-ctest-names.txt <build-linux names>   -> empty   (nothing removed)
comm -13 ...                                                   -> 13 added: c0's 5, plus
    ResourceEmit.ACreateMarksTheSlotLiveAndCarriesItsDescriptor
    ResourceEmit.ADestroyDropsTheRecordAndAStaleGenerationResolvesToNothing
    ResourceEmit.ARespecifyReplacesTheDescriptorAndOnlyAMutationMovesTheSerial
    ResourceEmit.AWriteOutsideTheDeclaredStorageIsRefusedNamingTheResource
    ResourceEmit.EveryResourceCallDispatchesByHandleThroughTheInstalledTableOnly
    ResourceEmit.MapPersistentCountsEveryAttemptWhetherItMintsOrDeclines
    ResourceEmit.NoResourcePathInThisPhaseLeavesHostWritesLive
    ResourceEmit.TheSubDataRangeEncodingRefusesExactlyAtItsTwoBounds
```

**The suite itself, in all three trees** (`ctest -R 'ResourceEmit\.' --no-tests=error --output-on-failure`):

```
build-push    9/9 Passed
build-verify  9/9 Passed   (the bounds case takes its forked-abort arm here and reads
                            "Fatal{ProtocolCorruption} resource_subdata {slot=7, gen=3, glName=41}"
                            out of the log after SIGABRT)
build-linux   8 Skipped, 1 Passed (the range encoding)
```

**G13's two greps**: `grep -c 'MG_State' MobileGL/MG_Pipe/PipeApply.h` → **0**;
`grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'` → empty.

**Ownership**: `git diff --stat 39722687..HEAD` names exactly `MG_Pipe/PipeApply.cpp`, `MG_Pipe/PipeApply.h` and
`MG_Test/Pipe/ResourceEmitTest.cpp`. Working tree clean. Nothing pushed; no other worktree touched.

**Not run here, and why**: G2's execution half, G3/G3b/G4's retrace, G5 (§3 A7), G6-G12, G15. This package registers
no `MGPipeResourceOps` and emits no call, so it changes no observable and those gates belong to the integrator's D.1
merges.

---

## 5. What is left, and for whom

1. **The context-reset hole (§3 A2) — for the integrator, B and C.** `MGPipeApplierReset()` drops `Resources` at the
   first validate of a fresh context, but buffer objects are share-group-scoped and can outlive a context switch. The
   applier's refusal is defined and reported, so nothing corrupts — but a `glBufferSubData` on a shared store after a
   context switch would be **dropped** rather than applied. B (whose `ResourceTracker.h` mints the handles) or the
   integrator must decide: re-emit `ResourceCreate` + `ResourceRespecify` on first use after a reset, or take
   `Resources` / `VertexElementsCsos` out of the reset because they describe share-group objects. This is not
   reachable today (nothing emits), and it must be settled before `p3a/client` lands.
2. **`create_vertex_elements` requires `desc.Blob.Size` to be filled — for B.** The applier refuses, as
   `Fatal{ProtocolCorruption}`, any record where `Blob.Size != AttributeCount*24 + BindingPointCount*16`. In monolith
   the *bytes* travel through the companion pointer, but the **declared length must still be set**, or every emission
   aborts a verify build. Both counts are also bounded by 32.
3. **`resource_subdata` requires a `Width` — for B.** The applier refuses a write outside `record.Desc.Width`, so the
   store's extent must reach the applier (via `ResourceRespecify`) before any write to it, and `Width` must be the
   real `GetSize()`. Same for `resource_flush_range` and `resource_readback`.
4. **`VertexFetchBaseInstance` is raw — for C.** The applier arm computes
   `UseNativeBaseInstance() ? 0 : MGPipeApplier().VertexFetchBaseInstance` itself; `EmulatedFetchBaseInstance` and
   `BaseInstanceByteShift` move inside that arm, exactly as D-H2.2 describes, but the `?:` is C's line and not this
   file's.
5. **Serial contract — for C.** `Serial` 0 = created-never-mutated; `ContentSerial` 0 = never created (first create
   lands on 1); both bumped **before** the backend hook runs; both reset on a create at a recycled slot, and the twin
   re-keys on `Gen` (A's `BackendSlotTable::GetOrCreate` already resets a twin whose `Gen` moved).
6. **The splitter's case — for B** (§3 A6), appended beside these in `ResourceEmitTest.cpp` as a disjoint `TEST`
   body; A landed first, so B's edit is an append.
7. **The vertex-input applier's own cases — for B**, in `VertexInputEmitTest.cpp` (C.5 gives B that file's contents
   beyond c0's placeholder). Worth covering there, because `c3` could not: the counts/length refusal, a blob unpack
   round trip over all 32 slots, `Bind`/`Delete` and the null-handle bind, and that a create at a recycled slot resets
   `ContentSerial` while a re-create on the same handle counts up.
8. **Doc corrections owed to the integrator**, beyond c0's list: D-H2.2's "Espryt's applier computes …" reads as if
   `MG_Pipe`'s applier resolved the shift; it is the backend arm (§3 A1). D-A2's `MGPResourceDesc::Target` row names a
   value set with no enumerator behind it (§3 A3).

Intermediate logs this package created — `~/w7/p3a-wire-build-*.log` and `~/w7/p3a-wire-names-*.txt` — were deleted.
`~/w7/p3a-trees2.log` is kept, as instructed.
