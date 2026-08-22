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

// A GL 1D-ARRAY level keeps its LAYER COUNT in the state-side HEIGHT: that is what
// glTexImage2D(GL_TEXTURE_1D_ARRAY, width, layers) means, and the frontend records the level
// as {width, layers, 1} (see GL_Texture.cpp's AllocateStorage and the completeness walk in
// TextureObject.cpp, which shrinks only x down the chain). Vulkan packs it the other way: a
// 1D array is a VK_IMAGE_TYPE_1D image whose extent.height MUST be 1 and whose layers live in
// arrayLayers - i.e. in the slot this backend reads out of z. So every place that turns a GL
// level size into Vulkan image geometry has to move the count across first, and every GL-space
// sub-box that rides along with it has to move its y the same way. DirectGLES performs the
// identical remap onto the ES 2D array it maps 1D arrays to (GetBackendUploadSize).
//
// Applied to nothing else: a 2D array, a cube array and a 3D texture all already carry their
// depth/layer count in z, which is where the Vulkan side expects it.
inline IntVec3 ToVulkanLevelExtent(TextureTarget stateTarget, const IntVec3& glTexelSize) {
    if (stateTarget == TextureTarget::Texture1DArray) {
        return {glTexelSize.x(), 1, glTexelSize.y()};
    }
    return glTexelSize;
}

// A GL framebuffer attachment's level/layer, and a GL image unit's, are relative to the texture
// the application NAMED. When that texture was created by glTextureView (ARB_texture_view) they
// are relative to the VIEW, and have to be shifted into the storage image's numbering before they
// can index a Vulkan subresource - DirectVulkan gives a view no image of its own, it shares the
// storage texture's (VkTextureManager::StorageTextureOf).
//
// Apply EXACTLY ONCE, at the boundary where a GL level/layer becomes a subresource index. Every
// GetOrCreate*View entry point below expects values that have already been through here, and so
// does everything that reads or copies an attachment directly. Both are identity on a plain
// texture (TEXTURE_VIEW_MIN_LEVEL / MIN_LAYER are 0 there), so the conversion is unconditional
// and there is no second, view-only code path to keep in step.
inline Uint32 ToStorageMipLevel(const MG_State::GLState::ITextureObject* texture, Int glLevel) {
    const Uint32 level = static_cast<Uint32>(glLevel > 0 ? glLevel : 0);
    return texture != nullptr ? level + static_cast<Uint32>(texture->GetViewMinLevel()) : level;
}

inline Uint32 ToStorageArrayLayer(const MG_State::GLState::ITextureObject* texture, Int glLayer) {
    const Uint32 layer = static_cast<Uint32>(glLayer > 0 ? glLayer : 0);
    return texture != nullptr ? layer + static_cast<Uint32>(texture->GetViewMinLayer()) : layer;
}

class VkTextureManager {
public:
    // Monotonic epoch bumped whenever a texture VkImage is (re)created. The render-pass
    // manager keys its per-draw fast path on this so an attachment's image recreation
    // invalidates the cached render pass (dirty-flag tracking; portable to Vulkan 1.1).
    Uint64 GetTextureImageEpoch() const { return m_textureImageEpoch; }
    // Bumped whenever any tracked texture resource is erased; cached
    // TextureResource pointers are valid only while this is unchanged.
    Uint64 GetResourceEraseEpoch() const { return m_resourceEraseEpoch; }

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
        // VK_KHR_image_format_list is enabled: MUTABLE_FORMAT images can name the exact set of
        // formats they will be viewed as, which is what lets a tiler keep them compressed.
        Bool imageFormatListSupported = false;
        // Union of shader stages sampled-read barriers may name on this device; the renderer
        // builds it from the enabled features because geometry/tessellation stage bits are
        // invalid in a barrier when their feature is off.
        VkPipelineStageFlags sampledReadStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        // Family of `graphicsQueue`; the manager creates its own command pool
        // on it for the recycled upload-batch command buffers, so their parked
        // allocations never sit in (and fragment) the renderer's shared pool
        // that frame command buffers churn through every frame.
        Uint32 graphicsQueueFamilyIndex = 0;
    };

    struct TextureResource {
        struct AttachmentViewKey {
            Uint32 mipLevel = 0;
            Uint32 baseArrayLayer = 0;
            Uint32 layerCount = 1;
            VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
            // May differ from the image format: sRGB images attach through their UNORM
            // twin while GL_FRAMEBUFFER_SRGB is disabled.
            VkFormat viewFormat = VK_FORMAT_UNDEFINED;

            Bool operator==(const AttachmentViewKey& other) const {
                return mipLevel == other.mipLevel &&
                       baseArrayLayer == other.baseArrayLayer &&
                       layerCount == other.layerCount &&
                       viewType == other.viewType &&
                       viewFormat == other.viewFormat;
            }
        };

        struct AttachmentViewKeyHash {
            SizeT operator()(const AttachmentViewKey& key) const {
                SizeT hash = std::hash<Uint32>{}(key.mipLevel);
                hash ^= std::hash<Uint32>{}(key.baseArrayLayer) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<Uint32>{}(key.layerCount) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<Uint32>{}(static_cast<Uint32>(key.viewType)) +
                        0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<Uint32>{}(static_cast<Uint32>(key.viewFormat)) +
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

        // Layer range and aspect join the key because a GL texture view (ARB_texture_view) can
        // differ from its storage on either: the Better Clouds shape samples ONE D24S8 image
        // through two GL names in one draw, the parent with the stencil aspect and the view with
        // the depth aspect, and a layer-sliced view of an array texture names a sub-range of the
        // same image. Without these two fields those views would alias each other in the cache.
        struct SampledImageViewKey {
            Uint32 baseMipLevel = 0;
            Uint32 levelCount = 1;
            Uint32 baseArrayLayer = 0;
            Uint32 layerCount = 1;
            VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;

            Bool operator==(const SampledImageViewKey& other) const {
                return baseMipLevel == other.baseMipLevel &&
                       levelCount == other.levelCount &&
                       baseArrayLayer == other.baseArrayLayer &&
                       layerCount == other.layerCount &&
                       viewType == other.viewType &&
                       format == other.format &&
                       aspect == other.aspect;
            }
        };

        struct SampledImageViewKeyHash {
            SizeT operator()(const SampledImageViewKey& key) const {
                SizeT hash = std::hash<Uint32>{}(key.baseMipLevel);
                hash ^= std::hash<Uint32>{}(key.levelCount) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<Uint32>{}(key.baseArrayLayer) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<Uint32>{}(key.layerCount) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<Uint32>{}(static_cast<Uint32>(key.viewType)) +
                        0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<Uint32>{}(static_cast<Uint32>(key.format)) +
                        0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<Uint32>{}(static_cast<Uint32>(key.aspect)) +
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
        // Usage the live image was created with. STORAGE is only requested for textures that
        // have actually been bound to a GL image unit, because on Adreno a storage-capable
        // image loses UBWC bandwidth compression; a later image binding upgrades the usage
        // and recreates the image, so the resolved usage has to be part of the compatibility
        // check that decides whether the existing image can be kept.
        VkImageUsageFlags usageFlags = 0;
        // True once this image was (re)resolved while the texture was already marked as an
        // image-unit texture. Distinguishes "not upgraded yet" from "cannot be upgraded"
        // (a format whose optimalTilingFeatures lack STORAGE_IMAGE never gains the bit), so
        // NeedsStorageImagePreparation cannot ask for a recreate that will never happen.
        Bool storageUsageResolved = false;
        Uint16 syncedTextureParamsVersion = 0;
        // Recording generation (VkTextureManager::GetRecordingGeneration) of the last
        // command referencing this image that was recorded into the CURRENT frame
        // command buffer. An image untouched by the open recording may have its
        // out-of-pass work (deferred clears, sampled-layout transitions) recorded
        // into the frame's PRE command buffer - which executes strictly before the
        // frame's commands - instead of splitting the active render pass.
        Uint64 lastRecordingGeneration = 0;
        // Snapshot of ITextureObject::GetContentVersion() at the last successful sync;
        // lets SyncTexture skip the whole re-check/re-upload when content is unchanged.
        Uint64 syncedContentVersion = 0;
        // Snapshot of the defined mip-level count at the last sync. Folded into the early-out key
        // as defense-in-depth: any path that grows the level set (which resizes the sampled view)
        // busts the skip even if it failed to bump the content version.
        Uint32 syncedMipLevelCount = 0;
        // Snapshot of ITextureObject::GetShapeVersion() at the last successful sync. The content
        // version alone does NOT cover a re-specification: glTexImage2D(..., nullptr) on an
        // already-defined level changes its size or format and dirties no texel, so it moves the
        // shape version and nothing else. Without this in the early-out key the image, its views
        // and therefore imageSize() all keep answering with the texture's PREVIOUS shape.
        Uint64 syncedShapeVersion = 0;

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
            std::swap(this->usageFlags, that.usageFlags);
            std::swap(this->storageUsageResolved, that.storageUsageResolved);
            std::swap(this->syncedTextureParamsVersion, that.syncedTextureParamsVersion);
            std::swap(this->lastRecordingGeneration, that.lastRecordingGeneration);
            std::swap(this->syncedContentVersion, that.syncedContentVersion);
            std::swap(this->syncedMipLevelCount, that.syncedMipLevelCount);
            std::swap(this->syncedShapeVersion, that.syncedShapeVersion);
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
            usageFlags = 0;
            storageUsageResolved = false;
            syncedTextureParamsVersion = 0;
            syncedContentVersion = 0;
            syncedMipLevelCount = 0;
            syncedShapeVersion = 0;
        }

        ~TextureResource() {
            Reset();
        }

        static inline VkDevice s_device = VK_NULL_HANDLE;
        static inline VmaAllocator s_allocator = VK_NULL_HANDLE;
    };

    struct SampledTextureSnapshot {
        VkImageView imageView = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    Bool Initialize(const InitInfo& initInfo);
    void Shutdown();
    void BeginFrame(Uint32 frameIndex);
    // Submits the accumulated texture-upload batch (one command buffer, one
    // vkQueueSubmit, one pooled fence) if any uploads are pending. MUST run
    // before any other vkQueueSubmit on the shared graphics queue whose
    // commands may consume an image the batch writes - the frame command
    // buffer submit (mid-frame flush, readback, Present) and the
    // preserve-on-recreate copy are the existing callers. No-op when the
    // batch is empty.
    void FlushPendingUploads();
    // Drains every frame slot's deferred image/view releases. Only valid when
    // the caller has proven every queue submission complete; used by the
    // present-less frame-boundary drain.
    void CollectAllDeferredReleases();

    // ---- GL texture views (ARB_texture_view / GL 4.6 core 8.18) ----
    // The GL texture whose STORAGE backs the given one: itself, or - for a texture created by
    // glTextureView - the texture it views. Every image-scoped question (which VkImage, its
    // LAYOUT, its uploads, its extent, its usage) must be asked of this object, because a view
    // has none of its own; only the VkImageViews differ per GL texture object. Sharing one
    // TextureResource is not an optimisation, it is the only correct arrangement: layout is a
    // property of the image, and VulkanRenderer caches raw pointers straight to the resource's
    // layout field, so a second resource aliasing the same image would desynchronise the moment
    // either of them transitioned it.
    static MG_State::GLState::ITextureObject& StorageTextureOf(MG_State::GLState::ITextureObject& texture);

    // The window a GL texture object opens onto its storage image. For a plain texture this is
    // the resource's own full extent; for a view it is the sub-range, format and aspect
    // glTextureView gave it. Views built from a non-default window must live in the KEYED caches
    // (attachmentViews / alternateSampledViews), never in the per-mip vectors, which belong to
    // the storage texture's own defaults.
    struct TextureViewWindow {
        Uint32 baseMipLevel = 0;
        Uint32 levelCount = 1;
        Uint32 baseArrayLayer = 0;
        Uint32 layerCount = 1;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
        VkImageAspectFlags sampledAspect = VK_IMAGE_ASPECT_COLOR_BIT;
        Bool isTextureView = false;
    };
    TextureViewWindow ResolveTextureViewWindow(MG_State::GLState::ITextureObject& texture,
                                               const TextureResource& resource) const;
    // Records what a GL texture view needs of the image it views, so the next sync of the
    // STORAGE texture creates (or recreates and copies forward) an image the view can be built
    // over. See m_viewRequestedImageFlags for why this is lazy rather than unconditional.
    void NoteTextureViewImageRequirements(MG_State::GLState::ITextureObject& viewTexture,
                                          MG_State::GLState::ITextureObject& storageTexture);
    VkImageCreateFlags GetViewRequestedImageFlags(const MG_State::GLState::ITextureObject& storageTexture) const;
    // Builds (and caches, keyed by the whole window) one sampled VkImageView over a storage
    // image. Shared back end of every GL-texture-view sampled path.
    VkImageView GetOrCreateWindowedSampledView(MG_State::GLState::ITextureObject& texture,
                                               TextureResource& resource, const TextureViewWindow& window);

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
    // Copies the complete sampler-visible mip range into a transient sampled image. The source is
    // restored to its prior layout, so image-store descriptors continue to name the original image.
    // The transient ownership is tied to the current frame slot and is safe through its submission.
    Bool SnapshotTextureForSampling(VkCommandBuffer commandBuffer, MG_State::GLState::ITextureObject& texture,
                                    SamplerNumericDomain numericDomain,
                                    VkPipelineStageFlags consumerShaderStageMask,
                                    SampledTextureSnapshot& outSnapshot);

    // Recording-generation bookkeeping for the pre-pass command stream. The
    // generation advances every time the frame command buffer (re)begins
    // recording; a resource whose stamp does not match was not referenced by
    // any command in the open recording, so its out-of-pass work may safely
    // execute ahead of the whole recording (in the pre command buffer).
    void AdvanceRecordingGeneration() { ++m_recordingGeneration; }
    void StampResourceRecordingUse(TextureResource& resource) const {
        resource.lastRecordingGeneration = m_recordingGeneration;
    }
    // Map-lookup variant for callers that only hold the GL texture object.
    void StampTextureRecordingUse(MG_State::GLState::ITextureObject* texture);
    Bool WasTouchedThisRecording(const TextureResource& resource) const {
        return resource.lastRecordingGeneration == m_recordingGeneration;
    }
    // Records that this texture is bound to a GL image unit, so its image must carry
    // VK_IMAGE_USAGE_STORAGE_BIT. Must be called before NeedsStorageImagePreparation, and
    // therefore before the render pass is committed: an image that has to be upgraded is
    // recreated, which is illegal inside a render pass. Sticky for the texture's lifetime -
    // GL lets an image binding come and go, and re-creating the image every time it does
    // would cost far more than the compression it wins back.
    void MarkStorageImageTexture(MG_State::GLState::ITextureObject& texture);
    // True when this texture is marked but its live image predates the mark, i.e. the next sync
    // will recreate it with STORAGE usage and copy the old contents forward. Callers use this to
    // submit their pending recording first, so that copy cannot read pre-flush content.
    Bool NeedsStorageUsageUpgrade(MG_State::GLState::ITextureObject& texture) const;
    // The same ordering question for the other recreate-and-preserve trigger: true when this
    // texture's live image carries a shorter mip chain than a full one, so defining the missing
    // levels recreates it and copies the old contents forward.
    Bool NeedsMipChainGrowth(MG_State::GLState::ITextureObject& texture) const;
    // Non-mutating probe for the per-draw storage-image fast path: true when preparing this
    // texture as a storage image may need work that is illegal inside a render pass (resource
    // creation, dirty-content upload, or a layout transition to GENERAL). Unknown state reports
    // true - a false positive merely ends the render pass, a false negative would skip a barrier.
    Bool NeedsStorageImagePreparation(MG_State::GLState::ITextureObject& texture) const;

    // `depthStencilTextureMode` is the texture's GL_DEPTH_STENCIL_TEXTURE_MODE; it only decides
    // anything for an image that carries both aspects. Defaulted so the call sites that have no
    // texture in hand keep the depth-aspect answer they have always given.
    static VkImageAspectFlags ResolveSampledImageViewAspectMask(VkImageAspectFlags imageAspect,
                                                                GLenum depthStencilTextureMode = GL_DEPTH_COMPONENT);
    static VkFormat ResolveSampledImageViewFormat(VkFormat imageFormat, SamplerNumericDomain numericDomain);
    static Bool AreSampledImageViewFormatsCompatible(VkFormat imageFormat, VkFormat viewFormat);
    static Bool AreStorageImageViewFormatsCompatible(VkFormat imageFormat, VkFormat viewFormat);

    // Moves `image` to `newLayout` and writes the new layout back through `trackedLayout`.
    //
    // The barrier covers EVERY array layer of the image, and there is deliberately no layer
    // parameter to say otherwise: layout here is tracked per IMAGE (one `TextureResource::layout`,
    // or one caller-owned variable), so a barrier narrower than the image would leave the layers it
    // skipped in the old layout while the tracker claims they moved. Every transfer against a
    // framebuffer attachment above layer 0 - glReadPixels, glBlitFramebuffer, glCopyTexSubImage,
    // glCopyImageSubData - then ran its copy on a layer no barrier had transitioned.
    //
    // The mip range IS a parameter, because mip levels really are transitioned piecewise (see
    // UpdateTrackedImageLayoutAfterAttachmentWrite and the mipmap generation loops): those callers
    // move the complement of the level they wrote so the whole image converges on one layout again.
    // Nothing does, or can, do that per layer.
    static Bool TransitionImageLayout(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout& trackedLayout,
                               VkImageLayout newLayout, VkPipelineStageFlags srcStageMask,
                               VkPipelineStageFlags dstStageMask, VkAccessFlags srcAccessMask,
                               VkAccessFlags dstAccessMask, VkImageAspectFlags aspectMask,
                               Uint32 baseMipLevel = 0, Uint32 levelCount = 1);

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
    // See AdvanceRecordingGeneration. Starts above every resource's default
    // stamp of 0 so a fresh resource counts as untouched.
    Uint64 m_recordingGeneration = 1;

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
    // Frees the fence/command buffer/staging buffer of every in-flight texture
    // upload whose fence has signaled (submission order = completion order on
    // the single queue, so the scan stops at the first still-pending entry).
    // waitAll blocks on every entry - Shutdown's drain.
    void ReclaimCompletedUploads(Bool waitAll = false);
    static TextureIdentity MakeTextureIdentity(MG_State::GLState::ITextureObject* texture);
    void EraseTrackedTexture(const TextureIdentity& identity);
    void PruneStaleTextureAliases(MG_State::GLState::ITextureObject* texture);
    SizeT PruneDeadTextures();

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VmaAllocator m_allocator = nullptr;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    // Dedicated pool for the recycled upload-batch command buffers (see
    // InitInfo::graphicsQueueFamilyIndex).
    VkCommandPool m_uploadCommandPool = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    Bool m_imageFormatListSupported = false;
    Uint32 m_currentFrameIndex = 0;

    Uint8 m_gcCounter = 0;
    // Frame-boundary GC gate: counts BeginFrame calls, not draws, so texture churn
    // through non-draw paths (FBO clears, readbacks) still reaches the prune.
    Uint32 m_gcFrameCounter = 0;
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
    // Cross-draw sampled-texture memo: the same few textures (atlas, lightmap)
    // are resolved on every draw, so cache their resource pointers and skip the
    // alive/resource map lookups. Node-based std::unordered_map keeps the
    // pointees stable across inserts; erases bump m_resourceEraseEpoch, which
    // every memo entry must match. SyncTexture still runs on memo hits, so
    // content/param freshness is unaffected. A dead-then-reused texture address
    // cannot false-hit: the new object carries a new lifetime id.
    struct SyncedTextureMemoEntry {
        const MG_State::GLState::ITextureObject* texture = nullptr;
        Uint64 lifetimeId = 0;
        Uint64 eraseEpoch = 0;
        TextureResource* resource = nullptr;
    };
    static constexpr Uint32 kSyncedTextureMemoSize = 8;
    SyncedTextureMemoEntry m_syncedTextureMemo[kSyncedTextureMemoSize];
    Uint32 m_syncedTextureMemoNext = 0;
    Uint64 m_resourceEraseEpoch = 1;
    // Formats whose mutable-image probe failed on this device; their images are created
    // without MUTABLE_FORMAT_BIT so repeat syncs neither re-probe nor flag-mismatch.
    std::unordered_set<VkFormat> m_mutableFormatUnsupported;
    // Formats whose 3D images refused VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT. Per format+usage,
    // exactly like the mutable-format verdict above, so it is answered at image creation and
    // remembered rather than probed once globally.
    std::unordered_set<VkFormat> m_2dArrayCompatibleUnsupported;
    std::unordered_map<TextureIdentity, WeakPtr<MG_State::GLState::ITextureObject>, TextureIdentityHash> m_aliveObjects;
    std::unordered_map<TextureIdentity, TextureResource, TextureIdentityHash> m_textureResources;
    // Textures that have been bound to a GL image unit (see MarkStorageImageTexture).
    std::unordered_set<TextureIdentity, TextureIdentityHash> m_storageImageTextures;
    // Extra VkImageCreateFlags a GL texture view needs on the storage image it views, keyed by
    // the STORAGE texture's identity. Requested lazily, exactly like STORAGE usage above and for
    // the same reason: VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT costs bandwidth compression on tilers
    // (it is what VK_KHR_image_format_list exists to claw back), so setting it on every
    // immutable-storage texture would tax every glTexStorage2D render target in a game for a
    // feature almost none of them use. A SAME-format view - which is the common case, and the
    // Better Clouds case - needs no flag at all and therefore costs nothing.
    std::unordered_map<TextureIdentity, VkImageCreateFlags, TextureIdentityHash> m_viewRequestedImageFlags;
    // Supported multisample counts per format, so repeat texture syncs do not
    // re-query vkGetPhysicalDeviceImageFormatProperties.
    std::unordered_map<VkFormat, VkSampleCountFlags> m_multisampleCountsByFormat;
    Vector<Vector<TextureResource>> m_deferredReleases;
    Vector<Vector<VkImageView>> m_deferredViewReleases;

    // --- Batched upload machinery ---
    // Uploads within a frame are recorded into ONE shared command buffer and
    // submitted with ONE vkQueueSubmit at FlushPendingUploads (the renderer
    // flushes before every frame-command-buffer submit). Staging memory comes
    // from a pool of persistently-mapped, reusable blocks instead of a
    // vmaCreateBuffer per upload.
    struct UploadStagingBlock {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        Uint8* mapped = nullptr; // persistently mapped for the block's lifetime
        VkDeviceSize capacity = 0;
        VkDeviceSize cursor = 0; // bump cursor while the block backs the open batch
    };
    // Opens the batch command buffer lazily (allocates/reuses + begins recording).
    VkCommandBuffer EnsureUploadBatchOpen();
    // Bump-allocates `size` staging bytes for the open batch, growing onto a
    // new/pooled block when the current one cannot fit. Returns the write
    // pointer; outBuffer/outBaseOffset locate the space for copy commands.
    Uint8* AcquireUploadStagingSpace(VkDeviceSize size, VkBuffer& outBuffer, VkDeviceSize& outBaseOffset);
    void RecycleUploadStagingBlock(UploadStagingBlock&& block);
    // Drops a recorded-but-unsubmitted batch on the floor. Shutdown only: the
    // device is being torn down, so the lost texel data is unobservable.
    void DiscardPendingUploadBatch();
    void DestroyUploadPools();

    Vector<UploadStagingBlock> m_freeUploadStagingBlocks;
    VkDeviceSize m_freeUploadStagingBytes = 0;
    Vector<VkCommandBuffer> m_freeUploadCommandBuffers;
    Vector<VkFence> m_freeUploadFences;
    Bool m_uploadBatchOpen = false;
    VkCommandBuffer m_uploadBatchCommandBuffer = VK_NULL_HANDLE;
    // Blocks whose staging bytes the open batch's copies reference (last =
    // the block the bump cursor is currently allocating from).
    Vector<UploadStagingBlock> m_uploadBatchBlocks;
    // Images the open batch writes; consulted for the rare re-upload-after-
    // draw flush and by DeferResourceRelease (an unsubmitted command buffer
    // referencing a deferred-released image would escape every fence-based
    // destruction proof, so the batch is flushed before the image is parked).
    Vector<VkImage> m_uploadBatchImages;
    VkDeviceSize m_uploadBatchStagingBytes = 0;

    // Texture uploads are submitted out-of-band but NOT waited on (waiting
    // behind the queue serialized the CPU against the previous frame's GPU
    // work every time an animated atlas re-uploaded). Each flushed batch's
    // transients are parked here and RECYCLED (fence reset to the fence pool,
    // command buffer reset to the CB pool, staging blocks back to the block
    // pool) once the batch fence signals.
    struct PendingUploadReclaim {
        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        Vector<UploadStagingBlock> stagingBlocks;
    };
    Vector<PendingUploadReclaim> m_pendingUploadReclaims;
};
} // namespace MobileGL::MG_Backend::DirectVulkan
