// MobileGL - MobileGL/MG_Test/Backend/DirectGLES/IntegerAttributePlanTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// What DirectGLES does with each (array data type, shader input type) pair. ES defines an integer
// attribute only when the array has the input's signedness, so every integer array is issued with
// it; 8/16-bit data of the other signedness also needs the program's seeds to put GL's value back,
// and without working seeds such an array keeps the application's type instead.

#include <gtest/gtest.h>

#include <MG_Backend/DirectGLES/IntegerAttributePlan.h>
#include <MG_Backend/DirectGLES/NarrowIntegerSeeds.h>

using namespace MobileGL;
using MobileGL::MG_Backend::DirectGLES::HeldIntegerRetype;
using MobileGL::MG_Backend::DirectGLES::IntegerAttribTypeForInput;
using MobileGL::MG_Backend::DirectGLES::IntegerAttributeDataMasks;
using MobileGL::MG_Backend::DirectGLES::IntegerAttributePlan;
using MobileGL::MG_Backend::DirectGLES::IntegerVertexInputMasks;
using MobileGL::MG_Backend::DirectGLES::NarrowIntegerSeedState;
using MobileGL::MG_Backend::DirectGLES::ResolveIntegerAttributePlan;

namespace {
    struct DataCase {
        DataType type;
        GLenum glType;
        Uint32 bits;
        Bool isSigned;
    };
    constexpr DataCase kData[] = {
        {DataType::Int8, GL_BYTE, 8, true},    {DataType::Uint8, GL_UNSIGNED_BYTE, 8, false},
        {DataType::Int16, GL_SHORT, 16, true}, {DataType::Uint16, GL_UNSIGNED_SHORT, 16, false},
        {DataType::Int32, GL_INT, 32, true},   {DataType::Uint32, GL_UNSIGNED_INT, 32, false},
    };

    Bool IsSignedGLType(GLenum type) {
        return type == GL_BYTE || type == GL_SHORT || type == GL_INT;
    }

    IntegerAttributePlan PlanFor(DataType data, GLenum input, Uint32 location, Bool seedsUsable = true) {
        IntegerVertexInputMasks inputs;
        inputs.Note(location, input);
        IntegerAttributeDataMasks masks;
        masks.Note(location, data);
        return ResolveIntegerAttributePlan(inputs, masks, seedsUsable);
    }
} // namespace

TEST(IntegerAttributePlan, EveryIntegerArrayIsIssuedWithItsInputsSignedness) {
    constexpr Uint32 location = 3;
    constexpr Uint32 bit = 1u << location;
    for (const DataCase& data : kData) {
        for (const GLenum input : {GL_INT_VEC4, GL_UNSIGNED_INT_VEC2}) {
            const Bool signedInput = input == GL_INT_VEC4;
            const IntegerAttributePlan plan = PlanFor(data.type, input, location);
            const Bool mismatch = signedInput != data.isSigned;
            SCOPED_TRACE(testing::Message() << "data 0x" << std::hex << data.glType << " input 0x" << input);
            EXPECT_EQ(plan.careMask, bit);
            EXPECT_EQ(plan.retypeMask, mismatch ? bit : 0u);
            EXPECT_EQ(IsSignedGLType(IntegerAttribTypeForInput(data.glType, (plan.retypeMask & bit) != 0)),
                      signedInput);
            // Only 8/16-bit data of the other signedness needs the seeds; 32-bit is the same bits.
            EXPECT_EQ(plan.widths8, mismatch && data.bits == 8 ? bit : 0u);
            EXPECT_EQ(plan.widths16, mismatch && data.bits == 16 ? bit : 0u);
            EXPECT_EQ(plan.widths8 & plan.widths16, 0u);
        }
    }
}

TEST(IntegerAttributePlan, AFloatInputLeavesItsArrayAlone) {
    for (const DataCase& data : kData) {
        const IntegerAttributePlan plan = PlanFor(data.type, GL_FLOAT_VEC4, 5);
        EXPECT_EQ(plan, IntegerAttributePlan{}) << std::hex << data.glType;
    }
    // ...and an integer input fed float data has no integer array to issue.
    EXPECT_EQ(PlanFor(DataType::Float32, GL_INT_VEC4, 5), IntegerAttributePlan{});
}

TEST(IntegerAttributePlan, WithoutWorkingSeedsANarrowArrayKeepsTheApplicationsType) {
    for (const DataCase& data : kData) {
        const GLenum input = data.isSigned ? GL_UNSIGNED_INT : GL_INT;
        const IntegerAttributePlan plan = PlanFor(data.type, input, 7, false);
        EXPECT_FALSE(plan.NeedsSeeds()) << std::hex << data.glType;
        EXPECT_EQ(plan.retypeMask, data.bits == 32 ? (1u << 7) : 0u) << std::hex << data.glType;
        EXPECT_EQ(plan, PlanFor(data.type, input, 7).WithoutSeeds());
    }
}

TEST(IntegerAttributePlan, LocationsStopAt31) {
    EXPECT_EQ(PlanFor(DataType::Uint8, GL_INT, 31).widths8, 1u << 31);
    EXPECT_EQ(PlanFor(DataType::Uint8, GL_INT, 32), IntegerAttributePlan{});
}

TEST(IntegerAttributePlan, AVertexArrayKeepsWhatItHoldsWhereNothingCanSeeIt) {
    IntegerAttributePlan plan;
    plan.careMask = 0b0110u;
    plan.retypeMask = 0b0100u;
    // Bit 0 is held and unobservable: kept. Bit 1 is observable and wanted plain: cleared.
    EXPECT_EQ(HeldIntegerRetype(0b0011u, plan), 0b0101u);
    EXPECT_EQ(HeldIntegerRetype(0u, IntegerAttributePlan{}), 0u);
}

TEST(IntegerAttributePlan, TheIssuedTypeOnlyEverSwapsSignedness) {
    for (const DataCase& data : kData) {
        EXPECT_EQ(IntegerAttribTypeForInput(data.glType, false), data.glType);
        const GLenum swapped = IntegerAttribTypeForInput(data.glType, true);
        EXPECT_NE(IsSignedGLType(swapped), data.isSigned);
        EXPECT_EQ(IntegerAttribTypeForInput(swapped, true), data.glType);
    }
    EXPECT_EQ(IntegerAttribTypeForInput(GL_FLOAT, true), static_cast<GLenum>(GL_FLOAT));
}

// A program's arming across builds: a failed armed build is retried without the seeds for that
// link only, and an application relink starts over from what the new link declares.
TEST(NarrowIntegerSeedState, AFailedArmedBuildLastsUntilTheNextLink) {
    using MobileGL::MG_Backend::DirectGLES::BeginNarrowIntegerSeedBuild;
    using MobileGL::MG_Backend::DirectGLES::FinishNarrowIntegerSeedBuild;
    NarrowIntegerSeedState state;
    state.PrepareBuild(1, true, false);
    EXPECT_FALSE(state.wanted) << "a program that does not capture is armed by a draw, not a build";
    state.wanted = true;
    BeginNarrowIntegerSeedBuild(state);
    FinishNarrowIntegerSeedBuild(state, 0, false);
    EXPECT_TRUE(state.RefusedAt(1));
    EXPECT_TRUE(state.Stale()) << "the failed link is rebuilt without the seeds";

    state.PrepareBuild(1, true, false);
    BeginNarrowIntegerSeedBuild(state);
    EXPECT_FALSE(state.built);
    EXPECT_TRUE(state.RefusedAt(1));
    EXPECT_FALSE(state.Stale());
    EXPECT_FALSE(state.RefusedAt(2)) << "a link not built yet is presumed to take the seeds";

    state.PrepareBuild(2, true, false);
    EXPECT_FALSE(state.RefusedAt(2));
    EXPECT_TRUE(state.Target()) << "a relink arms again";
    EXPECT_TRUE(state.Stale());

    state.PrepareBuild(3, false, false);
    EXPECT_FALSE(state.wanted) << "a link without integer inputs has nothing to arm";
    EXPECT_FALSE(state.Target());
    state.PrepareBuild(4, true, true);
    EXPECT_TRUE(state.Target()) << "a capturing link is armed at its build";
}
