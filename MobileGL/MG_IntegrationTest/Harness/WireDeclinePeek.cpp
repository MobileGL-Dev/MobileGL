// MobileGL - MobileGL/MG_IntegrationTest/Harness/WireDeclinePeek.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "WireDeclinePeek.h"

#if !defined(__ANDROID__)
#include <Config.h>
#if MOBILEGL_BUILD_DISAGGREGATED
#include <MG_Backend/DirectVulkan/Renderer/WireDeclineTally.h>
#include <cstring>
#define MGITEST_WIRE_DECLINE_PEEK_LIVE 1
#endif
#endif

namespace MGITest {

#if defined(MGITEST_WIRE_DECLINE_PEEK_LIVE)
    bool PeekWireDeclineCount(const char* row, unsigned long long* out) {
        if (row == nullptr || out == nullptr) return false;
        namespace Magma = MobileGL::MG_Backend::DirectVulkan;
        for (MobileGL::SizeT i = 0; i < Magma::WireDeclineTally::kSiteCount; ++i) {
            const auto site = static_cast<Magma::WireDeclineSite>(i);
            if (std::strcmp(Magma::WireDeclineSiteName(site), row) != 0) continue;
            *out = Magma::WireDeclineTally::Get(site);
            return true;
        }
        return false;
    }
#else
    bool PeekWireDeclineCount(const char*, unsigned long long*) { return false; }
#endif

} // namespace MGITest
