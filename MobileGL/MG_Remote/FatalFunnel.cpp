// MobileGL - MobileGL/MG_Remote/FatalFunnel.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <MG_Remote/FatalFunnel.h>

#include "../Includes.h" // MGLOG_F and the umbrella; a .cpp may pull it, a Transport/ header may not
#include <MG_Pipe/PipeSessionFail.h> // the seam MG_Backend's renderer dies through
#include "Client/ClientSession.h"
#include "Protocol/generated/protocol_generated.h"
#include "Server/ServerSession.h"
#include "Transport/ITransport.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <unordered_map>

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

    namespace {
        // THE PROJECTION FROM MG_Pipe's TWO-WORD BOUNDARY ENUM ONTO THE .def's VOCABULARY, as a
        // switch with a named default rather than a cast. MGPipeFatalFamily is deliberately not
        // MGFatalFamily's ordinals (PipeSessionFail.h says why), so this arm is the only place
        // the two vocabularies meet - and adding a third word there means arguing for a row here.
        //
        // The line is forwarded through "%s" rather than re-used as a format: it has already been
        // vsnprintf'd once, and a detail string that happened to contain a `%` would otherwise be
        // read as a conversion the second time round.
        [[noreturn]] void PipeSessionFailAdapter(MG_Pipe::MGPipeFatalFamily family,
                                                 const char* line) {
            MGFatalFamily mapped = MGFatalFamily::ProtocolCorruption;
            switch (family) {
            case MG_Pipe::MGPipeFatalFamily::UnmigratedVerb:
                mapped = MGFatalFamily::UnmigratedVerb;
                break;
            case MG_Pipe::MGPipeFatalFamily::RoleViolation:
                mapped = MGFatalFamily::RoleViolation;
                break;
#if MOBILEGL_BUILD_DISAGGREGATED
            case MG_Pipe::MGPipeFatalFamily::ProtocolCorruption:
                mapped = MGFatalFamily::ProtocolCorruption;
                break;
#endif
            }
            SessionFail(mapped, "%s", line);
        }
    } // namespace

    void InstallPipeSessionFailHook() {
        MG_Pipe::MGPipeInstallSessionFailHook(&PipeSessionFailAdapter);
    }

    namespace {
        // The line, formatted exactly as the site's old MGLOG_F would have. Same buffer size and
        // same fallback as WireLogFatal, so a death that used either path reads identically.
        void FormatFaultLine(char (&line)[512], const char* fmt, va_list args) {
            const int written = std::vsnprintf(line, sizeof(line), fmt, args);
            if (written < 0) {
                std::snprintf(line, sizeof(line),
                              "MG_Remote: unformattable Fatal diagnostic (format=%s)", fmt);
            }
        }

        // The log line and its stderr echo: Defines.h builds the logger with the console sink
        // off, so an MGLOG line reaches only the file - and a gtest death matcher reads the
        // child's stderr. Flushed because abort() does not flush stdio.
        void LogFaultLine(const char* line) {
            MGLOG_F("%s", line);
            std::fputs(line, stderr);
            std::fputc('\n', stderr);
            std::fflush(stderr);
        }

        // The SessionFault frame to whichever peer this process has; defined below SessionFail,
        // whose body it was until PH-1 (3) gave the latch a second caller.
        void PublishSessionFault(MGFatalFamily family, const char* line);

        // PH-1 (3): the latch's state. Armed once, by the session child; latched once, by the
        // first fault. The line is written under the mutex BEFORE the release store that
        // publishes `g_latched`, so a reader that saw true reads a whole line.
        //
        // P14 S2: ONE OF THESE PER SESSION (FatalFunnel.h's domain block). A domain's storage is
        // heap-allocated by FatalLatchDomainOpen and lives until FatalLatchDomainClose; domain 0
        // is this process-wide one, which is what every thread that binds nothing uses.
        struct LatchState {
            int id = 0;
            std::atomic<bool> armed{false};
            std::atomic<bool> latched{false};
            std::atomic<std::uint64_t> count{0};
            std::mutex mutex;
            MGFatalFamily latchedFamily = MGFatalFamily::ProtocolCorruption;
            char latchedLine[512] = {};
        };

        LatchState g_defaultLatch;

        thread_local LatchState* t_latch = &g_defaultLatch;

        std::mutex g_domainsMutex;
        std::unordered_map<int, LatchState*> g_domains;
        int g_nextDomain = 1;

        LatchState& Latch() { return *t_latch; }
    } // namespace

    int FatalLatchDomainOpen() {
        const std::lock_guard<std::mutex> lock(g_domainsMutex);
        if (g_nextDomain <= 0 || g_nextDomain > kMaxLatchDomains) return 0;
        LatchState* state = new LatchState{};
        state->id = g_nextDomain++;
        g_domains.emplace(state->id, state);
        return state->id;
    }

    void FatalLatchDomainClose(int domain) {
        if (domain == 0) return;
        LatchState* state = nullptr;
        {
            const std::lock_guard<std::mutex> lock(g_domainsMutex);
            const auto found = g_domains.find(domain);
            if (found == g_domains.end()) return;
            state = found->second;
            g_domains.erase(found);
        }
        delete state;
    }

    int FatalLatchDomainBind(int domain) {
        LatchState* const previous = t_latch;
        if (domain == 0) {
            t_latch = &g_defaultLatch;
            return previous->id;
        }
        {
            const std::lock_guard<std::mutex> lock(g_domainsMutex);
            const auto found = g_domains.find(domain);
            t_latch = found == g_domains.end() ? &g_defaultLatch : found->second;
        }
        return previous->id;
    }

    void ArmSessionLatch() { Latch().armed.store(true, std::memory_order_release); }

    void ResetSessionLatch() {
        LatchState& latch = Latch();
        const std::lock_guard<std::mutex> lock(latch.mutex);
        latch.armed.store(false, std::memory_order_release);
        latch.count.store(0, std::memory_order_relaxed);
        latch.latchedFamily = MGFatalFamily::ProtocolCorruption;
        latch.latchedLine[0] = '\0';
        latch.latched.store(false, std::memory_order_release);
    }

    bool SessionLatchArmed() { return Latch().armed.load(std::memory_order_acquire); }

    bool SessionLatched() { return Latch().latched.load(std::memory_order_acquire); }

    MGFatalFamily SessionLatchedFamily() {
        LatchState& latch = Latch();
        const std::lock_guard<std::mutex> lock(latch.mutex);
        return latch.latchedFamily;
    }

    const char* SessionLatchedLine() { return Latch().latchedLine; }

    std::uint64_t SessionLatchCount() { return Latch().count.load(std::memory_order_relaxed); }

    bool SessionLatch(MGFatalFamily family, const char* fmt, ...) {
        char line[512];
        va_list args;
        va_start(args, fmt);
        FormatFaultLine(line, fmt, args);
        va_end(args);

        LatchState& latch = Latch();

        // UNARMED IS SessionFail, byte for byte: the inproc server, the client and every unit
        // death test keep the death they had (FatalFunnel.h's block says why inproc must).
        if (!latch.armed.load(std::memory_order_acquire)) {
            SessionFail(family, "%s", line);
        }

        g_sessionFaultCount.fetch_add(1, std::memory_order_relaxed);
        latch.count.fetch_add(1, std::memory_order_relaxed);
        LogFaultLine(line);

        bool first = false;
        {
            const std::lock_guard<std::mutex> lock(latch.mutex);
            if (!latch.latched.load(std::memory_order_relaxed)) {
                std::snprintf(latch.latchedLine, sizeof(latch.latchedLine), "%s", line);
                latch.latchedFamily = family;
                latch.latched.store(true, std::memory_order_release);
                first = true;
            }
        }
        if (!first) {
            // A second fault after the first one latched: the session is already declining, so
            // this is logged and counted but it neither re-publishes nor replaces the cause.
            MGLOG_E("MG_Remote server: SessionLatch - a further named fault after the latched one "
                    "(%s); it is declined with the rest of the session",
                    FatalFamilyName(family));
            return false;
        }
        // ONE LINE THAT SAYS WHAT HAPPENS NEXT, so a log reader can tell a latched end from a
        // crash without the supervisor's reap line.
        MGLOG_E("MG_Remote server: SessionLatch{%s} - the first named fault of this session is "
                "latched; every further record and control op is declined and the session "
                "closes (exit %d) for the supervisor to serve the next connection (PH-1, "
                "ID-P7-1)",
                FatalFamilyName(family), kSessionLatchedExitCode);
        PublishSessionFault(family, line);
        return false;
    }

    void SessionFail(MGFatalFamily family, const char* fmt, ...) {
        char line[512];
        va_list args;
        va_start(args, fmt);
        FormatFaultLine(line, fmt, args);
        va_end(args);

        g_sessionFaultCount.fetch_add(1, std::memory_order_relaxed);

        LogFaultLine(line);
        PublishSessionFault(family, line);
        std::abort();
    }

    namespace {
    void PublishSessionFault(MGFatalFamily family, const char* line) {
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
    }
    } // namespace

} // namespace MobileGL::MG_Remote
