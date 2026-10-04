// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireSharedImage.inc
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// SHARED IMAGES (docs/Disaggregated/notes/anland/plan-ahb-dmabuf.md). Included by VulkanRenderer.cpp
// inside the backend namespace, after WireFramebuffer.inc, whose readback the present side's copy
// is modelled on: the same flush, the same write-visibility barrier, the same transition of the
// default framebuffer's image.
//
// OWNERSHIP. The image lives on more than one device: a client session writes it, the
// compositor's session (another VulkanRenderer, another VkDevice) samples it. Each present
// acquires it from VK_QUEUE_FAMILY_FOREIGN_EXT (UNDEFINED: the copy overwrites every texel) and
// releases it back in GENERAL. A reader acquires it (GENERAL) before the first use of a frame and
// again whenever the write generation moved, and releases it back at its frame boundary, so every
// acquire on either side matches a release on the other.
//
// ORDER (MG_Remote/Server/SharedImageRegistry.h). Every fence is a sync_file. The writer's copy
// waits on its GPU for the read fences still pending, signals a semaphore exported as the write
// fence, and the present returns without waiting. A reader's use waits on its GPU for the write
// fence of the generation it acquires; its frame's submission signals a semaphore exported as the
// read fence of every image the frame used. A device without SYNC_FD semaphore import and export
// waits on the CPU instead where it would have waited on the GPU - before the writer's copy, after
// it, and before the reader's use - and exports nothing for its reads: there is no fence to give,
// and waiting for the frame on the CPU only to say it is over would stall every frame of the
// compositor. The writer's next copy is then ordered after the reads by nothing but the client's
// own buffer rotation (a Wayland client writes a buffer only after its release) - which is all
// this path had before - on a device Android's Vulkan 1.1 requirements leave rare.
#if MOBILEGL_BUILD_DISAGGREGATED
    // A CPU wait on another session's fence is a fallback, and bounded: a peer that never signals
    // costs a frame's worth of stall, not a hang.
    static constexpr Uint32 kSharedImageCpuWaitMs = 1000;
    // Every stage a reader's use of a shared image can reach: sampling (draws and dispatches), a
    // blit or readback source, an attachment. The write fence's wait blocks these, and the
    // acquire's first scope is the same set, so it is ordered after that wait.
    static constexpr VkPipelineStageFlags kSharedImageUseStages =
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    static void CloseSyncFile(int fd) {
#if !defined(_WIN32)
        if (fd >= 0) ::close(fd);
#else
        (void)fd;
#endif
    }

    void VulkanRenderer::DestroySharedImagePresentTargets(Bool onlyDead) {
        if (onlyDead) RefreshCompletedSubmits();
        for (auto it = m_sharedImagePresentTargets.begin(); it != m_sharedImagePresentTargets.end();) {
            // A dead image's last copy may still be executing: the present no longer waits for it.
            if (onlyDead && (!it->second.owner.expired() || it->second.lastSubmit > m_completedSubmitCounter)) {
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

    Bool VulkanRenderer::BlitDefaultFramebufferToSharedImage(const SharedImageView& view,
                                                             const MG_Util::Damage::Region& region) {
        if (m_device == VK_NULL_HANDLE || view.Width == 0 || view.Height == 0) return false;
        // The debug knob's device check (MGPipeDebugDeviceLossDue) rides the frame boundary here.
        if (MG_Pipe::MGPipeDebugDeviceLossDue()) NoteDeviceLoss(VK_ERROR_DEVICE_LOST, "the debug knob (shared-image present)");
        if (m_deviceLost && LatchWireDeviceLoss("shared-image-present")) return false;
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
        // The registry's record carries the image's fences. The server holds a reference for as
        // long as the present runs; a lapsed one is an image released under it.
        const auto image =
            std::static_pointer_cast<const MG_Remote::Server::SharedImages::Image>(found->second.owner.lock());
        if (image == nullptr) {
            MGLOG_E_ONCE("Magma: shared image %llu is not live; the present is declined",
                         static_cast<unsigned long long>(view.Id));
            return false;
        }

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
        // ONLY WHERE THE IMAGE DIFFERS FROM THE FRAME: an image this renderer wrote before, copied
        // one to one (no scaling, no half turn). Its other texels must survive, so the acquire then
        // keeps the layout the last reader released it in rather than discarding it.
        MG_Util::Damage::Region part = region;
        if (!part.IsFull()) part.Normalize(static_cast<Int32>(view.Width), static_cast<Int32>(view.Height));
        const Bool partial = !part.IsFull() && found->second.written && sameExtent && !halfTurn;
        // The region's rectangles, top row first and made disjoint: a copy's regions may not overlap.
        Vector<MG_Util::Damage::Rect> rows;
        if (partial) {
            for (const MG_Util::Damage::Rect& rect : MG_Util::Damage::Disjoint(part.Rects()))
                rows.push_back(MG_Util::Damage::FlipY(rect, static_cast<Int32>(view.Height)));
        }
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

        // NOT OVER WHAT IS STILL READ. The reads published since this image's last write (a
        // compositor frame sampling it) end before the copy starts: on the GPU, as waits of the
        // submission below at the transfer stage; on the CPU, where they cannot be imported.
        // The frame's draws were flushed above, so the next submission is the copy's.
        namespace SI = MG_Remote::Server::SharedImages;
        if (m_sharedImageSyncFd) {
            for (const int fence : SI::DupPendingReadFences(*image))
                StageSharedImageWait(fence, VK_PIPELINE_STAGE_TRANSFER_BIT);
        } else if (!SI::WaitForReads(*image, kSharedImageCpuWaitMs)) {
            MGLOG_W_ONCE("Magma: shared image %llu: a read of the previous frame did not finish within %u ms; it is "
                         "overwritten anyway",
                         static_cast<unsigned long long>(view.Id), kSharedImageCpuWaitMs);
        }

        // The acquire's layout transition runs after those waits: its first scope is the stage
        // they block.
        VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        acquire.srcAccessMask = 0;
        acquire.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        // Every present releases the image in GENERAL, and so does every reader.
        acquire.oldLayout = partial ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        acquire.dstQueueFamilyIndex = destination.queueFamily;
        acquire.image = destination.image;
        acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &acquire);

        const VkImageSubresourceLayers layers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        if (partial) {
            // An empty region copies nothing: the image already is the frame.
            if (copy && !rows.empty()) {
                Vector<VkImageCopy> regions;
                regions.reserve(rows.size());
                for (const MG_Util::Damage::Rect& row : rows) {
                    VkImageCopy one{};
                    one.srcSubresource = layers;
                    one.dstSubresource = layers;
                    one.srcOffset = {row.X, row.Y, 0};
                    one.dstOffset = {row.X, row.Y, 0};
                    one.extent = {static_cast<Uint32>(row.Width), static_cast<Uint32>(row.Height), 1};
                    regions.push_back(one);
                }
                vkCmdCopyImage(frame.commandBuffer, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<Uint32>(regions.size()), regions.data());
            } else if (!rows.empty()) {
                Vector<VkImageBlit> regions;
                regions.reserve(rows.size());
                for (const MG_Util::Damage::Rect& row : rows) {
                    VkImageBlit one{};
                    one.srcSubresource = layers;
                    one.dstSubresource = layers;
                    one.srcOffsets[0] = {row.X, row.Y, 0};
                    one.srcOffsets[1] = {row.X + row.Width, row.Y + row.Height, 1};
                    one.dstOffsets[0] = one.srcOffsets[0];
                    one.dstOffsets[1] = one.srcOffsets[1];
                    regions.push_back(one);
                }
                vkCmdBlitImage(frame.commandBuffer, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<Uint32>(regions.size()), regions.data(), VK_FILTER_NEAREST);
            }
        } else if (copy) {
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

        // THE WRITE FENCE. The client attaches the buffer to its surface as soon as this returns;
        // what orders the compositor's read after the copy is the fence published here, which the
        // compositor's session waits for on its GPU when it next acquires the image. The copy's
        // submission signals a semaphore exported as that fence, and nothing waits for it here.
        // The submission is an ordinary pooled-fence flush: the renderer's in-flight tracking keeps
        // the command buffer and every resource it names alive until it has completed.
        if (m_sharedImageSyncFd && m_sharedImageSignal == VK_NULL_HANDLE)
            m_sharedImageSignal = TakeSharedImageSemaphore(/*exportable=*/true);
        if (m_sharedImageSignal != VK_NULL_HANDLE) {
            m_sharedImageSignalSubmitted = VK_NULL_HANDLE;
            if (!FlushPendingCommands()) {
                MGLOG_E("Magma: shared image %llu: the copy could not be submitted",
                        static_cast<unsigned long long>(view.Id));
                return false;
            }
            found->second.lastSubmit = m_submitCounter;
            found->second.written = true;
            const VkSemaphore signal = std::exchange(m_sharedImageSignalSubmitted, VK_NULL_HANDLE);
            int fence = -1;
            if (signal != VK_NULL_HANDLE && ExportSharedImageSignal(signal, fence)) {
                SI::PublishWrite(*image, fence);
                return true;
            }
            // Submitted, but no fence came out of it: the copy is waited for, as on a device that
            // exports none.
            if (!WaitForSubmitsUpTo(m_submitCounter, UINT64_MAX)) {
                MGLOG_E("Magma: shared image %llu: the copy could not be waited for",
                        static_cast<unsigned long long>(view.Id));
                return false;
            }
            SI::PublishWrite(*image, -1);
            return true;
        }
        if (!SubmitReadbackCommandsAndWait(frame)) {
            MGLOG_E("Magma: shared image %llu: the copy could not be submitted or waited for",
                    static_cast<unsigned long long>(view.Id));
            return false;
        }
        found->second.lastSubmit = m_submitCounter;
        found->second.written = true;
        SI::PublishWrite(*image, -1);
        return true;
    }

    // ---- fences as semaphores ------------------------------------------------------------------

    VkSemaphore VulkanRenderer::TakeSharedImageSemaphore(Bool exportable) {
        auto& pool = exportable ? m_freeSharedImageSignalSemaphores : m_freeSharedImageWaitSemaphores;
        if (!pool.empty()) {
            const VkSemaphore semaphore = pool.back();
            pool.pop_back();
            return semaphore;
        }
        VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (exportable) info.pNext = &exportInfo;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        const VkResult result = vkCreateSemaphore(m_device, &info, nullptr, &semaphore);
        if (result != VK_SUCCESS) {
            MGLOG_E_ONCE("Magma: a shared-image semaphore could not be created (vkCreateSemaphore -> %d)",
                         static_cast<Int>(result));
            return VK_NULL_HANDLE;
        }
        return semaphore;
    }

    void VulkanRenderer::StageSharedImageWait(int fence, VkPipelineStageFlags stages) {
        if (fence < 0) return;
        if (m_sharedImageSyncFd) {
            const VkSemaphore semaphore = TakeSharedImageSemaphore(/*exportable=*/false);
            if (semaphore != VK_NULL_HANDLE) {
                // A sync_file import is always temporary: the wait consumes it, and the semaphore
                // is a plain unsignaled one again once that wait has executed.
                VkImportSemaphoreFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
                import.semaphore = semaphore;
                import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
                import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
                import.fd = fence;
                const VkResult result = m_importSemaphoreFd(m_device, &import);
                if (result == VK_SUCCESS) {
                    m_sharedImageWaits.push_back({semaphore, stages}); // the import owns `fence` now
                    return;
                }
                m_freeSharedImageWaitSemaphores.push_back(semaphore);
                MGLOG_W_ONCE("Magma: a shared-image fence could not be imported (vkImportSemaphoreFdKHR -> %d); it "
                             "is waited for on the CPU",
                             static_cast<Int>(result));
            }
        }
        // Nothing recorded since reaches the queue before this returns.
        if (!MG_Remote::Server::SharedImages::SyncFile::Wait(fence, kSharedImageCpuWaitMs)) {
            MGLOG_W_ONCE("Magma: a shared-image fence did not signal within %u ms; the image is used anyway",
                         kSharedImageCpuWaitMs);
        }
        CloseSyncFile(fence);
    }

    Bool VulkanRenderer::ExportSharedImageSignal(VkSemaphore semaphore, int& fence) {
        fence = -1;
        VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
        info.semaphore = semaphore;
        info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        // A sync_file export has a wait's effect on the semaphore: it is unsignaled again, and
        // reusable once the submission that signaled it has completed. -1 back means it had
        // already signaled.
        const VkResult result = m_getSemaphoreFd(m_device, &info, &fence);
        if (result != VK_SUCCESS) {
            fence = -1;
            MGLOG_W_ONCE("Magma: a shared-image fence could not be exported (vkGetSemaphoreFdKHR -> %d)",
                         static_cast<Int>(result));
            return false;
        }
        for (auto& entry : m_sharedImageSemaphoresInFlight) {
            if (entry.semaphore == semaphore) entry.exported = true;
        }
        return true;
    }

    void VulkanRenderer::AttachSharedImageSync(VkSubmitInfo& info, SharedImageSubmitSync& storage) {
        storage.attached = false;
        if (m_sharedImageWaits.empty() && m_sharedImageSignal == VK_NULL_HANDLE) return;
        storage.waits.assign(info.pWaitSemaphores, info.pWaitSemaphores + info.waitSemaphoreCount);
        storage.stages.assign(info.pWaitDstStageMask, info.pWaitDstStageMask + info.waitSemaphoreCount);
        for (const auto& wait : m_sharedImageWaits) {
            storage.waits.push_back(wait.semaphore);
            storage.stages.push_back(wait.stages);
        }
        storage.signals.assign(info.pSignalSemaphores, info.pSignalSemaphores + info.signalSemaphoreCount);
        if (m_sharedImageSignal != VK_NULL_HANDLE) storage.signals.push_back(m_sharedImageSignal);
        info.waitSemaphoreCount = static_cast<Uint32>(storage.waits.size());
        info.pWaitSemaphores = storage.waits.data();
        info.pWaitDstStageMask = storage.stages.data();
        info.signalSemaphoreCount = static_cast<Uint32>(storage.signals.size());
        info.pSignalSemaphores = storage.signals.data();
        storage.attached = true;
    }

    void VulkanRenderer::CommitSharedImageSync(const SharedImageSubmitSync& storage) {
        if (!storage.attached) return;
        for (const auto& wait : m_sharedImageWaits)
            m_sharedImageSemaphoresInFlight.push_back({wait.semaphore, m_submitCounter, false, false});
        m_sharedImageWaits.clear();
        if (m_sharedImageSignal != VK_NULL_HANDLE) {
            m_sharedImageSemaphoresInFlight.push_back({m_sharedImageSignal, m_submitCounter, true, false});
            m_sharedImageSignalSubmitted = std::exchange(m_sharedImageSignal, VK_NULL_HANDLE);
        }
    }

    void VulkanRenderer::RecycleSharedImageSemaphores(Uint64 completedSubmit) {
        SizeT done = 0;
        for (; done < m_sharedImageSemaphoresInFlight.size() &&
               m_sharedImageSemaphoresInFlight[done].submitIndex <= completedSubmit;
             ++done) {
            const auto& entry = m_sharedImageSemaphoresInFlight[done];
            if (entry.semaphore == m_sharedImageSignalSubmitted) m_sharedImageSignalSubmitted = VK_NULL_HANDLE;
            if (!entry.exportable) {
                m_freeSharedImageWaitSemaphores.push_back(entry.semaphore);
            } else if (entry.exported) {
                m_freeSharedImageSignalSemaphores.push_back(entry.semaphore);
            } else if (m_device != VK_NULL_HANDLE) {
                // Signaled and never exported, so still signaled: a binary semaphore cannot be
                // signaled again until something waits on it, and nothing will.
                vkDestroySemaphore(m_device, entry.semaphore, nullptr);
            }
        }
        if (done != 0) {
            m_sharedImageSemaphoresInFlight.erase(
                m_sharedImageSemaphoresInFlight.begin(),
                m_sharedImageSemaphoresInFlight.begin() + static_cast<std::ptrdiff_t>(done));
        }
    }

    void VulkanRenderer::DestroySharedImageSync() {
        // The device is idle: every semaphore is inert.
        const auto destroy = [this](VkSemaphore semaphore) {
            if (semaphore != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
                vkDestroySemaphore(m_device, semaphore, nullptr);
        };
        for (const auto& entry : m_sharedImageSemaphoresInFlight) destroy(entry.semaphore);
        m_sharedImageSemaphoresInFlight.clear();
        for (const auto& wait : m_sharedImageWaits) destroy(wait.semaphore);
        m_sharedImageWaits.clear();
        destroy(std::exchange(m_sharedImageSignal, VK_NULL_HANDLE));
        m_sharedImageSignalSubmitted = VK_NULL_HANDLE;
        for (const VkSemaphore semaphore : m_freeSharedImageWaitSemaphores) destroy(semaphore);
        m_freeSharedImageWaitSemaphores.clear();
        for (const VkSemaphore semaphore : m_freeSharedImageSignalSemaphores) destroy(semaphore);
        m_freeSharedImageSignalSemaphores.clear();
        m_sharedImagesHeld.clear();
        m_sharedImageReads.PublishFrame(-1); // drops the frame's references; publishes nothing
    }

    // ---- the reading side ------------------------------------------------------------------------
    //
    // REACHED ON EVERY USE. A wire draw or dispatch prepares its sampled textures through
    // PrepareWireTextureResources on every call - the wire arm keeps no sampled-set cache across
    // draws (the walk-skip is the monolith arm's SetupDraw) - and that notes each shared-image
    // texture it resolves; SetupWireDraw and both dispatch paths acquire the noted ones straight
    // after (AcquireNotedSharedImages). ResolveWireImage, through which every attachment, blit,
    // copy and readback reaches a texture's image, acquires directly. The test is the write
    // generation against the one last acquired, so a generation that moves between two draws of
    // one frame is caught at the second.

    void VulkanRenderer::AcquireNotedSharedImagesSlow() {
        m_textureManager->TakeNotedSharedImageUses(m_sharedImageUseScratch);
        for (const Uint64 key : m_sharedImageUseScratch) {
            if (auto* resource = m_textureManager->FindWireTextureResource(key)) AcquireSharedImage(*resource);
        }
        m_sharedImageUseScratch.clear();
    }

    void VulkanRenderer::AcquireSharedImage(VkTextureManager::TextureResource& resource) {
        namespace SI = MG_Remote::Server::SharedImages;
        // A YUV image is converted into the texture's own image instead (WireYuvImage.inc).
        if (resource.yuvImageId != 0) {
            (void)ConvertYuvSharedImage(resource);
            return;
        }
        if (resource.sharedImageId == 0 || resource.image == VK_NULL_HANDLE || resource.sharedImageOwner == nullptr)
            return;
        const auto image = std::static_pointer_cast<const SI::Image>(resource.sharedImageOwner);
        Uint64 generation = 0;
        const int fence = SI::DupWriteFence(*image, &generation);
        // Held, and not written since it was acquired: every use after a frame's first.
        if (resource.sharedImageHeld && generation == resource.sharedImageGeneration) {
            CloseSyncFile(fence);
            return;
        }

        // Out of any render pass, in the recording the use goes into (or one ahead of it).
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) m_frameContext.BeginCommandRecording();
        if (VkRenderPassManager::GetActiveRenderPass()) VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        // The write's fence gates the submission carrying the acquire: this recording's, or an
        // earlier one if a flush takes it first - either way ahead of every later command.
        StageSharedImageWait(fence, kSharedImageUseStages);
        // A session that writes images (implicit sync by flush) may be about to overwrite what
        // another session still samples: those reads go first.
        if (m_sharedImageImplicitSync && !resource.sharedImageHeld) {
            for (const int read : SI::DupPendingReadFences(*image)) StageSharedImageWait(read, kSharedImageUseStages);
        }

        const Uint32 family = static_cast<Uint32>(m_physicalDevice.queueFamilies.graphicsFamily);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.image = resource.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkPipelineStageFlags acquireAfter = kSharedImageUseStages;
        if (resource.sharedImageHeld) {
            // Written again while this frame holds it - a buffer reused before the compositor let
            // it go. It is handed back first, so the acquire below matches a release.
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = 0;
            barrier.oldLayout = resource.layout;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = family;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
            vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            acquireAfter = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
        // GENERAL: the layout the writer releases in, and the one every wire descriptor samples
        // in. An image no present has written yet (generation 0) has nothing to keep.
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout = generation == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        barrier.dstQueueFamilyIndex = family;
        vkCmdPipelineBarrier(frame.commandBuffer, acquireAfter, kSharedImageUseStages, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
        // The open recording names the image now: its out-of-pass work may not move ahead of it.
        m_textureManager->StampResourceRecordingUse(resource);
        resource.layout = VK_IMAGE_LAYOUT_GENERAL;
        resource.sharedImageGeneration = generation;
        if (!resource.sharedImageHeld) {
            resource.sharedImageHeld = true;
            // The current recording is the next submission (RetireWireObjects' rule).
            m_sharedImagesHeld.push_back({resource.sharedImageKey, m_submitCounter + 1});
        }
        m_sharedImageReads.NoteRead(image);
    }

    // THE FRAME BOUNDARY (Present). Every image this frame acquired goes back to the foreign
    // family in the frame's last recording, and that recording's submission signals the semaphore
    // whose sync_file is published as the frame's read fence for all of them at once.
    void VulkanRenderer::ReleaseHeldSharedImages() {
        // A frame boundary: a YUV texture's next use converts its image again.
        ++m_yuvFrameSerial;
        if (m_sharedImagesHeld.empty()) return;
        const Uint32 family = static_cast<Uint32>(m_physicalDevice.queueFamilies.graphicsFamily);
        Vector<VkImageMemoryBarrier> releases;
        releases.reserve(m_sharedImagesHeld.size());
        for (const auto& hold : m_sharedImagesHeld) {
            // Gone or re-imaged since: a destroyed image needs no release, and a replacement
            // acquired this frame has an entry of its own.
            auto* resource = m_textureManager->FindWireTextureResource(hold.key);
            if (resource == nullptr || !resource->sharedImageHeld) continue;
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = 0;
            barrier.oldLayout = resource->layout;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = family;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
            barrier.image = resource->image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            releases.push_back(barrier);
            resource->sharedImageHeld = false;
            resource->layout = VK_IMAGE_LAYOUT_GENERAL;
            m_textureManager->StampResourceRecordingUse(*resource);
        }
        m_sharedImagesHeld.clear();
        if (releases.empty()) return;
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) m_frameContext.BeginCommandRecording();
        if (VkRenderPassManager::GetActiveRenderPass()) VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                             static_cast<Uint32>(releases.size()), releases.data());
    }

    Bool VulkanRenderer::PublishSharedImageAccesses() {
        m_sharedImageImplicitSync = true;
        if (m_sharedImageReads.Pending() == 0 && m_sharedImagesHeld.empty()) return true;
        ReleaseHeldSharedImages();
        if (m_sharedImageSyncFd && m_sharedImageSignal == VK_NULL_HANDLE)
            m_sharedImageSignal = TakeSharedImageSemaphore(/*exportable=*/true);
        m_sharedImageSignalSubmitted = VK_NULL_HANDLE;
        if (!FlushPendingCommands()) {
            MGLOG_E_ONCE("Magma: a flush's shared-image accesses could not be submitted");
            m_sharedImageReads.PublishFrame(-1); // drops the references; nothing to order against
            return false;
        }
        const VkSemaphore signal = std::exchange(m_sharedImageSignalSubmitted, VK_NULL_HANDLE);
        int fence = -1;
        if (signal == VK_NULL_HANDLE || !ExportSharedImageSignal(signal, fence)) {
            // No fence came out of it: the submission is waited for and published as completed.
            fence = -1;
            if (!WaitForSubmitsUpTo(m_submitCounter, UINT64_MAX)) {
                MGLOG_E_ONCE("Magma: a flush's shared-image accesses could not be waited for");
            }
        }
        m_sharedImageReads.PublishFrameAsWrite(fence);
        return true;
    }

    // THE NATIVE FENCE. A sync_file of everything the session submitted so far, for the client's
    // EGL_ANDROID_native_fence_sync: the application hands the descriptor on (Chrome's GPU process,
    // to the browser that commits the buffer), so it has to signal no earlier than the last of that
    // work. With nothing recorded, an empty
    // recording is submitted to carry the signal: the in-order queue runs it after every earlier
    // submission, and it is tracked like any other (the semaphore's reuse waits for it).
    Bool VulkanRenderer::ExportNativeFence(int* fence) {
        *fence = -1;
        // Every fence the application asks for ends one of its frames as far as a YUV texture is
        // concerned (an offscreen producer - a browser's GPU process - has no present to do it).
        ++m_yuvFrameSerial;
        if (m_device == VK_NULL_HANDLE || m_graphicsQueue == VK_NULL_HANDLE || m_frameContext.GetFrameCount() == 0)
            return true; // nothing was ever submitted: nothing to fence
        // A session that writes images ends its frame here, as at a flush; a compositor that only
        // samples them keeps its reads for its present, which follows the fence's work anyway.
        const Bool boundary = m_sharedImageImplicitSync;
        if (boundary) ReleaseHeldSharedImages();
        if (m_sharedImageSyncFd && m_sharedImageSignal == VK_NULL_HANDLE)
            m_sharedImageSignal = TakeSharedImageSemaphore(/*exportable=*/true);
        m_sharedImageSignalSubmitted = VK_NULL_HANDLE;
        if (!HasPendingRecordedWork() && m_sharedImageSignal != VK_NULL_HANDLE)
            m_frameContext.BeginCommandRecording();
        const Bool submitted = HasPendingRecordedWork() && FlushPendingCommands();
        if (HasPendingRecordedWork()) MGLOG_E_ONCE("Magma: a native fence's commands could not be submitted");
        const VkSemaphore signal =
            submitted ? std::exchange(m_sharedImageSignalSubmitted, VK_NULL_HANDLE) : VK_NULL_HANDLE;
        int exported = -1;
        if (signal == VK_NULL_HANDLE || !ExportSharedImageSignal(signal, exported)) exported = -1;
        if (exported < 0) {
            // No descriptor: the work is waited out, so the caller may report it completed.
            if (!WaitForSubmitsUpTo(m_submitCounter, UINT64_MAX))
                MGLOG_E_ONCE("Magma: a native fence's work could not be waited for");
        }
        if (boundary && m_sharedImageReads.Pending() != 0) {
            int published = -1;
#if !defined(_WIN32)
            if (exported >= 0) published = ::dup(exported);
#endif
            m_sharedImageReads.PublishFrameAsWrite(published);
        }
        *fence = exported;
        return true;
    }

    void VulkanRenderer::PublishSharedImageReads() {
        const VkSemaphore signal = std::exchange(m_sharedImageSignalSubmitted, VK_NULL_HANDLE);
        int fence = -1;
        // An export also unsignals the semaphore, which is what lets it be signaled again: a
        // submitted signal is exported even when nothing is left to publish it to.
        if (signal != VK_NULL_HANDLE && !ExportSharedImageSignal(signal, fence)) fence = -1;
        if (m_sharedImageReads.Pending() == 0) {
            CloseSyncFile(fence);
            return;
        }
        // No fence (no export on this device): the frame's reads go unpublished - the file
        // comment says why that is the choice.
        // A session that writes images publishes every boundary as a write (PublishSharedImageAccesses).
        if (m_sharedImageImplicitSync)
            m_sharedImageReads.PublishFrameAsWrite(fence);
        else
            m_sharedImageReads.PublishFrame(fence);
    }

    void VulkanRenderer::AbandonSharedImageReads() {
        // Acquires recorded into the dropped recording never ran, so those images are not held;
        // ones an earlier flush submitted are, and the next frame boundary releases them.
        SizeT kept = 0;
        for (const auto& hold : m_sharedImagesHeld) {
            if (hold.acquireSubmit <= m_submitCounter) {
                m_sharedImagesHeld[kept++] = hold;
                continue;
            }
            if (auto* resource = m_textureManager->FindWireTextureResource(hold.key))
                resource->sharedImageHeld = false;
        }
        m_sharedImagesHeld.resize(kept);
        // Its waits and signal were never submitted. A wait semaphore's unconsumed temporary
        // payload is replaced by its next import; the signal semaphore was never signaled.
        for (const auto& wait : m_sharedImageWaits) m_freeSharedImageWaitSemaphores.push_back(wait.semaphore);
        m_sharedImageWaits.clear();
        if (m_sharedImageSignal != VK_NULL_HANDLE)
            m_freeSharedImageSignalSemaphores.push_back(std::exchange(m_sharedImageSignal, VK_NULL_HANDLE));
    }
#endif // MOBILEGL_BUILD_DISAGGREGATED
