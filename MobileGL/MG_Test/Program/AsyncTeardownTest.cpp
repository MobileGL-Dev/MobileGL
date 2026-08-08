// MobileGL - MobileGL/MG_Test/Program/AsyncTeardownTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P1 stage 4, item S6: MobileGL::Destroy() with compile AND link jobs still in flight.
//
// This is the one cancellation path in the whole design that WAITS, and the order it waits
// in is load-bearing: in-flight jobs own their own inputs and are safe against everything
// teardown does EXCEPT glslang's process globals and the TShader/TProgram objects hanging off
// pGLContext - both of which DestroyImpl is about to free. StopAndDrain() therefore runs
// first, before pGLContext.reset() and before glslang::FinalizeProcess().
//
// ITS OWN BINARY, deliberately. ShaderCompilePool::StopAndDrain() is a one-way latch: from
// the first eglTerminate onwards every job in the process runs inline on the calling thread.
// Sharing a binary with AsyncCompileTest/AsyncLinkTest would silently turn every case
// declared after this one synchronous, and they would keep passing while testing nothing.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Config.h"
#include "Includes.h"
#include "Init.h"
#include "MG_Impl/GLImpl/Getter/GL_Getter.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_State/GLState/Core.h"
#include "MG_Util/Async/ShaderCompilePool.h"

using namespace MobileGL;
using namespace MobileGL::MG_Impl::GLImpl;

namespace {
    const char* kVs = R"(#version 460
layout(location = 0) in vec3 aPos;
uniform vec4 uColor;
out vec4 vColor;
void main() {
    vColor = uColor;
    gl_Position = vec4(aPos, 1.0);
}
)";

    String MakeBulkySource(const int index) {
        String source = "#version 460\nlayout(location = 0) out vec4 fragColor;\n";
        source += "uniform float uSeed" + std::to_string(index) + ";\n";
        source += "void main() {\n    float acc = uSeed" + std::to_string(index) + ";\n";
        for (int i = 0; i < 220; ++i) {
            source += "    acc = acc * 1.0001 + sin(acc + " + std::to_string(i) + ".0) * cos(acc);\n";
        }
        source += "    fragColor = vec4(acc, acc, acc, 1.0);\n}\n";
        return source;
    }

    GLuint MakeShader(const GLenum type, const char* source) {
        const GLuint shader = CreateShader(type);
        ShaderSource(shader, 1, &source, nullptr);
        CompileShader(shader);
        return shader;
    }
} // namespace

// Fills the pool with compiles, chains links behind them, and tears the library down without
// reading a single result. Nothing here can assert on the jobs' outcomes - by design there is
// no one left to ask - so what it asserts is that teardown COMPLETES: it must not hang
// (StopAndDrain joining a worker that is itself waiting on something), must not crash (a
// worker inside glslang while FinalizeProcess frees its symbol tables, or a link job reading
// a shader node the GL thread has dropped), and must leave the process able to come back up.
TEST(AsyncTeardown, DestroyWithCompilesAndLinksInFlight) {
    // After Initialize(), not before: MG_ConfigLoader::Init() re-reads the whole feature
    // block from the environment and would overwrite the override.
    MobileGL::Initialize();
    MG_Config::Features.AsyncShaderCompile = MG_Config::QuirkOverride::ForceOn;
    ASSERT_TRUE(MG_Util::Async::AsyncShaderCompileEnabled());

    constexpr int kCount = 64;
    Vector<String> sources;
    Vector<GLuint> shaders;
    Vector<GLuint> programs;
    sources.reserve(kCount);

    // Bare compiles first, so the pool has a backlog the links below will queue behind.
    for (int i = 0; i < kCount; ++i) {
        sources.push_back(MakeBulkySource(30000 + i));
        const char* text = sources.back().c_str();
        const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &text, nullptr);
        CompileShader(fs);
        shaders.push_back(fs);
    }

    // Then links, each chained behind a compile that is very probably still outstanding: at
    // the moment Destroy() runs there are queued compiles, running compiles, links waiting on
    // a dependency edge, and links already handed to the pool.
    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    for (int i = 0; i < kCount; ++i) {
        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, shaders[static_cast<SizeT>(i)]);
        LinkProgram(program);
        programs.push_back(program);
    }

    // No status read anywhere above - the jobs are genuinely in flight.
    MobileGL::Destroy();

    // Back up again. The pool stays stopped for the rest of the process (a one-way latch), so
    // this second life is synchronous - which is exactly the documented behaviour, and it has
    // to still be a WORKING one.
    MobileGL::Initialize();
    const GLuint vs2 = MakeShader(GL_VERTEX_SHADER, kVs);
    const char* fsSource = R"(#version 460
in vec4 vColor;
layout(location = 0) out vec4 fragColor;
void main() { fragColor = vColor; }
)";
    const GLuint fs2 = MakeShader(GL_FRAGMENT_SHADER, fsSource);
    const GLuint program = CreateProgram();
    AttachShader(program, vs2);
    AttachShader(program, fs2);
    LinkProgram(program);

    GLint status = GL_FALSE;
    GetProgramiv(program, GL_LINK_STATUS, &status);
    EXPECT_EQ(status, GL_TRUE) << "the library must be usable after a teardown that drained jobs in flight";
    EXPECT_GE(GetUniformLocation(program, "uColor"), 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}
