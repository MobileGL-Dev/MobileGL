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
        class RenderbufferObject;
    }

    enum class BackendType {
        DirectGLES,
        DirectVulkan,
        BackendTypeCount,
        Unknown = -1
    };

    namespace MG_Backend {
        // One endpoint of a glCopyImageSubData. GL 4.6 core 18.3.2 accepts GL_RENDERBUFFER
        // alongside the ten whole-image texture targets, and a renderbuffer name lives in a
        // namespace of its own - so an endpoint is a sum type, not an ITextureObject. At most
        // one of the two pointers is set; neither is set when the name named nothing, which is
        // the INVALID_VALUE the frontend validator reports.
        struct CopyImageEndpoint {
            SharedPtr<MG_State::GLState::ITextureObject> Texture;
            SharedPtr<MG_State::GLState::RenderbufferObject> Renderbuffer;

            Bool IsRenderbuffer() const { return Renderbuffer != nullptr; }
            Bool Exists() const { return Texture != nullptr || Renderbuffer != nullptr; }
        };

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
            // GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, which is a SEPARATE limit from the
            // uniform one and is routinely larger: Adreno 830 reports 32 for uniform buffers and
            // 64 for storage buffers. Answering the storage query with the uniform value let an
            // application bind a storage range at an offset the driver cannot address, which it
            // accepted without error and then wrote somewhere else entirely.
            SizeT ShaderStorageBufferOffsetAlignment = 256;
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
            // Per-stage GL_MAX_*_SHADER_STORAGE_BLOCKS. Zero is a legal answer for the four
            // non-compute, non-fragment stages and these defaults are the spec minimums, not
            // placeholders: GL 4.6 table 23.64 and ES 3.2 table 21.44 both set the minimum for
            // vertex, tessellation control, tessellation evaluation and geometry at 0, and only
            // fragment (8 in GL, 4 in ES) and compute are guaranteed to have any. Every real ARM
            // GLES driver takes that allowance - a Mali-G925 reports 0 for all four - so a
            // backend that cannot honour a graphics-stage storage block MUST report 0 here
            // rather than a hopeful number. Advertising a non-zero count the driver will refuse
            // does not make the block work; it only moves the failure from an honest
            // "unsupported" at query time to a backend link error the frontend never surfaces,
            // after which every draw with that program silently renders nothing.
            Int MaxVertexShaderStorageBlocks = 0;
            Int MaxTessControlShaderStorageBlocks = 0;
            Int MaxTessEvaluationShaderStorageBlocks = 0;
            Int MaxGeometryShaderStorageBlocks = 0;
            Int MaxFragmentShaderStorageBlocks = 8;
            Int MaxComputeUniformBlocks = 12;
            Int MaxComputeWorkGroupInvocations = 128;
            // GL_MAX_COMPUTE_WORK_GROUP_COUNT / GL_MAX_COMPUTE_WORK_GROUP_SIZE, one value per
            // axis. These six, with the invocations limit above, are the only indexed limits a
            // backend genuinely OWNS - the device answers them (glGetIntegeri_v on DirectGLES,
            // VkPhysicalDeviceLimits::maxComputeWorkGroupCount/Size on DirectVulkan) - and so
            // the only ones that survive the retirement of the GetIntegeri_v table entry: they
            // cross the MGPipe boundary inside MGPCaps, by inclusion of this struct (plan B
            // section 4.4.1). Every other indexed pname names frontend state. RAW driver
            // answers, like the invocations limit: GL_Getter and the compile environment floor
            // them at the shared MIN_COMPUTE_WORK_GROUP_* minimums themselves. The defaults are
            // the GL 4.3 core minimums (table 23.60) and describe the no-backend case, as
            // MaxClipDistances' does.
            Int MaxComputeWorkGroupCount[3] = {65535, 65535, 65535};
            Int MaxComputeWorkGroupSize[3] = {1024, 1024, 64};
            Int MaxShaderStorageBufferBindings = 8;
            Int MaxTextureBufferSize = 65536;
            // GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT; 1 means the offset is unconstrained.
            Int TextureBufferOffsetAlignment = 1;
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
            // GL_MAX_CLIP_DISTANCES. Zero is a legal answer here, not a placeholder, and a
            // backend that cannot host a clip distance MUST report it: advertising eight the
            // backend will refuse does not make gl_ClipDistance work, it only moves the failure
            // from an honest "unsupported" at query time to a backend shader-compile error the
            // frontend never surfaces, after which every draw with that program silently renders
            // nothing. DirectGLES fills it from GL_EXT_clip_cull_distance, DirectVulkan from the
            // shaderClipDistance device feature. The DEFAULT stays at the GL 4.3 core minimum
            // because it describes the no-backend case (standalone shader compiles, unit tests),
            // where there is no device to be honest about and BuildTBuiltInResource still has to
            // hand glslang a workable gl_MaxClipDistances.
            Int MaxClipDistances = 8;
            // GL_MAX_CULL_DISTANCES and GL_MAX_COMBINED_CLIP_AND_CULL_DISTANCES, under exactly
            // the contract stated for MaxClipDistances above: ZERO IS A LEGAL ANSWER and a
            // backend that cannot host a cull distance MUST report it. The failure this prevents
            // is worse than the clip one, because cull distance discards the whole primitive:
            // glslang bounds gl_CullDistance[i] against maxCullDistances and expands
            // gl_MaxCullDistances from it, SPIRV-Cross then emits
            // `#extension GL_EXT_clip_cull_distance : require` into the ESSL, and a host driver
            // without that extension rejects the program in an info log nobody surfaces. These
            // used to be bare 8s inside BuildTBuiltInResource with no backend consulted at all.
            // The DEFAULTS are the GL 4.5 core minimums for the same reason MaxClipDistances'
            // is: they describe the no-backend case (standalone compiles, unit tests).
            Int MaxCullDistances = 8;
            Int MaxCombinedClipAndCullDistances = 8;
            Int MaxViewports = 16;
            // GL_LAYER_PROVOKING_VERTEX / GL_VIEWPORT_INDEX_PROVOKING_VERTEX: which vertex of a
            // primitive supplies gl_Layer and gl_ViewportIndex. GL 4.6 table 23.65 makes
            // GL_UNDEFINED_VERTEX a legal answer for both, and it is the honest default - naming
            // a convention is a statement about behaviour, so a backend that does not pin one
            // must not claim it does. DirectGLES fills the layer one from the ES 3.2 query and
            // the viewport one from GL_OES_viewport_array, and leaves UNDEFINED where the
            // capability is absent: without the viewport array extension only viewport 0 is ever
            // rasterized, so no convention selects anything. DirectVulkan keeps UNDEFINED for
            // both - which vertex provokes is decided per pipeline by
            // VulkanRenderer::SelectProvokingVertexMode out of VK_EXT_provoking_vertex,
            // provokingVertexModePerPipeline and the topology, so no single convention is true
            // of the backend.
            GLenum LayerProvokingVertex = GL_UNDEFINED_VERTEX;
            GLenum ViewportIndexProvokingVertex = GL_UNDEFINED_VERTEX;
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
            // Whether attaching a single layer of a 3D or array texture to a framebuffer actually
            // renders to that layer. DirectGLES hands the layer straight to
            // glFramebufferTextureLayer, so it does; DirectVulkan maps a GL layer onto a Vulkan
            // array layer with no notion of a 3D depth slice, so it does not yet. Defaults to false
            // so a backend that never sets it gets the conservative answer.
            // Which layered texture targets this backend can attach ONE layer of to a framebuffer
            // and then really clear, render and read back that layer. Bit (1u << TextureTarget) is
            // set for each supported target. Deliberately per target rather than one flag: the three
            // ways a GL layer maps onto Vulkan are independent capabilities. A 2D or 2D multisample
            // array layer IS a VkImage array layer and needs nothing extra; a 3D texture's layer is
            // a z slice, which needs a 2D-array-compatible image and a per-slice clear that
            // vkCmdClearColorImage cannot express; a cube map array needs an image shape and the
            // imageCubeArray feature before it can be attached at any layer at all. Defaults to 0 so
            // a backend that never sets it gets the conservative answer.
            Uint32 PerLayerFramebufferAttachmentTargets = 0;

            static constexpr Uint32 PerLayerFramebufferAttachmentBit(TextureTarget target) {
                return (static_cast<Int>(target) >= 0 &&
                        static_cast<Int>(target) < static_cast<Int>(TextureTarget::TextureTargetCount))
                           ? (1u << static_cast<Uint32>(target))
                           : 0u;
            }

            Bool SupportsPerLayerFramebufferAttachment(TextureTarget target) const {
                const Uint32 bit = PerLayerFramebufferAttachmentBit(target);
                return bit != 0 && (PerLayerFramebufferAttachmentTargets & bit) != 0;
            }
            // Whether this backend can CONSUME a shader module that still declares 64-bit floats,
            // i.e. whether `double` survives the transpile instead of being narrowed to `float`
            // (ShaderTranspiler::DemoteFloat64Pass). Detected, never assumed:
            //   * DirectVulkan sets it from VkPhysicalDeviceFeatures::shaderFloat64, the feature
            //     VUID-VkShaderModuleCreateInfo-pCode-08740 requires before a module declaring
            //     OpCapability Float64 may be created at all. lavapipe has it; Adreno and Mali
            //     both report VK_FALSE, so no real mobile device does.
            //   * DirectGLES can NEVER have it. GLSL ES has no 64-bit float type in any version
            //     or extension, so SPIRV-Cross cannot emit one ("FP64 not supported in ES
            //     profile") and the demotion there is mathematically mandatory, always.
            // Defaults to false so a backend that never sets it - and the no-backend case, which
            // is what standalone shader compiles and the unit tests run under - keeps the
            // demotion, which is the behaviour that works everywhere.
            Bool SupportsShaderFloat64 = false;
            // Whether glVertexAttribLFormat / glVertexArrayAttribLFormat can be honoured, i.e.
            // whether a 64-bit vertex attribute can actually reach a shader unconverted. Detected,
            // never assumed: DirectVulkan needs VkPhysicalDeviceFeatures::shaderFloat64 (the
            // attribute travels as its 32-bit word pair, so no VK_FORMAT_R64* is required, but the
            // bitcast result is Float64); DirectGLES can never have it, ESSL having no fp64 type at
            // all. Defaults to false so a backend that never sets it gets the conservative answer.
            //
            // INDEPENDENT of SupportsShaderFloat64, and it has to be: this flag decides a VkFormat
            // from the VAO ATTRIBUTE alone, which does not know what type the shader declared, and
            // glVertexAttribFormat(GL_DOUBLE) feeding a plain `in vec4` is both legal and common
            // (KHR-GL43.vertex_attrib_binding.basic-input-case4/5, advanced-bindingUpdate). A
            // backend with native fp64 that still cannot FETCH 64 bits keeps this false and relies
            // on the per-MODULE rule in ShaderCompiler::SanitizeAndOptimizeBinary instead: a vertex
            // module that declares a 64-bit float INPUT is demoted whole, so the two shader-side
            // halves (PackDoubleVertexInputsPass and VertexInputStateFactory::ToVkVertexFormat)
            // still see one consistent world.
            Bool SupportsFloat64VertexAttributes = false;
            // Whether a TESSELLATION stage of this backend may access gl_PointSize - i.e.
            // whether a module declaring OpCapability TessellationPointSize can reach the
            // driver at all. DirectVulkan sets both this and the geometry twin from the one
            // shaderTessellationAndGeometryPointSize feature; DirectGLES sets them
            // independently from the EXT/OES_tessellation_point_size /
            // geometry_point_size extension pairs (PointSizeTier), which really do come
            // separately. When absent, ProgramSpirvTask demotes the built-in to an ordinary
            // varying program-wide (ShaderCompiler::
            // DemoteTessellationGeometryPointSizeForProgram); MOBILEGL_POINT_SIZE_DEMOTION
            // overrides the detection in either direction at backend init.
            //
            // Defaults TRUE, deliberately against the house "assume absent" rule: false
            // ARMS a rewrite, so the conservative no-backend answer (standalone compiles,
            // unit tests) is the one that leaves modules untouched. A backend that never
            // sets it gets standard modules and, at worst, the old honest declines.
            Bool SupportsTessellationPointSize = true;
            // The geometry-stage twin (OpCapability GeometryPointSize).
            Bool SupportsGeometryPointSize = true;
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
