// MobileGL - MobileGL/MG_Backend/DirectVulkanTMP/Managers/ProgramManager.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "../Renderer/VulkanContext.h"
#include "../Renderer/VkCommon.h"
#include "MG_State/GLState/ProgramState/ProgramObject.h"

namespace MobileGL::MG_Backend::DirectVulkanTMP::VkManager {
    enum class ShaderTransformBit : Uint {
        None = 0,
        PositionYFlip = 1 << 0,
        PositionZRemap = 1 << 1,
    };
    using ShaderTransformFlags = Flags<ShaderTransformBit>;

    class ProgramManager {
    public:
        using HashType = Uint64;

        explicit ProgramManager(VulkanContext& ctx) : m_ctx(ctx) {}
        ~ProgramManager();

        ProgramManager(const ProgramManager&) = delete;
        ProgramManager& operator=(const ProgramManager&) = delete;

        Vector<VkPipelineShaderStageCreateInfo>& CreatePipelineShaderStages(
            MG_State::GLState::ProgramObject* program,
            ShaderTransformFlags transformFlags = ShaderTransformBit::PositionZRemap);
        HashType ComputeProgramHash(MG_State::GLState::ProgramObject* program,
                                    ShaderTransformFlags transformFlags = ShaderTransformBit::PositionZRemap) const;

    private:
        struct ProgramStages {
            HashType sourceHash = 0;
            HashType hash = 0;
            Vector<VkPipelineShaderStageCreateInfo> stages;
            Vector<VkShaderModule> modules;
        };

        void DestroyStages(ProgramStages& stages);
        HashType ComputeSourceSpvHash(MG_State::GLState::ProgramObject* program) const;
        HashType ComputeSpvHash(MG_State::GLState::ProgramObject* program, ShaderTransformFlags transformFlags) const;
        HashType ComputeSpvHash(const Vector<Vector<Uint32>>& spirvs) const;
        void BuildPipelineSpirvModules(MG_State::GLState::ProgramObject* program, Vector<Vector<Uint32>>& outSpirvs,
                                       ShaderTransformFlags transformFlags) const;
        VkShaderStageFlagBits ToVkStage(ShaderStage stage) const;

        VulkanContext& m_ctx;
        UnorderedMap<const MG_State::GLState::ProgramObject*, ProgramStages> m_cache;
    };
} // namespace MobileGL::MG_Backend::DirectVulkanTMP::VkManager
