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

#include <MG_State/EGLState/Core.h>
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
        // its own trampolines over ServerLoop's control channel and ServerLoop::Backend(),
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
        // The forwarders pack a SurfaceControlFrame and block on mgl-srv-apply themselves (P5f
        // fc; the channel was a function-pointer mailbox before that) and run INLINE when the
        // caller is already on that thread, so this file needs no frame, no per-call args
        // struct, and no null-backend check of its own - the dispatch's null-backend arm is the
        // one place that answers "the bring-up did not complete", and it answers `false` rather
        // than crashing or succeeding locally.

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

    namespace {
        // P5e (ra), CONTRACT-P5E §2.5: THE WAIT BEFORE EVERY `Server*` EGL FORWARDER.
        //
        // These calls do not travel on SEG_CMD. They go through ServerLoop's control mailbox,
        // which is pumped BETWEEN drain batches (ServerLoop.cpp's PumpControlRequest, above
        // DrainRing) - so a make-current, a surface creation or a resize can land between two
        // records the client published and never waited for. Under lockstep that was
        // impossible: the client had waited out every record it issued before it could reach
        // this line. Under run-ahead it is one queued frame wide, and a context switch applied
        // in the middle of another context's draws is not a wrong pixel, it is a wrong
        // everything.
        //
        // ServerSwapEGLBuffers is deliberately NOT on this list, and its own virtual says why:
        // present IS the swap and it travels as a record, in order, with its own credit.
        void WaitForApplyBeforeEglForwarder(const char* forwarder) {
            if (ClientSession* session = ClientSession::Active()) {
                session->WaitForApplyToCatchUp(forwarder);
            }
        }
    } // namespace

    // ---- the nine EGL lifecycle virtuals ---------------------------------------------------
    //
    // FORWARD FIRST, THEN RUN THE BASE. The server has to own the context before the client's
    // base class latches "the surface is initialised" and calls InitCapabilities, because
    // InitCapabilities' answer comes from a snapshot the server can only publish once its own
    // InitCapabilities has run.

    Bool BackendObject_Remote::InitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor) {
        WaitForApplyBeforeEglForwarder("InitializeEGLDisplay");
        if (!Server::ServerInitializeEGLDisplay(dpy, major, minor)) return false;
        return MG_Backend::BackendObject::InitializeEGLDisplay(dpy, major, minor);
    }

    Bool BackendObject_Remote::CreateEGLWindowSurface(EGLSurface surface,
                                                      const MG_Backend::WindowHandle& handle) {
        // The handle first: the server's backend has to know which window it is about to make
        // a surface for, and ServerSetWindowHandle is the only way to tell it.
        WaitForApplyBeforeEglForwarder("CreateEGLWindowSurface");
        // P12 (D1): unless the window is the SERVER's - then there is no handle of ours to tell.
        if (MG_Config::ServerOwnedWindowSurfaces()) return CreateServerOwnedWindowSurface(surface, handle);
        Server::ServerSetWindowHandle(handle);
        if (!Server::ServerCreateEGLWindowSurface(surface, handle)) return false;
        // The server's surface init published the default framebuffer's shape as a
        // surface-changed EVENT (P5c ev): DirectGLES' depth/stencil format, Magma's
        // swapchain extent. Apply it NOW - the RPC's return is a moment the apply thread
        // is known idle - because the first verb's drain would otherwise let every pre-verb
        // query answer from the placeholder attachments (GL_DEPTH32F_STENCIL8 for a
        // depth24+stencil8 surface, and every buffer allocated from that answer is
        // blit-incompatible with the real thing).
        if (ClientSession* session = ClientSession::Active()) session->DrainPublishedEvents();
        return MG_Backend::BackendObject::CreateEGLWindowSurface(surface, handle);
    }

    Bool BackendObject_Remote::ResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height) {
        WaitForApplyBeforeEglForwarder("ResizeEGLWindowSurface");
        // P12 review fix: a surface on the SERVER's window is resized by resizing that window. The
        // server asks its display for the size and answers with the extent the window really took,
        // and that - not the size asked for, which EGLImpl already wrote into the EGL state - is what
        // eglQuerySurface answers from here on.
        if (ClientSession::IsServerOwnedWindowSurface(surface)) {
            const Server::ServerOwnedWindowReply reply =
                Server::ServerResizeServerOwnedWindowSurface(surface, width, height);
            if (!reply.ok) {
                MGLOG_E("MG_Remote client: the server could not resize the server-owned window surface to %ux%u "
                        "(channel rc=%d, refusal %s)",
                        width, height, static_cast<int>(reply.transport), Server::SurfaceRefusalCodeName(reply.refusal));
                return false;
            }
            const Uint32 realWidth = reply.width != 0 ? reply.width : width;
            const Uint32 realHeight = reply.height != 0 ? reply.height : height;
            if (MG_State::pEGLContext) {
                (void)MG_State::pEGLContext->SetSurfaceExtent(surface, static_cast<EGLint>(realWidth),
                                                              static_cast<EGLint>(realHeight));
            }
            if (ClientSession* session = ClientSession::Active()) {
                session->NoteServerOwnedWindowSurface(surface, realWidth, realHeight);
                session->DrainPublishedEvents();
            }
            if (realWidth != width || realHeight != height) {
                MGLOG_W("MG_Remote client: the server window took %ux%u, not the %ux%u asked for; the surface "
                        "reports the window's size",
                        realWidth, realHeight, width, height);
            }
            return MG_Backend::BackendObject::ResizeEGLWindowSurface(surface, realWidth, realHeight);
        }
        if (!Server::ServerResizeEGLWindowSurface(surface, width, height)) return false;
        // A resize re-creates the server's swapchain, which re-posts the surface-changed
        // event - same drain, same reason as CreateEGLWindowSurface.
        if (ClientSession* session = ClientSession::Active()) session->DrainPublishedEvents();
        return MG_Backend::BackendObject::ResizeEGLWindowSurface(surface, width, height);
    }

    Bool BackendObject_Remote::CreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height) {
        WaitForApplyBeforeEglForwarder("CreateEGLPbufferSurface");
        Server::SurfaceRefusalCode refusal = Server::SurfaceRefusalCode::None;
        if (!Server::ServerCreateEGLPbufferSurface(surface, width, height, &refusal)) {
            if (refusal == Server::SurfaceRefusalCode::SurfaceModeMismatch) {
                // P12 (D4), named on THIS side too: the server logged its half.
                MGLOG_E("MG_Remote client: SurfaceModeMismatch - eglCreatePbufferSurface (%dx%d) in a session "
                        "whose surface mode is on-screen: its first surface was the server's own window "
                        "(MOBILEGL_IPC_SURFACE=server), and only one rendering path is active per session. "
                        "The server refused it; the session carries on",
                        width, height);
            }
            return false;
        }
        // Same drain as the window surface: InitPbufferSurface publishes the default
        // framebuffer's depth/stencil format on SEG_EVENT from inside this very RPC.
        if (ClientSession* session = ClientSession::Active()) session->DrainPublishedEvents();
        return MG_Backend::BackendObject::CreateEGLPbufferSurface(surface, width, height);
    }

    namespace {
        // THE CLIENT'S OWN RECORD OF A SERVER-OWNED WINDOW NEEDS A NON-NULL HANDLE, and a headless
        // client has none: BackendObject::RegisterEGLWindowSurface refuses a null handle, and that
        // base class is in the pull build (G1). This address stands in for it. It is never
        // dereferenced - the client's InitWindowSurface is a no-op - and never crosses the wire
        // (the ServerOwned frame carries token 0).
        char g_serverOwnedWindowPlaceholder = 0;
    } // namespace

    Bool BackendObject_Remote::CreateServerOwnedWindowSurface(EGLSurface surface,
                                                              const MG_Backend::WindowHandle& handle) {
        const Server::ServerOwnedWindowReply reply =
            Server::ServerCreateServerOwnedWindowSurface(surface, handle.Width, handle.Height);
        if (!reply.ok) {
            // P12: FAILS BY NAME ON THIS SIDE TOO. The server logged its reason; the reply's refusal
            // code is what lets this line say the same thing instead of "ok=false".
            switch (reply.refusal) {
            case Server::SurfaceRefusalCode::NoServerDisplay:
                MGLOG_E("MG_Remote client: Refuse ServerOwned (NoServerDisplay) - MOBILEGL_IPC_SURFACE=server asked "
                        "the server to create this %ux%u window surface on its own window, and the server owns no "
                        "display: it is an offscreen server. eglCreateWindowSurface fails with "
                        "EGL_BAD_NATIVE_WINDOW; connect to the on-screen display server, or unset "
                        "MOBILEGL_IPC_SURFACE",
                        handle.Width, handle.Height);
                break;
            case Server::SurfaceRefusalCode::NoServerWindow:
                MGLOG_E("MG_Remote client: Refuse ServerOwned (NoServerWindow) - the server owns a display but no "
                        "window came up for this %ux%u surface within its wait (is the display's surface visible?). "
                        "eglCreateWindowSurface fails with EGL_BAD_NATIVE_WINDOW",
                        handle.Width, handle.Height);
                break;
            case Server::SurfaceRefusalCode::SurfaceModeMismatch:
                MGLOG_E("MG_Remote client: SurfaceModeMismatch - eglCreateWindowSurface on the server's window "
                        "(MOBILEGL_IPC_SURFACE=server) in a session whose surface mode is offscreen: its first "
                        "surface was a pbuffer, and only one rendering path is active per session. The server "
                        "refused it; the session carries on");
                break;
            default:
                MGLOG_E("MG_Remote client: the server could not create the server-owned window surface "
                        "(MOBILEGL_IPC_SURFACE=server, %ux%u requested; channel rc=%d, refusal %s)",
                        handle.Width, handle.Height, static_cast<int>(reply.transport),
                        Server::SurfaceRefusalCodeName(reply.refusal));
                break;
            }
            return false;
        }
        // D1: THE GEOMETRY FLOWS BACK BEFORE THIS RETURNS. The reply carries the server window's real
        // extent, and it goes into the EGL state here, so eglQuerySurface(EGL_WIDTH/EGL_HEIGHT)
        // answers the server's size the moment eglCreateWindowSurface returns - whatever the backend
        // (Magma builds its swapchain, and publishes its extent, only at the first MakeCurrent). The
        // session records the surface as the server-owned one, so every later surface-changed event
        // that carries an extent (the window resized or rotated) resizes it too; then the same drain
        // as the ordinary window path, for the default framebuffer's shape the server's surface init
        // may have published from inside this very RPC.
        if (MG_State::pEGLContext && reply.width != 0 && reply.height != 0) {
            (void)MG_State::pEGLContext->SetSurfaceExtent(surface, static_cast<EGLint>(reply.width),
                                                          static_cast<EGLint>(reply.height));
        }
        if (ClientSession* session = ClientSession::Active()) {
            session->NoteServerOwnedWindowSurface(surface, reply.width, reply.height);
            session->DrainPublishedEvents();
        }
        MGLOG_I("MG_Remote client: surface=window %ux%u owner=server (MOBILEGL_IPC_SURFACE=server, %ux%u requested)",
                reply.width, reply.height, handle.Width, handle.Height);
        MG_Backend::WindowHandle local = handle;
        if (local.Backend == MG_Backend::WindowBackend::Unknown) local.Backend = MG_Backend::WindowBackend::Android;
        if (local.Handle == nullptr) local.Handle = &g_serverOwnedWindowPlaceholder;
        local.Width = reply.width;
        local.Height = reply.height;
        return MG_Backend::BackendObject::CreateEGLWindowSurface(surface, local);
    }

    Bool BackendObject_Remote::MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                                              EGLContext ctx) {
        // FORWARD FIRST, THEN RUN THE BASE. The server has to own the context before the base
        // class latches "the surface is initialised" and calls InitCapabilities, because
        // InitCapabilities' answer comes from a snapshot the server can only publish once its
        // own InitCapabilities has run - and ServerMakeEGLCurrent is what publishes it.
        WaitForApplyBeforeEglForwarder("MakeEGLCurrent");
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
                // The event ring beside the caps channel: a make-current can follow a
                // surface (re)creation that posted a surface-changed event, and this is
                // the same known-idle instant the RPC returns at.
                session->DrainPublishedEvents();
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
        // P12: a released server-owned surface stops taking the server window's geometry.
        if (ClientSession* session = ClientSession::Active()) session->ForgetServerOwnedWindowSurface(surface);
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
