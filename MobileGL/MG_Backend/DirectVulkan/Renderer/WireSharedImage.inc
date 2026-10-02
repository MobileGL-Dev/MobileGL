// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireSharedImage.inc
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// SHARED IMAGES, the present side (docs/Disaggregated/notes/anland/plan-ahb-dmabuf.md). Included by
// VulkanRenderer.cpp inside the backend namespace, after WireFramebuffer.inc, whose readback this
// copy is modelled on: the same flush, the same write-visibility barrier, the same transition of
// the default framebuffer's image and the same submit-and-wait.
//
// OWNERSHIP. The image lives on more than one device: this session writes it, the compositor's
// session (another VulkanRenderer, another VkDevice) samples it. Each present acquires it from
// VK_QUEUE_FAMILY_FOREIGN_EXT (UNDEFINED: the copy overwrites every texel) and releases it back in
// GENERAL, the layout the sampling side acquires it in (VkTextureManager::SyncWireSharedImage).
#if MOBILEGL_BUILD_DISAGGREGATED
    void VulkanRenderer::DestroySharedImagePresentTargets(Bool onlyDead) {
        for (auto it = m_sharedImagePresentTargets.begin(); it != m_sharedImagePresentTargets.end();) {
            if (onlyDead && !it->second.owner.expired()) {
                ++it;
                continue;
            }
            VkTextureManager::DestroyImportedSharedImage(m_device, it->second.image);
            it = m_sharedImagePresentTargets.erase(it);
        }
    }

    // A copy moves bytes; a blit converts through the format. The bytes are right only between
    // formats with one memory layout, which for the RGBA8 buffers means RGBA8 itself or its sRGB
    // twin (a blit would decode the sRGB bytes on the way).
    static Bool SharedImageCopiesAsIs(VkFormat source, VkFormat destination) {
        if (source == destination) return true;
        return destination == VK_FORMAT_R8G8B8A8_UNORM && source == VK_FORMAT_R8G8B8A8_SRGB;
    }

    Bool VulkanRenderer::BlitDefaultFramebufferToSharedImage(const SharedImageView& view) {
        if (m_device == VK_NULL_HANDLE || view.Width == 0 || view.Height == 0) return false;
        // Every earlier present waited for its copy, so an entry whose image has died is idle.
        DestroySharedImagePresentTargets(true);

        // The default framebuffer's image is stored in display orientation (row 0 is the top of
        // the frame) on an identity surface, and turned half round on a 180 one; a blit expresses
        // both. A quarter turn transposes, which no copy or blit can - and only the compositor's
        // own window is ever quarter-turned, which presents to the display rather than here.
        const VkSurfaceTransformFlagBitsKHR preTransform = m_swapchainObject.GetPreTransform();
        if (IsQuarterTurnPreTransform(preTransform)) {
            MGLOG_E_ONCE("Magma: shared image %llu: the current surface %llx is quarter-turned (transform %u); a "
                         "present into a shared image is declined there",
                         static_cast<unsigned long long>(view.Id), static_cast<unsigned long long>(m_activeTargetKey),
                         static_cast<Uint32>(preTransform));
            return false;
        }

        auto found = m_sharedImagePresentTargets.find(view.Id);
        if (found == m_sharedImagePresentTargets.end()) {
            const auto owner = MG_Remote::Server::SharedImages::Find(view.Id);
            if (owner == nullptr) {
                MGLOG_E_ONCE("Magma: shared image %llu is not live; the present is declined",
                             static_cast<unsigned long long>(view.Id));
                return false;
            }
            SharedImagePresentTarget target;
            String why;
            if (!VkTextureManager::ImportSharedImage(view.NativeBuffer, view.Width, view.Height, target.image, why)) {
                MGLOG_E_ONCE("Magma: shared image %llu (%ux%u) not imported - %s; the present is declined",
                             static_cast<unsigned long long>(view.Id), view.Width, view.Height, why.c_str());
                return false;
            }
            target.owner = owner;
            found = m_sharedImagePresentTargets.emplace(view.Id, Move(target)).first;
        }
        const VkTextureManager::ImportedSharedImage& destination = found->second.image;

        // The frame's draws reach the queue first; the copy below is a later submission that the
        // readback barrier orders behind them.
        if (HasPendingRecordedWork() && !FlushPendingCommands()) {
            MGLOG_E("Magma: shared image %llu: the frame's recorded work could not be submitted",
                    static_cast<unsigned long long>(view.Id));
            return false;
        }
        MG_Pipe::MGPFramebufferState defaultFramebuffer{};
        defaultFramebuffer.IsDefault = 1;
        const MG_Pipe::MGPSurface noSurface{};
        WireImage source = ResolveWireImage(defaultFramebuffer, noSurface, VK_IMAGE_ASPECT_COLOR_BIT,
                                            /*isWriteTarget=*/false);
        if (source.image == VK_NULL_HANDLE || source.extent.width == 0 || source.extent.height == 0) {
            MGLOG_E_ONCE("Magma: shared image %llu: no default framebuffer is current; the present is declined",
                         static_cast<unsigned long long>(view.Id));
            return false;
        }
        if (!m_swapchainObject.IsImageContentDefined(source.swapchainImageIndex)) {
            MGLOG_W_ONCE("Magma: shared image present: no write is recorded for image %u of the default "
                         "framebuffer; its raw pixels are copied",
                         source.swapchainImageIndex);
        }

        const Bool sameExtent = source.extent.width == view.Width && source.extent.height == view.Height;
        const Bool halfTurn = preTransform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR;
        const Bool copy = sameExtent && !halfTurn && SharedImageCopiesAsIs(source.format, destination.format);
        VkFormatProperties sourceProperties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice.handle, source.format, &sourceProperties);
        if (!copy) {
            if ((sourceProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0 ||
                (destination.features & VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0) {
                MGLOG_E_ONCE("Magma: shared image %llu: a %ux%u format-%d default framebuffer needs a blit into "
                             "the %ux%u format-%d image, which this device cannot do; the present is declined",
                             static_cast<unsigned long long>(view.Id), source.extent.width, source.extent.height,
                             static_cast<Int>(source.format), view.Width, view.Height,
                             static_cast<Int>(destination.format));
                return false;
            }
            if (vkuFormatIsSRGB(source.format) && !vkuFormatIsSRGB(destination.format)) {
                MGLOG_W_ONCE("Magma: shared image present: the sRGB default framebuffer is blitted into a UNORM "
                             "image, which stores linear values");
            }
        }

        m_textureManager->FlushPendingUploads();
        RecordWireReadbackWriteBarrier();
        TransitionWireImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        auto& frame = m_frameContext.GetCurrent();

        VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        acquire.srcAccessMask = 0;
        acquire.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        acquire.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        acquire.dstQueueFamilyIndex = destination.queueFamily;
        acquire.image = destination.image;
        acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &acquire);

        const VkImageSubresourceLayers layers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        if (copy) {
            VkImageCopy region{};
            region.srcSubresource = layers;
            region.dstSubresource = layers;
            region.extent = {view.Width, view.Height, 1};
            vkCmdCopyImage(frame.commandBuffer, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        } else {
            // Identity: row 0 is already the top. 180: the stored image is the frame turned half
            // round, so the destination rectangle runs backwards on both axes.
            const Int32 width = static_cast<Int32>(view.Width);
            const Int32 height = static_cast<Int32>(view.Height);
            VkImageBlit region{};
            region.srcSubresource = layers;
            region.srcOffsets[1] = {static_cast<Int32>(source.extent.width), static_cast<Int32>(source.extent.height),
                                    1};
            region.dstSubresource = layers;
            region.dstOffsets[0] = halfTurn ? VkOffset3D{width, height, 0} : VkOffset3D{0, 0, 0};
            region.dstOffsets[1] = halfTurn ? VkOffset3D{0, 0, 1} : VkOffset3D{width, height, 1};
            const VkFilter filter =
                !sameExtent && (sourceProperties.optimalTilingFeatures &
                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0
                    ? VK_FILTER_LINEAR
                    : VK_FILTER_NEAREST;
            vkCmdBlitImage(frame.commandBuffer, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, filter);
        }

        VkImageMemoryBarrier release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        release.dstAccessMask = 0;
        release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        release.srcQueueFamilyIndex = destination.queueFamily;
        release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        release.image = destination.image;
        release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &release);

        // The client attaches the buffer to its surface as soon as this returns, and nothing else
        // orders the compositor's read after the copy: wait for it here.
        if (!SubmitReadbackCommandsAndWait(frame)) {
            MGLOG_E("Magma: shared image %llu: the copy could not be submitted or waited for",
                    static_cast<unsigned long long>(view.Id));
            return false;
        }
        return true;
    }
#endif // MOBILEGL_BUILD_DISAGGREGATED
