# Call Catalog and Record Format (formerly ARCHITECTURE §3–§4)

> Design details. The design highlights and an index of all sections are in [`../ARCHITECTURE.md`](../ARCHITECTURE.md). Section numbers follow the original `ARCHITECTURE.md`; `ARCHITECTURE.md §N` in code comments resolves by number here.

## 3. Call catalog

### 3.1 Single source of truth

`MobileGL/MG_Pipe/PipeCalls.def`: one `X(Name, Payload, Class, Flags, WaitClass)` per line. **The on-wire opcode is the line order**; you can only append, and retired lines keep their slot; the line count is pinned by `MGP_CALL_LIST_DOCUMENTED_COUNT` (81 today) and `PipeCatalogueTest`. Generator outputs are committed to the tree; CI `pipe-gates` regenerates them and runs `git diff --exit-code`:

| | Output | Content |
|---|---|---|
| G1 | `PipeTables.inc` | the two function pointer tables |
| G2 | `PipeThunks.inc` | monolith direct-call thunks |
| G3 | `PipeWire.inc` | wire records, size `static_assert`s, applier bounds checks |
| G4 | `PipeVerify.inc` | `MOBILEGL_PIPE_VERIFY` field-by-field comparator (`PipeFields.def`) |
| G5 | `PipeFilled.inc` | `PipeInputs` field ids and per-verb generation poison |
| G6 | `PipeCoverage.inc` | backend read sites → call mapping (`Coverage.def`); 0 UNMAPPED is the gate |
| G7 | `PipeSpanTable.inc` | render-state pipeline subset membership table |
| G8 | `PipeFieldOwnership.inc` | each `PipeInputs` field belongs to exactly one of `RECORD-SUPPLIED / APPLIER-DERIVED / BARRIER-PULLED / FATAL` (0 BARRIER-PULLED since P5f) |

There are also `FillPoints.def` (fill points) and `DirtySurface.def` (§5.2). G1 codegen rule: when the capability gate replaces a pointer expression, preserve the **expression shape**, otherwise the pull `.text` drifts.

### 3.2 Groups and flags

- Class: `kScreen` (caps, resource, persistent map, fence, `ApplierReset`), `kCtxQuery`, `kCtxCso` (create/bind/delete × five kinds of CSO), `kCtxState` (`Set*` state, `SetContextValues`, `SetProgramBindings`, `SetResidualValueState` during migration), `kCtxObject` (subdata, readback, mip, `ObjectDeath` …), `kCtxVerb` (blit, clear, `DrawVbo`, dispatch, XFB, `Present` …).
- Flags: `kNeedsAck`, `kHasBlob`, `kVarTail`, `kHostSpan`, `kReplySlot`, `kOptional`. The fifth column, `WaitClass` (P5e), decides whether the client waits under run-ahead (§11.6).
- The 20 draw entry points collapse into the single `DrawVbo`; `Clear` is a single discriminative call; the sampler set **has no stage dimension** (a merged 192-unit space); `SetTextureParams` is resource-addressed and separate from sampler views (D10); `SetIndexBuffer` is independent of VAO configuration (D5).
- Deliberately not ported: `GetIntegeri_v` / `GetInteger64i_v` / `GetProgramiv`, `set_pixel_unpack_state` (the frontend already resolves it), the compressed format concept, `pipe_transfer`.

#### 3.2.1 Meaning of the flags and WaitClass

- `kHasBlob` strictly means the payload owns an `MGPBlobRef` member, not "this call carries bytes": if the decoder trusts it, there must be a member to read. The initial contents of `ResourceRespecify` and the bytes of `ResourceFlushRange` do not go through a blobref; they cross the wire as an adjacent `resource_subdata` record.
- `kNeedsAck` only means this call's record **may** need an ack, decided by a per-record predicate (`MGPipeResourceRespecifyNeedsAck`, §8.3).
- `kReplySlot`: the applier returns an answer the client will act on (clearing level dirty bits, latching parameters). The flag exports only one `kMGPipeCallFlags[]` (`generated/PipeWire.inc`); consumers do not copy it.
- The fifth column, `WaitClass`, is a value rather than an opcode, generated as `MGPipeWaitClassFor(op)`, one static entry of `MGPipeBarriered` (§11.6); the client uses it to decide whether to wait only when run-ahead is armed (the server publishes `kCapRunAheadApply`); otherwise it goes lockstep:
  - `kWaitReply`: the answer is not derivable and the client needs it, so block on the reply slot. It is present and absent together with `kReplySlot`, and `gen_pipe.py` rejects inconsistent lines: a reply line that does not wait would make the client read a slot nobody fills; a waiting line with no slot would wait for an answer nobody sends. A per-record route predicate can relax this further (e.g. `MGPipeSubDataWantsItsReply`).
  - `kWaitPresent`: the pace is decided by the present credit taken before encoding, and there is no waiting after publish.
  - `kWaitApplied`: the client stops until the record has been applied (today only `applier_reset`).
  - `kWaitNone`: apply does not read client memory, so publish and go.

### 3.3 Capability bits (`MGPCapBit`)

`CallMask` replaces the implicit capability probe of "is the slot null"; `MGPCaps` = `DynamicBackendParameters` + `CallMask` + two blobs, the format capability table and the renderer string; snapshotted after the handshake, republished per generation after `MakeCurrent` / `InitCapabilities`. The bits include viewport array, fp64 vertices, `kCapResidentSubData`, the XFB / query family, `kCapNeedsHostIndexBytes`, `kCapNeedsHostUboBytes`, `kCapBackendOwnsXfbCapture`, `kCapRunAheadApply` (`MGPipeRunAheadCapBitsFor`: Espryt since P5e, and Magma run-ahead since P5f, whose contract is in the git history). **There is no "ownership switch" (D-B7)**: multi-draw tiering and restart rewriting always belong to the server, and the client supplies index bytes only when the caps ask for them.

## 4. Record and payload conventions (`MG_Pipe/MGPipeTypes.h`)

- Each payload is a flat POD with explicit padding and a `static_assert` on the exact size; **it never contains pointers**.
- `MGPBlobRef{Offset, Size, Seg}` points into the blob area; `MGHostSpan` is the only shape that changes with the transport (a pointer under monolith, bytes in `SEG_STAGE` under split), and it only goes into the variable-length tail.
- The wire record header `MGPWireRecHeader{Op, Flags, Size}` (8 B); **there is no per-record sequence number**, the seq is the record ordinal.
- The upper bound of a single record is half the ring, and **a record itself is never chunked** (R-10); **content is chunked**: a blob that would exceed `SEG_STAGE` is split by `MGPipeStageChunkBytes()` (default segment/4 = 8 MiB) into several records (buffer range walks, full-width texture slabs; the server reassembles the whole level).

### 4.1 Key payloads

| payload | Key points |
|---|---|
| `MGPResourceDesc` | one discriminative create/respecify shape for buffer / texture / renderbuffer; `ImageBindableHint`, `ViewOf` (the storage owner of a view), `HasDefinedContent` |
| `MGPRenderStateDesc` / `MGPBindRenderState` / `MGPDynamicState` | §5.3 |
| `MGPVertexElements` | the blob carries both the parsed attributes and the binding-point views (the record is self-consistent) |
| `MGPSamplerView` / `MGPTextureParams` | a view carries only view restrictions; texture parameters hang off the texture object, and the `BuiltinSampler` handle must not be null (D-E1) |
| `MGPProgramDesc` | per-stage SPIR-V + reflection archive (§7) |
| `MGPFramebufferState` | 8 color + depth + stencil + the client-resolved read surface; `ContentHash` is both the render-pass memo key and the emission suppressor; `Target` includes `Named = 3`, and the applier stores by framebuffer handle |
| `MGPSubData` / `MGPSubRegion` | §6 |
| `MGPDrawInfo` / `MGPDrawRange` | `DrawVbo`; multi-draw ranges and user indices in the variable-length tail, indirect's `DrawCount` resolved by the client |

Every `kVarTail` `set_*` carries a `ContentHash`; if the hash is unchanged, nothing is sent (§5.4).

### 4.2 Honesty rules under split

In one address space a host pointer can be dereferenced, a blobref with a host address as `Offset` can be resolved, and an address returned by persistent map can be used directly; if `inproc` turns green by borrowing these shortcuts, `spawn` cannot reproduce it. So under split, encoding and decoding always follow cross-process rules, `inproc` included (`MG_Remote/Wire/PipeWireCodec.h`):

1. The encoder always writes `nullptr` for `MGHostSpan::Ptr`, puts the bytes in `SEG_STAGE`, and names them by `Seg` / `Offset`.
2. `MGPBlobRef` either fully declares itself (real `Seg`, an in-segment `Offset`, nonzero `Size`), or all three fields are zero. `Seg` or `Offset` nonzero with `Size == 0` is exactly the shape of a monolith emitter (`Offset` is a host address); under split it is rejected as corruption and must not be read as "no blob", otherwise the bytes of an unmigrated emitter would be silently dropped.
3. The decoder reports `ProtocolCorruption` for `Ptr != nullptr`, for `Size == 0` on a content record, for `Size != 0` with `Seg == kSegNone`, and for `Offset + Size` running past the end of the segment (`CheckBlobIsHonest`, `RequireDeclaredBlob`, `CheckHostSpanIsHonest`). Whether a record carries content is decided by other fields of the payload, so "a content record must have a blob" is checked by each per-op branch of the decoder calling `RequireDeclaredBlob`.
4. In the T2 tier, `MGPipeApplyMapPersistent` always returns `nullptr` under split (§12); the client does not take the server's address.
5. With `MOBILEGL_IPC_AUDIT=1` the server fills the `SEG_STAGE` bytes of retired records with `0xDD`; an implementation that still holds a pointer after apply reads `0xDD` on the next frame.
