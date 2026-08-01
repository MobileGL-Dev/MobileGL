// MobileGL - MobileGL/MG_Util/Classifiers/TextureEnumClassifier.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TextureEnumClassifier.h"

namespace MobileGL {
    namespace MG_Util {
        bool IsDepthFormatInternalFormat(TextureInternalFormat internalformat) {
            switch (internalformat) {
            case TextureInternalFormat::DepthComponent16:
            case TextureInternalFormat::DepthComponent24:
            case TextureInternalFormat::DepthComponent32:
            case TextureInternalFormat::DepthComponent32F:
            case TextureInternalFormat::Depth24Stencil8:
            case TextureInternalFormat::Depth32FStencil8:
            case TextureInternalFormat::DepthComponent:
            case TextureInternalFormat::DepthStencil:
                return true;
            default:
                return false;
            }
        }

        bool IsStencilFormatInternalFormat(TextureInternalFormat internalformat) {
            switch (internalformat) {
            case TextureInternalFormat::StencilIndex8:
            case TextureInternalFormat::Depth24Stencil8:
            case TextureInternalFormat::Depth32FStencil8:
            case TextureInternalFormat::DepthStencil:
                return true;
            default:
                return false;
            }
        }
    } // namespace MG_Util
} // namespace MobileGL