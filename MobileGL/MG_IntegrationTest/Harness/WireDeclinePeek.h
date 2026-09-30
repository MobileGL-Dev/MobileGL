// MobileGL - MobileGL/MG_IntegrationTest/Harness/WireDeclinePeek.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// P8-SV: one row of the Magma wire arm's decline tally (WireDeclines.def, the counts behind the
// server's MGWIRE-DECLINES lines), read as a run total out of THIS process - WireIndirectPeek.h's
// shape. Process-wide means the SERVER's counts only where the server is this process: the inproc
// arm, never spawn or tcp, and never the monolith arm, which takes no wire path. False where the
// reading cannot be taken: a pull build, Android, or a name that is not a row.
#pragma once

namespace MGITest {

    bool PeekWireDeclineCount(const char* row, unsigned long long* out);

} // namespace MGITest
