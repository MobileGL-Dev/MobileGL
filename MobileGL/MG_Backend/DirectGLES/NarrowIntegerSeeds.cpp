// MobileGL - MobileGL/MG_Backend/DirectGLES/NarrowIntegerSeeds.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "NarrowIntegerSeeds.h"

#include "DirectGLESInternal.h"

#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>

namespace MobileGL::MG_Backend::DirectGLES {
    namespace {
        using MG_Util::ShaderTranspiler::kNarrowIntegerWidths16GlobalName;
        using MG_Util::ShaderTranspiler::kNarrowIntegerWidths8GlobalName;

        // The twin a handle already has, without creating one: a program that was never built has
        // no seeds to refuse.
        PrgramImpl::BackendProgramObjectImpl* ExistingProgramTwin(MG_Pipe::MGPipeHandle program) {
            auto* slot = PrgramImpl::g_backendProgramObjects.FindByHandle(program);
            return slot != nullptr ? slot->get() : nullptr;
        }

        String UniformDeclaration(const char* name) {
            return String("uniform highp uint ") + name + ";";
        }
    } // namespace

    IntegerVertexInputMasks IntegerVertexInputsOf(const MG_Pipe::MGPipeShaderCsoRecord& record) {
        auto& memo = record.BackendIntegerInputs;
        if (!memo.Valid || memo.Serial != record.Serial) {
            IntegerVertexInputMasks inputs;
            if (record.Archive) {
                // Indexed by location, an empty name being no active input: the same reading
                // SyncCurrentVertexAttributeValues makes of these two vectors.
                const auto& link = record.Archive->Link;
                const SizeT count = std::min<SizeT>(std::min(link.attribs.size(), link.attribTypes.size()), 32);
                for (SizeT location = 0; location < count; ++location) {
                    if (!link.attribs[location].empty())
                        inputs.Note(static_cast<Uint32>(location), link.attribTypes[location]);
                }
            }
            memo.SignedInputs = inputs.signedInputs;
            memo.UnsignedInputs = inputs.unsignedInputs;
            memo.Serial = record.Serial;
            memo.Valid = true;
        }
        IntegerVertexInputMasks inputs;
        inputs.signedInputs = memo.SignedInputs;
        inputs.unsignedInputs = memo.UnsignedInputs;
        return inputs;
    }

    IntegerAttributePlan ResolveIntegerAttributePlanForDraw(MG_Pipe::MGPipeHandle program) {
        if (MG_Pipe::MGPipeHandleIsNull(program)) return {};
        const MG_Pipe::MGPipeShaderCsoRecord* const record = PipeShaderCsoRecordForHandle(program);
        if (record == nullptr || !record->Archive) return {};
        const IntegerVertexInputMasks inputs = IntegerVertexInputsOf(*record);
        Uint32 remaining = inputs.signedInputs | inputs.unsignedInputs;
        if (remaining == 0) return {};
        const auto* elements = VertexArrayImpl::BoundVertexElementsRecord(MG_Pipe::MGPipeApplier());
        if (elements == nullptr) return {};
        IntegerAttributeDataMasks data;
        for (; remaining != 0; remaining &= remaining - 1) {
            const Uint32 location = static_cast<Uint32>(std::countr_zero(remaining));
            const auto& attrib = elements->Attributes[location];
            if (attrib.Enabled && attrib.IsInteger && !attrib.IsLong)
                data.Note(location, static_cast<DataType>(attrib.Type));
        }
        auto* twin = ExistingProgramTwin(program);
        return ResolveIntegerAttributePlan(inputs, data, twin == nullptr || !twin->NarrowIntegerSeeds().Refused());
    }

    Bool DropRefusedNarrowIntegerSeeds(IntegerAttributePlan& plan, MG_Pipe::MGPipeHandle program) {
        if (!plan.NeedsSeeds()) return false;
        auto* twin = ExistingProgramTwin(program);
        if (twin == nullptr || !twin->NarrowIntegerSeeds().Refused()) return false;
        plan = plan.WithoutSeeds();
        return true;
    }

    void PushNarrowIntegerWidthsForDraw(const IntegerAttributePlan& plan, MG_Pipe::MGPipeHandle program) {
        // Only the twin this draw's sync stashed, and only while it is the program Use() bound: a
        // uniform call with program 0, or on another program, is an error or a wrong write.
        if (PrgramImpl::g_currentDrawProgramHandle != program) return;
        auto* twin = PrgramImpl::g_currentDrawBackendProgram;
        if (twin == nullptr || !twin->IsBackendProgramUsable() ||
            PrgramImpl::g_lastUsedBackendProgramId != twin->GetBackendProgramId())
            return;
        NarrowIntegerSeedState& state = twin->NarrowIntegerSeeds();
        if (!state.available) return;
        if (state.pushed8 != plan.widths8) {
            g_GLESFuncs.glUniform1ui(state.widths8Location, plan.widths8);
            state.pushed8 = plan.widths8;
        }
        if (state.pushed16 != plan.widths16) {
            g_GLESFuncs.glUniform1ui(state.widths16Location, plan.widths16);
            state.pushed16 = plan.widths16;
        }
    }

    void BeginNarrowIntegerSeedBuild(NarrowIntegerSeedState& state) {
        state.built = state.Target();
        state.declared = false;
        state.available = false;
        state.widths8Location = -1;
        state.widths16Location = -1;
        state.pushed8 = 0;
        state.pushed16 = 0;
    }

    void NoteNarrowIntegerSeedSource(NarrowIntegerSeedState& state, const String& source, GLenum shaderType) {
        if (!state.built || shaderType != GL_VERTEX_SHADER) return;
        state.declared = source.find(UniformDeclaration(kNarrowIntegerWidths8GlobalName)) != String::npos &&
                         source.find(UniformDeclaration(kNarrowIntegerWidths16GlobalName)) != String::npos;
    }

    void FinishNarrowIntegerSeedBuild(NarrowIntegerSeedState& state, GLuint program, Bool programUsable) {
        if (!state.built) return;
        if (!programUsable) {
            // The seeds may be why the build failed; the next build goes without them.
            state.disabled = true;
            MGLOG_E_ONCE("DirectGLES: a program built with narrow integer seeds did not link; it is rebuilt "
                         "without them, and its 8/16-bit integer attributes keep the driver's own value");
            return;
        }
        if (state.declared) {
            state.widths8Location = g_GLESFuncs.glGetUniformLocation(program, kNarrowIntegerWidths8GlobalName);
            state.widths16Location = g_GLESFuncs.glGetUniformLocation(program, kNarrowIntegerWidths16GlobalName);
        }
        state.available = state.widths8Location >= 0 && state.widths16Location >= 0;
        if (!state.available) {
            MGLOG_E_ONCE("DirectGLES: a program built with narrow integer seeds has no width uniforms (%s); its "
                         "8/16-bit integer attributes keep the driver's own value",
                         state.declared ? "the link dropped them" : "the vertex stage does not declare them");
        }
    }

    const Vector<unsigned int>* SeedNarrowIntegerVertexInputsForEssl(Bool armed, const Vector<unsigned int>& spirv,
                                                                     Vector<unsigned int>& storage,
                                                                     Bool enableSpirvValidation) {
        if (!armed) return &spirv;
        Bool changed = false;
        if (!MG_Util::ShaderTranspiler::ShaderCompiler::ExtendNarrowIntegerVertexInputsForEssl(spirv, storage, changed,
                                                                                               enableSpirvValidation)) {
            MGLOG_E_ONCE("DirectGLES: the narrow integer seed pass declined a vertex stage; it keeps the driver's "
                         "own value for 8/16-bit integer attributes");
            return &spirv;
        }
        return changed && !storage.empty() ? &storage : &spirv;
    }

    String PromoteNarrowIntegerWidthGlobals(String source) {
        for (const char* name : {kNarrowIntegerWidths8GlobalName, kNarrowIntegerWidths16GlobalName}) {
            // The zero-initialised Private global is printed with or without a precision qualifier;
            // only a declaration starting its own line is the global itself.
            for (const char* prefix : {"highp uint ", "mediump uint ", "lowp uint ", "uint "}) {
                Bool promoted = false;
                for (const char* initializer : {" = 0u;", ";"}) {
                    const String declaration = String(prefix) + name + initializer;
                    const SizeT pos = source.find(declaration);
                    if (pos == String::npos || (pos != 0 && source[pos - 1] != '\n')) continue;
                    source.replace(pos, declaration.size(), UniformDeclaration(name));
                    promoted = true;
                    break;
                }
                if (promoted) break;
            }
        }
        return source;
    }
} // namespace MobileGL::MG_Backend::DirectGLES
