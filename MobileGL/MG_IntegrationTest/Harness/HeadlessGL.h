// MobileGL - MobileGL/MG_IntegrationTest/Harness/HeadlessGL.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// A headless GL context and the small vocabulary the scenarios are written in.
//
// The scenarios in this module are end-to-end: they drive MobileGL's own GL and
// EGL entry points (this binary links MobileGL_s, so gl*/egl* resolve straight
// into the implementation) and assert on glReadPixels output. Nothing here
// inspects backend state - both bugs this module pins were invisible to
// state-level assertions and visible only in pixels.
//
// Headless by construction: desktop uses an EGL pbuffer and Android uses an
// AImageReader-backed ANativeWindow that needs no Activity. No window manager,
// no human. Unlike DriverBench the scenarios do draw to the DEFAULT framebuffer
// (that is where the Y-flip lives) and do call eglSwapBuffers (that is the frame
// boundary the cross-frame scenarios need to be real).
//
// One process is one backend: MOBILEGL_BACKEND_TYPE is latched at
// initialization, so the CMake wiring runs this binary once per backend rather
// than trying to switch in-process.

#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace MGITest {

    // True when MOBILEGL_ITEST_REQUIRE_GPU is set in the environment: the runner
    // is asserting that this machine HAS a usable GPU, so "no GPU" stops being a
    // clean skip and becomes a failure. Without it the integration-gpu label is
    // unfalsifiable - a CI job that ran nothing reports exactly the same green as
    // a job that ran everything.
    bool RequireGpu();

    // True when MOBILEGL_ITEST_REQUIRE_HARDWARE_GPU is set: additionally asserts
    // that the context did NOT land on a software rasterizer. Deliberately a
    // SEPARATE switch from RequireGpu - a GPU-less CI runner is a supported and
    // intended configuration for these scenarios (they pin backend draw logic,
    // which llvmpipe/lavapipe execute faithfully), so CI wants the falsifiability
    // of REQUIRE_GPU without the hardware demand. Use this one only where a vendor
    // pin silently degrading to software would invalidate the measurement.
    bool RequireHardwareGpu();

    struct Rgba8 {
        std::uint8_t r = 0, g = 0, b = 0, a = 0;

        bool operator==(const Rgba8& other) const {
            return r == other.r && g == other.g && b == other.b && a == other.a;
        }
        bool operator!=(const Rgba8& other) const { return !(*this == other); }
    };

    // Prints as "rgba(255,0,0,255)" so a gtest failure names the colour it saw.
    std::ostream& operator<<(std::ostream& os, const Rgba8& c);

    // An RGBA8 readback. Row 0 is the BOTTOM row: that is GL's convention for
    // glReadPixels and it is what "correctly oriented" means everywhere below.
    class Image {
    public:
        Image() = default;
        Image(int width, int height)
            : m_width(width), m_height(height), m_pixels(static_cast<std::size_t>(width) * height * 4, 0) {}

        int Width() const { return m_width; }
        int Height() const { return m_height; }
        bool Empty() const { return m_pixels.empty(); }
        std::uint8_t* Data() { return m_pixels.data(); }
        const std::uint8_t* Data() const { return m_pixels.data(); }

        Rgba8 At(int x, int y) const;
        // Nearest of {black, red, green, blue, white, other} - the scenarios only
        // ever draw those, so this turns a pixel into something readable.
        const char* ColorName(int x, int y) const;

        bool operator==(const Image& other) const {
            return m_width == other.m_width && m_height == other.m_height && m_pixels == other.m_pixels;
        }

        // Count of differing bytes, for a failure message that says how wrong.
        std::size_t ByteDiffCount(const Image& other) const;

        // The four quadrant centres, in the fixed order
        // bottom-left, bottom-right, top-left, top-right.
        //
        // This replaces the old VerticalSignature(bandCount), which read three
        // full-width horizontal stripes down the centre line and was therefore
        // blind to an X flip, to a transpose, and to a 180 rotation composed with
        // a Y flip - all of those left the stripe order alone. Four quadrant
        // colours are asymmetric in BOTH axes, so each of the eight square
        // symmetries produces a different string (see OrientationScenario, which
        // spells all eight out).
        std::string QuadrantSignature() const;

    private:
        int m_width = 0;
        int m_height = 0;
        std::vector<std::uint8_t> m_pixels;
    };

    // The process-wide headless context. Brought up lazily on the first Get() so
    // that `--gtest_list_tests` (which CMake runs at build time to discover the
    // cases) never touches a GPU.
    class HeadlessGL {
    public:
        static HeadlessGL& Get();

        // False on a machine with no usable GPU/display/ICD. SkipReason() then
        // says which step failed; every fixture turns that into GTEST_SKIP().
        bool Usable() const { return m_usable; }
        const std::string& SkipReason() const { return m_skipReason; }

        // Backend actually in use, as reported by MOBILEGL_BACKEND_TYPE.
        const std::string& BackendName() const { return m_backendName; }
        const std::string& RendererString() const { return m_renderer; }

        int Width() const { return m_width; }
        int Height() const { return m_height; }

        // THE frame boundary. eglSwapBuffers is what retires a frame in the
        // renderer, and the cross-frame scenarios are meaningless without it.
        void EndFrame();

        // Frames completed so far, for failure messages.
        int FrameIndex() const { return m_frameIndex; }

        // Releases the context and surface and terminates the display. Called
        // once, after the last scenario: MobileGL frees its backend objects
        // through eglTerminate, and letting a process simply exit on top of a
        // live context leaves those objects to be torn down from a static
        // destructor with no driver left underneath.
        void ShutDown();

    private:
        HeadlessGL();
        HeadlessGL(const HeadlessGL&) = delete;
        HeadlessGL& operator=(const HeadlessGL&) = delete;

        bool BringUp();

        bool m_usable = false;
        std::string m_skipReason;
        std::string m_backendName;
        std::string m_renderer;
        int m_width = 0;
        int m_height = 0;
        int m_frameIndex = 0;
        void* m_display = nullptr;
        void* m_surface = nullptr;
        void* m_context = nullptr;
    };

    // ---- the scenario vocabulary -------------------------------------------
    // Deliberately tiny. A scenario should read like a story; anything that
    // needs a comment about GL mechanics belongs here instead.

    // Compiles and links vs+fs, pinning attribute 0 to "aPos" and 1 to "aColor".
    // Returns 0 and fills outError on failure.
    unsigned int CompileProgram(const char* vertexSource, const char* fragmentSource, std::string* outError);

    struct ColorFbo {
        unsigned int fbo = 0;
        unsigned int texture = 0;
        int width = 0;
        int height = 0;
    };

    // A complete RGBA8 render target. Returns fbo==0 on failure.
    ColorFbo MakeColorFbo(int width, int height);
    void DestroyColorFbo(ColorFbo& target);

    // Binds a target and sets the viewport to match. Passing fbo 0 means the
    // default (presentable) framebuffer.
    void BindDefaultFramebuffer();
    void BindFbo(const ColorFbo& target);

    void ClearTo(float r, float g, float b, float a);

    // Reads back the whole currently bound READ framebuffer.
    Image ReadPixels(int width, int height);

    // A PARTIAL glReadPixels. Row 0 of the returned image is GL row `y` of the
    // framebuffer, i.e. the bottom row of the requested rect - the same
    // convention ReadPixels uses, just with an origin. This is the shape the
    // conformance suite reads in (a random sub-rect of the default
    // framebuffer), and the shape DirectVulkan's default-FBO readback used to
    // hand back in Vulkan row order because its re-orientation only ran on an
    // exact full-extent read.
    Image ReadPixelsRect(int x, int y, int width, int height);

    // Drains any GL error queue and returns the first error, or 0.
    unsigned int FirstGLError();
    const char* GLErrorName(unsigned int error);

    // ---- whole-region readback predicates ----------------------------------
    // The scenarios used to assert on two or three individual pixels, which is
    // provably too weak: a draw in which 3 of a quad's 4 vertices carry stale
    // data still paints the sampled centre the expected colour (that exact case
    // is a standing negative-control test - see CrossFrameBufferScenario). The
    // readback is already fully in memory, so counting every pixel in a region
    // costs nothing and turns "the middle looks right" into "all of it is right".

    // Everything a caller needs to say what was wrong and where.
    struct RegionScan {
        int total = 0;     // pixels examined
        int offenders = 0; // pixels whose ColorName() != expected
        int firstX = -1;   // first offender in bottom-to-top, left-to-right order
        int firstY = -1;
        Rgba8 firstColor{};
        std::string firstColorName;
    };

    // Inclusive pixel bounds, clamped to the image. Row 0 is the bottom row.
    RegionScan ScanRegion(const Image& image, int x0, int x1, int y0, int y1, const char* expectedColor);

    // gtest predicate wrapper: EXPECT_TRUE(RegionIsMostly(...)) reports the
    // offender count, the offender fraction and the FIRST offending pixel's
    // coordinates and colour. `tolerance` is the fraction of the region allowed
    // to disagree; pass 0.0 to demand every pixel (which is what the scenarios
    // do - they inset their regions away from primitive edges so exactness is
    // achievable).
    ::testing::AssertionResult RegionIsMostly(const Image& image, int x0, int x1, int y0, int y1,
                                              const char* expectedColor, double tolerance,
                                              const std::string& when);

} // namespace MGITest
