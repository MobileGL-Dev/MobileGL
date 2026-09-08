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
#include <iterator>
#include <limits>
#include <type_traits>

#include "Includes.h"
#include <MG_Pipe/MGPipe.h>
// P4a: MGPipeUnmigratedEmulation's declaration, and the applier's records the catalogue's size
// pins now reach. Push-only, like the translation unit that defines them - in a pull build the
// symbol does not exist and the one case that calls it is compiled out.
#if MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Pipe/PipeApply.h>
#endif

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
    // P2 ate 1240 of the 1248: RenderStateParameters retired to create/bind_render_state and
    // set_dynamic_state, PixelStoreParameters to set_pixel_pack_state, the patch quintet to
    // set_patch_state. What is left is one Uint64 of capability bits, and it is redundant on
    // purpose - the applier's trip wire compares it against the assembled block.
    EXPECT_EQ(sizeof(ResidualValueBlock), 8u);
    EXPECT_LT(sizeof(ResidualValueBlock), sizeof(RenderStateParameters));
    EXPECT_EQ(offsetof(ResidualValueBlock, CapabilityBits), 0u);
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

// P2 ATE THE TWO VALUE STRUCTS AND THE PATCH TAIL the name still remembers, and the name
// stays because a removed test name is a gate failure of its own (G14, additions only).
// What it now pins is the other half of the same statement: the carrier is one capability
// word, at offset 0, and the members it used to carry are gone rather than merely moved -
// which is exactly what "MGL_RESIDUAL_BLOCK_SIZE only ever goes down" has to mean.
TEST(PipeCatalogue, ResidualBlockIsExactlyItsTwoValueStructsPlusPatchTail) {
    EXPECT_EQ(offsetof(ResidualValueBlock, CapabilityBits), 0u);
    EXPECT_EQ(sizeof(ResidualValueBlock), sizeof(Uint64));
    // The three carriers that took the retired members over.
    EXPECT_EQ(sizeof(MGPPixelPackState), sizeof(PixelStoreParameters));
    EXPECT_EQ(sizeof(MGPPatchState), 40u);
    EXPECT_EQ(sizeof(MGPBindRenderState), 12u);
}

// P4a's two payload edits, which are the only two the phase makes, and both are the kind a
// compiler catches only where somebody asked it to. MGP_ASSERT_POD already pins both sizes in
// MGPipeTypes.h; what is pinned HERE is the SHAPE the two edits were made for, because that is
// what a later phase would silently undo.
TEST(PipeCatalogue, TextureParamsNameTheirBuiltinSamplerAndFramebufferStateNamesItsTarget) {
    // 32 -> 40: the CSO handle carrying the SamplerParameters of the SamplerObject every
    // ITextureObject owns, plus the second resync bit. Naming the CSO rather than widening
    // this payload with a filter/wrap/border block is what keeps ONE authority for one value -
    // duplicating SamplerParameters on the wire would give two.
    EXPECT_EQ(sizeof(MGPTextureParams), 40u);
    EXPECT_EQ(offsetof(MGPTextureParams, Res), 0u);
    EXPECT_EQ(offsetof(MGPTextureParams, BuiltinSampler), 8u);
    EXPECT_EQ(offsetof(MGPTextureParams, SamplerResync), 26u);
    // The two resync bits are SEPARATE bytes and must stay so: ForceResync guards a swizzle
    // override the frontend params version does not move for, SamplerResync guards an
    // incomplete texture sampling (0,0,0,1) after a driver re-mint. Different failures,
    // different owners, one byte each.
    EXPECT_NE(offsetof(MGPTextureParams, ForceResync), offsetof(MGPTextureParams, SamplerResync));

    // Pad0 -> Uint8 Target, and the SIZE DID NOT MOVE, which is the whole point: the record is
    // emitted once per bound target that moved, or once with Both, and that costs a byte the
    // struct already had.
    EXPECT_EQ(sizeof(MGPFramebufferState), 304u);
    EXPECT_EQ(static_cast<Uint8>(MGPipeFramebufferTarget::Draw), 0u);
    EXPECT_EQ(static_cast<Uint8>(MGPipeFramebufferTarget::Read), 1u);
    EXPECT_EQ(static_cast<Uint8>(MGPipeFramebufferTarget::Both), 2u);
    // The wire's colour-attachment width is ONE width, and it is the wire's rather than the
    // driver's: a driver reporting more attachments than this is refused at bring-up, never
    // truncated into the record.
    EXPECT_EQ(kMGPipeMaxColorAttachments, 8u);
    EXPECT_EQ(std::extent_v<decltype(MGPFramebufferState::Color)>, kMGPipeMaxColorAttachments);
    EXPECT_EQ(std::extent_v<decltype(MGPFramebufferState::DrawBuffers)>, kMGPipeMaxColorAttachments);

    // And the two unit bounds, which bound all three var-tail sets. One merged unit space, no
    // stage dimension.
    EXPECT_EQ(kMGPipeMaxTextureUnits, 192u);
    EXPECT_EQ(kMGPipeMaxImageUnits, 192u);
}

// D-A3: the resource-target enum minted beside the field, and the property that makes it worth
// minting - EVERY TextureTarget has a row, checked at compile time by a table with no
// `default:` arm, so adding a target is a build break rather than a descriptor that silently
// describes the wrong kind of storage.
TEST(PipeCatalogue, EveryTextureTargetMapsToItsOwnResourceTarget) {
    // The compile-time half is MGPipeEveryTextureTargetIsMapped's static_assert; this is the
    // same walk at runtime, so the case names the offender instead of the build naming a line.
    for (SizeT i = 0; i < static_cast<SizeT>(TextureTarget::TextureTargetCount); ++i) {
        const auto target = static_cast<TextureTarget>(i);
        EXPECT_NE(MGPipeResourceTargetForTextureTarget(target), kMGPipeResourceTargetUnmapped)
            << "TextureTarget " << i << " has no MGPResourceDesc::Target row";
        EXPECT_LT(MGPipeResourceTargetForTextureTarget(target),
                  static_cast<Uint32>(MGPipeResourceTarget::Count));
    }
    // Buffer is 0 and stays 0: P3a's constant is what a zero-initialised record already says,
    // and the narrowed ack predicate below compares against it.
    EXPECT_EQ(static_cast<Uint32>(MGPipeResourceTarget::Buffer), 0u);
    EXPECT_EQ(kMGPipeResourceTargetBuffer, 0u);
    // No texture target may collide with the buffer target, or a texture descriptor would ask
    // for a synchronous acknowledgement.
    for (SizeT i = 0; i < static_cast<SizeT>(TextureTarget::TextureTargetCount); ++i) {
        EXPECT_NE(MGPipeResourceTargetForTextureTarget(static_cast<TextureTarget>(i)),
                  static_cast<Uint32>(kMGPipeResourceTargetBuffer));
    }
    // A rectangle texture is NOT a 2D texture on the wire. Espryt lowers both to GL_TEXTURE_2D
    // at bind time and lowers Texture1D the same way, and Tex1D still has an enumerator of its
    // own; folding rectangle onto Tex2D here would erase a distinction both backends switch on.
    EXPECT_NE(MGPipeResourceTargetForTextureTarget(TextureTarget::Texture2D),
              MGPipeResourceTargetForTextureTarget(TextureTarget::TextureRectangle));
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

    // The residual carrier is one field since P2, so the nested-struct case it used to
    // demonstrate is demonstrated on RenderStateParameters directly - which is where it
    // actually matters now that the block travels as create/bind_render_state chunks.
    ResidualValueBlock left{};
    ResidualValueBlock right{};
    const char* field = nullptr;
    EXPECT_TRUE(MGPipeVerify(left, right, &field));
    right.CapabilityBits = 1ull << static_cast<Uint64>(CapabilityInput::FramebufferSrgb);
    EXPECT_FALSE(MGPipeVerify(left, right, &field));
    EXPECT_STREQ(field, "CapabilityBits");

    RenderStateParameters leftState{};
    RenderStateParameters rightState{};
    const char* inner = nullptr;
    EXPECT_TRUE(MGPipeVerify(leftState, rightState, &inner));
    rightState.BlendStates[3].SrcFactorRGB = BlendFactor::DstColor;
    EXPECT_FALSE(MGPipeVerify(leftState, rightState, &inner));
    EXPECT_STREQ(inner, "BlendStates");
    // P2's three new capability bools are members like any other, so the comparator names
    // them rather than folding them into a neighbour's padding.
    rightState = leftState;
    rightState.FramebufferSrgbEnabled = true;
    EXPECT_FALSE(MGPipeVerify(leftState, rightState, &inner));
    EXPECT_STREQ(inner, "FramebufferSrgbEnabled");
    // A NaN patch level equals itself too.
    rightState = leftState;
    leftState.PatchDefaultOuterLevel = FloatVec4{nan, 1.f, 1.f, 1.f};
    rightState.PatchDefaultOuterLevel = FloatVec4{nan, 1.f, 1.f, 1.f};
    EXPECT_TRUE(MGPipeVerify(leftState, rightState, &inner));
}

// The six value structs have field lists of their own (P1 brief D8): 63 + 6 payloads, and
// the struct that used to memcmp is compared member by member. P3a added the two vertex wire
// views as a seventh and eighth non-payload entry (63 + 8), for the same reason: they are the
// elements of create_vertex_elements' blob, and a memcmp over that blob would false-differ on
// MGPVertexAttribWire::Pad0. P4a adds SamplerParameters as a ninth (63 + 9 = 72), and the name
// of this case stays what it was, because a removed test name is a gate failure of its own.
//
// SamplerParameters IS THE SHARPEST OF THE NINE. It is 100 bytes with THREE BYTES OF TRAILING
// PADDING (96 bytes of members plus the one-byte borderColorForm), it rides
// MGPSamplerDesc::Parameters as a blob, and until P4a it had no field list and no verify-list
// row at all - so the comparator fell back to comparing the blob as BYTES and could
// false-differ on padding nobody writes. That is not a theoretical hazard for this struct:
// the client's CSO cache confirms a hash hit with a memcmp over the same bytes, so a codec or
// a cache that read the padding would mint a fresh CSO per call and the verify lane would
// abort at random.
TEST(PipeCatalogue, SixValueStructsHaveFieldLists) {
    EXPECT_EQ(kMGPipeVerifiedPayloadCount, 72u);
    static_assert(MGPipeHasFieldVerifier<RenderStateParameters>::value);
    static_assert(MGPipeHasFieldVerifier<PixelStoreParameters>::value);
    static_assert(MGPipeHasFieldVerifier<PerBufferBlendState>::value);
    static_assert(MGPipeHasFieldVerifier<StencilFaceState>::value);
    static_assert(MGPipeHasFieldVerifier<DynamicBackendParameters>::value);
    static_assert(MGPipeHasFieldVerifier<MGHostSpan>::value);
    static_assert(MGPipeHasFieldVerifier<MGPVertexAttribWire>::value);
    static_assert(MGPipeHasFieldVerifier<MGPVertexBindingPointWire>::value);
    static_assert(MGPipeHasFieldVerifier<SamplerParameters>::value);
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

    // P4a's ninth, and its two halves. First: the comparator sees the members, INCLUDING
    // borderColorForm - which is the field a backend picks glSamplerParameterIiv over fv by,
    // and which no value comparison can infer because all three border representations are
    // always numerically populated.
    SamplerParameters left{};
    SamplerParameters right{};
    EXPECT_TRUE(MGPipeVerify(left, right, &field));
    right.borderColorForm = BorderColorForm::Int;
    EXPECT_FALSE(MGPipeVerify(left, right, &field));
    EXPECT_STREQ(field, "borderColorForm");
    right = left;
    right.borderColorI = IntVec4{1, 0, 0, 0};
    EXPECT_FALSE(MGPipeVerify(left, right, &field));
    EXPECT_STREQ(field, "borderColorI");
    right = left;
    right.maxAnisotropy = 4.0f;
    EXPECT_FALSE(MGPipeVerify(left, right, &field));
    EXPECT_STREQ(field, "maxAnisotropy");

    // Second, and this is the one a byte comparison gets wrong: the THREE TRAILING PADDING
    // BYTES are not fields, so garbage in them cannot make two equal sampler states differ.
    // Written through a byte pointer, because that is the only way to reach a byte the struct
    // does not name.
    static_assert(sizeof(SamplerParameters) == 100);
    right = left;
    auto* rightBytes = reinterpret_cast<unsigned char*>(&right);
    for (SizeT i = sizeof(SamplerParameters) - 3; i < sizeof(SamplerParameters); ++i) {
        rightBytes[i] = 0x5A;
    }
    EXPECT_TRUE(MGPipeVerify(left, right, &field))
        << "the comparator read a padding byte: field=" << (field != nullptr ? field : "(none)");
}

// G7 pins the member list the pipeline/dynamic split is derived from.
TEST(PipeCatalogue, PipelineSubsetMembersArePinned) {
    // 44 as of P2, in DECLARATION order. It grew from the 24 members
    // ComputePipelineStateHash used to hash because the chunk table's rule is "a byte is
    // pipeline state iff a setter that calls BumpVersions() writes it", and that is a strict
    // superset: sample coverage, front face, provoking vertex, the scissor-test mask, the
    // back polygon mode, eleven capability bools the hash never read, and the three
    // capabilities P2 gave storage to.
    EXPECT_EQ(kMGPipePipelineStateMemberCount, 44u);
    EXPECT_STREQ(kMGPipePipelineStateMembers[0], "PatchVertices");
    EXPECT_STREQ(kMGPipePipelineStateMembers[kMGPipePipelineStateMemberCount - 1],
                 "ScissorTestEnabledMask");
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

// P3a, D-H1: set_vertex_buffers carries the vertex-FETCH base instance explicitly, one per
// emitted set rather than one per entry, and the header grew 16 -> 24 bytes to hold it.
//
// The size is the cheap half. The half a compiler cannot catch is the PipeFields.def row:
// MGPVertexBuffers still HAS a ContentHash and still asserts its size whether or not the
// field list names BaseInstance, and a comparator blind to the field would let a
// baseInstance-only divergence through under MOBILEGL_PIPE_VERIFY - which is the one gate
// that would otherwise have seen the suppression bug the ContentHash rule exists to prevent.
// So the field list is pinned the only way it can be: by making the comparator name it.
TEST(PipeCatalogue, VertexBufferSetCarriesAnExplicitBaseInstance) {
    static_assert(sizeof(MGPVertexBuffers) == 24);
    static_assert(sizeof(MGPVertexBuffer) == 32); // the per-entry struct did NOT change
    EXPECT_EQ(sizeof(MGPVertexBuffers), 24u);

    MGPVertexBuffers a{};
    MGPVertexBuffers b{};
    const char* field = nullptr;
    EXPECT_TRUE(MGPipeVerify(a, b, &field));
    b.Pad0 = 0x5A; // padding is not a field
    EXPECT_TRUE(MGPipeVerify(a, b, &field));
    b.Pad0 = 0;
    b.BaseInstance = 7;
    EXPECT_FALSE(MGPipeVerify(a, b, &field));
    EXPECT_STREQ(field, "BaseInstance");

    // The set still carries no fetch shift per entry: an entry that disagreed with its own
    // header is a shape the applier would have to police, and MGPVertexBuffer's Pad0 stays
    // padding rather than becoming a second copy of the same number.
    MGPVertexBuffer left{};
    MGPVertexBuffer right{};
    right.Pad0 = 0x5A;
    EXPECT_TRUE(MGPipeVerify(left, right, &field));
}

// P3a, D-G2: the two vertex wire views. They are what create_vertex_elements' blob is made
// of, so their sizes are the blob's stride and the applier's bounds arithmetic; and IsLong is
// carried SEPARATELY from Type, because a GL_DOUBLE format converted to float and a long
// format that keeps all 64 bits are different requests that a backend has to tell apart.
TEST(PipeCatalogue, VertexWireViewsAreFlatAndCarryIsLongSeparately) {
    static_assert(sizeof(MGPVertexAttribWire) == 24);
    static_assert(sizeof(MGPVertexBindingPointWire) == 16);
    EXPECT_EQ(sizeof(MGPVertexAttribWire), 24u);
    EXPECT_EQ(sizeof(MGPVertexBindingPointWire), 16u);

    MGPVertexAttribWire a{};
    MGPVertexAttribWire b{};
    const char* field = nullptr;
    EXPECT_TRUE(MGPipeVerify(a, b, &field));
    b.Pad0 = 0x5A;
    EXPECT_TRUE(MGPipeVerify(a, b, &field));
    b.Pad0 = 0;
    // Type unchanged, IsLong moved: a comparator that folded the two would miss this.
    b.IsLong = 1;
    EXPECT_FALSE(MGPipeVerify(a, b, &field));
    EXPECT_STREQ(field, "IsLong");

    MGPVertexBindingPointWire p{};
    MGPVertexBindingPointWire q{};
    EXPECT_TRUE(MGPipeVerify(p, q, &field));
    q.Divisor = 2;
    EXPECT_FALSE(MGPipeVerify(p, q, &field));
    EXPECT_STREQ(field, "Divisor");
}

// P3a, D-A5: the tree's FIRST kNeedsAck, and the reason it is not a bare flag.
//
// Flags are a PER-CALL static property and resource_respecify serves both glBufferData and
// glBufferStorage. A bare kNeedsAck on the call would acknowledge every glBufferData in a
// world upload - a round trip per chunk store the moment a transport is under it. So the flag
// declares that records of this call MAY need one and MGPipeResourceRespecifyNeedsAck decides
// per record: only an immutable store, which is a real synchronous allocation.
//
// This is the negative control for a future flag that over-acks: in monolith the ack is a
// no-op, so the mistake cannot be shipped from here, and the phase where it would bite
// inherits this pin rather than the guess.
TEST(PipeCatalogue, ResourceRespecifyAcksOnlyImmutableStorage) {
    Uint32 flags = 0;
#define MGP_FLAGS_OF_RESOURCE_RESPECIFY(Name, Payload, Class, Flags)                                                   \
    if (std::strcmp(#Name, "ResourceRespecify") == 0) flags = static_cast<Uint32>(Flags);
    MGP_CALL_LIST(MGP_FLAGS_OF_RESOURCE_RESPECIFY)
#undef MGP_FLAGS_OF_RESOURCE_RESPECIFY
    EXPECT_EQ(flags & static_cast<Uint32>(kNeedsAck), static_cast<Uint32>(kNeedsAck));
    // And it is the ONLY call that carries it: a second one would be a second decision, and
    // this predicate answers for exactly one call.
    Uint32 ackingCalls = 0;
#define MGP_COUNT_ACKING_CALLS(Name, Payload, Class, Flags)                                                            \
    if ((static_cast<Uint32>(Flags) & static_cast<Uint32>(kNeedsAck)) != 0) ++ackingCalls;
    MGP_CALL_LIST(MGP_COUNT_ACKING_CALLS)
#undef MGP_COUNT_ACKING_CALLS
    EXPECT_EQ(ackingCalls, 1u);

    // glBufferStorage: an immutable store, and the one entry point allowed a synchronous ack.
    MGPResourceDesc immutable{};
    immutable.Immutable = 1;
    EXPECT_TRUE(MGPipeResourceRespecifyNeedsAck(immutable));

    // glBufferData through the same call: never acknowledged, whatever else the descriptor
    // says. The usage hint and a defined initial content are the two things a "well it looks
    // synchronous" reading would key on, so both are set here on purpose.
    MGPResourceDesc mutableStore{};
    mutableStore.Immutable = 0;
    mutableStore.Usage = 0x88E4; // GL_STATIC_DRAW, i.e. the most "final-looking" hint there is
    mutableStore.HasDefinedContent = 1;
    mutableStore.Width = 64u * 1024u;
    EXPECT_FALSE(MGPipeResourceRespecifyNeedsAck(mutableStore));

    // P4a: THE TWO IDIOMS THAT MADE THE PREDICATE HAVE TO NARROW. Textures travel on the same
    // resource_respecify row as buffers, and glTexStorage* sets Immutable for a real reason -
    // it is a descriptor fact the backend reads - so an Immutable-only predicate would have
    // started acknowledging every immutable texture allocation the moment P4a's texture family
    // landed. Texture allocation is already deferred to sync time in monolith (glTexImage* and
    // glTexStorage* only mark the storage dirty, and even glRenderbufferStorage* allocates
    // lazily inside SyncToBackend), so splitting changes no observable behaviour and this batch
    // must not ack. glBufferStorage stays the only entry point allowed a synchronous one.
    //
    // This is the negative control for a future widening, in both directions: a predicate that
    // stopped naming the buffer target would turn these two green-and-wrong.
    MGPResourceDesc immutableTexture{};
    immutableTexture.Immutable = 1; // glTexStorage2D
    immutableTexture.Target =
        static_cast<Uint8>(MGPipeResourceTargetForTextureTarget(TextureTarget::Texture2D));
    immutableTexture.Width = 256;
    immutableTexture.Height = 256;
    immutableTexture.Levels = 9;
    EXPECT_FALSE(MGPipeResourceRespecifyNeedsAck(immutableTexture));

    MGPResourceDesc renderbuffer{};
    renderbuffer.Immutable = 1; // glRenderbufferStorage: one shot, and still lazy in the backend
    renderbuffer.Target = static_cast<Uint8>(MGPipeResourceTarget::Renderbuffer);
    renderbuffer.Width = 1920;
    renderbuffer.Height = 1080;
    EXPECT_FALSE(MGPipeResourceRespecifyNeedsAck(renderbuffer));

    // And the buffer half still answers true with the target spelled explicitly rather than
    // relying on a zero-initialised record to mean "buffer".
    MGPResourceDesc immutableBuffer{};
    immutableBuffer.Immutable = 1;
    immutableBuffer.Target = kMGPipeResourceTargetBuffer;
    EXPECT_TRUE(MGPipeResourceRespecifyNeedsAck(immutableBuffer));

    // And the opcode did not move: a flag-word edit is not a catalogue edit.
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::ResourceRespecify), 3);
}

// G13b, D-M: "emulation 在 split 下显式 Fatal 直到 P8" costs P4a a NAMED, GREPPABLE call site
// per unmigrated emulation and nothing else - in monolith MGPipeUnmigratedEmulation is a no-op
// and the emulation still runs on exactly the code path it runs on today. What this pins is
// the LIST, because the whole value of the mechanism is that P5 and P8 edit one function
// instead of rediscovering five call sites, and a site that quietly disappears has to be a red
// gate rather than a surprise three phases later.
//
// The names are pinned here rather than counted in the backend, because the count alone cannot
// say WHICH one was lost. The purity gate greps the count; this says what the count is of.
TEST(PipeCatalogue, EveryUnmigratedEmulationIsNamedOnce) {
    // Every one of these is an emulation that reads or writes CLIENT memory a split server
    // would not have: a CPU shadow mirror, a CPU mipmap fallback, a shadow-conversion readback,
    // and the re-dirty of already-uploaded levels that a texture re-mint performs.
    const char* const kNames[] = {
        "copy-image-shadow-mirror",     // the glCopyImageSubData CPU-shadow mirror
        "generate-mipmap-storage",      // EnsureGenerateMipmapStorageAllocated
        "generate-mipmap-cpu-fallback", // GenerateThreeChannelFloatMipmapOnCpu
        "get-tex-image-shadow",         // GetTexImageViaShadowConversion
        "texture-remint-pull",          // RequireImageBindableStorage's re-dirty
    };
    EXPECT_EQ(std::size(kNames), 5u);
    // No duplicates: two sites sharing a name would make the grepped count and this list
    // disagree in the one direction nobody would notice.
    for (SizeT i = 0; i < std::size(kNames); ++i) {
        for (SizeT j = i + 1; j < std::size(kNames); ++j) {
            EXPECT_STRNE(kNames[i], kNames[j]);
        }
    }
    // The last one is the head of the only NEW stall class the design admits, and P4a supplies
    // exactly one of its four mitigations - prevention, through ImageBindableHint on every
    // create and respecify. The async pull, the bounded retention and the
    // ResourceSubDataComplete terminator are a later phase's, and P4a must not build half a
    // terminator.
    EXPECT_STREQ(kNames[4], "texture-remint-pull");
#if MOBILEGL_PIPE_PUSH
    // In monolith it really is a no-op: calling it changes nothing and returns nothing. The
    // teeth are a split server's, and the call site is what P8 gives them to.
    for (const char* name : kNames) MGPipeUnmigratedEmulation(name);
#endif
}

// THE ShaderCso COMPOSITE BAND IS A SECOND SPACE, AND THE ALLOCATOR REPORTS IT SEPARATELY.
//
// The band's base is 983040, so a composite handle passes every bound an ordinary one does and
// a slot-indexed table that forgets the band allocates ~983k entries for one program pipeline.
// That is why the allocator keeps two dense tables - and it is also why the two must be
// COUNTED apart: a high-water mark that folded them would be pinned at ~983k from the first
// composite mint onward, and every "the high-water mark did not move over N churn rounds"
// assertion about ORDINARY ShaderCso slots - the shape that catches a dense table that never
// shrinks, i.e. the ~1.3 KB-per-record leak the P3a final review found - would be vacuously
// true for the rest of the process. One merged number is one real assertion and one that
// cannot go red; two numbers are two real assertions, which is what the per-kind leak cases
// need.
//
// This case pins both halves: a leaked COMPOSITE moves the band's marks and not the ordinary
// one, and an ordinary leak still moves the ordinary mark with a composite outstanding.
TEST(PipeCatalogue, TheCompositeShaderBandIsCountedApartFromTheOrdinarySpace) {
#if MOBILEGL_PIPE_PUSH
    MGPipeSlotAllocator slots;

    const Uint32 ordinaryBefore = slots.HighWater(MGPipeKind::ShaderCso);
    EXPECT_EQ(slots.CompositeHighWater(), kMGPipeShaderCsoCompositeSlotBase)
        << "the band's high-water mark starts at its base, so it is monotone from the first mint";
    EXPECT_EQ(slots.CompositeLiveCount(), 0u);
    EXPECT_EQ(slots.CompositeFreeCount(), 0u);

    // A COMPOSITE MOVES THE BAND'S MARKS AND ONLY THOSE.
    const MGPipeHandle composite = slots.AllocateComposite(9001);
    ASSERT_FALSE(MGPipeHandleIsNull(composite));
    ASSERT_TRUE(MGPipeIsCompositeShaderSlot(composite.Slot));
    EXPECT_EQ(slots.HighWater(MGPipeKind::ShaderCso), ordinaryBefore)
        << "a composite mint moved the ORDINARY high-water mark, so the ordinary space's leak "
           "assertion is vacuous from here on";
    EXPECT_EQ(slots.CompositeHighWater(), kMGPipeShaderCsoCompositeSlotBase + 1u);
    EXPECT_EQ(slots.CompositeLiveCount(), 1u);
    // A live composite IS a live ShaderCso: the merged count is deliberate and stays.
    EXPECT_EQ(slots.LiveCount(MGPipeKind::ShaderCso), 1u);

    // AND THE ORDINARY MARK STILL MOVES WITH A COMPOSITE OUTSTANDING - the half that stopped
    // existing when one number carried both spaces.
    const MGPipeHandle ordinary = slots.Allocate(MGPipeKind::ShaderCso);
    ASSERT_FALSE(MGPipeHandleIsNull(ordinary));
    EXPECT_FALSE(MGPipeIsCompositeShaderSlot(ordinary.Slot));
    EXPECT_GT(slots.HighWater(MGPipeKind::ShaderCso), ordinaryBefore);
    EXPECT_EQ(slots.CompositeHighWater(), kMGPipeShaderCsoCompositeSlotBase + 1u)
        << "an ordinary mint moved the BAND's high-water mark";

    // The slot goes back to the BAND's free list, and the high-water marks do not come back
    // down - which is exactly what makes them a leak witness rather than a live count.
    const Uint32 ordinaryHighWater = slots.HighWater(MGPipeKind::ShaderCso);
    slots.Free(MGPipeKind::ShaderCso, composite);
    EXPECT_EQ(slots.CompositeLiveCount(), 0u);
    EXPECT_EQ(slots.CompositeFreeCount(), 1u);
    EXPECT_EQ(slots.FreeCount(MGPipeKind::ShaderCso), 1u);
    EXPECT_EQ(slots.CompositeHighWater(), kMGPipeShaderCsoCompositeSlotBase + 1u);
    EXPECT_EQ(slots.HighWater(MGPipeKind::ShaderCso), ordinaryHighWater);
    EXPECT_EQ(slots.LiveCount(MGPipeKind::ShaderCso), 1u);
#else
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no client slot allocator in a pull build";
#endif
}
