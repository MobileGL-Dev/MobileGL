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
                };

                struct SurfaceObject {
                    EGLDisplayHandle Display = EGL_NO_DISPLAY;
                    EGLConfigHandle Config = nullptr;
                    SurfaceType Type = SurfaceType::Window;
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

                void ReleaseDisplayObjects(EGLDisplayHandle display);
                void ReleaseThreadUnlocked(const std::thread::id& threadKey);

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

        extern UniquePtr<EGLState::EGLContext> pEGLContext;
    } // namespace MG_State
} // namespace MobileGL
