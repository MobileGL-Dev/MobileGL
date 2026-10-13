// MobileGL - MobileGL/MG_Backend/SubgroupImplementation.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include <Config.h>
#include <Includes.h>

namespace MobileGL::MG_Backend {
    // How a backend implements GL_KHR_shader_subgroup - decided once, from what its driver
    // offers, and shared by capability advertisement and module lowering so the two can never
    // disagree.
    //   Native   - the driver's own subgroup operations, at whatever width it runs (8, 16, 32,
    //              64, 128 lanes); MobileGL advertises the driver's size, stages and features.
    //   Emulated - a 32-lane VIRTUAL subgroup in workgroup-shared memory
    //              (ShaderTranspiler::EmulateSubgroupsPass): compute stage only, every feature
    //              bit. What a driver with no subgroup operations at all gets.
    //   None     - GL_KHR_shader_subgroup is not advertised.
    // MOBILEGL_SUBGROUP_EMULATION: Auto emulates exactly where nothing native exists; ForceOn
    // emulates even over native support (to exercise the emulation on any device); ForceOff
    // never emulates.
    enum class SubgroupImplementation : Uint8 { None, Native, Emulated };

    inline constexpr Uint32 kEmulatedSubgroupSize = 32u;
    inline constexpr Uint32 kEmulatedSubgroupStages = GL_COMPUTE_SHADER_BIT;
    inline constexpr Uint32 kEmulatedSubgroupFeatures =
        GL_SUBGROUP_FEATURE_BASIC_BIT_KHR | GL_SUBGROUP_FEATURE_VOTE_BIT_KHR |
        GL_SUBGROUP_FEATURE_ARITHMETIC_BIT_KHR | GL_SUBGROUP_FEATURE_BALLOT_BIT_KHR |
        GL_SUBGROUP_FEATURE_SHUFFLE_BIT_KHR | GL_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT_KHR |
        GL_SUBGROUP_FEATURE_CLUSTERED_BIT_KHR | GL_SUBGROUP_FEATURE_QUAD_BIT_KHR;

    inline SubgroupImplementation ResolveSubgroupImplementation(const Bool nativeSupported) {
        switch (MG_Config::Features.SubgroupEmulation) {
        case MG_Config::QuirkOverride::ForceOn:
            return SubgroupImplementation::Emulated;
        case MG_Config::QuirkOverride::ForceOff:
            return nativeSupported ? SubgroupImplementation::Native : SubgroupImplementation::None;
        case MG_Config::QuirkOverride::Auto:
        default:
            return nativeSupported ? SubgroupImplementation::Native : SubgroupImplementation::Emulated;
        }
    }

    // MOBILEGL_ITRP_FIX: whether iterationRP's two compute-shader repairs run (Config.h). Both
    // passes are fingerprint-gated, so Auto applies them; only ForceOff replays the pack's bugs.
    inline Bool ShouldApplyIterationRPFixes() {
        return MG_Config::Features.IterationRPFix != MG_Config::QuirkOverride::ForceOff;
    }

    inline const char* SubgroupImplementationName(const SubgroupImplementation implementation) {
        switch (implementation) {
        case SubgroupImplementation::Native: return "native";
        case SubgroupImplementation::Emulated: return "emulated";
        case SubgroupImplementation::None: break;
        }
        return "none";
    }
} // namespace MobileGL::MG_Backend
