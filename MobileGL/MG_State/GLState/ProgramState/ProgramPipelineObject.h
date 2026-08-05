// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramPipelineObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "ProgramObject.h"

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            // A GL_ARB_separate_shader_objects program pipeline (GL 4.6 core 7.4): a set of
            // per-stage program references plus the program glProgramUniform* addresses when no
            // program object is in use.
            class ProgramPipelineObject {
            public:
                explicit ProgramPipelineObject(Uint externalIndex) : m_externalIndex(externalIndex) {}

                const SharedPtr<ProgramObject>& GetStageProgram(ShaderStage stage) const {
                    return m_stagePrograms[static_cast<SizeT>(stage)];
                }
                void SetStageProgram(ShaderStage stage, const SharedPtr<ProgramObject>& program) {
                    m_stagePrograms[static_cast<SizeT>(stage)] = program;
                }

                const SharedPtr<ProgramObject>& GetActiveProgram() const { return m_activeProgram; }
                void SetActiveProgram(const SharedPtr<ProgramObject>& program) { m_activeProgram = program; }

                // Distinct from ProgramObject's, which starts true: a pipeline that has never been
                // validated must report GL_VALIDATE_STATUS as 0 (GL 4.6 core table 23.31).
                Bool GetValidateStatus() const { return m_validateStatus; }
                void SetValidateStatus(Bool value) { m_validateStatus = value; }

                const String& GetInfoLog() const { return m_infoLog; }
                void SetInfoLog(String log) { m_infoLog = Move(log); }

                Uint GetExternalIndex() const { return m_externalIndex; }

            private:
                Array<SharedPtr<ProgramObject>, static_cast<SizeT>(ShaderStage::ShaderStageCount)> m_stagePrograms{};
                SharedPtr<ProgramObject> m_activeProgram;
                String m_infoLog;
                const Uint m_externalIndex = 0;
                Bool m_validateStatus = false;
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
