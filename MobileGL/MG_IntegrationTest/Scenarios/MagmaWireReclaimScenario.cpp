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
// WHAT THE CASES PIN, all inside ONE frame (no swap between the first and the last
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
//   * ADrawAfterTheEarlyReclaim... (round 2, ID-P7-43) - the reclaim's own hazard: a store that
//     dies mid-frame frees a VkBuffer handle value the descriptor memos still map to a set baked
//     to its memory, and the next mint can get that value back. The pixel of a draw made
//     through the re-minted handle must follow the new store's bytes.
//
// READ FROM THE SERVER, NOT THE PICTURE. The numbers are the wbuf[] gauges the server role's
// VkBufferManager publishes (PipeStats.h, Gauge::WireBuffers..WireDeferredSyncs): RUN MAXIMA,
// because the peak lives inside the frame and a swap-time sample would read after the frame
// boundary's own sweep. They reach this process through the server's `MGPipe stats:` line, so
// the lane pins MOBILEGL_PIPE_STATS=1 and a private log path, and the entries are registered on
// the inproc and spawn arms ONLY: the tcp arm's server is the pre-started fixture supervisor,
// whose environment the lane cannot set and whose log this process cannot read
// (scripts/ci/spawn_lane_parity.py names the exception).

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

        // Case 4: the memo ABA. The store that dies is SMALL and the one minted through its
        // handle afterwards is LARGE, with a pinned 1 MiB store minted right after the small one
        // so its hole stays a hole: a same-size mint would take the just-freed memory range back
        // (VMA's best-fit bucket) and the stale descriptor would read the new bytes by aliasing
        // accident, which is a green that proves nothing. 64 KiB cannot land in a 256-byte hole.
        constexpr std::size_t kAbaDeadStoreBytes = 256;
        constexpr std::size_t kAbaPinStoreBytes = static_cast<std::size_t>(kMiB);
        constexpr std::size_t kAbaNewStoreBytes = 64 * 1024;
        constexpr GLuint64 kAbaWaitNs = 2000000000ull; // 2 s: a 64x16 draw on lavapipe retires in ms
        constexpr int kDescriptorDraws = 2049; // one past UniformManager's wire epoch budget

        constexpr const char* kVertex = R"(#version 330 core
layout(location = 0) in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); })";

        constexpr const char* kFragment = R"(#version 330 core
uniform vec4 uColor;
out vec4 fragColor;
void main() { fragColor = uColor; })";

        // Case 4 paints from a uniform BLOCK, because a block bound through glBindBufferBase is
        // what the wire arm binds directly - the descriptor names the store's own VkBuffer
        // (UniformManager::ResolveWireUniformBufferPayload) - while a plain uniform goes
        // through a transient slice that no store handle can alias.
        constexpr const char* kBlockFragment = R"(#version 330 core
layout(std140) uniform Block { vec4 color; };
out vec4 fragColor;
void main() { fragColor = color; })";

        constexpr const char* kStorageVertex = R"(#version 430 core
layout(location = 0) in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); })";
        constexpr const char* kStorageFragment = R"(#version 430 core
layout(std430, binding = 0) readonly buffer Colors { vec4 color[]; };
out vec4 fragColor;
void main() { fragColor = color[0]; })";

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
            // An absent gauge reads -1, which would pass the one-sided bound below as a leak that
            // never happened; the peak has to be a reading before it can be a small one.
            ASSERT_GE(livePeak, 0) << "the server published no wlivepk= gauge: " << window.line;
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

        // --------------------------------------------------------------------------------------
        // CASE 4 (round 2, ID-P7-43). THE EARLY RECLAIM MUST NOT LEAVE A DESCRIPTOR MEMO NAMING
        // THE DEAD STORE'S HANDLE. UniformManager memoizes descriptor sets by VkBuffer handle
        // (m_descriptorReuseMemo's signature hashes the VkDescriptorBufferInfo words,
        // FastRebindMemo compares uboBuffer) and clears them at the frame boundary - before M2
        // the only point a wire store could die. With M2 a store dies at a park mid-frame, the
        // next mint gets its handle value back (a heap pointer under lavapipe; glibc's tcache
        // hands the last chunk freed to the next same-size malloc), and a draw that resolves the
        // SAME (handle, range) reuses a set baked to the dead store's memory: wrong bytes, no
        // Fatal. The fix folds VkBufferManager's wire-store destroy epoch into both memos.
        //
        // THE SHAPE, all in one frame, every step deterministic (no sleep, no fence poll race):
        //   1. D1 draws with UBO store S1 (256 B, colour A): the memo now maps S1's handle to
        //      D1's descriptor set.
        //   2. glBufferData(U, 0) orphans S1 - parked, tagged for the batch D1 is recorded in -
        //      and mints nothing in its place, so the handle value stays free once S1 dies.
        //   3. glClientWaitSync(fence, FLUSH_COMMANDS, 0) SUBMITS that batch without waiting:
        //      the review's "flush without wait".
        //   4. glBufferSubData on the vertex store D1 named records a staged copy, so the
        //      recording is pending again and the wait in 5 CANNOT take the frame-boundary
        //      drain that would clear the memos (TryDrainFrameTransients refuses with work
        //      pending).
        //   5. glClientWaitSync(fence, 0, 2 s): D1's batch has retired; the memos are intact.
        //   6. glBufferData(V, 0) parks the vertex store, and THAT park's sweep finds S1's
        //      batch complete and destroys S1: its handle value is free, its memory a 256-byte
        //      hole bounded by the pin store minted right after it.
        //   7. glBufferData(U, 64 KiB, colour B) mints through the freed handle value; 64 KiB
        //      cannot land in the hole, so the dead store's bytes stay where D1's set points.
        //   8. D2 draws with U: it resolves (S1's handle value, 16 bytes) - a memo hit before
        //      the fix, and D1's set reads A off the dead store.
        // The pixel is the whole assertion: it has to be B, the bytes the live store holds.
        TEST_F(MagmaWireReclaimScenario, ADrawAfterTheEarlyReclaimFollowsTheNewStoreNotTheMemoizedHandle) {
            if (!Ready() || IsSkipped()) return;
            std::string error;
            const GLuint program = CompileProgram(kVertex, kBlockFragment, &error);
            ASSERT_NE(program, 0u) << error;
            const GLuint block = glGetUniformBlockIndex(program, "Block");
            ASSERT_NE(block, GLuint(GL_INVALID_INDEX));
            glUniformBlockBinding(program, block, 0);

            const PipeStatsWindow::LogMark mark = SetupFrame();
            ClearTo(0.0f, 0.0f, 0.0f, 0.0f);

            const Rgba8 colourA{200, 60, 30, 255};
            const Rgba8 colourB{60, 200, 90, 255};
            const auto blockBytes = [](std::size_t size, const Rgba8& c) {
                // The block's vec4 at offset 0; every other slot holds a colour no step draws, so a
                // read off any other offset is as visible as a read off the dead store.
                std::vector<std::uint8_t> bytes(size, 0);
                float* f = reinterpret_cast<float*>(bytes.data());
                for (std::size_t i = 0; i + 4 <= size / sizeof(float); i += 4) {
                    f[i] = 1.0f, f[i + 1] = 0.0f, f[i + 2] = 1.0f, f[i + 3] = 1.0f;
                }
                f[0] = c.r / 255.0f, f[1] = c.g / 255.0f, f[2] = c.b / 255.0f, f[3] = 1.0f;
                return bytes;
            };

            // Step 1's stores, in this order: S1, then the pin right after it, then the vertex
            // quad D1 names (m_buffer, a store this frame's recording will name and step 6 parks).
            GLuint ubo = 0, pin = 0;
            glGenBuffers(1, &ubo);
            glGenBuffers(1, &pin);
            glBindBuffer(GL_UNIFORM_BUFFER, ubo);
            const std::vector<std::uint8_t> bytesA = blockBytes(kAbaDeadStoreBytes, colourA);
            glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(bytesA.size()), bytesA.data(), GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
            glBindBuffer(GL_COPY_WRITE_BUFFER, pin);
            glBufferData(GL_COPY_WRITE_BUFFER, GLsizeiptr(kAbaPinStoreBytes), nullptr, GL_STATIC_DRAW);
            std::vector<std::uint8_t> quad(kUndrawnStoreBytes, 0);
            glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
            Respecify(quad, 0, kWidth);
            glUseProgram(program);
            glDrawArrays(GL_TRIANGLES, 0, 6);

            // Step 2: S1 orphaned, nothing minted.
            glBindBuffer(GL_UNIFORM_BUFFER, ubo);
            glBufferData(GL_UNIFORM_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
            // Step 3: the batch D1 is in, submitted and not waited for.
            GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            ASSERT_NE(fence, nullptr);
            const GLenum flushed = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
            RecordProperty("flush_wait_result", static_cast<int>(flushed));
            // Step 4: a recorded copy into the busy vertex store keeps the recording pending.
            glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
            glBufferSubData(GL_ARRAY_BUFFER, 0, 4, quad.data());
            // Step 5: D1's batch retires; the memos survive because step 4 is still recorded.
            const GLenum waited = glClientWaitSync(fence, 0, kAbaWaitNs);
            ASSERT_TRUE(waited == GL_ALREADY_SIGNALED || waited == GL_CONDITION_SATISFIED)
                << "glClientWaitSync returned " << waited << ": the batch that drew from the first "
                   "store did not retire within " << (kAbaWaitNs / 1000000) << " ms";
            // Step 6: the vertex store's park sweeps S1 out - the destroy the memos must notice.
            glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STREAM_DRAW);
            // Step 7: a new store through the freed handle value, holding B.
            glBindBuffer(GL_UNIFORM_BUFFER, ubo);
            const std::vector<std::uint8_t> bytesB = blockBytes(kAbaNewStoreBytes, colourB);
            glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(bytesB.size()), bytesB.data(), GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
            glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
            Respecify(quad, 0, kWidth);
            // Step 8.
            glUseProgram(program);
            glDrawArrays(GL_TRIANGLES, 0, 6);

            const Image image = ReadPixels(kWidth, kHeight);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            const Rgba8 got = image.At(kWidth / 2, kHeight / 2);
            EXPECT_TRUE(Near(got, colourB))
                << "read " << got << ", wanted " << colourB << " (the live store's bytes). " << colourA
                << " is the DEAD store's colour: the draw through the re-minted VkBuffer handle reused a "
                   "descriptor set memoized under the first store's handle and read its freed memory "
                   "(ID-P7-43); anything else is that memory already reused";

            glDeleteSync(fence);
            glDeleteBuffers(1, &pin);
            glDeleteBuffers(1, &ubo);
            glDeleteProgram(program);
            const PipeStatsWindow::Window window = CloseFrameAndReadServerWindow(mark);
            if (window.found) RecordProperty("wire_stores_peak",
                                             static_cast<int>(PipeStatsWindow::CounterOrAbsent(window, "wlivepk")));
        }

        // M3: a fixed SSBO store and one live layout, but 2049 distinct aligned
        // windows force 2049 descriptor writes before a present. M2's buffer-store
        // ceiling cannot help: no store is orphaned. The first 2048 sets must be
        // retired by submit index before their per-layout cursor rewinds, and the
        // last draw must read the last window rather than a set still in flight.
        TEST_F(MagmaWireReclaimScenario, DescriptorSetsRewindInsideOneLongFrameAfterTheirSubmitRetires) {
            if (!Ready() || IsSkipped()) return;
            std::string error;
            const GLuint program = CompileProgram(kStorageVertex, kStorageFragment, &error);
            ASSERT_NE(program, 0u) << error;
            const PipeStatsWindow::LogMark mark = SetupFrame();
            ClearTo(0.0f, 0.0f, 0.0f, 0.0f);
            std::vector<std::uint8_t> quad(kUndrawnStoreBytes, 0);
            glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
            Respecify(quad, 0, kWidth);

            GLint alignment = 0;
            glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &alignment);
            ASSERT_GT(alignment, 0);
            const std::size_t stride = static_cast<std::size_t>(std::max(alignment, 16));
            std::vector<std::uint8_t> bytes(stride * kDescriptorDraws, 0);
            const Rgba8 last{60, 200, 90, 255};
            for (int i = 0; i < kDescriptorDraws; ++i) {
                const float color[4] = {i + 1 == kDescriptorDraws ? last.r / 255.0f : 200.0f / 255.0f,
                                        i + 1 == kDescriptorDraws ? last.g / 255.0f : 60.0f / 255.0f,
                                        i + 1 == kDescriptorDraws ? last.b / 255.0f : 30.0f / 255.0f, 1.0f};
                std::memcpy(bytes.data() + stride * i, color, sizeof(color));
            }
            GLuint storage = 0;
            glGenBuffers(1, &storage);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, storage);
            glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(bytes.size()), bytes.data(), GL_STATIC_DRAW);
            glUseProgram(program);
            for (int i = 0; i < kDescriptorDraws; ++i) {
                glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, storage,
                                  static_cast<GLintptr>(stride * i), 16);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
            const Image image = ReadPixels(kWidth, kHeight);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            EXPECT_TRUE(Near(image.At(kWidth / 2, kHeight / 2), last))
                << "the last SSBO window was not read after descriptor recycling";
            (void)CloseFrameAndReadServerWindow(mark);
            const std::string serverLog = PipeStatsWindow::ReadServerLogSince(mark);
            const std::string marker = "Magma descriptor rewind:";
            const std::size_t rewind = serverLog.find(marker);
            ASSERT_NE(rewind, std::string::npos) << "2049 distinct descriptor writes in one frame did not rewind";
            const std::size_t cachedAt = serverLog.find(" cached=", rewind);
            ASSERT_NE(cachedAt, std::string::npos) << serverLog.substr(rewind, 160);
            const unsigned long long cached = std::strtoull(serverLog.c_str() + cachedAt + 8, nullptr, 10);
            EXPECT_LE(cached, 2080ull) << "the fixed-layout cache grew beyond the 2048-set epoch: "
                                       << serverLog.substr(rewind, 160);
            RecordProperty("descriptor_cached_at_rewind", static_cast<int>(cached));
            glDeleteBuffers(1, &storage);
            glDeleteProgram(program);
        }

    } // namespace
} // namespace MGITest
