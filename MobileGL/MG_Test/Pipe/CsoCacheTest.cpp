// MobileGL - MobileGL/MG_Test/Pipe/CsoCacheTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The render-state CSO cache (P2 brief D7). Owned by P2 package B (p2/tracker); the file and
// its CMake registration are the contract commit's.
//
// Needs the push sources, so every case is a visible SKIP in a pull build rather than a
// vanishing test.
#include <gtest/gtest.h>

#include "Includes.h"
#include <MG_Pipe/MGPipe.h>

#if MOBILEGL_PIPE_PUSH
#include <Config.h>
#include <MG_Impl/Pipe/CsoCache.h>
#include <MG_Impl/Pipe/SetHashSuppressor.h>
#include <MG_Pipe/MGPipeRenderStateSpans.h>
#include <MG_Pipe/PipeApply.h>
#endif

using namespace MobileGL;
using namespace MobileGL::MG_Pipe;

namespace {
#if !MOBILEGL_PIPE_PUSH
    TEST(CsoCache, SkippedInAPullBuild) {
        GTEST_SKIP() << "the CSO cache is compiled only under MOBILEGL_PIPE_PUSH";
    }
#else
    class CsoCacheTest : public ::testing::Test {
    protected:
        void SetUp() override {
            m_savedPush = MG_Config::Features.PipePush;
            MGPipeCsoCache::s_hashForTest = nullptr;
            MGPipeApplierReset();
        }
        void TearDown() override {
            MG_Config::Features.PipePush = m_savedPush;
            MGPipeCsoCache::s_hashForTest = nullptr;
            MGPipeApplierReset();
        }

        // A render state that differs from every other `seed` in a PIPELINE byte, so each one
        // is a genuinely different CSO. SampleMaskValue is in pipeline chunk P2.
        static RenderStateParameters PipelineState(Uint32 seed) {
            RenderStateParameters params{};
            params.SampleMaskValue = seed;
            return params;
        }

        Uint64 m_savedPush = 0;
    };

    TEST_F(CsoCacheTest, TheSameStateIsMintedOnceAndReusedForever) {
        MGPipeCsoCache cache;
        Uint64 bytes = 0;
        const RenderStateParameters params = PipelineState(7);
        const MGPipeHandle first = cache.Acquire(params, bytes);
        for (int i = 0; i < 16; ++i) EXPECT_TRUE(cache.Acquire(params, bytes) == first);
        EXPECT_EQ(cache.GetCounters().Mints, 1u);
        EXPECT_EQ(cache.GetCounters().Hits, 16u);
        EXPECT_EQ(cache.Size(), 1u);
        cache.Reset();
    }

    TEST_F(CsoCacheTest, LruEvictsTheOldestAndEmitsDelete) {
        MGPipeCsoCache cache;
        Uint64 bytes = 0;
        Vector<MGPipeHandle> handles;
        for (Uint32 i = 0; i < kMGPipeCsoCacheCapacity; ++i) {
            handles.push_back(cache.Acquire(PipelineState(i), bytes));
        }
        EXPECT_EQ(cache.Size(), kMGPipeCsoCacheCapacity);
        EXPECT_EQ(cache.GetCounters().Evictions, 0u);
        // Touch entry 0 so it is no longer the oldest; entry 1 becomes the victim.
        EXPECT_TRUE(cache.Acquire(PipelineState(0), bytes) == handles[0]);

        const MGPipeHandle overflow = cache.Acquire(PipelineState(kMGPipeCsoCacheCapacity), bytes);
        EXPECT_EQ(cache.Size(), kMGPipeCsoCacheCapacity);
        EXPECT_EQ(cache.GetCounters().Evictions, 1u);
        EXPECT_EQ(cache.GetCounters().Mints, kMGPipeCsoCacheCapacity + 1);
        EXPECT_FALSE(overflow == handles[1]);
        // The one that was touched survived; the evicted one has to be minted again.
        EXPECT_TRUE(cache.Acquire(PipelineState(0), bytes) == handles[0]);
        const MGPipeHandle reborn = cache.Acquire(PipelineState(1), bytes);
        EXPECT_FALSE(reborn == handles[1]);
        EXPECT_EQ(cache.GetCounters().Evictions, 2u);
        cache.Reset();
    }

    // A 64-bit hash collision between two different render states would alias them onto one
    // CSO, which is silent wrong pixels with no gate that can see it. The memcmp confirm is
    // what stops it, and this is what proves the memcmp is doing something.
    TEST_F(CsoCacheTest, HashCollisionDoesNotAliasTwoStates) {
        MGPipeCsoCache::s_hashForTest = [](const void*) -> Uint64 { return 0x1234'5678'9abc'def0ull; };
        MGPipeCsoCache cache;
        Uint64 bytes = 0;
        const MGPipeHandle a = cache.Acquire(PipelineState(1), bytes);
        const MGPipeHandle b = cache.Acquire(PipelineState(2), bytes);
        EXPECT_FALSE(a == b) << "two different render states were aliased onto one CSO";
        EXPECT_EQ(cache.GetCounters().Collisions, 1u);
        EXPECT_EQ(cache.GetCounters().Mints, 2u);
        EXPECT_EQ(cache.GetCounters().Hits, 0u);
        cache.Reset();
    }

    // The negative control the whole CSO design is measured against (ROADMAP.md P2). It turns
    // off the PROBE and the handle reuse, not the records - otherwise it would measure a
    // different design rather than this one without content addressing.
    TEST_F(CsoCacheTest, ContentAddressingOffMintsEveryTime) {
        MG_Config::Features.PipePush |= kMGPipeBehaviourNoCsoContentAddressing;
        MGPipeCsoCache cache;
        Uint64 bytes = 0;
        const RenderStateParameters params = PipelineState(3);
        const MGPipeHandle first = cache.Acquire(params, bytes);
        const MGPipeHandle second = cache.Acquire(params, bytes);
        const MGPipeHandle third = cache.Acquire(params, bytes);
        EXPECT_FALSE(first == second);
        EXPECT_FALSE(second == third);
        EXPECT_EQ(cache.GetCounters().Mints, 3u);
        EXPECT_EQ(cache.GetCounters().Hits, 0u);
        cache.Reset();
    }

    TEST_F(CsoCacheTest, EveryAcquireCountsItsPayloadBytes) {
        MGPipeCsoCache cache;
        Uint64 bytes = 0;
        cache.Acquire(PipelineState(11), bytes);
        // A mint puts the descriptor and the whole pipeline half on the wire.
        EXPECT_EQ(bytes, sizeof(MGPRenderStateDesc) + kMGPipePipelineChunkBytes);
        const Uint64 afterMint = bytes;
        cache.Acquire(PipelineState(11), bytes);
        // A hit puts NOTHING on the wire: the 12-byte bind is the caller's, not the cache's.
        EXPECT_EQ(bytes, afterMint);
        cache.Reset();
    }

    // ---- the set-hash suppressor (D11) ----

    TEST(SetHashSuppressorTest, TheFirstEmissionAlwaysGoesOutOnEverySlot) {
        MGPipeSetHashSuppressor suppressor;
        for (SizeT i = 0; i < kMGPipeSuppressorSlotCount; ++i) {
            const auto slot = static_cast<MGPipeSuppressorSlot>(i);
            EXPECT_EQ(suppressor.LastEmitted(slot), 0u) << "slot " << i << " did not start at 0";
            EXPECT_TRUE(suppressor.ShouldEmit(slot, 0)) << "slot " << i << " suppressed its first set";
            EXPECT_FALSE(suppressor.ShouldEmit(slot, 0)) << "slot " << i << " re-emitted an unmoved set";
        }
    }

    TEST(SetHashSuppressorTest, AComputedZeroIsRemappedSoItIsNeverConfusedWithNeverEmitted) {
        MGPipeSetHashSuppressor suppressor;
        const auto slot = MGPipeSuppressorSlot::SetVertexAttribDefaults;
        EXPECT_TRUE(suppressor.ShouldEmit(slot, 0));
        EXPECT_EQ(suppressor.LastEmitted(slot), 1u) << "a computed 0 must not read as never emitted";
        EXPECT_FALSE(suppressor.ShouldEmit(slot, 0));
    }

    TEST(SetHashSuppressorTest, SlotsAreIndependent) {
        MGPipeSetHashSuppressor suppressor;
        EXPECT_TRUE(suppressor.ShouldEmit(MGPipeSuppressorSlot::SetVertexBuffers, 42));
        EXPECT_TRUE(suppressor.ShouldEmit(MGPipeSuppressorSlot::SetSamplerViews, 42));
        EXPECT_FALSE(suppressor.ShouldEmit(MGPipeSuppressorSlot::SetVertexBuffers, 42));
    }

    TEST(SetHashSuppressorTest, InvalidateMakesTheNextSetGoOutWhateverItHashesTo) {
        MGPipeSetHashSuppressor suppressor;
        const auto slot = MGPipeSuppressorSlot::SetShaderImages;
        EXPECT_TRUE(suppressor.ShouldEmit(slot, 99));
        EXPECT_FALSE(suppressor.ShouldEmit(slot, 99));
        suppressor.Invalidate(slot);
        EXPECT_TRUE(suppressor.ShouldEmit(slot, 99));
        suppressor.InvalidateAll();
        EXPECT_TRUE(suppressor.ShouldEmit(slot, 99));
    }
#endif // MOBILEGL_PIPE_PUSH
} // namespace
