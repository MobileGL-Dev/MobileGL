# P5 scout — the caps snapshot, the reply slot, and the reverse channel

Tree read: `/home/swung/w7/pipe` (WSL), branch `feat/disaggregated` @ `a29807cc` (P4a landed), read over
`//wsl.localhost/Arch/...`. **Nothing was written, built, or run in that tree.** Every line number below was
opened. Repo-relative paths are under `MobileGL/` unless the path starts with `docs/`, `tools/`, `scripts/`
or `~/w7/`.

Background read first: `docs/Disaggregated/ARCHITECTURE.md` §3.3, §4, §8, §10, §11, §13.1, §15.1;
`docs/Disaggregated/ROADMAP.md:21` (the P5 row) and open questions 1-2 (`:80-85`);
`~/w7/notes/p4a/INTEGRATOR-DECISIONS.md` ID-1..57.

**One-sentence summary of the whole subject.** Of the five things P5's row names in my area, exactly one
has a live producer *and* a live consumer today (`OnGpuWritten` / `OnBufferWriteback`, DirectGLES-only);
`MGPCaps`, `MGPReplySlot` and the other eight callbacks are declared-and-asserted **but have zero call
sites in the whole tree** — P5 is their first implementation, not their migration.

---

# PART 1 — `MGPCaps`: the startup snapshot

## 1.1 Where it is defined

| thing | file:line | notes |
|---|---|---|
| `enum MGPCapBit : Uint64` | `MG_Pipe/MGPipeTypes.h:108-124` | 9 bits: `kCapNone`, `kCapViewportArray`, `kCapFloat64VertexAttrib`, `kCapResidentSubData`, `kCapCpuXfbPrimitiveAccounting`, `kCapTimerQuery`, `kCapOcclusionQuery`, `kCapXfbPrimitivesQuery`, `kCapNeedsHostIndexBytes`, `kCapNeedsHostUboBytes`. (That is 8 real caps + `kCapNone`; `kCapNeedsHostUboBytes` is `1<<8`.) |
| `struct MGPCaps` | `MG_Pipe/MGPipeTypes.h:126-139` | four members: `DynamicBackendParameters Dynamic` (`:132`), `Uint64 CallMask` (`:133`), `MGPBlobRef FormatCapabilities` (`:137`), `MGPBlobRef RendererInfo` (`:138`) |
| size assert | `MGPipeTypes.h:140`, `:145-146` | stated as a **composition**, not a literal: `sizeof(MGPCaps) == sizeof(DynamicBackendParameters) + 8 + 24 + 24`, because `DynamicBackendParameters` still carries `SizeT` members and is ABI-dependent until P0.5's fixed-width move (which has **not** happened — see 1.6/risk R1) |
| field list for G4/verify | `MG_Pipe/PipeFields.def:41-42` (`MGP_FIELDS_MGPCaps(F) = F(Dynamic) F(CallMask) F(FormatCapabilities) F(RendererInfo)`), registered in the POD list at `PipeFields.def:338` |
| catalogue row | `MG_Pipe/PipeCalls.def:80` — `X(GetCaps, MGPCaps, kScreen, kReplySlot)`, **opcode 1** (the wire opcode is the 1-based position in `MGP_CALL_LIST`, `PipeCalls.def:29`) |
| generated table slot | `MG_Pipe/generated/PipeTables.inc:18` — `void (*GetCaps)(const MGPCaps* payload, MGPReplySlot* reply);` |
| generated monolith thunk | `MG_Pipe/generated/PipeThunks.inc:20-22` — `gMGPipeScreen.GetCaps(payload, reply)` |
| generated wire record | `MG_Pipe/generated/PipeWire.inc:123-126` |
| generated verify | `MG_Pipe/generated/PipeVerify.inc:53`, `:133`, `:340-341` |

`DynamicBackendParameters` itself is `MG_Backend/BackendObject.h:316-549` (the struct ends at `:549`;
`GpuVendorKind GpuVendor` at `:548` is the last member, `enum class WindowBackend` follows at `:551`).
That is ~90 scalars plus the six per-axis compute limits `MaxComputeWorkGroupCount[3]` /
`MaxComputeWorkGroupSize[3]` and one method pair (`PerLayerFramebufferAttachmentBit` /
`SupportsPerLayerFramebufferAttachment`) — POD-safe, methods are `constexpr`/const.

`MGPipeTypes.h:28` records the deliberate coupling: `MGPCaps` embeds `DynamicBackendParameters`
**by inclusion**, so that a caps field added in `BackendObject.h` needs no second edit.

## 1.2 Who fills it, and when

**Today nobody fills it.** `MGPCaps` has **no producer and no consumer** anywhere:
`gMGPipeScreen` / `gMGPipeContext` are declared at `MG_Pipe/MGPipe.h:137-138` and are read by exactly one
place in the tree, `MG_Test/Pipe/PipeCatalogueTest.cpp:107` and `:111` (which walk them as arrays of
function pointers to assert the table arity). No backend installs either table; the landed P3a/P4a wiring
goes through the free functions `MGPipeApply*` in `MG_Pipe/PipeApply.h` instead, and `PipeApply.h`
declares **no** `MGPipeApplyGetCaps`. (Full list of the 37 `MGPipeApply*` entry points that do exist:
`PipeApply.h:740-1052`; no caps, no read_pixels, no fence, no query.)

What *does* fill the underlying state, and when:

- `BackendObject::InitCapabilities()` is pure virtual at `BackendObject.h:573` and is called **lazily,
  from inside `BackendObject::MakeEGLCurrent`** at `MG_Backend/BackendObject.cpp:341-347`, guarded by
  `m_backendCapabilitiesInitialized` (`:341`, set at `:346`). It is **not** called from
  `MG_Backend::Init()`.
- The latch is cleared by `BackendObject::ResetEGLRuntimeState()` (`BackendObject.cpp:359`, clear at
  `:362`), which runs on surface release/teardown from three sites (`:429`, `:461`, `:467`). So
  **`InitCapabilities` can run more than once per process** — at most once per surface lifetime, exactly
  as `ARCHITECTURE.md` §13.1 says ("surface 生命周期与首次 `MakeCurrent`+`InitCapabilities` 每 surface 至多一次").
- DirectGLES: `BackendObject_DirectGLES::InitCapabilities()` at
  `MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp:853-874` — `FillInGLESCapabilities` →
  `SetGLESCapabilities` → `UpdateAdvertisedCapabilityExtensions` → `UpdateDynamicBackendParameters` →
  `PopulateFormatCapabilities` → `PrintFormatCapabilities`.
- DirectVulkan: `BackendObject_DirectVulkan::InitCapabilities()` at
  `MG_Backend/DirectVulkan/BackendObject_DirectVulkan.cpp:371-395` — `QueryVulkanCapabilities` →
  `UpdateDynamicBackendParameters` → `UpdateAdvertisedExtensions` → **`MGB_CTX->InvalidateCompileEnv()`
  (`:390`, guarded by `MGB_CTX_LIVE`)** → `PopulateFormatCapabilities`.

**The ordering trap P5 must design around** (`BackendObject_DirectGLES.cpp:780-792`, verbatim in the
comment): `LogBackendInfo()` (`MG_Backend/Init.cpp:15`) reads
`pActiveBackendObject->GetRendererInfo()` at `MG_Backend/Init.cpp:21` **during `MG_Backend::Init()`**
(`Init.cpp:48`, `LogBackendInfo()` called at `:69`) — i.e. **before any ES/Vulkan context exists and
before `InitCapabilities` has run**. Both backends therefore build a *provisional* extension list in
their static initializer and reconcile it at the end of `InitCapabilities`
(`UpdateAdvertisedCapabilityExtensions`, `BackendObject_DirectGLES.cpp:793-799`;
`BackendObject_DirectVulkan::UpdateAdvertisedExtensions`, `:793`). Under split, `MG_Backend::Init()` is
the single hook where `BackendObject_Remote` is installed (`MG_Backend/Init.cpp:48-70`), and
`LogBackendInfo()` will call `GetRendererInfo()` on it *before the server has a surface*. Two readings,
and the brief must pick one:

  (a) `BackendObject_Remote::GetRendererInfo()` returns an empty/placeholder `RendererInfo` until the
      first `CapsSnapshot` arrives, and P5 accepts one wrong log line at startup; or
  (b) `MG_Backend::Init()` is restructured so `LogBackendInfo()` is deferred to first-MakeCurrent.

  **Evidence that would settle it:** whether anything other than `MGLOG_I` reads `GetRendererInfo()`
  before the first `eglMakeCurrent`. I checked: in non-test code the only pre-MakeCurrent reader is
  `LogBackendInfo` (`Init.cpp:21`); every other reader is behind a GL entry point (list in 1.3), so (a)
  is cheap. **Guess:** (a) is right, but I did not trace the FCL/launcher startup order.

## 1.3 Who reads caps on the client today — the exact enumeration

Counts are of **non-test, non-`MG_Backend`** sites (`MG_Impl` + `MG_State` + `MG_Util` + `Init.cpp`):

| accessor | sites | where |
|---|---|---|
| `GetDynamicParameters()` | **40** | `GLImpl/Getter/GL_Getter.cpp` 19 · `GLImpl/Texture/GL_Texture.cpp` 6 · `GLImpl/Framebuffer/GL_Framebuffer.cpp` 4 · `GLImpl/Program/GL_Program.cpp` 4 · `GLImpl/{Buffer,Framebuffer,Texture,VertexArray}/Validators.cpp` 1 each · `GLImpl/VertexArray/GL_VertexArray.cpp` 1 · `MG_Util/ShaderTranspiler/CompileEnv.cpp` 1 · `MG_Util/ShaderTranspiler/ShaderCompiler.cpp` 1 |
| `GetRendererInfo()` | **7** | `GL_Getter.cpp:596`, `:654`, `:2400` · `GL_Program.cpp:1424` · `GL_Query.cpp:77` · `GL_Texture.cpp:5903` · `CompileEnv.cpp:124` |
| `GetFormatCapabilities()` | **4** | `GL_Framebuffer.cpp:127`, `:784` · `GL_Texture.cpp:570`, `:6855` |
| `GetBackendType()` | **3** | `GL_Framebuffer.cpp:47` · `GL_Texture.cpp:6536` · `CompileEnv.cpp:122` |
| `GetBackendAPIVersionString()` | **2** | `GL_Getter.cpp` 1 · `MG_Util/SelfTest/DriverPost.cpp` 1 (the POST probe — **server-side** under split, `ARCHITECTURE.md` open question 7) |

That is **56 client caps read points** (the doc's "40 `pActiveBackendObject->` 站点 + 89 caps 读点" in
`ARCHITECTURE.md:116` counts differently — it counts *field* reads inside those sites, e.g. `GL_Getter.cpp:867`
binds one `dynamicParameters` reference and then reads many members. Do not use 40+89 as a work estimate;
**40 is the number of `GetDynamicParameters()` call expressions**, and it coincides with the doc's 40 by
accident of arithmetic. Guess: the doc's 40 means the `pActiveBackendObject->` *expressions* and the 89
means individual field reads; I did not count the 89.)

Two hot call sites are worth naming because they bind a reference and read many fields at once:
`GL_Getter.cpp:2400-2401` (`glGetFloatv`/`glGetIntegerv` scalar family; binds **both** `rendererInfo`
and `dynamicParameters`) and `MG_Util/ShaderTranspiler/CompileEnv.cpp:120-124`
(`env->backend = GetBackendType()`, `env->params = GetDynamicParameters()` — a **whole-struct copy** —
and `env->advertisedExtensions = GetRendererInfo().RendererGLInfo.Extensions`).

## 1.4 What MUST be in the snapshot (a split client cannot call the driver)

Everything below is client-consumed and device-answered. I list the carrier that already exists.

1. **`DynamicBackendParameters` (whole struct, by value).** `MGPCaps::Dynamic`
   (`MGPipeTypes.h:132`). Consumers: the 40 sites above. Non-negotiable — `CompileEnv.cpp:123` copies
   the whole struct into the compile env, so a partial snapshot is not an option.
2. **`RendererInfo`** = `RendererName`, `BackendName`, `Optional<String> ExtraVendor`,
   `GLInfo RendererGLInfo{TargetGLVersion, TargetGLSLVersion, Vector<GLExtension> Extensions,
   IsCompatibilityProfile}`, `StaticBackendCapabilityT{AllowVSOnlyPrograms}`
   (`MG_Util/Types.h:317`, `:321`, `:328-334`). Not POD (three `String`s + a `Vector`) → travels as the
   `MGPCaps::RendererInfo` blob (`MGPipeTypes.h:138`). Consumers: `glGetString` (`GL_Getter.cpp:596`),
   `glGetStringi(GL_EXTENSIONS)` (`GL_Getter.cpp:654`), `glGetIntegerv` scalar family (`:2400`),
   `AllowVSOnlyPrograms` at link (`GL_Program.cpp:1424`), timer-query gating in
   `glGenQueries`/`glQueryCounter` (`GL_Query.cpp:77`), anisotropy/`GL_EXT_texture_filter_anisotropic`
   in `glTexParameter` (`GL_Texture.cpp:5903`), and the **compile env's advertised extension list**
   (`CompileEnv.cpp:124`) — which feeds glslang phase A/B, which stays on the client
   (`ARCHITECTURE.md` open question 7).
3. **`FormatCapabilityCache`** (`BackendObject.h:93-98`) = `FullCaps` + `CaveatCaps`
   (`FormatCapabilityTable`, `BackendObject.h:88`) + `SampleCounts` (`FormatSampleCountTable`,
   `BackendObject.h:90` — a `Vector<Int>` per (target, format) pair, which is why
   `MGPipeTypes.h:134-136` says it must be a blob). Dimensions:
   `kFormatCapabilityTargetCount = TextureTargetCount + 1` (`BackendObject.h:81-83`) ×
   `kFormatCapabilityFormatCount = TextureInternalFormatCount` (`:84`). Consumers:
   `GL_Framebuffer.cpp:127` (renderable-format check in `CheckCompleteness`'s helper),
   `GL_Framebuffer.cpp:784` and `GL_Texture.cpp:570` (`GL_SAMPLES`/`GL_NUM_SAMPLE_COUNTS` answers),
   `GL_Texture.cpp:6855`. **Serialization shape is P5's to invent** — `MGPipeTypes.h:136` says in as many
   words "Their serializers land with the transport (P5)".
4. **`BackendType`** (3 sites). Already implied by `Hello.backendType` (`protocol.fbs:57-64`) but the
   *client* reads it through `pActiveBackendObject->GetBackendType()`; `BackendObject_Remote` must answer
   it locally. `GL_Texture.cpp:6536` uses it to pick the `GetTextureImage` path, `GL_Framebuffer.cpp:47`
   to pick the distinct-depth-stencil path, `CompileEnv.cpp:122` to key the compile env.
5. **`GetBackendAPIVersionString()`** (1 client site in `GL_Getter.cpp`). Already in the control-plane
   schema as `CapsSnapshot.apiVersion` (`protocol.fbs:91`).
6. **`CallMask`** (`MGPipeTypes.h:133`) — replaces "is this table slot null" as the feature probe
   (`ARCHITECTURE.md:114`). The schema already reserves `CapsSnapshot.tableSlotMask: ulong`
   (`protocol.fbs:94`) with the comment "which GLFunctionsTable slots the peer registered".
   **Note the mismatch:** `CallMask` is documented as `MGPCapBit` bits (8 of them), but `tableSlotMask`
   is documented as *table slots*. Two readings: (i) they are the same field and the schema comment is
   stale; (ii) they are two different masks and both are needed (cap bits + registered-slot bitmap).
   **Evidence that would settle it:** `GL_Drawing.cpp:283`'s live use of "slot non-null" as a capability
   probe, and `ARCHITECTURE.md:114`'s sentence "`CallMask` 取代『槽位是否为 null』这个隐式能力探测" — which
   reads as (i). **Guess: (i)**, and the brief should say so explicitly, because 71 catalogue rows do not
   fit in the 8 cap bits and someone will otherwise build both.
7. **The six compute limits** ride inside `Dynamic` (`BackendObject.h:381-392`); the schema *also* carries
   `maxComputeWorkGroupCount`/`Size` as separate `[int]` arrays (`protocol.fbs:92-93`) — redundant with
   (1), and `AdvertisedLimitsScenario.ComputeWorkGroupLimitsAreTheCapsBlocksAnswer`
   (`MG_IntegrationTest/Scenarios/AdvertisedLimitsScenario.cpp:580-623`, harness
   `MG_IntegrationTest/Harness/BackendCapsPeek.{h,cpp}`) is the existing gate that pins the caps copy
   and `glGetIntegeri_v` to one number. **Reuse it as P5's caps gate** — it already runs on every lane.
8. **`prefersCpuXfbPrimitiveAccounting`** (`protocol.fbs:95`) — the schema's spelling of
   `kCapCpuXfbPrimitiveAccounting`; same redundancy question as (6).

**Already present in the control plane (do not re-invent):** `protocol.fbs:86-96`
`table CapsSnapshot { dynamicParameters: [ubyte]; rendererInfo: [ubyte]; formatCaps: [ubyte];
extensions: [string]; apiVersion: string; maxComputeWorkGroupCount: [int]; maxComputeWorkGroupSize: [int];
tableSlotMask: ulong; prefersCpuXfbPrimitiveAccounting: bool; }`, with the header comment at
`protocol.fbs:83-85` ("The three blobs are byte-for-byte images of the corresponding POD structs; they are
versioned by structSize-first discipline, not by this schema"). It is a `CtrlMsg` union member
(`protocol.fbs:220`) — i.e. **the snapshot travels on the CTRL socket, not in `SEG_CMD`**, and the
`GetCaps` catalogue row (`PipeCalls.def:80`) is the *in-process/monolith* spelling of the same answer.
`protocol.fbs:98-104` also already has `table DefaultFramebufferInfo` and `SurfaceReply.defaultFb`
(`:142-148`) — that is the `OnSurfaceChanged` payload's control-plane twin.

## 1.5 What a caps snapshot CANNOT answer

1. **Caps *invalidation* is an event, not a value.** `GLContext::GetCompileEnv()`
   (`MG_State/GLState/Core.cpp:33-43`) memoises the compile env and re-captures it when
   **the raw pointer `MG_Backend::pActiveBackendObject.get()` changes** (`Core.cpp:34`,
   `m_compileEnvBackend`). Under split that pointer is the client's single long-lived
   `BackendObject_Remote` and **never changes**, so a server-side `InitCapabilities` re-run (surface
   recreate, `ResetEGLRuntimeState` → next `MakeEGLCurrent`) would leave the client's compile env, its
   preprocess memos and its advertised-extension list stale with nothing saying so. This is exactly what
   `OnCapsInvalidated` is for (`MG_Pipe/MGPipeCallbacks.h:45`), and its two producers today are
   `BackendObject_DirectVulkan.cpp:390` and `:788`. **DirectGLES has zero producers** — it re-runs
   `UpdateAdvertisedCapabilityExtensions` + `UpdateDynamicBackendParameters` at
   `BackendObject_DirectGLES.cpp:865-871` and tells the frontend nothing, because in monolith the
   frontend reads the same object. **Under split that silence becomes a bug.** P5 either (a) ships the
   caps snapshot again on every `InitCapabilities` and treats re-arrival as invalidation, or (b) adds a
   DirectGLES `InvalidateCompileEnv` call to match DirectVulkan. **(a) is the smaller diff and does not
   touch `dev`-shaped backend code** — recommend (a).
2. **Default-framebuffer geometry is not caps.** `pDefaultFramebufferInfo` is written by the *backends*
   into `MG_Impl` (`DirectGLES.cpp:11467` `PublishDefaultFramebufferDepthStencilFormat`;
   `DirectVulkan/Renderer/SwapchainObject.cpp:276-295`, which even calls
   `TextureObject2D::AllocateStorage` on a frontend object). It changes per surface/resize, so it is
   `OnSurfaceChanged` (`MGPipeCallbacks.h:43`, payload `MGPSurfaceInfo` at `MGPipeTypes.h:1321-1328`) /
   `SurfaceReply.defaultFb` — never the snapshot. Note `kMGPipeDefaultFramebuffer = {0,1}`
   (`MG_Pipe/MGPipeHandles.h:73`) already exists to retire the four
   `pDefaultFramebufferInfo->defaultFBO` identity compares in DirectGLES (`DirectGLES.cpp:2846`,
   `:3137`, `:4370`, `:4414`).
3. **Per-format sample counts have a second, non-caps source.**
   `ClampSamplesToBackendSupport` (`BackendObject_DirectGLES.cpp:806-828`) falls back to
   `GetGLESFormatMaxSamples(g_GLESCapabilities, ...)` when the probed `SampleCounts` vector is empty
   (`:820-826`). That fallback reads a **backend-private** capabilities struct. It is a backend-side
   function so it stays server-side, but if any client path ever needs the clamped answer it cannot get
   it from `MGPCaps` alone.
4. **Live GL state dressed as a "cap".** `GL_Getter.cpp:531-545` floors backend limits against frontend
   constants, and `GL_Getter.cpp:267-269` only consults the backend when the *currently bound* buffer
   target is `ShaderStorage`. These are fine (the flooring is client-side arithmetic over snapshot
   values) — flagged only so a reviewer does not mistake them for driver round trips.
5. **`GetBackendFunctions()`** (`BackendObject.h:592`) has **zero** client-side call sites outside
   `MG_Backend` (verified). The client's copy `gBackendFunctionsTable` is assigned once at
   `MG_Backend/Init.cpp:44`; under split it must be either a table of emitters or all-null with
   `CallMask` answering instead.

## 1.6 Risks for the brief

- **R1 (real, blocks the wire).** `MGPCaps`'s size assert is a *composition* precisely because
  `DynamicBackendParameters` contains `SizeT` (`BackendObject.h:317`, `:321`, `:546`) and `GLenum`
  (`:445-446`). `ARCHITECTURE.md` said P0.5 would move the caps block into `MGPipeValueTypes.h` with
  fixed-width members; **that has not happened** (`MGPipeTypes.h:141-144` still says "until P0.5 moves…").
  A byte-for-byte blob of a struct with `SizeT` members is not portable client↔server if the two ever
  differ in word size. For `inproc` (P5) this is harmless; for `spawn` (P6) on the same machine it is
  also harmless. **Recommend: P5 asserts same-`sizeof` in the `Hello`/`Welcome` handshake
  (`protocol.fbs:57-75`, `abiMajor/abiMinor/buildFingerprint`) and defers the fixed-width rewrite.**
- **R2.** `MGPCaps` is `kScreen` + `kReplySlot` in the catalogue but the real snapshot is a CTRL-socket
  `CapsSnapshot` message. Two mechanisms for one answer; the brief should state which one `inproc` uses
  (**guess:** the CTRL one, because `InProcessTransport` must run the *same* G3 codec as spawn and the
  handshake is the same code path).

---

# PART 2 — `kReplySlot` and `MGPReplySlot`

## 2.1 The flag and the type

- `kReplySlot = 1u << 4` (`MG_Pipe/MGPipe.h:50`), documented as **"Answers into an `MGPReplySlot`; never
  blocks."** The legend in the catalogue header is `PipeCalls.def:18`.
- `struct MGPReplySlot { Uint64 Id; }`, `MGP_ASSERT_POD(MGPReplySlot, 8)` —
  `MG_Pipe/MGPipeTypes.h:76-81`. The comment at `:76-77`: *"Where an asynchronous answer lands. Every
  server query in this catalogue is async-with-handle; none of them blocks (section 4.4.6, 'the total
  rule')."* Field list `PipeFields.def:32-33`.
- It is **8 bytes and one opaque `Id`** — there is no status, no size, no segment. Everything about where
  the bytes are is P5's to define. The obvious home is `SEG_REPLY` (server-owned, 8 MiB, 4 KiB slots —
  `ARCHITECTURE.md` §11.1; `SegmentKind.Reply = 3` at `protocol.fbs:40`; `Welcome.replyPool` at
  `protocol.fbs:73`).

## 2.2 The ten calls that carry it

All from `PipeCalls.def`; opcode = 1-based position in `MGP_CALL_LIST` starting at `:80`.

| op | call | payload | class | flags | line |
|---|---|---|---|---|---|
| 1 | `GetCaps` | `MGPCaps` | `kScreen` | `kReplySlot` | `:80` |
| 5 | `MapPersistent` | `MGPHandleOnly` | `kScreen` | `kReplySlot\|kOptional` | `:84` |
| 8 | `FenceStatus` | `MGPHandleOnly` | `kScreen` | `kReplySlot` | `:87` |
| 9 | `FenceWait` | `MGPFenceWait` | `kScreen` | `kReplySlot` | `:88` |
| 15 | `QueryAvailable` | `MGPHandleOnly` | `kCtxQuery` | `kReplySlot` | `:94` |
| 16 | `QueryResult` | `MGPQueryResultRequest` | `kCtxQuery` | `kReplySlot` | `:95` |
| 55 | `ResourceReadback` | `MGPReadback` | `kCtxObject` | `kReplySlot` | `:138` |
| 58 | `GetTextureImage` | `MGPReadbackInfo` | `kCtxObject` | `kReplySlot` | `:141` |
| 61 | `ReadPixels` | `MGPReadbackInfo` | `kCtxVerb` | `kReplySlot` | `:145` |
| 69 | `QueryTimestamp` | `MGPTimestampRequest` | `kCtxQuery` | `kReplySlot` | `:160` |

(I derived the opcodes by position; the brief should re-derive rather than trust me on the exact
numbers — the *set* of ten rows is what matters and is exact.)

Generated artefacts for each: `PipeTables.inc:18,22,25,26,37,38,75,78,81,92`;
`PipeThunks.inc:20,36,48,52,72,76,224,236,248,292`.

## 2.3 How the answer gets back in monolith today — **it does not use the reply slot at all**

`MGPReplySlot` has **zero producers and zero consumers** outside the generated tables and the field-list
macro. The two reply-slot calls that are actually implemented today return their answers as **ordinary
C++ return values from the free-function applier**:

- `void* MGPipeApplyMapPersistent(const MGPHandleOnly& handle, Uint64 size, const void* seedBytes);`
  — `PipeApply.h:917`, defined `PipeApply.cpp:1893-1921`. Returns the coherent host pointer, **or
  `nullptr` for a DECLINE, which is a real answer** (`PipeApply.cpp:1913-1919`, and the same sentence at
  `PipeApply.h:914-916`: *"a real answer and the reason the call is `kOptional` as well as
  `kReplySlot`"*). Client caller: `MGPipeEmitMapPersistent`, `MG_Impl/Pipe/PipeFill.cpp:772-789`
  (returns the pointer straight up the stack).
- `void MGPipeApplyResourceReadback(const MGPReadback& record);` — `PipeApply.h:908`. Returns nothing;
  **the bytes come back through the reverse channel instead**, via `OnBufferWriteback`
  (`PipeApply.h:905-907`, and the producer at `MG_Backend/DirectGLES/Managers.cpp:2135-2139`). The
  client emitter `MGPipeEmitResourceReadback` (`PipeFill.cpp:754-768`) says so at `:764-766`.

The other eight reply-slot calls (`GetCaps`, `FenceStatus`, `FenceWait`, `QueryAvailable`,
`QueryResult`, `GetTextureImage`, `ReadPixels`, `QueryTimestamp`) have **no applier at all** — they are
still served by `MG_Backend::gBackendFunctionsTable.GL.*` directly from `MG_Impl`. Example:
`ReadPixels_Backend` at `MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.cpp:3093-3096` is
`MGP_FILL(ReadPixels); gBackendFunctionsTable.GL.ReadPixels(...)` — a synchronous call with an out
pointer into the application's memory.

**Consequence for the brief.** P5 is the first phase that has to give `MGPReplySlot` a meaning, and it
has to answer three questions that nothing in the tree answers today:

1. **Who allocates the `Id`?** The slot pool is server-owned (`SegmentKind.Reply`), but the *record*
   carrying the request is client-produced. Most likely the client mints a monotonic `Id` and the server
   writes into `SEG_REPLY[Id % slots]`; nothing in the tree says so. **Guess.**
2. **How does the client know the answer landed?** `ARCHITECTURE.md` §11.7 names `EvReadbackDone`,
   `EvQueryResult`, `EvFenceSignaled` as `SEG_EVENT` records — i.e. the reply slot carries the *payload*
   and the event ring carries the *completion*. No code exists for either.
3. **`MapPersistent`'s decline** must remain expressible: the answer is a pointer **or `nullptr`**, and
   `nullptr` means "declined", not "failed" (`PipeApply.cpp:1913-1916`). A reply-slot encoding that
   cannot distinguish "not yet" from "declined" will hang `MGPipeEmitMapPersistent`, which returns its
   value synchronously into `BufferObject` storage definition. **`MapPersistent` is the one reply-slot
   call whose client caller genuinely cannot proceed without the answer**, which is why
   `ARCHITECTURE.md` §13.1 lists `MapPersistent` under "不可避免" (T1 tier, once per storage definition)
   and why `MOBILEGL_IPC_ADOPT_TIER=2` (decline) is the permanent correct fallback (§12).

**Also note:** `kNeedsAck` is a *different* mechanism from `kReplySlot`. Only `ResourceRespecify`
carries `kNeedsAck` (`PipeCalls.def:82`), and only when the per-record predicate
`MGPipeResourceRespecifyNeedsAck(desc)` says so — `MG_Pipe/MGPipeTypes.h:680`, pinned by
`MG_Test/Pipe/PipeCatalogueTest.cpp:552-585`
(`ResourceRespecifyAcksOnlyImmutableStorage`). `ARCHITECTURE.md:298` says in as many words: **"monolith
下 ack 是 `((void)0)`… P5 把门铃接到这个谓词上"**, and P4a narrowed the predicate to *buffer* targets
(D-A2). So P5 owns: the doorbell on that predicate, and the reply-slot mechanism, and they are separate.

---

# PART 3 — Blocking `read_pixels`

## 3.1 The current path, end to end

```
glReadPixels  (MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.cpp:3099)
  └─ ReadPixels_State(...)                      :2924-3091   pure frontend validation
  └─ ReadPixels_Backend(...)                    :3093-3096
       ├─ MGP_FILL(ReadPixels)                  :3094        fills the kReadback class (18 fields)
       └─ gBackendFunctionsTable.GL.ReadPixels  :3095        -> the backend, synchronous
```
`glReadnPixels` is the same two calls with a size bound (`GL_Framebuffer.cpp:3135-3152`; validation at
`:3143`, backend at `:3151`).

The DirectGLES implementation is `DirectGLES.cpp:10851-11000`:

- `:10858-10859` picks native vs. converted readback;
- `:10926-10929` depth/stencil special paths;
- **`:10936-10937`** `MGB_CTX->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject()` — the PACK
  PBO, read *from the context* inside the backend;
- `:10952` `ScopedPixelPackBuffer packBufferBinding(packBufferId)` — the driver binding is scoped so it
  returns to 0 on every exit;
- `:10955` `glReadPixels`;
- **`:10982-10997` — the blocking half.** When a PBO was used, the backend immediately
  `glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, size, GL_MAP_READ_BIT)` (`:10985-10986`),
  `pixelPackBufferObject->WritebackFromBackend({pboMappedPtr, size}, 0)` (`:10989`),
  `BufferImpl::BumpBufferMutationEpoch()` (`:10991`), `glUnmapBuffer` (`:10993`).

There are **five** `MGB_CTX->GetBufferBindingSlot(BufferTarget::PixelPack)` reads in DirectGLES:
`:9407` (inside `StoreReadbackRowsToClient`, `DirectGLES.cpp:9397` — the **per-scanline** writeback loop,
`:9418`), `:10389` (`ReadPixelsViaFormatConversion`, `:10371`), `:10626`
(`GetTexImageViaShadowConversion`, `:10604`), `:10936` (`ReadPixels`), `:11362` (`GetTextureImage`).
DirectVulkan has its own at `Renderer/VulkanRenderer.cpp:10988-11004` (`ReadDepthStencilPixels`, also a
per-row `WritebackFromBackend` loop at `:11000`). DirectGLES `Utils.cpp:2343` is a sixth per-row
writeback.

`MGPReadbackInfo` (`MGPipeTypes.h:1197-1205`, 64 B) is the payload already minted for this:
`MGPipeHandle Res` — **`:1198` says "null for read_pixels: the bound read surface answers"** — plus
`MGPBox Box`, `Format`, `Type`, `Target`, `Level`, `DstOffset`, `DstSize`.

## 3.2 What the split version must block on

**It must block on exactly one thing: the pixels, and only when the destination is client memory.**
`ARCHITECTURE.md` §13.1 puts it in the "不可避免" list in those words: *"`glReadPixels` → 客户内存（像素进
`SEG_REPLY`，逐行写回循环留在 server 内按操作级批成一段）"*.

Concretely, the blocking version is:
1. publish the command stream up to and including the `ReadPixels` record (doorbell — `ARCHITECTURE.md`
   §11.6 lists `kNeedsAck` and the polling entry points as explicit doorbell points; a blocking readback
   is the same shape);
2. wait on the reply slot / `EvReadbackDone`;
3. copy the pixels out of `SEG_REPLY` into the application's pointer.

Two things it must **NOT** block on, both stated in `ARCHITECTURE.md` §13.1's zero-round-trip list:

- **`glReadPixels` into a PACK PBO must NOT block.** It becomes fire-and-forget: emit the record, and
  **the client marks the PBO GPU-written itself** ("`glReadPixels` → pack PBO（fire-and-forget +
  client 侧 `MarkGpuWritten`，严格优于 monolith 的无条件停等）"). This is the *only* place in P5 where the
  split path is strictly **better** than monolith — today `DirectGLES.cpp:10982-10997` stalls on
  `glMapBufferRange(READ)` for every PBO readback, and the whole point of a PBO is not to. The
  client-side conservative `MarkGpuWritten` P5's row names is exactly this: the existing client callback
  `MGPipeClientOnGpuWritten` (`MG_Impl/Pipe/ResourceTracker.h:579-603`) already sets
  `m_gpuWritePending` + `m_hasDefinedContent` via `BufferObject::MarkGpuWritten()`
  (`MG_State/GLState/BufferState/BufferObject.cpp:364`), and the client just has to *call it itself*
  at the emission point instead of waiting for the server to announce. The contract note at
  `Managers.cpp:2533-2538` is the one to obey: **both** flags, or the next `ResourceRespecify` carries
  `HasDefinedContent = 0` and the shader-written contents are dropped silently.
- **The per-scanline writeback loop must NOT become per-scanline IPC.** `MGPipeCallbacks.h:32`'s
  `OnBufferWriteback` comment and `ARCHITECTURE.md` §8.1 both say it: *"按操作级批处理（今天两处逐行循环绝不能
  变成每扫描线一次 IPC）"*. The two loops are `DirectGLES.cpp:9397-9424`
  (`StoreReadbackRowsToClient`, writeback at `:9418`) and `Utils.cpp:2337-2351`; DirectVulkan's third is
  `VulkanRenderer.cpp:10996-11004`. Under split those loops stay **inside the server** and produce one
  `SEG_REPLY` region.
- It must also **not** block on `glGetError` (always local), on `glFinish`/`glFlush` (no-ops), or on the
  framebuffer-completeness check — `ReadPixels_State` (`GL_Framebuffer.cpp:2924-3091`) is 100% frontend
  and answers locally, including `CheckCompleteness()` (`:2963`).

## 3.3 The seam that will bite

`MGP_FILL(ReadPixels)` maps to fill class `kReadback` (`MG_Pipe/FillPoints.def:115`), whose field set is
`FillPoints.def:293-313` — **18 fields**, and the first three are
`GetPixelStoreParameters`, **`GetBufferBindingSlot`**, `GetFramebufferBindingSlot`. `GetBufferBindingSlot`
returns `BindingSlot<BufferObject>&` (`MG_Backend/MGPipe/PipeInputs.h:485-492`), i.e. **a pointer into
client memory** — `PipeFill.cpp:98-105` fills it as `dst.m_bufferBindingSlot[target] =
&ctx.GetBufferBindingSlot(target)`. That is fine in `inproc` (one address space) and **impossible** in
`spawn`. Its declared coverage owner is `SetIndirectBuffers` (`MG_Pipe/Coverage.def:70`,
`generated/PipeCoverage.inc:40`), and `Coverage.def:184` notes it is polymorphic over a target set P3a
only partly covers.

So: **P5's `inproc` milestone can ship with the backend still reading the PACK PBO out of `PipeInputs`;
P6's `spawn` cannot.** Two readings of the P5 charter: (i) P5 only has to make `inproc` work, so the PBO
binding can stay a `PipeInputs` pointer read and `MGPReadbackInfo.Res`/`DstOffset`/`DstSize` stay unused;
(ii) P5 should already route the PBO destination through `MGPReadbackInfo.Res` + `DstOffset` because
`MGPReadbackInfo:1198` was minted for it and P6 is only 5 days later. **Evidence that would settle it:**
whether `DirectGLES.Split.ClearThenReadPixels` (the P5 gate) uses a PBO — it does **not** (see 3.4), so
(i) passes the gate. **Recommendation: (ii) anyway**, because the P5 exit gate explicitly includes
"未迁移字段读 = `Fatal{UnmigratedPipeInput}`", and under `MOBILEGL_BUILD_DISAGGREGATED` the poison is
armed (`MG_Backend/MGPipe/PipeInputs.h:21-26`) — a split build that reads a client pointer through
`PipeInputs` is precisely what that gate exists to catch, and leaving it for P6 means P5's own gate is
only half-armed.

## 3.4 The P5 gate targets, as they exist today

`DirectGLES.Split.ClearThenReadPixels` — the scenario file exists:
`MG_IntegrationTest/Scenarios/ClearThenReadPixelsScenario.cpp`, five cases at `:80`, `:143`, `:191`,
`:241`, `:308`. All five read the **default framebuffer into client memory** (no PBO). It is registered
in `MG_IntegrationTest/CMakeLists.txt:76`; the `TEST_PREFIX` mechanism that makes lane names
(`DirectGLES.`, `DirectGLES.AsyncOn.`, `DirectGLES.HandleRecycle.Handles.`, …) is at
`CMakeLists.txt:739`, `:868`, `:1157` etc. — **P5 adds a `DirectGLES.Split.` prefixed lane the same way.**

`DirectGLES.Split.Triangle` — **there is no Triangle scenario in the tree.** Full scenario list (85
files) checked; nothing matching `Triangle`. P5 must write it.

`PersistentCoherentMapScenario` — **does not exist either.** The nearest existing relatives are
`StorageBufferRegrowScenario.cpp` (`:255`,
`NStorageDefinitionsCostNMapPersistentRoundtripsNotOnePerDraw`) and `LargeArenaAdoptionScenario.cpp`
(`:450`, `AnAdoptionCostsExactlyOneMapPersistentRoundtrip`) — the two `map-persistent-roundtrips` gates
(`ARCHITECTURE.md` §12). P5 must write `PersistentCoherentMapScenario` from the §12 recipe (map
`PERSISTENT|WRITE|COHERENT`, write, **no other GL call**, draw, readback-verify).

`MOBILEGL_TRANSPORT` — **no parsing exists.** `grep` over `Config.h`, `MG_Util/` finds no `Transport`
and no `MOBILEGL_IPC_*`; the `MOBILEGL_PIPE_*` knobs are `Config.h:320-415`. `ARCHITECTURE.md` §15.1
requires `MG_Config::Transport` to exist and to be force-set to `Monolith` in `mobilegl_server_main`.

`add_trace_replay_test` is `tools/trace_replay/CMakeLists.txt:311`, with
`add_trace_replay_test_for_backends` at `:384-387`. There is no `SPLIT` argument and no
`TRACE_TRANSPORT`; P5 adds both.

---

# PART 4 — `MGPipeCallbacks`: ten reverse callbacks + one forward terminator

## 4.1 The table

`MG_Pipe/MGPipeCallbacks.h:27-52`. `kMGPipeCallbackCount = 10` at `:56`, asserted at `:57-58` ("an
eleventh cannot be added without touching the transport's reverse-channel record table"), pinned by
`MG_Test/Pipe/PipeCatalogueTest.cpp:684`. The single global instance `gMGPipeCallbacks` is
null-initialised at `MGPipeCallbacks.h:62`.

| # | callback | decl | producers today | consumers today | needed by P5's reduced path? |
|---|---|---|---|---|---|
| 1 | `OnGlError(code)` | `:29` | **none** (the 6 sites still call `MGB_CTX->RecordError`, forwarded) | none | **no** for Clear/Triangle/ReadPixels; **yes** if the gate asserts `glGetError()==0` |
| 2 | `OnGpuWritten(res, rangeCount, ranges[])` | `:31` | `MG_Backend/DirectGLES/Managers.cpp:2516-2539` (`MarkBufferGpuWritten`, push arm only) | `MGPipeClientOnGpuWritten`, `MG_Impl/Pipe/ResourceTracker.h:579-603` | **yes** — and P5 *narrows* it (client-side conservative set) |
| 3 | `OnBufferWriteback(res, offset, bytes)` | `:32` | `Managers.cpp:2135-2139` (`Ops_H_Readback`) | `MGPipeClientOnBufferWriteback`, `ResourceTracker.h:553-572` | **yes** for `resource_readback`; **not** for the two gate scenarios |
| 4 | `OnTextureWriteback(res, box, bytes)` | `:33` | **none** | none | no (CPU-fallback mip generation only) |
| 5 | `OnTexturePullRequest(res, target, firstLevel, levelCount, pullSerial)` | `:35-36` | **none** — the site is only *named*, `Managers.cpp:5334` raises `MGPipeUnmigratedEmulation("texture-remint-pull")` | none | **no** — see Part 5 |
| 6 | `OnMipLevelsGenerated(res, base, count)` | `:39` | **none** (`DirectGLES.cpp:8051` raises `MGPipeUnmigratedEmulation("generate-mipmap-storage")` instead) | none | no |
| 7 | `OnSurfaceChanged(info)` | `:43` | **none** — the layering inversion is still live: `DirectGLES.cpp:11467` and `SwapchainObject.cpp:276-295` write `MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo` directly | none | **yes** — nothing renders without a default FB |
| 8 | `OnCapsInvalidated()` | `:45` | **none** — `BackendObject_DirectVulkan.cpp:390`, `:788` call `MGB_CTX->InvalidateCompileEnv()` instead | none | **probably yes** on DirectVulkan; see 1.5(1) |
| 9 | `OnLog(level, text)` | `:47` | **none** | none | **yes if the server is a second process** (P6); for `inproc` the logger is shared, so **no** |
| 10 | `OnXfbScatterReady(scratch, packedStride, vertices)` | `:50` | **none** | none | no |
| — | `ResourceSubDataComplete` (forward terminator, **not** in this struct) | `PipeCalls.def:136`, payload `MGPSubDataComplete` `MGPipeTypes.h:1135-1140` (24 B) | **none** — `PipeCatalogueTest.cpp:953` says in as many words that the terminator is a later phase's and "P4a must not build half a terminator" | none | no |

So: **2 of 10 wired, 8 stubs.** Installation is `MGPipeInstallClientResourceCallbacks()`,
`ResourceTracker.h:605-613`, which installs #2 and #3 **only if the entry is still null** — the comment
at `:605-607` states the ownership rule: *"the backend installs the rest of the table and these two
answer for it."* Under split that rule inverts (the server has no client pointers at all), and the brief
should say so explicitly.

## 4.2 What P5's reduced path actually needs — and the ordering rule

`ARCHITECTURE.md` §8.2 is the one non-negotiable: **the reverse channel needs the same ordering guarantee
as the forward one.** The concrete instance is spelled out at `Managers.cpp:2120-2136`: writeback →
unmap → serial stamp → **epoch bump strictly last** ("The bump must NEVER move earlier"). The
mirror-image note on the client side is `ResourceTracker.h:550-552`.

For P5's milestone ("first inproc IPC frame, reduced path") the minimum is **#2, #3, #7**, plus **#1** if
the gate checks `glGetError`, plus **#8** on the DirectVulkan lane. #4, #5, #6, #10 and the terminator
are P7/P9 (texture and XFB families). #9 is P6.

## 4.3 The group the split will break first — `PipeInputs`'s "forwarded to the live context"

`MG_Backend/MGPipe/PipeInputs.h:558` opens a group of **six accessors + `RecordError`** that are *not*
snapshot fields: they forward to `MG_State::pGLContext` through
`MG_Impl/Pipe/PipeFill.cpp`'s `LiveContext()` (`PipeFill.cpp:313`,
`GLContext* LiveContext() { return MG_State::pGLContext.get(); }`). **In the server process
`LiveContext()` is null**, and every one of them then silently degrades:

| accessor | decl | impl | backend callers | split answer |
|---|---|---|---|---|
| `GetBufferBindingPointCount(target)` | `PipeInputs.h:569` | `PipeFill.cpp:1553-1557` | `DirectGLES.cpp:579` (`SyncAtomicCounterBuffers`), `UniformManager.cpp:1190`, `:2014` | server-side binding tables (P7/P9) |
| `GetProgramObject(index)` | `:570` | `PipeFill.cpp:1559-1562` | `DirectGLES.cpp:5810`, `:9203`, `DirectVulkan.cpp:274` | handle tables |
| `GetTextureObject(index)` | `:571` | `PipeFill.cpp:1564-1566` | `VkTextureManager.cpp:810` | handle tables |
| `HasOpenTransformFeedbackSpan(lifetimeId)` | `:572` | `PipeFill.cpp:1569-1572` | `VulkanRenderer.cpp:11527` | handle tables |
| `InvalidateCompileEnv()` | `:573` | `PipeFill.cpp:1574-1576` | `BackendObject_DirectVulkan.cpp:390`, `:788` | **→ `OnCapsInvalidated` (#8)** |
| `ValidateProgramName(index)` | `:574` | `PipeFill.cpp:1578-1581` | `DirectGLES.cpp:5805`, `:9202`, `DirectVulkan.cpp:271` | handle tables |
| `RecordError(code, info)` | `:577` | `PipeFill.cpp:1583-1590` | `Managers.cpp:12812`, `DirectGLES.cpp:8123`, `DirectVulkan.cpp:717`, `VulkanRenderer.cpp:1339`, `:1343`, `:1399` | **→ `OnGlError` (#1)** |

`PipeInputs.h:563-568` states that this group deliberately carries **no** `MGP_INPUT_CHECK`, i.e. **no
`Fatal{UnmigratedPipeInput}`** — *"a forward is a live call, not a stored value, and
`InvalidateCompileEnv` is reached from backend initialisation before any verb has filled, where a check
would be `Fatal{...@<none>}` on every start."* **This is the hole in P5's exit gate.** The gate says "a
read of an unmigrated field = `Fatal{UnmigratedPipeInput}`", but these seven are the very reads a split
build most needs to catch and they are exempt by construction. `RecordError` degrades to
`MGLOG_E_ONCE` + drop (`PipeFill.cpp:1585-1588`); the other six degrade to null/false/zero **silently**.

**Recommendation for the brief:** P5 should add a split-only arm to those seven — under
`MOBILEGL_BUILD_DISAGGREGATED` with no live context, `Fatal{UnmigratedPipeInput}` (or the callback where
one exists), not a silent null. Cheap (7 sites), and it is the difference between P5's gate being armed
and being decorative. The precedent for a split-only Fatal arm is `MGPipeUnmigratedEmulation`
(`PipeApply.h:1068`, monolith body `PipeApply.cpp:2820` = `(void)name;`).

**Good news for the reduced path:** of the seven, a Clear + Triangle + ReadPixels sequence on DirectGLES
hits **none**. `SyncAtomicCounterBuffers` (`DirectGLES.cpp:574-620`) only runs when a program declares
atomic counters; `MarkShaderStorageBuffersGpuWritten` (`DirectGLES.cpp:562-573`) uses
`GetTouchedBufferBindingPointCount` / `GetBufferBindingPoint`, which are **migrated** fields, not
forwarded ones. `ValidateProgramName`/`GetProgramObject` are on `glGetProgramBinary`-shaped paths
(`DirectGLES.cpp:5805`, `:9202`), not on draw.

## 4.4 The `Fatal{UnmigratedPipeInput}` machinery P5 inherits

- Armed by `MOBILEGL_PIPE_POISON`, `PipeInputs.h:21-26` — **and `MOBILEGL_BUILD_DISAGGREGATED` is one of
  the three arming conditions**, so a split build has the poison on by construction.
- The check macro `MGP_INPUT_CHECK` is `PipeInputs.h:44-53`; the abort is
  `MGPipeInputPoisonFatalForVerb` (`PipeInputs.h:31`), `MGLOG_F` + `std::abort()`, **live at every log
  level** (`:29-31`).
- Existing gates that assert the message shape: `MG_IntegrationTest/Scenarios/PoisonOmissionScenario.cpp`
  (prefix at `:88`, pair assertion at `:341`) and `PipeVerifyArmingScenario.cpp:78`; unit side
  `MG_Test/Pipe/PipeInputsTest.cpp:93`, `:292-296`.
- The knob that injects an omission is `MOBILEGL_PIPE_POISON_OMIT=<Verb>:<FieldName>` (`Config.h:370`).

---

# PART 5 — D-B6, the one new stall class (server-initiated texture remint pull)

## 5.1 The design, summarised

`ARCHITECTURE.md` §8.4 (`docs/Disaggregated/ARCHITECTURE.md:303-312`). The server keeps no texels
(§10.1). Three things make it ask for a level it was already sent:
`RequireImageBindableStorage`'s re-dirty, a whole-format re-creation, and a texture-view source remint.
Four mitigations run at once:

1. **Prevention (the main one):** the client stamps `everImageBound` and carries
   `MGPResourceDesc::ImageBindableHint` on every `ResourceCreate`/`ResourceRespecify`, so
   image-bindable storage is allocated up front.
2. **The pull is async:** the server emits `OnTexturePullRequest` and marks the twin not-ready; the
   client re-sends on its next publish. **What blocks is `mgl-srv-apply`, never the app thread.**
3. **Bounded retention, default off:** `MOBILEGL_PIPE_TEXEL_RETAIN_MB` default 0 (`Config.h:397`) —
   `MipmapStorage` holds a full CPU shadow per level, so a pull can always be served; the cache buys
   latency, not correctness.
4. **Explicit terminator:** `ResourceSubDataComplete(res, target, firstLevel, levelCount, pullSerial)`,
   **may carry zero regions** (a level whose content came only from rendering, from a copy
   `CanMirrorCopyImageShadow` refused, or from GPU-side mip generation — the client has no bytes). Zero
   regions means the server proceeds with allocated-but-empty storage and logs `MGLOG_W`.
   **Without the terminator the apply thread parks forever.**

Gate: `TextureRemintPullScenario` (must include an unservable case, and must be **red** before the
terminator lands). Pull counts are published per trace case.

## 5.2 What is actually in the tree

- The site is **named, not built**: `MG_Backend/DirectGLES/Managers.cpp:5275-5345`. The comment at
  `:5275-5284` says it exactly: *"P4a supplies mitigation 1 — the prevention half… and NAMES the site.
  Mitigations 2-4 (the async pull, the bounded retention and the `ResourceSubDataComplete` terminator)
  and `TextureRemintPullScenario` are P9's, and P4a must not build half a terminator."*
- The marker is `MG_Pipe::MGPipeUnmigratedEmulation("texture-remint-pull")` at `Managers.cpp:5334`,
  raised **once per transition** and **only where a level is actually replayed** (review N-4,
  `Managers.cpp:5289-5297`). Monolith body is a no-op (`PipeApply.cpp:2820`).
- The counter is `PipeStats::CallClass::TextureRemintPulls` (`MG_Util/Metrics/PipeStats.h:166`,
  documented `:161-166`), incremented at `Managers.cpp:5339` **only when `hadBackendStorage`**; name
  `"tex-remint-pulls"` (`PipeStats.cpp:182`), summary token `trp=` (`PipeStats.cpp:459`). Test-side
  peek: `MG_IntegrationTest/Harness/P4aFinalFixPeek.{h,cpp}` (`P4aFinalFixPeek.h:35`,
  `.cpp:50-55`), used by `Scenarios/P4aFinalFixScenario.cpp:518`, `:579-582`, `:602`.
- There are **three sibling unmigrated-emulation markers** P5 will meet on the same path:
  `"generate-mipmap-storage"` (`DirectGLES.cpp:8051`), `"generate-mipmap-cpu-fallback"` (`:8702`),
  `"copy-image-shadow-mirror"` (`:8997`), `"get-tex-image-shadow"` (`:10623`) — four, in fact. **All four
  are no-ops today and all four become split-only Fatals or callbacks.** `"get-tex-image-shadow"` is the
  one that touches P5's readback subject.

## 5.3 The measured rate, and the answer for P5

`ROADMAP.md:85` (open question 2) is **answered**, by P4a:

> **P4a 已答：可以忽略，保留 LRU 维持默认 0。** … 在 79 例 retrace × 两后端、`MOBILEGL_PIPE_STATS_PERIOD=60`、
> push 构建（`8c458cd5`）下：**780 个统计窗口里总共 2 次**，分布在 **2 个用例**
> (`iris-photon-v1.3b-in-world`、`iris-derivative-main-d24.4.14-in-world`，都只在 DirectGLES 侧，各 1 次)，
> 其余 77 例恒 0。

Corroborated in `~/w7/notes/p4a/INTEGRATOR-DECISIONS.md:770` (the final-fix round: *"`tex-remint-pulls`
= `ROADMAP:85` 的数字"*) and `:726` (the review finding that made the counter real —
before it, the hint was always 0 and the number was unmeasurable). `ImageBindableHint` got its first real
producers in P4a (`glBindImageTexture` and sampler-view resolution each set a bit), so **2 is the residue
after prevention is already working**.

**Can P5's reduced path avoid it? Yes, and it does so by construction, three times over:**

1. The path is entered only from `RequireImageBindableStorage` — i.e. only a texture that gets
   `glBindImageTexture`'d after its storage was already minted non-image-bindable. P5's two gate
   scenarios (`ClearThenReadPixels`, a new `Triangle`) use no image loads/stores at all.
2. Both fixtures where it fired are Iris shader-pack in-world traces; the P5 SSIM gate is **OpenRA**,
   not Iris (`ROADMAP.md:21`).
3. Even when it fires, monolith behaviour is unchanged — `Managers.cpp:5286-5288`: *"In monolith the code
   below keeps running exactly as it does today: the Fatal is a split-only arm."*

**Recommendation:** P5 should (a) leave mitigations 2-4 to P9, exactly as `Managers.cpp:5281-5284` and
`PipeCatalogueTest.cpp:953` instruct, and (b) make the **split-only arm of `MGPipeUnmigratedEmulation`
a `Fatal`** so that if a P5 trace does reach it, the phase finds out loudly instead of rendering a wrong
mip. That single edit (`PipeApply.cpp:2820` gains a `#if MOBILEGL_BUILD_DISAGGREGATED` arm) is also what
half-arms the P5 exit gate "a read of an unmigrated field = `Fatal{UnmigratedPipeInput}`" for the
*emulation* family, complementing the `PipeInputs` half in 4.3.

---

# PART 6 — Consolidated list of things P5 must invent (nothing exists today)

| item | evidence it does not exist |
|---|---|
| `MGPCaps` producer and consumer | zero call sites; `gMGPipeScreen` read only by `PipeCatalogueTest.cpp:107` |
| `BackendObject_Remote` | `grep BackendObject_Remote` over the tree: 0 hits outside `ROADMAP.md:21` |
| `MG_Remote/Client/*` (emitter tables) | `MG_Remote/` contains only `Protocol/` (3 files) and `Transport/` (12 files) |
| `MG_Remote/Server/*` (`PipeApplier`, `ServerLoop`, `PipeObjectTables`, `IndexHostMirror`) | same |
| `MGPReplySlot` semantics | zero producers/consumers |
| `MG_Config::Transport` / `MOBILEGL_TRANSPORT` / `MOBILEGL_IPC_*` | zero hits in `Config.h`, `MG_Util/` |
| `SEG_REPLY` / `SEG_EVENT` runtime code | named in `ShmSegment.h:9`, `Ring.h:80` (`eventRingFull`), `protocol.fbs:40-41`; no implementation |
| `DirectGLES.Split.Triangle` | no `Triangle*` scenario in the 85-file scenario list |
| `PersistentCoherentMapScenario` | not in the scenario list |
| `add_trace_replay_test(... SPLIT)` / `-DTRACE_TRANSPORT=` | `tools/trace_replay/CMakeLists.txt:311`, `:384-387` have neither |
| a round-trip counter | `PipeStats::CallClass` (`PipeStats.h:100-168`) has `MapPersistentRoundtrips` and `TextureRemintPulls` but no general round-trip class. **Any new class must be inside the `#if MOBILEGL_PIPE_PUSH` block (`PipeStats.h:118-167`)** — G1 forbids resizing the pull build's counter arrays. |
| peak-RSS recording for both roles | no harness; `tools/device_bench/bench.sh` records frame times (`ARCHITECTURE.md` §13.2-④) |

---

# Appendix — three claims I could not verify, flagged as guesses

1. **Opcode numbers in Part 2's table** are derived by counting `MGP_CALL_LIST` positions, not read off
   a generated artefact. The *set* of ten `kReplySlot` rows and their `PipeCalls.def` line numbers are
   exact; the opcodes are a **guess** and should be re-derived from
   `MG_Pipe/generated/PipeWire.inc` before anyone pins a wire value.
2. **`CallMask` vs `CapsSnapshot.tableSlotMask`** — argued in 1.4(6); my reading is that they are one
   field and the schema comment is stale, but I did not find a line that settles it.
3. **Startup ordering of `LogBackendInfo()`** relative to the first `eglMakeCurrent` in the real
   launcher — argued in 1.2; I verified it in library code only, not in FCL.
