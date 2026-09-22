// MobileGL - MobileGL/MG_Impl/Pipe/PipeFill.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The client side of the PipeInputs block (ARCHITECTURE.md 9.2 phase A): the only place in
// the push arm that reads MG_State::pGLContext. Holds the per-verb filler, the F-class
// forwarders, IsLive, the MOBILEGL_PIPE_POISON_OMIT knob and - in a verify build - the
// second arm (SnapshotFromGLContext), the entry compare, the compare-at-read hook and the
// MOBILEGL_PIPE_VERIFY_CORRUPT / _FATAL knobs. Compiled only under MOBILEGL_PIPE_PUSH
// (CMakeLists.txt appends it to SOURCE_FILES there).
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/BufferState/BufferState.h>
// C-1: the vertex-elements CSO's death path raises the backend notice from here, between the
// applier's delete and the slot free, so that the whole order lives in one place.
#include <MG_State/GLState/StateObjectDeathNotice.h>
#include <MG_Backend/MGPipe/PipeInputs.h>
#include <MG_Impl/Pipe/CsoCache.h>
// P4a's five client emitters. This translation unit is the ONLY one that includes them in the
// library, exactly as it is for Tracker.h, CsoCache.h, ResourceTracker.h and VertexInputEmit.h
// - all of them header-only for the same ownership reason. Each carries its family's
// kMGPipeWired*Subsystem constant, so the bit that switches a family on is added by the commit
// that gives that family's emitters their bodies, and no two packages ever edit one file.
#include <MG_Impl/Pipe/FramebufferEmit.h>
#include <MG_Impl/Pipe/ImageEmit.h>
#include <MG_Impl/Pipe/PipeFill.h>
#include <MG_Impl/Pipe/ProgramEmit.h>
#include <MG_Impl/Pipe/ResourceTracker.h>
#include <MG_Impl/Pipe/SamplerEmit.h>
#include <MG_Impl/Pipe/SetHashSuppressor.h>
#include <MG_Impl/Pipe/ShaderBufferEmit.h>
#include <MG_Impl/Pipe/TextureEmit.h>
#include <MG_Impl/Pipe/Tracker.h>
#include <MG_Impl/Pipe/VertexInputEmit.h>
#include <MG_Pipe/MGPipeRenderStateSpans.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Pipe/PipeRoute.h>
#include <MG_Pipe/PipeMutation.h>
#include <Config.h>

#if MOBILEGL_BUILD_DISAGGREGATED
// R-8 (c1): the client's liveness gates read the caps mirror, never MGPipeGetResourceOps().
// Behind the build option for G1's reason - nothing under MG_Remote may be reachable from a
// pull build - and every use below is additionally gated on the resolved TRANSPORT, because
// build-split runs MOBILEGL_TRANSPORT=monolith in every unit and integration-gpu lane and those
// lanes must keep answering exactly what they answered before.
#include <MG_Remote/Client/CapsMirror.h>
// MGPipeStageChunkBytes: the resource emitters' content cap (see MGPipeContentChunkCap below).
#include <MG_Remote/Client/GpuWritePending.h>
#include <MG_Remote/Client/WireTables.h>
// P5c gt (CONTRACT-P5C §6 layer 2, audit A1): the client-side gPipeInputs check consults
// InBarrierWait() and ApplyThreadIsInsideApplier() - both live here.
#include <MG_Remote/Client/ClientSession.h>
// P5f fv: backend errors use the same owned reverse callback table as other events.
// The producer, rather than this client-side translation unit, knows the server session.
#include <MG_Pipe/MGPipeCallbacks.h>
#endif

#include <atomic>
#include <mutex>
#include <cstdlib>
#include <cstring>

namespace MobileGL::MG_Pipe {
    using GLContext = MG_State::GLState::GLContext;

    // The one door into PipeInputs' storage on the client side. A struct rather than a
    // list of friend functions so the header names exactly one friend.
    struct MGPipeFillAccess {
        // Copies ONE field's storage out of the live context by calling the GLContext
        // accessor of the same name (P1 brief D4: no derivation logic is re-implemented
        // here, which is what keeps the copy semantically identical by construction).
        // A forwarded field has no storage and copies nothing.
        // The two doors the P2 emission step needs into PipeInputs' storage. They exist
        // only for ApplierDerivesRenderStateFields' one-shot probe below; nothing on the hot
        // path writes through them.
        static RenderStateParameters& RenderStateOf(PipeInputs& inputs) { return inputs.m_renderState; }
        static Uint32& ClearStencilOf(PipeInputs& inputs) { return inputs.m_clearStencil; }
        // Read-only, and it exists for one thing: after set_vertex_attrib_defaults goes out,
        // the emitter compares what the applier left here against what the frontend holds
        // (EmitVertexAttribDefaults). Reading it through this door rather than through the
        // accessor is deliberate - the accessor is poison-checked and this is a fill-time
        // read, not a backend read.
        static const PipeInputs::CurrentVertexAttributeValue* VertexAttribDefaultsOf(const PipeInputs& inputs) {
            return inputs.m_currentVertexAttribute;
        }

#if MOBILEGL_BUILD_DISAGGREGATED
        // P5e (ra, CONTRACT-P5E §2.3). THE FOUR O-CLASS ROWS, AND ONLY THEY: these are the only
        // members of this block that own a frontend object rather than point into one, so they
        // are the only ones through which the apply thread can become a last owner. The raw
        // pointer rows (the binding-slot bases) are borrowed from a live GLContext and own
        // nothing, so releasing them would buy no lifetime and lose the monolith's arms.
        static void ReleaseObjectPins(PipeInputs& inputs) {
            inputs.m_boundVertexArray.reset();
            inputs.m_programForDispatch.reset();
            inputs.m_programForDraw.reset();
            inputs.m_transformFeedbackProgram.reset();
        }
#endif

        static void CopyField(PipeInputs& dst, GLContext& ctx, MGPipeInputField field) {
            using F = MGPipeInputField;
            using MG_State::GLState::BufferBindPointTargets;
            using MG_State::GLState::GlobalBufferTargets;
            switch (field) {
            case F::GetActiveTextureUnit:
                dst.m_activeTextureUnit = ctx.GetActiveTextureUnit();
                break;
            case F::GetBlendColor:
                dst.m_blendColor = ctx.GetBlendColor();
                break;
            case F::GetBlendEquationIndexed:
                for (Uint i = 0; i < kMGMaxDrawBuffers; ++i) {
                    ctx.GetBlendEquationIndexed(i, dst.m_blendEquation[i][0], dst.m_blendEquation[i][1]);
                }
                break;
            case F::GetBlendFuncIndexed:
                for (Uint i = 0; i < kMGMaxDrawBuffers; ++i) {
                    ctx.GetBlendFuncIndexed(i, dst.m_blendFunc[i][0], dst.m_blendFunc[i][1], dst.m_blendFunc[i][2],
                                            dst.m_blendFunc[i][3]);
                }
                break;
            case F::GetBoundTransformFeedbackName:
                dst.m_boundTransformFeedbackName = ctx.GetBoundTransformFeedbackName();
                break;
            case F::GetBoundVertexArray:
                dst.m_boundVertexArray = ctx.GetBoundVertexArray();
                break;
            case F::GetBufferBindingSlot:
                // Every global target has a slot; Index stays null - GLContext resolves it
                // through the bound VAO's element-buffer slot (Core.cpp), a derivation no
                // FillPoints.def row can copy - and a read of it is the poison Fatal in the
                // accessor. No backend reads it today (every slot read is DrawIndirect,
                // DispatchIndirect, Parameter or PixelPack).
                for (const auto target : GlobalBufferTargets) {
                    dst.m_bufferBindingSlot[static_cast<SizeT>(target)] = &ctx.GetBufferBindingSlot(target);
                }
                break;
            case F::GetBufferBindingPoint:
                // The live storage is Array<Array<BindingSlotRange1D, BufferBindingPointCount>, N>
                // (BufferState.h), so the address of point 0 is the base of that target's row.
                for (const auto target : BufferBindPointTargets) {
                    dst.m_bufferBindingPointBase[static_cast<SizeT>(target)] = &ctx.GetBufferBindingPoint(target, 0);
                }
                break;
            case F::GetTouchedBufferBindingPointCount:
                for (const auto target : BufferBindPointTargets) {
                    dst.m_touchedBindingPointCount[static_cast<SizeT>(target)] =
                        ctx.GetTouchedBufferBindingPointCount(target);
                }
                break;
            case F::GetClampReadColor:
                dst.m_clampReadColor = ctx.GetClampReadColor();
                break;
            case F::GetClearColor:
                dst.m_clearColor = ctx.GetClearColor();
                break;
            case F::GetClearDepth:
                dst.m_clearDepth = ctx.GetClearDepth();
                break;
            case F::GetClearStencil:
                dst.m_clearStencil = ctx.GetClearStencil();
                break;
            case F::GetColorMaskIndexed:
                for (Uint i = 0; i < kMGMaxDrawBuffers; ++i) {
                    dst.m_colorMask[i] = ctx.GetColorMaskIndexed(i);
                }
                break;
            case F::GetCullFaceMode:
                dst.m_cullFaceMode = ctx.GetCullFaceMode();
                break;
            case F::GetCurrentVertexAttribute:
                for (Uint i = 0; i < PipeInputs::kMaxVertexAttribs; ++i) {
                    dst.m_currentVertexAttribute[i] = ctx.GetCurrentVertexAttribute(i);
                }
                break;
            case F::GetDepthFunc:
                dst.m_depthFunc = ctx.GetDepthFunc();
                break;
            case F::GetDepthMask:
                dst.m_depthMask = ctx.GetDepthMask();
                break;
            case F::GetDepthRangeIndexed:
                for (Uint i = 0; i < PipeInputs::kMaxViewports; ++i) {
                    dst.m_depthRange[i] = ctx.GetDepthRangeIndexed(i);
                }
                break;
            case F::GetFramebufferBindingSlot:
                for (SizeT i = 0; i < PipeInputs::kFramebufferTargetCount; ++i) {
                    dst.m_framebufferBindingSlot[i] =
                        &ctx.GetFramebufferBindingSlot(static_cast<PipeInputs::FramebufferTarget>(i));
                }
                break;
            case F::GetImageTextureBinding:
                // Array<ImageTextureBinding, MAX_TEXTURE_IMAGE_UNITS> (TextureState.h): unit 0's
                // address is the base.
                dst.m_imageTextureBindingBase = &ctx.GetImageTextureBinding(0);
                break;
            case F::GetLineWidth:
                dst.m_lineWidth = ctx.GetLineWidth();
                break;
            case F::GetLogicOp:
                dst.m_logicOp = ctx.GetLogicOp();
                break;
            case F::GetMaxTouchedTextureUnit:
                dst.m_maxTouchedTextureUnit = ctx.GetMaxTouchedTextureUnit();
                break;
            case F::GetMinSampleShadingValue:
                dst.m_minSampleShadingValue = ctx.GetMinSampleShadingValue();
                break;
            case F::GetPatchDefaultInnerLevel:
                dst.m_patchDefaultInnerLevel = ctx.GetPatchDefaultInnerLevel();
                break;
            case F::GetPatchDefaultOuterLevel:
                dst.m_patchDefaultOuterLevel = ctx.GetPatchDefaultOuterLevel();
                break;
            case F::GetPatchVertices:
                dst.m_patchVertices = ctx.GetPatchVertices();
                break;
            case F::GetPipelineStateVersion:
                dst.m_pipelineStateVersion = ctx.GetPipelineStateVersion();
                break;
            case F::GetPixelStoreParameters:
                dst.m_pixelStore[0] = ctx.GetPixelStoreParameters(false);
                dst.m_pixelStore[1] = ctx.GetPixelStoreParameters(true);
                break;
            case F::GetPolygonModeFront:
                dst.m_polygonModeFront = ctx.GetPolygonModeFront();
                break;
            case F::GetPolygonOffsetFactor:
                dst.m_polygonOffsetFactor = ctx.GetPolygonOffsetFactor();
                break;
            case F::GetPolygonOffsetUnits:
                dst.m_polygonOffsetUnits = ctx.GetPolygonOffsetUnits();
                break;
            case F::GetPrimitiveRestartIndex:
                dst.m_primitiveRestartIndex = ctx.GetPrimitiveRestartIndex();
                break;
            case F::GetProgramForDispatch:
                dst.m_programForDispatch = ctx.GetProgramForDispatch();
                break;
            case F::GetProgramForDraw:
                dst.m_programForDraw = ctx.GetProgramForDraw();
                break;
            case F::GetProvokingVertexMode:
                dst.m_provokingVertexMode = ctx.GetProvokingVertexMode();
                break;
            case F::GetRenderStateParameters:
                dst.m_renderState = ctx.GetRenderStateParameters();
                break;
            case F::GetRenderStateParametersVersion:
                dst.m_renderStateParametersVersion = ctx.GetRenderStateParametersVersion();
                break;
            case F::GetSamplingResolutionGeneration:
                dst.m_samplingResolutionGeneration = ctx.GetSamplingResolutionGeneration();
                break;
            case F::GetScissorBox:
                dst.m_scissorBox = ctx.GetScissorBox();
                break;
            case F::GetStencilState:
                dst.m_stencil[0] = ctx.GetStencilState(StencilFace::Front);
                dst.m_stencil[1] = ctx.GetStencilState(StencilFace::Back);
                break;
            case F::GetTextureBindGeneration:
                dst.m_textureBindGeneration = ctx.GetTextureBindGeneration();
                break;
            case F::GetTextureContextId:
                dst.m_textureContextId = ctx.GetTextureContextId();
                break;
            case F::GetTextureUnitObject:
                // Array<TextureUnit, MAX_TEXTURE_IMAGE_UNITS> (TextureState.h): unit 0 is the base.
                dst.m_textureUnitBase = &ctx.GetTextureUnitObject(0);
                break;
            case F::GetTransformFeedbackCapturedVertices:
                dst.m_transformFeedbackCapturedVertices = ctx.GetTransformFeedbackCapturedVertices();
                break;
            case F::GetTransformFeedbackGeneration:
                dst.m_transformFeedbackGeneration = ctx.GetTransformFeedbackGeneration();
                break;
            case F::GetTransformFeedbackPausedPrimitiveCounter:
                dst.m_transformFeedbackPausedPrimitiveCounter = ctx.GetTransformFeedbackPausedPrimitiveCounter();
                break;
            case F::GetTransformFeedbackProgram:
                dst.m_transformFeedbackProgram = ctx.GetTransformFeedbackProgram();
                break;
            case F::GetViewport:
                dst.m_viewport = ctx.GetViewport();
                break;
            case F::GetViewportIndexed:
                for (Uint i = 0; i < PipeInputs::kMaxViewports; ++i) {
                    dst.m_viewportIndexed[i] = ctx.GetViewportIndexed(i);
                }
                break;
            case F::IsCapabilityEnabled:
                // Every capability, FramebufferSrgb included: it copies today's constant false
                // (MEASUREMENTS.md), so no value changes.
                for (SizeT i = 0; i < PipeInputs::kCapabilityCount; ++i) {
                    dst.m_capability[i] = ctx.IsCapabilityEnabled(static_cast<CapabilityInput>(i));
                }
                break;
            case F::IsCapabilityEnabledIndexed:
                // The only two indexed capabilities GLContext keeps (RenderState).
                for (Uint i = 0; i < kMGMaxDrawBuffers; ++i) {
                    dst.m_capabilityIndexed.Blend[i] = ctx.IsCapabilityEnabledIndexed(CapabilityInput::Blend, i);
                }
                for (Uint i = 0; i < PipeInputs::kMaxViewports; ++i) {
                    dst.m_capabilityIndexed.ScissorTest[i] =
                        ctx.IsCapabilityEnabledIndexed(CapabilityInput::ScissorTest, i);
                }
                break;
            case F::IsTransformFeedbackActive:
                dst.m_transformFeedbackActive = ctx.IsTransformFeedbackActive();
                break;
            case F::IsTransformFeedbackPaused:
                dst.m_transformFeedbackPaused = ctx.IsTransformFeedbackPaused();
                break;
            case F::GetBoundTransformFeedbackLifetimeId:
                dst.m_boundTransformFeedbackLifetimeId = ctx.GetBoundTransformFeedbackLifetimeId();
                break;
            // The seven forwarded fields: nothing to copy.
            case F::GetBufferBindingPointCount:
            case F::GetProgramObject:
            case F::GetTextureObject:
            case F::HasOpenTransformFeedbackSpan:
            case F::InvalidateCompileEnv:
            case F::ValidateProgramName:
            case F::RecordError:
            case F::kFieldCount:
                break;
            }
        }

        static void SetIdentity(PipeInputs& inputs, GLContext* ctx) {
            inputs.m_live = ctx != nullptr;
            inputs.m_contextIdentity = ctx;
        }
        static void SetVerb(PipeInputs& inputs, MGPipeVerb verb) { inputs.m_currentVerb = verb; }
#if MOBILEGL_PIPE_POISON
        static MGPipeFilledState& Filled(PipeInputs& inputs) { return inputs.m_filled; }
#endif
    };

    namespace {
        GLContext* LiveContext() { return MG_State::pGLContext.get(); }

#if MOBILEGL_BUILD_DISAGGREGATED
        // ---- P5e (ra): the fill decision (CONTRACT-P5E §3.1) -----------------------------
        //
        // Is this process a run-ahead CLIENT right now? Three questions, cheapest first, and
        // the last one is the latch ClientSession took at its first caps adoption - so this is
        // one pointer test and one bool load on the verb path once the first two are constant.
        Bool ClientRunsAhead() {
            if (MG_Config::Transport == MG_Config::TransportMode::Monolith) return false;
            if (MG_Remote::Client::RunsAsTheServerRole()) return false;
            const MG_Remote::Client::ClientSession* session = MG_Remote::Client::ClientSession::Active();
            return session != nullptr && session->RunAheadArmed();
        }

        // THE WIRE OP A VERB BOUNDARY BECOMES, which is the join MGPipeVerbForWireOp draws in
        // the other direction. Built by walking the op space once at compile time rather than
        // written out, because a second hand-written table is a second thing to forget a row
        // in - and the generator already refuses a verb-shaped op with no row.
        constexpr MGPWireOp WireOpForVerb(MGPipeVerb verb) {
            for (SizeT i = 0; i < static_cast<SizeT>(MGPWireOp::kOpCount); ++i) {
                const auto op = static_cast<MGPWireOp>(i);
                if (MGPipeVerbForWireOp(op) == verb) return op;
            }
            return MGPWireOp::kOpCount;
        }

        // CONTRACT-P5E §2.1's predicate, ASKED AT THE VALIDATE POINT - which is before the
        // record exists, so it is asked of the verb and the live context rather than of the
        // payload. The two halves must agree with MGPipeBarriered(op, payload, applierState),
        // which is what the server computes, so each clause is the same clause:
        //
        //   1. the static WaitClass column          - the same generated table, same op;
        //   2. kCtxVerb inside an open XFB span     - ctx.IsTransformFeedbackActive() here,
        //      the applied MGPContextValues mirror there, and set_context_values precedes the
        //      verb on the ring, so the two read the same value (§2.1);
        //   3. a draw carrying kDrawClientArrays    - NOT asked here, because under run-ahead
        //      such a draw never reaches a record at all: vi refuses it on this very thread in
        //      EmitDrawRecord's array arm (§5.1, ruling 2). Asking it here would be a second
        //      spelling of vi's "does any enabled attribute lack a buffer" walk, and two
        //      spellings of an escalation is precisely what ruling 3 replaced.
        //
        // AN OP WITH NO VERB ROW ANSWERS BARRIERED. A verb the join does not know is one this
        // file cannot reason about, and the safe answer - fill it, wait for it - is also the
        // pre-P5e answer.
        // `ctx` MAY BE NULL, and that is not a convenience: the validate point asks this
        // question BEFORE it has decided whether there is anything to fill, so that the answer
        // is about the RECORD and not about the state of the block. A null context has no open
        // transform-feedback span, so clause 2 is false for it - and the verb is a no-op below
        // either way.
        Bool ClientVerbIsBarriered(MGPipeVerb verb, GLContext* ctx) {
            MGPWireOp op = WireOpForVerb(verb);
            // ---- P5e (ra2): THE JOIN IS MANY-TO-ONE ON THE DRAW FAMILY, SO THE INVERSE IS NOT
            // A FUNCTION, AND THE DEFAULT BELOW WAS ANSWERING FOR NINETEEN VERBS -----------------
            //
            // MGP_VERB_OP_LIST carries ONE row for the whole draw family - `DrawVbo ->
            // DrawArrays` - because that is the direction the SERVER needs: a draw_vbo record
            // stamps a verb boundary and DrawArrays is the name it prints. Inverting it
            // verb-first therefore answers kOpCount for DrawElements, DrawElementsBaseVertex,
            // DrawElementsIndirect, MultiDrawElementsBaseVertex and every other indexed /
            // instanced / multi / indirect verb - all of which emit exactly that same draw_vbo.
            //
            // The conservative default below reads "a verb the join does not know answers
            // BARRIERED: fill it, wait for it". THE FILL HAPPENS AND THE WAIT DOES NOT. The wait
            // is not decided here - it is decided per RECORD, by MGPipeBarriered(op, payload,
            // st) at the publish (§2.1) - and the record is a draw_vbo, whose wait class is
            // kWaitNone. So every indexed draw filled gPipeInputs while telling
            // RefusePipeInputsTouchWhileApplierOwnsIt, through isBarrieredFill, that this thread
            // was about to park behind it, and then ran on without parking. The apply thread was
            // measured inside a record at that instant, and the abort it produced named the
            // CLIENT's verb - a verb the applier cannot stamp, which is what identifies the
            // writer (report §2).
            //
            // So the family's one row is applied to the family. Every other unknown verb keeps
            // the conservative answer, which is now honest rather than hoped for: the fill site
            // establishes quiescence before it writes (MGPipeValidateForVerb below), so a
            // barriered fill no longer rests on a park that may never come.
            static_assert(MGPipeVerbForWireOp(MGPWireOp::DrawVbo) == MGPipeVerb::DrawArrays,
                          "the draw family's representative row moved; the fallback below names "
                          "draw_vbo because that is the record every kDraw verb emits");
            // P5e (ra2): THE CLIENT'S HALF OF ESCALATION (iii) WAS HERE AND WENT WITH IT
            // (ID-133, withdrawn by ID-136). It named MultiDrawArrays / MultiDrawElements /
            // MultiDrawElementsBaseVertex so the client would FILL for the records the server
            // was escalating - a fill the server then read as a barriered pull. Retiring that
            // pull outright (MultiDraw.cpp's BoundDrawIndirectBufferId takes a handle arm) left
            // nothing for the fill to serve, so a plain multi-draw is an ordinary kDraw verb
            // again and takes the draw_vbo answer below. The pair is the phase's own lesson
            // written twice: a wait added to make a lane green is paid by an arm, and here the
            // arm was the default tier the phone ships.
            if (op == MGPWireOp::kOpCount &&
                kMGPipeVerbClass[static_cast<SizeT>(verb)] == MGPipeVerbClass::kDraw) {
                op = MGPWireOp::DrawVbo;
            }
            if (op == MGPWireOp::kOpCount) return true;
            if (MGPipeWaitClassFor(op) != kWaitNone) return true;
            if (MGPipeCallClassFor(op) == kCtxVerb && ctx != nullptr &&
                ctx->IsTransformFeedbackActive()) {
                return true;
            }
            return false;
        }

        // ---- P5e (ra2), CONTRACT-P5E §3.5 AMENDED: A BARRIERED FILL MAKES ITSELF QUIESCENT ----
        //
        // §3.5 exempted the residual fill of a BARRIERED record from the single-writer rule on
        // the ground that "this thread is about to park behind it". That is a claim about the
        // FUTURE, and the write is in the PRESENT: the order at the validate point is fill,
        // then emit, then park, and between the fill and the park the apply thread is still
        // draining the UNBARRIERED records the client ran ahead of. The claim was therefore
        // never an argument about this instant, and the guard that took it - the
        // `if (isBarrieredFill) return;` arm of RefusePipeInputsTouchWhileApplierOwnsIt - could
        // not fire however wrong the fill was.
        //
        // This makes the claim TRUE instead of asserting it. §2.5's forced wait is exactly
        // "publish nothing, wait for the applier to reach LastPublishedSeq, drain the reverse
        // channel", which is the definition of the window the fill needs, and it already exists
        // for glFinish and for BackendObject_Remote's forwarders.
        //
        // IT IS CALLED AT EVERY GL-THREAD WRITE INTO THE BLOCK, not once per verb, because the
        // validate point writes the block in TWO phases that straddle record publication: the
        // serial bump / stamp withdrawal / verb rename run BEFORE the emitters, and the 63-field
        // walk of step 4 runs AFTER them - and the records the emitters published are records
        // whose apply READS the block. One wait cannot cover both halves.
        //
        // THE ARM IS STATED, NOT INFERRED (ID-81): WaitForApplyToCatchUp's own first test is
        // `m_runAheadArmed && m_started`, and m_runAheadArmed is the latch of
        // `Transport != Monolith && Ipc.RunAhead && kMGPipeP5eRunAheadReady && the caps bit`. So
        // on the monolith arm, on the pull build and under MOBILEGL_IPC_RUN_AHEAD=0 this is a
        // call that returns, and the lockstep client's behaviour is unchanged byte for byte
        // (G1) - under lockstep appliedSeq is already at LastPublishedSeq by construction.
        void QuiesceApplierBeforeFill(const char* surface) {
            MG_Remote::Client::ClientSession* session = MG_Remote::Client::ClientSession::Active();
            if (session == nullptr) return; // no session: this process has no applier to outrun
            session->WaitForApplyToCatchUp(surface);
        }

        // Whether the LAST fill actually happened, i.e. whether the rows in the block describe
        // the verb in flight. Read by MGPipeNoteFrontendMutation, which refreshes one field of
        // that fill: with no fill behind it there is nothing to refresh and the write would be
        // the role violation §3.5 names.
        Bool g_lastFillWasBarriered = true;
#endif

        template <class T>
        const SharedPtr<T>& NullShared() {
            static const SharedPtr<T> null;
            return null;
        }

        [[noreturn]] void BadKnob(const char* knob, const char* value, const char* why) {
            MGLOG_F("MGPipe: Fatal{PipeVerifyBadKnob, \"%s=%s\": %s}", knob, value, why);
            std::abort();
        }

        // ---- MOBILEGL_PIPE_POISON_OMIT (negative control B, P1 brief D6) ----
        // The filler skips the STAMP (never the value) of one (verb, field) pair: an omission
        // indistinguishable from a forgotten FillPoints.def row, so that verb's read of the
        // field is Fatal{UnmigratedPipeInput, "Field@Verb"} and no other verb is affected.
        struct PoisonOmission {
            Bool Armed = false;
            MGPipeVerb Verb = MGPipeVerb::kVerbCount;
            MGPipeInputField Field = MGPipeInputField::kFieldCount;
        };
        PoisonOmission g_omission;
        Bool g_omissionKnobParsed = false;
        String g_omissionKnobValue; // the value the last parse saw

        // Parsed on the first fill and again only when the value changes. A lane loads
        // Features once, before any fill, so that is one parse per process there; a forked
        // test child that sets Features after its parent already filled gets its own parse,
        // which is what puts the parser and its Fatal{PipeVerifyBadKnob} under a unit test.
        // An empty value never clears an omission a test armed through MGPipeSetPoisonOmission.
        void ParsePoisonOmissionKnob() {
            const String& knob = MG_Config::Features.PipePoisonOmit;
            // THE UNSET KNOB IS TWO LENGTH LOADS AND A BRANCH, inline, and that is the whole
            // change (P5d r3, package C): this runs at every verb - 852 draws a frame on the
            // profiled workload - and `knob == g_omissionKnobValue` is an out-of-line String
            // compare even when both sides are empty, which is every shipping configuration.
            // Sizes first, contents only when a non-empty value is involved; the re-parse
            // condition is unchanged (the value differing from what the last parse latched).
            if (g_omissionKnobParsed && knob.size() == g_omissionKnobValue.size() &&
                (knob.empty() || knob == g_omissionKnobValue)) {
                return;
            }
            g_omissionKnobParsed = true;
            g_omissionKnobValue = knob;
            if (knob.empty()) return;
            const auto colon = knob.find(':');
            if (colon == String::npos || colon == 0 || colon + 1 >= knob.size()) {
                BadKnob("MOBILEGL_PIPE_POISON_OMIT", knob.c_str(), "expected <Verb>:<FieldName>");
            }
            const String verbName = knob.substr(0, colon);
            const String fieldName = knob.substr(colon + 1);
            const auto verb = MGPipeFindVerb(verbName.c_str());
            if (!verb) BadKnob("MOBILEGL_PIPE_POISON_OMIT", knob.c_str(), "no such verb in kMGPipeVerbNames");
            const auto field = MGPipeFindInputField(fieldName.c_str());
            if (!field) BadKnob("MOBILEGL_PIPE_POISON_OMIT", knob.c_str(), "no such field in kMGPipeInputFieldNames");
            MGPipeSetPoisonOmission(verbName.c_str(), fieldName.c_str());
        }

        [[maybe_unused]] Bool IsOmitted(MGPipeVerb verb, MGPipeInputField field) {
            return g_omission.Armed && g_omission.Verb == verb && g_omission.Field == field;
        }

#if MOBILEGL_PIPE_VERIFY
        // ---- the MOBILEGL_PIPE_VERIFY comparator (P1 brief D8) ----
        // Two mechanisms, both active only when Features.PipeVerify is set: the ENTRY compare
        // once per verb (the pushed block against a second snapshot of the live context,
        // taken at the same instant - tautological until P2 gives the first arm a real
        // filler, and kept falsifiable by MOBILEGL_PIPE_VERIFY_CORRUPT), and the
        // COMPARE-AT-READ in every accessor (the stored value against a fresh read of the
        // live context at the moment the backend reads it - the arm that is real in P1: it
        // catches a value that changed between the verb boundary and the read).
        // LEAK-AT-EXIT STORAGE, for gPipeInputs' reason (MG_Backend/MGPipe/PipeInputs.h): a
        // PipeInputs holds SharedPtrs to frontend objects in its O class, and these two are
        // filled from the live context, so either can hold the LAST reference to a
        // VertexArrayObject or a ProgramObject. Destroying them from __run_exit_handlers
        // would run ~VertexArrayObject / ~BufferObject at exit, into a pipe and a backend
        // that are already being torn down. References so the ~50 uses below need no edit.
        PipeInputs& g_snapshot = *new PipeInputs();    // the second arm
        PipeInputs& g_readScratch = *new PipeInputs(); // where the compare-at-read re-read lands

        // The read hook arms at the first fill (ArmVerify below), so it cannot see a read
        // made before that. That window is covered by the poison instead: MGP_INPUT_CHECK
        // precedes MGP_INPUT_VERIFY_READ in every accessor and a stamp of 0 is never fresh,
        // so such a read is Fatal{UnmigratedPipeInput, "<Field>@<none>"} before the hook
        // could matter - which holds only while a verify build always carries the poison.
        static_assert(MOBILEGL_PIPE_POISON, "the compare-at-read hook relies on the poison for reads before the first fill");

        struct VerifyState {
            Bool Parsed = false;
            Bool Enabled = false;
            Bool Fatal = true;
            Bool InHook = false; // a re-read that re-enters an accessor is not re-verified
            Optional<MGPipeInputField> Corrupt;
            String CorruptKnob; // the MOBILEGL_PIPE_VERIFY_CORRUPT value the last arm saw
            std::atomic<Uint64> Divergences{0};
            ~VerifyState() {
                const Uint64 count = Divergences.load(std::memory_order_relaxed);
                if (count != 0) {
                    MGLOG_E("MGPipe: verify summary - %llu divergence(s) survived MOBILEGL_PIPE_VERIFY_FATAL=0",
                            static_cast<unsigned long long>(count));
                }
            }
        };
        VerifyState g_verify;

        // Armed on the first fill and re-armed when any of the three verify knobs'
        // Features value (PipeVerify, PipeVerifyFatal, PipeVerifyCorrupt) differs from what
        // the last arm latched (the same reason as ParsePoisonOmissionKnob: one arm per lane
        // process, a fresh arm for a forked test child that turns a knob after its parent
        // filled). Cost: two Bool compares and one String compare per fill, verify builds only.
        void ArmVerify() {
            const auto& features = MG_Config::Features;
            if (g_verify.Parsed && g_verify.Enabled == features.PipeVerify && g_verify.Fatal == features.PipeVerifyFatal &&
                g_verify.CorruptKnob == features.PipeVerifyCorrupt) {
                return;
            }
            g_verify.Parsed = true;
            g_verify.Enabled = features.PipeVerify;
            g_verify.Fatal = features.PipeVerifyFatal;
            g_verify.CorruptKnob = features.PipeVerifyCorrupt;
            g_verify.Corrupt = Optional<MGPipeInputField>{};
            if (!g_verify.Enabled) return;
            const String& corrupt = g_verify.CorruptKnob;
            if (!corrupt.empty()) {
                const auto field = MGPipeFindInputField(corrupt.c_str());
                if (!field) {
                    BadKnob("MOBILEGL_PIPE_VERIFY_CORRUPT", corrupt.c_str(), "no such field in kMGPipeInputFieldNames");
                }
                g_verify.Corrupt = field;
            }
            // The lanes grep for this line: a verify run whose log lacks it never armed.
            MGLOG_I("MGPipe: verify armed - %u fields, %u verbs, fatal=%d", static_cast<unsigned>(kMGPipeInputFieldCount),
                    static_cast<unsigned>(kMGPipeVerbCount), g_verify.Fatal ? 1 : 0);
            if (g_verify.Corrupt) {
                MGLOG_I("MGPipe: verify corruption armed - %s", kMGPipeInputFieldNames[static_cast<SizeT>(*g_verify.Corrupt)]);
            }
        }

        void ReportDivergence(MGPipeInputField field, const char* where) {
            const Uint64 serial = MGPipeFillAccess::Filled(MGPipeClientInputs()).CurrentVerbSerial;
            MGLOG_F("MGPipe: Fatal{PipeVerifyDiffer, \"%s@%s\", verb=%llu, where=%s}",
                    kMGPipeInputFieldNames[static_cast<SizeT>(field)], MGPipeVerbName(MGPipeClientInputs().CurrentVerb()),
                    static_cast<unsigned long long>(serial), where);
            if (g_verify.Fatal) std::abort();
            g_verify.Divergences.fetch_add(1, std::memory_order_relaxed);
        }

        void EntryCompare(PipeInputs& inputs, const MGPipeFieldMask& mask) {
            if (!g_verify.Enabled) return;
            SnapshotFromGLContext(g_snapshot, mask);
            // Negative control A: perturb the SNAPSHOT arm, so a green run goes red naming the
            // field. A field outside this verb's mask is not compared and stays untouched.
            if (g_verify.Corrupt && MGPipeFieldMaskHas(mask, *g_verify.Corrupt)) {
                MGPipeApplyVerifyCorruption(g_snapshot, *g_verify.Corrupt);
            }
            MGPipeInputField differing = MGPipeInputField::kFieldCount;
            if (!MGPipeVerifyInputs(inputs, g_snapshot, mask, &differing)) ReportDivergence(differing, "entry");
        }
#endif // MOBILEGL_PIPE_VERIFY
    } // namespace

#if MOBILEGL_PIPE_VERIFY
    void SnapshotFromGLContext(PipeInputs& snapshot, const MGPipeFieldMask& mask) {
        auto* ctx = LiveContext();
        MGPipeFillAccess::SetIdentity(snapshot, ctx);
        MGPipeFillAccess::SetVerb(snapshot, MGPipeClientInputs().CurrentVerb());
        if (ctx == nullptr) return;
        for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
            const auto field = static_cast<MGPipeInputField>(i);
            if (!MGPipeFieldMaskHas(mask, field) || kMGPipeInputFieldSticky[i]) continue;
            MGPipeFillAccess::CopyField(snapshot, *ctx, field);
        }
    }

    void MGPipeVerifyReadHook(const PipeInputs& self, MGPipeInputField field, Uint index0, Uint index1) {
        if (&self != &gPipeInputs || !g_verify.Enabled || g_verify.InHook) return;
        const auto index = static_cast<SizeT>(field);
        if (kMGPipeInputFieldSticky[index]) return;
        auto* ctx = LiveContext();
        if (ctx == nullptr) return;
        // The whole field is re-read and compared - a superset of "the same indices", so a
        // divergence in an index the backend did not ask for is still a divergence between
        // the boundary value and the live value. The indices only decorate the report. The
        // cost is per backend read (GetRenderStateParameters re-copies and compares the whole
        // struct; GetProgramForDraw re-joins the pending link), inside the verify budget and
        // to be kept in mind when reading the verify lane's wall time.
        // InHook: the re-read calls the same GLContext accessor the filler calls, and
        // GetProgramForDraw's join can re-enter a backend and with it another gPipeInputs
        // accessor; that inner read is a plain load rather than a second hook, so the hook
        // never recurses (and never reports the inner read against a half-copied scratch).
        g_verify.InHook = true;
        MGPipeFillAccess::CopyField(g_readScratch, *ctx, field);
        const Bool equal = MGPipeInputsFieldEqual(field, self, g_readScratch);
        g_verify.InHook = false;
        if (equal) return;
        MGLOG_E("MGPipe: verify read of %s (index %u, %u) differs from the live context", kMGPipeInputFieldNames[index],
                index0, index1);
        ReportDivergence(field, "read");
    }
#endif // MOBILEGL_PIPE_VERIFY

    // ---- push on mutation (P1 lane finding F2) ----
    // A backend that writes a frontend object inside its own verb moves a value the verb
    // boundary already copied: Magma's ResolveSamplerDescriptor synthesises a fallback
    // texture for an unbound sampler and its AllocateStorage/SetInternalFormat bump the
    // context's sampling-resolution generation, so every read of that field after the
    // fallback differs from the live context (the two SampledSetStaleness / six
    // UnboundImageDescriptor entries the verify lane aborted on). The frontend mutator
    // spells MGP_NOTE_MUTATION(Field) at the point of the move and lands here.
    //
    // Only the value is refreshed. The stamp is deliberately left alone: a field whose stamp
    // this verb withheld (negative control B) must stay stale, and a field the verb never
    // filled must stay Fatal{UnmigratedPipeInput} on the next read rather than be healed by
    // an unrelated frontend write.
    void MGPipeNoteFrontendMutation(MGPipeInputField field) {
        PipeInputs& inputs = MGPipeClientInputs();
#if MOBILEGL_BUILD_DISAGGREGATED
        // P5e (ra, CONTRACT-P5E §3.1): THIS REFRESHES ONE FIELD OF THE LAST FILL, so with no
        // fill behind it there is nothing to refresh. Under run-ahead the last verb may have
        // been unbarriered, in which case the block describes an older verb the server is no
        // longer being asked about and a write here would be a GL-thread touch of server-role
        // memory. It returns instead - the same answer, one line earlier, as the class-mask
        // test below gives for a field the verb never pushed.
        if (!g_lastFillWasBarriered) return;
        // P5c (gt, layer 2): the single-field refresh is a client write into gPipeInputs too -
        // same gate as the fill, and it makes the same claim the fill made: the rows it is
        // touching belong to a record this thread is parked behind (or will park behind).
        //
        // P5e (ra2): SO IT TAKES THE SAME WAIT. This one runs at a frontend mutation point, not
        // at a verb, so "will park behind" is even weaker here than it is at the fill - the
        // mutation can land anywhere between two verbs, with the whole run-ahead backlog in
        // flight. The wait is a no-op unless run-ahead is armed, and at a mutation point after a
        // barriered verb the applier is usually already caught up, so this is a watermark test
        // rather than a park in the common case.
        QuiesceApplierBeforeFill("MGPipeNoteFrontendMutation");
        MG_Remote::Client::ClientSession::RefusePipeInputsTouchWhileApplierOwnsIt(
            "MGPipeNoteFrontendMutation", /*isBarrieredFill=*/true);
#endif
        auto* ctx = LiveContext();
        if (ctx == nullptr) return;
        const auto verb = inputs.CurrentVerb();
        if (verb == MGPipeVerb::kVerbCount) return; // nothing has filled the block yet
        const auto index = static_cast<SizeT>(field);
        if (kMGPipeInputFieldSticky[index]) return; // forwarded: no storage to refresh
        const MGPipeFieldMask& mask =
            kMGPipeClassFieldMask[static_cast<SizeT>(kMGPipeVerbClass[static_cast<SizeT>(verb)])];
        if (!MGPipeFieldMaskHas(mask, field)) return; // this verb never pushed it
        MGPipeFillAccess::CopyField(inputs, *ctx, field);
    }

    // ---- the aggregate generations (P2 brief D4) ----
    // MGP_NOTE_AGGREGATE lands here. The bump points are on OBJECTS, which have no
    // back-pointer to the state container that owns them, so the note finds the live
    // context - the same shape, and for the same reason, as MGPipeNoteFrontendMutation
    // above. No verb has to be in flight and no field is stamped: an aggregate generation
    // is not a PipeInputs field, it is what the tracker's shutter compares against.
    void MGPipeNoteAggregate(MGPipeAggregate aggregate) {
        auto* ctx = LiveContext();
        if (ctx == nullptr) return;
        switch (aggregate) {
        case MGPipeAggregate::VaoAttribute:
            ctx->NoteVaoAttributeChanged();
            break;
        case MGPipeAggregate::FramebufferAttachment:
            ctx->NoteFramebufferAttachmentChanged();
            break;
        case MGPipeAggregate::TextureContent:
            ctx->NoteTextureContentChanged();
            break;
        case MGPipeAggregate::TextureParams:
            ctx->NoteTextureParamsChanged();
            break;
        case MGPipeAggregate::BufferChange:
            ctx->NoteBufferChanged();
            break;
        case MGPipeAggregate::VertexAttribDefault:
            ctx->NoteVertexAttribDefaultChanged();
            break;
        case MGPipeAggregate::Count:
            break;
        }
    }

    // ================================================================================
    // P3a: the resource family's emission (brief D-A, D-B, D-C, D-D)
    // ================================================================================
    //
    // Declared in MG_Pipe/PipeMutation.h and defined here for the layering reason that
    // header states: the emission sites are BufferObject's dispatchers, which are MG_State's,
    // and MG_State may see a declaration but never MG_Impl/Pipe/ResourceTracker.h.
    //
    // Every one of these is called from a site that has ALREADY asked
    // MGPipeResourceSubsystemEnabled(), except the mint and the destroy - the handle is
    // client state and its lifetime is the frontend object's, not the subsystem's.
    namespace {
        using MG_State::GLState::BufferObject;

        MGPHandleOnly BufferHandleOnly(MGPipeHandle handle) {
            MGPHandleOnly only{};
            only.Handle = handle;
            only.Kind = static_cast<Uint32>(MGPipeKind::Buffer);
            return only;
        }

        // THE CONTENT PATHS LOOK THE HANDLE UP, THEY DO NOT MINT IT. Acquire mutates the
        // process-global slot allocator (a map insert on a miss, a free-list pop) and then
        // resizes the tracker's inverse vector; D-A2 preserves the off-thread/stale-queue arm
        // of Ops_SubData, so BufferObject::NotifySubData is reachable off the render thread,
        // and two threads inside Acquire - or one there while ~BufferObject is in Free - is a
        // torn free list and a dangling span. The mint happens ONCE, on the GL thread, in the
        // BufferObject constructor (MGPipeMintResourceHandle), so on every content path the
        // handle already exists and a pure lookup is not merely safe but strictly correct.
        //
        // A null answer therefore means a buffer whose constructor did not mint - which
        // cannot happen in a push build - or a lifetime id already freed. Either way the call
        // is dropped, so it is said out loud rather than passing kMGPipeNullHandle to the
        // applier, which would count it as a refusal with no way back to the cause.
        MGPipeHandle ContentHandleFor(const BufferObject& buffer, const char* call) {
            const MGPipeHandle handle = MGPipeResourceTrackerInstance().Find(buffer);
            if (MGPipeHandleIsNull(handle)) {
                MGLOG_E_ONCE("MGPipe: %s on buffer %u has no resource handle - the call is dropped; a "
                             "push build mints one in the BufferObject constructor, so this is a lifetime "
                             "id that was already freed",
                             call, buffer.GetExternalIndex());
            }
            return handle;
        }
    } // namespace

    // R-8 (c1). THE SECOND CONJUNCT MOVES UNDER SPLIT, AND ONLY UNDER SPLIT.
    //
    // `MGPipeGetResourceOps() != nullptr` asks "has a backend registered the consumer". That
    // table is the SERVER's registration and it is a PROCESS-WIDE global (PipeApply.cpp:402):
    // under inproc a client reading it answers correctly BY ACCIDENT, and under spawn the
    // client process has no backend at all, so the read answers null and five record families
    // stop emitting - silently, while the emitters go on clearing their per-level dirty flags
    // on the acceptance they never asked for. That is ID-39's 66 lost DirectVulkan uploads with
    // a wire in between. The client asks the caps mirror instead, which carries the answer the
    // SERVER gave at the handshake (CallMask bits 32..47).
    //
    // s1 made the server end Fatal when nobody sets the mask; this is the client end.
    Bool MGPipeResourceSubsystemEnabled() {
        if ((MG_Config::Features.PipePush & kMGPipeSubsystemResources) == 0) return false;
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
            return MG_Remote::Client::CapsMirrorInstance().ServerConsumes(kMGPipeSubsystemResources);
        }
#endif
        return MGPipeGetResourceOps() != nullptr;
    }

    Bool MGPipeResourceOpsHaveSubDataResident() {
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
            // CONTRACT-P5.md §7's THIRD NAMED CAPABILITY PROBE. `ops->SubDataResident != nullptr`
            // is not a safety check - it decides whether the resident-upload path EXISTS - and
            // under split there is no op table here to probe. kCapResidentSubData is the bit the
            // server publishes for exactly this question.
            return MG_Remote::Client::CapsMirrorInstance().HasCap(kCapResidentSubData);
        }
#endif
        const MGPipeResourceOps* ops = MGPipeGetResourceOps();
        return ops != nullptr && ops->SubDataResident != nullptr;
    }

    void MGPipeMintResourceHandle(BufferObject& buffer) {
        // UNCONDITIONAL in a push build, deliberately: set_vertex_buffers names a buffer by
        // handle whether or not the resource family is switched on, so gating the mint on
        // the resource subsystem bit would make the vertex-input subsystem emit null handles
        // in exactly the A/B arm that exists to isolate the two. It costs one free-list pop
        // and one map insert per buffer object and emits nothing.
        //
        // The client resource callbacks are MONOLITH-ONLY (CONTRACT-P5C §4.1): with an
        // active transport the server session installs its producer callbacks at Accept and
        // the client's consumers are invoked by name from DrainEventRing, so installing
        // them here would be the double installation the check now aborts on.
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MG_Config::Transport == MG_Config::TransportMode::Monolith) {
            MGPipeInstallClientResourceCallbacks();
        }
#else
        MGPipeInstallClientResourceCallbacks();
#endif
        MGPipeResourceTrackerInstance().Acquire(buffer);
    }

    void MGPipeEmitResourceCreate(BufferObject& buffer) {
        MGPipeResourceTracker& tracker = MGPipeResourceTrackerInstance();
        const MGPipeHandle handle = tracker.Acquire(buffer);
        Uint16 bindMask = tracker.BindMask(handle);
        if (auto* ctx = LiveContext()) bindMask = tracker.RefreshBindMask(*ctx, buffer, handle);
        // storageDefined = false: the constructor has no store yet, storage is defined lazily
        // by the first respecify, and the backend's ensure path already tolerates a resource
        // that has none.
        const MGPResourceDesc desc = MGPipeBuildResourceDesc(buffer, handle, bindMask, false);
        tracker.NoteDesc(desc, true);
        // LATCHED, so the destroy is gated on whether this create actually went out rather
        // than on whether a table is still registered when the object dies (D-L, m12).
        tracker.NotePublished(handle);
        MGPipeRouteResourceCreate(desc);
    }

    void MGPipeEmitResourceRespecify(BufferObject& buffer) {
        MGPipeResourceTracker& tracker = MGPipeResourceTrackerInstance();
        const MGPipeHandle handle = tracker.Acquire(buffer);
        Uint16 bindMask = tracker.BindMask(handle);
        if (auto* ctx = LiveContext()) bindMask = tracker.RefreshBindMask(*ctx, buffer, handle);
        // M-1: THE CREATE FIRST, IF THIS HANDLE NEVER PUBLISHED ONE - which makes the
        // create/destroy latch self-healing in both directions instead of only one.
        //
        // The constructor's create is gated on MGPipeResourceSubsystemEnabled(), which is bit 7
        // AND "a backend registered MGPipeResourceOps"; the CONSUMER's gate is bit 7 alone. The
        // two disagree across a register/unregister boundary, and there is a real window:
        // UnregisterBufferBackendOps nulls the table from OnBackendContextDestroyed
        // (DestroyEGLContext) and the re-register happens at the next MakeCurrent, while D-A2
        // deliberately keeps NotifySubData reachable off the render thread. A buffer born in
        // that window latched Published = false, so the applier had no record for it and every
        // later respecify was REFUSED - after which EnsureBufferResourceForHandle read
        // ResourceRecordOf == nullptr, took size 0, returned a twin with no store and drew
        // through id 0, with no diagnostic anywhere. The legacy arm recovers from the same
        // window by twinning lazily off the frontend object and full-uploading from the shadow;
        // this is the handle arm's equivalent, and it costs one bool compare per respecify.
        //
        // A create rather than a respecify because that is what the record's absence means: the
        // applier starts the record over on a create (it does not edit one), so this cannot
        // resurrect a field from a recycled slot, and the respecify below then defines the
        // storage exactly as it would have.
        if (!tracker.WasPublished(handle)) {
            const MGPResourceDesc createDesc =
                MGPipeBuildResourceDesc(buffer, handle, bindMask, /*storageDefined=*/false);
            tracker.NoteDesc(createDesc, true);
            tracker.NotePublished(handle);
            MGPipeRouteResourceCreate(createDesc);
        }
        const MGPResourceDesc desc = MGPipeBuildResourceDesc(buffer, handle, bindMask, true);
        tracker.NoteDesc(desc, false);
        // initialBytes is the client's own shadow base - zero copy, and null is a real answer
        // for the orphaning idiom (a NULL-data respecify leaves the store undefined and the
        // backend must not upload the stale bytes).
        //
        // THE SIZE TEST IS NOT REDUNDANT. A zero-byte store is DEFINED content - glBufferData's
        // `size == 0` arm sets HasDefinedContent (BufferObject.cpp:242) because the store exists
        // and is empty - and MappedData() answers a NON-NULL pointer for it, since the shadow
        // reserves one byte whatever the size (PipeResource.h:140-143). Reading MappedData()
        // alone therefore answered "bytes to carry" for a store that has none, which sent the
        // split branch below down the respecify(nullptr)-plus-follow-up shape; the follow-up
        // walk emits nothing for a zero-length range (ResourceTracker.h:253), so the
        // InitialBytesNotCarried self-check aborted by name on the first
        // glBufferData(target, 0, NULL, usage) of the bsl-esc-menu trace. `GetSize() > 0` is
        // what makes the answer mean "there are bytes here" rather than "the store is defined".
        const void* initialBytes =
            (desc.HasDefinedContent != 0 && buffer.GetSize() > 0) ? buffer.MappedData() : nullptr;
        // kNeedsAck rides on the CALL and MGPipeResourceRespecifyNeedsAck(desc) decides per
        // record: only an immutable store (a glBufferStorage*) is a real synchronous
        // allocation and only it is allowed one. In monolith the acknowledgement is
        // ((void)0), because the applier is one function call away and has already run by
        // the time this returns; the transport wires the doorbell to that same predicate.
#if MOBILEGL_BUILD_DISAGGREGATED
        // R-13.3's MISSING PRODUCER, and it is the reason the first joint inproc run died.
        // CONTRACT-P5 §2 row 19 rules that `initialBytes` is ALWAYS nullptr under split and
        // that "initial content arrives as ResourceSubData records immediately after this
        // one" - but nothing emitted those records, so the split arm's refusal
        // (Fatal{UncarriedInitialBytes}) fired on the first glBufferData with data, which is
        // the first thing every scenario does.
        //
        // IT IS THE CONTRACT'S OWN PRESCRIBED ROUTE, not a new one: "the chosen route reuses a
        // path that is already chunked (MGPipeForEachSubDataRecordRange) and already
        // acceptance-gated; it costs one extra record". So the respecify defines the storage
        // and the walk below ships the bytes, through the same emitter every later
        // glBufferSubData uses - which also means the HasLiveHostWrites bit and the
        // acceptance latch are computed in exactly one place instead of two.
        //
        // SPLIT-ONLY, and that is load-bearing for G2: under monolith the applier reads
        // `initialBytes` directly and a second upload would be a real behaviour change in the
        // arm the split arm is measured against.
        //
        // ROLE-AWARE (M5). Under inproc the apply thread reaches this very emitter when the
        // server's own backend respecifies a buffer (c1-v2 §4 R-17.3: that is where
        // Fatal{BarrierTimeout, "ResourceRespecify"} from mgl-srv-apply came from). On that
        // thread this branch would emit CLIENT wire records - which the server role does not
        // produce, so ClientWireRecordsEmitted() would not move and the self-check below would
        // abort the SERVER by name; it would also run the respecify(nullptr)+follow-up shape,
        // giving the server role a path monolith does not have. So the server role takes the
        // ELSE below, exactly as monolith does. RunsAsTheServerRole() is v1's
        // ServerLoop::OnApplyThread(), false on the GL thread that owns this fill.
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith &&
            !MG_Remote::Client::RunsAsTheServerRole() && initialBytes != nullptr) {
            MGPipeRouteResourceRespecify(desc, nullptr);
            // COUNTED, NOT ASSUMED, and this is the only thing that can gate the follow-up at
            // all. Dropping the walk below leaves a respecify that went out with nullptr and
            // bytes that nothing carried - and no P5 scenario's PICTURE changes, because every
            // one of them re-uploads its vertices through the ordinary dirty path afterwards.
            // So the statement "the content followed" is made HERE, against the client's own
            // record ordinal, rather than left to a lane that cannot see it. Remove the call
            // below and this aborts by name on the first glBufferData that carries data.
            const Uint64 before = MG_Remote::Client::ClientWireRecordsEmitted();
            MGPipeEmitResourceSubData(buffer, 0, static_cast<SizeT>(buffer.GetSize()));
            if (MG_Remote::Client::ClientWireRecordsEmitted() == before) {
                MGLOG_F("MGPipe: Fatal{InitialBytesNotCarried, \"resource_respecify\"} - the "
                        "respecify crossed with initialBytes = nullptr (R-13.3) and the "
                        "resource_subdata records that were supposed to follow it emitted "
                        "NOTHING, so %llu bytes of initial content exist on no side of the wire",
                        static_cast<unsigned long long>(buffer.GetSize()));
                std::abort();
            }
            return;
        }
#endif
        MGPipeRouteResourceRespecify(desc, initialBytes);
    }

    // THE CAP THE TWO CONTENT WALKS BELOW CUT A RANGE AT. The record's own bound (2^32-1) is not
    // the one a real upload meets first: one piece's bytes are staged WHOLE in SEG_STAGE, a
    // linear arena, and a blob larger than that arena is Fatal{RingOverrun, "SEG_STAGE"} at the
    // encoder rather than a split (PipeWireCodec.cpp:856-864). Measured on the CI traces: a
    // 128 MiB arena's whole-buffer follow-up against a 32 MiB segment aborted there, which is
    // the RingOverrun this walk exists to prevent.
    //
    // 0 from the helper means "nothing to fit" - monolith, the server role's own uploads (which
    // run the monolith adapter), or a process with no session - and the record's own bound is
    // then the answer, exactly as it was before the cap existed.
#if MOBILEGL_BUILD_DISAGGREGATED
    Uint64 MGPipeContentChunkCap() {
        const SizeT stageChunk = MG_Remote::Client::MGPipeStageChunkBytes();
        return stageChunk == 0 ? kMGPipeSubDataMaxRecordSize : static_cast<Uint64>(stageChunk);
    }
#else
    constexpr Uint64 MGPipeContentChunkCap() { return kMGPipeSubDataMaxRecordSize; }
#endif

    void MGPipeEmitResourceSubData(BufferObject& buffer, SizeT offset, SizeT size) {
        const MGPipeHandle handle = ContentHandleFor(buffer, "resource_subdata");
        if (MGPipeHandleIsNull(handle)) return;
        const Uint8* base = buffer.MappedData();
        const Bool encodable =
            MGPipeForEachSubDataRecordRange(offset, size, [&](Uint64 at, Uint64 length) {
                MGPSubData record{};
                // The pre-pass inside the walk proved every piece encodable before the first
                // one was emitted, so this cannot be false - but a zeroed record (null
                // handle, size 0) is not the answer if that pre-pass is ever relaxed.
                if (!MGPipeBuildSubDataRecord(handle, at, length, record, /*verbatimShadow=*/true)) return;
#if MOBILEGL_BUILD_DISAGGREGATED
                // P5 (b1): the live-host-writes bit rides the content record, because
                // "someone may be writing these bytes without telling you" is a fact about the
                // CONTENT and not about the storage. It is set from the object's PUBLISHED
                // value rather than from a live IsMapped() read so that the record and the
                // edge that announced it can never disagree. MGPipeBuildSubDataRecord does not
                // take the object, which is why it is set here and not in the builder.
                record.HasLiveHostWrites = buffer.HasLiveHostWritesForWire() ? 1 : 0;
#endif
                // `length` is this chunk's byte count, which the record also declares
                // (MGPipeBuildSubDataRecord writes it into the destination range) - passed
                // rather than re-read so the staged run and the record's own claim come from
                // one number.
                MGPipeRouteResourceSubData(record, base + at, length);
            }, MGPipeContentChunkCap());
        if (!encodable) {
            MGLOG_E_ONCE("MGPipe: resource_subdata range [%llu, +%llu) on buffer %u cannot be encoded - "
                         "one record's destination box caps the offset at 2^31-1",
                         static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size),
                         buffer.GetExternalIndex());
        }
    }

    void MGPipeEmitBufferSubDataResident(BufferObject& buffer, SizeT offset, const void* bytes, SizeT size) {
        const MGPipeHandle handle = ContentHandleFor(buffer, "buffer_subdata_resident");
        if (MGPipeHandleIsNull(handle)) return;
        const auto* base = static_cast<const Uint8*>(bytes);
        const Bool encodable =
            MGPipeForEachSubDataRecordRange(offset, size, [&](Uint64 at, Uint64 length) {
                MGPSubData record{};
                // NOT a verbatim level shadow: these bytes are the application's staging
                // store, or the pattern FillSubData expanded locally, and neither is this
                // client's untransformed shadow of the level.
                if (!MGPipeBuildSubDataRecord(handle, at, length, record, /*verbatimShadow=*/false)) return;
#if MOBILEGL_BUILD_DISAGGREGATED
                // P5 (b1): THE SECOND CONTENT EMITTER, and it has to speak for the same reason
                // the first does. ApplyBufferWrite ASSIGNS the bit - a content record emitted
                // while nothing maps the buffer is how the state goes back to false - so a
                // resident sub-data that stayed silent would write false over a live write
                // map. `glBufferSubData` against a persistently mapped arena is legal and is
                // the ordinary Flywheel/Create shape, so that is not a corner.
                record.HasLiveHostWrites = buffer.HasLiveHostWritesForWire() ? 1 : 0;
#endif
                // The application's STAGING store, valid for the duration of the call only.
                MGPipeRouteBufferSubDataResident(record, base + (at - offset), length);
            }, MGPipeContentChunkCap());
        if (!encodable) {
            MGLOG_E_ONCE("MGPipe: buffer_subdata_resident range [%llu, +%llu) on buffer %u cannot be encoded",
                         static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size),
                         buffer.GetExternalIndex());
        }
    }

    void MGPipeEmitResourceFlushRange(BufferObject& buffer, SizeT offset, SizeT size, Uint32 accessFlags) {
        const MGPipeHandle handle = ContentHandleFor(buffer, "resource_flush_range");
        if (MGPipeHandleIsNull(handle)) return;
        MGPFlushRange record{};
        record.Res = handle;
        record.Offset = offset;
        record.Size = size;
        // The application's REAL flags, not a normalised subset: the backend's kill-switch
        // arm reads INVALIDATE_RANGE / INVALIDATE_BUFFER / UNSYNCHRONIZED per call to choose
        // between a map+memcpy+unmap and an upload, so merging them here would change which.
        record.AccessFlags = accessFlags;
#if MOBILEGL_BUILD_DISAGGREGATED
        // R-13.2's MISSING PRODUCER, the twin of the respecify one above, and the cause of the
        // seven PersistentCoherentMapScenario aborts on the first joint inproc run:
        //     Fatal{ProtocolCorruption} resource_flush_range {slot=1, gen=0, glName=1}:
        //     a non-empty flush carries no bytes (offset=0, size=120, storage=120 bytes)
        //
        // CONTRACT-P5 §2 row 20 rules that this record carries NO bytes under split - it is a
        // {range, AccessFlags} control record, and a blobref here "would be a second,
        // forgeable way to say the same thing" - and that "the bytes of [Offset, Offset+Size)
        // arrive AHEAD of it as ResourceSubData records covering exactly that range". Nothing
        // emitted those records, so v1's StagedShadowStore had nothing staged for the range
        // the flush names, which is precisely the refusal ID-37 asked it to make rather than
        // silently reading the bytes again.
        //
        // EXACTLY THAT RANGE, not the whole buffer: the flush's own [offset, size) is what
        // the ladder rewrites, and staging more would be the coverage WIDENING ID-37 forbids.
        // Split-only, for the respecify's G2 reason - and ROLE-AWARE for M5's reason, the twin of
        // the respecify branch above: the apply thread flushing the server's own buffer emits no
        // client wire records, so it takes the plain route below rather than this split follow-up.
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith &&
            !MG_Remote::Client::RunsAsTheServerRole() && size != 0) {
            MGPipeEmitResourceSubData(buffer, offset, size);
        }
#endif
        MGPipeRouteResourceFlushRange(record, buffer.MappedData() + offset);
    }

    void MGPipeEmitResourceReadback(BufferObject& buffer) {
        // Whole-buffer by contract (BufferObject.h: the op pulls the backend's current
        // contents for the WHOLE buffer into the shadow). The split arm's slicing lives in
        // the CALLER (BufferObject::SyncGpuWrites): what "whole" costs is decided by the
        // event ring's capacity, which this layer does not read.
        MGPipeEmitResourceReadbackRange(buffer, 0, buffer.GetSize());
    }

    void MGPipeEmitResourceReadbackRange(BufferObject& buffer, SizeT offset, SizeT size) {
        const MGPipeHandle handle = ContentHandleFor(buffer, "resource_readback");
        if (MGPipeHandleIsNull(handle)) return;
        MGPReadback record{};
        record.Res = handle;
        record.Offset = offset;
        record.Size = size;
        // The answer travels back through MGPipeClientOnBufferWriteback, and the server's
        // epoch bump happens AFTER that writeback, never before.
        MGPipeRouteResourceReadback(record);
    }

    // NO UnmapPersistent PRODUCER IN P3a, AND THAT IS DELIBERATE. The catalogue has the call
    // and wire implemented it, but D-J forbids new behaviour and there is nothing to convert:
    // BufferBackendOps has seven hooks and none of them is an unmap, and
    // PipeResource::ReleasePersistentMap() (BufferObject.cpp, from RedefineStorage) tells the
    // backend nothing today - it learns from the Respecify that follows. Emitting
    // unmap_persistent here would therefore be a new call to a backend that has never been
    // told about a release, so the client emits none and the applier's refusal counter stays
    // at 0 for it. The producer lands with the phase that gives the backend an unmap hook.
    void* MGPipeEmitMapPersistent(BufferObject& buffer) {
        const MGPipeHandle handle = ContentHandleFor(buffer, "map_persistent");
        if (MGPipeHandleIsNull(handle)) return nullptr;
        MGPipeResourceTrackerInstance().NoteMapPersistent();
        // THE map-persistent-roundtrips SITE, and it counts every EMISSION - mint OR
        // DECLINE - because every one of them needs an answer from the resource owner. A
        // counter defined as "round trips actually taken" is 0 by construction in monolith
        // and could never go red for the reason it exists; this one is the same number in
        // both modes and is "one per storage definition" exactly as the design requires.
        if (MG_Util::PipeStats::Enabled()) {
            MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::MapPersistentRoundtrips, 1);
        }
        return MGPipeRouteMapPersistent(BufferHandleOnly(handle), buffer.GetSize(), buffer.MappedData());
    }

    Bool MGPipeEmitResourceDestroyAndFree(BufferObject& buffer) {
        MGPipeResourceTracker& tracker = MGPipeResourceTrackerInstance();
        const MGPipeHandle handle = tracker.Find(buffer);
        if (MGPipeHandleIsNull(handle)) return false;
        // THE LATCHED ANSWER, not the live one (m12): create and destroy are gated at two
        // different moments, and a buffer constructed while a backend's table was registered
        // and destroyed after UnregisterBufferBackendOps() would otherwise free its slot with
        // the applier's record still Live and the backend's twin still attached to it - on a
        // slot the allocator is about to hand out again.
        const Bool published = tracker.WasPublished(handle);
        if (published) {
            tracker.NoteDestroy();
            MGPipeRouteResourceDestroy(BufferHandleOnly(handle));
        }
        // THE ORDER IS FIXED (D-L): the applier clears the record and the backend drops its
        // twin while the handle still resolves, and only then does the slot go back. Free
        // erases the lifetimeId -> slot mapping, so a notice resolved twice finds nothing the
        // second time - and the Gen bump happens on the NEXT handout of the slot, not here,
        // so a double free cannot skip a generation.
        tracker.Retire(handle);
        MGPipeSlots().Free(MGPipeKind::Buffer, handle);
        return published;
    }

#if MOBILEGL_BUILD_DISAGGREGATED
    namespace {
        struct MGPipeDeferredDestroy {
            MGPipeKind kind;
            Uint64 lifetimeId;
        };
        // A plain mutex, not a lock-free queue: producers are destructors that lost the
        // last-reference race (rare), the consumer is the GL thread's verb hook, and neither
        // holds the lock past a vector push/swap. The count is the fast no-work check for
        // the per-verb drain.
        std::mutex g_deferredDestroyMutex;
        Vector<MGPipeDeferredDestroy> g_deferredDestroys;
        std::atomic<Uint32> g_deferredDestroyCount{0};
    } // namespace

    Bool MGPipeDeferDestroyAndFreeIfOnApplyThread(MGPipeKind kind, Uint64 lifetimeId) {
        if (MG_Config::Transport == MG_Config::TransportMode::Monolith) return false;
        if (!MG_Remote::Client::RunsAsTheServerRole()) return false;
        // P5e (ra), CONTRACT-P5E §2.7 / ruling 13. The queue stays - a BARRIERED record's apply
        // may still pin a frontend object (its fill's O-class rows, XFB's targets), so the
        // apply thread can still be a last owner and this is still the belt that keeps that
        // death off the client's allocator. But an enqueue from an UNBARRIERED record is a
        // FINDING, not a service: rule F says such an apply names no client memory at all, so
        // a SharedPtr it could be the last owner of means some site is still pinning a
        // frontend object across a record and the migration this phase believes it finished is
        // not finished. Named once with the kind so the site is findable, and Fatal under
        // strict so the lane owns the red rather than a log nobody reads.
        if (!MG_Pipe::MGPipeApplierCurrentRecordIsBarriered()) {
            if (MG_Config::Ipc.StrictErrors) {
                MGLOG_F("MGPipe: Fatal{RoleViolation, \"deferred-destroy\"} - an UNBARRIERED "
                        "apply was the last owner of a frontend object of kind %u (lifetime "
                        "%llu). CONTRACT-P5E rule F says an unbarriered apply reads no client "
                        "memory, so nothing it touched should have been a SharedPtr at all",
                        static_cast<unsigned>(kind), static_cast<unsigned long long>(lifetimeId));
                std::abort();
            }
            MGLOG_E_ONCE("MGPipe: an UNBARRIERED apply deferred the destruction of a frontend "
                         "object of kind %u - CONTRACT-P5E §2.7's finding: some apply-thread "
                         "site still holds a frontend SharedPtr across a record",
                         static_cast<unsigned>(kind));
        }
        {
            std::lock_guard<std::mutex> lock(g_deferredDestroyMutex);
            g_deferredDestroys.push_back(MGPipeDeferredDestroy{kind, lifetimeId});
        }
        g_deferredDestroyCount.fetch_add(1, std::memory_order_release);
        return true;
    }

    // P5e (ra), CONTRACT-P5E §2.3. Declared in PipeMutation.h, where its WHY is argued.
    //
    // NO GUARD ON THE CALLER'S BEHALF: ClientSession calls this only after a barriered apply
    // has returned on a run-ahead session, and adding a second "is run-ahead armed" test here
    // would be a copy of a decision that belongs on the other side. What this side owns is
    // WHICH rows go, and that answer is ReleaseObjectPins' four.
    //
    // THE STAMPS ARE LEFT ALONE, deliberately. A released row reads back as null, and a
    // BARRIERED verb's fill re-copies it before that verb's apply can pull it (§3.1 skips the
    // fill only for UNBARRIERED records); an unbarriered apply may not read it at all, and
    // §3.3's detector is what says so by name. Clearing the stamps as well would trade that
    // named abort for the poison Fatal, which names the field but not the rule it broke.
    void MGPipeReleaseResidualFillPins() { MGPipeFillAccess::ReleaseObjectPins(MGPipeClientInputs()); }

    // P5e (ra), CONTRACT-P5E §2.5. Declared in PipeMutation.h; see there for why it is not a
    // ClientSession call at the GL entry point.
    void MGPipeClientFlush() {
        if (MG_Config::Transport == MG_Config::TransportMode::Monolith) return;
        if (MG_Remote::Client::RunsAsTheServerRole()) return;
        if (MG_Remote::Client::ClientSession* session = MG_Remote::Client::ClientSession::Active()) {
            session->Flush();
        }
    }

    void MGPipeClientFinish() {
        if (MG_Config::Transport == MG_Config::TransportMode::Monolith) return;
        if (MG_Remote::Client::RunsAsTheServerRole()) return;
        if (MG_Remote::Client::ClientSession* session = MG_Remote::Client::ClientSession::Active()) {
            session->Finish();
        }
    }

    void MGPipeDrainDeferredDestroys() {
        if (g_deferredDestroyCount.load(std::memory_order_acquire) == 0) return;
        Vector<MGPipeDeferredDestroy> drained;
        {
            std::lock_guard<std::mutex> lock(g_deferredDestroyMutex);
            drained.swap(g_deferredDestroys);
            g_deferredDestroyCount.store(0, std::memory_order_release);
        }
        for (const MGPipeDeferredDestroy& one : drained) {
            // Each replay lands back in the helper itself, on the GL thread this time, so
            // the deferral branch passes and the helper's own order runs unchanged.
            switch (one.kind) {
            case MGPipeKind::VertexElementsCso:
                MGPipeEmitVertexElementsDestroyAndFree(one.lifetimeId);
                break;
            case MGPipeKind::SamplerViewCso:
                MGPipeEmitSamplerViewCsoDestroyAndFree(one.lifetimeId);
                break;
            case MGPipeKind::Texture:
                MGPipeEmitTextureDestroyAndFree(one.lifetimeId);
                break;
            case MGPipeKind::Renderbuffer:
                MGPipeEmitRenderbufferDestroyAndFree(one.lifetimeId);
                break;
            case MGPipeKind::Framebuffer:
                MGPipeEmitFramebufferDestroyAndFree(one.lifetimeId);
                break;
            case MGPipeKind::SamplerCso:
                MGPipeEmitSamplerCsoDestroyAndFree(one.lifetimeId);
                break;
            case MGPipeKind::ShaderCso:
                MGPipeEmitShaderCsoDestroyAndFree(one.lifetimeId);
                break;
            default:
                break;
            }
        }
    }
#endif

    Bool MGPipeEmitVertexElementsDestroyAndFree(Uint64 lifetimeId) {
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MGPipeDeferDestroyAndFreeIfOnApplyThread(MGPipeKind::VertexElementsCso,
                                                     lifetimeId)) {
            return false;
        }
#endif
        // C-1. THE SAME SHAPE AS MGPipeEmitResourceDestroyAndFree ABOVE, and for the same
        // reason: whatever mints a handle owns the death of that handle, and the mint for this
        // kind is MGPipeVertexInputEmitter::EmitVertexElements - i.e. the client, on every
        // backend. Espryt's StateObjectDeathOps notice used to be the only free, so under a
        // backend that installs none the slot and the applier's record leaked per VAO, for
        // ever. It is now the SECOND, redundant path (Managers.cpp's
        // OnFrontendStateObjectDestroyed) and it must stay idempotent, which it is: the
        // notice resolves through the same lifetimeId -> slot map this function frees, and
        // MGPipeSlotAllocator::Free refuses a slot that is not live at that generation.
        // May be the null handle: no slot is minted for a VAO that no draw ever validated with
        // and no backend twin table ever looked up. That case still raises the notice below -
        // see there.
        const MGPipeHandle handle =
            MGPipeSlots().FindByLifetimeId(MGPipeKind::VertexElementsCso, lifetimeId);

        // ASKED, NOT ASSUMED. A slot is not evidence of a record: DirectGLES mints one from
        // BackendSlotTable::GetOrCreate at every VAO sync, whether or not bit 8 asked this
        // client to emit a create - the shipping 0x7f A/B control arm is exactly that
        // configuration. delete_vertex_elements on a handle the applier has no record for is a
        // refusal, and the refusal asserts (PipeApply.cpp's ResolveVertexElements), i.e. it
        // stops a verify build.
        MGPipeVertexInputEmitter& emitter = MGPipeVertexInputEmitterInstance();
        const Bool published = emitter.RecordIsPublished(handle);
        if (published) {
            MGPHandleOnly only{};
            only.Handle = handle;
            only.Kind = static_cast<Uint32>(MGPipeKind::VertexElementsCso);
            MGPipeRouteDeleteVertexElements(only);
            emitter.NoteRecordDestroyed(handle);
        }

        // THE ORDER IS D-L's, WITH THE BACKEND NOTICE IN THE MIDDLE, and each of the three
        // positions is load-bearing:
        //   * the applier's record is dropped FIRST, while nothing else can have re-handed the
        //     slot out, so a recycled slot cannot inherit a field;
        //   * the death notice is raised SECOND, because it resolves the handle through the
        //     allocator and a backend told after the Free below could no longer find its twin
        //     - which would move the leak from the client to the driver VAO. It is raised
        //     UNCONDITIONALLY, exactly as ~VertexArrayObject raised it before C-1: whether a
        //     slot exists is this client's business, and a consumer that records notices (the
        //     P2 e2 gate does) must not stop seeing this class announce itself;
        //   * the slot goes back LAST. Espryt's notice frees it too; that Free and this one
        //     are the same call on the same handle and the second is a no-op, because Free
        //     bumps no generation (the bump rides the next handout) and refuses a slot that is
        //     no longer live at this generation.
        MG_State::GLState::NotifyStateObjectDestroyed(MGPipeKind::VertexElementsCso, lifetimeId);
        if (!MGPipeHandleIsNull(handle)) MGPipeSlots().Free(MGPipeKind::VertexElementsCso, handle);
        return published;
    }

    // ================================================================================
    // P4a: the BIRTH half - the gate, the four mints, the publication latch and the seam
    // ================================================================================
    //
    // Declared in MG_Pipe/PipeMutation.h, which is the one door MG_State has into the client
    // (the closure gate's mutation-header probe keeps it a declaration), and defined here for
    // the reason every other client-side emission point is: this file is package A's for the
    // whole phase, so the gate is written ONCE and the packages that own the emitters never
    // edit it.
    namespace {
        using MG_State::GLState::FramebufferObject;
        using MG_State::GLState::ITextureObject;
        using MG_State::GLState::ProgramObject;
        using MG_State::GLState::RenderbufferObject;
        using MG_State::GLState::SamplerObject;

        // THE FOUR FAMILIES P4a MIGRATES, as one mask, so the consumer rule below is stated
        // once instead of four times. It is deliberately NOT kMGPipeSubsystemsMigratedAtP4a
        // (which is 0x1fff, every bit through P4a): the rule belongs to the families this
        // phase adds and to no earlier one.
        inline constexpr Uint64 kMGPipeP4aFamilySubsystems =
            kMGPipeSubsystemFramebuffer | kMGPipeSubsystemTextureResources |
            kMGPipeSubsystemSamplers | kMGPipeSubsystemPrograms;

        // AND THE THIRD HALF, WHICH IS P3a's SECOND ONE: HAS A BACKEND REGISTERED THE CONSUMER?
        //
        // `MGPipeResourceSubsystemEnabled()` (above, ~:612) is bit 7 AND
        // `MGPipeGetResourceOps() != nullptr`, and the second conjunct is not decoration - it is
        // what keeps P3a's buffers on the legacy pull path under a backend that registers no
        // table. DirectVulkan (Magma) is exactly that backend: it registers no
        // MGPipeResourceOps and has none of P4a's twins. Without this conjunct the four P4a
        // families emitted there anyway, the applier ACCEPTED every record, the emitters cleared
        // their per-level dirty flags on that acceptance (D-D5 as amended by ID-18 M3), and
        // Magma's legacy upload path then found nothing left to upload: 66 texture-upload-shaped
        // DirectVulkan integration-gpu cases red on the push build at the default mask, with the
        // pull build 966/966 green (ID-39).
        //
        // ALL FOUR FAMILIES RIDE THE ONE SIGNAL, and the reason is D-D1: a texture and a
        // renderbuffer are RESOURCE rows - they travel on P3a's own resource_create /
        // resource_respecify / resource_subdata catalogue, whose consumer IS this table - so the
        // texture family's gate is P3a's gate by construction. The other three name texture
        // handles and cannot be live without it (MGPSurface::Res is a texture or renderbuffer
        // handle, MGPBoundView::Texture and MGPImageView::Res are texture handles, and
        // MGPTextureParams is addressed by one), so they follow. There is no fifth signal to
        // invent and no per-family registration to add: a backend that consumes P4a records
        // consumes resource rows first.
        //
        // A BACKEND THAT REGISTERS ONE IS UNAFFECTED. DirectGLES (Espryt) registers the table
        // at RegisterBufferBackendOps, unconditionally and at bring-up, so every predicate
        // below answers exactly what it answered before this commit.
        //
        // THE REGISTER/UNREGISTER WINDOW IS THE SAME ONE P3a LIVES WITH, and it is closed the
        // same way: UnregisterBufferBackendOps nulls the table at context teardown and the
        // re-register happens at the next MakeCurrent, so an object born in that window never
        // publishes a create and latches Published = false - after which the family's own
        // self-healing create on the next respecify (TextureEmit.h ~:576 / ~:709, the shape
        // MGPipeEmitResourceRespecify above uses for buffers) publishes it. Nothing here needs
        // to remember the window.
        Bool P4aFamilyHasItsConsumer(Uint64 subsystem) {
            if ((subsystem & kMGPipeP4aFamilySubsystems) == 0) return true;
#if MOBILEGL_BUILD_DISAGGREGATED
            // R-8 (c1), the same move as MGPipeResourceSubsystemEnabled's and for the same
            // reason. ALL FOUR FAMILIES RIDE THE ONE SIGNAL, exactly as they do in monolith:
            // the paragraph above explains why the resource consumer IS the texture family's
            // consumer, and the split spelling of "a backend registered the resource op table"
            // is "the server published the resource subsystem's consumer bit". Asking per
            // family here would be a NEW rule, and a client that withheld more than the server
            // refuses leaves the server's handle arm live with no records to read.
            if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
                // P5f fm: texture/framebuffer records have their own server consumers;
                // Magma consumes them without claiming the still-unmigrated buffer family.
                return MG_Remote::Client::CapsMirrorInstance().ServerConsumes(
                    subsystem & kMGPipeP4aFamilySubsystems);
            }
#endif
            return MGPipeGetResourceOps() != nullptr;
        }

        // ================================================================================
        // AND THE FOURTH HALF: D-K2's DEPENDENCY TABLE, ON THE CLIENT (S-3, ID-41)
        // ================================================================================
        //
        // THE DEFECT THIS CLOSES. Espryt's four `Resolve<Family>SubsystemArm()` functions
        // (Managers.cpp ~:3595-3745) REFUSE a family whose D-K2 dependency bit is clear and run
        // the legacy arm instead - the shape ResolveVertexInputSubsystemArm's bit-8-requires-
        // bit-7 refusal set as the precedent. That refusal is a BACKSTOP and it cannot restore a
        // correct picture on its own, because the client's emission was gated on the operator's
        // mask ALONE: at 0x7ff (bit 10 set, bit 11 clear) the client emitted the whole texture
        // family, the applier accepted it, the emitter cleared each level's per-level dirty flag
        // on that acceptance (D-D5 as amended by ID-18 M3) - and then the server refused bit 10
        // and ran the legacy path, which found nothing left to upload. 438/491 on the DirectGLES
        // integration lane, the same 47 texture-upload failures ID-39 saw on Magma for the
        // consumer-less version of exactly this mistake.
        //
        // So the rule is the SAME "nothing at all, not less" rule as the consumer conjunct
        // above: with a dependency unmet the client emits NOTHING for that family and the legacy
        // pull path runs untouched, on both sides of the boundary.
        //
        // THE TABLE IS WRITTEN ONCE, HERE, and every one of its rows is the client mirror of the
        // refusal Espryt already implements, bit for bit and non-transitively - the two must say
        // the SAME thing, because a client that withheld more than the server refuses would
        // leave the server's handle arm live with no records to read, and a client that withheld
        // less is the defect above.
        struct P4aFamilyDependencyRow {
            Uint64 Family;   // exactly one bit, and it is one of kMGPipeP4aFamilySubsystems
            Uint64 Requires; // the bits MOBILEGL_PIPE_PUSH must ALSO carry for it to be live
        };

        inline constexpr P4aFamilyDependencyRow kMGPipeP4aFamilyDependencies[] = {
            // BIT 9 REQUIRES BIT 10. Every MGPSurface::Res in a set_framebuffer_state record
            // names a Texture or a Renderbuffer handle, and only bit 10 populates those two slot
            // tables (Managers.cpp ResolveFramebufferSubsystemArm).
            {kMGPipeSubsystemFramebuffer, kMGPipeSubsystemTextureResources},

            // BIT 10 REQUIRES BIT 7 - a buffer texture's MGPResourceDesc::BufferForTexBuffer
            // names a Buffer handle and only bit 7 puts twins in the resource slot table (D-D1,
            // ResolveTextureResourceSubsystemArm's first row) - AND BIT 11, which is D-K2's
            // FOURTH row (ID-14/ID-15): MGPTextureParams::BuiltinSampler is a SamplerCso HANDLE,
            // only bit 11 mints sampler CSOs (c0b's four unconditional mints deliberately
            // exclude it), and the applier's verdict for a null one is Fatal{ProtocolCorruption}
            // rather than a decline. The brief's original "bit 10 without 11 is fine" is
            // WITHDRAWN for P4a as built.
            {kMGPipeSubsystemTextureResources,
             kMGPipeSubsystemResources | kMGPipeSubsystemSamplers},

            // BIT 11 REQUIRES BIT 10. Every MGPBoundView::Texture and every MGPImageView::Res
            // names a Texture handle and only bit 10 populates that slot table; without it every
            // per-unit lookup would miss and the walk would `continue` WITHOUT unbinding
            // (ResolveSamplerSubsystemArm). With the row above this is SYMMETRIC: bits 10 and 11
            // are one arm with two switches, and the only two masks that reach either handle arm
            // are "both set" and "neither set".
            {kMGPipeSubsystemSamplers, kMGPipeSubsystemTextureResources},

            // BIT 12 DEPENDS ON NOTHING, and that is a ROW rather than an absence so the table
            // covers the four families exhaustively (the static_assert below): a ShaderCso handle
            // names no texture and no buffer, the archive rides beside the record as a companion
            // pointer, and the extra inputs the server specialises on are read from state the
            // backend already holds (ResolveProgramSubsystemArm).
            {kMGPipeSubsystemPrograms, 0},
        };

        // THE MIRROR PAIRS THAT STAY FINE, said out loud rather than left as an absence, because
        // an unreachable branch that says something different is how the reachable one drifts
        // (Managers.cpp's own words at :2377-2381) - and because the table is only trustworthy if
        // what it does NOT contain was decided rather than forgotten:
        //   - bit 10 set, bit 9 clear: FINE. The legacy FBO sync reaches the texture twin through
        //     SyncTextureObjectToBackend, which dispatches to the handle arm by itself.
        //   - bit 11 set, bit 9 clear: FINE, for the same reason - a sampler view names a texture,
        //     never a framebuffer.
        //   - bit 7 set, bit 10 clear: FINE, and it is P3a's shipped configuration.
        //   - bit 12 set with any or none of 9/10/11: FINE, per the last row.
        //   - bit 10 set, bit 11 clear (and its mirror) is NOT fine and is the row above; this is
        //     the one sentence in the brief that P4a as built withdrew.
        constexpr Uint64 P4aFamilyDependencyBits(Uint64 subsystem) {
            Uint64 required = 0;
            for (const P4aFamilyDependencyRow& row : kMGPipeP4aFamilyDependencies) {
                if ((subsystem & row.Family) != 0) required |= row.Requires;
            }
            return required;
        }

        // The table covers the four families this phase migrates and nothing else, so a fifth
        // family added to kMGPipeP4aFamilySubsystems without a row here does not silently inherit
        // "depends on nothing".
        constexpr Uint64 P4aFamilyDependencyTableCoverage() {
            Uint64 covered = 0;
            for (const P4aFamilyDependencyRow& row : kMGPipeP4aFamilyDependencies) covered |= row.Family;
            return covered;
        }
        static_assert(P4aFamilyDependencyTableCoverage() == kMGPipeP4aFamilySubsystems,
                      "every P4a family needs a D-K2 dependency row, even an empty one");
        static_assert(P4aFamilyDependencyBits(kMGPipeSubsystemFramebuffer) ==
                          kMGPipeSubsystemTextureResources,
                      "bit 9 requires bit 10");
        static_assert(P4aFamilyDependencyBits(kMGPipeSubsystemTextureResources) ==
                          (kMGPipeSubsystemResources | kMGPipeSubsystemSamplers),
                      "bit 10 requires bit 7 and bit 11");
        static_assert(P4aFamilyDependencyBits(kMGPipeSubsystemSamplers) ==
                          kMGPipeSubsystemTextureResources,
                      "bit 11 requires bit 10");
        static_assert(P4aFamilyDependencyBits(kMGPipeSubsystemPrograms) == 0, "bit 12 depends on nothing");
        // No family may depend on itself: a row that did would be unfalsifiable (its own bit is
        // set by the time the conjunct is evaluated) and would read as a dependency nobody has.
        static_assert((P4aFamilyDependencyBits(kMGPipeSubsystemFramebuffer) &
                       kMGPipeSubsystemFramebuffer) == 0 &&
                          (P4aFamilyDependencyBits(kMGPipeSubsystemTextureResources) &
                           kMGPipeSubsystemTextureResources) == 0 &&
                          (P4aFamilyDependencyBits(kMGPipeSubsystemSamplers) &
                           kMGPipeSubsystemSamplers) == 0,
                      "a D-K2 row must not name its own family");
        // The default mask carries every dependency, so the shipped arm is unchanged by all of
        // this - the table only ever narrows a HAND-PICKED A/B mask.
        static_assert((kMGPipeSubsystemsMigratedAtP4a &
                       P4aFamilyDependencyBits(kMGPipeP4aFamilySubsystems)) ==
                          P4aFamilyDependencyBits(kMGPipeP4aFamilySubsystems),
                      "the P4a phase mask must satisfy every dependency it declares");

        // IT IS THE RUNTIME BIT THAT IS TESTED, NOT THE OTHER FAMILY'S LIVENESS, and that is
        // deliberate: Espryt's resolvers classify their arms from MOBILEGL_PIPE_PUSH alone, so
        // testing anything else here would make the two sides disagree at some mask - which is
        // the failure this whole commit is about, one level up. The mask is passed in rather than
        // read, so the walk's single read of MG_Config::Features.PipePush stays the one read a
        // whole validate point resolves against.
        Bool P4aFamilyDependenciesAreSet(Uint64 subsystem, Uint64 pushMask) {
            const Uint64 required = P4aFamilyDependencyBits(subsystem);
            return (pushMask & required) == required;
        }

        // THE SAME QUADRUPLE `wants()` APPLIES TO EVERY EMISSION at the validate point, and it is
        // deliberately the same predicate rather than a second copy of it: the operator's
        // per-subsystem A/B bit in MOBILEGL_PIPE_PUSH, this build having WIRED the family
        // at all, AND - for a P4a family - a backend having registered the consumer and every
        // D-K2 dependency bit of the family being set. The second half is the family's own
        // kMGPipeWired*Subsystem constant, which lives in the family's emit header and is 0 until
        // the commit that gives the emitter its body - so a client path that lands before its
        // emitter does is inert by construction rather than by everyone remembering to check; the
        // third is P4aFamilyHasItsConsumer above and the fourth is P4aFamilyDependenciesAreSet.
        // P5e (sb, ID-106 and CONTRACT-P5E.md §1). THE BINDING-POINT FAMILY's TWO EXTRA
        // CONJUNCTS, kept beside P4a's rather than folded into them, because it asks a
        // DIFFERENT consumer question. P4a's four families all ride the resource family's one
        // signal (P4aFamilyHasItsConsumer says why); bit 13's consumer question is bit 13's
        // own - MG_Backend/Init.cpp publishes it in the same commit that sets
        // ShaderBufferEmit.h's wired constant, and a server that does not publish it is a
        // server whose four binding-point walks still read the frontend, so a record sent to it
        // would be stored and never looked at while the client latched its suppressor.
        //
        // AND BIT 13 REQUIRES BIT 7, for bit 11's reason one family over: every
        // MGPBufferRange::Res names a Buffer handle and only bit 7 puts one in the resource
        // slot table, so without it every EnsureBufferResourceForHandle on the server would
        // mint a twin with no record behind it. Withholding the whole family is the safe
        // direction - the legacy frontend walk runs untouched on both sides.
        Bool P5eFamilyIsLive(Uint64 subsystem, Uint64 pushMask) {
            if ((subsystem & kMGPipeSubsystemBufferBindings) == 0) return true;
            if ((pushMask & kMGPipeSubsystemResources) == 0) return false;
#if MOBILEGL_BUILD_DISAGGREGATED
            if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
                return MG_Remote::Client::CapsMirrorInstance().ServerConsumes(
                    kMGPipeSubsystemBufferBindings);
            }
#endif
            // Under monolith the "consumer" is the backend that registered the resource op
            // table, exactly as it is for P4a's four: there is no caps snapshot to ask and the
            // binding-point walks live in the same DirectGLES that registers it.
            return MGPipeGetResourceOps() != nullptr;
        }

        Bool FamilyIsLive(Uint64 subsystem, Uint64 wired) {
            const Uint64 pushMask = MG_Config::Features.PipePush;
            return (pushMask & subsystem) != 0 && (wired & subsystem) != 0 &&
                   P4aFamilyHasItsConsumer(subsystem) &&
                   P4aFamilyDependenciesAreSet(subsystem, pushMask) &&
                   P5eFamilyIsLive(subsystem, pushMask);
        }

        // ---- THE FAMILY SEAM ----
        //
        // The forwarding from a birth hook to its family's emitter has to be written HERE,
        // once, against an emitter whose entry point does not exist yet: A owns this file for
        // the whole phase and B/C own the five emit headers, and neither may edit the other's.
        // A plain call would not compile against the stub emitter and a runtime `if` would not
        // link. So the call is made from a TEMPLATE whose `if constexpr` condition is the
        // family's own wired constant, passed as a template ARGUMENT so the condition is
        // value-dependent: while the constant is 0 the statement is discarded and never
        // instantiated, so this tree compiles against the stubs; the moment a family sets its
        // constant the statement instantiates and a missing or misspelled entry point is a
        // COMPILE ERROR in that family's own commit rather than a surprise at the merge. That
        // is the same property the four `kMGPipeWired*Subsystem == 0 || == its own bit`
        // asserts below give, one level further in.
        //
        // `call` must be a GENERIC lambda - `[&](auto& emitter) { ... }` - so its body is
        // checked at instantiation and not at definition. A non-generic one would be checked
        // here and would defeat the whole seam.
        template <Uint64 kWired, class Emitter, class Fn>
        constexpr void ForwardWhenWired(Emitter& emitter, Fn&& call) {
            if constexpr (kWired != 0) {
                call(emitter);
            } else {
                (void)emitter;
                (void)call;
            }
        }

        // THE SEAM'S POSITIVE CONTROL, and it is not decoration: every use of it in this tree
        // passes a constant that is 0, so the TAKEN arm is never instantiated here and a seam
        // that failed to compile or failed to call would be discovered by package B or C
        // rather than by the commit that wrote it. This drives both arms against a probe
        // emitter shaped like the ones the emit headers will carry, and asserts that exactly
        // one call happened - so "discarded when 0, called when set" is a checked property of
        // this build rather than a claim in the paragraph above.
        struct SeamProbeEmitter {
            Uint32 Calls = 0;
            constexpr void Probe() { ++Calls; }
        };

        constexpr Bool SeamForwardsExactlyWhenWired() {
            SeamProbeEmitter probe{};
            ForwardWhenWired<1ull>(probe, [](auto& emitter) { emitter.Probe(); });
            ForwardWhenWired<0ull>(probe, [](auto& emitter) { emitter.Probe(); });
            return probe.Calls == 1;
        }

        static_assert(SeamForwardsExactlyWhenWired(),
                      "the family seam must forward exactly when its wired constant is non-zero");

        // ---- THE PUBLICATION LATCH (D-I1) ----
        //
        // "Did a create for exactly this handle actually go out?" - asked by the six death
        // helpers below and answered by whatever emitted the create. It exists because the
        // create is gated at its call site and the destroy inside the helper, so the two ask
        // the same question at two different moments; and because A SLOT IS NOT EVIDENCE OF A
        // RECORD - a backend twin table mints one through MGPipeSlots().Acquire whether or not
        // the subsystem ever asked this client to emit anything, which is exactly what a
        // MOBILEGL_PIPE_PUSH lane with P4a's bits clear runs, and a delete_* on such a handle
        // is a refused call the applier asserts on in a verify build.
        //
        // KEYED BY {kind, slot, gen}, so a recycled slot cannot inherit its predecessor's
        // answer - the same reason the identity carries a generation at all.
        //
        // THE ShaderCso COMPOSITE BAND GETS A TABLE OF ITS OWN, exactly as the allocator's
        // does and for the same arithmetic: the band's base is 983040, so a single composite
        // in a slot-indexed vector would allocate ~983k entries. Anything that indexes a
        // ShaderCso slot must test MGPipeIsCompositeShaderSlot(slot) FIRST; this is the
        // client-side worked example of that rule.
        class MGPipePublicationLatch {
        public:
            void NotePublished(MGPipeKind kind, MGPipeHandle handle) {
                Entry* entry = Grow(kind, handle.Slot);
                if (entry == nullptr) return;
                entry->Gen = handle.Gen;
                entry->Published = true;
            }

            Bool IsPublished(MGPipeKind kind, MGPipeHandle handle) const {
                const Entry* entry = Find(kind, handle.Slot);
                return entry != nullptr && entry->Published && entry->Gen == handle.Gen;
            }

            void NoteUnpublished(MGPipeKind kind, MGPipeHandle handle) {
                Entry* entry = const_cast<Entry*>(Find(kind, handle.Slot));
                if (entry == nullptr || entry->Gen != handle.Gen) return;
                *entry = Entry{};
            }

        private:
            struct Entry {
                Uint32 Gen = 0;
                Bool Published = false;
            };

            static constexpr SizeT kKindCount = static_cast<SizeT>(MGPipeKind::KindCount);

            Bool IsBand(MGPipeKind kind, Uint32 slot) const {
                return kind == MGPipeKind::ShaderCso && MGPipeIsCompositeShaderSlot(slot);
            }

            Entry* Grow(MGPipeKind kind, Uint32 slot) {
                const SizeT index = static_cast<SizeT>(kind);
                if (index >= kKindCount) return nullptr;
                if (IsBand(kind, slot)) {
                    const SizeT banded = slot - kMGPipeShaderCsoCompositeSlotBase;
                    if (banded >= m_band.size()) m_band.resize(banded + 1);
                    return &m_band[banded];
                }
                Vector<Entry>& table = m_kinds[index];
                if (slot >= table.size()) table.resize(static_cast<SizeT>(slot) + 1);
                return &table[slot];
            }

            const Entry* Find(MGPipeKind kind, Uint32 slot) const {
                const SizeT index = static_cast<SizeT>(kind);
                if (index >= kKindCount) return nullptr;
                if (IsBand(kind, slot)) {
                    const SizeT banded = slot - kMGPipeShaderCsoCompositeSlotBase;
                    return banded < m_band.size() ? &m_band[banded] : nullptr;
                }
                const Vector<Entry>& table = m_kinds[index];
                return slot < table.size() ? &table[slot] : nullptr;
            }

            Array<Vector<Entry>, kKindCount> m_kinds{};
            Vector<Entry> m_band{};
        };

        MGPipePublicationLatch& PublicationLatch() {
            // NEVER DESTROYED, for MGPipeSlots()' reason: the six death helpers reach this
            // from frontend destructors that __run_exit_handlers drives AFTER a function-local
            // static would have gone, and a destroyed latch answers out of freed vectors.
            static MGPipePublicationLatch* latch = new MGPipePublicationLatch();
            return *latch;
        }
    } // namespace

    void MGPipeNoteHandlePublished(MGPipeKind kind, MGPipeHandle handle) {
        if (MGPipeHandleIsNull(handle)) return;
        PublicationLatch().NotePublished(kind, handle);
    }

    Bool MGPipeHandleIsPublished(MGPipeKind kind, MGPipeHandle handle) {
        if (MGPipeHandleIsNull(handle)) return false;
        return PublicationLatch().IsPublished(kind, handle);
    }

    void MGPipeNoteHandleUnpublished(MGPipeKind kind, MGPipeHandle handle) {
        if (MGPipeHandleIsNull(handle)) return;
        PublicationLatch().NoteUnpublished(kind, handle);
    }

    // THE GATE ITSELF, AS AN OBSERVABLE (ID-39, widened by S-3 / ID-41). Every P4a birth hook
    // below and every `wants()` row in the walk resolve through FamilyIsLive /
    // P4aFamilyHasItsConsumer / P4aFamilyDependenciesAreSet, and none of the three is reachable
    // from a test - so this is the one door a unit case has onto the answer, and it is the SAME
    // expression rather than a second copy of it. A subsystem outside kMGPipeP4aFamilySubsystems
    // answers the pair the P2/P3a families have always answered (its consumer conjunct is
    // vacuous and its dependency set is empty), which is what makes "nothing that emits today
    // changes" checkable instead of asserted.
    Bool MGPipeP4aFamilyEmits(Uint64 subsystem, Uint64 wired) {
        return FamilyIsLive(subsystem, wired);
    }

    void MGPipeMintTextureHandle(ITextureObject& texture) {
        MGPipeSlots().Acquire(MGPipeKind::Texture, texture.GetLifetimeId());
    }

    void MGPipeMintRenderbufferHandle(RenderbufferObject& renderbuffer) {
        MGPipeSlots().Acquire(MGPipeKind::Renderbuffer, renderbuffer.GetLifetimeId());
    }

    void MGPipeMintFramebufferHandle(FramebufferObject& framebuffer) {
        MGPipeSlots().Acquire(MGPipeKind::Framebuffer, framebuffer.GetLifetimeId());
    }

    void MGPipeMintShaderCsoHandle(ProgramObject& program) {
        MGPipeSlots().Acquire(MGPipeKind::ShaderCso, program.GetLifetimeId());
    }

    void MGPipeEmitTextureResourceCreate(ITextureObject& texture) {
        if (!FamilyIsLive(kMGPipeSubsystemTextureResources, kMGPipeWiredTextureSubsystem)) return;
        ForwardWhenWired<kMGPipeWiredTextureSubsystem>(
            MGPipeTextureEmitterInstance(), [&](auto& emitter) { emitter.EmitResourceCreate(texture); });
    }

    void MGPipeEmitTextureResourceRespecify(ITextureObject& texture, MGPipeTextureRespecifyScope scope,
                                            Uint32 uploadTarget, Uint32 level) {
        if (!FamilyIsLive(kMGPipeSubsystemTextureResources, kMGPipeWiredTextureSubsystem)) return;
        ForwardWhenWired<kMGPipeWiredTextureSubsystem>(
            MGPipeTextureEmitterInstance(),
            [&](auto& emitter) { emitter.EmitResourceRespecify(texture, scope, uploadTarget, level); });
    }

    void MGPipeEmitTextureParams(ITextureObject& texture) {
        if (!FamilyIsLive(kMGPipeSubsystemTextureResources, kMGPipeWiredTextureSubsystem)) return;
        ForwardWhenWired<kMGPipeWiredTextureSubsystem>(
            MGPipeTextureEmitterInstance(), [&](auto& emitter) { emitter.EmitTextureParams(texture); });
    }

    void MGPipeNoteTextureLevelDirty(ITextureObject& storageOwner, Uint32 uploadTarget, Uint32 level) {
        if (!FamilyIsLive(kMGPipeSubsystemTextureResources, kMGPipeWiredTextureSubsystem)) return;
        ForwardWhenWired<kMGPipeWiredTextureSubsystem>(
            MGPipeTextureEmitterInstance(),
            [&](auto& emitter) { emitter.NoteLevelDirty(storageOwner, uploadTarget, level); });
    }

    void MGPipeEmitRenderbufferResourceCreate(RenderbufferObject& renderbuffer) {
        if (!FamilyIsLive(kMGPipeSubsystemTextureResources, kMGPipeWiredTextureSubsystem)) return;
        ForwardWhenWired<kMGPipeWiredTextureSubsystem>(
            MGPipeTextureEmitterInstance(),
            [&](auto& emitter) { emitter.EmitRenderbufferCreate(renderbuffer); });
    }

    void MGPipeEmitRenderbufferResourceRespecify(RenderbufferObject& renderbuffer) {
        if (!FamilyIsLive(kMGPipeSubsystemTextureResources, kMGPipeWiredTextureSubsystem)) return;
        ForwardWhenWired<kMGPipeWiredTextureSubsystem>(
            MGPipeTextureEmitterInstance(),
            [&](auto& emitter) { emitter.EmitRenderbufferRespecify(renderbuffer); });
    }

    void MGPipeNoteTextureBoundAs(MGPipeHandle texture, Uint32 bindBit) {
        // Not gated on FamilyIsLive: the mask is client state (see the declaration), and the
        // emitter gates the emission it causes.
        ForwardWhenWired<kMGPipeWiredTextureSubsystem>(
            MGPipeTextureEmitterInstance(),
            [&](auto& emitter) { emitter.NoteTextureBoundAs(texture, static_cast<Uint16>(bindBit)); });
    }

    void MGPipeNoteTextureImageBound(ITextureObject& texture) {
        ForwardWhenWired<kMGPipeWiredTextureSubsystem>(MGPipeTextureEmitterInstance(), [&](auto& emitter) {
            emitter.NoteTextureBoundAs(emitter.AcquireTexture(texture.GetLifetimeId(), &texture),
                                       static_cast<Uint16>(kMGPipeBindShaderImage));
        });
    }

    void MGPipeEmitSamplerCsoCreate(SamplerObject& sampler) {
        if (!FamilyIsLive(kMGPipeSubsystemSamplers, kMGPipeWiredSamplerSubsystem)) return;
        ForwardWhenWired<kMGPipeWiredSamplerSubsystem>(
            MGPipeSamplerEmitterInstance(), [&](auto& emitter) { emitter.EmitSamplerCso(sampler); });
    }

    void MGPipeEmitSamplerViewCreate(ITextureObject& texture) {
        if (!FamilyIsLive(kMGPipeSubsystemSamplers, kMGPipeWiredSamplerSubsystem)) return;
        ForwardWhenWired<kMGPipeWiredSamplerSubsystem>(
            MGPipeSamplerEmitterInstance(), [&](auto& emitter) { emitter.EmitSamplerView(texture); });
    }

    void MGPipeEmitShaderCsoCreate(ProgramObject& program) {
        if (!FamilyIsLive(kMGPipeSubsystemPrograms, kMGPipeWiredProgramSubsystem)) return;
        ForwardWhenWired<kMGPipeWiredProgramSubsystem>(
            MGPipeProgramEmitterInstance(), [&](auto& emitter) { emitter.EmitShaderCso(program); });
    }

    // ================================================================================
    // P4a: one client-side death helper per kind P4a mints (D-I1)
    // ================================================================================
    //
    // BACKEND-NEUTRAL FROM THE FIRST COMMIT, which is the whole point: before P3a's C-1 fix
    // the only thing that ever returned a VertexElementsCso slot was DirectGLES'
    // StateObjectDeathOps table, so under a backend that installs none every VAO leaked a slot
    // and a ~1.3 KB applier record for the life of the process. P4a mints SIX kinds and there
    // is no intermediate state in which a backend table is the only path for any of them.
    //
    // THE THREE-STEP ORDER IS FIXED and each position is load-bearing (see PipeMutation.h):
    // wire delete, then the death notice, then the slot free. Each helper returns whether its
    // delete actually went out, which is the LATCH taken at the object's create - asking a
    // live predicate twice pairs a create emitted under one registration with a destroy gated
    // on another, and either direction leaks.
    //
    // EVERY ONE OF THEM IS PUBLISHED-GATED RATHER THAN SLOT-GATED. A slot is not evidence of a
    // record: a backend twin table mints one through MGPipeSlots().Acquire whether or not the
    // subsystem ever asked this client to emit a create - which is exactly what a
    // MOBILEGL_PIPE_PUSH lane with P4a's bits clear runs - and a delete_* on such a handle is
    // a refused call the applier counts and asserts on. So the PUBLICATION LATCH above is
    // asked before any delete goes out, and it is the SAME latch whatever emitted the create
    // wrote - one answer per {kind, slot, gen}, not a second reading of a live predicate.
    //
    // THE LATCH RATHER THAN A PER-EMITTER RecordIsPublished(handle), deliberately, and it is
    // the one place P4a's shape differs from P3a's: P3a had one kind and one emitter, so the
    // emitter could hold the latch. P4a has six kinds behind FOUR emitters and one kind -
    // SamplerViewCso - with no frontend object at all, and a ShaderCso whose composite band
    // has two independent release paths. A latch this file owns is then the only thing all
    // six can read, and it keeps the answer out of the emit headers B and C are writing.
    //
    // AT THE CONTRACT COMMIT nothing latches a publication, because every family emitter is a
    // stub, so every helper here answers false and the legacy path runs unchanged - which is
    // what makes this commit behaviourally inert while the SHAPE is already the final one.
    namespace {
        // Steps 2 and 3, shared: raise the notice while the handle still resolves, then return
        // the slot. Raised UNCONDITIONALLY, exactly as the five destructors raised it before
        // P4a: whether a slot exists is this client's business, and a consumer that records
        // notices must not stop seeing a class announce itself.
        void NotifyAndFree(MGPipeKind kind, Uint64 lifetimeId, MGPipeHandle handle) {
            MG_State::GLState::NotifyStateObjectDestroyed(kind, lifetimeId);
            if (!MGPipeHandleIsNull(handle)) MGPipeSlots().Free(kind, handle);
        }

        MGPHandleOnly HandleOnly(MGPipeKind kind, MGPipeHandle handle) {
            MGPHandleOnly only{};
            only.Handle = handle;
            only.Kind = static_cast<Uint32>(kind);
            return only;
        }

        // Step 1, shared: the wire delete goes out FIRST and only for a PUBLISHED handle, and
        // the latch is cleared with it so a second death path - a composite's two, a backend's
        // redundant notice - cannot emit a second delete for a record that is already gone.
        //
        // `route` IS A MGPipeRoute<Name> AND NEVER A MGPipeApply<Name> (B1). R-17 converted the
        // 40 direct applier CALLS to route calls by renaming `MGPipeApply<Name>(` -> but these
        // five sites take the entry point BY ADDRESS, `&MGPipeApply<Name>`, so the call-expression
        // rename missed them and four routed rows (DeleteSamplerView, DeleteShaderState,
        // DeleteSamplerState, ResourceDestroy for textures/renderbuffers) still ran the applier
        // synchronously on the GL thread under split - two writers on g_applier with the barrier
        // not consulted, and under spawn a silent no-op that leaks every one of those objects.
        // The parameter type is the route's, which is byte-identical to the applier's
        // (const MGPHandleOnly&, void return), so the fix is `&MGPipeRoute<Name>` at the five call
        // sites; PipeCatalogue.FrontendNeverTakesAnApplierAddress checks MG_Impl in every unit lane.
        Bool EmitDeleteIfPublished(MGPipeKind kind, MGPipeHandle handle,
                                   void (*route)(const MGPHandleOnly&)) {
            if (!MGPipeHandleIsPublished(kind, handle)) return false;
            route(HandleOnly(kind, handle));
            MGPipeNoteHandleUnpublished(kind, handle);
            return true;
        }
    } // namespace

    // THE EMITTER IS TOLD BETWEEN THE WIRE DELETE AND THE FREE (P4a final review C-2), for
    // every kind that keeps client state under a handle: a texture's drain entries, pointer,
    // cache reference and latches; a renderbuffer's entry; a framebuffer's Named latch; a
    // sampler view's and a shader CSO's record memo. Before this the six helpers freed the slot
    // and told nobody, so the texture emitter kept the freed ITextureObject* and the level on
    // the drain list, and `glTexImage2D; glDeleteTextures; <verb>` called a virtual on freed
    // memory from the next validate point. The forward is the P3a shape
    // (MGPipeEmitVertexElementsDestroyAndFree's emitter.NoteRecordDestroyed) applied to the
    // five P4a kinds that have an entry to retire; the content-addressed sampler CSO keeps
    // none per object (its death is the cache's LRU, ID-17). Unconditional in a push build,
    // like the mints: the entries exist whether or not the family bit is set.
    Bool MGPipeEmitSamplerViewCsoDestroyAndFree(Uint64 lifetimeId) {
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MGPipeDeferDestroyAndFreeIfOnApplyThread(MGPipeKind::SamplerViewCso, lifetimeId)) {
            return false;
        }
#endif
        const MGPipeHandle handle =
            MGPipeSlots().FindByLifetimeId(MGPipeKind::SamplerViewCso, lifetimeId);
        const Bool published =
            EmitDeleteIfPublished(MGPipeKind::SamplerViewCso, handle, &MGPipeRouteDeleteSamplerView);
        ForwardWhenWired<kMGPipeWiredSamplerSubsystem>(
            MGPipeSamplerEmitterInstance(), [&](auto& emitter) { emitter.NoteRecordDestroyed(handle); });
        // THE NOTICE IS RAISED FOR THIS KIND TOO, and the reason it once was not is wrong:
        // NotifyStateObjectDestroyed takes a KIND and a lifetime id, not an object
        // (StateObjectDeathNotice.h - one entry point for every kind rather than one ops table
        // per kind), MGPipeKind has SamplerViewCso, and the view IS keyed in that kind's
        // ByLifetimeId map under the texture's id - which is exactly what the FindByLifetimeId
        // above just resolved. "It has no frontend object of its own" is why it takes the
        // lifetime id; it is not a reason to drop step 2. A backend that holds a twin per
        // SamplerViewCso slot - which is the shape both backends' slot tables take - would
        // otherwise never be told to drop it, and under a backend with no other per-kind free
        // path never drop it at all: the C-1 leak, one kind later, and invisible to
        // PipeSlotPeek because the SLOT was returned correctly.
        NotifyAndFree(MGPipeKind::SamplerViewCso, lifetimeId, handle);
        return published;
    }

    Bool MGPipeEmitTextureDestroyAndFree(Uint64 lifetimeId) {
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MGPipeDeferDestroyAndFreeIfOnApplyThread(MGPipeKind::Texture, lifetimeId)) {
            return false;
        }
#endif
        const MGPipeHandle handle = MGPipeSlots().FindByLifetimeId(MGPipeKind::Texture, lifetimeId);
        const Bool published =
            EmitDeleteIfPublished(MGPipeKind::Texture, handle, &MGPipeRouteResourceDestroy);
        // The emitter retires its entry while the handle still resolves (C-2): the drain list
        // drops the dead texture's levels, the raw pointer goes, the built-in sampler's cache
        // reference is given back, the latches and the sticky mask are cleared.
        ForwardWhenWired<kMGPipeWiredTextureSubsystem>(
            MGPipeTextureEmitterInstance(), [&](auto& emitter) { emitter.NoteTextureDied(handle); });
        NotifyAndFree(MGPipeKind::Texture, lifetimeId, handle);
        // THE SAMPLER VIEW DIES WITH ITS TEXTURE, because it is minted off the same lifetime
        // id: one SamplerViewCso per ITextureObject (D-F2), re-issued on the same handle
        // whenever the restrictions move. Released AFTER the texture's own record, so a server
        // that reads the view to answer "what is this texture" still can while the texture is
        // being dropped.
        //
        // THE BUILT-IN SAMPLER IS NOT RELEASED HERE, and that is a correction to the design
        // table rather than an omission: the SamplerObject every ITextureObject owns is a real
        // frontend object with its OWN lifetime id and its own #if MOBILEGL_PIPE_PUSH
        // destructor, so freeing it from the texture's lifetime id would resolve the wrong slot
        // (or, worse, a live one belonging to another object). Its release therefore rides
        // ~SamplerObject and MGPipeEmitSamplerCsoDestroyAndFree below - the same helper, the
        // same three-step order, idempotent.
        //
        // WHEN that runs is NOT ordered against this body and nothing here may assume it is.
        // m_sampler is a SharedPtr, so a texture unit slot or a sampler-view resolution that
        // took a reference delays ~SamplerObject arbitrarily; "a member's destructor follows
        // its owner's body" would be true of a by-value member and is not true of this one.
        // The conclusion above does not depend on the timing - the two ids are different, so
        // the two releases are independent whichever order they happen in - but a package must
        // not build an ordering on it.
        //
        // AND THE VIEW'S ANSWER IS OR-ED IN, not dropped: a texture whose ResourceDestroy was
        // suppressed (nothing ever published it) but whose DeleteSamplerView did go out has
        // already spoken on the wire for this object, and reporting false would run the legacy
        // path for both halves.
        const Bool viewPublished = MGPipeEmitSamplerViewCsoDestroyAndFree(lifetimeId);
        return published || viewPublished;
    }

    Bool MGPipeEmitRenderbufferDestroyAndFree(Uint64 lifetimeId) {
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MGPipeDeferDestroyAndFreeIfOnApplyThread(MGPipeKind::Renderbuffer, lifetimeId)) {
            return false;
        }
#endif
        const MGPipeHandle handle =
            MGPipeSlots().FindByLifetimeId(MGPipeKind::Renderbuffer, lifetimeId);
        const Bool published =
            EmitDeleteIfPublished(MGPipeKind::Renderbuffer, handle, &MGPipeRouteResourceDestroy);
        ForwardWhenWired<kMGPipeWiredTextureSubsystem>(
            MGPipeTextureEmitterInstance(), [&](auto& emitter) { emitter.NoteRenderbufferDied(handle); });
        NotifyAndFree(MGPipeKind::Renderbuffer, lifetimeId, handle);
        return published;
    }

    Bool MGPipeEmitFramebufferDestroyAndFree(Uint64 lifetimeId) {
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MGPipeDeferDestroyAndFreeIfOnApplyThread(MGPipeKind::Framebuffer, lifetimeId)) {
            return false;
        }
#endif
        // NO WIRE DELETE EXISTS FOR THIS KIND, and none is invented: PipeCalls.def has
        // resource_destroy and the five delete_* rows and no framebuffer delete, because a
        // framebuffer is not a resource and is not a CSO - it is STATE, and
        // set_framebuffer_state is the only call that names one. The catalogue is closed.
        //
        // So the handle is minted and freed entirely client-side and this helper is steps 2
        // and 3 only. What makes a dangling Fbo unreachable is the frontend's own
        // MarkFramebufferObjectForDeletion path, which already rebinds any slot holding the
        // victim to framebuffer 0; and a RECYCLED framebuffer handle can never be suppressed
        // against its predecessor's record, because Fbo carries Gen and Gen is inside the
        // record's ContentHash.
        //
        // NOTHING EVER TAKES THE PUBLICATION LATCH FOR THIS KIND, by contract and not by
        // omission: with no create there is nothing to latch, and with no delete there is
        // nothing for a latch to gate. The answer is therefore the literal false rather than a
        // latch read, and false is the right one - it means "the legacy path still owes
        // whatever it owed", which for a framebuffer is the death notice this just raised.
        const MGPipeHandle handle =
            MGPipeSlots().FindByLifetimeId(MGPipeKind::Framebuffer, lifetimeId);
        ForwardWhenWired<kMGPipeWiredFramebufferSubsystem>(
            MGPipeFramebufferEmitterInstance(), [&](auto& emitter) { emitter.NoteFramebufferDied(handle); });
        NotifyAndFree(MGPipeKind::Framebuffer, lifetimeId, handle);
        return false;
    }

    Bool MGPipeEmitSamplerCsoDestroyAndFree(Uint64 lifetimeId) {
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MGPipeDeferDestroyAndFreeIfOnApplyThread(MGPipeKind::SamplerCso, lifetimeId)) {
            return false;
        }
#endif
        const MGPipeHandle handle =
            MGPipeSlots().FindByLifetimeId(MGPipeKind::SamplerCso, lifetimeId);
        const Bool published =
            EmitDeleteIfPublished(MGPipeKind::SamplerCso, handle, &MGPipeRouteDeleteSamplerState);
        // NOTHING TO RETIRE IN AN EMITTER FOR THIS KIND, stated rather than implied: a sampler
        // CSO is content-addressed and belongs to a value, so no emitter keeps an entry under
        // a SamplerObject's handle - the cache's entries are keyed by value and reference
        // count, and the death of a bound sampler object releases its unit's reference at the
        // next bind_sampler_states pass (SamplerEmit.h's reconciliation).
        NotifyAndFree(MGPipeKind::SamplerCso, lifetimeId, handle);
        return published;
    }

    Bool MGPipeEmitShaderCsoDestroyAndFree(Uint64 lifetimeId) {
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MGPipeDeferDestroyAndFreeIfOnApplyThread(MGPipeKind::ShaderCso, lifetimeId)) {
            return false;
        }
#endif
        // ORDINARY PROGRAMS AND PIPELINE COMPOSITES TAKE THE SAME PATH, deliberately: the
        // server never learns a composite is a composite, and the only difference on this side
        // is which band the slot came out of. A composite's slot has TWO independent release
        // paths - the pipeline cache's LRU eviction and the composite ProgramObject's own
        // destructor - and the second is a proven no-op, because MGPipeSlotAllocator::Free
        // refuses a slot that is not live at that generation and bumps no generation of its
        // own (the bump rides the next handout).
        const MGPipeHandle handle =
            MGPipeSlots().FindByLifetimeId(MGPipeKind::ShaderCso, lifetimeId);
        const Bool published =
            EmitDeleteIfPublished(MGPipeKind::ShaderCso, handle, &MGPipeRouteDeleteShaderState);
        ForwardWhenWired<kMGPipeWiredProgramSubsystem>(
            MGPipeProgramEmitterInstance(), [&](auto& emitter) { emitter.NoteRecordDestroyed(handle); });
        NotifyAndFree(MGPipeKind::ShaderCso, lifetimeId, handle);
        return published;
    }

    void MGPipeSetPoisonOmission(const char* verb, const char* field) {
        if (verb == nullptr || field == nullptr) {
            g_omission = PoisonOmission{};
            return;
        }
        const auto v = MGPipeFindVerb(verb);
        const auto f = MGPipeFindInputField(field);
        if (!v || !f) BadKnob("MOBILEGL_PIPE_POISON_OMIT", verb, "unknown verb or field");
        g_omission.Armed = true;
        g_omission.Verb = *v;
        g_omission.Field = *f;
#if MOBILEGL_PIPE_POISON
        MGLOG_I("MGPipe: poison omission armed - %s@%s", field, verb);
#else
        MGLOG_W_ONCE("MGPipe: poison omission %s@%s requested but the poison is not compiled in "
                     "(MOBILEGL_PIPE_POISON=0): no stamp exists to omit",
                     field, verb);
#endif
    }

    // ---- liveness ----
    Bool PipeInputs::IsLive() const {
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith)
            return MGPipeServerContextIsLive();
#endif
        return LiveContext() != nullptr;
    }

    // ---- the seven F-class forwarders ----
    //
    // P5 (R-7.3, CONTRACT-P5.md table 2's "the seven sticky forwards"): each of them now opens
    // with MGP_STICKY_FORWARD_PULL. They are THE SEVEN THAT HAND THE SERVER A RAW FRONTEND
    // OBJECT OR WRITE INTO THE FRONTEND, and they are also the only fields the poison cannot
    // see - they carry no MGP_INPUT_CHECK at all, by the declared exception argued at
    // PipeInputs.h's F-class block, so freshness never reaches them and the exit gate was
    // structurally blind on exactly the seven most dangerous rows. The hook is a no-op outside
    // a server-stamped verb, so InvalidateCompileEnv keeps being reachable from backend
    // initialisation - the case the exemption was written for - and every monolith lane, split
    // build included, behaves as it does today.
#if MOBILEGL_BUILD_DISAGGREGATED
#define MGP_STICKY_FORWARD_PULL(Field) MGPipeStickyForwardPull(MGPipeInputField::Field)
#else
#define MGP_STICKY_FORWARD_PULL(Field) ((void)0)
#endif

    SizeT PipeInputs::GetBufferBindingPointCount(BufferTarget target) const {
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
            switch (target) {
            case BufferTarget::Uniform:
            case BufferTarget::ShaderStorage:
            case BufferTarget::AtomicCounter:
            case BufferTarget::TransformFeedback:
                return kMGPipeMaxBufferBindingPoints;
            default: return 0;
            }
        }
#endif
        MGP_STICKY_FORWARD_PULL(GetBufferBindingPointCount);
        const auto* ctx = LiveContext();
        return ctx != nullptr ? ctx->GetBufferBindingPointCount(target) : 0;
    }

    const SharedPtr<PipeInputs::ProgramObject>& PipeInputs::GetProgramObject(Uint index) {
        MGP_STICKY_FORWARD_PULL(GetProgramObject);
        auto* ctx = LiveContext();
        return ctx != nullptr ? ctx->GetProgramObject(index) : NullShared<ProgramObject>();
    }

    const SharedPtr<PipeInputs::ITextureObject>& PipeInputs::GetTextureObject(Uint index) {
        MGP_STICKY_FORWARD_PULL(GetTextureObject);
        auto* ctx = LiveContext();
        return ctx != nullptr ? ctx->GetTextureObject(index) : NullShared<ITextureObject>();
    }

    Bool PipeInputs::HasOpenTransformFeedbackSpan(Uint64 lifetimeId) const {
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
            return lifetimeId != 0 && MGPipeApplier().StreamOutputSpans.count(lifetimeId) != 0;
        }
#endif

        MGP_STICKY_FORWARD_PULL(HasOpenTransformFeedbackSpan);
        const auto* ctx = LiveContext();
        return ctx != nullptr && ctx->HasOpenTransformFeedbackSpan(lifetimeId);
    }

    void PipeInputs::InvalidateCompileEnv() {
        MGP_STICKY_FORWARD_PULL(InvalidateCompileEnv);
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
            // DELETED with an active transport (CONTRACT-P5C §4.2): R-12's caps
            // re-publication already invalidates the CLIENT's compile environment when the
            // second snapshot arrives (CapsMirror.cpp:78-80), and this forward is a write
            // into the frontend with no wire shape. A caller that still needs it under a
            // transport is a defect to fix, not a pull to serve.
            return;
        }
#endif
        if (auto* ctx = LiveContext()) ctx->InvalidateCompileEnv();
    }

    Bool PipeInputs::ValidateProgramName(Uint index) const {
        MGP_STICKY_FORWARD_PULL(ValidateProgramName);
        const auto* ctx = LiveContext();
        return ctx != nullptr && ctx->ValidateProgramName(index);
    }

    void PipeInputs::RecordError(ErrorCode code, UniquePtr<ErrorInfo> info) {
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
            // The error queue is CLIENT state and the apply thread may not write it (R4).
            // kEventGlError carries the code and the message; the client records it into its
            // own queue at the next drain point. ORDERING IS P9's (CONTRACT-P5C §4.2): the
            // event is observed at the next EmitAndWait drain, which preserves per-thread
            // program order of error-then-read but not cross-verb interleaving - the
            // accepted P5c shape, stated in the contract rather than discovered in P6.
            const String message = info != nullptr ? info->toString() : String{};
            if (gMGPipeCallbacks.OnGlError == nullptr) {
                MGLOG_F("MGPipe: Fatal{RoleViolation, \"OnGlError.callback-missing\"} - "
                        "a transport backend error has no reverse-channel owner");
                std::abort();
            }
            gMGPipeCallbacks.OnGlError(static_cast<Uint32>(code), message.c_str());
            return;
        }
#endif
        MGP_STICKY_FORWARD_PULL(RecordError);
        auto* ctx = LiveContext();
        if (ctx == nullptr) {
            MGLOG_E_ONCE("PipeInputs::RecordError: no live context, dropping error %d", static_cast<int>(code));
            return;
        }
        ctx->RecordError(code, Move(info));
    }
#undef MGP_STICKY_FORWARD_PULL

    // P3a D-H2.1. The draw's RAW vertex-fetch base instance, set immediately before the fill
    // at the three *BaseInstance draw entry points. It replaces the ambient process global
    // the backend used to read, which is a shape that cannot cross a pushed boundary; the
    // value travels as an explicit field of set_vertex_buffers and the SERVER decides
    // whether to emulate the fetch shift or let GL_EXT_base_instance do it.
    //
    // Indirect draws pass nothing: none of the three sites is in an indirect loop, per-command
    // base instances are resolved server-side out of the indirect commands, and the client
    // emits 0 for every indirect path.
    void MGPipeSetPendingBaseInstance(Uint32 baseInstance) {
        MGPipeTrackerInstance().SetPendingBaseInstance(baseInstance);
    }

    Uint32 MGPipePendingBaseInstance() { return MGPipeTrackerInstance().PendingBaseInstance(); }

    void MGPipeLeaveVerb() {
        PipeInputs& inputs = MGPipeClientInputs();
#if MOBILEGL_BUILD_DISAGGREGATED
        // P5e (ra, §3.1): a verb whose fill was skipped has no stamps of this thread's to
        // retire, and the bump below would move a serial the SERVER's stamp owns. Leave
        // returns, and the tracker's base-instance clear - which is frontend state, not block
        // state - still runs at the bottom.
        if (g_lastFillWasBarriered) {
            // Same layer-2 gate as the fill: the serial bump and the verb reset below are
            // writes into gPipeInputs (gt).
            MG_Remote::Client::ClientSession::RefusePipeInputsTouchWhileApplierOwnsIt(
                "MGPipeLeaveVerb", /*isBarrieredFill=*/true);
        }
        if (!g_lastFillWasBarriered) {
            MGPipeTrackerInstance().ClearPendingBaseInstance();
            return;
        }
#endif
#if MOBILEGL_PIPE_POISON
        // Same bump the next fill would make, without a verb to fill from: no field is
        // stamped, so every stamp this verb made falls behind the serial.
        ++MGPipeFillAccess::Filled(inputs).CurrentVerbSerial;
#endif
#if MOBILEGL_BUILD_DISAGGREGATED
        MGPipeClientClearVerbBoundary();
#endif
        MGPipeFillAccess::SetVerb(inputs, MGPipeVerb::kVerbCount);
        // The pending base instance belongs to the verb that was about to run, so leaving
        // one drops it.
        //
        // THIS IS NOT THE CLEAR PRODUCTION RELIES ON, and saying so is better than implying
        // two independent guarantees where there is one: no GL entry point calls
        // MGPipeLeaveVerb - grep finds MG_Test/ScopedPipeVerb.h and MG_Test/Pipe/TrackerTest
        // .cpp and nothing else - so what this line guarantees is that a unit case which
        // opens a ScopedPipeVerb cannot leak a base instance into the next case. The
        // production property ("consumed by exactly the verb whose entry point set it, and 0
        // at every other Update") is held by MGPipeValidateForVerb, on both of its exits.
        MGPipeTrackerInstance().ClearPendingBaseInstance();
    }


    // ================================================================================
    // The emission step (P2 brief D1 step 3, D5, D6, D7)
    // ================================================================================
    namespace {
        // Which runtime MOBILEGL_PIPE_PUSH subsystem owns a field, through the call that now
        // supplies it. Zero means "still pulled".
        constexpr Uint64 SubsystemForEmitter(MGPipeFieldEmitter emitter) {
            switch (emitter) {
            case MGPipeFieldEmitter::BindRenderState:
            case MGPipeFieldEmitter::CreateRenderState:
            case MGPipeFieldEmitter::SetDynamicState:
                return kMGPipeSubsystemRenderState;
            case MGPipeFieldEmitter::SetPatchState:
                return kMGPipeSubsystemPatchState;
            case MGPipeFieldEmitter::SetVertexAttribDefaults:
                return kMGPipeSubsystemVertexAttribDefaults;
            // P3a. bind_vertex_elements is the vertex-input family's only emitter row today
            // (Coverage.def says why the other two candidates are not there); the resource
            // family has none at all, because its calls are dispatched at the GL call that
            // causes them rather than filled into a PipeInputs field.
            case MGPipeFieldEmitter::BindVertexElements:
                return kMGPipeSubsystemVertexInput;
            // P4a's six emitted rows, across three of its four subsystems. The fourth,
            // kMGPipeSubsystemTextureResources, names NO emitted field and cannot: the texture
            // and renderbuffer resource_* calls and set_texture_params are dispatched at the
            // GL call that causes them rather than filled into a PipeInputs field, exactly as
            // P3a's buffer family is, so there is no Coverage.def emitted row for them and
            // there must not be one.
            case MGPipeFieldEmitter::SetFramebufferState:
                return kMGPipeSubsystemFramebuffer;
            case MGPipeFieldEmitter::SetSamplerViews:
            case MGPipeFieldEmitter::SetShaderImages:
                return kMGPipeSubsystemSamplers;
            case MGPipeFieldEmitter::SetDrawProgram:
            case MGPipeFieldEmitter::SetDispatchProgram:
                return kMGPipeSubsystemPrograms;
            // P5c rv (CONTRACT-P5C.md §5.3): the residual-value record rides the residual
            // subsystem - the one family with no dirty bit of its own, which is why its
            // emission gate is the subsystem bit plus the whole-record hash and nothing else.
            case MGPipeFieldEmitter::SetContextValues:
                return kMGPipeSubsystemResidualValues;
            // P5e's one emitted row (MG_Remote/CONTRACT-P5E.md §5.6): the indexed buffer
            // binding points. Written HERE at the contract commit, with the Coverage.def row
            // and EmittedCallSuppliesTheWholeField's arm beside it, for the reason
            // kMGPipeWiredSubsystems' block states one paragraph down - this file belongs to
            // the contract package for the whole phase, and an emitter enumerator whose
            // dispatch arm lived in the family's own worktree is the merge trap that block
            // exists to close. It is inert until that family's wired constant leaves 0.
            case MGPipeFieldEmitter::SetShaderBuffers:
                return kMGPipeSubsystemBufferBindings;
            case MGPipeFieldEmitter::kNone:
                break;
            }
            return 0;
        }

        // The two maps answer different questions - this one takes a field's EMITTER, the
        // tracker's MGPipeSubsystemForDirty takes a dirty BIT - and they must agree, because
        // the emission is gated on one and the residual-fill skip on the other. A divergence
        // would push a call whose field is still pulled, or (worse) skip a field whose call
        // was never emitted. Cheap to state, impossible to drift:
        static_assert(SubsystemForEmitter(MGPipeFieldEmitter::BindRenderState) ==
                          MGPipeSubsystemForDirty(MGPipeDirty::NewPipelineState),
                      "bind_render_state and NEW_PIPELINE_STATE must name one subsystem");
        static_assert(SubsystemForEmitter(MGPipeFieldEmitter::SetDynamicState) ==
                          MGPipeSubsystemForDirty(MGPipeDirty::NewRenderState),
                      "set_dynamic_state and NEW_RENDER_STATE must name one subsystem");
        // set_pixel_pack_state has no emitter row on purpose (Coverage.def, above
        // MGP_COVERAGE_EMITTED_LIST): it carries the PACK half of PipeInputs::m_pixelStore[2]
        // only, so the field keeps going through the residual fill loop and no field may be
        // skipped on its account. The NEW_PIXEL_PACK bit still names the subsystem the call
        // belongs to, which is what the emission gate consults.
        static_assert(MGPipeSubsystemForDirty(MGPipeDirty::NewPixelPack) == kMGPipeSubsystemPixelPack,
                      "NEW_PIXEL_PACK must name the pixel-pack subsystem");
        static_assert(SubsystemForEmitter(MGPipeFieldEmitter::SetPatchState) ==
                          MGPipeSubsystemForDirty(MGPipeDirty::NewPatchState),
                      "set_patch_state and NEW_PATCH_STATE must name one subsystem");
        static_assert(SubsystemForEmitter(MGPipeFieldEmitter::SetVertexAttribDefaults) ==
                          MGPipeSubsystemForDirty(MGPipeDirty::NewVertexAttribDefaults),
                      "set_vertex_attrib_defaults and NEW_VERTEX_ATTRIB_DEFAULTS must name one subsystem");

        // P3a's pairing, now stated as the SAME EQUALITY the four above are (contract-review
        // m4, closed here).
        //
        // It was written with an escape hatch - `MGPipeSubsystemForDirty(...) == 0 ||` - because
        // at the contract commit Tracker.h's bit 5 / 9 / 10 arms did not exist yet and the
        // direct form would have failed for a reason that was not a defect. That hatch was
        // explicitly conditional on the dirty half being unmapped, and the dirty half is now
        // mapped (Tracker.h:145-148), so it is removed: leaving it would mean a later edit that
        // unmapped one of these bits again passed silently, which is precisely what these
        // assertions exist to catch.
        //
        // AND ALL THREE COMPARE AGAINST SubsystemForEmitter, not against the constant. Two of
        // them named kMGPipeSubsystemVertexInput directly, which asks a different and weaker
        // question: it pins the dirty half to a constant instead of pinning the two MAPS to
        // each other, so an emitter row moved onto another subsystem would still satisfy them
        // while the emission gate and the residual-fill skip had begun to disagree. C.5's trap
        // is exactly that kind of near-miss. bind_vertex_elements is the family's only
        // Coverage.def emitter row, so it is the emitter side of all three.
        static_assert(SubsystemForEmitter(MGPipeFieldEmitter::BindVertexElements) ==
                          kMGPipeSubsystemVertexInput,
                      "bind_vertex_elements must name the vertex-input subsystem");
        static_assert(MGPipeSubsystemForDirty(MGPipeDirty::NewVertexElements) ==
                          SubsystemForEmitter(MGPipeFieldEmitter::BindVertexElements),
                      "bind_vertex_elements and NEW_VERTEX_ELEMENTS must name one subsystem");
        static_assert(MGPipeSubsystemForDirty(MGPipeDirty::NewVertexBuffers) ==
                          SubsystemForEmitter(MGPipeFieldEmitter::BindVertexElements),
                      "set_vertex_buffers and NEW_VERTEX_BUFFERS must name one subsystem");
        static_assert(MGPipeSubsystemForDirty(MGPipeDirty::NewIndexBuffer) ==
                          SubsystemForEmitter(MGPipeFieldEmitter::BindVertexElements),
                      "set_index_buffer and NEW_INDEX_BUFFER must name one subsystem");
        // The two vertex views' capacity is one number on both sides of the boundary. This is
        // the one translation unit that sees the frontend constant and the MG_Pipe one, so it
        // is where they are pinned together; MGPipeTypes.h says so in place.
        static_assert(kMGPipeMaxVertexAttribs ==
                          MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS,
                      "the MGPipe vertex-attribute capacity and the frontend's have drifted");

        // P5e (MG_Remote/CONTRACT-P5E.md §1, ruling 10), pinned here for the same reason and in
        // the same place: this translation unit sees the frontend constant and the MG_Pipe one,
        // and nothing else does. 84 is what the emitter may describe and what the wire carries;
        // the BACKEND clamps to the device's real GL_MAX_UNIFORM_BUFFER_BINDINGS, as it does
        // today, because a client-side clamp would read a device capability from the wrong side.
        static_assert(kMGPipeMaxBufferBindingPoints ==
                          MG_State::GLState::BufferBindingPointCount,
                      "the MGPipe buffer-binding-point capacity and the frontend's have drifted");

        // ---- P4a's SEVEN pairings, and EVERY ONE OF THEM COMPARES AGAINST
        // SubsystemForEmitter RATHER THAN AGAINST A CONSTANT. That is the lesson written out
        // twenty lines above and it is not a style preference: naming the subsystem constant
        // directly pins the dirty half to a constant instead of pinning the two MAPS to each
        // other, so an emitter row moved onto another subsystem would still satisfy the
        // assertion while the emission gate and the residual-fill skip had begun to disagree.
        //
        // One emitter row stands for each family: set_framebuffer_state for the framebuffer,
        // set_sampler_views for the sampler family (set_shader_images is the same subsystem
        // and is pinned to it below), and set_draw_program for the program family.
        static_assert(MGPipeSubsystemForDirty(MGPipeDirty::NewFramebuffer) ==
                          SubsystemForEmitter(MGPipeFieldEmitter::SetFramebufferState),
                      "set_framebuffer_state and NEW_FRAMEBUFFER must name one subsystem");
        static_assert(MGPipeSubsystemForDirty(MGPipeDirty::NewSamplerViews) ==
                          SubsystemForEmitter(MGPipeFieldEmitter::SetSamplerViews),
                      "set_sampler_views and NEW_SAMPLER_VIEWS must name one subsystem");
        static_assert(MGPipeSubsystemForDirty(MGPipeDirty::NewSamplers) ==
                          SubsystemForEmitter(MGPipeFieldEmitter::SetSamplerViews),
                      "bind_sampler_states and NEW_SAMPLERS must name the sampler subsystem");
        static_assert(MGPipeSubsystemForDirty(MGPipeDirty::NewShaderImages) ==
                          SubsystemForEmitter(MGPipeFieldEmitter::SetShaderImages),
                      "set_shader_images and NEW_SHADER_IMAGES must name one subsystem");
        static_assert(MGPipeSubsystemForDirty(MGPipeDirty::NewShader) ==
                          SubsystemForEmitter(MGPipeFieldEmitter::SetDrawProgram),
                      "create/bind_shader_state and NEW_SHADER must name one subsystem");
        static_assert(MGPipeSubsystemForDirty(MGPipeDirty::NewShaderBindings) ==
                          SubsystemForEmitter(MGPipeFieldEmitter::SetDrawProgram),
                      "the program family and NEW_SHADER_BINDINGS must name one subsystem");
        static_assert(MGPipeSubsystemForDirty(MGPipeDirty::NewGlobalConstants) ==
                          SubsystemForEmitter(MGPipeFieldEmitter::SetDispatchProgram),
                      "set_global_constants and NEW_GLOBAL_CONSTANTS must name one subsystem");
        // And the two program emitters really are one subsystem, which is what makes the two
        // assertions above a statement about the family rather than about one call.
        static_assert(SubsystemForEmitter(MGPipeFieldEmitter::SetDrawProgram) ==
                          SubsystemForEmitter(MGPipeFieldEmitter::SetDispatchProgram),
                      "set_draw_program and set_dispatch_program are one family and one A/B");

        // P5e's pairing, in the same shape and for the same reason as P4a's seven: the two maps
        // answer different questions - one takes a field's EMITTER, the other a dirty BIT - and
        // the emission is gated on one while the residual-fill skip is gated on the other, so a
        // divergence would push a call whose field is still pulled, or skip a field whose call
        // was never emitted. Compared against SubsystemForEmitter rather than against the
        // constant, because pinning the two MAPS to each other is the statement that cannot
        // drift.
        static_assert(MGPipeSubsystemForDirty(MGPipeDirty::NewShaderBuffers) ==
                          SubsystemForEmitter(MGPipeFieldEmitter::SetShaderBuffers),
                      "set_shader_buffers and NEW_SHADER_BUFFERS must name one subsystem");
        static_assert(MGPipeSubsystemForDirty(MGPipeDirty::NewConstBuffers) ==
                          MGPipeSubsystemForDirty(MGPipeDirty::NewShaderBuffers),
                      "the three binding-point bits are one family and one A/B");
        static_assert(MGPipeSubsystemForDirty(MGPipeDirty::NewSoTargets) ==
                          MGPipeSubsystemForDirty(MGPipeDirty::NewShaderBuffers),
                      "the three binding-point bits are one family and one A/B");

        // THE TEXTURE-RESOURCE SUBSYSTEM HAS NO DIRTY BIT, and that has to be asserted rather
        // than left as an absence: its calls are dispatched from the GL entry points that
        // cause them, so a bit that started naming it would gate the emission twice - once at
        // the dispatch site and once in the walk - and the two would disagree the first time
        // one of them was edited. Exactly the shape NoDirtyBitOwnsTheResidualSubsystem uses.
        constexpr Bool NoDirtyBitOwnsTheTextureResourceSubsystem() {
            for (SizeT i = 0; i < kMGPipeDirtyCount; ++i) {
                if (MGPipeSubsystemForDirty(static_cast<MGPipeDirty>(i)) ==
                    kMGPipeSubsystemTextureResources) {
                    return false;
                }
            }
            return true;
        }
        static_assert(NoDirtyBitOwnsTheTextureResourceSubsystem(),
                      "a MGPipeDirty bit now owns kMGPipeSubsystemTextureResources: the texture "
                      "and renderbuffer resource_* calls are dispatched at the GL call that "
                      "causes them, so a dirty bit would gate them a second time");

        // The two texture-unit capacities are one number on both sides of the boundary, and
        // this is the one translation unit that sees the frontend constant and the MG_Pipe
        // one - the same pinning kMGPipeMaxVertexAttribs gets, for the same reason.
        static_assert(kMGPipeMaxTextureUnits ==
                          static_cast<Uint32>(MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS),
                      "the MGPipe texture-unit capacity and the frontend's have drifted");
        static_assert(kMGPipeMaxImageUnits ==
                          static_cast<Uint32>(MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS),
                      "the MGPipe image-unit capacity and the frontend's have drifted");

        // Which of those subsystems THIS BUILD actually emits for. It grows one commit at a
        // time, and a field whose emitter is not wired here keeps being pulled - so adding a
        // row to Coverage.def can never silently drop a field on the floor before the call
        // that carries it exists.
        //
        // P3a's two were DELIBERATELY ABSENT at the contract commit, because the emitters
        // were stubs; each is added by the commit that gives its own emitters their bodies.
        //
        // NEITHER OF THEM RETIRES A PULL, and saying so is the point of adding them
        // deliberately rather than by reflex:
        //
        //   kMGPipeSubsystemResources names NO emitted field at all. SubsystemForEmitter
        //     above can never return it, because the resource family is dispatched at the GL
        //     call that causes it rather than filled into a PipeInputs field - there is no
        //     Coverage.def emitted row for it and there cannot be one. It is here so the
        //     constant states what this build emits for, which is what an operator reading
        //     a MOBILEGL_PIPE_PUSH value has to be able to trust.
        //
        //   kMGPipeSubsystemVertexInput names exactly one emitted field, GetBoundVertexArray
        //     through bind_vertex_elements - and EmittedCallSuppliesTheWholeField below says
        //     false for it, with the reason. So this bit switches the EMISSION on and
        //     changes nothing about the fill loop.
        //
        // THE TWO BITS ARE NOT AN INDEPENDENT A/B IN ONE DIRECTION, and an operator turning
        // them on one at a time has to know which: set_vertex_buffers and set_index_buffer
        // name their buffers by {slot, gen} whether or not the resource family created a
        // record for them, so bit 8 WITHOUT bit 7 sends the server handles it cannot resolve
        // and every one of those calls lands in RefusedResourceCalls. Bit 7 without bit 8 is
        // fine. Neither the P3a default (0x1ff, both on) nor G12's control (0x7f, both off)
        // is in that arm, which is why nothing in the phase trips over it.
        // P4a's FOUR ARE NOT WRITTEN HERE AT ALL, and that is the structural half of the
        // ownership rule rather than a stylistic choice. This file is the contract package's
        // for the entire phase: it carries Coverage.def's enum-coupled switch, the validate
        // point and the death helpers, so the packages that fill the emitters in must never
        // edit it - which is exactly the merge trap that produced a push and verify build that
        // did not compile on the integrated tree while both branches were green apart. So each
        // family's bit is the value of a constant DEFINED IN THAT FAMILY'S OWN EMIT HEADER,
        // initialised to 0 there and set to the subsystem constant by the commit that gives
        // those emitters their bodies. A mistake is then a compile error at the contract
        // commit, not at the merge, and no file is touched twice.
        //
        // The sampler bit covers SamplerEmit.h AND ImageEmit.h: one family, one A/B.
        // P5c rv: the residual subsystem joins the wired mask for set_context_values - the
        // record's emission is NOT gated on a dirty bit (there is none for the family,
        // NoDirtyBitOwnsTheResidualSubsystem says so), so this bit is what the residual-fill
        // skip consults for the eight fields the record supplies.
        constexpr Uint64 kMGPipeWiredSubsystems = kMGPipeSubsystemRenderState |
                                                  kMGPipeSubsystemPixelPack |
                                                  kMGPipeSubsystemPatchState |
                                                  kMGPipeSubsystemVertexAttribDefaults |
                                                  kMGPipeSubsystemResidualValues |
                                                  kMGPipeSubsystemResources |
                                                  kMGPipeSubsystemVertexInput |
                                                  kMGPipeWiredFramebufferSubsystem |
                                                  kMGPipeWiredTextureSubsystem |
                                                  kMGPipeWiredSamplerSubsystem |
                                                  kMGPipeWiredProgramSubsystem |
                                                  // P5e (sb): the indexed buffer binding
                                                  // points, by the same rule and from the same
                                                  // kind of header. ID-106 pins the other half
                                                  // of the switch: MG_Backend/Init.cpp's
                                                  // consumer mask gains bit 13 in the same
                                                  // commit, or R-8 withholds the whole family.
                                                  kMGPipeWiredBufferBindingSubsystem;
        // Each family constant is either 0 or its own subsystem bit and nothing else. Without
        // this a header that set the wrong constant - the sampler bit in the program header,
        // say - would switch the wrong family on and every gate would still pass.
        static_assert(kMGPipeWiredFramebufferSubsystem == 0 ||
                          kMGPipeWiredFramebufferSubsystem == kMGPipeSubsystemFramebuffer,
                      "FramebufferEmit.h's wired constant must be 0 or the framebuffer bit");
        static_assert(kMGPipeWiredTextureSubsystem == 0 ||
                          kMGPipeWiredTextureSubsystem == kMGPipeSubsystemTextureResources,
                      "TextureEmit.h's wired constant must be 0 or the texture-resource bit");
        static_assert(kMGPipeWiredSamplerSubsystem == 0 ||
                          kMGPipeWiredSamplerSubsystem == kMGPipeSubsystemSamplers,
                      "SamplerEmit.h's wired constant must be 0 or the sampler bit");
        static_assert(kMGPipeWiredProgramSubsystem == 0 ||
                          kMGPipeWiredProgramSubsystem == kMGPipeSubsystemPrograms,
                      "ProgramEmit.h's wired constant must be 0 or the program bit");
        static_assert(kMGPipeWiredBufferBindingSubsystem == 0 ||
                          kMGPipeWiredBufferBindingSubsystem == kMGPipeSubsystemBufferBindings,
                      "ShaderBufferEmit.h's wired constant must be 0 or the binding-point bit");

        // Does the record supply this exact getter's representation? P5f consumers now
        // read handle/range records directly, while the old pointer getters are FATAL on
        // the server. A handle is still not a SharedPtr/client table base, so these false
        // arms remain: otherwise the ownership generator would misclassify the retired
        // getters as RECORD_SUPPLIED. Monolith still fills its original pointer mirrors.
        // PixelStoreParameters has two halves and only PACK is supplied.
        constexpr Bool EmittedCallSuppliesTheWholeField(MGPipeInputField field) {
            switch (field) {
            case MGPipeInputField::GetPixelStoreParameters:
            case MGPipeInputField::GetBoundVertexArray:
            case MGPipeInputField::GetFramebufferBindingSlot:
            case MGPipeInputField::GetImageTextureBinding:
            case MGPipeInputField::GetTextureUnitObject:
            case MGPipeInputField::GetProgramForDraw:
            case MGPipeInputField::GetProgramForDispatch:
            // Indexed binding points also have a different representation: ranges on
            // the server, raw client table bases in the legacy getter.
            case MGPipeInputField::GetBufferBindingPoint:
                return false;
            default:
                return true;
            }
        }

        // The fields the applier writes DIRECTLY, out of the chunk bytes it scattered. Every
        // other emitted field reaches PipeInputs only through
        // MGPipeDeriveRenderStateFields, which is why the probe below exists.
        constexpr Bool AppliedWithoutDerivation(MGPipeInputField field) {
            switch (field) {
            case MGPipeInputField::GetRenderStateParameters:
            case MGPipeInputField::GetRenderStateParametersVersion:
            case MGPipeInputField::GetPipelineStateVersion:
            case MGPipeInputField::GetPixelStoreParameters:
            case MGPipeInputField::GetPatchVertices:
            case MGPipeInputField::GetPatchDefaultOuterLevel:
            case MGPipeInputField::GetPatchDefaultInnerLevel:
            case MGPipeInputField::GetCurrentVertexAttribute:
            // P5c rv's eight: MGPipeApplySetContextValues writes them out of the record,
            // field for field, through MGPipeApplyAccess::SetContextValues.
            case MGPipeInputField::GetActiveTextureUnit:
            case MGPipeInputField::GetMaxTouchedTextureUnit:
            case MGPipeInputField::GetTouchedBufferBindingPointCount:
            case MGPipeInputField::IsTransformFeedbackActive:
            case MGPipeInputField::IsTransformFeedbackPaused:
            case MGPipeInputField::GetTransformFeedbackGeneration:
            case MGPipeInputField::GetBoundTransformFeedbackLifetimeId:
            case MGPipeInputField::GetTransformFeedbackCapturedVertices:
                return true;
            default:
                return false;
            }
        }

        // DOES THIS TREE'S APPLIER ACTUALLY DERIVE?
        //
        // MGPipeDeriveRenderStateFields is package A's, and on the P2 contract tag it is a
        // declared stub whose body lands in A's follow-on commit. A field that reaches
        // PipeInputs only through that derivation must NOT be skipped by the residual fill
        // while the derivation is a stub: skipping it would leave the mirror unwritten and
        // the backend reading a default.
        //
        // Rather than hard-code which branch this is, the filler asks once: it puts a
        // sentinel in a scratch block's working RenderStateParameters, clears the mirror the
        // derivation is supposed to recompute, runs the derivation, and looks. The answer is
        // latched for the process and costs one compare, once.
        //
        // It stays useful after A lands: if the derivation is ever deleted or gated off, the
        // filler degrades to PULLING those fields instead of rendering a default, which is
        // the safe direction. The verify lane and RenderStateSpansTest are what say the
        // derivation is CORRECT; this only says it is THERE.
        //
        // AND IT IS A ONE-FIELD SAMPLE, deliberately: it probes m_clearStencil and nothing
        // else, so a PARTIAL derivation - one that recomputes m_clearStencil and forgets, say,
        // GetViewport's rounding - flips this latch to true and lets the other mirrors go
        // unwritten. That is a real risk of a half-landed package A and the backstop for it is
        // the verify lane (which re-reads every field at every backend read), not this probe.
        // Widening the probe to all 29 would re-implement the derivation to check it.
        Bool ApplierDerivesRenderStateFields() {
            static const Bool answer = [] {
                // Leak-at-exit, for gPipeInputs' reason: a PipeInputs is never destroyed by
                // an exit handler. This one only ever carries render state, but the rule is
                // stated over the TYPE rather than over each instance's current contents -
                // an instance that grows an O-class write later must not become the next
                // exit-time chain starter.
                static PipeInputs& probe = *new PipeInputs();
                constexpr Uint32 kSentinel = 0x5a5a5a5au;
                MGPipeFillAccess::RenderStateOf(probe).ClearStencil = kSentinel;
                MGPipeFillAccess::ClearStencilOf(probe) = 0u;
                MGPipeDeriveRenderStateFields(probe);
                const Bool derives = MGPipeFillAccess::ClearStencilOf(probe) == kSentinel;
                if (!derives) {
                    MGLOG_W_ONCE("MGPipe: MGPipeDeriveRenderStateFields does not derive on this "
                                 "build - the render-state mirrors stay on the pull path");
                }
                return derives;
            }();
            return answer;
        }

        // ---- the residual fill's SUPPLIED SET, computed once per environment (P5d r3, C) ----
        //
        // WHY THIS CACHE EXISTS, AND WHAT IT DOES NOT CHANGE. Step 4 of the validate point asks,
        // for every one of the 63 fields and at EVERY verb, whether an emitted call already
        // supplied it. The question is the seven-term conjunction spelled below - and it is still
        // spelled exactly once, so there is ONE copy of it: five of its terms are constants of the
        // FIELD, and the other two are facts about the PROCESS - which subsystems the operator's
        // mask carries, and whether the backend consumes the P4a families - that move a handful of
        // times in a process's life and never inside a verb. So the conjunction is evaluated per
        // field once per distinct environment and read back as a bit. Nothing about WHICH fields
        // are copied moves: same expression, same inputs, same answer.
        //
        // AND THE PROFILE IS WHY IT IS WORTH A CACHE AT ALL. The 2026-09-17 inproc profile
        // (Minecraft 26.3-rc-3, view distance 12, ~852 draws/frame) put MGPipeValidateForVerb at
        // 4.11% self / 10.9% inclusive of the GL thread, of which MGPipeTracker::Update is 1.46
        // and CopyField 2.54; most of the rest is this walk's per-field predicate.
        // P4aFamilyHasItsConsumer alone is a CapsMirror read under split, and it was taken 63
        // times per verb for an answer that is the same 63 times.
        //
        // THE KEY IS THE WHOLE OF WHAT THE EXPRESSION READS BESIDE THE FIELD ID, which is what
        // makes this a memo rather than a latch that goes stale:
        //   - the push mask (MG_Config::Features.PipePush), which the per-subsystem A/B lanes move;
        //   - ApplierDerivesRenderStateFields(), a one-shot probe - in the key anyway, so that a
        //     build where it ever stopped being one-shot cannot keep a stale answer silently;
        //   - contextValuesWireLive, which flips when a session starts, stops, or tears its tables
        //     down (P5c rv, CONTRACT-P5C.md 5.3) - the half that must never disagree with the
        //     emission's half at the validate point, which is why it is PASSED IN rather than
        //     re-read here;
        //   - P4aFamilyHasItsConsumer, which flips when the caps mirror adopts a snapshot (R-12: a
        //     second arrival IS the invalidation) or a backend registers its resource op table.
        //     ALL FOUR FAMILIES RIDE THE ONE SIGNAL - that is P4aFamilyHasItsConsumer's own rule,
        //     argued where it is defined - so asking it once for the whole family mask is asking
        //     it for every family at once, and the rebuild re-asks it per subsystem from the same
        //     unchanged mirror. It is also the one key input that CANNOT MOVE AN ANSWER TODAY,
        //     and the static_assert below is that fact's trip wire rather than a claim in prose.
        //
        // NOT THREAD-LOCAL, AND THAT IS THIS FILE'S EXISTING RULE RATHER THAN A NEW ONE:
        // g_residualDue, g_omission and the verify latches beside it are file-scope too, and what
        // keeps a single writer on them is the verb barrier (ROADMAP.md's G1 row states it for
        // gPipeInputs itself). A validate that runs on the apply thread is inside that barrier by
        // construction.
        //
        // BUT THE ANSWER IS HANDED OUT BY VALUE, WHICH IS NOT THE SAME ARGUMENT (P5d r3 package C
        // review, minor 1). Those latches are single BITS: a reader cannot observe one
        // half-written. A 2x64-bit mask can, so the rebuild below fills a LOCAL and publishes it
        // into the memo in one assignment, and the caller gets a copy rather than a reference into
        // storage the next rebuild would zero under it. Sixteen bytes is cheaper at the use site
        // than the indirection was anyway - the walk reads the copy 63 times.

        // AND THE FOURTH KEY INPUT CANNOT MOVE AN ANSWER TODAY, WHICH IS A FACT WITH A TRIP WIRE
        // RATHER THAN A COMMENT (P5d r3 package C review, the major).
        //
        // The consumer conjunct sits in the expression because the expression reads it, and the
        // key carries it because a key that omits an input the expression reads is how a memo
        // goes stale. But every field whose emitter belongs to a P4a family - the five of them:
        // GetFramebufferBindingSlot, GetImageTextureBinding, GetTextureUnitObject,
        // GetProgramForDraw, GetProgramForDispatch - is ALSO a field
        // EmittedCallSuppliesTheWholeField() answers `false` for: their storage
        // is a frontend heap reference (a BindingSlot, an ImageTextureBinding, a TextureUnit,
        // two SharedPtr<ProgramObject>) that a payload cannot carry, so the fill pulls them
        // whatever the consumer says. The consumer conjunct is therefore DOMINATED: no motion of
        // the caps mirror or of MGPipeGetResourceOps() can change one bit of the mask, and no
        // unit case can distinguish a key that carries it from one that does not.
        //
        // WHEN THIS FIRES, that stopped being true - P3b/P4b/P7/P8 gave one of those rows a twin
        // the applier can write - and the consumer signal became observable through
        // MGPipeResidualFillSuppliesField. At that point the memo's key needs the same
        // move-it-and-read-it-back pair steps 1-3 of
        // FieldOwnershipTest.TheResidualFillsSuppliedMemoReKeysOnEveryInputThatMovesAnAnswer
        // give the other three inputs, and that case's step 4 (which today pins the domination
        // instead) has to become it. Do that rather than deleting this line.
        constexpr Bool NoP4aFamilyFieldIsWhollySupplied() {
            for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
                const auto field = static_cast<MGPipeInputField>(i);
                const Uint64 subsystem = SubsystemForEmitter(kMGPipeFieldEmittedBy[i]);
                if ((subsystem & kMGPipeP4aFamilySubsystems) != 0 &&
                    EmittedCallSuppliesTheWholeField(field)) {
                    return false;
                }
            }
            return true;
        }
        static_assert(NoP4aFamilyFieldIsWhollySupplied(),
                      "a P4a-family field became whole-supplied: the residual fill's supplied-set "
                      "memo can now answer differently for the consumer signal, so that key input "
                      "needs the unit case the paragraph above names");

        // ---- P5e (gl), ID-112: THE SAME TRIP WIRE FOR THE TWO ROWS THAT ONE CANNOT SEE ------
        //
        // The assertion above is keyed on the EMITTER'S SUBSYSTEM, so it covers exactly the five
        // P4a-family rows. GetBoundVertexArray (P3a's, emitted by BindVertexElements) and
        // GetBufferBindingPoint (P5e sb's, emitted by SetShaderBuffers) sit outside
        // kMGPipeP4aFamilySubsystems and had therefore no compile-time protection at all.
        //
        // WHY THAT IS A DEVICE CRASH AND NOT A STYLE POINT. This residual fill is MAGMA'S ONLY
        // SOURCE for all seven pointer-backed rows, and Magma dereferences them on its first
        // draw: it is in lockstep for the whole of P5e (ID-90) and reads them through
        // MagmaP7AllocatorDebtScope. A package retiring a DirectGLES consumer that "tidied up"
        // by deleting one of these two rows from EmittedCallSuppliesTheWholeField would not
        // break the build - it would SIGSEGV Magma on a phone, which is the exact shape that
        // cost this phase 38 scenarios once already (ID-107). So the seven are NAMED, and the
        // naming is the deliverable: it converts the most likely mistake of every remaining
        // package from a device crash into a build break.
        //
        // IF THIS ASSERTION FIRED ON YOU: the answer for each row is argued above
        // EmittedCallSuppliesTheWholeField and it is the same argument every time - the field's
        // storage is a frontend heap reference and no payload may carry a pointer. What retires
        // a row is the phase where the BACKEND stops reading a frontend object (P7 for the
        // texture/framebuffer/program mirrors, P8 for the VAO), never a consumer-side cleanup
        // in the package you are writing.
        constexpr MGPipeInputField kMGPipePointerBackedResidualRows[] = {
            MGPipeInputField::GetBoundVertexArray,       MGPipeInputField::GetBufferBindingPoint,
            MGPipeInputField::GetFramebufferBindingSlot, MGPipeInputField::GetImageTextureBinding,
            MGPipeInputField::GetTextureUnitObject,      MGPipeInputField::GetProgramForDraw,
            MGPipeInputField::GetProgramForDispatch,
        };
        static_assert(sizeof(kMGPipePointerBackedResidualRows) / sizeof(MGPipeInputField) == 7,
                      "ID-112 names SEVEN pointer-backed residual rows; this list is the whole of "
                      "them and a row removed from it is a row with no trip wire");
        constexpr Bool NoPointerBackedRowIsWhollySupplied() {
            for (const MGPipeInputField field : kMGPipePointerBackedResidualRows) {
                if (EmittedCallSuppliesTheWholeField(field)) return false;
            }
            return true;
        }
        static_assert(NoPointerBackedRowIsWhollySupplied(),
                      "a pointer-backed residual row stopped being pulled (ID-112). This fill is "
                      "Magma's ONLY source for GetBoundVertexArray, GetBufferBindingPoint and the "
                      "five P4a-family mirrors, and Magma dereferences them on its first draw - so "
                      "this is a build break standing in for a device SIGSEGV. Retire the row in "
                      "the phase that stops the backend reading a frontend object (P7/P8)");

        struct ResidualFillPlan {
            Bool Valid = false;
            Uint64 PushMask = 0;
            Bool ApplierDerives = false;
            Bool ContextValuesWireLive = false;
            Bool P4aConsumer = false;
#if MOBILEGL_BUILD_DISAGGREGATED
            Uint64 CapsGeneration = 0;
#endif
            // One bit per FIELD - not per verb class. The class mask is applied at the walk
            // exactly as it always was, so "does this verb read this field" stays the walk's
            // business and this stays a statement about EMISSION alone.
            MGPipeFieldMask Supplied{};
        };
        ResidualFillPlan g_fillPlan;

        MGPipeFieldMask SuppliedFieldMask(Uint64 pushMask, Bool applierDerives,
                                          Bool contextValuesWireLive) {
            // THE CONSUMER SIGNAL IS READ ONLY WHERE THE WALK BELOW COULD REACH IT, and that
            // guard is not a micro-optimisation - it is what keeps R-8's DIAGNOSTIC COUNTERS
            // where they were (P5d r3 package C review, minor 2). In the conjunction below
            // P4aFamilyHasItsConsumer sits AFTER `(pushMask & subsystem) != 0`, and it answers a
            // constant `true` for every subsystem outside the P4a families - so at a mask that
            // carries no P4a bit NO field reaches the caps mirror at all. Asking it here anyway
            // would have moved CapsMirror::ServerConsumes' refusal count and its one-shot
            // MGLOG_W at exactly the hand-picked A/B lanes (and the bring-up window, CallMask 0)
            // where the baseline never touched the mirror, which is the opposite direction from
            // the one this change is supposed to move them in. The key stays COMPLETE because
            // pushMask is itself in the key: a mask that gains a P4a bit re-keys on the mask.
            const Bool p4aConsumer = (pushMask & kMGPipeP4aFamilySubsystems) != 0 &&
                                     P4aFamilyHasItsConsumer(kMGPipeP4aFamilySubsystems);
            if (g_fillPlan.Valid && g_fillPlan.PushMask == pushMask &&
                g_fillPlan.ApplierDerives == applierDerives &&
                g_fillPlan.ContextValuesWireLive == contextValuesWireLive &&
                g_fillPlan.P4aConsumer == p4aConsumer
#if MOBILEGL_BUILD_DISAGGREGATED
                && g_fillPlan.CapsGeneration == (MG_Config::Transport != MG_Config::TransportMode::Monolith ?
                    MG_Remote::Client::CapsMirrorInstance().Generation() : 0)
#endif
                ) {
                return g_fillPlan.Supplied;
            }
            MGPipeFieldMask built{};
            for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
                const auto field = static_cast<MGPipeInputField>(i);
                // A field a P2 call now supplies is not pulled again - that second pull is exactly
                // the cost P2 exists to remove. THE STAMP IS UNCHANGED either way: a stamp says
                // "this verb published this field", which is as true of an emitted field as of a
                // copied one, and withholding it would abort every backend read of the very fields
                // the migration just took over. The stamp is still written at the walk; only the
                // QUESTION moved up here.
                const MGPipeFieldEmitter emitter = kMGPipeFieldEmittedBy[i];
                const Uint64 subsystem = SubsystemForEmitter(emitter);
                // P4aFamilyHasItsConsumer and P4aFamilyDependenciesAreSet are in this conjunction
                // for the reason they are in `wants()`: "supplied" means A CALL WENT OUT CARRYING
                // THIS FIELD, and on a backend with no consumer - or at a mask that leaves one of
                // the family's D-K2 dependency bits clear - no P4a call went out at all, so
                // withholding the pull here would leave the field unfilled at the very verb that
                // reads it.
                //
                // AND THE LAST CONJUNCT IS P5c rv's (CONTRACT-P5C.md 5.3): set_context_values has
                // NO PRODUCER without a live wire (its emission is transport-gated at the validate
                // point), so its eight fields keep being pulled under monolith - G1's byte-for-byte
                // rule - and are skipped only when the record really crosses.
                const Bool supplied = subsystem != 0 && (subsystem & kMGPipeWiredSubsystems) != 0 &&
                                      (pushMask & subsystem) != 0 &&
                                      P4aFamilyHasItsConsumer(subsystem) &&
                                      P4aFamilyDependenciesAreSet(subsystem, pushMask) &&
                                      EmittedCallSuppliesTheWholeField(field) &&
                                      (applierDerives || AppliedWithoutDerivation(field)) &&
                                      (emitter != MGPipeFieldEmitter::SetContextValues ||
                                       contextValuesWireLive);
                if (supplied) built.Words[i / 64] |= (Uint64{1} << (i % 64));
            }
            // PUBLISHED IN ONE ASSIGNMENT, after the walk - see the paragraph above the struct.
            g_fillPlan.Valid = true;
            g_fillPlan.PushMask = pushMask;
            g_fillPlan.ApplierDerives = applierDerives;
            g_fillPlan.ContextValuesWireLive = contextValuesWireLive;
            g_fillPlan.P4aConsumer = p4aConsumer;
#if MOBILEGL_BUILD_DISAGGREGATED
            g_fillPlan.CapsGeneration = MG_Config::Transport != MG_Config::TransportMode::Monolith ?
                MG_Remote::Client::CapsMirrorInstance().Generation() : 0;
#endif
            g_fillPlan.Supplied = built;
            return built;
        }

        // set_pixel_pack_state. PACK only, deliberately: nothing on the far side of the
        // boundary reads unpack state, and the staged-repack upload path does not even issue
        // glPixelStorei (ARCHITECTURE.md 4.6 D5).
        Uint64 EmitPixelPackState(GLContext& ctx) {
            MGPPixelPackState pack{};
            pack.Pack = ctx.GetPixelStoreParameters(false);
            MGPipeRouteSetPixelPackState(pack);
            return sizeof(MGPPixelPackState);
        }

        // set_patch_state. The trio ALSO travels in pipeline chunk P0, and that redundancy is
        // a trip wire rather than waste: the applier asserts under verify that the two
        // carriers agree. 28 bytes on a state that changes about once per program.
        Uint64 EmitPatchState(GLContext& ctx) {
            const RenderStateParameters& live = ctx.GetRenderStateParameters();
            MGPPatchState patch{};
            patch.Vertices = live.PatchVertices;
            for (SizeT i = 0; i < 4; ++i) patch.Outer[i] = live.PatchDefaultOuterLevel[i];
            for (SizeT i = 0; i < 2; ++i) patch.Inner[i] = live.PatchDefaultInnerLevel[i];
            MGPipeRouteSetPatchState(patch);
            return sizeof(MGPPatchState);
        }

        // set_vertex_attrib_defaults, behind D11's set-hash suppressor: the RESOLVED set - all
        // 32 values, all three views - is hashed on the client and the call does not go out
        // when the hash has not moved. That is coalescing rule 4, and this is its one wired
        // consumer in P2.
        //
        // THE PAYLOAD, SINCE P5c rv (CONTRACT-P5C.md §5.3). A CurrentVertexAttributeValue is
        // one value in three views, and GLContext CONVERTS between them numerically, so "the
        // bytes of one view" is not the value: glVertexAttrib4f(loc, 1.5f, ...) leaves 1 in
        // intValue and 0x3FC00000 in floatValue, and every glVertexAttribI4i/ui is a different
        // pair again. MGPAttribValue now carries all three views VERBATIM
        // (FloatView/IntView/UintView) plus the class the frontend actually wrote
        // (GLContext::GetCurrentVertexAttributeClass), and MGPipeApplySetVertexAttribDefaults
        // writes each view from its own array - the cross-view conversion's authoritative
        // answer is the client's, and the applier no longer reconverts anything. The pre-rv
        // shape (one Data[4] memcpied into all three views, ValueClass ignored) is exactly
        // what kept this row EMITTED-AND-STILL-PULLED in EmittedCallSuppliesTheWholeField;
        // with the applier fixed, the field is RECORD_SUPPLIED outright.
        //
        // The suppressing memcmp below is over the three VIEWS only, and that is not an
        // oversight: the class decides how the views are REBUILT, so two writes that leave
        // the three views identical rebuild identically whichever class they carried, and a
        // class that moved without moving any view has nothing to publish.
        //
        // So the emitter CHECKS rather than assumes, the same self-healing shape as
        // ApplierDerivesRenderStateFields: after the call it compares the mirror the applier
        // wrote against the frontend's value, and when they differ it copies the field itself
        // and says so once. That is what keeps the block correct in the window this call used
        // to corrupt - a glVertexAttrib4f followed by a non-kDraw verb, where the residual
        // fill does not run for this field and nothing else would have put the value back.
        // Since rv the applier writes all three views verbatim, so the compare below is
        // expected to pass on every emission; it stays armed because it is the one observable
        // of that write being verbatim in a window no other gate looks at.
        Uint64 g_attribDefaultRepairs = 0;

        // The header of the last set_vertex_attrib_defaults that actually went out. Count == 0
        // means none ever did, because a call that names no attribute is not emitted at all.
        // It is the observable for the two things about this call that cannot be read back
        // without a poisoned read of m_currentVertexAttribute: that a fresh context republishes
        // the COMPLETE set, and that a single moved attribute publishes exactly that one.
        MGPVertexAttribDefaults g_attribDefaultLastHeader{};

        Uint64 EmitVertexAttribDefaults(GLContext& ctx, Bool freshlyPrimed) {
            MGPipeTracker& tracker = MGPipeTrackerInstance();
            auto& staged = tracker.StagedAttribDefaults();
            constexpr SizeT kAttribs = MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS;
            static_assert(kAttribs <= 32, "MGPVertexAttribDefaults::Mask is a Uint32");

            Array<MG_State::GLState::CurrentVertexAttributeValue, kAttribs> resolved;
            for (SizeT i = 0; i < kAttribs; ++i) resolved[i] = ctx.GetCurrentVertexAttribute(static_cast<Uint>(i));

            const Uint64 contentHash = XXH64(resolved.data(), sizeof(resolved), 0);
            if (!MGPipeSetHashSuppressorInstance().ShouldEmit(MGPipeSuppressorSlot::SetVertexAttribDefaults,
                                                             contentHash)) {
                return 0;
            }

            // A FRESH CONTEXT PUBLISHES ALL 32, not the difference against a mirror that
            // describes a context that is gone. Tracker::Reset() sets the staging mirror to
            // AttribDefaults{}, whose NSDMIs are the GL defaults {0,0,0,1} - and a fresh
            // GLContext's m_currentVertexAttributes hold exactly those, so the diff below is
            // EMPTY on the one walk that must publish everything. The server's mirror is not
            // default: MGPipeApplierReset() clears the CSO store and the residual block and
            // leaves gPipeInputs.m_currentVertexAttribute holding the PREVIOUS context's
            // defaults. So the InvalidateAll() a fresh context does to the set-hash
            // suppressor would have been cancelled two lines later by this diff, and the one
            // call P2 fully owns would publish nothing across a context change - exactly the
            // "memo that serves a stale answer" the tracker's own COMPLETE-state rule
            // (Tracker.h) exists to forbid. EmitRenderState has the same arm
            // (freshlyPrimed ? kAllDynamicChunks) and the other two calls send whole values.
            Array<MGPAttribValue, kAttribs> tail{};
            MGPVertexAttribDefaults header{};
            for (SizeT i = 0; i < kAttribs; ++i) {
                if (!freshlyPrimed && std::memcmp(&resolved[i], &staged[i], sizeof(resolved[i])) == 0) {
                    continue;
                }
                // The class the frontend WROTE, and that class's own bytes. Not a literal 0
                // and not ClassifyVertexAttribType's answer: that one is the SHADER's question
                // ("which view does this input consume"), asked at the backend read sites, and
                // it says nothing about which view holds the value the other two were
                // converted from.
                MGPipeFillAttribValue(static_cast<Uint32>(i), resolved[i],
                                      ctx.GetCurrentVertexAttributeClass(static_cast<Uint>(i)),
                                      tail[header.Count]);
                header.Mask |= Uint32{1} << static_cast<Uint32>(i);
                ++header.Count;
                staged[i] = resolved[i];
            }
            if (header.Count == 0) return 0;
            g_attribDefaultLastHeader = header;
            MGPipeRouteSetVertexAttribDefaults(header, tail.data());

            // Did the applier reproduce it? Byte for byte, over the attributes this call
            // named - anything less would be a mirror that disagrees with the frontend in a
            // window no gate looks at.
            //
            // P5e (ra, CONTRACT-P5E §3.4): NOT UNDER RUN-AHEAD. The mirror is the APPLIER's
            // copy of this record, and set_vertex_attrib_defaults is a kWaitNone row - so
            // under run-ahead this thread has not waited for the apply and the read races it,
            // and the repair below (a CopyField straight into the block) is precisely the
            // GL-thread write §3.5 refuses. The client's own authority is `resolved` and
            // `g_attribDefaultLastHeader`, which is what it just published; there is nothing
            // the mirror could add that the wire does not already carry. A build that ever
            // needs the repair arm again has to earn it with a barriered row.
#if MOBILEGL_BUILD_DISAGGREGATED
            if (ClientRunsAhead()) return sizeof(MGPVertexAttribDefaults) + header.Count * sizeof(MGPAttribValue);
#endif
            const auto* mirror = MGPipeFillAccess::VertexAttribDefaultsOf(MGPipeClientInputs());
            Bool reproduced = true;
            for (SizeT i = 0; i < kAttribs && reproduced; ++i) {
                if ((header.Mask & (Uint32{1} << static_cast<Uint32>(i))) == 0) continue;
                reproduced = std::memcmp(&mirror[i], &resolved[i], sizeof(resolved[i])) == 0;
            }
            if (!reproduced) {
                ++g_attribDefaultRepairs;
                MGLOG_W_ONCE("MGPipe: MGPipeApplySetVertexAttribDefaults did not reproduce the "
                             "carried three views on this build - the client is keeping "
                             "m_currentVertexAttribute authoritative");
                MGPipeFillAccess::CopyField(MGPipeClientInputs(), ctx, MGPipeInputField::GetCurrentVertexAttribute);
            }
            return sizeof(MGPVertexAttribDefaults) + header.Count * sizeof(MGPAttribValue);
        }

#if MOBILEGL_BUILD_DISAGGREGATED
        // set_context_values (P5c rv, CONTRACT-P5C.md §5.3): the residual-value record. One
        // POD carrying every value-class field no other set_* supplies - the two texture-unit
        // counters, the 15 per-target touched-buffer-binding counts and the five XFB values -
        // emitted at validate WHEN ANY COVERED VALUE MOVED, which the whole-record hash says:
        // there is deliberately no dirty bit for the family (the tracker's value-class dirty
        // accounting is untouched - "零新增记账", ARCHITECTURE.md 5.2) and no dirty mask in the
        // payload, so a suppressed record means "nothing moved", never "field invalid" (§1).
        //
        // THE PRODUCER IS TRANSPORT-GATED, and the residual-fill skip for the same eight fields
        // is gated on the same answer (MGPipeValidateForVerb's contextValuesWireLive): under
        // monolith - or with a transport configured but no live session (the bring-up window, a
        // server-role-only fixture) - nothing is emitted and the fields keep being pulled, byte
        // for byte as before (G1).
        Uint64 EmitContextValues(GLContext& ctx) {
            MGPContextValues values{};
            values.ActiveTextureUnit = static_cast<Uint32>(ctx.GetActiveTextureUnit());
            values.MaxTouchedTextureUnit = static_cast<Uint32>(ctx.GetMaxTouchedTextureUnit());
            // The array is indexed by BufferTarget value, all 15 of them (MGPipeTypes.h);
            // targets with no binding points answer 0 (BufferState::GetTouchedBindPointCount).
            static_assert(
                std::extent_v<decltype(MGPContextValues::TouchedBufferBindingPointCount)> ==
                    static_cast<SizeT>(BufferTarget::BufferTargetCount),
                "MGPContextValues' per-target array and BufferTargetCount have drifted");
            for (Uint32 t = 0; t < static_cast<Uint32>(BufferTarget::BufferTargetCount); ++t) {
                values.TouchedBufferBindingPointCount[t] =
                    static_cast<Uint32>(ctx.GetTouchedBufferBindingPointCount(static_cast<BufferTarget>(t)));
            }
            values.IsTransformFeedbackActive = ctx.IsTransformFeedbackActive() ? 1 : 0;
            values.IsTransformFeedbackPaused = ctx.IsTransformFeedbackPaused() ? 1 : 0;
            values.TransformFeedbackGeneration = ctx.GetTransformFeedbackGeneration();
            values.BoundTransformFeedbackLifetimeId = ctx.GetBoundTransformFeedbackLifetimeId();
            values.TransformFeedbackCapturedVertices = ctx.GetTransformFeedbackCapturedVertices();
            // The whole record is the hash input, padding included - `values{}` zeroes it, so
            // the pad bytes are defined and the hash is stable.
            const Uint64 contentHash = XXH64(&values, sizeof(values), 0);
            if (!MGPipeSetHashSuppressorInstance().ShouldEmit(MGPipeSuppressorSlot::SetContextValues,
                                                             contentHash)) {
                return 0;
            }
            MGPipeRouteSetContextValues(values);
            return sizeof(MGPContextValues);
        }

        // The fields set_context_values supplies. A validate whose verb class reads NONE of
        // them skips the record build entirely - the record exists so a verb's reads are
        // answered, and a verb that never reads them needs no publication.
        constexpr Bool VerbMaskReadsContextValues(const MGPipeFieldMask& mask) {
            return MGPipeFieldMaskHas(mask, MGPipeInputField::GetActiveTextureUnit) ||
                   MGPipeFieldMaskHas(mask, MGPipeInputField::GetMaxTouchedTextureUnit) ||
                   MGPipeFieldMaskHas(mask, MGPipeInputField::GetTouchedBufferBindingPointCount) ||
                   MGPipeFieldMaskHas(mask, MGPipeInputField::IsTransformFeedbackActive) ||
                   MGPipeFieldMaskHas(mask, MGPipeInputField::IsTransformFeedbackPaused) ||
                   MGPipeFieldMaskHas(mask, MGPipeInputField::GetTransformFeedbackGeneration) ||
                   MGPipeFieldMaskHas(mask, MGPipeInputField::GetBoundTransformFeedbackLifetimeId) ||
                   MGPipeFieldMaskHas(mask, MGPipeInputField::GetTransformFeedbackCapturedVertices);
        }
#endif


        // set_residual_value_state (P2 brief D9, ARCHITECTURE.md 9.4).
        //
        // Since P2 the block is one Uint64 of capability bits, and every one of the 35 is
        // ALSO answerable from the assembled working block now that the contract closed the
        // FramebufferSrgb / DepthClamp / TextureCubeMapSeamless storage holes. That
        // redundancy is the whole point: the bits are read HERE from the frontend, and the
        // applier compares them against the assembled answer, so the day a later call takes
        // a capability over and forgets to carry it the block says so on the next draw
        // (Fatal{PipeResidualDiverged, "<Cap>"}).
        //
        // Building the carried bits from the ASSEMBLED block instead would make the trip
        // wire a tautology, which is exactly the failure P1's entry compare had and P2 is
        // paying to remove.
        //
        // ON THIS BRANCH IT IS STILL HALF A TAUTOLOGY, and saying so is part of the honesty
        // the trip wire is for: the applier compares these bits against gPipeInputs'
        // capability mirror, and while MGPipeDeriveRenderStateFields is a stub that mirror is
        // filled by the residual fill from the SAME IsCapabilityEnabled accessor a few lines
        // below. It becomes an independent oracle the moment package A's c1 lands and the
        // fill stops copying those fields. What it proves already is that the block is
        // emitted, sized and suppressed - the resid= byte class and the one divergence it
        // caught during development (GL_DITHER) are that evidence.
        //
        // Emitted once per context and again whenever the capability set may have moved
        // (D9). THE SHUTTER FOR THAT IS NEW_RENDER_STATE, NOT NEW_PIPELINE_STATE, and the
        // difference is a hole rather than a nicety: SetCapability's ClipDistance0..7 arms
        // are deliberately NOT BumpVersions() (RenderState.cpp says so in as many words), so
        // glEnable(GL_CLIP_DISTANCE0) moves m_version alone - and ClipDistance0..7 are 8 of
        // the 35 CapabilityInputs this block carries. Arming on the pipeline version would
        // leave the trip wire disarmed for those eight for an unbounded window, which is the
        // under-firing direction ARCHITECTURE.md 13.2 names as the dangerous one, and no gate
        // could see it: a block that is never emitted cannot diverge.
        //
        // So the arming is the coarsest always-true shutter - either render-state counter
        // moved - which is the same answer DirtySurface.def's derivation gives SetCapability.
        // It over-fires (a glViewport re-sends 8 bytes and re-runs the compare) and that is
        // the intended trade: over-firing costs one 35-bit loop on a verb that already moved
        // render state, under-firing renders stale.
        Uint64 EmitResidualValueState(GLContext& ctx) {
            ResidualValueBlock block{};
            constexpr SizeT kCapabilityCount = static_cast<SizeT>(CapabilityInput::CapabilityInputCount);
            static_assert(kCapabilityCount <= 64, "CapabilityBits is a Uint64");
            for (SizeT i = 0; i < kCapabilityCount; ++i) {
                if (ctx.IsCapabilityEnabled(static_cast<CapabilityInput>(i))) {
                    block.CapabilityBits |= Uint64{1} << i;
                }
            }
            MGPipeRouteSetResidualValueState(block);
            if (MG_Util::PipeStats::Enabled()) {
                // ByteClass::ResidualValueBlock has been a placeholder that "stays at 0
                // until P2" since P0. This is what makes it non-zero.
                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::ResidualValueBlock,
                                             sizeof(ResidualValueBlock));
            }
            return sizeof(MGPResidualValueState) + sizeof(ResidualValueBlock);
        }

        // Set when the capability set may have moved, cleared when the block goes out. It is
        // not part of the tracker because it is emission state, not a shutter: the shutter
        // (the render-state counter) has already been consumed by the time this is read.
        Bool g_residualDue = true;

        // The residual block is the ONE emission whose gate names a subsystem constant
        // directly instead of going through MGPipeSubsystemForDirty, and the reason is that
        // it has no dirty bit: it carries what has no shutter of its own, which is what makes
        // it the residue. That exception is safe only while no dirty bit claims the same
        // subsystem - if one ever did, the block would be gated twice and that bit's own
        // emission would silently inherit the residual A/B switch. Asserted rather than
        // assumed, the same discipline SubsystemForEmitter's five static_asserts use.
        constexpr Bool NoDirtyBitOwnsTheResidualSubsystem() {
            for (SizeT i = 0; i < kMGPipeDirtyCount; ++i) {
                if (MGPipeSubsystemForDirty(static_cast<MGPipeDirty>(i)) ==
                    kMGPipeSubsystemResidualValues) {
                    return false;
                }
            }
            return true;
        }
        static_assert(NoDirtyBitOwnsTheResidualSubsystem(),
                      "a MGPipeDirty bit now owns kMGPipeSubsystemResidualValues: route the "
                      "residual block's gate through MGPipeSubsystemForDirty like every other "
                      "emission, or the two gates will disagree");

        constexpr Uint32 kAllDynamicChunks =
            static_cast<Uint32>((Uint64{1} << kMGPipeDynamicChunkCount) - 1);

        // create/bind_render_state and set_dynamic_state. Returns the bytes that went on the
        // wire, for the payload histogram.
        Uint64 EmitRenderState(GLContext& ctx, Uint32 dirty, Bool freshlyPrimed) {
            MGPipeTracker& tracker = MGPipeTrackerInstance();
            const RenderStateParameters& live = ctx.GetRenderStateParameters();
            const auto version = static_cast<Uint16>(ctx.GetRenderStateParametersVersion());
            const auto pipelineVersion = static_cast<Uint16>(ctx.GetPipelineStateVersion());
            Uint64 payloadBytes = 0;

            if (dirty & MGPipeDirtyBit(MGPipeDirty::NewPipelineState)) {
                const MGPipeHandle cso = MGPipeCsoCacheInstance().Acquire(live, payloadBytes);
                MGPBindRenderState bind{};
                bind.Cso = cso;
                bind.Version = version;
                bind.PipelineVersion = pipelineVersion;
                MGPipeRouteBindRenderState(bind);
                payloadBytes += sizeof(MGPBindRenderState);
                if (MG_Util::PipeStats::Enabled()) {
                    MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::RenderStateCsoBinds, 1);
                }
            }

            if (dirty & MGPipeDirtyBit(MGPipeDirty::NewRenderState)) {
                // The chunk-level suppressor: only the dynamic chunks that differ from what
                // the server has. A glViewport sends chunk D0 and nothing else; a
                // glClearColor sends D2. An EMPTY mask still sends the 32-byte header,
                // because the VERSION is what Magma's dynamic tail gates on and it moved.
                const Uint32 chunkMask =
                    freshlyPrimed ? kAllDynamicChunks
                                  : MGPipeDynamicChunksThatMoved(live, tracker.Staged());
                Array<Uint8, kMGPipeDynamicChunkBytes> blob;
                const SizeT blobBytes = MGPipeDynamicChunkBlobBytes(chunkMask);
                MGPipeGatherDynamicChunks(live, chunkMask, blob.data());
                MGPDynamicState dyn{};
                dyn.ChunkMask = chunkMask;
                dyn.Version = version;
                dyn.Blob.Size = blobBytes;
                MGPipeRouteSetDynamicState(dyn, blob.data());
                payloadBytes += sizeof(MGPDynamicState) + blobBytes;
            }

            // The staging mirror is what set_dynamic_state diffs against, so it may only be
            // advanced by the branch that actually SENT dynamic bytes. Latching it whenever
            // either bit fired would, if NEW_PIPELINE_STATE could ever fire alone, claim the
            // server holds chunks it never received - and the chunk-level suppressor would
            // then never resend them, which is a permanently stale answer with no gate on it.
            //
            // It cannot fire alone today because BumpVersions() moves both counters
            // (RenderState.h), but that is an invariant of ANOTHER package's file. So it is
            // asserted here rather than assumed, and the assignment is narrowed to the one
            // bit that owns the mirror.
            //
            // MOBILEGL_ASSERT compiles out in Release/INFO, which is the G1/G3
            // configuration, so the assert itself is a debug/verify-only alarm. THE
            // BEHAVIOUR IS SAFE IN EVERY BUILD REGARDLESS, and it is the narrowing below
            // rather than the assert that makes it so: if the invariant ever broke in a
            // shipping build the mirror would simply not advance, which costs a re-send of
            // chunks the server already has and never claims it holds chunks it does not.
            MOBILEGL_ASSERT((dirty & MGPipeDirtyBit(MGPipeDirty::NewPipelineState)) == 0 ||
                                (dirty & MGPipeDirtyBit(MGPipeDirty::NewRenderState)) != 0,
                            "NEW_PIPELINE_STATE fired without NEW_RENDER_STATE: RenderState's "
                            "BumpVersions no longer moves both counters");
            if (dirty & MGPipeDirtyBit(MGPipeDirty::NewRenderState)) {
                tracker.Staged() = live;
            }
            return payloadBytes;
        }

        // ---- P3a's three vertex-input emitters (D-G3, D-H3, D-I) ----
        //
        // The shape - three functions in the fixed order elements, then buffers, then index,
        // after the four P2 emitters - is the contract commit's, so that the commit which
        // fills the bodies in does not also have to edit the validate point. All three now
        // have bodies and MGPipeSubsystemForDirty maps their bits onto the vertex-input
        // subsystem, so `wants()` can be true.
        //
        // Everything they do lives in MG_Impl/Pipe/VertexInputEmit.h; what is here is the
        // adaptation to the validate point's byte-counting contract.
        Uint64 EmitVertexElements(GLContext& ctx) {
            return MGPipeVertexInputEmitterInstance().EmitVertexElements(ctx);
        }

        Uint64 EmitVertexBuffers(GLContext& ctx) {
            MGPipeTracker& tracker = MGPipeTrackerInstance();
            return MGPipeVertexInputEmitterInstance().EmitVertexBuffers(ctx, tracker.PendingBaseInstance());
        }

        Uint64 EmitIndexBuffer(GLContext& ctx) {
            return MGPipeVertexInputEmitterInstance().EmitIndexBuffer(ctx);
        }

        // ---- P4a's seven emitters (D-C, D-D, D-F, D-G, D-H) ----
        //
        // THE SHAPE IS THE CONTRACT COMMIT'S, exactly as P3a's three were: seven adapters
        // whose bodies live in the five family headers, so the commits that fill those
        // emitters in never touch this file. Every one of them returns 0 today.
        //
        // THE ORDER IS ARCHITECTURE.md 5.4's RECOMMENDED ONE - framebuffer, then program, then
        // textures/sampler/image/global constants - and that document is explicit that the
        // order is code organisation and NOT a contract: all of a verb's set_*/bind_* must
        // complete before the verb, and apart from "a resource create precedes a bind to it"
        // there is no ordering requirement between them. The server specialises the shader and
        // the pipeline lazily at the verb, from everything it holds at that moment, which is
        // what makes deriving the fragColor broadcast count from the framebuffer record legal
        // at the verb rather than at the FBO sync.
        Uint64 EmitFramebufferState(GLContext& ctx) {
            return MGPipeFramebufferEmitterInstance().EmitFramebufferState(ctx);
        }

        Uint64 EmitShaderState(GLContext& ctx) {
            return MGPipeProgramEmitterInstance().EmitShaderState(ctx);
        }

        Uint64 EmitGlobalConstants(GLContext& ctx) {
            return MGPipeProgramEmitterInstance().EmitGlobalConstants(ctx);
        }

        Uint64 EmitSamplerViews(GLContext& ctx) {
            return MGPipeSamplerEmitterInstance().EmitSamplerViews(ctx);
        }

        Uint64 EmitSamplerStates(GLContext& ctx) {
            return MGPipeSamplerEmitterInstance().EmitSamplerStates(ctx);
        }

        Uint64 EmitShaderImages(GLContext& ctx) {
            return MGPipeImageEmitterInstance().EmitShaderImages(ctx);
        }

        // The texture sub-data DRAIN, and it is the one P4a emitter with no dirty bit over it.
        // Its calls are dispatched from the GL entry points that cause them (a constructor, a
        // storage definition, a glTexParameter) and the only thing that has to wait for the
        // validate point is the accumulated upload, so the gate is the subsystem bit alone.
        // With nothing dirty the drain list is empty and this is one test.
        Uint64 DrainTextureSubData(GLContext& ctx) {
            return MGPipeTextureEmitterInstance().DrainTextureSubData(ctx);
        }

        // ---- P5e's two, and they are TWO adapters over THREE records (sb, §5.6) ----
        //
        // The split is the DIRTY BITS' and not the classes': bit 15 is the uniform binding
        // points and bit 16 is the two writable classes, which share a shutter because a
        // storage bind and a counter bind are the same event to every reader of the record.
        // Bit 17's family (set_stream_output_targets) has no adapter at all - XFB stays
        // lockstep for the whole of P5e (§5.7) - and that absence is the catalogue's split,
        // not an omission.
        Uint64 EmitConstBuffers(GLContext& ctx) {
            return MGPipeShaderBufferEmitterInstance().EmitConstBuffers(ctx);
        }

        Uint64 EmitShaderBuffers(GLContext& ctx) {
            return MGPipeShaderBufferEmitterInstance().EmitShaderBuffers(ctx);
        }
    } // namespace

    Uint64 MGPipeVertexAttribDefaultRepairCount() { return g_attribDefaultRepairs; }
    MGPVertexAttribDefaults MGPipeVertexAttribDefaultsLastHeader() { return g_attribDefaultLastHeader; }

    // The unit gate's door onto step 4's predicate (PipeFill.h says why it needs one). It
    // returns the memo's answer rather than a second copy of the expression, so a case that
    // moves one input and reads the answer back is a statement about the MEMO and not about a
    // re-implementation of it.
    Bool MGPipeResidualFillSuppliesField(MGPipeInputField field, Uint64 pushMask, Bool applierDerives,
                                         Bool contextValuesWireLive) {
        return MGPipeFieldMaskHas(SuppliedFieldMask(pushMask, applierDerives, contextValuesWireLive),
                                  field);
    }

    // ---- the validate point (P2 brief D1) ----
    void MGPipeValidateForVerb(MGPipeVerb verb) {
        // P5f (f1): the fill side's one new spelling. With MOBILEGL_IPC_ROLE_SPLIT_STATE=1 this
        // is the CLIENT-role block and gPipeInputs is the server's alone; off, or under
        // monolith transport, it folds back onto gPipeInputs and nothing below changes.
        PipeInputs& inputs = MGPipeClientInputs();
#if MOBILEGL_BUILD_DISAGGREGATED
        // ---- P5e (ra), CONTRACT-P5E §3: WHO OWNS gPipeInputs FOR THIS VERB ----------------
        //
        // Under run-ahead the block is SERVER-ROLE MEMORY for an unbarriered record: the
        // client does not fill it, does not stamp it and does not withdraw the server's stamp,
        // because it will not be parked while the apply runs and every one of those writes
        // would race the applier's own reads. What still runs, unchanged, is the tracker walk
        // and the emitters (steps 2 and 3): those PRODUCE RECORDS, which is the whole of what
        // an unbarriered verb is allowed to hand the server.
        //
        // `fillOwed` is deliberately a separate name from `barriered`, and that is what makes
        // the red-once one line: setting it to `true` restores the old behaviour (a fill for
        // every verb) and the guard below then fires Fatal{RoleViolation, "gPipeInputs"} on
        // the first unbarriered verb, by name.
        const Bool runAhead = ClientRunsAhead();
#endif
        ParsePoisonOmissionKnob();
#if MOBILEGL_PIPE_VERIFY
        ArmVerify();
#else
        // The runtime knob without the compiled comparator is a no-op that would look green;
        // this warning is what a lane's arming assertion turns into red.
        if (MG_Config::Features.PipeVerify) {
            MGLOG_W_ONCE("MGPipe: MOBILEGL_PIPE_VERIFY=1 requested but the comparator is not compiled in "
                         "(configure with -DMOBILEGL_PIPE_VERIFY=ON)");
        }
#endif
        auto* ctx = LiveContext();
        // THE IDENTITY / LIVENESS PAIR IS WRITTEN ON EVERY VERB, run-ahead or not, and that is
        // a NAMED DEVIATION from CONTRACT-P5E §3.1's list (see the report): `m_live` and
        // `m_contextIdentity` are not FIELDS of the residual model - no FieldOwnership.def row
        // owns them, no applier record carries them, and no server stamp can answer them -
        // they are "does this process still have a GL context", which every null-context guard
        // in the backend reads and which only this thread can know. Two stores, no freshness
        // and no stamp, so an unbarriered apply reading them reads a fact about the client's
        // process rather than a value the wire owes it.
        MGPipeFillAccess::SetIdentity(inputs, ctx);
#if MOBILEGL_BUILD_DISAGGREGATED
        // §2.1's predicate, client-side (see ClientVerbIsBarriered).
        const Bool barriered = !runAhead || ClientVerbIsBarriered(verb, ctx);
        // THE ONE LINE THE RED-ONCE FLIPS: `= true` here is "fill for every verb", the
        // pre-P5e behaviour, and it turns the guard below into the abort §3.5 names.
        const Bool fillOwed = barriered;
        g_lastFillWasBarriered = fillOwed;
        if (fillOwed) {
            // P5c (gt, CONTRACT-P5C §6 layer 2) / P5e §3.5: the residual fill is THE
            // client-side write into gPipeInputs. Under lockstep it is legal because it runs
            // before the record is published - the apply thread's in-applier flag is provably
            // down here. Under run-ahead it is legal because this record is BARRIERED: this
            // thread is about to park behind it. The second argument is which of the two
            // claims the caller is making, and a fill that made the second one falsely is the
            // named abort.
            //
            // P5e (ra2): AND IT IS MADE TRUE FIRST. Phase 1 of the block write is three lines
            // below - the serial bump, MGPipeServerClearVerbBoundary() and SetVerb - and every
            // one of them is a scalar the apply thread reads at each field access inside the
            // record it is currently applying. Withdrawing the server's own stamp from under it
            // is what produced `Fatal{UnmigratedPipeInput, "<RECORD-SUPPLIED field>@<this
            // thread's verb>"}` on the apply thread (report §2). THE RED-ONCE IS THIS LINE:
            // delete it and the guard below aborts with Fatal{RoleViolation, "gPipeInputs"}
            // naming MGPipeValidateForVerb, because the guard now tests the fact rather than
            // taking the claim.
            QuiesceApplierBeforeFill("MGPipeValidateForVerb");
            MG_Remote::Client::ClientSession::RefusePipeInputsTouchWhileApplierOwnsIt(
                "MGPipeValidateForVerb", /*isBarrieredFill=*/barriered);
        }
#else
        constexpr Bool fillOwed = true;
#endif
        if (fillOwed) {
#if MOBILEGL_PIPE_POISON
            // Starts at 1: FilledGen == 0 is "never filled", and MGPipeInputFieldIsFresh
            // refuses it on both branches, so a read before this first bump is
            // Fatal{UnmigratedPipeInput, "<Field>@<none>"} rather than default storage.
            ++MGPipeFillAccess::Filled(inputs).CurrentVerbSerial;
#endif
#if MOBILEGL_BUILD_DISAGGREGATED
            // The client is filling, so whatever the server stamped at its last verb boundary
            // is withdrawn: the stamps below are the CLIENT's again and a stale read is a
            // defect, not a residual pull. Disarming here rather than at the end of the
            // applier's work is what makes the arming flag say "the current stamps are the
            // server's" no matter which of the two roles ran last.
            //
            // P5e (§3.2): under run-ahead this runs only inside a barriered fill, which is the
            // whole of the E note's hazard (b.4) - a GL thread withdrawing the SERVER's stamp
            // while the apply thread is inside a record that depends on it. For an unbarriered
            // verb the stamp is not touched at all, and the applier's own LeaveApplier is then
            // its only writer.
            //
            // P5f (f1): this is now the CLIENT block's flag (MGPipeClientClearVerbBoundary).
            // With the rehearsal off both names denote the one shared block and the semantics
            // above are unchanged; with it on the client block's flag is never raised and the
            // clear is a no-op, and withdrawing the SERVER's stamp is the applier's job alone
            // (PipeApplier::LeaveApplier) - which is CONTRACT-P5E §3.2's per-role stamp
            // ownership, landed as the dual block rather than as moved fields.
            MGPipeClientClearVerbBoundary();
#endif
            MGPipeFillAccess::SetVerb(inputs, verb);
        }
#if MOBILEGL_PIPE_POISON
        MGPipeFilledState& filled = MGPipeFillAccess::Filled(inputs);
#endif
        if (ctx == nullptr) {
            // The pending base instance belongs to THIS verb, and this exit skips step 3's
            // clear, so it has to make the same promise here: a base-instanced draw with no
            // live context is a no-op, but leaving its argument standing would hand it to the
            // next verb - which, since the tracker's Reset() no longer clears it, is the one
            // path that could still carry a stale shift across.
            MGPipeTrackerInstance().ClearPendingBaseInstance();
            return;
        }
        const MGPipeVerbClass verbClass = kMGPipeVerbClass[static_cast<SizeT>(verb)];
        const MGPipeFieldMask& mask = kMGPipeClassFieldMask[static_cast<SizeT>(verbClass)];

        // ---- step 2: the dirty walk (P2 brief D1, D4) ----
        // The mask is computed, latched and counted here and nothing is emitted from it
        // yet: this commit is the safety net that says the walk is semantically free
        // before any field stops being pulled. The emission steps land on top of it.
        MGPipeTracker& tracker = MGPipeTrackerInstance();
        const Uint32 dirty = tracker.Update(*ctx, verbClass);

        // ---- step 3: emission ----
        // Every gate below goes through MGPipeSubsystemForDirty, the ONE map from a dirty bit
        // to the runtime subsystem that owns it. Naming the subsystem constants here instead
        // would be a second copy of that map in the only path that runs, and mis-gating a bit
        // in it would pass every test the map has.
        //
        // FIVE CONDITIONS, AND THE WIRED MASK IS ONE OF THEM. `kMGPipeWiredSubsystems` is the
        // OR of the per-family constants each emit header defines, and the whole ownership
        // design rests on it MEANING what the headers, this file and the result files all say
        // it means: an emitter runs only once the commit that gave it a body set its family's
        // constant. Without this condition a family whose header still says 0 would be CALLED
        // at every verb whose bit fires under the shipped default mask, so the commit that
        // lands the body would go live one commit early and every gate run in between would
        // measure an arm nobody thinks is on - and the mirror error is worse: a family that
        // lands its body and forgets the constant would emit nothing and look broken. The
        // P2/P3a bits are all in the mask, so nothing that emits today changes.
        //
        // AND THE FIFTH IS P4aFamilyHasItsConsumer (ID-39), the same conjunct FamilyIsLive
        // applies to every birth hook: a P4a family whose records nothing on this backend
        // consumes emits NOTHING, so the legacy pull path runs exactly as it does on the pull
        // build. It is written here rather than folded into kMGPipeWiredSubsystems because the
        // wired mask is a property of the BUILD - a constexpr an emit header sets - and this is
        // a property of the RUNNING BACKEND, and collapsing the two would make a bisect that
        // lands between them unreadable. The P2/P3a bits are outside kMGPipeP4aFamilySubsystems,
        // so the conjunct is true for every one of them and nothing that emits today changes.
        //
        // AND THE SIXTH IS P4aFamilyDependenciesAreSet (S-3 / ID-41), the client half of D-K2:
        // a family one of whose dependency bits the operator left clear emits NOTHING here for
        // the same reason - the server REFUSES that family and runs its legacy arm, and an
        // emission the server refuses is an emission whose acceptance already cleared a frontend
        // dirty flag the legacy arm still owed. Same table, same four families, one place.
        const Uint64 pushMask = MG_Config::Features.PipePush;
#if MOBILEGL_BUILD_DISAGGREGATED
        // P5c rv (CONTRACT-P5C.md §5.3): set_context_values is the carrier for the eight
        // value-class fields ONLY with a live wire. ONE answer gates both halves - the emission
        // below and the residual-fill skip in step 4 - so they can never disagree about who
        // supplies a field: with no live session (a monolith transport, the bring-up window, a
        // server-role-only fixture) nothing is emitted and the fields keep being pulled, byte
        // for byte as before (G1).
        const Bool contextValuesWireLive =
            MG_Config::Transport != MG_Config::TransportMode::Monolith &&
            MG_Remote::Client::ContextValuesWireLive();
#else
        constexpr Bool contextValuesWireLive = false;
#endif
        // AND THE SEVENTH IS P5eFamilyIsLive (ID-106), which is the same sentence for the
        // binding-point family and asks bit 13's OWN consumer bit rather than the resource
        // family's. It answers true for every subsystem but bit 13, so nothing that emitted
        // before P5e changes.
        const auto wants = [&](MGPipeDirty bit) {
            const Uint64 subsystem = MGPipeSubsystemForDirty(bit);
            return subsystem != 0 && (pushMask & subsystem) != 0 &&
                   (kMGPipeWiredSubsystems & subsystem) != 0 &&
                   P4aFamilyHasItsConsumer(subsystem) &&
                   P4aFamilyDependenciesAreSet(subsystem, pushMask) &&
                   P5eFamilyIsLive(subsystem, pushMask) &&
                   (dirty & MGPipeDirtyBit(bit)) != 0;
        };
        Uint64 payloadBytes = 0;

        // A fresh context is a fresh server, and that is true of EVERY subsystem, so it is
        // handled BEFORE the per-subsystem gates rather than inside one of them. It used to
        // live inside EmitRenderState, which runs only when bit 0 of MOBILEGL_PIPE_PUSH is
        // set - so the per-subsystem A/B D14 invites (clear bit 0, keep bits 1..3) gave a
        // fresh context a never-reset applier while every other slot WAS invalidated.
        //   - the CSO cache's handles name slots this client's allocator is about to hand
        //     out again, so both sides start over together rather than one of them
        //     remembering the other's objects;
        //   - what the server has is no longer what any suppressor slot last emitted;
        //   - and the residual block owes a fresh publication whatever else moved.
        if (tracker.FreshlyPrimed()) {
#if MOBILEGL_BUILD_DISAGGREGATED
            // P5c (ct), CONTRACT-P5C.md §5.1: with an active transport the server's reset
            // crosses AS A RECORD, ahead of every reset below - the client-side ones (the CSO
            // cache, the hash suppressor, the vertex-input emitter) and the emitters' latches
            // - because the record's barrier is what orders the server's MGPipeApplierReset()
            // against every verb that follows. The GL-thread direct call it replaces is
            // Fatal{RoleViolation, "g_applier"} inside MGPipeApplierReset itself (§6 layer 2),
            // so reverting this arm to the direct call goes red by name rather than rendering
            // stale. Monolith keeps the direct call, byte for byte (G1).
            //
            // THE APPLY THREAD IS EXCLUDED (M5's rule): a validate running on the server's own
            // thread produces no client record - EmitAndWait there would wait on the thread
            // that has to apply the record - and the direct call is exactly what the apply
            // thread is allowed to make (the sink's own path runs it there).
            //
            // AND A CONFIGURED-BUT-WIRELESS TRANSPORT TAKES THE DIRECT CALL TOO:
            // EmitApplierResetRecord answers false when no live session could carry the
            // record (the pre-Start bring-up window, a ServerLoop fixture with no client at
            // all), and in that shape this process IS the only place the reset can run.
            const Bool transportActive =
                MG_Config::Transport != MG_Config::TransportMode::Monolith &&
                !MG_Remote::Client::RunsAsTheServerRole();
            Bool resetCrossed = false;
            if (transportActive) {
                resetCrossed = MG_Remote::Client::EmitApplierResetRecord();
            }
#endif
            MGPipeCsoCacheInstance().Reset();
#if MOBILEGL_BUILD_DISAGGREGATED
            if (!resetCrossed)
#endif
                MGPipeApplierReset();
            MGPipeSetHashSuppressorInstance().InvalidateAll();
            // P3a: and the vertex-input emitter's latches. NOT because the applier dropped
            // its vertex-elements records - it does not, they are share-group object state
            // and survive a make-current - but because the emitter's OTHER latch, the bound
            // handle, mirrors the applier's BoundVertexElements, which MGPipeApplierReset
            // DOES clear. Without this the bind after a make-current would be suppressed as
            // unchanged and the server would draw with no vertex elements bound. Re-creating
            // an unchanged configuration alongside it is a bounded over-fire; a dropped bind
            // is not. The resource tracker is deliberately NOT reset here for the same
            // reason its records survive: see ResourceTracker.h's ResetForTest.
            MGPipeVertexInputEmitterInstance().Reset();
            // P4a's five, and ONLY their latches: MGPipeApplierReset clears the framebuffer
            // records, the three unit sets and the three program handles, so the emitters'
            // mirrors of those must go with them or the first emission after a make-current
            // would be suppressed as unchanged and the server would draw with the previous
            // context's bindings. What must NOT reset is the RECORD half - the applier keeps
            // its texture, sampler, view and shader-CSO records across a make-current, because
            // a GL object lives in a share group, and re-publishing one would move its Serial
            // for nothing.
            MGPipeFramebufferEmitterInstance().Reset();
            MGPipeTextureEmitterInstance().Reset();
            MGPipeSamplerEmitterInstance().Reset();
            MGPipeImageEmitterInstance().Reset();
            MGPipeProgramEmitterInstance().Reset();
            // P5e (sb): and the binding-point emitter's mirrors. MGPipeApplierReset clears all
            // three of the applier's windows and ADVANCES ShaderBuffersSerial, so the emitter's
            // latch has to reset with it - the three suppressor slots are already cleared by
            // InvalidateAll() above, and without that the first emission after a make-current
            // would be suppressed as unchanged and the server would draw against a window it
            // had just been told to empty.
            MGPipeShaderBufferEmitterInstance().Reset();
            g_residualDue = true;
        }

        // P4a's segment, in ARCHITECTURE.md 5.4's RECOMMENDED order - framebuffer, then
        // program, then textures / sampler / image / global constants - which is why it stands
        // before the render-state block rather than after it. That order is explicitly code
        // organisation and not a contract (all of a verb's set_*/bind_* complete before the
        // verb, and the server specialises lazily AT the verb from everything it then holds),
        // so nothing about the P2 and P3a emissions changes by standing after it; what it buys
        // is that the file reads in the order the design states.
        //
        // ALL SEVEN ARE STUBS AT THE CONTRACT COMMIT and all four family bits are absent from
        // kMGPipeWiredSubsystems, so `wants()` is false for every one of them - it tests that
        // mask as its third condition, which is what makes the sentence true rather than
        // merely intended - and this whole block is dead until the packages that own the
        // emitters land. Placing it here, once, is what keeps those packages out of this file.
        if (wants(MGPipeDirty::NewFramebuffer)) {
            payloadBytes += EmitFramebufferState(*ctx);
        }
        if (wants(MGPipeDirty::NewShader) || wants(MGPipeDirty::NewShaderBindings)) {
            payloadBytes += EmitShaderState(*ctx);
        }
        // The texture drain has no dirty bit over it (see its definition); it is gated on the
        // subsystem bit, on this build having wired the family at all, on a backend having
        // registered the consumer and on D-K2's dependency bits for the family being set, which
        // is the same quadruple `wants()` applies to every other emission. The last two are the
        // whole of ID-39 and of S-3 on the path where they mattered most: the drain is what
        // clears a level's dirty flags on acceptance, so a drain that ran against an applier no
        // backend reads is exactly how Magma lost its texel uploads, and a drain that ran at a
        // mask whose bit 11 or bit 7 is clear is how Espryt lost them at 0x7ff and 0x5ff.
        if ((pushMask & kMGPipeSubsystemTextureResources) != 0 &&
            (kMGPipeWiredSubsystems & kMGPipeSubsystemTextureResources) != 0 &&
            P4aFamilyHasItsConsumer(kMGPipeSubsystemTextureResources) &&
            P4aFamilyDependenciesAreSet(kMGPipeSubsystemTextureResources, pushMask)) {
            payloadBytes += DrainTextureSubData(*ctx);
        }
        if (wants(MGPipeDirty::NewSamplerViews)) {
            payloadBytes += EmitSamplerViews(*ctx);
        }
        if (wants(MGPipeDirty::NewSamplers)) {
            payloadBytes += EmitSamplerStates(*ctx);
        }
        if (wants(MGPipeDirty::NewShaderImages)) {
            payloadBytes += EmitShaderImages(*ctx);
        }
        if (wants(MGPipeDirty::NewGlobalConstants)) {
            payloadBytes += EmitGlobalConstants(*ctx);
        }

        // P5e's segment (sb, §5.6): the indexed buffer binding points, AFTER the program
        // segment for the same "code organisation, not a contract" reason ARCHITECTURE.md 5.4
        // gives for P4a's order - all of a verb's set_*/bind_* complete before the verb, and
        // the server resolves a point against its own descriptor at its sync point. It reads
        // better here because the uniform window is what the program's block bindings INDEX
        // (DirectGLES.cpp's UBO loop), so the two records that describe a draw's uniform
        // buffers stand together.
        //
        // NOTHING FOR BIT 17: set_stream_output_targets stays unemitted for the whole of P5e
        // (§5.7). The bit is still computed, counted and mapped onto this subsystem, so an
        // operator clearing bit 13 gets the whole family's frontend walk back.
        if (wants(MGPipeDirty::NewConstBuffers)) {
            payloadBytes += EmitConstBuffers(*ctx);
        }
        if (wants(MGPipeDirty::NewShaderBuffers)) {
            payloadBytes += EmitShaderBuffers(*ctx);
        }

        if (wants(MGPipeDirty::NewPipelineState) || wants(MGPipeDirty::NewRenderState)) {
            payloadBytes += EmitRenderState(*ctx, dirty, tracker.FreshlyPrimed());
        }
        if (wants(MGPipeDirty::NewPixelPack)) {
            payloadBytes += EmitPixelPackState(*ctx);
        }
        if (wants(MGPipeDirty::NewPatchState)) {
            payloadBytes += EmitPatchState(*ctx);
        }
        if (wants(MGPipeDirty::NewVertexAttribDefaults)) {
            payloadBytes += EmitVertexAttribDefaults(*ctx, tracker.FreshlyPrimed());
        }

#if MOBILEGL_BUILD_DISAGGREGATED
        // P5c rv (CONTRACT-P5C.md §5.3): the residual-value record, emitted when any covered
        // value moved. THE GATE IS THE SUBSYSTEM BIT PLUS THE WIRE BEING LIVE - the family has
        // no dirty bit (NoDirtyBitOwnsTheResidualSubsystem) and no P4a consumer predicate (it
        // is not one of the four families), and the "did anything move" question is the
        // whole-record hash inside EmitContextValues. The class-mask test skips the build for
        // verbs that read none of the eight fields.
        if (contextValuesWireLive && (pushMask & kMGPipeSubsystemResidualValues) != 0 &&
            VerbMaskReadsContextValues(mask)) {
            payloadBytes += EmitContextValues(*ctx);
        }
#endif

        // P3a's vertex segment, in the order the design fixes: vertex elements, then the
        // vertex buffers that fill them, then the index binding. All three are LIVE now (m1):
        // bits 5 / 9 / 10 map onto kMGPipeSubsystemVertexInput in Tracker.h:145-148 and all
        // three emitters have bodies, so `wants()` answers true whenever bit 8 is in the push
        // mask - which the phase default 0x1ff sets, on every backend. The sentence that used
        // to stand here ("all three still resolve to false today - their dirty bits map to no
        // subsystem") was the contract commit's and stopped being true when the client landed.
        if (wants(MGPipeDirty::NewVertexElements)) {
            payloadBytes += EmitVertexElements(*ctx);
        }
        if (wants(MGPipeDirty::NewVertexBuffers)) {
            payloadBytes += EmitVertexBuffers(*ctx);
        }
        if (wants(MGPipeDirty::NewIndexBuffer)) {
            payloadBytes += EmitIndexBuffer(*ctx);
        }
        // CONSUMED, so the next verb starts from zero. The tracker's bit-9 shutter read it
        // above and EmitVertexBuffers put it on the wire; leaving it set would give the next
        // draw the previous draw's fetch shift, which is the exact defect the explicit field
        // exists to remove.
        tracker.ClearPendingBaseInstance();

        // ---- step 4: the residual fill, for what an emitted call did NOT supply ----
        // THE SUPPLIED QUESTION IS ASKED ONCE PER ENVIRONMENT, NOT ONCE PER FIELD PER VERB
        // (SuppliedFieldMask above, P5d r3 package C): the conjunction it used to spell inline
        // here reads nothing about the verb, so it is a memo keyed on the four process facts it
        // does read. The two halves of P5c rv's gate still read the ONE `contextValuesWireLive`
        // computed at the top of this function - it is the memo's key AND its argument - so they
        // cannot disagree about who supplies the eight value-class fields.
        //
        // P5e (ra, §3.1): AND IT DOES NOT RUN AT ALL FOR AN UNBARRIERED RECORD. Every write
        // below - the CopyField and the FilledGen stamp alike - is a GL-thread write into a
        // block the apply thread is about to read without this thread being parked, which is
        // exactly what rule F forbids. The server does not go without an answer: it stamps its
        // own boundary and §3.3's detector turns any field it still needs from here into
        // Fatal{UnmigratedPipeInput, "<field>@<verb>"} by name, which is what makes the strict
        // lane a gate rather than a count.
#if MOBILEGL_BUILD_DISAGGREGATED
        // P5e (ra2): PHASE 2 OF THE BLOCK WRITE, AND IT NEEDS ITS OWN WAIT. Everything between
        // the phase-1 wait and here PUBLISHED RECORDS - the fourteen emitters above - and the
        // apply thread reads gPipeInputs while it applies them. So the window the fill opened at
        // the top of this function was closed again by this function's own emissions, and the
        // 63-field walk below would run straight into it. Same call, same arm, same no-op
        // everywhere run-ahead is not armed; the guard beside it is what turns a missing one
        // into a named abort rather than a torn read.
        if (fillOwed) {
            QuiesceApplierBeforeFill("MGPipeValidateForVerb/residual");
            MG_Remote::Client::ClientSession::RefusePipeInputsTouchWhileApplierOwnsIt(
                "MGPipeValidateForVerb/residual", /*isBarrieredFill=*/true);
        }
#endif
        const Bool applierDerives = ApplierDerivesRenderStateFields();
        // BY VALUE, NOT BY REFERENCE: the memo's storage is rebuilt in place when the key moves,
        // and the walk below holds this across 63 iterations.
        const MGPipeFieldMask supplied =
            SuppliedFieldMask(pushMask, applierDerives, contextValuesWireLive);
        for (SizeT i = 0; fillOwed && i < kMGPipeInputFieldCount; ++i) {
            const auto field = static_cast<MGPipeInputField>(i);
            if (!MGPipeFieldMaskHas(mask, field)) continue;
#if MOBILEGL_PIPE_POISON
            if (kMGPipeInputFieldSticky[i]) {
                // Stamped once by the first fill that sees a live context; fresh through the
                // Sticky -> FilledGen != 0 branch of MGPipeInputFieldIsFresh from then on.
                if (filled.FilledGen[i] == 0) filled.FilledGen[i] = 1;
                continue;
            }
#else
            if (kMGPipeInputFieldSticky[i]) continue;
#endif
            if (!MGPipeFieldMaskHas(supplied, field)) MGPipeFillAccess::CopyField(inputs, *ctx, field);
#if MOBILEGL_PIPE_POISON
            // The value is copied either way; only the stamp is withheld for the omitted pair.
            if (!IsOmitted(verb, field)) filled.FilledGen[i] = filled.CurrentVerbSerial;
#endif
        }
        // ---- step 4b: the residual value block, and it goes out HERE ----
        // ARMED OUTSIDE THE SUBSYSTEM GATE: whether the capability set may have moved is a
        // fact about the frontend, not about which subsystems this build pushes, and a
        // per-subsystem A/B that turns the block off must not also lose the record that one
        // is owed.
        if ((dirty & (MGPipeDirtyBit(MGPipeDirty::NewRenderState) |
                      MGPipeDirtyBit(MGPipeDirty::NewPipelineState))) != 0) {
            g_residualDue = true;
        }
        // Its trip wire compares the carried bits against the ASSEMBLED capability mirror,
        // and that mirror is written either by the applier's derivation or by the fill loop
        // above - so the block is only meaningful once step 4 has run. Emitting it with the
        // other calls would compare against the previous verb's answer.
        if ((pushMask & kMGPipeSubsystemResidualValues) != 0) {
            // The trip wire compares against the ASSEMBLED capability mirror, so it can only
            // run at a verb whose class actually carries that mirror - IsCapabilityEnabled is
            // in seven of the nine class masks and kQuery and kXfbSpan do not read it, so at
            // those verbs the mirror is whatever the last verb that did read it left behind.
            // The change is HELD rather than dropped: dropping it would silently disarm the
            // wire for a capability that moved between two queries.
            if (g_residualDue && MGPipeFieldMaskHas(mask, MGPipeInputField::IsCapabilityEnabled)) {
                payloadBytes += EmitResidualValueState(*ctx);
                g_residualDue = false;
            }
        }
        if (payloadBytes != 0 && MG_Util::PipeStats::Enabled()) {
            // PipeStats::RecordDrawPayloadBytes has been implemented and unit-tested since
            // P0 and called by nothing; this is its first emitter, and the 24-bucket
            // histogram is what answers ROADMAP.md open question 4's chunk-granularity
            // retune with data instead of a guess.
            MG_Util::PipeStats::RecordDrawPayloadBytes(payloadBytes);
        }
#if MOBILEGL_PIPE_VERIFY
        EntryCompare(inputs, mask);
#endif
    }
} // namespace MobileGL::MG_Pipe
