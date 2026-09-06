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
// Death: announced where the package may announce it, discovered everywhere else.
// DestroyByLifetimeId() below is step e2's backend half and it is complete - it drops the twin
// and returns the slot the moment the frontend object's last SharedPtr goes - and the notice
// that drives it (MG_State/GLState/StateObjectDeathNotice.h, BufferBackendOps' shape) is
// registered for all six kinds. What is only PARTLY wired is the firing side: a destructor has
// to raise the notice, and of the six object classes the P2 file-ownership table gives
// {Texture,Framebuffer,Sampler,VertexArray}State/* to other packages, so only ProgramObject
// and RenderbufferObject fire it here. The four that do not still rely on the sweep, which is
// why the table keeps ONE weak_ptr per entry and uses it for exactly one thing:
// ReclaimDeadSlots() frees the slot - and the twin, and the driver storage it owns - once the
// frontend object is gone. That is a liveness sweep, not an identity test, and it is what
// bumps Gen, which is precisely the ABA defence: a slot is only ever handed out again after it
// was freed. The four remaining one-line destructor calls retire the sweep entirely.
//
// Because the sweep is still the only death signal, this table carries BOTH of the drivers
// the registry it replaces carries, and for the same reasons:
//   * the draw-path tick (kGCInterval = 1024 CollectGarbageIfNeeded calls), and
//   * the CREATION tick (kCreationGCInterval = 64 first-time insertions), because object CHURN
//     rather than draw count is what makes the sweep urgent - a CTS-shaped case runs ~10
//     per-draw ticks, so 1024 of them span ~100 cases' worth of dead, gigabyte-sized objects.
// Dropping the second one would have made this table's memory behaviour strictly WORSE than
// the map it replaces, which is the opposite of what the slice is for.
//
// P3+ DEBT, recorded rather than hidden: this header is under MG_Backend/ and it MINTS
// handles (MGPipeSlots().Acquire below) off a frontend SharedPtr's GetLifetimeId().
// MGPipeHandles.h:13-16 says a handle is minted by the CLIENT and never by the server, and
// under a real split neither the frontend object nor its lifetime id exists on this side of
// the wire. This is monolith glue: the minting and the lifetimeId -> handle resolution both
// belong on the client, and the backend should receive the handle in the verb payload. It is
// NOT part of "Track H done" and check_include_closure.py does not probe MG_Backend headers,
// so nothing catches it automatically.
namespace MobileGL::MG_Backend::DirectGLES {

#if MOBILEGL_PIPE_PUSH

    // Reads the config, logs, and traps when the operator left no arm at all. Cold: called
    // exactly once per process, from the latch below and from backend context creation.
    Bool ResolveEsprytSlotTablesArm();

    // True when this process runs the {slot, gen} arm. Fixed for the life of the process: the
    // two arms hold their twins in different containers, so flipping mid-run would strand them.
    //
    // INLINE on purpose. Every Find / GetOrCreate / HandleOf / ForEachLive / CollectGarbage*
    // on the twin tables consults it, i.e. it is on the per-draw path several times per draw.
    // As an out-of-line function in Managers.cpp (no LTO in any shipped configuration) that was
    // a call through the PLT per lookup; here the caller sees a guard-variable load and a
    // perfectly-predicted branch, and the arm dispatch folds into the caller.
    inline Bool EsprytSlotTablesEnabled() {
        static const Bool enabled = ResolveEsprytSlotTablesArm();
        return enabled;
    }

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
            // No assert on null here, unlike the map arm: null is TOLERATED, so a DEBUG build
            // must not trap where the release build quietly does the documented thing.
            if (stateObj == nullptr) {
                // The registry this replaces inserted a null key and handed back ITS twin slot
                // (DirectGLES.cpp's SyncTextureObjectToBackend documents relying on exactly
                // that tolerance), so a release build never dereferenced null here. Keep the
                // shape: one per-table parking slot, never live, never swept, never handed a
                // handle. A null object has no identity and therefore cannot have a twin.
                m_nullTwin.reset();
                return m_nullTwin;
            }

            // Sweep BEFORE the entry reference below exists, for the same reason the map arm
            // does it here: EntryAt may grow m_slots and move every element, so a reference
            // taken first would not survive it. The sweep is owed from an earlier creation
            // rather than triggered by this one.
            if (m_creationTick >= kCreationGCInterval) {
                m_creationTick = 0;
                ReclaimDeadSlots();
            }

            const MG_Pipe::MGPipeHandle handle =
                MG_Pipe::MGPipeSlots().Acquire(kKind, stateObj->GetLifetimeId());
            MOBILEGL_ASSERT(!MG_Pipe::MGPipeHandleIsNull(handle),
                            "MGPipe slot space of kind %u is exhausted",
                            static_cast<Uint32>(kKind));
            Entry& entry = EntryAt(handle.Slot);
            const Bool firstInsertion = !entry.Live || entry.Gen != handle.Gen;
            if (entry.Live && entry.Gen != handle.Gen) {
                // The slot was reclaimed and handed to a new object: the twin at it describes
                // driver ids the new state object never made.
                entry.backend.reset();
            }
            entry.Gen = handle.Gen;
            entry.Live = true;
            entry.stateRef = stateObj;
            if (firstInsertion) {
                // A slot this table has never held (or held for a previous owner). Nothing
                // tells the backend that a texture or renderbuffer was DELETED - the twin, and
                // the driver storage it owns, lives until a collection - and
                // CollectGarbageIfNeeded is ticked only from the per-draw sync paths, which a
                // CTS-shaped workload runs about ten times per case. 1024 of those ticks then
                // span ~100 cases, so ~100 cases' worth of dead (and, for this suite,
                // gigabyte-sized) objects would stay allocated at once. Object CHURN rather
                // than draw count is what makes the sweep urgent, so a twin the table has
                // never seen ticks it too - and it does so on the path that is about to
                // allocate, which is exactly when the memory is needed.
                ++m_creationTick;
            }
            RememberHandle(stateObj->GetLifetimeId(), handle);
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
            const Uint64 lifetimeId = stateObj->GetLifetimeId();
            if (lifetimeId == m_memoLifetimeId) return m_memoHandle;
            const MG_Pipe::MGPipeHandle handle =
                MG_Pipe::MGPipeSlots().FindByLifetimeId(kKind, lifetimeId);
            RememberHandle(lifetimeId, handle);
            return handle;
        }

        // Drop the twin of every slot whose frontend object is gone and return the slot to the
        // allocator. Freeing is what makes the NEXT handout of that slot bump Gen.
        void ReclaimDeadSlots() {
            if (m_isCollecting) return;
            m_isCollecting = true;
            for (SizeT slot = 0; slot < m_slots.size(); ++slot) {
                Uint32 gen = 0;
                // The twin's destructor is a driver call and could, in principle, re-enter
                // GetOrCreate on this table and resize m_slots. So NOTHING that outlives the
                // destructor may be a reference into m_slots: the twin is moved out into a
                // local, the entry is finished with, and only then is the local released.
                BackendPtr dead;
                {
                    Entry& entry = m_slots[slot];
                    if (!entry.Live || !entry.stateRef.expired()) continue;
                    gen = entry.Gen;
                    dead = std::move(entry.backend);
                    entry.backend.reset();
                    entry.stateRef.reset();
                    entry.Live = false;
                }
                MG_Pipe::MGPipeSlots().Free(
                    kKind, MG_Pipe::MGPipeHandle{static_cast<Uint32>(slot), gen});
                if (m_memoHandle.Slot == static_cast<Uint32>(slot)) ForgetHandle();
                dead.reset();
            }
            m_isCollecting = false;
        }

        // P2 step e2's backend half: the frontend object with this lifetime id has just been
        // DESTROYED, so drop its twin and return its slot now rather than waiting for a sweep
        // to notice the weak_ptr expired. Announced death is what the sweep is a stand-in for;
        // it frees the driver storage the twin owns at the moment the application let go of
        // the object, which is what Managers.h's "dead gigabytes" note is about.
        //
        // Returns whether this table held the slot. The slot goes back to the allocator ONLY
        // then, and this is not defensive: two holders of one kind already exist (the
        // ScopedDirectGLESTextureBindings fixture's saved copy is a second live table of kind
        // Texture; Magma's subsystem-4 table shares the VertexElementsCso kind), and a table
        // that never twinned the object must not free a slot the other one still names.
        Bool DestroyByLifetimeId(Uint64 lifetimeId) {
            const MG_Pipe::MGPipeHandle handle =
                MG_Pipe::MGPipeSlots().FindByLifetimeId(kKind, lifetimeId);
            if (MG_Pipe::MGPipeHandleIsNull(handle)) return false;
            if (handle.Slot >= m_slots.size()) return false;
            // Same ordering rule as ReclaimDeadSlots(): the twin's destructor is a driver call
            // and could re-enter GetOrCreate and resize m_slots, so nothing that outlives it
            // may be a reference into the vector.
            BackendPtr dead;
            {
                Entry& entry = m_slots[handle.Slot];
                if (!entry.Live || entry.Gen != handle.Gen) return false;
                dead = std::move(entry.backend);
                entry.backend.reset();
                entry.stateRef.reset();
                entry.Live = false;
            }
            if (m_memoHandle.Slot == handle.Slot) ForgetHandle();
            MG_Pipe::MGPipeSlots().Free(kKind, handle);
            dead.reset();
            return true;
        }

        void CollectGarbageIfNeeded() {
            ++m_gcTick;
            if (m_gcTick < kGCInterval) return;
            m_gcTick = 0;
            m_creationTick = 0;
            ReclaimDeadSlots();
        }

        void CollectGarbageNow() {
            m_creationTick = 0;
            ReclaimDeadSlots();
        }

        // Test-only introspection: how many first-time insertions are owed before the
        // creation-driven sweep fires. Reading it is what lets a test pin the CADENCE rather
        // than only the effect of an explicit CollectGarbageNow().
        Uint32 CreationTickForTest() const { return m_creationTick; }
        static constexpr Uint32 CreationGCIntervalForTest() { return kCreationGCInterval; }

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

        void RememberHandle(Uint64 lifetimeId, MG_Pipe::MGPipeHandle handle) const {
            m_memoLifetimeId = lifetimeId;
            m_memoHandle = handle;
        }
        void ForgetHandle() const {
            m_memoLifetimeId = 0;
            m_memoHandle = MG_Pipe::kMGPipeNullHandle;
        }

        static constexpr Uint32 kGCInterval = 1024;
        // Creations are far rarer than draws, so this counts in a much smaller unit than
        // kGCInterval does. Same value the map arm uses, so the two arms sweep at the same
        // cadence under the same workload.
        static constexpr Uint32 kCreationGCInterval = 64;

        // Indexed by MGPipeHandle::Slot; [0] is the reserved slot and is never live.
        Vector<Entry> m_slots;
        Uint32 m_gcTick = 0;
        Uint32 m_creationTick = 0;
        Bool m_isCollecting = false;
        // Handed back by GetOrCreate for a null state object. Never live, never swept.
        BackendPtr m_nullTwin;

        // ONE-entry resolution memo, lifetimeId -> handle. The three per-draw resolution paths
        // (ResolveVaoTwin, SyncCurrentProgram, BindCurrentFBO) ask the SAME table for the SAME
        // object every draw, so this turns the steady state back into an integer compare plus
        // one array index - which is what the deleted TwinLookupMemos bought and what D13
        // promises ("direct slot indexing - the memo existed only to avoid the hash probe").
        // Without it every resolution went through the allocator's ByLifetimeId hash.
        //
        // It cannot serve a stale answer, by two independent arguments:
        //   * the key is a lifetime id, which MG_State never hands out twice, so a recycled
        //     heap address cannot hit this memo the way it could hit an address-keyed one; and
        //   * even a hit for a slot that has since been freed and re-handed is caught, because
        //     the caller resolves the handle through FindByHandle, which compares Gen.
        // Cleared anyway when the sweep frees the memoised slot. 0 is never a live lifetime id
        // (MG_State's counters start at 1), so a zeroed memo is a guaranteed miss.
        mutable Uint64 m_memoLifetimeId = 0;
        mutable MG_Pipe::MGPipeHandle m_memoHandle = MG_Pipe::kMGPipeNullHandle;
    };

#endif // MOBILEGL_PIPE_PUSH
} // namespace MobileGL::MG_Backend::DirectGLES
