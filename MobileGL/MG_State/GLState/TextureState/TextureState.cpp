// MobileGL - MobileGL/MG_State/GLState/TextureState/TextureState.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TextureState.h"
#include "Defines.h"
#include "TextureEnum.h"
#include "TextureObject.h"
#include "TextureObject1D.h"
#include "TextureObject2D.h"
#include "TextureObject3D.h"
#include "TextureObject2DCube.h"
#include "TextureObjectBuffer.h"
#include "TextureObjectStubs.h"

namespace MobileGL::MG_State::GLState {
    static SharedPtr<ITextureObject> MakeTextureObjectForTarget(Uint index, TextureTarget target) {
        switch (target) {
        case TextureTarget::Texture1D:
            return MakeShared<TextureObject1D>(index);
        case TextureTarget::TextureCubeMap:
            return MakeShared<TextureObject2DCube>(index);
        case TextureTarget::Texture2D:
            return MakeShared<TextureObject2D>(index);
        case TextureTarget::Texture3D:
            return MakeShared<TextureObject3D>(index);
        case TextureTarget::TextureBuffer:
            return MakeShared<TextureObjectBuffer>(index);

            // These texture types are still stubbed:
        case TextureTarget::TextureRectangle:
            return MakeShared<TextureObjectRectangle>(index);
        case TextureTarget::Texture2DMultisample:
            return MakeShared<TextureObject2DMultisample>(index);
        case TextureTarget::Texture1DArray:
            return MakeShared<TextureObject1DArray>(index);
        case TextureTarget::Texture2DArray:
            return MakeShared<TextureObject2DArray>(index);
        case TextureTarget::TextureCubeMapArray:
            return MakeShared<TextureObjectCubeMapArray>(index);
        case TextureTarget::Texture2DMultisampleArray:
            return MakeShared<TextureObject2DMultisampleArray>(index);
        default:
            MOBILEGL_ASSERT(false, "Unimplemented texture type when creating texture object!: %d", (int)target);
            return nullptr;
        }
    }

    TextureState::TextureState() : m_indexGenerator(1024, 1) {
        // GL 3.3 core 3.8: each target owns one default texture object (name 0) per context,
        // shared across all texture units, and it is the initial binding of every unit/target
        // slot. It is created outside m_textureObjects so name-based paths (glIsTexture,
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

    const SharedPtr<ITextureObject>& TextureState::GetTextureObject(Uint index) {
        auto it = m_textureObjects.find(index);
        if (it != m_textureObjects.end()) {
            return it->second;
        }
        static SharedPtr<ITextureObject> nullTextureObject = nullptr;
        return nullTextureObject;
    }

    const SharedPtr<ITextureObject>& TextureState::GetDefaultTextureObject(TextureTarget target) const {
        MOBILEGL_ASSERT(target > TextureTarget::Unknown && target < TextureTarget::TextureTargetCount,
                        "GetDefaultTextureObject: invalid texture target %d", (int)target);
        return m_defaultTextureObjects[(int)target];
    }

    void TextureState::GenerateNames(Uint number, Vector<Uint>& textures) {
        textures.resize(number);
        m_indexGenerator.Generate(number, textures.data());
    }

    const SharedPtr<ITextureObject>& TextureState::CreateTextureObject(Uint index, TextureTarget target) {
        auto& textureObject = m_textureObjects[index];
        textureObject = MakeTextureObjectForTarget(index, target);
        if (!textureObject) {
            static SharedPtr<ITextureObject> nullTextureObject = nullptr;
            return nullTextureObject;
        }
        return textureObject;
    }

    void TextureState::MarkTextureObjectForDeletion(Uint index, Bool keepUnboundReservation) {
        if (m_indexGenerator.IsValid(index)) {
            auto it = m_textureObjects.find(index);
            if (it != m_textureObjects.end()) {
                // Units past the touched high-water mark can never reference a texture.
                for (Int unit = 0; unit <= m_maxTouchedUnit; ++unit) {
                    auto& bindingSlots = m_textureUnits[unit].GetAllBindingSlots();
                    for (auto& bindingSlot : bindingSlots) {
                        if (bindingSlot.GetBoundObject() == it->second) {
                            // GL 3.3 core 3.8.1: deleting a bound texture rebinds zero, i.e. the
                            // target's default texture object, on every unit it was bound to.
                            bindingSlot.Bind(m_defaultTextureObjects[(int)bindingSlot.GetTarget()]);
                        }
                    }
                }
                for (Int unit = 0; unit <= m_maxTouchedUnit; ++unit) {
                    auto& imageBinding = m_imageTextureBindings[unit];
                    if (imageBinding.Texture == it->second) {
                        imageBinding.Bind(nullptr, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
                    }
                }
                // Deleting a texture unbinds it from every unit above; treat that as a binding
                // change so a cached sampled-texture set (which may hold this raw pointer) is
                // re-resolved instead of dangling.
                BumpTextureBindGeneration();
                m_textureObjects.erase(index);
                m_indexGenerator.Delete(index);
            } else if (!keepUnboundReservation) {
                // GL 3.3 core 3.8.1 makes a deleted name unused again even when GenTextures only
                // reserved it and no bind ever instantiated an object (so a later bind of it must
                // fail), and the reservation has to return to the free list.
                m_indexGenerator.Delete(index);
            }
            // Relaxed semantics: legacy apps may delete a generated name before its first
            // bind, then bind and populate that same name. Keep such a reservation alive there;
            // a real texture object reaching deletion still releases its index above.
        }
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

    Bool TextureState::ValidateName(Uint index) const {
        return m_indexGenerator.IsValid(index);
    }

    Bool TextureState::ValidateTextureObject(Uint index) const {
        return m_textureObjects.find(index) != m_textureObjects.end();
    }
} // namespace MobileGL::MG_State::GLState
