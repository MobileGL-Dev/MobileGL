// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkClearManager.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "../VkIncludes.h"
#include "../VulkanRendererConfig.h"
#include "MG_State/GLState/FramebufferState/FramebufferObject.h"
#include "MG_Util/Math/VectorTypes.h"

#include <Includes.h>
#include <atomic>
#include <unordered_map>

namespace MobileGL::MG_Backend::DirectVulkan {
    struct ClearFramebufferPayload {
        FloatVec4 color;
        Float depth{};
        Uint32 stencil{};
    };

    // A colour clear reaches us from one of glClear/ClearBufferfv, ClearBufferiv or
    // ClearBufferuiv, and Vulkan reads VkClearColorValue's union according to the destination
    // image's format rather than converting between the members - a float written where an
    // integer format is expected is reinterpreted bit for bit, not rounded. Remember which entry
    // point supplied the value so the member written when the clear is materialized matches.
    enum class ClearColorEncoding : Uint8 { Float, Int, Uint };

    struct ClearAttachmentPayload {
        GLbitfield mask = 0;
        FloatVec4 color = FloatVec4(0.0f, 0.0f, 0.0f, 0.0f);
        ClearColorEncoding colorEncoding = ClearColorEncoding::Float;
        IntVec4 colorInt = IntVec4(0, 0, 0, 0);
        UintVec4 colorUint = UintVec4(0u, 0u, 0u, 0u);
        Float depth = 1.0f;
        Uint32 stencil = 0;
    };

    // Builds the clear value for `payload` in the union member its encoding calls for.
    // `formatLacksAlpha` applies GL's rule that a format without an alpha channel reads as one,
    // expressed in whichever type matches (GL 4.6 core 15.2.3).
    VkClearColorValue MakeVkClearColorValue(const ClearAttachmentPayload& payload, Bool formatLacksAlpha);

    // vkCmdClearColorImage names the image, so the driver applies the destination format's transfer
    // function to whatever value it is handed. Every other write path in this backend goes through
    // the UNORM twin view while GL_FRAMEBUFFER_SRGB is off (ResolveSrgbAttachmentWriteFormat) and
    // therefore stores the raw value GL asked for. Rewrites `payload` to the linear colour whose
    // encoding is that raw value, so a direct image clear of an sRGB destination agrees with them.
    // A no-op for every other format, for integer clear encodings, and when GL is doing the
    // encoding itself.
    void PreCompensateSrgbClearColor(ClearAttachmentPayload& payload, VkFormat destinationFormat);

} // namespace MobileGL::MG_Backend::DirectVulkan
