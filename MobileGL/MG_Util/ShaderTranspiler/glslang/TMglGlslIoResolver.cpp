// MobileGL - MobileGL/MG_Util/ShaderTranspiler/glslang/TMglGlslIoResolver.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

//
// Created by Swung 0x48 on 2025/11/10.
//

#include "TMglGlslIoResolver.h"

namespace MobileGL {
    bool TMglGlslIoResolver::ShouldAssignPlainUniformLocation(const glslang::TType& type) const {
        if (!doAutoLocationMapping()) {
            return false;
        }

        if (type.getQualifier().hasLocation()) {
            return false;
        }

        if (type.isBuiltIn() || type.getBasicType() == glslang::EbtBlock || type.isAtomic() || type.isSpirvType() ||
            (type.containsOpaque() && referenceIntermediate.getSpv().openGl == 0)) {
            return false;
        }

        if (type.isStruct()) {
            if (type.getStruct()->size() < 1) {
                return false;
            }
            if ((*type.getStruct())[0].type->isBuiltIn()) {
                return false;
            }
        }

        return true;
    }

    void TMglGlslIoResolver::EnsurePlainUniformLocationsAssigned() {
        if (m_plainUniformLocationsAssigned) {
            return;
        }
        m_plainUniformLocationsAssigned = true;

        const int resourceKey = buildStorageKey(EShLangCount, glslang::EvqUniform);
        auto& slotMap = storageSlotMap[resourceKey];
        for (const auto& [name, size] : m_plainUniformLocationSizeByName) {
            const auto existingLocation = slotMap.find(name);
            if (existingLocation != slotMap.end()) {
                m_plainUniformLocationByName[name] = existingLocation->second;
                continue;
            }

            const int location = getFreeSlot(resourceKey, 0, size);
            slotMap[name] = location;
            m_plainUniformLocationByName[name] = location;
        }
    }

    void TMglGlslIoResolver::reserverStorageSlot(glslang::TVarEntryInfo& ent, TInfoSink& infoSink) {
        const glslang::TType& type = ent.symbol->getType();
        const glslang::TString& name = ent.symbol->getAccessName();
        // OpenGL assigns generic vertex attribute locations only to active inputs. glslang gathers
        // both live and dead declarations before mapping, so allowing the default collector to
        // reserve a dead vertex input would make it consume a location that an active input should
        // reuse. Other stage interfaces still need the default cross-stage matching behavior.
        if (!ent.live && currentStage == EShLangVertex && type.getQualifier().isPipeInput()) {
            return;
        }
        // glBindAttribLocation only affects active inputs in the linked program. Applying an API
        // binding to an inactive declaration would reserve its slot in glslang's collector and
        // incorrectly push an active, automatically mapped input to a different location.
        if (ent.live && currentStage == EShLangVertex && type.getQualifier().isPipeInput()) {
            auto it = m_explicitVertexIns.find(name.c_str());
            if (it != m_explicitVertexIns.end()) {
                auto& writableType = ent.symbol->getWritableType();
                writableType.getQualifier().layoutLocation = it->second;
            }
        }
        if (currentStage == EShLangFragment && type.getQualifier().isPipeOutput()) {
            auto it = m_explicitFragOuts.find(name.c_str());
            if (it != m_explicitFragOuts.end()) {
                auto& writableType = ent.symbol->getWritableType();
                writableType.getQualifier().layoutLocation = it->second;
            }
            // Dual-source blend color index (glBindFragDataLocationIndexed) -> layout(index = N).
            // Only the non-zero (dual-source) index is emitted: index 0 is the GL default, and
            // emitting an explicit "index = 0" qualifier would demand GL_EXT_blend_func_extended on
            // GLES even for ordinary single-source fragment outputs.
            auto idxIt = m_explicitFragOutIndices.find(name.c_str());
            if (idxIt != m_explicitFragOutIndices.end() && idxIt->second != 0) {
                auto& writableType = ent.symbol->getWritableType();
                writableType.getQualifier().layoutIndex = idxIt->second;
            }
        }
        if (ShouldAssignPlainUniformLocation(type)) {
            const int size = glslang::TIntermediate::computeTypeUniformLocationSize(type);
            auto& recordedSize = m_plainUniformLocationSizeByName[name];
            recordedSize = std::max(recordedSize, size);
        }
        TDefaultGlslIoResolver::reserverStorageSlot(ent, infoSink);
    }

    int TMglGlslIoResolver::resolveInOutLocation(EShLanguage stage, glslang::TVarEntryInfo& ent) {
        if (!ent.live && stage == EShLangVertex && ent.symbol->getType().getQualifier().isPipeInput()) {
            return ent.newLocation = -1;
        }
        return TDefaultGlslIoResolver::resolveInOutLocation(stage, ent);
    }

    void TMglGlslIoResolver::reserverResourceSlot(glslang::TVarEntryInfo& ent, TInfoSink& infoSink) {
        const glslang::TType& type = ent.symbol->getType();
        if (m_explicitOpaqueUniformBindings != nullptr && type.getBasicType() == glslang::EbtSampler &&
            type.getQualifier().hasBinding()) {
            const glslang::TString& name = ent.symbol->getAccessName();
            (*m_explicitOpaqueUniformBindings)[name.c_str()] = type.getQualifier().layoutBinding;
        }

        TDefaultGlslIoResolver::reserverResourceSlot(ent, infoSink);
    }

    int TMglGlslIoResolver::resolveUniformLocation(EShLanguage stage, glslang::TVarEntryInfo& ent) {
        const glslang::TType& type = ent.symbol->getType();
        if (type.getQualifier().hasLocation()) {
            return TDefaultGlslIoResolver::resolveUniformLocation(stage, ent);
        }

        if (!ShouldAssignPlainUniformLocation(type)) {
            return TDefaultGlslIoResolver::resolveUniformLocation(stage, ent);
        }

        EnsurePlainUniformLocationsAssigned();

        const glslang::TString& name = ent.symbol->getAccessName();
        const auto location = m_plainUniformLocationByName.find(name);
        if (location == m_plainUniformLocationByName.end()) {
            return ent.newLocation = -1;
        }

        return ent.newLocation = location->second;
    }
} // namespace MobileGL
