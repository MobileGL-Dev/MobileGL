// MobileGL - MobileGL/MG_Test/Pipe/RenderStateSpansTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// G7: the render-state chunk table, its subset hash and the setter-consistency walk
// (P2 brief D19). Four cases:
//
//   ChunkTablePartitionsTheBlock           - the table is sorted, non-overlapping and
//                                            complete, and its two halves are exactly the
//                                            membership generated/PipeSpanTable.inc names.
//   SetterConsistency                      - THE gate. For every public RenderState setter,
//                                            the pipeline-subset hash moves IF AND ONLY IF
//                                            m_pipelineStateVersion moves.
//   DerivationMatchesTheFrontendGetters    - D5's 29 derived PipeInputs fields against the
//                                            frontend getters they were transcribed from.
//   DynamicChunksCoverMagmasDynamicTailKey - every GL-state input of DirectVulkan's
//                                            DynamicTailKey, against the dynamic half.
//
// The suite needs the push sources (MGPipeRenderStateSpans.cpp and PipeApply.cpp are
// compiled only under MOBILEGL_PIPE_PUSH), so every case is a visible SKIP in a pull build
// rather than a vanishing test - and the four names are the SAME four in every build, which
// is what keeps `ctest -N` name-for-name identical between the pull and the push tree.
#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>

#include "Includes.h"
#include <MG_Pipe/MGPipe.h>

#if MOBILEGL_PIPE_PUSH
#include <MG_Backend/MGPipe/PipeInputs.h>
#include <MG_Pipe/MGPipeRenderStateSpans.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/RenderState/RenderState.h>
#include <MG_Test/ScopedPipeVerb.h>
#endif

using namespace MobileGL;
using namespace MobileGL::MG_Pipe;

namespace {
#if MOBILEGL_PIPE_PUSH
    using MG_State::GLState::RenderState;
    using GLContext = MG_State::GLState::GLContext;

    // ---------------------------------------------------------------------------------
    // The member table, built from the SAME list gen_pipe.py checks against the struct
    // (PipeFields.def's MGP_FIELDS_RenderStateParameters). Using that list rather than a
    // hand-written one is what makes "every member is accounted for" true: a member added
    // to RenderStateParameters without a row there already fails pipe-gates, and a member
    // added WITH a row lands here automatically and has to be classified by the test.
    // ---------------------------------------------------------------------------------
    struct Member {
        const char* Name;
        SizeT Offset;
        SizeT Size;
    };

#define MGP_RS_MEMBER_ROW(name) \
    Member{#name, offsetof(RenderStateParameters, name), sizeof(RenderStateParameters::name)},
    constexpr Member kMembers[] = {MGP_FIELDS_RenderStateParameters(MGP_RS_MEMBER_ROW)};
#undef MGP_RS_MEMBER_ROW
    constexpr SizeT kMemberCount = sizeof(kMembers) / sizeof(kMembers[0]);

    SizeT OverlapWith(const MGPStateChunk* chunks, SizeT chunkCount, SizeT offset, SizeT size) {
        SizeT covered = 0;
        for (SizeT i = 0; i < chunkCount; ++i) {
            const SizeT chunkBegin = chunks[i].Offset;
            const SizeT chunkEnd = chunkBegin + chunks[i].Length;
            const SizeT begin = offset > chunkBegin ? offset : chunkBegin;
            const SizeT end = (offset + size) < chunkEnd ? (offset + size) : chunkEnd;
            if (begin < end) covered += end - begin;
        }
        return covered;
    }

    SizeT PipelineBytesOf(SizeT offset, SizeT size) {
        return OverlapWith(kMGPipePipelineChunks, kMGPipePipelineChunkCount, offset, size);
    }

    Bool IsWhollyDynamic(SizeT offset, SizeT size) {
        return OverlapWith(kMGPipeDynamicChunks, kMGPipeDynamicChunkCount, offset, size) == size;
    }

    Bool IsWhollyPipeline(SizeT offset, SizeT size) { return PipelineBytesOf(offset, size) == size; }

    // ---------------------------------------------------------------------------------
    // D5's 25 kDraw derivations against the frontend getters they were transcribed from.
    // Factored out because two cases need exactly this comparison: the whole-block walk
    // below, and the INCREMENTAL walk that drives the applier's chunk-scoped derivation one
    // family at a time. Everything here is a kDraw fill point (MG_Pipe/FillPoints.def), so
    // the caller must be inside a kDraw verb; the three clear values and GetClampReadColor
    // belong to kClear and kReadback and are checked in their own phases.
    // ---------------------------------------------------------------------------------
    void ExpectDerivedDrawFieldsMatch(GLContext& ctx, const char* tag) {
        SCOPED_TRACE(tag);
        EXPECT_EQ(gPipeInputs.GetBlendColor(), ctx.GetBlendColor());
        for (Uint i = 0; i < kMGMaxDrawBuffers; ++i) {
            BlendEquation gotColor{}, gotAlpha{}, wantColor{}, wantAlpha{};
            gPipeInputs.GetBlendEquationIndexed(i, gotColor, gotAlpha);
            ctx.GetBlendEquationIndexed(i, wantColor, wantAlpha);
            EXPECT_EQ(gotColor, wantColor) << "blend equation " << i;
            EXPECT_EQ(gotAlpha, wantAlpha) << "blend equation " << i;

            BlendFactor gotSrcRGB{}, gotDstRGB{}, gotSrcA{}, gotDstA{};
            BlendFactor wantSrcRGB{}, wantDstRGB{}, wantSrcA{}, wantDstA{};
            gPipeInputs.GetBlendFuncIndexed(i, gotSrcRGB, gotDstRGB, gotSrcA, gotDstA);
            ctx.GetBlendFuncIndexed(i, wantSrcRGB, wantDstRGB, wantSrcA, wantDstA);
            EXPECT_EQ(gotSrcRGB, wantSrcRGB) << "blend func " << i;
            EXPECT_EQ(gotDstRGB, wantDstRGB) << "blend func " << i;
            EXPECT_EQ(gotSrcA, wantSrcA) << "blend func " << i;
            EXPECT_EQ(gotDstA, wantDstA) << "blend func " << i;

            EXPECT_EQ(gPipeInputs.GetColorMaskIndexed(i), ctx.GetColorMaskIndexed(i)) << "colour mask " << i;
            EXPECT_EQ(gPipeInputs.IsCapabilityEnabledIndexed(CapabilityInput::Blend, i),
                      ctx.IsCapabilityEnabledIndexed(CapabilityInput::Blend, i))
                << "indexed blend enable " << i;
        }
        EXPECT_EQ(gPipeInputs.GetCullFaceMode(), ctx.GetCullFaceMode());
        EXPECT_EQ(gPipeInputs.GetDepthFunc(), ctx.GetDepthFunc());
        EXPECT_EQ(gPipeInputs.GetDepthMask(), ctx.GetDepthMask());
        for (Uint i = 0; i < RenderStateParameters::MAX_VIEWPORTS; ++i) {
            EXPECT_EQ(gPipeInputs.GetDepthRangeIndexed(i), ctx.GetDepthRangeIndexed(i)) << "depth range " << i;
            EXPECT_EQ(gPipeInputs.GetViewportIndexed(i), ctx.GetViewportIndexed(i)) << "viewport " << i;
            EXPECT_EQ(gPipeInputs.IsCapabilityEnabledIndexed(CapabilityInput::ScissorTest, i),
                      ctx.IsCapabilityEnabledIndexed(CapabilityInput::ScissorTest, i))
                << "indexed scissor enable " << i;
        }
        EXPECT_EQ(gPipeInputs.GetLineWidth(), ctx.GetLineWidth());
        EXPECT_EQ(gPipeInputs.GetLogicOp(), ctx.GetLogicOp());
        EXPECT_EQ(gPipeInputs.GetMinSampleShadingValue(), ctx.GetMinSampleShadingValue());
        EXPECT_EQ(gPipeInputs.GetPatchDefaultInnerLevel(), ctx.GetPatchDefaultInnerLevel());
        EXPECT_EQ(gPipeInputs.GetPatchDefaultOuterLevel(), ctx.GetPatchDefaultOuterLevel());
        EXPECT_EQ(gPipeInputs.GetPatchVertices(), ctx.GetPatchVertices());
        EXPECT_EQ(gPipeInputs.GetPolygonModeFront(), ctx.GetPolygonModeFront());
        EXPECT_EQ(gPipeInputs.GetPolygonOffsetFactor(), ctx.GetPolygonOffsetFactor());
        EXPECT_EQ(gPipeInputs.GetPolygonOffsetUnits(), ctx.GetPolygonOffsetUnits());
        EXPECT_EQ(gPipeInputs.GetPrimitiveRestartIndex(), ctx.GetPrimitiveRestartIndex());
        EXPECT_EQ(gPipeInputs.GetProvokingVertexMode(), ctx.GetProvokingVertexMode());
        EXPECT_EQ(gPipeInputs.GetScissorBox(), ctx.GetScissorBox());
        EXPECT_EQ(gPipeInputs.GetViewport(), ctx.GetViewport());
        for (const StencilFace face : {StencilFace::Front, StencilFace::Back}) {
            const StencilFaceState& got = gPipeInputs.GetStencilState(face);
            const StencilFaceState& want = ctx.GetStencilState(face);
            EXPECT_EQ(std::memcmp(&got, &want, sizeof(StencilFaceState)), 0)
                << "stencil face " << static_cast<int>(face);
        }
        for (SizeT i = 0; i < static_cast<SizeT>(CapabilityInput::CapabilityInputCount); ++i) {
            const CapabilityInput cap = static_cast<CapabilityInput>(i);
            EXPECT_EQ(gPipeInputs.IsCapabilityEnabled(cap), ctx.IsCapabilityEnabled(cap)) << "capability " << i;
        }
    }

    // THE ROUND TRIP D2's whole argument rests on: "the block they read IS the assembled
    // block" (ARCHITECTURE.md 5.3), which is what lets Espryt's SyncRenderState stay
    // untouched. The 29 derived fields cover barely half of RenderStateParameters; the other
    // ~25 members - SampleCoverageValue/Invert, SampleMaskValue, PolygonModeBack, PointSize,
    // PointFadeThresholdSize, PointSpriteCoordOrigin, the four hints, ClipOrigin,
    // ClipDepthMode, PolygonOffsetClamp, FrontFaceModeSetting, ScissorBoxes[1..15],
    // ScissorBoxWrittenMask, ClipDistanceEnabledMask and the raw capability bools - have no
    // derived field at all and are read RAW, through Espryt's span memcmp. This one line is
    // the only thing in the suite that covers them.
    void ExpectAssembledBlockIsTheLiveBlock(GLContext& ctx, const char* tag) {
        SCOPED_TRACE(tag);
        EXPECT_EQ(std::memcmp(&gPipeInputs.GetRenderStateParameters(), &ctx.GetRenderStateParameters(),
                              sizeof(RenderStateParameters)),
                  0)
            << "the assembled working block is not byte-identical to the live one - a chunk of "
               "RenderStateParameters is not being carried, and Espryt reads those bytes raw";
    }
#endif // MOBILEGL_PIPE_PUSH

    // -------------------------------------------------------------------------------------
    // 1. The table itself.
    // -------------------------------------------------------------------------------------
    TEST(RenderStateSpans, ChunkTablePartitionsTheBlock) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
#else
        // Sorted, non-overlapping and complete. That is already a static_assert in
        // MGPipeRenderStateSpans.h - a gap there is a build break, not a red test - and the
        // point of re-asserting it at run time is that a reader of the suite sees the
        // invariant stated rather than having to find the header.
        SizeT walked = 0;
        for (SizeT i = 0; i < kMGPipeRenderStateChunkCount; ++i) {
            const MGPStateChunk chunk = MGPipeRenderStateChunkAt(i);
            EXPECT_EQ(SizeT{chunk.Offset}, walked) << "chunk " << i << " does not start where its predecessor ended";
            EXPECT_GT(chunk.Length, 0u) << "chunk " << i << " is empty";
            walked = SizeT{chunk.Offset} + SizeT{chunk.Length};
        }
        EXPECT_EQ(walked, sizeof(RenderStateParameters));
        EXPECT_EQ(kMGPipePipelineChunkBytes + kMGPipeDynamicChunkBytes, sizeof(RenderStateParameters));
        EXPECT_EQ(kMGPipePipelineChunkBytes, SizeT{396});
        EXPECT_EQ(kMGPipeDynamicChunkBytes, SizeT{772});

        // The two exported arrays are the two halves of that same table, ascending.
        for (SizeT i = 0; i + 1 < kMGPipePipelineChunkCount; ++i) {
            EXPECT_LT(kMGPipePipelineChunks[i].Offset, kMGPipePipelineChunks[i + 1].Offset);
        }
        for (SizeT i = 0; i + 1 < kMGPipeDynamicChunkCount; ++i) {
            EXPECT_LT(kMGPipeDynamicChunks[i].Offset, kMGPipeDynamicChunks[i + 1].Offset);
        }

        // Membership. kMGPipePipelineChunks covers every member kMGPipePipelineStateMembers
        // names and NO byte of any member it does not - which is the half of G7 that the
        // hash cannot check for itself, because a hash over the wrong bytes is still a hash.
        std::set<std::string> pipelineNames;
        for (SizeT i = 0; i < kMGPipePipelineStateMemberCount; ++i) {
            pipelineNames.insert(kMGPipePipelineStateMembers[i]);
        }
        SizeT namedFound = 0;
        for (SizeT i = 0; i < kMemberCount; ++i) {
            const Member& member = kMembers[i];
            const SizeT pipelineBytes = PipelineBytesOf(member.Offset, member.Size);
            if (pipelineNames.count(member.Name) != 0) {
                ++namedFound;
                // WHOLLY pipeline, not merely touched by a pipeline chunk. A named member
                // with only SOME of its bytes in the pipeline half is the silent form of the
                // bug this suite exists to prevent: the demoted bytes drop out of the CSO's
                // content-addressed identity, so one handle serves two different pipeline
                // states and the same cached VkPipeline draws with, say, draw buffer 0's
                // blend enable set both ways. The subset hash cannot see it - a hash over
                // the wrong bytes is still a hash - and the setter walk cannot see it either
                // as long as SOME byte of the member stayed pipeline, because the version
                // and the hash then still move together.
                //
                // StencilStates is the ONE member allowed to straddle, by design and at
                // sub-member granularity; the loop below pins its split face by face.
                if (std::strcmp(member.Name, "StencilStates") == 0) {
                    EXPECT_GT(pipelineBytes, SizeT{0}) << "StencilStates has no pipeline bytes at all";
                    continue;
                }
                EXPECT_TRUE(IsWhollyPipeline(member.Offset, member.Size))
                    << member.Name << " is named as pipeline state but only " << pipelineBytes << " of its "
                    << member.Size << " bytes are in the pipeline half - the rest have silently left the "
                                      "CSO's identity";
            } else {
                EXPECT_EQ(pipelineBytes, SizeT{0})
                    << member.Name << " is not named as pipeline state but " << pipelineBytes
                    << " of its bytes are in the pipeline half";
                EXPECT_TRUE(IsWhollyDynamic(member.Offset, member.Size))
                    << member.Name << " is neither wholly pipeline nor wholly dynamic";
            }
        }
        EXPECT_EQ(namedFound, kMGPipePipelineStateMemberCount)
            << "a name in kMGPipePipelineStateMembers matches no member of RenderStateParameters";

        // StencilStates is the ONE member that straddles, and it straddles at sub-member
        // granularity by design: Ref/ValueMask/WriteMask are VK_DYNAMIC_STATE_STENCIL_*, so
        // glStencilFunc changing only the reference must not evict a cached pipeline.
        // StencilFaceState is deliberately NOT reordered - reordering it would move Espryt's
        // shadow bytes for no gain - so the split is a hole in the middle of each face.
        for (SizeT face = 0; face < 2; ++face) {
            const SizeT base = offsetof(RenderStateParameters, StencilStates) + face * sizeof(StencilFaceState);
            EXPECT_TRUE(IsWhollyDynamic(base + offsetof(StencilFaceState, Ref),
                                        offsetof(StencilFaceState, FailOp) - offsetof(StencilFaceState, Ref)))
                << "stencil face " << face << ": Ref/ValueMask/WriteMask must be dynamic";
            EXPECT_TRUE(IsWhollyPipeline(base + offsetof(StencilFaceState, Func), sizeof(StencilFaceState::Func)))
                << "stencil face " << face << ": Func must be pipeline";
            EXPECT_TRUE(IsWhollyPipeline(base + offsetof(StencilFaceState, FailOp),
                                         sizeof(StencilFaceState) - offsetof(StencilFaceState, FailOp)))
                << "stencil face " << face << ": the three ops must be pipeline";
        }
#endif
    }

    // -------------------------------------------------------------------------------------
    // 2. G7 itself: the setter walk.
    // -------------------------------------------------------------------------------------
    TEST(RenderStateSpans, SetterConsistency) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
#else
        RenderState rs;

        // Drives one setter and asserts the G7 invariant on it. The m_version expectation is
        // the VACUITY GUARD: a setter handed a value equal to the one already stored would
        // satisfy "neither moved" trivially and prove nothing. The one setter that moves no
        // counter at all, SetPixelStoreParam, is driven separately at the end.
        const auto check = [&rs](const char* name, void (*apply)(RenderState&)) {
            const Uint64 hashBefore = MGPipeComputePipelineSubsetHash(rs.GetAllParameters());
            const Uint versionBefore = rs.GetVersion();
            const Uint pipelineBefore = rs.GetPipelineStateVersion();
            apply(rs);
            const Uint64 hashAfter = MGPipeComputePipelineSubsetHash(rs.GetAllParameters());
            EXPECT_NE(versionBefore, rs.GetVersion())
                << name << " wrote a value equal to the one already stored - the case proves nothing";
            EXPECT_EQ(hashBefore != hashAfter, pipelineBefore != rs.GetPipelineStateVersion())
                << name << ": the pipeline-subset hash " << (hashBefore != hashAfter ? "MOVED" : "held")
                << " but m_pipelineStateVersion "
                << (pipelineBefore != rs.GetPipelineStateVersion() ? "MOVED" : "held");
        };

        // ---- Rasterization ----
        check("SetViewport", [](RenderState& s) { s.SetViewport(IntVec4(1, 2, 30, 40)); });
        check("SetViewportIndexed", [](RenderState& s) { s.SetViewportIndexed(3, FloatVec4(4.f, 5.f, 60.f, 70.f)); });
        // EVERY indexed setter is driven at INDEX 0 as well as at a middle index, and index 0
        // is the one that matters most: it is the element both backends actually consume
        // (IsCapabilityEnabled(Blend) is BlendStates[0].Enabled, GetScissorBox/GetViewport
        // answer for rectangle 0) and it is the element a chunk boundary landing at the HEAD
        // of an array demotes first. A walk that only ever touches index 3 cannot tell a
        // boundary that swallowed index 0 from a correct table.
        check("SetViewportIndexed(0)",
              [](RenderState& s) { s.SetViewportIndexed(0, FloatVec4(0.5f, 1.5f, 31.5f, 41.5f)); });
        check("SetLineWidth", [](RenderState& s) { s.SetLineWidth(3.5f); });
        check("SetPointSize", [](RenderState& s) { s.SetPointSize(7.25f); });
        check("SetPatchVertices", [](RenderState& s) { s.SetPatchVertices(4); });
        check("SetPatchDefaultOuterLevel",
              [](RenderState& s) { s.SetPatchDefaultOuterLevel(FloatVec4(2.f, 3.f, 4.f, 5.f)); });
        check("SetPatchDefaultInnerLevel", [](RenderState& s) { s.SetPatchDefaultInnerLevel(FloatVec2(6.f, 7.f)); });
        check("SetPolygonOffset", [](RenderState& s) { s.SetPolygonOffset(1.5f, 2.5f); });
        check("SetPolygonOffsetClamped", [](RenderState& s) { s.SetPolygonOffsetClamped(3.5f, 4.5f, 0.25f); });
        check("SetClipControl", [](RenderState& s) { s.SetClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE); });
        check("SetHint", [](RenderState& s) { s.SetHint(GL_LINE_SMOOTH_HINT, GL_NICEST); });
        check("SetPointFadeThresholdSize", [](RenderState& s) { s.SetPointFadeThresholdSize(2.5f); });
        check("SetPointSpriteCoordOrigin", [](RenderState& s) { s.SetPointSpriteCoordOrigin(GL_LOWER_LEFT); });
        check("SetClampReadColor", [](RenderState& s) { s.SetClampReadColor(GL_TRUE); });
        check("SetPrimitiveRestartIndex", [](RenderState& s) { s.SetPrimitiveRestartIndex(0xabcdu); });

        // SetPolygonMode with ONLY the back face changing, because PolygonModeBack is one of
        // the members P2's subset added over the 24 ComputePipelineStateHash used to hash -
        // if it had stayed out, this is the case that would have caught it.
        rs.SetPolygonMode(GL_LINE, GL_LINE);
        check("SetPolygonMode(back only)", [](RenderState& s) { s.SetPolygonMode(GL_LINE, GL_POINT); });
        check("SetPolygonMode(front only)", [](RenderState& s) { s.SetPolygonMode(GL_FILL, GL_POINT); });

        // ---- Capabilities: every SET_CAPABILITY name, D3's three new ones included ----
#define MGP_CHECK_CAPABILITY(cap)                                                                                      \
    check("SetCapability(" #cap ")", [](RenderState& s) {                                                              \
        s.SetCapability(CapabilityInput::cap, !s.IsCapabilityEnabled(CapabilityInput::cap));                            \
    });
        MGP_CHECK_CAPABILITY(ColorLogicOp)
        MGP_CHECK_CAPABILITY(DebugOutput)
        MGP_CHECK_CAPABILITY(DebugOutputSynchronous)
        MGP_CHECK_CAPABILITY(DepthClamp)
        MGP_CHECK_CAPABILITY(DepthTest)
        MGP_CHECK_CAPABILITY(CullFace)
        MGP_CHECK_CAPABILITY(Dither)
        MGP_CHECK_CAPABILITY(FramebufferSrgb)
        MGP_CHECK_CAPABILITY(LineSmooth)
        MGP_CHECK_CAPABILITY(Multisample)
        MGP_CHECK_CAPABILITY(PolygonOffsetFill)
        MGP_CHECK_CAPABILITY(PolygonOffsetLine)
        MGP_CHECK_CAPABILITY(PolygonOffsetPoint)
        MGP_CHECK_CAPABILITY(PolygonSmooth)
        MGP_CHECK_CAPABILITY(PrimitiveRestart)
        MGP_CHECK_CAPABILITY(PrimitiveRestartFixedIndex)
        MGP_CHECK_CAPABILITY(RasterizerDiscard)
        MGP_CHECK_CAPABILITY(SampleAlphaToCoverage)
        MGP_CHECK_CAPABILITY(SampleAlphaToOne)
        MGP_CHECK_CAPABILITY(SampleCoverage)
        MGP_CHECK_CAPABILITY(SampleMask)
        MGP_CHECK_CAPABILITY(SampleShading)
        MGP_CHECK_CAPABILITY(StencilTest)
        MGP_CHECK_CAPABILITY(TextureCubeMapSeamless)
        MGP_CHECK_CAPABILITY(ProgramPointSize)
        MGP_CHECK_CAPABILITY(Blend)
        MGP_CHECK_CAPABILITY(ScissorTest)
#undef MGP_CHECK_CAPABILITY

        // The eight clip distances are the one family that moves m_version and NOT
        // m_pipelineStateVersion (RenderState.cpp says why: no backend bakes a clip-distance
        // enable into a pipeline object), so the hash must not move either -
        // ClipDistanceEnabledMask lives in dynamic chunk D7.
        for (Uint i = 0; i < 8; ++i) {
            const CapabilityInput cap =
                static_cast<CapabilityInput>(static_cast<Uint>(CapabilityInput::ClipDistance0) + i);
            const Uint64 hashBefore = MGPipeComputePipelineSubsetHash(rs.GetAllParameters());
            const Uint versionBefore = rs.GetVersion();
            const Uint pipelineBefore = rs.GetPipelineStateVersion();
            rs.SetCapability(cap, true);
            EXPECT_NE(versionBefore, rs.GetVersion()) << "ClipDistance" << i << " did not move m_version";
            EXPECT_EQ(pipelineBefore, rs.GetPipelineStateVersion())
                << "ClipDistance" << i << " moved m_pipelineStateVersion";
            EXPECT_EQ(hashBefore, MGPipeComputePipelineSubsetHash(rs.GetAllParameters()))
                << "ClipDistance" << i << " moved the pipeline-subset hash";
        }

        // Both are DISABLES: the non-indexed toggles above have just enabled every draw
        // buffer's blend and every viewport's scissor test, so re-enabling one index would
        // write the value already stored and trip the vacuity guard.
        check("SetCapabilityIndexed(Blend, 3)",
              [](RenderState& s) { s.SetCapabilityIndexed(CapabilityInput::Blend, 3, false); });
        check("SetCapabilityIndexed(ScissorTest, 5)",
              [](RenderState& s) { s.SetCapabilityIndexed(CapabilityInput::ScissorTest, 5, false); });
        // Index 0 of both: BlendStates[0].Enabled is the single bit glEnable(GL_BLEND)
        // answers for and the first four bytes of chunk P1, and ScissorTestEnabledMask bit 0
        // is what DynamicTailKey::scissorEnabled reads.
        check("SetCapabilityIndexed(Blend, 0)",
              [](RenderState& s) { s.SetCapabilityIndexed(CapabilityInput::Blend, 0, false); });
        check("SetCapabilityIndexed(ScissorTest, 0)",
              [](RenderState& s) { s.SetCapabilityIndexed(CapabilityInput::ScissorTest, 0, false); });

        // ---- Blending ----
        check("SetBlendFunc", [](RenderState& s) {
            s.SetBlendFunc(BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendFactor::One, BlendFactor::Zero);
        });
        check("SetBlendFuncIndexed", [](RenderState& s) {
            s.SetBlendFuncIndexed(2, BlendFactor::DstColor, BlendFactor::SrcColor, BlendFactor::DstAlpha,
                                  BlendFactor::SrcAlpha);
        });
        check("SetBlendFuncIndexed(0)", [](RenderState& s) {
            s.SetBlendFuncIndexed(0, BlendFactor::ConstantColor, BlendFactor::ConstantAlpha, BlendFactor::OneMinusDstColor,
                                  BlendFactor::OneMinusDstAlpha);
        });
        check("SetBlendEquation",
              [](RenderState& s) { s.SetBlendEquation(BlendEquation::Subtract, BlendEquation::Min); });
        check("SetBlendEquationIndexed",
              [](RenderState& s) { s.SetBlendEquationIndexed(4, BlendEquation::ReverseSubtract, BlendEquation::Max); });
        check("SetBlendEquationIndexed(0)",
              [](RenderState& s) { s.SetBlendEquationIndexed(0, BlendEquation::Max, BlendEquation::Add); });
        check("SetLogicOp", [](RenderState& s) { s.SetLogicOp(LogicOperation::Xor); });

        // ---- Depth and stencil ----
        check("SetDepthFunc", [](RenderState& s) { s.SetDepthFunc(DepthTestFunc::GreaterEqual); });
        check("SetDepthMask", [](RenderState& s) { s.SetDepthMask(false); });

        // SetStencilFunc TWICE, and this pair is why the case exists. Ref and ValueMask are
        // dynamic (VK_DYNAMIC_STATE_STENCIL_REFERENCE / _COMPARE_MASK) while Func is
        // pipeline, and RenderState.cpp's ++m_pipelineStateVersion is conditional on Func
        // moving - so a reference-only change must move the version and NOT the hash.
        rs.SetStencilFunc(StencilFace::Front, DepthTestFunc::Equal, 1, 0xffu);
        {
            const Uint64 hashBefore = MGPipeComputePipelineSubsetHash(rs.GetAllParameters());
            const Uint versionBefore = rs.GetVersion();
            const Uint pipelineBefore = rs.GetPipelineStateVersion();
            rs.SetStencilFunc(StencilFace::Front, DepthTestFunc::Equal, 7, 0xffu);
            EXPECT_NE(versionBefore, rs.GetVersion()) << "SetStencilFunc(ref only) did not move m_version";
            EXPECT_EQ(pipelineBefore, rs.GetPipelineStateVersion())
                << "SetStencilFunc(ref only) moved m_pipelineStateVersion";
            EXPECT_EQ(hashBefore, MGPipeComputePipelineSubsetHash(rs.GetAllParameters()))
                << "SetStencilFunc(ref only) moved the pipeline-subset hash - Ref is not in the dynamic half";
        }
        check("SetStencilFunc(func)",
              [](RenderState& s) { s.SetStencilFunc(StencilFace::Front, DepthTestFunc::NotEqual, 7, 0xffu); });
        check("SetStencilMask", [](RenderState& s) { s.SetStencilMask(StencilFace::Back, 0x0fu); });
        check("SetStencilOp", [](RenderState& s) {
            s.SetStencilOp(StencilFace::Back, StencilOperation::Replace, StencilOperation::IncrementClamp,
                           StencilOperation::DecrementWrap);
        });

        // ---- Colour mask, clear state, sampling ----
        check("SetColorMask", [](RenderState& s) { s.SetColorMask(BoolVec4(true, false, true, false)); });
        check("SetColorMaskIndexed", [](RenderState& s) { s.SetColorMaskIndexed(6, BoolVec4(false, false, true, true)); });
        check("SetColorMaskIndexed(0)",
              [](RenderState& s) { s.SetColorMaskIndexed(0, BoolVec4(false, true, false, true)); });
        check("SetClearColor", [](RenderState& s) { s.SetClearColor(FloatVec4(0.1f, 0.2f, 0.3f, 0.4f)); });
        check("SetClearDepth", [](RenderState& s) { s.SetClearDepth(0.75f); });
        check("SetClearStencil", [](RenderState& s) { s.SetClearStencil(9); });
        check("SetBlendColor", [](RenderState& s) { s.SetBlendColor(FloatVec4(0.5f, 0.6f, 0.7f, 0.8f)); });
        check("SetDepthRange", [](RenderState& s) { s.SetDepthRange(FloatVec2(0.25f, 0.75f)); });
        check("SetDepthRangeIndexed", [](RenderState& s) { s.SetDepthRangeIndexed(9, FloatVec2(0.1f, 0.9f)); });
        check("SetDepthRangeIndexed(0)", [](RenderState& s) { s.SetDepthRangeIndexed(0, FloatVec2(0.3f, 0.6f)); });
        // SetSampleCoverage calls BumpVersions(), so under the rule it is PIPELINE state -
        // which is why MGPipeTypes.h's MGPDynamicState comment no longer claims otherwise.
        check("SetSampleCoverage", [](RenderState& s) { s.SetSampleCoverage(0.375f, true); });
        check("SetSampleMaskValue", [](RenderState& s) { s.SetSampleMaskValue(0x5a5au); });
        check("SetMinSampleShadingValue", [](RenderState& s) { s.SetMinSampleShadingValue(0.625f); });

        // ---- Faces and scissor ----
        check("SetCullFaceMode", [](RenderState& s) { s.SetCullFaceMode(CullFaceMode::Front); });
        check("SetFrontFaceMode", [](RenderState& s) { s.SetFrontFaceMode(FrontFaceMode::Clockwise); });
        check("SetProvokingVertexMode",
              [](RenderState& s) { s.SetProvokingVertexMode(ProvokingVertexMode::FirstVertex); });
        // The first glScissor on a context both writes the rectangles and flips the
        // "never written" bit, so the transition and the steady state are separate cases.
        check("SetScissorBox(first write)", [](RenderState& s) { s.SetScissorBox(IntVec4(1, 2, 3, 4)); });
        check("SetScissorBox(again)", [](RenderState& s) { s.SetScissorBox(IntVec4(5, 6, 7, 8)); });
        check("SetScissorBoxIndexed", [](RenderState& s) { s.SetScissorBoxIndexed(11, IntVec4(9, 10, 11, 12)); });
        check("SetScissorBoxIndexed(0)", [](RenderState& s) { s.SetScissorBoxIndexed(0, IntVec4(13, 14, 15, 16)); });

        // SetPixelStoreParam moves NEITHER counter and touches no byte of
        // RenderStateParameters: the pixel store lives in its own two structs and travels as
        // set_pixel_pack_state. Driven here so the claim is tested rather than assumed.
        {
            const Uint64 hashBefore = MGPipeComputePipelineSubsetHash(rs.GetAllParameters());
            const Uint versionBefore = rs.GetVersion();
            const Uint pipelineBefore = rs.GetPipelineStateVersion();
            rs.SetPixelStoreParam(PixelStoreParam::PackAlignment, 8);
            EXPECT_EQ(rs.GetPixelStoreParam(PixelStoreParam::PackAlignment), 8);
            EXPECT_EQ(versionBefore, rs.GetVersion());
            EXPECT_EQ(pipelineBefore, rs.GetPipelineStateVersion());
            EXPECT_EQ(hashBefore, MGPipeComputePipelineSubsetHash(rs.GetAllParameters()));
        }
#endif
    }

    // -------------------------------------------------------------------------------------
    // 3. D5's derivations against the getters they were transcribed from.
    // -------------------------------------------------------------------------------------
    TEST(RenderStateSpans, DerivationMatchesTheFrontendGetters) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
#else
        // A live frontend context for the setters to write and the getters to answer from,
        // restored on the way out so the case stays independent (SanityTest's idiom).
        struct ContextGuard {
            UniquePtr<GLContext> Previous;
            ContextGuard() : Previous(Move(MG_State::pGLContext)) {
                MG_State::pGLContext = MakeUnique<GLContext>();
                MGPipeApplierReset();
            }
            ~ContextGuard() {
                MGPipeApplierReset();
                MG_State::pGLContext = Move(Previous);
            }
        } guard;
        GLContext& ctx = *MG_State::pGLContext;

        // Assembles the working block the way the tracker will: one brand-new CSO carrying
        // every pipeline chunk, its bind, then every dynamic chunk. A fresh slot per call,
        // so a later phase cannot be answered by an earlier phase's record.
        Uint32 nextSlot = kMGPipeFirstAllocatableSlot;
        const auto applyWholeBlock = [&ctx, &nextSlot]() {
            const RenderStateParameters& live = ctx.GetRenderStateParameters();
            const Uint32 allPipeline = static_cast<Uint32>((Uint64{1} << kMGPipePipelineChunkCount) - 1);
            const Uint32 allDynamic = static_cast<Uint32>((Uint64{1} << kMGPipeDynamicChunkCount) - 1);

            Array<Uint8, kMGPipePipelineChunkBytes> pipelineBytes{};
            MGPipeGatherPipelineBytes(live, pipelineBytes.data());
            MGPRenderStateDesc desc{};
            desc.Cso = MGPipeHandle{nextSlot++, 0};
            desc.BaseCso = kMGPipeNullHandle;
            desc.ChunkMask = allPipeline;
            MGPipeApplyCreateRenderState(desc, pipelineBytes.data());

            MGPBindRenderState bind{};
            bind.Cso = desc.Cso;
            bind.Version = static_cast<Uint16>(ctx.GetRenderStateParametersVersion());
            bind.PipelineVersion = static_cast<Uint16>(ctx.GetPipelineStateVersion());
            MGPipeApplyBindRenderState(bind);

            Vector<Uint8> dynamicBytes(MGPipeDynamicChunkBlobBytes(allDynamic));
            MGPipeGatherDynamicChunks(live, allDynamic, dynamicBytes.data());
            MGPDynamicState dyn{};
            dyn.ChunkMask = allDynamic;
            dyn.Version = bind.Version;
            MGPipeApplySetDynamicState(dyn, dynamicBytes.data());
        };

        // THREE PHASES, one per verb class, because the poison is right and the fill table is
        // the only thing that says what a verb may read: 25 of these fields are kDraw's, the
        // three clear values are kClear's and GetClampReadColor is kReadback's
        // (MG_Pipe/FillPoints.def). Reading a clear value under DrawArrays would be
        // Fatal{UnmigratedPipeInput}, and rightly - a draw does not read it.
        //
        // Phase 1, kDraw. The fill happens FIRST, against the DEFAULT state; every mutation
        // below is driven afterwards, so a field that still agrees with the context at the
        // end can only have got there through the applier's derivation.
        {
        MG_Test::ScopedPipeVerb verb(MGPipeVerb::DrawArrays);

        ctx.SetViewportIndexed(0, FloatVec4(1.5f, 2.5f, 63.5f, 32.25f));
        ctx.SetViewportIndexed(7, FloatVec4(8.f, 9.f, 10.f, 11.f));
        ctx.SetLineWidth(3.5f);
        ctx.SetPatchVertices(4);
        ctx.SetPatchDefaultOuterLevel(FloatVec4(2.f, 3.f, 4.f, 5.f));
        ctx.SetPatchDefaultInnerLevel(FloatVec2(6.f, 7.f));
        ctx.SetPolygonOffsetClamped(1.5f, 2.5f, 0.25f);
        ctx.SetClampReadColor(GL_TRUE);
        ctx.SetPolygonMode(GL_LINE, GL_POINT);
        ctx.SetPrimitiveRestartIndex(0xabcdu);
        ctx.SetBlendFuncIndexed(2, BlendFactor::DstColor, BlendFactor::SrcColor, BlendFactor::DstAlpha,
                                BlendFactor::SrcAlpha);
        ctx.SetBlendEquationIndexed(4, BlendEquation::ReverseSubtract, BlendEquation::Max);
        ctx.SetCapabilityIndexed(CapabilityInput::Blend, 3, true);
        ctx.SetCapabilityIndexed(CapabilityInput::ScissorTest, 5, true);
        ctx.SetLogicOp(LogicOperation::Xor);
        ctx.SetDepthFunc(DepthTestFunc::GreaterEqual);
        ctx.SetDepthMask(false);
        ctx.SetStencilFunc(StencilFace::Front, DepthTestFunc::Equal, 7, 0xf0u);
        ctx.SetStencilOp(StencilFace::Back, StencilOperation::Replace, StencilOperation::IncrementClamp,
                         StencilOperation::DecrementWrap);
        ctx.SetStencilMask(StencilFace::Back, 0x0fu);
        ctx.SetColorMaskIndexed(6, BoolVec4(false, false, true, true));
        ctx.SetClearColor(FloatVec4(0.1f, 0.2f, 0.3f, 0.4f));
        ctx.SetClearDepth(0.75f);
        ctx.SetClearStencil(9);
        ctx.SetBlendColor(FloatVec4(0.5f, 0.6f, 0.7f, 0.8f));
        ctx.SetDepthRangeIndexed(9, FloatVec2(0.1f, 0.9f));
        ctx.SetMinSampleShadingValue(0.625f);
        ctx.SetCullFaceMode(CullFaceMode::Front);
        ctx.SetProvokingVertexMode(ProvokingVertexMode::FirstVertex);
        ctx.SetScissorBox(IntVec4(1, 2, 3, 4));
        // One capability out of every arm of the 35-way switch: plain bools (the three D3
        // gave storage to among them), the two indexed ones through their non-indexed entry
        // point, and a clip distance.
        ctx.SetCapability(CapabilityInput::DepthTest, true);
        ctx.SetCapability(CapabilityInput::FramebufferSrgb, true);
        ctx.SetCapability(CapabilityInput::DepthClamp, true);
        ctx.SetCapability(CapabilityInput::TextureCubeMapSeamless, true);
        ctx.SetCapability(CapabilityInput::Blend, true);
        ctx.SetCapability(CapabilityInput::ClipDistance3, true);

        // The vacuity guard: the block still holds what the fill copied out of the DEFAULT
        // context, so it must currently DISAGREE with the live one. If this ever passes, the
        // comparisons below would be checking the filler against itself.
        ASSERT_NE(gPipeInputs.GetLineWidth(), ctx.GetLineWidth());

        applyWholeBlock();

        // ---- the 25 kDraw fields ----
        ExpectDerivedDrawFieldsMatch(ctx, "whole block, kDraw");
        // ...and the ~25 members that have NO derived field, which only a byte compare of the
        // whole block reaches.
        ExpectAssembledBlockIsTheLiveBlock(ctx, "whole block, kDraw");
        // The rounding half of GetViewport, exercised on purpose: viewport 0 is
        // (1.5, 2.5, 63.5, 32.25), so a transcription that truncated instead of rounding
        // would hand the backends a 63-wide rectangle where 64 was asked for. The literal
        // is std::lround's answer - round half AWAY FROM ZERO, so 1.5 -> 2 and 2.5 -> 3,
        // not the banker's rounding a nearbyint() transcription would give.
        EXPECT_EQ(gPipeInputs.GetViewport(), IntVec4(2, 3, 64, 32));
        } // phase 1, kDraw

        // Phase 2, kClear: the three clear values. The verb's own fill runs FIRST and copies
        // what phase 1 left in the context, so the values are changed AGAIN afterwards - the
        // block therefore disagrees before the apply and can only be made to agree by it.
        {
            MG_Test::ScopedPipeVerb verb(MGPipeVerb::Clear);
            ctx.SetClearColor(FloatVec4(0.9f, 0.8f, 0.7f, 0.6f));
            ctx.SetClearDepth(0.125f);
            ctx.SetClearStencil(21);
            ASSERT_NE(gPipeInputs.GetClearDepth(), ctx.GetClearDepth());

            applyWholeBlock();

            EXPECT_EQ(gPipeInputs.GetClearColor(), ctx.GetClearColor());
            EXPECT_EQ(gPipeInputs.GetClearDepth(), ctx.GetClearDepth());
            EXPECT_EQ(gPipeInputs.GetClearStencil(), ctx.GetClearStencil());
        }

        // Phase 3, kReadback: GetClampReadColor, the one derived field no draw and no clear
        // may read at all (FillPoints.def gives it to kReadback alone). Same shape.
        {
            MG_Test::ScopedPipeVerb verb(MGPipeVerb::ReadPixels);
            ctx.SetClampReadColor(GL_FALSE);
            ASSERT_NE(gPipeInputs.GetClampReadColor(), ctx.GetClampReadColor());

            applyWholeBlock();

            EXPECT_EQ(gPipeInputs.GetClampReadColor(), ctx.GetClampReadColor());
            EXPECT_EQ(gPipeInputs.GetClampReadColor(), static_cast<GLenum>(GL_FALSE));
        }
#endif
    }

    // -------------------------------------------------------------------------------------
    // 4. Magma's DynamicTailKey against the dynamic half.
    // -------------------------------------------------------------------------------------
    TEST(RenderStateSpans, DynamicChunksCoverMagmasDynamicTailKey) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
#else
        // The complete GL-state input inventory of ApplyDynamicDrawStateTail, transcribed
        // from the comment above `struct DynamicTailKey`
        // (MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp). extentX/extentY/
        // preTransform/isDefaultFbo are backend facts, not GL state, and are not here.
        struct Input {
            const char* Name;
            SizeT Offset;
            SizeT Size;
        };
        const SizeT stencil0 = offsetof(RenderStateParameters, StencilStates);
        const SizeT stencil1 = stencil0 + sizeof(StencilFaceState);
        const Input inputs[] = {
            {"Viewports[0]", offsetof(RenderStateParameters, Viewports), sizeof(FloatVec4)},
            {"DepthRanges[0]", offsetof(RenderStateParameters, DepthRanges), sizeof(FloatVec2)},
            {"BlendColor", offsetof(RenderStateParameters, BlendColor), sizeof(FloatVec4)},
            {"PolygonOffsetFactor", offsetof(RenderStateParameters, PolygonOffsetFactor), sizeof(Float)},
            {"PolygonOffsetUnits", offsetof(RenderStateParameters, PolygonOffsetUnits), sizeof(Float)},
            {"LineWidth", offsetof(RenderStateParameters, LineWidth), sizeof(Float)},
            {"StencilStates[0].Ref", stencil0 + offsetof(StencilFaceState, Ref), sizeof(Int)},
            {"StencilStates[0].ValueMask", stencil0 + offsetof(StencilFaceState, ValueMask), sizeof(Uint32)},
            {"StencilStates[0].WriteMask", stencil0 + offsetof(StencilFaceState, WriteMask), sizeof(Uint32)},
            {"StencilStates[1].Ref", stencil1 + offsetof(StencilFaceState, Ref), sizeof(Int)},
            {"StencilStates[1].ValueMask", stencil1 + offsetof(StencilFaceState, ValueMask), sizeof(Uint32)},
            {"StencilStates[1].WriteMask", stencil1 + offsetof(StencilFaceState, WriteMask), sizeof(Uint32)},
            {"ScissorBoxes[0]", offsetof(RenderStateParameters, ScissorBoxes), sizeof(IntVec4)},
        };
        for (const Input& input : inputs) {
            EXPECT_TRUE(IsWhollyDynamic(input.Offset, input.Size))
                << input.Name << " is read by DynamicTailKey but is not inside kMGPipeDynamicChunks";
        }

        // THE ONE EXCEPTION, and it is a fact about the tree rather than an oversight in it.
        // DynamicTailKey's `scissorEnabled` reads ScissorTestEnabledMask bit 0, and that
        // member is PIPELINE state under P2's rule, because SetCapability(ScissorTest) and
        // SetCapabilityIndexed(ScissorTest, i) both call BumpVersions(). It is harmless:
        // BumpVersions() moves m_version too, so MGPDynamicState::Version - the value
        // ApplyDynamicDrawStateTail's own gate reads - still moves on a scissor-enable
        // change and the tail still re-runs. Asserted the other way round, so a later table
        // edit that quietly demotes the mask is loud here rather than silent.
        EXPECT_FALSE(IsWhollyDynamic(offsetof(RenderStateParameters, ScissorTestEnabledMask),
                                     sizeof(RenderStateParameters::ScissorTestEnabledMask)))
            << "ScissorTestEnabledMask moved into the dynamic half; ApplyDynamicDrawStateTail's "
               "scissorEnabled input and this expectation both need re-reading";
        EXPECT_TRUE(IsWhollyPipeline(offsetof(RenderStateParameters, ScissorTestEnabledMask),
                                     sizeof(RenderStateParameters::ScissorTestEnabledMask)));
#endif
    }

    // -------------------------------------------------------------------------------------
    // 5. The INCREMENTAL path, which is the shape the tracker actually emits: a
    //    create_render_state naming only the pipeline chunks that moved against a BaseCso
    //    (D7 step 2's miss path), and a set_dynamic_state naming only the dynamic chunks that
    //    moved (D8's chunk-level suppressor). Nothing but this case enters
    //    MGPipeApplyCreateRenderState's base-inherit branch, and nothing but this case drives
    //    the applier's CHUNK-SCOPED derivation - a whole-block apply asks for every chunk and
    //    so cannot tell a correctly scoped guard from one that is too narrow.
    // -------------------------------------------------------------------------------------
    TEST(RenderStateSpans, IncrementalChunksKeepEveryDerivedFieldInStep) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
#else
        struct ContextGuard {
            UniquePtr<GLContext> Previous;
            ContextGuard() : Previous(Move(MG_State::pGLContext)) {
                MG_State::pGLContext = MakeUnique<GLContext>();
                MGPipeApplierReset();
            }
            ~ContextGuard() {
                MGPipeApplierReset();
                MG_State::pGLContext = Move(Previous);
            }
        } guard;
        GLContext& ctx = *MG_State::pGLContext;

        // Every read below is a kDraw fill point, so one verb covers the whole case. The fill
        // runs at construction against the DEFAULT context, which is what makes the first
        // step's comparison meaningful rather than a comparison of the filler with itself.
        MG_Test::ScopedPipeVerb verb(MGPipeVerb::DrawArrays);

        // `staged` is the tracker's "what the server has" mirror (D8). The masks are computed
        // from it exactly as the tracker will compute them.
        RenderStateParameters staged{};
        MGPipeHandle previousCso = kMGPipeNullHandle;
        Uint32 nextSlot = kMGPipeFirstAllocatableSlot;
        Bool firstPush = true;

        const auto push = [&](const char* tag) {
            const RenderStateParameters& live = ctx.GetRenderStateParameters();
            const Uint32 movedPipeline =
                firstPush ? static_cast<Uint32>((Uint64{1} << kMGPipePipelineChunkCount) - 1)
                          : MGPipePipelineChunksThatMoved(staged, live);
            const Uint32 movedDynamic =
                firstPush ? static_cast<Uint32>((Uint64{1} << kMGPipeDynamicChunkCount) - 1)
                          : MGPipeDynamicChunksThatMoved(staged, live);

            if (movedPipeline != 0) {
                Vector<Uint8> blob(MGPipePipelineChunkBlobBytes(movedPipeline));
                MGPipeGatherPipelineChunks(live, movedPipeline, blob.data());
                MGPRenderStateDesc desc{};
                desc.Cso = MGPipeHandle{nextSlot++, 0};
                // The base-inherit branch: everything this desc does NOT name has to come
                // from the record the client is pointing at.
                desc.BaseCso = previousCso;
                desc.ChunkMask = movedPipeline;
                MGPipeApplyCreateRenderState(desc, blob.data());

                MGPBindRenderState bind{};
                bind.Cso = desc.Cso;
                bind.Version = static_cast<Uint16>(ctx.GetRenderStateParametersVersion());
                bind.PipelineVersion = static_cast<Uint16>(ctx.GetPipelineStateVersion());
                MGPipeApplyBindRenderState(bind);
                previousCso = desc.Cso;

                // The reconstructed record must be the WHOLE pipeline half of the live block,
                // byte for byte: an incremental create that inherited the wrong chunk would
                // otherwise only show up as a wrong pixel much later, on the first bind that
                // scatters it. This is also MGPipeHashPipelineBytes' only caller - and its
                // agreement with the from-scratch hash is what lets CsoCache hash the bytes
                // it already holds instead of re-gathering them.
                Array<Uint8, kMGPipePipelineChunkBytes> gathered{};
                MGPipeGatherPipelineBytes(live, gathered.data());
                const MGPipeRenderStateCsoRecord& record = MGPipeApplier().RenderStateCsos[desc.Cso.Slot];
                EXPECT_EQ(std::memcmp(record.PipelineBytes.data(), gathered.data(), kMGPipePipelineChunkBytes), 0)
                    << tag << ": the incrementally created CSO is not the live pipeline half";
                EXPECT_EQ(MGPipeHashPipelineBytes(gathered.data()), MGPipeComputePipelineSubsetHash(live))
                    << tag << ": hashing the gathered bytes disagrees with hashing the block";
            }

            if (movedDynamic != 0) {
                Vector<Uint8> blob(MGPipeDynamicChunkBlobBytes(movedDynamic));
                MGPipeGatherDynamicChunks(live, movedDynamic, blob.data());
                MGPDynamicState dyn{};
                dyn.ChunkMask = movedDynamic;
                dyn.Version = static_cast<Uint16>(ctx.GetRenderStateParametersVersion());
                MGPipeApplySetDynamicState(dyn, blob.data());
            }

            staged = live;
            firstPush = false;
            // Every derived field, after a scatter that named only the chunks that moved. A
            // derivation guard that is too narrow leaves the previous step's value standing
            // and this is where it shows.
            ExpectDerivedDrawFieldsMatch(ctx, tag);
            ExpectAssembledBlockIsTheLiveBlock(ctx, tag);
        };

        // Step 0: the whole block, so every later step is a genuine delta.
        ctx.SetViewportIndexed(0, FloatVec4(1.5f, 2.5f, 63.5f, 32.25f));
        push("step 0: the whole block");

        // One step per derivation guard, each moving as few chunks as the setter allows.
        ctx.SetViewportIndexed(0, FloatVec4(4.f, 5.f, 60.f, 70.f));
        ctx.SetViewportIndexed(7, FloatVec4(8.f, 9.f, 10.f, 11.f));
        push("step 1: viewports only (dynamic chunk D0)");

        ctx.SetDepthRangeIndexed(3, FloatVec2(0.2f, 0.8f));
        push("step 2: depth ranges only (dynamic chunk D2)");

        ctx.SetBlendFuncIndexed(0, BlendFactor::DstColor, BlendFactor::SrcColor, BlendFactor::DstAlpha,
                                BlendFactor::SrcAlpha);
        ctx.SetBlendEquationIndexed(5, BlendEquation::ReverseSubtract, BlendEquation::Max);
        ctx.SetColorMaskIndexed(0, BoolVec4(false, true, false, true));
        push("step 3: blend and colour mask (pipeline chunk P1)");

        ctx.SetCapabilityIndexed(CapabilityInput::Blend, 0, true);
        push("step 4: indexed blend enable at index 0");

        ctx.SetCapabilityIndexed(CapabilityInput::ScissorTest, 2, true);
        push("step 5: indexed scissor enable (pipeline chunk P6)");

        ctx.SetScissorBox(IntVec4(3, 4, 5, 6));
        push("step 6: scissor rectangles (dynamic chunk D7)");

        ctx.SetStencilFunc(StencilFace::Front, DepthTestFunc::Equal, 7, 0xf0u);
        ctx.SetStencilOp(StencilFace::Back, StencilOperation::Replace, StencilOperation::IncrementClamp,
                         StencilOperation::DecrementWrap);
        ctx.SetStencilMask(StencilFace::Back, 0x0fu);
        push("step 7: the stencil faces, which straddle four chunks");

        ctx.SetLineWidth(3.5f);
        ctx.SetPolygonOffsetClamped(1.5f, 2.5f, 0.25f);
        ctx.SetLogicOp(LogicOperation::Xor);
        ctx.SetDepthFunc(DepthTestFunc::GreaterEqual);
        ctx.SetDepthMask(false);
        ctx.SetCullFaceMode(CullFaceMode::Front);
        ctx.SetProvokingVertexMode(ProvokingVertexMode::FirstVertex);
        ctx.SetPrimitiveRestartIndex(0xabcdu);
        ctx.SetMinSampleShadingValue(0.625f);
        ctx.SetPatchVertices(4);
        ctx.SetPatchDefaultOuterLevel(FloatVec4(2.f, 3.f, 4.f, 5.f));
        ctx.SetPatchDefaultInnerLevel(FloatVec2(6.f, 7.f));
        ctx.SetPolygonMode(GL_LINE, GL_POINT);
        push("step 8: the unguarded scalars");

        // EVERY capability, one at a time. This is what pins the capability walk's guard
        // exhaustively: the 25 plain bools sit in three different pipeline chunks, Blend is
        // BlendStates[0].Enabled, ScissorTest is a pipeline mask and the eight ClipDistances
        // are a DYNAMIC mask - so a guard that named only "the capability chunk" would leave
        // one of those families stale, and the flip that reaches it fails here by name.
        for (SizeT i = 0; i < static_cast<SizeT>(CapabilityInput::CapabilityInputCount); ++i) {
            const CapabilityInput cap = static_cast<CapabilityInput>(i);
            ctx.SetCapability(cap, !ctx.IsCapabilityEnabled(cap));
            push(("step 9: capability " + std::to_string(i)).c_str());
        }
#endif
    }
} // namespace
