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

// P14 S6: the key this table is filed under includes the calling thread's {session, share
// group}, and the probe that answers it lives on the backend's public surface beside S4's
// native-tuple key (DirectGLES.h). Included here so this header is self-contained for the two
// translation units that include it without copying Managers.h's include order.
#include "DirectGLES.h"

#if MOBILEGL_BUILD_DISAGGREGATED
#include <MG_Pipe/PipeSessionFail.h>
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
// from their destructor carrying {kind, lifetimeId, handle}; the backend consumes it in
// Managers.cpp and releases the twin BY THAT HANDLE in every holder of the kind
// (ReleaseTwinByHandle below - the same release the wire's object_death runs on a server), and
// the CLIENT returns the slot after the notice (PipeFill.cpp's NotifyAndFree). So:
//   * there is NO draw-path tick, NO creation tick and NO sweep of any kind;
//   * a twin, and the driver storage it owns, is freed when the application lets go of the
//     object rather than up to 64 creations or 1024 draw ticks later. That is what
//     Managers.h's "dead gigabytes" note asked for.
//
// EVERY HOLDER OF THE KIND, not one. Two live tables of one kind is a real configuration - the
// ScopedDirectGLESTextureBindings fixture keeps a by-value copy of the Texture registry for the
// length of a test, and a context reset does the same in reverse. Every table therefore links
// itself into a per-table-type list at construction and out at destruction, and one release
// drops the twin in each holder by handle. (The list is per table TYPE; the kind is the type's
// template parameter, and each of the six kinds has exactly one table type in this backend.)
//
// A destructor that runs after exit() has begun has its notice dropped by InProcessTeardown(),
// and that twin is then a DELIBERATE leak (the process is exiting, the driver reclaims the
// object, and a twin destructor must not call into a driver that may already be unloaded).
//
// P13: THE TABLE IS HANDLE-ONLY. The frontend-keyed half - a GetOrCreate that minted handles
// off a frontend object's lifetime id from inside MG_Backend, the object-keyed Find / HandleOf
// with their lifetime-id memo, the weak per-entry state note and the lifetime-id death walk -
// was monolith glue, unreachable once every family ran the record arm, and is deleted. A handle
// is minted by the CLIENT and arrives in the record; nothing on this side touches the client's
// slot allocator (the link ratchet's P13 exit gate pins that).
namespace MobileGL::MG_Backend::DirectGLES {


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
    // This process's verdict, read off MG_Config::Features. Latches nothing and stops nothing.
    EsprytSlotArmVerdict CurrentEsprytSlotArmVerdict();

    void DiagnoseEsprytSlotArm();

    // Reads the config, logs, installs the death-notice consumer, and STOPS when the operator
    // left no arm at all. Cold: called exactly once per process, from the latch below - i.e. at
    // the first twin lookup, which is the first moment an arm is actually needed. A process
    // that never twins anything needs no arm and is not stopped.
    Bool ResolveEsprytSlotTablesArm();

    // True when this process runs the {slot, gen} arm. Fixed for the life of the process: the
    // two arms hold their twins in different containers, so flipping mid-run would strand them.
    //
    // INLINE on purpose. Every FindByHandle / GetOrCreate / ForEachLive on the twin tables
    // consults it, i.e. it is on the per-draw path several times per draw. As an out-of-line
    // function in Managers.cpp (no LTO in any shipped configuration) that was a call through
    // the PLT per lookup; here the caller sees a guard-variable load and a perfectly-predicted
    // branch, and the arm dispatch folds into the caller.
    inline Bool EsprytSlotTablesEnabled() {
        static const Bool enabled = ResolveEsprytSlotTablesArm();
        return enabled;
    }

    // =====================================================================================
    // P14 S6 (docs/Disaggregated/design/11-state-ownership.md): THE TWIN TABLE IS PER
    // {session, share group}, NOT PER PROCESS.
    // =====================================================================================
    //
    // WHAT WAS WRONG, in one sentence: a twin is a DRIVER object - a GL buffer id, a program, a
    // texture, the storage behind them - so it belongs to one native context's object namespace,
    // and a table keyed by {slot, gen} ALONE resolves the same handle value to the same twin
    // whichever context asked. The frontend gives two contexts that do NOT share a group two
    // independent name spaces, so the same {slot, gen} in both is legal and means two different
    // objects; one process-wide table made the second context adopt the first's twin (including
    // its driver ids, its persistent map and its pooled store). At the same time two contexts of
    // ONE group - which must see one set of objects - were served by whichever of them twinned
    // first, i.e. by luck rather than by the group the two agree on.
    //
    // THE KEY IS the same pair the applier files its OBJECT RECORDS under: {session, share
    // group}. Same probe, same `0 means this context started its own group` rule (see
    // DirectGLES.h's TwinKey and SessionRuntime.cpp's ResolveThreadTwinKey), so a record and the
    // twin it describes are always in the same bucket. Session is in the key because a twin
    // names a driver id and no driver id crosses a connection - two connections in one process
    // are two clients, and nothing may be shared with either from the other.
    //
    // THE HOLDER LIST STAYS PROCESS-WIDE AND THAT IS NOT A COMPROMISE. A notice is about an
    // OBJECT, not about a bucket: ReleaseTwinByHandle must reach
    // the by-value copies a fixture or a context reset is holding, whichever bucket they sit in,
    // and "which bucket holds it" is exactly the question that produced the two-holder leak the
    // list exists to fix. Each SHARED holder of the list is ONE bucket; the walk therefore covers
    // every bucket of every table.
    //
    // THE SAME TABLE OBJECT IS REUSED, NOT REPLACED, and that is what keeps ~200 call sites
    // alive: `g_backendTextureObjects.Find(...)` still answers from the bucket the calling thread
    // is in, because the object the global names multiplexes on the key at each lookup. A caller
    // that must address a bucket OTHER than the calling thread's - the death walks, the
    // session-teardown drop - takes Shared() or calls the static entries, which walk buckets;
    // and a caller that wants the whole process's view (the teardown, the tests) takes Snapshot().
    // FORWARD-DECLARED, because the registry below is DEFINED before the table it holds; the
    // members that name the table are defined out of line at the bottom of this header.
    template <typename StateObject, typename BackendObject, MG_Pipe::MGPipeKind kKind>
    class BackendSlotTable;

    template <typename StateObject, typename BackendObject, MG_Pipe::MGPipeKind kKind>
    class SlotTableRegistry {
    public:
        using Table = BackendSlotTable<StateObject, BackendObject, kKind>;
        using Key = TwinKey;
        using StatePtr = SharedPtr<StateObject>;
        using BackendPtr = SharedPtr<BackendObject>;
        // The bound the two tables that ARE a bucket registry have always spelled by name; the
        // six registries reach it through their own `SlotTable` alias.
        static constexpr Uint32 kMaxHandleSlot = 1u << 20;

        // One bucket. SHARED, so a caller may keep it across other lookups: the map owns the
        // pointee and never moves it.
        SharedPtr<Table> Shared(const Key& key) const {
            // The common case on the hot path: the same key as last time. One compare, no probe.
            if (m_memoTable != nullptr && m_memoKey == key) return m_memoTable;
            const auto found = m_buckets.find(key);
            if (found != m_buckets.end()) {
                m_memoKey = key;
                m_memoTable = found->second;
                return found->second;
            }
            SharedPtr<Table> fresh = MakeShared<Table>();
            fresh->SetOwnerSession(key.SessionKey);
            m_buckets.emplace(key, fresh);
            m_memoKey = key;
            m_memoTable = fresh;
            return fresh;
        }

        // The calling thread's bucket - the ONE entry every in-place call site below resolves
        // through, and the process-wide answer on a thread that belongs to no session.
        //
        // CONST, AND IT ANSWERS A MUTABLE BUCKET: the index is `mutable` because a LOOKUP may
        // have to create the bucket it is about to miss in (a handle with no twin yet is the
        // ordinary case), and the bucket's own entries are the table's business rather than the
        // registry's. The alternative - a const overload returning "no table" - would make every
        // read-only caller handle a case that cannot arise.
        Table& ForCallingThread() const {
            Key key;
            CurrentTwinKey(&key);
            return *Shared(key);
        }

        // Every bucket, in no particular order.
        Vector<SharedPtr<Table>> Snapshot() const {
            Vector<SharedPtr<Table>> all;
            all.reserve(m_buckets.size());
            for (const auto& bucket : m_buckets) all.push_back(bucket.second);
            return all;
        }

        SizeT BucketCount() const { return m_buckets.size(); }

        // THE PER-BUCKET ONE-SHOT, because a bucket held by SharedPtr is never dropped by an
        // assignment: a caller that means "this context/group is gone" has to be able to say so,
        // and the key is how it says which one. The bucket LEAVES the index before its twins are
        // released, so a twin destructor that re-entered Shared() cannot find a half-erased one.
        Bool Drop(const Key& key) {
            const auto found = m_buckets.find(key);
            if (found == m_buckets.end()) return false;
            SharedPtr<Table> dropped = found->second;
            m_buckets.erase(found);
            if (m_memoTable == dropped) {
                m_memoKey = Key{};
                m_memoTable = nullptr;
            }
            dropped->Reset();
            return true;
        }

        // EVERY bucket of one session, for the teardown of a session that owned several groups.
        // The session is the key's high half, so this is one pass and no per-group bookkeeping -
        // and it is the difference between "this session's twins go" and "the process's do".
        Uint32 DropSession(Uint64 sessionKey) {
            Uint32 dropped = 0;
            for (auto bucket = m_buckets.begin(); bucket != m_buckets.end();) {
                if (bucket->first.SessionKey != sessionKey) {
                    ++bucket;
                    continue;
                }
                SharedPtr<Table> table = bucket->second;
                if (m_memoTable == table) {
                    m_memoKey = Key{};
                    m_memoTable = nullptr;
                }
                bucket = m_buckets.erase(bucket);
                table->Reset();
                ++dropped;
            }
            return dropped;
        }

        // Replace this registry with an empty one, releasing every bucket's twins. This is what
        // `registry = {}` meant when the registry WAS one table; a caller that must keep the
        // tables alive past the call takes a Snapshot() first.
        void Reset() {
            const Vector<SharedPtr<Table>> all = Snapshot();
            m_buckets.clear();
            m_memoKey = Key{};
            m_memoTable = nullptr;
            for (const SharedPtr<Table>& table : all) table->Reset();
        }

        // ---- the calling thread's bucket, member for member ---------------------------------
        //
        // THIS IS WHY THE SHAPE COST THE CALL SITES NOTHING. Every handle-keyed entry the table
        // has is repeated here and forwarded to the bucket the calling thread resolves to, so
        // `g_backendTextureObjects.FindByHandle(h)` and `g_backendBufferResources.GetOrCreate(h)`
        // - unchanged source - answer from the right context's object namespace. A caller that
        // must name a bucket OTHER than its own (a death walk, a case) takes Shared()/Snapshot()
        // and calls the table directly.
        BackendPtr& GetOrCreate(MG_Pipe::MGPipeHandle handle) {
            return ForCallingThread().GetOrCreate(handle);
        }
        BackendPtr* FindByHandle(MG_Pipe::MGPipeHandle handle) {
            return ForCallingThread().FindByHandle(handle);
        }
        const BackendPtr* FindByHandle(MG_Pipe::MGPipeHandle handle) const {
            return ForCallingThread().FindByHandle(handle);
        }
        // Resolve-or-create in a bucket the caller NAMES rather than the one it is in. The
        // monolith-glue frontend-keyed entry (StateBackendObjectRegistry::GetOrCreate below)
        // needs it: a state object handed over on the client side of a sync is described by a
        // record in the CALLING thread's group, and a twin built for it must land in the same
        // bucket as that record.
        BackendPtr& GetOrCreateIn(const Key& key, MG_Pipe::MGPipeHandle handle) {
            return Shared(key)->GetOrCreate(handle);
        }
        Uint32 LiveGenAt(Uint32 slot) const { return ForCallingThread().LiveGenAt(slot); }
        template <typename Fn>
        void ForEachLive(Fn&& fn) const {
            // THE CALLING THREAD'S BUCKET, not every bucket, and that is the semantic answer
            // rather than a shortcut: a caller walking twins is asking "which of the objects I
            // can see need this", and what it can see is its own share group. Every bucket in
            // one process would walk a neighbour session's objects.
            ForCallingThread().ForEachLive(fn);
        }
        // Every bucket, in one pass. The caller receives the key with the handle so it can tell
        // which context a twin belongs to - the only walks that need the whole process's view
        // are a diagnostic and a teardown.
        template <typename Fn>
        void ForEachBucket(Fn&& fn) const {
            for (const auto& bucket : m_buckets) {
                bucket.second->ForEachLive(
                    [&](MG_Pipe::MGPipeHandle handle, const BackendPtr& twin) { fn(bucket.first, handle, twin); });
            }
        }
        // The process-wide answer for the two statics: a notice is about an OBJECT, so it must
        // reach the twin in EVERY bucket of EVERY holder. They forward to the table's own
        // statics, which walk the holder list - each SHARED holder of it is one bucket.
        static Bool ReleaseTwinByHandle(MG_Pipe::MGPipeHandle handle) {
            return Table::ReleaseTwinByHandle(handle);
        }
        // The instance spelling, and it HANDS THE TWIN OUT rather than answering a Bool: the two
        // tables that ARE a bucket registry (buffers, sampler views) have always been called as
        // `g_backendBufferResources.ReleaseByHandle(res)` and their callers still have to decide
        // what happens to the driver id the twin owns. The bucket is the CALLING THREAD'S, which
        // is the bucket that twin was created in.
        BackendPtr ReleaseByHandle(MG_Pipe::MGPipeHandle handle) {
            return ForCallingThread().ReleaseByHandle(handle);
        }
        static Uint32 HolderCount() { return Table::HolderCount(); }
        Uint32 LiveCount() const { return ForCallingThread().LiveCount(); }
        Uint32 CompositeLiveCount() const { return ForCallingThread().CompositeLiveCount(); }
        SizeT OrdinaryCapacityForTest() const { return ForCallingThread().OrdinaryCapacityForTest(); }
        SizeT CompositeCapacityForTest() const { return ForCallingThread().CompositeCapacityForTest(); }

        // ---- the bucket index's own controls, for the two tables that ARE a registry --------
        //
        // The six TwinRegistry registries carry the same three under the names in Managers.h
        // (DropBucket / DropBucketsOfSession / ResetBuckets); they are repeated here because
        // SlotTableRegistry IS the public type of the buffer and sampler-view tables, so a caller
        // of those has no registry object in between.
        Bool DropBucket(const Key& key) { return Drop(key); }
        // Drop every bucket of one session; answers how many groups that took with it.
        Uint32 DropBucketsOfSession(Uint64 sessionKey) { return DropSession(sessionKey); }
        // Empty every bucket (the process-shaped world's teardown, key {0, 0}).
        void ResetBuckets() { Reset(); }

    private:
        // ONE BUCKET PER LIVE {session, share group}, held by SHARED pointer so a caller may
        // snapshot one across other lookups and so `Drop` can take a bucket's twins away
        // explicitly - an embedded table could do neither. `std::unordered_map` rather than the
        // tree's own flat Vector: this map is probed once per twin lookup on the draw path.
        mutable std::unordered_map<Key, SharedPtr<Table>, TwinKeyHash> m_buckets;
        // Hot-path memo. A registry is per KIND and each kind is touched several times per draw;
        // a thread only ever moves between buckets when its session binds another context.
        mutable SharedPtr<Table> m_memoTable;
        mutable Key m_memoKey;
    };

    template <typename StateObject, typename BackendObject, MG_Pipe::MGPipeKind kKind>
    class BackendSlotTable {
    public:
        using BackendPtr = SharedPtr<BackendObject>;

        // The largest slot index this table will grow to for a handle that ARRIVED in a call's
        // payload. Slots are dense and allocated per kind, so a million of one kind is already
        // far past any application's live object count; the cap is here because the alternative
        // is letting a corrupt 32-bit slot decide a vector resize. See GetOrCreate(MGPipeHandle).
        static constexpr Uint32 kMaxHandleSlot = 1u << 20;

        // P5e (id), CONTRACT-P5E §4.3: THE COMPOSITE BAND, and it is a P5e PREREQUISITE rather
        // than a follow-up. ShaderCso's top 1/16 of slot space (from
        // kMGPipeShaderCsoCompositeSlotBase = 983040) is the program-pipeline composites'
        // (MGPipeHandles.h); EntryAt below indexes m_slots BY SLOT and resizes to it, so a
        // single composite used to grow g_backendProgramObjects to ~983k entries of ~40 B -
        // ~40 MB for one program pipeline. Today that is reachable only through
        // GetOrCreate(StatePtr) for a composite program; after the rekey the by-handle
        // resolution IS the ordinary path, so every composite bind would pay it.
        //
        // A SECOND VECTOR RATHER THAN MORE OF THE FIRST, exactly as the allocator
        // (SlotAllocator.h's KindState::BandSlots) and the applier (PipeApply.h's
        // CompositeShaderCsos) already do, and for the same reason: both spaces stay dense
        // against their own high-water mark, which is the property that lets the server index
        // rather than hash. The band is EMPTY for every kind but ShaderCso and costs those
        // kinds one empty Vector per table.
        static constexpr Bool kHasCompositeBand = (kKind == MG_Pipe::MGPipeKind::ShaderCso);
        static constexpr SizeT kMaxBandSlots =
            MG_Pipe::kMGPipeShaderCsoSlotLimit - MG_Pipe::kMGPipeShaderCsoCompositeSlotBase;

        struct Entry {
            BackendPtr backend;
            // The generation this entry's twin was built for. An entry whose Gen no longer
            // matches the allocator's is a twin of the slot's PREVIOUS owner.
            Uint32 Gen = 0;
            Bool Live = false;
        };

        // Every constructor links the table into the per-type holder list and the destructor
        // unlinks it, so a by-value copy (the ScopedDirectGLESTextureBindings fixture's saved
        // registry) is a holder for exactly as long as it exists. Copy and move carry the
        // ENTRIES; the links are the table's own and are never copied.
        BackendSlotTable() { LinkHolder(); }
        BackendSlotTable(const BackendSlotTable& other):
            m_ownerSession(other.m_ownerSession),
            m_slots(other.m_slots),
            m_band(other.m_band),
            m_nullTwin(other.m_nullTwin) {
            LinkHolder();
        }
        BackendSlotTable(BackendSlotTable&& other) noexcept:
            m_ownerSession(other.m_ownerSession),
            m_slots(std::move(other.m_slots)),
            m_band(std::move(other.m_band)),
            m_nullTwin(std::move(other.m_nullTwin)) {
            other.m_slots.clear();
            other.m_band.clear();
            LinkHolder();
        }
        BackendSlotTable& operator=(const BackendSlotTable& other) {
            if (this != &other) {
                m_slots = other.m_slots;
                m_band = other.m_band;
                m_nullTwin = other.m_nullTwin;
            }
            return *this;
        }
        BackendSlotTable& operator=(BackendSlotTable&& other) noexcept {
            if (this != &other) {
                m_slots = std::move(other.m_slots);
                m_band = std::move(other.m_band);
                m_nullTwin = std::move(other.m_nullTwin);
                other.m_slots.clear();
                other.m_band.clear();
            }
            return *this;
        }
        ~BackendSlotTable() { UnlinkHolder(); }

        // The served session whose bucket this table is (SlotTableRegistry::Shared), 0 for a
        // table that belongs to none (the process-wide registry on a sessionless thread, a
        // fixture's copy of one).
        void SetOwnerSession(Uint64 session) { m_ownerSession = session; }
        Uint64 OwnerSession() const { return m_ownerSession; }

        // P3a: resolve-or-create BY HANDLE - since P13 the only resolve-or-create there is. The
        // handle ARRIVED, in the call's payload, already minted by the client; this never
        // touches the allocator. It indexes the slot, notices a generation that no longer
        // matches (the slot was recycled, so the twin at it describes driver ids the new
        // resource never made) and hands back the twin pointer.
        //
        // Death stays ANNOUNCED: the family's own destroy call or the shared death notice
        // releases the twin by handle, and the slot is freed by the CLIENT after that returns.
        //
        BackendPtr& GetOrCreate(MG_Pipe::MGPipeHandle handle) {
            MOBILEGL_ASSERT(!MG_Pipe::MGPipeHandleIsNull(handle),
                            "GetOrCreate(handle) named the reserved null handle");
            if (MG_Pipe::MGPipeHandleIsNull(handle)) return m_nullTwin;

            // A slot index that ARRIVED in a payload indexes a vector this call would RESIZE,
            // and nothing between the payload and here bounds it: the applier's blob gates sit
            // in front of the vertex-input family, not in front of the resource family, which
            // dispatches ops->Create(record.Res, ...) straight through. There is no allocator
            // constant to check against on this side - the allocator is the client's - so this
            // is a sanity cap and is documented as one: kMaxHandleSlot entries of one kind is
            // already orders of magnitude past any real GL object count, while a corrupt 32-bit
            // slot asks for a four-billion-entry resize.
            if (handle.Slot >= kMaxHandleSlot) {
#if MOBILEGL_BUILD_DISAGGREGATED
                // PH-2: handle.Slot is peer supplied on the disaggregated arm. Release builds
                // must publish a named protocol fault instead of silently returning m_nullTwin.
                MG_Pipe::MGPipeSessionFail( // @Ph-declined (ID-P7-1): PH-2 stays Fatal, CONTRACT-P7 §12
                    MG_Pipe::MGPipeFatalFamily::ProtocolCorruption,
                    "MGPipe: Fatal{ProtocolCorruption, \"BackendSlotTable.HandleSlot\"} - "
                    "GetOrCreate(handle) named slot %u, past this table's %u bound",
                    handle.Slot, kMaxHandleSlot);
#else
                MOBILEGL_ASSERT(false, "GetOrCreate(handle) named slot %u, past this table's %u bound",
                                handle.Slot, kMaxHandleSlot);
                return m_nullTwin;
#endif
            }

            // Twin creation is the moment a driver-owned id starts needing a guarded destructor.
            EnsureProcessTeardownSentinel();

            // THE TWO DIRECTIONS ARE NOT SYMMETRIC. The handle arrived in a payload, so
            // `handle.Gen < entry.Gen` is a reachable input, and adopting it would destroy the INCUMBENT LIVE twin - a driver buffer id,
            // a persistent map, a pooled store, released by a defaulted destructor that issues
            // no glDeleteBuffers and no pool enrolment - and then stamp the slot back to the
            // dead resource's generation, after which the incumbent's own FindByHandle refuses
            // it and it is silently handed a fresh, empty twin. That is a leak AND a resource
            // that loses its storage with no diagnostic, i.e. the shape commit d7655247 fixed
            // and the thing MGPipeHandle::Gen exists to prevent. So: forward is a recycle and
            // resets the twin, BACKWARD is refused - which is the same answer FindByHandle
            // below already gives the same input.
            Entry& entry = EntryAt(handle.Slot);
            if (entry.Live && entry.Gen > handle.Gen) {
#if MOBILEGL_BUILD_DISAGGREGATED
                // PH-2: a stale peer generation must not silently shed the incumbent twin in a
                // release server. The funnel names the exact identity fault and notifies the peer.
                MG_Pipe::MGPipeSessionFail( // @Ph-declined (ID-P7-1): PH-2 stays Fatal, CONTRACT-P7 §12
                    MG_Pipe::MGPipeFatalFamily::ProtocolCorruption,
                    "MGPipe: Fatal{ProtocolCorruption, \"BackendSlotTable.Generation\"} - "
                    "GetOrCreate(handle) named generation %u at slot %u, behind live generation %u",
                    handle.Gen, handle.Slot, entry.Gen);
#else
                MOBILEGL_ASSERT(false,
                                "GetOrCreate(handle) named generation %u at slot %u, which is BEHIND "
                                "the live entry's %u - refusing rather than destroying the incumbent",
                                handle.Gen, handle.Slot, entry.Gen);
                return m_nullTwin;
#endif
            }
            if (entry.Live && entry.Gen != handle.Gen) entry.backend.reset();
            entry.Gen = handle.Gen;
            entry.Live = true;
            return entry.backend;
        }

        // The generation of the LIVE entry at this slot, or 0 when the slot is out of range or
        // holds no live entry. It exists so a caller can DIAGNOSE - in a release build, where
        // MOBILEGL_ASSERT is inert - the refusal GetOrCreate(handle) above performs silently.
        Uint32 LiveGenAt(Uint32 slot) const {
            const Entry* const entry = EntryOrNull(slot);
            if (entry == nullptr) return 0;
            return entry->Live ? entry->Gen : 0;
        }

        // P3a: the death half of the overload above, for a kind whose announcement is its own
        // destroy CALL rather than the shared death notice (D-L). Hands the twin OUT rather
        // than destroying it in place, because the caller may still have to decide what
        // happens to the driver id it owns - Espryt pools it, deletes it, or parks it on the
        // deferred-release list when no context is current on this thread - and every one of
        // those outcomes has to be reached with the entry already retired, so a re-entrant
        // GetOrCreate from a twin destructor cannot resurrect it.
        //
        // The slot itself is NOT freed here: it belongs to the kind, and for a handle-keyed
        // kind the CLIENT frees it after the destroy call returns (SlotAllocator.h:60 - the
        // Gen bump rides the next handout, so a double free cannot skip a generation). An
        // entry whose Gen no longer matches is a twin of the slot's previous owner and is
        // left alone: the successor's own GetOrCreate resets it.
        BackendPtr ReleaseByHandle(MG_Pipe::MGPipeHandle handle) {
            if (MG_Pipe::MGPipeHandleIsNull(handle)) return BackendPtr{};
            Entry* const entry = EntryOrNull(handle.Slot);
            if (entry == nullptr || !entry->Live || entry->Gen != handle.Gen) return BackendPtr{};
            BackendPtr dead = std::move(entry->backend);
            entry->backend.reset();
            entry->Live = false;
            return dead;
        }

        BackendPtr* FindByHandle(MG_Pipe::MGPipeHandle handle) {
            if (MG_Pipe::MGPipeHandleIsNull(handle)) return nullptr;
            Entry* const entry = EntryOrNull(handle.Slot);
            if (entry == nullptr || !entry->Live || entry->Gen != handle.Gen) return nullptr;
            return &entry->backend;
        }

        // P5c (ct), CONTRACT-P5C.md §5.2: the handle-keyed half of the above, for a death that
        // arrived AS A WIRE RECORD (object_death) rather than as the shared notice. The
        // handle IS the resolution - it crossed in the record's payload - so the client's
        // allocator is never asked: under an active transport MGPipeSlots() is a client-only
        // surface (rule E, §3.1) and this function runs on the apply thread. Every holder
        // lets go exactly as the notice arm does, in the same successor-first order. What
        // does NOT happen here is the allocator Free: the slot's owner - the client -
        // returned it itself after the record went out (PipeFill.cpp's NotifyAndFree), and a
        // double Free would be refused by generation anyway. Idempotent against the kind's
        // own delete opcode, exactly as the notice arm is: a twin the delete already released
        // fails ReleaseTwinAt's generation check and the walk moves on.
        //
        // ONE SESSION'S HOLDERS, WHEN THE DEATH IS ONE SESSION'S. A handle is numbered by its own
        // client's allocator, so with several clients served by one process the same {slot, gen}
        // names a live, unrelated object in every other session: walking every holder released
        // THOSE twins too - a client deleting a texture (or exiting) deleted the compositor's
        // driver texture with the same handle, and its windows sampled freed memory. On a thread
        // that belongs to a session only that session's buckets answer; a sessionless thread
        // (the in-library backend, a unit fixture) keeps the process-wide walk.
        static Bool ReleaseTwinByHandle(MG_Pipe::MGPipeHandle handle) {
            if (MG_Pipe::MGPipeHandleIsNull(handle)) return false;
            TwinKey key;
            const Bool sessionScoped = CurrentTwinKey(&key) && key.SessionKey != 0;
            Bool released = false;
            for (BackendSlotTable* holder = s_firstHolder; holder != nullptr;) {
                BackendSlotTable* const next = holder->m_nextHolder;
                if (!sessionScoped || holder->m_ownerSession == key.SessionKey) {
                    released = holder->ReleaseTwinAt(handle) || released;
                }
                holder = next;
            }
            return released;
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

        // P5e (id), CONTRACT-P5E §4.1: fn(MGPipeHandle, const BackendPtr& twin) over every
        // live, twinned entry. Replaces the registry's begin()/end(), whose iterator exposed
        // the raw frontend address as the map key - the one place the backend read an identity
        // it must not have - and replaces P3a's own fn(StatePtr, BackendPtr), which handed a
        // frontend SharedPtr OUT OF SERVER MEMORY on every step.
        //
        // The handle is the entry's own {slot, gen}, band-aware: a composite ShaderCso's slot
        // is reported as kMGPipeShaderCsoCompositeSlotBase + its band index, i.e. the slot the
        // client minted, never the index into m_band.
        template <typename Fn>
        void ForEachLive(Fn&& fn) const {
            // Index loop and a COPIED twin, not a range-for over references: fn is arbitrary
            // backend code, and a nested GetOrCreate on this table would resize m_slots and
            // invalidate both the iterator and any reference into the vector that outlives the
            // call. The one caller today happens not to insert; that is not a property the
            // walk should depend on.
            for (SizeT index = 0; index < m_slots.size(); ++index) {
                const Entry& entry = m_slots[index];
                if (!entry.Live || !entry.backend) continue;
                const BackendPtr twin = entry.backend;
                fn(MG_Pipe::MGPipeHandle{static_cast<Uint32>(index), entry.Gen}, twin);
            }
            for (SizeT index = 0; index < m_band.size(); ++index) {
                const Entry& entry = m_band[index];
                if (!entry.Live || !entry.backend) continue;
                const BackendPtr twin = entry.backend;
                fn(MG_Pipe::MGPipeHandle{
                       static_cast<Uint32>(index) + MG_Pipe::kMGPipeShaderCsoCompositeSlotBase,
                       entry.Gen},
                   twin);
            }
        }

        // Empty this table, releasing every twin it holds. THE SPACES ARE MOVED OUT FIRST and
        // that is the whole of the care taken here: a twin's destructor is a driver call that
        // could re-enter GetOrCreate on this table (SlotTables.h's ReleaseTwinAt says why), and
        // destroying the entries in place would leave that re-entry resizing the vector the
        // destructors are being walked out of. Here the table is already empty when the entries
        // die, so a re-entrant insert grows a fresh vector.
        //
        // This is what `table = {}` used to mean, spelled so a caller cannot miss the ordering.
        void Reset() {
            Vector<Entry> slots = std::move(m_slots);
            Vector<Entry> band = std::move(m_band);
            BackendPtr nullTwin = std::move(m_nullTwin);
            slots.clear();
            band.clear();
            nullTwin.reset();
        }

        Uint32 LiveCount() const {
            Uint32 count = 0;
            for (const Entry& entry : m_slots) {
                if (entry.Live) ++count;
            }
            for (const Entry& entry : m_band) {
                if (entry.Live) ++count;
            }
            return count;
        }

        // The band's own live count, so a case can tell "the composite is twinned" from "the
        // ordinary table grew to reach it" - which is the whole assertion the band exists for
        // and is untestable from LiveCount alone.
        Uint32 CompositeLiveCount() const {
            Uint32 count = 0;
            for (const Entry& entry : m_band) {
                if (entry.Live) ++count;
            }
            return count;
        }

        // How many entries each space has ALLOCATED, live or not. A leak case asserts on these
        // rather than on LiveCount: growth is what the band prevents, and a dense table that
        // never shrinks is invisible to a liveness count.
        SizeT OrdinaryCapacityForTest() const { return m_slots.size(); }
        SizeT CompositeCapacityForTest() const { return m_band.size(); }

    private:
        // Drop the twin at `handle` if THIS table holds it. Frees nothing: the slot belongs to
        // the kind, not to the table, and the client returns it once, after the release.
        Bool ReleaseTwinAt(MG_Pipe::MGPipeHandle handle) {
            // The twin's destructor is a driver call and could, in principle, re-enter
            // GetOrCreate on this table and resize m_slots. So NOTHING that outlives the
            // destructor may be a reference into m_slots: the twin is moved out into a local,
            // the entry is finished with, and only then is the local released.
            BackendPtr dead;
            {
                Entry* const entry = EntryOrNull(handle.Slot);
                if (entry == nullptr || !entry->Live || entry->Gen != handle.Gen) return false;
                dead = std::move(entry->backend);
                entry->backend.reset();
                entry->Live = false;
            }
            dead.reset();
            return true;
        }

        // ---- P5e (id): the two slot spaces, resolved in ONE place ---------------------------
        //
        // Every reader and writer below goes through these three, so a caller that forgot the
        // band cannot exist - the shape MGPipeSlotAllocator::EntryOf already uses one level
        // out. kHasCompositeBand folds to `false` at compile time for the five non-ShaderCso
        // instantiations, so their band is dead code and an empty Vector.
        static constexpr Bool SlotIsBanded(Uint32 slot) {
            return kHasCompositeBand && MG_Pipe::MGPipeIsCompositeShaderSlot(slot);
        }
        static constexpr SizeT IndexOfSlot(Uint32 slot) {
            return SlotIsBanded(slot)
                       ? static_cast<SizeT>(slot - MG_Pipe::kMGPipeShaderCsoCompositeSlotBase)
                       : static_cast<SizeT>(slot);
        }

        // The entry a slot names, or null when its space has never grown that far. NEVER grows:
        // a lookup that resized would turn every miss into an allocation, which is the hazard
        // Find documents.
        Entry* EntryOrNull(Uint32 slot) {
            Vector<Entry>& table = SlotIsBanded(slot) ? m_band : m_slots;
            const SizeT index = IndexOfSlot(slot);
            if (index >= table.size()) return nullptr;
            return &table[index];
        }
        const Entry* EntryOrNull(Uint32 slot) const {
            return const_cast<BackendSlotTable*>(this)->EntryOrNull(slot);
        }

        // Grows the right space to hold `slot`. Every caller bounds `slot` first - against
        // kMaxHandleSlot - because this is the one place a client-supplied number decides an
        // allocation size. A composite slot grows m_band by (slot - base) + 1, so the ordinary
        // table never learns the band exists.
        Entry& EntryAt(Uint32 slot) {
            Vector<Entry>& table = SlotIsBanded(slot) ? m_band : m_slots;
            const SizeT index = IndexOfSlot(slot);
            if (index >= table.size()) table.resize(index + 1);
            return table[index];
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
        Uint64 m_ownerSession = 0;

        // Indexed by MGPipeHandle::Slot; [0] is the reserved slot and is never live.
        Vector<Entry> m_slots;
        // P5e (id): the ShaderCso COMPOSITE band, indexed by (slot -
        // kMGPipeShaderCsoCompositeSlotBase) and EMPTY for every other kind. See
        // kHasCompositeBand above for why it is a second vector and not more of the first.
        Vector<Entry> m_band;
        // Handed back by GetOrCreate for a null state object. Never live, never handed a handle.
        BackendPtr m_nullTwin;

    };

} // namespace MobileGL::MG_Backend::DirectGLES
