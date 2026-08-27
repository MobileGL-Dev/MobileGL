// MobileGL - MobileGL/MG_Test/Query/QueryTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ios>

#include "Includes.h"
#include "Init.h"
#include <Config.h>

#include <MG_Backend/BackendObjects.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/Query/GL_Query.h>
#include <MG_State/GLState/Core.h>

using namespace MobileGL;

namespace {
    // On the ctest host no ES context is ever current, but the timer-query
    // POINTERS of gBackendFunctionsTable.GL are NOT null: MG_Backend::Init
    // populates the real DirectGLES hooks (see GetBackendFunctions in
    // BackendObject_DirectGLES.cpp, which installs them whenever
    // MOBILEGL_DISABLE_TIMERQUERY is unset). Without a current ES context
    // those hooks degrade to returning null HANDLES, so the fallback these
    // tests exercise is the frontend's null-handle path (results immediately
    // available and zero), not a null-pointer path. Tests that need
    // controllable backend behavior install stubs and restore the table
    // afterwards.

    // Snapshots MG_Config::Features on construction and restores it on
    // destruction, so a test that flips DisableTimerQuery cannot leak the
    // setting into later tests even if an assertion unwinds the test body
    // early (same pattern as SanityTest's ScopedGLESCapabilitiesOverride).
    struct ScopedFeaturesOverride {
        ScopedFeaturesOverride(): m_snapshot(MG_Config::Features) {}
        ~ScopedFeaturesOverride() { MG_Config::Features = m_snapshot; }
        ScopedFeaturesOverride(const ScopedFeaturesOverride&) = delete;
        ScopedFeaturesOverride& operator=(const ScopedFeaturesOverride&) = delete;

    private:
        MG_Config::FeaturesTable m_snapshot;
    };

    // Snapshots the global backend function table on construction and restores
    // it on destruction, so stub timer-query pointers cannot leak into later
    // tests.
    struct ScopedBackendFunctionsOverride {
        ScopedBackendFunctionsOverride(): m_snapshot(MG_Backend::gBackendFunctionsTable) {}
        ~ScopedBackendFunctionsOverride() { MG_Backend::gBackendFunctionsTable = m_snapshot; }
        ScopedBackendFunctionsOverride(const ScopedBackendFunctionsOverride&) = delete;
        ScopedBackendFunctionsOverride& operator=(const ScopedBackendFunctionsOverride&) = delete;

    private:
        MG_Backend::GlobalBackendFunctionsTable m_snapshot;
    };

    // Stub backend timer-query implementation (plain function pointers, so the
    // observable state lives in file-scope globals).
    Int g_stubBeginCount = 0;
    Int g_stubEndCount = 0;
    Int g_stubCounterCount = 0;
    Int g_stubDeleteCount = 0;
    Bool g_stubTimerQuerySupported = true;
    Bool g_stubResultAvailable = true;
    // false = GetQueryResult64 cannot produce the result YET (returns false
    // without writing outNanoseconds), mirroring e.g. a Vulkan wait on a
    // not-yet-submitted frame serial.
    Bool g_stubResultObtainable = true;
    Uint64 g_stubResultNs = 0;

    Bool StubIsTimerQuerySupported() { return g_stubTimerQuerySupported; }

    MG_Backend::BackendQueryHandle StubBeginTimeElapsedQuery() {
        ++g_stubBeginCount;
        return reinterpret_cast<MG_Backend::BackendQueryHandle>(static_cast<uintptr_t>(0x51));
    }

    void StubEndTimeElapsedQuery(MG_Backend::BackendQueryHandle) { ++g_stubEndCount; }

    MG_Backend::BackendQueryHandle StubQueryCounterTimestamp() {
        ++g_stubCounterCount;
        return reinterpret_cast<MG_Backend::BackendQueryHandle>(static_cast<uintptr_t>(0x52));
    }

    Bool StubIsQueryResultAvailable(MG_Backend::BackendQueryHandle) { return g_stubResultAvailable; }

    Bool StubGetQueryResult64(MG_Backend::BackendQueryHandle, Bool, Uint64* outNanoseconds) {
        if (!g_stubResultObtainable) {
            return false;
        }
        *outNanoseconds = g_stubResultNs;
        return true;
    }

    void StubDeleteBackendQuery(MG_Backend::BackendQueryHandle) { ++g_stubDeleteCount; }

    void InstallStubBackendTimerQueries() {
        auto& backendGL = MG_Backend::gBackendFunctionsTable.GL;
        backendGL.IsTimerQuerySupported = StubIsTimerQuerySupported;
        backendGL.BeginTimeElapsedQuery = StubBeginTimeElapsedQuery;
        backendGL.EndTimeElapsedQuery = StubEndTimeElapsedQuery;
        backendGL.QueryCounterTimestamp = StubQueryCounterTimestamp;
        backendGL.IsQueryResultAvailable = StubIsQueryResultAvailable;
        backendGL.GetQueryResult64 = StubGetQueryResult64;
        backendGL.DeleteBackendQuery = StubDeleteBackendQuery;
        g_stubBeginCount = 0;
        g_stubEndCount = 0;
        g_stubCounterCount = 0;
        g_stubDeleteCount = 0;
        g_stubTimerQuerySupported = true;
        g_stubResultAvailable = true;
        g_stubResultObtainable = true;
        g_stubResultNs = 0;
    }

    // Stub backend transform feedback primitive queries. g_stubXfbQuerySupported = false
    // models a backend with no GPU counter at all (null handle), which is what leaves the
    // frontend's CPU accounting as the only source; g_stubResultNs is what the "driver"
    // would answer when its query IS read, deliberately set to a value the CPU accounting
    // never produces so the two sources are told apart.
    Int g_stubXfbBeginCount = 0;
    Int g_stubXfbEndCount = 0;
    Bool g_stubXfbQuerySupported = true;

    MG_Backend::BackendQueryHandle StubBeginXfbPrimitivesQuery(Bool) {
        if (!g_stubXfbQuerySupported) {
            return nullptr;
        }
        ++g_stubXfbBeginCount;
        return reinterpret_cast<MG_Backend::BackendQueryHandle>(static_cast<uintptr_t>(0x53));
    }

    void StubEndXfbPrimitivesQuery(MG_Backend::BackendQueryHandle) { ++g_stubXfbEndCount; }

    // Stub backend occlusion queries. The host has no ES context, and BeginQuery refuses the
    // occlusion targets outright when the backend advertises no hook - so a conditional-render
    // test cannot get a legal predicate object without these. g_stubResultNs is the sample count
    // the "driver" reports, which is the whole input to the predicate.
    MG_Backend::BackendQueryHandle StubBeginOcclusionQuery() {
        return reinterpret_cast<MG_Backend::BackendQueryHandle>(static_cast<uintptr_t>(0x54));
    }

    void StubEndOcclusionQuery(MG_Backend::BackendQueryHandle) {}

    void InstallStubBackendOcclusionQueries() {
        auto& backendGL = MG_Backend::gBackendFunctionsTable.GL;
        backendGL.BeginOcclusionQuery = StubBeginOcclusionQuery;
        backendGL.EndOcclusionQuery = StubEndOcclusionQuery;
        backendGL.IsQueryResultAvailable = StubIsQueryResultAvailable;
        backendGL.GetQueryResult64 = StubGetQueryResult64;
        backendGL.DeleteBackendQuery = StubDeleteBackendQuery;
        g_stubDeleteCount = 0;
        g_stubResultAvailable = true;
        g_stubResultObtainable = true;
        g_stubResultNs = 0;
    }

    void InstallStubBackendXfbQueries() {
        auto& backendGL = MG_Backend::gBackendFunctionsTable.GL;
        backendGL.BeginXfbPrimitivesQuery = StubBeginXfbPrimitivesQuery;
        backendGL.EndXfbPrimitivesQuery = StubEndXfbPrimitivesQuery;
        backendGL.IsQueryResultAvailable = StubIsQueryResultAvailable;
        backendGL.GetQueryResult64 = StubGetQueryResult64;
        backendGL.DeleteBackendQuery = StubDeleteBackendQuery;
        // Off by default: the tests that exercise the DirectGLES preference turn it on.
        backendGL.PrefersCpuXfbPrimitiveAccounting = false;
        g_stubXfbBeginCount = 0;
        g_stubXfbEndCount = 0;
        g_stubXfbQuerySupported = true;
        g_stubDeleteCount = 0;
        g_stubResultAvailable = true;
        g_stubResultObtainable = true;
        g_stubResultNs = 0;
    }

    // What AccountTransformFeedbackPrimitives (GL_Drawing.cpp) records for one captured
    // draw, without needing a draw: `assembled` primitives came out of the vertex stage
    // and `written` of them fitted in the capture buffers (they differ once the buffers
    // overflow, which is the whole point of PRIMITIVES_WRITTEN).
    void SimulateAccountedCaptureDraw(Uint64 assembled, Uint64 written, Bool throughGeometryStage = false) {
        MG_State::pGLContext->AddTransformFeedbackInputPrimitives(assembled);
        if (throughGeometryStage) {
            MG_State::pGLContext->AddTransformFeedbackGeometryCaptureDraw();
        }
        MG_State::pGLContext->AddTransformFeedbackPrimitives(written);
        MG_State::pGLContext->AddTransformFeedbackAccountedCaptureDraw();
    }
} // namespace

class QueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        MobileGL::Initialize();
        // Drain errors recorded by earlier tests so assertions here are
        // attributable to this test alone (GetError pops one queued error
        // per call).
        while (MG_Impl::GLImpl::GetError() != GL_NO_ERROR) {
        }
    }
};

TEST_F(QueryTest, GenQueriesReturnsDistinctNonzeroIdsAndTracksLiveness) {
    GLuint ids[3] = {0, 0, 0};
    MG_Impl::GLImpl::GenQueries(3, ids);

    EXPECT_NE(ids[0], 0u);
    EXPECT_NE(ids[1], 0u);
    EXPECT_NE(ids[2], 0u);
    EXPECT_NE(ids[0], ids[1]);
    EXPECT_NE(ids[0], ids[2]);
    EXPECT_NE(ids[1], ids[2]);

    // GenQueries only reserves names: "they acquire query state only when they are first used by
    // calling BeginQuery" (GL 4.6 core 4.2.1), and IsQuery answers for objects, not for reserved
    // names. So all three read as FALSE here even though the names are taken.
    EXPECT_EQ(MG_Impl::GLImpl::IsQuery(0), GL_FALSE);
    EXPECT_EQ(MG_Impl::GLImpl::IsQuery(ids[0]), GL_FALSE);
    EXPECT_EQ(MG_Impl::GLImpl::IsQuery(ids[1]), GL_FALSE);
    EXPECT_EQ(MG_Impl::GLImpl::IsQuery(ids[2]), GL_FALSE);

    // First use is what creates the object.
    MG_Impl::GLImpl::BeginQuery(GL_TIME_ELAPSED, ids[0]);
    MG_Impl::GLImpl::EndQuery(GL_TIME_ELAPSED);
    EXPECT_EQ(MG_Impl::GLImpl::IsQuery(ids[0]), GL_TRUE);
    EXPECT_EQ(MG_Impl::GLImpl::IsQuery(ids[1]), GL_FALSE);

    MG_Impl::GLImpl::DeleteQueries(3, ids);
    EXPECT_EQ(MG_Impl::GLImpl::IsQuery(ids[0]), GL_FALSE);
    EXPECT_EQ(MG_Impl::GLImpl::IsQuery(ids[1]), GL_FALSE);
    EXPECT_EQ(MG_Impl::GLImpl::IsQuery(ids[2]), GL_FALSE);

    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// The direct state access counterpart: glCreateQueries creates the object outright, so unlike a
// glGenQueries name it is a query before anything is ever recorded into it.
TEST_F(QueryTest, CreateQueriesYieldsQueryObjectsImmediately) {
    GLuint ids[2] = {0, 0};
    MG_Impl::GLImpl::CreateQueries(GL_TIME_ELAPSED, 2, ids);

    EXPECT_NE(ids[0], 0u);
    EXPECT_NE(ids[1], 0u);
    EXPECT_NE(ids[0], ids[1]);

    EXPECT_EQ(MG_Impl::GLImpl::IsQuery(ids[0]), GL_TRUE);
    EXPECT_EQ(MG_Impl::GLImpl::IsQuery(ids[1]), GL_TRUE);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::DeleteQueries(2, ids);
    EXPECT_EQ(MG_Impl::GLImpl::IsQuery(ids[0]), GL_FALSE);
    EXPECT_EQ(MG_Impl::GLImpl::IsQuery(ids[1]), GL_FALSE);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(QueryTest, TimeElapsedSpanFallsBackToImmediateZeroResult) {
    GLuint id = 0;
    MG_Impl::GLImpl::GenQueries(1, &id);
    ASSERT_NE(id, 0u);

    MG_Impl::GLImpl::BeginQuery(GL_TIME_ELAPSED, id);
    MG_Impl::GLImpl::EndQuery(GL_TIME_ELAPSED);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // With null backend timer-query pointers (no ES context on the test host)
    // the result is immediately available and reads as zero.
    GLint available = -1;
    MG_Impl::GLImpl::GetQueryObjectiv(id, GL_QUERY_RESULT_AVAILABLE, &available);
    EXPECT_EQ(available, 1);

    GLint64 result = -1;
    MG_Impl::GLImpl::GetQueryObjecti64v(id, GL_QUERY_RESULT, &result);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::DeleteQueries(1, &id);
}

TEST_F(QueryTest, NestedBeginQueryRecordsInvalidOperation) {
    GLuint ids[2] = {0, 0};
    MG_Impl::GLImpl::GenQueries(2, ids);

    MG_Impl::GLImpl::BeginQuery(GL_TIME_ELAPSED, ids[0]);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::BeginQuery(GL_TIME_ELAPSED, ids[1]);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // The failed nested begin must not have displaced the active query.
    GLint currentQuery = -1;
    MG_Impl::GLImpl::GetQueryiv(GL_TIME_ELAPSED, GL_CURRENT_QUERY, &currentQuery);
    EXPECT_EQ(currentQuery, static_cast<GLint>(ids[0]));

    MG_Impl::GLImpl::EndQuery(GL_TIME_ELAPSED);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::DeleteQueries(2, ids);
}

TEST_F(QueryTest, EndQueryWithoutActiveQueryRecordsInvalidOperation) {
    MG_Impl::GLImpl::EndQuery(GL_TIME_ELAPSED);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);
}

TEST_F(QueryTest, QueryCounterTimestampResultImmediatelyAvailable) {
    GLuint id = 0;
    MG_Impl::GLImpl::GenQueries(1, &id);
    ASSERT_NE(id, 0u);

    MG_Impl::GLImpl::QueryCounter(id, GL_TIMESTAMP);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLint available = -1;
    MG_Impl::GLImpl::GetQueryObjectiv(id, GL_QUERY_RESULT_AVAILABLE, &available);
    EXPECT_EQ(available, 1);

    GLuint64 result = 123u;
    MG_Impl::GLImpl::GetQueryObjectui64v(id, GL_QUERY_RESULT, &result);
    EXPECT_EQ(result, 0u);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::DeleteQueries(1, &id);
}

TEST_F(QueryTest, GetQueryivCurrentQueryTracksActiveId) {
    GLint currentQuery = -1;
    MG_Impl::GLImpl::GetQueryiv(GL_TIME_ELAPSED, GL_CURRENT_QUERY, &currentQuery);
    EXPECT_EQ(currentQuery, 0);

    GLuint id = 0;
    MG_Impl::GLImpl::GenQueries(1, &id);
    ASSERT_NE(id, 0u);

    MG_Impl::GLImpl::BeginQuery(GL_TIME_ELAPSED, id);
    MG_Impl::GLImpl::GetQueryiv(GL_TIME_ELAPSED, GL_CURRENT_QUERY, &currentQuery);
    EXPECT_EQ(currentQuery, static_cast<GLint>(id));

    // GL_TIMESTAMP queries are never active, so GL_CURRENT_QUERY stays 0 there.
    MG_Impl::GLImpl::GetQueryiv(GL_TIMESTAMP, GL_CURRENT_QUERY, &currentQuery);
    EXPECT_EQ(currentQuery, 0);

    MG_Impl::GLImpl::EndQuery(GL_TIME_ELAPSED);
    MG_Impl::GLImpl::GetQueryiv(GL_TIME_ELAPSED, GL_CURRENT_QUERY, &currentQuery);
    EXPECT_EQ(currentQuery, 0);

    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::DeleteQueries(1, &id);
}

TEST_F(QueryTest, QueryCounterBitsReportsZeroWhenTimerQueryDisabled) {
    const ScopedFeaturesOverride featuresGuard;
    const ScopedBackendFunctionsOverride backendGuard;
    InstallStubBackendTimerQueries();

    // With the stub backend reporting live timer-query support and the
    // feature enabled, the frontend advertises 64-bit counters for both timer
    // targets.
    MG_Config::Features.DisableTimerQuery = false;
    GLint counterBits = -1;
    MG_Impl::GLImpl::GetQueryiv(GL_TIME_ELAPSED, GL_QUERY_COUNTER_BITS, &counterBits);
    EXPECT_EQ(counterBits, 64);
    MG_Impl::GLImpl::GetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &counterBits);
    EXPECT_EQ(counterBits, 64);

    // MOBILEGL_DISABLE_TIMERQUERY zeroes the advertised counter bits even when
    // the backend supports timer queries.
    MG_Config::Features.DisableTimerQuery = true;
    MG_Impl::GLImpl::GetQueryiv(GL_TIME_ELAPSED, GL_QUERY_COUNTER_BITS, &counterBits);
    EXPECT_EQ(counterBits, 0);
    MG_Impl::GLImpl::GetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &counterBits);
    EXPECT_EQ(counterBits, 0);

    // A disabled span must not touch the backend and must read back as an
    // immediately available zero result.
    GLuint id = 0;
    MG_Impl::GLImpl::GenQueries(1, &id);
    MG_Impl::GLImpl::BeginQuery(GL_TIME_ELAPSED, id);
    MG_Impl::GLImpl::EndQuery(GL_TIME_ELAPSED);
    EXPECT_EQ(g_stubBeginCount, 0);

    GLint available = -1;
    MG_Impl::GLImpl::GetQueryObjectiv(id, GL_QUERY_RESULT_AVAILABLE, &available);
    EXPECT_EQ(available, 1);
    GLuint64 result = 123u;
    MG_Impl::GLImpl::GetQueryObjectui64v(id, GL_QUERY_RESULT, &result);
    EXPECT_EQ(result, 0u);

    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::DeleteQueries(1, &id);
}

TEST_F(QueryTest, QueryCounterBitsReportsZeroWhenBackendUnsupported) {
    const ScopedFeaturesOverride featuresGuard;
    const ScopedBackendFunctionsOverride backendGuard;
    InstallStubBackendTimerQueries();
    MG_Config::Features.DisableTimerQuery = false;

    // All backend hooks are installed and the feature is enabled, but the
    // dynamic support check says the live backend cannot time right now
    // (e.g. missing extension or zero timestamp valid bits) - the advertised
    // counter bits must read 0 for both timer targets.
    g_stubTimerQuerySupported = false;
    GLint counterBits = -1;
    MG_Impl::GLImpl::GetQueryiv(GL_TIME_ELAPSED, GL_QUERY_COUNTER_BITS, &counterBits);
    EXPECT_EQ(counterBits, 0);
    counterBits = -1;
    MG_Impl::GLImpl::GetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &counterBits);
    EXPECT_EQ(counterBits, 0);

    // Support coming back (fresh caps after a context recreation) flips the
    // advertisement back to 64 without reinstalling the table.
    g_stubTimerQuerySupported = true;
    MG_Impl::GLImpl::GetQueryiv(GL_TIME_ELAPSED, GL_QUERY_COUNTER_BITS, &counterBits);
    EXPECT_EQ(counterBits, 64);

    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(QueryTest, FailedResultWaitKeepsHandleUntilResultLands) {
    const ScopedFeaturesOverride featuresGuard;
    const ScopedBackendFunctionsOverride backendGuard;
    InstallStubBackendTimerQueries();
    MG_Config::Features.DisableTimerQuery = false;
    g_stubResultObtainable = false;
    g_stubResultNs = 777;

    GLuint id = 0;
    MG_Impl::GLImpl::GenQueries(1, &id);
    ASSERT_NE(id, 0u);
    MG_Impl::GLImpl::BeginQuery(GL_TIME_ELAPSED, id);
    MG_Impl::GLImpl::EndQuery(GL_TIME_ELAPSED);

    // A wait the backend cannot satisfy yet (e.g. Vulkan refusing to block on
    // the current unsubmitted frame serial) reads 0 for this call - without
    // recording an error - and must NOT consume the backend handle or cache
    // the zero.
    GLuint64 result = 123u;
    MG_Impl::GLImpl::GetQueryObjectui64v(id, GL_QUERY_RESULT, &result);
    EXPECT_EQ(result, 0u);
    EXPECT_EQ(g_stubDeleteCount, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // Repeated failed waits behave identically...
    result = 123u;
    MG_Impl::GLImpl::GetQueryObjectui64v(id, GL_QUERY_RESULT, &result);
    EXPECT_EQ(result, 0u);
    EXPECT_EQ(g_stubDeleteCount, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // ...and availability still polls the backend afterwards (the failed
    // read did not force-complete the query).
    GLint available = -1;
    g_stubResultAvailable = false;
    MG_Impl::GLImpl::GetQueryObjectiv(id, GL_QUERY_RESULT_AVAILABLE, &available);
    EXPECT_EQ(available, 0);
    g_stubResultAvailable = true;
    MG_Impl::GLImpl::GetQueryObjectiv(id, GL_QUERY_RESULT_AVAILABLE, &available);
    EXPECT_EQ(available, 1);

    // Once the backend can produce the value, the same query returns the
    // real result; only then is the backend handle released and the value
    // cached for later reads.
    g_stubResultObtainable = true;
    result = 0;
    MG_Impl::GLImpl::GetQueryObjectui64v(id, GL_QUERY_RESULT, &result);
    EXPECT_EQ(result, 777u);
    EXPECT_EQ(g_stubDeleteCount, 1);
    result = 0;
    MG_Impl::GLImpl::GetQueryObjectui64v(id, GL_QUERY_RESULT, &result);
    EXPECT_EQ(result, 777u);
    EXPECT_EQ(g_stubDeleteCount, 1);

    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::DeleteQueries(1, &id);
    EXPECT_EQ(g_stubDeleteCount, 1); // handle already released by the result read
}

TEST_F(QueryTest, BackendResultsPropagateThroughFrontend) {
    const ScopedFeaturesOverride featuresGuard;
    const ScopedBackendFunctionsOverride backendGuard;
    InstallStubBackendTimerQueries();
    MG_Config::Features.DisableTimerQuery = false;
    g_stubResultAvailable = false;
    g_stubResultNs = 42;

    GLuint id = 0;
    MG_Impl::GLImpl::GenQueries(1, &id);
    MG_Impl::GLImpl::BeginQuery(GL_TIME_ELAPSED, id);
    MG_Impl::GLImpl::EndQuery(GL_TIME_ELAPSED);
    EXPECT_EQ(g_stubBeginCount, 1);
    EXPECT_EQ(g_stubEndCount, 1);

    // Availability follows the backend while the result has not been read.
    GLint available = -1;
    MG_Impl::GLImpl::GetQueryObjectiv(id, GL_QUERY_RESULT_AVAILABLE, &available);
    EXPECT_EQ(available, 0);
    g_stubResultAvailable = true;
    MG_Impl::GLImpl::GetQueryObjectiv(id, GL_QUERY_RESULT_AVAILABLE, &available);
    EXPECT_EQ(available, 1);

    // Reading the result consumes the backend handle exactly once and caches
    // the value for later reads.
    GLuint64 result = 0;
    MG_Impl::GLImpl::GetQueryObjectui64v(id, GL_QUERY_RESULT, &result);
    EXPECT_EQ(result, 42u);
    EXPECT_EQ(g_stubDeleteCount, 1);
    result = 0;
    MG_Impl::GLImpl::GetQueryObjectui64v(id, GL_QUERY_RESULT, &result);
    EXPECT_EQ(result, 42u);
    EXPECT_EQ(g_stubDeleteCount, 1);

    MG_Impl::GLImpl::DeleteQueries(1, &id);
    EXPECT_EQ(g_stubDeleteCount, 1); // handle already released by the result read

    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// The two transform feedback targets count different things and must therefore read
// different counters: PRIMITIVES_WRITTEN what the capture buffers took, PRIMITIVES_GENERATED
// every primitive the capture stage assembled - including the ones a paused span threw away,
// which are generated but never written. Answering both from the written counter (as the
// fallback used to) reports the clamped number as the generated one.
TEST_F(QueryTest, TransformFeedbackQueryTargetsReadTheirOwnCounter) {
    const ScopedBackendFunctionsOverride backendGuard;
    InstallStubBackendXfbQueries();
    g_stubXfbQuerySupported = false; // no GPU counter: the CPU accounting is the only source

    GLuint ids[2] = {0, 0};
    MG_Impl::GLImpl::GenQueries(2, ids);
    ASSERT_NE(ids[0], 0u);
    ASSERT_NE(ids[1], 0u);

    MG_Impl::GLImpl::BeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, ids[0]);
    MG_Impl::GLImpl::BeginQuery(GL_PRIMITIVES_GENERATED, ids[1]);
    // Four points assembled into a buffer with room for three.
    SimulateAccountedCaptureDraw(/*assembled=*/4, /*written=*/3);
    // ...and two more points assembled while the span was paused: generated, never written.
    MG_State::pGLContext->AddTransformFeedbackPausedPrimitives(2);
    MG_Impl::GLImpl::EndQuery(GL_PRIMITIVES_GENERATED);
    MG_Impl::GLImpl::EndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);

    GLuint written = 0;
    GLuint generated = 0;
    MG_Impl::GLImpl::GetQueryObjectuiv(ids[0], GL_QUERY_RESULT, &written);
    MG_Impl::GLImpl::GetQueryObjectuiv(ids[1], GL_QUERY_RESULT, &generated);
    EXPECT_EQ(written, 3u);
    EXPECT_EQ(generated, 6u);

    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::DeleteQueries(2, ids);
}

// A query span that captured nothing at all reads zero from the CPU accounting rather than
// the unsigned wrap-around a bare End-minus-Begin subtraction produces the moment the
// snapshot is not below the counter (GetQueryObjectuiv would hand the app 4294967295).
TEST_F(QueryTest, AnEmptyTransformFeedbackSpanReadsZero) {
    const ScopedBackendFunctionsOverride backendGuard;
    InstallStubBackendXfbQueries();
    g_stubXfbQuerySupported = false;

    GLuint id = 0;
    MG_Impl::GLImpl::GenQueries(1, &id);
    ASSERT_NE(id, 0u);

    MG_Impl::GLImpl::BeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, id);
    MG_Impl::GLImpl::EndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);

    GLuint result = 123u;
    MG_Impl::GLImpl::GetQueryObjectuiv(id, GL_QUERY_RESULT, &result);
    EXPECT_EQ(result, 0u);

    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::DeleteQueries(1, &id);
}

// The DirectGLES preference: for a capture the frontend counted exactly - every draw
// accounted, none of them amplified by a geometry stage - the CPU number is the
// desktop-exact one and the ES driver's PRIMITIVES_WRITTEN counter is not consulted, even
// though the backend query ran. The backend query object is released at EndQuery instead of
// being left to a result read that will never come.
TEST_F(QueryTest, VertexOnlyCaptureSpansPreferTheCpuPrimitiveAccounting) {
    const ScopedBackendFunctionsOverride backendGuard;
    InstallStubBackendXfbQueries();
    MG_Backend::gBackendFunctionsTable.GL.PrefersCpuXfbPrimitiveAccounting = true;
    g_stubResultNs = 6; // what the driver's counter would have said - twice the truth

    GLuint id = 0;
    MG_Impl::GLImpl::GenQueries(1, &id);
    ASSERT_NE(id, 0u);

    MG_Impl::GLImpl::BeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, id);
    SimulateAccountedCaptureDraw(/*assembled=*/4, /*written=*/3);
    MG_Impl::GLImpl::EndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    EXPECT_EQ(g_stubXfbBeginCount, 1);
    EXPECT_EQ(g_stubXfbEndCount, 1);
    EXPECT_EQ(g_stubDeleteCount, 1); // ended, then released - not leaked

    GLint available = -1;
    MG_Impl::GLImpl::GetQueryObjectiv(id, GL_QUERY_RESULT_AVAILABLE, &available);
    EXPECT_EQ(available, 1);

    GLuint result = 0;
    MG_Impl::GLImpl::GetQueryObjectuiv(id, GL_QUERY_RESULT, &result);
    EXPECT_EQ(result, 3u);
    EXPECT_EQ(g_stubDeleteCount, 1); // the read had no handle left to release

    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::DeleteQueries(1, &id);
    EXPECT_EQ(g_stubDeleteCount, 1);
}

// The regression gate for that preference: a capture fed by a geometry stage writes whatever
// the shader emits, which the CPU accounting cannot model, so the backend's counter stays the
// answer and its handle survives EndQuery to be read later.
TEST_F(QueryTest, AGeometryStageCaptureKeepsTheBackendPrimitiveResult) {
    const ScopedBackendFunctionsOverride backendGuard;
    InstallStubBackendXfbQueries();
    MG_Backend::gBackendFunctionsTable.GL.PrefersCpuXfbPrimitiveAccounting = true;
    g_stubResultNs = 9; // the amplified count only the driver knows

    GLuint id = 0;
    MG_Impl::GLImpl::GenQueries(1, &id);
    ASSERT_NE(id, 0u);

    MG_Impl::GLImpl::BeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, id);
    SimulateAccountedCaptureDraw(/*assembled=*/1, /*written=*/1, /*throughGeometryStage=*/true);
    MG_Impl::GLImpl::EndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    EXPECT_EQ(g_stubDeleteCount, 0); // still to be read

    GLuint result = 0;
    MG_Impl::GLImpl::GetQueryObjectuiv(id, GL_QUERY_RESULT, &result);
    EXPECT_EQ(result, 9u);
    EXPECT_EQ(g_stubDeleteCount, 1);

    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::DeleteQueries(1, &id);
}

// The other half of that gate: the instanced, indirect and multi-draw entry points never
// reach the CPU accounting, so a span made of those moves no counter at all. Its delta would
// be zero, which is not "nothing was written" - it is "nothing was counted" - and the
// backend's result has to stand.
TEST_F(QueryTest, ACaptureSpanTheAccountingNeverSawKeepsTheBackendResult) {
    const ScopedBackendFunctionsOverride backendGuard;
    InstallStubBackendXfbQueries();
    MG_Backend::gBackendFunctionsTable.GL.PrefersCpuXfbPrimitiveAccounting = true;
    g_stubResultNs = 12;

    GLuint id = 0;
    MG_Impl::GLImpl::GenQueries(1, &id);
    ASSERT_NE(id, 0u);

    MG_Impl::GLImpl::BeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, id);
    MG_Impl::GLImpl::EndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);

    GLuint result = 0;
    MG_Impl::GLImpl::GetQueryObjectuiv(id, GL_QUERY_RESULT, &result);
    EXPECT_EQ(result, 12u);

    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::DeleteQueries(1, &id);
}

// GL_PRIMITIVES_GENERATED counts primitives whether or not a capture is active, while the
// CPU accounting only ever sees capture draws - so the preference above deliberately does
// not extend to that target, whatever the backend asked for.
TEST_F(QueryTest, PrimitivesGeneratedKeepsTheBackendResultUnderTheCpuPreference) {
    const ScopedBackendFunctionsOverride backendGuard;
    InstallStubBackendXfbQueries();
    MG_Backend::gBackendFunctionsTable.GL.PrefersCpuXfbPrimitiveAccounting = true;
    g_stubResultNs = 7;

    GLuint id = 0;
    MG_Impl::GLImpl::GenQueries(1, &id);
    ASSERT_NE(id, 0u);

    MG_Impl::GLImpl::BeginQuery(GL_PRIMITIVES_GENERATED, id);
    SimulateAccountedCaptureDraw(/*assembled=*/4, /*written=*/3);
    MG_Impl::GLImpl::EndQuery(GL_PRIMITIVES_GENERATED);
    EXPECT_EQ(g_stubDeleteCount, 0);

    GLuint result = 0;
    MG_Impl::GLImpl::GetQueryObjectuiv(id, GL_QUERY_RESULT, &result);
    EXPECT_EQ(result, 7u);

    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::DeleteQueries(1, &id);
}

// Environment-agnostic property test for the env -> ConfigLoader -> Features
// chain: whatever MOBILEGL_DISABLE_TIMERQUERY is set to in the environment of
// this test process, MG_ConfigLoader::Init must have parsed it with the
// unified truthy rule (set, non-empty, not "0", case-insensitive not "false").
// Running the binary under MOBILEGL_DISABLE_TIMERQUERY=1 therefore exercises
// the real end-to-end path rather than the struct field alone.
// KHR-GL43.compute_shader.conditional-dispatching and the conditional_render family.
// glBeginConditionalRender/glEndConditionalRender were bare stubs: every command inside a
// conditional block executed whatever the query said, so the block that should have been
// discarded ran and doubled the atomic counter the case reads back.
TEST_F(QueryTest, ConditionalRenderResolvesItsPredicateFromTheOcclusionQuery) {
    ScopedBackendFunctionsOverride backendGuard;
    InstallStubBackendOcclusionQueries();

    GLuint ids[2] = {0, 0};
    MG_Impl::GLImpl::GenQueries(2, ids);
    ASSERT_NE(ids[0], 0u);
    ASSERT_NE(ids[1], 0u);

    // One span that saw samples and one that saw none, which is exactly the pair the
    // conformance case builds out of a passing and a failing depth test.
    g_stubResultNs = 1;
    MG_Impl::GLImpl::BeginQuery(GL_ANY_SAMPLES_PASSED, ids[0]);
    MG_Impl::GLImpl::EndQuery(GL_ANY_SAMPLES_PASSED);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    GLuint passedResult = 0xFFFFFFFFu;
    MG_Impl::GLImpl::GetQueryObjectuiv(ids[0], GL_QUERY_RESULT, &passedResult);
    ASSERT_EQ(passedResult, 1u);

    g_stubResultNs = 0;
    MG_Impl::GLImpl::BeginQuery(GL_ANY_SAMPLES_PASSED, ids[1]);
    MG_Impl::GLImpl::EndQuery(GL_ANY_SAMPLES_PASSED);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // A block on the query that passed executes.
    MG_Impl::GLImpl::BeginConditionalRender(ids[0], GL_QUERY_WAIT);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_TRUE(MG_State::pGLContext->IsConditionalRenderActive());
    EXPECT_FALSE(MG_State::pGLContext->ConditionalRenderDiscardsCommands());
    MG_Impl::GLImpl::EndConditionalRender();
    EXPECT_FALSE(MG_State::pGLContext->IsConditionalRenderActive());
    EXPECT_FALSE(MG_State::pGLContext->ConditionalRenderDiscardsCommands());

    // A block on the query that did not passes nothing through.
    MG_Impl::GLImpl::BeginConditionalRender(ids[1], GL_QUERY_WAIT);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_TRUE(MG_State::pGLContext->ConditionalRenderDiscardsCommands());
    MG_Impl::GLImpl::EndConditionalRender();

    // ...and the _INVERTED modes swap both verdicts.
    MG_Impl::GLImpl::BeginConditionalRender(ids[0], GL_QUERY_WAIT_INVERTED);
    EXPECT_TRUE(MG_State::pGLContext->ConditionalRenderDiscardsCommands());
    MG_Impl::GLImpl::EndConditionalRender();
    MG_Impl::GLImpl::BeginConditionalRender(ids[1], GL_QUERY_BY_REGION_NO_WAIT_INVERTED);
    EXPECT_FALSE(MG_State::pGLContext->ConditionalRenderDiscardsCommands());
    MG_Impl::GLImpl::EndConditionalRender();
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::DeleteQueries(2, ids);
}

TEST_F(QueryTest, ConditionalRenderRejectsTheErrorsTheSpecNames) {
    ScopedBackendFunctionsOverride backendGuard;
    InstallStubBackendOcclusionQueries();

    GLuint ids[2] = {0, 0};
    MG_Impl::GLImpl::GenQueries(2, ids);
    g_stubResultNs = 1;
    MG_Impl::GLImpl::BeginQuery(GL_ANY_SAMPLES_PASSED, ids[0]);
    MG_Impl::GLImpl::EndQuery(GL_ANY_SAMPLES_PASSED);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // GL 4.6 core 10.9, one rule at a time.
    MG_Impl::GLImpl::BeginConditionalRender(ids[0], GL_TIME_ELAPSED);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_ENUM);
    EXPECT_FALSE(MG_State::pGLContext->IsConditionalRenderActive());

    // A generated NAME is not yet a query object.
    MG_Impl::GLImpl::BeginConditionalRender(ids[1], GL_QUERY_WAIT);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE);
    MG_Impl::GLImpl::BeginConditionalRender(0, GL_QUERY_WAIT);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE);

    // A query that is not an occlusion query cannot drive one.
    GLuint timerId = 0;
    MG_Impl::GLImpl::GenQueries(1, &timerId);
    MG_Impl::GLImpl::BeginQuery(GL_TIME_ELAPSED, timerId);
    MG_Impl::GLImpl::EndQuery(GL_TIME_ELAPSED);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::BeginConditionalRender(timerId, GL_QUERY_WAIT);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // End without a block, and a nested Begin.
    MG_Impl::GLImpl::EndConditionalRender();
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);
    MG_Impl::GLImpl::BeginConditionalRender(ids[0], GL_QUERY_WAIT);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::BeginConditionalRender(ids[0], GL_QUERY_WAIT);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);
    // The rejected nested Begin must not have disturbed the open block.
    EXPECT_EQ(MG_State::pGLContext->GetConditionalRenderQuery(), ids[0]);
    MG_Impl::GLImpl::EndConditionalRender();
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::DeleteQueries(2, ids);
    MG_Impl::GLImpl::DeleteQueries(1, &timerId);
}

TEST_F(QueryTest, DisableTimerQueryFeatureMatchesEnvironment) {
    const char* raw = std::getenv("MOBILEGL_DISABLE_TIMERQUERY");
    Bool expected = false;
    if (raw != nullptr && raw[0] != '\0') {
        String value = raw;
        String lowered = value;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        expected = value != "0" && lowered != "false";
    }
    EXPECT_EQ(MG_Config::Features.DisableTimerQuery, expected);
}

// ---------------------------------------------------------------------------------------------
// GL_ARB_pipeline_statistics_query, core since 4.6. The eleven counter targets had no arm in
// glBeginQuery's accepted-target list, so the very first glBeginQuery(GL_VERTICES_SUBMITTED)
// raised GL_INVALID_ENUM and killed
// pipeline_statistics_query_tests_ARB.api_coverage_invalid_glbeginquery_calls before it could
// check anything. MobileGL instruments none of the counters and says so through the mechanism
// GL 4.6 core 4.2.1 provides for exactly this: GL_QUERY_COUNTER_BITS = 0, which the conformance
// suite reads and treats as "skip the functional half of this target".
// ---------------------------------------------------------------------------------------------

TEST_F(QueryTest, PipelineStatisticsTargetsAreAcceptedAndReportZeroCounterBits) {
    // The NINE unconditional targets. The two tessellation ones are conditional on tessellation
    // support and have their own test below.
    static constexpr GLenum kTargets[] = {
        GL_VERTICES_SUBMITTED,          GL_PRIMITIVES_SUBMITTED,
        GL_VERTEX_SHADER_INVOCATIONS,   GL_GEOMETRY_SHADER_INVOCATIONS,
        GL_GEOMETRY_SHADER_PRIMITIVES_EMITTED, GL_FRAGMENT_SHADER_INVOCATIONS,
        GL_COMPUTE_SHADER_INVOCATIONS,  GL_CLIPPING_INPUT_PRIMITIVES,
        GL_CLIPPING_OUTPUT_PRIMITIVES,
    };

    for (const GLenum target: kTargets) {
        GLuint id = 0;
        MG_Impl::GLImpl::GenQueries(1, &id);
        ASSERT_NE(id, 0u);

        MG_Impl::GLImpl::BeginQuery(target, id);
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR)
            << "glBeginQuery must accept pipeline-statistics target 0x" << std::hex << target;

        GLint current = 0;
        MG_Impl::GLImpl::GetQueryiv(target, GL_CURRENT_QUERY, &current);
        EXPECT_EQ(static_cast<GLuint>(current), id) << "GL_CURRENT_QUERY has to track this target too";

        MG_Impl::GLImpl::EndQuery(target);
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
        EXPECT_EQ(MG_Impl::GLImpl::IsQuery(id), GL_TRUE);

        GLint counterBits = -1;
        MG_Impl::GLImpl::GetQueryiv(target, GL_QUERY_COUNTER_BITS, &counterBits);
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
        EXPECT_EQ(counterBits, 0) << "an uninstrumented counter reports zero bits, per GL 4.6 core 4.2.1";

        // The result is immediately available (nothing was ever submitted to wait on) and reads
        // as the zero the zero counter-bit answer marks indeterminate.
        GLuint available = 0;
        MG_Impl::GLImpl::GetQueryObjectuiv(id, GL_QUERY_RESULT_AVAILABLE, &available);
        EXPECT_EQ(available, static_cast<GLuint>(GL_TRUE));
        GLuint result = 0xDEADBEEFu;
        MG_Impl::GLImpl::GetQueryObjectuiv(id, GL_QUERY_RESULT, &result);
        EXPECT_EQ(result, 0u);
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

        MG_Impl::GLImpl::DeleteQueries(1, &id);
    }
}

// GL_TESS_CONTROL_SHADER_PATCHES / GL_TESS_EVALUATION_SHADER_INVOCATIONS are the two
// pipeline-statistics targets ARB_pipeline_statistics_query makes CONDITIONAL on tessellation
// support, and the extension string is the only thing an application can read to decide whether
// an implementation has it. So the target and the string have to move together: accepting a
// tessellation-conditional token while withholding the string that announces the condition is
// self-contradictory, and the conformance suite catches exactly that contradiction
// (KHR-GL46.pipeline_statistics_query_tests_ARB.api_coverage_unsupported_calls demands
// GL_INVALID_ENUM for every target its own probe calls unsupported, and its probe for these two
// is `compatibility(4,0) || GL_ARB_tessellation_shader` - a CORE context fails the first half).
//
// Written against the advertisement rather than against today's answer on purpose: the day a
// backend starts emitting GL_ARB_tessellation_shader this test keeps passing and keeps pinning
// the coupling, and it fails loudly if only one of the two halves moves.
TEST_F(QueryTest, TessellationPipelineStatisticsTargetsFollowTheTessellationShaderAdvertisement) {
    const auto* extensionsString =
        reinterpret_cast<const char*>(MG_Impl::GLImpl::GetString(GL_EXTENSIONS));
    ASSERT_NE(extensionsString, nullptr);
    const Bool advertised = String(extensionsString).find("GL_ARB_tessellation_shader") != String::npos;
    const GLenum expectedError = advertised ? GL_NO_ERROR : GL_INVALID_ENUM;

    static constexpr GLenum kTessTargets[] = {
        GL_TESS_CONTROL_SHADER_PATCHES,
        GL_TESS_EVALUATION_SHADER_INVOCATIONS,
    };

    for (const GLenum target: kTessTargets) {
        GLuint id = 0;
        MG_Impl::GLImpl::GenQueries(1, &id);
        ASSERT_NE(id, 0u);

        MG_Impl::GLImpl::BeginQuery(target, id);
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), expectedError)
            << "glBeginQuery on tessellation pipeline-statistics target 0x" << std::hex << target
            << " must agree with the GL_ARB_tessellation_shader advertisement";

        if (advertised) {
            MG_Impl::GLImpl::EndQuery(target);
            EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
            GLint counterBits = -1;
            MG_Impl::GLImpl::GetQueryiv(target, GL_QUERY_COUNTER_BITS, &counterBits);
            EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
            EXPECT_EQ(counterBits, 0);
        } else {
            // Refused at glEndQuery too, not just at glBeginQuery: a target the implementation
            // does not have is not half-accepted.
            MG_Impl::GLImpl::EndQuery(target);
            EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_ENUM);
            // GL_QUERY_COUNTER_BITS still answers the honest zero rather than an error - the
            // getter has never validated its target, and zero is what "no such counter" reads as
            // (GL 4.6 core 4.2.1), so the refusal costs no information.
            GLint counterBits = -1;
            MG_Impl::GLImpl::GetQueryiv(target, GL_QUERY_COUNTER_BITS, &counterBits);
            EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
            EXPECT_EQ(counterBits, 0);
            // And GL_CURRENT_QUERY reads as "no query" rather than tracking a slot that was
            // never opened.
            GLint current = -1;
            MG_Impl::GLImpl::GetQueryiv(target, GL_CURRENT_QUERY, &current);
            EXPECT_EQ(current, 0);
        }

        MG_Impl::GLImpl::DeleteQueries(1, &id);
        while (MG_Impl::GLImpl::GetError() != GL_NO_ERROR) {
        }
    }
}

// The negative half the conformance case actually asserts: an object already latched onto one
// pipeline-statistics target must refuse a different one with GL_INVALID_OPERATION. This is what
// per-target active slots buy - a single shared slot would have reported "a query is already
// active on this target" for an unrelated target instead.
TEST_F(QueryTest, PipelineStatisticsQueryObjectRefusesASecondTargetAndTargetsAreIndependent) {
    GLuint id = 0;
    MG_Impl::GLImpl::GenQueries(1, &id);
    ASSERT_NE(id, 0u);

    MG_Impl::GLImpl::BeginQuery(GL_VERTICES_SUBMITTED, id);
    MG_Impl::GLImpl::EndQuery(GL_VERTICES_SUBMITTED);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::BeginQuery(GL_PRIMITIVES_SUBMITTED, id);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // Two different objects on two different targets are simultaneously active, because each
    // target owns its own slot.
    GLuint first = 0;
    GLuint second = 0;
    MG_Impl::GLImpl::GenQueries(1, &first);
    MG_Impl::GLImpl::GenQueries(1, &second);
    MG_Impl::GLImpl::BeginQuery(GL_VERTICES_SUBMITTED, first);
    MG_Impl::GLImpl::BeginQuery(GL_PRIMITIVES_SUBMITTED, second);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLint current = 0;
    MG_Impl::GLImpl::GetQueryiv(GL_VERTICES_SUBMITTED, GL_CURRENT_QUERY, &current);
    EXPECT_EQ(static_cast<GLuint>(current), first);
    MG_Impl::GLImpl::GetQueryiv(GL_PRIMITIVES_SUBMITTED, GL_CURRENT_QUERY, &current);
    EXPECT_EQ(static_cast<GLuint>(current), second);

    // Deleting an ACTIVE query implicitly ends it and releases its slot; the sibling target is
    // untouched.
    MG_Impl::GLImpl::DeleteQueries(1, &first);
    MG_Impl::GLImpl::GetQueryiv(GL_VERTICES_SUBMITTED, GL_CURRENT_QUERY, &current);
    EXPECT_EQ(current, 0);
    MG_Impl::GLImpl::GetQueryiv(GL_PRIMITIVES_SUBMITTED, GL_CURRENT_QUERY, &current);
    EXPECT_EQ(static_cast<GLuint>(current), second);

    MG_Impl::GLImpl::EndQuery(GL_PRIMITIVES_SUBMITTED);
    MG_Impl::GLImpl::DeleteQueries(1, &second);
    MG_Impl::GLImpl::DeleteQueries(1, &id);
    while (MG_Impl::GLImpl::GetError() != GL_NO_ERROR) {
    }
}

// glCreateQueries keeps its own, shorter accepted-target list on purpose: it is unchanged here,
// and this pins that the pipeline-statistics addition did not leak into it.
TEST_F(QueryTest, EndQueryOnAPipelineStatisticsTargetWithNoActiveQueryIsInvalidOperation) {
    MG_Impl::GLImpl::EndQuery(GL_FRAGMENT_SHADER_INVOCATIONS);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);
}
