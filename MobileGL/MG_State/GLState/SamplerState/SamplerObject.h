// MobileGL - MobileGL/MG_State/GLState/SamplerState/SamplerObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/Math/VectorTypes.h>

namespace MobileGL {
    enum class SamplerFilterMode {
        Nearest,
        Linear,
        SamplerFilterCount,
        Unknown = -1
    };

    enum class SamplerMipmapMode {
        None,
        Nearest,
        Linear,
        SamplerMipmapModeCount,
        Unknown = -1
    };

    enum class SamplerWrapMode {
        ClampToEdge,
        MirroredRepeat,
        Repeat,
        ClampToBorder,
        MirrorClampToEdge,
        SamplerWrapModeCount,
        Unknown = -1
    };

    enum class SamplerCompareMode {
        None,
        CompareToTexture,
        SamplerCompareModeCount,
        Unknown = -1
    };

    enum class SamplerCompareFunc {
        Never,
        Less,
        Equal,
        LessEqual,
        Greater,
        NotEqual,
        GreaterEqual,
        Always,
        SamplerCompareFuncCount,
        Unknown = -1
    };

    struct SamplerParameters {
        SamplerWrapMode wrapS = SamplerWrapMode::Repeat;
        SamplerWrapMode wrapT = SamplerWrapMode::Repeat;
        SamplerWrapMode wrapR = SamplerWrapMode::Repeat;
        SamplerFilterMode minFilter = SamplerFilterMode::Nearest;
        SamplerFilterMode magFilter = SamplerFilterMode::Linear;
        SamplerMipmapMode mipmapMode = SamplerMipmapMode::Linear;
        Float minLod = -1000.0f;
        Float maxLod = 1000.0f;
        Float lodBias = 0.0f;
        Float maxAnisotropy = 1.0f;
        // GL 4.6 core table 23.18 / GLES 3.2 table 21.16: TEXTURE_COMPARE_FUNC starts at LEQUAL,
        // for both sampler objects and the sampler state a texture object carries.
        SamplerCompareFunc compareFunc = SamplerCompareFunc::LessEqual;
        SamplerCompareMode compareMode = SamplerCompareMode::None;
        // TEXTURE_BORDER_COLOR is sampler state (GL 4.6 core table 23.18), so it belongs here and
        // not on the texture - a texture object reaches it through the sampler object it owns. The
        // three representations are the float, integer and unsigned-integer forms glSamplerParameterfv,
        // glSamplerParameterIiv and glSamplerParameterIuiv set; whichever is written last defines
        // the colour and the other two follow it, so a getter always has an answer.
        FloatVec4 borderColor = {0.0f, 0.0f, 0.0f, 0.0f};
        IntVec4 borderColorI = {0, 0, 0, 0};
        UintVec4 borderColorUI = {0, 0, 0, 0};
    };

    namespace MG_State {
        namespace GLState {
            class SamplerObject {
            public:
                SamplerObject(Uint externalIndex);

                void SetWrapS(SamplerWrapMode mode);
                void SetWrapT(SamplerWrapMode mode);
                void SetWrapR(SamplerWrapMode mode);
                void SetMinFilter(SamplerFilterMode mode);
                void SetMagFilter(SamplerFilterMode mode);
                void SetMipmapMode(SamplerMipmapMode mode);
                void SetLodRange(Float minLod, Float maxLod);
                void SetLodBias(Float bias);
                void SetMaxAnisotropy(Float maxAnisotropy);
                void SetSamplerCompareFunc(SamplerCompareFunc func);
                void SetCompareMode(SamplerCompareMode mode);
                void SetBorderColor(const FloatVec4& color);
                void SetBorderColorI(const IntVec4& color);
                void SetBorderColorUI(const UintVec4& color);

                SamplerWrapMode GetWrapS() const;
                SamplerWrapMode GetWrapT() const;
                SamplerWrapMode GetWrapR() const;
                SamplerFilterMode GetMinFilter() const;
                SamplerFilterMode GetMagFilter() const;
                SamplerMipmapMode GetMipmapMode() const;
                Float GetMinLod() const;
                Float GetMaxLod() const;
                Float GetLodBias() const;
                Float GetMaxAnisotropy() const;
                SamplerCompareMode GetCompareMode() const;
                SamplerCompareFunc GetSamplerCompareFunc() const;
                const FloatVec4& GetBorderColor() const;
                const IntVec4& GetBorderColorI() const;
                const UintVec4& GetBorderColorUI() const;
                Uint GetExternalIndex() const;
                Uint16 GetVersion() const;
                // Globally-unique, never-reused id for this sampler object's lifetime. Lets a
                // cache distinguish a freed-and-reallocated sampler (same heap address, GL name,
                // or version count) from the original - the sampler analogue of the texture
                // lifetime id. Used by the Vulkan backend's per-binding sampler fast path.
                Uint64 GetLifetimeId() const;
                const SamplerParameters& GetAllSamplerParameters() const;

            private:
                static Uint64 AllocateLifetimeId();

                const Uint m_externalIndex;
                const Uint64 m_lifetimeId;
                Uint16 m_version = 0;
                SamplerParameters m_samplerParameters;
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
