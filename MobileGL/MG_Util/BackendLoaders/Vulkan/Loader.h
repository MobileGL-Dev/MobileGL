// MobileGL - MobileGL/MG_Util/BackendLoaders/Vulkan/Loader.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL {
    namespace MG_External {
        struct VulkanCapabilities {
            Version VulkanAPIVersion{1, 0, 0};
            String DeviceName;
            String DriverVersionString;
            Int UniformBufferOffsetAlignment = 256;
            Float AliasedLineWidthRangeMin = 1.0f;
            Float AliasedLineWidthRangeMax = 1.0f;
            // VkPhysicalDeviceLimits::maxSamplerAnisotropy. Whether it can be used at all depends on
            // the samplerAnisotropy feature, which the renderer decides at device creation.
            Float MaxSamplerAnisotropy = 1.0f;
            Float SmoothLineWidthRangeMin = 1.0f;
            Float SmoothLineWidthRangeMax = 1.0f;
            Float SmoothLineWidthGranularity = 1.0f;
            Float PointSizeRangeMin = 1.0f;
            Float PointSizeRangeMax = 1.0f;
            Float PointSizeGranularity = 1.0f;
            Int Max3DTextureSize = 16384;
            Int MaxArrayTextureLayers = 2048;
            Int MaxCubeMapTextureSize = 16384;
            Int MaxFramebufferWidth = 16384;
            Int MaxFramebufferHeight = 16384;
            Int MaxFramebufferLayers = 2048;
            Int MaxRenderbufferSize = 16384;
            Int MaxTextureSize = 16384;
            Int MaxColorTextureSamples = 1;
            Int MaxDepthTextureSamples = 1;
            Int MaxFramebufferSamples = 1;
            Int MaxIntegerSamples = 1;
            Int MaxSamples = 1;
            Int MaxSampleMaskWords = 1;
            Int MaxTextureImageUnits = 32;
            Int MaxVertexTextureImageUnits = 32;
            Int MaxComputeTextureImageUnits = 32;
            Int MaxCombinedTextureImageUnits = 192;
            Int MaxVertexAttribs = 16;
            Int MaxComputeShaderStorageBlocks = 8;
            Int MaxCombinedShaderStorageBlocks = 32;
            Int MaxComputeUniformBlocks = 12;
            Int MaxComputeWorkGroupInvocations = 128;
            Int MaxShaderStorageBufferBindings = 8;
            Int MaxTextureBufferSize = 65536;
            Int MaxUniformBufferBindings = 24;
            Int MaxUniformBlockSize = 16384;
            Int MaxImageUnits = 8;
            Int MaxCombinedImageUniforms = 8;
            Int MaxComputeImageUniforms = 8;
            Int MaxDrawBuffers = 8;
            Int MaxColorAttachments = 8;
            Int MaxClipDistances = 8;
            Int MaxViewports = 16;
            Int MaxViewportWidth = 16384;
            Int MaxViewportHeight = 16384;
            Float ViewportBoundsRangeMin = 0.0f;
            Float ViewportBoundsRangeMax = 0.0f;
            Int ViewportSubpixelBits = 0;
            Bool SupportsWideLines = false;
            // Storage-image descriptors are limited per stage by
            // maxPerStageDescriptorStorageImages, but writes/atomics outside compute additionally
            // require these core Vulkan features to be enabled on the logical device.
            Bool SupportsVertexPipelineStoresAndAtomics = false;
            Bool SupportsFragmentStoresAndAtomics = false;
            Bool SupportsGeometryShader = false;
            SizeT MaxShaderStorageBlockSize = 128 * 1024 * 1024;
            Bool SupportsShaderSubgroup = false;
            Uint32 SubgroupSize = 0;
            Uint32 SubgroupSupportedStages = 0;
            Uint32 SubgroupSupportedOperations = 0;
            Bool SubgroupQuadOperationsInAllStages = false;
        };
    } // namespace MG_External
    namespace MG_Util::BackendLoader {
        Bool QueryVulkanCapabilities(MobileGL::MG_External::VulkanCapabilities& caps, VkInstance instance,
                                     VkPhysicalDevice physicalDevice);
        void FillInVulkanCapabilities(MobileGL::MG_External::VulkanCapabilities& caps,
                                      VkPhysicalDeviceProperties properties);
    } // namespace MG_Util::BackendLoader
} // namespace MobileGL
