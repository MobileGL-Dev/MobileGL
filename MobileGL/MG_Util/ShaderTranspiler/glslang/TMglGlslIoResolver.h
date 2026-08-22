// MobileGL - MobileGL/MG_Util/ShaderTranspiler/glslang/TMglGlslIoResolver.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

//
// Created by Swung 0x48 on 2025/11/10.
//

#pragma once

#include <set>
#include <vector>
#include <unordered_map>
#include <glslang/Public/ShaderLang.h>
#include <glslang/Include/Types.h>
#include <glslang/Include/intermediate.h>
#include <glslang/MachineIndependent/iomapper.h>
#include "TVarEntryInfo.h"
#include "MG_Util/Types.h"

namespace MobileGL {
    class TMglGlslIoResolver : public glslang::TDefaultGlslIoResolver {
    public:
        using ExplicitVarSlotMap = UnorderedMap<String, Uint>;
        TMglGlslIoResolver(const glslang::TIntermediate& intermediate, const ExplicitVarSlotMap& vertexIns,
                           const ExplicitVarSlotMap& fragOuts, const ExplicitVarSlotMap& fragOutIndices,
                           ExplicitVarSlotMap* opaqueUniformBindings,
                           std::set<String>* storageBlocksWithoutBinding = nullptr,
                           std::set<String>* uniformBlocksWithoutBinding = nullptr)
            : TDefaultGlslIoResolver(intermediate), m_explicitVertexIns(vertexIns), m_explicitFragOuts(fragOuts),
              m_explicitFragOutIndices(fragOutIndices), m_explicitOpaqueUniformBindings(opaqueUniformBindings),
              m_storageBlocksWithoutBinding(storageBlocksWithoutBinding),
              m_uniformBlocksWithoutBinding(uniformBlocksWithoutBinding) {}
        TMglGlslIoResolver(const glslang::TProgram& program, const EShLanguage stage,
                           const ExplicitVarSlotMap& vertexIns, const ExplicitVarSlotMap& fragOuts,
                           const ExplicitVarSlotMap& fragOutIndices, ExplicitVarSlotMap* opaqueUniformBindings,
                           std::set<String>* storageBlocksWithoutBinding = nullptr,
                           std::set<String>* uniformBlocksWithoutBinding = nullptr)
            : TMglGlslIoResolver(*program.getIntermediate(stage), vertexIns, fragOuts, fragOutIndices,
                                 opaqueUniformBindings, storageBlocksWithoutBinding, uniformBlocksWithoutBinding) {}
        void reserverStorageSlot(glslang::TVarEntryInfo& ent, TInfoSink& infoSink) override;
        void reserverResourceSlot(glslang::TVarEntryInfo& ent, TInfoSink& infoSink) override;
        int resolveInOutLocation(EShLanguage stage, glslang::TVarEntryInfo& ent) override;
        int resolveUniformLocation(EShLanguage stage, glslang::TVarEntryInfo& ent) override;

    protected:
        bool ShouldAssignPlainUniformLocation(const glslang::TType& type) const;
        void EnsurePlainUniformLocationsAssigned();

        const ExplicitVarSlotMap& m_explicitVertexIns;
        const ExplicitVarSlotMap& m_explicitFragOuts;
        const ExplicitVarSlotMap& m_explicitFragOutIndices;
        // Two OUT channels, both filled from reserverResourceSlot and never read back by this
        // resolver. They exist because the collect callback is the LAST place the shader's own
        // declaration is still legible: ten lines later (iomapper.cpp:240) mapIO writes its
        // auto-assigned binding into the very qualifier that says whether the shader declared
        // one. Anything downstream that needs "as DECLARED" rather than "as ASSIGNED" has to be
        // handed it from here.
        ExplicitVarSlotMap* m_explicitOpaqueUniformBindings = nullptr;
        // Block TYPE names of the shader storage blocks that reached mapIO carrying NO
        // layout(binding = N). GL 4.3 core 7.8 gives such a block binding ZERO; see
        // ProgramLinkTask::SeedDefaultStorageBlockBindings for what is done with them.
        std::set<String>* m_storageBlocksWithoutBinding = nullptr;
        // The same capture for UNIFORM blocks. GL 4.6 core 7.6.2 gives an unqualified uniform
        // block binding ZERO, and glslang's auto-mapper does not: it packs uniform blocks into
        // the same slot space as samplers and images (spvVersion.openGl is 0 under
        // setEnvClient(EShClientVulkan), so TDefaultGlslIoResolver::resolveBinding keys every
        // resource kind on set 0), so an unbound block declared after an unbound image lands on
        // 1. See ProgramLinkTask's UBO reflection loop for what is done with them.
        std::set<String>* m_uniformBlocksWithoutBinding = nullptr;
        std::map<glslang::TString, int> m_plainUniformLocationSizeByName;
        std::map<glslang::TString, int> m_plainUniformLocationByName;
        bool m_plainUniformLocationsAssigned = false;
        // Descending allocator for INACTIVE vertex inputs (see resolveInOutLocation): they
        // still have to carry a Location because glslang emits them, but they must not take a
        // slot an active input would get. 15, not 31: the location survives into the ESSL
        // SPIRV-Cross emits for DirectGLES, and GL/ES only guarantee GL_MAX_VERTEX_ATTRIBS
        // >= 16 - a location of 31 makes the generated shader fail to compile on a real ES
        // driver (caught by the super-duper-vanilla and chocapic retrace fixtures).
        static constexpr int kInactiveVertexInLocationTop = 15;
        int m_nextInactiveVertexInLocation = kInactiveVertexInLocationTop;
    };
} // namespace MobileGL
