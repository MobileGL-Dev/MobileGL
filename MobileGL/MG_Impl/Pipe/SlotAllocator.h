// MobileGL - MobileGL/MG_Impl/Pipe/SlotAllocator.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#include <MG_Pipe/MGPipeHandles.h>

// The client's slot allocator: it mints every MGPipeHandle, so no create_* call needs a round
// trip (ARCHITECTURE.md §2.1). A free list plus a high-water mark per kind keeps slots dense,
// so the server's object table is an array. Unrelated to MG_State's IndexGenerator, whose LIFO
// GL-name reuse is the ABA {slot, gen} exists to prevent. Gen increments only when a slot is
// reused, never on a respecify; it is separate from the server's MGGen epoch, which no MGPipe
// call may require the client to know (§2.2). The lifetimeId -> slot map keeps GL names out of
// every key: the frontend key is the lifetime id, the backend key is the handle.
namespace MobileGL::MG_Pipe {

    class MGPipeSlotAllocator {
    public:
        static constexpr SizeT kKindCount = static_cast<SizeT>(MGPipeKind::KindCount);

        // A fresh {slot, gen} of this kind, from the free list if one is waiting and from the
        // high-water mark otherwise. Never returns slot 0 (reserved: null, and the default
        // framebuffer for kind Framebuffer), and never returns a ShaderCso slot inside the
        // composite band, which the program-pipeline resolver mints out of separately.
        MGPipeHandle Allocate(MGPipeKind kind);
        // Allocate and remember `lifetimeId` as this handle's frontend identity.
        MGPipeHandle AllocateFor(MGPipeKind kind, Uint64 lifetimeId);

        // The only way into the ShaderCso composite band, which Allocate() refuses: a program
        // pipeline's flattened composite is minted client-side and looks like an ordinary
        // program to the server, so it gets its own entry point rather than a flag on the
        // handle. Otherwise it behaves like AllocateFor (free list first, Gen moves only on
        // reuse, the lifetimeId map is written). Returns kMGPipeNullHandle, after a named
        // assert, when the band is full. Freed through Free(ShaderCso, handle): of its two
        // release paths (the pipeline cache's LRU and ~ProgramObject), the second finds the
        // slot no longer live at that generation and is a no-op.
        MGPipeHandle AllocateComposite(Uint64 lifetimeId);
        // The handle a lifetime id was allocated for, or kMGPipeNullHandle. A recycled heap
        // address does NOT reproduce a mapping: MG_State hands out a fresh lifetime id per
        // object, so the map key is unique for the life of the process.
        MGPipeHandle FindByLifetimeId(MGPipeKind kind, Uint64 lifetimeId) const;
        // FindByLifetimeId, then AllocateFor when it misses. The ordinary client path.
        MGPipeHandle Acquire(MGPipeKind kind, Uint64 lifetimeId);
        // P15: the same two answers, first trying `hint` - the handle this object was last given,
        // kept on the frontend object itself (MGPipePackHandleHint). The hint is believed only when
        // the slot it names is live, at that generation, and owned by this very lifetime id, so a
        // reset, a free or a recycle can only make it miss; the hint is refreshed on every miss.
        MGPipeHandle FindByLifetimeIdHinted(MGPipeKind kind, Uint64 lifetimeId, Uint64& hint) const;
        MGPipeHandle AcquireHinted(MGPipeKind kind, Uint64 lifetimeId, Uint64& hint);

        // Returns the slot to the free list. The Gen bump happens on the NEXT handout of that
        // slot, not here, so a handle that is freed twice cannot skip a generation and the
        // "gen moves only on reuse" contract holds for an object that is never reused.
        void Free(MGPipeKind kind, MGPipeHandle handle);

        Bool IsLive(MGPipeKind kind, MGPipeHandle handle) const;
        // 0 for a slot that was never handed out; the generation of the LAST handout
        // otherwise, live or not.
        Uint32 GenOfSlot(MGPipeKind kind, Uint32 slot) const;
        Uint64 LifetimeIdOfSlot(MGPipeKind kind, Uint32 slot) const;
        // One past the highest ordinary slot ever handed out of this kind. For ShaderCso the
        // composite band is a separate dense space, answered by CompositeHighWater(): folded
        // in, this number would sit at ~983k after the first composite, and a leak test
        // asserting it stays put over churn could no longer fail for ordinary slots. Nor is it
        // a table size for ShaderCso: a consumer indexing by slot tests
        // MGPipeIsCompositeShaderSlot(slot) first and keeps the band in a table of its own.
        Uint32 HighWater(MGPipeKind kind) const;
        // One past the highest COMPOSITE slot ever handed out, i.e.
        // kMGPipeShaderCsoCompositeSlotBase + (band slots ever handed out), and exactly the
        // base when none ever was. Kind ShaderCso is the only kind with a band, so it is
        // implied - as it is for AllocateComposite. A LEAKED COMPOSITE MOVES THIS and moves
        // nothing else, which is what the composite's own leak case asserts on.
        Uint32 CompositeHighWater() const;
        // Live slots of this kind, ORDINARY AND COMPOSITE TOGETHER for ShaderCso: a live
        // composite is a live ShaderCso, the applier's two record tables are one object class,
        // and a caller asking "how many shader CSOs does this client hold" wants both. The
        // band's own count is CompositeLiveCount(); the ordinary space's is the difference.
        Uint32 LiveCount(MGPipeKind kind) const;
        Uint32 CompositeLiveCount() const;
        // Slots waiting on a free list. Also BOTH SPACES for ShaderCso, for LiveCount's
        // reason and with the same caveat: a caller that needs to know WHICH space a slot went
        // back to reads CompositeFreeCount() and subtracts.
        Uint32 FreeCount(MGPipeKind kind) const;
        Uint32 CompositeFreeCount() const;

        // Context teardown / server reset / a unit test's fixture.
        void Reset();
        // Moves at every Reset(): a memo of handles minted by this allocator is good only while it
        // holds (a handle lives as long as its object, and only a reset forgets that).
        Uint64 ResetEpoch() const { return m_resetEpoch; }

    private:
        struct SlotState {
            Uint32 Gen = 0;
            Bool Live = false;
            Bool EverHandedOut = false;
            Uint64 LifetimeId = 0;
        };

        struct KindState {
            // Indexed by slot; [0] is the reserved slot and is never live.
            Vector<SlotState> Slots;
            Vector<Uint32> FreeList;
            // P4a: the ShaderCso COMPOSITE band, indexed by (slot - the band's base) and
            // EMPTY for every other kind. A SECOND VECTOR RATHER THAN MORE OF THE FIRST, and
            // it is not a micro-optimization: the band starts at 983040, so minting one
            // composite into the slot-indexed vector above would allocate ~983k SlotStates -
            // ~23 MB - for a single program pipeline, and a consumer that sized a table off
            // HighWater would pay the same shape again with a far bigger record. Both spaces
            // stay dense against their own high-water mark, which is the property this
            // allocator exists to give the server.
            Vector<SlotState> BandSlots;
            Vector<Uint32> BandFreeList;
            UnorderedMap<Uint64, Uint32> ByLifetimeId;
            // P14: a direct-mapped front for ByLifetimeId. Every draw resolves its VAO, its
            // vertex buffers and its textures by lifetime id, and the hash map was ~2-3% of a
            // Minecraft frame. Keyed on the lifetime id (monotonic, never reused) and confirmed
            // against the slot's own LifetimeId and Live bit on every hit, so a stale entry can
            // only miss. Mutable: FindByLifetimeId is const and fills it.
            struct RecentEntry {
                Uint64 LifetimeId = 0;
                Uint32 Slot = 0;
            };
            static constexpr SizeT kRecentEntries = 1024;
            mutable Array<RecentEntry, kRecentEntries> Recent{};
            Uint32 LiveCount = 0;
            // The band's share of LiveCount above, so the two spaces can be reported apart
            // without walking either table. Always 0 for every kind but ShaderCso.
            Uint32 BandLiveCount = 0;
        };

        Uint64 m_resetEpoch = 0;
        KindState& StateOf(MGPipeKind kind);
        const KindState& StateOf(MGPipeKind kind) const;
        // The SlotState a (kind, slot) names, in whichever of the two vectors holds it, or
        // null when the slot has never been handed out. One resolver, so a caller that forgets
        // the band cannot exist.
        static SlotState* EntryOf(KindState& state, MGPipeKind kind, Uint32 slot);
        static const SlotState* EntryOf(const KindState& state, MGPipeKind kind, Uint32 slot);

        Array<KindState, kKindCount> m_kinds{};
    };

    // The monolith's one client allocator. Under split there is one per client context.
    MGPipeSlotAllocator& MGPipeSlots();

    // A handle packed into the Uint64 a frontend object keeps as its hint (0 = none; slot 0 is
    // never handed out).
    inline Uint64 MGPipePackHandleHint(MGPipeHandle handle) {
        return static_cast<Uint64>(handle.Slot) | (static_cast<Uint64>(handle.Gen) << 32);
    }

#if MOBILEGL_BUILD_DISAGGREGATED
    // P5c (hd, CONTRACT-P5C §3.1 / §6 layer 1): with an active transport this allocator is a
    // CLIENT-only surface. Its singleton accessor and original three method guards reject apply -
    // i.e. a server that resolves or mints handles off a frontend object's lifetime id (T2),
    // which is memory that will not exist on its side of a real split - are
    // Fatal{RoleViolation, "MGPipeSlots"}. Compiled out entirely outside split builds, so the
    // pull build's bytes do not move (G1).
    void MGPipeRefuseAllocatorFromApplyThread(const char* entry);

#else
    // P13 W5: no transport, no apply thread - the allocator's role guard has nothing to refuse.
    inline void MGPipeRefuseAllocatorFromApplyThread(const char*) {}
#endif
} // namespace MobileGL::MG_Pipe
