// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkRenderPassManager.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "SwapchainObject.h"
#include "VkClearManager.h"
#include "VkTextureManager.h"
#include "../VkIncludes.h"
#include "../VulkanRendererConfig.h"
#include "MG_State/GLState/FramebufferState/FramebufferObject.h"

#include <Includes.h>
#include <unordered_map>
#include <vk_mem_alloc.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    enum class TrackedAttachmentTarget : Uint8 {
        Texture,
        Renderbuffer,
        SwapchainColor,
        SwapchainDepthStencil
    };

    struct PendingClearAttachmentInfo {
        // Index into the render pass attachment descriptions (VkRenderPassBeginInfo::pClearValues space).
        Uint32 attachmentIndex = 0;
        // Index into the subpass pColorAttachments (VkClearAttachment::colorAttachment space) — the GL
        // draw-buffer slot. Differs from attachmentIndex when earlier slots are GL_NONE/incomplete.
        // Only meaningful for color clears.
        Uint32 colorAttachmentSlot = 0;
        PendingClearKey key{};
        MG_State::GLState::RenderbufferObject* renderbuffer = nullptr;
        Bool hasInlinePayload = false;
        ClearAttachmentPayload inlinePayload{};
    };

    struct TrackedAttachmentLayoutInfo {
        TrackedAttachmentTarget target = TrackedAttachmentTarget::Texture;
        WeakPtr<MG_State::GLState::ITextureObject> texture;
        // Identity-compare shortcut for the per-draw "does the active pass use
        // this sampled texture" probe: comparing this against a LIVE texture's
        // address needs no weak_ptr::lock (two refcount atomics per probe).
        // May dangle once the texture dies - compare only, never dereference.
        MG_State::GLState::ITextureObject* textureRaw = nullptr;
        WeakPtr<MG_State::GLState::RenderbufferObject> renderbuffer;
        Uint32 textureMipLevel = 0;
        Uint32 swapchainImageIndex = 0;
        VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct DepthStencilAttachmentLoadInfo {
        VkAttachmentLoadOp depthLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        VkAttachmentLoadOp stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    DepthStencilAttachmentLoadInfo ResolveDepthStencilAttachmentLoadInfo(
        VkImageLayout trackedLayout, Bool clearDepth, Bool clearStencil);
    IntVec2 ResolveRenderPassFramebufferExtent(Bool isDefaultFbo, const TextureSize& attachmentExtent,
                                               VkExtent2D swapchainExtent);

    struct RenderPassEntry {
        static inline VkDevice s_device;
        static inline Vector<VkTextureManager::TextureResource*> s_textureResourcesScratch;
        Uint64 hash = 0;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        Uint64 compatibilityHash = 0;
        Vector<PendingClearAttachmentInfo> pendingClearAttachments;
        Vector<TrackedAttachmentLayoutInfo> trackedAttachmentLayouts;
        Uint32 attachmentCount = 0;
        Uint32 colorAttachmentCount = 0;
        Bool hasDepthStencilAttachment = false;
        VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;
        IntVec2 extent = {0, 0};
        // VkFramebufferCreateInfo::layers of the entry's framebuffer (>1 for layered GL attachments).
        Uint32 layers = 1;
        // Frame counter value of the last GetOrCreateRenderPass hit; drives cache eviction.
        Uint64 lastUsedFrame = 0;

        RenderPassEntry() = default;
        RenderPassEntry(const RenderPassEntry&) = delete;
        RenderPassEntry(RenderPassEntry&& that) noexcept {
            std::swap(hash, that.hash);
            std::swap(renderPass, that.renderPass);
            std::swap(framebuffer, that.framebuffer);
            std::swap(compatibilityHash, that.compatibilityHash);
            std::swap(pendingClearAttachments, that.pendingClearAttachments);
            std::swap(trackedAttachmentLayouts, that.trackedAttachmentLayouts);
            std::swap(attachmentCount, that.attachmentCount);
            std::swap(colorAttachmentCount, that.colorAttachmentCount);
            std::swap(hasDepthStencilAttachment, that.hasDepthStencilAttachment);
            std::swap(sampleCount, that.sampleCount);
            std::swap(extent, that.extent);
            std::swap(layers, that.layers);
            std::swap(lastUsedFrame, that.lastUsedFrame);
        }
        // Move ASSIGNMENT, not just construction. The destructor below suppresses the
        // implicit one, which left the type move-constructible but not move-assignable -
        // and therefore not swappable, which std::swap(pair&, pair&) requires. That was
        // invisible while UnorderedMap only ever move-CONSTRUCTED an element into a fresh
        // slot. ska::flat_hash_map probes robin-hood: inserting swaps the entry being
        // placed against the one already sitting in the slot whenever it has travelled
        // further from its desired position, so the mapped type has to be swappable or the
        // whole table fails to instantiate.
        //
        // Swap-based, exactly like the move constructor: our old handles land in `that`
        // and are destroyed when it dies, so ownership stays with a single object and no
        // VkRenderPass/VkFramebuffer is leaked or double-destroyed.
        RenderPassEntry& operator=(RenderPassEntry&& that) noexcept {
            if (this != &that) {
                std::swap(hash, that.hash);
                std::swap(renderPass, that.renderPass);
                std::swap(framebuffer, that.framebuffer);
                std::swap(compatibilityHash, that.compatibilityHash);
                std::swap(pendingClearAttachments, that.pendingClearAttachments);
                std::swap(trackedAttachmentLayouts, that.trackedAttachmentLayouts);
                std::swap(attachmentCount, that.attachmentCount);
                std::swap(colorAttachmentCount, that.colorAttachmentCount);
                std::swap(hasDepthStencilAttachment, that.hasDepthStencilAttachment);
                std::swap(sampleCount, that.sampleCount);
                std::swap(extent, that.extent);
                std::swap(layers, that.layers);
                std::swap(lastUsedFrame, that.lastUsedFrame);
            }
            return *this;
        }
        RenderPassEntry(
            Uint64 hash,
            VkRenderPass renderpass,
            VkFramebuffer framebuffer,
            Uint64 compatibilityHash,
            const Vector<PendingClearAttachmentInfo>& pendingClearAttachments,
            const Vector<TrackedAttachmentLayoutInfo>& trackedAttachmentLayouts,
            Uint32 attachmentCount,
            Uint32 colorAttachmentCount,
            Bool hasDepthStencilAttachment,
            VkSampleCountFlagBits sampleCount,
            IntVec2 extent, Uint32 layers):
            hash(hash),
            renderPass(renderpass),
            framebuffer(framebuffer),
            compatibilityHash(compatibilityHash),
            pendingClearAttachments(Move(pendingClearAttachments)),
            trackedAttachmentLayouts(Move(trackedAttachmentLayouts)),
            attachmentCount(attachmentCount),
            colorAttachmentCount(colorAttachmentCount),
            hasDepthStencilAttachment(hasDepthStencilAttachment),
            sampleCount(sampleCount),
            extent(extent),
            layers(layers)
        {}

        ~RenderPassEntry() {
            if (renderPass != VK_NULL_HANDLE) {
                vkDestroyRenderPass(s_device, renderPass, nullptr);
            }
            if (framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(s_device, framebuffer, nullptr);
            }
        }

        Bool CompatibleWith(const RenderPassEntry& that) const {
            return this->compatibilityHash == that.compatibilityHash;
        }

        Bool CompatibleWith(Uint64 compatibilityHash) const {
            return this->compatibilityHash == compatibilityHash;
        }
    };

    struct ActiveRenderPassInfo {
        Uint64 hash = 0;
        Uint64 compatibilityHash = 0;
        Vector<TrackedAttachmentLayoutInfo> trackedAttachmentLayouts;
        IntVec2 extent = {0, 0};

        Bool CompatibleWith(const RenderPassEntry& that) const {
            return compatibilityHash == that.compatibilityHash;
        }

        Bool CompatibleWith(Uint64 thatCompatibilityHash) const {
            return compatibilityHash == thatCompatibilityHash;
        }
    };

    class VkRenderPassManager {
    public:
        using HashType = Uint64;

        // Notified once per OnPresent sweep with every aged-out entry's VkRenderPass
        // value: pipelines are hashed on the raw handle, and once destroyed the value
        // may be recycled for an incompatible pass, so dependent caches must purge
        // everything keyed on them before any new pass can be created (the sweep and
        // the notification run back-to-back with no creation in between; observers
        // compare the values, never dereference them). Batched so a mass-idle cohort
        // (shader-pack switch, dimension exit) costs the observer one pipeline-cache
        // scan, not one per dying pass. The wholesale paths
        // (Shutdown/RecreateSwapchain) do not notify - their callers already drop
        // every pipeline outright.
        class IEvictionObserver {
        public:
            virtual ~IEvictionObserver() = default;
            virtual void OnRenderPassesDestroyed(const Vector<VkRenderPass>& renderPasses) = 0;
        };

        VkRenderPassManager(VkDevice device,
            VkPhysicalDevice physicalDevice, VmaAllocator allocator, const VulkanRendererConfig& config,
            VkClearManager& clearManager, VkTextureManager& textureManager, SwapchainObject& swapchainObject);
        ~VkRenderPassManager();

        // Observer may be null (no notifications). Not owned.
        void SetEvictionObserver(IEvictionObserver* observer) { m_evictionObserver = observer; }

        Bool Initialize();
        void Shutdown();

        HashType ComputeHash(
            const MG_State::GLState::FramebufferObject& fbo,
            Uint32 swapchainImageIndex,
            Bool includePendingClear = true,
            Bool includeDefaultFboDepthStencil = true);
        // drawUsesDepthStencil: whether the operation about to run inside the pass
        // reads or writes the depth/stencil buffer (depth test or stencil test
        // enabled, or a depth/stencil clear). Only consulted for the DEFAULT
        // framebuffer: EGL undefines its ancillary buffers at every swap, so a
        // default-FBO pass whose draws provably never touch depth/stencil is
        // created WITHOUT the depth attachment - on a tiler that skips the whole
        // depth tile load AND store. The flavor only escalates: once a pass with
        // depth is active, later depth-less draws keep using it, and a depth-using
        // draw against a depth-less active pass resolves to a new (incompatible)
        // entry, which the caller's compatibility check turns into a pass split;
        // the new pass's depth loads DONT_CARE (content was undefined all along).
        RenderPassEntry& GetOrCreateRenderPass(const MG_State::GLState::FramebufferObject& fbo,
                                               Uint32 swapchainImageIndex,
                                               Bool drawUsesDepthStencil = true);
        void QueueRenderbufferClear(GLbitfield mask, const ClearFramebufferPayload& clearPayload,
                                    const MG_State::GLState::FramebufferObject& drawFbo);
        void QueueRenderbufferClear(const ClearAttachmentPayload& clearPayload,
                                    const MG_State::GLState::FramebufferAttachmentObject& attachment);
        void PopPendingRenderbufferClear(MG_State::GLState::RenderbufferObject* renderbuffer);
        // Frame boundary hook: ages the render-pass cache and evicts long-unused
        // entries (their command buffers retired many frames ago).
        void OnPresent();
        static Bool BeginRenderPass(VkCommandBuffer commandBuffer, RenderPassEntry& renderPassEntry);
        static Bool EndRenderPass(VkCommandBuffer commandBuffer);
        static ActiveRenderPassInfo* GetActiveRenderPass();
    private:
        VkDevice m_device = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VmaAllocator m_allocator = nullptr;
        const VulkanRendererConfig& m_config;
        VkClearManager& m_clearManager;
        VkTextureManager& m_textureManager;
        SwapchainObject& m_swapchainObject;
        UnorderedMap<Uint64, RenderPassEntry> m_renderPasses;
        // Monotonic frame counter (bumped in OnPresent) for render-pass cache aging.
        Uint64 m_frameCounter = 0;
        IEvictionObserver* m_evictionObserver = nullptr;

        // Bumped whenever a renderbuffer VkImage is (re)created; together with the texture
        // manager's image epoch this invalidates the render-pass fast path on any attachment
        // image recreation.
        Uint64 m_renderbufferImageEpoch = 1;

    public:
        // Bumped whenever a renderbuffer backing is (re)created; consecutive-draw
        // snapshots include it so an attachment respecify forces a re-resolve.
        Uint64 GetRenderbufferImageEpoch() const { return m_renderbufferImageEpoch; }

    private:

        // Per-draw fast-path memo for GetOrCreateRenderPass (dirty-flag state tracking): when the
        // framebuffer state is provably unchanged since the last resolution, the active render pass
        // is reused WITHOUT recomputing the expensive per-draw hash. Invalidated by FBO switch /
        // version change, swapchain rotation, any attachment image recreation (the two epochs),
        // or a pending clear. Portable to Vulkan 1.1 (no dynamic_rendering / imageless FB needed).
        Bool m_rpFastValid = false;
        const MG_State::GLState::FramebufferObject* m_rpFastFbo = nullptr;
        Uint16 m_rpFastFboVersion = 0;
        Uint32 m_rpFastSwapchainIndex = 0;
        Uint64 m_rpFastTexEpoch = 0;
        Uint64 m_rpFastRbEpoch = 0;
        Uint64 m_rpFastRenderPassHash = 0;
        // Whether the memoized entry carries a depth/stencil attachment; a
        // default-FBO resolution whose effective depth request differs must
        // miss the memo (the depth-less/depth-full flavors hash differently).
        Bool m_rpFastHadDepthStencil = false;

    public:
        struct RenderbufferResource {
            // deadSinceFrame sentinel: the owning weak reference has not been observed
            // expired. Dead resources age past every in-flight frame before Destroy
            // (see CollectRenderbufferGarbage); the GPU may still reference the image
            // for frames-in-flight frames after the GL object dies.
            static constexpr Uint64 kNeverObservedDead = UINT64_MAX;

            WeakPtr<MG_State::GLState::RenderbufferObject> renderbuffer;
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation allocation = nullptr;
            VkImageView view = VK_NULL_HANDLE;
            // UNORM reinterpretation of an sRGB image, used as the attachment view while
            // GL_FRAMEBUFFER_SRGB is disabled (raw writes). Null for non-sRGB formats.
            VkImageView unormTwinView = VK_NULL_HANDLE;
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_NONE;
            VkExtent2D extent = {0, 0};
            VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;
            TextureInternalFormat internalFormat = TextureInternalFormat::Unknown;
            Int samples = 0;
            // m_frameCounter value at which the weak reference was first seen expired.
            Uint64 deadSinceFrame = kNeverObservedDead;

            void Destroy(VkDevice device, VmaAllocator allocator);
        };

        // Public so the renderer's blit/copy/readback bindings can source renderbuffer
        // attachments the same way texture attachments go through the texture manager.
        RenderbufferResource* GetOrCreateRenderbufferResource(
            const SharedPtr<MG_State::GLState::RenderbufferObject>& renderbuffer);
        Bool GetPendingRenderbufferClear(MG_State::GLState::RenderbufferObject* renderbuffer,
                                         ClearAttachmentPayload& outPayload) const;

    private:
        struct PendingRenderbufferClear {
            WeakPtr<MG_State::GLState::RenderbufferObject> renderbuffer;
            ClearAttachmentPayload payload{};
        };

        // A superseded renderbuffer backing (glRenderbufferStorage respecify) parked
        // until enough frame boundaries have passed that no in-flight command buffer
        // can still reference it; destroyed in OnPresent (see RetireAgeFrames).
        struct DeferredRenderbufferRelease {
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation allocation = nullptr;
            VkImageView view = VK_NULL_HANDLE;
            VkImageView unormTwinView = VK_NULL_HANDLE;
            Uint64 deferredAtFrame = 0;
        };

        // Node-based std::unordered_map, deliberately NOT the open-addressing UnorderedMap:
        // callers cache a RenderbufferResource* - or a bare &resource->layout - and then make further
        // calls that touch this map. BlitFramebuffer is the one that bit: it resolves the source and
        // destination colour bindings (ResolveColorBlitBinding caches &rbResource->layout), then
        // materializes the source's pending clear, which looks that same resource up again. Growing
        // an open-addressed table relocates every element, so the cached pointer went on to name
        // freed storage still holding the pre-clear VK_IMAGE_LAYOUT_UNDEFINED; BlitFramebuffer bailed
        // out at "source image layout is undefined", silently dropping the blit -
        // renderbuffers_storage_multisample read back zero instead of the clear colour on exactly the
        // iterations that grew the table.
        //
        // Reordering the materialize ahead of the resolves - the fix ReadPixels got - does not cover
        // this: the destination resolve still runs after the source pointer is taken. The depth blit,
        // GetOrCreateRenderPass's depthRenderbufferResource and ReadDepthStencilPixels cache the same
        // kind of pointer, so the invariant belongs in the container rather than in a per-call-site
        // ordering rule. m_textureResources is node-based for the same reason.
        //
        // The case for keeping this node-based got STRONGER with ska::flat_hash_map, so do not read
        // the paragraph above as merely historical: ska erases by shifting the rest of the probe
        // cluster backwards into the hole, so erasing one renderbuffer relocates OTHER renderbuffers'
        // entries - a cached pointer can now be invalidated by a key it has nothing to do with, which
        // no call-site ordering rule can defend against. (What did change: ska's operator[] returns on
        // a hit before it runs its grow check, so a plain lookup of a PRESENT key no longer relocates.
        // That narrows the insert hazard; it does not touch the erase one.)
        std::unordered_map<MG_State::GLState::RenderbufferObject*, RenderbufferResource> m_renderbufferResources;
        UnorderedMap<MG_State::GLState::RenderbufferObject*, PendingRenderbufferClear> m_pendingRenderbufferClears;
        Vector<DeferredRenderbufferRelease> m_deferredRenderbufferReleases;
        // Supported sample counts per attachment format, so per-draw resource lookups
        // do not repeat vkGetPhysicalDeviceImageFormatProperties.
        UnorderedMap<VkFormat, VkSampleCountFlags> m_attachmentSampleCountsByFormat;

        Bool HasPendingRenderbufferClear(
            const MG_State::GLState::FramebufferAttachmentObject& attachment) const;
        void CollectRenderbufferGarbage();
        // Frame-boundary margin after which a resource last referenced by a retired
        // GL object (or superseded backing) is provably past every in-flight frame.
        Uint64 RetireAgeFrames() const;
        void DeferRenderbufferBackingRelease(RenderbufferResource& resource);
        void CollectDeferredRenderbufferReleases(Bool destroyAll);

        static inline XXH64_state_t* m_hashState = XXH64_createState();
        static inline ActiveRenderPassInfo s_activeRenderPass{};
        static inline Bool s_hasActiveRenderPass = false;
        static inline VkClearManager* s_clearManager = nullptr;
        static inline VkTextureManager* s_textureManager = nullptr;
        static inline SwapchainObject* s_swapchainObject = nullptr;
        static inline VkRenderPassManager* s_renderPassManager = nullptr;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
