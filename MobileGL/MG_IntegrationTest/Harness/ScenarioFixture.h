// MobileGL - MobileGL/MG_IntegrationTest/Harness/ScenarioFixture.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The base fixture every scenario derives from. Its only jobs are to bring the
// headless context up once per process and to decide what "this machine has no
// usable GPU" means.
//
// By default it means a clean GTEST_SKIP() - never a failure, never a hang -
// because a developer box or a container without a GPU should not fail a run it
// was never able to perform. But a skip is indistinguishable from a pass in
// every CI summary, so the `integration-gpu` label on its own is unfalsifiable:
// a runner whose driver pinning silently broke reports the same green as one
// that rendered every frame. MOBILEGL_ITEST_REQUIRE_GPU is the caller saying
// "this machine HAS a GPU and I am relying on these scenarios actually running";
// with it set, an unusable harness is a FAILURE carrying the pre-flight's reason.

#pragma once

#include <gtest/gtest.h>

#include "HeadlessGL.h"

namespace MGITest {

    class ScenarioTest : public ::testing::Test {
    protected:
        void SetUp() override {
            m_ready = false;
            HeadlessGL& gl = HeadlessGL::Get();
            if (!gl.Usable()) {
                if (RequireGpu()) {
                    // FAIL() is a FATAL failure but does NOT mark the test skipped,
                    // so a derived SetUp that guards on IsSkipped() alone would run
                    // straight into GL calls with no current context and SIGSEGV -
                    // that exact crash shipped from the first version of this guard.
                    // Derived fixtures must gate on Ready() (below), which is false
                    // on BOTH the skip path and this failure path.
                    FAIL() << "MOBILEGL_ITEST_REQUIRE_GPU is set, so an unusable harness is a failure, not a skip. "
                           << "Backend " << gl.BackendName() << " could not be brought up: " << gl.SkipReason();
                }
                GTEST_SKIP() << "no usable GPU/display/ICD for backend " << gl.BackendName() << ": " << gl.SkipReason();
            }
            if (RequireGpu() && LooksLikeSoftwareRasterizer(gl.RendererString())) {
                // "Ran on llvmpipe" must not be able to pass as "ran on the GPU":
                // a misconfigured vendor pin silently lands on the software
                // rasterizer, and REQUIRE_GPU exists precisely to make that loud.
                FAIL() << "MOBILEGL_ITEST_REQUIRE_GPU is set but the context landed on a software rasterizer: "
                       << gl.RendererString();
            }
            // A scenario starts from a clean slate but shares the context (and so
            // the renderer's memos) with every other scenario in this process -
            // which is exactly the situation both shipped bugs needed.
            RecordProperty("backend", gl.BackendName());
            RecordProperty("renderer", gl.RendererString());
            m_ready = true;
        }

        // The ONLY gate a derived SetUp/TearDown may use: `if (!Ready()) return;`.
        // True only when the base SetUp brought the context up and neither skipped
        // nor failed. IsSkipped() alone is WRONG here (see the comment at FAIL()).
        bool Ready() const { return m_ready; }

        static HeadlessGL& Gl() { return HeadlessGL::Get(); }

    private:
        static bool LooksLikeSoftwareRasterizer(const std::string& renderer) {
            static const char* kNames[] = {"llvmpipe", "lavapipe", "softpipe", "SwiftShader", "swrast"};
            for (const char* name : kNames) {
                if (renderer.find(name) != std::string::npos) {
                    return true;
                }
            }
            return false;
        }

        bool m_ready = false;
    };

} // namespace MGITest
