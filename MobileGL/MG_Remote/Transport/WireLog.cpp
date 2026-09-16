// MobileGL - MobileGL/MG_Remote/Transport/WireLog.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "WireLog.h"

#include <MG_Util/Debug/Log.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace MobileGL::MG_Remote::Transport {

    void WireLogError(const char* format, ...) {
        // One stack line, no allocation: this runs on paths that have just
        // decided the connection is unusable.
        char line[512];
        va_list args;
        va_start(args, format);
        const int written = std::vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        if (written < 0) {
            MGLOG_E("MG_Remote wire: unformattable diagnostic (format=%s)", format);
            return;
        }
        MGLOG_E("%s", line);
    }

    void WireLogFatal(const char* format, ...) {
        char line[512];
        va_list args;
        va_start(args, format);
        const int written = std::vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        if (written < 0) {
            std::snprintf(line, sizeof(line),
                          "MG_Remote wire: unformattable Fatal diagnostic (format=%s)", format);
        }
        MGLOG_F("%s", line);
        // The stderr echo is the half a death test can see (WireLog.h). Unbuffered
        // by default and flushed anyway: abort() does not flush stdio.
        std::fputs(line, stderr);
        std::fputc('\n', stderr);
        std::fflush(stderr);
        std::abort();
    }

} // namespace MobileGL::MG_Remote::Transport
