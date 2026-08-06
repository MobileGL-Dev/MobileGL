// MobileGL - MobileGL/MG_State/GLState/TextureState/TextureObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TextureObject.h"
#include "MG_State/GLState/Core.h"
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
                if (target == TextureTarget::TextureRectangle) {
                    // A rectangle texture has no mip chain, so its initial sampler state is not
                    // the shared one: TEXTURE_MIN_FILTER is LINEAR and TEXTURE_WRAP_S/T are
                    // CLAMP_TO_EDGE (GL 4.6 core table 23.15). Leaving the 2D default of
                    // NEAREST_MIPMAP_LINEAR in place makes the texture mipmap-incomplete from
                    // birth, and every lookup that the application never re-filtered reads
                    // (0, 0, 0, 1) instead of its contents.
                    m_sampler->SetMinFilter(SamplerFilterMode::Linear);
                    m_sampler->SetMipmapMode(SamplerMipmapMode::None);
                    m_sampler->SetWrapS(SamplerWrapMode::ClampToEdge);
                    m_sampler->SetWrapT(SamplerWrapMode::ClampToEdge);
                    m_sampler->SetWrapR(SamplerWrapMode::ClampToEdge);
                }
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

                // A default texture (name 0) changes IsUndefinedDefaultTexture on the
                // Unknown<->defined transition, which changes per-draw sampled-set membership
                // without any bind happening; bump the bind generation so cached sampled sets
                // re-resolve instead of replaying the stale membership. The identity check
                // excludes the other externalIndex-0 objects (proxy textures, default-FBO
                // attachments) whose definedness never feeds sampled-set membership, so e.g.
                // proxy probes cannot churn the cache.
                if (m_externalIndex == 0 && pGLContext &&
                    (m_internalFormat == TextureInternalFormat::Unknown) !=
                        (format == TextureInternalFormat::Unknown) &&
                    pGLContext->GetDefaultTextureObject(GetTarget()).get() == this) {
                    pGLContext->BumpTextureBindGeneration();
                }

                m_internalFormat = format;
                ++m_textureParamsVersion;
            }

            Uint TextureObjectBase::GetExternalIndex() const {
                return m_externalIndex;
            }

            // TEXTURE_BORDER_COLOR is sampler state, so it lives on the SamplerObject this texture
            // owns rather than being duplicated here - a sampler object bound over the texture then
            // supplies its own, exactly as GL says it should. The texture params version still moves
            // on a write, because the DirectGLES texture sync memoises on it.
            const FloatVec4& TextureObjectBase::GetBorderColor() const {
                return m_sampler->GetBorderColor();
            }

            void TextureObjectBase::SetBorderColor(const FloatVec4& color) {
                if (color == m_sampler->GetBorderColor()) return;

                m_sampler->SetBorderColor(color);
                ++m_textureParamsVersion;
            }

            const IntVec4& TextureObjectBase::GetBorderColorI() const {
                return m_sampler->GetBorderColorI();
            }

            void TextureObjectBase::SetBorderColorI(const IntVec4& color) {
                if (color == m_sampler->GetBorderColorI()) return;

                m_sampler->SetBorderColorI(color);
                ++m_textureParamsVersion;
            }

            const UintVec4& TextureObjectBase::GetBorderColorUI() const {
                return m_sampler->GetBorderColorUI();
            }

            void TextureObjectBase::SetBorderColorUI(const UintVec4& color) {
                if (color == m_sampler->GetBorderColorUI()) return;

                m_sampler->SetBorderColorUI(color);
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
                if (IsImmutable() && m_immutableLevels > 0) {
                    baseLevel = std::min(baseLevel, m_immutableLevels - 1);
                }
                if (baseLevel == m_levelRange.x()) return;

                m_levelRange.x() = baseLevel;
                if (IsImmutable() && m_levelRange.y() < m_levelRange.x()) {
                    m_levelRange.y() = m_levelRange.x();
                }
                ++m_textureParamsVersion;
            }

            void TextureObjectBase::SetMaxLevel(Uint maxLevel) {
                if (IsImmutable() && m_immutableLevels > 0) {
                    maxLevel = std::min(std::max(maxLevel, m_levelRange.x()), m_immutableLevels - 1);
                }
                if (maxLevel == m_levelRange.y()) return;

                m_levelRange.y() = maxLevel;
                ++m_textureParamsVersion;
            }

            Bool TextureObjectBase::IsImmutable() const {
                return m_immutableLevels > 0;
            }

            Uint TextureObjectBase::GetImmutableLevels() const {
                return m_immutableLevels;
            }

            void TextureObjectBase::SetImmutableLevels(Uint levels) {
                if (m_immutableLevels == levels) return;

                m_immutableLevels = levels;
                if (m_immutableLevels > 0) {
                    m_levelRange.x() = std::min(m_levelRange.x(), m_immutableLevels - 1);
                    m_levelRange.y() = std::min(std::max(m_levelRange.y(), m_levelRange.x()), m_immutableLevels - 1);
                }
                ++m_textureParamsVersion;
            }

            Uint16 TextureObjectBase::GetTextureParamsVersion() const {
                return m_textureParamsVersion;
            }

            Uint64 TextureObjectBase::GetContentVersion() const {
                return m_contentVersion;
            }

            void TextureObjectBase::BumpContentVersion() {
                ++m_contentVersion;
            }

            Int TextureObjectBase::GetSamples() const {
                return m_samples;
            }

            void TextureObjectBase::SetSamples(Int samples) {
                m_samples = samples;
                ++m_textureParamsVersion;
            }

            Bool TextureObjectBase::HasFixedSampleLocations() const {
                return m_fixedSampleLocations;
            }

            void TextureObjectBase::SetFixedSampleLocations(Bool fixedSampleLocations) {
                m_fixedSampleLocations = fixedSampleLocations;
                ++m_textureParamsVersion;
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

            void TextureObjectWithOneMipmap::TruncateMipmapLevels(TextureUploadTarget uploadTarget, Uint levelCount) {
                m_textureStorage.TruncateToLevelCount(GetIndexOfTextureUploadTarget(uploadTarget), levelCount);
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
                if (dirty) {
                    ++m_contentVersion;
                }
                m_textureStorage.MarkDirty(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel, dirty);
            }

            Bool TextureObjectWithOneMipmap::IsStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel) const {
                return m_textureStorage.IsDirty(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel);
            }

            void TextureObjectWithOneMipmap::MarkStorageDirtyRegion(TextureUploadTarget uploadTarget, Uint mipmapLevel,
                                                                    IntVec3 offset, IntVec3 size) {
                ++m_contentVersion;
                m_textureStorage.MarkDirtyRegion(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel, offset,
                                                 size);
            }

            MipmapDirtyRegion TextureObjectWithOneMipmap::GetStorageDirtyRegion(TextureUploadTarget uploadTarget,
                                                                                Uint mipmapLevel) const {
                return m_textureStorage.GetDirtyRegion(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel);
            }

            void TextureObjectWithOneMipmap::SetMipmapCompressedImage(TextureUploadTarget uploadTarget, Uint mipmapLevel,
                                                              GLenum internalFormat, const void* data, SizeT size) {
                m_textureStorage.SetCompressedImage(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel,
                                                    internalFormat, data, size);
            }

            GLenum TextureObjectWithOneMipmap::GetMipmapCompressedFormat(TextureUploadTarget uploadTarget,
                                                                  Uint mipmapLevel) const {
                return m_textureStorage.GetCompressedFormat(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel);
            }

            SizeT TextureObjectWithOneMipmap::GetMipmapCompressedByteSize(TextureUploadTarget uploadTarget,
                                                                    Uint mipmapLevel) const {
                return m_textureStorage.GetCompressedByteSize(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel);
            }

            const void* TextureObjectWithOneMipmap::MapMipmapCompressedImage(TextureUploadTarget uploadTarget,
                                                                       Uint mipmapLevel) const {
                return m_textureStorage.MapCompressedData(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel);
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

        Bool SamplesAsIncompleteTexture(const ITextureObject* texture, const SamplerObject* effectiveSampler) {
            const Bool mipmapped =
                effectiveSampler != nullptr && effectiveSampler->GetMipmapMode() != SamplerMipmapMode::None;
            return !IsMipmapCompleteForFilter(texture, mipmapped);
        }

        Bool IsMipmapCompleteForFilter(const ITextureObject* texture, Bool mipmapped) {
            if (texture == nullptr) return true;
            if (!texture->IsComplete()) return false;
            if (!mipmapped) return true;

            const auto* mipmapTexture = AsMipmapTexture(texture);
            if (mipmapTexture == nullptr) return true; // no mip chain to be incomplete about

            const UintVec2& levelRange = texture->GetLevelRange();
            const Uint baseLevel = levelRange.x();
            const Uint storedLevels = mipmapTexture->GetMipmapLevelCount();
            if (baseLevel >= storedLevels) return false;

            // An array texture's layer count is not a dimension of the image: it stays put all
            // the way down the chain (GL 4.6 core 8.14.3). GetMipmapTexelSize reports it in the
            // slot after the image's own dimensions.
            const TextureTarget target = texture->GetTarget();
            Int shrinkingComponents = 3;
            if (target == TextureTarget::Texture1DArray) {
                shrinkingComponents = 1;
            } else if (target == TextureTarget::Texture2DArray || target == TextureTarget::TextureCubeMapArray) {
                shrinkingComponents = 2;
            }

            for (const auto uploadTarget : texture->GetUploadTargets()) {
                const IntVec3 baseSize = mipmapTexture->GetMipmapTexelSize(uploadTarget, baseLevel);
                Int largest = 0;
                for (Int component = 0; component < shrinkingComponents; ++component) {
                    largest = std::max(largest, baseSize[component]);
                }
                if (largest <= 0) return false;

                // p = log2 of the largest base dimension: the last level the chain needs
                // before every dimension has reached 1. TEXTURE_MAX_LEVEL can cut it short.
                Uint p = 0;
                for (Int extent = largest; extent > 1; extent >>= 1) ++p;
                const Uint lastLevel = std::min(baseLevel + p, levelRange.y());

                for (Uint level = baseLevel; level <= lastLevel; ++level) {
                    if (level >= storedLevels) return false;
                    const IntVec3 actual = mipmapTexture->GetMipmapTexelSize(uploadTarget, level);
                    for (Int component = 0; component < 3; ++component) {
                        const Int expected = component < shrinkingComponents
                            ? std::max(1, baseSize[component] >> (level - baseLevel))
                            : baseSize[component];
                        if (actual[component] != expected) return false;
                    }
                }
            }
            return true;
        }

        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
