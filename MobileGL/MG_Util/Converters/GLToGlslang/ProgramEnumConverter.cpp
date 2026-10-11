// MobileGL - MobileGL/MG_Util/Converters/GLToGlslang/ProgramEnumConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ProgramEnumConverter.h"

namespace MobileGL {
    namespace MG_Util {
        EShLanguage ConvertGLEnumToEShLanguage(GLenum shaderType) {
            switch (shaderType) {
            case GL_VERTEX_SHADER:
                return EShLanguage::EShLangVertex;
            case GL_FRAGMENT_SHADER:
                return EShLanguage::EShLangFragment;
            case GL_COMPUTE_SHADER:
                return EShLanguage::EShLangCompute;
            case GL_TASK_SHADER_NV:
                return EShLanguage::EShLangTaskNV;
            case GL_MESH_SHADER_NV:
                return EShLanguage::EShLangMeshNV;
            case GL_TESS_CONTROL_SHADER:
                return EShLanguage::EShLangTessControl;
            case GL_TESS_EVALUATION_SHADER:
                return EShLanguage::EShLangTessEvaluation;
            case GL_GEOMETRY_SHADER:
                return EShLanguage::EShLangGeometry;
            default:
                return EShLanguage::EShLangCount;
            }
        }
    } // namespace MG_Util
} // namespace MobileGL