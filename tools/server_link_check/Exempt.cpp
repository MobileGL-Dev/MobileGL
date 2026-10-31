// MobileGL - tools/server_link_check/Exempt.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// P13 W7 (D12): the one definition every named exemption of the server-only link check resolves to
// (scripts/ci/server_link_check.py writes `--defsym=<symbol>=mgl_server_link_check_exempt` for each
// frontend reference the link ratchet's baseline names). The check target is a link proof and is
// never loaded; reaching this would mean a server image called into the frontend half.
#include <cstdio>
#include <cstdlib>

extern "C" [[noreturn]] void mgl_server_link_check_exempt() {
    std::fputs("MobileGL server link check: an exempted frontend symbol was called\n", stderr);
    std::abort();
}
