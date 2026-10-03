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
// SetEGLSwapInterval IS NOT OVERRIDDEN (P10 B). It used to be, because the base implementation
// calls GetBackendFunctions().SetSwapInterval (BackendObject.cpp:402) and that slot was the last
// class-C Fatal in the emit table. The slot is now the forwarder the override called
// (Server::ServerSetEGLSwapInterval, EmitTables.cpp), so the base's route IS the forward and a
// second spelling of it here would only be a second place for the two to drift apart.

#pragma once
#include <Includes.h>

#include <MG_Backend/BackendObject.h>

#include <unordered_map>
#include <unordered_set>
#include <utility>

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

        // ---- the EGL lifecycle virtuals (SetEGLSwapInterval is the base's: see above) -----
        Bool InitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor) override;
        Bool CreateEGLWindowSurface(EGLSurface surface, const MG_Backend::WindowHandle& handle) override;
        Bool ResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height) override;
        // A server-owned window surface: the published surface-changed events are applied first (after the
        // server caught up), so a client polling eglQuerySurface sees a resize it drew nothing for.
        void RefreshSurfaceExtent(EGLSurface surface) override;
        Bool CreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height) override;
        Bool MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) override;
        Bool SwapEGLBuffers(EGLDisplay dpy, EGLSurface draw) override;
        Bool SwapEGLBuffersWithDamage(EGLDisplay dpy, EGLSurface draw, const MG_Util::Damage::Region& damage) override;
        void ReleaseEGLSurface(EGLSurface surface) override;
        void ReleaseEGLResources() override;

        // ---- shared images (docs/Disaggregated/notes/anland/plan-ahb-dmabuf.md) -------------
        Bool AllocateSharedImage(Uint32 width, Uint32 height, Uint32 fourcc,
                                 MG_Backend::SharedImageExport* out) override;
        Bool ImportSharedImage(int fd, Uint32 width, Uint32 height, Uint32 fourcc, Uint64* outId) override;
        Bool ReleaseSharedImage(Uint64 id) override;
        Bool PresentToSharedImage(Uint64 id, const MG_Util::Damage::Region& region) override;
        Bool QueryBufferAge(Bool damageRegionFollows, EGLint* age) override;
        Bool SetDamageRegion(const MG_Util::Damage::Region& region) override;
        Bool AttachSharedImageToTexture(Uint64 textureLifetimeId, Uint64 id) override;
        Bool FlushSharedImageAccesses() override;
        Bool CreateNativeFence(int* fence) override;

        // Copies the caps mirror's FormatCapabilityCache into the base class's
        // m_formatCapabilities. Public because CapsMirror's adoption hook is a free function
        // and this is what it calls; it is the whole of trap 2's answer.
        void RefreshFormatCapabilities();

        // A FRESH SESSION REPLACED A LOST ONE (ClientSession::RecoverAfterDeviceLoss). The
        // initialized display is initialized on the new server; every surface keeps its client
        // registration (EGL keeps surfaces across a context loss, only their contents go) but
        // has no server side yet, and is re-created there the next time it is made current,
        // swapped or resized; no thread is current any more; the shared images of the lost
        // session are forgotten without a release (their server is gone).
        void OnSessionReplaced();

        // EGL_KHR_surfaceless_context's hidden pbuffer (EGLImpl's SurfacelessStandIn): an ordinary
        // pbuffer on this side, created on the server with kPbufferFlagSurfacelessStandIn so it
        // takes no surface mode there, and re-created the same way on a fresh session.
        Bool CreateEGLSurfacelessStandIn(EGLSurface surface, EGLint width, EGLint height);

    protected:
        Bool InitPbufferSurface(EGLint width, EGLint height) override;

    private:
        // P12 (on-screen server window), D1: MOBILEGL_IPC_SURFACE=server's arm of
        // CreateEGLWindowSurface - ONE ServerOwned frame, no SetWindowHandle, the server's real
        // geometry adopted before it returns, and every refusal named in this process's log.
        Bool CreateServerOwnedWindowSurface(EGLSurface surface, const MG_Backend::WindowHandle& handle);

        // The stream position right after the last shared-image Flush record: a flush with nothing
        // published since has nothing new to fence and costs no round trip.
        Uint64 m_sharedImageFlushedAt = 0;

        // The generation of the snapshot m_formatCapabilities was filled from. Exposed only
        // through the log line on a refresh: a cache that silently stopped tracking the mirror
        // is exactly the shape trap 2 exists to prevent.
        Uint64 m_formatsGeneration = 0;

        // The server side of `surface` on the CURRENT session: re-created from its client
        // registration when the session that had it was replaced. True when it exists now.
        Bool EnsureSurfaceHomed(EGLSurface surface);
        Bool CreatePbufferOnServer(EGLSurface surface, EGLint width, EGLint height, Bool standIn);
        void NoteHomed(EGLSurface surface) {
            const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
            m_homedSurfaces.insert(surface);
        }
        Bool HoldsSharedImage(Uint64 id) const {
            const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
            return m_sessionSharedImages.count(id) != 0;
        }

        // The surfaces and shared images the CURRENT session's server holds. A surface missing
        // from the first is one a lost session had (EnsureSurfaceHomed); an image id missing from
        // the second (counted per reference taken) is a lost session's, and is neither released
        // nor presented to nor attached on the new server, which never issued it. Guarded by the
        // base's m_eglStateMutex.
        std::unordered_set<EGLSurface> m_homedSurfaces;
        // The surfaceless stand-ins among this object's pbuffers. Guarded by m_eglStateMutex.
        std::unordered_set<EGLSurface> m_standInSurfaces;
        std::unordered_map<Uint64, Uint32> m_sessionSharedImages; // id -> references taken
        // The ids a replaced session held: releasing one is a success with nothing sent.
        std::unordered_set<Uint64> m_lostSessionSharedImages;
        // The size each server-owned window surface ASKED for (0x0: the window's own), which is what a
        // fresh session is asked for again when the surface is re-created there - not the extent the
        // window happened to have, which would fix the window's size for good. Guarded by
        // m_eglStateMutex.
        std::unordered_map<EGLSurface, std::pair<Uint32, Uint32>> m_serverOwnedRequests;
        void NoteServerOwnedRequest(EGLSurface surface, Uint32 width, Uint32 height) {
            const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
            m_serverOwnedRequests[surface] = {width, height};
        }
    };

} // namespace MobileGL::MG_Remote::Client
