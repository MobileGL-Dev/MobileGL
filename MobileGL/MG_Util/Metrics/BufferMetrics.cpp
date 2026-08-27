// MobileGL - MobileGL/MG_Util/Metrics/BufferMetrics.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BufferMetrics.h"

namespace MobileGL {
    namespace MG_Util {
        Uint32 FixedRestartIndexForGLType(GLenum indexType) {
            switch (indexType) {
            case GL_UNSIGNED_BYTE: return 0xFFu;
            case GL_UNSIGNED_SHORT: return 0xFFFFu;
            case GL_UNSIGNED_INT: return 0xFFFFFFFFu;
            default: return 0;
            }
        }

        SizeT GetGLTypeSize(GLenum type) {
            switch (type) {
            // Scalars
            case GL_BYTE:
            case GL_UNSIGNED_BYTE:
            case GL_UNSIGNED_BYTE_3_3_2:
                return 1;
            case GL_SHORT:
            case GL_UNSIGNED_SHORT:
            case GL_HALF_FLOAT:
            case GL_UNSIGNED_SHORT_5_6_5:
            case GL_UNSIGNED_SHORT_4_4_4_4:
            case GL_UNSIGNED_SHORT_5_5_5_1:
                return 2;
            case GL_BOOL:
            case GL_INT:
            case GL_UNSIGNED_INT:
            case GL_FLOAT:
            case GL_FIXED:
            case GL_INT_2_10_10_10_REV:
            case GL_UNSIGNED_INT_2_10_10_10_REV:
            case GL_UNSIGNED_INT_10F_11F_11F_REV:
            case GL_UNSIGNED_INT_5_9_9_9_REV:
                return 4;
            case GL_DOUBLE:
#ifdef GL_ARB_gpu_shader_int64
            case GL_UNSIGNED_INT64_ARB:
            case GL_INT64_ARB:
#endif
                return 8;

            // ---- Vector Types ----
            // 2-component vec
            case GL_FLOAT_VEC2:
            case GL_INT_VEC2:
            case GL_UNSIGNED_INT_VEC2:
            case GL_BOOL_VEC2:
                return 2 * sizeof(GLfloat); // GL_BOOL_VEC2 is also 2 * 4 bytes
            case GL_DOUBLE_VEC2:
                return 2 * sizeof(GLdouble);

            // 3-component vec
            case GL_FLOAT_VEC3:
            case GL_INT_VEC3:
            case GL_UNSIGNED_INT_VEC3:
            case GL_BOOL_VEC3:
                return 3 * sizeof(GLfloat);
            case GL_DOUBLE_VEC3:
                return 3 * sizeof(GLdouble);

            // 4-component vec
            case GL_FLOAT_VEC4:
            case GL_INT_VEC4:
            case GL_UNSIGNED_INT_VEC4:
            case GL_BOOL_VEC4:
                return 4 * sizeof(GLfloat);
            case GL_DOUBLE_VEC4:
                return 4 * sizeof(GLdouble);

            // ---- Matrix Types ----
            // Double non-square
            case GL_FLOAT_MAT2:
                return 2 * 2 * sizeof(GLfloat);
            case GL_FLOAT_MAT3:
                return 3 * 3 * sizeof(GLfloat);
            case GL_FLOAT_MAT4:
                return 4 * 4 * sizeof(GLfloat);

            // Float non-square
            case GL_FLOAT_MAT2x3:
                return 2 * 3 * sizeof(GLfloat);
            case GL_FLOAT_MAT2x4:
                return 2 * 4 * sizeof(GLfloat);
            case GL_FLOAT_MAT3x2:
                return 3 * 2 * sizeof(GLfloat);
            case GL_FLOAT_MAT3x4:
                return 3 * 4 * sizeof(GLfloat);
            case GL_FLOAT_MAT4x2:
                return 4 * 2 * sizeof(GLfloat);
            case GL_FLOAT_MAT4x3:
                return 4 * 3 * sizeof(GLfloat);

            // Double mat
            case GL_DOUBLE_MAT2:
                return 2 * 2 * sizeof(GLdouble);
            case GL_DOUBLE_MAT3:
                return 3 * 3 * sizeof(GLdouble);
            case GL_DOUBLE_MAT4:
                return 4 * 4 * sizeof(GLdouble);

            // Double non-square
            case GL_DOUBLE_MAT2x3:
                return 2 * 3 * sizeof(GLdouble);
            case GL_DOUBLE_MAT2x4:
                return 2 * 4 * sizeof(GLdouble);
            case GL_DOUBLE_MAT3x2:
                return 3 * 2 * sizeof(GLdouble);
            case GL_DOUBLE_MAT3x4:
                return 3 * 4 * sizeof(GLdouble);
            case GL_DOUBLE_MAT4x2:
                return 4 * 2 * sizeof(GLdouble);
            case GL_DOUBLE_MAT4x3:
                return 4 * 3 * sizeof(GLdouble);

            default:
                MOBILEGL_ASSERT(false, "Unknown GL Type!");
                return -1;
            }
        }
    } // namespace MG_Util
} // namespace MobileGL
