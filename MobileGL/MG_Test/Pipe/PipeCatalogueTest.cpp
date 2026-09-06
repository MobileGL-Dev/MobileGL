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
    EXPECT_EQ(kMGPipeInputFieldCount, 61u);
    MGPipeFilledState state{};
    state.CurrentVerbSerial = 1;
    EXPECT_FALSE(MGPipeInputFieldIsFresh(state, MGPipeInputField::GetRenderStateParameters));
    state.FilledGen[static_cast<SizeT>(MGPipeInputField::GetRenderStateParameters)] = 1;
    EXPECT_TRUE(MGPipeInputFieldIsFresh(state, MGPipeInputField::GetRenderStateParameters));
    // The next verb makes the same value stale, which a written-once bitmap could not see.
    state.CurrentVerbSerial = 2;
    EXPECT_FALSE(MGPipeInputFieldIsFresh(state, MGPipeInputField::GetRenderStateParameters));
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
