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

        // ----------------------------------------------------------------------------
        // P3a: resolving a handle, growing a slot table, and the bounds gate.
        // ----------------------------------------------------------------------------

        // Grows a slot-indexed record table so `slot` is in it. Slot spaces are DENSE per
        // kind - the allocator is a free list plus a high-water mark - which is exactly why
        // the server's object table is an array a handle indexes rather than a map, and why
        // this grows only when a new high-water mark arrives.
        template <class Record>
        Record& RecordAt(Vector<Record>& records, Uint32 slot) {
            if (slot >= records.size()) records.resize(static_cast<SizeT>(slot) + 1);
            return records[slot];
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

        // WHY A DEAD HANDLE IS NOT A TRIP WIRE HERE, and the bounds faults below are.
        //
        // A resource call is applied at the GL call that causes it, while MGPipeApplierReset
        // runs at the first validate of a FRESH CONTEXT - so a buffer that outlives a context
        // switch (a shared store, a worker context) legitimately reaches this applier with its
        // record already dropped. Aborting a verify lane on a sequence that is legal would be
        // a wire that fires for a reason it does not exist for, so the refusal is a DEFINED
        // no-op: nothing is stored, nothing is dispatched, no serial moves, and the debug
        // assertion names it. That is the shape bind_render_state already uses for a dead CSO.
        //
        // The faults below are the other class entirely: a record that does not describe its
        // own bytes would have the BACKEND read or write outside a store, which is memory
        // corruption rather than a dropped call, so those get the tag ARCHITECTURE.md reserves
        // for exactly this - Fatal{ProtocolCorruption} - and the record's identity in the line.
        constexpr const char* kResourceRefusalNote =
            "the record is not this applier's; the call is dropped, not applied";

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
            return nullptr;
        }

#if MOBILEGL_PIPE_VERIFY
        // D-A4's pin. HasLiveHostWrites is ALWAYS false in this phase and is written by
        // nobody: it exists so the phase that pushes persistent-mapped host writes can set it
        // with no new record kind. A producer that landed under it would change what
        // IsBufferDrawClean answers with no other visible edit, so a verify build refuses to
        // let one arrive unannounced.
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

        // The one gate every content-carrying buffer write goes through. resource_subdata and
        // buffer_subdata_resident differ only in which backend hook takes the bytes and in the
        // fact that one of them is allowed to be absent, so a second copy of this arithmetic
        // would be a second place to get it wrong.
        void ApplyBufferWrite(const char* call, const MGPSubData& record, const void* bytes, Bool resident) {
            MGPipeResourceRecord* stored = FindResource(record.Res);
            MOBILEGL_ASSERT(stored != nullptr, "%s named {slot=%u, gen=%u}: %s", call, record.Res.Slot,
                            record.Res.Gen, kResourceRefusalNote);
            if (stored == nullptr) return;

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
                return;
            }
            PinNoLiveHostWrites(*stored, record.Res, call);

            // THE SERIAL MOVES BEFORE THE BACKEND IS TOLD, and that order is load-bearing:
            // the backend stamps its own synced serial from this record inside the hook, so a
            // bump afterwards would leave the twin stamped one mutation behind and the next
            // draw would re-upload what it had just landed.
            ++stored->Serial;

            if (g_resourceOps == nullptr) return;
            if (resident) {
                // kOptional, and the frontend already checks the same way for the table this
                // one replaces: a backend that does not implement the resident path leaves the
                // member null and the write is landed by its ordinary sub-data route instead.
                if (g_resourceOps->SubDataResident != nullptr) {
                    g_resourceOps->SubDataResident(record.Res, record, bytes);
                }
                return;
            }
            if (g_resourceOps->SubData != nullptr) g_resourceOps->SubData(record.Res, record, bytes);
        }
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

    void MGPipeApplyResourceCreate(const MGPResourceDesc& desc) {
        MOBILEGL_ASSERT(desc.Resource.Slot >= kMGPipeFirstAllocatableSlot,
                        "resource_create named the reserved slot 0");
        if (desc.Resource.Slot < kMGPipeFirstAllocatableSlot) return;

        // A CREATE STARTS THE RECORD OVER rather than editing it. The slot it names may be a
        // RECYCLED one whose record still describes the previous occupant, and inheriting one
        // field of that - a Width, a Serial, an Immutable - is precisely how a buffer at a
        // recycled address inherits its predecessor's contents. The generation is the client
        // allocator's answer to "is this still the same GL object", so it is taken from the
        // handle and nothing else survives.
        MGPipeResourceRecord& record = RecordAt(g_applier.Resources, desc.Resource.Slot);
        record = MGPipeResourceRecord{};
        record.Gen = desc.Resource.Gen;
        record.Live = true;
        record.Desc = desc;
        // Serial stays 0: a create is not a mutation. The descriptor a create carries defines
        // no storage - that is the first respecify's job, and a backend tolerates a resource
        // that has none - and a fresh backend twin starts its own synced serial at 0, so the
        // two agree from the first instant without either side publishing anything.
        if (g_resourceOps != nullptr && g_resourceOps->Create != nullptr) {
            g_resourceOps->Create(desc.Resource, desc);
        }
    }

    void MGPipeApplyResourceRespecify(const MGPResourceDesc& desc, const void* initialBytes) {
        MGPipeResourceRecord* record = FindResource(desc.Resource);
        MOBILEGL_ASSERT(record != nullptr, "resource_respecify named {slot=%u, gen=%u}: %s",
                        desc.Resource.Slot, desc.Resource.Gen, kResourceRefusalNote);
        if (record == nullptr) return;
        PinNoLiveHostWrites(*record, desc.Resource, "resource_respecify");

        // The descriptor is replaced WHOLE, because that is what a respecify is: the store's
        // extent, usage, storage flags, immutability and defined-content flag are all restated
        // by the call that redefines it, and the backend reads them from here instead of
        // asking a frontend object for them.
        record->Desc = desc;
        ++record->Serial;

        // resource_respecify is the catalogue's only kNeedsAck call, and the per-record half
        // of that flag is MGPipeResourceRespecifyNeedsAck(desc): glBufferStorage is a real
        // synchronous allocation and the only entry point allowed a synchronous ack, while
        // glBufferData travels through the same call and must not acknowledge one. In monolith
        // the acknowledgement IS the return of this function - the applier is one call away -
        // so the predicate has nothing to gate here and is deliberately not branched on: a
        // branch whose arms were identical would be dead code the transport would then have to
        // find and remove. PipeCatalogueTest.ResourceRespecifyAcksOnlyImmutableStorage is what
        // keeps the predicate honest until the doorbell reads it.
        if (g_resourceOps != nullptr && g_resourceOps->Respecify != nullptr) {
            g_resourceOps->Respecify(desc.Resource, desc, initialBytes);
        }
    }

    void MGPipeApplyResourceSubData(const MGPSubData& record, const void* bytes) {
        // The applier stores NOTHING per record - the contents are the backend's, and the
        // range is the backend's to land - so the whole of its job is the gate and the serial.
        ApplyBufferWrite("resource_subdata", record, bytes, /*resident=*/false);
    }

    void MGPipeApplyBufferSubDataResident(const MGPSubData& record, const void* bytes) {
        // `bytes` is the application's staging store and is valid for THE DURATION OF THE CALL
        // ONLY, which is why the backend hook copies rather than remembering the pointer.
        ApplyBufferWrite("buffer_subdata_resident", record, bytes, /*resident=*/true);
    }

    void MGPipeApplyResourceFlushRange(const MGPFlushRange& record, const void* bytes) {
        MGPipeResourceRecord* stored = FindResource(record.Res);
        MOBILEGL_ASSERT(stored != nullptr, "resource_flush_range named {slot=%u, gen=%u}: %s",
                        record.Res.Slot, record.Res.Gen, kResourceRefusalNote);
        if (stored == nullptr) return;

        const char* fault = BufferRangeFault(record.Offset, record.Size, stored->Desc.Width);
        if (fault == nullptr && record.Size != 0 && bytes == nullptr) {
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
        MGPipeResourceRecord* stored = FindResource(record.Res);
        MOBILEGL_ASSERT(stored != nullptr, "resource_readback named {slot=%u, gen=%u}: %s", record.Res.Slot,
                        record.Res.Gen, kResourceRefusalNote);
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
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::Buffer), "resource_destroy on kind %u",
                        handle.Kind);
        MGPipeResourceRecord* record = FindResource(handle.Handle);
        MOBILEGL_ASSERT(record != nullptr, "resource_destroy named {slot=%u, gen=%u}: %s", handle.Handle.Slot,
                        handle.Handle.Gen, kResourceRefusalNote);
        if (record == nullptr) return;

        // The record is dropped WHOLE and the generation is kept. The client allocator owns
        // the Gen bump, and it takes it on the next HANDOUT of the slot rather than on the
        // free, so a server-side bump here would put the two identities out of step and a
        // double free could skip a generation. Everything else goes: a stale read of a
        // destroyed slot must find nothing, not the extent of the buffer that used to be there.
        const Uint32 gen = record->Gen;
        *record = MGPipeResourceRecord{};
        record->Gen = gen;

        // The applier's own state is consistent before the backend hears the news, so a hook
        // that looked back at this applier could not see a resource that is already gone. The
        // CLIENT frees the slot after this returns, in that order, because the allocator
        // forgets the lifetime id on free and a notice resolved twice finds nothing the second
        // time.
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

        MGPipeResourceRecord* record = FindResource(handle.Handle);
        MOBILEGL_ASSERT(record != nullptr, "map_persistent named {slot=%u, gen=%u}: %s", handle.Handle.Slot,
                        handle.Handle.Gen, kResourceRefusalNote);
        if (record == nullptr) return nullptr;

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
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::Buffer), "unmap_persistent on kind %u",
                        handle.Kind);
        MGPipeResourceRecord* record = FindResource(handle.Handle);
        MOBILEGL_ASSERT(record != nullptr, "unmap_persistent named {slot=%u, gen=%u}: %s", handle.Handle.Slot,
                        handle.Handle.Gen, kResourceRefusalNote);
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

        // THE COUNTS ARE CHECKED AGAINST THE BLOB BEFORE A BYTE OF IT IS TOUCHED, and the
        // check is the record's own self-description: the blob is
        // MGPVertexAttribWire[AttributeCount] immediately followed by
        // MGPVertexBindingPointWire[BindingPointCount], so the two counts and the declared
        // blob length are three statements of one fact and any disagreement between them makes
        // the record unreadable. THIS is the reason the second view travels at all - a record
        // that declares a BindingPointCount it does not carry would otherwise be a shape this
        // gate had to police forever with nothing to police it against.
        //
        // Both counts are bounded by GL's attribute limit, which is also the size of the two
        // arrays they are unpacked into, so the bound and the destination cannot drift apart.
        const Uint64 attributeBytes = Uint64{desc.AttributeCount} * sizeof(MGPVertexAttribWire);
        const Uint64 bindingBytes = Uint64{desc.BindingPointCount} * sizeof(MGPVertexBindingPointWire);
        const Uint64 declared = attributeBytes + bindingBytes;
        const char* fault = nullptr;
        if (desc.AttributeCount > kMGPipeMaxVertexAttribs) {
            fault = "the declared attribute count is above GL's attribute limit";
        } else if (desc.BindingPointCount > kMGPipeMaxVertexAttribs) {
            fault = "the declared binding-point count is above GL's attribute limit";
        } else if (desc.Blob.Size != declared) {
            fault = "the declared counts do not describe the blob's own byte length";
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

        MGPipeVertexElementsRecord& record = RecordAt(g_applier.VertexElementsCsos, desc.Cso.Slot);
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
        const MGPipeVertexElementsRecord* record = FindVertexElements(handle.Handle);
        MOBILEGL_ASSERT(record != nullptr, "bind_vertex_elements named a dead CSO {slot=%u, gen=%u}",
                        handle.Handle.Slot, handle.Handle.Gen);
        if (record == nullptr) return;
        g_applier.BoundVertexElements = handle.Handle;
    }

    void MGPipeApplyDeleteVertexElements(const MGPHandleOnly& handle) {
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::VertexElementsCso),
                        "delete_vertex_elements on kind %u", handle.Kind);
        MGPipeVertexElementsRecord* record = FindVertexElements(handle.Handle);
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
} // namespace MobileGL::MG_Pipe
