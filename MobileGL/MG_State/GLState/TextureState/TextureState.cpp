// MobileGL - MobileGL/MG_State/GLState/TextureState/TextureState.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TextureState.h"

#include <atomic>
#include "Defines.h"
#include "TextureEnum.h"
#include "TextureObject.h"
#include "TextureObject1D.h"
#include "TextureObject2D.h"
#include "TextureObject3D.h"
#include "TextureObject2DCube.h"
#include "TextureObjectBuffer.h"
#include "TextureObjectStubs.h"
#include "TextureObjectView.h"
#include "MG_State/GLState/ShareGroupState.h"

namespace MobileGL::MG_State::GLState {
    static std::atomic<Uint64> s_nextTextureStateContextId = 1;

    Uint64 TextureState::AllocateContextId() {
        return s_nextTextureStateContextId.fetch_add(1, std::memory_order_relaxed);
    }

    TextureState::TextureState() : m_contextId(AllocateContextId()) {
        // GL 3.3 core 3.8: each target owns one default texture object (name 0) per context,
        // shared across all texture units, and it is the initial binding of every unit/target
        // slot. It is created outside the share group's table so name-based paths (glIsTexture,
        // GenTextures/DeleteTextures, by-name DSA lookups) never see it.
        for (int i = 0; i < (int)TextureTarget::TextureTargetCount; ++i) {
            m_defaultTextureObjects[i] = MakeTextureObjectForTarget(0, static_cast<TextureTarget>(i));
        }
        for (int i = 0; i < MAX_TEXTURE_IMAGE_UNITS; ++i) {
            m_textureUnits[i] = TextureUnit();
            for (auto& bindingSlot : m_textureUnits[i].GetAllBindingSlots()) {
                bindingSlot.Bind(m_defaultTextureObjects[(int)bindingSlot.GetTarget()]);
            }
        }
    }

    const SharedPtr<ITextureObject>& TextureState::GetDefaultTextureObject(TextureTarget target) const {
        MOBILEGL_ASSERT(target > TextureTarget::Unknown && target < TextureTarget::TextureTargetCount,
                        "GetDefaultTextureObject: invalid texture target %d", (int)target);
        return m_defaultTextureObjects[(int)target];
    }

    void TextureState::UnbindTextureFromUnits(const SharedPtr<ITextureObject>& textureObject) {
        if (!textureObject) return;
        // Units past the touched high-water mark can never reference a texture.
        for (Int unit = 0; unit <= m_maxTouchedUnit; ++unit) {
            auto& bindingSlots = m_textureUnits[unit].GetAllBindingSlots();
            for (auto& bindingSlot : bindingSlots) {
                if (bindingSlot.GetBoundObject() == textureObject) {
                    // GL 3.3 core 3.8.1: deleting a bound texture rebinds zero, i.e. the
                    // target's default texture object, on every unit it was bound to.
                    bindingSlot.Bind(m_defaultTextureObjects[(int)bindingSlot.GetTarget()]);
                }
            }
        }
        for (Int unit = 0; unit <= m_maxTouchedUnit; ++unit) {
            auto& imageBinding = m_imageTextureBindings[unit];
            if (imageBinding.Texture == textureObject) {
                imageBinding.Bind(nullptr, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
            }
        }
        // Deleting a texture unbinds it from every unit above; treat that as a binding
        // change so a cached sampled-texture set (which may hold this raw pointer) is
        // re-resolved instead of dangling.
        BumpTextureBindGeneration();
    }

    TextureUnit& TextureState::GetUnitObject(Int unit) {
        MOBILEGL_ASSERT(unit >= 0 && unit < MAX_TEXTURE_IMAGE_UNITS, "Texture unit is out of range: %d > %d", unit,
                        MAX_TEXTURE_IMAGE_UNITS - 1);
        return m_textureUnits[unit];
    }

    ImageTextureBinding& TextureState::GetImageTextureBinding(Int unit) {
        MOBILEGL_ASSERT(unit >= 0 && unit < MAX_TEXTURE_IMAGE_UNITS, "Image unit is out of range: %d > %d", unit,
                        MAX_TEXTURE_IMAGE_UNITS - 1);
        return m_imageTextureBindings[unit];
    }

    const ImageTextureBinding& TextureState::GetImageTextureBinding(Int unit) const {
        MOBILEGL_ASSERT(unit >= 0 && unit < MAX_TEXTURE_IMAGE_UNITS, "Image unit is out of range: %d > %d", unit,
                        MAX_TEXTURE_IMAGE_UNITS - 1);
        return m_imageTextureBindings[unit];
    }

    Int TextureState::GetActiveTextureUnit() const {
        return m_activeTextureUnit;
    }

    void TextureState::SetActiveTextureUnit(Int unit) {
        m_activeTextureUnit = unit;
    }
} // namespace MobileGL::MG_State::GLState
