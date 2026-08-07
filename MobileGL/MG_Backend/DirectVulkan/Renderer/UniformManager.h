// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/UniformDescriptorBinder.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "ProgramFactory.h"
#include "VkBufferManager.h"
#include "VkSamplerManager.h"
#include "VkTextureManager.h"
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
        struct SamplerBindingOverride {
            Uint32 binding = 0;
            MG_State::GLState::ITextureObject* texture = nullptr;
            const MG_State::GLState::SamplerObject* sampler = nullptr;
            VkImageView imageView = VK_NULL_HANDLE;
        };

        Bool Initialize(VkDevice device, VkBufferManager* bufferManager,
                        ProgramFactory* programFactory,
                        VkDeviceSize minUniformBufferOffsetAlignment, Uint32 frameCount,
                        Uint32 maxBindings = 16, Uint32 setsPerFrame = 64,
                        VkTextureManager* textureManager = nullptr, VkSamplerManager* samplerManager = nullptr);
        void Shutdown();

        void BeginFrame(Uint32 frameIndex);
        // A command buffer (re)began recording: descriptor bindings recorded into
        // the previous buffer do not carry over, so drop the bind-dedup shadow.
        void OnCommandBufferBoundary() { m_lastBindValid = false; }
        // A ProgramFactory eviction just destroyed this layout: purge every frame
        // slot's cached descriptor sets for it, so a recycled handle value can never
        // stale-hit sets written for the dead layout's bindings. The sets are
        // vkFreeDescriptorSets'd back to their pools (created with
        // FREE_DESCRIPTOR_SET_BIT) and the pool accounting is credited, so program
        // churn recycles pool capacity instead of abandoning it. GPU-safe: the layout
        // only dies after >1024 idle frame boundaries, so no in-flight command buffer
        // references its sets. This is the only eviction path for the per-layout
        // caches - a live layout's entry must never be purged (its sets would be
        // unreachable pool slots), so there is deliberately no age-based sweep here.
        void OnDescriptorSetLayoutDestroyed(VkDescriptorSetLayout descriptorSetLayout);
        // One record per visited CombinedImageSampler binding (post fallback substitution,
        // in binding order): the resolved texture and effective sampler, as never-reused
        // lifetime ids so a freed-and-reallocated object at the same heap address can only
        // MISS a comparison, never false-hit it (same ABA rule as SamplerResolveMemo).
        struct SampledBindingRecord {
            Uint64 textureLifetimeId = 0;
            Uint64 samplerLifetimeId = 0;
        };
        Bool CollectSampledTextures(const MG_State::GLState::ProgramObject& program,
                                    const ProgramFactory::VkProgramObject& programObj,
                                    Vector<MG_State::GLState::ITextureObject*>& outTextures,
                                    Vector<SampledBindingRecord>* outBindingRecords = nullptr);
        // Shadow-compare for the SetupDraw fast path: re-runs the CollectSampledTextures
        // walk and reports whether every visited binding still resolves to the recorded
        // (texture, effective sampler) pair. A texture bind generation bump alone (e.g. a
        // redundant glBindSampler, which always bumps it) does not prove the sampled set
        // moved; this walk does, without rebuilding the set or falling off the fast path.
        Bool SampledBindingsUnchanged(const MG_State::GLState::ProgramObject& program,
                                      const ProgramFactory::VkProgramObject& programObj,
                                      const Vector<SampledBindingRecord>& previousRecords) const;
        Bool CollectStorageImageTextures(const MG_State::GLState::ProgramObject& program,
                                         const ProgramFactory::VkProgramObject& programObj,
                                         Vector<MG_State::GLState::ITextureObject*>& outTextures) const;
        // samplerDescriptorsUnchangedHint: the caller (SetupDraw fast path) proved that
        // every input of every combined-image-sampler resolution is unchanged since the
        // previous draw's resolve - same (texture, sampler) per binding, texture params
        // sum, sampling-resolution generation (sampler params + texture shape), image
        // epochs AND per-resource layout values - so the per-binding cached
        // VkDescriptorImageInfo may be reused without re-running the resolve chain.
        Bool BindProgramUniformBuffers(VkCommandBuffer commandBuffer,
                                       const MG_State::GLState::ProgramObject& program,
                                       const ProgramFactory::VkProgramObject& programObj,
                                       Uint32 frameIndex,
                                       VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       const SamplerBindingOverride* samplerBindingOverride = nullptr,
                                       Bool samplerDescriptorsUnchangedHint = false);

        // Pure format-policy helper kept public for host regression tests. Formatted storage
        // images use their shader qualifier; transformed float images use glBindImageTexture's
        // format and never silently fall back to the backing image format.
        static VkFormat ResolveStorageImageViewFormat(VkFormat reflectedFormat, GLenum bindingFormat,
                                                      VkFormat resourceFormat, Bool useBindingFormat);

        // True when the program reads at least one sampler and every one of them is bound to a
        // texture whose GL level range is a single level. Such a sampler resolves to
        // minLod = maxLod = 0 (see VkSamplerManager::GetOrCreateSampler), so an implicit-LOD sample
        // and an explicit LOD 0 sample must read the same texel - which is what makes the
        // ExplicitLod0Sampling SPIR-V rewrite safe to request. Deliberately conservative: it reads
        // only GL state, so a texture that ends up single-level for another reason (one uploaded
        // level under a wide level range) merely misses the rewrite.
        static Bool ProgramSamplesOnlySingleLevelTextures(const MG_State::GLState::ProgramObject& program,
                                                          const ProgramFactory::VkProgramObject& programObj);

    private:
        struct DescriptorPoolBucket {
            VkDescriptorPool handle = VK_NULL_HANDLE;
            Uint32 maxSets = 0;
            Uint32 allocatedSets = 0;
        };

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

        struct FrameResources {
            Vector<DescriptorPoolBucket> descriptorPools;
            UnorderedMap<VkDescriptorSetLayout, DescriptorSetCacheEntry> descriptorSetCacheByLayout;
            Vector<VkBufferView> texelBufferViews;
            Uint32 activeDescriptorPoolIndex = 0;
            Uint32 allocatedSetsThisFrame = 0;
            Uint32 peakAllocatedSetsThisFrame = 0;
        };

        static Bool ResolveSamplerTexture(const MG_State::GLState::ProgramObject& program,
                                   const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                   SharedPtr<MG_State::GLState::ITextureObject>& outTexture);
        // Shared per-binding resolution for CollectSampledTextures and
        // SampledBindingsUnchanged, so membership and comparison can never diverge:
        // texture after the fallback substitution (may still be null when no fallback
        // exists), effective sampler = unit override else the texture's own sampler.
        // False = the binding is skipped (unbound with a non-2D fallback target).
        Bool ResolveSampledBinding(const MG_State::GLState::ProgramObject& program,
                                   const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                   MG_State::GLState::ITextureObject*& outTexture,
                                   const MG_State::GLState::SamplerObject*& outSampler) const;
        // Raw-pointer variant for the per-draw sampled-texture walk (CollectSampledTextures):
        // the bound texture stays alive through the draw via GL binding state, so callers that
        // only need the pointer skip the SharedPtr copy's atomic refcount churn.
        static MG_State::GLState::ITextureObject* ResolveSamplerTextureRaw(
            const MG_State::GLState::ProgramObject& program,
            const ProgramFactory::VkProgramObject& programObj, Uint32 binding);
        SharedPtr<MG_State::GLState::ITextureObject> GetFallbackTexture(TextureTarget target) const;
        // trustUnchangedHint: reuse this binding's cached VkDescriptorImageInfo outright
        // (see BindProgramUniformBuffers' samplerDescriptorsUnchangedHint for the proof
        // obligations the caller carries).
        Bool ResolveSamplerDescriptor(VkCommandBuffer commandBuffer, const MG_State::GLState::ProgramObject& program,
                                      const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                      VkDescriptorImageInfo& outImageInfo,
                                      Bool trustUnchangedHint = false) const;
        Bool ResolveSamplerDescriptorOverride(const SamplerBindingOverride& samplerBindingOverride,
                                              VkDescriptorImageInfo& outImageInfo) const;
        Bool ResolveTexelBufferDescriptor(const MG_State::GLState::ProgramObject& program,
                                          const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                          Uint32 frameIndex, VkBufferView& outBufferView);
        Bool ResolveStorageBufferDescriptor(const MG_State::GLState::ProgramObject& program,
                                            const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                            VkDescriptorBufferInfo& outBufferInfo) const;
        Bool ResolveStorageImageDescriptor(VkCommandBuffer commandBuffer,
                                           const MG_State::GLState::ProgramObject& program,
                                           const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                           VkDescriptorImageInfo& outImageInfo) const;
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
        Bool ResolveUniformBufferPayload(const MG_State::GLState::ProgramObject& program,
                                         const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                         Uint32 arrayElement, UboBindResult& out) const;
        // Shared resolution of one dynamic-UBO binding element into the
        // (buffer, range, dynamicOffset) triple the descriptor consumes: direct
        // bind, global-slice reuse, or transient upload. Used by the full walk
        // and by the dynamic-offset-only rebind (see FastRebindMemo).
        Bool ResolveDynamicUboDescriptor(const MG_State::GLState::ProgramObject& program,
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
        Bool CreateDescriptorPool(Uint32 maxSets, VkDescriptorPool& outPool) const;
        Bool GrowFrameDescriptorPool(FrameResources& frame, Uint32 frameIndex);
        VkResult AllocateDescriptorSetsFromActivePool(
            Uint32 frameIndex, const ProgramFactory::VkProgramObject& programObj, VkDescriptorSet& outDescriptorSet);
        VkResult AcquireDescriptorSet(Uint32 frameIndex,
                                      const ProgramFactory::VkProgramObject& programObj,
                                      VkDescriptorSet& outDescriptorSet);

        VkDevice m_device = VK_NULL_HANDLE;
        VkBufferManager* m_bufferManager = nullptr;
        ProgramFactory* m_programFactory = nullptr;
        Vector<FrameResources> m_frames;

        VkDeviceSize m_minDynamicOffsetAlignment = 1;
        Uint32 m_frameCount = 0;
        Uint32 m_maxBindings = 0;
        Uint32 m_setsPerFrame = 0;
        Uint32 m_peakDescriptorSetsObserved = 0;
        VkTextureManager* m_textureManager = nullptr;
        VkSamplerManager* m_samplerManager = nullptr;
        mutable SharedPtr<MG_State::GLState::ITextureObject> m_fallbackTexture2D;

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

        // Dynamic-offset-only rebind (see BindProgramUniformBuffers): records the
        // descriptor set selected by the last cacheable full walk of a program
        // whose active bindings are exactly one dynamic UBO (single descriptor)
        // plus combined-image samplers. When the next call proves every sampler
        // descriptor input unchanged (samplerDescriptorsUnchangedHint) and the
        // UBO re-resolves to the SAME VkBuffer+range - only the dynamic offset
        // moved, the per-draw glUniform case - the walk collapses to: resolve one
        // offset, rebind the recorded set with new pDynamicOffsets (Vulkan allows
        // rebinding the same set with different dynamic offsets).
        // Invalidation inventory: BeginFrame clears it (the frame's sets are
        // recycled) and the frameIndex field guards cross-frame confusion on top;
        // OnDescriptorSetLayoutDestroyed clears it (the set may be freed); a
        // sampler-override walk clears it (mirrors m_descriptorReuseMemo); a
        // program relink bumps the backend state version and thus programObj.hash
        // so the key misses; the program lifetime id is never reused, so a
        // deleted-and-recreated program misses; a texture/sampler/binding change
        // drops the hint upstream; an arena wrap or growth resolves a different
        // VkBuffer and misses. AcquireDescriptorSet's per-frame cursor only
        // advances, so the recorded set is never re-written within its frame.
        struct FastRebindMemo {
            Bool valid = false;
            Uint32 frameIndex = 0;
            Uint64 programLifetimeId = 0;
            ProgramFactory::HashType programHash = 0;
            Uint32 uboBinding = 0;
            VkBuffer uboBuffer = VK_NULL_HANDLE;
            VkDeviceSize uboRange = 0;
            VkDescriptorSet set = VK_NULL_HANDLE;
        };
        FastRebindMemo m_fastRebindMemo;

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
        // reset each frame alongside the descriptor-set cache. Indexed by binding.
        struct SamplerResolveMemo {
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
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
