// MobileGL - MobileGL/MG_Impl/GLImpl/Texture/MipmapGenerationPlan.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
#pragma once
#include <Includes.h>

// The plan is the GL call's own window, not a transport's, so both arms of
// EnsureGeneratedMipmapStorageAllocated compute it here and the wire carrier
// (MGPMipPlan::LevelCount) publishes the same end-exclusive number.
#include <MG_State/GLState/TextureState/TextureObject.h>
#include <algorithm>

namespace MobileGL::MG_Impl::GLImpl {
    struct MipmapGenerationRange {
        Uint Base = 0;
        Uint End = 0; // logical end-exclusive, including the source base level
    };

    inline MipmapGenerationRange ComputeMipmapGenerationRange(
        const MG_State::GLState::TextureObjectMipmap& texture, TextureUploadTarget uploadTarget) {
        const auto range = texture.GetLevelRange();
        MipmapGenerationRange result{range.x(), range.x()};
        const Uint available = texture.GetMipmapLevelCount();
        if (result.Base >= available || range.y() < result.Base) return result;
        const auto extent = texture.GetMipmapTexelSize(uploadTarget, result.Base);
        if (extent.x() <= 0 || extent.y() <= 0 || extent.z() <= 0) return result;
        Int largest = extent.x();
        if (texture.GetTarget() != TextureTarget::Texture1D &&
            texture.GetTarget() != TextureTarget::Texture1DArray)
            largest = std::max(largest, extent.y());
        if (texture.GetTarget() == TextureTarget::Texture3D) largest = std::max(largest, extent.z());
        Uint levels = 1;
        while (largest > 1) { largest /= 2; ++levels; }
        Uint64 end = std::min(static_cast<Uint64>(result.Base) + levels,
                             static_cast<Uint64>(range.y()) + 1);
        // A view's level count is already relative to its own window. Neither
        // immutable storage nor a view may acquire levels beyond that allocation.
        if (texture.IsImmutable()) end = std::min(end, static_cast<Uint64>(available));
        result.End = static_cast<Uint>(end);
        return result;
    }
}
