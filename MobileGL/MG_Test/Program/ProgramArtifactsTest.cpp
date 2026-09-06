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

#include <cstdio>
#include <set>
#include <string>
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

    // A visitor that counts what a table hands it and checks the names are distinct - the
    // archive's one field table per type has to name every member exactly once.
    struct CountingVisitor {
        std::size_t count = 0;
        std::set<std::string> names;
        template <class Field>
        void operator()(const char* name, Field&) {
            ++count;
            names.insert(name);
        }
    };

    template <class T>
    std::size_t CountFields() {
        T value{};
        CountingVisitor mutableVisitor;
        VisitFields(value, mutableVisitor);
        const T& constValue = value;
        CountingVisitor constVisitor;
        VisitFields(constValue, constVisitor);
        EXPECT_EQ(mutableVisitor.count, constVisitor.count) << "const and non-const walks disagree";
        EXPECT_EQ(mutableVisitor.names.size(), mutableVisitor.count) << "a field name is listed twice";
        EXPECT_EQ(mutableVisitor.names, constVisitor.names);
        return mutableVisitor.count;
    }

    // The field counts are the member counts of the moved structs (LinkArtifacts minus its
    // never-archived `program`). A member added without a table entry is caught by the sizeof
    // trip wires in the header; a table entry dropped without a member change is caught here.
    TEST(ProgramArtifacts, VisitFieldsCoversEveryMember) {
        EXPECT_EQ(CountFields<TypeFacts>(), 20u);
        EXPECT_EQ(CountFields<ResourceReflection>(), 14u);
        EXPECT_EQ(CountFields<XfbVarying>(), 11u);
        EXPECT_EQ(CountFields<LinkArtifacts>(), 57u);
        EXPECT_EQ(CountFields<SpirvArtifacts>(), 8u);
    }

    // A const walk hands the visitor const references, a non-const walk mutable ones: the one
    // table really serves both directions.
    TEST(ProgramArtifacts, VisitFieldsPassesConstnessThrough) {
        LinkArtifacts artifacts;
        std::size_t mutableFields = 0;
        VisitFields(artifacts, [&](const char*, auto& field) {
            static_assert(!std::is_const_v<std::remove_reference_t<decltype(field)>>);
            ++mutableFields;
        });
        const LinkArtifacts& constArtifacts = artifacts;
        std::size_t constFields = 0;
        VisitFields(constArtifacts, [&](const char*, auto& field) {
            static_assert(std::is_const_v<std::remove_reference_t<decltype(field)>>);
            ++constFields;
        });
        EXPECT_EQ(mutableFields, constFields);
        // The table can write through: a deserializer's shape.
        VisitFields(artifacts, [](const char* name, auto& field) {
            if constexpr (std::is_same_v<std::remove_reference_t<decltype(field)>, Bool>) {
                if (std::string(name) == "linkStatus") field = true;
            }
        });
        EXPECT_TRUE(artifacts.linkStatus);
    }

    // The sizeof numbers, visible in every `ctest -V` log on every platform: this is where the
    // integrator reads a new toolchain's values from before pinning them in the header.
    TEST(ProgramArtifacts, SizesArePinnedOnThisToolchain) {
        RecordProperty("sizeof_TypeFacts", static_cast<int>(sizeof(TypeFacts)));
        RecordProperty("sizeof_ResourceReflection", static_cast<int>(sizeof(ResourceReflection)));
        RecordProperty("sizeof_XfbVarying", static_cast<int>(sizeof(XfbVarying)));
        RecordProperty("sizeof_LinkArtifacts", static_cast<int>(sizeof(LinkArtifacts)));
        RecordProperty("sizeof_SpirvArtifacts", static_cast<int>(sizeof(SpirvArtifacts)));
        std::printf("sizeof: TypeFacts=%zu ResourceReflection=%zu XfbVarying=%zu LinkArtifacts=%zu SpirvArtifacts=%zu\n",
                    sizeof(TypeFacts), sizeof(ResourceReflection), sizeof(XfbVarying), sizeof(LinkArtifacts),
                    sizeof(SpirvArtifacts));
#if defined(__GLIBCXX__) && !defined(_GLIBCXX_DEBUG) && (SIZE_MAX == UINT64_MAX)
        EXPECT_EQ(sizeof(ResourceReflection), static_cast<std::size_t>(MGL_RESOURCEREFLECTION_SIZE));
        EXPECT_EQ(sizeof(XfbVarying), static_cast<std::size_t>(MGL_XFBVARYING_SIZE));
        EXPECT_EQ(sizeof(LinkArtifacts), static_cast<std::size_t>(MGL_LINKARTIFACTS_SIZE));
        EXPECT_EQ(sizeof(SpirvArtifacts), static_cast<std::size_t>(MGL_SPIRVARTIFACTS_SIZE));
#else
        RecordProperty("sizes_pinned", "no: not the libstdc++ 64-bit toolchain");
#endif
    }
} // namespace
