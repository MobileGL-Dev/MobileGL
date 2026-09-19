# P5 scout — adversarial pre-mortem

**Subject:** assume P5 fails. Say why, with evidence.
**Tree:** `~/w7/pipe` = `feat/disaggregated` @ `a29807cc` (P4a landed, docs D.5 pushed). Read-only; nothing
was built, run, or edited. Every line number below was read in this tree at that head.
**Inputs read end to end:** `docs/Disaggregated/ARCHITECTURE.md` (619 lines), `ROADMAP.md` (P5 row `:21`,
open questions `:72-100`), `MEASUREMENTS.md` §22–§25 (`:534-632`), `~/w7/notes/p4a/p4a-results/fable-seam-audit.md`,
`~/w7/notes/p4a/INTEGRATOR-DECISIONS.md` ID-1…57. Code read: `MG_Pipe/{PipeCalls.def, MGPipeTypes.h,
MGPipeHostSpan.h, PipeApply.h, PipeApply.cpp, generated/PipeFilled.inc}`, `MG_Impl/Pipe/{PipeFill.cpp,
TextureEmit.h, ProgramEmit.h, SamplerEmit.h}`, `MG_Backend/MGPipe/PipeInputs.h`,
`MG_Backend/DirectGLES/Managers.{h,cpp}`, `MG_Remote/{Transport/*, Protocol/protocol.fbs}`,
`tools/trace_replay/CMakeLists.txt`, `MG_IntegrationTest/{CMakeLists.txt,Scenarios/}`.

**Confidence scale** (fable-seam-audit's): **proven** = code-proven on both sides; **mechanism** = a
code-proven chain not yet demonstrated by a run; **guess** = said so in the sentence.

---

## 0. Verdict in brief — the three ways P5 dies

**(A) `inproc` cannot fail the way `spawn` will, so P5's own gates certify nothing about the wire.**
Every byte carrier in the tree today is *a host pointer with `Blob.Size = 0`* — `ProgramEmit.h:193-194`
(default UBO), `ProgramEmit.h:249-255` (six SPIR-V modules **and the reflection archive: `Offset =
reinterpret_cast<Uint64>(&link)`**), `TextureEmit.h:1266-1267` (`Blob.Offset` = the level shadow's
address), `SamplerEmit.h:434-435` (`Parameters = {0,0}`), plus the companion `const void*` on nine
applier entry points (`PipeApply.h:85-96`). `MGPipeHostBytes` prefers the pointer branch
(`MGPipeHostSpan.h:50-52`). In one address space every one of those addresses resolves. P5's exit gates
(`ROADMAP.md:21`) all run under `inproc`; none runs under `spawn` (that is P6, `:22`). So a P5 that
lands the applier, the two `Split` scenarios and OpenRA SSIM ≥ 0.99 has proved that **the two roles can
share a heap**, which they already could. Class 1 and class 9 of the P4a taxonomy are invisible in that
lane *by construction*.

**(B) The un-migrated surface is bigger than the "reduced path", and the gate meant to catch it is
structurally blind on its seven worst fields.** `PipeInputs` has **63** fields
(`generated/PipeFilled.inc:28-92`); **23 of them have emitter `kNone`** (`:336-399`), i.e. no record in the
71-call catalogue carries them. Sixteen are non-sticky and are **pulled from the live `GLContext` at
every validate point** by `PipeFill.cpp:2572-2604` ("step 4: the residual fill, for what an emitted call
did NOT supply"; the pull is `:2604`). The other **seven are sticky** (`PipeFilled.inc:167-232`,
`kMGPipeInputStickyFieldCount = 7`) — `GetTextureObject`, `GetProgramObject`, `ValidateProgramName`,
`HasOpenTransformFeedbackSpan`, `GetBufferBindingPointCount`, `InvalidateCompileEnv`, `RecordError` —
and sticky fields are **exempt from the poison**: `MGPipeInputFieldIsFresh` returns true for them
unconditionally (`PipeFilled.inc:421`). P5's exit gate "a read of an unmigrated field =
`Fatal{UnmigratedPipeInput}`" therefore **cannot fire for the only fields that hand the server a raw
frontend object or write into the frontend**. There are 19 backend read sites on those seven
(`GetTextureObject` 1, `GetProgramObject` 3, `RecordError` 6, `ValidateProgramName` 3,
`InvalidateCompileEnv` 2, `HasOpenTransformFeedbackSpan` 1, `GetBufferBindingPointCount` 3; counted by
`grep '->\<name\>(' MG_Backend/DirectGLES/*.cpp MG_Backend/DirectVulkan`). Proven.

**(C) P4a made destructive client-side state changes depend on a *synchronous return value from the
applier*, and P5 is where that stops being available.** `Bool MGPipeApplyResourceCreate`
(`PipeApply.h:820`), `…Respecify` (`:868`), `…ResourceSubData` (`:897`), `…SetTextureParams` (`:1023`),
and `void* MGPipeApplyMapPersistent` (`:917`, table entry `:95`). The client clears a texture level's
dirty flag **only** on acceptance (`TextureEmit.h:1274-1307`); it latches params on acceptance (ID-55's
F-7 fix, `TextureEmit.h:805`, `:831`); and `MGPipeEmitMapPersistent` **returns the applier's raw pointer
to the frontend, which adopts it as the buffer's store** (`PipeFill.cpp:784` →
`MG_State/GLState/BufferState/BufferObject.cpp:238`, `:603`, `:657`). Under a queue, none of these
answers exists at the call. Whatever P5 chooses — round-trip per subdata (a stall per texture upload),
client-side re-derivation of the acceptance predicate (two implementations of one rule, the exact shape
of the D-K2 mirror that cost c0f + c0g), or "always accept" (the 66-DirectVulkan upload loss of ID-39
with a wire in between) — it is a *contract* decision that must be in the brief before a package starts,
not a discovery in the clientfb package's v2.

---

## 1. The nine P4a seam classes, each located on P5's surface

Class numbering follows `MEASUREMENTS.md:561-571` §23 and `fable-seam-audit.md` §A.

### Class 1 — "an encoding stated nowhere" (`MGPSubData::Target`'s class)

**Where it recurs in P5: the caps snapshot, and the segment-id space.**

*Caps.* There are already **two** caps carriers with **different field sets**:
`MGPCaps` (`MGPipeTypes.h:126-141`) = `DynamicBackendParameters Dynamic` *by inclusion* + `Uint64
CallMask` + two `MGPBlobRef` whose "serializers land with the transport (P5)" (`:135-138`); and
`CapsSnapshot` (`MG_Remote/Protocol/protocol.fbs:86-96`) = `dynamicParameters/rendererInfo/formatCaps`
byte images + `extensions: [string]` + `apiVersion` + `maxComputeWorkGroupCount/Size` +
**`tableSlotMask: ulong`** + `prefersCpuXfbPrimitiveAccounting`. `CallMask` (`MGPCapBit`, 9 bits,
`MGPipeTypes.h:110-123`) and `tableSlotMask` ("which `GLFunctionsTable` slots the peer registered") are
not the same bit space and not the same question, and no document says which is authoritative.
`MGPCaps` has **no literal size assertion** — only a composition one (`MGPipeTypes.h:140-145`) —
"because `DynamicBackendParameters` still carries `SizeT` fields, ABI-dependent until P0.5 moves the
caps block", and P0.5 explicitly did **not** move it (`ROADMAP.md:16`).

*Segments.* `MGPBlobRef::Seg` and `MGHostSpan::Seg` share one namespace with exactly two constants
written down — `kMGHostSpanSegNone = 0` and `kMGHostSpanSegFromServerIndexMirror = 0xFFFFFFFF`
(`MGPipeHostSpan.h:21-26`) — while the segment table (`SEG_CMD / SEG_STAGE / SEG_REPLY / SEG_EVENT /
SEG_SHADOW[n] / SEG_ADOPT[n]`, `ARCHITECTURE.md:409-416`) has no numbering anywhere in the tree. `0`
meaning "no segment" *and* being the natural first id is `TextureUploadTarget::Texture1D == 0` colliding
with `kMGPipeResourceTargetBuffer == 0` (ID-12) one level up.

**The brief must state, up front:** an ENCODING TABLE in the contract (not in a package header) with one
row per wire field that is not a handle: bit layout, the meaning of zero, and the reader. It must
include: the single caps carrier and its exact field list with fixed-width members (or an explicit
"`DynamicBackendParameters` crosses as an opaque byte image whose length is a field and whose
`structSize` discipline is `mg_protocol_base.h`'s"); the segment id space including whether 0 is legal
as a real segment; and the three P4a encodings that still live only in package headers (F-6a/b/c:
`MGPImageView::Access` at `ImageEmit.h:146-159`, `MGPSamplerView::Target` at `SamplerEmit.h:900`,
`MGPFramebufferState::DrawBuffers[8]`'s −1/default-token narrowing at `FramebufferEmit.h:146-163`),
which `fable-seam-audit.md:405-417` already parked for exactly this phase — the first wire reader is the
"one reader away" the audit warned about.

### Class 2 — "identity vs content: who mints, who looks up" (S-1's class, the 17 Iris traces)

**Where it recurs in P5: every id the transport mints beside the handle — reply slots first, then
`SEG_ADOPT`/`SEG_SHADOW` block ids.**

`MGPReplySlot { Uint64 Id; }` exists (`MGPipeTypes.h:78-81`) and is field-listed
(`PipeFields.def:32`, `:338`) — and **no payload of the ten `kReplySlot` calls contains one** (grep over
`MobileGL/`: the only hits are those two files and a comment at `MGPipe.h:49`). So the ten calls that
"answer into a reply slot" — `GetCaps` (`PipeCalls.def:80`), `MapPersistent` (`:84`), `FenceStatus`
(`:87`), `FenceWait` (`:88`), `QueryAvailable` (`:94`), `QueryResult` (`:95`), `ResourceReadback`
(`:138`), `GetTextureImage` (`:141`), `ReadPixels` (`:145`), `QueryTimestamp` (`:160`) — **cannot name
their reply**. The first package to need one will invent a minting rule in its own header; if two do,
that is S-1 verbatim (one allocator, two disjoint families, a lookup that never finds).

Second instance, mechanism: the reserved handle `{0,1}` of kind `Framebuffer` = the default framebuffer
(`ARCHITECTURE.md:39`). It is **a handle the client names and never describes** — which is precisely the
raw-depth-fetch sampler's shape (`fable-seam-audit.md:90`, `§B`: `GetRawDepthFetchSampler()` at
`DirectGLES.cpp:207-278`, a backend-minted object no client record can exist for; the fix at `1e8cdc59`
was the "the object IS the authority" branch). P4a's applier already admits `{0,1}` specially
(ID-27: "`{0,1}` admitted, `{0,0}` faults"). In split, the default FB's description arrives from
`OnSurfaceChanged` — a P9 reverse-channel event — so during P5 the server has a handle with no record
and no channel to get one.

**The brief must state:** a HANDLE-AND-ID RULE TABLE per kind **for both sides**, extended beyond the six
object kinds to every id the transport introduces (reply slot, segment, shadow block, adopted store):
*who mints, on what key (identity or content), who frees, and every table on the other side that may be
keyed on it*, with the static check the audit asked for (`fable-seam-audit.md:376-379`) — "a
content-addressed kind may not have a lifetime-id twin table" — extended to "a transport-minted id may
not be looked up in a client-minted table, and vice versa".

### Class 3 — "a record keyed on the wrong dimension" (the DSA framebuffer class)

**Where it recurs in P5: the lifetime of a staging slot, keyed on `stageAppliedTail` when its consumer
keys on its own drain.**

`ARCHITECTURE.md:445` (Phase 1): "GL 调用时刻把字节拷进 ring slot, slot 到 `stageAppliedTail` 越过它
为止不可变，危害按构造消除". `PipeApply.h:90` states the same rule for the monolith: the companion bytes
are "valid for the duration of the call only". **Espryt's own handle arm violates both.**
`GLESBufferResource::hostBytes` (`Managers.h:839`) is *the client's shadow base, stored and read later*:
written under `pendingMutex` in `Ops_H_SubData` (`Managers.cpp:1980-1983`) and in `Ops_H_FlushRange`
(`:2035`), and read at a *later* draw/readback/flush — `Managers.cpp:2000` (ensure path), `:2062-2072`
(the kill-switch map arm `Memcpy(mappedData, resource->hostBytes + start, …)`), `:2080`
(`UploadRangeFrom`), `:2111` (`Ops_H_Readback`'s pre-read flush), `:2741`, `:2843`. The rationale is
written in the tree at `Managers.cpp:1862-1864`: "the one thing a handle op cannot do is ask an object
for its shadow, which is why `GLESBufferResource::hostBytes` exists: the three content-carrying calls
hand the base over and **the later drains read it**." Proven.

So the retirement dimension is wrong for a whole family of records: those slots retire when the *drain*
consumes them, not when the record is applied. `Ring.h` already has the vocabulary — `kRecBorrowSlot`
("slot is borrowed into the GPU timeline; retires late", `Ring.h:~108`) and the second tail
(`cmdRetiredTail`/`stageRetiredTail`, `Ring.h:60-68`) — and nothing in the design says which records use
it.

**The brief must state:** for every content-carrying call, the slot's retirement event (apply / submit /
GPU-complete), and the rule "no applier entry point may retain a pointer past its return unless the
record is marked borrowed". Espryt's queue is the first named exception and it must be named.

### Class 4 — "a process singleton keyed by a per-context name" (`CompositeResolver`'s class)

**Where it recurs in P5: the role split itself.** `ARCHITECTURE.md:581` claims MGPipe reduces the
globals that need role-doubling "from four (`pGLContext`, `gBackendFunctionsTable`,
`pActiveBackendObject`, `pDefaultFramebufferInfo`) to **two**". That census is short by at least five,
all of which are written by *both* roles in an `inproc` process:

| global | file:line | who writes it |
|---|---|---|
| `gPipeInputs` (the whole ~20 KB strangler block) | `MG_Backend/MGPipe/PipeInputs.h:706` | client at `PipeFill.cpp:530`, `:2131`, `:2604`; applier at `PipeApply.cpp:1336, 1364, 1373, 1377, 1436, 1512` |
| `g_applier` (`MGPipeApplierState`) | `MG_Pipe/PipeApply.cpp:396`, accessor `:1092` | header says "under split there is one per served context" (`PipeApply.h:684`) — today there is one per process |
| `g_resourceOps` | `MG_Pipe/PipeApply.cpp:402`, set at `MG_Backend/DirectGLES/Managers.cpp:2556` | the **server's** registration is what the **client's** liveness gate reads (`PipeFill.cpp:2598` `P4aFamilyHasItsConsumer`) |
| `gMGPipeSegmentResolver` | `MG_Pipe/MGPipeHostSpan.h:47` (a plain non-atomic `inline` variable) | installed by `MG_Remote`; one resolver for two roles |
| the ten `*Instance()` singletons in `MG_Impl/Pipe` | enumerated in `fable-seam-audit.md:122-124` | client only — but the texture drain list `m_drain` is process-wide and the audit **already files it as a P5 item**: "it is a P5 item (one drain per client context)" (`fable-seam-audit.md:132-135`) |

The `g_resourceOps` row is the dangerous one and is **mechanism-proven**: in `inproc` the client's
consumer gate accidentally reads the *server's* registration and answers "yes" (right answer, wrong
reason); in `spawn` the client process has **no backend**, `MGPipeGetResourceOps()` is null, and the four
P4a families plus P3a's buffers **emit nothing at all** (`PipeFill.cpp:2598`, and the P3a buffer gate
`MGPipeResourceSubsystemEnabled()` cited at `ARCHITECTURE.md:379`). That is a silent, total loss of the
push path in the only mode that matters — and `inproc` cannot see it.

**The brief must state:** a role/thread ownership table for **every** process global in `MG_Pipe`,
`MG_Impl/Pipe`, `MG_Backend/MGPipe` and the backends' registration globals (see §5, table 3), and the
rule that a *client-side* gate may never be answered by a *server-side* fact — the consumer question
becomes a field of the handshake (`Hello`/`Welcome`, `protocol.fbs:59-78`) or of `MGPCaps::CallMask`,
which is what `CallMask` was invented for ("`CallMask` 取代'槽位是否为 null'这个隐式能力探测",
`ARCHITECTURE.md:110`).

### Class 5 — "a destructive client action with no precondition" (c0f/c0g's class: 66 DirectVulkan cases, 438/491)

**Where it recurs in P5: the acceptance contract (verdict C above) and the make-current reset.**

Beyond the four `Bool`-returning entry points, there is a second instance that is *not in the catalogue
at all*: the client calls `MGPipeApplierReset()` **directly, synchronously, from the validate point**
(`PipeFill.cpp:2462`, inside the `tracker.FreshlyPrimed()` arm at `:2455-2480`), together with
`MGPipeCsoCacheInstance().Reset()` and `MGPipeSetHashSuppressorInstance().InvalidateAll()`. The whole
argument for that being safe is that both sides start over *in the same instant* ("both sides start over
together rather than one of them remembering the other's objects", `PipeFill.cpp:2450-2453`). Under a
queue they are separated by the ring depth: the client drops its suppressors while records that assume
the old applier state are still in flight, and the applier's working state is wiped *ahead of* records
that precede the make-current. `MGPipeApplierReleaseObjectRecords()` (`PipeApply.h:731`,
`PipeApply.cpp:1223`) is the same shape at teardown. **Neither has an opcode**: the catalogue
(`PipeCalls.def:78-166`) has 71 entries and none of them is a reset.

**The brief must state:** (a) the acceptance rule for split, once, with its cost measured and published;
(b) that every server-state mutation is a record — the two resets get opcodes appended at the end of the
catalogue (`PipeCalls.def:26-29`: numbering never churns, new calls are appended), or an explicit
written ruling that they ride the control plane with a defined ordering point relative to the ring; and
(c) c0g's rule restated for the wire: *a family is live on the client iff every family it depends on is
live and its consumer is registered — evaluated with the same predicate on both sides*
(`fable-seam-audit.md:380-385`), with "consumer" now meaning "the **peer's** consumer".

### Class 6 — "a shutter that cannot see its own subject" (c0d / SD-0 / F-1 / F-2 / F-3)

**Where it recurs in P5: client-side conservative `MarkGpuWritten`, and the persistent-map dirty set.**

*GPU-write set.* Today the producer is the backend, at six sites:
`MG_Backend/DirectGLES/DirectGLES.cpp:570`, `:618`, `:2603`; `Managers.cpp:2512`;
`MG_Backend/DirectVulkan/Renderer/UniformManager.cpp:1075`, `:1231`;
`VulkanRenderer.cpp:11618`. P5's charter moves it to "client-side conservative `MarkGpuWritten`", i.e.
the client computes, at each draw/dispatch emission point, the set of resources the *backend* is about to
write — from the frontend's bindings. This is F-1's shape exactly (`fable-seam-audit.md:189-204`): the
emitter resolves a set for the *current program* while the shutter reads only binds and content. A
client-side walk over frontend bindings cannot see: XFB capture targets resolved by the backend, image
stores reached through a program-resolved unit set, Magma's UBO ring, Espryt's scratch FBO/buffers, and
anything a backend-minted object touches (the `g_rawDepthFetchSamplerState` class,
`ARCHITECTURE.md:327`). The failure is silent and in the **unsafe** direction: a missed mark means a CPU
read returns stale bytes with no refusal.

*Persistent map.* A coherent mapped write makes **no GL call at all** — there is no shutter that can
exist. `ARCHITECTURE.md:499` concedes this by making the Phase-1 rule "whole mapped span, by block, at
every validate point over the union of the twenty `SyncPersistentMappedRange` sites". The hazard is that
the *reachability walk* ("VAO/index/indirect/UBO/SSBO/atomic/XFB target") is written in prose, not
derived from the twenty sites, so it is an unchecked second copy of a list the backend owns.

**The brief must state:** the P4a lesson generalised (`fable-seam-audit.md:361-371`): **every field of
every emitted record, and every member of every emitted *set*, names the frontend counter (or the
enumerated site list) that moves when its source changes, and that counter is in the emitting shutter or
the emission is unconditional.** Concretely for P5: a table with one row per backend `MarkGpuWritten`
site and per `SyncPersistentMappedRange` site (20) and `SyncGpuWrites` site (6) — the per-site attribution
table P1 already produced (`ROADMAP.md:17`) — mapped to the client-side predicate that must fire, with a
unit case per row.

### Class 7 — "a serial that is not an emitted test" (E §0, F-1b)

**Where it recurs in P5: `appliedSeq` as the reconcile waiter's test.**

`ARCHITECTURE.md:247` makes the client reconcile "publish → wait `appliedSeq` → drain events → touch the
shadow" before the two client-side scans. `ARCHITECTURE.md:468` says the consumer updates `appliedSeq`
**once every 64 records**. A batched watermark is *coarser* than the test the waiter needs ("has MY
record been applied"), which is F-1b inverted (`fable-seam-audit.md:206-221`: "a record-serial epoch must
not be narrower than the shutter it replaces"; here it is wider than the test). Either the waiter
over-waits by up to 63 records (a latency bug, benign) or somebody makes it not-wait by comparing the
wrong way (a correctness bug, silent). And `seq` has no field: "没有逐记录序号字段——seq 就是记录序数"
(`ARCHITECTURE.md:124`), so the two counters (`m_emitSeq`/`m_applySeq`) must stay in lockstep across a
hard drain, a wrap pad (`kRingPadRecordKind`, `Ring.h:~105`) and a refused record. A pad record that one
side counts and the other does not desynchronises every subsequent wait, permanently, with no checksum.

**The brief must state:** for each of the five watermarks (`appliedSeq`, `submittedSeq`, `retiredSeq`,
`completedFrameSerial`, `presentAckSerial`, `Ring.h:70-75`) — what advances it, what may wait on it, the
exact comparison (`>=`), the batching rule ("batching may only DELAY a watermark; it may never let a
waiter observe a value implying more work than was done"), and **whether pad records advance the seq**.
One sentence per watermark; each is a defect if omitted.

### Class 8 — "a clear that is too wide" (wire C1 / finalfix C-1)

**Where it recurs in P5: the hard drain.** `ARCHITECTURE.md:460` says recovery after a hard drain is
cheap because "tracker 把全部 dirty 位置为'必须重推'，下一个 verb 重发完整 `set_*` 集合，纹理侧由发射
游标负责". **That is no longer true for textures after P4a.** The client cleared the level flags on
acceptance (`TextureEmit.h:1300-1307`) and the outstanding work lives in the *applier's* `PendingUploads`
(`PipeApply.h:265-284`), which a hard drain discards. The emission cursor the sentence relies on is
explicitly **not done** — "上面那条按存储属主键控的发射游标与索引重映射仍然是 P3b/P4b 的，P4a 没有做"
(`ARCHITECTURE.md:258`). So a hard drain in P5 loses accepted-but-unconsumed uploads with no owner left
to re-send them: `L0; draw(other); L1; draw(T)` reads black — ID-52's C-1, resurrected by a different
mechanism.

**The brief must state:** the drain/resync contract as a *state* contract, not a flush: what each side
must be able to reconstruct, who owns each piece of outstanding work after `ringGeneration++`
(`Ring.h:35`), and the rule "a client may not discard the record of work whose only other copy is in
flight". `ResyncRequest/Done` already exist on the control plane (`protocol.fbs`, listed at
`ARCHITECTURE.md:437`) and are undefined.

### Class 9 — "a gate that cannot go red"

**Where it recurs in P5:** three places, all checkable today.

1. **The `inproc`-only lane** (verdict A). Nothing P5 gates runs `spawn`.
2. **The retrace `SPLIT` arm's output directory.** `add_trace_replay_test` derives
   `TRACE_OUTPUT_DIR=…/${CASE_NAME}/${BACKEND}` (`tools/trace_replay/CMakeLists.txt:370`) and
   `TRACE_ARTIFACT_DIR=…/${CASE_NAME}/actual-images` (`:371`). `ARCHITECTURE.md:584` remembers to add a
   `SPLIT` suffix to the **test name** (`:351`) and says nothing about the directories. Two arms then
   write one `output/mobilegl.log` — and that file is the *only* valid refusal census
   (`MEASUREMENTS.md:554`: "`ctest -V` 做拒绝普查是假零"; `fable-seam-audit.md:288-291`). The census
   would read whichever arm ran last: a false zero with a different cause than P4a's, in the same place.
3. **The `ENVIRONMENT` property.** `tools/trace_replay/CMakeLists.txt:375` sets `ENVIRONMENT` as a
   literal string and does **not** use `mgl_itest_join_environment` (which exists, but only for itests,
   `MG_IntegrationTest/CMakeLists.txt:321`). ctest `ENVIRONMENT` replaces rather than appends
   (`ARCHITECTURE.md:584`, and the CI-lane memory of the same trap), so a `SPLIT` arm that adds
   `MOBILEGL_TRANSPORT` naively drops `EGL_PLATFORM=surfaceless;LIBGL_ALWAYS_SOFTWARE=1;
   MESA_GL_VERSION_OVERRIDE=3.3` and either opens a window on CI or runs on a different GL — and the
   failure looks like a split bug.

**The brief must state:** the census recipe for P5 (file sink, per-process **and per-role** log, run on
itest AND retrace, with the role's own log path in the brief), the directory/name scheme for the SPLIT
arm, and the rule from `ROADMAP.md:7` applied literally: **每个门必须能因它存在的理由变红** — for each
P5 gate, the named negative control and the fact that it was observed red once.

---

## 2. Hazards that exist only once the client and the applier are two threads

The P0 transport skeleton has no thread yet — there is no `ServerLoop`, and `InProcessTransport`'s own
header says what it does not do: "What it does NOT exercise is serialization of the byte stream"
(`InProcessTransport.h:15-17`). Everything below is new work whose hazards are not covered by any
existing test.

**H1 — `gPipeInputs` is a single process-global struct written by both threads, and the problem is
ordering before it is a race.** `PipeInputs.h:706` (`inline PipeInputs& gPipeInputs = *new PipeInputs();`).
The client writes it at the validate point for verb N (`PipeFill.cpp:2604`, plus `:530`
`MGPipeNoteFrontendMutation` and `:2131`); the applier writes it as it applies records
(`PipeApply.cpp:1336, 1364, 1373, 1377, 1436, 1512`); the backend reads it through `MGB_CTX` everywhere.
With a queue, the client is filling verb N's fields while the apply thread is executing verb N−k. Every
field the client fills is *wrong by construction* the moment the queue is non-empty — not torn, just
from the future. **Rule the brief must state: under split the apply thread owns `gPipeInputs`
exclusively; the client thread may not write it; the verb serial is advanced by the applier, not by
`MGPipeValidateForVerb`** (which advances it today at `PipeFill.cpp:2384`).

**H2 — lifetime of client memory referenced by a record.** Two distinct sub-hazards:
 - *Retention past the call*: `resource->hostBytes` (§1 class 3) — proven, six read sites.
 - *Var-tails that point into emitter-owned `Vector`s reused next emission*: `MGPipeApplyResourceSubData`
   is handed `m_regions.data()` (`TextureEmit.h:1278-1280`, member at `:1311`); the same shape for
   `SetVertexBuffers` (`PipeApply.h:941`), `SetSamplerViews` (`:1028`), `BindSamplerStates` (`:1029`),
   `SetShaderImages` (`:1030`), `SetVertexAttribDefaults` (`:756`). In monolith the applier consumes them
   before returning; on a queue the emitter overwrites them on the next verb.
 - *Frontend object lifetime*: `MGPSurface::Res` is "left unresolved on purpose (D-I3: the keep-alives
   are the frontend's `SharedPtr`s)" (`PipeApply.h:~513`). Those `SharedPtr`s live on the GL thread; the
   apply thread holds no reference. ID-52's C-2 (`glTexImage2D; glDeleteTextures; draw` → SIGABRT "pure
   virtual method called") was fixed by ordering the death notice before the free *in one thread*
   (ID-55, `a690032f`). With a queue the free happens on the client while the delete record is still in
   flight. `{slot, gen}` protects the *slot*, not the object the server still holds a `hostBytes`/twin
   for.

**H3 — the blob rule is a monolith rule and must be inverted for split.** `MGPipeTypes.h:398-410`: "A
ZERO `Blob.Size` means *this record does not declare its blob*, which is what a monolith emission is …
and it is not a fault." Five emitters emit zero (listed in verdict A). Under split, zero must be
`Fatal{ProtocolCorruption}` for every record whose payload has an `MGPBlobRef`, and `Blob.Offset`
must stop being an address — it is one today at `TextureEmit.h:1266`, `ProgramEmit.h:193`, `:250`,
`:254`. Note the asymmetry that will bite a reviewer: four sites **do** declare a real size
(`CsoCache.h:156`, `VertexInputEmit.h:398`, `ResourceTracker.h:216`, `PipeFill.cpp:2259`), so "some
records declare, some do not" is the current, legal state.

**H4 — `MGHostSpan` in `inproc`.** `MGPipeHostBytes` takes the `Ptr != nullptr` branch first
(`MGPipeHostSpan.h:50-52`). In one process the client's `Ptr` is valid, so unless the encoder
*deliberately nulls it*, `inproc` silently keeps using host pointers and the `Seg` path
(`gMGPipeSegmentResolver`, `:47`, a non-atomic process-global installed by `MG_Remote`) is never
exercised. The brief must require: in split the encoder sets `Ptr = nullptr` unconditionally and a
non-null `Ptr` on the apply side is Fatal; and the resolver is per-role, installed before the apply
thread starts.

**H5 — teardown and exit order.** ID-8's rule (P3a ID-18/21) — "no frontend destructor may run from an
exit handler into pipe/backend state; every new static holder of frontend `SharedPtr`s is
leak-at-exit storage" — now acquires a thread. `ARCHITECTURE.md:537` gives the order (publish → server
drains and acks → stop apply → close transport → drain compile pool → `MobileGL::Destroy()` → release
sync/query handles) but **does not name `Doorbell::Kill()`** (`Doorbell.h:221`, with `Dead()` at `:116`,
`:210`), which is the only way to un-park a waiter. A parked apply thread that is never killed hangs
`Shutdown`; one that is killed but not joined returns into emitter `Vector`s the GL thread has freed.
And `RecordError` is a *sticky, forwarded* field (`PipeFilled.inc:228`) — the apply thread calling
`ctx->RecordError()` during teardown is a write into a frontend that is being destroyed.

**H6 — it is not two threads, it is at least three.** Espryt's buffer ops are **already** reachable off
the render thread today: `Managers.cpp:1963-1969` ("this line runs BEFORE the `CanTouchGLNow()` test
below — i.e. on the arm D-A2 deliberately keeps reachable off the render thread") with `pendingMutex`
guarding `hostBytes`/`pendingRanges`, while "every reader of `hostBytes` … is on the render thread". Add
`mgl-srv-apply` (which `ARCHITECTURE.md:535` says holds the native context *for life*) and
`ShaderCompilePool` (stays on the client) and you have four. The brief must say what `CanTouchGLNow()`
answers on the apply thread, who `g_backendContextOwnerThread` is set to, and whether the off-thread
buffer arm still exists under split.

**H7 — re-entrancy.** `PipeFill.cpp:492` records that "`GetProgramForDraw`'s join can re-enter a backend
and with it another `gPipeInputs` [fill]". With a second thread that re-entrancy is concurrent, not
nested.

**H8 — the exit-order rule's second half.** `g_applier` (`PipeApply.cpp:396`) and `gPipeInputs`
(`PipeInputs.h:706`) are deliberately `*new` leak-at-exit. Good for teardown; bad for `inproc`, because
there is exactly one of each for two roles (§1 class 4).

---

## 3. Calls in the 71-entry catalogue that are under-specified for a real wire

"Fine in monolith because a pointer stays valid; not fine once the record outlives the caller."
Catalogue lines are `MobileGL/MG_Pipe/PipeCalls.def`; entry points are `MobileGL/MG_Pipe/PipeApply.h`.

| call (`PipeCalls.def`) | flags | what actually crosses today | why the wire form is undefined |
|---|---|---|---|
| `ResourceRespecify` `:82` | `kNeedsAck` — **no blob flag** | `const void* initialBytes` = `buffer.MappedData()`, the whole shadow (`PipeFill.cpp:685-691`); entry `PipeApply.h:868` | `MGPResourceDesc` (`MGPipeTypes.h:283-318`, 88 B) **has no `MGPBlobRef` member at all**, and `PipeApply.h:76-79` says so explicitly ("resource_respecify deliberately does NOT carry kHasBlob here, because a kHasBlob record must own an MGPBlobRef and MGPResourceDesc has none"). **Every `glBufferData(size, data)` and every `glTexImage*(…, data)` initial upload has no carrier.** |
| `CreateSamplerState` `:104` | **`kNone`** | `const SamplerParameters*` (`PipeApply.h:1000`), while the payload's `MGPBlobRef Parameters` is left `{0,0}` (`SamplerEmit.h:434-435`) | three sources disagree: the catalogue says no blob, the payload has one (`MGPipeTypes.h:427-431`), the entry point takes a pointer. A G3 encoder driven by the catalogue ships 32 bytes and **no sampler parameters** — and S-1 already showed what a sampler with driver defaults does to 17 Iris traces. |
| `CreateShaderState` `:108` | `kHasBlob` | `const LinkArtifacts*` + `const SpirvArtifacts*` (`PipeApply.h:1037-1040`); all seven blobs `Size = 0`, `Offset` = host address (`ProgramEmit.h:249-255`, incl. `Reflection.Offset = (Uint64)&link`) | the archive is a 58-member container-holding struct; `ProgramArtifactsCodec` exists but **is called only in verify builds** (`ARCHITECTURE.md:265`, D-H3), and its NDK/libc++ size pin is still lazy (`MGL_ARTIFACT_SIZES_LIBCXX_PINNED` undefined, same line). Also the **only** record that certainly exceeds `MaxRecordBytes() == Capacity()/2` (`ARCHITECTURE.md:125`) → needs G3 chunking, which `ROADMAP.md:25` schedules in **P8**. The header itself says "Splitting this record for a transport … is P5's problem" (`PipeApply.h:1036`). |
| `SetGlobalConstants` `:122` | `kHasBlob` | `const void* bytes` = `MapUBO()`'s image, `Blob.Size = 0` (`ProgramEmit.h:192-194`, entry `PipeApply.h:1052`) | per program per frame, unbounded length, no declared size |
| `ResourceSubData` `:134` | `kHasBlob\|kVarTail` | `const void* bytes` **and** `const MGPSubRegion*` tail (`PipeApply.h:897-899`); `Blob.Offset` = shadow address, `Size = 0` (`TextureEmit.h:1266-1267`); tail = `m_regions.data()` (`:1278-1280`) | the byte count is "the server's to compute once it has picked box-or-rects" (`TextureEmit.h:1261-1264`) — i.e. the length is *derived on the far side*, which cannot be a bounds check |
| `BufferSubDataResident` `:135` | `kHasBlob\|kOptional` | `const void* bytes` = application staging, "valid for the duration of the call only" (`PipeApply.h:90`) | `kOptional` + a nullable table slot is also a *capability* question under split (`kCapResidentSubData` is unwired, `ROADMAP.md:93`) |
| `ResourceFlushRange` `:137` | **`kNone`** | `const void* bytes` (`PipeApply.h:904`) | a content-carrying call with neither `kHasBlob` nor a blob member |
| `MapPersistent` `:84` | `kReplySlot\|kOptional` | **returns `void*`** (`PipeApply.h:95`, `:917`) plus `const void* seedBytes`; the frontend adopts the returned pointer as the buffer's store (`PipeFill.cpp:784` → `BufferObject.cpp:238`, `:603`, `:657`) | a pointer cannot cross a process boundary; and `kReplySlot` is documented as *never blocking* (`MGPipeTypes.h:76-78`) while this call is synchronous by construction |
| `ResourceReadback` `:138` | `kReplySlot` | nothing: `MGPReadback` is `{Res, Offset, Size}` (`MGPipeTypes.h:1151-1155`) — the destination is Espryt writing the client shadow through `resource->hostBytes` (`Managers.cpp:2111` and the writeback below it) | server→client bytes with **no channel** until P9 (`ROADMAP.md:26`) |
| `ReadPixels` `:145`, `GetTextureImage` `:141` | `kReplySlot` | `MGPReadbackInfo` has `DstOffset/DstSize` (`MGPipeTypes.h:1197-1206`) but **no `Seg`** | which segment the destination is in is unstated; P5's charter says `read_pixels` blocks, contradicting the reply-slot rule above |
| all ten `kReplySlot` calls | — | — | **no payload contains an `MGPReplySlot`** (§1 class 2): the reply has no name |
| `SetVertexBuffers` `:114`, `SetSamplerViews` `:117`, `BindSamplerStates` `:118`, `SetShaderImages` `:119`, `SetStreamOutputTargets` `:121`, `SetVertexAttribDefaults` `:123` | `kVarTail` | tails as `const T*` into emitter-owned `Vector`s (`PipeApply.h:756, 941, 1028-1030`) | tails are declared; the *copy* and the count/length cross-check are P5's, and the applier's existing bound gate is on the slot, not the tail |
| `SetShaderBuffers` `:120` | `kVarTail\|kHostSpan` | second tail of `MGHostSpan[HostSpanCount]` only under `kCapNeedsHostUboBytes` | nobody sets that cap today; it is Magma's (P7) and its shape is still `ROADMAP.md:89` open question 6 |
| `DrawVbo` `:146` | `kHostSpan\|kVarTail` | user indices via `MGHostSpan.Ptr` | the only `kHostSpan` on the hot path; `MGPDrawInfo`'s own comment defers the fixed-head-vs-tail decision to P5 (`MGPipeTypes.h:1219-1224`) |
| **not in the catalogue at all** | — | `MGPipeApplierReset()` (`PipeFill.cpp:2462`), `MGPipeApplierReleaseObjectRecords()` (`PipeApply.h:731`), `MGPipeSetResourceOps()` (`PipeApply.cpp:1094`) | three server-state mutations the client performs by direct call; §1 classes 4 and 5 |

Counting the rows above: **13 of the 71 calls carry content that has no wire carrier today**, plus six
var-tail calls whose tails are borrowed, plus three server mutations with no opcode.

---

## 4. The weakest P5 exit gate, and a stronger formulation

`ROADMAP.md:21` lists: `DirectGLES.Split.*(ClearThenReadPixels|Triangle)` green under `inproc`; OpenRA
trace split SSIM ≥ 0.99; **`PersistentCoherentMapScenario` green**; both roles' peak RSS recorded;
`persistent-map-push` published; a read of an unmigrated field = `Fatal{UnmigratedPipeInput}`.

**Weakest as written: `PersistentCoherentMapScenario` green.** Three independent reasons, all
verifiable today:

1. **The scenario does not exist.** `grep -rl 'PersistentCoherentMap' MobileGL/` returns nothing; the
   only match in the gate's sentence that exists is `Scenarios/ClearThenReadPixelsScenario.cpp`. The
   phase that is gated authors the gate. (`DirectGLES.Split.…Triangle` has the same problem: no
   `Triangle` test exists under `MG_IntegrationTest/` today either — the prefix machinery is
   `MG_IntegrationTest/CMakeLists.txt:739` `TEST_PREFIX "DirectGLES."`.)
2. **Its subject is skipped in the only lane it runs in.** The client-side block push exists *because*
   tier T2 returns `nullptr` (`ARCHITECTURE.md:490`, `:496-500`). Under `inproc`,
   `MGPipeApplyMapPersistent` can return a perfectly good pointer (`PipeApply.h:917`; it is the same
   heap), the frontend adopts it (`BufferObject.cpp:238`), and the scenario passes **with the push code
   never executed**. It is a green gate over dead code — MEASUREMENTS §23's last row verbatim
   ("绿得毫无意义").
3. **It has no stated negative control**, while the sibling counter `map-persistent-roundtrips` was given
   a definition precisely so that it *could* go red (`ARCHITECTURE.md:492`, P3a's ruling). The scenario
   inherited none of that rigour.

**Stronger formulation** (replace the one line with five, all mechanically checkable):

> `PersistentCoherentMapScenario` is green under `inproc` **with the adoption tier forced to T2**
> (`MOBILEGL_IPC_ADOPT_TIER=2`), and:
> (a) the same scenario is **RED** with the block push disabled by a named negative control
>     (`MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0`), and that red has been observed once and recorded;
> (b) the scenario contains a write that **no GL call announces** — map, write, draw, write again, draw
>     again, readback — so that "push at every validate point" is actually required rather than
>     incidentally satisfied by a `glBufferSubData`;
> (c) the split run asserts `persistent-map-push` bytes > 0 **and** `mpr` equal to the monolith arm's
>     (P3a's definition, `ARCHITECTURE.md:492`), so a regression to per-draw acquisition shows up;
> (d) an assertion that under split `MGPipeApplyMapPersistent` **never returns a non-null pointer** —
>     the negative control that catches the `inproc` address leak, and the one line that makes the gate
>     mean the same thing in `inproc` and `spawn`;
> (e) the same five run with `MOBILEGL_IPC_PRESENT_CREDIT=1` and a ring small enough to force at least
>     one back-pressure wait, so the push interacts with the watermark it depends on.

**Runner-up, and arguably worse in kind: "a read of an unmigrated field = `Fatal{UnmigratedPipeInput}`".**
It is a *good* gate that is provably blind on its seven most dangerous subjects (verdict B;
`PipeFilled.inc:421`). Stronger formulation:

> Under split, each of the 63 `PipeInputs` fields is in exactly one published set — **record-supplied**
> (naming the call), **applier-derived** (naming the record it is derived from), or **FATAL**; the table
> is generated (the `gen_pipe_dirty_surface.py --check` shape) and a field in no set fails the build.
> The sticky exemption at `PipeFilled.inc:421` is removed for split builds: a sticky field is a
> *forwarded call into the frontend* and is therefore FATAL on the server, not fresh. The phase ships one
> negative control per set: moving a field from supplied to FATAL must turn a named test red.

(Also worth fixing in the same row: "both roles' peak RSS recorded" and "`persistent-map-push` 出数" are
*records*, not gates, and should be labelled as such the way `ROADMAP.md:57` labelled performance —
otherwise they read as gates that can never be red.)

---

## 5. The three tables the P5 brief must freeze before any package starts

P4a needed seven contract corrections (`c0b`…`c0g`) plus a seam audit and a final-review fix round; the
root-cause list is `fable-seam-audit.md:359-398` and `MEASUREMENTS.md:557-573`. Each of these three
tables kills one whole column of that list.

### Table 1 — the byte-carrier table (kills classes 1, 3, 5 and H2/H3)

One row per call that carries content or a tail (the 19 rows of §3). Columns: `PipeCalls.def` flags ·
payload blob member (or none) · applier companion pointer today · **which segment the bytes live in** ·
**who owns the memory** · **when the slot retires** (apply / submit / GPU-complete, i.e. does the record
need `kRecBorrowSlot`) · **what declares the length** and what cross-checks it · the reply's name for
`kReplySlot` rows. Fold in the segment-id space and reply-slot minting as two columns, because they are
the class-2 recurrence and they have no other home.

The three rules to state once, above the table:
* in split, `Blob.Size == 0` on a record whose payload has an `MGPBlobRef` is `Fatal{ProtocolCorruption}`
  (today it is explicitly *legal*, `MGPipeTypes.h:398-410`);
* `MGHostSpan::Ptr != nullptr` on the apply side is `Fatal` (today it is the fast path,
  `MGPipeHostSpan.h:51`);
* no applier entry point may retain a pointer past its return unless its record is marked borrowed —
  **and Espryt's `hostBytes` queue is the first named exception** (`Managers.h:839`,
  `Managers.cpp:1983`, `:2111`).

Every disagreement between the catalogue flags, the payload members and the entry-point signature is a
row that fails today; §3 lists at least four (`CreateSamplerState`, `ResourceRespecify`,
`ResourceFlushRange`, and the ten reply-slot calls).

### Table 2 — the `PipeInputs` field-ownership table (kills class 6 and verdict B)

All 63 fields × {record-supplied (which call) · applier-derived (from which record) · client-only (never
crosses) · FATAL}, **including the seven sticky ones**, with the owning thread for each write. This is
the P5 analogue of the "record field → source setter → shutter" table the seam audit says the next brief
must state (`fable-seam-audit.md:361-371`), and it is the only thing that gives "the reduced path" a
definition — today the phrase has none, and the gap between "two Split scenarios" and "OpenRA SSIM ≥
0.99" is exactly the gap between 40 supplied fields and 63. Extend the same table shape to the two
*sets* that have no field id: the conservative GPU-write set (one row per backend `MarkGpuWritten` site,
7 sites listed in §1 class 6) and the persistent-map reachability set (one row per
`SyncPersistentMappedRange` site, 20 of them, per `ROADMAP.md:17`'s P1 attribution table).

### Table 3 — the role / thread ownership table for every process global (kills classes 4, 7 and H1/H5/H6)

Every `*Instance()` in `MG_Impl/Pipe` (ten, `fable-seam-audit.md:122-124`), every `g_*` / `gPipe*` in
`MG_Pipe` and `MG_Backend/MGPipe` (at least the five in §1 class 4), and the backends' registration
globals. Columns: owning role · writing thread · what the other role in the same process does about it
(the `MOBILEGL_BUILD_DISAGGREGATED_INPROC` shim, which `ARCHITECTURE.md:595` lists as *not existing*) ·
what a make-current and a teardown do to it. It must be a **superset** of `ARCHITECTURE.md:581`'s
"four → two" claim, which this scout believes is now wrong by at least five entries. Attach the five
watermark rules (§1 class 7) to the same table, since they are the only globals two *processes* share.

---

## 6. Recorded, lower confidence, and what would settle it

* **`present` 1:1 and the frame-boundary drain.** `ARCHITECTURE.md:532` requires strict 1:1 because both
  backends' frame-boundary ageing only happens inside `Present`. P5 has no `Present`-related gate; a
  batching or credit bug shows up as a slow leak (Espryt's three rings and `TrimBufferPool` never
  retiring), which SSIM cannot see and a two-scenario lane cannot reach. *Mechanism.* The cheap gate is
  the one already in the row: "both roles' peak RSS recorded" — make it "recorded **over N frames with a
  stated slope**", not a single peak.
* **`MOBILEGL_TRANSPORT` parsing has nowhere to land yet.** `grep -rn 'MG_Config::Transport'` over
  `MobileGL/` and `CMakeLists.txt` returns **nothing**; `Config.h` has no `Transport` member and
  `Init.cpp` is the plain backend `switch` (`MG_Backend/Init.cpp:48-63`). So the "single hook" of the
  charter is a new member, a new parse, a new `#if`, and a `constexpr Monolith` fallback
  (`ARCHITECTURE.md:580`) — all of which is fine, but it means **no existing test exercises any
  transport selection**, and the `MG_Test/Wire` suites only cover the transport primitives.
* **Espryt still names frontend types in 155 places** (`grep -c 'MG_State::'`: `Managers.h` 58,
  `DirectGLES.cpp` 97). Most are on the legacy arm compiled under `MOBILEGL_PIPE_LEGACY_MEMOS`
  (forced ON in pull builds, `ARCHITECTURE.md:597`), so the number overstates the split-relevant
  residue — I did **not** separate the two arms, and the brief writer should not quote 155 as a split
  figure. The one that is certainly on the handle arm is `g_rawDepthFetchSamplerState`
  (`ARCHITECTURE.md:327`, `DirectGLES.cpp:207-278`), whose nativisation is P3b/P4b and which is
  therefore *still a frontend `SamplerObject` constructed inside the backend* when P5's apply thread
  starts. **This is the single most likely first crash of `mgl-srv-apply`.** *Mechanism, high
  confidence.*
* **Two readings of "InProcessTransport 走与 spawn 相同的 G3 编解码" (`ROADMAP.md:21`).** Reading A: the
  hot path encodes into the ring identically and only the doorbell/copy mechanism differs
  (`ARCHITECTURE.md:391`). Reading B: only the *control plane* is shared, since
  `InProcessTransport.h:15-17` says framing is not exercised. The evidence that settles it is whether
  the P5 encoder writes `Ptr = nullptr` / real `Blob.Size` in `inproc` — if it does, reading A holds and
  verdict A is mitigated; if it does not, `inproc` is a different protocol wearing the same name. **The
  brief should make this an explicit, one-sentence ruling**, because every gate in the row depends on it.
* **`create-indirect` on Adreno remains a `dev`-side failure** (`ROADMAP.md:100`, open question 17) and
  P8 wants `roundtrips-per-frame == 0` on it. Not P5's, but P5's persistent-map push is measured on the
  Create/Flywheel fixtures (`ARCHITECTURE.md:499`), so the brief should say which fixtures the
  `persistent-map-push` number is taken on and that the device arm excludes that one (D.4.2's precedent).
* **Not checked by me:** the `MG_Test/Wire` five suites' actual coverage; `FdPassing` on this box;
  `protocol.fbs`'s `SurfaceOp`/`AuxRequest` shapes against §15's surface plan; whether any
  `MOBILEGL_PIPE_STATS` byte class other than `persistent-map-push` is unwired. None of these changes
  the five answers above; all are cheap for the brief writer to confirm.

---

## 7. One-paragraph summary for the brief writer

P5's stated exit gates can all be met by a build in which no byte ever leaves the process's address
space, no record ever outlives its caller, and the seven most dangerous `PipeInputs` fields still reach
straight into the frontend — because `inproc` is the only lane, `MGHostSpan.Ptr`/`Blob.Offset` are host
addresses everywhere in the tree today, and the poison exempts sticky fields by construction. The three
tables in §5 (byte carriers with segment + retirement + length declarant; `PipeInputs` field ownership
including the sticky seven; role/thread ownership of every process global) are what turn the P4a seam
taxonomy into a set of compile-time and gate-time obligations before the first package branches. The two
contract decisions that cannot be deferred to a package's v2 are the **acceptance rule** (four `Bool`
entry points and `MapPersistent`'s `void*` today gate destructive client-side state changes) and the
**consumer/liveness gate**, which in `spawn` is answered on the wrong side of the wire and silently
disables the whole push path.
