// MobileGL - MobileGL/MG_State/GLState/SamplerState/SamplerObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "SamplerObject.h"

#include <atomic>

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            static std::atomic<Uint64> s_nextSamplerLifetimeId = 1;

            Uint64 SamplerObject::AllocateLifetimeId() {
                return s_nextSamplerLifetimeId.fetch_add(1, std::memory_order_relaxed);
            }

            SamplerObject::SamplerObject(Uint externalIndex)
                : m_externalIndex(externalIndex), m_lifetimeId(AllocateLifetimeId()) {}

            void SamplerObject::SetWrapS(SamplerWrapMode mode) {
                if (mode == m_samplerParameters.wrapS) return;

                m_samplerParameters.wrapS = mode;
                ++m_version;
            }

            void SamplerObject::SetWrapT(SamplerWrapMode mode) {
                if (mode == m_samplerParameters.wrapT) return;

                m_samplerParameters.wrapT = mode;
                ++m_version;
            }

            void SamplerObject::SetWrapR(SamplerWrapMode mode) {
                if (mode == m_samplerParameters.wrapR) return;

                m_samplerParameters.wrapR = mode;
                ++m_version;
            }

            void SamplerObject::SetMinFilter(SamplerFilterMode mode) {
                if (mode == m_samplerParameters.minFilter) return;

                m_samplerParameters.minFilter = mode;
                ++m_version;
            }

            void SamplerObject::SetMagFilter(SamplerFilterMode mode) {
                if (mode == m_samplerParameters.magFilter) return;

                m_samplerParameters.magFilter = mode;
                ++m_version;
            }

            void SamplerObject::SetMipmapMode(SamplerMipmapMode mode) {
                if (mode == m_samplerParameters.mipmapMode) return;

                m_samplerParameters.mipmapMode = mode;
                ++m_version;
            }

            void SamplerObject::SetLodRange(Float minLod, Float maxLod) {
                if (minLod == m_samplerParameters.minLod && maxLod == m_samplerParameters.maxLod) return;
                m_samplerParameters.minLod = minLod;
                m_samplerParameters.maxLod = maxLod;
                ++m_version;
            }

            void SamplerObject::SetLodBias(Float bias) {
                if (bias == m_samplerParameters.lodBias) return;

                m_samplerParameters.lodBias = bias;
                ++m_version;
            }

            void SamplerObject::SetMaxAnisotropy(Float maxAnisotropy) {
                if (maxAnisotropy == m_samplerParameters.maxAnisotropy) return;

                m_samplerParameters.maxAnisotropy = maxAnisotropy;
                ++m_version;
            }

            void SamplerObject::SetSamplerCompareFunc(SamplerCompareFunc func) {
                if (func == m_samplerParameters.compareFunc) return;

                m_samplerParameters.compareFunc = func;
                ++m_version;
            }

            void SamplerObject::SetCompareMode(SamplerCompareMode mode) {
                if (mode == m_samplerParameters.compareMode) return;

                m_samplerParameters.compareMode = mode;
                ++m_version;
            }

            SamplerWrapMode SamplerObject::GetWrapS() const {
                return m_samplerParameters.wrapS;
            }

            SamplerWrapMode SamplerObject::GetWrapT() const {
                return m_samplerParameters.wrapT;
            }

            SamplerWrapMode SamplerObject::GetWrapR() const {
                return m_samplerParameters.wrapR;
            }

            SamplerFilterMode SamplerObject::GetMinFilter() const {
                return m_samplerParameters.minFilter;
            }

            SamplerFilterMode SamplerObject::GetMagFilter() const {
                return m_samplerParameters.magFilter;
            }

            SamplerMipmapMode SamplerObject::GetMipmapMode() const {
                return m_samplerParameters.mipmapMode;
            }

            Float SamplerObject::GetMinLod() const {
                return m_samplerParameters.minLod;
            }

            Float SamplerObject::GetMaxLod() const {
                return m_samplerParameters.maxLod;
            }

            Float SamplerObject::GetLodBias() const {
                return m_samplerParameters.lodBias;
            }

            Float SamplerObject::GetMaxAnisotropy() const {
                return m_samplerParameters.maxAnisotropy;
            }

            SamplerCompareMode SamplerObject::GetCompareMode() const {
                return m_samplerParameters.compareMode;
            }

            SamplerCompareFunc SamplerObject::GetSamplerCompareFunc() const {
                return m_samplerParameters.compareFunc;
            }

            Uint SamplerObject::GetExternalIndex() const {
                return m_externalIndex;
            }

            const SamplerParameters& SamplerObject::GetAllSamplerParameters() const {
                return m_samplerParameters;
            }

            Uint16 SamplerObject::GetVersion() const {
                return m_version;
            }

            Uint64 SamplerObject::GetLifetimeId() const {
                return m_lifetimeId;
            }
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
