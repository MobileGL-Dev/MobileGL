// MobileGL - MobileGL/MG_IntegrationTest/Harness/SplitLane.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The four harness markers the `DirectGLES.Split.` ctest entries set, and the one question
// every Split case has to ask before it asserts anything.
//
// WHY A MARKER AND NOT MOBILEGL_TRANSPORT. The library's own variable says what was ASKED FOR,
// not what happened. In a build without -DMOBILEGL_BUILD_DISAGGREGATED the whole
// MOBILEGL_TRANSPORT parser does not exist (CONTRACT-P5.md 5: putting a complaint in the
// unconditional part of ConfigLoader would move a pull-build symbol and break G1), so
// MOBILEGL_TRANSPORT=inproc is accepted by the environment and SILENTLY IGNORED. A Split case
// that keyed off MOBILEGL_TRANSPORT would therefore run monolith and go green - which is
// precisely the failure the whole lane exists to make impossible. The build-level guard against
// that is `nm --defined-only libMobileGL.so | grep -i MG_Remote` in build-split, asserted by the
// gate's part 1 and by the build-linux-split CI job; the markers here are the TEST-level guard
// for the other half of the same question, "is there a client to assert about yet".
//
// MGITEST_SPLIT_LANE=1
//     Set by the DirectGLES.Split.* entries and by nothing else. A scenario's split-only
//     assertions are the ones that are meaningless in the ambient DirectGLES./DirectVulkan.
//     lanes, and this is how a case tells the two apart in ONE binary that is registered many
//     times over.
//
// MGITEST_REMOTE_CLIENT_PRESENT=1
//     Set by a CMake content probe over MobileGL/MG_Remote/Client (package c1's directory), for
//     a SYMBOL and never a filename - the reason is argued at length above
//     mgl_itest_probe_for_symbol in MG_IntegrationTest/CMakeLists.txt: the owning package picks
//     its own file layout, and a filename probe answers "no" forever the moment it moves the
//     code. Until c1 lands there is no emitter, so `MOBILEGL_TRANSPORT=inproc` reaches a library
//     that parses it, logs it and then runs monolith anyway. A Split entry in that state must
//     SKIP NAMING THE MISSING THING; it must not be deleted (gate G14 - a ctest name may never
//     disappear) and it must not go green.
//
// MGITEST_PERSISTENT_MAP_ARM=adopted|emulated
//     The arm the LANE declares. AcquireMemoryRange adopts a PERSISTENT|WRITE map that is not
//     FLUSH_EXPLICIT whenever the resource owner mints one (BufferObject.cpp:645-661), and
//     declines to the shadow otherwise - two completely different code paths, chosen by the
//     driver and the build rather than by the test, and MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION
//     does NOT separate them (it guards TryAdoptLargeStorage's 16 MiB path, which a
//     scenario-sized buffer never reaches at all). So the lane states which arm it expects and
//     the scenario asserts it landed there. R-6 pins the split lane at T2 = emulated.
//
// MGITEST_SPLIT_EXPECT_TRANSPORT=inproc|monolith
//     What the lane expects MG_Config::Transport to have resolved to, for the case that reads
//     the library's own log back. Only meaningful in a lane that gave itself a private
//     MOBILEGL_LOG_FILE_PATH.

#pragma once

#include <cstdlib>
#include <string>

namespace MGITest::SplitLane {

    inline std::string MarkerValue(const char* name) {
        const char* value = std::getenv(name);
        return (value != nullptr) ? std::string(value) : std::string();
    }

    inline bool MarkerIsOne(const char* name) { return MarkerValue(name) == "1"; }

    // True in the DirectGLES.Split.* entries only.
    inline bool IsSplitLane() { return MarkerIsOne("MGITEST_SPLIT_LANE"); }

    // True once any source under MG_Remote/Client names one of the symbols CONTRACT-P5 fixes
    // for it. See the probe in CMakeLists.txt.
    inline bool RemoteClientPresent() { return MarkerIsOne("MGITEST_REMOTE_CLIENT_PRESENT"); }

    // Empty when this case may assert; otherwise the reason to GTEST_SKIP() with. The reason
    // is spelled out rather than summarised because a skip line is the only thing anyone reads
    // when they ask "did the split lane actually run".
    inline std::string SkipReasonForSplitOnlyAssertions() {
        if (!IsSplitLane()) {
            return "not the split lane (MGITEST_SPLIT_LANE is unset): this case's split-only "
                   "assertions are about MOBILEGL_TRANSPORT=inproc and say nothing in a monolith "
                   "process";
        }
        if (!RemoteClientPresent()) {
            return "MobileGL/MG_Remote/Client does not exist yet - no source there names "
                   "kRemoteEmitSlotCount / BackendObject_Remote / CapsMirror, so package c1 has "
                   "not landed and MOBILEGL_TRANSPORT=inproc reaches a library that parses it and "
                   "then runs monolith. Passing here would be a green that means 'the thing I test "
                   "does not exist yet'; the entry stays registered (G14) and skips instead";
        }
        return {};
    }

    // "adopted", "emulated", or empty when the lane declared nothing.
    inline std::string DeclaredPersistentMapArm() { return MarkerValue("MGITEST_PERSISTENT_MAP_ARM"); }

    // "inproc", "monolith", or empty when the lane declared nothing.
    inline std::string DeclaredTransport() { return MarkerValue("MGITEST_SPLIT_EXPECT_TRANSPORT"); }

} // namespace MGITest::SplitLane
