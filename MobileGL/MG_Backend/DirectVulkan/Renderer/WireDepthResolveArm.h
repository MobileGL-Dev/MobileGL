// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireDepthResolveArm.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
#pragma once

#include <Includes.h>

// P7 gate 5 (g5-msrbo): WHICH ARM RESOLVES A MULTISAMPLE DEPTH/STENCIL ASPECT FIRST.
//
// The wire arm has two (WireFramebuffer.inc, ResolveWireDepthStencil): a render pass that carries
// a VK_KHR_depth_stencil_resolve attachment and no draw, and the baked shader pass of
// WireMultisampleResolve.inc, which draws. P7 wave 2-B2 took the render pass wherever the
// extension exists. On the Redmi (Adreno 830, Vulkan 1.3.284, driver 512.800.71) that pass left
// its resolve target unwritten - KHR-GL46.direct_state_access.renderbuffers_storage_multisample
// read every resolved depth and stencil value back as 0 on the inproc arm while the monolith arm
// passed, and the same case passed on the device with the shader arm forced
// (MGITEST_MAGMA_FORCE_SHADER_DEPTH_RESOLVE=1). So on a Qualcomm device the shader arm goes first;
// the render pass stays the fallback for what the shader cannot write (a stencil aspect without
// VK_EXT_shader_stencil_export). Every other vendor keeps B2's order: nothing measured says
// otherwise, and a vendor list that grew on a guess would be the same mistake the other way round.
//
// A pure function of the vendor id so the policy is a unit-testable fact rather than a branch
// buried in the apply thread.
namespace MobileGL::MG_Backend::DirectVulkan {
    inline constexpr Uint32 kWireVendorIdQualcomm = 0x5143u;

    inline constexpr Bool WirePrefersShaderDepthResolve(Uint32 vendorId) {
        return vendorId == kWireVendorIdQualcomm;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
