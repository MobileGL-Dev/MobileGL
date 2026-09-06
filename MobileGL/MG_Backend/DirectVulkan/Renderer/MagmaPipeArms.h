// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#include <Config.h>
#if MOBILEGL_PIPE_PUSH
// kMGPipeSubsystem* - the runtime bitmask's named bits - and MGPipeHandle itself. Both are
// header-only constant/POD declarations, and both are push-only, so the pull build's include
// graph is unchanged (G1).
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/MGPipeHandles.h>
#endif

#include <cstdlib>

// Magma's arm selector for the P2 Track H / render-state re-keys (P2 brief D14), and the
// {slot, gen} mint the re-keyed sites are written against.
//
// Two switches decide which arm a re-keyed site runs, and they are NOT the same switch:
//
//   MOBILEGL_PIPE_PUSH (compile)          - is the pushed state there to be keyed on at all
//   Features.PipePush  (runtime bitmask)  - is THIS subsystem migrated in THIS run
//   MOBILEGL_PIPE_LEGACY_MEMOS (compile)  - is the pre-handle arm compiled beside it
//   Features.PipeLegacyMemos (runtime)    - may the pre-handle arm be ENTERED in this run
//
// ARCHITECTURE.md 9.6's point: once a handle wave lands, a clear MOBILEGL_PIPE_PUSH bit is
// only a valid A/B while the legacy arm is still compiled, because with the bit clear the
// backend would otherwise still run the re-keyed code. So a clear bit selects the legacy
// arm, and a run that has explicitly disabled the legacy arm may not fall into it.
//
// D14 spends that last sentence at STARTUP, not per draw: "a Track-H subsystem whose bit is
// clear is a startup Fatal{PipeLegacyMemosDisabled}". Nothing in the draw path aborts, and
// nothing outside Track H consults the legacy-memo lever at all - see
// MagmaPipeValidateSubsystemConfiguration below for both halves of that rule.
//
// The whole header is inert in a pull build: MOBILEGL_PIPE_PUSH is 0 there, every helper
// below is behind it, and the pull build's translation units are byte-identical (G1).
namespace MobileGL::MG_Backend::DirectVulkan {

#if MOBILEGL_PIPE_PUSH
    // Is `subsystemBit` (MG_Pipe/MGPipe.h's kMGPipeSubsystem*) migrated in this run?
    inline Bool MagmaPipeSubsystemOn(Uint64 subsystemBit) {
        return (MG_Config::Features.PipePush & subsystemBit) != 0;
    }

    // ---------------------------------------------------------------------------------
    // D14's startup gate
    // ---------------------------------------------------------------------------------
    //
    // Called once from VulkanRenderer::Initialize(), i.e. only when Magma is the backend
    // that is actually running. It answers exactly one question and it answers it before the
    // first draw: is there an arm for Magma's Track-H subsystem in this configuration?
    //
    // Three deliberate boundaries, each of which the per-draw shape this replaces got wrong:
    //
    //  * ONLY Magma's own Track-H bit is checked. Espryt's bit 5 is Espryt's business (a
    //    DirectVulkan run does not execute one line of DirectGLES' re-key), so
    //    MOBILEGL_PIPE_PUSH=0x20 must not kill a Magma run, and MOBILEGL_PIPE_PUSH=0x40 must
    //    not kill an Espryt one.
    //  * bit 0 (kMGPipeSubsystemRenderState) is NOT Track H and is NOT fatal. It is not a
    //    memo re-key at all: it decides where the pipeline memo's STATE KEY comes from, and
    //    a clear bit there simply means the client is not pushing render-state CSOs in this
    //    run, which GetOrCreatePipeline answers with its own state hash. D14 labels bits 5
    //    and 6 "Track H" and labels bit 0 nothing of the sort.
    //  * it is Fatal at STARTUP, once, not on a draw. A per-draw abort inside
    //    GetOrCreatePipeline turns a configuration mistake into a mid-frame crash and puts a
    //    branch nobody needs on the hottest path in the backend.
    //
    // [declared deviation from D14, review v2 minor 2] D14's runtime row reads "false: the
    // legacy arm is never entered", and D14's compile-switch row names ComputePipelineStateHash
    // as part of the pre-handle arm. Those two together would make MOBILEGL_PIPE_LEGACY_MEMOS=0
    // with bit 0 CLEAR a contradiction: the pipeline memo has no CSO handle to key on, so it
    // keys on a state hash, and in a build that compiles the pre-handle arm that hash IS
    // ComputePipelineStateHash. Magma does not make that fatal - bit 0 is not Track H, and
    // there is a correct answer (the state hash) where for bits 5/6 there is none - but it no
    // longer does it SILENTLY: the combination is named once, at startup, right here.
    inline void MagmaPipeValidateSubsystemConfiguration() {
        if (!MG_Config::Features.PipeLegacyMemos &&
            !MagmaPipeSubsystemOn(MG_Pipe::kMGPipeSubsystemRenderState)) {
            MGLOG_W("MGPipe: MOBILEGL_PIPE_LEGACY_MEMOS=0 with kMGPipeSubsystemRenderState (bit 0 "
                    "of MOBILEGL_PIPE_PUSH) clear - Magma's pipeline memo has no CSO handle to key "
                    "on, so every draw whose pipeline-state version moved runs the pre-handle STATE "
                    "HASH instead. That is not a Track-H subsystem and not fatal, but it is not the "
                    "handle arm either: set bit 0 (MOBILEGL_PIPE_PUSH=0x%llx) if this run was meant "
                    "to measure it.",
                    static_cast<unsigned long long>(MG_Config::Features.PipePush |
                                                    MG_Pipe::kMGPipeSubsystemRenderState));
        }
#if MOBILEGL_PIPE_LEGACY_MEMOS
        // The pre-handle arm is compiled AND the operator has not forbidden entering it, so a
        // clear bit is an ordinary, valid A/B: the site takes the legacy arm.
        if (MG_Config::Features.PipeLegacyMemos) return;
#endif
        if (MagmaPipeSubsystemOn(MG_Pipe::kMGPipeSubsystemMagmaVertexInput)) return;
#if MOBILEGL_PIPE_LEGACY_MEMOS
        const char* const why = "this run has MOBILEGL_PIPE_LEGACY_MEMOS=0";
#else
        const char* const why =
            "this build has cmake -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF, which compiles no such arm";
#endif
        MGLOG_F("MGPipe: Fatal{PipeLegacyMemosDisabled} Magma's Track-H subsystem "
                "(kMGPipeSubsystemMagmaVertexInput, bit 6 of MOBILEGL_PIPE_PUSH) is clear, so the "
                "vertex-input cache and the VAO draw memo want the pre-handle arm - but %s. Set "
                "bit 6 (MOBILEGL_PIPE_PUSH=0x%llx, or the default 0x%llx), or allow the legacy arm.",
                why,
                static_cast<unsigned long long>(MG_Config::Features.PipePush |
                                                MG_Pipe::kMGPipeSubsystemMagmaVertexInput),
                static_cast<unsigned long long>(MG_Pipe::kMGPipeSubsystemsMigratedAtP2));
        std::abort();
    }

    // "Does this Track-H site run the handle arm?" - the ONE question every re-keyed Track-H
    // site asks, so that they cannot disagree with each other or with the startup gate.
    inline Bool MagmaPipeTrackHArmIsHandles(Uint64 trackHBit) {
#if MOBILEGL_PIPE_LEGACY_MEMOS
        return MagmaPipeSubsystemOn(trackHBit);
#else
        // No pre-handle arm exists in this build, and MagmaPipeValidateSubsystemConfiguration
        // has already made a clear bit a startup Fatal, so the handle arm is the only arm a
        // running process can be on.
        (void)trackHBit;
        return true;
#endif
    }

    // ---------------------------------------------------------------------------------
    // The {slot, gen} mint
    // ---------------------------------------------------------------------------------
    //
    // Maps a frontend object's never-reused lifetime id to a dense {slot, gen}. Three
    // properties, and the third is the one review v2 got wrong:
    //
    //   1. exact identity - Gen moves whenever a slot changes owner, so a stale handle can
    //      never match a live object even if the allocator hands back the same heap address
    //      (the ABA HandleRecycleScenario reproduces);
    //   2. dense slots - the slot IS an index, so a consumer's per-slot table needs no hash,
    //      no probe and no mix;
    //   3. NO CAPACITY CLIFF. A live object's handle never changes while the object is being
    //      drawn, whatever the working set size.
    //
    // Property 3 is why this is not the fixed 2-way set-associative LRU the previous round
    // shipped. That structure evicted a LIVE object once the working set passed its capacity,
    // and every consumer memo keyed on the handle died with it: measured on a verbatim
    // transcription, 54% of uses lost their handle at 2500 live VAOs against 2048 entries, and
    // 20% at 1024 live VAOs once the lifetime ids are sparse (an app that creates and destroys
    // VAOs, which is the Minecraft chunk shape this exists for). Two of the three memos it
    // fed - the content-hash memo and the resolved-state memo - had NO capacity before this
    // package: they were unbounded mutable fields on VertexArrayObject. Introducing eviction
    // there turns one ComputeHash per VAO reconfiguration into one per DRAW, and, once the
    // buffer table thrashes too, makes the vertex-input content hash a per-draw value that
    // inserts a fresh heap-allocated BackendVertexInputState into an unbounded map on every
    // draw. That is a worse leak than the one it was introduced to avoid.
    //
    // So: grow on demand, and reclaim by AGE instead of by capacity.
    //
    //   * Acquire hits an UnorderedMap<lifetimeId, slotIndex>, in front of which sits a
    //     one-entry memo. Every re-keyed site in a draw asks about the SAME VAO, so the memo
    //     turns the five-or-six acquisitions a draw makes into one map probe plus five Uint64
    //     compares - less than the address multiply plus two-way probe the pre-handle arm ran.
    //   * OnFrameBoundary retires slots whose object has not been drawn for
    //     kRetireAgeBoundaries boundaries and returns them to a free list, so the table's
    //     footprint tracks the LIVE DRAWN working set, not objects ever created. That is the
    //     property MG_Impl/Pipe/SlotAllocator cannot have here: nothing in P2 can call its
    //     Free (the tracker emits no object-class state, BufferBackendOps::OnDestroy is handed
    //     a BackendBufferResource rather than the BufferObject, and VertexArrayObject has no
    //     death hook at all - adding one is D13's explicit-destroy work, which covers Espryt's
    //     six kinds, not VertexElementsCso), so an allocator here would grow by one SlotState
    //     plus one map node per object EVER created, for the life of the process, on a
    //     platform with an LMK. Age-based reclamation is the stand-in for the death
    //     notification, and it is exactly as ABA-proof, because reuse bumps Gen.
    //   * A retire costs at most one memo recompute if the object is drawn again - the same
    //     price a cache miss costs - and it is charged only to objects that went idle for
    //     ~1024 frames, never to a hot one.
    //
    // Memory: one map node plus one 24-byte Entry per live object, i.e. tens of bytes against
    // the kilobyte a VertexArrayObject or a BufferObject already costs the frontend. There is
    // no capacity to size off a device measurement because there is no capacity; what the
    // device run in D.4.2 can still want is the number itself, so the high-water mark is
    // logged at MGLOG_D on the allocate-a-new-slot branch (once per new object, never on a
    // draw - ROADMAP.md:7).
    //
    // Single-threaded, like the rest of the renderer. Owned per VulkanRenderer (see
    // MagmaPipeIdentityTables): a process-global would share one table, and one reclamation
    // clock, across two live contexts.
    class MagmaPipeIdentityTable {
    public:
        explicit MagmaPipeIdentityTable(const char* kindName) : m_kindName(kindName) {}

        // Slots ever minted. A consumer table indexed by MagmaPipeSlotIndex() needs this many
        // entries; MagmaPipeSlotTable below grows itself, so nobody has to ask.
        Uint32 Count() const { return static_cast<Uint32>(m_entries.size()); }
        // Objects currently holding a slot - the live working set this table tracks.
        Uint32 LiveCount() const { return static_cast<Uint32>(m_index.size()); }

        MG_Pipe::MGPipeHandle Acquire(Uint64 lifetimeId) {
            // Unreachable: MG_State hands out lifetime ids from 1 precisely so that a
            // zero-initialised memo slot cannot carry a live object's id. Guarded anyway so
            // that a zero can never be minted into a slot and then indexed with.
            if (lifetimeId == 0) return MG_Pipe::kMGPipeNullHandle;
            // The one-entry front memo. Cleared by any retire, so it can never serve a slot
            // that has been handed back to the free list.
            if (lifetimeId == m_lastLifetimeId) {
                m_entries[m_lastIndex].LastUse = m_boundary;
                return m_lastHandle;
            }
            Uint32 index = 0;
            const auto it = m_index.find(lifetimeId);
            if (it != m_index.end()) {
                index = it->second;
            } else {
                index = ClaimSlot();
                m_entries[index].LifetimeId = lifetimeId;
                m_index.emplace(lifetimeId, index);
            }
            Entry& entry = m_entries[index];
            entry.LastUse = m_boundary;
            m_lastLifetimeId = lifetimeId;
            m_lastIndex = index;
            m_lastHandle = MG_Pipe::MGPipeHandle{index + MG_Pipe::kMGPipeFirstAllocatableSlot,
                                                 entry.Gen};
            return m_lastHandle;
        }

        // Ages the table and returns idle slots to the free list. Same shape and the same
        // self-gating as VertexInputStateFactory::OnFrameBoundary, which is what the reclaimed
        // slots' consumers use.
        void OnFrameBoundary() {
            ++m_boundary;
            if ((m_boundary % kSweepInterval) != 0) return;
            SizeT retired = 0;
            for (auto it = m_index.begin(); it != m_index.end();) {
                Entry& entry = m_entries[it->second];
                if ((m_boundary - entry.LastUse) > kRetireAgeBoundaries) {
                    entry.LifetimeId = 0;
                    m_freeSlots.push_back(it->second);
                    it = m_index.erase(it);
                    ++retired;
                } else {
                    ++it;
                }
            }
            if (retired != 0) {
                // A retired slot's Gen has not moved yet - it moves when the slot is reused -
                // so a front memo pointing at one would still hand out a handle the consumer
                // tables would accept. Drop it.
                m_lastLifetimeId = 0;
                m_lastHandle = MG_Pipe::kMGPipeNullHandle;
                MGLOG_D("MagmaPipeIdentityTable(%s): retired %zu idle slots, %u live of %u minted",
                        m_kindName, retired, LiveCount(), Count());
            }
        }

    private:
        // Sweep cadence and retirement age, deliberately the same numbers
        // VertexInputStateFactory::OnFrameBoundary uses for the entries these slots key: a slot
        // retired earlier than its cache entry would mint a new handle for an object whose
        // entry is still live and still correct, which is a pure waste.
        static constexpr Uint64 kSweepInterval = 256;
        static constexpr Uint64 kRetireAgeBoundaries = 1024;

        struct Entry {
            Uint64 LifetimeId = 0;
            Uint64 LastUse = 0;
            // Moves ONLY on slot reuse, never on respecify: an object that keeps its slot keeps
            // its generation, which is what makes a memo survive a reconfiguration.
            Uint32 Gen = 0;
        };

        Uint32 ClaimSlot() {
            while (!m_freeSlots.empty()) {
                const Uint32 index = m_freeSlots.back();
                m_freeSlots.pop_back();
                // MGPipeHandles.h:52-58 defends the Gen wrap only in a debug allocator, and
                // MOBILEGL_ASSERT is compiled out of every build P2 runs (Defines.h: asserts are
                // live only at MOBILEGL_LOG_ACTIVE_LEVEL == DEBUG). So the wrap is handled on the
                // RELEASE path instead of asserted: a slot that has been reused 2^32 times is
                // permanently retired rather than wrapped, because a wrapped Gen would let a
                // stale handle match a live object. It costs one slot.
                if (m_entries[index].Gen == ~Uint32{0}) {
                    MGLOG_W("MagmaPipeIdentityTable(%s): slot %u reached generation 2^32-1 and is "
                            "retired for good; {slot, gen} stays unique",
                            m_kindName, index + MG_Pipe::kMGPipeFirstAllocatableSlot);
                    continue;
                }
                ++m_entries[index].Gen;
                return index;
            }
            const Uint32 index = static_cast<Uint32>(m_entries.size());
            m_entries.push_back(Entry{});
            m_entries[index].Gen = 1;
            // The high-water mark, at powers of two from 1024 up: at most a handful of lines
            // for a whole session, emitted from the allocate-a-NEW-slot branch, i.e. once per
            // object this backend has ever seen and never on a draw (ROADMAP.md:7).
            //
            // [narrow, declared deviation from D20's "MGLOG_D for anything non-critical"] This
            // one is I, not D, because D is compiled out of every build that ships and of every
            // build P2 measures, and this line IS the measurement review v2's MAJOR 1 asks for:
            // the live-object high-water mark of minecraft-1.21.4-in-world and
            // ...-sodium-in-world, which nothing on desktop reaches and no gate here can see.
            // The structure no longer has a capacity to size off it, so the number is evidence
            // rather than a tuning input - but D.4.2 should still read it out of the device log,
            // and it cannot read a line that was compiled away.
            const SizeT minted = m_entries.size();
            if (minted >= 1024 && (minted & (minted - 1)) == 0) {
                MGLOG_I("MagmaPipeIdentityTable(%s): high-water %zu slots minted, %u live",
                        m_kindName, minted, LiveCount());
            }
            return index;
        }

        const char* m_kindName = "";
        Uint64 m_boundary = 0;
        Vector<Entry> m_entries;
        Vector<Uint32> m_freeSlots;
        UnorderedMap<Uint64, Uint32> m_index;
        // One-entry front memo (see Acquire). m_lastLifetimeId == 0 means "empty": a live
        // object's lifetime id is never 0.
        Uint64 m_lastLifetimeId = 0;
        Uint32 m_lastIndex = 0;
        MG_Pipe::MGPipeHandle m_lastHandle = MG_Pipe::kMGPipeNullHandle;
    };

    // The two mints one renderer owns. Per renderer, NOT process-global: two live contexts (or
    // a context recreation, which destroys and rebuilds the renderer) would otherwise share one
    // table and one reclamation clock, and both consumer tables are per-instance already.
    class MagmaPipeIdentityTables {
    public:
        // A VAO is kind VertexElementsCso: that is the gallium-shaped CSO a vertex array
        // resolves to, and the only kind in MGPipeKind that names vertex-input state.
        MG_Pipe::MGPipeHandle HandleOf(MG_Pipe::MGPipeKind kind, Uint64 lifetimeId) {
            return kind == MG_Pipe::MGPipeKind::Buffer ? m_buffers.Acquire(lifetimeId)
                                                       : m_vaos.Acquire(lifetimeId);
        }
        void OnFrameBoundary() {
            m_vaos.OnFrameBoundary();
            m_buffers.OnFrameBoundary();
        }
        const MagmaPipeIdentityTable& Vaos() const { return m_vaos; }
        const MagmaPipeIdentityTable& Buffers() const { return m_buffers; }

    private:
        MagmaPipeIdentityTable m_vaos{"VertexElementsCso"};
        MagmaPipeIdentityTable m_buffers{"Buffer"};
    };

    // The table entry a handle names. Every per-slot table Magma keeps is indexed by this.
    //
    // A null handle has no slot, and it is unreachable here: both lifetime-id sources start at
    // 1 (VertexArrayObject.cpp, BufferObject.cpp), so Acquire's zero guard never fires. The
    // ternary, not the assertion, is what has effect in a shipped build (Defines.h compiles
    // MOBILEGL_ASSERT out at INFO), and slot 0 of a consumer table is a real entry that a null
    // handle can never match, because MGPipeHandleIsNull is also what the consumers compare.
    inline Uint32 MagmaPipeSlotIndex(const MG_Pipe::MGPipeHandle& handle) {
        MOBILEGL_ASSERT(!MG_Pipe::MGPipeHandleIsNull(handle),
                        "a null MGPipeHandle has no slot to index a per-slot table with");
        return MG_Pipe::MGPipeHandleIsNull(handle)
                   ? 0u
                   : handle.Slot - MG_Pipe::kMGPipeFirstAllocatableSlot;
    }

    // A grow-on-demand per-slot table whose ENTRY ADDRESSES NEVER MOVE.
    //
    // D12.4 asks for a grow-on-demand Vector, and with an unbounded mint that is what a
    // consumer needs - but a Vector that grows relocates its elements, and the draw path holds
    // references into these entries across nested calls. Chunks of kChunkEntries are appended
    // instead: the Vector of owning pointers reallocates, the chunks never do, so an entry
    // reference is valid for the life of the table. That is the same guarantee the fixed table
    // it replaces gave, without the fixed capacity.
    template <typename T, Uint32 kChunkEntries = 256>
    class MagmaPipeSlotTable {
    public:
        T& operator[](Uint32 index) {
            const Uint32 chunk = index / kChunkEntries;
            while (m_chunks.size() <= chunk) {
                m_chunks.push_back(MakeUnique<Chunk>());
            }
            return m_chunks[chunk]->Entries[index % kChunkEntries];
        }
        SizeT Capacity() const { return m_chunks.size() * kChunkEntries; }

    private:
        struct Chunk {
            T Entries[kChunkEntries] = {};
        };
        Vector<UniquePtr<Chunk>> m_chunks;
    };
#endif // MOBILEGL_PIPE_PUSH
} // namespace MobileGL::MG_Backend::DirectVulkan
