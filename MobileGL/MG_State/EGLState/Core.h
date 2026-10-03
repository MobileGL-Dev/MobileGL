// MobileGL - MobileGL/MG_State/EGLState/Core.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <type_traits>
#include <MG_State/GLState/Core.h>

namespace MobileGL {
    namespace MG_State {
        namespace EGLState {
            class EGLContext {
            public:
                using EGLDisplayHandle = ::EGLDisplay;
                using EGLConfigHandle = ::EGLConfig;
                using EGLSurfaceHandle = ::EGLSurface;
                using EGLContextHandle = ::EGLContext;
                using EGLSyncHandle = ::EGLSync;
                using EGLImageHandle = ::EGLImage;

                EGLContext() = default;

                // Error
                void SetError(EGLint errorCode);
                EGLint ConsumeError();

                // API
                void SetBoundAPI(EGLenum api);
                EGLenum GetBoundAPI() const;

                // Display
                EGLDisplayHandle GetDisplay(NativeDisplayType nativeDisplay);
                EGLDisplayHandle GetPlatformDisplay(EGLenum platform, void* nativeDisplay);
                Bool ValidateDisplay(EGLDisplayHandle display) const;
                Bool IsDisplayInitialized(EGLDisplayHandle display) const;
                // The native display a display was made for and the platform it was asked for on
                // (EGL_NONE for eglGetDisplay): what decides how its window surfaces are presented.
                Bool GetDisplayNative(EGLDisplayHandle display, Uint64* nativeDisplayKey, EGLenum* platform) const;
                Bool InitializeDisplay(EGLDisplayHandle display, EGLint* major, EGLint* minor);
                Bool TerminateDisplay(EGLDisplayHandle display);
                // Whole-library idle checks used by EGLImpl::Terminate to decide
                // when the last eglTerminate may tear MobileGL down entirely.
                Bool HasAnyInitializedDisplay() const;
                Bool HasAnyCurrentContext() const;

                // Config
                Bool ChooseConfig(EGLDisplayHandle display, const EGLint* attribList, EGLConfigHandle* configs,
                                  EGLint configSize, EGLint* numConfig);
                Bool GetConfigs(EGLDisplayHandle display, EGLConfigHandle* configs, EGLint configSize,
                                EGLint* numConfig);
                Bool ValidateConfig(EGLConfigHandle config) const;
                Bool ValidateConfigOnDisplay(EGLDisplayHandle display, EGLConfigHandle config) const;
                Bool GetConfigAttrib(EGLDisplayHandle display, EGLConfigHandle config, EGLint attribute,
                                     EGLint* value) const;

                // Context
                // `deferGLState`: the context is created without its GL state, which
                // BuildContextGLState builds later. Building it creates the context's default
                // objects, and a split client has to tell its server which context they belong to
                // first (the server must know the context before anything is filed under it).
                EGLContextHandle CreateContext(EGLDisplayHandle display, EGLConfigHandle config,
                                               EGLContextHandle shareCtx, const EGLint* attribList,
                                               Bool deferGLState = false);
                Bool BuildContextGLState(EGLContextHandle context);
                // EGL 1.5 3.7.2: a context current to some thread is not destroyed until it is no
                // longer current. Such a destroy answers true with `*deferred` set: the handle is
                // invalid for any new use from here on, and the release that ends its last binding
                // moves it to the reaped list (TakeReapedContexts), where the caller finishes it.
                Bool DestroyContext(EGLDisplayHandle display, EGLContextHandle context, Bool* deferred = nullptr);
                struct ReapedContext {
                    EGLContextHandle Handle = nullptr;
                    Uint64 ClientContextToken = 0;
                    Uint64 ShareGroupToken = 0;
                    SharedPtr<GLState::GLContext> GLStateObject;
                };
                // The contexts whose deferred destroy a release has just completed, oldest first.
                // The caller owns them now: dropping a GLStateObject destroys the context's objects.
                Vector<ReapedContext> TakeReapedContexts();
                Bool QueryContext(EGLDisplayHandle display, EGLContextHandle context, EGLint attribute,
                                  EGLint* value) const;
                Bool ValidateContext(EGLContextHandle context) const;
                Bool ValidateContextOnDisplay(EGLDisplayHandle display, EGLContextHandle context) const;
                Bool IsCurrentContextOpenGLCoreProfile() const;
                Bool IsCurrentContextOpenGLCompatibilityProfile() const;
                EGLint GetCurrentContextFlags() const;
                // The calling thread's current context's reset notification behavior
                // (EGL_NO_RESET_NOTIFICATION with no context current) and whether it asked for
                // robust buffer access.
                EGLint GetCurrentContextResetNotificationStrategy() const;
                Bool IsCurrentContextRobustAccessRequested() const;
                // The client API the calling thread's current context was created for
                // (EGL_OPENGL_API with no context current).
                EGLenum GetCurrentContextClientAPI() const;

                // P14 S1. The wire identity of a context, and of the context the CALLING THREAD
                // has current (0 when it has none - the release edge's value, and the value a
                // session that never crossed a bind_context answers with). Read by MG_Impl's two
                // producers: EGLImpl's CreateContext/DestroyContext control frames and PipeFill's
                // bind_context emission at the make-current edge.
                Uint64 GetContextClientToken(EGLContextHandle context) const;
                Uint64 GetContextShareGroupToken(EGLContextHandle context) const;
                Uint64 CurrentContextClientToken() const;

                // P14 S3. The frontend GL state this context owns, so a caller outside MG_State
                // can reach the other half of a context it just made current. Null for an
                // unknown handle.
                SharedPtr<GLState::GLContext> GetContextGLState(EGLContextHandle context) const;

                // Surface
                EGLSurfaceHandle CreateWindowSurface(EGLDisplayHandle display, EGLConfigHandle config,
                                                     NativeWindowType window, const EGLint* attribList);
                EGLSurfaceHandle CreatePbufferSurface(EGLDisplayHandle display, EGLConfigHandle config,
                                                      const EGLint* attribList);
                EGLSurfaceHandle CreatePixmapSurface(EGLDisplayHandle display, EGLConfigHandle config,
                                                     EGLNativePixmapType pixmap, const EGLint* attribList);
                EGLSurfaceHandle CreatePbufferFromClientBuffer(EGLDisplayHandle display, EGLenum bufferType,
                                                               EGLClientBuffer buffer, EGLConfigHandle config,
                                                               const EGLint* attribList);
                EGLSurfaceHandle CreatePlatformWindowSurface(EGLDisplayHandle display, EGLConfigHandle config,
                                                             void* nativeWindow, const EGLAttrib* attribList);
                EGLSurfaceHandle CreatePlatformPixmapSurface(EGLDisplayHandle display, EGLConfigHandle config,
                                                             void* nativePixmap, const EGLAttrib* attribList);
                Bool DestroySurface(EGLDisplayHandle display, EGLSurfaceHandle surface);
                Bool ResizeSurface(EGLDisplayHandle display, EGLSurfaceHandle surface, EGLint width, EGLint height);
                Bool QuerySurface(EGLDisplayHandle display, EGLSurfaceHandle surface, EGLint attribute,
                                  EGLint* value) const;
                Bool ValidateSurface(EGLSurfaceHandle surface) const;
                Bool ValidateSurfaceOnDisplay(EGLDisplayHandle display, EGLSurfaceHandle surface) const;
                Bool SwapInterval(EGLDisplayHandle display, EGLint interval);
                // The interval eglSwapInterval last set on `display` (1, EGL's default, until then or
                // for a display this state does not know).
                EGLint GetSwapInterval(EGLDisplayHandle display) const;
#if MOBILEGL_BUILD_DISAGGREGATED
                // P12 (on-screen server window), MOBILEGL_IPC_SURFACE=server. A window surface whose
                // window is the SERVER's: the client's native window may be NULL (a headless client
                // has none), and the extent it asked for (EGL_WIDTH/EGL_HEIGHT; 0/0 = the server
                // window's own) stands until the server's real one arrives through SetSurfaceExtent.
                // `platformWindow` keeps eglCreatePlatformWindowSurface's surface type. Split builds
                // only: the pull build's state class is unchanged (G1).
                EGLSurfaceHandle CreateServerOwnedWindowSurface(EGLDisplayHandle display, EGLConfigHandle config,
                                                                const void* nativeWindow, EGLint width,
                                                                EGLint height, Bool platformWindow);
                // P12: the server's REAL extent of a surface - a server-owned window's geometry, from
                // the CreateWindowSurface reply and from every surface-changed event after it - so
                // eglQuerySurface(EGL_WIDTH/EGL_HEIGHT) answers what the server renders at. Unlike
                // ResizeSurface it needs no display (the event carries none) and keeps a 0 as 0.
                Bool SetSurfaceExtent(EGLSurfaceHandle surface, EGLint width, EGLint height);
#endif

                // Current
                Bool MakeCurrent(EGLDisplayHandle display, EGLSurfaceHandle draw, EGLSurfaceHandle read,
                                 EGLContextHandle context);
                void ReleaseThread();
                EGLContextHandle GetCurrentContext() const;
                EGLDisplayHandle GetCurrentDisplay() const;
                EGLSurfaceHandle GetCurrentSurface(EGLint readdraw) const;
                Bool IsDoubleBufferedSurface(EGLSurfaceHandle surface) const;

                // Sync
                EGLSyncHandle CreateSync(EGLDisplayHandle display, EGLenum type, const EGLAttrib* attribList);
                Bool DestroySync(EGLDisplayHandle display, EGLSyncHandle sync);
                EGLint ClientWaitSync(EGLDisplayHandle display, EGLSyncHandle sync, EGLint flags, EGLTime timeout);
                Bool GetSyncAttrib(EGLDisplayHandle display, EGLSyncHandle sync, EGLint attribute,
                                   EGLAttrib* value) const;
                Bool WaitSync(EGLDisplayHandle display, EGLSyncHandle sync, EGLint flags) const;

                // Image
                EGLImageHandle CreateImage(EGLDisplayHandle display, EGLContextHandle context, EGLenum target,
                                           EGLClientBuffer buffer, const EGLAttrib* attribList);
                // `releasedSharedImageId` (optional) receives the shared image the destroyed image
                // named, 0 for any other image: the caller drops the backend's reference to it.
                Bool DestroyImage(EGLDisplayHandle display, EGLImageHandle image,
                                  Uint64* releasedSharedImageId = nullptr);

                // DMA-BUF IMPORT (EGL_EXT_image_dma_buf_import[_modifiers]). A dma-buf this library
                // can name is one of the backend's shared images (MG_Backend::SharedImageExport):
                // single plane, DRM ABGR8888 / XBGR8888 / ARGB8888 / XRGB8888 (all stored RGBA8 by the
                // server, whose layout no client can observe). The state
                // validates and records; the backend identifies the descriptor in between.
                struct DmaBufImportAttribs {
                    EGLint Width = 0;
                    EGLint Height = 0;
                    Uint32 Fourcc = 0;
                    int Fd = -1;
                    EGLint Offset = 0;
                    EGLint Pitch = 0;
                    Bool HasModifier = false;
                    Uint64 Modifier = 0;
                };
                struct SharedImageInfo {
                    Uint64 Id = 0;
                    EGLint Width = 0;
                    EGLint Height = 0;
                    Uint32 Fourcc = 0;
                    // The server session the reference was taken on (MG_State::CurrentWireEpoch() at the
                    // import). A reference of an ended session died with it and is never released on
                    // another: the same image imported again on the fresh session has the same Id.
                    Uint64 WireEpoch = 0;
                };
                static constexpr Uint32 kDrmFourccAbgr8888 = 0x34324241u; // 'AB24'
                static constexpr Uint32 kDrmFourccXbgr8888 = 0x34324258u; // 'XB24'
                static constexpr Uint32 kDrmFourccArgb8888 = 0x34325241u; // 'AR24'
                static constexpr Uint32 kDrmFourccXrgb8888 = 0x34325258u; // 'XR24'
                static Bool IsDmaBufFourccSupported(Uint32 fourcc);

                // The checks eglCreateImage owes an EGL_LINUX_DMA_BUF_EXT target before anything
                // is imported: the display, a null context and buffer, and a complete single-plane
                // attribute list of a supported format. False with the EGL error set.
                Bool PrepareDmaBufImport(EGLDisplayHandle display, EGLContextHandle context, EGLClientBuffer buffer,
                                         const EGLAttrib* attribList, DmaBufImportAttribs* out);
                // An EGLImage naming an imported shared image.
                EGLImageHandle CreateSharedImage(EGLDisplayHandle display, const SharedImageInfo& info);
                // The shared image `image` names; false for anything else (an invalid image, or one
                // of another target).
                Bool GetSharedImage(EGLImageHandle image, SharedImageInfo* out) const;
                // Shared images whose EGLImages went with an eglTerminate: the caller releases them.
                Vector<Uint64> TakeOrphanedSharedImages();
                // eglQueryDmaBufFormatsEXT / eglQueryDmaBufModifiersEXT. `available` is whether the
                // backend has shared images at all; without them the lists are empty.
                Bool QueryDmaBufFormats(EGLDisplayHandle display, Bool available, EGLint maxFormats, EGLint* formats,
                                        EGLint* numFormats);
                Bool QueryDmaBufModifiers(EGLDisplayHandle display, Bool available, EGLint format,
                                          EGLint maxModifiers, Uint64* modifiers, EGLBoolean* externalOnly,
                                          EGLint* numModifiers);

            private:
                enum class SurfaceType {
                    Window,
                    Pbuffer,
                    Pixmap,
                    PbufferFromClientBuffer,
                    PlatformWindow,
                    PlatformPixmap
                };

                struct DisplayLookupKey {
                    Uint64 NativeDisplayKey = 0;
                    EGLenum Platform = EGL_NONE;

                    Bool operator==(const DisplayLookupKey& rhs) const;
                };

                struct DisplayLookupHasher {
                    SizeT operator()(const DisplayLookupKey& key) const;
                };

                struct DisplayObject {
                    Uint64 NativeDisplayKey = 0;
                    EGLenum Platform = EGL_NONE;
                    Bool Initialized = false;
                    EGLint MajorVersion = 1;
                    EGLint MinorVersion = 5;
                    EGLint SwapInterval = 1;
                    Vector<EGLConfigHandle> Configs;
                };

                struct ConfigObject {
                    EGLDisplayHandle Display = EGL_NO_DISPLAY;
                    EGLint ConfigId = 1;
                    EGLint RedSize = 8;
                    EGLint GreenSize = 8;
                    EGLint BlueSize = 8;
                    EGLint AlphaSize = 8;
                    EGLint DepthSize = 24;
                    EGLint StencilSize = 8;
                    EGLint SurfaceType = EGL_WINDOW_BIT | EGL_PBUFFER_BIT | EGL_PIXMAP_BIT;
                    EGLint RenderableType = EGL_OPENGL_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT;
                    EGLint MinSwapInterval = 0;
                    EGLint MaxSwapInterval = 4;
                    EGLint NativeVisualId = 0;
                };

                struct ContextObject {
                    EGLDisplayHandle Display = EGL_NO_DISPLAY;
                    EGLConfigHandle Config = nullptr;
                    EGLContextHandle SharedContext = nullptr;
                    // P14 S1 (docs/Disaggregated/design/11-state-ownership.md). The wire identity of
                    // this context: `ClientContextToken` is what bind_context carries and what the
                    // server's per-session context table is keyed by, `ShareGroupToken` the group
                    // it was derived into (ShareContext's group, or one of its own). Minted here,
                    // in the one place a context is born, so no caller can invent a second one.
                    Uint64 ClientContextToken = 0;
                    Uint64 ShareGroupToken = 0;
                    // P14 S3. The GL state of THIS EGL context. Its object registries live in the
                    // share group's ShareGroupState (shared with every context that shares with
                    // this one); its bindings, errors and current program are its own.
                    SharedPtr<GLState::GLContext> GLStateObject;
                    // The group GLStateObject joins when it is built later (CreateContext's
                    // deferGLState); null once it is built.
                    SharedPtr<GLState::ShareGroupState> PendingShareGroup;
                    // eglDestroyContext was called while the context was current to a thread.
                    Bool DestroyPending = false;
                    EGLenum ClientAPI = EGL_OPENGL_API;
                    EGLint ClientVersion = 1;
                    EGLint MajorVersion = 1;
                    EGLint MinorVersion = 0;
                    EGLint OpenGLProfileMask = 0;
                    EGLint EGLContextFlags = 0;
                    // GL_CONTEXT_FLAGS as requested, without GL_CONTEXT_FLAG_ROBUST_ACCESS_BIT: that
                    // one is reported only where robust buffer access is actually provided, which
                    // the GL layer decides (RobustAccessRequested below is the request).
                    EGLint OpenGLContextFlags = 0;
                    // EGL_EXT_create_context_robustness / EGL 1.5. EGL_NO_RESET_NOTIFICATION or
                    // EGL_LOSE_CONTEXT_ON_RESET; contexts that share must agree on it.
                    EGLint ResetNotificationStrategy = EGL_NO_RESET_NOTIFICATION;
                    // EGL_CONTEXT_OPENGL_ROBUST_ACCESS(_EXT) or the KHR flag bit was given as true.
                    Bool RobustAccessRequested = false;
                };

                struct SurfaceObject {
                    EGLDisplayHandle Display = EGL_NO_DISPLAY;
                    EGLConfigHandle Config = nullptr;
                    SurfaceType Type = SurfaceType::Window;
                    Bool DestroyPending = false;
                    Uint64 NativeHandleKey = 0;
                    EGLClientBuffer ClientBuffer = nullptr;
                    EGLenum BufferType = EGL_NONE;
                    EGLint Width = 0;
                    EGLint Height = 0;
                    EGLint TextureFormat = EGL_NO_TEXTURE;
                    EGLint TextureTarget = EGL_NO_TEXTURE;
                    EGLint MipmapLevel = 0;
                    EGLint MipmapTexture = EGL_FALSE;
                    EGLint RenderBuffer = EGL_BACK_BUFFER;
                    EGLint SwapBehavior = EGL_BUFFER_DESTROYED;
                };

                struct SyncObject {
                    EGLDisplayHandle Display = EGL_NO_DISPLAY;
                    EGLenum Type = EGL_SYNC_FENCE;
                    EGLenum Condition = EGL_SYNC_PRIOR_COMMANDS_COMPLETE;
                    EGLenum Status = EGL_SIGNALED;
                };

                struct ImageObject {
                    EGLDisplayHandle Display = EGL_NO_DISPLAY;
                    EGLContextHandle Context = nullptr;
                    EGLenum Target = EGL_NONE;
                    EGLClientBuffer Buffer = nullptr;
                    // EGL_LINUX_DMA_BUF_EXT only: the backend's shared image (Id != 0).
                    SharedImageInfo Shared;
                };

                struct ThreadCurrentState {
                    EGLDisplayHandle Display = EGL_NO_DISPLAY;
                    EGLSurfaceHandle DrawSurface = EGL_NO_SURFACE;
                    EGLSurfaceHandle ReadSurface = EGL_NO_SURFACE;
                    EGLContextHandle Context = nullptr;
                    // P14 S3. What pGLContext held on this thread BEFORE the first eglMakeCurrent
                    // bound an EGL context to it, so releasing restores exactly the binding the
                    // thread had rather than a guessed default.
                    SharedPtr<GLState::GLContext> RestoreThreadGLState;
                };

                template <typename HandleType>
                static HandleType EncodeHandle(Uint64 rawHandle) {
                    return reinterpret_cast<HandleType>(static_cast<SizeT>(rawHandle));
                }

                template <typename NativeType>
                static Uint64 ToNativeKey(NativeType nativeHandle) {
                    if constexpr (std::is_pointer_v<NativeType>) {
                        return static_cast<Uint64>(reinterpret_cast<SizeT>(nativeHandle));
                    } else {
                        return static_cast<Uint64>(nativeHandle);
                    }
                }

                static std::thread::id CurrentThreadKey();

                EGLDisplayHandle GetOrCreateDisplay(Uint64 nativeDisplayKey, EGLenum platform);
                EGLConfigHandle CreateDefaultConfig(EGLDisplayHandle display, EGLint configId, EGLint stencilSize);

                DisplayObject* TryGetDisplay(EGLDisplayHandle display);
                const DisplayObject* TryGetDisplay(EGLDisplayHandle display) const;
                const ConfigObject* TryGetConfig(EGLConfigHandle config) const;
                const ContextObject* TryGetContext(EGLContextHandle context) const;
                const SurfaceObject* TryGetSurface(EGLSurfaceHandle surface) const;
                const SyncObject* TryGetSync(EGLSyncHandle sync) const;
                const ImageObject* TryGetImage(EGLImageHandle image) const;

                void ReleaseDisplayObjects(EGLDisplayHandle display);
                void ReleaseThreadUnlocked(const std::thread::id& threadKey);
                Bool IsSurfaceCurrentUnlocked(EGLSurfaceHandle surface) const;
                void DestroyPendingSurfaceIfUnused(EGLSurfaceHandle surface);

            private:
                mutable std::recursive_mutex m_mutex;

                Uint64 m_nextDisplayHandle = 1;
                Uint64 m_nextConfigHandle = 1;
                Uint64 m_nextSurfaceHandle = 1;
                Uint64 m_nextContextHandle = 1;
                Uint64 m_nextSyncHandle = 1;
                Uint64 m_nextImageHandle = 1;
                // P14 S1. Dense from 1, never reused, never 0: 0 is "no context" on both planes.
                Uint64 m_nextClientContextToken = 1;
                Uint64 m_nextShareGroupToken = 1;

                UnorderedMap<DisplayLookupKey, EGLDisplayHandle, DisplayLookupHasher> m_displayLookup;
                UnorderedMap<EGLDisplayHandle, DisplayObject> m_displays;
                UnorderedMap<EGLConfigHandle, ConfigObject> m_configs;
                UnorderedMap<EGLSurfaceHandle, SurfaceObject> m_surfaces;
                UnorderedMap<EGLContextHandle, ContextObject> m_contexts;
                Vector<ReapedContext> m_reapedContexts;
                UnorderedMap<EGLSyncHandle, SyncObject> m_syncs;
                UnorderedMap<EGLImageHandle, ImageObject> m_images;
                Vector<Uint64> m_orphanedSharedImages;

                UnorderedMap<std::thread::id, EGLint> m_threadErrors;
                UnorderedMap<std::thread::id, EGLenum> m_threadBoundAPI;
                UnorderedMap<std::thread::id, ThreadCurrentState> m_threadCurrents;
                UnorderedMap<EGLContextHandle, std::thread::id> m_contextOwners;
            };
        } // namespace EGLState

        extern UniquePtr<EGLState::EGLContext>& pEGLContext;
    } // namespace MG_State
} // namespace MobileGL
