// MobileGL - MobileGL/MG_Util/ShaderTranspiler/Types.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            inline const char* GLOBAL_UBO_NAME = "MGL_GLOBAL_UBO";

            struct EmptyType {};

            enum class ShaderCompileBits : Uint {
                CompileForOpenGL = 1 << 0,
                EmitDiscardAsDemote = 1 << 1,
            };

            struct ShaderAttrib {
                GLenum shaderType;
                StringView sourceStr;
                Flags<ShaderCompileBits> flags;
            };

            struct ProgramAttrib {
                Vector<SharedPtr<glslang::TShader>> shaders;
                UnorderedMap<String, Uint> explicitVertexInLocations;
                UnorderedMap<String, Uint> explicitFragmentOutLocations;
                UnorderedMap<String, Uint>* explicitOpaqueUniformBindings = nullptr;
            };

            struct ProgramBinaryAttrib {
                Vector<GLenum> shaderTypes;
                const glslang::TProgram& program;
            };

            struct ResultInfo {
                Int errc = 0;
                String log;
            };

            template <typename T>
            using Result = std::expected<T, ResultInfo>;

            struct InterfaceVariable {
                String name;
                Uint32 location;

                Bool operator<(const InterfaceVariable& other) const { return location < other.location; }

                Bool operator==(const InterfaceVariable& other) const {
                    return location == other.location && name == other.name;
                }
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
