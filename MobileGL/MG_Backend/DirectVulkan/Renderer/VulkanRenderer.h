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
        // Interpret indexByteOffset as a raw client pointer even when an element
        // array buffer is bound (backend-synthesized index lists, e.g. the
        // GL_LINE_LOOP -> LINE_STRIP rewrite).
        Bool forceClientMemory = false;
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

        // VkRenderPassManager::IEvictionObserver: the render-pass aging sweep just
        // destroyed these VkRenderPasses; evict every graphics pipeline hashed on a
        // dying handle (they share its >1024-boundary idleness, so immediate
        // destruction is safe) and drop the last-pipeline memo if any went.
        void OnRenderPassesDestroyed(const Vector<VkRenderPass>& renderPasses) override;

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
        // ANGLE-style consecutive-draw fast path: SetupDraw snapshots the fully
        // resolved draw configuration; the next draw whose cheap version/identity
        // checks all match skips the resolution half (LOD probe, sampled-set
        // walk, render-pass and pipeline resolution) and jumps straight to the
        // per-draw tail. Returns false (leaving no side effects that the full
        // path cannot redo idempotently) whenever anything might have changed.
        Bool TrySetupDrawFastPath(FrameContext::FrameData& frame, GLenum mode, Flags<DrawSetupAspect> aspects,
                                  const DrawCmdParam& drawParams, const IndexBufferView* pIndexBufferView);
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
        void ClearNamedFramebufferiv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                     GLenum buffer, GLint drawbuffer, const GLint* value);
        void ClearNamedFramebufferuiv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                      GLenum buffer, GLint drawbuffer, const GLuint* value);
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
        // GL_DEPTH_COMPONENT / GL_DEPTH_STENCIL / GL_STENCIL_INDEX readback from the
        // read framebuffer's depth/stencil attachment (per-aspect buffer copies with
        // CPU repacking into the requested client layout).
        void ReadDepthStencilPixels(MG_State::GLState::FramebufferObject& readFbo, GLint x, GLint y, GLsizei width,
                                    GLsizei height, GLenum format, GLenum type, void* pixels);
        // Copy-and-repack core shared by depth-stencil ReadPixels and GetTexImage;
        // expects command recording to be active and any render pass already ended.
        void ReadDepthStencilImageToClient(VkImage image, VkFormat vkFormat, VkImageLayout* trackedLayout,
                                           VkImageAspectFlags imageAspect, Uint32 mipLevel, Uint32 baseArrayLayer,
                                           GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                                           void* pixels);
        // Same-extent depth blit between images of different depth formats: host
        // round-trip with a per-texel re-encode (see BlitNamedFramebuffer).
        Bool BlitDepthAcrossFormats(FrameContext::FrameData& frame, VkImage srcImage, VkFormat srcFormat,
                                    VkImageLayout* srcTrackedLayout, Uint32 srcMipLevel, Uint32 srcBaseArrayLayer,
                                    VkImage dstImage, VkFormat dstFormat, VkImageLayout* dstTrackedLayout,
                                    Uint32 dstMipLevel, Uint32 dstBaseArrayLayer, GLint srcX, GLint srcY, GLint dstX,
                                    GLint dstY, GLint width, GLint height, VkImageLayout srcRestoreLayout,
                                    VkImageLayout dstRestoreLayout, Bool stencilAspect);
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

        // GL_SAMPLES_PASSED occlusion queries: every app draw between Start and Stop is
        // wrapped in a Vulkan occlusion query slot; the result is the slot sum. Requires
        // hostQueryReset for slot recycling - Start fails (frontend keeps the query
        // unsupported) when the device lacks it.
        Bool StartOcclusionQueryCapture();
        void StopOcclusionQueryCapture(Vector<Uint32>& outSlots);
        // Flushes pending commands, waits, sums the slots, and recycles them.
        Bool ResolveOcclusionQueryResult(const Vector<Uint32>& slots, Uint64& outSamples);

        void RequestSwapchainResize(Uint32 width, Uint32 height);
        // Re-query the surface and report whether the live swapchain no longer matches it
        // (size or orientation). This - not a VK_SUBOPTIMAL_KHR result - is what decides a
        // rebuild, so a surface the driver merely considers suboptimal cannot thrash.
        Bool SwapchainIsOutOfDate();
        // Returns false when the surface is zero-area (minimized/hidden window):
        // no new swapchain is installed and presentation must stay suspended.
        Bool RecreateSwapchain();

    private:
        // Tiered emission for an already-set-up multi-draw batch (state bound, index
        // buffer bound for the indexed form). Tier 1: VK_EXT_multi_draw. Tier 2: one
        // vkCmdDraw(Indexed)Indirect over a transient command array. Tier 3: unrolled
        // vkCmdDraw(Indexed) loop. Tier eligibility is per-batch (uniform instance
        // state for tier 1, firstInstance/feature legality for tier 2); every tier
        // consumes the same param span, so contiguous-run merging done by the caller
        // benefits all of them.
        void EmitMultiDrawIndexed(VkCommandBuffer commandBuffer, const DrawIndexedCmdParam* pParams, Uint32 drawCount);
        void EmitMultiDraw(VkCommandBuffer commandBuffer, const DrawCmdParam* pParams, Uint32 drawCount);

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
        // Drains since the last Present, gating the drain's frame-boundary-equivalent
        // work (arena rewind + cache aging): a presenting app's mid-frame
        // readbacks/waits must neither churn the transient caches nor accelerate the
        // aging clocks, while present-less loops still cross a boundary every few
        // iterations. Reset in Present.
        Uint32 m_drainsSinceLastPresent = 0;

        NativeWindowType m_window = 0;
        void* m_platformDisplay = nullptr;
        void* m_platformLibrary = nullptr;
        void* m_platformCloseDisplay = nullptr;
        // Some real ICDs (e.g. NVIDIA's proprietary Linux driver) don't implement
        // VK_EXT_headless_surface at all. Detected once in CreateInstance() from the
        // enumerated instance extensions; when false, CreateSurface() falls back to a
        // hidden Xlib window instead of vkCreateHeadlessSurfaceEXT.
        Bool m_headlessSurfaceSupported = true;
        // Set when CreateSurface() had to create its own Xlib window for the fallback
        // above (rather than being handed one by the caller), so Shutdown() knows it
        // owns that window and must destroy it.
        Bool m_ownsFallbackXlibWindow = false;
        // Android has the same shortfall: no Mali/Adreno driver seen so far exposes
        // VK_EXT_headless_surface, so a windowless (EGL pbuffer) context gets an
        // AImageReader's ANativeWindow to hand the WSI instead. Nothing is ever
        // displayed - the reader's images are simply never acquired. Owned here, so
        // Shutdown() deletes it.
        void* m_fallbackImageReader = nullptr;
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
        // Fallback reporting channel for drivers that ship the validation layers but
        // only expose the older VK_EXT_debug_report (Adreno 650 / Vulkan 1.1.128).
        VkDebugReportCallbackEXT m_debugReportCallback = VK_NULL_HANDLE;
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
        // drawIndirectFirstInstance gates indirect commands whose firstInstance != 0;
        // cached at device creation because the tier-2 multi-draw path (a transient
        // VkDrawIndexedIndirectCommand array) is illegal for such a sub-draw without it.
        Bool m_drawIndirectFirstInstanceFeatureEnabled = false;
        // VK_EXT_multi_draw: native batched submission for the CPU-side glMultiDraw*
        // families (tier 1 of the multi-draw dispatch).
        Bool m_multiDrawExtensionEnabled = false;
        Uint32 m_maxMultiDrawCount = 0;
        // Multi-draw dispatch tiers, resolved once at device creation from device support
        // clamped by MOBILEGL_MAGMA_MULTIDRAW_MODE (a preference, never a demand):
        //   tier 1 (ext):      one vkCmdDrawMulti(Indexed)EXT           - m_multiDrawAllowExt
        //   tier 2 (indirect): one vkCmdDraw(Indexed)Indirect batch     - m_multiDrawAllowIndirect
        //   tier 3 (unroll):   one vkCmdDraw(Indexed) per sub-draw      - always available
        // m_multiDrawForceUnrollIndirect additionally forces the GPU-parameter
        // glMultiDraw*Indirect paths onto their per-command loop (mode=unroll only).
        Bool m_multiDrawAllowExt = false;
        Bool m_multiDrawAllowIndirect = false;
        Bool m_multiDrawForceUnrollIndirect = false;
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
        // Union of shader stages sampled-read barriers may name; built at device creation
        // because geometry/tessellation stage bits are invalid in a barrier when their
        // feature is off (VUID-vkCmdPipelineBarrier-srcStageMask-04090/-04091), and
        // ALL_GRAPHICS would also serialize against non-shader stages.
        VkPipelineStageFlags m_sampledReadStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
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
        // VK_EXT_multi_draw entry points, loaded at device creation when the extension
        // (and its multiDraw feature) is enabled; null otherwise.
        static inline PFN_vkCmdDrawMultiEXT s_vkCmdDrawMultiEXT = nullptr;
        static inline PFN_vkCmdDrawMultiIndexedEXT s_vkCmdDrawMultiIndexedEXT = nullptr;

        // VK_EXT_transform_feedback (GL transform feedback capture)
        Bool m_transformFeedbackFeatureEnabled = false;
        // VK_EXT_provoking_vertex. Vulkan's built-in convention is "provoking vertex first"; GL's
        // default is LAST_VERTEX_CONVENTION, and GL derives BOTH flat shading and the transform
        // feedback vertex order from it. provokingVertexLast alone fixes flat shading and the
        // input-assembler capture order and has no dependency on transform feedback; only
        // transformFeedbackPreservesProvokingVertex does.
        Bool m_provokingVertexLastEnabled = false;
        // transformFeedbackPreservesProvokingVertex was actually enabled at device creation. Kept
        // separate because it is the only thing that arms
        // VUID-VkGraphicsPipelineCreateInfo-topology-04884, the rule that forbids a TRIANGLE_FAN
        // pipeline from asking for LAST on a device that cannot preserve a fan's provoking vertex.
        Bool m_provokingVertexXfbPreserveEnabled = false;
        // provokingVertexModePerPipeline: when VK_FALSE every pipeline in one render pass instance
        // must agree on the mode, so glProvokingVertex(GL_FIRST_VERTEX_CONVENTION) cannot be honoured
        // per draw and every pipeline takes GL's default (LAST) instead.
        Bool m_provokingVertexModePerPipeline = false;
        // transformFeedbackPreservesTriangleFanProvokingVertex.
        Bool m_provokingVertexFanPreserved = false;
        // Per-pipeline provoking-vertex mode. capturesXfbFromGeometryStage must be a LINK-TIME
        // property of the program, never the dynamic "is transform feedback active" flag: the
        // 8-entry m_pipelineMemo and the SetupDrawSnapshot fast path key on programObj.hash and
        // the pipeline-state value hash, neither of which moves when glBeginTransformFeedback is
        // called, so a dynamic input here would hand back a stale VkPipeline.
        VkProvokingVertexModeEXT SelectProvokingVertexMode(VkPrimitiveTopology topology,
                                                          Bool capturesXfbFromGeometryStage) const;
        // VK_EXT_vertex_attribute_divisor: without it every non-zero glVertexAttribDivisor
        // behaves as 1, because that is all Vulkan's instance input rate can express.
        Bool m_vertexAttributeDivisorEnabled = false;
        static inline PFN_vkCmdBindTransformFeedbackBuffersEXT s_vkCmdBindTransformFeedbackBuffersEXT = nullptr;
        static inline PFN_vkCmdBeginTransformFeedbackEXT s_vkCmdBeginTransformFeedbackEXT = nullptr;
        static inline PFN_vkCmdEndTransformFeedbackEXT s_vkCmdEndTransformFeedbackEXT = nullptr;
        // Counter buffers (one 4-byte slot per capture binding) let consecutive
        // draws within one glBeginTransformFeedback append GL-style. Transform feedback
        // objects can each hold an open, paused span at the same time, so the counters are
        // per object: one group of four slots each, handed out on first use.
        static constexpr SizeT kXfbCounterObjectSlots = 16;
        VkBufferObject m_xfbCounterBuffer;
        UnorderedMap<Uint, Uint32> m_xfbCounterSlotByObject;
        Uint32 m_xfbNextCounterSlot = 0;
        // Set for a slot once a captured draw has been recorded into its span; selects
        // counter-buffer resume on the next captured draw of the same span.
        Array<Bool, kXfbCounterObjectSlots> m_xfbCountersValid{};
        Array<Uint64, kXfbCounterObjectSlots> m_xfbLastSeenGeneration{};
        // Counter slot group of the bound transform feedback object.
        Uint32 CurrentXfbCounterSlot();
        // Wraps a recorded draw with BeginTransformFeedbackEXT/EndTransformFeedbackEXT
        // when GL transform feedback is active; binds capture buffers on demand.
        Bool BeginXfbCaptureForDraw(FrameContext::FrameData& frame);
        void EndXfbCaptureForDraw(FrameContext::FrameData& frame, Bool began);
        // Makes the captured bytes visible to whatever reads them next. Deferred rather than
        // recorded next to the capture, because the capturing draw runs inside a render pass
        // that declares no self-dependency.
        void MakeXfbWritesVisible();
        Bool m_xfbWritesPendingVisibility = false;
        // Wrap one app draw in an occlusion-query slot while a GL_SAMPLES_PASSED
        // query is active. Returns whether a slot was begun (End must mirror it).
        Bool BeginOcclusionForDraw(VkCommandBuffer commandBuffer);
        void EndOcclusionForDraw(VkCommandBuffer commandBuffer, Bool began);
        Bool m_occlusionQueryPreciseEnabled = false;
        Bool m_hostQueryResetEnabled = false;
        PFN_vkResetQueryPool s_vkResetQueryPool = nullptr;
        VkQueryPool m_occlusionQueryPool = VK_NULL_HANDLE;
        static constexpr Uint32 kOcclusionQuerySlots = 8192;
        Uint32 m_occlusionSlotCursor = 0;
        Bool m_occlusionCaptureActive = false;
        Vector<Uint32> m_occlusionActiveSlots;
        // Transform feedback primitive queries: one pool slot per captured draw yields
        // the (written, needed) pair; GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN sums the
        // first, GL_PRIMITIVES_GENERATED the second - exact with geometry shaders,
        // unlike the CPU fallback accounting.
        Bool m_xfbQueriesSupported = false;
        PFN_vkCmdBeginQueryIndexedEXT s_vkCmdBeginQueryIndexedEXT = nullptr;
        PFN_vkCmdEndQueryIndexedEXT s_vkCmdEndQueryIndexedEXT = nullptr;
        VkQueryPool m_xfbQueryPool = VK_NULL_HANDLE;
        static constexpr Uint32 kXfbQuerySlots = 8192;
        Uint32 m_xfbQuerySlotCursor = 0;
        Bool m_xfbQueryCaptureActive[2] = {false, false}; // [0]=written, [1]=generated
        Vector<Uint32> m_xfbQueryActiveSlots[2];
        Bool m_xfbQuerySlotOpen = false;
        Uint32 m_xfbQueryOpenSlot = 0;

    public:
        // kind: 0 = PRIMITIVES_WRITTEN, 1 = PRIMITIVES_GENERATED.
        Bool StartXfbQueryCapture(Uint32 kind);
        void StopXfbQueryCapture(Uint32 kind, Vector<Uint32>& outSlots);
        Bool ResolveXfbQueryResult(const Vector<Uint32>& slots, Bool wantGenerated, Uint64& outPrimitives);

    private:
        void BeginXfbQueryForDraw(VkCommandBuffer commandBuffer);
        void EndXfbQueryForDraw(VkCommandBuffer commandBuffer);

        VkCommandPool m_commandPool = VK_NULL_HANDLE;

        VkBufferManager m_bufferManager;

        Uint m_imageIndexAcquired = 0;
        FrameContext m_frameContext;

        UniquePtr<PipelineFactory> m_pipelineFactory;
        // Single-slot "last pipeline" memo: skip the per-draw GetOrCreatePipeline work (state
        // gather + synthetic vertex-input rebuild + payload hash + lookup) when the full pipeline
        // state is unchanged from the previous draw. The key provably covers every pipeline field.
        // Reset per-frame and on pipeline destruction so the cached handle can never dangle.
        // Small N-way pipeline-resolution memo (round-robin replacement). A
        // single-entry memo thrashed on draw sequences that alternate a few
        // pipelines (GUI text/quad program ping-pong), paying the full
        // payload-hash lookup per draw; eight entries cover such working sets
        // while keeping the hit path a trivial linear scan.
        struct PipelineMemoEntry {
            GLenum mode = 0;
            Uint64 programHash = 0;
            Uint64 vertexInputHash = 0;
            Uint64 renderPassHash = 0;
            // VALUE hash of the pipeline-relevant fixed-function state (see
            // ComputePipelineStateHash), not the monotonic pipeline-state version:
            // the version never repeats, so a per-draw GL_BLEND toggle would miss
            // all entries forever even though the state alternates between two
            // values the memo already holds.
            Uint64 pipelineStateHash = 0;
            ProgramFactory::CompileOptionFlags transformFlags = {};
            VkPipeline pipeline = VK_NULL_HANDLE;
        };
        static constexpr Uint32 kPipelineMemoSize = 8;
        PipelineMemoEntry m_pipelineMemo[kPipelineMemoSize];
        Uint32 m_pipelineMemoCount = 0;
        Uint32 m_pipelineMemoNext = 0;
        // Hash of every fixed-function GL state the pipeline payload reads that the
        // memo key's other fields (mode / program / vertex input / render pass /
        // transform flags) do not already pin down. Equal hash under an equal rest
        // of key => byte-identical PipelineCreatePayload. Cached per pipeline-state
        // version: the version is monotonic and bumps on every pipeline-state
        // change, so an unchanged (version, colorAttachmentCount) proves the state
        // bytes are unchanged and the hash can be reused without re-reading them.
        Uint64 ComputePipelineStateHash(Uint32 colorAttachmentCount) const;
        Uint m_pipelineStateHashVersion = 0;
        Uint32 m_pipelineStateHashColorCount = 0;
        Uint64 m_pipelineStateHash = 0;
        Bool m_pipelineStateHashValid = false;
        // GetShaderTransformFlags memo. NOT pure in the pre-transform alone: the
        // function also reads whether the bound DRAW framebuffer is the default one
        // (only the default framebuffer gets the Y-flip and rotation bits - an FBO
        // pass renders unflipped). Keyed on BOTH inputs; missing the FBO bit shipped
        // an upside-down default-framebuffer pass after any render-to-texture
        // (minecraft-1.17-main-menu retrace, whole frame flipped).
        VkSurfaceTransformFlagBitsKHR m_baseTransformFlagsPreTransform =
            VK_SURFACE_TRANSFORM_FLAG_BITS_MAX_ENUM_KHR;
        Bool m_baseTransformFlagsIsDefaultFbo = false;
        Bool m_baseTransformFlagsKeyValid = false;
        Uint32 m_baseTransformFlagsCache = 0;
        // isDefaultFbo must be the default-ness of the CURRENTLY bound draw framebuffer;
        // every caller already has it in hand from its own guards.
        Uint32 GetBaseTransformFlagsRaw(Bool isDefaultFbo);
        // Drops every memoized pipeline handle. Required at command-buffer
        // boundaries and whenever any pipeline may have been destroyed. Also drops
        // the cached pipeline-state hash: the same boundaries can retire the GL
        // context whose monotonic version the cache is keyed on.
        void InvalidatePipelineMemo() {
            m_pipelineMemoCount = 0;
            m_pipelineMemoNext = 0;
            m_pipelineStateHashValid = false;
        }
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

        // Memo for the per-draw explicit-LOD-0 eligibility probe
        // (ProgramSamplesOnlySingleLevelTextures): same key family as the
        // sampled-set memo, plus the sampled textures' params-version sum so a
        // level-range or filter change re-probes. On a hit the resolved
        // transform flags are reused, which also collapses the two
        // GetOrCreateProgram lookups into one.
        Bool m_lastLodDecisionValid = false;
        Uint64 m_lastLodProgramLifetimeId = 0;
        Uint32 m_lastLodProgramVersion = 0;
        Uint64 m_lastLodBindGeneration = 0;
        Uint64 m_lastLodParamsSum = 0;
        ProgramFactory::CompileOptionFlags m_lastLodBaseFlags = {};
        ProgramFactory::CompileOptionFlags m_lastLodResultFlags = {};

        // Snapshot behind TrySetupDrawFastPath. Values only: the program and
        // render-pass caches are open-addressing maps whose entries move on
        // insert, so no pointers into them are cached; the pipeline handle is
        // protected by the command-buffer-boundary reset plus the mid-frame
        // pipeline-destruction resets, and monotonic epochs guard everything
        // that can be destroyed or recreated between draws.
        struct SetupDrawSnapshot {
            Bool valid = false;
            Uint8 aspects = 0;
            GLenum mode = 0;
            Uint64 programLifetimeId = 0;
            Uint32 programVersion = 0;
            const void* vao = nullptr;
            // Same rule as VaoDrawMemo::vaoLifetimeId: (address, config version) is not an
            // identity, because a recycled address can arrive carrying a config version
            // the dead VAO also had (two mutations to configure one attribute is the
            // common shape), and "the VAO did not move" would then skip the layout
            // re-resolve for a different VAO.
            Uint64 vaoLifetimeId = 0;
            Uint32 vaoConfigVersion = 0;
            const void* drawFbo = nullptr;
            Uint16 fboVersion = 0;
            Bool drawFboIsDefault = false;
            Uint renderStateVersion = 0;
            Uint64 bindGeneration = 0;
            Uint32 baseTransformFlags = 0;
            Uint32 resolvedTransformFlags = 0;
            Uint64 renderPassHash = 0;
            Uint32 imageIndex = 0;
            Uint64 textureEraseEpoch = 0;
            Uint64 textureImageEpoch = 0;
            Uint64 renderbufferImageEpoch = 0;
            Uint64 sampledContentSum = 0;
            Uint64 sampledParamsSum = 0;
            // Guards the sampler-descriptor reuse hint: bumped by any sampler-object
            // parameter or texture shape change (see GetSamplingResolutionGeneration),
            // none of which the sums above cover.
            Uint64 samplingResolutionGeneration = 0;
            // Render-pass flavor input (DepthTest || StencilTest at snapshot time).
            // A pipeline-state change that leaves this equal cannot change which
            // render pass GetOrCreateRenderPass would pick, so the fast path may
            // re-resolve just the pipeline against the active pass; a change that
            // flips it must fall back to the full path's pass selection.
            Bool drawUsesDepthStencil = false;
            IntVec2 renderPassExtent = {0, 0};
            // colorAttachmentCount of the snapshotting draw's render pass: the
            // pipeline-state hash input, so the fast path can refresh that hash and
            // probe the pipeline memo after a state change without re-fetching the
            // render-pass entry (the pass itself is pinned by renderPassHash above).
            Uint32 renderPassColorCount = 0;
            VkPipeline pipeline = VK_NULL_HANDLE;
            // layoutHash of the snapshotting draw's vertex-input state. The pipeline and
            // the vertex-input pre-flight depend on the VAO only through this (plus the
            // program, pinned separately), so a changed VAO whose aux memo carries the
            // same layoutHash re-uses the snapshot's pipeline and pre-flight verdict
            // outright - the VAO-cycling case Minecraft chunk rendering hits every draw.
            Uint64 vaoLayoutHash = 0;
            // Memoised ProgramFactory entry of the snapshotting draw, valid while
            // (programLifetimeId, programVersion, resolvedTransformFlags) match - all
            // checked above - AND the factory's cache structure epoch is unchanged (the
            // cache is open-addressing and holds entries by value, so any insert/erase
            // moves them). The fast path must re-stamp use through StampProgramUse when
            // it bypasses GetOrCreateProgram, or the idle sweep could evict a live entry.
            const ProgramFactory::VkProgramObject* programObj = nullptr;
            Uint64 programFactoryEpoch = 0;
            // Per-entry copies of the snapshotting draw's sampled set (the scratch
            // vectors below hold only the LAST full-path draw's set, which with more
            // than one snapshot entry is not necessarily this entry's program).
            // sampledTextures/sampledResources carry the same epoch-guarded pointer
            // lifetime rules as the scratch originals: textureEraseEpoch (checked
            // every probe) declines the entry before any erased resource pointer
            // could be dereferenced. sampledLayouts is the layout VALUE each
            // resource held when this entry's descriptors were built (the
            // descriptor-reuse hint needs the SAME layout, not just a sampleable
            // one), and sampledBindingRecords feeds SampledBindingsUnchanged when
            // the bind generation moved.
            Vector<MG_State::GLState::ITextureObject*> sampledTextures;
            Vector<VkTextureManager::TextureResource*> sampledResources;
            Vector<VkImageLayout> sampledLayouts;
            Vector<UniformManager::SampledBindingRecord> sampledBindingRecords;
        };
        // Program-keyed snapshot entries: program ping-pong (Sodium switches programs
        // mid-frame every few draws) would otherwise evict the single snapshot on
        // every switch and send every draw through the full path. Entries are found
        // by programLifetimeId (MRU-first probe); every other guard stays per-probe,
        // so a stale entry declines itself exactly like the old single snapshot did.
        static constexpr Uint32 kSetupDrawSnapshotCount = 4;
        SetupDrawSnapshot m_setupDrawSnapshots[kSetupDrawSnapshotCount];
        Uint32 m_setupDrawSnapshotMru = 0;    // last entry that hit or was filled
        Uint32 m_setupDrawSnapshotVictim = 0; // round-robin fill cursor when all entries are live
        void InvalidateSetupDrawSnapshots() {
            for (auto& snapshot : m_setupDrawSnapshots) {
                snapshot.valid = false;
            }
        }

        // Per-draw scratch buffers (clear keeps capacity) — these paths run for every
        // draw call and must not allocate.
        Vector<MG_State::GLState::ITextureObject*> m_sampledTexturesScratch;
        // Per-binding (texture, effective sampler) lifetime-id records from the same
        // CollectSampledTextures walk that filled m_sampledTexturesScratch. The fast
        // path shadow-compares against them (SampledBindingsUnchanged) when the
        // texture bind generation moved, so a redundant glBindSampler/glBindTexture
        // storm that resolves to the same bindings keeps the fast path.
        Vector<UniformManager::SampledBindingRecord> m_sampledBindingRecordsScratch;
        // Parallel to m_sampledTexturesScratch, refilled by every SetupDraw's
        // first sampled-texture loop: the resolved backend resources, so the
        // post-transition loop can skip re-resolving textures whose layout is
        // already sampleable.
        Vector<VkTextureManager::TextureResource*> m_sampledResourcesScratch;
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

        // One VAO's resolved vkCmdBindVertexBuffers arguments, reusable by a later draw
        // that would resolve them to the same thing. Consecutive draws in a chunk-renderer
        // frame keep the program and the vertex layout and only swap the VAO, so a
        // per-VAO memo turns the second and later draws through each VAO into a validate
        // plus (usually skipped) rebind.
        //
        // Only whole-buffer bindings are memoised. Client-memory and format-converted
        // streams re-upload from a range that depends on the draw's own vertex/index
        // range, and synthetic bindings carry glVertexAttrib* values that are not part
        // of any key here; a layout using any of them is never stored.
        // Field order is hit-path cache locality, hot to cold: the per-draw validate
        // reads the scalars and the EBO memo head, then only the first bindingCount
        // elements of vkBuffers/vkOffsets; the per-binding revalidation arrays at the
        // tail are touched once per frame at most.
        struct ResolvedVertexBindings {
            // Must equal DynamicStateShadow::kMaxShadowedVertexBindings (static_assert in
            // the .cpp): past that width the bind shadow cannot skip a redundant bind
            // either, so a wider layout resolves per draw. Minecraft-shaped layouts use four.
            static constexpr Uint32 kMaxBindings = 8;

            // Frame serial of the last completed resolve OR cross-frame revalidation.
            // Zero until a resolve completes, and reset to zero before one starts, so a
            // resolve that bails out midway cannot leave a half-filled entry matchable.
            // Unlike the original frame-scoped memo, an entry whose buffers are all
            // resident and unmapped is revalidated across frames (per-binding slice
            // epoch compares) instead of re-resolved - see TryBindResolvedVertexBindings.
            Uint64 frameSerial = 0;
            // Identity of the resolved Vulkan layout: the VAO's content hash
            // (VertexInputStateFactory::GetOrComputeHash - the same value the factory
            // keys its entries on) fixes bindings.size(), each binding's base offset,
            // which bindings are client/converted, and (through the mixed-in buffer
            // addresses) which buffer each binding reads. Compared against the VAO's
            // own hash memo on the hit path, so a hit never touches the factory entry.
            VertexInputStateFactory::HashType vertexInputHash = 0;
            // The program's vertex input layout: decides the synthetic-binding set and
            // hence the total binding count.
            Uint32 activeAttribMask = 0;
            Uint32 bindingCount = 0;
            // VkBufferManager::GetSliceEpochCounter() at resolve time. Still equal means
            // no buffer anywhere changed its slice or was persistently mapped since, which
            // settles every per-binding question below in one compare.
            Uint64 sliceEpochCounter = 0;
            // Any bound buffer already carrying a host map when the slice was resolved.
            // Such a buffer can mutate its shadow with no API call, so it has to be
            // re-pushed per draw and the one-compare path above cannot apply.
            Bool anyBufferMapped = true;

            // Resident element-buffer slice memo (skips the per-draw AcquireResidentSlice
            // for the VAO's EBO, which cold-chases 500+ distinct resources in a
            // chunk-cycling frame). Self-validating exactly like the bindings above: a hit
            // requires the LIVE bound EBO pointer to equal indexBuffer AND either an
            // unmoved manager-wide slice-epoch counter (nothing anywhere changed slices
            // or gained a host map, the same one-compare rescue the vertex half uses) or
            // that buffer's resource still carrying indexSliceEpoch (epochs are minted
            // from a process-lifetime counter, so a recycled address can never
            // revalidate). Restart-substituted and streamed EBOs are never stored.
            // indexFrameSerial tracks the last frame the resource's GPU-use serial was
            // stamped through this memo; 0 means no index memo. Independent of the
            // vertex half: both are (pointer, epoch)-validated, so neither can serve
            // stale state for the other.
            const MG_State::GLState::BufferObject* indexBuffer = nullptr;
            Uint64 indexSliceEpoch = 0;
            // GetSliceEpochCounter() when the resource's epoch was last verified; only
            // meaningful while indexFrameSerial matches the current frame serial.
            Uint64 indexSliceEpochCounter = 0;
            VkBuffer indexVkBuffer = VK_NULL_HANDLE;
            VkDeviceSize indexSliceOffset = 0;
            Uint64 indexFrameSerial = 0;

            // Bound per draw (first bindingCount elements).
            VkBuffer vkBuffers[kMaxBindings] = {};
            VkDeviceSize vkOffsets[kMaxBindings] = {};
            // Per binding: the VAO attribute location its buffer comes from, that buffer,
            // and the buffer's VkBufferManager slice epoch when the slice was resolved.
            // Only read by the per-frame revalidation and the something-moved fallback.
            Uint8 attributeLocations[kMaxBindings] = {};
            const MG_State::GLState::BufferObject* buffers[kMaxBindings] = {};
            Uint64 sliceEpochs[kMaxBindings] = {};
        };
        // One direct-mapped slot of the per-VAO draw-memo table below. A slot belongs to
        // the object whose (vaoKey, vaoLifetimeId) pair it carries: the address alone
        // only picks the slot, and the never-reused lifetime id is what proves the slot
        // is THIS VAO's, so the successor allocated onto a destroyed VAO's address
        // always misses. That identity check is load-bearing and the content-hash
        // validations below do NOT stand in for it - a recycled address under a
        // byte-identical configuration reproduces the content hash exactly, which is
        // how a destroyed VAO's resolved bindings were once handed to its successor's
        // draw. The slot is still never dereferenced through vaoKey, and every fact it
        // carries is still validated against live state before use:
        //  - layoutHash/layoutAuxMasks are valid only while contentHash equals the LIVE
        //    VAO's own hash memo (which the VAO's config version guards), so a config
        //    change or a buffer rebind misses even for the same object.
        //  - bindings revalidates per draw exactly as before (frame serial, content
        //    hash, per-binding live buffer pointers and slice epochs).
        struct alignas(64) VaoDrawMemo {
            const MG_State::GLState::VertexArrayObject* vaoKey = nullptr;
            // The VAO's never-reused lifetime id, checked alongside vaoKey. The pointer
            // ALONE is not an identity: a deleted VAO's heap address is handed straight
            // back by the next glGenVertexArrays-shaped allocation, and the successor then
            // matched this slot and inherited the dead object's memos. Both stated
            // defences failed with it, because both reduce to the content hash and the
            // content hash's buffer-identity component was itself a recycled heap address.
            Uint64 vaoLifetimeId = 0;
            // The VAO content hash (VertexInputStateFactory::GetOrComputeHash) the two
            // layout facts below were derived from; 0 while nothing valid is stored.
            Uint64 contentHash = 0;
            Bool layoutFactsValid = false;
            // The resolved layout identity + packed (unsupported, location) masks -
            // the exact values GetBackendAuxMemo used to serve, moved here so the
            // per-draw probe stays inside this table's one hot line instead of
            // touching a second cold line of every cycled VAO object.
            Uint64 layoutHash = 0;
            Uint64 layoutAuxMasks = 0;
            ResolvedVertexBindings bindings;
        };
        // Fixed-size, allocated on first use, never rehashed or swept: entries are
        // recycled in place on slot collisions (two-slot probe, older frame serial
        // evicted), and stale entries self-invalidate through the compares above. A
        // fixed table also makes every VaoDrawMemo/ResolvedVertexBindings pointer
        // stable for the duration of a draw, which the EBO memo handoff
        // (m_currentDrawResolvedEntry) relies on.
        static constexpr Uint32 kVaoDrawMemoSlotCount = 2048; // power of two
        Vector<VaoDrawMemo> m_vaoDrawMemoTable;
        // Finds the slot holding `vao`, or recycles the older of its two candidate
        // slots into an empty memo keyed on `vao`. Never returns null.
        VaoDrawMemo* LookupVaoDrawMemo(const MG_State::GLState::VertexArrayObject* vao);
        // The current draw's memo entry, set by UploadAndBindVertexBuffers and consumed
        // by the same draw's UploadAndBindIndexBuffer (the EBO memo lives in the same
        // entry). Valid ONLY within that window: the next draw's lookup can recycle the
        // slot. Null when the draw's layout is not memoisable.
        ResolvedVertexBindings* m_currentDrawResolvedEntry = nullptr;

        void CreateInstance();
        VkResult SetupDebugMessenger();
        VkResult DestroyDebugMessenger();
        VkResult SetupDebugReportCallback();
        void DestroyDebugReportCallback();
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
        // Takes the frame rather than a command buffer: a first-time storage-usage upgrade has to
        // flush the pending recording (see the body), which retires the current command buffer.
        Bool PrepareStorageImageTextures(
            FrameContext::FrameData& frame,
            const MG_State::GLState::ProgramObject& program,
            const ProgramFactory::VkProgramObject& programObj);

        // The per-draw dynamic-state tail (viewport, scissor, blend constants, depth
        // bias, line width, stencil), gated behind one render-state-parameters-version
        // compare per command buffer - see the gate fields in DynamicStateShadow.
        void ApplyDynamicDrawStateTail(FrameContext::FrameData& frame, const IntVec2& extent, Bool isDefaultFbo);

        Bool UploadAndBindVertexBuffers(VkCommandBuffer commandBuffer, const MG_State::GLState::VertexArrayObject& vao,
                                        const ProgramFactory::VkProgramObject& programObj,
                                        const DrawCmdParam& drawParams,
                                        const IndexBufferView* pIndexBufferView);
        // Binds `entry`'s memoised buffers when every input it was resolved from is
        // still live and unchanged, else returns false and leaves nothing bound.
        // vaoContentHash is the VAO's memoised content hash (GetBackendHashMemo), which
        // pins the layout AND the bound buffers without resolving the factory entry.
        // Non-const entry: a cross-frame revalidation refreshes its serial/epoch stamps.
        Bool TryBindResolvedVertexBindings(VkCommandBuffer commandBuffer,
                                           const MG_State::GLState::VertexArrayObject& vao,
                                           ResolvedVertexBindings& entry,
                                           Uint64 vaoContentHash,
                                           Uint32 activeAttribMask, Uint64 frameSerial);
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
        // Clears one z slice of a VK_IMAGE_TYPE_3D colour image. See the call site in
        // MaterializePendingClearForTexture for why a transfer clear cannot do this.
        Bool ClearDepthSliceWithRenderPass(VkCommandBuffer commandBuffer,
                                           MG_State::GLState::ITextureObject& texture, Uint32 mipLevel,
                                           Uint32 depthSlice, const VkClearValue& clearValue);
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

    public:
        // Submits whatever is recorded and waits for it. The CPU is about to read memory
        // a shader wrote (a mapped shader storage buffer), and coherent host-visible
        // storage only guarantees visibility once the work that produced it has retired.
        Bool FinishPendingGpuWork();

    private:

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
        // VK_KHR_image_format_list: lets MUTABLE_FORMAT images declare their exact view-format
        // set so the driver can keep bandwidth compression (see CreateLogicalDeviceAndQueues).
        Bool m_imageFormatListExtensionEnabled = false;

        static constexpr const char* s_deviceExtensionNames[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        static Bool CheckValidationLayerSupport();

        static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                                            VkDebugUtilsMessageTypeFlagsEXT messageType,
                                                            const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                            void* pUserData);
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
