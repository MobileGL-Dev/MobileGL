# P5 scout — the unmigrated-input census

Tree: `/home/swung/w7/pipe` @ `a29807cc` (feat/disaggregated, P4a landed). READ-ONLY survey; nothing
was built, compiled or run. Every claim below is `file:line` at that commit. Where I am inferring
rather than reading, the sentence says **guess**.

---

## 0. The one-paragraph answer

`PipeInputs` is a 63-field block the *client* fills at every verb boundary and the *backends* read
through `MGB_CTX->`. The poison that backs the P5 gate is real, loud and already wired
(`MobileGL/MG_Pipe/generated/PipeFilled.inc:407-416`), but **only the client stamps it**
(`MobileGL/MG_Impl/Pipe/PipeFill.cpp:306`, `MGPipeFillAccess`), and the applier **deliberately does
not** (`MobileGL/MG_Backend/MGPipe/PipeInputs.h:613-618`). So on a split server — which does not link
`MG_Impl` — `CurrentVerbSerial` stays 0, every `FilledGen[]` stays 0, `MGPipeInputFieldIsFresh`
returns false for everything, and **the first accessor read of any field is
`Fatal{UnmigratedPipeInput, "<Field>@<none>"}`**. Making that gate mean something (fire on genuinely
unmigrated fields, not on all 63) is the single largest hidden item in P5. 21 of the 63 fields are
genuinely unserviceable by any pushed record on the reduced path; §3 is that list.

---

## 1. What `PipeInputs.h` is, how big, how filled, and the poison mechanism

### 1.1 The file

`MobileGL/MG_Backend/MGPipe/PipeInputs.h` (736 lines) + `PipeInputs.cpp` (179 lines).
`PipeInputs.cpp` is compiled **only under `MOBILEGL_PIPE_PUSH`** (`PipeInputs.cpp:11-13`).
The struct is `struct PipeInputs` (`:158`), the single global is
`inline PipeInputs& gPipeInputs = *new PipeInputs();` (`:706`, and see §5 for why it is a `new`).

The switch that routes backend reads is `MobileGL/MG_Pipe/PipeInputsSwitch.h`:

```
#if MOBILEGL_PIPE_PUSH
#define MGB_CTX (&::MobileGL::MG_Pipe::gPipeInputs)
#define MGB_CTX_LIVE (::MobileGL::MG_Pipe::gPipeInputs.IsLive())
#define MGB_CTX_IDENTITY (::MobileGL::MG_Pipe::gPipeInputs.ContextIdentity())
#else
#define MGB_CTX (::MobileGL::MG_State::pGLContext)          // pull arm
...
```

Purity gate C (`grep -c pGLContext MG_Backend/` == 0) is **already effectively green**: the only hit
in `MG_Backend/` at `a29807cc` is a comment, `PipeInputs.h:693`. One hit, zero code.

### 1.2 Field count and classes

`kMGPipeInputFieldCount == 63` (`MobileGL/MG_Pipe/generated/PipeFilled.inc:96`, `static_assert`).
Note: `ARCHITECTURE.md:83` still says "61 个" for G5 — **that number is stale**; the generated
assertion says 63 and `ROADMAP.md`'s P1 row says 63.

Three storage classes, documented at `PipeInputs.h:139-157`:

| class | count | storage | notes |
|---|---|---|---|
| **V** value | 47 | copied out of `GLContext` by calling the *same* accessor at fill time | plain POD mirrors, `PipeInputs.h:628-675` |
| **O** object reference | 9 | `SharedPtr<>` or a raw pointer **into the live GLContext** | `PipeInputs.h:677-686`; this is what a payload can never carry |
| **F** forwarded | 7 | none — defined out-of-line in `MG_Impl/Pipe/PipeFill.cpp` (client side) | `PipeInputs.h:558-577`; sticky |

`MGP_INPUT_STORAGE_LIST` (`PipeInputs.h:73-129`) is 56 rows = V+O; +7 F = 63, asserted at
`PipeInputs.h:710-711`. `sizeof(PipeInputs) < 20 KiB` asserted at `:714`.

The **9 O-class fields** (`PipeInputs.h:477-556` accessors, `:678-686` storage) are the ones P5 has to
care about first:

```
GetBoundVertexArray        SharedPtr<VertexArrayObject>
GetBufferBindingSlot       BindingSlot<BufferObject>*      [kBufferTargetCount]
GetBufferBindingPoint      BindingSlotRange1D<BufferObject>* [kBufferTargetCount]
GetFramebufferBindingSlot  BindingSlot<FramebufferObject>* [kFramebufferTargetCount]
GetImageTextureBinding     ImageTextureBinding*            (base pointer)
GetProgramForDispatch      SharedPtr<ProgramObject>
GetProgramForDraw          SharedPtr<ProgramObject>
GetTransformFeedbackProgram SharedPtr<ProgramObject>
GetTextureUnitObject       TextureUnit*                    (base pointer)
```

The **7 F-class = the 7 sticky fields** (`PipeInputs.h:569-577`; the identity is asserted, not tested,
at `PipeInputs.h:135-137`; argued in `Coverage.def:138-150`):

```
GetBufferBindingPointCount(BufferTarget)      HasOpenTransformFeedbackSpan(Uint64)
GetProgramObject(Uint)                        InvalidateCompileEnv()            [reverse channel]
GetTextureObject(Uint)                        ValidateProgramName(Uint)
RecordError(ErrorCode, UniquePtr<ErrorInfo>)  [reverse channel]
```

F-class accessors carry **no** `MGP_INPUT_CHECK` and **no** verify read-hook — the declared exception,
argued at `PipeInputs.h:563-568` (`InvalidateCompileEnv` is reached from backend init before any verb
has filled, where a check would fatal on every start). So the poison cannot protect these seven; in
split they must become handle tables / callbacks, and a wrong answer is silent.

### 1.3 The per-verb fill points

The single fill/validate entry is `MGPipeValidateForVerb(MGPipeVerb)`
(`MobileGL/MG_Impl/Pipe/PipeFill.h:35`), spelled at 83 statements over 69 verbs via
`#define MGP_FILL(Verb)` (`PipeFill.h:141`). Its five ordered steps are documented at
`PipeFill.h:21-29`: bump serial → tracker dirty walk → emit the P2/P3a/P4a calls for set bits →
**P1 residual fill for every field an emitted call did NOT supply, stamping each with the new
serial** → (verify build) entry compare.

The verb set *is* `MG_Backend::GLFunctionsTable`'s member list in declaration order, refused by
`gen_pipe.py` if it drifts (`FillPoints.def:16-22`). 69 verbs, 9 classes
(`FillPoints.def:152-154`, `generated/PipeFillPoints.inc:183` asserts 9). The may-read table is
`MGP_FILL_FIELD_LIST` (`FillPoints.def:157-339`), compiled to
`kMGPipeClassFieldMask[9]` (`generated/PipeFillPoints.inc:279-297`):

| class | fields (own + 7 sticky) |
|---|---|
| kDraw | 54 (47 + 7) |
| kDispatch | 22 (15 + 7) |
| kClear | 25 (18 + 7) |
| kBlitOrCopy | 29 (22 + 7) |
| kTextureOp | 17 (10 + 7) |
| kReadback | 24 (17 + 7) |
| kXfbSpan | 15 (8 + 7) |
| kProgramOp | 18 (11 + 7) |
| kQuery | 8 (1 + 7) |

Eight rows of that table are deliberate static over-approximations, all KEPT, each argued at
`FillPoints.def:28-56` (depth/stencil readback emulation pausing XFB; Magma's `VkClearManager`
reading `GL_FRAMEBUFFER_SRGB` from `GenerateMipmap`/`PrepareStorageImageTextures`; Magma's
`TryBlitToDefaultFramebufferWithShader` being a real draw). **P5 must not "tidy" these**: the rule
there is that removing a row on desktop-corpus evidence turns a rare path into a shipped-build Fatal.

### 1.4 The poison / generation mechanism (P1's per-verb generation poison)

```
MOBILEGL_PIPE_POISON  ==  MOBILEGL_PIPE_PUSH && ( LOG<=DEBUG || MOBILEGL_BUILD_DISAGGREGATED
                                                   || MOBILEGL_PIPE_VERIFY )
```
`PipeInputs.h:21-26`. **A disaggregated build arms the poison by construction** — that is the
mechanism behind the P5 exit gate, and it needs no new switch.

- State: `struct MGPipeFilledState { Uint64 CurrentVerbSerial; Uint64 FilledGen[63]; }`
  (`generated/PipeFilled.inc:404-406`), held as `PipeInputs::m_filled` (`PipeInputs.h:625`, only
  under `MOBILEGL_PIPE_POISON`).
- Freshness: `MGPipeInputFieldIsFresh` (`generated/PipeFilled.inc:418-426`) —
  `gen == 0` ⇒ **not fresh** (the "`@<none>`" case, argued in the comment at `:415-417`);
  otherwise fresh iff **sticky** or `gen == CurrentVerbSerial`. It is a *generation*, not a bitmap,
  precisely to catch "filled by the previous draw, read by the following `glTexSubImage`"
  (`generated/PipeFilled.inc:19-23`, `ARCHITECTURE.md:357`).
- Check: `MGP_INPUT_CHECK(Field)` (`PipeInputs.h:44-49`) on **every non-forwarded accessor**, ahead
  of `MGP_INPUT_VERIFY_READ` and the load.
- Five accessors additionally poison on a *structurally* impossible argument, with an `MGLOG_E`
  naming the argument first: `IsCapabilityEnabledIndexed` (`:461-463`), `GetBufferBindingSlot`
  (`:489-492`), `GetBufferBindingPoint` (`:499-503`), `GetFramebufferBindingSlot` (`:512-515`),
  `GetImageTextureBinding` (`:521-523`, `:529-531`), `GetTextureUnitObject` (`:552-554`).
- **Who stamps**: `MGPipeFillAccess`, i.e. `MG_Impl/Pipe/PipeFill.cpp` only
  (`PipeInputs.h:609-611` names it; `PipeFill.cpp:306` is `SetVerb`). Grep for writers of
  `m_currentVerb` / `m_filled` outside `MG_Test` finds nothing else.
- **Who does NOT stamp**: `MGPipeApplyAccess` (`MG_Pipe/PipeApply.cpp:176`), the applier's door into
  the same block. `PipeInputs.h:612-618`: *"It deliberately does NOT stamp the poison generations —
  a stamp says 'the filler published this for THIS verb', which is the walk's statement, not the
  applier's."* The applier writes `m_renderState`, `m_pixelStore[0]`, `m_capability`,
  `m_currentVertexAttribute`, the render-state versions and the patch fields
  (`PipeApply.cpp:177-184`, `:1338-1437`), and derives the rest of the render-state mirrors through
  `MGPipeDeriveRenderStateFields` (`PipeApply.h:1084`, `PipeApply.cpp:2157`).

Negative controls already exist: `MGPipeSetPoisonOmission(verb, field)` / `MOBILEGL_PIPE_POISON_OMIT`
(`PipeFill.h:103-109`, `Config.h:373`), pinned by
`MG_IntegrationTest/Scenarios/PoisonOmissionScenario.cpp:88` and
`MG_Test/Pipe/PipeInputsTest.cpp:93,292-296`.

---

## 2. THE CENSUS at `a29807cc`

### 2.1 Two censuses, and they answer different questions

**(a) `MGB_CTX->` read sites** — what the backends textually read. Recursive counts
(`MobileGL/MG_Backend/`, note `DirectVulkan/Renderer/` is a third level and a shallow glob misses it):

| | arrow reads | `MGB_CTX_LIVE` | `MGB_CTX_IDENTITY` |
|---|---|---|---|
| Espryt `DirectGLES/` | **117** | 8 | 1 |
| Magma `DirectVulkan/` | **165** | 49 | 0 |
| total | **282** | 57 | 1 |

per file: `DirectGLES.cpp` 98, `Managers.cpp` 23, `MultiDraw.cpp` 5, `Utils.cpp` 2,
`DirectVulkan.cpp` 47, `BackendObject_DirectVulkan.cpp` 4, `Renderer/UniformManager.cpp` 23,
`Renderer/VulkanRenderer.cpp` 130, `Renderer/VkRenderPassManager.cpp` 3,
`Renderer/VkTextureManager.cpp` 2, `Renderer/VkClearManager.cpp` 1.

Split by class:

| | O-class | F-class | V-class | total |
|---|---|---|---|---|
| Espryt | **60** | 7 | 50 | 117 |
| Magma | **52** | 12 | 101 | 165 |

**This count under-reports reachability.** Espryt funnels several fields through helpers, so one
`MGB_CTX->` site serves many call sites:
- `GetFramebufferBindingSlotChecked` (`DirectGLES/DirectGLES.cpp:173`) wraps
  `MGB_CTX->GetFramebufferBindingSlot` and has **8** call sites: `:2077 :2236 :2844 :2995 :3106
  :4175 :4357 :4447`.
- `CaptureDrawTextureSyncKeys` (`DirectGLES.cpp:2019`) reads `GetTextureContextId` `:2021`,
  `GetMaxTouchedTextureUnit` `:2034`, `GetSamplingResolutionGeneration` `:2035`, and
  `CurrentUnitBindingsEpoch` (`:1875`) reads `GetTextureContextId` `:1904` +
  `GetTextureBindGeneration` `:1905`.
The poison fires at the accessor either way, so the Fatal still names the right field — but a P5
brief that sizes work off "60 Espryt O-class sites" will undercount.

**(b) The field census** — what no pushed record can supply. This is the one that matters, because
the applier fills `PipeInputs` from records and everything else has to come from somewhere.

From `generated/PipeFilled.inc:336-402` (`kMGPipeFieldEmittedBy`, `kMGPipeEmittedFieldCount = 40`)
and `MG_Impl/Pipe/PipeFill.cpp:1924-1939` (`EmittedCallSuppliesTheWholeField`):

* **23 fields have NO emitter at all** (`kNone`): `GetActiveTextureUnit`,
  `GetBoundTransformFeedbackName`, `GetBufferBindingSlot`, `GetBufferBindingPoint`,
  `GetBufferBindingPointCount`\*, `GetTouchedBufferBindingPointCount`, `GetPixelStoreParameters`,
  `GetProgramObject`\*, `GetSamplingResolutionGeneration`, `GetTextureBindGeneration`,
  `GetTextureContextId`, `GetTextureObject`\*, `GetTransformFeedbackCapturedVertices`,
  `GetTransformFeedbackGeneration`, `GetTransformFeedbackPausedPrimitiveCounter`,
  `GetTransformFeedbackProgram`, `IsTransformFeedbackActive`, `IsTransformFeedbackPaused`,
  `InvalidateCompileEnv`\*, `ValidateProgramName`\*, `RecordError`\*,
  `GetBoundTransformFeedbackLifetimeId`, `HasOpenTransformFeedbackSpan`\*  (\* = the 7 sticky F).
* **9 fields are emitted but STILL PULLED** — `EmittedCallSuppliesTheWholeField` returns false
  (`PipeFill.cpp:1924-1939`, reasons at `:1875-1923`): `GetPixelStoreParameters` (only the PACK half
  has a carrier, D5/D10), `GetCurrentVertexAttribute` (the applier cannot reproduce GLContext's
  cross-view conversion), `GetBoundVertexArray`, `GetFramebufferBindingSlot`,
  `GetImageTextureBinding`, `GetTextureUnitObject`, `GetProgramForDraw`, `GetProgramForDispatch`
  (all six: *the storage is a frontend heap reference and the record carries an 8-byte
  `{slot, gen}`*), `GetMaxTouchedTextureUnit` (the set is hash-suppressed while the high-water mark
  still moves).

Union, de-duplicating `GetPixelStoreParameters`: **31 of 63 fields are not served by any pushed
record** — 24 V/O + the 7 sticky F. Only 32 fields (render state, dynamic, patch, attrib defaults,
pixel-pack half) are genuinely applier-supplied today.

### 2.2 The census table — by site, backend, what it reads, owner

O/F sites only (the V-class ones are all applier-supplied except the ones in §3). "owner" = the phase
whose row in `ROADMAP.md` retires it.

| site | backend | reads | owner |
|---|---|---|---|
| `DirectGLES/DirectGLES.cpp:4486` `PrepareForDraw` | Espryt | `GetBoundVertexArray` — **unconditional, every draw** | P8 (`PipeFill.cpp:1902-1905`: "what retires the pull is P8") |
| `DirectGLES.cpp:4497` `PrepareForDraw` | Espryt | `GetProgramForDraw` — **unconditional, every draw** | P3b/P4b→P8 |
| `DirectGLES.cpp:6287` `DrawArrays`, `:6097` `BoundElementArrayBuffer`, `:6326` MultiDrawArrays, `:1507` `SyncCurrentVertexAttributeValues`, `MultiDraw.cpp:96` | Espryt | `GetBoundVertexArray` | P8 |
| `DirectGLES.cpp:2995` `SyncCurrentFBO` (via `GetFramebufferBindingSlotChecked`) | Espryt | `GetFramebufferBindingSlot` → `.GetBoundObject()` handed to `SyncToBackend` — **labelled "MONOLITH GLUE" at `DirectGLES.cpp:2961`** | P3b/P4b |
| `DirectGLES.cpp:4357` `BindCurrentFBO`, `:4447` `ForceBindCurrentFBO`, `:2077 :2236 :2844 :3106 :4175` | Espryt | `GetFramebufferBindingSlot` | P3b/P4b |
| `Managers.cpp:8603` `GetReadColorAttachment`, `:8966` `IsFixedPointFallbackReadAttachment` | Espryt | `GetFramebufferBindingSlot(Read)` | P3b/P4b |
| `DirectGLES.cpp:1702 :1730 :1765 :1778` (`CaptureUnitBindings`/`UnitBindingsUnchanged` ×2 arms), `:2205` `SyncNeccessaryTextures`, `:4577 :4863 :5262`, `:7847 :8529 :8624 :8770 :11049` | Espryt | `GetTextureUnitObject` (13) | P3b/P4b (`g_unitTextureSyncList` epoch row) |
| `DirectGLES.cpp:2475` `SyncImageTextureBinding`, `:2592`, `Managers.cpp:10011` | Espryt | `GetImageTextureBinding` | P3b/P4b |
| `DirectGLES.cpp:627` `SyncBoundBuffer`, `:359 :905`, `:6397 :6428 :6429 :6499 :6530 :6531 :6703 :6744`, `:9407 :10389 :10626 :10936 :11362`, `Utils.cpp:2298`, `MultiDraw.cpp:88` | Espryt | `GetBufferBindingSlot` (18) — PixelPack / DrawIndirect / Parameter / DispatchIndirect / transfer targets, the 7 of 15 `BufferTarget`s **no call covers** (`Coverage.def:37-70`) | P8 (indirect), P9 (readback), P13 (transfer) |
| `DirectGLES.cpp:468 :527 :563 :586 :1250 :5164` | Espryt | `GetBufferBindingPoint` | P3b/P4b + P7 |
| `DirectGLES.cpp:1230` `StartPendingTransformFeedback` | Espryt | `GetTransformFeedbackProgram` | P3b/P4b (XFB scatter to client) |
| `DirectGLES.cpp:5779` `PrepareForCompute` | Espryt | `GetProgramForDispatch` | P3b/P4b |
| `DirectGLES.cpp:5805 :5810`, `:9202 :9203` (`ShaderStorageBlockBinding`) | Espryt | `ValidateProgramName` + `GetProgramObject` (F, sticky) | P5/P9 — these must become `kClientResolved`, ARCHITECTURE §3.2 "显式不移植" |
| `DirectGLES.cpp:8123`, `Managers.cpp:12812` | Espryt | `RecordError` (F, reverse channel) | P9 (`OnGlError`, ordered) |
| `DirectGLES.cpp:579` `SyncAtomicCounterBuffers` | Espryt | `GetBufferBindingPointCount` (F, sticky) | P7/P13 |
| `Managers.cpp:2656-2674` `IsBufferDrawCleanByHandle` | Espryt | **not via `MGB_CTX`** — a `const BufferObject*` parameter, `IsMapped()`. Comment: *"The frontend read retires the moment **P5** gives `HasLiveHostWrites` a producer, and it is the last frontend read in this function"* | **P5** |
| `Managers.cpp:2850-2862` `EnsureBufferResourceForHandle` | Espryt | **not via `MGB_CTX`** — `bufferObject->HasDefinedContent()`; *"retires the moment the client publishes a live content flag beside the descriptor (P5/P8)"* | **P5**/P8 |
| `Managers.cpp:7700`, `:9268`, `:12107`, `:12716`, `Managers.h:895`, `:2687`, `SlotTables.h:74`, `:258` | Espryt | eight more sites explicitly labelled "MONOLITH GLUE" — handle minted from a frontend object/lifetime id *inside* `MG_Backend` | P3b/P4b, P13 |
| `Renderer/VulkanRenderer.cpp:3084 :5413 :5633 :6263 :6368 :6748 :7551 :7919 :8877 :8878 :9513 :10224`, `Renderer/UniformManager.cpp:556` | Magma | `GetFramebufferBindingSlot` (13) | **P7** |
| `VulkanRenderer.cpp:7423 :10979 :12397 :12403 :12498 :12568`, `DirectVulkan.cpp:279 :403 :466 :534 :587` | Magma | `GetBufferBindingSlot` (11) | **P7** |
| `UniformManager.cpp:507 :783 :822 :848 :1663`, `VulkanRenderer.cpp:9505 :11010 :11254` | Magma | `GetTextureUnitObject` (8) | **P7** |
| `VulkanRenderer.cpp:6349 :6755 :12387 :12488`, `DirectVulkan.cpp:787 :911` | Magma | `GetBoundVertexArray` (6) | **P7** |
| `UniformManager.cpp:1017 :1296 :1851 :1939` | Magma | `GetImageTextureBinding` (4) | **P7** |
| `VulkanRenderer.cpp:6287 :6718 :6756` | Magma | `GetProgramForDraw` (3) | **P7** |
| `UniformManager.cpp:1195 :2019`, `VulkanRenderer.cpp:11607` | Magma | `GetBufferBindingPoint` (3) | **P7** |
| `VulkanRenderer.cpp:11568 :11661` | Magma | `GetTransformFeedbackProgram` (2) | **P7** |
| `VulkanRenderer.cpp:7327 :7379` | Magma | `GetProgramForDispatch` (2) | **P7** |
| `VulkanRenderer.cpp:1339 :1343 :1399`, `DirectVulkan.cpp:717` | Magma | `RecordError` (F) | P9 |
| `BackendObject_DirectVulkan.cpp:390 :788` | Magma | `InvalidateCompileEnv` (F) | P9 (`OnCapsInvalidated`) |
| `UniformManager.cpp:1190 :2014` | Magma | `GetBufferBindingPointCount` (F) | P7 |
| `VkTextureManager.cpp:810` | Magma | `GetTextureObject` (F, by GL name) | P7 |
| `VulkanRenderer.cpp:11527` | Magma | `HasOpenTransformFeedbackSpan` (F) | P7/P9 |
| `DirectVulkan.cpp:271 :274` | Magma | `ValidateProgramName` + `GetProgramObject` (F) | P7 |

Coverage.def's own arithmetic, for cross-checking a brief:
`generated/PipeCoverage.inc:98-104` — 477 inventory read points = 299 → call + 5 client-resolved +
6 reverse-channel + **167 structural handle** + 0 UNMAPPED. Coverage.def:16 says UNMAPPED becomes a
**hard zero gate from P5 onward** — it is already 0, so that half of the gate is free.

`MGPipeUnmigratedEmulation` (declared `MG_Pipe/PipeApply.h:1068`, monolith body
`MG_Pipe/PipeApply.cpp:2820` = `(void)name;`) has **five** call sites, not the six the P4a ROADMAP row
claims ("`Managers.cpp` 2 处、`DirectGLES.cpp` 4 处" — `Managers.cpp:5287` is a *comment*, not a call):

```
DirectGLES/Managers.cpp:5334   "texture-remint-pull"
DirectGLES/DirectGLES.cpp:8051 "generate-mipmap-storage"
DirectGLES/DirectGLES.cpp:8702 "generate-mipmap-cpu-fallback"
DirectGLES/DirectGLES.cpp:8997 "copy-image-shadow-mirror"
DirectGLES/DirectGLES.cpp:10623 "get-tex-image-shadow"
```
`PipeApply.cpp:2812-2817` and `PipeApply.h:1055-1066` both say in so many words: **"P5 and P8 give it
teeth by editing one function instead of rediscovering five call sites."** The list is pinned by
`MG_Test/Pipe/PipeCatalogueTest.cpp:923-959`
(`EveryUnmigratedEmulationIsNamedOnce`). None of the five is on the reduced path (§3).

---

## 3. **The most important answer: what a SPLIT reduced path actually hits**

Reduced path = `glClear` (class `kClear`) + a triangle (`kDraw`) + `glReadPixels` (`kReadback`),
then the OpenRA trace (adds `kBlitOrCopy`, `kTextureOp`, `kProgramOp`).

Intersect each class's may-read mask (`FillPoints.def:157-339`) with §2.1(b)'s
"no record supplies it" set. **These are the fields P5 must either migrate or Fatal on:**

### kClear — 7 of its 18 own fields
`GetFramebufferBindingSlot`, `GetTextureUnitObject`, `GetImageTextureBinding`,
`GetTextureContextId`, `GetSamplingResolutionGeneration`, `GetTextureBindGeneration`,
`GetMaxTouchedTextureUnit`.
The other 11 (`GetRenderStateParameters/Version`, `GetViewport`, `IsCapabilityEnabled`,
`GetClearColor/Depth/Stencil`, `GetScissorBox`, `GetColorMaskIndexed`, `GetDepthMask`,
`GetStencilState`) are applier-supplied today. Espryt's `Clear` is `DirectGLES.cpp:5828`; it calls
`SyncNeccessaryTextures()` (`:2313` → `CaptureDrawTextureSyncKeys` `:2019`), `SyncCurrentFBO()`
(`:3084`), `SyncRenderState`, `BindCurrentFBO(Draw)` (`:4303`).

### kDraw — **19 of its 47 own fields**
`GetBoundVertexArray`, `GetProgramForDraw`, `GetBufferBindingSlot`, `GetBufferBindingPoint`,
`GetTouchedBufferBindingPointCount`, `GetTextureUnitObject`, `GetTextureContextId`,
`GetTextureBindGeneration`, `GetMaxTouchedTextureUnit`, `GetSamplingResolutionGeneration`,
`GetImageTextureBinding`, `GetCurrentVertexAttribute`, `GetFramebufferBindingSlot`,
`IsTransformFeedbackActive`, `IsTransformFeedbackPaused`, `GetTransformFeedbackProgram`,
`GetTransformFeedbackGeneration`, `GetBoundTransformFeedbackLifetimeId`,
`GetTransformFeedbackCapturedVertices`.
The remaining 28 are render-state/dynamic/patch — applier-supplied.

### kReadback — 12 of its 17 own fields
`GetPixelStoreParameters`\*, `GetBufferBindingSlot`, `GetFramebufferBindingSlot`,
`GetActiveTextureUnit`, `GetTextureUnitObject`, `GetTextureContextId`,
`GetSamplingResolutionGeneration`, `GetTextureBindGeneration`, `GetMaxTouchedTextureUnit`,
`GetImageTextureBinding`, `IsTransformFeedbackActive`, `IsTransformFeedbackPaused`.
Supplied: `GetClampReadColor`, `IsCapabilityEnabled`, `GetRenderStateParameters/Version`,
`GetViewport`.

\* **Two readings of `GetPixelStoreParameters`, and they differ.** (a) The field is
`m_pixelStore[2]` and only `[0]` (pack) has a carrier, so the *field* is unmigrated —
that is why it is absent from `MGP_COVERAGE_EMITTED_LIST` (`Coverage.def:172-181`) *and* false in
`EmittedCallSuppliesTheWholeField` (`PipeFill.cpp:1877-1881`). (b) A readback only ever reads the
pack half, which the applier does write (`PipeApply.cpp:1373`), so a split readback is materially
served. **What settles it:** whether any `MGB_CTX->GetPixelStoreParameters(...)` on the readback path
passes `isUnpack == true`. The five Espryt sites are `DirectGLES.cpp:7924` (`~ScopedPackState`),
`:9399`, `:10893`, `:11272`, `Utils.cpp:2302` — read the argument at each. My **guess** is all-pack,
i.e. reading (b), but the poison is per-field so it will Fatal under (a)'s rules regardless: **P5
either splits the field into pack/unpack or teaches the server stamper to stamp it.**

### **Union across the three = 21 distinct fields**
```
GetActiveTextureUnit            GetPixelStoreParameters        GetTextureUnitObject
GetBoundVertexArray             GetProgramForDraw              GetTransformFeedbackCapturedVertices
GetBufferBindingSlot            GetSamplingResolutionGeneration GetTransformFeedbackGeneration
GetBufferBindingPoint           GetTextureBindGeneration       GetTransformFeedbackProgram
GetTouchedBufferBindingPointCount GetTextureContextId          GetBoundTransformFeedbackLifetimeId
GetCurrentVertexAttribute       GetMaxTouchedTextureUnit       IsTransformFeedbackActive
GetFramebufferBindingSlot       GetImageTextureBinding         IsTransformFeedbackPaused
```
plus, of the 7 sticky F, `RecordError` (reachable from any error) and — on Magma only —
`InvalidateCompileEnv` at init. Only 3 of the 24 non-sticky unmigrated fields are **off** the reduced
path: `GetBoundTransformFeedbackName` (dead — `PipeInputs.h:232-234`, read by no backend since D21),
`GetTransformFeedbackPausedPrimitiveCounter` (`kQuery` only), `GetProgramForDispatch`
(`kDispatch` only).

### Concrete blockers the reduced path hits, ranked

1. **Nothing stamps anything on the server.** See §0. Until P5 gives the applier (or a server-side
   verb boundary) a stamper, `Fatal{…@<none>}` fires on the first read of `GetRenderStateParameters`
   inside `SyncRenderState` — i.e. before the interesting cases. **This is the gate's own
   prerequisite and it is not in the ROADMAP P5 row.**
2. **`PrepareForDraw` reads two pointers unconditionally** — `DirectGLES.cpp:4486`
   (`GetBoundVertexArray`) and `:4497` (`GetProgramForDraw`). No `#if`, no arm guard, no record
   fallback. Every triangle in split hits both. `PipeFill.cpp:1897-1908` says outright the pull is
   retired by P8, not P5 — so **P5's only options are a server-side substitute or a Fatal**, and a
   Fatal here means no split triangle, which is the exit gate. This is the phase's sharpest tension.
3. **`SyncCurrentFBO` is self-declared monolith glue.** `DirectGLES.cpp:2961-2965`: even on the handle
   arm, the twin's `SyncToBackend` still takes the frontend `SharedPtr<FramebufferObject>`, obtained
   at `:2995` via `GetFramebufferBindingSlotChecked`. Clear *and* readback both go through it.
   Contrast `BindCurrentFBO` (`:4303-4353`), which **does** return early from the record when
   `FramebufferSubsystemEnabled() && g_fboRecordsTrusted` and never reaches `:4357` — so the *bind*
   is already split-clean, the *sync* is not.
4. **`IsBufferDrawCleanByHandle` needs `HasLiveHostWrites` to have a producer** —
   `Managers.cpp:2656`, `Managers.h:911-913`, `PipeApply.cpp:839-845`, `:1905`, `PipeApply.h:225`.
   The field exists and is asserted pinned-false (`Managers.cpp:2671-2672`). ROADMAP's P5 bullet
   "client 侧块粒度 persistent-map 推送" **is** this producer; the assert at `:2671` fires the moment
   P5 sets it, which is the intended tripwire (`Managers.cpp:2644-2646`).
5. **`EnsureBufferResourceForHandle` reads `HasDefinedContent()`** — `Managers.cpp:2860-2862`,
   named P5/P8.
6. The three texture *shutters* (`GetTextureContextId`, `GetTextureBindGeneration`,
   `GetSamplingResolutionGeneration`) are read on **all three** classes and `Coverage.def:220-224`
   is explicit that no call carries them and none should — *"what replaces them server-side is the
   applier's own `Serial`, which is a different value with a different owner"*. So for P5 these are
   not "migrate the value", they are "the server answers them from its own state". `GetTextureContextId`
   and `GetActiveTextureUnit` are in the same bucket (`Coverage.def:215-219`).

### OpenRA trace (the second half of the gate)
Adds `kBlitOrCopy` (22 own; unmigrated subset adds `GetViewportIndexed`, `GetDepthRangeIndexed`,
`GetProvokingVertexMode` only if Magma — on Espryt those three are render-state-derived, and all
three of the `kBlitOrCopy`-specific rows are the Magma shader-blit rows of `FillPoints.def:265-275`),
`kTextureOp` (10 own, adds nothing new beyond `GetActiveTextureUnit`) and `kProgramOp` (11 own, adds
nothing new). **So the OpenRA trace does not widen the field set beyond the 21 above** — it widens the
*site* set, and it is the first thing that reaches `Managers.cpp:8603`/`:8966` (read-attachment) and
the texture-unit walks with `maxTouchedUnit >= 0`. **Guess**, on the grounds that OpenRA is a
textured 2D sprite renderer with FBO blits and no compute/XFB; a brief should verify against the
fixture's actual call set rather than take this on my word.

---

## 4. What `Fatal{UnmigratedPipeInput}` is today

**A real fatal, fully wired, with negative controls — not a stub.**

```cpp
// MobileGL/MG_Pipe/generated/PipeFilled.inc:407-413
[[noreturn]] inline void MGPipeInputPoisonFatal(MGPipeInputField field, const char* verb) {
    MGLOG_F("MGPipe: Fatal{UnmigratedPipeInput, \"%s@%s\"}",
            kMGPipeInputFieldNames[static_cast<SizeT>(field)], verb);
    std::abort();
}
```
Generated from `scripts/gen_pipe.py:909`. The verb-named wrapper is
`MGPipeInputPoisonFatalForVerb` (`MG_Backend/MGPipe/PipeInputs.cpp:25-27`), declared
`PipeInputs.h:32`. `MGLOG_F` + `std::abort()`, **live at every log level on purpose** —
`PipeInputs.h:29-31` says so explicitly: *"this is not `MOBILEGL_ASSERT`, which is inert in INFO
builds."*

Armed whenever `MOBILEGL_PIPE_POISON` (§1.4), which a disaggregated build turns on for free.
Existing coverage:
- `MG_Test/Pipe/PipeInputsTest.cpp:93` (`"…GetActiveTextureUnit@GenerateMipmap"`), `:292-296`
  (`"GetLineWidth@<none>"`), `:576`.
- `MG_IntegrationTest/Scenarios/PoisonOmissionScenario.cpp:85-88,341` — the negative control: omit
  one stamp, the child must abort with that exact prefix.
- `MG_IntegrationTest/Scenarios/PipeVerifyArmingScenario.cpp:78`.
- `MG_Test/ScopedPipeVerb.h:21` — the test-side "I am not inside a verb" helper.

**So P5 does not build this.** P5 builds the *server side of the stamp*, which is what turns a
universal abort into the gate ROADMAP describes. The sibling mechanism for emulations,
`MGPipeUnmigratedEmulation`, is the reverse: named and greppable but a **no-op today**
(`PipeApply.cpp:2820`), explicitly waiting for P5/P8 to give it a body.

---

## 5. The exit-order rule (P3a ID-18/21) and what it implies for a two-role build

### The rule
`~/w7/notes/p3a/INTEGRATOR-DECISIONS.md:215-227` (ID-18) and `:244-251` (ID-21); restated as a
standing ruling binding every later package at `~/w7/notes/p4a/INTEGRATOR-DECISIONS.md:42-44` (ID-8):

> **No frontend destructor may run from an exit handler into pipe/backend state. Every new static
> holder of frontend `SharedPtr`s is leak-at-exit storage; every new singleton reachable from a
> destructor is never-destroyed. Prove with `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` on both
> lanes.**

The original defect chain (ID-18): `exit` → `~PipeInputs` (`gPipeInputs` held a
`SharedPtr<VertexArrayObject>`) → `~VertexArrayObject` → `~BufferObject` →
`MGPipeEmitResourceDestroyAndFree` → `MGPipeSlotAllocator::FindByLifetimeId` on a hash table already
freed by `~MGPipeSlotAllocator` (a Meyers singleton constructed at the first buffer, so it dies
first). Every case ran the chain; only when the freed table still resolved did `Free()` write freed
vectors and glibc report `double free or corruption` — which is why it was **CI-only** until
`GLIBC_TUNABLES=glibc.malloc.tcache_count=0` made it deterministic.

In the tree today the rule is written out at `MG_Backend/MGPipe/PipeInputs.h:692-706`
(the 15-line comment above `inline PipeInputs& gPipeInputs = *new PipeInputs();`), and enforced at:
`MG_Impl/Pipe/PipeFill.cpp:376` (the verify snapshot block), `MG_Impl/Pipe/ProgramEmit.h:477`
(*"it MUST NOT hold a frontend `SharedPtr`"*), `MG_Impl/Pipe/ImageEmit.h:177`,
`MG_Impl/Pipe/SamplerEmit.h:1080`, `MG_Test/Pipe/FramebufferEmitTest.cpp:196`,
`MG_Util/Async/ShaderCompilePool.h:91`, and the pre-existing `Init.cpp:168`.

### What it implies for a two-role build

1. **The rule doubles.** Today it says "one process, one leak-at-exit set". In split there are two
   role-local singleton sets that must *each* obey it, and they die at different times and for
   different reasons (client `MobileGL::Destroy()`; server EOF-detect + `_exit`). Every new
   `MG_Remote/Client/*` and `MG_Remote/Server/*` singleton — `PipeEmitter`, `EmitTables`,
   `CapsMirror`, `ShadowArena`, `PersistentMapTracker`, `GpuWritePending`, `PipeApplier`,
   `PipeObjectTables`, `ReplyPool`, `EventRing`, `ServerLoop` (the file list is
   `ARCHITECTURE.md:575-576`) — is reachable from a destructor and therefore **never-destroyed by
   the standing ID-8 ruling**, not by a fresh judgement call.
2. **The server side is the easy half and the client side is not.** The server holds no frontend
   objects at all (that is the whole design), so its tables can be plain never-destroyed singletons.
   The client still holds `gPipeInputs` with its nine O-class members *and* now additionally owns the
   transport, whose teardown is ordered.
3. **`ARCHITECTURE.md:537` already fixes the teardown order and it is the opposite direction from a
   destructor chain**: *publish + server drains and acks → stop the apply thread → close the
   transport → client drains the compile pool (before `glslang::FinalizeProcess()` and the
   `pGLContext` destructor) → `MobileGL::Destroy()` → release sync/query handles.* Every one of those
   steps is an **explicit** call, not a destructor — which is exactly what ID-18 demands. **P5's job
   is to make sure nothing added in P5 can start a chain that runs *after* step 3**, because after
   the transport is closed a frontend destructor that reaches an emitter has no ring to write to.
4. **Two new death modes ID-18 never saw**: the server dying first (client reads EOF → device-lost
   latch, `ARCHITECTURE.md:563`) and the client dying first (server reads EOF → destroy native
   context and exit). In both, one side's objects are torn down while the other's are mid-flight. The
   `Doorbell.h` `Kill()` death state (`ARCHITECTURE.md:432`) exists for exactly this, but it is a
   *transport* answer, not an *exit-handler* answer.
5. **`HeadlessGL`'s fork pre-check is the exit-order trap in the new shape** —
   `ARCHITECTURE.md:556`: it forks a child that runs a full EGL bring-up and `_exit`s, and in split
   that child would spawn an orphan server. The stated rule (server EOF detection immediate and
   unconditional, sub-second; client fd shaped so `_exit` closes it deterministically) is listed as a
   **P6** acceptance item, but P5's `InProcessTransport` arm reaches the same `HeadlessGL` harness
   first. **Guess**: `inproc` has no fork and no orphan, so P5 is safe here and P6 is the right
   owner; a brief should say so explicitly rather than leave it ambiguous.
6. **The proof recipe is inherited, not new**: `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` on both
   lanes (ID-8, ID-20, ID-21). P3a ran `integration-verify 844/844` and push `integration-gpu
   966/966` under it. P5 should run its new split lane the same way from the first green, because the
   failure is layout-dependent and a clean run without the tunable proves nothing (ID-20 is exactly
   that lesson: *"not a fix, a layout change"*).

---

## 6. Side findings a brief writer will want

- **None of P5's named deliverables exist yet.** `MobileGL/MG_Remote/` at `a29807cc` contains only
  `Protocol/` and `Transport/` (19 files) — **no `Client/`, no `Server/`**.
  `MOBILEGL_TRANSPORT` appears exactly once in the whole source tree, in a comment
  (`MG_Remote/Transport/InProcessTransport.h:19`); there is no `MG_Config::Transport` and no
  `ConfigLoader` parsing. `MobileGL/MG_Backend/Init.cpp` has no disaggregated branch.
  `MOBILEGL_BUILD_DISAGGREGATED_INPROC` is not a CMake option (`CMakeLists.txt:23,28,29,36` are the
  four MGPipe options that do exist). `SPLIT` / `TRACE_TRANSPORT` appear nowhere in
  `CMakeLists.txt`, `tools/` or `scripts/`. `PersistentCoherentMapScenario` does not exist.
  `MGPCaps` the **struct** does exist (`MG_Pipe/MGPipeTypes.h:126`, `PipeCalls.def:80` `GetCaps`),
  but no snapshot machinery.
- **No `DirectGLES.Split.*Triangle` test exists**, and there is no `Triangle` scenario at all —
  `ClearThenReadPixelsScenario.cpp` is real (`MG_IntegrationTest/Scenarios/`, 5 `TEST_F`s, the
  closest to a triangle being `ADrawIntoTheDefaultFramebufferSurvivesAnEarlierClear` at `:308`).
  Two readings: (a) the gate names two *new* minimal scenarios P5 must author; (b) it means the
  existing `ClearThenReadPixels` suite plus some existing draw test renamed under a `Split.` prefix.
  **What settles it:** whether `add_trace_replay_test`/the ctest registration grows a `Split.` name
  prefix (ARCHITECTURE.md:584 describes the `SPLIT` suffix for *trace* tests, not for ctest scenario
  names). Reading (a) is my **guess**.
- **`ARCHITECTURE.md:83` is stale**: "PipeInputs 字段 id（61 个）" vs the generated
  `static_assert(kMGPipeInputFieldCount == 63)`.
- **ROADMAP's P4a row is off by one** on the emulation count: it says 6 sites
  (`Managers.cpp` 2 + `DirectGLES.cpp` 4); there are 5 calls + 1 comment.
- **`ARCHITECTURE.md:299` names the ack hook P5 inherits**: *"monolith 下 ack 是 `((void)0)`
  （applier 只隔一次函数调用），**P5 把门铃接到这个谓词上**"* — the predicate being
  `MGPipeResourceRespecifyNeedsAck(desc)` (`MG_Pipe/MGPipeTypes.h:680`), narrowed by P4a (D-A2) to
  buffer targets only. That is a one-line P5 wiring item with a test already pinning it
  (`MG_Test/Pipe/PipeCatalogueTest.cpp:552-585`).
- **`MG_Pipe/PipeApply.h:1034-1038`**: `create_shader_state` passes the artefacts **by pointer**
  beside the record with `Size 0` and does not call the codec — *"Splitting this record for a
  transport whose ring caps one record at half its capacity is P5's problem."* So the program record
  is a P5 chunking item, and `ProgramArtifactsCodec.{h,cpp}` goes hot for the first time
  (`ARCHITECTURE.md:265`). The companion-pointer shape recurs at `PipeApply.h:1052-1053`
  (`set_global_constants` takes `const void* bytes` with `Blob.Size 0`) — **guess**: every
  companion-pointer payload in `PipeApply.h` is a P5 serialisation item; worth a systematic grep for
  `Size 0` / `companion pointer` in that header.
- **`MG_Impl/Pipe/SamplerEmit.h:432`** is a fourth self-declared "P5's problem" marker.
- `ARCHITECTURE.md:492` pins `map-persistent-roundtrips` (`mpr`) as *every* `MapPersistent`
  emission, minted or refused (`MG_Impl/Pipe/PipeFill.cpp:736`,
  `MG_Util/Metrics/PipeStats.h:126`), with two existing gates
  (`StorageBufferRegrowScenario.cpp:255`, `LargeArenaAdoptionScenario.cpp:450`). P5's
  `persistent-map-push` counter is the one that is **not yet wired** — `ARCHITECTURE.md:619` lists it
  as "P0 未接线：monolith 期不存在推送".
