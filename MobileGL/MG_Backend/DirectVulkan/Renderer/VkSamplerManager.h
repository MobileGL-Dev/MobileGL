// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkSamplerManager.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "../VkIncludes.h"
#include "../VulkanRendererConfig.h"
#include <Includes.h>
#include <MG_State/GLState/SamplerState/SamplerObject.h>

namespace MobileGL::MG_State::GLState {
class SamplerObject;
class ITextureObject;
}

namespace MobileGL::MG_Backend::DirectVulkan {
class VkSamplerManager {
public:
    struct InitInfo {
        VkDevice device = VK_NULL_HANDLE;
        const VulkanRendererConfig* config = nullptr;
        // The samplerAnisotropy device feature was requested and granted at vkCreateDevice.
        Bool samplerAnisotropySupported = false;
        // VkPhysicalDeviceLimits::maxSamplerAnisotropy.
        Float maxSamplerAnisotropy = 1.0f;
    };

    Bool Initialize(const InitInfo& initInfo);
    void Shutdown();

    VkSampler GetOrCreateSampler(const MG_State::GLState::SamplerObject& sampler,
                                 const MG_State::GLState::ITextureObject& texture,
                                 Bool forceNearestFiltering = false);
    // Frame boundary hook: ages the sampler cache and destroys samplers not used
    // for many frames. The key hashes continuous float state (lodBias, LOD clamps,
    // anisotropy), so an app animating those would otherwise mint an unbounded
    // stream of never-destroyed VkSamplers and eventually exhaust the device's
    // maxSamplerAllocationCount. A sampler idle for over a thousand frame
    // boundaries cannot be referenced by any in-flight command buffer (frames in
    // flight are single digits), and every descriptor set the GPU consumes is
    // written that same frame with live handles (the per-binding resolve memo and
    // descriptor-set reuse are both frame-reset), so destruction here needs no
    // fence wait. Self-gated: one counter bump and compare except on sweep
    // boundaries.
    void OnFrameBoundary();

private:
    struct SamplerCacheEntry {
        VkSampler handle = VK_NULL_HANDLE;
        Uint externalIndex = 0;
        Uint16 version = 0;
        // Frame boundary of the last cache hit; entries idle past the
        // OnFrameBoundary retirement age have their VkSampler destroyed.
        Uint64 lastUsedFrameBoundary = 0;
    };

    Uint64 BuildSamplerKey(const MG_State::GLState::SamplerObject& sampler,
                           const MG_State::GLState::ITextureObject& texture,
                           Bool forceNearestFiltering) const;
    static VkFilter ToVkFilter(SamplerFilterMode mode);
    static VkSamplerMipmapMode ToVkMipmapMode(SamplerMipmapMode mode);
    static VkSamplerAddressMode ToVkAddressMode(SamplerWrapMode mode);
    static VkCompareOp ToVkCompareOp(SamplerCompareFunc func);
    static SamplerCompareFunc ResolveCompareFunc(const MG_State::GLState::SamplerObject& sampler,
                                                 const MG_State::GLState::ITextureObject& texture);
    static VkBorderColor ResolveVkBorderColor(const MG_State::GLState::SamplerObject& sampler,
                                              const MG_State::GLState::ITextureObject& texture);
    // The anisotropy Vulkan will actually apply: 1.0 (i.e. disabled) unless the feature is on and
    // the sampler filters linearly both ways, otherwise the GL request clamped to the device limit.
    // GL happily carries GL_TEXTURE_MAX_ANISOTROPY on a NEAREST sampler (Blaze3D's blocks do exactly
    // that) while Vulkan forbids anisotropyEnable there, so the GL value must never be forwarded raw.
    Float ResolveEffectiveMaxAnisotropy(const MG_State::GLState::SamplerObject& sampler,
                                        Bool forceNearestFiltering) const;

    VkDevice m_device = VK_NULL_HANDLE;
    const VulkanRendererConfig* m_config = nullptr;
    Bool m_samplerAnisotropySupported = false;
    Float m_maxSamplerAnisotropy = 1.0f;
    UnorderedMap<Uint64, SamplerCacheEntry> m_samplers;
    // Monotonic frame-boundary counter (bumped in OnFrameBoundary) for cache aging.
    Uint64 m_frameBoundaryCounter = 0;
    static inline XXH64_state_t* m_hashState = XXH64_createState();
};
} // namespace MobileGL::MG_Backend::DirectVulkan
