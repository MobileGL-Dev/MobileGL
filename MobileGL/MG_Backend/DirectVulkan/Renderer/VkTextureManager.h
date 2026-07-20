// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkTextureManager.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "../VkIncludes.h"
#include <Includes.h>
#include <MG_State/GLState/TextureState/TextureObject.h>
#include <vk_mem_alloc.h>
#include <unordered_map>
#include <unordered_set>

namespace MobileGL::MG_State::GLState {
class ITextureObject;
}

namespace MobileGL::MG_Backend::DirectVulkan {
enum class SamplerNumericDomain : Uint8;

class VkTextureManager {
public:
    // Monotonic epoch bumped whenever a texture VkImage is (re)created. The render-pass
    // manager keys its per-draw fast path on this so an attachment's image recreation
    // invalidates the cached render pass (dirty-flag tracking; portable to Vulkan 1.1).
    Uint64 GetTextureImageEpoch() const { return m_textureImageEpoch; }

    struct TextureIdentity {
        MG_State::GLState::ITextureObject* texture = nullptr;
        Uint64 lifetimeId = 0;

        Bool operator==(const TextureIdentity& other) const {
            return texture == other.texture && lifetimeId == other.lifetimeId;
        }
    };

    struct TextureIdentityHash {
        SizeT operator()(const TextureIdentity& key) const {
            SizeT hash = std::hash<MG_State::GLState::ITextureObject*>{}(key.texture);
            hash ^= std::hash<Uint64>{}(key.lifetimeId) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
            return hash;
        }
    };

    struct InitInfo {
        VkDevice device = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VmaAllocator allocator = nullptr;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkQueue graphicsQueue = VK_NULL_HANDLE;
        Uint32 frameCount = 0;
    };

    struct TextureResource {
        struct AttachmentViewKey {
            Uint32 mipLevel = 0;
            Uint32 baseArrayLayer = 0;
            Uint32 layerCount = 1;
            VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;

            Bool operator==(const AttachmentViewKey& other) const {
                return mipLevel == other.mipLevel &&
                       baseArrayLayer == other.baseArrayLayer &&
                       layerCount == other.layerCount &&
                       viewType == other.viewType;
            }
        };

        struct AttachmentViewKeyHash {
            SizeT operator()(const AttachmentViewKey& key) const {
                SizeT hash = std::hash<Uint32>{}(key.mipLevel);
                hash ^= std::hash<Uint32>{}(key.baseArrayLayer) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<Uint32>{}(key.layerCount) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<Uint32>{}(static_cast<Uint32>(key.viewType)) +
                        0x9e3779b9u + (hash << 6) + (hash >> 2);
                return hash;
            }
        };

        struct StorageImageViewKey {
            Uint32 mipLevel = 0;
            Uint32 baseArrayLayer = 0;
            Uint32 layerCount = 1;
            VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
            VkFormat format = VK_FORMAT_UNDEFINED;

            Bool operator==(const StorageImageViewKey& other) const {
                return mipLevel == other.mipLevel &&
                       baseArrayLayer == other.baseArrayLayer &&
                       layerCount == other.layerCount &&
                       viewType == other.viewType &&
                       format == other.format;
            }
        };

        struct SampledImageViewKey {
            Uint32 baseMipLevel = 0;
            Uint32 levelCount = 1;
            VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
            VkFormat format = VK_FORMAT_UNDEFINED;

            Bool operator==(const SampledImageViewKey& other) const {
                return baseMipLevel == other.baseMipLevel &&
                       levelCount == other.levelCount &&
                       viewType == other.viewType &&
                       format == other.format;
            }
        };

        struct SampledImageViewKeyHash {
            SizeT operator()(const SampledImageViewKey& key) const {
                SizeT hash = std::hash<Uint32>{}(key.baseMipLevel);
                hash ^= std::hash<Uint32>{}(key.levelCount) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<Uint32>{}(static_cast<Uint32>(key.viewType)) +
                        0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<Uint32>{}(static_cast<Uint32>(key.format)) +
                        0x9e3779b9u + (hash << 6) + (hash >> 2);
                return hash;
            }
        };

        struct StorageImageViewKeyHash {
            SizeT operator()(const StorageImageViewKey& key) const {
                SizeT hash = std::hash<Uint32>{}(key.mipLevel);
                hash ^= std::hash<Uint32>{}(key.baseArrayLayer) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<Uint32>{}(key.layerCount) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<Uint32>{}(static_cast<Uint32>(key.viewType)) +
                        0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<Uint32>{}(static_cast<Uint32>(key.format)) +
                        0x9e3779b9u + (hash << 6) + (hash >> 2);
                return hash;
            }
        };

        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView fullView = VK_NULL_HANDLE;
        VkImageView sampledView = VK_NULL_HANDLE;
        Vector<VkImageView> perMipViews;
        Vector<VkImageView> perMipSampledViews;
        UnorderedMap<AttachmentViewKey, VkImageView, AttachmentViewKeyHash> attachmentViews;
        UnorderedMap<SampledImageViewKey, VkImageView, SampledImageViewKeyHash> alternateSampledViews;
        UnorderedMap<StorageImageViewKey, VkImageView, StorageImageViewKeyHash> storageImageViews;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkExtent2D extent = {0, 0};
        Uint32 depth = 1;
        Uint32 arrayLayers = 1;
        Uint32 mipLevels = 1;
        Uint32 sampledBaseMipLevel = 0;
        Uint32 sampledLevelCount = 1;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_NONE;
        VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
        VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;
        VkImageCreateFlags imageCreateFlags = 0;
        Uint16 syncedTextureParamsVersion = 0;
        // Snapshot of ITextureObject::GetContentVersion() at the last successful sync;
        // lets SyncTexture skip the whole re-check/re-upload when content is unchanged.
        Uint64 syncedContentVersion = 0;
        // Snapshot of the defined mip-level count at the last sync. Folded into the early-out key
        // as defense-in-depth: any path that grows the level set (which resizes the sampled view)
        // busts the skip even if it failed to bump the content version.
        Uint32 syncedMipLevelCount = 0;

        TextureResource() = default;
        TextureResource(const TextureResource&) = delete;
        TextureResource(TextureResource&& that) noexcept {
            std::swap(this->image, that.image);
            std::swap(this->allocation, that.allocation);
            std::swap(this->fullView, that.fullView);
            std::swap(this->sampledView, that.sampledView);
            std::swap(this->perMipViews, that.perMipViews);
            std::swap(this->perMipSampledViews, that.perMipSampledViews);
            std::swap(this->attachmentViews, that.attachmentViews);
            std::swap(this->alternateSampledViews, that.alternateSampledViews);
            std::swap(this->storageImageViews, that.storageImageViews);
            std::swap(this->layout, that.layout);
            std::swap(this->extent, that.extent);
            std::swap(this->depth, that.depth);
            std::swap(this->arrayLayers, that.arrayLayers);
            std::swap(this->mipLevels, that.mipLevels);
            std::swap(this->sampledBaseMipLevel, that.sampledBaseMipLevel);
            std::swap(this->sampledLevelCount, that.sampledLevelCount);
            std::swap(this->format, that.format);
            std::swap(this->aspect, that.aspect);
            std::swap(this->viewType, that.viewType);
            std::swap(this->sampleCount, that.sampleCount);
            std::swap(this->imageCreateFlags, that.imageCreateFlags);
            std::swap(this->syncedTextureParamsVersion, that.syncedTextureParamsVersion);
            std::swap(this->syncedContentVersion, that.syncedContentVersion);
            std::swap(this->syncedMipLevelCount, that.syncedMipLevelCount);
        }

        void Reset() {
            if (fullView != VK_NULL_HANDLE) {
                vkDestroyImageView(s_device, fullView, nullptr);
            }
            if (sampledView != VK_NULL_HANDLE) {
                vkDestroyImageView(s_device, sampledView, nullptr);
            }
            for (const auto attachmentView : perMipViews) {
                if (attachmentView != VK_NULL_HANDLE) {
                    vkDestroyImageView(s_device, attachmentView, nullptr);
                }
            }
            for (const auto sampledView : perMipSampledViews) {
                if (sampledView != VK_NULL_HANDLE) {
                    vkDestroyImageView(s_device, sampledView, nullptr);
                }
            }
            for (const auto& [_, attachmentView] : attachmentViews) {
                if (attachmentView != VK_NULL_HANDLE) {
                    vkDestroyImageView(s_device, attachmentView, nullptr);
                }
            }
            for (const auto& [_, sampledView] : alternateSampledViews) {
                if (sampledView != VK_NULL_HANDLE) {
                    vkDestroyImageView(s_device, sampledView, nullptr);
                }
            }
            for (const auto& [_, storageImageView] : storageImageViews) {
                if (storageImageView != VK_NULL_HANDLE) {
                    vkDestroyImageView(s_device, storageImageView, nullptr);
                }
            }
            if (image != VK_NULL_HANDLE && allocation != nullptr) {
                vmaDestroyImage(s_allocator, image, allocation);
            }
            fullView = VK_NULL_HANDLE;
            sampledView = VK_NULL_HANDLE;
            perMipViews.clear();
            perMipSampledViews.clear();
            attachmentViews.clear();
            alternateSampledViews.clear();
            storageImageViews.clear();
            image = VK_NULL_HANDLE;
            allocation = nullptr;
            layout = VK_IMAGE_LAYOUT_UNDEFINED;
            extent = {0, 0};
            depth = 1;
            arrayLayers = 1;
            mipLevels = 1;
            sampledBaseMipLevel = 0;
            sampledLevelCount = 1;
            format = VK_FORMAT_UNDEFINED;
            aspect = VK_IMAGE_ASPECT_NONE;
            viewType = VK_IMAGE_VIEW_TYPE_2D;
            sampleCount = VK_SAMPLE_COUNT_1_BIT;
            imageCreateFlags = 0;
            syncedTextureParamsVersion = 0;
            syncedContentVersion = 0;
            syncedMipLevelCount = 0;
        }

        ~TextureResource() {
            Reset();
        }

        static inline VkDevice s_device = VK_NULL_HANDLE;
        static inline VmaAllocator s_allocator = VK_NULL_HANDLE;
    };

    Bool Initialize(const InitInfo& initInfo);
    void Shutdown();
    void BeginFrame(Uint32 frameIndex);

    TextureResource* SyncTextureAndGetDescriptor(
        MG_State::GLState::ITextureObject& texture);
    VkImageView GetOrCreateViewAtMipLevel(MG_State::GLState::ITextureObject& texture, Uint32 mipLevel);
    VkImageView GetOrCreateAttachmentViewAtMipLevel(MG_State::GLState::ITextureObject& texture, Uint32 mipLevel,
                                                    Uint32 baseArrayLayer, Uint32 layerCount,
                                                    VkImageViewType viewType);
    VkImageView GetOrCreateSampledViewAtMipLevel(MG_State::GLState::ITextureObject& texture, Uint32 mipLevel);
    VkImageView GetOrCreateSampledImageView(MG_State::GLState::ITextureObject& texture, VkFormat format);
    VkImageView GetOrCreateStorageImageView(MG_State::GLState::ITextureObject& texture, Uint32 mipLevel,
                                            VkFormat format, Bool layered, Int32 layer);
    void UpdateTrackedImageLayout(MG_State::GLState::ITextureObject* texture, VkImageLayout newLayout);
    void UpdateTrackedImageLayoutAfterAttachmentWrite(VkCommandBuffer commandBuffer,
                                                      MG_State::GLState::ITextureObject* texture,
                                                      Uint32 writtenMipLevel,
                                                      VkImageLayout newLayout);
    Bool TransitionTextureForSampling(VkCommandBuffer commandBuffer, MG_State::GLState::ITextureObject& texture);
    Bool TransitionTextureForStorageImage(VkCommandBuffer commandBuffer, MG_State::GLState::ITextureObject& texture);
    // Non-mutating probe for the per-draw storage-image fast path: true when preparing this
    // texture as a storage image may need work that is illegal inside a render pass (resource
    // creation, dirty-content upload, or a layout transition to GENERAL). Unknown state reports
    // true - a false positive merely ends the render pass, a false negative would skip a barrier.
    Bool NeedsStorageImagePreparation(MG_State::GLState::ITextureObject& texture) const;

    static VkImageAspectFlags ResolveSampledImageViewAspectMask(VkImageAspectFlags imageAspect);
    static VkFormat ResolveSampledImageViewFormat(VkFormat imageFormat, SamplerNumericDomain numericDomain);
    static Bool AreSampledImageViewFormatsCompatible(VkFormat imageFormat, VkFormat viewFormat);
    static Bool AreStorageImageViewFormatsCompatible(VkFormat imageFormat, VkFormat viewFormat);

    static Bool TransitionImageLayout(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout& trackedLayout,
                               VkImageLayout newLayout, VkPipelineStageFlags srcStageMask,
                               VkPipelineStageFlags dstStageMask, VkAccessFlags srcAccessMask,
                               VkAccessFlags dstAccessMask, VkImageAspectFlags aspectMask,
                               Uint32 baseMipLevel = 0, Uint32 levelCount = 1,
                               Uint32 layerCount = 1);

    SizeT CollectGarbage();

    // Per-draw sync memo. Within a single SetupDraw the same sampled texture is
    // resolved ~3x (SetupDraw's layout-probe loop, its post-transition loop, and
    // again inside ResolveSamplerDescriptor). No GL texture mutation can happen
    // mid-SetupDraw, and layout is tracked on the TextureResource independently of
    // SyncTexture, so after the first successful sync of a texture in a draw the
    // heavy SyncTexture work (mip-completeness/resource/view resync + dirty scan)
    // is pure redundancy. BeginDrawSyncScope opens a window in which repeat
    // SyncTextureAndGetDescriptor calls short-circuit to the already-synced
    // resource; EndDrawSyncScope closes it. Use the RAII DrawSyncScope guard.
    void BeginDrawSyncScope();
    void EndDrawSyncScope();

    // RAII guard that opens/closes a per-draw sync memo window (see above).
    class DrawSyncScope {
    public:
        explicit DrawSyncScope(VkTextureManager& manager) : m_manager(manager) { m_manager.BeginDrawSyncScope(); }
        ~DrawSyncScope() { m_manager.EndDrawSyncScope(); }
        DrawSyncScope(const DrawSyncScope&) = delete;
        DrawSyncScope& operator=(const DrawSyncScope&) = delete;
    private:
        VkTextureManager& m_manager;
    };

private:
    // Bumped in SyncTextureResource right after vmaCreateImage(texture). See GetTextureImageEpoch().
    Uint64 m_textureImageEpoch = 1;

    Bool SyncTexture(MG_State::GLState::ITextureObject &texture,
                     TextureResource &outResource);
    Bool SyncTextureResource(const MG_State::GLState::ITextureObject &texture,
                             TextureUploadTarget uploadTarget,
                             const IntVec3 &texelSize, SizeT byteSize, Uint32 mipLevels,
                             TextureResource &resource);
    Bool SyncTextureViews(const MG_State::GLState::ITextureObject& texture, TextureResource& resource);
    VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect,
                                VkImageViewType viewType, Uint32 baseMipLevel, Uint32 levelCount,
                                Uint32 baseArrayLayer,
                                Uint32 layerCount,
                                const VkComponentMapping* components = nullptr,
                                VkImageUsageFlags viewUsage = 0) const;
    Bool UploadDirtyMipLevels(MG_State::GLState::TextureObjectMipmap &mipmapTexture,
                      TextureUploadTarget uploadTarget,
                      TextureResource &outResource);
    static Bool CheckMipmapCompleteness(const MG_State::GLState::ITextureObject& texture,
                                        TextureUploadTarget& outTarget,
                                        IntVec3& outTexelSize,
                                        SizeT& outByteSize,
                                        Uint32& outMipLevelCount);
    static Uint32 GetUploadMipLevelCount(const MG_State::GLState::TextureObjectMipmap& texture, TextureUploadTarget target);
    static void ResolveViewMipRange(const MG_State::GLState::ITextureObject& texture, Uint32 mipLevels,
                                    Uint32& outBaseMipLevel, Uint32& outLevelCount);
    static VkImageAspectFlags GetAspectMaskForFormat(VkFormat format);
    void DeferResourceRelease(TextureResource&& resource);
    void DeferViewRelease(VkImageView view);
    void CollectDeferredReleases(Uint32 frameIndex);
    void DestroyDeferredReleases();
    static TextureIdentity MakeTextureIdentity(MG_State::GLState::ITextureObject* texture);
    void EraseTrackedTexture(const TextureIdentity& identity);
    void PruneStaleTextureAliases(MG_State::GLState::ITextureObject* texture);

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VmaAllocator m_allocator = nullptr;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    Uint32 m_currentFrameIndex = 0;

    Uint8 m_gcCounter = 0;
    // Active only between BeginDrawSyncScope/EndDrawSyncScope; identities of
    // textures already fully synced in the current draw (small N -> flat scan).
    Bool m_drawSyncScopeActive = false;
    // Per-draw sync memo: the identity plus the resolved resource pointer. The pointer is stable
    // across rehash in the node-based m_textureResources and stays valid for the draw (a texture
    // synced this draw is alive and is not erased mid-draw), so a repeat sync of the same texture
    // returns the resource without re-hashing the identity into m_textureResources.
    struct DrawSyncedTexture {
        TextureIdentity identity;
        TextureResource* resource = nullptr;
    };
    Vector<DrawSyncedTexture> m_drawSyncedThisDraw;
    // Formats whose mutable-image probe failed on this device; their images are created
    // without MUTABLE_FORMAT_BIT so repeat syncs neither re-probe nor flag-mismatch.
    std::unordered_set<VkFormat> m_mutableFormatUnsupported;
    std::unordered_map<TextureIdentity, WeakPtr<MG_State::GLState::ITextureObject>, TextureIdentityHash> m_aliveObjects;
    std::unordered_map<TextureIdentity, TextureResource, TextureIdentityHash> m_textureResources;
    Vector<Vector<TextureResource>> m_deferredReleases;
    Vector<Vector<VkImageView>> m_deferredViewReleases;
};
} // namespace MobileGL::MG_Backend::DirectVulkan
