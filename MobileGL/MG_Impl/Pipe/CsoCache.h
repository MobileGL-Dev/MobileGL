// MobileGL - MobileGL/MG_Impl/Pipe/CsoCache.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// The render-state CSO cache (ARCHITECTURE.md 4.5.2 / 5.3, P2 brief D7).
//
// THE LOOKUP, and the first step is the whole point:
//   1. m_pipelineStateVersion (widened) did not move -> reuse the last handle. ZERO hashing,
//      zero probing, and nothing is emitted unless m_version also moved. That is the steady
//      state of every frame, and it is why the tracker asks the cache at all only when the
//      dirty walk says the pipeline version moved.
//   2. moved -> hash the 396 pipeline bytes, probe, and on a hit CONFIRM WITH A MEMCMP
//      before reusing the handle. ARCHITECTURE.md 4.1 says content addressing on an
//      xxHash; a bare 64-bit equality would let a collision alias two different render
//      states onto one CSO, which is silent wrong pixels with no gate that can see it.
//      Mesa's cso_cache memcmps for the same reason. The memcmp only ever runs on a
//      pipeline-version change, i.e. never in the steady state.
//   3. miss -> mint a slot, emit create_render_state with every pipeline chunk, then bind.
//
// CAPACITY 64 (ROADMAP.md P2). 64 x (8 + 8 + 396 + 8) = about 26 KB per context. ROADMAP.md
// open question 4 says 64 is provisional and the counters retune it at P13; this ships 64
// and publishes the mint / bind / evict counters that retune reads.
//
// THE NEGATIVE CONTROL. kMGPipeBehaviourNoCsoContentAddressing (bit 63 of the runtime
// MOBILEGL_PIPE_PUSH bitmask) turns off the PROBE and the handle reuse, not the records:
// every pipeline-version change then mints a fresh CSO, binds it and evicts, which is
// precisely "whole-block content addressing" and reproduces the regression
// RenderState.h records. It is what separates "push is slower" from "the CSO design is
// slower", and CsoContentAddressingScenario (package E) is the always-on ctest that stops
// the switch from rotting.
//
// Header-only for the same ownership reason as Tracker.h: the root CMakeLists.txt that
// would name a new .cpp is package A's and is frozen behind the p2/contract tag.
#if MOBILEGL_PIPE_PUSH
#include <Config.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/MGPipeRenderStateSpans.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Util/Metrics/PipeStats.h>

#include <cstring>

namespace MobileGL::MG_Pipe {

    inline constexpr SizeT kMGPipeCsoCacheCapacity = 64;

    class MGPipeCsoCache {
    public:
        struct Counters {
            Uint64 Mints = 0;     // create_render_state emissions
            // bind_render_state emissions, mint or reuse. Counted in Acquire because Acquire
            // has exactly ONE caller (PipeFill.cpp's EmitRenderState) and that caller binds
            // immediately after every call - so "acquisitions" and "binds" are the same
            // number, and counting it here keeps the count from depending on an emitter
            // remembering to tick it. mints/binds is the cache's hit rate and it is the
            // number the CSO content-addressing negative control moves.
            Uint64 Binds = 0;
            Uint64 Hits = 0;      // a probe that found a live entry and passed the memcmp
            Uint64 Collisions = 0; // a hash hit the memcmp REJECTED - the reason it exists
            Uint64 Evictions = 0; // LRU evictions, each one a delete_render_state
        };

        // The handle for `params`' pipeline subset. Mints and emits create_render_state on a
        // miss; emits delete_render_state for whatever it evicts to make room. `payloadBytes`
        // accumulates what went on the wire, for PipeStats::RecordDrawPayloadBytes.
        MGPipeHandle Acquire(const RenderStateParameters& params, Uint64& payloadBytes) {
            Array<Uint8, kMGPipePipelineChunkBytes> bytes;
            MGPipeGatherPipelineBytes(params, bytes.data());
            ++m_counters.Binds;

            const Bool contentAddressed =
                (MG_Config::Features.PipePush & kMGPipeBehaviourNoCsoContentAddressing) == 0;
            if (contentAddressed) {
                const Uint64 hash = s_hashForTest != nullptr ? s_hashForTest(bytes.data())
                                                             : MGPipeHashPipelineBytes(bytes.data());
                for (SizeT i = 0; i < m_entries.size(); ++i) {
                    if (m_entries[i].Hash != hash) continue;
                    if (std::memcmp(m_entries[i].Bytes.data(), bytes.data(), bytes.size()) != 0) {
                        // A 64-bit collision between two DIFFERENT render states. Reusing the
                        // handle here would render one state with the other's pipeline, so the
                        // entry is dropped and the caller mints - correctness first, and the
                        // counter says how often it happened.
                        ++m_counters.Collisions;
                        Evict(i);
                        break;
                    }
                    m_entries[i].LastUsed = ++m_clock;
                    ++m_counters.Hits;
                    return m_entries[i].Cso;
                }
                return Mint(hash, bytes, payloadBytes);
            }
            // Content addressing OFF: never probe, always mint. The records still exist, so
            // the arm differs from the default one in exactly one thing - whether a handle is
            // reused - which is what makes it a control rather than a different design.
            return Mint(0, bytes, payloadBytes);
        }

        // Context teardown, a server reset, a unit test's fixture. Emits nothing: the applier
        // is reset alongside, and a delete for a record that is about to be dropped anyway
        // would be a wire message with no reader.
        void Reset() {
            for (auto& entry : m_entries) MGPipeSlots().Free(MGPipeKind::RenderStateCso, entry.Cso);
            m_entries.clear();
            m_clock = 0;
        }

        void ResetCounters() { m_counters = Counters{}; }

        SizeT Size() const { return m_entries.size(); }
        const Counters& GetCounters() const { return m_counters; }

        // TEST SEAM, and it is here because the thing it tests cannot be reached any other
        // way. A 64-bit collision between two DIFFERENT render states is silent wrong pixels
        // and it is exactly what the memcmp confirm above exists to stop, so
        // CsoCacheTest.HashCollisionDoesNotAliasTwoStates has to be able to make one happen.
        // Null in every real build - one never-taken, perfectly-predicted branch on a path
        // that runs only when the pipeline version moved, i.e. never in the steady state.
        using HashForTestFn = Uint64 (*)(const void* pipelineBytes);
        inline static HashForTestFn s_hashForTest = nullptr;

    private:
        struct Entry {
            Uint64 Hash = 0;
            Uint64 LastUsed = 0;
            MGPipeHandle Cso = kMGPipeNullHandle;
            Array<Uint8, kMGPipePipelineChunkBytes> Bytes{};
        };

        MGPipeHandle Mint(Uint64 hash, const Array<Uint8, kMGPipePipelineChunkBytes>& bytes,
                          Uint64& payloadBytes) {
            if (m_entries.size() >= kMGPipeCsoCacheCapacity) {
                SizeT victim = 0;
                for (SizeT i = 1; i < m_entries.size(); ++i) {
                    if (m_entries[i].LastUsed < m_entries[victim].LastUsed) victim = i;
                }
                Evict(victim);
            }

            const MGPipeHandle cso = MGPipeSlots().Allocate(MGPipeKind::RenderStateCso);
            MGPRenderStateDesc desc{};
            desc.Cso = cso;
            desc.BaseCso = kMGPipeNullHandle;
            // A brand-new CSO names every pipeline chunk; the incremental form against a
            // BaseCso is what the applier's assertion allows and P3 will use once a CSO is
            // minted from a neighbour rather than from nothing.
            desc.ChunkMask = kAllPipelineChunks;
            desc.Blob.Size = kMGPipePipelineChunkBytes;
            MGPipeApplyCreateRenderState(desc, bytes.data());
            payloadBytes += sizeof(MGPRenderStateDesc) + kMGPipePipelineChunkBytes;

            Entry entry;
            entry.Hash = hash;
            entry.LastUsed = ++m_clock;
            entry.Cso = cso;
            entry.Bytes = bytes;
            m_entries.push_back(entry);

            ++m_counters.Mints;
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::RenderStateCsoMints, 1);
            }
            return cso;
        }

        void Evict(SizeT index) {
            MGPHandleOnly handle{};
            handle.Handle = m_entries[index].Cso;
            handle.Kind = static_cast<Uint32>(MGPipeKind::RenderStateCso);
            MGPipeApplyDeleteRenderState(handle);
            MGPipeSlots().Free(MGPipeKind::RenderStateCso, m_entries[index].Cso);
            m_entries[index] = m_entries.back();
            m_entries.pop_back();
            ++m_counters.Evictions;
        }

        static constexpr Uint32 kAllPipelineChunks =
            static_cast<Uint32>((Uint64{1} << kMGPipePipelineChunkCount) - 1);

        Vector<Entry> m_entries;
        Uint64 m_clock = 0;
        Counters m_counters;
    };

    // The monolith's one cache, held beside the tracker. A Vector scan rather than a hash
    // map on purpose: 64 entries of Uint64 is a handful of cache lines, it is probed only
    // when the pipeline version moved, and it keeps the eviction order in the same array as
    // the content - a map would need a second structure to answer "which is oldest".
    inline MGPipeCsoCache& MGPipeCsoCacheInstance() {
        // NEVER DESTROYED, for MGPipeTrackerInstance()' reason (MG_Impl/Pipe/Tracker.h): the
        // rule covers every MGPipe process singleton, not only the ones on today's death
        // paths.
        static MGPipeCsoCache* cache = new MGPipeCsoCache();
        return *cache;
    }
} // namespace MobileGL::MG_Pipe
#endif // MOBILEGL_PIPE_PUSH
