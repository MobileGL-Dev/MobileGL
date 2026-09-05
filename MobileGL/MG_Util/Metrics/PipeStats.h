// MobileGL - MobileGL/MG_Util/Metrics/PipeStats.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// MGPipe boundary counters (plan B section 11 "P0 - hygiene, measurement, gates and
// skeleton", and the corollary in section 2.3.1).
//
// WHAT THIS IS FOR. The disaggregation plan has to size two things it cannot size by
// reading the tree: how many BYTES cross the frontend/backend boundary per frame (that
// sizes SEG_STAGE and the command segment), and how many accessor CALLS and memo-gate
// probes the backends actually execute per draw (that decides whether pushing state is
// cheaper than pulling it at all). Section 2.3.1 makes the second one the load-bearing
// number: the static call-site counts everyone quoted - Espryt 124 / Magma 169 - are NOT
// the dynamic per-draw cost, because every one of those paths is memo-gated, and the real
// steady state is believed to be 10-25 accessor calls per backend per draw. Without a
// dynamic counter the P2 verdict stays a guess.
//
// COST WHEN OFF. g_pipeStatsEnabled is a plain global Bool latched once at Init() from
// MG_Config::Features.PipeStats (MOBILEGL_PIPE_STATS). Every counting site in the two
// backends is written as
//
//     if (MG_Util::PipeStats::Enabled()) MG_Util::PipeStats::Add...(...);
//
// so with the feature off a site costs one load of a hot global plus one never-taken,
// perfectly-predicted branch, and none of the counter state is touched. The counters
// themselves are relaxed atomics rather than plain integers because texture and buffer
// staging can be reached from more than one thread; relaxed adds cost nothing extra on the
// off path, which never reaches them.
//
// WHAT IS COUNTED AND WHAT IS NOT: see the site inventory in PipeStats.cpp.
namespace MobileGL::MG_Util::PipeStats {

    // Byte classes. Every one of these names a population of bytes that would have to be
    // MOVED across the boundary once the backend no longer shares an address space with
    // the frontend, which is why they are grouped this way rather than by call site.
    enum class ByteClass : Uint32 {
        // Buffer object contents flushed to the driver: glBufferData / glBufferSubData /
        // map-write ranges / the persistent upload ring.
        StageBuffer = 0,
        // Texel bytes handed to glTexSubImage & friends, whichever upload shape was chosen.
        StageTexture,
        // The default-uniform-block ("global UBO") image, uploaded at most once per program
        // per frame.
        StageUboGlobal,
        // Named uniform-block bytes that a backend has to repack itself, i.e. Magma's UBO
        // ring. Espryt binds the frontend buffer straight to the driver and contributes
        // nothing here - which is exactly the asymmetry D-B8 is about.
        StageUboNamed,
        // Client-memory vertex arrays uploaded into a scratch VBO on the draw path.
        StageVertexClient,
        // Client-memory / rewritten index data staged on the draw path.
        StageIndexClient,
        // Bytes pushed because a persistently mapped range was published to the backend.
        PersistentMapPush,
        // PLACEHOLDER (plan section 6.3): the residual value block does not exist yet. The
        // class is minted now so the counter names never churn; it stays at 0 until P2.
        ResidualValueBlock,
        Count
    };

    // Call classes: the dynamic per-draw cost section 2.3.1 says P2 cannot be decided
    // without.
    enum class CallClass : Uint32 {
        // Draws that reached an instrumented backend draw-preparation entry point. The
        // denominator for every "per draw" number below.
        Draws = 0,
        // GLContext accessor calls actually EXECUTED on the instrumented paths. Counted in
        // static tallies at the ~10 hot entry points, not by wrapping all 293 call sites -
        // see the inventory in PipeStats.cpp for exactly what is and is not in this number.
        AccessorCalls,
        // Texture upload emissions: one per (upload target, level) that actually shipped
        // texels. The eventual resource_subdata record count.
        TextureUploadEmissions,
        // Emissions that took the union-box shape (one driver upload job).
        TextureUploadBoxEmissions,
        // Emissions that took the refined rect-list shape (N driver upload jobs). The
        // box/rect split is the thing SSIM cannot see and the +6 ms/frame Mali cliff came
        // from, so it is counted separately from the byte total.
        TextureUploadRectEmissions,
        // Driver upload jobs issued by those emissions: 1 per box emission, N per rect-list
        // emission.
        TextureUploadJobs,
        Count
    };

    // Memo gates. Each is a place where a backend decides "nothing moved, skip the work".
    // Hit == the gate short-circuited; Miss == it fell through and did the work. The six
    // are exactly the ones section 2.3.1 tabulates.
    enum class Gate : Uint32 {
        // DirectGLES.cpp SyncRenderState: the render-state-version early-out.
        EsprytRenderState = 0,
        // DirectGLES.cpp SyncNeccessaryTextures: the six-value sync-list key compare.
        EsprytTextureSyncList,
        // DirectGLES.cpp CurrentUnitBindingsEpoch: the (context, max unit, bind generation)
        // shutter over the unit walk.
        EsprytUnitBindingsEpoch,
        // VulkanRenderer.cpp TrySetupDrawFastPath: the whole snapshot fast path.
        MagmaDrawFastPath,
        // VulkanRenderer.cpp GetOrCreatePipeline: the pipeline memo.
        MagmaPipelineMemo,
        // VulkanRenderer.cpp ApplyDynamicDrawStateTail: the version+extent tail gate.
        MagmaDynamicTail,
        Count
    };

    // Per-draw command payload size histogram (plan section 4.5.7: SEG_CMD has to be sized
    // off the DISTRIBUTION, not off a per-frame total). PLACEHOLDER in P0: MGPipe emits no
    // records yet, so nothing in the backends calls RecordDrawPayloadBytes. The bucketing
    // and the reporting are implemented and unit-tested so that the first generator to
    // emit records only has to add the one call.
    inline constexpr Uint32 kPayloadHistogramBuckets = 24;

    // Frames between two summary lines when MOBILEGL_PIPE_STATS=1.
    inline constexpr Uint64 kSummaryFramePeriod = 120;

    // The latch. Read directly by Enabled() so the off path is a global load and a
    // predicted branch - do not turn this into a function call.
    extern Bool g_pipeStatsEnabled;

    inline Bool Enabled() { return g_pipeStatsEnabled; }

    // Latches g_pipeStatsEnabled from MG_Config::Features.PipeStats and resets every
    // counter. Called from MobileGL::Initialize() right after the config load.
    void Init();

    // Final summary line plus, if MOBILEGL_PIPE_STATS_FILE names a path, the JSON dump.
    // Called from MobileGL's teardown. Idempotent.
    void Shutdown();

    void AddBytes(ByteClass byteClass, Uint64 bytes);
    void AddCalls(CallClass callClass, Uint64 count);
    void CountGate(Gate gate, Bool hit);
    void RecordDrawPayloadBytes(Uint64 bytes);

    // Frame boundary: publishes the frame's values to Tracy (when TRACY_ENABLE), folds them
    // into the run totals, clears the frame accumulators, and every kSummaryFramePeriod
    // frames emits the summary line. Called from each backend's Present().
    void OnPresent();

    // --- introspection, for the unit test and the JSON dump -------------------------
    Uint64 FrameBytes(ByteClass byteClass);
    Uint64 TotalBytes(ByteClass byteClass);
    Uint64 FrameCalls(CallClass callClass);
    Uint64 TotalCalls(CallClass callClass);
    Uint64 TotalGateHits(Gate gate);
    Uint64 TotalGateMisses(Gate gate);
    Uint64 TotalPayloadBucket(Uint32 bucket);
    Uint64 FrameCount();

    const char* NameOf(ByteClass byteClass);
    const char* NameOf(CallClass callClass);
    const char* NameOf(Gate gate);

    // The compact fixed-format one-liner MGLOG_I prints. Same text in the log and in the
    // test, so the format is pinned by a test rather than by the log reader's memory.
    String FormatSummaryLine();
    // The teardown dump. Run totals only: a per-frame JSON stream is a different tool.
    String FormatJson();

    // Test hooks. Not used by any shipping path.
    void SetEnabledForTesting(Bool enabled);
    void ResetForTesting();

} // namespace MobileGL::MG_Util::PipeStats
