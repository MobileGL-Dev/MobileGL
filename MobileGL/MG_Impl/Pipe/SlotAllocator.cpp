// MobileGL - MobileGL/MG_Impl/Pipe/SlotAllocator.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// SlotAllocator.h. Compiled only under MOBILEGL_PIPE_PUSH.
#include <MG_Impl/Pipe/SlotAllocator.h>

namespace MobileGL::MG_Pipe {
    namespace {
        // The ShaderCso band the ordinary allocator must never enter: the top 1/16 of the
        // ShaderCso slot space is reserved for PROGRAM PIPELINE COMPOSITES, which are minted
        // client-side out of the stage programs bound to a pipeline object. Reserving a band
        // rather than a flag keeps the composite resolver's lifetime bookkeeping out of here
        // (MGPipeHandles.h, ARCHITECTURE.md 5.6.3).
        Bool SlotIsAllocatable(MGPipeKind kind, Uint32 slot) {
            if (slot < kMGPipeFirstAllocatableSlot) return false;
            if (kind != MGPipeKind::ShaderCso) return true;
            return slot < kMGPipeShaderCsoCompositeSlotBase;
        }
    } // namespace

    MGPipeSlotAllocator::KindState& MGPipeSlotAllocator::StateOf(MGPipeKind kind) {
        const SizeT index = static_cast<SizeT>(kind);
        MOBILEGL_ASSERT(index < kKindCount, "MGPipeKind %zu out of range", index);
        return m_kinds[index < kKindCount ? index : 0];
    }

    const MGPipeSlotAllocator::KindState& MGPipeSlotAllocator::StateOf(MGPipeKind kind) const {
        const SizeT index = static_cast<SizeT>(kind);
        MOBILEGL_ASSERT(index < kKindCount, "MGPipeKind %zu out of range", index);
        return m_kinds[index < kKindCount ? index : 0];
    }

    MGPipeHandle MGPipeSlotAllocator::Allocate(MGPipeKind kind) {
        KindState& state = StateOf(kind);
        if (state.Slots.empty()) {
            // Slot 0 exists so the vector is slot-indexed, and is never handed out.
            state.Slots.resize(kMGPipeFirstAllocatableSlot);
        }

        Uint32 slot = 0;
        Bool reused = false;
        while (!state.FreeList.empty()) {
            const Uint32 candidate = state.FreeList.back();
            state.FreeList.pop_back();
            if (!SlotIsAllocatable(kind, candidate)) continue;
            slot = candidate;
            reused = true;
            break;
        }

        if (!reused) {
            slot = static_cast<Uint32>(state.Slots.size());
            MOBILEGL_ASSERT(SlotIsAllocatable(kind, slot),
                            "MGPipe slot space of kind %u is exhausted at slot %u",
                            static_cast<Uint32>(kind), slot);
            if (!SlotIsAllocatable(kind, slot)) return kMGPipeNullHandle;
            state.Slots.emplace_back();
        }

        SlotState& entry = state.Slots[slot];
        if (entry.EverHandedOut) {
            // The one place Gen may move. 2^32 recycles of ONE slot is ~50 days of continuous
            // churn at one recycle per frame at 1000 fps, which is why the bound is asserted
            // in a debug allocator rather than defended in release.
            MOBILEGL_ASSERT(entry.Gen != ~Uint32{0},
                            "MGPipe handle generation wrapped on kind %u slot %u; {slot, gen} is "
                            "no longer unique",
                            static_cast<Uint32>(kind), slot);
            ++entry.Gen;
        }
        entry.EverHandedOut = true;
        entry.Live = true;
        entry.LifetimeId = 0;
        ++state.LiveCount;
        return MGPipeHandle{slot, entry.Gen};
    }

    MGPipeHandle MGPipeSlotAllocator::AllocateFor(MGPipeKind kind, Uint64 lifetimeId) {
        const MGPipeHandle handle = Allocate(kind);
        if (MGPipeHandleIsNull(handle)) return handle;
        KindState& state = StateOf(kind);
        state.Slots[handle.Slot].LifetimeId = lifetimeId;
        if (lifetimeId != 0) {
            MOBILEGL_ASSERT(state.ByLifetimeId.find(lifetimeId) == state.ByLifetimeId.end(),
                            "lifetime id %llu already owns a slot of kind %u",
                            static_cast<unsigned long long>(lifetimeId), static_cast<Uint32>(kind));
            state.ByLifetimeId[lifetimeId] = handle.Slot;
        }
        return handle;
    }

    MGPipeHandle MGPipeSlotAllocator::FindByLifetimeId(MGPipeKind kind, Uint64 lifetimeId) const {
        if (lifetimeId == 0) return kMGPipeNullHandle;
        const KindState& state = StateOf(kind);
        const auto it = state.ByLifetimeId.find(lifetimeId);
        if (it == state.ByLifetimeId.end()) return kMGPipeNullHandle;
        const Uint32 slot = it->second;
        if (slot >= state.Slots.size() || !state.Slots[slot].Live) return kMGPipeNullHandle;
        return MGPipeHandle{slot, state.Slots[slot].Gen};
    }

    MGPipeHandle MGPipeSlotAllocator::Acquire(MGPipeKind kind, Uint64 lifetimeId) {
        const MGPipeHandle existing = FindByLifetimeId(kind, lifetimeId);
        if (!MGPipeHandleIsNull(existing)) return existing;
        return AllocateFor(kind, lifetimeId);
    }

    void MGPipeSlotAllocator::Free(MGPipeKind kind, MGPipeHandle handle) {
        KindState& state = StateOf(kind);
        if (handle.Slot >= state.Slots.size()) return;
        SlotState& entry = state.Slots[handle.Slot];
        // A stale handle must not free the slot its successor now owns - that is the whole
        // reason the generation is in the key.
        if (!entry.Live || entry.Gen != handle.Gen) return;
        if (entry.LifetimeId != 0) {
            const auto it = state.ByLifetimeId.find(entry.LifetimeId);
            if (it != state.ByLifetimeId.end() && it->second == handle.Slot) {
                state.ByLifetimeId.erase(it);
            }
        }
        entry.Live = false;
        entry.LifetimeId = 0;
        --state.LiveCount;
        state.FreeList.push_back(handle.Slot);
    }

    Bool MGPipeSlotAllocator::IsLive(MGPipeKind kind, MGPipeHandle handle) const {
        const KindState& state = StateOf(kind);
        if (handle.Slot >= state.Slots.size()) return false;
        const SlotState& entry = state.Slots[handle.Slot];
        return entry.Live && entry.Gen == handle.Gen;
    }

    Uint32 MGPipeSlotAllocator::GenOfSlot(MGPipeKind kind, Uint32 slot) const {
        const KindState& state = StateOf(kind);
        if (slot >= state.Slots.size()) return 0;
        return state.Slots[slot].Gen;
    }

    Uint64 MGPipeSlotAllocator::LifetimeIdOfSlot(MGPipeKind kind, Uint32 slot) const {
        const KindState& state = StateOf(kind);
        if (slot >= state.Slots.size()) return 0;
        return state.Slots[slot].LifetimeId;
    }

    Uint32 MGPipeSlotAllocator::HighWater(MGPipeKind kind) const {
        return static_cast<Uint32>(StateOf(kind).Slots.size());
    }

    Uint32 MGPipeSlotAllocator::LiveCount(MGPipeKind kind) const { return StateOf(kind).LiveCount; }

    Uint32 MGPipeSlotAllocator::FreeCount(MGPipeKind kind) const {
        return static_cast<Uint32>(StateOf(kind).FreeList.size());
    }

    void MGPipeSlotAllocator::Reset() {
        for (KindState& state : m_kinds) {
            state.Slots.clear();
            state.FreeList.clear();
            state.ByLifetimeId.clear();
            state.LiveCount = 0;
        }
    }

    MGPipeSlotAllocator& MGPipeSlots() {
        static MGPipeSlotAllocator allocator;
        return allocator;
    }
} // namespace MobileGL::MG_Pipe
