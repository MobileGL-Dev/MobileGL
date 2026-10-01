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

                // How many planes EGL describes an imported buffer with: EGL_DMA_BUF_PLANE0_* comes
                // from EGL_EXT_image_dma_buf_import, EGL_DMA_BUF_PLANE3_* and every MODIFIER pair
                // from EGL_EXT_image_dma_buf_import_modifiers.  The reader in Core.cpp and the image
                // object below have to agree on the count, so it is named once.
                static constexpr Int kMaxImagePlanes = 4;

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
                EGLContextHandle CreateContext(EGLDisplayHandle display, EGLConfigHandle config,
                                               EGLContextHandle shareCtx, const EGLint* attribList);
                Bool DestroyContext(EGLDisplayHandle display, EGLContextHandle context);
                Bool QueryContext(EGLDisplayHandle display, EGLContextHandle context, EGLint attribute,
                                  EGLint* value) const;
                Bool ValidateContext(EGLContextHandle context) const;
                Bool ValidateContextOnDisplay(EGLDisplayHandle display, EGLContextHandle context) const;
                Bool IsCurrentContextOpenGLCoreProfile() const;
                Bool IsCurrentContextOpenGLCompatibilityProfile() const;
                EGLint GetCurrentContextFlags() const;

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
                // MOBILEGL_IPC_SURFACE=host: the same registration under the other on-screen name.
                // The EGL state does not care which side owns the frames - that is the backend's
                // business - but a trace has to be able to tell the two runs apart, and the two
                // paths part company in the backend call that follows.
                EGLSurfaceHandle CreateHostFrameWindowSurface(EGLDisplayHandle display, EGLConfigHandle config,
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
                // WHAT A HANDLE NAMES, for the GL side of GL_OES_EGL_image: the two entry points in
                // MG_Impl/GLImpl/Exporting/Definitions.cpp are handed a GLeglImageOES and have to
                // answer whether it is an image this display created, and with what geometry.  A
                // read-only lookup belongs beside the other queries a caller outside this class
                // makes, and it is the same object eglCreateImageKHR built and eglDestroyImage
                // removes - the private one stays private for the class's own use.
                // WHAT A HANDLE NAMES, for the GL side of GL_OES_EGL_image.  The two entry points in
                // MG_Impl/GLImpl/Exporting/Definitions.cpp are handed a GLeglImageOES and have to
                // answer (a) whether it is an image this display created and (b) what buffer it
                // describes.  The numbers come back rather than the record itself: the image table and
                // its layout are this class's own business, and a caller outside it has no use for
                // either - which is also why this is a function and not a friend declaration.
                Bool DescribeImage(EGLImageHandle image, EGLint* width, EGLint* height, EGLint* fourCc,
                                   EGLint* planeCount) const {
                    const ImageObject* imageObject = TryGetImage(image);
                    if (imageObject == nullptr) {
                        return false;
                    }
                    if (width != nullptr) *width = imageObject->Width;
                    if (height != nullptr) *height = imageObject->Height;
                    if (fourCc != nullptr) *fourCc = imageObject->FourCC;
                    if (planeCount != nullptr) *planeCount = imageObject->PlaneCount;
                    return true;
                }

                // THE PLANE DESCRIPTORS OF AN IMAGE THIS DISPLAY CREATED, for the one caller that has
                // to hand them to the render server: the dma-buf import (a texture bound to an imported
                // EGLImage has no storage on the server, so the server is told which buffer backs it -
                // one descriptor per plane, over the session transport's SCM_RIGHTS channel).  The fd
                // here is THIS side's dup(), owned by the image; the transport dups again for the
                // sending process, which is why the caller does not close anything.
                Bool DescribeImagePlanes(EGLImageHandle image, EGLint* planeCount, EGLint* fds, EGLint* offsets,
                                         EGLint* pitches, Uint64* modifiers, Bool* hasModifiers, Int maxPlanes) const {
                    const ImageObject* imageObject = TryGetImage(image);
                    if (imageObject == nullptr) {
                        return false;
                    }
                    Int count = imageObject->PlaneCount;
                    if (count > maxPlanes) count = maxPlanes;
                    if (count < 0) count = 0;
                    if (planeCount != nullptr) *planeCount = count;
                    for (Int plane = 0; plane < count; plane++) {
                        const ImagePlane& described = imageObject->Planes[plane];
                        if (fds != nullptr) fds[plane] = described.Fd;
                        if (offsets != nullptr) offsets[plane] = described.Offset;
                        if (pitches != nullptr) pitches[plane] = described.Pitch;
                        if (modifiers != nullptr) modifiers[plane] = described.Modifier;
                        if (hasModifiers != nullptr) hasModifiers[plane] = described.HasModifier;
                    }
                    return true;
                }
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
                Bool DestroyImage(EGLDisplayHandle display, EGLImageHandle image);

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
                    EGLenum ClientAPI = EGL_OPENGL_API;
                    EGLint ClientVersion = 1;
                    EGLint MajorVersion = 1;
                    EGLint MinorVersion = 0;
                    EGLint OpenGLProfileMask = 0;
                    EGLint EGLContextFlags = 0;
                    EGLint OpenGLContextFlags = 0;
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

                // ONE PLANE OF AN IMPORTED BUFFER.  The fd/offset/pitch triple is the same
                // description drmModeAddFB2 takes, and the modifier is the DRM_FORMAT_MOD_* the
                // caller declared; EGL carries up to four of these (EGL_DMA_BUF_PLANE0..3_*).
                struct ImagePlane {
                    // THIS library's descriptor, dup()ed from the caller's when the image was made
                    // and closed when it dies: an EGLImage outlives the call that created it, and
                    // the caller is free to close its own fd the moment eglCreateImage returns.
                    EGLint Fd = -1;
                    EGLint Offset = 0;
                    EGLint Pitch = 0;
                    // The two EGL_DMA_BUF_PLANEn_MODIFIER_LO/HI_EXT halves joined back into the
                    // DRM_FORMAT_MOD_* they were cut from.
                    Uint64 Modifier = 0;
                    Bool HasModifier = false;
                };

                struct ImageObject {
                    EGLDisplayHandle Display = EGL_NO_DISPLAY;
                    EGLContextHandle Context = nullptr;
                    EGLenum Target = EGL_NONE;
                    EGLClientBuffer Buffer = nullptr;
                    // WHAT AN EGL_LINUX_DMA_BUF_EXT IMAGE IS HOLDING, and zero for every other
                    // target: the buffer's geometry and the fourcc that says what its samples mean.
                    // It is kept here because the image is the only thing that outlives the call -
                    // a caller that handed over an fd has nothing left to describe it with.
                    EGLint Width = 0;
                    EGLint Height = 0;
                    EGLint FourCC = 0;
                    EGLint PlaneCount = 0;
                    // Planes 0..PlaneCount-1 are described, in order; index 0 is the plane every
                    // import in this tree reads, the rest are carried because EGL describes them and
                    // a consumer that understands a multi-plane format must be able to see them.
                    ImagePlane Planes[kMaxImagePlanes];
                };

                struct ThreadCurrentState {
                    EGLDisplayHandle Display = EGL_NO_DISPLAY;
                    EGLSurfaceHandle DrawSurface = EGL_NO_SURFACE;
                    EGLSurfaceHandle ReadSurface = EGL_NO_SURFACE;
                    EGLContextHandle Context = nullptr;
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
                // Closes the descriptors an image owns.  Both places an image can die call it:
                // eglDestroyImage, and eglTerminate, which drops a display's images wholesale.
                static void ReleaseImageDescriptors(ImageObject& imageObject);

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

                UnorderedMap<DisplayLookupKey, EGLDisplayHandle, DisplayLookupHasher> m_displayLookup;
                UnorderedMap<EGLDisplayHandle, DisplayObject> m_displays;
                UnorderedMap<EGLConfigHandle, ConfigObject> m_configs;
                UnorderedMap<EGLSurfaceHandle, SurfaceObject> m_surfaces;
                UnorderedMap<EGLContextHandle, ContextObject> m_contexts;
                UnorderedMap<EGLSyncHandle, SyncObject> m_syncs;
                UnorderedMap<EGLImageHandle, ImageObject> m_images;

                UnorderedMap<std::thread::id, EGLint> m_threadErrors;
                UnorderedMap<std::thread::id, EGLenum> m_threadBoundAPI;
                UnorderedMap<std::thread::id, ThreadCurrentState> m_threadCurrents;
                UnorderedMap<EGLContextHandle, std::thread::id> m_contextOwners;
            };
        } // namespace EGLState

        extern UniquePtr<EGLState::EGLContext>& pEGLContext;
    } // namespace MG_State
} // namespace MobileGL
