// MobileGL - MobileGL/MG_Remote/Client/BackendObject_Remote.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 package c1. See BackendObject_Remote.h for the three traps and why each is paid here.

#include "BackendObject_Remote.h"

#include "CapsMirror.h"
#include "ClientSession.h"
#include "EmitTables.h"

#include "../Server/ServerLoop.h"

#include <MG_Util/Debug/Log.h>

// Declared rather than included: MG_Backend/BackendObjects.h drags in both concrete backend
// objects, and this translation unit must not depend on either - the client role links the same
// library but never constructs one.
namespace MobileGL::MG_Backend {
    extern UniquePtr<BackendObject>& pActiveBackendObject;
}

namespace MobileGL::MG_Remote::Client {

    namespace {

        // ---- the EGL seam ------------------------------------------------------------------
        //
        // THE NINE EGL VIRTUALS CALL v1's TWELVE FORWARDERS AND NOTHING ELSE. c1 round 1 built
        // its own trampolines over ServerLoop::RunOnApplyThread and ServerLoop::Backend(),
        // which ran the right driver call on the right thread and was still wrong, because
        // three of the twelve do MORE than forward:
        //
        //   ServerMakeEGLCurrent   re-publishes the caps snapshot after the server's own
        //                          InitCapabilities has run (R-12 arm (a))
        //   ServerInitCapabilities the same, on the explicit path
        //   ServerSetWindowHandle  hands the surface to the server's backend
        //
        // Calling `Backend()->MakeEGLCurrent(...)` skips the republish, so the client's mirror
        // keeps the snapshot Accept() sent BEFORE any context existed - every limit, every
        // advertised extension and the compile-env fingerprint read off an empty backend, with
        // InitCapabilities below happily reporting success because a snapshot did arrive once.
        // That is the failure this seam exists to make impossible: there is no second route to
        // the server's EGL, so there is no route that can skip what the forwarder does.
        //
        // The forwarders block on mgl-srv-apply themselves and run INLINE when the caller is
        // already on that thread, so this file no longer needs RunOnApplyThread, a per-call
        // args struct, or a null-backend check of its own - ServerBackendOrNull() inside each
        // forwarder is the one place that answers "the bring-up did not complete", and it
        // answers `false` rather than crashing or succeeding locally.

        // CapsMirror's adoption hook. A free function because the hook is a raw function
        // pointer (ID-8: this can fire on a path that must not allocate), and it reaches the
        // live object through pActiveBackendObject rather than through a second global.
        void OnCapsAdopted() {
            auto* self = dynamic_cast<BackendObject_Remote*>(MG_Backend::pActiveBackendObject.get());
            if (self != nullptr) self->RefreshFormatCapabilities();
        }

    } // namespace

    BackendObject_Remote::BackendObject_Remote() {
        // Installed in the constructor rather than at the first snapshot, because the first
        // snapshot has usually already arrived by then: ClientSession::Start pumps the control
        // plane during the handshake, and MG_Backend::Init() constructs this object after it.
        // RefreshFormatCapabilities below picks up that already-adopted generation.
        SetCapsAdoptedHook(&OnCapsAdopted);
        RefreshFormatCapabilities();
    }

    BackendObject_Remote::~BackendObject_Remote() {
        // The hook holds a raw function pointer, not a pointer to this - but the function it
        // names reaches pActiveBackendObject, which is being destroyed right now. Uninstall.
        SetCapsAdoptedHook(nullptr);
    }

    void BackendObject_Remote::RefreshFormatCapabilities() {
        CapsMirror& mirror = CapsMirrorInstance();
        if (!mirror.Valid() || mirror.Generation() == m_formatsGeneration) return;
        // TRAP 2. GetFormatCapabilities() is non-virtual and hands back this member, so the
        // only way a remote object can answer it is to fill it.
        MutableFormatCapabilities() = mirror.Formats();
        m_formatsGeneration = mirror.Generation();
        MGLOG_I("MG_Remote client: format capabilities filled from caps mirror generation %llu",
                static_cast<unsigned long long>(m_formatsGeneration));
    }

    // ---- the eight pure virtuals ----------------------------------------------------------

    void BackendObject_Remote::Initialize() {
        // NOT FORWARDED. The server's own BackendObject_DirectGLES is created and initialised
        // by v1's ServerLoop, on the apply thread, before this object exists; forwarding here
        // would be a second Initialize() on an already-initialised backend. What this call
        // does is drain whatever the handshake left and take the caps that came with it.
        if (ClientSession* session = ClientSession::Active()) {
            session->PumpControlPlane();
        }
        RefreshFormatCapabilities();
    }

    Bool BackendObject_Remote::InitCapabilities() {
        // Reached from the base class's MakeEGLCurrent, lazily, once per surface lifetime
        // (BackendObject.cpp:341-347). By this point MakeEGLCurrent below has already run the
        // SERVER's MakeEGLCurrent on the apply thread, whose own base class ran the server
        // backend's InitCapabilities and whose ServerSession re-published the snapshot - so
        // the client's job here is to pick that snapshot up. R-12: re-arrival IS the
        // invalidation, and this is the second place it is drained (the other is Present).
        ClientSession* session = ClientSession::Active();
        if (session == nullptr) {
            MGLOG_E("MG_Remote client: InitCapabilities with no session");
            return false;
        }
        // ASK THE SERVER RATHER THAN ASSUMING ServerMakeEGLCurrent HAS ALREADY ASKED IT. The
        // comment above says "by this point MakeEGLCurrent has already run the SERVER's
        // MakeEGLCurrent", and that is true on the make-current path - but the base class
        // reaches InitCapabilities lazily, once per SURFACE lifetime, and a surface can be
        // replaced without a new make-current. ServerInitCapabilities re-publishes the
        // snapshot the same way, so asking twice costs one snapshot and never asking costs
        // every limit in the mirror. It is idempotent on the server's side.
        if (!Server::ServerInitCapabilities()) {
            MGLOG_E("MG_Remote client: the server's InitCapabilities failed, so there is no "
                    "snapshot to adopt and going current would read default limits");
            return false;
        }
        session->PumpControlPlane();
        RefreshFormatCapabilities();
        // A PLACEHOLDER MIRROR IS A FAILURE HERE, unlike at startup. LogBackendInfo reading a
        // placeholder costs one wrong log line; a context going current on one costs every
        // limit, every advertised extension and the compile-env fingerprint.
        if (!CapsMirrorInstance().Valid()) {
            MGLOG_E("MG_Remote client: InitCapabilities found no CapsSnapshot - the server has "
                    "not published one. Going current on a placeholder caps mirror would put "
                    "default limits into every glGetIntegerv answer and into the compile env");
            return false;
        }
        return true;
    }

    Bool BackendObject_Remote::InitWindowSurface() {
        // The real surface work happened on the apply thread inside the server backend's own
        // ActivateEGLSurface; this is the client's half of the base state machine and has
        // nothing of its own to do.
        return true;
    }

    Bool BackendObject_Remote::InitPbufferSurface(EGLint, EGLint) { return true; }

    const RendererInfo& BackendObject_Remote::GetRendererInfo() const {
        // TRAP 1: a reference, so the storage is the mirror's and not a temporary's.
        return CapsMirrorInstance().Renderer();
    }

    String BackendObject_Remote::GetBackendAPIVersionString() const {
        return CapsMirrorInstance().ApiVersion();
    }

    const MG_Backend::GlobalBackendFunctionsTable& BackendObject_Remote::GetBackendFunctions() const {
        return RemoteEmitTable();
    }

    const MG_Backend::DynamicBackendParameters& BackendObject_Remote::GetDynamicParameters() const {
        return CapsMirrorInstance().Dynamic();
    }

    BackendType BackendObject_Remote::GetBackendType() const {
        // TRAP 3: the SERVER's backend, never a new enumerator.
        return CapsMirrorInstance().Backend();
    }

    // ---- the nine EGL lifecycle virtuals ---------------------------------------------------
    //
    // FORWARD FIRST, THEN RUN THE BASE. The server has to own the context before the client's
    // base class latches "the surface is initialised" and calls InitCapabilities, because
    // InitCapabilities' answer comes from a snapshot the server can only publish once its own
    // InitCapabilities has run.

    Bool BackendObject_Remote::InitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor) {
        if (!Server::ServerInitializeEGLDisplay(dpy, major, minor)) return false;
        return MG_Backend::BackendObject::InitializeEGLDisplay(dpy, major, minor);
    }

    Bool BackendObject_Remote::CreateEGLWindowSurface(EGLSurface surface,
                                                      const MG_Backend::WindowHandle& handle) {
        // The handle first: the server's backend has to know which window it is about to make
        // a surface for, and ServerSetWindowHandle is the only way to tell it.
        Server::ServerSetWindowHandle(handle);
        if (!Server::ServerCreateEGLWindowSurface(surface, handle)) return false;
        return MG_Backend::BackendObject::CreateEGLWindowSurface(surface, handle);
    }

    Bool BackendObject_Remote::ResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height) {
        if (!Server::ServerResizeEGLWindowSurface(surface, width, height)) return false;
        return MG_Backend::BackendObject::ResizeEGLWindowSurface(surface, width, height);
    }

    Bool BackendObject_Remote::CreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height) {
        if (!Server::ServerCreateEGLPbufferSurface(surface, width, height)) return false;
        return MG_Backend::BackendObject::CreateEGLPbufferSurface(surface, width, height);
    }

    Bool BackendObject_Remote::MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                                              EGLContext ctx) {
        // FORWARD FIRST, THEN RUN THE BASE. The server has to own the context before the base
        // class latches "the surface is initialised" and calls InitCapabilities, because
        // InitCapabilities' answer comes from a snapshot the server can only publish once its
        // own InitCapabilities has run - and ServerMakeEGLCurrent is what publishes it.
        if (!Server::ServerMakeEGLCurrent(dpy, draw, read, ctx)) return false;
        if (!MG_Backend::BackendObject::MakeEGLCurrent(dpy, draw, read, ctx)) return false;

        // R-12 ARM (a) ON EVERY SUCCESSFUL MAKE-CURRENT (codex 12). ServerMakeEGLCurrent above
        // republishes the caps snapshot on every call (ServerLoop.cpp:613-628), but the base
        // class only runs InitCapabilities - the one place that pumps and refreshes - on the
        // FIRST make-current per surface (BackendObject.cpp:341-347). A repeated make-current
        // onto an already-initialised surface therefore left the client mirror one generation
        // behind while unpumped snapshots accumulated, so a cap getter or a shader compile before
        // the next Present read the prior mirror. Adopting here closes that: "a second snapshot
        // arrival IS the invalidation" (R-12) now holds AT the make-current that caused it. It is
        // idempotent - on the first make-current InitCapabilities already pumped, so this adopts
        // 0 - and a release-current (draw/ctx cleared) publishes nothing and is skipped.
        if (draw != EGL_NO_SURFACE && ctx != EGL_NO_CONTEXT) {
            if (ClientSession* session = ClientSession::Active()) {
                session->PumpControlPlane();
                RefreshFormatCapabilities();
            }
        }
        return true;
    }

    Bool BackendObject_Remote::SwapEGLBuffers(EGLDisplay dpy, EGLSurface draw) {
        // NOT FORWARDED, and this is the one that must not be. The base implementation's last
        // act is GetBackendFunctions().Present() (BackendObject.cpp:396) - which is this
        // client's class-B Present EMITTER, the only route by which Present is reached at all
        // (it has zero MG_Impl call sites). Calling Server::ServerSwapEGLBuffers would present
        // on the server directly and put no record on the wire, which is the shape every gate
        // in this phase exists to catch. The forwarder exists for a spawned P6 client whose
        // Present record cannot carry the swap; in P5 it has no caller and that is deliberate.
        return MG_Backend::BackendObject::SwapEGLBuffers(dpy, draw);
    }

    void BackendObject_Remote::SetEGLSwapInterval(Int interval) {
        // OVERRIDDEN BECAUSE THE BASE WOULD FATAL. BackendObject.cpp:402 null-checks
        // GetBackendFunctions().SetSwapInterval and calls it when non-null - one of the 41
        // null checks R-4 turns into "always supported" - and SetSwapInterval is class C, so
        // the base implementation would abort on every eglSwapInterval. The answer is the
        // caps-mirror-read rule's general shape: the question "can the presentation path take
        // an interval" belongs to the server, so it is asked of the server.
        Server::ServerSetEGLSwapInterval(interval);
    }

    void BackendObject_Remote::ReleaseEGLSurface(EGLSurface surface) {
        Server::ServerReleaseEGLSurface(surface);
        MG_Backend::BackendObject::ReleaseEGLSurface(surface);
    }

    void BackendObject_Remote::ReleaseEGLResources() {
        // BLOCKING BY CONTRACT (ServerLoop.h's header note): MobileGL::Destroy()
        // (MobileGL/Init.cpp:68) walks on the moment this returns, and the server still holds
        // the context until the apply thread has run it.
        Server::ServerReleaseEGLResources();
        MG_Backend::BackendObject::ReleaseEGLResources();
    }

    // NO strong CreateRemoteBackendObject() lives here, and the reason is a link fact, not an
    // oversight. v1's Init.cpp calls MG_Remote::Client::CreateRemoteBackendObject() and ships a
    // __attribute__((weak)) placeholder for it beside ServerLoop that aborts by name; its comment
    // expects "c1's strong definition [to] displace it at link time". A strong definition here
    // does NOT: libMobileGL is linked from a static archive, ServerLoop.o (weak) is already in
    // the link and satisfies Init's reference, and nothing else references this TU's
    // CreateRemoteBackendObject - so BackendObject_Remote.o is never pulled to override it, and
    // the weak's abort fires (measured: readelf shows one local symbol, the log shows
    // Fatal{UnimplementedRemoteBackendObject}). The integrator's Init.cpp hunk works because it
    // constructs BackendObject_Remote DIRECTLY (MakeUnique<BackendObject_Remote>), which both
    // references this object - forcing its TU into the link - and bypasses the weak symbol. So
    // the merge-time construction stays v1's Init.cpp edit (or a --whole-archive / forced
    // reference the integrator adds); see c1-v3.md.

} // namespace MobileGL::MG_Remote::Client
