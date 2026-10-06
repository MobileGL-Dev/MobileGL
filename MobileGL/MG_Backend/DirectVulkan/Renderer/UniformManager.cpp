// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/UniformDescriptorBinder.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "RenderPassGuard.h"
#include "UniformManager.h"

#include "MG_Backend/DirectVulkan/DirectVulkanResourceState.h"
#include "MG_State/GLState/Core.h"
#include <MG_Backend/MGPipe/PipeInputs.h>
// P5c ev: the GPU-write announcement routes through the reverse channel (R2).
#include <MG_Impl/Pipe/ResourceTracker.h>
#include "MG_State/GLState/ProgramState/ProgramObject.h"
#include "MG_State/GLState/TextureState/TextureObject1D.h"
#include "MG_State/GLState/TextureState/TextureObject2D.h"
#include "MG_State/GLState/TextureState/TextureObject2DCube.h"
#include "MG_State/GLState/TextureState/TextureObject3D.h"
#include "MG_State/GLState/TextureState/TextureObjectBuffer.h"
#include "MG_State/GLState/TextureState/TextureObjectStubs.h"
#include "MG_Util/Converters/GLToMG/TextureEnumConverter.h"
#include "MG_Util/Converters/MGToStr/FramebufferEnumConverter.h"
#include "MG_Util/Converters/MGToVk/TextureEnumConverter.h"
#include "MG_Util/Metrics/PipeStats.h"
#include "MG_Util/Metrics/TextureMetrics.h"
#include "MG_Util/ShaderTranspiler/Types.h"
#include <Config.h>
#include <MG_Pipe/PipeApply.h>
// P7 wave 0: the seam WireDescriptorFatal dies through. See MG_Pipe/PipeSessionFail.h.
#include <MG_Pipe/PipeSessionFail.h>
// P7 wave 2 package B3: rule I's tally for this file's silent wire-draw exits.
#include "WireDeclineTally.h"
#include <vulkan/utility/vk_format_utils.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace MobileGL::MG_Backend::DirectVulkan {
    SizeT UniformManager::WireImageViewKeyHash::operator()(const WireImageViewKey& key) const {
        SizeT hash = std::hash<VkImage>{}(key.image);
        const auto mix = [&hash](Uint64 value) {
            hash ^= std::hash<Uint64>{}(value) + static_cast<SizeT>(0x9e3779b97f4a7c15ULL) +
                    (hash << 6) + (hash >> 2);
        };
        mix(key.root.Slot);
        mix(key.root.Gen);
        mix(key.imageEpoch);
        mix(key.flags);
        mix(key.type);
        mix(key.format);
        mix(key.components.r);
        mix(key.components.g);
        mix(key.components.b);
        mix(key.components.a);
        mix(key.range.aspectMask);
        mix(key.range.baseMipLevel);
        mix(key.range.levelCount);
        mix(key.range.baseArrayLayer);
        mix(key.range.layerCount);
        return hash;
    }

    // P7 wave 0, the descriptor half of WireFramebuffer.inc's MagmaWireFatal - same message,
    // same seam, same reason. Four of the fifteen P7-marked refusals die here.
    [[noreturn]] static void WireDescriptorFatal(const char* detail) {
        MG_Pipe::MGPipeSessionFail(MG_Pipe::MGPipeFatalFamily::UnmigratedVerb,
                                   "MGPipe: Fatal{UnmigratedVerb, \"Magma:%s\"}", detail);
    }

    static Bool ResolveWireRange(Uint64 offset, Uint64 declaredSize, VkDeviceSize bufferSize,
                                 VkDeviceSize& start, VkDeviceSize& size) {
        if (offset >= bufferSize) return false;
        start = static_cast<VkDeviceSize>(offset);
        const VkDeviceSize remaining = bufferSize - start;
        size = declaredSize == MG_Pipe::kMGPipeWholeBuffer
            ? remaining : std::min<VkDeviceSize>(declaredSize, remaining);
        return size != 0;
    }

    static VkImageViewType WireViewType(MG_Pipe::MGPipeResourceTarget target) {
        using T = MG_Pipe::MGPipeResourceTarget;
        switch (target) {
        case T::Tex1D: return VK_IMAGE_VIEW_TYPE_1D;
        case T::Tex1DArray: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        case T::Tex3D: return VK_IMAGE_VIEW_TYPE_3D;
        case T::TexCube: return VK_IMAGE_VIEW_TYPE_CUBE;
        case T::TexCubeArray: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        case T::Tex2DArray: case T::Tex2DMSArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        default: return VK_IMAGE_VIEW_TYPE_2D;
        }
    }

    // GL ignores Layer/Layered for a target with no layers. In particular image1D stays a 1D
    // view, and an ignored nonzero Layer must not address another slice.
    static Bool WireTargetHasLayers(MG_Pipe::MGPipeResourceTarget target) {
        using T = MG_Pipe::MGPipeResourceTarget;
        return target == T::Tex1DArray || target == T::Tex2DArray || target == T::Tex2DMSArray ||
               target == T::Tex3D || target == T::TexCube || target == T::TexCubeArray;
    }

    // GL 4.6 core 8.26: an access through an image unit whose texture does not HAVE the
    // (level, layer) the unit names is INVALID. A load returns zero, a store does nothing, an
    // atomic updates nothing - and none of it is an error, neither at bind time (the bind only
    // checks the level's and the layer's signs) nor at the draw. It is the observable an empty
    // unit has, so the answer is the empty unit's (ResolveWirePlaceholderImage): a NULL storage
    // descriptor where the device enables VK_EXT_robustness2 nullDescriptor, else a storage
    // placeholder private to the unit (per shape), cleared before each use, of the dimensionality
    // the SHADER declared.
    //
    // KHR-GL46.shader_image_load_store.incomplete_textures is the shape that found it: level 2
    // of a texture that defines only level 0 (MAX_LEVEL 7, a mipmapping filter). The same
    // question comes up empty for a level past an immutable texture's storage, a layer past an
    // array's last slice, a texture that has no storage at all, and a level past a texture
    // view's own window even where its owner has one. The record carries exactly what
    // glBindImageTexture was given, so it is decided here, by the walk every wire consumer uses
    // to find a texel's storage, and from that walk's "not there" answer ONLY: a null for any
    // other reason (a dead record, a stale view CSO) stays the descriptor resolve's refusal.
    //
    // NOT DECIDED HERE, deliberately: a level that IS in storage but lies outside
    // [BASE_LEVEL, MAX_LEVEL], or of a texture that is not mipmap-complete. The monolith arm
    // binds those as it finds them too, and the P7 gate 5 comparison is against that arm.
    static Bool WireShaderImageNamesNoTexel(VkTextureManager& textures, const MG_Pipe::MGPImageView& image) {
        const auto& state = MG_Pipe::MGPipeApplier();
        if (MG_Pipe::MGPipeHandleIsNull(image.Res) || image.Res.Slot >= state.TextureResources.size()) return false;
        const auto& record = state.TextureResources[image.Res.Slot];
        if (!record.Live || record.Gen != image.Res.Gen) return false;
        const Bool selectsLayer =
            WireTargetHasLayers(static_cast<MG_Pipe::MGPipeResourceTarget>(record.Desc.Target)) && !image.Layered;
        Uint32 level = image.Level;
        Uint32 layer = selectsLayer ? image.Layer : 0;
        Bool outsideWindow = false;
        (void)textures.ResolveWireTextureStorage(image.Res, level, layer, nullptr, nullptr, &outsideWindow);
        return outsideWindow;
    }

    static VkComponentSwizzle WireSwizzle(Uint8 value) {
        switch (static_cast<TextureSwizzleParam>(value)) {
        case TextureSwizzleParam::Red: return VK_COMPONENT_SWIZZLE_R;
        case TextureSwizzleParam::Green: return VK_COMPONENT_SWIZZLE_G;
        case TextureSwizzleParam::Blue: return VK_COMPONENT_SWIZZLE_B;
        case TextureSwizzleParam::Alpha: return VK_COMPONENT_SWIZZLE_A;
        case TextureSwizzleParam::Zero: return VK_COMPONENT_SWIZZLE_ZERO;
        case TextureSwizzleParam::One: return VK_COMPONENT_SWIZZLE_ONE;
        default: WireDescriptorFatal("sampler-swizzle");
        }
    }

    Bool UniformManager::ResolveWireImageDescriptor(
        VkCommandBuffer commandBuffer, const MagmaProgramSource& program,
        const ProgramFactory::VkProgramObject& programObj, Uint32 binding, Uint32 element,
        Bool storage, VkDescriptorImageInfo& out) const {
        out = {};
        auto& state = MG_Pipe::MGPipeApplier();
        const Int baseLocation = programObj.samplerUniformLocationByBinding[binding];
        const Int location = baseLocation + static_cast<Int>(element);
        if (baseLocation < 0 || !program.UniformLocationsAliasSameUniform(baseLocation, location)) return false;
        const Int unit = program.GetUniformSamplerOrImageUnitIndex(static_cast<Uint>(location));
        if (unit < 0 || static_cast<Uint32>(unit) >=
            (storage ? MG_Pipe::kMGPipeMaxImageUnits : MG_Pipe::kMGPipeMaxTextureUnits)) return false;
        const auto handle = storage ? state.BoundShaderImages[unit].Res : state.BoundSamplerViews[unit].Texture;
        // An EMPTY image unit is the same 8.26 invalid access as the named-but-missing texel
        // below, so it takes the same answer: a null storage descriptor where the device has
        // one, else the unit-private placeholder (codex closeout finding 2).
        if (MG_Pipe::MGPipeHandleIsNull(handle))
            return ResolveWirePlaceholderImage(commandBuffer, program, programObj, binding, storage, out,
                                               VK_FORMAT_UNDEFINED, static_cast<Uint32>(unit));
        if (handle.Slot >= state.TextureResources.size()) WireDescriptorFatal("image-record");
        const auto& record = state.TextureResources[handle.Slot];
        if (!record.Live || record.Gen != handle.Gen) WireDescriptorFatal("image-record-generation");
        // Before the sync: a texture with no storage at all is one of the shapes, and syncing
        // it would refuse rather than answer.
        if (storage && WireShaderImageNamesNoTexel(*m_textureManager, state.BoundShaderImages[unit])) {
            const VkFormat bound = MG_Util::ConvertTextureInternalFormatToVkEnum(
                MG_Util::ConvertGLEnumToTextureInternalFormat(state.BoundShaderImages[unit].InternalFormat));
            return ResolveWirePlaceholderImage(commandBuffer, program, programObj, binding, storage, out, bound,
                                               static_cast<Uint32>(unit));
        }
        auto* resource = m_textureManager->SyncTextureResourceByHandle(handle, false, storage);
        if (!resource) WireDescriptorFatal("image-resource");
        m_textureManager->FlushPendingUploads();

        const auto target = static_cast<MG_Pipe::MGPipeResourceTarget>(record.Desc.Target);
        const Bool layerable = WireTargetHasLayers(target);
        Uint32 level = storage ? state.BoundShaderImages[unit].Level : record.Params.BaseLevel;
        // GL ignores Layer/Layered for non-layerable targets (WireTargetHasLayers).
        Uint32 layer = storage && layerable && !state.BoundShaderImages[unit].Layered
            ? state.BoundShaderImages[unit].Layer : 0;
        const Uint32 localLevel = level;
        const Uint32 localLayer = layer;
        Uint32 layers = 0;
        VkFormat aliasFormat = VK_FORMAT_UNDEFINED;
        const auto root = m_textureManager->ResolveWireTextureStorage(handle, level, layer, &aliasFormat, &layers);
        if (MG_Pipe::MGPipeHandleIsNull(root)) WireDescriptorFatal("image-view-window");
        VkImageViewType type = WireViewType(target);
        if (storage && layerable && !state.BoundShaderImages[unit].Layered) {
            layers = 1;
            type = target == MG_Pipe::MGPipeResourceTarget::Tex1DArray ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D;
        }
        if (type == VK_IMAGE_VIEW_TYPE_3D) layers = 1;
        Uint32 levels = 1;
        if (!storage) {
            if (record.Params.MaxLevel < localLevel || record.Desc.Levels <= localLevel) return false;
            levels = std::min<Uint32>(record.Desc.Levels - localLevel,
                                     record.Params.MaxLevel - localLevel + 1);
            const auto viewHandle = state.BoundSamplerViews[unit].View;
            if (!MG_Pipe::MGPipeHandleIsNull(viewHandle) && viewHandle.Slot < state.SamplerViewCsos.size()) {
                const auto& view = state.SamplerViewCsos[viewHandle.Slot];
                if (!view.Live || view.Gen != viewHandle.Gen || view.View.Texture != handle)
                    WireDescriptorFatal("sampler-view-record");
                // Ordinary texture views can encode an unrestricted window with zero
                // counts; only an actual restriction narrows the resource/parameter range.
                if (view.View.NumLevels != 0) {
                    if (view.View.NumLevels <= localLevel) WireDescriptorFatal("sampler-view-level");
                    levels = std::min<Uint32>(levels, view.View.NumLevels - localLevel);
                }
            } else if (!MG_Pipe::MGPipeHandleIsNull(viewHandle)) {
                WireDescriptorFatal("sampler-view-record");
            }
            levels = std::min(levels, resource->mipLevels - level);
        }
        VkFormat format = aliasFormat == VK_FORMAT_UNDEFINED ? resource->format : aliasFormat;
        const auto domain = programObj.samplerNumericDomainByBinding[binding];
        if (storage) {
            format = ResolveStorageImageViewFormat(programObj.storageImageFormatByBinding[binding],
                state.BoundShaderImages[unit].InternalFormat, format,
                programObj.storageImageUsesBindingFormatByBinding[binding]);
        } else if (resource->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
            format = VkTextureManager::ResolveSampledImageViewFormat(format, domain);
        }
        if (format == VK_FORMAT_UNDEFINED) return false;
        VkImageAspectFlags aspect = resource->aspect;
        if (!storage && (aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
            aspect = record.Params.DepthStencilMode == MG_Pipe::kMGPipeDepthStencilModeStencil
                ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;

        // All wire image descriptors use GENERAL. A texture sampled and image-bound in
        // the same shader then has one truthful layout, including views of the same owner.
        if (!VkTextureManager::TransitionImageLayout(commandBuffer, resource->image, resource->layout,
            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            resource->layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, resource->aspect, 0, resource->mipLevels))
            WireDescriptorFatal("image-descriptor-transition");
        // A second descriptor/draw can keep GENERAL after a shader write. A layout
        // equality fast return is not a memory dependency for that prior write.
        // Only when something may have written an image since the last such barrier
        // (WireImageWriteEpoch): with nothing written in between, consecutive draws keep their
        // render pass open instead of closing it for a barrier that orders nothing.
        if (WireImageWriteEpoch() != m_wireImageBarrierEpoch) {
            VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            memory.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            EndActiveRenderPassOn(commandBuffer);
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0, 1, &memory, 0, nullptr, 0, nullptr);
            m_wireImageBarrierEpoch = WireImageWriteEpoch();
        }
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = resource->image;
        info.viewType = type;
        info.format = format;
        info.subresourceRange = {aspect, level, levels, layer, layers};
        if (!storage) {
            // An X-format shared image carries undefined alpha bytes, exactly like an expanded RGB.
            const Bool alphaIsOne = resource->sharedImageAlphaOne || ResolveTextureFormatInfo(
                static_cast<TextureInternalFormat>(record.Desc.InternalFormat)).expandRgbToRgba;
            const auto swizzle = [&](Uint32 channel) {
                const auto value = record.Params.Swizzle[channel];
                return alphaIsOne && static_cast<TextureSwizzleParam>(value) == TextureSwizzleParam::Alpha
                    ? VK_COMPONENT_SWIZZLE_ONE : WireSwizzle(value);
            };
            info.components = {swizzle(0), swizzle(1), swizzle(2), swizzle(3)};
        }
        VkImageView view = VK_NULL_HANDLE;
        const WireImageViewKey key{root, info.image, m_textureManager->GetTextureImageEpoch(),
            info.flags, info.viewType, info.format, info.components, info.subresourceRange};
        const auto& frame = m_frames[m_wireFrameIndex];
        const auto cached = frame.wireImageViewCache.find(key);
        if (cached != frame.wireImageViewCache.end()) {
            view = cached->second;
        } else {
            if (vkCreateImageView(m_device, &info, nullptr, &view) != VK_SUCCESS) return false;
            frame.wireImageViews.push_back(view);
            frame.wireImageViewCache.emplace(key, view);
        }
        // Only view creation is memoized. Layout/memory dependencies above and
        // write tracking below still run for every descriptor, including hits.
        out.imageView = view;
        out.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        if (storage) {
            if (state.BoundShaderImages[unit].Access != kMGPipeImageAccessReadOnly)
                m_textureManager->MarkWireTextureGpuWritten(handle, localLevel, localLayer, layers);
        } else {
            auto samplerHandle = state.BoundSamplerStates[unit];
            if (MG_Pipe::MGPipeHandleIsNull(samplerHandle)) samplerHandle = record.Params.BuiltinSampler;
            // A texture's built-in sampler CSO can be missing or gone while the texture still names
            // it: sampler CSOs are content-addressed and recycled at capacity, and the texture's
            // record is only re-pointed by its next set_texture_params (a texture whose params never
            // crossed names none at all). Sample with GL's default parameters rather than end the
            // session - the DirectGLES arm declines the push in both cases.
            using SamplerCsoRecord = std::decay_t<decltype(state.SamplerCsos[0])>;
            static const decltype(SamplerCsoRecord::Params) kDefaultSamplerParams{};
            const Bool known = !MG_Pipe::MGPipeHandleIsNull(samplerHandle) &&
                               samplerHandle.Slot < state.SamplerCsos.size();
            const SamplerCsoRecord* sampler = known ? &state.SamplerCsos[samplerHandle.Slot] : nullptr;
            const Bool stale = sampler == nullptr || !sampler->Live || sampler->Gen != samplerHandle.Gen;
            if (stale) {
                MGLOG_E_ONCE("Magma wire: sampler CSO {%u, %u} named by texture unit %u is missing or no longer "
                             "live; sampling with default parameters", samplerHandle.Slot, samplerHandle.Gen, unit);
            }
            out.sampler = m_samplerManager->GetOrCreateSamplerFromParameters(stale ? kDefaultSamplerParams : sampler->Params,
                static_cast<TextureInternalFormat>(record.Desc.InternalFormat),
                domain == SamplerNumericDomain::SignedInteger || domain == SamplerNumericDomain::UnsignedInteger,
                levels);
            if (!out.sampler) return false;
        }
        return true;
    }
    namespace {
        // The R32 member of each numeric class. Every one of the three is a MANDATORY-support
        // format for uniform texel buffers, storage texel buffers and storage images alike
        // (Vulkan 1.0, "Required Format Support"), which is what makes them a fallback that
        // cannot itself fail for want of device features.
        VkFormat PlaceholderFormatForNumericDomain(SamplerNumericDomain numericDomain) {
            switch (numericDomain) {
            case SamplerNumericDomain::Float:
                return VK_FORMAT_R32_SFLOAT;
            case SamplerNumericDomain::SignedInteger:
                return VK_FORMAT_R32_SINT;
            case SamplerNumericDomain::UnsignedInteger:
                return VK_FORMAT_R32_UINT;
            case SamplerNumericDomain::Unknown:
                break;
            }
            return VK_FORMAT_UNDEFINED;
        }

        Bool BufferFormatSupportsFeature(VkPhysicalDevice physicalDevice, VkFormat format,
                                         VkFormatFeatureFlags requiredFeature) {
            if (physicalDevice == VK_NULL_HANDLE || format == VK_FORMAT_UNDEFINED) {
                return false;
            }
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
            return (properties.bufferFeatures & requiredFeature) == requiredFeature;
        }

    }

#include "WirePlaceholderImages.inc"

    // descriptorCount this binding declares in the descriptor set layout (1 for everything that
    // is not an array). Kept in one place because the layout, the scratch reservation and the
    // per-element write loops must all agree on it.
    static Uint32 BindingDescriptorCount(const ProgramFactory::VkProgramObject& programObj, Uint32 binding) {
        return binding < programObj.bindingDescriptorCounts.size()
                   ? std::max<Uint32>(1u, programObj.bindingDescriptorCounts[binding])
                   : 1u;
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

    Bool UniformManager::Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                                             VkBufferManager* bufferManager,
                                             ProgramFactory* programFactory,
                                             VkDeviceSize minUniformBufferOffsetAlignment, Uint32 frameCount,
                                             Uint32 maxBindings, Uint32 setsPerFrame,
                                             VkTextureManager* textureManager, VkSamplerManager* samplerManager) {
        Shutdown();

        MOBILEGL_ASSERT(device != VK_NULL_HANDLE, "UniformDescriptorBinder::Initialize requires valid VkDevice");
        MOBILEGL_ASSERT(physicalDevice != VK_NULL_HANDLE,
                        "UniformDescriptorBinder::Initialize requires valid VkPhysicalDevice");
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
        m_physicalDevice = physicalDevice;
        m_bufferManager = bufferManager;
        m_programFactory = programFactory;
        m_minDynamicOffsetAlignment = std::max<VkDeviceSize>(1, minUniformBufferOffsetAlignment);
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &properties);
        m_wireStorageOffsetAlignment = std::max<VkDeviceSize>(1, properties.limits.minStorageBufferOffsetAlignment);
        m_wireTexelOffsetAlignment = std::max<VkDeviceSize>(1, properties.limits.minTexelBufferOffsetAlignment);
        m_wireMaxUniformRange = properties.limits.maxUniformBufferRange;
        m_wireMaxStorageRange = properties.limits.maxStorageBufferRange;
        m_wireMaxTexelElements = properties.limits.maxTexelBufferElements;
        m_frameCount = frameCount;
        m_maxBindings = maxBindings;
        m_samplerResolveMemo.assign(m_maxBindings, SamplerResolveMemo{});
        // Every entry is freshly constructed (all-invalid), so nothing needs sweeping until
        // a resolve writes one.
        m_samplerResolveMemoHighWater = 0;
        m_setsPerFrame = setsPerFrame;
        m_peakDescriptorSetsObserved = 0;
        m_trimQuietEpochs = MG_Config::Features.MagmaDescriptorTrimFrames;
        // The renderer records its first frame into slot 0 before any BeginFrame runs.
        m_epochSlot = 0;
        m_epochOpen = true;
        m_descriptorSlotTrims = 0;
        m_textureManager = textureManager;
        m_samplerManager = samplerManager;

        m_frames.resize(m_frameCount);
        for (Uint32 frameIndex = 0; frameIndex < m_frameCount; ++frameIndex) {
            auto& frame = m_frames[frameIndex];
            frame.activeDescriptorPoolIndex = 0;
            frame.allocatedSetsThisFrame = 0;
            frame.peakAllocatedSetsThisFrame = 0;
            frame.capacitySets = m_setsPerFrame;
            frame.quietEpochs = 0;
            frame.descriptorPools.clear();

            VkDescriptorPool initialPool = VK_NULL_HANDLE;
            if (!CreateDescriptorPool(m_setsPerFrame, false, initialPool)) {
                MGLOG_E_ONCE("UniformDescriptorBinder::Initialize failed: cannot create frame descriptor pool %u",
                        frameIndex);
                Shutdown();
                return false;
            }
            frame.descriptorPools.push_back({initialPool, m_setsPerFrame, 0, false});
            MGLOG_D("UniformDescriptorBinder: frame %u descriptor pool created (maxSets=%u)", frameIndex,
                    m_setsPerFrame);
        }

        return true;
    }

    void UniformManager::Shutdown() {
        // Before the per-frame loop, because these views are NOT owned by any frame slot (see
        // m_unboundTexelBufferViews) and the loop below is what clears m_device.
        if (m_device != VK_NULL_HANDLE) {
            for (const auto& viewEntry : m_unboundTexelBufferViews) {
                if (viewEntry.second != VK_NULL_HANDLE) {
                    vkDestroyBufferView(m_device, viewEntry.second, nullptr);
                }
            }
        }
        m_unboundTexelBufferViews.clear();
        for (auto& frame : m_frames) {
            if (m_device != VK_NULL_HANDLE) {
                frame.wireImageViewCache.clear();
                for (const auto view : frame.wireImageViews) vkDestroyImageView(m_device, view, nullptr);
                frame.wireImageViews.clear();
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
            frame.capacitySets = 0;
            frame.quietEpochs = 0;
        }
        m_frames.clear();
        for (auto& entry : m_wirePlaceholderImages) DestroyWirePlaceholderImage(entry.second);
        m_wirePlaceholderImages.clear();
        m_wirePrivateStoragePlaceholders = 0;
        m_wireInvalidStorageImagesBindNull = false;
        m_epochSlot = 0;
        m_epochOpen = false;

        m_bufferManager = nullptr;
        m_programFactory = nullptr;
        m_device = VK_NULL_HANDLE;
        m_physicalDevice = VK_NULL_HANDLE;
        m_minDynamicOffsetAlignment = 1;
        m_frameCount = 0;
        m_maxBindings = 0;
        m_samplerResolveMemo.clear();
        m_samplerResolveMemoHighWater = 0;
        m_setsPerFrame = 0;
        m_peakDescriptorSetsObserved = 0;
        m_textureManager = nullptr;
        m_samplerManager = nullptr;
    }

    void UniformManager::BeginFrame(Uint32 frameIndex) {
        MOBILEGL_ASSERT(frameIndex < m_frames.size(), "UniformDescriptorBinder::BeginFrame invalid frame index");
        auto& frame = m_frames[frameIndex];
        m_wireFrameIndex = frameIndex;
        frame.wireImageViewCache.clear();
        for (const auto view : frame.wireImageViews) vkDestroyImageView(m_device, view, nullptr);
        frame.wireImageViews.clear();
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
        // The epoch that just ended is the one of the slot begun last - this slot again on the
        // drain path, the previous frame's slot on the Present path. Its peak is still unreset.
        if (m_epochOpen && m_epochSlot < m_frames.size()) {
            NoteEndedEpochForTrim(m_frames[m_epochSlot].peakAllocatedSetsThisFrame);
        }
        m_epochSlot = frameIndex;
        m_epochOpen = true;
        // No pending command buffer references this slot's sets here (see TrimFrameDescriptorPools
        // for why that holds at both of BeginFrame's call sites), so a quiet grown slot can be
        // handed back whole.
        if (m_trimQuietEpochs != 0 && frame.descriptorPools.size() > 1 &&
            frame.capacitySets >= kDescriptorTrimMinCapacity && frame.quietEpochs >= m_trimQuietEpochs) {
            TrimFrameDescriptorPools(frame, frameIndex);
        }
        frame.activeDescriptorPoolIndex = 0;
        frame.allocatedSetsThisFrame = 0;
        frame.peakAllocatedSetsThisFrame = 0;
        for (auto& cacheEntryPair : frame.descriptorSetCacheByLayout) {
            cacheEntryPair.second.cursor = 0;
        }
        // The frame's descriptor sets are recycled above, so last frame's reuse targets
        // are gone: start the per-draw descriptor-reuse cache fresh this frame.
        InvalidateDescriptorSetMemos();
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

    Bool UniformManager::WireDescriptorSetBudgetReached(Uint32 frameIndex) const {
        MOBILEGL_ASSERT(frameIndex < m_frames.size(), "WireDescriptorSetBudgetReached invalid frame index");
        return m_frames[frameIndex].allocatedSetsThisFrame >= kWireDescriptorSetBudget;
    }

    SizeT UniformManager::RewindWireDescriptorSets(Uint32 frameIndex) {
        MOBILEGL_ASSERT(frameIndex < m_frames.size(), "RewindWireDescriptorSets invalid frame index");
        auto& frame = m_frames[frameIndex];
        // The caller proved that the last graphics submit using these sets has
        // retired and that no unsubmitted command buffer still references them.
        // Keep the pools and sets: only the per-layout write cursors rewind.
        // Image and buffer views stay alive until the regular frame boundary.
        m_peakDescriptorSetsObserved = std::max(m_peakDescriptorSetsObserved, frame.peakAllocatedSetsThisFrame);
        SizeT cachedSets = 0;
        for (auto& entry : frame.descriptorSetCacheByLayout) {
            cachedSets += entry.second.sets.size();
            entry.second.cursor = 0;
        }
        frame.allocatedSetsThisFrame = 0;
        // The PEAK is not reset: a rewind ends a window of the epoch, not the epoch. The trim scores
        // an epoch by the most sets the slot held at once in it (NoteEndedEpochForTrim at the next
        // BeginFrame), and on this arm that is the largest window - resetting here would score a
        // burst frame by its last window alone and trim a slot that re-grows every frame.
        InvalidateDescriptorSetMemos();
        const Uint32 touchedBindings =
            std::min<Uint32>(m_samplerResolveMemoHighWater, static_cast<Uint32>(m_samplerResolveMemo.size()));
        for (Uint32 binding = 0; binding < touchedBindings; ++binding) {
            m_samplerResolveMemo[binding].valid = false;
            m_samplerResolveMemo[binding].infoValid = false;
        }
        return cachedSets;
    }

    void UniformManager::InvalidateDescriptorSetMemos() {
        // Every VkDescriptorSet handle held outside the per-layout caches - in this backend they all
        // live in this class. A trim can hand a destroyed set's handle value to a fresh allocation,
        // so the bind-dedup shadow must drop too, not only the two reuse memos.
        for (auto& entry : m_descriptorReuseMemo) {
            entry.valid = false;
        }
        m_fastRebindMemo.valid = false;
        m_lastBindValid = false;
    }

    void UniformManager::NoteEndedEpochForTrim(Uint32 epochPeakSets) {
        for (auto& slot : m_frames) {
            if (static_cast<Uint64>(epochPeakSets) * kDescriptorTrimHeadroom > slot.capacitySets) {
                slot.quietEpochs = 0;
            } else if (slot.quietEpochs < std::numeric_limits<Uint32>::max()) {
                ++slot.quietEpochs;
            }
        }
    }

    void UniformManager::TrimFrameDescriptorPools(FrameResources& frame, Uint32 frameIndex) {
        const SizeT poolsBefore = frame.descriptorPools.size();
        const Uint64 capacityBefore = frame.capacitySets;
        // Holders first: the per-layout cache and every memo that may name one of these sets.
        frame.descriptorSetCacheByLayout.clear();
        InvalidateDescriptorSetMemos();
        for (SizeT i = 1; i < frame.descriptorPools.size(); ++i) {
            if (frame.descriptorPools[i].handle != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(m_device, frame.descriptorPools[i].handle, nullptr);
            }
        }
        frame.descriptorPools.resize(1);
        auto& base = frame.descriptorPools[0];
        // Frees every set allocated from it; the spec defines no failure for vkResetDescriptorPool.
        vkResetDescriptorPool(m_device, base.handle, 0);
        base.allocatedSets = 0;
        frame.activeDescriptorPoolIndex = 0;
        frame.capacitySets = base.maxSets;
        frame.quietEpochs = 0;
        ++m_descriptorSlotTrims;
        MGLOG_D("UniformDescriptorBinder: frame %u trimmed its descriptor pools (%zu pools / %llu sets -> 1 / %u)",
                frameIndex, poolsBefore, static_cast<unsigned long long>(capacityBefore), base.maxSets);
    }

    DescriptorPoolCensus UniformManager::GetDescriptorPoolCensus() const {
        DescriptorPoolCensus census;
        census.available = true;
        census.frameSlots = static_cast<uint32_t>(m_frames.size());
        for (const auto& frame : m_frames) {
            const auto poolCount = static_cast<uint32_t>(frame.descriptorPools.size());
            census.pools += poolCount;
            census.maxPoolsInOneSlot = std::max(census.maxPoolsInOneSlot, poolCount);
            census.capacitySets += frame.capacitySets;
            for (const auto& cacheEntryPair : frame.descriptorSetCacheByLayout) {
                census.cachedSets += cacheEntryPair.second.sets.size();
            }
        }
        census.slotTrims = m_descriptorSlotTrims;
        return census;
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
                                                            const MagmaProgramSource& program,
                                                            const ProgramFactory::VkProgramObject& programObj,
                                                            Uint32 binding, Uint32 element,
                                                            VkDescriptorImageInfo& outImageInfo,
                                                            Bool trustUnchangedHint) const {
        return ResolveWireImageDescriptor(commandBuffer, program, programObj, binding, element, false, outImageInfo);
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
                                                            *samplerBindingOverride.texture,
                                                            samplerBindingOverride.forceNearestFiltering,
                                                            resource->sampledLevelCount),
            .imageView = samplerBindingOverride.imageView != VK_NULL_HANDLE ?
                samplerBindingOverride.imageView :
                // Same reason as in ResolveSamplerDescriptor: the resource's own views describe
                // the storage texture, so a view has to be asked for its own.
                (samplerBindingOverride.texture->IsTextureView()
                     ? m_textureManager->GetOrCreateSampledImageView(*samplerBindingOverride.texture,
                                                                     VK_FORMAT_UNDEFINED)
                     : (resource->sampledView != VK_NULL_HANDLE ? resource->sampledView : resource->fullView)),
            .imageLayout = samplerBindingOverride.imageLayout != VK_IMAGE_LAYOUT_UNDEFINED ?
                samplerBindingOverride.imageLayout : resource->layout,
        };
        return outImageInfo.sampler != VK_NULL_HANDLE;
    }

    Bool UniformManager::ResolveWireTexelBufferDescriptor(const MagmaProgramSource& program,
            const ProgramFactory::VkProgramObject& programObj, Uint32 binding, Uint32 frameIndex,
            Bool storage, VkBufferView& out) {
        out = VK_NULL_HANDLE;
        if (binding >= programObj.samplerUniformLocationByBinding.size() ||
            binding >= programObj.samplerNumericDomainByBinding.size() || frameIndex >= m_frames.size()) return false;
        const Int location = programObj.samplerUniformLocationByBinding[binding];
        if (location < 0) return false;
        const Int unit = program.GetUniformSamplerOrImageUnitIndex(static_cast<Uint>(location));
        const auto& state = MG_Pipe::MGPipeApplier();
        if (unit < 0 || static_cast<Uint32>(unit) >=
            (storage ? MG_Pipe::kMGPipeMaxImageUnits : MG_Pipe::kMGPipeMaxTextureUnits)) return false;
        if (storage && binding >= programObj.storageImageFormatByBinding.size()) return false;
        const VkFormat declaredFormat = storage ? programObj.storageImageFormatByBinding[binding] : VK_FORMAT_UNDEFINED;
        const auto placeholder = [&] {
            out = AcquireUnboundTexelBufferView(declaredFormat,
                programObj.samplerNumericDomainByBinding[binding], storage);
            return out != VK_NULL_HANDLE;
        };
        const auto texture = storage ? state.BoundShaderImages[unit].Res : state.BoundSamplerViews[unit].Texture;
        if (MG_Pipe::MGPipeHandleIsNull(texture)) return placeholder();
        if (texture.Slot >= state.TextureResources.size()) WireDescriptorFatal("texel-buffer-record");
        const auto& record = state.TextureResources[texture.Slot];
        if (!record.Live || record.Gen != texture.Gen) WireDescriptorFatal("texel-buffer-generation");
        if (record.Desc.Target != static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::TexBuffer))
            WireDescriptorFatal("texel-buffer-target");
        const auto buffer = record.Desc.BufferForTexBuffer;
        if (MG_Pipe::MGPipeHandleIsNull(buffer)) return placeholder();

        BufferSlice slice{};
        if (!m_bufferManager->AcquireWireSlice(BufferKind::TextureBuffer, buffer, slice) || !slice.IsValid()) return false;
        const auto internalFormat = static_cast<TextureInternalFormat>(record.Desc.InternalFormat);
        VkFormat format = declaredFormat;
        if (storage && format == VK_FORMAT_UNDEFINED && state.BoundShaderImages[unit].InternalFormat != 0)
            format = MG_Util::ConvertTextureInternalFormatToVkEnum(MG_Util::ConvertGLEnumToTextureInternalFormat(
                state.BoundShaderImages[unit].InternalFormat));
        if (format == VK_FORMAT_UNDEFINED) format = MG_Util::ConvertTextureInternalFormatToVkEnum(internalFormat);
        const auto feature = storage ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT : VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
        if (format == VK_FORMAT_UNDEFINED || !BufferFormatSupportsFeature(m_physicalDevice, format, feature)) {
            // P7 A.5, a decline (rule I (a)). GL's buffer-texture format list is wider than what
            // Vulkan mandates for texel buffers - GL_RGB32{F,I,UI} and the 16-bit UNORM members
            // are the realistic misses on a real driver - so this is a device capability gap, not
            // anything the application did wrong, and nothing it could have sent differently.
            //
            // The placeholder is the RIGHT answer here, where it was not for the unaligned range
            // above, and the difference is whether a correct answer exists at all. There, the
            // bytes are addressable and only the descriptor cannot name them, so a placeholder
            // would have hidden a fixable gap behind silently dropped writes. Here the device
            // cannot represent this format as a texel buffer in any form - so the honest
            // observable is monolith's for a missing view: the fetch reads zeros and the draw
            // survives. AcquireUnboundTexelBufferView falls back to the R32 member of the
            // shader's numeric class, whose texel-buffer support is mandatory, so the fallback
            // itself cannot fail for want of device features.
            MGLOG_E_ONCE("ResolveWireTexelBufferDescriptor: binding %u wants VkFormat %d as a %s texel buffer and "
                         "this device does not support it; the binding falls back to the zero placeholder and the "
                         "fetch reads zeros (texel-buffer-native-format)",
                         binding, static_cast<Int>(format), storage ? "storage" : "uniform");
            return placeholder();
        }
        const VkDeviceSize texelSize = MG_Util::GetSizedInternalFormatSizeInBytes(internalFormat);
        VkDeviceSize start = 0, size = 0;
        if (texelSize == 0 || !ResolveWireRange(record.Desc.BufOffset, record.Desc.BufSize, slice.size, start, size)) return false;
        size = std::min(size / texelSize, static_cast<VkDeviceSize>(m_wireMaxTexelElements)) * texelSize;
        if (size == 0) return false;
        if (start > std::numeric_limits<VkDeviceSize>::max() - slice.offset)
            WireDescriptorFatal("texel-buffer-offset-overflow");
        if ((slice.offset + start) % m_wireTexelOffsetAlignment != 0) {
            // P7 A.4, a named decline (rule I (a)). Not reachable through the public API:
            // glTexBufferRange already refuses an offset that is not a multiple of
            // GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT (GL_Texture.cpp), and that cap is published
            // from this same minTexelBufferOffsetAlignment. It stays a decline rather than a
            // copy for the reason below.
            const Bool writable = storage && state.BoundShaderImages[unit].Access != kMGPipeImageAccessReadOnly;
            MGLOG_E_ONCE("ResolveWireTexelBufferDescriptor: binding %u is bound at offset %llu, which is not a "
                         "multiple of this device's minTexelBufferOffsetAlignment (%llu); the %s view is DECLINED "
                         "(unaligned-texel-buffer-range)", binding,
                         static_cast<unsigned long long>(slice.offset + start),
                         static_cast<unsigned long long>(m_wireTexelOffsetAlignment),
                         writable ? "writable" : "read-only");
            // A read-only fetch can still be served the way an unbound buffer texture is -
            // zeros, which is what a missing view gives in monolith - and that keeps the draw.
            // A writable imageBuffer must not: the placeholder is shared by every unbound
            // binding, so absorbing real stores into it would alias other bindings AND lose
            // the application's writes without a word. Copying into an aligned transient slice
            // is not the answer either: the transient arena carries no
            // {UNIFORM,STORAGE}_TEXEL_BUFFER usage (VkBufferManager::InitializeTransientArenas),
            // so no VkBufferView can be made over it, and widening a shared per-frame
            // allocation to buy an unreachable path is the wrong trade. See notes/p7/magma-a.md.
            return writable ? false : placeholder();
        }
        VkBufferViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
        viewInfo.buffer = slice.buffer;
        viewInfo.format = format;
        viewInfo.offset = slice.offset + start;
        viewInfo.range = size;
        if (vkCreateBufferView(m_device, &viewInfo, nullptr, &out) != VK_SUCCESS || out == VK_NULL_HANDLE) return false;
        m_frames[frameIndex].texelBufferViews.push_back(out);
        if (storage && state.BoundShaderImages[unit].Access != kMGPipeImageAccessReadOnly)
            m_bufferManager->MarkWireBufferGpuWritten(buffer, start, size);
        return true;
    }

    Bool UniformManager::ResolveTexelBufferDescriptor(const MagmaProgramSource& program,
                                                      const ProgramFactory::VkProgramObject& programObj,
                                                      Uint32 binding, Uint32 frameIndex,
                                                      VkBufferView& outBufferView) {
        return ResolveWireTexelBufferDescriptor(program, programObj, binding, frameIndex, false, outBufferView);
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
    Bool UniformManager::ResolveStorageTexelBufferDescriptor(const MagmaProgramSource& program,
                                                             const ProgramFactory::VkProgramObject& programObj,
                                                             Uint32 binding, Uint32 frameIndex,
                                                             VkBufferView& outBufferView) {
        return ResolveWireTexelBufferDescriptor(program, programObj, binding, frameIndex, true, outBufferView);
    }

    Bool UniformManager::ResolveStorageBufferDescriptor(const MagmaProgramSource& program,
                                                        const ProgramFactory::VkProgramObject& programObj,
                                                        Uint32 binding, Uint32 element,
                                                        VkDescriptorBufferInfo& outBufferInfo) const {
        outBufferInfo = {};
        MOBILEGL_ASSERT(m_bufferManager != nullptr, "ResolveStorageBufferDescriptor: buffer manager is null");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "ResolveStorageBufferDescriptor: GL context is null");
        MOBILEGL_ASSERT(binding < programObj.storageBlockIndexByBinding.size(),
                        "ResolveStorageBufferDescriptor: binding %u out of range", binding);
        if ((binding >= programObj.storageBlockIndexByBinding.size() ||
                                binding >= programObj.storageBlockNameByBinding.size()))
            WireDescriptorFatal("storage-buffer-reflection-binding");

        const Int blockIndex = programObj.storageBlockIndexByBinding[binding];
        MOBILEGL_ASSERT(blockIndex >= 0, "ResolveStorageBufferDescriptor: no SSBO block mapped to binding %u",
                        binding);
        if (blockIndex < 0) WireDescriptorFatal("storage-buffer-reflection-block");
        // An atomic counter is not an SSBO the application ever declared: glslang lowers every
        // atomic_uint onto a synthesized gl_AtomicCounterBlock_<N> storage block, where N is the
        // GL ATOMIC-COUNTER binding. That block arrives here auto-mapped to an arbitrary
        // storage-block slot, so resolving it the SSBO way looked up GL_SHADER_STORAGE_BUFFER
        // point N' - which is never where glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, N, ...) put
        // the buffer. The counter therefore never reached the shader (KHR-GL43
        // shader_atomic_counters.advanced-usage-*), and when the application also bound an SSBO at
        // the colliding slot the descriptor silently aliased it, so the dispatch wrote over the
        // application's own buffer. DirectGLES has always taken this branch explicitly
        // (SyncAtomicCounterBuffers); this is the same rule in Magma's descriptor resolution.
        //
        // Only the SOURCE of the handle differs. The per-counter layout(offset=) is already folded
        // into the block's SPIR-V member offsets on this path (FlattenAtomicCounterBlockPass is
        // DirectGLES-only), so everything below - residency, the glBindBufferRange window, the
        // descriptor fill - is target-agnostic and stays exactly as it was.
        const String& blockName = programObj.storageBlockNameByBinding[binding];
        const Int atomicCounterBinding = MG_Util::ShaderTranspiler::AtomicCounterBlockGlBinding(blockName);
        const Bool isAtomicCounterBlock = atomicCounterBinding >= 0;
        const BufferTarget bufferTarget =
            isAtomicCounterBlock ? BufferTarget::AtomicCounter : BufferTarget::ShaderStorage;
        // A block instance array declares one block whose elements take consecutive GL binding
        // points from the declared one (GL 4.6 core 7.8), and the reflection collapses the whole
        // array to that one block - so the element index IS the offset from its binding. glslang
        // synthesizes one counter block per GL binding, so a counter block is never an instance
        // array and `element` is always 0 there; the +element rule stays with the SSBO case.
        const GLuint frontendBinding =
            isAtomicCounterBlock
                ? static_cast<GLuint>(atomicCounterBinding)
                : program.GetShaderStorageBlockBinding(static_cast<GLuint>(blockIndex)) + element;
        const auto& state = MG_Pipe::MGPipeApplier();
        const Uint32 cls = isAtomicCounterBlock ? MG_Pipe::kMGPipeShaderBufferClassAtomicCounter
                                               : MG_Pipe::kMGPipeShaderBufferClassShaderStorage;
        if (frontendBinding >= MG_Pipe::kMGPipeMaxBufferBindingPoints)
            WireDescriptorFatal("storage-buffer-binding-point");
        // The applier retains entries outside the last partial update window. Count
        // describes that update, not the upper bound of the complete binding table.
        const auto& range = state.BoundShaderBuffers[cls][frontendBinding];
        if (MG_Pipe::MGPipeHandleIsNull(range.Res)) {
            const BufferSlice placeholder = m_bufferManager->AcquireUnboundStorageDescriptor();
            if (!placeholder.IsValid()) return false;
            outBufferInfo = {placeholder.buffer, placeholder.offset, placeholder.size};
            return true;
        }
        BufferSlice slice{};
        if (!m_bufferManager->AcquireWireSlice(BufferKind::ShaderStorage, range.Res, slice) || !slice.IsValid()) return false;
        VkDeviceSize start = 0, size = 0;
        if (!ResolveWireRange(range.Offset, range.Size, slice.size, start, size)) return false;
        if (start > std::numeric_limits<VkDeviceSize>::max() - slice.offset)
            WireDescriptorFatal("storage-buffer-offset-overflow");
        if ((slice.offset + start) % m_wireStorageOffsetAlignment != 0) {
            // P7 A.4, a named decline (rule I (a)), and the one shape in this cluster an
            // application can actually reach. GL has no queryable atomic-counter buffer
            // offset alignment, so glBindBufferRange only enforces offset % 4 for
            // GL_ATOMIC_COUNTER_BUFFER (GL 4.6 core 6.1.1, GL_Buffer.cpp
            // ValidateBufferRangeOffsetAndSize); glslang lowers the counter block onto a
            // storage buffer, whose descriptor offset must be a multiple of
            // minStorageBufferOffsetAlignment - 16 on lavapipe, 64 on Adreno. Offset 4 is
            // therefore a legal bind this backend cannot express. The plain SSBO half is
            // unreachable by comparison: GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT is
            // published from that same Vulkan limit, so the frontend already refused it
            // with GL_INVALID_VALUE.
            //
            // DECLINED RATHER THAN COPIED, deliberately. Copying the window into an aligned
            // transient slice reads correctly and then silently DROPS every shader write,
            // and this site cannot tell a writable block from a read-only one: there is no
            // readonly bit in the storage-block reflection (ProgramFactory.h carries only
            // name and index), an atomic counter is written by definition, and
            // MarkWireBufferGpuWritten below already assumes any binding here may be
            // written. A correct copy needs a post-dispatch copy-back from the transient
            // slice to the unaligned source, ordered with a barrier - and the place to
            // record that is the draw/dispatch tail in VulkanRenderer.cpp / WireDraw.inc,
            // which belong to wave-2 packages B and C, not to this file. Losing the
            // dispatch with a named line in the role log beats returning stale counters
            // with no error at all. notes/p7/magma-a.md carries the recipe.
            MGLOG_E_ONCE("ResolveStorageBufferDescriptor: block '%s' is bound at offset %llu, which is not a "
                         "multiple of this device's minStorageBufferOffsetAlignment (%llu); %s ranges cannot be "
                         "expressed as a descriptor and the shader's writes would be dropped by a read-only "
                         "copy, so the binding is DECLINED (unaligned-%s-buffer-range)",
                         blockName.c_str(), static_cast<unsigned long long>(slice.offset + start),
                         static_cast<unsigned long long>(m_wireStorageOffsetAlignment),
                         isAtomicCounterBlock ? "atomic-counter" : "shader-storage",
                         isAtomicCounterBlock ? "atomic" : "storage");
            return false;
        }
        if (size > m_wireMaxStorageRange) {
            // P7 A.3: clamp, do not refuse. The monolith arm binds whatever the GL range
            // asked for and never consults maxStorageBufferRange at all (see the resident
            // path below), so refusing here was the wire arm inventing a death the other
            // backend does not have. A clamped binding keeps every byte the device can
            // name reachable; past it the shader is out of the descriptor and
            // robustBufferAccess answers zero, which beats losing the draw outright.
            MGLOG_E_ONCE("ResolveStorageBufferDescriptor: block '%s' bound %llu bytes, past this device's "
                         "maxStorageBufferRange of %llu; the binding is clamped to the limit",
                         blockName.c_str(), static_cast<unsigned long long>(size),
                         static_cast<unsigned long long>(m_wireMaxStorageRange));
            size = m_wireMaxStorageRange;
        }
        outBufferInfo = {slice.buffer, slice.offset + start, size};
        // Shader writes stay in the canonical server VkBuffer. The event marks the
        // client's shadow stale; neither this path nor later acquires seed it back.
        m_bufferManager->MarkWireBufferGpuWritten(range.Res, start, size);
        return true;
    }

    Bool UniformManager::ResolveStorageImageDescriptor(VkCommandBuffer commandBuffer,
                                                       const MagmaProgramSource& program,
                                                       const ProgramFactory::VkProgramObject& programObj,
                                                       Uint32 binding, Uint32 element,
                                                       VkDescriptorImageInfo& outImageInfo) const {
        return ResolveWireImageDescriptor(commandBuffer, program, programObj, binding, element, true, outImageInfo);
    }

    VkBufferView UniformManager::AcquireUnboundTexelBufferView(VkFormat declaredFormat,
                                                               SamplerNumericDomain numericDomain, Bool storage) {
        MOBILEGL_ASSERT(m_bufferManager != nullptr, "AcquireUnboundTexelBufferView: buffer manager is null");
        const VkFormatFeatureFlags requiredFeature = storage ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT
                                                             : VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
        const VkFormat fallbackFormat = PlaceholderFormatForNumericDomain(numericDomain);

        VkFormat format = declaredFormat;
        if (format == VK_FORMAT_UNDEFINED || !BufferFormatSupportsFeature(m_physicalDevice, format, requiredFeature)) {
            // The declared format is what a shader that WRITES through this descriptor is
            // validated against, so it is tried first and kept whenever the device can use it.
            // Falling back is for the two cases where it cannot be: a sampled texel buffer, which
            // declares no format at all, and a device that does not list the declared one as a
            // texel buffer. The fallback stays inside the shader's numeric class, which is the
            // part the descriptor is checked on for a formatless declaration - and the R32
            // members of the three classes are mandatory-support formats, so this cannot fail for
            // want of device features.
            format = fallbackFormat;
        }
        if (format == VK_FORMAT_UNDEFINED || !BufferFormatSupportsFeature(m_physicalDevice, format, requiredFeature)) {
            MGLOG_E_ONCE("AcquireUnboundTexelBufferView: no usable placeholder format (declared=%d fallback=%d "
                         "storage=%s)",
                         static_cast<Int>(declaredFormat), static_cast<Int>(fallbackFormat),
                         storage ? "true" : "false");
            return VK_NULL_HANDLE;
        }

        const Uint64 key = (static_cast<Uint64>(format) << 1) | (storage ? 1ull : 0ull);
        const auto cached = m_unboundTexelBufferViews.find(key);
        if (cached != m_unboundTexelBufferViews.end()) {
            return cached->second;
        }

        const BufferSlice placeholder = m_bufferManager->AcquireUnboundTexelBufferDescriptor();
        if (!placeholder.IsValid()) {
            MGLOG_E_ONCE("AcquireUnboundTexelBufferView: placeholder buffer unavailable");
            return VK_NULL_HANDLE;
        }
        // A buffer view's range must be a whole number of texels of its own format, and the
        // placeholder is sized for the largest of them - so floor rather than assume.
        const VkDeviceSize texelSize = std::max<VkDeviceSize>(1, vkuFormatTexelBlockSize(format));
        const VkDeviceSize range = (placeholder.size / texelSize) * texelSize;
        if (range == 0) {
            MGLOG_E_ONCE("AcquireUnboundTexelBufferView: placeholder holds no whole texel of format=%d",
                         static_cast<Int>(format));
            return VK_NULL_HANDLE;
        }

        VkBufferViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        viewInfo.buffer = placeholder.buffer;
        viewInfo.format = format;
        viewInfo.offset = placeholder.offset;
        viewInfo.range = range;

        VkBufferView view = VK_NULL_HANDLE;
        const VkResult result = vkCreateBufferView(m_device, &viewInfo, nullptr, &view);
        if (result != VK_SUCCESS || view == VK_NULL_HANDLE) {
            MGLOG_E_ONCE("AcquireUnboundTexelBufferView: vkCreateBufferView failed result=%d format=%d", result,
                         static_cast<Int>(format));
            return VK_NULL_HANDLE;
        }
        m_unboundTexelBufferViews.emplace(key, view);
        MGLOG_D("AcquireUnboundTexelBufferView: created placeholder view format=%d storage=%s",
                static_cast<Int>(format), storage ? "true" : "false");
        return view;
    }

    Bool UniformManager::SamplerOverlapsWritableImageSubresource(Int samplerBaseLevel, Int samplerMaxLevel,
                                                                 GLint imageLevel, GLenum imageAccess) {
        return imageAccess != GL_READ_ONLY && imageLevel >= samplerBaseLevel && imageLevel <= samplerMaxLevel;
    }

    Bool UniformManager::ResolveWireUniformBufferPayload(const MagmaProgramSource& program,
            Uint32 blockIndex, Uint32 bindingPoint, UboBindResult& out) const {
        if (bindingPoint >= MG_Pipe::kMGPipeMaxBufferBindingPoints)
            WireDescriptorFatal("uniform-buffer-binding-point");
        VkDeviceSize blockSize = program.GetUBOSizeAt(blockIndex);
        if (blockSize == 0) return false;
        if (blockSize > m_wireMaxUniformRange) {
            // P7 A.3: clamp, do not refuse. A conformant program cannot get here -
            // GL_MAX_UNIFORM_BLOCK_SIZE is published straight from this same
            // maxUniformBufferRange (BackendLoaders/Vulkan/Loader.cpp), so a block this big
            // fails to link long before a draw - which makes refusing it a Fatal for a shape
            // nothing can produce. If the two ever disagree, the honest answer is the bytes
            // the device CAN name: everything past the clamp is outside the descriptor and
            // reads back as zero under robustBufferAccess, which is inside GL's "undefined"
            // for a block the implementation never promised to hold. Clamping keeps the range
            // constant across draws, so the descriptor-set reuse hash is unaffected.
            MGLOG_E_ONCE("ResolveWireUniformBufferPayload: reflected uniform block %u is %llu bytes, past this "
                         "device's maxUniformBufferRange of %llu; the binding is clamped to the limit and the "
                         "remainder reads zero",
                         blockIndex, static_cast<unsigned long long>(blockSize),
                         static_cast<unsigned long long>(m_wireMaxUniformRange));
            blockSize = m_wireMaxUniformRange;
        }
        const auto& range = MG_Pipe::MGPipeApplier().BoundShaderBuffers[
            MG_Pipe::kMGPipeShaderBufferClassUniform][bindingPoint];
        BufferSlice source{};
        VkDeviceSize start = 0, available = 0;
        const Bool bound = !MG_Pipe::MGPipeHandleIsNull(range.Res);
        if (bound) {
            if (!m_bufferManager->AcquireWireSlice(BufferKind::Uniform, range.Res, source) || !source.IsValid()) return false;
            if (!ResolveWireRange(range.Offset, range.Size, source.size, start, available)) return false;
            if (start > std::numeric_limits<VkDeviceSize>::max() - source.offset)
                WireDescriptorFatal("uniform-buffer-offset-overflow");
            const VkDeviceSize absoluteOffset = source.offset + start;
            if (available >= blockSize && absoluteOffset % m_minDynamicOffsetAlignment == 0 &&
                absoluteOffset <= std::numeric_limits<Uint32>::max()) {
                out.directBindable = true;
                out.buffer = source.buffer;
                out.range = blockSize;
                out.dynamicOffset = absoluteOffset;
                return true;
            }
        }

        // Preserve the existing short-range compatibility policy without sampling the
        // client's shadow: zero the complete reflected block, then GPU-copy only bytes
        // inside the GL binding range. A readback here could retire the command buffer
        // whose value BindProgramUniformBuffers is currently holding.
        const VkDeviceSize copied = std::min(available, blockSize);
        static thread_local Vector<Uint8> zero;
        zero.assign(static_cast<SizeT>(blockSize), 0);
        BufferSlice padded{};
        if (!m_bufferManager->UploadTransient(BufferKind::Uniform, m_wireFrameIndex, zero.data(), blockSize,
                std::max<VkDeviceSize>(4, m_minDynamicOffsetAlignment), padded) || !padded.IsValid()) return false;
        if (bound && copied != 0) {
            // P7 A.1 retires `uniform-buffer-byte-tail`. The window is word-aligned at both
            // ends on nearly every bind - glBindBufferRange already forces the offset onto
            // GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, and that is a multiple of four on every
            // device - but GL 4.6 core 6.1.1 puts no such rule on the SIZE, so `copied` is free
            // to end mid-word and the Redmi Minecraft run lands there
            // (notes/p5f/magma-inproc-fix.md #5). An unaligned start is not reachable through
            // the public API and only a protocol-level offset can produce one; the sub-word arm
            // handles it anyway rather than leaving a second shape to trip over later. Both arms
            // copy the same bytes to the same place: the sub-word one pays one extra transient
            // slice and one extra region copy, and nothing outside [start, start + copied)
            // reaches the block, which is what makes the padded zeros the visible tail.
            const Bool wordWindow = ((start | copied) & 3u) == 0;
            if (!(wordWindow
                      ? m_bufferManager->CopyWireBufferRangeToSlice(range.Res, start, copied, padded)
                      : m_bufferManager->CopyWireBufferSubWordRangeToSlice(range.Res, start, copied,
                                                                           m_wireFrameIndex, padded, 0)))
                return false;
        }
        if (padded.offset > std::numeric_limits<Uint32>::max()) {
            // P7 A.2, a decline (rule I (a)), not a session death. A Vulkan dynamic offset is a
            // uint32_t, so a transient uniform ring that grew past 4 GiB inside one frame has
            // nowhere to put this block - which is our ring's problem, not a protocol fault, and
            // nothing the peer could have sent differently. One binding is skipped and the draw
            // is lost; the session keeps running and the next frame's rewound ring resolves it.
            // The monolith arm has the same ceiling and truncates silently on the way out
            // (ResolveDynamicUboDescriptor's static_cast<Uint32>), i.e. it binds a WRONG window
            // rather than none, so declining is the strictly better observable of the two.
            MGLOG_E_ONCE("ResolveWireUniformBufferPayload: the transient uniform ring reached offset "
                         "%llu, past the 4 GiB a Vulkan dynamic offset can name; uniform binding "
                         "point %u is declined for this draw",
                         static_cast<unsigned long long>(padded.offset), bindingPoint);
            return false;
        }
        out.directBindable = true;
        out.buffer = padded.buffer;
        out.range = blockSize;
        out.dynamicOffset = padded.offset;
        return true;
    }

    Bool UniformManager::ResolveUniformBufferPayload(const MagmaProgramSource& program,
                                                     const ProgramFactory::VkProgramObject& programObj, Uint32 binding,
                                                     Uint32 arrayElement, UboBindResult& out) const {
        const void* outData = nullptr;
        VkDeviceSize outSize = 0;

        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "ResolveUniformBufferPayload: GL context is null");
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
        if (binding >= programObj.uniformBlockIndexByBinding.size())
            WireDescriptorFatal("uniform-buffer-reflection-binding");
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
        if ((blockIndex < 0 || static_cast<Uint32>(blockIndex) >= activeUniformBlockCount))
            WireDescriptorFatal("uniform-buffer-reflection-block");

        const Uint32 frontendBinding = program.GetUniformBlockBinding(static_cast<Uint32>(blockIndex));
        return ResolveWireUniformBufferPayload(program, static_cast<Uint32>(blockIndex), frontendBinding, out);
    }

    Bool UniformManager::CreateDescriptorPool(Uint32 maxSets, Bool updateAfterBind, VkDescriptorPool& outPool) const {
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
            MGLOG_E_ONCE("UniformDescriptorBinder::CreateDescriptorPool failed: descriptorCount overflow");
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
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT |
                         (updateAfterBind ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0);
        poolInfo.maxSets = maxSets;
        poolInfo.poolSizeCount = static_cast<Uint32>(std::size(poolSizes));
        poolInfo.pPoolSizes = poolSizes;

        const VkResult result = vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &outPool);
        if (result != VK_SUCCESS) {
            MGLOG_E_ONCE("UniformDescriptorBinder::CreateDescriptorPool failed: vkCreateDescriptorPool returned %d",
                    result);
            return false;
        }
        return true;
    }

    Uint32 UniformManager::NextDescriptorPoolMaxSets(const Vector<DescriptorPoolBucket>& pools, Bool updateAfterBind,
                                                     Uint32 initialMaxSets) {
        const Uint32 minSets = std::max<Uint32>(1, initialMaxSets);
        const Uint32 maxSets = std::max(minSets, kMaxDescriptorPoolSets);
        Uint32 largest = 0;
        for (const auto& pool : pools) {
            if (pool.updateAfterBind == updateAfterBind) {
                largest = std::max(largest, pool.maxSets);
            }
        }
        if (largest == 0) {
            return minSets;
        }
        const Uint64 doubled = static_cast<Uint64>(largest) * 2;
        return static_cast<Uint32>(std::clamp<Uint64>(doubled, minSets, maxSets));
    }

    Bool UniformManager::GrowFrameDescriptorPool(FrameResources& frame, Uint32 frameIndex, Bool updateAfterBind) {
        if (frame.descriptorPools.empty()) {
            return false;
        }

        Uint32 grownMaxSets = NextDescriptorPoolMaxSets(frame.descriptorPools, updateAfterBind, m_setsPerFrame);
        VkDescriptorPool grownPool = VK_NULL_HANDLE;
        // A large pool is a large driver allocation; if the device refuses one, settle for less
        // rather than failing the draw - halving down to the base size, where the old policy lived.
        while (!CreateDescriptorPool(grownMaxSets, updateAfterBind, grownPool)) {
            if (grownMaxSets <= m_setsPerFrame) {
                MGLOG_E_ONCE("UniformDescriptorBinder::GrowFrameDescriptorPool failed: cannot create grown pool (%u sets)",
                             grownMaxSets);
                return false;
            }
            grownMaxSets = std::max(m_setsPerFrame, grownMaxSets / 2);
        }

        frame.descriptorPools.push_back({grownPool, grownMaxSets, 0, updateAfterBind});
        frame.activeDescriptorPoolIndex = static_cast<Uint32>(frame.descriptorPools.size() - 1);
        frame.capacitySets += grownMaxSets;
        // Growing is the opposite of quiet: a trim must wait a full quiet run from here.
        frame.quietEpochs = 0;
        MGLOG_D("UniformDescriptorBinder: frame %u descriptor pool exhausted, grew a %u-set pool (updateAfterBind=%d), "
                "poolCount=%zu",
                frameIndex, grownMaxSets, updateAfterBind ? 1 : 0, frame.descriptorPools.size());
        return true;
    }

    VkResult UniformManager::AllocateDescriptorSetsFromActivePool(Uint32 frameIndex, const ProgramFactory::VkProgramObject& programObj, VkDescriptorSet& outDescriptorSet) {
        auto& frame = m_frames[frameIndex];
        const Bool updateAfterBind = programObj.usesUpdateAfterBind;
        if (frame.activeDescriptorPoolIndex >= frame.descriptorPools.size() ||
            frame.descriptorPools[frame.activeDescriptorPoolIndex].updateAfterBind != updateAfterBind ||
            frame.descriptorPools[frame.activeDescriptorPoolIndex].allocatedSets >=
                frame.descriptorPools[frame.activeDescriptorPoolIndex].maxSets) {
            const auto availableBucket = std::find_if(
                frame.descriptorPools.begin(), frame.descriptorPools.end(),
                [updateAfterBind](const DescriptorPoolBucket& candidate) {
                    return candidate.updateAfterBind == updateAfterBind && candidate.allocatedSets < candidate.maxSets;
                });
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
                if (!GrowFrameDescriptorPool(frame, frameIndex, programObj.usesUpdateAfterBind)) {
                    MGLOG_E_ONCE("UniformDescriptorBinder::AcquireDescriptorSet failed: descriptor pool growth failed");
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

    Bool UniformManager::ResolveDynamicUboDescriptor(const MagmaProgramSource& program,
                                                     const ProgramFactory::VkProgramObject& programObj,
                                                     Uint32 binding, Uint32 arrayElement, Uint32 frameIndex,
                                                     VkBuffer& outBuffer, VkDeviceSize& outRange,
                                                     Uint32& outDynamicOffset) {
        UboBindResult ubo{};
        const Bool hasPayload = ResolveUniformBufferPayload(program, programObj, binding, arrayElement, ubo);
        if (!hasPayload) return false;
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
        if (isGlobalUbo && MG_Util::PipeStats::Enabled()) {
            // Magma's half of stage-ubo-global, so the class means the same on both
            // backends. The memo hit above returns before this, so a frame that reuses the
            // slice correctly contributes nothing.
            MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageUboGlobal,
                                         static_cast<Uint64>(ubo.payloadSize));
        }
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

    Bool UniformManager::PrepareWireTextureResources(const MagmaProgramSource& program,
                                                      const ProgramFactory::VkProgramObject& programObj) {
        if (programObj.declinedDescriptors || m_textureManager == nullptr) {
            MGL_WIRE_DECLINE_AT(DescriptorDeclined, "the program carries a descriptor MobileGL could not resolve");
            return false;
        }
        const auto& state = MG_Pipe::MGPipeApplier();
        // Resolve writable images first: aliases sampled by the same draw must
        // see the final STORAGE-capable allocation before any view is built.
        for (const Bool storage : {true, false}) {
            const auto wanted = storage ? ProgramFactory::DescriptorBindingKind::StorageImage
                                        : ProgramFactory::DescriptorBindingKind::CombinedImageSampler;
            for (const Uint32 binding : programObj.activeBindings) {
                if (binding >= m_maxBindings) break;
                if (programObj.bindingKinds[binding] != wanted) continue;
                const Int baseLocation = programObj.samplerUniformLocationByBinding[binding];
                const Uint32 count = BindingDescriptorCount(programObj, binding);
                for (Uint32 element = 0; element < count; ++element) {
                    const Int location = baseLocation + static_cast<Int>(element);
                    if (baseLocation < 0 || !program.UniformLocationsAliasSameUniform(baseLocation, location)) {
                        MGL_WIRE_DECLINE_AT(SamplerLocationAlias,
                                            "binding %u element %u does not alias one sampler uniform", binding,
                                            element);
                        return false;
                    }
                    const Int unit = program.GetUniformSamplerOrImageUnitIndex(static_cast<Uint>(location));
                    if (unit < 0 || static_cast<Uint32>(unit) >=
                        (storage ? MG_Pipe::kMGPipeMaxImageUnits : MG_Pipe::kMGPipeMaxTextureUnits)) {
                        MGL_WIRE_DECLINE_AT(SamplerUnitRange, "sampler/image unit %d is out of range", unit);
                        return false;
                    }
                    const auto handle = storage ? state.BoundShaderImages[unit].Res
                                                : state.BoundSamplerViews[unit].Texture;
                    // Null is the wire's unbound/incomplete binding. Its native
                    // placeholder needs no resource upload or frontend allocation.
                    if (MG_Pipe::MGPipeHandleIsNull(handle)) continue;
                    // Nor does an image unit naming a (level, layer) its texture lacks: the
                    // descriptor resolve binds that same placeholder (GL 4.6 core 8.26), and
                    // preparing the texture would decline a storage-less one and lose the pass.
                    if (storage && WireShaderImageNamesNoTexel(*m_textureManager, state.BoundShaderImages[unit]))
                        continue;
                    const auto* resource = m_textureManager->SyncTextureResourceByHandle(handle, false, storage);
                    if (resource == nullptr) return false;
                    // Another session writes it: the renderer acquires it before this use.
                    if (resource->sharedImageId != 0 || resource->yuvImageId != 0)
                        m_textureManager->NoteSharedImageUse(*resource);
                }
            }
        }
        m_textureManager->FlushPendingUploads();
        return true;
    }

    Bool UniformManager::BindProgramUniformBuffers(VkCommandBuffer commandBuffer,
                                                             const MagmaProgramSource& program,
                                                             const ProgramFactory::VkProgramObject& programObj,
                                                             Uint32 frameIndex,
                                                             VkPipelineBindPoint bindPoint,
                                                             const SamplerBindingOverride* samplerBindingOverride,
                                                             Bool samplerDescriptorsUnchangedHint,
                                                             const Vector<SamplerBindingOverride>* samplerBindingOverrides) {
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
            MGLOG_E_ONCE("UniformDescriptorBinder::BindProgramUniformBuffers failed: frame descriptor pools are invalid");
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
        const Bool cacheable = samplerBindingOverride == nullptr &&
                               (samplerBindingOverrides == nullptr || samplerBindingOverrides->empty());
        if (cacheable && samplerDescriptorsUnchangedHint && m_fastRebindMemo.valid &&
            m_fastRebindMemo.frameIndex == frameIndex &&
            // A wire store destroyed since the memo was taken may have handed its handle
            // value to a later mint (see FastRebindMemo): "same VkBuffer" then names a
            // different store, so the memo is refused and the full walk re-records it.
            m_fastRebindMemo.wireStoreDestroyEpoch == m_bufferManager->GetWireStoreDestroyEpoch() &&
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
                    MGLOG_E_ONCE(
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
                    MGLOG_E_ONCE("UniformDescriptorBinder::BindProgramUniformBuffers failed: image buffer binding %u "
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
                        MGLOG_E_ONCE(
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
                        MGLOG_E_ONCE(
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
                    const SamplerBindingOverride* overrideForElement =
                        overrideThisBinding && element == 0 ? samplerBindingOverride : nullptr;
                    if (overrideForElement == nullptr && samplerBindingOverrides != nullptr) {
                        const auto overrideIt = std::find_if(
                            samplerBindingOverrides->begin(), samplerBindingOverrides->end(),
                            [binding, element](const SamplerBindingOverride& candidate) {
                                return candidate.binding == binding && candidate.element == element;
                            });
                        if (overrideIt != samplerBindingOverrides->end()) {
                            overrideForElement = &*overrideIt;
                        }
                    }
                    const Bool hasImage = overrideForElement != nullptr
                                              ? ResolveSamplerDescriptorOverride(*overrideForElement, imageInfo)
                                              : ResolveSamplerDescriptor(commandBuffer, program, programObj, binding,
                                                                         element, imageInfo,
                                                                         samplerDescriptorsUnchangedHint);
                    if (!hasImage) {
                        MGLOG_E_ONCE(
                            "UniformDescriptorBinder::BindProgramUniformBuffers failed: sampler binding %u element %u "
                            "has no valid texture descriptor",
                            binding, element);
                        return false;
                    }
                    if (imageInfo.sampler == VK_NULL_HANDLE || imageInfo.imageView == VK_NULL_HANDLE) {
                        MGLOG_E_ONCE(
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
            // P7 M2 round 2 (ID-P7-43): the buffer infos below name WIRE stores by VkBuffer
            // handle, and a wire store can be destroyed mid-frame (VkBufferManager::
            // DeferredWireRelease) with its handle value re-minted before this frame's memo
            // is cleared. Folding the destroy epoch in makes every entry taken before such a
            // destroy miss, so a byte-identical info can never revive a set baked to a dead
            // store. NOT the slice epoch: that moves on every glBufferSubData and would make
            // the memo miss on every draw.
            mix64(m_bufferManager->GetWireStoreDestroyEpoch());
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
                MGLOG_E_ONCE("UniformDescriptorBinder::BindProgramUniformBuffers failed: descriptor set acquire returned %d",
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
                bufferInfos[0].range,    descriptorSet,
                m_bufferManager->GetWireStoreDestroyEpoch(),
            };
        } else {
            m_fastRebindMemo.valid = false;
        }

        BindDescriptorSetDeduped(commandBuffer, bindPoint, programObj.pipelineLayout, descriptorSet,
                                 dynamicOffsets);
        return true;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
