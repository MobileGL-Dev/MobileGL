// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireDepthResolveProbe.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
#pragma once

#include "WireDepthResolveArm.h"

#include <Config.h>
#include <Includes.h>

// P7 gate 5 (g5-msprobe): the Vulkan half of the multisample depth/stencil resolve probe, and the
// two resolve arms it measures.
//
// THE ARMS LIVE HERE, NOT ONLY IN THE RENDERER. A probe that rebuilt "something like" the wire's
// render pass would measure its own copy; the render-pass description, the shader pass's modules,
// pipeline and draw, and the view shapes both arms attach are defined once below and called by
// ResolveWireDepthStencil (WireFramebuffer.inc), ResolveWireDepthStencilWithShader
// (WireMultisampleResolve.inc) and the probe alike.
//
// Two callers: the Magma renderer at device bring-up (server side, once per process, only where
// the render-pass arm exists at all - VulkanRenderer::ArmWireDepthResolveOrder) and the POST's
// "Known Driver Bugs" section (MG_Util/SelfTest/DriverBugProbes.cpp) on a throwaway device.
//
// Disaggregated builds only: the monolith never records either arm, and the pull build's image
// must not change (P7 gate G1).
#if MOBILEGL_BUILD_DISAGGREGATED
namespace MobileGL::MG_Backend::DirectVulkan {
    // Every aspect a wire depth/stencil format has.
    VkImageAspectFlags WireDepthStencilFormatAspects(VkFormat format);

    // A 2D view of one level/layer of a depth/stencil image naming `aspects`. The render-pass arm
    // attaches both images with every aspect of the format; the shader arm SAMPLES its source
    // through a view of exactly one aspect (a depth/stencil image can only be sampled that way) and
    // attaches its target with every aspect.
    VkImageViewCreateInfo WireDepthStencilViewInfo(VkImage image, VkFormat format, VkImageAspectFlags aspects,
                                                   Uint32 level, Uint32 layer);

    // THE RENDER-PASS ARM: attachment 0 the multisample source (LOAD/STORE), attachment 1 the
    // single-sample resolve target (DONT_CARE/STORE), both in DEPTH_STENCIL_ATTACHMENT_OPTIMAL, one
    // subpass whose VkSubpassDescriptionDepthStencilResolve resolves every aspect of the format
    // with VK_RESOLVE_MODE_SAMPLE_ZERO_BIT. The caller begins and ends it with NO draw.
    VkResult CreateWireDepthStencilResolvePass(PFN_vkCreateRenderPass2 createRenderPass2, VkDevice device,
                                               VkFormat format, VkSampleCountFlagBits samples, VkRenderPass* pass);

    // THE SHADER ARM's device objects that do not depend on the format.
    struct WireShaderResolveKit {
        VkShaderModule vertex = VK_NULL_HANDLE;
        VkShaderModule depthFragment = VK_NULL_HANDLE;
        // Only where VK_EXT_shader_stencil_export is enabled: its SPIR-V declares StencilExportEXT.
        VkShaderModule stencilFragment = VK_NULL_HANDLE;
        VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkSampler nearest = VK_NULL_HANDLE;
    };
    // Creates every member; the first failure is returned and the members made so far stay set,
    // for DestroyWireShaderResolveKit.
    VkResult CreateWireShaderResolveKit(VkDevice device, Bool shaderStencilExport, WireShaderResolveKit& kit);
    void DestroyWireShaderResolveKit(VkDevice device, WireShaderResolveKit& kit);
    // One (format, aspect)'s single-attachment render pass: DONT_CARE/STORE on both aspects,
    // DEPTH_STENCIL_ATTACHMENT_OPTIMAL throughout.
    VkResult CreateWireShaderResolveRenderPass(VkDevice device, VkFormat format, VkRenderPass* pass);
    // Its pipeline: full-screen triangle, gl_FragDepth (ALWAYS + write) or gl_FragStencilRefARB
    // (REPLACE everywhere), viewport and scissor dynamic. VK_ERROR_FEATURE_NOT_PRESENT when the kit
    // has no fragment module for `aspect`.
    VkResult CreateWireShaderResolvePipeline(VkDevice device, const WireShaderResolveKit& kit, VkRenderPass pass,
                                             VkImageAspectFlags aspect, VkPipeline* pipeline);
    // Records the draw: the render area and the scissor are `rect`, the viewport is the WHOLE
    // attachment (gl_FragCoord is what the fragment stage fetches by), one triangle.
    void RecordWireShaderResolveDraw(VkCommandBuffer commandBuffer, const WireShaderResolveKit& kit,
                                     VkRenderPass pass, VkPipeline pipeline, VkFramebuffer framebuffer,
                                     VkDescriptorSet descriptorSet, VkExtent2D attachmentExtent, VkRect2D rect);

    // What the probe runs on. The device must have been created with VK_KHR_create_renderpass2 and
    // VK_KHR_depth_stencil_resolve (or Vulkan 1.2 core), and with VK_EXT_shader_stencil_export where
    // `shaderStencilExport` says so. The queue must be otherwise idle for the probe's duration.
    struct WireDepthResolveProbeContext {
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        Uint32 queueFamilyIndex = 0;
        PFN_vkCreateRenderPass2 createRenderPass2 = nullptr;
        VkResolveModeFlags depthResolveModes = 0;
        VkResolveModeFlags stencilResolveModes = 0;
        Bool shaderStencilExport = false;
        // TEST ONLY (MGITEST_MAGMA_DEPTH_RESOLVE_PROBE=elide-subject, WireDepthResolveArm.h): record
        // NO render-pass resolve, so every subject target keeps its sentinel - the Adreno 830's
        // reading - while the shader control runs as always. Everything else is the real probe.
        Bool elideSubject = false;
    };

    // Per format the device can host as a 4x multisample depth/stencil attachment AND for which
    // ResolveWireDepthStencil would take the render-pass arm (D16, X8_D24, D32F, D24S8, D32FS8, S8):
    //   - two identical 4x4 4x multisample sources (production usage), each cleared to depth 0.25 /
    //     stencil 0x5A the way the wire clears (a LOAD pass with vkCmdClearAttachments);
    //   - single-sample resolve targets (the production scratch usage) pre-filled with the sentinel
    //     depth 0.75 / stencil 0xA5;
    //   - SUBJECT: the render-pass arm's exact pass, begun and ended with no draw;
    //   - CONTROL: the shader arm's exact pass on the other source, one target per aspect (stencil
    //     only with VK_EXT_shader_stencil_export);
    //   - every target copied aspect by aspect into a host-visible buffer and compared texel by texel.
    // One submission, a bounded fence wait (on timeout every object is leaked and the measurement
    // says so - the caller must then not destroy or idle-wait the device), teardown on every other
    // path. Never touches renderer state.
    WireDepthResolveProbeMeasurement RunWireDepthResolveProbe(const WireDepthResolveProbeContext& context);

    // The POST's entry: creates a throwaway device on `physicalDevice` with exactly the extensions
    // the renderer enables for the two arms, runs the probe on its graphics queue and destroys the
    // device (leaks it on a fence timeout). `renderPassArmAvailable` is false - and nothing is run -
    // when the device lacks VK_KHR_create_renderpass2 / VK_KHR_depth_stencil_resolve; the shader
    // pass is then the only arm and the probe has nothing to choose between.
    // `shaderStencilExport` reports whether VK_EXT_shader_stencil_export was available.
    WireDepthResolveProbeMeasurement RunWireDepthResolveProbeOnThrowawayDevice(
        PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance instance, VkPhysicalDevice physicalDevice,
        Uint32 graphicsQueueFamilyIndex, const Vector<VkExtensionProperties>& deviceExtensions,
        Bool& renderPassArmAvailable, Bool& shaderStencilExport);
} // namespace MobileGL::MG_Backend::DirectVulkan
#endif
