// MobileGL - MobileGL/MG_Backend/DirectVulkan/DescriptorPoolCensus.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include <cstdint>

namespace MobileGL::MG_Backend::DirectVulkan {
    // A count of Magma's per-frame descriptor pools and the sets they hold (UniformManager), for
    // tests. Dependency-free on purpose: the integration scenarios include it next to the system GL
    // headers. They link the static library on desktop; the shipping shared library is built with
    // hidden visibility, so on Android nothing outside the library can call GetDescriptorPoolCensus.
    struct DescriptorPoolCensus {
        bool available = false;         // false when no Magma renderer is live (e.g. the DirectGLES backend)
        uint32_t frameSlots = 0;
        uint32_t pools = 0;             // descriptor pools across every frame slot
        uint32_t maxPoolsInOneSlot = 0;
        uint64_t capacitySets = 0;      // sum of maxSets over those pools
        uint64_t cachedSets = 0;        // sets allocated from them and held in the per-layout caches
        uint64_t slotTrims = 0;         // whole-slot trims since the renderer came up
    };

    DescriptorPoolCensus GetDescriptorPoolCensus();
} // namespace MobileGL::MG_Backend::DirectVulkan
