// MobileGL - MobileGL/MG_Remote/FatalFunnel.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <MG_Remote/FatalFunnel.h>

#include "../Includes.h" // MGLOG_F and the umbrella; a .cpp may pull it, a Transport/ header may not
#include "Client/ClientSession.h"
#include "Protocol/generated/protocol_generated.h"
#include "Server/ServerSession.h"
#include "Transport/ITransport.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

#include <flatbuffers/flatbuffers.h>

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

        // A SessionFault frame to the peer, BEFORE the abort, so the other side's log names the
        // family instead of reading a bare EOF (5.2). Best-effort by nature: this thread is about
        // to die, so a send that blocks or fails changes nothing it could have changed anyway.
        //
        // WHICHEVER ROLE THIS PROCESS IS. Under spawn only one session is active per process -
        // the client has ClientSession, the server has ServerSession - so publishing on whatever
        // control plane exists reaches the far side. Under inproc both exist and the frame goes
        // to an in-process transport the aborting process will never read; harmless, and not
        // worth a special case that would then be the untested one.
        //
        // `code` is the coarse seven-value projection; `family` carries the full word; `message`
        // is the same line just logged. seq/op are not threaded through the 90 call sites and the
        // message already names the op where it matters, so the frame carries what it can name
        // honestly rather than a field that is always zero.
        Transport::ITransport* control = nullptr;
        if (Client::ClientSession* client = Client::ClientSession::Active()) {
            control = client->Control_Plane();
        }
        if (control == nullptr) {
            if (Server::ServerSession* server = Server::ServerSession::Active()) {
                control = server->Control_Plane();
            }
        }
        if (control != nullptr) {
            ::flatbuffers::FlatBufferBuilder builder(256);
            const auto fatal = ::MobileGL::Wire::CreateFatalDirect(
                builder, FatalCodeForFamily(family), line, FatalFamilyName(family));
            const auto root = ::MobileGL::Wire::CreateCtrlEnvelope(
                builder, ::MobileGL::Wire::CtrlMsg::Fatal, fatal.Union());
            ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, root);
            (void)control->SendFrame(
                MobileGLByteSpan{builder.GetBufferPointer(), builder.GetSize()});
        }

        std::abort();
    }

} // namespace MobileGL::MG_Remote
