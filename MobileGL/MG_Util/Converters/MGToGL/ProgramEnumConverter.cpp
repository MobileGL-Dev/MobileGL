// MobileGL - MobileGL/MG_Util/Converters/MGToGL/ProgramEnumConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ProgramEnumConverter.h"

namespace MobileGL {
    namespace MG_Util {
        GLenum ConvertShaderStageToGLEnum(ShaderStage stage) {
            switch (stage) {
            case ShaderStage::Vertex:
                return GL_VERTEX_SHADER;
            case ShaderStage::TessControl:
                return GL_TESS_CONTROL_SHADER;
            case ShaderStage::TessEval:
                return GL_TESS_EVALUATION_SHADER;
            case ShaderStage::Geometry:
                return GL_GEOMETRY_SHADER;
            case ShaderStage::Fragment:
                return GL_FRAGMENT_SHADER;
            case ShaderStage::Task:
                return GL_TASK_SHADER_NV;
            case ShaderStage::Mesh:
                return GL_MESH_SHADER_NV;
            case ShaderStage::Compute:
                return GL_COMPUTE_SHADER;
            default:
                return GL_UNKNOWN_MGL;
            }
        }
    } // namespace MG_Util
} // namespace MobileGL