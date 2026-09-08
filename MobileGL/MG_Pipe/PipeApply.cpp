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
    } // namespace

    MGPipeApplierState& MGPipeApplier() { return g_applier; }

    void MGPipeSetResourceOps(const MGPipeResourceOps* ops) { g_resourceOps = ops; }
    const MGPipeResourceOps* MGPipeGetResourceOps() { return g_resourceOps; }

    void MGPipeApplierReset() {
        g_applier.RenderStateCsos.clear();
        g_applier.BoundRenderStateCso = kMGPipeNullHandle;
        g_applier.Residual = ResidualValueBlock{};
        g_applier.HasResidual = false;
        g_applier.ScatteredChunkBits = 0;
        g_applier.ResidualCapabilitiesCompared = 0;
        g_applier.ResidualDivergences = 0;
        g_applier.PatchCarrierComparisons = 0;
        g_applier.PatchCarrierDivergences = 0;
        // P3a. A fresh context is a fresh server: the records describe objects the new
        // context never made, and the three serials are per-context MGGens that must not
        // carry a previous context's count into a twin's "have I synced this?" compare.
        // The OP TABLE is deliberately NOT cleared here - it is installed and uninstalled by
        // the backend's own bring-up and teardown, not by a state reset.
        g_applier.Resources.clear();
        g_applier.VertexElementsCsos.clear();
        g_applier.BoundVertexElements = kMGPipeNullHandle;
        g_applier.VertexBuffers = {};
        g_applier.VertexBufferStart = 0;
        g_applier.VertexBufferCount = 0;
        g_applier.VertexFetchBaseInstance = 0;
        g_applier.VertexBuffersSerial = 0;
        g_applier.IndexBuffer = MGPIndexBuffer{};
        g_applier.IndexBufferSerial = 0;
        g_applier.MapPersistentRoundtrips = 0;
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
            // The three views are always populated; which one a shader input consumes is
            // ClassifyVertexAttribType's answer, not the carrier's, so all three cross.
            std::memcpy(slot.floatValue.data(), value.Data, sizeof(slot.floatValue));
            std::memcpy(slot.intValue.data(), value.Data, sizeof(slot.intValue));
            std::memcpy(slot.uintValue.data(), value.Data, sizeof(slot.uintValue));
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
    // P3a: the fourteen new entry points, AT THE CONTRACT COMMIT ONLY.
    //
    // Every body below is a deliberate no-op. The contract commit's job is the SHAPE - the
    // signatures the client, the backend and the gates compile against, the records they
    // write into and the op table they dispatch through - and the bodies land in the two
    // commits that follow on this branch, before anything emits a single one of these calls.
    //
    // NOTHING REACHES THEM HERE, and that is checked rather than hoped: the two subsystem
    // bits are not in MG_Impl/Pipe/PipeFill.cpp's kMGPipeWiredSubsystems, the dirty bits that
    // would gate the emission still map to no subsystem, and the emitters beside them are
    // stubs that emit nothing. A no-op that could be reached would be worse than an
    // unimplemented one - it would silently drop a mutation - which is exactly why the two
    // halves land in one commit apiece rather than one half at a time.
    // ================================================================================

    void MGPipeApplyResourceCreate(const MGPResourceDesc& desc) { (void)desc; }

    void MGPipeApplyResourceRespecify(const MGPResourceDesc& desc, const void* initialBytes) {
        (void)desc;
        (void)initialBytes;
    }

    void MGPipeApplyResourceSubData(const MGPSubData& record, const void* bytes) {
        (void)record;
        (void)bytes;
    }

    void MGPipeApplyBufferSubDataResident(const MGPSubData& record, const void* bytes) {
        (void)record;
        (void)bytes;
    }

    void MGPipeApplyResourceFlushRange(const MGPFlushRange& record, const void* bytes) {
        (void)record;
        (void)bytes;
    }

    void MGPipeApplyResourceReadback(const MGPReadback& record) { (void)record; }

    void MGPipeApplyResourceDestroy(const MGPHandleOnly& handle) { (void)handle; }

    // A DECLINE, which is a real answer rather than a failure: the persistent-map acquisition
    // is allowed to say no, the caller already has that branch, and null is what it reads.
    void* MGPipeApplyMapPersistent(const MGPHandleOnly& handle, Uint64 size, const void* seedBytes) {
        (void)handle;
        (void)size;
        (void)seedBytes;
        return nullptr;
    }

    void MGPipeApplyUnmapPersistent(const MGPHandleOnly& handle) { (void)handle; }

    void MGPipeApplyCreateVertexElements(const MGPVertexElements& desc, const void* blobBytes) {
        (void)desc;
        (void)blobBytes;
    }

    void MGPipeApplyBindVertexElements(const MGPHandleOnly& handle) { (void)handle; }

    void MGPipeApplyDeleteVertexElements(const MGPHandleOnly& handle) { (void)handle; }

    void MGPipeApplySetVertexBuffers(const MGPVertexBuffers& hdr, const MGPVertexBuffer* tail) {
        (void)hdr;
        (void)tail;
    }

    void MGPipeApplySetIndexBuffer(const MGPIndexBuffer& record) { (void)record; }

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
