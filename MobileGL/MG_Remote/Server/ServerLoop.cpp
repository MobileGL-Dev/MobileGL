// MobileGL - MobileGL/MG_Remote/Server/ServerLoop.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 package v1: the apply thread, its affinity, its parking, and the EGL ownership move.

#include "ServerLoop.h"
#include "ApplyThreadPolicy.h"
#include "SessionRuntime.h"
#include <MG_Remote/FatalFunnel.h>
#include <MG_Pipe/PipeSessionFail.h>
#include <MG_Remote/Transport/LinkMetrics.h>
#include <MG_Backend/MGPipe/PipeInputs.h>

#include <Config.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_Backend/DirectGLES/DirectGLES.h>
#include <MG_Util/Debug/Log.h>
#include <MG_Util/Metrics/PipeStats.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

#if defined(__linux__) || defined(__ANDROID__)
#include <pthread.h>
#include <sched.h>
#endif

namespace MobileGL::MG_Remote::Server {

    namespace {

        // P14: THE BACKEND'S TURN. The in-process display server runs one apply thread per
        // session, and the backend those threads drive was written for one GL thread: one owner
        // thread, one set of binding shadows, process-wide rings and pools. Two sessions applying
        // at once raced all of it - a session's buffer ops were deferred while the other thread
        // owned the backend, and a texture bind was skipped because the shadow described the
        // other session's context. So an apply thread holds the turn while it does backend work
        // (control requests, a drain batch, fence reports, teardown) and gives it up before it
        // parks. FIFO, so a session with a busy ring cannot starve its neighbours; reentrant for
        // the thread that holds it. When the turn moves to a different thread the backend takes
        // ownership for it and drops its binding shadows (DirectGLES::OnBackendTurnHandoff).
        class BackendTurnLock {
        public:
            void Lock() {
                const std::thread::id self = std::this_thread::get_id();
                std::unique_lock<std::mutex> lock(m_mutex);
                if (m_owner == self) {
                    ++m_depth;
                    return;
                }
                const Uint64 ticket = m_nextTicket++;
                m_turn.wait(lock, [&] { return m_serving == ticket; });
                m_owner = self;
                m_depth = 1;
                if (m_lastOwner != self) {
                    const Bool handoff = m_lastOwner != std::thread::id{};
                    m_lastOwner = self;
                    lock.unlock();
                    if (handoff) MG_Backend::DirectGLES::OnBackendTurnHandoff();
                }
            }
            void Unlock() {
                const std::lock_guard<std::mutex> lock(m_mutex);
                if (--m_depth > 0) return;
                m_owner = std::thread::id{};
                ++m_serving;
                m_turn.notify_all();
            }
            // Gives the turn up however deep the calling thread holds it, for a wait that does no
            // backend work; answers the depth Resume restores (0: the thread did not hold it).
            Uint32 YieldIfHeld() {
                const std::lock_guard<std::mutex> lock(m_mutex);
                if (m_owner != std::this_thread::get_id()) return 0;
                const Uint32 depth = m_depth;
                m_depth = 0;
                m_owner = std::thread::id{};
                ++m_serving;
                m_turn.notify_all();
                return depth;
            }
            void Resume(Uint32 depth) {
                if (depth == 0) return;
                Lock();
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_depth = depth;
            }

        private:
            std::mutex m_mutex;
            std::condition_variable m_turn;
            Uint64 m_nextTicket = 0;
            Uint64 m_serving = 0;
            std::thread::id m_owner{};
            std::thread::id m_lastOwner{};
            Uint32 m_depth = 0;
        };

        BackendTurnLock& BackendTurn() {
            static BackendTurnLock& turn = *new BackendTurnLock(); // leaked at exit (ID-8)
            return turn;
        }

        class ScopedBackendTurn {
        public:
            ScopedBackendTurn() { BackendTurn().Lock(); }
            ~ScopedBackendTurn() { BackendTurn().Unlock(); }
            ScopedBackendTurn(const ScopedBackendTurn&) = delete;
            ScopedBackendTurn& operator=(const ScopedBackendTurn&) = delete;
        };

        // A WAIT FOR THE SERVER WINDOW HOLDS NO TURN. It can last seconds - a window that is not up
        // yet, or the lease of a session that is ending - and the other sessions' apply threads need
        // the turn meanwhile: the ending one to tear its backend down, which is what ends its lease.
        // Nothing of the backend is touched while it is given up; it is taken back (a handoff, when
        // another thread had it) before the waiter goes on.
        class ScopedBackendTurnYield {
        public:
            ScopedBackendTurnYield() : m_depth(BackendTurn().YieldIfHeld()) {}
            ~ScopedBackendTurnYield() { BackendTurn().Resume(m_depth); }
            ScopedBackendTurnYield(const ScopedBackendTurnYield&) = delete;
            ScopedBackendTurnYield& operator=(const ScopedBackendTurnYield&) = delete;

        private:
            const Uint32 m_depth;
        };

        // The process-wide test seam (ServerLoop.h).
        std::atomic<ServerLoop::ServerOwnedSurfaceHook> g_serverOwnedSurfaceHook{nullptr};

        // FatalFunnel's latch observer. The latching thread is one of the session's own (its scope
        // resolves ServerLoopInstance to that session's loop): a session that latched is over, and
        // its lease on the server window, if it holds one, ends once its apply thread has torn the
        // backend down - which a fresh session of the same client waits for (MarkLeaseEnding).
        void MarkLatchedSessionsLeaseEnding() { ServerDisplayInstance().MarkLeaseEnding(&ServerLoopInstance()); }

        // The bounded join. InProcessTransportTest.cpp:344 uses five seconds for the same
        // reason: a lost wakeup must be a RED TEST and not a hung CI job.
        constexpr Uint32 kJoinTimeoutMs = 5000;
        // THE BUSY BOUND. A thread that is not parked when kJoinTimeoutMs runs out is inside the
        // backend - a draw or a fence wait the GPU has not finished - and on a GPU the driver has
        // to reset (a shader that never ends: Adreno's preemption fault fires after ~10 s, once per
        // stuck submission) that takes several times the lost-wakeup bound. Dying at 5 s there
        // took the whole display server - every session, the compositor's included - down with
        // the one client whose GPU work hung. The wait goes on, to this bound, which still turns
        // a thread that never comes back into a named death.
        constexpr Uint32 kBusyJoinTimeoutMs = 60000;

        // A core counts as "big" if its cpufreq ceiling is within 15% of the fastest core's -
        // the identical rule and the identical constant as ShaderCompilePool.cpp:28 and :73-95.
        // The probe is re-derived here rather than exported from there because the two want
        // DIFFERENT ANSWERS from the same data: the pool wants a COUNT (how many workers), and
        // an affinity wants a MASK (which cpus). DetectBigCoreCount() cannot answer the second,
        // so exporting it would have meant either a second function in another package's file
        // or a mask reconstructed from a count, which is wrong on any asymmetric topology whose
        // big cores are not cpu0..cpuN-1.
        constexpr Uint64 kBigCoreFrequencyPercent = 85;

        Uint64 ReadCpuMaxFrequencyKHz(const Uint cpu) {
            char path[128];
            std::snprintf(path, sizeof(path),
                          "/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_max_freq", cpu);
            std::FILE* file = std::fopen(path, "r");
            if (file == nullptr) return 0;
            unsigned long long value = 0;
            const int scanned = std::fscanf(file, "%llu", &value);
            std::fclose(file);
            return scanned == 1 ? static_cast<Uint64>(value) : 0;
        }

        // The set of cpus whose ceiling is within 15% of the peak, as a bit per cpu. Zero when
        // the topology cannot be read - Windows, macOS, a container that hides the cpufreq
        // tree, or a partially readable one - because with no asymmetry information the honest
        // answer is "do not pin", not "pin to a guess".
        // P11 B1: and the PRIME set beside it - the cpus AT the peak ceiling - which the dialled-in
        // shared-segment rule needs so it never hands the spinning apply thread the only prime core.
        void DetectCoreTopology(Uint64* outBig, Uint64* outPrime) {
            *outBig = *outPrime = 0;
            const Uint cpuCount = std::min(64u, std::max(1u, std::thread::hardware_concurrency()));
            Uint64 frequencies[64] = {};
            for (Uint cpu = 0; cpu < cpuCount; ++cpu) {
                frequencies[cpu] = ReadCpuMaxFrequencyKHz(cpu);
                if (frequencies[cpu] == 0) return;
            }
            Uint64 peak = 0;
            for (Uint cpu = 0; cpu < cpuCount; ++cpu) peak = std::max(peak, frequencies[cpu]);
            if (peak == 0) return;
            const Uint64 threshold = peak * kBigCoreFrequencyPercent / 100;
            for (Uint cpu = 0; cpu < cpuCount; ++cpu) {
                if (frequencies[cpu] >= threshold) *outBig |= (1ull << cpu);
                if (frequencies[cpu] == peak) *outPrime |= (1ull << cpu);
            }
        }

        // One bit per cpu the process can count, the same count DetectCoreTopology walks - what
        // lets ApplyThreadPolicy tell a real prime set from a symmetric machine's "every cpu".
        Uint64 OnlineCpuMask() {
            const Uint cpuCount = std::min(64u, std::max(1u, std::thread::hardware_concurrency()));
            return cpuCount >= 64 ? ~0ull : ((1ull << cpuCount) - 1);
        }

        // Whether MOBILEGL_IPC_SPIN_US was set at all: ConfigLoader answers 50 for "unset" and
        // for "50", and the dialled-in peer's default spin (ApplyThreadPolicy.h) applies only to
        // the first.
        Bool SpinSetExplicitly() {
            const char* text = std::getenv("MOBILEGL_IPC_SPIN_US");
            return text != nullptr && text[0] != '\0';
        }

        // The per-frame P65ServerFrame line is a measurement aid, not a log: at one line per present
        // of every session it floods the device log (and pushes the lines tests grep for out of the
        // ring). Opt in with MOBILEGL_SERVER_FRAME_STATS=1, or on Android the property
        // debug.mobilegl.server_frame_stats=1; read once.
        Bool ServerFrameStatsEnabled() {
            static const Bool enabled = [] {
                const char* value = std::getenv("MOBILEGL_SERVER_FRAME_STATS");
#if defined(__ANDROID__)
                char property[PROP_VALUE_MAX] = {};
                if (value == nullptr && __system_property_get("debug.mobilegl.server_frame_stats", property) > 0)
                    value = property;
#endif
                return value != nullptr && value[0] != ' ' && value[0] != '0';
            }();
            return enabled;
        }

        void NameThisThread(const char* name) {
#if defined(__linux__) || defined(__ANDROID__)
            pthread_setname_np(pthread_self(), name);
#else
            (void)name;
#endif
        }

        // Returns the mask that the kernel ACTUALLY applied - the EFFECTIVE mask read back with
        // sched_getaffinity, not the requested one (codex 11). A cpuset that permits only a subset
        // of the requested cpus is accepted by sched_setaffinity with the intersection, and
        // logging the request would then claim cpus the thread never ran on - which is precisely
        // the "an affinity that silently did nothing looks like one that worked" failure the
        // resolved mask exists to make visible. 0 when nothing was applied.
        Uint64 ApplyAffinity(Uint64 requested) {
            if (requested == 0) return 0;
#if defined(__linux__) || defined(__ANDROID__)
            cpu_set_t set;
            CPU_ZERO(&set);
            Uint applied = 0;
            for (Uint cpu = 0; cpu < 64; ++cpu) {
                if ((requested & (1ull << cpu)) != 0) {
                    CPU_SET(cpu, &set);
                    ++applied;
                }
            }
            if (applied == 0) return 0;
            if (sched_setaffinity(0, sizeof(set), &set) != 0) return 0;
            // Read back the mask the kernel really honoured. This is the codex-11 fix: the log
            // and ResolvedAffinityMask() report the EFFECTIVE set, so a request the cpuset trimmed
            // is visible as a smaller resolved mask rather than as a lie that matched the request.
            cpu_set_t effective;
            CPU_ZERO(&effective);
            if (sched_getaffinity(0, sizeof(effective), &effective) != 0) {
                return requested; // best effort: the set succeeded, the read did not
            }
            Uint64 mask = 0;
            for (Uint cpu = 0; cpu < 64; ++cpu) {
                if (CPU_ISSET(cpu, &effective)) mask |= (1ull << cpu);
            }
            return mask;
#else
            return 0;
#endif
        }

        Uint32 SpinUsFromConfig() {
#if MOBILEGL_BUILD_DISAGGREGATED
            return MG_Config::Ipc.SpinUs;
#else
            return Transport::kDefaultSpinUs;
#endif
        }

        const char* AffinityStringFromConfig() {
#if MOBILEGL_BUILD_DISAGGREGATED
            return MG_Config::Ipc.ServerAffinity.c_str();
#else
            return "off";
#endif
        }

        // P9 W3, FAULT INJECTION F2 (HANDOFF-P9 §4): THE APPLY THREAD AS A LOG FIREHOSE.
        //
        // MOBILEGL_TEST_APPLY_LOG_FLOOD=<n> makes the apply thread log, after EVERY record it applies,
        // `n` WARN lines of about 600 bytes and one ERROR line, each numbered in its own series
        // ("flood warn #k", "flood error #k"). It is the one producer F2 needs and nothing in the
        // tree otherwise is: production logging is not allowed to repeat on a per-record path
        // (Log.h), so no real verb can be made to fill a TCP control connection with lines at will -
        // and "the apply thread is not held by logging while its client does not read" can only be
        // asserted about a thread that is logging a lot. The numbering is what lets the test prove
        // every ERROR line arrived (in order) while WARN lines were dropped.
        //
        // Meant for a server in its OWN process (a `--serve` session child). A TEST knob: read once
        // per Start(), 0 unless set, capped, and it says so in the log when it is set.
        Uint32 ApplyLogFloodLinesForTest() {
            const char* text = std::getenv("MOBILEGL_TEST_APPLY_LOG_FLOOD");
            if (text == nullptr || *text == '\0') return 0;
            const long lines = std::strtol(text, nullptr, 10);
            if (lines <= 0) return 0;
            const Uint32 capped = static_cast<Uint32>(std::min<long>(lines, 4096));
            MGLOG_W("MG_Remote server: MOBILEGL_TEST_APPLY_LOG_FLOOD=%u - every applied record logs that many "
                    "WARN lines and one ERROR line. A test-only lever (fault injection F2); never set it in a "
                    "measured run",
                    static_cast<unsigned>(capped));
            return capped;
        }

        void FloodLogForTest(Uint32 warnLines) {
            // One apply thread per process (ServerLoopInstance's), so plain counters are enough.
            static unsigned long long s_warn = 0;
            static unsigned long long s_error = 0;
            static const std::string kFiller(560, 'w');
            for (Uint32 i = 0; i < warnLines; ++i) {
                MGLOG_W("MG_Remote server: MOBILEGL_TEST_APPLY_LOG_FLOOD flood warn #%llu %s", ++s_warn,
                        kFiller.c_str());
            }
            MGLOG_E("MG_Remote server: MOBILEGL_TEST_APPLY_LOG_FLOOD flood error #%llu", ++s_error);
        }

    } // namespace

    // P11 B1: the one place the reserved core is computed for anybody outside the apply thread - the
    // supervisor announces it at start-up and the server app's broker relays it to the helper. The
    // supervisor has not loaded MG_Config (its session children do), so the knob is read from the
    // environment the children inherit.
    Uint64 ReservedApplyCoreForThisProcess(const char** outRule) {
        Uint64 big = 0, prime = 0;
        DetectCoreTopology(&big, &prime);
        ReservedCoreReason reason = ReservedCoreReason::NoAsymmetry;
        const Uint64 core = ReservedApplyCoreForConfig(std::getenv("MOBILEGL_IPC_SERVER_AFFINITY"), big, prime,
                                                       OnlineCpuMask(), &reason);
        if (outRule != nullptr) *outRule = ReservedCoreReasonText(reason);
        return core;
    }

    // ---------------------------------------------------------------------------------
    // The private backend object
    // ---------------------------------------------------------------------------------

    MobileGLResult ServerLoop::CreateBackend(BackendType type) {
        if (m_backend != nullptr) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        switch (type) {
        case BackendType::DirectGLES:
            m_backend = MakeUnique<MG_Backend::DirectGLES::BackendObject_DirectGLES>();
            break;
        case BackendType::DirectVulkan:
            m_backend = MakeUnique<MG_Backend::DirectVulkan::BackendObject_DirectVulkan>();
            break;
        default:
            // NOT a fallback to DirectGLES. An unknown backend under split has to be refused by
            // name: the alternative is a lane that says "split, DirectVulkan" and renders with
            // the other backend.
            MGLOG_E("MG_Remote server: MG_Config::ActiveBackendType is Unknown; the server role "
                    "has no backend to own a context with and the session is refused");
            return MOBILEGL_ERR_UNSUPPORTED;
        }
        // Loads the driver entry points and registers the resource op table
        // (BackendObject_DirectGLES.cpp:844-851). NO GL CALL AND NO EGL CALL HAPPENS HERE - the
        // native context is created and made current later, on the apply thread, when the
        // client's eglMakeCurrent crosses as a blocking control request.
        m_backend->Initialize();
        return MOBILEGL_OK;
    }

    MG_Backend::BackendObject* ServerLoop::Backend() { return m_backend.get(); }

    MobileGLResult ServerLoop::InstallBackendForTesting(UniquePtr<MG_Backend::BackendObject> backend) {
        if (m_backend != nullptr || backend == nullptr || m_running.load(std::memory_order_acquire)) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        m_backend = std::move(backend);
        return MOBILEGL_OK;
    }

    void ServerLoop::SetServerOwnedSurfaceHookForTesting(ServerOwnedSurfaceHook hook) {
        g_serverOwnedSurfaceHook.store(hook, std::memory_order_release);
    }

    // ---------------------------------------------------------------------------------
    // Start / the loop / Stop
    // ---------------------------------------------------------------------------------

    MobileGLResult ServerLoop::Start(ServerSession& session) {
        if (m_running.load(std::memory_order_acquire)) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        if (!session.Accepted()) {
            MGLOG_E("MG_Remote server: ServerLoop::Start before ServerSession::Accept - there "
                    "are no rings to park on");
            return MOBILEGL_ERR_NOT_INITIALIZED;
        }
        m_session = &session;
        // A session that latches marks its window lease as ending before its client hears of it.
        SetSessionLatchObserver(&MarkLatchedSessionsLeaseEnding);
        m_stopRequested.store(false, std::memory_order_release);
        m_abandonQueue.store(false, std::memory_order_release);
        // PH-6 fix round: the session's copy of the request, for ReserveEventOrBlock's wait. Lowered
        // with the loop's own, so a loop restarted on the same session does not forfeit at once.
        session.ClearApplyStopRequest();
        // m-3: reset THIS session's own diagnostics. DrainedRecords()/ParkCount() are absolute and
        // a case asserts == N, so a process that opens a SECOND session (context loss, or
        // Initialize after Destroy) must start them at zero rather than carry the first session's
        // tally - and a developer running the binary directly gets the count CI sees.
        m_drained.store(0, std::memory_order_release);
        m_parks.store(0, std::memory_order_release);
        m_parkBlocks.store(0, std::memory_order_release);
        m_nativeBinds.store(0, std::memory_order_release);
        m_clientReleases.store(0, std::memory_order_release);
        m_makeCurrentRepublishes.store(0, std::memory_order_release);
        m_controlSeq.store(0, std::memory_order_release);
        m_controlFramesDispatched.store(0, std::memory_order_release);
        m_haveCurrentTuple = false;
        // P12 (D4, D6): a new session decides its own surface mode, holds no window lease yet and
        // has no window-lost request pending. The apply thread that reads these is not started yet.
        // P14 S2: and it is THIS session's latch - the loop is the session's, so a second session
        // in the same process starts here too and neither's first surface decides the other's.
        m_surfaceMode = SessionSurfaceMode::None;
        m_holdsWindowLease = false;
        m_leasedWindow = nullptr;
        m_serverOwnedSurfaces.clear();
        m_windowRequests.store(0, std::memory_order_release);
        m_serverWindowsLost.store(0, std::memory_order_release);
        m_serverWindowSuspended.store(false, std::memory_order_release);
        m_serverWindowsSuspended.store(0, std::memory_order_release);
        m_serverWindowsResumed.store(0, std::memory_order_release);
        m_serverWindowWantWidth = 0;
        m_serverWindowWantHeight = 0;
        m_publishedWindowWidth = 0;
        m_publishedWindowHeight = 0;
        m_serverWindowResizes.store(0, std::memory_order_release);
        m_testLogFloodLines = ApplyLogFloodLinesForTest();
        {
            const std::lock_guard<std::mutex> lock(m_exitMutex);
            m_exited = false;
        }
        m_running.store(true, std::memory_order_release);
        m_thread = std::thread([this] { ApplyThreadMain(); });
        return MOBILEGL_OK;
    }

    Bool ServerLoop::Running() const { return m_running.load(std::memory_order_acquire); }

    // OnApplyThread() is inline in the header now (P5d round 3, package D). Its whole body is
    // one relaxed load of Detail::g_applyThreadKey and one comparison against the caller's own
    // thread pointer; the two writes to that key are in ApplyThreadMain, below.

    Uint64 ServerLoop::ResolvedAffinityMask() const { return m_affinityMask; }
    Uint64 ServerLoop::DrainedRecords() const { return m_drained.load(std::memory_order_acquire); }
    Uint64 ServerLoop::ParkCount() const { return m_parks.load(std::memory_order_acquire); }

    Uint64 ServerLoop::ParkBlockCount() const {
        return m_parkBlocks.load(std::memory_order_relaxed);
    }
    Uint64 ServerLoop::NativeBindCount() const { return m_nativeBinds.load(std::memory_order_acquire); }
    Uint64 ServerLoop::ClientReleaseCount() const {
        return m_clientReleases.load(std::memory_order_acquire);
    }
    Uint64 ServerLoop::MakeCurrentRepublishCount() const {
        return m_makeCurrentRepublishes.load(std::memory_order_acquire);
    }
    void ServerLoop::NoteMakeCurrentRepublished() {
        m_makeCurrentRepublishes.fetch_add(1, std::memory_order_acq_rel);
    }

    // C7 / ID-54. A release-current request (the three NO_* markers, exactly IsReleaseCurrentRequest's
    // test in BackendObject_DirectGLES.cpp) is a ClientRelease the server records but does not
    // forward. Otherwise an identical (dpy, draw, read, ctx) already held is a RepeatNoOp, and
    // anything else is a real NativeBind. Pure: a unit case drives it with no EGL context.
    EglBindAction ClassifyEglMakeCurrent(Bool haveCurrent, EGLDisplay curDpy, EGLSurface curDraw,
                                         EGLSurface curRead, EGLContext curCtx, EGLDisplay dpy,
                                         EGLSurface draw, EGLSurface read, EGLContext ctx) {
        if (draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE && ctx == EGL_NO_CONTEXT) {
            return EglBindAction::ClientRelease;
        }
        if (haveCurrent && curDpy == dpy && curDraw == draw && curRead == read && curCtx == ctx) {
            return EglBindAction::RepeatNoOp;
        }
        return EglBindAction::NativeBind;
    }

    ServerLoop::MakeCurrentOutcome ServerLoop::ApplyMakeCurrent(MG_Backend::BackendObject* backend,
                                                               EGLDisplay dpy, EGLSurface draw,
                                                               EGLSurface read, EGLContext ctx) {
        // Apply thread only (RunSurfaceControlFrame's dispatch put us here), so
        // m_haveCurrentTuple/m_cur* need no
        // lock. The counters are atomic for the reader on the test thread.
        //
        // P14 S4 left this classification alone on purpose - ServerLoop.h states the argument (the
        // currently bound tuple IS the unit ID-67 is about, and only one EGLContext can be current on
        // this thread). What S4 changed is one layer down: the tuple now names the (session, context)
        // registry's native context instead of the process's one.
        MakeCurrentOutcome outcome;
        const EglBindAction action = ClassifyEglMakeCurrent(m_haveCurrentTuple, m_curDpy, m_curDraw,
                                                            m_curRead, m_curCtx, dpy, draw, read, ctx);
        switch (action) {
        case EglBindAction::ClientRelease:
            // Recorded, NOT forwarded (ID-54): the context stays current on this thread until
            // ~BackendObject_DirectGLES or context loss. Forwarding backend->MakeEGLCurrent here
            // would route to DirectGLES::ReleaseCurrent, unbind, and clear
            // g_backendContextOwnerThread - reinstating the off-thread degradation the phase
            // removes. m_haveCurrentTuple is left intact so a later identical bind is still a
            // RepeatNoOp (the driver never lost the context).
            m_clientReleases.fetch_add(1, std::memory_order_acq_rel);
            MG_Pipe::MGPipeServerSetContextLive(false);
            outcome.ok = true;
            outcome.boundNatively = false;
            return outcome;
        case EglBindAction::RepeatNoOp:
            MG_Pipe::MGPipeServerSetContextLive(true);
            // ID-54's "a no-op apart from the R-12 republish decision": the decision for an
            // identical repeat is NO republish, because nothing ran that could have moved the
            // caps - InitCapabilities runs inside the backend's MakeEGLCurrent, which this arm
            // does not reach, so a republish here would re-send the snapshot the last real bind
            // already sent. (c1's BackendObject_Remote::InitCapabilities does not lean on this
            // either way: it asks the server through ServerInitCapabilities, which publishes.)
            outcome.ok = true;
            outcome.boundNatively = false;
            return outcome;
        case EglBindAction::NativeBind:
            break;
        }
        // "Forwarded" is the honest word: the backend object decides for itself whether the
        // driver needs a native eglMakeCurrent (BackendObject_DirectGLES skips it for a surface
        // that is already current on this thread - its own creation bound it), and the C7
        // control counts THAT at the EGL function table. This counter counts forwards.
        outcome.ok = backend->MakeEGLCurrent(dpy, draw, read, ctx);
        if (!outcome.ok) return outcome;
        MG_Pipe::MGPipeServerSetContextLive(true);
        m_haveCurrentTuple = true;
        m_curDpy = dpy;
        m_curDraw = draw;
        m_curRead = read;
        m_curCtx = ctx;
        m_nativeBinds.fetch_add(1, std::memory_order_acq_rel);
        outcome.boundNatively = true;
        return outcome;
    }

    void ServerLoop::ForgetCurrentTuple() {
        m_haveCurrentTuple = false;
        m_curDpy = EGL_NO_DISPLAY;
        m_curDraw = EGL_NO_SURFACE;
        m_curRead = EGL_NO_SURFACE;
        m_curCtx = EGL_NO_CONTEXT;
    }

    void ServerLoop::ForgetCurrentTupleIfItNames(EGLSurface surface) {
        if (!m_haveCurrentTuple) return;
        if (m_curDraw == surface || m_curRead == surface) ForgetCurrentTuple();
    }

    void ServerLoop::ApplyThreadMain() {
        // THE IDENTITY IS PUBLISHED FIRST, BEFORE THE THREAD HAS A NAME OR AN AFFINITY. Every
        // role guard in the tree reads it, and the very first record this thread applies must
        // already answer "yes" to OnApplyThread() - otherwise the applier's own reads would
        // look like client reads to the guards.
        //
        // P14 S2: two words, both about THIS thread. t_onApplyThread is the guard's answer; the
        // process counter beside it is what MG_Backend/DirectGLES/Managers.cpp reads as "an apply
        // thread is running" (ServerLoop.h's block). The scope is opened BEFORE either, because
        // every read below - the session's peer, its rings, the applier it attaches - must already
        // resolve to this session rather than to the process's default.
        ServerSession& session = *m_session;
        std::unique_ptr<Detail::ThreadSessionScope> scope;
        if (m_runtime != nullptr) scope = std::make_unique<Detail::ThreadSessionScope>(*m_runtime);
        Detail::t_onApplyThread = true;
        Detail::g_applyThreadKey.fetch_add(1, std::memory_order_relaxed);
        NameThisThread("mgl-srv-apply");
        // P6: THIS THREAD IS THE SERVER, and saying so is what sends its lines to the server's
        // own log. Under spawn the process role already answers this; under INPROC it is the
        // only thing that can, because the client role is another thread of this same process.
        MG_Util::Debug::SetThreadLogRole(MG_Util::Debug::LogRole::Server);
        // P12 review fix: and it is one of the session's own threads, whose lines a session-scoped
        // forwarder (the in-process display server's) sends to the client. Inert otherwise.
        MG_Util::Debug::SetThreadForwardsLogToPeer(true);

        // MOBILEGL_IPC_SERVER_AFFINITY = `auto` | `off` | an explicit mask (0x... or decimal).
        // The RAW STRING is what Config keeps, because the resolved mask is what gets logged -
        // "an affinity that silently did nothing looks exactly like one that worked"
        // (CONTRACT-P5 5). P11 B1: `auto` now depends on who the client is (ApplyThreadPolicy.h),
        // and so does the spin before each park.
        const char* raw = AffinityStringFromConfig();
        const Bool automatic = raw == nullptr || raw[0] == '\0' || std::strcmp(raw, "auto") == 0;
        Uint64 bigCores = 0, primeCores = 0;
        if (automatic) DetectCoreTopology(&bigCores, &primeCores);
        const ApplyThreadPolicy policy =
            SelectApplyThreadPolicy(raw, SpinSetExplicitly(), SpinUsFromConfig(), session.Peer(),
                                    session.PeerSharedSegments(), bigCores, primeCores, OnlineCpuMask());
        const Bool recognised = policy.recognised;
        const Uint64 requested = policy.requestedMask;
        m_affinityMask = ApplyAffinity(requested);
        if (!recognised) {
            MGLOG_E("MG_Remote server: MOBILEGL_IPC_SERVER_AFFINITY='%s' is not `auto`, `off` or "
                    "a number; NO affinity was applied. This is logged rather than defaulted "
                    "because an affinity that silently did nothing is indistinguishable from "
                    "one that worked",
                    raw == nullptr ? "" : raw);
        }
        // THE RESOLVED MASK, ALWAYS, INCLUDING 0. Contract 5's whole point: the string is what
        // an operator typed and the mask is what the kernel took.
        MGLOG_I("MG_Remote server: mgl-srv-apply started. MOBILEGL_IPC_SERVER_AFFINITY='%s' "
                "requested mask 0x%llx, RESOLVED mask 0x%llx (0 = no affinity applied), spin "
                "%u us; peer %s on %s, policy %s",
                raw == nullptr ? "" : raw, static_cast<unsigned long long>(requested),
                static_cast<unsigned long long>(m_affinityMask), policy.spinUs, ApplyPeerName(session.Peer()),
                session.PeerSharedSegments() ? "shared segments" : "stream", policy.rule);

        // A served session shares its process with other sessions: its backend's own native state
        // is bound to this thread before anything can apply a record (BackendObject.h). The inproc
        // one-process shape has no runtime and keeps the process-wide state.
        if (m_runtime != nullptr && m_backend != nullptr) m_backend->BindSessionStateToThisThread();

        // The decoder is built HERE, on this thread, because PipeWireDecoder is "not thread
        // safe: one decoder on the apply thread, by construction" and its constructor installs
        // the process-wide apply hook.
        session.Applier().Attach(session.DataLink(), m_backend.get());

        const auto signals = session.DataLink()->Signals();
        Transport::Doorbell& bell = session.DataLink()->ConsumerBell();
        Transport::RingConsumer& ring = session.CommandRing();
        const Uint32 spinUs = policy.spinUs;

        // THE PARK CONDITION IS THREE THINGS, NOT ONE, AND THAT IS THE WHOLE REASON THIS LOOP
        // DOES NOT CALL SessionConsumer::WaitForWork.
        //
        // WaitForWork's predicate is "a record is waiting" and nothing else. Park a thread on
        // kWaitForever with that predicate and neither a control request nor a Stop() can get
        // it out: Doorbell::Wait consumes the Notify with one Park, re-tests a condition
        // nothing published, finds the bell alive, and parks again - forever. Only
        // CondVarDoorbell::Kill() breaks that, and Kill is the CLIENT's teardown call, not
        // something an EGL make-current request may perform. So the predicate carries all
        // three arming conditions and the doorbell wakes the thread for any of them.
        // P5e (ra, CONTRACT-P5E §2.6) ADDS A FOURTH, AND IT IS THE ONE THAT SAYS "NOT NOW"
        // RATHER THAN "WAKE UP". A full SEG_EVENT means the apply thread has nowhere to put
        // the events the next record's apply would produce, so a waiting record is NOT work
        // yet: applying it would only park the producer half way through. Stop and Control
        // still win - a teardown may not be held up by a client that stopped draining - which
        // is why they are tested first and the ring test sits between them and the queue.
        //
        // The wake comes from EventRingConsumer::Drained(), which clears the latch AND rings
        // this bell (that pairing is the whole of the flow-control protocol; a cleared flag
        // with no bell is the lost wakeup the forward direction's publish-then-ring order
        // exists to prevent).
        //
        // PH-6 (ID-P7-2) ADDS THE FORFEIT LATCH BESIDE STOP, AND FOR STOP'S REASON. A session whose
        // reverse channel was forfeited (ServerSession::ForfeitReverseChannel) has a client that
        // is not reading SEG_EVENT, so its ring stays full - and the ring test below would park
        // this thread on `eventRingFull` for ever, waiting for a drain that the forfeit has
        // already decided will not come. The latch wins over the ring exactly as Stop does, and
        // the loop takes the same way out.
        const auto stopOrForfeit = [this, &session] {
            return m_stopRequested.load(std::memory_order_acquire) || session.ReverseChannelForfeited();
        };
        // P12 (D6) ADDS ONE MORE WAKE, BESIDE CONTROL: a window request - lost from ServerDisplay::
        // Detach (the UI thread's surfaceDestroyed, which is blocked until this thread answers), or
        // attached for a suspended session (a new window to resume on).
        const auto ready = [this, signals, &ring, &stopOrForfeit] {
            if (stopOrForfeit() || ControlIsPending() || m_windowRequests.load(std::memory_order_acquire) != 0)
                return true;
            if (signals.EventRingFull->load(std::memory_order_acquire) != 0) return false;
            return signals.CmdHead->load(std::memory_order_acquire) != ring.LocalTail();
        };

        // P10 (CONTRACT-P10.md §1): the client answers a fence poll from what this thread reports,
        // so a fence the GPU finishes must be reported whether or not another record ever arrives.
        // After every drain batch the pending fences are asked (a status query, no submit); a
        // thread about to go idle submits first and, while any fence is still pending, parks for
        // at most kFenceIdlePollMs rather than for ever. Never on a full SEG_EVENT: posting there
        // would park this thread inside the post, and the ring test below already says "not now".
        ServerVerbSink& verbs = session.Applier().Verbs();
        constexpr Uint32 kFenceIdlePollMs = 1;
        const auto eventRingHasRoom = [signals] {
            return signals.EventRingFull->load(std::memory_order_acquire) == 0;
        };

        for (;;) {
            std::optional<ScopedBackendTurn> turn(std::in_place);
            PumpControlRequest();
            if (stopOrForfeit()) break;
            // A SESSION WHOSE OWN GPU WORK HUNG THE GPU APPLIES NOTHING MORE. A shader that never
            // ends stops every process's GPU work until the kernel resets it (MG_Backend/
            // GpuHangWatch.h), and the same client's next frame usually carries the same shader. The
            // backend's verdict - the driver's GUILTY report on Espryt, the hang watch on Magma - is
            // asked HERE, before the drain, so that frame is never applied: the session latches
            // (DeviceLost, its client reads a lost context) and leaves below. Cheap by contract.
            if (m_backend != nullptr && !SessionLatched()) (void)m_backend->LatchIfGpuFaulted();
            if (eventRingHasRoom()) DrainRing();
            if (stopOrForfeit()) break;
            // The session-targeted device-loss debug knob (PipeSessionFail.h): asked here, after a
            // drain, so it reaches a client that never presents or reads back, once the session
            // has a context bound (it renders); latched as the backends latch a real loss. Unset
            // it is one clock read per iteration.
            if (!SessionLatched() && session.CurrentContextToken() != 0 &&
                MG_Pipe::MGPipeDebugSessionLossDue(session.ClientPid())) {
                (void)MG_Pipe::MGPipeSessionLatch(MG_Pipe::MGPipeFatalFamily::DeviceLost,
                                                  "MGPipe: Fatal{BackendDeviceLost, \"injected:pid=%u\"} - the debug "
                                                  "knob lost this session's device; the session ends and its client "
                                                  "reads a lost context, every other session keeps running",
                                                  session.ClientPid());
            }
            if (eventRingHasRoom() && !SessionLatched()) verbs.ReportSignaledFences(/*flush=*/false);
            // PH-1 (3): a latched session applies nothing more, so this thread leaves the loop
            // (and does not spin on a ring whose head is still ahead of a tail DrainRing will no
            // longer move). The exit path below tears the backend down as for a Stop; the
            // control connection's owner (ServerMain::RunSession) sees the latch and closes.
            // Pinned by ServerLoopTest's latched-batch case: without this break the thread is
            // still running, with no Stop(), three seconds after the latch.
            if (SessionLatched()) {
                MGLOG_E("MG_Remote server: mgl-srv-apply leaves its loop - the session latched a "
                        "named fault and declines the rest of the ring (PH-1)");
                break;
            }
            if (ready()) continue;
            // P10: going idle with a client fence still pending - submit what the driver holds and
            // ask again, once, before the park.
            if (verbs.HasUnreportedFences() && eventRingHasRoom()) {
                verbs.ReportSignaledFences(/*flush=*/true);
                if (ready()) continue;
            }

            // Never park holding the turn: a neighbour session may be waiting for it.
            turn.reset();
            const Uint64 waits = m_parks.fetch_add(1, std::memory_order_acq_rel) + 1;
            // THE WAIT LEDGER'S SERVER HALF (P5d round 3, package T item 4). The client may not
            // name this counter - CONTRACT-P5C rule E, layer 2: a GL-thread read of
            // ServerLoop's memory is exactly the cross-role access P5c zeroed - so the server
            // publishes its own reading and MG_Util::PipeStats is the meeting point. Two
            // relaxed stores behind the stats latch, on the one path in this loop that is about
            // to spin or block anyway; with MOBILEGL_PIPE_STATS unset it is a global load and a
            // predicted branch.
            //
            // BOTH NUMBERS, because they answer different questions. `srv` is how often the
            // loop reached Doorbell::Wait at all (ParkCount()'s long-standing meaning: an
            // intention, counted on the way TOWARD a park - ServerLoopTest's C10 note); `srvpark`
            // is how often it really blocked. The ratio is what says whether the spin budget is
            // sized for this workload, and it is only readable as a ratio because the two
            // numbers count the same events: m_parkBlocks is THIS loop's tally, handed to the
            // Wait below, not the bell's own ParkEntries(). A bell counts every waiter on its
            // endpoint, so reading it here would have made `srvpark` a superset of `srv` the
            // moment anything else waited on the consumer bell.
            if (MG_Util::PipeStats::Enabled()) {
                using MG_Util::PipeStats::Gauge;
                MG_Util::PipeStats::PublishGauge(Gauge::ServerWaits, waits);
                MG_Util::PipeStats::PublishGauge(Gauge::ServerParks, ParkBlockCount());
            }
            // Flush-on-idle publishes the last reply record even below the batch threshold.
            session.FlushDataProgress();
            const Bool fencePending = verbs.HasUnreportedFences();
            m_applyParked.store(true, std::memory_order_release);
            const bool woke = bell.Wait(*signals.ConsumerParked, ready, spinUs,
                                        fencePending ? kFenceIdlePollMs : Transport::kWaitForever,
                                        &m_parkBlocks);
            m_applyParked.store(false, std::memory_order_release);
            if (!woke) {
                // Wait returns false only on a dead bell or an expired deadline. A dead bell IS the
                // shutdown signal (table 3's teardown step 2); treating it as anything else would
                // spin at full clock for ever, since parking on a dead bell no longer blocks.
                if (bell.Dead()) {
                    MGLOG_D("MG_Remote server: the consumer doorbell is dead; mgl-srv-apply is "
                            "shutting down");
                    break;
                }
                // P10: the deadline is the fence re-check, and the only park that has one.
                if (fencePending) continue;
                MGLOG_E("MG_Remote server: Doorbell::Wait(kWaitForever) returned false on a live "
                        "bell - that is impossible by Doorbell::Wait's park loop (the only "
                        "false returns are the Dead() arm and an expired deadline) and means "
                        "the bell's "
                        "Dead() answer moved under the waiter. Shutting the loop down rather "
                        "than spinning");
                break;
            }
        }

        // Whatever is still queued gets applied before the context goes. The client's own
        // drain (ClientSession::Stop step 1) normally empties the ring first and is BOUNDED, so
        // a timeout there leaves records here; applying them costs nothing and dropping them
        // would leave the emitter's var-tails referenced by records nobody ever read.
        //
        // EXCEPT AFTER A FORFEIT (PH-6). That stop was not asked for by a client finishing its
        // work; it was decided about a peer that stopped reading this session's answers, and
        // every event the rest of its queue produced would only be one more counted drop - while
        // a peer that keeps publishing could keep this final drain going as long as it liked,
        // which is exactly the unbounded hold the forfeit exists to end. The queue is left
        // unapplied; the rings die with the session.
        //
        // AND AFTER AbandonQueuedRecords (P12 review fix), for the forfeit's reason: the server is
        // stopping under a client that may still be streaming, and draining what it keeps queuing
        // is the unbounded hold Stop()'s bounded join turns into an abort.
        //
        // THE SESSION IS ENDING, whatever ended it: its lease on the server window (if it holds one)
        // ends below, once the backend is gone, and a fresh session of the same client that asks for
        // the window meanwhile waits for that instead of being refused.
        ServerDisplayInstance().MarkLeaseEnding(this);
        std::optional<ScopedBackendTurn> exitTurn(std::in_place);
        const Bool forfeited = session.ReverseChannelForfeited();
        const Bool abandoned = m_abandonQueue.load(std::memory_order_acquire);
        PumpControlRequest();
        if (forfeited) {
            MGLOG_E("MG_Remote server: mgl-srv-apply stops on ReverseChannelForfeit - %llu reverse-channel "
                    "event(s) dropped, applied seq %llu; the records still queued are not applied",
                    static_cast<unsigned long long>(session.ForfeitDrops()),
                    static_cast<unsigned long long>(session.Consumer().AppliedSeq()));
        } else if (abandoned) {
            MGLOG_W("MG_Remote server: mgl-srv-apply stops because the server is stopping - applied seq %llu; "
                    "the records still queued are not applied",
                    static_cast<unsigned long long>(session.Consumer().AppliedSeq()));
        } else {
            DrainRing();
        }

        // THE BACKEND IS DESTROYED ON THIS THREAD, WHILE IT IS STILL THE CONTEXT OWNER.
        // ~BackendObject_DirectGLES calls DestroyEGLContext (BackendObject_DirectGLES.cpp:
        // 833-835), which runs eglMakeCurrent(NO_SURFACE) / eglDestroyContext / eglTerminate.
        // Those must happen on the thread eglMakeCurrent was issued from; doing it on the app
        // thread - which is what pActiveBackendObject.reset() at MobileGL/Init.cpp:68 would do
        // - destroys a context this thread still holds current.
        session.Applier().Detach();
        if (m_backend != nullptr) {
            MGLOG_I("MG_Remote server: destroying the server's BackendObject on mgl-srv-apply, "
                    "which is the context owner");
            m_backend.reset();
        }
        exitTurn.reset();
        // P12 (D6): the backend - and with it every surface on the server's window - is gone, so the
        // session's lease on that window ends here, AFTER the reset. A Detach waiting on it returns
        // now; one that arrives later finds no lease and releases the window at once. A suspended
        // session stops waiting for a window to resume on.
        m_windowRequests.store(0, std::memory_order_release);
        ForgetServerWindow();
        MG_Pipe::MGPipeServerSetContextLive(false);
        // N-3: the context died with the backend; a tuple that outlives it would make the next
        // session's first make-current onto the same (recycled) handle values a RepeatNoOp.
        ForgetCurrentTuple();

        // C2: THE m_running CLEAR IS INSIDE THIS SAME CRITICAL SECTION AS THE FINAL DRAIN OF THE
        // MAILBOX. It used to be a separate store after the block, and that gap was a lost-forever
        // hang: a caller that read m_running == true just before this thread exited, then had this
        // thread run the block (finding nothing pending) and clear m_running OUTSIDE the lock,
        // would go on to publish a request into a mailbox no thread will ever pump and block on
        // m_controlDone with no answer possible (the bounded join has already succeeded, so there
        // is not even a Fatal{ApplyThreadJoinTimeout}). With the clear under the lock, a caller
        // either takes the lock FIRST (its request is here and answered NOT_INITIALIZED) or takes
        // it AFTER (it sees !m_running under the lock in RunSurfaceControlFrame and returns
        // NOT_INITIALIZED without publishing). There is no third order.
        {
            const std::lock_guard<std::mutex> lock(m_controlMutex);
            // The shadow dies with the mailbox it shadows, under the same lock and inside the
            // same critical section as the m_running clear: nothing will ever pump again, so a
            // `true` left here would be a claim no thread can retire.
            m_controlPosted.store(false, std::memory_order_release);
            if (m_controlPending) {
                m_controlPending = false;
                m_controlFinished = true;
                m_controlResult = MOBILEGL_ERR_NOT_INITIALIZED;
                MGLOG_E("MG_Remote server: a control request was still posted when mgl-srv-apply "
                        "exited; it is answered NOT_INITIALIZED rather than left blocking");
            }
            m_running.store(false, std::memory_order_release);
            // THE IDENTITY DIES WITH m_running, IN THE SAME CRITICAL SECTION, because it IS
            // m_running as far as OnApplyThread() is concerned (P5d round 3, package D): the
            // old predicate was `m_running && id == mine`, so it stopped answering true at
            // exactly this store, and the key has to stop meaning "this thread" at exactly the
            // same store or the window would have moved. Everything that must still run as the
            // server role - the final PumpControlRequest/DrainRing, Applier().Detach(),
            // ~BackendObject_DirectGLES and the ReleaseEGLResources it posts back to itself -
            // is already above this block.
            //
            // DO NOT MOVE THIS STORE UP ON THE STRENGTH OF A GREEN ctest (review round 3). That
            // this line EXISTS is pinned below (ServerLoopTest's
            // AThreadCreatedAfterTheLoopStoppedIsNotMistakenForTheApplyThread: delete the store
            // and the key survives the join, and the next thread that inherits this one's TLS
            // block - hence its thread pointer - answers OnApplyThread() TRUE). That it sits
            // HERE rather than fifteen lines earlier is NOT pinned by any unit case and cannot
            // be: ServerLoopTest has no GL context, so CreateBackend is never called and the
            // ~BackendObject_DirectGLES -> ReleaseEGLResources self-post named above is never
            // built. Move the store above m_backend.reset() and that post misses
            // RunSurfaceControlFrame's re-entrancy arm (`if (OnApplyThread()) return
            // ApplySurfaceControlFrame(frame);`) while m_running is still true, so it publishes
            // into the one-slot channel and waits on m_controlDone for an answer only THIS
            // thread could ever give. The integration lane - a real EGL teardown - is the only
            // thing that would catch that, and it would catch it as a hang, not as a failed
            // assertion.
            //
            // P14 S2: the guard's own word is lowered here too, and for the same reason - with
            // N sessions it can only ever have been this thread's, and the thread is about to
            // stop being an apply thread. The scope is destroyed by this function's frame.
            Detail::t_onApplyThread = false;
            Detail::g_applyThreadKey.fetch_sub(1, std::memory_order_relaxed);
        }
        m_controlDone.notify_all();

        SignalExited();
    }

    Bool ServerLoop::ControlIsPending() const {
        // ONE ACQUIRE LOAD, NO LOCK - see the header block on m_controlPosted. This is the
        // hottest call in the whole loop (the `ready` lambda runs it on every spin iteration of
        // every wait) and it used to be a mutex round trip.
        return m_controlPosted.load(std::memory_order_acquire);
    }

    Bool ServerLoop::PumpControlRequest() {
        // P12 (D6): A LOST SERVER WINDOW IS ANSWERED FIRST, before any frame this pump would run -
        // a frame taken now might render into the window the UI thread is waiting to destroy. One
        // relaxed-cost load on the idle poll, beside the shadow's. A suspended session's resume on
        // a new window is the same word's other bit.
        if (m_windowRequests.load(std::memory_order_acquire) != 0) HandleServerWindowRequests();
        // THE IDLE POLL'S FAST NEGATIVE. The loop calls this once per iteration whether or not
        // anything was posted, and the common answer is "nothing". Taking m_controlMutex to
        // learn that was the other half of package T's finding; the shadow answers it with a
        // load, and everything below this line still runs under the lock exactly as before.
        if (!m_controlPosted.load(std::memory_order_acquire)) return false;

        SurfaceControlFrame frame;
        Bool declinedOnLatch = false;
        {
            const std::lock_guard<std::mutex> lock(m_controlMutex);
            if (!m_controlPending) {
                // The shadow outlived its request - it cannot happen through the poster's
                // path, but a `true` that nobody clears would make `ready` permanently true
                // and the loop would never park again, so it is cleared here rather than
                // trusted.
                m_controlPosted.store(false, std::memory_order_release);
                return false;
            }
            frame = m_controlFrame;
            // CLEARED WHEN THE REQUEST IS TAKEN, not when it finishes. m_controlPending stays
            // true for the duration of the dispatch (the C2 exit block reads it), but the thread
            // that would act on the shadow is THIS one and it is busy running the frame; leaving
            // the shadow set would only make the post-work iteration spin instead of park.
            m_controlPosted.store(false, std::memory_order_release);
            // PH-1 (3), ID-P7-1: A LATCHED SESSION RUNS NO FURTHER CONTROL OP. A frame can be
            // posted before the latch and taken after it - RunSession passed SurfaceOpCodec's
            // latch check a moment before a record latched on this thread, and this thread's
            // exit path pumps once more - and a MakeCurrent, a surface create or a swap has no
            // business running on a backend whose session has already been declared over. It
            // is answered here, without a dispatch, with the code SurfaceOpCodec's own latched
            // arm gives. ServerLoopTest's post-after-latch case goes red without it.
            if (SessionLatched()) {
                m_controlResult = MOBILEGL_ERR_PROTOCOL_MISMATCH;
                m_controlPending = false;
                m_controlFinished = true;
                declinedOnLatch = true;
            }
        }
        if (declinedOnLatch) {
            m_controlDone.notify_all();
            return true;
        }
        // A TEARDOWN ORDERS AFTER THE RECORDS ITS CLIENT PUBLISHED BEFORE ASKING FOR IT. The loop
        // pumps a posted frame before it drains the ring, so an eglTerminate (or a surface or
        // context release) could otherwise run ahead of that client's last swap still queued in
        // the ring - and the swap then reaches a backend whose renderer the release destroyed. Not
        // on a full SEG_EVENT, for the loop's own reason: the drain would park inside a post.
        if ((frame.kind == SurfaceControlOp::ReleaseResources || frame.kind == SurfaceControlOp::ReleaseSurface ||
             frame.kind == SurfaceControlOp::DestroyContext) &&
            m_session != nullptr && m_session->DataLink() != nullptr &&
            m_session->DataLink()->Signals().EventRingFull->load(std::memory_order_acquire) == 0) {
            DrainRing();
        }
        // The dispatch runs OUTSIDE the lock, exactly as the old work(user) did, and fills the
        // frame's reply half; the reply is then published back into the slot under the lock so
        // the poster's copy-out after m_controlDone sees it.
        const auto dispatchStart = std::chrono::steady_clock::now();
        const MobileGLResult result = ApplySurfaceControlFrame(
            frame, m_controlProbeHook.load(std::memory_order_acquire),
            m_controlProbeUser.load(std::memory_order_acquire));
        // p7/spawnhang: A SLOW DISPATCH NAMES ITSELF. The first surface creation / MakeCurrent is
        // where a lazy native bring-up runs, and on a cold CI runner it outran the client's whole
        // reply budget with nothing in this log to say so - the investigation had to infer it.
        const auto dispatchMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - dispatchStart).count();
        if (dispatchMs >= static_cast<long long>(kSlowControlDispatchMs)) {
            MGLOG_W("MG_Remote server: %s seq %llu ran %lld ms on mgl-srv-apply (a lazy native "
                    "bring-up runs inside the first surface creation or MakeCurrent)",
                    SurfaceControlOpName(frame.kind), static_cast<unsigned long long>(frame.seq),
                    static_cast<long long>(dispatchMs));
        }
        {
            const std::lock_guard<std::mutex> lock(m_controlMutex);
            m_controlFrame = frame;
            m_controlResult = result;
            m_controlPending = false;
            m_controlFinished = true;
        }
        m_controlDone.notify_all();
        return true;
    }

    Uint64 ServerLoop::DrainRing() {
        // THE EMPTY-RING ANSWER COSTS TWO LOADS AND NOTHING ELSE, and that was AUDITED rather
        // than assumed (P5d round 3, package T item 2): the idle poll calls this on every
        // iteration, so a lock or a clock read in here would be the same defect
        // PumpControlRequest had. The path is SessionConsumer::ApplyOne -> RingConsumer::Pop,
        // and Pop's first act on an empty ring is one acquire load of cmdHead compared against
        // the consumer's own m_localTail (Ring.cpp:222-232) - no mutex, no steady_clock, no
        // allocation; `applied == 0` then skips the retire block below entirely. Keep it that
        // way: anything added here runs ~900 times a frame doing nothing.
        //
        // PH-1 (3), ID-P7-1: THE LATCH CHECK, ONCE, IMMEDIATELY BEFORE EVERY POP. Once a named
        // fault has latched (an armed spawn / TCP session child only - unarmed this is one acquire
        // load of a flag nobody sets), no further record is applied: the rest of the ring is
        // declined unread, the apply thread leaves its loop, and RunSession closes the session by
        // name. The one check does three jobs, and each has a case that goes red without it:
        //   * a drain ENTERED latched pops nothing - the apply thread's exit-path drain after it
        //     left on the latch, and any drain after RunSession's thread latched a control op
        //     (ServerLoopTest's latched-batch case: the exit-path drain applies the record behind
        //     the latched one);
        //   * a record that latched is the last one this drain pops, even when the peer published
        //     more behind it in the same batch (PeerLatchTest's two-record case, ServerLoopTest's
        //     latched-batch case);
        //   * a latch ANOTHER thread stores between two records - RunSession's control thread
        //     latching a malformed SurfaceOp while this drain is mid-batch - stops the next pop
        //     when it is stored before this load (codex closeout finding 6; ServerLoopLatchTest's
        //     between-records case). The first version checked at the top of the function and after
        //     each record, and made one more load (the PH-6 forfeit check) before the next pop;
        //     checking HERE only NARROWS that control/apply window to this load-to-pop span, it
        //     does not close it.
        // A latch stored inside that span still lets the one record already admitted be popped and
        // applied; the session then ends at the next check, one record later. Closing the span would
        // take a lock the control thread shares with every pop.
        //
        // The idle poll's cost is unchanged: this load replaces the function-top check the first
        // version made, and the empty-ring answer is still it plus Pop's cmdHead load.
        //
        // P12 (D6) REVIEW FIX: A LOST SERVER WINDOW IS ANSWERED BEFORE EVERY POP TOO, not only by
        // PumpControlRequest between batches. A batch ends when the ring is empty, and a client
        // streaming frames keeps it from emptying for seconds: surfaceDestroyed (ServerDisplay::
        // Detach) waited that long on the UI thread - past its 3 s bound, so it gave up and the
        // session went on presenting into the destroyed window. Checked AFTER the latch check (the
        // MECHANICS row in ph_latch_sites.py pins that one as the loop's first statement). A backend
        // that cannot suspend latches ServerWindowLost, so `continue` takes the latch check's way out;
        // a suspended session (and a resumed one - the request word's other bit) pops on. One more
        // acquire load per pop; the empty-ring answer pays it once.
        ServerSession& session = *m_session;
        Transport::SessionConsumer& consumer = session.Consumer();
        PipeApplier& applier = session.Applier();
        // ---- P65ServerFrame: WHERE THE SERVER'S TIME GOES (P12 measurement) -----------------
        //
        // THE CLIENT SIDE IS MEASURED OUT AND THE SERVER'S IS NOT MEASURED AT ALL. Every reading
        // this harness produces is the CLIENT's - wall_ns, wait_replies, client_thread_cpu_ns - and
        // they now add up to less than the frame: at window 6 the client burns 2.5 s of CPU and
        // 1,162 blocking waits, and removing 292 of those waits (window 8) moves the wall clock by
        // nothing. The same 101 MB over USB instead of WiFi moves it by nothing either. What is
        // left is the one process nobody has timed: the server applying 103,097 records.
        //
        // THE SPLIT IS THE POINT. apply_ms is the sum of the time spent INSIDE
        // PipeApplier::ApplyOne - the GL work, the decode, the reply posts - and wall_ms is Present
        // to Present on the apply thread. Their difference is everything that is NOT applying:
        // waiting on the ring for the next record, retiring, and the socket. So one line answers
        // "is the phone CPU-bound, or is it waiting for the client?" without a profiler, which is
        // the question the next optimisation has to be chosen from.
        //
        // FUNCTION-LOCAL STATIC BECAUSE THE FRAME DOES NOT FIT IN ONE DrainRing CALL: records
        // trickle in, so a frame spans many drains, and state that reset per drain would measure a
        // batch. There is exactly one apply thread (ServerLoopInstance's), which is what makes a
        // static safe here; the same reasoning the drain tally above uses.
        static auto sFrameWallStart = std::chrono::steady_clock::now();
        static std::uint64_t sFrameApplyNs = 0;
        static std::uint64_t sFrameRecords = 0;
        static std::uint64_t sFrameIndex = 0;
        Uint64 applied = 0;
        const bool timed = ServerFrameStatsEnabled();
        for (;;) {
            if (SessionLatched()) break;
            if (m_windowRequests.load(std::memory_order_acquire) != 0) {
                HandleServerWindowRequests();
                continue;
            }
            bool corrupt = false;
            const bool popped = consumer.ApplyOne(
                [this, &applier, timed](const Transport::RingRecordView& record) {
                    // P65ServerFrame: timed around the ONE call that is the server's work, and
                    // summed across the drains a frame spans. See the block above DrainRing's loop.
                    // Only when the line is on: two clock reads per record were a measurable part
                    // of a record's cost.
                    if (timed) {
                        const auto applyStarted = std::chrono::steady_clock::now();
                        applier.ApplyOne(record);
                        sFrameApplyNs += static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - applyStarted)
                                .count());
                    } else {
                        applier.ApplyOne(record);
                    }
                    ++sFrameRecords;
                    // P9 W3, F2: outside the apply timing above; 0 in every run that is not F2.
                    if (m_testLogFloodLines != 0) FloodLogForTest(m_testLogFloodLines);
                    if (record.kind == static_cast<std::uint16_t>(MG_Pipe::MGPWireOp::Present)) {
                        const auto now = std::chrono::steady_clock::now();
                        const auto wallNs = static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(now - sFrameWallStart)
                                .count());
                        const double applyMs = static_cast<double>(sFrameApplyNs) / 1e6;
                        const double wallMs = static_cast<double>(wallNs) / 1e6;
                        // AND THE STAGE NOTHING ELSE COVERS: the io thread's socket reads, which
                        // its own MGLOG_ lines cannot report (the phone's forwarded log carries
                        // mgl-srv-apply and mgl-display-ser only). read_ms is wall time inside
                        // recv - blocking included - so a frame that is 90% read_ms is a frame the
                        // reader spent WAITING for bytes, and one that is mostly recv calls with
                        // little read_ms is a reader that is copying.
                        const auto read = Transport::LinkMetricsTakeReadStats();
                        if (ServerFrameStatsEnabled()) {
                            MGLOG_I("P65ServerFrame frame=%llu records=%llu apply_ms=%.1f wall_ms=%.1f "
                                    "outside_ms=%.1f | read: %llu B in %llu recv(%llu reads) "
                                    "recv_ms=%.1f total_ms=%.1f (%.0f%% blocked, %.1f MB/s)",
                                    static_cast<unsigned long long>(++sFrameIndex),
                                    static_cast<unsigned long long>(sFrameRecords), applyMs, wallMs,
                                    wallMs - applyMs,
                                    static_cast<unsigned long long>(read.bytes),
                                    static_cast<unsigned long long>(read.calls),
                                    static_cast<unsigned long long>(read.reads),
                                    static_cast<double>(read.nsInRecv) / 1e6,
                                    static_cast<double>(read.nsTotal) / 1e6,
                                    read.nsTotal ? 100.0 * static_cast<double>(read.nsInRecv) /
                                                       static_cast<double>(read.nsTotal)
                                                 : 0.0,
                                    read.nsTotal ? static_cast<double>(read.bytes) /
                                                       (static_cast<double>(read.nsTotal) / 1e9) / 1e6
                                                 : 0.0);
                        }
                        sFrameWallStart = now;
                        sFrameApplyNs = 0;
                        sFrameRecords = 0;
                    }
                    // THE TALLY MOVES HERE, INSIDE THE CALLBACK, AND NOT AFTER THE BATCH. s1's
                    // SessionConsumer::ApplyOne publishes appliedSeq the instant this callback
                    // returns, and publishing appliedSeq is what releases the client from the
                    // verb barrier (R-1). Anything this thread records about the record AFTER
                    // that publish is visible to the client only eventually - and a diagnostic
                    // that can lag the watermark it describes is the exact shape of the
                    // LeaveApplier race PipeApplier::ApplyOne's block describes, one level up.
                    // The first version of this function tallied once per batch, below, and
                    // AClearRecordCrossesAndIsStampedAsAVerbBoundary /
                    // TheSessionWatermarkAndTheDecoderTallyAgreeAfterEveryRecord read
                    // DrainedRecords() one behind appliedSeq on 1 run in ~40 under `-j 8`.
                    // Per-record, before the publish, it can never be behind.
                    m_drained.fetch_add(1, std::memory_order_acq_rel);
                },
                &corrupt);
            if (corrupt) {
                // Ring.h's own rule: a header the producer could not have written is
                // Fatal{ProtocolCorruption}, never a retry. Retrying re-reads the same bytes
                // for ever; skipping desynchronises seq, and seq IS the reply-slot id. PH-1 (3):
                // the header is the peer's bytes, so an armed session child latches here and
                // stops reading the ring - neither a retry nor a skip.
                (void)SessionLatch(MGFatalFamily::ProtocolCorruption, "MGPipe: Fatal{ProtocolCorruption, \"SEG_CMD record header\"} - the "
                        "consumer refused a record header at applied seq %llu",
                        static_cast<unsigned long long>(consumer.AppliedSeq()));
                break;
            }
            if (!popped) break;
            ++applied;
            // PH-6: the record whose event forfeited the reverse channel is the last one this
            // session applies. Checked only AFTER a popped record, so the empty-ring answer above
            // still costs its two loads and nothing else. (A record that LATCHED is stopped by the
            // loop-head check on the next iteration - ph_latch_sites.py's MECHANICS row pins that
            // check as the first statement of this loop.)
            if (session.ReverseChannelForfeited()) break;
            // P12 review fix: and the record in hand is the last one when the server is stopping
            // (AbandonQueuedRecords); same placement, same free empty-ring answer.
            if (m_abandonQueue.load(std::memory_order_acquire)) break;
            // TEST-ONLY scheduling point BETWEEN two records: after this record's own checks and
            // before the next pop's latch check. A control-thread latch stored here is seen by that
            // check (the first version let it through); one stored after the check and before the
            // pop still is not - the window is narrowed to check-to-pop, not closed, and the session
            // ends at the check after that record. Only after a popped record, so the idle poll never
            // pays for it; null outside ServerLoopLatchTest.
            if (const auto hook = m_betweenRecordsHook.load(std::memory_order_acquire)) hook();
        }
        if (applied != 0) {
            // THE TWO TALLIES MUST AGREE, AND THAT IS WHAT MAKES R-9's BATCHING BAN CHECKABLE.
            // RingControl::appliedSeq has one writer (SessionConsumer::ApplyOne, +1 per record)
            // and PipeWireDecoder keeps its own count; a batched publish would move one and not
            // the other, which a single counter could not have told apart (w1-v1 5).
            if (applier.DecoderAppliedSeq() != consumer.AppliedSeq()) {
                // PH-1 (3): latched, and nothing is retired against a watermark this side no
                // longer believes.
                (void)SessionLatch(MGFatalFamily::ProtocolCorruption, "MGPipe: Fatal{ProtocolCorruption, \"appliedSeq batched\"} - the "
                        "session's watermark is %llu and the decoder applied %llu records. P5 "
                        "forbids batching appliedSeq (R-9): the verb barrier's waiter reads it, "
                        "and a watermark ahead of the decoder promises work that has not run",
                        static_cast<unsigned long long>(consumer.AppliedSeq()),
                        static_cast<unsigned long long>(applier.DecoderAppliedSeq()));
                return applied;
            }
            // RETIRE. MANDATORY, not optional: RingProducer::FreeBytes() reclaims against
            // retiredTail only, and w1's SEG_STAGE linear allocator reclaims on retiredSeq - so
            // a loop that applies and never retires ends the first MOBILEGL_IPC_STAGE_MB of
            // staging in Fatal{RingOverrun, "SEG_STAGE"} (w1-v1 5). Once per drain batch, not
            // once per record: retiring LATE is always legal, retiring EARLY never is.
            if (const auto hook = m_beforeRetireHook.load(std::memory_order_acquire)) hook();
            consumer.RetireThrough(consumer.AppliedSeq());
            // LEAVING THE APPLIER (p1's M-5) IS *NOT* DONE HERE. It is done inside
            // PipeApplier::ApplyOne, before s1's SessionConsumer::ApplyOne publishes appliedSeq
            // - see the block there. A clear at this point races the client, which the barrier
            // has already released by then.
        }
        return applied;
    }

    void ServerLoop::SetRemoteControlSink(RemoteControlSink sink, void* user) {
        m_remoteSink = sink;
        m_remoteSinkUser = user;
    }

    MobileGLResult ServerLoop::RunSurfaceControlFrame(SurfaceControlFrame& frame) {
        if (frame.kind == SurfaceControlOp::None) return MOBILEGL_ERR_INVALID_ARGUMENT;

        // `cp`: the server is another process, so the frame CROSSES instead of
        // being posted. Checked before the seq mint below, because a wire request
        // must carry the CLIENT's number all the way through dispatch and the
        // reply - the local mint at :662 is for unnumbered LOCAL requests and
        // renumbering a wire one would make its reply uncorrelatable.
        if (m_remoteSink != nullptr) {
            return m_remoteSink(m_remoteSinkUser, frame);
        }
        // Zero marks an unnumbered local request. A wire request is already numbered by its
        // client and MUST retain that number all the way through dispatch and the reply.
        if (frame.seq == 0) frame.seq = m_controlSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
        // RE-ENTRANCY IS NOT A DEADLOCK. The teardown path posts from the apply thread itself -
        // ~BackendObject_DirectGLES reaches ReleaseEGLResources - and so does anything the
        // applier calls that wants "run this where the context is". Running inline is the
        // correct answer there and the only non-hanging one.
        if (OnApplyThread()) return ApplySurfaceControlFrame(frame);

        const std::lock_guard<std::mutex> callerLock(m_callerMutex);
        return PostSurfaceControlFrameWithCallerLock(frame);
    }

    MobileGLResult ServerLoop::PostSurfaceControlFrameWithCallerLock(SurfaceControlFrame& frame) {
        std::unique_lock<std::mutex> lock(m_controlMutex);
        // C2 + M-7: the m_running check and the publish are ONE critical section, and m_running is
        // cleared under this same lock on the way out (ApplyThreadMain's exit block), so this read
        // cannot see `true` for a thread that then vanishes before the publish. When there is no
        // thread there are two sub-cases and neither is a silent EGL-on-the-app-thread fallback:
        if (!m_running.load(std::memory_order_acquire)) {
            const Bool backendAlive = m_backend != nullptr;
            lock.unlock();
            if (backendAlive) {
                // M-7: the backend exists but no thread owns it - ClientSession::Start refused
                // (ServerLoop.cpp Start's !Accepted arm) or std::thread's constructor threw.
                // Dispatching the frame inline here would issue eglMakeCurrent on the APP thread
                // and stamp g_backendContextOwnerThread with it, so a lane called split would
                // render correctly with the context on the wrong thread - the one outcome R-1
                // exists to make impossible. Fatal by name, never a fallback; compare
                // CreateBackend's default: arm, which also refuses rather than substitutes.
                SessionFail(MGFatalFamily::ApplyThreadNotRunning, "MGPipe: Fatal{ApplyThreadNotRunning, \"EGL on the app thread\"} - a "
                        "control frame reached RunSurfaceControlFrame with the server's backend "
                        "built but no mgl-srv-apply thread running (ClientSession::Start failed "
                        "after ServerLoop::CreateBackend). Dispatching it inline would make the "
                        "context current on the APP thread - the split lane's whole premise. "
                        "Refusing by name rather than falling back to monolith");
            }
            // Backend already gone (post-Stop teardown, or the pre-Init window): the dispatch's
            // null-backend arm would answer NOT_INITIALIZED anyway, so that is the honest result
            // and running inline is pointless. This is the deterministic half of C2 - a forwarder
            // call after the loop stopped returns NOT_INITIALIZED, it does not hang and it does
            // not run on the caller.
            return MOBILEGL_ERR_NOT_INITIALIZED;
        }
        m_controlFrame = frame;
        m_controlPending = true;
        m_controlFinished = false;
        // THE SHADOW IS PUBLISHED LAST OF THE THREE AND WITH A RELEASE, inside this same
        // critical section: the apply thread's park predicate reads ONLY this word, so an
        // acquire load that returns true must also see the slot and flags above it. Storing it
        // first would let a pump that is already past its own fast-negative read a slot that
        // does not hold the frame yet.
        m_controlPosted.store(true, std::memory_order_release);
        // Publish THEN ring, in that order and never the other (Doorbell.h, NotifyIfParked's
        // PRECONDITION block: the fence only orders what precedes it). The bell
        // is rung unconditionally rather than through NotifyIfParked because this side does not
        // know whether the apply thread is parked or spinning, and CondVarDoorbell remembers a
        // wakeup that arrives while nobody is waiting. Held under m_controlMutex: wait() releases
        // it atomically, so the apply thread cannot observe the request until this side is
        // waiting, and the doorbell remembers the notify regardless.
        if (m_session != nullptr) {
            m_session->DataLink()->ConsumerBell().Notify();
        }
        // p7/spawnhang: THE WAIT IS SLICED, AND A SLICE THE APPLY THREAD SPENT RUNNING THIS FRAME IS
        // REPORTED (SetControlProgressSink). It used to be one untimed wait, so the poster - under
        // spawn, the control pump in ServerMain, the only thread that talks to the client - was mute
        // for as long as the dispatch ran, and a client could not tell a cold bring-up from a wedge.
        // "Running" is read under this lock from the mailbox's own fields: the pump clears the
        // shadow when it TAKES the frame (PumpControlRequest), and m_controlPending stays true until
        // the reply is published under this same lock. Posted-but-not-taken says nothing. The sink
        // runs without the lock, so a dispatch finishing meanwhile is not held up by the send; the
        // predicate is re-asked on the way back in. Only the timing of the wakeups changed: the
        // handshake, the C2 exit block's answer and the copy-out below are exactly as before.
        const auto postedAt = std::chrono::steady_clock::now();
        while (!m_controlDone.wait_for(lock, std::chrono::milliseconds(kControlProgressIntervalMs),
                                       [this] { return m_controlFinished; })) {
            const Bool running = m_controlPending && !m_controlPosted.load(std::memory_order_acquire);
            const ControlProgressSink sink = m_progressSink;
            void* const user = m_progressUser;
            if (!running || sink == nullptr) continue;
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - postedAt).count();
            lock.unlock();
            sink(user, frame.kind, frame.seq,
                 static_cast<Uint32>(std::min<long long>(elapsedMs, 0xFFFFFFFFll)));
            lock.lock();
        }
        frame = m_controlFrame; // the reply half, written by the dispatch
        return m_controlResult;
    }

    void ServerLoop::SetControlProgressSink(ControlProgressSink sink, void* user) {
        // Under the mailbox's lock, which the poster holds when it reads the pair, so a sink is
        // never called with another sink's user.
        const std::lock_guard<std::mutex> lock(m_controlMutex);
        m_progressSink = sink;
        m_progressUser = user;
    }

    MobileGLResult ServerLoop::RunProbeOnApplyThreadForTesting(ControlProbeHook hook, void* user) {
        SurfaceControlFrame frame;
        frame.kind = SurfaceControlOp::ProbeForTesting;
        frame.seq = m_controlSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
        // Nested probes never touch the shared pair: another poster may already be waiting,
        // and its arguments must survive a probe run inline on the apply thread.
        if (OnApplyThread()) return ApplySurfaceControlFrame(frame, hook, user);
        const std::lock_guard<std::mutex> callerLock(m_callerMutex);
        m_controlProbeHook.store(hook, std::memory_order_release);
        m_controlProbeUser.store(user, std::memory_order_release);
        return PostSurfaceControlFrameWithCallerLock(frame);
    }

    Uint64 ServerLoop::ControlFramesDispatched() const {
        return m_controlFramesDispatched.load(std::memory_order_acquire);
    }

    void ServerLoop::SignalExited() {
        {
            const std::lock_guard<std::mutex> lock(m_exitMutex);
            m_exited = true;
        }
        m_exitCv.notify_all();
    }

    void ServerLoop::AbandonQueuedRecords() { m_abandonQueue.store(true, std::memory_order_release); }

    void ServerLoop::Stop() {
        if (!m_thread.joinable()) {
            // Never started, or already stopped. The backend may still exist - the hook builds
            // it before the thread - and it has to go somewhere, so it goes here, on whatever
            // thread called Stop. No context was ever made current from another thread in that
            // case, which is exactly the condition that makes this safe.
            if (m_backend != nullptr) m_backend.reset();
            // P12 (D6): as on the apply thread's own exit - no thread runs, so nothing else ends it.
            ForgetServerWindow();
            MG_Pipe::MGPipeServerSetContextLive(false);
            // N-3, same reason as ApplyThreadMain's exit: no thread runs, so the apply-thread-only
            // rule on the tuple has no other writer to race.
            ForgetCurrentTuple();
            m_running.store(false, std::memory_order_release);
            return;
        }
        // As on the apply thread's own way out: the window lease ends once the thread has torn the
        // backend down, and a fresh session waits for it meanwhile.
        ServerDisplayInstance().MarkLeaseEnding(this);
        m_stopRequested.store(true, std::memory_order_release);
        if (m_session != nullptr) {
            // PH-6 fix round: the apply thread may be parked in ReserveEventOrBlock rather than in
            // the loop's own park, and that wait's predicate is the session's, not this loop's.
            // Raised BEFORE the ring below for the same publish-then-ring reason as the flag above;
            // without it a knob longer than kJoinTimeoutMs outlives this join.
            m_session->RequestApplyStop();
            // The bell may already be dead - ClientSession::Stop calls transport->Shutdown()
            // first, which is what table 3's step 2 requires - and Notify on a dead bell is
            // harmless. Ringing anyway covers the paths that Stop without a Kill.
            m_session->DataLink()->ConsumerBell().Notify();
        }

        // THE JOIN IS BOUNDED. std::thread::join has no deadline, so a lost wakeup would wedge
        // CI rather than fail it; the thread signals m_exited last and this waits with the same
        // five seconds InProcessTransportTest.cpp:344 uses.
        Bool exited = false;
        {
            std::unique_lock<std::mutex> lock(m_exitMutex);
            exited = m_exitCv.wait_for(lock, std::chrono::milliseconds(kJoinTimeoutMs),
                                       [this] { return m_exited; });
        }
        if (!exited && !m_applyParked.load(std::memory_order_acquire)) {
            // BUSY, NOT LOST (kBusyJoinTimeoutMs): the thread is applying, most likely blocked on
            // GPU work that has not finished. What is still queued behind it belongs to a peer
            // that is leaving, so it is not applied; the join waits for the work in hand.
            MGLOG_W("MG_Remote server: mgl-srv-apply is still busy in the backend %u ms after Stop() (a "
                    "GPU stall, not a lost wakeup); the queued records are abandoned and the join waits "
                    "up to %u ms",
                    kJoinTimeoutMs, kBusyJoinTimeoutMs);
            AbandonQueuedRecords();
            std::unique_lock<std::mutex> lock(m_exitMutex);
            exited = m_exitCv.wait_for(lock, std::chrono::milliseconds(kBusyJoinTimeoutMs - kJoinTimeoutMs),
                                       [this] { return m_exited; });
        }
        if (!exited) {
            // ABORT, NOT DETACH. A detached apply thread still owns the EGL context and would
            // run on into the client's teardown, reading rings the client is about to unmap -
            // a use-after-free whose only symptom is an intermittent crash somewhere else.
            // Aborting here is red, immediate, and names the cause.
            SessionFail(MGFatalFamily::ApplyThreadJoinTimeout, "MGPipe: Fatal{ApplyThreadJoinTimeout} - mgl-srv-apply did not exit within "
                    "%u ms of Stop(). Stop() published m_stopRequested (in the park predicate) "
                    "BEFORE it rang, so a plain Notify should already have un-parked the thread; "
                    "Doorbell::Kill() (Doorbell.h, CondVarDoorbell::Kill; table 3 step 2) is "
                    "the belt to that "
                    "Notify's braces. If the thread is still parked after both, the wakeup was "
                    "lost, not slow. Aborting rather than detaching: a detached apply thread still "
                    "owns the context and would read rings the client is about to unmap",
                    kJoinTimeoutMs);
        }
        m_thread.join();
        // The identity is NOT cleared here any more: the thread clears it itself, under
        // m_controlMutex, beside the m_running store it used to be paired with. Clearing it a
        // second time after the join would be harmless but would also be the second place a
        // reader has to check to know when the key stops meaning "the apply thread".
        m_running.store(false, std::memory_order_release);
        m_session = nullptr;
    }

    ServerLoop& ServerLoopInstance() {
        // P14 S2: the session of the CALLING THREAD, else the process's one instance. The client
        // process (the forwarders, whose sink makes them cross the wire), the inproc one-process
        // client+server shape and every unit case have no thread session and keep this instance.
        if (ServerLoop* const own = Detail::CurrentLoop()) return *own;
        // ID-8: leak at exit, like every MG_Remote singleton.
        static ServerLoop& instance = *new ServerLoop{};
        return instance;
    }

    // ---------------------------------------------------------------------------------
    // The control-frame dispatch and the twelve EGL forwarders (P5f, package fc)
    // ---------------------------------------------------------------------------------
    //
    // THE MAILBOX'S PAYLOAD CHANGED SHAPE, NOT ITS PROTOCOL. The channel is still one blocking
    // slot pumped between drain batches, with the shadow, the publish-then-ring doorbell and the
    // C2 exit block exactly as before; what is gone is the payload - a raw function pointer (the
    // op's identity) plus a void* to a stack-local Args (its arguments and out-pointers), neither
    // of which means anything in another address space (P5c audit row G4). The slot now carries
    // one SurfaceControlFrame BY VALUE: `kind` is the op, the scalar fields are the arguments,
    // and the reply half (ok / eglMajor / eglMinor) comes back in the same value. Nine of the
    // twelve forwarders map onto protocol.fbs's ten SurfaceOp kinds; the three without a wire
    // kind (f0-egl's census: present is a record, capabilities are a CapsSnapshot, and
    // InitWindowSurface has no caller) ride the same channel as inproc-only kinds rather than
    // keeping a second, pointer-shaped path alive for dead or answered code.

    namespace {

        // EGL handles are pointers on every platform in scope; the frame carries them as Uint64
        // tokens. Inproc the token IS the handle value bit-cast (same process, same driver), so
        // the round-trip is exact and the N-3 tuple bookkeeping - which compares handle VALUES -
        // is unchanged. Under spawn the client mints dense tokens instead (P6-CONTRACT-DRAFT
        // table 0); the field width is already the token's.
        template <class Handle>
        Uint64 TokenFromHandle(Handle handle) {
            static_assert(std::is_pointer_v<Handle>, "an EGL handle is a pointer");
            return static_cast<Uint64>(reinterpret_cast<std::uintptr_t>(handle));
        }
        template <class Handle>
        Handle HandleFromToken(Uint64 token) {
            static_assert(std::is_pointer_v<Handle>, "an EGL handle is a pointer");
            return reinterpret_cast<Handle>(static_cast<std::uintptr_t>(token));
        }

        // WindowHandle -> frame values (the forwarder side). The frame carries the BACKEND tag,
        // not the wire's WindowKind: the WindowKind mapping belongs to the wire codec
        // (Protocol/SurfaceOpCodec.cpp) and must not leak into the inproc path.
        void PackWindowHandle(SurfaceControlFrame& frame, const MG_Backend::WindowHandle& handle) {
            frame.windowBackend = static_cast<Int>(handle.Backend);
            frame.nativeToken = TokenFromHandle(handle.Handle);
            frame.width = static_cast<Int>(handle.Width);
            frame.height = static_cast<Int>(handle.Height);
        }

        // The dispatch-side range check. Inproc the value came from the poster's own
        // WindowHandle, so an out-of-range tag is a memory-corruption shape, not bad input -
        // same discipline as the ring's Fatal{ProtocolCorruption} on a header the producer could
        // not have written.
        //
        // PH-1 (3): false = the refusal latched (an armed spawn / TCP session child, where the
        // frame's values came off the wire through SurfaceOpCodec). Under spawn the codec has
        // already mapped the wire WindowKind onto a legal tag, so this arm is defence in depth
        // there; unarmed it still dies.
        Bool WindowBackendFromFrameValue(Int value, MG_Backend::WindowBackend* out) {
            if (value < static_cast<Int>(MG_Backend::WindowBackend::Unknown) ||
                value >= static_cast<Int>(MG_Backend::WindowBackend::WindowBackendCount)) {
                return SessionLatch(MGFatalFamily::ProtocolCorruption, "MGPipe: Fatal{ProtocolCorruption, \"SurfaceOp.windowBackend\"} - a "
                        "control frame carried window backend tag %d, which names no "
                        "WindowBackend", value);
            }
            *out = static_cast<MG_Backend::WindowBackend>(value);
            return true;
        }

        Bool UnpackWindowHandle(const SurfaceControlFrame& frame, MG_Backend::WindowHandle* out) {
            MG_Backend::WindowHandle handle;
            if (!WindowBackendFromFrameValue(frame.windowBackend, &handle.Backend)) return false;
            handle.Handle = HandleFromToken<void*>(frame.nativeToken);
            handle.Width = static_cast<Uint32>(frame.width);
            handle.Height = static_cast<Uint32>(frame.height);
            *out = handle;
            return true;
        }

        // P7 CI. THE TEST LEVER FOR THE CLIENT'S COLD-START BUDGET (ClientSession.cpp,
        // ControlReplyBudgetMs), in the style of MOBILEGL_TEST_DELAY_FIRST_CAPS_MS.
        //
        // MOBILEGL_TEST_DELAY_FIRST_BRINGUP_MS holds back this process's FIRST surface creation
        // and its FIRST MakeCurrent by that many milliseconds each - the two dispatches a lazy
        // native backend bring-up runs inside (Espryt's eglInitialize, Magma's instance and
        // device). It stands in for a cold software rasteriser on a loaded runner, where that
        // bring-up outlasted the client's steady reply bound and the retrace-split spawn legs died
        // with the server still alive. Only the reply is late; the op itself is unchanged.
        //
        // Meant for a server in its OWN process (spawn / tcp), which is where the client's reply
        // bound is. It is a TEST knob: it does nothing unless set, and is read once.
        void DelayFirstBringUpForTest(SurfaceControlOp kind) {
            static std::atomic<bool> surfaceSpent{false};
            static std::atomic<bool> makeCurrentSpent{false};
            std::atomic<bool>* spent = nullptr;
            if (kind == SurfaceControlOp::CreatePbufferSurface || kind == SurfaceControlOp::CreateWindowSurface) {
                spent = &surfaceSpent;
            } else if (kind == SurfaceControlOp::MakeCurrent) {
                spent = &makeCurrentSpent;
            } else {
                return;
            }
            static const long ms = [] {
                const char* text = std::getenv("MOBILEGL_TEST_DELAY_FIRST_BRINGUP_MS");
                return (text == nullptr || *text == '\0') ? 0L : std::strtol(text, nullptr, 10);
            }();
            if (ms <= 0 || spent->exchange(true, std::memory_order_acq_rel)) return;
            MGLOG_W("MG_Remote server: MOBILEGL_TEST_DELAY_FIRST_BRINGUP_MS=%ld - the reply to the "
                    "first %s is held back by that many milliseconds. This is the cold-start "
                    "budget's test-only lever and must never be set in a measured run",
                    ms, SurfaceControlOpName(kind));
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }

    } // namespace

    MobileGLResult ServerLoop::ApplySurfaceControlFrame(SurfaceControlFrame& frame,
                                                        ControlProbeHook probeHook, void* probeUser) {
        // The frame channel's own tally, bumped for EVERY dispatch including probes: a forwarder
        // that stopped posting frames (the fc red-once revert shape) leaves this unmoved, which
        // is what ServerLoopTest's AVoidForwarderCrossesAsOneDispatchedFrame reads.
        m_controlFramesDispatched.fetch_add(1, std::memory_order_acq_rel);
        if (frame.kind == SurfaceControlOp::ProbeForTesting) {
            if (probeHook == nullptr) return MOBILEGL_ERR_INVALID_ARGUMENT;
            return probeHook(probeUser);
        }
        // Null means the hook never ran or the session is already down. Every op answers
        // NOT_INITIALIZED rather than dereferencing: a client that calls eglMakeCurrent on a
        // process whose split bring-up failed must see a refusal, not a crash.
        MG_Backend::BackendObject* backend = m_backend.get();
        if (backend == nullptr) return MOBILEGL_ERR_NOT_INITIALIZED;
        DelayFirstBringUpForTest(frame.kind);
        switch (frame.kind) {
        case SurfaceControlOp::InitializeDisplay: {
            // The old Args carried the poster's `major`/`minor` OUT-POINTERS; here they are
            // reply fields. A pointer to the caller's stack is exactly what does not survive a
            // process boundary, so the version numbers travel back in the frame's value.
            EGLint major = 0;
            EGLint minor = 0;
            frame.ok = backend->InitializeEGLDisplay(HandleFromToken<EGLDisplay>(frame.display),
                                                     &major, &minor);
            frame.eglMajor = major;
            frame.eglMinor = minor;
            return MOBILEGL_OK;
        }
        case SurfaceControlOp::CreateWindowSurface: {
            // P12 (D3): the SERVER's window, asked for by a headless client. Before the unpack,
            // which would refuse the frame-local tag as a WindowBackend out of range.
            if (frame.windowBackend == kServerOwnedWindowBackend) return ApplyServerOwnedWindowSurface(backend, frame);
            MG_Backend::WindowHandle window;
            if (!UnpackWindowHandle(frame, &window)) {
                frame.ok = false;
                return MOBILEGL_ERR_PROTOCOL_MISMATCH; // PH-1 (3): latched
            }
            frame.ok = backend->CreateEGLWindowSurface(HandleFromToken<EGLSurface>(frame.surface),
                                                       window);
            // N-3: BackendObject_DirectGLES destroys and recreates the native context to create
            // a DIFFERENT surface, so whatever tuple was bound names a dead context.
            if (frame.ok) ForgetCurrentTuple();
            return MOBILEGL_OK;
        }
        case SurfaceControlOp::ResizeWindowSurface:
            // P12 review fix: a server-owned surface is resized by resizing the SERVER's window.
            if (IsServerOwnedSurface(HandleFromToken<EGLSurface>(frame.surface)))
                return ApplyServerOwnedWindowResize(backend, frame);
            frame.ok = backend->ResizeEGLWindowSurface(HandleFromToken<EGLSurface>(frame.surface),
                                                       static_cast<Uint32>(frame.width),
                                                       static_cast<Uint32>(frame.height));
            return MOBILEGL_OK;
        case SurfaceControlOp::CreatePbufferSurface: {
            // A surfaceless stand-in (kPbufferFlagSurfacelessStandIn) is no rendering path: it is
            // admitted whatever the mode and latches none, so a client that binds surfacelessly
            // before it creates its server-owned window still gets that window.
            const Bool standIn = (frame.contextFlags & kPbufferFlagSurfacelessStandIn) != 0;
            // P12 (D4): a pbuffer in a session that went on-screen is the other mode, refused by
            // name and not latched - the session and its window surface carry on. P14 S2: the mode
            // is the session's own - latched by its first surface, on its own loop - and never
            // another session's.
            if (!standIn && !SessionSurfaceModeAdmits(m_surfaceMode, /*serverOwnedWindow=*/false)) {
                frame.ok = false;
                frame.refusal = static_cast<Uint8>(SurfaceRefusalCode::SurfaceModeMismatch);
                MGLOG_E("MG_Remote server: SurfaceModeMismatch - CreatePbufferSurface (seq %llu, %dx%d) in a "
                        "session whose surface mode is %s: its first surface was a server-owned window, and "
                        "only one rendering path is active per session. Refused, the session is not latched",
                        static_cast<unsigned long long>(frame.seq), frame.width, frame.height,
                        SessionSurfaceModeName(m_surfaceMode));
                return MOBILEGL_ERR_INVALID_ARGUMENT;
            }
            frame.ok = backend->CreateEGLPbufferSurface(HandleFromToken<EGLSurface>(frame.surface),
                                                        frame.width, frame.height);
            // N-3: as for the window surface - a (re)creation may have destroyed the context the
            // held tuple named. The surface's own creation binds natively, so the client's
            // make-current that follows is forwarded and deduped one layer down (ID-54).
            if (frame.ok) {
                ForgetCurrentTuple();
                if (standIn) {
                    MGLOG_I("MG_Remote server: surface=pbuffer %dx%d (surfaceless stand-in; the session's surface "
                            "mode stays %s)",
                            frame.width, frame.height, SessionSurfaceModeName(m_surfaceMode));
                    return MOBILEGL_OK;
                }
                if (m_surfaceMode == SessionSurfaceMode::None) m_surfaceMode = SessionSurfaceMode::Offscreen;
                // P12: THE ARM PROOF (ID-124), once per surface: which of the two paths ran.
                MGLOG_I("MG_Remote server: surface=pbuffer %dx%d", frame.width, frame.height);
            }
            return MOBILEGL_OK;
        }
        case SurfaceControlOp::MakeCurrent: {
            // C7 / ID-54: the apply thread binds the native context ONCE per context lifetime
            // and holds it for life. ApplyMakeCurrent forwards a bind only for a tuple it does
            // not hold and treats an identical repeat as a no-op; the native call for a surface
            // already current on this thread is skipped one layer down
            // (BackendObject_DirectGLES's ID-54 arm), which is what makes "the owner slot is
            // written once" TRUE and measured (ServerLoopTest's C7 control) rather than claimed.
            //
            // P14 MULTI-WINDOW: the frame names the client's CONTEXT TOKEN (BackendObject_Remote's
            // MakeEGLCurrent), and the native bind below resolves the context through the session's
            // current token - which still names whatever another thread bound last, since this
            // thread's own bind_context follows the frame. Bound first, so the surface is made
            // current with ITS context and not attached to a neighbour's. A frame from a client
            // that sends a handle names no token and leaves the binding alone.
            if (m_session != nullptr && frame.context != 0) (void)m_session->BindContext(frame.context);
            const MakeCurrentOutcome outcome =
                ApplyMakeCurrent(backend, HandleFromToken<EGLDisplay>(frame.display),
                                 HandleFromToken<EGLSurface>(frame.surface),
                                 HandleFromToken<EGLSurface>(frame.readSurface),
                                 HandleFromToken<EGLContext>(frame.context));
            frame.ok = outcome.ok;
            if (!outcome.ok || !outcome.boundNatively) return MOBILEGL_OK;
            // R-12, arm (a): the caps snapshot is REPUBLISHED because InitCapabilities has now
            // run for real - on every forwarded bind of a tuple this loop did not hold (the
            // first, and every DIFFERENT tuple after it), and NEVER on an identical repeat
            // (ID-67: a repeat is a native no-op AND publishes nothing, so the client's mirror
            // generation does not move and nothing accumulates). DirectGLES has no
            // OnCapsInvalidated producer at all, and c0's answer is that a SECOND arrival IS the
            // invalidation - so the client's mirror is refreshed with no dev-shaped backend edit
            // and with no eleventh MGPipeCallbacks slot (MGPipeCallbacks.h:56-58's static_assert
            // exists to make that cost visible). Red once by republishing only on the first
            // bind: the ID-67 control's different tuple reads 1 republish where 2 are required.
            ServerSession* session = ServerSession::Active();
            if (session != nullptr && session->Accepted()) {
                // P11 B2: THE T0 GRANT, HERE AND NOWHERE EARLIER - the first moment this session has
                // a live context (Magma: a device) to run the POST self-test on, and before the
                // snapshot below, which is the one the client's make-current adopts. Once per
                // session; a session that did not ask T0 does nothing.
                session->SettleAdoptT0AtBind();
                const MobileGLResult published = session->PublishCapsSnapshot();
                if (published == MOBILEGL_OK) NoteMakeCurrentRepublished();
                if (published != MOBILEGL_OK) {
                    MGLOG_E("MG_Remote server: the post-make-current CapsSnapshot could not be "
                            "published (rc=%d); the client's mirror still holds the empty "
                            "snapshot Accept() sent before any context existed",
                            static_cast<int>(published));
                }
            }
            return MOBILEGL_OK;
        }
        case SurfaceControlOp::ReleaseCurrent: {
            // The release request's own op on the wire. It reaches the same ClientRelease arm of
            // ApplyMakeCurrent as before (recorded, NOT forwarded - ID-54: the context stays
            // current on this thread until ~BackendObject_DirectGLES or context loss, so
            // forwarding would unbind and reinstate the off-thread degradation the phase
            // removes). A client release never republishes: nothing ran that could move caps.
            const MakeCurrentOutcome outcome =
                ApplyMakeCurrent(backend, HandleFromToken<EGLDisplay>(frame.display),
                                 EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            frame.ok = outcome.ok;
            return MOBILEGL_OK;
        }
        case SurfaceControlOp::CreateContext: {
            // P14 S1 built the session's table entry; P14 S4 gives it its NATIVE context. The
            // share group is what makes two client contexts see one set of buffer/texture objects:
            // a group whose leader already has a native EGLContext hands it to eglCreateContext as
            // the share argument, so this arm is the whole of "server-side sharing".
            //
            // An `ok = false` here is a named refusal the client's eglCreateContext turns into
            // EGL_NO_CONTEXT, which is what a server that declined to table the token must produce -
            // a token the server never heard of is worse than no context at all. A native context
            // that could not be built rolls the table entry back, or the session would hold a token
            // whose every MakeCurrent fails one layer down.
            ServerSession* session = ServerSession::Active();
            if (session == nullptr) {
                frame.ok = false;
                return MOBILEGL_ERR_NOT_INITIALIZED;
            }
            frame.ok = session->CreateContext(frame.context, frame.shareGroupToken, frame.contextFlags);
            if (frame.ok && backend != nullptr &&
                backend->GetBackendType() == BackendType::DirectGLES) {
                if (!MG_Backend::DirectGLES::CreateNativeContextFor(frame.context, frame.shareGroupToken)) {
                    (void)session->DestroyContext(frame.context);
                    frame.ok = false;
                }
            }
            return MOBILEGL_OK;
        }
        case SurfaceControlOp::DestroyContext: {
            ServerSession* session = ServerSession::Active();
            if (session == nullptr) {
                frame.ok = false;
                return MOBILEGL_ERR_NOT_INITIALIZED;
            }
            frame.ok = session->DestroyContext(frame.context);
            // P14 S4: the native context goes with the entry, and ONLY that one - the session's
            // other contexts, and every other session's, are untouched. The display is terminated
            // only when the last native context in the process has gone.
            if (frame.ok && backend != nullptr &&
                backend->GetBackendType() == BackendType::DirectGLES) {
                MG_Backend::DirectGLES::DestroyNativeContextFor(frame.context);
            }
            return MOBILEGL_OK;
        }
        case SurfaceControlOp::SetSwapInterval:
            backend->SetEGLSwapInterval(frame.swapInterval);
            frame.ok = true;
            return MOBILEGL_OK;
        case SurfaceControlOp::ReleaseSurface: {
            const EGLSurface surface = HandleFromToken<EGLSurface>(frame.surface);
            backend->ReleaseEGLSurface(surface);
            ForgetServerOwnedSurface(surface);
            // N-3: a released surface the held tuple names may have taken the context with it
            // (BackendObject::ReleaseEGLSurface -> OnEGLSurfaceReleased -> DestroyEGLContext once
            // nothing holds it current). Forgetting when the base class only DEFERRED the destroy
            // costs one forwarded bind; remembering when it did not would cost a silent
            // no-context-current.
            ForgetCurrentTupleIfItNames(surface);
            frame.ok = true;
            return MOBILEGL_OK;
        }
        case SurfaceControlOp::ReleaseResources:
            // BLOCKING BY CONTRACT (the forwarder's own comment, below): EGLImpl.cpp's teardown
            // walks into MobileGL::Destroy() the moment this answers. For DirectGLES this runs
            // DestroyEGLContext - eglMakeCurrent(NO_SURFACE), eglDestroyContext, eglTerminate -
            // on the thread that made the context current.
            backend->ReleaseEGLResources();
            MG_Pipe::MGPipeServerSetContextLive(false);
            // N-3: DestroyEGLContext just ran; the tuple names nothing. Without this a
            // destroy-recreate with the same handle values (every EGL handle on this host is
            // 0x1) classified as a RepeatNoOp, bound nothing, and republished no caps. Red once
            // by deleting it: ServerLoopTest's recreate control reads NativeBindCount() == 1
            // where 2 is required.
            ForgetCurrentTuple();
            frame.ok = true;
            return MOBILEGL_OK;
        case SurfaceControlOp::SetWindowHandle: {
            MG_Backend::WindowHandle window;
            if (!UnpackWindowHandle(frame, &window)) {
                frame.ok = false;
                return MOBILEGL_ERR_PROTOCOL_MISMATCH; // PH-1 (3): latched
            }
            backend->SetWindowHandle(window);
            frame.ok = true;
            return MOBILEGL_OK;
        }
        case SurfaceControlOp::InitCapabilities:
            // Inproc-only (f0-egl §4.2): on the wire the ANSWER to this op is the CapsSnapshot
            // frame itself, so there is no SurfaceOp kind for it. The frame channel still has to
            // carry it inproc because InitCapabilities runs GL queries and therefore belongs to
            // the apply thread.
            frame.ok = backend->InitCapabilities();
            if (frame.ok) {
                ServerSession* session = ServerSession::Active();
                if (session != nullptr && session->Accepted()) {
                    session->SettleAdoptT0AtBind(); // P11 B2: idempotent; the bind has usually settled it
                    (void)session->PublishCapsSnapshot();
                }
            }
            return MOBILEGL_OK;
        case SurfaceControlOp::SwapBuffersInprocOnly:
            // Inproc-only (f0-egl F8): present travels as a class-B record with its own credit;
            // this forwarder has no production caller and survives for ServerLoopTest's N-3
            // recreate control, which needs a swap that is NOT a record.
            frame.ok = backend->SwapEGLBuffers(HandleFromToken<EGLDisplay>(frame.display),
                                               HandleFromToken<EGLSurface>(frame.surface));
            return MOBILEGL_OK;
        case SurfaceControlOp::InitWindowSurfaceInprocOnly:
            // Inproc-only (f0-egl F8): no caller at all (the client's InitWindowSurface is a
            // no-op); kept because deleting the free function would delete the seam the
            // inproc-only rule is stated against.
            frame.ok = backend->InitWindowSurface();
            return MOBILEGL_OK;
        case SurfaceControlOp::None:
        case SurfaceControlOp::ProbeForTesting:
            // Unreachable in a correct process (None is refused at post, the probe is answered
            // above the switch); a slot that still arrives here with one shares the Fatal below.
            break;
        }
        // A kind the switch does not know never legitimately posts (RunSurfaceControlFrame
        // refuses None), so reaching this line means the slot's bytes were not written by the
        // poster at all - the ring's Fatal{ProtocolCorruption} discipline, one channel over.
        // PH-1 (3): latched in an armed session child (under spawn SurfaceOpCodec has already
        // refused an unknown wire kind, so this is defence in depth); unarmed it dies.
        frame.ok = false;
        (void)SessionLatch(MGFatalFamily::ProtocolCorruption, "MGPipe: Fatal{ProtocolCorruption, \"SurfaceOp.kind\"} - the control slot held "
                "kind %d, which names no dispatchable op", static_cast<int>(frame.kind));
        return MOBILEGL_ERR_PROTOCOL_MISMATCH;
    }

    // ---------------------------------------------------------------------------------
    // P12 (on-screen server window): the ServerOwned arm, the surface mode, the lost window
    // ---------------------------------------------------------------------------------

    const char* SessionSurfaceModeName(SessionSurfaceMode mode) {
        switch (mode) {
        case SessionSurfaceMode::None: return "undecided";
        case SessionSurfaceMode::OnScreen: return "on-screen";
        case SessionSurfaceMode::Offscreen: return "offscreen";
        }
        return "<unknown SessionSurfaceMode>";
    }

    Bool SessionSurfaceModeAdmits(SessionSurfaceMode current, Bool serverOwnedWindow) {
        switch (current) {
        case SessionSurfaceMode::None: return true;
        case SessionSurfaceMode::OnScreen: return serverOwnedWindow;
        case SessionSurfaceMode::Offscreen: return !serverOwnedWindow;
        }
        return false;
    }

    void ServerLoop::ServerWindowLostThunk(void* self) {
        auto* loop = static_cast<ServerLoop*>(self);
        // Publish, then ring (the park predicate reads the flag; Doorbell.h's order). Called under
        // the display's lock while this loop holds its lease - and the lease ends before the apply
        // thread exits and before Stop() lets go of m_session - so the session is still here.
        loop->m_windowRequests.fetch_or(kWindowLost, std::memory_order_acq_rel);
        if (loop->m_session != nullptr && loop->m_session->DataLink() != nullptr) {
            loop->m_session->DataLink()->ConsumerBell().Notify();
        }
    }

    void ServerLoop::ServerWindowAttachedThunk(void* self) {
        auto* loop = static_cast<ServerLoop*>(self);
        // As the lost hook. Called under the display's lock while this loop is registered as suspended,
        // and the registration is cancelled (under that lock) before the apply thread exits and before
        // Stop() lets go of m_session.
        loop->m_windowRequests.fetch_or(kWindowAttached, std::memory_order_acq_rel);
        if (loop->m_session != nullptr && loop->m_session->DataLink() != nullptr) {
            loop->m_session->DataLink()->ConsumerBell().Notify();
        }
    }

    void ServerLoop::ServerWindowResizedThunk(void* self) {
        auto* loop = static_cast<ServerLoop*>(self);
        // As the lost hook: called under the display's lock while this loop holds the lease.
        loop->m_windowRequests.fetch_or(kWindowResized, std::memory_order_acq_rel);
        if (loop->m_session != nullptr && loop->m_session->DataLink() != nullptr) {
            loop->m_session->DataLink()->ConsumerBell().Notify();
        }
    }

    Bool ServerLoop::ServerWindowWaitCancelled(void* self) {
        const auto* loop = static_cast<const ServerLoop*>(self);
        // A window-lost request also ends the wait: this thread holds a lease on a window that is
        // being destroyed, and the UI thread is blocked until it answers (PumpControlRequest).
        return loop->m_stopRequested.load(std::memory_order_acquire) || SessionLatched() ||
               (loop->m_windowRequests.load(std::memory_order_acquire) & kWindowLost) != 0;
    }

    MobileGLResult ServerLoop::AcquireServerWindow(Uint32 width, Uint32 height, Uint32 timeoutMs,
                                                   ServerWindowLease* out, SurfaceRefusalCode* refusal) {
        ServerDisplay& display = ServerDisplayInstance();
        ServerWindowLease local;
        ServerWindowLease* const lease = out != nullptr ? out : &local;
        ServerWindowAcquire acquired = ServerWindowAcquire::NoDisplay;
        {
            const ScopedBackendTurnYield yield;
            acquired = display.AcquireFor(width, height, timeoutMs, this, &ServerWindowLostThunk,
                                          &ServerWindowWaitCancelled, this, lease, &ServerWindowResizedThunk);
        }
        switch (acquired) {
        case ServerWindowAcquire::Acquired:
            m_holdsWindowLease = true;
            m_leasedWindow = lease->window;
            return MOBILEGL_OK;
        case ServerWindowAcquire::NoDisplay:
            // A CONFIGURATION ANSWER, NOT A LATCH: the client asked an offscreen server (the exec'd
            // supervisor's session child, or any host server) for a window it does not have.
            if (refusal != nullptr) *refusal = SurfaceRefusalCode::NoServerDisplay;
            MGLOG_E("MG_Remote server: Refuse ServerOwned: this server owns no display - a client "
                    "(MOBILEGL_IPC_SURFACE=server) asked for a %ux%u window surface on the server's own window, "
                    "and this process is an offscreen server. Start the on-screen display server, or run the "
                    "client offscreen. Refused (NoServerDisplay); the session is not latched",
                    width, height);
            return MOBILEGL_ERR_UNSUPPORTED;
        case ServerWindowAcquire::NoWindow:
        case ServerWindowAcquire::Cancelled:
        case ServerWindowAcquire::Interrupted:
        case ServerWindowAcquire::LeasedElsewhere:
            break;
        }
        if (refusal != nullptr) *refusal = SurfaceRefusalCode::NoServerWindow;
        // P14 S2: LeasedElsewhere IS NOT A BUG ANY MORE. The display's window has one holder at a
        // time and a session per holder, so a second ON-SCREEN session asking for it is refused -
        // by name, not latched, exactly as a display with no window up. Offscreen (pbuffer)
        // sessions take no lease at all and are unaffected.
        MGLOG_E("MG_Remote server: Refuse ServerOwned: this server owns a display but no window for a %ux%u surface "
                "(%s after at most %u ms)%s. Refused (NoServerWindow); the session is not latched",
                width, height, ServerWindowAcquireName(acquired), timeoutMs,
                acquired == ServerWindowAcquire::LeasedElsewhere
                    ? " - the server's window is leased by another session, and one on-screen session at a time is "
                      "the display's own rule"
                    : " - the display's surface is not up");
        return MOBILEGL_ERR_TIMEOUT;
    }

    MobileGLResult ServerLoop::ApplyServerOwnedWindowSurface(MG_Backend::BackendObject* backend,
                                                            SurfaceControlFrame& frame) {
        frame.ok = false;
        frame.refusal = static_cast<Uint8>(SurfaceRefusalCode::None);
        const Uint32 wantWidth = frame.width > 0 ? static_cast<Uint32>(frame.width) : 0u;
        const Uint32 wantHeight = frame.height > 0 ? static_cast<Uint32>(frame.height) : 0u;
        // D4 FIRST: an offscreen session may not go on-screen, display or no display. P14 S2: the
        // refusal is against THIS session's own latch - a second on-screen session in the same
        // process is a different refusal (no free window), never a mode mismatch.
        if (!SessionSurfaceModeAdmits(m_surfaceMode, /*serverOwnedWindow=*/true)) {
            frame.refusal = static_cast<Uint8>(SurfaceRefusalCode::SurfaceModeMismatch);
            MGLOG_E("MG_Remote server: SurfaceModeMismatch - a ServerOwned CreateWindowSurface (seq %llu, %ux%u) in a "
                    "session whose surface mode is %s: its first surface was a pbuffer, and only one rendering "
                    "path is active per session. Refused, the session is not latched",
                    static_cast<unsigned long long>(frame.seq), wantWidth, wantHeight,
                    SessionSurfaceModeName(m_surfaceMode));
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        ServerWindowLease lease;
        SurfaceRefusalCode refusal = SurfaceRefusalCode::None;
        const MobileGLResult acquired = AcquireServerWindow(wantWidth, wantHeight, kServerWindowWaitMs, &lease, &refusal);
        if (acquired != MOBILEGL_OK) {
            frame.refusal = static_cast<Uint8>(refusal);
            return acquired;
        }
        m_serverWindowWantWidth = wantWidth;
        m_serverWindowWantHeight = wantHeight;
        // A SUSPENDED session that creates a surface on the new window first gets back the ones it had.
        if (m_serverWindowSuspended.load(std::memory_order_acquire) && !ResumeServerWindowOnLease(lease)) {
            return MOBILEGL_OK; // latched by name
        }
        // THE SUBSTITUTION. The client named no window; the server's own goes into the SAME backend
        // call monolith Android makes (Register -> Activate -> SetWindowHandle -> InitWindowSurface),
        // so nothing below this line knows the window was not the client's. Android is the only
        // platform that installs a display (the host never does), hence the backend tag.
        MG_Backend::WindowHandle window;
        window.Backend = MG_Backend::WindowBackend::Android;
        window.Handle = lease.window;
        window.Width = lease.width;
        window.Height = lease.height;
        // Review fix: the backend is told which window is the server's own, so its surface init
        // reports the extent for this window and for no window a client named (MGPipeServerOwnedWindow).
        NoteServerWindowExtent(lease.window, lease.width, lease.height);
        const ServerOwnedSurfaceHook surfaceHook = g_serverOwnedSurfaceHook.load(std::memory_order_acquire);
        const EGLSurface ownedSurface = HandleFromToken<EGLSurface>(frame.surface);
        frame.ok = surfaceHook != nullptr ? surfaceHook(backend, ownedSurface, window)
                                          : backend->CreateEGLWindowSurface(ownedSurface, window);
        if (!frame.ok) {
            MGLOG_E("MG_Remote server: the backend could not create a window surface on the server window %p "
                    "(%ux%u) for ServerOwned seq %llu",
                    lease.window, lease.width, lease.height, static_cast<unsigned long long>(frame.seq));
            // A session that never got a surface on the window has nothing to release: the lease goes.
            if (m_surfaceMode != SessionSurfaceMode::OnScreen) EndServerWindowLease();
            return MOBILEGL_OK;
        }
        // N-3: as for any window surface - a (re)creation may have destroyed the held context.
        ForgetCurrentTuple();
        m_surfaceMode = SessionSurfaceMode::OnScreen;
        if (!IsServerOwnedSurface(HandleFromToken<EGLSurface>(frame.surface)))
            m_serverOwnedSurfaces.push_back(HandleFromToken<EGLSurface>(frame.surface));
        // The reply's geometry (SurfaceReply.width/height): the window's real extent, which is what
        // the headless client's eglQuerySurface answers from here on.
        frame.width = static_cast<Int>(lease.width);
        frame.height = static_cast<Int>(lease.height);
        // THE ARM PROOF (ID-124), once per surface: which of the two paths ran, and at what size.
        MGLOG_I("MG_Remote server: surface=window %ux%u owner=server (ServerOwned seq %llu, window %p generation %llu%s)",
                lease.width, lease.height, static_cast<unsigned long long>(frame.seq), lease.window,
                static_cast<unsigned long long>(lease.generation),
                lease.sizeAsRequested ? "" : ", NOT the size the client asked for");
        return MOBILEGL_OK;
    }

    Bool ServerLoop::IsServerOwnedSurface(EGLSurface surface) const {
        return std::find(m_serverOwnedSurfaces.begin(), m_serverOwnedSurfaces.end(), surface) !=
               m_serverOwnedSurfaces.end();
    }

    void ServerLoop::ForgetServerOwnedSurface(EGLSurface surface) {
        m_serverOwnedSurfaces.erase(std::remove(m_serverOwnedSurfaces.begin(), m_serverOwnedSurfaces.end(), surface),
                                    m_serverOwnedSurfaces.end());
    }

    // P12 review fix (client resize). eglResize on a headless client's server-owned surface used to
    // resize only the backend's record of the window - the phone's SurfaceView kept its size - and
    // the client's EGL state answered the size it ASKED for. The window is the server's, so the
    // resize is a geometry request like the creation's: the display is asked for the size and waited
    // for (the same lease, re-taken by its holder), the backend surface follows the window's REAL
    // extent, and that extent is the reply's width/height - which the client adopts.
    MobileGLResult ServerLoop::ApplyServerOwnedWindowResize(MG_Backend::BackendObject* backend,
                                                           SurfaceControlFrame& frame) {
        frame.ok = false;
        frame.refusal = static_cast<Uint8>(SurfaceRefusalCode::None);
        const Uint32 wantWidth = frame.width > 0 ? static_cast<Uint32>(frame.width) : 0u;
        const Uint32 wantHeight = frame.height > 0 ? static_cast<Uint32>(frame.height) : 0u;
        if (m_serverWindowSuspended.load(std::memory_order_acquire)) {
            // NO WINDOW TO RESIZE, AND NONE TO WAIT FOR: the screen is off. The size is what the resume
            // asks the next window for, and the extent the client adopts then is published by the
            // backend's rebuild. Answered at once - waiting here would hold the apply thread for the
            // whole of AcquireServerWindow's bound and refuse anyway.
            m_serverWindowWantWidth = wantWidth;
            m_serverWindowWantHeight = wantHeight;
            frame.ok = true;
            MGLOG_I("MG_Remote server: surface=window resize to %ux%u while suspended (ServerOwned seq %llu) - kept "
                    "for the next window",
                    wantWidth, wantHeight, static_cast<unsigned long long>(frame.seq));
            return MOBILEGL_OK;
        }
        ServerWindowLease lease;
        SurfaceRefusalCode refusal = SurfaceRefusalCode::None;
        const MobileGLResult acquired = AcquireServerWindow(wantWidth, wantHeight, kServerWindowWaitMs, &lease, &refusal);
        if (acquired != MOBILEGL_OK) {
            frame.refusal = static_cast<Uint8>(refusal);
            return acquired;
        }
        m_serverWindowWantWidth = wantWidth;
        m_serverWindowWantHeight = wantHeight;
        NoteServerWindowExtent(lease.window, lease.width, lease.height);
        frame.ok = backend->ResizeEGLWindowSurface(HandleFromToken<EGLSurface>(frame.surface), lease.width, lease.height);
        frame.width = static_cast<Int>(lease.width);
        frame.height = static_cast<Int>(lease.height);
        MGLOG_I("MG_Remote server: surface=window %ux%u owner=server resized (ServerOwned seq %llu, %ux%u requested%s)",
                lease.width, lease.height, static_cast<unsigned long long>(frame.seq), wantWidth, wantHeight,
                lease.sizeAsRequested ? "" : ", NOT reached");
        return MOBILEGL_OK;
    }

    void ServerLoop::EndServerWindowLease() {
        if (!m_holdsWindowLease) return;
        m_holdsWindowLease = false;
        m_leasedWindow = nullptr;
        NoteServerWindowExtent(nullptr, 0, 0);
        ServerDisplayInstance().EndLease(this);
    }

    void ServerLoop::ForgetServerWindow() {
        EndServerWindowLease();
        // Under the display's lock: no attached hook names this loop once it returns.
        ServerDisplayInstance().CancelResume(this);
        m_serverWindowSuspended.store(false, std::memory_order_release);
    }

    void ServerLoop::HandleServerWindowRequests() {
        // Both bits at once: a lost request is answered first (the UI thread is blocked on it), and a
        // new window after it. A request posted after this exchange is the next check's.
        const Uint32 requests = m_windowRequests.exchange(0, std::memory_order_acq_rel);
        if ((requests & kWindowLost) != 0) ReleaseLostServerWindow();
        if ((requests & kWindowAttached) != 0 && !SessionLatched()) ResumeSuspendedServerWindow();
        if ((requests & kWindowResized) != 0 && !SessionLatched()) PublishServerWindowResize();
    }

    void ServerLoop::NoteServerWindowExtent(void* window, Uint32 width, Uint32 height) {
        MG_Pipe::MGPipeServerSetOwnedWindow(window);
        MG_Pipe::MGPipeServerSetOwnedWindowExtent(width, height);
        m_publishedWindowWidth = width;
        m_publishedWindowHeight = height;
    }

    void ServerLoop::PublishServerWindowResize() {
        if (!m_holdsWindowLease || m_backend == nullptr) return;
        // THE DISPLAY'S EXTENT, READ NOW: the latest report wins over any a burst of them left queued.
        void* window = nullptr;
        Uint32 width = 0;
        Uint32 height = 0;
        if (!ServerDisplayInstance().WindowExtent(this, &window, &width, &height) || window != m_leasedWindow ||
            width == 0 || height == 0)
            return;
        if (width == m_publishedWindowWidth && height == m_publishedWindowHeight) return;
        const Uint32 previousWidth = m_publishedWindowWidth;
        const Uint32 previousHeight = m_publishedWindowHeight;
        NoteServerWindowExtent(window, width, height);
        const Bool published = m_backend->ServerWindowResized(window, width, height);
        if (published) m_serverWindowResizes.fetch_add(1, std::memory_order_acq_rel);
        MGLOG_I("MG_Remote server: server window %p resized %ux%u -> %ux%u; %s", window, previousWidth, previousHeight,
                width, height,
                published ? "the new extent is published to the client"
                          : "the backend published nothing (no surface on the window, or it follows at its present)");
    }

    void ServerLoop::LatchServerWindowLost(const char* why) {
        if (m_backend != nullptr) m_backend->ReleaseEGLResources();
        MG_Pipe::MGPipeServerSetContextLive(false);
        ForgetCurrentTuple();
        m_serverWindowSuspended.store(false, std::memory_order_release);
        m_serverWindowsLost.fetch_add(1, std::memory_order_acq_rel);
        (void)SessionLatch(MGFatalFamily::ServerWindowLost,
                           "MGPipe: Fatal{ServerWindowLost, \"surfaceDestroyed\"} - %s, and the session ends so the "
                           "client reads a clean device loss. The display server keeps listening",
                           why);
        EndServerWindowLease();
    }

    void ServerLoop::ReleaseLostServerWindow() {
        if (!m_holdsWindowLease) return; // raced with the session's own end; nothing is held
        // THE ORDER IS THE CONTRACT (D6): the backend lets go of the window first, and only then does
        // the lease end, which is what lets surfaceDestroyed return.
        //
        // SUSPEND, NOT END. The window going is the screen going off (or the lock screen), not the
        // session's end: a backend that can drops only what is bound to the window - Espryt its EGL
        // window surfaces, Magma the swapchain and VkSurfaceKHR - keeps its contexts, objects and
        // device, and the session waits for the display's next window with every record still applied.
        void* const window = m_leasedWindow;
        if (m_backend != nullptr && m_backend->SuspendServerWindow(window)) {
            // N-3: the native binding moved off the window's surface.
            ForgetCurrentTuple();
            m_serverWindowSuspended.store(true, std::memory_order_release);
            const Uint64 suspends = m_serverWindowsSuspended.fetch_add(1, std::memory_order_acq_rel) + 1;
            m_holdsWindowLease = false;
            m_leasedWindow = nullptr;
            NoteServerWindowExtent(nullptr, 0, 0);
            MGLOG_I("MG_Remote server: server window %p lost - the on-screen session is SUSPENDED, not ended: what "
                    "was bound to the window was released on mgl-srv-apply, its surfaces draw into an offscreen "
                    "placeholder, and it resumes on the next window (suspend #%llu)",
                    window, static_cast<unsigned long long>(suspends));
            // Ends the lease - surfaceDestroyed returns - and waits for the next window, in one step.
            ServerDisplayInstance().SuspendLease(this, &ServerWindowAttachedThunk);
            return;
        }
        LatchServerWindowLost("the server's own display window was destroyed under this on-screen session, whose "
                              "backend cannot suspend; its EGL resources were released on mgl-srv-apply before the "
                              "window went");
    }

    void ServerLoop::ResumeSuspendedServerWindow() {
        if (!m_serverWindowSuspended.load(std::memory_order_acquire) || m_holdsWindowLease) return;
        // The geometry the session last asked for, asked again (the window is new; a fixed size may not
        // have survived it). Bounded by the geometry grace: the window IS attached - that is what woke
        // this - so only its size is waited for.
        ServerWindowLease lease;
        ServerWindowAcquire acquired = ServerWindowAcquire::NoDisplay;
        {
            const ScopedBackendTurnYield yield;
            acquired = ServerDisplayInstance().AcquireFor(m_serverWindowWantWidth, m_serverWindowWantHeight,
                                                          ServerDisplay::kGeometryGraceMs, this, &ServerWindowLostThunk,
                                                          &ServerWindowWaitCancelled, this, &lease,
                                                          &ServerWindowResizedThunk);
        }
        if (acquired != ServerWindowAcquire::Acquired) {
            // Still registered: the next new window wakes this again.
            MGLOG_I("MG_Remote server: a suspended on-screen session did not get the new server window (%s); it "
                    "stays suspended until the next one",
                    ServerWindowAcquireName(acquired));
            return;
        }
        m_holdsWindowLease = true;
        m_leasedWindow = lease.window;
        (void)ResumeServerWindowOnLease(lease);
    }

    Bool ServerLoop::ResumeServerWindowOnLease(const ServerWindowLease& lease) {
        // Before the backend: its republished extent is the server window's (MGPipeServerOwnedWindow).
        NoteServerWindowExtent(lease.window, lease.width, lease.height);
        if (m_backend == nullptr || !m_backend->ResumeServerWindow(lease.window, lease.width, lease.height)) {
            LatchServerWindowLost("the suspended on-screen session's surfaces could not be rebuilt on the new server "
                                  "window");
            return false;
        }
        // N-3: the native binding moved onto the new window's surface.
        ForgetCurrentTuple();
        m_serverWindowSuspended.store(false, std::memory_order_release);
        const Uint64 resumes = m_serverWindowsResumed.fetch_add(1, std::memory_order_acq_rel) + 1;
        MGLOG_I("MG_Remote server: the on-screen session RESUMED on server window %p at %ux%u (generation %llu%s, "
                "resume #%llu); its surfaces were rebuilt there and the extent republished",
                lease.window, lease.width, lease.height, static_cast<unsigned long long>(lease.generation),
                lease.sizeAsRequested ? "" : ", NOT the size it asked for", static_cast<unsigned long long>(resumes));
        return true;
    }

    // ---------------------------------------------------------------------------------
    // The twelve forwarders: pack the frame, post it, unpack the reply
    // ---------------------------------------------------------------------------------

    Bool ServerInitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor) {
        SurfaceControlFrame frame;
        frame.kind = SurfaceControlOp::InitializeDisplay;
        frame.display = TokenFromHandle(dpy);
        const MobileGLResult rc = ServerLoopInstance().RunSurfaceControlFrame(frame);
        if (rc == MOBILEGL_OK) {
            // Written back whenever the dispatch ran, success or not - the old out-pointers
            // reached the backend directly, and a failed eglInitialize's partial writes are the
            // caller's to ignore exactly as before.
            if (major != nullptr) *major = frame.eglMajor;
            if (minor != nullptr) *minor = frame.eglMinor;
        }
        return rc == MOBILEGL_OK && frame.ok;
    }

    Bool ServerCreateEGLWindowSurface(EGLSurface surface, const MG_Backend::WindowHandle& handle) {
        SurfaceControlFrame frame;
        frame.kind = SurfaceControlOp::CreateWindowSurface;
        frame.surface = TokenFromHandle(surface);
        PackWindowHandle(frame, handle);
        const MobileGLResult rc = ServerLoopInstance().RunSurfaceControlFrame(frame);
        return rc == MOBILEGL_OK && frame.ok;
    }

    Bool ServerResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height) {
        SurfaceControlFrame frame;
        frame.kind = SurfaceControlOp::ResizeWindowSurface;
        frame.surface = TokenFromHandle(surface);
        frame.width = static_cast<Int>(width);
        frame.height = static_cast<Int>(height);
        const MobileGLResult rc = ServerLoopInstance().RunSurfaceControlFrame(frame);
        return rc == MOBILEGL_OK && frame.ok;
    }

    Bool ServerCreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height, SurfaceRefusalCode* refusal,
                                       Uint32 flags) {
        SurfaceControlFrame frame;
        frame.kind = SurfaceControlOp::CreatePbufferSurface;
        frame.surface = TokenFromHandle(surface);
        frame.width = width;
        frame.height = height;
        frame.contextFlags = flags;
        const MobileGLResult rc = ServerLoopInstance().RunSurfaceControlFrame(frame);
        if (refusal != nullptr) *refusal = static_cast<SurfaceRefusalCode>(frame.refusal);
        return rc == MOBILEGL_OK && frame.ok;
    }

    ServerOwnedWindowReply ServerCreateServerOwnedWindowSurface(EGLSurface surface, Uint32 width, Uint32 height) {
        // P12 (D1/D2): ONE frame, and no SetWindowHandle in front of it - the window is the
        // server's, so there is nothing of the client's to name. The frame-local tag is what the
        // codec turns into WindowKind::ServerOwned with token 0.
        SurfaceControlFrame frame;
        frame.kind = SurfaceControlOp::CreateWindowSurface;
        frame.surface = TokenFromHandle(surface);
        frame.windowBackend = kServerOwnedWindowBackend;
        frame.nativeToken = 0;
        frame.width = static_cast<Int>(width);
        frame.height = static_cast<Int>(height);
        const MobileGLResult rc = ServerLoopInstance().RunSurfaceControlFrame(frame);
        ServerOwnedWindowReply reply;
        reply.transport = rc;
        reply.ok = rc == MOBILEGL_OK && frame.ok;
        reply.refusal = static_cast<SurfaceRefusalCode>(frame.refusal);
        reply.width = frame.width > 0 ? static_cast<Uint32>(frame.width) : 0u;
        reply.height = frame.height > 0 ? static_cast<Uint32>(frame.height) : 0u;
        return reply;
    }

    ServerOwnedWindowReply ServerResizeServerOwnedWindowSurface(EGLSurface surface, Uint32 width, Uint32 height) {
        // The ordinary resize frame: the server knows the surface is on its own window.
        SurfaceControlFrame frame;
        frame.kind = SurfaceControlOp::ResizeWindowSurface;
        frame.surface = TokenFromHandle(surface);
        frame.width = static_cast<Int>(width);
        frame.height = static_cast<Int>(height);
        const MobileGLResult rc = ServerLoopInstance().RunSurfaceControlFrame(frame);
        ServerOwnedWindowReply reply;
        reply.transport = rc;
        reply.ok = rc == MOBILEGL_OK && frame.ok;
        reply.refusal = static_cast<SurfaceRefusalCode>(frame.refusal);
        reply.width = frame.width > 0 ? static_cast<Uint32>(frame.width) : 0u;
        reply.height = frame.height > 0 ? static_cast<Uint32>(frame.height) : 0u;
        return reply;
    }

    Bool ServerMakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
        SurfaceControlFrame frame;
        // The release request (the three NO_* markers, ClassifyEglMakeCurrent's ClientRelease
        // shape) crosses as its own op: on the wire that is what ReleaseCurrent IS
        // (protocol.fbs), and the dispatch reaches the same ApplyMakeCurrent arm either way.
        frame.kind = (draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE && ctx == EGL_NO_CONTEXT)
                         ? SurfaceControlOp::ReleaseCurrent
                         : SurfaceControlOp::MakeCurrent;
        frame.display = TokenFromHandle(dpy);
        frame.surface = TokenFromHandle(draw);
        frame.readSurface = TokenFromHandle(read);
        frame.context = TokenFromHandle(ctx);
        const MobileGLResult rc = ServerLoopInstance().RunSurfaceControlFrame(frame);
        return rc == MOBILEGL_OK && frame.ok;
    }

    // P14 S1. A context creation is a frame like any other: `context` IS the client token (the
    // schema's field, not a bit-cast handle - a context has no handle the server could name),
    // `shareGroupToken` and `contextFlags` the two values CreateContext adds to it. Same blocking
    // handshake, so eglCreateContext cannot return a token the server has not tabled - which is
    // what keeps bind_context's "this session never created that token" refusal unreachable on
    // the well-behaved path.
    Bool ServerCreateEGLContext(Uint64 clientContextToken, Uint64 shareGroupToken, Uint32 flags) {
        SurfaceControlFrame frame;
        frame.kind = SurfaceControlOp::CreateContext;
        frame.context = clientContextToken;
        frame.shareGroupToken = shareGroupToken;
        frame.contextFlags = flags;
        const MobileGLResult rc = ServerLoopInstance().RunSurfaceControlFrame(frame);
        return rc == MOBILEGL_OK && frame.ok;
    }

    Bool ServerDestroyEGLContext(Uint64 clientContextToken) {
        SurfaceControlFrame frame;
        frame.kind = SurfaceControlOp::DestroyContext;
        frame.context = clientContextToken;
        const MobileGLResult rc = ServerLoopInstance().RunSurfaceControlFrame(frame);
        return rc == MOBILEGL_OK && frame.ok;
    }

    Bool ServerSwapEGLBuffers(EGLDisplay dpy, EGLSurface draw) {
        SurfaceControlFrame frame;
        frame.kind = SurfaceControlOp::SwapBuffersInprocOnly;
        frame.display = TokenFromHandle(dpy);
        frame.surface = TokenFromHandle(draw);
        const MobileGLResult rc = ServerLoopInstance().RunSurfaceControlFrame(frame);
        return rc == MOBILEGL_OK && frame.ok;
    }

    void ServerSetEGLSwapInterval(Int interval) {
        SurfaceControlFrame frame;
        frame.kind = SurfaceControlOp::SetSwapInterval;
        frame.swapInterval = interval;
        (void)ServerLoopInstance().RunSurfaceControlFrame(frame);
    }

    void ServerReleaseEGLSurface(EGLSurface surface) {
        SurfaceControlFrame frame;
        frame.kind = SurfaceControlOp::ReleaseSurface;
        frame.surface = TokenFromHandle(surface);
        (void)ServerLoopInstance().RunSurfaceControlFrame(frame);
    }

    void ServerReleaseEGLResources() {
        // BLOCKING BY CONTRACT. EGLImpl.cpp:326 calls this on the app thread and then, if no
        // display and no context are left, calls MobileGL::Destroy() at :333. For DirectGLES it
        // runs DestroyEGLContext - eglMakeCurrent(NO_SURFACE), eglDestroyContext, eglTerminate -
        // and those must happen on the thread that made the context current. A fire-and-forget
        // here lets Destroy() walk on while the server still holds the context, which is
        // scout-install 5.3's named hazard. The frame channel is blocking, so nothing about
        // that changed with fc.
        SurfaceControlFrame frame;
        frame.kind = SurfaceControlOp::ReleaseResources;
        (void)ServerLoopInstance().RunSurfaceControlFrame(frame);
    }

    Bool ServerInitCapabilities() {
        SurfaceControlFrame frame;
        frame.kind = SurfaceControlOp::InitCapabilities;
        const MobileGLResult rc = ServerLoopInstance().RunSurfaceControlFrame(frame);
        return rc == MOBILEGL_OK && frame.ok;
    }

    Bool ServerInitWindowSurface() {
        SurfaceControlFrame frame;
        frame.kind = SurfaceControlOp::InitWindowSurfaceInprocOnly;
        const MobileGLResult rc = ServerLoopInstance().RunSurfaceControlFrame(frame);
        return rc == MOBILEGL_OK && frame.ok;
    }

    void ServerSetWindowHandle(const MG_Backend::WindowHandle& handle) {
        SurfaceControlFrame frame;
        frame.kind = SurfaceControlOp::SetWindowHandle;
        PackWindowHandle(frame, handle);
        (void)ServerLoopInstance().RunSurfaceControlFrame(frame);
    }

} // namespace MobileGL::MG_Remote::Server
