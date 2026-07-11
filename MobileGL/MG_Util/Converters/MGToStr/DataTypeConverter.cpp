// MobileGL - MobileGL/MG_Util/Converters/MGToStr/DataTypeConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DataTypeConverter.h"

namespace MobileGL {
    namespace MG_Util {
        String ConvertDataTypeToString(DataType type) {
            switch (type) {
            case DataType::Int8:
                return "Int8";
            case DataType::Uint8:
                return "Uint8";
            case DataType::Int16:
                return "Int16";
            case DataType::Uint16:
                return "Uint16";
            case DataType::Int32:
                return "Int32";
            case DataType::Uint32:
                return "Uint32";
            case DataType::Float16:
                return "Float16";
            case DataType::Float32:
                return "Float32";
            case DataType::Float64:
                return "Float64";
            case DataType::Fixed32:
                return "Fixed32";
            case DataType::Int2101010Rev:
                return "Int2101010Rev";
            case DataType::Uint2101010Rev:
                return "Uint2101010Rev";
            case DataType::Unknown:
            default:
                return "Unknown";
            }
        }
    } // namespace MG_Util
} // namespace MobileGL