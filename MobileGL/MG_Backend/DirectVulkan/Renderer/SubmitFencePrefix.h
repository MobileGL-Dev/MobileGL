// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/SubmitFencePrefix.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include <cstdint>
#include <vector>

namespace MobileGL::MG_Backend::DirectVulkan {

// Collect every fence in the ordered submission prefix. Waiting on only the last matching
// frame-serial record is insufficient: the same serial may have an earlier in-flight submit.
template <typename Records, typename Fence>
bool CollectSubmitFencePrefix(const Records& records, std::uint64_t submitIndex,
                              Fence nullFence, std::vector<Fence>& fences) {
    fences.clear();
    bool foundTarget = false;
    for (const auto& record : records) {
        if (record.submitIndex > submitIndex) {
            break;
        }
        if (record.fence == nullFence) {
            fences.clear();
            return false;
        }
        fences.push_back(record.fence);
        foundTarget = foundTarget || record.submitIndex == submitIndex;
    }
    if (!foundTarget) {
        fences.clear();
        return false;
    }
    return true;
}

} // namespace MobileGL::MG_Backend::DirectVulkan
