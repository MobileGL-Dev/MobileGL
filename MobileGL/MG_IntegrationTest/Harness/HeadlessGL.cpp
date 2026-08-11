// MobileGL - MobileGL/MG_IntegrationTest/Harness/HeadlessGL.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "HeadlessGL.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <sstream>

// MobileGL's own headers, in the order MobileGL/Includes.h uses them: GL/gl.h
// first, then glcorearb.h for the 3.x+ entry points. This binary links
// MobileGL_s, so every gl*/egl* below binds to MobileGL's implementation, not
// to a system loader.
#ifdef GLAPI
#undef GLAPI
#endif
#include <EGL/egl.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

// The pre-flight below runs the whole EGL bring-up in a forked child, which is
// the only construction that is actually predictive here: MobileGL ABORTS
// (MOBILEGL_ASSERT -> SIGTRAP) rather than returning an error on an unusable
// platform, so nothing the parent can call in-process is allowed to be wrong.
#if !defined(_WIN32) && !defined(__APPLE__) && __has_include(<sys/wait.h>)
#define MGITEST_HAVE_FORK_PREFLIGHT 1
#include <csignal>
#include <ctime>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#define MGITEST_HAVE_FORK_PREFLIGHT 0
#endif

namespace MGITest {

    namespace {
        // Small enough that a readback is cheap, big enough that "top third" and
        // "bottom third" are unambiguous. Non-square on purpose: a transposing
        // bug cannot hide behind a square.
        constexpr int kSurfaceWidth = 128;
        constexpr int kSurfaceHeight = 96;

        std::string EnvOr(const char* name, const char* fallback) {
            const char* value = std::getenv(name);
            return (value != nullptr && value[0] != '\0') ? std::string(value) : std::string(fallback);
        }

        // A skip reason is only useful if it says which call failed AND why, so
        // every bring-up step reports the EGL error it left behind.
        std::string WithEglError(const char* what) {
            std::ostringstream out;
            out << what << " (eglGetError=0x" << std::hex << eglGetError() << ")";
            return out.str();
        }

        // The EGL objects one bring-up produces.
        struct EglBringUp {
            void* display = nullptr;
            void* surface = nullptr;
            void* context = nullptr;
            std::string renderer;
        };

        // The harness is headless BY CONSTRUCTION, on every machine: it must never
        // reach a window system, not even where one happens to be running. This is
        // not a CI accommodation - it is what keeps a developer's run and a CI run
        // the same run. The lane was wired up green on a workstation and immediately
        // died on the runner precisely because the workstation had a DISPLAY (WSLg)
        // and took Mesa's x11 platform, while the runner has none; that divergence
        // is the bug, and pinning the platform here is the fix for it.
        //
        // Mesa selects its EGL platform from EGL_PLATFORM at loader time, so this
        // has to run before the first EGL call in the process (see EnsureHeadless
        // callers). surfaceless is the platform with no window-system dependency at
        // all; the surface this file then creates is still a pbuffer, which every
        // platform supports and which the amendment to this rule requires as the
        // fallback shape. DISPLAY/WAYLAND_DISPLAY are cleared as well so that a
        // driver that consults them directly cannot reintroduce the dependency
        // behind EGL's back. Desktop-only file: MG_IntegrationTest never builds
        // for Android, so no device path is affected.
        void EnsureHeadlessPlatform() {
#if defined(__linux__) && !defined(__ANDROID__)
            static bool done = false;
            if (done) {
                return;
            }
            done = true;
            // An explicit EGL_PLATFORM from the operator still wins: pinning a
            // platform is exactly how someone reproduces a platform-specific bug.
            if (std::getenv("EGL_PLATFORM") == nullptr) {
                setenv("EGL_PLATFORM", "surfaceless", 1);
            }
            unsetenv("DISPLAY");
            unsetenv("WAYLAND_DISPLAY");
#endif
        }

        // THE bring-up, in one function so the pre-flight child and the parent run
        // literally the same sequence - a pre-flight that tests something narrower
        // than what the parent will do is exactly the kind of "predictive" check
        // that is not.
        //
        // Returns 0 on success, or the 1-based index of the step that failed, and
        // fills outReason either way.
        int RunEglBringUp(EglBringUp& out, std::string& outReason) {
            // Belt and braces: the pre-flight child and the parent both enter here,
            // and neither may be the first to touch EGL without this having run.
            EnsureHeadlessPlatform();
            EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            if (display == EGL_NO_DISPLAY) {
                outReason = WithEglError("eglGetDisplay(EGL_DEFAULT_DISPLAY) returned EGL_NO_DISPLAY");
                return 1;
            }
            EGLint major = 0, minor = 0;
            if (eglInitialize(display, &major, &minor) != EGL_TRUE) {
                outReason = WithEglError("eglInitialize failed: no usable display/driver on this machine");
                return 2;
            }
            if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) {
                outReason = WithEglError("eglBindAPI(EGL_OPENGL_API) failed");
                return 3;
            }

            const EGLint configAttribs[] = {EGL_SURFACE_TYPE,
                                            EGL_PBUFFER_BIT,
                                            EGL_RED_SIZE,
                                            8,
                                            EGL_GREEN_SIZE,
                                            8,
                                            EGL_BLUE_SIZE,
                                            8,
                                            EGL_ALPHA_SIZE,
                                            8,
                                            EGL_DEPTH_SIZE,
                                            24,
                                            EGL_RENDERABLE_TYPE,
                                            EGL_OPENGL_BIT,
                                            EGL_NONE};
            EGLConfig config = nullptr;
            EGLint configCount = 0;
            if (eglChooseConfig(display, configAttribs, &config, 1, &configCount) != EGL_TRUE || configCount < 1) {
                outReason = WithEglError("eglChooseConfig found no pbuffer-capable RGBA8/D24 config");
                return 4;
            }

            const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE};
            EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
            if (context == EGL_NO_CONTEXT) {
                context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
            }
            if (context == EGL_NO_CONTEXT) {
                outReason = WithEglError("eglCreateContext failed: no desktop-GL context available");
                return 5;
            }

            const EGLint pbufferAttribs[] = {EGL_WIDTH, kSurfaceWidth, EGL_HEIGHT, kSurfaceHeight, EGL_NONE};
            EGLSurface surface = eglCreatePbufferSurface(display, config, pbufferAttribs);
            if (surface == EGL_NO_SURFACE) {
                outReason = WithEglError("eglCreatePbufferSurface failed");
                return 6;
            }
            // The step that brings the whole backend up (DirectVulkan creates its
            // instance, device and surface in here) and therefore the step that
            // aborts instead of returning an error on an unusable platform.
            if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
                outReason = WithEglError("eglMakeCurrent failed");
                return 7;
            }

            const GLubyte* renderer = glGetString(GL_RENDERER);
            if (renderer == nullptr) {
                outReason = "glGetString(GL_RENDERER) returned null after eglMakeCurrent";
                return 8;
            }

            out.display = display;
            out.surface = surface;
            out.context = context;
            out.renderer = reinterpret_cast<const char*>(renderer);
            outReason.clear();
            return 0;
        }

        // Platform pre-flight, and the reason this module can claim to skip
        // cleanly rather than merely hope to.
        //
        // MobileGL does not return errors when the platform is unusable - it
        // ABORTS. MOBILEGL_ASSERT raises SIGTRAP, and the DirectVulkan bring-up
        // asserts its way through instance, physical-device and surface creation
        // inside eglMakeCurrent. So there is no in-process question the harness
        // can ask that is guaranteed to be survivable, and the old form (dlopen
        // the Vulkan loader, count physical devices, look for
        // VK_EXT_headless_surface) was a guess at the abort conditions rather
        // than a test of them: it named three of the ways bring-up can die and
        // was silent about every other one, including every DirectGLES one.
        //
        // What is actually predictive is to run the bring-up itself somewhere a
        // SIGTRAP is a datum instead of a crash. fork() gives exactly that: the
        // child performs the identical sequence and _exit(0)s on success, and
        // ANY non-zero exit or ANY signal in the parent's waitpid() means "this
        // platform is unusable" - whatever the reason, including reasons nobody
        // has thought of. Only then does the parent do the real bring-up.
        //
        // Returns an empty string when the platform survived a full bring-up.
        std::string PreflightBringUp() {
#if !MGITEST_HAVE_FORK_PREFLIGHT
            // No fork(): let the in-process bring-up speak for itself, which is
            // what this module did before. Windows/macOS are not CI targets for
            // the headless scenarios.
            return {};
#else
            int channel[2] = {-1, -1};
            if (pipe(channel) != 0) {
                return {}; // cannot pre-flight; fall through to the in-process attempt
            }
            // The child inherits our stdio buffers; flush so nothing is printed twice.
            std::fflush(nullptr);
            const pid_t child = fork();
            if (child < 0) {
                close(channel[0]);
                close(channel[1]);
                return {};
            }
            if (child == 0) {
                close(channel[0]);
                // No core suppression here, deliberately: when the child dies on a
                // signal, the core IS the diagnosis (an rlimit that used to sit here
                // made a CI-only crash undebuggable). Machines that do not want
                // cores control that with the usual ulimit/core_pattern knobs.
                std::fprintf(stderr, "[itest] pre-flight child: attempting a full EGL bring-up\n");
                EglBringUp local;
                std::string reason;
                const int step = RunEglBringUp(local, reason);
                if (!reason.empty()) {
                    const std::size_t bytes = std::min<std::size_t>(reason.size(), 480);
                    const ssize_t written = write(channel[1], reason.data(), bytes);
                    (void)written;
                }
                close(channel[1]);
                // _exit, never exit(): every atexit handler and static destructor
                // in this address space belongs to the parent's copy of the world,
                // and the child is holding a live context it must not tear down.
                _exit(step);
            }

            close(channel[1]);
            // Reap first, read after: the message is bounded well below the pipe
            // buffer so the child can never block writing it, and polling the exit
            // status is what lets a wedged child be killed instead of hanging the
            // parent on a read that will never return.
            constexpr int kPreflightTimeoutMs = 30000;
            int status = 0;
            int waitedMs = 0;
            for (;;) {
                const pid_t reaped = waitpid(child, &status, WNOHANG);
                if (reaped == child) break;
                if (reaped < 0) {
                    close(channel[0]);
                    return "waitpid on the EGL bring-up pre-flight child failed";
                }
                if (waitedMs >= kPreflightTimeoutMs) {
                    kill(child, SIGKILL);
                    (void)waitpid(child, &status, 0);
                    close(channel[0]);
                    std::ostringstream out;
                    out << "the EGL bring-up wedged: a forked pre-flight child made no progress in "
                        << kPreflightTimeoutMs / 1000 << "s and was killed";
                    return out.str();
                }
                timespec nap{0, 10 * 1000 * 1000};
                nanosleep(&nap, nullptr);
                waitedMs += 10;
            }

            std::string childSays;
            char buffer[512];
            for (;;) {
                const ssize_t got = read(channel[0], buffer, sizeof(buffer));
                if (got <= 0) break;
                childSays.append(buffer, static_cast<std::size_t>(got));
            }
            close(channel[0]);

            if (WIFSIGNALED(status)) {
                const int signalNumber = WTERMSIG(status);
                const char* signalName = strsignal(signalNumber);
                std::ostringstream out;
                out << "the EGL bring-up ABORTS on this platform: a forked pre-flight child died on signal "
                    << signalNumber << " (" << (signalName != nullptr ? signalName : "?") << ")";
                if (!childSays.empty()) out << " after: " << childSays;
                out << ". MobileGL asserts rather than returning an error here, so the scenarios would "
                       "have taken the whole test binary down with them";
                return out.str();
            }
            if (!WIFEXITED(status)) {
                return "the EGL bring-up pre-flight child neither exited nor was signalled";
            }
            const int exitStatus = WEXITSTATUS(status);
            if (exitStatus != 0) {
                std::ostringstream out;
                out << (childSays.empty() ? "the EGL bring-up failed" : childSays)
                    << " (forked pre-flight child exit status " << exitStatus << ")";
                return out.str();
            }
            return {};
#endif
        }
    } // namespace

    namespace {
        bool EnvFlag(const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
        }
    } // namespace

    bool RequireGpu() {
        return EnvFlag("MOBILEGL_ITEST_REQUIRE_GPU");
    }

    bool RequireHardwareGpu() {
        return EnvFlag("MOBILEGL_ITEST_REQUIRE_HARDWARE_GPU");
    }

    std::ostream& operator<<(std::ostream& os, const Rgba8& c) {
        os << "rgba(" << int(c.r) << "," << int(c.g) << "," << int(c.b) << "," << int(c.a) << ")";
        return os;
    }

    Rgba8 Image::At(int x, int y) const {
        if (x < 0 || y < 0 || x >= m_width || y >= m_height) {
            return Rgba8{};
        }
        const std::size_t index = (static_cast<std::size_t>(y) * m_width + x) * 4;
        return Rgba8{m_pixels[index], m_pixels[index + 1], m_pixels[index + 2], m_pixels[index + 3]};
    }

    const char* Image::ColorName(int x, int y) const {
        const Rgba8 c = At(x, y);
        const bool r = c.r > 160, g = c.g > 160, b = c.b > 160;
        const bool nr = c.r < 96, ng = c.g < 96, nb = c.b < 96;
        if (nr && ng && nb) return "black";
        if (r && g && b) return "white";
        if (r && ng && nb) return "red";
        if (nr && g && nb) return "green";
        if (nr && ng && b) return "blue";
        if (r && g && nb) return "yellow";
        return "other";
    }

    std::size_t Image::ByteDiffCount(const Image& other) const {
        if (m_width != other.m_width || m_height != other.m_height) {
            return std::max(m_pixels.size(), other.m_pixels.size());
        }
        std::size_t differing = 0;
        for (std::size_t i = 0; i < m_pixels.size(); ++i) {
            if (m_pixels[i] != other.m_pixels[i]) ++differing;
        }
        return differing;
    }

    std::string Image::QuadrantSignature() const {
        if (m_width < 2 || m_height < 2) return "<empty>";
        // Quadrant CENTRES, so a one-pixel rounding difference at a quadrant edge
        // never decides the answer. Order is fixed and load-bearing: bottom-left,
        // bottom-right, top-left, top-right.
        const int leftX = m_width / 4;
        const int rightX = m_width * 3 / 4;
        const int bottomY = m_height / 4;
        const int topY = m_height * 3 / 4;
        std::ostringstream out;
        out << ColorName(leftX, bottomY) << "," << ColorName(rightX, bottomY) << "," << ColorName(leftX, topY) << ","
            << ColorName(rightX, topY);
        return out.str();
    }

    RegionScan ScanRegion(const Image& image, int x0, int x1, int y0, int y1, const char* expectedColor) {
        RegionScan scan;
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, image.Width() - 1);
        y1 = std::min(y1, image.Height() - 1);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                ++scan.total;
                const char* name = image.ColorName(x, y);
                if (std::strcmp(name, expectedColor) == 0) continue;
                ++scan.offenders;
                if (scan.firstX < 0) {
                    scan.firstX = x;
                    scan.firstY = y;
                    scan.firstColor = image.At(x, y);
                    scan.firstColorName = name;
                }
            }
        }
        return scan;
    }

    ::testing::AssertionResult RegionIsMostly(const Image& image, int x0, int x1, int y0, int y1,
                                              const char* expectedColor, double tolerance,
                                              const std::string& when) {
        const RegionScan scan = ScanRegion(image, x0, x1, y0, y1, expectedColor);
        if (scan.total == 0) {
            return ::testing::AssertionFailure()
                   << when << ": region x[" << x0 << "," << x1 << "] y[" << y0 << "," << y1
                   << "] is empty against a " << image.Width() << "x" << image.Height() << " readback";
        }
        const double offendingFraction = static_cast<double>(scan.offenders) / scan.total;
        if (offendingFraction <= tolerance) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure()
               << when << ": region x[" << x0 << "," << x1 << "] y[" << y0 << "," << y1 << "] should be all "
               << expectedColor << ", but " << scan.offenders << " of " << scan.total << " pixels ("
               << static_cast<int>(offendingFraction * 100.0 + 0.5) << "%) are not; first offender at (" << scan.firstX
               << "," << scan.firstY << ") is " << scan.firstColorName << " " << scan.firstColor;
    }

    HeadlessGL& HeadlessGL::Get() {
        static HeadlessGL instance;
        return instance;
    }

    HeadlessGL::HeadlessGL() {
        // Before anything else in this process can reach EGL, and in particular
        // before the pre-flight forks - the child must measure the same platform
        // the parent will use.
        EnsureHeadlessPlatform();
        m_backendName = EnvOr("MOBILEGL_BACKEND_TYPE", "<unset>");
        m_usable = BringUp();
    }

    bool HeadlessGL::BringUp() {
        // Ask a disposable copy of this process first. Only if it survived does
        // the real one try - see PreflightBringUp for why nothing weaker is
        // predictive against a stack that aborts instead of returning errors.
        const std::string preflightProblem = PreflightBringUp();
        if (!preflightProblem.empty()) {
            m_skipReason = preflightProblem;
            return false;
        }

        // Same shape as DriverBench's boot_egl(), minus the dlopen: the provider
        // is this binary. A pbuffer needs no window system, but MobileGL's own
        // loader still has to reach a real driver underneath - and the child
        // above just proved it can.
        EglBringUp brought;
        std::string reason;
        if (RunEglBringUp(brought, reason) != 0) {
            // The pre-flight passed and the parent's identical attempt did not.
            // That is a real result, not a machine without a GPU, so say so: it
            // means something is different between the two attempts (a leaked
            // exclusive device, an environment the child did not have).
            m_skipReason = reason + " - although an identical bring-up in a forked pre-flight child succeeded";
            return false;
        }

        m_display = brought.display;
        m_surface = brought.surface;
        m_context = brought.context;
        m_width = kSurfaceWidth;
        m_height = kSurfaceHeight;
        m_renderer = std::move(brought.renderer);
        return true;
    }

    void HeadlessGL::EndFrame() {
        if (!m_usable) return;
        eglSwapBuffers(static_cast<EGLDisplay>(m_display), static_cast<EGLSurface>(m_surface));
        ++m_frameIndex;
    }

    void HeadlessGL::ShutDown() {
        if (!m_usable) return;
        EGLDisplay display = static_cast<EGLDisplay>(m_display);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_context != nullptr) eglDestroyContext(display, static_cast<EGLContext>(m_context));
        if (m_surface != nullptr) eglDestroySurface(display, static_cast<EGLSurface>(m_surface));
        eglTerminate(display);
        m_context = nullptr;
        m_surface = nullptr;
        m_display = nullptr;
        m_usable = false;
        m_skipReason = "the headless context has already been torn down";
    }

    // ---- scenario vocabulary ------------------------------------------------

    namespace {
        unsigned int CompileStage(GLenum stage, const char* source, std::string* outError) {
            const GLuint shader = glCreateShader(stage);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint compiled = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (compiled == GL_FALSE) {
                char log[2048] = {};
                GLsizei length = 0;
                glGetShaderInfoLog(shader, sizeof(log) - 1, &length, log);
                if (outError != nullptr) {
                    *outError = std::string(stage == GL_VERTEX_SHADER ? "vertex" : "fragment") +
                                " shader failed to compile: " + log;
                }
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        }
    } // namespace

    unsigned int CompileProgram(const char* vertexSource, const char* fragmentSource, std::string* outError) {
        const GLuint vs = CompileStage(GL_VERTEX_SHADER, vertexSource, outError);
        if (vs == 0) return 0;
        const GLuint fs = CompileStage(GL_FRAGMENT_SHADER, fragmentSource, outError);
        if (fs == 0) {
            glDeleteShader(vs);
            return 0;
        }
        const GLuint program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        // Pinned rather than queried so the scenarios can set up a VAO without a
        // round trip, and so a driver that reorders attributes cannot change what
        // the test means.
        glBindAttribLocation(program, 0, "aPos");
        glBindAttribLocation(program, 1, "aColor");
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked == GL_FALSE) {
            char log[2048] = {};
            GLsizei length = 0;
            glGetProgramInfoLog(program, sizeof(log) - 1, &length, log);
            if (outError != nullptr) *outError = std::string("program failed to link: ") + log;
            glDeleteProgram(program);
            return 0;
        }
        return program;
    }

    ColorFbo MakeColorFbo(int width, int height) {
        ColorFbo target;
        target.width = width;
        target.height = height;
        glGenTextures(1, &target.texture);
        glBindTexture(GL_TEXTURE_2D, target.texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        glGenFramebuffers(1, &target.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            DestroyColorFbo(target);
        }
        return target;
    }

    void DestroyColorFbo(ColorFbo& target) {
        if (target.fbo != 0) glDeleteFramebuffers(1, &target.fbo);
        if (target.texture != 0) glDeleteTextures(1, &target.texture);
        target.fbo = 0;
        target.texture = 0;
    }

    void BindDefaultFramebuffer() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, HeadlessGL::Get().Width(), HeadlessGL::Get().Height());
    }

    void BindFbo(const ColorFbo& target) {
        glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
        glViewport(0, 0, target.width, target.height);
    }

    void ClearTo(float r, float g, float b, float a) {
        glClearColor(r, g, b, a);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    Image ReadPixels(int width, int height) {
        Image image(width, height);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, image.Data());
        return image;
    }

    unsigned int FirstGLError() {
        const GLenum first = glGetError();
        if (first == GL_NO_ERROR) return GL_NO_ERROR;
        // Drain, bounded: a broken stack must not turn an error check into a hang.
        for (int i = 0; i < 64 && glGetError() != GL_NO_ERROR; ++i) {}
        return first;
    }

    const char* GLErrorName(unsigned int error) {
        switch (error) {
        case GL_NO_ERROR:
            return "GL_NO_ERROR";
        case GL_INVALID_ENUM:
            return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:
            return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION:
            return "GL_INVALID_OPERATION";
        case GL_OUT_OF_MEMORY:
            return "GL_OUT_OF_MEMORY";
        case GL_INVALID_FRAMEBUFFER_OPERATION:
            return "GL_INVALID_FRAMEBUFFER_OPERATION";
        default:
            return "GL_<unknown>";
        }
    }

} // namespace MGITest
