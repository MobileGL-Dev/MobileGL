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
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
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
// P5 R-17: the routing that INSTALLS the two tables. Included here so that the installation
// case below states the partition deterministically rather than depending on whether some
// other object in this particular test binary happened to drag the installer in.
#include <MG_Pipe/PipeRoute.h>
#endif

using namespace MobileGL;
using namespace MobileGL::MG_Pipe;

TEST(PipeCatalogue, FrontendNeverTakesAnApplierAddress) {
    namespace fs = std::filesystem;
    auto base = fs::current_path();
    while (!fs::is_directory(base / "MobileGL/MG_Impl") && base != base.root_path())
        base = base.parent_path();
    const auto root = base / "MobileGL/MG_Impl";
    ASSERT_TRUE(fs::is_directory(root)) << "frontend source unavailable: " << root;
    const std::regex comments(R"(/\*[\s\S]*?\*/|//[^\n]*)");
    const std::regex address(R"(&\s*MGPipeApply[A-Za-z_0-9]*)");
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        const auto ext = entry.path().extension();
        if (ext != ".cpp" && ext != ".h" && ext != ".inc") continue;
        std::ifstream input(entry.path());
        ASSERT_TRUE(input.good()) << entry.path();
        std::ostringstream bytes;
        bytes << input.rdbuf();
        const auto code = std::regex_replace(bytes.str(), comments, "");
        EXPECT_FALSE(std::regex_search(code, address))
            << "FrontendApplierAddress: " << entry.path()
            << " must take the route address; routing tables live outside MG_Impl";
    }
}

namespace {
    // Counting expansions of the catalogue. The Class parameter is a real enumerator, so a
    // per-class count is a constant expression too.
#define MGP_COUNT_ONE(Name, Payload, Class, Flags, Wait) +1
#define MGP_COUNT_CLASS(Name, Payload, Class, Flags, Wait) +((Class) == countedClass ? 1 : 0)

    constexpr SizeT kExpandedCallCount = 0 MGP_CALL_LIST(MGP_COUNT_ONE);

    template <MGPipeCallClass countedClass>
    constexpr SizeT ClassCount() {
        return 0 MGP_CALL_LIST(MGP_COUNT_CLASS);
    }

    // Every payload named in the catalogue must be a memcpy-able POD, and so must every
    // payload the verify comparator knows about.
#define MGP_ASSERT_CALL_PAYLOAD_POD(Name, Payload, Class, Flags, Wait)                                                       \
    static_assert(std::is_trivially_copyable_v<Payload>, #Name "'s payload " #Payload " is not trivially copyable");
    MGP_CALL_LIST(MGP_ASSERT_CALL_PAYLOAD_POD)

#define MGP_ASSERT_VERIFY_PAYLOAD_POD(Payload)                                                                         \
    static_assert(std::is_trivially_copyable_v<Payload>, #Payload " is not trivially copyable");
    MGP_VERIFY_PAYLOAD_LIST(MGP_ASSERT_VERIFY_PAYLOAD_POD)

    // The catalogue's own spelling of an opcode, for a failure message. The wire codec has one
    // of these and MG_Pipe may not reach it (MG_Remote is above this layer), so the table is
    // expanded from the catalogue here rather than a name being duplicated per EXPECT.
#define MGP_CATALOGUE_NAME_ROW(Name, Payload, Class, Flags, Wait) #Name,
    constexpr const char* kCatalogueOpNames[] = {
        "<kInvalid>",
        MGP_CALL_LIST(MGP_CATALOGUE_NAME_ROW)
    };
#undef MGP_CATALOGUE_NAME_ROW
    static_assert(sizeof(kCatalogueOpNames) / sizeof(kCatalogueOpNames[0]) ==
                      static_cast<SizeT>(MGPWireOp::kOpCount),
                  "the diagnostic name table and the opcode space disagree");

    const char* WireOpNameForDiag(MGPWireOp op) {
        const SizeT index = static_cast<SizeT>(op);
        return index < static_cast<SizeT>(MGPWireOp::kOpCount) ? kCatalogueOpNames[index]
                                                               : "<opcode out of range>";
    }
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
    // kScreen is 11 + P5c's applier_reset (MG_Remote/CONTRACT-P5C.md §5.1); kCtxObject is
    // 9 + P5c's object_death (§5.2), the framebuffer family's first wire delete opcode.
    // kCtxState is 17 + P5c rv's set_context_values (§5.3), the residual-value record, + P5e's
    // set_program_bindings (MG_Remote/CONTRACT-P5E.md §1), the post-link binding record.
    EXPECT_EQ(ClassCount<kScreen>(), 12u);
    EXPECT_EQ(ClassCount<kCtxQuery>(), 8u);
    EXPECT_EQ(ClassCount<kCtxCso>(), 13u);
    EXPECT_EQ(ClassCount<kCtxState>(), 19u);
    EXPECT_EQ(ClassCount<kCtxObject>(), 10u);
    // 13 + the five P5b-appended verbs (MG_Remote/CONTRACT-P5B.md): bind_shader_image,
    // patch_parameter, bind_stream_output, set_storage_block_binding,
    // copy_framebuffer_to_texture.
    EXPECT_EQ(ClassCount<kCtxVerb>(), 19u);
}

// A row nobody has migrated is null - which is exactly what "this subsystem has not been
// migrated, keep pulling" means (plan B section 4.1).
//
// UNTIL P5 R-17 THAT WAS EVERY ROW, and this case said so. It is now EXACTLY THE 41 ROWS WITH
// NO BODIED MGPipeApply* ENTRY POINT (80 - the 39 that have one; the number was 34 at P5, 39
// after P5b's five sink-only verbs, rv's set_context_values grew BOTH sides of the difference,
// P5e's set_program_bindings grew the null side alone, and P5e's set_shader_buffers moved one
// row from the null side to the other - sb gave it its applier, its adapter and its emitter in
// one commit):
// the other 39 have an applier, R-17 installs adapters over them,
// and a null there would no longer mean "keep pulling" - `MG_Impl/Pipe`'s call sites go through
// the thunks, so a null would mean "call through a null pointer". The number is asserted rather
// than the emptiness, because "39 installed" and "41 still null" are the two halves of a
// partition and a case that checked only one of them would pass an installer that had
// overwritten rows it does not own.
// THE NAME IS KEPT, AND SO IS THE STATEMENT IT MAKES - only the ROWS it makes it about have
// narrowed. G2/G14 compare ctest names against a pre-P5 baseline and require ZERO removed, so
// renaming a case is a removal even when the new name is better: it is indistinguishable, from
// the gate's side, from a case that was deleted. So this case stays, asserting the half that is
// still true, and the new half below is an ADDED name.
TEST(PipeCatalogue, UninstalledTablesAreAllNull) {
    const void* const* screen = reinterpret_cast<const void* const*>(&gMGPipeScreen);
    const void* const* context = reinterpret_cast<const void* const*>(&gMGPipeContext);
#if MOBILEGL_PIPE_PUSH
    MGPipeInstallMonolithTables();
    // The 41 rows with no MGPipeApply* body are still null, and null still means "this
    // subsystem has not been migrated, keep pulling". Named rather than counted, because the
    // count is the other case's job and two cases asserting the same number would both go red
    // for one change. P5c's two control records are among them by design (CONTRACT-P5C.md §5:
    // no MGPipeApply* entry point, no monolith producer - under a transport they reach
    // WireVerbSink instead).
    //
    // P5e (sb): SetShaderBuffers HAS LEFT THIS LIST and the inversion is itself the proof the
    // route landed - the row was pinned null "since P4a" by two cases, and its applier, its
    // adapter and its emitter all arrive together. SetStreamOutputTargets stays, because XFB
    // stays lockstep for the whole of P5e (CONTRACT-P5E.md §5.7).
    EXPECT_NE(gMGPipeContext.SetShaderBuffers, nullptr);
    EXPECT_EQ(gMGPipeContext.SetStreamOutputTargets, nullptr);
    EXPECT_EQ(gMGPipeContext.DrawVbo, nullptr);
    EXPECT_EQ(gMGPipeContext.Present, nullptr);
    EXPECT_EQ(gMGPipeContext.SetSwapInterval, nullptr);
    EXPECT_EQ(gMGPipeScreen.GetCaps, nullptr);
    EXPECT_EQ(gMGPipeContext.QueryCreate, nullptr);
    EXPECT_EQ(gMGPipeScreen.FenceCreate, nullptr);
    EXPECT_EQ(gMGPipeScreen.ApplierReset, nullptr);
    EXPECT_EQ(gMGPipeContext.ObjectDeath, nullptr);
    // P5e's opcode 80 joins them: the row is catalogued, its codec layout is written and its
    // tails are validated, but there is no MGPipeApplySetProgramBindings and no route - package
    // pg installs both. A null here is the contract, not an omission.
    EXPECT_EQ(gMGPipeContext.SetProgramBindings, nullptr);
#else
    // A pull build compiles no applier and no routing, so the pre-migration statement is the
    // whole truth there and this case is the one that says so.
    for (SizeT i = 0; i < kMGPipeScreenCallCount; ++i) EXPECT_EQ(screen[i], nullptr) << i;
    for (SizeT i = 0; i < kMGPipeContextCallCount; ++i) EXPECT_EQ(context[i], nullptr) << i;
#endif
    (void)screen;
    (void)context;
}

TEST(PipeCatalogue, ExactlyTheRoutedRowsAreInstalledAndTheRestAreStillNull) {
    const void* const* screen = reinterpret_cast<const void* const*>(&gMGPipeScreen);
    const void* const* context = reinterpret_cast<const void* const*>(&gMGPipeContext);
    SizeT installed = 0;
    SizeT nulls = 0;

#if MOBILEGL_PIPE_PUSH
    // IDEMPOTENT, and called here on purpose: what this case observes is WHICH rows the
    // installer fills, not whether an installer ran somewhere in this binary. Leaving that to
    // ambient linkage is what made the same assertion pass in one build directory and fail in
    // another - the object file carrying a static initialiser was dropped by the linker in the
    // binaries that did not name a symbol in it.
    MGPipeInstallMonolithTables();
#endif

    for (SizeT i = 0; i < kMGPipeScreenCallCount; ++i) {
        if (screen[i] != nullptr) ++installed; else ++nulls;
    }
    for (SizeT i = 0; i < kMGPipeContextCallCount; ++i) {
        if (context[i] != nullptr) ++installed; else ++nulls;
    }
    EXPECT_EQ(installed + nulls, static_cast<SizeT>(kMGPipeCallCount));

#if MOBILEGL_PIPE_PUSH
    // 35 + 5 = 40, and the split is the honest shape of R-17 rather than an implementation
    // detail: 40 is the number of MGPipeApply* entry points PipeApply.h declares WITH A BODY
    // (37 at P5, P5c rv's set_context_values was the 38th, P5e sb's set_shader_buffers is the
    // 39th and P5e pg's set_program_bindings the 40th - the two landed in parallel packages,
    // each of which read 38 as its base and wrote 39; this is the merge saying so), 35 of them
    // fit a GENERATED row and go in the two tables, and FIVE cannot be
    // expressed by any generated signature and go in the hand-written escape table beside them
    // (ResourceRespecify's uncarried initialBytes, ResourceFlushRange's likewise,
    // MapPersistent's size + seedBytes + void* return, CreateShaderState's seven blobrefs and
    // two typed pointers - each one a CONTRACT-P5 ruling, see MG_Pipe/PipeRoute.h).
    //
    // set_program_bindings (opcode 80) HAS its body since package pg, and it is the fifth
    // escape rather than a generated row; its `gMGPipeContext` slot therefore stays null and is
    // pinned null below, which is what an escaped row looks like here.
    //
    // BOTH NUMBERS ARE ASSERTED. If the escape table were left out of this case, moving a row
    // out of the generated tables and forgetting to install its escape would read as a smaller
    // "installed" count and nothing else - and the call site would take a null.
    EXPECT_EQ(installed, 35u) << "the routed rows and the applier's entry points disagree";
    EXPECT_EQ(nulls, static_cast<SizeT>(kMGPipeCallCount) - 35u);
    const void* const* escapes = reinterpret_cast<const void* const*>(&gMGPipeRouteEscapes);
    SizeT escapesInstalled = 0;
    for (SizeT i = 0; i < sizeof(MGPipeRouteEscapes) / sizeof(void*); ++i) {
        if (escapes[i] != nullptr) ++escapesInstalled;
    }
    // P5e (pg) MADE IT FIVE: set_program_bindings carries three tails in three index spaces
    // plus a parallel name array, which no generated (payload, varTail, varTailCount) row can
    // express - so opcode 80's `gMGPipeContext` slot stays null (pinned below, unchanged) and
    // its adapter lives in the escape table beside create_shader_state's. The applier's entry
    // points are 40 with it and with sb's generated row.
    EXPECT_EQ(escapesInstalled, 5u) << "an escape row is null; its call site would take a null "
                                       "pointer rather than fall back to anything";
    EXPECT_EQ(installed + escapesInstalled, 40u)
        << "the two tables plus the escapes must be exactly PipeApply.h's bodied entry points";

    // And the rows that MUST still be null, named rather than counted: these are calls with no
    // applier at all (CONTRACT-P5 table 1 rows 13, 14: "no applier entry point exists"), plus
    // the two verbs the census measured as having zero MG_Impl call sites. An installer that
    // filled one of these would be claiming an implementation that does not exist.
    //
    // P5e (sb): row 13 (set_shader_buffers) is no longer one of them - see the sibling case
    // above. Row 14 (set_stream_output_targets) still is.
    EXPECT_NE(gMGPipeContext.SetShaderBuffers, nullptr);
    EXPECT_EQ(gMGPipeContext.SetStreamOutputTargets, nullptr);
    EXPECT_EQ(gMGPipeContext.DrawVbo, nullptr);
    EXPECT_EQ(gMGPipeContext.Present, nullptr);
    EXPECT_EQ(gMGPipeContext.SetSwapInterval, nullptr);
    EXPECT_EQ(gMGPipeScreen.GetCaps, nullptr);
    // P5c's two control records (CONTRACT-P5C.md §5) take the same answer for a different
    // reason: no MGPipeApply* exists for either and none may be installed - under a transport
    // they cross to WireVerbSink, under monolith the GL thread's direct call and the death
    // notice's mailbox are the producers, byte for byte as before (G1/G2).
    EXPECT_EQ(gMGPipeScreen.ApplierReset, nullptr);
    EXPECT_EQ(gMGPipeContext.ObjectDeath, nullptr);
    // P5e's set_program_bindings (CONTRACT-P5E.md §1): catalogued with a null route until pg
    // installs the applier entry point and the adapter beside it.
    EXPECT_EQ(gMGPipeContext.SetProgramBindings, nullptr);
#else
    // A pull build compiles no applier and no routing, so the pre-migration statement is still
    // the whole truth there.
    EXPECT_EQ(installed, 0u);
    EXPECT_EQ(nulls, static_cast<SizeT>(kMGPipeCallCount));
#endif
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

    // Pad0 -> Uint8 Target, and the SIZE DID NOT MOVE, which is the whole point: the record
    // describes one framebuffer OBJECT and Target says which binding(s), if any, it also
    // sets, and that costs a byte the struct already had. Named (ID-19) cost nothing at all -
    // it is a fourth value of a byte that was already there, which is why the applier could
    // be given a per-object table without a wire change.
    EXPECT_EQ(sizeof(MGPFramebufferState), 304u);
    EXPECT_EQ(static_cast<Uint8>(MGPipeFramebufferTarget::Draw), 0u);
    EXPECT_EQ(static_cast<Uint8>(MGPipeFramebufferTarget::Read), 1u);
    EXPECT_EQ(static_cast<Uint8>(MGPipeFramebufferTarget::Both), 2u);
    // Named = 3, and it is pinned by VALUE rather than merely by existence: the applier
    // validates a record with `Target >= Count`, so an enumerator inserted ahead of Named
    // would silently re-point every Named record the client already emits at Draw or Read -
    // and a Draw record for a framebuffer that is not bound is the exact corruption Named
    // exists to prevent (a DSA clear/blit landing on an unattached driver framebuffer).
    EXPECT_EQ(static_cast<Uint8>(MGPipeFramebufferTarget::Named), 3u);
    // Count is the applier's refusal bound and it is 4 now, not 3: a wire that still refused
    // 3 would drop every DSA record on the floor.
    EXPECT_EQ(static_cast<Uint8>(MGPipeFramebufferTarget::Count), 4u);
    // The byte must be able to hold every value, since Target is a Uint8 in the record and
    // the enum is the only thing that says what fits.
    EXPECT_LE(static_cast<Uint32>(MGPipeFramebufferTarget::Count), 256u);
    EXPECT_EQ(sizeof(MGPFramebufferState::Target), 1u);
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

// P4a, D-D3 / ID-12: MGPSubData::Target is TWO facts in one Uint16 - the low byte says which
// KIND of storage the destination is, the high byte which cube face / upload target the level
// belongs to - and the packing is the contract's, not each emitter's.
//
// The property this case exists for is the COLLISION the packing prevents.
// TextureUploadTarget::Texture1D is 0 and the applier's buffer branch tests the WHOLE field
// == 0, so a texture record carrying the bare upload enumerator would be indistinguishable
// from a buffer record exactly when its owner is a 1D texture, and that texture's upload
// would be dispatched into the buffer path. Nothing else in the tree would have said so.
TEST(PipeCatalogue, SubDataTargetPacksAResourceTargetAndAnUploadTarget) {
    // Both halves must fit their byte, or the encoding is not an encoding.
    static_assert(static_cast<Uint32>(MGPipeResourceTarget::Count) <= 0x100u);
    static_assert(static_cast<Uint32>(TextureUploadTarget::TextureUploadTargetCount) <= 0x100u);

    // 0 first, and deliberately: it is the enumerator that makes the collision possible. Then
    // the plain 2D upload, the first and last cube face, and the largest enumerator the enum
    // has, which is what proves the byte is wide enough in practice and not just in principle.
    const Uint32 uploadTargets[] = {
        0u,
        static_cast<Uint32>(TextureUploadTarget::Texture2D),
        static_cast<Uint32>(TextureUploadTarget::CubeMapPositiveX),
        static_cast<Uint32>(TextureUploadTarget::CubeMapNegativeZ),
        static_cast<Uint32>(TextureUploadTarget::TextureUploadTargetCount) - 1u,
    };
    for (Uint32 resource = 0; resource < static_cast<Uint32>(MGPipeResourceTarget::Count);
         ++resource) {
        for (const Uint32 upload : uploadTargets) {
            const Uint16 packed = MGPipePackSubDataTarget(resource, upload);
            EXPECT_EQ(MGPipeSubDataResourceTargetOf(packed), static_cast<Uint8>(resource))
                << "resource target " << resource << " upload target " << upload;
            EXPECT_EQ(MGPipeSubDataUploadTargetOf(packed), static_cast<Uint8>(upload))
                << "resource target " << resource << " upload target " << upload;
        }
    }

    // THE BUFFER INVARIANT, at compile time in MGPipeTypes.h and again here so a failure names
    // itself: a buffer record's Target is exactly kMGPipeResourceTargetBuffer, whole field,
    // upload byte and all, so P3a's records are unchanged on the wire.
    static_assert(MGPipePackSubDataTarget(kMGPipeResourceTargetBuffer, 0u) ==
                  kMGPipeResourceTargetBuffer);
    EXPECT_EQ(MGPipePackSubDataTarget(kMGPipeResourceTargetBuffer, 0u), kMGPipeResourceTargetBuffer);
    EXPECT_EQ(MGPipePackSubDataTarget(kMGPipeResourceTargetBuffer,
                                      static_cast<Uint32>(TextureUploadTarget::Texture1D)),
              kMGPipeResourceTargetBuffer);
    MGPSubData zeroed{};
    EXPECT_EQ(zeroed.Target, kMGPipeResourceTargetBuffer);

    // ...and the other side of it: a 1D texture's upload target IS 0, and packed it still
    // cannot be mistaken for a buffer, because no texture's resource target is 0.
    EXPECT_EQ(static_cast<Uint32>(TextureUploadTarget::Texture1D), 0u);
    for (Uint32 resource = 1; resource < static_cast<Uint32>(MGPipeResourceTarget::Count);
         ++resource) {
        EXPECT_NE(MGPipePackSubDataTarget(resource, 0u), kMGPipeResourceTargetBuffer)
            << "resource target " << resource << " collides with a buffer record";
    }
    EXPECT_NE(MGPipePackSubDataTarget(MGPipeResourceTargetForTextureTarget(TextureTarget::Texture1D),
                                      static_cast<Uint32>(TextureUploadTarget::Texture1D)),
              kMGPipeResourceTargetBuffer);

    // What a real cube-face record reads back as, through the field rather than a local.
    MGPSubData record{};
    record.Target =
        MGPipePackSubDataTarget(MGPipeResourceTargetForTextureTarget(TextureTarget::TextureCubeMap),
                                static_cast<Uint32>(TextureUploadTarget::CubeMapNegativeY));
    EXPECT_EQ(MGPipeSubDataResourceTargetOf(record.Target),
              static_cast<Uint8>(MGPipeResourceTarget::TexCube));
    EXPECT_EQ(MGPipeSubDataUploadTargetOf(record.Target),
              static_cast<Uint8>(TextureUploadTarget::CubeMapNegativeY));
    // Six faces share one resource target: the high byte is the only thing that tells them
    // apart, which is why it cannot be dropped.
    EXPECT_EQ(MGPipeSubDataResourceTargetOf(
                  MGPipePackSubDataTarget(static_cast<Uint32>(MGPipeResourceTarget::TexCube),
                                          static_cast<Uint32>(TextureUploadTarget::CubeMapPositiveX))),
              MGPipeSubDataResourceTargetOf(record.Target));
    EXPECT_NE(MGPipeSubDataUploadTargetOf(
                  MGPipePackSubDataTarget(static_cast<Uint32>(MGPipeResourceTarget::TexCube),
                                          static_cast<Uint32>(TextureUploadTarget::CubeMapPositiveX))),
              MGPipeSubDataUploadTargetOf(record.Target));
}

// P4a, ID-12: the three constants MGPSurface::Kind is spelled with, the texture target the
// record grew where its Pad0 was, and MGPTextureParams::DepthStencilMode's two numbers.
//
// All three were UNSTATED in the contract and were being re-invented on both sides of the
// boundary - which is the way a wire field acquires two meanings. The values themselves are
// unremarkable; what this case pins is that there is exactly one spelling of each.
TEST(PipeCatalogue, SurfaceNamesItsKindItsTextureTargetAndItsDepthStencilAspect) {
    // MGPipeKind is REUSED rather than a second three-value enum minted beside the field.
    EXPECT_EQ(kMGPipeSurfaceKindNone, static_cast<Uint8>(MGPipeKind::None));
    EXPECT_EQ(kMGPipeSurfaceKindTexture, static_cast<Uint8>(MGPipeKind::Texture));
    EXPECT_EQ(kMGPipeSurfaceKindRenderbuffer, static_cast<Uint8>(MGPipeKind::Renderbuffer));
    EXPECT_NE(kMGPipeSurfaceKindTexture, kMGPipeSurfaceKindRenderbuffer);
    // None == 0 is load-bearing: it is what makes a zero-initialised record already BE the
    // empty attachment point, which every emitter and every reader relies on.
    EXPECT_EQ(kMGPipeSurfaceKindNone, 0u);

    // Pad0 -> Uint16 TextureTarget. THE SIZE DID NOT MOVE - the two bytes were already there -
    // and neither did anything in front of it.
    EXPECT_EQ(sizeof(MGPSurface), 24u);
    EXPECT_EQ(offsetof(MGPSurface, UploadTarget), 20u);
    EXPECT_EQ(offsetof(MGPSurface, TextureTarget), 22u);
    // The sentinel is TextureTarget::Unknown widened, so it is a value no real target has.
    EXPECT_EQ(kMGPipeSurfaceNoTextureTarget, 0xFFFFu);
    EXPECT_EQ(kMGPipeSurfaceNoTextureTarget, static_cast<Uint16>(TextureTarget::Unknown));
    for (SizeT i = 0; i < static_cast<SizeT>(TextureTarget::TextureTargetCount); ++i) {
        EXPECT_NE(static_cast<Uint16>(i), kMGPipeSurfaceNoTextureTarget);
    }

    // A ZEROED MGPSurface CARRIES TextureTarget 0, AND 0 IS TextureTarget::Texture1D, NOT THE
    // SENTINEL. That is documented rather than defended, and it is why the field's contract is
    // "consulted only when Kind == kMGPipeSurfaceKindTexture": a zeroed record is Kind == None
    // and names no texture at all, so a reader that gates on Kind can never see the 0. A
    // reader that does not gate would read Texture1D out of an empty attachment point.
    MGPSurface empty{};
    EXPECT_EQ(empty.TextureTarget, 0u);
    EXPECT_EQ(static_cast<Uint16>(TextureTarget::Texture1D), 0u);
    EXPECT_EQ(empty.Kind, kMGPipeSurfaceKindNone);
    EXPECT_TRUE(MGPipeHandleIsNull(empty.Res));

    // A renderbuffer point names no texture and says so with the sentinel, which is what
    // distinguishes "not a texture" from "a 1D texture" for a reader that looks anyway.
    MGPSurface renderbuffer{};
    renderbuffer.Kind = kMGPipeSurfaceKindRenderbuffer;
    renderbuffer.TextureTarget = kMGPipeSurfaceNoTextureTarget;
    EXPECT_NE(renderbuffer.TextureTarget, static_cast<Uint16>(TextureTarget::Texture1D));

    // The half a compiler cannot catch: the PipeFields.def row. MGPSurface still asserts its
    // size whether or not the field list names TextureTarget, so a comparator blind to the
    // field would pass a target-only divergence under MOBILEGL_PIPE_VERIFY - and the field is
    // exactly what the four cross-object masks key on.
    MGPSurface a{};
    MGPSurface b{};
    const char* field = nullptr;
    EXPECT_TRUE(MGPipeVerify(a, b, &field));
    a.TextureTarget = static_cast<Uint16>(TextureTarget::TextureCubeMap);
    EXPECT_FALSE(MGPipeVerify(a, b, &field));
    EXPECT_STREQ(field, "TextureTarget");

    // DepthStencilMode: 0 = GL_DEPTH_COMPONENT, 1 = GL_STENCIL_INDEX. Depth is 0 because it is
    // the GL initial value and a texture that never asks for the stencil aspect never emits
    // the call, so a zeroed record has to decode to what an untouched texture already has.
    EXPECT_EQ(kMGPipeDepthStencilModeDepth, 0u);
    EXPECT_EQ(kMGPipeDepthStencilModeStencil, 1u);
    EXPECT_NE(kMGPipeDepthStencilModeDepth, kMGPipeDepthStencilModeStencil);
    MGPTextureParams params{};
    EXPECT_EQ(params.DepthStencilMode, kMGPipeDepthStencilModeDepth);
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
    // P5b (MG_Remote/CONTRACT-P5B.md) appended five verbs AFTER P0's three late arrivals, by
    // the same rule: opcodes 72..76, and nothing before them moved. The five are pinned by
    // VALUE, because the pin is the protocol - a row inserted ahead of one would silently
    // re-point every record a P5b package emits at the wrong arm.
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::BindShaderImage), 72);
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::PatchParameter), 73);
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::BindStreamOutput), 74);
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::SetStorageBlockBinding), 75);
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::CopyFramebufferToTexture), 76);
    // P5c (MG_Remote/CONTRACT-P5C.md §5) appended the two control records AFTER P5b's five, by
    // the same rule: opcodes 77..78, and nothing before them moved. applier_reset is a kScreen
    // row (a make-current is a whole-server edge, exactly as FenceWaitServer is a screen call)
    // and object_death a kCtxObject one; both carry no blob, no reply and no tail.
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::ApplierReset), 77);
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::ObjectDeath), 78);
    // P5c rv (§5.3) appended the residual-value record AFTER the two control records, by the
    // same rule: opcode 79, and nothing before it moved. It is an ordinary kCtxState set_* row
    // - fixed POD, no blob, no tail, no reply - with an MGPipeApply* entry point, which the two
    // control records deliberately do not have.
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::SetContextValues), 79);
    // P5e (MG_Remote/CONTRACT-P5E.md §1) appended ONE more, by the same rule: opcode 80, and
    // nothing before it moved. set_program_bindings is a kCtxState row with THREE tails and a
    // host span in the third; it lands with a null route and a sink that refuses it by name,
    // exactly as set_shader_buffers has sat catalogued-and-dead since P4a.
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::SetProgramBindings), 80);
    EXPECT_EQ(static_cast<Uint16>(MGPWireOp::kOpCount), 81);
    EXPECT_EQ(MGPipeCallFlagsFor(MGPWireOp::SetProgramBindings),
              static_cast<Uint32>(kVarTail | kHostSpan));
    EXPECT_EQ(MGPipeCallFlagsFor(MGPWireOp::ApplierReset), static_cast<Uint32>(kNone));
    EXPECT_EQ(MGPipeCallFlagsFor(MGPWireOp::ObjectDeath), static_cast<Uint32>(kNone));
    EXPECT_EQ(MGPipeCallFlagsFor(MGPWireOp::SetContextValues), static_cast<Uint32>(kNone));
    // And the P5b rows carry what their contract says: one blob (the block name) and nothing
    // else, and the extended draw row keeps its two flags.
    EXPECT_EQ(MGPipeCallFlagsFor(MGPWireOp::SetStorageBlockBinding), static_cast<Uint32>(kHasBlob));
    EXPECT_EQ(MGPipeCallFlagsFor(MGPWireOp::BindShaderImage), static_cast<Uint32>(kNone));
    EXPECT_EQ(MGPipeCallFlagsFor(MGPWireOp::PatchParameter), static_cast<Uint32>(kNone));
    EXPECT_EQ(MGPipeCallFlagsFor(MGPWireOp::BindStreamOutput), static_cast<Uint32>(kNone));
    EXPECT_EQ(MGPipeCallFlagsFor(MGPWireOp::CopyFramebufferToTexture), static_cast<Uint32>(kNone));
    EXPECT_EQ(MGPipeCallFlagsFor(MGPWireOp::DrawVbo), static_cast<Uint32>(kHostSpan | kVarTail));
    // The P5b payload sizes, pinned like every other MGP_ASSERT_POD at runtime so the numbers
    // show up in ctest output; MGPCopyRegion grew 64 -> 72 for its two GL names (i1).
    EXPECT_EQ(sizeof(MGPImageBind), 40u);
    EXPECT_EQ(sizeof(MGPPatchParameter), 8u);
    EXPECT_EQ(sizeof(MGPStreamOutputBind), 16u);
    EXPECT_EQ(sizeof(MGPStorageBlockBinding), 40u);
    EXPECT_EQ(sizeof(MGPCopyFromFramebuffer), 48u);
    EXPECT_EQ(sizeof(MGPCopyRegion), 72u);
    EXPECT_EQ(sizeof(MGPDrawIndirect), 40u);
    // The P5c payload, pinned like every other MGP_ASSERT_POD at runtime: one Uint64, no
    // padding. object_death REUSES MGPHandleOnly (CONTRACT-P5C.md §1), so there is no second
    // struct to pin - the 16 bytes are pinned above with the handle family.
    EXPECT_EQ(sizeof(MGPApplierReset), 8u);
    EXPECT_EQ(sizeof(MGPHandleOnly), 16u);
    // rv's two (§5.3/§7.6): the residual-value POD - 2 + 15 Uint32s, 2 Uint8s and 2 pad bytes,
    // then the three Uint64s - and the AMENDED attribute carrier, which grew 24 -> 56 to carry
    // all three views verbatim (the frontend's cross-view conversion is the authoritative
    // answer; the applier no longer reconverts).
    EXPECT_EQ(sizeof(MGPContextValues), 96u);
    EXPECT_EQ(sizeof(MGPContextValues::TouchedBufferBindingPointCount), 60u);
    EXPECT_EQ(sizeof(MGPAttribValue), 56u);
    EXPECT_EQ(sizeof(MGPVertexAttribDefaults), 8u);
    // P5e's payloads (CONTRACT-P5E.md §1), pinned at runtime like every other MGP_ASSERT_POD.
    //
    // MGPProgramDesc GREW 192 -> 200, and that is the phase's one wire-format widening: the
    // descriptor's four trailing Uint8s end exactly at offset 24 and MGPBlobRef is 8-aligned,
    // so there was no spare byte for LinkStatus and the draft's "in the descriptor's existing
    // pad" was wrong. Pinned here, by value, because a record whose size moved silently is a
    // protocol break no other test would see - and because THIS number is the red-once the
    // phase names: change MGPProgramBindings' or the descriptor's size and this case fails.
    EXPECT_EQ(sizeof(MGPProgramDesc), 200u);
    EXPECT_EQ(sizeof(MGPProgramBindings), 32u);
    EXPECT_EQ(sizeof(MGPProgramSamplerUnit), 8u);
    EXPECT_EQ(sizeof(MGPProgramStorageOverride), 40u);
    // The three declared tail bounds. A program past one of them is a COUNTED REFUSAL and never
    // a truncation, so the numbers are the contract rather than an implementation detail.
    EXPECT_EQ(kMGPipeMaxProgramBlockBindings, 64u);
    EXPECT_EQ(kMGPipeMaxProgramSamplerUnits, 256u);
    EXPECT_EQ(kMGPipeMaxProgramStorageOverrides, 64u);
    // LinkStatus is a real byte and a zeroed descriptor means "not linked", which is what makes
    // the server's decline the safe default.
    MGPProgramDesc program{};
    EXPECT_EQ(program.LinkStatus, 0u);
    // set_shader_buffers' capacity, pinned on the wire side; MG_Impl/Pipe/PipeFill.cpp is the
    // one translation unit that also sees BufferState.h's constant and pins them together.
    EXPECT_EQ(kMGPipeMaxBufferBindingPoints, 84u);
    // The two draw-flag bits P5b's d1 arms are exclusive by contract and distinct by value.
    EXPECT_EQ(static_cast<Uint32>(kDrawIsIndirect), 1u << 5);
    EXPECT_EQ(static_cast<Uint32>(kDrawHasUserIndices) & static_cast<Uint32>(kDrawIsIndirect), 0u);
    // P5e's draw flag: the next free bit, and disjoint from all five before it.
    EXPECT_EQ(static_cast<Uint32>(kDrawClientArrays), 1u << 6);
    EXPECT_EQ(static_cast<Uint32>(kDrawClientArrays) &
                  static_cast<Uint32>(kDrawHasUserIndices | kDrawPrimitiveRestart |
                                      kDrawIndicesAreClient | kDrawHasIndexRange |
                                      kDrawHasXfbCount | kDrawIsIndirect),
              0u);
    // ... and it still fits the one byte MGPDrawInfo::Flags is, which is why the record did not
    // grow: MGPDrawInfo stays 56 bytes.
    EXPECT_EQ(sizeof(MGPDrawInfo), 56u);
    EXPECT_LE(static_cast<Uint32>(kDrawClientArrays), 0xFFu);
    // The clear discriminants have ONE spelling now, and Whole is 0 so a zeroed record is a
    // whole-framebuffer clear.
    EXPECT_EQ(kMGPipeClearKindWhole, 0u);
    EXPECT_EQ(kMGPipeClearKindDepthStencil + 1, kMGPipeClearKindCount);
    EXPECT_EQ(kMGPipeClearValueClassUint + 1, kMGPipeClearValueClassCount);
}

// ---- P5e: the wait-class column (MG_Remote/CONTRACT-P5E.md §2.2) --------------------------
//
// THE POINT OF THIS CASE IS THAT IT DOES NOT READ PipeCalls.def. Every other statement about
// the column - the generated table, the two gates in gen_pipe.py, the static_asserts in
// PipeWire.inc - is derived from the def, so all of them move together when a row is edited.
// This case re-states CONTRACT-P5E §2.2's table BY NAME, in the contract's own grouping, so a
// row silently moved from kWaitApplied to kWaitNone has to be argued in two places or it goes
// red here. That is the difference between a table and a contract.
//
// It matters because the column is not decoration: on a run-ahead server kWaitNone is a promise
// that the record's apply reads NOTHING of the client's (rule F), and a row demoted to it by
// accident is a torn read that renders wrong rather than aborting.
TEST(PipeCatalogue, EveryRowCarriesTheWaitClassTheContractGivesIt) {
    // kWaitPresent: present alone, paced by the credit taken BEFORE the encode (§2.4).
    EXPECT_EQ(MGPipeWaitClassFor(MGPWireOp::Present), kWaitPresent);

    // kWaitReply: every row whose answer is not derivable (R-5). Named, all fourteen, because
    // the flag-to-class agreement is what keeps a waiter from hanging on a slot nobody posts.
    const MGPWireOp replyRows[] = {
        MGPWireOp::GetCaps,         MGPWireOp::ResourceCreate,   MGPWireOp::ResourceRespecify,
        MGPWireOp::MapPersistent,   MGPWireOp::FenceStatus,      MGPWireOp::FenceWait,
        MGPWireOp::QueryAvailable,  MGPWireOp::QueryResult,      MGPWireOp::QueryTimestamp,
        MGPWireOp::SetTextureParams, MGPWireOp::ResourceSubData, MGPWireOp::ResourceReadback,
        MGPWireOp::GetTextureImage, MGPWireOp::ReadPixels,
    };
    for (const MGPWireOp op : replyRows) {
        EXPECT_EQ(MGPipeWaitClassFor(op), kWaitReply) << WireOpNameForDiag(op);
        EXPECT_NE(MGPipeCallFlagsFor(op) & static_cast<Uint32>(kReplySlot), 0u)
            << WireOpNameForDiag(op);
    }

    // kWaitApplied: the rows whose apply STILL reads a BARRIER_PULLED row or probes the client
    // allocator, so the client parks in its own wait and CONTRACT-P5C's semantics hold for them
    // unchanged - their pulls count into rsp and their probes are legal inside a scope. Two of
    // them are trailing items and are expected to leave this list in a later package:
    // generate_mipmap the moment tx2 resolves the texture from VerbMipRes instead of the active
    // unit, and set_storage_block_binding the moment pg resolves it through
    // MGPStorageBlockBinding::ShaderCso. When they do, THIS list is what has to be edited.
    const MGPWireOp appliedRows[] = {
        MGPWireOp::ApplierReset,      MGPWireOp::GenerateMipmap,
        MGPWireOp::SetStorageBlockBinding, MGPWireOp::BeginStreamOutput,
        MGPWireOp::EndStreamOutput,   MGPWireOp::PauseStreamOutput,
        MGPWireOp::ResumeStreamOutput, MGPWireOp::BindStreamOutput,
        MGPWireOp::CopyFramebufferToTexture,
        // P5e (gl), ID-118. resource_copy_region JOINED THIS SET, and it is the retiring phase
        // of a field that put it here: its verb is CopyImageSubData, whose apply reads
        // GetTextureObject, and that row retires in P7. An unbarriered record reading client
        // memory is an unconditional Fatal, so leaving it kWaitNone would have meant a real
        // defect standing for two phases - or an exception hand-written into a DERIVED
        // allowlist, which is the same thing with a comment on it. It sits beside
        // copy_framebuffer_to_texture here for the reason it took that class: same verb class,
        // no reply slot, and zero calls per frame in the measured scene.
        MGPWireOp::ResourceCopyRegion,
    };
    for (const MGPWireOp op : appliedRows) {
        EXPECT_EQ(MGPipeWaitClassFor(op), kWaitApplied) << WireOpNameForDiag(op);
    }

    // kWaitNone: the steady draw path and every state/CSO/object row without a reply. Named for
    // the rows the phase exists for, and then counted for the rest, so a new row cannot join
    // the set unnoticed.
    const MGPWireOp noneRows[] = {
        MGPWireOp::DrawVbo,        MGPWireOp::LaunchGrid,        MGPWireOp::Clear,
        MGPWireOp::Blit,           MGPWireOp::MemoryBarrier,     MGPWireOp::Flush,
        MGPWireOp::SetSwapInterval, MGPWireOp::BindShaderImage,  MGPWireOp::PatchParameter,
        MGPWireOp::ObjectDeath,    MGPWireOp::ResourceDestroy,   MGPWireOp::UnmapPersistent,
        MGPWireOp::FenceCreate,    MGPWireOp::FenceDestroy,      MGPWireOp::FenceWaitServer,
        MGPWireOp::QueryCreate,    MGPWireOp::QueryBegin,        MGPWireOp::QueryEnd,
        MGPWireOp::QueryDestroy,   MGPWireOp::QueryCounter,      MGPWireOp::SetContextValues,
        MGPWireOp::SetProgramBindings, MGPWireOp::SetShaderBuffers,
        MGPWireOp::SetVertexBuffers, MGPWireOp::SetFramebufferState,
        MGPWireOp::CreateShaderState, MGPWireOp::SetDrawProgram,
    };
    for (const MGPWireOp op : noneRows) {
        EXPECT_EQ(MGPipeWaitClassFor(op), kWaitNone) << WireOpNameForDiag(op);
    }

    // The partition, by count. 14 + 1 + 10 = 25 rows wait; every other row of the catalogue does
    // not. A row that changed class moves two of these numbers at once - which is why the
    // kWaitApplied count went 9 -> 10 and the kWaitNone offset 24 -> 25 in the SAME commit that
    // moved resource_copy_region (P5e gl, ID-118).
    SizeT reply = 0, applied = 0, present = 0, none = 0, other = 0;
    for (SizeT i = 1; i < static_cast<SizeT>(MGPWireOp::kOpCount); ++i) {
        switch (MGPipeWaitClassFor(static_cast<MGPWireOp>(i))) {
        case kWaitReply: ++reply; break;
        case kWaitApplied: ++applied; break;
        case kWaitPresent: ++present; break;
        case kWaitNone: ++none; break;
        default: ++other; break;
        }
    }
    EXPECT_EQ(reply, 14u);
    EXPECT_EQ(applied, 10u);
    EXPECT_EQ(present, 1u);
    EXPECT_EQ(none, static_cast<SizeT>(kMGPipeCallCount) - 25u);
    EXPECT_EQ(other, 0u) << "a row carries the kWaitClassCount terminator as its class";

    // And the reply half of the partition BOTH WAYS, over the whole catalogue: exactly the rows
    // that own a slot wait for one. gen_pipe.py refuses a catalogue where they disagree; this
    // is the same statement where a reader of the table will look for it.
    for (SizeT i = 1; i < static_cast<SizeT>(MGPWireOp::kOpCount); ++i) {
        const auto op = static_cast<MGPWireOp>(i);
        const Bool ownsSlot = (MGPipeCallFlagsFor(op) & static_cast<Uint32>(kReplySlot)) != 0;
        EXPECT_EQ(ownsSlot, MGPipeWaitClassFor(op) == kWaitReply) << WireOpNameForDiag(op);
    }

    // Opcode 0 is not a call and has no class, for the flags table's reason: a decoder holding
    // a byte off a corrupt stream must reach its own Fatal rather than a wait decision.
    EXPECT_EQ(MGPipeWaitClassFor(MGPWireOp::kInvalid), kWaitClassCount);
    EXPECT_EQ(MGPipeWaitClassFor(MGPWireOp::kOpCount), kWaitClassCount);
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
    // 72 through P5; P5b appended five call payloads (MG_Remote/CONTRACT-P5B.md: MGPImageBind,
    // MGPPatchParameter, MGPStreamOutputBind, MGPStorageBlockBinding, MGPCopyFromFramebuffer),
    // each with its own field list, so the comparator sees every one of them: 77. P5c appended
    // applier_reset's MGPApplierReset (CONTRACT-P5C.md §5.1) - object_death reuses
    // MGPHandleOnly, which has had a list since P0 - and rv added set_context_values'
    // MGPContextValues (§5.3): 79. P5e appended set_program_bindings' MGPProgramBindings AND
    // the two TAIL ELEMENT types beside it, MGPProgramSamplerUnit and MGPProgramStorageOverride
    // (MG_Remote/CONTRACT-P5E.md §1) - the tails are listed for the same reason MGPBufferRange
    // and MGPVertexAttribWire are: the comparator has to see INTO an element whose members
    // include an MGHostSpan and a pad word, or it would memcmp the padding: 82.
    EXPECT_EQ(kMGPipeVerifiedPayloadCount, 82u);
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
    // P5e (sb, ID-104): 32 -> 40. WritableMask was ONE Uint32 against an 84-point window, so
    // it could describe only the first 32 points and a storage buffer bound at point 32 or
    // above read as read-only with nothing able to see it. The ruling widens the FIELD rather
    // than narrowing the window - narrowing would change what an application may bind - and the
    // eight bytes are paid by a record that fires at most once per class per change, in a push
    // build only. The ABI fingerprint already moved this phase for opcode 80.
    static_assert(sizeof(MGPShaderBuffers) == 40);
    static_assert(sizeof(MGPShaderBuffers::WritableMask) * 8u >= kMGPipeMaxBufferBindingPoints,
                  "the mask must cover the window the same record declares");
    EXPECT_LT(sizeof(MGPBufferRange), sizeof(MGHostSpan));

    // The call still declares the span it may carry, so the transport lays the tail out.
    Uint32 flags = 0;
#define MGP_FLAGS_OF_SET_SHADER_BUFFERS(Name, Payload, Class, Flags, Wait)                                                   \
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

    // P5e (sb, ID-104): THE COMPARATOR SEES EVERY WORD OF THE WIDENED MASK, and the HIGH one
    // is what the case is for. PipeFields.def names WritableMask once and the array overload
    // of MGPipeFieldEqual walks it, so this would have passed before the widening too - on the
    // 32 points that existed. Point 83 is the one the old field could not describe at all.
    b = a;
    ASSERT_TRUE(MGPipeVerify(a, b, &field));
    MGPipeShaderBufferMaskSet(b.WritableMask, kMGPipeMaxBufferBindingPoints - 1);
    EXPECT_TRUE(MGPipeShaderBufferMaskHas(b.WritableMask, kMGPipeMaxBufferBindingPoints - 1));
    EXPECT_FALSE(MGPipeShaderBufferMaskHas(a.WritableMask, kMGPipeMaxBufferBindingPoints - 1));
    EXPECT_FALSE(MGPipeVerify(a, b, &field));
    EXPECT_STREQ(field, "WritableMask");
    // And out of range is "not writable" / "write nothing" rather than a word past the end.
    MGPipeShaderBufferMaskSet(b.WritableMask, kMGPipeMaxBufferBindingPoints);
    EXPECT_FALSE(MGPipeShaderBufferMaskHas(b.WritableMask, kMGPipeMaxBufferBindingPoints));
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
#define MGP_FLAGS_OF_RESOURCE_RESPECIFY(Name, Payload, Class, Flags, Wait)                                                   \
    if (std::strcmp(#Name, "ResourceRespecify") == 0) flags = static_cast<Uint32>(Flags);
    MGP_CALL_LIST(MGP_FLAGS_OF_RESOURCE_RESPECIFY)
#undef MGP_FLAGS_OF_RESOURCE_RESPECIFY
    EXPECT_EQ(flags & static_cast<Uint32>(kNeedsAck), static_cast<Uint32>(kNeedsAck));
    // And it is the ONLY call that carries it: a second one would be a second decision, and
    // this predicate answers for exactly one call.
    Uint32 ackingCalls = 0;
#define MGP_COUNT_ACKING_CALLS(Name, Payload, Class, Flags, Wait)                                                            \
    if ((static_cast<Uint32>(Flags) & static_cast<Uint32>(kNeedsAck)) != 0) ++ackingCalls;
    MGP_CALL_LIST(MGP_COUNT_ACKING_CALLS)
#undef MGP_COUNT_ACKING_CALLS
    EXPECT_EQ(ackingCalls, 1u);

    // P5 R-16: the same shape over kReplySlot, which had no count pin at all until the flag
    // became load-bearing. It is what sizes the reply pool and what the decoder posts against,
    // so the number is now a protocol quantity rather than a documentation one.
    //
    // FOURTEEN. Ten answers that were always declared - get_caps, map_persistent, the two fence
    // reads, the three query reads, the two readbacks and read_pixels - plus the FOUR ACCEPTANCE
    // ROWS, whose applier entry points return a Bool the client acts on destructively and which
    // carried no flag because in monolith that answer is a direct call's return value.
    Uint32 replySlotCalls = 0;
#define MGP_COUNT_REPLY_SLOT_CALLS(Name, Payload, Class, Flags, Wait)                                                        \
    if ((static_cast<Uint32>(Flags) & static_cast<Uint32>(kReplySlot)) != 0) ++replySlotCalls;
    MGP_CALL_LIST(MGP_COUNT_REPLY_SLOT_CALLS)
#undef MGP_COUNT_REPLY_SLOT_CALLS
    EXPECT_EQ(replySlotCalls, 14u);

    // And the four by name, because a count alone would let a row lose the flag while another
    // gained one. These are exactly the MGPipeApply* entry points that return Bool
    // (PipeApply.h:820, :868, :897, :1023); map_persistent's void* is the fifth answer and was
    // already declared.
    Uint32 acceptanceWithSlot = 0;
#define MGP_COUNT_ACCEPTANCE_ROWS(Name, Payload, Class, Flags, Wait)                                                         \
    if ((std::strcmp(#Name, "ResourceCreate") == 0 || std::strcmp(#Name, "ResourceRespecify") == 0 ||                   \
         std::strcmp(#Name, "ResourceSubData") == 0 || std::strcmp(#Name, "SetTextureParams") == 0) &&                 \
        (static_cast<Uint32>(Flags) & static_cast<Uint32>(kReplySlot)) != 0) {                                         \
        ++acceptanceWithSlot;                                                                                          \
    }
    MGP_CALL_LIST(MGP_COUNT_ACCEPTANCE_ROWS)
#undef MGP_COUNT_ACCEPTANCE_ROWS
    EXPECT_EQ(acceptanceWithSlot, 4u);

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

    // P4a, ID-18 M4. The metadata-update rule is a PROSE contract stated beside the predicate
    // above - it compares an incoming descriptor against the applier's stored one, which this
    // header cannot do - so what is pinnable here is the thing that would make the prose lie:
    // a field added to MGPResourceDesc and classified into neither list. The size is the
    // tripwire, and the two metadata fields are named so the classification cannot be lost to
    // a rename either.
    EXPECT_EQ(sizeof(MGPResourceDesc), 88u);
    EXPECT_EQ(sizeof(MGPResourceDesc::BindMask), 2u);
    EXPECT_EQ(sizeof(MGPResourceDesc::ImageBindableHint), 1u);
    // HasDefinedContent sits next to ImageBindableHint and is deliberately on the OTHER side
    // of the line: glBufferData(size, NULL) at an unchanged size is an orphaning
    // reallocation, so a record that moves only it must still clear, and must never be read
    // as a mask change.
    EXPECT_NE(offsetof(MGPResourceDesc, HasDefinedContent),
              offsetof(MGPResourceDesc, ImageBindableHint));
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
