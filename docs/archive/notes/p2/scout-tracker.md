# Scout: what the P2 frontend state tracker must do, and what already exists

Worktree: `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`, branch `feat/disaggregated` @ `e7a6a72f`
(`[Docs] (Disaggregated): record the P1 landing and what its verify lane found`).
All paths below are relative to that worktree root. Every line number was opened and read.

Design context the tracker has to satisfy, in the repo's own words:
- `docs/Disaggregated/ARCHITECTURE.md:147` — "前端 state tracker（`MG_Impl/Pipe/Tracker`，P2 起）".
- `docs/Disaggregated/ARCHITECTURE.md:149-151` — the push happens **at the validate moment before a verb, not in the GL setter**; per-setter push is explicitly rejected because Blaze3D brackets every batch with `glEnable/glDisable(GL_BLEND)`.
- `docs/Disaggregated/ARCHITECTURE.md:153` — **eight** validate entries generated from `PipeCalls.def`'s `kCtxVerb`/`kCtxObject` rows: `ValidateForDraw` (20 draw entries), `ValidateForDispatch`, `ValidateForClear`, `ValidateForBlitOrCopy`, `ValidateForTextureOp`, `ValidateForReadback`, `ValidateForXfbSpan`, `ValidateForQuery`. Note this is **eight**, while the landed P1 fill machinery has **nine** classes (`kProgramOp` was split out later — `MobileGL/MG_Pipe/FillPoints.def:106-109`). The tracker either adopts nine or has to explain the merge.
- `docs/Disaggregated/ARCHITECTURE.md:552` — `MobileGL/MG_Impl/Pipe/` is where `Tracker, SlotAllocator, CsoCache, HostResolve, CompositeResolver` land at P2+. Today that directory contains **only** `PipeFill.h` / `PipeFill.cpp`.
- `docs/Disaggregated/ROADMAP.md:18` — P2 deliverable is `MG_Impl/Pipe/Tracker` (dirty bits, **5 aggregate generations**, suppressor skeleton) + first `gen_pipe_dirty_surface.py` mapping round **as a gate**; acceptance includes "两台设备配对逐线程 CPU p50/p99 不差且 tracker 绝对 ns 在上限内".

---

## 1. The P1 fill machinery the tracker replaces or wraps

### 1.1 The fill point macro — `MobileGL/MG_Impl/Pipe/PipeFill.h`

- `PipeFill.h:16` — the whole header is under `#if MOBILEGL_PIPE_PUSH`; `PipeFill.h:52` defines `MGP_FILL(Verb)` → `MGPipeFillForVerb(MGPipeVerb::Verb)`; `PipeFill.h:54` makes it `((void)0)` in the pull build, so the pull build is byte-identical to a tree without it (verified in P1 acceptance, `docs/Disaggregated/MEASUREMENTS.md:133`).
- `PipeFill.h:25` — `void MGPipeFillForVerb(MGPipeVerb verb)`.
- `PipeFill.h:34` — `void MGPipeLeaveVerb()`: ends the verb in flight without starting another. Nothing in the GL entry points calls it; it exists for `MG_Test/ScopedPipeVerb.h` (a real 79-line header, present).
- `PipeFill.h:42` — `MGPipeSetPoisonOmission(const char* verb, const char* field)`: negative control B.
- `PipeFill.h:49` — `SnapshotFromGLContext(PipeInputs&, const MGPipeFieldMask&)`, verify builds only; the comment says this is "the branch that survives P13".
- Build wiring: `CMakeLists.txt:28` (`MOBILEGL_PIPE_PUSH` option), `CMakeLists.txt:29` (`MOBILEGL_PIPE_VERIFY`), `CMakeLists.txt:462-464` (VERIFY forces PUSH), `CMakeLists.txt:471` (`MobileGL/MG_Impl/Pipe/PipeFill.cpp` appended only under PUSH), `CMakeLists.txt:548-552` (the `-D` defines).

### 1.2 The filler and the per-class copy loop — `MobileGL/MG_Impl/Pipe/PipeFill.cpp`

`MGPipeFillForVerb` is `PipeFill.cpp:566-613`. In order:

1. `PipeFill.cpp:568` `ParsePoisonOmissionKnob()` (string compare per fill, `PipeFill.cpp:310-327`).
2. `PipeFill.cpp:570` `ArmVerify()` under verify; `PipeFill.cpp:574-577` warns in a push-without-verify build if the runtime knob is set.
3. `PipeFill.cpp:584` `++filled.CurrentVerbSerial` — **the poison stamp**. Starts at 1 so `FilledGen == 0` means "never filled" (`PipeFill.cpp:581-583`).
4. `PipeFill.cpp:586` `SetVerb`, `PipeFill.cpp:588` `SetIdentity` (writes `m_live` + `m_contextIdentity`), `PipeFill.cpp:589` early return when no context is live.
5. `PipeFill.cpp:590` picks the mask: `kMGPipeClassFieldMask[kMGPipeVerbClass[verb]]`.
6. `PipeFill.cpp:591-609` **the per-class copy loop**: iterate all 63 field ids, skip those outside the mask, stamp-only for sticky fields (`PipeFill.cpp:595-599`: `if (filled.FilledGen[i] == 0) filled.FilledGen[i] = 1;`), otherwise `CopyField` (`PipeFill.cpp:604`) then `filled.FilledGen[i] = filled.CurrentVerbSerial` unless the pair is the armed omission (`PipeFill.cpp:607`).
7. `PipeFill.cpp:611` `EntryCompare(inputs, mask)` under verify.

`MGPipeLeaveVerb` is `PipeFill.cpp:555-563`: bumps the serial (so every stamp goes stale) and sets the verb back to `kVerbCount`.

**`CopyField` is the whole per-field copy switch**, `PipeFill.cpp:36-266`. It is a flat `switch` over `MGPipeInputField` that calls the **same-named GLContext accessor** for each field — P1 brief D4's "no derivation logic is re-implemented here". The loops inside it are what makes a draw fill expensive; see §5.1.

Notable copy shapes worth knowing before rewriting them:
- `PipeFill.cpp:64-73` `GetBufferBindingSlot`: copies **pointers** to the live slots for every `GlobalBufferTargets` entry (14 targets, `MG_State/GLState/BufferState/BufferState.h:15-19`); the `Index` target is deliberately left null because GLContext derives it through the bound VAO's element-buffer slot — a derivation no `FillPoints.def` row can copy.
- `PipeFill.cpp:74-80` `GetBufferBindingPoint`: stores the **base address** of each target's binding-point row for the 4 `BufferBindPointTargets` (`BufferState.h:20-21`), each row being 84 wide (`BufferState.h:29`).
- `PipeFill.cpp:129-133` / `PipeFill.cpp:205-208`: `GetImageTextureBinding` / `GetTextureUnitObject` store the **base pointer** of the 192-entry unit arrays (`MG_State/GLState/TextureState/TextureState.h:42`, `MAX_TEXTURE_IMAGE_UNITS = 192`) — O(1), not O(192).
- `PipeFill.cpp:183-185` `GetRenderStateParameters`: a **whole-struct value copy** of `RenderStateParameters` (1168 bytes, `docs/Disaggregated/MEASUREMENTS.md:86`).
- `PipeFill.cpp:255-264`: the seven forwarded fields fall through with no copy.

### 1.3 Poison stamping and the read check

- `MobileGL/MG_Backend/MGPipe/PipeInputs.h:21-26` derives `MOBILEGL_PIPE_POISON` (on under DEBUG log level, or `MOBILEGL_BUILD_DISAGGREGATED`, or `MOBILEGL_PIPE_VERIFY`).
- `PipeInputs.h:44-49` `MGP_INPUT_CHECK(Field)` → `MGPipeInputFieldIsFresh(m_filled, Field)` else `MGPipeInputPoisonFatalForVerb(Field, m_currentVerb)`.
- `MobileGL/MG_Pipe/generated/PipeFilled.inc:302-304` — `struct MGPipeFilledState { Uint64 CurrentVerbSerial; Uint64 FilledGen[63]; }`, i.e. **512 bytes of stamps** carried inside `PipeInputs` in poison builds (`PipeInputs.h:669-671`).
- `PipeFilled.inc:316-320` `MGPipeInputFieldIsFresh`: `gen != 0 && (sticky || gen == CurrentVerbSerial)`.
- `PipeFilled.inc:96` `static_assert(kMGPipeInputFieldCount == 63, ...)`; `PipeFilled.inc:167` `kMGPipeInputFieldSticky[]`; `PipeFilled.inc:236` `kMGPipeInputFieldFilledBy[]`.
- `MobileGL/MG_Backend/MGPipe/PipeInputs.cpp:20` `MGPipeVerbName`, `:25` `MGPipeInputPoisonFatalForVerb`, `:29` `MGPipeFindInputField`, `:37` `MGPipeFindVerb`.

### 1.4 The verify hooks

- Entry compare: `PipeFill.cpp:412-422` `EntryCompare` → `SnapshotFromGLContext` (`PipeFill.cpp:427-437`) → `MGPipeVerifyInputs` → `ReportDivergence(..., "entry")`. `PipeFill.cpp:337` notes the entry arm is **tautological until P2 gives the first arm a real filler** — i.e. the tracker is exactly what makes this comparison meaningful.
- Compare-at-read: `PipeInputs.h:62-63` `MGP_INPUT_VERIFY_READ` → `MGPipeVerifyReadHook` (`PipeFill.cpp:439-463`). This is the arm that is real in P1: it re-reads the whole field from the live context at every backend read and compares. `PipeFill.cpp:455-458` uses an `InHook` flag so the re-read cannot recurse.
- `PipeFill.cpp:350` `static_assert(MOBILEGL_PIPE_POISON, ...)` — the read hook relies on the poison covering reads before the first fill.
- Negative controls: `MOBILEGL_PIPE_VERIFY_CORRUPT` perturbs the **snapshot** arm (`PipeFill.cpp:417-419`, injector at `MobileGL/MG_Backend/MGPipe/PipeInputs.cpp:172`); `MOBILEGL_PIPE_POISON_OMIT` withholds one `(verb, field)` stamp (`PipeFill.cpp:293-331`, `PipeFill.cpp:493-511`).
- Comparator API: `PipeInputs.h:250` `MGPipeInputsFieldEqual`, `PipeInputs.h:258` `MGPipeVerifyInputs` (exported with default visibility so CI can `nm -D` it — `PipeInputs.h:252-257`), `PipeInputs.h:264` `MGPipeApplyVerifyCorruption`.
- Field-wise (never `memcmp`) equality lists live in `MobileGL/MG_Pipe/PipeFields.def`; `PipeFields.def:230-247` is `RenderStateParameters` (65 members), `:249-251` `PixelStoreParameters`, `:253-258` `PerBufferBlendState` / `StencilFaceState`. `PipeFields.def:9-19` explains why padding is excluded and that `gen_pipe.py` asserts each list names exactly the struct's direct members.

### 1.5 `MobileGL/MG_Pipe/FillPoints.def` — 69 verbs, 9 classes, the may-read masks

- `FillPoints.def:35-104` `MGP_FILL_VERB_LIST`: **69 rows**, one per function-pointer member of `MG_Backend::GLFunctionsTable` **in declaration order**; `gen_pipe.py` parses that struct and refuses any other row set (`FillPoints.def:17-20`). `Present` and `SetSwapInterval` go through `BackendObject` virtuals and are not verbs (`FillPoints.def:21-22`).
- Class distribution in that list: 20 `kDraw` (`FillPoints.def:36-55`), 9 `kClear` (`:56-64`), 5 `kBlitOrCopy` (`:65-69`), 2 `kTextureOp` (`:70`, `:78`), 3 `kReadback` (`:71-73`), 2 `kDispatch` (`:74-75`), 6 `kXfbSpan` (`:98-103`), 1 `kProgramOp` (`ShaderStorageBlockBinding`, `:80`), the rest `kQuery`.
- `FillPoints.def:108-109` `MGP_FILL_CLASS_LIST`: the **nine** classes. The comment says ARCHITECTURE.md names eight and `kProgramOp` is split out because `ShaderStorageBlockBinding` is the one non-draw verb that syncs Espryt's render state and textures.
- `FillPoints.def:114-294` `MGP_FILL_FIELD_LIST`: the may-read table. `kDraw` owns 47 rows (`:116-162`), `kDispatch` 15 (`:165-182`), `kClear` 18 (`:184-201`), `kBlitOrCopy` 22 (`:203-231`), `kTextureOp` 10 (`:233-248`), `kReadback` 17 (`:250-269`), `kXfbSpan` 8 (`:271-278`), `kProgramOp` 11 (`:280-290`), `kQuery` 1 (`:294`).
- Generated masks: `MobileGL/MG_Pipe/generated/PipeFillPoints.inc:270-272` `struct MGPipeFieldMask { Uint64 Words[2]; }`, `:274-277` `MGPipeFieldMaskHas`, `:279-298` `kMGPipeClassFieldMask` with per-class counts in comments: **kDraw 54 (47 own + 7 sticky)**, kDispatch 22, kClear 25, kBlitOrCopy 29, kTextureOp 17, kReadback 24, kXfbSpan 15, kProgramOp 18, kQuery 8.
- `PipeFillPoints.inc:95` `static_assert(kMGPipeVerbCount == 69)`; `:183` `static_assert(kMGPipeVerbClassCount == 9)`; `:197` `kMGPipeVerbClass[]`.
- `FillPoints.def:24-27` states the discipline the tracker inherits: a `Fatal{UnmigratedPipeInput}` is fixed by adding a `(class, field)` row, **never** by marking a field sticky.

**83 `MGP_FILL` statements over these 69 verbs** (`FillPoints.def:21`, and `docs/Disaggregated/MEASUREMENTS.md:110`). Verified by grep: `MG_Impl/GLImpl/Drawing/GL_Drawing.cpp` 36, `Framebuffer/GL_Framebuffer.cpp` 11, `Query/GL_Query.cpp` 18, `Texture/GL_Texture.cpp` 8, `Sync/GL_Sync.cpp` 6, `Getter/GL_Getter.cpp` 3, `Program/GL_Program.cpp` 1 = 83.

### 1.6 `MobileGL/MG_Pipe/PipeMutation.h` and the three push-on-mutation hooks

- `PipeMutation.h:10-11` double include guard; `:29` gated on `MOBILEGL_PIPE_PUSH`; `:35` declares `MGPipeNoteFrontendMutation(MGPipeInputField)`; `:37-38` the `MGP_NOTE_MUTATION(Field)` macro; `:40` `((void)0)` in pull.
- Semantics (`PipeMutation.h:12-28`): refreshes **one** field's value in the pushed block when the field is in the current verb's mask, **value only, never the stamp**, so a withheld stamp (negative control B) stays withheld.
- Implementation `PipeFill.cpp:479-491`: no-op if no live context, if no verb has filled (`CurrentVerb() == kVerbCount`), if the field is sticky (no storage), or if the field is outside the class mask; otherwise `CopyField`.
- **The three hook sites** (the only `MGP_NOTE_MUTATION` spellings in the tree outside the header/comment):
  - `MobileGL/MG_State/GLState/TextureState/TextureState.h:85` — `MGP_NOTE_MUTATION(GetMaxTouchedTextureUnit)` in `NoteUnitTouched`'s high-water-mark branch.
  - `MobileGL/MG_State/GLState/TextureState/TextureState.h:98` — `MGP_NOTE_MUTATION(GetTextureBindGeneration)` in `NoteUnitTouched`'s `bindingChanged` branch.
  - `MobileGL/MG_State/GLState/TextureState/TextureState.h:111` — `MGP_NOTE_MUTATION(GetTextureBindGeneration)` in `BumpTextureBindGeneration()`.
  - `MobileGL/MG_State/GLState/TextureState/TextureState.h:133` — `MGP_NOTE_MUTATION(GetSamplingResolutionGeneration)` in `BumpSamplingResolutionGeneration()`.
  (Three *fields*, four *statements*. `docs/Disaggregated/MEASUREMENTS.md:121-127` tabulates the three fields and argues the hooks sit on the **counters**, not on the ~40 writer sites, because every backend-writes-frontend path funnels through them; it explicitly says `BufferObject`/`ProgramObject`/`VertexArrayObject` backend writes move no pushed field because those are handle-class read points.)
- Why it exists: `PipeFill.cpp:466-478` and `docs/Disaggregated/MEASUREMENTS.md:117` — Magma writes frontend objects **inside its own verb** (synthesising a fallback texture for an unbound sampler, materialising a queued clear, overriding a sampler filter), which moved values the boundary had already copied; 8 DirectVulkan cases + 2 traces aborted with `Fatal{PipeVerifyDiffer, "GetSamplingResolutionGeneration@Draw*", where=read}`. MEASUREMENTS.md:117 says push-on-mutation "正是 P2 tracker 需要的形状" — **the tracker must keep this refresh path, or absorb it into the dirty walk.**

### 1.7 `MobileGL/MG_Backend/MGPipe/PipeInputs.h` — the 63 fields and their storage

- `PipeInputs.h:73-129` `MGP_INPUT_STORAGE_LIST`: **56 (field, member) rows**. `PipeInputs.h:135` `kMGPipeForwardedFieldCount = 7`; `PipeInputs.h:240-241` asserts 56 + 7 == 63.
- `PipeInputs.h:136-137` asserts the forwarded set **is** the sticky set (an eighth sticky row without a forwarder is a compile error, not a test failure).
- `PipeInputs.h:144-153` documents the three storage classes V / O / F.
- `PipeInputs.h:155-157`: every non-forwarded accessor is `MGP_INPUT_CHECK` → `MGP_INPUT_VERIFY_READ` → the storage; both compile away in a plain push build, so the accessor is a plain load.
- Identity/liveness (not fields): `PipeInputs.h:189` `IsLive()` (forwarded, defined `PipeFill.cpp:514`), `:191` `ContextIdentity()`, `:193` `CurrentVerb()`, `:195` `FilledState()`.
- Storage members: V block `PipeInputs.h:621-665`, O block `PipeInputs.h:671-680`.
- `PipeInputs.h:236` `inline PipeInputs gPipeInputs{}` — the single global; `MobileGL/MG_Pipe/PipeInputsSwitch.h:19-21` maps `MGB_CTX` / `MGB_CTX_LIVE` / `MGB_CTX_IDENTITY` onto it under push and onto `pGLContext` under pull (`PipeInputsSwitch.h:24-26`). `PipeInputsSwitch.h:15-16`: the pull arm is the only place outside `MG_State`/`MG_Impl` that may spell `pGLContext`, and purity gate C greps for it.
- `PipeInputs.h:244` `static_assert(sizeof(PipeInputs) < 20 * 1024)`.

---

## 2. The frontend mutator surface the tracker must observe

### 2.1 `scripts/gen_pipe_dirty_surface.py` — what it enumerates and what it says today

Read in full (202 lines).

- Purpose (`gen_pipe_dirty_surface.py:11-19`): MGPipe replaces "the backend rediscovers what changed" with "the frontend says what changed", which only works if **every** frontend mutation a backend can observe bumps an aggregate generation; the failure mode is silent and one-directional (a forgotten bump renders stale, and no purity gate can see it), so the surface must be enumerated mechanically.
- Scope (`:34`): `MobileGL/MG_Impl/GLImpl` only, `.cpp` and `.h` (`:149`).
- What counts as a mutator (`:37-40`): `pGLContext->` followed by a method whose name starts with `Add|Set|Mark|Bump|Allocate|Truncate|Record|Notify|Begin|End`.
- What counts as reaching the backend (`:41`): `gBackendFunctionsTable.GL.<x>` or `pActiveBackendObject-><x>`.
- Function bodies are found textually by brace matching (`:42-43`, `:88-103`); comments and string literals are masked first (`:46-85`) — the same `MaskCommentsAndQuotedText` idiom the repo prefers.
- Classification (`:115-134`): a function containing **both** a mutator and a backend call is an **IMMEDIATE PUBLISH POINT**; everything else is **DEFERRED** — "published by the NEXT verb, and it is exactly those that need an aggregate generation rather than an inline push" (`:117-119`).
- Every mutator is printed `UNMAPPED` (`:178`, `:193-194`): "the aggregate-generation mapping file lands in P1, and this report is what it has to cover." The mapping file **does not exist in this tree** — grep for it finds nothing.
- Known limits it declares itself (`:195-197`): a mutator inside a lambda is attributed to the enclosing function, and a mutation published through a helper the entry point calls reads as deferred.

**Run in this tree** (`python scripts/gen_pipe_dirty_surface.py --summary`), output reproduced exactly:

```
dirty-surface: 41 files scanned under MG_Impl/GLImpl
dirty-surface: 926 mutator calls in total, 73 distinct mutators
dirty-surface: 92 of them sit in 36 IMMEDIATE PUBLISH POINTS - functions that also reach the backend - across 7 distinct mutators
dirty-surface: the remaining 834 are DEFERRED: nothing reaches the backend in the same function, so the next verb publishes them, and each one needs an aggregate generation
```

This matches `docs/Disaggregated/MEASUREMENTS.md:81` and `docs/Disaggregated/ARCHITECTURE.md:172` exactly — the numbers have not drifted.

The 7 immediate-publish mutators (marked `(immediate)` in the report): `RecordError` (836 of the 926 calls, i.e. **90% of the whole surface is one error-reporting call**), `SetTransformFeedbackPaused` (2), `BeginTransformFeedback` (1), `EndTransformFeedback` (1), `MarkTransformFeedbackObjectForDeletion` (1), `SetActiveTextureUnit` (1), `SetPatchVertices` (1).

The remaining 66 distinct mutators are all 1–5 calls each: the whole `Set*` render-state family (`SetBlendColor`, `SetBlendFunc`/`Indexed`, `SetBlendEquation`/`Indexed`, `SetColorMask`/`Indexed`, `SetDepthFunc`, `SetDepthMask`, `SetDepthRange`/`Indexed`, `SetStencilFunc`/`Mask`/`Op`, `SetScissorBox`/`Indexed`, `SetViewport`/`Indexed`, `SetCullFaceMode`, `SetFrontFaceMode`, `SetProvokingVertexMode`, `SetPolygonMode`, `SetPolygonOffset`/`Clamped`, `SetLineWidth`, `SetPointSize`, `SetPointFadeThresholdSize`, `SetPointSpriteCoordOrigin`, `SetLogicOp`, `SetClipControl`, `SetHint`, `SetSampleCoverage`, `SetSampleMaskValue`, `SetMinSampleShadingValue`, `SetPrimitiveRestartIndex`, `SetClearColor`/`Depth`/`Stencil`, `SetClampReadColor`, `SetCapability`, `SetCapabilityIndexed`, `SetPixelStoreParam` ×3, `SetPatchDefaultInnerLevel`, `SetPatchDefaultOuterLevel`, `SetCurrentVertexAttributeFloat` ×5 / `Int` / `Uint`, `SetNamedTransformFeedbackBinding` ×2, `BumpTextureBindGeneration` ×2), the nine `Mark*ForDeletion` calls, `BeginConditionalRender` / `EndConditionalRender`, and six `AddTransformFeedback*` accounting calls.

**Is it a gate?** No — informational today. `.github/workflows/test.yml:1528` runs `python3 scripts/gen_pipe_dirty_surface.py --summary` in the `pipe-gates` job, and `.github/workflows/test.yml:1524-1526` says verbatim: "Informational: the frontend mutation surface an MGPipe aggregate generation has to cover. It becomes a gate in P2, when the mapping file exists to diff against (ROADMAP.md:18 puts the first mapping round in P2, not P1)." `ARCHITECTURE.md:172` still says "P1 起成为门" — **the doc and the workflow disagree; the workflow is what runs.** Making it a gate is P2 work: `ROADMAP.md:18` lists "`gen_pipe_dirty_surface.py` 首轮映射成门".

### 2.2 Aggregate generations that already exist in the frontend

There is **no** `m_any*Generation` anywhere in the tree (grepped `m_any[A-Za-z]*Generation` across `MobileGL/` — zero hits). The five aggregates of `ARCHITECTURE.md:164-168` (`VertexArrayState::m_anyVaoAttributeGeneration`, `FramebufferState::m_anyAttachmentGeneration`, `TextureState::m_anyTextureContentGeneration`, `TextureState::m_anyTextureParamsGeneration`, `BufferState::m_anyBufferChangeGeneration`) are all **new**. What exists is:

**Context-level counters (all are pushed `PipeInputs` fields):**

| Counter | Getter | Bumped by | Read by (backend) |
|---|---|---|---|
| render-state version `m_version` (Uint16) | `RenderState::GetVersion()` `MG_State/GLState/RenderState/RenderState.h:20`, `.cpp:53-55`; exposed as `GLContext::GetRenderStateParametersVersion()` `Core.h:202`, `Core.cpp:786-787` | `RenderState::BumpVersions()` `RenderState.h:159-162` — called from ~25 setters in `RenderState.cpp` (:178, :214, :233, :244, :313 (macro), :346, :357, :452, :468, :512, :538, :564, :583, :599, :611, :622, :663, :680, :691, :783, :798, :812, :886, :897, :908). `RenderState.cpp:374` documents one deliberate NON-bump (clip-distance enable). | `DirectGLES.cpp:2028` (Espryt `SyncRenderState` early-out), `VulkanRenderer.cpp:5920` (Magma dynamic tail) |
| pipeline-state version `m_pipelineStateVersion` (Uint16) | `RenderState::GetPipelineStateVersion()` `RenderState.h:22`, `.cpp:57-59`; `GLContext::GetPipelineStateVersion()` `Core.h:204`, `Core.cpp:782-783` | same `BumpVersions()` `RenderState.h:160-161` — bumped **together** with `m_version`. `RenderState.h:165-171` explains the split: only pipeline-baked state, so a `glViewport` must not evict a cached pipeline. | `VulkanRenderer.cpp:4985` (`GetOrCreatePipeline`), `:6122`, `:6973` (fast-path snapshot) |
| texture bind generation `m_textureBindGeneration` (Uint64) | `TextureState::GetTextureBindGeneration()` `TextureState.h:102`; `GLContext::GetTextureBindGeneration()` `Core.h:130` | `TextureState::NoteUnitTouched`'s `bindingChanged` branch `TextureState.h:96-99`; `TextureState::BumpTextureBindGeneration()` `TextureState.h:109-112`; `TextureState.cpp:142` (delete-unbind); `TextureObject.cpp:96`; `TextureUnit.cpp:39` | `DirectGLES.cpp:1417`; `VulkanRenderer.cpp:6123`, `:6527`, `:6651`, `:6974` |
| sampling-resolution generation `m_samplingResolutionGeneration` (Uint64) | `TextureState::GetSamplingResolutionGeneration()` `TextureState.h:130`; `GLContext::…` `Core.h:136-138` | `TextureState::BumpSamplingResolutionGeneration()` `TextureState.h:131-134`, reached from `SamplerObject.cpp:34` (every `SamplerObject::BumpVersion`) and `TextureObject.cpp:37` | `DirectGLES.cpp:1520`, `Managers.cpp:3647`, `:3776`, `:3847`, `:4738`; `VulkanRenderer.cpp:6305`, `:6533`, `:6670`, `:7000` |
| texture context id `m_contextId` (Uint64) | `TextureState::GetContextId()` `TextureState.h:141`; `GLContext::GetTextureContextId()` `Core.h:143` | allocated once per context (`TextureState.h:144` `AllocateContextId()`); never bumped. Exists because both generations restart at 0 in a new context and a recreated context can reuse the heap address (`Core.h:140-142`, `TextureState.h:136-140`). | `DirectGLES.cpp:1416`, `:1517`; `Managers.cpp:3646`, `:3775`, `:3846`, `:4737` |
| transform-feedback generation `m_transformFeedbackGeneration` (Uint64) | `GLContext::GetTransformFeedbackGeneration()` `Core.h:334`; storage `Core.h:518` | `Core.h:312` (`= ++m_transformFeedbackNextGeneration` on every `BeginTransformFeedback`); swapped on bind at `Core.cpp:1301` / `:1324` | `VulkanRenderer.cpp:11348` |
| bound-XFB lifetime id | `GLContext::GetBoundTransformFeedbackLifetimeId()` `Core.h:441` | set on bind | pushed field (D21 counter-slot rekey) |
| max touched texture unit (high-water mark, not a generation) | `TextureState::GetMaxTouchedUnit()` `TextureState.h:101`; `GLContext::GetMaxTouchedTextureUnit()` `Core.h:126` | `NoteUnitTouched` `TextureState.h:78-86` | prefix bound for the unit walk in both backends |

**Per-object versions (NOT pushed fields — the backends reach them through the O-class pointers/SharedPtrs, see §3):**

| Version | Declared | Bumped | Backend read sites |
|---|---|---|---|
| `VertexArrayObject::GetConfigVersion()` (Uint32) | `VertexArrayState/VertexArrayObject.h:101` | `VertexArrayObject.cpp:299`, `:305`, `:311` | 7 |
| `FramebufferObject::GetObjectVersion()` (Uint16) | `FramebufferState/FramebufferObject.h:150` | `FramebufferObject.cpp:192`, `:203` (macro), `:215` | 10 |
| `BufferObject::GetChangeSerial()` (Uint64) | `BufferState/BufferObject.h:218`, storage `:264` | `BufferObject.cpp:45, 52, 61, 75, 82, 304, 368` | 15 |
| `ITextureObject::GetContentVersion()` (Uint64) | `TextureState/TextureObject.h:65` | `TextureObject.cpp:285, 354, 365`; `TextureObject2DCube.cpp:51, 62` | 10 |
| `ITextureObject::GetShapeVersion()` (Uint64) | `TextureObject.h:71` | `TextureObject.cpp:29` | 3 |
| `ITextureObject::GetTextureParamsVersion()` (Uint16) | `TextureObject.h:61` | `TextureObject.cpp:101,126,140,154,203,210,227,238,258,294,303`; `TextureObject.h:182` | 15 |
| `SamplerObject::GetVersion()` (Uint16) | `SamplerState/SamplerObject.h:52`, `.cpp:249` | `SamplerObject::BumpVersion()` `.cpp:27-35`, called from 14 setters (`.cpp:41,48,55,62,69,76,83,90,97,104,111,180,197,214`) | — (feeds the sampling-resolution generation) |
| `ProgramObject::GetLinkVersion()` | `ProgramState/ProgramObject.h:793` | `ProgramObject.cpp:328` | 4 |
| `ProgramObject::GetBackendStateVersion()` | `ProgramObject.h:790` | `ProgramObject.cpp:327`, `:493`; `ProgramObject.h:838`, `:1025` | 9 |
| `ProgramObject::GetImageUnitVersion()` | `ProgramObject.h:855` | `ProgramObject.h:848` | 2 |
| `ProgramObject::GetBlockBindingVersion()` | `ProgramObject.h:1013` | `ProgramObject.h:1026`, `:1048` | 1 |
| `ProgramObject::GetUBOContentVersion()` | `ProgramObject.h:740` | `ProgramObject.h:742` (wraps at `~0u`) | 2 |
| `ProgramObject::GetUniformWriteSetVersion()` | `ProgramObject.h:601` | `ProgramObject.h:625` | **0 backend readers** — only `ProgramPipelineObject.h:137-140` folds it into a signature. Worth flagging: `ARCHITECTURE.md:163` names "uniform-write-set 版本" as a `NEW_SHADER_BINDINGS` shutter source, so the tracker would be its first real consumer. |
| `GetLifetimeId()` on Buffer/Framebuffer/Program/Renderbuffer/Sampler/Texture/VAO | `BufferObject.h:215`, `FramebufferObject.h:159`, `ProgramObject.h:1262`, `RenderbufferObject.h:52`, `SamplerObject.h:57`, `TextureObject.h:83`/`:161`, `VertexArrayObject.h:66` | on construction | 38 backend read sites |

Counts of readers were taken by grepping `MobileGL/MG_Backend/` and `MobileGL/MG_Impl/`; **`MG_Impl` reads none of these version getters** — every one is consumed by a backend memo. That is the tracker's opening: today the *reader* re-derives; P2 moves the derivation to the *writer* side.

`ARCHITECTURE.md:174` flags the wrap hazard: three of these are `Uint16` (`RenderState::m_version`, `m_pipelineStateVersion`, `FramebufferObject::m_objectVersion`, plus `SamplerObject`/`TextureObject` params) and must be **widened at the tracker boundary** in the tracker's own `m_lastPushed[]`, without touching `MG_State`; a wrap there is harmless (one extra re-push, never a missed push) and is swallowed by the set-hash suppressor.

---

## 3. Value-class vs object-class split of the 63 fields

The classification is in `MobileGL/MG_Pipe/Coverage.def` (sticky/F list) plus the accessor layout of `PipeInputs.h`. Counts: **49 V + 7 O + 7 F = 63**.

### 3.1 O-class — 7 fields, the ones that hand out live frontend memory

These are what a real disaggregated boundary has to handle-ify; today they leak the address space.

| Field | Accessor + line | Storage | What it returns **today** |
|---|---|---|---|
| `GetBoundVertexArray` | `PipeInputs.h:477-481` | `SharedPtr<VertexArrayObject> m_boundVertexArray` `:671` | a **SharedPtr copy** taken at fill time; the pointee is the live frontend VAO, so every read through it (`GetConfigVersion`, attribute arrays) is a live pull |
| `GetProgramForDraw` | `PipeInputs.h:540-543` | `SharedPtr<ProgramObject> m_programForDraw` `:677` | SharedPtr copy; live `ProgramObject` |
| `GetProgramForDispatch` | `PipeInputs.h:535-538` | `:676` | SharedPtr copy |
| `GetTransformFeedbackProgram` | `PipeInputs.h:545-548` | `:678` | SharedPtr copy (`const` accessor) |
| `GetBufferBindingSlot(BufferTarget)` | `PipeInputs.h:485-494` | `BindingSlot<BufferObject>* m_bufferBindingSlot[15]` `:672` | a **non-const reference into the live GLContext** (`return *m_bufferBindingSlot[index]`). A target the fill left null (anything outside `GlobalBufferTargets`, incl. `Index`) is `MGPipeInputPoisonFatalForVerb` at `:490-492`. |
| `GetBufferBindingPoint(BufferTarget, Uint)` | `PipeInputs.h:495-508` | `BindingSlotRange1D<BufferObject>* m_bufferBindingPointBase[15]` `:673` | `base[index]` — a live reference; the base is the row start, rows are 84 wide (`PipeInputs.h:503-505`) |
| `GetFramebufferBindingSlot(FramebufferTarget)` | `PipeInputs.h:509-518` | `BindingSlot<FramebufferObject>* m_framebufferBindingSlot[]` `:674` | live reference; null → poison fatal `:513-516` |
| `GetImageTextureBinding(Int)` ×2 (mutable + const) | `PipeInputs.h:519-533` | `ImageTextureBinding* m_imageTextureBindingBase` `:675` | `base[unit]` into the live 192-entry array |
| `GetTextureUnitObject(Int)` | `PipeInputs.h:549-557` | `TextureUnit* m_textureUnitBase` `:679` | `base[unit]` into the live 192-entry array |

(That table lists 9 accessors over 7 distinct *field ids* — `GetImageTextureBinding` has two overloads, and `GetBufferBindingSlot`/`GetBufferBindingPoint` are separate ids. Precisely: the O-block in `PipeInputs.h:476-557` covers field ids `GetBoundVertexArray`, `GetBufferBindingSlot`, `GetBufferBindingPoint`, `GetFramebufferBindingSlot`, `GetImageTextureBinding`, `GetProgramForDispatch`, `GetProgramForDraw`, `GetTransformFeedbackProgram`, `GetTextureUnitObject` = **9 ids**; `PipeInputs.h:148-149` describes the class as "a SharedPtr copy, or a raw pointer to the live GLContext-owned slot/array … Identity is what phase C turns into a handle".)

**The load-bearing consequence for P2:** the verify comparator treats O fields by **identity** only (`PipeInputs.h:248-249`: "V by value … O by identity, F always equal"). So a texture whose content moved, a VAO whose config version bumped, or a binding slot whose bound object changed **is invisible to the P1 verify lane** as long as the pointer is the same. The tracker's dirty walk over object-class state has no oracle in the current harness — `ARCHITECTURE.md:502` calls this out ("dirty 位触发得太少" is the dangerous direction) and `:170` explains why polling per-object versions cannot substitute for the five new aggregates (there is no existing aggregate that answers "did any bound texture move", which is exactly why Magma resorts to a lossy `sampledContentSum`).

Backend read volume through the O accessors (grep `MGB_CTX-><name>(` under `MG_Backend/`): `GetBufferBindingSlot` 29, `GetFramebufferBindingSlot` 19, `GetTextureUnitObject` 19, `GetBoundVertexArray` 12, `GetBufferBindingPoint` 9, `GetProgramForDraw` 7, `GetImageTextureBinding` 7, `GetProgramForDispatch` 3, `GetTransformFeedbackProgram` 3.

### 3.2 F-class — 7 fields, forwarded and sticky

Declared `PipeInputs.h:558-572`, defined `PipeFill.cpp:517-553`. They carry **no** `MGP_INPUT_CHECK` and **no** `MGP_INPUT_VERIFY_READ` — the declared exception to "every accessor body" (`PipeInputs.h:565-570`), because a forward is a live call and `InvalidateCompileEnv` is reached from backend init before any verb has filled.

| Field | Signature | Definition | Null-context behaviour | Sticky rationale (`Coverage.def:115-122`) |
|---|---|---|---|---|
| `GetBufferBindingPointCount` | `SizeT(BufferTarget) const` `:566` | `PipeFill.cpp:517-520` | returns 0 | "keyed by target: a constexpr capacity table, not verb state" |
| `GetProgramObject` | `const SharedPtr<ProgramObject>&(Uint)` `:567` | `PipeFill.cpp:522-525` | returns a static null SharedPtr (`NullShared<T>` `PipeFill.cpp:281-285`) | "keyed by GL name: an object lookup" |
| `GetTextureObject` | `const SharedPtr<ITextureObject>&(Uint)` `:568` | `PipeFill.cpp:527-530` | static null SharedPtr | "keyed by GL name: an object lookup" |
| `HasOpenTransformFeedbackSpan` | `Bool(Uint64) const` `:569` | `PipeFill.cpp:532-535` | false | "keyed by lifetime id" |
| `InvalidateCompileEnv` | `void()` `:570` | `PipeFill.cpp:537-539` | no-op | "reverse channel: a write into the frontend" |
| `ValidateProgramName` | `Bool(Uint) const` `:571` | `PipeFill.cpp:541-544` | false | "keyed by GL name: a name-table lookup" |
| `RecordError` | `void(ErrorCode, UniquePtr<ErrorInfo>)` `:574` | `PipeFill.cpp:546-553` | `MGLOG_E_ONCE` and drop | "reverse channel: a write into the frontend" |

`Coverage.def:97-99` maps the pseudo-calls: `InvalidateCompileEnv` and `ValidateProgramName` → `kClientResolved` (the frontend answers itself), `RecordError` → `kReverseChannel` (one of the ten `MGPipeCallbacks`).

### 3.3 V-class — the remaining 47 field ids

Everything in `PipeInputs.h:198-474`. Storage `PipeInputs.h:621-665`. Grouped by shape, because the tracker's dirty granularity has to line up with these:

- **Whole-struct**: `GetRenderStateParameters` → `RenderStateParameters m_renderState` (`:645`, 1168 B).
- **Per-index arrays (kMGMaxDrawBuffers = 8, `MGPipeValueTypes.h:30`)**: `m_blendEquation[8][2]` `:624`, `m_blendFunc[8][4]` `:625`, `m_colorMask[8]` `:632`, `m_capabilityIndexed.Blend[8]` (`:181`).
- **Per-viewport arrays (MAX_VIEWPORTS = 16, `MGPipeValueTypes.h:244`)**: `m_depthRange[16]` `:637`, `m_viewportIndexed[16]` `:661`, `m_capabilityIndexed.ScissorTest[16]` (`:182`).
- **Per-attribute (MAX_VERTEX_ATTRIBS = 32, `VertexArrayObject.h:24`)**: `m_currentVertexAttribute[32]` `:634`.
- **Per-capability (CapabilityInputCount = 35, `MGPipeValueTypes.h:168-204`)**: `m_capability[35]` `:662`.
- **Per-buffer-target (BufferTargetCount = 15, `BufferObject.h:15-31`)**: `m_touchedBindingPointCount[15]` `:627`.
- **Pack/unpack pair**: `m_pixelStore[2]` `:641`. **Stencil pair**: `m_stencil[2]` `:655`.
- **Scalars/vectors** (the rest): active texture unit, blend colour, XFB name, clamp-read colour, clear colour/depth/stencil, cull face mode, depth func/mask, line width, logic op, max touched unit, min sample shading, patch inner/outer/vertices, pipeline & render-state versions, polygon mode/offset factor/units, primitive restart index, provoking vertex, sampling-resolution / texture-bind / texture-context / XFB generations & counters, scissor box, viewport, XFB active/paused flags, bound-XFB lifetime id.

`GetBoundTransformFeedbackName` (`PipeInputs.h:234-238`, storage `:626`) is a **dead field**: `Coverage.def:34-35` marks it "dead: no backend reads it since D21; kept for inventory row 594", and `MEASUREMENTS.md:109` confirms 62 of 63 accessors are actually called. It is still copied on every `kXfbSpan` fill.

---

## 4. Where a per-draw dirty walk sits relative to the existing validate paths

### 4.1 The shape of a draw today, `MobileGL/MG_Impl/GLImpl/Drawing/GL_Drawing.cpp`

A GL draw entry point is thin and calls a `*_Backend` wrapper. `glDrawElements` is `GL_Drawing.cpp:1219-1225`:

```
1219  void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
1220      if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
1221      if (!PrepareCurrentProgramForDraw(__func__)) return;
1222      if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
1223      AccountTransformFeedbackPrimitives(mode, count);
1224      DrawElements_Backend(mode, count, type, indices);
1225  }
```

`glDrawArrays` is the same minus the index-type check (`GL_Drawing.cpp:1165-1171`); `glDrawElementsBaseVertex` adds a count check (`:1155-1163`); `MultiDrawElementsBaseVertex` additionally walks the whole `count[]` array before issuing anything (`:1194-1213`, and `:1201-1206` explains why the whole call is rejected before any sub-draw). `glClear` is bare: `GL_Drawing.cpp:1215-1217`.

The `*_Backend` wrapper is where `MGP_FILL` sits, e.g. `GL_Drawing.cpp:535-542`:

```
535  void DrawElements_Backend(GLenum mode, GLsizei count, GLenum type, const void* indices) {
536  #ifdef TRACY_ENABLE
537      ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
538  #endif
539      if (ConditionalRenderDiscardsCommand()) return;
540      MGP_FILL(DrawElements);
541      MG_Backend::gBackendFunctionsTable.GL.DrawElements(mode, count, type, indices);
542  }
```

**So `MGP_FILL` is already exactly one statement before the table call, after the last early return** — `PipeFill.h:10-15` states this contract, and `GL_Drawing.cpp:514-524` explains why `ConditionalRenderDiscardsCommand()` gates the wrapper rather than the entry point (a discarded draw must still raise its argument errors). These 21 wrappers (`Clear_Backend` `:526`, then `:535`, `:544`, `:554`, `:565`, `:574`, `:583`, `:593`, `:603`, `:612`, `:623`, `:634`, `:645`, `:655`, `:667`, `:678`, `:689`, `:699`, `:707`, `:718`, `:727`) are **the natural home for `ValidateForDraw`/`ValidateForClear`**: one call site per verb class, already past every early return, already the boundary the pull/push switch is drawn at.

### 4.2 What the frontend already does per draw, before the backend

This is the work the tracker will sit beside, and it is not free:

- `PrepareCurrentProgramForDraw` `GL_Drawing.cpp:108-113`. `:104-107` calls it "the one funnel every drawing command passes through" and says the order is load-bearing: validate first, then publish the sample count, which reads the DRAW framebuffer binding. It calls `GetProgramForDraw()` (`:109`) and `PublishDrawFramebufferSampleCount` (`:111` → `:101` `program->WriteReservedNumSamples(...)` after `ResolveDrawFramebufferSampleCount()`). **This is already a per-draw frontend write into a frontend object.**
- `ValidatePrimitiveModeForBackend` `GL_Drawing.cpp:278-…`: checks `pActiveBackendObject` (`:283-289`), reads `GetBoundVertexArray()` and rejects the default VAO in core profile (`:291-298`), reads `GetProgramForDraw()` again (`:300`).
- `AccountTransformFeedbackPrimitives` `GL_Drawing.cpp:173-239`: reads `IsTransformFeedbackActive` (`:174`), `IsTransformFeedbackPaused` (`:177`), `GetTransformFeedbackProgram` (`:207`), walks `GetTransformFeedbackBufferCount()` binding points via `GetBufferBindingPoint(BufferTarget::TransformFeedback, i)` (`:220-221`), and issues up to six `Add*` mutations (`:178`, `:183`, `:213`, `:232`, `:233`, `:238`). `:234-238` notes the instanced and indirect entry points never call it. **Every one of these `Add*` calls shows up in the dirty-surface report as DEFERRED.**
- `ConditionalRenderDiscardsCommand` `GL_Drawing.cpp:522-524`.

So a draw already touches `GetProgramForDraw` twice, `GetBoundVertexArray` once, the XFB state four ways, and does a framebuffer sample-count resolve — **before** `MGP_FILL` copies 47 fields and **before** the backend re-reads all of it through `MGB_CTX`. The tracker's opportunity is to fold the fill, this prologue and the backends' memo probes into one walk; its risk is becoming a fourth pass.

### 4.3 The other seven classes' fill points

For completeness (each is a candidate `ValidateFor*` site): `Framebuffer/GL_Framebuffer.cpp:620, 634` (blit), `:646, 657, 668, 679` (`ClearNamedFramebuffer*`), `:2739, 2746, 2753, 2760` (`ClearBuffer*`), `:3008` (`ReadPixels`); `Texture/GL_Texture.cpp:1080` (readback), `:1624` (`GenerateMipmap`), `:4030, 4047, 4469` (copies), `:5080, 6463` (`GetTexImage`/`GetTextureImage`), `:6668` (`BindImageTexture`); `Program/GL_Program.cpp:3402` (`ShaderStorageBlockBinding`, the sole `kProgramOp`); `Query/GL_Query.cpp` 18 sites; `Sync/GL_Sync.cpp` 6 sites; `Getter/GL_Getter.cpp:1177, 1358, 2269`. `GL_Drawing.cpp:764, 817` are the two dispatches, `:839` `PatchParameteri`, `:910, 932, 946` the barriers, `:1269, 1352, 1385, 1400, 1606, 1638` the XFB span verbs.

---

## 5. The measured per-draw cost baseline P2 must beat

### 5.1 What the fill costs today (derived from the code, not measured)

A `kDraw` fill (`PipeFill.cpp:591-609`) iterates 63 field ids, tests a 2-word bitmask each (`PipeFillPoints.inc:274-277`), and executes **47 `CopyField` switch arms** (`PipeFillPoints.inc:280-281`: "kDraw: 54 fields (47 own + 7 sticky)"). Inside those arms, per draw:

- one `RenderStateParameters` struct copy — **1168 B** (`MEASUREMENTS.md:86`), `PipeFill.cpp:184`;
- 8×2 blend equations + 8×4 blend factors + 8 colour masks (`PipeFill.cpp:47-57`, `:99-103`);
- 32 `CurrentVertexAttributeValue` copies (`PipeFill.cpp:107-111`);
- 16 depth ranges + 16 indexed viewports (`:118-122`, `:224-228`);
- 35 capability bools + 8 + 16 indexed capability bools (`:229-245`);
- 14 buffer-binding-slot pointers + 4 binding-point base pointers + 4 touched counts (`:64-86`);
- 4 SharedPtr copies with atomic refcount traffic: `m_boundVertexArray`, `m_programForDraw`, plus `m_transformFeedbackProgram` on XFB classes (`PipeFill.cpp:62`, `:178`, `:219`).

Plus, in a poison build, 47 `Uint64` stamp stores into `FilledGen[]` (`PipeFill.cpp:607`) — but note `MOBILEGL_PIPE_POISON` is **off** in a plain push Release/INFO build without `MOBILEGL_BUILD_DISAGGREGATED` (`PipeInputs.h:21-26`), so the shipping push build pays the copy and not the stamps.

Order-of-magnitude: **~1.5–2 KB memcpy-equivalent plus ~47 switch dispatches and 2–4 atomic refcount pairs per draw**, unconditionally, for a verb that in steady state reads ~8 fields. That gap is the P2 thesis.

### 5.2 `docs/Disaggregated/MEASUREMENTS.md` §3 — the device baseline (`:46-76`)

Method (`:48`): `MOBILEGL_PIPE_STATS=1` through the retrace channel's `--env`, trace APK built from `7ef7c7e5`, spike OFF, **last complete 120-frame window**. acc/draw and gate numbers are software-deterministic (identical on both devices; they count code paths, not hardware) — only wall-clock varies.

| trace (frames) | backend | draws/f | **acc/draw** | buf B/f | tex B/f (box/rect) | ubo-global B/f | ubo-named B/f | gates hit/miss |
|---|---|---|---|---|---|---|---|---|
| `minecraft-1.21.4-in-world` (360) | Espryt | 91.6 | **9.28** | 13.5 K | 635 K (185 box / 0 rect) | 16.7 K | 0 | ers 9257/2577, etl 10538/1296, eub 10720/1114 |
| `minecraft-1.21.4-in-world` (360) | Magma | 91.6 | **8.56** | 13.5 K | 39.9 K (97 box / 89 rect) | 16.7 K | 0 | mfp 0/10994, mpm 9240/1754, mdt 9120/1874 |
| `…fabric-iris-bsl-in-world` (120, memo cold) | Espryt | 23.2 | 21.04 | 32.6 K | 8.8 K | 1.8 K | 0 | ers 1958/1843, etl 722/3079 |
| `…fabric-iris-bsl-in-world` (120, memo cold) | Magma | 23.2 | 11.26 | 313 K | 256 K | 1.8 K | 0 (vtxc 1.7 K) | mpm 1890/895, mdt 1573/1212 |
| `improved-transparency-minecraft-26.3` (1200) | Espryt | 1320 | **8.44** | 333 K | 0 | 0 | 0 | ers 156925/2791, etl 148606/11110, eub 148246/11470 |
| `improved-transparency-minecraft-26.3` (1200) | Magma | 1320 | **6.53** | 173 K | 0 | 0 | **331 K** | mfp 21360/137036, mpm 134421/2615, mdt 156611/1785 |
| `…neoforge-create-indirect-in-world` | both | — | — | — | — | — | — | fails on both devices (§5), <120 frames |

`MEASUREMENTS.md:64` is the sentence P2 is judged against: **"真机稳态动态 accessor 成本是每 draw 6.5–9.3 次"** (the low end of the 10–25 prediction; llvmpipe's 15.5/20.7 are memo-cold). "推送要打败的是 ~8 次 accessor + memo 探测，不是 124/169 的静态调用点数。GO/NO-GO 的 tracker 绝对 ns 上限从这里定。"

Other §3 readings: `:65` Magma repacks **331 KB** of named UBO bytes per frame in the 26.3 world while Espryt binds directly (0) — the first real host-payload number; `:66` union-box vs rect-list is a **16×** texel-byte difference (635 K vs 40 K) on the same 185 emissions.

Repro command, `MEASUREMENTS.md:70-76`:
```sh
ANDROID_SERIAL=<serial> MSYS_NO_PATHCONV=1 \
python3 tools/trace_replay/run_android_retrace_local.py \
  --case minecraft-1.21.4-in-world --backend DirectGLES \
  --env MOBILEGL_PIPE_STATS=1 --env MOBILEGL_PIPE_STATS_PERIOD=120
# grep 'MGPipe stats:' in the result dir's mobilegl.log, take the last complete window
```
Harness traps that bite a P2 measurement run: `MEASUREMENTS.md:92` (the trace app never reaches `DestroyImpl`, so `MOBILEGL_PIPE_STATS_FILE` JSON is **never written on device** — only the periodic log line; a run shorter than one period produces nothing), `:93` (both devices share one `.trace-work/android-retrace-result` root which is `rmtree`d per invocation — **must run serially from one tree**), `:94` (`MSYS_NO_PATHCONV=1`), `:96` (`create-indirect` is broken on `dev@81b17c0b` too, not this branch).

### 5.3 `docs/Disaggregated/MEASUREMENTS.md` §6 — scale (`:102-111`)

| quantity | value | source |
|---|---|---|
| backend `pGLContext->` arrow sites | 277 (Espryt 113, Magma 164) | plan said 293, from a stale vendored inventory |
| non-arrow lines | 58 (Espryt 9, Magma 49; 43 of Magma's are per-verb `MOBILEGL_ASSERT`) | as planned |
| `PipeInputs` fields | **63** | plan said 61; `GetBoundTransformFeedbackLifetimeId` and `HasOpenTransformFeedbackSpan` are post-D21 read points |
| distinct accessors the backends call | **62** (Espryt 32, Magma 56) | `GetBoundTransformFeedbackName` has no reader, kept as an annotated dead row |
| fill points | **83 `MGP_FILL`, 69 verbs, 9 classes** | `MG_Pipe/FillPoints.def` |
| `SyncPersistentMappedRange` / `SyncGpuWrites` | 20 / 6 | as planned |

(The task brief's "63 fields, 61" phrasing: 63 is current, 61 was the plan's stale figure — `MEASUREMENTS.md:108`.)

Related §7 findings the tracker must not re-open (`MEASUREMENTS.md:113-127`): the verify lane found **9 missing fill rows** across 79 retraces and 818 integration-verify cases, and says **8 of them are static over-approximation** (the code path is reachable but the lane never got there) — "**P2 收紧填充表时先复查这 8 行**" (`:115`). An over-approximated `(class, field)` pair permanently disables that pair's poison, so tightening the table is P2's job and these 8 rows are the first place to look.

### 5.4 `MobileGL/MG_Util/Metrics/PipeStats.{h,cpp}` — the counters P2 must extend

Header purpose, `PipeStats.h:15-23`: sizes two things the tree cannot answer by reading — bytes per frame across the boundary, and **accessor calls + memo-gate probes per draw**, "the load-bearing number: … the real steady state is believed to be 10-25 accessor calls per backend per draw. Without a dynamic counter the P2 verdict stays a guess."

Cost when off, `PipeStats.h:25-36`: `g_pipeStatsEnabled` is a plain global (`PipeStats.h:139`, defined `PipeStats.cpp:104`) latched once at `Init()` from `MG_Config::Features.PipeStats` (`PipeStats.cpp:257`); every site is written `if (Enabled()) Add...(...)` (`PipeStats.h:29`), so off costs one hot-global load and a predicted branch. Counters are relaxed atomics (`PipeStats.cpp:111-120`) because texture/buffer staging is multi-threaded.

**`ByteClass`** (`PipeStats.h:46-77`): `StageBuffer`, `StageTexture`, `StageUboGlobal`, `StageUboNamed`, `StageVertexClient`, `StageIndexClient`, `StageIndirectCmd`, `PersistentMapPush`, `ResidualValueBlock`. `PipeStats.h:73-75`: `ResidualValueBlock` is a **placeholder that stays 0 until P2** — P2 is the phase that must make it non-zero.

**`CallClass`** (`PipeStats.h:81-102`): `Draws` (the denominator; `PipeStats.cpp:74-75` — "A dispatch is not a draw and is not counted"), `AccessorCalls`, `TextureUploadEmissions`, `TextureUploadBoxEmissions`, `TextureUploadRectEmissions`, `TextureUploadJobs`.

**How `acc/draw` is actually counted** — critical for writing an honest P2 target. `PipeStats.h:85-88` and the site inventory `PipeStats.cpp:76-88`: `AccessorCalls` is a set of **static tallies at ~10 hot entry points**, not a wrapper around all call sites. Each instrumented function adds the number of GLContext accessor calls **its own body** executed on the path taken, and each tally sits **after** the last early return that would skip those reads. Covered: `PrepareForDraw`'s own reads, `SyncRenderState`, `CaptureDrawTextureSyncKeys`/`CurrentUnitBindingsEpoch`, `SyncNeccessaryTextures`' walk, `TrySetupDrawFastPath`, `GetOrCreatePipeline`, `ApplyDynamicDrawStateTail`. **Not covered**: reads inside the callees those functions invoke (buffer/VAO/FBO/program sync, and the pipeline payload builder's **~40 reads on a memo miss**), and every non-draw entry point. So the number is a **LOWER BOUND**.

The actual tally sites (constants are the per-path read counts):
- Espryt: `DirectGLES.cpp:1421` (+2), `:1524` (+3), `:1575-1576` (variable, the unit walk), `:2043` (+1, render-state gate hit), `:2053` (+3, gate miss), `:2977` (+2, in `PrepareForDraw`, alongside `:2976` `Draws +1`); `Managers.cpp:4461-4467` (texture emissions/jobs).
- Magma: `VulkanRenderer.cpp:5009` (+1 pipeline memo hit), `:5016` (+1 miss), `:5274` (**+15**), `:5927` (+1 dynamic-tail hit), `:5934` (+2 miss), `:6416` (+6), `:6449` (+1, alongside `:6448` `Draws +1`), `:6462` (+3); `VkTextureManager.cpp:3174-3178`; `UniformManager.cpp:2088` (`StageUboNamed` bytes).

**`Gate`** (`PipeStats.h:107-122`) — the six memo gates, each counted exactly once per probe (`PipeStats.cpp:91`), hit == short-circuited, miss == did the work:
- `EsprytRenderState` — `DirectGLES.cpp SyncRenderState` render-state-version early-out (probe at `DirectGLES.cpp:2042`/`:2048`)
- `EsprytTextureSyncList` — `SyncNeccessaryTextures` six-value key compare (`DirectGLES.cpp:1558`/`:1573`)
- `EsprytUnitBindingsEpoch` — the `(context, max unit, bind generation)` shutter over the unit walk (`DirectGLES.cpp:1426`/`:1431`)
- `MagmaDrawFastPath` — `TrySetupDrawFastPath` whole-snapshot fast path (`VulkanRenderer.cpp:6453`/`:6458`)
- `MagmaPipelineMemo` — `GetOrCreatePipeline` (`VulkanRenderer.cpp:5008`/`:5015`)
- `MagmaDynamicTail` — `ApplyDynamicDrawStateTail` version+extent tail gate (`VulkanRenderer.cpp:5926`/`:5933`)

**The reading caveat P2 must respect**, `PipeStats.cpp:93-99`: three of the instrumented functions (`SyncRenderState`, the texture-key capture, `SyncNeccessaryTextures`) are also reached from **non-draw** call sites — Clear, readbacks, the DSA by-name entries — which the `Draws` counter deliberately does not count. So `acc/draw` is the per-draw steady-state number only in a **draw-dominated window**; in a clear/readback-dominated window it is inflated by exactly those probes, and "the gate hit/miss pairs are the honest reading there."

Other API: `PipeStats.h:129` `kPayloadHistogramBuckets = 24` and `RecordDrawPayloadBytes` (`:154`) are implemented and unit-tested but **called by nothing** (`PipeStats.h:124-128`: MGPipe emits no records yet — "the first generator to emit records only has to add the one call"). `PipeStats.h:132` `kDefaultSummaryFramePeriod = 120`, overridable via `MOBILEGL_PIPE_STATS_PERIOD` (`PipeStats.cpp:258-260`). `PipeStats.h:159` `OnPresent()` folds the frame into run totals and every period emits the summary line. `PipeStats.h:179` `FormatWindowLine()` is documented **PURE** (calling it twice returns the same text and steals nothing), with `AdvanceSummaryWindow()` (`:183`) as the separate window-closing call — `PipeStats.cpp:366-393` computes window deltas against `g_windowBase*`. `PipeStats.cpp:224-231` emits it via `MGLOG_I` with an explicit justification for breaking the MGLOG_D rule.

### 5.5 What P2 therefore has to measure

Concretely, the brief should ask for:

1. **`acc/draw` per backend, before vs after, on the same three traces** in §5.2 — same last-complete-120-frame-window protocol, same devices, serial runs. The target is: strictly below Espryt 9.28 / Magma 8.56 (vanilla in-world) and 8.44 / 6.53 (26.3), on a like-for-like tally. **Caveat to state up front:** the tracker will *move* reads out of the instrumented functions, so the existing static tallies will under-report the new world unless P2 re-audits the tally constants (`DirectGLES.cpp:1421, 1524, 2043, 2053, 2977`; `VulkanRenderer.cpp:5009, 5016, 5274, 5927, 5934, 6416, 6449, 6462`). A tracker that scores 0 acc/draw by relocating the reads has proved nothing.
2. **The six gate hit/miss pairs** — the honest reading per `PipeStats.cpp:93-99`, and the one that survives the relocation problem. `mfp 0/10994` (Magma fast path never hits in the vanilla world) and `mfp 21360/137036` (13% in 26.3) are the two most interesting existing numbers.
3. **A new counter class for the tracker itself** — absolute ns per validate, per class. `ROADMAP.md:18`'s acceptance is "两台设备配对逐线程 CPU p50/p99 不差且 tracker 绝对 ns 在上限内", and `MEASUREMENTS.md:64` says the ns ceiling is derived from the ~8 acc/draw figure. `PipeStats` has no timing counter today; `RecordDrawPayloadBytes` + the 24-bucket histogram (`PipeStats.h:129`) is the nearest existing machinery and is currently dead.
4. **`ResidualValueBlock` bytes/frame** — the placeholder at `PipeStats.h:73-75` that P2 is supposed to make real, sized against `ResidualValueBlock` = 1248 B of which `RenderStateParameters` is 1168 (`MEASUREMENTS.md:86`).
5. **The fill cost it replaces** — there is no counter for it today. Adding a `ByteClass` or `CallClass` for "fields copied at the verb boundary" would make the 47-copies-for-8-reads gap (§5.1) a measured number rather than an argument.

---

## 6. Open questions the implementers will hit

1. **Eight validate entries or nine?** `ARCHITECTURE.md:153` says eight; the landed `FillPoints.def:108-109` has nine classes because `kProgramOp` was split out for `ShaderStorageBlockBinding`. Pick one and update the other.
2. **The dirty-surface mapping file has no format yet.** The script prints `UNMAPPED` for all 73 mutators (`gen_pipe_dirty_surface.py:193-194`) and nothing in the tree defines what it maps *to*. It also has three self-declared blind spots (`:195-197`): lambdas, helper-published mutations, and — worth adding — it only scans `MG_Impl/GLImpl`, so the four `MGP_NOTE_MUTATION` sites in `MG_State/GLState/TextureState/TextureState.h` are outside its scan root entirely.
3. **`RecordError` is 90% of the surface and is a reverse channel, not state** (`Coverage.def:99`, `PipeInputs.h:574`). The mapping file should exempt it explicitly rather than let it dominate the count; that leaves 72 real mutators / 90 real calls.
4. **The 8 over-approximated fill rows** (`MEASUREMENTS.md:115`) must be re-checked before the fill table is tightened, or the poison stays permanently disabled on those pairs.
5. **Object-class dirty has no oracle.** The verify comparator compares O fields by identity only (`PipeInputs.h:248-249`), so under-triggering a `NEW_SAMPLER_VIEWS`/`NEW_FRAMEBUFFER` dirty bit is invisible to today's harness. `ARCHITECTURE.md:502` names this as the dangerous direction and describes the retention mode (`保留模式`) verify needs for consume-and-clear groups. That extension is P2 work, not something the P1 lane already provides.
6. **Push-on-mutation vs the dirty walk.** The four `MGP_NOTE_MUTATION` statements exist because backends write frontend state *inside* their own verb (`PipeFill.cpp:466-478`). A tracker that computes dirty state once at the validate point has the same exposure; `MEASUREMENTS.md:117` says push-on-mutation is "正是 P2 tracker 需要的形状", so keep it rather than replace it.
