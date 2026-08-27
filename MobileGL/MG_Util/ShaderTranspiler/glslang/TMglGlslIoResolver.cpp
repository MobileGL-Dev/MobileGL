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
#include <cstdlib>
#include <string>

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
    // GLSL 4.30 4.4.5 and ES 3.1 4.4.4: `layout(binding = N)` on any opaque uniform, uniform
    // block, storage block or atomic counter is a COMPILE-TIME error when N is not less than that
    // resource kind's implementation limit - and, for an ARRAY of them, when base + count - 1 is
    // not. MobileGL enforces it here rather than at compile because here is the last point where
    // `qualifier.hasBinding()` still means "the SHADER said so" (see the comment on the caller),
    // and because the per-device ceilings are deliberately not part of the compile pipeline's
    // memo keys. The conformance suite accepts a link-time rejection: its predicate is
    // compiledAndLinked(), which is the AND of the two.
    //
    // ONE enforcement point for all five kinds, on purpose. Before this, exactly one kind -
    // shader-storage blocks - was checked, by a bespoke lexical scan of the shader source, which
    // is why the storage sub-family was the one that passed while sampler, image, uniform-block
    // and atomic-counter bindings sailed past every ceiling. That scanner is retired; a second
    // enforcement point is a second thing to drift.
    void TMglGlslIoResolver::CheckDeclaredBindingRange(const glslang::TType& type, const glslang::TString& name) {
        if (m_bindingLimits == nullptr || m_bindingViolation == nullptr) return;
        if (!m_bindingViolation->empty()) return;   // first violation wins; the link is already lost

        const glslang::TQualifier& qualifier = type.getQualifier();
        const char* kind = nullptr;
        const char* limitName = nullptr;
        Int limit = 0;
        long long binding = -1;

        if (type.getBasicType() == glslang::EbtSampler && qualifier.hasBinding()) {
            const bool isImage = type.getSampler().isImage();
            kind = isImage ? "image" : "sampler";
            limitName = isImage ? "GL_MAX_IMAGE_UNITS" : "GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS";
            limit = isImage ? m_bindingLimits->MaxImageBindings : m_bindingLimits->MaxSamplerBindings;
            binding = qualifier.layoutBinding;
        } else if (type.getBasicType() == glslang::EbtBlock) {
            // An atomic counter never reaches here as a counter: the relaxed parse has already
            // folded it into a synthesized "gl_AtomicCounterBlock_<binding>" storage block whose
            // TRAILING NUMBER is the GL binding the shader asked for (ParseContextBase::
            // growAtomicCounterBlock names it from bufferBinding). That name is the only surviving
            // record of the declaration, so it is what the counter ceiling is read off.
            const Int counterBinding = MG_Util::ShaderTranspiler::AtomicCounterBlockGlBinding(
                StringView(name.c_str(), name.size()));
            if (counterBinding >= 0) {
                kind = "atomic_uint";
                limitName = "GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS";
                limit = m_bindingLimits->MaxAtomicCounterBufferBindings;
                binding = counterBinding;
            } else if (qualifier.hasBinding() && qualifier.storage == glslang::EvqUniform &&
                       name.compare(MG_Util::ShaderTranspiler::GLOBAL_UBO_NAME) != 0) {
                kind = "uniform block";
                limitName = "GL_MAX_UNIFORM_BUFFER_BINDINGS";
                limit = m_bindingLimits->MaxUniformBufferBindings;
                binding = qualifier.layoutBinding;
            } else if (qualifier.hasBinding() && qualifier.storage == glslang::EvqBuffer) {
                kind = "buffer block";
                limitName = "GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS";
                limit = m_bindingLimits->MaxShaderStorageBufferBindings;
                binding = qualifier.layoutBinding;
            }
        }

        if (kind == nullptr || limit <= 0 || binding < 0) return;

        // The ARRAYED-INSTANCE rule: an array of N takes bindings base .. base + N - 1, and every
        // one of them has to fit. getCumulativeArraySize() folds a multi-dimensional array into
        // the count of leaf elements, which is exactly how many consecutive bindings GL hands out.
        // An unsized or implicitly-sized array reports 0; treat it as one binding rather than
        // guess, since it cannot be the shape the rule is about.
        long long elementCount = 1;
        if (type.isArray()) {
            const int cumulative = static_cast<int>(type.getCumulativeArraySize());
            if (cumulative > 1) elementCount = cumulative;
        }
        const long long lastBinding = binding + elementCount - 1;
        if (lastBinding < static_cast<long long>(limit)) return;

        String message = "Error: layout(binding = " + std::to_string(binding) + ") on " + kind + " '" +
                         String(name.c_str()) + "'";
        if (elementCount > 1) {
            message += " (an array of " + std::to_string(elementCount) + ", occupying bindings " +
                       std::to_string(binding) + ".." + std::to_string(lastBinding) + ")";
        }
        message += " is not less than " + String(limitName) + " (" + std::to_string(limit) + ").";
        *m_bindingViolation = Move(message);
    }

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

        // A UNIFORM block that declared no binding. Same capture point and same union-across-
        // stages reasoning as the storage-block set above, and the same reason it cannot be
        // asked later: mapIO is about to write an auto-assigned binding into this very
        // qualifier. MGL_GLOBAL_UBO is MobileGL's own synthesized block, not an application
        // one - it never reaches the GL block space and must not be seeded here.
        if (m_uniformBlocksWithoutBinding != nullptr && type.getBasicType() == glslang::EbtBlock &&
            qualifier.storage == glslang::EvqUniform && !qualifier.hasBinding() &&
            name.compare(MG_Util::ShaderTranspiler::GLOBAL_UBO_NAME) != 0) {
            m_uniformBlocksWithoutBinding->insert(name.c_str());
        }

        CheckDeclaredBindingRange(type, name);

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
