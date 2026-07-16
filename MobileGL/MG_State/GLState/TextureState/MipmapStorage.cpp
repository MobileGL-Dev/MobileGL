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
                m_data.reserve(std::bit_ceil(level + 1));
                m_data.resize(level + 1);
                m_texelSizes.reserve(std::bit_ceil(level + 1));
                m_texelSizes.resize(level + 1);
                m_texelSizes[level] = input.texelSize;
                m_isDirty.resize(level + 1, false);

                auto& data = m_data[level];
                data.resize(input.byteSize, 0);
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
                    m_isDirty[level] = true;
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
                return m_data[level].size();
            }

            void MipmapStorage::MarkDirty(Uint level, bool dirty) {
                MOBILEGL_ASSERT(level < m_isDirty.size(), "MarkDirty: level out of range");
                m_isDirty[level] = dirty;
            }

            bool MipmapStorage::IsDirty(Uint level) const {
                MOBILEGL_ASSERT(level < m_isDirty.size(), "IsDirty: level out of range");
                return m_isDirty[level];
            }
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
