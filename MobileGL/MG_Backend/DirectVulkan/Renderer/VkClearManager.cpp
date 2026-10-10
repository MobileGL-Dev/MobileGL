// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkClearManager.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VkClearManager.h"

// For the shared ResolveAttachmentLayerCount (and the ToVulkanLevelExtent it is built on): the
// clear key's layer span has to be the same one the render pass builds its attachment view from.
#include "VkTextureManager.h"

#include "MG_State/GLState/Core.h"
#include <MG_Backend/MGPipe/PipeInputs.h>
#include "MG_Util/Converters/MGToStr/FramebufferEnumConverter.h"
#include "MG_Util/Converters/MGToStr/TextureEnumConverter.h"

#include <algorithm>
#include <cmath>

namespace MobileGL::MG_Backend::DirectVulkan {
    static Bool IsCubeMapFaceUploadTarget(TextureUploadTarget target) {
        return target >= TextureUploadTarget::CubeMapPositiveX &&
               target <= TextureUploadTarget::CubeMapNegativeZ;
    }

    VkClearColorValue MakeVkClearColorValue(const ClearAttachmentPayload& payload, Bool formatLacksAlpha) {
        VkClearColorValue clearValue{};
        switch (payload.colorEncoding) {
        case ClearColorEncoding::Int:
            clearValue.int32[0] = payload.colorInt.x();
            clearValue.int32[1] = payload.colorInt.y();
            clearValue.int32[2] = payload.colorInt.z();
            clearValue.int32[3] = formatLacksAlpha ? 1 : payload.colorInt.w();
            break;
        case ClearColorEncoding::Uint:
            clearValue.uint32[0] = payload.colorUint.x();
            clearValue.uint32[1] = payload.colorUint.y();
            clearValue.uint32[2] = payload.colorUint.z();
            clearValue.uint32[3] = formatLacksAlpha ? 1u : payload.colorUint.w();
            break;
        case ClearColorEncoding::Float:
            clearValue.float32[0] = payload.color.x();
            clearValue.float32[1] = payload.color.y();
            clearValue.float32[2] = payload.color.z();
            clearValue.float32[3] = formatLacksAlpha ? 1.0f : payload.color.w();
            break;
        }
        return clearValue;
    }

    void PreCompensateSrgbClearColor(ClearAttachmentPayload& payload, VkFormat destinationFormat) {
        if (payload.colorEncoding != ClearColorEncoding::Float) return;
        // With GL_FRAMEBUFFER_SRGB enabled GL performs the encoding itself, so the driver doing it
        // is exactly right and there is nothing to undo.
        if (MG_Pipe::gPipeInputs->IsCapabilityEnabled(MobileGL::CapabilityInput::FramebufferSrgb)) return;
        if (ResolveSrgbAttachmentWriteFormat(destinationFormat, false) == destinationFormat) return;

        // sRGB -> linear (GL 4.6 core 8.24), applied to the colour channels only: alpha is stored
        // linearly in an sRGB format and must pass through untouched.
        const auto toLinear = [](Float encoded) {
            const Float value = std::clamp(encoded, 0.0f, 1.0f);
            return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
        };
        payload.color = FloatVec4(toLinear(payload.color.x()), toLinear(payload.color.y()),
                                  toLinear(payload.color.z()), payload.color.w());
    }

    // ResolveAttachmentLayerCount used to be duplicated here, reading attachment.GetSize().z()
    // raw - no ToVulkanLevelExtent remap for a 1D array, no six-faces arm for a cube map. That is
    // not a cosmetic difference: the count below is not key-only, it is written straight into
    // VkImageSubresourceRange::layerCount by MaterializePendingClearForTexture, which then POPS
    // the entry - so a layered cube map's glClear reached one face and the other five were lost
    // for good, while the very same queued clear cleared all six through the render pass's
    // LOAD_OP_CLEAR. The helper now lives once, in VkTextureManager.h beside ToVulkanLevelExtent.

} // namespace MobileGL::MG_Backend::DirectVulkan
