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
#include <Config.h>
#include <MG_Impl/Pipe/CsoCache.h>
#include <MG_Impl/Pipe/Tracker.h>
#include <MG_Pipe/MGPipeRenderStateSpans.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Pipe/PipeMutation.h>
#include <MG_State/GLState/Core.h>
#include <MG_Util/Metrics/PipeStats.h>

#include <limits>
#include <string>
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

    // ===================================================================================
    // The dirty walk itself, and the render-state emission it drives (P2 brief D4, D6, D7)
    // ===================================================================================
    //
    // These drive the tracker and the cache DIRECTLY rather than through
    // MGPipeValidateForVerb. That is deliberate: MGPipeValidateForVerb reaches the library's
    // one process-wide tracker, and a unit test that asserts on a shared singleton is a test
    // that fails when ctest runs the suite in parallel. The emission logic these reproduce is
    // three lines long and is the same three lines the validate point runs.
    class TrackerWalk : public ::testing::Test {
    protected:
        void SetUp() override {
            m_previous = Move(MG_State::pGLContext);
            MG_State::pGLContext = MakeUnique<GLContext>();
            m_savedPush = MG_Config::Features.PipePush;
            MGPipeApplierReset();
        }
        void TearDown() override {
            MG_Config::Features.PipePush = m_savedPush;
            MGPipeApplierReset();
            MG_State::pGLContext = Move(m_previous);
        }

        static GLContext& Ctx() { return *MG_State::pGLContext; }

        // What MGPipeValidateForVerb's step 3 does, minus the PipeStats plumbing: acquire a
        // CSO when the pipeline version moved, and compute the dynamic chunk mask when
        // m_version moved.
        Uint32 Walk(MGPipeVerbClass verbClass = MGPipeVerbClass::kDraw) {
            const Uint32 dirty = m_tracker.Update(Ctx(), verbClass);
            const RenderStateParameters& live = Ctx().GetRenderStateParameters();
            m_lastDynamicMask = 0;
            if (dirty & MGPipeDirtyBit(MGPipeDirty::NewPipelineState)) {
                m_lastCso = m_cache.Acquire(live, m_payloadBytes);
                ++m_binds;
            }
            if (dirty & MGPipeDirtyBit(MGPipeDirty::NewRenderState)) {
                m_lastDynamicMask = m_tracker.FreshlyPrimed()
                                        ? ~0u
                                        : MGPipeDynamicChunksThatMoved(live, m_tracker.Staged());
            }
            if (dirty & (MGPipeDirtyBit(MGPipeDirty::NewPipelineState) |
                         MGPipeDirtyBit(MGPipeDirty::NewRenderState))) {
                m_tracker.Staged() = live;
            }
            return dirty;
        }

        MGPipeTracker m_tracker;
        MGPipeCsoCache m_cache;
        MGPipeHandle m_lastCso = kMGPipeNullHandle;
        Uint32 m_lastDynamicMask = 0;
        Uint64 m_payloadBytes = 0;
        Uint64 m_binds = 0;
        Uint64 m_savedPush = 0;
        UniquePtr<GLContext> m_previous;
    };

    TEST_F(TrackerWalk, EveryBitHasAName) {
        for (SizeT i = 0; i < kMGPipeDirtyCount; ++i) {
            ASSERT_NE(kMGPipeDirtyNames[i], nullptr);
            EXPECT_EQ(std::string(kMGPipeDirtyNames[i]).rfind("NEW_", 0), 0u);
        }
    }

    // The five P2 emits for each name their own subsystem; the rest name none, which is what
    // makes MOBILEGL_PIPE_PUSH a per-subsystem A/B instead of one switch.
    TEST_F(TrackerWalk, OnlyTheFiveEmittedBitsNameASubsystem) {
        for (SizeT i = 0; i < kMGPipeDirtyCount; ++i) {
            const auto bit = static_cast<MGPipeDirty>(i);
            const Bool emitted = (kMGPipeDirtyEmittedAtP2 & MGPipeDirtyBit(bit)) != 0;
            EXPECT_EQ(MGPipeSubsystemForDirty(bit) != 0, emitted) << kMGPipeDirtyNames[i];
        }
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewRenderState), kMGPipeSubsystemRenderState);
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewPixelPack), kMGPipeSubsystemPixelPack);
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewPatchState), kMGPipeSubsystemPatchState);
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewVertexAttribDefaults),
                  kMGPipeSubsystemVertexAttribDefaults);
    }

    TEST_F(TrackerWalk, TheFirstWalkOnAFreshContextPublishesEverything) {
        const Uint32 dirty = Walk();
        EXPECT_TRUE(m_tracker.FreshlyPrimed());
        for (SizeT i = 0; i < kMGPipeDirtyCount; ++i) {
            EXPECT_NE(dirty & (Uint32{1} << static_cast<Uint32>(i)), 0u)
                << kMGPipeDirtyNames[i] << " did not fire on a fresh context";
        }
    }

    // The whole point of a validate-point tracker: two identical draws in a row cost two
    // Uint16 compares and emit nothing at all.
    TEST_F(TrackerWalk, SteadyStateEmitsNothing) {
        Walk();
        const Uint64 mintsAfterFirst = m_cache.GetCounters().Mints;
        const Uint64 bindsAfterFirst = m_binds;
        for (int i = 0; i < 8; ++i) EXPECT_EQ(Walk(), 0u) << "walk " << i << " fired with nothing moved";
        EXPECT_EQ(m_cache.GetCounters().Mints, mintsAfterFirst);
        EXPECT_EQ(m_binds, bindsAfterFirst);
    }

    // The Blaze3D shape ARCHITECTURE.md 5.1 names as the reason push happens at validate and
    // not in the setter: enable / draw / disable / draw forever mints exactly TWO CSOs and
    // reuses them for every toggle after that.
    TEST_F(TrackerWalk, BlendToggleReusesTwoCsos) {
        constexpr int kToggles = 32;
        Walk(); // prime
        // The priming walk already minted and cached the blend-DISABLED state, so the cache
        // starts empty here or the count below would be one short of the shape it describes.
        m_cache.Reset();
        m_cache.ResetCounters();
        m_binds = 0;
        for (int i = 0; i < kToggles; ++i) {
            Ctx().SetCapability(CapabilityInput::Blend, true);
            Walk();
            Ctx().SetCapability(CapabilityInput::Blend, false);
            Walk();
        }
        EXPECT_EQ(m_cache.GetCounters().Mints, 2u)
            << "a two-state ping-pong must mint two CSOs and then never mint again";
        EXPECT_EQ(m_binds, static_cast<Uint64>(2 * kToggles));
        EXPECT_EQ(m_cache.GetCounters().Hits, static_cast<Uint64>(2 * kToggles - 2));
        EXPECT_EQ(m_cache.Size(), 2u);
        m_cache.Reset();
    }

    // The regression RenderState.h records: a glViewport must not evict a cached pipeline.
    // It mints nothing and its payload is one dynamic chunk - D0, the viewports - and not the
    // other seven.
    TEST_F(TrackerWalk, ViewportDoesNotMintACso) {
        Walk(); // prime
        m_cache.ResetCounters();
        for (Int i = 1; i <= 16; ++i) {
            Ctx().SetViewport(IntVec4(0, 0, 64 + i, 48 + i));
            const Uint32 dirty = Walk();
            EXPECT_NE(dirty & MGPipeDirtyBit(MGPipeDirty::NewRenderState), 0u);
            EXPECT_EQ(dirty & MGPipeDirtyBit(MGPipeDirty::NewPipelineState), 0u)
                << "glViewport moved the PIPELINE version";
            EXPECT_EQ(m_lastDynamicMask, 1u) << "glViewport sent something other than chunk D0";
        }
        EXPECT_EQ(m_cache.GetCounters().Mints, 0u);
        EXPECT_EQ(m_binds, 1u) << "only the priming walk may bind";
        m_cache.Reset();
    }

    // RenderState's two shutters are Uint16 and the tracker widens them in its OWN state,
    // never in MG_State. A wrap must cost an extra re-push at worst and never a missed one.
    TEST_F(TrackerWalk, WrapAroundRePushesButNeverMisses) {
        Walk(); // prime
        constexpr int kMoves = 70000; // past 65535 with room to spare
        Uint64 fired = 0;
        for (int i = 0; i < kMoves; ++i) {
            // Never the default 1.0f: a setter that early-outs on an unchanged value would
            // not move m_version, and the first iteration would then be a false miss.
            Ctx().SetLineWidth((i & 1) ? 2.0f : 3.0f);
            if (Walk() & MGPipeDirtyBit(MGPipeDirty::NewRenderState)) ++fired;
        }
        EXPECT_EQ(fired, static_cast<Uint64>(kMoves))
            << "a Uint16 wrap swallowed a render-state change";
        m_cache.Reset();
    }

    // The one direction the P1 verify comparator cannot see: it compares object-class fields
    // by IDENTITY only, so a bound texture whose CONTENT moved looks unchanged to it.
    // ARCHITECTURE.md 13.2 names under-firing as the dangerous direction, and this is the
    // first test of it.
    TEST_F(TrackerWalk, AggregateGenerationCatchesABoundTextureMoving) {
        const auto& tex = Ctx().CreateTextureObject(1, TextureTarget::Texture2D);
        ASSERT_TRUE(tex != nullptr);
        Walk(); // prime
        EXPECT_EQ(Walk() & MGPipeDirtyBit(MGPipeDirty::NewSamplerViews), 0u);

        static_cast<TextureObjectBase*>(tex.get())->BumpContentVersion();
        EXPECT_NE(Walk() & MGPipeDirtyBit(MGPipeDirty::NewSamplerViews), 0u)
            << "a bound texture's content moved and NEW_SAMPLER_VIEWS did not fire";
        // and it settles again, so the bit is a shutter and not a stuck flag
        EXPECT_EQ(Walk() & MGPipeDirtyBit(MGPipeDirty::NewSamplerViews), 0u);
        m_cache.Reset();
    }

    // A NaN outer level is a legal glPatchParameterfv value and has to compare equal to
    // itself, which float equality does not do and a byte compare does.
    TEST_F(TrackerWalk, ANaNPatchLevelEqualsItselfAndDoesNotFireForever) {
        Walk(); // prime
        Ctx().SetPatchDefaultOuterLevel(
            FloatVec4(std::numeric_limits<Float>::quiet_NaN(), 1.0f, 1.0f, 1.0f));
        EXPECT_NE(Walk() & MGPipeDirtyBit(MGPipeDirty::NewPatchState), 0u);
        EXPECT_EQ(Walk() & MGPipeDirtyBit(MGPipeDirty::NewPatchState), 0u)
            << "a NaN patch level re-fired against itself";
        m_cache.Reset();
    }

    TEST_F(TrackerWalk, ThePixelPackShutterIsAByteCompareOfThePackHalfOnly) {
        Walk(); // prime
        EXPECT_EQ(Walk() & MGPipeDirtyBit(MGPipeDirty::NewPixelPack), 0u);
        Ctx().SetPixelStoreParam(PixelStoreParam::PackAlignment, 8);
        EXPECT_NE(Walk() & MGPipeDirtyBit(MGPipeDirty::NewPixelPack), 0u);
        EXPECT_EQ(Walk() & MGPipeDirtyBit(MGPipeDirty::NewPixelPack), 0u);
        // The UNPACK half has no carrier at all, so it must not move the pack shutter.
        Ctx().SetPixelStoreParam(PixelStoreParam::UnpackAlignment, 8);
        EXPECT_EQ(Walk() & MGPipeDirtyBit(MGPipeDirty::NewPixelPack), 0u)
            << "an unpack write moved the PACK shutter";
        m_cache.Reset();
    }

    TEST_F(TrackerWalk, TheFireTalliesOnlyRunWhilePipeStatsIsOn) {
        // PipeStats is off in a unit-test process, which is the state the ROADMAP rule about
        // hot-path instrumentation cares about: the walk must cost nothing extra there.
        ASSERT_FALSE(MG_Util::PipeStats::Enabled());
        Walk();
        Ctx().SetLineWidth(3.0f);
        Walk();
        EXPECT_EQ(m_tracker.WalkCount(), 0u);
        EXPECT_EQ(m_tracker.FireCount(MGPipeDirty::NewRenderState), 0u);
        m_cache.Reset();
    }

#endif // MOBILEGL_PIPE_PUSH
} // namespace
