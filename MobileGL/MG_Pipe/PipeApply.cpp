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

#include <cmath>
#include <cstdlib>
#include <cstring>

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

        MGPipeApplierState g_applier{};

        MGPipeRenderStateCsoRecord* FindCso(MGPipeHandle handle) {
            if (handle.Slot >= g_applier.RenderStateCsos.size()) return nullptr;
            MGPipeRenderStateCsoRecord& record = g_applier.RenderStateCsos[handle.Slot];
            if (!record.Live || record.Gen != handle.Gen) return nullptr;
            return &record;
        }

        constexpr Uint32 kAllPipelineChunks =
            static_cast<Uint32>((Uint64{1} << kMGPipePipelineChunkCount) - 1);
    } // namespace

    MGPipeApplierState& MGPipeApplier() { return g_applier; }

    void MGPipeApplierReset() {
        g_applier.RenderStateCsos.clear();
        g_applier.BoundRenderStateCso = kMGPipeNullHandle;
        g_applier.Residual = ResidualValueBlock{};
        g_applier.HasResidual = false;
    }

    void MGPipeApplyCreateRenderState(const MGPRenderStateDesc& desc, const void* chunkBytes) {
        MOBILEGL_ASSERT(desc.Cso.Slot >= kMGPipeFirstAllocatableSlot,
                        "create_render_state named the reserved slot 0");
        if (desc.Cso.Slot >= g_applier.RenderStateCsos.size()) {
            g_applier.RenderStateCsos.resize(desc.Cso.Slot + 1);
        }
        MGPipeRenderStateCsoRecord& record = g_applier.RenderStateCsos[desc.Cso.Slot];

        if (MGPipeHandleIsNull(desc.BaseCso)) {
            // A brand-new CSO carries its whole content; there is no earlier record to
            // inherit the unnamed chunks from.
            MOBILEGL_ASSERT((desc.ChunkMask & kAllPipelineChunks) == kAllPipelineChunks,
                            "create_render_state with no BaseCso must name every pipeline chunk "
                            "(mask=0x%x, expected 0x%x)",
                            desc.ChunkMask, kAllPipelineChunks);
            record.PipelineBytes = {};
        } else {
            const MGPipeRenderStateCsoRecord* base = FindCso(desc.BaseCso);
            MOBILEGL_ASSERT(base != nullptr,
                            "create_render_state named a dead BaseCso {slot=%u, gen=%u}",
                            desc.BaseCso.Slot, desc.BaseCso.Gen);
            if (base != nullptr) record.PipelineBytes = base->PipelineBytes;
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
        // whatever mask minted it - so the pipeline chunks are all "moved" here.
        MGPipeDeriveRenderStateFieldsForChunks(
            inputs, MGPipeGlobalChunkBitsOfPipelineMask(kAllPipelineChunks));
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
        MGPipeDeriveRenderStateFieldsForChunks(inputs, MGPipeGlobalChunkBitsOfDynamicMask(dyn.ChunkMask));
    }

    void MGPipeApplySetPixelPackState(const MGPPixelPackState& pack) {
        MGPipeApplyAccess::PackState(gPipeInputs) = pack.Pack;
    }

    void MGPipeApplySetPatchState(const MGPPatchState& patch) {
        PipeInputs& inputs = gPipeInputs;
        RenderStateParameters& working = MGPipeApplyAccess::RenderState(inputs);
        const FloatVec4 outer(patch.Outer[0], patch.Outer[1], patch.Outer[2], patch.Outer[3]);
        const FloatVec2 inner(patch.Inner[0], patch.Inner[1]);

#if MOBILEGL_PIPE_POISON || MOBILEGL_PIPE_VERIFY
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
        // Only once a CSO has delivered chunk P0: before the first bind_render_state of a
        // context the working block still holds its defaults, and a set_patch_state that
        // legitimately precedes the first bind has nothing to agree with yet. That is the
        // ordering contract this trip wire places on the tracker - within a validate, the
        // bind comes first.
        if (!MGPipeHandleIsNull(g_applier.BoundRenderStateCso)) {
            const Bool agrees = working.PatchVertices == patch.Vertices &&
                                std::memcmp(&working.PatchDefaultOuterLevel, &outer, sizeof(outer)) == 0 &&
                                std::memcmp(&working.PatchDefaultInnerLevel, &inner, sizeof(inner)) == 0;
            if (!agrees) {
                MGLOG_F("MGPipe: Fatal{PipePatchCarriersDiffer} set_patch_state says vertices=%u "
                        "outer=(%g,%g,%g,%g) inner=(%g,%g); chunk P0 delivered vertices=%u "
                        "outer=(%g,%g,%g,%g) inner=(%g,%g)",
                        patch.Vertices, static_cast<double>(outer.x()), static_cast<double>(outer.y()),
                        static_cast<double>(outer.z()), static_cast<double>(outer.w()),
                        static_cast<double>(inner.x()), static_cast<double>(inner.y()), working.PatchVertices,
                        static_cast<double>(working.PatchDefaultOuterLevel.x()),
                        static_cast<double>(working.PatchDefaultOuterLevel.y()),
                        static_cast<double>(working.PatchDefaultOuterLevel.z()),
                        static_cast<double>(working.PatchDefaultOuterLevel.w()),
                        static_cast<double>(working.PatchDefaultInnerLevel.x()),
                        static_cast<double>(working.PatchDefaultInnerLevel.y()));
                std::abort();
            }
        }
#endif

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
        Uint32 consumed = 0;
        for (Uint32 location = 0; location < PipeInputs::kMaxVertexAttribs; ++location) {
            if ((hdr.Mask & (1u << location)) == 0) continue;
            MOBILEGL_ASSERT(consumed < hdr.Count,
                            "set_vertex_attrib_defaults: Mask names more attributes than Count");
            if (consumed >= hdr.Count) break;
            const MGPAttribValue& value = tail[consumed++];
            MOBILEGL_ASSERT(value.Location == location,
                            "set_vertex_attrib_defaults: tail out of ascending location order "
                            "(%u where %u was expected)",
                            value.Location, location);
            PipeInputs::CurrentVertexAttributeValue& slot = slots[location];
            // The three views are always populated; which one a shader input consumes is
            // ClassifyVertexAttribType's answer, not the carrier's, so all three cross.
            std::memcpy(slot.floatValue.data(), value.Data, sizeof(slot.floatValue));
            std::memcpy(slot.intValue.data(), value.Data, sizeof(slot.intValue));
            std::memcpy(slot.uintValue.data(), value.Data, sizeof(slot.uintValue));
        }
        MOBILEGL_ASSERT(consumed == hdr.Count,
                        "set_vertex_attrib_defaults: Count %u does not match the %u attributes Mask "
                        "names",
                        hdr.Count, consumed);
    }

    void MGPipeApplySetResidualValueState(const ResidualValueBlock& block) {
        g_applier.Residual = block;
        g_applier.HasResidual = true;

        // THE TRIP WIRE (ARCHITECTURE.md 9.4, P2 brief D9). CapabilityBits is redundant with
        // the assembled working block by design: every one of the 35 capabilities is
        // answerable from RenderStateParameters now that P2 closed the three storage holes.
        // So the day a later call takes a capability over and forgets to carry it, the two
        // answers part and this says so on the next draw - which is what a migration carrier
        // is for.
        //
        // The comparison reads the WORKING BLOCK, not PipeInputs::m_capability. Those two
        // agree after any scatter - the derivation is what puts the block's answer there -
        // but m_capability is written ONLY by the derivation, so comparing against it would
        // make this trip wire depend on a bind_render_state or a set_dynamic_state having
        // already been applied to this context. The residual block is emitted ONCE PER
        // CONTEXT (D9) and may legitimately be the first call of all, at which point
        // m_capability is still all-false while the block's own defaults have Dither and
        // Multisample true - the trip wire would fire on a context that is perfectly correct.
        // Asking DeriveCapability the same question the derivation asks removes that ordering
        // contract without weakening the check by one bit.
        const RenderStateParameters& working = MGPipeApplyAccess::RenderState(gPipeInputs);
        for (SizeT i = 0; i < kCapabilityCount; ++i) {
            const Bool carried = ((block.CapabilityBits >> i) & 1ull) != 0;
            const Bool assembledBit = MGPipeApplyAccess::DeriveCapability(working, static_cast<CapabilityInput>(i));
            if (carried == assembledBit) continue;
#if MOBILEGL_PIPE_POISON || MOBILEGL_PIPE_VERIFY
            MGLOG_F("MGPipe: Fatal{PipeResidualDiverged, \"%s\"} carried=%d assembled=%d",
                    kCapabilityNames[i], static_cast<int>(carried), static_cast<int>(assembledBit));
            std::abort();
#else
            MGLOG_E("MGPipe: residual value block diverged on %s (carried=%d assembled=%d)",
                    kCapabilityNames[i], static_cast<int>(carried), static_cast<int>(assembledBit));
#endif
        }
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
} // namespace MobileGL::MG_Pipe
