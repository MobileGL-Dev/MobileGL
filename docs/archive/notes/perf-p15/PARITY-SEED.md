# Deleted-symbol inventory: 88a11b81 (OLD) -> a0f4aa83 (NEW)

Seed for the stage 0.1 parity audit (PLAN-P15.md). Regex-based extraction, not compiler-accurate. See method notes at the end.

## Summary

| file | present at NEW | OLD bytes | NEW bytes | deleted names | of which gone |
|---|---|---|---|---|---|
| `MobileGL/MG_Backend/DirectVulkan/Renderer/VkClearManager.cpp` | yes | 24665 | 4092 | 29 | 29 |
| `MobileGL/MG_Backend/DirectVulkan/Renderer/VkClearManager.h` | yes | 8751 | 2769 | 11 | 11 |
| `MobileGL/MG_Backend/DirectVulkan/Renderer/VkRenderPassManager.cpp` | yes | 102628 | 6349 | 23 | 22 |
| `MobileGL/MG_Backend/DirectVulkan/Renderer/VkRenderPassManager.h` | yes | 24416 | 8866 | 8 | 7 |
| `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp` | yes | 1009784 | 534405 | 85 | 85 |
| `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.h` | yes | 148622 | 108551 | 10 | 10 |
| `MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.cpp` | yes | 224566 | 120448 | 25 | 23 |
| `MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.h` | yes | 46216 | 38007 | 2 | 2 |
| `MobileGL/MG_Backend/DirectVulkan/Renderer/VkTextureManager.cpp` | yes | 291971 | 138980 | 51 | 50 |
| `MobileGL/MG_Backend/DirectVulkan/Renderer/VkTextureManager.h` | yes | 61972 | 45328 | 12 | 12 |
| `MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.cpp` | yes | 116229 | 84585 | 25 | 25 |
| `MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.h` | yes | 28699 | 22339 | 3 | 3 |
| `MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.cpp` | yes | 49266 | 26249 | 7 | 7 |
| `MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.h` | yes | 13262 | 8134 | 3 | 2 |
| `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp` | yes | 1149763 | 898589 | 53 | 50 |
| `MobileGL/MG_Backend/DirectGLES/Managers.cpp` | yes | 1063201 | 912235 | 49 | 48 |
| `MobileGL/MG_Backend/DirectGLES/Managers.h` | yes | 254434 | 229808 | 14 | 14 |

## `MobileGL/MG_Backend/DirectVulkan/Renderer/VkClearManager.cpp`

OLD defs: 32, NEW defs: 3, deleted: 29 (gone 29, params differ 0).

| name | body lines | status | HEAD elsewhere (token) | comment |
|---|---|---|---|---|
| `VkClearManager::QueueClear(GLbitfield mask, const ClearFramebufferPayload& clearPayload, const MG_State::GLState::Fra...` | 48 | gone | 0 |  |
| `VkClearManager::GetPendingClear(const PendingClearKey& key, ClearAttachmentPayload& outPayload, SharedPtr<MG_State::G...` | 29 | gone | 0 |  |
| `VkClearManager::CollectGarbage()` | 23 | gone | 3: VkTextureManager.cpp, VkTextureManager.h, ... |  |
| `VkClearManager::GetPendingClears(MG_State::GLState::ITextureObject* texture, Vector<PendingClearEntry>& outEntries)` | 23 | gone | 0 |  |
| `VkClearManager::QueueClear(const ClearAttachmentPayload& clearPayload, const MG_State::GLState::FramebufferAttachment...` | 22 | gone | 0 |  |
| `VkClearManager::LockTextureIdentityLocked(const TextureIdentity& identity, SharedPtr<MG_State::GLState::ITextureObjec...` | 21 | gone | 0 |  |
| `VkClearManager::HasPendingClear(MG_State::GLState::ITextureObject* texture)` | 20 | gone | 0 |  |
| `VkClearManager::MergeClearPayload(ClearAttachmentPayload& dst, const ClearAttachmentPayload& src)` | 20 | gone | 0 |  |
| `VkClearManager::MakePendingClearKey(MG_State::GLState::ITextureObject* rawTexture, Uint32 mipLevel, Uint32 baseArrayL...` | 18 | gone | 0 |  |
| `VkClearManager::PopPendingClear(const PendingClearKey& key)` | 18 | gone | 0 |  |
| `VkClearManager::HasPendingClear(const PendingClearKey& key)` | 16 | gone | 0 |  |
| `VkClearManager::MakeTextureIdentity(MG_State::GLState::ITextureObject* texture)` | 15 | gone | 0 |  |
| `VkClearManager::ErasePendingClearsForTextureLocked(const TextureIdentity& identity)` | 14 | gone | 0 |  |
| `VkClearManager::QueueClear(const ClearAttachmentPayload& clearPayload, const SharedPtr<MG_State::GLState::ITextureObj...` | 14 | gone | 0 |  |
| `ForceOpaqueClearAlpha(ClearAttachmentPayload& payload)` | 13 | gone | 0 |  |
| `VkClearManager::PopPendingClear(MG_State::GLState::ITextureObject* texture)` | 13 | gone | 0 |  |
| `GetClearableAttachment(const MG_State::GLState::FramebufferObject& drawFbo, FramebufferAttachmentType attachmentType)` | 12 | gone | 0 |  |
| `ResolveAttachmentBaseArrayLayer(const MG_State::GLState::FramebufferAttachmentObject& attachment)` | 10 | gone | 1: BackendObject_DirectVulkan.cpp |  |
| `VkClearManager::MakePendingClearKey(const MG_State::GLState::FramebufferAttachmentObject& attachment)` | 10 | gone | 0 |  |
| `ClearStorageTextureOf(MG_State::GLState::ITextureObject* texture)` | 7 | gone | 0 | The texture a pending clear is actually ABOUT. A clear issued through a GL texture view |
| `VkClearManager::GetPendingClear(const MG_State::GLState::FramebufferAttachmentObject& attachment, ClearAttachmentPayl...` | 7 | gone | 0 |  |
| `ResolveAttachmentBaseArrayLayer(TextureUploadTarget target)` | 6 | gone | 1: BackendObject_DirectVulkan.cpp |  |
| `VkClearManager::HasPendingClear(const MG_State::GLState::FramebufferAttachmentObject& attachment)` | 6 | gone | 0 |  |
| `VkClearManager::LockTextureLocked(const PendingClearKey& key, SharedPtr<MG_State::GLState::ITextureObject>& outTexture)` | 6 | gone | 0 |  |
| `VkClearManager::PopPendingClear(const MG_State::GLState::FramebufferAttachmentObject& attachment)` | 6 | gone | 0 |  |
| `VkClearManager::Shutdown()` | 6 | gone | 26: BackendObjects.h, BufferArena.cpp, ... |  |
| `VkClearManager::GetPendingClear(const PendingClearKey& key, ClearAttachmentPayload& outPayload)` | 4 | gone | 0 |  |
| `PendingClearMatchesTextureIdentity(const PendingClearKey& key, const TextureIdentity& identity)` | 3 | gone | 0 |  |
| `VkClearManager::Initialize()` | 3 | gone | 32: BackendObject.h, BackendObjects.h, ... |  |

## `MobileGL/MG_Backend/DirectVulkan/Renderer/VkClearManager.h`

OLD defs: 13, NEW defs: 2, deleted: 11 (gone 11, params differ 0).

| name | body lines | status | HEAD elsewhere (token) | comment |
|---|---|---|---|---|
| `class VkClearManager` | 55 | gone | 5: VkClearManager.cpp, VkRenderPassManager.cpp, ... |  |
| `class PendingClearKeyHash` | 15 | gone | 0 |  |
| `PendingClearKeyHash::operator()(const PendingClearKey& key)` | 13 | gone | 0 |  |
| `class PendingClearKey` | 13 | gone | 0 |  |
| `class TextureIdentity` | 8 | gone | 0 |  |
| `class TextureIdentityHash` | 7 | gone | 0 |  |
| `PendingClearKey::operator==(const PendingClearKey& other)` | 5 | gone | 0 |  |
| `TextureIdentityHash::operator()(const TextureIdentity& key)` | 5 | gone | 0 |  |
| `class PendingClearEntry` | 4 | gone | 0 |  |
| `TextureIdentity::operator==(const TextureIdentity& other)` | 3 | gone | 0 |  |
| `VkClearManager::HasAnyPendingClears()` | 1 | gone | 0 | Lock-free probe for the consecutive-draw fast path: any pending clear |

## `MobileGL/MG_Backend/DirectVulkan/Renderer/VkRenderPassManager.cpp`

OLD defs: 35, NEW defs: 13, deleted: 23 (gone 22, params differ 1).

| name | body lines | status | HEAD elsewhere (token) | comment |
|---|---|---|---|---|
| `VkRenderPassManager::GetOrCreateRenderPass(const MG_State::GLState::FramebufferObject& fbo, Uint32 swapchainImageInde...` | 760 | gone | 2: VkRenderPassManager.h, VulkanRenderer.cpp |  |
| `VkRenderPassManager::GetOrCreateRenderbufferResource(const SharedPtr<MG_State::GLState::RenderbufferObject>& renderbu...` | 169 | gone | 0 |  |
| `VkRenderPassManager::ComputeHash(const MG_State::GLState::FramebufferObject& fbo, Uint32 swapchainImageIndex, Bool in...` | 163 | gone | 5: PipelineFactory.cpp, PipelineFactory.h, ... |  |
| `VkRenderPassManager::OnPresent()` | 43 | gone | 6: DirectGLES.cpp, DirectVulkan.cpp, ... |  |
| `ResolveCompleteColorAttachmentTexture(const MG_State::GLState::FramebufferObject& fbo, FramebufferAttachmentType atta...` | 35 | gone | 0 |  |
| `ResolveAttachmentViewType(const MG_State::GLState::FramebufferAttachmentObject& attachment, const VkTextureManager::T...` | 30 | gone | 0 | VUID-VkFramebufferCreateInfo-flags-04113: every view handed to vkCreateFramebuffer must have |
| `VkRenderPassManager::CollectRenderbufferGarbage()` | 30 | gone | 0 |  |
| `VkRenderPassManager::QueueRenderbufferClear(const ClearAttachmentPayload& clearPayload, const MG_State::GLState::Fram...` | 27 | gone | 0 |  |
| `VkRenderPassManager::QueueRenderbufferClear(GLbitfield mask, const ClearFramebufferPayload& clearPayload, const MG_St...` | 24 | gone | 0 |  |
| `VkRenderPassManager::RenderbufferResource::Destroy(VkDevice device, VmaAllocator allocator)` | 24 | gone | 17: BackendObjects.h, DirectGLES.cpp, ... |  |
| `VkRenderPassManager::CollectDeferredRenderbufferReleases(Bool destroyAll)` | 21 | gone | 0 |  |
| `ResolveAttachmentBaseArrayLayer(const MG_State::GLState::FramebufferAttachmentObject& attachment)` | 19 | gone | 1: BackendObject_DirectVulkan.cpp |  |
| `TryResolveSampleCountFlagBits(Int requestedSamples, VkSampleCountFlagBits& outSampleCount)` | 17 | gone | 1: VkTextureManager.cpp |  |
| `ResolveImageAspectMaskForFormat(VkFormat format)` | 16 | gone | 0 |  |
| `VkRenderPassManager::GetPendingRenderbufferClear(MG_State::GLState::RenderbufferObject* renderbuffer, ClearAttachment...` | 15 | gone | 0 |  |
| `VkRenderPassManager::DeferRenderbufferBackingRelease(RenderbufferResource& resource)` | 14 | gone | 0 |  |
| `VkRenderPassManager::PurgeRenderPasses()` | 10 | gone | 0 |  |
| `VkRenderPassManager::RetireAgeFrames()` | 8 | gone | 0 |  |
| `VkRenderPassManager::HasPendingRenderbufferClear(const MG_State::GLState::FramebufferAttachmentObject& attachment)` | 7 | gone | 0 |  |
| `VkRenderPassManager::VkRenderPassManager(VkDevice device, VkPhysicalDevice physicalDevice, VmaAllocator allocator, co...` | 7 | params differ | 12: PipelineFactory.cpp, PipelineFactory.h, ... |  |
| `VkRenderPassManager::PopPendingRenderbufferClear(MG_State::GLState::RenderbufferObject* renderbuffer)` | 5 | gone | 0 |  |
| `IsCubeMapFaceUploadTarget(TextureUploadTarget target)` | 4 | gone | 2: VkClearManager.cpp, VkTextureManager.cpp |  |
| `ColorFormatLacksAlpha(const MG_State::GLState::ITextureObject* texture)` | 3 | gone | 0 |  |

## `MobileGL/MG_Backend/DirectVulkan/Renderer/VkRenderPassManager.h`

OLD defs: 21, NEW defs: 13, deleted: 8 (gone 7, params differ 1).

| name | body lines | status | HEAD elsewhere (token) | comment |
|---|---|---|---|---|
| `class RenderbufferResource` | 26 | gone | 0 |  |
| `class TrackedAttachmentLayoutInfo` | 13 | gone | 0 |  |
| `class PendingClearAttachmentInfo` | 12 | gone | 0 |  |
| `class DeferredRenderbufferRelease` | 7 | gone | 0 | A superseded renderbuffer backing (glRenderbufferStorage respecify) parked |
| `class IEvictionObserver` | 5 | gone | 2: ProgramFactory.h, VulkanRenderer.h | Notified once per OnPresent sweep with every aged-out entry's VkRenderPass |
| `class PendingRenderbufferClear` | 4 | gone | 0 |  |
| `RenderPassEntry::RenderPassEntry(Uint64 hash, VkRenderPass renderpass, VkFramebuffer framebuffer, Uint64 compatibilit...` | 1 | params differ | 4: VkRenderPassManager.cpp, VulkanRenderer.cpp, ... |  |
| `VkRenderPassManager::SetEvictionObserver(IEvictionObserver* observer)` | 1 | gone | 2: ProgramFactory.h, VulkanRenderer.cpp | Observer may be null (no notifications). Not owned. |

## `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp`

OLD defs: 301, NEW defs: 216, deleted: 85 (gone 85, params differ 0).

| name | body lines | status | HEAD elsewhere (token) | comment |
|---|---|---|---|---|
| `VulkanRenderer::BlitNamedFramebuffer(const SharedPtr<MG_State::GLState::FramebufferObject>& readFbo, const SharedPtr<...` | 624 | gone | 5: BackendObject.h, DirectGLES.cpp, ... |  |
| `VulkanRenderer::UploadAndBindVertexBuffers(VkCommandBuffer commandBuffer, const MG_State::GLState::VertexArrayObject&...` | 417 | gone | 0 |  |
| `VulkanRenderer::TrySetupDrawFastPath(FrameContext::FrameData& frame, GLenum mode, Flags<DrawSetupAspect> aspects, con...` | 401 | gone | 0 |  |
| `VulkanRenderer::GenerateDepthMipmapWithShader(FrameContext::FrameData& frame, MG_State::GLState::ITextureObject& text...` | 351 | gone | 2: VulkanRenderer.h, WireDepthMipmap.inc |  |
| `VulkanRenderer::GetTextureImage(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, TextureUploadTarge...` | 235 | gone | 2: BackendObject.h, RecordVerbSink.cpp |  |
| `VulkanRenderer::TryBlitToDefaultFramebufferWithShader(FrameContext::FrameData& frame, MG_State::GLState::FramebufferO...` | 195 | gone | 0 |  |
| `VulkanRenderer::UploadAndBindIndexBuffer(FrameContext::FrameData& frame, const MG_State::GLState::VertexArrayObject& ...` | 179 | gone | 2: DirectVulkan.cpp, VulkanRenderer.h |  |
| `VulkanRenderer::MaterializePendingClearForTexture(VkCommandBuffer commandBuffer, MG_State::GLState::ITextureObject& t...` | 175 | gone | 2: VkClearManager.cpp, VkTextureManager.h |  |
| `VulkanRenderer::QueueClearBufferPayloadForFramebuffer(const MG_State::GLState::FramebufferObject& framebuffer, GLenum...` | 132 | gone | 0 |  |
| `VulkanRenderer::ReadDepthStencilPixels(MG_State::GLState::FramebufferObject& readFbo, GLint x, GLint y, GLsizei width...` | 118 | gone | 0 |  |
| `VulkanRenderer::LookupVaoDrawMemo(const MG_State::GLState::VertexArrayObject* vao)` | 114 | gone | 0 |  |
| `ResolveFramebufferBlitBinding(MG_State::GLState::FramebufferObject& fbo, Bool isReadFramebuffer, Uint32 swapchainImag...` | 113 | gone | 0 |  |
| `ResolveTextureCopySourceBinding(MG_State::GLState::FramebufferObject& fbo, Uint32 swapchainImageIndex, SwapchainObjec...` | 108 | gone | 0 |  |
| `ResolveColorBlitBinding(MG_State::GLState::FramebufferObject& fbo, Bool isReadFramebuffer, Uint32 swapchainImageIndex...` | 103 | gone | 0 |  |
| `VulkanRenderer::MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride)` | 101 | gone | 8: BackendObject.h, BackendObject_DirectGLES.cpp, ... |  |
| `VulkanRenderer::MultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount, GL...` | 99 | gone | 9: BackendObject.h, BackendObject_DirectGLES.cpp, ... |  |
| `VulkanRenderer::PrepareStorageImageTextures(FrameContext::FrameData& frame, const MagmaProgramSource& program, const ...` | 93 | gone | 0 |  |
| `VulkanRenderer::MaterializePendingClearForRenderbuffer(VkCommandBuffer commandBuffer, const SharedPtr<MG_State::GLSta...` | 87 | gone | 0 |  |
| `VulkanRenderer::MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei ...` | 81 | gone | 8: BackendObject.h, BackendObject_DirectGLES.cpp, ... |  |
| `VulkanRenderer::ClearDepthSliceWithRenderPass(VkCommandBuffer commandBuffer, MG_State::GLState::ITextureObject& textu...` | 79 | gone | 0 |  |
| `VulkanRenderer::MaterializePendingClearForDefaultFramebuffer(VkCommandBuffer commandBuffer, MG_State::GLState::Frameb...` | 77 | gone | 0 | A glClear on the DEFAULT framebuffer is parked as a pending clear and folded into the next |
| `VulkanRenderer::ClearAttachmentsOnActiveRenderPass(VkCommandBuffer commandBuffer, const RenderPassEntry &compatibleRe...` | 72 | gone | 0 |  |
| `VulkanRenderer::PrepareSamplerImageFeedbackSnapshots(FrameContext::FrameData& frame, const MagmaProgramSource& progra...` | 71 | gone | 0 |  |
| `VulkanRenderer::MaterializeMultisamplePendingClear(VkCommandBuffer commandBuffer, MG_State::GLState::ITextureObject& ...` | 67 | gone | 0 |  |
| `VulkanRenderer::MaterializePendingDepthStencilClearForDefaultFramebuffer(VkCommandBuffer commandBuffer, const MG_Stat...` | 67 | gone | 0 | The depth/stencil half of MaterializePendingClearForDefaultFramebuffer. Separate only |
| `VulkanRenderer::PrepareScissoredClear(const MG_State::GLState::FramebufferObject& framebuffer, VkClearRect& outClearR...` | 63 | gone | 0 |  |
| `TryComputeMaxIndexFromHostBytes(const MG_State::GLState::VertexArrayObject& vao, const IndexBufferView& indexView, Ui...` | 62 | gone | 0 | Scans the draw's index range from host-visible index bytes and returns the largest |
| `VulkanRenderer::TryBindResolvedVertexBindings(VkCommandBuffer commandBuffer, const MG_State::GLState::VertexArrayObje...` | 62 | gone | 0 |  |
| `VulkanRenderer::GetOrCreateBlitPipeline(const RenderPassEntry& renderPassEntry)` | 54 | gone | 0 | P7 wave 2-B2 (CONTRACT-P7 §5.2, (B')): the pull build's blit pipeline, built out of the |
| `VulkanRenderer::RecordScissoredClearBuffer(const MG_State::GLState::FramebufferObject& framebuffer, GLenum buffer, GL...` | 54 | gone | 0 |  |
| `VulkanRenderer::AcquireMultisampleResolveScratchImage(VkCommandBuffer commandBuffer, VkFormat format, VkExtent2D extent)` | 53 | gone | 0 |  |
| `EnsureGenerateMipmapStorageAllocated(::MobileGL::MG_State::GLState::TextureObjectMipmap& texture, Uint32 baseMipLevel...` | 50 | gone | 1: DirectGLES.cpp | glGenerateMipmap defines levels BASE_LEVEL+1 up to the level the base image's extent and |
| `GetNumericDomainForTextureInternalFormat(TextureInternalFormat format)` | 42 | gone | 0 |  |
| `VulkanRenderer::CopyDepthStencilAspectThroughBuffer(FrameContext::FrameData& frame, VkImage srcImage, VkImage dstImag...` | 37 | gone | 0 | A single-aspect depth/stencil copy between two images of the same format. An image copy |
| `VulkanRenderer::OrderPendingUploadAfterRecording(FrameContext::FrameData& frame, MG_State::GLState::ITextureObject& t...` | 37 | gone | 1: VkTextureManager.h |  |
| `RewriteRestartIndices(const void* source, SizeT sizeBytes, VkIndexType indexType, Uint32 applicationRestartIndex, Vec...` | 36 | gone | 1: DirectGLES.cpp | Copies index data, replacing every occurrence of the application's arbitrary restart |
| `ResolveTextureCopyDestinationBinding(MG_State::GLState::ITextureObject& texture, Uint32 mipLevel, VkTextureManager& t...` | 34 | gone | 0 |  |
| `EnsureGenerateMipmapShadowAllocated(const VkTextureManager::TextureResource& resource, Uint32 baseMipLevel, const Vec...` | 32 | gone | 0 | P5c (T5 / tx): the split arm of EnsureGenerateMipmapStorageAllocated. Under an active |
| `VkImageLayoutToString(VkImageLayout layout)` | 28 | gone | 0 |  |
| `ShadowedBindVertexBuffers(VkCommandBuffer commandBuffer, const VkBuffer* buffers, const VkDeviceSize* offsets, Uint32...` | 25 | gone | 0 | vkCmdBindVertexBuffers, skipped when this command buffer already holds these |
| `GetComponentCountForShaderValueType(GLenum glType)` | 22 | gone | 0 |  |
| `ConvertFloat64VertexStreamToFloat32(const MG_State::GLState::VertexAttribute& attribute, const Uint8* sourceData, Siz...` | 20 | gone | 0 | The fetch half of the 64-bit vertex narrowing, whose shader half is guaranteed by |
| `VulkanRenderer::ClearNamedFramebufferiv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum bu...` | 20 | gone | 1: BackendObject.h | The integer clears carry the same payload as their target-based siblings; only the |
| `ConvertIntegerVertexStreamToFloat32(const MG_State::GLState::VertexAttribute& attribute, const Uint8* sourceData, Siz...` | 19 | gone | 0 |  |
| `HasUnsupportedCompleteRenderbufferAttachment(const MG_State::GLState::FramebufferObject& framebufferObject)` | 19 | gone | 0 |  |
| `VulkanRenderer::ClearNamedFramebufferfv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum bu...` | 19 | gone | 1: BackendObject.h |  |
| `ConvertScaledIntegerVertexStreamToFloat32(const MG_State::GLState::VertexAttribute& attribute, const Uint8* sourceDat...` | 18 | gone | 0 |  |
| `GetSupportedColorWriteMaskForComponentCount(SizeT componentCount)` | 18 | gone | 0 |  |
| `HasDistinctCompleteDepthStencilTextureAttachments(const MG_State::GLState::FramebufferObject& framebufferObject)` | 17 | gone | 0 |  |
| `GetDepthStencilAspectMaskForFormat(VkFormat format)` | 16 | gone | 0 | The aspects a depth/stencil format actually carries. VkTextureManager keeps its own copy of |
| `ShouldUseTransientVertexIndexBuffer(const MG_State::GLState::BufferObject& bufferObject)` | 16 | gone | 0 |  |
| `ActiveRenderPassUsesTexture(const ActiveRenderPassInfo& activeRenderPass, const MG_State::GLState::ITextureObject& te...` | 15 | gone | 0 |  |
| `ApplyNativeBlitDefaultFramebufferSourceTransform(VkSurfaceTransformFlagBitsKHR preTransform, const BlitImageBinding& ...` | 14 | gone | 0 | The same conversion on the READ side, which never had one: a blit whose source is the |
| `ApplyNativeBlitDefaultFramebufferTransform(VkSurfaceTransformFlagBitsKHR preTransform, const BlitImageBinding& dstBin...` | 14 | gone | 0 |  |
| `VulkanRenderer::SettleTexelsBeforeQueuedClear(const MG_State::GLState::FramebufferObject& framebuffer, const MG_State...` | 14 | gone | 0 |  |
| `ConvertIntegerVertexComponentToFloat(ComponentT value, Bool normalized)` | 13 | gone | 0 |  |
| `ResolveAttachmentBaseArrayLayer(const MG_State::GLState::FramebufferAttachmentObject& attachment)` | 13 | gone | 1: BackendObject_DirectVulkan.cpp |  |
| `GetSwapchainDepthStencilAspectMask(const SwapchainObject& swapchainObject)` | 12 | gone | 0 |  |
| `IsValidSampledImageLayout(VkImageLayout layout)` | 12 | gone | 0 |  |
| `RepackVertexStream(const Uint8* sourceData, SizeT sourceStride, SizeT elementSize, SizeT elementCount, Vector<Uint8>&...` | 12 | gone | 0 |  |
| `ResolveFramebufferCopyAttachmentType(const MG_State::GLState::FramebufferObject& fbo, Bool isReadFramebuffer, VkImage...` | 12 | gone | 0 |  |
| `VulkanRenderer::ClearNamedFramebufferuiv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum b...` | 12 | gone | 1: BackendObject.h |  |
| `VulkanRenderer::OnRenderPassesDestroyed(const Vector<VkRenderPass>& renderPasses)` | 12 | gone | 0 |  |
| `ComputeFullMipLevelCount(const IntVec3& baseTexelSize)` | 11 | gone | 1: VkTextureManager.cpp |  |
| `MipShrinkingComponentCount(TextureTarget target)` | 11 | gone | 0 | How many components of a GL-space texel size actually halve down the mip chain. An array |
| `VulkanRenderer::ClearNamedFramebufferfi(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum bu...` | 11 | gone | 1: BackendObject.h |  |
| `VulkanRenderer::FinishPendingGpuWork()` | 11 | gone | 0 |  |
| `RequiresShaderBlitToDefaultFramebuffer(VkSurfaceTransformFlagBitsKHR preTransform)` | 9 | gone | 0 |  |
| `ComputeMipTexelSizeWithFixedComponents(const IntVec3& baseTexelSize, Uint32 relativeMipLevel, Int shrinkingComponents)` | 7 | gone | 0 |  |
| `RecordUnsupportedFramebufferError(const char* func)` | 7 | gone | 0 |  |
| `VulkanRenderer::GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels)` | 7 | gone | 6: BackendObject.h, DirectGLES.cpp, ... |  |
| `ComputeMipTexelSize(const IntVec3& baseTexelSize, Uint32 relativeMipLevel)` | 6 | gone | 0 |  |
| `IsUnsupportedFramebufferForDirectVulkan(const MG_State::GLState::FramebufferObject& framebufferObject)` | 5 | gone | 0 |  |
| `ResolveGenerateMipmapFinalLayout(VkImageAspectFlags aspectMask)` | 5 | gone | 0 |  |
| `VulkanRenderer::GetOrCreatePipeline(GLenum mode, const MagmaProgramSource& program, const ProgramFactory::VkProgramOb...` | 5 | gone | 4: MagmaPipeArms.h, PipelineFactory.cpp, ... |  |
| `class SnapshotCacheEntry` | 5 | gone | 0 |  |
| `constexpr(std::is_signed_v<ComponentT>)` | 5 | gone | 47: BackendObject.h, DirectGLES.cpp, ... |  |
| `AlignPixelRow(SizeT rowBytes, Int alignment)` | 4 | gone | 1: DirectGLES.cpp |  |
| `AttachmentIsDepthSlice(const MG_State::GLState::FramebufferAttachmentObject& attachment)` | 4 | gone | 0 | A 3D image has arrayLayers == 1: its "layer" is a z slice, which has to travel as an |
| `IsColorAttachment(FramebufferAttachmentType attachmentType)` | 4 | gone | 0 |  |
| `IsCubeMapFaceUploadTarget(TextureUploadTarget target)` | 4 | gone | 2: VkClearManager.cpp, VkTextureManager.cpp |  |
| `ColorFormatLacksAlpha(const MG_State::GLState::ITextureObject* texture)` | 3 | gone | 0 | GL 4.6 core 15.2.3: a colour format with no alpha channel reads as if alpha were one. |
| `CombinePipelineStateWord(Uint64 hash, Uint64 word)` | 3 | gone | 0 | Boost-style hash combine. The inputs are tiny enum ordinals and bit masks, so |
| `RecordClearBufferError(const char* func, ErrorCode code, const char* message)` | 3 | gone | 0 |  |
| `VulkanRenderer::IsDrawIndirectCountExtensionEnabled()` | 3 | gone | 0 |  |

## `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.h`

OLD defs: 55, NEW defs: 52, deleted: 10 (gone 10, params differ 0).

| name | body lines | status | HEAD elsewhere (token) | comment |
|---|---|---|---|---|
| `class SetupDrawSnapshot` | 99 | gone | 1: VulkanRenderer.cpp | Snapshot behind TrySetupDrawFastPath. Values only: the program and |
| `class ResolvedVertexBindings` | 73 | gone | 0 | One VAO's resolved vkCmdBindVertexBuffers arguments, reusable by a later draw |
| `class ConvertedVertexStreamKey` | 19 | gone | 0 |  |
| `class ConvertedVertexStreamKeyHash` | 17 | gone | 0 |  |
| `ConvertedVertexStreamKeyHash::operator()(const ConvertedVertexStreamKey& key)` | 15 | gone | 0 |  |
| `class ConvertedVertexStream` | 11 | gone | 0 |  |
| `ConvertedVertexStreamKey::operator==(const ConvertedVertexStreamKey& other)` | 6 | gone | 0 |  |
| `VulkanRenderer::InvalidateSetupDrawSnapshots()` | 5 | gone | 0 |  |
| `VulkanRenderer::ResolveVaoHandle(const MG_State::GLState::VertexArrayObject& vao)` | 4 | gone | 0 | The VAO's {slot, gen}. A one-entry memo hit for every acquisition after a draw's |
| `VulkanRenderer::VaoContentHashIfKnown(const MG_State::GLState::VertexArrayObject& vao, Uint64& outHash)` | 3 | gone | 0 | "Is this VAO's content hash already memoized?", asked of whichever side owns the |

## `MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.cpp`

OLD defs: 63, NEW defs: 41, deleted: 25 (gone 23, params differ 2).

| name | body lines | status | HEAD elsewhere (token) | comment |
|---|---|---|---|---|
| `UniformManager::BindProgramUniformBuffers(VkCommandBuffer commandBuffer, const MagmaProgramSource& program, const Pro...` | 410 | params differ | 5: ProgramFactory.cpp, ProgramFactory.h, ... |  |
| `UniformManager::ResolveSamplerDescriptor(VkCommandBuffer commandBuffer, const MagmaProgramSource& program, const Prog...` | 244 | gone | 1: UniformManager.h |  |
| `UniformManager::ResolveWireImageDescriptor(VkCommandBuffer commandBuffer, const MagmaProgramSource& program, const Pr...` | 167 | params differ | 1: UniformManager.h |  |
| `UniformManager::CollectSamplerImageFeedback(const MagmaProgramSource& program, const ProgramFactory::VkProgramObject&...` | 78 | gone | 0 |  |
| `UniformManager::CollectStorageImageTextures(const MagmaProgramSource& program, const ProgramFactory::VkProgramObject&...` | 76 | gone | 0 |  |
| `UniformManager::GetFallbackMultisampleTexture(TextureTarget target, SamplerNumericDomain numericDomain)` | 75 | gone | 0 |  |
| `UniformManager::ResolveSampledBinding(const MagmaProgramSource& program, const ProgramFactory::VkProgramObject& progr...` | 64 | gone | 0 |  |
| `UniformManager::ProgramSamplesOnlySingleLevelTextures(const MagmaProgramSource& program, const ProgramFactory::VkProg...` | 59 | gone | 0 |  |
| `UniformManager::CollectSampledTextures(const MagmaProgramSource& program, const ProgramFactory::VkProgramObject& prog...` | 46 | gone | 0 |  |
| `UniformManager::GetUnboundStorageImageTexture(TextureTarget target, VkFormat format)` | 43 | gone | 1: ProgramFactory.h |  |
| `UniformManager::SampledBindingsUnchanged(const MagmaProgramSource& program, const ProgramFactory::VkProgramObject& pr...` | 42 | gone | 0 |  |
| `UniformManager::GetFallbackTexture(TextureTarget target, SamplerNumericDomain numericDomain)` | 39 | gone | 0 |  |
| `PlaceholderShapeForTarget(TextureTarget target)` | 37 | gone | 0 |  |
| `UniformManager::ResolveSamplerDescriptorOverride(const SamplerBindingOverride& samplerBindingOverride, VkDescriptorIm...` | 37 | gone | 0 |  |
| `UniformManager::ResolveSamplerTextureRaw(const MagmaProgramSource& program, const ProgramFactory::VkProgramObject& pr...` | 25 | gone | 0 |  |
| `FindFramebufferAttachmentForTexture(const MG_State::GLState::FramebufferObject& framebuffer, const MG_State::GLState:...` | 24 | gone | 0 |  |
| `UniformManager::ResolveSamplerTexture(const MagmaProgramSource& program, const ProgramFactory::VkProgramObject& progr...` | 23 | gone | 0 |  |
| `MakePlaceholderTextureObject(TextureTarget target, Uint index)` | 22 | gone | 0 | TextureObjectMipmap, not ITextureObject: AllocateStorage and MarkStorageDirty live |
| `UniformManager::ResolveUnboundStorageImagePlaceholder(const ProgramFactory::VkProgramObject& programObj, Uint32 bindi...` | 16 | gone | 0 |  |
| `InternalFormatForVkFormat(VkFormat format)` | 13 | gone | 0 | Reverse of MG_Util::ConvertTextureInternalFormatToVkEnum. A placeholder texture is |
| `IsValidSampledImageLayout(VkImageLayout layout)` | 12 | gone | 0 |  |
| `ResolveSamplerUnitIndex(const MagmaProgramSource& program, Int location, Uint32 binding)` | 11 | gone | 0 |  |
| `class PlaceholderShape` | 8 | gone | 0 | What a 1x1 placeholder of a given target has to allocate for the backend to give it the |
| `IsPlaceholderTexture(const MG_State::GLState::ITextureObject* texture)` | 7 | gone | 0 | MobileGL's own stand-in textures, by the reserved ids above. Nothing an application can |
| `ResolveDescriptorElementLocation(const MagmaProgramSource& program, Int baseLocation, Uint32 element)` | 7 | gone | 0 | Uniform location of ELEMENT `element` of the opaque-uniform array at `baseLocation`, or |

## `MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.h`

OLD defs: 19, NEW defs: 19, deleted: 2 (gone 2, params differ 0).

| name | body lines | status | HEAD elsewhere (token) | comment |
|---|---|---|---|---|
| `class FastRebindMemo` | 13 | gone | 0 | Dynamic-offset-only rebind (see BindProgramUniformBuffers): records the |
| `class SamplerBindingOverride` | 9 | gone | 0 |  |

## `MobileGL/MG_Backend/DirectVulkan/Renderer/VkTextureManager.cpp`

OLD defs: 104, NEW defs: 55, deleted: 51 (gone 50, params differ 1).

| name | body lines | status | HEAD elsewhere (token) | comment |
|---|---|---|---|---|
| `VkTextureManager::UploadDirtyMipLevels(MG_State::GLState::TextureObjectMipmap &mipmapTexture, TextureUploadTarget upl...` | 524 | gone | 0 |  |
| `VkTextureManager::SyncTextureResource(const MG_State::GLState::ITextureObject &texture, TextureUploadTarget uploadTar...` | 416 | gone | 4: BackendObject_DirectVulkan.cpp, VkTextureManager.h, ... |  |
| `VkTextureManager::SnapshotTextureForSampling(VkCommandBuffer commandBuffer, MG_State::GLState::ITextureObject& textur...` | 148 | gone | 0 |  |
| `VkTextureManager::SyncTexture(MG_State::GLState::ITextureObject &texture, TextureResource &outResource)` | 117 | gone | 1: VkTextureManager.h |  |
| `VkTextureManager::GetOrCreateStorageImageView(MG_State::GLState::ITextureObject& texture, Uint32 mipLevel, VkFormat f...` | 112 | gone | 0 |  |
| `VkTextureManager::SyncTextureAndGetDescriptor(MG_State::GLState::ITextureObject& textureOrView)` | 108 | gone | 2: VkTextureManager.h, VulkanRenderer.cpp |  |
| `PreserveTextureContentsOnRecreate(VkDevice device, VkCommandPool commandPool, VkQueue graphicsQueue, const VkTextureM...` | 106 | params differ | 0 |  |
| `TryResolveTextureShapeInfo(const MG_State::GLState::ITextureObject& texture, TextureUploadTarget uploadTarget, const ...` | 102 | gone | 1: VulkanRenderer.cpp |  |
| `VkTextureManager::GetOrCreateAttachmentViewAtMipLevel(MG_State::GLState::ITextureObject& texture, Uint32 mipLevel, Ui...` | 91 | gone | 0 |  |
| `VkTextureManager::GetOrCreateSampledImageView(MG_State::GLState::ITextureObject& texture, VkFormat format)` | 83 | gone | 0 |  |
| `VkTextureManager::SyncTextureViews(const MG_State::GLState::ITextureObject& texture, TextureResource& resource)` | 56 | gone | 1: VkTextureManager.h |  |
| `VkTextureManager::ResolveTextureViewWindow(MG_State::GLState::ITextureObject& texture, const TextureResource& resource)` | 55 | gone | 0 |  |
| `VkTextureManager::UpdateTrackedImageLayoutAfterAttachmentWrite(VkCommandBuffer commandBuffer, MG_State::GLState::ITex...` | 52 | gone | 1: VkTextureManager.h |  |
| `VkTextureManager::TransitionTextureForSampling(VkCommandBuffer commandBuffer, MG_State::GLState::ITextureObject& text...` | 49 | gone | 0 |  |
| `VkTextureManager::GetOrCreateSampledViewAtMipLevel(MG_State::GLState::ITextureObject& texture, Uint32 mipLevel)` | 44 | gone | 0 |  |
| `VkTextureManager::GetOrCreateWindowedSampledView(MG_State::GLState::ITextureObject& texture, TextureResource& resourc...` | 41 | gone | 0 | Builds (and caches) one sampled VkImageView over `resource`'s image for an arbitrary |
| `VkTextureManager::NoteTextureViewImageRequirements(MG_State::GLState::ITextureObject& viewTexture, MG_State::GLState:...` | 41 | gone | 0 | The extra VkImageCreateFlags a GL texture view needs on the image it views. Recorded |
| `VkTextureManager::CheckMipmapCompleteness(const MG_State::GLState::ITextureObject& texture, TextureUploadTarget& outT...` | 36 | gone | 0 |  |
| `VkTextureManager::GetOrCreateViewAtMipLevel(MG_State::GLState::ITextureObject& texture, Uint32 mipLevel)` | 36 | gone | 0 |  |
| `VkTextureManager::TransitionTextureForStorageImage(VkCommandBuffer commandBuffer, MG_State::GLState::ITextureObject& ...` | 30 | gone | 0 |  |
| `VkTextureManager::ResolveViewMipRange(const MG_State::GLState::ITextureObject& texture, Uint32 mipLevels, Uint32& out...` | 28 | gone | 0 |  |
| `VkTextureManager::CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect, VkImageViewType viewType...` | 27 | gone | 0 |  |
| `VkTextureManager::NeedsStorageImagePreparation(MG_State::GLState::ITextureObject& texture)` | 25 | gone | 1: VkTextureManager.h |  |
| `class UploadItem` | 24 | gone | 0 |  |
| `VkTextureManager::MakeTextureIdentity(MG_State::GLState::ITextureObject* texture)` | 22 | gone | 0 |  |
| `ResolveTextureViewImageViewType(TextureTarget target, VkImageViewType storageViewType)` | 21 | gone | 0 | The VkImageViewType a GL texture view's own target asks for. Deliberately derived from the |
| `VkTextureManager::PruneStaleTextureAliases(MG_State::GLState::ITextureObject* texture)` | 20 | gone | 0 |  |
| `ToVkComponentSwizzle(TextureSwizzleParam swizzle)` | 19 | gone | 0 |  |
| `VkTextureManager::GetUploadMipLevelCount(const MG_State::GLState::TextureObjectMipmap& texture, TextureUploadTarget t...` | 17 | gone | 0 |  |
| `VkTextureManager::DeferViewRelease(VkImageView view)` | 15 | gone | 0 |  |
| `VkTextureManager::EraseTrackedTexture(const TextureIdentity& identity)` | 14 | gone | 0 |  |
| `VkTextureManager::NeedsMipChainGrowth(MG_State::GLState::ITextureObject& texture)` | 13 | gone | 0 |  |
| `IsValidSampledImageLayout(VkImageLayout layout)` | 12 | gone | 0 |  |
| `VkTextureManager::AppendViewRequestedFormats(const MG_State::GLState::ITextureObject& storageTexture, Vector<VkFormat...` | 12 | gone | 0 |  |
| `ResolveSampledViewComponents(const MG_State::GLState::ITextureObject& texture, const TextureFormatInfo& formatInfo)` | 11 | gone | 0 |  |
| `VkTextureManager::NeedsStorageUsageUpgrade(MG_State::GLState::ITextureObject& texture)` | 11 | gone | 0 |  |
| `VkTextureManager::HasPendingTexelUpload(MG_State::GLState::ITextureObject& textureOrView)` | 9 | gone | 0 |  |
| `VkTextureManager::StampTextureRecordingUse(MG_State::GLState::ITextureObject* texture)` | 9 | gone | 0 |  |
| `VkTextureManager::StampTextureRecordingWrite(MG_State::GLState::ITextureObject* texture)` | 9 | gone | 0 |  |
| `VkTextureManager::UpdateTrackedImageLayout(MG_State::GLState::ITextureObject* texture, VkImageLayout newLayout)` | 9 | gone | 0 |  |
| `VkTextureManager::AreStorageImageViewFormatsCompatible(VkFormat imageFormat, VkFormat viewFormat)` | 7 | gone | 0 |  |
| `IsMultisampleTextureUploadTarget(TextureUploadTarget target)` | 6 | gone | 0 |  |
| `ToVkSampledComponentSwizzle(TextureSwizzleParam swizzle, Bool alphaIsImplicitOne)` | 6 | gone | 0 |  |
| `VkTextureManager::GetViewRequestedImageFlags(const MG_State::GLState::ITextureObject& storageTexture)` | 5 | gone | 0 |  |
| `VkTextureManager::BeginDrawSyncScope()` | 4 | gone | 0 |  |
| `VkTextureManager::EndDrawSyncScope()` | 4 | gone | 0 |  |
| `VkTextureManager::FindTextureResource(MG_State::GLState::ITextureObject& textureOrView)` | 4 | gone | 0 |  |
| `VkTextureManager::HasLiveImage(MG_State::GLState::ITextureObject& textureOrView)` | 4 | gone | 0 |  |
| `VkTextureManager::StorageTextureOf(MG_State::GLState::ITextureObject& texture)` | 4 | gone | 0 |  |
| `VkTextureManager::WasTextureWrittenThisRecording(MG_State::GLState::ITextureObject& textureOrView)` | 4 | gone | 0 |  |
| `VkTextureManager::MarkStorageImageTexture(MG_State::GLState::ITextureObject& texture)` | 3 | gone | 0 |  |

## `MobileGL/MG_Backend/DirectVulkan/Renderer/VkTextureManager.h`

OLD defs: 48, NEW defs: 37, deleted: 12 (gone 12, params differ 0).

| name | body lines | status | HEAD elsewhere (token) | comment |
|---|---|---|---|---|
| `class DrawSyncScope` | 9 | gone | 0 | RAII guard that opens/closes a per-draw sync memo window (see above). |
| `class TextureIdentity` | 8 | gone | 0 |  |
| `class TextureIdentityHash` | 7 | gone | 0 |  |
| `class SyncedTextureMemoEntry` | 6 | gone | 0 | Cross-draw sampled-texture memo: the same few textures (atlas, lightmap) |
| `TextureIdentityHash::operator()(const TextureIdentity& key)` | 5 | gone | 0 |  |
| `VkTextureManager::PackComponentSwizzle(const VkComponentMapping& components)` | 5 | gone | 0 | The four component swizzles packed into one value, for the sampled-view cache key. |
| `ToStorageArrayLayer(const MG_State::GLState::ITextureObject* texture, Int glLayer)` | 4 | gone | 0 |  |
| `ToStorageMipLevel(const MG_State::GLState::ITextureObject* texture, Int glLevel)` | 4 | gone | 0 | A GL framebuffer attachment's level/layer, and a GL image unit's, are relative to the texture |
| `class DrawSyncedTexture` | 4 | gone | 0 | Per-draw sync memo: the identity plus the resolved resource pointer. The pointer is stable |
| `TextureIdentity::operator==(const TextureIdentity& other)` | 3 | gone | 0 |  |
| `DrawSyncScope::DrawSyncScope(VkTextureManager& manager)` | 1 | gone | 0 |  |
| `DrawSyncScope::~DrawSyncScope()` | 1 | gone | 0 |  |

## `MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.cpp`

OLD defs: 78, NEW defs: 53, deleted: 25 (gone 25, params differ 0).

| name | body lines | status | HEAD elsewhere (token) | comment |
|---|---|---|---|---|
| `VkBufferManager::AcquireStreamedSlice(BufferKind kind, const SharedPtr<MG_State::GLState::BufferObject>& bufferObject...` | 85 | gone | 0 |  |
| `VkBufferManager::AcquirePersistentMap(MG_State::GLState::BufferObject& bufferObject)` | 56 | gone | 2: Managers.cpp, Managers.h |  |
| `VkBufferManager::AcquireResidentSlice(BufferKind kind, const SharedPtr<MG_State::GLState::BufferObject>& bufferObject...` | 49 | gone | 0 |  |
| `VkBufferManager::StagedRangeCopy(VkBufferResource& resource, const void* data, SizeT offset, SizeT size)` | 46 | gone | 0 |  |
| `VkBufferManager::OnRespecify(MG_State::GLState::BufferObject& bufferObject)` | 43 | gone | 1: VkBufferManager.h |  |
| `VkBufferManager::OnSubData(MG_State::GLState::BufferObject& bufferObject, SizeT offset, SizeT size)` | 36 | gone | 0 |  |
| `VkBufferManager::OnFlushMappedRange(MG_State::GLState::BufferObject& bufferObject, Range1D range, Flags<BufferMapping...` | 35 | gone | 0 |  |
| `VkBufferManager::GetVkBufferUsage(BufferKind kind)` | 27 | gone | 0 |  |
| `VkBufferManager::CreateResidentStorage(VkBufferResource& resource, VkDeviceSize size, VkBufferUsageFlags usage, VkMem...` | 26 | gone | 0 |  |
| `VkBufferManager::OnResidentSubData(MG_State::GLState::BufferObject& bufferObject, SizeT offset, DataPtr data)` | 19 | gone | 0 |  |
| `VkBufferManager::SwapStorageAndUploadAll(VkBufferResource& resource, MG_State::GLState::BufferObject& bufferObject)` | 19 | gone | 0 |  |
| `VkBufferManager::OnResourceDestroyed(SharedPtr<MG_State::GLState::BackendBufferResource>&& resource)` | 18 | gone | 0 |  |
| `VkBufferManager::ReleaseAllLiveResources()` | 15 | gone | 0 |  |
| `VkBufferManager::DeferRelease(VkBufferObject&& buffer)` | 14 | gone | 1: VkBufferManager.h |  |
| `VkBufferManager::GetOrCreateResource(const SharedPtr<MG_State::GLState::BufferObject>& bufferObject)` | 14 | gone | 0 |  |
| `VkBufferManager::TrackLiveResource(const SharedPtr<VkBufferResource>& resource)` | 13 | gone | 0 |  |
| `Ops_OnDestroy(SharedPtr<BackendBufferResource>&& resource)` | 9 | gone | 1: Managers.cpp |  |
| `Ops_ReadbackFromGpu(BufferObject& bufferObject)` | 7 | gone | 0 | The CPU is about to read a buffer a shader wrote. Its bytes live in coherent |
| `Ops_AcquirePersistentMap(BufferObject& bufferObject)` | 6 | gone | 1: Managers.cpp |  |
| `Ops_FlushMappedRange(BufferObject& bufferObject, Range1D range, Flags<BufferMappingAccessBit> appAccess)` | 5 | gone | 0 |  |
| `Ops_ResidentSubData(BufferObject& bufferObject, SizeT offset, DataPtr data)` | 5 | gone | 0 |  |
| `Ops_Respecify(BufferObject& bufferObject)` | 5 | gone | 0 |  |
| `Ops_SubData(BufferObject& bufferObject, SizeT offset, SizeT size)` | 5 | gone | 0 |  |
| `VkBufferManager::IsResourceBusy(const VkBufferResource& resource)` | 3 | gone | 1: VkBufferManager.h |  |
| `VkBufferManager::ResourceOf(MG_State::GLState::BufferObject& bufferObject)` | 3 | gone | 0 |  |

## `MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.h`

OLD defs: 13, NEW defs: 13, deleted: 3 (gone 3, params differ 0).

| name | body lines | status | HEAD elsewhere (token) | comment |
|---|---|---|---|---|
| `class VkBufferResource` | 46 | gone | 2: BufferArena.cpp, VkBufferManager.cpp | The DirectVulkan storage behind one frontend buffer (pipe_resource analogue). |
| `VkBufferManager::BumpSliceEpoch(VkBufferResource& resource)` | 1 | gone | 1: VkBufferManager.cpp | See VkBufferResource::sliceEpoch. |
| `VkBufferManager::GetSliceEpochCounter()` | 1 | gone | 0 | Highest value handed to any VkBufferResource::sliceEpoch. Unchanged since a |

## `MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.cpp`

OLD defs: 14, NEW defs: 10, deleted: 7 (gone 7, params differ 0).

| name | body lines | status | HEAD elsewhere (token) | comment |
|---|---|---|---|---|
| `VertexInputStateFactory::GetOrCreateVertexInputState(const MG_State::GLState::VertexArrayObject& vao, HashType hash)` | 218 | gone | 0 |  |
| `VertexInputStateFactory::ComputeHash(const MG_State::GLState::VertexArrayObject& vao)` | 72 | gone | 5: PipelineFactory.cpp, PipelineFactory.h, ... |  |
| `VertexInputStateFactory::GetOrCreateVertexInputState(const MG_State::GLState::VertexArrayObject& vao)` | 32 | gone | 0 |  |
| `VertexInputStateFactory::OnFrameBoundary()` | 30 | gone | 8: PipelineFactory.cpp, PipelineFactory.h, ... |  |
| `VertexInputStateFactory::MemosFor(const MG_State::GLState::VertexArrayObject& vao)` | 18 | gone | 0 |  |
| `VertexInputStateFactory::GetOrComputeHash(const MG_State::GLState::VertexArrayObject& vao)` | 15 | gone | 0 |  |
| `VertexInputStateFactory::TryGetMemoizedHash(const MG_State::GLState::VertexArrayObject& vao, Uint64& outHash)` | 9 | gone | 0 |  |

## `MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.h`

OLD defs: 5, NEW defs: 4, deleted: 3 (gone 2, params differ 1).

| name | body lines | status | HEAD elsewhere (token) | comment |
|---|---|---|---|---|
| `class VaoBackendMemos` | 11 | gone | 0 | ---- P2 D12.5: the backend's memos, off the frontend VAO and into the backend ---- |
| `VertexInputStateFactory::PackVertexInputAuxMasks(Uint32 unsupportedAttribMask, Uint32 attributeLocationMask)` | 3 | gone | 0 | The VAO aux-memo payload GetOrCreateVertexInputState(vao) stamps: aux0 is the |
| `VertexInputStateFactory::VertexInputStateFactory(const VulkanRendererConfig& config, VkPhysicalDevice physicalDevice,...` | 1 | params differ | 6: BackendObject.h, ProgramFactory.cpp, ... | The mint is the RENDERER's (MagmaPipeIdentityTables), not a process-global and not |

## `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp`

OLD defs: 510, NEW defs: 458, deleted: 53 (gone 50, params differ 3).

| name | body lines | status | HEAD elsewhere (token) | comment |
|---|---|---|---|---|
| `GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void* pixels)` | 417 | gone | 6: BackendObject.h, Managers.h, ... |  |
| `ScopedDetachedTextureFramebufferAttachments::ScopedDetachedTextureFramebufferAttachments(const SharedPtr<MG_State::GL...` | 166 | params differ | 2: Managers.cpp, Managers.h |  |
| `BlitLayeredDestinationAspects(const SharedPtr<MG_State::GLState::FramebufferObject>& readFramebuffer, const SharedPtr...` | 136 | params differ | 0 | ---- glBlitFramebuffer onto a non-zero array layer ------------------------------------- |
| `SyncCurrentProgram(const SharedPtr<MG_State::GLState::ProgramObject>& currentProgram)` | 116 | gone | 2: Managers.cpp, Utils.h |  |
| `GetTexImageViaShadowConversion(MG_State::GLState::TextureObjectMipmap* textureMipmapObject, TextureUploadTarget uploa...` | 100 | gone | 1: WireTextureReadback.inc | GetTexImage fallback for internal formats the ES driver cannot attach to a framebuffer |
| `SyncVaoAttributeBuffersByHandle(const SharedPtr<MG_State::GLState::VertexArrayObject>& currentVAOObject, VertexArrayI...` | 86 | gone | 0 | The handle arm of the resolved-draw-buffers memo (D-G4). Two substitutions and |
| `SyncTextureObjectToBackend(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, Bool imageBindableStora...` | 80 | gone | 3: Managers.cpp, Managers.h, ... |  |
| `SyncReadFramebufferTextureAttachments(const DrawTextureSyncKeys& keys, const MG_State::GLState::FramebufferObject* dr...` | 74 | gone | 0 |  |
| `GenerateThreeChannelFloatMipmapOnCpu(const SharedPtr<MG_State::GLState::ITextureObject>& texture)` | 69 | gone | 1: Managers.cpp | ES has no colour-renderable three-channel float format, and its glGenerateMipmap |
| `SamplerUniformTextureTarget(GLenum uniformType)` | 59 | gone | 0 | Frontend texture target a GLSL sampler uniform samples from. Only used to find |
| `SyncClientSideVertexArraysForFetch(Uint8 indexSize, Uint32 elementStart, GLsizei count, GLsizei instanceCount, GLint ...` | 53 | gone | 0 |  |
| `GenerateMipmapThroughViewWindowForTexture(const SharedPtr<MG_State::GLState::ITextureObject>& texture)` | 47 | gone | 0 | The monolith twin of the record arm's view-window rule. Same window, read off the frontend |
| `SyncBufferBindingPoints(BufferTarget target, GLenum glTarget)` | 45 | gone | 1: Managers.cpp |  |
| `ResolveUnitSamplerBackend(Int unit, const SharedPtr<MG_State::GLState::SamplerObject>& samplerObject)` | 44 | gone | 0 |  |
| `BlitNamedFramebuffer(const SharedPtr<MG_State::GLState::FramebufferObject>& readFramebuffer, const SharedPtr<MG_State...` | 43 | gone | 4: BackendObject.h, Managers.h, ... |  |
| `EnsureGenerateMipmapStorageAllocated(MG_State::GLState::TextureObjectMipmap& texture, TextureUploadTarget uploadTarge...` | 38 | gone | 0 |  |
| `SyncTransformFeedbackBindingPoints(SizeT bufferCount)` | 33 | gone | 0 | The capture points the CAPTURE PROGRAM uses, and nothing else. |
| `SyncAndBindFramebufferObject(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, FramebufferTarget ta...` | 32 | gone | 0 |  |
| `MarkShaderStorageBuffersGpuWritten()` | 31 | gone | 0 | Called once the storage-buffer points are bound and the draw/dispatch is about to |
| `GenerateDepthTexture2DMipmap(const SharedPtr<MG_State::GLState::ITextureObject>& texture, const SharedPtr<TextureImpl...` | 28 | gone | 0 |  |
| `GetBackendProgramId(GLuint program)` | 28 | gone | 1: Managers.h | P5f (fr): no declaration, dispatch-table slot or caller remains for this legacy |
| `GenerateColorTexture2DMipmap(const SharedPtr<MG_State::GLState::ITextureObject>& texture, const SharedPtr<TextureImpl...` | 27 | gone | 0 |  |
| `SyncImageTextureBinding(Uint unit)` | 26 | params differ | 1: Utils.h | The pre-handle form: the MONOLITH one, and now purely frontend. P4a e3 read the four |
| `ResolveBufferBindingSubsystemArm()` | 23 | gone | 0 | ---- P5e (sb, MG_Remote/CONTRACT-P5E.md §5.6): THE INDEXED BINDING POINTS BY RECORD -- |
| `ClearNamedFramebufferfv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer, GLint draw...` | 18 | gone | 1: BackendObject.h |  |
| `ClearNamedFramebufferiv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer, GLint draw...` | 18 | gone | 1: BackendObject.h |  |
| `ClearNamedFramebufferuiv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer, GLint dra...` | 18 | gone | 1: BackendObject.h |  |
| `ResolveVaoTwin(const SharedPtr<MG_State::GLState::VertexArrayObject>& vao)` | 18 | gone | 2: Managers.cpp, Managers.h | Resolve-or-create the VAO's backend twin, once per draw: PrepareForDraw passes |
| `SyncBoundBuffer(BufferTarget target, GLenum glTarget)` | 17 | gone | 0 |  |
| `ClearNamedFramebufferfi(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer, GLint draw...` | 15 | gone | 1: BackendObject.h |  |
| `RefuseElementArrayBufferFromTheFrontend(const char* entry)` | 15 | gone | 1: MultiDraw.cpp | MONOLITH GLUE, AND LOUD ABOUT IT AS OF P5e (vi). CONTRACT-P5E §5.1 makes the restart |
| `SyncRenderbufferObjectToBackend(const SharedPtr<MG_State::GLState::RenderbufferObject>& renderbufferObject)` | 15 | gone | 0 | The renderbuffer twin of TextureImpl::SyncTextureObjectToBackend: the same |
| `TextureLayerCount(const SharedPtr<MG_State::GLState::ITextureObject>& texture)` | 15 | gone | 0 | Where a target keeps its LAYER count, the state-side twin of the wire descriptor's |
| `EnsureGenerateMipmapStorageAllocated(const SharedPtr<MG_State::GLState::ITextureObject>& texture)` | 14 | gone | 0 |  |
| `IsNativeGetTexImagePair(GLenum format, GLenum type)` | 14 | gone | 0 | Combinations the ES driver has always handled directly for GetTexImage; everything else that maps |
| `ResolveGlobalConstantsRecord(const MG_State::GLState::ProgramObject* program)` | 14 | gone | 0 | THE MONOLITH-GLUE HALF (ruling 1 / ID-81), unchanged in what it does: resolve the |
| `IsWidenedNamedDrawBuffer(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer, GLint dra...` | 13 | gone | 0 | The same question for an explicitly named framebuffer (the DSA clears), which is NOT |
| `SyncCurrentVAO(const SharedPtr<MG_State::GLState::VertexArrayObject>& currentVAOObject, BackendVertexArrayObject* vao...` | 13 | gone | 1: Managers.cpp |  |
| `ComputeFullMipmapLevelCount(const IntVec3& baseTexelSize)` | 11 | gone | 0 |  |
| `ReadClientSnapshotBytes(void* user, MG_Pipe::MGPipeClientBufferKind kind, Uint64 offset, SizeT size, void* destination)` | 10 | gone | 0 |  |
| `VaoHasEnabledClientArray(const MG_State::GLState::VertexArrayObject& vao, Bool* vertexRate = nullptr)` | 10 | gone | 0 | The draw's own fetch, spelled as the range record spells it: `indexSize` 0 is arrays, |
| `BoundElementArrayBuffer()` | 7 | gone | 0 |  |
| `BoundElementArrayBufferId()` | 7 | gone | 0 | The GL name PrepareForDraw left on GL_ELEMENT_ARRAY_BUFFER, i.e. what the |
| `ComputeMipmapTexelSize(const IntVec3& baseTexelSize, Uint relativeLevel)` | 7 | gone | 0 |  |
| `DrawProgramFromRecords()` | 7 | gone | 0 | P5e (pa), CONTRACT-P5E §5.5 / §5.8, ruling ID-81: "IS THERE ANYBODY LEFT WHO NEEDS THE |
| `MultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices, GLsizei drawcount)` | 6 | gone | 5: BackendObject.h, DirectVulkan.cpp, ... | Both glMultiDrawElements entry points are emulated - ES has neither in core - by the |
| `PairingsIntact(const Vector<UnitTextureSyncEntry>& list)` | 6 | gone | 0 | True while every entry's borrowed slot still holds the texture the entry was paired |
| `StampSyncedFBO(FramebufferTarget target, Uint16 slotVersion, Uint16 objectVersion, MG_State::GLState::FramebufferObje...` | 6 | gone | 1: Managers.h | Record that `target` now reflects this exact (binding, object, revision) triple. |
| `BindingPointsComeFromRecords() (x2)` | 4 | gone | 0 | FILE-LOCAL AND INLINE-MEMOISED, for EsprytSlotTablesEnabled's reason: this is |
| `IsWritableImageBufferTexture(const MG_State::GLState::ImageTextureBinding& binding)` | 4 | gone | 0 |  |
| `UnitTexturesByHandle()` | 4 | gone | 0 | P5e (tx2), CONTRACT-P5E §5.8 / ruling 1: THE ARM SELECTOR for every by-handle site in |
| `FramebufferRecordArmIsMandatory()` | 3 | gone | 0 | P5e (fb, CONTRACT-P5E.md §5.4 and §0's rule F). THE DECLINE BECOMES THE DETECTOR. |
| `AreOcclusionQueriesSupported()` | 1 | gone | 0 |  |

## `MobileGL/MG_Backend/DirectGLES/Managers.cpp`

OLD defs: 398, NEW defs: 354, deleted: 49 (gone 48, params differ 1).

| name | body lines | status | HEAD elsewhere (token) | comment |
|---|---|---|---|---|
| `BackendFramebufferObject::SyncToBackend(const SharedPtr<MG_State::GLState::FramebufferObject>& stateFBOObject, Frameb...` | 383 | gone | 2: DirectGLES.cpp, Managers.h |  |
| `BackendTextureObject::RequireImageBindableStorage(const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObj...` | 158 | gone | 1: Managers.h |  |
| `EnsureBufferResource(const SharedPtr<MG_State::GLState::BufferObject>& bufferObject)` | 137 | gone | 3: DirectGLES.cpp, Managers.h, ... |  |
| `OnFrontendStateObjectDestroyed(MG_Pipe::MGPipeKind kind, Uint64 lifetimeId)` | 119 | params differ | 0 | P2 step e2's dispatcher: the frontend told us an object died, so free its slot and |
| `BackendRenderbufferObject::SyncToBackend(const SharedPtr<MG_State::GLState::RenderbufferObject>& stateRBOObject)` | 117 | gone | 2: DirectGLES.cpp, Managers.h |  |
| `BackendTextureObject::SyncTextureViewToBackend(const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject)` | 91 | gone | 1: BackendObject_DirectGLES.cpp |  |
| `SyncAttachmentObject(GLenum glFBOTarget, const MG_State::GLState::FramebufferAttachmentObject& attachmentObject, GLen...` | 86 | gone | 2: BackendObject_DirectGLES.cpp, DirectGLES.cpp |  |
| `BackendVertexArrayObject::SyncClientSideAttributesForDraw(const SharedPtr<MG_State::GLState::VertexArrayObject>& stat...` | 83 | gone | 1: Managers.h | THE CLIENT-ARRAY UPLOAD FOR A DRAW WHOSE FETCHED ELEMENTS (first, count) DOES NOT |
| `Ops_AcquirePersistentMap(BufferObject& bufferObject)` | 78 | gone | 0 | Zero-copy persistent map: back the buffer with real immutable, |
| `BackendFramebufferObject::SyncReadBufferToBackend(const SharedPtr<MG_State::GLState::FramebufferObject>& stateFBOObject)` | 63 | gone | 2: DirectGLES.cpp, Managers.h |  |
| `Ops_FlushMappedRange(BufferObject& bufferObject, Range1D range, Flags<BufferMappingAccessBit> appAccess)` | 63 | gone | 0 |  |
| `Ops_Respecify(BufferObject& bufferObject)` | 55 | gone | 0 |  |
| `ResolveVertexInputSubsystemArm()` | 47 | gone | 1: Managers.h |  |
| `BackendProgramObjectImpl::SyncToBackend(const SharedPtr<MG_State::GLState::ProgramObject>& stateProgramObject)` | 42 | gone | 2: DirectGLES.cpp, Managers.h | ---- the two public heads (P5e pg) ----------------------------------------------- |
| `Ops_ReadbackFromGpu(BufferObject& bufferObject)` | 37 | gone | 0 | A shader wrote this buffer through a storage/atomic-counter binding, so the ES |
| `Ops_SubData(BufferObject& bufferObject, SizeT offset, SizeT size)` | 34 | gone | 0 |  |
| `MarkBufferGpuWritten(const SharedPtr<MG_State::GLState::BufferObject>& bufferObject)` | 32 | gone | 2: DirectGLES.cpp, Managers.h |  |
| `SyncZeroStrideAttribute(Uint attribIndex, const MG_State::GLState::VertexAttribute& attrib)` | 31 | gone | 0 | Declares one attribute through the ES binding-point API, the only spelling that can |
| `HandleOfBuffer(const MG_State::GLState::BufferObject* bufferObject)` | 29 | gone | 1: DirectGLES.cpp |  |
| `IsBufferDrawClean(const MG_State::GLState::BufferObject* frontend, const GLESBufferResource* resource)` | 25 | gone | 2: DirectGLES.cpp, Managers.h |  |
| `ProgramArchiveSource::FromFrontend(const MG_State::GLState::ProgramObject& program)` | 24 | gone | 0 |  |
| `GetOrCreateSamplerViewForHandle(MG_Pipe::MGPipeHandle view)` | 23 | gone | 0 |  |
| `ComputeShaderStorageBlockBindingSignatureOf(const MG_State::GLState::ProgramObject& program)` | 22 | gone | 0 | The COMPUTATION, still here and still the only one on this side: FromFrontend seeds a |
| `BackendVertexArrayObject::SyncToBackend(const SharedPtr<MG_State::GLState::VertexArrayObject>& stateVAOObject)` | 21 | gone | 2: DirectGLES.cpp, Managers.h |  |
| `ResolveResourceSubsystemArm()` | 21 | gone | 0 |  |
| `BindAttributeBuffer(const MG_State::GLState::VertexAttribute& attrib)` | 16 | gone | 0 |  |
| `ComputeAlphaWidenedDrawBufferMask(const MG_State::GLState::FramebufferObject& fbo)` | 16 | gone | 0 |  |
| `IsSnormFallbackAttachment(const MG_State::GLState::FramebufferAttachmentObject& attachmentObject)` | 14 | gone | 0 |  |
| `IsUnormFallbackAttachment(const MG_State::GLState::FramebufferAttachmentObject& attachmentObject)` | 14 | gone | 0 |  |
| `NoteDriverSideTextureWrite(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject)` | 14 | gone | 1: Managers.h |  |
| `IsAlphaWidenedColorAttachment(const MG_State::GLState::FramebufferAttachmentObject& attachmentObject)` | 13 | gone | 0 |  |
| `GetReadColorAttachment()` | 12 | gone | 0 | The colour attachment glReadPixels/glGetTexImage would read from, or nullptr when the |
| `HandleOfSamplerViewForTexture(const MG_State::GLState::ITextureObject* textureObject)` | 12 | gone | 0 |  |
| `IsIntegerColorAttachment(const MG_State::GLState::FramebufferAttachmentObject& attachmentObject)` | 11 | gone | 0 |  |
| `Ops_ResidentSubData(BufferObject& bufferObject, SizeT offset, DataPtr data)` | 9 | gone | 0 | App bytes for an ADOPTED store: queue them untouched-by-the-mapping; the |
| `EnsureNoColorAttachment(ScratchFramebuffer& fb, GLenum fbTarget)` | 7 | gone | 0 |  |
| `EnsureNoDepthAttachment(ScratchFramebuffer& fb, GLenum fbTarget)` | 7 | gone | 0 |  |
| `MGPipeP5eSeamNotLanded(const char* name, const char* owner, MG_Pipe::MGPipeHandle handle)` | 7 | gone | 0 |  |
| `Ops_AcquirePersistentMapTracked(BufferObject& bufferObject)` | 7 | gone | 0 |  |
| `FramebuffersAttachingTexture(MG_Pipe::MGPipeHandle texture)` | 6 | gone | 0 | BY VALUE, and deliberately: the caller re-binds framebuffers while it walks the |
| `PushedFramebufferIsBoundTo(FramebufferTarget target, MG_Pipe::MGPipeHandle fbo)` | 6 | gone | 0 |  |
| `GetBufferResource(MG_State::GLState::BufferObject* bufferObject)` | 4 | gone | 0 |  |
| `Ops_FlushMappedRangeTracked(BufferObject& bufferObject, Range1D range, Flags<BufferMappingAccessBit> appAccess)` | 4 | gone | 0 |  |
| `Ops_OnDestroyTracked(SharedPtr<BackendBufferResource>&& resource)` | 4 | gone | 0 |  |
| `Ops_ReadbackFromGpuTracked(BufferObject& bufferObject)` | 4 | gone | 0 |  |
| `Ops_ResidentSubDataTracked(BufferObject& bufferObject, SizeT offset, DataPtr data)` | 4 | gone | 0 |  |
| `Ops_RespecifyTracked(BufferObject& bufferObject)` | 4 | gone | 0 | Epoch-tracking wrappers: every op bumps the buffer-mutation epoch AFTER |
| `Ops_SubDataTracked(BufferObject& bufferObject, SizeT offset, SizeT size)` | 4 | gone | 0 |  |
| `ResourceOf(BufferObject& bufferObject)` | 3 | gone | 0 |  |

## `MobileGL/MG_Backend/DirectGLES/Managers.h`

OLD defs: 138, NEW defs: 124, deleted: 14 (gone 14, params differ 0).

| name | body lines | status | HEAD elsewhere (token) | comment |
|---|---|---|---|---|
| `StateBackendObjectRegistry::GetOrCreate(const StatePtr& stateObj)` | 50 | gone | 4: DirectGLES.cpp, Managers.cpp, ... |  |
| `StateBackendObjectRegistry::CollectGarbage()` | 21 | gone | 3: VkTextureManager.cpp, VkTextureManager.h, ... |  |
| `StateBackendObjectRegistry::Find(StateObject* stateObj) (x2)` | 19 | gone | 5: DirectGLES.cpp, Managers.cpp, ... | Null when no live state object owns this key. |
| `VertexInputReadsRecords()` | 8 | gone | 2: DirectGLES.cpp, MultiDraw.cpp | P5e (vi), CONTRACT-P5E §5.1 + §5.8 (ruling 1 / ID-81): THE ARM SELECTOR for this |
| `StateBackendObjectRegistry::CollectGarbageNow()` | 6 | gone | 0 | Pre-P2 API, kept for the legacy arm. On the handle arm there is nothing it could |
| `StateBackendObjectRegistry::DestroyByLifetimeId(Uint64 lifetimeId)` | 6 | gone | 0 | P2 step e2. STATIC, because a death notice is about an object and not about a |
| `StateBackendObjectRegistry::HandleOf(const StateObject* stateObj)` | 6 | gone | 3: DirectGLES.cpp, Managers.cpp, ... | The {slot, gen} this object's twin is keyed on, or the null handle. This is what a |
| `StateBackendObjectRegistry::StateForHandle(MG_Pipe::MGPipeHandle handle)` | 6 | gone | 1: Managers.cpp |  |
| `StateBackendObjectRegistry::begin() (x2)` | 6 | gone | 35: BackendObject_DirectGLES.cpp, DirectGLES.cpp, ... |  |
| `StateBackendObjectRegistry::CollectGarbageIfNeeded()` | 5 | gone | 0 | The seven DirectGLES.cpp call sites drive the LEGACY arm and nothing else. On the |
| `StateBackendObjectRegistry::NoteStateForHandle(MG_Pipe::MGPipeHandle handle, const StatePtr& stateObj)` | 5 | gone | 1: DirectGLES.cpp | P5c (hd): the two halves of SlotTables.h's state note, forwarded. A caller holding |
| `ResourceSubsystemEnabled()` | 4 | gone | 0 | INLINE for the reason SlotTables.h spells out at EsprytSlotTablesEnabled: both are |
| `TextureResourceSubsystemEnabled()` | 4 | gone | 2: DirectGLES.cpp, Managers.cpp |  |
| `VertexInputSubsystemEnabled()` | 4 | gone | 0 |  |

## git log 88a11b81..HEAD -- listed paths (first 80 of 47)

```text
ec973fba [Fix] (Magma): a hang-watch marker slot is reused once its submission's fence completed, not once the host read its event.
553eb86d [Perf] (Magma): texture level uploads stop waiting for the GPU to drain; queue order and the upload barrier already order them (rd12 monolith 70.4 -> 83.4, openra 367 -> 745 fps).
785df78d [Perf] (P14): one UBO-ring lookup per draw, split residual copy/stamp loops, one context read for GPU-write marks, Magma slice stamps from the owner's counter (rd12 monolith Espryt 88.8 -> 90.3, Magma 69.8 -> 70.3 fps).
81bdfb1f [Perf] (Espryt): twin lookups skip the bucket SharedPtr copy and the native-context state is one thread-local block (rd12 monolith 81.0 -> 84.1 fps).
032dcd24 [Perf] (Magma): the wire vertex-input layout is kept on its vertex-elements record and buffer handles resolve through a slot-indexed front.
0158a115 [Perf] (Magma): a wire draw that provably continues the open pass skips the attachment resolve and pass-key walk (rd12 monolith 63.2 -> 65.5, inproc 89.8 -> 92.6 fps).
5788a602 [Perf] (Magma): a wire draw whose image descriptors cannot have moved rebinds its descriptor set with fresh buffer offsets (rd12 monolith 60.5 -> 63.3, inproc 83.7 -> 89.7 fps).
afcf8d96 [Perf] (Espryt): a VAO twin adopts an unchanged buffer set and element buffer instead of re-emitting them (rd12 monolith 75.2 -> 80.5 fps).
e6281b26 [Perf] (Magma): wire texture and buffer lookups get direct-mapped fronts and a continued pass stops re-marking its attachments (rd12 monolith 59.6 -> 60.5 fps).
4ef9705e [Perf] (Magma): wire draws memoise each unit's sampler and skip rebinding an unchanged pipeline, vertex or index binding (rd12 monolith 57.9 -> 60.3 fps).
b556ec31 [Perf] (Espryt): a clean framebuffer attachment skips its per-draw resync behind the unit list's gate (rd12 monolith 74.5 -> 77.0 fps).
a9108e1a [Perf] (Espryt): a VAO twin whose buffer set alone moved re-points enabled arrays without the 32-slot enable walk (rd12 monolith 70.3 -> 73.5 fps).
267211e8 [Perf] (Magma): wire draws stop rehashing program SPIR-V and stop allocating per draw (rd12 monolith 45.7 -> 56.7 fps).
de6ab167 [Refactor] (P13): the legacy BufferBackendOps table, Magma's never-populated texture/render-pass/renderbuffer caches and VkClearManager go, and the buffer mock tests drive the resource op table instead (ratchet 36 -> 27).
fa77b187 [Refactor] (P13): Espryt's frontend-keyed slot-table entries and the apply-thread refusals go, death notices release twins by handle, and the link ratchet refuses any '# P13' line (49 -> 36 symbols).
922507f8 [Refactor] (P13 W6f): Magma's pipeline build reads the draw framebuffer record only, and the frontend texture sync behind it goes; link ratchet 58 -> 49.
dab827d5 [Refactor] (P13 W6e): the backends' frontend-framebuffer clear, blit and readback entries go - the verb port owns those slots in every mode - with Magma's frontend render-pass, pending-clear and renderbuffer halves.
8bc39c79 [Test] (P13 W6d): the unmigrated-emulation list is empty, the dead storage-view format case goes, and Espryt's bring-up check is the colour-attachment cap alone.
8abae647 [Refactor] (P13 W6d): Espryt's family arm predicates fold to the record arm, checked once at capability bring-up, and the frontend-arm walks they selected go.
dc1e95b4 [Refactor] (P13 W6c): Magma's frontend arm goes - the frontend program source, sampled-set walk, placeholder textures, VAO draw memos, identity tables and fast path - leaving the record arm.
e0113503 [Refactor] (P13 W6b): the frontend-arm code after each record-arm return goes, and the transform-feedback state keeps one role.
742d43ae [Refactor] (P13 W6b): the data-arm predicate folds out of every backend branch; frontend-arm unit cases go, three adapted, and two GL sampling rules move to an integration scenario.
7fb18840 [Refactor] (P13 W5 S8): MOBILEGL_BUILD_RECORD_ARM is folded away - the record arm is unconditional - and retired_switches keeps it from coming back.
a0b9ca07 [Fix] (P13 W5 S7): Magma's T0 import and its self-test stay transport-only inside the record arm.
a37212ea [Fix] (P13 W5 S7): Espryt's T0 re-import after a context loss stays transport-only inside the record arm's twin re-arm.
1cd35d39 [Refactor] (P13 W5 S7): Magma's wire-object collection, wire-store teardown, depth-resolve entry point and the fast-rebind destroy epoch follow the record-arm guard.
557975bf [Refactor] (P13 W5 S7): the record verbs' verb-state stamps, the record-reading draw and sampler passes and the image-bindable re-mint follow the record-arm guard.
aa5c12b0 [Fix] (P13 W5 S7): Espryt's shared-image arm in the storage sync stays on the transport guard inside the record arm.
7ac3ce03 [Fix] (P13 W5 S7): Espryt's shared-image sync stays on the transport guard.
956e3847 [Refactor] (P13 W5 S7): the record arm's remaining transport-guarded blocks (XFB capture landing, staged-texture follows, view-window mips, tracker membership, unit-window checks) follow the record-arm guard.
4ec65e80 [Fix] (P13 W5 S7): Espryt's copy-image store follow and its driver-write mark build with the record arm.
f4be8c38 [Style] (P13 W5): VertexInputStateFactory.cpp includes its own header first.
7f194a85 [Refactor] (P13 W5 S3): the apply side's backend comes through MG_Backend's ApplyRoleBackend hook, which ServerLoop installs.
bb45648c [Refactor] (P13 W5 S3): Magma's wire texture and vertex-input builders and the buffer shadow size follow the record-arm guard.
17e36589 [Refactor] (P13 W5 S3): the latch twin and Magma's shared-image binding take their guards; the dual-block diagnostics stay transport.
b13af729 [Refactor] (P13 W5 S5): the record arm's three knobs move from IpcTable to RecordArmTable, which every record-arm build parses.
cb81610b [Refactor] (P13 W5 S3): the record arm's definitions and its depth-resolve probe source follow the record-arm guard.
1956d6cd [Refactor] (P13 W5 S3): transport-only hooks get no-op twins where the record arm calls them, and their guards return to the transport.
c879776a [Refactor] (P13 W5 S3): Espryt's shared-image checks stay on the transport guard.
8a2ddc81 [Refactor] (P13 W5 S3): Magma's funnels, the texture ops and the staged-texture macros follow the record-arm guard; shared images stay transport.
d00d5c94 [Refactor] (P13 W5 S3): the record stores' includes and the slot tables' refusals follow the record-arm guard.
b26167a0 [Refactor] (P13 W5 S3): Espryt's sampler-view refusal latches through the record seam.
73a185f6 [Refactor] (P13 W5 S4): the staged stores move to MG_Backend/Record as MG_Record and die through the record fail seam.
e544121e [Refactor] (P13 W5 S3): record-arm declarations, role-guard stubs and the T0 split in the backends.
22739a03 [Refactor] (P13 W5 S2): MG_Backend/DirectVulkan's record-arm guards name MOBILEGL_BUILD_RECORD_ARM.
036d1ac3 [Refactor] (P13 W5 S2): MG_Backend/DirectGLES's record-arm guards name MOBILEGL_BUILD_RECORD_ARM.
cfd6c873 [Fix] (P13 W4b): the aliasing texture store follows glCopyImageSubData into a server-owned destination.
```

## Method notes

- Definitions: function and class/struct bodies found by regex on comment- and literal-stripped text, then brace matching. Declarations without a body are not counted.
- Header inline members are qualified by their innermost enclosing class. Out-of-class definitions keep their own qualified name.
- Match key = qualified name + whitespace-stripped parameter list. Status 'params differ' means the qualified name is still defined in NEW with a different parameter list.
- Body lines = brace-matched span from the opening brace line to the closing brace line, inclusive.
- Comment = topmost line of the contiguous comment block directly above the definition (a template line is skipped), truncated to 160 chars.
- 'HEAD elsewhere (token)' = number of other MobileGL/MG_Backend source files at HEAD containing the bare identifier as a token (not the same file). Approximate; common names will over-match. Indicates possible relocation, not proof.
- Skipped: unqualified names shorter than 4 characters and control keywords (if/for/while/switch/return/catch/...).
- Duplicate keys (overloads with identical parameter text) are merged and shown with a count.
