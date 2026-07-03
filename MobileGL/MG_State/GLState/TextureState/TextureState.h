// MobileGL - MobileGL/MG_State/GLState/TextureState/TextureState.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/Miscellany/IndexGenerator.h>
#include "MG_State/GLState/TextureState/TextureObject.h"
#include "MG_Util/Types.h"
#include "TextureUnit.h"

namespace MobileGL::MG_State::GLState {
    struct ImageTextureBinding {
        SharedPtr<ITextureObject> Texture;
        GLint Level = 0;
        GLboolean Layered = GL_FALSE;
        GLint Layer = 0;
        GLenum Access = GL_READ_ONLY;
        GLenum Format = GL_R8;
        Uint16 Version = 0;

        void Bind(SharedPtr<ITextureObject> texture, GLint level, GLboolean layered, GLint layer, GLenum access,
                  GLenum format) {
            Texture = Move(texture);
            Level = level;
            Layered = layered;
            Layer = layer;
            Access = access;
            Format = format;
            ++Version;
        }
    };

    class TextureState {
    public:
        static constexpr int MAX_TEXTURE_IMAGE_UNITS = 192;

        TextureState();
        void GenerateNames(Uint number, Vector<Uint>& textures);
        const SharedPtr<ITextureObject>& CreateTextureObject(Uint index, TextureTarget target);
        const SharedPtr<ITextureObject>& GetTextureObject(Uint index);
        TextureUnit& GetUnitObject(Int unit);
        ImageTextureBinding& GetImageTextureBinding(Int unit);
        const ImageTextureBinding& GetImageTextureBinding(Int unit) const;
        Int GetActiveTextureUnit() const;
        void SetActiveTextureUnit(Int unit);
        void MarkTextureObjectForDeletion(Uint index);
        Bool ValidateName(Uint index) const;
        Bool ValidateTextureObject(Uint index) const;

    private:
        Int m_activeTextureUnit = 0;
        Array<TextureUnit, MAX_TEXTURE_IMAGE_UNITS> m_textureUnits;
        Array<ImageTextureBinding, MAX_TEXTURE_IMAGE_UNITS> m_imageTextureBindings;
        IndexGenerator<Uint> m_indexGenerator;
        UnorderedMap<GLuint, SharedPtr<ITextureObject>> m_textureObjects;
    };
} // namespace MobileGL::MG_State::GLState
