// MobileGL - MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The arithmetic of the MGPipe catalogue (plan B section 4.4, appendix A). Everything here
// is cheap on purpose: it is the test that fails when PipeCalls.def and the seven generated
// files stop agreeing, and it must not need a GL context to say so.

#include <gtest/gtest.h>

#include <cstring>
#include <limits>

#include "Includes.h"
#include <MG_Pipe/MGPipe.h>

using namespace MobileGL;
using namespace MobileGL::MG_Pipe;

namespace {
    // Counting expansions of the catalogue. The Class parameter is a real enumerator, so a
    // per-class count is a constant expression too.
#define MGP_COUNT_ONE(Name, Payload, Class, Flags) +1
#define MGP_COUNT_CLASS(Name, Payload, Class, Flags) +((Class) == countedClass ? 1 : 0)

    constexpr SizeT kExpandedCallCount = 0 MGP_CALL_LIST(MGP_COUNT_ONE);

    template <MGPipeCallClass countedClass>
    constexpr SizeT ClassCount() {
        return 0 MGP_CALL_LIST(MGP_COUNT_CLASS);
    }

    // Every payload named in the catalogue must be a memcpy-able POD, and so must every
    // payload the verify comparator knows about.
#define MGP_ASSERT_CALL_PAYLOAD_POD(Name, Payload, Class, Flags)                                                       \
    static_assert(std::is_trivially_copyable_v<Payload>, #Name "'s payload " #Payload " is not trivially copyable");
    MGP_CALL_LIST(MGP_ASSERT_CALL_PAYLOAD_POD)

#define MGP_ASSERT_VERIFY_PAYLOAD_POD(Payload)                                                                         \
    static_assert(std::is_trivially_copyable_v<Payload>, #Payload " is not trivially copyable");
    MGP_VERIFY_PAYLOAD_LIST(MGP_ASSERT_VERIFY_PAYLOAD_POD)
} // namespace

// The handle is the whole object model. Eight bytes, a register pair, no padding.
TEST(PipeCatalogue, HandleIsEightBytes) {
    static_assert(sizeof(MGPipeHandle) == 8);
    static_assert(alignof(MGPipeHandle) == 4);
    static_assert(std::is_trivially_copyable_v<MGPipeHandle>);
    EXPECT_EQ(sizeof(MGPipeHandle), 8u);

    // The two reserved handles, and the composite band that the program-pipeline resolver
    // allocates out of.
    EXPECT_TRUE(MGPipeHandleIsNull(kMGPipeNullHandle));
    EXPECT_FALSE(MGPipeHandleIsNull(kMGPipeDefaultFramebuffer));
    EXPECT_FALSE(MGPipeIsCompositeShaderSlot(kMGPipeFirstAllocatableSlot));
    EXPECT_TRUE(MGPipeIsCompositeShaderSlot(kMGPipeShaderCsoCompositeSlotBase));
    EXPECT_FALSE(MGPipeIsCompositeShaderSlot(kMGPipeShaderCsoSlotLimit));
}

// The catalogue, the number documented in its header, and the two generated tables are one
// fact stated three times. This is the test that notices when they stop being.
TEST(PipeCatalogue, EntryCountMatchesTheDocumentedCount) {
    static_assert(kExpandedCallCount == MGP_CALL_LIST_DOCUMENTED_COUNT);
    static_assert(kExpandedCallCount == kMGPipeCallCount);
    EXPECT_EQ(kExpandedCallCount, static_cast<SizeT>(MGP_CALL_LIST_DOCUMENTED_COUNT));
    EXPECT_EQ(kMGPipeCallCount, kExpandedCallCount);
}

TEST(PipeCatalogue, GeneratedTablesHoldTheWholeCatalogue) {
    static_assert(ClassCount<kScreen>() == kMGPipeScreenCallCount);
    static_assert(ClassCount<kScreen>() + ClassCount<kCtxCso>() + ClassCount<kCtxState>() +
                      ClassCount<kCtxObject>() + ClassCount<kCtxVerb>() + ClassCount<kCtxQuery>() ==
                  kMGPipeCallCount);
    // The tables ARE their function pointers: a struct that is bigger than its call count
    // has grown a member no generator knows about.
    static_assert(sizeof(MGPipeScreen) == kMGPipeScreenCallCount * sizeof(void (*)()));
    static_assert(sizeof(MGPipeContext) == kMGPipeContextCallCount * sizeof(void (*)()));

    EXPECT_EQ(kMGPipeScreenCallCount, ClassCount<kScreen>());
    EXPECT_EQ(kMGPipeContextCallCount, kMGPipeCallCount - ClassCount<kScreen>());

    // The per-class counts PipeCalls.def documents in its header.
    EXPECT_EQ(ClassCount<kScreen>(), 11u);
    EXPECT_EQ(ClassCount<kCtxQuery>(), 8u);
    EXPECT_EQ(ClassCount<kCtxCso>(), 13u);
    EXPECT_EQ(ClassCount<kCtxState>(), 17u);
    EXPECT_EQ(ClassCount<kCtxObject>(), 9u);
    EXPECT_EQ(ClassCount<kCtxVerb>(), 13u);
}

// An uninstalled pipe is every entry null - which is exactly what "this subsystem has not
// been migrated, keep pulling" means (plan B section 4.1).
TEST(PipeCatalogue, UninstalledTablesAreAllNull) {
    const void* const* screen = reinterpret_cast<const void* const*>(&gMGPipeScreen);
    for (SizeT i = 0; i < kMGPipeScreenCallCount; ++i) {
        EXPECT_EQ(screen[i], nullptr) << "screen entry " << i;
    }
    const void* const* context = reinterpret_cast<const void* const*>(&gMGPipeContext);
    for (SizeT i = 0; i < kMGPipeContextCallCount; ++i) {
        EXPECT_EQ(context[i], nullptr) << "context entry " << i;
    }
}

// The retirement ratchet of the migration carrier (section 6.3): the constant and the
// struct must agree, and the constant only ever goes down.
TEST(PipeCatalogue, ResidualBlockSizeIsPinned) {
    static_assert(sizeof(ResidualValueBlock) == MGL_RESIDUAL_BLOCK_SIZE);
    EXPECT_EQ(sizeof(ResidualValueBlock), static_cast<SizeT>(MGL_RESIDUAL_BLOCK_SIZE));
    // It carries the whole of both value structs today; that is what the later stages eat.
    EXPECT_GE(sizeof(ResidualValueBlock), sizeof(RenderStateParameters) + sizeof(PixelStoreParameters));
}

// P0.5 moved the value structs into MG_Pipe/MGPipeValueTypes.h. These are the runtime twins
// of that header's static assertions, so the numbers show up in ctest output on every
// platform - including one where a static assertion is skipped. Every number here is also
// what MGL_RESIDUAL_BLOCK_SIZE (MGPipeTypes.h) and the Espryt offsetof spans depend on.
TEST(PipeCatalogue, ValueTypeLayoutsArePinned) {
    EXPECT_EQ(sizeof(PixelStoreParameters), 28u);
    EXPECT_EQ(sizeof(PerBufferBlendState), 28u);
    EXPECT_EQ(sizeof(StencilFaceState), 28u);
    EXPECT_EQ(sizeof(RenderStateParameters), 1168u);
    EXPECT_EQ(sizeof(SamplerParameters), 100u);
    EXPECT_EQ(sizeof(MG_State::GLState::VertexAttributeVersion), 6u);
    EXPECT_TRUE(std::is_trivially_copyable_v<PixelStoreParameters>);
    EXPECT_TRUE(std::is_trivially_copyable_v<PerBufferBlendState>);
    EXPECT_TRUE(std::is_trivially_copyable_v<StencilFaceState>);
    EXPECT_TRUE(std::is_trivially_copyable_v<RenderStateParameters>);
    EXPECT_TRUE(std::is_standard_layout_v<RenderStateParameters>);
    EXPECT_TRUE(std::is_trivially_copyable_v<SamplerParameters>);
    EXPECT_TRUE(std::is_trivially_copyable_v<MG_State::GLState::VertexAttributeVersion>);
    EXPECT_LT(offsetof(RenderStateParameters, BlendStates), offsetof(RenderStateParameters, LogicOp));
    EXPECT_EQ(std::tuple_size_v<decltype(RenderStateParameters::BlendStates)>, static_cast<SizeT>(kMGMaxDrawBuffers));
    EXPECT_EQ(std::tuple_size_v<decltype(RenderStateParameters::ColorMasks)>, static_cast<SizeT>(kMGMaxDrawBuffers));
    EXPECT_EQ(kMGMaxDrawBuffers, 8u);
}

// The move did not alter the carrier: the residual block is still the render-state struct,
// then the pack struct, then the 8-aligned capability word, at the offsets it had before.
TEST(PipeCatalogue, ResidualBlockIsExactlyItsTwoValueStructsPlusPatchTail) {
    EXPECT_EQ(offsetof(ResidualValueBlock, RenderState), 0u);
    EXPECT_EQ(offsetof(ResidualValueBlock, Pack), sizeof(RenderStateParameters));
    EXPECT_EQ(offsetof(ResidualValueBlock, CapabilityBits), 1200u);
    EXPECT_EQ(offsetof(ResidualValueBlock, PatchVertices), 1208u);
}

// G3's opcode numbering is the wire protocol. Position in PipeCalls.def, 1-based, no holes.
TEST(PipeCatalogue, WireOpcodesAreThePositionsInTheCatalogue) {
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::GetCaps), 1);
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::kOpCount), kMGPipeCallCount + 1);
    EXPECT_EQ(sizeof(MGPWireRecHeader), 8u);
    // Every record is a multiple of the stream's 8-byte granularity, which is half of the
    // applier's precondition.
    EXPECT_EQ(sizeof(MGPWireRec_DrawVbo) % 8, 0u);
    EXPECT_EQ(sizeof(MGPWireRec_BindRenderState) % 8, 0u);
    EXPECT_EQ(sizeof(MGPWireRec_SetResidualValueState) % 8, 0u);
}

// Records are append-only. The three carriers added after the first cut - for the live
// GLFunctionsTable entries GetGpuTimestampNs, QueryCounterTimestamp and WaitSync - sit at
// the END of the list, after SetSwapInterval, so no opcode the first cut assigned has moved.
TEST(PipeCatalogue, LateArrivalsAreAppendedWithoutRenumbering) {
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::SetSwapInterval), 68);
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::QueryTimestamp), 69);
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::QueryCounter), 70);
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::FenceWaitServer), 71);
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::kOpCount), 72);
}

// A well-formed record passes the applier's bounds gate. P0 has no applier, so "accepted"
// is reported as "not applied" rather than "fatal".
TEST(PipeCatalogue, ApplierAcceptsAWellFormedRecord) {
    MGPWireRec_Present record{};
    record.Header.Op = static_cast<Uint16>(MGPWireOp::Present);
    record.Header.Size = sizeof(record);
    record.Payload.FrameSerial = 42;
    EXPECT_FALSE(MGPipeApplyWireRecord(MGPWireOp::Present, &record, sizeof(record), sizeof(record)));
}

// G4 reports the FIRST differing field by name, and compares field by field so that
// padding cannot produce a difference that does not exist.
TEST(PipeCatalogue, VerifyComparatorNamesTheDifferingField) {
    MGPDrawInfo a{};
    MGPDrawInfo b{};
    const char* field = nullptr;
    EXPECT_TRUE(MGPipeVerify(a, b, &field));

    b.InstanceCount = 7;
    EXPECT_FALSE(MGPipeVerify(a, b, &field));
    EXPECT_STREQ(field, "InstanceCount");

    // Padding bytes are not fields: writing to them cannot make two payloads differ.
    MGPBindRenderState c{};
    MGPBindRenderState d{};
    c.Cso = MGPipeHandle{3, 1};
    d.Cso = MGPipeHandle{3, 1};
    field = nullptr;
    EXPECT_TRUE(MGPipeVerify(c, d, &field));

    // Nested payloads recurse, and arrays compare element-wise.
    MGPFramebufferState left{};
    MGPFramebufferState right{};
    right.Color[3].Level = 2;
    EXPECT_FALSE(MGPipeVerify(left, right, &field));
    EXPECT_STREQ(field, "Color");
}

// G6's join over the backend read inventory. P0 allows unmapped rows; from P5 the gate is
// zero, so the numbers are asserted here to make a regression visible the day it happens.
TEST(PipeCatalogue, CoverageAccountsForEveryInventoryRow) {
    EXPECT_EQ(kMGPipeInventoryReadPoints, 477u);
    EXPECT_EQ(kMGPipeInventoryUnmapped, 0u);
    EXPECT_EQ(kMGPipeInventoryMappedToCall + kMGPipeInventoryClientResolved +
                  kMGPipeInventoryReverseChannel + kMGPipeInventoryStructuralHandle +
                  kMGPipeInventoryUnmapped,
              kMGPipeInventoryReadPoints);
    EXPECT_GT(kMGPipeCoverageEntryCount, 0u);
}

// G5's field ids come from the same accessor list as the coverage table, and every field
// starts un-filled: reading one before its verb fills it is the poison's whole job.
TEST(PipeCatalogue, PipeInputFieldsStartUnfilled) {
    EXPECT_EQ(kMGPipeInputFieldCount, 63u);
    MGPipeFilledState state{};
    // Before the first fill the serial is 0 as well: 0 == 0 must not read as fresh, on the
    // sticky branch either (the window D6 names "<Field>@<none>").
    EXPECT_EQ(state.CurrentVerbSerial, 0u);
    for (SizeT f = 0; f < kMGPipeInputFieldCount; ++f) {
        EXPECT_FALSE(MGPipeInputFieldIsFresh(state, static_cast<MGPipeInputField>(f))) << kMGPipeInputFieldNames[f];
    }
    state.CurrentVerbSerial = 1;
    EXPECT_FALSE(MGPipeInputFieldIsFresh(state, MGPipeInputField::GetRenderStateParameters));
    state.FilledGen[static_cast<SizeT>(MGPipeInputField::GetRenderStateParameters)] = 1;
    EXPECT_TRUE(MGPipeInputFieldIsFresh(state, MGPipeInputField::GetRenderStateParameters));
    // The next verb makes the same value stale, which a written-once bitmap could not see.
    state.CurrentVerbSerial = 2;
    EXPECT_FALSE(MGPipeInputFieldIsFresh(state, MGPipeInputField::GetRenderStateParameters));
}

// G5b: the verb enum is GLFunctionsTable's member list (69 entries), every class has verbs,
// and the seven sticky fields ride in every class mask (P1 brief D7).
TEST(PipeCatalogue, VerbTableIsTheFunctionTable) {
    EXPECT_EQ(kMGPipeVerbCount, 69u);
    EXPECT_EQ(kMGPipeVerbClassCount, 9u);
    SizeT perClass[kMGPipeVerbClassCount] = {};
    for (SizeT v = 0; v < kMGPipeVerbCount; ++v) {
        ++perClass[static_cast<SizeT>(kMGPipeVerbClass[v])];
    }
    for (SizeT c = 0; c < kMGPipeVerbClassCount; ++c) {
        EXPECT_GT(perClass[c], 0u) << kMGPipeVerbClassNames[c];
        for (SizeT f = 0; f < kMGPipeInputFieldCount; ++f) {
            if (kMGPipeInputFieldSticky[f]) {
                EXPECT_TRUE(MGPipeFieldMaskHas(kMGPipeClassFieldMask[c], static_cast<MGPipeInputField>(f)))
                    << kMGPipeInputFieldNames[f] << " in " << kMGPipeVerbClassNames[c];
            }
        }
    }
    // The class table of D7, spot-checked at its edges: a draw reads the render state, a
    // query reads only the paused-primitive counter, and GenerateMipmap is a texture op.
    const auto& draw = kMGPipeClassFieldMask[static_cast<SizeT>(MGPipeVerbClass::kDraw)];
    const auto& query = kMGPipeClassFieldMask[static_cast<SizeT>(MGPipeVerbClass::kQuery)];
    EXPECT_TRUE(MGPipeFieldMaskHas(draw, MGPipeInputField::GetRenderStateParameters));
    EXPECT_FALSE(MGPipeFieldMaskHas(query, MGPipeInputField::GetRenderStateParameters));
    EXPECT_TRUE(MGPipeFieldMaskHas(query, MGPipeInputField::GetTransformFeedbackPausedPrimitiveCounter));
    EXPECT_EQ(kMGPipeVerbClass[static_cast<SizeT>(MGPipeVerb::GenerateMipmap)], MGPipeVerbClass::kTextureOp);
    EXPECT_STREQ(kMGPipeVerbNames[static_cast<SizeT>(MGPipeVerb::GetGpuTimestampNs)], "GetGpuTimestampNs");
}

// The sticky set is exactly the seven forwarded, argument-keyed accessors (P1 brief D6); no
// version or generation accessor is among them.
TEST(PipeCatalogue, StickyFieldsAreExactlyTheSeven) {
    const char* const expected[] = {"GetBufferBindingPointCount", "GetProgramObject",   "GetTextureObject",
                                    "HasOpenTransformFeedbackSpan", "InvalidateCompileEnv", "ValidateProgramName",
                                    "RecordError"};
    SizeT count = 0;
    for (SizeT f = 0; f < kMGPipeInputFieldCount; ++f) {
        Bool listed = false;
        for (const char* name : expected) {
            if (std::strcmp(kMGPipeInputFieldNames[f], name) == 0) listed = true;
        }
        EXPECT_EQ(kMGPipeInputFieldSticky[f], listed) << kMGPipeInputFieldNames[f];
        if (kMGPipeInputFieldSticky[f]) ++count;
    }
    EXPECT_EQ(count, 7u);
    EXPECT_EQ(kMGPipeInputStickyFieldCount, 7u);
    EXPECT_FALSE(kMGPipeInputFieldSticky[static_cast<SizeT>(MGPipeInputField::GetTextureContextId)]);
    EXPECT_FALSE(kMGPipeInputFieldSticky[static_cast<SizeT>(MGPipeInputField::GetSamplingResolutionGeneration)]);
    EXPECT_FALSE(kMGPipeInputFieldSticky[static_cast<SizeT>(MGPipeInputField::GetPipelineStateVersion)]);
}

// G4 compares floating point BY BITS (P1 brief D8): a NaN equals itself, a negative zero
// does not equal a positive one, and a vector type inside an Array inside a value struct is
// reached field by field - the differing member of the residual block is named.
TEST(PipeCatalogue, FloatVectorsCompareBitwise) {
    const Float nan = std::numeric_limits<Float>::quiet_NaN();
    const FloatVec4 a{nan, 1.f, 2.f, 3.f};
    const FloatVec4 b{nan, 1.f, 2.f, 3.f};
    EXPECT_TRUE(MGPipeFieldEqual(a, b));
    EXPECT_FALSE(a == b); // IEEE ==, the comparison the comparator must NOT use
    const FloatVec4 zero{0.f, 0.f, 0.f, 0.f};
    const FloatVec4 negativeZero{-0.f, 0.f, 0.f, 0.f};
    EXPECT_FALSE(MGPipeFieldEqual(zero, negativeZero));
    EXPECT_TRUE(zero == negativeZero);
    EXPECT_TRUE(MGPipeFieldEqual(1.5f, 1.5f));
    EXPECT_FALSE(MGPipeFieldEqual(-0.f, 0.f));

    ResidualValueBlock left{};
    ResidualValueBlock right{};
    const char* field = nullptr;
    EXPECT_TRUE(MGPipeVerify(left, right, &field));
    right.RenderState.BlendStates[3].SrcFactorRGB = BlendFactor::DstColor;
    EXPECT_FALSE(MGPipeVerify(left, right, &field));
    EXPECT_STREQ(field, "RenderState");
    const char* inner = nullptr;
    EXPECT_FALSE(MGPipeVerify(left.RenderState, right.RenderState, &inner));
    EXPECT_STREQ(inner, "BlendStates");
    // A NaN patch level in the render state equals itself too.
    right = left;
    left.RenderState.PatchDefaultOuterLevel = FloatVec4{nan, 1.f, 1.f, 1.f};
    right.RenderState.PatchDefaultOuterLevel = FloatVec4{nan, 1.f, 1.f, 1.f};
    EXPECT_TRUE(MGPipeVerify(left, right, &field));
}

// The six value structs have field lists of their own (P1 brief D8): 63 + 6 payloads, and
// the struct that used to memcmp is compared member by member.
TEST(PipeCatalogue, SixValueStructsHaveFieldLists) {
    EXPECT_EQ(kMGPipeVerifiedPayloadCount, 69u);
    static_assert(MGPipeHasFieldVerifier<RenderStateParameters>::value);
    static_assert(MGPipeHasFieldVerifier<PixelStoreParameters>::value);
    static_assert(MGPipeHasFieldVerifier<PerBufferBlendState>::value);
    static_assert(MGPipeHasFieldVerifier<StencilFaceState>::value);
    static_assert(MGPipeHasFieldVerifier<DynamicBackendParameters>::value);
    static_assert(MGPipeHasFieldVerifier<MGHostSpan>::value);
    PixelStoreParameters p{};
    PixelStoreParameters q{};
    const char* field = nullptr;
    EXPECT_TRUE(MGPipeVerify(p, q, &field));
    q.SkipRows = 2;
    EXPECT_FALSE(MGPipeVerify(p, q, &field));
    EXPECT_STREQ(field, "SkipRows");
    MGHostSpan s{};
    MGHostSpan t{};
    t.Pad0 = 0x5A; // padding is not a field
    EXPECT_TRUE(MGPipeVerify(s, t, &field));
    t.Offset = 8;
    EXPECT_FALSE(MGPipeVerify(s, t, &field));
    EXPECT_STREQ(field, "Offset");
}

// G7 pins the member list the pipeline/dynamic split is derived from.
TEST(PipeCatalogue, PipelineSubsetMembersArePinned) {
    EXPECT_EQ(kMGPipePipelineStateMemberCount, 24u);
    EXPECT_STREQ(kMGPipePipelineStateMembers[0], "CullFaceEnabled");
    EXPECT_STREQ(kMGPipePipelineStateMembers[kMGPipePipelineStateMemberCount - 1], "ColorMasks");
}

// The reverse channel is exactly ten callbacks (section 7.1).
TEST(PipeCatalogue, ReverseChannelHasTenCallbacks) {
    EXPECT_EQ(kMGPipeCallbackCount, 10u);
    EXPECT_EQ(sizeof(MGPipeCallbacks), kMGPipeCallbackCount * sizeof(void (*)()));
}

// The one shape that changes with the transport. In a monolith it resolves to the pointer
// it was given; with no transport installed a segment-backed span resolves to nothing
// rather than to garbage.
TEST(PipeCatalogue, HostSpanResolvesTheMonolithPointer) {
    static_assert(sizeof(MGHostSpan) == 32);
    const Uint8 bytes[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    MGHostSpan span{};
    span.Ptr = bytes;
    span.Size = sizeof(bytes);
    span.Offset = 2;
    EXPECT_EQ(MGPipeHostBytes(span), bytes + 2);

    MGHostSpan staged{};
    staged.Seg = 4;
    staged.Size = 16;
    EXPECT_EQ(gMGPipeSegmentResolver, nullptr);
    EXPECT_EQ(MGPipeHostBytes(staged), nullptr);
}

// D-B8: a bound buffer range carries no inline host span. The named-UBO bytes are an
// optional second var-tail announced by HostSpanCount, so the SSBO, atomic-counter and XFB
// ranges - the majority - pay nothing for a payload whose shape is not frozen yet.
TEST(PipeCatalogue, BufferRangeCarriesNoInlineHostSpan) {
    static_assert(sizeof(MGPBufferRange) == 24);
    static_assert(sizeof(MGPShaderBuffers) == 32);
    EXPECT_LT(sizeof(MGPBufferRange), sizeof(MGHostSpan));

    // The call still declares the span it may carry, so the transport lays the tail out.
    Uint32 flags = 0;
#define MGP_FLAGS_OF_SET_SHADER_BUFFERS(Name, Payload, Class, Flags)                                                   \
    if (std::strcmp(#Name, "SetShaderBuffers") == 0) flags = static_cast<Uint32>(Flags);
    MGP_CALL_LIST(MGP_FLAGS_OF_SET_SHADER_BUFFERS)
#undef MGP_FLAGS_OF_SET_SHADER_BUFFERS
    EXPECT_EQ(flags & (kVarTail | kHostSpan), static_cast<Uint32>(kVarTail | kHostSpan));

    // And the comparator sees the count that announces the tail.
    MGPShaderBuffers a{};
    MGPShaderBuffers b{};
    const char* field = nullptr;
    EXPECT_TRUE(MGPipeVerify(a, b, &field));
    b.HostSpanCount = 4;
    EXPECT_FALSE(MGPipeVerify(a, b, &field));
    EXPECT_STREQ(field, "HostSpanCount");
}

// The buffer half of resource_subdata has no level and no box of its own: [offset, size)
// rides in UnionBox.X / UnionBox.W, and only through the two helpers, which also say where
// one record stops and the emitter has to split.
TEST(PipeCatalogue, SubDataBufferRangeRidesInTheUnionBox) {
    MGPSubData record{};
    record.Level = 3;
    record.RegionCount = 2;
    ASSERT_TRUE(MGPipeSetSubDataBufferRange(record, 4096, 65536));
    EXPECT_EQ(record.UnionBox.X, 4096);
    EXPECT_EQ(record.UnionBox.W, 65536u);
    EXPECT_EQ(record.UnionBox.Y, 0);
    EXPECT_EQ(record.UnionBox.Z, 0);
    EXPECT_EQ(record.UnionBox.H, 1u);
    EXPECT_EQ(record.UnionBox.D, 1u);
    EXPECT_EQ(record.Level, 0);
    EXPECT_EQ(record.RegionCount, 0u);
    EXPECT_EQ(MGPipeSubDataBufferOffset(record), 4096u);
    EXPECT_EQ(MGPipeSubDataBufferSize(record), 65536u);

    // The largest range one record expresses...
    ASSERT_TRUE(MGPipeSetSubDataBufferRange(record, 0x7FFFFFFFull, 0xFFFFFFFFull));
    EXPECT_EQ(MGPipeSubDataBufferOffset(record), 0x7FFFFFFFull);
    EXPECT_EQ(MGPipeSubDataBufferSize(record), 0xFFFFFFFFull);
    // ...and beyond it the emitter splits: refused, record untouched.
    EXPECT_FALSE(MGPipeSetSubDataBufferRange(record, 0x80000000ull, 1));
    EXPECT_FALSE(MGPipeSetSubDataBufferRange(record, 0, 0x100000000ull));
    EXPECT_EQ(MGPipeSubDataBufferOffset(record), 0x7FFFFFFFull);
    EXPECT_EQ(MGPipeSubDataBufferSize(record), 0xFFFFFFFFull);
}
