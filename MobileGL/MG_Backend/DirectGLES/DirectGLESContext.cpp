// MobileGL - MobileGL/MG_Backend/DirectGLES/DirectGLESContext.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DirectGLESInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    namespace PrgramImpl {
        // The identity+version key above is only monotonic WITHIN one GLContext: a
        // library teardown + re-init frees every FramebufferObject and restarts the
        // draw slot's counter at zero, so a recycled FBO address with coinciding
        // fresh versions would false-hit. Cleared at the same boundaries as the
        // structurally identical SyncCurrentFBO trio (InvalidateFramebufferBindingCache).
        void InvalidateBroadcastMemo() {
            g_broadcastMemoValid = false;
            g_broadcastMemoHandleValid = false;
        }
    } // namespace PrgramImpl

    static void ReleasePackedWordScratchTexture() {
        // The ES context (and the name with it) is gone; deleting here would target a recycled
        // name in the successor context.
        g_packedWordScratchTextureId = 0;
        g_packedWordScratchWidth = 0;
        g_packedWordScratchHeight = 0;
    }

    // ---------------------------------------------------------------------------------
    // P14 S4 (docs/Disaggregated/design/11-state-ownership.md): the native tuple is per context.
    //
    // THE DISPLAY STAYS ONE OBJECT. eglInitialize is once per process and eglTerminate runs when the
    // last native context has gone, so a second context - or a second session - never terminates the
    // display the first one is still using.
    //
    // EVERYTHING ELSE THAT WAS A FILE-LEVEL SINGLETON IS KEYED. A native EGLContext, the config it
    // was created with and the native surfaces are keyed by (session, client context token): the
    // identity S1 put on the wire (bind_context / SurfaceOp.context) and S2 gave a per-thread owner.
    // The key is resolved on the calling thread through a probe the server layer installs
    // (SetNativeContextKeyResolver); with none installed the answer is the single key {0, 0}, which
    // is byte-for-byte the process singleton this replaces - monolith, a frontend-only process and
    // every unit case keep exactly one tuple.
    //
    // WHY THE SURFACES ARE KEYED BY SESSION AND NOT BY CONTEXT: an EGLSurface is not owned by an
    // EGLContext (EGL has no such relationship - only the draw/read BINDING is per context), so a
    // session's surfaces are one set and each context records which of them it currently draws to.
    // That is also what makes pbuffer -> window -> pbuffer keep its objects: creating the second
    // surface does not touch the context, so the twins the first surface's draws built are still
    // there when the third one is bound.

    EGLDisplay g_Display = EGL_NO_DISPLAY;
    // eglInitialize/eglTerminate pairing: the display is terminated only when the count drops to 0.
    static Uint32 g_displayUsers = 0;
    // How many times the display has been terminated. eglTerminate destroys every EGLImage made on
    // it, and a re-initialized display hands back the same EGLDisplay handle, so a holder destroys
    // its EGLImage itself only while this has not moved since the image was made.
    std::atomic<Uint64> g_displayTerminations{0};

#if MOBILEGL_BUILD_DISAGGREGATED
    namespace SharedImageImpl {
        // The display (and every EGLImage and renderbuffer made on it) is gone: forget the present
        // targets without calling into the driver, releasing the images they held.
        void ForgetPresentTargetsAfterTerminate();
    } // namespace SharedImageImpl
#endif
    static void NoteDisplayTerminated() {
        g_displayTerminations.fetch_add(1, std::memory_order_acq_rel);
#if MOBILEGL_BUILD_DISAGGREGATED
        SharedImageImpl::ForgetPresentTargetsAfterTerminate();
#endif
    }

    std::mutex g_nativeRegistryMutex;
    // Bumped on every registry mutation. The per-thread active-tuple cache below is keyed on it, so
    // a destroyed tuple can never be handed out from a stale cache entry.
    std::atomic<Uint64> g_nativeRegistryEpoch{1};
    // P15: moves whenever the size QueryCurrentSurfaceSize would answer may have moved - after every
    // swap (a window surface takes a new size at its next swap) and at every change of a session's
    // draw surface (NoteDrawSurfaceMoved at each `session.Draw =`). Surface creation and destruction
    // also move g_nativeRegistryEpoch, which the size memo keys on as well.
    std::atomic<Uint64> g_surfaceSizeEpoch{1};
    // Never erased while the process lives: a context teardown marks its tuple dead (Context =
    // EGL_NO_CONTEXT) instead of freeing it, so a cached pointer is never dangling between the
    // epoch bump and the next resolution.
    Vector<NativeContextTuple*>& NativeContexts() {
        static Vector<NativeContextTuple*>* contexts = new Vector<NativeContextTuple*>();
        return *contexts;
    }
    Vector<NativeSessionState*>& NativeSessions() {
        static Vector<NativeSessionState*>* sessions = new Vector<NativeSessionState*>();
        return *sessions;
    }

    std::atomic<NativeContextKeyResolver> g_nativeContextKeyResolver{nullptr};
    thread_local NativeThreadState t_native;

    void SetNativeContextKeyResolver(NativeContextKeyResolver resolver) {
        g_nativeContextKeyResolver.store(resolver, std::memory_order_release);
    }

    // P14 S6: the twin tables' key. Same probe/fallback shape as the native tuple's above and
    // for the same reason (the whole single-context world must keep getting one key), and
    // deliberately a SEPARATE resolver: this one answers for a table that is consulted on paths
    // the native tuple is not, and a case that drives twins without EGL must be able to set it
    // alone. See DirectGLES.h for what the two halves mean.
    static std::atomic<TwinKeyResolver> g_twinKeyResolver{nullptr};

    void SetTwinKeyResolver(TwinKeyResolver resolver) {
        g_twinKeyResolver.store(resolver, std::memory_order_release);
    }

    Bool CurrentTwinKey(TwinKey* outKey) {
        outKey->SessionKey = 0;
        outKey->ShareGroupKey = 0;
        const TwinKeyResolver resolver = g_twinKeyResolver.load(std::memory_order_acquire);
        if (resolver == nullptr) return false;
        return resolver(outKey);
    }

    Uint64 CurrentNativeSessionKey() {
        Uint64 session = 0;
        Uint64 token = 0;
        ResolveNativeKey(session, token);
        return session;
    }

    namespace {
        // The active tuple for this thread. The fast path is a key compare against a thread-local
        // cache, because IsBackendContextCurrentOnThisThread sits under every draw; the slow path
        // (a first call, a bind_context switch, or any registry mutation) walks the registry, which
        // holds one tuple per live client context.
        // (t_native.activeTuple / activeEpoch / activeSession / activeToken)
        // The native context THIS THREAD last made current. An EGLContext can be current on only one
        // thread, and MakeCurrent is the only writer, so a plain thread-local is exact - and it is
        // what makes MakeNativeContextCurrentForBoundToken a no-op when the bind did not actually
        // change the native context (the single-context world's every bind).
        // (t_native.boundContext)
        // And the surface it was made current with: a bind to the same context on a different
        // surface (one client context drawing to several windows) is a real switch.
        // (t_native.boundDraw)
        // ...and that context's lifetime serial, the key of its container objects.
        // (t_nativeContextThread.BoundSerial, DirectGLES.h)
        std::atomic<Uint64> g_nativeContextSerials{0};

        std::mutex& PerNativeContextRegistryMutex() {
            static auto* mutex = new std::mutex();
            return *mutex;
        }
        Vector<PerNativeContextBase*>& PerNativeContextRegistry() {
            static auto* registry = new Vector<PerNativeContextBase*>();
            return *registry;
        }
    } // namespace

    namespace {
        std::atomic<Uint64> g_perNativeContextEpoch{1};
    } // namespace
    Uint64 PerNativeContextEpoch() {
        return g_perNativeContextEpoch.load(std::memory_order_acquire);
    }
    void BumpPerNativeContextEpoch() {
        g_perNativeContextEpoch.fetch_add(1, std::memory_order_acq_rel);
    }

    namespace {
        std::atomic<Uint32> g_perNativeContextCacheSlots{0};
    } // namespace

    PerNativeContextBase::PerNativeContextBase()
        : m_cacheSlot(g_perNativeContextCacheSlots.fetch_add(1, std::memory_order_relaxed) %
                      NativeContextThreadState::kCacheEntries) {
        const std::lock_guard<std::mutex> lock(PerNativeContextRegistryMutex());
        PerNativeContextRegistry().push_back(this);
    }

    PerNativeContextBase::~PerNativeContextBase() {
        BumpPerNativeContextEpoch();
        const std::lock_guard<std::mutex> lock(PerNativeContextRegistryMutex());
        auto& registry = PerNativeContextRegistry();
        registry.erase(std::remove(registry.begin(), registry.end(), this), registry.end());
    }

    void ForgetNativeContextObjects(Uint64 serial) {
        if (serial == 0) return;
        const std::lock_guard<std::mutex> lock(PerNativeContextRegistryMutex());
        for (PerNativeContextBase* holder : PerNativeContextRegistry())
            holder->ForgetContext(serial);
    }

    // The tuple the calling thread works on, created on first use. Lazy creation is what keeps the
    // pre-S4 shapes working unchanged: a session that never crossed a CreateContext - the whole
    // single-context world, and every existing fixture - resolves to its token-0 tuple and gets
    // exactly the one native context it always had.
    NativeContextTuple& ActiveNativeContext() {
        NativeThreadState& tls = t_native;
        Uint64 session = 0;
        Uint64 token = 0;
        if (tls.forcedValid) {
            session = tls.forcedSession;
            token = tls.forcedToken;
        } else {
            const NativeContextKeyResolver resolver = g_nativeContextKeyResolver.load(std::memory_order_acquire);
            if (resolver != nullptr) (void)resolver(&session, &token);
        }
        const Uint64 epoch = g_nativeRegistryEpoch.load(std::memory_order_acquire);
        if (tls.activeTuple != nullptr && tls.activeEpoch == epoch && tls.activeSession == session &&
            tls.activeToken == token) {
            return *tls.activeTuple;
        }
        const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
        NativeContextTuple* found = nullptr;
        for (auto* tuple : NativeContexts()) {
            if (tuple->SessionKey == session && tuple->ContextToken == token) {
                found = tuple;
                break;
            }
        }
        if (found == nullptr) {
            found = new NativeContextTuple{};
            found->SessionKey = session;
            found->ContextToken = token;
            NativeContexts().push_back(found);
            (void)AcquireNativeSession(session);
            g_nativeRegistryEpoch.fetch_add(1, std::memory_order_acq_rel);
        }
        t_native.activeTuple = found;
        t_native.activeEpoch = g_nativeRegistryEpoch.load(std::memory_order_acquire);
        t_native.activeSession = session;
        t_native.activeToken = token;
        return *found;
    }

    static NativeContextTuple* FindNativeContext(Uint64 sessionKey, Uint64 contextToken) {
        for (auto* tuple : NativeContexts()) {
            if (tuple->SessionKey == sessionKey && tuple->ContextToken == contextToken) return tuple;
        }
        return nullptr;
    }

    Uint64 NativeContextCount() {
        const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
        Uint64 count = 0;
        for (auto* tuple : NativeContexts()) {
            if (tuple->Context != EGL_NO_CONTEXT) ++count;
        }
        return count;
    }

    Uint64 NativeContextCountForSession(Uint64 sessionKey) {
        const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
        Uint64 count = 0;
        for (auto* tuple : NativeContexts()) {
            if (tuple->SessionKey == sessionKey && tuple->Context != EGL_NO_CONTEXT) ++count;
        }
        return count;
    }

    EGLContext NativeContextHandleFor(Uint64 sessionKey, Uint64 contextToken) {
        const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
        const NativeContextTuple* tuple = FindNativeContext(sessionKey, contextToken);
        return tuple != nullptr ? tuple->Context : EGL_NO_CONTEXT;
    }

    void ForceNativeContextKeyForTesting(Uint64 sessionKey, Uint64 contextToken) {
        t_native.forcedSession = sessionKey;
        t_native.forcedToken = contextToken;
        t_native.forcedValid = true;
        // The cache is keyed on the effective key, so drop it rather than let a stale entry answer.
        t_native.activeTuple = nullptr;
        t_native.activeEpoch = 0;
    }

    // The display's one-time initialization. Returns false only when there is no EGL to talk to.
    static Bool EnsureDisplayInitialized() {
        if (g_Display != EGL_NO_DISPLAY) return true;
        if (!g_EGLFuncs.eglGetDisplay) return false;
        g_Display = g_EGLFuncs.eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (g_Display == EGL_NO_DISPLAY) return false;
        if (!g_EGLFuncs.eglInitialize(g_Display, nullptr, nullptr)) {
            g_Display = EGL_NO_DISPLAY;
            return false;
        }
        if (g_EGLFuncs.eglBindAPI) g_EGLFuncs.eglBindAPI(EGL_OPENGL_ES_API);
        return true;
    }

#if MOBILEGL_BUILD_DISAGGREGATED
    // P12 (on-screen server window), D3: true only while InitWindowSurface publishes the default
    // framebuffer's shape, so that publish - and no pbuffer's - may carry the surface's extent.
    // Split builds only: the pull build's publish is unchanged (G1).
    static Bool g_publishWindowExtent = false;
#endif

    Bool QueryCurrentSurfaceSize(Int& outWidth, Int& outHeight) {
        outWidth = 0;
        outHeight = 0;
        // P15: ANSWERED FROM A MEMO while nothing that moves the size moved. Two eglQuerySurface
        // calls behind the registry lock on every dirty render-state sync (draws and clears whose
        // scissor box the app never wrote) cost 0.065 ms/frame on FCL Espryt.
        Uint64 memoSession = 0, memoToken = 0;
        ResolveNativeKey(memoSession, memoToken);
        const Uint64 registryEpoch = g_nativeRegistryEpoch.load(std::memory_order_acquire);
        const Uint64 sizeEpoch = g_surfaceSizeEpoch.load(std::memory_order_acquire);
        NativeThreadState& memo = t_native;
        if (memo.sizeMemoValid && memo.sizeMemoSession == memoSession && memo.sizeMemoToken == memoToken &&
            memo.sizeMemoRegistryEpoch == registryEpoch && memo.sizeMemoSizeEpoch == sizeEpoch) {
            outWidth = memo.sizeMemoWidth;
            outHeight = memo.sizeMemoHeight;
            return true;
        }
        // P14 S4: the surface is the SESSION's, not the process's - the tuple that draws to it is
        // one of the session's contexts.
        const EGLSurface surface = ActiveNativeSession().Draw;
        if (!g_EGLFuncs.eglQuerySurface || g_Display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE) {
            return false;
        }

        EGLint width = 0;
        EGLint height = 0;
        if (!g_EGLFuncs.eglQuerySurface(g_Display, surface, EGL_WIDTH, &width) ||
            !g_EGLFuncs.eglQuerySurface(g_Display, surface, EGL_HEIGHT, &height) || width <= 0 || height <= 0) {
            return false;
        }

        outWidth = static_cast<Int>(width);
        outHeight = static_cast<Int>(height);
        memo.sizeMemoValid = true;
        memo.sizeMemoSession = memoSession;
        memo.sizeMemoToken = memoToken;
        memo.sizeMemoRegistryEpoch = registryEpoch;
        memo.sizeMemoSizeEpoch = sizeEpoch;
        memo.sizeMemoWidth = outWidth;
        memo.sizeMemoHeight = outHeight;
        return true;
    }

    // The frontend's default framebuffer is a placeholder FramebufferObject whose attachments
    // carry a format and nothing else (MG_Impl/Init.cpp), and it is built before any surface
    // exists - so it starts on a guess, GL_DEPTH32F_STENCIL8. Every attachment query about the
    // default framebuffer is answered out of that guess, and being wrong is not cosmetic: GL
    // blits depth/stencil only between IDENTICAL formats, so a caller that reads
    // GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, allocates the buffer it was told about and blits
    // gets GL_INVALID_OPERATION and a silently dropped blit - colour bits included, because a
    // rejected glBlitFramebuffer transfers nothing at all. DirectVulkan already publishes its
    // real format when it creates the swapchain (SwapchainObject::Create); this is the
    // DirectGLES half, and here the answer can simply be asked of the ES default framebuffer.
    //
    // Only the format is published. The placeholder's 512x512 extent is left alone: all three
    // attachments share it, and FramebufferObject::CheckCompleteness requires them to agree, so
    // resizing depth/stencil without colour would report the default framebuffer incomplete.
    static void PublishDefaultFramebufferDepthStencilFormat() {
        auto& defaultFBOInfo = MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo;
        // P8-SE: THE PLACEHOLDER IS ONLY THE MONOLITH ARM'S TARGET. With a transport the answer is
        // posted (OnSurfaceChanged, below) and the client writes its own placeholder; a spawn or tcp
        // server process has no frontend at all, so pDefaultFramebufferInfo is null there. Gating on
        // it returned before the post, the client kept Init.cpp's GL_DEPTH32F_STENCIL8 guess for a
        // D24S8 pbuffer, and a blit into a buffer allocated from that guess lost depth and stencil
        // (docs/archive/notes/p8/SE.md). Inproc only passed because client and server share the process.
        if (!g_GLESFuncs.glGetFramebufferAttachmentParameteriv) return;
        if (!defaultFBOInfo && MG_Pipe::gMGPipeCallbacks.OnSurfaceChanged == nullptr) return;

        // GL_DEPTH / GL_STENCIL are the default framebuffer's spellings; a user framebuffer
        // would need GL_DEPTH_ATTACHMENT / GL_STENCIL_ATTACHMENT and answers GL_INVALID_ENUM
        // for these. Nothing else can be bound this early, but bind explicitly so the answer
        // describes the default framebuffer even if this is ever called later.
        GLint previousDrawFramebuffer = 0;
        g_GLESFuncs.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
        if (previousDrawFramebuffer != 0) {
            g_GLESFuncs.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        }

        ClearGLErrors();
        GLint depthBits = 0;
        GLint stencilBits = 0;
        GLint depthComponentType = GL_UNSIGNED_NORMALIZED;
        g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH,
                                                          GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depthBits);
        g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_STENCIL,
                                                          GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencilBits);
        g_GLESFuncs.glGetFramebufferAttachmentParameteriv(
            GL_DRAW_FRAMEBUFFER, GL_DEPTH, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &depthComponentType);
        ClearGLErrors();

        if (previousDrawFramebuffer != 0) {
            g_GLESFuncs.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
        }
        FramebufferImpl::InvalidateFramebufferBindingCache();
        FramebufferImpl::InvalidateFramebufferHandleArmMemos();

        // A driver that refuses the query leaves both at 0. Fall back to what the EGL config
        // was chosen with, which is what the surface actually has. P14 S4: the config belongs to the
        // context the surface is bound to, so it is the active tuple's.
        const EGLConfig activeConfig = ActiveNativeContext().Config;
        if (depthBits == 0 && stencilBits == 0 && g_EGLFuncs.eglGetConfigAttrib && g_Display != EGL_NO_DISPLAY &&
            activeConfig != nullptr) {
            EGLint eglDepth = 0;
            EGLint eglStencil = 0;
            if (g_EGLFuncs.eglGetConfigAttrib(g_Display, activeConfig, EGL_DEPTH_SIZE, &eglDepth)) {
                depthBits = static_cast<GLint>(eglDepth);
            }
            if (g_EGLFuncs.eglGetConfigAttrib(g_Display, activeConfig, EGL_STENCIL_SIZE, &eglStencil)) {
                stencilBits = static_cast<GLint>(eglStencil);
            }
        }

        if (depthBits <= 0 && stencilBits <= 0) {
            // Nothing usable came back from either witness. Returning here leaves whatever a
            // PREVIOUS surface published in place, which would be stale - this function runs
            // once per surface activation, not once per process. It is written this way anyway
            // because the branch is unreachable for a surface MobileGL chose itself:
            // ChooseConfigForSurface asks eglChooseConfig for EGL_DEPTH_SIZE 24 and
            // EGL_STENCIL_SIZE 8, and so does its alpha-free retry, so the context's config always
            // has both and the EGL fallback above always answers. A caller that supplies its own
            // depth-less config would keep the previous surface's description; publishing a
            // guess instead would be a different lie, and the placeholder attachment model has
            // no way to say "this buffer does not exist" short of detaching it.
            MGLOG_D("DirectGLES: default framebuffer reports no depth or stencil; leaving the "
                    "placeholder attachment formats untouched");
            return;
        }

        const Bool floatDepth = depthComponentType == GL_FLOAT;
        TextureInternalFormat depthFormat = TextureInternalFormat::Depth24Stencil8;
        TextureInternalFormat stencilFormat = TextureInternalFormat::Depth24Stencil8;
        if (depthBits > 0 && stencilBits > 0) {
            // Packed: both frontend attachments name the same combined format, as DirectVulkan does.
            depthFormat = (floatDepth || depthBits > 24) ? TextureInternalFormat::Depth32FStencil8
                                                         : TextureInternalFormat::Depth24Stencil8;
            stencilFormat = depthFormat;
        } else if (depthBits > 0) {
            depthFormat = floatDepth          ? TextureInternalFormat::DepthComponent32F
                          : (depthBits <= 16) ? TextureInternalFormat::DepthComponent16
                                              : TextureInternalFormat::DepthComponent24;
            stencilFormat = depthFormat;
        } else {
            depthFormat = TextureInternalFormat::StencilIndex8;
            stencilFormat = TextureInternalFormat::StencilIndex8;
        }

        // Null in a spawn / tcp server (above); only the monolith arm below writes through them.
        auto* depthTexture = defaultFBOInfo ? defaultFBOInfo->depthAttachment.get() : nullptr;
        auto* stencilTexture = defaultFBOInfo ? defaultFBOInfo->stencilAttachment.get() : nullptr;
        if (MG_Pipe::gMGPipeCallbacks.OnSurfaceChanged != nullptr) {
            // P5c ev (CONTRACT-P5C §4.2): with an active transport the attachments are CLIENT
            // memory and this thread may not write them - the backend fills MGPSurfaceInfo
            // and posts, and the client applies it to its own pDefaultFramebufferInfo on the
            // GL thread. FORMAT ONLY, exactly as the monolith arm below: Width/Height stay 0,
            // which is the consumer's cue to leave the placeholder extent untouched. The
            // callback's presence IS the transport probe - the server session installs it at
            // Accept, and under monolith nobody ever does.
            MG_Pipe::MGPSurfaceInfo info{};
            info.InternalFormat = static_cast<Uint32>(depthFormat);
            info.IsDefault = 1;
#if MOBILEGL_BUILD_DISAGGREGATED
            // P12 (on-screen server window), D3: A WINDOW SURFACE OF A REMOTE SESSION ALSO SAYS HOW
            // BIG IT IS. Under spawn / tcp (this server process set Transport=Spawn in RunSession)
            // the only window surface is the server's own - a headless client's ServerOwned one -
            // and that client learns its extent here and from the reply, as Magma's swapchain
            // already tells it. An extent switches the client's consumer to the reallocating arm
            // (ClientSession.cpp ApplySurfaceChangedToClient), so it is deliberately NOT published
            // for a pbuffer, nor under inproc / monolith: those keep the format-only shape above.
            if (g_publishWindowExtent && MG_Config::Transport == MG_Config::TransportMode::Spawn) {
                // THE DISPLAY'S EXTENT, NEVER THE SURFACE'S: eglQuerySurface answers the buffer last
                // dequeued, which after a resize is still the old size - publishing it told the client a
                // size the window no longer had.
                Uint32 extentWidth = 0;
                Uint32 extentHeight = 0;
                if (MG_Pipe::MGPipeServerOwnedWindowExtent(&extentWidth, &extentHeight)) {
                    info.Width = extentWidth;
                    info.Height = extentHeight;
                }
            }
#endif
            MG_Pipe::gMGPipeCallbacks.OnSurfaceChanged(&info);
        } else {
            if (depthTexture) depthTexture->SetInternalFormat(depthFormat);
            if (stencilTexture) stencilTexture->SetInternalFormat(stencilFormat);
        }
        MGLOG_D("DirectGLES: default framebuffer depth=%d stencil=%d float=%d; published attachment "
                "formats depth=%d stencil=%d",
                depthBits, stencilBits, floatDepth ? 1 : 0, static_cast<int>(depthFormat),
                static_cast<int>(stencilFormat));
    }

#if defined(__linux__) && !defined(__ANDROID__)
    static void* OpenX11Lib() {
        void* x11Lib = dlopen("libX11.so.6", RTLD_LOCAL | RTLD_NOW);
        if (!x11Lib) {
            x11Lib = dlopen("libX11.so", RTLD_LOCAL | RTLD_NOW);
        }
        return x11Lib;
    }
#endif

    static EGLint QueryDefaultX11VisualId() {
#if defined(__linux__) && !defined(__ANDROID__)
        const char* displayName = std::getenv("DISPLAY");
        if (!displayName || !MG_Util::X11::MayDialX11Display(displayName)) {
            return 0;
        }

        void* x11Lib = OpenX11Lib();
        if (!x11Lib) {
            return 0;
        }

        using XOpenDisplayFn = void* (*)(const char*);
        using XDefaultScreenFn = int (*)(void*);
        using XDefaultVisualFn = void* (*)(void*, int);
        using XVisualIDFromVisualFn = unsigned long (*)(void*);
        using XCloseDisplayFn = int (*)(void*);

        auto* xOpenDisplay = reinterpret_cast<XOpenDisplayFn>(dlsym(x11Lib, "XOpenDisplay"));
        auto* xDefaultScreen = reinterpret_cast<XDefaultScreenFn>(dlsym(x11Lib, "XDefaultScreen"));
        auto* xDefaultVisual = reinterpret_cast<XDefaultVisualFn>(dlsym(x11Lib, "XDefaultVisual"));
        auto* xVisualIDFromVisual = reinterpret_cast<XVisualIDFromVisualFn>(dlsym(x11Lib, "XVisualIDFromVisual"));
        auto* xCloseDisplay = reinterpret_cast<XCloseDisplayFn>(dlsym(x11Lib, "XCloseDisplay"));
        if (!xOpenDisplay || !xDefaultScreen || !xDefaultVisual || !xVisualIDFromVisual || !xCloseDisplay) {
            dlclose(x11Lib);
            return 0;
        }

        void* display = xOpenDisplay(displayName);
        if (!display) {
            dlclose(x11Lib);
            return 0;
        }
        const int screen = xDefaultScreen(display);
        void* visual = xDefaultVisual(display, screen);
        const auto visualId = visual ? static_cast<EGLint>(xVisualIDFromVisual(visual)) : 0;
        xCloseDisplay(display);
        dlclose(x11Lib);
        return visualId;
#else
        return 0;
#endif
    }

    static EGLint QueryX11WindowVisualId(NativeWindowType window) {
#if defined(__linux__) && !defined(__ANDROID__) && __has_include(<X11/Xlib.h>)
        if (!window) {
            return 0;
        }
        const char* displayName = std::getenv("DISPLAY");
        if (!displayName || !MG_Util::X11::MayDialX11Display(displayName)) {
            return 0;
        }

        void* x11Lib = OpenX11Lib();
        if (!x11Lib) {
            return 0;
        }

        using XOpenDisplayFn = Display* (*)(const char*);
        using XGetWindowAttributesFn = int (*)(Display*, Window, XWindowAttributes*);
        using XVisualIDFromVisualFn = unsigned long (*)(Visual*);
        using XCloseDisplayFn = int (*)(Display*);

        auto* xOpenDisplay = reinterpret_cast<XOpenDisplayFn>(dlsym(x11Lib, "XOpenDisplay"));
        auto* xGetWindowAttributes = reinterpret_cast<XGetWindowAttributesFn>(dlsym(x11Lib, "XGetWindowAttributes"));
        auto* xVisualIDFromVisual = reinterpret_cast<XVisualIDFromVisualFn>(dlsym(x11Lib, "XVisualIDFromVisual"));
        auto* xCloseDisplay = reinterpret_cast<XCloseDisplayFn>(dlsym(x11Lib, "XCloseDisplay"));
        if (!xOpenDisplay || !xGetWindowAttributes || !xVisualIDFromVisual || !xCloseDisplay) {
            dlclose(x11Lib);
            return 0;
        }

        Display* display = xOpenDisplay(displayName);
        if (!display) {
            dlclose(x11Lib);
            return 0;
        }

        XWindowAttributes attrs{};
        EGLint visualId = 0;
        if (xGetWindowAttributes(display, static_cast<Window>(window), &attrs) && attrs.visual) {
            visualId = static_cast<EGLint>(xVisualIDFromVisual(attrs.visual));
        }
        xCloseDisplay(display);
        dlclose(x11Lib);
        return visualId;
#else
        (void)window;
        return 0;
#endif
    }

    static Bool GetConfigAttrib(EGLConfig config, EGLint attr, EGLint& value) {
        return g_EGLFuncs.eglGetConfigAttrib && g_EGLFuncs.eglGetConfigAttrib(g_Display, config, attr, &value);
    }

    static Bool ConfigSupports(EGLConfig config, EGLint surfaceBit) {
        EGLint surfaceType = 0;
        EGLint renderableType = 0;
        if (!GetConfigAttrib(config, EGL_SURFACE_TYPE, surfaceType)) {
            return false;
        }
        if (!GetConfigAttrib(config, EGL_RENDERABLE_TYPE, renderableType)) {
            return false;
        }
        return (surfaceType & surfaceBit) && (renderableType & EGL_OPENGL_ES3_BIT);
    }

    static Bool ChooseConfigForSurface(EGLint surfaceBit, EGLConfig& outConfig,
                                       NativeWindowType window = static_cast<NativeWindowType>(0)) {
        const EGLint configAttribs[] = {EGL_SURFACE_TYPE, surfaceBit, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                        EGL_RED_SIZE,     8,          EGL_GREEN_SIZE,      8,
                                        EGL_BLUE_SIZE,    8,          EGL_ALPHA_SIZE,      8,
                                        EGL_DEPTH_SIZE,   24,         EGL_STENCIL_SIZE,    8,
                                        EGL_NONE};

        EGLint numConfigs = 0;
        if (!g_EGLFuncs.eglChooseConfig(g_Display, configAttribs, nullptr, 0, &numConfigs) || numConfigs == 0) {
            return false;
        }

        Vector<EGLConfig> configs(static_cast<SizeT>(numConfigs));
        if (!g_EGLFuncs.eglChooseConfig(g_Display, configAttribs, configs.data(), numConfigs, &numConfigs) ||
            numConfigs == 0) {
            return false;
        }
        configs.resize(static_cast<SizeT>(numConfigs));

        if (surfaceBit == EGL_WINDOW_BIT) {
            // X11 drivers can reserve every alpha-8 config for 32-bit ARGB
            // visuals (NVIDIA), so a default-visual (depth 24) window only
            // matches an alpha-0 config; keep those as a second candidate tier
            // for the visual match or eglCreateWindowSurface hits BAD_CONFIG.
            Vector<EGLConfig> alphaFreeConfigs;
            const EGLint alphaFreeAttribs[] = {EGL_SURFACE_TYPE,
                                               surfaceBit,
                                               EGL_RENDERABLE_TYPE,
                                               EGL_OPENGL_ES3_BIT,
                                               EGL_RED_SIZE,
                                               8,
                                               EGL_GREEN_SIZE,
                                               8,
                                               EGL_BLUE_SIZE,
                                               8,
                                               EGL_DEPTH_SIZE,
                                               24,
                                               EGL_STENCIL_SIZE,
                                               8,
                                               EGL_NONE};
            EGLint numAlphaFree = 0;
            if (g_EGLFuncs.eglChooseConfig(g_Display, alphaFreeAttribs, nullptr, 0, &numAlphaFree) &&
                numAlphaFree > 0) {
                alphaFreeConfigs.resize(static_cast<SizeT>(numAlphaFree));
                if (!g_EGLFuncs.eglChooseConfig(g_Display, alphaFreeAttribs, alphaFreeConfigs.data(), numAlphaFree,
                                                &numAlphaFree)) {
                    numAlphaFree = 0;
                }
                alphaFreeConfigs.resize(static_cast<SizeT>(numAlphaFree));
            }

            const EGLint windowVisualId = QueryX11WindowVisualId(window);
            const EGLint visualIds[] = {windowVisualId, QueryDefaultX11VisualId()};
            for (const auto visualId : visualIds) {
                if (visualId == 0) {
                    continue;
                }
                for (const auto* candidates : {&configs, &alphaFreeConfigs}) {
                    for (const auto config : *candidates) {
                        EGLint nativeVisualId = 0;
                        if (ConfigSupports(config, surfaceBit) &&
                            GetConfigAttrib(config, EGL_NATIVE_VISUAL_ID, nativeVisualId) &&
                            nativeVisualId == visualId) {
                            outConfig = config;
                            return true;
                        }
                    }
                }
            }
        }

        for (const auto config : configs) {
            if (ConfigSupports(config, surfaceBit)) {
                outConfig = config;
                return true;
            }
        }

        outConfig = configs.front();
        return true;
    }

    // P14 S4. THE CONTEXT FOR `tuple`, AND THE SURFACE'S CONFIG, WITH NO TEARDOWN IN FRONT.
    //
    // The S4 deletion is the first statement this function used to begin with: `DestroyEGLContext()`
    // ran UNCONDITIONALLY, so creating a second surface (let alone a second context) destroyed the
    // native context the first one's objects lived in. A tuple that already has a native context
    // keeps it and only (re)chooses the config the new surface is created with; a tuple that has
    // none builds one, sharing with the group leader when CreateNativeContextFor named a group.
    //
    // A CONFIG THAT IS ALREADY SET IS NOT RE-CHOSEN FOR A DIFFERENT SURFACE KIND, and that is the
    // deliberate trade: an EGLSurface's config must be compatible with the context's, so re-choosing
    // would mean recreating the context - exactly the teardown this slice deletes. A driver that
    // refuses the resulting eglCreate*Surface refuses it by name, which is the honest failure.
    // P14 S4. THE BRING-UP DIAGNOSIS, in one place because BOTH surface paths owe it (it used to be
    // the first statement of InitDisplayAndContext, which both went through). Extracted rather than
    // duplicated when that function was replaced by the per-tuple EnsureNativeContext.
    //
    // Diagnose, and deliberately NOT resolve: resolving raises
    // Fatal{PipeLegacyMemosDisabled}, and this function runs inside eglMakeCurrent, which
    // the integration harness pre-flights in a FORKED CHILD
    // (MG_IntegrationTest/Harness/HeadlessGL.cpp). A child that dies on a signal is reported
    // to the parent as "no usable GPU/display/ICD" and every scenario in the lane is
    // SKIPPED - so the stop became a green lane that ran nothing, on exactly the two env
    // vars the D14/D18 A/B is driven with (ARCHITECTURE.md §13.2). The stop now belongs to the first
    // twin lookup, which happens in a scenario body where a crash IS a test failure.
    static void DiagnoseEsprytSlotArmAtBringUp() {
        DiagnoseEsprytSlotArm();
    }

    // RESET NOTIFICATION, decided once per process by its first native context (EnsureNativeContext
    // says why). MOBILEGL_ESPRYT_RESET_NOTIFICATION=0 keeps the driver's default.

    namespace {
        // EGL_EXT_create_context_robustness (eglext.h is not part of this file's include set).
        constexpr EGLint kEglResetNotificationStrategyExt = 0x3138;
        constexpr EGLint kEglLoseContextOnResetExt = 0x31BF;
        enum class ResetNotification : Uint8 {
            Undecided,
            On,
            Off
        };
        std::atomic<ResetNotification> g_resetNotification{ResetNotification::Undecided};

        Bool ResetNotificationWanted() {
            const ResetNotification decided = g_resetNotification.load(std::memory_order_acquire);
            if (decided != ResetNotification::Undecided) return decided == ResetNotification::On;
            if (const char* value = std::getenv("MOBILEGL_ESPRYT_RESET_NOTIFICATION");
                value != nullptr && value[0] == '0')
                return false;
            if (g_Display == EGL_NO_DISPLAY || !g_EGLFuncs.eglQueryString) return false;
            const char* extensions = g_EGLFuncs.eglQueryString(g_Display, EGL_EXTENSIONS);
            if (extensions == nullptr) return false;
            constexpr const char kWord[] = "EGL_EXT_create_context_robustness";
            for (const char* at = std::strstr(extensions, kWord); at != nullptr; at = std::strstr(at + 1, kWord)) {
                const char after = at[sizeof(kWord) - 1];
                if ((at == extensions || at[-1] == ' ') && (after == '\0' || after == ' ')) return true;
            }
            return false;
        }

        void NoteResetNotificationRefused() {
            ResetNotification expected = ResetNotification::Undecided;
            if (g_resetNotification.compare_exchange_strong(expected, ResetNotification::Off,
                                                            std::memory_order_acq_rel))
                MGLOG_W("DirectGLES: the driver refused EGL_LOSE_CONTEXT_ON_RESET; a context that hangs the GPU is "
                        "not told, and its session cannot be ended for it");
        }

        void NoteResetNotificationDecided(Bool used) {
            ResetNotification expected = ResetNotification::Undecided;
            const ResetNotification decided = used ? ResetNotification::On : ResetNotification::Off;
            if (g_resetNotification.compare_exchange_strong(expected, decided, std::memory_order_acq_rel))
                MGLOG_I("DirectGLES: native contexts %s reset notification (EGL_LOSE_CONTEXT_ON_RESET): a context "
                        "whose work hangs or faults the GPU %s",
                        used ? "ask for" : "do not ask for",
                        used ? "is named by the driver and ends its own session" : "is not told");
        }
    } // namespace

    Bool NativeContextsNotifyResets() {
        return g_resetNotification.load(std::memory_order_acquire) == ResetNotification::On;
    }

    static Bool EnsureNativeContext(NativeContextTuple& tuple, EGLint surfaceBit, NativeWindowType window) {
        if (!EnsureDisplayInitialized()) {
            MGLOG_E("EnsureNativeContext: the EGL display did not initialize");
            return false;
        }
        if (tuple.Context != EGL_NO_CONTEXT) return true;

        if (tuple.Config == nullptr && !ChooseConfigForSurface(surfaceBit, tuple.Config, window)) {
            MGLOG_E("EnsureNativeContext: no EGL config for surface bit 0x%x (window=%p)",
                    static_cast<unsigned>(surfaceBit), reinterpret_cast<void*>(window));
            return false;
        }

        // A share group whose leader has a live native context hands it to eglCreateContext: this is
        // the whole of "shared contexts see the same objects" on the server. The group is recorded
        // by CreateNativeContextFor before this runs; a tuple nobody named a group for starts one.
        EGLContext share = EGL_NO_CONTEXT;
        for (auto* other : NativeContexts()) {
            if (other != &tuple && other->SessionKey == tuple.SessionKey && other->Context != EGL_NO_CONTEXT &&
                tuple.ShareGroupToken != 0 && other->ShareGroupToken == tuple.ShareGroupToken) {
                share = other->Context;
                break;
            }
        }
        // P14: AND OTHERWISE ANY LIVE NATIVE CONTEXT IN THE PROCESS. The backend keeps buffers,
        // textures and syncs of its own in process-wide state (the UBO/unpack/upload rings, the
        // buffer pool, scratch textures, frame fences) and every session's apply thread uses
        // them; they are only names in another session's context if all the contexts are one
        // share group. Client isolation does not come from the driver's namespaces - the twin
        // tables are bucketed per {session, share group} and never hand one bucket's names to
        // another - so a process-wide group costs nothing in separation.
        if (share == EGL_NO_CONTEXT) {
            for (auto* other : NativeContexts()) {
                if (other != &tuple && other->Context != EGL_NO_CONTEXT) {
                    share = other->Context;
                    break;
                }
            }
        }

        // Negotiate the highest ES 3.x context. Version-strict EGL implementations
        // (ANGLE) return exactly the requested minor, and a bare CLIENT_VERSION 3
        // request yields a 3.0 context that lacks the 3.1/3.2 texture targets the
        // capability probes exercise; mobile drivers ignore the minor and hand out
        // their maximum either way.
        //
        // RESET NOTIFICATION. With the driver's default strategy a context whose work hung the GPU
        // is never told: after Adreno's ~2 s timeout reset it silently drops everything that context
        // submits (the client's window just stops), and its client never learns it should recreate.
        // Asking for EGL_LOSE_CONTEXT_ON_RESET lets the driver name the guilty context
        // (glGetGraphicsResetStatus answers GUILTY, once) and LatchIfDeviceLost ends that session
        // alone, so the client reads a lost context and can recover. (A ~10 s reset that a failed
        // preemption ends was not reported to the server's contexts in measurement - see
        // GpuHangWatch.h.) Every context of the process shares one group, and contexts that share
        // must agree on the strategy, so it is decided once, by the first context, and kept.
        for (const EGLint minorVersion : {2, 1, 0}) {
            const Bool notify = ResetNotificationWanted();
            const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION,
                                             3,
                                             EGL_CONTEXT_MINOR_VERSION,
                                             minorVersion,
                                             notify ? kEglResetNotificationStrategyExt : EGL_NONE,
                                             kEglLoseContextOnResetExt,
                                             EGL_NONE};
            tuple.Context = g_EGLFuncs.eglCreateContext(g_Display, tuple.Config, share, contextAttribs);
            if (tuple.Context == EGL_NO_CONTEXT && notify && share == EGL_NO_CONTEXT) {
                // The process's first context: if the same version WITHOUT the strategy is accepted,
                // it was the strategy the driver refused, and no context of the process asks for it.
                const EGLint plainAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, minorVersion,
                                               EGL_NONE};
                tuple.Context = g_EGLFuncs.eglCreateContext(g_Display, tuple.Config, share, plainAttribs);
                if (tuple.Context != EGL_NO_CONTEXT) NoteResetNotificationRefused();
            }
            if (tuple.Context != EGL_NO_CONTEXT) {
                NoteResetNotificationDecided(notify);
                tuple.Serial = g_nativeContextSerials.fetch_add(1, std::memory_order_relaxed) + 1;
                ++g_displayUsers;
                return true;
            }
        }

        const Bool notify = ResetNotificationWanted();
        const EGLint legacyContextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                                               notify ? kEglResetNotificationStrategyExt : EGL_NONE,
                                               kEglLoseContextOnResetExt, EGL_NONE};
        tuple.Context = g_EGLFuncs.eglCreateContext(g_Display, tuple.Config, share, legacyContextAttribs);
        if (tuple.Context == EGL_NO_CONTEXT) return false;
        NoteResetNotificationDecided(notify);
        tuple.Serial = g_nativeContextSerials.fetch_add(1, std::memory_order_relaxed) + 1;
        ++g_displayUsers;
        return true;
    }

    Bool CreateNativeContextFor(Uint64 contextToken, Uint64 shareGroupToken) {
        if (contextToken == 0) return false;
        const Uint64 sessionKey = CurrentNativeSessionKey();
        NativeContextTuple* tuple = nullptr;
        {
            const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
            AcquireNativeSession(sessionKey);
            for (auto* candidate : NativeContexts()) {
                if (candidate->SessionKey == sessionKey && candidate->ContextToken == contextToken) {
                    tuple = candidate;
                    break;
                }
            }
            if (tuple == nullptr) {
                tuple = new NativeContextTuple{};
                tuple->SessionKey = sessionKey;
                tuple->ContextToken = contextToken;
                NativeContexts().push_back(tuple);
                g_nativeRegistryEpoch.fetch_add(1, std::memory_order_acq_rel);
            }
        }
        if (tuple->Context != EGL_NO_CONTEXT) return true; // idempotent
        tuple->ShareGroupToken = shareGroupToken;
        return EnsureNativeContext(*tuple, EGL_PBUFFER_BIT, static_cast<NativeWindowType>(0));
    }

    void DestroyNativeContextFor(Uint64 contextToken) {
        const Uint64 sessionKey = CurrentNativeSessionKey();
        if (contextToken == 0) return;
        const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
        NativeContextTuple* tuple = FindNativeContext(sessionKey, contextToken);
        if (tuple == nullptr || tuple->Context == EGL_NO_CONTEXT) return;
#if MOBILEGL_BUILD_DISAGGREGATED
        // Its last shared-image reads, fenced while it is still current.
        if (t_native.boundContext == tuple->Context) SharedImageImpl::PublishPendingReads();
#endif
        if (g_EGLFuncs.eglDestroyContext && g_Display != EGL_NO_DISPLAY) {
            // The context may be current on this very thread; unbinding first is what EGL requires
            // (eglDestroyContext of a current context is EGL_BAD_ACCESS).
            if (g_EGLFuncs.eglGetCurrentContext && g_EGLFuncs.eglGetCurrentContext() == tuple->Context &&
                g_EGLFuncs.eglMakeCurrent) {
                g_EGLFuncs.eglMakeCurrent(g_Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
            g_EGLFuncs.eglDestroyContext(g_Display, tuple->Context);
        }
        // This thread no longer has it current either way; a context created later at the same
        // address must not read as "already bound".
        if (t_native.boundContext == tuple->Context) {
            t_native.boundContext = EGL_NO_CONTEXT;
            t_native.boundDraw = EGL_NO_SURFACE;
            t_nativeContextThread.BoundSerial = 0;
        }
        ForgetNativeContextObjects(tuple->Serial);
        tuple->Serial = 0;
        tuple->Context = EGL_NO_CONTEXT;
        tuple->Config = nullptr;
        tuple->Draw = EGL_NO_SURFACE;
        tuple->Read = EGL_NO_SURFACE;
        g_nativeRegistryEpoch.fetch_add(1, std::memory_order_acq_rel);
        if (g_displayUsers > 0) --g_displayUsers;
        // NOT WHILE A SESSION STILL HOLDS SURFACES. eglTerminate destroys them with the display, and
        // the session keeps naming them: the next context made current on one (an application that
        // destroys its only context and creates another on the same surface, as eglinfo does with
        // its surfaceless stand-in) bound a surface the driver no longer had. The session's own
        // teardown terminates the display once its surfaces are gone (DestroyNativeStateOfSession).
        Bool surfacesRemain = false;
        for (const auto* session : NativeSessions())
            surfacesRemain = surfacesRemain || !session->Surfaces.empty();
        if (g_displayUsers == 0 && !surfacesRemain && g_Display != EGL_NO_DISPLAY && g_EGLFuncs.eglTerminate) {
            g_EGLFuncs.eglTerminate(g_Display);
            NoteDisplayTerminated();
            g_Display = EGL_NO_DISPLAY;
        }
    }

    // P14 S4. THE NATIVE HALF OF A CONTEXT SWITCH. See DirectGLES.h; in one line, `bind_context`
    // moves the session's context attribution and this makes the calling thread's native tuple
    // follow it, binding the session's current surface to the newly named context's EGLContext.
    static Bool SessionOwnsSurface(const NativeSessionState& session, EGLSurface surface) {
        if (surface == EGL_NO_SURFACE) return false;
        for (const auto& entry : session.Surfaces) {
            if (entry.Surface == surface) return true;
        }
        return false;
    }

    Bool MakeNativeContextCurrentForBoundToken() {
        NativeContextTuple& tuple = ActiveNativeContext();
        // Nothing to switch to: a session that never sent CreateContext has no native context for
        // this token, and building one here would bind a context the client does not know it has.
        if (tuple.Context == EGL_NO_CONTEXT) return true;
        // A MULTI-WINDOW CLIENT: each context draws to the surface it was last made current with,
        // not to whichever surface the session bound most recently - Qt renders each window on its
        // own thread with its own context, and binding a context back onto another window's
        // surface drew one window's frame into the other.
        NativeSessionState& session = ActiveNativeSession();
        // Nothing to make it current on yet: a context bound for the records of its own creation
        // (EGLImpl's CreateContext), which no make-current has given a surface (or whose surface
        // is gone). Binding it to whichever surface the session holds would hand it another
        // context's surface - one the driver may not even have any more. Its first make-current
        // binds it natively.
        if (tuple.Draw == EGL_NO_SURFACE) return true;
        if (tuple.Draw != session.Draw && SessionOwnsSurface(session, tuple.Draw)) {
            session.Draw = tuple.Draw;
            NoteDrawSurfaceMoved();
            session.Read = SessionOwnsSurface(session, tuple.Read) ? tuple.Read : tuple.Draw;
        }
        if (t_native.boundContext == tuple.Context && t_native.boundDraw == session.Draw) return true;
        return MakeCurrent();
    }

    EGLSurface CurrentSessionDrawSurface() {
        return ActiveNativeSession().Draw;
    }

    EGLSurface CurrentContextDrawSurface() {
        return ActiveNativeContext().Draw;
    }

    namespace {
        // Whether the driver of `display` has EGL_EXT_buffer_age (or EGL_KHR_partial_update, which
        // defines the same query). Asked once per display.
        // EGL_BUFFER_AGE_EXT (= EGL_BUFFER_AGE_KHR), spelled out: this file sees only the core EGL header.
        constexpr EGLint kEglBufferAge = 0x313D;

        // How the driver answers a buffer age, if at all. EGL_EXT_buffer_age promises the whole
        // buffer's content for the age it reports. EGL_KHR_partial_update alone promises it only
        // OUTSIDE the frame's damage region, which is the whole surface until eglSetDamageRegionKHR
        // narrows it - a frame that asks the age and then draws part of the surface without setting
        // the region finds the rest undefined (on a tiler: tiles never loaded, noise on screen).
        struct NativeBufferAge {
            Bool Ext = false;     // EGL_EXT_buffer_age
            Bool Partial = false; // EGL_KHR_partial_update
            EGLBoolean (*SetDamageRegion)(EGLDisplay, EGLSurface, EGLint*, EGLint) = nullptr;
        };
        const NativeBufferAge& NativeBufferAgeSupport() {
            static EGLDisplay asked = EGL_NO_DISPLAY;
            static NativeBufferAge support;
            if (g_Display == EGL_NO_DISPLAY || !g_EGLFuncs.eglQueryString) {
                static const NativeBufferAge none;
                return none;
            }
            if (asked != g_Display) {
                asked = g_Display;
                const char* extensions = g_EGLFuncs.eglQueryString(g_Display, EGL_EXTENSIONS);
                support = {};
                support.Ext = extensions != nullptr && std::strstr(extensions, "EGL_EXT_buffer_age") != nullptr;
                support.Partial = extensions != nullptr && std::strstr(extensions, "EGL_KHR_partial_update") != nullptr;
                if (support.Partial && g_EGLFuncs.eglGetProcAddress) {
                    support.SetDamageRegion = reinterpret_cast<decltype(support.SetDamageRegion)>(
                        g_EGLFuncs.eglGetProcAddress("eglSetDamageRegionKHR"));
                }
                if (support.SetDamageRegion == nullptr) support.Partial = false;
                MGLOG_I("DirectGLES: native buffer age: EGL_EXT_buffer_age %s, EGL_KHR_partial_update %s",
                        support.Ext ? "yes" : "no", support.Partial ? "yes" : "no");
            }
            return support;
        }
    } // namespace

    // The window surface whose age the driver was asked in the frame being drawn on this (the
    // session's apply) thread; the present ends the frame.
    thread_local EGLSurface t_ageAskedSurface = EGL_NO_SURFACE;

    // Whether the frame being drawn on this thread has had its flush ahead of the window
    // buffer (FlushAheadOfWindowBufferUse); the present ends the frame.
    thread_local Bool t_windowBufferFlushIssued = false;

    namespace {
        // A WINDOW SURFACE'S BUFFER AGE, on unless MOBILEGL_ESPRYT_WINDOW_AGE=0 (or, on Android, the
        // property debug.mobilegl.espryt_window_age=0); off, a window answers 0 and its client
        // repaints whole frames. It was opt-in while a window repainted by age sometimes showed a
        // client frame two or three presents old: the client's shared image had kept its previous
        // frame, its copy clipped by a scissor guard that trusted another session's render-state
        // shadow (ScopedScissorDisable) - whole repaints only happened to leave that shadow
        // agreeing with the client. Pbuffers (client windows) are not affected.
        Bool WindowBufferAgeEnabled() {
            static const Bool enabled = [] {
                if (const char* env = std::getenv("MOBILEGL_ESPRYT_WINDOW_AGE")) return std::strcmp(env, "0") != 0;
#if defined(__ANDROID__)
                char property[PROP_VALUE_MAX] = {};
                if (__system_property_get("debug.mobilegl.espryt_window_age", property) > 0)
                    return std::strcmp(property, "0") != 0;
#endif
                return true;
            }();
            return enabled;
        }
    } // namespace

    // THE OFF-SCREEN PART OF A FRAME DOES NOT WAIT FOR THE WINDOW BUFFER. A window surface takes
    // its next buffer when the frame first uses the default framebuffer, and about every other
    // frame that is the buffer the display has only just released: its release fence is still
    // pending. Whatever the driver has not yet submitted then goes out at the swap, in one batch
    // behind that fence - the frame's off-screen passes included - so the GPU starts the whole
    // frame only after the display's next commit. A flush just before the first use lets the
    // off-screen work start now; only the window pass waits. Once per frame, on window surfaces
    // only (a pbuffer has no buffer to wait for), and a flush is not a wait.
    void FlushAheadOfWindowBufferUse() {
        if (t_windowBufferFlushIssued) return;
        t_windowBufferFlushIssued = true;
        NativeSessionState& session = ActiveNativeSession();
        const EGLSurface surface = session.Draw;
        if (surface == EGL_NO_SURFACE) return;
        {
            const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
            const auto* entry = FindSessionSurfaceLocked(session, surface);
            if (entry == nullptr || entry->Window == static_cast<NativeWindowType>(0)) return;
        }
        if (g_GLESFuncs.glFlush) g_GLESFuncs.glFlush();
    }

    Int32 CurrentDrawBufferAge(Bool damageRegionFollows) {
        NativeSessionState& session = ActiveNativeSession();
        const EGLSurface surface = session.Draw;
        if (surface == EGL_NO_SURFACE || g_Display == EGL_NO_DISPLAY) return 0;
        Bool window = false;
        Uint64 presents = 0;
        {
            const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
            const auto* entry = FindSessionSurfaceLocked(session, surface);
            if (entry == nullptr) return 0;
            window = entry->Window != static_cast<NativeWindowType>(0);
            presents = entry->Presents;
        }
        if (!window) return presents > 0 ? 1 : 0;
        if (!WindowBufferAgeEnabled()) return 0;
        // The driver's answer. On Android the query dequeues the buffer the next frame draws into -
        // what the frame's first draw would do anyway, and the client asks before it draws.
        const NativeBufferAge& support = NativeBufferAgeSupport();
        const Bool askable = MG_Util::Damage::NativeAgeAskable(support.Ext, support.Partial, damageRegionFollows);
        if (presents == 0 || !askable || !g_EGLFuncs.eglQuerySurface) return 0;
        EGLint age = 0;
        if (!g_EGLFuncs.eglQuerySurface(g_Display, surface, kEglBufferAge, &age) || age < 0) return 0;
        t_ageAskedSurface = surface;
        return age;
    }

    Bool SetCurrentDrawDamageRegion(const MG_Util::Damage::Region& region) {
        NativeSessionState& session = ActiveNativeSession();
        const EGLSurface surface = session.Draw;
        if (surface == EGL_NO_SURFACE || g_Display == EGL_NO_DISPLAY) return true;
        {
            const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
            const auto* entry = FindSessionSurfaceLocked(session, surface);
            if (entry == nullptr || entry->Window == static_cast<NativeWindowType>(0)) return true;
        }
        const NativeBufferAge& support = NativeBufferAgeSupport();
        if (!support.Partial || !WindowBufferAgeEnabled()) return true;
        // EGL_KHR_partial_update wants the age asked in this frame before the region is set; a client
        // that was answered 0 without the driver being asked (its first frame) repaints everything.
        if (t_ageAskedSurface != surface) {
            EGLint ignored = 0;
            (void)g_EGLFuncs.eglQuerySurface(g_Display, surface, kEglBufferAge, &ignored);
            t_ageAskedSurface = surface;
        }
        EGLint rects[MG_Util::Damage::kMaxRects * 4] = {};
        const EGLint count =
            region.IsFull()
                ? 0
                : static_cast<EGLint>(MG_Util::Damage::PackRects(region, rects, MG_Util::Damage::kMaxRects));
        if (support.SetDamageRegion(g_Display, surface, count > 0 ? rects : nullptr, count) != EGL_TRUE) {
            MGLOG_W_ONCE("DirectGLES: eglSetDamageRegionKHR refused a %d-rectangle region (EGL error 0x%x)", count,
                         g_EGLFuncs.eglGetError ? static_cast<unsigned>(g_EGLFuncs.eglGetError()) : 0u);
        }
        return true;
    }

    Bool SelectSessionSurface(EGLSurface surface) {
        NativeSessionState& session = ActiveNativeSession();
        if (!SessionOwnsSurface(session, surface)) return false;
        session.Draw = surface;
        NoteDrawSurfaceMoved();
        session.Read = surface;
        return true;
    }

    Bool BindSessionSurface(EGLSurface surface) {
        NativeSessionState& session = ActiveNativeSession();
        if (!SessionOwnsSurface(session, surface)) return false;
        session.Draw = surface;
        NoteDrawSurfaceMoved();
        session.Read = surface;
        if (ActiveNativeContext().Context == EGL_NO_CONTEXT) return true;
        if (t_native.boundContext == ActiveNativeContext().Context && t_native.boundDraw == surface) return true;
        return MakeCurrent();
    }

    EGLSurface CreateSessionPbuffer(EGLint width, EGLint height) {
        NativeContextTuple& tuple = ActiveNativeContext();
        if (!EnsureNativeContext(tuple, EGL_PBUFFER_BIT, static_cast<NativeWindowType>(0))) return EGL_NO_SURFACE;
        const EGLint attribs[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
        const EGLSurface surface = g_EGLFuncs.eglCreatePbufferSurface(g_Display, tuple.Config, attribs);
        if (surface == EGL_NO_SURFACE) return EGL_NO_SURFACE;
        NativeSessionState& session = ActiveNativeSession();
        const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
        session.Surfaces.push_back({static_cast<NativeWindowType>(0), surface});
        g_nativeRegistryEpoch.fetch_add(1, std::memory_order_acq_rel);
        return surface;
    }

    void ReplaceSessionSurface(EGLSurface previous, EGLSurface replacement) {
        if (previous == EGL_NO_SURFACE || replacement == EGL_NO_SURFACE || previous == replacement) return;
        NativeSessionState& session = ActiveNativeSession();
        {
            const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
            for (auto* tuple : NativeContexts()) {
                if (tuple->SessionKey != session.SessionKey) continue;
                if (tuple->Draw == previous) tuple->Draw = replacement;
                if (tuple->Read == previous) tuple->Read = replacement;
            }
            auto& entries = session.Surfaces;
            entries.erase(std::remove_if(
                              entries.begin(), entries.end(),
                              [&](const NativeSessionState::SurfaceEntry& entry) { return entry.Surface == previous; }),
                          entries.end());
            g_nativeRegistryEpoch.fetch_add(1, std::memory_order_acq_rel);
        }
        if (session.Draw == previous) {
            session.Draw = replacement;
            NoteDrawSurfaceMoved();
        }
        if (session.Read == previous) session.Read = replacement;
        // The calling thread drew to the old one: draw to the new one from here (the old is then
        // no longer current, and its destruction below is immediate).
        if (t_native.boundDraw == previous && ActiveNativeContext().Context != EGL_NO_CONTEXT) (void)MakeCurrent();
        if (g_EGLFuncs.eglDestroySurface && g_Display != EGL_NO_DISPLAY)
            g_EGLFuncs.eglDestroySurface(g_Display, previous);
    }

    void DestroySessionSurface(EGLSurface surface) {
        if (surface == EGL_NO_SURFACE) return;
        NativeSessionState& session = ActiveNativeSession();
        {
            const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
            auto& entries = session.Surfaces;
            const auto it =
                std::find_if(entries.begin(), entries.end(),
                             [&](const NativeSessionState::SurfaceEntry& entry) { return entry.Surface == surface; });
            if (it == entries.end()) return;
            entries.erase(it);
            for (auto* tuple : NativeContexts()) {
                if (tuple->SessionKey != session.SessionKey) continue;
                if (tuple->Draw == surface) tuple->Draw = EGL_NO_SURFACE;
                if (tuple->Read == surface) tuple->Read = EGL_NO_SURFACE;
            }
            g_nativeRegistryEpoch.fetch_add(1, std::memory_order_acq_rel);
        }
        if (session.Draw == surface || session.Read == surface) {
            // Fall back onto a surface the session still has, so the next bind has somewhere to go.
            const EGLSurface fallback = session.Surfaces.empty() ? EGL_NO_SURFACE : session.Surfaces.front().Surface;
            session.Draw = fallback;
            NoteDrawSurfaceMoved();
            session.Read = fallback;
            if (t_native.boundDraw == surface && fallback != EGL_NO_SURFACE &&
                ActiveNativeContext().Context != EGL_NO_CONTEXT) {
                (void)MakeCurrent();
            }
        }
        // A surface still current on this thread is destroyed by EGL once it is not (EGL 1.5
        // 3.5.6), so this needs no unbind first.
        if (g_EGLFuncs.eglDestroySurface && g_Display != EGL_NO_DISPLAY)
            g_EGLFuncs.eglDestroySurface(g_Display, surface);
        if (t_native.boundDraw == surface && session.Draw == EGL_NO_SURFACE) t_native.boundDraw = EGL_NO_SURFACE;
    }

    namespace {
        // Last swap interval the app requested through eglSwapInterval; -1 = never
        // requested (keep the EGL default of 1). Re-applied when the window surface
        // is (re)created since interval is per-surface state.
        Int g_requestedSwapInterval = -1;

        void ApplyRequestedSwapInterval() {
            if (g_requestedSwapInterval < 0) return;
            const EGLSurface surface = ActiveNativeSession().Draw;
            if (!g_EGLFuncs.eglSwapInterval || g_Display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE) return;
            const EGLBoolean ok = g_EGLFuncs.eglSwapInterval(g_Display, g_requestedSwapInterval);
            MGLOG_D("DirectGLES: applied native swap interval %d (%s)", g_requestedSwapInterval, ok ? "ok" : "failed");
        }
    } // namespace

    void SetSwapInterval(Int interval) {
        g_requestedSwapInterval = interval;
        ApplyRequestedSwapInterval();
    }

#if MOBILEGL_BUILD_DISAGGREGATED
    void ForgetRequestedSwapInterval() {
        g_requestedSwapInterval = -1;
    }
#endif

    // A crashed predecessor session may have left its surface bound to this window (the
    // server-owned display window is one ANativeWindow for the process's lifetime, and
    // several drivers refuse a second eglCreateWindowSurface on it with EGL_BAD_NATIVE_WINDOW).
    // Teardown is keyed on the session the surface belongs to, so a session that died without
    // one leaks it; find and destroy those before asking the driver.
    static void DestroyStaleSurfacesOnWindow(NativeWindowType window) {
        const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
        for (auto* session : NativeSessions()) {
            auto& entries = session->Surfaces;
            for (auto it = entries.begin(); it != entries.end();) {
                if (it->Window == window) {
                    if (g_EGLFuncs.eglDestroySurface) g_EGLFuncs.eglDestroySurface(g_Display, it->Surface);
                    if (session->Draw == it->Surface) session->Draw = EGL_NO_SURFACE;
                    if (session->Read == it->Surface) session->Read = EGL_NO_SURFACE;
                    it = entries.erase(it);
                } else {
                    ++it;
                }
            }
        }
        g_nativeRegistryEpoch.fetch_add(1, std::memory_order_acq_rel);
    }

    // The default framebuffer's shape after a window surface was bound - with its extent when the
    // window is the server's own (see the push arm of the publish).
    static void PublishWindowSurfaceShape(NativeWindowType window) {
#if MOBILEGL_BUILD_DISAGGREGATED
        // P12 (D3): this publish may carry the window's extent (see the push arm of the publish) -
        // review fix: only when the window is the SERVER's own (a headless client's ServerOwned
        // surface). A window the client named keeps the format-only publish without the knob.
        g_publishWindowExtent = MG_Pipe::MGPipeServerOwnedWindow() != nullptr &&
                                reinterpret_cast<const void*>(window) == MG_Pipe::MGPipeServerOwnedWindow();
        PublishDefaultFramebufferDepthStencilFormat();
        g_publishWindowExtent = false;
#else
        (void)window;
        PublishDefaultFramebufferDepthStencilFormat();
#endif
    }

    // THE SERVER WINDOW CHANGED SIZE (the app's layout moved the view: a bar shown or hidden, a
    // rotation). The new extent - the display's, set by the server loop before this runs - is published
    // as the creation publishes it, at once: the client sizes its viewport and default framebuffer from
    // it, and a compositor with nothing to redraw makes no present to hang this on.
    Bool RepublishWindowSurfaceShape(EGLSurface surface, NativeWindowType window) {
        if (surface == EGL_NO_SURFACE || !window) return false;
        // Bound, because the publish reads the default framebuffer's format.
        if (!BindSessionSurface(surface) || t_native.boundDraw != surface) return false;
        PublishWindowSurfaceShape(window);
        return true;
    }

    EGLSurface SuspendSessionWindowSurface(EGLSurface windowSurface) {
        if (windowSurface == EGL_NO_SURFACE || g_Display == EGL_NO_DISPLAY) return EGL_NO_SURFACE;
        if (!SessionOwnsSurface(ActiveNativeSession(), windowSurface)) return EGL_NO_SURFACE;
        // 1x1: nothing is shown while the window is gone, and nothing is published for it - the
        // client keeps the extent it has until the resume publishes the new window's.
        const EGLSurface placeholder = CreateSessionPbuffer(1, 1);
        if (placeholder == EGL_NO_SURFACE) {
            const EGLint error = g_EGLFuncs.eglGetError ? g_EGLFuncs.eglGetError() : EGL_SUCCESS;
            MGLOG_E("DirectGLES: no placeholder pbuffer could stand in for the window surface %p (EGL error 0x%x)",
                    reinterpret_cast<void*>(windowSurface), static_cast<unsigned>(error));
            return EGL_NO_SURFACE;
        }
        ReplaceSessionSurface(windowSurface, placeholder);
        // The rebind onto the placeholder failed and the window surface is still current here - EGL
        // destroys a current surface only once it is not, and the window goes when this returns.
        if (t_native.boundDraw == windowSurface) (void)ReleaseCurrent();
        return placeholder;
    }

    EGLSurface ResumeSessionWindowSurface(EGLSurface placeholder, NativeWindowType window) {
        if (!window || g_Display == EGL_NO_DISPLAY) return EGL_NO_SURFACE;
        NativeContextTuple& tuple = ActiveNativeContext();
        if (!EnsureNativeContext(tuple, EGL_WINDOW_BIT, window)) return EGL_NO_SURFACE;
        DestroyStaleSurfacesOnWindow(window);
        const EGLSurface surface = g_EGLFuncs.eglCreateWindowSurface(g_Display, tuple.Config, window, nullptr);
        if (surface == EGL_NO_SURFACE) {
            const EGLint error = g_EGLFuncs.eglGetError ? g_EGLFuncs.eglGetError() : EGL_SUCCESS;
            MGLOG_E("DirectGLES: eglCreateWindowSurface(window=%p) failed with EGL error 0x%x on resume",
                    reinterpret_cast<void*>(window), static_cast<unsigned>(error));
            return EGL_NO_SURFACE;
        }
        NativeSessionState& session = ActiveNativeSession();
        {
            const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
            session.Surfaces.push_back({window, surface});
            g_nativeRegistryEpoch.fetch_add(1, std::memory_order_acq_rel);
        }
        if (placeholder != EGL_NO_SURFACE && SessionOwnsSurface(session, placeholder)) {
            ReplaceSessionSurface(placeholder, surface);
        }
        // Bound, because the publish below reads the default framebuffer.
        if (!BindSessionSurface(surface) || t_native.boundDraw != surface) {
            MGLOG_E("DirectGLES: the resumed window surface %p could not be made current; its extent is published "
                    "at its next bind",
                    reinterpret_cast<void*>(surface));
            return surface;
        }
        ApplyRequestedSwapInterval();
        PublishWindowSurfaceShape(window);
        return surface;
    }

    Bool InitWindowSurface(NativeWindowType window) {
        if (!window) return false;

        DiagnoseEsprytSlotArmAtBringUp();

        NativeContextTuple& tuple = ActiveNativeContext();
        if (!EnsureNativeContext(tuple, EGL_WINDOW_BIT, window)) return false;

        DestroyStaleSurfacesOnWindow(window);

        const EGLSurface surface = g_EGLFuncs.eglCreateWindowSurface(g_Display, tuple.Config, window, nullptr);
        if (surface == EGL_NO_SURFACE) {
            const EGLint error = g_EGLFuncs.eglGetError ? g_EGLFuncs.eglGetError() : EGL_SUCCESS;
            MGLOG_E("InitWindowSurface: eglCreateWindowSurface(window=%p) failed with EGL error 0x%x",
                    reinterpret_cast<void*>(window), static_cast<unsigned>(error));
            return false;
        }
        NativeSessionState& session = ActiveNativeSession();
        session.Surfaces.push_back({window, surface});
        session.Draw = surface;
        NoteDrawSurfaceMoved();
        session.Read = surface;
        tuple.Draw = surface;
        tuple.Read = surface;

        if (!MakeCurrent()) return false;

        ApplyRequestedSwapInterval();
        PublishWindowSurfaceShape(window);

        MGLOG_D("EGL window surface created successfully: display=%p, surface=%p, context=%p. window=%p", g_Display,
                surface, tuple.Context, window);
        return true;
    }

    Bool InitPbufferSurface(EGLint width, EGLint height) {
        if (width <= 0 || height <= 0) return false;

        DiagnoseEsprytSlotArmAtBringUp();

        NativeContextTuple& tuple = ActiveNativeContext();
        if (!EnsureNativeContext(tuple, EGL_PBUFFER_BIT, static_cast<NativeWindowType>(0))) return false;

        const EGLint surfaceAttribs[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
        const EGLSurface surface = g_EGLFuncs.eglCreatePbufferSurface(g_Display, tuple.Config, surfaceAttribs);
        if (surface == EGL_NO_SURFACE) return false;
        NativeSessionState& session = ActiveNativeSession();
        session.Surfaces.push_back({static_cast<NativeWindowType>(0), surface});
        session.Draw = surface;
        NoteDrawSurfaceMoved();
        session.Read = surface;
        tuple.Draw = surface;
        tuple.Read = surface;

        if (!MakeCurrent()) return false;

        PublishDefaultFramebufferDepthStencilFormat();

        MGLOG_D("EGL pbuffer surface created successfully: display=%p, surface=%p, context=%p. size=%dx%d", g_Display,
                surface, tuple.Context, width, height);
        return true;
    }
    // The single backend ES context migrates between app threads (FCL/pojav-style
    // LWJGL hands the EGL context from JVM thread to JVM thread). Ownership must
    // live in ONE global slot: a per-thread flag can never be cleared on the
    // LOSING thread when another thread takes (or destroys/releases) the context,
    // leaving a stale "current" claim behind. A stale claim makes buffer ops issue
    // GL calls that silently no-op (no context is current on that thread) while
    // still updating shadow bookkeeping (bind cache, synced serials), permanently
    // desynchronizing backend buffer state.
    std::atomic<std::thread::id> g_backendContextOwnerThread{};

    // Bumped whenever the backend ES context is destroyed; fence and
    // timer-query handles created under an older generation belong to a
    // dead context and must never be passed back to GL (mirrors
    // BufferImpl's context tracking).
    Uint g_syncContextGeneration = 1;

    // One-fence-per-frame ring driving the buffer-storage pool's recycle gate.
    // A buffer retired during frame N is safe to reuse once frame N's fence has
    // signaled. Touched only in Present()/DestroyEGLContext() on the owning
    // thread -> no lock. Each fence carries the sync generation so a dead-context
    // GLsync is never polled/deleted. Depth 4 >> the 2-3 frames Adreno keeps in
    // flight; wrap-before-signal only happens during a stall and just degrades to
    // the allocate path.
    std::atomic<Uint64> g_currentFrameSerial{0};
    std::atomic<Uint64> g_completedFrameSerial{0};
    FrameFence g_frameFenceRing[kFrameFenceRingDepth];

    // Defined next to the stamp globals below; every owner-id writer must reset the
    // EGL verification stamp BEFORE publishing the new owner.
    void InvalidateEglVerifiedStamp();

    // EVERY SHADOW OF THE CURRENT NATIVE CONTEXT'S BINDINGS, dropped whenever the context the
    // calling thread renders into may have changed (MakeCurrent, a turn handoff between two
    // sessions' apply threads). Re-issuing a bind is cheap; trusting a shadow that describes
    // another context skips one the driver needed (a texture sampled from the wrong unit, a
    // draw against another context's VAO). Requires a current context: it re-establishes
    // GL_TEXTURE0 so the active-unit shadow is true rather than guessed.
    static void InvalidateNativeBindingShadows() {
        // Conservatively drop the redundant-glUseProgram guard: re-issuing one bind
        // after a MakeCurrent is cheaper than trusting a possibly-reset context.
        PrgramImpl::g_lastUsedBackendProgramId = 0;
        // The GLContext becoming current may be a fresh one whose slot versions
        // restarted at zero; the broadcast memo's key is only monotonic within one.
        PrgramImpl::InvalidateBroadcastMemo();
        BufferImpl::InvalidateArrayBufferBindingCache();
        BufferImpl::InvalidateIndexedBufferBindingCache();
        BufferImpl::InvalidatePixelBufferBindingCaches();
        BufferImpl::InvalidateTransformFeedbackBindingShadows();
        FramebufferImpl::InvalidateFramebufferBindingCache();
        FramebufferImpl::InvalidateFramebufferHandleArmMemos();
        VertexArrayImpl::InvalidateVAOBindingCache();
        PixelStoreImpl::InvalidatePackStateCache();
        // The render-state shadow belongs in this list for the same reason as the ones above:
        // it describes the real ES context, which outlives the MobileGL context that is
        // becoming current. See InvalidateSyncedRenderState.
        RenderStateImpl::InvalidateSyncedRenderState();
        TextureImpl::g_boundTexturesCache = {};
        SamplerImpl::g_boundSamplersCache = {};
        if (g_GLESFuncs.glActiveTexture) g_GLESFuncs.glActiveTexture(GL_TEXTURE0);
        TextureImpl::g_activeTextureUnit = 0;
    }

    Bool MakeCurrent() {
        // P14 S4: the context comes from the calling thread's tuple (the session's bound token),
        // the surface from the session. Both halves used to be file-level singletons.
        NativeContextTuple& tuple = ActiveNativeContext();
        NativeSessionState& session = ActiveNativeSession();
        tuple.Draw = session.Draw;
        tuple.Read = session.Read;
        if (!g_EGLFuncs.eglMakeCurrent || g_Display == EGL_NO_DISPLAY || session.Draw == EGL_NO_SURFACE ||
            tuple.Context == EGL_NO_CONTEXT) {
            MGLOG_E_ONCE("DirectGLES::MakeCurrent failed: EGL display/surface/context is not initialized");
            return false;
        }
#if MOBILEGL_BUILD_DISAGGREGATED
        // Shared-image reads can only be fenced from the context that made them.
        if (t_native.boundContext != EGL_NO_CONTEXT && t_native.boundContext != tuple.Context) {
            SharedImageImpl::PublishPendingReads();
        }
#endif
        if (!g_EGLFuncs.eglMakeCurrent(g_Display, session.Draw, session.Draw, tuple.Context)) {
            const EGLint error = g_EGLFuncs.eglGetError ? g_EGLFuncs.eglGetError() : EGL_SUCCESS;
            MGLOG_E_ONCE("DirectGLES::MakeCurrent failed: native eglMakeCurrent returned error 0x%04x", error);
            return false;
        }
        g_EGLFuncs.eglSwapBuffersWithDamageEXT = MG_Util::BackendLoader::ResolveSwapBuffersWithDamage(g_EGLFuncs);
        InvalidateEglVerifiedStamp();
        t_native.boundContext = tuple.Context;
        t_native.boundDraw = session.Draw;
        t_nativeContextThread.BoundSerial = tuple.Serial;
        g_backendContextOwnerThread.store(std::this_thread::get_id(), std::memory_order_release);
        // The ops table may have been unregistered when a previous ES context was
        // destroyed (e.g. a probe context); re-register now that GL is usable.
        BufferImpl::RegisterBufferBackendOps();
        InvalidateNativeBindingShadows();
        // eglSwapInterval requires a current context; a request made while none was
        // current (and dropped by the driver) is retried here.
        ApplyRequestedSwapInterval();
        return true;
    }

    // P14: THE BACKEND'S TURN MOVED TO ANOTHER APPLY THREAD (ServerLoop's BackendTurn). Each
    // session's apply thread keeps its own native context current, but the backend's owner
    // thread and every binding shadow are process-wide and still describe the context of the
    // thread that held the turn before. Take ownership for this thread and forget them.
    void OnBackendTurnHandoff() {
        InvalidateEglVerifiedStamp();
        if (t_native.boundContext == EGL_NO_CONTEXT) {
            g_backendContextOwnerThread.store(std::thread::id{}, std::memory_order_release);
            return;
        }
        g_backendContextOwnerThread.store(std::this_thread::get_id(), std::memory_order_release);
        InvalidateNativeBindingShadows();
    }

    Bool ReleaseCurrent() {
        if (!g_EGLFuncs.eglMakeCurrent || g_Display == EGL_NO_DISPLAY) {
            InvalidateEglVerifiedStamp();
            g_backendContextOwnerThread.store(std::thread::id{}, std::memory_order_release);
            return true;
        }
#if MOBILEGL_BUILD_DISAGGREGATED
        if (t_native.boundContext != EGL_NO_CONTEXT) SharedImageImpl::PublishPendingReads();
#endif
        if (!g_EGLFuncs.eglMakeCurrent(g_Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
            const EGLint error = g_EGLFuncs.eglGetError ? g_EGLFuncs.eglGetError() : EGL_SUCCESS;
            MGLOG_E_ONCE("DirectGLES::ReleaseCurrent failed: native eglMakeCurrent returned error 0x%04x", error);
            return false;
        }
        // Clearing the global owner works from ANY thread (a release request can
        // legally arrive on a thread other than the current owner); erring towards
        // "not current" only defers buffer ops, which is always safe.
        InvalidateEglVerifiedStamp();
        t_native.boundContext = EGL_NO_CONTEXT;
        t_native.boundDraw = EGL_NO_SURFACE;
        t_nativeContextThread.BoundSerial = 0;
        g_backendContextOwnerThread.store(std::thread::id{}, std::memory_order_release);
        return true;
    }

    void InvalidateEglVerifiedStamp() {
        // ~0 frame serial matches no real frame, so the next owner-thread call
        // re-verifies against EGL itself.
        g_eglVerifiedFrameSerial.store(~0ull, std::memory_order_relaxed);
        g_eglVerifiedContextGeneration.store(0, std::memory_order_relaxed);
    }

    // P14 S4: THE FULL TEARDOWN, SCOPED TO THE CALLING THREAD'S SESSION.
    //
    // This used to destroy the process's one context, its one surface and terminate the display.
    // The callers (ReleaseEGLResources, ~BackendObject_DirectGLES) mean "this session's EGL world
    // goes away", so under S4 it destroys EVERY native context that session holds, its surfaces, and
    // terminates the display only when no context anywhere in the process is left - which is what
    // keeps a second live session (S2) serving while this one closes. With no resolver installed
    // there is exactly one session (key 0) and this is the old statement, byte for word.
    //
    // ONE CONTEXT's teardown is DestroyNativeContextFor (the DestroyContext control frame); this is
    // the session's.
    static void DestroyNativeStateOfSession(Uint64 sessionKey);

    void DestroyEGLContext() {
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith) MG_Pipe::MGPipeServerSetContextLive(false);
#endif
        // P14: ANOTHER SESSION'S CONTEXT STILL LIVES. Every native context is in one share group, so
        // the buffers, textures and syncs this process's backend holds outlive this session's
        // contexts: bumping the process-wide generations here told every live session that its
        // GL objects were dead, and they were rebuilt EMPTY (a client exiting or closing its last
        // window turned the compositor's textures to noise). Only this session's own state goes;
        // the process-wide reset below is for the last context in the process.
        const Uint64 endingSession = CurrentNativeSessionKey();
        Bool othersLive = false;
        {
            const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
            for (auto* tuple : NativeContexts()) {
                if (tuple->SessionKey != endingSession && tuple->Context != EGL_NO_CONTEXT) {
                    othersLive = true;
                    break;
                }
            }
        }
        if (othersLive) {
            BufferImpl::OnServerSessionEndedWithOthersLive();
            DestroyNativeStateOfSession(endingSession);
            return;
        }
        BufferImpl::OnBackendContextDestroyed();
        XfbImpl::OnBackendContextDestroyed();
        MultiDrawImpl::OnBackendContextDestroyed();
        OnRestartSubstitutionContextDestroyed();
        ScratchFBOImpl::OnBackendContextDestroyed();
        ReleasePackedWordScratchTexture();
        FramebufferImpl::InvalidateFramebufferBindingCache();
        FramebufferImpl::InvalidateFramebufferHandleArmMemos();
        VertexArrayImpl::InvalidateVAOBindingCache();
        PixelStoreImpl::InvalidatePackStateCache();
        PrgramImpl::InvalidateBroadcastMemo();
        // Texture ids belong to the dying context; wrappers destroyed later must
        // not glDeleteTextures a recycled name in a successor context.
        ++g_backendContextGeneration;
        g_backendContextOwnerThread.store(std::thread::id{}, std::memory_order_release);
        // Outstanding fence handles now refer to a dead context; treat them as
        // signaled from here on.
        ++g_syncContextGeneration;
        // The frame-fence ring's syncs belong to the dead context too; abandon them
        // (the context reclaims its syncs) and floor the completed watermark to the
        // current serial so buffers retired under the old context read as GPU-idle.
        for (FrameFence& slot : g_frameFenceRing)
            slot = {};
        g_completedFrameSerial.store(g_currentFrameSerial.load(std::memory_order_relaxed), std::memory_order_relaxed);

        DestroyNativeStateOfSession(CurrentNativeSessionKey());
    }

    // This session's native contexts and surfaces, and the display when they were the last.
    static void DestroyNativeStateOfSession(Uint64 sessionKey) {
        if (g_Display == EGL_NO_DISPLAY) return;
#if MOBILEGL_BUILD_DISAGGREGATED
        if (t_native.boundContext != EGL_NO_CONTEXT) SharedImageImpl::PublishPendingReads();
#endif
        {
            const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
            if (g_EGLFuncs.eglMakeCurrent) {
                g_EGLFuncs.eglMakeCurrent(g_Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
            t_native.boundContext = EGL_NO_CONTEXT;
            t_native.boundDraw = EGL_NO_SURFACE;
            t_nativeContextThread.BoundSerial = 0;
            for (auto* tuple : NativeContexts()) {
                if (tuple->SessionKey != sessionKey || tuple->Context == EGL_NO_CONTEXT) continue;
                ForgetNativeContextObjects(tuple->Serial);
                tuple->Serial = 0;
                if (g_EGLFuncs.eglDestroyContext) g_EGLFuncs.eglDestroyContext(g_Display, tuple->Context);
                tuple->Context = EGL_NO_CONTEXT;
                tuple->Config = nullptr;
                tuple->Draw = EGL_NO_SURFACE;
                tuple->Read = EGL_NO_SURFACE;
                if (g_displayUsers > 0) --g_displayUsers;
            }
            for (auto* session : NativeSessions()) {
                if (session->SessionKey != sessionKey) continue;
                if (g_EGLFuncs.eglDestroySurface) {
                    for (const auto& entry : session->Surfaces) {
                        if (entry.Surface != EGL_NO_SURFACE) g_EGLFuncs.eglDestroySurface(g_Display, entry.Surface);
                    }
                }
                session->Surfaces.clear();
                session->Draw = EGL_NO_SURFACE;
                session->Read = EGL_NO_SURFACE;
            }
            g_nativeRegistryEpoch.fetch_add(1, std::memory_order_acq_rel);
        }
        // THE LAST CONTEXT ANYWHERE IN THE PROCESS IS WHAT TERMINATES THE DISPLAY. A session that
        // closes while another is rendering must not take eglTerminate with it.
        if (g_displayUsers == 0 && g_EGLFuncs.eglTerminate) {
            g_EGLFuncs.eglTerminate(g_Display);
            NoteDisplayTerminated();
            g_Display = EGL_NO_DISPLAY;
        }
    }
} // namespace MobileGL::MG_Backend::DirectGLES
