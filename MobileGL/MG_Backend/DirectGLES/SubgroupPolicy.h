// MobileGL - MobileGL/MG_Backend/DirectGLES/SubgroupPolicy.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include <Includes.h>
#include <MG_Backend/SubgroupImplementation.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>

namespace MobileGL::MG_Backend::DirectGLES {
    // How DirectGLES implements GL_KHR_shader_subgroup (MG_Backend/SubgroupImplementation.h),
    // shared by the extension list, the dynamic parameters and the ESSL transpile.
    //
    // NATIVE where the ES driver itself has GL_KHR_shader_subgroup: the stage is transpiled
    // with its GroupNonUniform operations intact and SPIRV-Cross spells them as the
    // GL_KHR_shader_subgroup_* built-ins. Without Vulkan semantics SPIRV-Cross can only print
    // the basic, vote and ballot operations (plus add/mul reductions, which do not make up the
    // arithmetic feature), so those three features are all the native path advertises.
    //
    // EMULATED where it has none (Adreno's ES driver among them): the compute stage is lowered
    // onto a 32-lane virtual subgroup in workgroup-shared memory before the transpile
    // (EmulateSubgroupsPass), which needs nothing from the driver beyond barrier() and shared
    // memory, and advertises every feature.
    inline SubgroupImplementation ResolveEsprytSubgroupImplementation(const MG_External::GLESCapabilities& caps) {
        return ResolveSubgroupImplementation(caps.SupportsShaderSubgroup);
    }

    inline constexpr Uint32 kEsslPrintableSubgroupFeatures =
        GL_SUBGROUP_FEATURE_BASIC_BIT_KHR | GL_SUBGROUP_FEATURE_VOTE_BIT_KHR | GL_SUBGROUP_FEATURE_BALLOT_BIT_KHR;

    struct EsprytSubgroupParameters {
        Uint32 size = 0;
        Uint32 stages = 0;
        Uint32 features = 0;
        Bool quadInAllStages = false;
    };

    inline EsprytSubgroupParameters ResolveEsprytSubgroupParameters(const MG_External::GLESCapabilities& caps) {
        switch (ResolveEsprytSubgroupImplementation(caps)) {
        case SubgroupImplementation::Native:
            return {caps.SubgroupSize, caps.SubgroupSupportedStages,
                    caps.SubgroupSupportedFeatures & kEsslPrintableSubgroupFeatures, false};
        case SubgroupImplementation::Emulated:
            return {kEmulatedSubgroupSize, kEmulatedSubgroupStages, kEmulatedSubgroupFeatures, false};
        case SubgroupImplementation::None:
            break;
        }
        return {};
    }
} // namespace MobileGL::MG_Backend::DirectGLES
