// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/VertexArrayEnableDisableScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// KHR-GL45.direct_state_access.vertex_arrays_enable_disable_attributes, rebuilt.
//
// The case is small and does one unusual thing twice: it turns half of
// GL_MAX_VERTEX_ATTRIBS attribute arrays on and the other half off with
// glEnableVertexArrayAttrib / glDisableVertexArrayAttrib on a vertex array object
// that is NOT bound (it binds the default one first, on purpose), draws one point
// through a program that reads exactly the enabled half, and checks the sum those
// arrays produced. Then it swaps which half is enabled, draws again through a
// SECOND program, and checks the other sum.
//
// Both draws capture into ONE four-byte transform feedback buffer, allocated once
// with immutable storage and read back with glMapBuffer - so anything that only
// works on the first capture span through a buffer fails the second check while
// leaving the first one green.
//
// It is reassembled here rather than shortened because every one of those details
// is a candidate: the unbound-VAO enables, the two-program swap, the integer
// attributes fetched with glVertexAttribIPointer at a stride wider than one
// element, the second capture span, and the fact that the sums differ ONLY in
// which arrays contributed (a fetch that ignored the enable state, or one that
// read the wrong element, lands on a different number, not on garbage).

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/ScenarioFixture.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {

        GLuint CompileShader(GLenum type, const std::string& source, std::string* log) {
            const GLuint shader = glCreateShader(type);
            const char* text = source.c_str();
            glShaderSource(shader, 1, &text, nullptr);
            glCompileShader(shader);
            GLint status = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            if (status == GL_FALSE) {
                GLint length = 0;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> buffer(static_cast<std::size_t>(length) + 1, '\0');
                glGetShaderInfoLog(shader, length + 1, nullptr, buffer.data());
                if (log != nullptr) *log = buffer.data();
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        }

        // Declares and sums the even (parity 0) or odd (parity 1) attributes only, with the
        // locations assigned by glBindAttribLocation rather than a layout qualifier - which is
        // what the CTS case does, and which makes the attribute set the program reads a link
        // property rather than a source one.
        GLuint BuildSumProgram(int parity, int attributeCount, std::string* log) {
            std::string declarations;
            std::string copies = "  sum = 0;\n";
            for (int i = parity; i < attributeCount; i += 2) {
                declarations += "in int a_" + std::to_string(i) + ";\n";
                copies += "  sum += a_" + std::to_string(i) + ";\n";
            }
            const std::string vertexSource = "#version 450\n\n" + declarations + "out int sum;\n\nvoid main()\n{\n" +
                                             copies + "}\n";
            const std::string fragmentSource = R"(#version 450

out vec4 color;

void main()
{
    color = vec4(1.0);
}
)";
            const GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexSource, log);
            if (vertexShader == 0) return 0;
            const GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentSource, log);
            if (fragmentShader == 0) {
                glDeleteShader(vertexShader);
                return 0;
            }

            const GLuint program = glCreateProgram();
            glAttachShader(program, vertexShader);
            glAttachShader(program, fragmentShader);
            const char* varying = "sum";
            glTransformFeedbackVaryings(program, 1, &varying, GL_INTERLEAVED_ATTRIBS);
            for (int i = parity; i < attributeCount; i += 2) {
                const std::string name = "a_" + std::to_string(i);
                glBindAttribLocation(program, static_cast<GLuint>(i), name.c_str());
            }
            glLinkProgram(program);
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);

            GLint status = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &status);
            if (status == GL_FALSE) {
                GLint length = 0;
                glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> buffer(static_cast<std::size_t>(length) + 1, '\0');
                glGetProgramInfoLog(program, length + 1, nullptr, buffer.data());
                if (log != nullptr) *log = buffer.data();
                glDeleteProgram(program);
                return 0;
            }
            return program;
        }

        class VertexArrayEnableDisableScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &m_attributeCount);
                ASSERT_GE(m_attributeCount, 16);

                std::string log;
                m_even = BuildSumProgram(0, m_attributeCount, &log);
                ASSERT_NE(m_even, 0u) << "even program failed to build: " << log;
                m_odd = BuildSumProgram(1, m_attributeCount, &log);
                ASSERT_NE(m_odd, 0u) << "odd program failed to build: " << log;

                // One element per attribute, read as one vertex whose stride spans them all.
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                std::vector<GLint> reference(static_cast<std::size_t>(m_attributeCount));
                for (int i = 0; i < m_attributeCount; ++i) reference[static_cast<std::size_t>(i)] = i;
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(reference.size() * sizeof(GLint)),
                             reference.data(), GL_STATIC_DRAW);
                for (int i = 0; i < m_attributeCount; ++i) {
                    glVertexAttribIPointer(static_cast<GLuint>(i), 1, GL_INT,
                                           static_cast<GLsizei>(sizeof(GLint) * m_attributeCount),
                                           reinterpret_cast<const void*>(static_cast<std::size_t>(i) * sizeof(GLint)));
                }
                glBindBuffer(GL_ARRAY_BUFFER, 0);

                // Immutable storage, allocated once, read back with glMapBuffer - the capture
                // buffer is never respecified between the two spans.
                glGenBuffers(1, &m_xfb);
                glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, m_xfb);
                glBufferStorage(GL_TRANSFORM_FEEDBACK_BUFFER, sizeof(GLint), nullptr, GL_MAP_READ_BIT);
                glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, m_xfb);
                ASSERT_EQ(glGetError(), GL_NO_ERROR) << "capture buffer setup";
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindVertexArray(0);
                if (m_xfb != 0) glDeleteBuffers(1, &m_xfb);
                if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_even != 0) glDeleteProgram(m_even);
                if (m_odd != 0) glDeleteProgram(m_odd);
                ScenarioTest::TearDown();
            }

            // Enables one parity's arrays and disables the other's, THROUGH THE OBJECT NAME
            // while a different vertex array object is bound.
            void TurnOnAttributes(int enabledParity) {
                glBindVertexArray(0);
                for (int i = 0; i < m_attributeCount; ++i) {
                    if (i % 2 == enabledParity % 2) {
                        glEnableVertexArrayAttrib(m_vao, static_cast<GLuint>(i));
                    } else {
                        glDisableVertexArrayAttrib(m_vao, static_cast<GLuint>(i));
                    }
                    ASSERT_EQ(glGetError(), GL_NO_ERROR) << "attribute " << i << ", parity " << enabledParity;
                }
                glBindVertexArray(m_vao);
            }

            int ExpectedSum(int parity) const {
                int sum = 0;
                for (int i = parity; i < m_attributeCount; i += 2) sum += i;
                return sum;
            }

            // One capture span, read back the way the CTS case does.
            int DrawAndRead(int parity) {
                glUseProgram(parity == 0 ? m_even : m_odd);
                glBindVertexArray(m_vao);
                glBeginTransformFeedback(GL_POINTS);
                glDrawArrays(GL_POINTS, 0, 1);
                glEndTransformFeedback();

                const void* mapped = glMapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, GL_READ_ONLY);
                if (mapped == nullptr) {
                    ADD_FAILURE() << "glMapBuffer returned null for parity " << parity;
                    return -1;
                }
                GLint result = -1;
                std::memcpy(&result, mapped, sizeof(result));
                glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
                return result;
            }

            GLint m_attributeCount = 16;
            GLuint m_even = 0;
            GLuint m_odd = 0;
            GLuint m_vao = 0;
            GLuint m_vbo = 0;
            GLuint m_xfb = 0;
        };

        // The case verbatim: even half on, draw, check; odd half on, draw, check.
        TEST_F(VertexArrayEnableDisableScenario, EitherHalfOfTheAttributesInTurn) {
            if (!Ready()) GTEST_SKIP();

            TurnOnAttributes(0);
            EXPECT_EQ(DrawAndRead(0), ExpectedSum(0)) << "even attributes";

            TurnOnAttributes(1);
            EXPECT_EQ(DrawAndRead(1), ExpectedSum(1)) << "odd attributes";

            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

        // The first span on its own, so a failure of the case above can be read as "the second
        // span" rather than "the enables".
        TEST_F(VertexArrayEnableDisableScenario, TheEvenHalfAlone) {
            if (!Ready()) GTEST_SKIP();

            TurnOnAttributes(0);
            EXPECT_EQ(DrawAndRead(0), ExpectedSum(0));
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

        // And the odd half as the FIRST span, which separates "the odd program/arrays are
        // wrong" from "the second span is wrong".
        TEST_F(VertexArrayEnableDisableScenario, TheOddHalfAlone) {
            if (!Ready()) GTEST_SKIP();

            TurnOnAttributes(1);
            EXPECT_EQ(DrawAndRead(1), ExpectedSum(1));
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

    } // namespace
} // namespace MGITest
