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

        MGLOG_I("Backend format capabilities:");
        for (SizeT targetIndex = 0; targetIndex < kFormatCapabilityTargetCount; ++targetIndex) {
            MGLOG_I("");
            const String targetName = GetFormatCapabilityTargetName(targetIndex);
            MGLOG_I("- %s", targetName.c_str());
            const String header = BuildFormatCapabilityHeader(formatNameWidth);
            MGLOG_I("%s", header.c_str());
            for (SizeT formatIndex = 0; formatIndex < kFormatCapabilityFormatCount; ++formatIndex) {
                const String row = BuildFormatCapabilityRow(cache, targetIndex, formatIndex, formatNameWidth);
                MGLOG_I("%s", row.c_str());
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
            m_windowHandle.Backend == handle.Backend && m_windowHandle.Handle == handle.Handle &&
            m_windowHandle.Width == handle.Width && m_windowHandle.Height == handle.Height) {
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

    Bool BackendObject::ResizeEGLWindowSurface(Uint32 width, Uint32 height) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (!m_eglSurfaceInitialized || m_eglSurfaceKind != SurfaceKind::Window) {
            MGLOG_E("ResizeEGLWindowSurface failed: no window surface is initialized");
            return false;
        }
        m_windowHandle.Width = width;
        m_windowHandle.Height = height;
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
