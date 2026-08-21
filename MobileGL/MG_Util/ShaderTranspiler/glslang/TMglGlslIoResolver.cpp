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

#include <cstring>

#include <MG_Util/ShaderTranspiler/Types.h>

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
        // NO dead-vertex-input early-out here, deliberately - the skip belongs in
        // reserverStorageSlot() and ONLY there.
        //
        // Skipping RESERVATION is the GL semantic: only active inputs get generic attribute
        // locations, so a dead declaration must not consume a slot an active input should
        // have. Skipping RESOLUTION as well used to look like the same statement, but it is a
        // different one: it leaves the variable with no layoutLocation, and glslang still
        // EMITS it - a declared input is in the shader's linker objects and therefore in the
        // entry point's interface. The result is an OpVariable of storage class Input with no
        // Location decoration, which SPIR-V forbids
        // (VUID-StandaloneSpirv-Location-04916). lavapipe tolerates it; Adreno rejects the
        // whole pipeline with VK_ERROR_UNKNOWN, which is how this shipped undetected - every
        // desktop gate, retrace corpus included, is blind to it.
        //
        // Found 2026-08-11 on an Adreno 830: the Iris weather program (mc_midTexCoord among
        // seven attributes, only some of them glBindAttribLocation-bound) died at the first
        // rainy-world draw, 100% reproducible, programHash 0x4a7e9a37fb49caa1.
        //
        // They cannot simply be handed to the base resolver either. Auto-assignment for inputs
        // WITHOUT an explicit binding happens entirely in the resolve pass, in sort order, so a
        // dead declaration reaching the free-slot search first would take location 0 and push
        // the active input up - which is precisely the GL violation the reservation skip
        // exists to prevent (ProgramTest.InactiveExplicitVertexBindingsDoNotReserveLocations
        // pins it: Iris injects Position/UV0 into packs that actually read vaPosition).
        //
        // So dead inputs get their locations from the TOP of the attribute range downward,
        // while the base resolver hands active ones out from 0 upward. Both properties hold at
        // once: every emitted input carries a Location, and no active input is displaced. The
        // two allocators can only meet if live + dead exceed the attribute limit, which is an
        // over-subscribed program GL would reject anyway; if that happens we leave the
        // variable to the base resolver rather than hand out a colliding location.
        const glslang::TType& type = ent.symbol->getType();
        if (!ent.live && stage == EShLangVertex && type.getQualifier().isPipeInput() &&
            !type.getQualifier().hasLocation() && !type.isBuiltIn()) {
            const int size = std::max(1, glslang::TIntermediate::computeTypeLocationSize(type, stage));
            if (m_nextInactiveVertexInLocation - (size - 1) >= 0) {
                m_nextInactiveVertexInLocation -= (size - 1);
                ent.symbol->getWritableType().getQualifier().layoutLocation = m_nextInactiveVertexInLocation;
                --m_nextInactiveVertexInLocation;
            }
        }
        return TDefaultGlslIoResolver::resolveInOutLocation(stage, ent);
    }

    // THE COLLECT CALLBACK IS THE CAPTURE POINT, and the reason is a matter of ten lines of
    // glslang. mapIO gathers every declared symbol of every stage and calls this on each of
    // them (iomapper.cpp addStage -> TSlotCollector) BEFORE it resolves anything; only
    // afterwards, in doMap(), does it write the slots it chose back into the types
    // (iomapper.cpp:240, `layoutBinding = at->second.newBinding`). Up to here
    // `qualifier.hasBinding()` still answers "did the SHADER say so?"; past it, every resource
    // carries a number and the question can no longer be asked at all.
    //
    // Both captures below used to be lexical scans of the shader source, which had to run
    // before the preprocessor's macros were expanded and therefore could not read
    // `binding = SOME_MACRO` - the spelling Flywheel's indirect engine uses for every one of
    // its storage blocks. Asking the AST instead makes the macro case ordinary.
    void TMglGlslIoResolver::reserverResourceSlot(glslang::TVarEntryInfo& ent, TInfoSink& infoSink) {
        const glslang::TType& type = ent.symbol->getType();
        const glslang::TQualifier& qualifier = type.getQualifier();
        // getAccessName() is the BLOCK TYPE name for a block and the declared name for
        // everything else (IntermTraverse.cpp TIntermSymbol::getAccessName) - which is exactly
        // the key both consumers want.
        const glslang::TString& name = ent.symbol->getAccessName();

        if (m_explicitOpaqueUniformBindings != nullptr && type.getBasicType() == glslang::EbtSampler &&
            qualifier.hasBinding()) {
            (*m_explicitOpaqueUniformBindings)[name.c_str()] = qualifier.layoutBinding;
        }

        // A storage block that declared no binding. UNION across stages by construction - one
        // resolver serves the whole program - which is what GLSL's "every stage must declare
        // the same block identically" rule makes correct.
        //
        // NOT the atomic-counter blocks glslang SYNTHESIZES, which are storage blocks by every
        // structural test available here and are still not what this set means. Relaxed parsing
        // folds each atomic_uint into a "gl_AtomicCounterBlock_<GL binding>" block
        // (ParseContextBase::growAtomicCounterBlock) and leaves it unbound because MobileGL asks
        // for auto-mapped bindings - so it arrives looking exactly like an unqualified
        // application block. Seeding one to GL binding 0 would overwrite the counter buffer's
        // real binding, which is the trailing number in that very name.
        if (m_storageBlocksWithoutBinding != nullptr && type.getBasicType() == glslang::EbtBlock &&
            qualifier.storage == glslang::EvqBuffer && !qualifier.hasBinding() &&
            name.compare(0, std::strlen(MG_Util::ShaderTranspiler::ATOMIC_COUNTER_BLOCK_PREFIX),
                         MG_Util::ShaderTranspiler::ATOMIC_COUNTER_BLOCK_PREFIX) != 0) {
            m_storageBlocksWithoutBinding->insert(name.c_str());
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
