// MobileGL - MobileGL/MG_IntegrationTest/Harness/WireIndirectPeek.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// P8-D: the Magma wire arm's indirect-draw counters (PipeStats `windr[...]`), read as run totals
// out of THIS process - P4aFinalFixPeek.cpp's shape. Process-wide means the SERVER's counts only
// where the server is this process: monolith and inproc, never spawn or tcp. The first call arms
// the counters (a case reads before and after its workload, so it must peek before it draws).
// False where the reading cannot be taken: a pull build, Android.
#pragma once

namespace MGITest {

    struct WireIndirectCounters {
        unsigned long long nativeDraws = 0;    // wire-indirect-native-draws (`wind`)
        unsigned long long cpuExpansions = 0;  // wire-indirect-cpu-expansions (`wixp`)
        unsigned long long barriers = 0;       // wire-indirect-barriers (`wibar`)
        unsigned long long hostWaits = 0;      // wire-host-waits (`whw`)
        unsigned long long indirectWaits = 0;  // wire-host-waits-indirect (`whwi`)
    };

    bool PeekWireIndirectCounters(WireIndirectCounters* out);

} // namespace MGITest
