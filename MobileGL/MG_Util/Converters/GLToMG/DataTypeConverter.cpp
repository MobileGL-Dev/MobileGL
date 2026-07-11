// MobileGL - MobileGL/MG_Util/Converters/GLToMG/DataTypeConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DataTypeConverter.h"

namespace MobileGL {
    namespace MG_Util {
        DataType ConvertGLEnumToDataType(GLenum type) {
            switch (type) {
            case GL_BYTE:
                return DataType::Int8;
            case GL_UNSIGNED_BYTE:
                return DataType::Uint8;
            case GL_SHORT:
                return DataType::Int16;
            case GL_UNSIGNED_SHORT:
                return DataType::Uint16;
            case GL_INT:
                return DataType::Int32;
            case GL_UNSIGNED_INT:
                return DataType::Uint32;
            case GL_FIXED:
                return DataType::Fixed32;
            case GL_HALF_FLOAT:
                return DataType::Float16;
            case GL_FLOAT:
                return DataType::Float32;
            case GL_DOUBLE:
                return DataType::Float64;
            case GL_INT_2_10_10_10_REV:
                return DataType::Int2101010Rev;
            case GL_UNSIGNED_INT_2_10_10_10_REV:
                return DataType::Uint2101010Rev;
            default:
                return DataType::Unknown;
            }
        }
    } // namespace MG_Util
} // namespace MobileGL
