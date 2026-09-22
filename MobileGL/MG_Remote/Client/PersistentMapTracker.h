// MobileGL - MobileGL/MG_Remote/Client/PersistentMapTracker.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// THE BLOCK-GRANULARITY PERSISTENT-MAP PUSH (P5 R-6, CONTRACT-P5.md section 3, table 2's
// second set). Owner: package b1.
//
// WHY THIS EXISTS AT ALL. A coherent persistent map is the one buffer shape with no per-write
// API call: the application memcpys through the pointer and neither a serial, an epoch nor a
// record moves. In monolith that is free, because the pointer IS the backend's GPU storage -
// MEASUREMENTS.md:87 prices the adoption at p99 163 -> 21 ms and ~400 MB saved, and
// ARCHITECTURE.md:481 requires it to hold unmoved for the whole monolith track. Across a
// process boundary the adopted address is meaningless, so P5 runs at tier T2 (emulate): the
// client keeps the shadow, MGPipeApplyMapPersistent declines, and the bytes the application
// wrote with no call have to be SHIPPED. `persistent-map-push` (PipeStats `pmap`) is exactly
// those bytes, and it is structurally zero while adoption survives - which is why forcing T2
// and wiring the counter are one deliverable and not two.
//
// THE SET. `m_livePersistentMaps` is SyncPersistentMappedRange's own early-out chain
// (BufferObject.cpp:341-353) read as a membership test - mapped, NOT GPU-resident, Persistent,
// Write, NOT FlushExplicit, non-empty mapped range - and nothing else. Reading it as a
// predicate rather than re-deriving one is deliberate: the day that chain grows a sixth
// early-out, a re-derived predicate silently keeps pushing a buffer the monolith path stopped
// pushing, and the two arms diverge with no test able to say so. IsLivePersistentMap() is the
// single spelling; BufferObject::SyncPersistentMappedRange is held to it by a unit case.
//
// THE GRANULARITY. MOBILEGL_IPC_PERSISTENT_BLOCK_KB (default 64) blocks, keyed
// {MGPipeHandle, blockIndex} - the key is implicit, because a block ships as an ORDINARY
// resource_subdata record whose destination range is [blockIndex * blockBytes, + blockBytes).
// NO NEW RECORD KIND: the existing record is already chunked by
// MGPipeForEachSubDataRecordRange and already acceptance-gated, and a second way to say
// "these bytes go there" is a second way to get it wrong. A block size of 0 is E3(a)'s
// NEGATIVE CONTROL and means "push nothing" - not "one unlimited block" - so
// PersistentCoherentMapScenario must go red under it.
//
// PHASE 1 IS CONSERVATIVE, AND SAYS SO. The whole mapped span is pushed, by block, at every
// validate point; no dirty bits, no memcmp. Phase 2 (MOBILEGL_IPC_SHADOW_SHM, P6+) makes it
// precise, and ARCHITECTURE.md:499 grants it the right to be pulled forward if Phase 1 is
// unacceptable on the Create/Flywheel fixtures - the one place in the plan where a
// measurement may reorder phases.
//
// ORDERING IS THE CORRECTNESS PROPERTY, NOT THE GRANULARITY. In monolith the push is a memcpy
// on the same thread as the draw that follows it, so the bytes the application wrote before
// the draw are the bytes the draw sees. Under split both travel SEG_CMD in order, so the ring
// preserves it - PROVIDED the push is emitted AT the validate point and not lazily. A push
// deferred past its own draw record is the C-1 regression re-committed at the transport layer.

#pragma once
#include <Includes.h>

#include <Config.h>

#include <atomic>
#include <cstdint>

namespace MobileGL::MG_State::GLState {
    class BufferObject;
}

namespace MobileGL::MG_Remote::Client {

    // The mprotect tracker's per-map slot, declared at namespace scope rather than in the
    // .cpp's anonymous namespace so PersistentMapTracker's membership map can CACHE a
    // pointer to a member's slot (the 64-slot rescan per buffer per draw was measured on
    // device). Every field's reader/writer discipline is unchanged and lives in the .cpp;
    // only the type's visibility moved.
    struct TrackedWriteMap {
        std::atomic<uintptr_t> base{0};
        std::atomic<uintptr_t> end{0};
        std::atomic<Uint64>* pageBits = nullptr; // allocated once, never freed or moved
        SizeT pageCount = 0;                     // GL thread only
        Uint64 lifetimeId = 0;                   // GL thread only
        // GL thread only, and only on the INWARD fallback (TrackWriteMap: a shadow that
        // failed the page-granular test). There the two partial-page edge spans are
        // never protected, so nothing faults for them; pushing their blocks
        // unconditionally measured ~2 x 64 KB x every draw for a hot unaligned buffer
        // (112 MB/frame). They are hashed per push instead - 8 KB of XXH3 at worst - and
        // their containing block ships only on a change. A page-granular shadow (every
        // shadow the allocator hands out in a split build) is aligned OUTWARD and has no
        // edges, so these are never read for it - that per-draw XXH3 was 8.5% of the
        // client thread at VD12 (P5d round 3), on nearly every live map.
        Uint64 edgeHashHead = 0;
        Uint64 edgeHashTail = 0;
        // GL thread only: whether the mapped range has UNPROTECTED partial-page edge spans
        // at all - true only on the inward fallback. Feeds the epoch skip's per-member edge
        // service (PushDrawConsumers, through m_edgedMembers) - edge bytes are never
        // protected, so their dirtiness is invisible to the fault epoch. The push itself
        // does not read it: its two edge hashes are gated by the span geometry, which
        // says the same thing.
        Bool hasEdges = false;
    };

    class PersistentMapTracker {
    public:
        // Client-role singleton (table 3: the MG_Impl/MG_Remote/Client singletons are
        // client-exclusive). Leaks at exit for ID-8's reason, once per role-local singleton.
        static PersistentMapTracker& Instance();

        // MOBILEGL_IPC_PERSISTENT_BLOCK_KB * 1024. Zero means the push is OFF (E3(a)).
        static Uint64 BlockBytes();
        // Transport != Monolith. The whole module is inert on the monolith path: an extra
        // resource_subdata record there would be new behaviour, which D-J forbids.
        static Bool PushIsArmed();

        // r1 (P5-close codex finding 1, ID-52): true on the SERVER role's thread - the apply thread
        // of a running ServerLoop, which under inproc lives in this very process. This module is
        // the CLIENT's producer: it reads the frontend object's MappedData() and pushes it as
        // resource_subdata. On the apply thread that record is routed through the monolith adapter
        // straight into the server's shadow (Ops_H_SubData), which REPLACES the transported bytes
        // with client memory - and under an active transport the staged copy is the draw's ONLY
        // base (ID-52 item 3). So the producer asks this before it runs: on the server role the
        // answer is "there is nothing to sync" (SyncPersistentMappedRange) or a refusal by name
        // (PushMappedSpanBlock). False in every monolith process, where no ServerLoop runs.
        static Bool OnServerRole();

        // SyncPersistentMappedRange's early-out chain as a predicate. THE only spelling.
        static Bool IsLivePersistentMap(const MG_State::GLState::BufferObject& buffer);

        // Membership maintenance, both idempotent and both safe to call on a buffer that is
        // not a member. Called from BufferObject on every event that can move the predicate:
        // map, unmap, respecify, adoption, destruction.
        void NoteMapStateChanged(MG_State::GLState::BufferObject& buffer);
        void Forget(const MG_State::GLState::BufferObject& buffer);

        // One member's whole mapped span, by block. Re-checks the predicate first, so a
        // member that stopped being one (an adoption, an unmap that did not route through
        // NoteMapStateChanged) is dropped rather than pushed.
        void PushBlocksFor(MG_State::GLState::BufferObject& buffer);

        // THE VALIDATE-POINT HOOK. Every member, before the verb record is emitted.
        void PushAllMembers();

        // THE DRAW-VERB HOOK, FILTERED. A draw or dispatch can only read the buffers its
        // own bindings name (the VAO's attribute and element buffers, the indexed
        // uniform/storage/atomic/XFB binding points, the indirect and parameter slots), so
        // pushing every live persistent map at every draw is a whole-range rescan of
        // buffers the verb can never touch - the dominant cost on a real workload (the
        // whole-range scan ran ~135x per frame over every arena in the process). Pushing
        // only the consumers keeps the guarantee ("a changed byte reaches the server before
        // the verb that reads it") and drops the rest. Read-only verbs (clear / blit /
        // readback / present) keep PushAllMembers: they read no binding points and their
        // rate is a few per frame, so the conservative whole push is cheap there.
        void PushDrawConsumers();

        SizeT MemberCount() const { return m_livePersistentMaps.size(); }
        Uint64 BlocksPushed() const { return m_blocksPushed; }
        Uint64 BytesPushed() const { return m_bytesPushed; }
        // Unit tests only: the counters are diagnostics, the set is not reset by it.
        void ResetCountersForTest() {
            m_blocksPushed = 0;
            m_bytesPushed = 0;
            m_blockZeroAnnounced = false;
        }
        // Unit tests only: clears the membership, the hash cache and the mprotect tracked
        // slots (restoring every tracked page writable first), and resets the counters.
        void ClearForTest();
        // Unit tests only: the mprotect arm's white-box surface, for the alignment cases
        // (SplitBufferTest) that the public GL surface cannot see - which pages a map
        // protected is not observable through any push count. MprotectArmAvailableForTest
        // is the arm's precondition (handler installable, kernel page == tracker page);
        // a case that needs the arm skips on it rather than going red on a host that has
        // no arm. TrackedSlotForTest reads a member's live slot (nullptr on the hash arm).
        // TrackForTest / UntrackForTest register an arbitrary range with an arbitrary
        // declared extent, which is the only way to reach the inward fallback on a build
        // whose allocator never hands out an unaligned shadow. FaultEpochForTest is the
        // handler's answered-fault counter, the one observable a protected page has.
        static Bool MprotectArmAvailableForTest();
        static const TrackedWriteMap* TrackedSlotForTest(const MG_State::GLState::BufferObject& buffer);
        static const TrackedWriteMap* TrackForTest(Uint64 lifetimeId, const Uint8* shadow, SizeT rangeBegin,
                                                   SizeT rangeEnd, SizeT shadowExtent);
        static void UntrackForTest(Uint64 lifetimeId);
        static Uint64 FaultEpochForTest();
        // Unit tests only, and the ONE row that carries the arm64 tagged-pointer defect: the
        // kernel reports si_addr untagged while every base this table publishes comes from a
        // bionic-tagged heap pointer, so the ownership test has to normalise both sides.
        // UntagAddressForTest is that normalisation and OwnershipProbeForTest is the row
        // itself, run against a slot the caller composes rather than a live one. Both are
        // architecture-independent on purpose - the mask is the identity for every valid
        // userspace address off arm64 - so a host CI lane can drive them with the device's
        // own two addresses instead of the defect being reachable only on the phone.
        static uintptr_t UntagAddressForTest(uintptr_t address);
        static Bool OwnershipProbeForTest(uintptr_t slotBase, uintptr_t slotEnd, uintptr_t faultAddress,
                                          SizeT* pageIndexOut);
        // Unit tests and device triage: how many SEGV_ACCERRs the handler chained away as
        // foreign, and the last one's si_addr beside the first live tracked span. Zero on a
        // healthy client - the JVM's implicit null checks do land here, so a NON-zero count
        // with a nearest-tracked span whose low 56 bits contain si_addr is the signature of
        // the tagged/untagged mismatch and nothing else.
        struct DeclinedFaultReport {
            Uint64 count = 0;
            uintptr_t address = 0;
            uintptr_t nearestBase = 0;
            uintptr_t nearestEnd = 0;
        };
        static DeclinedFaultReport DeclinedFaults();

    private:
        // One live member. tracked is the member's tracker slot when it is on the
        // mprotect arm, nullptr on the hash arm; it is REFRESHED at every
        // NoteMapStateChanged (a resize can re-claim a different slot) and validated
        // against the slot's lifetimeId before every use, which is what makes a slot
        // retired and re-claimed by another buffer safe to have pointed at.
        struct MemberEntry {
            MG_State::GLState::BufferObject* buffer = nullptr;
            TrackedWriteMap* tracked = nullptr;
        };
        // Keyed on BufferObject::GetLifetimeId(), which is globally unique and never reused -
        // never the GL name (LIFO-recycled by glGenBuffers) and never the heap address
        // (recycled by the allocator). The raw pointer is safe because every removal path is
        // explicit: ~BufferObject and ReleaseMemory both call Forget/NoteMapStateChanged, and
        // PushBlocksFor re-checks the predicate before it dereferences anything it kept.
        UnorderedMap<Uint64, MemberEntry> m_livePersistentMaps;
        // Per-buffer per-block xxHash64 of the client shadow, for the dirty-block push
        // (MOBILEGL_IPC_PERSISTENT_HASH_SUPPRESS): a block is pushed only when its content
        // changed since the last push, instead of the whole mapped range every verb. Keyed
        // the same way as m_livePersistentMaps and retired beside it in Forget(). Zero
        // hashes mean "unknown", never "a block of zeros" (XXH64 of zero bytes is not zero).
        struct BlockHashState {
            Uint64 begin = 0;
            Uint64 end = 0;
            Uint64 blockBytes = 0;
            Vector<Uint64> hashes;
        };
        UnorderedMap<Uint64, BlockHashState> m_blockHashes;
        Uint64 m_blocksPushed = 0;
        Uint64 m_bytesPushed = 0;
        // THE EPOCH SKIP's state (see PushDrawConsumers). m_lastFaultEpoch is the fault
        // counter's value at the last walk - and the walk drains every tracked member
        // with a marked page, consumer or not, which is the only reason an unmoved
        // counter can stand for "nothing to push". m_untrackedMembers holds EVERY
        // live member that fell back to the hash arm, and any one of them vetoes the
        // skip: the walk is what tells an untracked member's consumption, and scanning
        // every untracked member unconditionally (the alternative, measured on device
        // at VD12) costs more than the walk it replaced.
        Uint64 m_lastFaultEpoch = 0;
        UnorderedMap<Uint64, Uint8> m_untrackedMembers;
        // The tracked members whose slot has UNPROTECTED edge spans (the inward fallback,
        // TrackedWriteMap::hasEdges): the epoch skip serves exactly these per draw and
        // nobody else, so it walks this set and not the membership. Empty in steady state
        // on a split build - every shadow is page-granular and aligns outward - which is
        // what makes the skip's common path a handful of loads with no per-member work.
        // Maintained beside m_untrackedMembers at the same three events.
        UnorderedMap<Uint64, Uint8> m_edgedMembers;
        // Everything of PushBlocksFor after the two role guards, for callers that already
        // ran them once (the two Members hooks) - the guard's thread-local read was a
        // measurable per-buffer-per-draw cost on the emutls path.
        void PushBlocksForChecked(MG_State::GLState::BufferObject& buffer);
        // E3(a)'s diagnostic is emitted ONCE per process. PushBlocksFor runs at every validate
        // point of every member, so an unlatched MGLOG_W would be one line per draw per mapping
        // - and a control that has to grep a log cannot tell a message that fired from a
        // message that flooded.
        Bool m_blockZeroAnnounced = false;
    };

    // What the client's emit table calls immediately BEFORE emitting any verb that can read a
    // buffer (draw, dispatch, readback, blit, present). It is a free function rather than a
    // method so the emit table does not have to name the singleton, and so the one-line call
    // reads as what it is: "publish everything the application wrote with no call".
    //
    // In P5 the 21 SyncPersistentMappedRange sites (CONTRACT-P5.md section 3: 9 Espryt + 12
    // Magma, MEASUREMENTS.md:111's 20 being one low) still stand where they are and route
    // into PushBlocksFor through BufferObject::SyncPersistentMappedRange, so the push already
    // happens at every point monolith pushes at. They retire into THIS call at P8, when the
    // draw-path binding walks move to the client.
    void PushPersistentMapsBeforeVerb();

    // R-6's tier gate, and the ONE spelling of it. True for MOBILEGL_IPC_ADOPT_TIER=2, the
    // only tier P5 implements; 0 (a real cross-process shared mapping) and 1 (a server-side
    // staging map) parse - so the negative control has a name before the thing it controls
    // exists - and are a NAMED refusal here rather than a silent fall back to T2. It is asked
    // by MGPipeApplyMapPersistent, which is where the decline is decided, so the client's
    // three adoption call sites keep their existing "null means declined" branch and the
    // map-persistent-roundtrips counter keeps counting ATTEMPTS in both arms (E3(c) asserts
    // mpr is equal between the monolith and the split arm, which is only true if the decline
    // happens after the count, on the applier's side of the emission).
    Bool AdoptTierIsEmulate();

} // namespace MobileGL::MG_Remote::Client
