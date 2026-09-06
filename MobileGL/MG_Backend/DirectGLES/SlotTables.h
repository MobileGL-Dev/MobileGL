// MobileGL - MobileGL/MG_Backend/DirectGLES/SlotTables.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#include <MG_Pipe/MGPipeHandles.h>

#if MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/SlotAllocator.h>
#endif

// Espryt 0b, the first Track H slice: the DENSE, {slot, gen}-keyed twin table that replaces
// StateBackendObjectRegistry's UnorderedMap<StateObject*, Entry>.
//
// What changes, and why each of them is the point:
//
//  * The KEY stops being a frontend heap address. It is MGPipeHandle{Slot, Gen}, minted by the
//    client's MGPipeSlotAllocator off the frontend object's GetLifetimeId(). A recycled heap
//    address cannot reproduce a handle, so the weak_ptr the registry carried per entry purely
//    to catch that (its Entry::stateRef, used as an IDENTITY test) stops being an identity
//    mechanism, and OwnerEquals / TwinLookupMemo x3 / UnitSamplerLookupMemo's owner compare all
//    lose their reason to exist.
//  * The lookup stops being a hash probe into an open-addressed map and becomes one bounds
//    check plus one array index, so a returned BackendPtr* is NOT invalidated by the next Find
//    or sweep on the table. That kills the hazard Managers.h documents at length, and with it
//    the by-value copy plus second Find that SyncTextureObjectToBackend paid to survive it.
//  * Slots are dense per kind, which is what lets the server side (ARCHITECTURE.md 10.1,
//    MG_Remote/Server/PipeObjectTables) be an array rather than an object graph.
//
// What has NOT changed, deliberately, and is this file's one departure from the P2 brief
// (recorded in the package result file): a frontend object's death is still discovered rather
// than announced. The brief's step e2 - a BufferBackendOps-shaped OnDestroy for the other six
// kinds - has to be installed in MG_State/GLState/{Texture,Framebuffer,Renderbuffer,Sampler,
// Program,VertexArray}State/*, and the P2 file-ownership table gives every one of those files
// to another package. So the table keeps ONE weak_ptr per entry and uses it for exactly one
// thing: ReclaimDeadSlots() frees the slot - and the twin, and the driver storage it owns -
// once the frontend object is gone. That is a liveness sweep, not an identity test, and it is
// what bumps Gen, which is precisely the ABA defence: a slot is only ever handed out again
// after it was freed. When e2 lands, ReclaimDeadSlots() becomes the fallback path of an
// explicit Destroy(handle) and the sweep call sites go away.
namespace MobileGL::MG_Backend::DirectGLES {

#if MOBILEGL_PIPE_PUSH

    // True when this process runs the {slot, gen} arm. Fixed for the life of the process: the
    // two arms hold their twins in different containers, so flipping mid-run would strand them.
    Bool EsprytSlotTablesEnabled();

    template <typename StateObject, typename BackendObject, MG_Pipe::MGPipeKind kKind>
    class BackendSlotTable {
    public:
        using StatePtr = SharedPtr<StateObject>;
        using StateWeakPtr = std::weak_ptr<StateObject>;
        using BackendPtr = SharedPtr<BackendObject>;

        struct Entry {
            BackendPtr backend;
            // LIVENESS ONLY. Never compared against another object to decide identity - that is
            // what Gen is for - and never dereferenced for its address. Read by
            // ReclaimDeadSlots(), and locked by ForEachLive() so the callee holds a strong ref.
            StateWeakPtr stateRef;
            // The generation this entry's twin was built for. An entry whose Gen no longer
            // matches the allocator's is a twin of the slot's PREVIOUS owner.
            Uint32 Gen = 0;
            Bool Live = false;
        };

        // Resolve-or-create. The handle comes from the client allocator keyed on the frontend
        // object's lifetime id, so two calls for the same live object always land on the same
        // slot, and a successor object at the same heap address never does.
        BackendPtr& GetOrCreate(const StatePtr& stateObj) {
            MOBILEGL_ASSERT(stateObj != nullptr, "State object must not be null");
            const MG_Pipe::MGPipeHandle handle =
                MG_Pipe::MGPipeSlots().Acquire(kKind, stateObj->GetLifetimeId());
            MOBILEGL_ASSERT(!MG_Pipe::MGPipeHandleIsNull(handle),
                            "MGPipe slot space of kind %u is exhausted",
                            static_cast<Uint32>(kKind));
            Entry& entry = EntryAt(handle.Slot);
            if (entry.Live && entry.Gen != handle.Gen) {
                // The slot was reclaimed and handed to a new object: the twin at it describes
                // driver ids the new state object never made.
                entry.backend.reset();
            }
            entry.Gen = handle.Gen;
            entry.Live = true;
            entry.stateRef = stateObj;
            return entry.backend;
        }

        // Null when no live twin of this object exists. Unlike the registry's Find this NEVER
        // mutates the table, so the returned pointer survives any later Find or sweep on it;
        // only a GetOrCreate that grows the vector can move it, and callers that hold one
        // across a possible insertion still copy the BackendPtr out.
        BackendPtr* Find(StateObject* stateObj) {
            if (stateObj == nullptr) return nullptr;
            return FindByHandle(HandleOf(stateObj));
        }

        const BackendPtr* Find(StateObject* stateObj) const {
            return const_cast<BackendSlotTable*>(this)->Find(stateObj);
        }

        BackendPtr* FindByHandle(MG_Pipe::MGPipeHandle handle) {
            if (MG_Pipe::MGPipeHandleIsNull(handle)) return nullptr;
            if (handle.Slot >= m_slots.size()) return nullptr;
            Entry& entry = m_slots[handle.Slot];
            if (!entry.Live || entry.Gen != handle.Gen) return nullptr;
            return &entry.backend;
        }

        // The handle this object's twin is keyed on, or the null handle. This is what a backend
        // memo stores instead of a raw pointer, a GL name or a bare lifetime id.
        MG_Pipe::MGPipeHandle HandleOf(const StateObject* stateObj) const {
            if (stateObj == nullptr) return MG_Pipe::kMGPipeNullHandle;
            return MG_Pipe::MGPipeSlots().FindByLifetimeId(kKind, stateObj->GetLifetimeId());
        }

        // Drop the twin of every slot whose frontend object is gone and return the slot to the
        // allocator. Freeing is what makes the NEXT handout of that slot bump Gen.
        void ReclaimDeadSlots() {
            if (m_isCollecting) return;
            m_isCollecting = true;
            for (SizeT slot = 0; slot < m_slots.size(); ++slot) {
                Entry& entry = m_slots[slot];
                if (!entry.Live || !entry.stateRef.expired()) continue;
                entry.backend.reset();
                entry.stateRef.reset();
                entry.Live = false;
                MG_Pipe::MGPipeSlots().Free(
                    kKind, MG_Pipe::MGPipeHandle{static_cast<Uint32>(slot), entry.Gen});
            }
            m_isCollecting = false;
        }

        void CollectGarbageIfNeeded() {
            ++m_gcTick;
            if (m_gcTick < kGCInterval) return;
            m_gcTick = 0;
            ReclaimDeadSlots();
        }

        void CollectGarbageNow() { ReclaimDeadSlots(); }

        // fn(const StatePtr& state, const BackendPtr& twin) over every live, still-owned entry.
        // Replaces the registry's begin()/end(), whose iterator exposed the raw frontend
        // address as the map key - the one place the backend read an identity it must not have.
        // The state object is handed over as a STRONG reference, so the callee cannot be handed
        // a dangling key the way the old iteration could.
        template <typename Fn>
        void ForEachLive(Fn&& fn) const {
            for (const Entry& entry : m_slots) {
                if (!entry.Live || !entry.backend) continue;
                const StatePtr state = entry.stateRef.lock();
                if (!state) continue;
                fn(state, entry.backend);
            }
        }

        Uint32 LiveCount() const {
            Uint32 count = 0;
            for (const Entry& entry : m_slots) {
                if (entry.Live) ++count;
            }
            return count;
        }

    private:
        Entry& EntryAt(Uint32 slot) {
            if (slot >= m_slots.size()) m_slots.resize(static_cast<SizeT>(slot) + 1);
            return m_slots[slot];
        }

        static constexpr Uint32 kGCInterval = 1024;

        // Indexed by MGPipeHandle::Slot; [0] is the reserved slot and is never live.
        Vector<Entry> m_slots;
        Uint32 m_gcTick = 0;
        Bool m_isCollecting = false;
    };

#endif // MOBILEGL_PIPE_PUSH
} // namespace MobileGL::MG_Backend::DirectGLES
