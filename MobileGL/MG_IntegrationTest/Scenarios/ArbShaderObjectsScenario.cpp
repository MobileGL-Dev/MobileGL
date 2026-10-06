// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ArbShaderObjectsScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - GL_ARB_shader_objects, which MobileGL advertises (KWin asks for it) and whose entry
// points were stubs until P13: glCreateShaderObjectARB / glCreateProgramObjectARB returned 1.
//
// WHAT IS PINNED:
//   * an ARB handle is a name of the core's ONE shared shader/program name space (GL 4.6 §7.1): it
//     never collides with a core-created name, and core and ARB calls take each other's names;
//   * a program built entirely through ARB calls - mixed with core calls on the same objects -
//     links, is made current, takes a uniform and draws the right pixels;
//   * the extension's own queries: GetHandleARB(PROGRAM_OBJECT_ARB) is the current program,
//     GetObjectParameter*ARB answers OBJECT_TYPE / OBJECT_SUBTYPE and the core status pnames per
//     kind, GetInfoLogARB / GetAttachedObjectsARB read either kind, and DeleteObjectARB deletes
//     either kind with the core's deferred-deletion rules;
//   * the errors the extension names (INVALID_ENUM / INVALID_OPERATION / INVALID_VALUE);
//   * every advertised entry point resolves through eglGetProcAddress (glvnd and libepoxy find
//     them that way, not by symbol);
//   * and, because shader/program names share one space while every glGen* family has its own,
//     the per-type glGen* name spaces: a generated name is reserved but is no object until its
//     first bind (where the spec says so), name 0 is never handed out, and a deleted name is
//     free again.
//
// ARMS: both backends, monolith and every split arm (the record arm carries shader and program
// objects by {slot, gen} whichever entry point created them).

#include <cstdint>
#include <set>
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

#include <EGL/egl.h>

// The extension's own tokens, spelled out: glcorearb.h has none of them.
#define MGITEST_PROGRAM_OBJECT_ARB 0x8B40
#define MGITEST_SHADER_OBJECT_ARB 0x8B48
#define MGITEST_OBJECT_TYPE_ARB 0x8B4E
#define MGITEST_OBJECT_SUBTYPE_ARB 0x8B4F

// GLhandleARB is GLuint everywhere but Apple, which this module does not build for. Resolved by
// the linker straight into MobileGL, like every other gl* call here.
extern "C" {
GLuint glCreateShaderObjectARB(GLenum shaderType);
GLuint glCreateProgramObjectARB(void);
void glDeleteObjectARB(GLuint obj);
GLuint glGetHandleARB(GLenum pname);
void glDetachObjectARB(GLuint containerObj, GLuint attachedObj);
void glAttachObjectARB(GLuint containerObj, GLuint obj);
void glShaderSourceARB(GLuint shaderObj, GLsizei count, const char** string, const GLint* length);
void glCompileShaderARB(GLuint shaderObj);
void glLinkProgramARB(GLuint programObj);
void glUseProgramObjectARB(GLuint programObj);
void glValidateProgramARB(GLuint programObj);
void glUniform4fARB(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
void glGetObjectParameterivARB(GLuint obj, GLenum pname, GLint* params);
void glGetObjectParameterfvARB(GLuint obj, GLenum pname, GLfloat* params);
void glGetInfoLogARB(GLuint obj, GLsizei maxLength, GLsizei* length, char* infoLog);
void glGetAttachedObjectsARB(GLuint containerObj, GLsizei maxCount, GLsizei* count, GLuint* obj);
GLint glGetUniformLocationARB(GLuint programObj, const char* name);
void glGetUniformfvARB(GLuint programObj, GLint location, GLfloat* params);
void glGetShaderSourceARB(GLuint obj, GLsizei maxLength, GLsizei* length, char* source);
}

namespace MGITest {
    namespace {

        // A full-viewport triangle from gl_VertexID: no attribute, so the draw depends on nothing
        // but the program and its uniform.
        constexpr const char* kVertexSource = R"(#version 330 core
void main() {
    vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";
        constexpr const char* kFragmentSource = R"(#version 330 core
uniform vec4 uColor;
out vec4 oColor;
void main() { oColor = uColor; }
)";

        constexpr int kEdge = 16;

        class ArbShaderObjectsScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_target = MakeColorFbo(kEdge, kEdge);
                ASSERT_NE(m_target.fbo, 0u) << "could not create the render target";
                glGenVertexArrays(1, &m_vao);
                FirstGLError();
            }
            void TearDown() override {
                if (Ready()) {
                    glUseProgram(0);
                    if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                    DestroyColorFbo(m_target);
                }
                ScenarioTest::TearDown();
            }

            // A shader built entirely through the extension, its compile status read both ways.
            GLuint ArbShader(GLenum type, const char* source) {
                const GLuint shader = glCreateShaderObjectARB(type);
                EXPECT_NE(shader, 0u);
                glShaderSourceARB(shader, 1, &source, nullptr);
                glCompileShaderARB(shader);
                GLint arbStatus = 0, coreStatus = 0;
                glGetObjectParameterivARB(shader, GL_COMPILE_STATUS, &arbStatus);
                glGetShaderiv(shader, GL_COMPILE_STATUS, &coreStatus);
                if (arbStatus != GL_TRUE) {
                    char log[1024] = {};
                    glGetInfoLogARB(shader, sizeof(log), nullptr, log);
                    ADD_FAILURE() << "ARB compile failed: " << log;
                }
                EXPECT_EQ(arbStatus, coreStatus) << "the ARB and core status queries disagree";
                return shader;
            }

            void DrawInto() {
                BindFbo(m_target);
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }

            ColorFbo m_target;
            GLuint m_vao = 0;
        };

        TEST_F(ArbShaderObjectsScenario, EveryEntryPointResolvesThroughGetProcAddress) {
            if (!Ready()) return;
            const char* names[] = {
                "glDeleteObjectARB", "glGetHandleARB", "glDetachObjectARB", "glCreateShaderObjectARB",
                "glShaderSourceARB", "glCompileShaderARB", "glCreateProgramObjectARB", "glAttachObjectARB",
                "glLinkProgramARB", "glUseProgramObjectARB", "glValidateProgramARB", "glUniform1fARB",
                "glUniform2fARB", "glUniform3fARB", "glUniform4fARB", "glUniform1iARB", "glUniform2iARB",
                "glUniform3iARB", "glUniform4iARB", "glUniform1fvARB", "glUniform2fvARB", "glUniform3fvARB",
                "glUniform4fvARB", "glUniform1ivARB", "glUniform2ivARB", "glUniform3ivARB", "glUniform4ivARB",
                "glUniformMatrix2fvARB", "glUniformMatrix3fvARB", "glUniformMatrix4fvARB",
                "glGetObjectParameterfvARB", "glGetObjectParameterivARB", "glGetInfoLogARB",
                "glGetAttachedObjectsARB", "glGetUniformLocationARB", "glGetActiveUniformARB",
                "glGetUniformfvARB", "glGetUniformivARB", "glGetShaderSourceARB"};
            for (const char* name : names) {
                EXPECT_NE(reinterpret_cast<void*>(eglGetProcAddress(name)), nullptr) << name << " does not resolve";
            }
            EXPECT_EQ(reinterpret_cast<void*>(eglGetProcAddress("glCreateShaderObjectARB")),
                      reinterpret_cast<void*>(&glCreateShaderObjectARB))
                << "eglGetProcAddress hands out a different glCreateShaderObjectARB than the library exports";
        }

        // One name space for shaders and programs, whichever entry point created them; core and
        // ARB calls take each other's names, and OBJECT_TYPE / OBJECT_SUBTYPE say which kind.
        TEST_F(ArbShaderObjectsScenario, ArbAndCoreHandlesShareOneNameSpace) {
            if (!Ready()) return;
            const GLuint coreShader = glCreateShader(GL_FRAGMENT_SHADER);
            const GLuint arbShader = glCreateShaderObjectARB(GL_VERTEX_SHADER);
            const GLuint coreProgram = glCreateProgram();
            const GLuint arbProgram = glCreateProgramObjectARB();
            ASSERT_EQ(FirstGLError(), 0u);
            const std::set<GLuint> names{coreShader, arbShader, coreProgram, arbProgram};
            EXPECT_EQ(names.size(), 4u) << "an ARB handle collided with a core-created name";
            EXPECT_EQ(names.count(0u), 0u) << "a create handed out name 0";

            EXPECT_EQ(glIsShader(arbShader), GL_TRUE);
            EXPECT_EQ(glIsProgram(arbProgram), GL_TRUE);
            EXPECT_EQ(glIsProgram(arbShader), GL_FALSE);
            EXPECT_EQ(glIsShader(arbProgram), GL_FALSE);
            GLint value = 0;
            glGetShaderiv(arbShader, GL_SHADER_TYPE, &value);
            EXPECT_EQ(value, GL_VERTEX_SHADER);
            glGetObjectParameterivARB(coreShader, MGITEST_OBJECT_SUBTYPE_ARB, &value);
            EXPECT_EQ(value, GL_FRAGMENT_SHADER);
            glGetObjectParameterivARB(coreShader, MGITEST_OBJECT_TYPE_ARB, &value);
            EXPECT_EQ(value, MGITEST_SHADER_OBJECT_ARB);
            glGetObjectParameterivARB(arbProgram, MGITEST_OBJECT_TYPE_ARB, &value);
            EXPECT_EQ(value, MGITEST_PROGRAM_OBJECT_ARB);
            GLfloat asFloat = 0.0f;
            glGetObjectParameterfvARB(arbShader, MGITEST_OBJECT_SUBTYPE_ARB, &asFloat);
            EXPECT_EQ(asFloat, static_cast<GLfloat>(GL_VERTEX_SHADER));
            EXPECT_EQ(FirstGLError(), 0u);

            // Attachments cross the two spellings too.
            glAttachShader(arbProgram, coreShader);
            glAttachObjectARB(coreProgram, arbShader);
            GLuint attached[4] = {};
            GLsizei count = 0;
            glGetAttachedObjectsARB(arbProgram, 4, &count, attached);
            ASSERT_EQ(count, 1);
            EXPECT_EQ(attached[0], coreShader);
            glGetAttachedShaders(coreProgram, 4, &count, attached);
            ASSERT_EQ(count, 1);
            EXPECT_EQ(attached[0], arbShader);
            glGetObjectParameterivARB(coreProgram, GL_ATTACHED_SHADERS, &value);
            EXPECT_EQ(value, 1);
            glDetachObjectARB(arbProgram, coreShader);
            glGetProgramiv(arbProgram, GL_ATTACHED_SHADERS, &value);
            EXPECT_EQ(value, 0);
            EXPECT_EQ(FirstGLError(), 0u);

            glDeleteObjectARB(coreShader);
            glDeleteShader(arbShader);   // attached to coreProgram: flagged, not yet gone
            glDeleteObjectARB(arbProgram);
            glDeleteProgram(coreProgram); // takes arbShader with it
            EXPECT_EQ(glIsShader(coreShader), GL_FALSE);
            EXPECT_EQ(glIsProgram(arbProgram), GL_FALSE);
            EXPECT_EQ(glIsProgram(coreProgram), GL_FALSE);
            EXPECT_EQ(glIsShader(arbShader), GL_FALSE);
            EXPECT_EQ(FirstGLError(), 0u);
        }

        // A program built through the extension, mixed with core calls on the same objects, draws.
        TEST_F(ArbShaderObjectsScenario, AnArbProgramMixedWithCoreCallsDraws) {
            if (!Ready()) return;
            const GLuint vs = ArbShader(GL_VERTEX_SHADER, kVertexSource);
            const GLuint fs = ArbShader(GL_FRAGMENT_SHADER, kFragmentSource);
            const GLuint program = glCreateProgramObjectARB();
            glAttachObjectARB(program, vs);
            glAttachShader(program, fs); // core call on an ARB handle
            glLinkProgramARB(program);
            GLint linked = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (linked != GL_TRUE) {
                char log[1024] = {};
                glGetInfoLogARB(program, sizeof(log), nullptr, log);
                FAIL() << "ARB link failed: " << log;
            }
            GLint arbLinked = 0;
            glGetObjectParameterivARB(program, GL_LINK_STATUS, &arbLinked);
            EXPECT_EQ(arbLinked, GL_TRUE);
            glValidateProgramARB(program);

            glUseProgramObjectARB(program);
            EXPECT_EQ(glGetHandleARB(MGITEST_PROGRAM_OBJECT_ARB), program);
            GLint current = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &current);
            EXPECT_EQ(static_cast<GLuint>(current), program);
            const GLint arbLocation = glGetUniformLocationARB(program, "uColor");
            EXPECT_EQ(arbLocation, glGetUniformLocation(program, "uColor"));
            ASSERT_GE(arbLocation, 0);
            glUniform4fARB(arbLocation, 0.0f, 1.0f, 0.0f, 1.0f);
            GLfloat readBack[4] = {};
            glGetUniformfvARB(program, arbLocation, readBack);
            EXPECT_EQ(readBack[1], 1.0f);
            ASSERT_EQ(FirstGLError(), 0u);
            DrawInto();
            const Image green = ReadPixels(kEdge, kEdge);
            EXPECT_TRUE(RegionIsMostly(green, 0, kEdge - 1, 0, kEdge - 1, "green", 0.0, "ARB program, ARB uniform"));

            // The same program through the core spelling of the uniform, then released by the core
            // call and observed through the ARB query.
            glUniform4f(glGetUniformLocation(program, "uColor"), 0.0f, 0.0f, 1.0f, 1.0f);
            DrawInto();
            const Image blue = ReadPixels(kEdge, kEdge);
            EXPECT_TRUE(RegionIsMostly(blue, 0, kEdge - 1, 0, kEdge - 1, "blue", 0.0, "core uniform on an ARB program"));
            glUseProgram(0);
            EXPECT_EQ(glGetHandleARB(MGITEST_PROGRAM_OBJECT_ARB), 0u);

            char source[512] = {};
            GLsizei sourceLength = 0;
            glGetShaderSourceARB(fs, sizeof(source), &sourceLength, source);
            EXPECT_EQ(std::string(source), std::string(kFragmentSource));
            glDeleteObjectARB(vs);
            glDeleteObjectARB(fs);
            glDeleteObjectARB(program);
            EXPECT_EQ(FirstGLError(), 0u);
        }

        // DeleteObjectARB follows the core's deferred deletion: a program in use and a shader still
        // attached are flagged (DELETE_STATUS) and stay objects until released.
        TEST_F(ArbShaderObjectsScenario, DeleteObjectArbDefersLikeTheCore) {
            if (!Ready()) return;
            const GLuint vs = ArbShader(GL_VERTEX_SHADER, kVertexSource);
            const GLuint fs = ArbShader(GL_FRAGMENT_SHADER, kFragmentSource);
            const GLuint program = glCreateProgramObjectARB();
            glAttachObjectARB(program, vs);
            glAttachObjectARB(program, fs);
            glLinkProgramARB(program);
            glUseProgramObjectARB(program);
            glDeleteObjectARB(program);
            glDeleteObjectARB(vs);
            GLint status = 0;
            EXPECT_EQ(glIsProgram(program), GL_TRUE) << "a program in use was deleted at once";
            glGetObjectParameterivARB(program, GL_DELETE_STATUS, &status);
            EXPECT_EQ(status, GL_TRUE);
            EXPECT_EQ(glIsShader(vs), GL_TRUE) << "an attached shader was deleted at once";
            glGetObjectParameterivARB(vs, GL_DELETE_STATUS, &status);
            EXPECT_EQ(status, GL_TRUE);
            EXPECT_EQ(FirstGLError(), 0u);
            glUseProgramObjectARB(0);
            EXPECT_EQ(glIsProgram(program), GL_FALSE) << "releasing the flagged program did not delete it";
            EXPECT_EQ(glIsShader(vs), GL_FALSE) << "deleting the program did not take its flagged shader";
            EXPECT_EQ(glIsShader(fs), GL_TRUE);
            glDeleteObjectARB(fs);
            EXPECT_EQ(glIsShader(fs), GL_FALSE);
            EXPECT_EQ(FirstGLError(), 0u);
        }

        TEST_F(ArbShaderObjectsScenario, TheExtensionsErrorsAreTheSpecs) {
            if (!Ready()) return;
            const GLuint shader = glCreateShaderObjectARB(GL_FRAGMENT_SHADER);
            const GLuint program = glCreateProgramObjectARB();
            GLint value = -7;
            EXPECT_EQ(glGetHandleARB(GL_SHADER_TYPE), 0u);
            EXPECT_EQ(FirstGLError(), static_cast<unsigned>(GL_INVALID_ENUM)) << "GetHandleARB(bad pname)";
            glGetObjectParameterivARB(program, GL_COMPILE_STATUS, &value);
            EXPECT_EQ(FirstGLError(), static_cast<unsigned>(GL_INVALID_OPERATION)) << "COMPILE_STATUS of a program";
            glGetObjectParameterivARB(shader, GL_LINK_STATUS, &value);
            EXPECT_EQ(FirstGLError(), static_cast<unsigned>(GL_INVALID_OPERATION)) << "LINK_STATUS of a shader";
            glGetObjectParameterivARB(program, MGITEST_OBJECT_SUBTYPE_ARB, &value);
            EXPECT_EQ(FirstGLError(), static_cast<unsigned>(GL_INVALID_OPERATION)) << "OBJECT_SUBTYPE of a program";
            glGetObjectParameterivARB(program, GL_TEXTURE_2D, &value);
            EXPECT_EQ(FirstGLError(), static_cast<unsigned>(GL_INVALID_ENUM)) << "an unknown pname";
            EXPECT_EQ(value, -7) << "a refused query wrote its output";
            const GLuint stranger = shader + program + 1000u;
            glGetObjectParameterivARB(stranger, MGITEST_OBJECT_TYPE_ARB, &value);
            EXPECT_EQ(FirstGLError(), static_cast<unsigned>(GL_INVALID_VALUE)) << "a name that is no object";
            glDeleteObjectARB(stranger);
            EXPECT_EQ(FirstGLError(), static_cast<unsigned>(GL_INVALID_VALUE)) << "DeleteObjectARB(no object)";
            glDeleteObjectARB(0);
            EXPECT_EQ(FirstGLError(), 0u) << "DeleteObjectARB(0) is ignored";
            char log[8] = {};
            glGetInfoLogARB(stranger, sizeof(log), nullptr, log);
            EXPECT_EQ(FirstGLError(), static_cast<unsigned>(GL_INVALID_VALUE)) << "GetInfoLogARB(no object)";
            glDeleteObjectARB(shader);
            glDeleteObjectARB(program);
            EXPECT_EQ(FirstGLError(), 0u);
        }

        // Names freed by DeleteObjectARB may come back from either create, and never while live.
        TEST_F(ArbShaderObjectsScenario, FreedNamesAreReusedWithoutCollision) {
            if (!Ready()) return;
            std::set<GLuint> live;
            std::vector<GLuint> freed;
            for (int round = 0; round < 4; ++round) {
                for (int i = 0; i < 8; ++i) {
                    const GLuint names[4] = {glCreateShaderObjectARB(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER),
                                             glCreateProgramObjectARB(), glCreateProgram()};
                    for (GLuint name : names) {
                        EXPECT_NE(name, 0u);
                        EXPECT_TRUE(live.insert(name).second) << "name " << name << " handed out while live";
                    }
                }
                // Free half of them, through both spellings.
                int k = 0;
                for (auto it = live.begin(); it != live.end();) {
                    if ((k++ & 1) == 0) {
                        if (k & 2) {
                            glDeleteObjectARB(*it);
                        } else if (glIsProgram(*it)) {
                            glDeleteProgram(*it);
                        } else {
                            glDeleteShader(*it);
                        }
                        freed.push_back(*it);
                        it = live.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (GLuint name : live) EXPECT_TRUE(glIsShader(name) || glIsProgram(name)) << name;
            for (GLuint name : live) glDeleteObjectARB(name);
            EXPECT_EQ(FirstGLError(), 0u);
        }

        // THE OTHER NAME SPACES, which the extension does not touch: each glGen* family has its own,
        // a generated name is reserved but is not an object until its first bind (samplers are the
        // exception - glGenSamplers creates them), name 0 is never generated, and a deleted name is
        // no object any more.
        TEST_F(ArbShaderObjectsScenario, GenNameSpacesAreSeparateAndBindCreates) {
            if (!Ready()) return;
            const GLuint shader = glCreateShader(GL_VERTEX_SHADER);
            GLuint buffer = 0, texture = 0, vao = 0, fbo = 0, rbo = 0, sampler = 0, query = 0, pipeline = 0, xfb = 0;
            glGenBuffers(1, &buffer);
            glGenTextures(1, &texture);
            glGenVertexArrays(1, &vao);
            glGenFramebuffers(1, &fbo);
            glGenRenderbuffers(1, &rbo);
            glGenSamplers(1, &sampler);
            glGenQueries(1, &query);
            glGenProgramPipelines(1, &pipeline);
            glGenTransformFeedbacks(1, &xfb);
            ASSERT_EQ(FirstGLError(), 0u);
            for (GLuint name : {buffer, texture, vao, fbo, rbo, sampler, query, pipeline, xfb}) EXPECT_NE(name, 0u);
            // Reserved, not yet objects.
            EXPECT_EQ(glIsBuffer(buffer), GL_FALSE);
            EXPECT_EQ(glIsTexture(texture), GL_FALSE);
            EXPECT_EQ(glIsVertexArray(vao), GL_FALSE);
            EXPECT_EQ(glIsFramebuffer(fbo), GL_FALSE);
            EXPECT_EQ(glIsRenderbuffer(rbo), GL_FALSE);
            EXPECT_EQ(glIsSampler(sampler), GL_TRUE) << "glGenSamplers creates its objects";
            EXPECT_EQ(glIsQuery(query), GL_FALSE);
            EXPECT_EQ(glIsProgramPipeline(pipeline), GL_FALSE);
            EXPECT_EQ(glIsTransformFeedback(xfb), GL_FALSE);
            // The first bind creates.
            glBindBuffer(GL_ARRAY_BUFFER, buffer);
            glBindTexture(GL_TEXTURE_2D, texture);
            glBindVertexArray(vao);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glBindRenderbuffer(GL_RENDERBUFFER, rbo);
            glBindProgramPipeline(pipeline);
            glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, xfb);
            glBeginQuery(GL_SAMPLES_PASSED, query);
            glEndQuery(GL_SAMPLES_PASSED);
            ASSERT_EQ(FirstGLError(), 0u);
            EXPECT_EQ(glIsBuffer(buffer), GL_TRUE);
            EXPECT_EQ(glIsTexture(texture), GL_TRUE);
            EXPECT_EQ(glIsVertexArray(vao), GL_TRUE);
            EXPECT_EQ(glIsFramebuffer(fbo), GL_TRUE);
            EXPECT_EQ(glIsRenderbuffer(rbo), GL_TRUE);
            EXPECT_EQ(glIsQuery(query), GL_TRUE);
            EXPECT_EQ(glIsProgramPipeline(pipeline), GL_TRUE);
            EXPECT_EQ(glIsTransformFeedback(xfb), GL_TRUE);
            // Separate spaces: being a buffer says nothing about being a shader, and the reverse.
            EXPECT_EQ(glIsShader(buffer), buffer == shader ? GL_TRUE : GL_FALSE);
            EXPECT_EQ(glIsBuffer(shader), shader == buffer ? GL_TRUE : GL_FALSE);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindVertexArray(0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            glBindProgramPipeline(0);
            glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, 0);
            glDeleteBuffers(1, &buffer);
            glDeleteTextures(1, &texture);
            glDeleteVertexArrays(1, &vao);
            glDeleteFramebuffers(1, &fbo);
            glDeleteRenderbuffers(1, &rbo);
            glDeleteSamplers(1, &sampler);
            glDeleteQueries(1, &query);
            glDeleteProgramPipelines(1, &pipeline);
            glDeleteTransformFeedbacks(1, &xfb);
            ASSERT_EQ(FirstGLError(), 0u);
            EXPECT_EQ(glIsBuffer(buffer), GL_FALSE);
            EXPECT_EQ(glIsTexture(texture), GL_FALSE);
            EXPECT_EQ(glIsVertexArray(vao), GL_FALSE);
            EXPECT_EQ(glIsFramebuffer(fbo), GL_FALSE);
            EXPECT_EQ(glIsRenderbuffer(rbo), GL_FALSE);
            EXPECT_EQ(glIsSampler(sampler), GL_FALSE);
            EXPECT_EQ(glIsQuery(query), GL_FALSE);
            EXPECT_EQ(glIsProgramPipeline(pipeline), GL_FALSE);
            EXPECT_EQ(glIsTransformFeedback(xfb), GL_FALSE);
            EXPECT_EQ(glIsShader(shader), GL_TRUE) << "deleting other kinds' names touched a shader";
            glDeleteShader(shader);
            EXPECT_EQ(FirstGLError(), 0u);
        }

    } // namespace
} // namespace MGITest
