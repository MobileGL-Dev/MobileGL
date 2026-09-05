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
// Byte classes
//   stage-buffer         DirectGLES Managers.cpp: RespecifyStorageNow's glBufferData,
//                        FlushPendingRangesNow's three shapes (map-write, glBufferSubData,
//                        upload-ring stage). Covers every byte Espryt hands the driver for
//                        a buffer object's contents.
//                        NOT covered: DirectVulkan's own buffer staging (its buffer bytes
//                        reach the GPU through a persistent map the frontend already owns,
//                        so there is no second copy to count) - see the note on
//                        persistent-map-push.
//   stage-texture        DirectGLES Managers.cpp texture upload: the bytes of whichever of
//                        the three upload shapes ran (rect list / union box / whole level).
//                        NOT covered: the DirectVulkan texture staging path, and Espryt's
//                        compressed-texture and readback paths.
//   stage-ubo-global     DirectGLES.cpp default-uniform-block image, both the UBO-ring
//                        memcpy and the glBufferSubData fallback.
//   stage-ubo-named      DirectVulkan UniformManager::ResolveUniformBufferPayload - the
//                        bytes Magma repacks into its own UBO ring. Espryt contributes
//                        nothing by construction (D-B8).
//   stage-vertex-client  DirectGLES BackendVertexArrayObject::SyncClientSideAttributesFor-
//                        DrawArrays, both the Float64-narrowing and the verbatim shapes.
//                        NOT covered: the DirectVulkan converted-vertex-stream cache.
//   stage-index-client   DirectGLES index rewriting (the primitive-restart substitution
//                        buffer).
//                        NOT covered: DirectVulkan's index staging.
//   persistent-map-push  Not wired in P0: today a persistent map is a permanent address
//                        space donation (D4/D-B4) that survives the whole monolith track,
//                        so there is no push to count until the IPC track breaks it.
//   residual-value-block Placeholder, always 0 until P2 (plan section 6.3).
//
// Call classes
//   draws                DirectGLES PrepareForDraw and DirectVulkan TrySetupDrawFastPath's
//                        caller-visible entry. A dispatch is not a draw and is not counted.
//   accessor-calls       STATIC TALLIES at the instrumented entry points, NOT a wrapper
//                        around all 293 pGLContext-> sites. Each instrumented function adds
//                        the number of GLContext accessor calls that its OWN body executed
//                        on the path taken. Covered: PrepareForDraw's own reads,
//                        SyncRenderState, CaptureDrawTextureSyncKeys/CurrentUnitBindings-
//                        Epoch, SyncNeccessaryTextures' walk, TrySetupDrawFastPath,
//                        GetOrCreatePipeline and ApplyDynamicDrawStateTail. NOT covered:
//                        the reads inside the callees those functions invoke (buffer/VAO/
//                        FBO/program sync, the pipeline payload builder's ~40 reads on a
//                        memo miss), and every non-draw entry point. The number is
//                        therefore a LOWER BOUND on the per-draw accessor count, and it is
//                        the bound over exactly the six gates section 2.3.1 tabulates.
//   texture-*            DirectGLES texture upload, per (target, level) emission.
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

        const char* const kByteClassNames[kByteClassCount] = {
            "stage-buffer",       "stage-texture",      "stage-ubo-global",    "stage-ubo-named",
            "stage-vertex-client", "stage-index-client", "persistent-map-push", "residual-value-block",
        };
        const char* const kCallClassNames[kCallClassCount] = {
            "draws", "accessor-calls", "tex-upload-emissions", "tex-upload-box", "tex-upload-rect",
            "tex-upload-jobs",
        };
        const char* const kGateNames[kGateCount] = {
            "espryt-render-state", "espryt-texture-sync-list", "espryt-unit-bindings-epoch",
            "magma-draw-fastpath", "magma-pipeline-memo",      "magma-dynamic-tail",
        };
        // Short forms, so the per-120-frame line stays one terminal line wide.
        const char* const kByteClassShort[kByteClassCount] = {"buf",  "tex",  "ubog", "ubon",
                                                              "vtxc", "idxc", "pmap", "resid"};
        const char* const kGateShort[kGateCount] = {"ers", "etl", "eub", "mfp", "mpm", "mdt"};

        void EmitSummaryLine() {
            const String line = FormatSummaryLine();
            // MGLOG_I on purpose, against the project's usual "MGLOG_D for anything
            // non-critical" rule: the line has to survive an INFO build (that is the only
            // build a device ever runs), it is emitted at most once per 120 frames, and it
            // exists at all only when the operator set MOBILEGL_PIPE_STATS=1. It is an
            // opt-in measurement channel, not per-frame noise.
            MGLOG_I("%s", line.c_str());
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
        ResetForTesting();
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
#ifdef TRACY_ENABLE
        // One plot per counter, the frame's value. Tracy keeps the series by name, and the
        // names are the static literals above, which is what TracyPlot requires.
        for (Uint32 i = 0; i < kByteClassCount; ++i) {
            TracyPlot(kByteClassNames[i], static_cast<Int64>(Read(g_frameBytes[i])));
        }
        for (Uint32 i = 0; i < kCallClassCount; ++i) {
            TracyPlot(kCallClassNames[i], static_cast<Int64>(Read(g_frameCalls[i])));
        }
        for (Uint32 i = 0; i < kGateCount; ++i) {
            TracyPlot(kGateNames[i], static_cast<Int64>(Read(g_frameGateMiss[i])));
        }
#endif
        for (Uint32 i = 0; i < kByteClassCount; ++i) {
            g_frameBytes[i].store(0, std::memory_order_relaxed);
        }
        for (Uint32 i = 0; i < kCallClassCount; ++i) {
            g_frameCalls[i].store(0, std::memory_order_relaxed);
        }
        for (Uint32 i = 0; i < kGateCount; ++i) {
            g_frameGateHit[i].store(0, std::memory_order_relaxed);
            g_frameGateMiss[i].store(0, std::memory_order_relaxed);
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

    String FormatSummaryLine() {
        // Window values: everything since the previous summary. A run total over a workload
        // whose shape changes (load, then steady state) hides exactly the number P2 wants.
        const Uint64 frames = Read(g_frameCount);
        const Uint64 windowFrames = frames - g_windowBaseFrames;
        const Uint64 divisorFrames = windowFrames == 0 ? 1 : windowFrames;

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
        line += " draws/f=" + std::to_string(draws / divisorFrames);
        line += " acc=" + std::to_string(accessorCalls);
        // Two decimals without <iomanip>: the per-draw accessor count is the number section
        // 2.3.1 wants to an integer's worth of precision, and it is small (10-25).
        const Uint64 accPerDrawHundredths = draws == 0 ? 0 : (accessorCalls * 100 + draws / 2) / draws;
        line += " acc/draw=" + std::to_string(accPerDrawHundredths / 100) + "." +
                (accPerDrawHundredths % 100 < 10 ? "0" : "") + std::to_string(accPerDrawHundredths % 100);
        line += " bytes/f[";
        for (Uint32 i = 0; i < kByteClassCount; ++i) {
            if (i != 0) {
                line += " ";
            }
            line += kByteClassShort[i];
            line += "=";
            line += std::to_string(bytes[i] / divisorFrames);
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
        g_windowBaseFrames = frames;
        return line;
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

    void ResetForTesting() {
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

} // namespace MobileGL::MG_Util::PipeStats
