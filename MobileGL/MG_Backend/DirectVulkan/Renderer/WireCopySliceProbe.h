// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireCopySliceProbe.h
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

// CROSS-FORMAT SLICE COPIES BETWEEN A LAYERED IMAGE AND A 3D ONE, and the probe that decides
// whether a device needs them split.
//
// glCopyImageSubData between a 2D array and a 3D texture maps layers onto depth slices (GL 4.6
// core 18.3.2), and Vulkan expresses the whole move as ONE VkImageCopy: layerCount on the layered
// side, extent.depth on the 3D side (VUID-VkImageCopy-srcImage-01790/01791). Adreno 750 performs
// that region correctly when the two formats are the same, but when they differ (size-compatible,
// e.g. A2B10G10R10_UINT into A2B10G10R10_UNORM - KHR-GL4x.copy_image's cross-format cases) it
// lands only the FIRST slice and leaves the rest of the destination untouched. The same copy
// recorded as one single-slice region per slice is exact. The probe uses the wire's image shape
// (MUTABLE_FORMAT, attachment usage, a 30x30x24 destination): a non-mutable 7x7x8 pair without
// attachment usage copied cleanly on the same device.
//
// VulkanRenderer::CopyImageSubData therefore splits such a copy per slice where the probe below
// measured the defect (VulkanRenderer::ArmWireCopySliceSplit at bring-up, the POST's "Known Driver
// Bugs" section in MG_Util/SelfTest/DriverBugProbes.cpp); MOBILEGL_MAGMA_SPLIT_SLICE_COPIES forces it.
namespace MobileGL::MG_Backend::DirectVulkan {
    struct WireCopySliceProbeMeasurement {
        // Recorded, submitted and read back.
        Bool ran = false;
        // The bounded fence wait expired: every object was deliberately leaked.
        Bool fenceWaitTimedOut = false;
        String failureReason;
        // SUBJECT: one region, A2B10G10R10_UINT 2D array -> A2B10G10R10_UNORM 3D, every slice.
        Uint32 texels = 0;
        Uint32 slices = 0;
        Uint32 subjectMismatches = 0;
        // The slices the subject got entirely right (the defect leaves exactly the first).
        Uint32 subjectExactSlices = 0;
        // CONTROL: the same copy recorded one slice per region. Must be exact, or nothing is concluded.
        Uint32 controlMismatches = 0;
    };

    enum class WireCopySliceProbeVerdict : Uint8 {
        NotRun,
        Inconclusive,
        Clean,
        // The defect: the single region missed texels the per-slice regions moved exactly.
        CrossFormatSlicesDropped,
    };

    inline const char* WireCopySliceProbeVerdictName(WireCopySliceProbeVerdict verdict) {
        switch (verdict) {
            case WireCopySliceProbeVerdict::NotRun: return "not-run";
            case WireCopySliceProbeVerdict::Inconclusive: return "inconclusive";
            case WireCopySliceProbeVerdict::Clean: return "clean";
            case WireCopySliceProbeVerdict::CrossFormatSlicesDropped: return "cross-format-slices-dropped";
        }
        return "?";
    }

    inline WireCopySliceProbeVerdict EvaluateWireCopySliceProbe(const WireCopySliceProbeMeasurement& measurement) {
        if (!measurement.ran || measurement.fenceWaitTimedOut || measurement.texels == 0)
            return WireCopySliceProbeVerdict::NotRun;
        if (measurement.controlMismatches != 0) return WireCopySliceProbeVerdict::Inconclusive;
        return measurement.subjectMismatches != 0 ? WireCopySliceProbeVerdict::CrossFormatSlicesDropped
                                                  : WireCopySliceProbeVerdict::Clean;
    }

    String DescribeWireCopySliceProbe(const WireCopySliceProbeMeasurement& measurement);

    struct WireCopySliceProbeContext {
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        // Must be otherwise idle for the probe's duration.
        VkQueue queue = VK_NULL_HANDLE;
        Uint32 queueFamilyIndex = 0;
    };

    // One submission on its own command pool, a bounded fence wait (on timeout every object is
    // leaked and the measurement says so), teardown on every other path.
    WireCopySliceProbeMeasurement RunWireCopySliceProbe(const WireCopySliceProbeContext& context);

    // The POST's entry: a throwaway device with one graphics queue.
    WireCopySliceProbeMeasurement RunWireCopySliceProbeOnThrowawayDevice(PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                                                                         VkInstance instance,
                                                                         VkPhysicalDevice physicalDevice,
                                                                         Uint32 graphicsQueueFamilyIndex);
} // namespace MobileGL::MG_Backend::DirectVulkan
