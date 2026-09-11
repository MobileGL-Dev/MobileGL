# CONTRACT-P5 — the four tables every P5 package is held to

Authority: this file. `~/w7/notes/p5/BRIEF-P5.md` states the rulings R-1…R-14; this file is
where they become implementable, and where the rulings the brief left to the contract package
are made. Where the two disagree, this file is newer and this file wins — and §6 lists every
place they disagree, so nobody has to diff them.

**How to change it.** `MG_Remote/CONTRACT-P5.md` is c0's file. A package that needs a row
changed goes through the integrator, who edits here first and only then lets the package land.
P4a's contract was corrected seven times and each correction cost a package a rework round;
the point of this file existing at all is that a correction is a one-line diff here rather than
a rediscovery in six places.

It lives beside the code, not in `~/w7/notes/`, because it describes bytes on a wire and has to
move with the branch that defines them.

Base: `feat/disaggregated @ a29807cc`. Every `file:line` below was read at that commit.

---

## §0 The three rules that sit above every table

These are R-2 and R-11 in their formal wording. They apply to every row of table 1 without
restatement, and every reviewer's first three questions (BRIEF §11.1, §11.4) are these three.

**Rule A — a content record must declare its bytes.** Under
`MG_Config::Transport != Monolith`, a record whose payload owns an `MGPBlobRef` and which
carries content must set `Seg` to a real segment, `Offset` to a byte offset **within that
segment**, and `Size` to a **non-zero** byte count. `Blob.Size == 0` on such a record is
`Fatal{ProtocolCorruption}`.

This inverts today's legal state, and deliberately. `MGPipeTypes.h:398-410` says in so many
words that a zero `Blob.Size` means *"this record does not declare its blob"* and *"is not a
fault"* — which is right for monolith, where the bytes travel beside the record as a companion
pointer. Under split there is no beside.

**Rule B — no host pointer crosses.** Under split, `MGHostSpan::Ptr` is written `nullptr` by
the encoder and any non-null `Ptr` seen by the apply side is `Fatal{ProtocolCorruption}`.
Today `Ptr` is the fast path (`MGPipeHostSpan.h:51`), which is exactly why this needs saying.
P5's reduced path should produce **zero** host spans at all — see table 0's cap-bit row.

**Rule C — an applier entry point may not hold a pointer past its return.** A `SEG_STAGE` run
is valid from publish until `retiredSeq` passes the record that named it. The tree has exactly
one violation and it is named rather than tolerated: Espryt's `GLESBufferResource::hostBytes`
(`Managers.h:839`), written by `Ops_H_SubData` (`Managers.cpp:1980-1983`) and `Ops_H_FlushRange`
(`:2035`), read by six later drains (`:2000`, `:2062`, `:2080`, `:2111`, `:2741`, `:2843`).
Under split those two writes must **copy into server-owned storage**. `MOBILEGL_IPC_AUDIT=1`'s
`0xDD` fill over retired staging bytes (R-2.5) is the mechanical control that says whether they
did; without it, an `inproc` implementation that kept the pointer is indistinguishable from one
that copied.

---

## §1 Table 0 — the encoding table

One row per wire field that is **not a handle**. Handles are `{slot, gen}` and are settled by
P2/P3a; everything else that is not a plain scalar is here.

| field | the ruling | zero means | who reads it | evidence / note |
|---|---|---|---|---|
| **segment id space** | `SEG_CMD=1, SEG_STAGE=2, SEG_REPLY=3, SEG_EVENT=4, SEG_SHADOW=5, SEG_ADOPT=6`. Identical values to `Protocol::SegmentKind`. | **"no segment", always.** 0 is never a real segment id. | encoder, decoder, `gMGPipeSegmentResolver` | `protocol.fbs:36-44`; `kMGHostSpanSegNone = 0` at `MGPipeHostSpan.h:21`. The two are tied by `static_assert` in `Wire/PipeWireCodec.cpp`, which is the only place the flatbuffers header and the codec enum meet. `kMGHostSpanSegFromServerIndexMirror = 0xFFFFFFFF` (`:26`) stays reserved for P8. |
| **`MGPBlobRef{Offset, Size, Seg, Pad0}`** (24 B, `MGPipeTypes.h:55-61`) | `Seg` from the row above. `Offset` is a **byte offset inside that segment**, never a host address. `Size != 0` ⇔ "this record declares its blob", and under split a content record **must** declare it (rule A). | `Size == 0` = "no blob declared" — legal in monolith, `Fatal` under split for a content record. | decoder, every bounds cross-check | Today every emitter writes `{Seg=None, Offset=<host address>, Size=0 or real}`. Five of the eleven blob members do declare a real size today (`CsoCache.h:156`, `VertexInputEmit.h:398`, `ResourceTracker.h:216`, `PipeFill.cpp:2259`); the rest write 0. See table 1's "length" column for which. |
| **`MGHostSpan{Ptr, Seg, Pad0, Size, Offset}`** (32 B, `MGPipeHostSpan.h:28-37`) | Split: `Ptr == nullptr` always (rule B). **The 32-byte layout must not be reordered** (`:29-31`). | `Seg == 0` with `Size != 0` is `Fatal`. | decoder | **P5's reduced path must produce none at all.** See the cap-bit row below. |
| **`kCapNeedsHostIndexBytes` (1<<7), `kCapNeedsHostUboBytes` (1<<8)** | **Both are 0 for the whole of P5**, by ruling. | — | client emitters | `MGPipeTypes.h:120-123`. This is the cheapest way to keep every `MGHostSpan` out of the first IPC frame: the two bits are the only things that ask for one. `TriangleScenario` must therefore use a **VBO-backed draw and not client-array indices**, or `kDrawHasUserIndices` (`MGPipeTypes.h:1212`) produces a span the split filling for which is P8's. |
| **`MGPCaps`** (4 members, `MGPipeTypes.h:126-139`) | **One carrier, not two.** `MGPCaps` is the model; `protocol.fbs`'s `CapsSnapshot` is its transport form. `Dynamic` and `CallMask` cross as POD bytes; `FormatCapabilities` and `RendererInfo` cross as the two blobs whose serializers are P5's new work (`MG_Remote/CapsCodec.h`). | — | `CapsMirror` (client), `ServerSession` (server) | `MGPCaps` has only a **compositional** size assertion (`MGPipeTypes.h:145-146`) because `DynamicBackendParameters` still carries `SizeT` and `GLenum`; P0.5's fixed-width rewrite never happened. **P5 does not rewrite it** — see the ABI row. |
| **`CapsSnapshot` redundancy** | `tableSlotMask` (`protocol.fbs:94`) is **DELETED**, not renamed. `maxComputeWorkGroupCount` / `maxComputeWorkGroupSize` (`:92-93`) and `prefersCpuXfbPrimitiveAccounting` (`:95`) are **deleted** too: the first two ride inside `Dynamic` already (`BackendObject.h:392-393`), and the third is answered by `kCapCpuXfbPrimitiveAccounting`. | — | s1 (the schema), c1 (the mirror) | R-8 offered rename-or-delete for `tableSlotMask`; **delete**, for two reasons and the second is decisive. `ARCHITECTURE.md:114` says `CallMask` *replaces* "is this table slot null" as the capability probe, so a field whose comment is "which `GLFunctionsTable` slots the peer registered" re-introduces precisely what it replaced. And `GLFunctionsTable` has **69** function-pointer slots (`BackendObject.h:117-292`), so a `ulong` mask cannot address it and never could — it is five bits short on day one. |
| **`MGPCaps::CallMask` layout** | bits **0..8** = `MGPCapBit`, unchanged. bits **9..31** reserved. bits **32..47** = the **consumer mask**: bit `32+n` means "the server has a consumer for MGPipe subsystem bit `n`". bits **48..63** reserved. | a clear consumer bit = "this server does not consume that family; emit nothing for it". | `CapsMirror::ServerConsumes` — the **only** legal client-side source | c0's ruling, and the thing that makes R-8 implementable at all: R-8 says the client's liveness gates must read the `CallMask` mirror, but `CallMask` as declared has only nine feature bits and no per-family bit. Constants and the two fold/test helpers are in `MG_Remote/CapsCodec.h`; `CapsCodec.cpp` asserts the block does not collide with `MGPCapBit` and that P4a's `0x1fff` fits sixteen bits. |
| **ABI agreement** | `Hello`/`Welcome` assert both peers agree on `sizeof(DynamicBackendParameters)`, `sizeof(MGPCaps)`, `sizeof(GLFunctionsTable)` and `buildFingerprint`. A mismatch is `Fatal{AbiMismatch}` and **never** a downgrade. | — | s1 | The compositional assertion above means the caps block's literal size is ABI-dependent. P6's spawn is same-machine, same-binary and inherits this unchanged. A fixed-width rewrite of `DynamicBackendParameters` is **P7's** account, not P5's. |
| **`MGPSubData::Target`** | Packed: **low byte = `MGPipeResourceTarget`, high byte = the cube-face upload target**. Read only through `MGPipeSubDataResourceTargetOf` / `MGPipeSubDataUploadTargetOf`. Whole field `== 0` is the **buffer** half; a low byte naming `Buffer`, `Renderbuffer` or `>= MGPipeResourceTarget::Count` is `Fatal{ProtocolCorruption}`. | whole field 0 = buffer upload | applier | P4a ID-12. Already settled; copied here because a decoder that open-codes it is the class-1 defect. |
| **`MGPImageView::Access`** | The three-value encoding at `ImageEmit.h:146-159`, verbatim. | — | decoder, applier | **This encoding has only ever lived in a package header** (P4a R-3). This table is its first wire reader, so it is now contract. |
| **`MGPSamplerView::Target`** | The encoding at `SamplerEmit.h:900`, verbatim. | — | decoder, applier | Same status as the row above. |
| **`MGPFramebufferState::DrawBuffers[8]`** | The `-1` / default-token narrowing at `FramebufferEmit.h:146-163`, verbatim. A draw-buffer entry naming a colour attachment outside the record's own array is `Fatal{ProtocolCorruption}` (the applier already says so). | `-1` = "no attachment" | decoder, applier | Same status. |
| **`MGPReplySlot::Id`** | **= the record's sequence number** (R-3). No new id space, no allocator. The server writes the answer into `SEG_REPLY[seq % slots]` and **stamps `seq` back into the slot header** so a wrong-slot read is detectable rather than plausible. | seq is 1-based; `0` = "no record / not encoded" | client barrier wait | `ARCHITECTURE.md:124`: the wire carries no per-record seq field, so seq *is* the ordinal. `MGPReplySlot` exists (`MGPipeTypes.h:78-81`) and **no payload of the ten `kReplySlot` calls contains one** — which is exactly why the id must be derived rather than carried. P9 generalises this to "seq is the id's initial value", which extends the rule rather than overturning it. |
| **reply slot header** | `{Uint64 Seq; Int32 Status; Uint32 Size;}` — 16 bytes, then the payload. `Status`: **0 = OK, 1 = DECLINED, 2 = ERROR**. | — | client | **`DECLINED` is a real answer, not a failure.** It is how `MapPersistent` says `nullptr` (R-6) and how the four `Bool` acceptance entry points say `false` (R-5). A client that treats DECLINED as an error re-creates ID-39's 66 lost uploads from the other side. |
| **`kRecPad` and seq** | A wrap filler **does not advance seq**, on either side. | — | both | R-9. `RingConsumer::Pop` already skips fillers; the rule is stated because the *counter* is the caller's, not `Pop`'s. A side that counts pads drifts by one per wrap, for ever — and since seq is the reply-slot id, a drifted seq reads another call's answer instead of failing. Pinned by `RingTest.AWrapFillerDoesNotAdvanceTheRecordSequence`. |
| **per-opcode flags** | `kMGPipeCallFlags[MGPWireOp::kOpCount]` in `generated/PipeWire.inc`, read only through `MGPipeCallFlagsFor(op)`. Index 0 (`kInvalid`) is `kNone`. | `kNone` = no flags | every package | R-13.4. Before this table existed nothing generated exported the flags, so six packages were each about to hard-code `PipeCalls.def`'s fourth column — which is how `GetCaps` and `CreateSamplerState` came to own an `MGPBlobRef` with no `kHasBlob` on their line. `gen_pipe.py` now also refuses a flag token that is not an `MGPipeCallFlags` enumerator, with two negative controls in `--self-test`. |
| **`kHasBlob`'s meaning** | **Exactly "the payload owns an `MGPBlobRef` member"** — nothing weaker. | — | decoder | `PipeApply.h:78-79` already says so. Three calls carry bytes with **no** `MGPBlobRef`; they are table 1 rows 19–21 and are deliberately unflagged, because a decoder that trusts `kHasBlob` has to find a member to read. |

---

## §2 Table 1 — the byte carriers

**23 rows, not 19.** BRIEF §3 lists 19 and `scout-premortem:§3` lists a different 19; the
union is 23 and the four that only the premortem lists — `MapPersistent`, `ResourceReadback`,
`ReadPixels`, `GetTextureImage` — are precisely the ones whose bytes travel **server → client**.
Leaving them out of the byte-carrier table is how a phase discovers in week three that it never
decided where readback pixels land. They are rows 20–23 and are marked with their owning phase.

Columns: **flags** · **blob member** · **companion pointer today** · **which segment the bytes
live in** · **who owns that memory** · **when the slot retires** · **who declares the length,
who cross-checks it** · **reply name** (for `kReplySlot` rows).

`apply` = retires when `DecodeAndApply` returns. `submit` = when the server has handed the bytes
to the driver. `gpu` = `completedFrameSerial`.

### Group A — `kHasBlob`, the payload owns an `MGPBlobRef`

| # | call (op) | flags | blob member | companion today | segment | owner | retires | length declared / cross-checked |
|---|---|---|---|---|---|---|---|---|
| 1 | `CreateRenderState` (17) | `kHasBlob` | `Blob` (`MGPipeTypes.h:364`) | `const void* chunkBytes` (`PipeApply.h:740`), passed `CsoCache.h:157` | `SEG_STAGE` | client stages, server copies on apply | **apply** | declared real (`CsoCache.h:156`, `= kMGPipePipelineChunkBytes`); **nothing reads it today** — `MGPipeApplyCreateRenderState` (`PipeApply.cpp:1280`) never touches `Blob.Size`. Decoder must cross-check against `ChunkMask`. |
| 2 | `CreateVertexElements` (20) | `kHasBlob` | `Blob` (`:419`) | `const void* blobBytes` (`PipeApply.h:932`), passed `VertexInputEmit.h:399` | `SEG_STAGE` | client stages, server copies | **apply** | declared real (`VertexInputEmit.h:398`); **cross-checked, and this is the model for every other row**: `PipeApply.cpp:1990-1999` recomputes `AttributeCount*sizeof(MGPVertexAttribWire) + BindingPointCount*sizeof(MGPVertexBindingPointWire)` and refuses a disagreement; both counts bounded by `kMGPipeMaxVertexAttribs`. |
| 3 | `CreateShaderState` (27) | `kHasBlob` | **seven**: `Spirv[6]` (`:507`) + `Reflection` (`:508`) | **two typed frontend pointers** — `const LinkArtifacts*` + `const SpirvArtifacts*` (`PipeApply.h:1039-1041`), passed `ProgramEmit.h:257` | `SEG_STAGE`, seven independent runs | client stages, server copies | **apply** | all seven declare `Size = 0` today and `Reflection.Offset` is literally `(Uint64)&link` (`ProgramEmit.h:254`). **The serializer already exists and no package may write a second one**: `EncodeProgramArtifacts`/`DecodeProgramArtifacts` (`ProgramArtifactsCodec.{h:53,60,cpp:252,264}`), its own suite, and the verify build already round-trips **every real program it links** (`PinProgramArchiveRoundTrip`, `PipeApply.cpp:1056-1087`, called `:2638`). |
| 4 | `SetDynamicState` (30) | `kHasBlob` | `Blob` (`:389`) | `const void* chunkBytes` (`PipeApply.h:748`), passed `PipeFill.cpp:2260` | `SEG_STAGE` | client stages, server copies | **apply** | declared real (`PipeFill.cpp:2259`); **nothing reads it** (`PipeApply.cpp:1363-1370` scatters by `ChunkMask`). Same fix as row 1. |
| 5 | `SetGlobalConstants` (40) | `kHasBlob` | `Blob` (`:848`) | `const void* bytes` = `MapUBO()`'s image (`PipeApply.h:1052`), passed `ProgramEmit.h:195` | `SEG_STAGE` | client stages, server copies | **apply** | declares **0** (`ProgramEmit.h:194`); cross-check exists at `PipeApply.cpp:2784` against `Desc.GlobalUboSize` but is **inert while Size is 0**. Under rule A it becomes live. Per program per frame, unbounded length — the row most worth watching against R-10's max-record counter. |
| 6 | `SetResidualValueState` (46) | `kHasBlob` | `Blob` (`:932`) | **none, and no record either**: the entry point is `MGPipeApplySetResidualValueState(const ResidualValueBlock&)` (`PipeApply.h:760`), passed `PipeFill.cpp:2184`. **`MGPResidualValueState` is never instantiated on the live path.** | `SEG_STAGE` | client | **apply** | nothing declares it. The encoder must invent **both** the record fill and the blob fill. `sizeof(ResidualValueBlock) == MGL_RESIDUAL_BLOCK_SIZE == 8` is statically asserted (`MGPipeTypes.h:924-927`) and only ever ratchets **down**. **This is the hardest row in the table** and neither scout flagged it; see §6. |
| 7 | `ResourceSubData` (48) | `kHasBlob\|kVarTail` | `Blob` (`:1009`) | `const void* bytes` + `const MGPSubRegion* regions` (`PipeApply.h:897-899`); buffer half `PipeFill.cpp:705`, texture half `TextureEmit.h:1278-1280` | `SEG_STAGE` | client stages, **server must copy** (rule C names this call) | **apply** | **the two halves disagree today**: buffer declares real (`ResourceTracker.h:216`), cross-checked at `PipeApply.cpp:702`; texture declares **0** (`TextureEmit.h:1265-1267`) on the grounds that the byte count is *"the server's to compute once it has picked box-or-rects"* — which cannot be a bounds check. **Under rule A the texture half must declare too.** Tail: `MGPSubRegion[RegionCount]` (`:1007`). |
| 8 | `BufferSubDataResident` (49) | `kHasBlob\|kOptional` | `Blob` (`:1009`, same payload) | `const void* bytes` = application staging, *"valid for the duration of the call only"* (`PipeApply.h:90`, `:901`); one caller, `PipeFill.cpp:727` | `SEG_STAGE` | client stages, server copies | **apply** | declared real via `MGPipeBuildSubDataRecord`, cross-checked at `PipeApply.cpp:702`. `kOptional` is a **capability** question under split, not a null-pointer question: the client must gate on `kCapResidentSubData` through the caps mirror, never on a table slot (R-8). |

### Group B — `kVarTail`, a tail and no blob member

Every row here declares its tail by a **count**, and `MGP_WIRE_CHECK_BOUNDS` **cannot see the
tail at all** — it only proves `size >= sizeof(MGPWireRec_X)`, so a record declaring
`Count = 4000` while carrying 8 bytes passes today. The decoder must recompute the total from
the declared count(s) and require it to **equal** `MGPWireRecHeader::Size`.

| # | call (op) | flags | tail element × count | companion today | segment | owner | retires | length |
|---|---|---|---|---|---|---|---|---|
| 9 | `SetVertexBuffers` (32) | `kVarTail` | `MGPVertexBuffer` × `Count` (`:730`) | `const MGPVertexBuffer*` (`PipeApply.h:941`), `VertexInputEmit.h:245` | `SEG_STAGE` (tail follows the payload in `SEG_CMD` only if it fits the record bound) | emitter-owned `Vector`, **reused next emission** | **apply** | `Count`; no tail cross-check today |
| 10 | `SetSamplerViews` (35) | `kVarTail` | `MGPBoundView` × `Count` (`:778`) | `const MGPBoundView*` (`PipeApply.h:1028`), `SamplerEmit.h:786` | as above | as above | **apply** | `Count`; `Start+Count` past the unit bound is already `Fatal` (`PipeApply.h:1024-1026`) — a *slot* bound, not a byte-length check |
| 11 | `BindSamplerStates` (36) | `kVarTail` | `MGPipeHandle` × `Count` (`:785`) | `const MGPipeHandle*` (`PipeApply.h:1029`), `SamplerEmit.h:860` | as above | as above | **apply** | as above |
| 12 | `SetShaderImages` (37) | `kVarTail` | `MGPImageView` × `Count` (`:802`) | `const MGPImageView*` (`PipeApply.h:1030`), `ImageEmit.h:129` | as above | as above | **apply** | as above |
| 13 | `SetShaderBuffers` (38) | `kVarTail\|kHostSpan` | **two tails**: `MGPBufferRange` × `Count` (`:826`), then `MGHostSpan` × `HostSpanCount` (`:828`) | **none — no applier entry point exists.** P5 writes the first producer *and* the first consumer. | `SEG_STAGE` | — | **apply** | `HostSpanCount` is 0 **or** `Count`, never anything else (`MGPipeTypes.h:820-823`), so the two arrays stay index-aligned. `kCapNeedsHostUboBytes` is 0 for all of P5, so the second tail is **always absent** in this phase. |
| 14 | `SetStreamOutputTargets` (39) | `kVarTail` | **two tails**: `MGPBufferRange` × `Count`, then `Uint32` × `Count` (`:836-838`) | **none — no applier entry point exists.** | `SEG_STAGE` | — | **apply** | one `Count` sizes both tails; off the reduced path in P5 |
| 15 | `SetVertexAttribDefaults` (41) | `kVarTail` | `MGPAttribValue` × `Count` (`:864`) | `const MGPAttribValue*` (`PipeApply.h:756`), `PipeFill.cpp:2115` | as above | as above | **apply** | **two declarants that must agree**: `Count` and `popcount(Mask)` (`:863`, contract at `PipeApply.h:754-755`). A disagreement is a wire fault nothing checks today; the decoder must. |
| 16 | `DrawVbo` (59) | `kHostSpan\|kVarTail` | `MGPDrawRange` × `NumDraws` (`:1239`), then a **conditional** `MGHostSpan` when `Flags & kDrawHasUserIndices` (`:1212`, `:1231`) | **none — no applier entry point exists.** | `SEG_STAGE` | — | **apply** | `NumDraws`; the span carries its own `Size`. **The only `kHostSpan` on the hot path, and P5 must produce none of them** — `TriangleScenario` uses a VBO-backed draw precisely so this tail never appears. `MGPipeTypes.h:1221-1224` defers the fixed-head-versus-tail question to this phase: **P5 keeps it in the tail, unchanged**; there are no per-draw byte histograms yet to justify moving it, and moving it would be a wire-format change with no measurement behind it. |

### Group C — carries content with **no** `MGPBlobRef` and **no** `kHasBlob`

These three are the reason `kHasBlob` had to be given an exact meaning (table 0).

| # | call (op) | flags | ruling | evidence |
|---|---|---|---|---|
| 17 | `CreateSamplerState` (23) | **now `kHasBlob`** (R-13.1) | `MGPSamplerDesc` **does** own an `MGPBlobRef Parameters` (`:429`) and the flag was simply missing. The blob is `memcpy(sizeof(SamplerParameters))` — a POD, and **`borderColorForm` must survive byte for byte** (`MGPipeTypes.h:425-428`), because all three colour representations are always numerically populated and it is the only thing that says which one the backend must use. | companion today is a **typed frontend pointer**, `const SamplerParameters*` (`PipeApply.h:1000`), passed `SamplerEmit.h:458`. Declares `Size = 0` (`SamplerEmit.h:433-435`); cross-check at `PipeApply.cpp:2353` is inert until rule A arms it. **Padding trap:** `SamplerEmit.h:437-445` — assignment leaves three trailing padding bytes stale, and the bytes staged must be the bytes a later `memcmp` compares. |
| 18 | `GetCaps` (1) | **now `kReplySlot\|kHasBlob`** (R-13.1) | `MGPCaps` owns **two** `MGPBlobRef`s, `FormatCapabilities` and `RendererInfo` (`:137-138`), and carried no `kHasBlob` at all. | `PipeCalls.def:80` before the fix. Serializers are P5's new work (`MG_Remote/CapsCodec.h`); the header itself defers them to this phase (`MGPipeTypes.h:134-136`). |
| 19 | `ResourceRespecify` (3) | stays `kNeedsAck`, **no `kHasBlob`** (R-13.3) | **`initialBytes` is always `nullptr` under split. Initial content arrives as `ResourceSubData` records immediately after this one.** `MGPResourceDesc` owns no `MGPBlobRef` and gains none. | The alternative was costed and rejected: `MGPBlobRef` is 24 bytes, `MGPResourceDesc`'s two pads are `Uint16 Pad0` (`:303`) + `Uint32 Pad1` (`:315`) = **6 bytes**, so a blob member takes the struct 88 → 112 and moves `MGP_ASSERT_POD(MGPResourceDesc, 88)` (`:320`). The chosen route reuses a path that is already chunked (`MGPipeForEachSubDataRecordRange`, `PipeFill.cpp:694-713`) and already acceptance-gated; it costs one extra record. `HasDefinedContent` (`:301`) is the field the encoder branches on, and it already exists. **The texture path already does exactly this** — `TextureEmit.h:1137` passes `nullptr` and relies on a following upload — so this generalises today's texture behaviour to buffers rather than inventing anything. |
| 19b | `ResourceRespecify`'s **second** uncarried companion | — | **`const MGPRespecifiedLevel* level` (`PipeApply.h:792-795`, 4 bytes: `Uint16 UploadTarget; Uint16 Level;`) has no wire carrier either, and it is not bytes — R-13.3 does not cover it.** Ruling: it rides in `MGPResourceDesc`'s existing pads — `Pad1` (4 B, `:315`) becomes `{Uint16 RespecifiedUploadTarget; Uint16 RespecifiedLevel;}` and one byte of `Pad0` (`:303`) becomes `Uint8 HasRespecifiedLevel`. **Zero size change, `MGP_ASSERT_POD(..., 88)` does not move**, and `PipeFields.def`'s `MGP_FIELDS_MGPResourceDesc` gains the two named members (pads are excluded from field lists, so this is required, not optional). | Null means "this respecify redefines the **whole** resource" and drops every pending upload; non-null names the single `(uploadTarget, level)` and drops **only** that key. Clearing the whole set for a per-level `glTexImage2D` loses exactly the texels the server-side set exists to protect (`PipeApply.h:805-820`). Without a carrier, every OpenRA per-level respecify would silently take the whole-resource arm. **c0 rules it and specifies it; c0 does NOT implement it** — `MGPipeTypes.h` is unowned and the edit goes through the integrator, who must land it before w1 encodes this record. |
| 20 | `ResourceFlushRange` (51) | stays `kNone` (R-13.2) | **It carries no bytes at all under split.** It is a `{range, AccessFlags}` control record; the bytes of `[Offset, Offset+Size)` arrive **ahead of it** as `ResourceSubData` records covering exactly that range. | R-13.2 offered "add a blobref" or "write the convention down". Neither, and for a reason: the ladder this record drives rewrites its range *"from the authoritative shadow"* (`Managers.cpp:1047-1076`), and under split the authoritative shadow is **server-owned** by rule C — so `resource_subdata` is already the only way bytes reach it, and a blobref here would be a second, forgeable way to say the same thing. `AccessFlags` must still cross **verbatim**, not normalised (`PipeApply.h:902-903`). **Overturn condition:** if the tier-1 `INVALIDATE_RANGE` arm turns out to need the bytes and the range in the *same* record — i.e. an intervening record could stale the subdata — this needs its own blobref. It cannot happen while the verb barrier holds, because nothing interleaves; **revisit when the barrier retires for the buffer family.** |

### Group D — the four server → client rows the brief's list omitted

| # | call (op) | flags | ruling | reply name |
|---|---|---|---|---|
| 21 | `MapPersistent` (5) | `kReplySlot\|kOptional` | **Returns `nullptr` under split, always** (R-6/R-2.4). Its `const void* seedBytes` companion (`PipeApply.h:917`) therefore never crosses in P5 and needs no carrier. The three frontend sites already tolerate a decline (`BufferObject.cpp:238`, `:603-606`, `:657-660`). Answer travels as `Status = DECLINED` with a zero-length payload. | `map_persistent.decline` |
| 22 | `ResourceReadback` (52) | `kReplySlot` | Bytes go **server → client** in `SEG_EVENT` via `OnBufferWriteback` (#3), not in the reply slot: the destination is the client's shadow and the size is the resource's, not a fixed slot's. The reply slot carries only completion. **The ordering rule is load-bearing:** the writeback is applied **before** the mutation epoch bumps, never after (`ARCHITECTURE.md:292-294`, `Managers.cpp:2120-2136`). | `resource_readback.done` |
| 23 | `ReadPixels` (58) / `GetTextureImage` (55) | `kReplySlot` | **`ReadPixels` blocks in P5** and its pixels come back in the reply slot, which is why `ReplyPool::SlotBytes()` is sized from the scenario's largest read rather than guessed. `MGPReadbackInfo` has `DstOffset`/`DstSize` but **no `Seg`** (`MGPipeTypes.h:1197-1206`): ruling — the destination is **always `SEG_REPLY`** in P5, so no `Seg` field is added; the PBO destination (fire-and-forget plus a client-side `MarkGpuWritten`) is b1's and also needs none, because a PBO destination is a resource handle rather than a segment. `GetTextureImage` is **not on P5's reduced path** and its slot stays `Fatal{UnmigratedVerb}`. | `read_pixels.pixels` |

---

## §3 Table 2 — `PipeInputs` field ownership

**This section is the SPEC and the four class definitions. The authoritative instance is
generated**: package p1 writes the generator that emits `generated/PipeFieldOwnership.inc` plus
a `--check`, in the shape of `gen_pipe_dirty_surface.py`. **A field in none of the four classes
is a build failure** (R-7.1) — that is the whole mechanism, and a hand-maintained table would
be wrong within a week.

The domain is **63 fields** (`kMGPipeInputFieldCount`, asserted `generated/PipeFilled.inc:96`)
**plus the 7 sticky forwards**, which are among those 63 but are exempted from the poison and
so need their own row. 70 rows, each in exactly one class.

### The four classes

**RECORD-SUPPLIED** — a pushed record supplies the **whole** field, so the server never needs
the client for it.
Membership is `kMGPipeFieldEmittedBy` (`generated/PipeFilled.inc:336-402`,
`kMGPipeEmittedFieldCount = 40`) **minus** the nine for which
`EmittedCallSuppliesTheWholeField` returns false (`MG_Impl/Pipe/PipeFill.cpp:1924-1939`;
reasons `:1875-1923`). **31 fields today.**
The nine excluded, with the generator's own reason: `GetPixelStoreParameters` ("only the PACK
half has a carrier"); `GetCurrentVertexAttribute` ("the applier cannot reproduce GLContext's
cross-view conversion"); `GetMaxTouchedTextureUnit` ("the set is hash-suppressed while the
high-water mark still moves"); and six sharing one reason — "the storage is a frontend heap
reference and the record carries an 8-byte `{slot, gen}`" — `GetBoundVertexArray`,
`GetFramebufferBindingSlot`, `GetImageTextureBinding`, `GetTextureUnitObject`,
`GetProgramForDraw`, `GetProgramForDispatch`.

**APPLIER-DERIVED** — the applier writes it from the records it already applies; no client
participation at all. Today: the render-state mirrors, `m_pixelStore[0]` (the **pack** half),
capability bits, the current vertex attribute, and the patch fields — written at
`PipeApply.cpp:177-184`, `:1338-1437`, plus everything `MGPipeDeriveRenderStateFields`
(`PipeApply.h:1084`, `PipeApply.cpp:2157`) derives.

**BARRIER-PULLED** — **P5's debt, and every row names the phase that retires it.** The server
answers by reading a value the client's residual fill put into the single shared `gPipeInputs`
while the verb barrier holds both threads apart. It is correct only because of that barrier,
which is why the barrier is load-bearing rather than cautious.
Each read increments `PipeStats::CallClass::ResidualPulls` (short name `rsp`, inside the
`#if MOBILEGL_PIPE_PUSH` block so G1 holds), published per frame. **`rsp`'s value at the end of
P5 is the size of the P6/P7/P8 debt** and goes into MEASUREMENTS.
`MOBILEGL_IPC_STRICT_ERRORS=1` promotes every read in this class to `Fatal`, and a named test
asserts the abort actually happens — an instrumentation that cannot go red is decoration.

**FATAL** — no carrier, and the reduced path never reads it, so a read is a real defect.
`Fatal{UnmigratedPipeInput, "<Field>@<verb>"}` (`generated/PipeFilled.inc:407-413`), live at
every log level on purpose (`PipeInputs.h:29-31`: *"this is not `MOBILEGL_ASSERT`, which is
inert in INFO builds"*).

### The known BARRIER-PULLED rows — the 21 the reduced path actually reads

Union of `kClear` (7 of its 18 own fields), `kDraw` (19 of 47) and `kReadback` (12 of 17).
OpenRA adds no field to this set — it widens the **site** set, not the field set, and is the
first thing to reach the read-attachment sites (`Managers.cpp:8603`, `:8966`) and the
`maxTouchedUnit >= 0` texture-unit walks.

| field | class | retires in | note |
|---|---|---|---|
| `GetBoundVertexArray` | O | **P8** | `DirectGLES.cpp:4486`, `PrepareForDraw`, **unconditional on every draw**. `PipeFill.cpp:1902-1905` says the pull retires at P8, not here. |
| `GetProgramForDraw` | O | **P8** (Espryt), P7 (Magma) | `DirectGLES.cpp:4497`, same site, also unconditional. |
| `GetBufferBindingSlot` | O | P8 (indirect half), P9 (readback), P13 (transfer) | 18 Espryt sites; the 7 of 15 `BufferTarget`s no call covers (`Coverage.def:37-70`). |
| `GetBufferBindingPoint` | O | P3b/P4b + P7 | |
| `GetTouchedBufferBindingPointCount` | V | P3b/P4b | |
| `GetFramebufferBindingSlot` | O | P3b/P4b (Espryt), **P7** (Magma) | 8 Espryt sites through `GetFramebufferBindingSlotChecked`; `SyncCurrentFBO` (`:2995`) is self-declared monolith glue (`DirectGLES.cpp:2961-2965`) while `BindCurrentFBO` (`:4303-4353`) is already split-clean. |
| `GetTextureUnitObject` | O | P3b/P4b, P7 | 13 Espryt + 8 Magma sites. |
| `GetImageTextureBinding` | O | P3b/P4b, P7 | |
| `GetActiveTextureUnit` | V | P3b/P4b | server answers from its own state (`Coverage.def:215-219`). |
| `GetMaxTouchedTextureUnit` | V | P3b/P4b | hash-suppressed set, high-water mark still moves. |
| `GetTextureContextId` | V | P3b/P4b | **not a value to migrate**: the server answers from its own `Serial`. `Coverage.def:220-224` is explicit that no call carries these three and none should. |
| `GetTextureBindGeneration` | V | P3b/P4b | as above |
| `GetSamplingResolutionGeneration` | V | P3b/P4b | as above |
| `GetCurrentVertexAttribute` | V | P3b/P4b | the applier cannot reproduce GLContext's cross-view conversion. |
| `GetPixelStoreParameters` | V | **split in P5 — see below** | |
| `IsTransformFeedbackActive`, `IsTransformFeedbackPaused`, `GetTransformFeedbackProgram`, `GetTransformFeedbackGeneration`, `GetBoundTransformFeedbackLifetimeId`, `GetTransformFeedbackCapturedVertices` | V/O | P3b/P4b (Espryt XFB scatter), P7 (Magma) | read on `kDraw`; XFB itself is off the reduced path but these are read on the draw walk regardless. |

**`GetPixelStoreParameters` is split into pack and unpack in this phase** (R-7's one small
migration, and it is c0's ruling to keep rather than soften). The field is `m_pixelStore[2]` and
only `[0]` (pack) has a carrier — which the applier does write (`PipeApply.cpp:1373`) — so the
whole field reads as unmigrated while the half a readback needs is fine. Splitting it is what
stops the readback path from taking a whole-field `Fatal` for a half it never touches. The five
Espryt read sites whose `isUnpack` argument decides which half they want are
`DirectGLES.cpp:7924`, `:9399`, `:10893`, `:11272` and `Utils.cpp:2302`; **the scout named them
and did not open them**, so p1 reads the argument at each before it writes the two rows.

### The FATAL rows

Three non-sticky fields, each off the reduced path for a different, checkable reason:

| field | why it is FATAL rather than pulled |
|---|---|
| `GetBoundTransformFeedbackName` | **dead** — read by no backend since D21 (`PipeInputs.h:232-234`). |
| `GetTransformFeedbackPausedPrimitiveCounter` | reachable only from class `kQuery`, which the reduced path never enters. |
| `GetProgramForDispatch` | reachable only from `kDispatch`; there is no compute on the reduced path. Sites: `DirectGLES.cpp:5779`, `VulkanRenderer.cpp:7327`, `:7379`. |

Plus **`MGPipeUnmigratedEmulation`'s five call sites**, which in a split build stop being
`(void)name;` and become `Fatal`: `Managers.cpp:5334` ("texture-remint-pull"),
`DirectGLES.cpp:8051` ("generate-mipmap-storage"), `:8702` ("generate-mipmap-cpu-fallback"),
`:8997` ("copy-image-shadow-mirror"), `:10623` ("get-tex-image-shadow"). **One function grows
teeth and five sites get them** — `PipeApply.cpp:2820`, today a no-op, which
`PipeApply.h:1055-1066` and `PipeApply.cpp:2812-2817` both already say is waiting for this
phase. None of the five is on the reduced path. (ROADMAP's P4a row says six sites; there are
**five calls and one comment** — `Managers.cpp:5287` is the comment.)

### The seven sticky forwards

`GetBufferBindingPointCount`, `GetProgramObject`, `GetTextureObject`,
`HasOpenTransformFeedbackSpan`, `ValidateProgramName`, `InvalidateCompileEnv`, `RecordError`
(`PipeInputs.h:569-577`; identity asserted `:135-137`; argued `Coverage.def:138-150`).

P5's assignment:

- the first five → **BARRIER-PULLED** (counted in `rsp`, `Fatal` under strict);
- `InvalidateCompileEnv` → **`OnCapsInvalidated`**, i.e. the re-arriving caps snapshot (R-12);
- `RecordError` → **BARRIER-PULLED**, counted in `rsp`; its *ordering* is P9's (`OnGlError`).

**And their exemption is cancelled in a split build.** Today F-class accessors carry no
`MGP_INPUT_CHECK` at all (`PipeInputs.h:563-568`) and `MGPipeInputFieldIsFresh`
(`generated/PipeFilled.inc:418-426`) answers "fresh" for a sticky field regardless of
generation. That is exactly backwards for this phase: these seven are the ones that hand a
**frontend object or a frontend write** straight to the backend, so the exit gate "an
unmigrated field read is Fatal" is structurally blind on the seven most dangerous fields.
Under `MOBILEGL_BUILD_DISAGGREGATED` the exemption is lifted so they enter `rsp`, and under
`MOBILEGL_IPC_STRICT_ERRORS=1` they become `Fatal` like any other BARRIER-PULLED row.

### The prerequisite nobody else owns: someone must stamp

`MGPipeApplyAccess` **deliberately does not stamp** the poison generations
(`PipeInputs.h:612-618`): *"a stamp says the filler published this for THIS verb, which is the
walk's statement, not the applier's"*. Under split the filler is in the other role, so nothing
stamps, every `FilledGen[]` stays 0, `MGPipeInputFieldIsFresh` returns false for **everything**,
and a purely-server-side read aborts on the **first** field inside `SyncRenderState` —
`Fatal{UnmigratedPipeInput, "GetRenderStateParameters@<none>"}` — long before any interesting
case. **The server stamps at the verb boundary**: p1 defines what is stamped and for which
verb, v1 places the call (`Server/PipeApplier::StampVerbBoundary`). Neither half works alone,
and this is not in the ROADMAP row.

### Two sets with no field ids

**The conservative client GPU-write set.** One row per backend `MarkGpuWritten` site, mapped to
the client predicate that must fire, one unit case each:

| backend site | what it marks | when |
|---|---|---|
| `DirectGLES.cpp:570` | every SSBO binding point | draw/dispatch about to go out |
| `DirectGLES.cpp:618` | every bound atomic counter | every conformance case reads the increment back |
| `DirectGLES.cpp:2603` | buffer textures on image units, **only when `Access != GL_READ_ONLY`** | from draw preparation, deliberately not from `glBindImageTexture` |
| `UniformManager.cpp:1075` | storage texel buffer, `Access != GL_READ_ONLY` | after `EnsureGpuResidentStorage()` |
| `UniformManager.cpp:1231` | SSBO block, unconditional | after `EnsureGpuResidentStorage()` |
| `VulkanRenderer.cpp:11618` | the XFB capture targets | "the capture is a GPU write like any shader's" |

Plus **two new producers P5 adds**, both client-side with no server participation
(`ARCHITECTURE.md:508`): `glReadPixels` into a pack PBO becomes fire-and-forget plus a client
`MarkGpuWritten` (strictly better than monolith's unconditional stall), and
`glEndTransformFeedback` drops its unbounded fence wait and marks the capture targets instead.

`SyncGpuWrites` gains a **third state** it cannot express today — *emitted, answer not back* —
and under split it must **block until `OnBufferWriteback` lands** rather than clear the flag
optimistically (`BufferObject.cpp:372-374` clears unconditionally; `ARCHITECTURE.md:509` lists
this among the unavoidable blocking points, because monolith already `glFinish()`es here).
**No narrowing in P5**: `ResourceTracker.h:587-592`'s `rangeCount == 1` assertion **stays**.

**The persistent-map reachable set: the census is 21 sites, not 20.** `MEASUREMENTS.md:111`
records 20 and `Managers.cpp:5047-5048` speaks of "the eleven Espryt" sites; the actual count
is **9 Espryt + 12 Magma = 21**, and Espryt's own count is 12 (9 + 3 `SyncGpuWrites`), so both
published numbers are exactly one low and the missing one is an Espryt line. **Ruling: 21 is
the number, and `DirectGLES.cpp:361` (`ResolveIndirectCommandBytes`) is in the set.** It is a
shared helper rather than a draw-path site, which is the most likely reason it was excluded,
but a helper that reaches a persistently-mapped range is exactly as able to read stale bytes as
a draw site is; excluding it would be a shutter that cannot see its own subject. The
per-site attribution table `ARCHITECTURE.md:290` refers to as "§5.7" **does not exist in the
tree** — b1 should not go looking for it. The nine Espryt sites: `DirectGLES.cpp:361`, `:6185`,
`:6439`, `:6440`, `:6541`, `:6542`, `Managers.cpp:2817`, `:2994`, `MultiDraw.cpp:511`. The
twelve Magma: `DirectVulkan.cpp:281`, `:472`, `:796`, `UniformManager.cpp:2024`,
`VulkanRenderer.cpp:3542`, `:3621`, `:4013`, `:7428`, `:12423`, `:12424`,
`VkBufferManager.cpp:628`, `:679`. **Correcting `MEASUREMENTS.md:111` from 20 to 21 is b1's
line to write**, in the same commit that lands the tracker.

`m_livePersistentMaps` is defined by `SyncPersistentMappedRange`'s own early-out chain read as
a membership test (`BufferObject.cpp:346-349`): persistent, write, **not** flush-explicit,
**not** GPU-resident.

---

## §4 Table 3 — role and thread ownership of every process global

`ARCHITECTURE.md:578-581` claims MGPipe brings the globals a role split must duplicate down
from four to two. **That census is at least seven short.** Every row below also answers what
make-current and teardown do to it — the column `ARCHITECTURE.md` does not have.

| global | where | who writes | P5 ruling | make-current / teardown |
|---|---|---|---|---|
| `gPipeInputs` (~20 KB) | `PipeInputs.h:706` | client residual fill (`PipeFill.cpp:530`, `:2131`, `:2604`) + applier (`PipeApply.cpp:1336`, `:1364`, `:1373`, `:1377`, `:1436`, `:1512`) | **One instance is legal, but only under the verb barrier.** The barrier makes at most one of {GL thread, apply thread} runnable at a time, so there is exactly one writer at any instant. **No second writer may be introduced before the barrier retires.** The invariant is a runtime assertion in debug/verify builds, not only a sentence here: the apply thread raises a flag on entering the applier and the client checks it when it touches `gPipeInputs` outside a barrier (`ClientSession::InBarrierWait` / `ApplyThreadIsInsideApplier`). | make-current: unchanged. teardown: nothing — it is POD in the image. |
| `g_applier` | `PipeApply.cpp:396` | applier | **Server-exclusive.** Its own header already says "under split there is one per served context" (`PipeApply.h:684`). | It is `*new MGPipeApplierState{}` and never destroyed, deliberately (`PipeApply.cpp:392-395`): `resource_destroy` is raised from `~BufferObject`, which runs from exit handlers after this TU's globals are gone. |
| `g_resourceOps` | `PipeApply.cpp:402` | the backend, at register time | **Server-exclusive, and the client must NEVER read it** (R-8). Under `inproc` a client reading it is right *by accident*; under spawn it is null and the four P4a families plus P3a's buffers emit **nothing at all**, silently. The client asks `CapsMirror::ServerConsumes` instead. | registered around `DirectGLES.cpp:11933`, nulled from `OnBackendContextDestroyed` (`Managers.cpp:2584`) — so it moves on every context loss, which is another reason the client cannot key on it. |
| `gMGPipeSegmentResolver` | `MGPipeHostSpan.h:47` — a **plain non-atomic inline variable** | `MG_Remote` installs | **One process-wide slot, so it cannot be per-role.** Ruling: **the server role installs it and the client never resolves a span at all** — the client only ever *writes* `Ptr = nullptr`. `SegmentTable::InstallProcessResolver()` asserts if one is already installed, so two roles racing on it is loud rather than silent. Install **before** the apply thread starts. | teardown: uninstall after the join, never before — a record still in flight can still resolve. |
| the ten `MG_Impl/Pipe` `*Instance()` singletons | `fable-seam-audit.md:120-135` | client | **Client-exclusive.** One correction carried forward: the texture **drain list `m_drain` is process-wide**, not per-context as D-D4 claims; the audit already booked "one drain per client context" as a P5 item. | `FreshlyPrimed` (`PipeFill.cpp:2414-2440`) resets them on make-current; leak at exit (ID-8). |
| `ScopedDefaultUnpackState::s_synced` + **six** value shadows | `Managers.cpp:5490-5496` | backend | **Server-exclusive — the sixth global the four→two census missed.** Latent rather than live in P5 only because the client role never touches GL on the reduced path. (`Managers.cpp:5491-5496` is six `GLint`s, not five: `s_skipImages` at `:5496` is the one usually dropped.) | **Never reset on context death** — `OnBackendContextDestroyed` resets the rings and the binding caches and not this. Benign while a lost context returns the driver to GL defaults; not benign the day a server re-attaches to a context something else moved. Register it now. |
| `pActiveBackendObject` | `GlobalObjects.cpp:23` | `MG_Backend::Init()` | **Client installs `BackendObject_Remote`; the server's `BackendObject_DirectGLES` is held privately by `ServerLoop`.** No thread-keyed shim, and therefore `MOBILEGL_BUILD_DISAGGREGATED_INPROC` needs none — but the cost is **seven** backend-internal reads across **six** functions, not the one the scout reported: `BackendObject_DirectGLES.cpp:815`, `:819` (`ClampSamplesToBackendSupport`) and `Utils.cpp:74`, `:82`, `:126`, `:220`, `:260`. All seven are format-capability lookups, so "pass the format cache down" still works. **`DirectGLES.cpp:12446` is NOT `ClampSamplesToBackendSupport`** — it is `Present()`'s fence poll, and `DirectGLES.cpp` contains no `pActiveBackendObject` reference at all. | `GetFormatCapabilities()` is **non-virtual** (`BackendObject.h:594`), so the remote object must **fill** `m_formatCapabilities` rather than override the accessor. Teardown: `pActiveBackendObject.reset()` (`MobileGL/Init.cpp:68`) runs `~BackendObject_DirectGLES` → `DestroyEGLContext()`, so under split it must be a **blocking** request onto the apply thread. |
| `gBackendFunctionsTable` | `GlobalObjects.cpp:24`, assigned `Init.cpp:44` | `MG_Backend::Init()` | **Client = the emit table (R-4); the server holds its real table directly and never goes through this global.** | cleared at `MobileGL/Init.cpp:91`. |

**Teardown order**, `ARCHITECTURE.md:537` plus the sentence it omits:

1. client publishes and waits for the server to drain and acknowledge;
2. **`Doorbell::Kill()`** — *the only thing that can wake an apply thread parked on
   `kWaitForever`* (`Doorbell.h:211-221`; the shape is already pinned by
   `InProcessTransportTest.cpp:344`);
3. **join**, bounded (that test uses 5 s) so a regression is a red test and not a hung CI job;
4. only then may the client free anything an emitter owns — a tail still referenced by an
   unapplied record is a use-after-free the join is what prevents;
5. then the existing order (`MobileGL/Init.cpp:38-98`).

**ID-8 applies once per role-local singleton, not once overall**: every new
`MG_Remote/Client/*` and `MG_Remote/Server/*` singleton leaks at exit. The proof recipe is
inherited: both lanes run `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`.

**Known open item, flagged not resolved.** `ARCHITECTURE.md:537`'s required order puts the
client's sync/query handle release **after** the transport closes, while today
`DestroyAllSyncObjects` / `DestroyAllQueryObjects` (`MobileGL/Init.cpp:62`, `:67`) deliberately
run **before** `pActiveBackendObject.reset()` (`:68`). The two are only reconcilable if a split
sync handle is client-minted and needs no backend call — which is P10's, not P5's. **P5 keeps
today's order** and v1 records which way it went.

---

## §5 The knobs

Parsed in `ConfigLoader.cpp`, declared in `Config.h`. All of them live behind
`#if MOBILEGL_BUILD_DISAGGREGATED` — including the parser — because `MG_ConfigLoader::Init()`
is a pull-build symbol and G1 admits **no resize**, which is the same reason the
`MOBILEGL_PIPE_VERIFY` knobs sit behind their own `#if`.

| knob | default | notes |
|---|---|---|
| `MOBILEGL_TRANSPORT` | `monolith` | `monolith\|inproc\|spawn\|unix:<path>\|pipe:<name>`. The three P6 forms **parse and are then refused by name**, staying on monolith: a P6 lane that set `spawn`, fell back silently and went green on the wrong arm is the failure this wording avoids. |
| `MOBILEGL_IPC_SERVER_PATH` | `""` | P6 consumes it; P5 parses it because t1's ctest `ENVIRONMENT` blocks and `add_trace_replay_test`'s SPLIT variant already carry it, and an unparsed variable is indistinguishable from a parsed-and-ignored one. |
| `MOBILEGL_IPC_RING_MB` | 8 | SEG_CMD. **One record may be at most half of this** (`RingProducer::MaxRecordBytes`), so 8 MiB caps a record at 4 MiB. R-10 makes the codec publish a max-record-bytes counter rather than assume that is enough. |
| `MOBILEGL_IPC_STAGE_MB` | 32 | SEG_STAGE. Every blob and every var-tail's bytes. |
| `MOBILEGL_IPC_SPIN_US` | 50 | spin before parking, either direction. |
| `MOBILEGL_IPC_PERSISTENT_BLOCK_KB` | 64 | **0 is the E3(a) negative control, not "unlimited"**: it turns the push off and `PersistentCoherentMapScenario` must go red. |
| `MOBILEGL_IPC_ADOPT_TIER` | 2 | 2 = emulate, the only tier P5 implements. 0 and 1 parse and are `Fatal` at use, naming P11. |
| `MOBILEGL_IPC_VERB_BARRIER` | 1 | 0 is R-1's negative control and is **expected** to be red. |
| `MOBILEGL_IPC_STRICT_ERRORS` | 0 | promotes BARRIER-PULLED reads — and, in a split build, the seven sticky forwards — to `Fatal`. |
| `MOBILEGL_IPC_AUDIT` | 0 | `0xDD` over retired staging bytes (rule C's mechanical control). |
| `MOBILEGL_IPC_SERVER_AFFINITY` | `auto` | kept as the raw string; whoever starts the apply thread logs the **resolved mask**, because an affinity that silently did nothing looks exactly like one that worked. |

**One consequence, stated so it is not rediscovered.** In a build *without*
`MOBILEGL_BUILD_DISAGGREGATED`, `MOBILEGL_TRANSPORT=inproc` is accepted by the environment and
**silently ignored** — the parser does not exist there, and putting a complaint in the
unconditional part of `ConfigLoader` would move a pull-build symbol and break G1. That is
precisely the shape of "the split lane ran monolith and went green", so the guard against it is
a **build-level** check, not a runtime one: `nm --defined-only libMobileGL.so | grep -i
MG_Remote` must be non-empty in `build-split`, and it is t1's CI job to assert that.

CMake gained `MOBILEGL_BUILD_DISAGGREGATED_INPROC` (implies `DISAGGREGATED`) and, new here,
**`MOBILEGL_BUILD_DISAGGREGATED` now implies `MOBILEGL_PIPE_PUSH`**: the split path decodes
into the MGPipe applier and `MOBILEGL_PIPE_PUSH` is what compiles the applier, so
`-DMOBILEGL_BUILD_DISAGGREGATED=ON` alone used to configure cleanly and then fail to link — a
shape indistinguishable at the CMake level from a legitimate transport-only build.

---

## §6 Rulings this file makes that the brief did not, and where the brief is wrong

Each entry says what would overturn it.

1. **`CallMask` bits 32..47 are the consumer mask.** R-8 says the client's liveness gates read
   the `CallMask` mirror, but `CallMask` as declared has only nine feature bits and no
   per-family bit, so R-8 was not implementable as written. Overturned by: a decision to carry
   a second mask field in `CapsSnapshot` instead — which costs a schema field and gains
   nothing, since 16 bits is enough through P8.

2. **`tableSlotMask` is deleted, not renamed** (R-8 allowed either). Decisive evidence:
   `GLFunctionsTable` has **69** slots and `ulong` is 64 bits, so the field cannot address the
   table its own comment names. Overturned by: widening the schema field *and* a reason to
   keep an explicit slot probe after `ARCHITECTURE.md:114` retired the concept.

3. **`ResourceFlushRange` carries no bytes at all** (R-13.2 offered "add a blobref" or "write
   the convention down"; this is a third answer, and a stronger one). The ladder it drives
   rewrites from the authoritative shadow, which under rule C is server-owned, so
   `resource_subdata` is already the only path bytes take. Overturned by: evidence that the
   tier-1 `INVALIDATE_RANGE` arm needs bytes and range in one record. Cannot arise while the
   verb barrier holds; **revisit when the barrier retires for the buffer family.**

4. **`ResourceRespecify` also has a SECOND uncarried companion, and the brief does not mention
   it.** `const MGPRespecifiedLevel* level` (`PipeApply.h:792-795`) is the *scope* of the
   redefinition and `MGPResourceDesc` cannot express it. Without a carrier every per-level
   `glTexImage2D` in OpenRA silently takes the whole-resource arm and drops every pending
   upload — the exact texel loss the server-side set exists to prevent. Ruling: two named
   fields in the existing pads, zero size change. **c0 rules and specifies; the integrator
   lands the `MGPipeTypes.h` + `PipeFields.def` edit before w1 encodes this record.**

5. **Table 1 is 23 rows, not 19.** The brief's 19 and `scout-premortem:§3`'s 19 are different
   lists; the four only the premortem carries are the server → client ones, and a phase that
   omits them discovers in week three that it never decided where readback pixels land.

6. **`SetResidualValueState` is a fourth typed companion, and neither scout nor the brief names
   it.** `MGPipeApplySetResidualValueState` takes `const ResidualValueBlock&` — not a payload,
   not a `const void*` — and `MGPResidualValueState` is **never instantiated on the live path**.
   The encoder has to invent both the record fill and the blob fill. Budget it as w1's hardest
   row, not as one of the easy `kHasBlob` eight.

7. **Three of the brief's 19 have no applier entry point at all** — `SetShaderBuffers` (38),
   `SetStreamOutputTargets` (39), `DrawVbo` (59). `scout-premortem:§3` cites
   `PipeApply.h:756, 941, 1028-1030` for a six-call row; those five citations cover five *other*
   calls. For these three, "what crosses today" is **nothing**, and P5 writes the first producer
   *and* the first consumer.

8. **`CreateRenderState` and `SetDynamicState` declare a `Blob.Size` that nothing ever reads.**
   `scout-wire-codec:§4.2`'s "`Size = 0`" column is stale for four rows (add
   `CreateVertexElements` and `ResourceSubData`'s buffer half). The applier's only four
   `Blob.Size` reads are `PipeApply.cpp:702`, `:1998`, `:2353`, `:2784`. A fifth
   flags-vs-payload-vs-signature disagreement for the reviewer's list.

9. **The emit table is 71 function pointers, not 69.** R-4 says 69 slots; that is
   `GLFunctionsTable`'s count. The table the client actually installs is
   `GlobalBackendFunctionsTable` = 69 + `Present` + `SetSwapInterval`. R-4's rule (no null slot,
   no pass-through) applies to all 71, and `Present` is on the reduced path. The Bool member is
   not a verb and is answered from `kCapCpuXfbPrimitiveAccounting`.

10. **`prefersCpuXfbPrimitiveAccounting` is a member of `GLFunctionsTable`
    (`BackendObject.h:274`), not of `DynamicBackendParameters`.** So it does **not** ride inside
    `MGPCaps::Dynamic`, and R-8's "same redundancy as (6)" is the wrong frame — it has three
    spellings and no carrier in `MGPCaps` except the cap bit. Its one non-test client reader is
    `GL_Query.cpp:221`.

11. **The persistent-map census is 21 sites, and `MEASUREMENTS.md:111`'s 20 is wrong.** Ruling
    and the missing site named above. **`ARCHITECTURE.md`'s cited "§5.7" attribution table does
    not exist in the tree.**

12. **`ScopedDefaultUnpackState` has six value shadows, not five** (`Managers.cpp:5491-5496`);
    both the brief and the scout say five.

13. **`CanTouchGLNow()` guards 16 call sites, not 19.** 19 is the raw grep: 1 definition
    (`Managers.cpp:928`) + 2 comment mentions (`:1967`, `:1979`) + 16 calls. The brief's "19
    sites" over-counts. `IsBackendContextCurrentOnThisThread`'s 16 is right.

14. **`MGPipeApply*` is 37 entry points and 41 call sites**, not "~45 entry points" — and
    `DirectGLES.cpp:12446` is `Present()`'s fence poll, **not** `ClampSamplesToBackendSupport`
    (which is `BackendObject_DirectGLES.cpp:807-828`). Table 3's `pActiveBackendObject` row is
    still correct but the diff is six functions, not one line.

15. **The 18 `build-split` unit aborts are not a poison problem.** They are
    `Fatal{ProtocolCorruption}` trip wires the tests *expect*; seven test TUs test
    `MOBILEGL_PIPE_POISON` without including the only header that defines it, so the macro reads
    as 0 and they compile the "logs and carries on" arm while `PipeApply.cpp` compiles the
    aborting one. Invisible in a push build (where it really is 0) and in a verify build (where
    `-DMOBILEGL_PIPE_VERIFY=1` is on the command line); `MOBILEGL_BUILD_DISAGGREGATED` is the one
    arming condition behind the header. Fixed in c0's own commit, test-local, no p1 surface.

16. **`MGHostSpan`, not `MGPHostSpan`.** The header is `MGPipeHostSpan.h`; the struct is
    `MGHostSpan` (`:28`). `MGPHostSpan` does not exist.

17. **`ARCHITECTURE.md`'s own corrections, carried here so they are not lost**: `:83` says 61
    `PipeInputs` fields, it is 63; `:492` cites `PipeStats.h:126` for
    `MapPersistentRoundtrips`, it is `:141`; ROADMAP's P4a row says six
    `MGPipeUnmigratedEmulation` sites, it is five calls plus one comment; `ARCHITECTURE.md:19`
    says eight EGL lifecycle virtuals, there are nine (`ResizeEGLWindowSurface` is the
    uncounted one).

