// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "Config.h"
#include "FrameContext.h"
#include "PipelineFactory.h"
#include "ProgramFactory.h"
#include "SwapchainObject.h"
#include "UniformManager.h"
#include "VertexInputStateFactory.h"
#include "VkBufferObject.h"
#include "VkBufferManager.h"
#include "VkClearManager.h"
#include "VkRenderPassManager.h"
#include "VkSamplerManager.h"
#include "VkTextureManager.h"
#include "VkTimerQueryManager.h"
#include "MG_Util/Math/VectorTypes.h"
#include <Includes.h>
#include <vk_mem_alloc.h>

#include "../VkIncludes.h"

namespace MobileGL::MG_State::GLState {
    class FramebufferObject;
    class ProgramObject;
    class SamplerObject;
    class VertexArrayObject;
} // namespace MobileGL::MG_State::GLState

namespace MobileGL::MG_Backend::DirectVulkan {
    enum class DrawSetupAspect: Uint8 {
        FramebufferObject  = 1 << 0,
        VertexArrayObject  = 1 << 1,
        UniformBuffer      = 1 << 2,
        VertexBuffer       = 1 << 3,
        IndexBuffer        = 1 << 4,
        IndirectDrawBuffer = 1 << 5,
        Viewport           = 1 << 6,
        Scissor            = 1 << 7,
    };

    struct DrawCmdParam {
        Uint32 vertexCount = 0;
        Uint32 instanceCount = 1;
        Uint32 firstVertex = 0;
        Uint32 firstInstance = 0;
        // Indexed-draw metadata for bounding vertex-stream conversion. baseVertex is the
        // draw's base-vertex offset; indexRangeIsExactView is true only when the draw
        // fetches exactly the indices its IndexBufferView describes (direct DrawElements;
        // multi/indirect forms leave it false because the CPU cannot bound their ranges).
        Int32 baseVertex = 0;
        Bool indexRangeIsExactView = false;
    };

    struct DrawIndexedCmdParam {
        Uint32 indexCount = 0;
        Uint32 instanceCount = 1;
        Uint32 firstIndex = 0;
        Int32 vertexOffset = 0;
        Int32 firstInstance = 0;
    };

    struct DrawCmd {
        GLenum mode = GL_TRIANGLES;
        DrawCmdParam params;
    };

    struct IndexBufferView {
        GLenum indexType = GL_UNSIGNED_SHORT;
        SizeT indexByteOffset = 0;
        SizeT indexByteSize = 0;
    };

    struct DrawIndexedCmd {
        GLenum mode = GL_TRIANGLES;
        IndexBufferView indexBufferView;

        DrawIndexedCmdParam params;
    };

    struct MultiDrawIndexedCmd {
        GLenum mode = GL_TRIANGLES;
        IndexBufferView indexBufferView;

        Uint32 drawCount = 0;
        DrawIndexedCmdParam* pParams = nullptr;
    };

    struct MultiDrawCmd {
        GLenum mode = GL_TRIANGLES;
        Uint32 drawCount = 0;
        DrawCmdParam* pParams = nullptr;
    };

    struct QueueFamilyIndices {
        Int32 graphicsFamily = -1;
        Int32 presentFamily = -1;
    };

    struct PhysicalDevice {
        QueueFamilyIndices queueFamilies;
        VkPhysicalDeviceProperties properties;
        VkPhysicalDevice handle = VK_NULL_HANDLE;

        Bool IsComplete() const {
            return handle != VK_NULL_HANDLE && queueFamilies.graphicsFamily != -1 && queueFamilies.presentFamily != -1;
        }
    };

    class VulkanRenderer : public IBufferCopyCommandProvider,
                           public FrameContext::IRecordingObserver,
                           public VkRenderPassManager::IEvictionObserver,
                           public ProgramFactory::IEvictionObserver {
    public:
        VulkanRenderer(NativeWindowType window, const VulkanRendererConfig& cfg = {});
        ~VulkanRenderer();

        void Initialize();
        void Shutdown();

        // IBufferCopyCommandProvider: recording command buffer, outside any
        // render pass, for immediate staged buffer copies.
        VkCommandBuffer AcquireBufferCopyCommandBuffer() override;

        // FrameContext::IRecordingObserver: prepares the frame's timer-query
        // pool (harvest + reset) right after the frame command buffer begins
        // recording, before any render pass.
        void OnFrameCommandRecordingBegan(VkCommandBuffer commandBuffer) override;

        // VkRenderPassManager::IEvictionObserver: the render-pass aging sweep is
        // destroying a VkRenderPass; evict every graphics pipeline hashed on the
        // dying handle (they share its >1024-boundary idleness, so immediate
        // destruction is safe) and drop the last-pipeline memo if any went.
        void OnRenderPassDestroyed(VkRenderPass renderPass) override;

        // ProgramFactory::IEvictionObserver: an aged-out program entry was
        // destroyed; evict its compute pipeline and graphics pipelines (same
        // idleness guarantee - they are only bound through draws/dispatches that
        // stamp the program entry) and purge the descriptor-set cache entries
        // keyed by its now-recyclable VkDescriptorSetLayout handle.
        void OnProgramEvicted(ProgramFactory::HashType programHash,
                              VkDescriptorSetLayout descriptorSetLayout) override;

        Bool SetupDraw(FrameContext::FrameData& frame, GLenum mode, Flags<DrawSetupAspect> aspects,
                       const DrawCmdParam& drawParams,
                       const IndexBufferView* pIndexBufferView = nullptr);
        void ClearAttachmentsOnActiveRenderPass(VkCommandBuffer commandBuffer,
                                                const RenderPassEntry& compatibleRenderPassEntry);

        enum class ScissoredClearPrep {
            NotNeeded,  // scissor covers the whole target — take the deferred whole-surface path instead
            NoOp,       // nothing to clear (degenerate target or empty scissor rect)
            Ready,      // a render pass is active; record vkCmdClearAttachments with the returned rect
        };
        ScissoredClearPrep PrepareScissoredClear(const MG_State::GLState::FramebufferObject& framebuffer,
                                                 VkClearRect& outClearRect);

        void Clear(GLbitfield mask);
        void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
        void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value);
        void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value);
        void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value);
        void ClearNamedFramebufferfv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                     GLenum buffer, GLint drawbuffer, const GLfloat* value);
        void ClearNamedFramebufferfi(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                     GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
        void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                             GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                             GLbitfield mask, GLenum filter);
        void BlitNamedFramebuffer(const SharedPtr<MG_State::GLState::FramebufferObject>& readFbo,
                                  const SharedPtr<MG_State::GLState::FramebufferObject>& drawFbo,
                                  GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                  GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                  GLbitfield mask, GLenum filter);
        void CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                       GLint x, GLint y, GLsizei width, GLsizei height);
        void CopyImageSubData(const SharedPtr<MG_State::GLState::ITextureObject>& srcTexture,
                              GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                              const SharedPtr<MG_State::GLState::ITextureObject>& dstTexture,
                              GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                              GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);
        void GenerateMipmap(GLenum target);
        void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
        static SizeT GetReadbackTexelSize(VkFormat sourceFormat);
        static Bool ConvertReadbackPixels(const Uint8* sourcePixels, VkFormat sourceFormat,
                                          GLsizei width, GLsizei height, GLenum destinationFormat,
                                          GLenum destinationType, SizeT destinationRowStride,
                                          Uint8* destinationPixels);
        void GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels);
        void GetTextureImage(const SharedPtr<MG_State::GLState::ITextureObject>& texture,
                             TextureUploadTarget uploadTarget, GLint level, GLenum format, GLenum type,
                             GLsizei bufSize, GLvoid* pixels);
        void DispatchCompute(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ);
        void DispatchComputeIndirect(GLintptr indirect);
        void MemoryBarrier(GLbitfield barriers);
        static VkMemoryBarrier BuildMemoryBarrierForGlBarriers(GLbitfield barriers);
        void DrawArrays(const DrawCmd& payload);
        void DrawElements(const DrawIndexedCmd& payload);
        void MultiDrawArrays(const MultiDrawCmd& payload);
        void MultiDrawElements(const MultiDrawIndexedCmd& payloads);
        void MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount,
                                       GLsizei stride);
        void MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride);
        void MultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount,
                                            GLsizei maxdrawcount, GLsizei stride);
        void Present();

        const PhysicalDevice& GetPhysicalDevice() const;
        VkInstance GetInstance() const;
        Bool IsDrawIndirectCountExtensionEnabled() const;

        // GL fence support, expressed in queue-submission indices backed by
        // real VkFences. A GL fence captures GetSyncPointSubmitIndex() at
        // creation: the index of the submission that will carry the commands
        // recorded so far (m_submitCounter + 1 while work is pending, or
        // m_submitCounter when nothing has been recorded since the last
        // submit). It is signaled once that submission's fence is observed
        // signaled - unlike the frame-serial heuristic, this makes fences
        // signal as soon as the GPU actually finishes, which MC 1.21.5's
        // fence-paced ring buffers rely on to recycle their space.
        Uint64 GetSyncPointSubmitIndex() const;
        // Non-blocking: polls outstanding submission fences and reports
        // whether every submission up to `submitIndex` has completed.
        Bool IsSubmitIndexComplete(Uint64 submitIndex);
        // Submits the commands recorded so far without waiting (GL flush).
        // Recording restarts lazily on a fresh command buffer; the submitted
        // one is retired until the frame slot's fence is next waited. Returns
        // true when a submission was made.
        Bool FlushPendingCommands();
        // Flush gated on usefulness: only flushes when `submitIndex` is still
        // unsubmitted, so poll loops on already-submitted fences do not split
        // the frame's render pass (a full tile load/store on TBDR GPUs).
        Bool FlushForSyncPoint(Uint64 submitIndex);
        // Blocking wait for a submission index with a nanosecond timeout.
        // When the index is still unsubmitted and flushIfPending is set, the
        // pending commands are flushed first so the wait can make progress.
        Bool WaitForSubmitIndex(Uint64 submitIndex, Uint64 timeoutNs, Bool flushIfPending);

        // Frame-serial completion, still used by the timer-query paths (their
        // records are bucketed per frame slot).
        Bool IsFrameSerialComplete(Uint64 serial) const;
        // Blocking wait for a submitted serial. Returns false when the serial
        // cannot complete without further submissions (it belongs to the
        // current, not-yet-presented frame) or when the wait failed.
        Bool WaitForFrameSerial(Uint64 serial, Uint64 timeoutNs);

        // GPU timer queries, backing the GL_TIME_ELAPSED / GL_TIMESTAMP
        // frontend. Timestamp support (queue timestampValidBits > 0 and a
        // non-zero timestampPeriod) is cached at device creation.
        Bool IsTimerQuerySupported() const;
        // The samplerAnisotropy device feature was granted, so GL_TEXTURE_MAX_ANISOTROPY_EXT is
        // honored rather than accepted-and-ignored.
        Bool IsSamplerAnisotropySupported() const { return m_samplerAnisotropyFeatureEnabled; }
        // Ensures the frame command buffer is recording (same lazy pattern as
        // SetupDraw) and writes a bottom-of-pipe timestamp into the current
        // frame's pool. Null when unsupported or the pool is exhausted.
        SharedPtr<VkTimerQueryManager::TimestampRecord> WriteTimerQueryTimestamp();
        // Non-blocking: true once the record's raw ticks are on the CPU
        // (harvests the slot once its frame serial has completed).
        Bool IsTimerQueryResultReady(VkTimerQueryManager::TimestampRecord& record);
        // Blocking wait, mirroring ClientWaitSync's caveat: a record written
        // this frame cannot complete until Present submits the commands, so
        // this returns false (result reads as 0) instead of deadlocking.
        Bool WaitForTimerQueryResult(VkTimerQueryManager::TimestampRecord& record);
        Uint64 GetTimerQueryElapsedNs(const VkTimerQueryManager::TimestampRecord& begin,
                                      const VkTimerQueryManager::TimestampRecord& end) const;
        Uint64 GetTimerQueryTimestampNs(const VkTimerQueryManager::TimestampRecord& record) const;

        void RequestSwapchainResize(Uint32 width, Uint32 height);
        // Returns false when the surface is zero-area (minimized/hidden window):
        // no new swapchain is installed and presentation must stay suspended.
        Bool RecreateSwapchain();

    private:
        struct BlitUniformData {
            float srcRect[4] = {0.f, 0.f, 1.f, 1.f};
            float dstRect[4] = {0.f, 0.f, 1.f, 1.f};
            Int surfaceTransform = 0;
            Int padding[3] = {0, 0, 0};
        };

        struct BlitResources {
            SharedPtr<MG_State::GLState::ProgramObject> program;
            SharedPtr<MG_State::GLState::SamplerObject> nearestSampler;
            SharedPtr<MG_State::GLState::SamplerObject> linearSampler;
            Int srcRectLocation = -1;
            Int dstRectLocation = -1;
            Int surfaceTransformLocation = -1;
            Uint32 samplerBinding = 0;
        };

        struct DepthMipmapResources {
            SharedPtr<MG_State::GLState::ProgramObject> program;
            Int srcRectLocation = -1;
            Int dstRectLocation = -1;
            Int surfaceTransformLocation = -1;
            Int srcTexelSizeLocation = -1;
            Uint32 samplerBinding = 0;
        };

        struct DeferredDepthMipmapCleanup {
            Vector<VkImageView> imageViews;
            Vector<VkFramebuffer> framebuffers;
            Vector<VkRenderPass> renderPasses;
            Vector<VkPipeline> pipelines;
        };

        void QueueClearBufferPayload(GLenum buffer, GLint drawbuffer, const ClearAttachmentPayload& clearPayload);
        void QueueClearBufferPayloadForFramebuffer(const MG_State::GLState::FramebufferObject& framebuffer,
                                                  GLenum buffer, GLint drawbuffer,
                                                  const ClearAttachmentPayload& clearPayload);
        void RecordScissoredClearBuffer(const MG_State::GLState::FramebufferObject& framebuffer,
                                        GLenum buffer, GLint drawbuffer,
                                        const ClearAttachmentPayload& clearPayload,
                                        const VkClearRect& clearRect);

        // ---- Submission fence tracking (GL sync objects) ----
        // One record per vkQueueSubmit still in flight, in ascending submit
        // order. Present/readback submissions reference the frame slot's
        // fence (not pool-owned); mid-frame flushes use pooled fences that are
        // recycled once their submission is observed complete.
        // Not thread-safe: like the rest of the renderer, the tracker relies
        // on GL calls being serialized (launchers migrate the context across
        // threads, but calls never run concurrently), so sync-object polls
        // may mutate it without locking.
        struct SubmitRecord {
            Uint64 submitIndex = 0;
            // Buffer-manager frame serial the submission was made under; its
            // completion raises the completed-serial floor (timer queries and
            // buffer busy-tracking live in frame-serial space).
            Uint64 frameSerial = 0;
            VkFence fence = VK_NULL_HANDLE;
            Bool pooledFence = false;
        };
        // Registers a submission that vkQueueSubmit just made with `fence`.
        // Invariant: every graphics-queue submission that outlives its call
        // site must be registered so GL fences observe it. Exempt are the
        // texture-upload/preserve submits in VkTextureManager, which
        // vkWaitForFences inline before returning.
        void RegisterSubmit(VkFence fence, Bool pooledFence);
        // Builds the submit packet for the frame's pending command buffer
        // (consuming the acquire semaphore on the slot's first submission),
        // submits it with `fence`, and registers the submission. On failure
        // the frame state is left untouched. Shared by the mid-frame flush
        // and the readback path so the semaphore-consumption invariant lives
        // in one place.
        Bool SubmitPendingCommandBuffer(FrameContext::FrameData& frame, VkFence fence, Bool pooledFence);
        // Polls in-flight submission fences (prefix order) and advances the
        // completed counter past every fence observed signaled.
        void RefreshCompletedSubmits();
        // All submissions up to `submitIndex` are known complete (their fence
        // was waited or the device was idled); drops their records and
        // recycles pooled fences.
        void OnSubmitsCompletedUpTo(Uint64 submitIndex);
        VkFence AcquirePooledSubmitFence();
        void DestroySubmitFencePool();
        Bool HasPendingRecordedWork() const;
        // Frame-boundary housekeeping for paths that never reach Present's
        // tail (present-less readback loops, suspended presentation, blocking
        // sync waits): runs the same per-frame drains Present performs, but
        // only when every queue submission has been observed complete AND no
        // recorded-but-unsubmitted commands exist - i.e. when CPU-GPU overlap
        // is provably already zero. Never blocks (non-blocking fence poll
        // only), so the presenting path's frames-in-flight pipelining is
        // untouched. Returns true when the drain ran.
        Bool TryDrainFrameTransients();

        Vector<SubmitRecord> m_inFlightSubmits;
        Vector<VkFence> m_freeSubmitFences;
        Uint64 m_submitCounter = 0;
        Uint64 m_completedSubmitCounter = 0;

        NativeWindowType m_window = 0;
        void* m_platformDisplay = nullptr;
        void* m_platformLibrary = nullptr;
        void* m_platformCloseDisplay = nullptr;
        VulkanRendererConfig m_config;
        Bool m_swapchainResizeRequested = false;
        // Presentation is suspended while the window is zero-area (minimized): the
        // swapchain is unusable/out of date, so Present drops frames instead of
        // submitting on a signaled fence / presenting never-acquired images.
        Bool m_presentSuspended = false;

        // Vulkan objects
        Bool m_validationLayersEnabled = false;
        Vector<VkExtensionProperties> m_extensions;
        VkInstance m_instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
        PhysicalDevice m_physicalDevice;
        VkDevice m_device = VK_NULL_HANDLE;
        VmaAllocator m_allocator = nullptr;
        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
        SwapchainObject m_swapchainObject;

        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        VkQueue m_presentQueue = VK_NULL_HANDLE;
        Bool m_drawIndirectCountExtensionEnabled = false;
        Bool m_indexTypeUint8ExtensionEnabled = false;
        Bool m_logicOpFeatureEnabled = false;
        Bool m_multiDrawIndirectFeatureEnabled = false;
        Bool m_samplerAnisotropyFeatureEnabled = false;
        Bool m_shaderDrawParametersExtensionEnabled = false;
        Bool m_shaderDrawParametersFeatureEnabled = false;
        Bool m_unformattedFloatStorageImagesEnabled = false;
        // fillModeNonSolid gates VK_POLYGON_MODE_LINE/_POINT (glPolygonMode); independentBlend gates
        // per-draw-buffer color write masks (glColorMaski). Both are cached at device creation and
        // drive a runtime fallback when the device lacks them.
        Bool m_fillModeNonSolidFeatureEnabled = false;
        Bool m_independentBlendFeatureEnabled = false;
        // dualSrcBlend gates GL_SRC1_* blend factors (glBindFragDataLocationIndexed dual-source blend);
        // primitiveTopologyListRestart gates primitive restart on *list* topologies (strip/fan restart
        // needs no feature). Both cached at device creation and drive a hard-fail-at-draw when absent.
        Bool m_dualSrcBlendFeatureEnabled = false;
        Bool m_primitiveTopologyListRestartFeatureEnabled = false;
        // Cached at device creation from the graphics queue family properties
        // and device limits; drives timer-query support.
        Uint32 m_timestampValidBits = 0;
        Float m_timestampPeriodNs = 0.0f;
        Bool m_timerQuerySupported = false;
        using PFNDrawIndexedIndirectCountFunc = void(VKAPI_PTR*)(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                                                 VkDeviceSize offset, VkBuffer countBuffer,
                                                                 VkDeviceSize countBufferOffset, Uint32 maxDrawCount,
                                                                 Uint32 stride);
        static inline PFNDrawIndexedIndirectCountFunc s_vkCmdDrawIndexedIndirectCount = nullptr;

        VkCommandPool m_commandPool = VK_NULL_HANDLE;

        VkBufferManager m_bufferManager;

        Uint m_imageIndexAcquired = 0;
        FrameContext m_frameContext;

        UniquePtr<PipelineFactory> m_pipelineFactory;
        // Single-slot "last pipeline" memo: skip the per-draw GetOrCreatePipeline work (state
        // gather + synthetic vertex-input rebuild + payload hash + lookup) when the full pipeline
        // state is unchanged from the previous draw. The key provably covers every pipeline field.
        // Reset per-frame and on pipeline destruction so the cached handle can never dangle.
        Bool m_lastPipelineValid = false;
        GLenum m_lastPipelineMode = 0;
        Uint64 m_lastPipelineProgramHash = 0;
        Uint64 m_lastPipelineVertexInputHash = 0;
        Uint64 m_lastPipelineRenderPassHash = 0;
        Uint m_lastPipelineRenderStateVersion = 0;
        ProgramFactory::CompileOptionFlags m_lastPipelineTransformFlags = {};
        VkPipeline m_lastPipelineResult = VK_NULL_HANDLE;
        UnorderedMap<ProgramFactory::HashType, VkPipeline> m_computePipelines;
        UniquePtr<ProgramFactory> m_programFactory;
        UniquePtr<UniformManager> m_uniformManager;
        UniquePtr<VertexInputStateFactory> m_vertexInputStateFactory;
        UniquePtr<VkClearManager> m_clearManager;
        UniquePtr<VkRenderPassManager> m_renderPassManager;
        UniquePtr<VkTextureManager> m_textureManager;
        UniquePtr<VkSamplerManager> m_samplerManager;
        UniquePtr<VkTimerQueryManager> m_timerQueryManager;
        BlitResources m_blitResources;
        DepthMipmapResources m_depthMipmapResources;
        Vector<DeferredDepthMipmapCleanup> m_deferredDepthMipmapCleanup;

        // Skip the per-draw CollectSampledTextures walk (~5% of the render thread) when the sampled
        // texture SET is provably unchanged from the previous draw: same program (lifetime id +
        // backend-state version, which covers sampler-uniform reassignment / relink) and transform
        // flags, and no texture bind/unbind/delete since (GetTextureBindGeneration). On a hit,
        // m_sampledTexturesScratch still holds the previous draw's list and steps 2-4 (feedback /
        // layout probe / transition) re-run on it, so layout correctness is unaffected - only the GL
        // walk is skipped. The program lifetime id (never reused, unlike the GL name) and the
        // monotonic bind generation make the key ABA-proof; the per-command-buffer reset is a cheap
        // belt-and-suspenders.
        Bool m_lastSampledSetValid = false;
        Uint64 m_lastSampledSetProgramLifetimeId = 0;
        Uint32 m_lastSampledSetProgramVersion = 0;
        ProgramFactory::CompileOptionFlags m_lastSampledSetTransformFlags = {};
        Uint64 m_lastSampledSetBindGeneration = 0;

        // Per-draw scratch buffers (clear keeps capacity) — these paths run for every
        // draw call and must not allocate.
        Vector<MG_State::GLState::ITextureObject*> m_sampledTexturesScratch;
        Vector<MG_State::GLState::ITextureObject*> m_storageImageTexturesScratch;
        Vector<VkBuffer> m_vertexBuffersScratch;
        Vector<VkDeviceSize> m_vertexOffsetsScratch;
        Vector<VkVertexInputAttributeDescription> m_patchedAttributesScratch;
        Vector<Float> m_vertexConversionScratch;
        Vector<Uint8> m_vertexRepackScratch;

        struct ConvertedVertexStreamKey {
            const MG_State::GLState::BufferObject* buffer = nullptr;
            Uint64 changeSerial = 0;
            SizeT baseOffset = 0;
            Uint32 sourceStride = 0;
            DataType type = DataType::Float32;
            Int size = 0;
            Bool normalized = false;
            Bool isInteger = false;
            VertexInputStateFactory::VertexStreamConversion conversion =
                VertexInputStateFactory::VertexStreamConversion::None;

            Bool operator==(const ConvertedVertexStreamKey& other) const {
                return buffer == other.buffer && changeSerial == other.changeSerial &&
                       baseOffset == other.baseOffset && sourceStride == other.sourceStride &&
                       type == other.type && size == other.size && normalized == other.normalized &&
                       isInteger == other.isInteger && conversion == other.conversion;
            }
        };

        struct ConvertedVertexStreamKeyHash {
            SizeT operator()(const ConvertedVertexStreamKey& key) const {
                SizeT hash = std::hash<const void*>{}(key.buffer);
                auto combine = [&hash](SizeT value) {
                    hash ^= value + static_cast<SizeT>(0x9e3779b97f4a7c15ull) + (hash << 6) + (hash >> 2);
                };
                combine(std::hash<Uint64>{}(key.changeSerial));
                combine(std::hash<SizeT>{}(key.baseOffset));
                combine(std::hash<Uint32>{}(key.sourceStride));
                combine(std::hash<Uint32>{}(static_cast<Uint32>(key.type)));
                combine(std::hash<Int>{}(key.size));
                combine(std::hash<Bool>{}(key.normalized));
                combine(std::hash<Bool>{}(key.isInteger));
                combine(std::hash<Uint32>{}(static_cast<Uint32>(key.conversion)));
                return hash;
            }
        };

        struct ConvertedVertexStream {
            BufferSlice slice;
            // Number of source elements the cached slice covers. A draw needing a prefix of
            // this range reuses the slice (converted streams are tightly packed); a draw
            // needing more reconverts and replaces the entry, so per (buffer, layout) a
            // frame converts at most the largest range any draw asked for.
            SizeT elementCount = 0;
            // Pins the source buffer for the frame so its heap address cannot be reused by
            // a new BufferObject while this pointer-keyed entry is alive.
            SharedPtr<const MG_State::GLState::BufferObject> sourcePin;
        };
        UnorderedMap<ConvertedVertexStreamKey, ConvertedVertexStream, ConvertedVertexStreamKeyHash>
            m_convertedVertexStreams;

        void CreateInstance();
        VkResult SetupDebugMessenger();
        VkResult DestroyDebugMessenger();
        VkDebugUtilsMessengerCreateInfoEXT PopulateDebugMessengerCreateInfo();
        void CreateSurface();
        void PickPhysicalDevice();
        void CreateLogicalDeviceAndQueues();
        void CreateAllocator();
        void DestroyAllocator();
        void CreateSwapchain();
        void CreateCommandPool();

        VkPipeline GetOrCreatePipeline(
            GLenum mode,
            const MG_State::GLState::ProgramObject& program,
            const ProgramFactory::VkProgramObject& programObj,
            ProgramFactory::CompileOptionFlags transformFlags,
            const MG_State::GLState::VertexArrayObject& vao,
            const RenderPassEntry& renderPassEntry);
        VkPipeline GetOrCreateComputePipeline(const ProgramFactory::VkProgramObject& programObj);
        void DestroyComputePipelines();
        Bool PrepareStorageImageTextures(
            VkCommandBuffer commandBuffer,
            const MG_State::GLState::ProgramObject& program,
            const ProgramFactory::VkProgramObject& programObj);

        Bool UploadAndBindVertexBuffers(VkCommandBuffer commandBuffer, const MG_State::GLState::VertexArrayObject& vao,
                                        const ProgramFactory::VkProgramObject& programObj,
                                        const DrawCmdParam& drawParams,
                                        const IndexBufferView* pIndexBufferView);
        Bool UploadAndBindIndexBuffer(FrameContext::FrameData& frame,
                                     const MG_State::GLState::VertexArrayObject& vao,
                                      const IndexBufferView* pIndexBufferView = nullptr);
        Bool InitializeBlitResources();
        Bool InitializeDepthMipmapResources();
        void ShutdownBlitResources();
        void ShutdownDepthMipmapResources();
        void CollectDeferredDepthMipmapCleanup(Uint32 frameIndex);
        void DestroyDeferredDepthMipmapCleanup();
        Bool TryBlitToDefaultFramebufferWithShader(FrameContext::FrameData& frame,
                                                   MG_State::GLState::FramebufferObject& readFbo,
                                                   MG_State::GLState::FramebufferObject& drawFbo,
                                                   GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                                   GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                                   GLenum filter);
        Bool MaterializePendingClearForTexture(VkCommandBuffer commandBuffer,
                                               MG_State::GLState::ITextureObject& texture);
        Bool MaterializePendingClearForRenderbuffer(
            VkCommandBuffer commandBuffer,
            const SharedPtr<MG_State::GLState::RenderbufferObject>& renderbuffer);
        VkPipeline GetOrCreateBlitPipeline(const RenderPassEntry& renderPassEntry);
        Bool GenerateDepthMipmapWithShader(FrameContext::FrameData& frame,
                                           MG_State::GLState::ITextureObject& texture,
                                           VkTextureManager::TextureResource& resource,
                                           Uint32 baseMipLevel,
                                           Uint32 generateMipLevelCount,
                                           const IntVec3& storageBaseTexelSize,
                                           VkImageLayout originalLayout,
                                           VkImageLayout finalLayout);
        Bool SubmitReadbackCommandsAndWait(FrameContext::FrameData& frame);

        void ShutdownSwapchain();

        // Static functions
        static Int GetPresentQueueFamilyIndex(const PhysicalDevice& physicalDevice, VkSurfaceKHR surface,
                                              const Vector<VkQueueFamilyProperties>& queueFamilies,
                                              Int preferredFamilyIndex = -1);
        static Vector<VkQueueFamilyProperties> GetQueueFamilyFromPhysicalDevice(VkPhysicalDevice device);
        static Int GetQueueFamilyIndex(const Vector<VkQueueFamilyProperties>& queueFamilies, VkQueueFlagBits flag);
        static Vector<VkExtensionProperties> EnumerateInstanceExtensions();
        static Vector<VkExtensionProperties> EnumerateDeviceExtensions(VkPhysicalDevice device);
        static Bool IsExtensionSupported(const Vector<VkExtensionProperties>& availableExtensions,
                                         const char* extensionName);
        static Bool IsExtensionAlreadyEnabled(const Vector<const char*>& enabledExtensions, const char* extensionName);
        static Bool EnableOptionalDeviceExtension(const Vector<VkExtensionProperties>& availableExtensions,
                                                  Vector<const char*>& inOutEnabledExtensions,
                                                  const char* extensionName);
        void ResolveOptionalDeviceExtensions(const Vector<VkExtensionProperties>& availableExtensions,
                                             Vector<const char*>& inOutEnabledExtensions);
        static Bool IsNecessaryDeviceExtensionSupported(VkPhysicalDevice device);
        static Bool GetMoreCapablePhysicalDevice(VkPhysicalDevice newVkDevice, VkSurfaceKHR surface,
                                                 const PhysicalDevice& compareWithDevice,
                                                 PhysicalDevice& outBetterDevice);
        static constexpr const char* s_validationLayerNames[] = {"VK_LAYER_KHRONOS_validation"};
        static constexpr const char* s_deviceExtensionNames[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        static Bool CheckValidationLayerSupport();

        static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                                            VkDebugUtilsMessageTypeFlagsEXT messageType,
                                                            const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                            void* pUserData);
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
