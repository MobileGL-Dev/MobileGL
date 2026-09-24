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
// Doorbell::Kill() (Doorbell.h, CondVarDoorbell::Kill) is load-bearing only for a predicate
// that has NOTHING to see, which is why the redcheck's park-predicate-loses-control entry - the one that drops the
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

namespace MobileGL::MG_Remote::Server {

    // ---------------------------------------------------------------------------------
    // THE APPLY-THREAD IDENTITY, AS ONE INTEGER (P5d round 3, package D)
    // ---------------------------------------------------------------------------------
    //
    // OnApplyThread() is the predicate under EVERY role guard on both hot paths -
    // RefuseLegacyBufferArmFromApplyThread in BufferObject's accessors, PersistentMapTracker::
    // OnServerRole, ClientSession::RefusePipeInputsTouchWhileApplierOwnsIt, RunsAsTheServerRole,
    // MGPipeRefuseAllocatorFromApplyThread, RefuseLegacyTextureArmFromApplyThread. simpleperf at
    // head 56a77348 (FCL + Minecraft, VD12, Adreno 830) measured it at 2.66% self / 3.64%
    // inclusive ON THE CLIENT THREAD, for an answer that is "no" every single time.
    //
    // The old body paid five times over for that "no": ServerLoopInstance()'s function-static
    // guard byte, an acquire load of m_running, an acquire load of an atomic<std::thread::id>,
    // std::this_thread::get_id(), and a thread::id comparison that is an out-of-line
    // pthread_equal through the PLT (@plt was 6.1% of the apply thread). None of it carries
    // information the guard needs. A live thread's identity is already sitting in a
    // register-cheap place - the thread pointer, TPIDR_EL0 on aarch64 - so the loop publishes
    // ITS identity as a plain integer here and every caller compares one relaxed load against
    // one register read, inline, with no call and no singleton.
    //
    // ZERO MEANS "NO APPLY THREAD IS RUNNING", which is exactly the answer m_running == false
    // used to give: the key is published as the apply thread's FIRST act (ApplyThreadMain) and
    // cleared in the same m_controlMutex critical section that clears m_running on the way out
    // (ApplyThreadMain's C2 exit block), so "key != 0" and "m_running" move together and the
    // window in which a live-but-exiting apply thread answers true is byte-for-byte the old
    // one. The ServerLoop object still OWNS the key - nothing but ApplyThreadMain writes it -
    // it simply no longer has to be reached through the singleton to be read.
    namespace Detail {
        // RELAXED ON BOTH SIDES, and that is a claim about recycling, not a shrug. The only
        // dangerous outcome would be a NEW thread that reuses the exited apply thread's TLS
        // block (hence its thread pointer) while still reading the pre-clear key. It cannot
        // happen: the clear is sequenced before the apply thread's exit, the exit releases the
        // TLS block under libc's own lock, and the thread that later receives that block takes
        // the same lock - so the clear happens-before any load the new thread performs, and
        // coherence forbids a relaxed load from reading a value that a happens-before store
        // overwrote. A stale ZERO is harmless in the other direction: a thread that is not the
        // apply thread wants "false" anyway, and the apply thread always reads its own store.
        //
        // No acquire is needed for a second reason: this is an IDENTITY, not a handshake. No
        // caller publishes data through it; the real ordering lives in the ring, the doorbell
        // and m_controlMutex.
        inline std::atomic<Uint64> g_applyThreadKey{0};

        // The calling thread's identity as the same integer. aarch64 - the split's only
        // measured target - reads TPIDR_EL0 with a single instruction and no call. Everywhere
        // else falls back to std::this_thread::get_id() memcpy'd into the key: still one libc
        // call, but the atomic<thread::id> load and the out-of-line pthread_equal that
        // dominated the old body are gone either way. x86_64 is deliberately NOT on the
        // builtin path - __builtin_thread_pointer only grew x86 support in clang 17 / GCC 11
        // and older toolchains accept __has_builtin for it and then fail in codegen, which is
        // a build break on the desktop ctest lane for a target whose frame rate nobody
        // measures.
        //
        // A LIVE THREAD'S KEY IS NEVER 0 on either path (a thread pointer is a real TLS
        // address; std::this_thread::get_id() on a running thread is never the
        // default-constructed id, whose object representation is the all-zero one on every
        // library in scope), which is what lets 0 mean "nobody".
        //
        // THE SELECTOR IS A BUILD KNOB, NOT A HEADER SECRET (review round 3). This header is
        // pulled into MG_State, MG_Impl and MG_Test translation units now, so the name carries
        // the MOBILEGL_DETAIL_ prefix to read as internal, it is #ifndef-guarded so a toolchain
        // whose __has_builtin(__builtin_thread_pointer) answers 1 and then fails in codegen can
        // be steered off the path with -DMOBILEGL_DETAIL_APPLY_THREAD_POINTER=0 instead of a
        // header edit, and it is always DEFINED - to 0 when unavailable - and tested with #if
        // rather than #ifdef, so that a -D...=0 really disables it instead of being ignored.
#ifndef MOBILEGL_DETAIL_APPLY_THREAD_POINTER
#if defined(__aarch64__) && defined(__has_builtin)
#if __has_builtin(__builtin_thread_pointer)
#define MOBILEGL_DETAIL_APPLY_THREAD_POINTER 1
#endif
#endif
#endif
#ifndef MOBILEGL_DETAIL_APPLY_THREAD_POINTER
#define MOBILEGL_DETAIL_APPLY_THREAD_POINTER 0
#endif
        inline Uint64 CurrentThreadKey() {
#if MOBILEGL_DETAIL_APPLY_THREAD_POINTER
            return static_cast<Uint64>(reinterpret_cast<std::uintptr_t>(__builtin_thread_pointer()));
#else
            const std::thread::id id = std::this_thread::get_id();
            static_assert(sizeof(std::thread::id) <= sizeof(Uint64),
                          "std::thread::id does not fit the apply-thread key; this fallback has "
                          "to hash rather than copy on that platform");
            // AND IT MUST HAVE NO PADDING (review round 3). Copying the OBJECT REPRESENTATION of
            // a type that has padding bytes would let two calls ON THE SAME THREAD return
            // different keys, and OnApplyThread() would then answer FALSE ON THE APPLY THREAD -
            // a role guard that has stopped guarding, which is the one direction CONTRACT-P5C
            // rule E says a guard may never fail in (RunSurfaceControlFrame would also lose its
            // re-entrancy shortcut and the EGL teardown post would deadlock on m_controlDone).
            // Every library this fallback is built against today wraps a single scalar
            // (libstdc++ __gthread_t, libc++ __libcpp_thread_id, MSVC unsigned int) and
            // satisfies this; one that did not is a BUILD ERROR here rather than a silently
            // dead guard at run time.
            static_assert(std::has_unique_object_representations_v<std::thread::id>,
                          "std::thread::id has padding on this platform: copying its object "
                          "representation would make the apply-thread key unstable within one "
                          "thread. Hash the id (std::hash<std::thread::id>) instead.");
            Uint64 key = 0;
            std::memcpy(&key, &id, sizeof(id));
            return key;
#endif
        }
    } // namespace Detail

    // ---------------------------------------------------------------------------------
    // P12 (on-screen server window), D4: ONE SURFACE MODE PER SESSION
    // ---------------------------------------------------------------------------------
    //
    // "Only one of the two rendering paths is active at a time, decided at context creation": a
    // session's mode latches at its FIRST successful surface creation - a ServerOwned window makes
    // it on-screen, a pbuffer makes it offscreen - and a later surface of the OTHER kind in the same
    // session is refused by name (SurfaceRefusal::SurfaceModeMismatch: reply ok=false, logged, the
    // session NOT latched). Windows the client names itself (the X11/Win32/None tokens CONTRACT-P6
    // D8 has yet to close) are neither kind and neither latch nor are refused: nothing about them
    // changes here. Reset by ServerLoop::Start, i.e. per session.
    enum class SessionSurfaceMode : Uint8 { None, OnScreen, Offscreen };
    const char* SessionSurfaceModeName(SessionSurfaceMode mode);
    // The decision, pure, so a unit case can drive all six pairs without a backend.
    Bool SessionSurfaceModeAdmits(SessionSurfaceMode current, Bool serverOwnedWindow);

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

        // Run one blocking control request on the apply thread and wait for it. This is how all
        // nine EGL lifecycle virtuals cross; it is deliberately NOT a queue of async messages,
        // because every one of them has a return value the caller acts on immediately.
        //
        // THE REQUEST IS A VALUE FRAME (P5f, package fc). It used to be a raw function pointer
        // plus a void* to a stack-local Args struct - neither has a meaning across a process
        // boundary, which is the whole of P5c audit row G4. Now the slot carries one
        // SurfaceControlFrame BY VALUE: the op's identity is the frame's `kind`, its arguments
        // are scalars, and its reply fields come back in the same value when the wait returns.
        // What did NOT move: the blocking handshake, the one slot, and the apply-thread
        // ownership. Under spawn the same frame is what SurfaceOpCodec encodes into a
        // Wire::SurfaceOp - this signature is the seam both transports share.
        // P6 `cp`: where a control frame goes when the server is ANOTHER PROCESS.
        //
        // All twelve Server* forwarders funnel through RunSurfaceControlFrame, so
        // this is ONE seam rather than twelve (a6 row A4-13 counted them). The
        // client installs a sink at handshake; with none installed the behaviour
        // is exactly what it was - post into the one-slot mailbox, or run inline
        // when we are already on the apply thread.
        //
        // A HOOK RATHER THAN A CALL INTO ClientSession, deliberately: the server
        // half calling the client half is four of the six symbols a6 found
        // MG_Remote owes itself (a6-link-experiment §5), and this is the one
        // place that would have added a fifth.
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
            const Uint64 key = Detail::g_applyThreadKey.load(std::memory_order_relaxed);
            // Short-circuit ON PURPOSE: with TransportMode::Monolith, and in every process
            // before Start() and after Stop(), the key is 0 and the caller pays one relaxed
            // load and one branch - it never even reads its own identity.
            return key != 0 && key == Detail::CurrentThreadKey();
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

        // Part of the apply thread's park predicate: a posted control request must be able to
        // un-park a thread waiting on kWaitForever, which a Notify alone cannot do.
        //
        // IT READS ONE ATOMIC AND TAKES NO LOCK (P5d round 3, package T item 1). It used to
        // take m_controlMutex on every call, and the idle poll calls it on every iteration -
        // once inside the `ready` lambda and once more through PumpControlRequest - which
        // simpleperf at head 56a77348 measured as ~49% of the whole apply thread's cycles
        // (mutex::lock 12.4 + mutex::unlock 13.6 + the two pthread_* halves, 58-60% of them
        // with ApplyThreadMain as the DIRECT caller). An idle poll on an empty ring must cost
        // loads, not a futex-backed critical section. m_controlPosted is that load; the
        // mailbox's own fields stay under the mutex and the handshake below is unchanged.
        //
        // PUBLIC because ServerLoopTest reads it: the clear side of the shadow has a failure
        // mode - a shadow left set makes `ready` permanently true and the loop never parks
        // again - that no other public surface can see.
        //
        // READ THE PREDICATE LITERALLY: "a request is POSTED AND NOT YET TAKEN", which is
        // narrower than the name's "a control request is in flight". The shadow is cleared
        // when the pump takes the work, not when the work returns, so for the whole duration
        // of `work(user)` this answers false although m_controlPending is still true and the
        // poster is still blocked in RunSurfaceControlFrame. That is deliberate - it is a PARK
        // PREDICATE, and the thread that would act on it is the one running the work - but it
        // means this is not a liveness query and must not be used as one. The only in-tree
        // reader outside the test is the apply thread's own `ready` lambda, which by
        // construction cannot be inside that window.
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
        MobileGLResult AcquireServerWindow(Uint32 width, Uint32 height, Uint32 timeoutMs,
                                           ServerWindowLease* out, SurfaceRefusalCode* refusal);
        // APPLY THREAD ONLY: this session's surface mode (D4).
        SessionSurfaceMode SurfaceMode() const { return m_surfaceMode; }
        // How many times this loop released a lost server window and latched ServerWindowLost (D6).
        Uint64 ServerWindowsLost() const { return m_serverWindowsLost.load(std::memory_order_acquire); }

    private:
        RemoteControlSink m_remoteSink = nullptr;
        void* m_remoteSinkUser = nullptr;

        // P12. The ServerOwned arm of the CreateWindowSurface dispatch (D3, D4).
        MobileGLResult ApplyServerOwnedWindowSurface(MG_Backend::BackendObject* backend, SurfaceControlFrame& frame);
        // P12 (D6), apply thread: the lost window's backend surface goes, the lease ends, and the
        // session latches ServerWindowLost. Runs from PumpControlRequest when Detach asked.
        void ReleaseLostServerWindow();
        // Ends this loop's display lease if it holds one (after the backend let go of the window).
        void EndServerWindowLease();
        // ServerDisplay's lost hook: called under the display's lock from Detach's thread. Sets the
        // request and rings the apply thread's bell; never blocks.
        static void ServerWindowLostThunk(void* self);
        // AcquireFor's cancel predicate: the loop is stopping, or the session latched.
        static Bool ServerWindowWaitCancelled(void* self);
        // D4: latched at the session's first successful surface creation; reset by Start().
        SessionSurfaceMode m_surfaceMode = SessionSurfaceMode::None;
        // Apply thread only: this loop holds the display's lease.
        Bool m_holdsWindowLease = false;
        // Set by ServerWindowLostThunk (any thread), taken by the apply thread. Part of the park
        // predicate, so a parked apply thread wakes for it.
        std::atomic<Bool> m_windowLostRequested{false};
        std::atomic<Uint64> m_serverWindowsLost{0};

        void ApplyThreadMain();
        // Runs a posted control frame, if there is one. Returns true if it ran one.
        Bool PumpControlRequest();
        // The dispatch, on the apply thread (or inline for a re-entrant post): executes the
        // frame's op against the private backend and fills the frame's reply half. The eleven
        // wire+inproc op bodies from the old forwarder Args structs live here now.
        MobileGLResult ApplySurfaceControlFrame(SurfaceControlFrame& frame,
                                                 ControlProbeHook probeHook = nullptr, void* probeUser = nullptr);
        // The caller already holds m_callerMutex; shared by normal and test-probe posting so
        // the probe's metadata is protected for the same lifetime as its frame.
        MobileGLResult PostSurfaceControlFrameWithCallerLock(SurfaceControlFrame& frame);
        // Pops and applies every record currently in the ring; returns how many it applied.
        Uint64 DrainRing();
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

        Uint64 m_affinityMask = 0;
        std::atomic<Uint64> m_drained{0};
        std::atomic<Uint64> m_parks{0};
        // Incremented by Doorbell::Wait through its `parkTally` out-parameter, once per wait
        // that reached the blocking Park. Relaxed everywhere: it is a gauge.
        std::atomic<Uint64> m_parkBlocks{0};
        std::atomic<void (*)()> m_beforeRetireHook{nullptr};
        std::atomic<void (*)()> m_betweenRecordsHook{nullptr};

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
    //
    // P5f (package fc): each of the twelve packs its arguments into a SurfaceControlFrame and
    // posts it through RunSurfaceControlFrame - the wire-shaped value channel that replaced the
    // function-pointer mailbox (P5c audit row G4). Nine of them map onto the ten wire ops of
    // protocol.fbs's SurfaceOp; ServerSwapEGLBuffers, ServerInitCapabilities and
    // ServerInitWindowSurface ride the same channel as inproc-only kinds (present travels as a
    // record, capabilities are answered by the CapsSnapshot frame, and the third has no caller -
    // f0-egl's census, findings F8/§4.2).
    Bool ServerInitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor);
    Bool ServerCreateEGLWindowSurface(EGLSurface surface, const MG_Backend::WindowHandle& handle);
    Bool ServerResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height);
    // P12: `refusal`, when given, receives the server's named refusal (SurfaceModeMismatch for a
    // pbuffer in an on-screen session), None otherwise.
    Bool ServerCreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height,
                                       SurfaceRefusalCode* refusal = nullptr);
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
