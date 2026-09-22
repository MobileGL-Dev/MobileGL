// MobileGL - MobileGL/MG_Remote/FatalFunnel.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <MG_Remote/FatalFunnel.h>

#include "../Includes.h" // MGLOG_F and the umbrella; a .cpp may pull it, a Transport/ header may not

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace MobileGL::MG_Remote {

    namespace {
        // One telemetry point (5.2). Bumped exactly once per death, before the abort, so a test
        // that forks a child and reads it back sees the increment even though the child is gone -
        // it reads its OWN process's counter for the funnel it drove, not the child's.
        std::atomic<std::uint64_t> g_sessionFaultCount{0};
    } // namespace

    std::uint64_t SessionFaultCount() {
        return g_sessionFaultCount.load(std::memory_order_relaxed);
    }

    void SessionFail(MGFatalFamily family, const char* fmt, ...) {
        // The line, formatted exactly as the site's old MGLOG_F would have. Same buffer size and
        // same fallback as WireLogFatal, so a death that used either path reads identically.
        char line[512];
        va_list args;
        va_start(args, fmt);
        const int written = std::vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        if (written < 0) {
            std::snprintf(line, sizeof(line),
                          "MG_Remote: unformattable Fatal diagnostic (format=%s)", fmt);
        }

        g_sessionFaultCount.fetch_add(1, std::memory_order_relaxed);

        MGLOG_F("%s", line);
        // The stderr echo is the half a death test can see: Defines.h builds the logger with the
        // console sink off, so an MGLOG line reaches only the file - and a gtest death matcher
        // reads the child's stderr. Flushed because abort() does not flush stdio.
        std::fputs(line, stderr);
        std::fputc('\n', stderr);
        std::fflush(stderr);

        // `dl` step three publishes a SessionFault{family, detail, seq, op} frame to the peer
        // here, so the guest's log names which record ended the session instead of reading a bare
        // EOF. The family's wire projection is FatalCodeForFamily(family); until the frame lands
        // the family is still carried in the line above and counted here.
        (void)family;

        std::abort();
    }

} // namespace MobileGL::MG_Remote
