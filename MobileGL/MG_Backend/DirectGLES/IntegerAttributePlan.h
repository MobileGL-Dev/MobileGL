// MobileGL - MobileGL/MG_Backend/DirectGLES/IntegerAttributePlan.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include <Includes.h>
#include <MG_Util/ShaderTranspiler/NarrowIntegerVertexInputs.h>

namespace MobileGL::MG_Backend::DirectGLES {
    using MG_Util::ShaderTranspiler::IntegerVertexInputMasks;

    // What one draw does with its integer vertex arrays. ES defines an integer attribute's value
    // only when the array's type has the input's signedness, so every integer array feeding an
    // integer input of the other signedness is ISSUED with the input's signedness (retypeMask).
    // For 32-bit data that is the same bits, which is GL's value. For 8/16-bit data the driver
    // then extends by the input's rule, and the program's seeds (ExtendNarrowIntegerVertexInputs-
    // Pass's RuntimeWidths mode) widen the low 8 or 16 bits again by the data's rule: widths8 and
    // widths16 are the locations to widen, and they are the two uniforms the draw sets.
    struct IntegerAttributePlan {
        Uint32 retypeMask = 0;
        // Integer arrays feeding integer inputs: the only locations whose issued type is
        // observable, so a VAO keeps whatever it holds for every other location.
        Uint32 careMask = 0;
        Uint32 widths8 = 0;
        Uint32 widths16 = 0;

        constexpr Bool NeedsSeeds() const { return (widths8 | widths16) != 0; }
        // The plan for a program without working seeds: a narrow array keeps the application's
        // type (the value the driver gives today) rather than a retype nothing widens back.
        constexpr IntegerAttributePlan WithoutSeeds() const {
            IntegerAttributePlan plan = *this;
            plan.retypeMask &= ~(widths8 | widths16);
            plan.widths8 = 0;
            plan.widths16 = 0;
            return plan;
        }
        constexpr Bool operator==(const IntegerAttributePlan&) const = default;
    };

    // The locations whose integer array data is of each width and signedness.
    struct IntegerAttributeDataMasks {
        MG_Util::ShaderTranspiler::NarrowIntegerAttributeMasks narrow;
        Uint32 unsigned32 = 0;
        Uint32 signed32 = 0;

        constexpr void Note(Uint32 location, DataType type) {
            if (location >= 32) return;
            const Uint32 bit = 1u << location;
            switch (type) {
            case DataType::Uint8:
                narrow.unsigned8 |= bit;
                break;
            case DataType::Uint16:
                narrow.unsigned16 |= bit;
                break;
            case DataType::Uint32:
                unsigned32 |= bit;
                break;
            case DataType::Int8:
                narrow.signed8 |= bit;
                break;
            case DataType::Int16:
                narrow.signed16 |= bit;
                break;
            case DataType::Int32:
                signed32 |= bit;
                break;
            default:
                break;
            }
        }
        constexpr Uint32 Unsigned() const { return narrow.unsigned8 | narrow.unsigned16 | unsigned32; }
        constexpr Uint32 Signed() const { return narrow.signed8 | narrow.signed16 | signed32; }
    };

    // `seedsUsable` is false once the program's armed build could not give it working seeds.
    constexpr IntegerAttributePlan ResolveIntegerAttributePlan(const IntegerVertexInputMasks& inputs,
                                                               const IntegerAttributeDataMasks& data,
                                                               Bool seedsUsable) {
        IntegerAttributePlan plan;
        plan.careMask = (inputs.signedInputs | inputs.unsignedInputs) & (data.Unsigned() | data.Signed());
        plan.retypeMask = (inputs.signedInputs & data.Unsigned()) | (inputs.unsignedInputs & data.Signed());
        const auto narrow = MG_Util::ShaderTranspiler::ResolveNarrowIntegerVertexInputs(inputs, data.narrow);
        plan.widths8 = narrow.zeroExtend8 | narrow.signExtend8;
        plan.widths16 = narrow.zeroExtend16 | narrow.signExtend16;
        return seedsUsable ? plan : plan.WithoutSeeds();
    }

    // The array type to issue: the application's, or its counterpart of the other signedness.
    constexpr GLenum IntegerAttribTypeForInput(GLenum applicationType, Bool retype) {
        if (!retype) return applicationType;
        switch (applicationType) {
        case GL_BYTE:
            return GL_UNSIGNED_BYTE;
        case GL_UNSIGNED_BYTE:
            return GL_BYTE;
        case GL_SHORT:
            return GL_UNSIGNED_SHORT;
        case GL_UNSIGNED_SHORT:
            return GL_SHORT;
        case GL_INT:
            return GL_UNSIGNED_INT;
        case GL_UNSIGNED_INT:
            return GL_INT;
        default:
            return applicationType;
        }
    }

    // What a vertex array issues after this plan: the plan's choice wherever it can be observed,
    // what it already holds everywhere else, so a program switch that only changes which
    // locations are read does not re-issue any pointer.
    constexpr Uint32 HeldIntegerRetype(Uint32 held, const IntegerAttributePlan& plan) {
        return (held & ~plan.careMask) | (plan.retypeMask & plan.careMask);
    }
} // namespace MobileGL::MG_Backend::DirectGLES
