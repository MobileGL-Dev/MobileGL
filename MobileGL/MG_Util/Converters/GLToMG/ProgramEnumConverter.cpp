// MobileGL - MobileGL/MG_Util/Converters/GLToMG/ProgramEnumConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ProgramEnumConverter.h"

namespace MobileGL {
    namespace MG_Util {
        ShaderStage ConvertGLEnumToShaderStage(GLenum type) {
            switch (type) {
            case GL_VERTEX_SHADER:
                return ShaderStage::Vertex;
            case GL_TESS_CONTROL_SHADER:
                return ShaderStage::TessControl;
            case GL_TESS_EVALUATION_SHADER:
                return ShaderStage::TessEval;
            case GL_GEOMETRY_SHADER:
                return ShaderStage::Geometry;
            case GL_FRAGMENT_SHADER:
                return ShaderStage::Fragment;
            case GL_TASK_SHADER_NV:
                return ShaderStage::Task;
            case GL_MESH_SHADER_NV:
                return ShaderStage::Mesh;
            case GL_COMPUTE_SHADER:
                return ShaderStage::Compute;
            default:
                return ShaderStage::Unknown;
            }
        }
    } // namespace MG_Util
} // namespace MobileGL