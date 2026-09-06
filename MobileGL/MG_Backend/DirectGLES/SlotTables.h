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
// Death is ANNOUNCED, and that is what lets this table have no garbage collector - the
// deliverable ROADMAP.md:18 spells "GC" in and the one D13 makes a precondition of the switch-
// over. All six re-keyed object classes raise MG_State::GLState::NotifyStateObjectDestroyed()
// from their destructor (BufferBackendOps' shape, one entry point for six kinds), the backend
// consumes it in Managers.cpp, and DestroyByLifetimeId() below drops the twin and returns the
// slot at the moment the frontend object's last SharedPtr goes. So:
//   * there is NO draw-path tick and NO creation tick on this arm. CollectGarbageIfNeeded() is
//     an empty call, and the seven call sites in DirectGLES.cpp drive the LEGACY registry only;
//   * a twin, and the driver storage it owns, is freed when the application lets go of the
//     object rather than up to 64 creations or 1024 draw ticks later. That is what
//     Managers.h's "dead gigabytes" note asked for.
//
// The weak_ptr per entry survives, and only for what it is honest about:
//   * ForEachLive() hands the callee a STRONG reference to the frontend object, which the one
//     direct-iteration site (ScopedDetachedTextureFramebufferAttachments) needs; and
//   * ReclaimDeadSlots() is kept as the body of the EXPLICIT CollectGarbageNow(), i.e. a
//     collection someone asks for, never a periodic one. It is the backstop for the one case
//     the notice cannot cover: a destructor that runs after exit() has begun, where
//     InProcessTeardown() drops the notice because a twin destructor must not call the driver.
// It is never an identity test - that is what Gen is for.
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

    // What the two knobs add up to. Split out as a PURE function of them so a test can drive
    // every combination without needing a process per combination.
    enum class EsprytSlotArmVerdict {
        Handles, // kMGPipeSubsystemEsprytSlots is set: the {slot, gen} tables run.
        Legacy,  // the bit is clear and the legacy address-keyed registry is reachable.
        NoArm,   // the bit is clear AND MOBILEGL_PIPE_LEGACY_MEMOS=0 made the legacy arm
                 // unreachable, so the operator asked for a configuration with no arm at all.
    };

    EsprytSlotArmVerdict ClassifyEsprytSlotArm(Bool subsystemBitSet, Bool legacyMemosEnabled);

    // This process's verdict, read off MG_Config::Features. Latches nothing and stops nothing.
    EsprytSlotArmVerdict CurrentEsprytSlotArmVerdict();

    // Says, at backend bring-up, that the knobs leave no arm - and does NOT stop.
    //
    // The stop cannot live here, and that is the whole point of the split. Backend context
    // creation runs inside eglMakeCurrent, and the integration harness pre-flights exactly that
    // sequence in a FORKED CHILD (MG_IntegrationTest/Harness/HeadlessGL.cpp): a child that dies
    // on a signal is reported as "no usable GPU/display/ICD" and every scenario in the lane is
    // SKIPPED - i.e. the lane goes green having run nothing, on the very pair of env vars the
    // D14/D18 A/B is driven with, which is what ROADMAP.md:7 forbids. So bring-up only
    // DIAGNOSES; the stop is raised by ResolveEsprytSlotTablesArm() at the first twin lookup,
    // which happens in the test body where the harness reports it as a failure.
    void DiagnoseEsprytSlotArm();

    // Reads the config, logs, installs the death-notice consumer, and STOPS when the operator
    // left no arm at all. Cold: called exactly once per process, from the latch below - i.e. at
    // the first twin lookup, which is the first moment an arm is actually needed. A process
    // that never twins anything needs no arm and is not stopped.
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
            // No creation tick and no sweep here. The registry this replaces needed both,
            // because nothing told it a texture or a renderbuffer had been DELETED and object
            // CHURN rather than draw count is what made that urgent. Every one of the six kinds
            // now announces its own death from its destructor, so a dead twin's slot is already
            // back before the next creation asks for one.
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

        // Deliberately EMPTY, and this is the P2 deliverable rather than an omission: on this
        // arm death is announced, so there is nothing for a periodic sweep to discover. The
        // seven DirectGLES.cpp call sites keep their spelling because they are the legacy
        // registry's driver and that arm is still compiled beside this one; on this arm they
        // cost the predicted branch in StateBackendObjectRegistry and return.
        void CollectGarbageIfNeeded() {}

        // An EXPLICIT collection - someone asked, so it runs. Not a driver: nothing calls this
        // on a tick. It is the backstop for a notice that could not be delivered (see the
        // InProcessTeardown() note in the file header) and the tests' way of forcing the
        // liveness sweep without waiting for one.
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

        void RememberHandle(Uint64 lifetimeId, MG_Pipe::MGPipeHandle handle) const {
            m_memoLifetimeId = lifetimeId;
            m_memoHandle = handle;
        }
        void ForgetHandle() const {
            m_memoLifetimeId = 0;
            m_memoHandle = MG_Pipe::kMGPipeNullHandle;
        }

        // Indexed by MGPipeHandle::Slot; [0] is the reserved slot and is never live.
        Vector<Entry> m_slots;
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
