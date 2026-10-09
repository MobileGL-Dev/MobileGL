# Monolith render-thread diff: dev vs p15 (FCL MC 1.21.5 vanilla, H2, cpuhunt)

The fixed function-level baseline for the monolith CPU cuts (SCOREBOARD.md has the fps
reference). Every cut starts from a symbolized profile of the p15 side and ends with a
before/after of the same function.

**Sources:**
- **Profiles:** simpleperf `cpu-clock --trace-offcpu --call-graph fp -f 4000 --duration 6`, taken
  after the measurement windows of an H2 run (FCL pinned to cpu2-6).
- **dev:** 08124c99 in `runs/safter/dev-Direct{Vulkan,GLES}-r1`.
- **p15:** p15t = HEAD 3def5cb2 (p15s 0b96a3ef plus the Magma blit view cache) in
  `runs/pt/t-Direct{Vulkan,GLES}-r1`.
- **Scope and units:** render thread only (the GL thread, which in monolith also applies), off-CPU
  samples dropped, in ms/frame. Frames are the run's measured fps × 6 s; profiling overhead
  inflates both sides alike.
- **Reducer:** `layerdiff.py` in the scratchpad.

**Attribution:** each sample goes to its innermost libMobileGL function, then to one layer.
- The leaf DSO decides first: driver (Adreno), libc+TLS (libc, libc++, `__emutls_get_address`,
  TLS init), or kernel.
- Otherwise the function's namespace decides:
  - client frontend: `MG_Impl`, `MG_State`;
  - pipe record: `MG_Pipe` validate, tracker and emitters, plus the record channel;
  - applier: `MG_Record` sinks and record emitters, plus `MG_Pipe` applier state;
  - backend: `MG_Backend`.
- Samples with no libMobileGL frame are app-side (JIT/JVM).

**Caveat on function rows:** some functions were renamed between dev and p15, so a "+" row and a
"-" row can be the same work. Read such pairs together:
- Espryt `FlushPendingRangesNow` -> `FlushPendingRangesFrom` (0.105 -> 0.116);
- `BackendVertexArrayObject::SyncToBackend` -> `SyncToBackendFromApplier` (0.062 -> 0.058);
- `BackendFramebufferObject::SyncToBackend` -> `SyncToBackendByHandle`;
- Magma `SetupDraw` -> `SetupWireDraw`.

## Where the excess lives (summary)

| layer | Magma diff | Espryt diff | what it is |
|---|---|---|---|
| pipe record | +0.36 | +0.35 | `MGPipeValidateForVerb` (0.34 inclusive on Magma). Its pieces: framebuffer-state build 0.06, vertex-buffer emit 0.04, global constants 0.03, slot lookups (`FindByLifetimeId` + `Acquire`) 0.05, residual `CopyField` 0.02, tracker 0.02 |
| driver | +0.23 | +0.06 | Magma: per-frame fixed Vulkan work (below). Espryt: about even once renames are paired |
| libc+TLS | +0.22 | +0.14 | `__emutls_get_address` 0.055-0.075 plus `pGLContext` TLS init 0.016 (dev: ~0). Magma: libc under the extra command-buffer and pass begins. Espryt: `QueryCurrentSurfaceSize` |
| client frontend | +0.12 | +0.17 | Spread thin. Atomics (`__aarch64_ldadd8_*`) +0.02, state lookups 0.005 each |
| backend | -0.10 | +0.11 | Magma's wire backend is cheaper than dev's `SetupDraw` (no XXH64 vertex-input hash). Espryt pays per-handle registry lookups |
| JIT/JVM (app) | +0.04 | +0.08 | Not ours; profiling noise between runs |

**Magma per-frame fixed costs** (inclusive ms/frame, dev -> p15):
- BeginRenderPass 0.045 -> 0.117.
- EndCommandRecording 0.054 -> 0.113.
- BeginCommandRecording 0.052 -> 0.089.
- EndRenderPass 0.023 -> 0.033.
- Present 0.303 -> 0.352.
- p15 also has `SyncTextureResourceByHandle` 0.068, of which `UploadPendingWireLevels` is 0.038
  and `FlushPendingCommands` 0.025.

Cause: every wire texture upload first submits the frame's recorded work
(`FlushWirePendingCommandsForTextureUpload`) and then uploads in a separate batch. MC updates
textures every frame, so the frame pays an extra submit, an extra command-buffer end/begin and a
render-pass restart. `GpuProgressMarkers::SubmitBracketed` is NOT new work: it wraps Present's
queue submit, which dev pays inside `Present` (0.121 vs 0.115).

**Espryt-only:** `QueryCurrentSurfaceSize` costs 0.065 ms/frame and dev pays nothing for it. It
is two `eglQuerySurface` calls on every dirty render-state sync (draws 0.048, clears 0.022). It
substitutes the surface for a scissor box the app never wrote (and for an empty viewport).

## Correction after cut 1 (2026-10-09): what the Magma fixed costs really are

Cut 1 (wire texture uploads stop flushing the frame recording) engaged on device:
`FlushPendingCommands` and `SubmitPendingCommandBuffer` left the profile, and
`UploadPendingWireLevels` went from 0.038 to 0.016. But Begin/EndCommandRecording and
BeginRenderPass did not move. H2 c1: 254.1 -> 249.8 fps, inside the noise.

Their cost is in the driver and scales with what is recorded:

| driver call | dev | p15 |
|---|---|---|
| vkResetCommandBuffer | 0.044 | 0.077 |
| vkEndCommandBuffer | 0.053 | 0.111 |
| vkCmdBeginRenderPass | 0.037 | 0.105 |

Host call counts per frame (diagnostic counting builds; FCL vanilla; 600-frame windows):
- **Magma, Vulkan calls: dev 331.6 vs p15 379.**
  - The draw-level stream is identical and almost free of repeats: DrawIndexed 86.1,
    BindVertexBuffers 87.1, BindDescriptorSets 87.1, BindPipeline 15.1, UpdateDescriptorSets 14,
    dynamic state 2 each.
  - Render passes: 4.1 -> 12.6.
  - Pipeline barriers: 3.4 -> 27.5.
  - CopyBufferToImage: 0.1 -> 2.2.
  - By call site (unwinding counter):
    - Pass begins: 6.5 at draws, 5.2 at clears, 1 at the blit.
    - Pass ends: 5.3 from `ResolveWireImageDescriptor` (sampling an image written earlier), 4.2
      from `TransitionWireImage` in the in-pass clear, 1.2 from `TransitionWireImage` in
      `SetupWireDraw`, 2 at the blit.
    - Barriers: 11.8 from `TransitionWireImage` at draw-pass begin, 6.3 at clear-pass begin, 5.6
      from `ResolveWireImageDescriptor`, 2 at the blit, 1 at present.
  - Cause: each new wire draw pass runs `TransitionWireImage(GENERAL -> GENERAL)` per
    attachment. That ends the open pass and records a full ALL_COMMANDS memory barrier whether or
    not a hazard exists.
- **Espryt, host GLES calls: dev 850.7 (479 repeat their previous arguments) vs p15 835.6 (446).**
  - p15 does not issue more GLES calls than dev.
  - In both, most of the repeats are glVertexAttribPointer 302 + glVertexAttribIPointer 75: the
    same layout re-pointed at a new VBO on every draw.

## Levers beyond the local cuts (evaluated 2026-10-09)

Estimates are ms/frame of FCL GL-thread CPU. "Measured" rows come from H2 A/Bs (SCOREBOARD).

| lever | result | status |
|---|---|---|
| Pipe batch (FB memo, handle hints, buffer reuse, gate cache, Magma hash TLS) | measured: Magma -0.06, Espryt -0.03 | landed 485dff14, c85c9439 |
| ThinLTO + -O3 + ICF/gc (gradle default ON) | measured: Magma -0.16 (2.52 -> 2.36), Espryt -0.09 | 190580f2 (coordinator pushes) |
| Magma rename spares 4 -> 12 + fence-poll quiet window | profile ceiling ~0.05-0.06 (Create 0.031, Destroy 0.014, vkGetFenceStatus 0.020) | a0015bad; m2ab A/B |
| API 29 separate artifact (native ELF TLS) | estimate 0.08-0.1 (emutls 0.054 + pGLContext TLS init 0.024 on Magma) | wip/api29-switch, A/B open |
| AutoFDO/PGO | estimate 0.1-0.25 | with the user |
| Emulated-TLS work below 29 (mirror, owner-thread path, dual lib) | estimate 0.024-0.08 | dropped by the user |
| App-side JIT delta | +0.065 vs dev (0.708 -> 0.773 app-only), cause unknown | needs a GL entry-count A/B |
| shared_ptr refcount avoidance in bind paths | 0.005-0.01 | not started |
| Espryt VertexAttribPointer split | <=0.04 | parked |

## Cuts, biggest first

**Revised order:**
1. **1b (Magma):** explicit external subpass dependencies on the wire draw passes. Barrier only
   images last written by transfer or storage, once and coalesced before the pass.
   Estimate: passes 12.6 -> ~5, barriers 27 -> ~5, and the driver's command-buffer and
   pass-begin cost with them.
2. **Cut 1 stays local** (no measurable gain).

**Order from 2026-10-09:**
1. 1c deferred clears (PARITY-AUDIT M1).
2. The remaining Magma barriers.
3. The pipe-record pieces.
4. The TLS hoist.

**Measured and parked: Espryt `glVertexAttribPointer` -> `glBindVertexBuffer` + `glVertexAttribFormat`.**
- Ceiling: `SyncToBackendFromApplier` costs 0.060 ms/frame inclusive, driver calls included
  (0.015 self). Source: the c4 p15w Espryt profile, cpuhunt.
- The repeats are MC's per-draw re-point: the same layout with a new buffer at offset 0, about
  4 attributes per buffer. The split would turn 1 `glBindBuffer` + ~4 pointer calls into 1
  `glBindVertexBuffer`.
- Expected saving: ~0.02-0.04 ms/frame. That is below H2 resolution (1.2-2.9% spread).
- Cost: a second VAO shadow (format + binding). ES `glVertexAttribPointer` and
  `glVertexAttribDivisor` silently rebind attribute -> binding, so the change is
  correctness-sensitive.

The original list follows.


Estimates are ms/frame on the H2 render thread. Each cut gets a before/after profile of its
function and an H2 A/B.

1. **Magma: record wire texture uploads into the frame's command buffer** instead of
   flush + separate batch. Estimate 0.10-0.15. It removes the extra submit, the command-buffer
   end/begin and the pass restart. Correctness-sensitive: draw / TexSubImage / draw ordering, and
   staging lifetime per frame. Red-once with the existing between-draws upload scenarios, plus
   syncval.
2. **Shared: the pipe-record layer**, about 0.35 on both backends. Parts:
   - (a) framebuffer-state build per verb: memo on the framebuffer's version;
   - (b) slot lookups by lifetime id;
   - (c) residual `CopyField` on monolith, where `fillOwed` is always true;
   - (d) vertex-buffer and global-constant emit.
   Each part is 0.02-0.06. Profile-driven, one at a time.
3. **Shared: TLS.** `__emutls_get_address` plus TLS init is ~0.07-0.09. The structural fix is
   native TLS (minSdk, the user's call). The in-code mitigation is to read
   `pGLContext` / `gPipeInputs` once per verb and pass the pointer down.
4. **Espryt: memoize the surface size** per draw surface, invalidated at swap and make-current.
   Estimate 0.06.
5. **Magma: the clear path** (`glClear` 0.285 vs dev ~0.13): first re-profile after cut 1, since
   much of it is the command-buffer and pass begins cut 1 removes.

## Magma (DirectVulkan): +0.88 ms/frame on the render thread (schedstat: 2.84 vs 2.00)

| layer | dev | p15 | diff |
|---|---|---|---|
| client frontend | 0.257 | 0.376 | +0.119 |
| pipe record | 0.000 | 0.363 | +0.363 |
| applier | 0.000 | 0.026 | +0.026 |
| backend | 0.516 | 0.416 | -0.100 |
| driver | 0.394 | 0.622 | +0.228 |
| libc+TLS | 0.213 | 0.430 | +0.218 |
| kernel (from MobileGL) | 0.112 | 0.115 | +0.003 |
| mgl other | 0.066 | 0.049 | -0.017 |
| JIT/JVM (app) | 0.659 | 0.697 | +0.038 |
| kernel (app) | 0.049 | 0.047 | -0.001 |
| **total** | 2.265 | 3.142 | +0.877 |

Top 15 p15 excess rows:

| layer | function (attributed) | dev | p15 | diff |
|---|---|---|---|---|
| libc+TLS | `__emutls_get_address` | 0.000 | 0.075 | +0.075 |
| driver | `VkRenderPassManager::BeginRenderPass` | 0.029 | 0.081 | +0.052 |
| driver | `FrameContext::EndCommandRecording` | 0.045 | 0.090 | +0.045 |
| kernel (from MobileGL) | `GpuProgressMarkers::SubmitBracketed` | 0.000 | 0.045 | +0.045 |
| driver | `VulkanRenderer::SetupWireDraw` | 0.000 | 0.040 | +0.040 |
| pipe record | `MG_Pipe::MGPipeValidateForVerb` | 0.000 | 0.035 | +0.035 |
| backend | `VulkanRenderer::SetupWireDraw` | 0.000 | 0.034 | +0.034 |
| driver | `VulkanRenderer::DrawElements` | 0.101 | 0.129 | +0.028 |
| pipe record | `MG_Pipe::MGPipeSlotAllocator::FindByLifetimeId` | 0.000 | 0.026 | +0.026 |
| driver | `GpuProgressMarkers::SubmitBracketed` | 0.000 | 0.024 | +0.024 |
| driver | `UniformManager::ResolveWireImageDescriptor` | 0.000 | 0.023 | +0.023 |
| pipe record | `MG_Pipe::MGPipeTracker::Update` | 0.000 | 0.022 | +0.022 |
| libc+TLS | `VkRenderPassManager::BeginRenderPass` | 0.010 | 0.032 | +0.022 |
| libc+TLS | `FrameContext::BeginCommandRecording` | 0.018 | 0.039 | +0.021 |

Per layer (top 6 each):

| layer | function (attributed) | dev | p15 | diff |
|---|---|---|---|---|
| client frontend | `__aarch64_ldadd8_acq_rel` | 0.013 | 0.023 | +0.010 |
| client frontend | `MG_State::GLState::FramebufferAttachmentObject::IsEmpty` | 0.000 | 0.008 | +0.007 |
| client frontend | `MG_State::GLState::BufferState::GetBindingSlot` | 0.000 | 0.006 | +0.006 |
| client frontend | `__aarch64_ldadd8_relax` | 0.004 | 0.010 | +0.006 |
| client frontend | `MG_State::GLState::ShareGroupState::GetBufferObject` | 0.000 | 0.005 | +0.005 |
| client frontend | `MG_State::GLState::FramebufferObject::CheckCompleteness` | 0.000 | 0.004 | +0.004 |
| pipe record | `MG_Pipe::MGPipeValidateForVerb` | 0.000 | 0.035 | +0.035 |
| pipe record | `MG_Pipe::MGPipeSlotAllocator::FindByLifetimeId` | 0.000 | 0.026 | +0.026 |
| pipe record | `MG_Pipe::MGPipeTracker::Update` | 0.000 | 0.022 | +0.022 |
| pipe record | `MG_Pipe::MGPipeFillAccess::CopyField` | 0.000 | 0.021 | +0.021 |
| pipe record | `MG_Pipe::MGPipeVertexInputEmitter::EmitVertexBuffers` | 0.000 | 0.018 | +0.018 |
| pipe record | `MG_Pipe::MGPipeProgramEmitter::EmitGlobalConstants` | 0.000 | 0.011 | +0.011 |
| applier | `MG_Record::RecordVerbSink::OnDrawVbo` | 0.000 | 0.005 | +0.005 |
| applier | `MG_Record::{anon}::EmitDrawRecord` | 0.000 | 0.004 | +0.004 |
| applier | `MG_Record::{anon}::EmitClear` | 0.000 | 0.003 | +0.003 |
| applier | `MG_Record::{anon}::EmitIndexedDraw` | 0.000 | 0.003 | +0.003 |
| applier | `MG_Record::{anon}::EmitDrawElements` | 0.000 | 0.003 | +0.003 |
| applier | `MG_Pipe::MGPipeApplierState::DrawFramebuffer` | 0.000 | 0.003 | +0.003 |
| backend | `VulkanRenderer::SetupWireDraw` | 0.000 | 0.034 | +0.034 |
| backend | `VulkanRenderer::GetOrCreatePipelineWithInput` | 0.000 | 0.021 | +0.021 |
| backend | `UniformManager::PrepareWireTextureResources` | 0.000 | 0.018 | +0.018 |
| backend | `_t const&, std::__ndk1::tuple<unsigned long const&>, std::__ndk1::tuple<> >` | 0.000 | 0.016 | +0.016 |
| backend | `UniformManager::ResolveWireImageDescriptor` | 0.000 | 0.014 | +0.014 |
| backend | `VkTextureManager::ResolveWireTextureStorage` | 0.000 | 0.012 | +0.012 |
| driver | `VkRenderPassManager::BeginRenderPass` | 0.029 | 0.081 | +0.052 |
| driver | `FrameContext::EndCommandRecording` | 0.045 | 0.090 | +0.045 |
| driver | `VulkanRenderer::SetupWireDraw` | 0.000 | 0.040 | +0.040 |
| driver | `VulkanRenderer::DrawElements` | 0.101 | 0.129 | +0.028 |
| driver | `GpuProgressMarkers::SubmitBracketed` | 0.000 | 0.024 | +0.024 |
| driver | `UniformManager::ResolveWireImageDescriptor` | 0.000 | 0.023 | +0.023 |
| libc+TLS | `__emutls_get_address` | 0.000 | 0.075 | +0.075 |
| libc+TLS | `VkRenderPassManager::BeginRenderPass` | 0.010 | 0.032 | +0.022 |
| libc+TLS | `FrameContext::BeginCommandRecording` | 0.018 | 0.039 | +0.021 |
| libc+TLS | `thread-local initialization routine for MG_State::pGLContext` | 0.000 | 0.017 | +0.017 |
| libc+TLS | `EndActiveRenderPassOn` | 0.000 | 0.014 | +0.014 |
| libc+TLS | `FrameContext::EndCommandRecording` | 0.009 | 0.023 | +0.014 |

## Espryt (DirectGLES): +0.93 ms/frame on the render thread (schedstat: 3.13 vs 2.34)

| layer | dev | p15 | diff |
|---|---|---|---|
| client frontend | 0.238 | 0.404 | +0.166 |
| pipe record | 0.000 | 0.349 | +0.349 |
| applier | 0.000 | 0.041 | +0.041 |
| backend | 0.245 | 0.356 | +0.111 |
| driver | 0.800 | 0.861 | +0.062 |
| libc+TLS | 0.209 | 0.350 | +0.141 |
| kernel (from MobileGL) | 0.209 | 0.221 | +0.011 |
| mgl other | 0.134 | 0.113 | -0.021 |
| JIT/JVM (app) | 0.693 | 0.774 | +0.081 |
| kernel (app) | 0.055 | 0.050 | -0.005 |
| **total** | 2.584 | 3.518 | +0.935 |

Top 15 p15 excess rows:

| layer | function (attributed) | dev | p15 | diff |
|---|---|---|---|---|
| driver | `BufferImpl::{anon}::FlushPendingRangesFrom` | 0.000 | 0.091 | +0.091 |
| libc+TLS | `__emutls_get_address` | 0.000 | 0.055 | +0.055 |
| driver | `VertexArrayImpl::BackendVertexArrayObject::SyncToBackendFromApplier` | 0.000 | 0.040 | +0.040 |
| pipe record | `MG_Pipe::MGPipeValidateForVerb` | 0.000 | 0.040 | +0.040 |
| driver | `DrawElements` | 0.276 | 0.312 | +0.036 |
| pipe record | `MG_Pipe::MGPipeTracker::Update` | 0.000 | 0.029 | +0.029 |
| libc+TLS | `QueryCurrentSurfaceSize` | 0.000 | 0.026 | +0.026 |
| libc+TLS | `BufferImpl::{anon}::FlushPendingRangesFrom` | 0.000 | 0.025 | +0.025 |
| driver | `FramebufferImpl::BackendFramebufferObject::SyncToBackendByHandle` | 0.000 | 0.025 | +0.025 |
| mgl other | `QueryCurrentSurfaceSize` | 0.000 | 0.023 | +0.023 |
| pipe record | `MG_Pipe::MGPipeSlotAllocator::FindByLifetimeId` | 0.000 | 0.022 | +0.022 |
| pipe record | `MG_Pipe::MGPipeVertexInputEmitter::EmitVertexBuffers` | 0.000 | 0.021 | +0.021 |
| pipe record | `MG_Pipe::MGPipeFillAccess::CopyField` | 0.000 | 0.018 | +0.018 |
| backend | `VertexArrayImpl::BackendVertexArrayObject::SyncToBackendFromApplier` | 0.000 | 0.018 | +0.018 |

Per layer (top 6 each):

| layer | function (attributed) | dev | p15 | diff |
|---|---|---|---|---|
| client frontend | `__aarch64_ldadd8_acq_rel` | 0.012 | 0.026 | +0.014 |
| client frontend | `__aarch64_ldadd8_relax` | 0.003 | 0.013 | +0.011 |
| client frontend | `MG_State::GLState::FramebufferAttachmentObject::IsEmpty` | 0.000 | 0.006 | +0.006 |
| client frontend | `MG_State::GLState::ShareGroupState::ValidateBufferObject` | 0.000 | 0.006 | +0.006 |
| client frontend | `MG_State::GLState::BufferState::GetBindingSlot` | 0.001 | 0.006 | +0.005 |
| client frontend | `MG_State::GLState::GLContext::GetProgramForDraw` | 0.002 | 0.007 | +0.005 |
| pipe record | `MG_Pipe::MGPipeValidateForVerb` | 0.000 | 0.040 | +0.040 |
| pipe record | `MG_Pipe::MGPipeTracker::Update` | 0.000 | 0.029 | +0.029 |
| pipe record | `MG_Pipe::MGPipeSlotAllocator::FindByLifetimeId` | 0.000 | 0.022 | +0.022 |
| pipe record | `MG_Pipe::MGPipeVertexInputEmitter::EmitVertexBuffers` | 0.000 | 0.021 | +0.021 |
| pipe record | `MG_Pipe::MGPipeFillAccess::CopyField` | 0.000 | 0.018 | +0.018 |
| pipe record | `MG_Pipe::MGPipeProgramEmitter::EmitGlobalConstants` | 0.000 | 0.009 | +0.009 |
| applier | `MG_Record::RecordVerbSink::OnDrawVbo` | 0.000 | 0.009 | +0.009 |
| applier | `MG_Record::{anon}::EmitDrawRecord` | 0.000 | 0.009 | +0.009 |
| applier | `MG_Pipe::MGPipeApplierState::DrawFramebuffer` | 0.000 | 0.005 | +0.005 |
| applier | `MG_Record::{anon}::EmitIndexedDraw` | 0.000 | 0.004 | +0.004 |
| applier | `MG_Record::{anon}::EmitDrawElements` | 0.000 | 0.002 | +0.002 |
| applier | `MG_Pipe::MGPipeApplierState::ReadFramebuffer` | 0.000 | 0.002 | +0.002 |
| backend | `VertexArrayImpl::BackendVertexArrayObject::SyncToBackendFromApplier` | 0.000 | 0.018 | +0.018 |
| backend | `TextureImpl::BackendTextureObject::IsDrawSyncCleanByRecord` | 0.000 | 0.013 | +0.013 |
| backend | `BufferImpl::EnsureBufferResourceForHandle` | 0.000 | 0.012 | +0.012 |
| backend | `eRegistry<MG_State::GLState::BufferObject, BufferImpl::GLESBufferResource, ` | 0.000 | 0.011 | +0.011 |
| backend | `stry<MG_State::GLState::ITextureObject, TextureImpl::BackendTextureObject, ` | 0.000 | 0.010 | +0.010 |
| backend | `ry<MG_State::GLState::ProgramObject, PrgramImpl::BackendProgramObjectImpl, ` | 0.000 | 0.009 | +0.009 |
| driver | `BufferImpl::{anon}::FlushPendingRangesFrom` | 0.000 | 0.091 | +0.091 |
| driver | `VertexArrayImpl::BackendVertexArrayObject::SyncToBackendFromApplier` | 0.000 | 0.040 | +0.040 |
| driver | `DrawElements` | 0.276 | 0.312 | +0.036 |
| driver | `FramebufferImpl::BackendFramebufferObject::SyncToBackendByHandle` | 0.000 | 0.025 | +0.025 |
| driver | `QueryCurrentSurfaceSize` | 0.000 | 0.016 | +0.016 |
| driver | `PrepareForDraw` | 0.005 | 0.010 | +0.005 |
| libc+TLS | `__emutls_get_address` | 0.000 | 0.055 | +0.055 |
| libc+TLS | `QueryCurrentSurfaceSize` | 0.000 | 0.026 | +0.026 |
| libc+TLS | `BufferImpl::{anon}::FlushPendingRangesFrom` | 0.000 | 0.025 | +0.025 |
| libc+TLS | `thread-local initialization routine for MG_State::pGLContext` | 0.000 | 0.016 | +0.016 |
| libc+TLS | `TextureImpl::SyncTextureToBackendByHandle` | 0.000 | 0.013 | +0.013 |
| libc+TLS | `FramebufferImpl::BackendFramebufferObject::SyncToBackendByHandle` | 0.000 | 0.007 | +0.007 |

