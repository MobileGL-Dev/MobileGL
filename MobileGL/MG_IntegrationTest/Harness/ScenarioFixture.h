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

#include <cctype>
#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

#include "HeadlessGL.h"
#include "SplitLane.h"

namespace MGITest {

    // How a MOBILEGL_* quirk variable reads in THIS process's environment.
    //
    // A scenario that needs a non-default configuration takes it from here and skips
    // when the process it was launched into is not in that configuration, rather than
    // writing MG_Config::Features itself. Two reasons, and the second one decides it:
    //
    //   - the feature table is an internal symbol. On Android this module links against
    //     the SHIPPING libMobileGL.so - deliberately, so the on-device run validates the
    //     real artifact - and that library is built -fvisibility=hidden, so nothing
    //     internal is reachable from here at all.
    //   - a quirk poked in-process is already too late for everything latched at
    //     initialization: the compile pool and its threads, and the backend's advertised
    //     extension list, which is built once from the configuration in force at first
    //     use. The process-wide variable is the only spelling that covers the whole
    //     configuration instead of the half of it that is still mutable afterwards.
    //
    // The reading rule is MG_ConfigLoader's, character for character (ConfigLoader.cpp,
    // QueryEnvQuirkOverride / IsTruthyValue): unset is Auto - device auto-detection or a
    // built-in default, i.e. a value only the implementation knows - a truthy value is
    // On, and anything else that IS set ("0", "false", "") is Off.
    enum class AmbientQuirk { Auto, On, Off };

    inline AmbientQuirk AmbientQuirkFromEnvironment(const char* name) {
        const char* value = std::getenv(name);
        if (value == nullptr) return AmbientQuirk::Auto;
        std::string lowered(value);
        for (char& c : lowered) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lowered.empty() || lowered == "0" || lowered == "false") return AmbientQuirk::Off;
        return AmbientQuirk::On;
    }

    class ScenarioTest : public ::testing::Test {
    protected:
        // THE BEHAVIOURAL HALF OF THE SPLIT LANE'S CLAIM, and it is in the DESTRUCTOR rather than
        // in TearDown() on purpose: gtest calls only the MOST DERIVED TearDown, and every scenario
        // that overrides it would have to remember to chain here. The fixture destructor always
        // runs, and it runs before the test result is finalized, so ADD_FAILURE() is recorded.
        //
        // What it asserts: a case that ran in an ARMED split lane must have moved the client
        // encoder's record ordinal. Everything else in the lane - the pixels, the readbacks, the
        // arm assertion - is equally true of a monolith run of the same workload; this is the one
        // statement that is only true if records crossed the ring. An emit table that resolves the
        // transport and then falls through to the driver passes every other assertion in the file
        // and fails exactly here.
        ~ScenarioTest() override {
            if (!m_splitAssertionsArmed) return;
            if (IsSkipped() || HasFailure()) return;
            const SplitRuntimeState after = PeekSplitRuntime();
            if (after.emitSeq <= m_emitSeqAtSetUp) {
                ADD_FAILURE() << "this case ran in an armed DirectGLES.Split. lane and the client "
                                 "encoder's record ordinal did not move: EmitSeq was "
                              << m_emitSeqAtSetUp << " at SetUp and is " << after.emitSeq
                              << " now. The workload drew, cleared and read pixels, so records were "
                                 "due - a sequence that did not advance means the emit table "
                                 "resolved the transport and then did not put anything on the wire, "
                                 "which every other assertion in this lane is blind to because a "
                                 "monolith run of the same workload produces the same pixels.";
            }
        }

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
            if (RequireHardwareGpu() && LooksLikeSoftwareRasterizer(gl.RendererString())) {
                // Only when hardware was asked for BY NAME. REQUIRE_GPU means "an
                // unusable harness is a failure, not a silent skip" - it is the
                // falsifiability switch, and CI is exactly where it belongs. But CI
                // runners have no GPU, so folding "must not be llvmpipe" into the
                // same switch made the CI lane unpassable by construction: the
                // scenarios pin backend draw logic, which a software rasterizer
                // executes just as faithfully. Landing on llvmpipe/lavapipe there is
                // the intended configuration, not a misconfiguration. A vendor pin
                // that must not silently degrade sets REQUIRE_HARDWARE_GPU.
                FAIL() << "MOBILEGL_ITEST_REQUIRE_HARDWARE_GPU is set but the context landed on a software "
                       << "rasterizer: " << gl.RendererString();
            }
            // P5's DirectGLES.Split. lanes, in ONE place rather than in each scenario they point
            // at - the Split family also points at ClearThenReadPixelsScenario, which is target A
            // of the reduced path and predates P5, and any later Split lane gets the same
            // guarantee without anyone having to remember it.
            //
            // THE ARMING QUESTION IS ASKED OF THE PROCESS, not of the source tree. Until a real
            // client session exists, MOBILEGL_TRANSPORT=inproc is parsed and then nothing consumes
            // it, so every case in the lane would go GREEN against the monolith path under a name
            // that says it tested the split one. The first version of this guard answered the
            // question with a CMake grep over c0's stub files, and review finding M-1 falsified it
            // by renaming one string: eleven lanes armed and eight went green. Harness/
            // SplitRuntimePeek.h now answers it from MG_Config::Transport, ClientSession::Active()
            // and ImplementedVerbCount(), none of which a message edit can move. Registrations are
            // never deleted (gate G14); they skip, naming exactly which fact is not true.
            if (SplitLane::IsSplitLane()) {
                if (const std::string splitSkip = SplitLane::SkipReasonForSplitOnlyAssertions();
                    !splitSkip.empty()) {
                    GTEST_SKIP() << splitSkip;
                }
                // Armed. Take the wire's baseline, so the destructor can require that this case
                // actually PUT SOMETHING THROUGH IT (review finding N-5: ten of the eleven Split
                // entries had no runtime evidence of anything, and their green meant only "the
                // same GL workload passed").
                const SplitRuntimeState state = PeekSplitRuntime();
                m_splitAssertionsArmed = true;
                m_emitSeqAtSetUp = state.emitSeq;
                RecordProperty("split_transport", state.transportName);
                RecordProperty("split_implemented_verbs", static_cast<int>(state.implementedVerbs));
                RecordProperty("split_emit_seq_at_setup", static_cast<int>(state.emitSeq));
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
        bool m_splitAssertionsArmed = false;
        unsigned long long m_emitSeqAtSetUp = 0;
    };

} // namespace MGITest
