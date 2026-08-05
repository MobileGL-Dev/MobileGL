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

#include "Includes.h"
#include "Init.h"
#include <Config.h>

#include <MG_Backend/BackendObjects.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/Query/GL_Query.h>

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

// Environment-agnostic property test for the env -> ConfigLoader -> Features
// chain: whatever MOBILEGL_DISABLE_TIMERQUERY is set to in the environment of
// this test process, MG_ConfigLoader::Init must have parsed it with the
// unified truthy rule (set, non-empty, not "0", case-insensitive not "false").
// Running the binary under MOBILEGL_DISABLE_TIMERQUERY=1 therefore exercises
// the real end-to-end path rather than the struct field alone.
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
