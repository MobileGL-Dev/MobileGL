// MobileGL - MobileGL/MG_State/GLState/FramebufferState/FramebufferObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "FramebufferObject.h"
#include "MG_Util/Types.h"

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            // FramebufferAttachmentObject
            FramebufferAttachmentObject::FramebufferAttachmentObject(
                SharedPtr<MG_State::GLState::ITextureObject> texture, Int level)
                : m_texture(texture), m_textureLevel(level) {}
            FramebufferAttachmentObject::FramebufferAttachmentObject(SharedPtr<RenderbufferObject> renderbuffer)
                : m_renderbuffer(renderbuffer) {}
            FramebufferAttachmentObject::FramebufferAttachmentObject(Bool IsValid)
                : m_texture(nullptr), m_renderbuffer(nullptr) {
                m_isValid = IsValid;
            }

            Bool FramebufferAttachmentObject::IsTexture() const {
                return m_texture != nullptr;
            }

            Bool FramebufferAttachmentObject::IsRenderbuffer() const {
                return m_renderbuffer != nullptr;
            }

            Bool FramebufferAttachmentObject::IsEmpty() const {
                return m_texture == nullptr && m_renderbuffer == nullptr;
            }

            SharedPtr<MG_State::GLState::ITextureObject> FramebufferAttachmentObject::GetTexture() const {
                return m_texture;
            }

            SharedPtr<RenderbufferObject> FramebufferAttachmentObject::GetRenderbuffer() const {
                return m_renderbuffer;
            }

            Int FramebufferAttachmentObject::GetTextureLevel() const {
                return m_textureLevel;
            }

            Bool FramebufferAttachmentObject::IsComplete() const {
                if (IsTexture()) {
                    Bool complete = m_texture->IsComplete();
                    return complete;
                } else if (IsRenderbuffer()) {
                    Bool complete = m_renderbuffer->IsAllocated();
                    return complete;
                }

                return false;
            }

            IntVec3 FramebufferAttachmentObject::GetSize() const {
                if (IsTexture()) {
                    // TODO: get correct upload target
                    MOBILEGL_ASSERT(nullptr != dynamic_cast<MG_State::GLState::TextureObjectMipmap*>(m_texture.get()),
                                    "Texture object here should always be an object with mipmap");
                    auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(m_texture.get());
                    return textureMipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture2D, m_textureLevel);
                } else if (IsRenderbuffer()) {
                    return IntVec3(m_renderbuffer->GetWidth(), m_renderbuffer->GetHeight(), 1);
                }
                return {0, 0, 0};
            }

            Bool FramebufferAttachmentObject::IsValid() const {
                return m_isValid;
            }

            // FramebufferObject
            FramebufferObject::FramebufferObject(Uint externalIndex) : m_externalIndex(externalIndex) {
                m_attachmentObjects.fill(FramebufferAttachmentObject(false));
                m_drawBuffers.fill(FramebufferAttachmentType::None);
                m_drawBuffers[0] = FramebufferAttachmentType::Color0;
                m_attachmentVersions.fill(0);
            }

            void FramebufferObject::AttachTexture(FramebufferAttachmentType type, SharedPtr<ITextureObject> texture,
                                                  int level) {
                m_attachmentObjects[static_cast<SizeT>(type)] = FramebufferAttachmentObject(std::move(texture), level);
                BumpAttachmentVersion(type);
            }

            void FramebufferObject::AttachRenderbuffer(FramebufferAttachmentType type,
                                                       std::shared_ptr<RenderbufferObject> renderbuffer) {
                m_attachmentObjects[static_cast<SizeT>(type)] = FramebufferAttachmentObject(renderbuffer);
                BumpAttachmentVersion(type);
            }

            void FramebufferObject::Detach(FramebufferAttachmentType type) {
                m_attachmentObjects[static_cast<SizeT>(type)] = FramebufferAttachmentObject(false);
                BumpAttachmentVersion(type);
            }

            const FramebufferAttachmentObject& FramebufferObject::GetAttachment(FramebufferAttachmentType type) const {
                return m_attachmentObjects[static_cast<SizeT>(type)];
            }

            const FramebufferObject::FramebufferAttachmentObjectArray& FramebufferObject::GetAllAttachmentObjects()
                const {
                return m_attachmentObjects;
            }

            Bool FramebufferObject::CheckCompleteness() const {
                if (m_attachmentObjects.empty()) {
                    return false;
                }

                Int width = -1, height = -1;
                Int validAttachmentCount = 0;
                for (SizeT i = 0; i < m_attachmentObjects.size(); ++i) {
                    if (!m_attachmentObjects[i].IsValid()) continue;

                    ++validAttachmentCount;
                    const auto& attachment = m_attachmentObjects[i];
                    auto attachmentSize = attachment.GetSize();
                    Int w = attachmentSize.x();
                    Int h = attachmentSize.y();

                    if (width == -1) {
                        width = w;
                        height = h;
                    } else if (width != w || height != h) {
                        return false;
                    }

                    if (!attachment.IsComplete()) {
                        return false;
                    }
                }

                if (validAttachmentCount == 0) return false;
                return true;
            }

            void FramebufferObject::SetDrawBuffer(Uint index, FramebufferAttachmentType buffer) {
                if (m_drawBuffers[index] == buffer) return;
                m_drawBuffers[index] = buffer;
                BumpAttachmentVersion(buffer);
            }

            const FramebufferObject::FramebufferAttachmentArray& FramebufferObject::GetDrawBuffers() const {
                return m_drawBuffers;
            }

            Uint FramebufferObject::GetExternalIndex() const {
                return m_externalIndex;
            }

            void FramebufferObject::BumpAttachmentVersion(FramebufferAttachmentType type) {
                ++m_attachmentVersions[static_cast<SizeT>(type)];
                ++m_objectVersion;
            }
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
