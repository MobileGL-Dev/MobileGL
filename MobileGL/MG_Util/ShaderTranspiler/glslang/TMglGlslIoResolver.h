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
                           const ExplicitVarSlotMap& fragOuts, ExplicitVarSlotMap* opaqueUniformBindings)
            : TDefaultGlslIoResolver(intermediate), m_explicitVertexIns(vertexIns), m_explicitFragOuts(fragOuts),
              m_explicitOpaqueUniformBindings(opaqueUniformBindings) {}
        TMglGlslIoResolver(const glslang::TProgram& program, const EShLanguage stage,
                           const ExplicitVarSlotMap& vertexIns, const ExplicitVarSlotMap& fragOuts,
                           ExplicitVarSlotMap* opaqueUniformBindings)
            : TMglGlslIoResolver(*program.getIntermediate(stage), vertexIns, fragOuts, opaqueUniformBindings) {}
        void reserverStorageSlot(glslang::TVarEntryInfo& ent, TInfoSink& infoSink) override;
        void reserverResourceSlot(glslang::TVarEntryInfo& ent, TInfoSink& infoSink) override;
        int resolveUniformLocation(EShLanguage stage, glslang::TVarEntryInfo& ent) override;

    protected:
        bool ShouldAssignPlainUniformLocation(const glslang::TType& type) const;
        void EnsurePlainUniformLocationsAssigned();

        const ExplicitVarSlotMap& m_explicitVertexIns;
        const ExplicitVarSlotMap& m_explicitFragOuts;
        ExplicitVarSlotMap* m_explicitOpaqueUniformBindings = nullptr;
        std::map<glslang::TString, int> m_plainUniformLocationSizeByName;
        std::map<glslang::TString, int> m_plainUniformLocationByName;
        bool m_plainUniformLocationsAssigned = false;
    };
} // namespace MobileGL
