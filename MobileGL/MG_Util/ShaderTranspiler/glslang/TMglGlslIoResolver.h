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
                           const ExplicitVarSlotMap& fragOuts, const ExplicitVarSlotMap& fragOutIndices,
                           ExplicitVarSlotMap* opaqueUniformBindings)
            : TDefaultGlslIoResolver(intermediate), m_explicitVertexIns(vertexIns), m_explicitFragOuts(fragOuts),
              m_explicitFragOutIndices(fragOutIndices), m_explicitOpaqueUniformBindings(opaqueUniformBindings) {}
        TMglGlslIoResolver(const glslang::TProgram& program, const EShLanguage stage,
                           const ExplicitVarSlotMap& vertexIns, const ExplicitVarSlotMap& fragOuts,
                           const ExplicitVarSlotMap& fragOutIndices, ExplicitVarSlotMap* opaqueUniformBindings)
            : TMglGlslIoResolver(*program.getIntermediate(stage), vertexIns, fragOuts, fragOutIndices,
                                 opaqueUniformBindings) {}
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
        ExplicitVarSlotMap* m_explicitOpaqueUniformBindings = nullptr;
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
