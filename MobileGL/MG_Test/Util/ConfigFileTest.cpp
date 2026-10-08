// MobileGL - MobileGL/MG_Test/Util/ConfigFileTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The system configuration files (ConfigLoader.cpp): environment > backend file > client.conf >
// built-in default, for MG_ConfigLoader::Init() and for the LookupSetting readers that run before
// it. No GL context, no driver: the loader is parsing.

#include <gtest/gtest.h>

#include <Config.h>
#include <Init.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include <unistd.h>

namespace {
    using MobileGL::BackendType;
    using MobileGL::String;
    namespace Config = MobileGL::MG_Config;
    namespace Loader = MobileGL::MG_ConfigLoader;

    // Every variable a case touches, restored after it.
    constexpr const char* kTouched[] = {
        "MOBILEGL_CONFIG_FILE",       "MOBILEGL_BACKEND_FILE", "MOBILEGL_BACKEND_TYPE",
        "MOBILEGL_MAGMA_FRAMESINFLIGHT", "MOBILEGL_TRANSPORT",   "MOBILEGL_IPC_CONTROL",
        "MOBILEGL_IPC_DATA",          "MOBILEGL_IPC_ROLE",     "MOBILEGL_TEST_ONLY_KEY",
        "MOBILEGL_FRAMES_IN_FLIGHT",  "MOBILEGL_IPC_PRESENT_CREDIT",
    };

    class ConfigFileTest : public ::testing::Test {
    protected:
        void SetUp() override {
            for (const char* name : kTouched) {
                const char* value = std::getenv(name);
                saved_.emplace_back(name, value != nullptr, value != nullptr ? value : "");
                ::unsetenv(name);
            }
            dir_ = std::filesystem::temp_directory_path() /
                   ("mgl-config-test-" + std::to_string(::getpid()) + "-" +
                    ::testing::UnitTest::GetInstance()->current_test_info()->name());
            std::filesystem::create_directories(dir_);
            conf_ = (dir_ / "client.conf").string();
            backend_ = (dir_ / "backend").string();
            // Point both at this case's files, which do not exist yet: the host's own /etc is
            // never read by a case.
            ::setenv("MOBILEGL_CONFIG_FILE", conf_.c_str(), 1);
            ::setenv("MOBILEGL_BACKEND_FILE", backend_.c_str(), 1);
        }
        void TearDown() override {
            for (const auto& [name, had, value] : saved_) {
                if (had) ::setenv(name.c_str(), value.c_str(), 1);
                else ::unsetenv(name.c_str());
            }
            std::error_code ec;
            std::filesystem::remove_all(dir_, ec);
            Loader::Init(); // leave the process's configuration as its environment says
        }
        static void Write(const std::string& path, const std::string& text) {
            std::ofstream out(path, std::ios::binary);
            out << text;
        }
        static String Lookup(const char* key) {
            String value;
            return Loader::LookupSetting(key, value) ? value : String("<unset>");
        }

        std::vector<std::tuple<std::string, bool, std::string>> saved_;
        std::filesystem::path dir_;
        std::string conf_;
        std::string backend_;
    };

    TEST_F(ConfigFileTest, WithoutFilesOrEnvironmentTheBuiltInDefaultsHold) {
        Loader::Init();
        EXPECT_EQ(Config::ActiveBackendType, BackendType::DirectGLES);
        EXPECT_EQ(Config::Features.FramesInFlight, 3u);
        EXPECT_EQ(Lookup("MOBILEGL_TRANSPORT"), "<unset>");
#if MOBILEGL_BUILD_DISAGGREGATED
        EXPECT_EQ(Config::Transport, Config::TransportMode::Monolith);
        EXPECT_EQ(Config::Ipc.Control, "fork");
#endif
    }

    // P15: one frames-in-flight knob for both backends, the Magma spelling as its alias, and the
    // split arms' present credit derived from it (max(1, N - 2), so the default keeps credit 1).
    TEST_F(ConfigFileTest, FramesInFlightIsBackendNeutralAndDerivesThePresentCredit) {
        Loader::Init();
        EXPECT_EQ(Config::Features.FramesInFlight, 3u);
#if MOBILEGL_BUILD_DISAGGREGATED
        EXPECT_EQ(Config::Ipc.PresentCredit, 1u);
#endif
        ::setenv("MOBILEGL_FRAMES_IN_FLIGHT", "4", 1);
        ::setenv("MOBILEGL_MAGMA_FRAMESINFLIGHT", "6", 1); // the new name wins over the alias
        Loader::Init();
        EXPECT_EQ(Config::Features.FramesInFlight, 4u);
#if MOBILEGL_BUILD_DISAGGREGATED
        EXPECT_EQ(Config::Ipc.PresentCredit, 2u);
        ::setenv("MOBILEGL_IPC_PRESENT_CREDIT", "1", 1); // the explicit override stays
        Loader::Init();
        EXPECT_EQ(Config::Ipc.PresentCredit, 1u);
        ::unsetenv("MOBILEGL_IPC_PRESENT_CREDIT");
#endif
        ::unsetenv("MOBILEGL_FRAMES_IN_FLIGHT");
        Loader::Init();
        EXPECT_EQ(Config::Features.FramesInFlight, 6u); // the alias alone still works
    }

    TEST_F(ConfigFileTest, TheFileFillsWhatTheEnvironmentLeavesUnset) {
        Write(conf_, "# a comment\n"
                     "\n"
                     "  MOBILEGL_MAGMA_FRAMESINFLIGHT = 5  \n"
                     "MOBILEGL_TRANSPORT=spawn\r\n"
                     "MOBILEGL_IPC_CONTROL=\"unix:@test-endpoint\"\n"
                     "MOBILEGL_IPC_DATA='shm'\n"
                     "NOT_OURS=1\n"
                     "garbage line without equals\n"
                     "MOBILEGL_TEST_ONLY_KEY=a=b\n");
        Loader::Init();
        EXPECT_EQ(Config::Features.FramesInFlight, 5u);
        EXPECT_EQ(Lookup("MOBILEGL_TRANSPORT"), "spawn");
        EXPECT_EQ(Lookup("MOBILEGL_IPC_CONTROL"), "unix:@test-endpoint");
        EXPECT_EQ(Lookup("MOBILEGL_IPC_DATA"), "shm");
        EXPECT_EQ(Lookup("MOBILEGL_TEST_ONLY_KEY"), "a=b"); // only the first '=' splits
        EXPECT_EQ(Lookup("NOT_OURS"), "<unset>");
#if MOBILEGL_BUILD_DISAGGREGATED
        EXPECT_EQ(Config::Transport, Config::TransportMode::Spawn);
        EXPECT_EQ(Config::Ipc.Control, "unix:@test-endpoint");
        EXPECT_EQ(Config::Ipc.Data, "shm");
#endif
    }

    TEST_F(ConfigFileTest, TheEnvironmentWinsOverTheFile) {
        Write(conf_, "MOBILEGL_MAGMA_FRAMESINFLIGHT=5\nMOBILEGL_IPC_CONTROL=unix:@from-file\n");
        ::setenv("MOBILEGL_MAGMA_FRAMESINFLIGHT", "7", 1);
        ::setenv("MOBILEGL_IPC_CONTROL", "unix:@from-env", 1);
        Loader::Init();
        EXPECT_EQ(Config::Features.FramesInFlight, 7u);
        EXPECT_EQ(Lookup("MOBILEGL_IPC_CONTROL"), "unix:@from-env");
#if MOBILEGL_BUILD_DISAGGREGATED
        EXPECT_EQ(Config::Ipc.Control, "unix:@from-env");
#endif
        // Set but empty is still the environment speaking.
        ::setenv("MOBILEGL_IPC_CONTROL", "", 1);
        EXPECT_EQ(Lookup("MOBILEGL_IPC_CONTROL"), "");
    }

    TEST_F(ConfigFileTest, TheBackendFileNamesTheBackendUnderTheEnvironmentAndOverClientConf) {
        Write(backend_, "  DirectVulkan \n");
        Write(conf_, "MOBILEGL_BACKEND_TYPE=DirectGLES\n");
        Loader::Init();
        EXPECT_EQ(Config::ActiveBackendType, BackendType::DirectVulkan);
        EXPECT_EQ(Lookup("MOBILEGL_BACKEND_TYPE"), "DirectVulkan");

        ::setenv("MOBILEGL_BACKEND_TYPE", "DirectGLES", 1);
        Loader::Init();
        EXPECT_EQ(Config::ActiveBackendType, BackendType::DirectGLES);
        ::unsetenv("MOBILEGL_BACKEND_TYPE");

        // Without the backend file, client.conf's line is the answer.
        std::filesystem::remove(backend_);
        Write(conf_, "MOBILEGL_BACKEND_TYPE=DirectVulkan\n");
        Loader::Init();
        EXPECT_EQ(Config::ActiveBackendType, BackendType::DirectVulkan);
    }

    TEST_F(ConfigFileTest, AnEmptyPathTurnsAFileOffAndAMissingFileIsNoError) {
        Write(conf_, "MOBILEGL_MAGMA_FRAMESINFLIGHT=5\n");
        Write(backend_, "DirectVulkan\n");
        ::setenv("MOBILEGL_CONFIG_FILE", "", 1);
        ::setenv("MOBILEGL_BACKEND_FILE", "", 1);
        Loader::Init();
        EXPECT_EQ(Config::Features.FramesInFlight, 3u);
        EXPECT_EQ(Config::ActiveBackendType, BackendType::DirectGLES);

        ::setenv("MOBILEGL_CONFIG_FILE", (dir_ / "absent.conf").string().c_str(), 1);
        Loader::Init();
        EXPECT_EQ(Config::Features.FramesInFlight, 3u);
    }

    TEST_F(ConfigFileTest, AServerProcessNeverReadsTheClientsFile) {
        Write(conf_, "MOBILEGL_MAGMA_FRAMESINFLIGHT=5\n");
        Write(backend_, "DirectVulkan\n");
        ::setenv("MOBILEGL_IPC_ROLE", "server", 1);
        Loader::Init();
        EXPECT_EQ(Config::Features.FramesInFlight, 3u);
        EXPECT_EQ(Config::ActiveBackendType, BackendType::DirectGLES);
        EXPECT_EQ(Lookup("MOBILEGL_MAGMA_FRAMESINFLIGHT"), "<unset>");
    }
} // namespace
