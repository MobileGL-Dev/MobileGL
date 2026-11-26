// MobileGL - MobileGL/MG_Util/Texture/TextureUploadBoxes.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace MobileGL::MG_Util::TextureUploadBoxes {
    constexpr std::size_t MaxRegions = 64;
    constexpr std::uint64_t MinSavedTexels = 16 * 1024 / 4;

    struct Box {
        int left, top, right, bottom;

        std::uint64_t Texels() const { return static_cast<std::uint64_t>(right - left) * (bottom - top); }
    };

    inline Box Union(Box a, Box b) {
        return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right),
                std::max(a.bottom, b.bottom)};
    }

    // A second upload job is worth taking only for separated groups with a
    // substantial absolute and relative reduction. Small mip levels keep one job.
    inline bool WorthSearching(Box bounds, std::size_t count) {
        return count >= 2 && count <= MaxRegions && bounds.Texels() > MinSavedTexels;
    }

    // Every input rectangle belongs to one group. Try cuts on both axes and
    // keep the least expensive pair of non-overlapping bounding boxes.
    inline std::optional<std::array<Box, 2>> Split(Box bounds, std::array<Box, MaxRegions>& regions,
                                                   std::size_t count) {
        if (!WorthSearching(bounds, count)) return std::nullopt;
        const auto totalTexels = bounds.Texels();
        auto bestTexels = std::min(totalTexels / 2, totalTexels - MinSavedTexels);
        std::optional<std::array<Box, 2>> best;
        std::array<Box, MaxRegions> suffix;
        for (bool horizontal : {true, false}) {
            std::sort(regions.begin(), regions.begin() + count,
                      [horizontal](Box a, Box b) { return horizontal ? a.left < b.left : a.top < b.top; });
            suffix[count - 1] = regions[count - 1];
            for (std::size_t i = count - 1; i > 1; --i) {
                suffix[i - 1] = Union(regions[i - 1], suffix[i]);
            }
            Box prefix = regions[0];
            for (std::size_t i = 1; i < count; ++i) {
                const Box rest = suffix[i];
                const bool separated = horizontal ? prefix.right < rest.left : prefix.bottom < rest.top;
                const auto texels = prefix.Texels() + rest.Texels();
                if (separated && texels <= bestTexels) {
                    bestTexels = texels;
                    best = std::array<Box, 2>{prefix, rest};
                }
                prefix = Union(prefix, regions[i]);
            }
        }
        return best;
    }
} // namespace MobileGL::MG_Util::TextureUploadBoxes
