// MobileGL - MobileGL/MG_State/GLState/TextureState/TextureObject2DCube.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TextureObject2DCube.h"

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            TextureObject2DCube::TextureObject2DCube(Uint externalIndex)
                : TextureObjectMipmap(TextureTarget::TextureCubeMap, externalIndex) {}

            Uint TextureObject2DCube::GetMipmapLevelCount() const {
                return m_textureStorage.GetLevelCount();
            }

            const IntVec3 TextureObject2DCube::GetMipmapTexelSize(TextureUploadTarget target, Uint mipmapLevel) const {
                return m_textureStorage.GetTexelSize(GetIndexOfTextureUploadTarget(target), mipmapLevel);
            }

            const SizeT TextureObject2DCube::GetMipmapByteSize(TextureUploadTarget target, Uint mipmapLevel) const {
                return m_textureStorage.GetByteSize(GetIndexOfTextureUploadTarget(target), mipmapLevel);
            }

            void TextureObject2DCube::AllocateStorage(TextureUploadTarget uploadTarget, Uint mipmapLevel,
                                                      MipmapInput input) {
                m_textureStorage.AllocateLevel(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel, input);
            }

            void TextureObject2DCube::UpdateMipmapSubData(TextureUploadTarget uploadTarget, Uint mipmapLevel,
                                                          DataPtr input) {
                m_textureStorage.UpdateSubData(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel, input);
            }

            void* TextureObject2DCube::MapMipmapData(TextureUploadTarget uploadTarget, Uint mipmapLevel) {
                return m_textureStorage.MapData(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel);
            }

            void TextureObject2DCube::MarkStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel, bool dirty) {
                m_textureStorage.MarkDirty(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel, dirty);
            }

            bool TextureObject2DCube::IsStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel) const {
                return m_textureStorage.IsDirty(GetIndexOfTextureUploadTarget(uploadTarget), mipmapLevel);
            }

            Uint TextureObject2DCube::GetIndexOfTextureUploadTarget(TextureUploadTarget target) const {
                MOBILEGL_ASSERT(TextureUploadTarget::CubeMapPositiveX <= target &&
                                    target <= TextureUploadTarget::CubeMapNegativeZ,
                                "Invalid TextureUploadTarget!");
                return (Uint)target - (Uint)TextureUploadTarget::CubeMapPositiveX;
            }

            IntVec3 TextureObject2DCube::GetBaseSize() const {
                if (m_textureStorage.GetLevelCount() == 0) {
                    return {0, 0, 0};
                }
                return m_textureStorage.GetTexelSize(0, 0);
            }

            Bool TextureObject2DCube::IsComplete() const {
                if (!TextureObjectBase::IsComplete()) return false;

                SizeT levelCount = m_textureStorage.GetLevelCount();
                if (levelCount == 0) {
                    return false;
                }

                for (SizeT t = 0; t < 6; ++t) {
                    for (SizeT i = 0; i < levelCount; ++i) {
                        const auto& levelSize = m_textureStorage.GetTexelSize(t, i);
                        if (levelSize.x() <= 0 || levelSize.y() <= 0 || levelSize.z() <= 0) {
                            return false;
                        }
                    }
                }

                // TODO: add more completeness checks based on texture type and mipmap levels
                return true;
            }
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
