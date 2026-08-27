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
        // VK_EXT_custom_border_color was enabled with BOTH customBorderColors and
        // customBorderColorWithoutFormat; see VulkanRenderer::m_customBorderColorFeatureEnabled.
        Bool customBorderColorSupported = false;
        // VkPhysicalDeviceCustomBorderColorPropertiesEXT::maxCustomBorderColorSamplers. A hard device
        // limit on how many LIVE samplers may carry a custom border colour, so the cache counts them
        // and falls back to the snapped predefined value once it is reached.
        Uint32 maxCustomBorderColorSamplers = 0;
    };

    Bool Initialize(const InitInfo& initInfo);
    void Shutdown();

    // viewLevelCount is the mip-level count of the image view this sampler will be paired
    // with; 0 means "unknown, do not narrow". See GetOrCreateSampler for why it matters.
    VkSampler GetOrCreateSampler(const MG_State::GLState::SamplerObject& sampler,
                                 const MG_State::GLState::ITextureObject& texture,
                                 Bool forceNearestFiltering = false,
                                 Uint32 viewLevelCount = 0);
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

    // What GL_TEXTURE_BORDER_COLOR resolves to for one (sampler, texture) pair. `color` is always a
    // legal VkBorderColor; when `isCustom` it is one of the *_CUSTOM_EXT values and `customValue`
    // carries the actual components in a VkSamplerCustomBorderColorCreateInfoEXT.
    //
    // Resolved ONCE per GetOrCreateSampler call and threaded into both the cache key and the
    // create-info, so the two cannot disagree - the same discipline the resolved anisotropy needs,
    // and here it also makes the maxCustomBorderColorSamplers fallback deterministic: whether a
    // custom colour was affordable is decided before the key is built, not twice with a budget
    // change in between.
    struct ResolvedBorderColor {
        VkBorderColor color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        VkClearColorValue customValue{};
        Bool isCustom = false;
    };

private:
    struct SamplerCacheEntry {
        VkSampler handle = VK_NULL_HANDLE;
        Uint externalIndex = 0;
        Uint16 version = 0;
        // Frame boundary of the last cache hit; entries idle past the
        // OnFrameBoundary retirement age have their VkSampler destroyed.
        Uint64 lastUsedFrameBoundary = 0;
        // Counted against maxCustomBorderColorSamplers for as long as this entry lives.
        Bool usesCustomBorderColor = false;
    };

    Uint64 BuildSamplerKey(const MG_State::GLState::SamplerObject& sampler, Bool forceNearestFiltering,
                           Bool singleLevelView, const ResolvedBorderColor& borderColor) const;
    static VkFilter ToVkFilter(SamplerFilterMode mode);
    static VkSamplerMipmapMode ToVkMipmapMode(SamplerMipmapMode mode);
    static VkSamplerAddressMode ToVkAddressMode(SamplerWrapMode mode);
    static VkCompareOp ToVkCompareOp(SamplerCompareFunc func);
    ResolvedBorderColor ResolveBorderColor(const MG_State::GLState::SamplerObject& sampler,
                                           const MG_State::GLState::ITextureObject& texture) const;
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
    Bool m_customBorderColorSupported = false;
    Uint32 m_maxCustomBorderColorSamplers = 0;
    // Live cache entries carrying a custom border colour. Kept in step with the entries themselves
    // in exactly the three places one can appear or disappear: creation, the OnFrameBoundary sweep,
    // and Shutdown.
    Uint32 m_customBorderColorSamplerCount = 0;
    UnorderedMap<Uint64, SamplerCacheEntry> m_samplers;
    // Monotonic frame-boundary counter (bumped in OnFrameBoundary) for cache aging.
    Uint64 m_frameBoundaryCounter = 0;
    static inline XXH64_state_t* m_hashState = XXH64_createState();
};
} // namespace MobileGL::MG_Backend::DirectVulkan
