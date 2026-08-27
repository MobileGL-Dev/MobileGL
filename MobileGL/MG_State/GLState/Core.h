// MobileGL - MobileGL/MG_State/GLState/Core.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "ErrorState/Error.h"
#include "BufferState/BufferState.h"
#include "MG_State/GLState/RenderbufferState/RenderbufferObject.h"
#include "MG_State/GLState/RenderbufferState/RenderbufferState.h"
#include "RenderState/RenderState.h"
#include "ProgramState/ProgramState.h"
#include "ProgramState/ProgramPipelineObject.h"
#include "SamplerState/SamplerState.h"
#include "TextureState/TextureState.h"
#include "FramebufferState/FramebufferState.h"
#include "VertexArrayState/VertexArrayState.h"
#include "RenderbufferState/RenderbufferState.h"

namespace MobileGL::MG_Util::ShaderTranspiler {
    struct CompileEnv;
}

namespace MobileGL {
    namespace MG_State {
        void Init();

        namespace GLState {
            struct CurrentVertexAttributeValue {
                Array<Float, 4> floatValue{0.f, 0.f, 0.f, 1.f};
                Array<Int32, 4> intValue{0, 0, 0, 1};
                Array<Uint32, 4> uintValue{0u, 0u, 0u, 1u};
            };

            // Which of the three views above a shader input of a given GLSL type consumes.
            enum class VertexAttribBaseType { Unsupported, Float, Int, Uint };

            struct VertexAttribTypeInfo {
                VertexAttribBaseType baseType = VertexAttribBaseType::Unsupported;
                Uint componentCount = 0;
            };

            // Maps a shader vertex-input type (GL_FLOAT_VEC3, GL_INT_VEC2, ...) onto the current-value
            // view that feeds it. Shared by every backend so that "a disabled array reads the current
            // value" resolves identically regardless of which backend is active; each backend only
            // translates the result into its own API call.
            VertexAttribTypeInfo ClassifyVertexAttribType(GLenum glType);

            // One indexed capture binding of a transform feedback object, as the by-name queries
            // report it. An empty Buffer means the binding point is unbound.
            struct NamedTransformFeedbackBinding {
                SharedPtr<BufferObject> Buffer;
                Range1D Range{};
                Bool HasExplicitRange = false;
            };

            class GLContext {
            public:
                GLContext() = default;

                // Error
                void RecordError(ErrorCode code, UniquePtr<ErrorInfo> info);
                Bool HasGLError() const;
                Optional<const Error*> PeekNonGLError() const;
                Optional<UniquePtr<Error>> PopNonGLError();
                Bool HasNonGLError() const;
                Optional<const Error*> PeekGLError() const;
                Optional<UniquePtr<Error>> PopGLError();
                void ClearErrors();

                // Buffer
                void GenBufferNames(Uint number, Vector<Uint>& buffers);
                const SharedPtr<BufferObject>& GetBufferObject(Uint index);
                BindingSlot<BufferObject>& GetBufferBindingSlot(BufferTarget target);
                BindingSlotRange1D<BufferObject>& GetBufferBindingPoint(BufferTarget target, Uint index);
                constexpr SizeT GetBufferBindingPointCount(BufferTarget target) const {
                    return m_bufferState.GetBindingPointCount(target);
                }
                void TouchBufferBindingPoint(BufferTarget target, Uint index) {
                    m_bufferState.TouchBindPoint(target, index);
                }
                SizeT GetTouchedBufferBindingPointCount(BufferTarget target) const {
                    return m_bufferState.GetTouchedBindPointCount(target);
                }
                const SharedPtr<BufferObject>& CreateBufferObject(Uint index);
                void MarkBufferObjectForDeletion(Uint index);
                Bool ValidateBufferName(Uint index) const;
                Bool ValidateBufferObject(Uint index) const;

                // VertexArray
                void GenVertexArrayNames(Uint number, Vector<Uint>& vertexArrays);
                const SharedPtr<VertexArrayObject>& GetVertexArrayObject(Uint index);
                void BindVertexArray(Uint index);
                const SharedPtr<VertexArrayObject>& CreateVertexArrayObject(Uint index);
                void MarkVertexArrayForDeletion(Uint index);
                Bool ValidateVertexArrayName(Uint index) const;
                Bool ValidateVertexArrayObject(Uint index) const;
                const SharedPtr<VertexArrayObject>& GetBoundVertexArray();
                void SetCurrentVertexAttributeFloat(Uint index, const Array<Float, 4>& value);
                void SetCurrentVertexAttributeInt(Uint index, const Array<Int32, 4>& value);
                void SetCurrentVertexAttributeUint(Uint index, const Array<Uint32, 4>& value);
                const CurrentVertexAttributeValue& GetCurrentVertexAttribute(Uint index) const;

                // Texture
                void GenTextureNames(Uint number, Vector<Uint>& textures);
                const SharedPtr<ITextureObject>& GetTextureObject(Uint index);
                // Per-target default texture object (name 0); see TextureState::GetDefaultTextureObject.
                const SharedPtr<ITextureObject>& GetDefaultTextureObject(TextureTarget target) const;
                const SharedPtr<ITextureObject>& CreateTextureObject(Uint index, TextureTarget target);
                // See TextureState::CreateTextureViewObject (glTextureView, GL 4.6 core 8.18).
                const SharedPtr<ITextureObject>& CreateTextureViewObject(Uint index, TextureTarget target,
                                                                         const SharedPtr<ITextureObject>& storageOwner,
                                                                         Uint minLevel, Uint numLevels, Uint minLayer,
                                                                         Uint numLayers);
                void MarkTextureObjectForDeletion(Uint index);
                TextureUnit& GetTextureUnitObject(Int unit);
                ImageTextureBinding& GetImageTextureBinding(Int unit);
                const ImageTextureBinding& GetImageTextureBinding(Int unit) const;
                void NoteTextureUnitTouched(Int unit, Bool bindingChanged = true) {
                    m_textureState.NoteUnitTouched(unit, bindingChanged);
                }
                Int GetMaxTouchedTextureUnit() const { return m_textureState.GetMaxTouchedUnit(); }
                // Monotonic counter bumped whenever a texture bind/unbind/delete changes which
                // texture is bound at a unit; lets a backend skip re-resolving an unchanged
                // per-draw sampled-texture set.
                Uint64 GetTextureBindGeneration() const { return m_textureState.GetTextureBindGeneration(); }
                void BumpTextureBindGeneration() { m_textureState.BumpTextureBindGeneration(); }
                // Monotonic counter bumped whenever a texture's shape or a sampler object's
                // parameters change, i.e. whenever a bound texture's mipmap-completeness (and so
                // whether a backend binds it at all) can have flipped without any bind moving;
                // see TextureState::GetSamplingResolutionGeneration.
                Uint64 GetSamplingResolutionGeneration() const {
                    return m_textureState.GetSamplingResolutionGeneration();
                }
                void BumpSamplingResolutionGeneration() { m_textureState.BumpSamplingResolutionGeneration(); }
                // Never-reused id of this context, for backend memos keyed on the two counters
                // above: both restart at 0 in a new context, and a recreated context can land on
                // the old heap address. See TextureState::GetContextId.
                Uint64 GetTextureContextId() const { return m_textureState.GetContextId(); }
                Bool ValidateTextureName(Uint index) const;
                Bool ValidateTextureObject(Uint index) const;
                Int GetActiveTextureUnit() const;
                void SetActiveTextureUnit(Int unit);

                // Program
                Uint CreateProgram();
                Uint CreateShader(ShaderStage stage);
                void MarkProgramForDeletion(Uint index);
                void MarkShaderForDeletion(Uint index);
                // Frees a deletion-flagged shader's name once it lost its last GL-visible
                // attachment (call after glDetachShader).
                void ReleaseShaderNameIfOrphaned(Uint index);
                Bool ValidateProgramName(Uint index) const;
                Bool ValidateShaderName(Uint index) const;
                const SharedPtr<ProgramObject>& GetProgramObject(Uint index);
                const SharedPtr<ShaderObject>& GetShaderObject(Uint index);
                // Settles every compile and link this context still owns; see
                // ProgramState::JoinAllPendingWork. Called by glMaxShaderCompilerThreadsKHR(0).
                void JoinAllPendingShaderWork();
                // P1 stage 6: the per-context index of adoptable compile nodes, for its
                // adoption counter. Diagnostics and tests only - no GL entry point reads it.
                ShaderCompileAdoptionMap& GetShaderCompileAdoptionMap() {
                    return m_programState.GetShaderCompileAdoptionMap();
                }
                void UseProgram(Uint program);
                const SharedPtr<ProgramObject>& GetCurrentProgram();
                // What a DRAW executes: the program in use, or - when there is none - the bound
                // pipeline's GRAPHICS stages composited into one program. A pipeline's compute
                // stage is never part of that composite; ask GetProgramForDispatch for it.
                const SharedPtr<ProgramObject>& GetProgramForDraw();
                // What a DISPATCH executes: the program in use, or - when there is none - the
                // bound pipeline's compute stage program itself. GL's compute stage is a whole
                // program on its own (GL 4.6 core 7.4: it may not be linked with any other
                // stage), so there is nothing to composite and no composite to cache.
                const SharedPtr<ProgramObject>& GetProgramForDispatch();
                // What glUniform* addresses: the program in use, or the bound pipeline's
                // active program (GL 4.6 core 7.6.1).
                const SharedPtr<ProgramObject>& GetProgramForUniform();

                // Program pipeline (GL_ARB_separate_shader_objects, GL 4.6 core 7.4). Like queries
                // and transform feedbacks, glGenProgramPipelines only RESERVES a name - the object
                // appears on first USE (any of bind, UseProgramStages, ActiveShaderProgram,
                // ValidateProgramPipeline) - while glCreateProgramPipelines makes it immediately.
                void GenProgramPipelineNames(Uint number, Vector<Uint>& pipelines);
                void CreateProgramPipelineObject(Uint index);
                Bool ValidateProgramPipelineName(Uint index) const;
                Bool IsProgramPipelineObject(Uint index) const;
                void BindProgramPipelineObject(Uint index);
                // Materializes a reserved name; returns null for 0 or a name that is not a live
                // GenProgramPipelines name.
                const SharedPtr<ProgramPipelineObject>& MaterializeProgramPipelineObject(Uint index);
                void MarkProgramPipelineForDeletion(Uint index);
                const SharedPtr<ProgramPipelineObject>& GetProgramPipelineObject(Uint index) const;
                Uint GetBoundProgramPipelineName() const { return m_boundProgramPipeline; }
                const SharedPtr<ProgramPipelineObject>& GetBoundProgramPipeline() const;

                // RenderState
                Uint GetRenderStateParametersVersion() const;
                // Only the pipeline-relevant subset - see RenderState::m_pipelineStateVersion.
                Uint GetPipelineStateVersion() const;
                const RenderStateParameters& GetRenderStateParameters() const;
                void SetViewport(IntVec4 viewport); // x, y, width, height; writes ALL viewports
                IntVec4 GetViewport() const;        // x, y, width, height; viewport 0, rounded
                void SetViewportIndexed(Uint index, FloatVec4 viewport);
                const FloatVec4& GetViewportIndexed(Uint index) const;
                void SetLineWidth(Float width);
                Float GetLineWidth() const;
                void SetPointSize(Float size);
                Float GetPointSize() const;
                void SetPatchVertices(Uint vertices);
                Uint GetPatchVertices() const;
                void SetPatchDefaultOuterLevel(const FloatVec4& levels);
                const FloatVec4& GetPatchDefaultOuterLevel() const;
                void SetPatchDefaultInnerLevel(const FloatVec2& levels);
                const FloatVec2& GetPatchDefaultInnerLevel() const;
                void SetPolygonOffset(Float factor, Float units);
                Float GetPolygonOffsetFactor() const;
                Float GetPolygonOffsetUnits() const;
                void SetHint(GLenum target, GLenum mode);
                GLenum GetHint(GLenum target) const;
                void SetPointFadeThresholdSize(Float size);
                Float GetPointFadeThresholdSize() const;
                void SetPointSpriteCoordOrigin(GLenum origin);
                GLenum GetPointSpriteCoordOrigin() const;
                void SetClampReadColor(GLenum clamp);
                GLenum GetClampReadColor() const;
                void SetPolygonMode(GLenum front, GLenum back);
                GLenum GetPolygonModeFront() const;
                GLenum GetPolygonModeBack() const;
                void SetPrimitiveRestartIndex(Uint32 index);
                Uint32 GetPrimitiveRestartIndex() const;
                void SetCapability(CapabilityInput cap, Bool enabled);
                Bool IsCapabilityEnabled(CapabilityInput cap) const;
                void SetCapabilityIndexed(CapabilityInput cap, Uint index, Bool enabled);
                Bool IsCapabilityEnabledIndexed(CapabilityInput cap, Uint index) const;
                void SetBlendFunc(BlendFactor srcRGB, BlendFactor dstRGB, BlendFactor srcAlpha, BlendFactor dstAlpha);
                void GetBlendFunc(BlendFactor& srcRGB, BlendFactor& dstRGB, BlendFactor& srcAlpha,
                                  BlendFactor& dstAlpha) const;
                void SetBlendFuncIndexed(Uint index, BlendFactor srcRGB, BlendFactor dstRGB, BlendFactor srcAlpha,
                                         BlendFactor dstAlpha);
                void GetBlendFuncIndexed(Uint index, BlendFactor& srcRGB, BlendFactor& dstRGB, BlendFactor& srcAlpha,
                                         BlendFactor& dstAlpha) const;
                void SetBlendEquation(BlendEquation color, BlendEquation alpha);
                void GetBlendEquation(BlendEquation& color, BlendEquation& alpha) const;
                void SetBlendEquationIndexed(Uint index, BlendEquation color, BlendEquation alpha);
                void GetBlendEquationIndexed(Uint index, BlendEquation& color, BlendEquation& alpha) const;
                void SetLogicOp(LogicOperation logicOp);
                LogicOperation GetLogicOp() const;
                void SetDepthFunc(DepthTestFunc func);
                DepthTestFunc GetDepthFunc() const;
                void SetDepthMask(Bool flag);
                Bool GetDepthMask() const;
                void SetStencilFunc(StencilFace face, DepthTestFunc func, Int ref, Uint32 mask);
                void SetStencilMask(StencilFace face, Uint32 mask);
                void SetStencilOp(StencilFace face, StencilOperation fail, StencilOperation depthFail,
                                  StencilOperation depthPass);
                const StencilFaceState& GetStencilState(StencilFace face) const;
                void SetColorMask(BoolVec4 mask);
                BoolVec4 GetColorMask() const;
                void SetColorMaskIndexed(Uint index, BoolVec4 mask);
                BoolVec4 GetColorMaskIndexed(Uint index) const;
                void SetClearColor(FloatVec4 color);
                const FloatVec4& GetClearColor() const;
                void SetClearDepth(Float depth);
                Float GetClearDepth() const;
                void SetClearStencil(Int stencil);
                Uint32 GetClearStencil() const;
                void SetBlendColor(FloatVec4 color);
                const FloatVec4& GetBlendColor() const;
                void SetDepthRange(FloatVec2 range); // writes ALL viewports' depth ranges
                const FloatVec2& GetDepthRange() const;
                void SetDepthRangeIndexed(Uint index, FloatVec2 range);
                const FloatVec2& GetDepthRangeIndexed(Uint index) const;
                void SetSampleCoverage(Float value, Bool invert);
                Float GetSampleCoverageValue() const;
                Bool GetSampleCoverageInvert() const;
                void SetSampleMaskValue(Uint32 mask);
                Uint32 GetSampleMaskValue() const;
                void SetPixelStoreParam(PixelStoreParam param, Int value);
                Int GetPixelStoreParam(PixelStoreParam param) const;
                PixelStoreParameters GetPixelStoreParameters(Bool isUnpack) const;
                void SetCullFaceMode(CullFaceMode mode);
                CullFaceMode GetCullFaceMode() const;
                void SetFrontFaceMode(FrontFaceMode mode);
                FrontFaceMode GetFrontFaceMode() const;
                void SetProvokingVertexMode(ProvokingVertexMode mode);
                ProvokingVertexMode GetProvokingVertexMode() const;
                void SetScissorBox(IntVec4 box);      // x, y, width, height; writes ALL rectangles
                const IntVec4& GetScissorBox() const; // x, y, width, height; rectangle 0
                void SetScissorBoxIndexed(Uint index, IntVec4 box);
                const IntVec4& GetScissorBoxIndexed(Uint index) const;

                // Transform feedback. The fields below are the state of the transform
                // feedback object currently bound to GL_TRANSFORM_FEEDBACK; see the object
                // block further down for how a bind swaps them.
                void BeginTransformFeedback(GLenum primitiveMode, const SharedPtr<ProgramObject>& program) {
                    m_transformFeedbackActive = true;
                    m_transformFeedbackPaused = false;
                    m_transformFeedbackPrimitiveMode = primitiveMode;
                    m_transformFeedbackProgram = program;
                    m_transformFeedbackGeneration = ++m_transformFeedbackNextGeneration;
                    m_transformFeedbackCapturedVertices = 0;
                    m_transformFeedbackInputPrimitives = 0;
                }
                void EndTransformFeedback() {
                    m_transformFeedbackActive = false;
                    m_transformFeedbackPaused = false;
                    m_transformFeedbackProgram.reset();
                    // What glDrawTransformFeedback on this object replays from now on.
                    auto& object = m_transformFeedbackObjects[m_boundTransformFeedback];
                    object.recordedVertices = m_transformFeedbackCapturedVertices;
                    object.hasCompletedSpan = true;
                }
                Bool IsTransformFeedbackActive() const { return m_transformFeedbackActive; }
                Bool IsTransformFeedbackPaused() const { return m_transformFeedbackPaused; }
                void SetTransformFeedbackPaused(Bool paused) { m_transformFeedbackPaused = paused; }
                GLenum GetTransformFeedbackPrimitiveMode() const { return m_transformFeedbackPrimitiveMode; }
                const SharedPtr<ProgramObject>& GetTransformFeedbackProgram() const {
                    return m_transformFeedbackProgram;
                }
                // Bumped on every BeginTransformFeedback; the backend uses it to
                // distinguish "resume appending" from "fresh capture".
                Uint64 GetTransformFeedbackGeneration() const { return m_transformFeedbackGeneration; }
                // CPU-side primitive accounting for the transform feedback queries:
                // every captured draw adds its primitive count (draws without a
                // geometry stage write exactly what they generate).
                void AddTransformFeedbackPrimitives(Uint64 primitives) {
                    m_transformFeedbackPrimitiveCounter += primitives;
                }
                Uint64 GetTransformFeedbackPrimitiveCounter() const { return m_transformFeedbackPrimitiveCounter; }
                // Primitives a draw assembled while the capture was paused. GL counts those in
                // PRIMITIVES_GENERATED, but a backend that answers the query with its own
                // transform feedback counter cannot see them - nothing was being captured.
                void AddTransformFeedbackPausedPrimitives(Uint64 primitives) {
                    m_transformFeedbackPausedPrimitiveCounter += primitives;
                    m_transformFeedbackGeneratedPrimitiveCounter += primitives;
                }
                Uint64 GetTransformFeedbackPausedPrimitiveCounter() const {
                    return m_transformFeedbackPausedPrimitiveCounter;
                }
                // Vertices already captured since BeginTransformFeedback (drives the
                // buffer-capacity clamp on the primitives-written accounting).
                void AddTransformFeedbackCapturedVertices(Uint64 vertices) {
                    m_transformFeedbackCapturedVertices += vertices;
                }
                Uint64 GetTransformFeedbackCapturedVertices() const { return m_transformFeedbackCapturedVertices; }
                // Raw assembled input primitives fed to the capture stage since Begin
                // (pre-clamp; drives the GS strip capture-order fixup at EndTF).
                void AddTransformFeedbackInputPrimitives(Uint64 primitives) {
                    m_transformFeedbackInputPrimitives += primitives;
                    m_transformFeedbackGeneratedPrimitiveCounter += primitives;
                }
                Uint64 GetTransformFeedbackInputPrimitives() const { return m_transformFeedbackInputPrimitives; }
                // What a GL_PRIMITIVES_GENERATED query counts over its span: every primitive the
                // capture stage assembled, including the ones a paused span discarded (those are
                // generated but never written). Kept as its own running total rather than derived
                // from the input counter above, which BeginTransformFeedback resets per span while
                // a query may cover several of them.
                Uint64 GetTransformFeedbackGeneratedCounter() const {
                    return m_transformFeedbackGeneratedPrimitiveCounter;
                }
                // Capture draws whose written-primitive count the CPU accounting reproduced
                // exactly, and the subset it could not: a program with a geometry stage amplifies
                // by whatever the shader emits, which only the driver's own counter knows. The
                // transform feedback queries diff both over their span to decide whether the CPU
                // delta may stand in for the backend's GPU result (GL_Query.cpp).
                void AddTransformFeedbackAccountedCaptureDraw() { ++m_transformFeedbackAccountedCaptureDraws; }
                Uint64 GetTransformFeedbackAccountedCaptureDraws() const {
                    return m_transformFeedbackAccountedCaptureDraws;
                }
                void AddTransformFeedbackGeometryCaptureDraw() { ++m_transformFeedbackGeometryCaptureDraws; }
                Uint64 GetTransformFeedbackGeometryCaptureDraws() const {
                    return m_transformFeedbackGeometryCaptureDraws;
                }

                // Conditional rendering (GL 4.6 core 10.9). `discard` is the verdict already
                // resolved from the query object at glBeginConditionalRender - the predicate is
                // read ONCE there, not per command, because GL specifies the block against the
                // result available at Begin and re-reading it would let a query that is still
                // being written change the answer mid-block.
                void BeginConditionalRender(GLuint queryId, GLenum mode, Bool discard) {
                    m_conditionalRenderActive = true;
                    m_conditionalRenderQuery = queryId;
                    m_conditionalRenderMode = mode;
                    m_conditionalRenderDiscards = discard;
                }
                void EndConditionalRender() {
                    m_conditionalRenderActive = false;
                    m_conditionalRenderQuery = 0;
                    m_conditionalRenderMode = GL_NONE;
                    m_conditionalRenderDiscards = false;
                }
                Bool IsConditionalRenderActive() const { return m_conditionalRenderActive; }
                GLuint GetConditionalRenderQuery() const { return m_conditionalRenderQuery; }
                // Whether the commands GL 4.6 core 10.9 makes conditional are being discarded
                // right now. False whenever no block is open, so a caller needs no second test.
                Bool ConditionalRenderDiscardsCommands() const {
                    return m_conditionalRenderActive && m_conditionalRenderDiscards;
                }

                // Transform feedback objects (ARB_transform_feedback2 / GL 4.0 core).
                // The capture state above and the indexed GL_TRANSFORM_FEEDBACK_BUFFER
                // binding points are object state, but the context keeps exactly one live
                // copy of both so that every existing reader - the backends' per-draw sync,
                // the drawing and getter paths - needs no notion of which object owns them.
                // A bind therefore saves the live copy into the outgoing object and restores
                // the incoming one's. Object 0 is the default object and always exists.
                static constexpr Uint MAX_TRANSFORM_FEEDBACK_BUFFERS = 4;
                void GenTransformFeedbackNames(Uint number, Vector<Uint>& ids);
                // A name glGenTransformFeedbacks handed out and glDeleteTransformFeedbacks
                // has not taken back. Name 0 is always valid.
                Bool ValidateTransformFeedbackName(Uint index) const;
                // What glIsTransformFeedback reports: a generated name only becomes the name
                // of an object once it has been bound at least once (GL 4.6 core 13.2.1).
                Bool IsTransformFeedbackObject(Uint index) const;
                void BindTransformFeedbackObject(Uint index);
                void MarkTransformFeedbackObjectForDeletion(Uint index);
                Uint GetBoundTransformFeedbackName() const { return m_boundTransformFeedback; }
                // Vertices the object captured in its last completed span; the vertex count
                // glDrawTransformFeedback replays.
                Uint64 GetTransformFeedbackRecordedVertices(Uint index) const;
                // Whether the object has ever completed a capture span. glDrawTransformFeedback
                // on an object that has not is INVALID_OPERATION, which a zero vertex count
                // cannot express: an empty completed span is legal and draws nothing.
                Bool HasTransformFeedbackCompletedSpan(Uint index) const;

                // The by-name (direct state access) view. A named object that happens to be the
                // bound one is answered from the live copy, since that is where its state actually
                // is until a bind swaps it out.
                void CreateTransformFeedbackObject(Uint index);
                Bool IsNamedTransformFeedbackActive(Uint index) const;
                Bool IsNamedTransformFeedbackPaused(Uint index) const;
                NamedTransformFeedbackBinding GetNamedTransformFeedbackBinding(Uint index, Uint bufferIndex) const;
                void SetNamedTransformFeedbackBinding(Uint index, Uint bufferIndex,
                                                      const SharedPtr<BufferObject>& buffer, Range1D range,
                                                      Bool hasExplicitRange);

                // Framebuffer
                void GenFramebufferNames(Uint number, Vector<Uint>& framebuffers);
                const SharedPtr<FramebufferObject>& GetFramebufferObject(Uint index);
                BindingSlot<FramebufferObject>& GetFramebufferBindingSlot(FramebufferTarget target);
                const SharedPtr<FramebufferObject>& CreateFramebufferObject(Uint index);
                void MarkFramebufferObjectForDeletion(Uint index);
                Bool ValidateFramebufferName(Uint index) const;
                Bool ValidateFramebufferObject(Uint index) const;

                // Sampler
                void GenSamplerNames(Uint number, Vector<Uint>& samplers);
                const SharedPtr<SamplerObject>& GetSamplerObject(Uint index);
                const SharedPtr<SamplerObject>& CreateSamplerObject(Uint index);
                void MarkSamplerObjectForDeletion(Uint index);
                Bool ValidateSamplerName(Uint index) const;
                Bool ValidateSamplerObject(Uint index) const;

                // Renderbuffer
                void GenRenderbufferNames(Uint number, Vector<Uint>& renderbuffers);
                const SharedPtr<RenderbufferObject>& GetRenderbufferObject(Uint index);
                BindingSlot<RenderbufferObject>& GetRenderbufferBindingSlot(RenderbufferTarget target);
                const SharedPtr<RenderbufferObject>& CreateRenderbufferObject(Uint index);
                void MarkRenderbufferObjectForDeletion(Uint index);
                Bool ValidateRenderbufferName(Uint index) const;
                Bool ValidateRenderbufferObject(Uint index) const;

                // P1: the shader compile/link pipeline's snapshot of everything it reads from
                // outside its own (stage, source) inputs. Captured lazily here because it
                // cannot be captured in MG_State::Init() - that runs BEFORE MG_Backend::Init(),
                // so there is no backend to query yet. Re-captured whenever the active backend
                // object changes, which also rolls the fingerprint and therefore invalidates
                // every P0b preprocess memo keyed against the old one. A backend whose dynamic
                // capabilities become available without changing object identity must call
                // InvalidateCompileEnv() after publishing them.
                // GL thread only.
                const SharedPtr<const MG_Util::ShaderTranspiler::CompileEnv>& GetCompileEnv();
                void InvalidateCompileEnv();

            private:
                // State Components
                ErrorState m_errorState;
                BufferState m_bufferState;
                VertexArrayState m_vertexArrayState;
                Array<CurrentVertexAttributeValue, VertexArrayObject::MAX_VERTEX_ATTRIBS> m_currentVertexAttributes{};
                Bool m_transformFeedbackActive = false;
                Bool m_transformFeedbackPaused = false;
                GLenum m_transformFeedbackPrimitiveMode = GL_POINTS;
                SharedPtr<ProgramObject> m_transformFeedbackProgram;
                Uint64 m_transformFeedbackGeneration = 0;
                // Source of the per-span ids above; never rolls back with an object switch.
                Uint64 m_transformFeedbackNextGeneration = 0;
                // Not object state: the transform feedback queries snapshot it at BeginQuery
                // and take the delta at EndQuery, which spans whatever objects were used.
                Uint64 m_transformFeedbackPrimitiveCounter = 0;
                Uint64 m_transformFeedbackPausedPrimitiveCounter = 0;
                Uint64 m_transformFeedbackCapturedVertices = 0;
                Uint64 m_transformFeedbackInputPrimitives = 0;
                Uint64 m_transformFeedbackGeneratedPrimitiveCounter = 0;
                Uint64 m_transformFeedbackAccountedCaptureDraws = 0;
                Uint64 m_transformFeedbackGeometryCaptureDraws = 0;

                // Conditional rendering. Context state, not object state: GL 4.6 core 10.9 allows
                // exactly one block open at a time and no object owns it.
                Bool m_conditionalRenderActive = false;
                Bool m_conditionalRenderDiscards = false;
                GLuint m_conditionalRenderQuery = 0;
                GLenum m_conditionalRenderMode = GL_NONE;

                // Everything a transform feedback object owns while it is NOT the bound one.
                struct TransformFeedbackObjectState {
                    struct SavedBufferBinding {
                        SharedPtr<BufferObject> buffer;
                        Range1D range;
                        Bool hasExplicitRange = false;
                    };
                    Array<SavedBufferBinding, MAX_TRANSFORM_FEEDBACK_BUFFERS> bindings;
                    Bool active = false;
                    Bool paused = false;
                    GLenum primitiveMode = GL_POINTS;
                    SharedPtr<ProgramObject> program;
                    Uint64 generation = 0;
                    Uint64 capturedVertices = 0;
                    Uint64 inputPrimitives = 0;
                    Uint64 recordedVertices = 0;
                    Bool hasCompletedSpan = false;
                    Bool everBound = false;
                };
                void SaveBoundTransformFeedbackState();
                void RestoreBoundTransformFeedbackState();
                // operator[] materialises an entry with the default state on first touch, so
                // the default object (name 0) needs no seeding here.
                UnorderedMap<Uint, TransformFeedbackObjectState> m_transformFeedbackObjects;
                IndexGenerator<Uint> m_transformFeedbackNames;
                Uint m_boundTransformFeedback = 0;
                // Map membership is object EXISTENCE, which is not the same as the answer
                // glIsProgramPipeline gives: any command that needs somewhere to put state
                // materializes a reserved name, so the object can exist well before it is
                // bound. ProgramPipelineObject::everBound carries the Is* answer.
                UnorderedMap<Uint, SharedPtr<ProgramPipelineObject>> m_programPipelines;
                IndexGenerator<Uint> m_programPipelineNames;
                Uint m_boundProgramPipeline = 0;
                TextureState m_textureState;
                ProgramState m_programState;
                RenderState m_renderState;
                FramebufferState m_framebufferState;
                SamplerState m_samplerState;
                RenderbufferState m_renderbufferState;

                mutable SharedPtr<const MG_Util::ShaderTranspiler::CompileEnv> m_compileEnv;
                // Identity of the backend object m_compileEnv was captured against; a plain
                // pointer compare, never dereferenced.
                const void* m_compileEnvBackend = nullptr;
            };
        } // namespace GLState

        extern UniquePtr<GLState::GLContext>& pGLContext;

        // True when relaxed GL semantics apply. Strict core rules are enforced only when the
        // current EGL context explicitly requested a core profile (core bit in
        // EGL_CONTEXT_OPENGL_PROFILE_MASK, or a >=3.1 version request without the compatibility
        // bit) and MOBILEGL_RELAXED_SEMANTICS is off; no current context, legacy version
        // requests, and the compatibility bit all relax.
        Bool IsRelaxedSemanticsActive();
    } // namespace MG_State
} // namespace MobileGL
