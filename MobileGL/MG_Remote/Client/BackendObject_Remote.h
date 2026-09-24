// MobileGL - MobileGL/MG_Remote/Client/BackendObject_Remote.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// THE CLIENT ROLE'S BackendObject. Owner: package c1.
//
// MG_Backend::Init() installs one of these into pActiveBackendObject when the transport
// resolved (the hook is v1's; the object is c1's, table 3). It is NOT a backend: it owns no
// context, no driver and no GL state. It is the frontend's single answer to four questions -
// "what can the device do", "what backend is it", "what table do I call", and the nine EGL
// lifecycle calls - and each of the four is answered somewhere that is not here.
//
// THE THREE TRAPS THE SCOUT FOUND, AND WHERE EACH IS PAID:
//
//  1. GetRendererInfo() RETURNS A REFERENCE (BackendObject.h:590), so the object cannot
//     synthesise one per call. CapsMirror owns the storage; this class forwards. And
//     LogBackendInfo() reads it at MG_Backend/Init.cpp:21, DURING MG_Backend::Init(), before
//     any surface exists - so the mirror answers a placeholder and P5 accepts one imprecise
//     startup log line. MG_Backend::Init() is NOT restructured (scout-caps-reply §1.2 (a)).
//
//  2. GetFormatCapabilities() IS NOT VIRTUAL (BackendObject.h:594). It returns the base class's
//     own m_formatCapabilities member, so there is no accessor to override: the cache has to be
//     PUSHED into that member, and the only moment this object can know to is when a snapshot
//     lands. CapsMirror's adoption hook is that moment.
//
//  3. GetBackendType() MUST ANSWER THE SERVER'S BACKEND. There is no "Remote" enumerator and
//     there must not be one: GL_Framebuffer.cpp:47, GL_Texture.cpp:6536 and CompileEnv.cpp:122
//     switch on this value, and a value they do not know takes a WRONG ARM rather than failing.
//
// THE EGL VIRTUALS ARE BOTH FORWARDED AND KEPT. The base class runs a real state machine -
// surface registration, per-thread current-context bookkeeping, the lazy InitCapabilities latch,
// and SwapEGLBuffers' route into GetBackendFunctions().Present() - and the client needs all of
// it, because Present is a class-B emitter reached through exactly that route (the verb census's
// trap 3: Present has zero MG_Impl call sites). So each override does BOTH: it runs the real
// EGL work on the apply thread, through ServerLoop's control-frame channel (P5f fc; v1's
// function-pointer mailbox before it), and then lets the
// base class keep the client-side books.
//
// SetEGLSwapInterval IS THE ONE THAT MUST NOT REACH THE TABLE. The base implementation
// null-checks GetBackendFunctions().SetSwapInterval (BackendObject.cpp:402) - one of the 41
// null checks R-4 turns into "always supported" - and SetSwapInterval is class C, so the base
// implementation would Fatal on every eglSwapInterval. It is overridden to forward instead,
// which is the caps-mirror-read rule's shape for a slot whose answer is "ask the server".

#pragma once
#include <Includes.h>

#include <MG_Backend/BackendObject.h>

namespace MobileGL::MG_Remote::Client {

    class BackendObject_Remote final : public MG_Backend::BackendObject {
    public:
        BackendObject_Remote();
        ~BackendObject_Remote() override;

        // ---- the eight pure virtuals ------------------------------------------------------
        void Initialize() override;
        Bool InitCapabilities() override;
        Bool InitWindowSurface() override;
        const RendererInfo& GetRendererInfo() const override;
        String GetBackendAPIVersionString() const override;
        const MG_Backend::GlobalBackendFunctionsTable& GetBackendFunctions() const override;
        const MG_Backend::DynamicBackendParameters& GetDynamicParameters() const override;
        BackendType GetBackendType() const override;

        // ---- the nine EGL lifecycle virtuals ---------------------------------------------
        Bool InitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor) override;
        Bool CreateEGLWindowSurface(EGLSurface surface, const MG_Backend::WindowHandle& handle) override;
        Bool ResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height) override;
        Bool CreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height) override;
        Bool MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) override;
        Bool SwapEGLBuffers(EGLDisplay dpy, EGLSurface draw) override;
        void SetEGLSwapInterval(Int interval) override;
        void ReleaseEGLSurface(EGLSurface surface) override;
        void ReleaseEGLResources() override;

        // Copies the caps mirror's FormatCapabilityCache into the base class's
        // m_formatCapabilities. Public because CapsMirror's adoption hook is a free function
        // and this is what it calls; it is the whole of trap 2's answer.
        void RefreshFormatCapabilities();

    protected:
        Bool InitPbufferSurface(EGLint width, EGLint height) override;

    private:
        // P12 (on-screen server window), D1: MOBILEGL_IPC_SURFACE=server's arm of
        // CreateEGLWindowSurface - ONE ServerOwned frame, no SetWindowHandle, the server's real
        // geometry adopted before it returns, and every refusal named in this process's log.
        Bool CreateServerOwnedWindowSurface(EGLSurface surface, const MG_Backend::WindowHandle& handle);

        // The generation of the snapshot m_formatCapabilities was filled from. Exposed only
        // through the log line on a refresh: a cache that silently stopped tracking the mirror
        // is exactly the shape trap 2 exists to prevent.
        Uint64 m_formatsGeneration = 0;
    };

} // namespace MobileGL::MG_Remote::Client
