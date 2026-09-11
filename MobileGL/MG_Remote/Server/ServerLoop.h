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
// kWaitForever) and shuts down when Wait returns false with Dead() set. Doorbell::Kill()
// (Doorbell.h:211-221) IS THE ONLY THING that wakes a thread parked on kWaitForever - a fact
// ARCHITECTURE.md's teardown order (:537) omits and InProcessTransportTest.cpp:344 already
// pins. Kill BEFORE join; join before the client frees any emitter-owned Vector; and the join
// must be bounded (that test uses 5 s) so a regression is a red test and not a hung CI job.
//
// THE EGL OWNERSHIP MOVE. eglMakeCurrent runs ONCE on this thread and is never released
// (DirectGLES.cpp:11925 plus the six cache invalidations at :11933-11953, which become a
// one-time startup cost instead of a per-make-current storm). The client's nine EGL virtuals
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

    private:
        ServerSession* m_session = nullptr;
        Bool m_running = false;
    };

    ServerLoop& ServerLoopInstance();

} // namespace MobileGL::MG_Remote::Server
