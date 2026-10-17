// MobileGL - MobileGL/MG_Backend/BackendObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <BackendCaps.h>
#include <MG_Backend/BackendFunctions.h>
#include <MG_Backend/BackendRoles.h>
#include <MG_Util/Damage/Damage.h>

namespace MobileGL {
    namespace MG_Backend {
        enum class WindowBackend {
            Android,
            X11,
            MetalLayer,
            Win32, // Handle is an HWND
            // TODO: Wayland, etc.
            WindowBackendCount,
            Unknown = -1
        };

        struct WindowHandle {
            WindowBackend Backend = WindowBackend::Unknown;
            void* Handle = nullptr;
            Uint32 Width = 0;
            Uint32 Height = 0;
        };

        class BackendObject {
        public:
            virtual ~BackendObject() = default;

            // The hooks only a server, or only a client of a remote server, calls (BackendRoles.h);
            // null for a backend that plays no such role.
            virtual ServerHooks* AsServer() { return nullptr; }
            virtual ClientHooks* AsClient() { return nullptr; }

            virtual void Initialize() = 0;
            virtual Bool InitCapabilities() = 0;
            virtual Bool InitWindowSurface() = 0;

            virtual Bool InitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor);
            virtual Bool CreateEGLWindowSurface(EGLSurface surface, const WindowHandle& handle);
            virtual Bool ResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height);
            virtual Bool CreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height);
            virtual Bool MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
            virtual Bool SwapEGLBuffers(EGLDisplay dpy, EGLSurface draw);
            // eglSwapBuffersWithDamage: `damage` (GL window coordinates) is a hint for the native
            // present. A backend with nothing to hand it to swaps as SwapEGLBuffers does.
            virtual Bool SwapEGLBuffersWithDamage(EGLDisplay dpy, EGLSurface draw,
                                                  const MG_Util::Damage::Region& damage) {
                (void)damage;
                return SwapEGLBuffers(dpy, draw);
            }
            // Forwards the app-requested eglSwapInterval to the backend's native
            // presentation path (no-op for backends without a SetSwapInterval hook).
            virtual void SetEGLSwapInterval(Int interval);
            virtual void ReleaseEGLSurface(EGLSurface surface);
            virtual void ReleaseEGLResources();

            void SetWindowHandle(const WindowHandle& handle);

            virtual const RendererInfo& GetRendererInfo() const = 0;
            virtual String GetBackendAPIVersionString() const = 0;
            virtual const GlobalBackendFunctionsTable& GetBackendFunctions() const = 0;
            virtual const DynamicBackendParameters& GetDynamicParameters() const = 0;
            const FormatCapabilityCache& GetFormatCapabilities() const;
            virtual BackendType GetBackendType() const = 0;

        protected:
            enum class SurfaceKind {
                None,
                Window,
                Pbuffer
            };

            struct EGLCurrentState {
                EGLDisplay Display = EGL_NO_DISPLAY;
                EGLSurface DrawSurface = EGL_NO_SURFACE;
                EGLSurface ReadSurface = EGL_NO_SURFACE;
                EGLContext Context = EGL_NO_CONTEXT;
            };

            struct EGLSurfaceState {
                SurfaceKind Kind = SurfaceKind::None;
                Bool DestroyPending = false;
                // Activated at least once: a re-activation (a multi-window client switching back)
                // is a re-bind and does not re-run the backend's capability probe.
                Bool Activated = false;
                WindowHandle Window;
                EGLint Width = 1;
                EGLint Height = 1;
            };

            void ResetEGLRuntimeState();
            // ResetEGLRuntimeState for a released ACTIVE surface: the backend has no active surface
            // any more, but every OTHER thread's current context and surface stay as they were (a
            // multi-window client closing one popup must not unbind its other windows' threads).
            void ForgetActiveEGLSurface();
            Bool RegisterEGLWindowSurface(EGLSurface surface, const WindowHandle& handle);
            Bool RegisterEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height);
            const EGLSurfaceState* GetRegisteredEGLSurface(EGLSurface surface) const;
            Bool ActivateEGLSurface(EGLSurface surface);
            virtual Bool InitPbufferSurface(EGLint width, EGLint height);
            virtual void OnEGLSurfaceReleased(EGLSurface surface);
            // Every registered surface the base class drops, current or not, after it is gone from
            // m_eglSurfaces. A backend that keeps a native surface per client surface frees it here.
            virtual void OnEGLSurfaceForgotten(EGLSurface surface);
            FormatCapabilityCache& MutableFormatCapabilities();

            mutable std::recursive_mutex m_eglStateMutex;
            FormatCapabilityCache m_formatCapabilities;
            WindowHandle m_windowHandle;
            EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
            EGLSurface m_eglSurface = EGL_NO_SURFACE;
            // The client surface ActivateEGLSurface is bringing up while it calls InitWindowSurface /
            // InitPbufferSurface, which take no surface argument: a backend that maps client surfaces
            // to native ones reads it to re-bind an existing native surface instead of making another.
            EGLSurface m_activatingSurface = EGL_NO_SURFACE;
            Bool m_eglDisplayInitialized = false;
            Bool m_eglSurfaceInitialized = false;
            Bool m_backendCapabilitiesInitialized = false;
            SurfaceKind m_eglSurfaceKind = SurfaceKind::None;
            UnorderedMap<std::thread::id, EGLCurrentState> m_eglCurrentThreads;
            UnorderedMap<EGLSurface, EGLSurfaceState> m_eglSurfaces;

        private:
            Bool IsEGLSurfaceCurrent(EGLSurface surface) const;
            void DestroyPendingEGLSurfaceIfUnused(EGLSurface surface);
            void ReleaseEGLCurrentThread(const std::thread::id& threadKey);
        };

        // A backend's role hooks; null for no backend, or one that plays no such role.
        inline ServerHooks* ServerHooksOf(BackendObject* backend) {
            return backend != nullptr ? backend->AsServer() : nullptr;
        }
        inline ClientHooks* ClientHooksOf(BackendObject* backend) {
            return backend != nullptr ? backend->AsClient() : nullptr;
        }

        extern UniquePtr<BackendObject>& pActiveBackendObject;
    } // namespace MG_Backend
} // namespace MobileGL
