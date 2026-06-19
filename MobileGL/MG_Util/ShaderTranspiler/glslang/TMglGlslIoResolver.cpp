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
        if (currentStage == EShLangVertex && type.getQualifier().isPipeInput()) {
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
        }
        if (ShouldAssignPlainUniformLocation(type)) {
            const int size = glslang::TIntermediate::computeTypeUniformLocationSize(type);
            auto& recordedSize = m_plainUniformLocationSizeByName[name];
            recordedSize = std::max(recordedSize, size);
        }
        TDefaultGlslIoResolver::reserverStorageSlot(ent, infoSink);
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
