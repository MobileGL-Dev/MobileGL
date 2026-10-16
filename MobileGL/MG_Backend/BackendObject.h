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
#include "MG_State/GLState/TextureState/TextureEnum.h"
#include <MG_Util/Damage/Damage.h>
#include <MG_Pipe/MGPipeHandles.h>

namespace MobileGL {
    namespace MG_State::GLState {
        class FramebufferObject;
        class ITextureObject;
        class RenderbufferObject;
    }

    namespace MG_Backend {
        // One endpoint of a glCopyImageSubData. GL 4.6 core 18.3.2 accepts GL_RENDERBUFFER
        // alongside the ten whole-image texture targets, and a renderbuffer name lives in a
        // namespace of its own - so an endpoint is a sum type, not an ITextureObject. At most
        // one of the two pointers is set; neither is set when the name named nothing, which is
        // the INVALID_VALUE the frontend validator reports.
        struct CopyImageEndpoint {
            SharedPtr<MG_State::GLState::ITextureObject> Texture;
            SharedPtr<MG_State::GLState::RenderbufferObject> Renderbuffer;
            MG_Pipe::MGPipeHandle TextureHandle = MG_Pipe::kMGPipeNullHandle;
            MG_Pipe::MGPipeHandle RenderbufferHandle = MG_Pipe::kMGPipeNullHandle;

            Bool IsRenderbuffer() const {
                if (!MG_Pipe::MGPipeHandleIsNull(RenderbufferHandle)) return true;
                return Renderbuffer != nullptr;
            }
            Bool Exists() const {
                if (!MG_Pipe::MGPipeHandleIsNull(TextureHandle)) return true;
                if (!MG_Pipe::MGPipeHandleIsNull(RenderbufferHandle)) return true;
                return Texture != nullptr || Renderbuffer != nullptr;
            }
        };

        // Opaque backend fence-sync handle, created by GLFunctionsTable::FenceSync
        // and released by GLFunctionsTable::DeleteSync.
        using BackendSyncHandle = void*;

        // Opaque backend timer-query handle, created by
        // GLFunctionsTable::BeginTimeElapsedQuery / QueryCounterTimestamp and
        // released by GLFunctionsTable::DeleteBackendQuery.
        using BackendQueryHandle = void*;

        struct GLFunctionsTable {
            void (*DrawArrays)(GLenum mode, GLint first, GLsizei count);
            void (*DrawElements)(GLenum mode, GLsizei count, GLenum type, const void* indices);
            void (*DrawElementsBaseVertex)(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLint basevertex);
            void (*MultiDrawArrays)(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount);
            void (*MultiDrawElements)(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                                      GLsizei drawcount);
            void (*MultiDrawElementsBaseVertex)(GLenum mode, const GLsizei* count, GLenum type,
                                                const GLvoid* const* indices, GLsizei drawcount,
                                                const GLint* basevertex);
            void (*MultiDrawElementsIndirect)(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount,
                                              GLsizei stride);
            void (*MultiDrawArraysIndirect)(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride);
            void (*MultiDrawElementsIndirectCount)(GLenum mode, GLenum type, const void* indirect,
                                                   GLintptr drawcount, GLsizei maxdrawcount, GLsizei stride);
            void (*MultiDrawArraysIndirectCount)(GLenum mode, const void* indirect, GLintptr drawcount,
                                                 GLsizei maxdrawcount, GLsizei stride);
            void (*DrawRangeElementsBaseVertex)(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                                const void* indices, GLint basevertex);
            void (*DrawRangeElements)(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                      const void* indices);
            void (*DrawElementsInstancedBaseVertexBaseInstance)(GLenum mode, GLsizei count, GLenum type,
                                                                const void* indices, GLsizei instancecount,
                                                                GLint basevertex, GLuint baseinstance);
            void (*DrawElementsInstancedBaseVertex)(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                    GLsizei instancecount, GLint basevertex);
            void (*DrawElementsInstancedBaseInstance)(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                      GLsizei instancecount, GLuint baseinstance);
            void (*DrawElementsInstanced)(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                          GLsizei instancecount);
            void (*DrawArraysInstancedBaseInstance)(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                                    GLuint baseinstance);
            void (*DrawArraysInstanced)(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
            void (*DrawElementsIndirect)(GLenum mode, GLenum type, const void* indirect);
            void (*DrawArraysIndirect)(GLenum mode, const void* indirect);
            void (*Clear)(GLbitfield mask);
            void (*ClearBufferfi)(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
            void (*ClearBufferfv)(GLenum buffer, GLint drawbuffer, const GLfloat* value);
            void (*ClearBufferuiv)(GLenum buffer, GLint drawbuffer, const GLuint* value);
            void (*ClearBufferiv)(GLenum buffer, GLint drawbuffer, const GLint* value);
            void (*ClearNamedFramebufferfv)(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                            GLenum buffer, GLint drawbuffer, const GLfloat* value);
            void (*ClearNamedFramebufferfi)(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                            GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
            void (*ClearNamedFramebufferiv)(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                            GLenum buffer, GLint drawbuffer, const GLint* value);
            void (*ClearNamedFramebufferuiv)(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                             GLenum buffer, GLint drawbuffer, const GLuint* value);
            void (*BlitFramebuffer)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                                    GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
            void (*BlitNamedFramebuffer)(const SharedPtr<MG_State::GLState::FramebufferObject>& readFramebuffer,
                                         const SharedPtr<MG_State::GLState::FramebufferObject>& drawFramebuffer,
                                         GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                         GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                         GLbitfield mask, GLenum filter);
            void (*CopyTexImage2D)(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                                   GLsizei height, GLint border);
            void (*CopyTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                                      GLsizei width, GLsizei height);
            void (*CopyImageSubData)(const CopyImageEndpoint& src,
                                     GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                                     const CopyImageEndpoint& dst,
                                     GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                                     GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);
            void (*GenerateMipmap)(GLenum target);
            void (*ReadPixels)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                               void* pixels);
            void (*GetTexImage)(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels);
            void (*GetTextureImage)(const SharedPtr<MG_State::GLState::ITextureObject>& texture,
                                    TextureUploadTarget uploadTarget, GLint level, GLenum format, GLenum type,
                                    GLsizei bufSize, GLvoid* pixels);
            void (*DispatchCompute)(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ);
            void (*DispatchComputeIndirect)(GLintptr indirect);
            void (*MemoryBarrier)(GLbitfield barriers);
            void (*MemoryBarrierByRegion)(GLbitfield barriers);
            void (*BindImageTexture)(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer,
                                     GLenum access, GLenum format);
            // The ONLY indexed query that is genuinely a backend one, and only for the pnames
            // MG_Impl/GLImpl/Getter/GL_Getter.cpp does not already own. Every indexed pname that
            // names FRONTEND state - the indexed buffer bindings, the per-unit texture/sampler
            // bindings, the image-unit bindings, the viewport rectangles, the indexed capabilities
            // - is answered in GL_Getter::GetIntegeri_v and never reaches this entry; the
            // 64-bit and float/double widths are derived there from the same answer, which is why
            // no GetInteger64i_v/GetFloati_v/GetDoublei_v table entry exists. In practice this
            // leaves GL_MAX_COMPUTE_WORK_GROUP_COUNT / _SIZE (also asked directly by
            // MG_Util/ShaderTranspiler/CompileEnv.cpp) plus whatever pname the frontend has no
            // case for at all.
            void (*GetIntegeri_v)(GLenum target, GLuint index, GLint* data);
            // There is deliberately NO GetProgramiv entry: glGetProgramiv describes the program
            // the APPLICATION wrote - link status, the transform-feedback mode, the compute local
            // size - all of which are frontend link artifacts on ProgramObject, and
            // MG_Impl/GLImpl/Program/GL_Program.cpp answers every one of them from there. Asking a
            // backend would mean asking about a DIFFERENT program (a SPIRV-Cross-generated ESSL
            // one, or a SPIR-V module), in a namespace the application never sees.
            // The GL program interface (glGetProgramInterfaceiv / glGetProgramResource*) is NOT
            // a backend query: it describes the program the application wrote, in the
            // application's namespace, which neither backend program is in. It is answered
            // entirely by MG_Impl/GLImpl/Program/ProgramInterface from the frontend reflection.
            // Takes the block's GL NAME, not glShaderStorageBlockBinding's index. The index
            // the application passes is the frontend interface-query enumeration's, and no
            // backend shares that index space: DirectVulkan enumerates SPIR-V descriptor
            // bindings and DirectGLES asks a real driver about SPIRV-Cross-generated ESSL.
            // The name is the one coordinate all three agree on, so the frontend resolves the
            // index against its own enumeration and each backend maps the name to its own.
            void (*ShaderStorageBlockBinding)(GLuint program, const GLchar* storageBlockName,
                                              GLuint storageBlockBinding);
            // GL fence sync objects. All entries are optional (may be null); the
            // frontend then falls back to always-signaled sync semantics.
            // FenceSync may itself return null when the backend cannot create a
            // fence right now (e.g. the calling thread does not own the backend
            // context); the frontend treats such a sync as always signaled.
            BackendSyncHandle (*FenceSync)();
            GLenum (*ClientWaitSync)(BackendSyncHandle sync, GLbitfield flags, GLuint64 timeout);
            void (*WaitSync)(BackendSyncHandle sync, GLbitfield flags, GLuint64 timeout);
            void (*DeleteSync)(BackendSyncHandle sync);
            Bool (*GetSyncStatus)(BackendSyncHandle sync); // true = signaled
            // GL timer-query objects (GL_ARB_timer_query). All entries are
            // optional (may be null); the frontend then falls back to zero
            // results and reports GL_QUERY_COUNTER_BITS == 0.
            // BeginTimeElapsedQuery / QueryCounterTimestamp may themselves
            // return null when the backend cannot create a query right now;
            // the frontend treats such a query as immediately available with
            // a zero result.
            // Dynamic support check: true only when the live backend can
            // actually time at the moment of the call (extension / entry
            // points / timestamp valid bits are known then, not at table
            // init). Gates the advertised GL_QUERY_COUNTER_BITS.
            Bool (*IsTimerQuerySupported)();
            BackendQueryHandle (*BeginTimeElapsedQuery)();            // starts a TIME_ELAPSED span
            void (*EndTimeElapsedQuery)(BackendQueryHandle query);    // ends the span
            BackendQueryHandle (*QueryCounterTimestamp)();            // glQueryCounter(GL_TIMESTAMP) one-shot
            Bool (*IsQueryResultAvailable)(BackendQueryHandle query); // non-blocking
            // Returns true when a final value was produced (*outNanoseconds
            // written; the frontend may cache it and release the handle).
            // Returns false when the result could not be obtained YET - e.g.
            // a Vulkan wait that refuses to block on a not-yet-submitted
            // frame serial - in which case the frontend must keep the handle
            // and leave the query readable later.
            Bool (*GetQueryResult64)(BackendQueryHandle query, Bool wait, Uint64* outNanoseconds);
            void (*DeleteBackendQuery)(BackendQueryHandle query);
            // GL_SAMPLES_PASSED occlusion queries (optional; null = unsupported,
            // the frontend then rejects the target). Results/deletion flow through
            // GetQueryResult64 / DeleteBackendQuery like timer queries.
            BackendQueryHandle (*BeginOcclusionQuery)();
            void (*EndOcclusionQuery)(BackendQueryHandle query);
            // Transform feedback primitive queries backed by real GPU query pools
            // (optional; null = frontend falls back to CPU accounting).
            BackendQueryHandle (*BeginXfbPrimitivesQuery)(Bool generated);
            void (*EndXfbPrimitivesQuery)(BackendQueryHandle query);
            // Whether GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN should be answered from the
            // frontend's own accounting wherever that accounting is exact - a capture with no
            // geometry stage - instead of from the query above. Set by DirectGLES, whose result
            // is whatever the ES driver's PRIMITIVES_WRITTEN counter says: Adreno reports twice
            // the written count for a vertex-only capture that follows a large render pass,
            // where the desktop-exact answer is the one the frontend already computed. Defaults
            // to false, so a backend that never sets it keeps using its GPU result.
            Bool PrefersCpuXfbPrimitiveAccounting = false;
            // Transform feedback capture spans, for backends whose own GL/ES driver
            // performs the capture (DirectGLES). Both optional; null means the backend
            // drives capture from its draw recording instead (DirectVulkan). End is
            // called while the frontend capture state is still active, so the backend
            // can still see the capture program and buffer bindings.
            // GL_PATCH_VERTICES; ES 3.2 spells it the same way.
            void (*PatchParameteri)(GLenum pname, GLint value);
            void (*BeginTransformFeedback)(GLenum primitiveMode);
            void (*EndTransformFeedback)();
            // ARB_transform_feedback2. A backend that leaves these null keeps the single
            // implicit capture span the frontend has always modelled; the frontend state
            // (paused flag, per-object bindings) is tracked either way.
            void (*PauseTransformFeedback)();
            void (*ResumeTransformFeedback)();
            void (*BindTransformFeedback)(GLuint name);
            void (*DeleteTransformFeedback)(GLuint name);
            Int64 (*GetGpuTimestampNs)(); // glGetInteger64v(GL_TIMESTAMP); 0 if unsupported
        };
        struct GlobalBackendFunctionsTable {
            GLFunctionsTable GL;
            void (*Present)();
            // Optional: applies the app-requested eglSwapInterval to the native
            // presentation path (null = backend keeps its own pacing policy).
            void (*SetSwapInterval)(Int interval);
        };

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

        // SHARED IMAGES (docs/archive/notes/anland/plan-ahb-dmabuf.md): server-allocated colour
        // buffers exported to clients as dma-buf descriptors.
        //
        // The server-side view of one image, as a backend sees it: `NativeBuffer` is the image's
        // AHardwareBuffer (null where there is none), `Fourcc` a DRM fourcc - ABGR8888 (R, G, B, A in
        // memory, GL RGBA8) or XBGR8888 (alpha ignored); ARGB8888 / XRGB8888 images are stored the
        // same way (SharedImageRegistry.h says why), so a backend binds every one as RGBA8.
        struct SharedImageView {
            Uint64 Id = 0;
            Uint32 Width = 0;
            Uint32 Height = 0;
            Uint32 Fourcc = 0;
            void* NativeBuffer = nullptr;
        };

        // The client-side answer of an allocation: the image's id and its exported descriptor
        // (`Fd`, owned by the caller), with the plane layout a linux-dmabuf wl_buffer is created with.
        struct SharedImageExport {
            Uint64 Id = 0;
            int Fd = -1;
            Uint32 Width = 0;
            Uint32 Height = 0;
            Uint32 Fourcc = 0;
            Uint32 Stride = 0;
            Uint32 Offset = 0;
            Uint64 Modifier = 0;
            // A YUV image's Cb Cr plane: in the same descriptor, at this offset and pitch.
            Uint32 Plane1Stride = 0;
            Uint32 Plane1Offset = 0;
        };

        // An import of a two-plane (YUV) dma-buf: where its planes are in the one descriptor, and
        // EGL_EXT_image_dma_buf_import's colour hints (0 = not given).
        struct SharedImageImportLayout {
            Uint32 PlaneCount = 1;
            Uint32 Offset[2] = {0, 0};
            Uint32 Pitch[2] = {0, 0};
            Uint32 ColorSpace = 0;
            Uint32 Range = 0;
            Uint32 SitingX = 0;
            Uint32 SitingY = 0;
            // The DRM format modifier the import named; HasModifier false = none (the implicit one).
            Bool HasModifier = false;
            Uint64 Modifier = 0;
        };

        // THE DAMAGE OF THE PRESENT BEING APPLIED on this thread: the server's apply thread sets it
        // around the backend's Present (from the present record), and a backend that can hand damage
        // to its native present reads it there. Full whenever nothing set it.
        const MG_Util::Damage::Region& CurrentPresentDamage();
        void SetCurrentPresentDamage(const MG_Util::Damage::Region& damage);

        class BackendObject {
        public:
            virtual ~BackendObject() = default;

            // ---- shared images, SERVER side (a backend implements these) ---------------------
            //
            // Copies the default framebuffer of the surface current on this (the apply) thread into
            // `image`, rows TOP FIRST - GL's top row (y = height-1) lands in the image's first memory
            // row, which is what a wl_buffer's first row is - scaled to the image's extent if the two
            // differ. The image is ORDERED THROUGH ITS SYNC STATE (SharedImageRegistry.h): the copy
            // waits for the image's pending read fences before it writes, and its own completion
            // is published with SharedImages::PublishWrite as a sync_file - the call returns once
            // the copy is submitted, never waiting for it on the CPU (a backend with no fence export
            // waits and publishes -1). A reader binding the image waits for that fence on its GPU
            // when the write generation moved, and at its frame boundary publishes its frame's fence
            // with PublishRead / ReadTracker. The application's GL state is untouched. False = not
            // supported / failed (logged).
            //
            // `region` (GL window coordinates, origin bottom-left) is the part of the frame to copy;
            // Full copies all of it. The rest of the image is left as it is - the client sends a
            // region only for an image that already holds the frame outside it - and a backend that
            // cannot be sure the image's contents survived (a target it has never written) copies
            // all of it regardless.
            virtual Bool BlitDefaultFramebufferToSharedImage(const SharedImageView& image,
                                                             const MG_Util::Damage::Region& region) {
                (void)image, (void)region;
                return false;
            }

            // EGL_BUFFER_AGE_EXT of the draw surface of the context current on this (the apply)
            // thread: how many presents ago the buffer the next frame draws into held the frame then
            // presented; 0 = unknown (the application repaints everything). Asking says the client
            // relies on the answer, so from then on the backend keeps what it presents.
            //
            // `damageRegionFollows`: the client declares the region it draws (SetCurrentDamageRegion)
            // before drawing. A driver whose only buffer age is EGL_KHR_partial_update's keeps a
            // buffer's content only outside that region, so without one coming it is not asked.
            virtual Int32 QueryCurrentBufferAge(Bool damageRegionFollows) {
                (void)damageRegionFollows;
                return 0;
            }
            // eglSetDamageRegionKHR for that draw surface: the frame draws `region` (GL window
            // coordinates; Full = all of it) and nothing outside it. A backend without the notion
            // ignores it.
            virtual Bool SetCurrentDamageRegion(const MG_Util::Damage::Region& region) {
                (void)region;
                return true;
            }
            // IMPLICIT SYNC BY FLUSH (docs/archive/notes/anland/plan-x11-gpu.md): the session
            // on this (the apply) thread ends a frame at a glFlush. Every shared image it used since
            // its last boundary is published as written and read by one fence of everything it
            // submitted (SharedImages::ReadTracker::PublishFrameAsWrite), and from now on its first
            // use of an image in a frame waits for that image's pending reads (it may be about to
            // overwrite what another session still samples). A backend without shared images has
            // nothing to publish.
            virtual Bool PublishSharedImageAccesses() { return true; }
            // EGL_ANDROID_native_fence_sync's fence command, on the server: in `*fence`, a sync_file
            // (the caller's to close) that signals once everything the session submitted so far has
            // completed - and, for a session that writes images, what PublishSharedImageAccesses
            // does. -1 when there is none to give: the backend cannot export one, or the work has
            // already completed - in both cases it HAS completed when this returns.
            virtual Bool ExportNativeFence(int* fence) {
                *fence = -1;
                return PublishSharedImageAccesses();
            }

            // ---- shared images, CLIENT side (BackendObject_Remote implements these) ----------
            //
            // A new image, allocated by the server; `out->Fd` is the caller's to close.
            virtual Bool AllocateSharedImage(Uint32 width, Uint32 height, Uint32 fourcc, SharedImageExport* out) {
                (void)width, (void)height, (void)fourcc, (void)out;
                return false;
            }
            // Names the image `fd` was exported from (the caller keeps `fd`). False: not one of this
            // server's images, or the size/format disagree.
            virtual Bool ImportSharedImage(int fd, Uint32 width, Uint32 height, Uint32 fourcc, Uint64* outId) {
                (void)fd, (void)width, (void)height, (void)fourcc, (void)outId;
                return false;
            }
            // The same for a YUV fourcc, whose planes and hints `layout` gives. A YUV buffer the
            // server did not export is taken too (its labelled CPU-copy fallback).
            virtual Bool ImportSharedImagePlanes(int fd, Uint32 width, Uint32 height, Uint32 fourcc,
                                                 const SharedImageImportLayout& layout, Uint64* outId) {
                (void)fd, (void)width, (void)height, (void)fourcc, (void)layout, (void)outId;
                return false;
            }
            // Drops this client's reference (an allocation's or an import's).
            virtual Bool ReleaseSharedImage(Uint64 id) {
                (void)id;
                return false;
            }
            // In the GL stream: the current context's default framebuffer, copied top row first into
            // the image (BlitDefaultFramebufferToSharedImage on the server) - `region` of it, Full for
            // all. Returns when it completed.
            virtual Bool PresentToSharedImage(Uint64 id, const MG_Util::Damage::Region& region) {
                (void)id, (void)region;
                return false;
            }
            // In the GL stream: EGL_BUFFER_AGE_EXT of the current draw surface (QueryCurrentBufferAge
            // on the server). False: the backend has no answer (the caller reports 0).
            virtual Bool QueryBufferAge(Bool damageRegionFollows, EGLint* age) {
                (void)damageRegionFollows, (void)age;
                return false;
            }
            // In the GL stream: eglSetDamageRegionKHR for the current draw surface
            // (SetCurrentDamageRegion on the server).
            virtual Bool SetDamageRegion(const MG_Util::Damage::Region& region) {
                (void)region;
                return false;
            }
            // In the GL stream: the texture whose lifetime id is `textureLifetimeId` takes the image as
            // its level-0 storage (glEGLImageTargetTexture2DOES), until its next storage definition.
            // The caller has already defined level 0 as Width x Height RGBA8 with no data.
            virtual Bool AttachSharedImageToTexture(Uint64 textureLifetimeId, Uint64 id) {
                (void)textureLifetimeId, (void)id;
                return false;
            }
            // In the GL stream, from glFlush/glFinish: PublishSharedImageAccesses on the server,
            // returning once the fence is published. Skipped (true) when nothing reached the stream
            // since the last one.
            virtual Bool FlushSharedImageAccesses() { return false; }
            // In the GL stream, from eglCreateSync(EGL_SYNC_NATIVE_FENCE_ANDROID) with no descriptor:
            // ExportNativeFence on the server. `*fence` is the sync_file it handed back (the
            // caller's to close), or -1 when it had none - the fenced work has then completed.
            virtual Bool CreateNativeFence(int* fence) {
                *fence = -1;
                return false;
            }

            virtual void Initialize() = 0;
            virtual Bool InitCapabilities() = 0;
            virtual Bool InitWindowSurface() = 0;

            // A served session's apply thread - the one thread that runs this backend from here on -
            // calls this first, in a process that may serve several sessions at once. A backend whose
            // native state is process-global by construction gives this object its own copy and binds
            // it to the calling thread. The default has nothing per process to separate.
            virtual void BindSessionStateToThisThread() {}
            // A served session's apply thread asks this each time it wakes, BEFORE it applies another
            // record: true when this session's own GPU work faulted or hung the GPU and the backend has
            // latched the session for it (MGPipeSessionLatch, family DeviceLost), so the next frame -
            // which would very likely stop the GPU again - is never applied. Cheap by contract: no
            // wait, at most a status read. The default knows of no fault.
            virtual Bool LatchIfGpuFaulted() { return false; }
            // A served session's ring moved to another of its client's contexts (bind_context). A
            // backend whose surface follows the native context it binds needs nothing; one that keeps
            // a single native surface binding re-binds the surface that context was last made current
            // with. `token` is the client's context token (the EGLContext MakeEGLCurrent was given).
            virtual void OnClientContextBound(Uint64 token) { (void)token; }

            virtual Bool InitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor);
            virtual Bool CreateEGLWindowSurface(EGLSurface surface, const WindowHandle& handle);
            virtual Bool ResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height);
            virtual Bool CreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height);
            virtual Bool MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
            virtual Bool SwapEGLBuffers(EGLDisplay dpy, EGLSurface draw);
            // eglSwapBuffersWithDamage: `damage` (GL window coordinates) is a hint for the native
            // present. A backend with nothing to hand it to swaps as SwapEGLBuffers does.
            virtual Bool SwapEGLBuffersWithDamage(EGLDisplay dpy, EGLSurface draw, const MG_Util::Damage::Region& damage) {
                (void)damage;
                return SwapEGLBuffers(dpy, draw);
            }
            // Forwards the app-requested eglSwapInterval to the backend's native
            // presentation path (no-op for backends without a SetSwapInterval hook).
            virtual void SetEGLSwapInterval(Int interval);
            virtual void ReleaseEGLSurface(EGLSurface surface);
            virtual void ReleaseEGLResources();

            // ---- the server's own display window going away and coming back -------------------
            //
            // `window` (a window surface's native window) is about to be destroyed - the display's
            // screen went off. Release ONLY what is bound to it: every window surface on it keeps its
            // client handle and draws into a placeholder from here on, and every context, object and
            // device stays. Called on the thread that runs this backend; nothing may reference the
            // window once it returns true. False: not supported (the caller ends the session instead).
            virtual Bool SuspendServerWindow(void* window) {
                (void)window;
                return false;
            }
            // The surfaces SuspendServerWindow parked are rebuilt on `window` (a new native window,
            // `width`x`height`), and their new extent is published as a creation publishes it. False:
            // they could not be (the caller ends the session).
            virtual Bool ResumeServerWindow(void* window, Uint32 width, Uint32 height) {
                (void)window, (void)width, (void)height;
                return false;
            }
            // `window` changed size to `width`x`height` (the display's report - the one size to publish).
            // Its surfaces publish that extent to the client now, without waiting for a present; a
            // swapchain that has to follow is rebuilt when no frame is half-recorded, else at the
            // present. False: nothing was published (no surface on it, or not supported).
            virtual Bool ServerWindowResized(void* window, Uint32 width, Uint32 height) {
                (void)window, (void)width, (void)height;
                return false;
            }
            // CLIENT side, from eglQuerySurface(EGL_WIDTH/EGL_HEIGHT): a backend whose surface extents
            // change under the application (a remote server's window) applies what it has received.
            virtual void RefreshSurfaceExtent(EGLSurface surface) { (void)surface; }

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

        extern UniquePtr<BackendObject>& pActiveBackendObject;
        extern GlobalBackendFunctionsTable gBackendFunctionsTable;
    } // namespace MG_Backend
} // namespace MobileGL
