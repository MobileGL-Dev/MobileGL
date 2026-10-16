---

## 4. (b) Verb-boundary entry points, and which accessors are reachable from each

The verb boundary is `struct GLFunctionsTable` (`MobileGL/MG_Backend/BackendObject.h:117-289`) —
**69 function pointers plus one plain `Bool` policy flag** (`PrefersCpuXfbPrimitiveAccounting`,
`BackendObject.h:265`) — wrapped by `struct GlobalBackendFunctionsTable`
(`BackendObject.h:293-300`), which adds `Present` (`:295`) and `SetSwapInterval` (`:298`).
**72 entries total.** Both tables are filled once, lazily, in
`BackendObject_DirectGLES::GetBackendFunctions()` (`DirectGLES/BackendObject_DirectGLES.cpp:1230-1325`)
and `BackendObject_DirectVulkan::GetBackendFunctions()` (`DirectVulkan/BackendObject_DirectVulkan.cpp:694-777`).

### 4.1 Fill-site differences between the two backends (a per-verb fill/validate must cope with both)

| entry | DirectGLES | DirectVulkan |
|---|---|---|
| `GetTextureImage` | **not filled** | `BackendObject_DirectVulkan.cpp:736` |
| `PatchParameteri` | `BackendObject_DirectGLES.cpp:1319` | **not filled** |
| `BeginTransformFeedback` / `End` / `Pause` / `Resume` / `Bind` / `Delete` | `:1320-1325` (all six, `XfbImpl::*`) | **none filled** — Magma drives capture from its own draw recording |
| `PrefersCpuXfbPrimitiveAccounting` | `= true` at `:1312` | left `false` |
| `SetSwapInterval` | `:1231` | **not filled** |
| `Present` | `:1230` | `:698` |
| timer-query group (`IsTimerQuerySupported`…`GetGpuTimestampNs`) | `:1292-1296` + `:1313-1315`, gated on `AreTimerQueriesSupported()` | `:754-761`, gated on `!MG_Config::Features.DisableTimerQuery` |

### 4.2 Verb → reachable accessors

Reachability was traced by following the helper call graph out of each entry point (caller greps
cited in §4.3). "direct" = the site's enclosing function is the entry point itself.

#### DirectGLES (Espryt)

| verb entry point (impl) | reachable accessors |
|---|---|
| `DrawArrays` (`DirectGLES.cpp:4564`) | direct: `GetBoundVertexArray`. Via `PrepareForDraw`: everything in the draw closure below. |
| `DrawElements`, `DrawElementsBaseVertex`, `DrawElementsInstanced*`, `DrawRangeElements*`, `DrawArraysInstanced*` (`DirectGLES.cpp:4556-4931`, 14 `PrepareForDraw` call sites) | the draw closure + `ResolveRestartSubstitution`/`ScopedRestartIndexSubstitution` (`IsCapabilityEnabled`×2, `GetPrimitiveRestartIndex`×2) + `BoundElementArrayBuffer` (`GetBoundVertexArray`) |
| `MultiDrawArrays` (`:4597`) | direct `GetBoundVertexArray`; + draw closure |
| `MultiDrawElementsIndirect` (`:4646`), `MultiDrawArraysIndirect` (`:4754`), `DrawElementsIndirect` (`:4956`), `DrawArraysIndirect` (`:5003`) | direct `GetBufferBindingSlot(DrawIndirect)`; + draw closure; + `ResolveIndirectCommandBytes` (`:261`) |
| `MultiDrawElementsIndirectCount` (`:4685`), `MultiDrawArraysIndirectCount` (`:4793`) | direct `GetBufferBindingSlot(DrawIndirect)` **and** `(Parameter)`; + draw closure |
| **the draw closure** = `PrepareForDraw` (`:2949`) | `GetBoundVertexArray` (`:2957`), `GetProgramForDraw` (`:2968`) → `SyncNeccessaryBuffers` (`:538`→`GetBufferBindingSlot`), `CaptureDrawTextureSyncKeys` (`:1513`→`GetTextureContextId`, `GetMaxTouchedTextureUnit`, `GetSamplingResolutionGeneration`), `SyncNeccessaryTextures` (`:1527`→`GetTextureUnitObject`, fb-slot fast cache), `SyncCurrentVertexAttributeValues` (`:1237`→`GetBoundVertexArray`, `GetCurrentVertexAttribute`), `SyncRenderState` (`:2022`→`GetRenderStateParametersVersion`, `GetRenderStateParameters`, `GetViewport`, `IsCapabilityEnabled`), `SyncCurrentProgram` (`:2722`→`GetPatchVertices`, `GetPatchDefaultOuterLevel`, `GetPatchDefaultInnerLevel`, fb-slot fast cache), `BindCurrentProgramWithResources` (`:3371`→`GetBufferBindingPoint(Uniform)`, `GetTextureUnitObject`), `BindCurrentTextures` (`:3299/:3361`→`GetProgramForDraw`), `ResolveAndBindUnitTextures` (`:3023`→`GetTextureUnitObject`), `BindCurrentUnitSamplers` (`:3200`→`GetTextureUnitObject`), `SyncBufferBindingPoints` (`:351`→`GetTouchedBufferBindingPointCount`, `GetBufferBindingPoint`), `MarkShaderStorageBuffersGpuWritten` (`:460`), `SyncAtomicCounterBuffers` (`:470`), `SyncBoundBuffer` (`:514`), `BeginViewportRoutingPasses` (`:3792`→`IsTransformFeedbackActive`, `IsCapabilityEnabled`, `GetRenderStateParameters`), `XfbImpl::StartPendingTransformFeedback` (`:981`→`GetTransformFeedbackProgram`, `GetBufferBindingPoint(TransformFeedback)`), `CurrentProgramMayNeedPerSubDrawBuiltins` (`:3711`), `GetCurrentBackendProgram` (`:3661`) |
| `DispatchCompute` (`:6983`), `DispatchComputeIndirect` (`:6991`) | `PrepareForCompute` (`:4054`) → `GetProgramForDispatch`, plus `CaptureDrawTextureSyncKeys`, `SyncNeccessaryTextures`, `BindCurrentProgramWithResources`, `SyncBufferBindingPoints`, `MarkShaderStorageBuffersGpuWritten`, `SyncBoundBuffer(DispatchIndirect)`, `MarkWritableImageBufferTexturesGpuWritten` (`:1816`) |
| `Clear` (`:4111`) | direct `GetRenderStateParameters` (`:4156`), `GetFramebufferBindingSlot(Draw)` (`:4215`); + `SyncRenderState(forColorClear)` (`:4121`), `SyncNeccessaryTextures` (`:4115`) |
| `ClearBufferfv/iv/uiv/fi` (`:7424`, `:7437`, `:7453`, `:7469`, `:7506`) | `SyncNeccessaryTextures` + `SyncRenderState(forColorClear)`; no direct pulls |
| `ClearNamedFramebuffer*` (`DirectGLES.h:60-67`) | same, plus the `SharedPtr<FramebufferObject>` handle argument |
| `BlitFramebuffer` (`:6015`) | direct `GetFramebufferBindingSlot(Read)` + `(Draw)` (`:6044-6045`); + `SyncCurrentFBO`, `SyncRenderState` (`:6029`), `BindCurrentFBO`×2, `SyncNeccessaryTextures` (`:6021`), `ResolveThenBlit` (`:5093`→`IsTransformFeedbackActive`, `IsTransformFeedbackPaused`) |
| `BlitNamedFramebuffer` (`:6055`) | same + two FBO handle arguments |
| `CopyTexImage2D` (`:6676`) | `GetActiveTextureUnit`, `GetTextureUnitObject`; + `SyncNeccessaryTextures` (`:6683`), `SyncRenderState` (`:6691`) |
| `CopyTexSubImage2D` (`:6769`) | same pair at `:6794-6795`; + `:6777`, `:6785` |
| `CopyImageSubData` | no direct pulls; both endpoints arrive as `CopyImageEndpoint` handles |
| `GenerateMipmap` (`:6928`) | `GetActiveTextureUnit`, `GetTextureUnitObject` |
| `ReadPixels` (`:8999`) | `GetPixelStoreParameters` (`:9041`), `GetBufferBindingSlot(PixelPack)` (`:9084`); + `PackStateFromContext` (`:6184`), `StoreReadbackRowsToClient` (`:7552`), `ReadPixelsViaFormatConversion` (`:8526`), `Utils::StoreClientRows` (`Utils.cpp:2294`) |
| `GetTexImage` (`:9166`) | `GetActiveTextureUnit`+`GetTextureUnitObject` (`:9194-9197`), `GetPixelStoreParameters` (`:9420`), `GetBufferBindingSlot(PixelPack)` (`:9510`); + `GetTexImageViaShadowConversion` (`:8759`) |
| `ShaderStorageBlockBinding` (`:7355`) | `ValidateProgramName`, `GetProgramObject`; + `SyncNeccessaryTextures` (`:7376`), `SyncRenderState` (`:7378`) |
| `BindImageTexture` (→`TextureImpl::SyncImageTextureBinding`, `:7317`) | `GetImageTextureBinding` (`:1728`) |
| `BeginTransformFeedback` (`:969`) / `EndTransformFeedback` (`:1102`) | `EndTransformFeedback`→`ScatterCapturedRecords` (`:885`→`GetTransformFeedbackCapturedVertices`) |
| `PauseTransformFeedback` (`:1131`), `ResumeTransformFeedback` (`:1140`), `BindTransformFeedback` (`:1147`), `DeleteTransformFeedback` (`:1167`), `PatchParameteri` (`:6923`) | **no pulls** |
| `Present` (`:10550`), `SetSwapInterval` (`:9945`) | **no pulls** |
| sync / timer / occlusion / xfb-query group | **no pulls** in DirectGLES |
| `GetIntegeri_v` | **no pulls** |
| error reporting (`RecordGLError`, `:6365`) reachable from every entry above | `RecordError` |

Two more DirectGLES pull sites sit on paths reached from several verbs and are worth calling out
separately because they are in `Managers.cpp`, not `DirectGLES.cpp`:

| site | function | reached from |
|---|---|---|
| `Managers.cpp:5355` | `GetReadColorAttachment` | readback + `SyncReadBufferToBackend` (framebuffer sync) |
| `Managers.cpp:5460` | `IsFixedPointFallbackReadAttachment` | readback |
| `Managers.cpp:6230` | `BoundImageUnitFormat` | program link (image-unit format) |
| `Managers.cpp:7193/7201/7204` | `AttachPassthroughTessControlStage` | program link (`SyncToBackend`) |
| `Managers.cpp:8751` | `BackendSamplerObject::SyncToBackend` | sampler sync — `RecordError` |
| `Managers.cpp:3645/3646/3774/3775/3845/3846/4735/4736/4737` | `StampViewSyncKeys` / `SyncMipmapsToBackend` | texture upload, from every verb that calls `SyncNeccessaryTextures` |
| `MultiDraw.cpp:45/51/52/87/95` | `RestartSentinelFor`, `RestartActive`, `BoundDrawIndirectBufferId`, `BoundIndexBuffer` | the CPU multi-draw flattener, reached from `MultiDrawElements*` |

#### DirectVulkan (Magma)

Every `DirectVulkan.cpp` entry point opens with
`MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::<verb> called with null GL context")` — that
is 30 of the 58 non-arrow lines, and it is the cleanest possible marker of the verb boundary: each
of those asserts is exactly where a per-verb `PipeInputs` fill/validate would go.

| verb entry point (impl) | reachable accessors |
|---|---|
| `DrawArrays` (`DirectVulkan.cpp:815`), `DrawElements` (`:840`), `DrawElementsBaseVertex` (`:975`), `MultiDrawArrays` (`:863`), `MultiDrawElements` (`:968`) / `MultiDrawElementsImpl` (`:894`→`GetBoundVertexArray`), `MultiDrawElementsBaseVertex` (`:998`), `DrawElementsInstancedBaseVertexBaseInstance` (`:493`), `DrawArraysInstancedBaseInstance` (`:564`), `BuildClosedLineLoopIndices` (`:779`→`GetBoundVertexArray`) | the **draw closure** below |
| `DrawElementsIndirect` (`:521`), `DrawArraysIndirect` (`:580`), `MultiDrawArraysIndirect` (`:392`), `MultiDrawElementsIndirect` (`:387`) | direct `GetBufferBindingSlot(DrawIndirect)` (`:533`, `:586`, `:402`) + `ResolveIndirectCommandBytes` (`:277-278`); + draw closure |
| `MultiDrawArraysIndirectCount` (`:448`), `MultiDrawElementsIndirectCount` (`:442`) | direct `GetBufferBindingSlot(Parameter)` (`:465`); + `VulkanRenderer::MultiDrawElementsIndirectCount` (`:12074`→`GetBoundVertexArray`, `GetBufferBindingSlot(DrawIndirect)`, `(Parameter)`) |
| **the draw closure** = `VulkanRenderer::SetupDraw` (`:6420`) and its fast path `TrySetupDrawFastPath` (`:6034`) | `GetProgramForDraw` (`:6043`, `:6434`, `:6472`), `GetBoundVertexArray` (`:6105`, `:6471`), `GetFramebufferBindingSlot(Draw)` (`:6110`, `:6464`), `GetPipelineStateVersion` (`:6121`, `:6972`), `GetTextureBindGeneration` (`:6122`, `:6526`, `:6650`, `:6973`), `GetRenderStateParameters` (`:6130`), `GetSamplingResolutionGeneration` (`:6304`, `:6532`, `:6669`, `:6999`), `IsTransformFeedbackActive` (`:6091`, `:6512`), `IsCapabilityEnabled(DepthTest/StencilTest)` (`:6830-6831`); via `GetBaseTransformFlagsRaw` (`:6005`→`GetFramebufferBindingSlot`, and `GetShaderTransformFlags` `:2985`); via `ResolvePrimitiveRestartEnable` (`:4935`→`GetRenderStateParameters`); via `GetOrCreatePipeline` (`:4950`, **25 sites**: `GetPipelineStateVersion`, 6×`IsCapabilityEnabled`, `GetFramebufferBindingSlot`×2, `GetStencilState`×2, `GetPolygonModeFront`, `IsCapabilityEnabled(SampleShading)`, `GetMinSampleShadingValue`, `GetPatchVertices`, `GetCullFaceMode`, `GetDepthMask`, `GetDepthFunc`, `GetLogicOp`, `GetPatchDefaultOuterLevel`, `GetPatchDefaultInnerLevel`, `GetBlendFuncIndexed`, `GetBlendEquationIndexed`, `IsCapabilityEnabledIndexed(Blend)`, `GetColorMaskIndexed`); via `ComputePipelineStateHash` (`:4820`→`GetRenderStateParameters`) and `ResolveEffectiveSampleMask` (`:4812`→2×`IsCapabilityEnabled`, `GetRenderStateParameters`); via `SelectProvokingVertexMode` (`:12737`→`GetProvokingVertexMode`); via `ApplyDynamicDrawStateTail` (`:5901`→`GetRenderStateParametersVersion`, `GetRenderStateParameters`) → `ComputeGLViewport` (`:450`→`GetViewportIndexed`, `GetDepthRangeIndexed`), `ComputeGLScissorRect` (`:5855`→`GetRenderStateParameters`), `ApplyBlendConstants` (`:522`→`GetBlendColor`), `ApplyPolygonOffsetState` (`:555`→`GetPolygonOffsetUnits`, `GetPolygonOffsetFactor`), `ApplyLineWidthState` (`:569`→`GetLineWidth`), `ApplyStencilState` (`:648`→`GetStencilState`×2); via `UploadAndBindVertexBuffers` (`:3580`→`GetCurrentVertexAttribute` `:3926`) → `TryComputeMaxIndexFromHostBytes` (`:3409`→2×`IsCapabilityEnabled`); via `UploadAndBindIndexBuffer` (`:4031`→`GetRenderStateParameters` `:4060`); via `UniformManager::BindProgramUniformBuffers` (`UniformManager.cpp:2346`) → the whole descriptor group below; via `BeginXfbCaptureForDraw` (`:11266`) / `EndXfbCaptureForDraw` (`:11366`) |
| **descriptor group** (`UniformManager.cpp`) | `ResolveSamplerDescriptor` (`:464`→`GetTextureUnitObject` `:506`, `GetFramebufferBindingSlot(Draw)` `:555`), `ResolveSamplerTexture` (`:808`→`GetTextureUnitObject` `:821`), `ResolveSamplerTextureRaw` (`:834`→`:847`), `ProgramSamplesOnlySingleLevelTextures` (`:750`→`:782`), `ResolveSampledBinding` (`:1643`→`:1662`), `ResolveStorageTexelBufferDescriptor` (`:986`→`GetImageTextureBinding` `:1016`), `ResolveStorageImageDescriptor` (`:1259`→`:1295`), `ResolveStorageBufferDescriptor` (`:1146`→`GetBufferBindingPointCount` `:1189`, `GetBufferBindingPoint` `:1194`), `ResolveUniformBufferPayload` (`:1962`→`GetBufferBindingPointCount` `:2013`, `GetBufferBindingPoint(Uniform)` `:2018`), `CollectStorageImageTextures` (`:1801`→`GetImageTextureBinding` `:1850`), `CollectSamplerImageFeedback` (`:1883`→`:1938`) |
| **xfb group** | `BeginXfbCaptureForDraw` (`VulkanRenderer.cpp:11266`): `IsTransformFeedbackActive` `:11268`, `IsTransformFeedbackPaused` `:11274`, `GetTransformFeedbackProgram` `:11277`, `GetBufferBindingPoint(TransformFeedback)` `:11316`, `GetTransformFeedbackGeneration` `:11347`; `EndXfbCaptureForDraw` (`:11366`): `GetTransformFeedbackProgram` `:11370`; `CurrentXfbCounterSlot` (`:11217`): `GetBoundTransformFeedbackLifetimeId` `:11219`, `HasOpenTransformFeedbackSpan` `:11236` |
| `DispatchCompute` (`DirectVulkan.cpp:642` → `VulkanRenderer.cpp:7031`) | `GetProgramForDispatch` `:7036`; + `BindProgramUniformBuffers` → descriptor group |
| `DispatchComputeIndirect` (`:648` → `VulkanRenderer.cpp:7084`) | `GetProgramForDispatch` `:7088`, `GetBufferBindingSlot(DispatchIndirect)` `:7132`; + descriptor group |
| `MemoryBarrier` / `MemoryBarrierByRegion` (`:654`) | **no pulls** beyond the null assert |
| `Clear` (`:756` → `VulkanRenderer.cpp:7251`, **13 sites**) | `IsCapabilityEnabled(RasterizerDiscard)` `:7257`, `GetFramebufferBindingSlot(Draw)` `:7260`, `GetClearColor` `:7268`, `GetClearDepth` `:7269`, `GetClearStencil` `:7270`, `IsCapabilityEnabled(ScissorTest)` `:7277`, `GetColorMaskIndexed` `:7300`/`:7389`/`:7410`, `GetDepthMask` `:7327`/`:7369`, `GetStencilState` `:7340`/`:7373`; + `PrepareScissoredClear` (`:7188`→`GetScissorBox`×2) |
| `ClearBufferfi/fv/iv/uiv` (`DirectVulkan.cpp:335-356`) | `QueueClearBufferPayload` (`VulkanRenderer.cpp:7626`→`GetFramebufferBindingSlot(Draw)` `:7628`) → `QueueClearBufferPayloadForFramebuffer` (`:7434`, 5 sites: `IsCapabilityEnabled(RasterizerDiscard)` `:7439`, `(ScissorTest)` `:7481`, `GetDepthMask` `:7512`, `GetStencilState` `:7514`, `GetColorMaskIndexed` `:7526`) → `RecordScissoredClearBuffer` (`:7568`: `GetColorMaskIndexed` `:7583`, `GetDepthMask` `:7601`, `GetStencilState` `:7609`) + `PreCompensateSrgbClearColor` (`VkClearManager.cpp:53`→`IsCapabilityEnabled(FramebufferSrgb)` `:57`) |
| `ClearNamedFramebuffer*` (`DirectVulkan.cpp:359-386`) | same minus the `GetFramebufferBindingSlot` (the FBO arrives as a handle) |
| `BlitFramebuffer` (`:1005` → `VulkanRenderer.cpp:8583`) | `GetFramebufferBindingSlot(Read)` `:8586`, `(Draw)` `:8587`; → `BlitNamedFramebuffer` (`:8591`→`IsCapabilityEnabled(ScissorTest)` `:8619`, `GetScissorBox` `:8620`) |
| `BlitNamedFramebuffer` | as above, FBOs by handle |
| `CopyTexImage2D` (`:613`), `CopyTexSubImage2D` (`:619` → `VulkanRenderer.cpp:9196`) | `GetTextureUnitObject`+`GetActiveTextureUnit` `:9214`, `GetFramebufferBindingSlot(Read)` `:9222` |
| `CopyImageSubData` (`:625`) | endpoints by handle; no direct pull |
| `GenerateMipmap` (`:636` → `VulkanRenderer.cpp:10940`) | `GetTextureUnitObject`+`GetActiveTextureUnit` `:10963` |
| `ReadPixels` (`:739` → `VulkanRenderer.cpp:9927`) | `GetFramebufferBindingSlot(Read)` `:9933`; → `PackReadbackToClientOrPbo` (`:2732`→`GetClampReadColor` `:2771`), `ReadDepthStencilImageToClient` (`:10424`→`GetBufferBindingSlot(PixelPack)` `:10688`, `GetPixelStoreParameters` `:10689`) |
| `GetTexImage` (`:744` → `VulkanRenderer.cpp:10716`) | `GetTextureUnitObject`+`GetActiveTextureUnit` `:10719`; + the same pack path |
| `GetTextureImage` (`:749`) | texture by handle; + the same pack path |
| `ShaderStorageBlockBinding` (`DirectVulkan.cpp:709`) | `RecordError` `:716`; + `TryGetDirectVulkanProgram` (`:269`→`ValidateProgramName` `:270`, `GetProgramObject` `:273`) |
| `BeginXfbPrimitivesQuery` (`DirectVulkan.cpp:1275`) | `GetTransformFeedbackPausedPrimitiveCounter` `:1284` |
| `GetQueryResult64` (`DirectVulkan.cpp:1209`) | `GetTransformFeedbackPausedPrimitiveCounter` `:1237` |
| `Present` (`DirectVulkan.cpp`, table `:698`) | **no pulls** |
| `BindImageTexture`, `GetIntegeri_v`, sync group, timer group, occlusion group, `EndXfbPrimitivesQuery` | **no pulls** |
| **init/caps** — `BackendObject_DirectVulkan::InitCapabilities` (`:370`) and `ApplyVulkanCapabilitiesForTesting` (`:781`) | `InvalidateCompileEnv` (`:389`, `:787`) — the only pulls outside a GL verb |
| framebuffer/render-pass helpers reachable from every drawing verb | `VkRenderPassManager::ComputeHash` (`:602`→`IsCapabilityEnabled(FramebufferSrgb)` `:613`), `GetOrCreateRenderPass` (`:767`→`:965`), `ConvertTextureInternalFormatToVkEnum` (`:1044`→`:1111`), `VkTextureManager::GetOrCreateAttachmentViewAtMipLevel` (`:898`→`:952`), `VkTextureManager::SyncTextureAndGetDescriptor` (`:752`→`GetTextureObject` `:809`) |
| error reporting reachable from many verbs | `RecordClearBufferError` (`VulkanRenderer.cpp:1242`), `RecordTextureCopyError` (`:1246`), `RecordUnsupportedFramebufferError` (`:1302`) — all `RecordError` |

### 4.3 Caller evidence used for the reachability above

`PrepareForDraw` 14 call sites `DirectGLES.cpp:4556,4569,4587,4602,4663,4703,4771,4811,4856,4869,4894,4916,4931`;
`PrepareForCompute` `:6983,6991`; `SyncRenderState` 13 call sites `:2994,4121,6029,6064,6691,6785,7378,7424,7437,7453,7469,7489,7506`;
`SyncNeccessaryTextures` 13 call sites `:2983,4066,4115,6021,6063,6683,6777,7376,7422,7435,7451,7468`;
`SyncCurrentProgram` `:2993,4069`; `BindCurrentProgramWithResources` `:3012,4084`;
`SyncBufferBindingPoints` `:678,687,688`; `MarkShaderStorageBuffersGpuWritten` `:679,689`;
`SyncBoundBuffer` `:664,691`; `SyncAtomicCounterBuffers` `:3523`;
`SyncCurrentVertexAttributeValues` `:3009`; `SyncImageTextureBinding` `:1841,7317`;
`XfbImpl::StartPendingTransformFeedback` `:3016`; `ScatterCapturedRecords` `:1123`;
`PackStateFromContext` `:9027,9270`; `StoreReadbackRowsToClient` `:8159,8271,8322`.
Vulkan: `SetupDraw` `VulkanRenderer.cpp:11422,11744,11780,11985,12127,12224,12289`;
`TrySetupDrawFastPath` `:6450`; `GetOrCreatePipeline` `:6371,6896`;
`ApplyDynamicDrawStateTail` `:6407,6949`; `ApplyStencilState` `:5911,5986`;
`ApplyBlendConstants` `:5908,5983`; `ApplyPolygonOffsetState` `:5909,5984`;
`ApplyLineWidthState` `:5910,5985`; `ComputeGLViewport` `:510,5889`; `ComputeGLScissorRect` `:5890`;
`ResolveEffectiveSampleMask` `:4845,4862,5290`; `ComputePipelineStateHash` `:4989,6338`;
`ResolvePrimitiveRestartEnable` `:6067,6897,6960`; `GetBaseTransformFlagsRaw` `:6139,6510,6974`;
`GetShaderTransformFlags` `:6026`; `UploadAndBindVertexBuffers` `:6400,6937`;
`UploadAndBindIndexBuffer` `:6404,6945`; `TryComputeMaxIndexFromHostBytes` `:3601`;
`BeginXfbCaptureForDraw` `:11430,11753`; `EndXfbCaptureForDraw` `:11439,11763`;
`CurrentXfbCounterSlot` `:11346,11372`; `PrepareScissoredClear` `:7279,7483`;
`QueueClearBufferPayload` `:7641,7661,7758,7779`;
`QueueClearBufferPayloadForFramebuffer` `:7632,7683,7708,7723,7737`;
`RecordScissoredClearBuffer` `:7489`; `PackReadbackToClientOrPbo` `:10111,10119,10936`;
`ReadDepthStencilImageToClient` `:10368,10420,10817`; `SelectProvokingVertexMode` `:4433,5309`;
`BindProgramUniformBuffers` `:4754,6394,6928,7071`; `CollectStorageImageTextures` `:5687`;
`CollectSamplerImageFeedback` `:5787`; `ProgramSamplesOnlySingleLevelTextures` `:6556`;
UniformManager internals `ResolveSamplerDescriptor` `:2619`, `ResolveSamplerTexture` `:875`,
`ResolveSamplerTextureRaw` `:505,773`, `ResolveStorageTexelBufferDescriptor` `:2522`,
`ResolveStorageBufferDescriptor` `:2543`, `ResolveStorageImageDescriptor` `:2569`,
`ResolveSampledBinding` `:1742,1785,1901`, `ResolveUniformBufferPayload` `:2257`.

---

## 5. (c) `SyncPersistentMappedRange` and `SyncGpuWrites` call sites

Both are methods on `MG_State::GLState::BufferObject`. They are the two places a backend forces the
frontend's CPU shadow to become authoritative before it reads bytes out of it — i.e. the two places
a pipe boundary would otherwise hand the server stale memory.

### 5.1 `SyncPersistentMappedRange()` — 20 sites

Semantics: pushes writes made through a **non-adopted** persistent map (which mutate the shadow
with no API call and therefore bump no change serial) down into the backend resource. A no-op for
every buffer that is not such a map.

| # | file:line | receiver | enclosing function | verb that owns it | what it syncs |
|---|---|---|---|---|---|
| 1 | `DirectGLES/DirectGLES.cpp:263` | `drawBuffer` | `ResolveIndirectCommandBytes` (`:260`) | draw (indirect, CPU fallback) | `GL_DRAW_INDIRECT_BUFFER` before the CPU reads the command struct out of `MappedData()` |
| 2 | `DirectGLES/DirectGLES.cpp:4468` | `indexBuffer` | `ScopedRestartIndexSubstitution` (`:4433`) | draw (primitive-restart rewrite) | the element-array buffer before the whole-buffer restart-index rewrite |
| 3 | `DirectGLES/DirectGLES.cpp:4722` | `drawBuffer` | `MultiDrawElementsIndirectCount` (`:4685`) | draw (indirect count) | `GL_DRAW_INDIRECT_BUFFER` |
| 4 | `DirectGLES/DirectGLES.cpp:4723` | `parameterBuffer` | same | draw (indirect count) | `GL_PARAMETER_BUFFER` |
| 5 | `DirectGLES/DirectGLES.cpp:4824` | `drawBuffer` | `MultiDrawArraysIndirectCount` (`:4793`) | draw (indirect count) | `GL_DRAW_INDIRECT_BUFFER` |
| 6 | `DirectGLES/DirectGLES.cpp:4825` | `parameterBuffer` | same | draw (indirect count) | `GL_PARAMETER_BUFFER` |
| 7 | `DirectGLES/Managers.cpp:1569` | `bufferObject` | `EnsureBufferResource` (the GLES buffer-resource ensure path) | buffer op (draw/dispatch resource sync) | pushes map writes before the respecify / pending-range flush decision at `:1575-1584` |
| 8 | `DirectGLES/MultiDraw.cpp:510` | `indexBuffer` | the CPU multi-draw index flattener | draw (MultiDrawElements flatten) | the element-array buffer before flattening indices |
| 9 | `DirectVulkan/DirectVulkan.cpp:280` | `drawBuffer` | `ResolveIndirectCommandBytes` (`:277`) | draw (indirect, CPU fallback) | `GL_DRAW_INDIRECT_BUFFER` |
| 10 | `DirectVulkan/DirectVulkan.cpp:471` | `parameterBuffer` | `MultiDrawArraysIndirectCount` (`:448`) | draw (indirect count, CPU fallback) | `GL_PARAMETER_BUFFER` before `memcpy`ing the draw count |
| 11 | `DirectVulkan/DirectVulkan.cpp:795` | `indexBufferShared` | line-loop / CPU index path (`:788`) | draw (index read) | the element-array buffer before reading indices |
| 12 | `DirectVulkan/Renderer/UniformManager.cpp:2023` | `bufferObject` | `ResolveUniformBufferPayload` (`:1962`) | program/uniform sync (UBO) | the bound UBO before its bytes are staged into a descriptor |
| 13 | `DirectVulkan/Renderer/VkBufferManager.cpp:628` | `bufferObject` | `AcquireResidentSlice` (`:620`) | buffer op (any verb that acquires a resident slice) | the buffer before its GPU-resident slice is used |
| 14 | `DirectVulkan/Renderer/VkBufferManager.cpp:679` | `bufferObject` | `AcquireStreamedSlice` (`:672`) | buffer op (streamed upload) | the buffer before the transient copy is taken |
| 15 | `DirectVulkan/Renderer/VulkanRenderer.cpp:3434` | `indexBufferShared` | `TryComputeMaxIndexFromHostBytes` (`:3409`) | draw (index range scan) | element-array buffer before the max-index scan |
| 16 | `DirectVulkan/Renderer/VulkanRenderer.cpp:3513` | `bufferObject` | vertex-buffer fast-path revalidation inside `UploadAndBindVertexBuffers` (`:3580`'s memo check block at `:3500-3520`) | vertex sync (draw fast path) | each bound vertex buffer before its slice epoch is trusted |
| 17 | `DirectVulkan/Renderer/VulkanRenderer.cpp:3828` | `sourceBufferShared` | 64-bit attribute conversion inside `UploadAndBindVertexBuffers` | vertex sync (fp64 narrowing) | the source vertex buffer before CPU-side conversion |
| 18 | `DirectVulkan/Renderer/VulkanRenderer.cpp:7137` | `indirectBuffer` | `DispatchComputeIndirect` (`:7084`) | dispatch (indirect) | `GL_DISPATCH_INDIRECT_BUFFER` |
| 19 | `DirectVulkan/Renderer/VulkanRenderer.cpp:12132` | `drawBuffer` | `MultiDrawElementsIndirectCount` (`:12074`) | draw (indirect count) | `GL_DRAW_INDIRECT_BUFFER` |
| 20 | `DirectVulkan/Renderer/VulkanRenderer.cpp:12133` | `parameterBuffer` | same | draw (indirect count) | `GL_PARAMETER_BUFFER` |

Split: 8 DirectGLES / 12 DirectVulkan. By verb: draw 13, buffer op 3, vertex sync 2, dispatch 1,
program/uniform sync 1.
(Two further mentions are comments, not calls: `DirectGLES/Managers.cpp:1461`, `:2681`,
`DirectVulkan/Renderer/VulkanRenderer.h:1214`.)

### 5.2 `SyncGpuWrites()` — 6 sites

Semantics: a submit-and-wait plus writeback for a buffer flagged gpu-write-pending (XFB capture,
SSBO, storage texel buffer). Always paired with a CPU read of `MappedData()`.

| # | file:line | receiver | enclosing function | verb that owns it | what it syncs |
|---|---|---|---|---|---|
| 1 | `DirectGLES/DirectGLES.cpp:4469` | `indexBuffer` | `ScopedRestartIndexSubstitution` (`:4433`) | draw (primitive-restart rewrite) | pulls shader-written index bytes back before the rewrite |
| 2 | `DirectGLES/Managers.cpp:2643` | `bufferObject` | the fp64 vertex-attribute converter (`ConvertAttributeStream`, in `BackendVertexArrayObject`) | vertex sync (fp64 narrowing) | pulls shader writes back before the CPU narrows doubles to floats |
| 3 | `DirectGLES/MultiDraw.cpp:511` | `indexBuffer` | CPU multi-draw index flattener | draw (MultiDrawElements flatten) | shader-written indices before flattening |
| 4 | `DirectVulkan/Renderer/VulkanRenderer.cpp:3433` | `indexBufferShared` | `TryComputeMaxIndexFromHostBytes` (`:3409`) | draw (index range scan) | recorded-but-unexecuted GPU writes into the coherent index mapping |
| 5 | `DirectVulkan/Renderer/VulkanRenderer.cpp:3827` | `sourceBufferShared` | 64-bit attribute conversion in `UploadAndBindVertexBuffers` (`:3580`) | vertex sync (fp64 narrowing) | XFB/SSBO/texel-buffer writes into the source vertex buffer |
| 6 | `DirectVulkan/Renderer/VulkanRenderer.cpp:4161` | `indexBufferShared` | `UploadAndBindIndexBuffer` (`:4031`), restart-substitution branch | draw (primitive-restart rewrite) | shader-written indices before the whole-buffer rewrite |

Split: 3 DirectGLES / 3 DirectVulkan. By verb: draw 4, vertex sync 2.
(Three further mentions are comments: `DirectGLES/DirectGLES.cpp:459`, `:509`,
`DirectGLES/Managers.cpp:2681`.)

**P1 note on both lists.** Every one of the 26 sites is a *synchronous, blocking* call that the
backend makes on a frontend object in order to read host bytes. Twelve of them are followed
immediately by `MappedData()` (`DirectGLES.cpp:269`, `4470`, `4740`; `MultiDraw.cpp:512`;
`DirectVulkan.cpp:282`, `477`, `796`; `UniformManager.cpp:2025`; `VulkanRenderer.cpp:3435`, `3830`,
`4163`). Under disaggregation there is no shared address space for `MappedData()` to point into, so
each of these 26 becomes either a *pull-host-bytes* reverse call or a client-side pre-resolution
that ships the bytes with the verb. `MG_Pipe/MGPipeHostSpan.h` and the `HostSpanCount` field of
`MGP_FIELDS_MGPShaderBuffers` (`PipeFields.def`) are the existing hooks; nothing in
`PipeFields.def` covers the *index buffer* host span, which is what sites 1-3, 8, 11, 15 and
4/6 above need.
