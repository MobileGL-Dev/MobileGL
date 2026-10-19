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
    using MG_Util::ShaderTranspiler::IntegerVertexInputMasks;
    using MG_Util::ShaderTranspiler::NarrowIntegerVertexInputs;
    using MG_Util::ShaderTranspiler::ResolveNarrowIntegerVertexInputs;

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

    // The vertex-input half, filled from the source formats when a vertex input layout is built,
    // so a draw reads four words instead of walking its attributes.
    struct NarrowIntegerAttributeMasks : MG_Util::ShaderTranspiler::NarrowIntegerAttributeMasks {
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
} // namespace MobileGL::MG_Backend::DirectVulkan
