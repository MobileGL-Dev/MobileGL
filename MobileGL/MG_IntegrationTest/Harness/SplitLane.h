// MobileGL - MobileGL/MG_IntegrationTest/Harness/SplitLane.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The harness markers the `DirectGLES.Split.` ctest entries set.
//
// WHAT IS AND IS NOT DECIDED HERE. These markers say what the LANE asked for. Whether the lane
// GOT it is a different question and it is answered by Harness/SplitRuntimePeek.h, out of the
// running process - see the long argument in that header. The split of responsibility matters:
// an environment variable is a request, and this package's first version treated a request (plus
// a grep over source text) as evidence that the request had been honoured. Review finding M-1
// falsified that by renaming one string in six files, which armed eleven lanes and turned eight
// of them green against the monolith path.
//
// MGITEST_SPLIT_LANE=1
//     Set by the DirectGLES.Split.* entries and by nothing else. It is how a case in ONE binary,
//     registered many times over, knows which registration it is running under. It is NOT
//     evidence of anything about the transport.
//
// MGITEST_PERSISTENT_MAP_ARM=adopted|emulated
//     The arm the LANE declares. AcquireMemoryRange adopts a PERSISTENT|WRITE map that is not
//     FLUSH_EXPLICIT whenever the resource owner mints one (BufferObject.cpp:645-661), and
//     declines to the shadow otherwise - two completely different code paths, chosen by the
//     driver and the build rather than by the test, and MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION
//     does NOT separate them (it guards TryAdoptLargeStorage's 16 MiB path, which a
//     scenario-sized buffer never reaches at all). So the lane states which arm it expects and
//     PersistentCoherentMapScenario asserts it landed there, through
//     Harness/PersistentMapPeek.h's read of IsBackendPersistentMapped(). R-6 pins the split lane
//     at T2 = declined = emulated.
//
// MGITEST_PMAP_LANE=1
//     The one counting entry per transport that reads the library's summary line back. It has a
//     MOBILEGL_LOG_FILE_PATH of its own and a RESOURCE_LOCK on it.
//
// MGITEST_SMALL_RING_LANE=1
//     Exit gate E3(e)'s lane: the same split scenarios with MOBILEGL_IPC_RING_MB and
//     MOBILEGL_IPC_STAGE_MB at their floor, so that the ring is small enough to make at least one
//     back-pressure wait happen. A case uses it only to say so in its recorded properties; the
//     ring sizes themselves reach the library through MOBILEGL_IPC_*.

#pragma once

#include <cstdlib>
#include <string>

#include "SplitRuntimePeek.h"

namespace MGITest::SplitLane {

    inline std::string MarkerValue(const char* name) {
        const char* value = std::getenv(name);
        return (value != nullptr) ? std::string(value) : std::string();
    }

    inline bool MarkerIsOne(const char* name) { return MarkerValue(name) == "1"; }

    // True in the DirectGLES.Split.* entries only.
    inline bool IsSplitLane() { return MarkerIsOne("MGITEST_SPLIT_LANE"); }

    // True in exit gate E3(e)'s small-ring lane.
    inline bool IsSmallRingLane() { return MarkerIsOne("MGITEST_SMALL_RING_LANE"); }

    // Empty when this case may assert; otherwise the reason to GTEST_SKIP() with. The reason is
    // spelled out rather than summarised because a skip line is the only thing anyone reads when
    // they ask "did the split lane actually run" - and because the previous version of this
    // message named the wrong missing thing (review finding N-1): it said MG_Remote/Client did
    // not exist, on a tree where it existed and compiled and every entry point aborted.
    inline std::string SkipReasonForSplitOnlyAssertions() {
        if (!IsSplitLane()) {
            return "not the split lane (MGITEST_SPLIT_LANE is unset): this case's split-only "
                   "assertions are about a live MG_Remote client session and say nothing in a "
                   "monolith process";
        }
        return SplitRuntimeSkipReason();
    }

    // "adopted", "emulated", or empty when the lane declared nothing.
    inline std::string DeclaredPersistentMapArm() { return MarkerValue("MGITEST_PERSISTENT_MAP_ARM"); }

} // namespace MGITest::SplitLane
