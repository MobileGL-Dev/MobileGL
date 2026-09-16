// MobileGL - MobileGL/MG_Remote/Server/ServerLoop.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The apply thread and the server's private backend object. Owner: package v1 - the highest
// risk item in P5. Signatures by c0.
//
// WHY THE THREAD IS THE POINT. DirectGLES has 16 IsBackendContextCurrentOnThisThread() guards
// (DirectGLES.cpp:12034..12428) and Managers.cpp has 16 CanTouchGLNow() guards (:1494..:3428);
// every one of them DEGRADES when the answer is false - fences become always-signaled, queries
// return null handles, Present creates no frame fence so the buffer pool's recycle watermark
// never advances, and the two persistent-map acquisitions (Managers.cpp:1494, :2170) DECLINE,
// which would make PersistentCoherentMapScenario unreachable. Making the apply thread the
// context owner for life turns all 32 of those answers true on the server and removes the
// whole degradation class at once. It is also exactly the shape P6's spawned server inherits.
//
// P5 BUILDS ONE THREAD, NOT TWO. No mgl-srv-io: inproc's control plane is in the same process.
// P6 splits it.
//
// PARKING AND SHUTDOWN. The thread parks on Doorbell::Wait(consumerParked, ready, spinUs,
// kWaitForever) and shuts down when Wait returns false with Dead() set. TWO things can un-park a
// kWaitForever waiter, not one, and the difference is the whole of review m-4: for the STOP case a
// plain Doorbell::Notify() is sufficient, because m_stopRequested is in the park predicate and
// Stop() publishes it BEFORE it rings (Doorbell.h re-tests ready() after every Park return);
// Doorbell::Kill() (Doorbell.h:211-221) is load-bearing only for a predicate that has NOTHING to
// see, which is why the redcheck's park-predicate-loses-control entry - the one that drops the
// control flag FROM the predicate - is the case that actually times out. Kill BEFORE join; join
// before the client frees any emitter-owned Vector; and the join must be bounded (that test uses
// 5 s) so a regression is a red test and not a hung CI job.
//
// THE EGL OWNERSHIP MOVE, AS MEASURED (ID-54; review v2 item 10 and N-3). The native
// eglMakeCurrent for a surface runs ONCE PER CONTEXT LIFETIME on this thread and the context is
// then held for life. That "once" lives in TWO layers, because one is not enough:
// DirectGLES::MakeCurrent always calls native eglMakeCurrent and rewrites the owner (codex C7),
// and it is reached twice per bring-up - once from InitPbufferSurface/InitWindowSurface when the
// surface is CREATED, and once more from the client's first eglMakeCurrent. So (1)
// ServerMakeEGLCurrent classifies the request against the tuple it last bound
// (ClassifyEglMakeCurrent): an identical repeat is a no-op, and the R-12 republish decision for
// it is "nothing to republish" (ID-67: the client's mirror generation must not move); a different
// tuple is a real forwarded bind AND a caps republish; a client release-current is RECORDED
// (ClientReleaseCount) and NOT forwarded. And (2) BackendObject_DirectGLES::MakeEGLCurrent, under
// an active transport only, skips the native call when the requested draw surface is the one
// already natively current on this thread (IsBackendContextCurrentOnThisThread, which is EGL
// ground truth) AND the virtual tuple is the one that bind was for - or the bind is the surface's
// own creation, which the first tuple adopts; a different virtual context onto the same surface
// binds natively again, because MakeCurrent's invalidations describe the frontend context that
// changed (ID-67). Measured at the EGL function
// table by ServerLoopTest's C7 control on a real llvmpipe context: surface creation + two
// identical make-currents + a client release + a bind after the release = ONE native
// eglMakeCurrent, ZERO native releases, and the apply thread still the owner afterwards. So
// DirectGLES.cpp's six cache invalidations run once per context lifetime rather than once per
// client make-current, and the 16 IsBackendContextCurrentOnThisThread() / 16 CanTouchGLNow()
// sites answer TRUE on the server. The tuple is FORGOTTEN (N-3) on every event after which the
// native context it names may be gone - ReleaseEGLResources, ReleaseEGLSurface of the surface it
// names, a surface (re)creation, backend destruction - so a recycled handle value after a destroy
// is a real bind again and never a silent no-context-current. The client's nine EGL virtuals
// become BLOCKING control requests executed here. ReleaseEGLResources and
// ~BackendObject_DirectGLES MUST be blocking: MobileGL::Destroy() (MobileGL/Init.cpp:68)
// otherwise walks on while the server still holds the context.
//
// THE FALLBACK IS PRE-DECLARED, NOT INVENTED UNDER PRESSURE (R-1). If the context migration is
// still not running ClearThenReadPixels at the end of v1's fourth working day, the integrator -
// not the package - declares `inproc-inline`: the client thread drains the ring itself, no
// thread is created, no context migrates, and a second package picks up the thread arm.

#pragma once
#include <Includes.h>

#include "ServerSession.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace MobileGL::MG_Remote::Server {

    class ServerLoop {
    public:
        // Creates the apply thread, names it mgl-srv-apply, applies
        // MOBILEGL_IPC_SERVER_AFFINITY (borrowing ShaderCompilePool's big-core detection) and
        // LOGS THE RESOLVED MASK - an affinity that silently did nothing is indistinguishable
        // from one that worked, and the split's whole performance claim rests on both halves
        // landing on fast cores.
        MobileGLResult Start(ServerSession& session);

        // Kill the doorbell, join the thread (bounded), then destroy the private backend object
        // ON THAT THREAD before it exits. Blocking by contract - see the header note.
        void Stop();

        Bool Running() const;

        // The server role's own backend object. NOT pActiveBackendObject: that global holds the
        // client's BackendObject_Remote. Table 3's ruling is that the server holds its
        // BackendObject_DirectGLES privately here, and that the seven backend-internal reads of
        // pActiveBackendObject - ClampSamplesToBackendSupport (BackendObject_DirectGLES.cpp:815,
        // :819) and five in Utils.cpp (:74, :82, :126, :220, :260), all of them format-capability
        // lookups - take the format cache as a parameter instead. That is six functions across
        // two files, and it is why no thread-keyed shim is needed for MOBILEGL_BUILD_DISAGGREGATED_INPROC.
        MG_Backend::BackendObject* Backend();

        // Run one blocking control request on the apply thread and wait for it. This is how all
        // nine EGL lifecycle virtuals cross; it is deliberately NOT a queue of async messages,
        // because every one of them has a return value the caller acts on immediately.
        //
        // A raw function pointer plus a user pointer, not std::function: this runs on the
        // teardown path too, and the teardown path may not allocate - ID-8's leak-at-exit rule
        // exists because frontend destructors reach here from exit handlers.
        using ControlWork = MobileGLResult (*)(void* user);
        MobileGLResult RunOnApplyThread(ControlWork work, void* user);

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

        // True on the apply thread itself. RunOnApplyThread uses it to run inline rather than
        // deadlock when the apply thread posts to itself - which the EGL teardown path does,
        // because ~BackendObject_DirectGLES runs THERE and reaches ReleaseEGLResources.
        static Bool OnApplyThread();

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
        // Scheduling perturbation only: the hook runs after application, before retirement.
        // Integration tests use it to observe real producer back-pressure from GL uploads.
        void SetBeforeRetireHookForTesting(void (*hook)()) {
            m_beforeRetireHook.store(hook, std::memory_order_release);
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
        MakeCurrentOutcome ApplyMakeCurrent(MG_Backend::BackendObject* backend, EGLDisplay dpy,
                                            EGLSurface draw, EGLSurface read, EGLContext ctx);

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

    private:
        void ApplyThreadMain();
        // Part of the apply thread's park predicate: a posted control request must be able to
        // un-park a thread waiting on kWaitForever, which a Notify alone cannot do.
        Bool ControlIsPending() const;
        // Runs a posted control request, if there is one. Returns true if it ran one.
        Bool PumpControlRequest();
        // Pops and applies every record currently in the ring; returns how many it applied.
        Uint64 DrainRing();
        void SignalExited();

        ServerSession* m_session = nullptr;
        std::atomic<Bool> m_running{false};
        std::atomic<Bool> m_stopRequested{false};
        std::thread m_thread;
        std::atomic<std::thread::id> m_applyThreadId{};

        // The private backend object. Destroyed ON the apply thread while it still owns the
        // context - see Stop().
        UniquePtr<MG_Backend::BackendObject> m_backend;

        // The blocking control mailbox. ONE slot, because the verb barrier already leaves one
        // client thread runnable at a time; m_callerMutex serialises anything that is not.
        std::mutex m_callerMutex;
        std::mutex m_controlMutex;
        std::condition_variable m_controlPosted;
        std::condition_variable m_controlDone;
        ControlWork m_controlWork = nullptr;
        void* m_controlUser = nullptr;
        MobileGLResult m_controlResult = MOBILEGL_OK;
        Bool m_controlPending = false;
        Bool m_controlFinished = false;

        // The BOUNDED join's other half. std::thread::join has no deadline, so a lost wakeup
        // would wedge CI rather than fail it; the thread signals here last and Stop() waits
        // with a deadline (InProcessTransportTest.cpp:344's five seconds).
        std::mutex m_exitMutex;
        std::condition_variable m_exitCv;
        Bool m_exited = false;

        Uint64 m_affinityMask = 0;
        std::atomic<Uint64> m_drained{0};
        std::atomic<Uint64> m_parks{0};
        std::atomic<void (*)()> m_beforeRetireHook{nullptr};

        // C7 / ID-54: the (dpy, draw, read, ctx) currently bound on the apply thread. Written and
        // read ONLY on the apply thread inside ApplyMakeCurrent, so it needs no lock; the two
        // counters beside it are atomic because a test reads them from another thread.
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
    enum class EglBindAction { NativeBind, RepeatNoOp, ClientRelease };
    EglBindAction ClassifyEglMakeCurrent(Bool haveCurrent, EGLDisplay curDpy, EGLSurface curDraw,
                                         EGLSurface curRead, EGLContext curCtx, EGLDisplay dpy,
                                         EGLSurface draw, EGLSurface read, EGLContext ctx);

    // ---------------------------------------------------------------------------------
    // THE EGL OWNERSHIP MOVE - the part that can sink the phase, expressed as twelve calls
    // ---------------------------------------------------------------------------------
    //
    // Under split the app thread must never reach the driver's eglMakeCurrent. Today it does:
    // EGLImpl.cpp:284 -> BackendObject_DirectGLES.cpp:963 -> DirectGLES.cpp:11925, and the
    // owner slot g_backendContextOwnerThread (DirectGLES.cpp:11865) is stamped with whatever
    // thread got there. So BackendObject_Remote's nine EGL virtuals - package c1's - call these
    // twelve, each of which is a BLOCKING control request that runs the SERVER's backend object
    // on mgl-srv-apply. The native eglMakeCurrent then runs once per context lifetime on that
    // thread (the surface's own creation binds; the client's make-currents onto that surface
    // are deduped at both layers, header block above) and a client release is never forwarded,
    // so g_backendContextOwnerThread is written once per context lifetime, DirectGLES.cpp's six
    // cache invalidations run once per context lifetime rather than once per client
    // make-current, the per-frame EGL re-verification stamp holds, and the 16
    // IsBackendContextCurrentOnThisThread() sites plus the 16 CanTouchGLNow() sites answer TRUE
    // on the server instead of silently degrading. Measured, not assumed: ServerLoopTest's C7
    // control counts the driver's eglMakeCurrent calls at the EGL function table.
    //
    // TWO OF THEM MUST BLOCK OR THE PROCESS TEARS ITS OWN CONTEXT DOWN UNDER ITSELF:
    // ReleaseEGLResources (reached from EGLImpl.cpp:326, which for DirectGLES runs
    // DestroyEGLContext) and ~BackendObject_DirectGLES (reached from
    // pActiveBackendObject.reset() at MobileGL/Init.cpp:68). Both are blocking here - the first
    // by being one of these calls, the second because ServerLoop::Stop() destroys the private
    // backend ON the apply thread before that thread exits and Stop() itself waits.
    //
    // WHY THEY ARE FREE FUNCTIONS AND NOT MEMBERS: c1 needs exactly this surface and nothing
    // else of the server, so the seam between the two packages is a list of twelve signatures
    // rather than a class with a lifecycle.
    Bool ServerInitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor);
    Bool ServerCreateEGLWindowSurface(EGLSurface surface, const MG_Backend::WindowHandle& handle);
    Bool ServerResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height);
    Bool ServerCreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height);
    // Also RE-PUBLISHES THE CAPS SNAPSHOT on success (R-12). BackendObject::MakeEGLCurrent runs
    // InitCapabilities() on the first make-current per surface (BackendObject.cpp:341-347), so
    // this is the moment the server's answers stop being the empty ones Accept() published -
    // and a SECOND arrival IS the invalidation signal, which is how DirectGLES, which has no
    // OnCapsInvalidated producer at all, tells the client without a dev-shaped backend edit.
    Bool ServerMakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
    Bool ServerSwapEGLBuffers(EGLDisplay dpy, EGLSurface draw);
    void ServerSetEGLSwapInterval(Int interval);
    void ServerReleaseEGLSurface(EGLSurface surface);
    void ServerReleaseEGLResources();
    Bool ServerInitCapabilities();
    Bool ServerInitWindowSurface();
    void ServerSetWindowHandle(const MG_Backend::WindowHandle& handle);

} // namespace MobileGL::MG_Remote::Server
