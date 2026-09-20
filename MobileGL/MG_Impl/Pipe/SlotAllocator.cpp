// MobileGL - MobileGL/MG_Impl/Pipe/SlotAllocator.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// SlotAllocator.h. Compiled only under MOBILEGL_PIPE_PUSH.
#include <MG_Impl/Pipe/SlotAllocator.h>

#if MOBILEGL_BUILD_DISAGGREGATED
#include <Config.h>
// P5e (id): the guard's exemption is keyed on whether the record being applied is BARRIERED
// (CONTRACT-P5E §4.4), and MGPipeApplierCurrentRecordIsBarriered() is where ApplyOne stamps
// that. Declarations only - this file names no applier state.
#include <MG_Pipe/PipeApply.h>
#include <MG_Remote/Server/ServerLoop.h>
#include <MG_Util/Debug/Log.h>

#include <cstdlib>
#endif

namespace MobileGL::MG_Pipe {
#if MOBILEGL_BUILD_DISAGGREGATED
    Bool MGPipeApplierIsUnbarrieredApply() {
        if (MG_Config::Transport == MG_Config::TransportMode::Monolith) return false;
        if (!MG_Remote::Server::ServerLoop::OnApplyThread()) return false;
        return !MGPipeApplierCurrentRecordIsBarriered();
    }

    void MGPipeRefuseAllocatorFromApplyThread(const char* entry) {
        if (MG_Config::Transport == MG_Config::TransportMode::Monolith) return;
        if (!MG_Remote::Server::ServerLoop::OnApplyThread()) return;
        // P5f: frontend-keyed registry probes are no longer an exemption. The
        // separate Magma allocator debt remains backend- and barrier-keyed until
        // its remaining monolith glue is removed; it cannot exempt twin-table APIs.
        if (MGPipeApplierCurrentRecordIsBarriered()) {
            // Ruling 12: Magma's four P7 debts, and ONLY on a DirectVulkan server. The key is
            // on the backend kind rather than on the site because the scope is a class anyone
            // can construct, and an Espryt probe borrowing Magma's exemption is exactly the
            // hole P5e is closing.
            if (MagmaP7AllocatorDebtScope::ActiveOnApplyThread() &&
                MG_Config::ActiveBackendType == BackendType::DirectVulkan) {
                return;
            }

        }
        MGLOG_F("MGPipe: Fatal{RoleViolation, \"MGPipeSlots\"} - the apply thread called "
                "MGPipeSlots().%s. With an active transport the client slot allocator is "
                "client-only memory (CONTRACT-P5C §3.1, rule E; CONTRACT-P5E §4.4): a handle "
                "arrives already minted in a record, and a server that resolves or mints one "
                "off a frontend object's lifetime id is reading memory that will not exist on "
                "its side of a real split. Only Magma's allocator debt scope admits it while the "
                "record being applied is BARRIERED (barriered=%d) and, for Magma's P7 debt, "
                "only on a DirectVulkan server",
                entry, MGPipeApplierCurrentRecordIsBarriered() ? 1 : 0);
        std::abort();
    }

    // P5f (fr): all frontend-identity registry surfaces are monolith glue, including
    // the ones that do not touch the allocator. Neither a wait nor a named scope can
    // make a frontend SharedPtr exist in a separate server process.
    void MGPipeRefuseFrontendKeyedRegistryFromApplyThread(const char* entry) {
        if (MG_Config::Transport == MG_Config::TransportMode::Monolith) return;
        if (!MG_Remote::Server::ServerLoop::OnApplyThread()) return;
        MGLOG_F("MGPipe: Fatal{RoleViolation, \"MGPipeSlots\"} - the apply thread reached "
                "BackendSlotTable::%s. Frontend-identity registry operations are monolith-only, "
                "including barriered records; resolve the twin from the record's handle instead",
                entry);
        std::abort();
    }

    namespace {
        // NOT thread_local any more, and not atomic either (P5d round 3, package D).
        //
        // WHY THERE IS NO DATA RACE. Both depths have exactly ONE reader in the whole tree:
        // MGPipeRefuseAllocatorFromApplyThread above, whose first two lines return unless the
        // transport is active AND ServerLoop::OnApplyThread() is true. So the only thread whose
        // depth could ever change an answer is the apply thread - and the four scope bodies
        // below now increment and decrement ONLY when OnApplyThread() says so. That makes the
        // apply thread the single writer and the single reader of both counters; no other
        // thread touches them, and a monolith process never even reads them (the guard's first
        // line returns on TransportMode::Monolith and there is no apply thread to make the
        // second true). Single-writer-single-reader on one thread needs no lock, no atomic and
        // no TLS.
        //
        // WHY IT WAS WORTH DOING. A shared-library thread_local costs an __emutls_get_address
        // call per access. On the MONOLITH's GL thread that symbol is the TOP entry of the
        // profile at 7.7%, and these two scopes' ctor/dtor are 32.7% of its samples
        // (MGPipeFrontendKeyedRegistryScope 18.75 ctor + 13.95 dtor, the scope P5e renamed
        // MagmaP7AllocatorDebtScope 8.4); on the split's apply thread emutls is 7.4%. The
        // measurement predates the rename and the numbers are quoted as measured. The scope is
        // constructed at ~20
        // sites in DirectGLES.cpp, several inside StateBackendObjectRegistry::HandleOf, which
        // is per-draw. The cost on the GL thread is now one inlined predicate per end.
        //
        // The query's meaning ON THE APPLY THREAD is unchanged, which is the only meaning
        // the guard reads. Off it the answer is now always false, and the query is named
        // ActiveOnApplyThread() rather than Active() so that a reader who needs the other
        // meaning cannot ask for it by accident: whoever wants "is a scope open on THIS
        // thread" has to add it, and move the counting back, rather than read a false that
        // looks like an answer.
        Uint32 g_magmaP7AllocatorDebtScopeDepth = 0;
    }

    MagmaP7AllocatorDebtScope::MagmaP7AllocatorDebtScope()
        // Decided ONCE and remembered: the destructor must undo exactly what the constructor
        // did, and re-asking the predicate would leak a count across a scope that straddled
        // the apply thread's exit block.
        : m_counted(MG_Remote::Server::ServerLoop::OnApplyThread()) {
        if (m_counted) ++g_magmaP7AllocatorDebtScopeDepth;
    }

    MagmaP7AllocatorDebtScope::~MagmaP7AllocatorDebtScope() {
        if (m_counted) --g_magmaP7AllocatorDebtScopeDepth;
    }

    // The DEPTH is not backend-keyed and deliberately so: the backend kind is read by the
    // GUARD, once, at the moment it decides. Counting only on DirectVulkan would make the
    // depth's meaning depend on a global that a test can move between the constructor and the
    // destructor, which is the m_counted bug one level out.
    Bool MagmaP7AllocatorDebtScope::ActiveOnApplyThread() {
        return g_magmaP7AllocatorDebtScopeDepth != 0;
    }

    namespace {
        // Same counter, same argument, same single reader - see the block above.
        Uint32 g_frontendKeyedRegistryScopeDepth = 0;
    }

    MGPipeFrontendKeyedRegistryScope::MGPipeFrontendKeyedRegistryScope()
        : m_counted(MG_Remote::Server::ServerLoop::OnApplyThread()) {
        if (m_counted) ++g_frontendKeyedRegistryScopeDepth;
    }

    MGPipeFrontendKeyedRegistryScope::~MGPipeFrontendKeyedRegistryScope() {
        if (m_counted) --g_frontendKeyedRegistryScopeDepth;
    }

    Bool MGPipeFrontendKeyedRegistryScope::ActiveOnApplyThread() {
        return g_frontendKeyedRegistryScopeDepth != 0;
    }
#endif

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

    MGPipeSlotAllocator::SlotState* MGPipeSlotAllocator::EntryOf(KindState& state, MGPipeKind kind,
                                                                 Uint32 slot) {
        if (kind == MGPipeKind::ShaderCso && MGPipeIsCompositeShaderSlot(slot)) {
            const SizeT index = slot - kMGPipeShaderCsoCompositeSlotBase;
            if (index >= state.BandSlots.size()) return nullptr;
            return &state.BandSlots[index];
        }
        if (slot >= state.Slots.size()) return nullptr;
        return &state.Slots[slot];
    }

    const MGPipeSlotAllocator::SlotState*
    MGPipeSlotAllocator::EntryOf(const KindState& state, MGPipeKind kind, Uint32 slot) {
        return EntryOf(const_cast<KindState&>(state), kind, slot);
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

    MGPipeHandle MGPipeSlotAllocator::AllocateComposite(Uint64 lifetimeId) {
        // P4a, D-H7. The mirror image of Allocate() above, restricted to the band that one
        // refuses, and kept in a table of its own so both spaces stay DENSE: the band's base
        // is 983040, and minting one composite into the slot-indexed vector would allocate
        // ~23 MB of SlotState for a single program pipeline.
        KindState& state = StateOf(MGPipeKind::ShaderCso);

        Uint32 slot = 0;
        Bool reused = false;
        if (!state.BandFreeList.empty()) {
            slot = state.BandFreeList.back();
            state.BandFreeList.pop_back();
            reused = true;
        }

        if (!reused) {
            const SizeT next = kMGPipeShaderCsoCompositeSlotBase + state.BandSlots.size();
            slot = static_cast<Uint32>(next);
            // The band's own exhaustion assert, mirroring Allocate()'s: a composite that
            // cannot be minted is a NAMED failure, not a silent fall-through into the ordinary
            // program slots, which is exactly what reserving a band rather than setting a flag
            // buys.
            MOBILEGL_ASSERT(next < kMGPipeShaderCsoSlotLimit,
                            "the MGPipe ShaderCso COMPOSITE band is exhausted at slot %zu; a "
                            "program-pipeline composite cannot be minted and must not take an "
                            "ordinary program's slot",
                            next);
            if (next >= kMGPipeShaderCsoSlotLimit) return kMGPipeNullHandle;
            state.BandSlots.emplace_back();
        }

        SlotState* entry = EntryOf(state, MGPipeKind::ShaderCso, slot);
        if (entry == nullptr) return kMGPipeNullHandle;
        if (entry->EverHandedOut) {
            MOBILEGL_ASSERT(entry->Gen != ~Uint32{0},
                            "MGPipe handle generation wrapped on the ShaderCso composite band, "
                            "slot %u; {slot, gen} is no longer unique",
                            slot);
            ++entry->Gen;
        }
        entry->EverHandedOut = true;
        entry->Live = true;
        entry->LifetimeId = lifetimeId;
        ++state.LiveCount;
        // The band's share of LiveCount, so CompositeLiveCount() can answer without a walk.
        ++state.BandLiveCount;
        if (lifetimeId != 0) {
            MOBILEGL_ASSERT(state.ByLifetimeId.find(lifetimeId) == state.ByLifetimeId.end(),
                            "lifetime id %llu already owns a ShaderCso slot",
                            static_cast<unsigned long long>(lifetimeId));
            state.ByLifetimeId[lifetimeId] = slot;
        }
        return MGPipeHandle{slot, entry->Gen};
    }

    MGPipeHandle MGPipeSlotAllocator::FindByLifetimeId(MGPipeKind kind, Uint64 lifetimeId) const {
#if MOBILEGL_BUILD_DISAGGREGATED
        MGPipeRefuseAllocatorFromApplyThread("FindByLifetimeId");
#endif
        if (lifetimeId == 0) return kMGPipeNullHandle;
        const KindState& state = StateOf(kind);
        const auto it = state.ByLifetimeId.find(lifetimeId);
        if (it == state.ByLifetimeId.end()) return kMGPipeNullHandle;
        const SlotState* entry = EntryOf(state, kind, it->second);
        if (entry == nullptr || !entry->Live) return kMGPipeNullHandle;
        return MGPipeHandle{it->second, entry->Gen};
    }

    MGPipeHandle MGPipeSlotAllocator::Acquire(MGPipeKind kind, Uint64 lifetimeId) {
#if MOBILEGL_BUILD_DISAGGREGATED
        MGPipeRefuseAllocatorFromApplyThread("Acquire");
#endif
        const MGPipeHandle existing = FindByLifetimeId(kind, lifetimeId);
        if (!MGPipeHandleIsNull(existing)) return existing;
        return AllocateFor(kind, lifetimeId);
    }

    void MGPipeSlotAllocator::Free(MGPipeKind kind, MGPipeHandle handle) {
#if MOBILEGL_BUILD_DISAGGREGATED
        MGPipeRefuseAllocatorFromApplyThread("Free");
#endif
        KindState& state = StateOf(kind);
        SlotState* entry = EntryOf(state, kind, handle.Slot);
        if (entry == nullptr) return;
        // A stale handle must not free the slot its successor now owns - that is the whole
        // reason the generation is in the key. It is also what makes the SECOND of a
        // composite's two independent release paths a proven no-op.
        if (!entry->Live || entry->Gen != handle.Gen) return;
        if (entry->LifetimeId != 0) {
            const auto it = state.ByLifetimeId.find(entry->LifetimeId);
            if (it != state.ByLifetimeId.end() && it->second == handle.Slot) {
                state.ByLifetimeId.erase(it);
            }
        }
        entry->Live = false;
        entry->LifetimeId = 0;
        --state.LiveCount;
        if (kind == MGPipeKind::ShaderCso && MGPipeIsCompositeShaderSlot(handle.Slot)) {
            --state.BandLiveCount;
            state.BandFreeList.push_back(handle.Slot);
        } else {
            state.FreeList.push_back(handle.Slot);
        }
    }

    Bool MGPipeSlotAllocator::IsLive(MGPipeKind kind, MGPipeHandle handle) const {
        const SlotState* entry = EntryOf(StateOf(kind), kind, handle.Slot);
        return entry != nullptr && entry->Live && entry->Gen == handle.Gen;
    }

    Uint32 MGPipeSlotAllocator::GenOfSlot(MGPipeKind kind, Uint32 slot) const {
        const SlotState* entry = EntryOf(StateOf(kind), kind, slot);
        return entry != nullptr ? entry->Gen : 0;
    }

    Uint64 MGPipeSlotAllocator::LifetimeIdOfSlot(MGPipeKind kind, Uint32 slot) const {
        const SlotState* entry = EntryOf(StateOf(kind), kind, slot);
        return entry != nullptr ? entry->LifetimeId : 0;
    }

    Uint32 MGPipeSlotAllocator::HighWater(MGPipeKind kind) const {
        // THE ORDINARY SPACE ONLY, and the band is reported by CompositeHighWater() below.
        // Folding the two would pin this at ~983k from the first composite mint onward and
        // take the ordinary space's "the high-water mark did not move" assertion away for the
        // rest of the process - the assertion that catches a dense table that never shrinks,
        // which is the leak shape this allocator exists to make visible. Two spaces, two
        // numbers, two real assertions. See SlotAllocator.h.
        return static_cast<Uint32>(StateOf(kind).Slots.size());
    }

    Uint32 MGPipeSlotAllocator::CompositeHighWater() const {
        const KindState& state = StateOf(MGPipeKind::ShaderCso);
        // One past the highest composite slot ever handed out; exactly the base when none ever
        // was, so the number is monotone from the first mint and a LEAKED COMPOSITE MOVES IT.
        return static_cast<Uint32>(kMGPipeShaderCsoCompositeSlotBase + state.BandSlots.size());
    }

    Uint32 MGPipeSlotAllocator::LiveCount(MGPipeKind kind) const { return StateOf(kind).LiveCount; }

    Uint32 MGPipeSlotAllocator::CompositeLiveCount() const {
        return StateOf(MGPipeKind::ShaderCso).BandLiveCount;
    }

    Uint32 MGPipeSlotAllocator::FreeCount(MGPipeKind kind) const {
        const KindState& state = StateOf(kind);
        return static_cast<Uint32>(state.FreeList.size() + state.BandFreeList.size());
    }

    Uint32 MGPipeSlotAllocator::CompositeFreeCount() const {
        return static_cast<Uint32>(StateOf(MGPipeKind::ShaderCso).BandFreeList.size());
    }

    void MGPipeSlotAllocator::Reset() {
        for (KindState& state : m_kinds) {
            state.Slots.clear();
            state.FreeList.clear();
            state.BandSlots.clear();
            state.BandFreeList.clear();
            state.ByLifetimeId.clear();
            state.LiveCount = 0;
            state.BandLiveCount = 0;
        }
    }

    MGPipeSlotAllocator& MGPipeSlots() {
        // NEVER DESTROYED, deliberately (one allocation for the life of the process). A
        // frontend object's destructor reaches this allocator - ~BufferObject through
        // MGPipeEmitResourceDestroyAndFree, ~VertexArrayObject through the death notice - and
        // MG_Backend/MGPipe/PipeInputs.h's gPipeInputs holds SharedPtrs to those objects at
        // namespace scope, so they are destroyed by __run_exit_handlers AFTER this
        // function-local static would have been. A destroyed allocator then answers
        // FindByLifetimeId out of a freed hash table and Free() writes into freed vectors -
        // an exit-time heap corruption whose fatality depends only on the allocator's layout.
        static MGPipeSlotAllocator* allocator = new MGPipeSlotAllocator();
        return *allocator;
    }
} // namespace MobileGL::MG_Pipe
