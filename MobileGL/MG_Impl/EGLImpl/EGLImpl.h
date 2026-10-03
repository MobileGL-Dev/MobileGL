// MobileGL - MobileGL/MG_Impl/EGLImpl/EGLImpl.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <EGL/eglext.h>

#include <mutex>

namespace MobileGL::MG_Impl::EGLImpl {
    EGLSurface CreateWindowSurface(EGLDisplay dpy, EGLConfig config, NativeWindowType window,
                                   const EGLint* attrib_list);
    EGLBoolean SwapBuffers(EGLDisplay dpy, EGLSurface draw);
    EGLBoolean SwapBuffersWithDamage(EGLDisplay dpy, EGLSurface draw, const EGLint* rects, EGLint n_rects);
    EGLBoolean SetDamageRegion(EGLDisplay dpy, EGLSurface surface, const EGLint* rects, EGLint n_rects);
    EGLBoolean ChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs, EGLint config_size,
                            EGLint* num_config);
    EGLContext CreateContext(EGLDisplay dpy, EGLConfig config, EGLContext shareCtx, const EGLint* attrib_list);
    EGLBoolean Initialize(EGLDisplay dpy, EGLint* major, EGLint* minor);
    EGLDisplay GetDisplay(NativeDisplayType display);
    EGLint GetError();
    EGLBoolean MakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
    EGLBoolean DestroyContext(EGLDisplay dpy, EGLContext ctx);
    EGLBoolean DestroySurface(EGLDisplay dpy, EGLSurface surface);
    EGLBoolean Terminate(EGLDisplay dpy);
    EGLBoolean ReleaseThread();
    EGLContext GetCurrentContext();
    EGLBoolean GetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint* value);
    EGLBoolean BindAPI(EGLenum api);
    EGLSurface GetCurrentSurface(EGLint readdraw);
    EGLBoolean QuerySurface(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint* value);
    const char* QueryString(EGLDisplay display, EGLint name);
    EGLBoolean SwapInterval(EGLDisplay dpy, EGLint interval);
    EGLSurface CreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint* attrib_list);
    EGLBoolean BindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer);
    EGLBoolean ReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer);
    EGLBoolean CopyBuffers(EGLDisplay dpy, EGLSurface surface, EGLNativePixmapType target);
    EGLSurface CreatePbufferFromClientBuffer(EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer, EGLConfig config,
                                             const EGLint* attrib_list);
    EGLSurface CreatePixmapSurface(EGLDisplay dpy, EGLConfig config, EGLNativePixmapType pixmap,
                                   const EGLint* attrib_list);
    EGLBoolean GetConfigs(EGLDisplay dpy, EGLConfig* configs, EGLint config_size, EGLint* num_config);
    EGLDisplay GetCurrentDisplay();
    EGLenum QueryAPI();
    EGLBoolean QueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint* value);
    EGLBoolean SurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value);
    EGLBoolean WaitClient();
    EGLBoolean WaitGL();
    EGLBoolean WaitNative(EGLint engine);
    EGLSync CreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib* attrib_list);
    EGLBoolean DestroySync(EGLDisplay dpy, EGLSync sync);
    EGLint ClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout);
    EGLBoolean GetSyncAttrib(EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib* value);
    EGLImage CreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
                         const EGLAttrib* attrib_list);
    EGLBoolean DestroyImage(EGLDisplay dpy, EGLImage image);
    EGLBoolean QueryDmaBufFormats(EGLDisplay dpy, EGLint max_formats, EGLint* formats, EGLint* num_formats);
    EGLBoolean QueryDmaBufModifiers(EGLDisplay dpy, EGLint format, EGLint max_modifiers, EGLuint64KHR* modifiers,
                                    EGLBoolean* external_only, EGLint* num_modifiers);
    // Whether the active backend has shared images (MG_Backend::BackendObject::AllocateSharedImage):
    // a split client's does, a monolith's does not. Decides the dma-buf EGL extensions, the
    // Wayland linux-dmabuf presentation and GL_OES_EGL_image.
    Bool SharedImagesAvailable();
    // The shared image an EGLImage names (EGL_LINUX_DMA_BUF_EXT), for glEGLImageTargetTexture2DOES.
    Bool LookupSharedImage(EGLImage image, Uint64* id, EGLint* width, EGLint* height);
    // glEGLImageTargetTexture2DOES bound a shared image to a texture: from now on glFlush/glFinish
    // may be shared-image boundaries (SharedImageFlushPolicy.h).
    void NoteSharedImageBoundToTexture();
    // From glFlush/glFinish, under the stream lock: publish this session's shared-image accesses
    // when the policy says a flush is a boundary in this process. Cheap when it is not.
    void FlushSharedImageAccesses();
    EGLDisplay GetPlatformDisplay(EGLenum platform, void* native_display, const EGLAttrib* attrib_list);
    EGLSurface CreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void* native_window,
                                           const EGLAttrib* attrib_list);
    EGLBoolean ResizePlatformWindowSurface(EGLDisplay dpy, EGLSurface surface, EGLint width, EGLint height);
    EGLSurface CreatePlatformPixmapSurface(EGLDisplay dpy, EGLConfig config, void* native_pixmap,
                                           const EGLAttrib* attrib_list);
    EGLBoolean WaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags);
    __eglMustCastToProperFunctionPointerType GetProcAddress(const char* name);

#if MOBILEGL_BUILD_DISAGGREGATED
    // THE CLIENT'S STREAM GATE. Under a split transport every thread of the process writes into
    // ONE ring, and the server attributes each record to the context the last applied
    // `bind_context` named - one current context per SESSION. A context, though, is current per
    // THREAD, and Qt renders each window on its own QSGRenderThread: two threads' records then
    // interleave on the ring with no binding between them and land in the wrong context's
    // applier (empty sampler windows at a Clear, another context's buffers under an upload).
    //
    // So every GL entry point and every EGL operation that writes to the stream runs under one
    // process-wide lock (EGLOperationMutex, which the EGL side already held), and before it
    // writes, the server is re-bound to the CALLING thread's context whenever the last binding
    // actually sent names a different one. A no-op scope on a monolith transport and on the
    // server's own apply thread.
    Bool StreamGateActive();
    std::recursive_mutex& StreamMutex();
    void StreamBindCallingThreadLocked();
    // P14: the share-group token of the context the stream is bound to - the group whose object
    // records anything written right now lands in on the server. False while the gate is inactive
    // (a monolith, the server's own apply thread), where no binding crosses. Read under
    // StreamMutex(), like the binding itself.
    Bool StreamBoundShareGroupToken(Uint64* outToken);

    class GLStreamScope {
    public:
        GLStreamScope() {
            if (!StreamGateActive()) return;
            m_mutex = &StreamMutex();
            m_mutex->lock();
            StreamBindCallingThreadLocked();
        }
        ~GLStreamScope() {
            if (m_mutex != nullptr) m_mutex->unlock();
        }
        GLStreamScope(const GLStreamScope&) = delete;
        GLStreamScope& operator=(const GLStreamScope&) = delete;

    private:
        std::recursive_mutex* m_mutex = nullptr;
    };
    // The lock alone, for every EGL entry point: an EGL call may publish into the ring too (a
    // surface forwarder's catch-up wait, a present, a context frame), and a second thread's GL
    // record written into the middle of it is a torn record the server latches the session on.
    class StreamLockScope {
    public:
        StreamLockScope() {
            if (!StreamGateActive()) return;
            m_mutex = &StreamMutex();
            m_mutex->lock();
        }
        ~StreamLockScope() {
            if (m_mutex != nullptr) m_mutex->unlock();
        }
        StreamLockScope(const StreamLockScope&) = delete;
        StreamLockScope& operator=(const StreamLockScope&) = delete;

    private:
        std::recursive_mutex* m_mutex = nullptr;
    };
#else
    class GLStreamScope {};
    class StreamLockScope {};
#endif
} // namespace MobileGL::MG_Impl::EGLImpl
