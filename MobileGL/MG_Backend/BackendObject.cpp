// MobileGL - MobileGL/MG_Backend/BackendObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BackendObject.h"

namespace MobileGL::MG_Backend {
    namespace {
        Bool IsReleaseCurrentRequest(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
            return dpy == EGL_NO_DISPLAY && draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE && ctx == EGL_NO_CONTEXT;
        }

        std::thread::id CurrentThreadKey() {
            return std::this_thread::get_id();
        }
    } // namespace

    void FormatCapabilityCache::Clear() {
        for (auto& row : FullCaps) {
            row.fill(FormatCapabilityFlags{});
        }
        for (auto& row : CaveatCaps) {
            row.fill(FormatCapabilityFlags{});
        }
        for (auto& row : SampleCounts) {
            for (auto& counts : row) {
                counts.clear();
            }
        }
    }

    Bool HasFormatCapability(FormatCapabilityFlags caps, FormatCapability capability) {
        return static_cast<Bool>(caps & capability);
    }

    SizeT GetFormatCapabilityTargetIndex(TextureTarget target) {
        if (target == TextureTarget::Unknown || static_cast<Int>(target) < 0 ||
            static_cast<SizeT>(target) >= kFormatCapabilityTextureTargetCount) {
            return kFormatCapabilityTargetCount;
        }
        return static_cast<SizeT>(target);
    }

    SizeT GetRenderbufferFormatCapabilityTargetIndex() {
        return kFormatCapabilityRenderbufferTargetIndex;
    }

    Bool BackendObject::InitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (dpy == EGL_NO_DISPLAY) {
            MGLOG_E("InitializeEGLDisplay failed: invalid EGLDisplay");
            return false;
        }

        if (m_eglDisplayInitialized && m_eglDisplay != dpy) {
            MGLOG_E("InitializeEGLDisplay failed: backend already bound to a different EGLDisplay");
            return false;
        }

        m_eglDisplay = dpy;
        m_eglDisplayInitialized = true;
        if (major) {
            *major = 1;
        }
        if (minor) {
            *minor = 5;
        }
        return true;
    }

    Bool BackendObject::CreateEGLWindowSurface(const WindowHandle& handle) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (!m_eglDisplayInitialized) {
            MGLOG_E("CreateEGLWindowSurface failed: EGL display is not initialized");
            return false;
        }
        if (handle.Backend == WindowBackend::Unknown || !handle.Handle) {
            MGLOG_E("CreateEGLWindowSurface failed: invalid native window handle");
            return false;
        }

        if (m_eglSurfaceInitialized && m_eglSurfaceKind == SurfaceKind::Window &&
            m_windowHandle.Backend == handle.Backend && m_windowHandle.Handle == handle.Handle) {
            return true;
        }

        SetWindowHandle(handle);
        if (!InitWindowSurface()) {
            MGLOG_E("CreateEGLWindowSurface failed: backend InitWindowSurface failed");
            return false;
        }

        m_eglSurfaceInitialized = true;
        m_eglSurfaceKind = SurfaceKind::Window;
        m_eglCurrentThreads.clear();
        m_backendCapabilitiesInitialized = false;
        return true;
    }

    Bool BackendObject::CreateEGLPbufferSurface(EGLint width, EGLint height) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (!m_eglDisplayInitialized) {
            MGLOG_E("CreateEGLPbufferSurface failed: EGL display is not initialized");
            return false;
        }
        if (width <= 0 || height <= 0) {
            MGLOG_E("CreateEGLPbufferSurface failed: invalid size %dx%d", width, height);
            return false;
        }

        if (m_eglSurfaceInitialized && m_eglSurfaceKind == SurfaceKind::Pbuffer) {
            return true;
        }

        if (!InitPbufferSurface(width, height)) {
            MGLOG_E("CreateEGLPbufferSurface failed: backend InitPbufferSurface failed");
            return false;
        }

        m_eglSurfaceInitialized = true;
        m_eglSurfaceKind = SurfaceKind::Pbuffer;
        m_eglCurrentThreads.clear();
        m_backendCapabilitiesInitialized = false;
        return true;
    }

    Bool BackendObject::MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        const auto threadKey = CurrentThreadKey();
        if (IsReleaseCurrentRequest(dpy, draw, read, ctx)) {
            m_eglCurrentThreads.erase(threadKey);
            return true;
        }

        if (!m_eglDisplayInitialized || m_eglDisplay != dpy) {
            MGLOG_E("MakeEGLCurrent failed: EGL display mismatch or not initialized");
            return false;
        }
        if (!m_eglSurfaceInitialized) {
            MGLOG_E("MakeEGLCurrent failed: EGL surface is not initialized");
            return false;
        }
        if (draw == EGL_NO_SURFACE || read == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) {
            MGLOG_E("MakeEGLCurrent failed: draw/read/context is invalid");
            return false;
        }

        if (!m_backendCapabilitiesInitialized) {
            if (!InitCapabilities()) {
                MGLOG_E("MakeEGLCurrent failed: InitCapabilities failed");
                return false;
            }
            m_backendCapabilitiesInitialized = true;
        }

        m_eglCurrentThreads[threadKey] = true;
        return true;
    }

    void BackendObject::ResetEGLRuntimeState() {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        m_eglSurfaceInitialized = false;
        m_backendCapabilitiesInitialized = false;
        m_eglSurfaceKind = SurfaceKind::None;
        m_eglCurrentThreads.clear();
    }

    Bool BackendObject::SwapEGLBuffers(EGLDisplay dpy, EGLSurface draw) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (!m_eglDisplayInitialized || m_eglDisplay != dpy) {
            MGLOG_E("SwapEGLBuffers failed: EGL display mismatch or not initialized");
            return false;
        }
        if (m_eglCurrentThreads.find(CurrentThreadKey()) == m_eglCurrentThreads.end()) {
            MGLOG_E("SwapEGLBuffers failed: no current context attached");
            return false;
        }
        if (!m_eglSurfaceInitialized || draw == EGL_NO_SURFACE) {
            MGLOG_E("SwapEGLBuffers failed: invalid draw surface");
            return false;
        }

        const auto& backendFunctions = GetBackendFunctions();
        if (!backendFunctions.Present) {
            MGLOG_E("SwapEGLBuffers failed: backend Present function is null");
            return false;
        }

        backendFunctions.Present();
        return true;
    }

    void BackendObject::ReleaseEGLResources() {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        ResetEGLRuntimeState();
        m_eglDisplay = EGL_NO_DISPLAY;
        m_eglDisplayInitialized = false;
        m_windowHandle = {};
    }

    void BackendObject::SetWindowHandle(const WindowHandle& handle) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        m_windowHandle = handle;
    }

    const FormatCapabilityCache& BackendObject::GetFormatCapabilities() const {
        return m_formatCapabilities;
    }

    FormatCapabilityCache& BackendObject::MutableFormatCapabilities() {
        return m_formatCapabilities;
    }

    Bool BackendObject::InitPbufferSurface(EGLint width, EGLint height) {
        (void)width;
        (void)height;
        return false;
    }
} // namespace MobileGL::MG_Backend
