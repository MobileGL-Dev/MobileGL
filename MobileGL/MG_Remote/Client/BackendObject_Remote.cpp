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

        // ---- the EGL bridge ---------------------------------------------------------------
        //
        // Every one of the nine crosses as a BLOCKING control request on the apply thread,
        // because every one of them has a return value the caller acts on immediately. v1's
        // ServerLoop::RunOnApplyThread takes a raw function pointer plus a user pointer rather
        // than a std::function, deliberately: this path runs at teardown too, and the teardown
        // path may not allocate (ID-8).
        //
        // A NULL SERVER BACKEND IS "false", NOT A CRASH AND NOT A LOCAL SUCCESS. Under inproc
        // the apply thread creates it during ClientSession::Start, so a null one here means the
        // bring-up did not complete - and answering `true` would let the frontend believe it
        // has a context.
        MG_Backend::BackendObject* ServerBackend() { return Server::ServerLoopInstance().Backend(); }

        struct DisplayArgs {
            EGLDisplay Dpy;
            EGLint* Major;
            EGLint* Minor;
            Bool Ok;
        };
        MobileGLResult RunInitDisplay(void* user) {
            auto* args = static_cast<DisplayArgs*>(user);
            args->Ok = ServerBackend()->InitializeEGLDisplay(args->Dpy, args->Major, args->Minor);
            return MOBILEGL_OK;
        }

        struct WindowSurfaceArgs {
            EGLSurface Surface;
            const MG_Backend::WindowHandle* Handle;
            Bool Ok;
        };
        MobileGLResult RunCreateWindowSurface(void* user) {
            auto* args = static_cast<WindowSurfaceArgs*>(user);
            args->Ok = ServerBackend()->CreateEGLWindowSurface(args->Surface, *args->Handle);
            return MOBILEGL_OK;
        }

        struct ResizeArgs {
            EGLSurface Surface;
            Uint32 Width;
            Uint32 Height;
            Bool Ok;
        };
        MobileGLResult RunResize(void* user) {
            auto* args = static_cast<ResizeArgs*>(user);
            args->Ok = ServerBackend()->ResizeEGLWindowSurface(args->Surface, args->Width, args->Height);
            return MOBILEGL_OK;
        }

        struct PbufferArgs {
            EGLSurface Surface;
            EGLint Width;
            EGLint Height;
            Bool Ok;
        };
        MobileGLResult RunCreatePbuffer(void* user) {
            auto* args = static_cast<PbufferArgs*>(user);
            args->Ok = ServerBackend()->CreateEGLPbufferSurface(args->Surface, args->Width, args->Height);
            return MOBILEGL_OK;
        }

        struct MakeCurrentArgs {
            EGLDisplay Dpy;
            EGLSurface Draw;
            EGLSurface Read;
            EGLContext Ctx;
            Bool Ok;
        };
        MobileGLResult RunMakeCurrent(void* user) {
            auto* args = static_cast<MakeCurrentArgs*>(user);
            args->Ok = ServerBackend()->MakeEGLCurrent(args->Dpy, args->Draw, args->Read, args->Ctx);
            return MOBILEGL_OK;
        }

        struct SwapIntervalArgs {
            Int Interval;
        };
        MobileGLResult RunSwapInterval(void* user) {
            ServerBackend()->SetEGLSwapInterval(static_cast<SwapIntervalArgs*>(user)->Interval);
            return MOBILEGL_OK;
        }

        struct SurfaceArgs {
            EGLSurface Surface;
        };
        MobileGLResult RunReleaseSurface(void* user) {
            ServerBackend()->ReleaseEGLSurface(static_cast<SurfaceArgs*>(user)->Surface);
            return MOBILEGL_OK;
        }

        MobileGLResult RunReleaseResources(void*) {
            ServerBackend()->ReleaseEGLResources();
            return MOBILEGL_OK;
        }

        // Runs `work` on the apply thread if there is a server backend to run it against, and
        // says so by name when there is not.
        Bool ForwardToApplyThread(const char* what, Server::ServerLoop::ControlWork work, void* user) {
            if (ServerBackend() == nullptr) {
                MGLOG_E("MG_Remote client: %s has no server backend to forward to - the apply "
                        "thread's bring-up did not complete, and answering success here would "
                        "tell the frontend it has a context it does not have",
                        what);
                return false;
            }
            return Server::ServerLoopInstance().RunOnApplyThread(work, user) == MOBILEGL_OK;
        }

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
        DisplayArgs args{dpy, major, minor, false};
        if (!ForwardToApplyThread("InitializeEGLDisplay", &RunInitDisplay, &args) || !args.Ok) {
            return false;
        }
        return MG_Backend::BackendObject::InitializeEGLDisplay(dpy, major, minor);
    }

    Bool BackendObject_Remote::CreateEGLWindowSurface(EGLSurface surface,
                                                      const MG_Backend::WindowHandle& handle) {
        WindowSurfaceArgs args{surface, &handle, false};
        if (!ForwardToApplyThread("CreateEGLWindowSurface", &RunCreateWindowSurface, &args) ||
            !args.Ok) {
            return false;
        }
        return MG_Backend::BackendObject::CreateEGLWindowSurface(surface, handle);
    }

    Bool BackendObject_Remote::ResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height) {
        ResizeArgs args{surface, width, height, false};
        if (!ForwardToApplyThread("ResizeEGLWindowSurface", &RunResize, &args) || !args.Ok) {
            return false;
        }
        return MG_Backend::BackendObject::ResizeEGLWindowSurface(surface, width, height);
    }

    Bool BackendObject_Remote::CreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height) {
        PbufferArgs args{surface, width, height, false};
        if (!ForwardToApplyThread("CreateEGLPbufferSurface", &RunCreatePbuffer, &args) || !args.Ok) {
            return false;
        }
        return MG_Backend::BackendObject::CreateEGLPbufferSurface(surface, width, height);
    }

    Bool BackendObject_Remote::MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                                              EGLContext ctx) {
        MakeCurrentArgs args{dpy, draw, read, ctx, false};
        if (!ForwardToApplyThread("MakeEGLCurrent", &RunMakeCurrent, &args) || !args.Ok) {
            return false;
        }
        // AND ONLY NOW the client's own bookkeeping, which is what calls InitCapabilities.
        return MG_Backend::BackendObject::MakeEGLCurrent(dpy, draw, read, ctx);
    }

    Bool BackendObject_Remote::SwapEGLBuffers(EGLDisplay dpy, EGLSurface draw) {
        // NOT FORWARDED, and this is the one that must not be. The base implementation's last
        // act is GetBackendFunctions().Present() (BackendObject.cpp:396) - which is this
        // client's class-B Present EMITTER, the only route by which Present is reached at all
        // (it has zero MG_Impl call sites). Forwarding would present on the server directly and
        // put no record on the wire, which is the shape every gate in this phase exists to
        // catch.
        return MG_Backend::BackendObject::SwapEGLBuffers(dpy, draw);
    }

    void BackendObject_Remote::SetEGLSwapInterval(Int interval) {
        // OVERRIDDEN BECAUSE THE BASE WOULD FATAL. BackendObject.cpp:402 null-checks
        // GetBackendFunctions().SetSwapInterval and calls it when non-null - one of the 41
        // null checks R-4 turns into "always supported" - and SetSwapInterval is class C, so
        // the base implementation would abort on every eglSwapInterval. The answer is the
        // caps-mirror-read rule's general shape: the question "can the presentation path take
        // an interval" belongs to the server, so it is asked of the server.
        SwapIntervalArgs args{interval};
        ForwardToApplyThread("SetEGLSwapInterval", &RunSwapInterval, &args);
    }

    void BackendObject_Remote::ReleaseEGLSurface(EGLSurface surface) {
        SurfaceArgs args{surface};
        ForwardToApplyThread("ReleaseEGLSurface", &RunReleaseSurface, &args);
        MG_Backend::BackendObject::ReleaseEGLSurface(surface);
    }

    void BackendObject_Remote::ReleaseEGLResources() {
        // BLOCKING BY CONTRACT (ServerLoop.h's header note): MobileGL::Destroy()
        // (MobileGL/Init.cpp:68) walks on the moment this returns, and the server still holds
        // the context until the apply thread has run it.
        ForwardToApplyThread("ReleaseEGLResources", &RunReleaseResources, nullptr);
        MG_Backend::BackendObject::ReleaseEGLResources();
    }

} // namespace MobileGL::MG_Remote::Client
