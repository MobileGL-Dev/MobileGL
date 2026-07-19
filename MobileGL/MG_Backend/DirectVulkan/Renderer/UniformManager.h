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
        Bool CollectSampledTextures(const MG_State::GLState::ProgramObject& program,
                                    const ProgramFactory::VkProgramObject& programObj,
                                    Vector<MG_State::GLState::ITextureObject*>& outTextures);
        Bool CollectStorageImageTextures(const MG_State::GLState::ProgramObject& program,
                                         const ProgramFactory::VkProgramObject& programObj,
                                         Vector<MG_State::GLState::ITextureObject*>& outTextures) const;
        Bool BindProgramUniformBuffers(VkCommandBuffer commandBuffer,
                                       const MG_State::GLState::ProgramObject& program,
                                       const ProgramFactory::VkProgramObject& programObj,
                                       Uint32 frameIndex,
                                       VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       const SamplerBindingOverride* samplerBindingOverride = nullptr);

        // Pure format-policy helper kept public for host regression tests. Formatted storage
        // images use their shader qualifier; transformed float images use glBindImageTexture's
        // format and never silently fall back to the backing image format.
        static VkFormat ResolveStorageImageViewFormat(VkFormat reflectedFormat, GLenum bindingFormat,
                                                      VkFormat resourceFormat, Bool useBindingFormat);

    private:
        struct DescriptorPoolBucket {
            VkDescriptorPool handle = VK_NULL_HANDLE;
            Uint32 maxSets = 0;
            Uint32 allocatedSets = 0;
        };

        struct DescriptorSetCacheEntry {
            Vector<VkDescriptorSet> sets;
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
        // Raw-pointer variant for the per-draw sampled-texture walk (CollectSampledTextures):
        // the bound texture stays alive through the draw via GL binding state, so callers that
        // only need the pointer skip the SharedPtr copy's atomic refcount churn.
        static MG_State::GLState::ITextureObject* ResolveSamplerTextureRaw(
            const MG_State::GLState::ProgramObject& program,
            const ProgramFactory::VkProgramObject& programObj, Uint32 binding);
        SharedPtr<MG_State::GLState::ITextureObject> GetFallbackTexture(TextureTarget target) const;
        Bool ResolveSamplerDescriptor(VkCommandBuffer commandBuffer, const MG_State::GLState::ProgramObject& program,
                                      const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                      VkDescriptorImageInfo& outImageInfo) const;
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
                                         UboBindResult& out) const;
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

        // Descriptor-set reuse across consecutive draws (see BindProgramUniformBuffers).
        // When a draw's resolved descriptor content is byte-identical to the previous
        // draw's, reuse the same VkDescriptorSet and skip AcquireDescriptorSet +
        // vkUpdateDescriptorSets - only the bind-time dynamic offsets differ. Reset each
        // frame in BeginFrame because the frame's descriptor sets are recycled there.
        VkDescriptorSet m_lastBoundDescriptorSet = VK_NULL_HANDLE;
        Uint64 m_lastDescriptorSignature = 0;
        Bool m_hasLastDescriptor = false;

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
            Uint16 samplerVersion = 0;
            Uint16 textureParamsVersion = 0;
            Bool forceNearestFiltering = false;
            Bool valid = false;
        };
        mutable Vector<SamplerResolveMemo> m_samplerResolveMemo;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
