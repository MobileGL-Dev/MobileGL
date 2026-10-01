// MobileGL - MobileGL/MG_State/GLState/TextureState/TextureObject2D.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "TextureObject.h"

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            class TextureObject2D : public TextureObjectWithOneMipmap {
            public:
                explicit TextureObject2D(Uint externalIndex);
                // An external texture (GL_TEXTURE_EXTERNAL_OES) is a 2D texture whose storage is an
                // imported EGLImage; it keeps that target so the render server can tell it apart.
                TextureObject2D(Uint externalIndex, TextureTarget target);
                const Vector<TextureUploadTarget>& GetUploadTargets() const override { return m_uploadTargets; }

            protected:
                Uint GetIndexOfTextureUploadTarget(TextureUploadTarget target) const override;
                const Vector<TextureUploadTarget> m_uploadTargets{TextureUploadTarget::Texture2D};
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
