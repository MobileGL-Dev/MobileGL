// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/TextureUploadShapeScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE TEXTURE UPLOAD SHAPE, GATED (BRIEF-P4A.md D-D4; P3b/P4b R-11, wave 2-D).
//
// WHAT IT IS FOR. SSIM is completely blind to the difference between "one union box" and "N
// separate rects", and that difference is the Mali upload cliff: Mali prices an upload by the
// number of JOBS, and ~100 one-rect sprite jobs against one union box measured +6 ms/frame
// (ARCHITECTURE.md:249-251). Every gate in the phase can be green while the emission shape has
// silently inverted, so the shape needs a number - and there are TWO numbers, deliberately:
//
//   tex[emit= box= rect= jobs=]   the SERVER's count, Espryt's own, which has existed since P2
//   emit[ctu=]                    the CLIENT's count of the same records (CallClass::
//                                 ClientTextureUploadEmissions)
//
// The two agreeing is the whole reason both are printed (D-L). An emission-shape divergence
// between the client that decides the rect model and the server that pays the GPU cost is then a
// difference of two published numbers rather than something only a device can see.
//
// ---------------------------------------------------------------------------------------------
// WHY IT IS A GATE NOW, AND WHAT CHANGED SINCE P4a. The P4a header said, correctly for its tree:
// "the per-storage-owner emission cursor with view/owner index remapping - which is what actually
// decides the shape for a texture uploaded through a VIEW - is the next phase's", and gating a
// shape the phase had not finished deciding would have pinned a non-answer. THAT WORK LANDED:
// the cursor is keyed on the storage owner (TextureEmit.h's drain list), the remap is
// TextureObjectView::ToOwner*, and TextureViewAliasScenario is its behavioural gate. So the
// numbers are no longer provisional and the RecordProperty block became EXPECT_EQ against the
// table below.
//
// THE CLIENT EMITTER LANDED TOO, and three places in the tree said otherwise until this commit:
// this header, the CMake probe's comment, and notes/p4a/p4a-results/gates-v1.md's re-run table.
// MG_Impl/Pipe/TextureEmit.h's EmitOneLevel increments CallClass::ClientTextureUploadEmissions on
// every piece it emits, so `ctu=` is a live reading and `ctu == emit` is a live assertion. The
// CMake probe's `else()` arm is kept - it is what makes the arming decision falsifiable - but it
// is unreachable on this tree.
//
// THE MALI FRAME-TIME DELTA IS NOT PUBLISHED BESIDE THESE NUMBERS AND WILL NOT BE IN THIS PHASE.
// D-D4 asks for the +6 ms/frame reading next to the shape gold. It is NOT SCHEDULABLE: the phase
// has one device, a Redmi with an Adreno, and no Mali part at all. Recorded as a DECLARED
// DEVIATION here and in docs/Disaggregated/MEASUREMENTS.md ("Adreno-only; Mali delta deferred")
// rather than faked from a model or copied from the 2026 measurement's conditions. What the gate
// therefore pins is the SHAPE, on the reasoning that the shape is the cliff's cause and the frame
// time is its consequence on one vendor's hardware.
// ---------------------------------------------------------------------------------------------
//
// WHAT IT ASSERTS:
//
//   1. the numbers could be READ AT ALL - the counters exist, the window covers the workload, and
//      the workload really uploaded (a run that uploaded nothing would read four zeroes and look
//      exactly like a healthy run whose emitter had been switched off);
//   2. the internal ARITHMETIC of the server's own bracket holds: emit == box + rect, and
//      jobs >= emit, because a box emission is one job and a rect-list emission is N;
//   3. the client and the server agree on the RECORD COUNT (ctu == emit);
//   4. THE GOLD TABLE: emit / box / rect / jobs are exactly kGold's, which is the assertion the
//      other three cannot make - they are all satisfied by a shape that inverted wholesale.
//
// THE WORKLOAD is the shape the decision is about, in three textures:
//   * MANY SMALL SCATTERED SUB-REGIONS per frame (the sprite-atlas / chunk-renderer shape). This
//     is where the box-versus-rect choice is made - MipmapStorage's 96-rect cascade and the
//     summedArea*4 >= unionArea*3 union-box fallback;
//   * one large CONTIGUOUS region, the control that must always be one box whatever the policy is;
//   * the same scattered pattern written THROUGH A glTextureView of the third texture's level 1,
//     so the view/owner index remap is inside the counted window. Without it the gold table would
//     pin a shape for exactly the input the P3b/P4b work changed and say nothing about the input
//     it changed it for.
//
// DIRECTGLES ONLY. The server-side counters are Espryt's (Managers.cpp:6360-6395); Magma's upload
// path is P7 and contributes nothing to them, so a DirectVulkan lane would record a bracket of
// zeroes and call it a shape.
//
// VIEWS ARE OPT-IN ON ESPRYT, so every lane that runs this case sets
// MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1. A lane without it SKIPS rather than dropping the third
// texture: a conditional workload behind a fixed gold table is a gate that passes by shrinking.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/PipeStatsWindow.h"
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitRuntimePeek.h"
#include "../Harness/TextureEmitPeek.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {

        // Set by the TextureUploadShape. ctest entries and by nothing else; a harness marker,
        // never read by the library.
        constexpr const char* kLaneMarker = "MGITEST_TEXTURE_UPLOAD_SHAPE_LANE";

        constexpr int kInset = 2;
        constexpr int kAtlasSize = 64;
        // Enough scattered rects that the box-versus-rect policy has a real decision to make: the
        // rect cascade caps at MipmapStorage::kMaxDirtyRects = 96, and the union-box fallback fires
        // on summedArea*4 >= unionArea*3, so a handful of rects would take neither branch
        // interestingly.
        constexpr int kScatteredRects = 40;
        constexpr int kRectSize = 2;
        constexpr int kFrames = 3;
        // THE SCATTER IS INSET BY ONE RECT, AND WITHOUT IT THIS GATE MEASURED NOTHING.
        //
        // The original stride was x = ((i*7) % (kAtlasSize/kRectSize)) * kRectSize and the same
        // with 5 for y. Both are surjective onto 0..31 over 40 iterations (rect 9 lands at x=62,
        // rect 19 at y=62), so the rects' UNION BOX is the whole 64x64 level - and the server's
        // subRectEligible (Managers.cpp:8486-8491) requires !dirtyRegion.CoversWholeLevel, a
        // BOUNDING-BOX test (MipmapStorage.h:30-33). Both scatter textures were therefore
        // rect-INELIGIBLE on every arm, before any policy about rect lists was consulted: the
        // box-versus-rect decision this scenario exists to pin was never reached, and the only
        // texture that could move was the contiguous control band.
        //
        // Insetting by one rect keeps the same 40 writes and the same coarse stride but bounds
        // the union box at (kRectSize, kRectSize)..(62, 54), which covers no level. The decision
        // point is now live for the two scatter textures, which is what makes the gold row below
        // a statement about the emission shape rather than about the workload's bounding box.
        constexpr int kScatterMargin = kRectSize;
        constexpr int kScatterCells = kAtlasSize / kRectSize - 2 * (kScatterMargin / kRectSize);
        static_assert(kScatterMargin + (kScatterCells - 1) * kRectSize + kRectSize < kAtlasSize,
                      "the inset scatter must not reach the level's far edge, or its union box "
                      "covers the level again and the server stops looking at the rect list");
        // The viewed texture's OWNER is twice the atlas so that its level 1 - which is what the
        // view opens onto - is exactly kAtlasSize and the same scattered pattern applies to it.
        constexpr int kViewedOwnerSize = kAtlasSize * 2;
        constexpr int kViewedOwnerLevels = 2;
        constexpr int kViewMinLevel = 1;

        // ---- THE GOLD TABLE -------------------------------------------------------------------
        //
        // MEASURED ON THIS CURSOR, not inherited. The P4a record for the two-texture workload was
        // `emit=6 box=6 rect=0 jobs=6` with `ctu=0`; this row is the three-texture workload's own
        // reading taken after the client emitter and the owner-keyed cursor landed, and the P4a
        // numbers are quoted only so that a reader can see they are not the same claim.
        //
        // KEYED ON THE WORKLOAD CONSTANTS, and the static_assert below is the key: change
        // kScatteredRects, kFrames, the number of contiguous bands or the number of textures and
        // these numbers describe a workload that no longer exists. The assert makes that a compile
        // error rather than a mysterious red.
        //
        // WHY box IS THE WHOLE OF emit ON THIS TREE, and this paragraph was WRONG in its first
        // version - the correction is worth keeping because the wrong version is the one a reader
        // reaches for.
        //
        // The client really does build a rect list here: MipmapStorage's cascade merges only
        // rects that overlap or abut (RegionsTouch, MipmapStorage.cpp:88-91), the inset scatter
        // leaves 2-texel gaps, and GetDirtyRects hands back THIRTY rects whose summed area (120
        // texels) is far under 3/4 of the union box - so neither the 2..maxRects test nor the
        // summedArea*4 >= unionArea*3 fallback takes the box arm. The record carries all thirty.
        //
        // THE SERVER DECIDES, and it decides BOX, at Managers.cpp:8649:
        //
        //     if (BufferImpl::UnpackRingAvailable()) dirtyRectCount = 0;
        //
        // "through the unpack ring every glTexSubImage is a GPU copy job (Mali), so ~100 sprite
        // rects become ~100 jobs whose fixed cost dwarfs the union box's extra bytes - measured
        // +6 ms/frame... One box, one job." That is the existing Mali-cliff mitigation, and
        // box=9 rect=0 is its INTENDED OUTPUT rather than an accident of this workload. Pinning
        // it is the point: a change that starts emitting rect lists through the ring is exactly
        // the +6 ms/frame inversion, and disabling the ring is what the red-once does.
        //
        // WHAT THE FIRST VERSION GOT WRONG, because the trap is easy to fall into twice: the
        // original scatter stride reached x=62 and y=62 in a 64-px atlas, so the union box
        // covered the whole level - and the server's subRectEligible (Managers.cpp:8486-8491)
        // requires !dirtyRegion.CoversWholeLevel, a BOUNDING-BOX test (MipmapStorage.h:30-33).
        // Both scatter textures were rect-INELIGIBLE before the ring policy was ever consulted,
        // so the gold looked identical while measuring something else entirely, and the only
        // texture that could move under any red-once was the contiguous control band. The
        // kScatterMargin inset above is what makes this paragraph true.
        struct UploadShapeGold {
            long long Emissions;
            long long BoxEmissions;
            long long RectEmissions;
            long long Jobs;
        };
        constexpr int kTexturesInTheWindow = 3;
        constexpr UploadShapeGold kGold{9, 9, 0, 9};
        static_assert(kScatteredRects == 40 && kFrames == 3 && kTexturesInTheWindow == 3,
                      "the gold table is keyed on the workload constants: re-measure before "
                      "changing kScatteredRects, kFrames or the texture count");
        static_assert(kGold.Emissions == kGold.BoxEmissions + kGold.RectEmissions,
                      "the gold row must itself satisfy the arithmetic the case asserts");
        static_assert(kGold.Emissions == kTexturesInTheWindow * kFrames,
                      "one emission per texture per frame is the shape; a gold row that does not "
                      "say so is pinning an accident");

        constexpr const char* kVS = R"(#version 330 core
in vec2 aPos;
out vec2 vUv;
void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kFS = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
out vec4 oColor;
void main() { oColor = texture(uTex, vUv); }
)";

        struct Vertex {
            float x, y;
        };

        bool BuildMarkerIsSet(const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr && value[0] == '1' && value[1] == '\0';
        }

        class TextureUploadShapeScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                std::string error;
                m_program = CompileProgram(kVS, kFS, &error);
                ASSERT_NE(m_program, 0u) << error;

                static const Vertex quad[6] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f},
                                               {-1.0f, -1.0f}, {1.0f, 1.0f},  {-1.0f, 1.0f}};
                glGenBuffers(1, &m_quadBuffer);
                glBindBuffer(GL_ARRAY_BUFFER, m_quadBuffer);
                glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
                glBindVertexArray(0);
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindVertexArray(0);
                glBindTexture(GL_TEXTURE_2D, 0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                if (m_scattered != 0) glDeleteTextures(1, &m_scattered);
                if (m_contiguous != 0) glDeleteTextures(1, &m_contiguous);
                if (m_view != 0) glDeleteTextures(1, &m_view);
                if (m_viewedOwner != 0) glDeleteTextures(1, &m_viewedOwner);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_quadBuffer != 0) glDeleteBuffers(1, &m_quadBuffer);
                if (m_program != 0) glDeleteProgram(m_program);
            }

            void SkipUnlessTheLaneIsAssertableHere() {
                if (std::getenv(kLaneMarker) == nullptr) {
                    GTEST_SKIP() << "runs only in its own lane: the TextureUploadShape. ctest entry "
                                    "sets " << kLaneMarker
                                 << " together with MOBILEGL_PIPE_STATS=1, "
                                    "MOBILEGL_PIPE_STATS_PERIOD=1 and a private "
                                    "MOBILEGL_LOG_FILE_PATH. None of that is configured in the "
                                    "ambient entries, and the ambient log is shared, so a read here "
                                    "would race.";
                    return;
                }
                if (Gl().BackendName() != "DirectGLES") {
                    GTEST_SKIP() << "DirectGLES only: the upload-shape counters are Espryt's "
                                    "(Managers.cpp:6360-6395) and " << Gl().BackendName()
                                 << " contributes nothing to them, so this lane would record a "
                                    "bracket of zeroes and call it a shape.";
                    return;
                }
                if (!BuildMarkerIsSet("MGITEST_PIPE_PUSH_BUILD")) {
                    GTEST_SKIP() << "this library was built without MOBILEGL_PIPE_PUSH: the client's "
                                    "half of the comparison (CallClass::ClientTextureUploadEmissions, "
                                    "the ctu= field) does not exist there, and a one-sided reading is "
                                    "not the comparison this scenario is for. The entry is registered "
                                    "in every build so that `ctest -L integration-gpu` names the same "
                                    "tests in the pull build and the push build (gate G2).";
                    return;
                }
                if (PipeStatsWindow::LibraryLogPath().empty()) {
                    GTEST_SKIP() << "the lane configured no MOBILEGL_LOG_FILE_PATH, and the library's "
                                    "summary line is the only channel this module has for reading "
                                    "PipeStats";
                    return;
                }
                if (!TextureViewUsable()) {
                    // A SKIP AND NOT A SMALLER WORKLOAD. The gold table has one row; dropping the
                    // third texture here would leave a case that asserts emit==9 against a
                    // six-emission workload, i.e. a gate that fails for the wrong reason - or,
                    // worse, a second gold row that nobody re-measures.
                    GTEST_SKIP() << "GL_ARB_texture_view is not advertised on this backend, so the "
                                    "third texture - the one whose upload goes through a "
                                    "glTextureView, which is the input the P3b/P4b emission cursor "
                                    "work changed - cannot be built. Every lane this case is "
                                    "registered in sets MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1.";
                    return;
                }
            }

            bool TextureViewUsable() {
                GLuint storage = 0;
                glGenTextures(1, &storage);
                glBindTexture(GL_TEXTURE_2D, storage);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
                glBindTexture(GL_TEXTURE_2D, 0);
                GLuint view = 0;
                glGenTextures(1, &view);
                while (glGetError() != GL_NO_ERROR) {
                }
                glTextureView(view, GL_TEXTURE_2D, storage, GL_RGBA8, 0, 1, 0, 1);
                const bool usable = glGetError() == GL_NO_ERROR;
                glDeleteTextures(1, &view);
                glDeleteTextures(1, &storage);
                return usable;
            }

            static std::vector<std::uint8_t> GreenTexels(int width, int height) {
                std::vector<std::uint8_t> texels(static_cast<std::size_t>(width) * height * 4);
                for (std::size_t i = 0; i < texels.size(); i += 4) {
                    texels[i] = 0;
                    texels[i + 1] = 255;
                    texels[i + 2] = 0;
                    texels[i + 3] = 255;
                }
                return texels;
            }

            GLuint MakeAtlas(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
                std::vector<std::uint8_t> texels(static_cast<std::size_t>(kAtlasSize) * kAtlasSize * 4);
                for (std::size_t i = 0; i < texels.size(); i += 4) {
                    texels[i] = r;
                    texels[i + 1] = g;
                    texels[i + 2] = b;
                    texels[i + 3] = 255;
                }
                GLuint texture = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kAtlasSize, kAtlasSize, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, texels.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                return texture;
            }

            // The third texture's OWNER: immutable, two levels, seeded green on both. glTextureView
            // requires immutable storage, so this one cannot be a MakeAtlas.
            GLuint MakeViewedOwner() {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexStorage2D(GL_TEXTURE_2D, kViewedOwnerLevels, GL_RGBA8, kViewedOwnerSize,
                               kViewedOwnerSize);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                for (int level = 0; level < kViewedOwnerLevels; ++level) {
                    const int size = kViewedOwnerSize >> level;
                    const std::vector<std::uint8_t> green = GreenTexels(size, size);
                    glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE,
                                    green.data());
                }
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glBindTexture(GL_TEXTURE_2D, 0);
                return texture;
            }

            // A view of the owner's LEVEL 1, which is kAtlasSize square: the view's own level 0.
            // Every glTexSubImage2D through this name has to be remapped to the owner's level 1 by
            // TextureObjectView::ToOwnerLevel before it reaches the drain list, and the emission it
            // produces has to land on the OWNER's cursor.
            GLuint MakeLevelView(GLuint owner) {
                GLuint view = 0;
                glGenTextures(1, &view);
                glTextureView(view, GL_TEXTURE_2D, owner, GL_RGBA8, kViewMinLevel, 1, 0, 1);
                EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR))
                    << "a single-level 2D view of a 2D texture is a legal view pair";
                glBindTexture(GL_TEXTURE_2D, view);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glBindTexture(GL_TEXTURE_2D, 0);
                return view;
            }

            void ScatterRectsInto(GLuint texture) {
                glBindTexture(GL_TEXTURE_2D, texture);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                const std::uint8_t patch[kRectSize * kRectSize * 4] = {
                    0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
                for (int rect = 0; rect < kScatteredRects; ++rect) {
                    const int x = kScatterMargin + ((rect * 7) % kScatterCells) * kRectSize;
                    const int y = kScatterMargin + ((rect * 5) % kScatterCells) * kRectSize;
                    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, kRectSize, kRectSize, GL_RGBA,
                                    GL_UNSIGNED_BYTE, patch);
                }
            }

            Image DrawSampled(GLuint texture) {
                BindDefaultFramebuffer();
                glViewport(0, 0, Gl().Width(), Gl().Height());
                glUseProgram(m_program);
                glUniform1i(glGetUniformLocation(m_program, "uTex"), 0);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, texture);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                return ReadPixels(Gl().Width(), Gl().Height());
            }

            GLuint m_program = 0;
            GLuint m_vao = 0;
            GLuint m_quadBuffer = 0;
            GLuint m_scattered = 0;
            GLuint m_contiguous = 0;
            GLuint m_viewedOwner = 0;
            GLuint m_view = 0;
        };

        TEST_F(TextureUploadShapeScenario, TheEmittedUploadShapeIsRecordedAndTheTwoSidesAgree) {
            if (!Ready()) return;
            SkipUnlessTheLaneIsAssertableHere();
            if (IsSkipped()) return;

            m_scattered = MakeAtlas(0, 255, 0);
            m_contiguous = MakeAtlas(0, 255, 0);
            m_viewedOwner = MakeViewedOwner();
            m_view = MakeLevelView(m_viewedOwner);
            // The first draw of each texture uploads its whole level, which is not the shape this
            // scenario is about; it happens in the SETUP window, before the one that is read.
            DrawSampled(m_scattered);
            DrawSampled(m_contiguous);
            DrawSampled(m_view);
            BindDefaultFramebuffer();
            Gl().EndFrame();
            // The server closes/resets the stats window inside Present. Under
            // run-ahead its client return is earlier: do not let this workload's
            // client emissions enter the setup window that has not retired yet.
            if (const auto runtime = PeekSplitRuntime(); runtime.transportResolved) {
                ASSERT_TRUE(runtime.sessionActive);
                ASSERT_TRUE(WaitForSplitAppliedForTesting(runtime.emitSeq));
            }

            // ---- the counted window ----
            //
            // THE CLIENT HALF IS READ FROM THE EMITTER, NOT FROM ctu=, and that is a correction
            // this package had to make rather than a preference. MEASURED on all four arms:
            //
            //   monolith  one role, one log: the window line carries tex[emit=9] AND ctu=9.
            //   inproc    two log FILES, one process, one set of counters: the SERVER's line
            //             carries both, and the client's log holds only the "counters ON" banner
            //             at the moment this case reads it.
            //   spawn/tcp two PROCESSES with two counter sets. The server's line carries
            //             tex[emit=9] ctu=0; the client process emits its own summary on a
            //             DIFFERENT Present cadence and had written only the banner by the time
            //             this case reads - and when it does write one, its window covers the
            //             setup emissions too (measured ctu=13 against emit=9), so the two
            //             numbers are not windowed on the same work and `ctu == emit` is not a
            //             well-posed comparison there at all.
            //
            // So the log's ctu= is kept as a RECORD (printed, and asserted where it is
            // window-aligned) and the ASSERTION reads MGPipeTextureEmitter's own counter through
            // the in-process peek. That is the same counter's SOURCE - EmitOneLevel increments
            // both on the same line - and the test process is the CLIENT under every one of
            // monolith / inproc / spawn / tcp, so the delta below is the client's true count of
            // the records this workload produced, in every arm. D-L asks for the two sides to
            // agree; this is what makes that a real question outside monolith.
            TextureEmitCountersPeek emitBefore{};
            const bool emitPeekLive = PeekTextureEmitCounters(&emitBefore);
            // The log mark the two role readings are taken against, so that a teardown window
            // cannot be mistaken for the workload's (see PipeStatsWindow.h).
            const PipeStatsWindow::LogMark statsMark = PipeStatsWindow::MarkLaneLog();

            Image lastScattered;
            Image lastContiguous;
            Image lastViewed;
            for (int frame = 0; frame < kFrames; ++frame) {
                // MANY SMALL SCATTERED RECTS: the shape whose box-versus-rect decision is the whole
                // subject. They are spread over the atlas on a coarse stride so that their union
                // box is most of the texture and their summed area is a small fraction of it -
                // which is the input the summedArea*4 >= unionArea*3 fallback is written for.
                ScatterRectsInto(m_scattered);
                lastScattered = DrawSampled(m_scattered);

                // ONE LARGE CONTIGUOUS REGION: the control. Whatever the policy is, this is one
                // box and one job, and a reading where it is not says the policy has stopped
                // looking at the region at all.
                glBindTexture(GL_TEXTURE_2D, m_contiguous);
                const std::vector<std::uint8_t> band = GreenTexels(kAtlasSize, 8);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kAtlasSize, 8, GL_RGBA, GL_UNSIGNED_BYTE,
                                band.data());
                lastContiguous = DrawSampled(m_contiguous);

                // THE SAME SCATTERED PATTERN THROUGH A VIEW. The writes name the view's level 0;
                // the drain has to key them on the OWNER at the owner's level 1, and the shape the
                // emitter then chooses has to be the same one the owner's own name would have got.
                // A cursor that keyed on the view would show up here as a DIFFERENT emit count,
                // not as a wrong picture.
                ScatterRectsInto(m_view);
                lastViewed = DrawSampled(m_view);
            }
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the upload workload left a GL error behind";
            TextureEmitCountersPeek emitAfter{};
            const bool emitPeekClosed = emitPeekLive && PeekTextureEmitCounters(&emitAfter);
            Gl().EndFrame(); // the swap that emits the window covering exactly the work above
            if (const auto runtime = PeekSplitRuntime(); runtime.transportResolved) {
                ASSERT_TRUE(runtime.sessionActive);
                ASSERT_TRUE(WaitForSplitAppliedForTesting(runtime.emitSeq));
            }

            // ---- READING TWO ROLES, NOT ONE LINE --------------------------------------------
            //
            // Under SPAWN and TCP the client and the server are two PROCESSES with two independent
            // sets of counters: the client's summary line carries `ctu=N tex[emit=0]` and the
            // server's carries `ctu=0 tex[emit=N]`. A single concatenated read takes whichever line
            // came last and answers BOTH questions with one role's numbers, which turns the
            // two-sided comparison into `N == 0`. So each side is read from ITS OWN role's log, and
            // the server bracket falls back to the client log only where there is no server half -
            // the pull build and the monolith lane, where one file IS the whole run.
            const PipeStatsWindow::Window clientWindow =
                PipeStatsWindow::LastFromClientLogSince(statsMark);
            const PipeStatsWindow::Window serverWindow =
                PipeStatsWindow::LastFromServerLogSince(statsMark);
            const PipeStatsWindow::Window& shapeWindow =
                (serverWindow.found && PipeStatsWindow::CounterOrAbsent(serverWindow, "emit") > 0)
                    ? serverWindow
                    : clientWindow;
            // THE ASSERTION IS ON THE WINDOW THAT CARRIES THE BRACKET, not on the client's, and
            // moving to a marked read is what showed why. The client role emits NO summary window
            // of its own under inproc/spawn/tcp at the point this case reads - only the
            // "counters ON" banner, which also contains the marker string. Asserting
            // clientWindow.found over the whole file therefore passed on the BANNER in all three
            // split arms and claimed a window had been read when none had. Since the mark there
            // is no banner to hide behind, so the claim has to be made about the role that
            // actually published the counters.
            ASSERT_TRUE(shapeWindow.found)
                << "no 'MGPipe stats:' window was appended to either role's log by the swap that "
                   "closes the counted window. client="
                << PipeStatsWindow::LibraryLogPath()
                << " server=" << PipeStatsWindow::ServerLibraryLogPath()
                << ". The shape could not be read at all - which is a different finding from a "
                   "shape that read zero.";
            RecordProperty("stats_line_client", clientWindow.line.c_str());
            if (serverWindow.found) RecordProperty("stats_line_server", serverWindow.line.c_str());

            const long long emissions = PipeStatsWindow::CounterOrAbsent(shapeWindow, "emit");
            const long long box = PipeStatsWindow::CounterOrAbsent(shapeWindow, "box");
            const long long rect = PipeStatsWindow::CounterOrAbsent(shapeWindow, "rect");
            const long long jobs = PipeStatsWindow::CounterOrAbsent(shapeWindow, "jobs");
            // The log's ctu=, from whichever role's window carries it. Under monolith and inproc
            // that is the same window the tex[] bracket came from; under spawn and tcp it is a
            // different process's differently-windowed counter, so it is RECORDED and only
            // asserted where the two are window-aligned (see the note over the counted window).
            const long long shapeWindowCtu = PipeStatsWindow::CounterOrAbsent(shapeWindow, "ctu");
            const long long clientWindowCtu = PipeStatsWindow::CounterOrAbsent(clientWindow, "ctu");
            // ALIGNED IS A PROPERTY OF THE TRANSPORT, NOT OF THE NUMBER, and asking the number
            // was this case's first mistake: `shapeWindowCtu >= 0` is TRUE under spawn, where the
            // server's line publishes a perfectly readable ctu=0 that belongs to a process which
            // emitted nothing, and the comparison became 0 == 9. Asking the process which
            // transport it resolved cannot be satisfied by a counter that happens to read zero -
            // and a single-process arm whose ctu= really did collapse to 0 still reds, which is
            // the failure the alignment check must not swallow.
            const SplitRuntimeState statsRuntime = PeekSplitRuntime();
            const bool oneProcessForCounters =
                !statsRuntime.transportResolved || statsRuntime.transportName == "inproc";
            const bool ctuIsWindowAligned = oneProcessForCounters && shapeWindowCtu >= 0;
            const long long clientEmissions =
                emitPeekClosed
                    ? static_cast<long long>(emitAfter.SubDataCount - emitBefore.SubDataCount)
                    : shapeWindowCtu;
            ASSERT_GE(emissions, 0) << "the summary line carries no tex[emit=]: " << shapeWindow.line;
            ASSERT_GE(box, 0) << "no box=: " << shapeWindow.line;
            ASSERT_GE(rect, 0) << "no rect=: " << shapeWindow.line;
            ASSERT_GE(jobs, 0) << "no jobs=: " << shapeWindow.line;

            std::cout << "[ TextureUploadShape ] backend=" << Gl().BackendName() << " frames=" << kFrames
                      << " scattered_rects_per_frame=" << kScatteredRects
                      << " textures=" << kTexturesInTheWindow
                      << " server[emit=" << emissions << " box=" << box << " rect=" << rect
                      << " jobs=" << jobs << "] client[emitter=" << clientEmissions
                      << " ctu_shape_window=" << shapeWindowCtu
                      << " ctu_client_window=" << clientWindowCtu << "]" << std::endl;
            RecordProperty("ctu_shape_window", static_cast<int>(shapeWindowCtu));
            RecordProperty("ctu_client_window", static_cast<int>(clientWindowCtu));
            RecordProperty("ctu_is_window_aligned", ctuIsWindowAligned ? 1 : 0);
            RecordProperty("server_emissions", static_cast<int>(emissions));
            RecordProperty("server_box_emissions", static_cast<int>(box));
            RecordProperty("server_rect_emissions", static_cast<int>(rect));
            RecordProperty("server_upload_jobs", static_cast<int>(jobs));
            RecordProperty("client_emissions", static_cast<int>(clientEmissions));

            // 1. the workload really uploaded. Without this the assertions below are all 0 == 0 and
            //    a run whose emitter was switched off records the same "healthy" shape as one that
            //    worked.
            ASSERT_GT(emissions, 0)
                << "the server counted no texture upload emission at all over " << kFrames
                << " frames of " << kScatteredRects
                << " sub-regions each, plus a contiguous band and a view write. Either the uploads "
                   "never reached the backend or the counter stopped counting; in both cases every "
                   "shape number below would be a zero that means nothing. "
                << shapeWindow.line;

            // 2. the server bracket's own arithmetic.
            EXPECT_EQ(box + rect, emissions)
                << "tex[box=] + tex[rect=] must be tex[emit=]: every emission takes exactly one of "
                   "the two shapes. " << shapeWindow.line;
            EXPECT_GE(jobs, emissions)
                << "tex[jobs=] must be at least tex[emit=]: a box emission is one driver upload job "
                   "and a rect-list emission is N. " << shapeWindow.line;

            // 3. THE GOLD TABLE (R-11 / D-D4). This is the assertion the three around it cannot
            //    make: emit == box + rect and jobs >= emit are both satisfied by a shape that
            //    inverted wholesale from N boxes to N rect lists, which is the +6 ms/frame
            //    direction on Mali and is invisible to SSIM.
            EXPECT_EQ(emissions, kGold.Emissions)
                << "the emission COUNT moved. One resource_subdata per (storage owner, upload "
                   "target, level) per frame is " << kTexturesInTheWindow << " textures x " << kFrames
                << " frames = " << kGold.Emissions
                << ". A count above this says a level is being emitted more than once per validate "
                   "point - the two-cursor shape, if the extra one is the viewed texture; a count "
                   "below says a level's record was dropped. " << shapeWindow.line;
            EXPECT_EQ(box, kGold.BoxEmissions)
                << "the BOX/RECT split moved: " << box << " box emissions against a gold of "
                << kGold.BoxEmissions << ". " << shapeWindow.line;
            EXPECT_EQ(rect, kGold.RectEmissions)
                << "the BOX/RECT split moved: " << rect << " rect-list emissions against a gold of "
                << kGold.RectEmissions
                << ". This is the direction that costs ~+6 ms/frame on Mali (ARCHITECTURE.md:249-"
                   "251): the driver is handed N upload jobs where it used to be handed one, and no "
                   "SSIM case can see it. If this is intended, re-measure the row and say so in "
                   "docs/Disaggregated/MEASUREMENTS.md. " << shapeWindow.line;
            EXPECT_EQ(jobs, kGold.Jobs)
                << "the JOB count moved: " << jobs << " against a gold of " << kGold.Jobs
                << ". Jobs are what Mali prices the upload by. " << shapeWindow.line;

            // 4. the two sides agree, WHEN THERE ARE TWO SIDES - and "there are two sides" is
            //    answered by the BUILD, not by the number (review F-m7).
            //
            //    ctu= IS ALWAYS PRESENT IN A PUSH BUILD: PipeStats.cpp writes the field whether or
            //    not anything ever incremented the counter, so `clientEmissions > 0` conflated
            //    three different trees - "no client emitter exists", "the emitter exists and
            //    emitted nothing", and "the counter was not published at all" - into one branch
            //    that asserts nothing. So the discriminator is
            //    MGITEST_PIPE_CLIENT_TEXTURE_UPLOAD_EMITTER_PRESENT, the build's own content probe
            //    for a MG_Impl/Pipe source that emits CallClass::ClientTextureUploadEmissions.
            //
            //    THAT PROBE HITS ON THIS TREE: MG_Impl/Pipe/TextureEmit.h's EmitOneLevel increments
            //    it on every piece. The else-arm below is retained because it is what makes the
            //    arming decision falsifiable in both directions, not because it is reachable here.
            const bool clientEmitterExists =
                BuildMarkerIsSet("MGITEST_PIPE_CLIENT_TEXTURE_UPLOAD_EMITTER_PRESENT");
            ASSERT_GE(clientEmissions, 0)
                << "the client half of the comparison could not be read at all. The in-process "
                   "emitter peek "
                << (emitPeekLive ? "opened but did not close" : TextureEmitPeekSkipReason())
                << ", and no role's summary line carried a ctu= field either. client="
                << clientWindow.line << " shape=" << shapeWindow.line;
            RecordProperty("client_emitter_present", clientEmitterExists ? 1 : 0);
            RecordProperty("client_half_source", emitPeekClosed ? "emitter peek" : "ctu= field");
            if (ctuIsWindowAligned) {
                // Where the two roles ARE one window, the published field must agree with the
                // emitter it is published from. A divergence here is PipeStats mis-publishing,
                // which is a different defect from an emission-shape divergence and has to stay
                // distinguishable from it.
                EXPECT_EQ(shapeWindowCtu, clientEmissions)
                    << "the published ctu= field and MGPipeTextureEmitter's own SubDataCount "
                       "disagree in a window that covers the same work. EmitOneLevel increments "
                       "both on the same line, so this is the counter's publication and not the "
                       "emission shape. " << shapeWindow.line;
            } else {
                RecordProperty("ctu_note",
                               "the client's summary window is not aligned with the server's in "
                               "this arm (two processes, two Present cadences), so ctu= is "
                               "recorded and the assertion reads the in-process emitter instead");
            }
            if (clientEmitterExists) {
                EXPECT_GT(clientEmissions, 0)
                    << "a MG_Impl/Pipe source emits CallClass::ClientTextureUploadEmissions on this "
                       "tree, and the SERVER counted " << emissions
                    << " texture upload emissions for this workload, but the client counted NONE. An "
                       "emitter that has stopped emitting reads exactly like no emitter at all in "
                       "this field, which is why this case asks the build rather than the number. "
                    << clientWindow.line;
                EXPECT_EQ(clientEmissions, emissions)
                    << "the CLIENT counted " << clientEmissions
                    << " texture upload records and the SERVER counted " << emissions
                    << " for the same workload in the same window. The two counting the same records "
                       "is the entire reason both are published (D-L): a divergence here is an "
                       "emission-shape divergence that SSIM is blind to and that costs ~+6 ms/frame "
                       "on Mali when it goes the wrong way. client=" << clientWindow.line
                    << " server=" << shapeWindow.line;
                EXPECT_EQ(clientEmissions, kGold.Emissions)
                    << "the client's own count against the gold row, so that a run where BOTH sides "
                       "moved together is still red. " << clientWindow.line;
            } else {
                // Not merely "not asserted": on a tree with no client emitter the counter must be
                // ZERO, and a non-zero one would mean the probe is looking for the wrong symbol -
                // i.e. that the arming decision above is wrong and every future run of this case
                // is mis-armed.
                EXPECT_EQ(clientEmissions, 0)
                    << "no MG_Impl/Pipe source emits CallClass::ClientTextureUploadEmissions on this "
                       "tree, yet the client counted " << clientEmissions
                    << " of them. Something is incrementing that counter which this build's probe "
                       "cannot see, so the probe is looking for the wrong symbol and this case's "
                       "arming decision is unreliable in both directions. " << clientWindow.line;
                RecordProperty("client_side", "absent");
            }

            // The pixels, so that a recorded shape cannot be the shape of a workload that drew
            // nothing.
            EXPECT_TRUE(RegionIsMostly(lastScattered, kInset, lastScattered.Width() - kInset, kInset,
                                       lastScattered.Height() - kInset, "green", 0.0,
                                       "the scattered-rect atlas"));
            EXPECT_TRUE(RegionIsMostly(lastContiguous, kInset, lastContiguous.Width() - kInset, kInset,
                                       lastContiguous.Height() - kInset, "green", 0.0,
                                       "the contiguous-band atlas"));
            EXPECT_TRUE(RegionIsMostly(lastViewed, kInset, lastViewed.Width() - kInset, kInset,
                                       lastViewed.Height() - kInset, "green", 0.0,
                                       "the atlas written through a glTextureView"));
        }

    } // namespace
} // namespace MGITest
