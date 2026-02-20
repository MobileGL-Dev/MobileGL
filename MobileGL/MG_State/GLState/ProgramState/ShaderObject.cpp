// MobileGL - MobileGL/MG_State/GLState/ProgramState/ShaderObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ShaderObject.h"
#include <MG_Util/ShaderTranspiler/Types.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/Converters/MGToGL/ProgramEnumConverter.h>
#include <MG_Util/ShaderTranspiler/ShaderSourceProcessor.h>
#include <MG_Util/ShaderTranspiler/glslang/UniformTraverser.h>

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            void ShaderObject::SetShaderSource(const String& source) {
                m_source = source;
            }

            void ShaderObject::SetShaderSource(String&& source) {
                m_source = Move(source);
            }

            void ShaderObject::Compile() {
                using namespace MG_Util::ShaderTranspiler;
                MG_Util::ShaderTranspiler::PreprocessShaderSource(m_stage, m_source);

                // Compile for OpenGL here, so that we can do validation and link
                // like a real OpenGL driver at linking stage
                // Will compile for other backends later.
                ShaderAttrib attrib{.shaderType = MG_Util::ConvertShaderStageToGLEnum(m_stage),
                                    .sourceStr = m_source,
                                    .flags = ShaderCompileBits::CompileForOpenGL};

                auto result = ShaderCompiler::CompileShader(attrib);
                if (result) {
                    m_compileStatus = true;
                    m_shader = result.value();
                } else {
                    m_compileStatus = false;
                    m_infoLog = result.error().log;
                    MGLOG_D("ShaderObject::Compile: Shader %d compilation failed.\nSource:\n%s\nInfoLog:\n%s\nSetting "
                            "m_compileStatus = false as a result.",
                            m_externalIndex, m_source.c_str(), m_infoLog.c_str());
                }
            }

            void ShaderObject::MarkAsDeleted() {
                m_deleteStatus = true;
            }
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
