// MobileGL - MobileGL/MG_Test/Pipe/TrackerTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The frontend state tracker: the dirty walk, the aggregate generations, the per-bit fire
// counters (P2 brief D4). Owned by P2 package B (p2/tracker); the file and its CMake
// registration are the contract commit's.
//
// Needs the push sources, so every case is a visible SKIP in a pull build rather than a
// vanishing test - the shape PipeInputsTest.cpp established.
#include <gtest/gtest.h>

#include "Includes.h"
#include <MG_Pipe/MGPipe.h>

#if MOBILEGL_PIPE_PUSH
#include <MG_Pipe/PipeMutation.h>
#include <MG_State/GLState/Core.h>
#endif

using namespace MobileGL;
using namespace MobileGL::MG_Pipe;

namespace {
    // The subsystem bitmask the tracker dispatches on is allocated in every build.
    TEST(Tracker, SubsystemBitsDoNotOverlapTheBehaviourBit) {
        EXPECT_EQ(kMGPipeSubsystemsMigratedAtP2 & kMGPipeBehaviourNoCsoContentAddressing, 0ull);
    }

#if !MOBILEGL_PIPE_PUSH
    TEST(Tracker, SkippedInAPullBuild) {
        GTEST_SKIP() << "the tracker is compiled only under MOBILEGL_PIPE_PUSH";
    }
#else
    using GLContext = MG_State::GLState::GLContext;
    
    using MG_State::GLState::TextureObjectBase;
    using MobileGL::TextureTarget;

    constexpr SizeT kAggregateCount = static_cast<SizeT>(MGPipeAggregate::Count);

    // A live frontend context for the bump points to find, restored on the way out so the
    // cases stay independent (PipeInputsTest's idiom).
    class TrackerAggregates : public ::testing::Test {
    protected:
        void SetUp() override {
            m_previous = Move(MG_State::pGLContext);
            MG_State::pGLContext = MakeUnique<GLContext>();
        }
        void TearDown() override { MG_State::pGLContext = Move(m_previous); }

        static GLContext& Ctx() { return *MG_State::pGLContext; }

        struct Snapshot {
            Uint64 Values[kAggregateCount];
            Uint64 operator[](MGPipeAggregate a) const { return Values[static_cast<SizeT>(a)]; }
        };

        static Snapshot Snap() {
            GLContext& c = Ctx();
            Snapshot s{};
            s.Values[static_cast<SizeT>(MGPipeAggregate::VaoAttribute)] = c.GetAnyVaoAttributeGeneration();
            s.Values[static_cast<SizeT>(MGPipeAggregate::FramebufferAttachment)] =
                c.GetAnyFramebufferAttachmentGeneration();
            s.Values[static_cast<SizeT>(MGPipeAggregate::TextureContent)] = c.GetAnyTextureContentGeneration();
            s.Values[static_cast<SizeT>(MGPipeAggregate::TextureParams)] = c.GetAnyTextureParamsGeneration();
            s.Values[static_cast<SizeT>(MGPipeAggregate::BufferChange)] = c.GetAnyBufferChangeGeneration();
            s.Values[static_cast<SizeT>(MGPipeAggregate::VertexAttribDefault)] =
                c.GetAnyVertexAttribDefaultGeneration();
            return s;
        }

        // The whole contract of an aggregate generation in one assertion: the bump point
        // moved ITS counter and moved NO other. The second half is what stops a bump point
        // being wired to the wrong aggregate, which would over-fire one dirty bit and
        // under-fire another - and under-firing is the direction that renders stale.
        static void ExpectOnly(MGPipeAggregate moved, const Snapshot& before, const Snapshot& after) {
            for (SizeT i = 0; i < kAggregateCount; ++i) {
                const auto which = static_cast<MGPipeAggregate>(i);
                if (which == moved) {
                    EXPECT_GT(after.Values[i], before.Values[i]) << "aggregate " << i << " did not move";
                } else {
                    EXPECT_EQ(after.Values[i], before.Values[i]) << "aggregate " << i << " moved and must not";
                }
            }
        }

        UniquePtr<GLContext> m_previous;
    };

    TEST_F(TrackerAggregates, EveryAggregateStartsAtZero) {
        const Snapshot s = Snap();
        for (SizeT i = 0; i < kAggregateCount; ++i) EXPECT_EQ(s.Values[i], 0ull);
    }

    TEST_F(TrackerAggregates, AVertexArrayAttributeMovesOnlyTheVaoAggregate) {
        const auto& vao = Ctx().CreateVertexArrayObject(1);
        ASSERT_TRUE(vao != nullptr);
        const Snapshot before = Snap();
        vao->EnableAttribute(3);
        ExpectOnly(MGPipeAggregate::VaoAttribute, before, Snap());
    }

    TEST_F(TrackerAggregates, AFramebufferObjectWriteMovesOnlyTheFramebufferAggregate) {
        const auto& fbo = Ctx().CreateFramebufferObject(1);
        ASSERT_TRUE(fbo != nullptr);
        fbo->SetReadBuffer(FramebufferAttachmentType::Color0);
        const Snapshot before = Snap();
        fbo->SetReadBuffer(FramebufferAttachmentType::Color1);
        ExpectOnly(MGPipeAggregate::FramebufferAttachment, before, Snap());
    }

    TEST_F(TrackerAggregates, AFramebufferDefaultSetterMovesOnlyTheFramebufferAggregate) {
        const auto& fbo = Ctx().CreateFramebufferObject(2);
        ASSERT_TRUE(fbo != nullptr);
        const Snapshot before = Snap();
        // The five MOBILEGL_DEFINE_FRAMEBUFFER_DEFAULT_SETTER bodies are one macro, so the
        // bump statement inside it has to carry its own line continuation or the macro
        // silently swallows the next line. This case is what says it did not.
        fbo->SetDefaultWidth(64);
        ExpectOnly(MGPipeAggregate::FramebufferAttachment, before, Snap());
    }

    TEST_F(TrackerAggregates, ATextureContentWriteMovesOnlyTheContentAggregate) {
        const auto& tex = Ctx().CreateTextureObject(1, TextureTarget::Texture2D);
        ASSERT_TRUE(tex != nullptr);
        const Snapshot before = Snap();
        static_cast<TextureObjectBase*>(tex.get())->BumpContentVersion();
        ExpectOnly(MGPipeAggregate::TextureContent, before, Snap());
    }

    TEST_F(TrackerAggregates, ATextureParameterMovesOnlyTheParamsAggregate) {
        const auto& tex = Ctx().CreateTextureObject(2, TextureTarget::Texture2D);
        ASSERT_TRUE(tex != nullptr);
        const Snapshot before = Snap();
        tex->SetMaxLevel(4);
        ExpectOnly(MGPipeAggregate::TextureParams, before, Snap());
    }

    TEST_F(TrackerAggregates, ASamplerParameterMovesOnlyTheParamsAggregate) {
        const auto& sampler = Ctx().CreateSamplerObject(1);
        ASSERT_TRUE(sampler != nullptr);
        const Snapshot before = Snap();
        sampler->SetWrapS(MobileGL::SamplerWrapMode::ClampToEdge);
        ExpectOnly(MGPipeAggregate::TextureParams, before, Snap());
    }

    TEST_F(TrackerAggregates, ABufferRespecifyMovesOnlyTheBufferAggregate) {
        const auto& buffer = Ctx().CreateBufferObject(1);
        ASSERT_TRUE(buffer != nullptr);
        const Snapshot before = Snap();
        buffer->Respecify(64, nullptr);
        ExpectOnly(MGPipeAggregate::BufferChange, before, Snap());
    }

    TEST_F(TrackerAggregates, AVertexAttribDefaultMovesOnlyItsOwnAggregate) {
        const Snapshot before = Snap();
        Ctx().SetCurrentVertexAttributeFloat(2, Array<Float, 4>{1.0f, 2.0f, 3.0f, 4.0f});
        ExpectOnly(MGPipeAggregate::VertexAttribDefault, before, Snap());
    }

    TEST_F(TrackerAggregates, ANoteWithoutALiveContextIsANoOp) {
        UniquePtr<GLContext> held = Move(MG_State::pGLContext);
        MGP_NOTE_AGGREGATE(BufferChange); // must not dereference a null context
        MG_State::pGLContext = Move(held);
        SUCCEED();
    }
#endif // MOBILEGL_PIPE_PUSH
} // namespace
