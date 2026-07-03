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
        void PrintFormatCapabilities(const FormatCapabilityCache& cache);

        struct GLFunctionsTable {
            void (*DrawArrays)(GLenum mode, GLint first, GLsizei count);
            void (*DrawElements)(GLenum mode, GLsizei count, GLenum type, const void* indices);
            void (*DrawElementsBaseVertex)(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLint basevertex);
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
        };
        struct GlobalBackendFunctionsTable {
            GLFunctionsTable GL;
            void (*Present)();
        };

        struct DynamicBackendParameters {
            SizeT UniformBufferOffsetAlignment = 256;
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
            Bool SupportsWideLines = false;
            SizeT MaxShaderStorageBlockSize = 128 * 1024 * 1024;
            Uint32 SubgroupSize = 0;
            Uint32 SubgroupSupportedStages = 0;
            Uint32 SubgroupSupportedFeatures = 0;
            Bool SubgroupQuadOperationsInAllStages = false;
        };

        enum class WindowBackend {
            Android,
            X11,
            MetalLayer,
            // TODO: Wayland, Windows, etc.
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
        };
    } // namespace MG_Backend
} // namespace MobileGL
