// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramState.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/Miscellany/IndexGenerator.h>
#include "ProgramObject.h"

namespace MobileGL::MG_State::GLState {
    class ProgramState {
    public:
        // This function WILL actually create the program object.
        // To retrieve created program object, use GetProgramObject()
        Uint CreateProgram();
        const SharedPtr<ProgramObject>& GetProgramObject(Uint id);
        void MarkProgramObjectForDeletion(Uint program);
        Bool ValidateProgramObject(Uint program) const;

        void UseProgram(Uint program);

        Uint CreateShader(ShaderStage stage);
        const SharedPtr<ShaderObject>& GetShaderObject(Uint shader);
        void MarkShaderObjectForDeletion(Uint shader);
        // Frees a deletion-flagged shader's name once no program holds a GL-visible
        // attachment to it (the deferred half of glDeleteShader-while-attached).
        void ReleaseShaderNameIfOrphaned(Uint shader);
        Bool ValidateShaderObject(Uint shader) const;

        const SharedPtr<ProgramObject>& GetCurrentProgram() const { return m_currentProgram; }

    private:
        Bool ShaderHasGLVisibleAttachment(const SharedPtr<ShaderObject>& shaderObject) const;
        // Frees the name slot and releases orphaned attached shaders; the immediate half
        // of glDeleteProgram (deferred while the program is current).
        void DestroyProgramSlot(Uint program);

        template <typename T>
        static Bool CheckIndexAvail(const SizeT idx, const Vector<T>& vec) {
            return idx < vec.size();
        }

        template <typename T>
        static void EnsureIndexAvail(const SizeT idx, Vector<T>& vec) {
            if (CheckIndexAvail(idx, vec)) return;

            vec.reserve(std::bit_ceil(idx));
            vec.resize(idx + 1);
        }

        // Programs and shaders share one GL name space (GL 3.3 core 2.11: a shader
        // name passed where a program is expected must be recognized as a shader and
        // rejected with INVALID_OPERATION, and vice versa). One generator for both
        // object kinds keeps the names disjoint; the object tables stay separate.
        IndexGenerator<Uint> m_programShaderNameGenerator;
        Vector<SharedPtr<ProgramObject>> m_programObjects;
        Vector<SharedPtr<ShaderObject>> m_shaderObjects;

        SharedPtr<ProgramObject> m_currentProgram;
    };
} // namespace MobileGL::MG_State::GLState
