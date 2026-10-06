// MobileGL - MobileGL/MG_Test/Pipe/MagmaPipeIdentityTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Magma's server-side program source and run-ahead capability, the parts of this suite that
// outlived the {slot, gen} mint. P13 W6 deleted MagmaPipeIdentityTables and the claim rule
// with the frontend arm's VAO memos they keyed (the record arm keys every object on the
// applier's own {slot, gen}); their four cases went with them (docs/ci/ci-cleanup-log.md).
#include <gtest/gtest.h>

#include "Includes.h"
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/MGPipeHandles.h>

#include <Config.h>
#if MOBILEGL_BUILD_DISAGGREGATED
#include <MG_Backend/DirectVulkan/Renderer/MagmaProgramSource.h>
#endif

using namespace MobileGL;

namespace {
    using namespace MobileGL::MG_Backend::DirectVulkan;
    using MG_Pipe::MGPipeHandle;

    // Historical test name retained. Each backend's readiness is now a real gate:
    // false forbids the capability, true permits it. Production keeps its own
    // Magma readiness constant, which the bootstrap and GPU queue tests also check.
    TEST(MagmaPipeIdentityTest, AMagmaServerNeverPublishesTheRunAheadCapBit) {
        EXPECT_EQ(MG_Pipe::MGPipeRunAheadCapBitsFor(BackendType::DirectVulkan, /*ready=*/false), 0u);
        EXPECT_EQ(MG_Pipe::MGPipeRunAheadCapBitsFor(BackendType::DirectVulkan, /*ready=*/true),
                  static_cast<Uint64>(MG_Pipe::kCapRunAheadApply));
        EXPECT_EQ(MG_Pipe::MGPipeRunAheadCapBitsFor(BackendType::Unknown, /*ready=*/true), 0u);

        // And the Espryt half, so the case says what the arm IS and not only what it is not:
        // the bit is published for DirectGLES and ONLY once the integration commit flips
        // kMGPipeP5eRunAheadReady. Until then every P5e package lands with the wait rule inert.
        EXPECT_EQ(MG_Pipe::MGPipeRunAheadCapBitsFor(BackendType::DirectGLES, /*ready=*/false) &
                      static_cast<Uint64>(MG_Pipe::kCapRunAheadApply),
                  0u);
        EXPECT_EQ(MG_Pipe::MGPipeRunAheadCapBitsFor(BackendType::DirectGLES, /*ready=*/true) &
                      static_cast<Uint64>(MG_Pipe::kCapRunAheadApply),
                  static_cast<Uint64>(MG_Pipe::kCapRunAheadApply));

        // Bit 10 and nothing else: the arm contributes ONE bit, so a future editor cannot fold
        // an unrelated capability into it and have the two pins above still pass.
        EXPECT_EQ(MG_Pipe::MGPipeRunAheadCapBitsFor(BackendType::DirectGLES, /*ready=*/true),
                  static_cast<Uint64>(MG_Pipe::kCapRunAheadApply));
        static_assert(static_cast<Uint64>(MG_Pipe::kCapRunAheadApply) == (1ull << 10),
                      "kCapRunAheadApply moved bit; CONTRACT-P5E table 0 names bit 10");
    }

    TEST(MagmaProgramSourceTest, ServerBindingTailsReplaceLinkTimeDefaults) {
#if MOBILEGL_BUILD_DISAGGREGATED
        auto archive = MakeShared<MG_State::GLState::ProgramArchive>();
        archive->Link.glBlockIndexToTProgram = {0};
        archive->Link.blockReflection.resize(1);
        archive->Link.blockReflection[0].name = "Block";
        archive->Link.blockReflection[0].size = 12;
        archive->Link.uniformBlockBinding = {9};
        archive->Link.uniformSamplerOrImageUnitIndex = {12, 13};
        archive->Spirv.globalUboScratch = {99, 98};
        MG_Pipe::MGPipeShaderCsoRecord record{};
        record.Archive = archive;
        record.BlockBindings = {3};
        record.SamplerUnits = {{0, 4}};
        record.GlobalConstants = {5, 6};
        record.GlobalConstantsVersion = 11;
        const MagmaProgramSource source({7, 2}, record);
        EXPECT_EQ(source.GetUniformBlockBinding(0), 3u);
        EXPECT_EQ(source.GetUniformBlockName(0), "Block");
        EXPECT_EQ(source.GetUBOSizeAt(0), 16u);
        EXPECT_EQ(source.GetUniformSamplerOrImageUnitIndex(0), 4);
        EXPECT_EQ(source.GetUniformSamplerOrImageUnitIndex(1), -1)
            << "an absent tail entry must not resurrect the archive's stale unit";
        ASSERT_EQ(source.GetUBOSize(), 2u);
        EXPECT_EQ(static_cast<const Uint8*>(source.GetUBOData())[0], 5u);
        EXPECT_EQ(source.GetUBOContentVersion(), 11u);
        record.BlockBindings[0] = 6;
        record.SamplerUnits[0].Unit = 8;
        record.GlobalConstants[0] = 42;
        record.GlobalConstantsVersion = 12;
        EXPECT_EQ(source.GetUniformBlockBinding(0), 6u);
        EXPECT_EQ(source.GetUniformSamplerOrImageUnitIndex(0), 8);
        EXPECT_EQ(static_cast<const Uint8*>(source.GetUBOData())[0], 42u);
        EXPECT_EQ(source.GetUBOContentVersion(), 12u);
#else
        GTEST_SKIP() << "server program sources require the disaggregated build";
#endif
    }

    TEST(MagmaProgramSourceTest, ArrayUniformNamesResolveWithinTheArchivedUniform) {
#if MOBILEGL_BUILD_DISAGGREGATED
        auto archive = MakeShared<MG_State::GLState::ProgramArchive>();
        auto& link = archive->Link;
        link.maxUniformLocation = 2;
        link.uniformLocations["arr[0]"] = 0;
        link.uniformIndexInTProgram = {0, 0, 0};
        link.tProgramUniformIndexToGl = {0};
        link.uniformReflection.resize(1);
        link.uniformReflection[0].type.isArray = true;
        link.uniformReflection[0].arraySize = 3;
        link.uniformReflection[0].glDefineType = GL_SAMPLER_2D;
        MG_Pipe::MGPipeShaderCsoRecord record{};
        record.Archive = archive;
        const MagmaProgramSource source({8, 1}, record);
        EXPECT_EQ(source.GetUniformLocation("arr"), 0);
        EXPECT_EQ(source.GetUniformLocation("arr[2]"), 2);
        EXPECT_EQ(source.GetUniformLocation("arr[3]"), -1);
        EXPECT_EQ(source.GetUniformLocation("arr[-1]"), -1);
        EXPECT_EQ(source.GetUniformLocation("arr[999999999999999]"), -1);
        EXPECT_EQ(source.GetUniformLocation("unknown"), -1);
        EXPECT_TRUE(source.UniformLocationsAliasSameUniform(0, 2));
        EXPECT_FALSE(source.UniformLocationsAliasSameUniform(0, 3));
        EXPECT_EQ(source.GetUniformType(2), GL_SAMPLER_2D);
#else
        GTEST_SKIP() << "server program sources require the disaggregated build";
#endif
    }

    TEST(MagmaProgramSourceTest, ReusedHandlesAndRecordVersionsHaveDistinctCacheIdentity) {
#if MOBILEGL_BUILD_DISAGGREGATED
        MG_Pipe::MGPipeShaderCsoRecord record{};
        record.Archive = MakeShared<MG_State::GLState::ProgramArchive>();
        record.Serial = 17;
        record.BindingsSerial = 21;
        record.GlobalConstantsVersion = 25;
        const MagmaProgramSource first({9, 2}, record);
        const MagmaProgramSource recycled({9, 3}, record);
        EXPECT_NE(first.GetLifetimeId(), recycled.GetLifetimeId());
        EXPECT_EQ(first.Handle(), (MG_Pipe::MGPipeHandle{9, 2}));
        const auto identity = first.GetLifetimeId();
        record.Serial = 18;
        record.BindingsSerial = 22;
        record.GlobalConstantsVersion = 26;
        EXPECT_EQ(first.GetLifetimeId(), identity) << "content changes do not mint object identities";
        EXPECT_EQ(first.GetBackendStateVersion(), 18u);
        EXPECT_EQ(first.GetBlockBindingVersion(), 22u);
        EXPECT_EQ(first.GetImageUnitVersion(), 22u);
        EXPECT_EQ(first.GetUBOContentVersion(), 26u);
#else
        GTEST_SKIP() << "server program sources require the disaggregated build";
#endif
    }
} // namespace
