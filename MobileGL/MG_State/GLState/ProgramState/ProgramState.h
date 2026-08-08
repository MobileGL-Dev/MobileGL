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
#include "ShaderPreprocessCache.h"

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

        // Joins every outstanding compile and link this context still owns, publishing each
        // one's artifacts through the ordinary gates. The single caller is
        // glMaxShaderCompilerThreadsKHR(0): GL_KHR_parallel_shader_compile requires a zero
        // count to leave nothing in flight, so that every subsequent
        // GL_COMPLETION_STATUS_KHR reads GL_TRUE.
        //
        // NOT a teardown path and NOT ShaderCompilePool::StopAndDrain(): the pool keeps its
        // threads and stays usable, because a later nonzero count has to bring asynchronous
        // compilation straight back. Nodes belonging to objects this context has already
        // dropped are not joined - nothing can observe them, and waiting on them would make
        // a GL call's cost depend on garbage.
        void JoinAllPendingWork();

        // P0b layer 2. Exposed for tests and diagnostics; the GL frontend never touches it
        // directly - shader objects reach it through the pointer they are handed at
        // CreateShader().
        ShaderPreprocessCache& GetShaderPreprocessCache() { return *m_shaderPreprocessCache; }

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

        // P0b layer 2: every shader object created here is handed shared ownership of this
        // cache, so its lifetime no longer depends on member destruction order (P1: an
        // in-flight compile job may outlive the context). The FIRST-member declaration is
        // kept anyway - it costs nothing and documents the intent.
        SharedPtr<ShaderPreprocessCache> m_shaderPreprocessCache = MakeShared<ShaderPreprocessCache>();

        Vector<SharedPtr<ProgramObject>> m_programObjects;
        Vector<SharedPtr<ShaderObject>> m_shaderObjects;

        SharedPtr<ProgramObject> m_currentProgram;
    };
} // namespace MobileGL::MG_State::GLState
