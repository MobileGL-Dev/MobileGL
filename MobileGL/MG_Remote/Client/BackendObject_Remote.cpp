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

#include <MG_Impl/Pipe/SlotAllocator.h>
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
        // THE EGL VIRTUALS CALL v1's TWELVE FORWARDERS AND NOTHING ELSE (the swap interval's
        // since P10 through the emit table's slot, not an override here). c1 round 1 built
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
        if (!MG_Backend::BackendObject::CreateEGLWindowSurface(surface, handle)) return false;
        NoteHomed(surface);
        return true;
    }

    Bool BackendObject_Remote::ResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height) {
        WaitForApplyBeforeEglForwarder("ResizeEGLWindowSurface");
        // A surface with no server side on this session (its session was lost) takes the size
        // into its registration only; it is re-created at that size when it is next used.
        {
            const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
            if (GetRegisteredEGLSurface(surface) != nullptr && m_homedSurfaces.count(surface) == 0)
                return MG_Backend::BackendObject::ResizeEGLWindowSurface(surface, width, height);
        }
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

    void BackendObject_Remote::RefreshSurfaceExtent(EGLSurface surface) {
        if (!ClientSession::IsServerOwnedWindowSurface(surface)) return;
        ClientSession* session = ClientSession::Active();
        if (session == nullptr) return;
        // The known-idle instant every EGL forwarder drains at: the server has applied what this client
        // published, so the events it posted for it - a resized window's extent - are all there.
        WaitForApplyBeforeEglForwarder("QuerySurface");
        session->DrainPublishedEvents();
    }

    Bool BackendObject_Remote::CreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height) {
        return CreatePbufferOnServer(surface, width, height, /*standIn=*/false);
    }

    Bool BackendObject_Remote::CreateEGLSurfacelessStandIn(EGLSurface surface, EGLint width, EGLint height) {
        if (!CreatePbufferOnServer(surface, width, height, /*standIn=*/true)) return false;
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        m_standInSurfaces.insert(surface);
        return true;
    }

    Bool BackendObject_Remote::CreatePbufferOnServer(EGLSurface surface, EGLint width, EGLint height, Bool standIn) {
        WaitForApplyBeforeEglForwarder("CreateEGLPbufferSurface");
        Server::SurfaceRefusalCode refusal = Server::SurfaceRefusalCode::None;
        if (!Server::ServerCreateEGLPbufferSurface(surface, width, height, &refusal,
                                                   standIn ? Server::kPbufferFlagSurfacelessStandIn : 0u)) {
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
        if (!MG_Backend::BackendObject::CreateEGLPbufferSurface(surface, width, height)) return false;
        NoteHomed(surface);
        return true;
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
        if (!MG_Backend::BackendObject::CreateEGLWindowSurface(surface, local)) return false;
        NoteHomed(surface);
        return true;
    }

    Bool BackendObject_Remote::MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                                              EGLContext ctx) {
        // FORWARD FIRST, THEN RUN THE BASE. The server has to own the context before the base
        // class latches "the surface is initialised" and calls InitCapabilities, because
        // InitCapabilities' answer comes from a snapshot the server can only publish once its
        // own InitCapabilities has run - and ServerMakeEGLCurrent is what publishes it.
        WaitForApplyBeforeEglForwarder("MakeEGLCurrent");
        const Bool release = draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE && ctx == EGL_NO_CONTEXT;
        // A RELEASE ALWAYS SUCCEEDS ON A LOST SESSION. Its server has nothing left to unbind, and
        // an application releasing a lost context before it destroys it - the order EGL asks for -
        // must not be told EGL_BAD_ACCESS and left with the lost context still current.
        if (release && ClientSession::SessionLatchedLost()) {
            return MG_Backend::BackendObject::MakeEGLCurrent(dpy, draw, read, ctx);
        }
        // A surface a lost session had gets its server side back before anything is bound to it.
        if (!release && (!EnsureSurfaceHomed(draw) || (read != draw && !EnsureSurfaceHomed(read)))) return false;
        // The frame carries the context's CLIENT TOKEN rather than its handle (the server's
        // MakeCurrent arm binds the session to it before the native bind). Identity is all the
        // server reads from the field otherwise, and a token is as unique as a handle.
        const Uint64 contextToken =
            (ctx != EGL_NO_CONTEXT && MG_State::pEGLContext) ? MG_State::pEGLContext->GetContextClientToken(ctx) : 0;
        const EGLContext wireContext =
            contextToken != 0 ? reinterpret_cast<EGLContext>(static_cast<std::uintptr_t>(contextToken)) : ctx;
        if (!Server::ServerMakeEGLCurrent(dpy, draw, read, wireContext)) return false;
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
        if (!EnsureSurfaceHomed(draw)) return false;
        return MG_Backend::BackendObject::SwapEGLBuffers(dpy, draw);
    }

    // The damage rides in the present record the base's Present emitter writes (EmitPresent takes it
    // from the session); the swap is otherwise the plain one above.
    Bool BackendObject_Remote::SwapEGLBuffersWithDamage(EGLDisplay dpy, EGLSurface draw,
                                                        const MG_Util::Damage::Region& damage) {
        ClientSession* session = ClientSession::Active();
        if (session != nullptr) session->SetPendingPresentDamage(damage);
        const Bool swapped = SwapEGLBuffers(dpy, draw);
        // A swap refused before it reached the emitter must not leave its damage for the next one.
        if (session != nullptr) session->SetPendingPresentDamage(MG_Util::Damage::Region::Full());
        return swapped;
    }

    // No SetEGLSwapInterval here any more (P10 B): the base class calls the emit table's slot,
    // which is now Server::ServerSetEGLSwapInterval itself (EmitTables.cpp).

    void BackendObject_Remote::ReleaseEGLSurface(EGLSurface surface) {
        // P12: a released server-owned surface stops taking the server window's geometry.
        if (ClientSession* session = ClientSession::Active()) session->ForgetServerOwnedWindowSurface(surface);
        // Only a surface the current session's server has: one a lost session had is gone with it.
        Bool homed = false;
        {
            const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
            homed = m_homedSurfaces.erase(surface) != 0;
            m_standInSurfaces.erase(surface);
        }
        if (homed && !ClientSession::SessionLatchedLost()) Server::ServerReleaseEGLSurface(surface);
        MG_Backend::BackendObject::ReleaseEGLSurface(surface);
    }

    void BackendObject_Remote::ReleaseEGLResources() {
        // BLOCKING BY CONTRACT (ServerLoop.h's header note): MobileGL::Destroy()
        // (MobileGL/Init.cpp:68) walks on the moment this returns, and the server still holds
        // the context until the apply thread has run it. A lost session's server holds nothing.
        if (!ClientSession::SessionLatchedLost()) Server::ServerReleaseEGLResources();
        {
            const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
            m_homedSurfaces.clear();
            m_standInSurfaces.clear();
        }
        MG_Backend::BackendObject::ReleaseEGLResources();
    }

    void BackendObject_Remote::OnSessionReplaced() {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        ResetEGLRuntimeState();
        m_homedSurfaces.clear();
        for (const auto& [id, references] : m_sessionSharedImages) m_lostSessionSharedImages.insert(id);
        m_sessionSharedImages.clear();
        // The display the application initialized is initialized on the new server too: its
        // surfaces and contexts are created against it from here on.
        if (m_eglDisplayInitialized) {
            EGLint major = 0;
            EGLint minor = 0;
            if (!Server::ServerInitializeEGLDisplay(m_eglDisplay, &major, &minor)) {
                MGLOG_E("MG_Remote client: the fresh session's server did not initialize the display; "
                        "surfaces and contexts on it will fail");
            }
        }
        if (ClientSession* session = ClientSession::Active()) session->PumpControlPlane();
        RefreshFormatCapabilities();
    }

    Bool BackendObject_Remote::EnsureSurfaceHomed(EGLSurface surface) {
        if (surface == EGL_NO_SURFACE) return true;
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (m_homedSurfaces.count(surface) != 0) return true;
        const EGLSurfaceState* registered = GetRegisteredEGLSurface(surface);
        // Unknown to this object: the base class's own checks answer for it.
        if (registered == nullptr) return true;
        const EGLSurfaceState state = *registered;
        Bool recreated = false;
        const char* kind = "pbuffer";
        if (state.Kind == SurfaceKind::Pbuffer) {
            const Uint32 flags = m_standInSurfaces.count(surface) != 0 ? Server::kPbufferFlagSurfacelessStandIn : 0u;
            recreated = Server::ServerCreateEGLPbufferSurface(surface, state.Width, state.Height, nullptr, flags);
        } else if (ClientSession::IsServerOwnedWindowSurface(surface)) {
            kind = "server-owned window";
            const Server::ServerOwnedWindowReply reply =
                Server::ServerCreateServerOwnedWindowSurface(surface, state.Window.Width, state.Window.Height);
            recreated = reply.ok;
            if (recreated && reply.width != 0 && reply.height != 0) {
                if (MG_State::pEGLContext) {
                    (void)MG_State::pEGLContext->SetSurfaceExtent(surface, static_cast<EGLint>(reply.width),
                                                                  static_cast<EGLint>(reply.height));
                }
                if (ClientSession* session = ClientSession::Active())
                    session->NoteServerOwnedWindowSurface(surface, reply.width, reply.height);
            }
        } else {
            kind = "window";
            Server::ServerSetWindowHandle(state.Window);
            recreated = Server::ServerCreateEGLWindowSurface(surface, state.Window);
        }
        if (!recreated) {
            MGLOG_E("MG_Remote client: the %s surface %p (%dx%d) of a lost session could not be re-created on "
                    "the fresh one",
                    kind, static_cast<void*>(surface), state.Width, state.Height);
            return false;
        }
        if (ClientSession* session = ClientSession::Active()) session->DrainPublishedEvents();
        m_homedSurfaces.insert(surface);
        MGLOG_I("MG_Remote client: the %s surface %p (%dx%d) of a lost session is re-created on the fresh one "
                "(EGL keeps surfaces across a context loss; their contents are undefined)",
                kind, static_cast<void*>(surface), state.Width, state.Height);
        return true;
    }

    // ---- shared images ----------------------------------------------------------------------
    //
    // One shared_image record each (ClientSession::EmitSharedImage). Allocate/Import/Release are
    // table operations and order against nothing; Present and Attach ride the ring behind the GL
    // calls they follow, which is the order they need.
    Bool BackendObject_Remote::AllocateSharedImage(Uint32 width, Uint32 height, Uint32 fourcc,
                                                   MG_Backend::SharedImageExport* out) {
        ClientSession* session = ClientSession::Active();
        if (session == nullptr || out == nullptr) return false;
        // A table operation, not a draw: the calling thread's GL state (a lost context, or none) has
        // no say in whether the server allocates.
        const ClientSession::ScopedCurrentSessionWork tableOp;
        MG_Pipe::MGPSharedImageOp op{};
        op.Op = MG_Pipe::kMGPSharedImageAllocate;
        op.Format = fourcc;
        op.Width = width;
        op.Height = height;
        MG_Pipe::MGPSharedImageReply reply{};
        int fd = -1;
        if (!session->EmitSharedImage(op, -1, &reply, &fd)) return false;
        out->Id = reply.ImageId;
        out->Fd = fd;
        out->Width = reply.Width;
        out->Height = reply.Height;
        out->Fourcc = reply.Format;
        out->Stride = reply.Stride;
        out->Offset = reply.Offset;
        out->Modifier = reply.Modifier;
        {
            const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
            ++m_sessionSharedImages[reply.ImageId];
        }
        return true;
    }

    Bool BackendObject_Remote::ImportSharedImage(int fd, Uint32 width, Uint32 height, Uint32 fourcc, Uint64* outId) {
        ClientSession* session = ClientSession::Active();
        if (session == nullptr || fd < 0) return false;
        const ClientSession::ScopedCurrentSessionWork tableOp;
        MG_Pipe::MGPSharedImageOp op{};
        op.Op = MG_Pipe::kMGPSharedImageImport;
        op.Format = fourcc;
        op.Width = width;
        op.Height = height;
        MG_Pipe::MGPSharedImageReply reply{};
        if (!session->EmitSharedImage(op, fd, &reply, nullptr)) return false;
        if (outId != nullptr) *outId = reply.ImageId;
        {
            const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
            ++m_sessionSharedImages[reply.ImageId];
        }
        return true;
    }

    Bool BackendObject_Remote::ReleaseSharedImage(Uint64 id) {
        ClientSession* session = ClientSession::Active();
        if (session == nullptr || !session->Started()) return false;
        // An image of a lost session is gone with its server; the fresh one never issued its id.
        {
            const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
            const auto held = m_sessionSharedImages.find(id);
            // Nothing to release on the new server, and nothing that failed: the reference died with
            // its session.
            if (held == m_sessionSharedImages.end()) return m_lostSessionSharedImages.erase(id) != 0;
            if (--held->second == 0) m_sessionSharedImages.erase(held);
        }
        const ClientSession::ScopedCurrentSessionWork tableOp;
        MG_Pipe::MGPSharedImageOp op{};
        op.Op = MG_Pipe::kMGPSharedImageRelease;
        op.ImageId = id;
        return session->EmitSharedImage(op, -1, nullptr, nullptr);
    }

    Bool BackendObject_Remote::PresentToSharedImage(Uint64 id, const MG_Util::Damage::Region& region) {
        ClientSession* session = ClientSession::Active();
        if (session == nullptr) return false;
        if (!HoldsSharedImage(id)) return false; // a lost session's image: gone with its server
        MG_Pipe::MGPSharedImageOp op{};
        op.Op = MG_Pipe::kMGPSharedImagePresent;
        op.ImageId = id;
        op.DamageCount = MG_Util::Damage::PackRects(region, op.Damage, MG_Pipe::kMGPMaxDamageRects);
        return session->EmitSharedImage(op, -1, nullptr, nullptr);
    }

    // In the stream, so the server answers for the buffer the records after it draw into.
    Bool BackendObject_Remote::QueryBufferAge(Bool damageRegionFollows, EGLint* age) {
        ClientSession* session = ClientSession::Active();
        if (session == nullptr || age == nullptr) return false;
        MG_Pipe::MGPSharedImageOp op{};
        op.Op = MG_Pipe::kMGPSharedImageQueryBufferAge;
        op.Format = damageRegionFollows ? MG_Pipe::kMGPBufferAgeDamageRegionFollows : 0u;
        MG_Pipe::MGPSharedImageReply reply{};
        if (!session->EmitSharedImage(op, -1, &reply, nullptr)) return false;
        *age = std::max<EGLint>(reply.BufferAge, 0);
        return true;
    }

    // In the stream too: it has to reach the server before the frame's first draw.
    Bool BackendObject_Remote::SetDamageRegion(const MG_Util::Damage::Region& region) {
        ClientSession* session = ClientSession::Active();
        if (session == nullptr) return false;
        MG_Pipe::MGPSharedImageOp op{};
        op.Op = MG_Pipe::kMGPSharedImageSetDamageRegion;
        op.DamageCount = MG_Util::Damage::PackRects(region, op.Damage, MG_Pipe::kMGPMaxDamageRects);
        return session->EmitSharedImage(op, -1, nullptr, nullptr);
    }

    Bool BackendObject_Remote::AttachSharedImageToTexture(Uint64 textureLifetimeId, Uint64 id) {
        ClientSession* session = ClientSession::Active();
        if (session == nullptr) return false;
        if (!HoldsSharedImage(id)) return false; // an EGLImage of a lost session: its image is gone
        MG_Pipe::MGPSharedImageOp op{};
        op.Op = MG_Pipe::kMGPSharedImageAttach;
        op.ImageId = id;
        // The handle the texture's resource records were published under (TextureEmit's own
        // AcquireTexture is this call on the same lifetime id).
        op.Texture = MG_Pipe::MGPipeSlots().Acquire(MG_Pipe::MGPipeKind::Texture, textureLifetimeId);
        return session->EmitSharedImage(op, -1, nullptr, nullptr);
    }

    Bool BackendObject_Remote::FlushSharedImageAccesses() {
        ClientSession* session = ClientSession::Active();
        if (session == nullptr || !session->Started()) return false;
        if (m_sharedImageFlushedAt != 0 && session->LastPublishedSeq() == m_sharedImageFlushedAt) return true;
        MG_Pipe::MGPSharedImageOp op{};
        op.Op = MG_Pipe::kMGPSharedImageFlush;
        const Bool published = session->EmitSharedImage(op, -1, nullptr, nullptr);
        m_sharedImageFlushedAt = session->LastPublishedSeq();
        return published;
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
