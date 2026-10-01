// MobileGL - MobileGL/MG_State/GLState/TextureState/TextureObject2D.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TextureObject2D.h"

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            TextureObject2D::TextureObject2D(Uint externalIndex)
                : TextureObjectWithOneMipmap(TextureTarget::Texture2D, externalIndex) {}

            TextureObject2D::TextureObject2D(Uint externalIndex, TextureTarget target)
                : TextureObjectWithOneMipmap(target, externalIndex) {}

            Uint TextureObject2D::GetIndexOfTextureUploadTarget(TextureUploadTarget target) const {
                MOBILEGL_ASSERT(target == TextureUploadTarget::Texture2D ||
                                    target == TextureUploadTarget::ProxyTexture2D,
                                "Invalid TextureUploadTarget!");
                return 0;
            }
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL