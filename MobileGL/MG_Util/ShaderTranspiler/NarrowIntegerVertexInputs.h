// MobileGL - MobileGL/MG_Util/ShaderTranspiler/NarrowIntegerVertexInputs.h
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
            // The vertex input locations whose integer value a vertex module has to widen itself,
            // one bit per location in each mask, and how. GL hands an integer input the source
            // integer converted to 32 bits by the SOURCE type's signedness and then reinterpreted
            // as the declared int/uint type. A backend whose fetch has to carry the input's
            // signedness instead extends 8/16-bit data by the wrong rule:
            //  - zeroExtend*: an int input fed unsigned bytes/shorts, fetched sign-extended, is
            //    masked back to its low 8/16 bits;
            //  - signExtend*: a uint input fed signed bytes/shorts, fetched zero-extended, has its
            //    low 8/16 bits sign-extended.
            // Plain data so the shader pass, the program cache key and the draw path share it.
            struct NarrowIntegerVertexInputs {
                Uint32 zeroExtend8 = 0;
                Uint32 zeroExtend16 = 0;
                Uint32 signExtend8 = 0;
                Uint32 signExtend16 = 0;

                constexpr Bool Any() const { return (zeroExtend8 | zeroExtend16 | signExtend8 | signExtend16) != 0; }
                constexpr Bool operator==(const NarrowIntegerVertexInputs&) const = default;
            };
            // Hashed as raw bytes by the program cache, so it must stay free of padding.
            static_assert(sizeof(NarrowIntegerVertexInputs) == 4 * sizeof(Uint32));

            // The vertex-input half: the locations whose attribute DATA is narrow integer, by width
            // and signedness. Each backend fills it from its own description of the data.
            struct NarrowIntegerAttributeMasks {
                Uint32 unsigned8 = 0;
                Uint32 unsigned16 = 0;
                Uint32 signed8 = 0;
                Uint32 signed16 = 0;
            };

            // The program half: the locations whose shader input is a 32-bit int or uint scalar/vector.
            // Both masks are zero for a program without integer vertex inputs, which is what keeps the
            // common draw at four AND instructions.
            struct IntegerVertexInputMasks {
                Uint32 signedInputs = 0;
                Uint32 unsignedInputs = 0;

                constexpr void Note(Uint32 location, GLenum shaderInputType) {
                    if (location >= 32) return;
                    const Uint32 bit = 1u << location;
                    switch (shaderInputType) {
                    case GL_INT:
                    case GL_INT_VEC2:
                    case GL_INT_VEC3:
                    case GL_INT_VEC4:
                        signedInputs |= bit;
                        break;
                    case GL_UNSIGNED_INT:
                    case GL_UNSIGNED_INT_VEC2:
                    case GL_UNSIGNED_INT_VEC3:
                    case GL_UNSIGNED_INT_VEC4:
                        unsignedInputs |= bit;
                        break;
                    default:
                        break;
                    }
                }
            };

            // Unsigned narrow data into an int input is fetched sign-extended and must be zero-extended;
            // signed narrow data into a uint input is fetched zero-extended and must be sign-extended.
            // Matching signedness, 32-bit data and float inputs need nothing.
            constexpr NarrowIntegerVertexInputs ResolveNarrowIntegerVertexInputs(
                const IntegerVertexInputMasks& inputs, const NarrowIntegerAttributeMasks& data) {
                NarrowIntegerVertexInputs result;
                result.zeroExtend8 = inputs.signedInputs & data.unsigned8;
                result.zeroExtend16 = inputs.signedInputs & data.unsigned16;
                result.signExtend8 = inputs.unsignedInputs & data.signed8;
                result.signExtend16 = inputs.unsignedInputs & data.signed16;
                return result;
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
