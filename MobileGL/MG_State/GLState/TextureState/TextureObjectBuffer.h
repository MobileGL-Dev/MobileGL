// MobileGL - MobileGL/MG_State/GLState/TextureState/TextureObjectBuffer.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "TextureObject.h"
#include <MG_State/GLState/BufferState/BufferObject.h>

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            class TextureObjectBuffer : public TextureObjectBase {
            public:
                TextureStorageType GetStorageType() const override { return TextureStorageType::Buffer; }
                explicit TextureObjectBuffer(Uint externalIndex);
                const Vector<TextureUploadTarget>& GetUploadTargets() const override { return m_uploadTargets; }
                BindingSlot<BufferObject>& GetBufferBindingSlot(
                    TextureUploadTarget target = TextureUploadTarget::TextureBuffer);

                // The window of the attached buffer the texture addresses. glTexBuffer attaches
                // the whole buffer, which is expressed here as an offset of 0 and a size of
                // kWholeBuffer so a later respecify of the buffer keeps being followed - a stored
                // size would freeze the texture at the size the buffer happened to have.
                static constexpr SizeT kWholeBuffer = ~static_cast<SizeT>(0);
                void SetBufferRange(SizeT offset, SizeT size) {
                    m_bufferRangeOffset = offset;
                    m_bufferRangeSize = size;
                }
                SizeT GetBufferRangeOffset() const { return m_bufferRangeOffset; }
                // Resolved against the buffer's current size, so kWholeBuffer tracks it.
                SizeT GetBufferRangeSizeInBytes() const {
                    const auto& buffer = m_bufferBindingSlot.GetBoundObject();
                    const SizeT bufferSize = buffer != nullptr ? buffer->GetSize() : 0;
                    if (m_bufferRangeSize == kWholeBuffer) return bufferSize;
                    const SizeT available = bufferSize > m_bufferRangeOffset ? bufferSize - m_bufferRangeOffset : 0;
                    return std::min(m_bufferRangeSize, available);
                }

            protected:
                Uint GetIndexOfTextureUploadTarget(TextureUploadTarget target) const override;

                BindingSlot<BufferObject> m_bufferBindingSlot = BindingSlot<BufferObject>(BufferTarget::Texture);
                SizeT m_bufferRangeOffset = 0;
                SizeT m_bufferRangeSize = kWholeBuffer;
                const Vector<TextureUploadTarget> m_uploadTargets{TextureUploadTarget::TextureBuffer};
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
