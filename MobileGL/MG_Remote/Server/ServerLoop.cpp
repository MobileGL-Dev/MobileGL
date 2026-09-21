// MobileGL - MobileGL/MG_Remote/Server/ServerLoop.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 package v1: the apply thread, its affinity, its parking, and the EGL ownership move.

#include "ServerLoop.h"
#include <MG_Backend/MGPipe/PipeInputs.h>

#include <Config.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_Util/Debug/Log.h>
#include <MG_Util/Metrics/PipeStats.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>

#if defined(__linux__) || defined(__ANDROID__)
#include <pthread.h>
#include <sched.h>
#endif

namespace MobileGL::MG_Remote::Server {

    namespace {

        // The bounded join. InProcessTransportTest.cpp:344 uses five seconds for the same
        // reason: a lost wakeup must be a RED TEST and not a hung CI job.
        constexpr Uint32 kJoinTimeoutMs = 5000;

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
        Uint64 DetectBigCoreMask() {
            const Uint cpuCount = std::min(64u, std::max(1u, std::thread::hardware_concurrency()));
            Uint64 frequencies[64] = {};
            for (Uint cpu = 0; cpu < cpuCount; ++cpu) {
                frequencies[cpu] = ReadCpuMaxFrequencyKHz(cpu);
                if (frequencies[cpu] == 0) return 0;
            }
            Uint64 peak = 0;
            for (Uint cpu = 0; cpu < cpuCount; ++cpu) peak = std::max(peak, frequencies[cpu]);
            if (peak == 0) return 0;
            const Uint64 threshold = peak * kBigCoreFrequencyPercent / 100;
            Uint64 mask = 0;
            for (Uint cpu = 0; cpu < cpuCount; ++cpu) {
                if (frequencies[cpu] >= threshold) mask |= (1ull << cpu);
            }
            return mask;
        }

        // MOBILEGL_IPC_SERVER_AFFINITY = `auto` | `off` | an explicit mask (0x... or decimal).
        // The RAW STRING is what Config keeps, because the resolved mask is what gets logged -
        // "an affinity that silently did nothing looks exactly like one that worked"
        // (CONTRACT-P5 5).
        Uint64 RequestedAffinityMask(const char* raw, Bool* outRecognised) {
            *outRecognised = true;
            if (raw == nullptr || raw[0] == '\0') return DetectBigCoreMask();
            if (std::strcmp(raw, "auto") == 0) return DetectBigCoreMask();
            if (std::strcmp(raw, "off") == 0) return 0;
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(raw, &end, 0);
            if (end == raw || (end != nullptr && *end != '\0')) {
                *outRecognised = false;
                return 0;
            }
            return static_cast<Uint64>(parsed);
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

    } // namespace

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
        m_stopRequested.store(false, std::memory_order_release);
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
        // look like client reads to the guards. Relaxed for the reason stated beside the key's
        // declaration: it is an identity, not a handshake, and this thread is the only writer.
        Detail::g_applyThreadKey.store(Detail::CurrentThreadKey(), std::memory_order_relaxed);
        NameThisThread("mgl-srv-apply");

        Bool recognised = true;
        const char* raw = AffinityStringFromConfig();
        const Uint64 requested = RequestedAffinityMask(raw, &recognised);
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
                "%u us",
                raw == nullptr ? "" : raw, static_cast<unsigned long long>(requested),
                static_cast<unsigned long long>(m_affinityMask), SpinUsFromConfig());

        ServerSession& session = *m_session;
        // The decoder is built HERE, on this thread, because PipeWireDecoder is "not thread
        // safe: one decoder on the apply thread, by construction" and its constructor installs
        // the process-wide apply hook.
        session.Applier().Attach(&session.Control(), m_backend.get());

        Transport::RingControl& control = session.Control();
        Transport::Doorbell& bell = session.ConsumerDoorbell();
        Transport::RingConsumer& ring = session.CommandRing();
        const Uint32 spinUs = SpinUsFromConfig();

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
        const auto ready = [this, &control, &ring] {
            if (m_stopRequested.load(std::memory_order_acquire) || ControlIsPending()) return true;
            if (control.eventRingFull.load(std::memory_order_acquire) != 0) return false;
            return control.cmdHead.load(std::memory_order_acquire) != ring.LocalTail();
        };

        for (;;) {
            PumpControlRequest();
            if (m_stopRequested.load(std::memory_order_acquire)) break;
            if (control.eventRingFull.load(std::memory_order_acquire) == 0) DrainRing();
            if (m_stopRequested.load(std::memory_order_acquire)) break;
            if (ready()) continue;

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
            const bool woke = bell.Wait(control.consumerParked, ready, spinUs,
                                        Transport::kWaitForever, &m_parkBlocks);
            if (!woke) {
                // Wait returns false only on a dead bell or an expired deadline, and this park
                // has no deadline. A dead bell IS the shutdown signal (table 3's teardown step
                // 2); treating it as anything else would spin at full clock for ever, since
                // parking on a dead bell no longer blocks.
                if (bell.Dead()) {
                    MGLOG_D("MG_Remote server: the consumer doorbell is dead; mgl-srv-apply is "
                            "shutting down");
                    break;
                }
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
        PumpControlRequest();
        DrainRing();

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
            Detail::g_applyThreadKey.store(0, std::memory_order_relaxed);
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
        // THE IDLE POLL'S FAST NEGATIVE. The loop calls this once per iteration whether or not
        // anything was posted, and the common answer is "nothing". Taking m_controlMutex to
        // learn that was the other half of package T's finding; the shadow answers it with a
        // load, and everything below this line still runs under the lock exactly as before.
        if (!m_controlPosted.load(std::memory_order_acquire)) return false;

        SurfaceControlFrame frame;
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
        }
        // The dispatch runs OUTSIDE the lock, exactly as the old work(user) did, and fills the
        // frame's reply half; the reply is then published back into the slot under the lock so
        // the poster's copy-out after m_controlDone sees it.
        const MobileGLResult result = ApplySurfaceControlFrame(
            frame, m_controlProbeHook.load(std::memory_order_acquire),
            m_controlProbeUser.load(std::memory_order_acquire));
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
        ServerSession& session = *m_session;
        Transport::SessionConsumer& consumer = session.Consumer();
        PipeApplier& applier = session.Applier();
        Uint64 applied = 0;
        for (;;) {
            bool corrupt = false;
            const bool popped = consumer.ApplyOne(
                [this, &applier](const Transport::RingRecordView& record) {
                    applier.ApplyOne(record);
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
                // for ever; skipping desynchronises seq, and seq IS the reply-slot id.
                MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"SEG_CMD record header\"} - the "
                        "consumer refused a record header at applied seq %llu",
                        static_cast<unsigned long long>(consumer.AppliedSeq()));
                std::abort();
            }
            if (!popped) break;
            ++applied;
        }
        if (applied != 0) {
            // THE TWO TALLIES MUST AGREE, AND THAT IS WHAT MAKES R-9's BATCHING BAN CHECKABLE.
            // RingControl::appliedSeq has one writer (SessionConsumer::ApplyOne, +1 per record)
            // and PipeWireDecoder keeps its own count; a batched publish would move one and not
            // the other, which a single counter could not have told apart (w1-v1 5).
            if (applier.DecoderAppliedSeq() != consumer.AppliedSeq()) {
                MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"appliedSeq batched\"} - the "
                        "session's watermark is %llu and the decoder applied %llu records. P5 "
                        "forbids batching appliedSeq (R-9): the verb barrier's waiter reads it, "
                        "and a watermark ahead of the decoder promises work that has not run",
                        static_cast<unsigned long long>(consumer.AppliedSeq()),
                        static_cast<unsigned long long>(applier.DecoderAppliedSeq()));
                std::abort();
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
                MGLOG_F("MGPipe: Fatal{ApplyThreadNotRunning, \"EGL on the app thread\"} - a "
                        "control frame reached RunSurfaceControlFrame with the server's backend "
                        "built but no mgl-srv-apply thread running (ClientSession::Start failed "
                        "after ServerLoop::CreateBackend). Dispatching it inline would make the "
                        "context current on the APP thread - the split lane's whole premise. "
                        "Refusing by name rather than falling back to monolith");
                std::abort();
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
            m_session->ConsumerDoorbell().Notify();
        }
        m_controlDone.wait(lock, [this] { return m_controlFinished; });
        frame = m_controlFrame; // the reply half, written by the dispatch
        return m_controlResult;
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

    void ServerLoop::Stop() {
        if (!m_thread.joinable()) {
            // Never started, or already stopped. The backend may still exist - the hook builds
            // it before the thread - and it has to go somewhere, so it goes here, on whatever
            // thread called Stop. No context was ever made current from another thread in that
            // case, which is exactly the condition that makes this safe.
            if (m_backend != nullptr) m_backend.reset();
            MG_Pipe::MGPipeServerSetContextLive(false);
            // N-3, same reason as ApplyThreadMain's exit: no thread runs, so the apply-thread-only
            // rule on the tuple has no other writer to race.
            ForgetCurrentTuple();
            m_running.store(false, std::memory_order_release);
            return;
        }
        m_stopRequested.store(true, std::memory_order_release);
        if (m_session != nullptr) {
            // The bell may already be dead - ClientSession::Stop calls transport->Shutdown()
            // first, which is what table 3's step 2 requires - and Notify on a dead bell is
            // harmless. Ringing anyway covers the paths that Stop without a Kill.
            m_session->ConsumerDoorbell().Notify();
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
        if (!exited) {
            // ABORT, NOT DETACH. A detached apply thread still owns the EGL context and would
            // run on into the client's teardown, reading rings the client is about to unmap -
            // a use-after-free whose only symptom is an intermittent crash somewhere else.
            // Aborting here is red, immediate, and names the cause.
            MGLOG_F("MGPipe: Fatal{ApplyThreadJoinTimeout} - mgl-srv-apply did not exit within "
                    "%u ms of Stop(). Stop() published m_stopRequested (in the park predicate) "
                    "BEFORE it rang, so a plain Notify should already have un-parked the thread; "
                    "Doorbell::Kill() (Doorbell.h, CondVarDoorbell::Kill; table 3 step 2) is "
                    "the belt to that "
                    "Notify's braces. If the thread is still parked after both, the wakeup was "
                    "lost, not slow. Aborting rather than detaching: a detached apply thread still "
                    "owns the context and would read rings the client is about to unmap",
                    kJoinTimeoutMs);
            std::abort();
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
        MG_Backend::WindowBackend WindowBackendFromFrameValue(Int value) {
            if (value < static_cast<Int>(MG_Backend::WindowBackend::Unknown) ||
                value >= static_cast<Int>(MG_Backend::WindowBackend::WindowBackendCount)) {
                MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"SurfaceOp.windowBackend\"} - a "
                        "control frame carried window backend tag %d, which names no "
                        "WindowBackend", value);
                std::abort();
            }
            return static_cast<MG_Backend::WindowBackend>(value);
        }

        MG_Backend::WindowHandle UnpackWindowHandle(const SurfaceControlFrame& frame) {
            MG_Backend::WindowHandle handle;
            handle.Backend = WindowBackendFromFrameValue(frame.windowBackend);
            handle.Handle = HandleFromToken<void*>(frame.nativeToken);
            handle.Width = static_cast<Uint32>(frame.width);
            handle.Height = static_cast<Uint32>(frame.height);
            return handle;
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
            frame.ok = backend->CreateEGLWindowSurface(HandleFromToken<EGLSurface>(frame.surface),
                                                       UnpackWindowHandle(frame));
            // N-3: BackendObject_DirectGLES destroys and recreates the native context to create
            // a DIFFERENT surface, so whatever tuple was bound names a dead context.
            if (frame.ok) ForgetCurrentTuple();
            return MOBILEGL_OK;
        }
        case SurfaceControlOp::ResizeWindowSurface:
            frame.ok = backend->ResizeEGLWindowSurface(HandleFromToken<EGLSurface>(frame.surface),
                                                       static_cast<Uint32>(frame.width),
                                                       static_cast<Uint32>(frame.height));
            return MOBILEGL_OK;
        case SurfaceControlOp::CreatePbufferSurface:
            frame.ok = backend->CreateEGLPbufferSurface(HandleFromToken<EGLSurface>(frame.surface),
                                                        frame.width, frame.height);
            // N-3: as for the window surface - a (re)creation may have destroyed the context the
            // held tuple named. The surface's own creation binds natively, so the client's
            // make-current that follows is forwarded and deduped one layer down (ID-54).
            if (frame.ok) ForgetCurrentTuple();
            return MOBILEGL_OK;
        case SurfaceControlOp::MakeCurrent: {
            // C7 / ID-54: the apply thread binds the native context ONCE per context lifetime
            // and holds it for life. ApplyMakeCurrent forwards a bind only for a tuple it does
            // not hold and treats an identical repeat as a no-op; the native call for a surface
            // already current on this thread is skipped one layer down
            // (BackendObject_DirectGLES's ID-54 arm), which is what makes "the owner slot is
            // written once" TRUE and measured (ServerLoopTest's C7 control) rather than claimed.
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
        case SurfaceControlOp::SetSwapInterval:
            backend->SetEGLSwapInterval(frame.swapInterval);
            frame.ok = true;
            return MOBILEGL_OK;
        case SurfaceControlOp::ReleaseSurface: {
            const EGLSurface surface = HandleFromToken<EGLSurface>(frame.surface);
            backend->ReleaseEGLSurface(surface);
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
        case SurfaceControlOp::SetWindowHandle:
            backend->SetWindowHandle(UnpackWindowHandle(frame));
            frame.ok = true;
            return MOBILEGL_OK;
        case SurfaceControlOp::InitCapabilitiesInprocOnly:
            // Inproc-only (f0-egl §4.2): on the wire the ANSWER to this op is the CapsSnapshot
            // frame itself, so there is no SurfaceOp kind for it. The frame channel still has to
            // carry it inproc because InitCapabilities runs GL queries and therefore belongs to
            // the apply thread.
            frame.ok = backend->InitCapabilities();
            if (frame.ok) {
                ServerSession* session = ServerSession::Active();
                if (session != nullptr && session->Accepted()) {
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
        MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"SurfaceOp.kind\"} - the control slot held "
                "kind %d, which names no dispatchable op", static_cast<int>(frame.kind));
        std::abort();
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

    Bool ServerCreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height) {
        SurfaceControlFrame frame;
        frame.kind = SurfaceControlOp::CreatePbufferSurface;
        frame.surface = TokenFromHandle(surface);
        frame.width = width;
        frame.height = height;
        const MobileGLResult rc = ServerLoopInstance().RunSurfaceControlFrame(frame);
        return rc == MOBILEGL_OK && frame.ok;
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
        frame.kind = SurfaceControlOp::InitCapabilitiesInprocOnly;
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
