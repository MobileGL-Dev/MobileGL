# P3a package A, commit c0 — the contract (`contract-v1`)

**Commit** `397226873c2fe06ed57a5b78058ff68bc1716896` (`39722687`) on branch `p3a/contract`, worktree
`/home/swung/w7/p3a-contract`, parent `44c2b5cf` (ID-1's base ref, not the brief's `55d2af9b`).
**Tag** `p3a/contract` points at it. Not pushed.

Message, single line, no body:

```
[Feat] (Pipe): land the P3a contract - the handle-shaped resource op table, the applier's resource and vertex-elements records, the two vertex wire views, an explicit baseInstance on set_vertex_buffers, and the first kNeedsAck
```

`p3a/{wire, client, espryt, gates}` can branch from the tag now.

---

## 1. Per-file summary

21 files, +1049 / -23. Every one of them is in C.0's table; C.0 lists two files that were deliberately not
touched (§3 D1, D2).

| file | what changed |
|---|---|
| `MobileGL/MG_Pipe/MGPipeValueTypes.h` | **+`MGPVertexAttribWire` (24 B) and `MGPVertexBindingPointWire` (16 B)**, in `namespace MobileGL` immediately after the `MG_State::GLState` block that holds `VertexAttribute` / `VertexBufferBindingPoint`, with the blob-layout rule, the "stride 0 is meaningful" note, the reason `Divisor` and `LegacyStride`/`LegacyPointer` are absent, and the surviving reason the second view travels at all. Four `static_assert`s (trivially-copyable + exact size + standard layout) in the `:547` trip-wire block, spelled out rather than `MGP_ASSERT_POD` because that macro is `MGPipeTypes.h`'s and that header includes this one. |
| `MobileGL/MG_Pipe/MGPipeTypes.h` | **`MGPVertexBuffers` 16 → 24 B**: `Uint32 BaseInstance; Uint32 Pad0;`, `MGP_ASSERT_POD(..., 24)`, comment naming the `ContentHash` requirement, the `MGPDrawInfo::StartInstance` distinction and the once-per-set rule. **+`MGPipeResourceRespecifyNeedsAck(const MGPResourceDesc&)`** next to `MGPipeSetSubDataBufferRange`. **+`kMGPipeMaxVertexAttribs = 32`** (see §3 A1). `MGPVertexBuffer` (per entry, 32 B) untouched. |
| `MobileGL/MG_Pipe/PipeCalls.def` | `ResourceRespecify` flags `kNone` → **`kNeedsAck`**; the `:18` legend gains the "MAY require an ack; a per-record predicate decides" sentence naming the predicate. No row added, moved or reordered; `MGP_CALL_LIST_DOCUMENTED_COUNT` stays **71**. |
| `MobileGL/MG_Pipe/PipeFields.def` | `MGP_FIELDS_MGPVertexBuffers` gains `F(BaseInstance)`; new `MGP_FIELDS_MGPVertexAttribWire` (10 fields, `Pad0` excluded) and `MGP_FIELDS_MGPVertexBindingPointWire` (3); **both appended to `MGP_VERIFY_PAYLOAD_LIST`** (69 → 71 payloads). |
| `MobileGL/MG_Pipe/MGPipe.h` | `kMGPipeSubsystemResources` (bit 7), `kMGPipeSubsystemVertexInput` (bit 8), `kMGPipeSubsystemsMigratedAtP3a = 0x1ff`; the reserved-bit comment moves to "bits 9..62"; `kMGPipeSubsystemsMigratedAtP2` kept, with the note that each phase's constant survives as the next phase's A/B control. |
| `MobileGL/MG_Pipe/PipeApply.h` | `MGPipeResourceOps` (9 members) + `MGPipeSetResourceOps` / `MGPipeGetResourceOps`; `MGPipeResourceRecord`; `MGPipeVertexElementsRecord`; **11 new `MGPipeApplierState` members**; **14 new entry points**, complete signatures, in two commented sections. No `MG_State` token anywhere in the file (G13). |
| `MobileGL/MG_Pipe/PipeApply.cpp` | file-static `g_resourceOps` + the two accessors (real bodies); the 14 entry points as **no-op stubs** with the block comment saying why a no-op is unreachable here rather than merely empty; `MGPipeApplierReset` clears all 11 new members and deliberately does **not** clear the op table. |
| `MobileGL/MG_Pipe/Coverage.def` | `GetBufferBindingSlot`'s comment **rewritten**: the per-target split is now stated target by target (vertex / index / indirect / shader-buffers / still-pulled), sourced from the emission site rather than from a re-vendored inventory, plus why the row stays one row and why it is not in the emitted list. `MGP_COVERAGE_EMITTED_LIST` gains **`X(GetBoundVertexArray, BindVertexElements)`** with a note for the commit that wires it. The `Buffer ops delta → ResourceRespecify` delta row is untouched. |
| `MobileGL/MG_Impl/Pipe/PipeFill.cpp` | *(contract-commit-only, enum-coupled block)* `SubsystemForEmitter` gains the `BindVertexElements → kMGPipeSubsystemVertexInput` arm; four new `static_assert`s (one direct, three conditional pairings — §3 B1) plus the `kMGPipeMaxVertexAttribs` ↔ `VertexArrayObject::MAX_VERTEX_ATTRIBS` drift assert; `kMGPipeWiredSubsystems` **unchanged, with the reason recorded in place** (§3 B2); three **stub emitters** `Emit{VertexElements,VertexBuffers,IndexBuffer}` returning 0 and their three `wants()`-gated call sites in `MGPipeValidateForVerb` step 3, in the fixed order after the four P2 emitters. |
| `MobileGL/MG_Pipe/generated/PipeVerify.inc` | regenerated: two forward decls, two `MGPipeHasFieldVerifier` specialisations, two comparators, `kMGPipeVerifiedPayloadCount` 69 → 71. |
| `MobileGL/MG_Pipe/generated/PipeFilled.inc` | regenerated: `MGPipeFieldEmitter::BindVertexElements`, its name-table row, `kMGPipeFieldEmittedBy[GetBoundVertexArray]`, `kMGPipeEmittedFieldCount` 33 → 34. |
| *(the other six `.inc`)* | regenerated, byte-identical (§3 C1). |
| `MobileGL/MG_Backend/DirectGLES/SlotTables.h` | `BackendSlotTable::GetOrCreate(MGPipeHandle)` — never calls `MGPipeSlots()`, no `StatePtr`, arms the teardown sentinel, resets a twin whose `Gen` no longer matches. Header-only, uninstantiated, both arms compile. `FindByHandle(MGPipeHandle)` already existed and already met the requirement (§3 C2). |
| `MobileGL/MG_Util/Metrics/PipeStats.{h,cpp}` | `CallClass::MapPersistentRoundtrips` inside the existing `#if MOBILEGL_PIPE_PUSH` block, name `"map-persistent-roundtrips"`, `FormatWindowLine` prints ` mpr=<n>` beside `csom`/`csob`. |
| `MobileGL/MG_Test/Util/PipeStatsTest.cpp` | `mpr=` pinned in `SummaryLineCarriesEveryClassAndGate`, the name pinned in `CounterNamesAreStable`, both inside the push guard. |
| `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp` | `kMGPipeVerifiedPayloadCount` 69 → **71** (a required consequence the brief does not mention); two `MGPipeHasFieldVerifier` static asserts; **3 new cases** — `VertexBufferSetCarriesAnExplicitBaseInstance` (size 24, `Pad0` is not a field, comparator names `BaseInstance`, per-entry struct unchanged), `VertexWireViewsAreFlatAndCarryIsLongSeparately` (both sizes, comparator names `IsLong` and `Divisor`), `ResourceRespecifyAcksOnlyImmutableStorage` (the flag is on the call, it is the **only** call carrying it, the predicate is true for `Immutable=1` and false for a `STATIC_DRAW` + defined-content mutable store, and opcode 3 did not move). |
| `MobileGL/MG_Test/Pipe/ResourceEmitTest.cpp` | **new**, suite `ResourceEmit`, own `main()`, one permanent case `TheResourceOpTableIsUnregisteredUntilABackendInstallsOne` (§3 D3). |
| `MobileGL/MG_Test/Pipe/VertexInputEmitTest.cpp` | **new**, suite `VertexInputEmit`, own `main()`, one permanent case `AResetApplierCarriesNoVertexInputStateOver`. |
| `MobileGL/MG_Test/Pipe/CMakeLists.txt` | both registered (`GTest::gtest`, own `main()`), `gtest_discover_tests(... PROPERTIES LABELS unit)`. Re-opened at `44c2b5cf` per ID-1: it already carried `MagmaPipeIdentityTest`. |
| `MobileGL/Config.h` | bit-list comment gains `0x80 resources` and `0x100 vertex input`; the default now names `kMGPipeSubsystemsMigratedAtP3a` and records `0x7f` as the phase-by-phase control. |
| `MobileGL/ConfigLoader.cpp` | push default `kMGPipeSubsystemsMigratedAtP2` → **`kMGPipeSubsystemsMigratedAtP3a`** (0x7f → 0x1ff). |

---

## 2. The contract, verbatim — what B, C and D compile against

### 2.1 The two wire structs (`MG_Pipe/MGPipeValueTypes.h`, `namespace MobileGL`)

```cpp
struct MGPVertexAttribWire {
    Uint64 Offset;       //  0
    Int32 Stride;        //  8
    Uint32 Type;         // 12  DataType
    Uint8 Size;          // 16  1..4; GL_BGRA keeps 4
    Uint8 Enabled;       // 17
    Uint8 Normalized;    // 18
    Uint8 IsInteger;     // 19
    Uint8 IsLong;        // 20  carried separately from Type == Float64
    Uint8 IsBgra;        // 21
    Uint8 BindingIndex;  // 22  < kMGPipeMaxVertexAttribs
    Uint8 Pad0;          // 23
};                                                  // sizeof == 24

struct MGPVertexBindingPointWire {
    Uint64 Offset;  // 0
    Int32 Stride;   // 8   initial value is 16, not 0
    Uint32 Divisor; // 12
};                                                  // sizeof == 16
```

Blob layout on `CreateVertexElements`: `MGPVertexAttribWire[AttributeCount]` immediately followed by
`MGPVertexBindingPointWire[BindingPointCount]`, both ascending, both counts `<= kMGPipeMaxVertexAttribs`.

### 2.2 `MGPVertexBuffers` (`MG_Pipe/MGPipeTypes.h`)

```cpp
struct MGPVertexBuffers {
    Uint32 Start, Count;
    Uint32 BaseInstance;   // NEW: the draw's raw vertex-FETCH base instance, one per set
    Uint32 Pad0;           // NEW
    Uint64 ContentHash;    // must include BaseInstance
};                                                  // sizeof == 24 (was 16)

inline constexpr Uint32 kMGPipeMaxVertexAttribs = 32;   // NEW
inline Bool MGPipeResourceRespecifyNeedsAck(const MGPResourceDesc& desc) { return desc.Immutable != 0; }  // NEW
```

`MGPVertexBuffer` (per-entry, 32 B) is unchanged; its `Pad0` stays padding.

### 2.3 `MGPipeResourceOps` (`MG_Pipe/PipeApply.h`, inside the existing push guard)

```cpp
struct MGPipeResourceOps {
    void  (*Create)         (MGPipeHandle res, const MGPResourceDesc& desc);
    void  (*Respecify)      (MGPipeHandle res, const MGPResourceDesc& desc, const void* initialBytes);
    void  (*SubData)        (MGPipeHandle res, const MGPSubData& record, const void* bytes);
    void  (*SubDataResident)(MGPipeHandle res, const MGPSubData& record, const void* bytes); // may be null
    void  (*FlushRange)     (MGPipeHandle res, const MGPFlushRange& record, const void* bytes);
    void  (*Readback)       (MGPipeHandle res, const MGPReadback& record);
    void  (*Destroy)        (MGPipeHandle res);
    void* (*MapPersistent)  (MGPipeHandle res, Uint64 size, const void* seedBytes);
    void  (*UnmapPersistent)(MGPipeHandle res);
};
void MGPipeSetResourceOps(const MGPipeResourceOps* ops);   // nullptr uninstalls
const MGPipeResourceOps* MGPipeGetResourceOps();           // nullptr until a backend installs one
```

### 2.4 The fourteen apply entry points

```cpp
void  MGPipeApplyResourceCreate       (const MGPResourceDesc& desc);
void  MGPipeApplyResourceRespecify    (const MGPResourceDesc& desc, const void* initialBytes);
void  MGPipeApplyResourceSubData      (const MGPSubData& record, const void* bytes);
void  MGPipeApplyBufferSubDataResident(const MGPSubData& record, const void* bytes);
void  MGPipeApplyResourceFlushRange   (const MGPFlushRange& record, const void* bytes);
void  MGPipeApplyResourceReadback     (const MGPReadback& record);
void  MGPipeApplyResourceDestroy      (const MGPHandleOnly& handle);
void* MGPipeApplyMapPersistent        (const MGPHandleOnly& handle, Uint64 size, const void* seedBytes);
void  MGPipeApplyUnmapPersistent      (const MGPHandleOnly& handle);

void  MGPipeApplyCreateVertexElements (const MGPVertexElements& desc, const void* blobBytes);
void  MGPipeApplyBindVertexElements   (const MGPHandleOnly& handle);
void  MGPipeApplyDeleteVertexElements (const MGPHandleOnly& handle);
void  MGPipeApplySetVertexBuffers     (const MGPVertexBuffers& hdr, const MGPVertexBuffer* tail);
void  MGPipeApplySetIndexBuffer       (const MGPIndexBuffer& record);
```

All fourteen are **no-op stubs** at `c0`; `MGPipeApplyMapPersistent` returns `nullptr` (a decline, which is a
supported answer).

### 2.5 The applier's records and state

```cpp
struct MGPipeResourceRecord {          // indexed by MGPipeHandle::Slot, kind Buffer; slot 0 never live
    Uint32 Gen = 0; Bool Live = false;
    MGPResourceDesc Desc{};
    Uint64 Serial = 0;                 // server-owned MGGen
    Bool HasLiveHostWrites = false;    // always false in P3a
};
struct MGPipeVertexElementsRecord {    // indexed by MGPipeHandle::Slot, kind VertexElementsCso
    Uint32 Gen = 0; Bool Live = false;
    Uint32 AttributeCount = 0, BindingPointCount = 0;
    Array<MGPVertexAttribWire,       kMGPipeMaxVertexAttribs> Attributes{};
    Array<MGPVertexBindingPointWire, kMGPipeMaxVertexAttribs> BindingPoints{};
    Uint64 ContentSerial = 0;          // server-owned MGGen
};

// appended to MGPipeApplierState:
Vector<MGPipeResourceRecord>       Resources;
Vector<MGPipeVertexElementsRecord> VertexElementsCsos;
MGPipeHandle                       BoundVertexElements = kMGPipeNullHandle;
Array<MGPVertexBuffer, kMGPipeMaxVertexAttribs> VertexBuffers{};
Uint32 VertexBufferStart = 0;
Uint32 VertexBufferCount = 0;
Uint32 VertexFetchBaseInstance = 0;
Uint64 VertexBuffersSerial = 0;
MGPIndexBuffer IndexBuffer{};
Uint64 IndexBufferSerial = 0;
Uint64 MapPersistentRoundtrips = 0;
```

`MGPipeApplierReset()` clears all eleven. It does **not** clear the op table.

### 2.6 Everything else B / C / D can rely on

- `kMGPipeSubsystemResources = 1ull << 7`, `kMGPipeSubsystemVertexInput = 1ull << 8`,
  `kMGPipeSubsystemsMigratedAtP3a = 0x1ffull`; push default is now `0x1ff`, `0x7f` is the P3a-off control.
- `BackendSlotTable::GetOrCreate(MG_Pipe::MGPipeHandle)` and the pre-existing `FindByHandle(MGPipeHandle)`.
- `PipeStats::CallClass::MapPersistentRoundtrips`, short name `mpr`, push-only.
- Stub emitters `EmitVertexElements/EmitVertexBuffers/EmitIndexBuffer(GLContext&) -> Uint64` in
  `PipeFill.cpp`'s anonymous namespace, already called from step 3 in the fixed order. **B replaces the
  bodies without moving a call site.**
- gtest suites `ResourceEmit` and `VertexInputEmit` (see D3).

---

## 3. Deviations from the brief, each with its reason

### A. Types

**A1 — `kMGPipeMaxVertexAttribs` was minted (not in the brief).** D-G4 writes the applier's arrays as
`Array<..., 32>`. A bare 32 in `MG_Pipe` that must equal `VertexArrayObject::MAX_VERTEX_ATTRIBS` is a
silent-drift hazard and `MG_Pipe` may not include a frontend header (purity gate A). So the constant is
declared once in `MGPipeTypes.h` and `PipeFill.cpp` — the one translation unit that sees both — carries
`static_assert(kMGPipeMaxVertexAttribs == VertexArrayObject::MAX_VERTEX_ATTRIBS)`.

**A2 — the wire structs use plain `static_assert`, not `MGP_ASSERT_POD`.** D-G2 writes `MGP_ASSERT_POD`, but
C.0 places the structs in `MGPipeValueTypes.h` and that macro is defined in `MGPipeTypes.h`, which *includes*
`MGPipeValueTypes.h`. The assertions are spelled out in the existing `:547` trip-wire block instead, in that
block's own style, and cover trivial copyability, exact size and standard layout.

**A3 — eleven new `MGPipeApplierState` members, not "ten".** C.0 says ten; D-G4's own code block declares
eleven (`VertexBufferStart` and `VertexBufferCount` are two members on one line, as are `IndexBuffer` and
`IndexBufferSerial`). D-G4's block is what landed, verbatim. No member was dropped or added.

### B. `PipeFill.cpp`'s enum-coupled block

**B1 — three of the four new pairing `static_assert`s are conditional, not equalities.** D-M asks for
`SubsystemForEmitter(...) == MGPipeSubsystemForDirty(...)` alongside three new `MGPipeSubsystemForDirty` arms
"all four in one commit". `MGPipeSubsystemForDirty` lives in `MG_Impl/Pipe/Tracker.h`, which **C.5 assigns to
package B**, and at `44c2b5cf` it returns 0 for bits 5/9/10. A direct equality would therefore fail to compile
at `c0` for a reason that is not a defect, and the only way to make it hold would be to edit B's file — which
hard rule (4) forbids and which would guarantee a rebase conflict. What landed:

- an unconditional `SubsystemForEmitter(BindVertexElements) == kMGPipeSubsystemVertexInput`;
- three of the form `MGPipeSubsystemForDirty(bit) == 0 || MGPipeSubsystemForDirty(bit) == kMGPipeSubsystemVertexInput`,
  for `NewVertexElements`, `NewVertexBuffers`, `NewIndexBuffer`. The escape hatch is *only* the not-yet-mapped
  case: the moment B maps a bit onto anything, the assertion becomes the equality D-M asks for. No later edit
  is required, and none is invited.

This is recorded as a brief-internal contradiction (D-M vs C.0/C.5) rather than a design change; the integrator
should correct D-M.

**B2 — `kMGPipeWiredSubsystems` does NOT gain the two bits at `c0`.** D-M asks for it in the same commit. It
would be a live bug. Traced through `MGPipeValidateForVerb` step 4 with the new default push mask `0x1ff`:
`supplied` for `GetBoundVertexArray` would evaluate `subsystem != 0` ✓, `subsystem & wired` ✓,
`pushMask & subsystem` ✓, `EmittedCallSuppliesTheWholeField` ✓ (default arm), `applierDerives` ✓ — so
`CopyField` would be **skipped** while the emitters are stubs and the applier's entry points are stubs, leaving
`PipeInputs::m_boundVertexArray` unwritten in every push and verify build. That is precisely the failure the
constant's own comment says it exists to prevent ("a field whose emitter is not wired here keeps being
pulled"). The reason is written into the file beside the constant, and B adds the bits in the commit that gives
the emitters their bodies.

**B3 — stub emitters are wired into step 3 at `c0`, not only declared.** C.0 says "stub emitters that emit
nothing"; the call sites were added too, gated by `wants()`, so B's commit is a body replacement rather than a
body plus a call-site edit at the validate point. They are unreachable today (their dirty bits map to no
subsystem) and return 0 so the payload histogram gains no bucket for bytes nobody sent.

### C. Generators and `Coverage.def`

**C1 — `scripts/gen_pipe.py` needed no edit.** C.0 lists it as MODIFY, "whatever the two new payloads and the
flag-word change require". Verified by reading the generator: `MGPipeValueTypes.h` is already in
`FIELD_LIST_STRUCT_HEADERS`, `gen_verify` is driven entirely off `MGP_VERIFY_PAYLOAD_LIST`, and no generator
materialises a call's flag word (`Call.Flags` is only consulted for `kVarTail` / `kReplySlot` signatures). So
`gen_pipe.py` is unchanged and **only two** of the eight `.inc` files actually moved. Related: **D-A5's claim
that "a flag-word edit changes the generated tables" is false in this tree** — `git diff --exit-code --
MobileGL/MG_Pipe/generated` was still run and is clean, but it is not what catches a missing regeneration for
that edit; `PipeCatalogueTest.ResourceRespecifyAcksOnlyImmutableStorage` is.

**C2 — only `GetOrCreate(MGPipeHandle)` was added to `SlotTables.h`.** C.0 asks for
"`GetOrCreate(MGPipeHandle)` / `FindByHandle` overloads that never call `MGPipeSlots()`".
`BackendSlotTable::FindByHandle(MGPipeHandle)` already exists at `44c2b5cf` and already meets the requirement
verbatim, so adding a second one would be churn. No `const` overload was added either — nothing needs one yet
and `MG_Backend/DirectGLES/**` is package C's from the tag on.

**C3 — `GetBufferBindingSlot`'s accessor row could not literally "split per target".**
`MGP_COVERAGE_ACCESSOR_LIST` *is* the `MGPipeInputField` enum and the `PipeInputs` field set (one row → one
enumerator → one member of `PipeInputs`' X-macro list, which is `MG_Backend`'s file), and the field is a single
array `m_bufferBindingSlot[kBufferTargetCount]`. A duplicate row would be a duplicate enumerator. The split is
therefore stated exhaustively in the rewritten comment (target → call, including the five targets that are
still pulled) and lives in the emitters; the row stays one row naming `SetIndirectBuffers`, and the comment
says why. It is deliberately kept **out** of the emitted list, because five of its targets are still pulled and
a row there would be the same half-truth `GetPixelStoreParameters` is excluded for.

**C4 — the emitted list gained exactly one row, `GetBoundVertexArray → BindVertexElements`.** C.0 says
"emitted-list vertex-input fields", plural. The accessor list contains exactly three vertex-input accessors:
`GetCurrentVertexAttribute` (already emitted since P2), `GetBufferBindingSlot` (C3) and `GetBoundVertexArray`.
The row is inert until B wires the subsystem (B2), and a note in `Coverage.def` tells the wiring commit that
the field is a `SharedPtr` to the frontend VAO which the applier does not reproduce, so the
`EmittedCallSuppliesTheWholeField` decision must be taken deliberately in `PipeFill.cpp` (B's file from the tag
on) rather than inherited silently from the row's presence.

### D. Build files and tests

**D1 — root `CMakeLists.txt` was not touched.** C.0's row is "the new client sources inside
`if (MOBILEGL_PIPE_PUSH)`". D-N fixes that P3a's new client files are **headers**
(`MG_Impl/Pipe/{ResourceTracker.h, VertexInputEmit.h}`), and `SOURCE_FILES` lists no headers, so there is no
source to add. The two other halves of that row — the push default and the bit-list comment — landed in
`ConfigLoader.cpp` and `Config.h`. If B later needs a `.cpp`, the root `CMakeLists.txt` is still A's file and
the contract would have to be re-opened; B was told the new files are headers for exactly this reason.

**D2 — `PipeCatalogueTest.cpp` also needed `kMGPipeVerifiedPayloadCount` 69 → 71.** Not in C.0's row, but a
mandatory consequence of appending to `MGP_VERIFY_PAYLOAD_LIST`; without it the suite fails.

**D3 — the two new gtest suites are `ResourceEmit` and `VertexInputEmit`, not `…Test`.** The brief names cases
as `VertexInputEmitTest.X` / `ResourceEmitTest.X` in prose (D-H2.3, C.1's list, E's risk table) but spells the
gates as `ctest -R 'VertexInputEmit\.'` (G6, G7) and `-R '…|VertexInputEmit\.|ResourceEmit\.'` (line 1488).
A suite named `VertexInputEmitTest` does not match `VertexInputEmit\.`, so with `--no-tests=error` G6 and G7
could never run. The suite names follow the gates and this directory's own convention
(`RenderStateSpansTest.cpp` → suite `RenderStateSpans`). **The prose case names in the brief must be read with
the `Test` dropped**: `VertexInputEmit.ABaseInstanceChangeAloneStillEmitsTheVertexBufferSet`, and so on.

**D4 — the two stubs link `GTest::gtest` and carry their own `main()`.** C.0 only asks for "one placeholder
`TEST`". The point of A owning the registration is that no later package edits
`MG_Test/Pipe/CMakeLists.txt`; a `gtest_main` target would force exactly that edit the first time a case needs
to read a trip wire's line back out of a log file (which both suites will: the applier's blob-bounds gate and
its protocol refusal report that way, and abort under poison/verify). The `main()` shape is copied from
`RenderStateSpansTest`/`PipeInputsTest`.

**D5 — the placeholders are permanent contract assertions, not throwaways.** G14 forbids removing a test name,
so a placeholder that B deletes would itself be a gate break. `ResourceEmit.TheResourceOpTableIsUnregisteredUntilABackendInstallsOne`
pins the fact the whole family's landability rests on (nothing is registered ⇒ every dispatch falls through)
plus the set/get round trip and that `MGPipeApplierReset` does not clear the table;
`VertexInputEmit.AResetApplierCarriesNoVertexInputStateOver` pins that a fresh context carries no handle, no
window and no serial over. Both skip visibly in a pull build.

---

## 4. Verification transcript

All commands in `/home/swung/w7/p3a-contract`, `CCACHE_BASEDIR=/home/swung/w7`, at the tagged commit.

**G1 — pull build symbol identity** (`symbol_report.py --before ~/w7/p3a-before-libMobileGL.so --after
build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0`), rc **0**:

```
symbol-report: Total 17209979 -> 17209979 (+0)
symbol-report: 27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
`.text` 10792739 -> 10792739 (+0, +0.000%). ... 27060 unchanged.
### Removed (0)   ### Added (0)   ### Resized (0)   ### Renamed only (same size) (0)
```

P3a's admitted-resize set is empty, as §A.2 G1 requires.

**Generators**

```
python3 scripts/gen_pipe.py --check        rc 0   "gen_pipe: generated files are up to date"
                                                  "71 calls (11 screen, 60 context), 71 verify payloads,
                                                   63 PipeInputs fields (7 sticky, 34 emitted), 69 verbs, 9 classes"
                                                  "inventory 477 rows: ... 0 UNMAPPED"
python3 scripts/gen_pipe.py --self-test    rc 0   "7 negative-control trip(s), positive control OK"
git diff --exit-code -- MobileGL/MG_Pipe/generated    rc 0
```

**Include closure** — the brief's form names `clang++-20`, which does not exist on this host (`clang++` is
22.1.6); both forms were run:

```
python3 scripts/check_include_closure.py                                            rc 0
    value-header OK 66 headers, 0 forbidden; artifacts-header OK 65; mutation-header OK 84; wire-header OK 2
    include-closure: 4 probes, 0 skipped, 0 problem(s)
python3 scripts/check_include_closure.py --mode both --compiler clang++ --self-test --require-all   rc 0
    include-closure: self-test: 6 negative-control trip(s), parser checks OK
    include-closure: 4 probes, 0 skipped, 0 problem(s)
```

**Three builds** — `cmake --build <dir> -j 24`

| build | rc |
|---|---|
| `build-linux` (pull) | 0 (339/339) |
| `build-push` | 0 (349/349) |
| `build-verify` | 0 (349/349) |

**Three unit runs** — `ctest --test-dir <dir> -L unit -j 12 --no-tests=error`, rc 0 in all three:

```
build-linux   100% tests passed, 0 tests failed out of 1571   (9.71 s)
build-push    100% tests passed, 0 tests failed out of 1571   (9.76 s)
build-verify  100% tests passed, 0 tests failed out of 1571   (9.72 s)
```

**Name-set checks (G2's name half, G14)**

```
ctest -N names: build-linux 2487, build-push 2487, build-verify 3315
diff <build-linux names> <build-push names>                       -> empty
comm -23 ~/w7/p3a-before-ctest-names.txt <build-linux names>       -> empty   (nothing removed)
comm -13 ...                                                      -> 5 added:
    PipeCatalogue.ResourceRespecifyAcksOnlyImmutableStorage
    PipeCatalogue.VertexBufferSetCarriesAnExplicitBaseInstance
    PipeCatalogue.VertexWireViewsAreFlatAndCarryIsLongSeparately
    ResourceEmit.TheResourceOpTableIsUnregisteredUntilABackendInstallsOne
    VertexInputEmit.AResetApplierCarriesNoVertexInputStateOver
ctest --test-dir build-push -N -R 'VertexInputEmit\.|ResourceEmit\.'   -> Total Tests: 2
```

(The baseline was 2482 names; 2487 = 2482 + 5.)

**G13's two greps**

```
grep -c 'MG_State' MobileGL/MG_Pipe/PipeApply.h            -> 0
grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'  -> empty
```

**Not run here, and why**: G2's execution half, G3/G3b/G4's retrace, G5 (`p3a_untouched_regions.sh` is package
D's and does not exist yet), G6-G12, G15. `c0` touches no `Managers.cpp` region and changes no behaviour, and
those gates belong to the integrator's D.1 merges.

---

## 5. Left undone / handed on

1. **`p3a/wire`'s `c1`, `c2`, `c3`** — the fourteen bodies and `ResourceEmitTest`'s applier cases. Not started;
   this task was `c0` only.
2. **`kMGPipeWiredSubsystems`** must gain `kMGPipeSubsystemResources | kMGPipeSubsystemVertexInput` in the
   commit that gives the emitters bodies (B2). Until then no vertex-input field is skipped by the residual
   fill — which is correct, but it also means the subsystem A/B (G12) cannot show a *fill-loop* difference yet.
3. **`Tracker.h`'s three `MGPipeSubsystemForDirty` arms** (bits 5, 9, 10 → `kMGPipeSubsystemVertexInput`) are
   package B's; the three conditional pairing asserts in `PipeFill.cpp` become equalities the moment they land,
   with no edit needed.
4. **`EmittedCallSuppliesTheWholeField(GetBoundVertexArray)`** — C4's note; B must decide it explicitly.
5. **Doc corrections owed to the integrator**, beyond the ones the brief already lists: D-M's "all four in one
   commit" is not achievable under C.5's ownership split (B1/B2); D-A5's "a flag-word edit changes the
   generated tables" is not true in this tree (C1); C.0's "ten new `MGPipeApplierState` members" is eleven
   (A3); the brief's `VertexInputEmitTest.`/`ResourceEmitTest.` case names must lose the `Test` (D3);
   `check_include_closure.py --compiler clang++-20` does not exist on this host (§4).
6. **Not pushed**, per the task. The tag is local to `/home/swung/w7/p3a-contract`; the other three trees must
   be created from it with `wsl_p3a_tree.sh <slug> p3a/contract`.

Intermediate logs this package created — `~/w7/p3a-contract-{sym,closure,closure2}.log` and
`~/w7/p3a-contract-names-build-*.txt` — were deleted. `~/w7/p3a-contract-tree.log` is kept, as instructed.
Six further files match the same glob and were **left alone because they are not this package's**: the tree
script's own `p3a-contract-{submodule,build-linux-configure,build-linux-build,build-push-configure,
build-push-build,build-verify-configure,build-verify-build}.log`. The integrator should delete them if the
tree-creation transcript is no longer wanted.
