# P5 scout — the wire codec (G3) and the call catalogue's flags

Tree: `/home/swung/w7/pipe` @ `a29807cc` (P4a landed). Read-only survey; nothing was built or run.
All line numbers are from that commit.

---

## 0. Headline (the three things the brief writer needs first)

1. **There is no codec.** G3 emits *no encoder and no decoder* — only record structs, an
   opcode enum, size `static_assert`s and a bounds gate whose every arm is
   `return false`. Nothing outside `MG_Test/Pipe/PipeCatalogueTest.cpp` touches a single
   G3 symbol. P5 writes the codec from zero; the only thing G3 hands it is the opcode
   numbering and the 8-byte header shape.
2. **The monolith never fills a blob.** Every `kHasBlob` record on the wire today declares
   `Blob.Size = 0` ("this record does not declare its blob") and the bytes travel as a
   **companion pointer beside the record** — and for three records that companion is not
   bytes at all but a *typed frontend pointer* (`const SamplerParameters*`,
   `const LinkArtifacts*` + `const SpirvArtifacts*`). A split emission has to invent the
   serialisation for those, and for the two payloads that carry an `MGPBlobRef` but are
   **not flagged `kHasBlob`** (`MGPCaps`, `MGPSamplerDesc`).
3. **34 of the 71 rows have no applier entry point at all** — not "an applier nobody
   calls". Exactly one row matches that description (`UnmapPersistent`, deliberately
   producer-less, `PipeFill.cpp:764-771`); the other 36 wired rows all have live callers.
   So P5's first-exercise set is 34 rows of *brand-new applier surface* (draws, clears,
   blits, present, fences, queries, readback) plus one dormant producer, not 34 rows of
   dormant code.

---

## 1. What `scripts/gen_pipe.py`'s G3 generator emits today

### 1.1 Generator and output

* Generator: `scripts/gen_pipe.py:605` `def gen_wire(calls, residual_fields=None)`.
* Single output: `MobileGL/MG_Pipe/generated/PipeWire.inc` (939 lines), written at
  `scripts/gen_pipe.py:1275-1277`.
* Banner (`scripts/gen_pipe.py:606-607`): *"G3: wire records, size assertions and the
  applier's bounds gate."* That title is exhaustive — those three things are all it emits.

### 1.2 The five things it actually emits

| # | Emitted | gen_pipe.py | PipeWire.inc |
|---|---|---|---|
| 1 | `struct MGPWireRecHeader {Uint16 Op; Uint16 Flags; Uint32 Size;}` + `sizeof == 8` + trivially-copyable asserts | `:628-634` | `:35-41` |
| 2 | `enum class MGPWireOp : Uint16` — `kInvalid = 0`, one enumerator per catalogue row at its 1-based position, `kOpCount = 72` | `:638-644` | `:45-119` |
| 3 | 71 × `struct alignas(8) MGPWireRec_<Name> { MGPWireRecHeader Header; <Payload> Payload; };` each with a composition size assert `sizeof(rec) == ((8 + sizeof(payload) + 7) & ~7)` | `:645-652` | `:121-687` (`GetCaps` at `:121`, `FenceWaitServer` at `:681`) |
| 4 | `[[noreturn]] MGPipeWireProtocolFatal(call, size, remaining)` → `MGLOG_F` + `std::abort()`, and macro `MGP_WIRE_CHECK_BOUNDS(RecType, CallName)` asserting `size >= sizeof(RecType) && size <= remaining && (size % 8) == 0` | `:653-664` | `:689-701` |
| 5 | `inline Bool MGPipeApplyWireRecord(MGPWireOp, const void* record, Uint64 size, Uint64 remaining)` — a 71-arm switch, **every arm does `MGP_WIRE_CHECK_BOUNDS(...); return false;`**, `default`/`kInvalid`/`kOpCount` → fatal | `:666-684` | `:708-933` |

Plus, when `PipeFields.def` gives a `ResidualValueBlock` field list, a tail of
`offsetof` asserts pinning that migration carrier's layout (`gen_pipe.py:686-708`,
`PipeWire.inc:935-941`). Today the block has exactly one member (`CapabilityBits`) so only
the offset-0 and `sizeof == MGL_RESIDUAL_BLOCK_SIZE` asserts survive.

**Encoder: none. Decoder: none.** There is no `Encode<Call>`, no `Decode<Call>`, no tail
walker, no blob packer, no `MGPWireRecHeader` *writer*. `record` is `(void)`-cast at
`gen_pipe.py:673` / `PipeWire.inc:709` — the skeleton never dereferences the payload.
The generator says so itself (`gen_pipe.py:666-671`, verbatim in `PipeWire.inc:702-707`):

> "Returns whether the record was applied. P0 is a **SKELETON**: every case validates its
> bounds and then reports 'not applied', because no applier exists until P5 wires
> `MG_Remote/Server/PipeApplier.cpp` to the real backend tables."

### 1.3 Who includes it — one file, and one test uses it

* `MobileGL/MG_Pipe/MGPipe.h:145` `#include "generated/PipeWire.inc"` (inside
  `namespace MobileGL::MG_Pipe`). That is the **only** includer.
* The only code that references any G3 symbol is
  `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp:404-432` and `:903`
  (`WireOpcodesAreThePositionsInTheCatalogue`, `LateArrivalsAreAppendedWithoutRenumbering`,
  `ApplierAcceptsAWellFormedRecord`). `MG_Remote/` references **zero** G3 symbols;
  `MG_Impl/` and `MG_Backend/` likewise. One prose mention only:
  `MobileGL/MG_Pipe/MGPipeTypes.h:22` ("these structs (generated/PipeWire.inc) are
  memcpy'd").
* Same story one layer out: G1's two tables `gMGPipeScreen` / `gMGPipeContext`
  (`MGPipe.h:137-138`) and G2's `MGP_<Call>` thunks (`generated/PipeThunks.inc`) are
  **also dead** — the only reference is `PipeCatalogueTest.cpp:107,111` reinterpreting them
  as pointer arrays to count entries. The monolith's real dispatch is the direct
  `MGPipeApply*` call and the `MGPipeResourceOps` table (`PipeApply.h:85-98`), not the
  generated tables. *(Guess: P5 may find it cheaper to keep dispatching through
  `MGPipeApply*` on the server side and use the tables only as the emitter's shape, rather
  than to finally install G1/G2 — but that is a design call, not something the tree decides.)*

### 1.4 The chunking contract, quoted

`scripts/gen_pipe.py:618-631` (identical text at `PipeWire.inc:25-33`):

> `// OVERSIZED PAYLOADS ARE CHUNKED, NEVER EMITTED WHOLE (plan section 8.2: G3 has to define`
> `// the path for a record larger than the segment). The bound is the ring's,`
> `// RingProducer::MaxRecordBytes() == Capacity()/2, and it is exact rather than`
> `// conservative: a record has to be placeable at every head offset of an empty ring, the`
> `// wrap pad in front of it costs up to total-8 bytes, and only a record of at most half the`
> `// ring survives that at every offset. An emitter holding more than Capacity()/2 bytes of`
> `// record (a large resource_subdata, a create_shader_state archive) splits it into several`
> `// records of at most that size; the transport refuses a bigger one outright - nullptr plus`
> `// an MGLOG_E - rather than let the producer wait on free bytes that can never suffice.`

Precisely, the contract is:

1. **Bound** = `RingProducer::MaxRecordBytes() == Capacity()/2`
   (`MobileGL/MG_Remote/Transport/Ring.h:195`), measured on **header + payload rounded up
   to 8**, not on the payload alone (`Ring.h:181-191`).
2. **Refusal is not backpressure.** `Reserve` returns `nullptr` *however empty the ring is*
   for an over-size record, with an error log; `Ring.h:186-191` spells out the disambiguation:
   *"nullptr with `FreeBytes() >= total` never means 'wait'; it can only mean 'too big, chunk'."*
3. **Splitting is the emitter's job**, explicitly ("chunking it is the emitter's job (plan
   section 8.2, the G3 chunking rule)", `Ring.h:182-183`). The generator names the two
   expected victims: a large `resource_subdata` and a `create_shader_state` archive.
4. **The chunk protocol itself does not exist.** There is no continuation flag, no chunk
   index, no reassembly buffer, no per-op "this is chunk k of n" anywhere in
   `PipeWire.inc`, `MGPipeTypes.h` or `Ring.h`. `MGPWireRecHeader.Flags` is documented only
   as *"MGPipeCallFlags of the call, for asserts and tracing"* (`PipeWire.inc:37`) — there
   is no reserved chunk bit.
5. **ROADMAP puts "G3 分块路径" (the G3 chunking path) in P8, not P5**
   (`docs/Disaggregated/ROADMAP.md:25`). Two readings, and the brief must pick one
   explicitly: (a) P5 ships the reduced path and simply *refuses* (Fatal) any record above
   `MaxRecordBytes()`, leaving chunking to P8; (b) P5 must chunk `create_shader_state`
   from day one because a real program archive can exceed 4 MiB against a default 8 MiB
   `SEG_CMD`. **Evidence that would settle it:** the `csob-blob` byte counter already
   published in P4a — `INTEGRATOR-DECISIONS.md:803` records DirectGLES median
   **2928 B/frame**, rd12 peak **1.06 MB/frame**. A 1.06 MB *per-frame* aggregate is under a
   4 MiB cap, but that is frame totals, not the largest single `CreateShaderState`
   record; a per-record histogram from a `MOBILEGL_PIPE_STATS` run would settle it.
   `scout-frontend-state.md:922` (P4a scout) already flagged this as risk 8.
6. Same bound is restated in `docs/Disaggregated/ARCHITECTURE.md:125`.

---

## 2. The 71 rows by flag class

Parsed from `MobileGL/MG_Pipe/PipeCalls.def:78-166`. Flag semantics are at
`MobileGL/MG_Pipe/MGPipe.h:39-54`. **Opcode = 1-based position**; verified against
`PipeWire.inc:45-119`.

**Totals:** `kHasBlob` 8, `kVarTail` 9, `kHostSpan` 2, `kReplySlot` 10, `kNeedsAck` 1,
`kOptional` 4. Unique flagged rows **29**; `kNone` **42**; 29 + 42 = 71. ✔

### 2.1 `kHasBlob` — 8 rows

| op | call | payload | class | def line |
|---|---|---|---|---|
| 17 | `CreateRenderState` | `MGPRenderStateDesc` (48) | kCtxCso | `:98` |
| 20 | `CreateVertexElements` | `MGPVertexElements` (40) | kCtxCso | `:101` |
| 27 | `CreateShaderState` | `MGPProgramDesc` (192) | kCtxCso | `:108` |
| 30 | `SetDynamicState` | `MGPDynamicState` (32) | kCtxState | `:112` |
| 40 | `SetGlobalConstants` | `MGPGlobalConstants` (40) | kCtxState | `:122` |
| 46 | `SetResidualValueState` | `MGPResidualValueState` | kCtxState | `:131` |
| 48 | `ResourceSubData` | `MGPSubData` (72) | kCtxObject | `:134` (also `kVarTail`) |
| 49 | `BufferSubDataResident` | `MGPSubData` (72) | kCtxObject | `:135` (also `kOptional`) |

**What the encoder must do beyond memcpy of the POD:** copy `Blob.Size` bytes from the
companion pointer into `SEG_STAGE` (or inline, ≤4 KiB, into `SEG_CMD` —
`ARCHITECTURE.md:409`), then *fill in* `Blob.{Seg, Offset, Size}` — fields the monolith
leaves at `{kMGHostSpanSegNone, <a raw host address>, 0}`. The decoder must resolve
`Seg/Offset` back to a readable pointer and hold the record to its declared length (§4).

**Two payloads carry an `MGPBlobRef` but are NOT flagged `kHasBlob`** — a real trap:

* `MGPCaps` carries **two**: `FormatCapabilities` and `RendererInfo`
  (`MGPipeTypes.h:137-138`), and `GetCaps` is `kReplySlot` only (`PipeCalls.def:80`).
  The header itself defers them to P5: *"Their serializers land with the transport (P5)"*
  (`MGPipeTypes.h:135-136`). P5's charter includes "MGPCaps 快照", so this is directly on
  the critical path: the format-capability cache holds `Vector<Int>` sample-count lists and
  the renderer strings are `String`s — neither is a POD.
* `MGPSamplerDesc::Parameters` (`MGPipeTypes.h:429`), and `CreateSamplerState` is `kNone`
  (`PipeCalls.def:104`). The P4a scout already flagged the asymmetry
  (`~/w7/notes/p4a/scout-pipe-and-client.md:106`). The applier takes it as a **typed**
  pointer, `const SamplerParameters* parameters` (`PipeApply.h:1000`), not bytes.
* Contrast: `ResourceRespecify` deliberately does **not** carry `kHasBlob` and that is
  argued — `MGPResourceDesc` has no `MGPBlobRef` member, so the flag would be a lie
  (`PipeApply.h:76-79`). So `kHasBlob` means "owns an `MGPBlobRef` member", which is
  exactly the invariant the two rows above break.
* And `kHasBlob` is **singular while the payload may hold seven**: `MGPProgramDesc` has
  `MGPBlobRef Spirv[6]` + `MGPBlobRef Reflection` (`MGPipeTypes.h:507-508`). One flag, seven
  independent byte runs on one record.

### 2.2 `kVarTail` — 9 rows

| op | call | payload | tail layout (source) |
|---|---|---|---|
| 32 | `SetVertexBuffers` | `MGPVertexBuffers` | `MGPVertexBuffer[]` (`PipeApply.h:941`) |
| 35 | `SetSamplerViews` | `MGPSamplerViews` | `MGPBoundView[]` (`PipeApply.h:1028`) |
| 36 | `BindSamplerStates` | `MGPSamplerStates` | `MGPipeHandle[]` (`PipeApply.h:1029`) |
| 37 | `SetShaderImages` | `MGPShaderImages` | `MGPImageView[]` (`PipeApply.h:1030`) |
| 38 | `SetShaderBuffers` | `MGPShaderBuffers` | `MGPBufferRange[Count]` **then** `MGHostSpan[HostSpanCount]` (`MGPipeTypes.h:820-822`) |
| 39 | `SetStreamOutputTargets` | `MGPStreamOutputTargets` | `MGPBufferRange[Count]` then `Uint32 offsets[Count]` (`MGPipeTypes.h:836`) |
| 41 | `SetVertexAttribDefaults` | `MGPVertexAttribDefaults` | `MGPAttribValue[]` (`PipeApply.h:756`) |
| 48 | `ResourceSubData` | `MGPSubData` | `MGPSubRegion[RegionCount]` (`MGPipeTypes.h` ~`:1005`) |
| 59 | `DrawVbo` | `MGPDrawInfo` (56) | `MGPDrawRange[NumDraws]`, plus an `MGHostSpan` only when `kDrawHasUserIndices` (`MGPipeTypes.h:1240,1219`) |

**What the encoder must do beyond memcpy:** compute the total record size from the header's
own count fields, reserve `header + fixed + tail`, and write **two or three** arrays back to
back (`SetShaderBuffers` and `SetStreamOutputTargets` each have *two* tails; `DrawVbo` has a
conditional second tail). The decoder must recompute the same arithmetic and check it
against `MGPWireRecHeader.Size` — note `MGP_WIRE_CHECK_BOUNDS` only proves
`size >= sizeof(MGPWireRec_X)`, i.e. **it does not see the tail at all**; a `kVarTail`
record that declares `Count = 4000` and carries 8 bytes passes today's gate. Every tail
bound today comes from a hand-written check inside `PipeApply.cpp` (e.g.
`kMGPipeMaxVertexAttribs` at `PipeApply.cpp:1993-1997`).

### 2.3 `kHostSpan` — 2 rows

`SetShaderBuffers` (op 38) and `DrawVbo` (op 59). Both are also `kVarTail`; the host span
*is* a tail element, never inline. See §3.

### 2.4 `kReplySlot` — 10 rows

op 1 `GetCaps`, 5 `MapPersistent`, 8 `FenceStatus`, 9 `FenceWait`, 14 `QueryAvailable`,
15 `QueryResult`, 52 `ResourceReadback`, 55 `GetTextureImage`, 58 `ReadPixels`,
69 `QueryTimestamp` (`PipeCalls.def:80,84,87,88,94,95,138,141,145,160`).

**What the encoder must do beyond memcpy:** mint a reply-slot id (`MGPReplySlot{Uint64 Id}`,
`MGPipeTypes.h:79-81`), record the pending answer client-side, and arrange for the server to
write back into `SEG_REPLY` (8 MiB, 4 KiB slots — `ARCHITECTURE.md:411`) and post an
`EvReadbackDone`/`EvQueryResult`/`EvFenceSignaled` on `SEG_EVENT`. The catalogue's stated
rule is that **none of these block** — *"Every server query in this catalogue is
async-with-handle; none of them blocks (section 4.4.6, 'the total rule')"*
(`MGPipeTypes.h:77-78`). **P5's charter contradicts that for one row**: ROADMAP P5 says
"阻塞 `read_pixels`" (blocking read_pixels, `ROADMAP.md:21`), and `SEG_REPLY` is P9
(`ROADMAP.md:29`). So in P5 `ReadPixels` is a synchronous round trip that does not use its
reply slot the way the type comment describes. Flag that as an explicit deviation to write
down, not an accident.
Note also that 7 of these 10 rows have **no applier at all** today (§5) — only
`MapPersistent` (`PipeApply.h:917`), `ResourceReadback` (`:908`) and nothing else from this
list exists.

### 2.5 `kNeedsAck` — 1 row

Only `ResourceRespecify` (op 3, `PipeCalls.def:82`). It is a **per-record** predicate, not
per-call: `MGPipeResourceRespecifyNeedsAck(desc)` (`MGPipeTypes.h:680`, per P3a notes it is
`== (desc.Immutable != 0)`), so the same call carries every `glBufferData` without acking
one and only `glBufferStorage` blocks (`PipeCalls.def:20-24`). **What the encoder must do:**
publish, ring the doorbell unconditionally (`ARCHITECTURE.md:464` lists "any `kNeedsAck`
request" as an explicit doorbell point), then spin `MOBILEGL_IPC_SPIN_US` → set
`producerParked` → block, and the decoder side must post the ack watermark.

### 2.6 `kOptional` — 4 rows

op 5 `MapPersistent`, 6 `UnmapPersistent`, 49 `BufferSubDataResident`, 68 `SetSwapInterval`
(`PipeCalls.def:84,85,135,155`). Semantics (`MGPipe.h:51-53`): a **null table entry is a
real answer**, not an error — DirectVulkan deliberately leaves `buffer_subdata_resident`
unregistered. **What the encoder must do:** the null check cannot live on the server. If the
client emits a record for a call the remote backend does not implement, the bytes are spent
and the frontend has already taken the "it happened" branch. The client therefore needs the
remote's *per-call implemented mask* before it emits — which is what `MGPCaps::CallMask`
(`MGPipeTypes.h:133`, `MGPCapBit`) is for. P4a learned this the hard way as the "consumer
gate" (`ROADMAP.md:23`: four families had to check that *some* backend registered
`MGPipeResourceOps`, because Magma had no P4a twin and the client cleared dirty flags on
acceptance, costing 66 DirectVulkan cases their uploads). P5 must wire `CallMask` into the
emitter before the first `kOptional` record goes out.

### 2.7 The flags are not available at runtime

`MGPWireRecHeader.Flags` is documented as carrying "MGPipeCallFlags of the call"
(`PipeWire.inc:37`) — but **no generator emits a per-opcode flags table**. Grep for
`kHasBlob|kVarTail|kHostSpan|kReplySlot|kNeedsAck|kOptional` across
`MobileGL/MG_Pipe/generated/` returns nothing. `PipeTables.inc` is function pointers only.
So a P5 decoder that wants to ask "does opcode 48 carry a blob?" has nowhere to ask, and an
encoder that wants to stamp `Flags` has to hard-code it. **This is a one-line addition to
`gen_pipe.py` (a `kMGPipeCallFlags[kOpCount]` array out of `gen_tables` or `gen_wire`) and
the brief should call for it explicitly**, because otherwise each of the ~6 emitter
packages will hard-code its own copy.

---

## 3. `MGHostSpan` / `MGPHostSpan`

**The name in the tree is `MGHostSpan`, not `MGPHostSpan`** — the header is
`MobileGL/MG_Pipe/MGPipeHostSpan.h` but the struct at `:28` is `MGHostSpan` (no P).
`MGPHostSpan` does not exist; the brief should use `MGHostSpan`.

### 3.1 What it is

```
struct MGHostSpan {           // MGPipeHostSpan.h:28-37, 32 bytes
    const void* Ptr;          // monolith: the frontend shadow or the app's own memory
    Uint32 Seg;               // split: a SEG_STAGE id, or a sentinel
    Uint32 Pad0;
    Uint64 Size;
    Uint64 Offset;
};
```

* `MGPipeHostSpan.h:12-13`: *"The ONE thing in MGPipe whose shape changes with the
  transport."* It is the only place in the whole interface where a pointer is allowed
  (`MGPipeTypes.h:17-19`), and even then only in a **variable tail**, never inline in a
  fixed payload — so the VBO path (every Minecraft/Sodium draw) never pays its 32 bytes
  (`MGPipeTypes.h:1226-1227`).
* Field order is chosen so the struct is 32 bytes on both 64-bit and 32-bit hosts
  (`MGPipeHostSpan.h:29-31`) — **do not reorder**.
* Two sentinels (`MGPipeHostSpan.h:21,26`):
  `kMGHostSpanSegNone = 0` and `kMGHostSpanSegFromServerIndexMirror = 0xFFFFFFFF`
  ("the bytes are already on your side, read them out of the index host mirror").
* Resolution is one predictable branch, `MGPipeHostBytes()` (`MGPipeHostSpan.h:50-56`):
  `Ptr != nullptr` → `Ptr + Offset`; else call `gMGPipeSegmentResolver(Seg, Offset, Size)`;
  else `nullptr`.
* `gMGPipeSegmentResolver` (`MGPipeHostSpan.h:46-47`) is a plain function-pointer global,
  `nullptr` today, **to be installed by `MG_Remote`** (`:42-45`).
  `PipeCatalogueTest.cpp:702` currently asserts it is null.

### 3.2 Which calls carry one

Exactly two, both `kHostSpan|kVarTail`:

* **`SetShaderBuffers`** (op 38). `MGHostSpan[HostSpanCount]` is an **optional second
  var-tail** behind `MGPBufferRange[Count]`. `HostSpanCount` is `0`, or `Count` for the
  `Uniform` class under `kCapNeedsHostUboBytes` — *"a range with nothing to ship carries an
  empty span, so the two arrays stay index-aligned"* (`MGPipeTypes.h:820-823`,
  `:806-811`, cap bit at `:122`). Rationale for not inlining it: it would have cost every
  SSBO / atomic-counter / XFB range 32 dead bytes, and D-B8 says not to freeze the shape
  before the `stage-ubo-named` counter has numbers (`MGPipeTypes.h:806-811`).
* **`DrawVbo`** (op 59). One `MGHostSpan` for the user index bytes, present only when
  `Flags & kDrawHasUserIndices` (`MGPipeTypes.h:1211,1219`).

### 3.3 What a split build has to do with it

Nothing today constructs an `MGHostSpan` outside tests. Grepping the type (excluding the
`kMGHostSpanSeg*` constants) finds only `MGPipeHostSpan.h` itself and
`PipeCatalogueTest.cpp:626,627,693,699`. The `kMGHostSpanSegNone` constant is used widely —
but as the `Seg` value of an **`MGPBlobRef`**, a *different* 24-byte struct
(`MGPipeTypes.h:55-61`): e.g. `ResourceTracker.h:215`, `TextureEmit.h:1265`,
`ProgramEmit.h:249,253`, `VertexInputEmit.h:396`, `SamplerEmit.h:433`,
`Managers.cpp:2138-2139`. **Do not let the brief conflate the two structs.**

For P5 specifically:

* Both `kHostSpan` rows (`SetShaderBuffers`, `DrawVbo`) are in the **34 rows with no
  applier and no emitter** (§5). So P5 is not "filling in the split branch of an existing
  host span" — it is writing the first producer of one, on the reduced path
  (`ClearThenReadPixels`, `Triangle`, an OpenRA trace).
* `ARCHITECTURE.md:122` states the split fill: `Ptr == nullptr`, bytes in
  `SEG_STAGE` named by `Seg/Offset`, or `Seg == kMGHostSpanSegFromServerIndexMirror`.
* **`HostResolve` is not P5.** `MG_Impl/Pipe/HostResolve.cpp` (client array ranges, max-index
  scan, `*IndirectCount` resolution, each with a per-site reconcile) and the `MGHostSpan`
  split fill and `Server/IndexHostMirror` are **all P8** (`ROADMAP.md:25`; the directory
  listing at `ARCHITECTURE.md:569` marks `HostResolve` as `[P2+]` but no such file exists —
  `ls MobileGL/MG_Impl/Pipe/` shows 15 files and `HostResolve.*` is not among them).
  `ARCHITECTURE.md:398-404` is the index-host-mirror section, also labelled P8.
* Practical consequence for P5: the reduced path must either not emit a `kDrawHasUserIndices`
  draw at all, or install `gMGPipeSegmentResolver` and stage the index bytes into
  `SEG_STAGE` by hand. A `Triangle` test with a VBO-backed draw avoids it entirely.
  `kCapNeedsHostUboBytes` and `kCapNeedsHostIndexBytes` (`MGPipeTypes.h:119,122`) are the
  two cap bits that arm the host-span path; leaving both clear in P5's `MGPCaps` snapshot is
  the cheapest way to keep `MGHostSpan` out of the first IPC frame. *(That is my reading of
  the cap bits' purpose; it is not stated as P5 guidance anywhere — guess.)*

---

## 4. The blob rule, and what a split emission has to change

### 4.1 The rule, stated twice in the header

`MobileGL/MG_Pipe/MGPipeTypes.h:398-410` (on `MGPVertexElements`) and again at `:985` (on
`MGPSubData`), restated in the applier at `PipeApply.cpp:1982-1989`:

> `// THE BLOB RULE - ONE RULE FOR EVERY RECORD IN THIS HEADER THAT CARRIES AN MGPBlobRef,`
> `// and MGPSubData below is the other one. `Blob.Size` is the record's own statement of how`
> `// many bytes its blob holds, and the applier holds the record to that statement WHENEVER`
> `// THE RECORD MAKES IT: a non-zero Blob.Size that disagrees with the byte length the`
> `// record's other fields describe is Fatal{ProtocolCorruption} and the call is refused. A`
> `// ZERO Blob.Size means "this record does not declare its blob", which is what a monolith`
> `// emission is - the bytes travel beside the record through the apply entry point's`
> `// companion `const void*` and no MGPBlobRef is filled - and it is not a fault.`

And the load-bearing half (`MGPipeTypes.h:407-410`): *"Either way the bytes read are bounded
by the record's OTHER fields (the two counts here, the destination range there), so the
length is a cross-check and never the safety property."*

`~/w7/notes/p4a/BRIEF-P4A.md:967` calls it "the one Blob rule (ID-8b)";
`p4a-results/clientsp-v1.md:144` confirms the P4a landing: *"All seven `MGPBlobRef`s declare
`Seg = kMGHostSpanSegNone, Size = 0` (the one Blob rule)."*

### 4.2 What a monolith emission puts in `MGPBlobRef` today

For every blob-carrying record: `Seg = kMGHostSpanSegNone (0)`, `Size = 0`,
`Offset = a raw host address` (the client's own shadow base), and the bytes are handed to
the applier as a **separate argument**. Concretely:

| record | `MGPBlobRef` filled as | the bytes arrive as | citation |
|---|---|---|---|
| `ResourceSubData` (texture) | `Seg = None`, `Offset = address of level shadow base`, `Size = 0` | `const void* bytes` | `TextureEmit.h:1260-1265`; `BRIEF-P4A.md:560` |
| `ResourceSubData` (buffer) | same shape | `const void* bytes` | `ResourceTracker.h:203,215` |
| `CreateVertexElements` | `Blob.Seg = None`, `Size = 0` | `const void* blobBytes` | `VertexInputEmit.h:396`; `PipeApply.h:932` |
| `CreateRenderState` / `SetDynamicState` | `Size = 0` | `const void* chunkBytes` | `PipeApply.h:740,748` |
| `SetGlobalConstants` | `Size = 0` | `const void* bytes` = `MapUBO()`'s image, `GetUBOSize()` long | `ProgramEmit.h:192`; `PipeApply.h:1047-1052` |
| `CreateSamplerState` | `Parameters.Seg = None`, `Size = 0` | **`const SamplerParameters*`** — a typed frontend struct, not bytes | `SamplerEmit.h:433`; `PipeApply.h:1000` |
| `CreateShaderState` | all **seven** (`Spirv[6]` + `Reflection`) `Size = 0` | **`const LinkArtifacts*` + `const SpirvArtifacts*`** — two typed frontend objects | `ProgramEmit.h:249,253`; `PipeApply.h:1032-1041` |

`MGPipeApplyCreateShaderState`'s own comment (`PipeApply.h:1032-1039`) is the cleanest
statement of the situation, and it names P5 by name:

> "THE ARTEFACTS TRAVEL BESIDE THE RECORD, by pointer: all seven of `desc.Spirv[]` and
> `desc.Reflection` are declared with Size 0 … **and the codec is NOT called** — zero
> serialisation cost on the monolith path. … Splitting this record for a transport whose
> ring caps one record at half its capacity is **P5's problem**, not this entry point's."

`MGPipeResourceOps`' header says the same thing generally (`PipeApply.h:73-79`): *"in
monolith a blob needs no `MGPBlobRef` and the pointer is the client's own shadow base, so
the call is zero-copy and behaviour is unchanged. How those bytes cross under a real
transport is that phase's problem and that phase's flag edit."*

### 4.3 What a split emission has to put there

1. **A real `Seg`** — a `SEG_STAGE` id (or a `SEG_CMD` inline id for ≤4 KiB payloads,
   `ARCHITECTURE.md:409`) — instead of `kMGHostSpanSegNone`.
2. **A real `Offset`** — a byte offset *into that segment*, not a host address. The
   monolith's `Offset` is a `reinterpret_cast<Uint64>(pointer)` (`Managers.cpp:2138`) and is
   meaningless across the line.
3. **A real, non-zero `Size`** — which arms the rule's Fatal arm for the first time. Today
   every cross-check in `PipeApply.cpp` is inert by construction (`p4a-results/wire-v1.md:140`:
   *"so the one Blob rule has nothing to cross-check and is inert here by construction"*).
   P5 is where those gates go live, so P5 is also where a stale or off-by-one length in any
   emitter becomes a hard abort. The known per-record cross-checks the server will start
   enforcing: `MGPSubData` buffer half must equal `MGPipeSubDataBufferSize(record)`
   (`wire-v1.md:320`); `MGPVertexElements` must equal
   `AttributeCount*sizeof(MGPVertexAttribWire) + BindingPointCount*sizeof(MGPVertexBindingPointWire)`
   (`PipeApply.cpp:1990-1998`); `MGPGlobalConstants` must equal `Desc.GlobalUboSize`
   (`wire-v1.md:192`); `MGPSamplerDesc` must equal `sizeof(SamplerParameters)`
   (`wire-review-v1.md:277`).
4. **A serialiser for the three typed-pointer companions.** Two of the three already exist,
   one does not:
   * `CreateShaderState`: **`EncodeProgramArtifacts` / `DecodeProgramArtifacts` already
     exist** — `MobileGL/MG_State/GLState/ProgramState/ProgramArtifactsCodec.{h:53,60,cpp:252,264}`,
     with a dedicated test suite `MG_Test/Program/ProgramArtifactsCodecTest.cpp` covering
     truncation, trailing garbage, version and size mismatch. Better still, a verify build
     already runs the round trip over **every real program the verify lane links**:
     `PinProgramArchiveRoundTrip` (`PipeApply.cpp:1056-1087`), called at `:2638`. It also
     pins the one member the codec must never carry: a decode that reconstructed a live
     glslang `TProgram` is a fault (`PipeApply.cpp:1073-1078`). **This is P5's single
     biggest piece of free work — the program archive codec is written, tested and already
     validated against production programs.**
   * `CreateSamplerState`: `SamplerParameters` is a POD value type in
     `MG_Pipe/MGPipeValueTypes.h`, so a `memcpy` of `sizeof(SamplerParameters)` is the
     serialisation, and `borderColorForm` must survive byte-for-byte
     (`MGPipeTypes.h:425-428`). Cheap.
   * `MGPCaps`: **no serialiser exists.** `FormatCapabilities` holds `Vector<Int>`
     sample-count lists and `RendererInfo` holds `String`s (`MGPipeTypes.h:135-138`), and
     the header explicitly defers *"Their serializers land with the transport (P5)"*.
     `MGPCaps` also embeds `DynamicBackendParameters` whose literal size is ABI-dependent
     (`MGPipeTypes.h:143-147` asserts the size as a *composition*, not a literal, and says
     fixed-width members are P1/P7 work). For `inproc` both sides share the ABI so this is
     latent; for P6's `spawn` it is not. **Say so in the brief: `MGPCaps` is the one record
     whose wire form is not yet ABI-stable, and P5 should either pin the ABI or accept that
     `spawn` (P6) inherits a `SegmentMismatch`/`AbiMismatch` risk.**
5. **Seven blob refs on one record.** `CreateShaderState` needs seven independent
   `{Seg, Offset, Size}` fills from one archive, and it is also the record most likely to
   exceed `MaxRecordBytes()` (§1.4).

---

## 5. Rows never emitted by the client in monolith, as of `a29807cc`

### 5.1 Correction to the question's premise

**Exactly one** call has an applier entry point with no production caller —
`UnmapPersistent`, and it is deliberate (below). Everything else splits into "applier +
emitter, both live" (36 rows) or "neither exists" (34 rows). I enumerated all
`MGPipeApply*` symbols in `PipeApply.{h,cpp}` (37 call-shaped entry points plus the helpers
`MGPipeApplier`/`MGPipeApplyAccess`) and counted references for each outside
`MG_Pipe/PipeApply.*`: every one has ≥ 1. Two are worth noting because they have no
`MG_Impl` caller at all in the grep:

* `MGPipeApplyUnmapPersistent` (op 6) — **the one row that genuinely matches the question's
  premise: the applier exists and nothing in production calls it.** Callers are only
  `MG_Test/Pipe/ResourceEmitTest.cpp:462,535,560,926,1648`. This is deliberate and argued at
  `MG_Impl/Pipe/PipeFill.cpp:764-771`:

  > "NO UnmapPersistent PRODUCER IN P3a, AND THAT IS DELIBERATE. The catalogue has the call
  > and wire implemented it, but D-J forbids new behaviour and there is nothing to convert:
  > `BufferBackendOps` has seven hooks and none of them is an unmap, and
  > `PipeResource::ReleasePersistentMap()` (`BufferObject.cpp`, from `RedefineStorage`) tells
  > the backend nothing today — it learns from the `Respecify` that follows. … **The producer
  > lands with the phase that gives the backend an unmap hook.**"

  The backend side is already there (`Managers.cpp:2250` `Ops_H_UnmapPersistent`, registered
  at `:2307`; dispatched at `PipeApply.cpp:1944-1945`). **P5 is plausibly that phase** — its
  charter is client-side block-granularity persistent-map push and
  `PersistentCoherentMapScenario` green — so the brief should decide explicitly whether P5
  lights this producer up or leaves it to P11. *(Guess: P5 needs it, because under split a
  persistent map is a server-side mapping that the client cannot silently let a `Respecify`
  clean up the way a shared address space does.)*
* `MGPipeApplyBufferSubDataResident` — exactly one production caller,
  `MG_Impl/Pipe/PipeFill.cpp:727`.

### 5.2 The 34 rows P5 is the first to exercise — **no applier, no emitter**

`71 − 37 = 34`. These are new surface on both sides.

**Screen (6):**
`GetCaps` (1), `FenceCreate` (7), `FenceStatus` (8), `FenceWait` (9), `FenceDestroy` (10),
`FenceWaitServer` (71).

**Context / query (8):**
`QueryCreate` (11), `QueryBegin` (12), `QueryEnd` (13), `QueryAvailable` (14),
`QueryResult` (15), `QueryDestroy` (16), `QueryTimestamp` (69), `QueryCounter` (70).

**Context / state (3):**
`SetIndirectBuffers` (34), `SetShaderBuffers` (38), `SetStreamOutputTargets` (39).

**Context / object (4):**
`ResourceSubDataComplete` (50), `ResourceCopyRegion` (53), `GenerateMipmap` (54),
`GetTextureImage` (55).

**Context / verb (13):**
`Blit` (56), `Clear` (57), `ReadPixels` (58), `DrawVbo` (59), `LaunchGrid` (60),
`MemoryBarrier` (61), `BeginStreamOutput` (62), `EndStreamOutput` (63),
`PauseStreamOutput` (64), `ResumeStreamOutput` (65), `Flush` (66), `Present` (67),
`SetSwapInterval` (68).

Cross-check: `ls MobileGL/MG_Impl/Pipe/` holds 15 files —
`CompositeResolver.h CsoCache.h FramebufferEmit.h ImageEmit.h PipeFill.{cpp,h}
ProgramEmit.h ResourceTracker.h SamplerEmit.h SetHashSuppressor.h SlotAllocator.{cpp,h}
TextureEmit.h Tracker.h VertexInputEmit.h`. There is no `DrawEmit`, no `CommandEmit`, no
`QueryEmit`, no `FenceEmit`, no `PresentEmit` — consistent with the 34 above.

### 5.3 What P5's exit gates actually require from that set

The P5 gates (`ROADMAP.md:21`) are
`DirectGLES.Split.*(ClearThenReadPixels|Triangle)` green under `inproc`, OpenRA trace split
SSIM ≥ 0.99, `PersistentCoherentMapScenario` green, both roles' peak RSS recorded,
`persistent-map-push` published, unmigrated field read = `Fatal{UnmigratedPipeInput}`.

Minimum new rows those gates force (my reading of the gate names, not stated anywhere —
**guess**, but a well-supported one):

* `ClearThenReadPixels` → `Clear` (57) + `ReadPixels` (58) + `Present` (67) + `GetCaps` (1).
* `Triangle` → additionally `DrawVbo` (59), `SetShaderBuffers` (38) if the shader has a UBO,
  `Flush` (66).
* OpenRA trace → whatever that trace uses; almost certainly `Blit` (56) and possibly
  `GenerateMipmap` (54).
* `PersistentCoherentMapScenario` → `MapPersistent`/`UnmapPersistent` (already have
  appliers) plus the new client-side block-granularity push.

That is roughly **6–9 of the 34** for the reduced path. The remaining ~25 (queries, fences,
XFB, indirect, compute, copy-region, texture writeback) are not on P5's critical path and
the brief should say so explicitly, or the packages will try to build all 34.

---

## 6. Other facts the brief writer will want

* `MG_Remote/` today is **Protocol + Transport only** — 19 files, no `Client/`, no `Server/`:
  `Protocol/{mg_protocol_base.h, protocol.fbs, generated/protocol_generated.h}` and
  `Transport/{Ring,Doorbell,ShmSegment,ShmSegmentPosix,ShmSegmentWin32,FdPassing,
  InProcessTransport,Framing,ITransport,WireLog}`. P5 creates both `Client/` and `Server/`.
  Planned contents at `ARCHITECTURE.md:575` (`PipeEmitter EmitTables BackendObject_Remote
  CapsMirror ShadowArena PersistentMapTracker GpuWritePending Surface/{X11,Win32,Android,Headless}`)
  and `:388-393` (`PipeObjectTables`, `PipeApplier`).
* `InProcessTransport` is explicitly *not* a test double and its header already warns what it
  does **not** cover: *"What it does NOT exercise is serialization of the byte stream, so the
  framing codec is covered separately by FramingTest"*
  (`InProcessTransport.h:12-16`). P5's charter says inproc must run **the same G3 codec as
  spawn** — so P5 has to change that property: inproc must stop queueing messages whole and
  start going through the ring + codec. Reading (b) is that only the `SEG_CMD`/`SEG_STAGE`
  ring path goes through the codec while `ITransport` keeps queueing control frames whole;
  `ARCHITECTURE.md:441` (*"热路径完全绕过它"* — the hot path bypasses `ITransport` entirely)
  supports (b). I read (b) as correct.
* Build gating: `MOBILEGL_BUILD_DISAGGREGATED` is `OFF` by default
  (`CMakeLists.txt:23`); with it off, *nothing* under `MG_Remote/` compiles and
  `nm --defined-only libMobileGL.so | grep -i MG_Remote` must be empty
  (`CMakeLists.txt:18-20`). Sources appended at `CMakeLists.txt:503-518`, define added at
  `:572-573`. It also hard-requires `3rdparty/flatbuffers/include` (`CMakeLists.txt:453-464`).
* `MOBILEGL_TRANSPORT` parsing lands in `ConfigLoader.cpp` at P5 (`ARCHITECTURE.md:583`),
  values `monolith | inproc | spawn | unix:<path> | pipe:<name>`. The `Init.cpp` hook is one
  `#if MOBILEGL_BUILD_DISAGGREGATED` branch installing `MG_Remote::BackendObject_Remote`
  when `MG_Config::Transport != Monolith` (`ARCHITECTURE.md:29`, which notes the branch does
  not exist yet).
* ctest wiring traps, already written down (`ARCHITECTURE.md:584`): ctest `ENVIRONMENT` is
  *replace*, not append; `;` must be escaped; the property overrides job env, so
  `mgl_itest_join_environment(... ${MGL_ITEST_COMMON_ENV})` is mandatory;
  `add_trace_replay_test` needs the `SPLIT` suffix (otherwise it collides with the same
  case+backend name) and `-DTRACE_TRANSPORT=` for `run_trace_case.cmake`. This matches
  MEMORY's ctest-ENVIRONMENT-overrides-job-env lesson from the CI integration lane.
* The wire has **no per-record sequence field** — seq is the record ordinal
  (`m_emitSeq++` / `m_applySeq++`), `ARCHITECTURE.md:124`.
* `gen_pipe.py --check` and `--self-test` are CI gates; the generated `.inc` files are
  **committed** and CI diffs them (`gen_pipe.py:25-33`). Any P5 edit to `gen_pipe.py` must
  regenerate and commit, and must keep the self-test's negative controls tripping
  (`gen_pipe.py:1196-1247`).
* `PipeCalls.def:26-29`: **record numbering never churns** — a new call is *appended*, a
  retired call keeps its slot. The opcode is the 1-based position, so reordering is a
  protocol break. P5 must not reorder, and `PipeCatalogueTest.cpp:418-422` pins the last
  four opcodes (68/69/70/71) against exactly that.

---

## 7. Concrete asks for the P5 brief

1. **Add a generated per-opcode flags table** to `gen_pipe.py` (§2.7) before any emitter
   package starts, or six packages will each hard-code their own.
2. **Fix or document the two unflagged blob carriers** — `GetCaps`/`MGPCaps` and
   `CreateSamplerState`/`MGPSamplerDesc` (§2.1). `PipeCalls.def` flag edits are cheap and do
   not move opcodes; the flag is what a generated encoder would key on.
3. **Decide the chunking question explicitly** (§1.4 item 5) — P5 refuses over-size records,
   or P5 chunks `CreateShaderState`. A per-record `csob-blob` histogram settles it.
4. **Name `EncodeProgramArtifacts`/`DecodeProgramArtifacts` in the brief** (§4.3) so no
   package writes a second program-archive codec.
5. **Write down the `ReadPixels` deviation** — the payload comment says nothing in this
   catalogue blocks, P5's gate says blocking `read_pixels` (§2.4).
6. **Scope the 34 rows** (§5.3): name the ~6–9 the gates actually force, and say the other
   ~25 are out of scope for P5.
7. **Pin or accept the `MGPCaps` ABI hole** (§4.3 item 4) before P6's `spawn`.
