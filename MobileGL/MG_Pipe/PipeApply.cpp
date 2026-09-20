// MobileGL - MobileGL/MG_Pipe/PipeApply.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The in-process applier (PipeApply.h). Compiled only under MOBILEGL_PIPE_PUSH.
//
// This is the one .cpp under MG_Pipe/ that reaches UP to MG_Backend/MGPipe/PipeInputs.h,
// and that is the point: under split it becomes the server, and the server is where the
// working state lives. Nothing in MG_Pipe's HEADERS reaches it, so purity gate A
// (MGPipeValueTypes.h's include closure) is untouched.
#include <MG_Pipe/PipeApply.h>

#include <MG_Backend/MGPipe/PipeInputs.h>

#if MOBILEGL_BUILD_DISAGGREGATED
// MGPipeUnmigratedEmulation's split arm reads MG_Config::Transport, which is what tells an
// emulation site whether it is running inside a server or in the monolith it was written for.
#include <Config.h>
#endif

#if MOBILEGL_PIPE_VERIFY
// THE ONE PLACE THE PROGRAM ARCHIVE'S CODEC IS CALLED, and it is compiled into the VERIFY
// build only. In monolith the archive does not travel - MGPProgramDesc's seven blob refs are
// declared with Size 0 and the two structs ride beside the record through the entry point's
// companion pointers - so a push build pays nothing for it and the header keeps its forward
// declarations. The verify build serialises, deserialises and compares before storing, which
// makes the codec LIVE CODE WITH A GATE rather than dead code with a unit test.
#include <MG_State/GLState/ProgramState/ProgramArtifactsCodec.h>
#endif

#if MOBILEGL_BUILD_DISAGGREGATED
// R-6's tier gate. One spelling, asked at the one place the decline is decided. Outside the
// MOBILEGL_PIPE_VERIFY block above on purpose: the tier is a property of the BUILD, not of the
// comparator, and a split build without the comparator still declines every acquisition.
#include <MG_Remote/Client/PersistentMapTracker.h>
// P5c (ct), CONTRACT-P5C.md §6 layer 2: MGPipeApplierReset's role guard asks
// ServerLoop::OnApplyThread() - the one predicate that tells the GL thread from the thread
// that owns g_applier under an active transport - and ClientSession::Active(), which is what
// tells a live wire from a configured-but-wireless one (the bring-up window and the
// server-role-only fixture: there the direct call is the only reset that exists).
#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/Server/ServerLoop.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

// THE VERDICT OF EVERY TRIP WIRE IN THIS FILE, IN ONE PLACE.
//
// MOBILEGL_ASSERT is inert at INFO (Defines.h), which is the level every P2 gate builds at,
// so nothing below is left to an assertion. A poison or verify build stops the process; a
// shipped push build logs at error level and carries on from a DEFINED state, and the
// applier counts the divergence so a unit case can see the wire fire there too. Only the
// poison/verify arm writes the "Fatal{...}" marker G4 greps the retrace logs for.
#if MOBILEGL_PIPE_POISON || MOBILEGL_PIPE_VERIFY
#define MGP_TRIP_WIRE_TAG(name) "Fatal{" name "}"
#define MGP_TRIP_WIRE_REPORT(...)                                                                                      \
    do {                                                                                                               \
        MGLOG_F(__VA_ARGS__);                                                                                          \
        std::abort();                                                                                                  \
    } while (0)
#else
#define MGP_TRIP_WIRE_TAG(name) name
#define MGP_TRIP_WIRE_REPORT(...) MGLOG_E(__VA_ARGS__)
#endif

// The 25 capabilities whose storage is a plain `<Name>Enabled` bool. Written ONCE and used
// twice - once for the switch arms of DeriveCapability and once for the chunk set that guards
// the capability walk - so the two cannot drift apart. The three P2 gave storage to
// (DepthClamp, FramebufferSrgb, TextureCubeMapSeamless) are in the list like any other; the
// three that are NOT are Blend (BlendStates[i].Enabled), ScissorTest (a 16-bit mask) and the
// eight ClipDistances (an 8-bit mask), each handled by name below.
#define MGP_PLAIN_CAPABILITY_LIST(X)                                                                                   \
    X(ColorLogicOp)                                                                                                    \
    X(DebugOutput)                                                                                                     \
    X(DebugOutputSynchronous)                                                                                          \
    X(DepthClamp)                                                                                                      \
    X(DepthTest)                                                                                                       \
    X(CullFace)                                                                                                        \
    X(Dither)                                                                                                          \
    X(FramebufferSrgb)                                                                                                 \
    X(LineSmooth)                                                                                                      \
    X(Multisample)                                                                                                     \
    X(PolygonOffsetFill)                                                                                               \
    X(PolygonOffsetLine)                                                                                               \
    X(PolygonOffsetPoint)                                                                                              \
    X(PolygonSmooth)                                                                                                   \
    X(PrimitiveRestart)                                                                                                \
    X(PrimitiveRestartFixedIndex)                                                                                      \
    X(RasterizerDiscard)                                                                                               \
    X(SampleAlphaToCoverage)                                                                                           \
    X(SampleAlphaToOne)                                                                                                \
    X(SampleCoverage)                                                                                                  \
    X(SampleMask)                                                                                                      \
    X(SampleShading)                                                                                                   \
    X(StencilTest)                                                                                                     \
    X(TextureCubeMapSeamless)                                                                                          \
    X(ProgramPointSize)

namespace MobileGL::MG_Pipe {
    namespace {
        // ----------------------------------------------------------------------------
        // WHICH CHUNKS EACH DERIVATION READS.
        //
        // The derivation is called after every scatter, and a scatter usually moves ONE
        // chunk: a per-frame glViewport sends dynamic chunk D0 and nothing else (D8). So the
        // three wide loops and the 35-arm capability switch are guarded by the chunks whose
        // bytes they read, and a scatter that did not touch those bytes does not pay for them.
        //
        // Nothing here is hand-mapped. Every constant is
        // MGPipeRenderStateChunkBitsCovering(offsetof(member), sizeof(member)) over the
        // members the guarded block actually reads, so the ONLY claim a reader has to check
        // is "does this block read anything else?" - and a boundary move re-computes the
        // guards rather than invalidating them.
        // ----------------------------------------------------------------------------
        using RSP = RenderStateParameters;
#define MGP_CHUNKS_OF(member) MGPipeRenderStateChunkBitsCovering(offsetof(RSP, member), sizeof(RSP::member))

        // The per-draw-buffer loop reads BlendStates (equations, factors, Enabled) and
        // ColorMasks, and nothing else.
        constexpr Uint32 kChunksBlendLoop = MGP_CHUNKS_OF(BlendStates) | MGP_CHUNKS_OF(ColorMasks);
        // m_viewportIndexed[16] and the rounded m_viewport both read Viewports, and nothing else.
        constexpr Uint32 kChunksViewportLoop = MGP_CHUNKS_OF(Viewports);
        constexpr Uint32 kChunksDepthRangeLoop = MGP_CHUNKS_OF(DepthRanges);
        constexpr Uint32 kChunksScissorEnableLoop = MGP_CHUNKS_OF(ScissorTestEnabledMask);
        // DeriveCapability's sources: the 25 plain bools, plus the three masks/arrays the
        // three special arms read.
#define MGP_CAPABILITY_CHUNKS(capability) | MGP_CHUNKS_OF(capability##Enabled)
        constexpr Uint32 kChunksCapabilityWalk = MGP_CHUNKS_OF(BlendStates) |
                                                 MGP_CHUNKS_OF(ScissorTestEnabledMask) |
                                                 MGP_CHUNKS_OF(ClipDistanceEnabledMask)
                                                     MGP_PLAIN_CAPABILITY_LIST(MGP_CAPABILITY_CHUNKS);
#undef MGP_CAPABILITY_CHUNKS

        // The patch trio's chunk, which is what arms the set_patch_state trip wire: the
        // question that wire asks is whether the chunk-P0 bytes in the working block are the
        // APPLIER'S, and only a scatter puts them there.
        constexpr Uint32 kChunksPatchTrio = MGP_CHUNKS_OF(PatchVertices) |
                                            MGP_CHUNKS_OF(PatchDefaultOuterLevel) |
                                            MGP_CHUNKS_OF(PatchDefaultInnerLevel);

        // Which chunks ONE capability's answer is read out of - the same sources
        // DeriveCapability reads, written from the same list so the two cannot drift. This is
        // what arms the residual trip wire PER CAPABILITY: a bind alone owns the pipeline
        // half, and the eight ClipDistances are answered from ClipDistanceEnabledMask in
        // DYNAMIC chunk D7, so between a bind and the first set_dynamic_state exactly those
        // eight are unanswerable and the other 27 are not.
        constexpr Uint32 CapabilitySourceChunks(CapabilityInput cap) {
#define MGP_CAPABILITY_SOURCE(capability)                                                                              \
    case CapabilityInput::capability:                                                                                  \
        return MGP_CHUNKS_OF(capability##Enabled);
            switch (cap) {
                MGP_PLAIN_CAPABILITY_LIST(MGP_CAPABILITY_SOURCE)
            case CapabilityInput::Blend:
                return MGP_CHUNKS_OF(BlendStates);
            case CapabilityInput::ScissorTest:
                return MGP_CHUNKS_OF(ScissorTestEnabledMask);
            case CapabilityInput::ClipDistance0:
            case CapabilityInput::ClipDistance1:
            case CapabilityInput::ClipDistance2:
            case CapabilityInput::ClipDistance3:
            case CapabilityInput::ClipDistance4:
            case CapabilityInput::ClipDistance5:
            case CapabilityInput::ClipDistance6:
            case CapabilityInput::ClipDistance7:
                return MGP_CHUNKS_OF(ClipDistanceEnabledMask);
            // A capability with no storage cannot be answered from any byte, and
            // DeriveCapability says so with a compile-time false. Demanding the whole table
            // keeps such a value out of the comparison until every chunk is owned, which is
            // the conservative direction: a wire that cannot be answered must not fire.
            default:
                return kMGPipeAllGlobalChunks;
            }
#undef MGP_CAPABILITY_SOURCE
        }

        // The scalar copies are left unguarded on purpose: they are ~20 stores and two
        // 28-byte struct copies, so guarding each would cost more branches than it saves
        // stores - and an unguarded copy cannot go stale, which keeps the risk of the scoping
        // confined to the four guards above.
#undef MGP_CHUNKS_OF
    } // namespace

    // The applier's door into PipeInputs' storage, the write-side twin of PipeFill.cpp's
    // MGPipeFillAccess. It does NOT stamp the poison generations: a stamp says "the filler
    // published this field for THIS verb", and that statement belongs to the walk that
    // called the applier, not to the applier - MG_Impl/Pipe/PipeFill.cpp stamps what it
    // emitted, exactly as it stamps what it copied.
    struct MGPipeApplyAccess {
        static RenderStateParameters& RenderState(PipeInputs& inputs) { return inputs.m_renderState; }
        static PixelStoreParameters& PackState(PipeInputs& inputs) { return inputs.m_pixelStore[0]; }
        static Bool* Capabilities(PipeInputs& inputs) { return inputs.m_capability; }
        static PipeInputs::CurrentVertexAttributeValue* VertexAttribDefaults(PipeInputs& inputs) {
            return inputs.m_currentVertexAttribute;
        }
        static void SetRenderStateVersions(PipeInputs& inputs, Uint parameters, Uint pipeline) {
            inputs.m_renderStateParametersVersion = parameters;
            inputs.m_pipelineStateVersion = pipeline;
        }
        static void SetRenderStateParametersVersion(PipeInputs& inputs, Uint parameters) {
            inputs.m_renderStateParametersVersion = parameters;
        }
        static void SetPatchState(PipeInputs& inputs, Uint vertices, const FloatVec4& outer,
                                  const FloatVec2& inner) {
            inputs.m_patchVertices = vertices;
            inputs.m_patchDefaultOuterLevel = outer;
            inputs.m_patchDefaultInnerLevel = inner;
        }
        // P5c (rv, CONTRACT-P5C.md §5.3): set_context_values' write, one field per payload
        // member, on the server side of the wire. Like PackState it is a plain store - the
        // stamp is the filler/verb boundary's statement, not the applier's (see the struct's
        // header comment).
        static void SetContextValues(PipeInputs& inputs, const MGPContextValues& values) {
            inputs.m_activeTextureUnit = static_cast<Int>(values.ActiveTextureUnit);
            inputs.m_maxTouchedTextureUnit = static_cast<Int>(values.MaxTouchedTextureUnit);
            for (SizeT i = 0; i < PipeInputs::kBufferTargetCount; ++i) {
                inputs.m_touchedBindingPointCount[i] =
                    static_cast<SizeT>(values.TouchedBufferBindingPointCount[i]);
            }
            inputs.m_transformFeedbackActive = values.IsTransformFeedbackActive != 0;
            inputs.m_transformFeedbackPaused = values.IsTransformFeedbackPaused != 0;
            inputs.m_transformFeedbackGeneration = values.TransformFeedbackGeneration;
            inputs.m_boundTransformFeedbackLifetimeId = values.BoundTransformFeedbackLifetimeId;
            inputs.m_transformFeedbackCapturedVertices = values.TransformFeedbackCapturedVertices;
        }

        // ----------------------------------------------------------------------------
        // D5: the 29 PipeInputs fields that are PURE FUNCTIONS of RenderStateParameters.
        //
        // Once bind_render_state / set_dynamic_state have assembled the working block,
        // copying these out of GLContext a second time would be exactly the per-verb pull
        // P2 exists to remove - so the applier DERIVES them instead. Each line below is a
        // transcription of the RenderState getter of the same name (RenderState.cpp);
        // GLContext's accessors are one-line forwards to those, so this block and the pull
        // path answer the same question from the same bytes.
        //
        // This departs from P1 brief D4's "no derivation logic is re-implemented in
        // PipeInputs", deliberately and with a guard: MOBILEGL_PIPE_VERIFY's compare-at-read
        // re-reads every one of these from the live context AT EVERY BACKEND READ and
        // compares field-wise, so a transcription error is caught on the first draw that
        // reads it. RenderStateSpansTest.DerivationMatchesTheFrontendGetters walks every
        // setter and checks all 29 against GLContext on top of that.
        // ----------------------------------------------------------------------------

        // RenderState::IsCapabilityEnabled, transcribed against the assembled block. One of
        // the two derivations that is not a field copy, and the reason D3's three storage
        // holes had to close FIRST: before P2, DepthClamp, FramebufferSrgb and
        // TextureCubeMapSeamless fell to `default: return false` and this could not have
        // been written at all.
        static Bool DeriveCapability(const RenderStateParameters& p, CapabilityInput cap) {
#define MGP_DERIVE_CAPABILITY(capability)                                                                              \
    case CapabilityInput::capability:                                                                                  \
        return p.capability##Enabled;
            switch (cap) {
                MGP_PLAIN_CAPABILITY_LIST(MGP_DERIVE_CAPABILITY)
            // The non-indexed query of an INDEXED capability answers for index 0
            // (GL 4.6 core 22.1) - RenderState.cpp says it in the same words.
            case CapabilityInput::Blend:
                return p.BlendStates[0].Enabled;
            case CapabilityInput::ScissorTest:
                return (p.ScissorTestEnabledMask & 1u) != 0;
            // CapabilityInput lists ClipDistance0..7 contiguously, so the subtraction below
            // is in range for exactly the eight values that reach here - RenderState.cpp's
            // file-local ClipDistanceBit is the same expression.
            case CapabilityInput::ClipDistance0:
            case CapabilityInput::ClipDistance1:
            case CapabilityInput::ClipDistance2:
            case CapabilityInput::ClipDistance3:
            case CapabilityInput::ClipDistance4:
            case CapabilityInput::ClipDistance5:
            case CapabilityInput::ClipDistance6:
            case CapabilityInput::ClipDistance7:
                return (p.ClipDistanceEnabledMask &
                        (1u << (static_cast<Uint>(cap) - static_cast<Uint>(CapabilityInput::ClipDistance0)))) != 0;
            default:
                return false;
            }
#undef MGP_DERIVE_CAPABILITY
        }

        // `chunkBits` names the GLOBAL chunks the scatter that called this actually moved;
        // kMGPipeAllGlobalChunks is the whole-block form. See the guard constants at the top
        // of this file for why the four wide walks are scoped and the scalars are not.
        static void DeriveRenderStateFields(PipeInputs& inputs, Uint32 chunkBits) {
            const RenderStateParameters& p = inputs.m_renderState;

            // Per draw buffer: GetBlendEquationIndexed, GetBlendFuncIndexed,
            // GetColorMaskIndexed and IsCapabilityEnabledIndexed(Blend).
            if ((chunkBits & kChunksBlendLoop) != 0) {
                for (Uint i = 0; i < kMGMaxDrawBuffers; ++i) {
                    const PerBufferBlendState& blend = p.BlendStates[i];
                    inputs.m_blendEquation[i][0] = blend.ColorEquation;
                    inputs.m_blendEquation[i][1] = blend.AlphaEquation;
                    inputs.m_blendFunc[i][0] = blend.SrcFactorRGB;
                    inputs.m_blendFunc[i][1] = blend.DstFactorRGB;
                    inputs.m_blendFunc[i][2] = blend.SrcFactorAlpha;
                    inputs.m_blendFunc[i][3] = blend.DstFactorAlpha;
                    inputs.m_colorMask[i] = p.ColorMasks[i];
                    inputs.m_capabilityIndexed.Blend[i] = blend.Enabled;
                }
            }

            // GetViewportIndexed, and GetViewport: viewport 0 ROUNDED - the other derivation
            // that is not a field copy. glGetIntegerv on floating-point state rounds to
            // nearest (GL 4.6 core 22.2), and truncating a 63.5-wide viewport would also hand
            // the backends a rectangle one pixel short of what was asked for. std::lround,
            // exactly as RenderState::GetViewport does it.
            if ((chunkBits & kChunksViewportLoop) != 0) {
                for (Uint i = 0; i < PipeInputs::kMaxViewports; ++i) {
                    inputs.m_viewportIndexed[i] = p.Viewports[i];
                }
                const FloatVec4& viewport = p.Viewports[0];
                inputs.m_viewport =
                    IntVec4(static_cast<Int>(std::lround(viewport.x())), static_cast<Int>(std::lround(viewport.y())),
                            static_cast<Int>(std::lround(viewport.z())), static_cast<Int>(std::lround(viewport.w())));
            }

            // GetDepthRangeIndexed. Its own chunk (D2) - a glClearColor moves that chunk and
            // a glViewport does not, so it cannot ride with the viewports.
            if ((chunkBits & kChunksDepthRangeLoop) != 0) {
                for (Uint i = 0; i < PipeInputs::kMaxViewports; ++i) {
                    inputs.m_depthRange[i] = p.DepthRanges[i];
                }
            }

            // IsCapabilityEnabledIndexed(ScissorTest): 16 bits of one pipeline word.
            if ((chunkBits & kChunksScissorEnableLoop) != 0) {
                for (Uint i = 0; i < PipeInputs::kMaxViewports; ++i) {
                    inputs.m_capabilityIndexed.ScissorTest[i] = (p.ScissorTestEnabledMask & (1u << i)) != 0;
                }
            }

            // The scalar copies, in the order MGP_COVERAGE_EMITTED_LIST names them.
            inputs.m_blendColor = p.BlendColor;
            inputs.m_clampReadColor = p.ClampReadColor;
            inputs.m_clearColor = p.ClearColor;
            inputs.m_clearDepth = p.ClearDepth;
            inputs.m_clearStencil = p.ClearStencil;
            inputs.m_cullFaceMode = p.CullFaceModeSetting;
            inputs.m_depthFunc = p.DepthFunc;
            inputs.m_depthMask = p.DepthMask;
            inputs.m_lineWidth = p.LineWidth;
            inputs.m_logicOp = p.LogicOp;
            inputs.m_minSampleShadingValue = p.MinSampleShadingValue;
            inputs.m_patchDefaultInnerLevel = p.PatchDefaultInnerLevel;
            inputs.m_patchDefaultOuterLevel = p.PatchDefaultOuterLevel;
            inputs.m_patchVertices = p.PatchVertices;
            inputs.m_polygonModeFront = p.PolygonModeFront;
            inputs.m_polygonOffsetFactor = p.PolygonOffsetFactor;
            inputs.m_polygonOffsetUnits = p.PolygonOffsetUnits;
            inputs.m_primitiveRestartIndex = p.PrimitiveRestartIndex;
            inputs.m_provokingVertexMode = p.ProvokingVertexModeSetting;
            // GetScissorBox answers for rectangle 0, like GetViewport - but WITHOUT any
            // rounding, because the scissor rectangle is integer state to begin with.
            inputs.m_scissorBox = p.ScissorBoxes[0];

            // GetStencilState: Front is index 0 and Back is index 1 on both sides
            // (RenderState.cpp's GetStencilFaceIndex and PipeInputs::GetStencilState agree),
            // so the two faces copy straight across.
            for (SizeT face = 0; face < PipeInputs::kStencilFaceCount; ++face) {
                inputs.m_stencil[face] = p.StencilStates[face];
            }

            // The 35-arm switch, dispatched 35 times. The widest single thing the derivation
            // does, and the one a per-frame glViewport most obviously must not pay for.
            if ((chunkBits & kChunksCapabilityWalk) != 0) {
                for (SizeT i = 0; i < PipeInputs::kCapabilityCount; ++i) {
                    inputs.m_capability[i] = DeriveCapability(p, static_cast<CapabilityInput>(i));
                }
            }
        }
    };

    namespace {
        // CapabilityInput in enum order, so the residual block's bit i and this name agree by
        // construction. The static_assert below is what makes a capability added to the enum
        // without a name here a build break rather than an "<unknown>" in a Fatal line.
        constexpr const char* kCapabilityNames[] = {
            "Blend",
            "ClipDistance0",
            "ClipDistance1",
            "ClipDistance2",
            "ClipDistance3",
            "ClipDistance4",
            "ClipDistance5",
            "ClipDistance6",
            "ClipDistance7",
            "ColorLogicOp",
            "CullFace",
            "DebugOutput",
            "DebugOutputSynchronous",
            "DepthClamp",
            "DepthTest",
            "Dither",
            "FramebufferSrgb",
            "LineSmooth",
            "Multisample",
            "PolygonOffsetFill",
            "PolygonOffsetLine",
            "PolygonOffsetPoint",
            "PolygonSmooth",
            "PrimitiveRestart",
            "PrimitiveRestartFixedIndex",
            "RasterizerDiscard",
            "SampleAlphaToCoverage",
            "SampleAlphaToOne",
            "SampleCoverage",
            "SampleShading",
            "SampleMask",
            "ScissorTest",
            "StencilTest",
            "TextureCubeMapSeamless",
            "ProgramPointSize",
        };
        constexpr SizeT kCapabilityCount = static_cast<SizeT>(CapabilityInput::CapabilityInputCount);
        static_assert(sizeof(kCapabilityNames) / sizeof(kCapabilityNames[0]) == kCapabilityCount,
                      "CapabilityInput gained a value; name it here or the residual trip wire "
                      "cannot say which capability diverged");
        static_assert(kCapabilityCount <= 64,
                      "ResidualValueBlock::CapabilityBits is a Uint64; 35 bits fit, 65 would not");

        // A REFERENCE TO A NEVER-DESTROYED BLOCK, for MGPipeSlots()' reason
        // (MG_Impl/Pipe/SlotAllocator.cpp): resource_destroy and delete_vertex_elements are
        // raised from ~BufferObject / ~VertexArrayObject, and those objects are released by
        // exit handlers that run after this translation unit's own globals are gone.
        MGPipeApplierState& g_applier = *new MGPipeApplierState{};

        // The installed handle-shaped resource table. Null until a backend registers one,
        // which is what makes the client half landable on its own: with nothing here every
        // frontend dispatch falls through to the op table this one replaces, and the tree
        // behaves exactly as it did.
        const MGPipeResourceOps* g_resourceOps = nullptr;

        MGPipeRenderStateCsoRecord* FindCso(MGPipeHandle handle) {
            if (handle.Slot >= g_applier.RenderStateCsos.size()) return nullptr;
            MGPipeRenderStateCsoRecord& record = g_applier.RenderStateCsos[handle.Slot];
            if (!record.Live || record.Gen != handle.Gen) return nullptr;
            return &record;
        }

        constexpr Uint32 kAllPipelineChunks =
            static_cast<Uint32>((Uint64{1} << kMGPipePipelineChunkCount) - 1);

        // ----------------------------------------------------------------------------
        // P3a: resolving a handle, growing a slot table, and the bounds gate.
        // ----------------------------------------------------------------------------

        // Grows a slot-indexed record table so `slot` is in it, or returns NULL when the slot
        // is outside the table's bound. Slot spaces are DENSE per kind - the allocator is a
        // free list plus a high-water mark - which is exactly why the server's object table is
        // an array a handle indexes rather than a map, and why this grows only when a new
        // high-water mark arrives.
        //
        // AND WHY IT IS BOUNDED. `slot` is a client-supplied Uint32 that arrives in a payload,
        // and this is the one number in the family that reaches an ALLOCATOR: unbounded, a
        // corrupt 0xFFFFFFFE asks for a four-billion-entry vector from inside the same commit
        // that polices Blob.Size, the destination range, Level, RegionCount and Start + Count.
        // The bounds are kMGPipeMax{Resource,VertexElements}Slots (PipeApply.h) and a slot at
        // or above one is the callers' Fatal{ProtocolCorruption}, with the identity in the
        // line like its siblings - never a resize.
        template <class Record>
        Record* RecordAt(Vector<Record>& records, Uint32 slot, Uint32 slotLimit) {
            if (slot >= slotLimit) return nullptr;
            if (slot >= records.size()) records.resize(static_cast<SizeT>(slot) + 1);
            return &records[slot];
        }

        // Null means "this applier does not have that resource": an out-of-range slot, a slot
        // that is not live, or a handle whose generation has moved on because the slot was
        // recycled under it. FindCso above is the same three questions for the CSO store.
        MGPipeResourceRecord* FindResource(MGPipeHandle res) {
            if (res.Slot >= g_applier.Resources.size()) return nullptr;
            MGPipeResourceRecord& record = g_applier.Resources[res.Slot];
            if (!record.Live || record.Gen != res.Gen) return nullptr;
            return &record;
        }

        MGPipeVertexElementsRecord* FindVertexElements(MGPipeHandle cso) {
            if (cso.Slot >= g_applier.VertexElementsCsos.size()) return nullptr;
            MGPipeVertexElementsRecord& record = g_applier.VertexElementsCsos[cso.Slot];
            if (!record.Live || record.Gen != cso.Gen) return nullptr;
            return &record;
        }

        // ----------------------------------------------------------------------------
        // P4a: THE SAME THREE QUESTIONS, ASKED OF A TABLE RATHER THAN OF THE ONE TABLE.
        //
        // The slot spaces of kinds Buffer, Texture and Renderbuffer are INDEPENDENT - the
        // allocator is per kind - so three different live objects can hold slot 7 at once and
        // one slot-indexed table would alias all three onto one record. The record TYPE is
        // shared, because the descriptor is one discriminated descriptor; only the table is
        // per kind, and choosing it is what the two selectors below do.
        // ----------------------------------------------------------------------------
        template <class Record>
        Record* FindIn(Vector<Record>& records, MGPipeHandle handle) {
            if (handle.Slot >= records.size()) return nullptr;
            Record& record = records[handle.Slot];
            if (!record.Live || record.Gen != handle.Gen) return nullptr;
            return &record;
        }

        // Null means "this is not a resource target this catalogue names", which is a corrupt
        // descriptor rather than an unknown object: acting on the wrong table would create,
        // respecify or destroy an unrelated LIVE object that happens to hold the same slot in
        // another kind's space, and that is the "act outside its own storage" class.
        Vector<MGPipeResourceRecord>* ResourceTableForTarget(Uint8 target) {
            if (target == kMGPipeResourceTargetBuffer) return &g_applier.Resources;
            if (target == static_cast<Uint8>(MGPipeResourceTarget::Renderbuffer)) {
                return &g_applier.RenderbufferResources;
            }
            if (target < static_cast<Uint8>(MGPipeResourceTarget::Count)) {
                // Every remaining enumerator is a texture target, and they share one table
                // because they share one kind: MGPipeKind::Texture. Tex1D..TexCubeArray,
                // Tex2DMS/MSArray, TexBuffer and TexRect are all one slot space.
                return &g_applier.TextureResources;
            }
            return nullptr;
        }

        // resource_destroy carries no descriptor, so its discriminator is the handle's KIND.
        Vector<MGPipeResourceRecord>* ResourceTableForKind(Uint32 kind) {
            switch (static_cast<MGPipeKind>(kind)) {
            case MGPipeKind::Buffer:
                return &g_applier.Resources;
            case MGPipeKind::Texture:
                return &g_applier.TextureResources;
            case MGPipeKind::Renderbuffer:
                return &g_applier.RenderbufferResources;
            default:
                return nullptr;
            }
        }

        // A sub-data record's own discriminator, and it is DELIBERATELY NOT the table selector
        // above: MGPSubData::Target carries the UPLOAD target as well - a cube face is one, and
        // those are not MGPipeResourceTarget enumerators - so the buffer question is asked of
        // the whole field, which is the one question that has an answer for every value.
        // kMGPipeResourceTargetBuffer is 0, a buffer has no upload target to pack beside it, and
        // no texture emission leaves the whole field 0: that is the contract the emitter is
        // held to.
        Bool SubDataNamesABuffer(const MGPSubData& record) {
            return record.Target == kMGPipeResourceTargetBuffer;
        }

        // THE VALUE SPACE OF MGPSubData::Target IS THE CONTRACT'S, AND THIS FILE NO LONGER
        // RESTATES IT. c0c (ID-12, the DV-3 seam) moved the encoding into MGPipeTypes.h as
        // MGPipePackSubDataTarget / MGPipeSubDataResourceTargetOf / MGPipeSubDataUploadTargetOf,
        // so wire v2's local `SubDataResourceTargetOf` - written because those helpers were not
        // on that round's base - is gone and every half-read below goes through the contract's
        // accessor. Nothing here open-codes `& 0xFF`.
        //
        // The applier still matches the WHOLE field for the buffer question above, exactly as
        // ID-12 says it does, and c0c's own static_assert is what keeps the two readings in
        // step: a buffer record's Target is exactly kMGPipeResourceTargetBuffer.

        // ID-18 M4: DOES THIS RESPECIFY REDEFINE ANY STORAGE, OR IS IT CARRYING METADATA?
        //
        // The question exists because a sticky BindMask / ImageBindableHint bit has exactly one
        // way onto the wire - a respecify - and an IMMUTABLE texture has no further respecify to
        // ride on. So B re-publishes the descriptor when the mask moves, and the applier has to
        // tell that call apart from a real redefinition: the first must NOT drop the pending
        // uploads standing against the storage (nothing replaced them), the second must.
        //
        // THE COMPARISON IS FIELD BY FIELD AND NOT A memcmp. MGPResourceDesc carries Pad0/Pad1
        // and neither side of this comparison is guaranteed to have written them - the client
        // fills a stack descriptor field by field - so a byte comparison of 88 bytes is a coin
        // flip on padding, which is the defect clientsp's SamplerParameters cache already paid
        // for once.
        //
        // WHAT IS NOT COMPARED IS THE SHORT LIST AND IT IS THE DEFINITION: BindMask and
        // ImageBindableHint (the two this call exists to carry), GlNameForDiag (diagnostics, and
        // never an identity - section 4.2.1) and the two padding words. EVERYTHING ELSE IS
        // COMPARED, which is deliberately WIDER than "the fields that define an allocation":
        // being wide can only mis-classify a metadata update as a redefinition, whose cost is
        // one level's pending upload dropped by a call B does not emit; being narrow would
        // mis-classify a REDEFINITION as metadata and keep boxes in a coordinate system that no
        // longer exists, which is an upload past the end of the new level. The asymmetry decides
        // the direction.
        //
        // A BUFFER IS NEVER METADATA-ONLY, and that is a rule rather than a consequence.
        // glBufferData at an unchanged size is the canonical ORPHANING idiom - a real
        // reallocation whose whole purpose is that the old store is gone - and glBufferStorage
        // is the one entry point in the catalogue allowed a synchronous acknowledgement
        // (MGPipeResourceRespecifyNeedsAck). Excluding the target here is what makes ID-18 M4's
        // "no reallocation ack" true by construction instead of by a second suppression rule,
        // and it costs nothing: the mask that M4 is about is a TEXTURE's (and a renderbuffer's),
        // and a buffer has no pending upload for the arm to protect anyway.
        Bool RespecifyRedefinesNoStorage(const MGPResourceDesc& stored, const MGPResourceDesc& next) {
            if (next.Target == kMGPipeResourceTargetBuffer) return false;
            if (stored.Target != next.Target) return false;
            if (stored.StorageKind != next.StorageKind) return false;
            if (stored.InternalFormat != next.InternalFormat) return false;
            if (stored.Width != next.Width || stored.Height != next.Height ||
                stored.Depth != next.Depth) {
                return false;
            }
            if (stored.ArrayLayers != next.ArrayLayers || stored.Levels != next.Levels ||
                stored.Samples != next.Samples) {
                return false;
            }
            if (stored.FixedSampleLocations != next.FixedSampleLocations) return false;
            if (stored.Immutable != next.Immutable) return false;
            if (stored.HasDefinedContent != next.HasDefinedContent) return false;
            if (stored.Usage != next.Usage || stored.StorageFlags != next.StorageFlags) return false;
            // The view and buffer-texture fields: a texture view's storage OWNER and a buffer
            // texture's backing range are its storage exactly as an extent is one, and
            // re-pointing either is a redefinition however unchanged the extent looks.
            if (!(stored.ViewOf == next.ViewOf)) return false;
            if (!(stored.BufferForTexBuffer == next.BufferForTexBuffer)) return false;
            if (stored.BufOffset != next.BufOffset || stored.BufSize != next.BufSize) return false;
            return true;
        }

        // WHY A DEAD HANDLE IS NOT A TRIP WIRE HERE, and the bounds faults below are - and
        // why it is nonetheless COUNTED rather than silently dropped.
        //
        // A make-current no longer takes the records with it (MGPipeApplierReset), so the
        // shared-store case that used to arrive here every context switch does not arrive at
        // all: a buffer that outlives a switch keeps its record and its writes keep landing.
        // What is left is (a) a genuine protocol error - an unknown slot, a stale generation -
        // and (b) ONE legal sequence, which is why this is still a no-op and not a wire:
        //
        //     the served context is torn down
        //       -> MGPipeApplierReleaseObjectRecords()          (the applier goes away with it)
        //       -> ~BufferObject / ~VertexArrayObject for every object the context still owns
        //       -> resource_destroy / delete_vertex_elements, each naming a record that the
        //          line above has already dropped.
        //
        // Every one of those death notices is legal, unavoidable and arrives after the
        // records are gone, and a wire here would abort a verify lane on the ordinary shutdown
        // of a context. So the refusal stays a DEFINED no-op - nothing stored, nothing
        // dispatched, no serial moved, and the debug assertion names it, which is the shape
        // bind_render_state already uses for a dead CSO.
        //
        // But MOBILEGL_ASSERT compiles out at INFO, which is what all three gate builds and
        // every shipped build are, so a no-op alone would make case (a) - a dropped
        // glBufferSubData - invisible in every build that matters. Both refusal paths
        // therefore go through ResolveResource / ResolveVertexElements below, which COUNT into
        // MGPipeApplierState::Refused{Resource,VertexInput}Calls. That is the observable: a
        // legal sequence leaves it at 0 and a refused call moves it, in every build.
        //
        // The faults below are the other class entirely: a record that does not describe its
        // own bytes would have the BACKEND read or write outside a store, which is memory
        // corruption rather than a dropped call, so those get the tag ARCHITECTURE.md reserves
        // for exactly this - Fatal{ProtocolCorruption} - and the record's identity in the line.
        constexpr const char* kResourceRefusalNote =
            "the record is not this applier's; the call is dropped, not applied";

        // The two resolvers every entry point below uses. One place resolves, asserts and
        // counts, so a call that forgets one of the three cannot exist.
        MGPipeResourceRecord* ResolveResource(const char* call, MGPipeHandle res) {
            MGPipeResourceRecord* record = FindResource(res);
            MOBILEGL_ASSERT(record != nullptr, "%s named {slot=%u, gen=%u}: %s", call, res.Slot, res.Gen,
                            kResourceRefusalNote);
            if (record == nullptr) ++g_applier.RefusedResourceCalls;
            return record;
        }

        // P4a: the same resolver over a chosen table. A texture's and a renderbuffer's
        // resource_* calls are RESOURCE calls and count into the resource counter, exactly as a
        // buffer's do; the object counter beside it is for the five families that have no
        // resource call at all (framebuffer, sampler, sampler view, program, texture params).
        MGPipeResourceRecord* ResolveResourceIn(Vector<MGPipeResourceRecord>& table, const char* call,
                                               MGPipeHandle res) {
            MGPipeResourceRecord* record = FindIn(table, res);
            MOBILEGL_ASSERT(record != nullptr, "%s named {slot=%u, gen=%u}: %s", call, res.Slot, res.Gen,
                            kResourceRefusalNote);
            if (record == nullptr) ++g_applier.RefusedResourceCalls;
            return record;
        }

        // P4a's object families. ONE counter for the five, because they share one legal refusal
        // sequence - the teardown order beside kResourceRefusalNote - and because what an
        // operator reading a log needs to know is that an object call was dropped; the line
        // itself names which call and which handle.
        template <class Record>
        Record* ResolveObject(Vector<Record>& table, const char* call, MGPipeHandle handle) {
            Record* record = FindIn(table, handle);
            MOBILEGL_ASSERT(record != nullptr, "%s named {slot=%u, gen=%u}: %s", call, handle.Slot,
                            handle.Gen, kResourceRefusalNote);
            if (record == nullptr) ++g_applier.RefusedObjectCalls;
            return record;
        }

        MGPipeVertexElementsRecord* ResolveVertexElements(const char* call, MGPipeHandle cso) {
            MGPipeVertexElementsRecord* record = FindVertexElements(cso);
            MOBILEGL_ASSERT(record != nullptr, "%s named {slot=%u, gen=%u}: %s", call, cso.Slot, cso.Gen,
                            kResourceRefusalNote);
            if (record == nullptr) ++g_applier.RefusedVertexInputCalls;
            return record;
        }

        // Returns the fault, or null when [offset, offset+size) lies inside `width` bytes.
        // Written so nothing can overflow: `offset > width` is answered before the subtraction
        // that the second question needs.
        const char* BufferRangeFault(Uint64 offset, Uint64 size, Uint64 width) {
            if (offset > width) return "the offset starts past the resource's declared storage";
            if (size > width - offset) return "the range runs past the resource's declared storage";
            return nullptr;
        }

        // The buffer half of MGPSubData is a CONVENTION over a texture record's box
        // (MGPipeTypes.h): offset in UnionBox.X, size in UnionBox.W, no level and no regions.
        // MGPipeSetSubDataBufferRange is its only encoder, so every field it writes is a field
        // the applier can hold the record to - which is what makes a hand-rolled or corrupted
        // record visible instead of being read as a plausible range.
        //
        // P4a SPLIT IT IN TWO RATHER THAN WIDENING IT. Every statement below is a statement
        // about the BUFFER convention - "no level", "no sub-regions", "the blob is the box's
        // own byte size" - and every one of them is false for a texture, which carries a real
        // level, a real region list and a blob whose length no other field describes. A single
        // predicate that tried to hold both would have to be right about which record it was
        // looking at anyway, so the branch is at the call and each half states only what it
        // can actually check. SubDataTextureFault is the other half.
        const char* SubDataBoxFault(const MGPSubData& record) {
            // The box's first coordinate is a signed Int32 on the wire and the encoder never
            // writes a negative one; read back as unsigned (which is what the decoder does,
            // deliberately, rather than sign-extending) a corrupt one lands above the
            // encodable bound and is refused here.
            if (MGPipeSubDataBufferOffset(record) > 0x7FFFFFFFull) {
                return "the destination offset is above the bound one record can encode";
            }
            if (record.Level != 0) return "the buffer half carries a mip level";
            if (record.RegionCount != 0) return "the buffer half carries sub-regions";
            // THE BLOB RULE, and it is the SAME rule create_vertex_elements is held to
            // (MGPipeTypes.h states it on both records): a declared blob length must be
            // exactly the byte length the record's other fields describe, and a length of 0
            // means "this record does not declare its blob" - which is what a monolith
            // emission is, because the bytes travel beside the record through the entry
            // point's companion pointer. So the gate is inert while the client leaves the
            // field zero and becomes a real one on the first record a transport truncates.
            if (record.Blob.Size != 0 && record.Blob.Size != MGPipeSubDataBufferSize(record)) {
                return "the declared blob length is not the record's own byte size";
            }
            return nullptr;
        }

        // ----------------------------------------------------------------------------
        // P4a: the texture half of the sub-data validator, and the pending-upload set.
        // ----------------------------------------------------------------------------

        // A box is EMPTY when any extent is zero, and an empty box unions to nothing. Written
        // once because the accumulation below needs it in three places.
        Bool BoxIsEmpty(const MGPBox& box) { return box.W == 0 || box.H == 0 || box.D == 0; }

        // A box stays inside the range one record can encode: the origin is a signed Int32 and
        // the extent an unsigned Uint32, so an origin plus an extent that leaves the positive
        // Int32 range is a record no encoder writes and is refused before any arithmetic below
        // has to survive it. It is the same bound, and the same reason, as the buffer half's
        // "above the bound one record can encode".
        Bool BoxIsEncodable(Int32 origin, Uint32 extent) {
            return origin >= 0 && static_cast<Int64>(origin) + static_cast<Int64>(extent) <= 0x7FFFFFFF;
        }

        Bool BoxIsEncodable(const MGPBox& box) {
            return BoxIsEncodable(box.X, box.W) && BoxIsEncodable(box.Y, box.H) &&
                   BoxIsEncodable(box.Z, box.D);
        }

        Bool AxisContains(Int32 outerOrigin, Uint32 outerExtent, Int32 innerOrigin, Uint32 innerExtent) {
            const Int64 outerEnd = static_cast<Int64>(outerOrigin) + static_cast<Int64>(outerExtent);
            const Int64 innerEnd = static_cast<Int64>(innerOrigin) + static_cast<Int64>(innerExtent);
            return innerOrigin >= outerOrigin && innerEnd <= outerEnd;
        }

        // THE ONE INVARIANT THE TEXTURE HALF CAN ACTUALLY CHECK, and it is the one that matters:
        // the union box IS the union of the regions (MipmapStorage maintains both through one
        // MarkDirtyRegion, and "0 regions" means "the box is the whole story"). The SERVER picks
        // the upload shape from the pair - one box job, or N rect jobs, and Mali prices that
        // choice at ~6 ms/frame - so a region outside the box means the two shapes describe
        // different texels and whichever the server picks is wrong: the box misses the region's
        // texels, and the region writes where the box never said it would.
        const char* SubDataTextureFault(const MGPSubData& record, const MGPSubRegion* regions) {
            if (record.Level >= kMGPipeMaxTextureLevels) {
                return "the level is above the bound any texture's storage can have";
            }
            if (!BoxIsEncodable(record.UnionBox)) {
                return "the union box has a negative origin or runs past the bound one record can encode";
            }
            if (record.RegionCount > kMGPipeMaxPendingUploadRegions) {
                return "the record declares more sub-regions than one upload may carry";
            }
            if (record.RegionCount != 0 && regions == nullptr) {
                return "the record declares sub-regions and carries none";
            }
            for (Uint32 i = 0; i < record.RegionCount; ++i) {
                const MGPSubRegion& region = regions[i];
                if (!BoxIsEncodable(region.X, region.W) || !BoxIsEncodable(region.Y, region.H) ||
                    !BoxIsEncodable(region.Z, region.D)) {
                    return "a sub-region has a negative origin or runs past the bound one record can encode";
                }
                if (!AxisContains(record.UnionBox.X, record.UnionBox.W, region.X, region.W) ||
                    !AxisContains(record.UnionBox.Y, record.UnionBox.H, region.Y, region.H) ||
                    !AxisContains(record.UnionBox.Z, record.UnionBox.D, region.Z, region.D)) {
                    return "a sub-region is not inside the union box the record declares";
                }
            }
            // WHAT IS DELIBERATELY NOT CHECKED, so that a later reader does not add it back as
            // an oversight. (a) The level against MGPResourceDesc::Levels: a MUTABLE texture
            // defines its levels one glTexImage2D at a time, so the descriptor's level count is
            // not an upper bound at every instant and a gate on it would refuse a legal upload
            // to a level the next respecify is about to declare. (b) The box against the
            // descriptor's extents: the record addresses the LEVEL's coordinate system, and a
            // view remaps that space, so the arithmetic is the storage owner's and not this
            // applier's. (c) The blob: no field of a texture record describes its own byte
            // length - the strides are per region and the level shadow's size is not carried -
            // so the one Blob rule has nothing to cross-check here and stays inert by
            // construction rather than by omission.
            return nullptr;
        }

        MGPBox UnionOfBoxes(const MGPBox& a, const MGPBox& b) {
            if (BoxIsEmpty(a)) return b;
            if (BoxIsEmpty(b)) return a;
            const Int32 x = a.X < b.X ? a.X : b.X;
            const Int32 y = a.Y < b.Y ? a.Y : b.Y;
            const Int32 z = a.Z < b.Z ? a.Z : b.Z;
            const Int64 xEnd = std::max(static_cast<Int64>(a.X) + a.W, static_cast<Int64>(b.X) + b.W);
            const Int64 yEnd = std::max(static_cast<Int64>(a.Y) + a.H, static_cast<Int64>(b.Y) + b.H);
            const Int64 zEnd = std::max(static_cast<Int64>(a.Z) + a.D, static_cast<Int64>(b.Z) + b.D);
            return MGPBox{x, y, z, static_cast<Uint32>(xEnd - x), static_cast<Uint32>(yEnd - y),
                          static_cast<Uint32>(zEnd - z)};
        }

        // D-D5's SAFETY NET, and the whole reason it is server-side state. The client clears
        // its own dirty flags AT EMISSION, for the levels whose record this applier accepted;
        // Espryt's upload loop has bail arms - an incomplete texture returns early, a
        // multisample target refreshes and skips - that today leave the frontend flag set, so a
        // naive move of the clear to the client would lose exactly those texels. The emitted
        // shape accumulates here instead, it survives any number of bails, and Espryt consumes
        // and clears an entry only where it actually uploads.
        //
        // ACCUMULATION IS THE CLIENT'S OWN MODEL, ONE LEVEL UP. MipmapStorage keeps a union box
        // and, behind it, a bounded disjoint rect list, and answers "0 rects" for everything it
        // cannot describe that way - which means "upload the box instead" and covers every
        // reason at once. So: boxes union; rect lists concatenate; and the moment either side
        // says "box only", or the list would outgrow its bound, the entry becomes box only.
        // Never a dropped region - the box still covers every texel the dropped list named.
        // Returns false when the record names more distinct (target, level) keys than any
        // texture can have, which the caller reports as a corrupt record.
        Bool AccumulatePendingUpload(MGPipeResourceRecord& stored, const MGPSubData& record,
                                     const MGPSubRegion* regions) {
            for (MGPipeResourceRecord::PendingUpload& entry : stored.PendingUploads) {
                if (entry.UploadTarget != record.Target || entry.Level != record.Level) continue;
                entry.UnionBox = UnionOfBoxes(entry.UnionBox, record.UnionBox);
                if (record.RegionCount == 0 || entry.Regions.empty() ||
                    static_cast<Uint64>(entry.Regions.size()) + record.RegionCount >
                        kMGPipeMaxPendingUploadRegions) {
                    entry.Regions.clear();
                    return true;
                }
                entry.Regions.insert(entry.Regions.end(), regions, regions + record.RegionCount);
                return true;
            }
            if (stored.PendingUploads.size() >= kMGPipeMaxPendingUploads) return false;
            MGPipeResourceRecord::PendingUpload entry;
            entry.UploadTarget = record.Target;
            entry.Level = record.Level;
            entry.UnionBox = record.UnionBox;
            // A first contribution with no regions makes the entry BOX ONLY from the start, and
            // that is why an empty list above means box only rather than "not filled in yet":
            // an entry is never created without its first contribution.
            if (record.RegionCount != 0) entry.Regions.assign(regions, regions + record.RegionCount);
            stored.PendingUploads.push_back(std::move(entry));
            return true;
        }

#if MOBILEGL_PIPE_VERIFY && !MOBILEGL_BUILD_DISAGGREGATED
        // D-A4's pin, AND P5 (b1) IS THE PHASE IT WAS WAITING FOR. It said "HasLiveHostWrites
        // is always false in this phase and is written by nobody: it exists so the phase that
        // pushes persistent-mapped host writes can set it with no new record kind", and a
        // verify build refused to let such a producer arrive unannounced.
        //
        // The producer is announced: MGPSubData::HasLiveHostWrites, set by
        // MGPipeEmitResourceSubData from BufferObject::HasLiveHostWritesForWire and read by
        // ApplyBufferWrite below. So the pin is LIFTED FOR A SPLIT BUILD ONLY, and left
        // standing everywhere else - in a monolith build nothing sets the bit and the wire is
        // still the thing that would say so if something started to. That is the whole value
        // of the pin and it survives the phase it was written for.
        void PinNoLiveHostWrites(const MGPipeResourceRecord& record, MGPipeHandle res, const char* call) {
            if (!record.HasLiveHostWrites) return;
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("PipeLiveHostWrites")
                                 " %s {slot=%u, gen=%u}: the resource record says host writes are live, and "
                                 "no path in this phase may set that",
                                 call, res.Slot, res.Gen);
        }
#else
        void PinNoLiveHostWrites(const MGPipeResourceRecord&, MGPipeHandle, const char*) {}
#endif

#if MOBILEGL_PIPE_VERIFY
        // P5's pin, the same shape and for the same reason. The respecify SCOPE now has a wire
        // carrier (MGPResourceDesc's HasRespecifiedLevel + the pair) and no producer: every
        // descriptor P5 builds is whole-resource, and the per-level scope still arrives the old
        // way, as the trailing MGPRespecifiedLevel* this function does not look at.
        //
        // The two must not disagree, and when a later package wires the carrier it will set the
        // fields at a call site that also still passes the pointer - so the first thing that can
        // go wrong is exactly one of the two moving. A verify build refuses to let that arrive
        // unannounced, because a descriptor that says "whole resource" while the pointer says
        // "level 1" drops every other level's pending upload with nothing saying so.
        void PinWholeResourceRespecifyScope(const MGPResourceDesc& desc, MGPipeHandle res, const char* call) {
            if (MGPipeRespecifyIsWholeResource(desc)) return;
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("PipeRespecifyScope")
                                 " %s {slot=%u, gen=%u}: the descriptor carries a per-level respecify "
                                 "scope (target=%u, level=%u), and no path in this phase may set one",
                                 call, res.Slot, res.Gen,
                                 static_cast<unsigned>(MGPipeRespecifiedUploadTargetOf(desc)),
                                 static_cast<unsigned>(MGPipeRespecifiedLevelOf(desc)));
        }
#else
        void PinWholeResourceRespecifyScope(const MGPResourceDesc&, MGPipeHandle, const char*) {}
#endif

#if MOBILEGL_PIPE_VERIFY
        // P5 b1's wire, and the one statement about HasLiveHostWrites that IS still always
        // true once the field has a producer: the bit is the BUFFER family's. It answers "does
        // this resource have a live host writer right now", which only a buffer can have - a
        // texture's host writes are the unpack path's and are announced by the upload itself -
        // and the two emitters that set it (MGPipeEmitResourceSubData,
        // MGPipeEmitBufferSubDataResident) are both buffer-only. A texture sub-data carrying
        // it is therefore an encoder that copied a field across payload halves, which is
        // precisely the class of defect a shared record type invites.
        //
        // It replaces the always-false pin in a split build. That pin could not survive the
        // producer it existed to announce; this one can, and unlike that one it is compiled in
        // EVERY verify build, including build-verify-split - the matrix cell where the field
        // can actually be non-zero.
        void PinLiveHostWritesNamesABuffer(const MGPSubData& record, const char* call) {
            if (record.HasLiveHostWrites == 0) return;
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("PipeLiveHostWritesTarget")
                                 " %s {slot=%u, gen=%u}: a record whose resource target is not a "
                                 "buffer (target=%u) declares live host writes, and only the buffer "
                                 "family has any",
                                 call, record.Res.Slot, record.Res.Gen, record.Target);
        }
#else
        void PinLiveHostWritesNamesABuffer(const MGPSubData&, const char*) {}
#endif

        // The one gate every content-carrying buffer write goes through. resource_subdata and
        // buffer_subdata_resident differ only in which backend hook takes the bytes and in the
        // fact that one of them is allowed to be absent, so a second copy of this arithmetic
        // would be a second place to get it wrong.
        // RETURNS THE ACCEPTANCE, for D-D5 step 1's reason: the emitter clears its own dirty
        // state on the strength of this answer and neither refusal path is otherwise visible
        // from the call site (a dead handle is a counted no-op, a corrupt record is a Fatal
        // that deliberately moves no counter).
        Bool ApplyBufferWrite(const char* call, const MGPSubData& record, const void* bytes, Bool resident) {
            MGPipeResourceRecord* stored = ResolveResource(call, record.Res);
            if (stored == nullptr) return false;

            const Uint64 offset = MGPipeSubDataBufferOffset(record);
            const Uint64 size = MGPipeSubDataBufferSize(record);
            const char* fault = SubDataBoxFault(record);
            if (fault == nullptr) fault = BufferRangeFault(offset, size, stored->Desc.Width);
            if (fault == nullptr && size != 0 && bytes == nullptr) {
                fault = "a non-empty write carries no bytes";
            }
            if (fault != nullptr) {
                MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                     " %s {slot=%u, gen=%u, glName=%u}: %s (offset=%llu, size=%llu, "
                                     "storage=%u bytes)",
                                     call, record.Res.Slot, record.Res.Gen, stored->Desc.GlNameForDiag,
                                     fault, static_cast<unsigned long long>(offset),
                                     static_cast<unsigned long long>(size), stored->Desc.Width);
                return false;
            }
            PinNoLiveHostWrites(*stored, record.Res, call);
#if MOBILEGL_BUILD_DISAGGREGATED
            // P5 (b1): THE PRODUCER. The record's own statement about the resource, applied
            // before the serial moves so that a probe re-entered from inside the backend hook
            // below already sees it. It is an assignment and not an OR: the bit is a STATE,
            // and a content record emitted while nothing maps the buffer is exactly how the
            // state goes back to false - which is why the falling edge pushes one block
            // (BufferObject::NotePersistentMapStateChanged) rather than relying on the next
            // ordinary write to arrive.
            stored->HasLiveHostWrites = record.HasLiveHostWrites != 0;
#endif

            // THE SERIAL MOVES BEFORE THE BACKEND IS TOLD, and that order is load-bearing:
            // the backend stamps its own synced serial from this record inside the hook, so a
            // bump afterwards would leave the twin stamped one mutation behind and the next
            // draw would re-upload what it had just landed.
            ++stored->Serial;

            // THE RANGE IS LANDED FROM HERE ON, so every path below returns true: whether a
            // backend table is registered, and whether it implements the optional resident
            // hook, is a property of the BUILD and not of the record. An emitter that read
            // "not accepted" off an unregistered table would keep re-sending a write the
            // applier has already taken responsibility for.
            if (g_resourceOps == nullptr) return true;
            if (resident) {
                // kOptional, and the frontend already checks the same way for the table this
                // one replaces: a backend that does not implement the resident path leaves the
                // member null and the write is landed by its ordinary sub-data route instead.
                if (g_resourceOps->SubDataResident != nullptr) {
                    g_resourceOps->SubDataResident(record.Res, record, bytes);
                }
                return true;
            }
            if (g_resourceOps->SubData != nullptr) g_resourceOps->SubData(record.Res, record, bytes);
            return true;
        }

        // The texture half of resource_subdata, and it DISPATCHES TO NOBODY at GL-call time:
        // a texture write marks a level dirty and Espryt uploads it at its own sync point, out
        // of the accumulated set below. So the whole of this function is the gate, the
        // accumulation, the serial and - P5c (tx), disaggregated builds only - the adoption
        // hook that moves the staged bytes into the server's staged-texture store while they
        // are still alive. MGPipeResourceOps grew its three texture members for exactly that
        // hook; the monolith shape of everything above them is unchanged.
        Bool ApplyTextureUpload(const MGPSubData& record, const void* bytes, const MGPSubRegion* regions) {
            MGPipeResourceRecord* stored =
                ResolveResourceIn(g_applier.TextureResources, "resource_subdata", record.Res);
            if (stored == nullptr) return false;

            const char* fault = SubDataTextureFault(record, regions);
            // A whole-level upload declares no regions and a non-empty box; a record that
            // declares neither has nothing to upload and nothing to accumulate, which is a
            // shape the drain list cannot produce.
            if (fault == nullptr && BoxIsEmpty(record.UnionBox) && record.RegionCount == 0) {
                fault = "the record describes no texels at all";
            }
            if (fault == nullptr && bytes == nullptr) {
                fault = "a texture upload carries no bytes";
            }
            if (fault != nullptr) {
                MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                     " resource_subdata {slot=%u, gen=%u, glName=%u}: %s (target=%u, "
                                     "level=%u, box=%d,%d,%d %ux%ux%u, regions=%u)",
                                     record.Res.Slot, record.Res.Gen, stored->Desc.GlNameForDiag, fault,
                                     record.Target, record.Level, record.UnionBox.X, record.UnionBox.Y,
                                     record.UnionBox.Z, record.UnionBox.W, record.UnionBox.H,
                                     record.UnionBox.D, record.RegionCount);
                return false;
            }
            if (!AccumulatePendingUpload(*stored, record, regions)) {
                MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                     " resource_subdata {slot=%u, gen=%u, glName=%u}: the resource names "
                                     "more distinct (upload target, level) pairs than any texture can "
                                     "have (%u)",
                                     record.Res.Slot, record.Res.Gen, stored->Desc.GlNameForDiag,
                                     kMGPipeMaxPendingUploads);
                return false;
            }
            // The serial moves for the buffer half's reason: the twin stamps its own synced
            // serial from inside the sync that reads this record, so a bump afterwards would
            // leave it one mutation behind and the next draw would re-upload what it had just
            // landed. The ACCEPTANCE is what the client reads to clear its own dirty flag - the
            // record was accumulated, so the texels are the server's now, and that is the true
            // this returns.
            ++stored->Serial;
#if MOBILEGL_BUILD_DISAGGREGATED
            // P5c (tx): THE STAGED BYTES ARE ADOPTED HERE, at the last instant `bytes` is known
            // alive (rule C: SEG_STAGE retires when this record does, and PendingUpload
            // deliberately holds no byte pointer). The hook copies the run into the server's
            // staged-texture store keyed by this record's own handle; it is a no-op in
            // monolith, so the monolith shape keeps P5's pointer-dropping expression exactly.
            if (g_resourceOps != nullptr && g_resourceOps->TextureSubData != nullptr) {
                g_resourceOps->TextureSubData(record.Res, record, bytes, regions);
            }
#endif
            return true;
        }

        // ----------------------------------------------------------------------------
        // P4a: the three kVarTail unit sets share one body.
        //
        // They differ in exactly one thing - what an entry IS - and in nothing else: the same
        // window rule, the same bound, the same "the entries outside the window are not
        // cleared", the same serial discipline. set_vertex_buffers wrote this arithmetic once
        // already; a second, third and fourth copy of it would be three more places to get the
        // Start + Count overflow wrong.
        //
        // THE WINDOW IS THE BOUND AND ENTRIES OUTSIDE IT ARE NOT CLEARED. The record is "the
        // last set as received": a set that names four units has said nothing about the other
        // 188, and clearing them would unbind textures the client never mentioned.
        //
        // THE ENTRY'S OWN Unit FIELD IS NOT POLICED, deliberately and for MGPVertexBuffer::
        // BindingIndex's reason: the DESTINATION is Start + i, which is the window this
        // function has already bounded, and the field beside it is the client's own label for
        // the entry. Policing it would hand the emitter a contract this applier cannot justify
        // - the two agree by construction or the emitter is broken in a way a unit test on the
        // emitter's side is the right place to catch.
        template <class Entry, class ArrayT>
        Bool ApplyUnitWindow(const char* call, Uint32 start, Uint32 count, Uint64 contentHash,
                             const Entry* tail, ArrayT& destination, Uint32& destinationStart,
                             Uint32& destinationCount) {
            const Uint64 capacity = destination.size();
            const Uint64 end = Uint64{start} + Uint64{count};
            const char* fault = nullptr;
            if (end > capacity) {
                fault = "the window runs past the merged texture-unit space";
            } else if (count != 0 && tail == nullptr) {
                fault = "a non-empty set carries no entries";
            }
            if (fault != nullptr) {
                MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                     " %s {start=%u, count=%u, hash=%llu}: %s (the applier holds %llu "
                                     "entries)",
                                     call, start, count, static_cast<unsigned long long>(contentHash),
                                     fault, static_cast<unsigned long long>(capacity));
                return false;
            }
            for (Uint32 i = 0; i < count; ++i) {
                destination[start + i] = tail[i];
            }
            destinationStart = start;
            destinationCount = count;
            return true;
        }

        // ----------------------------------------------------------------------------
        // P4a: the ShaderCso band split, mirrored from the allocator's.
        //
        // The composite band starts at 983040, so ONE program-pipeline composite in a
        // slot-indexed vector would grow that vector to ~983k records of ~240 bytes each - a
        // 236 MB spike on the first pipeline draw, which would be a defect this commit
        // introduced rather than one it inherited. Both spaces are kept dense against their OWN
        // high-water mark instead, exactly as MGPipeSlotAllocator keeps the band in a table of
        // its own.
        //
        // THE SERVER STILL NEVER LEARNS A HANDLE IS A COMPOSITE. The split is an indexing detail
        // on this side of the wire: create / bind / delete_shader_state and set_global_constants
        // name a composite exactly as they name any other program, and none of them branches on
        // it outside these two functions.
        // ----------------------------------------------------------------------------
        MGPipeShaderCsoRecord* ShaderCsoRecordAt(Uint32 slot) {
            if (MGPipeIsCompositeShaderSlot(slot)) {
                return RecordAt(g_applier.CompositeShaderCsos, slot - kMGPipeShaderCsoCompositeSlotBase,
                                kMGPipeShaderCsoSlotLimit - kMGPipeShaderCsoCompositeSlotBase);
            }
            // The ordinary table is bounded by the BAND'S BASE and not by the slot limit: an
            // ordinary program can never be handed a band slot (the allocator refuses it), so a
            // slot at or above the base that is not a composite is out of range by definition.
            return RecordAt(g_applier.ShaderCsos, slot, kMGPipeShaderCsoCompositeSlotBase);
        }

        MGPipeShaderCsoRecord* FindShaderCso(MGPipeHandle cso) {
            if (MGPipeIsCompositeShaderSlot(cso.Slot)) {
                return FindIn(g_applier.CompositeShaderCsos,
                              MGPipeHandle{cso.Slot - kMGPipeShaderCsoCompositeSlotBase, cso.Gen});
            }
            return FindIn(g_applier.ShaderCsos, cso);
        }

        MGPipeShaderCsoRecord* ResolveShaderCso(const char* call, MGPipeHandle cso) {
            MGPipeShaderCsoRecord* record = FindShaderCso(cso);
            MOBILEGL_ASSERT(record != nullptr, "%s named {slot=%u, gen=%u}: %s", call, cso.Slot, cso.Gen,
                            kResourceRefusalNote);
            if (record == nullptr) ++g_applier.RefusedObjectCalls;
            return record;
        }

#if MOBILEGL_PIPE_VERIFY
        // THE ARCHIVE'S ROUND TRIP, and it is a VERIFY-ONLY gate on a path that does not
        // serialise anything in a shipped build. What it asks is the question a transport will
        // ask on the first day it exists and nobody can ask afterwards: does the archive this
        // client is publishing survive being turned into bytes and back? A field the codec
        // skips in BOTH directions round-trips perfectly, which is why the unit suite reads the
        // members back explicitly; what THIS adds is that it runs over every real program the
        // verify lane links rather than over one hand-built instance.
        void PinProgramArchiveRoundTrip(const MGPProgramDesc& desc,
                                        const MG_State::GLState::LinkArtifacts& link,
                                        const MG_State::GLState::SpirvArtifacts& spirv) {
            Vector<Uint8> encoded;
            MG_State::GLState::EncodeProgramArtifacts(link, spirv, encoded);
            MG_State::GLState::LinkArtifacts decodedLink;
            MG_State::GLState::SpirvArtifacts decodedSpirv;
            Vector<Uint8> reencoded;
            const char* fault = nullptr;
            if (encoded.empty()) {
                fault = "the encoder produced no bytes for a program that has artefacts";
            } else if (!MG_State::GLState::DecodeProgramArtifacts(encoded.data(), encoded.size(),
                                                                  decodedLink, decodedSpirv)) {
                fault = "the archive this client would put on the wire does not decode";
            } else {
                MG_State::GLState::EncodeProgramArtifacts(decodedLink, decodedSpirv, reencoded);
                if (reencoded != encoded) {
                    fault = "the archive does not survive its own round trip";
                } else if (decodedLink.program != nullptr) {
                    // The live glslang TProgram is the one member the tables deliberately omit;
                    // a decode that reconstructed one would be carrying the compiler front end
                    // across a boundary that exists to keep it on this side.
                    fault = "the decoded archive carries a live program object";
                }
            }
            if (fault == nullptr) return;
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("PipeVerifyDiffer")
                                 " program-archive create_shader_state {slot=%u, gen=%u}: %s (%llu bytes "
                                 "encoded, %llu re-encoded)",
                                 desc.Cso.Slot, desc.Cso.Gen, fault,
                                 static_cast<unsigned long long>(encoded.size()),
                                 static_cast<unsigned long long>(reencoded.size()));
        }
#endif
    } // namespace

    MGPipeApplierState& MGPipeApplier() { return g_applier; }

    // P5c (rv, CONTRACT-P5C.md §5.3): the three texture shutters' server-side answers. See
    // MGPipeApplierState::TextureShutterSerial / ContextSerial for the bump rule.
    Uint64 MGPipeApplierTextureShutterSerial() { return g_applier.TextureShutterSerial; }
    Uint64 MGPipeApplierContextSerial() { return g_applier.ContextSerial; }
    void MGPipeApplierNoteTextureStateMoved() { ++g_applier.TextureShutterSerial; }

    void MGPipeSetResourceOps(const MGPipeResourceOps* ops) { g_resourceOps = ops; }
    const MGPipeResourceOps* MGPipeGetResourceOps() { return g_resourceOps; }

    namespace {
        // P4a's BELT (ID-39). See MGPipeApplierState::RefusedNoConsumer for the whole argument;
        // in one line: acceptance is a contract with the client, and accepting a record on a
        // backend that consumes none of them makes the emitter clear a dirty flag the legacy
        // pull path still owed.
        //
        // IT IS THE SAME SIGNAL THE CLIENT'S GATE READS, deliberately - g_resourceOps is the
        // table a backend installs at its own bring-up and uninstalls at teardown, and
        // MGPipeApplierReset does NOT touch it (see there), so the gate cannot flap between a
        // make-current and the emissions that follow it. The two therefore agree on every path
        // except the one this exists for: an emitter called directly, without passing PipeFill.
        //
        // EVERY CALLER PLACES IT AFTER THE RECORD'S OWN SHAPE CHECKS AND BEFORE ANYTHING MOVES.
        // A malformed record is Fatal{ProtocolCorruption} whether or not anything would have
        // read it - a trip wire that fires only on some backends is a trip wire nobody can
        // trust - so the refusal is the LAST thing asked and the first thing that stops the
        // write. The three unit-set entry points are the exception and say so at their call
        // site: ApplyUnitWindow validates and writes in one step, so the question has to be
        // asked in front of it.
        Bool NoP4aConsumer() {
            if (g_resourceOps != nullptr) return false;
            ++g_applier.RefusedNoConsumer;
            return true;
        }

        // P5e (sb, CONTRACT-P5E.md §5.6): the three binding-point windows, cleared in one
        // place because BOTH resets clear them and the two used to drift on exactly this kind
        // of member (the framebuffer family's four). The SERIAL is deliberately NOT touched
        // here - each caller advances it beside its own siblings, where the "advance, never
        // zero" rule is written out.
        void ClearShaderBufferWindows() {
            for (Uint32 cls = 0; cls < kMGPipeShaderBufferClassCount; ++cls) {
                g_applier.BoundShaderBuffers[cls] = {};
                g_applier.ShaderBufferStart[cls] = 0;
                g_applier.ShaderBufferCount[cls] = 0;
                for (Uint32 w = 0; w < kMGPipeShaderBufferWritableMaskWords; ++w) {
                    g_applier.ShaderBufferWritableMask[cls][w] = 0;
                }
            }
        }
    } // namespace

    void MGPipeApplierReset() {
#if MOBILEGL_BUILD_DISAGGREGATED
        // P5c (ct), CONTRACT-P5C.md §5.1 / §6 layer 2. With an active transport g_applier is
        // SERVER-PRIVATE and the GL thread's reset crosses as the applier_reset RECORD
        // (PipeFill.cpp's FreshlyPrimed arm emits it; ServerVerbSink::OnApplierReset runs this
        // function ON the apply thread). Reaching here from any other thread WITH A LIVE WIRE
        // is the direct call the record replaced - a write into server memory with no wire
        // shape, and exactly the revert the red-once gate must catch - so it is a named Fatal
        // rather than a silent reset of state another role owns. The window the Fatal
        // deliberately does NOT cover is a configured-but-wireless transport: before
        // ClientSession::Start() there is no wire for a record to cross (§6 layer 2's
        // documented bring-up exception), and a ServerLoop fixture with no client at all has
        // none either - there this call is the only reset that exists. Monolith keeps the
        // direct call, byte for byte (G1); in a pull build none of this is compiled at all.
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith &&
            MG_Remote::Client::ClientSession::Active() != nullptr &&
            !MG_Remote::Server::ServerLoop::OnApplyThread()) {
            MGLOG_F("MGPipe: Fatal{RoleViolation, \"g_applier\"} - MGPipeApplierReset() called "
                    "off the apply thread with an active transport; under split the reset "
                    "crosses as the applier_reset record (CONTRACT-P5C.md §5.1)");
            std::abort();
        }
#endif
        g_applier.RenderStateCsos.clear();
        g_applier.BoundRenderStateCso = kMGPipeNullHandle;
        g_applier.Residual = ResidualValueBlock{};
        g_applier.HasResidual = false;
        g_applier.ScatteredChunkBits = 0;
        g_applier.ResidualCapabilitiesCompared = 0;
        g_applier.ResidualDivergences = 0;
        g_applier.PatchCarrierComparisons = 0;
        g_applier.PatchCarrierDivergences = 0;
        // P3a. THIS RUNS AT EVERY CHANGE OF THE CURRENT CONTEXT, not once per fresh one:
        // MGPipeTracker::Update resets itself whenever the context pointer moves and the
        // emitter calls this from the walk that follows, so a make-current BACK to a context
        // that is still alive lands here too. Everything cleared below is therefore something
        // a returning context may not inherit, and nothing else is cleared.
        //
        // THE OBJECT RECORDS ARE NOT CLEARED. A GL object lives in a share group, not in a
        // context: the buffer a returning context is about to write to is the same buffer with
        // the same storage, and its record is where the extent and the mutation serial that
        // D-A4 re-keys IsBufferDrawClean onto now live. Dropping them here made every
        // glBufferSubData after a context switch resolve to nothing and be dropped, with the
        // only trace an assertion that compiles out at INFO. They go at the object's own death
        // (resource_destroy, delete_vertex_elements) and at MGPipeApplierReleaseObjectRecords.
        //
        // The OP TABLE is deliberately not cleared either - it is installed and uninstalled by
        // the backend's own bring-up and teardown, not by a state reset.
        g_applier.RefusedResourceCalls = 0;
        g_applier.RefusedVertexInputCalls = 0;
        g_applier.RefusedObjectCalls = 0;
        g_applier.RefusedNoConsumer = 0;
        g_applier.BoundVertexElements = kMGPipeNullHandle;
        g_applier.VertexBuffers = {};
        g_applier.VertexBufferStart = 0;
        g_applier.VertexBufferCount = 0;
        g_applier.VertexFetchBaseInstance = 0;
        g_applier.IndexBuffer = MGPIndexBuffer{};
        // m6 / wire n6, written down rather than left to be rediscovered: THIS counter is
        // per-applier and is zeroed at every make-current, while MG_Util::PipeStats' `mpr` -
        // emitted from the CLIENT at MG_Impl/Pipe/PipeFill.cpp's MGPipeEmitMapPersistent - is
        // process-wide and is windowed by EndFrame. The two therefore disagree across a context
        // switch, by design and not by accident: this one answers "how many round trips has THIS
        // applier been asked for since it was last reset", which is what a unit case driving the
        // applier directly wants, and PipeStats' answers "how many did the process take in this
        // window", which is what a lane reading a log line wants.
        //
        // WHICH ONE THE GATES ASSERT ON, because that was the open question: G10
        // (StorageBufferRegrow) and G12 read PipeStats' `mpr` out of the lane's own log through
        // MG_IntegrationTest/Harness/PipeStatsWindow.h - they cannot link this symbol at all, on
        // Android or anywhere else - so a make-current inside a scenario cannot silently reset
        // what they measure. Nothing outside MG_Test reads the member below.
        g_applier.MapPersistentRoundtrips = 0;
        // THE TWO SERIALS ADVANCE; THEY ARE NOT ZEROED. They are MGGens, and an MGGen that
        // walks backwards is not one. There are exactly three things a reset can do to a
        // version whose data it has just cleared:
        //   - carry the count over: the twin's memo still matches state that is now empty, so
        //     the very next draw reads clean over a cleared window. Wrong immediately;
        //   - restart at 0: the counter then walks back up through every value it has already
        //     stamped into a twin, and a VAO twin does NOT die with a make-current
        //     (OnBackendContextDestroyed runs on destroy) and has no context generation beside
        //     the serial - D-G4 deletes the identity patch that used to close exactly this
        //     hole. Wrong later, and reliably, because a context whose per-activation call
        //     count is stable lands on a stamped value every time;
        //   - advance: the clearing is itself announced, no stamped value can ever recur, and
        //     the first compare after the switch is a mismatch, which is the safe direction.
        ++g_applier.VertexBuffersSerial;
        ++g_applier.IndexBufferSerial;
        // P5c (rv): a make-current is a fresh server, and the two shutter serials are what the
        // PipeInputs texture-shutter accessors answer with - so both ADVANCE here, for the
        // serials' own reason: a counter that restarts walks back through values already
        // stamped into a memo that outlived the switch. ContextSerial's move is also
        // GetTextureContextId's whole job (the backends key their per-context memos on it).
        ++g_applier.TextureShutterSerial;
        ++g_applier.ContextSerial;

        // ---- P4a's working state, cleared for the same reason and with the same serial rule
        // (D-J4). The OBJECT records - texture and renderbuffer resources, sampler CSOs,
        // sampler views, shader CSOs and, since ID-19(b), the FRAMEBUFFER RECORDS - are
        // deliberately NOT here: a texture lives in a share group exactly as a buffer does, and
        // its record is where the extent, the parameters and the pending-upload set the backend
        // reads now live.
        //
        // WHAT A MAKE-CURRENT TAKES OF THE FRAMEBUFFER FAMILY IS THE TWO BOUND HANDLES AND
        // NOTHING ELSE, and that is the same observable it always took: DrawFramebuffer() /
        // ReadFramebuffer() answer null after this, exactly as the zeroed records used to answer
        // a null Fbo. FramebufferRecords stays because a framebuffer that is only ever addressed
        // BY NAME - the DSA blit and clear entry points - has no re-emission trigger at all, so
        // dropping its record here would lose it for the life of the process.
        g_applier.BoundFramebuffer = {};
        g_applier.BoundSamplerViews = {};
        g_applier.SamplerViewStart = 0;
        g_applier.SamplerViewCount = 0;
        g_applier.BoundSamplerStates = {};
        g_applier.SamplerStateStart = 0;
        g_applier.SamplerStateCount = 0;
        g_applier.BoundShaderImages = {};
        g_applier.ShaderImageStart = 0;
        g_applier.ShaderImageCount = 0;
        g_applier.DrawProgram = kMGPipeNullHandle;
        g_applier.DispatchProgram = kMGPipeNullHandle;
        g_applier.BoundShaderCso = kMGPipeNullHandle;
#if MOBILEGL_BUILD_DISAGGREGATED
        g_applier.BoundStreamOutputLifetimeId = 0;
        g_applier.StreamOutputSpans.clear();
#endif
        // P5e (sb, CONTRACT-P5E.md §5.6): the three binding-point windows are per-context
        // WORKING state and go with the rest of it - a returning context has its own
        // glBindBufferBase history and may not inherit the one this applier was left holding.
        // The SERIAL advances rather than restarting, for the reason written out above: the
        // clearing is itself a change every backend memo has to hear about, and a counter that
        // restarts walks back through values already stamped into a memo that outlived the
        // switch. The EMITTER's latch resets with it (PipeFill.cpp's FreshlyPrimed arm
        // invalidates the three suppressor slots), or the first emission after a make-current
        // would be suppressed as unchanged and the server would draw with a cleared window.
        ClearShaderBufferWindows();
        ++g_applier.FramebufferSerial;
        ++g_applier.SamplerViewsSerial;
        ++g_applier.SamplerStatesSerial;
        ++g_applier.ShaderImagesSerial;
        ++g_applier.ProgramBindingSerial;
        ++g_applier.ShaderBuffersSerial;
    }

    void MGPipeApplierReleaseObjectRecords() {
        // The served context is going away and this applier with it. Under split that is one
        // applier per served context; in the monolith there is one applier behind every
        // context, so nothing wires this - see PipeApply.h. The two serials advance here for
        // MGPipeApplierReset's reason: state was cleared, and a twin that outlives it must not
        // be able to match a value it has already seen.
        g_applier.Resources.clear();
        g_applier.VertexElementsCsos.clear();
        g_applier.BoundVertexElements = kMGPipeNullHandle;
        ++g_applier.VertexBuffersSerial;
        ++g_applier.IndexBufferSerial;
        // P5c (rv): same rule for the shutter serials - the served context is going away, and
        // a memo that outlives it must not match a value these have already answered with.
        ++g_applier.TextureShutterSerial;
        ++g_applier.ContextSerial;
        // P4a's five object tables go with them, and the working handles they could name go
        // too - a bound shader CSO whose record has just been dropped must not survive as a
        // handle the next call resolves against.
        g_applier.TextureResources.clear();
        g_applier.RenderbufferResources.clear();
        g_applier.SamplerCsos.clear();
        g_applier.SamplerViewCsos.clear();
        g_applier.ShaderCsos.clear();
        g_applier.CompositeShaderCsos.clear();
        // AND THE FRAMEBUFFER RECORDS, which MGPipeApplierReset above deliberately keeps. Here
        // there is nothing for them to outlive: the served context is going away and the
        // framebuffers it owned are container objects that go with it. A table left standing
        // would answer a lookup from the NEXT context on a slot the allocator has re-minted -
        // with the same generation, if the next context happens to reach the same one - and
        // that is the aliasing every other table is cleared to prevent.
        g_applier.FramebufferRecords.clear();
        g_applier.DrawProgram = kMGPipeNullHandle;
        g_applier.DispatchProgram = kMGPipeNullHandle;
        g_applier.BoundShaderCso = kMGPipeNullHandle;
#if MOBILEGL_BUILD_DISAGGREGATED
        g_applier.BoundStreamOutputLifetimeId = 0;
        g_applier.StreamOutputSpans.clear();
#endif
        // AND "THE WORKING HANDLES THEY COULD NAME" IS ALL OF THEM, NOT JUST THE THREE ABOVE.
        // Every framebuffer record holds eleven MGPSurface::Res naming texture and renderbuffer
        // records this function has just dropped - which is why the table itself goes above -
        // and the three unit windows hold entries naming sampler-view, sampler-CSO and texture
        // records it has just dropped. A window left populated here is a set of handles into
        // empty tables: the next resolve either refuses and counts (a teardown-time refusal
        // storm nothing asked for) or, on a slot the next context re-mints, resolves to somebody
        // else's record. So this takes the same working state MGPipeApplierReset does - which is
        // the honest reading of the rule as well: Reset keeps the object records BECAUSE they
        // outlive a make-current, and here they do not outlive anything.
        g_applier.BoundFramebuffer = {};
        g_applier.BoundSamplerViews = {};
        g_applier.SamplerViewStart = 0;
        g_applier.SamplerViewCount = 0;
        g_applier.BoundSamplerStates = {};
        g_applier.SamplerStateStart = 0;
        g_applier.SamplerStateCount = 0;
        g_applier.BoundShaderImages = {};
        g_applier.ShaderImageStart = 0;
        g_applier.ShaderImageCount = 0;
        // P5e (sb): and the three binding-point windows, for the paragraph above's reason in
        // its own words - every MGPBufferRange::Res names a resource record this function has
        // just dropped, so a window left populated here is a set of handles into an empty
        // table, which the next resolve either refuses or answers with somebody else's record
        // on a slot the next context re-mints.
        ClearShaderBufferWindows();
        ++g_applier.FramebufferSerial;
        ++g_applier.SamplerViewsSerial;
        ++g_applier.SamplerStatesSerial;
        ++g_applier.ShaderImagesSerial;
        ++g_applier.ProgramBindingSerial;
        ++g_applier.ShaderBuffersSerial;
    }

    void MGPipeApplyCreateRenderState(const MGPRenderStateDesc& desc, const void* chunkBytes) {
        MOBILEGL_ASSERT(desc.Cso.Slot >= kMGPipeFirstAllocatableSlot,
                        "create_render_state named the reserved slot 0");
        if (desc.Cso.Slot >= g_applier.RenderStateCsos.size()) {
            g_applier.RenderStateCsos.resize(desc.Cso.Slot + 1);
        }
        MGPipeRenderStateCsoRecord& record = g_applier.RenderStateCsos[desc.Cso.Slot];

        // NEITHER BRANCH MAY LEAVE ITS BAD CASE TO MOBILEGL_ASSERT. The slot the client is
        // naming may be a RECYCLED one whose record still holds the previous occupant's 396
        // bytes; in an INFO build - which is what every gate and every shipped build is - an
        // assertion is a no-op, so inheriting nothing onto those bytes and then scattering
        // the delta chunks on top would hand out a record that is half one CSO and half
        // another, with no gate able to see it. Both arms therefore start from a DEFINED
        // base and report through this file's trip-wire verdict.
        if (MGPipeHandleIsNull(desc.BaseCso)) {
            // A brand-new CSO carries its whole content; there is no earlier record to
            // inherit the unnamed chunks from.
            record.PipelineBytes = {};
            if ((desc.ChunkMask & kAllPipelineChunks) != kAllPipelineChunks) {
                MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("PipeIncompleteCso")
                                     " create_render_state {slot=%u, gen=%u} with no BaseCso named chunks "
                                     "0x%x, not the whole pipeline half 0x%x; the rest is zeroed",
                                     desc.Cso.Slot, desc.Cso.Gen, desc.ChunkMask,
                                     static_cast<Uint32>(kAllPipelineChunks));
            }
        } else {
            const MGPipeRenderStateCsoRecord* base = FindCso(desc.BaseCso);
            record.PipelineBytes =
                base != nullptr ? base->PipelineBytes : Array<Uint8, kMGPipePipelineChunkBytes>{};
            if (base == nullptr) {
                MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("PipeDeadBaseCso")
                                     " create_render_state {slot=%u, gen=%u} named a dead BaseCso "
                                     "{slot=%u, gen=%u}; the delta chunks land on zeroed bytes, not on "
                                     "the recycled slot's previous occupant",
                                     desc.Cso.Slot, desc.Cso.Gen, desc.BaseCso.Slot, desc.BaseCso.Gen);
            }
        }

        // The chunk bytes land in the record's own gathered order, so the record is always a
        // complete pipeline half whatever mask minted it.
        RenderStateParameters staging{};
        MGPipeScatterPipelineBytes(record.PipelineBytes.data(), staging);
        MGPipeScatterPipelineChunks(chunkBytes, desc.ChunkMask, staging);
        MGPipeGatherPipelineBytes(staging, record.PipelineBytes.data());

        record.Gen = desc.Cso.Gen;
        record.Live = true;
    }

    void MGPipeApplyBindRenderState(const MGPBindRenderState& bind) {
        const MGPipeRenderStateCsoRecord* record = FindCso(bind.Cso);
        MOBILEGL_ASSERT(record != nullptr, "bind_render_state named a dead CSO {slot=%u, gen=%u}",
                        bind.Cso.Slot, bind.Cso.Gen);
        if (record == nullptr) return;

        PipeInputs& inputs = gPipeInputs;
        MGPipeScatterPipelineBytes(record->PipelineBytes.data(),
                                   MGPipeApplyAccess::RenderState(inputs));
        MGPipeApplyAccess::SetRenderStateVersions(inputs, bind.Version, bind.PipelineVersion);
        g_applier.BoundRenderStateCso = bind.Cso;
        // A bind scatters the WHOLE pipeline half - the record is always a complete one,
        // whatever mask minted it - so the pipeline chunks are all "moved" here, and all
        // enter the applier's ledger of the bytes it owns.
        const Uint32 moved = MGPipeGlobalChunkBitsOfPipelineMask(kAllPipelineChunks);
        g_applier.ScatteredChunkBits |= moved;
        MGPipeDeriveRenderStateFieldsForChunks(inputs, moved);
    }

    void MGPipeApplyDeleteRenderState(const MGPHandleOnly& handle) {
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::RenderStateCso),
                        "delete_render_state on kind %u", handle.Kind);
        MGPipeRenderStateCsoRecord* record = FindCso(handle.Handle);
        if (record == nullptr) return;
        record->Live = false;
        // The Gen stays: it is the CLIENT allocator that bumps it when the slot is handed
        // out again (MGPipeHandles.h: "Gen increments only when a SLOT IS REUSED"), and a
        // server-side bump here would put the two identities out of step.
        if (g_applier.BoundRenderStateCso == handle.Handle) {
            g_applier.BoundRenderStateCso = kMGPipeNullHandle;
        }
    }

    void MGPipeApplySetDynamicState(const MGPDynamicState& dyn, const void* chunkBytes) {
        PipeInputs& inputs = gPipeInputs;
        MGPipeScatterDynamicChunks(chunkBytes, dyn.ChunkMask, MGPipeApplyAccess::RenderState(inputs));
        MGPipeApplyAccess::SetRenderStateParametersVersion(inputs, dyn.Version);
        const Uint32 moved = MGPipeGlobalChunkBitsOfDynamicMask(dyn.ChunkMask);
        g_applier.ScatteredChunkBits |= moved;
        MGPipeDeriveRenderStateFieldsForChunks(inputs, moved);
    }

    void MGPipeApplySetPixelPackState(const MGPPixelPackState& pack) {
        MGPipeApplyAccess::PackState(gPipeInputs) = pack.Pack;
    }

    // set_context_values (P5c rv, CONTRACT-P5C.md §5.3): the residual-value record's server
    // half. A plain store of the whole POD - there is no dirty mask and no field-level gate,
    // because the record crosses only when one of its source values moved (the client's
    // whole-record hash suppression) and a suppressed record means "nothing moved", never
    // "field invalid" (§1).
    void MGPipeApplySetContextValues(const MGPContextValues& values) {
        static_assert(std::extent_v<decltype(MGPContextValues::TouchedBufferBindingPointCount)> ==
                          PipeInputs::kBufferTargetCount,
                      "the record's per-target array and PipeInputs' have drifted");
        MGPipeApplyAccess::SetContextValues(gPipeInputs, values);
        // P5e (CONTRACT-P5E.md §2.1, escalation (i)): mirror the XFB-active flag into the
        // applier state as well. The barriered predicate has to be computable on the SERVER
        // from applier state alone - gPipeInputs is the block the run-ahead client stops
        // filling, so a predicate that read the answer out of THERE would be reading the very
        // memory the rule exists to stop it reading. One store, inert until the caps bit is
        // published, and it is what makes the two roles' third clause the same clause.
        g_applier.IsTransformFeedbackActive = values.IsTransformFeedbackActive != 0;
    }

    void MGPipeApplySetPatchState(const MGPPatchState& patch) {
        PipeInputs& inputs = gPipeInputs;
        RenderStateParameters& working = MGPipeApplyAccess::RenderState(inputs);
        const FloatVec4 outer(patch.Outer[0], patch.Outer[1], patch.Outer[2], patch.Outer[3]);
        const FloatVec2 inner(patch.Inner[0], patch.Inner[1]);

        // THE SECOND TRIP WIRE (D6, D10). The patch trio travels TWICE - once in pipeline
        // chunk P0, because it is pipeline state, and once as set_patch_state, because both
        // backends bake it into the synthesized control stage from a shader-build path. The
        // redundancy is the point: if the two carriers ever part, a stale set_patch_state
        // silently clobbers what bind_render_state scattered and the tessellation levels a
        // draw uses stop being the ones its CSO was minted for.
        //
        // Compared BITWISE, because a NaN outer level is a legal glPatchParameterfv value
        // (ARCHITECTURE.md 5.2) and must compare equal to itself.
        //
        // ARMED BY THE APPLIER'S OWN SCATTER LEDGER, not by "some CSO has been bound"
        // (PipeApply.h, MGPipeApplierState::ScatteredChunkBits). The question is whether the
        // chunk-P0 bytes in the working block are the applier's, and that single condition
        // covers both contracts this wire needs: the ORDERING one - a set_patch_state that
        // legitimately precedes the first bind of a context has nothing to agree with yet -
        // and the VERB-CLASS one - with the render-state subsystem off those bytes are the
        // per-verb fill loop's, and FillPoints.def does not publish GetRenderStateParameters
        // at kDispatch or kTextureOp, so they go stale there.
        //
        // It runs in the shipped push build too, because a wire that is compiled out of
        // every build a device runs is not a wire. The cost is a 24-byte memcmp on a call
        // that is emitted when the tessellation state CHANGES, i.e. about once per program.
        if ((g_applier.ScatteredChunkBits & kChunksPatchTrio) == kChunksPatchTrio) {
            ++g_applier.PatchCarrierComparisons;
            const Bool agrees = working.PatchVertices == patch.Vertices &&
                                std::memcmp(&working.PatchDefaultOuterLevel, &outer, sizeof(outer)) == 0 &&
                                std::memcmp(&working.PatchDefaultInnerLevel, &inner, sizeof(inner)) == 0;
            if (!agrees) {
                ++g_applier.PatchCarrierDivergences;
                MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("PipePatchCarriersDiffer")
                                     " set_patch_state says vertices=%u outer=(%g,%g,%g,%g) inner=(%g,%g); "
                                     "chunk P0 delivered vertices=%u outer=(%g,%g,%g,%g) inner=(%g,%g)",
                                     patch.Vertices, static_cast<double>(outer.x()),
                                     static_cast<double>(outer.y()), static_cast<double>(outer.z()),
                                     static_cast<double>(outer.w()), static_cast<double>(inner.x()),
                                     static_cast<double>(inner.y()), working.PatchVertices,
                                     static_cast<double>(working.PatchDefaultOuterLevel.x()),
                                     static_cast<double>(working.PatchDefaultOuterLevel.y()),
                                     static_cast<double>(working.PatchDefaultOuterLevel.z()),
                                     static_cast<double>(working.PatchDefaultOuterLevel.w()),
                                     static_cast<double>(working.PatchDefaultInnerLevel.x()),
                                     static_cast<double>(working.PatchDefaultInnerLevel.y()));
            }
        }

        working.PatchVertices = patch.Vertices;
        working.PatchDefaultOuterLevel = outer;
        working.PatchDefaultInnerLevel = inner;
        MGPipeApplyAccess::SetPatchState(inputs, working.PatchVertices, working.PatchDefaultOuterLevel,
                                         working.PatchDefaultInnerLevel);
    }

    void MGPipeApplySetVertexAttribDefaults(const MGPVertexAttribDefaults& hdr,
                                            const MGPAttribValue* tail) {
        PipeInputs& inputs = gPipeInputs;
        PipeInputs::CurrentVertexAttributeValue* slots = MGPipeApplyAccess::VertexAttribDefaults(inputs);
        // The loop walks the MASK's 32 bits, not the slot array, and every named bit consumes
        // its tail entry even when there is no slot to write it to: a named-but-unstorable
        // attribute that did not consume would write every attribute after it from the wrong
        // entry. PipeInputs::kMaxVertexAttribs is VertexArrayObject's 32 today, so the
        // out-of-range arm is unreachable - the guard is what keeps that true if the two ever
        // stop agreeing. None of the three consistency checks may be left to MOBILEGL_ASSERT,
        // which is inert at INFO: a malformed tail would then desynchronise the attribute
        // writes without a word in exactly the builds that ship.
        Uint32 consumed = 0;
        const char* fault = nullptr;
        for (Uint32 location = 0; location < 32 && fault == nullptr; ++location) {
            if ((hdr.Mask & (Uint32{1} << location)) == 0) continue;
            if (consumed >= hdr.Count) {
                fault = "Mask names more attributes than Count";
                break;
            }
            const MGPAttribValue& value = tail[consumed++];
            if (value.Location != location) {
                fault = "tail out of ascending location order";
                break;
            }
            if (location >= PipeInputs::kMaxVertexAttribs) {
                fault = "Mask names a location the block has no slot for";
                continue;
            }
            PipeInputs::CurrentVertexAttributeValue& slot = slots[location];
            // P5c (rv, CONTRACT-P5C.md §5.3): the record carries ALL THREE VIEWS VERBATIM, as
            // the frontend computed them, and the applier writes each view from its own array.
            // The pre-rv shape - one Data[4] memcpied into all three views - could not
            // reproduce the frontend's NUMERIC cross-view conversion (glVertexAttrib4f(loc,
            // 1.5f, ...) leaves 1 in intValue and 0x3FC00000 in floatValue), which is exactly
            // why the field stayed EMITTED-AND-STILL-PULLED until rv. ValueClass rides along
            // for the comparator; the applier no longer needs it to rebuild anything.
            std::memcpy(slot.floatValue.data(), value.FloatView, sizeof(slot.floatValue));
            std::memcpy(slot.intValue.data(), value.IntView, sizeof(slot.intValue));
            std::memcpy(slot.uintValue.data(), value.UintView, sizeof(slot.uintValue));
        }
        if (fault == nullptr && consumed != hdr.Count) {
            fault = "Count does not match the attributes Mask names";
        }
        if (fault != nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("PipeAttribTailMalformed")
                                 " set_vertex_attrib_defaults: %s (Mask=0x%x, Count=%u, consumed=%u)",
                                 fault, hdr.Mask, hdr.Count, consumed);
        }
    }

    void MGPipeApplySetResidualValueState(const ResidualValueBlock& block) {
        g_applier.Residual = block;
        g_applier.HasResidual = true;
        g_applier.ResidualCapabilitiesCompared = 0;

        // THE TRIP WIRE (ARCHITECTURE.md 9.4, P2 brief D9). CapabilityBits is redundant with
        // the assembled working block by design: every one of the 35 capabilities is
        // answerable from RenderStateParameters now that P2 closed the three storage holes.
        // So the day a later call takes a capability over and forgets to carry it, the two
        // answers part and this says so on the next draw - which is what a migration carrier
        // is for.
        //
        // THE ORACLE IS THE WORKING BLOCK, AND THE WIRE IS ARMED PER CAPABILITY by the
        // applier's own scatter ledger: capability i is compared only once every chunk its
        // answer is read out of has been scattered by this applier. That is not a weakening,
        // it is the wire's whole precondition:
        //
        //   - with the render-state subsystem off (MOBILEGL_PIPE_PUSH=0x10 is a legal
        //     configuration - D14's per-subsystem A/B) the ledger is empty and the wire says
        //     nothing at all, which is right: the working block is then the per-verb fill
        //     loop's, published per verb CLASS, so at a kDispatch or kTextureOp verb it holds
        //     the previous draw's bytes and disagreeing with it means nothing;
        //   - with it on, the applier is the block's only writer and its bytes are current at
        //     every verb of every class - including the two above, which is the case a
        //     RenderStateSpansTest case drives on purpose.
        //
        // NOT PipeInputs::m_capability, which the earlier form compared against and which
        // FillPoints.def does publish at seven classes rather than five: where that
        // publication is what makes m_capability fresh, the fill loop filled it out of the
        // same GLContext the client built CapabilityBits from, so the comparison is a
        // tautology. The redundancy this wire exists to check is between the CARRIED bits and
        // the ASSEMBLED block.
        const RenderStateParameters& working = MGPipeApplyAccess::RenderState(gPipeInputs);
        const Uint32 owned = g_applier.ScatteredChunkBits;
        for (SizeT i = 0; i < kCapabilityCount; ++i) {
            const CapabilityInput cap = static_cast<CapabilityInput>(i);
            const Uint32 sources = CapabilitySourceChunks(cap);
            if ((owned & sources) != sources) continue;
            ++g_applier.ResidualCapabilitiesCompared;
            const Bool carried = ((block.CapabilityBits >> i) & 1ull) != 0;
            const Bool assembledBit = MGPipeApplyAccess::DeriveCapability(working, cap);
            if (carried == assembledBit) continue;
            ++g_applier.ResidualDivergences;
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("PipeResidualDiverged, \"%s\"")
                                 " carried=%d assembled=%d",
                                 kCapabilityNames[i], static_cast<int>(carried),
                                 static_cast<int>(assembledBit));
        }
    }

    // ================================================================================
    // P3a: the nine resource entry points (D-A1, D-A2).
    //
    // WHAT THE APPLIER OWNS HERE IS IDENTITY, EXTENT AND ORDER - NOT CONTENT. A buffer's
    // bytes are the backend's; what crosses is a {slot, gen} handle, a flat descriptor and,
    // where the call carries content, the client's own shadow base. So each body below does
    // three things in this order: resolve the handle against this applier's record, check the
    // record against its own declared extent, and only then move the record and hand the call
    // to the backend.
    //
    // NOTHING REGISTERS MGPipeResourceOps IN THIS PACKAGE, so every dispatch below is a null
    // check that falls through, and the tree behaves exactly as it did. That is deliberate and
    // it is what makes this commit landable on its own: the frontend still dispatches the op
    // table these replace, the backend that will register one is a later package, and the
    // records these bodies keep are already correct when it does.
    //
    // THE THREE SERIALS ARE MGGen-CLASS: server-owned, monotone, never crossing the line. No
    // MGPipe call may require the client to supply or know one (ARCHITECTURE.md 4.2.2), which
    // is why they are incremented here rather than carried in a payload.
    // ================================================================================

    Bool MGPipeApplyResourceCreate(const MGPResourceDesc& desc) {
        MOBILEGL_ASSERT(desc.Resource.Slot >= kMGPipeFirstAllocatableSlot,
                        "resource_create named the reserved slot 0");
        if (desc.Resource.Slot < kMGPipeFirstAllocatableSlot) return false;

        // P4a: THE TABLE IS CHOSEN BY THE DESCRIPTOR'S TARGET, and getting that wrong is the one
        // way this call can damage an object it was not about - slot 7 is a live Buffer, a live
        // Texture and a live Renderbuffer at the same time, in three independent slot spaces.
        Vector<MGPipeResourceRecord>* table = ResourceTableForTarget(desc.Target);
        if (table == nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " resource_create {slot=%u, gen=%u, glName=%u}: the descriptor names no "
                                 "resource target (%u)",
                                 desc.Resource.Slot, desc.Resource.Gen, desc.GlNameForDiag, desc.Target);
            return false;
        }

        // A CREATE STARTS THE RECORD OVER rather than editing it. The slot it names may be a
        // RECYCLED one whose record still describes the previous occupant, and inheriting one
        // field of that - a Width, a Serial, an Immutable, a pending upload - is precisely how a
        // buffer at a recycled address inherits its predecessor's contents. The generation is
        // the client allocator's answer to "is this still the same GL object", so it is taken
        // from the handle and nothing else survives.
        MGPipeResourceRecord* record = RecordAt(*table, desc.Resource.Slot, kMGPipeMaxResourceSlots);
        if (record == nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " resource_create {slot=%u, gen=%u, glName=%u}: the slot is outside the "
                                 "record table's bound (%u)",
                                 desc.Resource.Slot, desc.Resource.Gen, desc.GlNameForDiag,
                                 kMGPipeMaxResourceSlots);
            return false;
        }
        // P4a's belt, and only over the P4a half of this call: the BUFFER row is P3a's and its
        // consumer question is the frontend's (MGPipeResourceSubsystemEnabled), which has
        // already answered it before the emission - a buffer record can only arrive here on a
        // backend that registered the table, so asking again would be a second copy of that
        // rule in the one path that runs. A texture or renderbuffer create declined here leaves
        // the client's publication latch at false, which is what makes the next respecify's
        // self-healing create do the right thing if a table appears later.
        if (desc.Target != kMGPipeResourceTargetBuffer && NoP4aConsumer()) return false;

        *record = MGPipeResourceRecord{};
        record->Gen = desc.Resource.Gen;
        record->Live = true;
        record->Desc = desc;
        // Serial stays 0: a create is not a mutation. The descriptor a create carries defines
        // no storage - that is the first respecify's job, and a backend tolerates a resource
        // that has none - and a fresh backend twin starts its own synced serial at 0, so the
        // two agree from the first instant without either side publishing anything.
        //
        // EVERY NON-BUFFER TARGET STORES AND RETURNS. Espryt allocates a texture's storage
        // lazily inside SyncMipmapsToBackend and a renderbuffer's inside its own SyncToBackend,
        // so there is no GL-call-time hook to dispatch to and P4a adds none: the record IS the
        // publication, and the backend reads it at the sync point it already has.
        if (desc.Target != kMGPipeResourceTargetBuffer) return true;
        if (g_resourceOps != nullptr && g_resourceOps->Create != nullptr) {
            g_resourceOps->Create(desc.Resource, desc);
        }
        return true;
    }

    Bool MGPipeApplyResourceRespecify(const MGPResourceDesc& desc, const void* initialBytes,
                                      const MGPRespecifiedLevel* level) {
        Vector<MGPipeResourceRecord>* table = ResourceTableForTarget(desc.Target);
        if (table == nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " resource_respecify {slot=%u, gen=%u, glName=%u}: the descriptor names "
                                 "no resource target (%u)",
                                 desc.Resource.Slot, desc.Resource.Gen, desc.GlNameForDiag, desc.Target);
            return false;
        }
        // P4a's belt, for MGPipeApplyResourceCreate's reason and in front of the resolution
        // rather than after it: with no consumer no create was ever accepted, so resolving
        // first would report the absence as RefusedResourceCalls - the counter that means "a
        // seam defect" - for a state that is by design.
        if (desc.Target != kMGPipeResourceTargetBuffer && NoP4aConsumer()) return false;

        MGPipeResourceRecord* record = ResolveResourceIn(*table, "resource_respecify", desc.Resource);
        if (record == nullptr) return false;
        PinNoLiveHostWrites(*record, desc.Resource, "resource_respecify");
        PinWholeResourceRespecifyScope(desc, desc.Resource, "resource_respecify");

        // IS THIS A REDEFINITION AT ALL? Asked BEFORE the descriptor is replaced, because the
        // stored one is the only thing there is to compare against (ID-18 M4). See
        // RespecifyRedefinesNoStorage: true means every storage-defining field is unchanged and
        // the call exists to carry a BindMask / ImageBindableHint that moved after the
        // allocation - the immutable-texture case, where there is no later respecify to ride on.
        const Bool metadataOnly = RespecifyRedefinesNoStorage(record->Desc, desc);

        // The descriptor is replaced WHOLE, because that is what a respecify is: the store's
        // extent, usage, storage flags, immutability and defined-content flag are all restated
        // by the call that redefines it, and the backend reads them from here instead of
        // asking a frontend object for them. A metadata update replaces it too - that is how
        // the mask arrives - and by construction only the non-storage fields differ.
        record->Desc = desc;
        // THE SERIAL MOVES EITHER WAY, and for a metadata update it is the entire publication:
        // the twin re-derives its storage flags from the new mask at its next sync and decides
        // for itself whether the backend needs a recreate.
        ++record->Serial;
        // P5c (rv): a respecify can move a texture's shape, which is exactly what the
        // sampling-resolution shutter guarded - the server-side answer moves with it.
        MGPipeApplierNoteTextureStateMoved();

        // A RESPECIFY REDEFINES A STORE, SO THE PENDING UPLOADS AGAINST THE STORE IT REPLACES
        // GO WITH IT - AND ONLY THOSE. They are boxes and rects in a level's coordinate system
        // and that space has just been replaced, so a box kept across a shrink would have
        // Espryt upload past the end of the new level. But WHICH storage a respecify replaces
        // is the call's to say and the descriptor's to say nothing about:
        //
        //   - `level == nullptr` is a WHOLE-RESOURCE redefinition - every glBufferData /
        //     glBufferStorage, every glTexStorage* (which defines every level at once), every
        //     texture view - and every key goes.
        //   - a non-null `level` is ONE glTexImage*D on a mutable texture, which redefines that
        //     (uploadTarget, level) AND NOTHING ELSE. Dropping the other keys here would lose
        //     texels the client no longer owes: MG_State's AllocateStorage / MarkStorageDirty
        //     are per (uploadTarget, level) too, so the re-mark this call causes covers ITS
        //     level only, while every other level's dirty flag was cleared at its own emission
        //     (D-D5 step 1). The canonical sequence that loses them is glTexImage2D(0, data) ->
        //     draw (the shape is emitted and accumulated; Espryt bails because the texture is
        //     not yet mipmap-complete, Managers.cpp's incomplete-texture early return, which is
        //     the arm this set exists for) -> glTexImage2D(1, data), which under a blanket
        //     clear destroys level 0's entry before anything ever uploaded it.
        //
        //   - and a METADATA update (ID-18 M4) with a NULL level drops NOTHING. It refines the
        //     whole-resource arm rather than contradicting it: the rule is "the uploads against
        //     the storage this call REPLACES go with it", and a call whose storage-defining
        //     fields all equal the stored descriptor replaces no storage, so no level's
        //     coordinate system has moved and every pending box is still described in the
        //     space it was accumulated in. B re-emits the descriptor when a sticky bind bit
        //     moves - with a null level, deliberately - which can land between a
        //     glTexSubImage2D and the sync that consumes it; eating those texels there would be
        //     C1's bug with a different trigger, and just as silent.
        //
        //   - A NAMED LEVEL IS DROPPED WHETHER OR NOT THE DESCRIPTOR MOVED (P4a final review
        //     C-1, refining wire's W11 clause). The level pointer is the CALLER's statement that
        //     it reallocated that level, and the descriptor cannot contradict it: a non-base
        //     level redefined at a new size moves no descriptor field at all (the descriptor
        //     carries the base extent and the level count), so "identical storage fields" says
        //     nothing about that level's coordinate system, and a box kept against the old
        //     level would be uploaded past the end of the new one. The client's mask republish
        //     passes null, so this arm can never eat a standing upload on its behalf.
        //
        // A buffer never has a pending upload at all, so all three arms are inert for P3a's
        // half - which is also why a buffer is never classified as metadata-only (below).
        if (level != nullptr) {
            // The keys are unique by AccumulatePendingUpload's construction - it looks for the
            // pair before it appends - so this erases at most one entry and stops.
            for (auto it = record->PendingUploads.begin(); it != record->PendingUploads.end(); ++it) {
                if (it->UploadTarget != level->UploadTarget || it->Level != level->Level) continue;
                record->PendingUploads.erase(it);
                break;
            }
        } else if (metadataOnly) {
            // nothing to drop, deliberately.
        } else {
            record->PendingUploads.clear();
        }

#if MOBILEGL_BUILD_DISAGGREGATED
        // P5c (tx): the staged-texture store's defined-ness and drop bookkeeping rides THE SAME
        // scope rules as the pending-set drops above - a named level redefines that one
        // (uploadTarget, level), a whole-resource respecify drops every level, and a metadata
        // update replaces no storage and is not delivered. Without this the store could not say
        // whether a level exists at all (a null-data glTexImage*D carries no sub-data), and a
        // whole-resource respecify would leave levels keyed against a replaced coordinate
        // system. Textures only: a buffer's storage is the ops table's own Respecify hook, and
        // a renderbuffer has no levels.
        if (desc.Target != kMGPipeResourceTargetBuffer &&
            desc.Target != static_cast<Uint8>(MGPipeResourceTarget::Renderbuffer) && !metadataOnly &&
            g_resourceOps != nullptr && g_resourceOps->TextureRespecify != nullptr) {
            g_resourceOps->TextureRespecify(desc.Resource, desc, level);
        }
#endif

        // resource_respecify is the catalogue's only kNeedsAck call, and the per-record half
        // of that flag is MGPipeResourceRespecifyNeedsAck(desc): glBufferStorage is a real
        // synchronous allocation and the only entry point allowed a synchronous ack, while
        // glBufferData travels through the same call and must not acknowledge one. In monolith
        // the acknowledgement IS the return of this function - the applier is one call away -
        // so the predicate has nothing to gate here and is deliberately not branched on: a
        // branch whose arms were identical would be dead code the transport would then have to
        // find and remove. PipeCatalogueTest.ResourceRespecifyAcksOnlyImmutableStorage is what
        // keeps the predicate honest until the doorbell reads it.
        //
        // ID-18 M4 ASKS FOR "NO REALLOCATION ACK" ON A METADATA UPDATE, AND IT IS TRUE HERE BY
        // CONSTRUCTION RATHER THAN BY A BRANCH: the predicate is false for every target that is
        // not a buffer, and RespecifyRedefinesNoStorage refuses the buffer target outright, so
        // MGPipeResourceRespecifyNeedsAck(desc) is false for every record this applier
        // classifies as metadata-only. That is why the classification excludes buffers rather
        // than suppressing the ack afterwards - a suppression would have been a second rule the
        // transport would then have to learn.
        if (desc.Target != kMGPipeResourceTargetBuffer) return true;
        if (g_resourceOps != nullptr && g_resourceOps->Respecify != nullptr) {
            g_resourceOps->Respecify(desc.Resource, desc, initialBytes);
        }
        return true;
    }

    Bool MGPipeApplyResourceSubData(const MGPSubData& record, const void* bytes,
                                    const MGPSubRegion* regions) {
        // ONE CALL, TWO HALVES, and the branch is one comparison. For a buffer the applier
        // stores NOTHING per record - the contents are the backend's, and the range is the
        // backend's to land - so its whole job is the gate and the serial. For a texture there
        // is no backend hook at all and the whole job is the gate, the accumulated shape and
        // the serial.
        if (SubDataNamesABuffer(record)) {
            MOBILEGL_ASSERT(regions == nullptr,
                            "resource_subdata: the buffer half declares no sub-regions and carries none");
            return ApplyBufferWrite("resource_subdata", record, bytes, /*resident=*/false);
        }
        // AND THE TEXTURE HALF IS NOT "EVERYTHING ELSE". The table selector's own argument -
        // acting on the wrong table would have this applier act outside the storage the record
        // names - is exactly the argument for refusing a resource-target half that names no
        // texture, and Renderbuffer is the reachable one: it is enumerator 10 and therefore a
        // perfectly well-formed MGPSubData::Target, so without this a renderbuffer record would
        // accumulate a pending upload onto whatever TEXTURE holds slot N in the texture slot
        // space. Renderbuffers have no sub-data path at all, so no correct client can produce
        // one and this is a protocol fault rather than a dropped call.
        PinLiveHostWritesNamesABuffer(record, "resource_subdata");
        const Uint8 resourceTarget = MGPipeSubDataResourceTargetOf(record.Target);
        if (resourceTarget == kMGPipeResourceTargetBuffer ||
            resourceTarget == static_cast<Uint8>(MGPipeResourceTarget::Renderbuffer) ||
            resourceTarget >= static_cast<Uint8>(MGPipeResourceTarget::Count)) {
            // BOTH HALVES GO IN THE LINE. The packed field alone leaves a reader doing the
            // arithmetic the encoding exists to stop anyone doing by hand, and the upload half
            // is what says whether the emitter packed the pair the wrong way round.
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " resource_subdata {slot=%u, gen=%u}: the record's resource target names "
                                 "no texture to upload into (target=%u, resource target=%u, upload "
                                 "target=%u)",
                                 record.Res.Slot, record.Res.Gen, record.Target,
                                 static_cast<Uint32>(resourceTarget),
                                 static_cast<Uint32>(MGPipeSubDataUploadTargetOf(record.Target)));
            return false;
        }
        // P4a's belt, AFTER the target fault above and before the upload is accumulated: this
        // is the call whose acceptance clears a level's dirty flags (D-D5 / ID-18 M3), so it is
        // the one that turned "no consumer" into lost texels on Magma.
        if (NoP4aConsumer()) return false;
        return ApplyTextureUpload(record, bytes, regions);
    }

    void MGPipeApplyBufferSubDataResident(const MGPSubData& record, const void* bytes) {
        // `bytes` is the application's staging store and is valid for THE DURATION OF THE CALL
        // ONLY, which is why the backend hook copies rather than remembering the pointer.
        ApplyBufferWrite("buffer_subdata_resident", record, bytes, /*resident=*/true);
    }

    // THE FOUR BUFFER-ONLY CALLS, AND WHAT POLICES THAT. resource_flush_range,
    // resource_readback and map_persistent resolve through ResolveResource - i.e. against
    // g_applier.Resources and never the texture or renderbuffer table - which is right, because
    // a mapped range, a readback and a persistent donation exist for buffers only. They are
    // buffer-only BY CATALOGUE and not by check: MGPFlushRange and MGPReadback carry no kind
    // at all, and map_persistent's MGPHandleOnly is not asked for one, so there is nothing here
    // to compare and a mistyped record can only be caught where a discriminator exists.
    // unmap_persistent is that place, and it Fatals - see there. If a later phase gives any of
    // these three a kind, the gate belongs beside that field on the same terms.
    void MGPipeApplyResourceFlushRange(const MGPFlushRange& record, const void* bytes) {
        MGPipeResourceRecord* stored = ResolveResource("resource_flush_range", record.Res);
        if (stored == nullptr) return;

        const char* fault = BufferRangeFault(record.Offset, record.Size, stored->Desc.Width);
        // item-12 / R-13.2 / ID-37: the "a non-empty flush carries no bytes" fault is a MONOLITH
        // rule. Under an ACTIVE TRANSPORT resource_flush_range carries no companion pointer AT ALL
        // (contract table 1 row 20 / R-13.2): the covering resource_subdata already staged those
        // bytes into the server-owned StagedShadowStore (R-11), and Ops_H_FlushRange reads that
        // shadow, not the record. So a null `bytes` on a non-empty flush is the NORMAL split case,
        // not corruption - and a flush whose bytes were genuinely never staged is caught more
        // precisely downstream by R-11's Fatal{StageSnapshotTooNarrow}, which names the missing
        // subdata rather than the flush. The check stays exact under monolith
        // (MG_Config::Transport is a constexpr Monolith in a non-disaggregated build, so this is
        // `&& true` there and the codegen is unchanged). This was the only seam between the joint
        // inproc lane's 14/21 and 21/21: all seven PersistentCoherentMapScenario entries aborted
        // here (c1-v2.md 6).
        if (fault == nullptr && record.Size != 0 && bytes == nullptr &&
            MG_Config::Transport == MG_Config::TransportMode::Monolith) {
            fault = "a non-empty flush carries no bytes";
        }
        if (fault != nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " resource_flush_range {slot=%u, gen=%u, glName=%u}: %s "
                                 "(offset=%llu, size=%llu, storage=%u bytes)",
                                 record.Res.Slot, record.Res.Gen, stored->Desc.GlNameForDiag, fault,
                                 static_cast<unsigned long long>(record.Offset),
                                 static_cast<unsigned long long>(record.Size), stored->Desc.Width);
            return;
        }
        PinNoLiveHostWrites(*stored, record.Res, "resource_flush_range");
        ++stored->Serial;

        // record.AccessFlags are the application's REAL mapping flags and this applier does
        // not normalise them: the backend reads INVALIDATE_RANGE / INVALIDATE_BUFFER /
        // UNSYNCHRONIZED per call to choose its upload shape, and merging them here would take
        // that choice away from the side that pays for it.
        if (g_resourceOps != nullptr && g_resourceOps->FlushRange != nullptr) {
            g_resourceOps->FlushRange(record.Res, record, bytes);
        }
    }

    void MGPipeApplyResourceReadback(const MGPReadback& record) {
        MGPipeResourceRecord* stored = ResolveResource("resource_readback", record.Res);
        if (stored == nullptr) return;

        const char* fault = BufferRangeFault(record.Offset, record.Size, stored->Desc.Width);
        if (fault != nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " resource_readback {slot=%u, gen=%u, glName=%u}: %s "
                                 "(offset=%llu, size=%llu, storage=%u bytes)",
                                 record.Res.Slot, record.Res.Gen, stored->Desc.GlNameForDiag, fault,
                                 static_cast<unsigned long long>(record.Offset),
                                 static_cast<unsigned long long>(record.Size), stored->Desc.Width);
            return;
        }
        // The readback READS the store this flag describes, so it is one of the calls whose
        // answer would change silently the day a producer sets it (D-A4).
        PinNoLiveHostWrites(*stored, record.Res, "resource_readback");

        // NO SERIAL MOVES. A readback does not mutate the resource; it produces host bytes out
        // of it. Bumping here would tell the backend twin its store had changed and buy a
        // re-upload of what it had just been read out of.
        //
        // THE ANSWER TRAVELS BACK THROUGH THE REVERSE CHANNEL, NOT THROUGH THIS FUNCTION, and
        // the applier's contribution to that is the ORDER: the hook is called synchronously
        // and this returns only once it has finished, so the writeback into the client's
        // shadow and the mutation-epoch bump that must follow it have both happened before the
        // caller reads. The epoch bump stays server-side and happens AFTER the writeback,
        // never before - a rule the reverse channel needs as much as the forward one, because
        // a bump that overtook its writeback would leave the draw-clean memo stale behind it.
        //
        // With no table registered nothing answers, and nothing asks either: the frontend is
        // still on the path this call replaces.
        if (g_resourceOps != nullptr && g_resourceOps->Readback != nullptr) {
            g_resourceOps->Readback(record.Res, record);
        }
    }

    void MGPipeApplyResourceDestroy(const MGPHandleOnly& handle) {
        // THE KIND IS THE DISCRIMINATOR HERE, because a destroy carries no descriptor, and it is
        // a Fatal rather than an assertion for the reason resource_create's target is: the three
        // slot spaces are independent, so a destroy routed to the wrong table would drop the
        // record of a live object of another kind that happens to hold the same slot.
        Vector<MGPipeResourceRecord>* table = ResourceTableForKind(handle.Kind);
        if (table == nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " resource_destroy {slot=%u, gen=%u}: the handle names no resource kind "
                                 "(%u)",
                                 handle.Handle.Slot, handle.Handle.Gen, handle.Kind);
            return;
        }
        MGPipeResourceRecord* record = ResolveResourceIn(*table, "resource_destroy", handle.Handle);
        if (record == nullptr) return;

        // The record is dropped WHOLE and the generation is kept. The client allocator owns
        // the Gen bump, and it takes it on the next HANDOUT of the slot rather than on the
        // free, so a server-side bump here would put the two identities out of step and a
        // double free could skip a generation. Everything else goes: a stale read of a
        // destroyed slot must find nothing, not the extent of the buffer that used to be there.
        const Uint32 gen = record->Gen;
        *record = MGPipeResourceRecord{};
        record->Gen = gen;
        // P5c (rv): a destroyed object may still be BOUND somewhere the texture shutters guard;
        // the server's answer for them moves with the death rather than waiting for the next
        // unit-set record to say so.
        MGPipeApplierNoteTextureStateMoved();

        // The applier's own state is consistent before the backend hears the news, so a hook
        // that looked back at this applier could not see a resource that is already gone. The
        // CLIENT frees the slot after this returns, in that order, because the allocator
        // forgets the lifetime id on free and a notice resolved twice finds nothing the second
        // time.
        //
        // AND ONLY A BUFFER IS HANDED ON, for resource_create's reason: the op table is the
        // buffer family's, its Destroy takes a handle whose kind that backend registered for,
        // and a texture's death is read out of the record at the sync that would have used it.
#if MOBILEGL_BUILD_DISAGGREGATED
        // P5c (tx): with ONE exception - the staged-texture store is keyed by the handle, so
        // the death must reach it or a recycled slot's stale levels would answer for the
        // successor. This does not hand the texture to the buffer family's Destroy.
        if (static_cast<MGPipeKind>(handle.Kind) == MGPipeKind::Texture &&
            g_resourceOps != nullptr && g_resourceOps->TextureDestroy != nullptr) {
            g_resourceOps->TextureDestroy(handle.Handle);
        }
#endif
        if (static_cast<MGPipeKind>(handle.Kind) != MGPipeKind::Buffer) return;
        if (g_resourceOps != nullptr && g_resourceOps->Destroy != nullptr) {
            g_resourceOps->Destroy(handle.Handle);
        }
    }

    void* MGPipeApplyMapPersistent(const MGPHandleOnly& handle, Uint64 size, const void* seedBytes) {
        // COUNTED FIRST AND UNCONDITIONALLY, because the counter is acquisition ATTEMPTS and
        // not acquisitions: every one of them - mint or decline - needs an answer from the
        // resource owner, and under a transport a decline costs the same round trip as a mint.
        // Defined that way the number is identical in both modes, equals "one per storage
        // definition", and is non-zero and assertable today; defined as "round trips actually
        // taken" it would be 0 by construction in monolith and could never go red.
        ++g_applier.MapPersistentRoundtrips;

        MGPipeResourceRecord* record = ResolveResource("map_persistent", handle.Handle);
        if (record == nullptr) return nullptr;
        // THE ONE CALL A LATER PHASE ATTACHES THE PRODUCER TO. D-A4 and ARCHITECTURE.md name
        // the persistent-map push as exactly where HasLiveHostWrites gets set, so this is the
        // call the pin must sit on: a producer landing under it here is the semantic change
        // the flag exists to announce, and the wire is what refuses to let it arrive unnamed.
        PinNoLiveHostWrites(*record, handle.Handle, "map_persistent");

#if MOBILEGL_BUILD_DISAGGREGATED
        // R-6: A SPLIT BUILD RUNS AT TIER T2 AND DECLINES EVERY ACQUISITION, ALWAYS.
        //
        // The mint returns a raw void* that the client stores as the store's base
        // (BufferObject.cpp:238 / :603 / :658). Across a process boundary that address is
        // meaningless, and under `inproc` it is WORSE than meaningless: it happens to work,
        // so a lane that kept adoption alive would be green for a reason spawn cannot
        // reproduce, and persistent-map-push - an exit-gate counter - would be structurally
        // zero (PipeStats.cpp says so in as many words). Forcing T2 here rather than at the
        // three client call sites is what keeps `mpr` identical between the arms: the
        // roundtrip is COUNTED above, unconditionally, because a decline costs the same round
        // trip as a mint.
        //
        // The frontend already tolerates a decline in all three places, and has since P3a.
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith &&
            MG_Remote::Client::AdoptTierIsEmulate()) {
            return nullptr;
        }
#endif

        // NO SERIAL MOVES and NO DESCRIPTOR CHANGES: the donation re-mints the backend's own
        // driver object, which is a server-local event that the backend's own id generation
        // already catches, and the client's view of the store's extent is untouched by it.
        //
        // A NULL RETURN IS A DECLINE, not a failure - the acquisition is allowed to say no,
        // the caller already has that branch, and that is why the call is kOptional as well as
        // kReplySlot. An unregistered table declines every acquisition, which is exactly the
        // answer a build with no migrated backend should give.
        if (g_resourceOps == nullptr || g_resourceOps->MapPersistent == nullptr) return nullptr;
        return g_resourceOps->MapPersistent(handle.Handle, size, seedBytes);
    }

    void MGPipeApplyUnmapPersistent(const MGPHandleOnly& handle) {
        // THE SAME VERDICT resource_destroy GIVES, AND FOR THE SAME REASON. This is the only
        // one of the four buffer-only calls that carries a discriminator at all, and an
        // assertion is not a check: MOBILEGL_ASSERT compiles out at INFO, which is what all
        // three gate builds and every shipped build are, so a mistyped record used to walk
        // straight into ResolveResource and alias whatever BUFFER holds that slot - the three
        // slot spaces being independent - which is exactly the failure resource_destroy's
        // ResourceTableForKind Fatal exists to stop. Acting on another kind's storage is the
        // corrupt-record verdict, not the dropped-call one, so it moves no refusal counter.
        if (static_cast<MGPipeKind>(handle.Kind) != MGPipeKind::Buffer) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " unmap_persistent {slot=%u, gen=%u}: the persistent donation is the "
                                 "buffer family's and the handle names another kind (%u)",
                                 handle.Handle.Slot, handle.Handle.Gen, handle.Kind);
            return;
        }
        MGPipeResourceRecord* record = ResolveResource("unmap_persistent", handle.Handle);
        if (record == nullptr) return;

        // Never emitted by this phase's own paths - the donation is permanent for the store's
        // life and the retire happens inside the backend - so this exists to keep the pair
        // complete and to give the transport both halves.
        if (g_resourceOps != nullptr && g_resourceOps->UnmapPersistent != nullptr) {
            g_resourceOps->UnmapPersistent(handle.Handle);
        }
    }

    // ================================================================================
    // P3a: the five vertex-input entry points (D-G, D-H, D-I).
    //
    // THESE FIVE DISPATCH TO NOBODY, and that is not an omission: MGPipeResourceOps is the
    // RESOURCE family's table, and the vertex-input calls have no backend hook because the
    // backend does not act on them when they arrive. It reads them at its own draw-time sync,
    // out of the state below, which is what the three serials here are for - they are what
    // retires the twin's wrapping-Uint16-plus-identity patches, so a compare that used to ask
    // "is my Uint16 configuration version still the frontend's?" asks "is my Uint64 serial
    // still the server's?" instead.
    //
    // Nothing emits them in this package either: their dirty bits map to no subsystem, the two
    // subsystem bits are not in MG_Impl/Pipe/PipeFill.cpp's kMGPipeWiredSubsystems, and the
    // emitters beside them are stubs. The state below is therefore correct and unread until
    // the packages that wire both ends land.
    // ================================================================================

    void MGPipeApplyCreateVertexElements(const MGPVertexElements& desc, const void* blobBytes) {
        MOBILEGL_ASSERT(desc.Cso.Slot >= kMGPipeFirstAllocatableSlot,
                        "create_vertex_elements named the reserved slot 0");
        if (desc.Cso.Slot < kMGPipeFirstAllocatableSlot) return;

        // THE COUNTS ARE CHECKED BEFORE A BYTE OF THE BLOB IS TOUCHED, and the check is the
        // record's own self-description: the blob is MGPVertexAttribWire[AttributeCount]
        // immediately followed by MGPVertexBindingPointWire[BindingPointCount]. THIS is the
        // reason the second view travels at all - a record that declares a BindingPointCount
        // it does not carry would otherwise be a shape this gate had to police forever with
        // nothing to police it against.
        //
        // Both counts are bounded by GL's attribute limit, which is also the size of the two
        // arrays they are unpacked into, so the bound and the destination cannot drift apart.
        // THE COUNTS ARE WHAT BOUNDS THE READ; the declared blob length is a cross-check.
        //
        // THE BLOB RULE IS THE SAME ONE resource_subdata IS HELD TO (SubDataBoxFault above,
        // and MGPipeTypes.h states it on both records): a non-zero Blob.Size must be exactly
        // the length the record's other fields describe, and a zero Blob.Size means "this
        // record does not declare its blob" - which is what a monolith emission is, since the
        // bytes travel beside the record through blobBytes and no MGPBlobRef is filled. One
        // rule, both blob-carrying families: a transport that fills the field gets a real gate
        // on the first truncated record, and a client that leaves it zero does not abort a
        // verify build over a field it never used.
        const Uint64 attributeBytes = Uint64{desc.AttributeCount} * sizeof(MGPVertexAttribWire);
        const Uint64 bindingBytes = Uint64{desc.BindingPointCount} * sizeof(MGPVertexBindingPointWire);
        const Uint64 declared = attributeBytes + bindingBytes;
        const char* fault = nullptr;
        if (desc.AttributeCount > kMGPipeMaxVertexAttribs) {
            fault = "the declared attribute count is above GL's attribute limit";
        } else if (desc.BindingPointCount > kMGPipeMaxVertexAttribs) {
            fault = "the declared binding-point count is above GL's attribute limit";
        } else if (desc.Blob.Size != 0 && desc.Blob.Size != declared) {
            fault = "the declared blob length is not the byte length the two counts describe";
        } else if (declared != 0 && blobBytes == nullptr) {
            fault = "a non-empty blob carries no bytes";
        }
        if (fault != nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " create_vertex_elements {slot=%u, gen=%u}: %s (attributes=%u, "
                                 "bindingPoints=%u, that describes %llu bytes, blob declares %llu)",
                                 desc.Cso.Slot, desc.Cso.Gen, fault, desc.AttributeCount,
                                 desc.BindingPointCount, static_cast<unsigned long long>(declared),
                                 static_cast<unsigned long long>(desc.Blob.Size));
            return;
        }

        MGPipeVertexElementsRecord* recordAt =
            RecordAt(g_applier.VertexElementsCsos, desc.Cso.Slot, kMGPipeMaxVertexElementsSlots);
        if (recordAt == nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " create_vertex_elements {slot=%u, gen=%u}: the slot is outside the record "
                                 "table's bound (%u)",
                                 desc.Cso.Slot, desc.Cso.Gen, kMGPipeMaxVertexElementsSlots);
            return;
        }
        MGPipeVertexElementsRecord& record = *recordAt;
        // A RE-CREATE ON THE SAME HANDLE IS HOW A CONFIGURATION CHANGE TRAVELS - the handle is
        // minted per frontend vertex array and a generation moves only when a slot is reused -
        // so an existing record of the same identity keeps its serial and counts up from it. A
        // record of a DIFFERENT identity is a recycled slot and starts over, or the walks below
        // would read the previous occupant's attributes out of the array's tail.
        if (!record.Live || record.Gen != desc.Cso.Gen) {
            record = MGPipeVertexElementsRecord{};
            record.Gen = desc.Cso.Gen;
        }

        // Zeroed first, so a configuration that shrinks does not leave the entries above its
        // new count describing the one before it. memcpy rather than a typed store because a
        // blob pointer carries no alignment guarantee.
        record.Attributes = {};
        record.BindingPoints = {};
        const Uint8* blob = static_cast<const Uint8*>(blobBytes);
        if (attributeBytes != 0) {
            std::memcpy(record.Attributes.data(), blob, static_cast<SizeT>(attributeBytes));
        }
        if (bindingBytes != 0) {
            std::memcpy(record.BindingPoints.data(), blob + attributeBytes, static_cast<SizeT>(bindingBytes));
        }
        record.AttributeCount = desc.AttributeCount;
        record.BindingPointCount = desc.BindingPointCount;
        record.Live = true;
        // Serial 0 means "never created", so the first create of an identity lands on 1 and a
        // backend twin that has synced nothing can never accidentally match a live record.
        ++record.ContentSerial;
        // AND IT DOES NOT REBIND. A create on the bound handle changes what the binding points
        // AT, which the serial already says; a create on any other handle must not steal the
        // binding.
    }

    void MGPipeApplyBindVertexElements(const MGPHandleOnly& handle) {
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::VertexElementsCso),
                        "bind_vertex_elements on kind %u", handle.Kind);
        // The null handle is legal and means "no vertex array bound" - GL's unbound state is a
        // state, not an error, and the backend has a branch for it.
        if (MGPipeHandleIsNull(handle.Handle)) {
            g_applier.BoundVertexElements = kMGPipeNullHandle;
            return;
        }
        // A dead handle leaves the PREVIOUS binding untouched, which is bind_render_state's
        // precedent for the same question, and is counted like every other refusal.
        const MGPipeVertexElementsRecord* record = ResolveVertexElements("bind_vertex_elements", handle.Handle);
        if (record == nullptr) return;
        g_applier.BoundVertexElements = handle.Handle;
    }

    void MGPipeApplyDeleteVertexElements(const MGPHandleOnly& handle) {
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::VertexElementsCso),
                        "delete_vertex_elements on kind %u", handle.Kind);
        // A death notice on a record this applier does not have is the SAME refusal every
        // other entry point makes, with the same verdict and the same counter: n1's
        // inconsistency (a bare `return` with no assertion and no reason) is closed by routing
        // it through the resolver rather than by giving it a private answer. It is also the
        // one refusal a legal sequence produces - the teardown order beside
        // kResourceRefusalNote - which is why it stays a no-op.
        MGPipeVertexElementsRecord* record = ResolveVertexElements("delete_vertex_elements", handle.Handle);
        if (record == nullptr) return;

        // Dropped whole, generation kept, for resource_destroy's reason: the client allocator
        // owns the Gen bump and takes it on the next handout of the slot. Emitted from ONE
        // place - the frontend object's death notice - so there is no second path to keep in
        // step with this one.
        const Uint32 gen = record->Gen;
        *record = MGPipeVertexElementsRecord{};
        record->Gen = gen;
        if (g_applier.BoundVertexElements == handle.Handle) {
            g_applier.BoundVertexElements = kMGPipeNullHandle;
        }
    }

    void MGPipeApplySetVertexBuffers(const MGPVertexBuffers& hdr, const MGPVertexBuffer* tail) {
        // The window is the bound: Start + Count entries land in an array of exactly GL's
        // attribute limit, and a var-tail header that describes more than its destination can
        // hold is the same class of fault as a blob outside its segment.
        const Uint64 end = Uint64{hdr.Start} + Uint64{hdr.Count};
        const char* fault = nullptr;
        if (end > kMGPipeMaxVertexAttribs) {
            fault = "the window runs past GL's attribute limit";
        } else if (hdr.Count != 0 && tail == nullptr) {
            fault = "a non-empty set carries no entries";
        }
        if (fault != nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " set_vertex_buffers {start=%u, count=%u, hash=%llu}: %s (the applier holds "
                                 "%u entries)",
                                 hdr.Start, hdr.Count, static_cast<unsigned long long>(hdr.ContentHash),
                                 fault, kMGPipeMaxVertexAttribs);
            return;
        }

        // Copied into the window the record declares and nowhere else. The entries outside it
        // are not cleared: this record is "the last set as received", and a set that names
        // four entries has said nothing about the rest.
        for (Uint32 i = 0; i < hdr.Count; ++i) {
            g_applier.VertexBuffers[hdr.Start + i] = tail[i];
        }
        g_applier.VertexBufferStart = hdr.Start;
        g_applier.VertexBufferCount = hdr.Count;

        // THE RAW VALUE IS STORED, NOT A RESOLVED SHIFT, and the resolution is one step
        // further out on purpose. Whether the fetch shift has to be emulated at all is a
        // BACKEND CAPABILITY - a device with native base-instance support does it in hardware
        // and shifts nothing - and emulation is server-owned, so the answer belongs to the
        // backend arm that computes the per-attribute byte shift out of this value and each
        // attribute's own stride and divisor. This applier is below MG_Backend and may not ask
        // the question; storing the raw value keeps the one answer in the one place that can
        // give it.
        //
        // The client sends it once per SET rather than per entry, and it is a ContentHash
        // input: set_vertex_buffers is suppressed on an unchanged hash, so a base instance
        // that moved while the buffer set did not would otherwise never arrive and the server
        // would keep the previous shift.
        g_applier.VertexFetchBaseInstance = hdr.BaseInstance;
        ++g_applier.VertexBuffersSerial;
    }

    void MGPipeApplySetIndexBuffer(const MGPIndexBuffer& record) {
        // Stored verbatim, with no gate over it, and each of the three fields has its own
        // reason to be taken as sent:
        //   - Res may legitimately be the null handle: no element-array buffer is bound, which
        //     is the state a client-memory index draw is in;
        //   - Offset and IndexSize are the DRAW's, not the binding's, and are 0 and 0 until a
        //     draw supplies them - so there is no extent here to check a range against, and
        //     the draw verb overrides them anyway;
        //   - it is an INDEPENDENT call and NOT a subset of the vertex-elements configuration
        //     (D5): a rebind of the index slot must move this serial without touching the
        //     configuration's, which is exactly what the backend's two separate compares need.
        g_applier.IndexBuffer = record;
        ++g_applier.IndexBufferSerial;
    }

    void MGPipeDeriveRenderStateFields(PipeInputs& inputs) {
        // The derivation itself lives in MGPipeApplyAccess above, because that is the one
        // struct PipeInputs names as a friend - see D5 there for what it recomputes and which
        // getter each line was transcribed from.
        MGPipeApplyAccess::DeriveRenderStateFields(inputs, kMGPipeAllGlobalChunks);
    }

    void MGPipeDeriveRenderStateFieldsForChunks(PipeInputs& inputs, Uint32 globalChunkBits) {
        MGPipeApplyAccess::DeriveRenderStateFields(inputs, globalChunkBits);
    }

    // ================================================================================
    // P4a: the fifteen object and working-state entry points
    // ================================================================================
    //
    // NOT ONE OF THEM DISPATCHES TO A BACKEND FUNCTION POINTER, and that is the structural
    // decision the whole phase rests on rather than an omission. Nothing in these families
    // reaches the backend at GL-call time today - texture storage only marks a level dirty,
    // texture parameters run from the draw-time sync, renderbuffer storage is allocated inside
    // SyncToBackend, a sampler twin is created lazily from the program pass, and the
    // framebuffer, the unit sets and the program are all resolved at PrepareForDraw - so every
    // call below is either an OBJECT RECORD the applier stores or WORKING STATE the applier
    // stores, and the backend reads the applier at the sync points it already has, keyed on a
    // server-owned Serial instead of a frontend version. MGPipeResourceOps is unchanged.
    //
    // THE SAME THREE STEPS, IN THE SAME ORDER, AS P3a's NINE: resolve the handle against this
    // applier's record, check the record against its own declared extent, and only then move
    // the record. The two verdicts are the same two, and the difference between them is the
    // whole of this file's discipline - a call naming a record this applier does not have is a
    // DEFINED NO-OP that is COUNTED (RefusedObjectCalls), because one legal sequence produces
    // it; a record that would make the server index or allocate outside its own storage is
    // Fatal{ProtocolCorruption} with the record's identity in the line.
    //
    // NOTHING EMITS ANY OF THEM IN THIS PACKAGE. The four subsystem bits are not in
    // kMGPipeWiredSubsystems, every client emitter beside them is still a stub, and no backend
    // reads the records yet - so the state below is correct and unread until the packages that
    // wire both ends land, and this tree is behaviourally identical to the contract commit's.

    // ================================================================================
    // w1: the framebuffer record, per bound target
    // ================================================================================

    void MGPipeApplySetFramebufferState(const MGPFramebufferState& state) {
        // GL HAS TWO INDEPENDENT FRAMEBUFFER BINDINGS AND THIS RECORD DESCRIBES ONE FRAMEBUFFER,
        // so Target says which binding - if any - the description also BINDS it to. Both is one
        // object bound to both and moves both handles from one call; Named
        // (MGPipeFramebufferTarget::Named) moves neither, which is how the DSA entry points
        // describe a framebuffer they are about to blit into or clear without claiming it is
        // bound (ID-19). A value above Named is not a target this server has, and guessing one
        // would put a draw's attachments into the read binding or the other way round.
        const char* fault = nullptr;
        if (state.Target >= static_cast<Uint8>(MGPipeFramebufferTarget::Count)) {
            fault = "the record names no framebuffer binding target";
        }
        // THE RECORD IS ADDRESSED BY THE FRAMEBUFFER HANDLE NOW, so the handle has to be one.
        // The null handle is refused rather than stored: {0,0} is what "nothing is bound to this
        // binding" reads as on the other side of BoundFramebuffer, and a record installed there
        // would be answered to every caller asking about an EMPTY binding. Every emitter has a
        // handle for every framebuffer it describes - kMGPipeDefaultFramebuffer {0,1} for the
        // default one, a minted {slot, gen} otherwise - so a null here is a malformed record.
        else if (MGPipeHandleIsNull(state.Fbo)) {
            fault = "the record names the null framebuffer handle";
        }
        // And the same bound gate the five object tables carry, for the same reason: Slot is a
        // client-supplied Uint32 arriving in a payload and this is the one number in the call
        // that reaches an allocator.
        else if (state.Fbo.Slot >= kMGPipeMaxFramebufferSlots) {
            fault = "the framebuffer slot is outside the record table's bound";
        }
        // THE DRAW-BUFFER ARRAY IS AN INDEX INTO THIS RECORD'S OWN Color[], and -1 is NONE. An
        // entry outside that range would have the server read a colour attachment the record
        // does not carry: the wire array is 8 wide, the driver's MaxColorAttachments is not
        // clamped to it, and a truncated record arriving here as a plausible index is exactly
        // what the framebuffer subsystem's cap refusal exists to prevent upstream.
        for (Uint32 i = 0; fault == nullptr && i < kMGPipeMaxColorAttachments; ++i) {
            if (state.DrawBuffers[i] < -1 ||
                state.DrawBuffers[i] >= static_cast<Int8>(kMGPipeMaxColorAttachments)) {
                fault = "a draw-buffer entry names a colour attachment outside the record's own array";
            }
        }
        if (fault != nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " set_framebuffer_state {slot=%u, gen=%u, target=%u}: %s (the record "
                                 "carries %u colour attachments)",
                                 state.Fbo.Slot, state.Fbo.Gen, state.Target, fault,
                                 kMGPipeMaxColorAttachments);
            return;
        }

        // NO HANDLE IS RESOLVED HERE AND NONE MAY BE. A framebuffer has a handle but no wire
        // lifetime - the catalogue has no framebuffer create and no framebuffer destroy,
        // because a framebuffer is state and this call is the only one that names one - so
        // there is nothing to look up, this entry point never counts a refusal, and the record
        // it writes OVERWRITES whatever the slot held. That last part is the point rather than
        // a compromise: the slot's previous occupant is dead by construction (the client frees
        // a framebuffer's slot when the object dies and the allocator bumps the generation on
        // reuse), and the record arriving now describes whatever holds the slot today.
        //
        // The surfaces' Res handles are not resolved either: an attachment PINS its texture,
        // and in monolith the frontend's own SharedPtr is that keep-alive, so a refusal here
        // would be enforcing a lifetime rule monolith cannot need and split has not defined.
        // P4a's belt, after the record's own shape checks and before the table is touched. This
        // entry point is the ONE the client can reach without passing PipeFill's gate -
        // GL_Framebuffer.cpp's PipePublishFramebufferByName calls the emitter directly at the
        // fifteen DSA sites - so on a backend with no consumer this is where those records
        // stop. RefusedNoConsumer and not RefusedObjectCalls: the framebuffer family's counter
        // contract (see the header) is that no framebuffer call ever moves that one.
        if (NoP4aConsumer()) return;

        MGPipeFramebufferRecord* record =
            RecordAt(g_applier.FramebufferRecords, state.Fbo.Slot, kMGPipeMaxFramebufferSlots);
        // Unreachable: the bound was checked above, before anything moved. The null check is
        // what makes that an argument rather than an assumption.
        if (record == nullptr) return;
        record->Gen = state.Fbo.Gen;
        record->Live = true;
        record->State = state;

        // AND ONLY THEN THE BINDINGS. Named moves neither, which is the whole of the fourth
        // target: "this record describes the framebuffer it names; no binding changes".
        if (state.Target == static_cast<Uint8>(MGPipeFramebufferTarget::Draw) ||
            state.Target == static_cast<Uint8>(MGPipeFramebufferTarget::Both)) {
            g_applier.BoundFramebuffer[static_cast<SizeT>(MGPipeFramebufferTarget::Draw)] = state.Fbo;
        }
        if (state.Target == static_cast<Uint8>(MGPipeFramebufferTarget::Read) ||
            state.Target == static_cast<Uint8>(MGPipeFramebufferTarget::Both)) {
            g_applier.BoundFramebuffer[static_cast<SizeT>(MGPipeFramebufferTarget::Read)] = state.Fbo;
        }
        // ONE SERIAL FOR THE FAMILY, and it moves once per applied record - a Both record is one
        // record and a Named record is one record. It is what retires the four g_fboSynced*
        // arrays and the twin's own {slot version, object version, backend id generation}
        // quadruple: a compare that used to ask "is my memo still the frontend's" asks "is my
        // serial still the server's". A Named record moves it for the same reason a bound one
        // does - a twin that memoised a framebuffer's attachments has to hear that they moved,
        // and whether the framebuffer happens to be bound is not that question.
        ++g_applier.FramebufferSerial;
    }

    // THE READ SIDE OF THE TABLE (ID-19(b)/(d)). Package D asks it by handle at every DSA entry
    // point; the two accessors under it ask it through a binding, which is the question package
    // E's SyncCurrentFBOByRecord has always asked.
    const MGPFramebufferState* MGPipeApplierState::FramebufferRecordFor(MGPipeHandle fbo) const {
        // "Nothing is bound" and "no record" are BOTH null and only the second of them is worth
        // a word: the null handle is what a make-current leaves in BoundFramebuffer and what an
        // unbound target reads as, it arrives here on every draw of a context that has not
        // described its framebuffers yet, and logging it would be a storm about the ordinary
        // transitional state. It is separated first so the generation test below can be loud.
        if (MGPipeHandleIsNull(fbo)) return nullptr;
        if (fbo.Slot >= FramebufferRecords.size()) return nullptr;
        const MGPipeFramebufferRecord& record = FramebufferRecords[fbo.Slot];
        // Nothing has been written at this slot. Also not an error, and also silent: it is what
        // every framebuffer looks like before its first set_framebuffer_state, and the caller's
        // answer to it is to run its pre-handle arm.
        if (!record.Live) return nullptr;
        if (record.Gen != fbo.Gen) {
            // AND THIS ONE IS. A record exists at the slot and it describes a DIFFERENT object:
            // either the caller is holding a handle whose framebuffer died and whose slot has
            // been recycled, or an emitter minted the successor without ever describing it. On
            // the handle arm the visible consequence is a DSA blit or clear into a driver
            // framebuffer that never got its attachments, which is precisely the failure
            // ID-19 exists to close - so this is the loud refusal P3a's lesson demands and
            // never a quiet fall-back.
            ++StaleFramebufferRecordLookups;
            MGLOG_E_ONCE("MGPipe: the framebuffer record at slot %u is generation %u and the "
                         "lookup named generation %u; refusing it rather than answering with "
                         "another framebuffer's attachments",
                         fbo.Slot, record.Gen, fbo.Gen);
            return nullptr;
        }
        return &record.State;
    }

    const MGPFramebufferState* MGPipeApplierState::DrawFramebuffer() const {
        return FramebufferRecordFor(BoundFramebuffer[static_cast<SizeT>(MGPipeFramebufferTarget::Draw)]);
    }

    const MGPFramebufferState* MGPipeApplierState::ReadFramebuffer() const {
        return FramebufferRecordFor(BoundFramebuffer[static_cast<SizeT>(MGPipeFramebufferTarget::Read)]);
    }

    // ================================================================================
    // w2: sampler CSOs, sampler views and the three unit sets
    // ================================================================================

    void MGPipeApplyCreateSamplerState(const MGPSamplerDesc& desc, const SamplerParameters* parameters) {
        MOBILEGL_ASSERT(desc.Cso.Slot >= kMGPipeFirstAllocatableSlot,
                        "create_sampler_state named the reserved slot 0");
        if (desc.Cso.Slot < kMGPipeFirstAllocatableSlot) return;

        // THE ONE BLOB RULE, on the family's own blob: a non-zero Parameters.Size must be
        // exactly one SamplerParameters, and a zero means "this record does not declare its
        // blob" - which is what a monolith emission is, because the value rides beside the
        // record through the companion pointer. Either way the bytes read are bounded by the
        // TYPE and not by the declared length, so the length is a cross-check and never the
        // safety property.
        const char* fault = nullptr;
        if (desc.Parameters.Size != 0 && desc.Parameters.Size != sizeof(SamplerParameters)) {
            fault = "the declared blob length is not one SamplerParameters";
        } else if (parameters == nullptr) {
            fault = "the record declares no parameters and carries none";
        }
        if (fault != nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " create_sampler_state {slot=%u, gen=%u}: %s (one SamplerParameters is "
                                 "%llu bytes, the blob declares %llu)",
                                 desc.Cso.Slot, desc.Cso.Gen, fault,
                                 static_cast<unsigned long long>(sizeof(SamplerParameters)),
                                 static_cast<unsigned long long>(desc.Parameters.Size));
            return;
        }

        MGPipeSamplerCsoRecord* recordAt =
            RecordAt(g_applier.SamplerCsos, desc.Cso.Slot, kMGPipeMaxSamplerCsoSlots);
        if (recordAt == nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " create_sampler_state {slot=%u, gen=%u}: the slot is outside the record "
                                 "table's bound (%u)",
                                 desc.Cso.Slot, desc.Cso.Gen, kMGPipeMaxSamplerCsoSlots);
            return;
        }
        // P4a's belt, after the blob rule and the slot bound and before the record moves.
        if (NoP4aConsumer()) return;

        MGPipeSamplerCsoRecord& record = *recordAt;
        // A CREATE STARTS THE RECORD OVER AND LEAVES Serial AT 0; A RE-ISSUE ON A LIVE IDENTITY
        // COUNTS UP. The first half is what stops a recycled slot contributing one field of its
        // predecessor, and it is why a fresh backend twin starting its own synced serial at 0
        // agrees with a fresh record without either side publishing anything. The second is how
        // a value change travels on a handle whose Gen moves only on slot reuse - and it is a
        // mutation, so it moves the serial.
        if (!record.Live || record.Gen != desc.Cso.Gen) {
            record = MGPipeSamplerCsoRecord{};
            record.Gen = desc.Cso.Gen;
        } else {
            ++record.Serial;
        }
        record.Live = true;
        // BY VALUE, INCLUDING borderColorForm. All three border representations are always
        // numerically populated, so the value alone cannot say which driver entry point to use,
        // and the backend's redundancy filter compares all four. The three trailing padding
        // bytes are why MOBILEGL_PIPE_VERIFY compares this FIELD BY FIELD through
        // PipeFields.def's MGP_FIELDS_SamplerParameters rather than as bytes.
        record.Params = *parameters;
    }

    void MGPipeApplyDeleteSamplerState(const MGPHandleOnly& handle) {
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::SamplerCso),
                        "delete_sampler_state on kind %u", handle.Kind);
        MGPipeSamplerCsoRecord* record =
            ResolveObject(g_applier.SamplerCsos, "delete_sampler_state", handle.Handle);
        if (record == nullptr) return;

        // Dropped whole, generation kept, for resource_destroy's reason: the client allocator
        // owns the Gen bump and takes it on the next handout of the slot, so a server-side bump
        // here would put the two identities out of step.
        //
        // AND THE UNIT SET IS NOT SWEPT. bind_sampler_states is "the last set as received" and
        // the client re-emits the whole window from its own resolved set at the next verb, so
        // walking 192 entries here to blank a handle the next set is about to overwrite would
        // buy nothing and would break the one rule the window has.
        const Uint32 gen = record->Gen;
        *record = MGPipeSamplerCsoRecord{};
        record->Gen = gen;
    }

    void MGPipeApplyCreateSamplerView(const MGPSamplerView& view) {
        MOBILEGL_ASSERT(view.Cso.Slot >= kMGPipeFirstAllocatableSlot,
                        "create_sampler_view named the reserved slot 0");
        if (view.Cso.Slot < kMGPipeFirstAllocatableSlot) return;

        MGPipeSamplerViewRecord* recordAt =
            RecordAt(g_applier.SamplerViewCsos, view.Cso.Slot, kMGPipeMaxSamplerViewSlots);
        if (recordAt == nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " create_sampler_view {slot=%u, gen=%u}: the slot is outside the record "
                                 "table's bound (%u)",
                                 view.Cso.Slot, view.Cso.Gen, kMGPipeMaxSamplerViewSlots);
            return;
        }
        // P4a's belt, after the slot bound and before the record moves.
        if (NoP4aConsumer()) return;

        MGPipeSamplerViewRecord& record = *recordAt;
        // RE-ISSUING ON THE SAME HANDLE IS HOW A RESTRICTION CHANGE TRAVELS - a view is
        // identity-addressed one per texture object, minted off that object's lifetime id, and
        // Gen moves only on slot reuse - so it bumps the serial and starts nothing over. AND IT
        // DOES NOT REBIND: a re-issue on a view some unit is holding changes what that unit
        // sees, which the serial already says; it must not make some other unit see it.
        if (!record.Live || record.Gen != view.Cso.Gen) {
            record = MGPipeSamplerViewRecord{};
            record.Gen = view.Cso.Gen;
        } else {
            ++record.Serial;
        }
        record.Live = true;
        record.View = view;

        // THE TEXTURE'S OWN ViewCso IS WRITTEN HERE, and it is a SILENT lookup rather than a
        // resolved one: the texture-resource bit and the sampler bit are independent, so a view
        // arriving before - or without - the texture record is an ordering fact and not a
        // refusal. When the record is there this is the back-pointer that lets a sync reach a
        // texture's view without walking every view the applier holds.
        MGPipeResourceRecord* texture = FindIn(g_applier.TextureResources, view.Texture);
        if (texture != nullptr) texture->ViewCso = view.Cso;
    }

    void MGPipeApplyDeleteSamplerView(const MGPHandleOnly& handle) {
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::SamplerViewCso),
                        "delete_sampler_view on kind %u", handle.Kind);
        MGPipeSamplerViewRecord* record =
            ResolveObject(g_applier.SamplerViewCsos, "delete_sampler_view", handle.Handle);
        if (record == nullptr) return;

        // The back-pointer goes first, while the record that names the texture still exists,
        // and only if that texture still names THIS view - a texture whose view has already
        // been re-minted must not have the new handle cleared out from under it.
        MGPipeResourceRecord* texture = FindIn(g_applier.TextureResources, record->View.Texture);
        if (texture != nullptr && texture->ViewCso == handle.Handle) {
            texture->ViewCso = kMGPipeNullHandle;
        }
        const Uint32 gen = record->Gen;
        *record = MGPipeSamplerViewRecord{};
        record->Gen = gen;
    }

    Bool MGPipeApplySetTextureParams(const MGPTextureParams& params) {
        // P4a's belt, and FIRST here because this call's first act is a resolution: with no
        // consumer no texture create was accepted, so resolving would report the absence as
        // RefusedObjectCalls - the counter that means a seam defect - for the designed state.
        if (NoP4aConsumer()) return false;

        // ADDRESSED BY RESOURCE AND BY NOTHING ELSE, which is the whole point of the call: a
        // texture that is only an FBO attachment, only an image-unit binding or only a
        // glCopyImageSubData endpoint has no sampler view to hang its parameters on, and the
        // READ-attachment case reaches no parameter push at all today. The record exists the
        // moment the parameters move, whether or not anything is bound.
        MGPipeResourceRecord* record =
            ResolveObject(g_applier.TextureResources, "set_texture_params", params.Res);
        if (record == nullptr) return false;

        // EVERY ITextureObject OWNS A SamplerObject, so the built-in sampler CSO is not
        // optional and a null handle is not "no sampler" - it is a record that would have the
        // backend sample a texture with whatever filter and wrap state the unit last left
        // behind. GL 4.6 table 23.18 makes filter/wrap/compare/border sampler state and Espryt
        // pushes it onto the TEXTURE with glTexParameter*, so this handle is the only thing
        // that says which values those are.
        if (MGPipeHandleIsNull(params.BuiltinSampler)) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " set_texture_params {slot=%u, gen=%u, glName=%u}: the record names no "
                                 "built-in sampler CSO, and every texture object owns one",
                                 params.Res.Slot, params.Res.Gen, record->Desc.GlNameForDiag);
            return false;
        }
        // AND THE CSO IT NAMES IS NOT RESOLVED. The sampler subsystem is its own bit and may be
        // clear while the texture bit is set, so a record that names a CSO this applier has not
        // been told about is an ORDERING fact rather than a corrupt one; refusing it would make
        // one legal A/B arm drop every texture's parameters. What the record carries is the
        // identity, and the identity is what the backend resolves at its own sync point.

        record->Params = params;
        // The serial moves BEFORE anything downstream is told, for ApplyBufferWrite's reason.
        // It plus the record's own ForceResync / SamplerResync bytes are what retire the twin's
        // m_syncedTextureParamsVersion + m_forceTextureParamsResync pair - and the two resync
        // bytes are CARRIED, never cleared here: the server ORs them into its own flags and
        // clears its own copy, and the client never clears a server flag.
        ++record->ParamsSerial;
        // P5c (rv): a parameter change is what the sampling-resolution shutter guarded.
        MGPipeApplierNoteTextureStateMoved();
        return true;
    }

    // The three of them, and NO STAGE DIMENSION on any of them: MobileGL's texture-unit space
    // is one merged array of 192, the same unit may be sampled from two stages, and stage is
    // derived server-side from the reflection archive only where the target API needs it.
    //
    // A NULL HANDLE IN A TAIL ENTRY IS LEGAL EVERYWHERE HERE and is not a refusal: a unit the
    // program does not resolve carries a null view, a unit with no sampler object carries a
    // null sampler CSO (the texture's built-in sampler applies then, exactly as today), and a
    // unit with no texture carries a null resource. None of the three is resolved against a
    // record either - a set is WORKING STATE, the records it names are OBJECT state, and the
    // backend resolves the pair at its own sync point where both are current.
    //
    // P4a's belt sits IN FRONT of ApplyUnitWindow on all three, and this is the one place it
    // is not last: ApplyUnitWindow validates the window and writes it in the same step, so
    // there is no point between the two to stand at. The cost is that on a backend with no
    // consumer a malformed window is declined rather than Fatal - which is the right trade the
    // one way round it can be made, because that backend would never have been handed the
    // window at all (the client's gate stops it) and the shipped configuration that DOES
    // consume these still trips the wire.
    void MGPipeApplySetSamplerViews(const MGPSamplerViews& hdr, const MGPBoundView* tail) {
        if (NoP4aConsumer()) return;
        if (!ApplyUnitWindow("set_sampler_views", hdr.Start, hdr.Count, hdr.ContentHash, tail,
                             g_applier.BoundSamplerViews, g_applier.SamplerViewStart,
                             g_applier.SamplerViewCount)) {
            return;
        }
        ++g_applier.SamplerViewsSerial;
        // P5c (rv): a texture bind is what the bind-generation shutter guarded.
        MGPipeApplierNoteTextureStateMoved();
    }

    void MGPipeApplyBindSamplerStates(const MGPSamplerStates& hdr, const MGPipeHandle* tail) {
        if (NoP4aConsumer()) return;
        if (!ApplyUnitWindow("bind_sampler_states", hdr.Start, hdr.Count, hdr.ContentHash, tail,
                             g_applier.BoundSamplerStates, g_applier.SamplerStateStart,
                             g_applier.SamplerStateCount)) {
            return;
        }
        ++g_applier.SamplerStatesSerial;
        // P5c (rv): a sampler bind or parameter change is what the shutters guarded.
        MGPipeApplierNoteTextureStateMoved();
    }

    void MGPipeApplySetShaderImages(const MGPShaderImages& hdr, const MGPImageView* tail) {
        // The image set carries InternalFormat and Access per entry and BOTH are live
        // glBindImageTexture state the format-less image bake keys on, so they are stored as
        // sent and recast on the server - the record carries the application's format, and the
        // bind-format recast that turns a GL_RG32F bind into something 19 of 26 non-core
        // formats on Adreno will accept is the backend's, not this applier's.
        if (NoP4aConsumer()) return;
        if (!ApplyUnitWindow("set_shader_images", hdr.Start, hdr.Count, hdr.ContentHash, tail,
                             g_applier.BoundShaderImages, g_applier.ShaderImageStart,
                             g_applier.ShaderImageCount)) {
            return;
        }
        ++g_applier.ShaderImagesSerial;
        // P5c (rv): an image bind moves the texture bind generation's guarded set too.
        MGPipeApplierNoteTextureStateMoved();
    }

    // ---------------------------------------------------------------------------------
    // P5e's two entry points, DECLARED BY c0e (MG_Remote/CONTRACT-P5E.md §1)
    // ---------------------------------------------------------------------------------
    //
    // set_shader_buffers HAS ITS BODY (package sb); set_program_bindings still refuses by name
    // until package pg writes its own. A record that reached an unbodied one would be a binding
    // set the server accepted and dropped, so the answer there is a named abort rather than a
    // quiet return: the picture would be wrong and the lane would be green, which is the one
    // failure mode this campaign spends its refusals on.

    // set_shader_buffers, ONE RECORD PER CLASS (CONTRACT-P5E.md §5.6, rulings 10/11). The whole
    // body is "validate the window, copy the tail into that class's array, take the mask, move
    // the serial" - the binding points are WORKING STATE and the objects they name are resolved
    // by the backend at its own sync point, from the handle each range carries.
    //
    // IT DOES NOT GO THROUGH ApplyUnitWindow, and the reason is worth a line rather than a
    // diff: that helper's refusal message names "the merged texture-unit space" and the three
    // unit sets share one capacity, while this row has THREE windows keyed by Class and two
    // refusals of its own that the contract names by string ("SetShaderBuffers.Class" /
    // ".Count"). A shared helper that had to grow a class parameter and a message parameter
    // would be the same code with more ways to pass the wrong one.
    //
    // AND THERE IS NO NoP4aConsumer BELT HERE, deliberately. P4a's belt exists because a client
    // clears a per-level dirty flag on ACCEPTANCE, so accepting on a backend that consumes
    // nothing loses uploads (ID-39). This family's liveness gate is the client's own
    // P5eFamilyIsLive (PipeFill.cpp), which asks R-8 for bit 13 in particular - a server that
    // does not publish bit 13 never receives one of these records at all - and storing a window
    // no backend reads costs nothing and loses nothing, because the emitter's only latch is the
    // set-hash suppressor and the record it latched did cross.
    void MGPipeApplySetShaderBuffers(const MGPShaderBuffers& hdr, const MGPBufferRange* tail) {
        if (hdr.Class >= kMGPipeShaderBufferClassCount) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " SetShaderBuffers.Class - class %u names no binding-point "
                                 "array; the three are Uniform=0, ShaderStorage=1, "
                                 "AtomicCounter=2",
                                 hdr.Class);
            return;
        }
        const Uint64 end = Uint64{hdr.Start} + Uint64{hdr.Count};
        if (end > kMGPipeMaxBufferBindingPoints || (hdr.Count != 0 && tail == nullptr)) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " SetShaderBuffers.Count {class=%u, start=%u, count=%u}: the "
                                 "window runs past the %u binding points the wire carries, or a "
                                 "non-empty set carries no ranges",
                                 hdr.Class, hdr.Start, hdr.Count,
                                 static_cast<unsigned>(kMGPipeMaxBufferBindingPoints));
            return;
        }
        // THE WINDOW IS THE BOUND AND ENTRIES OUTSIDE IT ARE NOT CLEARED, exactly as the three
        // unit sets and set_vertex_buffers work: the record is "the last set as received", and
        // a binding at or above Count means "nothing bound" to every reader (the frontend
        // array's default says the same thing, which is what makes the two pictures agree).
        auto& destination = g_applier.BoundShaderBuffers[hdr.Class];
        for (Uint32 i = 0; i < hdr.Count; ++i) destination[hdr.Start + i] = tail[i];
        g_applier.ShaderBufferStart[hdr.Class] = hdr.Start;
        g_applier.ShaderBufferCount[hdr.Class] = hdr.Count;
        for (Uint32 w = 0; w < kMGPipeShaderBufferWritableMaskWords; ++w) {
            g_applier.ShaderBufferWritableMask[hdr.Class][w] = hdr.WritableMask[w];
        }
        // ONE SERIAL FOR ALL THREE CLASSES, and it moves on EVERY applied record - a backend
        // memo that asks "have the binding points moved since I last bound them" must hear
        // about a uniform set exactly as it hears about a storage set, and splitting the serial
        // per class would make each of the four consumers pick which one to read.
        ++g_applier.ShaderBuffersSerial;
    }

    // P5e (pg) WRITES THE SECOND BODY. The three post-link mutable reflection fields -
    // glUniformBlockBinding's block-to-point map, glUniform1i's sampler unit per uniform
    // LOCATION, and glShaderStorageBlockBinding's name-keyed override set - all live INSIDE
    // the frontend's LinkArtifacts and all move after the link that produced it. An archive
    // snapshot alone therefore answers the wrong question: a twin built from it would bind the
    // uniform blocks the program was LINKED with rather than the ones it is BOUND with.
    void MGPipeApplySetProgramBindings(const MGPProgramBindings& hdr, const Int32* blockBindings,
                                       const MGPProgramSamplerUnit* samplerUnits,
                                       const MGPProgramStorageOverride* storageOverrides,
                                       const char* const* storageOverrideNames) {
        // THE BOUNDS ARE THE DECODER'S AND ARE RE-ASSERTED HERE, because the monolith adapter
        // reaches this entry point without passing the decoder at all: under monolith the only
        // validation a record ever gets is the one the applier does itself, which is the rule
        // every sibling in this file already keeps.
        const char* fault = nullptr;
        if (hdr.BlockBindingCount > kMGPipeMaxProgramBlockBindings) fault = "BlockBindingCount";
        else if (hdr.SamplerUnitCount > kMGPipeMaxProgramSamplerUnits) fault = "SamplerUnitCount";
        else if (hdr.StorageOverrideCount > kMGPipeMaxProgramStorageOverrides) fault = "StorageOverrideCount";
        else if (hdr.BlockBindingCount != 0 && blockBindings == nullptr) fault = "BlockBindings";
        else if (hdr.SamplerUnitCount != 0 && samplerUnits == nullptr) fault = "SamplerUnits";
        else if (hdr.StorageOverrideCount != 0 &&
                 (storageOverrides == nullptr || storageOverrideNames == nullptr)) {
            fault = "StorageOverrides";
        }
        if (fault != nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " set_program_bindings {slot=%u, gen=%u}: %s is out of range or "
                                 "its tail is absent (%u/%u/%u declared)",
                                 hdr.Cso.Slot, hdr.Cso.Gen, fault, hdr.BlockBindingCount,
                                 hdr.SamplerUnitCount, hdr.StorageOverrideCount);
            return;
        }
        // P4a's belt, for MGPipeApplyBindShaderState's reason: with no consumer this applier
        // holds no shader CSO record at all, so the resolution below would report the designed
        // state as RefusedObjectCalls.
        if (NoP4aConsumer()) return;

        MGPipeShaderCsoRecord* record = ResolveShaderCso("set_program_bindings", hdr.Cso);
        if (record == nullptr) return;

        // WHOLE-SET REPLACEMENT, NOT A MERGE, in all three tails. Each one describes the
        // program's complete state for its index space as the client observed it at emit; a
        // merge would leave a block binding the application has since reset to its declared
        // default sitting on top of the archive's value for ever, and there is no per-entry
        // "cleared" token on the wire that could say otherwise.
        record->BlockBindings.assign(blockBindings, blockBindings + hdr.BlockBindingCount);
        record->SamplerUnits.assign(samplerUnits, samplerUnits + hdr.SamplerUnitCount);
        record->StorageOverrides.clear();
        record->StorageOverrides.reserve(hdr.StorageOverrideCount);
        for (Uint32 i = 0; i < hdr.StorageOverrideCount; ++i) {
            MGPipeProgramStorageOverride entry;
            // COPIED, NOT POINTED AT (rule C). The name rides SEG_STAGE and the staged run
            // retires with the record that named it; the rebuild that reads this may be a
            // frame later, and by then those bytes are somebody else's.
            entry.Name = storageOverrideNames[i] != nullptr ? String(storageOverrideNames[i]) : String();
            entry.Binding = storageOverrides[i].Binding;
            record->StorageOverrides.push_back(Move(entry));
        }
        record->Signature = hdr.Signature;
        // ADVANCED, NEVER RETURNED TO A VALUE IT HAS HANDED OUT - VertexBuffersSerial's rule.
        // Both texture-unit memos and the program twin's clean condition key on it (ruling 6),
        // so a serial that went backwards would make a memo match state it has never seen.
        ++record->BindingsSerial;
    }

    // ---------------------------------------------------------------------------------
    // P5e: the barriered predicate and the applier's current-record flag (§2.1, §4.4)
    // ---------------------------------------------------------------------------------

    Bool MGPipeBarriered(MGPWireOp op, const void* payload, const MGPipeApplierState& st) {
        // 1. The static column. Every reply row, present, and the rows whose apply still pulls
        //    a BARRIER_PULLED field or probes the client allocator.
        if (MGPipeWaitClassFor(op) != kWaitNone) return true;
        // 2. Escalation (i): an open transform-feedback span makes every verb inside it
        //    barriered. XFB is not migrated by P5e - StartPendingTransformFeedback holds
        //    frontend SharedPtrs across two verbs, which rule C forbids an applier entry point
        //    outright - so the span keeps P5C's semantics and the cost on the steady Espryt
        //    path is one bool test per draw.
        if (MGPipeCallClassFor(op) == kCtxVerb && st.IsTransformFeedbackActive) return true;
        // 3. Escalation (ii): a draw whose enabled attributes include a client-memory array.
        //    The server uploads those bytes by dereferencing a raw client pointer, so the draw
        //    cannot run ahead. Under run-ahead the client refuses it before it is published
        //    (§5.1), so this clause is what the LOCKSTEP arm - where the draw is legal - uses.
        if (op == MGPWireOp::DrawVbo && payload != nullptr) {
            const auto& draw = *static_cast<const MGPDrawInfo*>(payload);
            if ((draw.Flags & static_cast<Uint8>(kDrawClientArrays)) != 0) return true;
            // ---- ESCALATION (iii) WAS HERE AND IS WITHDRAWN (P5e ra2, ID-133 then ID-136) ---
            //
            // ID-133 escalated a PLAIN multi-draw (`NumDraws > 1 && !kDrawIsIndirect`) so that
            // MultiDrawImpl::RunIndirect's read of the client's GL_DRAW_INDIRECT_BUFFER binding
            // became a legal barriered pull instead of an unbarriered Fatal. It worked, and it
            // cost a rendezvous on EVERY plain glMultiDraw* on the DEFAULT tier: there is one
            // draw opcode - all twenty entry points collapse onto draw_vbo - and the tier is
            // chosen on the server per batch (MultiDraw.cpp's ResolveTierForBatch), so no
            // predicate both roles can compute names the arm that actually needed it.
            //
            // THE CHECK ON AN ESCALATION IS "WHICH ARM PAYS FOR IT", NOT "WHICH LANE GOES
            // GREEN", and that is the rule this pair of rulings exists to record. The read was
            // a save/restore of a GL binding NAME around the tier's own scratch buffer, not a
            // data dependency, so the answer was to ask the side that did the binding:
            // BoundDrawIndirectBufferId now takes the handle arm its neighbour
            // ResolveBoundIndexBuffer already had, and the pull is GONE rather than legalised.
            //
            // Both halves landed in ONE commit on purpose: the pull retired without this clause
            // withdrawn is a cost with no reason, and this clause withdrawn without the pull
            // retired puts 18 lane entries back on the unbarriered arm. Neither is a state to
            // gate or to measure, so neither was ever a head.
        }
        return false;
    }

    // Server-private and deliberately NOT a member of MGPipeApplierState: it is not per-context
    // state the applier owns, it is "which record is the apply thread inside right now", and a
    // reader (the allocator guard) that had to find the right context's applier first would be
    // asking a harder question than the one it needs answered.
    //
    // TRUE IS THE DEFAULT AND THE LOCKSTEP ANSWER. On a server that does not publish
    // kCapRunAheadApply every record is barriered, the client is parked in its own wait for
    // each of them, and the two named exemption scopes of CONTRACT-P5C §3.1 keep working
    // exactly as they do today. Nothing writes this until package id sets it from
    // MGPipeBarriered in the sink's ApplyOne.
    namespace {
        thread_local Bool g_currentRecordBarriered = true;
        // P5e (gl), ID-128. FALSE IS THE DEFAULT AND IT IS THE STRICT ANSWER: "this record was
        // barriered by escalation" admits a pull that neither the wait class nor the retiring
        // phase admits, so a reader that runs before any writer must say no. The stamp above
        // defaults the other way because ITS safe answer is "the client is parked".
        thread_local Bool g_currentRecordBarrieredByEscalation = false;
    } // namespace

    Bool MGPipeApplierCurrentRecordIsBarriered() { return g_currentRecordBarriered; }

    void MGPipeApplierSetCurrentRecordBarriered(Bool barriered) {
        g_currentRecordBarriered = barriered;
    }

    Bool MGPipeApplierCurrentRecordIsBarrieredByEscalation() {
        return g_currentRecordBarrieredByEscalation;
    }

    void MGPipeApplierSetCurrentRecordBarrieredByEscalation(Bool escalated) {
        g_currentRecordBarrieredByEscalation = escalated;
    }

    // ================================================================================
    // w3: the shader CSO, the two program bindings and the default uniform block
    //
    // AND THE SERVER NEVER RE-LINKS. glslang lives entirely on the client and SPIRV-Cross
    // entirely on the server, a file-level cut: what crosses is the per-stage SPIR-V and the
    // reflection archive, never source, and there is no server-side compile pool to hand a
    // failure back from. Link failure needs no synchronous return either - it is one error log
    // plus a bind-program-0 empty draw today, GL_LINK_STATUS is never withdrawn, and the
    // synchronous query is answered by the client out of its own ProgramObject.
    //
    // WHAT THE SERVER STILL SPECIALISES IS NOT THIS RECORD'S BUSINESS. A backend program
    // depends on eight more inputs than the artefacts - the draw framebuffer's clamp masks and
    // fragColor broadcast count, the storage-block binding signature, the live image formats,
    // the patch parameters - and every one of them now arrives through ANOTHER record this
    // applier holds. create_shader_state publishes the artefacts; the verb specialises.
    // ================================================================================

    void MGPipeApplyCreateShaderState(const MGPProgramDesc& desc,
                                      const MG_State::GLState::LinkArtifacts* link,
                                      const MG_State::GLState::SpirvArtifacts* spirv,
                                      SharedPtr<const MG_State::GLState::ProgramArchive> archive) {
        MOBILEGL_ASSERT(desc.Cso.Slot >= kMGPipeFirstAllocatableSlot,
                        "create_shader_state named the reserved slot 0");
        if (desc.Cso.Slot < kMGPipeFirstAllocatableSlot) return;

        // THE ONE BLOB RULE, over seven blob refs at once. All seven are declared with Size 0 in
        // monolith - "this record does not declare its blob" - and the artefacts ride beside the
        // record through the two companion pointers, so a record that declares nothing AND
        // carries nothing describes a program with no reflection and no SPIR-V, which is not a
        // program. The declared lengths themselves cross-check nothing here: no other field of
        // MGPProgramDesc describes how long a module or an archive is, so the rule is inert on
        // this record by construction rather than by omission, and it becomes a real one when
        // the transport that fills those refs in lands with the splitter that owns them.
        const char* fault = nullptr;
        if (link == nullptr || spirv == nullptr) {
            fault = "the record declares no blobs and carries no artefacts";
        } else if (desc.GlobalUboSize > kMGPipeMaxGlobalConstantsBytes) {
            fault = "the default uniform block is larger than any program may declare";
        }
        if (fault != nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " create_shader_state {slot=%u, gen=%u}: %s (stages=0x%x, "
                                 "globalUboSize=%u, bound %u)",
                                 desc.Cso.Slot, desc.Cso.Gen, fault, desc.StageMask, desc.GlobalUboSize,
                                 kMGPipeMaxGlobalConstantsBytes);
            return;
        }

        MGPipeShaderCsoRecord* recordAt = ShaderCsoRecordAt(desc.Cso.Slot);
        if (recordAt == nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " create_shader_state {slot=%u, gen=%u}: the slot is outside the record "
                                 "table's bound (%u)",
                                 desc.Cso.Slot, desc.Cso.Gen, kMGPipeMaxShaderCsoSlots);
            return;
        }

#if MOBILEGL_PIPE_VERIFY
        // BEFORE STORING, so a lane that aborts here aborts on the record that was wrong rather
        // than on the next one that reads it.
        PinProgramArchiveRoundTrip(desc, *link, *spirv);
#endif

        // P4a's belt, after the blob rule, the slot bound and the verify round trip - all three
        // are checks on the RECORD and stay honest on every backend - and before it is stored.
        if (NoP4aConsumer()) return;

        MGPipeShaderCsoRecord& record = *recordAt;
        // A RE-ISSUE ON THE SAME HANDLE IS HOW A RELINK TRAVELS: the handle is minted per
        // frontend program and Gen moves only on slot reuse, so an existing record of the same
        // identity keeps its serial and counts up from it, and a record of a different identity
        // is a recycled slot and starts over.
        if (!record.Live || record.Gen != desc.Cso.Gen) {
            record = MGPipeShaderCsoRecord{};
            record.Gen = desc.Cso.Gen;
        } else {
            ++record.Serial;
            // A RELINK REPLACES THE DEFAULT UNIFORM BLOCK'S LAYOUT, so the image and the version
            // keyed to the old one go with it - keeping them would leave a block sized to a
            // layout that no longer exists, and the sentinel is exactly the value that says
            // "nothing has been uploaded for this program". The client re-emits against the
            // layout that now exists; the serial announces the clearing so a twin cannot match
            // what it uploaded before the relink.
            record.GlobalConstants.clear();
            record.GlobalConstantsVersion = ~Uint32{0};
            ++record.GlobalConstantsSerial;
        }
        record.Live = true;
        // The DESCRIPTOR is what is stored, and the artefacts are not: in monolith the backend
        // reads the frontend's own archive, and under split the descriptor is what the
        // deserialised archive is attached to. Either way the applier owns identity, extent and
        // order, and never content.
        record.Desc = desc;
        // P5e (pg), gap G-A: "under split the descriptor is what the deserialised archive is
        // attached to" is now literally true, and this is the attachment. ADOPTED
        // UNCONDITIONALLY, INCLUDING A NULL: a monolith create must CLEAR an archive a
        // previous split-arm create left behind, because the record outlives a transport
        // change (MGPipeApplierReset keeps the CSO table - a program lives in a share group)
        // and a stale archive is worse than none.
        //
        // AFTER Desc, so a reader that sees the new descriptor sees the artefacts it describes;
        // the apply thread is the only writer and the only reader, so ordering here is about
        // reading this function rather than about memory visibility.
        record.Archive = Move(archive);
        // THE RE-ISSUE CLEARS THE THREE BINDING TAILS for the reason it clears GlobalConstants
        // one branch up: they are indices INTO the archive that has just been replaced, so a
        // surviving tail would name block indices and uniform locations of a program that no
        // longer exists. Unconditional rather than inside the else-branch above, because a
        // recycled slot must not inherit its predecessor's bindings either - and a fresh record
        // has empty tails, so the clear costs a live program nothing. The client re-emits
        // set_program_bindings immediately after this create (ProgramEmit.h), which is the
        // ordering the applier relies on: emitting them BEFORE would put them where this line
        // wipes them.
        record.BlockBindings.clear();
        record.SamplerUnits.clear();
        record.StorageOverrides.clear();
        record.Signature = 0;
        ++record.BindingsSerial;
    }

    void MGPipeApplyBindShaderState(const MGPHandleOnly& handle) {
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::ShaderCso),
                        "bind_shader_state on kind %u", handle.Kind);
        // P4a's belt, and first for set_texture_params' reason: with no consumer this applier
        // holds no shader CSO record, so the resolution below would report the designed state
        // as RefusedObjectCalls. The null-handle unbind is behind it too - a binding this
        // applier never made is not one it may clear.
        if (NoP4aConsumer()) return;
        // The null handle is legal and means "nothing bound", which is a GL state and not an
        // error; a DEAD handle leaves the previous binding untouched and is counted, which is
        // bind_render_state's precedent for the same question.
        if (MGPipeHandleIsNull(handle.Handle)) {
            g_applier.BoundShaderCso = kMGPipeNullHandle;
            ++g_applier.ProgramBindingSerial;
            return;
        }
        const MGPipeShaderCsoRecord* record = ResolveShaderCso("bind_shader_state", handle.Handle);
        if (record == nullptr) return;
        g_applier.BoundShaderCso = handle.Handle;
        ++g_applier.ProgramBindingSerial;
    }

    void MGPipeApplyDeleteShaderState(const MGPHandleOnly& handle) {
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::ShaderCso),
                        "delete_shader_state on kind %u", handle.Kind);
        // A COMPOSITE'S SLOT HAS TWO INDEPENDENT RELEASE PATHS - the pipeline cache's eviction
        // and the composite program's own destructor - and both arrive here through one client
        // helper. The second is a refusal: the record is already gone, so it is counted and
        // dropped, which is the same defined no-op every other death notice gets and is why the
        // double free is a proven no-op rather than a race.
        MGPipeShaderCsoRecord* record = ResolveShaderCso("delete_shader_state", handle.Handle);
        if (record == nullptr) return;

        const Uint32 gen = record->Gen;
        *record = MGPipeShaderCsoRecord{};
        record->Gen = gen;

        // UNLIKE THE UNIT SETS, THE THREE PROGRAM BINDINGS ARE CLEARED. They are single handles
        // rather than a window of "the last set as received", and each of them is resolved
        // against the record that has just been dropped - leaving one behind would mean the
        // next verb resolving a binding to a record this applier no longer has, which is the
        // refusal counter firing for a state the applier itself created.
        Bool cleared = false;
        if (g_applier.BoundShaderCso == handle.Handle) {
            g_applier.BoundShaderCso = kMGPipeNullHandle;
            cleared = true;
        }
        if (g_applier.DrawProgram == handle.Handle) {
            g_applier.DrawProgram = kMGPipeNullHandle;
            cleared = true;
        }
        if (g_applier.DispatchProgram == handle.Handle) {
            g_applier.DispatchProgram = kMGPipeNullHandle;
            cleared = true;
        }
        if (cleared) ++g_applier.ProgramBindingSerial;
    }

    // TWO CALLS AND NOT ONE, because the frontend has two joins and two PipeInputs slots: a
    // draw resolves GetProgramForDraw and a dispatch resolves GetProgramForDispatch, and a
    // pipeline object flattened into a composite is the first of those and never the second.
    void MGPipeApplySetDrawProgram(const MGPHandleOnly& handle) {
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::ShaderCso),
                        "set_draw_program on kind %u", handle.Kind);
        if (NoP4aConsumer()) return; // P4a's belt, for MGPipeApplyBindShaderState's reason.
        if (MGPipeHandleIsNull(handle.Handle)) {
            g_applier.DrawProgram = kMGPipeNullHandle;
            ++g_applier.ProgramBindingSerial;
            return;
        }
        const MGPipeShaderCsoRecord* record = ResolveShaderCso("set_draw_program", handle.Handle);
        if (record == nullptr) return;
        g_applier.DrawProgram = handle.Handle;
        ++g_applier.ProgramBindingSerial;
    }

    void MGPipeApplySetDispatchProgram(const MGPHandleOnly& handle) {
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::ShaderCso),
                        "set_dispatch_program on kind %u", handle.Kind);
        if (NoP4aConsumer()) return; // P4a's belt, for MGPipeApplyBindShaderState's reason.
        if (MGPipeHandleIsNull(handle.Handle)) {
            g_applier.DispatchProgram = kMGPipeNullHandle;
            ++g_applier.ProgramBindingSerial;
            return;
        }
        const MGPipeShaderCsoRecord* record = ResolveShaderCso("set_dispatch_program", handle.Handle);
        if (record == nullptr) return;
        g_applier.DispatchProgram = handle.Handle;
        ++g_applier.ProgramBindingSerial;
    }

    void MGPipeApplySetGlobalConstants(const MGPGlobalConstants& record, const void* bytes) {
        // ON THE PROGRAM'S RECORD, not in the working state, and that is what makes it survive a
        // make-current: the block is (ShaderCso, Version) keyed and belongs to the program, not
        // to the context that last uploaded it.
        //
        // P4a's belt goes in front of the resolution for set_texture_params' reason, and it is
        // in front of the fault block too because THIS call's bounds come out of the resolved
        // program's own GlobalUboSize - there is nothing to validate against until the record
        // is in hand.
        if (NoP4aConsumer()) return;
        MGPipeShaderCsoRecord* stored = ResolveShaderCso("set_global_constants", record.ShaderCso);
        if (stored == nullptr) return;

        // THE LENGTH IS THE PROGRAM'S OWN GlobalUboSize, which arrived on the create and was
        // bounded there - so this call can only ever allocate what the create already declared,
        // and the blob rule has a real field to cross-check against for once.
        const Uint32 size = stored->Desc.GlobalUboSize;
        const char* fault = nullptr;
        if (record.Version == ~Uint32{0}) {
            fault = "the version is the backends' never-uploaded sentinel, which no record may carry";
        } else if (record.Blob.Size != 0 && record.Blob.Size != size) {
            fault = "the declared blob length is not the program's own default uniform block size";
        } else if (size > kMGPipeMaxGlobalConstantsBytes) {
            fault = "the program's default uniform block is larger than any program may declare";
        } else if (size != 0 && bytes == nullptr) {
            fault = "a non-empty block carries no bytes";
        }
        if (fault != nullptr) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption")
                                 " set_global_constants {slot=%u, gen=%u}: %s (version=%u, the program "
                                 "declares %u bytes, the blob declares %llu)",
                                 record.ShaderCso.Slot, record.ShaderCso.Gen, fault, record.Version, size,
                                 static_cast<unsigned long long>(record.Blob.Size));
            return;
        }

        if (size == 0) {
            stored->GlobalConstants.clear();
        } else {
            const Uint8* image = static_cast<const Uint8*>(bytes);
            stored->GlobalConstants.assign(image, image + size);
        }
        stored->GlobalConstantsVersion = record.Version;
        // Its OWN serial, beside the record's: a block upload is not a relink, and a twin that
        // memoises "which block image have I staged" must not be told the program moved.
        ++stored->GlobalConstantsSerial;
    }

    // THE MONOLITH BODY IS A NO-OP AND THAT IS THE WHOLE OF IT: the emulation this names still
    // runs, exactly as it does today, on the same code path. What the call site buys is that
    // the set of emulations a split server cannot serve is NAMED, GREPPABLE and PINNED, so P5
    // and P8 give it teeth by editing one function instead of rediscovering five call sites.
    //
    // It takes a literal and does nothing with it. Not a log line, not a counter: it sits on
    // paths a frame can reach many times, and ROADMAP.md forbids committing hot-path
    // instrumentation.
    // P5 GIVES IT TEETH, and this is the whole of it: ONE function edit arms FIVE call sites
    // (Managers.cpp:5334, DirectGLES.cpp:8051, :8702, :8997, :10623), exactly as the header
    // and P4a's comment above promised. Every one of them reaches back into the CLIENT's
    // address space - a texture shadow, a CPU mipmap fallback, a host-side copy - and a server
    // has no client address space to reach into, so degrading silently is the one outcome that
    // must not happen.
    //
    // THE ARM IS THE TRANSPORT, NOT THE BUILD. build-split runs MOBILEGL_TRANSPORT=monolith in
    // every unit and integration-gpu lane, and several of the five are on ordinary monolith
    // paths that those lanes exercise (glGenerateMipmap reaches two of them); arming on the
    // build would turn 1117 integration cases red for running code that is correct in the role
    // they run it in. None of the five is on P5's reduced path, so under a real transport this
    // costs nothing and catches a lot.
    void MGPipeUnmigratedEmulation(const char* name) {
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
            MGLOG_F("MGPipe: Fatal{UnmigratedEmulation, \"%s\"}", name != nullptr ? name : "<null>");
            std::abort();
        }
#endif
        (void)name;
    }
} // namespace MobileGL::MG_Pipe
