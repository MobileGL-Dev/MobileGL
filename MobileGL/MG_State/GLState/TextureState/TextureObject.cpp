// MobileGL - MobileGL/MG_State/GLState/TextureState/TextureObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TextureObject.h"
#include "MG_Util/Types.h"
#include <MG_Util/Metrics/TextureMetrics.h>

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            static std::atomic<Uint64> s_nextTextureLifetimeId = 1;

            // TextureObjectBase implementations
            Uint64 TextureObjectBase::AllocateLifetimeId() {
                return s_nextTextureLifetimeId.fetch_add(1, std::memory_order_relaxed);
            }

            TextureObjectBase::TextureObjectBase(TextureTarget target, Uint externalIndex)
                : m_externalIndex(externalIndex), m_lifetimeId(AllocateLifetimeId()), m_target(target) {
                m_sampler = MakeShared<SamplerObject>(0);
            }

            TextureInternalFormat TextureObjectBase::GetFormat() const {
                return m_internalFormat;
            }

            TextureTarget TextureObjectBase::GetTarget() const {
                return m_target;
            }

            IntVec3 TextureObjectBase::GetBaseSize() const {
                return {0, 0, 0};
            }

            const SharedPtr<SamplerObject>& TextureObjectBase::GetSamplerObject() const {
                return m_sampler;
            }

            Bool TextureObjectBase::IsComplete() const {
                if (m_internalFormat == TextureInternalFormat::Unknown) {
                    return false;
                }

                return true;
            }

            void TextureObjectBase::SetInternalFormat(TextureInternalFormat format) {
                if (format == m_internalFormat) return;

                m_internalFormat = format;
                ++m_textureParamsVersion;
            }

            Uint TextureObjectBase::GetExternalIndex() const {
                return m_externalIndex;
            }

            const FloatVec4& TextureObjectBase::GetBorderColor() const {
                return m_borderColor;
            }

            void TextureObjectBase::SetBorderColor(const FloatVec4& color) {
                if (color == m_borderColor) return;

                m_borderColor = color;
                m_borderColorI = IntVec4(static_cast<Int32>(color.x()), static_cast<Int32>(color.y()),
                                         static_cast<Int32>(color.z()), static_cast<Int32>(color.w()));
                m_borderColorUI = UintVec4(static_cast<Uint32>(color.x()), static_cast<Uint32>(color.y()),
                                           static_cast<Uint32>(color.z()), static_cast<Uint32>(color.w()));
                ++m_textureParamsVersion;
            }

            const IntVec4& TextureObjectBase::GetBorderColorI() const {
                return m_borderColorI;
            }

            void TextureObjectBase::SetBorderColorI(const IntVec4& color) {
                if (color == m_borderColorI) return;

                m_borderColorI = color;
                m_borderColorUI = UintVec4(static_cast<Uint32>(color.x()), static_cast<Uint32>(color.y()),
                                           static_cast<Uint32>(color.z()), static_cast<Uint32>(color.w()));
                m_borderColor = FloatVec4(static_cast<Float>(color.x()), static_cast<Float>(color.y()),
                                          static_cast<Float>(color.z()), static_cast<Float>(color.w()));
                ++m_textureParamsVersion;
            }

            const UintVec4& TextureObjectBase::GetBorderColorUI() const {
                return m_borderColorUI;
            }

            void TextureObjectBase::SetBorderColorUI(const UintVec4& color) {
                if (color == m_borderColorUI) return;

                m_borderColorUI = color;
                m_borderColorI = IntVec4(static_cast<Int32>(color.x()), static_cast<Int32>(color.y()),
                                         static_cast<Int32>(color.z()), static_cast<Int32>(color.w()));
                m_borderColor = FloatVec4(static_cast<Float>(color.x()), static_cast<Float>(color.y()),
                                          static_cast<Float>(color.z()), static_cast<Float>(color.w()));
                ++m_textureParamsVersion;
            }

            TextureSwizzleParam TextureObjectBase::GetSwizzleParam(TextureSwizzleParam param) const {
                switch (param) {
                case TextureSwizzleParam::Red:
                    return m_swizzleParams.r();
                case TextureSwizzleParam::Green:
                    return m_swizzleParams.g();
                case TextureSwizzleParam::Blue:
                    return m_swizzleParams.b();
                case TextureSwizzleParam::Alpha:
                    return m_swizzleParams.a();
                default:
                    MOBILEGL_ASSERT(false, "TextureObjectBase::GetSwizzleParam: Invalid TextureSwizzleParam: %d",
                                    static_cast<Int>(param));
                    return TextureSwizzleParam::Red;
                }
            }

            const Vec4<TextureSwizzleParam>& TextureObjectBase::GetAllSwizzleParams() const {
                return m_swizzleParams;
            }

            void TextureObjectBase::SetSwizzleParam(TextureSwizzleParam param, TextureSwizzleParam value) {
                if (GetSwizzleParam(param) == value) return;

                switch (param) {
                case TextureSwizzleParam::Red:
                    m_swizzleParams.r() = value;
                    break;
                case TextureSwizzleParam::Green:
                    m_swizzleParams.g() = value;
                    break;
                case TextureSwizzleParam::Blue:
                    m_swizzleParams.b() = value;
                    break;
                case TextureSwizzleParam::Alpha:
                    m_swizzleParams.a() = value;
                    break;
                default:
                    MOBILEGL_ASSERT(false, "TextureObjectBase::SetSwizzleParam: Invalid TextureSwizzleParam: %d",
                                    static_cast<Int>(param));
                    break;
                }
                ++m_textureParamsVersion;
            }

            void TextureObjectBase::SetSwizzleParamRGBA(const Vec4<TextureSwizzleParam>& values) {
                if (values == m_swizzleParams) return;

                m_swizzleParams = values;
                ++m_textureParamsVersion;
            }

            const UintVec2& TextureObjectBase::GetLevelRange() const {
                return m_levelRange;
            }

            void TextureObjectBase::SetBaseLevel(Uint baseLevel) {
                if (baseLevel == m_levelRange.x()) return;

                m_levelRange.x() = baseLevel;
                ++m_textureParamsVersion;
            }

            void TextureObjectBase::SetMaxLevel(Uint maxLevel) {
                if (maxLevel == m_levelRange.y()) return;

                m_levelRange.y() = maxLevel;
                ++m_textureParamsVersion;
            }

            Uint16 TextureObjectBase::GetTextureParamsVersion() const {
                return m_textureParamsVersion;
            }

            Int TextureObjectBase::GetSamples() const {
                return m_samples;
            }

            void TextureObjectBase::SetSamples(Int samples) {
                m_samples = samples;
            }

            Bool TextureObjectBase::HasFixedSampleLocations() const {
                return m_fixedSampleLocations;
            }

            void TextureObjectBase::SetFixedSampleLocations(Bool fixedSampleLocations) {
                m_fixedSampleLocations = fixedSampleLocations;
            }

            Uint64 TextureObjectBase::GetLifetimeId() const {
                return m_lifetimeId;
            }

            Uint TextureObjectWithOneMipmap::GetMipmapLevelCount() const {
                return m_textureStorage.GetLevelCount();
            }

            const IntVec3 TextureObjectWithOneMipmap::GetMipmapTexelSize(TextureUploadTarget target,
                                                                         Uint mipmapLevel) const {
                return m_textureStorage.GetTexelSize(GetIndexOfTextureUploadTarget(target), mipmapLevel);
            }

            const SizeT TextureObjectWithOneMipmap::GetMipmapByteSize(TextureUploadTarget target,
                                                                      Uint mipmapLevel) const {
                return m_textureStorage.GetByteSize(GetIndexOfTextureUploadTarget(target), mipmapLevel);
            }

            void TextureObjectWithOneMipmap::AllocateStorage(TextureUploadTarget uploadTarget, Uint mipmapLevel,
                                                             MipmapInput input) {
                m_textureStorage.AllocateLevel(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel, input);
            }

            void TextureObjectWithOneMipmap::UpdateMipmapSubData(TextureUploadTarget uploadTarget, Uint mipmapLevel,
                                                                 DataPtr input) {
                m_textureStorage.UpdateSubData(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel, input);
            }

            void* TextureObjectWithOneMipmap::MapMipmapData(TextureUploadTarget uploadTarget, Uint mipmapLevel) {
                return m_textureStorage.MapData(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel);
            }

            void TextureObjectWithOneMipmap::MarkStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel,
                                                              Bool dirty) {
                m_textureStorage.MarkDirty(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel, dirty);
            }

            Bool TextureObjectWithOneMipmap::IsStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel) const {
                return m_textureStorage.IsDirty(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel);
            }

            IntVec3 TextureObjectWithOneMipmap::GetBaseSize() const {
                if (m_textureStorage.GetLevelCount() == 0) {
                    return {0, 0, 0};
                }
                return m_textureStorage.GetTexelSize(0, 0);
            }

            Bool TextureObjectWithOneMipmap::IsComplete() const {
                if (!TextureObjectBase::IsComplete()) return false;

                SizeT levelCount = m_textureStorage.GetLevelCount();
                if (levelCount == 0) {
                    MGLOG_D("%s: not complete because levelCount == 0", __func__);
                    return false;
                }

                // For some reason mojang decided to have 0x0 in last level mipmap
                // Relaxing checks for that
                Bool hadZero = false;
                for (SizeT i = 0; i < levelCount; ++i) {
                    const auto& levelSize = m_textureStorage.GetTexelSize(0, i);
                    if (levelSize.x() <= 0 || levelSize.y() <= 0 || levelSize.z() <= 0) {
                        hadZero = true;
                    } else {
                        if (hadZero) {
                            // We're checking for "zero - not zero - zero" here
                            // "not zero - zero - zero" should pass this test
                            MGLOG_D("%s: not complete because 0x0 occurred, and is not last level mipmap", __func__);
                            return false;
                        }
                    }
                }

                // TODO: add more completeness checks based on texture type and mipmap levels
                return true;
            }

            // TODO: add other texture types as needed

        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
