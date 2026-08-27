// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkSamplerManager.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VkSamplerManager.h"

#include "MG_State/GLState/Core.h"

#include <algorithm>
#include <cmath>

namespace MobileGL::MG_Backend::DirectVulkan {
    namespace {
        Bool UsesBorderColor(const MG_State::GLState::SamplerObject& sampler) {
            return sampler.GetWrapS() == SamplerWrapMode::ClampToBorder ||
                   sampler.GetWrapT() == SamplerWrapMode::ClampToBorder ||
                   sampler.GetWrapR() == SamplerWrapMode::ClampToBorder;
        }

        // The numeric domain the texture is SAMPLED in. Vulkan splits VkBorderColor into a float
        // family and an integer family and requires the sampler's choice to match the image view's
        // format (a float border on an integer view, or the reverse, is undefined) - so the domain
        // comes from the TEXTURE, while the value comes from whichever GL entry point wrote it.
        enum class BorderColorDomain {
            Float,
            SignedInteger,
            UnsignedInteger
        };

        BorderColorDomain ResolveBorderColorDomain(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8I:
            case TextureInternalFormat::R16I:
            case TextureInternalFormat::R32I:
            case TextureInternalFormat::RG8I:
            case TextureInternalFormat::RG16I:
            case TextureInternalFormat::RG32I:
            case TextureInternalFormat::RGB8I:
            case TextureInternalFormat::RGB16I:
            case TextureInternalFormat::RGB32I:
            case TextureInternalFormat::RGBA8I:
            case TextureInternalFormat::RGBA16I:
            case TextureInternalFormat::RGBA32I:
                return BorderColorDomain::SignedInteger;
            case TextureInternalFormat::R8UI:
            case TextureInternalFormat::R16UI:
            case TextureInternalFormat::R32UI:
            case TextureInternalFormat::RG8UI:
            case TextureInternalFormat::RG16UI:
            case TextureInternalFormat::RG32UI:
            case TextureInternalFormat::RGB8UI:
            case TextureInternalFormat::RGB16UI:
            case TextureInternalFormat::RGB32UI:
            case TextureInternalFormat::RGBA8UI:
            case TextureInternalFormat::RGBA16UI:
            case TextureInternalFormat::RGBA32UI:
            case TextureInternalFormat::RGB10A2UI:
                return BorderColorDomain::UnsignedInteger;
            default:
                return BorderColorDomain::Float;
            }
        }

        Bool IsSignedNormalizedFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::R16Snorm:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RG16Snorm:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGB16Snorm:
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::RGBA16Snorm:
                return true;
            default:
                return false;
            }
        }

        // Float and half-float colour formats hold values outside [0,1] perfectly well, so their
        // border colour must NOT be clamped - the RGBA32F conformance rows use a border of 1.0 and
        // would be unaffected either way, but an HDR border of 4.0 must survive.
        Bool IsUnclampedColorFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R16F:
            case TextureInternalFormat::RG16F:
            case TextureInternalFormat::RGB16F:
            case TextureInternalFormat::RGBA16F:
            case TextureInternalFormat::R32F:
            case TextureInternalFormat::RG32F:
            case TextureInternalFormat::RGB32F:
            case TextureInternalFormat::RGBA32F:
            case TextureInternalFormat::R11FG11FB10F:
            case TextureInternalFormat::RGB9E5:
                return true;
            default:
                return false;
            }
        }

        Bool IsDepthTextureFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::DepthComponent:
            case TextureInternalFormat::DepthComponent16:
            case TextureInternalFormat::DepthComponent24:
            case TextureInternalFormat::DepthComponent32:
            case TextureInternalFormat::DepthComponent32F:
            case TextureInternalFormat::Depth24Stencil8:
            case TextureInternalFormat::Depth32FStencil8:
            case TextureInternalFormat::DepthStencil:
                return true;
            default:
                return false;
            }
        }

        Bool NearlyEqual(Float lhs, Float rhs) {
            return std::fabs(lhs - rhs) <= 1e-6f;
        }

        Float ResolveEffectiveMaxLod(const MG_State::GLState::SamplerObject& sampler) {
            if (sampler.GetMipmapMode() == SamplerMipmapMode::None) {
                return 0.0f;
            }
            return sampler.GetMaxLod();
        }

        Float ResolveEffectiveMinLod(const MG_State::GLState::SamplerObject& sampler, Float effectiveMaxLod) {
            return std::min(sampler.GetMinLod(), effectiveMaxLod);
        }

        // A single-level view can only ever deliver the base level, but the LOD clamp must not be
        // collapsed to exactly 0: both GL and Vulkan pick magFilter over minFilter from the
        // *clamped* lambda, so maxLod = 0 would make every fragment magnify and quietly retire the
        // min filter. 0.25 is the value VkSamplerCreateInfo's own note prescribes for emulating
        // GL's non-mipmapped minification - large enough for lambda to stay positive, small enough
        // that a NEAREST mip mode still rounds down to level 0. Clamped rather than assigned, so a
        // texture whose GL_TEXTURE_MAX_LOD really is 0 keeps magnifying as GL says it must.
        Float ResolveSingleLevelMaxLod(const MG_State::GLState::SamplerObject& sampler, Bool singleLevelView) {
            const Float maxLod = ResolveEffectiveMaxLod(sampler);
            return singleLevelView ? std::min(maxLod, 0.25f) : maxLod;
        }
    } // namespace

    Bool VkSamplerManager::Initialize(const InitInfo& initInfo) {
        Shutdown();

        m_device = initInfo.device;
        m_config = initInfo.config;
        m_samplerAnisotropySupported = initInfo.samplerAnisotropySupported;
        m_maxSamplerAnisotropy = std::max(initInfo.maxSamplerAnisotropy, 1.0f);
        m_customBorderColorSupported = initInfo.customBorderColorSupported;
        m_maxCustomBorderColorSamplers = initInfo.maxCustomBorderColorSamplers;
        m_customBorderColorSamplerCount = 0;
        MOBILEGL_ASSERT(m_device != VK_NULL_HANDLE && m_config != nullptr,
                        "VkSamplerManager::Initialize failed: invalid initialization info");
        return true;
    }

    Float VkSamplerManager::ResolveEffectiveMaxAnisotropy(const MG_State::GLState::SamplerObject& sampler,
                                                           Bool forceNearestFiltering) const {
        if (!m_samplerAnisotropySupported) return 1.0f;
        if (forceNearestFiltering) return 1.0f;
        // VUID-VkSamplerCreateInfo-anisotropyEnable-01071/01072: anisotropy requires both filters to
        // be LINEAR and the value to sit within [1, limits.maxSamplerAnisotropy].
        if (sampler.GetMinFilter() != SamplerFilterMode::Linear ||
            sampler.GetMagFilter() != SamplerFilterMode::Linear) {
            return 1.0f;
        }
        return std::clamp(sampler.GetMaxAnisotropy(), 1.0f, m_maxSamplerAnisotropy);
    }

    void VkSamplerManager::Shutdown() {
        for (auto& [_, sampler] : m_samplers) {
            if (m_device != VK_NULL_HANDLE && sampler.handle != VK_NULL_HANDLE) {
                vkDestroySampler(m_device, sampler.handle, nullptr);
            }
            sampler.handle = VK_NULL_HANDLE;
        }
        m_samplers.clear();

        m_device = VK_NULL_HANDLE;
        m_config = nullptr;
        m_frameBoundaryCounter = 0;
        m_customBorderColorSupported = false;
        m_maxCustomBorderColorSamplers = 0;
        m_customBorderColorSamplerCount = 0;
    }

    void VkSamplerManager::OnFrameBoundary() {
        ++m_frameBoundaryCounter;

        // Sweep occasionally; destroy samplers whose last use is far past every
        // in-flight frame. Destroy and erase must stay atomic, or Shutdown would
        // double-free the handle; an evicted key that recurs simply re-creates
        // its sampler on the next miss.
        constexpr Uint64 kSweepInterval = 256;
        constexpr Uint64 kRetireAgeBoundaries = 1024;
        if ((m_frameBoundaryCounter % kSweepInterval) != 0) {
            return;
        }

        for (auto it = m_samplers.begin(); it != m_samplers.end();) {
            auto& entry = it->second;
            if (m_frameBoundaryCounter - entry.lastUsedFrameBoundary > kRetireAgeBoundaries) {
                if (m_device != VK_NULL_HANDLE && entry.handle != VK_NULL_HANDLE) {
                    vkDestroySampler(m_device, entry.handle, nullptr);
                }
                if (entry.usesCustomBorderColor && m_customBorderColorSamplerCount > 0) {
                    --m_customBorderColorSamplerCount;
                }
                it = m_samplers.erase(it);
            } else {
                ++it;
            }
        }
    }

    Uint64 VkSamplerManager::BuildSamplerKey(const MG_State::GLState::SamplerObject& sampler,
                                             Bool forceNearestFiltering, Bool singleLevelView,
                                             const ResolvedBorderColor& borderColor) const {
        MOBILEGL_ASSERT(m_config != nullptr, "VkSamplerManager::BuildSamplerKey: m_config is null");
        XXHASH_VERIFY(XXH64_reset(m_hashState, m_config->CacheVersion));

        XXHASH_VERIFY(XXH64_update(m_hashState, &forceNearestFiltering, sizeof(forceNearestFiltering)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &singleLevelView, sizeof(singleLevelView)));

        const auto minFilter = sampler.GetMinFilter();
        XXHASH_VERIFY(XXH64_update(m_hashState, &minFilter, sizeof(minFilter)));
        const auto magFilter = sampler.GetMagFilter();
        XXHASH_VERIFY(XXH64_update(m_hashState, &magFilter, sizeof(magFilter)));
        const auto mipmapMode = sampler.GetMipmapMode();
        XXHASH_VERIFY(XXH64_update(m_hashState, &mipmapMode, sizeof(mipmapMode)));
        const auto wrapS = sampler.GetWrapS();
        XXHASH_VERIFY(XXH64_update(m_hashState, &wrapS, sizeof(wrapS)));
        const auto wrapT = sampler.GetWrapT();
        XXHASH_VERIFY(XXH64_update(m_hashState, &wrapT, sizeof(wrapT)));
        const auto wrapR = sampler.GetWrapR();
        XXHASH_VERIFY(XXH64_update(m_hashState, &wrapR, sizeof(wrapR)));
        const auto maxLod = ResolveSingleLevelMaxLod(sampler, singleLevelView);
        const auto minLod = ResolveEffectiveMinLod(sampler, maxLod);
        XXHASH_VERIFY(XXH64_update(m_hashState, &minLod, sizeof(minLod)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &maxLod, sizeof(maxLod)));
        const auto lodBias = sampler.GetLodBias();
        XXHASH_VERIFY(XXH64_update(m_hashState, &lodBias, sizeof(lodBias)));
        // The RESOLVED value, not the GL request: samplers that only differ in an anisotropy Vulkan
        // will not apply (NEAREST filtering, or requests past the device limit) must still share one
        // VkSampler, while two samplers that really do differ must not collide onto the first one's.
        const auto maxAnisotropy = ResolveEffectiveMaxAnisotropy(sampler, forceNearestFiltering);
        XXHASH_VERIFY(XXH64_update(m_hashState, &maxAnisotropy, sizeof(maxAnisotropy)));
        const auto compareMode = sampler.GetCompareMode();
        XXHASH_VERIFY(XXH64_update(m_hashState, &compareMode, sizeof(compareMode)));
        const auto compareFunc = sampler.GetSamplerCompareFunc();
        XXHASH_VERIFY(XXH64_update(m_hashState, &compareFunc, sizeof(compareFunc)));
        // The resolved enum AND, when it is one of the *_CUSTOM_EXT values, the sixteen bytes of the
        // colour itself: two samplers that differ only in a custom border colour carry the same enum
        // and would otherwise collide onto whichever one was created first.
        XXHASH_VERIFY(XXH64_update(m_hashState, &borderColor.color, sizeof(borderColor.color)));
        if (borderColor.isCustom) {
            XXHASH_VERIFY(XXH64_update(m_hashState, &borderColor.customValue, sizeof(borderColor.customValue)));
        }
        return XXH64_digest(m_hashState);
    }

    VkSampler VkSamplerManager::GetOrCreateSampler(const MG_State::GLState::SamplerObject& sampler,
                                                   const MG_State::GLState::ITextureObject& texture,
                                                   Bool forceNearestFiltering, Uint32 viewLevelCount) {
        // A view that exposes a single mip level has no second level to blend with, so GL's
        // *_MIPMAP_* minification filters degenerate to plain filtering on the base level -
        // sampling is unchanged by pinning the Vulkan sampler to NEAREST mip mode at LOD 0.
        // It is not cosmetic: MobileGL backs such a view with a fully allocated mip chain whose
        // tail is never written, and a LINEAR mip mode lets the texture unit issue the level+1
        // fetch anyway. On Adreno that fetch lands in uninitialized UBWC pages (or past the
        // allocation for a genuinely single-level image) and faults the GPU - the same failure
        // the default-framebuffer blit shader had to work around with an explicit-LOD sample.
        const Bool singleLevelView = viewLevelCount == 1;
        // Resolved once and used for both the key and the create-info; see ResolvedBorderColor.
        const ResolvedBorderColor borderColor = ResolveBorderColor(sampler, texture);
        const Uint64 key = BuildSamplerKey(sampler, forceNearestFiltering, singleLevelView, borderColor);
        auto it = m_samplers.find(key);
        if (it != m_samplers.end()) {
            it->second.lastUsedFrameBoundary = m_frameBoundaryCounter;
            return it->second.handle;
        }

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = forceNearestFiltering ? VK_FILTER_NEAREST : ToVkFilter(sampler.GetMagFilter());
        samplerInfo.minFilter = forceNearestFiltering ? VK_FILTER_NEAREST : ToVkFilter(sampler.GetMinFilter());
        samplerInfo.mipmapMode = (forceNearestFiltering || singleLevelView)
                                     ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                     : ToVkMipmapMode(sampler.GetMipmapMode());
        samplerInfo.addressModeU = ToVkAddressMode(sampler.GetWrapS());
        samplerInfo.addressModeV = ToVkAddressMode(sampler.GetWrapT());
        samplerInfo.addressModeW = ToVkAddressMode(sampler.GetWrapR());
        samplerInfo.mipLodBias = sampler.GetLodBias();
        // Must use the same resolver as BuildSamplerKey - a divergence would either collide two
        // different samplers or silently create duplicates.
        const Float maxAnisotropy = ResolveEffectiveMaxAnisotropy(sampler, forceNearestFiltering);
        samplerInfo.anisotropyEnable = maxAnisotropy > 1.0f ? VK_TRUE : VK_FALSE;
        samplerInfo.maxAnisotropy = maxAnisotropy;
        samplerInfo.compareEnable = sampler.GetCompareMode() == SamplerCompareMode::CompareToTexture ? VK_TRUE : VK_FALSE;
        samplerInfo.compareOp = ToVkCompareOp(sampler.GetSamplerCompareFunc());
        // Must match BuildSamplerKey's resolution exactly.
        samplerInfo.maxLod = ResolveSingleLevelMaxLod(sampler, singleLevelView);
        samplerInfo.minLod = ResolveEffectiveMinLod(sampler, samplerInfo.maxLod);
        samplerInfo.borderColor = borderColor.color;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;

        // VK_EXT_custom_border_color. `format` stays UNDEFINED, which is legal only because
        // customBorderColorWithoutFormat was required alongside customBorderColors at device
        // creation - a GL sampler object has no idea which texture it will be paired with.
        VkSamplerCustomBorderColorCreateInfoEXT customBorderColorInfo{};
        if (borderColor.isCustom) {
            customBorderColorInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT;
            customBorderColorInfo.customBorderColor = borderColor.customValue;
            customBorderColorInfo.format = VK_FORMAT_UNDEFINED;
            customBorderColorInfo.pNext = samplerInfo.pNext;
            samplerInfo.pNext = &customBorderColorInfo;
        }

        VkSampler vkSampler = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateSampler(m_device, &samplerInfo, nullptr, &vkSampler), "vkCreateSampler(texture)");

        SamplerCacheEntry entry{};
        entry.handle = vkSampler;
        entry.externalIndex = sampler.GetExternalIndex();
        entry.version = sampler.GetVersion();
        entry.lastUsedFrameBoundary = m_frameBoundaryCounter;
        entry.usesCustomBorderColor = borderColor.isCustom;
        if (entry.usesCustomBorderColor) {
            ++m_customBorderColorSamplerCount;
        }
        m_samplers[key] = entry;
        return vkSampler;
    }

    VkFilter VkSamplerManager::ToVkFilter(SamplerFilterMode mode) {
        return mode == SamplerFilterMode::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    }

    VkSamplerMipmapMode VkSamplerManager::ToVkMipmapMode(SamplerMipmapMode mode) {
        switch (mode) {
        case SamplerMipmapMode::Nearest:
            return VK_SAMPLER_MIPMAP_MODE_NEAREST;
        case SamplerMipmapMode::Linear:
            return VK_SAMPLER_MIPMAP_MODE_LINEAR;
        case SamplerMipmapMode::None:
        default:
            return VK_SAMPLER_MIPMAP_MODE_NEAREST;
        }
    }

    VkSamplerAddressMode VkSamplerManager::ToVkAddressMode(SamplerWrapMode mode) {
        switch (mode) {
        case SamplerWrapMode::ClampToEdge:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case SamplerWrapMode::MirroredRepeat:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case SamplerWrapMode::Repeat:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case SamplerWrapMode::ClampToBorder:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case SamplerWrapMode::MirrorClampToEdge:
            return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        default:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }

    VkCompareOp VkSamplerManager::ToVkCompareOp(SamplerCompareFunc func) {
        switch (func) {
        case SamplerCompareFunc::Never:
            return VK_COMPARE_OP_NEVER;
        case SamplerCompareFunc::Less:
            return VK_COMPARE_OP_LESS;
        case SamplerCompareFunc::Equal:
            return VK_COMPARE_OP_EQUAL;
        case SamplerCompareFunc::LessEqual:
            return VK_COMPARE_OP_LESS_OR_EQUAL;
        case SamplerCompareFunc::Greater:
            return VK_COMPARE_OP_GREATER;
        case SamplerCompareFunc::NotEqual:
            return VK_COMPARE_OP_NOT_EQUAL;
        case SamplerCompareFunc::GreaterEqual:
            return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case SamplerCompareFunc::Always:
        default:
            return VK_COMPARE_OP_ALWAYS;
        }
    }

    VkSamplerManager::ResolvedBorderColor VkSamplerManager::ResolveBorderColor(
        const MG_State::GLState::SamplerObject& sampler, const MG_State::GLState::ITextureObject& texture) const {
        ResolvedBorderColor resolved{};
        if (!UsesBorderColor(sampler)) {
            return resolved; // FLOAT_TRANSPARENT_BLACK, never sampled
        }

        // Border colour is sampler state: a bound sampler object supplies its own, and a texture
        // with none reaches the very same value through the sampler object it owns.
        const auto format = texture.GetFormat();
        const auto domain = ResolveBorderColorDomain(format);
        const Bool canUseCustom = m_customBorderColorSupported && m_maxCustomBorderColorSamplers > 0 &&
                                  m_customBorderColorSamplerCount < m_maxCustomBorderColorSamplers;

        if (domain != BorderColorDomain::Float) {
            // An integer image view REQUIRES an integer border colour, whatever the value is - even
            // (0,0,0,1). The value itself is whichever integer form the application wrote; a float
            // border on an integer texture is nonsense GL leaves undefined, so the derived integer
            // representation (a plain cast) is as good an answer as any.
            const auto& borderColorI = sampler.GetBorderColorI();
            const Bool allZeroRgb = borderColorI.x() == 0 && borderColorI.y() == 0 && borderColorI.z() == 0;
            if (allZeroRgb && borderColorI.w() == 0) {
                resolved.color = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
                return resolved;
            }
            if (allZeroRgb && borderColorI.w() == 1) {
                resolved.color = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
                return resolved;
            }
            if (borderColorI.x() == 1 && borderColorI.y() == 1 && borderColorI.z() == 1 && borderColorI.w() == 1) {
                resolved.color = VK_BORDER_COLOR_INT_OPAQUE_WHITE;
                return resolved;
            }
            if (canUseCustom) {
                resolved.color = VK_BORDER_COLOR_INT_CUSTOM_EXT;
                resolved.isCustom = true;
                if (domain == BorderColorDomain::UnsignedInteger) {
                    const auto& borderColorUI = sampler.GetBorderColorUI();
                    resolved.customValue.uint32[0] = borderColorUI.x();
                    resolved.customValue.uint32[1] = borderColorUI.y();
                    resolved.customValue.uint32[2] = borderColorUI.z();
                    resolved.customValue.uint32[3] = borderColorUI.w();
                } else {
                    resolved.customValue.int32[0] = borderColorI.x();
                    resolved.customValue.int32[1] = borderColorI.y();
                    resolved.customValue.int32[2] = borderColorI.z();
                    resolved.customValue.int32[3] = borderColorI.w();
                }
                return resolved;
            }
            // No custom colour available: pick the nearest of the three integer palette entries
            // rather than always answering transparent black, which is what turned an integer border
            // of (-1,-1,-1,-1) into 0 and broke the CTS's clamped-texel detection outright.
            const Bool opaque = borderColorI.w() != 0;
            const Bool bright = borderColorI.x() != 0 || borderColorI.y() != 0 || borderColorI.z() != 0;
            resolved.color = !opaque ? VK_BORDER_COLOR_INT_TRANSPARENT_BLACK
                                     : (bright ? VK_BORDER_COLOR_INT_OPAQUE_WHITE : VK_BORDER_COLOR_INT_OPAQUE_BLACK);
            return resolved;
        }

        // Float domain. GL 4.6 core 8.14.2/8.23: the border colour is interpreted in the texture's
        // format, so a fixed-point or depth texture clamps it to that format's representable range
        // first. Without the clamp the CTS's border of (255,255,255,255) on a GL_RGBA8 texture
        // matched none of the palette entries and fell through to transparent black - every border
        // texel sampled 0 where the test wanted 255.
        FloatVec4 borderColor = sampler.GetBorderColor();
        if (!IsUnclampedColorFormat(format)) {
            const Float lowerBound = IsSignedNormalizedFormat(format) ? -1.0f : 0.0f;
            borderColor = FloatVec4(std::clamp(borderColor.x(), lowerBound, 1.0f),
                                    std::clamp(borderColor.y(), lowerBound, 1.0f),
                                    std::clamp(borderColor.z(), lowerBound, 1.0f),
                                    std::clamp(borderColor.w(), lowerBound, 1.0f));
        }

        // A depth texture samples one component, so only x decides - and its alpha reads as 1.
        if (IsDepthTextureFormat(format)) {
            if (NearlyEqual(borderColor.x(), 1.0f)) {
                resolved.color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
                return resolved;
            }
            if (NearlyEqual(borderColor.x(), 0.0f)) {
                resolved.color = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
                return resolved;
            }
        }

        const Bool rgbZero = NearlyEqual(borderColor.x(), 0.0f) && NearlyEqual(borderColor.y(), 0.0f) &&
                             NearlyEqual(borderColor.z(), 0.0f);
        if (rgbZero && NearlyEqual(borderColor.w(), 0.0f)) {
            resolved.color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            return resolved;
        }
        if (rgbZero && NearlyEqual(borderColor.w(), 1.0f)) {
            resolved.color = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
            return resolved;
        }
        if (NearlyEqual(borderColor.x(), 1.0f) && NearlyEqual(borderColor.y(), 1.0f) &&
            NearlyEqual(borderColor.z(), 1.0f) && NearlyEqual(borderColor.w(), 1.0f)) {
            resolved.color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
            return resolved;
        }

        if (canUseCustom) {
            resolved.color = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            resolved.isCustom = true;
            resolved.customValue.float32[0] = borderColor.x();
            resolved.customValue.float32[1] = borderColor.y();
            resolved.customValue.float32[2] = borderColor.z();
            resolved.customValue.float32[3] = borderColor.w();
            return resolved;
        }

        // Nearest of the three float palette entries. Transparent black stays the answer for a
        // transparent border, which is what the old unconditional fallback got right by accident.
        const Bool opaque = borderColor.w() >= 0.5f;
        const Bool bright = (borderColor.x() + borderColor.y() + borderColor.z()) >= 1.5f;
        resolved.color = !opaque ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                                 : (bright ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK);
        return resolved;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
