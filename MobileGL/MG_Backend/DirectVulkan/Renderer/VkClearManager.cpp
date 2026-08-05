// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkClearManager.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VkClearManager.h"

#include "MG_State/GLState/Core.h"
#include "MG_Util/Converters/MGToStr/FramebufferEnumConverter.h"
#include "MG_Util/Converters/MGToStr/TextureEnumConverter.h"

#include <algorithm>
#include <cmath>

namespace MobileGL::MG_Backend::DirectVulkan {
    static Bool IsCubeMapFaceUploadTarget(TextureUploadTarget target) {
        return target >= TextureUploadTarget::CubeMapPositiveX &&
               target <= TextureUploadTarget::CubeMapNegativeZ;
    }

    VkClearColorValue MakeVkClearColorValue(const ClearAttachmentPayload& payload, Bool formatLacksAlpha) {
        VkClearColorValue clearValue{};
        switch (payload.colorEncoding) {
        case ClearColorEncoding::Int:
            clearValue.int32[0] = payload.colorInt.x();
            clearValue.int32[1] = payload.colorInt.y();
            clearValue.int32[2] = payload.colorInt.z();
            clearValue.int32[3] = formatLacksAlpha ? 1 : payload.colorInt.w();
            break;
        case ClearColorEncoding::Uint:
            clearValue.uint32[0] = payload.colorUint.x();
            clearValue.uint32[1] = payload.colorUint.y();
            clearValue.uint32[2] = payload.colorUint.z();
            clearValue.uint32[3] = formatLacksAlpha ? 1u : payload.colorUint.w();
            break;
        case ClearColorEncoding::Float:
            clearValue.float32[0] = payload.color.x();
            clearValue.float32[1] = payload.color.y();
            clearValue.float32[2] = payload.color.z();
            clearValue.float32[3] = formatLacksAlpha ? 1.0f : payload.color.w();
            break;
        }
        return clearValue;
    }

    void PreCompensateSrgbClearColor(ClearAttachmentPayload& payload, VkFormat destinationFormat) {
        if (payload.colorEncoding != ClearColorEncoding::Float) return;
        // With GL_FRAMEBUFFER_SRGB enabled GL performs the encoding itself, so the driver doing it
        // is exactly right and there is nothing to undo.
        if (MG_State::pGLContext->IsCapabilityEnabled(MobileGL::CapabilityInput::FramebufferSrgb)) return;
        if (ResolveSrgbAttachmentWriteFormat(destinationFormat, false) == destinationFormat) return;

        // sRGB -> linear (GL 4.6 core 8.24), applied to the colour channels only: alpha is stored
        // linearly in an sRGB format and must pass through untouched.
        const auto toLinear = [](Float encoded) {
            const Float value = std::clamp(encoded, 0.0f, 1.0f);
            return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
        };
        payload.color = FloatVec4(toLinear(payload.color.x()), toLinear(payload.color.y()),
                                  toLinear(payload.color.z()), payload.color.w());
    }

    void ForceOpaqueClearAlpha(ClearAttachmentPayload& payload) {
        switch (payload.colorEncoding) {
        case ClearColorEncoding::Int:
            payload.colorInt = IntVec4(payload.colorInt.x(), payload.colorInt.y(), payload.colorInt.z(), 1);
            break;
        case ClearColorEncoding::Uint:
            payload.colorUint = UintVec4(payload.colorUint.x(), payload.colorUint.y(), payload.colorUint.z(), 1u);
            break;
        case ClearColorEncoding::Float:
            payload.color = FloatVec4(payload.color.x(), payload.color.y(), payload.color.z(), 1.0f);
            break;
        }
    }

    static Bool PendingClearMatchesTextureIdentity(const PendingClearKey& key, const TextureIdentity& identity) {
        return key.texture == identity.texture && key.textureLifetimeId == identity.lifetimeId;
    }

    static Uint32 ResolveAttachmentBaseArrayLayer(TextureUploadTarget target) {
        if (!IsCubeMapFaceUploadTarget(target)) {
            return 0;
        }
        return static_cast<Uint32>(target) - static_cast<Uint32>(TextureUploadTarget::CubeMapPositiveX);
    }

    static Uint32 ResolveAttachmentBaseArrayLayer(
        const MG_State::GLState::FramebufferAttachmentObject& attachment) {
        if (attachment.IsLayered()) {
            return 0;
        }
        const TextureUploadTarget uploadTarget = attachment.GetTextureUploadTarget();
        if (!IsCubeMapFaceUploadTarget(uploadTarget)) {
            return static_cast<Uint32>(std::max(attachment.GetTextureLayer(), 0));
        }
        return ResolveAttachmentBaseArrayLayer(uploadTarget);
    }

    static Uint32 ResolveAttachmentLayerCount(
        const MG_State::GLState::FramebufferAttachmentObject& attachment) {
        if (attachment.IsLayered()) {
            return static_cast<Uint32>(std::max(attachment.GetSize().z(), 1));
        }
        return 1u;
    }

    static const MG_State::GLState::FramebufferAttachmentObject* GetClearableAttachment(
        const MG_State::GLState::FramebufferObject& drawFbo, FramebufferAttachmentType attachmentType) {
        if (attachmentType == FramebufferAttachmentType::None) {
            return nullptr;
        }

        const auto& attachment = drawFbo.GetAttachment(attachmentType);
        if (!attachment.IsTexture() || attachment.IsRenderbuffer()) {
            return nullptr;
        }

        return &attachment;
    }

    PendingClearKey VkClearManager::MakePendingClearKey(MG_State::GLState::ITextureObject* texture, Uint32 mipLevel,
                                                        Uint32 baseArrayLayer, Uint32 layerCount) {
        return PendingClearKey {
            .texture = texture,
            .textureLifetimeId = texture ? texture->GetLifetimeId() : 0,
            .mipLevel = mipLevel,
            .baseArrayLayer = baseArrayLayer,
            .layerCount = layerCount,
        };
    }

    PendingClearKey VkClearManager::MakePendingClearKey(
        const MG_State::GLState::FramebufferAttachmentObject& attachment) {
        MOBILEGL_ASSERT(attachment.IsTexture() && !attachment.IsRenderbuffer(),
                        "MakePendingClearKey requires a texture framebuffer attachment");
        auto* texture = attachment.GetTexture().get();
        MOBILEGL_ASSERT(texture != nullptr, "MakePendingClearKey: texture attachment resolved to null");
        const Uint32 mipLevel = static_cast<Uint32>(std::max(attachment.GetTextureLevel(), 0));
        const Uint32 baseArrayLayer = ResolveAttachmentBaseArrayLayer(attachment);
        const Uint32 layerCount = ResolveAttachmentLayerCount(attachment);
        return MakePendingClearKey(texture, mipLevel, baseArrayLayer, layerCount);
    }

    Bool VkClearManager::Initialize() {
        return true;
    }

    void VkClearManager::Shutdown() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingClears.clear();
        m_aliveObjects.clear();
        m_pendingCount.store(static_cast<Uint32>(m_pendingClears.size()), std::memory_order_relaxed);
    }

    TextureIdentity VkClearManager::MakeTextureIdentity(MG_State::GLState::ITextureObject* texture) {
        return TextureIdentity {
            .texture = texture,
            .lifetimeId = texture ? texture->GetLifetimeId() : 0,
        };
    }

    void VkClearManager::MergeClearPayload(ClearAttachmentPayload& dst, const ClearAttachmentPayload& src) {
        dst.mask |= src.mask;
        if ((src.mask & GL_COLOR_BUFFER_BIT) != 0) {
            dst.color = src.color;
        }
        if ((src.mask & GL_DEPTH_BUFFER_BIT) != 0) {
            dst.depth = src.depth;
        }
        if ((src.mask & GL_STENCIL_BUFFER_BIT) != 0) {
            dst.stencil = src.stencil;
        }
    }

    void VkClearManager::ErasePendingClearsForTextureLocked(const TextureIdentity& identity) {
        Vector<PendingClearKey> keysToErase;
        keysToErase.reserve(m_pendingClears.size());
        for (auto it = m_pendingClears.begin(); it != m_pendingClears.end(); ++it) {
            if (PendingClearMatchesTextureIdentity(it->first, identity)) {
                keysToErase.emplace_back(it->first);
            }
        }
        for (const auto& key : keysToErase) {
            m_pendingClears.erase(key);
        }
        m_aliveObjects.erase(identity);
        m_pendingCount.store(static_cast<Uint32>(m_pendingClears.size()), std::memory_order_relaxed);
    }

    Bool VkClearManager::LockTextureIdentityLocked(const TextureIdentity& identity,
                                                   SharedPtr<MG_State::GLState::ITextureObject>& outTexture) {
        outTexture.reset();
        if (identity.texture == nullptr) {
            return false;
        }

        auto aliveIt = m_aliveObjects.find(identity);
        if (aliveIt == m_aliveObjects.end()) {
            ErasePendingClearsForTextureLocked(identity);
            return false;
        }

        outTexture = aliveIt->second.lock();
        if (!outTexture || outTexture.get() != identity.texture || outTexture->GetLifetimeId() != identity.lifetimeId) {
            ErasePendingClearsForTextureLocked(identity);
            outTexture.reset();
            return false;
        }

        return true;
    }

    Bool VkClearManager::LockTextureLocked(const PendingClearKey& key,
                                           SharedPtr<MG_State::GLState::ITextureObject>& outTexture) {
        return LockTextureIdentityLocked(TextureIdentity{
            .texture = key.texture,
            .lifetimeId = key.textureLifetimeId,
        }, outTexture);
    }

    void VkClearManager::QueueClear(GLbitfield mask, const ClearFramebufferPayload& clearPayload,
                                    const MG_State::GLState::FramebufferObject& drawFbo) {
        if (mask & GL_COLOR_BUFFER_BIT) {
            auto& drawbufs = drawFbo.GetDrawBuffers();
            // This should automatically work on default & offscreen FBO
            for (auto drawbuf: drawbufs) {
                const auto* attachment = GetClearableAttachment(drawFbo, drawbuf);
                if (!attachment) {
                    continue;
                }

                QueueClear({
                    .mask = GL_COLOR_BUFFER_BIT,
                    .color = clearPayload.color
                }, *attachment);

                MGLOG_D("%s: %s (texture %d) - color = (%.2f, %.2f, %.2f, %.2f)", __func__,
                    MG_Util::ConvertFramebufferAttachmentTypeToString(drawbuf).c_str(),
                    attachment->GetTexture()->GetExternalIndex(),
                    clearPayload.color[0], clearPayload.color[1], clearPayload.color[2], clearPayload.color[3]);
            }
        }

        if (mask & GL_DEPTH_BUFFER_BIT) {
            const auto* attachment = GetClearableAttachment(drawFbo, FramebufferAttachmentType::Depth);
            if (attachment) {
                QueueClear({
                    .mask = GL_DEPTH_BUFFER_BIT,
                    .depth = clearPayload.depth,
                }, *attachment);

                MGLOG_D("%s: Depth (texture %d) - depth = (%.2f)", __func__,
                    attachment->GetTexture()->GetExternalIndex(), clearPayload.depth);
            }
        }

        if (mask & GL_STENCIL_BUFFER_BIT) {
            const auto* attachment = GetClearableAttachment(drawFbo, FramebufferAttachmentType::Stencil);
            if (attachment) {
                QueueClear({
                    .mask = GL_STENCIL_BUFFER_BIT,
                    .stencil = clearPayload.stencil,
                }, *attachment);

                MGLOG_D("%s: Stencil (texture %d) - stencil = (%u)", __func__,
                    attachment->GetTexture()->GetExternalIndex(), clearPayload.stencil);
            }
        }
    }

    void VkClearManager::QueueClear(const ClearAttachmentPayload& clearPayload,
                                    const SharedPtr<MG_State::GLState::ITextureObject>& texture) {
        if (clearPayload.mask == 0 || !texture) {
            return;
        }

        const PendingClearKey key = MakePendingClearKey(texture.get());
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_aliveObjects[MakeTextureIdentity(texture.get())] = texture;
        auto& pending = m_pendingClears[key];
        MergeClearPayload(pending, clearPayload);
        m_pendingCount.store(static_cast<Uint32>(m_pendingClears.size()), std::memory_order_relaxed);
    }

    void VkClearManager::QueueClear(const ClearAttachmentPayload& clearPayload,
                                    const MG_State::GLState::FramebufferAttachmentObject& attachment) {
        if (clearPayload.mask == 0 || !attachment.IsTexture() || attachment.IsRenderbuffer()) {
            return;
        }
        const auto texture = attachment.GetTexture();
        if (!texture) {
            return;
        }

        const PendingClearKey key = MakePendingClearKey(attachment);
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_aliveObjects[MakeTextureIdentity(texture.get())] = texture;
        auto& pending = m_pendingClears[key];
        MergeClearPayload(pending, clearPayload);
        m_pendingCount.store(static_cast<Uint32>(m_pendingClears.size()), std::memory_order_relaxed);
    }

    Bool VkClearManager::HasPendingClear(MG_State::GLState::ITextureObject* texture) {
        if (texture == nullptr) {
            return false;
        }

        if (m_pendingCount.load(std::memory_order_relaxed) == 0) {
            return false;  // per-draw hot path: nothing pending anywhere
        }

        const Uint64 lifetimeId = texture->GetLifetimeId();
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_pendingClears.begin(); it != m_pendingClears.end(); ++it) {
            if (it->first.texture == texture && it->first.textureLifetimeId == lifetimeId) {
                SharedPtr<MG_State::GLState::ITextureObject> liveTexture;
                return LockTextureLocked(it->first, liveTexture);
            }
        }
        return false;
    }

    Bool VkClearManager::HasPendingClear(const PendingClearKey& key) {
        if (key.texture == nullptr) {
            return false;
        }
        if (m_pendingCount.load(std::memory_order_relaxed) == 0) {
            return false;  // per-draw hot path: nothing pending anywhere
        }

        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pendingClears.find(key) == m_pendingClears.end()) {
            return false;
        }

        SharedPtr<MG_State::GLState::ITextureObject> liveTexture;
        return LockTextureLocked(key, liveTexture);
    }

    Bool VkClearManager::HasPendingClear(const MG_State::GLState::FramebufferAttachmentObject& attachment) {
        if (!attachment.IsTexture() || attachment.IsRenderbuffer() || !attachment.GetTexture()) {
            return false;
        }
        return HasPendingClear(MakePendingClearKey(attachment));
    }

    Bool VkClearManager::GetPendingClear(const PendingClearKey& key, ClearAttachmentPayload& outPayload) {
        SharedPtr<MG_State::GLState::ITextureObject> liveTexture;
        return GetPendingClear(key, outPayload, liveTexture);
    }

    Bool VkClearManager::GetPendingClear(const PendingClearKey& key, ClearAttachmentPayload& outPayload,
                                         SharedPtr<MG_State::GLState::ITextureObject>& outTexture) {
        if (key.texture == nullptr) {
            return false;
        }
        if (m_pendingCount.load(std::memory_order_relaxed) == 0) {
            return false;  // per-draw hot path: nothing pending anywhere
        }

        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!LockTextureLocked(key, outTexture)) {
            return false;
        }
        auto it = m_pendingClears.find(key);
        if (it == m_pendingClears.end()) {
            outTexture.reset();
            return false;
        }

        outPayload = it->second;
        MGLOG_D("%s: Got pending clear for texture@%p lifetime=%llu, mip=%u layer=%u count=%u mask=0x%x clear value: color = (%.2f, %.2f, %.2f, %.2f), depth = (%.2f), stencil = (%u)", __func__,
            static_cast<void*>(key.texture),
            static_cast<unsigned long long>(key.textureLifetimeId),
            key.mipLevel, key.baseArrayLayer, key.layerCount,
            static_cast<Uint32>(outPayload.mask),
            outPayload.color[0], outPayload.color[1], outPayload.color[2], outPayload.color[3],
            outPayload.depth,
            outPayload.stencil);
        return true;
    }

    Bool VkClearManager::GetPendingClear(const MG_State::GLState::FramebufferAttachmentObject& attachment,
                                         ClearAttachmentPayload& outPayload) {
        if (!attachment.IsTexture() || attachment.IsRenderbuffer() || !attachment.GetTexture()) {
            MGLOG_D("%s: Failed getting pending clear for non-texture framebuffer attachment", __func__);
            return false;
        }
        return GetPendingClear(MakePendingClearKey(attachment), outPayload);
    }

    Bool VkClearManager::GetPendingClears(MG_State::GLState::ITextureObject* texture,
                                          Vector<PendingClearEntry>& outEntries) {
        outEntries.clear();
        if (texture == nullptr) {
            return false;
        }
        if (m_pendingCount.load(std::memory_order_relaxed) == 0) {
            return false;  // per-draw hot path: nothing pending anywhere
        }

        const Uint64 lifetimeId = texture->GetLifetimeId();
        const std::lock_guard<std::mutex> lock(m_mutex);
        SharedPtr<MG_State::GLState::ITextureObject> liveTexture;
        if (!LockTextureIdentityLocked(MakeTextureIdentity(texture), liveTexture)) {
            return false;
        }
        for (auto it = m_pendingClears.begin(); it != m_pendingClears.end(); ++it) {
            if (it->first.texture == texture && it->first.textureLifetimeId == lifetimeId) {
                outEntries.emplace_back(PendingClearEntry{.key = it->first, .payload = it->second});
            }
        }
        return !outEntries.empty();
    }

    void VkClearManager::PopPendingClear(MG_State::GLState::ITextureObject* texture) {
        if (texture == nullptr) {
            return;
        }

        if (m_pendingCount.load(std::memory_order_relaxed) == 0) {
            return;  // per-draw hot path: nothing pending anywhere
        }
        const TextureIdentity identity = MakeTextureIdentity(texture);
        MGLOG_D("%s: Pop all pending clears for texture %d", __func__, texture->GetExternalIndex());
        const std::lock_guard<std::mutex> lock(m_mutex);
        ErasePendingClearsForTextureLocked(identity);
    }

    void VkClearManager::PopPendingClear(const PendingClearKey& key) {
        if (key.texture == nullptr) {
            return;
        }

        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_pendingClears.find(key);
            if (it != m_pendingClears.end()) {
                m_pendingClears.erase(it);
            m_pendingCount.store(static_cast<Uint32>(m_pendingClears.size()), std::memory_order_relaxed);
            }
        }

        MGLOG_D("%s: Pop pending clear for texture@%p lifetime=%llu mip=%u layer=%u count=%u", __func__,
                static_cast<void*>(key.texture), static_cast<unsigned long long>(key.textureLifetimeId),
                key.mipLevel, key.baseArrayLayer, key.layerCount);
    }

    void VkClearManager::PopPendingClear(const MG_State::GLState::FramebufferAttachmentObject& attachment) {
        if (!attachment.IsTexture() || attachment.IsRenderbuffer() || !attachment.GetTexture()) {
            return;
        }
        PopPendingClear(MakePendingClearKey(attachment));
    }

    SizeT VkClearManager::CollectGarbage() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_gcCounter++;
        if (m_gcCounter != 0) {
            return 0;
        }

        Vector<TextureIdentity> expiredTextures;
        expiredTextures.reserve(m_aliveObjects.size());
        for (auto it = m_aliveObjects.begin(); it != m_aliveObjects.end(); ++it) {
            if (it->second.expired()) {
                expiredTextures.emplace_back(it->first);
            }
        }
        if (expiredTextures.empty()) {
            return 0;
        }

        for (const auto& identity : expiredTextures) {
            ErasePendingClearsForTextureLocked(identity);
        }
        return expiredTextures.size();
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
