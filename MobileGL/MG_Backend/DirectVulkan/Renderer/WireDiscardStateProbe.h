// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireDiscardStateProbe.h
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

// DYNAMIC STATE ACROSS A RASTERIZER-DISCARD DRAW, and the probe that decides whether a device needs it
// set again.
//
// Dynamic viewport and scissor are command-buffer state: set once, they hold for every later draw
// whose pipeline declares them dynamic, across pipeline binds and render passes. Magma's draw path
// relies on that - its dynamic-state shadow skips a vkCmdSetViewport/vkCmdSetScissor whose value the
// command buffer already holds. On Adreno 750 a DRAW with a rasterizerDiscardEnable pipeline drops
// that state: the next draw that inherits it instead of setting it again draws nothing, in the same
// render pass or a later one. Binding the discard pipeline and setting the state without drawing
// with it is harmless, and so is the same draw with a rasterizing pipeline.
//
// The GL pattern is a vertex shader that writes with imageStore under GL_RASTERIZER_DISCARD, then a
// draw that reads what it wrote (KHR-GL4x.shader_image_load_store.advanced-sync-imageAccess and
// -vertexArray): the second draw has the same viewport and scissor, so the shadow sets neither, and
// it is lost.
//
// Where the probe below measured the defect, the draw path forgets the dynamic-state shadow after
// every discard draw (VulkanRenderer::ForgetDynamicStateAfterDiscardDraw), so the next draw sets its
// own. Armed by VulkanRenderer::ArmWireDiscardStateReemit at bring-up and reported in the POST's
// "Known Driver Bugs" (MG_Util/SelfTest/DriverBugProbes.cpp); MOBILEGL_MAGMA_REEMIT_STATE_AFTER_DISCARD
// forces it.
namespace MobileGL::MG_Backend::DirectVulkan {
    struct WireDiscardStateProbeMeasurement {
        Bool ran = false;
        Bool fenceWaitTimedOut = false;
        String failureReason;
        // Pixels of each lane's 16x16 target.
        Uint32 pixels = 0;
        // SUBJECT: a discard draw, then a full-target draw that inherits its viewport and scissor.
        // Green pixels (all of them, when correct).
        Uint32 subjectGreen = 0;
        // CONTROL: the same with the first draw rasterizing (clipped away). Must be all green.
        Uint32 controlGreen = 0;
    };

    enum class WireDiscardStateProbeVerdict : Uint8 {
        NotRun,
        Inconclusive,
        Clean,
        // The defect: the draw after the discard draw lost the state, the one after the clipped draw
        // did not.
        DiscardDrawDropsState,
    };

    inline const char* WireDiscardStateProbeVerdictName(WireDiscardStateProbeVerdict verdict) {
        switch (verdict) {
        case WireDiscardStateProbeVerdict::NotRun:
            return "not-run";
        case WireDiscardStateProbeVerdict::Inconclusive:
            return "inconclusive";
        case WireDiscardStateProbeVerdict::Clean:
            return "clean";
        case WireDiscardStateProbeVerdict::DiscardDrawDropsState:
            return "discard-draw-drops-state";
        }
        return "?";
    }

    inline WireDiscardStateProbeVerdict EvaluateWireDiscardStateProbe(const WireDiscardStateProbeMeasurement& m) {
        if (!m.ran || m.fenceWaitTimedOut || m.pixels == 0) return WireDiscardStateProbeVerdict::NotRun;
        if (m.controlGreen != m.pixels) return WireDiscardStateProbeVerdict::Inconclusive;
        return m.subjectGreen != m.pixels ? WireDiscardStateProbeVerdict::DiscardDrawDropsState
                                          : WireDiscardStateProbeVerdict::Clean;
    }

    String DescribeWireDiscardStateProbe(const WireDiscardStateProbeMeasurement& measurement);

    struct WireDiscardStateProbeContext {
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        // Must be otherwise idle for the probe's duration.
        VkQueue queue = VK_NULL_HANDLE;
        Uint32 queueFamilyIndex = 0;
    };

    // One submission on its own command pool, a bounded fence wait (on timeout every object is
    // leaked and the measurement says so), teardown on every other path.
    WireDiscardStateProbeMeasurement RunWireDiscardStateProbe(const WireDiscardStateProbeContext& context);

    // The POST's entry: a throwaway device with one graphics queue and no features.
    WireDiscardStateProbeMeasurement RunWireDiscardStateProbeOnThrowawayDevice(
        PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance instance, VkPhysicalDevice physicalDevice,
        Uint32 graphicsQueueFamilyIndex);
} // namespace MobileGL::MG_Backend::DirectVulkan
