// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/UniformDescriptorBinder.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "ProgramFactory.h"
#include "RenderPassGuard.h"
#include "MagmaProgramSource.h"
#include "VkBufferManager.h"
#include "VkSamplerManager.h"
#include "VkTextureManager.h"
#include "WirePlaceholderKey.h"
#include "../DescriptorPoolCensus.h"
#include "../VkIncludes.h"
#include <Includes.h>

namespace MobileGL::MG_State::GLState {
    class ITextureObject;
    class ProgramObject;
    class SamplerObject;
}

namespace MobileGL::MG_Backend::DirectVulkan {
    class UniformManager {
    public:
        struct SamplerImageFeedbackBinding {
            Uint32 samplerBinding = 0;
            Uint32 samplerElement = 0;
            MG_State::GLState::ITextureObject* texture = nullptr;
            const MG_State::GLState::SamplerObject* sampler = nullptr;
            SamplerNumericDomain numericDomain = SamplerNumericDomain::Unknown;
        };

        // `physicalDevice` is only ever asked for format properties: a placeholder descriptor for
        // an unbound texel-buffer binding has to be built from a format the DEVICE accepts as a
        // texel buffer, and there is no other route to that answer from here.
        Bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice, VkBufferManager* bufferManager,
                        ProgramFactory* programFactory,
                        VkDeviceSize minUniformBufferOffsetAlignment, Uint32 frameCount,
                        Uint32 maxBindings = 16, Uint32 setsPerFrame = 64,
                        VkTextureManager* textureManager = nullptr, VkSamplerManager* samplerManager = nullptr);
        void Shutdown();

        void BeginFrame(Uint32 frameIndex);
        // A present-less wire frame may run arbitrarily many draws. Before its
        // descriptor cursor reaches this budget, the renderer retires the
        // submission that last used the sets and rewinds every layout cursor.
        static constexpr Uint32 kWireDescriptorSetBudget = 2048;
        Bool WireDescriptorSetBudgetReached(Uint32 frameIndex) const;
        SizeT RewindWireDescriptorSets(Uint32 frameIndex);
        // How the wire arm answers a STORAGE image unit that GL 4.6 core 8.26 makes invalid (no
        // texture, or a (level, layer) its texture lacks): a load returns zero, a store and an
        // atomic are discarded. `deviceNullDescriptor` is the renderer's enablement of
        // VK_EXT_robustness2's nullDescriptor at device creation. With it the unit binds a NULL
        // storage-image descriptor, which is exactly that rule. Without it (or under
        // MGITEST_MAGMA_FORCE_PRIVATE_IMAGE_PLACEHOLDER=1, the host lanes' way onto this arm) the
        // unit binds a placeholder PRIVATE to its unit (per shape), cleared before each use, so no
        // two units ever alias; what that cannot give is a load of the SAME unit after its own
        // store inside one pass reading zero, nor privacy past kWirePrivateStoragePlaceholderCap
        // placeholders (CONTRACT-P7 §12). Called after Initialize.
        void SetWireInvalidStorageImageArm(Bool deviceNullDescriptor);
        // A command buffer (re)began recording: descriptor bindings recorded into
        // the previous buffer do not carry over, so drop the bind-dedup shadow.
        void OnCommandBufferBoundary() { m_lastBindValid = false; }
        // The image-write epoch the last full memory barrier made visible (see m_wireImageBarrierEpoch),
        // and the note a caller that just recorded that same barrier makes.
        Uint64 WireImageBarrierEpoch() const { return m_wireImageBarrierEpoch; }
        void NoteWireImageBarrier() const { m_wireImageBarrierEpoch = WireImageWriteEpoch(); }
        // A ProgramFactory eviction just destroyed this layout: purge every frame
        // slot's cached descriptor sets for it, so a recycled handle value can never
        // stale-hit sets written for the dead layout's bindings. The sets are
        // vkFreeDescriptorSets'd back to their pools (created with
        // FREE_DESCRIPTOR_SET_BIT) and the pool accounting is credited, so program
        // churn recycles pool capacity instead of abandoning it. GPU-safe: the layout
        // only dies after >1024 idle frame boundaries, so no in-flight command buffer
        // references its sets. This is the only PER-LAYOUT eviction path for the
        // caches - a live layout's entry must never be purged on its own (its sets would be
        // unreachable pool slots), so there is deliberately no age-based sweep here. The
        // other path, TrimFrameDescriptorPools, drops a slot's whole cache together with
        // the pools behind it.
        void OnDescriptorSetLayoutDestroyed(VkDescriptorSetLayout descriptorSetLayout);
        // One record per visited CombinedImageSampler DESCRIPTOR (post fallback substitution,
        // in binding order, and within a binding in array-element order): the resolved texture
        // and effective sampler, as never-reused lifetime ids so a freed-and-reallocated object
        // at the same heap address can only MISS a comparison, never false-hit it (same ABA
        // rule as SamplerResolveMemo). An arrayed binding contributes one record per element -
        // element granularity is required, or swapping the textures of two elements of the same
        // array would leave the record list identical and the fast path would keep a stale set.
        struct SampledBindingRecord {
            Uint64 textureLifetimeId = 0;
            Uint64 samplerLifetimeId = 0;
        };
        static Bool SamplerOverlapsWritableImageSubresource(Int samplerBaseLevel, Int samplerMaxLevel,
                                                             GLint imageLevel, GLenum imageAccess);
        // Resolve lazy texture uploads/promotions before the caller captures the
        // command buffer passed to BindProgramUniformBuffers. Preparation may
        // submit older work, but descriptor recording must never rotate it.
        Bool PrepareWireTextureResources(const MagmaProgramSource& program,
                                          const ProgramFactory::VkProgramObject& programObj);
        Bool BindProgramUniformBuffers(VkCommandBuffer commandBuffer,
                                       const MagmaProgramSource& program,
                                       const ProgramFactory::VkProgramObject& programObj,
                                       Uint32 frameIndex,
                                       VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS);

        // Pure format-policy helper kept public for host regression tests. Formatted storage
        // images use their shader qualifier; transformed float images use glBindImageTexture's
        // format and never silently fall back to the backing image format.
        static VkFormat ResolveStorageImageViewFormat(VkFormat reflectedFormat, GLenum bindingFormat,
                                                      VkFormat resourceFormat, Bool useBindingFormat);

        // ---- per-frame descriptor pools ----------------------------------------------------
        // Each frame slot owns a list of pools. The first is the base pool (non-update-after-bind,
        // setsPerFrame sets) and is never destroyed while the manager lives; every later one was
        // added by GrowFrameDescriptorPool when an allocation found no room. A pool serves one
        // updateAfterBind flavour, because a set can only come from a pool created with the flag
        // its layout was.
        struct DescriptorPoolBucket {
            VkDescriptorPool handle = VK_NULL_HANDLE;
            Uint32 maxSets = 0;
            Uint32 allocatedSets = 0;
            Bool updateAfterBind = false;
        };
        // One pool's maxSets never exceeds this. It bounds one pool's driver memory: CreateDescriptorPool
        // declares 8 descriptors of each of 6 types per set, so a 4096-set pool asks for 196,608
        // descriptors - about 12 MiB at 64 bytes each on a driver that backs every declared one.
        static constexpr Uint32 kMaxDescriptorPoolSets = 4096;
        // The growth policy, pure so the host tests can drive it: the size of the next pool a slot
        // adds for `updateAfterBind` layouts, given the slot's existing pools. Twice the LARGEST pool
        // of that flavour, capped at kMaxDescriptorPoolSets; `initialMaxSets` when the slot has no
        // pool of that flavour yet. So a frame that needs N sets adds O(log N) pools while N stays
        // under the cap, and one more pool per kMaxDescriptorPoolSets sets beyond it. Taking the
        // largest (not the first) pool is the point: the first same-flavour pool is the oldest,
        // smallest one, and doubling IT made every grown pool the same size.
        static Uint32 NextDescriptorPoolMaxSets(const Vector<DescriptorPoolBucket>& pools, Bool updateAfterBind,
                                                Uint32 initialMaxSets);
        // Trim policy (see TrimFrameDescriptorPools). A slot is "quiet" in an epoch whose peak set
        // count times kDescriptorTrimHeadroom still fits its capacity; a slot that has been quiet for
        // MOBILEGL_MAGMA_DESCRIPTOR_TRIM_FRAMES consecutive epochs, and holds at least
        // kDescriptorTrimMinCapacity sets, is trimmed. The headroom is what keeps the trim from
        // thrashing: geometric growth leaves a re-grown slot at least a third full (the pool a growth
        // adds is at most twice the largest before it, so at most twice everything the slot held, and
        // the epoch that grew it needed more than that), so the epoch that grew a slot is never quiet
        // for it. The floor keeps a slot sized for ordinary
        // frames from being trimmed at all - the trim exists to give back a burst.
        static constexpr Uint32 kDescriptorTrimHeadroom = 4;
        static constexpr Uint32 kDescriptorTrimMinCapacity = 1024;
        // For tests (DescriptorPoolCensus.h): pools, capacity, cached sets and trims over every slot.
        DescriptorPoolCensus GetDescriptorPoolCensus() const;

    private:

        // A cached descriptor set together with the pool it was allocated from, so a
        // layout-destroyed purge can vkFreeDescriptorSets it back and credit the
        // owning bucket's accounting.
        struct CachedDescriptorSet {
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkDescriptorPool pool = VK_NULL_HANDLE;
        };

        struct DescriptorSetCacheEntry {
            Vector<CachedDescriptorSet> sets;
            Uint32 cursor = 0;
        };

        // ResolveWireImageDescriptor emits no pNext chain. Key every view-create
        // value explicitly: hash collisions must never alias different windows,
        // formats or swizzles. The root handle and allocation epoch also prevent
        // a recycled native image handle from reviving an older view.
        struct WireImageViewKey {
            MG_Pipe::MGPipeHandle root{};
            VkImage image = VK_NULL_HANDLE;
            Uint64 imageEpoch = 0;
            VkImageViewCreateFlags flags = 0;
            VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkComponentMapping components{};
            VkImageSubresourceRange range{};

            Bool operator==(const WireImageViewKey& other) const {
                return root == other.root && image == other.image && imageEpoch == other.imageEpoch &&
                       flags == other.flags && type == other.type && format == other.format &&
                       components.r == other.components.r && components.g == other.components.g &&
                       components.b == other.components.b && components.a == other.components.a &&
                       range.aspectMask == other.range.aspectMask && range.baseMipLevel == other.range.baseMipLevel &&
                       range.levelCount == other.range.levelCount && range.baseArrayLayer == other.range.baseArrayLayer &&
                       range.layerCount == other.range.layerCount;
            }
        };

        struct WireImageViewKeyHash {
            SizeT operator()(const WireImageViewKey& key) const;
        };

        struct WirePlaceholderImage {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkSampler sampler = VK_NULL_HANDLE;
            VkRenderPass clearPass = VK_NULL_HANDLE;
            VkFramebuffer clearFramebuffer = VK_NULL_HANDLE;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
            Uint32 layers = 1;
        };
        // Keyed by shape, and a storage placeholder on the arm without a null descriptor by its
        // image unit too (WirePlaceholderKey.h: ChooseWirePlaceholderKey, and its cap).
        mutable UnorderedMap<WirePlaceholderKey, WirePlaceholderImage, WirePlaceholderKeyHash> m_wirePlaceholderImages;
        // How many of those are private to a unit (against kWirePrivateStoragePlaceholderCap).
        mutable SizeT m_wirePrivateStoragePlaceholders = 0;
        // See SetWireInvalidStorageImageArm.
        Bool m_wireInvalidStorageImagesBindNull = false;
        // boundStorageFormat: for a storage binding with no reflected format, the format the
        // unit's glBindImageTexture named, when the unit holds a texture it cannot address
        // (GL 4.6 core 8.26); UNDEFINED keeps the numeric-domain R32 default. `unit` is the image
        // unit of a storage binding (what its private placeholder is private to); unused for a sampler.
        Bool ResolveWirePlaceholderImage(VkCommandBuffer commandBuffer, const MagmaProgramSource& program,
            const ProgramFactory::VkProgramObject& programObj, Uint32 binding, Bool storage,
            VkDescriptorImageInfo& out, VkFormat boundStorageFormat = VK_FORMAT_UNDEFINED, Uint32 unit = ~0u) const;
        void DestroyWirePlaceholderImage(WirePlaceholderImage& image) const;

        // descriptorSetCacheByLayout is the per-frame set cache: BeginFrame rewinds every cursor, so
        // a slot re-writes the sets it allocated in earlier frames instead of allocating new ones, and
        // the cache only ever grows - except through TrimFrameDescriptorPools, which gives a slot's
        // whole burst back at once (and OnDescriptorSetLayoutDestroyed, one dead layout at a time).
        struct FrameResources {
            Vector<DescriptorPoolBucket> descriptorPools;
            UnorderedMap<VkDescriptorSetLayout, DescriptorSetCacheEntry> descriptorSetCacheByLayout;
            Vector<VkBufferView> texelBufferViews;
            mutable Vector<VkImageView> wireImageViews;
            // Own views in the vector above until this slot's fence/idle proof.
            // Cache hits preserve the handle, enabling descriptor-content reuse.
            mutable UnorderedMap<WireImageViewKey, VkImageView, WireImageViewKeyHash> wireImageViewCache;
            Uint32 activeDescriptorPoolIndex = 0;
            Uint32 allocatedSetsThisFrame = 0;
            Uint32 peakAllocatedSetsThisFrame = 0;
            // Sum of maxSets over descriptorPools, kept in step by Initialize, Grow and Trim.
            Uint64 capacitySets = 0;
            // Consecutive epochs (see NoteEndedEpochForTrim) that were quiet for THIS slot's capacity.
            Uint32 quietEpochs = 0;
        };

        // ---- placeholders for UNBOUND image-backed descriptors -------------------------
        // GL lets a program declare `samplerBuffer`, `imageBuffer` or `image2D` and bind nothing
        // to the unit it names: the fetch is then undefined (GL 4.6 core 8.9 for an incomplete
        // buffer texture, 8.26 for an image unit with no texture) - undefined VALUES, not a
        // dropped draw. Vulkan has no unwritten descriptor, so something valid has to sit in the
        // set or the whole draw or dispatch is lost, which is what these two build. Same shape as
        // VkBufferManager::AcquireUnboundStorageDescriptor, one level up: per FORMAT rather than
        // one shared object, because a descriptor whose format disagrees with the shader's
        // declaration is invalid Vulkan even when nothing ever reads it.
        //
        // `declaredFormat` is the format the SHADER declared (VK_FORMAT_UNDEFINED for a sampled
        // texel buffer, which never carries one, or for a formatless `writeonly` image);
        // `numericDomain` decides the format when there is no declaration and is the fallback
        // class when the device cannot use the declared one as a texel buffer.
        VkBufferView AcquireUnboundTexelBufferView(VkFormat declaredFormat, SamplerNumericDomain numericDomain,
                                                   Bool storage);
        Bool ResolveWireImageDescriptor(VkCommandBuffer commandBuffer, const MagmaProgramSource& program,
                                       const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                       Uint32 element, Bool storage, VkDescriptorImageInfo& out,
                                       VkTextureManager::TextureResource** outResource = nullptr) const;
        // P14: THE WIRE DESCRIPTOR REBIND. A draw whose image descriptors cannot have changed since
        // the last full resolve of the same program in the same frame - same applier context, same
        // unit / sampler / image / buffer-binding serials, same texture shutter, no texture image
        // re-created or erased, no sampler destroyed, every sampled image still in GENERAL and no
        // image write awaiting its barrier - re-resolves only its buffer descriptors (the UBO
        // dynamic offsets move every draw) and, when those name the same buffers and ranges,
        // rebinds the set it already has. Anything else, or any program with a texel buffer, storage
        // image or placeholder descriptor, takes the full resolve.
        struct WireDescriptorRebind {
            Bool valid = false;
            Uint32 frameIndex = 0;
            VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            Uint64 programHash = 0;
            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            MG_Pipe::MGPipeHandle programHandle = MG_Pipe::kMGPipeNullHandle;
            Uint64 programSerial = 0, programBindingsSerial = 0;
            Uint64 contextSerial = 0, samplerViewsSerial = 0, samplerStatesSerial = 0, shaderImagesSerial = 0;
            Uint64 shaderBuffersSerial = 0, textureShutterSerial = 0;
            Uint64 storeDestroyEpoch = 0, textureImageEpoch = 0, resourceEraseEpoch = 0;
            Uint64 samplerDestroyEpoch = 0, samplerFrameBoundary = 0;
            VkDescriptorSet set = VK_NULL_HANDLE;
            Vector<VkTextureManager::TextureResource*> sampledResources;
            // The buffer descriptors in walk order: dynamic UBOs with offset 0 (their offset is the
            // dynamic one), storage buffers whole.
            Vector<VkDescriptorBufferInfo> bufferInfos;
        };
        WireDescriptorRebind m_wireDescriptorRebind;
        Vector<VkDescriptorBufferInfo> m_wireRebindBufferScratch;
        Vector<VkTextureManager::TextureResource*> m_wireRebindResourcesScratch;
        Bool TryRebindWireDescriptorSet(VkCommandBuffer commandBuffer, const MagmaProgramSource& program,
                                        const ProgramFactory::VkProgramObject& programObj, Uint32 frameIndex,
                                        VkPipelineBindPoint bindPoint);
        Bool ResolveWireTexelBufferDescriptor(const MagmaProgramSource& program,
                                             const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                             Uint32 frameIndex, Bool storage, VkBufferView& out);
        Uint32 m_wireFrameIndex = 0;
        VkDeviceSize m_wireStorageOffsetAlignment = 1;
        VkDeviceSize m_wireTexelOffsetAlignment = 1;
        VkDeviceSize m_wireMaxUniformRange = ~VkDeviceSize{0};
        VkDeviceSize m_wireMaxStorageRange = ~VkDeviceSize{0};
        Uint32 m_wireMaxTexelElements = ~Uint32{0};
        Bool ResolveTexelBufferDescriptor(const MagmaProgramSource& program,
                                          const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                          Uint32 frameIndex, VkBufferView& outBufferView);
        // GLSL `imageBuffer`: the same VkBufferView descriptor as the sampled texel buffer above,
        // but resolved from an IMAGE unit (glBindImageTexture) rather than a texture unit, and
        // made GPU-resident-writable because the shader may store to it. No `element` parameter:
        // an imageBuffer ARRAY is refused at program creation, so a binding is always one
        // descriptor (see the array gate in RemapDescriptorBindingsForVulkan).
        Bool ResolveStorageTexelBufferDescriptor(const MagmaProgramSource& program,
                                                 const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                                 Uint32 frameIndex, VkBufferView& outBufferView);
        // `element` indexes a block INSTANCE array's descriptors; it is 0 for every ordinary
        // block. Each element resolves through its own GL storage block, and so its own GL
        // binding point, buffer and glBindBufferRange window.
        Bool ResolveStorageBufferDescriptor(const MagmaProgramSource& program,
                                            const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                            Uint32 element, VkDescriptorBufferInfo& outBufferInfo) const;
        // `element` indexes an image ARRAY inside one binding; each element carries its own
        // independently assigned GL image unit.
        Bool ResolveStorageImageDescriptor(VkCommandBuffer commandBuffer,
                                           const MagmaProgramSource& program,
                                           const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                           Uint32 element, VkDescriptorImageInfo& outImageInfo) const;
        // Result of resolving a UBO binding: either a zero-copy direct bind to the app's resident
        // VkBuffer (the GLES backend's approach - no per-draw copy) or the CPU payload to upload.
        struct UboBindResult {
            Bool directBindable = false;
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize range = 0;         // reflected block size; constant across draws (hashed)
            VkDeviceSize dynamicOffset = 0; // block range start; moves per draw (NOT hashed)
            const void* payload = nullptr;  // fallback UploadTransient path
            VkDeviceSize payloadSize = 0;
        };
        Bool ResolveWireUniformBufferPayload(const MagmaProgramSource& program, Uint32 blockIndex,
                                             Uint32 bindingPoint, UboBindResult& out) const;
        Bool ResolveUniformBufferPayload(const MagmaProgramSource& program,
                                         const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                         Uint32 arrayElement, UboBindResult& out) const;
        // Shared resolution of one dynamic-UBO binding element into the
        // (buffer, range, dynamicOffset) triple the descriptor consumes: direct
        // bind, global-slice reuse, or transient upload. Used by the full walk
        // and by the dynamic-offset-only rebind (see FastRebindMemo).
        Bool ResolveDynamicUboDescriptor(const MagmaProgramSource& program,
                                         const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                         Uint32 arrayElement, Uint32 frameIndex, VkBuffer& outBuffer,
                                         VkDeviceSize& outRange, Uint32& outDynamicOffset);
        // The vkCmdBindDescriptorSets tail shared by the full walk and the
        // dynamic-offset-only rebind: skips the driver call when this exact
        // binding is already live on the command buffer (see the bind-dedup
        // shadow below), otherwise binds and refreshes the shadow.
        void BindDescriptorSetDeduped(VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint,
                                      VkPipelineLayout pipelineLayout, VkDescriptorSet descriptorSet,
                                      const Vector<Uint32>& dynamicOffsets);
        Bool CreateDescriptorPool(Uint32 maxSets, Bool updateAfterBind, VkDescriptorPool& outPool) const;
        Bool GrowFrameDescriptorPool(FrameResources& frame, Uint32 frameIndex, Bool updateAfterBind);
        // An epoch is the stretch between two BeginFrame calls: one frame, or the part of one between
        // two drains (VulkanRenderer::TryDrainFrameTransients). Called by BeginFrame with the epoch
        // that just ended, it updates every slot's quiet count - a burst in any slot counts against
        // all of them, so bursts that land in the other slots still hold a slot's trim off.
        void NoteEndedEpochForTrim(Uint32 epochPeakSets);
        // Gives a slot's whole descriptor burst back: clears its per-layout cache, destroys every
        // grown pool and resets the base pool, so the slot re-grows geometrically from setsPerFrame.
        // Whole-slot rather than per-set: vkFreeDescriptorSets fragments a pool and can never empty
        // a grown one. Called only from BeginFrame, which runs only when no pending command buffer
        // references the slot's sets:
        //   - Present path (VulkanRenderer::Present, after WaitAndAcquireNextImage): the slot's
        //     imageInFlightFence has been waited. That fence rides the slot's LAST submission - the
        //     Present submit - and every mid-frame submission of the slot (FlushPendingCommands,
        //     SplitOversizedRecording, readbacks) went to the same queue earlier, so the fence's
        //     signal covers them (vkQueueSubmit's fence scope includes all earlier submissions).
        //   - Drain path (VulkanRenderer::TryDrainFrameTransients): every submission has been
        //     observed complete and no recording is open.
        // Sets are per slot: a slot's sets are only ever bound into recordings made while it is the
        // current slot, so trimming one slot never touches another slot's in-flight work.
        void TrimFrameDescriptorPools(FrameResources& frame, Uint32 frameIndex);
        // Drops every handle this class holds to a descriptor set outside the per-layout caches.
        void InvalidateDescriptorSetMemos();
        VkResult AllocateDescriptorSetsFromActivePool(
            Uint32 frameIndex, const ProgramFactory::VkProgramObject& programObj, VkDescriptorSet& outDescriptorSet);
        VkResult AcquireDescriptorSet(Uint32 frameIndex,
                                      const ProgramFactory::VkProgramObject& programObj,
                                      VkDescriptorSet& outDescriptorSet);

        VkDevice m_device = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkBufferManager* m_bufferManager = nullptr;
        ProgramFactory* m_programFactory = nullptr;
        Vector<FrameResources> m_frames;

        VkDeviceSize m_minDynamicOffsetAlignment = 1;
        Uint32 m_frameCount = 0;
        Uint32 m_maxBindings = 0;
        Uint32 m_setsPerFrame = 0;
        Uint32 m_peakDescriptorSetsObserved = 0;
        // MOBILEGL_MAGMA_DESCRIPTOR_TRIM_FRAMES, latched at Initialize (0 = never trim).
        Uint32 m_trimQuietEpochs = 0;
        // The slot whose epoch is open (the last one BeginFrame was called for); valid once
        // m_epochOpen is set. Its peak is the ended epoch's peak at the next BeginFrame.
        Uint32 m_epochSlot = 0;
        Bool m_epochOpen = false;
        Uint64 m_descriptorSlotTrims = 0;
        VkTextureManager* m_textureManager = nullptr;
        VkSamplerManager* m_samplerManager = nullptr;
        // See AcquireUnboundTexelBufferView. Lazily populated, never evicted (a program's declared
        // formats are a fixed, tiny set) and torn down with the manager. The texel views are keyed
        // by format AND by storage-vs-sampled
        // because the two descriptor kinds demand different format FEATURES of the device, so one
        // format can be usable for one and not the other. Deliberately NOT the per-frame
        // texelBufferViews list: those are destroyed at every frame boundary, and these must
        // outlive it or the placeholder would be rebuilt for every unbound binding every frame.
        UnorderedMap<Uint64, VkBufferView> m_unboundTexelBufferViews;

        // Per-draw scratch buffers for BindProgramUniformBuffers: reused (clear keeps
        // capacity) so the descriptor-write path stops allocating on every draw.
        Vector<VkWriteDescriptorSet> m_writesScratch;
        Vector<VkDescriptorBufferInfo> m_bufferInfosScratch;
        Vector<VkDescriptorImageInfo> m_imageInfosScratch;
        Vector<VkBufferView> m_texelBufferViewsScratch;
        Vector<Uint32> m_dynamicOffsetsScratch;

        // Descriptor-set reuse across recent draws (see BindProgramUniformBuffers).
        // When a draw's resolved descriptor content is byte-identical to one memoized
        // earlier, reuse that VkDescriptorSet and skip AcquireDescriptorSet +
        // vkUpdateDescriptorSets - only the bind-time dynamic offsets differ. Four
        // entries with round-robin replacement rather than one: draws alternating
        // between two programs (MC's chunk<->entity ping-pong) would thrash a single
        // slot into a full re-allocate+write every draw. Reset each frame in BeginFrame
        // because the frame's descriptor sets are recycled there.
        struct DescriptorReuseEntry {
            Uint64 signature = 0;
            VkDescriptorSet set = VK_NULL_HANDLE;
            Bool valid = false;
        };
        static constexpr Uint32 kDescriptorReuseMemoSize = 4;
        DescriptorReuseEntry m_descriptorReuseMemo[kDescriptorReuseMemoSize];
        Uint32 m_descriptorReuseMemoNext = 0;

        // vkCmdBindDescriptorSets dedup: consecutive draws with a static uniform
        // block resolve to the same set AND the same dynamic offsets, so the
        // driver call can be skipped outright. Command-buffer-scope state; reset
        // via OnCommandBufferBoundary whenever a recording (re)begins. Keyed on
        // layout+bind point, so a pipeline-layout switch always rebinds.
        static constexpr Uint32 kMaxShadowedDynamicOffsets = 8;
        Bool m_lastBindValid = false;
        VkDescriptorSet m_lastBindSet = VK_NULL_HANDLE;
        VkPipelineLayout m_lastBindLayout = VK_NULL_HANDLE;
        VkPipelineBindPoint m_lastBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        Uint32 m_lastBindOffsetCount = 0;
        Uint32 m_lastBindOffsets[kMaxShadowedDynamicOffsets] = {};

        // Global-UBO transient-slice reuse: MC leaves the default uniform block
        // untouched across long GUI/terrain runs, so the per-draw re-upload of
        // the same bytes can reuse the slice uploaded earlier THIS frame (frame
        // serial guards arena recycling; the content version guards writes).
        struct GlobalUboSliceMemo {
            Uint64 programLifetimeId = 0;
            Uint64 frameSerial = 0;
            Uint32 uboContentVersion = 0;
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            VkDeviceSize range = 0;
        };
        static constexpr Uint32 kGlobalUboMemoSize = 4;
        GlobalUboSliceMemo m_globalUboMemo[kGlobalUboMemoSize];
        Uint32 m_globalUboMemoNext = 0;

        // Per-binding fast path over VkSamplerManager's content-hashed sampler cache, which
        // stays the source of truth: its key hashes all sampler+texture state, so two distinct
        // sampler objects with identical state still resolve to one VkSampler. This memo only
        // skips recomputing that hash. Across a draw batch the bound sampler set is stable, so a
        // binding whose sampler (lifetime id + version, bumped on every setter) and texture
        // (lifetime id + params version, bumped on the format/border-color setters that feed the
        // key) are unchanged recycles the VkSampler it resolved last draw; a param change bumps
        // a version and forces a re-resolve. Both objects are keyed by a never-reused monotonic
        // lifetime id, so a freed-and-reallocated sampler or texture at the same heap address
        // always gets a fresh id and misses (a raw pointer would false-hit that ABA) - so a
        // stale guess can only miss and fall through to the hash, never resolve wrong. Still
        // reset each frame alongside the descriptor-set cache. Indexed by binding, but the
        // whole-descriptor entry is additionally keyed by program lifetime: Vulkan binding
        // numbers are layout-local and unrelated programs routinely reuse binding 0/1.
        struct SamplerResolveMemo {
            Uint64 infoProgramLifetimeId = 0;
            Uint64 samplerLifetimeId = 0;
            Uint64 textureLifetimeId = 0;
            VkSampler sampler = VK_NULL_HANDLE;
            Uint32 viewLevelCount = 0;
            Uint16 samplerVersion = 0;
            Uint16 textureParamsVersion = 0;
            Bool forceNearestFiltering = false;
            Bool valid = false;
            // ResolveSampledImageViewFormat is pure in (image format, numeric domain), but a
            // domain mismatch walks a ~184-entry format table. Memo the resolution per binding
            // so a reinterpreted sampler pays that scan once, not once per draw.
            VkFormat viewFormatSource = VK_FORMAT_UNDEFINED;
            SamplerNumericDomain viewFormatDomain = SamplerNumericDomain::Unknown;
            VkFormat viewFormat = VK_FORMAT_UNDEFINED;
            Bool viewFormatValid = false;
            // Whole resolved descriptor from this binding's last full resolve. Reused
            // ONLY under ResolveSamplerDescriptor's trustUnchangedHint, whose caller
            // proves every resolve input unchanged; cleared with the per-frame reset
            // (the cached VkSampler outlives a frame only via a fresh resolve, which
            // also re-stamps it against VkSamplerManager's frame-boundary sweep).
            //
            // This one field is keyed by binding but describes ONE descriptor, so it is
            // written and read only for single-descriptor bindings. A sampler ARRAY's
            // elements share the binding and would overwrite each other here - the last
            // element resolved would then be handed to element 0 on the next hinted draw.
            // Every other field above is self-validating (each compares its full key
            // before reuse, and the view-format entry is a pure function of format and
            // numeric domain), so an arrayed binding may keep using those.
            VkDescriptorImageInfo info{};
            Bool infoValid = false;
        };
        mutable Vector<SamplerResolveMemo> m_samplerResolveMemo;
        // Exclusive upper bound on the entries of m_samplerResolveMemo that any resolve
        // has ever written. The vector is sized to the DEVICE binding cap (256 on desktop
        // NVIDIA), but a program declares 1-8 bindings, so the per-frame reset below was
        // memsetting ~22 KB of never-touched entries every frame - a measurable slice of
        // the per-frame fixed cost on draw-light frames. Every site that can turn any of
        // an entry's *Valid flags on raises this mark first, so entries at or above it are
        // provably still in their constructed (all-invalid) state and clearing them is a
        // no-op. Never lowered except by Initialize/Shutdown, which rebuild the vector.
        mutable Uint32 m_samplerResolveMemoHighWater = 0;
        void NoteSamplerResolveMemoTouched(Uint32 binding) const {
            if (binding >= m_samplerResolveMemoHighWater) {
                m_samplerResolveMemoHighWater = binding + 1;
            }
        }
        // WireImageWriteEpoch() when ResolveWireImageDescriptor, or a new draw pass's attachment
        // barrier (VulkanRenderer::TransitionWireImage), last made image writes visible.
        mutable Uint64 m_wireImageBarrierEpoch = 0;
        // P14: the wire arm's sampler memo, one entry per texture unit. The sampler a unit resolves
        // to is a function of the sampler CSO it names (immutable for a {slot, gen, Serial} within
        // one applier - ContextSerial tells appliers apart), the texture's format, the numeric
        // domain and the view's level count. A hit skips the parameter hash and the cache map; it
        // is taken only while the sampler manager has destroyed nothing (DestroyEpoch) and only
        // within the frame boundary of the last real lookup, so every used entry is still stamped
        // once per frame and the idle sweep can never retire a sampler this memo hands out.
        struct WireSamplerMemo {
            Bool valid = false;
            Uint32 slot = 0, gen = 0;
            Uint64 serial = 0, contextSerial = 0;
            Uint32 format = 0, viewLevelCount = 0;
            Bool forceNearest = false;
            Uint64 frameBoundary = 0, destroyEpoch = 0;
            VkSampler sampler = VK_NULL_HANDLE;
        };
        static constexpr SizeT kWireSamplerMemoUnits = 192; // MG_Pipe::kMGPipeMaxTextureUnits
        mutable Array<WireSamplerMemo, kWireSamplerMemoUnits> m_wireSamplerMemo{};
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
