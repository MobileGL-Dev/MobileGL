// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkClearManager.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "../VkIncludes.h"
#include "../VulkanRendererConfig.h"
#include "MG_State/GLState/FramebufferState/FramebufferObject.h"
#include "MG_Util/Math/VectorTypes.h"

#include <Includes.h>
#include <atomic>
#include <unordered_map>

namespace MobileGL::MG_Backend::DirectVulkan {
    struct ClearFramebufferPayload {
        FloatVec4 color;
        Float depth{};
        Uint32 stencil{};
    };

    // A colour clear reaches us from one of glClear/ClearBufferfv, ClearBufferiv or
    // ClearBufferuiv, and Vulkan reads VkClearColorValue's union according to the destination
    // image's format rather than converting between the members - a float written where an
    // integer format is expected is reinterpreted bit for bit, not rounded. Remember which entry
    // point supplied the value so the member written when the clear is materialized matches.
    enum class ClearColorEncoding : Uint8 { Float, Int, Uint };

    struct ClearAttachmentPayload {
        GLbitfield mask = 0;
        FloatVec4 color = FloatVec4(0.0f, 0.0f, 0.0f, 0.0f);
        ClearColorEncoding colorEncoding = ClearColorEncoding::Float;
        IntVec4 colorInt = IntVec4(0, 0, 0, 0);
        UintVec4 colorUint = UintVec4(0u, 0u, 0u, 0u);
        Float depth = 1.0f;
        Uint32 stencil = 0;
    };

    // Builds the clear value for `payload` in the union member its encoding calls for.
    // `formatLacksAlpha` applies GL's rule that a format without an alpha channel reads as one,
    // expressed in whichever type matches (GL 4.6 core 15.2.3).
    VkClearColorValue MakeVkClearColorValue(const ClearAttachmentPayload& payload, Bool formatLacksAlpha);

    // Applies that same rule in place, for the paths that have to bake it into the payload before
    // the destination is known.
    void ForceOpaqueClearAlpha(ClearAttachmentPayload& payload);

    struct PendingClearKey {
        MG_State::GLState::ITextureObject* texture = nullptr;
        Uint64 textureLifetimeId = 0;
        Uint32 mipLevel = 0;
        Uint32 baseArrayLayer = 0;
        Uint32 layerCount = 1;

        Bool operator==(const PendingClearKey& other) const {
            return texture == other.texture && textureLifetimeId == other.textureLifetimeId &&
                   mipLevel == other.mipLevel &&
                   baseArrayLayer == other.baseArrayLayer && layerCount == other.layerCount;
        }
    };

    struct TextureIdentity {
        MG_State::GLState::ITextureObject* texture = nullptr;
        Uint64 lifetimeId = 0;

        Bool operator==(const TextureIdentity& other) const {
            return texture == other.texture && lifetimeId == other.lifetimeId;
        }
    };

    struct PendingClearEntry {
        PendingClearKey key{};
        ClearAttachmentPayload payload{};
    };

    struct PendingClearKeyHash {
        SizeT operator()(const PendingClearKey& key) const {
            const SizeT textureHash = std::hash<MG_State::GLState::ITextureObject*>{}(key.texture);
            const SizeT textureLifetimeHash = std::hash<Uint64>{}(key.textureLifetimeId);
            const SizeT mipHash = std::hash<Uint32>{}(key.mipLevel);
            const SizeT layerHash = std::hash<Uint32>{}(key.baseArrayLayer);
            const SizeT layerCountHash = std::hash<Uint32>{}(key.layerCount);
            SizeT hash = textureHash;
            hash ^= textureLifetimeHash + 0x9e3779b9u + (hash << 6) + (hash >> 2);
            hash ^= mipHash + 0x9e3779b9u + (hash << 6) + (hash >> 2);
            hash ^= layerHash + 0x9e3779b9u + (hash << 6) + (hash >> 2);
            hash ^= layerCountHash + 0x9e3779b9u + (hash << 6) + (hash >> 2);
            return hash;
        }
    };

    struct TextureIdentityHash {
        SizeT operator()(const TextureIdentity& key) const {
            SizeT hash = std::hash<MG_State::GLState::ITextureObject*>{}(key.texture);
            hash ^= std::hash<Uint64>{}(key.lifetimeId) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
            return hash;
        }
    };

    class VkClearManager {
    public:
        static PendingClearKey MakePendingClearKey(const MG_State::GLState::FramebufferAttachmentObject& attachment);
        static PendingClearKey MakePendingClearKey(MG_State::GLState::ITextureObject* texture, Uint32 mipLevel = 0,
                                                   Uint32 baseArrayLayer = 0, Uint32 layerCount = 1);

        Bool Initialize();
        void Shutdown();

        void QueueClear(GLbitfield mask, const ClearFramebufferPayload& clearPayload, const MG_State::GLState::FramebufferObject& drawFbo);
        void QueueClear(
            const ClearAttachmentPayload& clearPayload,
            const SharedPtr<MG_State::GLState::ITextureObject>& texture);
        void QueueClear(const ClearAttachmentPayload& clearPayload,
                        const MG_State::GLState::FramebufferAttachmentObject& attachment);
        Bool HasPendingClear(MG_State::GLState::ITextureObject* texture);
        Bool HasPendingClear(const PendingClearKey& key);
        Bool HasPendingClear(const MG_State::GLState::FramebufferAttachmentObject& attachment);
        Bool GetPendingClear(const PendingClearKey& key, ClearAttachmentPayload& outPayload);
        Bool GetPendingClear(const PendingClearKey& key, ClearAttachmentPayload& outPayload,
                             SharedPtr<MG_State::GLState::ITextureObject>& outTexture);
        Bool GetPendingClear(const MG_State::GLState::FramebufferAttachmentObject& attachment,
                             ClearAttachmentPayload& outPayload);
        Bool GetPendingClears(MG_State::GLState::ITextureObject* texture, Vector<PendingClearEntry>& outEntries);
        void PopPendingClear(MG_State::GLState::ITextureObject* texture);
        void PopPendingClear(const PendingClearKey& key);
        void PopPendingClear(const MG_State::GLState::FramebufferAttachmentObject& attachment);
        SizeT CollectGarbage();
    private:
        static TextureIdentity MakeTextureIdentity(MG_State::GLState::ITextureObject* texture);
        static void MergeClearPayload(ClearAttachmentPayload& dst, const ClearAttachmentPayload& src);
        void ErasePendingClearsForTextureLocked(const TextureIdentity& identity);
        Bool LockTextureIdentityLocked(const TextureIdentity& identity,
                                      SharedPtr<MG_State::GLState::ITextureObject>& outTexture);
        Bool LockTextureLocked(const PendingClearKey& key,
                               SharedPtr<MG_State::GLState::ITextureObject>& outTexture);

        Uint8 m_gcCounter = 0;
    public:
        // Lock-free probe for the consecutive-draw fast path: any pending clear
        // forces the full SetupDraw path (which materializes/consumes it).
        Bool HasAnyPendingClears() const { return m_pendingCount.load(std::memory_order_relaxed) != 0; }

    private:
        mutable std::mutex m_mutex;
        // Lock-free mirror of m_pendingClears.size(), maintained under m_mutex
        // by every mutation. The per-draw probes (HasPendingClear/GetPending*)
        // read it before taking the lock: during draw batches the pending set
        // is almost always empty, so this turns several locked map probes per
        // draw into one relaxed load.
        std::atomic<Uint32> m_pendingCount{0};
        std::unordered_map<PendingClearKey, ClearAttachmentPayload, PendingClearKeyHash> m_pendingClears;
        std::unordered_map<TextureIdentity, WeakPtr<MG_State::GLState::ITextureObject>, TextureIdentityHash> m_aliveObjects;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
