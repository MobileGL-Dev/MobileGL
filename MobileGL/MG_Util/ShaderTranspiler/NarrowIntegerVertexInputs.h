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
            // one bit per location in each mask, and how. Every desktop GL driver hands an integer
            // input the source integer converted to 32 bits by the SOURCE type's signedness and
            // then reinterpreted as the declared int/uint type. A Vulkan fetch whose format has to
            // carry the input's signedness instead extends by the wrong rule for 8/16-bit data:
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
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
