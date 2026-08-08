// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramState.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ProgramState.h"

namespace MobileGL::MG_State::GLState {
    Uint ProgramState::CreateProgram() {
        Uint programId = 0;
        m_programShaderNameGenerator.Generate(1, &programId);
        EnsureIndexAvail(programId, m_programObjects);
        auto programObject = MakeShared<ProgramObject>(programId);
        if (programObject == nullptr) return 0;
        m_programObjects[programId] = programObject;
        return programId;
    }

    const SharedPtr<ProgramObject>& ProgramState::GetProgramObject(const Uint id) {
        static SharedPtr<ProgramObject> nullProgramObject = nullptr;
        if (!CheckIndexAvail(id, m_programObjects)) return nullProgramObject; // FIXME: add error reporting here
        return m_programObjects[id];
    }

    void ProgramState::MarkProgramObjectForDeletion(const Uint program) {
        if (!CheckIndexAvail(program, m_programObjects)) return; // FIXME: add error reporting here
        auto& programObject = m_programObjects[program];
        if (programObject != nullptr) {
            programObject->MarkAsDeleted();
            // A program in use is only FLAGGED: its name (and every program query) stays
            // valid until it stops being current, at which point UseProgram finishes the job.
            if (programObject == m_currentProgram) return;
            DestroyProgramSlot(program);
        }
    }

    void ProgramState::DestroyProgramSlot(const Uint program) {
        auto& programObject = m_programObjects[program];
        // Snapshot the attachments: deleting the program is a detach point for shaders
        // that were flagged with glDeleteShader while still attached.
        const Vector<SharedPtr<ShaderObject>> attachedShaders = programObject->GetAttachedShaders();
        programObject.reset();
        m_programShaderNameGenerator.Delete(program);
        for (const auto& shader : attachedShaders) {
            const Uint shaderName = shader->GetExternalIndex();
            if (CheckIndexAvail(shaderName, m_shaderObjects) && m_shaderObjects[shaderName] == shader) {
                ReleaseShaderNameIfOrphaned(shaderName);
            }
        }
    }

    Bool ProgramState::ValidateProgramObject(const Uint program) const {
        return CheckIndexAvail(program, m_programObjects) && m_programObjects[program] != nullptr;
    }

    void ProgramState::UseProgram(Uint program) {
        const SharedPtr<ProgramObject> previous = m_currentProgram;

        if (program == 0) m_currentProgram.reset();

        if (CheckIndexAvail(program, m_programObjects)) {
            m_currentProgram = m_programObjects[program];
        }

        // A deletion flagged while the program was current takes effect the moment it
        // stops being current.
        if (previous != nullptr && previous != m_currentProgram && previous->GetDeleteStatus()) {
            const Uint previousName = previous->GetExternalIndex();
            if (CheckIndexAvail(previousName, m_programObjects) && m_programObjects[previousName] == previous) {
                DestroyProgramSlot(previousName);
            }
        }
    }

    Uint ProgramState::CreateShader(ShaderStage stage) {
        Uint shaderId = 0;
        m_programShaderNameGenerator.Generate(1, &shaderId);
        EnsureIndexAvail(shaderId, m_shaderObjects);
        auto shaderObject = MakeShared<ShaderObject>(stage, shaderId);
        if (shaderObject == nullptr) return 0;
        m_shaderObjects[shaderId] = shaderObject;
        return shaderId;
    }

    const SharedPtr<ShaderObject>& ProgramState::GetShaderObject(const Uint shader) {
        static SharedPtr<ShaderObject> nullShaderObject = nullptr;
        if (!CheckIndexAvail(shader, m_shaderObjects)) return nullShaderObject;
        return m_shaderObjects[shader];
    }

    void ProgramState::MarkShaderObjectForDeletion(Uint shader) {
        if (!CheckIndexAvail(shader, m_shaderObjects)) return;
        auto& shaderObject = m_shaderObjects[shader];
        if (shaderObject != nullptr) {
            // glDeleteShader on an attached shader only FLAGS it; the name stays valid (and
            // glShaderSource/glCompileShader keep working on it) until the shader is detached
            // from every program. The GL CTS compiles shaders through exactly this
            // create-attach-delete-source-compile sequence (uniform_block.common.name_matching).
            shaderObject->MarkAsDeleted();
            ReleaseShaderNameIfOrphaned(shader);
        }
    }

    Bool ProgramState::ShaderHasGLVisibleAttachment(const SharedPtr<ShaderObject>& shaderObject) const {
        for (const auto& programObject : m_programObjects) {
            if (programObject != nullptr && programObject->ShaderIsAttachedGLVisible(shaderObject)) {
                return true;
            }
        }
        // A program deleted while current vacates its table slot but stays alive as the
        // current program; its attachments still count.
        return m_currentProgram != nullptr && m_currentProgram->ShaderIsAttachedGLVisible(shaderObject);
    }

    void ProgramState::ReleaseShaderNameIfOrphaned(Uint shader) {
        if (!CheckIndexAvail(shader, m_shaderObjects)) return;
        auto& shaderObject = m_shaderObjects[shader];
        if (shaderObject == nullptr || !shaderObject->GetDeleteStatus()) return;
        if (ShaderHasGLVisibleAttachment(shaderObject)) return;
        shaderObject.reset();
        m_programShaderNameGenerator.Delete(shader);
    }

    Bool ProgramState::ValidateShaderObject(Uint shader) const {
        return CheckIndexAvail(shader, m_shaderObjects) && m_shaderObjects[shader] != nullptr;
    }
} // namespace MobileGL::MG_State::GLState
