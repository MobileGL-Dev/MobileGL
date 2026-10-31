// MobileGL - MobileGL/MG_Backend/BackendObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BackendObject.h"

namespace MobileGL::MG_Backend {
    Bool BackendObject::InitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor) {
        return m_egl.InitializeDisplay(dpy, major, minor);
    }

    Bool BackendObject::CreateEGLWindowSurface(EGLSurface surface, const WindowHandle& handle) {
        return m_egl.CreateWindowSurface(surface, handle);
    }

    Bool BackendObject::ResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height) {
        return m_egl.ResizeSurface(surface, width, height);
    }

    Bool BackendObject::CreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height) {
        return m_egl.CreatePbufferSurface(surface, width, height);
    }

    Bool BackendObject::MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
        return m_egl.MakeCurrent(dpy, draw, read, ctx);
    }

    Bool BackendObject::SwapEGLBuffers(EGLDisplay dpy, EGLSurface draw) {
        return m_egl.Swap(dpy, draw);
    }

    void BackendObject::ReleaseEGLSurface(EGLSurface surface) {
        m_egl.ReleaseSurface(surface);
    }

    void BackendObject::ReleaseEGLResources() {
        m_egl.ReleaseAll();
    }

    void BackendObject::SetWindowHandle(const WindowHandle& handle) {
        m_egl.SetWindowHandle(handle);
    }

    void BackendObject::SetEGLSwapInterval(Int interval) {
        const auto& backendFunctions = GetBackendFunctions();
        if (backendFunctions.SetSwapInterval) {
            backendFunctions.SetSwapInterval(interval);
        }
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

    void BackendObject::OnEGLSurfaceReleased(EGLSurface surface) {
        (void)surface;
    }

    void BackendObject::OnEGLSurfaceForgotten(EGLSurface surface) {
        (void)surface;
    }
} // namespace MobileGL::MG_Backend
