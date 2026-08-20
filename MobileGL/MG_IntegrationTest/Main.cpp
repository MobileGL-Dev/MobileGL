// MobileGL - MobileGL/MG_IntegrationTest/Main.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Entry point for the headless GPU integration scenarios.
//
// The banner lives in a gtest Environment rather than in main() on purpose:
// Environment::SetUp does not run for `--gtest_list_tests`, which is what CMake
// invokes at build time to discover the cases. Discovery therefore never brings
// up EGL, never needs a GPU and cannot hang.

#include <gtest/gtest.h>
#include <cstdio>

#include "Harness/HeadlessGL.h"

namespace {

    class HarnessBanner : public ::testing::Environment {
    public:
        void SetUp() override {
            const MGITest::HeadlessGL& gl = MGITest::HeadlessGL::Get();
            std::fprintf(stderr, "MobileGL integration scenarios: backend=%s\n", gl.BackendName().c_str());
            if (gl.Usable()) {
                // EGL_PLATFORM is echoed because it is the invariant this harness
                // rests on: the run is headless on every machine, so a run that
                // silently bound to a workstation's window system is a different
                // run from CI's and must be visible as one in the log.
                const char* eglPlatform = std::getenv("EGL_PLATFORM");
#if defined(__ANDROID__)
                constexpr const char* surfaceKind = "AImageReader window";
#else
                constexpr const char* surfaceKind = "pbuffer";
#endif
                std::fprintf(stderr, "  renderer: %s\n  surface:  %dx%d %s (headless, EGL_PLATFORM=%s)\n",
                             gl.RendererString().c_str(), gl.Width(), gl.Height(),
                             surfaceKind,
                             eglPlatform != nullptr ? eglPlatform : "<unset>");
            } else if (MGITest::RequireGpu()) {
                std::fprintf(stderr,
                             "  FAILING every scenario (MOBILEGL_ITEST_REQUIRE_GPU is set): %s\n",
                             gl.SkipReason().c_str());
            } else {
                std::fprintf(stderr,
                             "  SKIPPING every scenario: %s\n"
                             "  (set MOBILEGL_ITEST_REQUIRE_GPU=1 to make this a failure instead - a run that\n"
                             "   skipped everything is otherwise indistinguishable from one that passed)\n",
                             gl.SkipReason().c_str());
            }
        }

        void TearDown() override { MGITest::HeadlessGL::Get().ShutDown(); }
    };

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new HarnessBanner());
    return RUN_ALL_TESTS();
}
