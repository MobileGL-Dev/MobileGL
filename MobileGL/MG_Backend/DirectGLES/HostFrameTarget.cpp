// MobileGL - MobileGL/MG_Backend/DirectGLES/HostFrameTarget.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "HostFrameTarget.h"
#include "DirectGLES.h"
#include "Managers.h"

#if MOBILEGL_BUILD_DISAGGREGATED && defined(__ANDROID__)

#include <android/hardware_buffer.h>
#include <cstring>
#include <format>

namespace MobileGL::MG_Backend::DirectGLES {
    namespace {
        // The four entry points this is spelled in are optional everywhere: the loader's own
        // tables do not carry them (a missing EGL function there is MGLOG_F), so the extension
        // strings decide first and the pointers are taken only once they have.
        using GetNativeClientBufferFn = EGLClientBuffer (*)(const struct AHardwareBuffer*);
        using CreateImageFn = EGLImageKHR (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);
        using DestroyImageFn = EGLBoolean (*)(EGLDisplay, EGLImageKHR);
        using ImageTargetRenderbufferFn = void (*)(GLenum, GLeglImageOES);

        Bool HasExtensionWord(const char* list, const char* word) {
            if (list == nullptr) return false;
            const SizeT n = std::strlen(word);
            for (const char* at = std::strstr(list, word); at != nullptr; at = std::strstr(at + 1, word)) {
                const Bool startOk = at == list || at[-1] == ' ';
                const Bool endOk = !at[n] || at[n] == ' ';
                if (startOk && endOk) return true;
            }
            return false;
        }

        struct EntryPoints {
            GetNativeClientBufferFn GetNativeClientBuffer = nullptr;
            CreateImageFn CreateImage = nullptr;
            DestroyImageFn DestroyImage = nullptr;
            ImageTargetRenderbufferFn TargetRenderbuffer = nullptr;
        };

        // Resolved once per process and cached together with the refusal: a driver that lacks one
        // of these does not grow it later, and the answer names what is missing.
        const EntryPoints& Resolved(String& why) {
            static EntryPoints points;
            static Bool resolved = false;
            static String refusal;
            if (!resolved) {
                resolved = true;
                if (!g_EGLFuncs.eglGetProcAddress || !g_EGLFuncs.eglQueryString || !g_EGLFuncs.eglGetCurrentDisplay ||
                    !g_GLESFuncs.glGetString) {
                    refusal = "the loader has no eglGetProcAddress/eglQueryString/eglGetCurrentDisplay/glGetString";
                } else {
                    const char* egl = g_EGLFuncs.eglQueryString(g_EGLFuncs.eglGetCurrentDisplay(), EGL_EXTENSIONS);
                    const char* gl = reinterpret_cast<const char*>(g_GLESFuncs.glGetString(GL_EXTENSIONS));
                    if (!HasExtensionWord(egl, "EGL_ANDROID_get_native_client_buffer")) {
                        refusal = "EGL_ANDROID_get_native_client_buffer is not advertised";
                    } else if (!HasExtensionWord(egl, "EGL_KHR_image_base")) {
                        refusal = "EGL_KHR_image_base is not advertised";
                    } else if (!HasExtensionWord(gl, "GL_OES_EGL_image")) {
                        refusal = "GL_OES_EGL_image is not advertised";
                    } else {
                        points.GetNativeClientBuffer = reinterpret_cast<GetNativeClientBufferFn>(
                            g_EGLFuncs.eglGetProcAddress("eglGetNativeClientBufferANDROID"));
                        points.CreateImage =
                            reinterpret_cast<CreateImageFn>(g_EGLFuncs.eglGetProcAddress("eglCreateImageKHR"));
                        points.DestroyImage =
                            reinterpret_cast<DestroyImageFn>(g_EGLFuncs.eglGetProcAddress("eglDestroyImageKHR"));
                        points.TargetRenderbuffer = reinterpret_cast<ImageTargetRenderbufferFn>(
                            g_EGLFuncs.eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
                        if (points.GetNativeClientBuffer == nullptr || points.CreateImage == nullptr ||
                            points.DestroyImage == nullptr || points.TargetRenderbuffer == nullptr) {
                            refusal = "eglGetProcAddress gave no eglGetNativeClientBufferANDROID / eglCreateImageKHR / "
                                      "eglDestroyImageKHR / glEGLImageTargetRenderbufferStorageOES";
                            points = EntryPoints{};
                        }
                    }
                }
            }
            why = refusal;
            return points;
        }

        void DrainGLErrors() {
            if (!g_GLESFuncs.glGetError) return;
            for (Uint guard = 0; guard < 16 && g_GLESFuncs.glGetError() != GL_NO_ERROR; ++guard) {
            }
        }
    }

    Bool HostFrameTargetSupported(String& why) {
        const EntryPoints& points = Resolved(why);
        return points.GetNativeClientBuffer != nullptr;
    }

    Bool HostFrameTargetCreate(const struct AHardwareBuffer* buffer, HostFrameTarget& target, String& why) {
        target = HostFrameTarget{};
        if (buffer == nullptr) {
            why = "no buffer";
            return false;
        }
        const EntryPoints& points = Resolved(why);
        if (points.GetNativeClientBuffer == nullptr) return false;

        if (!g_GLESFuncs.glGenRenderbuffers || !g_GLESFuncs.glBindRenderbuffer || !g_GLESFuncs.glGenFramebuffers ||
            !g_GLESFuncs.glBindFramebuffer || !g_GLESFuncs.glFramebufferRenderbuffer ||
            !g_GLESFuncs.glCheckFramebufferStatus) {
            why = "the loader has no glGenRenderbuffers/glBindRenderbuffer/glGenFramebuffers/glBindFramebuffer/"
                  "glFramebufferRenderbuffer/glCheckFramebufferStatus";
            return false;
        }

        AHardwareBuffer_Desc desc{};
        AHardwareBuffer_describe(buffer, &desc);

        DrainGLErrors();
        const EGLClientBuffer clientBuffer = points.GetNativeClientBuffer(buffer);
        if (clientBuffer == nullptr) {
            why = "eglGetNativeClientBufferANDROID returned null";
            return false;
        }
        const EGLint imageAttribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        const EGLImageKHR image =
            points.CreateImage(g_EGLFuncs.eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                               clientBuffer, imageAttribs);
        if (image == EGL_NO_IMAGE_KHR) {
            why = "eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID) refused the host frame";
            return false;
        }

        GLuint renderbuffer = 0;
        g_GLESFuncs.glGenRenderbuffers(1, &renderbuffer);
        g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        points.TargetRenderbuffer(GL_RENDERBUFFER, reinterpret_cast<GLeglImageOES>(image));
        const GLenum renderbufferError = g_GLESFuncs.glGetError ? g_GLESFuncs.glGetError() : GL_NO_ERROR;
        if (renderbufferError != GL_NO_ERROR) {
            why = std::format("glEGLImageTargetRenderbufferStorageOES -> 0x{:04x}", renderbufferError);
            g_GLESFuncs.glDeleteRenderbuffers(1, &renderbuffer);
            points.DestroyImage(g_EGLFuncs.eglGetCurrentDisplay(), image);
            return false;
        }

        GLuint framebuffer = 0;
        g_GLESFuncs.glGenFramebuffers(1, &framebuffer);
        g_GLESFuncs.glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        g_GLESFuncs.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
        const GLenum status = g_GLESFuncs.glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            why = std::format("the host frame as a color attachment -> framebuffer status 0x{:04x}", status);
            g_GLESFuncs.glDeleteFramebuffers(1, &framebuffer);
            g_GLESFuncs.glDeleteRenderbuffers(1, &renderbuffer);
            points.DestroyImage(g_EGLFuncs.eglGetCurrentDisplay(), image);
            return false;
        }

        target.Image = image;
        target.Renderbuffer = renderbuffer;
        target.Framebuffer = framebuffer;
        target.Width = desc.width;
        target.Height = desc.height;
        MGLOG_I("host frame target: %ux%d as framebuffer %u (stride %u, format 0x%x)", desc.width, desc.height,
                framebuffer, desc.stride, desc.format);
        return true;
    }

    void HostFrameTargetBind(const HostFrameTarget& target) {
        if (target.Framebuffer == 0) return;
        // THROUGH THE ENGINE'S SHADOW, never raw: a raw bind would leave the shadow claiming
        // the previous framebuffer and false-skip the next re-bind (FramebufferImpl, Managers.h).
        FramebufferImpl::BindFramebufferId(GL_FRAMEBUFFER, target.Framebuffer);
    }

    void HostFrameTargetDestroy(HostFrameTarget& target) {
        if (target.Framebuffer != 0 && g_GLESFuncs.glDeleteFramebuffers)
            g_GLESFuncs.glDeleteFramebuffers(1, &target.Framebuffer);
        if (target.Renderbuffer != 0 && g_GLESFuncs.glDeleteRenderbuffers)
            g_GLESFuncs.glDeleteRenderbuffers(1, &target.Renderbuffer);
        if (target.Image != nullptr) {
            String why;
            const EntryPoints& points = Resolved(why);
            if (points.DestroyImage != nullptr && g_EGLFuncs.eglGetCurrentDisplay)
                points.DestroyImage(g_EGLFuncs.eglGetCurrentDisplay(), static_cast<EGLImageKHR>(target.Image));
        }
        target = HostFrameTarget{};
    }
}

#else  // !(MOBILEGL_BUILD_DISAGGREGATED && __ANDROID__)

namespace MobileGL::MG_Backend::DirectGLES {
    Bool HostFrameTargetSupported(String& why) {
        why = "host frames are a disaggregated Android path";
        return false;
    }
    Bool HostFrameTargetCreate(const struct AHardwareBuffer*, HostFrameTarget&, String& why) {
        why = "host frames are a disaggregated Android path";
        return false;
    }
    void HostFrameTargetBind(const HostFrameTarget&) {}
    void HostFrameTargetDestroy(HostFrameTarget& target) { target = HostFrameTarget{}; }
}

#endif
