// MobileGL - MobileGL/MG_Remote/Server/ServerLoop.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The apply thread (mgl-srv-apply) and the server's private backend object. The apply thread
// owns the backend's native EGL context for the context's whole life, so the backend's
// IsBackendContextCurrentOnThisThread() / CanTouchGLNow() guards answer true on the server
// instead of degrading. The client's EGL calls reach it as blocking control requests (the
// Server* forwarders below); ARCHITECTURE.md §15.5 has the make-current dedup and teardown rules.
// Stop() publishes m_stopRequested, which is in the park predicate, before it rings, so a plain
// Notify un-parks the thread; the join is bounded so a lost wakeup fails instead of hanging.

#pragma once
#include <Includes.h>

#include "ServerDisplay.h"
#include "ServerSession.h"
#include "SurfaceControlFrame.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace MobileGL::MG_Remote::Server {

    // The apply-thread identity. OnApplyThread() sits under every role guard on the per-draw hot
    // paths, so it is one inline read of the caller's own TLS word: no call, atomic or singleton.
    // It is a per-thread fact because a process runs one apply thread per served session.
    //   t_onApplyThread   the calling thread is an apply thread. ApplyThreadMain raises it before
    //                     it applies anything and lowers it in the m_controlMutex section that
    //                     clears m_running.
    //   g_applyThreadKey  how many apply threads this process runs; Managers.cpp reads `!= 0` as
    //                     "an apply thread is running somewhere". No hot path reads it.
    namespace Detail {
        // A fresh thread's TLS word is zero-initialized, so a thread that inherits the exited
        // apply thread's TLS block - the recycling case the old relaxed-load argument was about -
        // starts out answering false and can only become the apply thread by running
        // ApplyThreadMain. There is no stale value to read and nothing to order.
        inline thread_local Bool t_onApplyThread = false;

        // The count of live apply threads, as a relaxed atomic: an identity-like gauge, not a
        // handshake (the real ordering lives in the ring, the doorbell and m_controlMutex), and
        // its one reader wants "is it zero".
        inline std::atomic<Uint64> g_applyThreadKey{0};
    } // namespace Detail

    // One surface mode per session. It latches at the session's first successful surface
    // creation: a ServerOwned window makes it on-screen, a pbuffer offscreen. A later surface of
    // the other kind is refused with SurfaceModeMismatch (reply ok=false, nothing latched).
    // Client-named windows (X11/Win32/None) and the surfaceless stand-in pbuffer neither latch
    // nor are refused. The mode is a loop member reset by ServerLoop::Start, so each session
    // latches on its own; it is not read from MOBILEGL_IPC_SURFACE, because one process's
    // environment cannot speak for every session a unified server runs.
    enum class SessionSurfaceMode : Uint8 {
        None,
        OnScreen,
        Offscreen
    };
    const char* SessionSurfaceModeName(SessionSurfaceMode mode);
    // The decision, pure, so a unit case can drive all six pairs without a backend.
    Bool SessionSurfaceModeAdmits(SessionSurfaceMode current, Bool serverOwnedWindow);

    // P14 S2 (SessionRuntime.h): WHICH SESSION THIS LOOP IS, when it is one of several served by
    // the process. Set by ServerMain::RunSession before Start(); null for every other shape (the
    // inproc one-process client+server, a unit case), which is what keeps those byte-for-byte.
    class SessionRuntime;

    class ServerLoop {
    public:
        // Creates the apply thread, names it mgl-srv-apply, applies
        // MOBILEGL_IPC_SERVER_AFFINITY (borrowing ShaderCompilePool's big-core detection) and
        // LOGS THE RESOLVED MASK - an affinity that silently did nothing is indistinguishable
        // from one that worked, and the split's whole performance claim rests on both halves
        // landing on fast cores.
        MobileGLResult Start(ServerSession& session);

        // P14 S2. The session this loop belongs to, so its apply thread can open the same thread
        // scope the control thread has (session/loop/inputs/latch domain/segment resolver).
        void SetRuntime(SessionRuntime& runtime) { m_runtime = &runtime; }
        SessionRuntime* Runtime() const { return m_runtime; }

        // Kill the doorbell, join the thread (bounded), then destroy the private backend object
        // ON THAT THREAD before it exits. Blocking by contract - see the header note.
        void Stop();
        // P12 review fix: THE NEXT Stop() LEAVES THE QUEUE UNAPPLIED. A Stop() normally lets the
        // apply thread finish its batch and drain what is still queued - the client's own drain
        // bounds that. A SERVER that is stopping (the in-process display server's
        // mobilegl_server_stop_inprocess) has a client that may still be streaming, and a batch plus
        // a final drain of a ring that client keeps refilling can outlast the bounded join
        // (Fatal{ApplyThreadJoinTimeout} aborts the display Activity's process). Called before Stop():
        // the drain stops after the record in hand and the exit path declines the rest, as a
        // ReverseChannelForfeit does. Reset by Start().
        void AbandonQueuedRecords();

        Bool Running() const;

        // The server role's own backend object. NOT pActiveBackendObject: that global holds the
        // client's BackendObject_Remote. Table 3's ruling is that the server holds its
        // BackendObject_DirectGLES privately here, and that the seven backend-internal reads of
        // pActiveBackendObject - ClampSamplesToBackendSupport (BackendObject_DirectGLES.cpp:815,
        // :819) and five in Utils.cpp (:74, :82, :126, :220, :260), all of them format-capability
        // lookups - take the format cache as a parameter instead. That is six functions across
        // two files, and it is why no thread-keyed shim is needed for MOBILEGL_BUILD_DISAGGREGATED_INPROC.
        MG_Backend::BackendObject* Backend();
        // Its server hooks (BackendRoles.h); null with no backend. Asked at each use, never kept: a
        // test may swap the backend under a running apply thread.
        MG_Backend::ServerHooks* BackendHooks();

        // Runs one blocking control request on the apply thread and waits for it; every Server*
        // EGL forwarder funnels through here. Blocking rather than async, because each caller
        // acts on the reply at once. The frame crosses by value: `kind` names the op and the
        // reply fields come back in the same frame; under spawn SurfaceOpCodec encodes it as a
        // Wire::SurfaceOp. When the server is another process the client installs a
        // RemoteControlSink at handshake; with none, the frame goes to the one-slot mailbox, or
        // runs inline when already on the apply thread. A hook rather than a call into
        // ClientSession, so the server half does not depend on the client half.
        using RemoteControlSink = MobileGLResult (*)(void* user, SurfaceControlFrame& frame);
        void SetRemoteControlSink(RemoteControlSink sink, void* user);

        MobileGLResult RunSurfaceControlFrame(SurfaceControlFrame& frame);

        // The TEST seam through the same channel: posts a ProbeForTesting frame whose dispatch
        // runs the given hook on the apply thread. A raw function pointer held in a MEMBER (not
        // in the mailbox slot, which carries the frame and nothing else), because the suites that
        // drive guards and Fatal arms need arbitrary work on the apply thread and the teardown
        // path's no-allocation rule (ID-8) forbids a std::function here. Same discipline as
        // SetBeforeRetireHookForTesting.
        using ControlProbeHook = MobileGLResult (*)(void* user);
        MobileGLResult RunProbeOnApplyThreadForTesting(ControlProbeHook hook, void* user);

        // P7 (p7/spawnhang). WHAT A POSTER SAYS WHILE THE APPLY THREAD RUNS ITS FRAME.
        //
        // A posted frame is waited for in slices of kControlProgressIntervalMs, and after every
        // slice in which the apply thread is RUNNING it - taken, not yet answered - the poster calls
        // this sink with the frame's kind, its seq and how long ago it was posted, WITHOUT
        // m_controlMutex held. ServerMain installs one that sends Wire::SurfaceProgress on the
        // control connection: that is how a spawn / TCP client tells a server BUSY with its op (a
        // cold native bring-up - eglInitialize loading a software rasteriser off a cold disk ran
        // ~20 s on a CI runner) from a SILENT one, and restarts its reply budget. A frame posted but
        // NOT TAKEN reports nothing, because that silence is exactly what the client's budget
        // exists to name. Null (inproc, and every case that does not set one): the slices are
        // waited out and nothing is said. Read on the posting thread, set from any (both under
        // m_controlMutex).
        using ControlProgressSink = void (*)(void* user, SurfaceControlOp kind, Uint64 seq, Uint32 elapsedMs);
        void SetControlProgressSink(ControlProgressSink sink, void* user);
        // Well inside the smallest default reply budget (MOBILEGL_IPC_CONTROL_TIMEOUT_MS, 5000),
        // and a frame's worth of bytes per interval only while an op runs that long.
        static constexpr Uint32 kControlProgressIntervalMs = 250;
        // A dispatch that runs at least this long is logged, with its op, seq and duration, on the
        // apply thread when it returns - the line the retrace-split investigation did not have.
        static constexpr Uint32 kSlowControlDispatchMs = 1000;

        // How many frames this loop has dispatched (every kind, probe included). Reset by
        // Start(). The frame channel's own red-once handle: a forwarder that stopped posting
        // frames leaves this unmoved.
        Uint64 ControlFramesDispatched() const;

        // ---- v1's additions beyond c0's signature block ---------------------------------

        // Constructs the SERVER ROLE's private BackendObject and runs its non-GL Initialize().
        // Called from MG_Backend::Init()'s single hook, BEFORE ClientSession::Start(), because
        // ServerSession::Accept() publishes the first CapsSnapshot from it and because the two
        // CallMask halves - which have no default and Fatal when unset (ServerSession.h) - are
        // answered from what this backend is.
        //
        // NO GL AND NO EGL HAPPENS HERE. BackendObject_DirectGLES::Initialize() loads the
        // driver's entry points; the native context does not exist until the client's first
        // eglMakeCurrent crosses as a blocking control request and runs on the apply thread.
        // That split is what lets the backend object be BUILT on the app thread while its
        // context is never OWNED by it.
        MobileGLResult CreateBackend(BackendType type);

        // True on the apply thread itself. RunSurfaceControlFrame uses it to run inline rather
        // than deadlock when the apply thread posts to itself - which the EGL teardown path does,
        // because ~BackendObject_DirectGLES runs THERE and reaches ReleaseEGLResources.
        //
        // INLINE, AND THAT IS THE POINT (P5d round 3, package D): see the Detail block above.
        // Every role guard in MG_State, MG_Impl, MG_Pipe and MG_Remote/Client asks this once
        // or more per draw, so the call itself - a cross-library PLT hop - was a measurable
        // share of the cost of the answer.
        static Bool OnApplyThread() {
            // P14 S2: the caller's own TLS word. One constant-initialized load, no atomic, no
            // comparison and - unlike the process-wide key - the right answer with N sessions.
            return Detail::t_onApplyThread;
        }

        // The CPU mask MOBILEGL_IPC_SERVER_AFFINITY resolved to and sched_setaffinity accepted.
        // 0 means "no affinity was applied" - the honest answer for `off`, for a platform with
        // no affinity call, and for a failed syscall - and is exactly why the RESOLVED mask is
        // logged rather than the string an operator typed.
        Uint64 ResolvedAffinityMask() const;

        // Diagnostics the tests read. DrainedRecords is how many records this thread has handed
        // to the applier; ParkCount how many times it actually parked. A shutdown test that
        // asserts only "Stop() returned" cannot tell a thread that parked and was woken by Kill
        // from one that never parked at all - which is the R-16 shape of a check that cannot
        // fail for its own reason.
        Uint64 DrainedRecords() const;
        Uint64 ParkCount() const;
        // How many of those waits ran out of spin budget and really blocked. ParkCount is an
        // INTENTION (counted on the way toward a park, C10's note); this is the outcome, and
        // the pair is what says whether MOBILEGL_IPC_SPIN_US is sized for the workload. It is
        // this loop's OWN tally rather than the consumer bell's ParkEntries() because a bell
        // belongs to an endpoint and counts every waiter on it; only a tally passed into the
        // one Wait this loop makes is guaranteed to stay a subset of ParkCount().
        Uint64 ParkBlockCount() const;
        // Scheduling perturbation only: the hook runs after application, before retirement.
        // Integration tests use it to observe real producer back-pressure from GL uploads.
        void SetBeforeRetireHookForTesting(void (*hook)()) {
            m_beforeRetireHook.store(hook, std::memory_order_release);
        }
        // Scheduling point only, same discipline: DrainRing runs the hook on the apply thread
        // BETWEEN two records - after a popped record's own checks, before the next pop's latch
        // check - and never on an empty-ring poll. ServerLoopLatchTest latches from a second
        // thread there, which is the interleaving codex closeout finding 6 named.
        void SetBetweenRecordsHookForTesting(void (*hook)()) {
            m_betweenRecordsHook.store(hook, std::memory_order_release);
        }

        // C7 / ID-54 diagnostics, read by ServerLoopTest's C7 and N-3 controls. NativeBindCount is
        // how many times ApplyMakeCurrent FORWARDED a bind to the backend (a tuple it did not
        // hold); ClientReleaseCount how many client release-current requests were recorded and
        // not forwarded. Two identical binds must move the first by one and the second not at
        // all. The number of native eglMakeCurrent calls the DRIVER saw is a different number -
        // the backend object skips the native call for a surface already current (header block)
        // - and the control reads that one at the EGL function table, not here.
        Uint64 NativeBindCount() const;
        Uint64 ClientReleaseCount() const;
        // ID-67: how many caps snapshots ServerMakeEGLCurrent has re-published (R-12 arm (a)) - one
        // per forwarded bind of a tuple it did not hold, never for an identical repeat, so the
        // client's mirror generation moves exactly when the server's answers could have.
        Uint64 MakeCurrentRepublishCount() const;
        void NoteMakeCurrentRepublished();

        // The deduped make-current, on the apply thread. Classifies the request (see
        // ClassifyEglMakeCurrent), forwards a native bind only for a genuinely new tuple, records
        // a release without forwarding it, and returns whether a native bind happened so the
        // caller (ServerMakeEGLCurrent) knows whether to re-publish the caps snapshot (R-12).
        struct MakeCurrentOutcome {
            Bool ok = false;            // the request was honoured
            Bool boundNatively = false; // a native eglMakeCurrent ran (=> republish caps)
        };
        MakeCurrentOutcome ApplyMakeCurrent(MG_Backend::BackendObject* backend, EGLDisplay dpy, EGLSurface draw,
                                            EGLSurface read, EGLContext ctx);

        // N-3: forget the tuple ApplyMakeCurrent last bound. Apply thread only, like the tuple
        // itself (the forwarders that call these run their Args::Run there). Called on every
        // event after which the native context that tuple named may no longer exist -
        // ReleaseEGLResources, ReleaseEGLSurface of a surface the tuple names, a surface
        // (re)creation (BackendObject_DirectGLES destroys the context to create a different
        // surface), backend destruction - so the next make-current with the SAME handle values
        // (EGL handles are recycled; on this host every one of them is literally 0x1) is
        // classified as a real bind, not as a RepeatNoOp that binds nothing, runs no base-class
        // bookkeeping and republishes no caps. Forgetting is always safe: the cost of a
        // forgotten-but-still-current tuple is one forwarded bind the backend object dedups
        // natively; the cost of a remembered-but-dead one is a silent no-context-current.
        void ForgetCurrentTuple();
        void ForgetCurrentTupleIfItNames(EGLSurface surface);

        // Part of the apply thread's park predicate: a posted control request must un-park a
        // thread waiting on kWaitForever, which a Notify alone cannot do. It reads only
        // m_controlPosted, without the lock, because the idle poll calls it on every iteration and
        // a mutex here cost about half the apply thread's cycles. It means "posted and not yet
        // taken": the pump clears it when it takes the work, so it reads false while the work runs
        // and the poster still waits. Not a liveness query; the only reader is the apply thread's
        // `ready` lambda. Public so ServerLoopTest can catch a flag left set, which would stop the
        // loop from ever parking.
        Bool ControlIsPending() const;

        // ---- P12 (on-screen server window), D3/D6 -------------------------------------------
        //
        // How long a ServerOwned creation waits for the display server's window (D3: ~10 s). The
        // control pump's SurfaceProgress heartbeat keeps the client's reply budget alive meanwhile.
        static constexpr Uint32 kServerWindowWaitMs = ServerDisplay::kDefaultAcquireTimeoutMs;

        // APPLY THREAD ONLY. Leases the process display's window for this session (ServerDisplay::
        // AcquireFor with this loop as the holder), refusing by name when there is no display
        // (NoServerDisplay, NOT a latch - a configuration answer) or no window within `timeoutMs`
        // (NoServerWindow). The ServerOwned arm of the dispatch is its production caller; a test
        // reaches it through RunProbeOnApplyThreadForTesting. The lease is held until the backend
        // has let go of the window: ServerDisplay::Detach's lost hook (D6) or the session's end.
        MobileGLResult AcquireServerWindow(Uint32 width, Uint32 height, Uint32 timeoutMs, ServerWindowLease* out,
                                           SurfaceRefusalCode* refusal);
        // APPLY THREAD ONLY: this session's surface mode (D4).
        SessionSurfaceMode SurfaceMode() const { return m_surfaceMode; }
        // How many times this loop released a lost server window and latched ServerWindowLost (D6) -
        // the path of a backend that cannot suspend.
        Uint64 ServerWindowsLost() const { return m_serverWindowsLost.load(std::memory_order_acquire); }
        // SUSPEND, NOT END. How many times a lost server window SUSPENDED this session (its backend let
        // go of the window and kept everything else) and how many times a new window resumed it. A
        // suspended session holds no lease and keeps applying records; its window surfaces draw into
        // a placeholder until the display attaches a window again.
        Uint64 ServerWindowsSuspended() const { return m_serverWindowsSuspended.load(std::memory_order_acquire); }
        Uint64 ServerWindowsResumed() const { return m_serverWindowsResumed.load(std::memory_order_acquire); }
        // How many times a resize of the leased window was published to the client (the display's
        // surfaceChanged, answered on the apply thread without waiting for a present).
        Uint64 ServerWindowResizesPublished() const { return m_serverWindowResizes.load(std::memory_order_acquire); }
        Bool ServerWindowSuspended() const { return m_serverWindowSuspended.load(std::memory_order_acquire); }

        // TEST SEAM: the backend this loop owns, instead of CreateBackend's (a fake that can or cannot
        // suspend, for the window-loss cases). Before Start(); fails when one is already installed.
        MobileGLResult InstallBackendForTesting(UniquePtr<MG_Backend::BackendObject> backend);

        // PROCESS-WIDE TEST SEAM for a server forked from a test (every session's loop reads it; null -
        // the default - is the production path): the backend surface behind a ServerOwned window
        // surface. A host has no window a backend can draw into, so a case that serves a stand-in
        // window gives the backend a surface of the window's extent another way (a pbuffer). Called on
        // the apply thread with the lease held, in place of CreateEGLWindowSurface.
        using ServerOwnedSurfaceHook = Bool (*)(MG_Backend::BackendObject* backend, EGLSurface surface,
                                                const MG_Backend::WindowHandle& window);
        static void SetServerOwnedSurfaceHookForTesting(ServerOwnedSurfaceHook hook);

    private:
        RemoteControlSink m_remoteSink = nullptr;
        void* m_remoteSinkUser = nullptr;

        // P12. The ServerOwned arm of the CreateWindowSurface dispatch (D3, D4).
        MobileGLResult ApplyServerOwnedWindowSurface(MG_Backend::BackendObject* backend, SurfaceControlFrame& frame);
        // P12 (D6), apply thread: the lost window is let go of before the lease ends. A backend that
        // can suspend releases only what is bound to the window and the session carries on, waiting
        // for the next window (SuspendLease); otherwise the backend's EGL resources go and the session
        // latches ServerWindowLost. Runs from PumpControlRequest / DrainRing when Detach asked.
        void ReleaseLostServerWindow();
        // The old ending: the backend's EGL resources go, the session latches ServerWindowLost by
        // name with `why`, the lease ends.
        void LatchServerWindowLost(const char* why);
        // Apply thread: the window requests ServerWindowLostThunk / ServerWindowAttachedThunk posted.
        void HandleServerWindowRequests();
        // Apply thread: a suspended session leases the display's new window and its backend
        // rebuilds the suspended surfaces on it.
        void ResumeSuspendedServerWindow();
        // Apply thread, holding a fresh lease on `lease.window`: the backend rebuilds what it
        // suspended. False: it could not, and the session latched (LatchServerWindowLost).
        Bool ResumeServerWindowOnLease(const ServerWindowLease& lease);
        // Ends this loop's display lease if it holds one (after the backend let go of the window).
        void EndServerWindowLease();
        // The session's end: the lease, and any wait for a window to resume on.
        void ForgetServerWindow();
        // Review fix: the ResizeWindowSurface arm for a server-owned surface - a geometry request to
        // the display, the backend surface at the window's real extent, that extent in the reply.
        MobileGLResult ApplyServerOwnedWindowResize(MG_Backend::BackendObject* backend, SurfaceControlFrame& frame);
        Bool IsServerOwnedSurface(EGLSurface surface) const;
        void ForgetServerOwnedSurface(EGLSurface surface);
        // Apply thread only: this session's surfaces created on the server's window; reset by Start().
        std::vector<EGLSurface> m_serverOwnedSurfaces;
        // ServerDisplay's lost hook: called under the display's lock from Detach's thread. Sets the
        // request and rings the apply thread's bell; never blocks.
        static void ServerWindowLostThunk(void* self);
        // ServerDisplay's attached hook for a suspended session: the same discipline as the lost one.
        static void ServerWindowAttachedThunk(void* self);
        // ServerDisplay's resized hook for the lease holder: the same discipline again.
        static void ServerWindowResizedThunk(void* self);
        // Apply thread: the leased window's new extent (the display's) is handed to the backend, which
        // publishes it to the client now.
        void PublishServerWindowResize();
        // Apply thread: the extent a backend publishes for the server window from here on (MGPipe's), and
        // what this session last handed it.
        void NoteServerWindowExtent(void* window, Uint32 width, Uint32 height);
        // AcquireFor's cancel predicate: the loop is stopping, or the session latched.
        static Bool ServerWindowWaitCancelled(void* self);
        // D4: latched at the session's first successful surface creation; reset by Start(). Per
        // session, because the loop is (P14 S2).
        SessionSurfaceMode m_surfaceMode = SessionSurfaceMode::None;
        // P14 S2: the session this loop serves, when the process serves several; see SetRuntime.
        SessionRuntime* m_runtime = nullptr;
        // Apply thread only: this loop holds the display's lease, on this window.
        Bool m_holdsWindowLease = false;
        void* m_leasedWindow = nullptr;
        // The window requests: kWindowLost set by ServerWindowLostThunk, kWindowAttached by
        // ServerWindowAttachedThunk (any thread), taken by the apply thread. ONE word, so the idle
        // poll and DrainRing's per-pop check stay one load. Part of the park predicate, so a parked
        // apply thread wakes for it.
        static constexpr Uint32 kWindowLost = 1u;
        static constexpr Uint32 kWindowAttached = 2u;
        static constexpr Uint32 kWindowResized = 4u;
        std::atomic<Uint32> m_windowRequests{0};
        std::atomic<Uint64> m_serverWindowsLost{0};
        // Written by the apply thread, read by tests.
        std::atomic<Bool> m_serverWindowSuspended{false};
        std::atomic<Uint64> m_serverWindowsSuspended{0};
        std::atomic<Uint64> m_serverWindowsResumed{0};
        // Apply thread only: the geometry this session last asked the display for (0/0 = the
        // layout's), which a resume asks for again.
        Uint32 m_serverWindowWantWidth = 0;
        Uint32 m_serverWindowWantHeight = 0;
        // Apply thread only: the leased window's extent this session last published.
        Uint32 m_publishedWindowWidth = 0;
        Uint32 m_publishedWindowHeight = 0;
        std::atomic<Uint64> m_serverWindowResizes{0};

        void ApplyThreadMain();
        // Runs a posted control frame, if there is one. Returns true if it ran one.
        Bool PumpControlRequest();
        // The dispatch, on the apply thread (or inline for a re-entrant post): executes the
        // frame's op against the private backend and fills the frame's reply half. The eleven
        // wire+inproc op bodies from the old forwarder Args structs live here now.
        MobileGLResult ApplySurfaceControlFrame(SurfaceControlFrame& frame, ControlProbeHook probeHook = nullptr,
                                                void* probeUser = nullptr);
        // The caller already holds m_callerMutex; shared by normal and test-probe posting so
        // the probe's metadata is protected for the same lifetime as its frame.
        MobileGLResult PostSurfaceControlFrameWithCallerLock(SurfaceControlFrame& frame);
        // Pops and applies every record currently in the ring; returns how many it applied.
        // `polledHead`: the command ring's head as loaded just before the caller's own fault poll
        // (0 when the caller did not poll) - records below it need no poll of their own.
        Uint64 DrainRing(std::uint64_t polledHead = 0);
        void SignalExited();

        ServerSession* m_session = nullptr;
        std::atomic<Bool> m_running{false};
        std::atomic<Bool> m_stopRequested{false};
        // P12 review fix: AbandonQueuedRecords() - read after every popped record and at the exit.
        std::atomic<Bool> m_abandonQueue{false};
        std::thread m_thread;
        // The apply thread's identity used to live here as an atomic<std::thread::id>. It is
        // Detail::g_applyThreadKey now - see the block at the top of this header for why the
        // identity had to stop being a member reachable only through ServerLoopInstance().

        // The private backend object. Destroyed ON the apply thread while it still owns the
        // context - see Stop().
        UniquePtr<MG_Backend::BackendObject> m_backend;

        // The blocking control channel. ONE slot, because the verb barrier already leaves one
        // client thread runnable at a time; m_callerMutex serialises anything that is not.
        // The slot carries ONE SurfaceControlFrame BY VALUE (P5f, package fc) - no function
        // pointer, no address of caller storage. The poster's frame is copied in at publish and
        // the dispatch's reply half is copied back over it before m_controlDone is signalled.
        std::mutex m_callerMutex;
        std::mutex m_controlMutex;
        // THE MAILBOX'S ONE-BIT SHADOW, and the only field of it the idle poll may read.
        // Written to `true` by the poster under m_controlMutex AFTER m_controlPending, and
        // back to `false` under the same lock by whoever TAKES the request (PumpControlRequest,
        // or ApplyThreadMain's exit block when it answers NOT_INITIALIZED). A reader that sees
        // `true` through the release/acquire pair therefore also sees m_controlPending; a
        // reader that sees `false` cannot be missing a request, because the store precedes the
        // poster's Notify and Doorbell::Wait re-tests its predicate after every Park return.
        // The mutex still owns every other field - this is a fast NEGATIVE answer, not a second
        // copy of the mailbox.
        //
        // It replaces a std::condition_variable of the same name that had no waiter and no
        // notifier anywhere in the tree (m_controlDone is the one that carries the handshake).
        std::atomic<Bool> m_controlPosted{false};
        std::condition_variable m_controlDone;
        // The one slot. A frame by value; the reply half is written through it on the way back.
        SurfaceControlFrame m_controlFrame;
        MobileGLResult m_controlResult = MOBILEGL_OK;
        Bool m_controlPending = false;
        Bool m_controlFinished = false;
        // Minted per posted frame (0 = never posted) and the dispatch tally beside it. Both
        // atomic: the re-entrant inline arm mints without taking m_callerMutex.
        std::atomic<Uint64> m_controlSeq{0};
        std::atomic<Uint64> m_controlFramesDispatched{0};
        // The test probe (RunProbeOnApplyThreadForTesting). Members, NOT slot content - the slot
        // carries only the frame. Written under m_callerMutex before the probe frame is
        // published; held until its reply returns. Re-entrant probes use call-local arguments.
        std::atomic<ControlProbeHook> m_controlProbeHook{nullptr};
        std::atomic<void*> m_controlProbeUser{nullptr};
        // SetControlProgressSink's pair (p7/spawnhang). Guarded by m_controlMutex: the poster reads
        // both while it holds that lock between slices, so the pair is always read whole.
        ControlProgressSink m_progressSink = nullptr;
        void* m_progressUser = nullptr;

        // The BOUNDED join's other half. std::thread::join has no deadline, so a lost wakeup
        // would wedge CI rather than fail it; the thread signals here last and Stop() waits
        // with a deadline (InProcessTransportTest.cpp:344's five seconds).
        std::mutex m_exitMutex;
        std::condition_variable m_exitCv;
        Bool m_exited = false;
        // True only while mgl-srv-apply is parked on its consumer bell - the one place a lost
        // wakeup can strand it. Stop()'s join reads it to tell a lost wakeup (parked: Fatal at
        // the bound) from a thread that is BUSY in the backend (a GPU stall: waited for longer).
        std::atomic<Bool> m_applyParked{false};

        Uint64 m_affinityMask = 0;
        std::atomic<Uint64> m_drained{0};
        std::atomic<Uint64> m_parks{0};
        // Incremented by Doorbell::Wait through its `parkTally` out-parameter, once per wait
        // that reached the blocking Park. Relaxed everywhere: it is a gauge.
        std::atomic<Uint64> m_parkBlocks{0};
        std::atomic<void (*)()> m_beforeRetireHook{nullptr};
        std::atomic<void (*)()> m_betweenRecordsHook{nullptr};
        // P9 W3, fault injection F2: MOBILEGL_TEST_APPLY_LOG_FLOOD, read by Start(). Zero - the only
        // value a production run has - makes the per-record test a load of this and nothing else.
        Uint32 m_testLogFloodLines = 0;

        // C7 / ID-54: the (dpy, draw, read, ctx) currently bound on the apply thread. Written and
        // read ONLY on the apply thread inside ApplyMakeCurrent, so it needs no lock; the two
        // counters beside it are atomic because a test reads them from another thread.
        //
        // P14 S4 LOOKED AT MAKING THIS A PER-CONTEXT TABLE AND DID NOT, with the reason recorded
        // here rather than in a review thread: ID-67's pinned control
        // (ServerLoopEglTest.ADifferentTupleBindsNativelyAndRepublishesAnIdenticalRepeatDoesNeither)
        // defines "a different tuple" as a change of the CURRENTLY BOUND tuple, and asserts that
        // A -> B -> A costs three native binds. A table that remembered each context's tuple would
        // make the third one a RepeatNoOp - and it would be wrong for a real reason, not just for
        // the assertion: only ONE EGLContext can be current on the apply thread, so coming back to A
        // really does have to run the driver call that DirectGLES' seven frontend-context
        // invalidations hang off. "Each context holds its own native triple" is satisfied by the
        // backend's per-(session, token) registry; the dedup here is about the thread's CURRENT
        // binding and stays one tuple.
        Bool m_haveCurrentTuple = false;
        EGLDisplay m_curDpy = EGL_NO_DISPLAY;
        EGLSurface m_curDraw = EGL_NO_SURFACE;
        EGLSurface m_curRead = EGL_NO_SURFACE;
        EGLContext m_curCtx = EGL_NO_CONTEXT;
        std::atomic<Uint64> m_nativeBinds{0};
        std::atomic<Uint64> m_clientReleases{0};
        std::atomic<Uint64> m_makeCurrentRepublishes{0};
    };

    ServerLoop& ServerLoopInstance();

    // C7 / ID-54, factored out so a unit case can drive the DECISION without a live EGL context
    // (the native bind itself needs the joint lane). Given the tuple currently held on the apply
    // thread and the request, is this a native bind, an identical no-op repeat, or a client
    // release-current the server must record without forwarding?
    enum class EglBindAction {
        NativeBind,
        RepeatNoOp,
        ClientRelease
    };
    EglBindAction ClassifyEglMakeCurrent(Bool haveCurrent, EGLDisplay curDpy, EGLSurface curDraw, EGLSurface curRead,
                                         EGLContext curCtx, EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                                         EGLContext ctx);

    // P11 B1 (CONTRACT-P11 B1): the cpu core this process's `auto` policy reserves for a dialled-in
    // shared-segment client's apply thread (ApplyThreadPolicy.h ReservedApplyCoreForConfig over this
    // machine's topology and MOBILEGL_IPC_SERVER_AFFINITY), 0 when none; `outRule` receives the reason.
    // The supervisor announces it at start-up; the server app's broker relays it to the helper.
    Uint64 ReservedApplyCoreForThisProcess(const char** outRule);

    // The EGL control forwarders. BackendObject_Remote's EGL virtuals call these, so the app
    // thread never reaches the driver's eglMakeCurrent: each packs its arguments into a
    // SurfaceControlFrame and blocks in RunSurfaceControlFrame while mgl-srv-apply runs it against
    // the server's backend (ARCHITECTURE.md §15.5). ServerReleaseEGLResources must block, and so
    // must ~BackendObject_DirectGLES, which ServerLoop::Stop() runs on the apply thread before it
    // exits; otherwise teardown runs on while the server still holds the context. Free functions,
    // because the client needs only these calls. SwapBuffers and InitWindowSurface are
    // inproc-only frame kinds with no wire op: present travels as a record.
    Bool ServerInitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor);
    Bool ServerCreateEGLWindowSurface(EGLSurface surface, const MG_Backend::WindowHandle& handle);
    Bool ServerResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height);
    // P12: `refusal`, when given, receives the server's named refusal (SurfaceModeMismatch for a
    // pbuffer in an on-screen session), None otherwise. `flags`: kPbufferFlagSurfacelessStandIn
    // for the hidden pbuffer of a surfaceless make-current, which takes no surface mode.
    Bool ServerCreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height,
                                       SurfaceRefusalCode* refusal = nullptr, Uint32 flags = 0);
    // P12 (on-screen server window), D1/D2. CreateWindowSurface on the SERVER's window: one frame,
    // WindowKind::ServerOwned with nativeToken 0 on the wire, no SetWindowHandle. `width`/`height`
    // is the size the client asked for (0/0 = the server window's own size); the reply carries the
    // window's REAL extent, and the server's named refusal when it declined.
    struct ServerOwnedWindowReply {
        Bool ok = false;
        MobileGLResult transport = MOBILEGL_OK; // the frame channel's own answer
        SurfaceRefusalCode refusal = SurfaceRefusalCode::None;
        Uint32 width = 0;
        Uint32 height = 0;
    };
    ServerOwnedWindowReply ServerCreateServerOwnedWindowSurface(EGLSurface surface, Uint32 width, Uint32 height);
    // P12 review fix: ResizeWindowSurface for a surface created on the server's window. The server
    // resizes its WINDOW (a geometry request) and replies with the window's real extent.
    ServerOwnedWindowReply ServerResizeServerOwnedWindowSurface(EGLSurface surface, Uint32 width, Uint32 height);
    // Also RE-PUBLISHES THE CAPS SNAPSHOT on success (R-12). BackendObject::MakeEGLCurrent runs
    // InitCapabilities() on the first make-current per surface (BackendObject.cpp:341-347), so
    // this is the moment the server's answers stop being the empty ones Accept() published -
    // and a SECOND arrival IS the invalidation signal, which is how DirectGLES, which has no
    // OnCapsInvalidated producer at all, tells the client without a dev-shaped backend edit.
    Bool ServerMakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
    // P14 S1 (docs/Disaggregated/design/11-state-ownership.md). The EGL CONTEXT LIFECYCLE's two
    // frames, on the same channel and with the same blocking handshake as the twelve above.
    // `clientContextToken` is the token EGLState minted for the new context (never 0), and
    // `shareGroupToken` the group it joins - the sharing context's group, or a fresh one.
    // S1 creates a PLACEHOLDER server-side runtime: the token enters the session's table and
    // nothing native is built (S4 is what gives the entry a native context). DestroyContext
    // drops the entry and unbinds it if it was the current one.
    Bool ServerCreateEGLContext(Uint64 clientContextToken, Uint64 shareGroupToken, Uint32 flags);
    Bool ServerDestroyEGLContext(Uint64 clientContextToken);
    Bool ServerSwapEGLBuffers(EGLDisplay dpy, EGLSurface draw);
    void ServerSetEGLSwapInterval(Int interval);
    void ServerReleaseEGLSurface(EGLSurface surface);
    void ServerReleaseEGLResources();
    Bool ServerInitCapabilities();
    Bool ServerInitWindowSurface();
    void ServerSetWindowHandle(const MG_Backend::WindowHandle& handle);

} // namespace MobileGL::MG_Remote::Server
