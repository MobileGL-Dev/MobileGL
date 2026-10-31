// MobileGL - MobileGL/MG_Backend/DirectVulkan/SubgroupSupportPolicy.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include <Config.h>
#include <Includes.h>
#include <MG_Backend/SubgroupImplementation.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    // The single decision point for how DirectVulkan implements GL_KHR_shader_subgroup,
    // shared by capability advertisement (BackendObject) and module lowering
    // (VulkanRenderer / ProgramFactory) so the two can never disagree.
    //
    // Native subgroups are the implementation whenever the device has them, whatever
    // their width - subgroup operations execute on the hardware paths they were made
    // for. Module-level repairs keep the GL contract intact around them:
    //   - FixIterationRPSubgroupScratchPass patches the one known pack bug: iterationRP's
    //     prefixSumCache[32], under-declared for sub-16-lane devices (8-lane lavapipe);
    //   - FixIterationRPBarrierPass repairs Program 203's race between two reductions
    //     reusing that scratch (both iterationRP repairs: MOBILEGL_ITRP_FIX);
    //   - DeriveNumSubgroupsPass replaces the one builtin drivers get wrong
    //     (gl_NumSubgroups) with the value the rest of the topology implies.
    // The 32-lane shared-memory emulation (EmulateSubgroupsPass) is what a device with no
    // subgroup support at all gets (MG_Backend::ResolveSubgroupImplementation; forced with
    // MOBILEGL_SUBGROUP_EMULATION); it never replaces available native operations by default.

    using MG_Backend::kEmulatedSubgroupFeatures;
    using MG_Backend::kEmulatedSubgroupSize;
    using MG_Backend::kEmulatedSubgroupStages;

    inline MG_Backend::SubgroupImplementation ResolveVulkanSubgroupImplementation(const Bool nativeSubgroupSupported) {
        return MG_Backend::ResolveSubgroupImplementation(nativeSubgroupSupported);
    }

    inline Bool ShouldEmulateSubgroups(const Bool nativeSubgroupSupported) {
        return ResolveVulkanSubgroupImplementation(nativeSubgroupSupported) ==
               MG_Backend::SubgroupImplementation::Emulated;
    }

    // Native subgroup operations in use - the device has them and they are not overridden.
    // gl_NumSubgroups is derived on this path unconditionally (DeriveNumSubgroupsPass): GL
    // requires gl_SubgroupID < gl_NumSubgroups, Adreno's builtin reports 1 while the same
    // dispatch emits IDs 0..7, and the derived ceil() value is the one Vulkan guarantees
    // whenever the pipeline can request REQUIRE_FULL_SUBGROUPS.
    inline Bool UsesNativeSubgroups(const Bool nativeSubgroupSupported) {
        return ResolveVulkanSubgroupImplementation(nativeSubgroupSupported) ==
               MG_Backend::SubgroupImplementation::Native;
    }

    using MG_Backend::ShouldApplyIterationRPFixes;
} // namespace MobileGL::MG_Backend::DirectVulkan
