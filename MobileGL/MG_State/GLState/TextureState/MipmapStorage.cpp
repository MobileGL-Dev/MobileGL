// MobileGL - MobileGL/MG_State/GLState/TextureState/MipmapStorage.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "MipmapStorage.h"

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            SizeT MipmapStorage::GetLevelCount() const {
                return m_data.size();
            }

            void MipmapStorage::AllocateLevel(Uint level, MipmapInput input) {
                // Grow only. GL respecifies exactly the level it is handed, so allocating level 0
                // must not disturb the levels above it - but resize() shrinks as readily as it
                // grows, so this used to truncate the whole chain to a single level. Callers that
                // genuinely redefine the complete level set say so with TruncateToLevelCount.
                const SizeT requiredLevelCount = static_cast<SizeT>(level) + 1;
                if (m_data.size() < requiredLevelCount) {
                    m_data.reserve(std::bit_ceil(requiredLevelCount));
                    m_data.resize(requiredLevelCount);
                    m_texelSizes.reserve(std::bit_ceil(requiredLevelCount));
                    m_texelSizes.resize(requiredLevelCount);
                    m_isDirty.resize(requiredLevelCount, false);
                    m_dirtyRegions.resize(requiredLevelCount);
                    m_compressedData.resize(requiredLevelCount);
                    m_compressedFormats.resize(requiredLevelCount, GL_NONE);
                }

                m_texelSizes[level] = input.texelSize;
                // A respecified level invalidates any pending sub-region: its extents were
                // measured against the old size. If the level is still flagged dirty the
                // pending upload widens to the whole (new) level.
                if (level < m_dirtyRegions.size()) {
                    m_dirtyRegions[level] =
                        m_isDirty[level]
                            ? MipmapDirtyRegion{IntVec3{0, 0, 0},
                                                IntVec3{input.texelSize.x(), input.texelSize.y(),
                                                        std::max(input.texelSize.z(), 1)}}
                            : MipmapDirtyRegion{};
                }
                auto& data = m_data[level];
                data.resize(input.byteSize, 0);

                // Respecifying a level drops whatever compressed image it used to hold. Without this,
                // a glTexImage2D or glTexStorage2D over a level a previous glCompressedTexImage2D had
                // shadowed would leave GL_TEXTURE_COMPRESSED answering true and glGetCompressedTexImage
                // handing back the stale blob. Every allocation path funnels through here, so clearing
                // once covers all of them; the compressed path re-arms the tag immediately afterwards
                // via SetCompressedImage.
                m_compressedFormats[level] = GL_NONE;
                m_compressedData[level].clear();
                m_compressedData[level].shrink_to_fit();
            }

            void MipmapStorage::SetCompressedImage(Uint level, GLenum internalFormat, const void* data, SizeT size) {
                MOBILEGL_ASSERT(level < m_compressedData.size(), "SetCompressedImage: level out of range");

                m_compressedFormats[level] = internalFormat;
                auto& blob = m_compressedData[level];
                // Zero-filled when data is null: glCompressedTexImage* with a null pointer defines the
                // level's size and format but leaves its contents undefined, and zeros are the one
                // reproducible answer a later glGetCompressedTexImage can give.
                blob.assign(size, 0);
                if (data != nullptr && size > 0) {
                    Memcpy(blob.data(), data, size);
                }
            }

            GLenum MipmapStorage::GetCompressedFormat(Uint level) const {
                if (level >= m_compressedFormats.size()) return GL_NONE;
                return m_compressedFormats[level];
            }

            SizeT MipmapStorage::GetCompressedByteSize(Uint level) const {
                if (level >= m_compressedData.size()) return 0;
                return m_compressedData[level].size();
            }

            const void* MipmapStorage::MapCompressedData(Uint level) const {
                if (level >= m_compressedData.size()) return nullptr;
                return m_compressedData[level].data();
            }

            void MipmapStorage::TruncateToLevelCount(SizeT levelCount) {
                if (levelCount >= m_data.size()) return;

                m_data.resize(levelCount);
                m_texelSizes.resize(levelCount);
                m_isDirty.resize(levelCount);
                m_dirtyRegions.resize(levelCount);
                m_compressedData.resize(levelCount);
                m_compressedFormats.resize(levelCount);
            }

            void MipmapStorage::UpdateSubData(Uint level, DataPtr input) {
                auto& targetData = m_data;
                MOBILEGL_ASSERT(level < targetData.size(), "UpdateSubData: level out of range");
                auto& levelData = targetData[level];
                MOBILEGL_ASSERT(input.size <= levelData.size(), "UpdateSubData: input data larger than allocated");

                if (input.data && input.size > 0) {
                    const Uint8* src = static_cast<const Uint8*>(input.data);
                    // Clamp so a size mismatch can never write past the allocation.
                    Memcpy(levelData.data(), src, std::min(input.size, levelData.size()));
                    MarkDirty(level, true); // whole-level write: dirty region covers everything
                }
            }

            void* MipmapStorage::MapData(Uint level) {
                auto& targetData = m_data;
                MOBILEGL_ASSERT(level < targetData.size(), "UpdateSubData: level out of range");
                auto& levelData = targetData[level];
                return levelData.data();
            }

            IntVec3 MipmapStorage::GetTexelSize(Uint level) const {
                auto& targetTexelSizes = m_texelSizes;
                if (level >= targetTexelSizes.size()) return {0, 0, 0};
                return targetTexelSizes[level];
            }

            SizeT MipmapStorage::GetByteSize(Uint level) const {
                if (level >= m_data.size()) return 0;
                return m_data[level].size();
            }

            void MipmapStorage::MarkDirty(Uint level, bool dirty) {
                MOBILEGL_ASSERT(level < m_isDirty.size(), "MarkDirty: level out of range");
                m_isDirty[level] = dirty;
                if (level < m_dirtyRegions.size()) {
                    if (dirty) {
                        const IntVec3 size = level < m_texelSizes.size() ? m_texelSizes[level] : IntVec3{0, 0, 0};
                        m_dirtyRegions[level] = {IntVec3{0, 0, 0},
                                                 IntVec3{size.x(), size.y(), std::max(size.z(), 1)}};
                    } else {
                        m_dirtyRegions[level] = {};
                    }
                }
            }

            bool MipmapStorage::IsDirty(Uint level) const {
                MOBILEGL_ASSERT(level < m_isDirty.size(), "IsDirty: level out of range");
                return m_isDirty[level];
            }

            void MipmapStorage::MarkDirtyRegion(Uint level, IntVec3 offset, IntVec3 size) {
                MOBILEGL_ASSERT(level < m_isDirty.size(), "MarkDirtyRegion: level out of range");
                const IntVec3 levelSize = level < m_texelSizes.size() ? m_texelSizes[level] : IntVec3{0, 0, 0};
                MipmapDirtyRegion incoming;
                incoming.lo = {std::max(offset.x(), 0), std::max(offset.y(), 0), std::max(offset.z(), 0)};
                incoming.hi = {std::min(offset.x() + size.x(), levelSize.x()),
                               std::min(offset.y() + size.y(), levelSize.y()),
                               std::min(offset.z() + std::max(size.z(), 1), std::max(levelSize.z(), 1))};
                if (incoming.Empty()) return;
                if (level < m_dirtyRegions.size()) {
                    MipmapDirtyRegion& region = m_dirtyRegions[level];
                    if (m_isDirty[level] && !region.Empty()) {
                        region.lo = {std::min(region.lo.x(), incoming.lo.x()),
                                     std::min(region.lo.y(), incoming.lo.y()),
                                     std::min(region.lo.z(), incoming.lo.z())};
                        region.hi = {std::max(region.hi.x(), incoming.hi.x()),
                                     std::max(region.hi.y(), incoming.hi.y()),
                                     std::max(region.hi.z(), incoming.hi.z())};
                    } else {
                        region = incoming;
                    }
                }
                m_isDirty[level] = true;
            }

            MipmapDirtyRegion MipmapStorage::GetDirtyRegion(Uint level) const {
                if (level >= m_dirtyRegions.size()) return {};
                return m_dirtyRegions[level];
            }
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
