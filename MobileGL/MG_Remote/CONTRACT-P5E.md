# CONTRACT-P5E — the client runs ahead of apply on the Espryt draw path

Authority: this file, beside `CONTRACT-P5.md` (table 0, byte carriers, field ownership, R-1…R-17),
`CONTRACT-P5B.md` (the class-C slots) and `CONTRACT-P5C.md` (rule E, the two named exemptions,
SEG_EVENT, the guards). Where it disagrees with any of them this file is newer and wins; §8 lists
every such place. Base: `feat/disaggregated @ 2fde7034` (code head `1f8de61b`). Every `file:line`
was read at that commit and re-resolved at c0e's head; paths are under `MobileGL/` unless they
start with `docs/`.

**How to change it.** Package c0e's file, edited by the integrator first. A P5e package (id / vi /
sb / pg / tx2 / fb / ra) that needs a row changed goes through the integrator; the packages compile
against the rows below from day one.

**What c0e actually landed, where it differs from the plan this file was written as.** Four places,
each argued where it sits: `MGPProgramDesc` GREW 192 → 200 bytes because it has no spare byte (§1);
the applier's per-class binding-point windows are spelled `ShaderBufferStart/Count/WritableMask`
rather than bare `Start/Count/WritableMask` (§5.6); the suppressor's three slots are named
`SetShaderBuffersUniform/ShaderStorage/AtomicCounter` (§1); and the wire codec's record layout now
carries THREE tails rather than two, because `set_program_bindings` has three (§1). Everything else
below is the plan as written, and the nineteen rulings of §9 are as the integrator decided them
(ID-80…98).

---

## §0 What P5e is, and the rule above every row

P5d made `inproc` CPU-bound on the client thread with the frame still `client work + (per draw)
wait for apply + handoff` (R-1, `MG_Remote/Client/ClientSession.cpp:932-959`). The wait exists
because the apply of a draw still dereferences client-owned memory: the object-class BARRIER_PULLED
rows of `MG_Pipe/FieldOwnership.def:72-106` and the frontend-keyed twin registries
(`MG_Backend/DirectGLES/SlotTables.h:411-449`) inside the two P5C §3.1 exemption scopes. **P5e
retires the lockstep for every op on the Espryt draw path: the client publishes and moves on; the
server resolves every object from a handle a record carried, and the only client memory it may read
is the residual fill of a record the client is provably blocked behind.**

Rules A–E bind unchanged. P5e adds one definition and one rule:

**Definition — a BARRIERED record.** A record is barriered iff `MGPipeBarriered(op, payload,
applierState)` is true, a pure function of wire-visible data computed identically by the emit
table and by the sink (§2.1). The client blocks in `WaitForApplied(seq)` after publishing a
barriered record; it never blocks after an unbarriered one.

**Rule F — an unbarriered record's apply names no client memory.** With a live wire on a
run-ahead server (§6), the apply of an unbarriered record reads NOTHING from client-owned memory:
no frontend object dereference, no `gPipeInputs` BARRIER_PULLED row, no `MGPipeSlots()` probe or
mint, no `SharedPtr` to a frontend object taken or held. Every object is resolved from a handle the
record or the applier state carries, through a handle-keyed twin table (§4). A violation is
`Fatal{UnmigratedPipeInput, "<field>@<verb>"}` (a pulled row, §3.3), `Fatal{RoleViolation,
"MGPipeSlots"}` (an allocator probe, §4.4) or `Fatal{RoleViolation, "<surface>"}` (P5C §6 layer 1),
**regardless of `MOBILEGL_IPC_STRICT_ERRORS`** — the value would be torn or stale by construction,
so there is no "count it" arm. A barriered record's apply keeps P5C's rules exactly: its pulls count
into `rsp`, its probes are legal inside a scope, and the client's wait is what makes them legal.

Refusal vocabulary is P5C's, plus: `Fatal{UnmigratedVerb, "DrawArrays+CLIENT_ARRAYS"}` (§5.1),
`Fatal{UnmigratedVerb, "set_global_constants"}` (§5.5), `Fatal{UnmigratedEmulation,
"image-bindable-redirty"}` (§5.2), `Fatal{ProtocolCorruption, "SetSamplerViews.Count"}` /
`"BindSamplerStates.Count"` / `"SetShaderBuffers.Class"` / `"SetShaderBuffers.Count"` /
`"SetProgramBindings.<field>"` / `"Present.FrameSerial"` / `"ImageView.Access"` (§1, §5),
`Fatal{PresentCreditTimeout}` (§2.4), `Fatal{RoleViolation, "gPipeInputs"}` (§3.5).
`Fatal{EventRingOverflow}` is RETIRED on a run-ahead server (§2.6).

---

## §1 Table 0 additions — encodings the P5e rows introduce

| field | the ruling | zero means | who reads it |
|---|---|---|---|
| **`MGPCapBit::kCapRunAheadApply = 1ull << 10`** (`MGPipeTypes.h`, bits 0..9 used) | Set in the DirectGLES arm of the server's `CallMask` (`MG_Backend/Init.cpp`), NEVER in the DirectVulkan arm. THE ARM IS A PURE FUNCTION, `MGPipeRunAheadCapBitsFor(backendType, ready)` in `MGPipeTypes.h`, so "Magma never" is a unit case (`MG_Test/Pipe/MagmaPipeIdentityTest.cpp`) rather than a line only a live split session reaches; `ready` is `MG_Backend/Init.cpp`'s `kMGPipeP5eRunAheadReady`. It says "this server applies unbarriered records without reading client memory". The client latches `RunAheadArmed()` = `m_barrierArmed && Ipc.RunAhead && Caps().HasCap(kCapRunAheadApply)` at the first caps adoption after `Start`; a later snapshot may only turn it OFF. | absent = lockstep (Magma; and Espryt until the integration commit flips `kMGPipeP5eRunAheadReady`, §6.2) | `ClientSession::RunAheadArmed`, `MGPipeBarriered` on both sides |
| **`MGPipeWaitClass`** — a FIFTH column of `PipeCalls.def`'s X-macro (`MGPipe.h`'s enum, spelled as a bare token like the class column), generated beside `kMGPipeCallFlags` into `generated/PipeWire.inc` as `MGPipeWaitClassFor(op)` | `kWaitReply` (every `kReplySlot` row), `kWaitPresent` (`Present`), `kWaitApplied` (the static barriered rows of §2.2), `kWaitNone` (everything else). Opcode order and the flag column are untouched: the column is a value, not a new op. `gen_pipe.py` refuses a token that is not an `MGPipeWaitClass` enumerator AND a row where the flag column and this one disagree in either direction — a `kReplySlot` row that did not wait is a waiter that hangs, a `kWaitReply` row with no slot blocks on an answer nobody posts. Three negative controls in `--self-test` (twelve now, up from nine). | — | `EmitAndWaitTails`, `ServerVerbSink::ApplyOne`, `MGPipeBarriered` |
| **`MGPDrawInfo::Flags` gains `kDrawClientArrays`** = `1u << 6`, the next free bit beside `kDrawIsIndirect` (`MGPipeTypes.h`'s `MGPDrawFlagBit`; `MGPDrawInfo` stays 56 bytes, the flag byte had two bits left) | Set by the client emit arm when any ENABLED attribute of the bound VAO has no buffer. §2.1 escalation (ii). `PipeCatalogueTest` pins the bit's value and its disjointness from the five before it. | no client array in this draw | `MGPipeBarriered`; `EmitDrawRecord`'s refusal (§5.1) |
| **`MGPPresent::FrameSerial`** | Minted by the CLIENT, `++m_presentsSent`, 1-based — no longer 0 (`EmitTables.cpp:1040`). The sink calls `ServerSession::ReturnPresentCredit(FrameSerial)` after `table->Present()` returns (today `ReturnPresentCredit` has no production caller, `ServerSession.cpp:677-682`), one credit per swap. | 0 arriving on a run-ahead server is `Fatal{ProtocolCorruption, "Present.FrameSerial"}` | `OnPresent`, `WaitForPresentAck` |
| **`MGPShaderBuffers` + `MGPBufferRange[]`** (`MGPipeTypes.h`, wire op 38, catalogued since P4a, no route/sink/emitter today) | One record per **Class** ∈ {Uniform=0, ShaderStorage=1, AtomicCounter=2} — the three values are `kMGPipeShaderBufferClassUniform/ShaderStorage/AtomicCounter` in `MGPipeTypes.h`, which is the ONLY spelling of them: the payload's comment had named them since P4a and nothing numbered them, so the emitter and the applier were one literal each away from disagreeing. `Start = 0` always; `Count = TouchedBufferBindingPointCount[target]` (the rv value already carried) clamped to `kMGPipeMaxBufferBindingPoints = 84` (pinned against `BufferState.h:29` in `PipeFill.cpp`); per entry `Res` = the buffer handle or null, `Offset/Size` from the point's range, **`Size = kMGPipeWholeBuffer` (`MGPipeTypes.h:406`) for a base binding** so the server re-resolves the extent against ITS descriptor at use (a `glBufferData` between emit and apply then binds the new extent, as GL does); `WritableMask` bit per entry for classes 1/2. `HostSpanCount = 0` on Espryt for the whole of P5e (`kCapNeedsHostUboBytes` is 0, `CONTRACT-P5.md:64`); the codec's honesty pass (`PipeWireCodec.cpp:1783-1799`) is the guard. `Class >= 3` → `Fatal{ProtocolCorruption, "SetShaderBuffers.Class"}`; `Start + Count > 84` → `"SetShaderBuffers.Count"`. XFB targets do NOT ride this record (`set_stream_output_targets` keeps its row, still unemitted: XFB stays lockstep, §5.7). | `Res` null = nothing bound at that point | `MGPipeApplySetShaderBuffers`, `SyncBufferBindingPointsByRecord`, the UBO loop, `SyncAtomicCounterBuffers` |
| **`set_program_bindings` — opcode 80, appended.** `X(SetProgramBindings, MGPProgramBindings, kCtxState, kVarTail\|kHostSpan, kWaitNone)` | Head `MGPProgramBindings { MGPipeHandle Cso; Uint64 Signature; Uint32 BlockBindingCount, SamplerUnitCount, StorageOverrideCount, Pad0; }` (32 B, `MGP_ASSERT_POD`); three tails in order: `Int32 BlockBindings[BlockBindingCount]` (dense, GL uniform-block index order), `MGPProgramSamplerUnit {Uint32 Location; Int32 Unit;}[SamplerUnitCount]` (8 B, sparse, ascending Location), `MGPProgramStorageOverride {MGHostSpan Name; Int32 Binding; Uint32 Pad0;}[StorageOverrideCount]` (40 B; the storage-block override map is name-keyed BY DESIGN, `ProgramObject.h:1030-1034`; the name travels as a host span staged whole). `Signature` = the client's commutative hash of the override map, the same function as `Managers.cpp:10832-10841`, so the server's rebuild clause is one `Uint64` compare. Bounds: `kMGPipeMaxProgramBlockBindings` 64, `kMGPipeMaxProgramSamplerUnits` 256, `kMGPipeMaxProgramStorageOverrides` 64, else `Fatal{ProtocolCorruption, "SetProgramBindings.<field>"}` — a larger sampler array is a counted refusal, not a truncation. **THE RECORD ALWAYS DECLARES THREE TAILS**, empty ones included: the tails are positional and the third one's offset is derived from the first two, so a record that declared "one tail" when only the block bindings were present would put the same bytes at a different offset than one that declared three, and the two halves of the wire would disagree about where the overrides start. The wire codec's `WireRecordLayout` grew from two tails to three for it (`MG_Remote/Wire/PipeWireCodec.h`), and the override names are walked through `CheckHostSpanIsHonest` in the decode arm rather than by the encoder's blanket second-tail pass — they are MEMBERS of a 40-byte element, and reading that tail as bare spans would see one span and a quarter of garbage. Emitted BEFORE `create_shader_state` at the same validate point (a rebuild inside the verb must already see the bindings). Dirty rule: bit `NewShaderBindings` (`Tracker.h:364-371, 419-448`) already mixes the three counters; the emitter latches on `(Cso, backendStateVersion, blockBindingVersion)`. A re-issued create clears the record's tails (`GlobalConstants`' rule, `PipeApply.cpp:2903-2909`). | all three counts 0 = "defaults" | `MGPipeApplySetProgramBindings`, the program twin's sync and memo keys |
| **`MGPProgramDesc::LinkStatus`** (a `Uint8`; **the descriptor grows 192 → 200**, `PipeCatalogueTest` pins the new size) | **THE DRAFT SAID "in the descriptor's existing pad, size unchanged" AND THAT WAS WRONG.** `MGPProgramDesc`'s four trailing `Uint8`s end at offset 24 and `MGPBlobRef` is 8-aligned, so the fixed head is exactly full and a fifth byte costs the alignment slack behind it: 20 bytes of handle and counters + 5 bytes of flags, padded to 32, plus 168 of blob refs. It is paid once, on a record that fires at most once per link, and it is confined to the push build — this payload is only ever instantiated under `MOBILEGL_PIPE_PUSH` (`PipeApply.cpp`, `PipeRoute.cpp` and the emitters are all push-only sources), which is why G1 does not see it. 1 = the link the archive describes succeeded. `create_shader_state` is re-issued on the same handle at every link that changes `GetLinkVersion` (today's rule) and the record's `Serial` moves with it; a failed link re-issues with `LinkStatus = 0` and empty blobs IFF the frontend reports the program unlinked afterwards (§9 ruling 9 pins which). A draw on a `LinkStatus == 0` program DECLINES on the server, as the frontend raises `INVALID_OPERATION`. | not linked | the program twin (`SyncCurrentProgram`) |
| **`MGPProgramDesc::Spirv[6]` / `Reflection` blobs are NON-EMPTY** on a live wire | The client encodes the link artefacts once per link with `ProgramArtifactsCodec` into `SEG_STAGE`; the applier decodes into RECORD-OWNED `LinkArtifacts`/`SpirvArtifacts` and the companion pointers (`ProgramEmit.h:230-260`) are deleted under a transport. `Size 0` with no companion is the existing trip wire `Fatal{ProtocolCorruption}` (`PipeApply.cpp:2858-2871`). A `SharedPtr` pin of the client's artefacts was considered and REFUSED: rule B, and `Link()` replaces them in place (`ProgramObject.cpp:348-356`). | — | the program twin's rebuild |
| **`MGPImageView::Access`** | 0 = `GL_READ_ONLY`, 1 = `GL_WRITE_ONLY`, 2 = `GL_READ_WRITE` — the encoding `MGPipeEncodeImageAccess` already writes (`MG_Impl/Pipe/ImageEmit.h`). The NUMBERS move to `MG_Pipe/MGPipeValueTypes.h` as `enum class MGPipeImageAccess : Uint8` with `MGPipeDecodeImageAccess` / `MGPipeImageAccessIsValid` / `MGPipeImageAccessReads` / `MGPipeImageAccessWrites` beside them, and the client encode NAMES those enumerators instead of literal 0/1/2 — one table, both roles, moved rather than duplicated. The encode also stops being a private static so the unit case can reach both halves. The server comment at `DirectGLES.cpp:2662-2669` is deleted as stale (fb). Any other value is `Fatal{ProtocolCorruption, "ImageView.Access"}` AT THE READER: the value header may not log, so it answers the question and the caller owns the refusal. | read-only | `SyncImageTextureBinding` |
| **`kMGPipeSubsystemBufferBindings = 1ull << 13`** (`MG_Pipe/MGPipe.h`, bits 14..62 now reserved) | Owns dirty bits 15/16/17 (`NewConstBuffers/NewShaderBuffers/NewSoTargets`, `Tracker.h:97-99`) and the `SetShaderBuffers` emitter. Caps-required under a transport like bits 7–12; the default push mask becomes `kMGPipeSubsystemsMigratedAtP5e` = `0x3fff`, and `0x1fff` survives as P5e's "everything P4a had and nothing of mine" control. Bit 13 requires bit 7 (a range names a Buffer handle). `NewSoTargets` rides the bit even though `set_stream_output_targets` stays unemitted for the whole of P5e: the bit answers "which A/B switch owns this family's legacy arm", and an operator clearing it has to get the whole binding-point family's frontend walk back rather than two thirds of it. `TrackerTest.cpp` is rewritten, not deleted, and now also pins that the phase constant is the WHOLE dirty-bit set — after P5e every bit names a subsystem. | bit clear = the frontend walk (monolith / A/B only) | `MGPipeSubsystemForDirty`, `SubsystemForEmitter` |
| **`SetHashSuppressor` slots** (`MG_Impl/Pipe/SetHashSuppressor.h`) | `SetShaderBuffers` becomes THREE slots (`SetShaderBuffersUniform` / `SetShaderBuffersShaderStorage` / `SetShaderBuffersAtomicCounter`) — one per Class, so an emission of one class never cancels another's; `SetProgramBindings` gets one. `SetStreamOutputTargets` stays "P4b" (unemitted). | — | the emitters |
| **`IpcTable::RunAhead`** = `MOBILEGL_IPC_RUN_AHEAD` (default 1, range 0..1); **`IpcTable::PresentCredit`** = `MOBILEGL_IPC_PRESENT_CREDIT` (default **1**, range 1..8) | Parsed unconditionally like every IPC knob (`ConfigLoader.cpp:356-403`), and both are in the IPC log line, because a knob that silently means nothing on one arm of an A/B is how an A/B stops being one. `PipeVerify` forces `RunAhead = 0` beside `BatchWaits = 0`. `RunAhead=1` on a server without the caps bit logs once "run-ahead requested, server does not publish kCapRunAheadApply" and runs lockstep. The stats line gains `credit-waits`. | `RunAhead=0` is the A/B control: identical server code, only the client's wait differs | `RunAheadArmed`, `EmitPresent` |
| **`BufferState` bind-point generations** (client-only, `#if MOBILEGL_PIPE_PUSH`) | One `Uint64` per `BufferBindPointTargets` entry beside `NoteBufferChanged` (`BufferState.h:69-75`), bumped by `glBindBufferBase/Range` (`GL_Buffer.cpp:1501-1520, 1624-1650`) and `SetNamedTransformFeedbackBinding`. Not a wire field; listed because it is the emit rule's input and the pull build's object must not resize (the licence `TextureState::NoteImageUnitTouched` took at P5d r3). | — | bits 15/16/17's shutters |

---

## §2 ra — the wait rule

### 2.1 The barriered predicate (one function, two callers)

```
MGPipeBarriered(op, payload, st) =
    MGPipeWaitClassFor(op) != kWaitNone                                  // static column, §2.2
 || (MGPipeCallClassFor(op) == kCtxVerb && st.IsTransformFeedbackActive) // (i) XFB stays lockstep
 || (op == DrawVbo && (payload.Flags & kDrawClientArrays))               // (ii) client arrays
```
`st.IsTransformFeedbackActive` is `MGPContextValues`' value (`MGPipeTypes.h:1698`): on the client
it is `ctx.IsTransformFeedbackActive()` at emit, on the server the applied value — `set_context_values`
precedes the verb on the ring, so the two agree. **c0e LANDED THE FUNCTION, all three clauses, in
`MG_Pipe/PipeApply.cpp`**, and gave `MGPipeApplierState` its own `IsTransformFeedbackActive` mirror
written by `MGPipeApplySetContextValues`: the predicate has to be computable on the server from
APPLIER state, and `gPipeInputs` is the block a run-ahead client stops filling, so reading the
answer out of there would be reading the very memory rule F exists to stop it reading. `payload`
may be null, which makes clause 3 fall through — a caller holding an opcode but not yet the bytes
must not be told a draw is unbarriered on that account. Escalation (ii) is a refusal under run-ahead
(§5.1), so on a run-ahead server it never reaches the sink; it is listed so the predicate is total.
**No other runtime escalation exists**; adding one is an integrator ruling and a row here.

### 2.2 The static column, row by row

| class | rows | why |
|---|---|---|
| `kWaitReply` | every `kReplySlot` row (`PipeCalls.def:115-202, 206, 221`): GetCaps, ResourceCreate/Respecify, MapPersistent, FenceStatus/Wait, QueryAvailable/Result/Timestamp, SetTextureParams, ResourceSubData (texture half, §2.5), ResourceReadback, GetTextureImage, ReadPixels | the answer is not derivable (R-5) |
| `kWaitPresent` | `Present` | the credit, §2.4 |
| `kWaitApplied` | `ApplierReset`; `GenerateMipmap` (`kCtxObject`, waits today for the reason at `ClientSession.cpp:900-914` — until tx2 lands its `VerbMipRes` arm, then `kWaitNone`); `SetStorageBlockBinding` (`kCtxVerb`, resolves through `GetProgramObject(GlName)`, `PipeApplier.cpp:805-808`; trailing: pg resolves through `MGPStorageBlockBinding::ShaderCso` and flips it); `BeginStreamOutput`, `EndStreamOutput`, `PauseStreamOutput`, `ResumeStreamOutput`, `BindStreamOutput` (§5.7); `CopyFramebufferToTexture` (the CopyTex endpoint still syncs the frontend texture, `DirectGLES.cpp:9714-9735`; trailing, tx2) | each still pulls a row or probes; the wait keeps P5C's semantics for it |
| `kWaitNone` | `DrawVbo`, `LaunchGrid`, `Clear`, `Blit`, `MemoryBarrier`, `Flush`, `SetSwapInterval`, `BindShaderImage`, `PatchParameter`; every `kCtxState` / `kCtxCso` / `kCtxObject` row without a reply (incl. `ObjectDeath`, `ResourceSubData` buffer half); `ResourceDestroy`, `UnmapPersistent`, `FenceCreate/Destroy`, `FenceWaitServer`; `QueryCreate/Begin/End/Destroy/Counter` | rule F holds for their sinks after §4/§5 |

The column is inert until the integration commit publishes the caps bit (§6.2), after vi/sb/pg/tx2/fb.

### 2.3 `EmitAndWaitTails` after `PublishAndNotify` (`ClientSession.cpp:877`)

```
if (ownsReplySlot)              -> WaitForAppliedBudget + DrainEventRing + ReadReply   (unchanged)
else if (RunAheadArmed())
    switch (class)  kWaitNone:    return seq;
                    kWaitApplied: WaitForAppliedBudget + DrainEventRing + ReleaseFillPins(); return seq;
                    kWaitPresent: return seq;   // the credit wait ran BEFORE encode, §2.4
else                            -> today's :915-959 shape, byte for byte (lockstep, Magma, verify)
```
`ReleaseFillPins()` clears the four O-class `SharedPtr` rows of `gPipeInputs` (`PipeInputs.h:801-809`)
on the GL thread once the barriered apply returned — under run-ahead the next fill may be a frame
away and a pinned VAO/program must not make the apply thread its last owner. `VerbBarrier==0`
keeps its lockstep meaning (nothing but reply rows waits); under run-ahead it removes the
`kWaitApplied` waits too and is the run-ahead NEGATIVE CONTROL: expected red is
`Fatal{UnmigratedPipeInput, ...}` at the first barriered row's stale pull.

### 2.4 Present pacing

`EmitPresent` (`EmitTables.cpp:977-1053`): `BeforeReadOnlyVerb(); if (m_presentsSent >= PresentCredit)
WaitForPresentAck(m_presentsSent + 1 - PresentCredit, 30 s)` — `ShutDown` returns, `TimedOut` is
`Fatal{PresentCreditTimeout}`; then `DrainEventRing`, encode, publish, `PumpControlPlane`. Credit 1
= the client publishes frame N+1 while the server applies and swaps frame N: one frame of overlap,
at most one frame of added latency. Budget at ~850 draws + ~0.36 MB pmap/frame: SEG_CMD ~70 KB +
set/CSO records per frame against 8 MiB, SEG_STAGE ~1 MB/frame against 32 MiB, SEG_REPLY unchanged.
The credit, never the bytes, is the steady-state backpressure; `WaitForCmdSpace` and the stage bell
(`PipeWireCodec.cpp:836`) are the safety net and both drain events on exit (§2.6).

### 2.5 Drains and forced waits

Drain points: every wait exit in `ClientSession` (reply, applied, present-ack, cmd-space, stage
bell), the four EGL RPC returns (`BackendObject_Remote.cpp:200-245`), `Stop`. Forced waits:
`MapBuffer(READ)` / `GetBufferSubData` / a `CopyBufferSubData` source → `SyncGpuWrites` (reply
row, unchanged); `glFinish` → `WaitForApplied(LastPublishedSeq)` + drain (a new client `Finish`
slot; `Flush`/`Finish` stay no-ops on the wire, ARCHITECTURE:415); every `Server*` EGL forwarder
waits `WaitForApplied(LastPublishedSeq)` BEFORE the RPC (the mailbox is pumped between drain
batches, `ServerLoop.cpp:385-390`, so a make-current would otherwise land between two run-ahead
records); `ServerSwapEGLBuffers` is not on that list — present is the swap. **`glGetError`
relaxation:** a run-ahead verb's `kEventGlError` is observed at the next drain point, i.e. at most
one present credit later than the call after it; P5C §4.2 already assigns ordering to P9 and P5e
names the window. **`resource_subdata`:** the BUFFER half passes `wantReply = false` through
`MGPipeRouteResourceSubData` (the Bool is discarded today, `PipeFill.cpp:839`; the server still
posts, the slot is never read — legal, `ReplySlot.h:16, 105`), which is what makes the persistent-map
push (`PersistentMapTracker.cpp:735, 850`) fire-and-forget; the TEXTURE half keeps its wait because
`DrainTextureSubData` clears the level's dirty flag on the accepted reply (D-D5; trailing).

### 2.6 Event-ring flow control (ARCHITECTURE §11.7 made real)

On a run-ahead server `Reserve == nullptr` is NOT `Fatal{EventRingOverflow}`: the producer latches
`eventRingFull` (exists, `EventRing.h:135-143`), publishes, rings, and `ApplyThreadMain` parks at
the record boundary with `eventRingFull == 0` added to `ready` (`ServerLoop.cpp:380-383`). The
client re-checks `m_events.RingIsFull()` at every park exit and drains before re-parking. Deadlock
argument: the server blocks only on the event ring; the client blocks only on watermarks the
server advances; every client wait drains; so at most one side is parked at any instant. The
lockstep arm keeps the Fatal (P5C §4.4).

### 2.7 Object lifetime under run-ahead

The client frees a slot only after its death record went out (`PipeFill.cpp:1623-1626`), the ring
is in order, every record naming `{s,g}` precedes `destroy{s,g}` which precedes `create{s,g+1}`,
and every server twin table Gen-checks (§4.3). **No wait is added at any delete.** Persistent maps:
`Forget()` (`PersistentMapTracker.cpp:599-606`) drops tracker state only; pushed bytes were copied
into SEG_STAGE at `StageOptional` and reclaim waits on `retiredSeq`, so a map released with blocks
in flight is safe. The P5d deferred-destroy queue (`PipeMutation.h:216-236`) STAYS LIVE: a
barriered record's apply may still pin a frontend object (its fill's `SharedPtr` rows, XFB
`targets`), so the apply thread can still be a last owner. Under `RunAheadArmed` an enqueue from an
UNBARRIERED apply is a finding: `MGLOG_E_ONCE` with the kind, Fatal under strict.

---

## §3 `gPipeInputs` — server-role memory for unbarriered records

1. **The client fills only for barriered records.** In `MGPipeValidateForVerb`
   (`PipeFill.cpp:2962-3306`), when `RunAheadArmed()` and `!MGPipeBarriered(...)`: step 2 (tracker
   walk) and step 3 (emitters) run unchanged; `:2989` (`++CurrentVerbSerial`), `:2999-3001`
   (`SetVerb/SetIdentity`), step 4 (`:3252-3270`, `CopyField` + `FilledGen`) and `MGPipeLeaveVerb`'s
   writes are SKIPPED; `MGPipeNoteFrontendMutation` returns at `:554`. The residual-value trip
   wire (`:3284-3294`) still emits — it compares on the server against the applier mirror.
   `RunAhead=0` on a caps-publishing server uses the SAME fill decision (the A/B changes one
   variable: the wait). `PipeVerify` forces lockstep, a fill for every verb, and every record
   barriered (both roles read the in-process flag; the comparator `:3304` is inproc-only).
2. **The server's stamp is server-private.** `MGPipeServerStampVerbBoundary` writes
   `m_filled/m_currentVerb/m_serverStampedVerb` into applier-owned storage, never into the shared
   block; `MGPipeServerClearVerbBoundary()` from the GL thread (`:2997`) is DELETED under a
   transport. For a barriered record the server reads the client's fill (the client is parked) and
   its own stamp; for an unbarriered one it does not touch the block. `SetIdentity` (`:3001`) moves
   to `ApplyOne`, once per session. This removes the E note's hazard (b.4).
3. **The detector.** `MGPipeInputUnfreshRead` (`PipeInputs.cpp:191-224`) under an unbarriered
   record takes `StrictBarrierPullFatal` (`:60-66`) regardless of `StrictErrors`; the seven sticky
   forwards (`MGPipeStickyForwardPull`, `:251-257`) the same; `CountBarrierPull` stays for barriered
   records. This is what turns `integration-split-strict` into a hard green lane (§7).
4. `VertexAttribDefaultsOf` (`PipeFill.cpp:84-86`) diffs against the emitter's own last header
   (`g_attribDefaultLastHeader`, `:2949`), never against the block.
5. `RefusePipeInputsTouchWhileApplierOwnsIt` (`ClientSession.cpp:1015-1056`) is re-derived: the
   `BatchWaits` early return at `:1037` goes; under `RunAheadArmed` any GL-thread write to the block
   outside a barriered fill is `Fatal{RoleViolation, "gPipeInputs"}` by name.
   `Fatal{BarrierViolation}` (`:840`) stays for `BatchWaits==0 && !RunAhead`.

---

## §4 id — handle-keyed twins

### 4.1 The rule
With a live wire, every twin on the apply thread is resolved BY THE HANDLE the applier state or
the record carries, never by a frontend object: `GetOrCreate(MGPipeHandle)` (`SlotTables.h:294-342`),
`FindByHandle` (`:420-426`), `ReleaseByHandle` (`:394-408`). `HandleOf(stateObj)`,
`Find(StateObject*)`, `GetOrCreate(const StatePtr&)`, `NoteStateForHandle/StateForHandle` and
`Entry::stateRef` survive as the MONOLITH-GLUE half (§5.8) and are unreachable from an
unbarriered apply. The twin sync bodies take `(MGPipeHandle, const <Record>&)` and store
`m_handle` (the `m_pushedSyncHandle` pattern, `Managers.h:1857`); the object parameter is deleted
from the transport overload, not defaulted to null. The E note's "content-addressed VAO CSO" is
corrected: every kind mints its slot off the object's own lifetime id (`VertexInputEmit.h:170-172`,
`TextureEmit.h:422`, `FramebufferEmit.h:257`, `ProgramEmit.h:411-422`), so the rekey is a
replacement of the lookup ARGUMENT, not of the table.

### 4.2 Per kind
| kind / table | key on the server | the record that names the handle before any verb reads it | twin created | twin dies |
|---|---|---|---|---|
| Buffer `g_backendBufferResources` (already handle-keyed) | `Res` from `VertexBuffers[]`, `IndexBuffer`, `BoundShaderBuffers[c][i]`, `Desc.BufferForTexBuffer`, `VerbIndirect*` | `resource_create/respecify` | `EnsureBufferResourceForHandle(nullptr, h)` | `resource_destroy` |
| VAO `g_backendVertexArrayObjects` | `st.BoundVertexElements` (`PipeApply.h:594`; one CSO per VAO, re-created on the same handle when the config moves) | `create/bind_vertex_elements` | first `ResolveVaoTwin(handle)` at a draw | `object_death{VertexElementsCso}` / `delete_vertex_elements` |
| Texture `g_backendTextureObjects` | `BoundSamplerViews[u].Texture`, `BoundShaderImages[u].Res`, `FramebufferRecords[h].State.{Color[i],Depth,Stencil}.Res`, `VerbMipRes`, `VerbCopyTexDst`, `MGPCopyImage` src/dst | `resource_create/respecify` (Texture) | first `SyncTextureToBackendByHandle(h)` | `object_death{Texture}` / `resource_destroy` |
| Renderbuffer `g_backendRenderbufferObjects` | `MGPSurface.Res` with `Kind == Renderbuffer` | `resource_create/respecify` (Renderbuffer) | first attach sync | `object_death{Renderbuffer}` |
| Framebuffer `g_backendFramebufferObjects` | `st.BoundFramebuffer[t]`, `record.Fbo`, `VerbBlitReadFbo/DrawFbo`; `kMGPipeDefaultFramebuffer` is never a twin | `set_framebuffer_state` | `GetOrCreateByHandle(record.Fbo)` (already, `DirectGLES.cpp:3197`) | `object_death{Framebuffer}` |
| Program `g_backendProgramObjects` | `st.DrawProgram` / `DispatchProgram` / `BoundShaderCso`; composites at the band | `create_shader_state` (+ `set_program_bindings`) | first `SyncCurrentProgram(handle)` | `object_death{ShaderCso}` / `delete_shader_state` |
| Sampler `g_backendSamplerObjects` | the CSO family ONLY: `st.BoundSamplerStates[u]` or `TextureResources[h].Params.BuiltinSampler` | `create_sampler_state` | `ResolveSamplerCsoTwin` (already, `Managers.cpp:13457-13486`) | `object_death{SamplerCso}` |
| SamplerView `g_backendSamplerViews` | `BoundSamplerViews[u].View` (no draw-path resolver today; unchanged) | `create_sampler_view` | — | `object_death{SamplerViewCso}` |

The identity family of samplers (`ResolveUnitSamplerBackend` `DirectGLES.cpp:4985-5026`, the mint
at `:5623-5630`, `Managers.cpp:13264-13280`) is deleted under a transport; a unit whose
`BoundSamplerStates[u]` is null while the frontend held a sampler is a missing record and §5.3's
window rule makes it unrepresentable. The creation moment is LAZY everywhere (no twin at
`create_*`); the applier's Gen check refuses a bind or verb naming an unknown or stale handle
(`PipeApply.cpp:2266, 2549, 2625, 2683, 2896`), so a twin is never minted for an object the ring has
not introduced. `applier_reset` touches neither twins nor object records (`PipeApply.h:800-832`).

### 4.3 ABA
`Gen` moves only on reuse (`SlotAllocator.cpp:410-419`). At the server every record naming `{s,g}`
is applied before `object_death{s,g}`, which is applied before `create{s,g+1}`. `GetOrCreate({s,g+1})`
on a live `{s,g}` entry resets it (`SlotTables.h:338`); `FindByHandle({s,g})` after the recycle
answers null (`:424`); a backward `{s,g-1}` is refused (`:331`). Even with a death skipped (an
object that never crossed emits none, `WireTables.cpp:595-596`) the forward-Gen reset answers.
Twin memos key on `{slot,gen}` plus server serials, never on a pointer. `BackendSlotTable` gains
the composite band (`m_band`, indexed `slot - base`, as `SlotAllocator.h:144` / `PipeApply.h:511`)
so a pipeline program (slot >= 983040) does not grow the ordinary table to `kMaxHandleSlot` entries
— a P5e prerequisite, not a follow-up, because the rekey makes it the ordinary path.

### 4.4 The scopes and the guard
`MGPipeRefuseAllocatorFromApplyThread` (`SlotAllocator.cpp:22-38`) exempts a probe iff a scope is
active AND `MGPipeApplierCurrentRecordIsBarriered()` (server-private, set by `ApplyOne` from
§2.1; `true` for every record on a non-run-ahead server, §6). **c0e landed the storage and the
writer** (`MGPipeApplierSetCurrentRecordBarriered`, `MG_Pipe/PipeApply.cpp`) with `true` as the
default — the only seam in this phase whose body is implemented rather than aborting, because
there the safe answer and the landed lockstep answer are the same answer, and an abort would take
down the monolith and the lockstep split alike. id owns the `ApplyOne` call site. **`MGPipeFrontendKeyedRegistryScope`
is deleted at its draw-path sites** — `DirectGLES.cpp:1609, 1734, 2588, 3303, 4236, 4329, 4585, 4926,
4993, 5328, 5689, 6729, 6775, 9695` and `Managers.cpp:256, 2938 (draw-path callers), 4544, 5882,
7087, 8481, 8717, 9256, 9310, 9709, 12949, 13273, 13570` — with the frontend probes they wrap; the
class survives only around the barriered-row sites (`DirectGLES.cpp:6166, 8837, 9161, 9216, 10029,
11884`, the verify arms `Managers.cpp:10382, 10403`) with their P8/P9 phases re-annotated.
`MGPipeReverseAnnouncementScope` is renamed `MagmaP7AllocatorDebtScope`, compiled for the Magma
sites only (`VulkanRenderer.cpp:4554, 4634, 8986`, `ResourceTracker.h:723`), exempting only when
the server backend is DirectVulkan. The renderbuffer cross-check `Managers.cpp:9709-9720` (not
transport-gated today, unlike `:9628-9632`) is deleted by the rekey, not gated. The NoSession
server-only fixture hop (`Managers.cpp:224-246`) is deleted; such a fixture's twins die at Stop.

---

## §5 Per family — the clean gate, the records, the windows

### 5.1 VAO / vertex input / index (vi)
Twin from `st.BoundVertexElements`; memo key `{BoundVertexElements, rec->ContentSerial,
VertexBuffersSerial, IndexBufferSerial}` (all server-owned); the buffer walk is over
`st.VertexBuffers[Start..+Count).Res` (dedupe on `{slot,gen}`) with
`EnsureBufferResourceForHandle(nullptr, Res)` and `IsBufferDrawCleanByHandle(h, res, nullptr)`;
`ResolvedDrawBuffers::Entry::frontend` leaves the split arm; the IBO arm reads `st.IndexBuffer.Res`
+ `IndexBufferSerial` (also for the restart substitution, `DirectGLES.cpp:6447-6461`);
`SyncCurrentVertexAttributeValues` keys on `{elementsHandle, ContentSerial, activeMask}` and reads
`rec->Attributes[i].Enabled`; the dead `GetConfigVersion` read at `:4775` is deleted. **Client-memory
vertex arrays are NOT refused under split today** (only `CLIENT_INDICES/CLIENT_COMMANDS/
UNBOUND_PARAMETER`, `EmitTables.cpp:651-685`; the server dereferences `attrib.Offset` as a raw
pointer, `Managers.cpp:5382`): under `RunAheadArmed` a draw with `kDrawClientArrays` is
`Fatal{UnmigratedVerb, "DrawArrays+CLIENT_ARRAYS"}` raised on the GL thread in `EmitDrawRecord`'s
array arm; the lockstep arm is unchanged; the staged form (a per-attribute `{BindingIndex,
MGHostSpan}` tail like `kDrawHasUserIndices`) is P8's. Two pins by test: `CurrentBufferMutationEpoch()`
(`Managers.cpp:794`) is apply-thread-only under a transport; the VAO twin key is 1:1 only while the
vertex-elements CSO stays identity-addressed.

### 5.2 Textures (tx2)
Every draw-path entry is `SyncTextureToBackendByHandle(h, imageBindable)`: record first, twin by
`GetOrCreateByHandle`, three syncs `(h, *rec)`; the by-value twin copy + second `Find`
(`DirectGLES.cpp:1745-1755, 1781-1805`) go with the map arm. Clean = `m_isInitialized &&
m_syncedResourceSerial == rec->Serial && rec->PendingUploads.empty() && m_syncedParamsSerial ==
rec->ParamsSerial && !rec->Params.ForceResync && !m_forceTextureParamsResync &&
m_syncedBuiltinSampler == rec->Params.BuiltinSampler && m_syncedBuiltinSamplerSerial ==
SamplerCso(rec->Params.BuiltinSampler)->Serial && !rec->Params.SamplerResync && !m_forceSamplerResync
&& rec->Desc.StorageKind == Mipmap` — no new state. The four in-body live reads (`GetTarget` at
`Managers.cpp:8876-8877, 8620-8621`; `IsTextureView` `:7060`; the TexBuffer backing `:8223-8231`)
read `Desc.Target / Desc.ViewOf / Desc.BufferForTexBuffer`; `GetExternalIndex` in logs becomes
`Desc.GlNameForDiag`. The unit work list is `{Res, backend}` keyed `(ContextSerial,
SamplerViewsSerial, SamplerViewStart, SamplerViewCount, g_backendContextGeneration)`;
`PairingsIntact` is deleted; `ResolveAndBindUnitTextures` is one pass over the window with the
target from `SamplerViewCsos[View].View.Target`, then an unbind of every target the window did not
claim driven by `g_boundTexturesCache`; the alias arbiter dies (the client arbitrated,
`SamplerEmit.h:751-757`). **Named delta (P5e-5):** the unit list syncs the program-resolved texture
per unit, not every slot of every touched unit; coverage is unchanged (views ∪ image units ∪
attachment lists ∪ the waited texture ops). `RequireImageBindableStorage`'s re-dirty arm
(`Managers.cpp:5930-5962`, already aborting three frames deep) becomes a NAMED refusal at its
entry under a transport, `Fatal{UnmigratedEmulation, "image-bindable-redirty"}`; `ImageBindableHint`
is the prevention, P9 owns the pull. `GenerateMipmap(target)` under a transport resolves the
texture from `VerbMipRes` (as `EnsureGenerateMipmapStorageAllocated` already does, `DirectGLES.cpp:8736`),
never the active unit (`:9563-9566`), and its row flips to `kWaitNone`. `m_force*Resync` are
server-set and never cleared by the wire; the wire bits are never cleared by the server (D-E2, now
load-bearing for a gate). The pinned guard list (`MipmapStorage.cpp:39-51`, `RemoteClientTest.cpp:1938-1978`)
gains the six shape accessors it exempts today plus `GetSamplerObject/GetTextureParamsVersion/GetContentVersion`.

### 5.3 Samplers and the two unit windows (tx2; the rule binds the emitters)
The effective sampler of unit u is `st.BoundSamplerStates[u]` if non-null else
`TextureResources[view.Texture].Params.BuiltinSampler`; its values are the CSO record's
(`MGPipeValueTypes.h:462-486`); the raw-depth-fetch substitution stays on the server
(ARCHITECTURE §5.5); `texture2D->GetFormat()` becomes `SamplerViewCsos[View].View.InternalFormat`.
**Window rule (A8 closed):** `set_sampler_views` and `bind_sampler_states` MUST carry `Start == 0`
and `Count >= MaxTouchedTextureUnit + 1` (0 when none touched), as `SamplerEmit.h:733-737` emits
today; at every draw/dispatch apply the sink checks the two counts against the applied
`MGPContextValues::MaxTouchedTextureUnit` and a narrower window is `Fatal{ProtocolCorruption,
"SetSamplerViews.Count"}` / `"BindSamplerStates.Count"` — a server-side re-derivation is refused.
The `BindCurrentTextures` memo key is `{DrawProgram.{Slot,Gen}, ShaderCso.Serial, BindingsSerial,
SamplerViewsSerial}` (ruling 6). The decline arms `DirectGLES.cpp:5152-5169` and `:2073-2098` become
named refusals under a live wire; `CaptureUnitBindings/UnitBindingsUnchanged/g_observedUnitBindings`
(`:1867-1957`) are monolith-only text.

### 5.4 Framebuffers, attachments, images (fb)
`SyncCurrentFBO` is `SyncCurrentFBOByRecord` only; a decline under a transport is
`Fatal{UnmigratedPipeInput, "GetFramebufferBindingSlot@<verb>"}`; `FramebufferRecordMatchesBinding`
(`:3017-3033`) is monolith-only. `BackendFramebufferObject::SyncToBackend(record, target)`: the
attachment walk is over `Color[0..7] + Depth + Stencil` (`Color[0]` alone when `IsDefault`) — the
record's 11 surfaces ARE the point set (the emitter refuses a point at or above the wire width,
`FramebufferEmit.h:489-501`; FRONT/BACK tokens exist only on the default framebuffer); the empty
point is `Kind == None || Res null` for the detach (`Managers.cpp:10310`) and the N-6 refusal
(`:9601-9606`) alike; `m_syncedFrontendAttachmentVersions` is replaced by the record hash;
`g_attachmentBackendIdGeneration` stays. `SyncAttachmentSurface(target, surface, attachment)` takes
no object; storage through `SyncMipmapsToBackendByHandle` / RBO `SyncToBackendByHandle(surface.Res)`.
The two attachment texture lists key `(record.Fbo, ContentHash, ContextSerial,
g_backendContextGeneration)` with entries `{Res, backend}` and `PairingsIntact` = `entry.Res ==
surface.Res`. `ForceBindCurrentFBO` is handle-only and stamps nothing frontend (the stamps at
`:4734-4738` and the legacy memo are skipped together under the transport test). The named blit
takes two `const MGPFramebufferState&` (draw buffers, `ReadSurface`, `Kind`, layer/level/layered/
format from the surface, `Samples` from the record, `kMGPipeDefaultFramebuffer` for the default)
and retires `StateForHandle`; the bound arm reads `FramebufferRecordFor(st.BoundFramebuffer[t])`.
`ScopedDetachedTextureFramebufferAttachments` walks a reverse index texture → FBO slots maintained
at `set_framebuffer_state` apply. `MGPFramebufferState::Complete` is never a "may I draw" gate.
`SyncReadFramebufferTextureAttachments` is not gated on the framebuffer bit (`:2227-2230`), so its
retirement moves the object A/B baseline in BOTH arms (re-baselined, not a regression).
**Default-framebuffer resize:** the surface-changed consumer's `AllocateStorage` (`ClientSession.cpp:210-221`)
reaches `MGP_NOTE_AGGREGATE(FramebufferAttachment)` (`TextureObject.cpp:84-102`), so bit
`NewFramebuffer` (`Tracker.h:594`) re-emits the record — verified at this head, pinned by a test in fb.
Images: `SyncImageTextureBinding(const MGPImageView&)`: twin by `FindByHandle(Res)`, `Access`
decoded, target and storage format off the TWIN (no new wire field; `MGPImageView` is 24 B, no
pad); `ResolveShaderImageRecord`'s identity test and I5 seam log go. The sweep gate is
`(ShaderImagesSerial, TextureShutterSerial, ContextSerial, g_backendContextGeneration)` — never a
backend re-mint counter, so the "re-minted inside the sweep" property (`:2890-2901`) holds; the walk
stays the UNION of the record window and `g_imageUnitHighWaterMark` (`:2811-2821`), never narrowed
to the window. `MarkWritableImageBufferTexturesGpuWritten` is DELETED under a transport (the client
owns the GPU-write set, `GpuWritePending.h:9-55`, `:83-115`); `WritableMask` survives as the record
of which points P9's narrowing will name.

### 5.5 Programs (pg)
Twin from the ShaderCso handle; `SyncToBackend(h, const MGPipeShaderCsoRecord&)` answers every
reflection question from the record-owned archive and the bindings tails (`GetLinkedShaderSnapshot`
`:12049` is a debug log, deleted on this arm). The nine-clause clean condition (`DirectGLES.cpp:4437-4507`)
keeps its clause count and moves its inputs: `record.Serial` vs `m_syncedShaderCsoSerial` (exists,
stamped at `Managers.cpp:12944-12966`, unread today — P5e is its first reader), `BindingsSerial`,
`record.Signature`, `Desc.SpirvStatus`, `Desc.LinkStatus`; `ImageUnitFormatsStillMatch` and the
patch clauses are fb's / value rows. The stash `g_currentDrawFrontendProgram` (`:4097-4098`)
becomes `{MGPipeHandle, twin*}` compared against `st.DrawProgram`; `PrepareForCompute` reads
`st.DispatchProgram`. The sampler-pass memo (`:5522`) keys on `BindingsSerial`;
`sampledTargetForUnit` reads the sampler-unit tail and the archive's uniform types. **`MapUBO()`
has no run-ahead answer:** when `ResolveGlobalConstantsRecord` declines under a transport it is
`Fatal{UnmigratedVerb, "set_global_constants"}`, not a torn upload. `GetProgramObject/
ValidateProgramName` keep their P9 rows: `GetBackendProgramId(name)` (`:6151`) is a monolith entry;
`ShaderStorageBlockBinding` (`:10020`) is reached from the barriered `set_storage_block_binding`
(§2.2) until pg's trailing item resolves it by `MGPStorageBlockBinding::ShaderCso`. Codec
completeness: `ProgramArtifactsCodec.cpp:242`'s skipped member is named and either encoded or
proven unread by the twin before pg claims "every reflection question is answered".

### 5.6 Buffer binding points (sb)
Applier: `BoundShaderBuffers[3][84]`, `ShaderBufferStart[3]` / `ShaderBufferCount[3]` /
`ShaderBufferWritableMask[3]` (the draft said bare `Start/Count/WritableMask`; they sit in
`MGPipeApplierState` beside `SamplerViewStart`/`SamplerViewCount` and take the same shape), ONE
`ShaderBuffersSerial` advanced on every applied record and ADVANCED (never zeroed) by
`MGPipeApplierReset` (`VertexBuffersSerial`'s rule, `PipeApply.h`); the emitter's latch resets with
it (`PipeFill.cpp:3129-3143`'s list). c0e declares all of it, with
`MGPipeApplySetShaderBuffers` declared and aborting by name until sb writes the body.
**`WritableMask` IS A `Uint32` AND THE WINDOW IS 84** (`MGPShaderBuffers::WritableMask`, unchanged
since P4a): the mask can only describe the first 32 points of a class, so sb either narrows the
window it sets bits for, widens the field, or states the bound — an integrator ruling sb must ask
for rather than pick, because the field is on the wire. `SyncBufferBindingPointsByRecord(Class, glTarget)`: per entry
`EnsureBufferResourceForHandle(nullptr, Res)`; whole-vs-range is `Offset == 0 && Size ==
kMGPipeWholeBuffer`; clean = `resource->syncedChangeSerial` vs the resource record's `Serial`
(`Managers.cpp:3151`) plus `ShaderBuffersSerial`; no twin key is introduced (a point names an object,
it is not one). The UBO loop (`DirectGLES.cpp:5447-5497`) reads `BoundShaderBuffers[Uniform][binding]`
where `binding` is the program's block binding (pg's tail); `binding >= Count` = nothing bound;
`GetBufferResource(obj)` `:5472` becomes `FindBufferResourceForHandle`. `SyncAtomicCounterBuffers`
reads `Count[AtomicCounter]`; `GetBufferBindingPointCount`'s sticky forward becomes FATAL under
split (`InvalidateCompileEnv`'s shape, `FieldOwnership.def:163-165`). The three backend GPU-write
marks (`DirectGLES.cpp:606, 658, 2775`) are deleted under a transport in ONE edit with fb's.
**Client shutter (the P4b hole, verified: bits 15/16/17 read the content aggregate, `Tracker.h:525,
636-639`, and `glBindBufferBase` moves only `BindingSlot::m_version`):** bit 15 =
`Mix(bindPointGen[Uniform], programIdentity)`, bit 16 = `Mix(gen[ShaderStorage], gen[AtomicCounter])`,
bit 17 = `Mix(gen[TransformFeedback], TransformFeedbackGeneration)`; the content aggregate leaves all
three. `Touch` joins `gen_pipe_dirty_surface.py`'s `MUTATOR_PREFIXES` (`:87-90`) and `DirtySurface.def`
gains the rows the generator then demands. `Coverage.def`'s EMITTED list gains `GetBufferBindingPoint
→ SetShaderBuffers` as SHAPE-ONLY — **landed by c0e, with BOTH of its `PipeFill.cpp` switch arms
(`SubsystemForEmitter` and `EmittedCallSuppliesTheWholeField`) and the pairing `static_assert`s**,
because that file belongs to the contract package for the whole phase and an emitter enumerator
whose dispatch arm lived in the family's own worktree is exactly the merge trap the file's own
comment was written about. It is inert until the family's wired constant leaves 0.
`FieldOwnership.def:80-81` flips to RECORD_SUPPLIED when the four Espryt consumers are gone. `GetBufferBindingSlot`'s 15-pointer copy (`PipeFill.cpp:116-124`) needs no
special case: an unbarriered record has no fill at all.

### 5.7 XFB — stays lockstep
`StartPendingTransformFeedback` (`:1381-1510`), `SyncTransformFeedbackBindingPoints` (`:562-592`),
`ReadbackCapturedRanges` (`:1118-1160`) and the `xfb.targets` pins are not migrated
(`GetTransformFeedbackProgram` is an object row no package retires; the targets hold `SharedPtr`s
across two verbs, which rule C forbids an applier entry to do without a barrier). Escalation (i)
makes every verb inside an active span barriered; the cost on the Espryt steady path is one bool
test per draw (`:1386`).

### 5.8 Monolith / G1 / the push build under `Transport=monolith` (ruling 1)
Every server-side handle arm is selected by `MG_Config::Transport != Monolith` (the idiom at
`DirectGLES.cpp:3226, 4725, 6534`, `Managers.cpp:9629`) AND the family bit; it runs under ANY
active transport, lockstep or run-ahead, so `RUN_AHEAD=0` is a pure wait-rule A/B on identical
server code. The push build under `Transport=monolith` keeps its frontend arms token for token
(behaviour and bytes), which the verify comparator needs; the pull build compiles none of it (every
new symbol sits under `MOBILEGL_PIPE_PUSH` / `MOBILEGL_BUILD_DISAGGREGATED`; the `TwinRegistry`
alias keeps its mangled name, `Managers.h:626-640`; `BackendTextureObject`'s pull-build layout does
not move — the admitted-resize set stays empty). Two overloads, not an `#if` inside one body — the
frontend one is visibly the monolith-glue half, as `GetOrCreate(const StatePtr&)` is
(`SlotTables.h:236-244`). G1 0/0/0/0, G2/G14 name-set rules kept; the new public names are listed in
BRIEF-P5E per package. `MOBILEGL_PIPE_VERIFY` forces lockstep (`ConfigLoader.cpp:377`, confirmed).

---

## §6 Magma keeps the lockstep

1. The DirectVulkan arm of `CallMask` never sets `kCapRunAheadApply`; `RunAheadArmed()` is false;
   the client runs today's `:915-959` path; `MGPipeApplierCurrentRecordIsBarriered()` answers `true`
   for every record on a server that does not publish the bit, so Magma's probes inside
   `MagmaP7AllocatorDebtScope` and its BARRIER_PULLED reads keep P5C's semantics and `rsp`
   accounting. P7 retires them.
2. On Espryt the bit is published by `Init.cpp`'s DirectGLES arm gated on one constant
   (`kMGPipeP5eRunAheadReady`, false until the integration commit), so every package lands with the
   column inert and nothing changes behaviour until the integrator flips it.
3. Negative controls: `MOBILEGL_IPC_RUN_AHEAD=1` on Magma logs once and runs lockstep;
   `MagmaPipeIdentityTest`/`ServerLoopTest` pin that a DirectVulkan `CallMask` has no bit 10.

---

## §7 The strict gate

`integration-split-strict` (`.github/workflows/test.yml:1140-1206`) becomes a HARD GREEN lane for
the Espryt run-ahead server: with `MOBILEGL_IPC_STRICT_ERRORS=1` and the caps bit published, every
scenario must complete and every `[BARRIER-PULLED, ...]` marker in a private log is red. Because
§3.3 aborts on an unbarriered pull regardless of the knob, strict adds only the barriered rows'
pulls (readbacks, GenerateMipmap until tx2, XFB, CopyTex, set_storage_block_binding) — the lane's
step lists those rows as the ONLY admitted markers (an allowlist by `<field>@<verb>`) and fails on
any other. A revert of any handle arm in vi/sb/pg/tx2/fb goes red HERE, by field and verb, not in
a picture. Magma keeps the expected-red step. `rsp` on unbarriered records is 0 in every scenario
summary (pinned: a non-zero count would be a Fatal that did not fire). The `PipeSlotPeek` harness
asserts zero scope entries per frame on the draw path.

---

## §8 Amendments to CONTRACT-P5C (the complete list)

1. **§3.1's two named exemptions** are superseded by §4.4: the registry scope is deleted at every
   draw-path site and exempts only a barriered record elsewhere; the announcement scope is Magma's.
2. **§4.4 overflow**: lossless stays, but a run-ahead server parks on `Reserve == nullptr` (§2.6).
3. **§5.2 `object_death`**: the ring keeps the ordering; the row is `kWaitNone`.
4. **§5.1 `applier_reset`** is `kWaitApplied`; its `ContextSerial` assertion stands.
5. **§6 CI lane**: the two-sided lane becomes one-sided green for Espryt (§7).
6. **§6 layer 2** gains `Fatal{RoleViolation, "gPipeInputs"}` (§3.5); `InBarrierWait()` is
   re-derived as `MGPipeBarriered`. 7. **§3.3's state note** is retired by fb (§5.4).
8. **`FieldOwnership.def`**: `GetBoundVertexArray`, `GetTextureUnitObject`, `GetImageTextureBinding`,
   `GetFramebufferBindingSlot`, `GetProgramForDraw/Dispatch`, `GetBufferBindingPoint` gain retiring
   phase "P5e (Espryt unbarriered), P7 (Magma)"; `GetBufferBindingPointCount`'s forward → FATAL under
   split; `GetTransformFeedbackProgram`, `GetProgramObject`, `GetTextureObject`, `ValidateProgramName`,
   `HasOpenTransformFeedbackSpan`, `RecordError` keep their rows (barriered-only readers).
9. **Table 1** gains `set_program_bindings` (80), appended; `set_shader_buffers` (38) gains its
   route, sink and emitter (`PipeCatalogueTest.cpp:170-171, 242-243` invert). **Table 0** gains §1's
   thirteen rows. **Table 3** gains `IpcTable::RunAhead/PresentCredit`.
10. **P5C §6's `rsp` paragraph**: the measured object-class pulls (948.5/frame on bsl) go to 0 on
    unbarriered records; what remains is the barriered allowlist of §7.

## §9 The rulings, as made (ID-80…98; BRIEF-P5E §6 has the conflicts they resolve)

All nineteen are ACCEPTED AS WRITTEN. They are restated here rather than referenced, because a
package reads this file and not the brief.

1. Monolith arm selection: transport check + family bit, frontend arms kept for monolith (§5.8) —
   NOT S7's "handle arm under monolith too" (it would make the monolith push build depend on the
   new emissions and lose the verify comparator's frontend arm).
2. Client vertex arrays: refuse under run-ahead (§5.1), stage in P8; wait-escalation rejected (it
   would need `Find(vao)` on the apply thread, which §4.4 forbids for a draw).
3. XFB / atomic dispatch waits: the wire-derived predicate (§2.1); atomic-counter dispatch needs no
   escalation because sb lands 4.5.
4. Present credit default 1, range 1..8; credit 2 is a measurement arm only.
5. `resource_subdata`: buffer half `kWaitNone`, texture half keeps the reply (trailing D-D5).
6. `BindCurrentTextures` memo key: both S2's and S4's rows (§5.3).
7. Image sweep gate: applier serials, never a backend counter (§5.4); keep the union walk.
8. Program archive: encode per link into SEG_STAGE (§1), measured on MC + a shader pack before pg
   is sized; the `SharedPtr` pin is refused; lazy encode at first bind is the fallback.
9. Failed relink: re-issue with `LinkStatus = 0` iff the frontend reports the program unlinked
   afterwards; pg verifies which at `ProgramObject.cpp:348-356, 518-522`. Never an `object_death`.
10. `set_shader_buffers` window: the touched high-water count per target; capacity 84 on the wire,
    the backend clamps (`DirectGLES.cpp:500-504`). 11. Suppressor: three class slots.
12. Magma scope: one renamed scope, backend-kind-keyed exemption (§4.4).
13. Deferred-destroy queue: stays live (§2.7). 14. `ResolvedDrawBuffers`: measured after vi.
15. `Blit`'s `kWaitNone` is fb's landing, not a separate package; `Clear`'s is tx2+fb.

---

## §10 What c0e landed, by name — the surface every other package compiles against

Inert by construction: the caps bit is not published (`kMGPipeP5eRunAheadReady` is `false`), the
new opcode has no route, the new subsystem bit is not in `kMGPipeWiredSubsystems`, and every new
applier entry point and by-handle backend signature has a body that aborts by name. Unit,
`integration-split` and the generator gates are green with all of it in the tree.

| where | the names |
|---|---|
| `MG_Pipe/MGPipeTypes.h` | `kCapRunAheadApply`, `MGPipeRunAheadCapBitsFor(BackendType, Bool)`, `kMGPipeMaxBufferBindingPoints`, `kMGPipeShaderBufferClass{Uniform,ShaderStorage,AtomicCounter,Count}`, `MGPProgramBindings`, `MGPProgramSamplerUnit`, `MGPProgramStorageOverride`, `kMGPipeMaxProgram{BlockBindings,SamplerUnits,StorageOverrides}`, `MGPProgramDesc::LinkStatus`, `kDrawClientArrays` |
| `MG_Pipe/MGPipeValueTypes.h` | `MGPipeImageAccess`, `kMGPipeImageAccess{ReadOnly,WriteOnly,ReadWrite}`, `MGPipeImageAccessIsValid`, `MGPipeDecodeImageAccess`, `MGPipeImageAccessReads/Writes` |
| `MG_Pipe/MGPipe.h` | `MGPipeWaitClass` (`kWaitNone/kWaitApplied/kWaitReply/kWaitPresent/kWaitClassCount`), `kMGPipeSubsystemBufferBindings`, `kMGPipeSubsystemsMigratedAtP5e` |
| `MG_Pipe/generated/PipeWire.inc` | `MGPWireOp::SetProgramBindings` (80), `kMGPipeWaitClasses[]`, `MGPipeWaitClassFor(op)` |
| `MG_Pipe/PipeApply.h` | `MGPipeBarriered(op, payload, st)` (implemented), `MGPipeApplierCurrentRecordIsBarriered()` / `MGPipeApplierSetCurrentRecordBarriered()` (implemented, default `true`), `MGPipeApplySetShaderBuffers` and `MGPipeApplySetProgramBindings` (declared, aborting), `MGPipeApplierState::{BoundShaderBuffers, ShaderBufferStart, ShaderBufferCount, ShaderBufferWritableMask, ShaderBuffersSerial, IsTransformFeedbackActive}`, `MGPipeShaderCsoRecord::{BlockBindings, SamplerUnits, StorageOverrides, Signature, BindingsSerial}`, `MGPipeProgramStorageOverride` |
| `MG_Impl/Pipe/SetHashSuppressor.h` | `MGPipeSuppressorSlot::{SetShaderBuffersUniform, SetShaderBuffersShaderStorage, SetShaderBuffersAtomicCounter, SetProgramBindings}` |
| `MG_Impl/Pipe/Tracker.h` | `kMGPipeDirtyEmittedAtP5e`; bits 15/16/17 mapped onto bit 13 |
| `MG_Impl/Pipe/ImageEmit.h` | `MGPipeEncodeImageAccess` promoted to a free function over the shared table |
| `MG_Backend/DirectGLES/Managers.h` | `VertexArrayImpl::ResolveVaoTwin(handle)`, `TextureImpl::SyncTextureToBackendByHandle`, `TextureImpl::ResolveTextureTwin`, `BackendTextureObject::SyncMipmapsToBackendByHandle`, `BackendFramebufferObject::SyncToBackendByHandle`, `BackendRenderbufferObject::SyncToBackendByHandle`, `BackendProgramObjectImpl::SyncToBackendByHandle`, `PrgramImpl::ResolveProgramTwin` — all declared, all aborting by name, one block at the end of `Managers.cpp` |
| `Config.h` / `ConfigLoader.cpp` | `IpcTable::RunAhead`, `IpcTable::PresentCredit`; the push default is `kMGPipeSubsystemsMigratedAtP5e` |
| `MG_Backend/Init.cpp` | `kMGPipeP5eRunAheadReady` (`false`) and the caps arm through `MGPipeRunAheadCapBitsFor` |

**The three red-onces c0e ran, and what they name.** (1) flipping one row's `WaitClass` without
regenerating → `gen_pipe: OUT OF DATE: MobileGL/MG_Pipe/generated/PipeWire.inc`, rc 1; with the
regeneration, `PipeCatalogue.EveryRowCarriesTheWaitClassTheContractGivesIt` fails naming
`GenerateMipmap`. (2) `sizeof(MGPProgramBindings)` 32 → 40 →
`PipeCatalogue.LateArrivalsAreAppendedWithoutRenumbering` fails "Which is: 40 / 32". (3) making
the caps arm ignore the backend type →
`MagmaPipeIdentityTest.AMagmaServerNeverPublishesTheRunAheadCapBit` fails "Which is: 1024 / 0".
