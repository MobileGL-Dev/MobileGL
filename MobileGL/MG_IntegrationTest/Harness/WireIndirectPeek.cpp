// MobileGL - MobileGL/MG_IntegrationTest/Harness/WireIndirectPeek.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "WireIndirectPeek.h"

#if !defined(__ANDROID__)
#include <MG_Pipe/MGPipe.h>
#include <MG_Util/Metrics/PipeStats.h>
#define MGITEST_WIRE_INDIRECT_PEEK_LIVE 1
#endif

namespace MGITest {

#if defined(MGITEST_WIRE_INDIRECT_PEEK_LIVE)
    bool PeekWireIndirectCounters(WireIndirectCounters* out) {
        if (out == nullptr) return false;
        namespace Stats = MobileGL::MG_Util::PipeStats;
        if (!Stats::Enabled()) Stats::SetEnabledForTesting(true);
        out->nativeDraws = Stats::TotalCalls(Stats::CallClass::WireIndirectNativeDraws);
        out->cpuExpansions = Stats::TotalCalls(Stats::CallClass::WireIndirectCpuExpansions);
        out->barriers = Stats::TotalCalls(Stats::CallClass::WireIndirectBarriers);
        out->hostWaits = Stats::TotalCalls(Stats::CallClass::WireHostWaits);
        out->indirectWaits = Stats::TotalCalls(Stats::CallClass::WireHostWaitsIndirect);
        out->nativeDispatches = Stats::TotalCalls(Stats::CallClass::WireIndirectNativeDispatches);
        out->resourceReadbacks = Stats::TotalCalls(Stats::CallClass::ResourceReadbacks);
        return true;
    }
#else
    bool PeekWireIndirectCounters(WireIndirectCounters*) { return false; }
#endif

} // namespace MGITest
