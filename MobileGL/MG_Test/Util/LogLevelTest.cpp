// MobileGL - MobileGL/MG_Test/Util/LogLevelTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Guards the log-severity ordering and the MOBILEGL_ASSERT gate that hangs off it.
//
// Until 2026-08-13 the numeric order was DEBUG < WARN < ERROR < INFO < FATAL, so the
// production gate `#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_X` compiled
// MGLOG_W and MGLOG_E out of every INFO build. Failures logged with MGLOG_E were
// invisible in exactly the builds that shipped. Nothing in the suite noticed, which is
// why this file exists.
//
// This test is written to be meaningful in BOTH configurations - build it at
// MOBILEGL_LOG_LEVEL_INFO and at MOBILEGL_LOG_LEVEL_DEBUG and it checks the contract
// appropriate to each. It runs headless: no GL context, no device, just the file sink.

#include <gtest/gtest.h>

#include <Defines.h>
#include <MG_Util/Debug/Log.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

    // ---------------------------------------------------------------------------
    // Compile-time contract
    // ---------------------------------------------------------------------------

    // The ordering itself. A renumbering that re-inverts the scale fails here.
    static_assert(MOBILEGL_LOG_LEVEL_DEBUG < MOBILEGL_LOG_LEVEL_INFO, "DEBUG must be below INFO");
    static_assert(MOBILEGL_LOG_LEVEL_INFO < MOBILEGL_LOG_LEVEL_WARN, "INFO must be below WARN");
    static_assert(MOBILEGL_LOG_LEVEL_WARN < MOBILEGL_LOG_LEVEL_ERROR, "WARN must be below ERROR");
    static_assert(MOBILEGL_LOG_LEVEL_ERROR < MOBILEGL_LOG_LEVEL_FATAL, "ERROR must be below FATAL");

    // DEBUG must stay the floor: the MOBILEGL_ASSERT gate in Defines.h is spelled
    // `ACTIVE <= MOBILEGL_LOG_LEVEL_DEBUG` and means "only in a DEBUG build". That
    // reading is only correct while DEBUG is the minimum.
    static_assert(MOBILEGL_LOG_LEVEL_DEBUG == 0, "DEBUG must be the lowest level");

    // Defines.h and Log.h each define the five constants. Log.h's copy wins when both
    // are included; if the two ever drift, the duplicate-definition warning is not
    // guaranteed to be an error, so pin the values a second time from this TU's view.
    static_assert(MOBILEGL_LOG_LEVEL_INFO == 1, "INFO must be 1 in both Defines.h and Log.h");
    static_assert(MOBILEGL_LOG_LEVEL_WARN == 2, "WARN must be 2 in both Defines.h and Log.h");
    static_assert(MOBILEGL_LOG_LEVEL_ERROR == 3, "ERROR must be 3 in both Defines.h and Log.h");
    static_assert(MOBILEGL_LOG_LEVEL_FATAL == 4, "FATAL must be 4 in both Defines.h and Log.h");

    // Whether this translation unit was compiled with asserts live. This is a literal
    // copy of the Defines.h gate - the point of the test is to prove it agrees with
    // MGLOG_D's liveness, observed at runtime below.
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
    constexpr bool kAssertsLive = true;
#else
    constexpr bool kAssertsLive = false;
#endif

    // Whether the build is the production INFO configuration.
    constexpr bool kBuiltAtInfo = (MOBILEGL_LOG_ACTIVE_LEVEL == MOBILEGL_LOG_LEVEL_INFO);

    // ---------------------------------------------------------------------------
    // Runtime observation of the sink
    // ---------------------------------------------------------------------------

    // The log file path is latched by MG_Util::Debug::InitFile() on the first write and
    // never reopened, so the whole process gets one file. main() below points
    // MOBILEGL_LOG_FILE_PATH at a temp file before gtest runs; this fixture emits one
    // line per level and then reads the file back.
    std::string g_logPath;

    struct Emitted {
        bool debug = false;
        bool info = false;
        bool warn = false;
        bool error = false;
        bool fatal = false;
    };

    Emitted EmitAndRead() {
        // Distinctive markers so a substring search cannot collide with unrelated output.
        MGLOG_D("MGLOGTEST_MARKER_DEBUG_5f3a");
        MGLOG_I("MGLOGTEST_MARKER_INFO_5f3a");
        MGLOG_W("MGLOGTEST_MARKER_WARN_5f3a");
        MGLOG_E("MGLOGTEST_MARKER_ERROR_5f3a");
        MGLOG_F("MGLOGTEST_MARKER_FATAL_5f3a");

        std::ifstream in(g_logPath, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();

        Emitted e;
        e.debug = text.find("MGLOGTEST_MARKER_DEBUG_5f3a") != std::string::npos;
        e.info = text.find("MGLOGTEST_MARKER_INFO_5f3a") != std::string::npos;
        e.warn = text.find("MGLOGTEST_MARKER_WARN_5f3a") != std::string::npos;
        e.error = text.find("MGLOGTEST_MARKER_ERROR_5f3a") != std::string::npos;
        e.fatal = text.find("MGLOGTEST_MARKER_FATAL_5f3a") != std::string::npos;
        return e;
    }

    TEST(LogLevel, SinkIsReachableAtAll) {
        // Guards the test itself: if the file sink were disabled or the path override
        // ignored, every "level X is suppressed" assertion below would pass vacuously.
        ASSERT_FALSE(g_logPath.empty()) << "test harness did not set MOBILEGL_LOG_FILE_PATH";
        const Emitted e = EmitAndRead();
        EXPECT_TRUE(e.fatal) << "FATAL is compiled in at every level; an empty log means the "
                                "file sink never opened and this suite proves nothing";
    }

    TEST(LogLevel, ProductionBuildKeepsErrorAndWarn) {
        if constexpr (!kBuiltAtInfo) {
            GTEST_SKIP() << "only meaningful when built at MOBILEGL_LOG_LEVEL_INFO";
        } else {
            const Emitted e = EmitAndRead();
            // The regression this file exists for.
            EXPECT_TRUE(e.error) << "MGLOG_E must be live in an INFO build";
            EXPECT_TRUE(e.warn) << "MGLOG_W must be live in an INFO build";
            EXPECT_TRUE(e.info) << "MGLOG_I must be live in an INFO build";
            EXPECT_TRUE(e.fatal) << "MGLOG_F must be live in an INFO build";
            // ...and the other half: D must still be compiled out, or production pays
            // for every dev-only diagnostic in the tree.
            EXPECT_FALSE(e.debug) << "MGLOG_D must be compiled out of an INFO build";
        }
    }

    TEST(LogLevel, DebugBuildKeepsEverything) {
        if constexpr (MOBILEGL_LOG_ACTIVE_LEVEL != MOBILEGL_LOG_LEVEL_DEBUG) {
            GTEST_SKIP() << "only meaningful when built at MOBILEGL_LOG_LEVEL_DEBUG";
        } else {
            const Emitted e = EmitAndRead();
            EXPECT_TRUE(e.debug);
            EXPECT_TRUE(e.info);
            EXPECT_TRUE(e.warn);
            EXPECT_TRUE(e.error);
            EXPECT_TRUE(e.fatal);
        }
    }

    // ---------------------------------------------------------------------------
    // The assert contract
    // ---------------------------------------------------------------------------

    TEST(LogLevel, AssertGateTracksDebugLiveness) {
        // The contract: "INFO builds: asserts OFF; DEBUG builds: asserts ON". Stated
        // without naming a level, that is exactly "asserts are live iff MGLOG_D is
        // live" - which is checkable in whichever configuration this was built in,
        // and is what makes the renumbering safe.
        const Emitted e = EmitAndRead();
        EXPECT_EQ(kAssertsLive, e.debug)
            << "MOBILEGL_ASSERT liveness (" << kAssertsLive << ") disagrees with MGLOG_D liveness ("
            << e.debug << "). The Defines.h assert gate and the Log.h MGLOG_D gate have drifted.";
    }

    TEST(LogLevel, AssertIsCompiledOutOfProductionBuilds) {
        if constexpr (kAssertsLive) {
            GTEST_SKIP() << "asserts are live in this configuration; see AssertIsLiveInDebugBuilds";
        } else {
            // If MOBILEGL_ASSERT were live here this would TRAP and take the process
            // down, which is the behavioural half of the contract.
            MOBILEGL_ASSERT(false, "this assert must be compiled out at %s", "INFO");
            SUCCEED();
        }
    }

    TEST(LogLevel, AssertIsLiveInDebugBuilds) {
        if constexpr (!kAssertsLive) {
            GTEST_SKIP() << "asserts are compiled out in this configuration";
        } else {
            // A satisfied assert must be a no-op rather than a trap; that it expands to
            // real code at all is what kAssertsLive already established.
            MOBILEGL_ASSERT(true, "a satisfied assert must not trap");
            SUCCEED();
        }
    }

} // namespace

int main(int argc, char** argv) {
    // Must happen before anything logs: MG_Util::Debug::InitFile() reads the variable
    // once, on the first write, and caches the FILE*.
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "mobilegl-loglevel-test.log";
    std::error_code ec;
    fs::remove(path, ec);
    g_logPath = path.string();

#if defined(_WIN32)
    _putenv_s("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str());
#else
    setenv("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str(), 1);
#endif

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
