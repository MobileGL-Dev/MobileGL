// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkTextureManager.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "../VkIncludes.h"
#include "../MagmaSession.h"
#include <Includes.h>
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>
#include <MG_State/GLState/TextureState/TextureObject.h>
#include <vk_mem_alloc.h>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include <Config.h>
// The handle-keyed arm's key type. Header-only POD, disagg-only, exactly the shape
// MagmaPipeArms.h already gives this directory.
#include <MG_Pipe/MGPipeHandles.h>

namespace MobileGL::MG_State::GLState {
class ITextureObject;
}
namespace MobileGL::MG_Pipe {
struct MGPipeResourceRecord;
}

namespace MobileGL::MG_Backend::DirectVulkan {
enum class SamplerNumericDomain : Uint8;

// What VkFormat a GL internal format is BACKED with, and how a shadow upload has to be reshaped to
// fit it. This is not the same question as "is there an exact VkFormat for this GL format", which is
// what ConvertTextureInternalFormatToVkEnum answers: several GL formats have no Vulkan twin at all
// (RGBA2, RGBA12) and several three-channel ones are deliberately widened to their four-channel twin
// because Vulkan devices rarely support the 3-channel layouts.
//
// SHARED, and it must stay the only answer to that question. A renderbuffer and a texture of the
// same GL format have to resolve to the SAME VkFormat or every blit, resolve and glCopyImageSubData
// between them crosses a size-incompatible pair, which vkCmdCopyImage leaves undefined
// (VUID-vkCmdCopyImage-srcImage-01548). The renderbuffer path used to carry a hand-maintained second
// copy of this table that was missing four rows - RGBA2, RGBA4, RGB5A1 and RGBA12 - so those four
// renderbuffer formats either got no image at all or a 16-bit-packed one facing a 32-bit texture.
struct TextureFormatInfo {
    VkFormat format = VK_FORMAT_UNDEFINED;
    // The GL format has three channels and is carried in a four-channel image; a shadow upload has
    // to be expanded, inserting `alphaBytes` after every `componentByteCount * 3` source bytes.
    Bool expandRgbToRgba = false;
    Uint32 componentByteCount = 0;
    Array<Uint8, 4> alphaBytes = {0, 0, 0, 0};
};

// Callers that only need the backing VkFormat (a renderbuffer has no shadow upload to reshape) take
// `.format` and ignore the rest.
TextureFormatInfo ResolveTextureFormatInfo(TextureInternalFormat format);

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

// How many Vulkan array layers (or, for a 3D image, z slices) a GL framebuffer attachment spans.
//
// THE ONE COPY, deliberately. This used to exist twice - privately in VkRenderPassManager.cpp and
// again in VkClearManager.cpp - and the two are not independent: the render pass builds the
// attachment view and VkFramebufferCreateInfo::layers from one, while the CLEAR key built from the
// other is written verbatim into VkImageSubresourceRange::layerCount when a queued glClear is
// materialised outside a render pass (MaterializePendingClearForTexture). They are two consumers
// of the same GL clear, so any disagreement means the same glClear produces two different pictures
// depending only on which path happens to consume it first - and the materialise path then POPS
// the entry, so the other one never runs. Fixing one copy and leaving the other is exactly how
// that split gets introduced; keep them the same function.
//
// Two shapes make this more than `size.z()`:
//   * GL_TEXTURE_1D_ARRAY keeps its layer count in the state-side HEIGHT (see ToVulkanLevelExtent
//     just above), so z reads 1 and every layer above the first was silently dropped.
//   * GL_TEXTURE_CUBE_MAP is attached layered as its REPRESENTATIVE upload target, the +X face
//     (ResolveRepresentableFramebufferTextureUploadTarget), and one face's level size has z = 1 -
//     but a layered cube attachment names all six faces (GL 4.6 core 9.2.8), which are the image's
//     six array layers. A cube ARRAY needs no such arm: its representative target carries 6n in z.
inline Uint32 ResolveAttachmentLayerCount(const MG_State::GLState::FramebufferAttachmentObject& attachment) {
    if (!attachment.IsLayered()) {
        return 1u;
    }
    const auto& texture = attachment.GetTexture();
    const TextureTarget target = texture != nullptr ? texture->GetTarget() : TextureTarget::Unknown;
    if (target == TextureTarget::TextureCubeMap) {
        return 6u;
    }
    return static_cast<Uint32>(std::max(ToVulkanLevelExtent(target, attachment.GetSize()).z(), 1));
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
            // GL_TEXTURE_SWIZZLE_* is per-texture state, so two views over one storage with the
            // same window but different swizzles are different views. Baked into the key because
            // a GL texture view's ONLY sampled view lives in this cache: unlike the storage
            // texture's own sampledView, which SyncTextureViews rebuilds whenever the params
            // version moves, nothing else would ever notice a swizzle change on a view.
            Uint32 componentSwizzle = 0;

            Bool operator==(const SampledImageViewKey& other) const {
                return baseMipLevel == other.baseMipLevel &&
                       levelCount == other.levelCount &&
                       baseArrayLayer == other.baseArrayLayer &&
                       layerCount == other.layerCount &&
                       viewType == other.viewType &&
                       format == other.format &&
                       aspect == other.aspect &&
                       componentSwizzle == other.componentSwizzle;
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
                hash ^= std::hash<Uint32>{}(key.componentSwizzle) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
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
        // The same, for the last recorded command that WRITES the image (a render pass it is an
        // attachment of, a clear, a blit/copy into it, a mip generation, a storage-image binding).
        // An upload into an image written this recording has to be ordered after that write
        // (VulkanRenderer::OrderPendingUploadAfterRecording).
        Uint64 lastRecordingWriteGeneration = 0;
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
        // The handle-keyed arm's (P5f fm) early-out key: the applier record's server-owned
        // Serial at the last successful sync. 0 also means "this resource has only ever been
        // synced by the frontend arm" - a wire record's Serial starts at 0 on a create and moves
        // on every mutation, so the first handle sync of a fresh image always re-checks.
        Uint64 syncedWireSerial = 0;
        // SHARED IMAGES: level 0 is a server shared image (an imported AHardwareBuffer), not a
        // VMA allocation. The resource owns the image and its dedicated imported memory, and
        // holds the registry's reference so the buffer outlives every use (deferred releases
        // included). `sharedImageAlphaOne`: an X-format image, whose alpha bytes are undefined.
        VkDeviceMemory importedMemory = VK_NULL_HANDLE;
        Uint64 sharedImageId = 0;
        SharedPtr<const void> sharedImageOwner;
        Bool sharedImageAlphaOne = false;
        // The reading side's ownership of the image (VulkanRenderer::AcquireSharedImage): whether
        // this device holds it now - acquired from the foreign family and not yet released back at
        // the frame boundary - the writer's generation it last acquired, and the
        // m_wireTextureResources key it lives under, by which the frame boundary finds it again.
        Bool sharedImageHeld = false;
        Uint64 sharedImageGeneration = 0;
        Uint64 sharedImageKey = 0;
        // A YUV shared image (NV12/P010) is never the texture's image: the texture keeps its own
        // RGBA8 level 0, into which the renderer converts the image at its first use in each frame
        // (WireYuvImage.inc) - `yuvSource` being that conversion's import of the buffer, made on
        // first use, and `yuvConvertedFrame` the frame it last ran in. `yuvOwner` holds the
        // registry's image for as long as the texture names it.
        Uint64 yuvImageId = 0;
        SharedPtr<const void> yuvOwner;
        SharedPtr<void> yuvSource;
        Uint64 yuvConvertedFrame = 0;

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
            std::swap(this->lastRecordingWriteGeneration, that.lastRecordingWriteGeneration);
            std::swap(this->syncedContentVersion, that.syncedContentVersion);
            std::swap(this->syncedMipLevelCount, that.syncedMipLevelCount);
            std::swap(this->syncedShapeVersion, that.syncedShapeVersion);
            std::swap(this->syncedWireSerial, that.syncedWireSerial);
            std::swap(this->importedMemory, that.importedMemory);
            std::swap(this->sharedImageId, that.sharedImageId);
            std::swap(this->sharedImageOwner, that.sharedImageOwner);
            std::swap(this->sharedImageAlphaOne, that.sharedImageAlphaOne);
            std::swap(this->sharedImageHeld, that.sharedImageHeld);
            std::swap(this->sharedImageGeneration, that.sharedImageGeneration);
            std::swap(this->sharedImageKey, that.sharedImageKey);
            std::swap(this->yuvImageId, that.yuvImageId);
            std::swap(this->yuvOwner, that.yuvOwner);
            std::swap(this->yuvSource, that.yuvSource);
            std::swap(this->yuvConvertedFrame, that.yuvConvertedFrame);
        }

        void Reset() {
            if (fullView != VK_NULL_HANDLE) {
                vkDestroyImageView(s_device.Get(), fullView, nullptr);
            }
            if (sampledView != VK_NULL_HANDLE) {
                vkDestroyImageView(s_device.Get(), sampledView, nullptr);
            }
            for (const auto attachmentView : perMipViews) {
                if (attachmentView != VK_NULL_HANDLE) {
                    vkDestroyImageView(s_device.Get(), attachmentView, nullptr);
                }
            }
            for (const auto sampledView : perMipSampledViews) {
                if (sampledView != VK_NULL_HANDLE) {
                    vkDestroyImageView(s_device.Get(), sampledView, nullptr);
                }
            }
            for (const auto& [_, attachmentView] : attachmentViews) {
                if (attachmentView != VK_NULL_HANDLE) {
                    vkDestroyImageView(s_device.Get(), attachmentView, nullptr);
                }
            }
            for (const auto& [_, sampledView] : alternateSampledViews) {
                if (sampledView != VK_NULL_HANDLE) {
                    vkDestroyImageView(s_device.Get(), sampledView, nullptr);
                }
            }
            for (const auto& [_, storageImageView] : storageImageViews) {
                if (storageImageView != VK_NULL_HANDLE) {
                    vkDestroyImageView(s_device.Get(), storageImageView, nullptr);
                }
            }
            if (image != VK_NULL_HANDLE && allocation != nullptr) {
                vmaDestroyImage(s_allocator.Get(), image, allocation);
            }
            if (importedMemory != VK_NULL_HANDLE) {
                if (image != VK_NULL_HANDLE) vkDestroyImage(s_device.Get(), image, nullptr);
                vkFreeMemory(s_device.Get(), importedMemory, nullptr);
            }
            importedMemory = VK_NULL_HANDLE;
            sharedImageId = 0;
            sharedImageOwner.reset();
            sharedImageAlphaOne = false;
            sharedImageHeld = false;
            sharedImageGeneration = 0;
            sharedImageKey = 0;
            yuvImageId = 0;
            yuvOwner.reset();
            yuvSource.reset();
            yuvConvertedFrame = 0;
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
            syncedWireSerial = 0;
        }

        ~TextureResource() {
            Reset();
        }

        static inline SessionLocal<VkDevice> s_device;  // per Magma session (MagmaSession.h)
        static inline SessionLocal<VmaAllocator> s_allocator;
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
    // Non-blocking idle proof for wire-object retirement. Independent texture
    // submissions are not represented by VulkanRenderer's submit counter.
    Bool WireUploadsAreIdle();

    // Drains every frame slot's deferred image/view releases. Only valid when
    // the caller has proven every queue submission complete; used by the
    // present-less frame-boundary drain.
    void CollectAllDeferredReleases();

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
        VkComponentMapping components{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                                      VK_COMPONENT_SWIZZLE_A};
        Bool isTextureView = false;
    };

    // P5f (fm): THE HANDLE-KEYED ARM of SyncTextureAndGetDescriptor. Under an active transport
    // the apply thread may not name the client's ITextureObject (rule E), so the clear / blit /
    // readback / mipmap verbs resolve their attachment textures from the wire handle the
    // framebuffer or mip record carried: shape from the applier's resource record
    // (MGPipeApplier().TextureResources), texels from the server's staged-texture store
    // (P5c tx), pending-upload bookkeeping consumed from the same record. Returns null - never
    // a frontend fallback - when the record or the shape does not resolve; the caller declines
    // loudly, which is the honest answer a missing record deserves.
    //
    // The resource lives in m_wireTextureResources keyed by StagedTextureStore::KeyForHandle -
    // keyed by the handle's {slot, gen}, so a recycled slot can never inherit its
    // predecessor's image. Texture views resolve the storage owner from
    // Desc.ViewOf and the window from ViewCso; buffer-backed textures remain P7's buffer arm.
    Bool ReadUnbackedWireLevel(MG_Pipe::MGPipeHandle handle, TextureUploadTarget target, Uint32 level,
                               const IntVec3& extent, VkFormat& format, Vector<Uint8>& bytes);
    TextureResource* SyncTextureResourceByHandle(MG_Pipe::MGPipeHandle handle, Bool renderbuffer = false,
                                                 Bool requireStorage = false);
    // Map a texture/view-relative subresource to its live storage owner. layerCount, when
    // supplied, receives the number of accessible layers starting at the input layer.
    // viewFormat is UNDEFINED for an ordinary texture, or the outer view's format.
    // outsideWindow, when supplied, says WHY a null came back: true only when every record on
    // the way was live and consistent and the (level, layer) simply is not there - past the
    // owner's levels or layers, or past a view's own window. A dead record, a missing or stale
    // view CSO and an unresolvable view format leave it false.
    MG_Pipe::MGPipeHandle ResolveWireTextureStorage(MG_Pipe::MGPipeHandle handle, Uint32& level,
        Uint32& layer, VkFormat* viewFormat = nullptr, Uint32* layerCount = nullptr,
        Bool* outsideWindow = nullptr);
    // Grows the live image of a handle-keyed resource to `requiredMipLevels`, carrying the
    // existing levels' content across with an in-command-buffer copy. The caller
    // (GenerateMipmap's record arm) has flushed every pending submission first, so the old
    // image is GPU-idle and the copy may be recorded into `commandBuffer` directly; the old
    // image goes to the deferred-release ring. False when the chain cannot grow (the resource
    // stays as it was).
    Bool GrowWireTextureMipChain(MG_Pipe::MGPipeHandle handle, Uint32 requiredMipLevels,
                                 VkCommandBuffer commandBuffer);
    // Records that GPU writes superseded the staged CPU snapshot of this level. Layer
    // ranges select cube faces; array and 3D levels share one staged level shadow.
    void MarkWireTextureGpuWritten(MG_Pipe::MGPipeHandle handle, Uint32 mipLevel,
                                    Uint32 baseArrayLayer = 0, Uint32 layerCount = 1);

    // SHARED IMAGES (docs/Disaggregated/notes/anland/plan-ahb-dmabuf.md): a server shared image's
    // AHardwareBuffer bound as a single-level 2D colour image on THIS session's device, on dedicated
    // memory imported from the buffer. Android only (false with `why` elsewhere, or when the device
    // did not take the AHardwareBuffer extension). The image is born UNDEFINED; whoever uses it
    // acquires it from the foreign queue family first, and a writer releases it back after.
    struct ImportedSharedImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageUsageFlags usage = 0;
        VkFormatFeatureFlags features = 0;
        Uint32 queueFamily = 0;
    };
    static Bool ImportSharedImage(void* nativeBuffer, Uint32 width, Uint32 height, ImportedSharedImage& out,
                                  String& why);
    static void DestroyImportedSharedImage(VkDevice device, ImportedSharedImage& image);
    // The reading side's per-use hook: PrepareWireTextureResources notes every shared-image
    // texture a draw or dispatch samples, and the renderer takes the list right after
    // (VulkanRenderer::AcquireNotedSharedImages) to acquire each one ahead of the sampling.
    void NoteSharedImageUse(const TextureResource& resource) {
        if (std::find(m_sharedImageUses.begin(), m_sharedImageUses.end(), resource.sharedImageKey) ==
            m_sharedImageUses.end())
            m_sharedImageUses.push_back(resource.sharedImageKey);
    }
    Bool HasNotedSharedImageUses() const { return !m_sharedImageUses.empty(); }
    void TakeNotedSharedImageUses(Vector<Uint64>& out) {
        out.clear();
        std::swap(out, m_sharedImageUses);
    }
    // The live handle-keyed texture resource under `key`, or null.
    TextureResource* FindWireTextureResource(Uint64 key) {
        const auto it = m_wireTextureResources.find(key);
        return it != m_wireTextureResources.end() && it->second.image != VK_NULL_HANDLE ? &it->second : nullptr;
    }
    // Recording-generation bookkeeping for the pre-pass command stream. The
    // generation advances every time the frame command buffer (re)begins
    // recording; a resource whose stamp does not match was not referenced by
    // any command in the open recording, so its out-of-pass work may safely
    // execute ahead of the whole recording (in the pre command buffer).
    void AdvanceRecordingGeneration() { ++m_recordingGeneration; }
    void StampResourceRecordingUse(TextureResource& resource) const {
        resource.lastRecordingGeneration = m_recordingGeneration;
    }
    Bool WasTouchedThisRecording(const TextureResource& resource) const {
        return resource.lastRecordingGeneration == m_recordingGeneration;
    }
    // A write is a use too. See TextureResource::lastRecordingWriteGeneration.
    void StampResourceRecordingWrite(TextureResource& resource) const {
        resource.lastRecordingGeneration = m_recordingGeneration;
        resource.lastRecordingWriteGeneration = m_recordingGeneration;
    }
    // `depthStencilTextureMode` is the texture's GL_DEPTH_STENCIL_TEXTURE_MODE; it only decides
    // anything for an image that carries both aspects. Defaulted so the call sites that have no
    // texture in hand keep the depth-aspect answer they have always given.
    static VkImageAspectFlags ResolveSampledImageViewAspectMask(VkImageAspectFlags imageAspect,
                                                                GLenum depthStencilTextureMode = GL_DEPTH_COMPONENT);
    static VkFormat ResolveSampledImageViewFormat(VkFormat imageFormat, SamplerNumericDomain numericDomain);
    static Bool AreSampledImageViewFormatsCompatible(VkFormat imageFormat, VkFormat viewFormat);
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

private:
    // Bumped in SyncTextureResource right after vmaCreateImage(texture). See GetTextureImageEpoch().
    Uint64 m_textureImageEpoch = 1;
    // See AdvanceRecordingGeneration. Starts above every resource's default
    // stamp of 0 so a fresh resource counts as untouched.
    Uint64 m_recordingGeneration = 1;

    static VkImageAspectFlags GetAspectMaskForFormat(VkFormat format);
    void DeferResourceRelease(TextureResource&& resource);
    void CollectDeferredReleases(Uint32 frameIndex);
    void DestroyDeferredReleases();
    // Frees the fence/command buffer/staging buffer of every in-flight texture
    // upload whose fence has signaled (submission order = completion order on
    // the single queue, so the scan stops at the first still-pending entry).
    // waitAll blocks on every entry - Shutdown's drain.
    void ReclaimCompletedUploads(Bool waitAll = false);
    SizeT PruneDeadTextures();
    // The two halves of SyncTextureResourceByHandle: shape from the resource record's
    // descriptor (create / recreate / compatibility), then the pending-upload walk against the
    // server's staged store. UploadPendingWireLevels mutates the applier's record (consumed
    // entries are dropped), which is why the record is a non-const reference.
    Bool SyncWireTextureShape(const MG_Pipe::MGPipeResourceRecord& record, TextureResource& resource,
                              Bool requireStorage);
    Bool UploadPendingWireLevels(MG_Pipe::MGPipeHandle handle, MG_Pipe::MGPipeResourceRecord& record,
                                 TextureResource& resource);
    // SyncWireTextureShape's arm for a record whose level 0 IS a shared image (SharedImageId):
    // binds the imported buffer instead of allocating. Unresolved = the image is gone or cannot be
    // imported here; `resource` is then untouched and the caller allocates as for any texture.
    enum class SharedImageBind : Uint8 { Bound, Failed, Unresolved };
    SharedImageBind SyncWireSharedImage(const MG_Pipe::MGPipeResourceRecord& record, TextureResource& resource,
                                        Bool requireStorage);
    // The handle-keyed twin of m_textureResources, keyed by StagedTextureStore::KeyForHandle.
    // Node-based for the same reason m_renderbufferResources is: callers hold TextureResource*
    // across map-touching calls.
    std::unordered_map<Uint64, TextureResource> m_wireTextureResources;
    // Shared-image textures sampled since the renderer last took the list (NoteSharedImageUse).
    Vector<Uint64> m_sharedImageUses;
    // Handles are per-kind; a renderbuffer and texture may have identical slot/gen.
    std::unordered_map<Uint64, TextureResource> m_wireRenderbufferResources;

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
    // SyncWireTextureShape defining the RGBA8 storage a YUV shared image is converted into.
    Bool m_syncingYuvTextureStorage = false;
    Uint64 m_resourceEraseEpoch = 1;
    // Formats whose mutable-image probe failed on this device; their images are created
    // without MUTABLE_FORMAT_BIT so repeat syncs neither re-probe nor flag-mismatch.
    std::unordered_set<VkFormat> m_mutableFormatUnsupported;
    // Formats whose 3D images refused VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT. Per format+usage,
    // exactly like the mutable-format verdict above, so it is answered at image creation and
    // remembered rather than probed once globally.
    std::unordered_set<VkFormat> m_2dArrayCompatibleUnsupported;
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
