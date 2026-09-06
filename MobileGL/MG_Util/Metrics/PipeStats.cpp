// MobileGL - MobileGL/MG_Util/Metrics/PipeStats.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "PipeStats.h"

#include <Config.h>

#include <fstream>

// ---------------------------------------------------------------------------------------
// SITE INVENTORY - what these counters DO and DO NOT cover.
//
// This list is the contract. A byte class that reads 0 while a real copy runs uncounted is
// worse than a missing counter, because the zero is then read as an answer, so every path
// that moves bytes and is NOT wired is named here by file and function.
//
// Byte classes
//   stage-buffer         ESPRYT (DirectGLES Managers.cpp): RespecifyStorageNow's
//                        glBufferData, FlushPendingRangesNow's three shapes (map-write,
//                        glBufferSubData, upload-ring stage), and the pool-recycle reseed
//                        in SyncBufferObject.
//                        MAGMA (DirectVulkan VkBufferManager.cpp): every host->device copy
//                        of a buffer object's contents - SwapStorageAndUploadAll, the
//                        StagedRangeCopy staging fill, the in-place uploads in OnRespecify /
//                        OnSubData / OnFlushMappedRange, the AcquirePersistentMap seed, the
//                        AcquireResidentSlice initial upload and the AcquireStreamedSlice
//                        arena fill.
//                        NOT covered: bytes an app writes THROUGH a persistent map. Those
//                        never pass through either backend (D4/D-B4) - see
//                        persistent-map-push.
//   stage-texture        ESPRYT (Managers.cpp texture upload): the bytes of whichever of
//                        the three upload shapes ran (rect list / union box / whole level).
//                        MAGMA (VkTextureManager.cpp): the packed staging slice of an
//                        upload batch item set.
//                        NOT covered: Espryt's compressed-texture path, and both backends'
//                        readback (device->host) paths, which are a different direction and
//                        want their own class when the reverse channel of section 7 exists.
//   stage-ubo-global     ESPRYT (DirectGLES.cpp): the default-uniform-block image, both the
//                        UBO-ring memcpy and the glBufferSubData fallback.
//                        MAGMA (UniformManager::ResolveDynamicUboDescriptor): the same
//                        image, counted after the per-frame slice memo, so a frame that
//                        re-uses the slice correctly contributes nothing.
//   stage-ubo-named      DirectVulkan UniformManager::ResolveUniformBufferPayload - the
//                        bytes Magma repacks into its own UBO ring, counted AFTER the
//                        zero-copy direct-bind decision (a direct bind repacks nothing).
//                        Espryt contributes nothing by construction (D-B8).
//   stage-vertex-client  ESPRYT: BackendVertexArrayObject::SyncClientSideAttributesFor-
//                        DrawArrays (both the Float64-narrowing and the verbatim shapes)
//                        and the VBO-backed Float64->Float32 narrowing scratch upload.
//                        MAGMA: VkBufferManager::UploadTransient(BufferKind::Vertex), which
//                        is the single chokepoint for the converted-vertex-stream and
//                        client-array staging.
//   stage-index-client   ESPRYT: the primitive-restart substitution buffer, and MultiDraw's
//                        rewritten (rebased) index stream.
//                        MAGMA: VkBufferManager::UploadTransient(BufferKind::Index).
//   stage-indirect-cmd   ESPRYT MultiDraw.cpp: the DrawElementsIndirectCommand array staged
//                        for the indirect tiers, and the compute tier's per-draw info
//                        array. Kept out of stage-index-client because these are draw
//                        PARAMETERS - the population that becomes MGPipe command-record
//                        payload, not resource bytes.
//                        NOT covered: Magma builds no such array (it issues one vkCmdDraw*
//                        per sub-draw), so this class is Espryt-only by construction.
//   persistent-map-push  Not wired in P0: today a persistent map is a permanent address
//                        space donation (D4/D-B4) that survives the whole monolith track,
//                        so there is no push to count until the IPC track breaks it.
//   residual-value-block Placeholder, always 0 until P2 (plan section 6.3).
//
// Call classes
//   draws                DirectGLES PrepareForDraw and DirectVulkan SetupDraw's entry. A
//                        dispatch is not a draw and is not counted.
//   accessor-calls       STATIC TALLIES at the instrumented entry points, NOT a wrapper
//                        around all 293 pGLContext-> sites. Each instrumented function adds
//                        the number of GLContext accessor calls that its OWN body executed
//                        on the path taken, and each tally sits AFTER the last early return
//                        that would skip those reads. Covered: PrepareForDraw's own reads,
//                        SyncRenderState, CaptureDrawTextureSyncKeys/CurrentUnitBindings-
//                        Epoch, SyncNeccessaryTextures' walk, TrySetupDrawFastPath,
//                        GetOrCreatePipeline and ApplyDynamicDrawStateTail. NOT covered:
//                        the reads inside the callees those functions invoke (buffer/VAO/
//                        FBO/program sync, the pipeline payload builder's ~40 reads on a
//                        memo miss), and every non-draw entry point. The number is
//                        therefore a LOWER BOUND on the per-draw accessor count, and it is
//                        the bound over exactly the six gates section 2.3.1 tabulates.
//   texture-*            Per (target, level) emission, both backends.
//
// Gates: the six of section 2.3.1, each counted exactly once per probe.
//
// READING acc/draw. The accessor tally covers the instrumented functions wherever they
// run, and three of them (SyncRenderState, the texture-key capture, SyncNeccessaryTextures)
// are also reached from NON-draw call sites - Clear, readbacks, the DSA by-name entry
// points - which the `draws` counter deliberately does not count. So acc/draw is the
// per-draw steady-state number section 2.3.1 asks for only in a DRAW-DOMINATED window; in a
// window dominated by clears and readbacks it is inflated by exactly those non-draw
// probes, and the gate hit/miss pairs are the honest reading there.
// ---------------------------------------------------------------------------------------

namespace MobileGL::MG_Util::PipeStats {

    Bool g_pipeStatsEnabled = false;

    namespace {
        constexpr Uint32 kByteClassCount = static_cast<Uint32>(ByteClass::Count);
        constexpr Uint32 kCallClassCount = static_cast<Uint32>(CallClass::Count);
        constexpr Uint32 kGateCount = static_cast<Uint32>(Gate::Count);

        using Counter = std::atomic<Uint64>;

        Counter g_frameBytes[kByteClassCount];
        Counter g_totalBytes[kByteClassCount];
        Counter g_frameCalls[kCallClassCount];
        Counter g_totalCalls[kCallClassCount];
        Counter g_frameGateHit[kGateCount];
        Counter g_totalGateHit[kGateCount];
        Counter g_frameGateMiss[kGateCount];
        Counter g_totalGateMiss[kGateCount];
        Counter g_totalPayloadBuckets[kPayloadHistogramBuckets];
        Counter g_frameCount{0};

        // Window bases: the run totals as of the previous summary line. Only ever touched
        // from OnPresent()/Shutdown() (the present thread), so plain integers.
        Uint64 g_windowBaseBytes[kByteClassCount] = {};
        Uint64 g_windowBaseCalls[kCallClassCount] = {};
        Uint64 g_windowBaseGateHit[kGateCount] = {};
        Uint64 g_windowBaseGateMiss[kGateCount] = {};
        Uint64 g_windowBaseFrames = 0;
        Bool g_shutdownDone = false;

        inline void Bump(Counter& counter, Uint64 amount) {
            counter.fetch_add(amount, std::memory_order_relaxed);
        }

        inline Uint64 Read(const Counter& counter) { return counter.load(std::memory_order_relaxed); }

        // Bucket 0 is "0 bytes", bucket n>0 holds [2^(n-1), 2^n). Saturates at the last
        // bucket so a pathological record cannot index out of the array.
        Uint32 PayloadBucketOf(Uint64 bytes) {
            if (bytes == 0) {
                return 0;
            }
            Uint32 bucket = 1;
            while (bucket + 1 < kPayloadHistogramBuckets && bytes >= (Uint64{1} << bucket)) {
                ++bucket;
            }
            return bucket;
        }

        // Two decimals without <iomanip>. Every per-frame and per-draw field in the summary
        // goes through this: the numbers are small (a per-draw accessor count in the 10-25
        // band, a per-frame byte count that sizes SEG_STAGE), so truncating integer division
        // loses up to a whole unit on exactly the figures the package exists to produce.
        // A zero denominator is "n/a" rather than a division by a faked 1.
        String FormatFixed2(Uint64 numerator, Uint64 denominator) {
            if (denominator == 0) {
                return "n/a";
            }
            const Uint64 hundredths = (numerator * 100 + denominator / 2) / denominator;
            return std::to_string(hundredths / 100) + "." + (hundredths % 100 < 10 ? "0" : "") +
                   std::to_string(hundredths % 100);
        }

        const char* const kByteClassNames[kByteClassCount] = {
            "stage-buffer",        "stage-texture",       "stage-ubo-global",
            "stage-ubo-named",     "stage-vertex-client", "stage-index-client",
            "stage-indirect-cmd",  "persistent-map-push", "residual-value-block",
        };
        const char* const kCallClassNames[kCallClassCount] = {
            "draws", "accessor-calls", "tex-upload-emissions", "tex-upload-box", "tex-upload-rect",
            "tex-upload-jobs",
        };
        const char* const kGateNames[kGateCount] = {
            "espryt-render-state", "espryt-texture-sync-list", "espryt-unit-bindings-epoch",
            "magma-draw-fastpath", "magma-pipeline-memo",      "magma-dynamic-tail",
        };
        // Tracy needs a stable string literal per series, and a gate is TWO series: plotting
        // only the misses (which is what the first cut did) hides the denominator, and a
        // gate's whole point is the ratio.
        const char* const kGateHitPlotNames[kGateCount] = {
            "espryt-render-state-hit", "espryt-texture-sync-list-hit", "espryt-unit-bindings-epoch-hit",
            "magma-draw-fastpath-hit", "magma-pipeline-memo-hit",      "magma-dynamic-tail-hit",
        };
        const char* const kGateMissPlotNames[kGateCount] = {
            "espryt-render-state-miss", "espryt-texture-sync-list-miss", "espryt-unit-bindings-epoch-miss",
            "magma-draw-fastpath-miss", "magma-pipeline-memo-miss",      "magma-dynamic-tail-miss",
        };
        // Short forms, so the per-120-frame line stays one terminal line wide.
        const char* const kByteClassShort[kByteClassCount] = {"buf",  "tex",  "ubog", "ubon", "vtxc",
                                                              "idxc", "icmd", "pmap", "resid"};
        const char* const kGateShort[kGateCount] = {"ers", "etl", "eub", "mfp", "mpm", "mdt"};

        void ResetCounters() {
            for (Uint32 i = 0; i < kByteClassCount; ++i) {
                g_frameBytes[i].store(0, std::memory_order_relaxed);
                g_totalBytes[i].store(0, std::memory_order_relaxed);
                g_windowBaseBytes[i] = 0;
            }
            for (Uint32 i = 0; i < kCallClassCount; ++i) {
                g_frameCalls[i].store(0, std::memory_order_relaxed);
                g_totalCalls[i].store(0, std::memory_order_relaxed);
                g_windowBaseCalls[i] = 0;
            }
            for (Uint32 i = 0; i < kGateCount; ++i) {
                g_frameGateHit[i].store(0, std::memory_order_relaxed);
                g_totalGateHit[i].store(0, std::memory_order_relaxed);
                g_frameGateMiss[i].store(0, std::memory_order_relaxed);
                g_totalGateMiss[i].store(0, std::memory_order_relaxed);
                g_windowBaseGateHit[i] = 0;
                g_windowBaseGateMiss[i] = 0;
            }
            for (Uint32 i = 0; i < kPayloadHistogramBuckets; ++i) {
                g_totalPayloadBuckets[i].store(0, std::memory_order_relaxed);
            }
            g_frameCount.store(0, std::memory_order_relaxed);
            g_windowBaseFrames = 0;
        }

        void EmitSummaryLine() {
            const String line = FormatWindowLine();
            // MGLOG_I on purpose, against the project's usual "MGLOG_D for anything
            // non-critical" rule: the line has to survive an INFO build (that is the only
            // build a device ever runs), it is emitted at most once per 120 frames, and it
            // exists at all only when the operator set MOBILEGL_PIPE_STATS=1. It is an
            // opt-in measurement channel, not per-frame noise.
            MGLOG_I("%s", line.c_str());
            AdvanceSummaryWindow();
        }

        void WriteJsonDump() {
            const String& path = MG_Config::Features.PipeStatsFile;
            if (path.empty()) {
                return;
            }
            std::ofstream out(path, std::ios::out | std::ios::trunc);
            if (!out) {
                MGLOG_W("PipeStats: could not open MOBILEGL_PIPE_STATS_FILE='%s' for writing", path.c_str());
                return;
            }
            out << FormatJson();
            out.flush();
            if (!out) {
                MGLOG_W("PipeStats: failed writing MOBILEGL_PIPE_STATS_FILE='%s'", path.c_str());
                return;
            }
            MGLOG_I("MGPipe stats: wrote JSON dump to %s", path.c_str());
        }
    } // namespace

    void Init() {
        ResetCounters();
        g_shutdownDone = false;
        g_pipeStatsEnabled = MG_Config::Features.PipeStats;
        if (g_pipeStatsEnabled) {
            MGLOG_I("MGPipe stats: counters ON (MOBILEGL_PIPE_STATS), summary every %llu frames%s%s",
                    static_cast<unsigned long long>(kSummaryFramePeriod),
                    MG_Config::Features.PipeStatsFile.empty() ? "" : ", JSON dump to ",
                    MG_Config::Features.PipeStatsFile.c_str());
        }
    }

    void Shutdown() {
        if (!g_pipeStatsEnabled || g_shutdownDone) {
            return;
        }
        g_shutdownDone = true;
        EmitSummaryLine();
        WriteJsonDump();
    }

    void AddBytes(ByteClass byteClass, Uint64 bytes) {
        const Uint32 index = static_cast<Uint32>(byteClass);
        Bump(g_frameBytes[index], bytes);
        Bump(g_totalBytes[index], bytes);
    }

    void AddCalls(CallClass callClass, Uint64 count) {
        const Uint32 index = static_cast<Uint32>(callClass);
        Bump(g_frameCalls[index], count);
        Bump(g_totalCalls[index], count);
    }

    void CountGate(Gate gate, Bool hit) {
        const Uint32 index = static_cast<Uint32>(gate);
        if (hit) {
            Bump(g_frameGateHit[index], 1);
            Bump(g_totalGateHit[index], 1);
        } else {
            Bump(g_frameGateMiss[index], 1);
            Bump(g_totalGateMiss[index], 1);
        }
    }

    void RecordDrawPayloadBytes(Uint64 bytes) { Bump(g_totalPayloadBuckets[PayloadBucketOf(bytes)], 1); }

    void OnPresent() {
        // Every frame accumulator is EXCHANGED for zero, and the exchanged value is what gets
        // plotted. A read followed by a store(0) would lose any Bump that lands in between -
        // buffer and texture staging reach these counters from more than one thread - from
        // the plot AND from every frame; an exchange hands every add to exactly one frame.
        // Without Tracy the value is taken and dropped: the clear is still the point.
        //
        // One plot per counter, the frame's value. Tracy keeps the series by name, and the
        // names are the static literals above, which is what TracyPlot requires. A gate is
        // two series - hits and misses - because the ratio is the deliverable and a miss
        // count alone cannot be read.
        //
        // The payload histogram is deliberately NOT plotted: it is a run-total distribution
        // over draws (section 4.5.7), not a per-frame scalar, and Tracy has no histogram
        // series. It reaches the operator through the JSON dump.
        const auto take = [](Counter& counter) { return counter.exchange(0, std::memory_order_relaxed); };
        for (Uint32 i = 0; i < kByteClassCount; ++i) {
            const Uint64 value = take(g_frameBytes[i]);
            (void)value;
#ifdef TRACY_ENABLE
            TracyPlot(kByteClassNames[i], static_cast<Int64>(value));
#endif
        }
        for (Uint32 i = 0; i < kCallClassCount; ++i) {
            const Uint64 value = take(g_frameCalls[i]);
            (void)value;
#ifdef TRACY_ENABLE
            TracyPlot(kCallClassNames[i], static_cast<Int64>(value));
#endif
        }
        for (Uint32 i = 0; i < kGateCount; ++i) {
            const Uint64 hits = take(g_frameGateHit[i]);
            const Uint64 misses = take(g_frameGateMiss[i]);
            (void)hits;
            (void)misses;
#ifdef TRACY_ENABLE
            TracyPlot(kGateHitPlotNames[i], static_cast<Int64>(hits));
            TracyPlot(kGateMissPlotNames[i], static_cast<Int64>(misses));
#endif
        }
        const Uint64 frames = g_frameCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (frames % kSummaryFramePeriod == 0) {
            EmitSummaryLine();
        }
    }

    Uint64 FrameBytes(ByteClass byteClass) { return Read(g_frameBytes[static_cast<Uint32>(byteClass)]); }
    Uint64 TotalBytes(ByteClass byteClass) { return Read(g_totalBytes[static_cast<Uint32>(byteClass)]); }
    Uint64 FrameCalls(CallClass callClass) { return Read(g_frameCalls[static_cast<Uint32>(callClass)]); }
    Uint64 TotalCalls(CallClass callClass) { return Read(g_totalCalls[static_cast<Uint32>(callClass)]); }
    Uint64 TotalGateHits(Gate gate) { return Read(g_totalGateHit[static_cast<Uint32>(gate)]); }
    Uint64 TotalGateMisses(Gate gate) { return Read(g_totalGateMiss[static_cast<Uint32>(gate)]); }
    Uint64 TotalPayloadBucket(Uint32 bucket) {
        return bucket < kPayloadHistogramBuckets ? Read(g_totalPayloadBuckets[bucket]) : 0;
    }
    Uint64 FrameCount() { return Read(g_frameCount); }

    const char* NameOf(ByteClass byteClass) { return kByteClassNames[static_cast<Uint32>(byteClass)]; }
    const char* NameOf(CallClass callClass) { return kCallClassNames[static_cast<Uint32>(callClass)]; }
    const char* NameOf(Gate gate) { return kGateNames[static_cast<Uint32>(gate)]; }

    String FormatWindowLine() {
        // Window values: everything since the previous summary. A run total over a workload
        // whose shape changes (load, then steady state) hides exactly the number P2 wants.
        const Uint64 frames = Read(g_frameCount);
        const Uint64 windowFrames = frames - g_windowBaseFrames;
        // A window with no Present in it (teardown before the first frame, or a slice whose
        // whole workload runs off-screen) has NO per-frame reading. Printing the window
        // totals under a "/f" label there is how a 47x overstatement of the SEG_STAGE sizing
        // input got printed as a per-frame figure; the label changes instead.
        const Bool perFrame = windowFrames != 0;

        Uint64 bytes[kByteClassCount];
        for (Uint32 i = 0; i < kByteClassCount; ++i) {
            bytes[i] = Read(g_totalBytes[i]) - g_windowBaseBytes[i];
        }
        Uint64 calls[kCallClassCount];
        for (Uint32 i = 0; i < kCallClassCount; ++i) {
            calls[i] = Read(g_totalCalls[i]) - g_windowBaseCalls[i];
        }
        Uint64 gateHit[kGateCount];
        Uint64 gateMiss[kGateCount];
        for (Uint32 i = 0; i < kGateCount; ++i) {
            gateHit[i] = Read(g_totalGateHit[i]) - g_windowBaseGateHit[i];
            gateMiss[i] = Read(g_totalGateMiss[i]) - g_windowBaseGateMiss[i];
        }

        const Uint64 draws = calls[static_cast<Uint32>(CallClass::Draws)];
        const Uint64 accessorCalls = calls[static_cast<Uint32>(CallClass::AccessorCalls)];

        String line = "MGPipe stats:";
        line += " frames=" + std::to_string(frames);
        line += " window=" + std::to_string(windowFrames);
        line += " draws=" + std::to_string(draws);
        line += " draws/f=" + FormatFixed2(draws, windowFrames);
        line += " acc=" + std::to_string(accessorCalls);
        // Same rule as the per-frame fields: a window with no draw in it has no per-draw
        // number, and "0.00" next to a non-zero acc= is the same lie in a smaller font.
        line += " acc/draw=" + FormatFixed2(accessorCalls, draws);
        // "bytes/f[...]" only when there IS a frame to divide by; otherwise the bracket is
        // labelled "bytes[...]" and carries the window totals verbatim.
        line += perFrame ? " bytes/f[" : " bytes[";
        for (Uint32 i = 0; i < kByteClassCount; ++i) {
            if (i != 0) {
                line += " ";
            }
            line += kByteClassShort[i];
            line += "=";
            line += perFrame ? FormatFixed2(bytes[i], windowFrames) : std::to_string(bytes[i]);
        }
        line += "] tex[emit=" + std::to_string(calls[static_cast<Uint32>(CallClass::TextureUploadEmissions)]);
        line += " box=" + std::to_string(calls[static_cast<Uint32>(CallClass::TextureUploadBoxEmissions)]);
        line += " rect=" + std::to_string(calls[static_cast<Uint32>(CallClass::TextureUploadRectEmissions)]);
        line += " jobs=" + std::to_string(calls[static_cast<Uint32>(CallClass::TextureUploadJobs)]);
        line += "] gates[";
        for (Uint32 i = 0; i < kGateCount; ++i) {
            if (i != 0) {
                line += " ";
            }
            line += kGateShort[i];
            line += "=";
            line += std::to_string(gateHit[i]);
            line += "/";
            line += std::to_string(gateMiss[i]);
        }
        line += "]";
        return line;
    }

    void AdvanceSummaryWindow() {
        for (Uint32 i = 0; i < kByteClassCount; ++i) {
            g_windowBaseBytes[i] = Read(g_totalBytes[i]);
        }
        for (Uint32 i = 0; i < kCallClassCount; ++i) {
            g_windowBaseCalls[i] = Read(g_totalCalls[i]);
        }
        for (Uint32 i = 0; i < kGateCount; ++i) {
            g_windowBaseGateHit[i] = Read(g_totalGateHit[i]);
            g_windowBaseGateMiss[i] = Read(g_totalGateMiss[i]);
        }
        g_windowBaseFrames = Read(g_frameCount);
    }

    String FormatJson() {
        String json = "{\n";
        json += "  \"frames\": " + std::to_string(Read(g_frameCount)) + ",\n";
        json += "  \"bytes\": {\n";
        for (Uint32 i = 0; i < kByteClassCount; ++i) {
            json += "    \"";
            json += kByteClassNames[i];
            json += "\": " + std::to_string(Read(g_totalBytes[i]));
            json += (i + 1 == kByteClassCount) ? "\n" : ",\n";
        }
        json += "  },\n  \"calls\": {\n";
        for (Uint32 i = 0; i < kCallClassCount; ++i) {
            json += "    \"";
            json += kCallClassNames[i];
            json += "\": " + std::to_string(Read(g_totalCalls[i]));
            json += (i + 1 == kCallClassCount) ? "\n" : ",\n";
        }
        json += "  },\n  \"gates\": {\n";
        for (Uint32 i = 0; i < kGateCount; ++i) {
            json += "    \"";
            json += kGateNames[i];
            json += "\": {\"hit\": " + std::to_string(Read(g_totalGateHit[i])) +
                    ", \"miss\": " + std::to_string(Read(g_totalGateMiss[i])) + "}";
            json += (i + 1 == kGateCount) ? "\n" : ",\n";
        }
        json += "  },\n  \"cmd-bytes-per-draw-histogram\": [";
        for (Uint32 i = 0; i < kPayloadHistogramBuckets; ++i) {
            if (i != 0) {
                json += ", ";
            }
            json += std::to_string(Read(g_totalPayloadBuckets[i]));
        }
        json += "]\n}\n";
        return json;
    }

    void SetEnabledForTesting(Bool enabled) { g_pipeStatsEnabled = enabled; }

    void ResetForTesting() { ResetCounters(); }

} // namespace MobileGL::MG_Util::PipeStats
