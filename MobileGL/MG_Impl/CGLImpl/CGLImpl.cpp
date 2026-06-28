// MobileGL - MobileGL/MG_Impl/CGLImpl/CGLImpl.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "CGLImpl.h"

#if defined(__APPLE__)
#include "../EGLImpl/EGLImpl.h"

namespace MobileGL::MG_Impl::CGLImpl {
    namespace {
        struct PixelFormatObject {
            Uint32 RetainCount = 1;
            Bool DoubleBuffer = true;
            GLint ColorSize = 24;
            GLint AlphaSize = 8;
            GLint DepthSize = 24;
            GLint StencilSize = 8;
            GLint SampleBuffers = 0;
            GLint Samples = 0;
            GLint Profile = kCGLOGLPVersion_3_2_Core;
            GLint RendererId = 0x4d474c;
        };

        struct ContextObject {
            Uint32 RetainCount = 1;
            CGLPixelFormatObj PixelFormat = nullptr;
            CGLContextObj Share = nullptr;
            EGLDisplay Display = EGL_NO_DISPLAY;
            EGLConfig Config = nullptr;
            EGLContext Context = EGL_NO_CONTEXT;
            EGLSurface Surface = EGL_NO_SURFACE;
            void* NSObject = nullptr;
            void* View = nullptr;
            void* MetalLayer = nullptr;
            GLint SwapInterval = 1;
            GLint VirtualScreen = 0;
            GLint SurfaceBackingSize[2] = {0, 0};
            Bool HasDrawable = false;
            Bool Locked = false;
        };

        std::recursive_mutex& RegistryMutex() {
            static auto* mutex = new std::recursive_mutex();
            return *mutex;
        }

        Uint64& NextPixelFormatHandle() {
            static auto* handle = new Uint64(1);
            return *handle;
        }

        Uint64& NextContextHandle() {
            static auto* handle = new Uint64(1);
            return *handle;
        }

        UnorderedMap<CGLPixelFormatObj, PixelFormatObject>& PixelFormats() {
            static auto* formats = new UnorderedMap<CGLPixelFormatObj, PixelFormatObject>();
            return *formats;
        }

        UnorderedMap<CGLContextObj, ContextObject>& Contexts() {
            static auto* contexts = new UnorderedMap<CGLContextObj, ContextObject>();
            return *contexts;
        }

        UnorderedMap<std::thread::id, CGLContextObj>& CurrentContexts() {
            static auto* contexts = new UnorderedMap<std::thread::id, CGLContextObj>();
            return *contexts;
        }

        CGLPixelFormatObj EncodePixelFormat(Uint64 handle) {
            return reinterpret_cast<CGLPixelFormatObj>(static_cast<SizeT>(handle));
        }

        CGLContextObj EncodeContext(Uint64 handle) {
            return reinterpret_cast<CGLContextObj>(static_cast<SizeT>(handle));
        }

        std::thread::id CurrentThreadKey() {
            return std::this_thread::get_id();
        }

        Bool AttributeHasValue(CGLPixelFormatAttribute attrib) {
            switch (attrib) {
            case kCGLPFAColorSize:
            case kCGLPFAAlphaSize:
            case kCGLPFADepthSize:
            case kCGLPFAStencilSize:
            case kCGLPFASampleBuffers:
            case kCGLPFASamples:
            case kCGLPFARendererID:
            case kCGLPFADisplayMask:
            case kCGLPFAOpenGLProfile:
                return true;
            default:
                return false;
            }
        }

        void ApplyPixelFormatAttribute(PixelFormatObject& pixelFormat,
                                       CGLPixelFormatAttribute attrib,
                                       GLint value) {
            switch (attrib) {
            case kCGLPFADoubleBuffer:
                pixelFormat.DoubleBuffer = true;
                break;
            case kCGLPFAColorSize:
                pixelFormat.ColorSize = value;
                break;
            case kCGLPFAAlphaSize:
                pixelFormat.AlphaSize = value;
                break;
            case kCGLPFADepthSize:
                pixelFormat.DepthSize = value;
                break;
            case kCGLPFAStencilSize:
                pixelFormat.StencilSize = value;
                break;
            case kCGLPFASampleBuffers:
                pixelFormat.SampleBuffers = value;
                break;
            case kCGLPFASamples:
                pixelFormat.Samples = value;
                break;
            case kCGLPFAOpenGLProfile:
                pixelFormat.Profile = value;
                break;
            case kCGLPFARendererID:
                pixelFormat.RendererId = value;
                break;
            default:
                break;
            }
        }

        Bool InitEGLContext(ContextObject& object, CGLPixelFormatObj pix, CGLContextObj share) {
            auto* pixelFormat = [&]() -> PixelFormatObject* {
                auto& pixelFormats = PixelFormats();
                auto it = pixelFormats.find(pix);
                return it == pixelFormats.end() ? nullptr : &it->second;
            }();
            if (!pixelFormat) {
                return false;
            }

            EGLDisplay display = EGLImpl::GetDisplay(EGL_DEFAULT_DISPLAY);
            if (display == EGL_NO_DISPLAY) {
                return false;
            }
            if (!EGLImpl::Initialize(display, nullptr, nullptr)) {
                return false;
            }
            EGLImpl::BindAPI(EGL_OPENGL_API);

            const EGLint attribs[] = {
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_ALPHA_SIZE, std::max(pixelFormat->AlphaSize, 0),
                EGL_DEPTH_SIZE, std::max(pixelFormat->DepthSize, 0),
                EGL_STENCIL_SIZE, std::max(pixelFormat->StencilSize, 0),
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                EGL_NONE,
            };

            EGLConfig config = nullptr;
            EGLint count = 0;
            if (!EGLImpl::ChooseConfig(display, attribs, &config, 1, &count) || count <= 0) {
                return false;
            }

            EGLContext shareContext = EGL_NO_CONTEXT;
            if (share != nullptr) {
                auto& contexts = Contexts();
                auto shareIt = contexts.find(share);
                if (shareIt == contexts.end()) {
                    return false;
                }
                shareContext = shareIt->second.Context;
            }

            const EGLint contextAttribs[] = {
                EGL_CONTEXT_MAJOR_VERSION, 3,
                EGL_CONTEXT_MINOR_VERSION, 3,
                EGL_NONE,
            };
            EGLContext eglContext = EGLImpl::CreateContext(display, config, shareContext, contextAttribs);
            if (eglContext == EGL_NO_CONTEXT) {
                return false;
            }

            object.Display = display;
            object.Config = config;
            object.Context = eglContext;
            object.PixelFormat = pix;
            object.Share = share;
            return true;
        }

        ContextObject* TryGetContext(CGLContextObj ctx) {
            auto& contexts = Contexts();
            auto it = contexts.find(ctx);
            return it == contexts.end() ? nullptr : &it->second;
        }

        const ContextObject* TryGetContext(CGLContextObj ctx, const std::lock_guard<std::recursive_mutex>&) {
            auto& contexts = Contexts();
            auto it = contexts.find(ctx);
            return it == contexts.end() ? nullptr : &it->second;
        }

        PixelFormatObject* TryGetPixelFormat(CGLPixelFormatObj pix) {
            auto& pixelFormats = PixelFormats();
            auto it = pixelFormats.find(pix);
            return it == pixelFormats.end() ? nullptr : &it->second;
        }

        CGLError MakeCurrentLocked(CGLContextObj ctx, ContextObject& object) {
            CurrentContexts()[CurrentThreadKey()] = ctx;
            if (!object.HasDrawable || object.Surface == EGL_NO_SURFACE) {
                return kCGLNoError;
            }
            if (!EGLImpl::MakeCurrent(object.Display, object.Surface, object.Surface, object.Context)) {
                return kCGLBadState;
            }
            return kCGLNoError;
        }

        CGLError RecreateSurfaceLocked(CGLContextObj ctx, ContextObject& object) {
            if (!object.MetalLayer) {
                return kCGLBadDrawable;
            }
            if (object.Surface != EGL_NO_SURFACE) {
                EGLImpl::DestroySurface(object.Display, object.Surface);
                object.Surface = EGL_NO_SURFACE;
            }
            const EGLAttrib attribs[] = {
                EGL_WIDTH, std::max<GLint>(object.SurfaceBackingSize[0], 1),
                EGL_HEIGHT, std::max<GLint>(object.SurfaceBackingSize[1], 1),
                EGL_NONE,
            };
            EGLSurface surface = EGLImpl::CreatePlatformWindowSurface(object.Display, object.Config,
                                                                      object.MetalLayer, attribs);
            if (surface == EGL_NO_SURFACE) {
                object.HasDrawable = false;
                return kCGLBadDrawable;
            }
            object.Surface = surface;
            object.HasDrawable = true;
            return GetCurrentContext() == ctx ? MakeCurrentLocked(ctx, object) : kCGLNoError;
        }
    } // namespace

    CGLError ChoosePixelFormat(const CGLPixelFormatAttribute* attribs, CGLPixelFormatObj* pix, GLint* npix) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        if (!pix || !npix) {
            return kCGLBadAddress;
        }

        PixelFormatObject object;
        if (attribs) {
            for (SizeT i = 0; attribs[i] != static_cast<CGLPixelFormatAttribute>(0); ++i) {
                const auto attrib = attribs[i];
                GLint value = 1;
                if (AttributeHasValue(attrib)) {
                    value = static_cast<GLint>(attribs[++i]);
                }
                ApplyPixelFormatAttribute(object, attrib, value);
            }
        }

        const auto handle = EncodePixelFormat(NextPixelFormatHandle()++);
        PixelFormats()[handle] = object;
        *pix = handle;
        *npix = 1;
        return kCGLNoError;
    }

    CGLError DestroyPixelFormat(CGLPixelFormatObj pix) {
        ReleasePixelFormat(pix);
        return kCGLNoError;
    }

    CGLError DescribePixelFormat(CGLPixelFormatObj pix, GLint pixNum, CGLPixelFormatAttribute attrib, GLint* value) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        if (!value) {
            return kCGLBadAddress;
        }
        if (pixNum != 0 && pixNum != 1) {
            return kCGLBadValue;
        }
        auto* pixelFormat = TryGetPixelFormat(pix);
        if (!pixelFormat) {
            return kCGLBadPixelFormat;
        }

        switch (attrib) {
        case kCGLPFADoubleBuffer:
            *value = pixelFormat->DoubleBuffer ? 1 : 0;
            return kCGLNoError;
        case kCGLPFAAccelerated:
        case kCGLPFAAcceleratedCompute:
        case kCGLPFASupportsAutomaticGraphicsSwitching:
            *value = 1;
            return kCGLNoError;
        case kCGLPFAColorSize:
            *value = pixelFormat->ColorSize;
            return kCGLNoError;
        case kCGLPFAAlphaSize:
            *value = pixelFormat->AlphaSize;
            return kCGLNoError;
        case kCGLPFADepthSize:
            *value = pixelFormat->DepthSize;
            return kCGLNoError;
        case kCGLPFAStencilSize:
            *value = pixelFormat->StencilSize;
            return kCGLNoError;
        case kCGLPFASampleBuffers:
            *value = pixelFormat->SampleBuffers;
            return kCGLNoError;
        case kCGLPFASamples:
            *value = pixelFormat->Samples;
            return kCGLNoError;
        case kCGLPFARendererID:
            *value = pixelFormat->RendererId;
            return kCGLNoError;
        case kCGLPFAOpenGLProfile:
            *value = pixelFormat->Profile;
            return kCGLNoError;
        case kCGLPFAVirtualScreenCount:
            *value = 1;
            return kCGLNoError;
        default:
            *value = 0;
            return kCGLNoError;
        }
    }

    void ReleasePixelFormat(CGLPixelFormatObj pix) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* pixelFormat = TryGetPixelFormat(pix);
        if (!pixelFormat) {
            return;
        }
        if (pixelFormat->RetainCount > 1) {
            --pixelFormat->RetainCount;
            return;
        }
        PixelFormats().erase(pix);
    }

    CGLPixelFormatObj RetainPixelFormat(CGLPixelFormatObj pix) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* pixelFormat = TryGetPixelFormat(pix);
        if (pixelFormat) {
            ++pixelFormat->RetainCount;
        }
        return pix;
    }

    GLuint GetPixelFormatRetainCount(CGLPixelFormatObj pix) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* pixelFormat = TryGetPixelFormat(pix);
        return pixelFormat ? pixelFormat->RetainCount : 0;
    }

    CGLError CreateContext(CGLPixelFormatObj pix, CGLContextObj share, CGLContextObj* ctx) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        if (!ctx) {
            return kCGLBadAddress;
        }
        if (!TryGetPixelFormat(pix)) {
            return kCGLBadPixelFormat;
        }
        if (share && !TryGetContext(share)) {
            return kCGLBadMatch;
        }

        ContextObject object;
        if (!InitEGLContext(object, pix, share)) {
            return kCGLBadAlloc;
        }
        RetainPixelFormat(pix);
        const auto handle = EncodeContext(NextContextHandle()++);
        Contexts()[handle] = object;
        *ctx = handle;
        return kCGLNoError;
    }

    CGLError DestroyContext(CGLContextObj ctx) {
        ReleaseContext(ctx);
        return kCGLNoError;
    }

    CGLContextObj RetainContext(CGLContextObj ctx) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(ctx);
        if (object) {
            ++object->RetainCount;
        }
        return ctx;
    }

    void ReleaseContext(CGLContextObj ctx) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(ctx);
        if (!object) {
            return;
        }
        if (object->RetainCount > 1) {
            --object->RetainCount;
            return;
        }
        if (object->Surface != EGL_NO_SURFACE) {
            EGLImpl::DestroySurface(object->Display, object->Surface);
        }
        if (object->Context != EGL_NO_CONTEXT) {
            EGLImpl::DestroyContext(object->Display, object->Context);
        }
        ReleasePixelFormat(object->PixelFormat);
        auto& currentContexts = CurrentContexts();
        for (auto it = currentContexts.begin(); it != currentContexts.end();) {
            if (it->second == ctx) {
                it = currentContexts.erase(it);
            } else {
                ++it;
            }
        }
        Contexts().erase(ctx);
    }

    GLuint GetContextRetainCount(CGLContextObj ctx) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(ctx);
        return object ? object->RetainCount : 0;
    }

    CGLPixelFormatObj GetPixelFormat(CGLContextObj ctx) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(ctx);
        return object ? object->PixelFormat : nullptr;
    }

    CGLError SetCurrentContext(CGLContextObj ctx) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        if (!ctx) {
            CurrentContexts().erase(CurrentThreadKey());
            EGLImpl::MakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            return kCGLNoError;
        }
        auto* object = TryGetContext(ctx);
        if (!object) {
            return kCGLBadContext;
        }
        return MakeCurrentLocked(ctx, *object);
    }

    CGLContextObj GetCurrentContext() {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto& currentContexts = CurrentContexts();
        auto it = currentContexts.find(CurrentThreadKey());
        return it == currentContexts.end() ? nullptr : it->second;
    }

    CGLError SetParameter(CGLContextObj ctx, CGLContextParameter pname, const GLint* params) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(ctx);
        if (!object) {
            return kCGLBadContext;
        }
        if (!params && pname != kCGLCPReclaimResources) {
            return kCGLBadAddress;
        }
        switch (pname) {
        case kCGLCPSwapInterval:
            object->SwapInterval = params[0];
            EGLImpl::SwapInterval(object->Display, object->SwapInterval);
            return kCGLNoError;
        case kCGLCPSurfaceBackingSize:
        {
            const GLint width = std::max<GLint>(params[0], 1);
            const GLint height = std::max<GLint>(params[1], 1);
            if (object->SurfaceBackingSize[0] == width && object->SurfaceBackingSize[1] == height) {
                return kCGLNoError;
            }
            object->SurfaceBackingSize[0] = width;
            object->SurfaceBackingSize[1] = height;
            if (object->MetalLayer) {
                return RecreateSurfaceLocked(ctx, *object);
            }
            return kCGLNoError;
        }
        case kCGLCPSurfaceOpacity:
        case kCGLCPSurfaceOrder:
        case kCGLCPMPSwapsInFlight:
        case kCGLCPReclaimResources:
            return kCGLNoError;
        default:
            return kCGLNoError;
        }
    }

    CGLError GetParameter(CGLContextObj ctx, CGLContextParameter pname, GLint* params) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(ctx);
        if (!object) {
            return kCGLBadContext;
        }
        if (!params) {
            return kCGLBadAddress;
        }
        switch (pname) {
        case kCGLCPSwapInterval:
            params[0] = object->SwapInterval;
            return kCGLNoError;
        case kCGLCPSurfaceBackingSize:
            params[0] = object->SurfaceBackingSize[0];
            params[1] = object->SurfaceBackingSize[1];
            return kCGLNoError;
        case kCGLCPCurrentRendererID:
            params[0] = 0x4d474c;
            return kCGLNoError;
        case kCGLCPGPUVertexProcessing:
        case kCGLCPGPUFragmentProcessing:
        case kCGLCPHasDrawable:
            params[0] = object->HasDrawable ? 1 : 0;
            return kCGLNoError;
        case kCGLCPMPSwapsInFlight:
            params[0] = 1;
            return kCGLNoError;
        default:
            params[0] = 0;
            return kCGLNoError;
        }
    }

    CGLError UpdateContext(CGLContextObj ctx) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        return TryGetContext(ctx) ? kCGLNoError : kCGLBadContext;
    }

    CGLError ClearDrawable(CGLContextObj ctx) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(ctx);
        if (!object) {
            return kCGLBadContext;
        }
        if (object->Surface != EGL_NO_SURFACE) {
            EGLImpl::DestroySurface(object->Display, object->Surface);
        }
        object->Surface = EGL_NO_SURFACE;
        object->View = nullptr;
        object->MetalLayer = nullptr;
        object->HasDrawable = false;
        return kCGLNoError;
    }

    CGLError FlushDrawable(CGLContextObj ctx) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(ctx);
        if (!object) {
            return kCGLBadContext;
        }
        if (!object->HasDrawable || object->Surface == EGL_NO_SURFACE) {
            return kCGLBadDrawable;
        }
        const auto currentError = MakeCurrentLocked(ctx, *object);
        if (currentError != kCGLNoError) {
            return currentError;
        }
        return EGLImpl::SwapBuffers(object->Display, object->Surface) ? kCGLNoError : kCGLBadDrawable;
    }

    CGLError LockContext(CGLContextObj ctx) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(ctx);
        if (!object) {
            return kCGLBadContext;
        }
        object->Locked = true;
        return kCGLNoError;
    }

    CGLError UnlockContext(CGLContextObj ctx) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(ctx);
        if (!object) {
            return kCGLBadContext;
        }
        object->Locked = false;
        return kCGLNoError;
    }

    void GetVersion(GLint* majorvers, GLint* minorvers) {
        if (majorvers) {
            *majorvers = 1;
        }
        if (minorvers) {
            *minorvers = 0;
        }
    }

    const char* ErrorString(CGLError error) {
        switch (error) {
        case kCGLNoError:
            return "no error";
        case kCGLBadAttribute:
            return "invalid pixel format attribute";
        case kCGLBadPixelFormat:
            return "invalid pixel format";
        case kCGLBadContext:
            return "invalid context";
        case kCGLBadDrawable:
            return "invalid drawable";
        case kCGLBadState:
            return "invalid context state";
        case kCGLBadValue:
            return "invalid numerical value";
        case kCGLBadMatch:
            return "invalid share context";
        case kCGLBadAddress:
            return "invalid pointer";
        case kCGLBadAlloc:
            return "invalid memory allocation";
        default:
            return "unknown CGL error";
        }
    }

    CGLError AttachDrawable(CGLContextObj ctx, void* nsView, void* metalLayer, GLint width, GLint height) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(ctx);
        if (!object) {
            return kCGLBadContext;
        }
        if (!metalLayer) {
            return kCGLBadDrawable;
        }
        width = std::max<GLint>(width, 1);
        height = std::max<GLint>(height, 1);
        const Bool sameSize = object->SurfaceBackingSize[0] == width && object->SurfaceBackingSize[1] == height;
        object->SurfaceBackingSize[0] = width;
        object->SurfaceBackingSize[1] = height;
        if (object->Surface != EGL_NO_SURFACE && object->MetalLayer == metalLayer && sameSize) {
            object->View = nsView;
            object->HasDrawable = true;
            return kCGLNoError;
        }
        if (object->Surface != EGL_NO_SURFACE) {
            EGLImpl::DestroySurface(object->Display, object->Surface);
            object->Surface = EGL_NO_SURFACE;
        }
        object->View = nsView;
        object->MetalLayer = metalLayer;
        const auto recreateError = RecreateSurfaceLocked(ctx, *object);
        if (recreateError != kCGLNoError) {
            return recreateError;
        }
        return kCGLNoError;
    }

    void* GetContextNSObject(CGLContextObj ctx) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(ctx);
        return object ? object->NSObject : nullptr;
    }

    void SetContextNSObject(CGLContextObj ctx, void* nsObject) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(ctx);
        if (object) {
            object->NSObject = nsObject;
        }
    }
} // namespace MobileGL::MG_Impl::CGLImpl
#endif
