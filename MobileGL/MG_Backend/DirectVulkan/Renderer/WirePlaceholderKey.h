// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WirePlaceholderKey.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
#pragma once

#include <Includes.h>

// The pure half of UniformManager's wire placeholder cache (WirePlaceholderImages.inc): WHICH
// placeholder a binding with no usable image takes. A unit test pins it
// (MG_Test/Pipeline/PipelineQuirkTest.cpp); the Vulkan half that makes the image stays in the .inc.
#if MOBILEGL_BUILD_DISAGGREGATED
#include <MG_Pipe/MGPipeTypes.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    // The placeholder's SHAPE (format, target, storage, shadow) and, for a STORAGE placeholder on
    // the arm without a null descriptor, the image UNIT it is private to. GL aliases per UNIT, so the
    // unit is the whole of the no-alias guarantee (codex closeout finding 2): two bindings that name
    // one unit are one GL image, and keying them apart as well only multiplied allocations (cf-magma
    // critic). A sampled placeholder is never written, so it stays shared: its unit is kShared.
    struct WirePlaceholderKey {
        static constexpr Uint32 kShared = ~0u;
        Uint64 shape = 0;
        Uint32 unit = kShared;
        Bool operator==(const WirePlaceholderKey& other) const { return shape == other.shape && unit == other.unit; }
    };

    struct WirePlaceholderKeyHash {
        SizeT operator()(const WirePlaceholderKey& key) const {
            Uint64 h = key.shape * 0x9E3779B97F4A7C15ull;
            h ^= static_cast<Uint64>(key.unit) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            return static_cast<SizeT>(h);
        }
    };

    // Every private placeholder is its own vkCreateImage + vkAllocateMemory, freed only at Shutdown.
    // (shape, unit) bounds them by shapes x kMGPipeMaxImageUnits, and a peer rotating formats and
    // targets across the units could still walk that towards maxMemoryAllocationCount (4096 is the
    // spec minimum). So past this many, a NEW (shape, unit) takes its shape's SHARED placeholder - the
    // pre-fix aliasing, logged once by the caller - instead of another allocation; a unit that already
    // has its private placeholder keeps it. Two per unit is far past any real program.
    inline constexpr SizeT kWirePrivateStoragePlaceholderCap = 2 * static_cast<SizeT>(MG_Pipe::kMGPipeMaxImageUnits);

    // The key a placeholder of `shape` is found or made under. `privateStorage` = a storage binding on
    // the arm without a null descriptor (with one, the caller never gets here); `unit` = its image
    // unit; `privateCount` = how many private placeholders exist; `known(key)` = the cache holds `key`.
    template <typename KnownFn>
    WirePlaceholderKey ChooseWirePlaceholderKey(Uint64 shape, Bool privateStorage, Uint32 unit, SizeT privateCount,
                                                KnownFn&& known) {
        WirePlaceholderKey key;
        key.shape = shape;
        if (!privateStorage || unit == WirePlaceholderKey::kShared) return key;
        key.unit = unit;
        if (privateCount < kWirePrivateStoragePlaceholderCap || known(key)) return key;
        key.unit = WirePlaceholderKey::kShared;
        return key;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
#endif
