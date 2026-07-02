// MobileGL - MobileGL/MG_State/GLState/RenderbufferState/RenderbufferObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/Math/VectorTypes.h>
#include <MG_State/GLState/TextureState/TextureEnum.h>

namespace MobileGL {
    enum class RenderbufferTarget {
        Renderbuffer,
        RenderbufferTargetCount,
        Unknown = -1
    };

    namespace MG_State {
        namespace GLState {
            class RenderbufferObject {
            public:
                using TargetEnum = RenderbufferTarget;

                RenderbufferObject(Uint externalIndex);

                Uint GetExternalIndex() const;
                void SetInternalFormat(TextureInternalFormat format);
                void AllocateStorage(IntVec2 size);
                void SetSamples(Int samples);
                Int GetWidth() const;
                Int GetHeight() const;
                TextureInternalFormat GetInternalFormat() const;
                Bool IsAllocated() const;
                const ComponentSizes& GetComponentSizes() const;
                Int GetRedSize() const;
                Int GetGreenSize() const;
                Int GetBlueSize() const;
                Int GetAlphaSize() const;
                Int GetDepthSize() const;
                Int GetStencilSize() const;
                Int GetSamples() const;

            private:
                Uint m_externalIndex = 0;
                TextureInternalFormat m_internalFormat = TextureInternalFormat::RGBA;
                Int m_width = 0;
                Int m_height = 0;
                Int m_samples = 0;
                Bool m_allocated = false;
                ComponentSizes m_componentSizes;
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
