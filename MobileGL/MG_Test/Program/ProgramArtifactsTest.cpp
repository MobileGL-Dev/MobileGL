// MobileGL - MobileGL/MG_Test/Program/ProgramArtifactsTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// First include on purpose: the artifacts header must be self-contained (P0.5 gate A).
#include <MG_State/GLState/ProgramState/ProgramArtifacts.h>
#include <gtest/gtest.h>
// For the alias checks only.
#include <MG_State/GLState/ProgramState/ProgramObject.h>

#include <type_traits>

namespace {
    using namespace MobileGL;
    using namespace MobileGL::MG_State::GLState;

    // P0.5 moved the five types to namespace scope and left in-class aliases behind so the
    // existing spellings compile unchanged. The classic failure of that move is a COPY: two
    // same-named definitions that both compile and silently split the type. is_same_v is the
    // compile-time proof that ProgramObject::X and GLState::X are one type.
    TEST(ProgramArtifacts, AliasesAreTheSameTypes) {
        static_assert(std::is_same_v<ProgramObject::TypeFacts, TypeFacts>);
        static_assert(std::is_same_v<ProgramObject::ResourceReflection, ResourceReflection>);
        static_assert(std::is_same_v<ProgramObject::XfbVarying, XfbVarying>);
        static_assert(std::is_same_v<ProgramObject::LinkArtifacts, LinkArtifacts>);
        static_assert(std::is_same_v<ProgramObject::SpirvArtifacts, SpirvArtifacts>);
        static_assert(std::is_same_v<ProgramObject::UniformReflection, UniformReflection>);
        static_assert(std::is_same_v<ProgramObject::BlockReflection, BlockReflection>);
        static_assert(std::is_same_v<ProgramObject::PipeInputReflection, PipeInputReflection>);
        static_assert(std::is_same_v<ProgramObject::PipeOutputReflection, PipeOutputReflection>);
        static_assert(ProgramObject::kInvalidUniformOffset == kInvalidUniformOffset);
        EXPECT_EQ(ProgramObject::kInvalidUniformOffset, kInvalidUniformOffset);
        EXPECT_EQ(kInvalidUniformOffset, ~0u);
    }

    // 13 Bool + 3 bytes of padding + 7 x 4-byte scalars on every ABI.
    TEST(ProgramArtifacts, TypeFactsIsPodOf44Bytes) {
        static_assert(std::is_trivially_copyable_v<TypeFacts>);
        static_assert(std::is_standard_layout_v<TypeFacts>);
        EXPECT_EQ(sizeof(TypeFacts), 44u);
        EXPECT_EQ(alignof(TypeFacts), 4u);
    }
} // namespace
