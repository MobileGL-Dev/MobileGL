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
        Bool BindProgramUniformBuffers(VkCommandBuffer commandBuffer,
                                       const MG_State::GLState::ProgramObject& program,
                                       const ProgramFactory::VkProgramObject& programObj,
                                       Uint32 frameIndex,
                                       VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       const SamplerBindingOverride* samplerBindingOverride = nullptr);

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
        Bool ResolveUniformBufferPayload(const MG_State::GLState::ProgramObject& program,
                                         const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                         const void*& outData, VkDeviceSize& outSize) const;
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
    };
} // namespace MobileGL::MG_Backend::DirectVulkan

