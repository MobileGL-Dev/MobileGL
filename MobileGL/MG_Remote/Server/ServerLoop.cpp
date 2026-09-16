// MobileGL - MobileGL/MG_Remote/Server/ServerLoop.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 package v1: the apply thread, its affinity, its parking, and the EGL ownership move.

#include "ServerLoop.h"

#include <Config.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_Util/Debug/Log.h>

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

        // Returns the mask that was actually APPLIED, which is 0 when nothing was.
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
            return requested;
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
        {
            const std::lock_guard<std::mutex> lock(m_exitMutex);
            m_exited = false;
        }
        m_running.store(true, std::memory_order_release);
        m_thread = std::thread([this] { ApplyThreadMain(); });
        return MOBILEGL_OK;
    }

    Bool ServerLoop::Running() const { return m_running.load(std::memory_order_acquire); }

    Bool ServerLoop::OnApplyThread() {
        ServerLoop& loop = ServerLoopInstance();
        return loop.m_running.load(std::memory_order_acquire) &&
               loop.m_applyThreadId.load(std::memory_order_acquire) == std::this_thread::get_id();
    }

    Uint64 ServerLoop::ResolvedAffinityMask() const { return m_affinityMask; }
    Uint64 ServerLoop::DrainedRecords() const { return m_drained.load(std::memory_order_acquire); }
    Uint64 ServerLoop::ParkCount() const { return m_parks.load(std::memory_order_acquire); }

    void ServerLoop::ApplyThreadMain() {
        m_applyThreadId.store(std::this_thread::get_id(), std::memory_order_release);
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
        const auto ready = [this, &control, &ring] {
            return control.cmdHead.load(std::memory_order_acquire) != ring.LocalTail() ||
                   m_stopRequested.load(std::memory_order_acquire) || ControlIsPending();
        };

        for (;;) {
            PumpControlRequest();
            if (m_stopRequested.load(std::memory_order_acquire)) break;
            DrainRing();
            if (m_stopRequested.load(std::memory_order_acquire)) break;
            if (ready()) continue;

            m_parks.fetch_add(1, std::memory_order_acq_rel);
            const bool woke = bell.Wait(control.consumerParked, ready, spinUs, Transport::kWaitForever);
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
                        "bell - that is impossible by Doorbell.h:139-178 and means the bell's "
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

        // Anything still parked in RunOnApplyThread has to be answered rather than left
        // waiting: this thread is the only thing that could have run it.
        {
            const std::lock_guard<std::mutex> lock(m_controlMutex);
            if (m_controlPending) {
                m_controlPending = false;
                m_controlFinished = true;
                m_controlResult = MOBILEGL_ERR_NOT_INITIALIZED;
                MGLOG_E("MG_Remote server: a control request was still posted when mgl-srv-apply "
                        "exited; it is answered NOT_INITIALIZED rather than left blocking");
            }
        }
        m_controlDone.notify_all();

        m_running.store(false, std::memory_order_release);
        SignalExited();
    }

    Bool ServerLoop::ControlIsPending() const {
        const std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_controlMutex));
        return m_controlPending;
    }

    Bool ServerLoop::PumpControlRequest() {
        ControlWork work = nullptr;
        void* user = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_controlMutex);
            if (!m_controlPending) return false;
            work = m_controlWork;
            user = m_controlUser;
        }
        const MobileGLResult result = work == nullptr ? MOBILEGL_ERR_INVALID_ARGUMENT : work(user);
        {
            const std::lock_guard<std::mutex> lock(m_controlMutex);
            m_controlResult = result;
            m_controlPending = false;
            m_controlFinished = true;
        }
        m_controlDone.notify_all();
        return true;
    }

    Uint64 ServerLoop::DrainRing() {
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
            consumer.RetireThrough(consumer.AppliedSeq());
            // LEAVING THE APPLIER (p1's M-5) IS *NOT* DONE HERE. It is done inside
            // PipeApplier::ApplyOne, before s1's SessionConsumer::ApplyOne publishes appliedSeq
            // - see the block there. A clear at this point races the client, which the barrier
            // has already released by then.
        }
        return applied;
    }

    MobileGLResult ServerLoop::RunOnApplyThread(ControlWork work, void* user) {
        if (work == nullptr) return MOBILEGL_ERR_INVALID_ARGUMENT;
        // RE-ENTRANCY IS NOT A DEADLOCK. The teardown path posts from the apply thread itself -
        // ~BackendObject_DirectGLES reaches ReleaseEGLResources - and so does anything the
        // applier calls that wants "run this where the context is". Running inline is the
        // correct answer there and the only non-hanging one.
        if (OnApplyThread()) return work(user);
        // NO THREAD YET, OR ALREADY GONE: run inline on the caller. That is right for the
        // window between MG_Backend::Init()'s hook and ClientSession::Start (the backend object
        // exists, the thread does not, and nothing has made a context current), and for the
        // window after Stop(). It is NOT a silent fallback to monolith: the thread's absence in
        // those two windows is a fact about the lifecycle, not about the transport.
        if (!m_running.load(std::memory_order_acquire)) return work(user);

        const std::lock_guard<std::mutex> callerLock(m_callerMutex);
        {
            std::unique_lock<std::mutex> lock(m_controlMutex);
            m_controlWork = work;
            m_controlUser = user;
            m_controlPending = true;
            m_controlFinished = false;
        }
        // Publish THEN ring, in that order and never the other (Doorbell.h:186-193). The bell
        // is rung unconditionally rather than through NotifyIfParked because this side does not
        // know whether the apply thread is parked or spinning, and CondVarDoorbell remembers a
        // wakeup that arrives while nobody is waiting.
        if (m_session != nullptr) {
            m_session->ConsumerDoorbell().Notify();
        }
        std::unique_lock<std::mutex> lock(m_controlMutex);
        m_controlDone.wait(lock, [this] { return m_controlFinished; });
        return m_controlResult;
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
                    "%u ms of Stop(). It parks on Doorbell::Wait(kWaitForever), which only "
                    "CondVarDoorbell::Kill() can break (Doorbell.h:211-221, table 3 step 2), so "
                    "this is a lost wakeup and not a slow thread. Aborting rather than "
                    "detaching: a detached apply thread still owns the context and would read "
                    "rings the client is about to unmap",
                    kJoinTimeoutMs);
            std::abort();
        }
        m_thread.join();
        m_applyThreadId.store(std::thread::id{}, std::memory_order_release);
        m_running.store(false, std::memory_order_release);
        m_session = nullptr;
    }

    ServerLoop& ServerLoopInstance() {
        // ID-8: leak at exit, like every MG_Remote singleton.
        static ServerLoop& instance = *new ServerLoop{};
        return instance;
    }

    // ---------------------------------------------------------------------------------
    // The twelve EGL forwarders
    // ---------------------------------------------------------------------------------

    namespace {

        // One captureless trampoline for every call: ControlWork is a raw function pointer
        // plus a void*, not a std::function, because these run on the TEARDOWN path and the
        // teardown path may not allocate (ID-8 exists because frontend destructors reach here
        // from exit handlers).
        template <class Args>
        MobileGLResult RunOnApply(Args& args) {
            return ServerLoopInstance().RunOnApplyThread(
                +[](void* user) -> MobileGLResult { return static_cast<Args*>(user)->Run(); },
                &args);
        }

        // Null means the hook never ran or the session is already down. Every forwarder
        // answers false / no-op rather than dereferencing: a client that calls eglMakeCurrent
        // on a process whose split bring-up failed must see a refusal, not a crash.
        MG_Backend::BackendObject* ServerBackendOrNull() { return ServerLoopInstance().Backend(); }

    } // namespace

    Bool ServerInitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor) {
        struct Args {
            EGLDisplay dpy;
            EGLint* major;
            EGLint* minor;
            Bool ok = false;
            MobileGLResult Run() {
                MG_Backend::BackendObject* backend = ServerBackendOrNull();
                if (backend == nullptr) return MOBILEGL_ERR_NOT_INITIALIZED;
                ok = backend->InitializeEGLDisplay(dpy, major, minor);
                return MOBILEGL_OK;
            }
        } args{dpy, major, minor};
        return RunOnApply(args) == MOBILEGL_OK && args.ok;
    }

    Bool ServerCreateEGLWindowSurface(EGLSurface surface, const MG_Backend::WindowHandle& handle) {
        struct Args {
            EGLSurface surface;
            const MG_Backend::WindowHandle* handle;
            Bool ok = false;
            MobileGLResult Run() {
                MG_Backend::BackendObject* backend = ServerBackendOrNull();
                if (backend == nullptr) return MOBILEGL_ERR_NOT_INITIALIZED;
                ok = backend->CreateEGLWindowSurface(surface, *handle);
                return MOBILEGL_OK;
            }
        } args{surface, &handle};
        return RunOnApply(args) == MOBILEGL_OK && args.ok;
    }

    Bool ServerResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height) {
        struct Args {
            EGLSurface surface;
            Uint32 width;
            Uint32 height;
            Bool ok = false;
            MobileGLResult Run() {
                MG_Backend::BackendObject* backend = ServerBackendOrNull();
                if (backend == nullptr) return MOBILEGL_ERR_NOT_INITIALIZED;
                ok = backend->ResizeEGLWindowSurface(surface, width, height);
                return MOBILEGL_OK;
            }
        } args{surface, width, height};
        return RunOnApply(args) == MOBILEGL_OK && args.ok;
    }

    Bool ServerCreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height) {
        struct Args {
            EGLSurface surface;
            EGLint width;
            EGLint height;
            Bool ok = false;
            MobileGLResult Run() {
                MG_Backend::BackendObject* backend = ServerBackendOrNull();
                if (backend == nullptr) return MOBILEGL_ERR_NOT_INITIALIZED;
                ok = backend->CreateEGLPbufferSurface(surface, width, height);
                return MOBILEGL_OK;
            }
        } args{surface, width, height};
        return RunOnApply(args) == MOBILEGL_OK && args.ok;
    }

    Bool ServerMakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
        struct Args {
            EGLDisplay dpy;
            EGLSurface draw;
            EGLSurface read;
            EGLContext ctx;
            Bool ok = false;
            MobileGLResult Run() {
                MG_Backend::BackendObject* backend = ServerBackendOrNull();
                if (backend == nullptr) return MOBILEGL_ERR_NOT_INITIALIZED;
                // THE ONE eglMakeCurrent OF THE PROCESS'S LIFE, ON THIS THREAD. Every later
                // client-side eglMakeCurrent onto the same surface finds the context already
                // current here and costs nothing; the owner slot is written once.
                ok = backend->MakeEGLCurrent(dpy, draw, read, ctx);
                if (!ok) return MOBILEGL_OK;
                // R-12, arm (a): the caps snapshot is REPUBLISHED because InitCapabilities has
                // now run for real. DirectGLES has no OnCapsInvalidated producer at all, and
                // c0's answer is that a SECOND arrival IS the invalidation - so the client's
                // mirror is refreshed with no dev-shaped backend edit and with no eleventh
                // MGPipeCallbacks slot (MGPipeCallbacks.h:56-58's static_assert exists to make
                // that cost visible).
                ServerSession* session = ServerSession::Active();
                if (session != nullptr && session->Accepted()) {
                    const MobileGLResult published = session->PublishCapsSnapshot();
                    if (published != MOBILEGL_OK) {
                        MGLOG_E("MG_Remote server: the post-make-current CapsSnapshot could not "
                                "be published (rc=%d); the client's mirror still holds the empty "
                                "snapshot Accept() sent before any context existed",
                                static_cast<int>(published));
                    }
                }
                return MOBILEGL_OK;
            }
        } args{dpy, draw, read, ctx};
        return RunOnApply(args) == MOBILEGL_OK && args.ok;
    }

    Bool ServerSwapEGLBuffers(EGLDisplay dpy, EGLSurface draw) {
        struct Args {
            EGLDisplay dpy;
            EGLSurface draw;
            Bool ok = false;
            MobileGLResult Run() {
                MG_Backend::BackendObject* backend = ServerBackendOrNull();
                if (backend == nullptr) return MOBILEGL_ERR_NOT_INITIALIZED;
                ok = backend->SwapEGLBuffers(dpy, draw);
                return MOBILEGL_OK;
            }
        } args{dpy, draw};
        return RunOnApply(args) == MOBILEGL_OK && args.ok;
    }

    void ServerSetEGLSwapInterval(Int interval) {
        struct Args {
            Int interval;
            MobileGLResult Run() {
                MG_Backend::BackendObject* backend = ServerBackendOrNull();
                if (backend == nullptr) return MOBILEGL_ERR_NOT_INITIALIZED;
                backend->SetEGLSwapInterval(interval);
                return MOBILEGL_OK;
            }
        } args{interval};
        (void)RunOnApply(args);
    }

    void ServerReleaseEGLSurface(EGLSurface surface) {
        struct Args {
            EGLSurface surface;
            MobileGLResult Run() {
                MG_Backend::BackendObject* backend = ServerBackendOrNull();
                if (backend == nullptr) return MOBILEGL_ERR_NOT_INITIALIZED;
                backend->ReleaseEGLSurface(surface);
                return MOBILEGL_OK;
            }
        } args{surface};
        (void)RunOnApply(args);
    }

    void ServerReleaseEGLResources() {
        // BLOCKING BY CONTRACT. EGLImpl.cpp:326 calls this on the app thread and then, if no
        // display and no context are left, calls MobileGL::Destroy() at :333. For DirectGLES it
        // runs DestroyEGLContext - eglMakeCurrent(NO_SURFACE), eglDestroyContext, eglTerminate -
        // and those must happen on the thread that made the context current. A fire-and-forget
        // here lets Destroy() walk on while the server still holds the context, which is
        // scout-install 5.3's named hazard.
        struct Args {
            MobileGLResult Run() {
                MG_Backend::BackendObject* backend = ServerBackendOrNull();
                if (backend == nullptr) return MOBILEGL_ERR_NOT_INITIALIZED;
                backend->ReleaseEGLResources();
                return MOBILEGL_OK;
            }
        } args{};
        (void)RunOnApply(args);
    }

    Bool ServerInitCapabilities() {
        struct Args {
            Bool ok = false;
            MobileGLResult Run() {
                MG_Backend::BackendObject* backend = ServerBackendOrNull();
                if (backend == nullptr) return MOBILEGL_ERR_NOT_INITIALIZED;
                ok = backend->InitCapabilities();
                if (!ok) return MOBILEGL_OK;
                ServerSession* session = ServerSession::Active();
                if (session != nullptr && session->Accepted()) {
                    (void)session->PublishCapsSnapshot();
                }
                return MOBILEGL_OK;
            }
        } args{};
        return RunOnApply(args) == MOBILEGL_OK && args.ok;
    }

    Bool ServerInitWindowSurface() {
        struct Args {
            Bool ok = false;
            MobileGLResult Run() {
                MG_Backend::BackendObject* backend = ServerBackendOrNull();
                if (backend == nullptr) return MOBILEGL_ERR_NOT_INITIALIZED;
                ok = backend->InitWindowSurface();
                return MOBILEGL_OK;
            }
        } args{};
        return RunOnApply(args) == MOBILEGL_OK && args.ok;
    }

    void ServerSetWindowHandle(const MG_Backend::WindowHandle& handle) {
        struct Args {
            const MG_Backend::WindowHandle* handle;
            MobileGLResult Run() {
                MG_Backend::BackendObject* backend = ServerBackendOrNull();
                if (backend == nullptr) return MOBILEGL_ERR_NOT_INITIALIZED;
                backend->SetWindowHandle(*handle);
                return MOBILEGL_OK;
            }
        } args{&handle};
        (void)RunOnApply(args);
    }

} // namespace MobileGL::MG_Remote::Server

// ---------------------------------------------------------------------------------
// The weak placeholder for package c1's remote backend object
// ---------------------------------------------------------------------------------
//
// MG_Backend/Init.cpp's hook calls MG_Remote::Client::CreateRemoteBackendObject(), which is
// c1's BackendObject_Remote and does not exist while v1 is written. A WEAK definition here
// lets v1 compile, link and be tested today, and c1's strong definition displaces it at link
// time with no edit anywhere.
//
// IT ABORTS BY NAME AND DOES NOT RETURN A WORKING MONOLITH OBJECT. That distinction is the
// whole point: a placeholder that handed back a BackendObject_DirectGLES would give a lane
// called "split" a correct picture produced entirely by the monolith path, which is
// ARCHITECTURE.md 10.3's failure and the one this phase exists to make impossible.
#if defined(__GNUC__) || defined(__clang__)
namespace MobileGL::MG_Remote::Client {
    __attribute__((weak)) UniquePtr<MG_Backend::BackendObject> CreateRemoteBackendObject() {
        MGLOG_F("MGPipe: Fatal{UnimplementedRemoteBackendObject} - MG_Backend::Init()'s split "
                "hook asked for the client's BackendObject_Remote and only v1's weak "
                "placeholder is linked in. That object is package c1's (BRIEF 5); until it "
                "lands there is no client role, and this build refuses to substitute the "
                "monolith backend for it");
        std::abort();
    }
} // namespace MobileGL::MG_Remote::Client
#endif
