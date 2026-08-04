// MobileGL - MobileGL/MG_Backend/BackendObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "MG_State/GLState/TextureState/TextureEnum.h"

namespace MobileGL {
    namespace MG_State::GLState {
        class FramebufferObject;
        class ITextureObject;
    }

    enum class BackendType {
        DirectGLES,
        DirectVulkan,
        BackendTypeCount,
        Unknown = -1
    };

    namespace MG_Backend {
        enum class FormatCapability : Uint64 {
            Creatable = 1ull << 0,

            Sampled = 1ull << 1,
            LinearFilter = 1ull << 2,
            GenerateMipmap = 1ull << 3,
            TextureGather = 1ull << 4,
            TextureShadow = 1ull << 5,

            FramebufferRenderable = 1ull << 6,
            FramebufferLayered = 1ull << 7,
            MultisampleTexture = 1ull << 8,
            MultisampleRenderbuffer = 1ull << 9,

            ColorAttachment = 1ull << 10,
            DepthAttachment = 1ull << 11,
            StencilAttachment = 1ull << 12,

            TextureBuffer = 1ull << 13
        };

        using FormatCapabilityFlags = Flags<FormatCapability>;

        inline constexpr Array<FormatCapability, 14> kReportedFormatCapabilities = {
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

        inline constexpr SizeT kFormatCapabilityTextureTargetCount =
            static_cast<SizeT>(TextureTarget::TextureTargetCount);
        inline constexpr SizeT kFormatCapabilityRenderbufferTargetIndex = kFormatCapabilityTextureTargetCount;
        inline constexpr SizeT kFormatCapabilityTargetCount = kFormatCapabilityTextureTargetCount + 1;
        inline constexpr SizeT kFormatCapabilityFormatCount =
            static_cast<SizeT>(TextureInternalFormat::TextureInternalFormatCount);

        using FormatCapabilityTable =
            Array<Array<FormatCapabilityFlags, kFormatCapabilityFormatCount>, kFormatCapabilityTargetCount>;
        using FormatSampleCountTable =
            Array<Array<Vector<Int>, kFormatCapabilityFormatCount>, kFormatCapabilityTargetCount>;

        struct FormatCapabilityCache {
            FormatCapabilityTable FullCaps{};
            FormatCapabilityTable CaveatCaps{};
            FormatSampleCountTable SampleCounts{};

            void Clear();
        };

        Bool HasFormatCapability(FormatCapabilityFlags caps, FormatCapability capability);
        SizeT GetFormatCapabilityTargetIndex(TextureTarget target);
        SizeT GetRenderbufferFormatCapabilityTargetIndex();
        const char* GetFormatCapabilityName(FormatCapability capability);
        String GetFormatCapabilityTargetName(SizeT targetIndex);
        void PrintFormatCapabilities(const FormatCapabilityCache& cache);

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
            void (*CopyImageSubData)(const SharedPtr<MG_State::GLState::ITextureObject>& srcTexture,
                                     GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                                     const SharedPtr<MG_State::GLState::ITextureObject>& dstTexture,
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
            void (*GetIntegeri_v)(GLenum target, GLuint index, GLint* data);
            void (*GetInteger64i_v)(GLenum target, GLuint index, GLint64* data);
            void (*GetProgramiv)(GLuint program, GLenum pname, GLint* params);
            void (*GetProgramInterfaceiv)(GLuint program, GLenum programInterface, GLenum pname, GLint* params);
            GLuint (*GetProgramResourceIndex)(GLuint program, GLenum programInterface, const GLchar* name);
            void (*GetProgramResourceName)(GLuint program, GLenum programInterface, GLuint index, GLsizei bufSize,
                                           GLsizei* length, GLchar* name);
            void (*GetProgramResourceiv)(GLuint program, GLenum programInterface, GLuint index, GLsizei propCount,
                                         const GLenum* props, GLsizei bufSize, GLsizei* length, GLint* params);
            GLint (*GetProgramResourceLocation)(GLuint program, GLenum programInterface, const GLchar* name);
            GLint (*GetProgramResourceLocationIndex)(GLuint program, GLenum programInterface, const GLchar* name);
            void (*ShaderStorageBlockBinding)(GLuint program, GLuint storageBlockIndex, GLuint storageBlockBinding);
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

        // Coarse GPU vendor identity for gating device-specific quirks. Detected from the
        // Vulkan physical-device vendorID or the GLES GL_VENDOR/GL_RENDERER strings; stays
        // Unknown when detection is inconclusive, in which case auto-gated quirks stay off.
        enum class GpuVendorKind : Uint8 {
            Unknown = 0,
            Qualcomm,
            Arm,
            Nvidia,
            Amd,
            Intel,
            ImgTec,
            // Software rasterizers (llvmpipe/lavapipe, SwiftShader).
            Software,
        };

        struct DynamicBackendParameters {
            SizeT UniformBufferOffsetAlignment = 256;
            // GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT. 1.0 means the backend cannot filter anisotropically,
            // which is also why the extension is not advertised in that case.
            Float MaxTextureMaxAnisotropy = 1.0f;
            Float AliasedLineWidthRangeMin = 1.0f;
            Float AliasedLineWidthRangeMax = 1.0f;
            Float SmoothLineWidthRangeMin = 1.0f;
            Float SmoothLineWidthRangeMax = 1.0f;
            Float SmoothLineWidthGranularity = 1.0f;
            Float PointSizeRangeMin = 1.0f;
            Float PointSizeRangeMax = 1.0f;
            Float PointSizeGranularity = 1.0f;
            Int Max3DTextureSize = 16384;
            Int MaxArrayTextureLayers = 2048;
            Int MaxCubeMapTextureSize = 16384;
            Int MaxFramebufferWidth = 16384;
            Int MaxFramebufferHeight = 16384;
            Int MaxFramebufferLayers = 2048;
            Int MaxRenderbufferSize = 16384;
            Int MaxTextureSize = 16384;
            Int MaxColorTextureSamples = 1;
            Int MaxDepthTextureSamples = 1;
            Int MaxFramebufferSamples = 1;
            Int MaxIntegerSamples = 1;
            Int MaxSamples = 1;
            Int MaxSampleMaskWords = 1;
            // Tessellation limits; defaults are the GL 4.0 core minimums.
            Int MaxPatchVertices = 32;
            Int MaxTessGenLevel = 64;
            // GL_MIN/MAX_PROGRAM_TEXTURE_GATHER_OFFSET. Defaults are the GL 4.0 core
            // minimums, which every ES 3.1 driver also guarantees.
            Int MinProgramTextureGatherOffset = -8;
            Int MaxProgramTextureGatherOffset = 7;
            Int MaxTextureImageUnits = 32;
            Int MaxVertexTextureImageUnits = 32;
            Int MaxComputeTextureImageUnits = 32;
            Int MaxCombinedTextureImageUnits = 192;
            Int MaxVertexAttribs = 16;
            Int MaxComputeShaderStorageBlocks = 8;
            Int MaxCombinedShaderStorageBlocks = 32;
            Int MaxComputeUniformBlocks = 12;
            Int MaxComputeWorkGroupInvocations = 128;
            Int MaxShaderStorageBufferBindings = 8;
            Int MaxTextureBufferSize = 65536;
            Int MaxUniformBufferBindings = 24;
            Int MaxUniformBlockSize = 16384;
            Int MaxImageUnits = 8;
            Int MaxCombinedImageUniforms = 8;
            Int MaxVertexImageUniforms = 0;
            Int MaxGeometryImageUniforms = 0;
            Int MaxFragmentImageUniforms = 8;
            Int MaxComputeImageUniforms = 8;
            Int MaxDrawBuffers = 8;
            Int MaxColorAttachments = 8;
            Int MaxClipDistances = 8;
            Int MaxViewports = 16;
            Int MaxViewportWidth = 16384;
            Int MaxViewportHeight = 16384;
            Float ViewportBoundsRangeMin = 0.0f;
            Float ViewportBoundsRangeMax = 0.0f;
            Int ViewportSubpixelBits = 0;
            // GL 4.x fragment-interpolation offset limits. These defaults are the
            // core minimums and are replaced by live GLES/Vulkan device limits.
            Float MinFragmentInterpolationOffset = -0.5f;
            // For four fractional bits the greatest required legal offset is
            // 0.5 - 2^-4 = 0.4375 (GL 4.6 table 23.70).
            Float MaxFragmentInterpolationOffset = 0.4375f;
            Int FragmentInterpolationOffsetBits = 4;
            Bool SupportsWideLines = false;
            // Whether a framebuffer whose depth and stencil attachments are distinct
            // images can be rendered to. GL only requires support when both refer to the
            // same image and lets an implementation answer GL_FRAMEBUFFER_UNSUPPORTED
            // otherwise, which is what DirectVulkan (one combined attachment) and the
            // real ES drivers behind DirectGLES both do. Defaults to true so a backend
            // that never sets it keeps the permissive behaviour.
            Bool SupportsDistinctDepthStencilAttachments = true;
            SizeT MaxShaderStorageBlockSize = 128 * 1024 * 1024;
            Uint32 SubgroupSize = 0;
            Uint32 SubgroupSupportedStages = 0;
            Uint32 SubgroupSupportedFeatures = 0;
            Bool SubgroupQuadOperationsInAllStages = false;
            GpuVendorKind GpuVendor = GpuVendorKind::Unknown;
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

        class BackendObject {
        public:
            virtual ~BackendObject() = default;

            virtual void Initialize() = 0;
            virtual Bool InitCapabilities() = 0;
            virtual Bool InitWindowSurface() = 0;

            virtual Bool InitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor);
            virtual Bool CreateEGLWindowSurface(EGLSurface surface, const WindowHandle& handle);
            virtual Bool ResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height);
            virtual Bool CreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height);
            virtual Bool MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
            virtual Bool SwapEGLBuffers(EGLDisplay dpy, EGLSurface draw);
            // Forwards the app-requested eglSwapInterval to the backend's native
            // presentation path (no-op for backends without a SetSwapInterval hook).
            virtual void SetEGLSwapInterval(Int interval);
            virtual void ReleaseEGLSurface(EGLSurface surface);
            virtual void ReleaseEGLResources();

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
                WindowHandle Window;
                EGLint Width = 1;
                EGLint Height = 1;
            };

            void ResetEGLRuntimeState();
            Bool RegisterEGLWindowSurface(EGLSurface surface, const WindowHandle& handle);
            Bool RegisterEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height);
            const EGLSurfaceState* GetRegisteredEGLSurface(EGLSurface surface) const;
            Bool ActivateEGLSurface(EGLSurface surface);
            virtual Bool InitPbufferSurface(EGLint width, EGLint height);
            virtual void OnEGLSurfaceReleased(EGLSurface surface);
            FormatCapabilityCache& MutableFormatCapabilities();

            mutable std::recursive_mutex m_eglStateMutex;
            FormatCapabilityCache m_formatCapabilities;
            WindowHandle m_windowHandle;
            EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
            EGLSurface m_eglSurface = EGL_NO_SURFACE;
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
    } // namespace MG_Backend
} // namespace MobileGL
