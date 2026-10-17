// MobileGL - MobileGL/MG_Test/Program/ShaderPrewarmTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// MOBILEGL_DISABLE_SHADER_PREWARM. Initialize() builds glslang's built-in tables ahead of the
// first compile unless the variable is truthy; the CI test lanes set it, so these cases set or
// clear it themselves. MG_ConfigLoader::Init() rereads the environment on every Initialize(),
// so the variable, not the feature block, is the input. Its own binary: every case calls
// Destroy(), and ShaderCompilePool's stop is a one-way latch (see AsyncTeardownTest).

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "Config.h"
#include "Includes.h"
#include "Init.h"
#include "MG_Impl/GLImpl/Getter/GL_Getter.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_Util/ShaderTranspiler/ShaderCompiler.h"

using namespace MobileGL;
using namespace MobileGL::MG_Impl::GLImpl;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;

namespace {
    constexpr const char* kPrewarmKnob = "MOBILEGL_DISABLE_SHADER_PREWARM";
    constexpr const char* kAsyncKnob = "MOBILEGL_ASYNC_SHADER_COMPILE";

    // Sets (or, with nullptr, clears) one environment variable for a scope.
    class ScopedEnv {
    public:
        ScopedEnv(const char* name, const char* value) : m_name(name) {
            if (const char* old = std::getenv(name)) {
                m_previous = old;
                m_had = true;
            }
            Apply(value);
        }
        ~ScopedEnv() { Apply(m_had ? m_previous.c_str() : nullptr); }
        ScopedEnv(const ScopedEnv&) = delete;
        ScopedEnv& operator=(const ScopedEnv&) = delete;

    private:
        void Apply(const char* value) const {
#if defined(_WIN32)
            _putenv_s(m_name.c_str(), value != nullptr ? value : "");
#else
            if (value != nullptr)
                setenv(m_name.c_str(), value, 1);
            else
                unsetenv(m_name.c_str());
#endif
        }

        std::string m_name, m_previous;
        bool m_had = false;
    };

    GLint CompileStatusOf(const GLenum type, const char* source) {
        const GLuint shader = CreateShader(type);
        ShaderSource(shader, 1, &source, nullptr);
        CompileShader(shader);
        GLint status = GL_FALSE;
        GetShaderiv(shader, GL_COMPILE_STATUS, &status);
        DeleteShader(shader);
        return status;
    }

    constexpr const char* kFragment460 =
        "#version 460\nlayout(location = 0) out vec4 c;\nvoid main() { c = vec4(1.0); }\n";
} // namespace

TEST(ShaderPrewarm, InitializePrewarmsByDefaultAndAgainAfterDestroy) {
    const ScopedEnv knob(kPrewarmKnob, nullptr);
    const ScopedEnv async(kAsyncKnob, "1");
    MobileGL::Initialize();
    EXPECT_FALSE(MG_Config::Features.DisableShaderPrewarm);
    EXPECT_TRUE(ShaderCompiler::BuiltinsPrewarmed());
    MobileGL::Destroy();
    // Destroy finalizes glslang, which frees the tables the latch stands for.
    EXPECT_FALSE(ShaderCompiler::BuiltinsPrewarmed());
    MobileGL::Initialize();
    EXPECT_TRUE(ShaderCompiler::BuiltinsPrewarmed());
    MobileGL::Destroy();
}

TEST(ShaderPrewarm, ATruthyKnobSkipsThePrewarmAndCompilesStillWork) {
    const ScopedEnv knob(kPrewarmKnob, "1");
    const ScopedEnv async(kAsyncKnob, "1");
    MobileGL::Initialize();
    EXPECT_TRUE(MG_Config::Features.DisableShaderPrewarm);
    EXPECT_FALSE(ShaderCompiler::BuiltinsPrewarmed());
    EXPECT_EQ(CompileStatusOf(GL_FRAGMENT_SHADER, kFragment460), GL_TRUE);
    EXPECT_FALSE(ShaderCompiler::BuiltinsPrewarmed());
    MobileGL::Destroy();
}

TEST(ShaderPrewarm, AFalsyKnobKeepsThePrewarm) {
    const ScopedEnv knob(kPrewarmKnob, "0");
    const ScopedEnv async(kAsyncKnob, "1");
    MobileGL::Initialize();
    EXPECT_FALSE(MG_Config::Features.DisableShaderPrewarm);
    EXPECT_TRUE(ShaderCompiler::BuiltinsPrewarmed());
    MobileGL::Destroy();
}
