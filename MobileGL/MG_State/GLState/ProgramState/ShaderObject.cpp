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
#include <MG_Backend/BackendObjects.h>

namespace {
    struct ComputeLocalSize {
        MobileGL::Uint x = 1;
        MobileGL::Uint y = 1;
        MobileGL::Uint z = 1;
        bool declared = false;
    };

    static MobileGL::String StripGlslComments(const MobileGL::String& source) {
        MobileGL::String result;
        result.reserve(source.length());

        bool inLineComment = false;
        bool inBlockComment = false;
        for (MobileGL::SizeT i = 0; i < source.length(); ++i) {
            if (inLineComment) {
                if (source[i] == '\n') {
                    inLineComment = false;
                    result.push_back(source[i]);
                } else {
                    result.push_back(' ');
                }
                continue;
            }

            if (inBlockComment) {
                if (source[i] == '*' && i + 1 < source.length() && source[i + 1] == '/') {
                    inBlockComment = false;
                    result.append("  ");
                    ++i;
                } else {
                    result.push_back(source[i] == '\n' ? '\n' : ' ');
                }
                continue;
            }

            if (source[i] == '/' && i + 1 < source.length()) {
                if (source[i + 1] == '/') {
                    inLineComment = true;
                    result.append("  ");
                    ++i;
                    continue;
                }
                if (source[i + 1] == '*') {
                    inBlockComment = true;
                    result.append("  ");
                    ++i;
                    continue;
                }
            }

            result.push_back(source[i]);
        }

        return result;
    }

    static ComputeLocalSize ParseComputeLocalSize(const MobileGL::String& source) {
        ComputeLocalSize localSize;
        const MobileGL::String uncommentedSource = StripGlslComments(source);
        const std::regex localSizePattern(R"(local_size_([xyz])\s*=\s*([0-9]+))");

        for (std::sregex_iterator it(uncommentedSource.begin(), uncommentedSource.end(), localSizePattern), end;
             it != end; ++it) {
            const char axis = (*it)[1].str()[0];
            const auto value = static_cast<unsigned long long>(std::stoull((*it)[2].str()));
            const MobileGL::Uint clampedValue = value > UINT_MAX ? UINT_MAX : static_cast<MobileGL::Uint>(value);

            // TODO: Replace this literal layout scanner with parser/AST-backed validation so expressions and
            // specialization-id layouts are handled consistently with glslang.
            localSize.declared = true;
            if (axis == 'x') {
                localSize.x = clampedValue;
            } else if (axis == 'y') {
                localSize.y = clampedValue;
            } else {
                localSize.z = clampedValue;
            }
        }

        return localSize;
    }

    static MobileGL::Uint GetComputeWorkGroupSizeLimit(MobileGL::Uint index) {
        constexpr MobileGL::Uint kFrontendMinComputeWorkGroupSizes[] = {1024, 1024, 64};
        MobileGL::Int backendValue = 0;
        if (MobileGL::MG_Backend::gBackendFunctionsTable.GL.GetIntegeri_v) {
            MobileGL::MG_Backend::gBackendFunctionsTable.GL.GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, index,
                                                                          &backendValue);
        }

        // TODO: Share these exposed compute limit helpers with GL_Getter.cpp instead of duplicating the frontend minima.
        return std::max(static_cast<MobileGL::Uint>(std::max(backendValue, 0)),
                        kFrontendMinComputeWorkGroupSizes[index]);
    }

    static unsigned long long GetComputeWorkGroupInvocationLimit() {
        constexpr unsigned long long kFrontendMaxComputeWorkGroupInvocations = 1024;
        if (!MobileGL::MG_Backend::pActiveBackendObject) return kFrontendMaxComputeWorkGroupInvocations;

        return std::max(static_cast<unsigned long long>(std::max(
                            MobileGL::MG_Backend::pActiveBackendObject->GetDynamicParameters()
                                .MaxComputeWorkGroupInvocations,
                            0)),
                        kFrontendMaxComputeWorkGroupInvocations);
    }

    static std::optional<MobileGL::String> ValidateComputeLocalSizeLimits(const MobileGL::String& source) {
        const ComputeLocalSize localSize = ParseComputeLocalSize(source);
        if (!localSize.declared) return std::nullopt;

        if (localSize.x > GetComputeWorkGroupSizeLimit(0) || localSize.y > GetComputeWorkGroupSizeLimit(1) ||
            localSize.z > GetComputeWorkGroupSizeLimit(2)) {
            return "Compute shader local_size exceeds GL_MAX_COMPUTE_WORK_GROUP_SIZE.";
        }

        const unsigned long long invocations = static_cast<unsigned long long>(localSize.x) * localSize.y * localSize.z;
        if (invocations > GetComputeWorkGroupInvocationLimit()) {
            return "Compute shader local_size product exceeds GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS.";
        }

        return std::nullopt;
    }
}

namespace MobileGL::MG_State::GLState {
    void ShaderObject::SetShaderSource(const String& source) {
        m_source = source;
        m_shader.reset();
        m_compileStatus = false;
        m_infoLog.clear();
    }

    void ShaderObject::SetShaderSource(String&& source) {
        m_source = Move(source);
        m_shader.reset();
        m_compileStatus = false;
        m_infoLog.clear();
    }

    void ShaderObject::Compile() {
        using namespace MG_Util::ShaderTranspiler;
        String compileSource = m_source;
        MG_Util::ShaderTranspiler::PreprocessShaderSource(m_stage, compileSource);

        if (m_stage == ShaderStage::Compute) {
            const std::optional<String> localSizeError = ValidateComputeLocalSizeLimits(compileSource);
            if (localSizeError) {
                m_compileStatus = false;
                m_shader.reset();
                m_infoLog = *localSizeError;
                return;
            }
        }

        // Compile for OpenGL here, so that we can do validation and link
        // like a real OpenGL driver at linking stage
        // Will compile for other backends later.
        ShaderAttrib attrib{.shaderType = MG_Util::ConvertShaderStageToGLEnum(m_stage),
                            .sourceStr = compileSource,
                            .flags = ShaderCompileBits::CompileForOpenGL};

        auto result = ShaderCompiler::CompileShader(attrib);
        if (result) {
            m_compileStatus = true;
            m_shader = result.value();
            m_infoLog.clear();
        } else {
            m_compileStatus = false;
            m_shader.reset();
            m_infoLog = result.error().log;
            MGLOG_D("ShaderObject::Compile: Shader %d compilation failed.\nSource:\n%s\nInfoLog:\n%s\nSetting "
                    "m_compileStatus = false as a result.",
                    m_externalIndex, compileSource.c_str(), m_infoLog.c_str());
        }
    }

    void ShaderObject::MarkAsDeleted() {
        m_deleteStatus = true;
    }
} // namespace MobileGL::MG_State::GLState
