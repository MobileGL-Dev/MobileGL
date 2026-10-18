// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/NarrowIntegerVertexFormats.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "../VkIncludes.h"
#include <MG_Util/ShaderTranspiler/NarrowIntegerVertexInputs.h>

#include <Includes.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    using MG_Util::ShaderTranspiler::NarrowIntegerVertexInputs;

    // Which vertex inputs need the ExtendNarrowIntegerVertexInputs program variant, decided from
    // two halves that are each computed once and cached with the object they describe.
    //
    // The pipeline gives an integer attribute the fetch format of the shader input's signedness
    // (VUID-VkGraphicsPipelineCreateInfo-Input-08733). For 32-bit data that swap only renames the
    // bits; for 8/16-bit data it changes the extension rule, so 240 in GL_UNSIGNED_BYTE data
    // reaches an `int` input as -16. These are exactly the (data, input) pairs that need it.
    enum class NarrowIntegerVertexFormat : Uint8 {
        None = 0,
        Unsigned8,
        Unsigned16,
        Signed8,
        Signed16,
    };

    constexpr NarrowIntegerVertexFormat ClassifyNarrowIntegerVertexFormat(VkFormat format) {
        switch (format) {
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8B8_UINT:
        case VK_FORMAT_R8G8B8A8_UINT:
            return NarrowIntegerVertexFormat::Unsigned8;
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R16G16B16_UINT:
        case VK_FORMAT_R16G16B16A16_UINT:
            return NarrowIntegerVertexFormat::Unsigned16;
        case VK_FORMAT_R8_SINT:
        case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R8G8B8_SINT:
        case VK_FORMAT_R8G8B8A8_SINT:
            return NarrowIntegerVertexFormat::Signed8;
        case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16B16_SINT:
        case VK_FORMAT_R16G16B16A16_SINT:
            return NarrowIntegerVertexFormat::Signed16;
        default:
            return NarrowIntegerVertexFormat::None;
        }
    }

    // The vertex-input half: the locations whose attribute DATA is narrow integer, by width and
    // signedness. Filled from the source formats when a vertex input layout is built, so a draw
    // reads four words instead of walking its attributes.
    struct NarrowIntegerAttributeMasks {
        Uint32 unsigned8 = 0;
        Uint32 unsigned16 = 0;
        Uint32 signed8 = 0;
        Uint32 signed16 = 0;

        constexpr void Note(Uint32 location, VkFormat format) {
            if (location >= 32) return;
            const Uint32 bit = 1u << location;
            switch (ClassifyNarrowIntegerVertexFormat(format)) {
            case NarrowIntegerVertexFormat::Unsigned8:
                unsigned8 |= bit;
                break;
            case NarrowIntegerVertexFormat::Unsigned16:
                unsigned16 |= bit;
                break;
            case NarrowIntegerVertexFormat::Signed8:
                signed8 |= bit;
                break;
            case NarrowIntegerVertexFormat::Signed16:
                signed16 |= bit;
                break;
            case NarrowIntegerVertexFormat::None:
                break;
            }
        }
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
    constexpr NarrowIntegerVertexInputs ResolveNarrowIntegerVertexInputs(const IntegerVertexInputMasks& inputs,
                                                                         const NarrowIntegerAttributeMasks& data) {
        NarrowIntegerVertexInputs result;
        result.zeroExtend8 = inputs.signedInputs & data.unsigned8;
        result.zeroExtend16 = inputs.signedInputs & data.unsigned16;
        result.signExtend8 = inputs.unsignedInputs & data.signed8;
        result.signExtend16 = inputs.unsignedInputs & data.signed16;
        return result;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
