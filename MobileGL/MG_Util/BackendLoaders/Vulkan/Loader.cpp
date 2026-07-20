// MobileGL - MobileGL/MG_Util/BackendLoaders/Vulkan/Loader.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Loader.h"

#include <Config.h>

namespace MobileGL::MG_Util::BackendLoader {
    namespace {
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
        caps.Max3DTextureSize = static_cast<Int>(p.limits.maxImageDimension3D);
        caps.MaxArrayTextureLayers = static_cast<Int>(p.limits.maxImageArrayLayers);
        caps.MaxCubeMapTextureSize = static_cast<Int>(p.limits.maxImageDimensionCube);
        caps.MaxFramebufferWidth = static_cast<Int>(p.limits.maxFramebufferWidth);
        caps.MaxFramebufferHeight = static_cast<Int>(p.limits.maxFramebufferHeight);
        caps.MaxFramebufferLayers = static_cast<Int>(p.limits.maxFramebufferLayers);
        caps.MaxRenderbufferSize = ResolveMaxRenderbufferSize(p.limits);
        caps.MaxTextureSize = static_cast<Int>(p.limits.maxImageDimension2D);
        caps.MaxColorTextureSamples = MaxSampleCountFromFlags(p.limits.sampledImageColorSampleCounts);
        caps.MaxDepthTextureSamples = MaxSampleCountFromFlags(p.limits.sampledImageDepthSampleCounts);
        caps.MaxFramebufferSamples = ResolveConservativeFramebufferSampleLimit(p.limits);
        caps.MaxIntegerSamples = MaxSampleCountFromFlags(p.limits.sampledImageIntegerSampleCounts);
        caps.MaxSamples = caps.MaxFramebufferSamples;
        caps.MaxSampleMaskWords = static_cast<Int>(p.limits.maxSampleMaskWords);
        caps.MaxTextureImageUnits = static_cast<Int>(p.limits.maxPerStageDescriptorSampledImages);
        caps.MaxVertexTextureImageUnits = static_cast<Int>(p.limits.maxPerStageDescriptorSampledImages);
        caps.MaxComputeTextureImageUnits = static_cast<Int>(p.limits.maxPerStageDescriptorSampledImages);
        caps.MaxCombinedTextureImageUnits = static_cast<Int>(p.limits.maxDescriptorSetSampledImages);
        caps.MaxVertexAttribs = static_cast<Int>(p.limits.maxVertexInputAttributes);
        caps.MaxComputeShaderStorageBlocks = static_cast<Int>(p.limits.maxPerStageDescriptorStorageBuffers);
        caps.MaxCombinedShaderStorageBlocks = static_cast<Int>(p.limits.maxDescriptorSetStorageBuffers);
        caps.MaxComputeUniformBlocks = static_cast<Int>(p.limits.maxPerStageDescriptorUniformBuffers);
        caps.MaxComputeWorkGroupInvocations = static_cast<Int>(p.limits.maxComputeWorkGroupInvocations);
        caps.MaxShaderStorageBufferBindings = static_cast<Int>(p.limits.maxDescriptorSetStorageBuffers);
        caps.MaxTextureBufferSize = static_cast<Int>(p.limits.maxTexelBufferElements);
        caps.MaxUniformBufferBindings = static_cast<Int>(p.limits.maxDescriptorSetUniformBuffers);
        caps.MaxUniformBlockSize = static_cast<Int>(p.limits.maxUniformBufferRange);
        caps.MaxImageUnits = static_cast<Int>(p.limits.maxPerStageDescriptorStorageImages);
        caps.MaxCombinedImageUniforms = static_cast<Int>(p.limits.maxDescriptorSetStorageImages);
        caps.MaxComputeImageUniforms = static_cast<Int>(p.limits.maxPerStageDescriptorStorageImages);
        caps.MaxDrawBuffers = static_cast<Int>(p.limits.maxFragmentOutputAttachments);
        caps.MaxColorAttachments = static_cast<Int>(p.limits.maxColorAttachments);
        caps.MaxClipDistances = static_cast<Int>(p.limits.maxClipDistances);
        caps.MaxViewports = static_cast<Int>(p.limits.maxViewports);
        caps.MaxViewportWidth = static_cast<Int>(p.limits.maxViewportDimensions[0]);
        caps.MaxViewportHeight = static_cast<Int>(p.limits.maxViewportDimensions[1]);
        caps.ViewportBoundsRangeMin = p.limits.viewportBoundsRange[0];
        caps.ViewportBoundsRangeMax = p.limits.viewportBoundsRange[1];
        caps.ViewportSubpixelBits = static_cast<Int>(p.limits.viewportSubPixelBits);

        VkPhysicalDeviceFeatures supportedFeatures{};
        vkGetPhysicalDeviceFeatures(physicalDevice, &supportedFeatures);
        caps.SupportsWideLines = supportedFeatures.wideLines == VK_TRUE;
        caps.SupportsVertexPipelineStoresAndAtomics =
            supportedFeatures.vertexPipelineStoresAndAtomics == VK_TRUE;
        caps.SupportsFragmentStoresAndAtomics = supportedFeatures.fragmentStoresAndAtomics == VK_TRUE;
        caps.SupportsGeometryShader = supportedFeatures.geometryShader == VK_TRUE;
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
        caps.Max3DTextureSize = static_cast<Int>(properties.limits.maxImageDimension3D);
        caps.MaxArrayTextureLayers = static_cast<Int>(properties.limits.maxImageArrayLayers);
        caps.MaxCubeMapTextureSize = static_cast<Int>(properties.limits.maxImageDimensionCube);
        caps.MaxFramebufferWidth = static_cast<Int>(properties.limits.maxFramebufferWidth);
        caps.MaxFramebufferHeight = static_cast<Int>(properties.limits.maxFramebufferHeight);
        caps.MaxFramebufferLayers = static_cast<Int>(properties.limits.maxFramebufferLayers);
        caps.MaxRenderbufferSize = ResolveMaxRenderbufferSize(properties.limits);
        caps.MaxTextureSize = static_cast<Int>(properties.limits.maxImageDimension2D);
        caps.MaxColorTextureSamples = MaxSampleCountFromFlags(properties.limits.sampledImageColorSampleCounts);
        caps.MaxDepthTextureSamples = MaxSampleCountFromFlags(properties.limits.sampledImageDepthSampleCounts);
        caps.MaxFramebufferSamples = ResolveConservativeFramebufferSampleLimit(properties.limits);
        caps.MaxIntegerSamples = MaxSampleCountFromFlags(properties.limits.sampledImageIntegerSampleCounts);
        caps.MaxSamples = caps.MaxFramebufferSamples;
        caps.MaxSampleMaskWords = static_cast<Int>(properties.limits.maxSampleMaskWords);
        caps.MaxTextureImageUnits = static_cast<Int>(properties.limits.maxPerStageDescriptorSampledImages);
        caps.MaxVertexTextureImageUnits = static_cast<Int>(properties.limits.maxPerStageDescriptorSampledImages);
        caps.MaxComputeTextureImageUnits = static_cast<Int>(properties.limits.maxPerStageDescriptorSampledImages);
        caps.MaxCombinedTextureImageUnits = static_cast<Int>(properties.limits.maxDescriptorSetSampledImages);
        caps.MaxVertexAttribs = static_cast<Int>(properties.limits.maxVertexInputAttributes);
        caps.MaxComputeShaderStorageBlocks = static_cast<Int>(properties.limits.maxPerStageDescriptorStorageBuffers);
        caps.MaxCombinedShaderStorageBlocks = static_cast<Int>(properties.limits.maxDescriptorSetStorageBuffers);
        caps.MaxComputeUniformBlocks = static_cast<Int>(properties.limits.maxPerStageDescriptorUniformBuffers);
        caps.MaxComputeWorkGroupInvocations = static_cast<Int>(properties.limits.maxComputeWorkGroupInvocations);
        caps.MaxShaderStorageBufferBindings = static_cast<Int>(properties.limits.maxDescriptorSetStorageBuffers);
        caps.MaxTextureBufferSize = static_cast<Int>(properties.limits.maxTexelBufferElements);
        caps.MaxUniformBufferBindings = static_cast<Int>(properties.limits.maxDescriptorSetUniformBuffers);
        caps.MaxUniformBlockSize = static_cast<Int>(properties.limits.maxUniformBufferRange);
        caps.MaxImageUnits = static_cast<Int>(properties.limits.maxPerStageDescriptorStorageImages);
        caps.MaxCombinedImageUniforms = static_cast<Int>(properties.limits.maxDescriptorSetStorageImages);
        caps.MaxComputeImageUniforms = static_cast<Int>(properties.limits.maxPerStageDescriptorStorageImages);
        caps.MaxDrawBuffers = static_cast<Int>(properties.limits.maxFragmentOutputAttachments);
        caps.MaxColorAttachments = static_cast<Int>(properties.limits.maxColorAttachments);
        caps.MaxClipDistances = static_cast<Int>(properties.limits.maxClipDistances);
        caps.MaxViewports = static_cast<Int>(properties.limits.maxViewports);
        caps.MaxViewportWidth = static_cast<Int>(properties.limits.maxViewportDimensions[0]);
        caps.MaxViewportHeight = static_cast<Int>(properties.limits.maxViewportDimensions[1]);
        caps.ViewportBoundsRangeMin = properties.limits.viewportBoundsRange[0];
        caps.ViewportBoundsRangeMax = properties.limits.viewportBoundsRange[1];
        caps.ViewportSubpixelBits = static_cast<Int>(properties.limits.viewportSubPixelBits);
        caps.SupportsWideLines = false;
        // This helper only receives properties, not VkPhysicalDeviceFeatures. Leave optional
        // stage writes disabled rather than inferring them from descriptor limits alone.
        caps.SupportsVertexPipelineStoresAndAtomics = false;
        caps.SupportsFragmentStoresAndAtomics = false;
        caps.SupportsGeometryShader = false;
        caps.MaxShaderStorageBlockSize = static_cast<SizeT>(properties.limits.maxStorageBufferRange);
        caps.SupportsShaderSubgroup = false;
        caps.SubgroupSize = 0;
        caps.SubgroupSupportedStages = 0;
        caps.SubgroupSupportedOperations = 0;
        caps.SubgroupQuadOperationsInAllStages = false;
    }
} // namespace MobileGL::MG_Util::BackendLoader
