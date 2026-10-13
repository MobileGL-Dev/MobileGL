// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireE5RawCopy.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
#pragma once

#include <Config.h>
#include <Includes.h>

#include <vulkan/vulkan_core.h>

// THE RAW E5B9G9R9 READ ROUTE, and the probe that decides whether a device needs it.
//
// GL makes RGB9_E5 a bit-exact format wherever it moves texel blocks: glGetTexImage with
// GL_UNSIGNED_INT_5_9_9_9_REV hands back the words the application stored, and
// glCopyImageSubData is a raw block move (GL 4.6 core 18.3.2). RGB9_E5 has redundant encodings -
// a shared exponent one higher with every mantissa halved is the same value - so a path that
// decodes and re-encodes keeps every value and still changes words.
//
// Adreno 750 does exactly that on TRANSFER READS of a VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 image once
// the level is not a power-of-two size: vkCmdCopyImageToBuffer and vkCmdCopyImage out of a 30x30
// level return every non-canonical word canonicalized, while a 16x16 or 64x64 level reads back verbatim.
// The stored words are intact - a shader reading the same image through an R32_UINT view sees
// every one of them - and so are transfer WRITES into it. Measured with format lists, without
// the colour-attachment usage and with storage usage: none of them changes the answer, and the
// device has no VK_EXT_image_compression_control to ask with.
//
// The route therefore reads through a shader: a compute pass fetches the source through an
// R32_UINT view (the wire's E5B9G9R9 textures are MUTABLE_FORMAT from birth, see
// VkTextureManager) and stores the words into an R32_UINT twin, whose transfer reads move bits.
// The caller then reads the twin where it would have read the source.
//
// Whether a device needs it is MEASURED (RunWireE5TransferProbe below), at Magma bring-up
// (VulkanRenderer::ArmWireE5RawReads) and in the POST's "Known Driver Bugs" section
// (MG_Util/SelfTest/DriverBugProbes.cpp); a device that reads E5B9G9R9 back verbatim keeps its
// plain transfers.
namespace MobileGL::MG_Backend::DirectVulkan {
    // Device objects that do not depend on any image.
    struct WireE5RawCopyKit {
        VkShaderModule arrayModule = VK_NULL_HANDLE;
        VkShaderModule volumeModule = VK_NULL_HANDLE;
        VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkPipeline arrayPipeline = VK_NULL_HANDLE;
        VkPipeline volumePipeline = VK_NULL_HANDLE;
        VkSampler nearest = VK_NULL_HANDLE;
    };
    // Creates every member; on failure the members made so far stay set, for DestroyWireE5RawCopyKit.
    VkResult CreateWireE5RawCopyKit(VkDevice device, WireE5RawCopyKit& kit);
    void DestroyWireE5RawCopyKit(VkDevice device, WireE5RawCopyKit& kit);

    // Whether the device can run the route at all: R32_UINT sampled and storage on optimal tiling.
    Bool WireE5RawCopySupported(VkPhysicalDevice physicalDevice);

    // One region of one level of the source. `volume` for a VK_IMAGE_TYPE_3D source, whose slices
    // are `offset.z` .. `offset.z + extent.depth`; any other source is a 2D (possibly layered or
    // cube-compatible) image whose layers are `baseLayer` .. `baseLayer + layerCount` and whose
    // `offset.z` / `extent.depth` are 0 / 1. 1D images are not supported.
    struct WireE5RawCopyRegion {
        VkImage source = VK_NULL_HANDLE;
        Bool volume = false;
        Uint32 level = 0;
        Uint32 baseLayer = 0;
        Uint32 layerCount = 1;
        VkOffset3D offset{0, 0, 0};
        VkExtent3D extent{1, 1, 1};
    };

    // The twin the caller creates for `region`: R32_UINT, one level, the region's extent at its
    // origin - a 3D image for a volume region, a 2D image of `layerCount` layers otherwise -
    // with STORAGE | TRANSFER_SRC usage. RecordWireE5RawCopy leaves it in TRANSFER_SRC_OPTIMAL.
    VkImageCreateInfo WireE5RawCopyTwinInfo(const WireE5RawCopyRegion& region);

    // What a recorded copy holds until its submission completes.
    struct WireE5RawCopyObjects {
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        VkImageView sourceView = VK_NULL_HANDLE;
        VkImageView twinView = VK_NULL_HANDLE;
    };
    void DestroyWireE5RawCopyObjects(VkDevice device, WireE5RawCopyObjects& objects);

    // Records the copy of `region` into `twin` (fresh, created from WireE5RawCopyTwinInfo).
    // The source must have been created MUTABLE_FORMAT with an E5B9G9R9 format, and be in
    // `sourceLayout` (GENERAL or SHADER_READ_ONLY_OPTIMAL); the recording starts with a global
    // barrier that makes every earlier write visible to the compute stage and ends with the twin
    // in TRANSFER_SRC_OPTIMAL, its writes visible to transfer reads. On failure nothing has been
    // recorded and `objects` holds whatever was created, for DestroyWireE5RawCopyObjects.
    VkResult RecordWireE5RawCopy(VkDevice device, VkCommandBuffer commandBuffer, const WireE5RawCopyKit& kit,
                                 const WireE5RawCopyRegion& region, VkImageLayout sourceLayout, VkImage twin,
                                 WireE5RawCopyObjects& objects);

    // ---------------------------------------------------------------------------------------
    // THE PROBE.
    struct WireE5TransferProbeMeasurement {
        // Recorded, submitted and read back.
        Bool ran = false;
        // The bounded fence wait expired: every object was deliberately leaked.
        Bool fenceWaitTimedOut = false;
        String failureReason;
        // SUBJECT: a 30x30 E5B9G9R9 level of non-canonical words read back by vkCmdCopyImageToBuffer.
        Uint32 texels = 0;
        Uint32 subjectMismatches = 0;
        Uint32 firstExpected = 0;
        Uint32 firstObserved = 0;
        // CONTROL: the same image read through the route. Must be exact, or the upload, the
        // comparison or the route itself is what failed, and nothing is concluded.
        Uint32 controlMismatches = 0;
        // Reported, not judged: the same words in a 16x16 level read back by plain transfer. Clean
        // on the device this was characterised on (power-of-two extents read back verbatim, 28, 30
        // and 31 do not), which is why the subject is 30x30.
        Uint32 smallTexels = 0;
        Uint32 smallMismatches = 0;
    };

    enum class WireE5TransferProbeVerdict : Uint8 {
        // Nothing was measured (the route is unavailable, or setup failed): plain transfers stay.
        NotRun,
        // Measured, but the route's own read was not exact: nothing is concluded.
        Inconclusive,
        // Plain transfer reads return the stored words.
        Clean,
        // The defect: plain transfer reads changed words the route read back exactly.
        TransferReadNormalizes,
    };

    inline const char* WireE5TransferProbeVerdictName(WireE5TransferProbeVerdict verdict) {
        switch (verdict) {
            case WireE5TransferProbeVerdict::NotRun: return "not-run";
            case WireE5TransferProbeVerdict::Inconclusive: return "inconclusive";
            case WireE5TransferProbeVerdict::Clean: return "clean";
            case WireE5TransferProbeVerdict::TransferReadNormalizes: return "transfer-read-normalizes";
        }
        return "?";
    }

    inline WireE5TransferProbeVerdict EvaluateWireE5TransferProbe(const WireE5TransferProbeMeasurement& measurement) {
        if (!measurement.ran || measurement.fenceWaitTimedOut || measurement.texels == 0)
            return WireE5TransferProbeVerdict::NotRun;
        if (measurement.controlMismatches != 0) return WireE5TransferProbeVerdict::Inconclusive;
        return measurement.subjectMismatches != 0 ? WireE5TransferProbeVerdict::TransferReadNormalizes
                                                  : WireE5TransferProbeVerdict::Clean;
    }

    // One line with every reading, for the bring-up log and the POST row.
    String DescribeWireE5TransferProbe(const WireE5TransferProbeMeasurement& measurement);

    struct WireE5TransferProbeContext {
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        // Must be otherwise idle for the probe's duration.
        VkQueue queue = VK_NULL_HANDLE;
        Uint32 queueFamilyIndex = 0;
    };

    // One submission on its own command pool, a bounded fence wait (on timeout every object is
    // leaked and the measurement says so - the caller must then not destroy or idle-wait the
    // device), teardown on every other path. Never touches renderer state.
    WireE5TransferProbeMeasurement RunWireE5TransferProbe(const WireE5TransferProbeContext& context);

    // The POST's entry: a throwaway device with one graphics queue, the probe, the device
    // destroyed (leaked on a fence timeout).
    WireE5TransferProbeMeasurement RunWireE5TransferProbeOnThrowawayDevice(
        PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance instance, VkPhysicalDevice physicalDevice,
        Uint32 graphicsQueueFamilyIndex);
} // namespace MobileGL::MG_Backend::DirectVulkan
