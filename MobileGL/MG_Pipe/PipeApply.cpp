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

namespace MobileGL::MG_Pipe {

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
                MGP_DERIVE_CAPABILITY(ColorLogicOp)
                MGP_DERIVE_CAPABILITY(DebugOutput)
                MGP_DERIVE_CAPABILITY(DebugOutputSynchronous)
                MGP_DERIVE_CAPABILITY(DepthClamp)
                MGP_DERIVE_CAPABILITY(DepthTest)
                MGP_DERIVE_CAPABILITY(CullFace)
                MGP_DERIVE_CAPABILITY(Dither)
                MGP_DERIVE_CAPABILITY(FramebufferSrgb)
                MGP_DERIVE_CAPABILITY(LineSmooth)
                MGP_DERIVE_CAPABILITY(Multisample)
                MGP_DERIVE_CAPABILITY(PolygonOffsetFill)
                MGP_DERIVE_CAPABILITY(PolygonOffsetLine)
                MGP_DERIVE_CAPABILITY(PolygonOffsetPoint)
                MGP_DERIVE_CAPABILITY(PolygonSmooth)
                MGP_DERIVE_CAPABILITY(PrimitiveRestart)
                MGP_DERIVE_CAPABILITY(PrimitiveRestartFixedIndex)
                MGP_DERIVE_CAPABILITY(RasterizerDiscard)
                MGP_DERIVE_CAPABILITY(SampleAlphaToCoverage)
                MGP_DERIVE_CAPABILITY(SampleAlphaToOne)
                MGP_DERIVE_CAPABILITY(SampleCoverage)
                MGP_DERIVE_CAPABILITY(SampleMask)
                MGP_DERIVE_CAPABILITY(SampleShading)
                MGP_DERIVE_CAPABILITY(StencilTest)
                MGP_DERIVE_CAPABILITY(TextureCubeMapSeamless)
                MGP_DERIVE_CAPABILITY(ProgramPointSize)
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

        static void DeriveRenderStateFields(PipeInputs& inputs) {
            const RenderStateParameters& p = inputs.m_renderState;

            // Per draw buffer: GetBlendEquationIndexed, GetBlendFuncIndexed,
            // GetColorMaskIndexed and IsCapabilityEnabledIndexed(Blend).
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

            // Per viewport: GetViewportIndexed, GetDepthRangeIndexed and
            // IsCapabilityEnabledIndexed(ScissorTest).
            for (Uint i = 0; i < PipeInputs::kMaxViewports; ++i) {
                inputs.m_viewportIndexed[i] = p.Viewports[i];
                inputs.m_depthRange[i] = p.DepthRanges[i];
                inputs.m_capabilityIndexed.ScissorTest[i] = (p.ScissorTestEnabledMask & (1u << i)) != 0;
            }

            // GetViewport: viewport 0 ROUNDED - the other derivation that is not a field
            // copy. glGetIntegerv on floating-point state rounds to nearest (GL 4.6 core
            // 22.2), and truncating a 63.5-wide viewport would also hand the backends a
            // rectangle one pixel short of what was asked for. std::lround, exactly as
            // RenderState::GetViewport does it.
            const FloatVec4& viewport = p.Viewports[0];
            inputs.m_viewport =
                IntVec4(static_cast<Int>(std::lround(viewport.x())), static_cast<Int>(std::lround(viewport.y())),
                        static_cast<Int>(std::lround(viewport.z())), static_cast<Int>(std::lround(viewport.w())));

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

            for (SizeT i = 0; i < PipeInputs::kCapabilityCount; ++i) {
                inputs.m_capability[i] = DeriveCapability(p, static_cast<CapabilityInput>(i));
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
        MGPipeDeriveRenderStateFields(inputs);
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
        MGPipeDeriveRenderStateFields(inputs);
    }

    void MGPipeApplySetPixelPackState(const MGPPixelPackState& pack) {
        MGPipeApplyAccess::PackState(gPipeInputs) = pack.Pack;
    }

    void MGPipeApplySetPatchState(const MGPPatchState& patch) {
        PipeInputs& inputs = gPipeInputs;
        RenderStateParameters& working = MGPipeApplyAccess::RenderState(inputs);
        working.PatchVertices = patch.Vertices;
        working.PatchDefaultOuterLevel =
            FloatVec4(patch.Outer[0], patch.Outer[1], patch.Outer[2], patch.Outer[3]);
        working.PatchDefaultInnerLevel = FloatVec2(patch.Inner[0], patch.Inner[1]);
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
        const Bool* assembled = MGPipeApplyAccess::Capabilities(gPipeInputs);
        for (SizeT i = 0; i < kCapabilityCount; ++i) {
            const Bool carried = ((block.CapabilityBits >> i) & 1ull) != 0;
            if (carried == assembled[i]) continue;
#if MOBILEGL_PIPE_POISON || MOBILEGL_PIPE_VERIFY
            MGLOG_F("MGPipe: Fatal{PipeResidualDiverged, \"%s\"} carried=%d assembled=%d",
                    kCapabilityNames[i], static_cast<int>(carried), static_cast<int>(assembled[i]));
            std::abort();
#else
            MGLOG_E("MGPipe: residual value block diverged on %s (carried=%d assembled=%d)",
                    kCapabilityNames[i], static_cast<int>(carried), static_cast<int>(assembled[i]));
#endif
        }
    }

    void MGPipeDeriveRenderStateFields(PipeInputs& inputs) {
        // The derivation itself lives in MGPipeApplyAccess above, because that is the one
        // struct PipeInputs names as a friend - see D5 there for what it recomputes, which
        // getter each line was transcribed from, and why the verify comparator is its guard.
        MGPipeApplyAccess::DeriveRenderStateFields(inputs);
    }
} // namespace MobileGL::MG_Pipe
