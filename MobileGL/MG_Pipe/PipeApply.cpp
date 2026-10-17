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

// MGPipeUnmigratedEmulation's split arm reads MG_Config::Transport, which is what tells an
// emulation site whether it is running inside a server or in the monolith it was written for.
#include <Config.h>

#if MOBILEGL_PIPE_VERIFY
// THE ONE PLACE THE PROGRAM ARCHIVE'S CODEC IS CALLED, and it is compiled into the VERIFY
// build only. In monolith the archive does not travel - MGPProgramDesc's seven blob refs are
// declared with Size 0 and the two structs ride beside the record through the entry point's
// companion pointers - so a push build pays nothing for it and the header keeps its forward
// declarations. The verify build serialises, deserialises and compares before storing, which
// makes the codec LIVE CODE WITH A GATE rather than dead code with a unit test.
#include <MG_State/GLState/ProgramState/ProgramArtifactsCodec.h>
#endif

// P7 wave 0: the seam the Magma wire funnels in MG_Backend die through. Declared in MG_Pipe and
// DEFINED below, so MG_Backend names no MG_Remote symbol of its own to reach Session::Fail. P13 W5:
// in every build, for the record arm's fail / latch half.
#include <MG_Pipe/PipeSessionFail.h>
#include <MG_Pipe/PipeClientSeam.h>
#if MOBILEGL_BUILD_DISAGGREGATED
// R-6's tier gate. One spelling, asked at the one place the decline is decided. Outside the
// MOBILEGL_PIPE_VERIFY block above on purpose: the tier is a property of the BUILD, not of the
// comparator, and a split build without the comparator still declines every acquisition.
#include <MG_State/GLState/BufferState/PersistentMapTracker.h>
#include <MG_Remote/Client/AdoptTierChoice.h>
// P5c (ct), CONTRACT-P5C.md §6 layer 2: MGPipeApplierReset's role guard asks
// ServerLoop::OnApplyThread() - the one predicate that tells the GL thread from the thread
// that owns g_applier under an active transport - and ClientSession::Active(), which is what
// tells a live wire from a configured-but-wireless one (the bring-up window and the
// server-role-only fixture: there the direct call is the only reset that exists).
#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/Server/ServerLoop.h>
#include <MG_Remote/Server/ServerSession.h>
#include <MG_Remote/CapsCodec.h>
#endif
#include <MG_Backend/Record/StagedTextureStore.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
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

    // ----------------------------------------------------------------------------
    // P13 W5: THE RECORD ARM'S FAIL / LATCH SEAM (PipeSessionFail.h). Constant-initialized
    // pointers, so a registration made during another translation unit's static init (FatalFunnel.cpp)
    // is never overwritten by this one's.
    namespace {
        constinit MGPipeRecordFailHook g_recordFailHook = nullptr;
        constinit MGPipeRecordLatchHook g_recordLatchHook = nullptr;
        constinit MGPipeRecordLatchedHook g_recordLatchedHook = nullptr;

        void FormatRecordLine(char (&line)[512], const char* fmt, va_list args) {
            const int written = std::vsnprintf(line, sizeof(line), fmt, args);
            if (written < 0) {
                std::snprintf(line, sizeof(line), "MGPipe: unformattable Fatal diagnostic (format=%s)", fmt);
            }
        }

        [[noreturn]] void RecordDeath(MGFatalFamily family, const char* line) {
            if (g_recordFailHook != nullptr) g_recordFailHook(family, line);
            // NO HOOK (no MG_Remote in this library): the line, its stderr echo - the logger's
            // console sink is off and a gtest death matcher reads the child's stderr - and the abort.
            MGLOG_F("%s", line);
            std::fputs(line, stderr);
            std::fputc('\n', stderr);
            std::fflush(stderr);
            std::abort();
        }
    } // namespace

    void MGPipeInstallRecordFailHooks(MGPipeRecordFailHook fail, MGPipeRecordLatchHook latch,
                                      MGPipeRecordLatchedHook latched) {
        g_recordFailHook = fail;
        g_recordLatchHook = latch;
        g_recordLatchedHook = latched;
    }

    void MGPipeRecordFail(MGFatalFamily family, const char* fmt, ...) {
        char line[512];
        va_list args;
        va_start(args, fmt);
        FormatRecordLine(line, fmt, args);
        va_end(args);
        RecordDeath(family, line);
    }

    bool MGPipeRecordLatch(MGFatalFamily family, const char* fmt, ...) {
        char line[512];
        va_list args;
        va_start(args, fmt);
        FormatRecordLine(line, fmt, args);
        va_end(args);
        if (g_recordLatchHook != nullptr) return g_recordLatchHook(family, line);
        RecordDeath(family, line);
    }

    bool MGPipeRecordLatched() {
        return g_recordLatchedHook != nullptr && g_recordLatchedHook();
    }

    void MGPipeProtocolFatal(const char* what, const char* detail) {
        MGPipeRecordFail(MGFatalFamily::ProtocolCorruption, "MGPipe: Fatal{ProtocolCorruption, \"%s\"} %s", what,
                         detail != nullptr ? detail : "");
    }

    void MGPipeProtocolFatalAt(const char* what, Uint64 got, Uint64 expected) {
        MGPipeRecordFail(MGFatalFamily::ProtocolCorruption,
                         "MGPipe: Fatal{ProtocolCorruption, \"%s\"} got=%llu expected=%llu", what,
                         static_cast<unsigned long long>(got), static_cast<unsigned long long>(expected));
    }

    MGPipeRecordFailHook MGPipeRecordFailHookInstalled() {
        return g_recordFailHook;
    }
    MGPipeRecordLatchHook MGPipeRecordLatchHookInstalled() {
        return g_recordLatchHook;
    }
    MGPipeRecordLatchedHook MGPipeRecordLatchedHookInstalled() {
        return g_recordLatchedHook;
    }

    // P13 W5: the client session's device-loss answer (PipeClientSeam.h). Constant-initialized for
    // the same static-init reason as the hooks above.
    namespace {
        constinit MGPipeClientDeviceLostProbe g_clientDeviceLostProbe = nullptr;
    } // namespace
    void MGPipeSetClientDeviceLostProbe(MGPipeClientDeviceLostProbe probe) {
        g_clientDeviceLostProbe = probe;
    }
    Bool MGPipeClientDeviceLost() {
        return g_clientDeviceLostProbe != nullptr && g_clientDeviceLostProbe();
    }
    namespace {
        constinit MGPipeClientLossConfirmer g_clientLossConfirmer = nullptr;
    } // namespace
    void MGPipeSetClientLossConfirmer(MGPipeClientLossConfirmer confirmer) {
        g_clientLossConfirmer = confirmer;
    }
    Bool MGPipeClientConfirmLossAfterDecline(Uint32 waitMs) {
        return g_clientLossConfirmer != nullptr && g_clientLossConfirmer(waitMs);
    }

    // ----------------------------------------------------------------------------
    // THE BACKEND-SIDE SESSION-FAIL SEAM (P7 wave 0). PipeSessionFail.h holds the argument;
    // this is the whole implementation.
    //
    // It lives in this file for the reason MGP_TRIP_WIRE_REPORT does, forty lines up: this is
    // where MG_Pipe already states "the verdict of every trip wire, in one place", and a seam
    // whose default is an abort belongs next to the other one rather than in a file of its own
    // that a reader has to be told about. Split-only, so a pull build gains nothing (G1) and a
    // push-monolith build - where the three Magma wire funnels are not compiled either - gains
    // nothing to call.
    namespace {
        MGPipeSessionFailHook g_sessionFailHook = nullptr;
    } // namespace

    void MGPipeInstallSessionFailHook(MGPipeSessionFailHook hook) {
        g_sessionFailHook = hook;
    }

    MGPipeSessionFailHook MGPipeSessionFailHookInstalled() {
        return g_sessionFailHook;
    }

    void MGPipeSessionFail(MGPipeFatalFamily family, const char* fmt, ...) {
        // The same 512-byte buffer and the same fallback SessionFail and WireLogFatal use, so a
        // death that travels this seam reads identically to one that did not.
        char line[512];
        va_list args;
        va_start(args, fmt);
        const int written = std::vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        if (written < 0) {
            std::snprintf(line, sizeof(line), "MGPipe: unformattable Fatal diagnostic (format=%s)", fmt);
        }
        if (g_sessionFailHook != nullptr) g_sessionFailHook(family, line);
        // NO HOOK: exactly what the three funnels did before this seam existed - the line, then
        // the abort. Reached in a server image whose role init has not run (a unit case that
        // drives the applier headless) and in the client process, where there is no session to
        // fail. The abort carries no `Fatal{` of its own because `line` already does;
        // fatal_census.py's FUNNEL_SITES names this function for that reason.
        MGLOG_F("%s", line);
        std::abort();
    }

    namespace {
        MGPipeSessionLatchHook g_sessionLatchHook = nullptr;
        MGPipeSessionLatchArmedHook g_sessionLatchArmedHook = nullptr;
    } // namespace

    void MGPipeInstallSessionLatchHook(MGPipeSessionLatchHook hook, MGPipeSessionLatchArmedHook armed) {
        g_sessionLatchHook = hook;
        g_sessionLatchArmedHook = armed;
    }

    MGPipeSessionLatchHook MGPipeSessionLatchHookInstalled() {
        return g_sessionLatchHook;
    }

    bool MGPipeSessionLatchArmed() {
        return g_sessionLatchArmedHook != nullptr && g_sessionLatchArmedHook();
    }

    bool MGPipeSessionLatch(MGPipeFatalFamily family, const char* fmt, ...) {
        char line[512];
        va_list args;
        va_start(args, fmt);
        const int written = std::vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        if (written < 0) {
            std::snprintf(line, sizeof(line), "MGPipe: unformattable Fatal diagnostic (format=%s)", fmt);
        }
        if (g_sessionLatchHook != nullptr) return g_sessionLatchHook(family, line);
        // NO HOOK: no session to latch, so the death the fail seam would have given.
        MGPipeSessionFail(family, "%s", line);
    }

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
        constexpr Uint32 kChunksCapabilityWalk = MGP_CHUNKS_OF(BlendStates) | MGP_CHUNKS_OF(ScissorTestEnabledMask) |
                                                 MGP_CHUNKS_OF(ClipDistanceEnabledMask)
                                                     MGP_PLAIN_CAPABILITY_LIST(MGP_CAPABILITY_CHUNKS);
#undef MGP_CAPABILITY_CHUNKS

        // The patch trio's chunk, which is what arms the set_patch_state trip wire: the
        // question that wire asks is whether the chunk-P0 bytes in the working block are the
        // APPLIER'S, and only a scatter puts them there.
        constexpr Uint32 kChunksPatchTrio = MGP_CHUNKS_OF(PatchVertices) | MGP_CHUNKS_OF(PatchDefaultOuterLevel) |
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
        static void SetPatchState(PipeInputs& inputs, Uint vertices, const FloatVec4& outer, const FloatVec2& inner) {
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
                inputs.m_touchedBindingPointCount[i] = static_cast<SizeT>(values.TouchedBufferBindingPointCount[i]);
            }
            inputs.m_transformFeedbackActive = values.IsTransformFeedbackActive != 0;
            inputs.m_transformFeedbackPaused = values.IsTransformFeedbackPaused != 0;
            inputs.m_transformFeedbackGeneration = values.TransformFeedbackGeneration;
            inputs.m_boundTransformFeedbackLifetimeId = values.BoundTransformFeedbackLifetimeId;
            inputs.m_transformFeedbackCapturedVertices = values.TransformFeedbackCapturedVertices;
        }

        // ----------------------------------------------------------------------------
        // The PipeInputs fields that are pure functions of RenderStateParameters. The applier
        // derives them from the assembled working block instead of copying them from GLContext:
        // each line transcribes the RenderState getter of the same name (RenderState.cpp), which
        // GLContext's accessors forward to. MOBILEGL_PIPE_VERIFY's compare-at-read and
        // RenderStateSpans.DerivationMatchesTheFrontendGetters catch a transcription error.
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

        // ============================================================================
        // P14 S5: THE APPLIER REGISTRY.
        // ============================================================================
        //
        // A REFERENCE TO A NEVER-DESTROYED BLOCK, for MGPipeSlots()' reason
        // (MG_Impl/Pipe/SlotAllocator.cpp): resource_destroy and delete_vertex_elements are
        // raised from ~BufferObject / ~VertexArrayObject, and those objects are released by
        // exit handlers that run after this translation unit's own globals are gone.
        //
        // THE DEFAULT IS THE OLD SINGLETON, unrenamed in what it means: the applier of a thread
        // that belongs to nobody - the client process, the monolith, the inproc one-process
        // shape, every unit case. Its object records are the group it owns alone.
        MGPipeObjectRecords& g_defaultObjects = *new MGPipeObjectRecords{};
        MGPipeApplierState& g_defaultApplier = *new MGPipeApplierState(g_defaultObjects);

        // Installed once by the session registry (MG_Remote/Server/SessionRuntime.cpp), never
        // replaced. Null in every process that serves no session, which is what keeps the
        // monolith's `MGPipeApplier()` a load and a branch.
        MGPipeApplierKeyResolver g_applierKeyResolver = nullptr;

        // One served session's appliers. Groups own object records; Contexts own working state
        // and point at their group. Both are held through UniquePtr so a table's ADDRESS is
        // stable across a rehash - MGPipeApplierState binds its group by reference - and the
        // maps are std::unordered_map rather than the house UnorderedMap (a flat hash map) for
        // that same reason at the map level: a reference into it may not be invalidated by an
        // insert somewhere else in the same call.
        struct ApplierSession {
            std::unordered_map<Uint64, UniquePtr<MGPipeObjectRecords>> Groups;
            std::unordered_map<Uint64, UniquePtr<MGPipeApplierState>> Contexts;
            std::unordered_map<Uint64, Uint64> ContextGroup;
            std::unordered_map<Uint64, Uint32> GroupContexts;
        };
        std::unordered_map<Uint64, ApplierSession> g_applierSessions;
        std::mutex g_applierSessionsMutex;
        // Bumped by EVERY registry mutation. A thread's cached applier pointer is only trusted
        // while it was bound at this generation, which is what makes "a context was destroyed on
        // another thread" unable to leave a dangling cache behind.
        std::atomic<Uint64> g_applierRegistryGeneration{1};

        // A context that started its own share group is keyed by its own token, in a namespace
        // of its own: the frontend mints share-group tokens and context tokens from separate
        // counters, so without this a group 7 and a context 7 would be one group.
        constexpr Uint64 kMGPipeOwnShareGroupBit = Uint64{1} << 63;

        // `shareGroupToken` 0 means "this context started its own group" - the sentinel S1's
        // ContextRuntime documents and the one a caller that frames a context by hand (a test,
        // the in-process server helpers) leaves behind.
        Uint64 ApplierShareGroupKey(Uint64 contextToken, Uint64 shareGroupToken) {
            return shareGroupToken != 0 ? shareGroupToken : (contextToken | kMGPipeOwnShareGroupBit);
        }

        // The three tables of the applier this thread's records are being applied through.
        // `generation` is the registry's, read here so a caller can bind a pointer under one
        // generation and publish it under the same one.
        MGPipeApplierState* FindApplierForKey(const MGPipeApplierKey& key) {
            std::lock_guard<std::mutex> guard(g_applierSessionsMutex);
            ApplierSession& session = g_applierSessions[key.SessionKey];
            const auto found = session.Contexts.find(key.ContextToken);
            if (found != session.Contexts.end() && found->second) return found->second.get();
            // A context the session never registered - a token that predates the first
            // CreateContext, or the 0 of a session that has not bound one yet. It gets the same
            // shape as any other: a working state in a group of its own.
            const Uint64 groupKey = ApplierShareGroupKey(key.ContextToken, 0);
            auto group = session.Groups.find(groupKey);
            if (group == session.Groups.end()) {
                group = session.Groups.emplace(groupKey, MakeUnique<MGPipeObjectRecords>()).first;
            }
            auto applier = MakeUnique<MGPipeApplierState>(*group->second);
            MGPipeApplierState* const raw = applier.get();
            session.Contexts.emplace(key.ContextToken, std::move(applier));
            session.ContextGroup.emplace(key.ContextToken, groupKey);
            ++session.GroupContexts[groupKey];
            return raw;
        }

        // Every share group's object records in the session the calling thread's records are
        // applied for, or nothing on a thread that serves no session (the process-wide applier is
        // then the only one). The pointers stay valid after the lock is dropped: a group's records
        // are held by UniquePtr, and only this session's own apply thread adds or drops its groups.
        Vector<MGPipeObjectRecords*> BoundSessionGroups() {
            Vector<MGPipeObjectRecords*> groups;
            const MGPipeApplierKeyResolver resolve = g_applierKeyResolver;
            MGPipeApplierKey key;
            if (resolve == nullptr || !resolve(&key)) return groups;
            std::lock_guard<std::mutex> guard(g_applierSessionsMutex);
            const auto session = g_applierSessions.find(key.SessionKey);
            if (session == g_applierSessions.end()) return groups;
            for (auto& [groupKey, records] : session->second.Groups) {
                (void)groupKey;
                if (records) groups.push_back(records.get());
            }
            return groups;
        }

        // One block, so a hit is one TLS address computation rather than four (P15).
        struct ApplierCache {
            MGPipeApplierState* Applier = nullptr;
            Uint64 Session = 0;
            Uint64 Context = 0;
            Uint64 Generation = 0;
        };
        constinit thread_local ApplierCache t_applierCache;
    } // namespace

    Uint64 MGPipeNextApplierSerialBase() {
        // Constant-initialized, so the default applier's static construction may already use it.
        static std::atomic<Uint64> appliers{0};
        return (appliers.fetch_add(1, std::memory_order_relaxed) + 1) << 40;
    }

    namespace {
        std::atomic<MGPipeCompileEnvChangedHook> g_compileEnvChangedHook{nullptr};
    } // namespace

    void MGPipeSetCompileEnvChangedHook(MGPipeCompileEnvChangedHook hook) {
        g_compileEnvChangedHook.store(hook, std::memory_order_release);
    }

    void MGPipeNotifyCompileEnvChanged() {
        if (const auto hook = g_compileEnvChangedHook.load(std::memory_order_acquire)) hook();
    }

    namespace Detail {
        void* g_applierResolverInstalled = nullptr;
        MGPipeApplierState* g_processApplier = &g_defaultApplier;
    } // namespace Detail

    MGPipeApplierState& MGPipeApplierForSession() {
        const MGPipeApplierKeyResolver resolve = g_applierKeyResolver;
        if (resolve == nullptr) return g_defaultApplier;
        MGPipeApplierKey key;
        if (!resolve(&key)) return g_defaultApplier;
        // THE FAST PATH, and the reason this is a cache rather than a lookup: this function is
        // called from the backend per state read. A hit is one TLS block, one atomic load and
        // the compares; a miss is the registry, which only moves when a context is created,
        // destroyed or made current.
        const Uint64 generation = g_applierRegistryGeneration.load(std::memory_order_acquire);
        ApplierCache& cache = t_applierCache;
        if (cache.Applier != nullptr && cache.Generation == generation && cache.Session == key.SessionKey &&
            cache.Context == key.ContextToken) {
            return *cache.Applier;
        }
        MGPipeApplierState* const applier = FindApplierForKey(key);
        cache = {applier, key.SessionKey, key.ContextToken, generation};
        return *applier;
    }

    void MGPipeSetApplierKeyResolver(MGPipeApplierKeyResolver resolver) {
        g_applierKeyResolver = resolver;
        Detail::g_applierResolverInstalled = reinterpret_cast<void*>(resolver);
    }

    void MGPipeApplierRegisterContext(Uint64 sessionKey, Uint64 contextToken, Uint64 shareGroupToken) {
        std::lock_guard<std::mutex> guard(g_applierSessionsMutex);
        ApplierSession& session = g_applierSessions[sessionKey];
        const Uint64 groupKey = ApplierShareGroupKey(contextToken, shareGroupToken);
        auto group = session.Groups.find(groupKey);
        if (group == session.Groups.end()) {
            group = session.Groups.emplace(groupKey, MakeUnique<MGPipeObjectRecords>()).first;
        }
        const auto existing = session.Contexts.find(contextToken);
        if (existing != session.Contexts.end() && existing->second) {
            // Idempotent: the same token registered twice keeps the state it already has. The
            // session refuses a duplicate CreateContext before reaching here, so this arm is a
            // caller that re-registers deliberately.
            g_applierRegistryGeneration.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        session.Contexts.emplace(contextToken, MakeUnique<MGPipeApplierState>(*group->second));
        session.ContextGroup[contextToken] = groupKey;
        ++session.GroupContexts[groupKey];
        g_applierRegistryGeneration.fetch_add(1, std::memory_order_acq_rel);
    }

    void MGPipeApplierUnregisterContext(Uint64 sessionKey, Uint64 contextToken) {
        std::lock_guard<std::mutex> guard(g_applierSessionsMutex);
        const auto sessionIt = g_applierSessions.find(sessionKey);
        if (sessionIt == g_applierSessions.end()) return;
        ApplierSession& session = sessionIt->second;
        const auto context = session.Contexts.find(contextToken);
        if (context == session.Contexts.end()) return;
        const auto groupIt = session.ContextGroup.find(contextToken);
        const Uint64 groupKey = groupIt != session.ContextGroup.end() ? groupIt->second : 0;
        // The OBJECT RECORDS GO WITH THE LAST CONTEXT OF THE GROUP, which is the per-group
        // spelling of MGPipeApplierReleaseObjectRecords' scope. A surviving context of the same
        // group keeps every record - that is what sharing means.
        const auto refs = session.GroupContexts.find(groupKey);
        const Bool lastOfGroup = refs != session.GroupContexts.end() && refs->second <= 1;
        session.Contexts.erase(context);
        if (groupIt != session.ContextGroup.end()) session.ContextGroup.erase(groupIt);
        if (lastOfGroup) {
            if (refs != session.GroupContexts.end()) session.GroupContexts.erase(refs);
            const auto group = session.Groups.find(groupKey);
            if (group != session.Groups.end()) session.Groups.erase(group);
        } else if (refs != session.GroupContexts.end()) {
            --refs->second;
        }
        if (session.Contexts.empty()) g_applierSessions.erase(sessionIt);
        g_applierRegistryGeneration.fetch_add(1, std::memory_order_acq_rel);
    }

    void MGPipeApplierReleaseSession(Uint64 sessionKey) {
        std::lock_guard<std::mutex> guard(g_applierSessionsMutex);
        if (g_applierSessions.erase(sessionKey) == 0) return;
        g_applierRegistryGeneration.fetch_add(1, std::memory_order_acq_rel);
    }

    Uint64 MGPipeApplierShareGroupCountForTesting(Uint64 sessionKey) {
        std::lock_guard<std::mutex> guard(g_applierSessionsMutex);
        const auto sessionIt = g_applierSessions.find(sessionKey);
        return sessionIt == g_applierSessions.end() ? 0 : static_cast<Uint64>(sessionIt->second.Groups.size());
    }

    Uint64 MGPipeApplierShareGroupKeyFor(Uint64 sessionKey, Uint64 contextToken) {
        std::lock_guard<std::mutex> guard(g_applierSessionsMutex);
        const auto sessionIt = g_applierSessions.find(sessionKey);
        if (sessionIt != g_applierSessions.end()) {
            const auto groupIt = sessionIt->second.ContextGroup.find(contextToken);
            if (groupIt != sessionIt->second.ContextGroup.end()) return groupIt->second;
        }
        // Not registered - the same fallback FindApplierForKey takes, spelled by the same one
        // function so the two cannot drift.
        return ApplierShareGroupKey(contextToken, 0);
    }

    namespace {

        // The installed handle-shaped resource table. Null until a backend registers one,
        // which is what makes the client half landable on its own: with nothing here every
        // frontend dispatch falls through to the op table this one replaces, and the tree
        // behaves exactly as it did.
        const MGPipeResourceOps* g_resourceOps = nullptr;

        MGPipeRenderStateCsoRecord* FindCso(MGPipeHandle handle) {
            if (handle.Slot >= MGPipeApplier().RenderStateCsos.size()) return nullptr;
            MGPipeRenderStateCsoRecord& record = MGPipeApplier().RenderStateCsos[handle.Slot];
            if (!record.Live || record.Gen != handle.Gen) return nullptr;
            return &record;
        }

        constexpr Uint32 kAllPipelineChunks = static_cast<Uint32>((Uint64{1} << kMGPipePipelineChunkCount) - 1);

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
            if (res.Slot >= MGPipeApplier().Resources.size()) return nullptr;
            MGPipeResourceRecord& record = MGPipeApplier().Resources[res.Slot];
            if (!record.Live || record.Gen != res.Gen) return nullptr;
            return &record;
        }

        MGPipeVertexElementsRecord* FindVertexElements(MGPipeHandle cso) {
            if (cso.Slot >= MGPipeApplier().VertexElementsCsos.size()) return nullptr;
            MGPipeVertexElementsRecord& record = MGPipeApplier().VertexElementsCsos[cso.Slot];
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
            if (target == kMGPipeResourceTargetBuffer) return &MGPipeApplier().Resources;
            if (target == static_cast<Uint8>(MGPipeResourceTarget::Renderbuffer)) {
                return &MGPipeApplier().RenderbufferResources;
            }
            if (target < static_cast<Uint8>(MGPipeResourceTarget::Count)) {
                // Every remaining enumerator is a texture target, and they share one table
                // because they share one kind: MGPipeKind::Texture. Tex1D..TexCubeArray,
                // Tex2DMS/MSArray, TexBuffer and TexRect are all one slot space.
                return &MGPipeApplier().TextureResources;
            }
            return nullptr;
        }

        // resource_destroy carries no descriptor, so its discriminator is the handle's KIND.
        Vector<MGPipeResourceRecord>* ResourceTableForKind(Uint32 kind) {
            switch (static_cast<MGPipeKind>(kind)) {
            case MGPipeKind::Buffer:
                return &MGPipeApplier().Resources;
            case MGPipeKind::Texture:
                return &MGPipeApplier().TextureResources;
            case MGPipeKind::Renderbuffer:
                return &MGPipeApplier().RenderbufferResources;
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

        // True when a non-buffer respecify carries only metadata: every storage-defining field
        // (the set listed beside MGPipeResourceRespecifyNeedsAck in MGPipeTypes.h) equals the
        // stored descriptor. The client resends the descriptor when a sticky BindMask /
        // ImageBindableHint bit moves (for an immutable texture the only way the bit reaches the
        // server), and that call must not drop pending uploads. Compared field by field because
        // padding is not reliably written. The set is wide on purpose: too wide can only drop
        // uploads on a call the client never sends, too narrow keeps boxes for a level that no
        // longer exists. A buffer is never metadata-only: same-size glBufferData orphans its store.
        Bool RespecifyRedefinesNoStorage(const MGPResourceDesc& stored, const MGPResourceDesc& next) {
            if (next.Target == kMGPipeResourceTargetBuffer) return false;
            if (stored.Target != next.Target) return false;
            if (stored.StorageKind != next.StorageKind) return false;
            if (stored.InternalFormat != next.InternalFormat) return false;
            if (stored.Width != next.Width || stored.Height != next.Height || stored.Depth != next.Depth) {
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

        // A call naming a record this applier lacks is a defined no-op, not a trip wire, because
        // one legal sequence produces it: context teardown runs MGPipeApplierReleaseObjectRecords,
        // then the destructors of objects the context still owns send resource_destroy /
        // delete_vertex_elements for records already dropped. The refusal stores, dispatches and
        // moves nothing; MOBILEGL_ASSERT compiles out at INFO, so the resolvers below count it
        // (MGPipeApplierState::Refused*Calls) and a dropped write stays visible in every build. A
        // record that would make the backend touch memory outside a store is the other class:
        // Fatal{ProtocolCorruption}, with the record's identity in the line.
        constexpr const char* kResourceRefusalNote =
            "the record is not this applier's; the call is dropped, not applied";

        // The two resolvers every entry point below uses. One place resolves, asserts and
        // counts, so a call that forgets one of the three cannot exist.
        MGPipeResourceRecord* ResolveResource(const char* call, MGPipeHandle res) {
            MGPipeResourceRecord* record = FindResource(res);
            MOBILEGL_ASSERT(record != nullptr, "%s named {slot=%u, gen=%u}: %s", call, res.Slot, res.Gen,
                            kResourceRefusalNote);
            if (record == nullptr) ++MGPipeApplier().RefusedResourceCalls;
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
            if (record == nullptr) ++MGPipeApplier().RefusedResourceCalls;
            return record;
        }

        // P4a's object families. ONE counter for the five, because they share one legal refusal
        // sequence - the teardown order beside kResourceRefusalNote - and because what an
        // operator reading a log needs to know is that an object call was dropped; the line
        // itself names which call and which handle.
        template <class Record>
        Record* ResolveObject(Vector<Record>& table, const char* call, MGPipeHandle handle) {
            Record* record = FindIn(table, handle);
            MOBILEGL_ASSERT(record != nullptr, "%s named {slot=%u, gen=%u}: %s", call, handle.Slot, handle.Gen,
                            kResourceRefusalNote);
            if (record == nullptr) ++MGPipeApplier().RefusedObjectCalls;
            return record;
        }

        MGPipeVertexElementsRecord* ResolveVertexElements(const char* call, MGPipeHandle cso) {
            MGPipeVertexElementsRecord* record = FindVertexElements(cso);
            MOBILEGL_ASSERT(record != nullptr, "%s named {slot=%u, gen=%u}: %s", call, cso.Slot, cso.Gen,
                            kResourceRefusalNote);
            if (record == nullptr) ++MGPipeApplier().RefusedVertexInputCalls;
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
        Bool BoxIsEmpty(const MGPBox& box) {
            return box.W == 0 || box.H == 0 || box.D == 0;
        }

        // A box stays inside the range one record can encode: the origin is a signed Int32 and
        // the extent an unsigned Uint32, so an origin plus an extent that leaves the positive
        // Int32 range is a record no encoder writes and is refused before any arithmetic below
        // has to survive it. It is the same bound, and the same reason, as the buffer half's
        // "above the bound one record can encode".
        Bool BoxIsEncodable(Int32 origin, Uint32 extent) {
            return origin >= 0 && static_cast<Int64>(origin) + static_cast<Int64>(extent) <= 0x7FFFFFFF;
        }

        Bool BoxIsEncodable(const MGPBox& box) {
            return BoxIsEncodable(box.X, box.W) && BoxIsEncodable(box.Y, box.H) && BoxIsEncodable(box.Z, box.D);
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
            return MGPBox{
                x, y, z, static_cast<Uint32>(xEnd - x), static_cast<Uint32>(yEnd - y), static_cast<Uint32>(zEnd - z)};
        }

        // The server-side pending-upload set (ARCHITECTURE.md §6.1). The client clears a level's
        // dirty flags when this applier accepts its record, but a backend's upload loop can bail
        // (Espryt returns early for an incomplete texture and skips a multisample target), so the
        // accepted shape accumulates here until the backend actually uploads it. Boxes union and
        // rect lists concatenate; once either side is box-only or the list would exceed
        // kMGPipeMaxPendingUploadRegions, the entry becomes box-only, which still covers every
        // texel. Returns false when the record names more (target, level) keys than any texture
        // can have; the caller reports that as a corrupt record.
        Bool AccumulatePendingUpload(MGPipeResourceRecord& stored, const MGPSubData& record,
                                     const MGPSubRegion* regions) {
            for (MGPipeResourceRecord::PendingUpload& entry : stored.PendingUploads) {
                if (entry.UploadTarget != record.Target || entry.Level != record.Level) continue;
                entry.UnionBox = UnionOfBoxes(entry.UnionBox, record.UnionBox);
                if (record.RegionCount == 0 || entry.Regions.empty()) {
                    // A whole-level contribution (the emitter's box-only spelling since P11 M2)
                    // owns every texel of the level, so the entry is that box, exactly. The one
                    // other box-only record is the emitter's oversize fallback (a footprint past
                    // kMGPipeMaxPendingUploadRegions goes out as the storage's cover, which it
                    // withholds as 0 at 3/4 of the box), a partial box spanning gaps - already
                    // inexact, and logged once where it is emitted (TextureEmit.h).
                    entry.Regions.clear();
                    return true;
                }
                if (static_cast<Uint64>(entry.Regions.size()) + record.RegionCount > kMGPipeMaxPendingUploadRegions) {
                    // The one lossy answer left: the box covers every texel the list named, and
                    // also the gaps between them (see kMGPipeMaxPendingUploadRegions).
                    MGLOG_W_ONCE("MGPipe: a texture level's pending upload outgrew %u regions and becomes its union "
                                 "box, which spans the gaps between the writes",
                                 kMGPipeMaxPendingUploadRegions);
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

        void PinNoLiveHostWrites(const MGPipeResourceRecord&, MGPipeHandle, const char*) {}

#if MOBILEGL_PIPE_VERIFY
        // Verify builds: a per-level respecify scope declared in the descriptor must match the
        // `level` pointer. The wire escape (MG_Remote/Client/WireTables.cpp) writes the carrier
        // from the pointer and the codec rebuilds the pointer from it. The applier drops pending
        // uploads by the pointer, so a declared scope with a null pointer would take the
        // whole-resource arm and eat every other level's texels, and a different pair would drop
        // the wrong key; both show up only as missing pixels. One-directional: in-process emission
        // (MG_Impl/Pipe/TextureEmit.h) leaves the carrier clear and states the scope in the
        // pointer alone, which is legal.
        void PinRespecifyScopeCoversItsDeclaration(const MGPResourceDesc& desc, const MGPRespecifiedLevel* level,
                                                   MGPipeHandle res, const char* call) {
            if (MGPipeRespecifyIsWholeResource(desc)) return;
            const Uint16 declaredTarget = MGPipeRespecifiedUploadTargetOf(desc);
            const Uint16 declaredLevel = MGPipeRespecifiedLevelOf(desc);
            if (level != nullptr && level->UploadTarget == declaredTarget && level->Level == declaredLevel) {
                return;
            }
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "PipeRespecifyScope") " %s {slot=%u, gen=%u}: the descriptor declares a per-level respecify "
                                          "scope (target=%u, level=%u) and this call covers %s(target=%u, "
                                          "level=%u), so a per-level producer is not covering the range it "
                                          "declares",
                call, res.Slot, res.Gen, static_cast<unsigned>(declaredTarget), static_cast<unsigned>(declaredLevel),
                level == nullptr ? "NOTHING " : "", level == nullptr ? 0u : static_cast<unsigned>(level->UploadTarget),
                level == nullptr ? 0u : static_cast<unsigned>(level->Level));
        }
#else
        void PinRespecifyScopeCoversItsDeclaration(const MGPResourceDesc&, const MGPRespecifiedLevel*, MGPipeHandle,
                                                   const char*) {}
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
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "PipeLiveHostWritesTarget") " %s {slot=%u, gen=%u}: a record whose resource target is not a "
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
                MGP_TRIP_WIRE_REPORT(
                    "MGPipe: " MGP_TRIP_WIRE_TAG(
                        "ProtocolCorruption") " %s {slot=%u, gen=%u, glName=%u}: %s (offset=%llu, size=%llu, "
                                              "storage=%u bytes)",
                    call, record.Res.Slot, record.Res.Gen, stored->Desc.GlNameForDiag, fault,
                    static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size), stored->Desc.Width);
                return false;
            }
            PinNoLiveHostWrites(*stored, record.Res, call);
            // P5 (b1): THE PRODUCER. The record's own statement about the resource, applied
            // before the serial moves so that a probe re-entered from inside the backend hook
            // below already sees it. It is an assignment and not an OR: the bit is a STATE,
            // and a content record emitted while nothing maps the buffer is exactly how the
            // state goes back to false - which is why the falling edge pushes one block
            // (BufferObject::NotePersistentMapStateChanged) rather than relying on the next
            // ordinary write to arrive.
            stored->HasLiveHostWrites = record.HasLiveHostWrites != 0;

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

        // Texture records can have a server consumer without the buffer ops table (Magma).
        // Their staged bytes still belong to the server before SEG_STAGE retires. Keep the
        // existing backend hooks authoritative when present, and use this fallback otherwise.
        void AdoptTextureWithoutBackendHook(const MGPipeResourceRecord& stored, const MGPSubData& upload,
                                            const void* bytes, const MGPSubRegion* regions) {
            if (!bytes || upload.Blob.Size == 0) return;
            auto& store = MG_Record::ServerStagedTexture();
            const auto& desc = stored.Desc;
            const Uint64 key = MG_Record::StagedTextureStore::KeyForHandle(upload.Res);
            const Uint16 target = MGPipeSubDataUploadTargetOf(upload.Target);
            const IntVec3 extent = MG_Record::StagedTextureUploadExtent(desc, upload);
            // RegionCount == 0 IS the whole-level spelling - "the run is the level shadow" - so it
            // is adopted with the spelling that replaces the level; every other record names a RUN
            // of the level by its own box (fix A2: a level too large to stage whole crosses as
            // slabs, one record each, and the first of them is at the level's first byte).
            // A client record's run: on monolith's record arm it is the frontend level shadow, which
            // an aliasing store keeps by address (P13 W4).
            if (upload.RegionCount == 0) {
                store.Adopt(key, target, upload.Level, extent, bytes, static_cast<SizeT>(upload.Blob.Size),
                            /*frontendShadow=*/true);
                return;
            }
            store.AdoptRun(key, target, upload.Level, extent, MG_Record::StagedTextureRunImageOffset(upload, regions),
                           bytes, static_cast<SizeT>(upload.Blob.Size), /*frontendShadow=*/true);
        }

        void DefineTextureWithoutBackendHook(const MGPResourceDesc& desc, const MGPRespecifiedLevel* level) {
            auto& store = MG_Record::ServerStagedTexture();
            const Uint64 key = MG_Record::StagedTextureStore::KeyForHandle(desc.Resource);
            const auto define = [&](Uint16 target, Uint16 mip, const IntVec3& extent) {
                store.NoteLevelDefined(key, target, mip, extent, desc.Target, desc.InternalFormat);
            };
            if (level) {
                define(MGPipeSubDataUploadTargetOf(level->UploadTarget), level->Level,
                       IntVec3{static_cast<Int>(level->Width), static_cast<Int>(level->Height),
                               static_cast<Int>(level->Depth)});
                return;
            }
            store.ResetLevels(key);
            if (!desc.Immutable || !desc.Levels) return;
            const auto defineChain = [&](TextureUploadTarget target) {
                for (Uint32 mip = 0; mip < desc.Levels; ++mip)
                    define(static_cast<Uint16>(target), static_cast<Uint16>(mip),
                           MG_Record::StagedTextureMipExtent(desc.Target, desc.Width, desc.Height, desc.Depth, mip));
            };
            switch (static_cast<MGPipeResourceTarget>(desc.Target)) {
            case MGPipeResourceTarget::Tex1D:
                defineChain(TextureUploadTarget::Texture1D);
                break;
            case MGPipeResourceTarget::Tex2D:
                defineChain(TextureUploadTarget::Texture2D);
                break;
            case MGPipeResourceTarget::Tex3D:
                defineChain(TextureUploadTarget::Texture3D);
                break;
            case MGPipeResourceTarget::Tex1DArray:
                defineChain(TextureUploadTarget::Texture1DArray);
                break;
            case MGPipeResourceTarget::Tex2DArray:
                defineChain(TextureUploadTarget::Texture2DArray);
                break;
            case MGPipeResourceTarget::TexCube:
                for (Uint32 face = static_cast<Uint32>(TextureUploadTarget::CubeMapPositiveX);
                     face <= static_cast<Uint32>(TextureUploadTarget::CubeMapNegativeZ); ++face)
                    defineChain(static_cast<TextureUploadTarget>(face));
                break;
            case MGPipeResourceTarget::TexCubeArray:
                defineChain(TextureUploadTarget::CubeMapArray);
                break;
            case MGPipeResourceTarget::Tex2DMS:
                defineChain(TextureUploadTarget::Texture2DMultisample);
                break;
            case MGPipeResourceTarget::Tex2DMSArray:
                defineChain(TextureUploadTarget::Texture2DMultisampleArray);
                break;
            case MGPipeResourceTarget::TexRect:
                defineChain(TextureUploadTarget::TextureRectangle);
                break;
            case MGPipeResourceTarget::TexBuffer:
                defineChain(TextureUploadTarget::TextureBuffer);
                break;
            default:
                break;
            }
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
                ResolveResourceIn(MGPipeApplier().TextureResources, "resource_subdata", record.Res);
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
                MGP_TRIP_WIRE_REPORT(
                    "MGPipe: " MGP_TRIP_WIRE_TAG(
                        "ProtocolCorruption") " resource_subdata {slot=%u, gen=%u, glName=%u}: %s (target=%u, "
                                              "level=%u, box=%d,%d,%d %ux%ux%u, regions=%u)",
                    record.Res.Slot, record.Res.Gen, stored->Desc.GlNameForDiag, fault, record.Target, record.Level,
                    record.UnionBox.X, record.UnionBox.Y, record.UnionBox.Z, record.UnionBox.W, record.UnionBox.H,
                    record.UnionBox.D, record.RegionCount);
                return false;
            }
            if (!AccumulatePendingUpload(*stored, record, regions)) {
                MGP_TRIP_WIRE_REPORT(
                    "MGPipe: " MGP_TRIP_WIRE_TAG(
                        "ProtocolCorruption") " resource_subdata {slot=%u, gen=%u, glName=%u}: the resource names "
                                              "more distinct (upload target, level) pairs than any texture can "
                                              "have (%u)",
                    record.Res.Slot, record.Res.Gen, stored->Desc.GlNameForDiag, kMGPipeMaxPendingUploads);
                return false;
            }
            // The serial moves for the buffer half's reason: the twin stamps its own synced
            // serial from inside the sync that reads this record, so a bump afterwards would
            // leave it one mutation behind and the next draw would re-upload what it had just
            // landed. The ACCEPTANCE is what the client reads to clear its own dirty flag - the
            // record was accumulated, so the texels are the server's now, and that is the true
            // this returns.
            ++stored->Serial;
            // P5c (tx): THE STAGED BYTES ARE ADOPTED HERE, at the last instant `bytes` is known
            // alive (rule C: SEG_STAGE retires when this record does, and PendingUpload
            // deliberately holds no byte pointer). The hook copies the run into the server's
            // staged-texture store keyed by this record's own handle; it is a no-op in
            // monolith, so the monolith shape keeps P5's pointer-dropping expression exactly.
            // The REGIONS travel with it because a record may carry a RUN of its level rather
            // than the whole of it (fix A2): they are what places that run in the level image,
            // and the applier's pending set - which already has them - is not the store's.
            if (g_resourceOps != nullptr && g_resourceOps->TextureSubData != nullptr) {
                g_resourceOps->TextureSubData(record.Res, record, bytes, regions);
            } else {
                AdoptTextureWithoutBackendHook(*stored, record, bytes, regions);
            }
            return true;
        }

        // ----------------------------------------------------------------------------
        // The body shared by the three var-tail unit sets, which differ only in the entry type.
        //
        // The window [start, start + count) is bounded against the destination (in 64 bits), and
        // entries outside it are not cleared: the record is the last set as received, and a set
        // naming four units says nothing about the other 188. An entry's own Unit field is not
        // checked; the destination is start + i and the field is only the client's label, as
        // with MGPVertexBuffer::BindingIndex.
        template <class Entry, class ArrayT>
        Bool ApplyUnitWindow(const char* call, Uint32 start, Uint32 count, Uint64 contentHash, const Entry* tail,
                             ArrayT& destination, Uint32& destinationStart, Uint32& destinationCount) {
            const Uint64 capacity = destination.size();
            const Uint64 end = Uint64{start} + Uint64{count};
            const char* fault = nullptr;
            if (end > capacity) {
                fault = "the window runs past the merged texture-unit space";
            } else if (count != 0 && tail == nullptr) {
                fault = "a non-empty set carries no entries";
            }
            if (fault != nullptr) {
                MGP_TRIP_WIRE_REPORT(
                    "MGPipe: " MGP_TRIP_WIRE_TAG(
                        "ProtocolCorruption") " %s {start=%u, count=%u, hash=%llu}: %s (the applier holds %llu "
                                              "entries)",
                    call, start, count, static_cast<unsigned long long>(contentHash), fault,
                    static_cast<unsigned long long>(capacity));
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
                return RecordAt(MGPipeApplier().CompositeShaderCsos, slot - kMGPipeShaderCsoCompositeSlotBase,
                                kMGPipeShaderCsoSlotLimit - kMGPipeShaderCsoCompositeSlotBase);
            }
            // The ordinary table is bounded by the BAND'S BASE and not by the slot limit: an
            // ordinary program can never be handed a band slot (the allocator refuses it), so a
            // slot at or above the base that is not a composite is out of range by definition.
            return RecordAt(MGPipeApplier().ShaderCsos, slot, kMGPipeShaderCsoCompositeSlotBase);
        }

        MGPipeShaderCsoRecord* FindShaderCso(MGPipeHandle cso) {
            if (MGPipeIsCompositeShaderSlot(cso.Slot)) {
                return FindIn(MGPipeApplier().CompositeShaderCsos,
                              MGPipeHandle{cso.Slot - kMGPipeShaderCsoCompositeSlotBase, cso.Gen});
            }
            return FindIn(MGPipeApplier().ShaderCsos, cso);
        }

        MGPipeShaderCsoRecord* ResolveShaderCso(const char* call, MGPipeHandle cso) {
            MGPipeShaderCsoRecord* record = FindShaderCso(cso);
            MOBILEGL_ASSERT(record != nullptr, "%s named {slot=%u, gen=%u}: %s", call, cso.Slot, cso.Gen,
                            kResourceRefusalNote);
            if (record == nullptr) ++MGPipeApplier().RefusedObjectCalls;
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
        void PinProgramArchiveRoundTrip(const MGPProgramDesc& desc, const MG_State::GLState::LinkArtifacts& link,
                                        const MG_State::GLState::SpirvArtifacts& spirv) {
            Vector<Uint8> encoded;
            MG_State::GLState::EncodeProgramArtifacts(link, spirv, encoded);
            MG_State::GLState::LinkArtifacts decodedLink;
            MG_State::GLState::SpirvArtifacts decodedSpirv;
            Vector<Uint8> reencoded;
            const char* fault = nullptr;
            if (encoded.empty()) {
                fault = "the encoder produced no bytes for a program that has artefacts";
            } else if (!MG_State::GLState::DecodeProgramArtifacts(encoded.data(), encoded.size(), decodedLink,
                                                                  decodedSpirv)) {
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
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "PipeVerifyDiffer") " program-archive create_shader_state {slot=%u, gen=%u}: %s (%llu bytes "
                                        "encoded, %llu re-encoded)",
                desc.Cso.Slot, desc.Cso.Gen, fault, static_cast<unsigned long long>(encoded.size()),
                static_cast<unsigned long long>(reencoded.size()));
        }
#endif
    } // namespace

    // MGPipeApplier() is defined with the registry at the top of this file: it is the one
    // function that has to know whose applier the calling thread is in.

    // P5c (rv, CONTRACT-P5C.md §5.3): the three texture shutters' server-side answers. See
    // MGPipeApplierState::TextureShutterSerial / ContextSerial for the bump rule.
    Uint64 MGPipeApplierTextureShutterSerial() {
        return MGPipeApplier().TextureShutterSerial;
    }
    Uint64 MGPipeApplierContextSerial() {
        return MGPipeApplier().ContextSerial;
    }
    void MGPipeApplierNoteTextureStateMoved() {
        ++MGPipeApplier().TextureShutterSerial;
    }

    void MGPipeSetResourceOps(const MGPipeResourceOps* ops) {
        g_resourceOps = ops;
    }
    const MGPipeResourceOps* MGPipeGetResourceOps() {
        return g_resourceOps;
    }

    namespace {
        // True (and counted in MGPipeApplierState::RefusedNoConsumer) when nothing in this
        // process consumes texture, framebuffer, sampler and program records: accepting one makes
        // the client clear dirty flags nobody will act on. It asks what the client's gate
        // (PipeFill.cpp's P4aFamilyHasItsConsumer) asks: the published consumer bit under a
        // transport, the registered op table in process. MGPipeApplierReset leaves g_resourceOps
        // alone, so the answer cannot flap across a make-current. Callers ask after the record's
        // shape checks (a malformed record is Fatal on every backend) and before anything moves;
        // calls that start with a resolution, and the unit sets, ask first.
        Bool NoP4aConsumer() {
#if MOBILEGL_BUILD_DISAGGREGATED
            if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
                const auto* session = MG_Remote::Server::ServerSession::Active();
                if (session && session->CallMaskIsSet() &&
                    MG_Remote::MGCapsServerConsumes(session->CallMask(), kMGPipeSubsystemTextureResources))
                    return false;
            }
#endif
            if (g_resourceOps != nullptr) return false;
            ++MGPipeApplier().RefusedNoConsumer;
            return true;
        }

        // P5e (sb, CONTRACT-P5E.md §5.6): the three binding-point windows, cleared in one
        // place because BOTH resets clear them and the two used to drift on exactly this kind
        // of member (the framebuffer family's four). The SERIAL is deliberately NOT touched
        // here - each caller advances it beside its own siblings, where the "advance, never
        // zero" rule is written out.
        void ClearShaderBufferWindows() {
            for (Uint32 cls = 0; cls < kMGPipeShaderBufferClassCount; ++cls) {
                MGPipeApplier().BoundShaderBuffers[cls] = {};
                MGPipeApplier().ShaderBufferStart[cls] = 0;
                MGPipeApplier().ShaderBufferCount[cls] = 0;
                for (Uint32 w = 0; w < kMGPipeShaderBufferWritableMaskWords; ++w) {
                    MGPipeApplier().ShaderBufferWritableMask[cls][w] = 0;
                }
            }
        }
    } // namespace

    void MGPipeApplierReset() {
#if MOBILEGL_BUILD_DISAGGREGATED
        // Under a transport the applier is server-private: the GL thread's reset crosses as the
        // applier_reset record, and ServerVerbSink::OnApplierReset runs this on the apply thread.
        // A direct call from a client-role thread (not the apply thread, not MGPipeServerArm())
        // while a session is live writes server memory with no record behind it: a named Fatal.
        // Before a session exists (before ClientSession::Start, or a ServerLoop fixture with no
        // client) the direct call is the only reset there is. MGPipeSessionLive() and
        // MGPipeServerArm() sit beside ClientSession::Active() and OnApplyThread(), not in their
        // place: fixtures without a full session have only the latter two.
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith &&
            (MG_Remote::Client::ClientSession::Active() != nullptr || MG_Pipe::MGPipeSessionLive()) &&
            !MG_Remote::Server::ServerLoop::OnApplyThread() && !MG_Pipe::MGPipeServerArm()) {
            MGLOG_F("MGPipe: Fatal{RoleViolation, \"g_applier\"} - MGPipeApplierReset() called "
                    "off the apply thread with an active transport; under split the reset "
                    "crosses as the applier_reset record (CONTRACT-P5C.md §5.1)");
            std::abort();
        }
#endif
        MGPipeApplier().RenderStateCsos.clear();
        MGPipeApplier().BoundRenderStateCso = kMGPipeNullHandle;
        MGPipeApplier().ScatteredChunkBits = 0;
        MGPipeApplier().PatchCarrierComparisons = 0;
        MGPipeApplier().PatchCarrierDivergences = 0;
        // This runs at every change of the current context, including a make-current back to a
        // live context (MGPipeTracker::Update resets whenever the current context changes), so it
        // clears only what a returning context may not inherit. Object records stay: GL objects
        // live in the share group, and a buffer's record holds the extent and mutation serial its
        // later writes and IsBufferDrawClean need. They go at the object's own death and at
        // MGPipeApplierReleaseObjectRecords. The op table stays too; the backend installs and
        // removes it at bring-up and teardown.
        MGPipeApplier().RefusedResourceCalls = 0;
        MGPipeApplier().RefusedVertexInputCalls = 0;
        MGPipeApplier().RefusedObjectCalls = 0;
        MGPipeApplier().RefusedNoConsumer = 0;
        MGPipeApplier().BoundVertexElements = kMGPipeNullHandle;
        MGPipeApplier().VertexBuffers = {};
        MGPipeApplier().VertexBufferStart = 0;
        MGPipeApplier().VertexBufferCount = 0;
        MGPipeApplier().VertexFetchBaseInstance = 0;
        MGPipeApplier().IndexBuffer = MGPIndexBuffer{};
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
        MGPipeApplier().MapPersistentRoundtrips = 0;
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
        ++MGPipeApplier().VertexBuffersSerial;
        ++MGPipeApplier().IndexBufferSerial;
        // P5c (rv): a make-current is a fresh server, and the two shutter serials are what the
        // PipeInputs texture-shutter accessors answer with - so both ADVANCE here, for the
        // serials' own reason: a counter that restarts walks back through values already
        // stamped into a memo that outlived the switch. ContextSerial's move is also
        // GetTextureContextId's whole job (the backends key their per-context memos on it).
        ++MGPipeApplier().TextureShutterSerial;
        ++MGPipeApplier().ContextSerial;

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
        MGPipeApplier().BoundFramebuffer = {};
        MGPipeApplier().BoundSamplerViews = {};
        MGPipeApplier().SamplerViewStart = 0;
        MGPipeApplier().SamplerViewCount = 0;
        MGPipeApplier().BoundSamplerStates = {};
        MGPipeApplier().SamplerStateStart = 0;
        MGPipeApplier().SamplerStateCount = 0;
        MGPipeApplier().BoundShaderImages = {};
        MGPipeApplier().ShaderImageStart = 0;
        MGPipeApplier().ShaderImageCount = 0;
        MGPipeApplier().DrawProgram = kMGPipeNullHandle;
        MGPipeApplier().DispatchProgram = kMGPipeNullHandle;
        MGPipeApplier().BoundShaderCso = kMGPipeNullHandle;
        MGPipeApplier().BoundStreamOutputLifetimeId = 0;
        // Begin is emitted once per span, not on make-current. Its immutable object
        // snapshot survives with the resource/program records until End or release;
        // set_context_values restores the returning context's bound lifetime id.
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
        ++MGPipeApplier().FramebufferSerial;
        ++MGPipeApplier().SamplerViewsSerial;
        ++MGPipeApplier().SamplerStatesSerial;
        ++MGPipeApplier().ShaderImagesSerial;
        ++MGPipeApplier().ProgramBindingSerial;
        ++MGPipeApplier().ShaderBuffersSerial;
    }

    void MGPipeApplierReleaseObjectRecords() {
        if ((g_resourceOps == nullptr || g_resourceOps->TextureDestroy == nullptr)) {
            MG_Record::ServerStagedTexture().DropAll();
        }
        // The served context is going away and this applier with it. Under split that is one
        // applier per served context; in the monolith there is one applier behind every
        // context, so nothing wires this - see PipeApply.h. The two serials advance here for
        // MGPipeApplierReset's reason: state was cleared, and a twin that outlives it must not
        // be able to match a value it has already seen.
        MGPipeApplier().Resources.clear();
        MGPipeApplier().VertexElementsCsos.clear();
        MGPipeApplier().BoundVertexElements = kMGPipeNullHandle;
        ++MGPipeApplier().VertexBuffersSerial;
        ++MGPipeApplier().IndexBufferSerial;
        // P5c (rv): same rule for the shutter serials - the served context is going away, and
        // a memo that outlives it must not match a value these have already answered with.
        ++MGPipeApplier().TextureShutterSerial;
        ++MGPipeApplier().ContextSerial;
        // P4a's five object tables go with them, and the working handles they could name go
        // too - a bound shader CSO whose record has just been dropped must not survive as a
        // handle the next call resolves against.
        MGPipeApplier().TextureResources.clear();
        MGPipeApplier().RenderbufferResources.clear();
        MGPipeApplier().SamplerCsos.clear();
        MGPipeApplier().SamplerViewCsos.clear();
        MGPipeApplier().ShaderCsos.clear();
        MGPipeApplier().CompositeShaderCsos.clear();
        // AND THE FRAMEBUFFER RECORDS, which MGPipeApplierReset above deliberately keeps. Here
        // there is nothing for them to outlive: the served context is going away and the
        // framebuffers it owned are container objects that go with it. A table left standing
        // would answer a lookup from the NEXT context on a slot the allocator has re-minted -
        // with the same generation, if the next context happens to reach the same one - and
        // that is the aliasing every other table is cleared to prevent.
        MGPipeApplier().FramebufferRecords.clear();
        MGPipeApplier().DrawProgram = kMGPipeNullHandle;
        MGPipeApplier().DispatchProgram = kMGPipeNullHandle;
        MGPipeApplier().BoundShaderCso = kMGPipeNullHandle;
        MGPipeApplier().BoundStreamOutputLifetimeId = 0;
        MGPipeApplier().StreamOutputSpans.clear();
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
        MGPipeApplier().BoundFramebuffer = {};
        MGPipeApplier().BoundSamplerViews = {};
        MGPipeApplier().SamplerViewStart = 0;
        MGPipeApplier().SamplerViewCount = 0;
        MGPipeApplier().BoundSamplerStates = {};
        MGPipeApplier().SamplerStateStart = 0;
        MGPipeApplier().SamplerStateCount = 0;
        MGPipeApplier().BoundShaderImages = {};
        MGPipeApplier().ShaderImageStart = 0;
        MGPipeApplier().ShaderImageCount = 0;
        // P5e (sb): and the three binding-point windows, for the paragraph above's reason in
        // its own words - every MGPBufferRange::Res names a resource record this function has
        // just dropped, so a window left populated here is a set of handles into an empty
        // table, which the next resolve either refuses or answers with somebody else's record
        // on a slot the next context re-mints.
        ClearShaderBufferWindows();
        ++MGPipeApplier().FramebufferSerial;
        ++MGPipeApplier().SamplerViewsSerial;
        ++MGPipeApplier().SamplerStatesSerial;
        ++MGPipeApplier().ShaderImagesSerial;
        ++MGPipeApplier().ProgramBindingSerial;
        ++MGPipeApplier().ShaderBuffersSerial;
    }

    void MGPipeApplyCreateRenderState(const MGPRenderStateDesc& desc, const void* chunkBytes) {
        MOBILEGL_ASSERT(desc.Cso.Slot >= kMGPipeFirstAllocatableSlot, "create_render_state named the reserved slot 0");
        MGPipeRenderStateCsoRecord* recordAt =
            RecordAt(MGPipeApplier().RenderStateCsos, desc.Cso.Slot, kMGPipeMaxRenderStateCsoSlots);
        if (recordAt == nullptr) {
#if MOBILEGL_BUILD_DISAGGREGATED
            MGPipeSessionFail(MGPipeFatalFamily::ProtocolCorruption,
                              "MGPipe: Fatal{ProtocolCorruption, \"CreateRenderState.Cso.Slot\"} - "
                              "create_render_state {slot=%u, gen=%u} is outside the record table bound (%u)",
                              desc.Cso.Slot, desc.Cso.Gen, kMGPipeMaxRenderStateCsoSlots);
#else
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG("ProtocolCorruption") " create_render_state {slot=%u, gen=%u} is outside "
                                                                   "the record table bound (%u)",
                desc.Cso.Slot, desc.Cso.Gen, kMGPipeMaxRenderStateCsoSlots);
#endif
            return;
        }
        MGPipeRenderStateCsoRecord& record = *recordAt;

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
                MGP_TRIP_WIRE_REPORT(
                    "MGPipe: " MGP_TRIP_WIRE_TAG(
                        "PipeIncompleteCso") " create_render_state {slot=%u, gen=%u} with no BaseCso named chunks "
                                             "0x%x, not the whole pipeline half 0x%x; the rest is zeroed",
                    desc.Cso.Slot, desc.Cso.Gen, desc.ChunkMask, static_cast<Uint32>(kAllPipelineChunks));
            }
        } else {
            const MGPipeRenderStateCsoRecord* base = FindCso(desc.BaseCso);
            record.PipelineBytes = base != nullptr ? base->PipelineBytes : Array<Uint8, kMGPipePipelineChunkBytes>{};
            if (base == nullptr) {
                MGP_TRIP_WIRE_REPORT(
                    "MGPipe: " MGP_TRIP_WIRE_TAG(
                        "PipeDeadBaseCso") " create_render_state {slot=%u, gen=%u} named a dead BaseCso "
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
        MOBILEGL_ASSERT(record != nullptr, "bind_render_state named a dead CSO {slot=%u, gen=%u}", bind.Cso.Slot,
                        bind.Cso.Gen);
        if (record == nullptr) return;

        PipeInputs& inputs = gPipeInputs;
        MGPipeScatterPipelineBytes(record->PipelineBytes.data(), MGPipeApplyAccess::RenderState(inputs));
        MGPipeApplyAccess::SetRenderStateVersions(inputs, bind.Version, bind.PipelineVersion);
        MGPipeApplier().BoundRenderStateCso = bind.Cso;
        // A bind scatters the WHOLE pipeline half - the record is always a complete one,
        // whatever mask minted it - so the pipeline chunks are all "moved" here, and all
        // enter the applier's ledger of the bytes it owns.
        const Uint32 moved = MGPipeGlobalChunkBitsOfPipelineMask(kAllPipelineChunks);
        MGPipeApplier().ScatteredChunkBits |= moved;
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
        if (MGPipeApplier().BoundRenderStateCso == handle.Handle) {
            MGPipeApplier().BoundRenderStateCso = kMGPipeNullHandle;
        }
    }

    void MGPipeApplySetDynamicState(const MGPDynamicState& dyn, const void* chunkBytes) {
        PipeInputs& inputs = gPipeInputs;
        MGPipeScatterDynamicChunks(chunkBytes, dyn.ChunkMask, MGPipeApplyAccess::RenderState(inputs));
        MGPipeApplyAccess::SetRenderStateParametersVersion(inputs, dyn.Version);
        const Uint32 moved = MGPipeGlobalChunkBitsOfDynamicMask(dyn.ChunkMask);
        MGPipeApplier().ScatteredChunkBits |= moved;
        MGPipeDeriveRenderStateFieldsForChunks(inputs, moved);
    }

    void MGPipeApplySetPixelPackState(const MGPPixelPackState& pack) {
#if MOBILEGL_PIPE_VERIFY
        MGPipeApplier().VerifyPixelPack = pack.Pack;
#endif
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
        MGPipeApplier().IsTransformFeedbackActive = values.IsTransformFeedbackActive != 0;
        MGPipeApplier().BoundStreamOutputLifetimeId = values.BoundTransformFeedbackLifetimeId;
    }

    void MGPipeApplySetPatchState(const MGPPatchState& patch) {
        PipeInputs& inputs = gPipeInputs;
        RenderStateParameters& working = MGPipeApplyAccess::RenderState(inputs);
        const FloatVec4 outer(patch.Outer[0], patch.Outer[1], patch.Outer[2], patch.Outer[3]);
        const FloatVec2 inner(patch.Inner[0], patch.Inner[1]);

        // Patch carrier trip wire. The patch trio travels in pipeline chunk P0 and again as
        // set_patch_state (both backends bake it into the synthesized control stage); if the two
        // ever part, a stale set_patch_state overwrites what bind_render_state scattered. Compared
        // bitwise so a NaN level equals itself. Armed only once this applier has scattered the
        // trio's chunk itself (kChunksPatchTrio in ScatteredChunkBits): before the context's first
        // bind there is nothing to agree with. Runs in every build; the cost is a 24-byte compare
        // on a call emitted only when tessellation state changes.
        if ((MGPipeApplier().ScatteredChunkBits & kChunksPatchTrio) == kChunksPatchTrio) {
            ++MGPipeApplier().PatchCarrierComparisons;
            const Bool agrees = working.PatchVertices == patch.Vertices &&
                                std::memcmp(&working.PatchDefaultOuterLevel, &outer, sizeof(outer)) == 0 &&
                                std::memcmp(&working.PatchDefaultInnerLevel, &inner, sizeof(inner)) == 0;
            if (!agrees) {
                ++MGPipeApplier().PatchCarrierDivergences;
                MGP_TRIP_WIRE_REPORT(
                    "MGPipe: " MGP_TRIP_WIRE_TAG(
                        "PipePatchCarriersDiffer") " set_patch_state says vertices=%u outer=(%g,%g,%g,%g) "
                                                   "inner=(%g,%g); "
                                                   "chunk P0 delivered vertices=%u outer=(%g,%g,%g,%g) inner=(%g,%g)",
                    patch.Vertices, static_cast<double>(outer.x()), static_cast<double>(outer.y()),
                    static_cast<double>(outer.z()), static_cast<double>(outer.w()), static_cast<double>(inner.x()),
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

    void MGPipeApplySetVertexAttribDefaults(const MGPVertexAttribDefaults& hdr, const MGPAttribValue* tail) {
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
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "PipeAttribTailMalformed") " set_vertex_attrib_defaults: %s (Mask=0x%x, Count=%u, consumed=%u)",
                fault, hdr.Mask, hdr.Count, consumed);
        }
    }

    // ================================================================================
    // The resource entry points.
    //
    // The applier owns identity, extent and order, not content: a buffer's bytes are the
    // backend's. Each body resolves the handle against this applier's record, checks the record
    // against its own declared extent, and only then moves the record and calls the backend.
    // Serials are server-owned and monotonic and never travel in a payload (ARCHITECTURE.md §2.2).
    // ================================================================================

    Bool MGPipeApplyResourceCreate(const MGPResourceDesc& desc) {
        if (!MGPipeRespecifyIsWholeResource(desc)) {
            MGP_TRIP_WIRE_REPORT("MGPipe: Fatal{ProtocolCorruption, \"ResourceCreate.RespecifyScope\"} - "
                                 "resource_create cannot carry a per-level respecify extent");
        }
        if (desc.Target != static_cast<Uint8>(MGPipeResourceTarget::TexBuffer) &&
            (desc.BufOffset != 0 || desc.BufSize != 0)) {
            MGP_TRIP_WIRE_REPORT("MGPipe: Fatal{ProtocolCorruption, \"ResourceCreate.BufferRange\"} - "
                                 "only a texture-buffer resource may carry BufOffset/BufSize");
        }
        MOBILEGL_ASSERT(desc.Resource.Slot >= kMGPipeFirstAllocatableSlot, "resource_create named the reserved slot 0");
        if (desc.Resource.Slot < kMGPipeFirstAllocatableSlot) return false;

        // P4a: THE TABLE IS CHOSEN BY THE DESCRIPTOR'S TARGET, and getting that wrong is the one
        // way this call can damage an object it was not about - slot 7 is a live Buffer, a live
        // Texture and a live Renderbuffer at the same time, in three independent slot spaces.
        Vector<MGPipeResourceRecord>* table = ResourceTableForTarget(desc.Target);
        if (table == nullptr) {
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " resource_create {slot=%u, gen=%u, glName=%u}: the descriptor names no "
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
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " resource_create {slot=%u, gen=%u, glName=%u}: the slot is outside the "
                                          "record table's bound (%u)",
                desc.Resource.Slot, desc.Resource.Gen, desc.GlNameForDiag, kMGPipeMaxResourceSlots);
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

    Bool MGPipeApplyAttachSharedImage(MGPipeHandle texture, Uint64 imageId) {
        MGPipeResourceRecord* record = FindIn(MGPipeApplier().TextureResources, texture);
        if (record == nullptr) return false;
        record->SharedImageId = imageId;
        // The storage moved, so every twin's synced serial is stale, and the sampling answer with it.
        ++record->Serial;
        MGPipeApplierNoteTextureStateMoved();
        return true;
    }

    Bool MGPipeApplyResourceRespecify(const MGPResourceDesc& desc, const void* initialBytes,
                                      const MGPRespecifiedLevel* level) {
        Vector<MGPipeResourceRecord>* table = ResourceTableForTarget(desc.Target);
        if (table == nullptr) {
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " resource_respecify {slot=%u, gen=%u, glName=%u}: the descriptor names "
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
        PinRespecifyScopeCoversItsDeclaration(desc, level, desc.Resource, "resource_respecify");
        if (level != nullptr && (desc.Target == kMGPipeResourceTargetBuffer ||
                                 desc.Target == static_cast<Uint8>(MGPipeResourceTarget::Renderbuffer) ||
                                 desc.Target == static_cast<Uint8>(MGPipeResourceTarget::TexBuffer))) {
            MGP_TRIP_WIRE_REPORT("MGPipe: Fatal{ProtocolCorruption, \"ResourceRespecify.Target\"} - "
                                 "this resource target cannot name a texture mip level");
        }
        if (!MGPipeRespecifyIsWholeResource(desc) && !MGPipeRespecifiedExtentCarrierIsCanonical(desc)) {
            MGP_TRIP_WIRE_REPORT("MGPipe: Fatal{ProtocolCorruption, \"ResourceRespecify.ExtentCarrier\"} - "
                                 "a named image level's depth word must fit in 32 bits (no high BufSize bits)");
        }
        if (level == nullptr && desc.Target != static_cast<Uint8>(MGPipeResourceTarget::TexBuffer) &&
            (desc.BufOffset != 0 || desc.BufSize != 0)) {
            MGP_TRIP_WIRE_REPORT("MGPipe: Fatal{ProtocolCorruption, \"ResourceRespecify.BufferRange\"} - "
                                 "a whole non-buffer resource may not carry BufOffset/BufSize");
        }

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
        // A shared image is the storage this call replaces - unless the call replaces none.
        if ((level != nullptr || !metadataOnly) && record->SharedImageId != 0) {
            MGLOG_D("MGPipe: texture %u leaves shared image %llu: a respecify (%s) redefined its storage",
                    desc.GlNameForDiag, static_cast<unsigned long long>(record->SharedImageId),
                    level != nullptr ? "one level" : "whole resource");
            record->SharedImageId = 0;
        }
        if (level != nullptr && desc.Target != static_cast<Uint8>(MGPipeResourceTarget::TexBuffer)) {
            // Scope and BufOffset/BufSize are transient respecify carriers, not resource state.
            MGPipeClearRespecifiedLevel(record->Desc);
            record->Desc.BufOffset = 0;
            record->Desc.BufSize = 0;
        }
        // THE SERIAL MOVES EITHER WAY, and for a metadata update it is the entire publication:
        // the twin re-derives its storage flags from the new mask at its next sync and decides
        // for itself whether the backend needs a recreate.
        ++record->Serial;
        // P5c (rv): a respecify can move a texture's shape, which is exactly what the
        // sampling-resolution shutter guarded - the server-side answer moves with it.
        MGPipeApplierNoteTextureStateMoved();

        // A respecify drops the pending uploads against the storage it replaces, and only those
        // (ARCHITECTURE.md §6.1): a box kept across a shrink would upload past the end of the new
        // level. A null `level` replaces the whole resource and drops every key, unless the call
        // is metadata-only, which replaces no storage and drops nothing. A named level (one
        // glTexImage*D) drops exactly that (uploadTarget, level), even if the descriptor did not
        // move; the other levels' dirty flags were cleared at emission, so their keys are the only
        // record of texels the backend still owes. Buffers never have pending uploads.
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

        // P5c (tx): the staged-texture store's defined-ness and drop bookkeeping rides THE SAME
        // scope rules as the pending-set drops above - a named level redefines that one
        // (uploadTarget, level), a whole-resource respecify drops every level, and a metadata
        // update replaces no storage and is not delivered. Without this the store could not say
        // whether a level exists at all (a null-data glTexImage*D carries no sub-data), and a
        // whole-resource respecify would leave levels keyed against a replaced coordinate
        // system. Textures only: a buffer's storage is the ops table's own Respecify hook, and
        // a renderbuffer has no levels.
        if (desc.Target != kMGPipeResourceTargetBuffer &&
            desc.Target != static_cast<Uint8>(MGPipeResourceTarget::Renderbuffer) &&
            (level != nullptr || !metadataOnly)) {
            if (g_resourceOps != nullptr && g_resourceOps->TextureRespecify != nullptr)
                g_resourceOps->TextureRespecify(desc.Resource, desc, level);
            else
                DefineTextureWithoutBackendHook(desc, level);
        }

        // resource_respecify is the only kNeedsAck call, and MGPipeResourceRespecifyNeedsAck(desc)
        // narrows the ack to an immutable buffer, i.e. glBufferStorage (ARCHITECTURE.md §8.3). In
        // process the ack is this function's return, so the predicate is not branched on;
        // PipeCatalogue.ResourceRespecifyAcksOnlyImmutableStorage pins it. A metadata-only update
        // never acks: the predicate is false for every non-buffer target, and
        // RespecifyRedefinesNoStorage never classifies a buffer as metadata-only.
        if (desc.Target != kMGPipeResourceTargetBuffer) return true;
        if (g_resourceOps != nullptr && g_resourceOps->Respecify != nullptr) {
            g_resourceOps->Respecify(desc.Resource, desc, initialBytes);
        }
        return true;
    }

    Bool MGPipeApplyResourceSubData(const MGPSubData& record, const void* bytes, const MGPSubRegion* regions) {
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
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " resource_subdata {slot=%u, gen=%u}: the record's resource target names "
                                          "no texture to upload into (target=%u, resource target=%u, upload "
                                          "target=%u)",
                record.Res.Slot, record.Res.Gen, record.Target, static_cast<Uint32>(resourceTarget),
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
    // MGPipeApplier().Resources and never the texture or renderbuffer table - which is right, because
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
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG(
                                     "ProtocolCorruption") " resource_flush_range {slot=%u, gen=%u, glName=%u}: %s "
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
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG(
                                     "ProtocolCorruption") " resource_readback {slot=%u, gen=%u, glName=%u}: %s "
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
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " resource_destroy {slot=%u, gen=%u}: the handle names no resource kind "
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
        // P5c (tx): with ONE exception - the staged-texture store is keyed by the handle, so
        // the death must reach it or a recycled slot's stale levels would answer for the
        // successor. This does not hand the texture to the buffer family's Destroy.
        if (static_cast<MGPipeKind>(handle.Kind) == MGPipeKind::Texture) {
            if (g_resourceOps != nullptr && g_resourceOps->TextureDestroy != nullptr)
                g_resourceOps->TextureDestroy(handle.Handle);
            else {
                MG_Record::ServerStagedTexture().Drop(MG_Record::StagedTextureStore::KeyForHandle(handle.Handle));
            }
        }
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
        ++MGPipeApplier().MapPersistentRoundtrips;

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
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith && MG_Remote::Client::AdoptTierIsEmulate()) {
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
        void* base = g_resourceOps->MapPersistent(handle.Handle, size, seedBytes);
        // An adopted store has no live host writer to announce: the client writes the store's
        // own memory and tells the applier nothing (BufferObject::NotePersistentMapStateChanged
        // sends no falling edge for it), so the rising edge the map published ends here.
        if (base != nullptr) record->HasLiveHostWrites = false;
        return base;
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
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " unmap_persistent {slot=%u, gen=%u}: the persistent donation is the "
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

#if MOBILEGL_BUILD_DISAGGREGATED
    Bool MGPipeApplyAdoptExternal(const MGPHandleOnly& handle, void* ahb, Uint64 size) {
        // The same kind verdict unmap_persistent gives: a store is the buffer family's.
        if (static_cast<MGPipeKind>(handle.Kind) != MGPipeKind::Buffer) {
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " adopt_external {slot=%u, gen=%u}: a T0 store is the buffer family's "
                                          "and the handle names another kind (%u)",
                handle.Handle.Slot, handle.Handle.Gen, handle.Kind);
            return false;
        }
        MGPipeResourceRecord* record = ResolveResource("adopt_external", handle.Handle);
        if (record == nullptr) return false;
        // The store the AHB replaces is the record's whole extent; a handle whose width moved
        // (a respecify the client has not caught up with) is not this store any more.
        if (size == 0 || size != record->Desc.Width) return false;
        // The same pin map_persistent takes: adoption is where a producer of live host writes
        // would attach, and T0 attaches none - the client writes the AHB through its lock, as a
        // monolith client writes its adopted map, and the applier is told nothing either way.
        PinNoLiveHostWrites(*record, handle.Handle, "adopt_external");
        if (g_resourceOps == nullptr || g_resourceOps->ImportExternal == nullptr) return false;
        if (!g_resourceOps->ImportExternal(handle.Handle, ahb, size)) return false;
        record->HasLiveHostWrites = false; // as map_persistent's mint: the adoption ends the state
        return true;
    }

    void* MGPipeApplyDonateInProcess(const MGPHandleOnly& handle) {
        if (static_cast<MGPipeKind>(handle.Kind) != MGPipeKind::Buffer) {
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " donate_in_process {slot=%u, gen=%u}: a T0 store is the buffer family's "
                                          "and the handle names another kind (%u)",
                handle.Handle.Slot, handle.Handle.Gen, handle.Kind);
            return nullptr;
        }
        MGPipeResourceRecord* record = ResolveResource("donate_in_process", handle.Handle);
        if (record == nullptr || record->Desc.Width == 0) return nullptr;
        // adopt_external's pin, for adopt_external's reason: the client writes the donated store
        // directly, as a monolith client writes its adopted map.
        PinNoLiveHostWrites(*record, handle.Handle, "donate_in_process");
        if (g_resourceOps == nullptr || g_resourceOps->DonateInProcess == nullptr) return nullptr;
        void* base = g_resourceOps->DonateInProcess(handle.Handle, record->Desc.Width);
        if (base != nullptr) record->HasLiveHostWrites = false; // as map_persistent's mint
        return base;
    }
#endif

    // ================================================================================
    // The five vertex-input entry points.
    //
    // None of them calls the backend. The backend reads this state at its own draw-time sync
    // and compares the three Uint64 serials kept here (ContentSerial, VertexBuffersSerial,
    // IndexBufferSerial) instead of a wrapping frontend version.
    // ================================================================================

    void MGPipeApplyCreateVertexElements(const MGPVertexElements& desc, const void* blobBytes) {
        MOBILEGL_ASSERT(desc.Cso.Slot >= kMGPipeFirstAllocatableSlot,
                        "create_vertex_elements named the reserved slot 0");
        if (desc.Cso.Slot < kMGPipeFirstAllocatableSlot) return;

        // The counts are checked before any blob byte is read, against the record's own layout:
        // MGPVertexAttribWire[AttributeCount] then MGPVertexBindingPointWire[BindingPointCount].
        // Both are bounded by kMGPipeMaxVertexAttribs, which also sizes the destination arrays, so
        // the counts bound the read. Blob.Size follows resource_subdata's rule (SubDataBoxFault):
        // non-zero must equal the length the counts describe; zero means the record does not
        // declare its blob, as with in-process emission, which passes the bytes in blobBytes.
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
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " create_vertex_elements {slot=%u, gen=%u}: %s (attributes=%u, "
                                          "bindingPoints=%u, that describes %llu bytes, blob declares %llu)",
                desc.Cso.Slot, desc.Cso.Gen, fault, desc.AttributeCount, desc.BindingPointCount,
                static_cast<unsigned long long>(declared), static_cast<unsigned long long>(desc.Blob.Size));
            return;
        }

        MGPipeVertexElementsRecord* recordAt =
            RecordAt(MGPipeApplier().VertexElementsCsos, desc.Cso.Slot, kMGPipeMaxVertexElementsSlots);
        if (recordAt == nullptr) {
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " create_vertex_elements {slot=%u, gen=%u}: the slot is outside the record "
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
        // A new configuration starts without kept bindings (see the record): the client drops
        // its mirror of them at the same create.
        record.StoredBuffersValid = false;
        record.StoredIndexValid = false;
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
            MGPipeApplier().BoundVertexElements = kMGPipeNullHandle;
            return;
        }
        // A dead handle leaves the PREVIOUS binding untouched, which is bind_render_state's
        // precedent for the same question, and is counted like every other refusal.
        const MGPipeVertexElementsRecord* record = ResolveVertexElements("bind_vertex_elements", handle.Handle);
        if (record == nullptr) return;
        MGPipeApplier().BoundVertexElements = handle.Handle;
        // The array's kept bindings come back with it, through the same two applies their
        // records take, so every serial and field moves exactly as it would for the records.
        // The window copy and the serial bumps are the two applies' own (the kept set passed
        // their gates when it arrived); only the store-back into this same record is skipped.
        auto& applier = MGPipeApplier();
        if (record->StoredBuffersValid) {
            const MGPVertexBuffers& header = record->StoredBuffersHeader;
            for (Uint32 i = 0; i < header.Count; ++i)
                applier.VertexBuffers[header.Start + i] = record->StoredBuffers[i];
            applier.VertexBufferStart = header.Start;
            applier.VertexBufferCount = header.Count;
            applier.VertexFetchBaseInstance = header.BaseInstance;
            ++applier.VertexBuffersSerial;
        }
        if (record->StoredIndexValid) {
            applier.IndexBuffer = record->StoredIndex;
            ++applier.IndexBufferSerial;
        }
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
        if (MGPipeApplier().BoundVertexElements == handle.Handle) {
            MGPipeApplier().BoundVertexElements = kMGPipeNullHandle;
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
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " set_vertex_buffers {start=%u, count=%u, hash=%llu}: %s (the applier holds "
                                          "%u entries)",
                hdr.Start, hdr.Count, static_cast<unsigned long long>(hdr.ContentHash), fault, kMGPipeMaxVertexAttribs);
            return;
        }

        // Copied into the window the record declares and nowhere else. The entries outside it
        // are not cleared: this record is "the last set as received", and a set that names
        // four entries has said nothing about the rest.
        for (Uint32 i = 0; i < hdr.Count; ++i) {
            MGPipeApplier().VertexBuffers[hdr.Start + i] = tail[i];
        }
        MGPipeApplier().VertexBufferStart = hdr.Start;
        MGPipeApplier().VertexBufferCount = hdr.Count;

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
        MGPipeApplier().VertexFetchBaseInstance = hdr.BaseInstance;
        ++MGPipeApplier().VertexBuffersSerial;
        // Kept by the bound array (MGPipeVertexElementsRecord's P14 note). A re-apply from the
        // bind writes back what it read, which is harmless.
        if (MGPipeVertexElementsRecord* bound = FindVertexElements(MGPipeApplier().BoundVertexElements)) {
            bound->StoredBuffersValid = hdr.Start == 0 && hdr.Count <= kMGPipeVaoStoredVertexBuffers;
            if (bound->StoredBuffersValid) {
                bound->StoredBuffersHeader = hdr;
                for (Uint32 i = 0; i < hdr.Count; ++i)
                    bound->StoredBuffers[i] = tail[i];
            }
        }
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
        MGPipeApplier().IndexBuffer = record;
        ++MGPipeApplier().IndexBufferSerial;
        // Kept by the bound array (MGPipeVertexElementsRecord's P14 note).
        if (MGPipeVertexElementsRecord* bound = FindVertexElements(MGPipeApplier().BoundVertexElements)) {
            bound->StoredIndex = record;
            bound->StoredIndexValid = true;
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

    // ================================================================================
    // The object and working-state entry points
    // ================================================================================
    //
    // None calls a backend function pointer: the backend reads these records at its own sync
    // points, keyed on server-owned serials. Same order as the resource entry points: resolve,
    // check the record against its own extent, then move it. A record this applier lacks is a
    // counted no-op (RefusedObjectCalls); one that would index outside storage is Fatal.

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
            if (state.DrawBuffers[i] < -1 || state.DrawBuffers[i] >= static_cast<Int8>(kMGPipeMaxColorAttachments)) {
                fault = "a draw-buffer entry names a colour attachment outside the record's own array";
            }
        }
        if (fault != nullptr) {
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " set_framebuffer_state {slot=%u, gen=%u, target=%u}: %s (the record "
                                          "carries %u colour attachments)",
                state.Fbo.Slot, state.Fbo.Gen, state.Target, fault, kMGPipeMaxColorAttachments);
            return;
        }

        // No handle is resolved: a framebuffer has a handle but no wire lifetime (no create or
        // destroy record), so this record overwrites the slot; the client frees the slot when the
        // object dies and reuse bumps the generation. Attachment Res handles are not resolved
        // either: attachments pin their textures on the client (ARCHITECTURE.md §5.6).
        // The consumer check follows the shape checks. This is the one entry point the client
        // reaches without PipeFill's gate (PipePublishFramebufferByName calls the emitter
        // directly), and it counts into RefusedNoConsumer, never RefusedObjectCalls.
        if (NoP4aConsumer()) return;

        MGPipeFramebufferRecord* record =
            RecordAt(MGPipeApplier().FramebufferRecords, state.Fbo.Slot, kMGPipeMaxFramebufferSlots);
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
            MGPipeApplier().BoundFramebuffer[static_cast<SizeT>(MGPipeFramebufferTarget::Draw)] = state.Fbo;
        }
        if (state.Target == static_cast<Uint8>(MGPipeFramebufferTarget::Read) ||
            state.Target == static_cast<Uint8>(MGPipeFramebufferTarget::Both)) {
            MGPipeApplier().BoundFramebuffer[static_cast<SizeT>(MGPipeFramebufferTarget::Read)] = state.Fbo;
        }
        // ONE SERIAL FOR THE FAMILY, and it moves once per applied record - a Both record is one
        // record and a Named record is one record. It is what retires the four g_fboSynced*
        // arrays and the twin's own {slot version, object version, backend id generation}
        // quadruple: a compare that used to ask "is my memo still the frontend's" asks "is my
        // serial still the server's". A Named record moves it for the same reason a bound one
        // does - a twin that memoised a framebuffer's attachments has to hear that they moved,
        // and whether the framebuffer happens to be bound is not that question.
        ++MGPipeApplier().FramebufferSerial;
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
        MOBILEGL_ASSERT(desc.Cso.Slot >= kMGPipeFirstAllocatableSlot, "create_sampler_state named the reserved slot 0");
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
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " create_sampler_state {slot=%u, gen=%u}: %s (one SamplerParameters is "
                                          "%llu bytes, the blob declares %llu)",
                desc.Cso.Slot, desc.Cso.Gen, fault, static_cast<unsigned long long>(sizeof(SamplerParameters)),
                static_cast<unsigned long long>(desc.Parameters.Size));
            return;
        }

        MGPipeSamplerCsoRecord* recordAt =
            RecordAt(MGPipeApplier().SamplerCsos, desc.Cso.Slot, kMGPipeMaxSamplerCsoSlots);
        if (recordAt == nullptr) {
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " create_sampler_state {slot=%u, gen=%u}: the slot is outside the record "
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
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::SamplerCso), "delete_sampler_state on kind %u",
                        handle.Kind);
        // A SAMPLER CSO IS ONE VALUE FOR THE WHOLE SESSION, and its delete drops it from every
        // share group that holds it. The client's cache is content-addressed and process-wide:
        // one handle names one canonical value, published into each group that uses it (a
        // record per group, all of them copies), and the cache's eviction frees the handle -
        // which ends it in every group at once, whichever context happens to be bound. Dropping
        // only the bound group's copy left every other group a record at a slot the client hands
        // out again. The handle space is the client's, so no other object can hold this slot at
        // this generation in any of the session's groups.
        if (const Vector<MGPipeObjectRecords*> groups = BoundSessionGroups(); !groups.empty()) {
            Bool dropped = false;
            for (MGPipeObjectRecords* group : groups) {
                MGPipeSamplerCsoRecord* held = FindIn(group->SamplerCsos, handle.Handle);
                if (held == nullptr) continue;
                const Uint32 gen = held->Gen;
                *held = MGPipeSamplerCsoRecord{};
                held->Gen = gen;
                dropped = true;
            }
            if (!dropped) (void)ResolveObject(MGPipeApplier().SamplerCsos, "delete_sampler_state", handle.Handle);
            return;
        }
        MGPipeSamplerCsoRecord* record =
            ResolveObject(MGPipeApplier().SamplerCsos, "delete_sampler_state", handle.Handle);
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
        MOBILEGL_ASSERT(view.Cso.Slot >= kMGPipeFirstAllocatableSlot, "create_sampler_view named the reserved slot 0");
        if (view.Cso.Slot < kMGPipeFirstAllocatableSlot) return;

        MGPipeSamplerViewRecord* recordAt =
            RecordAt(MGPipeApplier().SamplerViewCsos, view.Cso.Slot, kMGPipeMaxSamplerViewSlots);
        if (recordAt == nullptr) {
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " create_sampler_view {slot=%u, gen=%u}: the slot is outside the record "
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
        MGPipeResourceRecord* texture = FindIn(MGPipeApplier().TextureResources, view.Texture);
        if (texture != nullptr) texture->ViewCso = view.Cso;
    }

    void MGPipeApplyDeleteSamplerView(const MGPHandleOnly& handle) {
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::SamplerViewCso),
                        "delete_sampler_view on kind %u", handle.Kind);
        MGPipeSamplerViewRecord* record =
            ResolveObject(MGPipeApplier().SamplerViewCsos, "delete_sampler_view", handle.Handle);
        if (record == nullptr) return;

        // The back-pointer goes first, while the record that names the texture still exists,
        // and only if that texture still names THIS view - a texture whose view has already
        // been re-minted must not have the new handle cleared out from under it.
        MGPipeResourceRecord* texture = FindIn(MGPipeApplier().TextureResources, record->View.Texture);
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
            ResolveObject(MGPipeApplier().TextureResources, "set_texture_params", params.Res);
        if (record == nullptr) return false;

        // EVERY ITextureObject OWNS A SamplerObject, so the built-in sampler CSO is not
        // optional and a null handle is not "no sampler" - it is a record that would have the
        // backend sample a texture with whatever filter and wrap state the unit last left
        // behind. GL 4.6 table 23.18 makes filter/wrap/compare/border sampler state and Espryt
        // pushes it onto the TEXTURE with glTexParameter*, so this handle is the only thing
        // that says which values those are.
        if (MGPipeHandleIsNull(params.BuiltinSampler)) {
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " set_texture_params {slot=%u, gen=%u, glName=%u}: the record names no "
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

    // The three unit sets carry no stage dimension: the texture-unit space is one merged array
    // of 192, a unit may be sampled from two stages, and the server derives stage from the
    // reflection archive only where the target API needs it. A null handle in a tail entry is
    // legal (unresolved unit; no sampler object, so the texture's built-in sampler applies; no
    // texture), and nothing is resolved here: the backend pairs a set with the object records at
    // its own sync point. The consumer check runs before ApplyUnitWindow, which validates and
    // writes in one step, so a backend with no consumer declines a malformed window instead of
    // tripping; the client's gate never sends it one, and a consuming backend still trips.
    void MGPipeApplySetSamplerViews(const MGPSamplerViews& hdr, const MGPBoundView* tail) {
        if (NoP4aConsumer()) return;
        if (!ApplyUnitWindow("set_sampler_views", hdr.Start, hdr.Count, hdr.ContentHash, tail,
                             MGPipeApplier().BoundSamplerViews, MGPipeApplier().SamplerViewStart,
                             MGPipeApplier().SamplerViewCount)) {
            return;
        }
        ++MGPipeApplier().SamplerViewsSerial;
        // P5c (rv): a texture bind is what the bind-generation shutter guarded.
        MGPipeApplierNoteTextureStateMoved();
    }

    void MGPipeApplyBindSamplerStates(const MGPSamplerStates& hdr, const MGPipeHandle* tail) {
        if (NoP4aConsumer()) return;
        if (!ApplyUnitWindow("bind_sampler_states", hdr.Start, hdr.Count, hdr.ContentHash, tail,
                             MGPipeApplier().BoundSamplerStates, MGPipeApplier().SamplerStateStart,
                             MGPipeApplier().SamplerStateCount)) {
            return;
        }
        ++MGPipeApplier().SamplerStatesSerial;
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
                             MGPipeApplier().BoundShaderImages, MGPipeApplier().ShaderImageStart,
                             MGPipeApplier().ShaderImageCount)) {
            return;
        }
        ++MGPipeApplier().ShaderImagesSerial;
        // P5c (rv): an image bind moves the texture bind generation's guarded set too.
        MGPipeApplierNoteTextureStateMoved();
    }

    // ---------------------------------------------------------------------------------
    // P5e's two entry points, DECLARED BY c0e (docs/archive/contracts/CONTRACT-P5E.md §1)
    // ---------------------------------------------------------------------------------
    //
    // set_shader_buffers HAS ITS BODY (package sb); set_program_bindings still refuses by name
    // until package pg writes its own. A record that reached an unbodied one would be a binding
    // set the server accepted and dropped, so the answer there is a named abort rather than a
    // quiet return: the picture would be wrong and the lane would be green, which is the one
    // failure mode this campaign spends its refusals on.

    // set_shader_buffers: one record per class (uniform, storage, atomic counter). The body
    // validates the window, copies the tail into that class's array, takes the writable mask and
    // moves the serial; the backend resolves each range's handle at its own sync point. It does
    // not use ApplyUnitWindow, which serves the one merged texture-unit space, because this row
    // has three windows keyed by Class and its own named refusals. There is no consumer check:
    // the client sends these only to a server that publishes the buffer-binding consumer
    // (PipeFill.cpp's P5eFamilyIsLive), and an unread window loses nothing because the emitter
    // latches only its set-hash suppressor.
    void MGPipeApplySetShaderBuffers(const MGPShaderBuffers& hdr, const MGPBufferRange* tail) {
        if (hdr.Class >= kMGPipeShaderBufferClassCount) {
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG(
                                     "ProtocolCorruption") " SetShaderBuffers.Class - class %u names no binding-point "
                                                           "array; the three are Uniform=0, ShaderStorage=1, "
                                                           "AtomicCounter=2",
                                 hdr.Class);
            return;
        }
        const Uint64 end = Uint64{hdr.Start} + Uint64{hdr.Count};
        if (end > kMGPipeMaxBufferBindingPoints || (hdr.Count != 0 && tail == nullptr)) {
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " SetShaderBuffers.Count {class=%u, start=%u, count=%u}: the "
                                          "window runs past the %u binding points the wire carries, or a "
                                          "non-empty set carries no ranges",
                hdr.Class, hdr.Start, hdr.Count, static_cast<unsigned>(kMGPipeMaxBufferBindingPoints));
            return;
        }
        // THE WINDOW IS THE BOUND AND ENTRIES OUTSIDE IT ARE NOT CLEARED, exactly as the three
        // unit sets and set_vertex_buffers work: the record is "the last set as received", and
        // a binding at or above Count means "nothing bound" to every reader (the frontend
        // array's default says the same thing, which is what makes the two pictures agree).
        auto& destination = MGPipeApplier().BoundShaderBuffers[hdr.Class];
        for (Uint32 i = 0; i < hdr.Count; ++i)
            destination[hdr.Start + i] = tail[i];
        MGPipeApplier().ShaderBufferStart[hdr.Class] = hdr.Start;
        MGPipeApplier().ShaderBufferCount[hdr.Class] = hdr.Count;
        for (Uint32 w = 0; w < kMGPipeShaderBufferWritableMaskWords; ++w) {
            MGPipeApplier().ShaderBufferWritableMask[hdr.Class][w] = hdr.WritableMask[w];
        }
        // ONE SERIAL FOR ALL THREE CLASSES, and it moves on EVERY applied record - a backend
        // memo that asks "have the binding points moved since I last bound them" must hear
        // about a uniform set exactly as it hears about a storage set, and splitting the serial
        // per class would make each of the four consumers pick which one to read.
        ++MGPipeApplier().ShaderBuffersSerial;
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
        if (hdr.BlockBindingCount > kMGPipeMaxProgramBlockBindings)
            fault = "BlockBindingCount";
        else if (hdr.SamplerUnitCount > kMGPipeMaxProgramSamplerUnits)
            fault = "SamplerUnitCount";
        else if (hdr.StorageOverrideCount > kMGPipeMaxProgramStorageOverrides)
            fault = "StorageOverrideCount";
        else if (hdr.BlockBindingCount != 0 && blockBindings == nullptr)
            fault = "BlockBindings";
        else if (hdr.SamplerUnitCount != 0 && samplerUnits == nullptr)
            fault = "SamplerUnits";
        else if (hdr.StorageOverrideCount != 0 && (storageOverrides == nullptr || storageOverrideNames == nullptr)) {
            fault = "StorageOverrides";
        }
        if (fault != nullptr) {
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " set_program_bindings {slot=%u, gen=%u}: %s is out of range or "
                                          "its tail is absent (%u/%u/%u declared)",
                hdr.Cso.Slot, hdr.Cso.Gen, fault, hdr.BlockBindingCount, hdr.SamplerUnitCount,
                hdr.StorageOverrideCount);
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
            // A plain multi-draw is not escalated. Espryt's indirect multi-draw tiers resolve the
            // GL_DRAW_INDIRECT_BUFFER binding from a handle (MultiDraw.cpp's
            // BoundDrawIndirectBufferId) instead of reading client state, and the tier is chosen on
            // the server per batch, so no clause both roles can compute would name the arm that
            // needs it. Judge a new escalation by which arm pays for it (see MGPipeBarriered's
            // declaration in PipeApply.h).
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

    Bool MGPipeApplierCurrentRecordIsBarriered() {
        return g_currentRecordBarriered;
    }

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
    // The shader CSO, the two program bindings and the default uniform block.
    //
    // The server never re-links: SPIR-V and the reflection archive cross, never source, and
    // GL_LINK_STATUS is answered by the client from its ProgramObject (ARCHITECTURE.md §7).
    // The other inputs a backend program is specialised on (framebuffer clamp masks, storage
    // binding signature, image formats, patch parameters) arrive through other records.
    // ================================================================================

    void MGPipeApplyCreateShaderState(const MGPProgramDesc& desc, const MG_State::GLState::LinkArtifacts* link,
                                      const MG_State::GLState::SpirvArtifacts* spirv,
                                      SharedPtr<const MG_State::GLState::ProgramArchive> archive) {
        MOBILEGL_ASSERT(desc.Cso.Slot >= kMGPipeFirstAllocatableSlot, "create_shader_state named the reserved slot 0");
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
            MGP_TRIP_WIRE_REPORT("MGPipe: " MGP_TRIP_WIRE_TAG(
                                     "ProtocolCorruption") " create_shader_state {slot=%u, gen=%u}: %s (stages=0x%x, "
                                                           "globalUboSize=%u, bound %u)",
                                 desc.Cso.Slot, desc.Cso.Gen, fault, desc.StageMask, desc.GlobalUboSize,
                                 kMGPipeMaxGlobalConstantsBytes);
            return;
        }

        MGPipeShaderCsoRecord* recordAt = ShaderCsoRecordAt(desc.Cso.Slot);
        if (recordAt == nullptr) {
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " create_shader_state {slot=%u, gen=%u}: the slot is outside the record "
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
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::ShaderCso), "bind_shader_state on kind %u",
                        handle.Kind);
        // P4a's belt, and first for set_texture_params' reason: with no consumer this applier
        // holds no shader CSO record, so the resolution below would report the designed state
        // as RefusedObjectCalls. The null-handle unbind is behind it too - a binding this
        // applier never made is not one it may clear.
        if (NoP4aConsumer()) return;
        // The null handle is legal and means "nothing bound", which is a GL state and not an
        // error; a DEAD handle leaves the previous binding untouched and is counted, which is
        // bind_render_state's precedent for the same question.
        if (MGPipeHandleIsNull(handle.Handle)) {
            MGPipeApplier().BoundShaderCso = kMGPipeNullHandle;
            ++MGPipeApplier().ProgramBindingSerial;
            return;
        }
        const MGPipeShaderCsoRecord* record = ResolveShaderCso("bind_shader_state", handle.Handle);
        if (record == nullptr) return;
        MGPipeApplier().BoundShaderCso = handle.Handle;
        ++MGPipeApplier().ProgramBindingSerial;
    }

    void MGPipeApplyDeleteShaderState(const MGPHandleOnly& handle) {
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::ShaderCso), "delete_shader_state on kind %u",
                        handle.Kind);
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
        if (MGPipeApplier().BoundShaderCso == handle.Handle) {
            MGPipeApplier().BoundShaderCso = kMGPipeNullHandle;
            cleared = true;
        }
        if (MGPipeApplier().DrawProgram == handle.Handle) {
            MGPipeApplier().DrawProgram = kMGPipeNullHandle;
            cleared = true;
        }
        if (MGPipeApplier().DispatchProgram == handle.Handle) {
            MGPipeApplier().DispatchProgram = kMGPipeNullHandle;
            cleared = true;
        }
        if (cleared) ++MGPipeApplier().ProgramBindingSerial;
    }

    // TWO CALLS AND NOT ONE, because the frontend has two joins and two PipeInputs slots: a
    // draw resolves GetProgramForDraw and a dispatch resolves GetProgramForDispatch, and a
    // pipeline object flattened into a composite is the first of those and never the second.
    void MGPipeApplySetDrawProgram(const MGPHandleOnly& handle) {
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::ShaderCso), "set_draw_program on kind %u",
                        handle.Kind);
        if (NoP4aConsumer()) return; // P4a's belt, for MGPipeApplyBindShaderState's reason.
        if (MGPipeHandleIsNull(handle.Handle)) {
            MGPipeApplier().DrawProgram = kMGPipeNullHandle;
            ++MGPipeApplier().ProgramBindingSerial;
            return;
        }
        const MGPipeShaderCsoRecord* record = ResolveShaderCso("set_draw_program", handle.Handle);
        if (record == nullptr) return;
        MGPipeApplier().DrawProgram = handle.Handle;
        ++MGPipeApplier().ProgramBindingSerial;
    }

    void MGPipeApplySetDispatchProgram(const MGPHandleOnly& handle) {
        MOBILEGL_ASSERT(handle.Kind == static_cast<Uint32>(MGPipeKind::ShaderCso), "set_dispatch_program on kind %u",
                        handle.Kind);
        if (NoP4aConsumer()) return; // P4a's belt, for MGPipeApplyBindShaderState's reason.
        if (MGPipeHandleIsNull(handle.Handle)) {
            MGPipeApplier().DispatchProgram = kMGPipeNullHandle;
            ++MGPipeApplier().ProgramBindingSerial;
            return;
        }
        const MGPipeShaderCsoRecord* record = ResolveShaderCso("set_dispatch_program", handle.Handle);
        if (record == nullptr) return;
        MGPipeApplier().DispatchProgram = handle.Handle;
        ++MGPipeApplier().ProgramBindingSerial;
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
        // A span (MGPGlobalConstants::Range) lands on the block the record already holds.
        const Uint32 spanOffset = record.Range >> 16;
        const Uint32 spanLength = record.Range & 0xffffu;
        const char* fault = nullptr;
        if (record.Version == ~Uint32{0}) {
            fault = "the version is the backends' never-uploaded sentinel, which no record may carry";
        } else if (record.Range != 0 && (spanLength == 0 || spanOffset + spanLength > size)) {
            fault = "the span runs outside the program's default uniform block";
        } else if (record.Range != 0 && stored->GlobalConstants.size() != size) {
            fault = "a span arrived for a block the record does not hold";
        } else if (record.Range != 0 && record.Blob.Size != 0 && record.Blob.Size != spanLength) {
            fault = "the declared blob length is not the span's length";
        } else if (record.Range != 0 && bytes == nullptr) {
            fault = "a span carries no bytes";
        } else if (record.Range == 0 && record.Blob.Size != 0 && record.Blob.Size != size) {
            fault = "the declared blob length is not the program's own default uniform block size";
        } else if (size > kMGPipeMaxGlobalConstantsBytes) {
            fault = "the program's default uniform block is larger than any program may declare";
        } else if (size != 0 && bytes == nullptr) {
            fault = "a non-empty block carries no bytes";
        }
        if (fault != nullptr) {
            MGP_TRIP_WIRE_REPORT(
                "MGPipe: " MGP_TRIP_WIRE_TAG(
                    "ProtocolCorruption") " set_global_constants {slot=%u, gen=%u}: %s (version=%u, the program "
                                          "declares %u bytes, the blob declares %llu)",
                record.ShaderCso.Slot, record.ShaderCso.Gen, fault, record.Version, size,
                static_cast<unsigned long long>(record.Blob.Size));
            return;
        }

        if (record.Range != 0) {
            std::memcpy(stored->GlobalConstants.data() + spanOffset, bytes, spanLength);
        } else if (size == 0) {
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

    // MGPipeUnmigratedEmulation, below: a backend emulation that reads or writes client memory
    // names itself with a literal. In process it is a no-op and the emulation runs; under a
    // transport the server has no client memory, so it aborts with Fatal{UnmigratedEmulation}
    // rather than degrade silently. It arms on the transport, not the build, because split
    // builds also run in-process lanes where such code is correct. No log or counter: it may sit
    // on hot paths. Product code has no call sites; scripts/ci/unmigrated_emulation_sites.py
    // and PipeCatalogue.EveryUnmigratedEmulationIsNamedOnce keep the names and sites equal.
    namespace {
        thread_local Bool tl_serverOriginatedWrite = false;
    } // namespace

    Bool MGPipeApplyingServerOriginatedWrite() {
        return tl_serverOriginatedWrite;
    }
    MGPipeServerOriginatedWriteScope::MGPipeServerOriginatedWriteScope() : m_previous(tl_serverOriginatedWrite) {
        tl_serverOriginatedWrite = true;
    }
    MGPipeServerOriginatedWriteScope::~MGPipeServerOriginatedWriteScope() {
        tl_serverOriginatedWrite = m_previous;
    }

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
