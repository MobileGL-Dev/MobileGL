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
#include <MG_Impl/Pipe/PipeFill.h>
#include <MG_Impl/Pipe/SetHashSuppressor.h>
#include <MG_Impl/Pipe/Tracker.h>
#include <MG_Pipe/MGPipeRenderStateSpans.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Pipe/PipeMutation.h>
#include <MG_State/GLState/Core.h>
#include <MG_Util/Metrics/PipeStats.h>

#include <cstring>
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
    // G2 REQUIRES THE PULL AND PUSH CTEST NAME SETS TO BE IDENTICAL, name for name. A
    // push-only case therefore cannot be ABSENT from a pull build; it has to be there and
    // SKIP, which is the shape PipeInputsTest.cpp established for the same reason. This list
    // declares exactly the suite.name pairs the push build gets from the real cases below, so
    // a case added on one side and forgotten on the other shows up as a ctest-name diff
    // rather than as a test that silently is not there.
#define MGL_TRACKER_TEST_LIST(X) \
    X(TrackerAggregates, EveryAggregateStartsAtZero) \
    X(TrackerAggregates, AVertexArrayAttributeMovesOnlyTheVaoAggregate) \
    X(TrackerAggregates, AFramebufferObjectWriteMovesOnlyTheFramebufferAggregate) \
    X(TrackerAggregates, AFramebufferDefaultSetterMovesOnlyTheFramebufferAggregate) \
    X(TrackerAggregates, ATextureContentWriteMovesOnlyTheContentAggregate) \
    X(TrackerAggregates, ATextureParameterMovesOnlyTheParamsAggregate) \
    X(TrackerAggregates, ASamplerParameterMovesOnlyTheParamsAggregate) \
    X(TrackerAggregates, ABufferRespecifyMovesOnlyTheBufferAggregate) \
    X(TrackerAggregates, AVertexAttribDefaultMovesOnlyItsOwnAggregate) \
    X(TrackerAggregates, ANoteWithoutALiveContextIsANoOp) \
    X(TrackerWalk, EveryBitHasAName) \
    X(TrackerWalk, OnlyTheFiveEmittedBitsNameASubsystem) \
    X(TrackerWalk, TheFirstWalkOnAFreshContextPublishesEverything) \
    X(TrackerWalk, SteadyStateEmitsNothing) \
    X(TrackerWalk, BlendToggleReusesTwoCsos) \
    X(TrackerWalk, ViewportDoesNotMintACso) \
    X(TrackerWalk, WrapAroundRePushesButNeverMisses) \
    X(TrackerWalk, AggregateGenerationCatchesABoundTextureMoving) \
    X(TrackerWalk, ANaNPatchLevelEqualsItselfAndDoesNotFireForever) \
    X(TrackerWalk, ThePixelPackShutterIsAByteCompareOfThePackHalfOnly) \
    X(TrackerWalk, TheFireTalliesOnlyRunWhilePipeStatsIsOn) \
    X(TrackerAttribPayload, AFloatWriteCarriesTheFloatBitsAndNamesItsClass) \
    X(TrackerAttribPayload, AnIntWriteCarriesTheIntWordsAndNamesItsClass) \
    X(TrackerAttribPayload, AUintWriteCarriesTheUintWordsAndNamesItsClass) \
    X(TrackerAttribPayload, TheSameNumbersWrittenThroughADifferentClassAreADifferentValue) \
    X(TrackerShippedEmitter, ABlendToggleThroughTheValidatePointMintsTwoCsos) \
    X(TrackerShippedEmitter, TheSteadyStateThroughTheValidatePointEmitsNothing) \
    X(TrackerShippedEmitter, APushedAttributeDefaultTheApplierCannotReproduceIsRepaired) \
    X(TrackerShippedEmitter, AViewportThroughTheValidatePointMintsNoCso)

#define MGL_DECLARE_PULL_SKIP(Suite, Name)                                                         \
    TEST(Suite, Name) { GTEST_SKIP() << "compiled only under MOBILEGL_PIPE_PUSH"; }
    MGL_TRACKER_TEST_LIST(MGL_DECLARE_PULL_SKIP)
#undef MGL_DECLARE_PULL_SKIP
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
    // Resetting the applier without resetting the process-wide cache and tracker would leave
    // the next bind_render_state naming a CSO the applier no longer has - it asserts and
    // returns, leaving m_renderState unwritten. The three are one state, so they are reset
    // together, here and in TrackerShippedEmitter.
    void ResetTheServerSideSingletons() {
        MGPipeApplierReset();
        MGPipeCsoCacheInstance().Reset();
        MGPipeCsoCacheInstance().ResetCounters();
        MGPipeTrackerInstance().Reset();
        MGPipeSetHashSuppressorInstance().InvalidateAll();
    }

    class TrackerWalk : public ::testing::Test {
    protected:
        void SetUp() override {
            m_previous = Move(MG_State::pGLContext);
            MG_State::pGLContext = MakeUnique<GLContext>();
            m_savedPush = MG_Config::Features.PipePush;
            ResetTheServerSideSingletons();
        }
        void TearDown() override {
            MG_Config::Features.PipePush = m_savedPush;
            ResetTheServerSideSingletons();
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

    // ===================================================================================
    // set_vertex_attrib_defaults' payload (P2 brief D10)
    // ===================================================================================
    //
    // A CurrentVertexAttributeValue is ONE value in three views and GLContext converts
    // numerically between them, so four words on the wire are not the value unless the class
    // travels with them. These pin exactly that, because nothing else can: the emission
    // happens at step 3 and the residual fill re-pulls the field at step 4, so at a kDraw
    // verb a wrong payload is overwritten before any comparator or backend read sees it -
    // which is how a hard-coded ValueClass of 0 survived a green verify lane.
    class TrackerAttribPayload : public ::testing::Test {
    protected:
        void SetUp() override {
            m_previous = Move(MG_State::pGLContext);
            MG_State::pGLContext = MakeUnique<GLContext>();
        }
        void TearDown() override { MG_State::pGLContext = Move(m_previous); }

        static GLContext& Ctx() { return *MG_State::pGLContext; }

        static MGPAttribValue PayloadFor(Uint location) {
            MGPAttribValue value{};
            MGPipeFillAttribValue(static_cast<Uint32>(location), Ctx().GetCurrentVertexAttribute(location),
                                  Ctx().GetCurrentVertexAttributeClass(location), value);
            return value;
        }

        static Uint32 Word(Float value) {
            Uint32 bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            return bits;
        }

        UniquePtr<GLContext> m_previous;
    };

    TEST_F(TrackerAttribPayload, AFloatWriteCarriesTheFloatBitsAndNamesItsClass) {
        Ctx().SetCurrentVertexAttributeFloat(3, Array<Float, 4>{1.5f, -2.5f, 3.0f, 4.0f});
        const MGPAttribValue value = PayloadFor(3);
        EXPECT_EQ(value.Location, 3u);
        EXPECT_EQ(value.ValueClass, MG_State::GLState::kVertexAttribValueClassFloat);
        EXPECT_EQ(value.Data[0], Word(1.5f));
        EXPECT_EQ(value.Data[1], Word(-2.5f));
        // The defect this exists to stop: 1.5f's int VIEW is 1, and a carrier that sent the
        // float bits while calling them class 0 for every attribute would be sending
        // 0x3FC00000 where the frontend holds 1.
        EXPECT_NE(value.Data[0], static_cast<Uint32>(Ctx().GetCurrentVertexAttribute(3).intValue[0]));
    }

    TEST_F(TrackerAttribPayload, AnIntWriteCarriesTheIntWordsAndNamesItsClass) {
        Ctx().SetCurrentVertexAttributeInt(5, Array<Int32, 4>{7, -9, 11, 13});
        const MGPAttribValue value = PayloadFor(5);
        EXPECT_EQ(value.ValueClass, MG_State::GLState::kVertexAttribValueClassInt);
        EXPECT_EQ(static_cast<Int32>(value.Data[0]), 7);
        EXPECT_EQ(static_cast<Int32>(value.Data[1]), -9);
        // and NOT the float view the frontend converted it into
        EXPECT_NE(value.Data[0], Word(7.0f));
    }

    TEST_F(TrackerAttribPayload, AUintWriteCarriesTheUintWordsAndNamesItsClass) {
        Ctx().SetCurrentVertexAttributeUint(6, Array<Uint32, 4>{4000000000u, 2u, 3u, 4u});
        const MGPAttribValue value = PayloadFor(6);
        EXPECT_EQ(value.ValueClass, MG_State::GLState::kVertexAttribValueClassUint);
        EXPECT_EQ(value.Data[0], 4000000000u);
        EXPECT_NE(value.Data[0], Word(4000000000.0f));
    }

    // The class is PER ATTRIBUTE and it is the last writer's, not the context's - a payload
    // that took one attribute's class for all 32 would be the same defect as a hard-coded 0.
    TEST_F(TrackerAttribPayload, TheSameNumbersWrittenThroughADifferentClassAreADifferentValue) {
        Ctx().SetCurrentVertexAttributeFloat(1, Array<Float, 4>{1.0f, 2.0f, 3.0f, 4.0f});
        Ctx().SetCurrentVertexAttributeInt(2, Array<Int32, 4>{1, 2, 3, 4});
        EXPECT_EQ(PayloadFor(1).ValueClass, MG_State::GLState::kVertexAttribValueClassFloat);
        EXPECT_EQ(PayloadFor(2).ValueClass, MG_State::GLState::kVertexAttribValueClassInt);
        // Same numbers, different classes, so the same four words mean different things:
        // 1.0f is 0x3F800000 and the integer 1 is 0x00000001.
        EXPECT_NE(PayloadFor(1).Data[0], PayloadFor(2).Data[0]);
        // An attribute nobody wrote answers Float, which is what the GL default (0,0,0,1) is.
        EXPECT_EQ(PayloadFor(7).ValueClass, MG_State::GLState::kVertexAttribValueClassFloat);
        // and a later write of the other class moves the class of THAT attribute only
        Ctx().SetCurrentVertexAttributeUint(1, Array<Uint32, 4>{1u, 2u, 3u, 4u});
        EXPECT_EQ(PayloadFor(1).ValueClass, MG_State::GLState::kVertexAttribValueClassUint);
        EXPECT_EQ(PayloadFor(2).ValueClass, MG_State::GLState::kVertexAttribValueClassInt);
    }

    // ===================================================================================
    // The SHIPPED emitter, driven through MGPipeValidateForVerb itself
    // ===================================================================================
    //
    // TrackerWalk above reproduces step 3 against a local tracker and cache, which cannot
    // fail on a defect in the validate point itself (a bit gated on the wrong subsystem, an
    // emission dropped). These drive the real entry point and read the real singletons back.
    // Safe because ctest runs one gtest case per process and the fixture resets all three
    // pieces of server-side state on both sides of every case.
    class TrackerShippedEmitter : public ::testing::Test {
    protected:
        void SetUp() override {
            m_previous = Move(MG_State::pGLContext);
            MG_State::pGLContext = MakeUnique<GLContext>();
            m_savedPush = MG_Config::Features.PipePush;
            MG_Config::Features.PipePush = kMGPipeSubsystemsMigratedAtP2;
            ResetTheServerSideSingletons();
        }
        void TearDown() override {
            MGPipeLeaveVerb();
            MG_Config::Features.PipePush = m_savedPush;
            ResetTheServerSideSingletons();
            MG_State::pGLContext = Move(m_previous);
        }

        static GLContext& Ctx() { return *MG_State::pGLContext; }
        static void Draw() { MGPipeValidateForVerb(MGPipeVerb::DrawArrays); }
        static const MGPipeCsoCache::Counters& Cso() { return MGPipeCsoCacheInstance().GetCounters(); }

        Uint64 m_savedPush = 0;
        UniquePtr<GLContext> m_previous;
    };

    TEST_F(TrackerShippedEmitter, ABlendToggleThroughTheValidatePointMintsTwoCsos) {
        constexpr int kToggles = 16;
        Draw(); // prime: a fresh context resets the cache inside the emitter and mints once
        MGPipeCsoCacheInstance().ResetCounters();
        for (int i = 0; i < kToggles; ++i) {
            Ctx().SetCapability(CapabilityInput::Blend, true);
            Draw();
            Ctx().SetCapability(CapabilityInput::Blend, false);
            Draw();
        }
        EXPECT_EQ(Cso().Mints, 1u) << "the blend-disabled state was already cached by the priming draw";
        EXPECT_EQ(Cso().Binds, static_cast<Uint64>(2 * kToggles));
        EXPECT_EQ(Cso().Hits, static_cast<Uint64>(2 * kToggles - 1));
        EXPECT_EQ(MGPipeCsoCacheInstance().Size(), 2u);
    }

    TEST_F(TrackerShippedEmitter, TheSteadyStateThroughTheValidatePointEmitsNothing) {
        Draw();
        // The positive half, so "nothing was emitted" cannot pass because nothing is wired:
        // the first draw on a fresh context mints and binds exactly one CSO.
        ASSERT_EQ(Cso().Mints, 1u) << "the priming draw emitted no create_render_state at all";
        ASSERT_EQ(Cso().Binds, 1u);
        MGPipeCsoCacheInstance().ResetCounters();
        for (int i = 0; i < 8; ++i) {
            Draw();
            EXPECT_EQ(MGPipeTrackerInstance().LastDirty(), 0u) << "walk " << i << " fired with nothing moved";
        }
        EXPECT_EQ(Cso().Mints, 0u);
        EXPECT_EQ(Cso().Binds, 0u) << "a steady-state draw bound a render-state CSO";
    }

    // The window MAJOR-2's repair covers: a glVertexAttrib* write followed by a verb whose
    // class does NOT read m_currentVertexAttribute. The call still goes out (the dirty bit
    // and the subsystem bit are all step 3 looks at), the applier writes four words into all
    // three views because it ignores ValueClass, and nothing in step 4 puts the value back -
    // so the client checks and repairs. Reading the storage here to prove it would be the
    // poison violation the fill table forbids, so the repair counter is the observable.
    TEST_F(TrackerShippedEmitter, APushedAttributeDefaultTheApplierCannotReproduceIsRepaired) {
        Draw();
        const Uint64 before = MGPipeVertexAttribDefaultRepairCount();
        // 1.5f is the point: its int view is 1 and its bit pattern is 0x3FC00000, so the two
        // cannot be the same four words whichever view the carrier picks.
        Ctx().SetCurrentVertexAttributeFloat(0, Array<Float, 4>{1.5f, 2.5f, 3.5f, 4.5f});
        MGPipeValidateForVerb(MGPipeVerb::GenerateMipmap);
        EXPECT_EQ(MGPipeVertexAttribDefaultRepairCount(), before + 1)
            << "the emitter accepted an applier write that cannot reproduce a converted value";
    }

    TEST_F(TrackerShippedEmitter, AViewportThroughTheValidatePointMintsNoCso) {
        Draw();
        ASSERT_EQ(Cso().Mints, 1u) << "the priming draw emitted no create_render_state at all";
        MGPipeCsoCacheInstance().ResetCounters();
        for (Int i = 1; i <= 8; ++i) {
            Ctx().SetViewport(IntVec4(0, 0, 64 + i, 48 + i));
            Draw();
            EXPECT_NE(MGPipeTrackerInstance().LastDirty() & MGPipeDirtyBit(MGPipeDirty::NewRenderState), 0u);
            EXPECT_EQ(MGPipeTrackerInstance().LastDirty() & MGPipeDirtyBit(MGPipeDirty::NewPipelineState), 0u);
        }
        EXPECT_EQ(Cso().Mints, 0u);
        EXPECT_EQ(Cso().Binds, 0u) << "glViewport reached the CSO cache";
    }

#endif // MOBILEGL_PIPE_PUSH
} // namespace
