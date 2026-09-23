// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/MagmaWireReclaimScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// P7 wave 4 package M2 (ID-P7-27 / ID-P7-32): THE MAGMA WIRE ARM MUST RECLAIM ORPHANED BUFFER
// STORES INSIDE A FRAME.
//
// THE DEFECT. Every glBufferData that crosses the wire reaches VkBufferManager::
// RespecifyWireBuffer, which orphans the old VkBuffer (legitimately - a recorded draw may still
// name it) and mints a new one. Before this package the orphan went into a per-frame-slot bucket
// that only a FRAME BOUNDARY emptied, and minecraft-1.21.4-fabric-iris-bsl-esc-menu-854 has two
// eglSwapBuffers in 1,303,535 calls - the server sees one present per replay. On lavapipe the
// split server held 25,923 dead stores against 28 live wire buffers and 41k mappings; on the
// Redmi it died in scudo at 1.2 GiB while the monolith passed at 807 MiB.
//
// WHAT THE THREE CASES PIN, all inside ONE frame (no swap between the first and the last
// respecify), because that is the only shape in which the bug existed:
//   * RespecifiesWithNoDrawBetween... - the stores no GPU command ever named. They are dead the
//     moment they are orphaned, so the live store count must stay a small multiple of the wire
//     buffers alive (k = 4 here; the mechanism gives 1).
//   * RespecifyAndDrawEachStore... - every orphan was drawn from, so it is named by recorded,
//     unsubmitted work and nothing retires it inside the frame except the
//     MOBILEGL_IPC_WIRE_DEFERRED_MB watermark's forced sync point. The parked bytes must stay
//     within the budget plus one store, and every draw must still land on its own strip - a
//     store destroyed while its draw was pending shows up as a wrong strip (or a dead lavapipe).
//   * ManySmallRespecifyAndDrawRounds... - the same with stores too small to reach any byte
//     budget; the fixed 1024-store ceiling on the same sync point is what bounds them.
//
// READ FROM THE SERVER, NOT THE PICTURE. The numbers are the wbuf[] gauges the server role's
// VkBufferManager publishes (PipeStats.h, Gauge::WireBuffers..WireDeferredSyncs): RUN MAXIMA,
// because the peak lives inside the frame and a swap-time sample would read after the frame
// boundary's own sweep. They reach this process through the server's `MGPipe stats:` line, so
// the lane pins MOBILEGL_PIPE_STATS=1 and a private log path, and the entries are registered on
// the inproc and spawn arms ONLY: the tcp arm's server is the pre-started fixture supervisor,
// whose environment the lane cannot set and whose log this process cannot read
// (scripts/ci/spawn_lane_parity.py names the exception).

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/PipeStatsWindow.h"
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitRuntimePeek.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {

        // Set by the MagmaWireReclaim ctest entries and by nothing else; a harness marker, never
        // read by the library. The whole-binary informational lanes run this file without it and
        // without the stats channel, and skip.
        constexpr const char* kLaneMarker = "MGITEST_MAGMA_RECLAIM_LANE";

        constexpr int kWidth = 64;
        constexpr int kHeight = 16;
        constexpr std::uint64_t kMiB = 1024u * 1024u;

        // Case 1: many small respecifies of ONE buffer and no draw between them.
        constexpr int kUndrawnRespecifies = 1024;
        constexpr std::size_t kUndrawnStoreBytes = 4096;
        constexpr long long kLivePerWireBuffer = 4;

        // Case 2: respecify-and-draw, one strip per store. 48 stores of 1 MiB is six times the
        // 8 MiB budget the lane sets, so the watermark has to fire several times in the frame.
        constexpr int kDrawnRespecifies = 48;
        constexpr std::size_t kDrawnStoreBytes = static_cast<std::size_t>(kMiB);

        // Case 3: many SMALL respecify-and-draw rounds - 3000 x 256 B is 750 KB, nowhere near
        // the byte budget, so only the store-count ceiling (VkBufferManager::
        // kWireDeferredCountCeiling, 1024) can bound them inside the frame.
        constexpr int kSmallDrawnRespecifies = 3000;
        constexpr std::size_t kSmallDrawnStoreBytes = 256;
        constexpr long long kDeferredCountCeiling = 1024;

        constexpr const char* kVertex = R"(#version 330 core
layout(location = 0) in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); })";

        constexpr const char* kFragment = R"(#version 330 core
uniform vec4 uColor;
out vec4 fragColor;
void main() { fragColor = uColor; })";

        // Two triangles covering pixel columns [x0, x1) over the whole height, in NDC.
        void WriteStripQuad(float* out, int x0, int x1) {
            const float l = -1.0f + 2.0f * static_cast<float>(x0) / kWidth;
            const float r = -1.0f + 2.0f * static_cast<float>(x1) / kWidth;
            const float quad[12] = {l, -1.0f, r, -1.0f, r, 1.0f, l, -1.0f, r, 1.0f, l, 1.0f};
            for (int i = 0; i < 12; ++i) out[i] = quad[i];
        }

        // A colour per strip that no neighbour shares and the clear (all zero) is not.
        Rgba8 StripColor(int strip) {
            Rgba8 c;
            c.r = static_cast<std::uint8_t>(40 + (strip * 37) % 200);
            c.g = static_cast<std::uint8_t>(40 + (strip * 91) % 200);
            c.b = static_cast<std::uint8_t>(255 - strip);
            c.a = 255;
            return c;
        }

        bool Near(const Rgba8& a, const Rgba8& b) {
            return std::abs(int(a.r) - int(b.r)) <= 1 && std::abs(int(a.g) - int(b.g)) <= 1 &&
                   std::abs(int(a.b) - int(b.b)) <= 1 && std::abs(int(a.a) - int(b.a)) <= 1;
        }

        std::uint64_t DeferredBudgetBytes() {
            const char* value = std::getenv("MOBILEGL_IPC_WIRE_DEFERRED_MB");
            const std::uint64_t mb = (value != nullptr && *value != '\0') ? std::strtoull(value, nullptr, 10) : 64u;
            return mb * kMiB;
        }

        class MagmaWireReclaimScenario : public ScenarioTest {
        protected:
            GLuint m_program = 0, m_vao = 0, m_buffer = 0;
            GLint m_colorLocation = -1;
            ColorFbo m_target{};

            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                if (Gl().BackendName() != "DirectVulkan") {
                    GTEST_SKIP() << "the reclaim under test is the Magma wire arm's VkBufferManager";
                }
                if (std::getenv(kLaneMarker) == nullptr) {
                    GTEST_SKIP() << kLaneMarker << " is not set: this run has no private stats "
                                    "channel to read the server's wbuf[] gauges from";
                }
                const SplitRuntimeState runtime = PeekSplitRuntime();
                if (!runtime.transportResolved || !runtime.sessionActive) {
                    GTEST_SKIP() << "not a split run (" << runtime.transportName
                                 << "): the monolith arm has no wire buffer stores";
                }
                std::string error;
                m_program = CompileProgram(kVertex, kFragment, &error);
                ASSERT_NE(m_program, 0u) << error;
                m_colorLocation = glGetUniformLocation(m_program, "uColor");
                ASSERT_GE(m_colorLocation, 0);
                m_target = MakeColorFbo(kWidth, kHeight);
                ASSERT_NE(m_target.fbo, 0u);
                BindFbo(m_target);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_BLEND);
                ClearTo(0.0f, 0.0f, 0.0f, 0.0f);
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_buffer);
                glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
            }

            void TearDown() override {
                if (Ready()) {
                    if (m_buffer) glDeleteBuffers(1, &m_buffer);
                    if (m_vao) glDeleteVertexArrays(1, &m_vao);
                    if (m_program) glDeleteProgram(m_program);
                    if (m_target.fbo) DestroyColorFbo(m_target);
                }
                ScenarioTest::TearDown();
            }

            // glBufferData of `bytes` (a strip quad at offset 0, zeros after) and the attribute
            // pointer re-armed on the new store.
            void Respecify(std::vector<std::uint8_t>& bytes, int x0, int x1) {
                WriteStripQuad(reinterpret_cast<float*>(bytes.data()), x0, x1);
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes.size()), bytes.data(), GL_STREAM_DRAW);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
                glEnableVertexAttribArray(0);
            }

            void DrawStrip(int strip) {
                const Rgba8 c = StripColor(strip);
                glUseProgram(m_program);
                glUniform4f(m_colorLocation, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }

            // The swap that closes the counted frame, and the server's line for it.
            PipeStatsWindow::Window CloseFrameAndReadServerWindow(const PipeStatsWindow::LogMark& mark) {
                Gl().EndFrame();
                if (const SplitRuntimeState runtime = PeekSplitRuntime(); runtime.transportResolved) {
                    EXPECT_TRUE(WaitForSplitAppliedForTesting(runtime.emitSeq));
                }
                PipeStatsWindow::Window window = PipeStatsWindow::LastFromServerLogSince(mark);
                if (window.found) RecordProperty("stats_line_server", window.line.c_str());
                return window;
            }

            // A setup frame that draws once from the buffer, so the first orphan of the counted
            // frame is a store the GPU really named, then the mark the counted window starts at.
            PipeStatsWindow::LogMark SetupFrame() {
                std::vector<std::uint8_t> bytes(kUndrawnStoreBytes, 0);
                Respecify(bytes, 0, kWidth);
                DrawStrip(0);
                Gl().EndFrame();
                BindFbo(m_target);
                if (const SplitRuntimeState runtime = PeekSplitRuntime(); runtime.transportResolved) {
                    EXPECT_TRUE(WaitForSplitAppliedForTesting(runtime.emitSeq));
                }
                return PipeStatsWindow::MarkLaneLog();
            }
        };

        // --------------------------------------------------------------------------------------
        // CASE 1. 1024 glBufferData of one 4 KiB buffer and no draw between them: 1023 of the
        // orphans were never named by a GPU command and are dead the moment they are orphaned.
        // Before M2 every one of them waited for the frame boundary, so the peak was ~1025 live
        // VkBuffers for one wire buffer.
        TEST_F(MagmaWireReclaimScenario, RespecifiesWithNoDrawBetweenKeepTheLiveStoreCountBounded) {
            if (!Ready() || IsSkipped()) return;
            const PipeStatsWindow::LogMark mark = SetupFrame();

            std::vector<std::uint8_t> bytes(kUndrawnStoreBytes, 0);
            for (int i = 0; i < kUndrawnRespecifies; ++i) {
                // Only the LAST store's quad covers the target; the rest are never drawn.
                const bool last = (i + 1 == kUndrawnRespecifies);
                Respecify(bytes, 0, last ? kWidth : 1);
            }
            ClearTo(0.0f, 0.0f, 0.0f, 0.0f);
            DrawStrip(7);
            const Image image = ReadPixels(kWidth, kHeight);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            EXPECT_TRUE(Near(image.At(kWidth / 2, kHeight / 2), StripColor(7)))
                << "read " << image.At(kWidth / 2, kHeight / 2) << ", wanted " << StripColor(7)
                << ": the draw from the last of " << kUndrawnRespecifies << " respecified stores did "
                   "not land - the store it names was reclaimed, or never reached";

            const PipeStatsWindow::Window window = CloseFrameAndReadServerWindow(mark);
            ASSERT_TRUE(window.found) << "no 'MGPipe stats:' window in the server log "
                                      << PipeStatsWindow::ServerLibraryLogPath();
            const long long wireBuffers = PipeStatsWindow::CounterOrAbsent(window, "wbufs");
            const long long livePeak = PipeStatsWindow::CounterOrAbsent(window, "wlivepk");
            ASSERT_GT(wireBuffers, 0) << "the server published no wbufs= gauge: " << window.line;
            ASSERT_GE(livePeak, 0) << window.line;
            RecordProperty("wire_buffers", static_cast<int>(wireBuffers));
            RecordProperty("wire_stores_peak", static_cast<int>(livePeak));
            EXPECT_LE(livePeak, kLivePerWireBuffer * wireBuffers)
                << "the Magma server held " << livePeak << " VkBuffers at once for " << wireBuffers
                << " wire buffer record(s) inside one frame of " << kUndrawnRespecifies
                << " glBufferData calls. Orphans no GPU command named are dead at the park; a count "
                   "that tracks the respecifies is the bsl-esc-menu-854 leak (ID-P7-32). " << window.line;
        }

        // --------------------------------------------------------------------------------------
        // CASE 2. 48 respecify-and-draw rounds of a 1 MiB store, one strip each, in one frame.
        // Every orphan is named by recorded, unsubmitted work; only the watermark's forced sync
        // point can retire it before the frame ends. Before M2: 48 MiB parked against the lane's
        // 8 MiB budget. The strips are the soundness half: a store destroyed while its draw was
        // still pending does not leave its own colour behind.
        TEST_F(MagmaWireReclaimScenario, RespecifyAndDrawEachStoreInOneFrameStaysWithinTheDeferredBudget) {
            if (!Ready() || IsSkipped()) return;
            const PipeStatsWindow::LogMark mark = SetupFrame();

            ClearTo(0.0f, 0.0f, 0.0f, 0.0f);
            std::vector<std::uint8_t> bytes(kDrawnStoreBytes, 0);
            for (int strip = 0; strip < kDrawnRespecifies; ++strip) {
                Respecify(bytes, strip, strip + 1);
                DrawStrip(strip);
            }
            const Image image = ReadPixels(kWidth, kHeight);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            int wrongStrips = 0;
            for (int strip = 0; strip < kDrawnRespecifies; ++strip) {
                const Rgba8 got = image.At(strip, kHeight / 2);
                if (!Near(got, StripColor(strip))) {
                    ++wrongStrips;
                    ADD_FAILURE() << "strip " << strip << " reads " << got << ", wanted " << StripColor(strip)
                                  << ": the draw from that strip's own store did not land";
                }
                if (wrongStrips > 4) break;
            }

            const PipeStatsWindow::Window window = CloseFrameAndReadServerWindow(mark);
            ASSERT_TRUE(window.found) << "no 'MGPipe stats:' window in the server log "
                                      << PipeStatsWindow::ServerLibraryLogPath();
            const long long wireBuffers = PipeStatsWindow::CounterOrAbsent(window, "wbufs");
            const long long livePeak = PipeStatsWindow::CounterOrAbsent(window, "wlivepk");
            const long long deferredPeak = PipeStatsWindow::CounterOrAbsent(window, "wdefpk");
            const long long syncs = PipeStatsWindow::CounterOrAbsent(window, "wdefsync");
            ASSERT_GT(wireBuffers, 0) << "the server published no wbufs= gauge: " << window.line;
            ASSERT_GE(deferredPeak, 0) << window.line;
            RecordProperty("wire_buffers", static_cast<int>(wireBuffers));
            RecordProperty("wire_stores_peak", static_cast<int>(livePeak));
            RecordProperty("wire_deferred_bytes_peak", std::to_string(deferredPeak).c_str());
            RecordProperty("wire_deferred_syncs", static_cast<int>(syncs));

            // MOBILEGL_IPC_WIRE_DEFERRED_MB=0 is the knob's negative control (no forced sync). Such
            // a run is held to the lane's own 8 MiB, so that what goes red is the server's
            // numbers and not a precondition of this case.
            const std::uint64_t configured = DeferredBudgetBytes();
            const std::uint64_t budget = configured != 0 ? configured : 8u * kMiB;
            // One store past the budget is the most a park can add before the watermark answers it.
            const long long deferredBound = static_cast<long long>(budget + kDrawnStoreBytes);
            EXPECT_LE(deferredPeak, deferredBound)
                << "the Magma server parked " << deferredPeak << " bytes of orphaned stores inside one "
                   "frame against a MOBILEGL_IPC_WIRE_DEFERRED_MB budget of " << budget
                << " bytes (+ one " << kDrawnStoreBytes << "-byte store). " << window.line;
            const long long liveBound =
                wireBuffers + static_cast<long long>(budget / kDrawnStoreBytes) + 2;
            EXPECT_LE(livePeak, liveBound)
                << "the Magma server held " << livePeak << " VkBuffers at once; the records ("
                << wireBuffers << ") plus a budget's worth of parked stores allow " << liveBound
                << ". " << window.line;
        }

        // --------------------------------------------------------------------------------------
        // CASE 3. 3000 respecify-and-draw rounds of a 256-byte store in one frame. The byte
        // budget never trips (750 KB against 8 MiB), which is the bsl-esc-menu-854 shape the
        // count ceiling exists for: one stretch there parked 12,498 stores in 39.5 MB. Before M2
        // every one of the 3000 was alive at the end of the loop.
        TEST_F(MagmaWireReclaimScenario, ManySmallRespecifyAndDrawRoundsStayUnderTheStoreCountCeiling) {
            if (!Ready() || IsSkipped()) return;
            const PipeStatsWindow::LogMark mark = SetupFrame();

            ClearTo(0.0f, 0.0f, 0.0f, 0.0f);
            std::vector<std::uint8_t> bytes(kSmallDrawnStoreBytes, 0);
            for (int i = 0; i < kSmallDrawnRespecifies; ++i) {
                Respecify(bytes, 0, kWidth);
                DrawStrip(i % 200);
            }
            const Image image = ReadPixels(kWidth, kHeight);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            const Rgba8 want = StripColor((kSmallDrawnRespecifies - 1) % 200);
            EXPECT_TRUE(Near(image.At(kWidth / 2, kHeight / 2), want))
                << "read " << image.At(kWidth / 2, kHeight / 2) << ", wanted " << want
                << ": the last of " << kSmallDrawnRespecifies << " draws did not land on top";

            const PipeStatsWindow::Window window = CloseFrameAndReadServerWindow(mark);
            ASSERT_TRUE(window.found) << "no 'MGPipe stats:' window in the server log "
                                      << PipeStatsWindow::ServerLibraryLogPath();
            const long long wireBuffers = PipeStatsWindow::CounterOrAbsent(window, "wbufs");
            const long long livePeak = PipeStatsWindow::CounterOrAbsent(window, "wlivepk");
            const long long syncs = PipeStatsWindow::CounterOrAbsent(window, "wdefsync");
            ASSERT_GT(wireBuffers, 0) << "the server published no wbufs= gauge: " << window.line;
            RecordProperty("wire_buffers", static_cast<int>(wireBuffers));
            RecordProperty("wire_stores_peak", static_cast<int>(livePeak));
            RecordProperty("wire_deferred_syncs", static_cast<int>(syncs));
            // The ceiling is checked after the park that crosses it, so one store past it plus the
            // records is the most the arm can hold.
            const long long liveBound = wireBuffers + kDeferredCountCeiling + 2;
            EXPECT_LE(livePeak, liveBound)
                << "the Magma server held " << livePeak << " VkBuffers at once after "
                << kSmallDrawnRespecifies << " small respecify-and-draw rounds in one frame; the "
                   "records plus the " << kDeferredCountCeiling << "-store ceiling allow " << liveBound
                << ". A count that tracks the rounds is the bsl-esc-menu-854 shape. " << window.line;
        }

    } // namespace
} // namespace MGITest
