// MobileGL - MobileGL/MG_Util/BackendLoaders/Vulkan/Loader.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Loader.h"

#include <Config.h>
#include <cmath>
#include <limits>

namespace MobileGL::MG_Util::BackendLoader {
    namespace {
        // A Vulkan limit is an unsigned 32-bit count; a GL limit is a signed Int. Drivers do report
        // values with the top bit set (UINT32_MAX is the idiomatic "effectively unlimited"), and a
        // plain static_cast turned those into small negatives - which every downstream std::min or
        // ceiling comparison then accepted as "already small enough". Saturate instead, so a clamp
        // above this can be trusted to be the only thing that lowers a limit.
        Int SaturateToInt(Uint32 value) {
            constexpr Uint32 kMaxInt = static_cast<Uint32>(std::numeric_limits<Int>::max());
            return static_cast<Int>(std::min<Uint32>(value, kMaxInt));
        }

        struct VulkanDynamicFunctions {
            PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
            PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 = nullptr;
        };

        Int ResolveMaxRenderbufferSize(const VkPhysicalDeviceLimits& limits) {
            return std::min<Int>(static_cast<Int>(limits.maxImageDimension2D),
                                 std::min<Int>(static_cast<Int>(limits.maxFramebufferWidth),
                                               static_cast<Int>(limits.maxFramebufferHeight)));
        }

        Int MaxSampleCountFromFlags(VkSampleCountFlags flags) {
            if (flags & VK_SAMPLE_COUNT_64_BIT) return 64;
            if (flags & VK_SAMPLE_COUNT_32_BIT) return 32;
            if (flags & VK_SAMPLE_COUNT_16_BIT) return 16;
            if (flags & VK_SAMPLE_COUNT_8_BIT) return 8;
            if (flags & VK_SAMPLE_COUNT_4_BIT) return 4;
            if (flags & VK_SAMPLE_COUNT_2_BIT) return 2;
            return 1;
        }

        Int ResolveConservativeFramebufferSampleLimit(const VkPhysicalDeviceLimits& limits) {
            const VkSampleCountFlags commonFlags = limits.framebufferColorSampleCounts &
                                                   limits.framebufferDepthSampleCounts &
                                                   limits.framebufferStencilSampleCounts;
            return MaxSampleCountFromFlags(commonFlags);
        }

        void FillFragmentInterpolationLimits(MG_External::VulkanCapabilities& caps,
                                             const VkPhysicalDeviceLimits& limits) {
            caps.MinFragmentInterpolationOffset =
                std::isfinite(limits.minInterpolationOffset) && limits.minInterpolationOffset <= -0.5f
                    ? limits.minInterpolationOffset
                    : -0.5f;

            caps.MaxFragmentInterpolationOffset = 0.4375f;
            caps.FragmentInterpolationOffsetBits = 4;
            const Int bits = static_cast<Int>(limits.subPixelInterpolationOffsetBits);
            if (bits >= 4 && std::isfinite(limits.maxInterpolationOffset)) {
                const Float requiredMaxOffset = 0.5f - std::ldexp(1.0f, -bits);
                if (limits.maxInterpolationOffset >= requiredMaxOffset) {
                    caps.MaxFragmentInterpolationOffset = limits.maxInterpolationOffset;
                    caps.FragmentInterpolationOffsetBits = bits;
                }
            }
        }

        VulkanDynamicFunctions LoadVulkanDynamicFunctions(VkInstance instance) {
            VulkanDynamicFunctions loaded{};
            if (instance == VK_NULL_HANDLE) {
                MGLOG_E("Cannot load Vulkan instance-level function pointers: VkInstance is null");
                return loaded;
            }

            loaded.vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"));
            loaded.vkGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));

            if (!loaded.vkGetPhysicalDeviceProperties) {
                MGLOG_E("Failed to resolve vkGetPhysicalDeviceProperties via vkGetInstanceProcAddr");
            }

            return loaded;
        }

        Bool IsShaderSubgroupForcedDisabled() {
            return MG_Config::Features.DisableSubgroup;
        }
    } // namespace

    inline Version DecodeApiVersion(uint32_t version) {
        return {(Int)VK_VERSION_MAJOR(version), (Int)VK_VERSION_MINOR(version), (Int)VK_VERSION_PATCH(version)};
    }

    inline std::string DecodeDriverVersion(uint32_t driverVersion) {
        std::ostringstream oss;
        oss << VK_VERSION_MAJOR(driverVersion) << "." << VK_VERSION_MINOR(driverVersion) << "."
            << VK_VERSION_PATCH(driverVersion);
        return oss.str();
    }

    inline Bool HasUsableShaderSubgroupSupport(const VkPhysicalDeviceSubgroupProperties& subgroupProps) {
        return subgroupProps.subgroupSize > 0 &&
               (subgroupProps.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0 &&
               (subgroupProps.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) != 0;
    }

    Bool QueryVulkanCapabilities(MobileGL::MG_External::VulkanCapabilities& caps, VkInstance instance,
                                 VkPhysicalDevice physicalDevice) {
        if (!physicalDevice) {
            MGLOG_E("Invalid physical device handle");
            return false;
        }

        const auto vk = LoadVulkanDynamicFunctions(instance);
        if (!vk.vkGetPhysicalDeviceProperties) {
            MGLOG_E("Vulkan dynamic loading failed for vkGetPhysicalDeviceProperties");
            return false;
        }

        VkPhysicalDeviceProperties props{};
        vk.vkGetPhysicalDeviceProperties(physicalDevice, &props);

        if (props.apiVersion < VK_API_VERSION_1_1) {
            MGLOG_E("Vulkan API version %u.%u.%u is not supported, requires at least 1.1",
                    VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                    VK_VERSION_PATCH(props.apiVersion));
            return false;
        }

        VkPhysicalDeviceSubgroupProperties subgroupProps{};
        subgroupProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;

        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &subgroupProps;
        if (vk.vkGetPhysicalDeviceProperties2) {
            vk.vkGetPhysicalDeviceProperties2(physicalDevice, &props2);
        } else {
            MGLOG_W("vkGetPhysicalDeviceProperties2 not available, falling back to vkGetPhysicalDeviceProperties");
            props2.properties = props;
        }

        const VkPhysicalDeviceProperties& p = props2.properties;
        caps.VulkanAPIVersion = DecodeApiVersion(p.apiVersion);
        caps.DeviceName = p.deviceName;
        caps.DriverVersionString = DecodeDriverVersion(p.driverVersion);
        caps.VendorId = p.vendorID;
        caps.UniformBufferOffsetAlignment = static_cast<int>(p.limits.minUniformBufferOffsetAlignment);
        caps.AliasedLineWidthRangeMin = p.limits.lineWidthRange[0];
        caps.AliasedLineWidthRangeMax = p.limits.lineWidthRange[1];
        caps.MaxSamplerAnisotropy = p.limits.maxSamplerAnisotropy;
        caps.SmoothLineWidthRangeMin = p.limits.lineWidthRange[0];
        caps.SmoothLineWidthRangeMax = p.limits.lineWidthRange[1];
        caps.SmoothLineWidthGranularity = p.limits.lineWidthGranularity;
        caps.PointSizeRangeMin = p.limits.pointSizeRange[0];
        caps.PointSizeRangeMax = p.limits.pointSizeRange[1];
        caps.PointSizeGranularity = p.limits.pointSizeGranularity;
        caps.Max3DTextureSize = SaturateToInt(p.limits.maxImageDimension3D);
        caps.MaxArrayTextureLayers = SaturateToInt(p.limits.maxImageArrayLayers);
        caps.MaxCubeMapTextureSize = SaturateToInt(p.limits.maxImageDimensionCube);
        caps.MaxFramebufferWidth = SaturateToInt(p.limits.maxFramebufferWidth);
        caps.MaxFramebufferHeight = SaturateToInt(p.limits.maxFramebufferHeight);
        caps.MaxFramebufferLayers = SaturateToInt(p.limits.maxFramebufferLayers);
        caps.MaxRenderbufferSize = ResolveMaxRenderbufferSize(p.limits);
        caps.MaxTextureSize = SaturateToInt(p.limits.maxImageDimension2D);
        caps.MaxColorTextureSamples = MaxSampleCountFromFlags(p.limits.sampledImageColorSampleCounts);
        caps.MaxDepthTextureSamples = MaxSampleCountFromFlags(p.limits.sampledImageDepthSampleCounts);
        caps.MaxFramebufferSamples = ResolveConservativeFramebufferSampleLimit(p.limits);
        caps.MaxIntegerSamples = MaxSampleCountFromFlags(p.limits.sampledImageIntegerSampleCounts);
        caps.MaxSamples = caps.MaxFramebufferSamples;
        caps.MaxSampleMaskWords = SaturateToInt(p.limits.maxSampleMaskWords);
        caps.MaxTextureImageUnits = SaturateToInt(p.limits.maxPerStageDescriptorSampledImages);
        caps.MaxVertexTextureImageUnits = SaturateToInt(p.limits.maxPerStageDescriptorSampledImages);
        caps.MaxComputeTextureImageUnits = SaturateToInt(p.limits.maxPerStageDescriptorSampledImages);
        caps.MaxCombinedTextureImageUnits = SaturateToInt(p.limits.maxDescriptorSetSampledImages);
        caps.MaxVertexAttribs = SaturateToInt(p.limits.maxVertexInputAttributes);
        caps.MaxComputeShaderStorageBlocks = SaturateToInt(p.limits.maxPerStageDescriptorStorageBuffers);
        caps.MaxCombinedShaderStorageBlocks = SaturateToInt(p.limits.maxDescriptorSetStorageBuffers);
        caps.MaxComputeUniformBlocks = SaturateToInt(p.limits.maxPerStageDescriptorUniformBuffers);
        caps.MaxComputeWorkGroupInvocations = SaturateToInt(p.limits.maxComputeWorkGroupInvocations);
        caps.MaxShaderStorageBufferBindings = SaturateToInt(p.limits.maxDescriptorSetStorageBuffers);
        caps.MaxTextureBufferSize = SaturateToInt(p.limits.maxTexelBufferElements);
        caps.TextureBufferOffsetAlignment =
            static_cast<Int>(std::max<VkDeviceSize>(1, p.limits.minTexelBufferOffsetAlignment));
        caps.MaxUniformBufferBindings = SaturateToInt(p.limits.maxDescriptorSetUniformBuffers);
        caps.MaxUniformBlockSize = SaturateToInt(p.limits.maxUniformBufferRange);
        caps.MaxImageUnits = SaturateToInt(p.limits.maxPerStageDescriptorStorageImages);
        caps.MaxCombinedImageUniforms = SaturateToInt(p.limits.maxDescriptorSetStorageImages);
        caps.MaxComputeImageUniforms = SaturateToInt(p.limits.maxPerStageDescriptorStorageImages);
        caps.MaxDrawBuffers = SaturateToInt(p.limits.maxFragmentOutputAttachments);
        caps.MaxColorAttachments = SaturateToInt(p.limits.maxColorAttachments);
        caps.MaxClipDistances = SaturateToInt(p.limits.maxClipDistances);
        caps.MaxViewports = SaturateToInt(p.limits.maxViewports);
        caps.MaxViewportWidth = SaturateToInt(p.limits.maxViewportDimensions[0]);
        caps.MaxViewportHeight = SaturateToInt(p.limits.maxViewportDimensions[1]);
        caps.ViewportBoundsRangeMin = p.limits.viewportBoundsRange[0];
        caps.ViewportBoundsRangeMax = p.limits.viewportBoundsRange[1];
        caps.ViewportSubpixelBits = SaturateToInt(p.limits.viewportSubPixelBits);
        FillFragmentInterpolationLimits(caps, p.limits);

        VkPhysicalDeviceFeatures supportedFeatures{};
        vkGetPhysicalDeviceFeatures(physicalDevice, &supportedFeatures);
        caps.SupportsWideLines = supportedFeatures.wideLines == VK_TRUE;
        caps.SupportsShaderFloat64 = supportedFeatures.shaderFloat64 == VK_TRUE;
        caps.SupportsImageCubeArray = supportedFeatures.imageCubeArray == VK_TRUE;
        {
            // Probe the formats a colour render target actually uses. A driver that refuses the flag
            // for one of them refuses per-slice attachment for that format only, which
            // VkTextureManager detects and records at image creation; this field just says whether
            // the capability is worth offering at all.
            static constexpr VkFormat k3DSliceProbeFormats[] = {VK_FORMAT_R8G8B8A8_UNORM,
                                                                VK_FORMAT_R8G8B8A8_SRGB};
            Bool all2DArrayCompatible = true;
            for (const VkFormat probeFormat : k3DSliceProbeFormats) {
                VkImageFormatProperties probeProperties{};
                const VkResult probeResult = vkGetPhysicalDeviceImageFormatProperties(
                    physicalDevice, probeFormat, VK_IMAGE_TYPE_3D, VK_IMAGE_TILING_OPTIMAL,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT, &probeProperties);
                if (probeResult != VK_SUCCESS) {
                    all2DArrayCompatible = false;
                    break;
                }
            }
            caps.Supports2DArrayCompatible3DImages = all2DArrayCompatible;
        }
        caps.SupportsVertexPipelineStoresAndAtomics =
            supportedFeatures.vertexPipelineStoresAndAtomics == VK_TRUE;
        caps.SupportsFragmentStoresAndAtomics = supportedFeatures.fragmentStoresAndAtomics == VK_TRUE;
        caps.SupportsGeometryShader = supportedFeatures.geometryShader == VK_TRUE;
        caps.SupportsShaderClipDistance = supportedFeatures.shaderClipDistance == VK_TRUE;
        caps.MaxShaderStorageBlockSize = static_cast<SizeT>(p.limits.maxStorageBufferRange);
        const Bool supportsShaderSubgroup = vk.vkGetPhysicalDeviceProperties2 &&
                                            HasUsableShaderSubgroupSupport(subgroupProps);
        const Bool forceDisableShaderSubgroup = IsShaderSubgroupForcedDisabled();
        caps.SupportsShaderSubgroup = supportsShaderSubgroup && !forceDisableShaderSubgroup;
        if (caps.SupportsShaderSubgroup) {
            caps.SubgroupSize = subgroupProps.subgroupSize;
            caps.SubgroupSupportedStages = subgroupProps.supportedStages;
            caps.SubgroupSupportedOperations = subgroupProps.supportedOperations;
            caps.SubgroupQuadOperationsInAllStages = subgroupProps.quadOperationsInAllStages == VK_TRUE;
        } else {
            caps.SubgroupSize = 0;
            caps.SubgroupSupportedStages = 0;
            caps.SubgroupSupportedOperations = 0;
            caps.SubgroupQuadOperationsInAllStages = false;
        }

        MGLOG_I("Vulkan shader subgroup support: detected=%s advertised=%s size=%u stages=0x%x operations=0x%x",
                supportsShaderSubgroup ? "true" : "false", caps.SupportsShaderSubgroup ? "true" : "false",
                subgroupProps.subgroupSize, subgroupProps.supportedStages, subgroupProps.supportedOperations);
        if (supportsShaderSubgroup && forceDisableShaderSubgroup) {
            MGLOG_W("Vulkan shader subgroup support forced off by MOBILEGL_DISABLE_SUBGROUP");
        }

        return true;
    }

    void FillInVulkanCapabilities(MobileGL::MG_External::VulkanCapabilities& caps,
                                  VkPhysicalDeviceProperties properties) {
        caps.VulkanAPIVersion = DecodeApiVersion(properties.apiVersion);
        caps.DeviceName = properties.deviceName;
        caps.DriverVersionString = DecodeDriverVersion(properties.driverVersion);
        caps.VendorId = properties.vendorID;
        caps.UniformBufferOffsetAlignment = static_cast<int>(properties.limits.minUniformBufferOffsetAlignment);
        caps.AliasedLineWidthRangeMin = properties.limits.lineWidthRange[0];
        caps.AliasedLineWidthRangeMax = properties.limits.lineWidthRange[1];
        caps.MaxSamplerAnisotropy = properties.limits.maxSamplerAnisotropy;
        caps.SmoothLineWidthRangeMin = properties.limits.lineWidthRange[0];
        caps.SmoothLineWidthRangeMax = properties.limits.lineWidthRange[1];
        caps.SmoothLineWidthGranularity = properties.limits.lineWidthGranularity;
        caps.PointSizeRangeMin = properties.limits.pointSizeRange[0];
        caps.PointSizeRangeMax = properties.limits.pointSizeRange[1];
        caps.PointSizeGranularity = properties.limits.pointSizeGranularity;
        caps.Max3DTextureSize = SaturateToInt(properties.limits.maxImageDimension3D);
        caps.MaxArrayTextureLayers = SaturateToInt(properties.limits.maxImageArrayLayers);
        caps.MaxCubeMapTextureSize = SaturateToInt(properties.limits.maxImageDimensionCube);
        caps.MaxFramebufferWidth = SaturateToInt(properties.limits.maxFramebufferWidth);
        caps.MaxFramebufferHeight = SaturateToInt(properties.limits.maxFramebufferHeight);
        caps.MaxFramebufferLayers = SaturateToInt(properties.limits.maxFramebufferLayers);
        caps.MaxRenderbufferSize = ResolveMaxRenderbufferSize(properties.limits);
        caps.MaxTextureSize = SaturateToInt(properties.limits.maxImageDimension2D);
        caps.MaxColorTextureSamples = MaxSampleCountFromFlags(properties.limits.sampledImageColorSampleCounts);
        caps.MaxDepthTextureSamples = MaxSampleCountFromFlags(properties.limits.sampledImageDepthSampleCounts);
        caps.MaxFramebufferSamples = ResolveConservativeFramebufferSampleLimit(properties.limits);
        caps.MaxIntegerSamples = MaxSampleCountFromFlags(properties.limits.sampledImageIntegerSampleCounts);
        caps.MaxSamples = caps.MaxFramebufferSamples;
        caps.MaxSampleMaskWords = SaturateToInt(properties.limits.maxSampleMaskWords);
        caps.MaxTextureImageUnits = SaturateToInt(properties.limits.maxPerStageDescriptorSampledImages);
        caps.MaxVertexTextureImageUnits = SaturateToInt(properties.limits.maxPerStageDescriptorSampledImages);
        caps.MaxComputeTextureImageUnits = SaturateToInt(properties.limits.maxPerStageDescriptorSampledImages);
        caps.MaxCombinedTextureImageUnits = SaturateToInt(properties.limits.maxDescriptorSetSampledImages);
        caps.MaxVertexAttribs = SaturateToInt(properties.limits.maxVertexInputAttributes);
        caps.MaxComputeShaderStorageBlocks = SaturateToInt(properties.limits.maxPerStageDescriptorStorageBuffers);
        caps.MaxCombinedShaderStorageBlocks = SaturateToInt(properties.limits.maxDescriptorSetStorageBuffers);
        caps.MaxComputeUniformBlocks = SaturateToInt(properties.limits.maxPerStageDescriptorUniformBuffers);
        caps.MaxComputeWorkGroupInvocations = SaturateToInt(properties.limits.maxComputeWorkGroupInvocations);
        caps.MaxShaderStorageBufferBindings = SaturateToInt(properties.limits.maxDescriptorSetStorageBuffers);
        caps.MaxTextureBufferSize = SaturateToInt(properties.limits.maxTexelBufferElements);
        caps.TextureBufferOffsetAlignment =
            static_cast<Int>(std::max<VkDeviceSize>(1, properties.limits.minTexelBufferOffsetAlignment));
        caps.MaxUniformBufferBindings = SaturateToInt(properties.limits.maxDescriptorSetUniformBuffers);
        caps.MaxUniformBlockSize = SaturateToInt(properties.limits.maxUniformBufferRange);
        caps.MaxImageUnits = SaturateToInt(properties.limits.maxPerStageDescriptorStorageImages);
        caps.MaxCombinedImageUniforms = SaturateToInt(properties.limits.maxDescriptorSetStorageImages);
        caps.MaxComputeImageUniforms = SaturateToInt(properties.limits.maxPerStageDescriptorStorageImages);
        caps.MaxDrawBuffers = SaturateToInt(properties.limits.maxFragmentOutputAttachments);
        caps.MaxColorAttachments = SaturateToInt(properties.limits.maxColorAttachments);
        caps.MaxClipDistances = SaturateToInt(properties.limits.maxClipDistances);
        caps.MaxViewports = SaturateToInt(properties.limits.maxViewports);
        caps.MaxViewportWidth = SaturateToInt(properties.limits.maxViewportDimensions[0]);
        caps.MaxViewportHeight = SaturateToInt(properties.limits.maxViewportDimensions[1]);
        caps.ViewportBoundsRangeMin = properties.limits.viewportBoundsRange[0];
        caps.ViewportBoundsRangeMax = properties.limits.viewportBoundsRange[1];
        caps.ViewportSubpixelBits = SaturateToInt(properties.limits.viewportSubPixelBits);
        FillFragmentInterpolationLimits(caps, properties.limits);
        caps.SupportsWideLines = false;
        caps.SupportsShaderFloat64 = false;
        caps.SupportsImageCubeArray = false;
        caps.Supports2DArrayCompatible3DImages = false;
        // This helper only receives properties, not VkPhysicalDeviceFeatures. Leave optional
        // stage writes disabled rather than inferring them from descriptor limits alone.
        caps.SupportsVertexPipelineStoresAndAtomics = false;
        caps.SupportsFragmentStoresAndAtomics = false;
        caps.SupportsGeometryShader = false;
        caps.SupportsShaderClipDistance = false;
        caps.MaxShaderStorageBlockSize = static_cast<SizeT>(properties.limits.maxStorageBufferRange);
        caps.SupportsShaderSubgroup = false;
        caps.SubgroupSize = 0;
        caps.SubgroupSupportedStages = 0;
        caps.SubgroupSupportedOperations = 0;
        caps.SubgroupQuadOperationsInAllStages = false;
    }
} // namespace MobileGL::MG_Util::BackendLoader
