// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/RenderPassGuard.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "../VkIncludes.h"
#include <Includes.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    // Ends the render pass that is active on `commandBuffer`, if there is one. Called right before
    // recording a command a render pass may not contain (a barrier, a copy, a blit, a clear of an
    // image) into a command buffer that may hold an open pass: the wire draw path keeps its pass
    // open across consecutive draws to the same attachments, so the resource work that falls
    // between two draws must close it first. A pass on another command buffer is left alone.
    void EndActiveRenderPassOn(VkCommandBuffer commandBuffer);

    // A counter that moves whenever images may have been written since it was last read: a render
    // pass ended (its attachments), resource work recorded outside a pass (copies, clears, blits,
    // dispatches all close the pass first), or a draw with storage images. A sampled image needs
    // the memory barrier that makes earlier writes visible only when this moved since the last one.
    Uint64 WireImageWriteEpoch();
    void BumpWireImageWriteEpoch();
} // namespace MobileGL::MG_Backend::DirectVulkan
