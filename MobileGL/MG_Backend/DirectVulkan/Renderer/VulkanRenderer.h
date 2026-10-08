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
#include "MagmaPipeArms.h"
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
#include "GpuProgressMarkers.h"
#include "WireRenderPassCompatibility.h"
#if MOBILEGL_BUILD_DISAGGREGATED
#include <MG_Remote/Server/SharedImageRegistry.h>
#endif
#include "MG_Util/Math/VectorTypes.h"
#include <Includes.h>
#include <MG_Backend/BackendObject.h>
#include <MG_Pipe/MGPipeHandles.h>
#include <MG_Util/SelfTest/PrimitivesGeneratedNoXfbProbe.h>
// The applier's CSO store: MGPipeApplier().BoundRenderStateCso is what the pipeline memo
// keys on after P2 (D12.1). Push-only, so the pull build's include graph is unchanged.
#include <MG_Pipe/PipeApply.h>
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
                           public ProgramFactory::IEvictionObserver {
    public:
        VulkanRenderer(NativeWindowType window, const VulkanRendererConfig& cfg = {});
        ~VulkanRenderer();

        void Initialize();
        void Shutdown();

        // IBufferCopyCommandProvider: recording command buffer, outside any
        // render pass, for immediate staged buffer copies.
        VkCommandBuffer AcquireBufferCopyCommandBuffer() override;
        // EGL_BUFFER_AGE_EXT of the active target's default framebuffer for the next frame: the age
        // of the swapchain image acquired for it (SwapchainObject::BufferAgeOf). Asking makes the
        // target keep its presented images' content from then on.
        Int32 CurrentDrawBufferAge();
        VkBufferManager& GetWireBufferManager() { return m_bufferManager; }
        Bool FlushWirePendingCommandsForTextureUpdate() {
            // A new texture upload must not race graphics work that samples
            // the old texels. No current recording does not imply no in-flight
            // submission: finish the recorded work, then wait its whole prefix.
            if (HasPendingRecordedWork() && !FlushPendingCommands()) return false;
            return WaitForSubmitsUpTo(m_submitCounter, UINT64_MAX);
        }
        // The level-upload arm's half of the above, WITHOUT the CPU wait. Its bytes go through a
        // staging buffer into an upload batch that is submitted on this same queue after this
        // flush, and the batch's first barrier takes its source scope from the image's tracked
        // layout (ALL_COMMANDS for GENERAL, the sampled-read stages for a read-only layout), so
        // queue submission order already puts every earlier draw's access to the old texels
        // ahead of the copy. The preserve arm keeps the wait: it retires the old image.
        // (rd12 Magma: the wait was ~1.8 ms/frame of fence stall, one per lightmap update.)
        Bool FlushWirePendingCommandsForTextureUpload() {
            return !HasPendingRecordedWork() || FlushPendingCommands();
        }
        // P7 wave 2 package B3: the submission that will carry whatever is recorded NEXT.
        // RetireWireObjects (WireDraw.inc) already tags future objects with exactly this, and
        // for the same reason: a draw being set up now is not in any submission yet, so the
        // index that names it is one past the counter. Deliberately NOT
        // GetSyncPointSubmitIndex(), which answers m_submitCounter when nothing is recorded -
        // true for a fence taken at that instant, wrong for work about to be recorded.
        Uint64 GetWireNextSubmitIndex() const { return m_submitCounter + 1; }
        // P8-D: the wire arm's indirect draw families, issued as vkCmdDraw[Indexed]Indirect[Count]
        // from the wire stores MGPipeApplier().VerbIndirectBuffer / VerbIndirectParameterBuffer
        // name. `offset` / `countOffset` are byte offsets into those stores; `stride` 0 means
        // tightly packed. FALSE = this device cannot issue the COUNT form natively (the monolith
        // arm's own condition, see the definition in WireDraw.inc) and the caller must read the
        // words on the CPU; TRUE = issued, or declined by name.
        Bool DrawWireIndirectNative(GLenum mode, GLenum type, Uint64 offset, GLsizei drawcount, GLsizei stride,
                                    Bool indexed, Bool counted, Uint64 countOffset);
        // P11 B2 (T0): what importing a client's AHardwareBuffer as a wire buffer store needs from
        // this device. False when the device did not take VK_ANDROID_external_memory_android_
        // hardware_buffer at creation (not Android, not advertised, or a monolith device).
        struct WireAhbImport {
            VkDevice device = VK_NULL_HANDLE;
            VkQueue queue = VK_NULL_HANDLE;
            Uint32 queueFamily = 0;
            VkPhysicalDeviceMemoryProperties memory{};
            void* getAhbProperties = nullptr; // PFN_vkGetAndroidHardwareBufferPropertiesANDROID
        };
        Bool GetWireAhbImport(WireAhbImport& out) const;
        // SHARED IMAGES (BackendObject::BlitDefaultFramebufferToSharedImage): the active surface
        // target's default framebuffer, copied into `image` top row first and scaled to its
        // extent. Returns once the copy is submitted, with its fence published to the image
        // (SharedImages::PublishWrite); waits for it on the CPU only on a device without sync_file
        // export. False (logged) when it cannot be done here: no AHardwareBuffer import, a
        // quarter-turned surface, an unblittable format.
        //
        // `region` (GL window coordinates; Full = all) is the part to copy into an image this
        // renderer has written before - the rest of it is left as it is; a first write, a scaled
        // or turned copy copies all of it.
        Bool BlitDefaultFramebufferToSharedImage(const SharedImageView& image, const MG_Util::Damage::Region& region);
        // IMPLICIT SYNC BY FLUSH (BackendObject::PublishSharedImageAccesses): every image acquired
        // since the last boundary goes back to the foreign family, the recording is submitted with
        // the exportable semaphore, and its sync_file is published to each as a write and a read.
        // From then on an image's first acquire per frame also waits for its pending reads.
        Bool PublishSharedImageAccesses();
        // EGL_ANDROID_native_fence_sync's fence command (BackendObject::ExportNativeFence): the
        // submission's sync_file in `*fence` (-1: none, and the work is waited out here), and - for
        // a session that writes images - the boundary PublishSharedImageAccesses makes.
        Bool ExportNativeFence(int* fence);
        // The reading side's per-use hook (WireSharedImage.inc): every shared-image texture the
        // wire draw or dispatch being set up samples (VkTextureManager::NoteSharedImageUse) is
        // acquired from the foreign family before it, when it moved or is not held.
        void AcquireNotedSharedImages() {
#if MOBILEGL_BUILD_DISAGGREGATED
            if (m_textureManager && m_textureManager->HasNotedSharedImageUses()) AcquireNotedSharedImagesSlow();
#endif
        }
        // One shared-image texture, before a use: a new write generation, or an image released at
        // the last frame boundary, is acquired here - an ownership barrier recorded ahead of the
        // use, the write's fence a wait of the submission carrying it - and noted as read.
#if MOBILEGL_BUILD_DISAGGREGATED
        void AcquireSharedImage(VkTextureManager::TextureResource& resource);
#else
        // P13 W5: no transport, no shared image (only a server allocates one), so a texture's
        // sharedImageId is never set and there is nothing to acquire.
        void AcquireSharedImage(VkTextureManager::TextureResource&) {}
#endif

        // FrameContext::IRecordingObserver: prepares the frame's timer-query
        // pool (harvest + reset) right after the frame command buffer begins
        // recording, before any render pass.
        void OnFrameCommandRecordingBegan(VkCommandBuffer commandBuffer) override;

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
        enum class ScissoredClearPrep {
            NotNeeded,  // scissor covers the whole target — take the deferred whole-surface path instead
            NoOp,       // nothing to clear (degenerate target or empty scissor rect)
            Ready,      // a render pass is active; record vkCmdClearAttachments with the returned rect
        };
        void Clear(GLbitfield mask);
        void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
        void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value);
        void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value);
        void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value);
        void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                             GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                             GLbitfield mask, GLenum filter);
        void CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                       GLint x, GLint y, GLsizei width, GLsizei height);
        void CopyImageSubData(const CopyImageEndpoint& srcEndpoint,
                              GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                              const CopyImageEndpoint& dstEndpoint,
                              GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                              GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);
        void GenerateMipmap(GLenum target);
        void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
        // Server resource handle in, tightly packed owned bytes out. No frontend
        // texture, pixel-pack state or PBO is consulted by this readback.
        Bool ReadTextureImageWire(const MG_Pipe::MGPReadbackInfo& info, Vector<Uint8>& ownedBytes);
        // Copy-and-repack core shared by depth-stencil ReadPixels and GetTexImage;
        // expects command recording to be active and any render pass already ended.
        //
        // `defaultFramebufferOrientation` is set only when the source is the swapchain's
        // depth/stencil image, which this renderer stores display-side-up: the copy rect then
        // has to be mapped out of GL's bottom-origin space and the copied rows re-oriented on
        // the way back, exactly as the colour ReadPixels path does.
        // `sourceLayerCount` above 1 says the `height` rows the client is owed are stored as that
        // many ARRAY LAYERS of a one-row image rather than as rows of one layer - the shape a GL
        // 1D array has in Vulkan. The two produce byte-identical tightly-packed readbacks, so
        // only the copy region differs; everything after it is written against `height`.
        void ReadDepthStencilImageToClient(VkImage image, VkFormat vkFormat, VkImageLayout* trackedLayout,
                                           VkImageAspectFlags imageAspect, Uint32 mipLevel, Uint32 baseArrayLayer,
                                           GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                                           void* pixels, Bool defaultFramebufferOrientation = false,
                                           Uint32 sourceLayerCount = 1);
        // Same-extent depth blit between images of different depth formats: host
        // round-trip with a per-texel re-encode (see BlitNamedFramebuffer).
        Bool BlitDepthAcrossFormats(FrameContext::FrameData& frame, VkImage srcImage, VkFormat srcFormat,
                                    VkImageLayout* srcTrackedLayout, Uint32 srcMipLevel, Uint32 srcBaseArrayLayer,
                                    VkImage dstImage, VkFormat dstFormat, VkImageLayout* dstTrackedLayout,
                                    Uint32 dstMipLevel, Uint32 dstBaseArrayLayer, GLint srcX, GLint srcY, GLint dstX,
                                    GLint dstY, GLint width, GLint height, VkImageLayout srcRestoreLayout,
                                    VkImageLayout dstRestoreLayout, Bool stencilAspect);
        static SizeT GetReadbackTexelSize(VkFormat sourceFormat);
        // Map a GL bottom-left-origin rectangle into the display-oriented swapchain image.
        // Quarter-turn surface transforms swap the copy extent's axes.
        static Bool MapDefaultFramebufferReadbackRect(GLint x, GLint y, GLsizei width, GLsizei height,
                                                      VkExtent2D imageExtent,
                                                      VkSurfaceTransformFlagBitsKHR preTransform,
                                                      VkOffset2D* imageOffset, VkExtent2D* imageCopyExtent);
        // Reorder a tightly packed block copied with MapDefaultFramebufferReadbackRect back into
        // GL row order. The input block has swapped dimensions for 90/270 degree transforms.
        static Bool RemapDefaultFramebufferReadback(const Uint8* rawPixels, Uint32 logicalWidth,
                                                    Uint32 logicalHeight,
                                                    VkSurfaceTransformFlagBitsKHR preTransform,
                                                    SizeT texelSize, Uint8* outPixels);
        static Bool ConvertReadbackPixels(const Uint8* sourcePixels, VkFormat sourceFormat,
                                          GLsizei width, GLsizei height, GLenum destinationFormat,
                                          GLenum destinationType, SizeT destinationRowStride,
                                          Uint8* destinationPixels);
        void DispatchCompute(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ);
        void DispatchComputeIndirect(GLintptr indirect);
        void MemoryBarrier(GLbitfield barriers);
        static VkMemoryBarrier BuildMemoryBarrierForGlBarriers(GLbitfield barriers);
        void DrawArrays(const DrawCmd& payload);
        void DrawElements(const DrawIndexedCmd& payload);
        void MultiDrawArrays(const MultiDrawCmd& payload);
        void MultiDrawElements(const MultiDrawIndexedCmd& payloads);
        void Present();

        const PhysicalDevice& GetPhysicalDevice() const;
        VkInstance GetInstance() const;
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
        // For tests: the per-frame descriptor pools (see DescriptorPoolCensus.h).
        DescriptorPoolCensus GetDescriptorPoolCensus() const {
            return m_uniformManager ? m_uniformManager->GetDescriptorPoolCensus() : DescriptorPoolCensus{};
        }
        // Non-blocking: polls outstanding submission fences and reports
        // whether every submission up to `submitIndex` has completed.
        Bool IsSubmitIndexComplete(Uint64 submitIndex);
        // Submits the commands recorded so far without waiting (GL flush).
        // Recording restarts lazily on a fresh command buffer; the submitted
        // one is retired until the frame slot's fence is next waited. Returns
        // true when a submission was made.
        Bool FlushPendingCommands();
        // DEVICE LOSS. A GPU fault or hang the driver reset leaves this renderer's VkDevice lost:
        // every later submit and wait answers VK_ERROR_DEVICE_LOST. NoteDeviceLoss records it at
        // the submit/wait sites (cheap, any result); IsDeviceLost answers from the record and,
        // when nothing was recorded, asks the device once (only failure paths call it, so the
        // probe's wait costs nothing on a healthy frame). Each session owns its own VkDevice, so a
        // loss here is this session's alone.
        void NoteDeviceLoss(VkResult result, const char* where);
        Bool IsDeviceLost();
        // For a wire site whose submit or wait just failed: true when the failure is a lost
        // device, in which case this session has been latched (MGPipeSessionLatch - it ends, the
        // display server's other sessions do not) and the caller must return from its verb
        // without recording or submitting anything more. False: an ordinary failure, the site
        // keeps the death it had. `site` names it in the latch line.
        Bool LatchWireDeviceLoss(const char* site);
        // THE GPU HANG WATCH (GpuProgressMarkers.h): true once the watch named one of this device's
        // submissions as the one that stops the GPU. The device is then treated as lost, this session
        // is latched, and the caller returns from its verb without submitting anything more. Asked
        // before every frame submission and by the apply loop before it applies another record.
        Bool LatchIfGpuHung(const char* site);
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
        // ARB_base_instance extends indirect command records with a non-zero firstInstance and
        // requires gl_InstanceID to remain zero-based. Vulkan needs both features to honor that
        // complete contract: one legalizes the command word, the other enables the shader rebase.
        Bool IsNonZeroIndirectBaseInstanceSupported() const {
            return m_drawIndirectFirstInstanceFeatureEnabled && m_shaderDrawParametersFeatureEnabled;
        }
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
        // eglSwapInterval. Records the request; Present rebuilds the swapchain for it once the
        // current frame is on screen, and only when it maps to a different present mode. A
        // renderer created without an app window ignores it: its surface (headless, or an
        // AImageReader nobody consumes) is never displayed, and FIFO there would block once
        // the reader's queue fills.
        void SetSwapInterval(Int interval);
        // Re-query the surface and report whether the live swapchain no longer matches it
        // (size or orientation). This - not a VK_SUBOPTIMAL_KHR result - is what decides a
        // rebuild, so a surface the driver merely considers suboptimal cannot thrash.
        Bool SwapchainIsOutOfDate();
        // Returns false when the surface is zero-area (minimized/hidden window):
        // no new swapchain is installed and presentation must stay suspended.
        Bool RecreateSwapchain();

        // SURFACE TARGETS. One renderer - one device and every GL object's resources - serves
        // every EGL surface of its context's client; only the presentation side (VkSurfaceKHR,
        // swapchain, its render-finished semaphores, the acquired image, the pbuffer's image
        // reader) belongs to a surface. `key` names the client surface. The renderer starts with
        // the surface it was constructed for as its active target (SetActiveSurfaceTargetKey).
        // Activating another key parks the active target - its recorded work submitted, its
        // pending acquire consumed - and brings the other back, or builds it on first use.
        void SetActiveSurfaceTargetKey(Uint64 key) { m_activeTargetKey = key; }
        // False when a NEW target's surface could not be built (logged); the renderer is then left
        // with no active target - every present drops its frame - and the caller refuses by name.
        Bool ActivateSurfaceTarget(Uint64 key, NativeWindowType window, const VulkanRendererConfig& surfaceConfig);
        // Drops a surface's target, active or parked. A no-op for a key that has none.
        void DestroySurfaceTarget(Uint64 key);
        // The default framebuffer's extent (surface space, what PublishDefaultFramebufferInfo
        // publishes) of `key`'s target, active or parked; 0x0 when it has none or no swapchain.
        VkExtent2D SurfaceTargetExtent(Uint64 key) const;
        // The active WINDOW target's window changed size. Between frames (nothing recorded since the last
        // present) its swapchain is rebuilt now - which publishes the new extent - so a client that has
        // nothing to redraw learns it without presenting; with a frame half-recorded the rebuild waits
        // for that frame's present (it would otherwise land half in each swapchain). False: no active
        // window target.
        Bool FollowActiveWindowResize();
        // Whether `key` names the active target.
        Bool IsActiveSurfaceTarget(Uint64 key) const { return key == m_activeTargetKey && m_surface != VK_NULL_HANDLE; }

    private:
        // The presentation half of the renderer, parked while another surface is active. Its
        // fields mirror the renderer members ExchangeActiveTarget swaps them with.
        struct SurfaceTarget {
            Uint64 serial = 0;
            NativeWindowType window = 0;
            void* platformDisplay = nullptr;
            void* platformLibrary = nullptr;
            void* platformCloseDisplay = nullptr;
            void* fallbackImageReader = nullptr;
            Uint32 surfaceWidth = 0;
            Uint32 surfaceHeight = 0;
            Optional<Int> swapInterval;
            Bool presentsToAppWindow = false;
            Optional<Int> swapchainSwapInterval;
            Bool swapchainResizeRequested = false;
            Bool presentSuspended = false;
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            SwapchainObject swapchain;
            Vector<VkSemaphore> renderFinishedSemaphores;
            Uint imageIndexAcquired = 0;
            Uint defaultFramebufferImageIndex = 0;
        };
        void ExchangeActiveTarget(SurfaceTarget& target);
        // Submits the active target's work, consumes its pending acquire and moves it into
        // m_parkedTargets. Leaves the renderer with no target.
        void ParkActiveTarget();
        void OnActiveTargetChanged();
        void DestroyParkedTarget(SurfaceTarget& target);
        UnorderedMap<Uint64, SurfaceTarget> m_parkedTargets;
        Uint64 m_activeTargetKey = 0;
        Uint64 m_activeTargetSerial = 1;
        Uint64 m_nextTargetSerial = 2;

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

        // P7 wave 2-B2, CONTRACT-P7 §5.2 (B'): THE FRONTEND MEMBERS ARE PULL-BUILD ONLY. In a
        // disaggregated build the monolith arm blits and generates depth mips with the baked
        // modules (WireColorBlit.inc, WireDepthMipmap.inc), so no DirectVulkan object holds a
        // ProgramObject, a ShaderObject or a SamplerObject any more - which is what takes
        // ProgramObject::{AllocateLifetimeId, AttachShader, Link, ~ProgramObject},
        // ShaderObject::{SetShaderSource, Compile, JoinPendingCompile, ReleaseCompileNode,
        // DropCompileNode} and SamplerObject::{SetWrapS, SetWrapT, SetWrapR, SetLodRange} out
        // of the link ratchet's `p7-magma` bucket (§4.2). The destructor of a SharedPtr member
        // is a reference too, so the MEMBER has to go and not only its construction.
        //
        // The structs stay, empty, so ShutdownBlitResources and the two `= {}` assignments read
        // the same on both builds.
        struct BlitResources {
        };

        struct DepthMipmapResources {
        };

        // A single-sample staging image for multisample-resolve blits that also have to change
        // orientation. vkCmdResolveImage cannot flip (it takes one offset per side, not the
        // invertible pair vkCmdBlitImage takes), so a resolve into or out of the default
        // framebuffer used to land the mirrored band. Resolving here first and then blitting from
        // here separates the two operations, and each one then does only what it can express.
        //
        // Pooled rather than created per blit: the CTS runs hundreds of these back to back, and
        // create-destroy per call would both cost allocations and, worse, need per-call deferred
        // destruction to outlive the recording. It grows to the largest extent asked for and is
        // reused; format changes recreate it.
        struct MultisampleResolveScratchImage {
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkExtent2D extent = {0, 0};
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        };
        MultisampleResolveScratchImage m_msResolveScratch;
        void DestroyMultisampleResolveScratchImage();

        struct DeferredDepthMipmapCleanup {
            Vector<VkImageView> imageViews;
            Vector<VkFramebuffer> framebuffers;
            Vector<VkRenderPass> renderPasses;
            Vector<VkPipeline> pipelines;
        };

        void QueueClearBufferPayload(GLenum buffer, GLint drawbuffer, const ClearAttachmentPayload& clearPayload);
        // See NoteDeviceLoss. Set once, never cleared: a lost VkDevice stays lost.
        Bool m_deviceLost = false;
        Bool m_deviceLossLatched = false;
        // Brackets this device's frame submissions for the GPU hang watch (LatchIfGpuHung).
        GpuProgressMarkers m_progressMarkers;
        struct WireImage {
            VkImage image = VK_NULL_HANDLE;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
            VkExtent2D extent{};
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageLayout* trackedLayout = nullptr;
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
            VkImageAspectFlags aspect = 0;
            // Zero for the default framebuffer, which no arm that reads this ever reaches.
            VkImageUsageFlags usageFlags = 0;
            Uint32 level = 0, layer = 0, layers = 1, levels = 1;
            Bool isDefault = false;
            // Set when isDefault: the swapchain image index this role resolved to.
            Uint32 swapchainImageIndex = 0;
            Bool is3D = false;
            MG_Pipe::MGPipeHandle storage = MG_Pipe::kMGPipeNullHandle;
            // The storage's resource, for a non-default surface (null for the default framebuffer).
            VkTextureManager::TextureResource* resource = nullptr;
        };
        // `isWriteTarget` selects which side of the default framebuffer this resolves: a write
        // re-points it at the image Present() acquired, a read keeps the current one (see
        // m_defaultFramebufferImageIndex). Reads must pass false.
        WireImage ResolveWireImage(const MG_Pipe::MGPFramebufferState& fbo,
                                   const MG_Pipe::MGPSurface& surface, VkImageAspectFlags aspect,
                                   Bool isWriteTarget = false);
        void TransitionWireImage(WireImage& image, VkImageLayout layout);
        void ClearWireFramebuffer(const MG_Pipe::MGPFramebufferState& fbo,
                                  const ClearAttachmentPayload& payload, GLint drawbuffer = -1);
        void BlitWireFramebuffers(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                 GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter);
        void ResolveWireDepthStencil(WireImage source, WireImage destination,
                                     GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                     GLint dx0, GLint dy0, GLint dx1, GLint dy1);
        // One aspect of a depth/stencil image through a buffer (source in TRANSFER_SRC_OPTIMAL,
        // destination in TRANSFER_DST_OPTIMAL, one format). The returned buffer is named by the
        // recorded commands and must be retired with them.
        UniquePtr<VkBufferObject> CopyWireAspectThroughBuffer(VkImage srcImage, Uint32 srcLevel, Uint32 srcLayer,
                                                              GLint sx, GLint sy, const WireImage& destination,
                                                              GLint dx, GLint dy, Uint32 width, Uint32 height,
                                                              VkImageAspectFlags aspect, VkFormat format,
                                                              Bool mirrorY, const char* fatalDetail);
        // A single-sample depth/stencil blit whose rectangles make it a plain copy, performed
        // without vkCmdBlitImage (P7 gate 5). False when the shape is not a plain copy.
        Bool CopyWireDepthStencilBlit(WireImage& source, WireImage& destination,
                                      GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                      GLint dx0, GLint dy0, GLint dx1, GLint dy1, VkImageAspectFlags aspect);
        // True when the rectangles give a multisample depth/stencil resolve a shape it declines
        // (a scale, an X mirror, a Y mirror across two formats); the decline has then been
        // logged and its INVALID_OPERATION recorded. BlitWireFramebuffers asks it BEFORE any
        // aspect runs, so a call the SHAPE declines leaves every attachment alone - the user
        // and the default draw framebuffer alike. The CAPABILITY decline inside
        // ResolveWireDepthStencil (a stencil-only resolve without VK_EXT_shader_stencil_export
        // on a device without VK_KHR_depth_stencil_resolve) is still decided per aspect, after
        // colour ran; rule I (a) accepts that (notes/p7/magma-b2.md §2.10).
        Bool DeclineWireDepthStencilResolveShape(const WireImage& source, const WireImage& destination,
                                                 GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                                 GLint dx0, GLint dy0, GLint dx1, GLint dy1);
        PFN_vkCreateRenderPass2 m_wireCreateRenderPass2 = nullptr;
        VkResolveModeFlags m_wireDepthResolveModes = 0;
        VkResolveModeFlags m_wireStencilResolveModes = 0;
        // P7 wave 2-B2, CONTRACT-P7 §3.2 (`multisample-blit-aspect`): the SECOND arm of the
        // multisample depth/stencil resolve, for the device that does not carry
        // VK_KHR_depth_stencil_resolve. One baked pass per aspect, sample zero, no frontend
        // object - see WireMultisampleResolve.inc. `resolved` must be a single-sample image of
        // the source's format and extent; the rect is expressed by the scissor.
        Bool ResolveWireDepthStencilWithShader(WireImage source, WireImage resolved,
                                               VkImageAspectFlags aspect, GLint sx0, GLint sy0,
                                               Uint32 width, Uint32 height);
        struct WireMultisampleResolveResources;
        WireMultisampleResolveResources* m_wireMultisampleResolveResources = nullptr;
        void DestroyWireMultisampleResolveResources();
        Bool m_wireShaderStencilExport = false;
        // VK_EXT_robustness2 (or its KHR promotion) with ONLY nullDescriptor enabled, wire arms
        // only: an invalid image unit binds a null storage descriptor (codex closeout finding 2).
        Bool m_wireNullDescriptor = false;
        // The resolve probe's verdict for this device (WireDepthResolveArm.h,
        // WireDepthResolveProbe.h): true when the no-draw render pass was measured leaving its
        // target unwritten while the shader control resolved - the shader arm then resolves
        // first and the render-pass arm is the fallback (P7 gate 5, g5-msprobe).
        Bool m_wirePreferShaderDepthResolve = false;
        // P11 B2 (T0): the device took the AHardwareBuffer import extensions at creation, and the
        // one entry point the import needs (PFN_vkGetAndroidHardwareBufferPropertiesANDROID).
        Bool m_wireAhbImport = false;
        void* m_wireGetAhbProperties = nullptr;
        // YUV shared images (WireYuvImage.inc): the device enabled samplerYcbcrConversion, and the
        // conversion pipelines made so far (one per buffer format and colour conversion).
        Bool m_samplerYcbcrConversion = false;
        struct WireYuvResources;
        WireYuvResources* m_wireYuvResources = nullptr;
        void DestroyWireYuvResources();
        // A YUV texture's first use in a frame converts its image (yuvConvertedFrame against this,
        // which every frame boundary - present, publishing flush, native fence - moves).
        Uint64 m_yuvFrameSerial = 1;
        Bool ConvertYuvSharedImage(VkTextureManager::TextureResource& resource);
        // Shared images this session presents into, imported once each and keyed by the image's
        // id (monotonic, never reused). `owner` watches the registry's image: an entry whose image
        // died is dropped at a later present once `lastSubmit`, its last copy, has completed.
        struct SharedImagePresentTarget {
            VkTextureManager::ImportedSharedImage image;
            WeakPtr<const void> owner;
            Uint64 lastSubmit = 0;
            // A copy has written the image through this import: what it holds is a frame.
            Bool written = false;
        };
        UnorderedMap<Uint64, SharedImagePresentTarget> m_sharedImagePresentTargets;
        void DestroySharedImagePresentTargets(Bool onlyDead);

        // SHARED-IMAGE SYNCHRONISATION (WireSharedImage.inc). The sync_file fences other sessions
        // publish become binary semaphores: a temporary SYNC_FD import is a WAIT, an exportable
        // semaphore a SIGNAL, of the next frame submission - whichever of SubmitPendingCommandBuffer
        // and Present's submit comes first, which is the one carrying what was recorded since.
        // Each semaphore is recycled once the submission that used it has completed.
        // m_sharedImageSyncFd: the device imports and exports SYNC_FD binary semaphores; without
        // it the waits happen on the CPU and nothing is exported (see WireSharedImage.inc).
        Bool m_sharedImageSyncFd = false;
        // The session writes images - it ends frames at publishing flushes
        // (PublishSharedImageAccesses), or it reached one through a framebuffer (ResolveWireImage):
        // a first acquire waits for the image's pending reads as well, and its boundaries publish
        // what it used as written.
        Bool m_sharedImageImplicitSync = false;
        PFN_vkImportSemaphoreFdKHR m_importSemaphoreFd = nullptr;
        PFN_vkGetSemaphoreFdKHR m_getSemaphoreFd = nullptr;
        struct SharedImageWait {
            VkSemaphore semaphore = VK_NULL_HANDLE;
            VkPipelineStageFlags stages = 0;
        };
        Vector<SharedImageWait> m_sharedImageWaits;                // staged for the next submission
        VkSemaphore m_sharedImageSignal = VK_NULL_HANDLE;          // staged for the next submission
        VkSemaphore m_sharedImageSignalSubmitted = VK_NULL_HANDLE; // the last one submitted, to export
        struct SharedImageSemaphoreInFlight {
            VkSemaphore semaphore = VK_NULL_HANDLE;
            Uint64 submitIndex = 0;
            Bool exportable = false;
            Bool exported = false; // a signal whose sync_file was taken (which unsignals it)
        };
        Vector<SharedImageSemaphoreInFlight> m_sharedImageSemaphoresInFlight; // submit order
        Vector<VkSemaphore> m_freeSharedImageWaitSemaphores;
        Vector<VkSemaphore> m_freeSharedImageSignalSemaphores;
        // The reading side, per session: the shared-image textures this frame holds (acquired, not
        // yet released; keyed as in VkTextureManager's handle map, with the submission that carries
        // the acquire), and the frame's ReadTracker, published at Present.
        struct SharedImageHold {
            Uint64 key = 0;
            Uint64 acquireSubmit = 0;
        };
        Vector<SharedImageHold> m_sharedImagesHeld;
#if MOBILEGL_BUILD_DISAGGREGATED
        MG_Remote::Server::SharedImages::ReadTracker m_sharedImageReads;
#endif
        Vector<Uint64> m_sharedImageUseScratch;
        // What one vkQueueSubmit carries beyond its own semaphores (AttachSharedImageSync).
        struct SharedImageSubmitSync {
            Bool attached = false;
            Vector<VkSemaphore> waits;
            Vector<VkPipelineStageFlags> stages;
            Vector<VkSemaphore> signals;
        };
        void AttachSharedImageSync(VkSubmitInfo& info, SharedImageSubmitSync& storage);
        // After the submission `storage` was attached to was submitted and registered.
        void CommitSharedImageSync(const SharedImageSubmitSync& storage);
        // Takes ownership of `fence` (a sync_file; -1 = nothing to wait for).
        void StageSharedImageWait(int fence, VkPipelineStageFlags stages);
        VkSemaphore TakeSharedImageSemaphore(Bool exportable);
        // The submitted signal's sync_file (-1: it has already signaled). False: not exported.
        Bool ExportSharedImageSignal(VkSemaphore semaphore, int& fence);
        void RecycleSharedImageSemaphores(Uint64 completedSubmit);
        void DestroySharedImageSync();
        void AcquireNotedSharedImagesSlow();
        // The reading side's frame boundary: release every held image back to the foreign family
        // in the open recording, and - after the submission carrying it - publish its fence.
        void ReleaseHeldSharedImages();
        void PublishSharedImageReads();
        // A frame dropped unsubmitted (Present with no usable swapchain).
        void AbandonSharedImageReads();
        // Runs the probe (memoized per device identity) and sets the member above. Called at the end of
        // device creation, after ArmPrimGenReroute: it records on m_graphicsQueue.
        void ArmWireDepthResolveOrder();
        Bool BlitWireColorToDefault(WireImage source, WireImage destination,
                                   GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                   GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLenum filter);
        Bool BlitWireColorImage(WireImage source, WireImage destination,
                               GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                               GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLenum filter, Bool mipmap);
        // P7 wave 2-B, CONTRACT-P7 §3.2 (`default-color-blit-shape`): the rotated arm. Moves the
        // source region into an owned single-sample 2D image of a sampled-float format in the
        // same size-compatibility class, then shader-blits THAT to the rotated default
        // framebuffer - the shapes BlitWireColorImage itself cannot sample (an integer format, a
        // 3D or array source) become shapes it can.
        Bool BlitWireColorToDefaultThroughScratch(WireImage source, WireImage destination,
                                                  GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                                  GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                                                  GLenum filter);
        struct WireColorBlitResources;
        WireColorBlitResources* m_wireColorBlitResources = nullptr;
        void DestroyWireColorBlitResources();
        void ResetWireColorBlitFramePool(Uint32 frameIndex);
        // P7 wave 2-B, CONTRACT-P7 §5.1 (A): the baked depth-mip program (WireDepthMipmap.inc).
        // One level of a depth chain, source and destination being two depth subresources of
        // the same image; the caller loops. It owns no frontend object, which is what the
        // monolith arm's GenerateDepthMipmapWithShader cannot say.
        Bool GenerateWireDepthMipLevel(WireImage source, WireImage destination);
        struct WireDepthMipmapResources;
        WireDepthMipmapResources* m_wireDepthMipmapResources = nullptr;
        void DestroyWireDepthMipmapResources();
        // P7 wave 2-B (exit gate 3): the readback's own write-visibility barrier, recorded before
        // the copy instead of inferred from the image's tracked layout. See WireFramebuffer.inc.
        void RecordWireReadbackWriteBarrier();
        void ReadWirePixels(GLint x, GLint y, GLsizei width, GLsizei height,
                            GLenum format, GLenum type, void* pixels);
        void GenerateWireMipmap();
        Bool SetupWireDraw(FrameContext::FrameData& frame, GLenum mode, Flags<DrawSetupAspect> aspects,
                           const DrawCmdParam& drawParams, const IndexBufferView* indices);
        void DestroyWireDrawPass();
        void RetireWireDrawPass();
        struct WireDrawAttachmentKey {
            MG_Pipe::MGPipeHandle storage = MG_Pipe::kMGPipeNullHandle;
            Uint32 resourceKind = 0;
            VkImage image = VK_NULL_HANDLE;
            VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkImageAspectFlags aspect = 0, requestedAspect = 0;
            Uint32 level = 0, layer = 0, layers = 1, imageLevels = 1;
            Uint32 width = 0, height = 0;
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
            Bool operator==(const WireDrawAttachmentKey&) const = default;
        };
        struct WireDrawPassKey {
            Vector<WireDrawAttachmentKey> attachments;
            Vector<Uint32> colorReferences;
            Uint32 depthReference = VK_ATTACHMENT_UNUSED;
            Uint32 width = 0, height = 0, layers = 1;
            Uint64 textureImageEpoch = 0, resourceEraseEpoch = 0;
            VkSwapchainKHR swapchain = VK_NULL_HANDLE;
            Uint32 swapchainImageIndex = 0;
            Bool isDefault = false, framebufferSrgb = false;
            Bool operator==(const WireDrawPassKey&) const = default;
            // Default values, vectors emptied but their storage kept (the per-draw scratch).
            void ResetKeepingCapacity() {
                auto keptAttachments = Move(attachments);
                auto keptColorReferences = Move(colorReferences);
                *this = WireDrawPassKey{};
                keptAttachments.clear();
                keptColorReferences.clear();
                attachments = Move(keptAttachments);
                colorReferences = Move(keptColorReferences);
            }
        };
        struct WireDrawPassCacheEntry {
            WireDrawPassKey key;
            UniquePtr<RenderPassEntry> pass;
            Vector<VkImageView> views;
            // The last submission that recorded this pass (P15 S0): it may be destroyed only once
            // that submission completed.
            Uint64 lastUseSubmit = 0;
        };
        // Objects only: every draw still ends/begins its pass and records the
        // existing memory dependencies. P15 S0: ONE cache across frame slots, so a framebuffer
        // drawn every frame keeps its render pass, framebuffer and views instead of rebuilding them
        // each frame. An entry whose key's image epochs moved can never hit again (the key holds
        // them) and is destroyed once its last use completed (ClearWireDrawPassCache).
        Bool AcquireCachedWireDrawPass(const WireDrawPassKey& key);
        void ClearWireDrawPassCache(Uint32 frameIndex);
        void ClearAllWireDrawPassCaches();
        void DestroyWireDrawPassCacheEntry(WireDrawPassCacheEntry& entry);
        Vector<WireDrawPassCacheEntry> m_wireDrawPassCache;
        WireDrawPassKey m_wireDrawPassKey;
        WireRenderPassCompatibilityTable m_wireRenderPassCompatibility;
        // The interned compatibility id of m_wireDrawPass's attachments, or 0 when not known yet.
        // A draw that continues the pass (its WireDrawPassKey compares equal, which covers every
        // input of the compatibility key) reuses it instead of building and hashing the key again.
        Uint64 m_wireDrawPassCompatibilityId = 0;
        // P14: SetupWireDraw's per-draw containers, cleared and refilled every draw instead of
        // allocated and freed (the heap was ~10% of a Minecraft frame on the wire arm). A nested
        // SetupWireDraw - nothing does that today - would use a local set (WireDraw.inc).
        struct WireDrawScratch {
            VertexInputStateFactory::BackendVertexInputState input;
            Vector<BufferSlice> vertexSlices;
            Vector<VkAttachmentDescription> attachments;
            Vector<VkImageViewCreateInfo> viewInfos;
            Vector<VkAttachmentReference> colors;
            Vector<WireImage> sameLayoutAttachments;
            WireDrawPassKey passKey;
            WireRenderPassCompatibilityKey compatibilityKey;
            struct GpuWriteMark {
                MG_Pipe::MGPipeHandle storage;
                Uint32 level, layer, layers;
            };
            Vector<GpuWriteMark> gpuWriteMarks;
        };
        // The current wire pass's attachments were marked GPU-written at this StagedTextureStore
        // clear generation (SetupWireDraw); false whenever the pass changes.
        Bool m_wirePassMarksValid = false;
        // P14: THE OPEN PASS'S ATTACHMENTS, as the last full attachment walk resolved them. A draw
        // into the same framebuffer record (FramebufferSerial moves on every framebuffer write,
        // ContextSerial on every applier reset) while the pass is still open on this command buffer,
        // with no texture image re-created or erased, and every attachment still in GENERAL with no
        // pending upload, would resolve exactly these again and continue the pass: it skips both
        // resolves and the pass key. Never for the default framebuffer, whose image index moves
        // with each acquire.
        struct WirePassMemo {
            Bool valid = false;
            Uint64 framebufferSerial = 0, contextSerial = 0;
            MG_Pipe::MGPipeHandle drawFramebuffer = MG_Pipe::kMGPipeNullHandle;
            Uint64 textureImageEpoch = 0, resourceEraseEpoch = 0;
            Bool framebufferSrgb = false;
            Uint64 passHash = 0;
            struct Attachment {
                VkTextureManager::TextureResource* resource = nullptr;
                MG_Pipe::MGPipeHandle storage = MG_Pipe::kMGPipeNullHandle;
                Bool renderbuffer = false;
            };
            Vector<Attachment> attachments;
            Vector<WireDrawScratch::GpuWriteMark> marks;
        };
        WirePassMemo m_wirePassMemo;
        Bool WirePassMemoHolds(const MG_Pipe::MGPFramebufferState& fbo, const FrameContext::FrameData& frame) const;
        Uint64 m_wirePassMarksClearGeneration = 0;
        WireDrawScratch m_wireDrawScratch;
        Bool PrepareWireDrawPass(FrameContext::FrameData& frame, const MG_Pipe::MGPFramebufferState& fbo,
                                 WireDrawScratch& scratch, Bool passContinues, Bool& continuing);
        // P15: glClear / glClearBuffer* into the attachments of the wire draw pass, recorded inside it
        // (vkCmdClearAttachments). Returns the GL clear-mask bits it could not take; those go through
        // the standalone path.
        GLbitfield ClearWireFramebufferInPass(const MG_Pipe::MGPFramebufferState& fbo,
                                              const ClearAttachmentPayload& payload, GLint drawbuffer);
        struct WireRetiredObjects {
            Uint64 submitIndex = 0;
            UniquePtr<RenderPassEntry> drawPass;
            Vector<VkFramebuffer> framebuffers;
            Vector<VkRenderPass> renderPasses;
            Vector<VkImageView> imageViews;
            Vector<VkDescriptorPool> descriptorPools;
            // Transfer/resolve scratch storage belongs to the submission that
            // consumes it, just like the views and framebuffer above.
            Vector<UniquePtr<VkTextureManager::TextureResource>> textures;
            Vector<UniquePtr<VkBufferObject>> buffers;
        };
        void RetireWireObjects(WireRetiredObjects objects);
        void CollectWireObjects(Uint64 completedSubmit, Bool all = false);
        Vector<WireRetiredObjects> m_wireRetiredObjects;
        Uint32 m_wirePreparationDepth = 0;
        // P8-D: set only while DrawWireIndirectNative is inside SetupDraw, so SetupWireDraw can
        // tell the wire arm's own indirect draw from a monolith indirect entry point reached under
        // a transport (still the `buffer-legacy-arm` role violation).
        Bool m_wireNativeIndirectDraw = false;
        void DispatchWireCompute(GLuint x, GLuint y, GLuint z);
        // P8-SV: glDispatchComputeIndirect on the wire arm - vkCmdDispatchIndirect from the
        // dispatch-indirect store, as the monolith arm issues it (WireDraw.inc).
        void DispatchWireComputeIndirect(GLintptr offset);
        // False: the device was lost and the session latched; the caller drops its draw/dispatch.
        Bool RewindWireDescriptorSetsIfDue();
        UniquePtr<RenderPassEntry> m_wireDrawPass;
        Vector<VkImageView> m_wireDrawViews;
        VkPipeline GetOrCreatePipelineWithInput(GLenum mode, const MagmaProgramSource& program,
            const ProgramFactory::VkProgramObject& programObj, ProgramFactory::CompileOptionFlags transformFlags,
            const VertexInputStateFactory::BackendVertexInputState& vis, const RenderPassEntry& renderPassEntry,
            Bool primitiveRestartEnable, Uint64 wireRenderPassCompatibilityId = 0);

        void CopyWireFramebufferToTexture(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                           GLint x, GLint y, GLsizei width, GLsizei height);
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
        // A later fence alone cannot retire earlier submissions. Wait for the
        // whole still-live prefix before advancing the completed submit floor.
        Bool WaitForSubmitsUpTo(Uint64 submitIndex, Uint64 timeoutNs);
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
        // Bounded recording: called at the top of every GL draw and dispatch, before anything
        // of that call is recorded. Once the open frame command buffer holds
        // MagmaMaxDrawsPerCommandBuffer draws/dispatches, submits it (FlushPendingCommands) so
        // the call continues on a fresh one, then waits until at most frames-in-flight + 1
        // submissions are outstanding. A driver backs a command buffer with GPU memory it only
        // returns when the buffer is freed (Adreno: one 16 KiB mapping per chunk), so a frame
        // that is never split grows it without bound - rd12's loading frame records ~1M draws
        // into one buffer and runs the process out of vm.max_map_count.
        void SplitOversizedRecording();

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
        // GL draws/dispatches recorded into the open frame command buffer; reset whenever a
        // recording begins (OnFrameCommandRecordingBegan). See SplitOversizedRecording.
        Uint32 m_drawsInRecording = 0;
        Uint64 m_oversizedRecordingSplits = 0;

        NativeWindowType m_window = 0;
        void* m_platformDisplay = nullptr;
        void* m_platformLibrary = nullptr;
        void* m_platformCloseDisplay = nullptr;
        // Whether the loader exposes VK_EXT_headless_surface, detected once in
        // CreateInstance() from the enumerated instance extensions. On desktop an
        // offscreen surface REQUIRES it: false is a clean, loud bring-up failure, never
        // a substituted window. (Android is the one exception and has its own path -
        // no Mali/Adreno driver seen so far exposes the extension, so a windowless
        // context is given an AImageReader ANativeWindow that is never displayed.)
        // ENABLED ON THIS INSTANCE, not merely offered: a renderer created for a window never asks
        // for the extension, and its later offscreen targets must not call an entry point the
        // instance does not have (OffscreenSurfaceRoute.h). Hence false until CreateInstance says so.
        Bool m_headlessSurfaceSupported = false;
        // VK_KHR_android_surface was enabled on this instance (the AImageReader route needs it).
        Bool m_androidSurfaceEnabled = false;
        // Android has the same shortfall: no Mali/Adreno driver seen so far exposes
        // VK_EXT_headless_surface, so a windowless (EGL pbuffer) context gets an
        // AImageReader's ANativeWindow to hand the WSI instead. Nothing is ever
        // displayed - the reader's images are simply never acquired. Owned here, so
        // Shutdown() deletes it.
        void* m_fallbackImageReader = nullptr;
        VulkanRendererConfig m_config;
        // Constructed for the app's window rather than a pbuffer (m_window is later filled in
        // for the pbuffer fallbacks too, so it cannot tell them apart).
        Bool m_presentsToAppWindow = false;
        // The m_config.SwapInterval the live swapchain was built for; Present compares the two.
        Optional<Int> m_swapchainSwapInterval;
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
        // Native subgroup topology, queried at device creation for the compute-module
        // subgroup repairs (SubgroupSupportPolicy.h) and the REQUIRE_FULL_SUBGROUPS
        // stage flag; 0 / false when the device has no usable compute subgroups or
        // MOBILEGL_MAGMA_DISABLE_SUBGROUP forced them off.
        Uint32 m_nativeSubgroupSize = 0;
        Bool m_nativeSubgroupSupported = false;
        Bool m_computeFullSubgroupsFeatureEnabled = false;
        // VkPhysicalDeviceSubgroupSizeControlProperties::maxComputeWorkgroupSubgroups;
        // 0 when the extension (and therefore the full-subgroups flag) is unavailable.
        Uint32 m_maxComputeWorkgroupSubgroups = 0;
        Bool m_unformattedFloatStorageImagesEnabled = false;
        // Set only after descriptor-indexing feature AND property queries prove that
        // update-after-bind is legal for every descriptor category this renderer emits.
        ProgramFactory::UpdateAfterBindLimits m_updateAfterBindLimits{};
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
        // shaderTessellationAndGeometryPointSize gates the PointSize built-in in a tessellation
        // or geometry stage, which desktop GL treats as an ordinary per-vertex output (writable,
        // and capturable by name through transform feedback). Cached at device creation and
        // handed to ProgramFactory, which refuses a program whose tessellation or geometry module
        // declares the matching SPIR-V capability while this is false - SetupDraw then skips its
        // draws (VkProgramObject::pointSizeCapabilityUnsupported) rather than building a pipeline
        // that is invalid usage.
        Bool m_tessellationAndGeometryPointSizeFeatureEnabled = false;
        // VK_EXT_custom_border_color. Vulkan's four predefined VkBorderColor values cover only
        // transparent/opaque black and opaque white; GL_TEXTURE_BORDER_COLOR is an arbitrary vec4 (or
        // an arbitrary ivec4/uvec4 through the "I" entry points). Without this extension a border
        // colour outside the palette has to be snapped to the nearest predefined one. Both features
        // are required together: customBorderColorWithoutFormat is what lets a sampler carry a custom
        // colour without naming the image format it will be paired with, which GL's sampler objects
        // cannot know. maxCustomBorderColorSamplers is a real device limit, so the sampler cache has
        // to be able to fall back to the snapped value once it is reached.
        Bool m_customBorderColorFeatureEnabled = false;
        Uint32 m_maxCustomBorderColorSamplers = 0;
        // sampleRateShading gates VkPipelineMultisampleStateCreateInfo::sampleShadingEnable, i.e.
        // glEnable(GL_SAMPLE_SHADING) + glMinSampleShading. Unlike dualSrcBlend this does NOT
        // hard-fail the draw when absent: sample shading is a rate hint, and every sample-rate
        // pipeline is still correct (just not per-sample) at the default rate - so the enable is
        // dropped and the draw proceeds, which is what a GL implementation with SAMPLES=1 does too.
        Bool m_sampleRateShadingFeatureEnabled = false;
        // multiViewport gates rasterizing into more than one of ARB_viewport_array's 16 viewports
        // (gl_ViewportIndex). m_maxRasterizableViewports is min(MAX_VIEWPORTS, device limit), or 1
        // when the feature is off, and is the viewportCount a gl_ViewportIndex-writing pipeline
        // declares - it is NOT what GL_MAX_VIEWPORTS reports, which is the frontend state width.
        Bool m_multiViewportFeatureEnabled = false;
        Uint32 m_maxRasterizableViewports = 1;
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
        PFNDrawIndexedIndirectCountFunc s_vkCmdDrawIndexedIndirectCount = nullptr;
        // P8-D: vkCmdDrawIndirectCount (same signature, same extension), for the wire arm's
        // glMultiDrawArraysIndirectCount. The monolith arm never loads it: it reads that count on
        // the CPU (DirectVulkan.cpp's MultiDrawArraysIndirectCount).
        PFNDrawIndexedIndirectCountFunc s_vkCmdWireDrawIndirectCount = nullptr;
        // VK_EXT_multi_draw entry points, loaded at device creation when the extension
        // (and its multiDraw feature) is enabled; null otherwise.
        PFN_vkCmdDrawMultiEXT s_vkCmdDrawMultiEXT = nullptr;
        PFN_vkCmdDrawMultiIndexedEXT s_vkCmdDrawMultiIndexedEXT = nullptr;

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
        PFN_vkCmdBindTransformFeedbackBuffersEXT s_vkCmdBindTransformFeedbackBuffersEXT = nullptr;
        PFN_vkCmdBeginTransformFeedbackEXT s_vkCmdBeginTransformFeedbackEXT = nullptr;
        PFN_vkCmdEndTransformFeedbackEXT s_vkCmdEndTransformFeedbackEXT = nullptr;
        // Counter buffers (one 4-byte slot per capture binding) let consecutive
        // draws within one glBeginTransformFeedback append GL-style. Transform feedback
        // objects can each hold an open, paused span at the same time, so the counters are
        // per object: one group of four slots each, handed out on first use.
        static constexpr SizeT kXfbCounterObjectSlots = 16;
        VkBufferObject m_xfbCounterBuffer;
        // Which transform feedback object owns each slot group, by the frontend's never-reused
        // lifetime id (0 = the slot is free). This used to be an UnorderedMap keyed on the GL
        // NAME, which is recycled by glGenTransformFeedbacks: a deleted-and-recreated object
        // inherited the dead one's slot, and since nothing ever removed an entry the map also
        // grew for the life of the context. A fixed table cannot do either: a group is taken over
        // only from an owner with no OPEN span (see CurrentXfbCounterSlot), so an object whose
        // counters can still be resumed never loses them, and a dead object's group comes back.
        Array<Uint64, kXfbCounterObjectSlots> m_xfbCounterSlotOwner{};
        // Tie-break among reclaimable groups only; never on its own, because the paused span the
        // groups exist for is by construction the least recently used one.
        Array<Uint64, kXfbCounterObjectSlots> m_xfbCounterSlotLastUse{};
        Uint64 m_xfbCounterSlotUseSerial = 0;
        // Set for a slot once a captured draw has been recorded into its span; selects
        // counter-buffer resume on the next captured draw of the same span.
        Array<Bool, kXfbCounterObjectSlots> m_xfbCountersValid{};
        Array<Uint64, kXfbCounterObjectSlots> m_xfbLastSeenGeneration{};
        // Counter slot group of the bound transform feedback object.
        Uint32 CurrentXfbCounterSlot();
        // Wraps a recorded draw with BeginTransformFeedbackEXT/EndTransformFeedbackEXT
        // when GL transform feedback is active; binds capture buffers on demand.
        Bool BeginXfbCaptureForDraw(FrameContext::FrameData& frame);
        Bool BeginXfbCaptureWithBuffers(FrameContext::FrameData& frame, Uint32 count,
                                       const VkBuffer* buffers, const VkDeviceSize* offsets,
                                       const VkDeviceSize* sizes);
        Bool BeginWireXfbCaptureForDraw(FrameContext::FrameData& frame);
        Uint32 m_currentDrawXfbBufferCount = 0;
        Uint32 m_currentDrawXfbBufferMask = 0;
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
        // GL_PRIMITIVES_GENERATED reroute for draws made while transform feedback is
        // INACTIVE. The stream pool's primitivesNeeded is defined to count those draws
        // too, but a Mali driver (and Mesa lavapipe) answers 0 unless a capture span
        // is open (the CTS's tessellator-measuring shape). Where the bring-up probe
        // finds that defect with a working control - or
        // MOBILEGL_MAGMA_PRIMGEN_QUERY_REROUTE forces it - such draws accumulate the
        // GENERATED count through this pool instead, whose type the arming picks:
        // VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT where the device hosts the dedicated
        // query with its rasterizer-discard feature (exact semantics by definition -
        // the extension exists because GL needs this count without a capture), else a
        // VK_QUERY_TYPE_PIPELINE_STATISTICS pool over clipping-stage invocations (one
        // per primitive reaching primitive clipping - after every vertex processing
        // stage, before rasterizer discard - which is the same set).
        // XFB-ACTIVE draws keep the stream slot (exact today, and WRITTEN needs it);
        // every draw with no open capture - a PAUSED span's draws included - takes a
        // reroute slot, and the span then ignores the frontend's CPU paused-primitive
        // counter rather than adding it on top (see IsPrimGenRerouteArmed): that
        // counter is written by only 3 of the ~15 draw entry points and answers 0 for
        // GL_PATCHES, so it cannot price the draws this reroute exists to repair. One
        // GL query span may therefore hold slots of both pools.
        Bool m_pipelineStatisticsQueryFeatureEnabled = false;
        // VK_EXT_primitives_generated_query: base feature, and the
        // ...WithRasterizerDiscard feature without which a discarding draw inside the
        // query is invalid usage (so the reroute never picks the dedicated pool on a
        // base-only device - GL applications toggle discard freely).
        Bool m_primitivesGeneratedQueryFeatureEnabled = false;
        Bool m_primitivesGeneratedQueryDiscardFeatureEnabled = false;
        // tessellationShader was enabled at device creation (it is taken whenever the
        // device advertises it); gates the probe's PATCHES shape.
        Bool m_tessellationShaderFeatureEnabled = false;
        MG_Util::SelfTest::PrimGenRerouteKind m_primGenRerouteKind =
            MG_Util::SelfTest::PrimGenRerouteKind::None;
        // The bring-up probe measured this device's stream query as counting draws made
        // with no capture span open (the StreamCounts verdict) - so it counts the
        // PAUSED-span ones too, through the stream slot they take when nothing is
        // rerouted. Only the probe can know this, so it stays false wherever the probe
        // is not consulted (the forced arms), which keeps those lanes' accounting as it
        // was.
        Bool m_primGenStreamCountsXfbInactiveDraws = false;
        VkQueryPool m_primGenReroutePool = VK_NULL_HANDLE;
        Uint32 m_primGenRerouteSlotCursor = 0;
        Vector<Uint32> m_primGenRerouteActiveSlots;
        Bool m_primGenRerouteSlotOpen = false;
        Uint32 m_primGenRerouteOpenSlot = 0;
        // Runs the bring-up probe (memoized per process) and decides
        // m_primGenRerouteKind. Called at the end of device creation: it records on
        // m_graphicsQueue, which nothing else is using yet.
        void ArmPrimGenReroute();

    public:
        // Whether a GENERATED span opened now will have the draws made while the GL
        // span is PAUSED counted on the GPU - through the reroute pool, which takes
        // every draw with no open capture, or (where the reroute is not armed because
        // the stream query was measured to count capture-less draws) through the stream
        // slot such a draw still takes. The frontend's CPU paused-primitive counter
        // must not be added on top of either: it would double count, and it cannot
        // price the draws that matter anyway - only 3 of the ~15 draw entry points
        // write it and it answers 0 for GL_PATCHES. Read once per span, after
        // StartXfbQueryCapture (whose pool creation may disarm the reroute).
        Bool ArePausedDrawsGpuCounted() const;
        // kind: 0 = PRIMITIVES_WRITTEN, 1 = PRIMITIVES_GENERATED.
        Bool StartXfbQueryCapture(Uint32 kind);
        void StopXfbQueryCapture(Uint32 kind, Vector<Uint32>& outSlots, Vector<Uint32>& outRerouteSlots);
        Bool ResolveXfbQueryResult(const Vector<Uint32>& slots, const Vector<Uint32>& rerouteSlots,
                                   Bool wantGenerated, Uint64& outPrimitives);

    private:
        void BeginXfbQueryForDraw(VkCommandBuffer commandBuffer, Bool xfbActive);
        void EndXfbQueryForDraw(VkCommandBuffer commandBuffer);

        VkCommandPool m_commandPool = VK_NULL_HANDLE;

        VkBufferManager m_bufferManager;

        Uint m_imageIndexAcquired = 0;
        // The swapchain image GL addresses as the default framebuffer: the one a readback, a
        // blit source or a copy out of it resolves, and the one the app last rendered into.
        // Present() runs ahead - it presents the frame and acquires the next image without
        // waiting for the apply that would render into it (kWaitPresent) - so this must NOT
        // follow m_imageIndexAcquired. Before it existed a readback resolved the freshly
        // acquired image, which nothing had written yet: the pure-black sundial-lite frame.
        Uint m_defaultFramebufferImageIndex = 0;
        FrameContext m_frameContext;

        // Resolves the swapchain image index a READ of the default framebuffer must use. A
        // non-default framebuffer ignores the index; it keeps the acquired one, which still
        // keys its render-pass cache.
        Uint32 DefaultFramebufferReadIndex(Bool isDefaultFramebuffer) const {
            return isDefaultFramebuffer ? m_defaultFramebufferImageIndex : m_imageIndexAcquired;
        }

        // Same for a WRITE: the default framebuffer starts addressing the image Present()
        // acquired only once something renders into it again.
        Uint32 DefaultFramebufferWriteIndex(Bool isDefaultFramebuffer) {
            if (isDefaultFramebuffer) m_defaultFramebufferImageIndex = m_imageIndexAcquired;
            return DefaultFramebufferReadIndex(isDefaultFramebuffer);
        }

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
            Uint64 wireRenderPassCompatibilityId = 0;
            // The PRE-HANDLE arm's key component (P2 brief D12.1), and 0 in every entry the
            // handle arm mints. VALUE hash of the pipeline-relevant fixed-function state (see
            // ComputePipelineStateHash), not the monotonic pipeline-state version: the version
            // never repeats, so a per-draw GL_BLEND toggle would miss all entries forever even
            // though the state alternates between two values the memo already holds.
            Uint64 pipelineStateHash = 0;
            // The HANDLE arm's key component, and the whole of D12.1: the CLIENT already
            // hashed the pipeline subset of RenderStateParameters and minted a content-
            // addressed CSO for it (MG_Pipe/MGPipeRenderStateSpans.h, MG_Impl/Pipe/CsoCache),
            // so re-hashing the same 396 bytes here was work the boundary had already done.
            // Two draws share a CSO handle exactly when their pipeline bytes are equal, and
            // the client's subset is a strict SUPERSET of what ComputePipelineStateHash read,
            // so the handle discriminates at least as finely as the hash it replaces.
            //
            // renderPassHash STAYS beside it and is what keeps this key complete: the CSO
            // carries GL state only, while colorAttachmentCount and the rasterization sample
            // count - which ComputePipelineStateHash folded in through its signature and
            // through ResolveEffectiveSampleMask - are render-pass facts that the render-pass
            // hash already separates.
            //
            // Null in an entry minted by the legacy arm, so entries of the two arms can never
            // match each other: the compare below tests BOTH components.
            MG_Pipe::MGPipeHandle renderStateCso = MG_Pipe::kMGPipeNullHandle;
            ProgramFactory::CompileOptionFlags transformFlags = {};
            // Baked into the pipeline (PipelineFactory::ComputeHash mixes it), and NOT derivable
            // from anything else in this key: it depends on whether the draw is indexed and on the
            // index type, neither of which the mode/program/state hashes carry. Without it an
            // indexed and a non-indexed draw over the same program and state collide on one entry
            // and the second one gets the first one's restart setting.
            Bool primitiveRestartEnable = false;
            VkPipeline pipeline = VK_NULL_HANDLE;
        };
        static constexpr Uint32 kPipelineMemoSize = 8;
        PipelineMemoEntry m_pipelineMemo[kPipelineMemoSize];
        Uint32 m_pipelineMemoCount = 0;
        Uint32 m_pipelineMemoNext = 0;

        // P2 D12.1's arm selector, and the whole of the pipeline memo's re-key. Returns the
        // render-state CSO this draw is keyed on, or the null handle when the pre-handle arm
        // is the one that runs.
        //
        // Under the handle arm the memo's state key IS this handle. The client hashed those
        // 396 pipeline bytes when it minted the CSO (MGPipeComputePipelineSubsetHash), so
        // recomputing an overlapping hash here was work the boundary had already done; the
        // client's pipeline subset is a strict SUPERSET of what ComputePipelineStateHash read,
        // so the handle discriminates at least as finely as the hash it replaces. What the
        // handle does NOT carry is the render-pass side - colorAttachmentCount and the
        // rasterization sample count, which ComputePipelineStateHash folded in through its
        // signature and through ResolveEffectiveSampleMask - and that is exactly why
        // entry.renderPassHash stays in the key beside it.
        //
        // The arm is live only when the render-state subsystem is migrated in this run AND the
        // client has actually bound a CSO. The second half is not belt and braces: a tree whose
        // tracker does not emit create/bind_render_state yet has no handle to key on, and
        // delete_render_state clears the binding (MG_Pipe/PipeApply.cpp), so the null handle is
        // reachable on any tree. Keying every draw on it would alias every render state onto
        // one memo entry, so a null handle means "fall back to a state hash" - never an abort,
        // and never a per-draw consultation of the legacy-memo lever: bit 0 is not a Track-H
        // subsystem (D14 labels only bits 5 and 6 that), and the lever's Fatal is a STARTUP
        // one, in MagmaPipeValidateSubsystemConfiguration.
        //
        // The fallback is warned ONCE rather than logged at debug, and that is deliberate: a
        // silent fallback is what makes "the CSO arm never ran" easy to miss. W is compiled in
        // at every shipped log level.
        //
        // The latch is a plain member bool, NOT MGLOG_W_ONCE. MOBILEGL_LOG_ONCE_INTERNAL
        // (MG_Util/Debug/Log.h) is an UNCONDITIONAL std::atomic_flag::test_and_set - a locked
        // xchg, executed on every evaluation, not "one static bool test" as an earlier round of
        // this comment claimed - and this site is on the per-draw pipeline path in the very
        // configuration that reaches it (no tracker: every draw). ROADMAP.md:7 forbids leaving
        // instrumentation on a hot path, so the once-ness is one non-atomic, always-predicted
        // load of a member that is false exactly once. Single-threaded like the rest of the
        // renderer, and per renderer rather than per process, which is also the right scope: a
        // second context that never binds a CSO deserves to say so.
        //
        // What the absence of this warning from a run's log proves, EXACTLY: that no draw took
        // the fallback WHILE bit 0 was set. With kMGPipeSubsystemRenderState clear the function
        // returns before the latch, so absence proves nothing at all - and no draw is keyed on a
        // handle either. Grep the mask out of the log beside it (review v2 minor 3).
        //
        // Push-only by construction: the pull build does not compile this function at all, so
        // its two callers are statement-for-statement what they were (G1).
        //
        // [routed to the integrator, review v2 minor 11] MG_Pipe::MGPipeApplier() is ONE
        // process-global applier (MG_Pipe/PipeApply.cpp), not the per-context CSO store D2
        // specifies. In a multi-context process this reads whatever CSO another context last
        // bound. The defect is package A's and the fix belongs there; Magma is its only P2
        // consumer, so it is named here rather than left for both reviews to assume the other
        // caught it.
        MG_Pipe::MGPipeHandle ResolveBoundRenderStateCso() const {
            if (!MagmaPipeSubsystemOn(MG_Pipe::kMGPipeSubsystemRenderState)) {
                return MG_Pipe::kMGPipeNullHandle;
            }
            const MG_Pipe::MGPipeHandle boundCso = MG_Pipe::MGPipeApplier().BoundRenderStateCso;
            if (MG_Pipe::MGPipeHandleIsNull(boundCso) && !m_pipelineCsoFallbackWarned) {
                m_pipelineCsoFallbackWarned = true;
                MGLOG_W("MGPipe: kMGPipeSubsystemRenderState is on but no render-state CSO is "
                        "bound; the pipeline memo is running on a state hash, not on the CSO "
                        "handle (no tracker on this build, or a draw between "
                        "delete_render_state and the next bind)");
            }
            return boundCso;
        }
        // Latch for the warning above. Mutable because the resolve is const and the latch is
        // not part of the renderer's observable state.
        mutable Bool m_pipelineCsoFallbackWarned = false;
        // The memo key's STATE-HASH half, for a draw that has no CSO handle to key on: the
        // pre-handle arm, and the fallback of D12.1's handle arm. Cached on the pipeline-state
        // version plus the two render-pass facts the hash's inputs depend on, so an unchanged
        // (version, colorAttachmentCount, sampleCount) proves the bytes are unchanged.
        //
        // [deviation from D12.1] The brief deletes this gate and its cached fields outright.
        // They cannot go while a no-CSO draw is reachable - and it is, on any tree: a draw
        // between delete_render_state and the next bind has no handle. On a tree whose tracker
        // binds a CSO these five words are written once and never read again; they retire for
        // real when the pull path does, at P13.
        Uint64 ResolveFallbackPipelineStateHash(Uint renderStateVersion, Uint32 colorAttachmentCount,
                                                VkSampleCountFlagBits rasterizationSamples) {
            if (!m_pipelineStateHashValid || m_pipelineStateHashVersion != renderStateVersion ||
                m_pipelineStateHashColorCount != colorAttachmentCount ||
                m_pipelineStateHashSampleCount != rasterizationSamples) {
                m_pipelineStateHash = ComputePipelineSubsetStateHashFallback();
                m_pipelineStateHashVersion = renderStateVersion;
                m_pipelineStateHashColorCount = colorAttachmentCount;
                m_pipelineStateHashSampleCount = rasterizationSamples;
                m_pipelineStateHashValid = true;
            }
            return m_pipelineStateHash;
        }
        // The same answer as ComputePipelineStateHash, computed from the P2 chunk table
        // instead of from a hand-written field list, for the build that compiles no
        // pre-handle arm (cmake -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF). It is the CLIENT's own
        // hash function - MGPipeComputePipelineSubsetHash over the 396 pipeline bytes - so a
        // draw keyed on it and a draw keyed on a CSO handle are keyed on the same equivalence
        // class of state, and the render-pass facts stay separated by renderPassHash either
        // way. This is what makes the no-legacy build RUNNABLE rather than a configuration
        // that aborts on the first draw that arrives without a CSO.
        Uint64 ComputePipelineSubsetStateHashFallback() const;
        // The effective GL_SAMPLE_MASK word for a draw at this rasterization sample count; see
        // the definition for the GL-vs-Vulkan rule it reconciles. Shared by the pipeline payload
        // and the pipeline-state memo word so the two cannot disagree. NOT part of the legacy
        // arm: it is a PAYLOAD computation that depends on rasterizationSamples, so it survives
        // the re-key and keeps reading Multisample / SampleMask / SampleMaskValue out of the
        // working block.
        Uint32 ResolveEffectiveSampleMask(VkSampleCountFlagBits rasterizationSamples) const;
        // ResolveFallbackPipelineStateHash's cache. Written once and never read again on a
        // build whose client binds a render-state CSO; see that function for why it survives
        // the re-key at all.
        Uint m_pipelineStateHashVersion = 0;
        Uint32 m_pipelineStateHashColorCount = 0;
        // The sample count the cached hash was computed at. A pipeline-state input now depends on
        // it (the effective sample mask), so a draw that changes only the target's sample count
        // has to recompute rather than reuse.
        VkSampleCountFlagBits m_pipelineStateHashSampleCount = VK_SAMPLE_COUNT_1_BIT;
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
        // context whose monotonic version the cache is keyed on. The handle arm has no
        // such cache to drop - a CSO handle is not derived from a monotonic version.
        void InvalidatePipelineMemo() {
            m_pipelineMemoCount = 0;
            m_pipelineMemoNext = 0;
            m_pipelineStateHashValid = false;
        }
        UnorderedMap<ProgramFactory::HashType, VkPipeline> m_computePipelines;
        UniquePtr<ProgramFactory> m_programFactory;
        UniquePtr<UniformManager> m_uniformManager;
        UniquePtr<VertexInputStateFactory> m_vertexInputStateFactory;
        UniquePtr<VkRenderPassManager> m_renderPassManager;
        UniquePtr<VkTextureManager> m_textureManager;
        UniquePtr<VkSamplerManager> m_samplerManager;
        UniquePtr<VkTimerQueryManager> m_timerQueryManager;
        BlitResources m_blitResources;
        DepthMipmapResources m_depthMipmapResources;
        Vector<DeferredDepthMipmapCleanup> m_deferredDepthMipmapCleanup;

        // Set from the draw's resolved VkProgramObject on both the full and the fast setup paths;
        // read by BeginXfbCaptureForDraw, which has only GL state otherwise. See
        // VkProgramObject::xfbCaptureDeclined.
        Bool m_currentDrawXfbCaptureDeclined = false;

        // Per-draw scratch buffer (clear keeps capacity) - this path runs for every draw call and
        // must not allocate.
        Vector<VkVertexInputAttributeDescription> m_patchedAttributesScratch;


        void CreateInstance();
        VkResult SetupDebugMessenger();
        VkResult DestroyDebugMessenger();
        VkResult SetupDebugReportCallback();
        void DestroyDebugReportCallback();
        VkDebugUtilsMessengerCreateInfoEXT PopulateDebugMessengerCreateInfo();
        // False (logged) when the target's surface cannot be built; the renderer then has no surface.
        Bool CreateSurface();
        void PickPhysicalDevice();
        void CreateLogicalDeviceAndQueues();
        void CreateAllocator();
        void DestroyAllocator();
        void CreateSwapchain();
        void CreateCommandPool();

        // Whether THIS draw's primitive stream restarts, and therefore what
        // VkPipelineInputAssemblyStateCreateInfo::primitiveRestartEnable must be. Resolved by the
        // caller because it needs two facts a pipeline cannot see: whether the draw is indexed at
        // all (GL primitive restart acts on the index stream, so it is a no-op for glDrawArrays),
        // and the index TYPE (an application restart index that does not fit the type matches no
        // index, so that draw restarts nowhere - see UploadAndBindIndexBuffer).
        Bool ResolvePrimitiveRestartEnable(Flags<DrawSetupAspect> aspects,
                                           const IndexBufferView* pIndexBufferView) const;

        VkPipeline GetOrCreateComputePipeline(const ProgramFactory::VkProgramObject& programObj);
        void DestroyComputePipelines();
        // The per-draw dynamic-state tail (viewport, scissor, blend constants, depth
        // bias, line width, stencil), gated behind one render-state-parameters-version
        // compare per command buffer - see the gate fields in DynamicStateShadow.
        // viewportCount is the bound pipeline's declared viewport count: 1 for every program that
        // does not write gl_ViewportIndex (the memoized fast path), otherwise the renderer's
        // rasterizable viewport count, which takes the unmemoized array path.
        void ApplyDynamicDrawStateTail(FrameContext::FrameData& frame, const IntVec2& extent, Bool isDefaultFbo,
                                       Uint32 viewportCount = 1);
        void ApplyMultiViewportDynamicState(VkCommandBuffer commandBuffer, Uint32 viewportCount, const IntVec2& extent,
                                            VkSurfaceTransformFlagBitsKHR preTransform, Bool isDefaultFbo);
        VkRect2D ComputeGLScissorRect(Uint32 index, const IntVec2& extent,
                                      VkSurfaceTransformFlagBitsKHR preTransform, Bool isDefaultFbo) const;
        // How many viewports a draw with this program rasterizes into: 1 unless the program
        // assigns gl_ViewportIndex AND the device enabled multiViewport. Both the pipeline's
        // baked viewportCount and the dynamic arrays come from this one answer, so they cannot
        // disagree.
        Uint32 ResolveDrawViewportCount(Bool programWritesViewportIndex) const {
            return programWritesViewportIndex && m_multiViewportFeatureEnabled ? m_maxRasterizableViewports : 1u;
        }

        Bool InitializeBlitResources();
        Bool InitializeDepthMipmapResources();
        void ShutdownBlitResources();
        void ShutdownDepthMipmapResources();
        void CollectDeferredDepthMipmapCleanup(Uint32 frameIndex);
        void DestroyDeferredDepthMipmapCleanup();
        Bool SubmitReadbackCommandsAndWait(FrameContext::FrameData& frame);

    public:
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
