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
#include <MG_Impl/Pipe/VertexInputEmit.h>
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
    X(TrackerWalk, TheIndexBufferBitDoesNotFireOnAnUnrelatedBufferWrite) \
    X(TrackerWalk, TheIndexBufferBitFiresWhenTheSlotVersionWrapsOntoADifferentBuffer) \
    X(TrackerWalk, ABaseInstanceSurvivesTheFirstWalkOnAFreshContext) \
    X(TrackerAttribPayload, AFloatWriteCarriesTheFloatBitsAndNamesItsClass) \
    X(TrackerAttribPayload, AnIntWriteCarriesTheIntWordsAndNamesItsClass) \
    X(TrackerAttribPayload, AUintWriteCarriesTheUintWordsAndNamesItsClass) \
    X(TrackerAttribPayload, TheSameNumbersWrittenThroughADifferentClassAreADifferentValue) \
    X(TrackerShippedEmitter, ABlendToggleThroughTheValidatePointMintsTwoCsos) \
    X(TrackerShippedEmitter, TheSteadyStateThroughTheValidatePointEmitsNothing) \
    X(TrackerShippedEmitter, APushedAttributeDefaultTheApplierCannotReproduceIsRepaired) \
    X(TrackerShippedEmitter, AViewportThroughTheValidatePointMintsNoCso) \
    X(TrackerShippedEmitter, AClipDistanceEnableReArmsTheResidualBlock) \
    X(TrackerShippedEmitter, AFreshContextRepublishesEveryVertexAttributeDefault) \
    X(TrackerShippedEmitter, AFreshContextResetsTheApplierWithTheRenderStateSubsystemOff) \
    X(TrackerShippedEmitter, ABaseInstancedDrawAfterAMakeCurrentPublishesItsOwnBaseInstance)

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
    // P3a adds two more pieces to that one state. MGPipeApplierReset is a MAKE-CURRENT and
    // deliberately keeps the object records now, so a fixture that means "this applier is
    // going away" has to say the other verb as well (PipeApply.h); and the vertex-input
    // emitter's latches say "this handle has already published this configuration" about an
    // applier that is about to be empty, so a bind would be suppressed against a record that
    // is no longer there.
    void ResetTheServerSideSingletons() {
        MGPipeApplierReset();
        MGPipeApplierReleaseObjectRecords();
        MGPipeCsoCacheInstance().Reset();
        MGPipeCsoCacheInstance().ResetCounters();
        MGPipeTrackerInstance().Reset();
        MGPipeSetHashSuppressorInstance().InvalidateAll();
        MGPipeVertexInputEmitterInstance().Reset();
        MGPipeVertexInputEmitterInstance().ResetCounters();
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
    // THE NAME IS P2's AND IT STAYS. A test name is never removed (only added), so this case
    // keeps the name it was born with and follows the phase constant instead of a literal
    // five: what it has always asserted is "a bit names a subsystem if and only if this build
    // emits a call for it", which is the property the emission gate and the residual-fill
    // skip both rest on. P3a took the vertex-input family over and P4a takes seven more bits
    // across four subsystems, so the set it compares against is now kMGPipeDirtyEmittedAtP4a -
    // and a bit that gained an arm without gaining an emitter, or the reverse, still fails
    // here.
    TEST_F(TrackerWalk, OnlyTheFiveEmittedBitsNameASubsystem) {
        for (SizeT i = 0; i < kMGPipeDirtyCount; ++i) {
            const auto bit = static_cast<MGPipeDirty>(i);
            const Bool emitted = (kMGPipeDirtyEmittedAtP4a & MGPipeDirtyBit(bit)) != 0;
            EXPECT_EQ(MGPipeSubsystemForDirty(bit) != 0, emitted) << kMGPipeDirtyNames[i];
        }
        // Each phase's constant SURVIVES as the next phase's A/B control, so the three are
        // pinned as a chain rather than one being edited into the next: 0x1ff is P4a's "T2"
        // arm and 0x7f is P3a's, and an operator's recorded mask has to keep meaning what it
        // meant.
        EXPECT_EQ(kMGPipeDirtyEmittedAtP4a & kMGPipeDirtyEmittedAtP3a, kMGPipeDirtyEmittedAtP3a);
        EXPECT_EQ(kMGPipeDirtyEmittedAtP3a & kMGPipeDirtyEmittedAtP2, kMGPipeDirtyEmittedAtP2);
        // The three bits P4a still does not emit for - the const-buffer, shader-buffer and
        // stream-output sets - name no subsystem, so their fields keep going through the
        // residual fill. Stated positively as well as through the loop above, because "only
        // these three are left" is the phase's own scope statement.
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewConstBuffers), 0u);
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewShaderBuffers), 0u);
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewSoTargets), 0u);
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewRenderState), kMGPipeSubsystemRenderState);
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewPixelPack), kMGPipeSubsystemPixelPack);
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewPatchState), kMGPipeSubsystemPatchState);
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewVertexAttribDefaults),
                  kMGPipeSubsystemVertexAttribDefaults);
        // P3a's three, one subsystem: an operator who clears bit 8 gets the whole legacy
        // vertex-input arm rather than two thirds of it.
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewVertexElements), kMGPipeSubsystemVertexInput);
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewVertexBuffers), kMGPipeSubsystemVertexInput);
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewIndexBuffer), kMGPipeSubsystemVertexInput);
        // P4a's seven, across FOUR subsystems, and the grouping is the whole point: the three
        // program bits are one family because an operator switching programs off has to get
        // the whole legacy arm, and so are the three unit-set bits.
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewShader), kMGPipeSubsystemPrograms);
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewShaderBindings), kMGPipeSubsystemPrograms);
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewGlobalConstants), kMGPipeSubsystemPrograms);
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewFramebuffer), kMGPipeSubsystemFramebuffer);
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewSamplerViews), kMGPipeSubsystemSamplers);
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewSamplers), kMGPipeSubsystemSamplers);
        EXPECT_EQ(MGPipeSubsystemForDirty(MGPipeDirty::NewShaderImages), kMGPipeSubsystemSamplers);
        // AND NO BIT NAMES THE TEXTURE-RESOURCE SUBSYSTEM. Its calls are dispatched from the
        // GL entry points that cause them - a constructor, a storage definition, a
        // glTexParameter - not from a dirty walk, exactly as P3a's buffer family is, so a bit
        // that started naming it would gate the emission twice and the two gates would
        // disagree the first time one of them was edited.
        for (SizeT i = 0; i < kMGPipeDirtyCount; ++i) {
            EXPECT_NE(MGPipeSubsystemForDirty(static_cast<MGPipeDirty>(i)),
                      kMGPipeSubsystemTextureResources)
                << kMGPipeDirtyNames[i];
        }
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
    // P3a D-I: bit 10's narrowed shutter
    // ===================================================================================
    //
    // NEW_INDEX_BUFFER used to be MixShutter(the whole buffer-CONTENT aggregate, the VAO
    // identity), so it fired on any buffer write anywhere - a glBufferSubData into a texture
    // upload staging buffer re-published the index binding. It now reads the bound VAO's own
    // element-slot version and the identity of whatever is bound to it.
    //
    // THIS IS THE ONE CASE THE OLD SHUTTER COULD NOT PASS, which is why it is here rather
    // than in the narrowing commit's prose.
    TEST_F(TrackerWalk, TheIndexBufferBitDoesNotFireOnAnUnrelatedBufferWrite) {
        const SharedPtr<MG_State::GLState::BufferObject> indices = Ctx().CreateBufferObject(1);
        indices->Respecify(64, nullptr);
        Ctx().GetBoundVertexArray()->GetIndexBufferBindingSlot().Bind(indices);
        Walk();
        Walk();
        ASSERT_EQ(m_tracker.LastDirty() & MGPipeDirtyBit(MGPipeDirty::NewIndexBuffer), 0u)
            << "the steady state must be quiet before the interesting half of this case";

        // An entirely unrelated buffer's contents move. Nothing about the element binding
        // changed, so the bit must stay down.
        const SharedPtr<MG_State::GLState::BufferObject> unrelated = Ctx().CreateBufferObject(2);
        unrelated->Respecify(4096, nullptr);
        Array<Uint8, 16> bytes{};
        unrelated->UploadSubData(DataPtr{bytes.data(), bytes.size()}, 0);
        ASSERT_NE(Ctx().GetAnyBufferChangeGeneration(), 0u) << "the buffer aggregate did move";
        Walk();
        EXPECT_EQ(m_tracker.LastDirty() & MGPipeDirtyBit(MGPipeDirty::NewIndexBuffer), 0u)
            << "NEW_INDEX_BUFFER fired on a write to a buffer that is not the element binding";

        // And the control, on the same tracker: the binding itself moving DOES fire it, so
        // the quiet above is a narrowing and not a dead bit.
        const SharedPtr<MG_State::GLState::BufferObject> other = Ctx().CreateBufferObject(3);
        other->Respecify(64, nullptr);
        Ctx().GetBoundVertexArray()->GetIndexBufferBindingSlot().Bind(other);
        Walk();
        EXPECT_NE(m_tracker.LastDirty() & MGPipeDirtyBit(MGPipeDirty::NewIndexBuffer), 0u)
            << "NEW_INDEX_BUFFER did not fire when the element binding changed";
        m_cache.Reset();
    }

    // The slot version is a WRAPPING Uint16 that BindingSlot bumps only on a real change, so
    // it is widened at this boundary - and the bound object's lifetime id joins it because
    // identity is what closes the wrap hole. 65536 binds later the version reads the same
    // number it did at the start; if that number were the whole shutter, a binding that had
    // moved onto a DIFFERENT buffer would read as unchanged and the draw would fetch indices
    // from the previous one.
    TEST_F(TrackerWalk, TheIndexBufferBitFiresWhenTheSlotVersionWrapsOntoADifferentBuffer) {
        const SharedPtr<MG_State::GLState::BufferObject> objects[3] = {
            Ctx().CreateBufferObject(1), Ctx().CreateBufferObject(2), Ctx().CreateBufferObject(3)};
        for (const auto& object : objects) object->Respecify(64, nullptr);
        auto& slot = Ctx().GetBoundVertexArray()->GetIndexBufferBindingSlot();

        slot.Bind(objects[0]);
        Walk();
        Walk();
        ASSERT_EQ(m_tracker.LastDirty() & MGPipeDirtyBit(MGPipeDirty::NewIndexBuffer), 0u);
        const Uint16 versionAtStart = slot.GetVersion();

        // Drive the Uint16 all the way round WITHOUT the tracker looking, which is exactly
        // the window a wrap needs: every bind between two walks is invisible to it.
        //
        // THREE buffers, not two, and that is the whole construction: BindingSlot bumps its
        // version only on a real change, so strictly alternating between two objects makes
        // the version and the bound object share a parity - 65536 changes always land back on
        // the object they started from, and the wrap is unobservable. Cycling three lands on
        // objects[65536 % 3] == objects[1] at exactly the same raw version.
        for (Uint32 i = 1; i <= 65536u; ++i) slot.Bind(objects[i % 3]);
        ASSERT_EQ(slot.GetVersion(), versionAtStart) << "the version did not come back round";
        ASSERT_EQ(slot.GetBoundObject(), objects[1]) << "the binding did not land on a different buffer";

        Walk();
        EXPECT_NE(m_tracker.LastDirty() & MGPipeDirtyBit(MGPipeDirty::NewIndexBuffer), 0u)
            << "the slot version wrapped onto a DIFFERENT buffer and the bit stayed down - the "
               "identity half of the shutter is what has to close that hole";
        m_cache.Reset();
    }

    // THE PENDING BASE INSTANCE IS THIS CALL'S ARGUMENT, NOT A LATCH, and Update() is where
    // the difference bites: it calls Reset() from inside itself whenever the current
    // GLContext pointer moves, and the draw entry point wrote the value one statement EARLIER
    // (D-H2.1 puts MGP_SET_BASE_INSTANCE immediately above MGP_FILL, and Update is inside
    // MGP_FILL). So `eglMakeCurrent(ctxB); glDrawArraysInstancedBaseInstance(..., 7)` used to
    // put a BaseInstance of 0 on the wire - one silently mis-shifted instanced draw per
    // context switch, on the emulation path, invisible to a single-context retrace corpus and
    // to every case that drives the emitter directly.
    //
    // A FRESH TRACKER IS EXACTLY THAT SWITCH: m_context starts null, so the first Walk() takes
    // the same `m_context != &ctx` branch a make-current does.
    TEST_F(TrackerWalk, ABaseInstanceSurvivesTheFirstWalkOnAFreshContext) {
        m_tracker.SetPendingBaseInstance(7);
        const Uint32 dirty = Walk();
        EXPECT_EQ(m_tracker.PendingBaseInstance(), 7u)
            << "the walk that follows a make-current cleared the base instance the same call set";
        EXPECT_NE(dirty & MGPipeDirtyBit(MGPipeDirty::NewVertexBuffers), 0u)
            << "NEW_VERTEX_BUFFERS did not fire on the first walk of a fresh context";

        // And the control, so the assertion above is about Reset() and not about a value that
        // is never cleared at all: the verb that consumes it clears it, and the next walk on
        // the SAME context then sees 0.
        m_tracker.ClearPendingBaseInstance();
        Walk();
        EXPECT_EQ(m_tracker.PendingBaseInstance(), 0u);
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

    // The window the repair covers: a glVertexAttrib* write followed by a verb whose class
    // does NOT read m_currentVertexAttribute. The call still goes out (the dirty bit and the
    // subsystem bit are all step 3 looks at), and today's applier writes four words into all
    // three views because it ignores ValueClass, so nothing in step 4 puts the converted
    // value back and the client repairs the mirror itself.
    //
    // THE ASSERTION IS THE INVARIANT, NOT THE DEFECT. "repairs == before + 1" would pin
    // today's applier and go red the day package A teaches
    // MGPipeApplySetVertexAttribDefaults to switch on MGPAttribValue::ValueClass - which is
    // the hand-off this package declares as blocking, and which is supposed to need no edit
    // here. What must hold either way is that the call went out naming exactly the attribute
    // that moved, and that the mirror ends up right by at most one repair: zero repairs once
    // the applier reproduces the value, one until then.
    TEST_F(TrackerShippedEmitter, APushedAttributeDefaultTheApplierCannotReproduceIsRepaired) {
        Draw();
        const Uint64 before = MGPipeVertexAttribDefaultRepairCount();
        // 1.5f is the point: its int view is 1 and its bit pattern is 0x3FC00000, so the two
        // cannot be the same four words whichever view the carrier picks.
        Ctx().SetCurrentVertexAttributeFloat(0, Array<Float, 4>{1.5f, 2.5f, 3.5f, 4.5f});
        MGPipeValidateForVerb(MGPipeVerb::GenerateMipmap);
        const MGPVertexAttribDefaults header = MGPipeVertexAttribDefaultsLastHeader();
        ASSERT_EQ(header.Count, 1u) << "the moved attribute default did not go out at all";
        EXPECT_EQ(header.Mask, 1u) << "the call named an attribute that did not move";
        const Uint64 repairs = MGPipeVertexAttribDefaultRepairCount() - before;
        EXPECT_LE(repairs, 1u) << "one call cannot need two repairs";
        // and the repair is not free-running: a second identical walk moves nothing, so it
        // neither re-emits nor re-repairs.
        MGPipeValidateForVerb(MGPipeVerb::GenerateMipmap);
        EXPECT_EQ(MGPipeVertexAttribDefaultRepairCount() - before, repairs);
    }

    // MAJOR 1 of round 2's review, pinned. glEnable(GL_CLIP_DISTANCE0) is one of the 35
    // capabilities the residual block carries AND one of the eight whose SetCapability arm
    // deliberately does not BumpVersions(), so it moves m_version alone. An arming condition
    // that reads the PIPELINE version - which is what this emitter used - never re-arms for
    // those eight, and nothing can see it downstream: a block that is not emitted cannot
    // diverge, so the trip wire is simply disarmed.
    TEST_F(TrackerShippedEmitter, AClipDistanceEnableReArmsTheResidualBlock) {
        Draw();
        ASSERT_TRUE(MGPipeApplier().HasResidual) << "the priming draw sent no residual block";
        // Poison the server's copy so a re-emission is the only thing that can restore it.
        MGPipeApplier().Residual = ResidualValueBlock{};
        MGPipeApplier().HasResidual = false;

        Ctx().SetCapability(CapabilityInput::ClipDistance0, true);
        // The premise: this moved the render-state counter and NOT the pipeline one.
        const Uint16 pipelineBefore = static_cast<Uint16>(Ctx().GetPipelineStateVersion());
        Draw();
        ASSERT_EQ(MGPipeTrackerInstance().LastDirty() & MGPipeDirtyBit(MGPipeDirty::NewPipelineState), 0u)
            << "the premise is gone: a clip-distance enable now moves the pipeline version";
        EXPECT_EQ(static_cast<Uint16>(Ctx().GetPipelineStateVersion()), pipelineBefore);

        ASSERT_TRUE(MGPipeApplier().HasResidual)
            << "a capability change that moves only m_version never re-armed the residual block";
        const Uint64 bit = Uint64{1} << static_cast<SizeT>(CapabilityInput::ClipDistance0);
        EXPECT_NE(MGPipeApplier().Residual.CapabilityBits & bit, 0ull)
            << "the re-emitted block does not carry the capability that moved";
    }

    // MAJOR 3 of round 2's review, pinned. A fresh context resets the tracker's staging
    // mirror to the GL defaults, which are exactly what a fresh GLContext holds - so the
    // per-attribute diff is empty on the one walk that must publish everything, while the
    // applier's mirror still holds the PREVIOUS context's defaults.
    TEST_F(TrackerShippedEmitter, AFreshContextRepublishesEveryVertexAttributeDefault) {
        // The fixture's context is itself fresh, so the priming draw is the first half of the
        // same statement: a fresh context publishes the COMPLETE set, not a difference.
        Draw();
        ASSERT_EQ(MGPipeVertexAttribDefaultsLastHeader().Count, 32u)
            << "the first walk on a fresh context published an increment, not a complete state";
        Ctx().SetCurrentVertexAttributeFloat(3, Array<Float, 4>{9.f, 8.f, 7.f, 6.f});
        Draw();
        ASSERT_EQ(MGPipeVertexAttribDefaultsLastHeader().Count, 1u)
            << "a steady context published more than the one attribute that moved";

        // A different context, whose 32 defaults are the value-initialised {0,0,0,1} the
        // tracker's own reset produces - so a diff against the staging mirror finds nothing.
        MG_State::pGLContext = MakeUnique<GLContext>();
        Draw();
        const MGPVertexAttribDefaults header = MGPipeVertexAttribDefaultsLastHeader();
        EXPECT_EQ(header.Count, 32u)
            << "a fresh context published " << header.Count
            << " attribute defaults; the server's mirror still holds the previous context's";
        EXPECT_EQ(header.Mask, 0xFFFFFFFFu);
    }

    // Minor 4 of round 2's review. The fresh-context reset of the applier and the CSO cache
    // used to sit inside EmitRenderState, i.e. behind bit 0 of MOBILEGL_PIPE_PUSH, so the
    // per-subsystem A/B D14 invites gave a fresh context a never-reset applier holding the
    // previous context's CSO records while every suppressor slot WAS invalidated.
    TEST_F(TrackerShippedEmitter, AFreshContextResetsTheApplierWithTheRenderStateSubsystemOff) {
        Draw();
        ASSERT_FALSE(MGPipeApplier().RenderStateCsos.empty())
            << "the priming draw created no CSO record to leak into the next context";

        MG_Config::Features.PipePush = kMGPipeSubsystemsMigratedAtP2 & ~kMGPipeSubsystemRenderState;
        MG_State::pGLContext = MakeUnique<GLContext>();
        Draw();
        EXPECT_TRUE(MGPipeApplier().RenderStateCsos.empty())
            << "a fresh context kept the previous context's CSO records because the reset was "
               "behind the render-state subsystem bit";
        EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundRenderStateCso));
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

    // B-C1's scenario END TO END, through the shipped entry point and read back off the real
    // applier: the setter runs, THEN a make-current happens inside the same fill (the fresh
    // GLContext below is the switch), and what set_vertex_buffers carries has to be the 7 the
    // draw entry point passed - not the 0 the tracker's context Reset used to leave behind.
    //
    // It also pins the two halves D-H2.3 makes one property: the applier's raw
    // VertexFetchBaseInstance, and the ContentHash the suppressor keys on - which has to mix
    // BaseInstance in, or the SECOND draw at a different base instance over the same buffer
    // set would be suppressed as unchanged and the server would keep the first one's shift.
    TEST_F(TrackerShippedEmitter, ABaseInstancedDrawAfterAMakeCurrentPublishesItsOwnBaseInstance) {
        MG_Config::Features.PipePush = kMGPipeSubsystemsMigratedAtP2 | kMGPipeSubsystemResources |
                                       kMGPipeSubsystemVertexInput;
        Draw(); // prime this context, so the switch below is a real make-current
        ASSERT_EQ(MGPipeApplier().VertexFetchBaseInstance, 0u);

        // The make-current, then the entry point's one line, then the fill - in that order,
        // which is the order GL_Drawing.cpp has.
        MG_State::pGLContext = MakeUnique<GLContext>();
        const SharedPtr<MG_State::GLState::BufferObject> vertices = Ctx().CreateBufferObject(1);
        vertices->Respecify(4096, nullptr);
        Ctx().GetBoundVertexArray()->SetAttributeFormat(0, 4, DataType::Float32, false, 16, 0, false, false, -1);
        Ctx().GetBoundVertexArray()->BindAttributeBuffer(0, vertices);
        Ctx().GetBoundVertexArray()->EnableAttribute(0);

        MGPipeSetPendingBaseInstance(7);
        ASSERT_EQ(MGPipePendingBaseInstance(), 7u) << "the setter did not take";
        Draw();

        EXPECT_EQ(MGPipeApplier().VertexFetchBaseInstance, 7u)
            << "the make-current between the setter and the fill ate the base instance";
        EXPECT_EQ(MGPipeApplier().VertexBufferCount, 1u);
        const Uint64 hashAtSeven = MGPipeVertexInputEmitterInstance().LastVertexBuffers().ContentHash;
        EXPECT_EQ(hashAtSeven,
                  MGPipeVertexBufferSetContentHash(MGPipeVertexInputEmitterInstance().LastEntries().data(), 0,
                                                   1, 7))
            << "the emitted set's ContentHash does not include the base instance it went out with";

        // CONSUMED by the verb that carried it: the next plain draw sees 0 again, and the
        // clear that makes that true is the validate point's, not MGPipeLeaveVerb's (no GL
        // entry point calls that one).
        EXPECT_EQ(MGPipePendingBaseInstance(), 0u);
        const Uint64 setsAtSeven = MGPipeVertexInputEmitterInstance().VertexBufferSetCount();
        Draw();
        EXPECT_EQ(MGPipeApplier().VertexFetchBaseInstance, 0u)
            << "a plain draw after a base-instanced one kept the previous fetch shift";
        EXPECT_GT(MGPipeVertexInputEmitterInstance().VertexBufferSetCount(), setsAtSeven)
            << "the buffer set was suppressed on a base-instance-only change - the hash or the "
               "bit-9 shutter is missing it";
        EXPECT_NE(MGPipeVertexInputEmitterInstance().LastVertexBuffers().ContentHash, hashAtSeven);

        // THE OTHER EXIT. MGPipeValidateForVerb returns early when there is no live context,
        // which skips step 3 and therefore skips step 3's clear - and since Reset() no longer
        // clears it either, that exit is the only path left on which a base instance could
        // stand into the next verb. A draw with no context is a no-op; its argument must not
        // outlive it.
        MGPipeSetPendingBaseInstance(11);
        UniquePtr<GLContext> parked = Move(MG_State::pGLContext);
        Draw();
        EXPECT_EQ(MGPipePendingBaseInstance(), 0u)
            << "the no-live-context exit left the draw's base instance standing for the next verb";
        MG_State::pGLContext = Move(parked);
    }

#endif // MOBILEGL_PIPE_PUSH
} // namespace
