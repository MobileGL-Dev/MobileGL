// MobileGL - MobileGL/MG_State/GLState/TextureState/TextureObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "TextureEnum.h"
#include "MipmapUploadTargetArray.h"
#include "MG_Util/Types.h"
#include "../SamplerState/SamplerObject.h"
#include <Includes.h>
#include <MG_Util/Math/VectorTypes.h>

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            class ITextureObject {
            public:
                using TargetEnum = TextureTarget;
                virtual ~ITextureObject() = default;

                virtual TextureStorageType GetStorageType() const = 0;

                virtual TextureInternalFormat GetFormat() const = 0;
                virtual TextureTarget GetTarget() const = 0;
                virtual const Vector<TextureUploadTarget>& GetUploadTargets() const = 0;
                virtual IntVec3 GetBaseSize() const = 0;
                virtual SharedPtr<SamplerObject> GetSamplerObject() const = 0;
                virtual void SetInternalFormat(TextureInternalFormat format) = 0;
                virtual Bool IsComplete() const = 0;
                virtual Uint GetExternalIndex() const = 0;
                virtual const FloatVec4& GetBorderColor() const = 0;
                virtual void SetBorderColor(const FloatVec4& color) = 0;
                virtual TextureSwizzleParam GetSwizzleParam(TextureSwizzleParam param) const = 0;
                virtual void SetSwizzleParam(TextureSwizzleParam param, TextureSwizzleParam value) = 0;
                virtual void SetSwizzleParamRGBA(const Vec4<TextureSwizzleParam>& values) = 0;
                virtual const Vec4<TextureSwizzleParam>& GetAllSwizzleParams() const = 0;
                virtual const UintVec2& GetLevelRange() const = 0;
                virtual void SetBaseLevel(Uint baseLevel) = 0;
                virtual void SetMaxLevel(Uint maxLevel) = 0;
                virtual Uint16 GetTextureParamsVersion() const = 0;

            protected:
                virtual Uint GetIndexOfTextureUploadTarget(TextureUploadTarget target) const = 0;
            };

            class TextureObjectBase : public ITextureObject {
            public:
                TextureObjectBase(TextureTarget target, Uint externalIndex);
                virtual ~TextureObjectBase() = default;

                TextureInternalFormat GetFormat() const override;
                TextureTarget GetTarget() const override;
                IntVec3 GetBaseSize() const override;
                SharedPtr<SamplerObject> GetSamplerObject() const override;
                void SetInternalFormat(TextureInternalFormat format) override;
                Bool IsComplete() const override;
                Uint GetExternalIndex() const override;
                const FloatVec4& GetBorderColor() const override;
                void SetBorderColor(const FloatVec4& color) override;
                TextureSwizzleParam GetSwizzleParam(TextureSwizzleParam param) const override;
                const Vec4<TextureSwizzleParam>& GetAllSwizzleParams() const override;
                void SetSwizzleParam(TextureSwizzleParam param, TextureSwizzleParam value) override;
                void SetSwizzleParamRGBA(const Vec4<TextureSwizzleParam>& values) override;
                const UintVec2& GetLevelRange() const override;
                void SetBaseLevel(Uint baseLevel) override;
                void SetMaxLevel(Uint maxLevel) override;
                Uint16 GetTextureParamsVersion() const override;

            protected:
                const Uint m_externalIndex;
                const TextureTarget m_target = TextureTarget::Unknown;
                TextureInternalFormat m_internalFormat = TextureInternalFormat::Unknown;
                SharedPtr<SamplerObject> m_sampler = nullptr;
                FloatVec4 m_borderColor = {0.0f, 0.0f, 0.0f, 0.0f};
                Vec4<TextureSwizzleParam> m_swizzleParams = {TextureSwizzleParam::Red, TextureSwizzleParam::Green,
                                                             TextureSwizzleParam::Blue, TextureSwizzleParam::Alpha};
                UintVec2 m_levelRange = {0, 1000};
                Uint16 m_textureParamsVersion = 0;
            };

            class TextureObjectMipmap : public TextureObjectBase {
            public:
                TextureObjectMipmap(TextureTarget target, Uint externalIndex)
                    : TextureObjectBase(target, externalIndex) {}

                TextureStorageType GetStorageType() const override { return TextureStorageType::Mipmap; }

                virtual Uint GetMipmapLevelCount() const = 0;
                virtual const IntVec3 GetMipmapTexelSize(TextureUploadTarget target, Uint mipmapLevel) const = 0;
                virtual const SizeT GetMipmapByteSize(TextureUploadTarget target, Uint mipmapLevel) const = 0;
                virtual void AllocateStorage(TextureUploadTarget uploadTarget, Uint mipmapLevel, MipmapInput input) = 0;
                virtual void UpdateMipmapSubData(TextureUploadTarget uploadTarget, Uint mipmapLevel, DataPtr input) = 0;
                virtual void* MapMipmapData(TextureUploadTarget uploadTarget, Uint mipmapLevel) = 0;
                virtual void MarkStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel,
                                              Bool dirty = true) = 0;
                virtual Bool IsStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel) const = 0;
            };

            class TextureObjectWithOneMipmap : public TextureObjectMipmap {
            public:
                TextureObjectWithOneMipmap(TextureTarget target, Uint externalIndex)
                    : TextureObjectMipmap(target, externalIndex) {}
                virtual ~TextureObjectWithOneMipmap() = default;

                Uint GetMipmapLevelCount() const override;
                const IntVec3 GetMipmapTexelSize(TextureUploadTarget target, Uint mipmapLevel) const override;
                const SizeT GetMipmapByteSize(TextureUploadTarget target, Uint mipmapLevel) const override;
                void AllocateStorage(TextureUploadTarget uploadTarget, Uint mipmapLevel, MipmapInput input) override;
                void UpdateMipmapSubData(TextureUploadTarget uploadTarget, Uint mipmapLevel, DataPtr input) override;
                void* MapMipmapData(TextureUploadTarget uploadTarget, Uint mipmapLevel) override;
                void MarkStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel, Bool dirty) override;
                bool IsStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel) const override;

                IntVec3 GetBaseSize() const override;
                Bool IsComplete() const override;

            protected:
                MipmapUploadTargetArray<1> m_textureStorage;
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
