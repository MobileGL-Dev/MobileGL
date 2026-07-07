// MobileGL - MobileGL/MG_Backend/BackendObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BackendObject.h"
#include "MG_Util/Converters/MGToStr/TextureEnumConverter.h"
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace MobileGL::MG_Backend {
    namespace {
        constexpr FormatCapability kPrintedFormatCapabilities[] = {
            FormatCapability::Creatable,
            FormatCapability::Sampled,
            FormatCapability::LinearFilter,
            FormatCapability::GenerateMipmap,
            FormatCapability::TextureGather,
            FormatCapability::TextureShadow,
            FormatCapability::FramebufferRenderable,
            FormatCapability::FramebufferLayered,
            FormatCapability::MultisampleTexture,
            FormatCapability::MultisampleRenderbuffer,
            FormatCapability::ColorAttachment,
            FormatCapability::DepthAttachment,
            FormatCapability::StencilAttachment,
            FormatCapability::TextureBuffer,
        };

        Bool IsReleaseCurrentRequest(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
            (void)dpy;
            return draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE && ctx == EGL_NO_CONTEXT;
        }

        std::thread::id CurrentThreadKey() {
            return std::this_thread::get_id();
        }

        const char* ConvertFormatCapabilityToString(FormatCapability capability) {
            switch (capability) {
            case FormatCapability::Creatable:
                return "Creatable";
            case FormatCapability::Sampled:
                return "Sampled";
            case FormatCapability::LinearFilter:
                return "LinearFilter";
            case FormatCapability::GenerateMipmap:
                return "GenerateMipmap";
            case FormatCapability::TextureGather:
                return "TextureGather";
            case FormatCapability::TextureShadow:
                return "TextureShadow";
            case FormatCapability::FramebufferRenderable:
                return "FramebufferRenderable";
            case FormatCapability::FramebufferLayered:
                return "FramebufferLayered";
            case FormatCapability::MultisampleTexture:
                return "MultisampleTexture";
            case FormatCapability::MultisampleRenderbuffer:
                return "MultisampleRenderbuffer";
            case FormatCapability::ColorAttachment:
                return "ColorAttachment";
            case FormatCapability::DepthAttachment:
                return "DepthAttachment";
            case FormatCapability::StencilAttachment:
                return "StencilAttachment";
            case FormatCapability::TextureBuffer:
                return "TextureBuffer";
            }
            return "Unknown";
        }

        const char* GetFormatCapabilitySupportString(const FormatCapabilityCache& cache,
                                                     SizeT targetIndex,
                                                     SizeT formatIndex,
                                                     FormatCapability capability) {
            if (HasFormatCapability(cache.FullCaps[targetIndex][formatIndex], capability)) return "Full";
            if (HasFormatCapability(cache.CaveatCaps[targetIndex][formatIndex], capability)) return "Caveat";
            return "None";
        }

        SizeT GetPrintedFormatNameWidth() {
            SizeT width = 0;
            for (SizeT formatIndex = 0; formatIndex < kFormatCapabilityFormatCount; ++formatIndex) {
                const auto format = static_cast<TextureInternalFormat>(formatIndex);
                width = std::max(width, MG_Util::ConvertTextureInternalFormatToString(format).size());
            }
            return width;
        }

        SizeT GetCapabilityColumnWidth(FormatCapability capability) {
            SizeT width = std::strlen(ConvertFormatCapabilityToString(capability));
            width = std::max<SizeT>(width, std::strlen("Caveat"));
            return width;
        }

        String GetFormatCapabilityTargetName(SizeT targetIndex) {
            if (targetIndex == kFormatCapabilityRenderbufferTargetIndex) return "Renderbuffer";
            return MG_Util::ConvertTextureTargetToString(static_cast<TextureTarget>(targetIndex));
        }

        String BuildFormatCapabilityHeader(SizeT formatNameWidth) {
            std::ostringstream line;
            line << std::left << std::setw(static_cast<Int>(formatNameWidth)) << "";
            for (FormatCapability capability : kPrintedFormatCapabilities) {
                line << " | " << std::left << std::setw(static_cast<Int>(GetCapabilityColumnWidth(capability)))
                     << ConvertFormatCapabilityToString(capability);
            }
            return line.str();
        }

        String BuildFormatCapabilityRow(const FormatCapabilityCache& cache,
                                        SizeT targetIndex,
                                        SizeT formatIndex,
                                        SizeT formatNameWidth) {
            const auto format = static_cast<TextureInternalFormat>(formatIndex);
            std::ostringstream line;
            line << std::left << std::setw(static_cast<Int>(formatNameWidth))
                 << MG_Util::ConvertTextureInternalFormatToString(format);
            for (FormatCapability capability : kPrintedFormatCapabilities) {
                line << " | " << std::left << std::setw(static_cast<Int>(GetCapabilityColumnWidth(capability)))
                     << GetFormatCapabilitySupportString(cache, targetIndex, formatIndex, capability);
            }
            return line.str();
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

    void PrintFormatCapabilities(const FormatCapabilityCache& cache) {
        const SizeT formatNameWidth = GetPrintedFormatNameWidth();

        MGLOG_D("Backend format capabilities:");
        for (SizeT targetIndex = 0; targetIndex < kFormatCapabilityTargetCount; ++targetIndex) {
            MGLOG_D("");
            const String targetName = GetFormatCapabilityTargetName(targetIndex);
            MGLOG_D("- %s", targetName.c_str());
            const String header = BuildFormatCapabilityHeader(formatNameWidth);
            MGLOG_D("%s", header.c_str());
            for (SizeT formatIndex = 0; formatIndex < kFormatCapabilityFormatCount; ++formatIndex) {
                const String row = BuildFormatCapabilityRow(cache, targetIndex, formatIndex, formatNameWidth);
                MGLOG_D("%s", row.c_str());
            }
        }
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

    Bool BackendObject::CreateEGLWindowSurface(EGLSurface surface, const WindowHandle& handle) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        return RegisterEGLWindowSurface(surface, handle) && ActivateEGLSurface(surface);
    }

    Bool BackendObject::ResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        auto surfaceIt = m_eglSurfaces.find(surface);
        if (surfaceIt == m_eglSurfaces.end() || surfaceIt->second.Kind != SurfaceKind::Window) {
            MGLOG_E("ResizeEGLWindowSurface failed: no window surface is initialized");
            return false;
        }
        surfaceIt->second.Window.Width = width;
        surfaceIt->second.Window.Height = height;
        if (m_eglSurface == surface) {
            m_windowHandle.Width = width;
            m_windowHandle.Height = height;
        }
        return true;
    }

    Bool BackendObject::CreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        return RegisterEGLPbufferSurface(surface, width, height) && ActivateEGLSurface(surface);
    }

    Bool BackendObject::RegisterEGLWindowSurface(EGLSurface surface, const WindowHandle& handle) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (!m_eglDisplayInitialized) {
            MGLOG_E("RegisterEGLWindowSurface failed: EGL display is not initialized");
            return false;
        }
        if (surface == EGL_NO_SURFACE) {
            MGLOG_E("RegisterEGLWindowSurface failed: invalid EGLSurface");
            return false;
        }
        if (handle.Backend == WindowBackend::Unknown || !handle.Handle) {
            MGLOG_E("RegisterEGLWindowSurface failed: invalid native window handle");
            return false;
        }

        auto& state = m_eglSurfaces[surface];
        state = EGLSurfaceState{
            .Kind = SurfaceKind::Window,
            .Window = handle,
            .Width = static_cast<EGLint>(std::max<Uint32>(handle.Width, 1)),
            .Height = static_cast<EGLint>(std::max<Uint32>(handle.Height, 1)),
        };
        return true;
    }

    Bool BackendObject::RegisterEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (!m_eglDisplayInitialized) {
            MGLOG_E("RegisterEGLPbufferSurface failed: EGL display is not initialized");
            return false;
        }
        if (surface == EGL_NO_SURFACE) {
            MGLOG_E("RegisterEGLPbufferSurface failed: invalid EGLSurface");
            return false;
        }
        if (width <= 0 || height <= 0) {
            MGLOG_E("RegisterEGLPbufferSurface failed: invalid size %dx%d", width, height);
            return false;
        }

        m_eglSurfaces[surface] = EGLSurfaceState{
            .Kind = SurfaceKind::Pbuffer,
            .Width = width,
            .Height = height,
        };
        return true;
    }

    const BackendObject::EGLSurfaceState* BackendObject::GetRegisteredEGLSurface(EGLSurface surface) const {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        auto surfaceIt = m_eglSurfaces.find(surface);
        return surfaceIt == m_eglSurfaces.end() ? nullptr : &surfaceIt->second;
    }

    Bool BackendObject::ActivateEGLSurface(EGLSurface surface) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        const auto* surfaceState = GetRegisteredEGLSurface(surface);
        if (!surfaceState) {
            MGLOG_E("ActivateEGLSurface failed: EGL surface is not registered");
            return false;
        }
        if (m_eglSurfaceInitialized && m_eglSurface == surface) {
            return true;
        }

        if (surfaceState->Kind == SurfaceKind::Window) {
            SetWindowHandle(surfaceState->Window);
            if (!InitWindowSurface()) {
                MGLOG_E("ActivateEGLSurface failed: backend InitWindowSurface failed");
                return false;
            }
        } else if (surfaceState->Kind == SurfaceKind::Pbuffer) {
            if (!InitPbufferSurface(surfaceState->Width, surfaceState->Height)) {
                MGLOG_E("ActivateEGLSurface failed: backend InitPbufferSurface failed");
                return false;
            }
        } else {
            MGLOG_E("ActivateEGLSurface failed: unsupported surface kind");
            return false;
        }

        m_eglSurface = surface;
        m_eglSurfaceInitialized = true;
        m_eglSurfaceKind = surfaceState->Kind;
        m_eglCurrentThreads.clear();
        m_backendCapabilitiesInitialized = false;
        return true;
    }

    Bool BackendObject::MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        const auto threadKey = CurrentThreadKey();
        if (IsReleaseCurrentRequest(dpy, draw, read, ctx)) {
            ReleaseEGLCurrentThread(threadKey);
            return true;
        }

        if (!m_eglDisplayInitialized || m_eglDisplay != dpy) {
            MGLOG_E("MakeEGLCurrent failed: EGL display mismatch or not initialized");
            return false;
        }
        if (!m_eglSurfaceInitialized) {
            if (draw != read || !ActivateEGLSurface(draw)) {
                MGLOG_E("MakeEGLCurrent failed: EGL surface is not initialized");
                return false;
            }
        }
        if (!GetRegisteredEGLSurface(draw) || !GetRegisteredEGLSurface(read)) {
            MGLOG_E("MakeEGLCurrent failed: EGL surface is not registered");
            return false;
        }
        if (draw != read) {
            MGLOG_E("MakeEGLCurrent failed: separate draw/read surfaces are not supported");
            return false;
        }
        if (draw != m_eglSurface && !ActivateEGLSurface(draw)) {
            MGLOG_E("MakeEGLCurrent failed: EGL surface is not backed by this backend");
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

        ReleaseEGLCurrentThread(threadKey);
        m_eglCurrentThreads[threadKey] = EGLCurrentState{
            .Display = dpy,
            .DrawSurface = draw,
            .ReadSurface = read,
            .Context = ctx,
        };
        return true;
    }

    void BackendObject::ResetEGLRuntimeState() {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        m_eglSurfaceInitialized = false;
        m_backendCapabilitiesInitialized = false;
        m_eglSurfaceKind = SurfaceKind::None;
        m_eglSurface = EGL_NO_SURFACE;
        m_windowHandle = {};
        m_eglCurrentThreads.clear();
    }

    Bool BackendObject::SwapEGLBuffers(EGLDisplay dpy, EGLSurface draw) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (!m_eglDisplayInitialized || m_eglDisplay != dpy) {
            MGLOG_E("SwapEGLBuffers failed: EGL display mismatch or not initialized");
            return false;
        }
        const auto currentIt = m_eglCurrentThreads.find(CurrentThreadKey());
        if (currentIt == m_eglCurrentThreads.end()) {
            MGLOG_E("SwapEGLBuffers failed: no current context attached");
            return false;
        }
        if (currentIt->second.Display != dpy || currentIt->second.DrawSurface != draw ||
            currentIt->second.Context == EGL_NO_CONTEXT) {
            MGLOG_E("SwapEGLBuffers failed: draw surface is not current on this thread");
            return false;
        }
        if (!m_eglSurfaceInitialized || draw == EGL_NO_SURFACE || draw != m_eglSurface) {
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

    Bool BackendObject::IsEGLSurfaceCurrent(EGLSurface surface) const {
        if (surface == EGL_NO_SURFACE) {
            return false;
        }
        for (const auto& current : m_eglCurrentThreads) {
            if (current.second.DrawSurface == surface || current.second.ReadSurface == surface) {
                return true;
            }
        }
        return false;
    }

    void BackendObject::DestroyPendingEGLSurfaceIfUnused(EGLSurface surface) {
        auto surfaceIt = m_eglSurfaces.find(surface);
        if (surfaceIt == m_eglSurfaces.end() || !surfaceIt->second.DestroyPending ||
            IsEGLSurfaceCurrent(surface)) {
            return;
        }

        m_eglSurfaces.erase(surfaceIt);
        if (m_eglSurface == surface) {
            OnEGLSurfaceReleased(surface);
            ResetEGLRuntimeState();
        }
    }

    void BackendObject::ReleaseEGLCurrentThread(const std::thread::id& threadKey) {
        auto currentIt = m_eglCurrentThreads.find(threadKey);
        if (currentIt == m_eglCurrentThreads.end()) {
            return;
        }

        const EGLSurface drawSurface = currentIt->second.DrawSurface;
        const EGLSurface readSurface = currentIt->second.ReadSurface;
        m_eglCurrentThreads.erase(currentIt);
        DestroyPendingEGLSurfaceIfUnused(drawSurface);
        DestroyPendingEGLSurfaceIfUnused(readSurface);
    }

    void BackendObject::ReleaseEGLSurface(EGLSurface surface) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        auto surfaceIt = m_eglSurfaces.find(surface);
        if (surfaceIt == m_eglSurfaces.end()) {
            return;
        }

        if (IsEGLSurfaceCurrent(surface)) {
            surfaceIt->second.DestroyPending = true;
            return;
        }

        m_eglSurfaces.erase(surfaceIt);
        if (m_eglSurface == surface) {
            OnEGLSurfaceReleased(surface);
            ResetEGLRuntimeState();
        }
    }

    void BackendObject::ReleaseEGLResources() {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        ResetEGLRuntimeState();
        m_eglSurfaces.clear();
        m_eglDisplay = EGL_NO_DISPLAY;
        m_eglDisplayInitialized = false;
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

    void BackendObject::OnEGLSurfaceReleased(EGLSurface surface) {
        (void)surface;
    }
} // namespace MobileGL::MG_Backend
