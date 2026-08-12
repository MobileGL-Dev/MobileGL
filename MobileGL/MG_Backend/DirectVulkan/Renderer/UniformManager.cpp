// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/UniformDescriptorBinder.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "UniformManager.h"

#include "MG_Backend/DirectVulkan/DirectVulkanResourceState.h"
#include "MG_State/GLState/Core.h"
#include "MG_State/GLState/ProgramState/ProgramObject.h"
#include "MG_State/GLState/TextureState/TextureObject2D.h"
#include "MG_State/GLState/TextureState/TextureObjectBuffer.h"
#include "MG_Util/Converters/GLToMG/TextureEnumConverter.h"
#include "MG_Util/Converters/MGToStr/FramebufferEnumConverter.h"
#include "MG_Util/Converters/MGToVk/TextureEnumConverter.h"
#include "MG_Util/Metrics/TextureMetrics.h"
#include <Config.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace MobileGL::MG_Backend::DirectVulkan {
    namespace {
        constexpr Uint kFallbackTexture2DExternalIndex = 0xFFFFFF00u;
    }

    static Bool FindFramebufferAttachmentForTexture(const MG_State::GLState::FramebufferObject& framebuffer,
                                                    const MG_State::GLState::ITextureObject& texture,
                                                    FramebufferAttachmentType& outAttachment, Int& outLevel) {
        const auto& attachments = framebuffer.GetAllAttachmentObjects();
        for (SizeT i = 0; i < attachments.size(); ++i) {
            const auto attachmentType = static_cast<FramebufferAttachmentType>(i);
            if (attachmentType == FramebufferAttachmentType::None) {
                continue;
            }

            const auto& attachment = attachments[i];
            if (!attachment.IsTexture()) {
                continue;
            }

            auto attachedTexture = attachment.GetTexture();
            if (attachedTexture && attachedTexture.get() == &texture) {
                outAttachment = attachmentType;
                outLevel = attachment.GetTextureLevel();
                return true;
            }
        }

        return false;
    }

    static Bool IsValidSampledImageLayout(VkImageLayout layout) {
        switch (layout) {
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_GENERAL:
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
            return true;
        default:
            return false;
        }
    }

    // Uniform location of ELEMENT `element` of the opaque-uniform array at `baseLocation`, or
    // -1 when the reflection did not reserve that element. DoReflection hands out one location
    // per array element, so the element's location is the base plus its index - bounded by the
    // array's real extent so a descriptorCount that outran the reflection cannot walk onto the
    // next uniform. Element 0 is the ordinary non-array case and costs nothing extra.
    static Int ResolveDescriptorElementLocation(const MG_State::GLState::ProgramObject& program, Int baseLocation,
                                                Uint32 element) {
        if (baseLocation < 0 || element == 0) {
            return baseLocation;
        }
        const Int location = baseLocation + static_cast<Int>(element);
        return program.UniformLocationsAliasSameUniform(baseLocation, location) ? location : -1;
    }

    // descriptorCount this binding declares in the descriptor set layout (1 for everything that
    // is not an array). Kept in one place because the layout, the scratch reservation and the
    // per-element write loops must all agree on it.
    static Uint32 BindingDescriptorCount(const ProgramFactory::VkProgramObject& programObj, Uint32 binding) {
        return binding < programObj.bindingDescriptorCounts.size()
                   ? std::max<Uint32>(1u, programObj.bindingDescriptorCounts[binding])
                   : 1u;
    }

    static Int ResolveSamplerUnitIndex(const MG_State::GLState::ProgramObject& program, Int location, Uint32 binding) {
        MOBILEGL_ASSERT(location >= -1, "ResolveSamplerUnitIndex: invalid sampler location for binding %u", binding);
        if (location < 0) {
            return 0;
        }
        const Int uniformUnit = program.GetUniformSamplerOrImageUnitIndex(static_cast<Uint>(location));
        MOBILEGL_ASSERT(uniformUnit >= -1,
                        "ResolveSamplerUnitIndex: invalid texture unit for binding %u location %d (unit=%d)", binding,
                        location, uniformUnit);
        return uniformUnit >= 0 ? uniformUnit : 0;
    }

    VkFormat UniformManager::ResolveStorageImageViewFormat(VkFormat reflectedFormat, GLenum bindingFormat,
                                                           VkFormat resourceFormat, Bool useBindingFormat) {
        if (useBindingFormat) {
            const TextureInternalFormat bindingInternalFormat =
                MG_Util::ConvertGLEnumToTextureInternalFormat(bindingFormat);
            return MG_Util::ConvertTextureInternalFormatToVkEnum(bindingInternalFormat);
        }
        return reflectedFormat != VK_FORMAT_UNDEFINED ? reflectedFormat : resourceFormat;
    }

    Bool UniformManager::Initialize(VkDevice device, VkBufferManager* bufferManager,
                                             ProgramFactory* programFactory,
                                             VkDeviceSize minUniformBufferOffsetAlignment, Uint32 frameCount,
                                             Uint32 maxBindings, Uint32 setsPerFrame,
                                             VkTextureManager* textureManager, VkSamplerManager* samplerManager) {
        Shutdown();

        MOBILEGL_ASSERT(device != VK_NULL_HANDLE, "UniformDescriptorBinder::Initialize requires valid VkDevice");
        MOBILEGL_ASSERT(bufferManager != nullptr, "UniformDescriptorBinder::Initialize requires valid buffer manager");
        MOBILEGL_ASSERT(programFactory != nullptr,
                        "UniformDescriptorBinder::Initialize requires valid program factory");
        MOBILEGL_ASSERT(frameCount > 0, "UniformDescriptorBinder::Initialize requires frameCount > 0");
        MOBILEGL_ASSERT(maxBindings > 0, "UniformDescriptorBinder::Initialize requires maxBindings > 0");
        MOBILEGL_ASSERT(setsPerFrame > 0, "UniformDescriptorBinder::Initialize requires setsPerFrame > 0");
        MOBILEGL_ASSERT(textureManager != nullptr,
                        "UniformDescriptorBinder::Initialize requires valid texture manager");
        MOBILEGL_ASSERT(samplerManager != nullptr,
                        "UniformDescriptorBinder::Initialize requires valid sampler manager");

        m_device = device;
        m_bufferManager = bufferManager;
        m_programFactory = programFactory;
        m_minDynamicOffsetAlignment = std::max<VkDeviceSize>(1, minUniformBufferOffsetAlignment);
        m_frameCount = frameCount;
        m_maxBindings = maxBindings;
        m_samplerResolveMemo.assign(m_maxBindings, SamplerResolveMemo{});
        // Every entry is freshly constructed (all-invalid), so nothing needs sweeping until
        // a resolve writes one.
        m_samplerResolveMemoHighWater = 0;
        m_setsPerFrame = setsPerFrame;
        m_peakDescriptorSetsObserved = 0;
        m_textureManager = textureManager;
        m_samplerManager = samplerManager;

        m_frames.resize(m_frameCount);
        for (Uint32 frameIndex = 0; frameIndex < m_frameCount; ++frameIndex) {
            auto& frame = m_frames[frameIndex];
            frame.activeDescriptorPoolIndex = 0;
            frame.allocatedSetsThisFrame = 0;
            frame.peakAllocatedSetsThisFrame = 0;
            frame.descriptorPools.clear();

            VkDescriptorPool initialPool = VK_NULL_HANDLE;
            if (!CreateDescriptorPool(m_setsPerFrame, initialPool)) {
                MGLOG_E("UniformDescriptorBinder::Initialize failed: cannot create frame descriptor pool %u",
                        frameIndex);
                Shutdown();
                return false;
            }
            frame.descriptorPools.push_back({initialPool, m_setsPerFrame, 0});
            MGLOG_D("UniformDescriptorBinder: frame %u descriptor pool created (maxSets=%u)", frameIndex,
                    m_setsPerFrame);
        }

        return true;
    }

    void UniformManager::Shutdown() {
        for (auto& frame : m_frames) {
            if (m_device != VK_NULL_HANDLE) {
                for (auto& view : frame.texelBufferViews) {
                    if (view != VK_NULL_HANDLE) {
                        vkDestroyBufferView(m_device, view, nullptr);
                    }
                }
                frame.texelBufferViews.clear();
                frame.descriptorSetCacheByLayout.clear();
                for (auto& bucket : frame.descriptorPools) {
                    if (bucket.handle != VK_NULL_HANDLE) {
                        vkDestroyDescriptorPool(m_device, bucket.handle, nullptr);
                        bucket.handle = VK_NULL_HANDLE;
                    }
                }
            }
            frame.descriptorPools.clear();
            frame.activeDescriptorPoolIndex = 0;
            frame.allocatedSetsThisFrame = 0;
            frame.peakAllocatedSetsThisFrame = 0;
        }
        m_frames.clear();

        m_bufferManager = nullptr;
        m_programFactory = nullptr;
        m_device = VK_NULL_HANDLE;
        m_minDynamicOffsetAlignment = 1;
        m_frameCount = 0;
        m_maxBindings = 0;
        m_samplerResolveMemo.clear();
        m_samplerResolveMemoHighWater = 0;
        m_setsPerFrame = 0;
        m_peakDescriptorSetsObserved = 0;
        m_textureManager = nullptr;
        m_samplerManager = nullptr;
        m_fallbackTexture2D.reset();
    }

    void UniformManager::BeginFrame(Uint32 frameIndex) {
        MOBILEGL_ASSERT(frameIndex < m_frames.size(), "UniformDescriptorBinder::BeginFrame invalid frame index");
        auto& frame = m_frames[frameIndex];
        for (auto& view : frame.texelBufferViews) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyBufferView(m_device, view, nullptr);
            }
        }
        frame.texelBufferViews.clear();
        if (frame.peakAllocatedSetsThisFrame > m_peakDescriptorSetsObserved) {
            m_peakDescriptorSetsObserved = frame.peakAllocatedSetsThisFrame;
            MGLOG_D(
                "UniformDescriptorBinder: new descriptor set peak observed=%u (base setsPerFrame=%u, frame=%u, pools=%zu)",
                m_peakDescriptorSetsObserved, m_setsPerFrame, frameIndex, frame.descriptorPools.size());
        }
        frame.activeDescriptorPoolIndex = 0;
        frame.allocatedSetsThisFrame = 0;
        frame.peakAllocatedSetsThisFrame = 0;
        for (auto& cacheEntryPair : frame.descriptorSetCacheByLayout) {
            cacheEntryPair.second.cursor = 0;
        }
        // The frame's descriptor sets are recycled above, so last frame's reuse targets
        // are gone: start the per-draw descriptor-reuse cache fresh this frame.
        for (auto& entry : m_descriptorReuseMemo) {
            entry.valid = false;
        }
        m_fastRebindMemo.valid = false;
        m_lastBindValid = false;
        // Re-fingerprint the bound sampler set fresh this frame so any GL object address
        // reuse cannot outlive a single frame (see SamplerResolveMemo). Only the entries a
        // resolve has actually written can be valid, so the high-water mark bounds the
        // sweep - the vector itself is sized to the device's binding cap (256 here), which
        // is ~30x more entries than any program declares.
        const Uint32 touchedBindings =
            std::min<Uint32>(m_samplerResolveMemoHighWater, static_cast<Uint32>(m_samplerResolveMemo.size()));
        for (Uint32 binding = 0; binding < touchedBindings; ++binding) {
            m_samplerResolveMemo[binding].valid = false;
            m_samplerResolveMemo[binding].infoValid = false;
        }
    }

    void UniformManager::OnDescriptorSetLayoutDestroyed(VkDescriptorSetLayout descriptorSetLayout) {
        SizeT purgedSets = 0;
        for (auto& frame : m_frames) {
            const auto it = frame.descriptorSetCacheByLayout.find(descriptorSetLayout);
            if (it == frame.descriptorSetCacheByLayout.end()) {
                continue;
            }
            // Free the sets back to their pools and credit the bucket accounting, so
            // program churn recycles pool capacity instead of abandoning the slots.
            // GPU-safe: the layout only dies after >1024 idle frame boundaries, so no
            // in-flight command buffer references these sets.
            for (const auto& cached : it->second.sets) {
                if (cached.set == VK_NULL_HANDLE) {
                    continue;
                }
                vkFreeDescriptorSets(m_device, cached.pool, 1, &cached.set);
                const auto bucket = std::find_if(
                    frame.descriptorPools.begin(), frame.descriptorPools.end(),
                    [&cached](const DescriptorPoolBucket& candidate) { return candidate.handle == cached.pool; });
                if (bucket != frame.descriptorPools.end() && bucket->allocatedSets > 0) {
                    --bucket->allocatedSets;
                }
            }
            purgedSets += it->second.sets.size();
            frame.descriptorSetCacheByLayout.erase(it);
        }
        if (purgedSets > 0) {
            // The per-draw reuse memo folds the layout handle into its signature; drop
            // every entry so a recycled handle value cannot revive a purged set mid-frame.
            for (auto& entry : m_descriptorReuseMemo) {
                entry.valid = false;
            }
            // The rebind memo's set may be among the freed ones.
            m_fastRebindMemo.valid = false;
            MGLOG_D("UniformDescriptorBinder: freed %zu descriptor sets for destroyed layout", purgedSets);
        }
    }

    Bool UniformManager::ResolveSamplerDescriptor(VkCommandBuffer commandBuffer,
                                                            const MG_State::GLState::ProgramObject& program,
                                                            const ProgramFactory::VkProgramObject& programObj,
                                                            Uint32 binding, Uint32 element,
                                                            VkDescriptorImageInfo& outImageInfo,
                                                            Bool trustUnchangedHint) const {
        MOBILEGL_ASSERT(m_textureManager != nullptr, "ResolveSamplerDescriptor: texture manager is null");
        MOBILEGL_ASSERT(m_samplerManager != nullptr, "ResolveSamplerDescriptor: sampler manager is null");
        // The whole-descriptor memo below is keyed by binding alone, so it describes a binding
        // that carries exactly one descriptor. An arrayed binding's elements would overwrite
        // each other in it (see SamplerResolveMemo::info); they re-resolve instead.
        const Bool descriptorMemoUsable = BindingDescriptorCount(programObj, binding) == 1u;
        // The caller proved every input of this binding's resolution unchanged since the
        // last full resolve (which also filled the cache), so the whole chain below -
        // texture/sampler resolution, completeness probe, sync, layout handling, sampler
        // and view lookups - would recompute the identical descriptor.
        if (trustUnchangedHint && descriptorMemoUsable && binding < m_samplerResolveMemo.size() &&
            m_samplerResolveMemo[binding].infoValid) {
            outImageInfo = m_samplerResolveMemo[binding].info;
            return true;
        }
        MOBILEGL_ASSERT(binding < programObj.samplerNameByBinding.size(),
                        "ResolveSamplerDescriptor: sampler binding %u name lookup out of range", binding);
        // Per ELEMENT, and resolved BEFORE anything is looked up through it: GLSL 4.20 gives every
        // element of `uniform sampler2D goku[4]` its own texture unit (consecutive from the
        // declared binding, but glUniform1i may scatter them afterwards), so the unit - and with
        // it the bound texture, the unit's sampler override and the fallback decision - is the
        // element's, not the binding's. An element past the array's reserved extent has no unit
        // at all, and must not fall back to resolving unit 0's texture.
        const Int location =
            ResolveDescriptorElementLocation(program, programObj.samplerUniformLocationByBinding[binding], element);
        if (location < 0 && element > 0) {
            MGLOG_D("ResolveSamplerDescriptor: binding %u element %u is past the end of its sampler array", binding,
                    element);
            return false;
        }
        const Int unit = ResolveSamplerUnitIndex(program, location, binding);
        // Raw-pointer resolve to skip the SharedPtr atomic refcount churn: the bound texture stays
        // alive through the draw via GL binding state. Only the fallback path needs a SharedPtr to
        // keep the fallback texture alive for the rest of this call.
        MG_State::GLState::ITextureObject* texture = ResolveSamplerTextureRaw(program, programObj, binding, element);
        auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
        const auto& samplerOverride = textureUnit.GetSamplerObject();
        const auto preferredTarget = programObj.samplerTextureTargetByBinding[binding];
        SharedPtr<MG_State::GLState::ITextureObject> fallbackHolder;
        // A texture that fails the completeness rules for the filter in effect reads
        // (0, 0, 0, 1), which is exactly what the fallback texture holds - so it takes the
        // same route as a sampler with nothing bound.
        if (texture != nullptr &&
            MG_State::GLState::SamplesAsIncompleteTexture(
                texture, samplerOverride ? samplerOverride.get() : texture->GetSamplerObject().get())) {
            texture = nullptr;
        }
        if (texture == nullptr) {
            fallbackHolder = GetFallbackTexture(preferredTarget);
            texture = fallbackHolder.get();
            if (texture == nullptr) {
                MGLOG_E("ResolveSamplerDescriptor: no fallback texture available for binding=%u ('%s') "
                        "location=%d unit=%d target=%d",
                        binding, programObj.samplerNameByBinding[binding].c_str(), location, unit,
                        static_cast<Int>(preferredTarget));
                return false;
            }
            MGLOG_W(
                "ResolveSamplerDescriptor: using fallback texture for unbound sampler binding=%u ('%s') location=%d unit=%d target=%d",
                binding, programObj.samplerNameByBinding[binding].c_str(), location, unit,
                static_cast<Int>(preferredTarget));
        }

        const MG_State::GLState::SamplerObject* samplerToUse =
            samplerOverride ? samplerOverride.get() : texture->GetSamplerObject().get();
        if (samplerToUse == nullptr) {
            MGLOG_E(
                "ResolveSamplerDescriptor: sampler binding %u ('%s') has no sampler object (textureId=%d location=%d unit=%d)",
                binding, programObj.samplerNameByBinding[binding].c_str(), texture->GetExternalIndex(), location,
                unit);
            return false;
        }
        VkTextureManager::TextureResource* resource = m_textureManager->SyncTextureAndGetDescriptor(*texture);
        if (resource == nullptr) {
            MGLOG_E(
                "ResolveSamplerDescriptor: sampler binding %u ('%s') failed to create/sync texture resource (textureId=%d target=%d location=%d unit=%d)",
                binding, programObj.samplerNameByBinding[binding].c_str(), texture->GetExternalIndex(),
                static_cast<Int>(texture->GetTarget()), location, unit);
            return false;
        }
        if (!IsValidSampledImageLayout(resource->layout)) {
            auto drawFbo = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
            FramebufferAttachmentType attachmentType = FramebufferAttachmentType::None;
            Int attachmentLevel = 0;
            if (drawFbo &&
                FindFramebufferAttachmentForTexture(*drawFbo, *texture, attachmentType, attachmentLevel)) {
                MGLOG_W("ResolveSamplerDescriptor: framebuffer feedback loop detected: textureId=%d is bound "
                        "for sampling at binding=%u, but is also attached to drawFbo=%u as %s (level=%d, "
                        "trackedLayout=%d)",
                        texture->GetExternalIndex(), binding, drawFbo->GetExternalIndex(),
                        MG_Util::ConvertFramebufferAttachmentTypeToString(attachmentType).c_str(),
                        attachmentLevel, static_cast<Int>(resource->layout));
            }

            const Bool readyForSampling = m_textureManager->TransitionTextureForSampling(commandBuffer, *texture);
            if (!readyForSampling) {
                MGLOG_E("ResolveSamplerDescriptor: failed to transition textureId=%d for sampler binding=%u",
                        texture->GetExternalIndex(), binding);
                return false;
            }
            resource = m_textureManager->SyncTextureAndGetDescriptor(*texture);
            MOBILEGL_ASSERT(resource != nullptr,
                            "ResolveSamplerDescriptor: failed to resync textureId=%d after sampling transition",
                            texture->GetExternalIndex());
            MOBILEGL_ASSERT(IsValidSampledImageLayout(resource->layout),
                            "ResolveSamplerDescriptor: invalid sampled image layout=%d for textureId=%d, binding=%u",
                            static_cast<Int>(resource->layout), texture->GetExternalIndex(), binding);
        }

        MOBILEGL_ASSERT(binding < programObj.samplerNumericDomainByBinding.size(),
                        "ResolveSamplerDescriptor: sampler numeric-domain binding %u out of range", binding);
        const SamplerNumericDomain numericDomain = programObj.samplerNumericDomainByBinding[binding];
        // Vulkan forbids linear filtering and anisotropy for integer sampled-image formats.
        // Some desktop GL shader packs deliberately bit-read a mutable float texture through a
        // usampler and still leave the texture's ordinary linear parameters in place; texelFetch
        // ignores filtering, so a nearest VkSampler preserves the operation while keeping the
        // descriptor valid.
        const Bool forceNearestFiltering = numericDomain == SamplerNumericDomain::SignedInteger ||
                                           numericDomain == SamplerNumericDomain::UnsignedInteger;
        SamplerResolveMemo* viewFormatMemo =
            binding < m_samplerResolveMemo.size() ? &m_samplerResolveMemo[binding] : nullptr;
        VkFormat sampledViewFormat;
        if (viewFormatMemo != nullptr && viewFormatMemo->viewFormatValid &&
            viewFormatMemo->viewFormatSource == resource->format &&
            viewFormatMemo->viewFormatDomain == numericDomain) {
            sampledViewFormat = viewFormatMemo->viewFormat;
        } else {
            sampledViewFormat =
                VkTextureManager::ResolveSampledImageViewFormat(resource->format, numericDomain);
            if (viewFormatMemo != nullptr) {
                viewFormatMemo->viewFormatSource = resource->format;
                viewFormatMemo->viewFormatDomain = numericDomain;
                viewFormatMemo->viewFormat = sampledViewFormat;
                viewFormatMemo->viewFormatValid = true;
                NoteSamplerResolveMemoTouched(binding);
            }
        }
        if (sampledViewFormat == VK_FORMAT_UNDEFINED) {
            MGLOG_E("ResolveSamplerDescriptor: no compatible sampled view for binding=%u ('%s') "
                    "textureId=%d imageFormat=%d numericDomain=%d",
                    binding, programObj.samplerNameByBinding[binding].c_str(), texture->GetExternalIndex(),
                    static_cast<Int>(resource->format), static_cast<Int>(numericDomain));
            return false;
        }
        // No reinterpretation requested: bind the depth-or-color aspect view the sync above
        // already produced instead of re-entering GetOrCreateSampledImageView's sync path.
        const VkImageView sampledImageView =
            sampledViewFormat == resource->format
                ? resource->sampledView
                : m_textureManager->GetOrCreateSampledImageView(*texture, sampledViewFormat);
        if (sampledImageView == VK_NULL_HANDLE) {
            MGLOG_E("ResolveSamplerDescriptor: failed to resolve sampled view for binding=%u ('%s') "
                    "textureId=%d imageFormat=%d viewFormat=%d numericDomain=%d",
                    binding, programObj.samplerNameByBinding[binding].c_str(), texture->GetExternalIndex(),
                    static_cast<Int>(resource->format), static_cast<Int>(sampledViewFormat),
                    static_cast<Int>(numericDomain));
            return false;
        }
        // Skip GetOrCreateSampler's per-draw key hash + map lookup when this binding's
        // sampler object and texture (both by lifetime id + version) are unchanged from the
        // last draw that resolved it: the resulting sampler key, and therefore the VkSampler
        // handle, are guaranteed identical. Lifetime ids are never reused, so a freed-and-
        // reallocated sampler/texture at the same address gets a fresh id and misses instead
        // of false-hitting. Cached handles live until Shutdown, so the memo can never hand
        // back a destroyed sampler.
        VkSampler resolvedSampler = VK_NULL_HANDLE;
        if (binding < m_samplerResolveMemo.size()) {
            auto& memo = m_samplerResolveMemo[binding];
            const Uint64 samplerLifetimeId = samplerToUse->GetLifetimeId();
            const Uint16 samplerVersion = samplerToUse->GetVersion();
            const Uint64 textureLifetimeId = texture->GetLifetimeId();
            const Uint16 textureParamsVersion = texture->GetTextureParamsVersion();
            // The sampler's LOD clamp depends on how many levels the sampled view exposes, and that
            // follows uploads as well as GL parameters - so it belongs in the memo key too.
            const Uint32 viewLevelCount = resource->sampledLevelCount;
            if (memo.valid && memo.samplerLifetimeId == samplerLifetimeId && memo.samplerVersion == samplerVersion &&
                memo.textureLifetimeId == textureLifetimeId && memo.textureParamsVersion == textureParamsVersion &&
                memo.forceNearestFiltering == forceNearestFiltering && memo.viewLevelCount == viewLevelCount) {
                resolvedSampler = memo.sampler;
            } else {
                resolvedSampler = m_samplerManager->GetOrCreateSampler(*samplerToUse, *texture,
                                                                       forceNearestFiltering, viewLevelCount);
                memo.samplerLifetimeId = samplerLifetimeId;
                memo.samplerVersion = samplerVersion;
                memo.textureLifetimeId = textureLifetimeId;
                memo.textureParamsVersion = textureParamsVersion;
                memo.forceNearestFiltering = forceNearestFiltering;
                memo.viewLevelCount = viewLevelCount;
                memo.sampler = resolvedSampler;
                memo.valid = true;
                NoteSamplerResolveMemoTouched(binding);
            }
        } else {
            resolvedSampler = m_samplerManager->GetOrCreateSampler(*samplerToUse, *texture, forceNearestFiltering,
                                                                   resource->sampledLevelCount);
        }
        outImageInfo = {
            .sampler = resolvedSampler,
            .imageView = sampledImageView,
            .imageLayout = resource->layout,
        };
        if (outImageInfo.sampler == VK_NULL_HANDLE) {
            return false;
        }
        // Only for a binding that carries a single descriptor - an array's elements would
        // publish each other's descriptors here, and the next hinted draw would hand element
        // N-1's texture to element 0.
        if (binding < m_samplerResolveMemo.size()) {
            if (descriptorMemoUsable) {
                m_samplerResolveMemo[binding].info = outImageInfo;
                m_samplerResolveMemo[binding].infoValid = true;
            } else {
                // An arrayed binding publishes nothing here, and clears what a previous program
                // published at this index. Not strictly required - the hint's proof obligations
                // are program-scoped and the entry is reset every frame - but leaving another
                // program's descriptor sitting in a slot this one never refreshes is the kind of
                // thing the next reader has to re-derive is safe.
                m_samplerResolveMemo[binding].infoValid = false;
            }
            NoteSamplerResolveMemoTouched(binding);
        }
        return true;
    }

    Bool UniformManager::ResolveSamplerDescriptorOverride(
        const SamplerBindingOverride& samplerBindingOverride, VkDescriptorImageInfo& outImageInfo) const {
        MOBILEGL_ASSERT(m_textureManager != nullptr, "ResolveSamplerDescriptorOverride: texture manager is null");
        MOBILEGL_ASSERT(m_samplerManager != nullptr, "ResolveSamplerDescriptorOverride: sampler manager is null");
        MOBILEGL_ASSERT(samplerBindingOverride.texture != nullptr,
                        "ResolveSamplerDescriptorOverride: override texture is null for binding %u",
                        samplerBindingOverride.binding);
        MOBILEGL_ASSERT(samplerBindingOverride.sampler != nullptr,
                        "ResolveSamplerDescriptorOverride: override sampler is null for binding %u",
                        samplerBindingOverride.binding);

        auto* resource = m_textureManager->SyncTextureAndGetDescriptor(*samplerBindingOverride.texture);
        MOBILEGL_ASSERT(resource != nullptr,
                        "ResolveSamplerDescriptorOverride: failed to sync override texture resource for binding %u textureId=%d",
                        samplerBindingOverride.binding, samplerBindingOverride.texture->GetExternalIndex());
        MOBILEGL_ASSERT(IsValidSampledImageLayout(resource->layout),
                        "ResolveSamplerDescriptorOverride: invalid layout %d for binding %u textureId=%d",
                        static_cast<Int>(resource->layout), samplerBindingOverride.binding,
                        samplerBindingOverride.texture->GetExternalIndex());

        outImageInfo = {
            .sampler = m_samplerManager->GetOrCreateSampler(*samplerBindingOverride.sampler,
                                                            *samplerBindingOverride.texture),
            .imageView = samplerBindingOverride.imageView != VK_NULL_HANDLE ?
                samplerBindingOverride.imageView :
                (resource->sampledView != VK_NULL_HANDLE ? resource->sampledView : resource->fullView),
            .imageLayout = resource->layout,
        };
        return outImageInfo.sampler != VK_NULL_HANDLE;
    }

    Bool UniformManager::ProgramSamplesOnlySingleLevelTextures(
        const MG_State::GLState::ProgramObject& program, const ProgramFactory::VkProgramObject& programObj) {
        // A declined program never draws (see VkProgramObject::declinedDescriptors), and its
        // declined binding has no resolvable uniform location - so there is nothing to prove
        // about the textures it would have sampled.
        if (programObj.declinedDescriptors) {
            return false;
        }
        Bool sawSampler = false;
        for (Uint32 binding = 0; binding < programObj.bindingKinds.size(); ++binding) {
            if (programObj.bindingKinds[binding] != ProgramFactory::DescriptorBindingKind::CombinedImageSampler) {
                continue;
            }
            // The rewrite this gates is program-wide, so EVERY sampler the program can read has
            // to qualify - including every element of a sampler array, each of which reaches a
            // different texture through its own unit.
            const Uint32 descriptorCount = BindingDescriptorCount(programObj, binding);
            for (Uint32 element = 0; element < descriptorCount; ++element) {
                // The element's own location first, exactly as ResolveSamplerDescriptor resolves
                // it - an element with no location would otherwise be judged on unit 0's texture.
                const Int location = ResolveDescriptorElementLocation(
                    program, programObj.samplerUniformLocationByBinding[binding], element);
                if (location < 0 && element > 0) return false;
                const auto* texture = ResolveSamplerTextureRaw(program, programObj, binding, element);
                if (texture == nullptr) return false;
                const auto& levelRange = texture->GetLevelRange();
                if (levelRange.x() != levelRange.y()) return false;

                // An explicit-LOD sample is a single filtered tap, so it also gives up anisotropic
                // filtering - which a single-level view can still have. Resolve the sampler exactly
                // the way ResolveSamplerDescriptor does and bail if anisotropy would apply.
                const Int unit = ResolveSamplerUnitIndex(program, location, binding);
                const auto& samplerOverride = MG_State::pGLContext->GetTextureUnitObject(unit).GetSamplerObject();
                const auto* effectiveSampler =
                    samplerOverride ? samplerOverride.get() : texture->GetSamplerObject().get();
                if (effectiveSampler == nullptr) return false;
                if (effectiveSampler->GetMaxAnisotropy() > 1.0f &&
                    effectiveSampler->GetMinFilter() == SamplerFilterMode::Linear &&
                    effectiveSampler->GetMagFilter() == SamplerFilterMode::Linear) {
                    return false;
                }

                // An explicit LOD 0 makes lambda exactly 0, which is the magnification side of the
                // min/mag decision. That only matches the implicit form when lambda could not have been
                // positive anyway (the LOD clamp already pins it at or below 0), or when the two
                // filters are the same and the choice cannot be observed.
                const Float effectiveMaxLod = effectiveSampler->GetMipmapMode() == SamplerMipmapMode::None
                                                  ? 0.0f
                                                  : effectiveSampler->GetMaxLod();
                if (effectiveMaxLod > 0.0f && effectiveSampler->GetMinFilter() != effectiveSampler->GetMagFilter()) {
                    return false;
                }
                sawSampler = true;
            }
        }
        return sawSampler;
    }

    Bool UniformManager::ResolveSamplerTexture(const MG_State::GLState::ProgramObject& program,
                                                         const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                                         SharedPtr<MG_State::GLState::ITextureObject>& outTexture) {
        outTexture.reset();
        MOBILEGL_ASSERT(MG_State::pGLContext != nullptr, "ResolveSamplerTexture: GL context is null");
        MOBILEGL_ASSERT(binding < programObj.samplerUniformLocationByBinding.size(),
                        "ResolveSamplerTexture: sampler location binding %u out of range", binding);
        MOBILEGL_ASSERT(binding < programObj.samplerTextureTargetByBinding.size(),
                        "ResolveSamplerTexture: sampler target binding %u out of range", binding);

        const Int location = programObj.samplerUniformLocationByBinding[binding];
        const Int unit = ResolveSamplerUnitIndex(program, location, binding);

        auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
        const TextureTarget preferredTarget = programObj.samplerTextureTargetByBinding[binding];
        outTexture = textureUnit.GetBindingSlot(preferredTarget).GetBoundObject();
        // The slot always holds at least the target's default texture (name 0). While that
        // default has no image it is unsampleable; report it as "unbound" so callers keep
        // taking their fallback paths instead of trying to sync a storage-less texture.
        if (MG_State::GLState::IsUndefinedDefaultTexture(outTexture.get())) {
            outTexture.reset();
        }

        return true;
    }

    MG_State::GLState::ITextureObject* UniformManager::ResolveSamplerTextureRaw(
        const MG_State::GLState::ProgramObject& program, const ProgramFactory::VkProgramObject& programObj,
        Uint32 binding, Uint32 element) {
        MOBILEGL_ASSERT(MG_State::pGLContext != nullptr, "ResolveSamplerTextureRaw: GL context is null");
        MOBILEGL_ASSERT(binding < programObj.samplerUniformLocationByBinding.size(),
                        "ResolveSamplerTextureRaw: sampler location binding %u out of range", binding);
        MOBILEGL_ASSERT(binding < programObj.samplerTextureTargetByBinding.size(),
                        "ResolveSamplerTextureRaw: sampler target binding %u out of range", binding);

        const Int location =
            ResolveDescriptorElementLocation(program, programObj.samplerUniformLocationByBinding[binding], element);
        const Int unit = ResolveSamplerUnitIndex(program, location, binding);

        auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
        const TextureTarget preferredTarget = programObj.samplerTextureTargetByBinding[binding];
        // GetBoundObject() returns the SharedPtr by const ref; .get() reads the pointer without
        // touching the refcount (no atomic inc/dec per binding per draw).
        MG_State::GLState::ITextureObject* texture =
            textureUnit.GetBindingSlot(preferredTarget).GetBoundObject().get();
        // The slot always holds at least the target's default texture (name 0). While that
        // default has no image it is unsampleable; report it as "unbound" so the caller
        // substitutes its fallback texture exactly like it did for the old null slot.
        if (MG_State::GLState::IsUndefinedDefaultTexture(texture)) {
            return nullptr;
        }
        return texture;
    }

    Bool UniformManager::ResolveTexelBufferDescriptor(const MG_State::GLState::ProgramObject& program,
                                                      const ProgramFactory::VkProgramObject& programObj,
                                                      Uint32 binding, Uint32 frameIndex,
                                                      VkBufferView& outBufferView) {
        outBufferView = VK_NULL_HANDLE;
        MOBILEGL_ASSERT(m_bufferManager != nullptr, "ResolveTexelBufferDescriptor: buffer manager is null");
        MOBILEGL_ASSERT(frameIndex < m_frames.size(), "ResolveTexelBufferDescriptor: frame index out of range");

        SharedPtr<MG_State::GLState::ITextureObject> texture;
        if (!ResolveSamplerTexture(program, programObj, binding, texture) || texture == nullptr) {
            MGLOG_E("ResolveTexelBufferDescriptor: texture buffer binding %u ('%s') is unbound", binding,
                    programObj.samplerNameByBinding[binding].c_str());
            return false;
        }

        if (texture->GetStorageType() != TextureStorageType::Buffer ||
            texture->GetTarget() != TextureTarget::TextureBuffer) {
            MGLOG_E(
                "ResolveTexelBufferDescriptor: binding %u ('%s') expected texture buffer, got textureId=%u target=%d storage=%d",
                binding, programObj.samplerNameByBinding[binding].c_str(), texture->GetExternalIndex(),
                static_cast<Int>(texture->GetTarget()), static_cast<Int>(texture->GetStorageType()));
            return false;
        }

        auto* textureBuffer = static_cast<MG_State::GLState::TextureObjectBuffer*>(texture.get());
        const auto& bufferObject = textureBuffer->GetBufferBindingSlot().GetBoundObject();
        if (bufferObject == nullptr) {
            MGLOG_E("ResolveTexelBufferDescriptor: texture buffer binding %u ('%s') has no GL buffer bound",
                    binding, programObj.samplerNameByBinding[binding].c_str());
            return false;
        }

        BufferSlice slice{};
        if (!m_bufferManager->AcquireResidentSlice(BufferKind::TextureBuffer, bufferObject, slice) || !slice.IsValid()) {
            MGLOG_E("ResolveTexelBufferDescriptor: failed to sync GL buffer %u for texture buffer %u",
                    bufferObject->GetExternalIndex(), texture->GetExternalIndex());
            return false;
        }

        const auto internalFormat = textureBuffer->GetFormat();
        const VkFormat vkFormat = MG_Util::ConvertTextureInternalFormatToVkEnum(internalFormat);
        if (vkFormat == VK_FORMAT_UNDEFINED) {
            MGLOG_E("ResolveTexelBufferDescriptor: unsupported texture buffer internal format %d",
                    static_cast<Int>(internalFormat));
            return false;
        }

        const VkDeviceSize texelSize =
            static_cast<VkDeviceSize>(MG_Util::GetSizedInternalFormatSizeInBytes(internalFormat));
        // glTextureBufferRange addresses a window of the buffer, not all of it; the whole-buffer
        // forms report the buffer's current size here, so both go through the same clamp.
        const VkDeviceSize rangeOffset = static_cast<VkDeviceSize>(textureBuffer->GetBufferRangeOffset());
        const VkDeviceSize rangeSize = static_cast<VkDeviceSize>(textureBuffer->GetBufferRangeSizeInBytes());
        VkDeviceSize viewRange = std::min(rangeSize, slice.size > rangeOffset ? slice.size - rangeOffset : 0);
        if (texelSize > 0) {
            viewRange = (viewRange / texelSize) * texelSize;
        }
        if (viewRange == 0) {
            MGLOG_E("ResolveTexelBufferDescriptor: texture buffer %u has empty view range", texture->GetExternalIndex());
            return false;
        }

        VkBufferViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        viewInfo.buffer = slice.buffer;
        viewInfo.format = vkFormat;
        viewInfo.offset = slice.offset + rangeOffset;
        viewInfo.range = viewRange;

        VkBufferView bufferView = VK_NULL_HANDLE;
        const VkResult result = vkCreateBufferView(m_device, &viewInfo, nullptr, &bufferView);
        if (result != VK_SUCCESS || bufferView == VK_NULL_HANDLE) {
            MGLOG_E("ResolveTexelBufferDescriptor: vkCreateBufferView failed result=%d format=%d range=%zu",
                    result, static_cast<Int>(vkFormat), static_cast<SizeT>(viewRange));
            return false;
        }

        m_frames[frameIndex].texelBufferViews.push_back(bufferView);
        outBufferView = bufferView;
        return true;
    }

    // GLSL `imageBuffer`. The one image uniform whose Vulkan descriptor is a VkBufferView rather
    // than a VkImageView, so it is half ResolveStorageImageDescriptor (the resource comes from an
    // IMAGE unit, i.e. from glBindImageTexture, not from a texture unit) and half
    // ResolveTexelBufferDescriptor (the descriptor is a buffer view over the GL buffer the
    // texture is attached to).
    //
    // Before this existed the descriptor kind reflected as SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_-
    // TEXEL_BUFFER and fell into ReflectDescriptorTypeToBindingKind's `default:`, whose only
    // complaint is an assert that compiles out above DEBUG - so a release build declared no
    // binding at all for a uniform the shader still read, and lavapipe segfaulted inside pipeline
    // creation on the JIT worker thread. KHR-GL44.multi_bind.dispatch_bind_image_textures is the
    // case that carries it.
    Bool UniformManager::ResolveStorageTexelBufferDescriptor(const MG_State::GLState::ProgramObject& program,
                                                             const ProgramFactory::VkProgramObject& programObj,
                                                             Uint32 binding, Uint32 frameIndex,
                                                             VkBufferView& outBufferView) {
        outBufferView = VK_NULL_HANDLE;
        MOBILEGL_ASSERT(m_bufferManager != nullptr, "ResolveStorageTexelBufferDescriptor: buffer manager is null");
        MOBILEGL_ASSERT(MG_State::pGLContext != nullptr, "ResolveStorageTexelBufferDescriptor: GL context is null");
        MOBILEGL_ASSERT(frameIndex < m_frames.size(),
                        "ResolveStorageTexelBufferDescriptor: frame index out of range");
        MOBILEGL_ASSERT(binding < programObj.samplerUniformLocationByBinding.size(),
                        "ResolveStorageTexelBufferDescriptor: binding %u out of range", binding);

        const Int location = programObj.samplerUniformLocationByBinding[binding];
        if (location < 0) {
            MGLOG_E("ResolveStorageTexelBufferDescriptor: binding %u ('%s') has no uniform location", binding,
                    programObj.samplerNameByBinding[binding].c_str());
            return false;
        }
        const Int imageUnit = program.GetUniformSamplerOrImageUnitIndex(static_cast<Uint>(location));
        if (imageUnit < 0 || imageUnit >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) {
            MGLOG_E("ResolveStorageTexelBufferDescriptor: image unit %d out of range for binding %u", imageUnit,
                    binding);
            return false;
        }

        auto& imageBinding = MG_State::pGLContext->GetImageTextureBinding(imageUnit);
        const auto& texture = imageBinding.Texture;
        if (texture == nullptr) {
            MGLOG_E("ResolveStorageTexelBufferDescriptor: image unit %d is unbound for binding %u", imageUnit,
                    binding);
            return false;
        }
        if (texture->GetStorageType() != TextureStorageType::Buffer ||
            texture->GetTarget() != TextureTarget::TextureBuffer) {
            MGLOG_E("ResolveStorageTexelBufferDescriptor: binding %u ('%s') expected a texture buffer on image "
                    "unit %d, got textureId=%u target=%d storage=%d",
                    binding, programObj.samplerNameByBinding[binding].c_str(), imageUnit,
                    texture->GetExternalIndex(), static_cast<Int>(texture->GetTarget()),
                    static_cast<Int>(texture->GetStorageType()));
            return false;
        }

        auto* textureBuffer = static_cast<MG_State::GLState::TextureObjectBuffer*>(texture.get());
        const auto& bufferObject = textureBuffer->GetBufferBindingSlot().GetBoundObject();
        if (bufferObject == nullptr) {
            MGLOG_E("ResolveStorageTexelBufferDescriptor: texture buffer on image unit %d has no GL buffer bound",
                    imageUnit);
            return false;
        }

        // Unlike the sampled texel buffer, the shader MAY write this one, and those writes land
        // in GPU memory behind the frontend's CPU shadow - which is what MapBuffer and
        // GetBufferSubData read. Same two calls, and for the same reason, as the storage-block
        // path above - but only the residency is unconditional. Marking a GL_READ_ONLY binding
        // GPU-written would make the next map or readback wait for a dispatch that could not have
        // changed a byte of it.
        bufferObject->EnsureGpuResidentStorage();
        if (imageBinding.Access != GL_READ_ONLY) {
            bufferObject->MarkGpuWritten();
        }

        BufferSlice slice{};
        if (!m_bufferManager->AcquireResidentSlice(BufferKind::TextureBuffer, bufferObject, slice) ||
            !slice.IsValid()) {
            MGLOG_E("ResolveStorageTexelBufferDescriptor: failed to sync GL buffer %u for texture buffer %u",
                    bufferObject->GetExternalIndex(), texture->GetExternalIndex());
            return false;
        }

        // The format the SHADER declared wins over the one glBindImageTexture named, on the same
        // policy as a storage image: a typed `layout(r32ui) uniform uimageBuffer` must be read as
        // r32ui whatever the texture's own attachment format says. Falling back, in order:
        // reflected format, then the bind format, then the texture's attached format.
        MOBILEGL_ASSERT(binding < programObj.storageImageFormatByBinding.size(),
                        "ResolveStorageTexelBufferDescriptor: binding %u has no reflected format slot", binding);
        const auto internalFormat = textureBuffer->GetFormat();
        const VkFormat resourceFormat = MG_Util::ConvertTextureInternalFormatToVkEnum(internalFormat);
        const VkFormat reflectedFormat = programObj.storageImageFormatByBinding[binding];
        VkFormat vkFormat = reflectedFormat;
        if (vkFormat == VK_FORMAT_UNDEFINED && imageBinding.Format != 0) {
            vkFormat = MG_Util::ConvertTextureInternalFormatToVkEnum(
                MG_Util::ConvertGLEnumToTextureInternalFormat(imageBinding.Format));
        }
        if (vkFormat == VK_FORMAT_UNDEFINED) {
            vkFormat = resourceFormat;
        }
        if (vkFormat == VK_FORMAT_UNDEFINED) {
            MGLOG_E("ResolveStorageTexelBufferDescriptor: unsupported image buffer format (internal=%d bind=0x%x)",
                    static_cast<Int>(internalFormat), imageBinding.Format);
            return false;
        }

        // Sized from the TEXTURE's attached format even though the view may carry a different
        // one. That is not a shortcut: GL requires the shader's format qualifier, the format
        // passed to glBindImageTexture and the texture's own internal format to belong to the
        // same format CLASS (GL 4.6 core, table 8.27), and every member of a class has the same
        // texel size. So the three can disagree on interpretation and never on bytes - which is
        // what the range below has to be a whole multiple of.
        const VkDeviceSize texelSize =
            static_cast<VkDeviceSize>(MG_Util::GetSizedInternalFormatSizeInBytes(internalFormat));
        const VkDeviceSize rangeOffset = static_cast<VkDeviceSize>(textureBuffer->GetBufferRangeOffset());
        const VkDeviceSize rangeSize = static_cast<VkDeviceSize>(textureBuffer->GetBufferRangeSizeInBytes());
        VkDeviceSize viewRange = std::min(rangeSize, slice.size > rangeOffset ? slice.size - rangeOffset : 0);
        if (texelSize > 0) {
            viewRange = (viewRange / texelSize) * texelSize;
        }
        if (viewRange == 0) {
            MGLOG_E("ResolveStorageTexelBufferDescriptor: texture buffer %u has empty view range",
                    texture->GetExternalIndex());
            return false;
        }

        VkBufferViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        viewInfo.buffer = slice.buffer;
        viewInfo.format = vkFormat;
        viewInfo.offset = slice.offset + rangeOffset;
        viewInfo.range = viewRange;

        VkBufferView bufferView = VK_NULL_HANDLE;
        const VkResult result = vkCreateBufferView(m_device, &viewInfo, nullptr, &bufferView);
        if (result != VK_SUCCESS || bufferView == VK_NULL_HANDLE) {
            MGLOG_E("ResolveStorageTexelBufferDescriptor: vkCreateBufferView failed result=%d format=%d range=%zu",
                    result, static_cast<Int>(vkFormat), static_cast<SizeT>(viewRange));
            return false;
        }

        m_frames[frameIndex].texelBufferViews.push_back(bufferView);
        outBufferView = bufferView;
        return true;
    }

    Bool UniformManager::ResolveStorageBufferDescriptor(const MG_State::GLState::ProgramObject& program,
                                                        const ProgramFactory::VkProgramObject& programObj,
                                                        Uint32 binding, Uint32 element,
                                                        VkDescriptorBufferInfo& outBufferInfo) const {
        outBufferInfo = {};
        MOBILEGL_ASSERT(m_bufferManager != nullptr, "ResolveStorageBufferDescriptor: buffer manager is null");
        MOBILEGL_ASSERT(MG_State::pGLContext != nullptr, "ResolveStorageBufferDescriptor: GL context is null");
        MOBILEGL_ASSERT(binding < programObj.storageBlockIndexByBinding.size(),
                        "ResolveStorageBufferDescriptor: binding %u out of range", binding);

        const Int blockIndex = programObj.storageBlockIndexByBinding[binding];
        MOBILEGL_ASSERT(blockIndex >= 0, "ResolveStorageBufferDescriptor: no SSBO block mapped to binding %u",
                        binding);
        // A block instance array declares one block whose elements take consecutive GL binding
        // points from the declared one (GL 4.6 core 7.8), and the reflection collapses the whole
        // array to that one block - so the element index IS the offset from its binding.
        const GLuint frontendBinding =
            GetShaderStorageBlockBinding(program, static_cast<GLuint>(blockIndex)) + element;
        const Uint32 bindingPointCount =
            static_cast<Uint32>(MG_State::pGLContext->GetBufferBindingPointCount(BufferTarget::ShaderStorage));
        MOBILEGL_ASSERT(frontendBinding < bindingPointCount,
                        "ResolveStorageBufferDescriptor: frontend SSBO binding %u out of range for block '%s'",
                        frontendBinding, programObj.storageBlockNameByBinding[binding].c_str());

        auto& bindingPoint = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, frontendBinding);
        const auto& bufferObject = bindingPoint.GetBoundObject();
        if (bufferObject == nullptr) {
            MGLOG_E("ResolveStorageBufferDescriptor: no SSBO bound at frontend binding %u for block '%s'",
                    frontendBinding, programObj.storageBlockNameByBinding[binding].c_str());
            return false;
        }

        // The shader may write this buffer, and those writes land in GPU memory behind the
        // frontend's CPU shadow - which is what MapBuffer and GetBufferSubData read.
        // Host-visible coherent GPU residency makes the shadow BE that memory, so the
        // results are visible without a readback path, exactly as for a capture buffer.
        bufferObject->EnsureGpuResidentStorage();
        // ... and the read that follows has to wait for this draw or dispatch to retire.
        bufferObject->MarkGpuWritten();

        BufferSlice slice{};
        if (!m_bufferManager->AcquireResidentSlice(BufferKind::ShaderStorage, bufferObject, slice) || !slice.IsValid()) {
            MGLOG_E("ResolveStorageBufferDescriptor: failed to sync GL buffer %u for block '%s'",
                    bufferObject->GetExternalIndex(), programObj.storageBlockNameByBinding[binding].c_str());
            return false;
        }

        const auto range = bindingPoint.GetRange();
        const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(bufferObject->GetSize());
        VkDeviceSize rangeStart = static_cast<VkDeviceSize>(std::min(range.start, bufferObject->GetSize()));
        VkDeviceSize rangeEnd = static_cast<VkDeviceSize>(std::min(range.end, bufferObject->GetSize()));
        if (rangeEnd <= rangeStart) {
            rangeStart = 0;
            rangeEnd = bufferSize;
        }
        if (rangeEnd <= rangeStart) {
            MGLOG_E("ResolveStorageBufferDescriptor: empty SSBO range for block '%s'",
                    programObj.storageBlockNameByBinding[binding].c_str());
            return false;
        }

        outBufferInfo.buffer = slice.buffer;
        outBufferInfo.offset = slice.offset + rangeStart;
        outBufferInfo.range = rangeEnd - rangeStart;
        return true;
    }

    Bool UniformManager::ResolveStorageImageDescriptor(VkCommandBuffer commandBuffer,
                                                       const MG_State::GLState::ProgramObject& program,
                                                       const ProgramFactory::VkProgramObject& programObj,
                                                       Uint32 binding, Uint32 element,
                                                       VkDescriptorImageInfo& outImageInfo) const {
        outImageInfo = {};
        MOBILEGL_ASSERT(m_textureManager != nullptr, "ResolveStorageImageDescriptor: texture manager is null");
        MOBILEGL_ASSERT(MG_State::pGLContext != nullptr, "ResolveStorageImageDescriptor: GL context is null");
        MOBILEGL_ASSERT(binding < programObj.samplerUniformLocationByBinding.size(),
                        "ResolveStorageImageDescriptor: binding %u out of range", binding);

        const Int baseLocation = programObj.samplerUniformLocationByBinding[binding];
        if (baseLocation < 0) {
            MGLOG_E("ResolveStorageImageDescriptor: storage image binding %u has no uniform location", binding);
            return false;
        }
        // Per ELEMENT, and this is where an image array differs from a storage-block array: GL
        // gives every element of `uniform image2D g_image[4]` its own glUniform1i-assigned image
        // unit, and the four units need not be consecutive or even ordered (the conformance case
        // uses 0, 2, 4, 6). DoReflection reserves one uniform location per array element, so the
        // element's location is the base plus its index - checked against the array's real
        // extent so a descriptorCount that outran the reflection cannot walk onto the next
        // uniform.
        const Int location = baseLocation + static_cast<Int>(element);
        if (!program.UniformLocationsAliasSameUniform(baseLocation, location)) {
            MGLOG_E("ResolveStorageImageDescriptor: binding %u element %u is past the end of its image array",
                    binding, element);
            return false;
        }
        const Int imageUnit = program.GetUniformSamplerOrImageUnitIndex(static_cast<Uint>(location));
        if (imageUnit < 0 || imageUnit >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) {
            MGLOG_E("ResolveStorageImageDescriptor: image unit %d out of range for binding %u",
                    imageUnit, binding);
            return false;
        }

        auto& imageBinding = MG_State::pGLContext->GetImageTextureBinding(imageUnit);
        if (imageBinding.Texture == nullptr) {
            MGLOG_E("ResolveStorageImageDescriptor: image unit %d is unbound for binding %u", imageUnit, binding);
            return false;
        }

        const Bool ready = m_textureManager->TransitionTextureForStorageImage(commandBuffer, *imageBinding.Texture);
        if (!ready) {
            MGLOG_E("ResolveStorageImageDescriptor: failed to transition textureId=%d for image unit %d",
                    imageBinding.Texture->GetExternalIndex(), imageUnit);
            return false;
        }
        auto* resource = m_textureManager->SyncTextureAndGetDescriptor(*imageBinding.Texture);
        if (resource == nullptr) {
            return false;
        }

        const Uint32 mipLevel = static_cast<Uint32>(std::max<GLint>(0, imageBinding.Level));
        MOBILEGL_ASSERT(binding < programObj.storageImageFormatByBinding.size(),
                        "ResolveStorageImageDescriptor: storage image format binding %u out of range", binding);
        MOBILEGL_ASSERT(binding < programObj.storageImageUsesBindingFormatByBinding.size(),
                        "ResolveStorageImageDescriptor: storage image format policy binding %u out of range",
                        binding);
        const VkFormat reflectedFormat = programObj.storageImageFormatByBinding[binding];
        const Bool useBindingFormat = programObj.storageImageUsesBindingFormatByBinding[binding];
        const VkFormat viewFormat = ResolveStorageImageViewFormat(
            reflectedFormat, imageBinding.Format, resource->format, useBindingFormat);
        if (viewFormat == VK_FORMAT_UNDEFINED) {
            MGLOG_E("ResolveStorageImageDescriptor: unsupported glBindImageTexture format=0x%x "
                    "for binding=%u imageUnit=%d textureId=%d bindingPolicy=%s",
                    imageBinding.Format, binding, imageUnit, imageBinding.Texture->GetExternalIndex(),
                    useBindingFormat ? "true" : "false");
            return false;
        }
        const VkImageView view = m_textureManager->GetOrCreateStorageImageView(
            *imageBinding.Texture, mipLevel, viewFormat, imageBinding.Layered != GL_FALSE, imageBinding.Layer);
        if (view == VK_NULL_HANDLE) {
            MGLOG_E("ResolveStorageImageDescriptor: failed to resolve storage view textureId=%d mip=%u "
                    "bindingFormat=0x%x imageFormat=%d reflectedFormat=%d selectedFormat=%d bindingPolicy=%s",
                    imageBinding.Texture->GetExternalIndex(), mipLevel, imageBinding.Format,
                    static_cast<Int>(resource->format), static_cast<Int>(reflectedFormat),
                    static_cast<Int>(viewFormat),
                    useBindingFormat ? "true" : "false");
            return false;
        }
        outImageInfo.sampler = VK_NULL_HANDLE;
        outImageInfo.imageView = view;
        outImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        return outImageInfo.imageView != VK_NULL_HANDLE;
    }

    SharedPtr<MG_State::GLState::ITextureObject> UniformManager::GetFallbackTexture(TextureTarget target) const {
        // The fallback is a single-sampled 2D image, so it can only stand in for a sampler that
        // would accept one. A multisample sampler in particular cannot: its descriptor demands a
        // multisample view, and handing it this one is invalid Vulkan, not a degraded picture.
        // Report that there is no fallback and let the caller decline the draw - aborting the
        // process over an unbound sampler is never the right answer.
        if (target != TextureTarget::Texture2D && target != TextureTarget::TextureRectangle) {
            MGLOG_E("UniformManager::GetFallbackTexture: no fallback exists for target=%d",
                    static_cast<Int>(target));
            return nullptr;
        }

        if (m_fallbackTexture2D == nullptr) {
            auto fallbackTexture = MakeShared<MG_State::GLState::TextureObject2D>(kFallbackTexture2DExternalIndex);
            fallbackTexture->SetInternalFormat(TextureInternalFormat::RGBA8);
            fallbackTexture->AllocateStorage(TextureUploadTarget::Texture2D, 0,
                                             {.texelSize = {1, 1, 1}, .byteSize = 4});
            // (0, 0, 0, 1): what GL reads from a texture that is not complete, and the only
            // sensible answer for a sampler with nothing bound.
            static Uint8 kOpaqueBlackTexel[4] = {0, 0, 0, 255};
            fallbackTexture->UpdateMipmapSubData(TextureUploadTarget::Texture2D, 0,
                                                 {kOpaqueBlackTexel, sizeof(kOpaqueBlackTexel)});
            fallbackTexture->MarkStorageDirty(TextureUploadTarget::Texture2D, 0, true);
            m_fallbackTexture2D = fallbackTexture;
        }

        return m_fallbackTexture2D;
    }

    Bool UniformManager::ResolveSampledBinding(const MG_State::GLState::ProgramObject& program,
                                               const ProgramFactory::VkProgramObject& programObj,
                                               Uint32 binding, Uint32 element,
                                               MG_State::GLState::ITextureObject*& outTexture,
                                               const MG_State::GLState::SamplerObject*& outSampler) const {
        // Open-coded ResolveSamplerTextureRaw so the unit is resolved once for both the
        // texture and the sampler override - this runs per binding per full-path draw,
        // and program-alternating draw streams take the full path on every draw.
        MOBILEGL_ASSERT(MG_State::pGLContext != nullptr, "ResolveSampledBinding: GL context is null");
        MOBILEGL_ASSERT(binding < programObj.samplerUniformLocationByBinding.size(),
                        "ResolveSampledBinding: sampler location binding %u out of range", binding);
        MOBILEGL_ASSERT(binding < programObj.samplerTextureTargetByBinding.size(),
                        "ResolveSampledBinding: sampler target binding %u out of range", binding);
        const Int location =
            ResolveDescriptorElementLocation(program, programObj.samplerUniformLocationByBinding[binding], element);
        if (location < 0 && element > 0) {
            return false;
        }
        const Int unit = ResolveSamplerUnitIndex(program, location, binding);
        auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
        const TextureTarget preferredTarget = programObj.samplerTextureTargetByBinding[binding];
        MG_State::GLState::ITextureObject* texture =
            textureUnit.GetBindingSlot(preferredTarget).GetBoundObject().get();
        // Undefined default texture (name 0, no image) resolves as "unbound", exactly
        // like ResolveSamplerTextureRaw reports it.
        if (MG_State::GLState::IsUndefinedDefaultTexture(texture)) {
            texture = nullptr;
        }
        if (texture == nullptr) {
            // ResolveSamplerDescriptor will substitute the fallback texture for this binding;
            // include it in the sampled set so the pre-render-pass sync/transition pass covers
            // its first use instead of leaving that work to happen inside an active pass.
            if (preferredTarget != TextureTarget::Texture2D &&
                preferredTarget != TextureTarget::TextureRectangle) {
                return false;
            }
            texture = GetFallbackTexture(preferredTarget).get();
        }
        const auto& samplerOverride = textureUnit.GetSamplerObject();
        outTexture = texture;
        outSampler = samplerOverride ? samplerOverride.get()
                                     : (texture != nullptr ? texture->GetSamplerObject().get() : nullptr);
        return true;
    }

    Bool UniformManager::CollectSampledTextures(const MG_State::GLState::ProgramObject& program,
                                                          const ProgramFactory::VkProgramObject& programObj,
                                                          Vector<MG_State::GLState::ITextureObject*>& outTextures,
                                                          Vector<SampledBindingRecord>* outBindingRecords) {
        outTextures.clear();
        if (outBindingRecords != nullptr) {
            outBindingRecords->clear();
        }
        // Nothing to prepare for a program the bind path is going to refuse; its declined
        // binding has no uniform location to resolve a texture through either.
        if (programObj.declinedDescriptors) {
            return true;
        }

        const Uint32 bindingCount =
            std::min<Uint32>(m_maxBindings, static_cast<Uint32>(programObj.bindingKinds.size()));
        for (Uint32 binding = 0; binding < bindingCount; ++binding) {
            if (programObj.bindingKinds[binding] != ProgramFactory::DescriptorBindingKind::CombinedImageSampler) {
                continue;
            }

            // Every ELEMENT of a sampler array reaches its own texture through its own unit,
            // so every element has to be in the sampled set: this walk is what gets those
            // textures synced and transitioned to a sampled layout BEFORE the render pass
            // opens, and a missed element would first be touched by the descriptor resolve
            // inside an active pass.
            const Uint32 descriptorCount = BindingDescriptorCount(programObj, binding);
            for (Uint32 element = 0; element < descriptorCount; ++element) {
                MG_State::GLState::ITextureObject* texture = nullptr;
                const MG_State::GLState::SamplerObject* sampler = nullptr;
                if (!ResolveSampledBinding(program, programObj, binding, element, texture, sampler)) {
                    continue;
                }
                if (outBindingRecords != nullptr) {
                    outBindingRecords->push_back({texture != nullptr ? texture->GetLifetimeId() : 0,
                                                  sampler != nullptr ? sampler->GetLifetimeId() : 0});
                }

                auto found = std::find(outTextures.begin(), outTextures.end(), texture);
                if (found == outTextures.end()) {
                    outTextures.push_back(texture);
                }
            }
        }
        return true;
    }

    Bool UniformManager::SampledBindingsUnchanged(const MG_State::GLState::ProgramObject& program,
                                                  const ProgramFactory::VkProgramObject& programObj,
                                                  const Vector<SampledBindingRecord>& previousRecords) const {
        // A declined program takes the full path every time and is refused there.
        if (programObj.declinedDescriptors) {
            return false;
        }
        SizeT recordIndex = 0;
        // Iterate only the bindings this program declares (ascending), exactly like
        // BindProgramUniformBuffers: this runs per draw whenever the texture bind
        // generation moved, and walking all m_maxBindings slots to find the 1-8 real
        // ones dominated it.
        for (const Uint32 binding : programObj.activeBindings) {
            if (binding >= m_maxBindings) {
                break; // ascending, so nothing past the cap can follow
            }
            if (programObj.bindingKinds[binding] != ProgramFactory::DescriptorBindingKind::CombinedImageSampler) {
                continue;
            }
            // Element-for-element, in the same order CollectSampledTextures recorded them -
            // the two walks have to visit the identical descriptor sequence or the positional
            // comparison below drifts.
            const Uint32 descriptorCount = BindingDescriptorCount(programObj, binding);
            for (Uint32 element = 0; element < descriptorCount; ++element) {
                MG_State::GLState::ITextureObject* texture = nullptr;
                const MG_State::GLState::SamplerObject* sampler = nullptr;
                if (!ResolveSampledBinding(program, programObj, binding, element, texture, sampler)) {
                    continue;
                }
                if (recordIndex >= previousRecords.size()) {
                    return false;
                }
                const SampledBindingRecord& record = previousRecords[recordIndex++];
                if (record.textureLifetimeId != (texture != nullptr ? texture->GetLifetimeId() : 0) ||
                    record.samplerLifetimeId != (sampler != nullptr ? sampler->GetLifetimeId() : 0)) {
                    return false;
                }
            }
        }
        return recordIndex == previousRecords.size();
    }

    Bool UniformManager::CollectStorageImageTextures(
        const MG_State::GLState::ProgramObject& program,
        const ProgramFactory::VkProgramObject& programObj,
        Vector<MG_State::GLState::ITextureObject*>& outTextures) const {
        outTextures.clear();
        MOBILEGL_ASSERT(MG_State::pGLContext != nullptr,
                        "CollectStorageImageTextures: GL context is null");
        // Same as the sampled walk: a declined program is refused at bind time, and its declined
        // binding has no uniform location to reach an image unit through.
        if (programObj.declinedDescriptors) {
            return true;
        }

        const Uint32 bindingCount =
            std::min<Uint32>(m_maxBindings, static_cast<Uint32>(programObj.bindingKinds.size()));
        for (Uint32 binding = 0; binding < bindingCount; ++binding) {
            if (programObj.bindingKinds[binding] != ProgramFactory::DescriptorBindingKind::StorageImage) {
                continue;
            }
            if (binding >= programObj.samplerUniformLocationByBinding.size()) {
                MGLOG_E("CollectStorageImageTextures: binding %u has no uniform-location mapping", binding);
                return false;
            }

            const Int baseLocation = programObj.samplerUniformLocationByBinding[binding];
            if (baseLocation < 0) {
                MGLOG_E("CollectStorageImageTextures: binding %u has no image uniform location", binding);
                return false;
            }
            // Per ELEMENT, for the same reason the sampled walk above is: an image ARRAY is one
            // binding whose elements each carry their own image unit, so each reaches its own
            // texture. This walk is what puts those textures into the pre-pass sync and layout
            // transition; collecting only element 0 left elements 1..N to be first touched by
            // the descriptor resolve, which happens with a render pass already open.
            const Uint32 descriptorCount = BindingDescriptorCount(programObj, binding);
            for (Uint32 element = 0; element < descriptorCount; ++element) {
                const Int location = ResolveDescriptorElementLocation(program, baseLocation, element);
                if (location < 0) {
                    MGLOG_E("CollectStorageImageTextures: binding %u element %u is past the end of its image array",
                            binding, element);
                    return false;
                }
                const Int imageUnit = program.GetUniformSamplerOrImageUnitIndex(static_cast<Uint>(location));
                if (imageUnit < 0 || imageUnit >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) {
                    MGLOG_E("CollectStorageImageTextures: image unit %d is invalid for binding %u element %u",
                            imageUnit, binding, element);
                    return false;
                }

                auto* texture = MG_State::pGLContext->GetImageTextureBinding(imageUnit).Texture.get();
                if (texture == nullptr) {
                    MGLOG_E("CollectStorageImageTextures: image unit %d is unbound for binding %u element %u",
                            imageUnit, binding, element);
                    return false;
                }
                if (std::find(outTextures.begin(), outTextures.end(), texture) == outTextures.end()) {
                    outTextures.push_back(texture);
                }
            }
        }
        return true;
    }

    Bool UniformManager::ResolveUniformBufferPayload(const MG_State::GLState::ProgramObject& program,
                                                     const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                                     Uint32 arrayElement, UboBindResult& out) const {
        const void* outData = nullptr;
        VkDeviceSize outSize = 0;

        MOBILEGL_ASSERT(MG_State::pGLContext != nullptr, "ResolveUniformBufferPayload: GL context is null");
        MOBILEGL_ASSERT(binding < programObj.bindingKinds.size(),
                        "ResolveUniformBufferPayload: binding %u out of range", binding);
        MOBILEGL_ASSERT(programObj.bindingKinds[binding] == ProgramFactory::DescriptorBindingKind::UniformBufferDynamic,
                        "ResolveUniformBufferPayload: binding %u is not a uniform buffer descriptor", binding);

        if (programObj.globalUboBinding == static_cast<Int>(binding)) {
            outData = program.GetUBOData();
            outSize = static_cast<VkDeviceSize>(program.GetUBOSize());
            static const Array<Uint8, 16> emptyGlobalUbo{};
            if (outData == nullptr || outSize == 0) {
                outData = emptyGlobalUbo.data();
                outSize = static_cast<VkDeviceSize>(emptyGlobalUbo.size());
            }
            // The global UBO is CPU uniform data, not an app buffer -> always UploadTransient.
            out.payload = outData;
            out.payloadSize = outSize;
            return outData != nullptr && outSize > 0;
        }

        MOBILEGL_ASSERT(binding < programObj.uniformBlockIndexByBinding.size(),
                        "ResolveUniformBufferPayload: UBO mapping binding %u out of range", binding);
        Int blockIndex = programObj.uniformBlockIndexByBinding[binding];
        if (arrayElement > 0) {
            const auto arrayIt = programObj.arrayedUniformBlockIndicesByBinding.find(binding);
            const Bool elementValid = arrayIt != programObj.arrayedUniformBlockIndicesByBinding.end() &&
                                      arrayElement < arrayIt->second.size();
            MOBILEGL_ASSERT(elementValid,
                            "ResolveUniformBufferPayload: UBO binding %u has no array element %u", binding,
                            arrayElement);
            if (!elementValid) {
                return false;
            }
            blockIndex = arrayIt->second[arrayElement];
        }
        MOBILEGL_ASSERT(blockIndex >= 0,
                        "ResolveUniformBufferPayload: no uniform block mapped to descriptor binding %u", binding);

        const Uint32 activeUniformBlockCount = static_cast<Uint32>(program.GetActiveUniformBlocksCount());
        MOBILEGL_ASSERT(static_cast<Uint32>(blockIndex) < activeUniformBlockCount,
                        "ResolveUniformBufferPayload: uniform block index %d out of range (count=%u)", blockIndex,
                        activeUniformBlockCount);

        const Uint32 frontendBinding = program.GetUniformBlockBinding(static_cast<Uint32>(blockIndex));
        const Uint32 uniformBindingPointCount =
            static_cast<Uint32>(MG_State::pGLContext->GetBufferBindingPointCount(BufferTarget::Uniform));
        MOBILEGL_ASSERT(frontendBinding < uniformBindingPointCount,
                        "ResolveUniformBufferPayload: frontend UBO binding %u out of range for block '%s'",
                        frontendBinding, program.GetUniformBlockName(static_cast<Uint32>(blockIndex)).c_str());

        auto& bindingPoint = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::Uniform, frontendBinding);
        const auto& bufferObject = bindingPoint.GetBoundObject();
        MOBILEGL_ASSERT(bufferObject != nullptr,
                        "ResolveUniformBufferPayload: no UBO bound at frontend binding %u for block '%s'",
                        frontendBinding, program.GetUniformBlockName(static_cast<Uint32>(blockIndex)).c_str());
        bufferObject->SyncPersistentMappedRange();

        MOBILEGL_ASSERT(bufferObject->MappedData() != nullptr && bufferObject->GetSize() != 0,
                        "ResolveUniformBufferPayload: bound UBO data is empty for block '%s'",
                        program.GetUniformBlockName(static_cast<Uint32>(blockIndex)).c_str());

        const auto range = bindingPoint.GetRange();
        const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(bufferObject->GetSize());
        const VkDeviceSize rangeStart = static_cast<VkDeviceSize>(range.start);
        MOBILEGL_ASSERT(rangeStart < bufferSize,
                        "ResolveUniformBufferPayload: UBO range start %zu exceeds buffer size %zu for block '%s'",
                        static_cast<SizeT>(rangeStart), static_cast<SizeT>(bufferSize),
                        program.GetUniformBlockName(static_cast<Uint32>(blockIndex)).c_str());

        VkDeviceSize rangeEnd = static_cast<VkDeviceSize>(range.end);
        if (rangeEnd > bufferSize) {
            rangeEnd = bufferSize;
        }
        MOBILEGL_ASSERT(rangeEnd > rangeStart,
                        "ResolveUniformBufferPayload: invalid UBO range [%zu, %zu) for block '%s'",
                        static_cast<SizeT>(rangeStart), static_cast<SizeT>(rangeEnd),
                        program.GetUniformBlockName(static_cast<Uint32>(blockIndex)).c_str());

        const VkDeviceSize blockSize = static_cast<VkDeviceSize>(program.GetUBOSizeAt(static_cast<Uint32>(blockIndex)));
        MOBILEGL_ASSERT(blockSize > 0,
                        "ResolveUniformBufferPayload: reflected UBO size is zero for block '%s'",
                        program.GetUniformBlockName(static_cast<Uint32>(blockIndex)).c_str());

        const VkDeviceSize available = rangeEnd - rangeStart;
        outSize = blockSize;
        outData = bufferObject->MappedData() + static_cast<SizeT>(rangeStart);
        if (available < blockSize) {
            static thread_local Vector<Uint8> paddedUbo;
            paddedUbo.assign(static_cast<SizeT>(blockSize), 0);
            Memcpy(paddedUbo.data(), outData, static_cast<SizeT>(available));
            outData = paddedUbo.data();
        }
        out.payload = outData;
        out.payloadSize = outSize;

        // Zero-copy direct bind: for a persistent-mapped coherent app buffer whose full reflected
        // block fits within the aligned bound range, point the descriptor straight at the app's
        // resident VkBuffer (the same buffer the GLES backend binds) with the block range as the
        // dynamic offset - no per-draw copy into a transient ring. Any gate failing keeps the
        // UploadTransient payload above. AcquireResidentSlice does the persistent busy-tracking and
        // (for the persistent case) hits a zero-work fast path returning the whole-buffer slice.
        if (bufferObject->IsBackendPersistentMapped() && available >= blockSize &&
            (rangeStart % m_minDynamicOffsetAlignment) == 0) {
            BufferSlice slice{};
            if (m_bufferManager->AcquireResidentSlice(BufferKind::Uniform, bufferObject, slice) &&
                slice.IsValid() && slice.offset == 0 && slice.size >= rangeStart + blockSize) {
                out.directBindable = true;
                out.buffer = slice.buffer;
                out.range = blockSize;
                out.dynamicOffset = rangeStart;
            }
        }
        return true;
    }

    Bool UniformManager::CreateDescriptorPool(Uint32 maxSets, VkDescriptorPool& outPool) const {
        outPool = VK_NULL_HANDLE;
        if (m_device == VK_NULL_HANDLE || maxSets == 0 || m_maxBindings == 0) {
            return false;
        }

        // Sized from what a real program declares, not from the 256-binding cap. A GL program's
        // single descriptor set holds the bindings shader reflection found - typically 2 to 8 - so
        // scaling by m_maxBindings declared 5 x 64 x 256 = 81,920 descriptors per pool and 245,760
        // across the three frames in flight, which drivers that reserve backing store proportional
        // to the declared count pay for at init. An outlier program is absorbed by the existing
        // VK_ERROR_OUT_OF_POOL_MEMORY -> GrowFrameDescriptorPool path: pool sizes are aggregate
        // budgets rather than per-set limits, and vkAllocateDescriptorSets is spec-required to
        // report that error rather than fail hard.
        static constexpr Uint32 kEstimatedBindingsPerSet = 8;
        const Uint64 descriptorCount64 =
            static_cast<Uint64>(maxSets) * static_cast<Uint64>(std::min(m_maxBindings, kEstimatedBindingsPerSet));
        if (descriptorCount64 > static_cast<Uint64>(std::numeric_limits<Uint32>::max())) {
            MGLOG_E("UniformDescriptorBinder::CreateDescriptorPool failed: descriptorCount overflow");
            return false;
        }

        const Uint32 descriptorCount = static_cast<Uint32>(descriptorCount64);
        VkDescriptorPoolSize poolSizes[6]{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        poolSizes[0].descriptorCount = descriptorCount;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = descriptorCount;
        poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        poolSizes[2].descriptorCount = descriptorCount;
        poolSizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[3].descriptorCount = descriptorCount;
        poolSizes[4].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[4].descriptorCount = descriptorCount;
        poolSizes[5].type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        poolSizes[5].descriptorCount = descriptorCount;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        // FREE_DESCRIPTOR_SET_BIT lets a destroyed layout's cached sets be freed back
        // (OnDescriptorSetLayoutDestroyed) so program churn recycles pool capacity.
        // The cost is on set allocation only, which happens when a layout's per-frame
        // cache grows - never on the per-draw reuse path.
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = maxSets;
        poolInfo.poolSizeCount = static_cast<Uint32>(std::size(poolSizes));
        poolInfo.pPoolSizes = poolSizes;

        const VkResult result = vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &outPool);
        if (result != VK_SUCCESS) {
            MGLOG_E("UniformDescriptorBinder::CreateDescriptorPool failed: vkCreateDescriptorPool returned %d",
                    result);
            return false;
        }
        return true;
    }

    Bool UniformManager::GrowFrameDescriptorPool(FrameResources& frame, Uint32 frameIndex) {
        if (frame.descriptorPools.empty()) {
            return false;
        }

        const auto& currentBucket = frame.descriptorPools[frame.activeDescriptorPoolIndex];
        const Uint32 currentMaxSets = std::max<Uint32>(1, currentBucket.maxSets);
        const Uint32 grownMaxSets = currentMaxSets <= (std::numeric_limits<Uint32>::max() / 2) ? (currentMaxSets * 2)
                                                                                                 : currentMaxSets;

        VkDescriptorPool grownPool = VK_NULL_HANDLE;
        if (!CreateDescriptorPool(grownMaxSets, grownPool)) {
            MGLOG_E("UniformDescriptorBinder::GrowFrameDescriptorPool failed: cannot create grown pool (%u -> %u sets)",
                    currentMaxSets, grownMaxSets);
            return false;
        }

        frame.descriptorPools.push_back({grownPool, grownMaxSets, 0});
        frame.activeDescriptorPoolIndex = static_cast<Uint32>(frame.descriptorPools.size() - 1);
        MGLOG_D(
            "UniformDescriptorBinder: frame %u descriptor pool exhausted, grew pool (%u -> %u sets), poolCount=%zu",
            frameIndex, currentMaxSets, grownMaxSets, frame.descriptorPools.size());
        return true;
    }

    VkResult UniformManager::AllocateDescriptorSetsFromActivePool(Uint32 frameIndex, const ProgramFactory::VkProgramObject& programObj, VkDescriptorSet& outDescriptorSet) {
        auto& frame = m_frames[frameIndex];
        if (frame.activeDescriptorPoolIndex >= frame.descriptorPools.size()) {
            frame.activeDescriptorPoolIndex = 0;
        }
        if (frame.descriptorPools[frame.activeDescriptorPoolIndex].allocatedSets >=
            frame.descriptorPools[frame.activeDescriptorPoolIndex].maxSets) {
            const auto availableBucket = std::find_if(
                frame.descriptorPools.begin(), frame.descriptorPools.end(),
                [](const DescriptorPoolBucket& candidate) { return candidate.allocatedSets < candidate.maxSets; });
            if (availableBucket == frame.descriptorPools.end()) {
                outDescriptorSet = VK_NULL_HANDLE;
                return VK_ERROR_OUT_OF_POOL_MEMORY;
            }
            frame.activeDescriptorPoolIndex =
                static_cast<Uint32>(std::distance(frame.descriptorPools.begin(), availableBucket));
        }
        auto& bucket = frame.descriptorPools[frame.activeDescriptorPoolIndex];
        if (bucket.allocatedSets >= bucket.maxSets) {
            outDescriptorSet = VK_NULL_HANDLE;
            return VK_ERROR_OUT_OF_POOL_MEMORY;
        }
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &programObj.descriptorSetLayout;

        allocInfo.descriptorPool = bucket.handle;
        VkResult result = vkAllocateDescriptorSets(m_device, &allocInfo, &outDescriptorSet);
        if (result == VK_SUCCESS) {
            ++bucket.allocatedSets;
        }
        return result;
    }

    VkResult UniformManager::AcquireDescriptorSet(Uint32 frameIndex,
                                                  const ProgramFactory::VkProgramObject& programObj,
                                                  VkDescriptorSet& outDescriptorSet) {
        auto& frame = m_frames[frameIndex];
        auto& cache = frame.descriptorSetCacheByLayout[programObj.descriptorSetLayout];
        if (cache.cursor < cache.sets.size()) {
            outDescriptorSet = cache.sets[cache.cursor++].set;
        } else {
            VkResult allocResult = AllocateDescriptorSetsFromActivePool(frameIndex, programObj, outDescriptorSet);
            if (allocResult == VK_ERROR_OUT_OF_POOL_MEMORY || allocResult == VK_ERROR_FRAGMENTED_POOL) {
                if (!GrowFrameDescriptorPool(frame, frameIndex)) {
                    MGLOG_E("UniformDescriptorBinder::AcquireDescriptorSet failed: descriptor pool growth failed");
                    return allocResult;
                }
                allocResult = AllocateDescriptorSetsFromActivePool(frameIndex, programObj, outDescriptorSet);
            }
            if (allocResult != VK_SUCCESS || outDescriptorSet == VK_NULL_HANDLE) {
                return allocResult;
            }

            // The successful allocation came from the bucket the alloc helper left
            // active; record it so a layout-destroyed purge can free the set back.
            cache.sets.push_back({outDescriptorSet, frame.descriptorPools[frame.activeDescriptorPoolIndex].handle});
            ++cache.cursor;
            MGLOG_D("UniformDescriptorBinder: cached descriptor set count for frame=%u grew to %zu", frameIndex,
                    cache.sets.size());
        }

        ++frame.allocatedSetsThisFrame;
        frame.peakAllocatedSetsThisFrame =
            std::max(frame.peakAllocatedSetsThisFrame, frame.allocatedSetsThisFrame);
        return VK_SUCCESS;
    }

    Bool UniformManager::ResolveDynamicUboDescriptor(const MG_State::GLState::ProgramObject& program,
                                                     const ProgramFactory::VkProgramObject& programObj,
                                                     Uint32 binding, Uint32 arrayElement, Uint32 frameIndex,
                                                     VkBuffer& outBuffer, VkDeviceSize& outRange,
                                                     Uint32& outDynamicOffset) {
        UboBindResult ubo{};
        const Bool hasPayload = ResolveUniformBufferPayload(program, programObj, binding, arrayElement, ubo);
        MOBILEGL_ASSERT(hasPayload && (ubo.directBindable || (ubo.payload != nullptr && ubo.payloadSize > 0)),
                        "UniformDescriptorBinder::ResolveDynamicUboDescriptor failed: missing UBO payload on binding %u element %u",
                        binding, arrayElement);
        if (ubo.directBindable) {
            // Zero-copy: bind the app's resident VkBuffer directly, no per-draw memcpy.
            outBuffer = ubo.buffer;
            outRange = ubo.range;
            outDynamicOffset = static_cast<Uint32>(ubo.dynamicOffset);
            return true;
        }
        // Global-UBO slice reuse (see GlobalUboSliceMemo): unchanged
        // uniform bytes re-use the slice already uploaded this frame.
        const Bool isGlobalUbo = programObj.globalUboBinding == static_cast<Int>(binding) && arrayElement == 0;
        const Uint64 uboFrameSerial = m_bufferManager->GetFrameSerial();
        const Uint64 uboProgramLifetimeId = program.GetLifetimeId();
        const Uint32 uboContentVersion = program.GetUBOContentVersion();
        if (isGlobalUbo) {
            for (const auto& memo : m_globalUboMemo) {
                if (memo.buffer != VK_NULL_HANDLE && memo.programLifetimeId == uboProgramLifetimeId &&
                    memo.frameSerial == uboFrameSerial && memo.uboContentVersion == uboContentVersion &&
                    memo.range == static_cast<VkDeviceSize>(ubo.payloadSize)) {
                    outBuffer = memo.buffer;
                    outRange = memo.range;
                    outDynamicOffset = static_cast<Uint32>(memo.offset);
                    return true;
                }
            }
        }
        BufferSlice slice{};
        if (!m_bufferManager->UploadTransient(BufferKind::Uniform, frameIndex, ubo.payload, ubo.payloadSize,
                                              m_minDynamicOffsetAlignment, slice)) {
            MOBILEGL_ASSERT(false,
                            "UniformDescriptorBinder::ResolveDynamicUboDescriptor failed: UBO upload failed on binding %u element %u",
                            binding, arrayElement);
            return false;
        }
        outBuffer = slice.buffer;
        outRange = ubo.payloadSize;
        outDynamicOffset = static_cast<Uint32>(slice.offset);
        if (isGlobalUbo) {
            m_globalUboMemo[m_globalUboMemoNext] =
                GlobalUboSliceMemo{uboProgramLifetimeId, uboFrameSerial, uboContentVersion,
                                   slice.buffer, slice.offset, static_cast<VkDeviceSize>(ubo.payloadSize)};
            m_globalUboMemoNext = (m_globalUboMemoNext + 1) % kGlobalUboMemoSize;
        }
        return true;
    }

    void UniformManager::BindDescriptorSetDeduped(VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint,
                                                  VkPipelineLayout pipelineLayout, VkDescriptorSet descriptorSet,
                                                  const Vector<Uint32>& dynamicOffsets) {
        // Skip the driver call when this exact binding is already live on the
        // command buffer (see the bind-dedup shadow in the header).
        const Uint32 offsetCount = static_cast<Uint32>(dynamicOffsets.size());
        Bool identicalBind = m_lastBindValid && m_lastBindSet == descriptorSet &&
                             m_lastBindLayout == pipelineLayout && m_lastBindPoint == bindPoint &&
                             m_lastBindOffsetCount == offsetCount && offsetCount <= kMaxShadowedDynamicOffsets;
        if (identicalBind) {
            for (Uint32 i = 0; i < offsetCount; ++i) {
                if (m_lastBindOffsets[i] != dynamicOffsets[i]) {
                    identicalBind = false;
                    break;
                }
            }
        }
        if (!identicalBind) {
            vkCmdBindDescriptorSets(commandBuffer, bindPoint, pipelineLayout, 0, 1,
                                    &descriptorSet, offsetCount, dynamicOffsets.data());
            if (offsetCount <= kMaxShadowedDynamicOffsets) {
                m_lastBindValid = true;
                m_lastBindSet = descriptorSet;
                m_lastBindLayout = pipelineLayout;
                m_lastBindPoint = bindPoint;
                m_lastBindOffsetCount = offsetCount;
                std::copy_n(dynamicOffsets.data(), offsetCount, m_lastBindOffsets);
            } else {
                m_lastBindValid = false;
            }
        }
    }

    Bool UniformManager::BindProgramUniformBuffers(VkCommandBuffer commandBuffer,
                                                             const MG_State::GLState::ProgramObject& program,
                                                             const ProgramFactory::VkProgramObject& programObj,
                                                             Uint32 frameIndex,
                                                             VkPipelineBindPoint bindPoint,
                                                             const SamplerBindingOverride* samplerBindingOverride,
                                                             Bool samplerDescriptorsUnchangedHint) {
        // This program has a descriptor MobileGL could not resolve (see
        // VkProgramObject::declinedDescriptors). Refusing here is the whole of the decline: the
        // binding is still declared in the layout, so the pipeline is consistent with the shader
        // and creating it is safe - what must not happen is the draw, because the descriptor
        // behind that binding can never be written. The draw setup skips the draw on a false
        // return. ReflectLayout already said why, once, at MGLOG_I.
        if (programObj.declinedDescriptors) {
            MGLOG_D("UniformDescriptorBinder::BindProgramUniformBuffers: refusing a program whose descriptor layout "
                    "was declined at reflection");
            return false;
        }
        auto& frame = m_frames[frameIndex];
        if (frame.descriptorPools.empty()) {
            MGLOG_E("UniformDescriptorBinder::BindProgramUniformBuffers failed: frame descriptor pools are invalid");
            return false;
        }
        if (frame.activeDescriptorPoolIndex >= frame.descriptorPools.size()) {
            frame.activeDescriptorPoolIndex = 0;
        }

        // Dynamic-offset-only rebind (see FastRebindMemo in the header): the last
        // cacheable walk of this exact program selected a set whose contents are
        // provably still what this walk would write - the hint covers every
        // sampler binding, and an unchanged (buffer, range) for the single
        // dynamic UBO covers the rest - except the dynamic offset, which rebinding
        // the SAME set delivers without any descriptor write.
        const Bool cacheable = (samplerBindingOverride == nullptr);
        if (cacheable && samplerDescriptorsUnchangedHint && m_fastRebindMemo.valid &&
            m_fastRebindMemo.frameIndex == frameIndex &&
            m_fastRebindMemo.programLifetimeId == program.GetLifetimeId() &&
            m_fastRebindMemo.programHash == programObj.hash) {
            VkBuffer uboBuffer = VK_NULL_HANDLE;
            VkDeviceSize uboRange = 0;
            Uint32 uboDynamicOffset = 0;
            if (ResolveDynamicUboDescriptor(program, programObj, m_fastRebindMemo.uboBinding, 0, frameIndex,
                                            uboBuffer, uboRange, uboDynamicOffset) &&
                uboBuffer == m_fastRebindMemo.uboBuffer && uboRange == m_fastRebindMemo.uboRange) {
                auto& fastOffsets = m_dynamicOffsetsScratch;
                fastOffsets.clear();
                fastOffsets.push_back(uboDynamicOffset);
                BindDescriptorSetDeduped(commandBuffer, bindPoint, programObj.pipelineLayout,
                                         m_fastRebindMemo.set, fastOffsets);
                return true;
            }
            // Any mismatch (arena wrap or growth, direct-bind retarget, upload
            // failure) falls through to the full walk, which re-records the memo.
        }

        // The descriptor set is chosen AFTER the writes are built (below), so a draw
        // whose resolved descriptor content matches the previous draw can reuse that
        // set and skip both AcquireDescriptorSet and vkUpdateDescriptorSets.
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

        MOBILEGL_ASSERT(m_textureManager != nullptr, "BindProgramUniformBuffers: texture manager is null");
        MOBILEGL_ASSERT(m_samplerManager != nullptr, "BindProgramUniformBuffers: sampler manager is null");
        MOBILEGL_ASSERT(m_bufferManager != nullptr, "BindProgramUniformBuffers: buffer manager is null");

        auto& writes = m_writesScratch;
        auto& bufferInfos = m_bufferInfosScratch;
        auto& imageInfos = m_imageInfosScratch;
        auto& texelBufferViews = m_texelBufferViewsScratch;
        auto& dynamicOffsets = m_dynamicOffsetsScratch;
        writes.clear();
        bufferInfos.clear();
        imageInfos.clear();
        texelBufferViews.clear();
        dynamicOffsets.clear();
        // Arrayed UBO bindings contribute extra buffer infos and dynamic offsets; reserve for
        // the worst case so the pBufferInfo pointers taken below never dangle on reallocation.
        // Arrayed SSBO bindings contribute extra buffer infos too (but no dynamic offsets).
        Uint32 uboArrayExtra = 0;
        for (const auto& arrayEntry : programObj.arrayedUniformBlockIndicesByBinding) {
            uboArrayExtra += static_cast<Uint32>(arrayEntry.second.size()) - 1u;
        }
        // Surplus descriptors over "one per binding", summed across EVERY arrayed binding
        // whatever its kind - storage blocks, image arrays and sampler arrays all land here.
        // One number for all of them because each container below is bounded by the same total.
        Uint32 arrayDescriptorExtra = 0;
        for (const Uint16 count : programObj.bindingDescriptorCounts) {
            if (count > 1) arrayDescriptorExtra += static_cast<Uint32>(count) - 1u;
        }
        writes.reserve(m_maxBindings);
        bufferInfos.reserve(m_maxBindings + uboArrayExtra + arrayDescriptorExtra);
        // Every binding pushes at most descriptorCount image infos, so bindings + surplus is the
        // worst case. Reserving only m_maxBindings here was exact while every binding pushed
        // exactly one - and reallocates under an image or sampler array, dangling every
        // pImageInfo already recorded in `writes` before vkUpdateDescriptorSets reads them. That
        // is reachable wherever m_maxBindings is small (it clamps to ~16 on Adreno and Mali),
        // which is exactly where a 7-element CTS sampler array does not fit the slack.
        imageInfos.reserve(m_maxBindings + arrayDescriptorExtra);
        // Exact, and safe only because it is: BOTH texel kinds (samplerBuffer and imageBuffer)
        // refuse descriptor arrays at program creation, so each contributes at most one view and
        // the total cannot exceed the binding count. The branches below take the address of
        // back(), so making a texel kind array-capable without also giving this the surplus
        // imageInfos gets would dangle every pTexelBufferView already recorded in `writes`.
        texelBufferViews.reserve(m_maxBindings);
        dynamicOffsets.reserve(programObj.dynamicBindings.size() + uboArrayExtra);

        // Eligibility probe for FastRebindMemo, filled by this walk: exactly one
        // dynamic-UBO descriptor (no arrayed elements) and otherwise only
        // combined-image samplers, so the whole set's content is pinned by the
        // sampler hint plus one (buffer, range) compare.
        Uint32 dynamicUboDescriptorCount = 0;
        Uint32 fastRebindUboBinding = 0;
        Bool fastRebindKindsEligible = true;

        // Iterate only the bindings this program declares. The old walk covered all 256 slots of
        // bindingKinds on every draw to find the 1-8 a real program uses.
        for (const Uint32 binding : programObj.activeBindings) {
            if (binding >= m_maxBindings) {
                break; // ascending, so nothing past the cap can follow
            }
            const auto kind = programObj.bindingKinds[binding];

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptorSet;
            write.dstBinding = binding;
            write.dstArrayElement = 0;
            write.descriptorCount = 1;

            if (kind == ProgramFactory::DescriptorBindingKind::UniformBufferDynamic) {
                const Uint32 descriptorCount = BindingDescriptorCount(programObj, binding);
                dynamicUboDescriptorCount += descriptorCount;
                fastRebindUboBinding = binding;
                const SizeT firstBufferInfoIndex = bufferInfos.size();
                for (Uint32 element = 0; element < descriptorCount; ++element) {
                    VkDescriptorBufferInfo bufferInfo{};
                    // Keep offset 0 (sub-range selected via the dynamic offset) so the hashed bufferInfo
                    // is stable across draws and the descriptor-set reuse cache keeps hitting.
                    bufferInfo.offset = 0;
                    Uint32 dynOffset = 0;
                    if (!ResolveDynamicUboDescriptor(program, programObj, binding, element, frameIndex,
                                                     bufferInfo.buffer, bufferInfo.range, dynOffset)) {
                        return false;
                    }
                    bufferInfos.push_back(bufferInfo);
                    // Dynamic offsets are consumed in binding order, then array element order,
                    // matching Vulkan's dynamic-offset consumption rules.
                    dynamicOffsets.push_back(dynOffset);
                }

                write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                write.descriptorCount = descriptorCount;
                write.pBufferInfo = &bufferInfos[firstBufferInfoIndex];
                writes.push_back(write);
            } else if (kind == ProgramFactory::DescriptorBindingKind::UniformTexelBuffer) {
                VkBufferView bufferView = VK_NULL_HANDLE;
                if (!ResolveTexelBufferDescriptor(program, programObj, binding, frameIndex, bufferView) ||
                    bufferView == VK_NULL_HANDLE) {
                    MGLOG_E(
                        "UniformDescriptorBinder::BindProgramUniformBuffers failed: texture buffer binding %u has no valid descriptor",
                        binding);
                    return false;
                }

                texelBufferViews.push_back(bufferView);
                fastRebindKindsEligible = false;
                write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
                write.pTexelBufferView = &texelBufferViews.back();
                writes.push_back(write);
            } else if (kind == ProgramFactory::DescriptorBindingKind::StorageTexelBuffer) {
                // Shares texelBufferViews with the sampled kind above, and may do so safely for
                // the same reason: neither kind can be an array, so each contributes exactly one
                // element and the reserve of m_maxBindings cannot be outrun - which is what keeps
                // the &back() below from dangling when a later binding pushes.
                VkBufferView bufferView = VK_NULL_HANDLE;
                if (!ResolveStorageTexelBufferDescriptor(program, programObj, binding, frameIndex, bufferView) ||
                    bufferView == VK_NULL_HANDLE) {
                    MGLOG_E("UniformDescriptorBinder::BindProgramUniformBuffers failed: image buffer binding %u "
                            "has no valid descriptor",
                            binding);
                    return false;
                }

                texelBufferViews.push_back(bufferView);
                fastRebindKindsEligible = false;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
                write.pTexelBufferView = &texelBufferViews.back();
                writes.push_back(write);
            } else if (kind == ProgramFactory::DescriptorBindingKind::StorageBuffer) {
                // One write per binding, but `descriptorCount` buffer infos: a GLSL block
                // instance array occupies a single binding whose elements each come from their
                // own GL binding point.
                const Uint32 descriptorCount = BindingDescriptorCount(programObj, binding);
                const SizeT firstBufferInfoIndex = bufferInfos.size();
                for (Uint32 element = 0; element < descriptorCount; ++element) {
                    VkDescriptorBufferInfo bufferInfo{};
                    if (!ResolveStorageBufferDescriptor(program, programObj, binding, element, bufferInfo)) {
                        MGLOG_E(
                            "UniformDescriptorBinder::BindProgramUniformBuffers failed: storage buffer binding %u "
                            "element %u has no valid descriptor",
                            binding, element);
                        return false;
                    }
                    bufferInfos.push_back(bufferInfo);
                }

                fastRebindKindsEligible = false;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.descriptorCount = descriptorCount;
                write.pBufferInfo = &bufferInfos[firstBufferInfoIndex];
                writes.push_back(write);
            } else if (kind == ProgramFactory::DescriptorBindingKind::StorageImage) {
                // One write per binding, but `descriptorCount` image infos: an ARRAY of image
                // uniforms is a single binding whose elements each carry their own image unit.
                // Writing only element 0 - which is all this used to do - left elements 1..N
                // never written at all, and a shader that indexes them reads an undefined
                // descriptor (lavapipe faults inside the shader; a real driver is free to do
                // anything).
                const Uint32 descriptorCount = BindingDescriptorCount(programObj, binding);
                const SizeT firstImageInfoIndex = imageInfos.size();
                for (Uint32 element = 0; element < descriptorCount; ++element) {
                    VkDescriptorImageInfo imageInfo{};
                    if (!ResolveStorageImageDescriptor(commandBuffer, program, programObj, binding, element,
                                                       imageInfo)) {
                        MGLOG_E(
                            "UniformDescriptorBinder::BindProgramUniformBuffers failed: storage image binding %u "
                            "element %u has no valid descriptor",
                            binding, element);
                        return false;
                    }
                    imageInfos.push_back(imageInfo);
                }
                fastRebindKindsEligible = false;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                write.descriptorCount = descriptorCount;
                write.pImageInfo = &imageInfos[firstImageInfoIndex];
                writes.push_back(write);
            } else {
                // One write per binding, but `descriptorCount` image infos: a sampler ARRAY is a
                // single binding whose elements each carry their own texture unit. Writing only
                // element 0 - which is all this used to do - left elements 1..N never written,
                // so a shader indexing them sampled a descriptor nobody had filled in
                // (KHR-GL42.shading_language_420pack.binding_sampler_array).
                const Uint32 descriptorCount = BindingDescriptorCount(programObj, binding);
                // Overrides come only from MobileGL's own blit and depth-mipmap programs, whose
                // samplers are scalars; the override replaces THE descriptor at its binding, so
                // there is no element for it to mean on an arrayed one.
                const Bool overrideThisBinding = samplerBindingOverride != nullptr &&
                                                 samplerBindingOverride->binding == binding &&
                                                 samplerBindingOverride->texture != nullptr &&
                                                 samplerBindingOverride->sampler != nullptr;
                MOBILEGL_ASSERT(
                    !overrideThisBinding || descriptorCount == 1,
                    "BindProgramUniformBuffers: sampler override targets arrayed binding %u (%u descriptors)",
                    binding, descriptorCount);
                const SizeT firstImageInfoIndex = imageInfos.size();
                for (Uint32 element = 0; element < descriptorCount; ++element) {
                    VkDescriptorImageInfo imageInfo{};
                    Bool hasImage = false;
                    if (overrideThisBinding && element == 0) {
                        hasImage = ResolveSamplerDescriptorOverride(*samplerBindingOverride, imageInfo);
                    } else {
                        hasImage = ResolveSamplerDescriptor(commandBuffer, program, programObj, binding, element,
                                                            imageInfo, samplerDescriptorsUnchangedHint);
                    }
                    if (!hasImage) {
                        MGLOG_E(
                            "UniformDescriptorBinder::BindProgramUniformBuffers failed: sampler binding %u element %u "
                            "has no valid texture descriptor",
                            binding, element);
                        return false;
                    }
                    if (imageInfo.sampler == VK_NULL_HANDLE || imageInfo.imageView == VK_NULL_HANDLE) {
                        MGLOG_E(
                            "UniformDescriptorBinder::BindProgramUniformBuffers failed: sampler binding %u element %u "
                            "has null sampler or imageView",
                            binding, element);
                        return false;
                    }
                    imageInfos.push_back(imageInfo);
                }
                if (descriptorCount > 1) {
                    // The dynamic-offset-only rebind replays a whole descriptor set on the
                    // strength of the sampler hint alone, and its eligibility probe was written
                    // for bindings that carry one descriptor each. An arrayed sampler binding
                    // also bypasses the per-binding descriptor memo, so there is nothing for it
                    // to win here either.
                    fastRebindKindsEligible = false;
                }
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.descriptorCount = descriptorCount;
                write.pImageInfo = &imageInfos[firstImageInfoIndex];
                writes.push_back(write);
            }
        }

        // Reuse a recent draw's descriptor set when the resolved content is
        // byte-identical (only the bind-time dynamic offsets differ). The signature
        // covers the descriptor-set layout + every write's binding/type/count + the
        // pointed-to buffer/image/texel-buffer infos (all value-initialized, so no
        // padding noise). Correctness: bindings are re-resolved every draw, so the
        // signature always reflects the current state and reuse happens only on an
        // exact match; a reused set is never re-acquired within a frame (the acquire
        // cursor only advances), so its written contents survive; the layout is part of
        // the signature so reuse never crosses programs. Sampler overrides (blits)
        // bypass and invalidate the cache.
        Uint64 signature = 0xcbf29ce484222325ULL;
        {
            const auto mix64 = [&signature](Uint64 word) {
                signature = (signature ^ word) * 0x100000001b3ULL;
            };
            // The hashed descriptor payloads (VkDescriptorBufferInfo=24B,
            // VkDescriptorImageInfo=24B, VkBufferView=8B) are all 8-byte-multiple sized
            // and value-initialized (padding is zero), so hashing 64-bit words at a time
            // is exact and ~8x cheaper than byte-wise - the signature is recomputed every
            // draw, so its own cost has to stay small.
            const auto mixWords = [&mix64](const void* data, SizeT byteSize) {
                const auto* words = static_cast<const Uint64*>(data);
                for (SizeT i = 0; i < byteSize / sizeof(Uint64); ++i) {
                    mix64(words[i]);
                }
            };
            mixWords(&programObj.descriptorSetLayout, sizeof(programObj.descriptorSetLayout));
            for (const auto& write : writes) {
                mix64((static_cast<Uint64>(write.dstBinding) << 40) ^
                      (static_cast<Uint64>(write.descriptorType) << 8) ^
                      static_cast<Uint64>(write.descriptorCount));
            }
            mixWords(bufferInfos.data(), bufferInfos.size() * sizeof(VkDescriptorBufferInfo));
            mixWords(imageInfos.data(), imageInfos.size() * sizeof(VkDescriptorImageInfo));
            mixWords(texelBufferViews.data(), texelBufferViews.size() * sizeof(VkBufferView));
        }

        VkDescriptorSet reusedSet = VK_NULL_HANDLE;
        if (cacheable) {
            for (const auto& entry : m_descriptorReuseMemo) {
                if (entry.valid && entry.signature == signature) {
                    reusedSet = entry.set;
                    break;
                }
            }
        }
        if (reusedSet != VK_NULL_HANDLE) {
            descriptorSet = reusedSet;
        } else {
            VkResult allocResult = AcquireDescriptorSet(frameIndex, programObj, descriptorSet);
            if (allocResult != VK_SUCCESS || descriptorSet == VK_NULL_HANDLE) {
                MGLOG_E("UniformDescriptorBinder::BindProgramUniformBuffers failed: descriptor set acquire returned %d",
                        allocResult);
                return false;
            }
            for (auto& write : writes) {
                write.dstSet = descriptorSet;
            }
            if (!writes.empty()) {
                vkUpdateDescriptorSets(m_device, static_cast<Uint32>(writes.size()), writes.data(), 0, nullptr);
            }
            if (cacheable) {
                m_descriptorReuseMemo[m_descriptorReuseMemoNext] =
                    DescriptorReuseEntry{signature, descriptorSet, true};
                m_descriptorReuseMemoNext = (m_descriptorReuseMemoNext + 1) % kDescriptorReuseMemoSize;
            } else {
                for (auto& entry : m_descriptorReuseMemo) {
                    entry.valid = false;
                }
            }
        }

        // (Re)record the dynamic-offset-only rebind memo. Recording on every
        // cacheable walk (allocated or reused set alike - both hold exactly the
        // content just computed) keeps the single slot tracking the most recent
        // program; a non-cacheable override walk drops it alongside the reuse
        // memo above.
        if (cacheable && fastRebindKindsEligible && dynamicUboDescriptorCount == 1) {
            m_fastRebindMemo = FastRebindMemo{
                /*valid=*/true,          frameIndex,      program.GetLifetimeId(), programObj.hash,
                fastRebindUboBinding,    bufferInfos[0].buffer,
                bufferInfos[0].range,    descriptorSet};
        } else {
            m_fastRebindMemo.valid = false;
        }

        BindDescriptorSetDeduped(commandBuffer, bindPoint, programObj.pipelineLayout, descriptorSet,
                                 dynamicOffsets);
        return true;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
