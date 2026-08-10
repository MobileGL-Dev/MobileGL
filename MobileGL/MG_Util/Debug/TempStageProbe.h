// MobileGL - MobileGL/MG_Util/Debug/TempStageProbe.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// TEMP-STAGE-PROBE: TEMPORARY instrumentation. Measures the per-stage wall time of the
// shader frontend (preprocess / glslang parse / glslang link+mapIO / SPIR-V generation /
// spirv-tools optimize / reflection / SPIRV-Cross global-UBO routing) so a shaderpack load
// can be attributed stage by stage. Delete this file together with every call site marked
// TEMP-STAGE-PROBE when the measurement is finished. Every identifier is spelled
// tempStageProbe* / TempStageProbe* / kTempStageProbe* so both spellings are greppable.

#pragma once

#include <MG_Util/Debug/Log.h>

#include <atomic>
#include <chrono>

namespace MobileGL::MG_Util::Debug {
    // TEMP-STAGE-PROBE: stage ids. The two *Total entries are SUPERSETS of the stages above
    // them (they time the whole task body), kept so the residue inside a task body is
    // visible; kTempStageProbeParseReparse is a SUBSET of kTempStageProbeParse (the link's
    // consume-once re-parse goes through the same ShaderCompiler::CompileShader).
    enum TempStageProbeStageId : int {
        kTempStageProbePreprocess = 0,
        kTempStageProbeParse,
        kTempStageProbeParseReparse,
        kTempStageProbeGlslangLink,
        kTempStageProbeSpirvGen,
        kTempStageProbeSpirvNull,
        kTempStageProbeSpirvOpt,
        kTempStageProbeReflection,
        kTempStageProbeSpvcRouting,
        kTempStageProbeCompileTaskTotal,
        kTempStageProbeLinkTaskTotal,
        kTempStageProbeSpirvTaskTotal,
        kTempStageProbeStageCount
    };

    inline const char* const kTempStageProbeStageNames[kTempStageProbeStageCount] = {
        "preprocess",
        "parse",
        "parse-reparse[subset-of-parse]",
        "glslang-link",
        "spirv-gen",
        "spirv-null[plumbing-only]",
        "spirv-opt",
        "reflection",
        "spvc-routing",
        "compiletask-total[superset]",
        "linktask-total[superset]",
        "spirvtask-total[superset]",
    };

    inline std::atomic<unsigned long long> tempStageProbeMicros[kTempStageProbeStageCount] = {};
    inline std::atomic<unsigned long long> tempStageProbeCalls[kTempStageProbeStageCount] = {};
    inline std::atomic<unsigned long long> tempStageProbeLinkCount{0};
    inline std::atomic<long long> tempStageProbeLastDumpNanos{0};

    // TEMP-STAGE-PROBE: scoped accumulator. Relaxed atomics only - nothing here orders any
    // other memory, and the probe must not perturb what it measures.
    class TempStageProbeScope {
    public:
        explicit TempStageProbeScope(int tempStageProbeStageId)
            : tempStageProbeStage(tempStageProbeStageId),
              tempStageProbeStart(std::chrono::steady_clock::now()) {}
        ~TempStageProbeScope() {
            const auto tempStageProbeElapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                                     std::chrono::steady_clock::now() - tempStageProbeStart)
                                                     .count();
            tempStageProbeMicros[tempStageProbeStage].fetch_add(
                static_cast<unsigned long long>(tempStageProbeElapsedUs), std::memory_order_relaxed);
            tempStageProbeCalls[tempStageProbeStage].fetch_add(1ull, std::memory_order_relaxed);
        }
        TempStageProbeScope(const TempStageProbeScope&) = delete;
        TempStageProbeScope& operator=(const TempStageProbeScope&) = delete;

    private:
        int tempStageProbeStage;
        std::chrono::steady_clock::time_point tempStageProbeStart;
    };

    // TEMP-STAGE-PROBE: one MGLOG_I line per stage carrying the running cumulative total.
    inline void TempStageProbeDumpTotals(unsigned long long tempStageProbeLinkIndex) {
        for (int tempStageProbeIdx = 0; tempStageProbeIdx < kTempStageProbeStageCount; ++tempStageProbeIdx) {
            const unsigned long long tempStageProbeUs =
                tempStageProbeMicros[tempStageProbeIdx].load(std::memory_order_relaxed);
            const unsigned long long tempStageProbeN =
                tempStageProbeCalls[tempStageProbeIdx].load(std::memory_order_relaxed);
            MGLOG_I("TEMP-STAGE-PROBE: %s total %llu.%03llu ms over %llu calls (after %llu links)",
                    kTempStageProbeStageNames[tempStageProbeIdx], tempStageProbeUs / 1000ull,
                    tempStageProbeUs % 1000ull, tempStageProbeN, tempStageProbeLinkIndex);
        }
    }

    // TEMP-STAGE-PROBE: ticks the program-link counter on scope exit (so a link that returns
    // early still counts) and dumps every stage's running total every 25 links. The 3-second
    // fallback exists so the LAST link of a load also publishes totals - the process is
    // force-stopped afterwards, so there is no exit hook to rely on.
    class TempStageProbeLinkTick {
    public:
        ~TempStageProbeLinkTick() {
            const unsigned long long tempStageProbeIndex =
                tempStageProbeLinkCount.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
            const long long tempStageProbeNowNanos =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            const long long tempStageProbeLastNanos = tempStageProbeLastDumpNanos.load(std::memory_order_relaxed);
            const bool tempStageProbeDueByCount = (tempStageProbeIndex % 25ull) == 0ull;
            const bool tempStageProbeDueByTime = tempStageProbeNowNanos - tempStageProbeLastNanos > 3000000000LL;
            if (!tempStageProbeDueByCount && !tempStageProbeDueByTime) return;
            tempStageProbeLastDumpNanos.store(tempStageProbeNowNanos, std::memory_order_relaxed);
            TempStageProbeDumpTotals(tempStageProbeIndex);
        }
    };
} // namespace MobileGL::MG_Util::Debug
