// MobileGL - MobileGL/MG_State/GLState/ShareGroupState.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ShareGroupState.h"

#include "TextureState/TextureObject1D.h"
#include "TextureState/TextureObject2D.h"
#include "TextureState/TextureObject3D.h"
#include "TextureState/TextureObject2DCube.h"
#include "TextureState/TextureObjectBuffer.h"
#include "TextureState/TextureObjectStubs.h"
#include "TextureState/TextureObjectView.h"

namespace MobileGL::MG_State::GLState {
    SharedPtr<ITextureObject> MakeTextureObjectForTarget(Uint index, TextureTarget target) {
        switch (target) {
        case TextureTarget::Texture1D:
            return MakeShared<TextureObject1D>(index);
        case TextureTarget::TextureCubeMap:
            return MakeShared<TextureObject2DCube>(index);
        case TextureTarget::Texture2D:
            return MakeShared<TextureObject2D>(index);
        case TextureTarget::Texture3D:
            return MakeShared<TextureObject3D>(index);
        case TextureTarget::TextureBuffer:
            return MakeShared<TextureObjectBuffer>(index);

            // These texture types are still stubbed:
        case TextureTarget::TextureRectangle:
            return MakeShared<TextureObjectRectangle>(index);
        case TextureTarget::Texture2DMultisample:
            return MakeShared<TextureObject2DMultisample>(index);
        case TextureTarget::Texture1DArray:
            return MakeShared<TextureObject1DArray>(index);
        case TextureTarget::Texture2DArray:
            return MakeShared<TextureObject2DArray>(index);
        case TextureTarget::TextureCubeMapArray:
            return MakeShared<TextureObjectCubeMapArray>(index);
        case TextureTarget::Texture2DMultisampleArray:
            return MakeShared<TextureObject2DMultisampleArray>(index);
        case TextureTarget::External:
            return MakeShared<TextureObjectExternal>(index);
        default:
            MOBILEGL_ASSERT(false, "Unimplemented texture type when creating texture object!: %d", (int)target);
            return nullptr;
        }
    }

    // Buffer
    const SharedPtr<BufferObject>& ShareGroupState::GetBufferObject(Uint index) {
        auto it = m_bufferObjects.find(index);
        if (it != m_bufferObjects.end()) {
            return it->second;
        }
        static SharedPtr<BufferObject> nullBufferObject = nullptr;
        return nullBufferObject;
    }

    void ShareGroupState::GenerateBufferNames(Uint number, Vector<Uint>& buffers) {
        buffers.resize(number);
        m_bufferNames.Generate(number, buffers.data());
    }

    const SharedPtr<BufferObject>& ShareGroupState::CreateBufferObject(Uint index) {
        auto& bufferObj = m_bufferObjects[index];
        if (!bufferObj) {
            bufferObj = MakeShared<BufferObject>(index);
        }
        return bufferObj;
    }

    void ShareGroupState::MarkBufferObjectForDeletion(Uint index) {
        if (m_bufferNames.IsValid(index)) {
            m_bufferObjects.erase(index);
            m_bufferNames.Delete(index);
        }
    }

    Bool ShareGroupState::ValidateBufferName(Uint index) const {
        return m_bufferNames.IsValid(index);
    }

    Bool ShareGroupState::ValidateBufferObject(Uint index) const {
        return m_bufferObjects.find(index) != m_bufferObjects.end();
    }

    // Texture
    void ShareGroupState::GenerateTextureNames(Uint number, Vector<Uint>& textures) {
        textures.resize(number);
        m_textureNames.Generate(number, textures.data());
    }

    const SharedPtr<ITextureObject>& ShareGroupState::GetTextureObject(Uint index) {
        auto it = m_textureObjects.find(index);
        if (it != m_textureObjects.end()) {
            return it->second;
        }
        static SharedPtr<ITextureObject> nullTextureObject = nullptr;
        return nullTextureObject;
    }

    const SharedPtr<ITextureObject>& ShareGroupState::CreateTextureObject(Uint index, TextureTarget target) {
        auto& textureObject = m_textureObjects[index];
        textureObject = MakeTextureObjectForTarget(index, target);
        if (!textureObject) {
            static SharedPtr<ITextureObject> nullTextureObject = nullptr;
            return nullTextureObject;
        }
        return textureObject;
    }

    const SharedPtr<ITextureObject>& ShareGroupState::CreateTextureViewObject(
        Uint index, TextureTarget target, const SharedPtr<ITextureObject>& storageOwner, Uint minLevel,
        Uint numLevels, Uint minLayer, Uint numLayers) {
        MOBILEGL_ASSERT(storageOwner != nullptr, "CreateTextureViewObject: storage owner is null");
        auto& textureObject = m_textureObjects[index];
        textureObject = MakeShared<TextureObjectView>(index, target, storageOwner, minLevel, numLevels, minLayer,
                                                      numLayers);
        return textureObject;
    }

    void ShareGroupState::MarkTextureObjectForDeletion(Uint index, Bool keepUnboundReservation) {
        if (m_textureNames.IsValid(index)) {
            auto it = m_textureObjects.find(index);
            if (it != m_textureObjects.end()) {
                m_textureObjects.erase(it);
                m_textureNames.Delete(index);
            } else if (!keepUnboundReservation) {
                // GL 3.3 core 3.8.1 makes a deleted name unused again even when GenTextures only
                // reserved it and no bind ever instantiated an object (so a later bind of it must
                // fail), and the reservation has to return to the free list.
                m_textureNames.Delete(index);
            }
            // Relaxed semantics: legacy apps may delete a generated name before its first
            // bind, then bind and populate that same name. Keep such a reservation alive there;
            // a real texture object reaching deletion still releases its index above.
        }
    }

    Bool ShareGroupState::ValidateTextureName(Uint index) const {
        return m_textureNames.IsValid(index);
    }

    Bool ShareGroupState::ValidateTextureObject(Uint index) const {
        return m_textureObjects.find(index) != m_textureObjects.end();
    }

    // Renderbuffer
    const SharedPtr<RenderbufferObject>& ShareGroupState::GetRenderbufferObject(Uint index) {
        auto it = m_renderbufferObjects.find(index);
        if (it != m_renderbufferObjects.end()) {
            return it->second;
        }
        static SharedPtr<RenderbufferObject> nullRenderbufferObject = nullptr;
        return nullRenderbufferObject;
    }

    void ShareGroupState::GenerateRenderbufferNames(Uint number, Vector<Uint>& renderbuffers) {
        renderbuffers.resize(number);
        m_renderbufferNames.Generate(number, renderbuffers.data());
    }

    const SharedPtr<RenderbufferObject>& ShareGroupState::CreateRenderbufferObject(Uint index) {
        auto& bufferObject = m_renderbufferObjects[index];
        if (!bufferObject) {
            bufferObject = MakeShared<RenderbufferObject>(index);
        }
        return bufferObject;
    }

    void ShareGroupState::MarkRenderbufferObjectForDeletion(Uint index) {
        if (m_renderbufferNames.IsValid(index)) {
            m_renderbufferObjects.erase(index);
            m_renderbufferNames.Delete(index);
        }
    }

    Bool ShareGroupState::ValidateRenderbufferName(Uint index) const {
        return m_renderbufferNames.IsValid(index);
    }

    Bool ShareGroupState::ValidateRenderbufferObject(Uint index) const {
        return m_renderbufferObjects.find(index) != m_renderbufferObjects.end();
    }
    UniquePtr<BufferObject> ShareGroupState::TakeClientArrayBuffer() {
        const std::lock_guard<std::mutex> lock(m_clientArrayBuffersMutex);
        if (m_clientArrayBuffers.empty()) return nullptr;
        UniquePtr<BufferObject> buffer = std::move(m_clientArrayBuffers.back());
        m_clientArrayBuffers.pop_back();
        return buffer;
    }

    void ShareGroupState::ReturnClientArrayBuffer(UniquePtr<BufferObject> buffer) {
        // Enough for a draw's every attribute plus its indices; more is never in use at once.
        constexpr SizeT kKeptClientArrayBuffers = 48;
        if (!buffer) return;
        {
            const std::lock_guard<std::mutex> lock(m_clientArrayBuffersMutex);
            if (m_clientArrayBuffers.size() < kKeptClientArrayBuffers) {
                m_clientArrayBuffers.push_back(std::move(buffer));
                return;
            }
        }
        // A surplus buffer is destroyed outside the lock (its destructor emits).
    }

} // namespace MobileGL::MG_State::GLState
