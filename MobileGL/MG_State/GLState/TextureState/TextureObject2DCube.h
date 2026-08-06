// MobileGL - MobileGL/MG_State/GLState/TextureState/TextureObject2DCube.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "TextureObject.h"

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            class TextureObject2DCube : public TextureObjectMipmap {
            public:
                explicit TextureObject2DCube(Uint externalIndex);

                const Vector<TextureUploadTarget>& GetUploadTargets() const override { return m_uploadTargets; }

                Uint GetMipmapLevelCount() const override;
                const IntVec3 GetMipmapTexelSize(TextureUploadTarget target, Uint mipmapLevel) const override;
                const SizeT GetMipmapByteSize(TextureUploadTarget target, Uint mipmapLevel) const override;
                void AllocateStorage(TextureUploadTarget uploadTarget, Uint mipmapLevel, MipmapInput input) override;
                void TruncateMipmapLevels(TextureUploadTarget uploadTarget, Uint levelCount) override;
                void UpdateMipmapSubData(TextureUploadTarget uploadTarget, Uint mipmapLevel, DataPtr input) override;
                void* MapMipmapData(TextureUploadTarget uploadTarget, Uint mipmapLevel) override;
                void MarkStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel, bool dirty) override;
                bool IsStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel) const override;
                void MarkStorageDirtyRegion(TextureUploadTarget uploadTarget, Uint mipmapLevel, IntVec3 offset,
                                            IntVec3 size) override;
                MipmapDirtyRegion GetStorageDirtyRegion(TextureUploadTarget uploadTarget,
                                                        Uint mipmapLevel) const override;
                void SetMipmapCompressedImage(TextureUploadTarget uploadTarget, Uint mipmapLevel,
                                              GLenum internalFormat, const void* data, SizeT size) override;
                GLenum GetMipmapCompressedFormat(TextureUploadTarget uploadTarget, Uint mipmapLevel) const override;
                SizeT GetMipmapCompressedByteSize(TextureUploadTarget uploadTarget, Uint mipmapLevel) const override;
                const void* MapMipmapCompressedImage(TextureUploadTarget uploadTarget,
                                                     Uint mipmapLevel) const override;

                IntVec3 GetBaseSize() const override;
                Bool IsComplete() const override;

            protected:
                Uint GetIndexOfTextureUploadTarget(TextureUploadTarget target) const override;
                MipmapUploadTargetArray<6> m_textureStorage;
                const Vector<TextureUploadTarget> m_uploadTargets{
                    TextureUploadTarget::CubeMapPositiveX, TextureUploadTarget::CubeMapNegativeX,
                    TextureUploadTarget::CubeMapPositiveY, TextureUploadTarget::CubeMapNegativeY,
                    TextureUploadTarget::CubeMapPositiveZ, TextureUploadTarget::CubeMapNegativeZ,
                };
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
