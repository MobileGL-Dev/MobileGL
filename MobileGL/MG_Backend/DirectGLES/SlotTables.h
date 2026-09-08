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
//    on the table. That kills the hazard Managers.h documents at length, and with it the
//    by-value copy plus second Find that SyncTextureObjectToBackend paid to survive it.
//  * Slots are dense per kind, which is what lets the server side (ARCHITECTURE.md 10.1,
//    MG_Remote/Server/PipeObjectTables) be an array rather than an object graph.
//
// Death is ANNOUNCED, and that is what lets this table have no garbage collector - the
// deliverable ROADMAP.md:18 spells "GC" in and the one D13 makes a precondition of the switch-
// over. All six re-keyed object classes raise MG_State::GLState::NotifyStateObjectDestroyed()
// from their destructor (BufferBackendOps' shape, one entry point for six kinds), the backend
// consumes it in Managers.cpp, and OnFrontendObjectDestroyed() below drops the twin in EVERY
// table of the kind and returns the slot, at the moment the frontend object's last SharedPtr
// goes. So:
//   * there is NO draw-path tick, NO creation tick and NO sweep of any kind on this arm. The
//     seven CollectGarbageIfNeeded call sites in DirectGLES.cpp drive the LEGACY registry only;
//   * a twin, and the driver storage it owns, is freed when the application lets go of the
//     object rather than up to 64 creations or 1024 draw ticks later. That is what
//     Managers.h's "dead gigabytes" note asked for.
//
// EVERY HOLDER OF THE KIND, not one. Two live tables of one kind is a real configuration - the
// ScopedDirectGLESTextureBindings fixture keeps a by-value copy of the Texture registry for the
// length of a test, and a context reset does the same in reverse - and the slot allocator
// erases its lifetimeId -> slot mapping on Free, so a notice delivered to one holder and
// resolved again by the next would find nothing to resolve. Every table therefore links itself
// into a per-table-type list at construction and out at destruction, and one notice resolves
// the handle ONCE, drops the twin in each holder BY HANDLE, and frees the slot once, last. No
// holder can be left naming a live entry for a dead object, and there is nothing a sweep could
// still find. (The list is per table TYPE; the kind is the type's template parameter, and each
// of the six kinds has exactly one table type in this backend. Magma's subsystem-4 table mints
// out of its own per-renderer allocator, not MGPipeSlots(), so it is not a holder here.)
//
// The weak_ptr per entry survives for exactly one reason: ForEachLive() hands the callee a
// STRONG reference to the frontend object, which the one direct-iteration site
// (ScopedDetachedTextureFramebufferAttachments) needs. It is never an identity test - that is
// what Gen is for - and it is never read to decide whether an entry is dead: a destructor that
// runs after exit() has begun has its notice dropped by InProcessTeardown(), and that twin is
// then a DELIBERATE leak (the process is exiting, the driver reclaims the object, and a twin
// destructor must not call into a driver that may already be unloaded), not something to be
// collected later.
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

    // Declared in Managers.h as well; repeated here because this header is included from it
    // before that declaration, and the table below is the arming site on this arm (D13: "the
    // arming site moves to the slot table's first insertion").
    void EnsureProcessTeardownSentinel();

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
    //
    // The CALL SITE (InitDisplayAndContext in DirectGLES.cpp) is pinned by
    // DirectGLESSlotTable.EglBringUpUnderTheArmlessKnobPairReturnsInsteadOfStopping, which runs
    // the real bring-up entry point under the pair in a forked child: edit that site back to
    // ResolveEsprytSlotTablesArm() and the case fails naming both knobs.
    void DiagnoseEsprytSlotArm();

    // Reads the config, logs, installs the death-notice consumer, and STOPS when the operator
    // left no arm at all. Cold: called exactly once per process, from the latch below - i.e. at
    // the first twin lookup, which is the first moment an arm is actually needed. A process
    // that never twins anything needs no arm and is not stopped.
    Bool ResolveEsprytSlotTablesArm();

    // True when this process runs the {slot, gen} arm. Fixed for the life of the process: the
    // two arms hold their twins in different containers, so flipping mid-run would strand them.
    //
    // INLINE on purpose. Every Find / GetOrCreate / HandleOf / ForEachLive on the twin tables
    // consults it, i.e. it is on the per-draw path several times per draw. As an out-of-line
    // function in Managers.cpp (no LTO in any shipped configuration) that was a call through
    // the PLT per lookup; here the caller sees a guard-variable load and a perfectly-predicted
    // branch, and the arm dispatch folds into the caller.
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
            // LIVENESS ONLY, and only for ForEachLive(), which locks it so the callee holds a
            // strong ref. Never compared against another object to decide identity - that is
            // what Gen is for - never dereferenced for its address, and never read to decide
            // whether the slot is dead: death is announced, not discovered.
            StateWeakPtr stateRef;
            // The generation this entry's twin was built for. An entry whose Gen no longer
            // matches the allocator's is a twin of the slot's PREVIOUS owner.
            Uint32 Gen = 0;
            Bool Live = false;
        };

        // Every constructor links the table into the per-type holder list and the destructor
        // unlinks it, so a by-value copy (the ScopedDirectGLESTextureBindings fixture's saved
        // registry) is a holder for exactly as long as it exists. Copy and move carry the
        // ENTRIES and the memo; the links are the table's own and are never copied.
        BackendSlotTable() { LinkHolder(); }
        BackendSlotTable(const BackendSlotTable& other):
            m_slots(other.m_slots),
            m_nullTwin(other.m_nullTwin),
            m_memoLifetimeId(other.m_memoLifetimeId),
            m_memoHandle(other.m_memoHandle) {
            LinkHolder();
        }
        BackendSlotTable(BackendSlotTable&& other) noexcept:
            m_slots(std::move(other.m_slots)),
            m_nullTwin(std::move(other.m_nullTwin)),
            m_memoLifetimeId(other.m_memoLifetimeId),
            m_memoHandle(other.m_memoHandle) {
            other.m_slots.clear();
            other.ForgetHandle();
            LinkHolder();
        }
        BackendSlotTable& operator=(const BackendSlotTable& other) {
            if (this != &other) {
                m_slots = other.m_slots;
                m_nullTwin = other.m_nullTwin;
                m_memoLifetimeId = other.m_memoLifetimeId;
                m_memoHandle = other.m_memoHandle;
            }
            return *this;
        }
        BackendSlotTable& operator=(BackendSlotTable&& other) noexcept {
            if (this != &other) {
                m_slots = std::move(other.m_slots);
                m_nullTwin = std::move(other.m_nullTwin);
                m_memoLifetimeId = other.m_memoLifetimeId;
                m_memoHandle = other.m_memoHandle;
                other.m_slots.clear();
                other.ForgetHandle();
            }
            return *this;
        }
        ~BackendSlotTable() { UnlinkHolder(); }

        // Resolve-or-create. The handle comes from the client allocator keyed on the frontend
        // object's lifetime id, so two calls for the same live object always land on the same
        // slot, and a successor object at the same heap address never does.
        BackendPtr& GetOrCreate(const StatePtr& stateObj) {
            // No assert on null here, unlike the map arm: null is TOLERATED, so a DEBUG build
            // must not trap where the release build quietly does the documented thing.
            if (stateObj == nullptr) {
                // The registry this replaces inserted a null KEY and handed back that entry's
                // twin (DirectGLES.cpp's SyncTextureObjectToBackend documents relying on
                // exactly that tolerance), so a release build never dereferenced null here.
                // Keep the shape exactly, INCLUDING across calls: the map kept its null-keyed
                // entry, so a second null call was handed the same twin the first one got.
                // Resetting here instead would have destroyed it - an arm difference in the one
                // path that documents relying on this. One per-table parking slot, never live,
                // never handed a handle, because a null object has no identity and
                // therefore cannot have a {slot, gen}.
                return m_nullTwin;
            }

            // D13: the teardown sentinel is armed by the slot table's first insertion. Twin
            // creation is the moment a driver-owned id starts needing a guarded destructor;
            // this is the cold path, so the once-guard costs nothing per draw. On the legacy
            // arm StateBackendObjectRegistry::GetOrCreate arms it itself.
            EnsureProcessTeardownSentinel();

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

        // P3a: resolve-or-create BY HANDLE, and it is the shape that discharges the debt this
        // header records against itself at the top of the file.
        //
        // The overload above mints - it calls MGPipeSlots().Acquire off a frontend object's
        // lifetime id, from inside MG_Backend - which is monolith glue: a handle is minted by
        // the CLIENT, and under a real split neither the object nor its lifetime id exists on
        // this side. This overload never touches the allocator at all. The handle ARRIVED, in
        // the call's payload, already minted by the side that owns minting; all this does is
        // index the slot, notice a generation that no longer matches (the slot was recycled,
        // so the twin at it describes driver ids the new resource never made) and hand back
        // the twin pointer. FindByHandle beside it is the same shape and already existed.
        //
        // No StatePtr, therefore no Entry::stateRef: the weak pointer is liveness for
        // ForEachLive() and a handle-keyed entry has no frontend object to weakly hold. Such
        // an entry is therefore invisible to ForEachLive, which is correct - the one direct
        // iteration site walks texture twins, and it is not one of these tables.
        //
        // Death stays ANNOUNCED, as it is on the other overload: for a handle-keyed kind the
        // announcement is the family's own destroy call, not the shared death notice, and the
        // slot is freed by the CLIENT after that call returns.
        //
        // UNUSED AT THE CONTRACT COMMIT, deliberately: it is a member of a class template, so
        // an uninstantiated one costs nothing anywhere, and the backend package is what gives
        // it its first caller.
        BackendPtr& GetOrCreate(MG_Pipe::MGPipeHandle handle) {
            MOBILEGL_ASSERT(!MG_Pipe::MGPipeHandleIsNull(handle),
                            "GetOrCreate(handle) named the reserved null handle");
            if (MG_Pipe::MGPipeHandleIsNull(handle)) return m_nullTwin;

            // Same arming as the minting overload, and for the same reason: twin creation is
            // the moment a driver-owned id starts needing a guarded destructor.
            EnsureProcessTeardownSentinel();

            Entry& entry = EntryAt(handle.Slot);
            if (entry.Live && entry.Gen != handle.Gen) entry.backend.reset();
            entry.Gen = handle.Gen;
            entry.Live = true;
            return entry.backend;
        }

        // Null when no live twin of this object exists. Unlike the registry's Find this NEVER
        // mutates the table, so the returned pointer survives any later Find on it; only a
        // GetOrCreate that grows the vector can move it, and callers that hold one across a
        // possible insertion still copy the BackendPtr out.
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
        //
        // A NULL answer is never memoised. The memo is per table and the allocator is per
        // kind, so with two holders of one kind the OTHER table can be the one that acquires;
        // a cached "no handle" here would then outlive the twin's creation over there, and
        // nothing on this table's own acquire path would ever refresh it. A miss costs the
        // allocator probe it always cost; a hit is refreshed the moment anyone acquires.
        MG_Pipe::MGPipeHandle HandleOf(const StateObject* stateObj) const {
            if (stateObj == nullptr) return MG_Pipe::kMGPipeNullHandle;
            const Uint64 lifetimeId = stateObj->GetLifetimeId();
            if (lifetimeId == m_memoLifetimeId) return m_memoHandle;
            const MG_Pipe::MGPipeHandle handle =
                MG_Pipe::MGPipeSlots().FindByLifetimeId(kKind, lifetimeId);
            if (!MG_Pipe::MGPipeHandleIsNull(handle)) RememberHandle(lifetimeId, handle);
            return handle;
        }

        // P2 step e2's backend half. The frontend object with this lifetime id has just been
        // DESTROYED: resolve its handle ONCE, drop its twin in EVERY table of this type, and
        // return the slot to the allocator - in that order, because the allocator forgets the
        // lifetime id on Free and a holder told second could no longer resolve it.
        //
        // The slot is returned whether or not any holder still had a twin at it: the lifetime
        // id is dead and MG_State never hands one out twice, so nothing can acquire it again,
        // and a slot minted for it that no table holds (a table reset with `= {}` drops its
        // entries without freeing) would otherwise stay allocated for the life of the process.
        //
        // STATIC, and deliberately so: a notice is about an object, not about a table, and
        // "which table holds it" is exactly the question that produced the two-holder leak.
        // Returns whether the object had a slot of this kind, i.e. whether anything was freed;
        // a second call for the same id answers false because the allocator no longer maps it.
        static Bool OnFrontendObjectDestroyed(Uint64 lifetimeId) {
            const MG_Pipe::MGPipeHandle handle =
                MG_Pipe::MGPipeSlots().FindByLifetimeId(kKind, lifetimeId);
            if (MG_Pipe::MGPipeHandleIsNull(handle)) return false;
            for (BackendSlotTable* holder = s_firstHolder; holder != nullptr;) {
                // The successor is read BEFORE the release: ReleaseTwinAt runs the twin's
                // destructor, which is a driver call, and nothing that outlives it may be a
                // reference into this holder.
                BackendSlotTable* const next = holder->m_nextHolder;
                holder->ReleaseTwinAt(handle);
                holder = next;
            }
            MG_Pipe::MGPipeSlots().Free(kKind, handle);
            return true;
        }

        // How many tables of this type exist right now. For the tests that pin the holder
        // list; nothing on a shipping path asks.
        static Uint32 HolderCount() {
            Uint32 count = 0;
            for (const BackendSlotTable* holder = s_firstHolder; holder != nullptr;
                 holder = holder->m_nextHolder) {
                ++count;
            }
            return count;
        }

        // fn(const StatePtr& state, const BackendPtr& twin) over every live, still-owned entry.
        // Replaces the registry's begin()/end(), whose iterator exposed the raw frontend
        // address as the map key - the one place the backend read an identity it must not have.
        // The state object is handed over as a STRONG reference, so the callee cannot be handed
        // a dangling key the way the old iteration could.
        template <typename Fn>
        void ForEachLive(Fn&& fn) const {
            // Index loop and a COPIED twin, not a range-for over references: fn is arbitrary
            // backend code, and a nested GetOrCreate on this table would resize m_slots and
            // invalidate both the iterator and any reference into the vector that outlives the
            // call. The one caller today happens not to insert; that is not a property the
            // walk should depend on.
            for (SizeT slot = 0; slot < m_slots.size(); ++slot) {
                const Entry& entry = m_slots[slot];
                if (!entry.Live || !entry.backend) continue;
                const StatePtr state = entry.stateRef.lock();
                if (!state) continue;
                const BackendPtr twin = entry.backend;
                fn(state, twin);
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
        // Drop the twin at `handle` if THIS table holds it. Frees nothing: the slot belongs to
        // the kind, not to the table, and OnFrontendObjectDestroyed returns it once, after
        // every holder has let go.
        Bool ReleaseTwinAt(MG_Pipe::MGPipeHandle handle) {
            // Forget the memo whenever it names this slot, even if this table has no entry
            // there: a memo can be a handle learned from the allocator for an object another
            // holder twinned, and it must not survive the slot's next handout.
            if (m_memoHandle.Slot == handle.Slot) ForgetHandle();
            if (handle.Slot >= m_slots.size()) return false;
            // The twin's destructor is a driver call and could, in principle, re-enter
            // GetOrCreate on this table and resize m_slots. So NOTHING that outlives the
            // destructor may be a reference into m_slots: the twin is moved out into a local,
            // the entry is finished with, and only then is the local released.
            BackendPtr dead;
            {
                Entry& entry = m_slots[handle.Slot];
                if (!entry.Live || entry.Gen != handle.Gen) return false;
                dead = std::move(entry.backend);
                entry.backend.reset();
                entry.stateRef.reset();
                entry.Live = false;
            }
            dead.reset();
            return true;
        }

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

        // The holder list: intrusive and doubly linked, so registering and unregistering are
        // two pointer writes with no allocation, and its head is a constant-initialised
        // static - which is what lets the process-lifetime registry globals in Managers.cpp
        // link themselves in from their own constructors with no initialisation-order
        // question to answer. Single-threaded, like every table it links (the tables live and
        // die on the context thread, as the notice they answer does).
        void LinkHolder() {
            m_prevHolder = nullptr;
            m_nextHolder = s_firstHolder;
            if (s_firstHolder != nullptr) s_firstHolder->m_prevHolder = this;
            s_firstHolder = this;
        }
        void UnlinkHolder() {
            if (m_prevHolder != nullptr) {
                m_prevHolder->m_nextHolder = m_nextHolder;
            } else {
                s_firstHolder = m_nextHolder;
            }
            if (m_nextHolder != nullptr) m_nextHolder->m_prevHolder = m_prevHolder;
            m_prevHolder = nullptr;
            m_nextHolder = nullptr;
        }

        static inline BackendSlotTable* s_firstHolder = nullptr;
        BackendSlotTable* m_prevHolder = nullptr;
        BackendSlotTable* m_nextHolder = nullptr;

        // Indexed by MGPipeHandle::Slot; [0] is the reserved slot and is never live.
        Vector<Entry> m_slots;
        // Handed back by GetOrCreate for a null state object. Never live, never handed a handle.
        BackendPtr m_nullTwin;

        // ONE-entry resolution memo, lifetimeId -> handle. It exists because without it every
        // resolution goes through the allocator's ByLifetimeId hash, which the deleted
        // TwinLookupMemos existed to avoid and which D13 promises to replace with "direct slot
        // indexing".
        //
        // It is one entry and therefore only helps a caller that asks for the SAME object twice
        // running - ResolveVaoTwin and SyncCurrentProgram do, once per draw each. Two callers
        // it does NOT help, recorded rather than claimed away: BindCurrentFBO resolves BOTH
        // targets in a frame, and ResolveUnitSamplerBackend asks for a different sampler per
        // texture unit, so both thrash a single-entry memo and pay the probe P1 did not (P1 had
        // a per-unit memo and a direct-mapped 6-slot array there). Making the memo per-unit /
        // per-target is the fix, and G11 - the device-side gate that would price it - is owed.
        //
        // It cannot serve a stale answer, by three independent arguments:
        //   * the key is a lifetime id, which MG_State never hands out twice, so a recycled
        //     heap address cannot hit this memo the way it could hit an address-keyed one;
        //   * a null answer is never stored, so another holder's acquire cannot be hidden by
        //     a "no handle" this table remembered earlier; and
        //   * even a hit for a slot that has since been freed and re-handed is caught, because
        //     the caller resolves the handle through FindByHandle, which compares Gen.
        // Cleared anyway when a death notice names the memoised slot. 0 is never a live
        // lifetime id (MG_State's counters start at 1), so a zeroed memo is a guaranteed miss.
        mutable Uint64 m_memoLifetimeId = 0;
        mutable MG_Pipe::MGPipeHandle m_memoHandle = MG_Pipe::kMGPipeNullHandle;
    };

#endif // MOBILEGL_PIPE_PUSH
} // namespace MobileGL::MG_Backend::DirectGLES
