// MobileGL - MobileGL/MG_State/GLState/SamplerState/SamplerObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "SamplerObject.h"

#include <MG_State/GLState/Core.h>

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

            void SamplerObject::BumpVersion() {
                ++m_version;
                // Every setter early-outs on an unchanged value, so this only runs on a real
                // parameter change. The generation is bumped for ALL parameters, not just filter
                // ones that feed mipmap-completeness: a backend memo of the resolved per-unit
                // bindings must never miss an invalidation, and over-invalidating on a wrap-mode
                // write costs one re-resolve.
                if (pGLContext) pGLContext->BumpSamplingResolutionGeneration();
            }

            void SamplerObject::SetWrapS(SamplerWrapMode mode) {
                if (mode == m_samplerParameters.wrapS) return;

                m_samplerParameters.wrapS = mode;
                BumpVersion();
            }

            void SamplerObject::SetWrapT(SamplerWrapMode mode) {
                if (mode == m_samplerParameters.wrapT) return;

                m_samplerParameters.wrapT = mode;
                BumpVersion();
            }

            void SamplerObject::SetWrapR(SamplerWrapMode mode) {
                if (mode == m_samplerParameters.wrapR) return;

                m_samplerParameters.wrapR = mode;
                BumpVersion();
            }

            void SamplerObject::SetMinFilter(SamplerFilterMode mode) {
                if (mode == m_samplerParameters.minFilter) return;

                m_samplerParameters.minFilter = mode;
                BumpVersion();
            }

            void SamplerObject::SetMagFilter(SamplerFilterMode mode) {
                if (mode == m_samplerParameters.magFilter) return;

                m_samplerParameters.magFilter = mode;
                BumpVersion();
            }

            void SamplerObject::SetMipmapMode(SamplerMipmapMode mode) {
                if (mode == m_samplerParameters.mipmapMode) return;

                m_samplerParameters.mipmapMode = mode;
                BumpVersion();
            }

            void SamplerObject::SetLodRange(Float minLod, Float maxLod) {
                if (minLod == m_samplerParameters.minLod && maxLod == m_samplerParameters.maxLod) return;
                m_samplerParameters.minLod = minLod;
                m_samplerParameters.maxLod = maxLod;
                BumpVersion();
            }

            void SamplerObject::SetLodBias(Float bias) {
                if (bias == m_samplerParameters.lodBias) return;

                m_samplerParameters.lodBias = bias;
                BumpVersion();
            }

            void SamplerObject::SetMaxAnisotropy(Float maxAnisotropy) {
                if (maxAnisotropy == m_samplerParameters.maxAnisotropy) return;

                m_samplerParameters.maxAnisotropy = maxAnisotropy;
                BumpVersion();
            }

            void SamplerObject::SetSamplerCompareFunc(SamplerCompareFunc func) {
                if (func == m_samplerParameters.compareFunc) return;

                m_samplerParameters.compareFunc = func;
                BumpVersion();
            }

            void SamplerObject::SetCompareMode(SamplerCompareMode mode) {
                if (mode == m_samplerParameters.compareMode) return;

                m_samplerParameters.compareMode = mode;
                BumpVersion();
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

            // The three border-colour representations are kept in step so a getter of any form has
            // an answer whichever form was written. Integer <-> float uses the plain value, matching
            // what glTexParameterIiv/Iuiv mean: those forms are for integer texture formats, whose
            // border components are the raw integers rather than a normalized fraction.
            //
            // Which of the three the application actually WROTE is recorded separately in
            // borderColorForm, because the derived values erase it: a backend has to know whether to
            // forward the colour through glSamplerParameterfv or glSamplerParameterIiv (and which
            // VkBorderColor family to ask Vulkan for), and the numbers alone cannot say. That is also
            // why every setter's early-out tests the form as well as the value - a float (0,0,0,1)
            // followed by an integer (0,0,0,1) is a real state change even though nothing numeric
            // moved, and swallowing it would leave the backend syncing the wrong entry point forever.
            void SamplerObject::SetBorderColor(const FloatVec4& color) {
                if (color == m_samplerParameters.borderColor &&
                    m_samplerParameters.borderColorForm == BorderColorForm::Float) {
                    return;
                }

                m_samplerParameters.borderColorForm = BorderColorForm::Float;
                m_samplerParameters.borderColor = color;
                m_samplerParameters.borderColorI =
                    IntVec4(static_cast<Int32>(color.x()), static_cast<Int32>(color.y()),
                            static_cast<Int32>(color.z()), static_cast<Int32>(color.w()));
                m_samplerParameters.borderColorUI =
                    UintVec4(static_cast<Uint32>(color.x()), static_cast<Uint32>(color.y()),
                             static_cast<Uint32>(color.z()), static_cast<Uint32>(color.w()));
                BumpVersion();
            }

            void SamplerObject::SetBorderColorI(const IntVec4& color) {
                if (color == m_samplerParameters.borderColorI &&
                    m_samplerParameters.borderColorForm == BorderColorForm::Int) {
                    return;
                }

                m_samplerParameters.borderColorForm = BorderColorForm::Int;
                m_samplerParameters.borderColorI = color;
                m_samplerParameters.borderColorUI =
                    UintVec4(static_cast<Uint32>(color.x()), static_cast<Uint32>(color.y()),
                             static_cast<Uint32>(color.z()), static_cast<Uint32>(color.w()));
                m_samplerParameters.borderColor =
                    FloatVec4(static_cast<Float>(color.x()), static_cast<Float>(color.y()),
                              static_cast<Float>(color.z()), static_cast<Float>(color.w()));
                BumpVersion();
            }

            void SamplerObject::SetBorderColorUI(const UintVec4& color) {
                if (color == m_samplerParameters.borderColorUI &&
                    m_samplerParameters.borderColorForm == BorderColorForm::Uint) {
                    return;
                }

                m_samplerParameters.borderColorForm = BorderColorForm::Uint;
                m_samplerParameters.borderColorUI = color;
                m_samplerParameters.borderColorI =
                    IntVec4(static_cast<Int32>(color.x()), static_cast<Int32>(color.y()),
                            static_cast<Int32>(color.z()), static_cast<Int32>(color.w()));
                m_samplerParameters.borderColor =
                    FloatVec4(static_cast<Float>(color.x()), static_cast<Float>(color.y()),
                              static_cast<Float>(color.z()), static_cast<Float>(color.w()));
                BumpVersion();
            }

            const FloatVec4& SamplerObject::GetBorderColor() const {
                return m_samplerParameters.borderColor;
            }

            const IntVec4& SamplerObject::GetBorderColorI() const {
                return m_samplerParameters.borderColorI;
            }

            const UintVec4& SamplerObject::GetBorderColorUI() const {
                return m_samplerParameters.borderColorUI;
            }

            BorderColorForm SamplerObject::GetBorderColorForm() const {
                return m_samplerParameters.borderColorForm;
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
