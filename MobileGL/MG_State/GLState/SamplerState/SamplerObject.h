// MobileGL - MobileGL/MG_State/GLState/SamplerState/SamplerObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Pipe/MGPipeValueTypes.h>

namespace MobileGL {
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
                BorderColorForm GetBorderColorForm() const;
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
                // The ONLY way m_version may move. Besides marking this object's parameters
                // dirty for the backends it bumps the context-wide sampling-resolution
                // generation: MIN_FILTER decides whether a lookup reads the mip chain, which
                // decides whether a bound texture is mipmap-complete, which decides whether a
                // backend binds it on its unit at all.
                void BumpVersion();

                const Uint m_externalIndex;
                const Uint64 m_lifetimeId;
                Uint16 m_version = 0;
                SamplerParameters m_samplerParameters;
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
