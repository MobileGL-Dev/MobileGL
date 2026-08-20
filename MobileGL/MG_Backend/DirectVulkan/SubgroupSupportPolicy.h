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
    //     reusing that scratch, when explicitly enabled;
    //   - DeriveNumSubgroupsPass replaces the one builtin drivers get wrong
    //     (gl_NumSubgroups) with the value the rest of the topology implies.
    // The 32-lane shared-memory emulation (EmulateSubgroupsPass) is a LAST RESORT for
    // devices with no subgroup support at all, and only when the user opts in with
    // MOBILEGL_MAGMA_EMULATE_SUBGROUP=1; it never replaces available native operations.

    inline constexpr Uint32 kEmulatedSubgroupSize = 32u;
    inline constexpr Uint32 kEmulatedSubgroupStages = GL_COMPUTE_SHADER_BIT;
    inline constexpr Uint32 kEmulatedSubgroupFeatures =
        GL_SUBGROUP_FEATURE_BASIC_BIT_KHR | GL_SUBGROUP_FEATURE_VOTE_BIT_KHR |
        GL_SUBGROUP_FEATURE_ARITHMETIC_BIT_KHR | GL_SUBGROUP_FEATURE_BALLOT_BIT_KHR |
        GL_SUBGROUP_FEATURE_SHUFFLE_BIT_KHR | GL_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT_KHR |
        GL_SUBGROUP_FEATURE_CLUSTERED_BIT_KHR | GL_SUBGROUP_FEATURE_QUAD_BIT_KHR;

    inline Bool ShouldEmulateSubgroups(const Bool nativeSubgroupSupported) {
        return MG_Config::Features.MagmaEmulateSubgroup && !nativeSubgroupSupported &&
               !MG_Config::Features.DisableSubgroup;
    }

    inline Bool ShouldFixIterationRPSubgroupScratch() {
        // Auto is ON: the patch is fingerprint-gated to iterationRP's reduction and
        // grows one under-declared array; every other module passes through untouched.
        return MG_Config::Features.FixIterationRPSubgroupScratch !=
               MG_Config::QuirkOverride::ForceOff;
    }

    inline Bool ShouldFixIterationRPBarrier() {
        return MG_Config::Features.IterationRPFixBarrier;
    }

    inline Bool ShouldDeriveNumSubgroups() {
        // Auto is ON: gl_NumSubgroups must agree with the gl_SubgroupID range for the GL
        // contract to hold, and the derived ceil() value is the one the renderer can pin
        // with REQUIRE_FULL_SUBGROUPS - the driver builtin is the value with no
        // cross-driver guarantee (Adreno returns 1 for an 8-subgroup dispatch).
        return MG_Config::Features.DeriveNumSubgroups != MG_Config::QuirkOverride::ForceOff;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
