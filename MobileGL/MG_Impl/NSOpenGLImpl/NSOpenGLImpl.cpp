// MobileGL - MobileGL/MG_Impl/NSOpenGLImpl/NSOpenGLImpl.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "NSOpenGLImpl.h"

#if defined(__APPLE__)
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#include "../CGLImpl/CGLImpl.h"
#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

namespace MobileGL::MG_Impl::NSOpenGLImpl {
    namespace {
        constexpr intptr_t kAssociationRetain = 1;
        constexpr intptr_t kAssociationAssign = 0;

        char kPixelFormatHandleKey;
        char kContextHandleKey;
        char kContextPixelFormatObjectKey;
        char kContextViewKey;
        char kContextLayerKey;

        std::once_flag g_installOnce;
        IMP g_pixelFormatDealloc = nullptr;
        IMP g_contextDealloc = nullptr;

        template <typename Fn>
        Fn ObjcMsgSend() {
            return reinterpret_cast<Fn>(objc_msgSend);
        }

        id SendId(id receiver, const char* selector) {
            return receiver ? ObjcMsgSend<id (*)(id, SEL)>()(receiver, sel_registerName(selector)) : nil;
        }

        void SendVoid(id receiver, const char* selector) {
            if (receiver) {
                ObjcMsgSend<void (*)(id, SEL)>()(receiver, sel_registerName(selector));
            }
        }

        void SendVoidBool(id receiver, const char* selector, bool value) {
            if (receiver) {
                ObjcMsgSend<void (*)(id, SEL, bool)>()(receiver, sel_registerName(selector), value);
            }
        }

        void SendVoidId(id receiver, const char* selector, id value) {
            if (receiver) {
                ObjcMsgSend<void (*)(id, SEL, id)>()(receiver, sel_registerName(selector), value);
            }
        }

        id SendIdCGLPixelFormat(id receiver, const char* selector, CGLPixelFormatObj value) {
            return receiver ? ObjcMsgSend<id (*)(id, SEL, CGLPixelFormatObj)>()(receiver, sel_registerName(selector), value)
                            : nil;
        }

        void SendVoidCGRect(id receiver, const char* selector, CGRect value) {
            if (receiver) {
                ObjcMsgSend<void (*)(id, SEL, CGRect)>()(receiver, sel_registerName(selector), value);
            }
        }

        void SendVoidCGSize(id receiver, const char* selector, CGSize value) {
            if (receiver) {
                ObjcMsgSend<void (*)(id, SEL, CGSize)>()(receiver, sel_registerName(selector), value);
            }
        }

        void SendVoidCGFloat(id receiver, const char* selector, CGFloat value) {
            if (receiver) {
                ObjcMsgSend<void (*)(id, SEL, CGFloat)>()(receiver, sel_registerName(selector), value);
            }
        }

        bool SendBool(id receiver, const char* selector) {
            return receiver ? ObjcMsgSend<bool (*)(id, SEL)>()(receiver, sel_registerName(selector)) : false;
        }

        CGRect SendCGRect(id receiver, const char* selector) {
            if (!receiver) {
                return {};
            }
            return ObjcMsgSend<CGRect (*)(id, SEL)>()(receiver, sel_registerName(selector));
        }

        CGRect SendCGRectCGRect(id receiver, const char* selector, CGRect value) {
            if (!receiver) {
                return {};
            }
            return ObjcMsgSend<CGRect (*)(id, SEL, CGRect)>()(receiver, sel_registerName(selector), value);
        }

        CGFloat SendCGFloat(id receiver, const char* selector) {
            return receiver ? ObjcMsgSend<CGFloat (*)(id, SEL)>()(receiver, sel_registerName(selector)) : 1.0;
        }

        void SetAssoc(id object, const void* key, id value, intptr_t policy) {
            objc_setAssociatedObject(object, key, value, static_cast<objc_AssociationPolicy>(policy));
        }

        id GetAssoc(id object, const void* key) {
            return object ? objc_getAssociatedObject(object, key) : nil;
        }

        CGLPixelFormatObj GetPixelFormatHandle(id object) {
            return reinterpret_cast<CGLPixelFormatObj>(GetAssoc(object, &kPixelFormatHandleKey));
        }

        void SetPixelFormatHandle(id object, CGLPixelFormatObj handle) {
            SetAssoc(object, &kPixelFormatHandleKey, reinterpret_cast<id>(handle), kAssociationAssign);
        }

        CGLContextObj GetContextHandle(id object) {
            return reinterpret_cast<CGLContextObj>(GetAssoc(object, &kContextHandleKey));
        }

        void SetContextHandle(id object, CGLContextObj handle) {
            SetAssoc(object, &kContextHandleKey, reinterpret_cast<id>(handle), kAssociationAssign);
        }

        id PixelFormatInitWithAttributes(id self, SEL, const CGLPixelFormatAttribute* attribs) {
            CGLPixelFormatObj pixelFormat = nullptr;
            GLint count = 0;
            if (CGLImpl::ChoosePixelFormat(attribs, &pixelFormat, &count) != kCGLNoError) {
                return nil;
            }
            SetPixelFormatHandle(self, pixelFormat);
            return self;
        }

        id PixelFormatInitWithCGLPixelFormatObj(id self, SEL, CGLPixelFormatObj format) {
            if (!format) {
                return nil;
            }
            SetPixelFormatHandle(self, CGLImpl::RetainPixelFormat(format));
            return self;
        }

        void PixelFormatGetValues(id self, SEL, GLint* vals, CGLPixelFormatAttribute attrib, GLint screen) {
            (void)screen;
            if (!vals) {
                return;
            }
            GLint value = 0;
            if (CGLImpl::DescribePixelFormat(GetPixelFormatHandle(self), 0, attrib, &value) == kCGLNoError) {
                *vals = value;
            }
        }

        GLint PixelFormatNumberOfVirtualScreens(id, SEL) {
            return 1;
        }

        CGLPixelFormatObj PixelFormatCGLPixelFormatObj(id self, SEL) {
            return GetPixelFormatHandle(self);
        }

        void PixelFormatDealloc(id self, SEL selector) {
            if (auto format = GetPixelFormatHandle(self)) {
                CGLImpl::ReleasePixelFormat(format);
                SetPixelFormatHandle(self, nullptr);
            }
            if (g_pixelFormatDealloc) {
                reinterpret_cast<void (*)(id, SEL)>(g_pixelFormatDealloc)(self, selector);
            }
        }

        id ContextInitWithFormatShare(id self, SEL, id format, id share) {
            auto pixelFormat = GetPixelFormatHandle(format);
            if (!pixelFormat) {
                return nil;
            }
            auto shareContext = GetContextHandle(share);
            CGLContextObj context = nullptr;
            if (CGLImpl::CreateContext(pixelFormat, shareContext, &context) != kCGLNoError) {
                return nil;
            }
            CGLImpl::SetContextNSObject(context, self);
            SetContextHandle(self, context);
            SetAssoc(self, &kContextPixelFormatObjectKey, format, kAssociationRetain);
            SetAssoc(self, &kContextViewKey, nil, kAssociationAssign);
            SetAssoc(self, &kContextLayerKey, nil, kAssociationRetain);
            return self;
        }

        id ContextInitWithCGLContextObj(id self, SEL, CGLContextObj context) {
            if (!context) {
                return nil;
            }
            CGLImpl::RetainContext(context);
            CGLImpl::SetContextNSObject(context, self);
            SetContextHandle(self, context);
            if (auto pixelFormat = CGLImpl::GetPixelFormat(context)) {
                id pixelFormatClass = reinterpret_cast<id>(objc_getClass("NSOpenGLPixelFormat"));
                id allocated = SendId(pixelFormatClass, "alloc");
                id pixelFormatObject = SendIdCGLPixelFormat(allocated, "initWithCGLPixelFormatObj:", pixelFormat);
                SetAssoc(self, &kContextPixelFormatObjectKey, pixelFormatObject, kAssociationRetain);
                SendVoid(pixelFormatObject, "release");
            }
            SetAssoc(self, &kContextViewKey, nil, kAssociationAssign);
            SetAssoc(self, &kContextLayerKey, nil, kAssociationRetain);
            return self;
        }

        struct DrawableGeometry {
            CGRect Bounds = {};
            CGSize DrawableSize = {1.0, 1.0};
            CGFloat Scale = 1.0;
        };

        DrawableGeometry GetDrawableGeometry(id view) {
            DrawableGeometry geometry;
            geometry.Bounds = SendCGRect(view, "bounds");
            if (geometry.Bounds.size.width <= 0.0 || geometry.Bounds.size.height <= 0.0) {
                geometry.Bounds.size.width = 1.0;
                geometry.Bounds.size.height = 1.0;
            }

            id window = SendId(view, "window");
            if (window) {
                geometry.Scale = SendCGFloat(window, "backingScaleFactor");
            }
            if (geometry.Scale <= 0.0) {
                geometry.Scale = 1.0;
            }

            CGRect backingBounds = SendCGRectCGRect(view, "convertRectToBacking:", geometry.Bounds);
            if (backingBounds.size.width > 0.0 && backingBounds.size.height > 0.0) {
                geometry.DrawableSize = backingBounds.size;
            } else {
                geometry.DrawableSize = {geometry.Bounds.size.width * geometry.Scale,
                                         geometry.Bounds.size.height * geometry.Scale};
            }
            geometry.DrawableSize.width = std::max<CGFloat>(std::round(geometry.DrawableSize.width), 1.0);
            geometry.DrawableSize.height = std::max<CGFloat>(std::round(geometry.DrawableSize.height), 1.0);
            return geometry;
        }

        void ConfigureMetalLayerForView(id layer, id view, const DrawableGeometry& geometry) {
            SendVoidCGRect(layer, "setFrame:", geometry.Bounds);
            SendVoidCGFloat(layer, "setContentsScale:", geometry.Scale);
            SendVoidCGSize(layer, "setDrawableSize:", geometry.DrawableSize);
            SendVoidBool(layer, "setNeedsDisplayOnBoundsChange:", true);
        }

        id CreateMetalLayerForView(id view, DrawableGeometry* outGeometry) {
            if (!view) {
                return nil;
            }
            id metalLayerClass = reinterpret_cast<id>(objc_getClass("CAMetalLayer"));
            if (!metalLayerClass) {
                MGLOG_E("NSOpenGLImpl: CAMetalLayer class not found");
                return nil;
            }

            SendVoidBool(view, "setWantsLayer:", true);
            id layer = SendId(metalLayerClass, "layer");
            if (!layer) {
                return nil;
            }

            const auto geometry = GetDrawableGeometry(view);
            ConfigureMetalLayerForView(layer, view, geometry);
            SendVoidId(view, "setLayer:", layer);
            if (outGeometry) {
                *outGeometry = geometry;
            }
            return layer;
        }

        void ContextSetView(id self, SEL, id view) {
            auto context = GetContextHandle(self);
            if (!context) {
                return;
            }
            if (!view) {
                CGLImpl::ClearDrawable(context);
                SetAssoc(self, &kContextViewKey, nil, kAssociationAssign);
                SetAssoc(self, &kContextLayerKey, nil, kAssociationRetain);
                return;
            }
            DrawableGeometry geometry;
            id layer = CreateMetalLayerForView(view, &geometry);
            if (!layer) {
                return;
            }
            SetAssoc(self, &kContextViewKey, view, kAssociationAssign);
            SetAssoc(self, &kContextLayerKey, layer, kAssociationRetain);
            const auto error = CGLImpl::AttachDrawable(context, view, layer,
                                                       static_cast<GLint>(geometry.DrawableSize.width),
                                                       static_cast<GLint>(geometry.DrawableSize.height));
            if (error != kCGLNoError) {
                MGLOG_E("NSOpenGLImpl: failed to attach drawable: %s", CGLImpl::ErrorString(error));
            }
        }

        id ContextView(id self, SEL) {
            return GetAssoc(self, &kContextViewKey);
        }

        void ContextMakeCurrent(id self, SEL) {
            auto context = GetContextHandle(self);
            if (!context) {
                return;
            }
            const auto error = CGLImpl::SetCurrentContext(context);
            if (error != kCGLNoError) {
                MGLOG_E("NSOpenGLImpl: makeCurrentContext failed: %s", CGLImpl::ErrorString(error));
            }
        }

        void ContextClearCurrent(id, SEL) {
            CGLImpl::SetCurrentContext(nullptr);
        }

        id ContextCurrentContext(id, SEL) {
            auto context = CGLImpl::GetCurrentContext();
            return reinterpret_cast<id>(CGLImpl::GetContextNSObject(context));
        }

        void ContextFlushBuffer(id self, SEL) {
            auto context = GetContextHandle(self);
            if (!context) {
                return;
            }
            const auto error = CGLImpl::FlushDrawable(context);
            if (error != kCGLNoError) {
                MGLOG_E("NSOpenGLImpl: flushBuffer failed: %s", CGLImpl::ErrorString(error));
            }
        }

        void ContextClearDrawable(id self, SEL) {
            auto context = GetContextHandle(self);
            if (context) {
                CGLImpl::ClearDrawable(context);
            }
        }

        void ContextUpdate(id self, SEL) {
            auto context = GetContextHandle(self);
            if (!context) {
                return;
            }
            id view = GetAssoc(self, &kContextViewKey);
            if (!view) {
                CGLImpl::UpdateContext(context);
                return;
            }
            id layer = GetAssoc(self, &kContextLayerKey);
            if (!layer) {
                ContextSetView(self, nullptr, view);
                return;
            }
            const auto geometry = GetDrawableGeometry(view);
            ConfigureMetalLayerForView(layer, view, geometry);
            const auto error = CGLImpl::AttachDrawable(context, view, layer,
                                                       static_cast<GLint>(geometry.DrawableSize.width),
                                                       static_cast<GLint>(geometry.DrawableSize.height));
            if (error != kCGLNoError) {
                MGLOG_E("NSOpenGLImpl: update failed to attach drawable: %s", CGLImpl::ErrorString(error));
                return;
            }
            CGLImpl::UpdateContext(context);
        }

        void ContextSetValues(id self, SEL, const GLint* values, long parameter) {
            auto context = GetContextHandle(self);
            if (context) {
                CGLImpl::SetParameter(context, static_cast<CGLContextParameter>(parameter), values);
            }
        }

        void ContextGetValues(id self, SEL, GLint* values, long parameter) {
            auto context = GetContextHandle(self);
            if (context) {
                CGLImpl::GetParameter(context, static_cast<CGLContextParameter>(parameter), values);
            }
        }

        CGLContextObj ContextCGLContextObj(id self, SEL) {
            return GetContextHandle(self);
        }

        id ContextPixelFormat(id self, SEL) {
            return GetAssoc(self, &kContextPixelFormatObjectKey);
        }

        void ContextDealloc(id self, SEL selector) {
            SetAssoc(self, &kContextPixelFormatObjectKey, nil, kAssociationRetain);
            if (auto context = GetContextHandle(self)) {
                CGLImpl::SetContextNSObject(context, nullptr);
                CGLImpl::ReleaseContext(context);
                SetContextHandle(self, nullptr);
            }
            if (g_contextDealloc) {
                reinterpret_cast<void (*)(id, SEL)>(g_contextDealloc)(self, selector);
            }
        }

        void ReplaceInstanceMethod(Class cls, const char* selectorName, IMP replacement, IMP* original = nullptr) {
            SEL selector = sel_registerName(selectorName);
            Method method = class_getInstanceMethod(cls, selector);
            if (!method) {
                MGLOG_W("NSOpenGLImpl: missing instance method %s", selectorName);
                return;
            }
            if (original) {
                *original = method_getImplementation(method);
            }
            method_setImplementation(method, replacement);
        }

        void ReplaceClassMethod(Class cls, const char* selectorName, IMP replacement) {
            SEL selector = sel_registerName(selectorName);
            Method method = class_getClassMethod(cls, selector);
            if (!method) {
                MGLOG_W("NSOpenGLImpl: missing class method %s", selectorName);
                return;
            }
            method_setImplementation(method, replacement);
        }

        void InstallHooksOnce() {
            Class pixelFormatClass = objc_getClass("NSOpenGLPixelFormat");
            Class contextClass = objc_getClass("NSOpenGLContext");
            if (!pixelFormatClass || !contextClass) {
                MGLOG_W("NSOpenGLImpl: NSOpenGL classes are not loaded; hooks not installed");
                return;
            }

            ReplaceInstanceMethod(pixelFormatClass, "initWithAttributes:",
                                  reinterpret_cast<IMP>(PixelFormatInitWithAttributes));
            ReplaceInstanceMethod(pixelFormatClass, "initWithCGLPixelFormatObj:",
                                  reinterpret_cast<IMP>(PixelFormatInitWithCGLPixelFormatObj));
            ReplaceInstanceMethod(pixelFormatClass, "getValues:forAttribute:forVirtualScreen:",
                                  reinterpret_cast<IMP>(PixelFormatGetValues));
            ReplaceInstanceMethod(pixelFormatClass, "numberOfVirtualScreens",
                                  reinterpret_cast<IMP>(PixelFormatNumberOfVirtualScreens));
            ReplaceInstanceMethod(pixelFormatClass, "CGLPixelFormatObj",
                                  reinterpret_cast<IMP>(PixelFormatCGLPixelFormatObj));
            ReplaceInstanceMethod(pixelFormatClass, "dealloc", reinterpret_cast<IMP>(PixelFormatDealloc),
                                  &g_pixelFormatDealloc);

            ReplaceInstanceMethod(contextClass, "initWithFormat:shareContext:",
                                  reinterpret_cast<IMP>(ContextInitWithFormatShare));
            ReplaceInstanceMethod(contextClass, "initWithCGLContextObj:",
                                  reinterpret_cast<IMP>(ContextInitWithCGLContextObj));
            ReplaceInstanceMethod(contextClass, "setView:", reinterpret_cast<IMP>(ContextSetView));
            ReplaceInstanceMethod(contextClass, "view", reinterpret_cast<IMP>(ContextView));
            ReplaceInstanceMethod(contextClass, "makeCurrentContext", reinterpret_cast<IMP>(ContextMakeCurrent));
            ReplaceClassMethod(contextClass, "clearCurrentContext", reinterpret_cast<IMP>(ContextClearCurrent));
            ReplaceClassMethod(contextClass, "currentContext", reinterpret_cast<IMP>(ContextCurrentContext));
            ReplaceInstanceMethod(contextClass, "flushBuffer", reinterpret_cast<IMP>(ContextFlushBuffer));
            ReplaceInstanceMethod(contextClass, "clearDrawable", reinterpret_cast<IMP>(ContextClearDrawable));
            ReplaceInstanceMethod(contextClass, "update", reinterpret_cast<IMP>(ContextUpdate));
            ReplaceInstanceMethod(contextClass, "setValues:forParameter:", reinterpret_cast<IMP>(ContextSetValues));
            ReplaceInstanceMethod(contextClass, "getValues:forParameter:", reinterpret_cast<IMP>(ContextGetValues));
            ReplaceInstanceMethod(contextClass, "CGLContextObj", reinterpret_cast<IMP>(ContextCGLContextObj));
            ReplaceInstanceMethod(contextClass, "pixelFormat", reinterpret_cast<IMP>(ContextPixelFormat));
            ReplaceInstanceMethod(contextClass, "dealloc", reinterpret_cast<IMP>(ContextDealloc), &g_contextDealloc);

            MGLOG_I("NSOpenGLImpl hooks installed");
        }
    } // namespace

    void InstallHooks() {
        std::call_once(g_installOnce, InstallHooksOnce);
    }
} // namespace MobileGL::MG_Impl::NSOpenGLImpl
#endif
