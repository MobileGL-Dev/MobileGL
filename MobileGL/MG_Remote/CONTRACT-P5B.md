# CONTRACT-P5B — the wire contract for the 25 measured class-C slots

Authority: this file, beside `CONTRACT-P5.md`, which stays authoritative for everything it
covers (table 0's encodings, the byte carriers, field ownership, role/thread ownership, the
knobs, R-1…R-17). This file ADDS rows to those tables for the verb-migration phase P5b
(`~/w7/notes/p5/INTEGRATOR-DECISIONS.md` ID-66, ID-68; the census
`~/w7/notes/p6/census-classC.md`) and makes the rulings the census left open. Where it
disagrees with CONTRACT-P5.md, this file is newer and wins, and §6 lists every such place.

**How to change it.** Package c0b's file, edited by the integrator first. A migration package
that needs a row changed goes through the integrator; the packages compile against the rows
below from day one, which is the whole reason the file exists.

Base: `feat/disaggregated @ c9878d69` (P5's c1g landed). Every `file:line` below was read at
that commit.

---

## §0 What P5b is, and the four rules above every row

P5 landed the first IPC frame on the reduced path: six verbs cross the wire to a real apply
thread under `MOBILEGL_TRANSPORT=inproc`, and 64 of the 71 client slots are
`Fatal{UnmigratedVerb, "<slot>"}`. The census measured which of the 64 real workloads hit
first: **every Minecraft trace first-stops at `DrawElements`**
(`improved-transparency-minecraft-26.3` at `DrawElementsInstancedBaseVertex`), and the
inproc integration lane's 505 verb aborts are led by `BindImageTexture` 138,
`BeginTransformFeedback` 95, `DrawElements` 56, `DispatchCompute` 49, `PatchParameteri` 43,
`CopyImageSubData` 30, `MemoryBarrier` 18, `MultiDrawElementsBaseVertex` 18, `ClearBufferiv`
12, `ClearBufferuiv` 8 and a measured tail of 15. **P5b migrates those 25, in four packages,
under inproc, with the lane's abort count and the traces' first-blocker list as the only
gates.**

The four rules every row below is held to. A, B and C are CONTRACT-P5 §0's, restated only
where P5b sharpens them; D is new.

**Rule A — a content record declares its bytes.** Unchanged. P5b adds ONE content-carrying
row (`set_storage_block_binding`'s block name) and it is `kHasBlob`, staged whole in
`SEG_STAGE`, `Size = strlen + 1`, never 0.

**Rule B — no host pointer crosses.** Unchanged, and P5b is the phase that ARMS the one
conditional `MGHostSpan` on the hot path: `draw_vbo`'s user-index span
(CONTRACT-P5 table 1 row 16). d1 fills it with `Ptr = nullptr, Seg = SEG_STAGE`, the encoder
and decoder run all four honesty arms on it, and the sink resolves it through
`MG_Pipe::MGPipeHostBytes`. `kCapNeedsHostIndexBytes` and `kCapNeedsHostUboBytes` STAY 0 in
P5b — they are about the SERVER needing the bytes of VBO-backed index buffers (P8's index host
mirror, ARCHITECTURE §10.3), not about a client array the client alone holds.

**Rule C — an applier entry point may not hold a pointer past its return.** Unchanged, and
it now binds the sinks: every `WireVerbSink::On*` argument that points into a record or a
staged run (`ranges`, `userIndices`, `indirect`, the block `name`) is valid for the call only.

**Rule D — a P5b verb crosses AS THE CALL, not as a reading of it.** A record for one of the
25 carries the GL arguments the frontend handed the backend slot - the GL enums verbatim,
the GL names the backend keys on, the offsets as the app spelled them - plus the handle the
P7/P8 form will dispatch on instead. The server's `ServerVerbSink` reproduces the backend
call the monolith makes, and the backend keeps reading the frontend state it reads today
through the BARRIER-PULLED fields of its verb class (CONTRACT-P5 table 2, `rsp`). That is
what makes migrating a verb a two-file change per slot rather than a backend rewrite, it is
why the monolith adapter is byte-identical (nothing on the monolith path changes at all),
and it is also the honest statement of the debt: P5b makes the verbs CROSS; retiring the
pulls they read is P3b/P4b/P7/P8's, exactly as CONTRACT-P5 table 2 already says, one row per
field. Under `MOBILEGL_IPC_STRICT_ERRORS=1` every such pull is still `Fatal`, so the debt
stays measurable.

**The census's correction, made structural.** None of the five P5 class-B verbs has an
`MGPipeApply*` entry point; they reach `WireVerbSink`. None of the 25 P5b slots has one
either and NONE GAINS ONE (§6.1). The seam a migration package writes is one
`ServerVerbSink::On<Row>` body plus one emit-table row on the client - and both already exist
as named stubs on this head.

---

## §1 Table 0 additions — encodings the P5b rows introduce

| field | the ruling | zero means | who reads it |
|---|---|---|---|
| **`MGPClear::Kind` / `::ValueClass`** | `kMGPipeClearKind{Whole=0, Color=1, Depth=2, Stencil=3, DepthStencil=4}`, `kMGPipeClearValueClass{Float=0, Int=1, Uint=2}`, in `MGPipeTypes.h` beside the struct. **One spelling.** Through P5 the numbers lived in `Client/EmitTables.h` and `Server/PipeApplier.h` as two hand-minted copies, each flagged for the integrator; both now alias the contract's. The GL-entry → Kind map is written beside the constants. | `Whole` with `BufferMask = 0`: a clear of nothing, legal | f1's emitters, `ServerVerbSink::OnClear` (live for all five kinds since v1) |
| **`MGPDrawFlagBit::kDrawIsIndirect` (1<<5)** | The draw's ranges come from a `GL_DRAW_INDIRECT_BUFFER`; `draw_vbo`'s SECOND TAIL is one `MGPDrawIndirect` instead of the user-index span. **Exclusive with `kDrawHasUserIndices`** and **`NumDraws` must be 0** - both `Fatal{ProtocolCorruption}` at the layout, on both sides. | clear = the range tail describes the draw | d1's emitters, the layout, `OnDrawVbo` |
| **`draw_vbo`'s second tail** | `WireRecordLayout::SecondTailIsHostSpans` says what it is: spans (`kDrawHasUserIndices`, and `set_shader_buffers` always) or the indirect block (`kDrawIsIndirect`). The encoder's host-span honesty pass reads the flag, not "a second tail exists" - a 40-byte indirect block read as spans is one span and a quarter of garbage. | — | codec |
| **GL enums on the wire** | `MGPMipPlan::Target`, `MGPCopyRegion::SrcTarget/DstTarget`, `MGPCopyFromFramebuffer::Target/InternalFormat`, `MGPImageBind::Access/Format`, `MGPPatchParameter::Pname`, `MGPMemoryBarrier::Bits`: **the GL token verbatim**, in a field wide enough for it (every GL texture target and access token fits a `Uint16`; formats and bitfields take a `Uint32`). Rule D: the sink passes the token to the backend slot that takes it. NOT `MGPipeResourceTarget`: the tree has no resource-target → GL-enum inverse to spend on a field the backend only ever forwards. | 0 = no target / no bits, refused where the backend would refuse it | sinks |
| **`MGPImageBind::Access`** | the GL access token (`GL_READ_ONLY` 0x88B8 …), NOT `MGPImageView::Access`'s three-value encoding (CONTRACT-P5 table 0 row `MGPImageView::Access`). Two different records, two different jobs: the set record describes a resolved unit for the draw prep, this record reproduces a call. | — | `OnBindShaderImage` |
| **GL names on the wire (`GlName` fields)** | `MGPCopyRegion::SrcGlName/DstGlName`, `MGPImageBind::GlName`, `MGPStreamOutputBind::GlName`, `MGPStorageBlockBinding::GlName`. **A GL name is never an identity** (ARCHITECTURE 4.2.1); it is either the key the backend has ALWAYS used (`XfbImpl::g_xfbObjects[name]`, `DirectGLES.cpp:1401`) or the key of a BARRIER-PULLED sticky forward the P5b sink resolves through (`MGB_CTX->GetTextureObject(name)`, `GetProgramObject(name)` - CONTRACT-P5 table 2's forward rows, retired by P7/P9). Every such field sits beside the handle that replaces it. | 0 = GL's name 0 (the default object / no texture) | sinks, through the forwards |
| **the block name blob** | `MGPStorageBlockBinding::Name`: `SEG_STAGE`, `Size = strlen + 1`, the NUL travels. Decoder bound: 4096 bytes; a run whose last byte is not NUL is `Fatal{ProtocolCorruption, "SetStorageBlockBinding.Name"}`. | `Size == 0` is Fatal (rule A) | decoder |

---

## §2 Table 1 additions — the 25 slots, by package

Columns per slot: **wire record** (row it rides, every new field and its carrier) · **class**
(all move C → B when the package flips them) · **sink** (`WireVerbSink` method; `ServerVerbSink`
override, a named-Fatal stub on this head) · **reply slot** · **thread rule** · **refusals** ·
**monolith**.

Every row: **no reply slot** (nothing the client acts on comes back - ID-31/R-5 apply to
the four acceptance rows only), **thread rule = CONTRACT-P5 table 3 unchanged** (the verb
barrier holds; the client's `EmitAndWait` blocks until `appliedSeq >= seq`; the server stamps
the verb boundary named in `FieldOwnership.def`'s `MGP_VERB_OP_LIST` before the sink runs;
`gPipeInputs` has one writer at a time), **monolith = untouched** (the client emit table
exists only under split; the backend slot is called directly as today; G2/G14 name-for-name).
Those three columns are therefore stated once here and the rows carry only what differs.

Refusal vocabulary, shared by all four packages: a record the codec cannot prove is
`Fatal{ProtocolCorruption, "<row>.<field>"}`; a shape the sink does not implement yet is
`Fatal{UnmigratedVerb, "<GL slot>[+<QUALIFIER>]"}` - the SAME family the client's class-C
table raises and the census greps, with the qualifier naming the arm
(`DrawElements+CLIENT_INDICES`, `CopyImageSubData+RENDERBUFFER`,
`ClearNamedFramebufferfv+UNBOUND`); a backend that leaves the slot null DECLINES
(`return false`), which is the monolith's null-slot answer in the same words.

### d1 — indexed / instanced / multi-draw / indirect (10 measured + 9 companions)

**One row: `draw_vbo` (59), `MGPDrawInfo` + `MGPDrawRange[NumDraws]` + a conditional second
tail.** ROADMAP P8's "`draw_vbo` 收编 multi-draw 族" is exactly this: the twenty GL draw entry
points collapse onto one record, and the `MGPDrawRange` array already has the shape the
`glMultiDraw*` family has. The P5 sink (`Server/PipeApplier.cpp` `OnDrawVbo`) implements
`NumDraws == 1`, non-instanced, VBO-backed; d1 fills in the cross product.

| slot (census entries) | record fields | sink dispatch (`GLFunctionsTable` slot the sink calls) |
|---|---|---|
| `DrawElements` (56 + every Minecraft trace) | `IndexSize` = 1/2/4 from `type`; `IndexResource` = the VAO's bound element buffer handle (the same handle `set_index_buffer` (32) carried at validate); one range `{Start = indices / IndexSize, Count = count, IndexBias = 0}`; `Flags = 0` | `DrawElementsBaseVertex(mode, count, type, (void*)(Start*IndexSize), 0)` - the P5 arm, unchanged |
| `DrawElementsBaseVertex` (4) | as above, `IndexBias = basevertex` | `DrawElementsBaseVertex(…, basevertex)` |
| `DrawElementsInstancedBaseVertex` (trace-only, `improved-transparency-minecraft-26.3`) | `InstanceCount = instancecount`, `IndexBias = basevertex` | `DrawElementsInstancedBaseVertex(mode, count, type, offset, instancecount, basevertex)` |
| `DrawArraysInstanced` (2) | `IndexSize = 0`, range `{first, count, 0}`, `InstanceCount` | `DrawArraysInstanced` |
| `DrawArraysInstancedBaseInstance` (4) | + `StartInstance = baseinstance`. The vertex-FETCH base instance already crossed in `set_vertex_buffers::BaseInstance` at validate (D-H1; `MGP_SET_BASE_INSTANCE` runs before `MGP_FILL`, `GL_Drawing.cpp:663-688`); `StartInstance` is `gl_BaseInstance`'s value and feeds the GL call | `DrawArraysInstancedBaseInstance` |
| `MultiDrawArrays` (2) | `IndexSize = 0`, `NumDraws = drawcount`, range i = `{first[i], count[i], 0}` | `MultiDrawArrays(mode, first[], count[], drawcount)` - the sink rebuilds the two arrays from the ranges (bounded locals; rule C) |
| `MultiDrawElements` (2) | `NumDraws = drawcount`, range i = `{indices[i] / IndexSize, count[i], 0}` | `MultiDrawElementsBaseVertex(mode, count[], type, offsets[], drawcount, zeros[])` |
| `MultiDrawElementsBaseVertex` (18) | range i `IndexBias = basevertex[i]` | `MultiDrawElementsBaseVertex(…, basevertex[])` |
| `MultiDrawElementsIndirect` (2) | `Flags = kDrawIsIndirect`, `NumDraws = 0`, second tail `MGPDrawIndirect{Buffer = the bound GL_DRAW_INDIRECT_BUFFER handle, ParameterBuffer = null, Offset = (Uint64)indirect, ParameterOffset = 0, Stride = stride, DrawCount = drawcount}` | `MultiDrawElementsIndirect(mode, type, (void*)Offset, DrawCount, Stride)` |
| `MultiDrawArraysIndirectCount` (2) | as above with `IndexSize = 0`, `ParameterBuffer = the bound GL_PARAMETER_BUFFER handle`, `ParameterOffset = (Uint64)drawcount` (the GLintptr the call spells `drawcount`), `DrawCount = maxdrawcount` | `MultiDrawArraysIndirectCount(mode, (void*)Offset, (GLintptr)ParameterOffset, DrawCount, Stride)` - the backend reads the count where it does today (`DirectGLES.cpp:6510`) |
| companions, unmeasured, same row, d1 owns the slots: `DrawElementsInstanced`, `DrawElementsInstancedBaseInstance`, `DrawElementsInstancedBaseVertexBaseInstance`, `DrawRangeElements`, `DrawRangeElementsBaseVertex` (`Flags |= kDrawHasIndexRange`, `MinIndex/MaxIndex = start/end`), `MultiDrawArraysIndirect`, `MultiDrawElementsIndirectCount`, `DrawArraysIndirect`, `DrawElementsIndirect` (`kDrawIsIndirect`, `DrawCount = 1`) | | |

**Sink:** `WireVerbSink::OnDrawVbo(const MGPDrawInfo&, const MGPDrawRange* ranges, const
MGHostSpan* userIndices, const MGPDrawIndirect* indirect)` - the P5 signature plus the
indirect block. On this head the sink declines by name: `MultiDrawArrays` /
`MultiDrawElements` for `NumDraws != 1`, `DrawArraysInstanced` / `DrawElementsInstanced` for
the instanced arms, `DrawElements+CLIENT_INDICES` for a span,
`MultiDraw{Arrays,Elements}Indirect` for the block.

**Client-side index arrays (no element buffer bound) — the P8 resolve-on-client rule,
applied.** The bytes exist on the client only, so the client resolves them: d1's emitter
stages `count * IndexSize` bytes at `indices` into `SEG_STAGE` and names the run in the
existing conditional `MGHostSpan` tail (`Flags |= kDrawHasUserIndices`, `Ptr = nullptr, Seg =
SEG_STAGE`). The codec already runs all four honesty arms on it
(`PipeWireCodec.cpp` `CheckHostSpanIsHonest(span, segments)`); the sink resolves it with
`MGPipeHostBytes` and passes the pointer as `indices`. This is the shape CONTRACT-P5 table 1
row 16 froze and table 0 said "P5 must produce none of"; P5b produces it for exactly the
case where nothing else can. **`kCapNeedsHostIndexBytes` stays 0**: it asks for the bytes of
VBO-backed index buffers (restart rewriting, multi-draw flattening) and is answered by P8's
`Server/IndexHostMirror` (ARCHITECTURE §10.3), not by a per-draw span. **A multi-draw with
client-side indices** (`indices[i]` are `drawcount` separate client pointers, one span can
name one run) is refused by name, `Fatal{UnmigratedVerb, "MultiDrawElements+CLIENT_INDICES"}`,
until P8's `HostResolve.cpp` flattens it; no measured workload has one.

**Client-side VERTEX arrays** (attribute pointers into client memory, no VBO) are NOT d1's
and are not a wire question: Espryt's `SyncClientSideAttributesForDrawArrays`
(`DirectGLES.cpp:6284-6291`) and its `DrawElements` twin upload them on the SERVER by reading
the frontend VAO through the BARRIER-PULLED `GetBoundVertexArray` (CONTRACT-P5 table 2, "P8").
Under inproc that read dereferences the client's pointers in the same address space and is
correct by the barrier; under spawn (P6) it is exactly the pull P8's `HostResolve.cpp`
(client array ranges, max-index scan) retires. d1 changes nothing there.

**`MinIndex / MaxIndex`** stay `~0` (unknown) for every P5b draw except the two
`DrawRangeElements*` companions; **`XfbCpuCapturedVertices`** rides only when the client's
XFB accounting has a value (`kDrawHasXfbCount`), which t2's rows make possible;
**`RestartIndex` / `kDrawPrimitiveRestart`** carry the frontend's primitive-restart state
verbatim (the backend's `ScopedRestartIndexSubstitution` reads the index bytes it needs on
its side - from the bound buffer, or from the resolved span).

**Stamp:** `DrawVbo → DrawArrays` (all twenty share `kDraw`'s mask; the Fatal NAME a pull
prints is `DrawArrays` for every one of them - known, accepted, and the reason d1 reports
next blockers by the wire's `UnmigratedVerb` name, which is exact).

**d1's gate:** the lane's `DrawElements` / `MultiDrawElementsBaseVertex` / `DrawArrays*` /
`DrawElementsBaseVertex` / `MultiDraw*` first-blocker entries → 0, each affected scenario at
its NEXT first blocker by name; **and every Minecraft trace reaches its next first blocker
under inproc, reported by name** (the census could not see past `DrawElements`).

### i1 — image / compute / barrier / copy-image / storage block (5 measured + 2 companions)

| slot (entries) | row (opcode) | record | sink | on this head |
|---|---|---|---|---|
| `BindImageTexture` (138) | **`bind_shader_image` (72), NEW**, `MGPImageBind` 40 B | `Res` = the texture's handle (null for 0), `Unit`, `GlName`, `Level`, `Layer`, `Layered`, `Access` = GL token, `Format` = GL enum. Emitted at the call (`GL_Texture.cpp:6736`), AFTER the frontend has written the unit's `ImageTextureBinding` and `MGP_FILL(BindImageTexture)` has run - the record's verb boundary is what makes the server's read of that binding legal. `set_shader_images` (37) still travels at the next validate, untouched: it is the draw-prep set, this is the call. | `OnBindShaderImage(const MGPImageBind&)` → `GL.BindImageTexture(Unit, GlName, Level, Layered, Layer, Access, Format)`. Espryt ignores everything but `Unit` and syncs the unit's binding from the barrier-pulled `GetImageTextureBinding` (`DirectGLES.cpp:9154`, `:2471`); Magma's slot is a no-op (`DirectVulkan.cpp:665`), so the same call is right for both. | `Fatal{UnmigratedVerb, "BindImageTexture"}` stub |
| `DispatchCompute` (49) | `launch_grid` (60), `MGPGridInfo` | `GridX/Y/Z` = the three counts; `Block* = 0` in P5b (the local size is a link artifact the backend reads from its own program; P7 Magma may fill it from the reflection archive); `IsIndirect = 0`. Companion `DispatchComputeIndirect`: `IsIndirect = 1`, `IndirectBuffer` = the bound `GL_DISPATCH_INDIRECT_BUFFER` handle, `IndirectOffset = indirect`. The emitter keeps b1's pre-verb hook ORDER the class-C stub already has: `PushPersistentMapsBeforeVerb(); MarkGpuWritesForDispatch();` then the record (`EmitTables.cpp` `DispatchCompute_Unmigrated`). | `OnLaunchGrid(const MGPGridInfo&)` → `GL.DispatchCompute(GridX, GridY, GridZ)` / `GL.DispatchComputeIndirect(IndirectOffset)` | stub, `"DispatchCompute"` / `"DispatchComputeIndirect"` by `IsIndirect` |
| `MemoryBarrier` (18) | `memory_barrier` (61), `MGPMemoryBarrier` | `Bits` = the GLbitfield verbatim (the frontend already validated it and already folds `glTextureBarrier` onto it, `GL_Drawing.cpp:920-937`); `ByRegion = 0`. Companion `MemoryBarrierByRegion`: `ByRegion = 1`. | `OnMemoryBarrier(const MGPMemoryBarrier&)` → `GL.MemoryBarrier(Bits)` / `GL.MemoryBarrierByRegion(Bits)`; Espryt's atomic-counter lowering stays inside the backend (`DirectGLES.cpp:8836`) | stub |
| `CopyImageSubData` (30) | `resource_copy_region` (53), `MGPCopyRegion` **64 → 72 B** | `Src/Dst` handles (kind Texture, or Renderbuffer when the target is `GL_RENDERBUFFER`), `SrcTarget/DstTarget` = the GL targets verbatim, `SrcBox = {srcX, srcY, srcZ, w, h, d}`, `DstX/Y/Z`, `SrcLevel/DstLevel`, **`SrcGlName/DstGlName`** (new). | `OnResourceCopyRegion(const MGPCopyRegion&)` → rebuilds two `CopyImageEndpoint`s (`BackendObject.h:33`) from the GL names through the barrier-pulled `MGB_CTX->GetTextureObject(name)` (`rsp`) and calls `GL.CopyImageSubData(src, SrcTarget, SrcLevel, SrcBox.X/Y/Z, dst, DstTarget, DstLevel, DstX/Y/Z, SrcBox.W/H/D)`. **A renderbuffer endpoint is refused by name** (`"CopyImageSubData+RENDERBUFFER"`): no sticky forward hands out a renderbuffer object, and P7 is where the backend takes handles. | stub |
| `ShaderStorageBlockBinding` (4) | **`set_storage_block_binding` (75), NEW**, `MGPStorageBlockBinding` 40 B, `kHasBlob` | `ShaderCso` = the program's CSO handle, `GlName` = the GL program name, `Binding`, `Name` = the block name staged in `SEG_STAGE`, `Size = strlen + 1`. Emitted at `GL_Program.cpp:3394` after the frontend recorded the binding on the program (`SetShaderStorageBlockBinding`, `:3392` - which is what reseeds a rebuilt backend program and what `GL_BUFFER_BINDING` reports; the record is the "push it onto the already-built driver program" optimisation Espryt performs, `DirectGLES.cpp:9182-9218`). | `OnSetStorageBlockBinding(const MGPStorageBlockBinding&, const char* name)` → `GL.ShaderStorageBlockBinding(GlName, name, Binding)`; both backends resolve the program through the barrier-pulled `GetProgramObject(GlName)` / `TryGetDirectVulkanProgram` (`rsp`, P9). | stub |

**The `copy-image-shadow-mirror` emulation** (`DirectGLES.cpp:8997`, `MGPipeUnmigratedEmulation`
→ `Fatal{UnmigratedEmulation}` under any non-monolith transport, `PipeApply.cpp:2947`).
Migrating `CopyImageSubData` moves the first blocker from the client's
`Fatal{UnmigratedVerb, "CopyImageSubData"}` to the server's
`Fatal{UnmigratedEmulation, "copy-image-shadow-mirror"}` on every Espryt copy between two
textures with CPU shadows. **Ruling (i1): under split the server SKIPS the mirror** - the site
becomes `if (monolith) mirror else skip`, behind `#if MOBILEGL_BUILD_DISAGGREGATED` so the
pull build's code does not move (G1) - and the client-side mirror ROADMAP P8 names ("CopyImage
镜像搬到 client") is P8's. What the skip loses is bounded by two Fatals: a later `glGetTexImage`
of the destination served from the client shadow (class C, wave 3, P9) and a texture re-mint
that re-uploads it (`texture-remint-pull`, also Fatal). i1 reports which scenarios reach either.

**Stamps:** `BindShaderImage → BindImageTexture` (kTextureOp), `LaunchGrid → DispatchCompute`
(kDispatch; `GetProgramForDispatch` is FATAL in CONTRACT-P5 table 2 because "there is no
compute on the reduced path" - **i1 moves it to BARRIER_PULLED, "P7 (Magma), P8 (Espryt)"**, the
same shape as `GetProgramForDraw`, in `FieldOwnership.def`; that is a one-row edit in a c0b file
and i1 may make it), `MemoryBarrier → MemoryBarrier`, `ResourceCopyRegion → CopyImageSubData`
(new row, §6.3), `SetStorageBlockBinding → ShaderStorageBlockBinding`.

**i1's gate:** the lane's five first-blocker counts → 0; each affected scenario at its next
first blocker by name (the census predicts `DispatchCompute` → `MemoryBarrier` → readback for
the image-store cases, and the emulation Fatal for some copies).

### t2 — transform-feedback spans, the XFB object bind, the patch parameter (3 measured + 4 companions)

| slot (entries) | row (opcode) | record | sink | on this head |
|---|---|---|---|---|
| `BeginTransformFeedback` (95) | `begin_stream_output` (62), `MGPStreamOutputBegin` | `PrimitiveMode` verbatim. Emitted at `GL_Drawing.cpp:1274` after the frontend's `BeginTransformFeedback(primitiveMode, program)` recorded the span - the capture program and the capture-buffer bindings the backend reads at the next draw (`StartPendingTransformFeedback`, `DirectGLES.cpp:1224-1250`) are `kXfbSpan`/`kDraw` BARRIER-PULLED fields (`GetTransformFeedbackProgram`, `GetBufferBindingPoint`…). **`set_stream_output_targets` (39) is NOT required for t2**: it has no applier and no producer, and the pulls cover the bindings while the barrier holds. Producing it is P9's (XFB scatter) and would need the consumer nobody can test yet (CONTRACT-P5 §7 `WireVerbSink` header). | `OnBeginStreamOutput(const MGPStreamOutputBegin&)` → `GL.BeginTransformFeedback(PrimitiveMode)` | stub |
| `PatchParameteri` (43) | **`patch_parameter` (73), NEW**, `MGPPatchParameter` 8 B | `Pname = GL_PATCH_VERTICES`, `Value`. Espryt pushes `glPatchParameteri` at the call (`DirectGLES.cpp:8760`) and NOT from its draw sync, so `set_patch_state` (43) - which carries the same number into the applier's working block at validate - cannot stand in; both travel, as both pushes happen today. Emitted at `GL_Drawing.cpp:844`. Magma registers no slot: the sink's null-slot DECLINE is the monolith's null-slot skip in the same words. | `OnPatchParameter(const MGPPatchParameter&)` → `GL.PatchParameteri(Pname, Value)` | stub |
| `BindTransformFeedback` (2) | **`bind_stream_output` (74), NEW**, `MGPStreamOutputBind` 16 B | `GlName` (Espryt's own key, `XfbImpl::g_xfbObjects[name]`, `DirectGLES.cpp:1401`; 0 = the default object), `LifetimeId` (the D21 counter-slot rekey's key; the frontend's `GetBoundTransformFeedbackLifetimeId`). Emitted at `GL_Drawing.cpp:1673` after `BindTransformFeedbackObject(id)`. | `OnBindStreamOutput(const MGPStreamOutputBind&)` → `GL.BindTransformFeedback(GlName)` | stub |
| companions, same rows, t2 owns the slots: `EndTransformFeedback` → `end_stream_output` (63) `MGPXfbAccounting{CapturedVertices, PrimitivesWritten, PrimitiveMode}` = the frontend's own accounting at the end (`GL_Drawing.cpp:1371`; the client's fence wait there is dropped per ARCHITECTURE 8.5 / CONTRACT-P5 table 2's "two new producers" - the capture targets are marked GPU-written instead); `PauseTransformFeedback` / `ResumeTransformFeedback` → (64)/(65) `MGPStreamOutputControl{Reserved = 0}`; `DeleteTransformFeedback` has no row in P5b (unmeasured; a `bind_stream_output` of name 0 is what the backend does on delete of the bound object, `DirectGLES.cpp:1422`, and the driver object leak until P9 is recorded, not solved). | | `OnEndStreamOutput` / `OnPauseStreamOutput` / `OnResumeStreamOutput` → the three GL slots | stubs |

**`FixupGsStripCaptureOrder` (`GL_Drawing.cpp:1290`)** probes `EndTransformFeedback != nullptr`
to decide whether the backend owns the capture. `SlotCaps.h` deliberately did not convert this
probe ("absent would be wrong"); with t2's emitter installed the slot is non-null on the
client, which for a server running Espryt is the right answer and for a server running Magma
(slot null on the server, the sink DECLINES) is WRONG in the P5 direction: the client would
skip a reorder Magma needs. **Ruling:** t2 answers the probe from the caps mirror - the
existing `kCapCpuXfbPrimitiveAccounting`-style shape, a new cap bit
`kCapBackendOwnsXfbCapture` (1<<9) in `MGPCapBit`, set by the server from its own table's
`EndTransformFeedback != nullptr` - through `MGL_BACKEND_SLOT_CAP`. That is the one new cap
bit of P5b and it is c0b's file; t2 adds it (§6.5).

**Stamps:** `BeginStreamOutput → BeginTransformFeedback`, `EndStreamOutput →
EndTransformFeedback`, `Pause/Resume` likewise (all pre-existing rows), `PatchParameter →
PatchParameteri` (kQuery: reads nothing), `BindStreamOutput → BindTransformFeedback` (kXfbSpan).

**t2's gate:** the three first-blocker counts → 0; each scenario at its next blocker by name
(the census predicts `EndTransformFeedback` or an XFB query end - the query family is wave 3
and stays `Fatal{UnmigratedVerb}`).

### f1 — the clear family, the framebuffer-sourced copies, mips (7 measured + 4 companions)

| slot (entries) | row (opcode) | record | sink | on this head |
|---|---|---|---|---|
| `ClearBufferiv` (12), `ClearBufferuiv` (8), `ClearBufferfv` (6), `ClearBufferfi` (1) | `clear` (57), `MGPClear` | `Fbo = kMGPipeNullHandle` (the bound draw framebuffer, as `EmitClear` does), `Kind` / `ValueClass` per the table in `MGPipeTypes.h` beside the constants, `DrawBufferIndex = drawbuffer`, `ColorValue[4]` = the four values' bits (`memcpy`; a float clear crosses as its bit pattern, exactly as MOBILEGL_PIPE_VERIFY compares floats), `DepthValue`, `StencilValue`. Emitted at `GL_Framebuffer.cpp:2829-2850` with `BeforeReadOnlyVerb()` first, as `EmitClear`. | `OnClear` - **already live for all five kinds** (`PipeApplier.cpp` `OnClear`): `GL.ClearBuffer{fv,iv,uiv,fi}` by `Kind`/`ValueClass`. The only work is the client. | live |
| `ClearNamedFramebufferfv` (2) + companions `fi/iv/uiv` | `clear` (57) | as above with **`Fbo` = the named framebuffer's handle**, preceded by its `MGPipeFramebufferTarget::Named` record (ID-19; the frontend already emits it: `PipePublishFramebufferByName`, `GL_Framebuffer.cpp:690`). | `OnClear`: `Fbo` null or equal to the applier's `BoundFramebuffer[Draw]` → the bound-form clear above. **Otherwise refused by name, `Fatal{UnmigratedVerb, "ClearNamedFramebufferfv+UNBOUND"}`**: the backend slot takes a frontend `SharedPtr<FramebufferObject>` (`DirectGLES.cpp:9308`), no forward hands one out, and the bound-form entries re-sync the CLIENT's binding through the barrier-pulled `GetFramebufferBindingSlot` (`SyncCurrentFBO`), so a server-side rebind-clear-restore through the applier would be undone by the pull. The handle-keyed `SyncAndBindFramebufferObject` sibling Espryt needs (`DirectGLES.cpp:4409`) is P7's / P3b-P4b's, or f1's stretch behind `#if MOBILEGL_BUILD_DISAGGREGATED` if the one measured scenario needs it. | live for the bound case; the unbound refusal is f1's to add |
| `CopyTexImage2D` (3) + companion `CopyTexSubImage2D` | **`copy_framebuffer_to_texture` (76), NEW**, `MGPCopyFromFramebuffer` 48 B | `Dst` = the bound texture's handle at the active unit (P8 form), `Target` = the GL target verbatim, `Level`, `InternalFormat` (image form; 0 for sub-image), `X, Y, Width, Height` = the read-framebuffer rectangle, `XOffset, YOffset` (sub-image form), `SubImage = 0/1`. The image form's level redefinition has already crossed as the level's `resource_respecify` from the frontend's own `TexImage2D_State` (`GL_Texture.cpp:4541`) before the backend slot is reached (`:4550`); the record carries only the copy. The read framebuffer is the bound one (`set_framebuffer_state`). | `OnCopyFramebufferToTexture(const MGPCopyFromFramebuffer&)` → `GL.CopyTexImage2D(Target, Level, InternalFormat, X, Y, Width, Height, 0)` or `GL.CopyTexSubImage2D(Target, Level, XOffset, YOffset, X, Y, Width, Height)`; the backend resolves the bound texture through the barrier-pulled unit state (`DirectGLES.cpp:8528-8531`, kBlitOrCopy's mask). | stub, `"CopyTexImage2D"` / `"CopyTexSubImage2D"` |
| `GenerateMipmap` (2) | `generate_mipmap` (54), `MGPMipPlan` | `Res` = the bound texture's handle, `Target` = the GL target verbatim, `BaseLevel/LevelCount` from the texture's base level and level count (informational in P5b). Emitted at `GL_Texture.cpp:1681`. | `OnGenerateMipmap(const MGPMipPlan&)` → `GL.GenerateMipmap(Target)`; the backend resolves the bound texture through the barrier-pulled unit state (`DirectGLES.cpp:8769-8772`). | stub |

**The two mipmap emulations** (`generate-mipmap-storage` `DirectGLES.cpp:8051`,
`generate-mipmap-cpu-fallback` `:8702`, both `Fatal{UnmigratedEmulation}` under split) are
reached only for `R11FG11FB10F`, depth-only, `RGB16F` and `RGB32F` textures
(`DirectGLES.cpp:8774-8778`). **Ruling (f1):** migrating `GenerateMipmap` moves those cases'
first blocker to the emulation Fatal, reported by name; every other format renders. The
storage grow reads the frontend level shadows to DECIDE the levels and could be re-derived from
the applier's descriptor (`Levels`) - f1 may do that behind `#if MOBILEGL_BUILD_DISAGGREGATED`;
the CPU three-channel fallback needs `OnTextureWriteback` (ROADMAP P8/P9) and stays Fatal.

**Stamps:** `Clear → Clear` (pre-existing), `CopyFramebufferToTexture → CopyTexImage2D` (the
sub-image companion shares the kBlitOrCopy mask), `GenerateMipmap → GenerateMipmap`.

**f1's gate:** the seven first-blocker counts → 0; each scenario at its next blocker by name.

---

## §3 Table 2 — field ownership under P5b

**P5b adds no PipeInputs field and retires none.** Every one of the 25 verbs reads, on the
server, exactly the fields its verb class's may-read mask names (`FillPoints.def`) and finds
them in the class CONTRACT-P5 table 2 already assigned: RECORD_SUPPLIED where a record
carries them, BARRIER_PULLED (counted in `rsp`) for the frontend-object and shutter fields,
FATAL for the three off-path ones. Two of the three FATAL rows are reached by P5b verbs and
move:

| field | CONTRACT-P5 class | P5b | why |
|---|---|---|---|
| `GetProgramForDispatch` | FATAL ("no compute on the reduced path") | **BARRIER_PULLED, "P7 (Magma), P8 (Espryt)"** | i1 puts compute on the path; the field is `GetProgramForDraw`'s twin (`DirectGLES.cpp:5779`, `VulkanRenderer.cpp:7327`) and takes its class and its retiring phases. i1 edits the row. |
| `GetTransformFeedbackPausedPrimitiveCounter` | FATAL ("reachable only from kQuery") | unchanged | its one reader is Magma's XFB query end (`kQuery`), and the query family is wave 3. |
| `GetBoundTransformFeedbackName` | FATAL (dead) | unchanged | |

**`rsp` grows, and that is the phase's measurement.** Every P5b verb that draws, dispatches
or copies pulls the same 20 non-sticky rows a P5 draw pulled plus the sticky forwards its arm
uses (`GetTextureObject` for `CopyImageSubData`, `GetProgramObject` for
`ShaderStorageBlockBinding`). The end-of-P5b `rsp` per Minecraft frame is the number that
sizes P3b/P4b/P7/P8, and each package's report carries it for its scenarios.

**The server stamp:** every P5b row that is a verb boundary has a `MGP_VERB_OP_LIST` row in
`FieldOwnership.def` on this head (18 rows; `FieldOwnershipTest` pins them). A P5b record that
arrived with NO stamp row would apply under the previous verb's serial and read a stale field
as fresh - the one silent failure the table has - which is why the six rows landed in c0b
rather than being left to the packages.

---

## §4 Table 3 — role and thread ownership under P5b

Unchanged, and the one thing to restate: **the verb barrier holds for every P5b verb.**
`ClientSession::EmitAndWait` blocks the GL thread until `appliedSeq >= seq` for every one of
the 25 records as it does for the five P5 verbs; the server's `PipeApplier::ApplyOne` stamps
the verb boundary, runs the sink, clears the stamp, and only then does the session publish
`appliedSeq` (R-1, R-9). The pre-verb hooks keep their P5 order on the client:
`PushPersistentMapsBeforeVerb()` then `MarkGpuWritesFor{Draw,Dispatch}()` then the record
(`EmitTables.cpp` `BeforeDrawVerb` / `DispatchCompute_Unmigrated`; ID-18). A P5b emitter for a
verb that reads buffers but starts no shader (the clears, the copies, the mip generation, the
barrier, the XFB and patch controls) calls `BeforeReadOnlyVerb()`.

**What the barrier buys P5b and what it costs.** It is why Rule D works at all - the server's
backend reads the client's `gPipeInputs` fill of the moment - and it is why P5b's inproc is
lockstep and slow (P5 said so; "P5 预期变慢" is P5b's expectation too, recorded, not gated).
Retiring it per family is P9's, after the pulls it protects are gone.

---

## §5 Knobs

**No new knob.** `MOBILEGL_TRANSPORT=inproc` with the P5 defaults is P5b's whole
configuration. One cap bit is added (§6.5, `kCapBackendOwnsXfbCapture`), which is not a knob.
`MOBILEGL_IPC_STRICT_ERRORS=1` remains the way to see the pulls a P5b verb costs, and
`MOBILEGL_IPC_AUDIT=1` remains rule C's control - now with a second consumer worth auditing,
the user-index span.

---

## §6 Rulings this file makes, and what would overturn each

1. **No P5b slot gains an `MGPipeApply*` entry point; all 25 reach `WireVerbSink`.** The
   applier's 37 entry points are the object and state families and write `gPipeInputs` /
   the applier's records; a verb is a backend CALL, and the census's correction - the five P5
   class-B verbs already go to the sink - is the pattern, not the exception. The generated
   route tables (`PipeRoute.h`, R-17, 33 + 4 = 37) are therefore untouched and
   `PipeCatalogueTest`'s partition still reads 33 installed + (76 − 33) null + 4 escapes.
   *Overturned by:* a verb that must be replayable from the monolith side through a table
   (the MGPipe recorder, ROADMAP P13) - at which point every sink method becomes a table row
   and the monolith adapter is the backend call; none of P5b needs it.

2. **Five rows appended, none inserted, opcodes 72..76.** `bind_shader_image`,
   `patch_parameter`, `bind_stream_output`, `set_storage_block_binding`,
   `copy_framebuffer_to_texture`. Each exists because the backend does work AT THE CALL that
   no validate-time set record reproduces (§0 Rule D; the evidence per row is in
   `MGPipeTypes.h` beside the struct). *Overturned by:* the backend moving that work into its
   draw prep (e.g. Espryt applying `glPatchParameteri` from `SyncRenderState`), which is a G5
   byte-identical region and therefore not P5b's to touch.

3. **`resource_copy_region` is `glCopyImageSubData` and nothing else.** CONTRACT-P5 / the
   `FieldOwnership.def` header left "which of three verbs" to the emitting phase; P5b gives
   the two framebuffer-sourced copies their own row (76) because they have a different
   source (the read framebuffer, no handle) and a different shape (a level redefinition on
   the image form). *Overturned by:* nothing cheaper than a second stamp rule.

4. **`kCapNeedsHostIndexBytes` stays 0; the user-index span is armed anyway.** The cap is the
   server's request for VBO-backed index bytes (P8's mirror); the span is the client's only
   way to hand over bytes only it has. The two were conflated in CONTRACT-P5 table 0's cap-bit
   row ("the two bits are the only things that ask for [a span]") because P5 produced neither.
   *Overturned by:* a measured draw whose client-side index array exceeds what one staged
   run can hold at the default `SEG_STAGE` (32 MiB; the R-10 `maxrec=` counter is what would
   say so), at which point d1 chunks or refuses by size.

5. **One new cap bit, `kCapBackendOwnsXfbCapture` (1<<9).** The `EndTransformFeedback !=
   nullptr` probe at `GL_Drawing.cpp:1290` decides whether the CLIENT reorders captured
   records; under split the answer is the SERVER's table, so it is a cap, set by the server
   from `GL.EndTransformFeedback != nullptr` and read through `MGL_BACKEND_SLOT_CAP`. It is the
   first of the "XFB span family" cap bits `SlotCaps.h` said would come; t2 adds it to
   `MGPCapBit` (c0b's file, granted for this bit) in the same commit that flips the probe.
   *Overturned by:* Magma registering `EndTransformFeedback`, which removes the difference.

6. **`MGPCopyRegion` widens 64 → 72 for two GL names.** A record with no producer and no
   consumer has no wire history to preserve; the names are the barrier-pulled lookup keys
   the P5b sink needs and the handles beside them are the identities P7 will use.
   *Overturned by:* P7 landing handle-keyed endpoints, when the two fields become padding
   again (and stay in the layout - a shrink would move every field behind them).

7. **Renderbuffer endpoints, unbound DSA clears and multi-draws with client-side indices are
   refused BY NAME, ID-57's shape.** Each is a form no barrier-pulled forward can serve, none
   was measured, and a named Fatal at the client or sink is the outcome R-4 asks for; a
   silently wrong picture is the one it forbids. *Overturned by:* a measured workload hitting
   one, which then gets its P7/P8 mechanism moved forward.

8. **The `copy-image-shadow-mirror` site is skipped under split; the two mipmap emulation
   sites stay Fatal.** The mirror's loss is bounded by two other Fatals (§2 i1); the mipmap
   storage grow may be re-derived from the descriptor, the CPU fallback cannot be without
   `OnTextureWriteback`. *Overturned by:* a measured `GetTexImage` of a copy destination
   before P9's readback carrier lands - which would show as the wave-3 `GetTexImage` Fatal
   anyway.

9. **`GetProgramForDispatch` moves FATAL → BARRIER_PULLED.** §3. *Overturned by:* nothing;
   the FATAL class was a statement about the reduced path, and P5b widens the path.

10. **The emit table's class-C list is partitioned per package** (`EmitTables.cpp`:
    `MGR_UNMIGRATED_{D1,I1,T2,F1,TAIL}_SLOTS`, `kEmittedSlots{D1,I1,T2,F1}`, five ownership
    `static_assert`s: 19 / 7 / 7 / 11 / 20 = 64). Four packages editing one X-macro list and
    one count would conflict on every landing; four lists and four counts do not, and a
    package that touches another's list breaks the other's assertion. *Overturned by:*
    nothing - it is arithmetic, and the totals `UnmigratedSlotCount() == 64` /
    `ImplementedVerbCount() == 5` the P5 tests pin are unchanged on this head.

11. **The server stubs are `Fatal{UnmigratedVerb, "<GL slot>"}`, the same family and name
    as the client's**, suffixed "(server sink)" in the message text only. A client flipped
    ahead of its server half therefore aborts under the census's own grep with the right
    slot name, and the lane's first-blocker table stays meaningful across a partial landing.
    *Overturned by:* nothing.

12. **Client-side vertex arrays are not a P5b question.** They are served on the server
    through the barrier-pulled `GetBoundVertexArray` (CONTRACT-P5 table 2, "P8"), correct
    under inproc by the barrier and the shared address space, and retired by P8's
    `HostResolve.cpp`. *Overturned by:* P6's spawn transport - which is after P5b by ID-68.

---

13. **Measured named blits use the existing `blit` row with a scoped client binding override.**
   P5b's next trace census reaches `BlitNamedFramebuffer` in iris-BSL and
   improved-transparency on both backends. The client saves its read/draw binding objects,
   binds the named arguments in the client shadow only, validates `BlitNamedFramebuffer`,
   and emits the original rectangles/mask/filter plus both original `MGPBlit` handles.
   The barrier holds those bindings until the server's bound `BlitFramebuffer` finishes;
   then RAII restores both client bindings. Their version changes make the next ordinary
   verb republish the restored state. The existing classified BARRIER-PULLED accessors
   remain the sole frontend read path; no pointer is added to the record or to a side channel,
   and no driver call occurs on the client. Name 0 carries `kMGPipeDefaultFramebuffer`.
   This adds no opcode and supersedes only named-blit's wave-3 deferral. *Overturned by:*
   removing the lockstep barrier or adding process separation; then both framebuffer
   handles need server-only resolution before dispatch. Pixel/binding gates cover unbound
   endpoints, default endpoints and ordinary clear/blit immediately after restoration.

## §7 The 71 slots after P5b's contract commit, and after the four packages

On this head the partition is CONTRACT-P5 §7's, unchanged: **A = 2, B = 5, C = 64**
(`RemoteClientTest` pins it). When the four packages land it is **A = 2, B = 5 + 25 (+ the
companions each package flips, up to 44), C = the wave-3 tail (20 at least)**, and the
`static_assert`s in `EmitTables.cpp` say which package moved which slot.

The wave-3 tail no P5b package owns, by name, so nobody discovers it by grep: `BlitNamedFramebuffer`,
`GetTexImage`, `GetTextureImage`, `WaitSync`, `DeleteSync`, `FenceSync`, `ClientWaitSync`,
`GetSyncStatus`, `BeginTimeElapsedQuery`, `EndTimeElapsedQuery`, `QueryCounterTimestamp`,
`IsQueryResultAvailable`, `GetQueryResult64`, `DeleteBackendQuery`, `BeginOcclusionQuery`,
`EndOcclusionQuery`, `BeginXfbPrimitivesQuery`, `EndXfbPrimitivesQuery`, `GetGpuTimestampNs`,
`SetSwapInterval` - P9 (readbacks), P10 (queries, syncs, swap interval).

---

## §8 Ownership

- `MG_Pipe/MGPipeTypes.h`, the three `.def` files, `FieldOwnership.def`, `gen_pipe*.py`,
  `CONTRACT-P5.md`, `CONTRACT-P5B.md`: c0b's (inherited from c0). A package that needs a payload
  shape changed goes through the integrator. Two grants are pre-made in this file: t2 may add
  `kCapBackendOwnsXfbCapture` (§6.5); i1 may edit `GetProgramForDispatch`'s row (§6.9).
- `MG_Remote/Wire/PipeWireCodec.{h,cpp}`: w1's; c0b landed the sink methods and the arms; a
  package edits ONLY the layout arm of a row it owns, and only with the integrator.
- `MG_Remote/Server/PipeApplier.{h,cpp}`: v1's; each package replaces ITS stub bodies and
  nothing else (the per-package blocks are marked).
- `MG_Remote/Client/EmitTables.{h,cpp}`: c1's; each package edits ITS `MGR_UNMIGRATED_*_SLOTS`
  list, ITS `kEmittedSlots*`, and adds ITS emitters in the class-B block.
- `MG_Test/Wire/PipeWireCodecTest.cpp`: each package appends cases for its rows below c0b's.
- The backends (`MG_Backend/DirectGLES`, `DirectVulkan`): untouched by c0b; a package that
  must touch one does so behind `#if MOBILEGL_BUILD_DISAGGREGATED` and names the region in
  its report (G1 admits no pull-build symbol motion; G5's untouched regions are pinned).
