// MobileGL - MobileGL/MG_Test/Program/ShaderPrewarmTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// MOBILEGL_DISABLE_SHADER_PREWARM. Initialize() builds glslang's built-in tables ahead of the
// first compile unless the variable is truthy, and logs which way it went; these cases read that
// line back. The CI test lanes set the variable, so every case sets or clears it itself:
// MG_ConfigLoader::Init() rereads the environment on every Initialize(). The log file is
// truncated by each Initialize() and closed by Destroy(), so each case reads it after Destroy().
// Its own binary: every case calls Destroy(), and ShaderCompilePool's stop is a one-way latch.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "Config.h"
#include "Includes.h"
#include "Init.h"
#include "MG_Impl/GLImpl/Getter/GL_Getter.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_Util/Debug/Log.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace MobileGL;
using namespace MobileGL::MG_Impl::GLImpl;

namespace {
    constexpr const char* kPrewarmKnob = "MOBILEGL_DISABLE_SHADER_PREWARM";
    constexpr const char* kAsyncKnob = "MOBILEGL_ASYNC_SHADER_COMPILE";
    constexpr const char* kPrewarmedLine = "prewarmed the glslang built-in tables";
    constexpr const char* kSkippedLine = "built-in table prewarm skipped";

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

    int ProcessId() {
#if defined(_WIN32)
        return _getpid();
#else
        return static_cast<int>(getpid());
#endif
    }

    // Points the library's log at a private file for the case; Log() reads what the client role
    // wrote, by the library's own naming rule.
    class PrivateLog {
    public:
        PrivateLog()
            : m_base((std::filesystem::temp_directory_path() /
                      ("mobilegl-shaderprewarm-test-" + std::to_string(ProcessId()) + ".log"))
                         .string()),
              m_env("MOBILEGL_LOG_FILE_PATH", m_base.c_str()) {}
        ~PrivateLog() {
            std::error_code ignored;
            std::filesystem::remove(Path(), ignored);
            std::filesystem::remove(m_base, ignored);
        }
        PrivateLog(const PrivateLog&) = delete;
        PrivateLog& operator=(const PrivateLog&) = delete;

        std::string Path() const {
            return MG_Util::Debug::RoleLogPath(m_base.c_str(), MG_Util::Debug::LogRole::Client);
        }

        std::string Log() const {
            std::ifstream in(Path());
            std::stringstream text;
            text << in.rdbuf();
            return text.str();
        }

    private:
        std::string m_base;
        ScopedEnv m_env;
    };

    int Count(const std::string& text, const char* line) {
        int count = 0;
        for (auto at = text.find(line); at != std::string::npos; at = text.find(line, at + 1))
            ++count;
        return count;
    }

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
    const PrivateLog log;
    const ScopedEnv knob(kPrewarmKnob, nullptr);
    const ScopedEnv async(kAsyncKnob, "1");
    MobileGL::Initialize();
    MobileGL::Destroy();
    EXPECT_EQ(Count(log.Log(), kPrewarmedLine), 1) << log.Log();
    // Destroy finalizes glslang, which frees the tables, so the next Initialize warms again.
    MobileGL::Initialize();
    MobileGL::Destroy();
    EXPECT_EQ(Count(log.Log(), kPrewarmedLine), 1) << log.Log();
}

TEST(ShaderPrewarm, ATruthyKnobSkipsThePrewarmAndCompilesStillWork) {
    const PrivateLog log;
    const ScopedEnv knob(kPrewarmKnob, "1");
    const ScopedEnv async(kAsyncKnob, "1");
    MobileGL::Initialize();
    EXPECT_EQ(CompileStatusOf(GL_FRAGMENT_SHADER, kFragment460), GL_TRUE);
    MobileGL::Destroy();
    const std::string text = log.Log();
    EXPECT_EQ(Count(text, kPrewarmedLine), 0) << text;
    EXPECT_EQ(Count(text, kSkippedLine), 1) << text;
}

TEST(ShaderPrewarm, AFalsyKnobKeepsThePrewarm) {
    const PrivateLog log;
    const ScopedEnv knob(kPrewarmKnob, "0");
    const ScopedEnv async(kAsyncKnob, "1");
    MobileGL::Initialize();
    MobileGL::Destroy();
    const std::string text = log.Log();
    EXPECT_EQ(Count(text, kPrewarmedLine), 1) << text;
    EXPECT_EQ(Count(text, kSkippedLine), 0) << text;
}
