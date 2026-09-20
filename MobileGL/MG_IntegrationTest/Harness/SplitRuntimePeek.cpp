// MobileGL - MobileGL/MG_IntegrationTest/Harness/SplitRuntimePeek.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "SplitRuntimePeek.h"

// MGITEST_SPLIT_RUNTIME_PEEK is defined by MG_IntegrationTest/CMakeLists.txt under
// MOBILEGL_BUILD_DISAGGREGATED and nowhere else. NO SOURCE PROBE decides it: the three symbols
// below are c0's, they exist in every disaggregated build from the contract commit onward, and
// their VALUES are what answer the question. That is the whole of the fix for review findings
// M-1, M-2 and M-3 - there is no longer a string for anyone to rename, comment out, or land
// outside a probed directory.
#if defined(MGITEST_SPLIT_RUNTIME_PEEK) && !defined(__ANDROID__)
#include <Config.h>

#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/Client/EmitTables.h>
#include <MG_Remote/Server/ServerLoop.h>
#include <chrono>
#include <atomic>
#include <thread>
#define MGITEST_SPLIT_RUNTIME_PEEK_LIVE 1
#endif

namespace MGITest {

#if defined(MGITEST_SPLIT_RUNTIME_PEEK_LIVE)
    namespace {
        std::atomic<bool> g_holdArmed{false}, g_holdActive{false}, g_holdRelease{true};
        void HoldOneAppliedBatch() {
            if (!g_holdArmed.exchange(false, std::memory_order_acq_rel)) return;
            g_holdActive.store(true, std::memory_order_release);
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!g_holdRelease.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            g_holdActive.store(false, std::memory_order_release);
        }
    }
#endif
    bool ArmSplitApplyHoldForTesting() {
#if defined(MGITEST_SPLIT_RUNTIME_PEEK_LIVE)
        g_holdRelease.store(false, std::memory_order_release);
        g_holdArmed.store(true, std::memory_order_release);
        MobileGL::MG_Remote::Server::ServerLoopInstance().SetBeforeRetireHookForTesting(&HoldOneAppliedBatch);
        return true;
#else
        return false;
#endif
    }
    bool WaitForSplitApplyHoldForTesting(unsigned int timeoutMs) {
#if defined(MGITEST_SPLIT_RUNTIME_PEEK_LIVE)
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < until) {
            if (g_holdActive.load(std::memory_order_acquire)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
#else
        (void)timeoutMs;
#endif
        return false;
    }
    bool SplitApplyHoldIsActiveForTesting() {
#if defined(MGITEST_SPLIT_RUNTIME_PEEK_LIVE)
        return g_holdActive.load(std::memory_order_acquire);
#else
        return false;
#endif
    }
    void ReleaseSplitApplyHoldForTesting() {
#if defined(MGITEST_SPLIT_RUNTIME_PEEK_LIVE)
        g_holdRelease.store(true, std::memory_order_release);
        g_holdArmed.store(false, std::memory_order_release);
        MobileGL::MG_Remote::Server::ServerLoopInstance().SetBeforeRetireHookForTesting(nullptr);
        while (g_holdActive.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
    }
    bool SplitProducerIsParkedForTesting() {
#if defined(MGITEST_SPLIT_RUNTIME_PEEK_LIVE)
        // This is the only reader intended for the observer thread. Do not call
        // PeekSplitRuntime there: its encoder and client-ledger fields are not atomic.
        auto* session = MobileGL::MG_Remote::Client::ClientSession::Active();
        return session && session->Control() && session->Control()->producerParked.load(std::memory_order_acquire);
#else
        return false;
#endif
    }
    void DelaySplitRetirementForTesting(bool enabled) {
#if defined(MGITEST_SPLIT_RUNTIME_PEEK_LIVE)
        MobileGL::MG_Remote::Server::ServerLoopInstance().SetBeforeRetireHookForTesting(
            enabled ? +[] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); } : nullptr);
#else
        (void)enabled;
#endif
    }

    SplitRuntimeState PeekSplitRuntime() {
        SplitRuntimeState state;
#if defined(MGITEST_SPLIT_RUNTIME_PEEK_LIVE)
        using MobileGL::MG_Config::TransportMode;
        state.peekAvailable = true;
        state.totalVerbSlots = MobileGL::MG_Remote::Client::kRemoteEmitSlotCount;
        switch (MobileGL::MG_Config::Transport) {
            case TransportMode::Monolith: state.transportName = "monolith"; break;
            case TransportMode::InProcess: state.transportName = "inproc"; break;
            default: state.transportName = "non-monolith"; break;
        }
        state.transportResolved = MobileGL::MG_Config::Transport != TransportMode::Monolith;

        // Active() is c0's one deliberately non-aborting accessor: "does a session exist" has a
        // legitimate no. Everything below it is only reached through a live session, so nothing
        // here can trip one of c0's Fatal stubs.
        MobileGL::MG_Remote::Client::ClientSession* session =
            MobileGL::MG_Remote::Client::ClientSession::Active();
        state.sessionActive = session != nullptr;
        state.implementedVerbs = MobileGL::MG_Remote::Client::ImplementedVerbCount();
        if (session != nullptr) {
            // Encoder() returns the member; EmitSeq() returns m_emitSeq. Neither is a stub, and
            // neither emits anything - this is a read.
            const MobileGL::MG_Remote::Wire::PipeWireEncoder& encoder = session->Encoder();
            state.emitSeq = encoder.EmitSeq();
            state.runAheadArmed = session->RunAheadArmed();
            state.presentCredit = MobileGL::MG_Config::Ipc.PresentCredit;
            state.presentCreditWaits = session->PresentCreditWaits();
            if (const auto* control = session->Control()) {
                state.appliedSeq = control->appliedSeq.load(std::memory_order_acquire);
                state.retiredSeq = control->retiredSeq.load(std::memory_order_acquire);
                state.presentAckSerial = control->presentAckSerial.load(std::memory_order_acquire);
            }

            // The producer's ledger. Every one of these is a plain member read on the encoder
            // or on the RingProducer it holds; none of them emits, publishes or waits, so a
            // case may read them between two GL calls without changing what the next record is.
            state.maxRecordBytes = encoder.MaxRecordBytesSeen();
            state.maxRecordBytesCap = encoder.MaxRecordBytesCap();
            state.cmdWraps = encoder.CmdWraps();
            state.cmdWrapPads = encoder.CmdWrapPads();
            state.cmdBytesWritten = encoder.CmdBytesWritten();
            state.stageReclaimWaits = encoder.StageReclaimWaits();
        }
#endif
        return state;
    }

    std::string SplitRuntimeSkipReason() {
        const SplitRuntimeState state = PeekSplitRuntime();
        if (!state.peekAvailable) {
            return "this build did not compile MG_Remote (no -DMOBILEGL_BUILD_DISAGGREGATED=ON), or "
                   "this is the Android binary, which links the shipping libMobileGL.so built "
                   "-fvisibility=hidden and can reach no internal symbol. MOBILEGL_TRANSPORT is "
                   "ACCEPTED AND SILENTLY IGNORED in such a build (CONTRACT-P5 5), so a green here "
                   "would be a monolith run under a name that says split";
        }
        if (!state.transportResolved) {
            return "MG_Config::Transport resolved to '" + state.transportName +
                   "', not to a split transport. The variable is read from the process, not from a "
                   "log line - a DEBUG-level pull build prints the same KEY=VALUE string out of "
                   "ConfigLoader's env dump (review M-5). Check MOBILEGL_TRANSPORT reached this "
                   "process";
        }
        if (!state.sessionActive) {
            return "MG_Remote::Client::ClientSession::Active() is null: no client session exists in "
                   "this process. c0 shipped Start() as a Fatal stub and Active() as a deliberate "
                   "null, so this is what 'packages s1 (construction and handshake) and c1 have not "
                   "landed' looks like from inside a running test. The entry stays registered (gate "
                   "G14) and skips rather than passing against the monolith path";
        }
        if (state.implementedVerbs == 0) {
            return "MG_Remote::Client::ImplementedVerbCount() is 0 of " +
                   std::to_string(state.totalVerbSlots) +
                   ": the emit table has no real emitter, so every verb this scenario issues would "
                   "take the Fatal{UnmigratedVerb} arm or fall through. Package c1 owns it";
        }
        return {};
    }

} // namespace MGITest
