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
    TextureState::TextureState() : m_indexGenerator(1024, 1) {
        for (int i = 0; i < MAX_TEXTURE_IMAGE_UNITS; ++i) {
            m_textureUnits[i] = TextureUnit();
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

    void TextureState::GenerateNames(Uint number, Vector<Uint>& textures) {
        textures.resize(number);
        m_indexGenerator.Generate(number, textures.data());
    }

    const SharedPtr<ITextureObject>& TextureState::CreateTextureObject(Uint index, TextureTarget target) {
        auto& textureObject = m_textureObjects[index];
        switch (target) {
        case TextureTarget::Texture1D:
            textureObject = MakeShared<TextureObject1D>(index);
            break;
        case TextureTarget::TextureCubeMap:
            textureObject = MakeShared<TextureObject2DCube>(index);
            break;
        case TextureTarget::Texture2D:
            textureObject = MakeShared<TextureObject2D>(index);
            break;
        case TextureTarget::Texture3D:
            textureObject = MakeShared<TextureObject3D>(index);
            break;
        case TextureTarget::TextureBuffer:
            textureObject = MakeShared<TextureObjectBuffer>(index);
            break;

            // These texture types are still stubbed:
        case TextureTarget::TextureRectangle:
            textureObject = MakeShared<TextureObjectRectangle>(index);
            break;
        case TextureTarget::Texture2DMultisample:
            textureObject = MakeShared<TextureObject2DMultisample>(index);
            break;
        case TextureTarget::Texture1DArray:
            textureObject = MakeShared<TextureObject1DArray>(index);
            break;
        case TextureTarget::Texture2DArray:
            textureObject = MakeShared<TextureObject2DArray>(index);
            break;
        case TextureTarget::TextureCubeMapArray:
            textureObject = MakeShared<TextureObjectCubeMapArray>(index);
            break;
        case TextureTarget::Texture2DMultisampleArray:
            textureObject = MakeShared<TextureObject2DMultisampleArray>(index);
            break;
        default:
            MOBILEGL_ASSERT(false, "Unimplemented texture type when creating texture object!: %d", (int)target);
            static SharedPtr<ITextureObject> nullTextureObject = nullptr;
            return nullTextureObject;
        }

        return textureObject;
    }

    void TextureState::MarkTextureObjectForDeletion(Uint index) {
        if (m_indexGenerator.IsValid(index)) {
            auto it = m_textureObjects.find(index);
            if (it != m_textureObjects.end()) {
                for (auto& unit : m_textureUnits) {
                    auto& bindingSlots = unit.GetAllBindingSlots();
                    for (auto& bindingSlot : bindingSlots) {
                        if (bindingSlot.GetBoundObject() == it->second) {
                            bindingSlot.Bind(nullptr);
                        }
                    }
                }
                for (auto& imageBinding : m_imageTextureBindings) {
                    if (imageBinding.Texture == it->second) {
                        imageBinding.Bind(nullptr, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
                    }
                }
                m_textureObjects.erase(it);
            }
            m_indexGenerator.Delete(index);
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
