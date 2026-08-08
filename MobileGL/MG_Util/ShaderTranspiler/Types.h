// MobileGL - MobileGL/MG_Util/ShaderTranspiler/Types.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/ShaderTranspiler/CompileEnv.h>

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
                // The compile-time backend snapshot the glslang resource limits come from.
                // Null means "read them off the live backend object" - only legal on the GL
                // thread, and only used by the standalone/test entry points. Non-owning: the
                // env outlives the attrib (it is a per-context SharedPtr).
                const CompileEnv* env = nullptr;
            };

            struct ProgramAttrib {
                Vector<SharedPtr<glslang::TShader>> shaders;
                UnorderedMap<String, Uint> explicitVertexInLocations;
                UnorderedMap<String, Uint> explicitFragmentOutLocations;
                // Dual-source blend color index per fragment output (glBindFragDataLocationIndexed) ->
                // emitted as layout(index = N).
                UnorderedMap<String, Uint> explicitFragmentOutIndices;
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
