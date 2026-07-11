// MobileGL - MobileGL/MG_Util/Converters/MGToGL/DataTypeConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DataTypeConverter.h"

namespace MobileGL {
    namespace MG_Util {
        GLenum ConvertDataTypeToGLEnum(DataType type) {
            switch (type) {
            case DataType::Int8:
                return GL_BYTE;
            case DataType::Uint8:
                return GL_UNSIGNED_BYTE;
            case DataType::Int16:
                return GL_SHORT;
            case DataType::Uint16:
                return GL_UNSIGNED_SHORT;
            case DataType::Int32:
                return GL_INT;
            case DataType::Uint32:
                return GL_UNSIGNED_INT;
            case DataType::Fixed32:
                return GL_FIXED;
            case DataType::Float16:
                return GL_HALF_FLOAT;
            case DataType::Float32:
                return GL_FLOAT;
            case DataType::Float64:
                return GL_DOUBLE;
            case DataType::Int2101010Rev:
                return GL_INT_2_10_10_10_REV;
            case DataType::Uint2101010Rev:
                return GL_UNSIGNED_INT_2_10_10_10_REV;
            default:
                return GL_UNKNOWN_MGL;
            }
        }
    } // namespace MG_Util
} // namespace MobileGL
