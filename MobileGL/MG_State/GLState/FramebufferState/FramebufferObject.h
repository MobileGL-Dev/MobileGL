// MobileGL - MobileGL/MG_State/GLState/FramebufferState/FramebufferObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "MG_Util/Types.h"
#include <Includes.h>
#include <MG_State/GLState/TextureState/TextureObject.h>
#include <MG_State/GLState/RenderbufferState/RenderbufferObject.h>

namespace MobileGL {
    enum class FramebufferTarget {
        Draw,
        Read,
        FramebufferTargetCount,
        Unknown = -1
    };

    enum class FramebufferAttachmentType {
        None,

        FrontLeft,
        FrontRight,
        BackLeft,
        BackRight,

        Depth,
        Stencil,

        Color0,
        Color1,
        Color2,
        Color3,
        Color4,
        Color5,
        Color6,
        Color7,
        Color8,
        Color9,
        Color10,
        Color11,
        Color12,
        Color13,
        Color14,
        Color15,
        Color16,
        Color17,
        Color18,
        Color19,
        Color20,
        Color21,
        Color22,
        Color23,
        Color24,
        Color25,
        Color26,
        Color27,
        Color28,
        Color29,
        Color30,
        Color31,
        ColorMax = Color31,

        FramebufferAttachmentTypeCount,
        Unknown = -1
    };

    namespace MG_State::GLState {
        class FramebufferAttachmentObject {
        public:
            explicit FramebufferAttachmentObject(const SharedPtr<MG_State::GLState::ITextureObject>& texture,
                                                 TextureUploadTarget textureUploadTarget,
                                                 Int level = 0, Int layer = 0, Bool layered = false);
            explicit FramebufferAttachmentObject(const SharedPtr<RenderbufferObject>& renderbuffer);
            explicit FramebufferAttachmentObject(Bool IsValid = true);

            Bool IsTexture() const;
            Bool IsRenderbuffer() const;
            Bool IsEmpty() const;
            const SharedPtr<MG_State::GLState::ITextureObject>& GetTexture() const;
            const SharedPtr<RenderbufferObject>& GetRenderbuffer() const;
            Int GetTextureLevel() const;
            Int GetTextureLayer() const;
            Bool IsLayered() const;
            TextureUploadTarget GetTextureUploadTarget() const;
            Bool IsComplete() const;
            IntVec3 GetSize() const;
            Bool IsValid() const;

        private:
            SharedPtr<MG_State::GLState::ITextureObject> m_texture = nullptr;
            SharedPtr<RenderbufferObject> m_renderbuffer = nullptr;
            TextureUploadTarget m_textureUploadTarget = TextureUploadTarget::Unknown;
            Int m_textureLevel = 0;
            Int m_textureLayer = 0;
            Bool m_layered = false;
            Bool m_isValid = true;
        };

        class FramebufferObject {
        public:
            static constexpr Uint MAX_DRAW_BUFFERS = 8;

            using TargetEnum = FramebufferTarget;
            using FramebufferAttachmentObjectArray =
                Array<FramebufferAttachmentObject,
                      static_cast<SizeT>(FramebufferAttachmentType::FramebufferAttachmentTypeCount)>;
            using FramebufferAttachmentArray = Array<FramebufferAttachmentType, MAX_DRAW_BUFFERS>;
            using FramebufferAttachmentVersionArray =
                Array<Uint16, static_cast<SizeT>(FramebufferAttachmentType::FramebufferAttachmentTypeCount)>;

            FramebufferObject(Uint externalIndex);

            void AttachTexture(FramebufferAttachmentType type, const SharedPtr<ITextureObject>& texture,
                               TextureUploadTarget textureUploadTarget = TextureUploadTarget::Unknown, int level = 0,
                               int layer = 0, Bool layered = false);
            void AttachRenderbuffer(FramebufferAttachmentType type, const SharedPtr<RenderbufferObject>& renderbuffer);
            void Detach(FramebufferAttachmentType type);
            const FramebufferAttachmentObject& GetAttachment(FramebufferAttachmentType type) const;
            const FramebufferAttachmentObjectArray& GetAllAttachmentObjects() const;
            Bool CheckCompleteness() const;
            // aka. `buffer` as in glDrawBuffers/glReadBuffers
            void SetDrawBuffer(Uint index, FramebufferAttachmentType buffer);
            const FramebufferAttachmentArray& GetDrawBuffers() const;
            void SetReadBuffer(FramebufferAttachmentType buf);
            FramebufferAttachmentType GetReadBuffer() const { return m_readBuffer; }

            FramebufferAttachmentVersionArray GetAllFramebufferAttachmentVersions() const {
                return m_attachmentVersions;
            }

            Uint16 GetObjectVersion() const { return m_objectVersion; }

            Uint GetExternalIndex() const;
            Bool IsDefaultFramebuffer() const { return m_externalIndex == 0; }

        private:
            void BumpAttachmentVersion(FramebufferAttachmentType type);

            const Uint m_externalIndex = 0;
            FramebufferAttachmentObjectArray m_attachmentObjects;
            FramebufferAttachmentVersionArray m_attachmentVersions;

            FramebufferAttachmentArray m_drawBuffers; // Probably no versioning needed for this, just check equality
            FramebufferAttachmentType m_readBuffer = FramebufferAttachmentType::None;

            // This version will bump when draw/read buffer changes (by `glDrawBuffer(s)`/`glReadBuffer`)
            Uint16 m_objectVersion = 0;
        };

    } // namespace MG_State::GLState
} // namespace MobileGL
