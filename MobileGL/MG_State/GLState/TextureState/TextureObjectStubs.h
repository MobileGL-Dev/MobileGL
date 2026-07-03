// MobileGL - MobileGL/MG_State/GLState/TextureState/TextureObjectStubs.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "MG_State/GLState/TextureState/TextureEnum.h"
#include "TextureObject.h"

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            /* These texture types are not yet implemented:
             * TextureRectangle,
             * TextureBuffer,
             * Texture1DArray,
             * Texture2DArray,
             * TextureCubeMapArray,
             * Texture2DMultisampleArray layered attachment behavior
             */
#define STUB_TEXTURE_OBJECT_CLASS_DEFINITION(className, texTarget, uploadTargets)                                      \
    class className : public TextureObjectWithOneMipmap {                                                              \
    public:                                                                                                            \
        explicit className(Uint externalIndex) : TextureObjectWithOneMipmap(texTarget, externalIndex) {}               \
        const Vector<TextureUploadTarget>& GetUploadTargets() const override { return m_uploadTargets; }               \
                                                                                                                       \
    protected:                                                                                                         \
        Uint GetIndexOfTextureUploadTarget(TextureUploadTarget target) const override { return 0; }                    \
        const Vector<TextureUploadTarget> m_uploadTargets = uploadTargets;                                             \
    };

            STUB_TEXTURE_OBJECT_CLASS_DEFINITION(TextureObjectRectangle, TextureTarget::TextureRectangle,
                                                 {TextureUploadTarget::TextureRectangle});

            STUB_TEXTURE_OBJECT_CLASS_DEFINITION(TextureObject2DMultisample, TextureTarget::Texture2DMultisample,
                                                 {TextureUploadTarget::Texture2DMultisample});

            // STUB_TEXTURE_OBJECT_CLASS_DEFINITION(TextureObjectBuffer, TextureTarget::TextureBuffer,
            //                                      {TextureUploadTarget::Unknown});

            STUB_TEXTURE_OBJECT_CLASS_DEFINITION(TextureObject1DArray, TextureTarget::Texture1DArray,
                                                 {TextureUploadTarget::Texture1DArray});

            STUB_TEXTURE_OBJECT_CLASS_DEFINITION(TextureObject2DArray, TextureTarget::Texture2DArray,
                                                 {TextureUploadTarget::Texture2DArray});

            STUB_TEXTURE_OBJECT_CLASS_DEFINITION(TextureObjectCubeMapArray, TextureTarget::TextureCubeMapArray,
                                                 {TextureUploadTarget::CubeMapArray});

            STUB_TEXTURE_OBJECT_CLASS_DEFINITION(TextureObject2DMultisampleArray,
                                                 TextureTarget::Texture2DMultisampleArray,
                                                 {TextureUploadTarget::Texture2DMultisampleArray});
#undef STUB_TEXTURE_OBJECT_CLASS_DEFINITION

        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
