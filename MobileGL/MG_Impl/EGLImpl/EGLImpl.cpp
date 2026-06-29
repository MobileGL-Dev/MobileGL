// MobileGL - MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "EGLImpl.h"
#include "../GetProcAddress.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_State/EGLState/Core.h>
#include <type_traits>

namespace MobileGL::MG_Impl::EGLImpl {
    namespace {
        using EGLStateContext = MG_State::EGLState::EGLContext;

        EGLStateContext* GetState() {
            if (!MG_State::pEGLContext) {
                MGLOG_E("pEGLContext is null. MG_State may not be initialized.");
            }
            return MG_State::pEGLContext.get();
        }

        MG_Backend::BackendObject* GetBackendObject(EGLStateContext* state) {
            auto* backendObject = MG_Backend::pActiveBackendObject.get();
            if (!backendObject && state) {
                state->SetError(EGL_NOT_INITIALIZED);
            }
            return backendObject;
        }

        MG_Backend::WindowBackend DetectWindowBackend() {
#if defined(ANDROID) || defined(__ANDROID__)
            return MG_Backend::WindowBackend::Android;
#elif defined(__APPLE__)
            return MG_Backend::WindowBackend::MetalLayer;
#elif defined(__linux__)
            return MG_Backend::WindowBackend::X11;
#else
            return MG_Backend::WindowBackend::Unknown;
#endif
        }

        EGLint GetAttribValue(const EGLint* attribList, EGLint attrib, EGLint defaultValue) {
            if (!attribList) {
                return defaultValue;
            }
            for (SizeT i = 0; attribList[i] != EGL_NONE; i += 2) {
                if (attribList[i] == attrib) {
                    return attribList[i + 1];
                }
            }
            return defaultValue;
        }

        EGLint GetAttribValue(const EGLAttrib* attribList, EGLint attrib, EGLint defaultValue) {
            if (!attribList) {
                return defaultValue;
            }
            for (SizeT i = 0; attribList[i] != EGL_NONE; i += 2) {
                if (attribList[i] == attrib) {
                    return static_cast<EGLint>(attribList[i + 1]);
                }
            }
            return defaultValue;
        }

        template <typename NativeType>
        Bool IsNullNativeHandle(NativeType nativeHandle) {
            if constexpr (std::is_pointer_v<NativeType>) {
                return nativeHandle == nullptr;
            } else {
                return nativeHandle == 0;
            }
        }

        template <typename NativeType>
        void* ToVoidHandle(NativeType nativeHandle) {
            if constexpr (std::is_pointer_v<NativeType>) {
                return reinterpret_cast<void*>(nativeHandle);
            } else {
                return reinterpret_cast<void*>(static_cast<SizeT>(nativeHandle));
            }
        }
    } // namespace

    EGLSurface CreateWindowSurface(EGLDisplay dpy, EGLConfig config, NativeWindowType window,
                                   const EGLint* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SURFACE;
        }
        if (!state->IsDisplayInitialized(dpy)) {
            state->SetError(EGL_NOT_INITIALIZED);
            return EGL_NO_SURFACE;
        }
        if (!state->ValidateConfigOnDisplay(dpy, config)) {
            state->SetError(EGL_BAD_CONFIG);
            return EGL_NO_SURFACE;
        }
        if (IsNullNativeHandle(window)) {
            state->SetError(EGL_BAD_NATIVE_WINDOW);
            return EGL_NO_SURFACE;
        }

        auto* backendObject = GetBackendObject(state);
        if (!backendObject) {
            MGLOG_E("activeBackendObject not initialized!");
            return EGL_NO_SURFACE;
        }

        const MG_Backend::WindowHandle windowHandle = {
            .Backend = DetectWindowBackend(),
            .Handle = ToVoidHandle(window),
            .Width = static_cast<Uint32>(std::max<EGLint>(GetAttribValue(attrib_list, EGL_WIDTH, 0), 0)),
            .Height = static_cast<Uint32>(std::max<EGLint>(GetAttribValue(attrib_list, EGL_HEIGHT, 0), 0)),
        };
        if (!backendObject->CreateEGLWindowSurface(windowHandle)) {
            state->SetError(EGL_BAD_NATIVE_WINDOW);
            return EGL_NO_SURFACE;
        }

        return state->CreateWindowSurface(dpy, config, window, attrib_list);
    }

    EGLBoolean SwapBuffers(EGLDisplay dpy, EGLSurface draw) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->ValidateSurfaceOnDisplay(dpy, draw)) {
            state->SetError(EGL_BAD_SURFACE);
            return EGL_FALSE;
        }

        auto* backendObject = GetBackendObject(state);
        if (!backendObject) {
            MGLOG_E("activeBackendObject not initialized!");
            return EGL_FALSE;
        }
        if (!backendObject->SwapEGLBuffers(dpy, draw)) {
            state->SetError(EGL_BAD_SURFACE);
            return EGL_FALSE;
        }
        return EGL_TRUE;
    }

    EGLBoolean ChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs, EGLint config_size,
                            EGLint* num_config) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->ChooseConfig(dpy, attrib_list, configs, config_size, num_config) ? EGL_TRUE : EGL_FALSE;
    }

    EGLContext CreateContext(EGLDisplay dpy, EGLConfig config, EGLContext shareCtx, const EGLint* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_CONTEXT;
        }
        return state->CreateContext(dpy, config, shareCtx, attrib_list);
    }

    EGLBoolean Initialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->InitializeDisplay(dpy, major, minor)) {
            return EGL_FALSE;
        }

        auto* backendObject = GetBackendObject(state);
        if (!backendObject) {
            MGLOG_E("activeBackendObject not initialized!");
            return EGL_FALSE;
        }
        if (!backendObject->InitializeEGLDisplay(dpy, major, minor)) {
            state->SetError(EGL_NOT_INITIALIZED);
            return EGL_FALSE;
        }
        return EGL_TRUE;
    }

    EGLDisplay GetDisplay(NativeDisplayType display) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_DISPLAY;
        }
        return state->GetDisplay(display);
    }

    EGLint GetError() {
        auto* state = GetState();
        if (!state) {
            return EGL_NOT_INITIALIZED;
        }
        return state->ConsumeError();
    }

    EGLBoolean MakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }

        const auto oldDisplay = state->GetCurrentDisplay();
        const auto oldDraw = state->GetCurrentSurface(EGL_DRAW);
        const auto oldRead = state->GetCurrentSurface(EGL_READ);
        const auto oldContext = state->GetCurrentContext();

        if (!state->MakeCurrent(dpy, draw, read, ctx)) {
            return EGL_FALSE;
        }

        const Bool releaseCurrentRequest =
            dpy == EGL_NO_DISPLAY && draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE && ctx == EGL_NO_CONTEXT;
        if (releaseCurrentRequest) {
            if (auto* backendObject = MG_Backend::pActiveBackendObject.get()) {
                (void)backendObject->MakeEGLCurrent(dpy, draw, read, ctx);
            }
            return EGL_TRUE;
        }

        auto* backendObject = GetBackendObject(state);
        if (!backendObject) {
            MGLOG_E("activeBackendObject not initialized!");
            state->MakeCurrent(oldDisplay, oldDraw, oldRead, oldContext);
            return EGL_FALSE;
        }
        if (!backendObject->MakeEGLCurrent(dpy, draw, read, ctx)) {
            state->SetError(EGL_BAD_ACCESS);
            state->MakeCurrent(oldDisplay, oldDraw, oldRead, oldContext);
            return EGL_FALSE;
        }
        return EGL_TRUE;
    }

    EGLBoolean DestroyContext(EGLDisplay dpy, EGLContext ctx) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->DestroyContext(dpy, ctx) ? EGL_TRUE : EGL_FALSE;
    }

    EGLBoolean DestroySurface(EGLDisplay dpy, EGLSurface surface) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->DestroySurface(dpy, surface) ? EGL_TRUE : EGL_FALSE;
    }

    EGLBoolean Terminate(EGLDisplay dpy) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->TerminateDisplay(dpy)) {
            return EGL_FALSE;
        }
        if (auto* backendObject = MG_Backend::pActiveBackendObject.get()) {
            backendObject->ReleaseEGLResources();
        }
        return EGL_TRUE;
    }

    EGLBoolean ReleaseThread() {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (auto* backendObject = MG_Backend::pActiveBackendObject.get()) {
            (void)backendObject->MakeEGLCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        state->ReleaseThread();
        return EGL_TRUE;
    }

    EGLContext GetCurrentContext() {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_CONTEXT;
        }
        return state->GetCurrentContext();
    }

    EGLBoolean GetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint* value) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->GetConfigAttrib(dpy, config, attribute, value) ? EGL_TRUE : EGL_FALSE;
    }

    EGLBoolean BindAPI(EGLenum api) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        switch (api) {
        case EGL_OPENGL_API:
        case EGL_OPENGL_ES_API:
        case EGL_OPENVG_API:
            state->SetBoundAPI(api);
            return EGL_TRUE;
        default:
            state->SetError(EGL_BAD_PARAMETER);
            return EGL_FALSE;
        }
    }

    EGLSurface GetCurrentSurface(EGLint readdraw) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SURFACE;
        }
        return state->GetCurrentSurface(readdraw);
    }

    EGLBoolean QuerySurface(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint* value) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->QuerySurface(display, surface, attribute, value) ? EGL_TRUE : EGL_FALSE;
    }

    char const* QueryString(EGLDisplay display, EGLint name) {
        auto* state = GetState();
        if (!state) {
            return nullptr;
        }

        if (display != EGL_NO_DISPLAY && !state->ValidateDisplay(display)) {
            state->SetError(EGL_BAD_DISPLAY);
            return nullptr;
        }

        switch (name) {
        case EGL_VENDOR:
            return "MobileGL";
        case EGL_VERSION:
            return "1.5 MobileGL";
        case EGL_CLIENT_APIS:
            return "OpenGL OpenGL_ES";
        case EGL_EXTENSIONS:
            return "";
        default:
            state->SetError(EGL_BAD_PARAMETER);
            return nullptr;
        }
    }

    EGLBoolean SwapInterval(EGLDisplay dpy, EGLint interval) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->SwapInterval(dpy, interval) ? EGL_TRUE : EGL_FALSE;
    }

    EGLSurface CreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SURFACE;
        }
        const EGLint width = GetAttribValue(attrib_list, EGL_WIDTH, 1);
        const EGLint height = GetAttribValue(attrib_list, EGL_HEIGHT, 1);
        EGLSurface surface = state->CreatePbufferSurface(dpy, config, attrib_list);
        if (surface == EGL_NO_SURFACE) {
            return EGL_NO_SURFACE;
        }

        auto* backendObject = GetBackendObject(state);
        if (!backendObject) {
            return EGL_NO_SURFACE;
        }
        if (!backendObject->CreateEGLPbufferSurface(width, height)) {
            state->DestroySurface(dpy, surface);
            state->SetError(EGL_BAD_ALLOC);
            return EGL_NO_SURFACE;
        }

        return surface;
    }

    EGLBoolean BindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->ValidateSurfaceOnDisplay(dpy, surface)) {
            state->SetError(EGL_BAD_SURFACE);
            return EGL_FALSE;
        }
        if (buffer != EGL_BACK_BUFFER) {
            state->SetError(EGL_BAD_PARAMETER);
            return EGL_FALSE;
        }
        return EGL_TRUE;
    }

    EGLBoolean ReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->ValidateSurfaceOnDisplay(dpy, surface)) {
            state->SetError(EGL_BAD_SURFACE);
            return EGL_FALSE;
        }
        if (buffer != EGL_BACK_BUFFER) {
            state->SetError(EGL_BAD_PARAMETER);
            return EGL_FALSE;
        }
        return EGL_TRUE;
    }

    EGLBoolean CopyBuffers(EGLDisplay dpy, EGLSurface surface, EGLNativePixmapType target) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->ValidateSurfaceOnDisplay(dpy, surface)) {
            state->SetError(EGL_BAD_SURFACE);
            return EGL_FALSE;
        }
        if (IsNullNativeHandle(target)) {
            state->SetError(EGL_BAD_NATIVE_PIXMAP);
            return EGL_FALSE;
        }
        return EGL_TRUE;
    }

    EGLSurface CreatePbufferFromClientBuffer(EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer, EGLConfig config,
                                             const EGLint* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SURFACE;
        }
        return state->CreatePbufferFromClientBuffer(dpy, buftype, buffer, config, attrib_list);
    }

    EGLSurface CreatePixmapSurface(EGLDisplay dpy, EGLConfig config, EGLNativePixmapType pixmap,
                                   const EGLint* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SURFACE;
        }
        return state->CreatePixmapSurface(dpy, config, pixmap, attrib_list);
    }

    EGLBoolean GetConfigs(EGLDisplay dpy, EGLConfig* configs, EGLint config_size, EGLint* num_config) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->GetConfigs(dpy, configs, config_size, num_config) ? EGL_TRUE : EGL_FALSE;
    }

    EGLDisplay GetCurrentDisplay() {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_DISPLAY;
        }
        return state->GetCurrentDisplay();
    }

    EGLenum QueryAPI() {
        auto* state = GetState();
        if (!state) {
            return EGL_OPENGL_API;
        }
        return state->GetBoundAPI();
    }

    EGLBoolean QueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint* value) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->QueryContext(dpy, ctx, attribute, value) ? EGL_TRUE : EGL_FALSE;
    }

    EGLBoolean SurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value) {
        (void)value;

        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->ValidateSurfaceOnDisplay(dpy, surface)) {
            state->SetError(EGL_BAD_SURFACE);
            return EGL_FALSE;
        }

        switch (attribute) {
        case EGL_MIPMAP_LEVEL:
        case EGL_SWAP_BEHAVIOR:
        case EGL_TEXTURE_FORMAT:
        case EGL_TEXTURE_TARGET:
        case EGL_MIPMAP_TEXTURE:
            return EGL_TRUE;
        default:
            state->SetError(EGL_BAD_ATTRIBUTE);
            return EGL_FALSE;
        }
    }

    EGLBoolean WaitClient() {
        return EGL_TRUE;
    }

    EGLBoolean WaitGL() {
        return EGL_TRUE;
    }

    EGLBoolean WaitNative(EGLint engine) {
        (void)engine;
        return EGL_TRUE;
    }

    EGLSync CreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SYNC;
        }
        return state->CreateSync(dpy, type, attrib_list);
    }

    EGLBoolean DestroySync(EGLDisplay dpy, EGLSync sync) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->DestroySync(dpy, sync) ? EGL_TRUE : EGL_FALSE;
    }

    EGLint ClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->ClientWaitSync(dpy, sync, flags, timeout);
    }

    EGLBoolean GetSyncAttrib(EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib* value) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->GetSyncAttrib(dpy, sync, attribute, value) ? EGL_TRUE : EGL_FALSE;
    }

    EGLImage CreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
                         const EGLAttrib* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_IMAGE;
        }
        return state->CreateImage(dpy, ctx, target, buffer, attrib_list);
    }

    EGLBoolean DestroyImage(EGLDisplay dpy, EGLImage image) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->DestroyImage(dpy, image) ? EGL_TRUE : EGL_FALSE;
    }

    EGLDisplay GetPlatformDisplay(EGLenum platform, void* native_display, const EGLAttrib* attrib_list) {
        (void)attrib_list;

        auto* state = GetState();
        if (!state) {
            return EGL_NO_DISPLAY;
        }
        return state->GetPlatformDisplay(platform, native_display);
    }

    EGLSurface CreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void* native_window,
                                           const EGLAttrib* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SURFACE;
        }
        if (native_window == nullptr) {
            state->SetError(EGL_BAD_NATIVE_WINDOW);
            return EGL_NO_SURFACE;
        }
        if (!state->IsDisplayInitialized(dpy)) {
            state->SetError(EGL_NOT_INITIALIZED);
            return EGL_NO_SURFACE;
        }
        if (!state->ValidateConfigOnDisplay(dpy, config)) {
            state->SetError(EGL_BAD_CONFIG);
            return EGL_NO_SURFACE;
        }

        auto* backendObject = GetBackendObject(state);
        if (!backendObject) {
            MGLOG_E("activeBackendObject not initialized!");
            return EGL_NO_SURFACE;
        }

        const MG_Backend::WindowHandle windowHandle = {
            .Backend = DetectWindowBackend(),
            .Handle = native_window,
            .Width = static_cast<Uint32>(std::max<EGLint>(GetAttribValue(attrib_list, EGL_WIDTH, 0), 0)),
            .Height = static_cast<Uint32>(std::max<EGLint>(GetAttribValue(attrib_list, EGL_HEIGHT, 0), 0)),
        };
        if (!backendObject->CreateEGLWindowSurface(windowHandle)) {
            state->SetError(EGL_BAD_NATIVE_WINDOW);
            return EGL_NO_SURFACE;
        }

        return state->CreatePlatformWindowSurface(dpy, config, native_window, attrib_list);
    }

    EGLBoolean ResizePlatformWindowSurface(EGLDisplay dpy, EGLSurface surface, EGLint width, EGLint height) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->ResizeSurface(dpy, surface, width, height)) {
            return EGL_FALSE;
        }
        auto* backendObject = GetBackendObject(state);
        if (!backendObject) {
            MGLOG_E("activeBackendObject not initialized!");
            return EGL_FALSE;
        }
        width = std::max<EGLint>(width, 1);
        height = std::max<EGLint>(height, 1);
        if (!backendObject->ResizeEGLWindowSurface(static_cast<Uint32>(width), static_cast<Uint32>(height))) {
            state->SetError(EGL_BAD_NATIVE_WINDOW);
            return EGL_FALSE;
        }
        return EGL_TRUE;
    }

    EGLSurface CreatePlatformPixmapSurface(EGLDisplay dpy, EGLConfig config, void* native_pixmap,
                                           const EGLAttrib* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SURFACE;
        }
        return state->CreatePlatformPixmapSurface(dpy, config, native_pixmap, attrib_list);
    }

    EGLBoolean WaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->WaitSync(dpy, sync, flags) ? EGL_TRUE : EGL_FALSE;
    }

    __eglMustCastToProperFunctionPointerType GetProcAddress(const char* name) {
        if (!name) {
            return nullptr;
        }

        MGLOG_D("eglGetProcAddress(%s)", name);
        void* proc = MG_Impl::GetProcAddress(name);
        if (!proc) {
            MGLOG_W("Failed to get function: %s", name);
            return nullptr;
        }
        return (__eglMustCastToProperFunctionPointerType)proc;
    }
} // namespace MobileGL::MG_Impl::EGLImpl
