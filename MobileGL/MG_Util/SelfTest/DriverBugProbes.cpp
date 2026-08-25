// MobileGL - MobileGL/MG_Util/SelfTest/DriverBugProbes.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DriverBugProbes.h"

#include <MG_Util/Debug/Log.h>

#include <cstring>
#include <optional>
#include <string>

namespace MobileGL::MG_Util::SelfTest {
    namespace {
        using MG_External::GLESFunctionsTable;

        constexpr const char* kGeometryProbeName = "geometry write-after-emit";

        constexpr GLuint kProbeMagic = 7u;
        constexpr GLsizei kProbeSize = 16;
        // Binding 0 carries the write issued BEFORE EmitVertex (the control), binding 1 the
        // write issued AFTER it (the subject). Same shader, same draw, same buffer shape - the
        // only difference between them is where the store sits.
        constexpr GLuint kBeforeEmitBinding = 0;
        constexpr GLuint kAfterEmitBinding = 1;

        const char* const kProbeVertexSource =
            "#version 320 es\n"
            "void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n";

        // No gl_PointSize anywhere: writing it from a geometry shader needs
        // EXT/OES_geometry_point_size, which not every ES 3.2 driver exposes (Mesa's does not),
        // and a probe that fails to COMPILE reaches no verdict at all.
        const char* const kProbeGeometrySource =
            "#version 320 es\n"
            "layout(points) in;\n"
            "layout(points, max_vertices = 1) out;\n"
            "layout(std430, binding = 0) coherent buffer BeforeEmit { uint data[4]; } g_before;\n"
            "layout(std430, binding = 1) coherent buffer AfterEmit { uint data[4]; } g_after;\n"
            "void main() {\n"
            "  g_before.data[0] = 7u;\n"
            "  gl_Position = gl_in[0].gl_Position;\n"
            "  EmitVertex();\n"
            "  EndPrimitive();\n"
            "  g_after.data[0] = 7u;\n"
            "}\n";

        const char* const kProbeFragmentSource =
            "#version 320 es\n"
            "precision highp float;\n"
            "layout(location = 0) out vec4 o_color;\n"
            "void main() { o_color = vec4(0.0, 1.0, 0.0, 1.0); }\n";

        Bool HasEveryEntryPoint(const GLESFunctionsTable& gl) {
            return gl.glCreateShader && gl.glShaderSource && gl.glCompileShader && gl.glGetShaderiv &&
                   gl.glGetShaderInfoLog && gl.glCreateProgram && gl.glAttachShader && gl.glLinkProgram &&
                   gl.glGetProgramiv && gl.glDeleteShader && gl.glDeleteProgram && gl.glUseProgram &&
                   gl.glGenBuffers && gl.glBindBuffer && gl.glBufferData && gl.glBindBufferBase &&
                   gl.glDeleteBuffers && gl.glGenVertexArrays && gl.glBindVertexArray &&
                   gl.glDeleteVertexArrays && gl.glGenFramebuffers && gl.glBindFramebuffer &&
                   gl.glFramebufferRenderbuffer && gl.glCheckFramebufferStatus && gl.glDeleteFramebuffers &&
                   gl.glGenRenderbuffers && gl.glBindRenderbuffer && gl.glRenderbufferStorage &&
                   gl.glDeleteRenderbuffers && gl.glViewport && gl.glDrawArrays && gl.glMemoryBarrier &&
                   gl.glMapBufferRange && gl.glUnmapBuffer && gl.glGetIntegerv && gl.glGetIntegeri_v &&
                   gl.glGetError && gl.glFinish && gl.glEnable && gl.glDisable && gl.glIsEnabled;
        }

        void Drain(const GLESFunctionsTable& gl) {
            // Bounded: a driver that returns an error forever must not hang the probe.
            for (Int i = 0; i < 32 && gl.glGetError() != GL_NO_ERROR; ++i) {
            }
        }

        GLuint CompileStage(const GLESFunctionsTable& gl, GLenum stage, const char* source,
                            const char* stageName, const char* probeName) {
            const GLuint shader = gl.glCreateShader(stage);
            if (shader == 0) return 0;
            gl.glShaderSource(shader, 1, &source, nullptr);
            gl.glCompileShader(shader);
            GLint compiled = 0;
            gl.glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (compiled == GL_FALSE) {
                // Bounded (at most three lines, once per process) and worth every one: a probe
                // that cannot build its own subject reaches no verdict, and without the driver's
                // reason that is indistinguishable from a clean driver.
                char log[512] = {0};
                gl.glGetShaderInfoLog(shader, static_cast<GLsizei>(sizeof(log) - 1), nullptr, log);
                MGLOG_I("[driver-bug] %s probe: %s stage did not compile: %s", probeName, stageName, log);
                gl.glDeleteShader(shader);
                return 0;
            }
            return shader;
        }

        // Every piece of GL state the probe disturbs, captured on the way in and put back on
        // the way out. It runs inside the POST context, which is not allowed to notice.
        // The image unit every image-using probe binds to. One unit for all of them keeps the
        // saved/restored set small, and nothing in the POST context is using it.
        constexpr GLuint kProbeImageUnit = 1;

        struct SavedState {
            GLint program = 0;
            GLint vertexArray = 0;
            GLint drawFramebuffer = 0;
            GLint readFramebuffer = 0;
            GLint renderbuffer = 0;
            GLint storageBuffer = 0;
            GLint viewport[4] = {0, 0, 0, 0};
            GLint indexedStorageBuffer[2] = {0, 0};
            GLboolean rasterizerDiscard = GL_FALSE;
            GLboolean scissorTest = GL_FALSE;
            GLboolean cullFace = GL_FALSE;
            // Everything the texture- and image-based probes disturb. Saved unconditionally
            // (a query is cheaper than deciding which probe ran) and restored in the same call,
            // so a probe cannot leave a binding behind for the next one to trip over.
            GLboolean depthTest = GL_FALSE;
            GLboolean blend = GL_FALSE;
            GLint activeTexture = GL_TEXTURE0;
            GLint texture2D = 0;
            GLint texture2DMultisample = 0;
            GLfloat clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            GLint packAlignment = 4;
            GLint packRowLength = 0;
            GLint imageName = 0;
            GLint imageLevel = 0;
            GLint imageLayered = 0;
            GLint imageLayer = 0;
            GLint imageAccess = GL_READ_ONLY;
            GLint imageFormat = GL_R32UI;
        };

        // The texture and image parts of the state are only queryable when the driver resolved
        // the entry points that read them; a probe that never touches them still restores the
        // rest.
        void Save(const GLESFunctionsTable& gl, SavedState& state) {
            gl.glGetIntegerv(GL_CURRENT_PROGRAM, &state.program);
            gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &state.vertexArray);
            gl.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &state.drawFramebuffer);
            gl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &state.readFramebuffer);
            gl.glGetIntegerv(GL_RENDERBUFFER_BINDING, &state.renderbuffer);
            gl.glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &state.storageBuffer);
            gl.glGetIntegerv(GL_VIEWPORT, state.viewport);
            for (GLuint i = 0; i < 2; ++i) {
                gl.glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, i, &state.indexedStorageBuffer[i]);
            }
            state.rasterizerDiscard = gl.glIsEnabled(GL_RASTERIZER_DISCARD);
            state.scissorTest = gl.glIsEnabled(GL_SCISSOR_TEST);
            state.cullFace = gl.glIsEnabled(GL_CULL_FACE);
            state.depthTest = gl.glIsEnabled(GL_DEPTH_TEST);
            state.blend = gl.glIsEnabled(GL_BLEND);
            gl.glGetIntegerv(GL_ACTIVE_TEXTURE, &state.activeTexture);
            gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &state.texture2D);
            gl.glGetIntegerv(GL_TEXTURE_BINDING_2D_MULTISAMPLE, &state.texture2DMultisample);
            gl.glGetIntegerv(GL_PACK_ALIGNMENT, &state.packAlignment);
            gl.glGetIntegerv(GL_PACK_ROW_LENGTH, &state.packRowLength);
            if (gl.glGetFloatv != nullptr) {
                gl.glGetFloatv(GL_COLOR_CLEAR_VALUE, state.clearColor);
            }
            gl.glGetIntegeri_v(GL_IMAGE_BINDING_NAME, kProbeImageUnit, &state.imageName);
            gl.glGetIntegeri_v(GL_IMAGE_BINDING_LEVEL, kProbeImageUnit, &state.imageLevel);
            gl.glGetIntegeri_v(GL_IMAGE_BINDING_LAYERED, kProbeImageUnit, &state.imageLayered);
            gl.glGetIntegeri_v(GL_IMAGE_BINDING_LAYER, kProbeImageUnit, &state.imageLayer);
            gl.glGetIntegeri_v(GL_IMAGE_BINDING_ACCESS, kProbeImageUnit, &state.imageAccess);
            gl.glGetIntegeri_v(GL_IMAGE_BINDING_FORMAT, kProbeImageUnit, &state.imageFormat);
            // Any of the above may be rejected by a driver that does not know the pname; the
            // fields keep their defaults and the restore below puts those back, which is the
            // right answer for a context nobody else is sharing.
            Drain(gl);
        }

        // Null-safe throughout: the state it puts back is the superset of what any probe
        // disturbs, and a probe that never needed (say) storage buffers is allowed to run on a
        // driver table where they were never resolved.
        void Restore(const GLESFunctionsTable& gl, const SavedState& state) {
            if (gl.glBindBufferBase != nullptr) {
                for (GLuint i = 0; i < 2; ++i) {
                    gl.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i,
                                        static_cast<GLuint>(state.indexedStorageBuffer[i]));
                }
            }
            if (gl.glBindBuffer != nullptr) {
                gl.glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(state.storageBuffer));
            }
            if (gl.glBindRenderbuffer != nullptr) {
                gl.glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(state.renderbuffer));
            }
            gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(state.drawFramebuffer));
            gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(state.readFramebuffer));
            gl.glBindVertexArray(static_cast<GLuint>(state.vertexArray));
            gl.glUseProgram(static_cast<GLuint>(state.program));
            gl.glViewport(state.viewport[0], state.viewport[1], state.viewport[2], state.viewport[3]);
            if (gl.glBindImageTexture != nullptr) {
                gl.glBindImageTexture(kProbeImageUnit, static_cast<GLuint>(state.imageName),
                                      state.imageLevel, state.imageLayered != 0 ? GL_TRUE : GL_FALSE,
                                      state.imageLayer, static_cast<GLenum>(state.imageAccess),
                                      static_cast<GLenum>(state.imageFormat));
            }
            if (gl.glActiveTexture != nullptr) {
                gl.glActiveTexture(GL_TEXTURE0);
                if (gl.glBindTexture != nullptr) {
                    gl.glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(state.texture2D));
                    gl.glBindTexture(GL_TEXTURE_2D_MULTISAMPLE,
                                     static_cast<GLuint>(state.texture2DMultisample));
                }
                gl.glActiveTexture(static_cast<GLenum>(state.activeTexture));
            }
            if (gl.glPixelStorei != nullptr) {
                gl.glPixelStorei(GL_PACK_ALIGNMENT, state.packAlignment);
                gl.glPixelStorei(GL_PACK_ROW_LENGTH, state.packRowLength);
            }
            if (gl.glClearColor != nullptr) {
                gl.glClearColor(state.clearColor[0], state.clearColor[1], state.clearColor[2],
                                state.clearColor[3]);
            }
            if (state.rasterizerDiscard) gl.glEnable(GL_RASTERIZER_DISCARD);
            if (state.scissorTest) gl.glEnable(GL_SCISSOR_TEST);
            if (state.cullFace) gl.glEnable(GL_CULL_FACE);
            if (state.depthTest) gl.glEnable(GL_DEPTH_TEST);
            if (state.blend) gl.glEnable(GL_BLEND);
            Drain(gl);
        }

        GLuint ReadFirstWord(const GLESFunctionsTable& gl, GLuint buffer) {
            gl.glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
            const void* mapped =
                gl.glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, kProbeSize, GL_MAP_READ_BIT);
            if (mapped == nullptr) return 0u;
            GLuint value = 0;
            std::memcpy(&value, mapped, sizeof(value));
            gl.glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
            return value;
        }
    } // namespace

    Bool ProbeGeometryStageSsboWriteAfterEmitDropped(const GLESFunctionsTable& gl) {
        if (!HasEveryEntryPoint(gl)) return false;

        // Nothing to measure if the driver serves no geometry storage blocks at all.
        GLint advertisedGeometryBlocks = 0;
        Drain(gl);
        gl.glGetIntegerv(GL_MAX_GEOMETRY_SHADER_STORAGE_BLOCKS, &advertisedGeometryBlocks);
        if (gl.glGetError() != GL_NO_ERROR || advertisedGeometryBlocks < 2) {
            Drain(gl);
            return false;
        }

        SavedState saved;
        Save(gl, saved);
        Drain(gl);

        Bool dropped = false;
        const char* inconclusive = nullptr;
        GLuint vertexShader = 0, geometryShader = 0, fragmentShader = 0, program = 0;
        GLuint buffers[2] = {0, 0};
        GLuint vertexArray = 0, framebuffer = 0, renderbuffer = 0;

        do {
            vertexShader = CompileStage(gl, GL_VERTEX_SHADER, kProbeVertexSource, "vertex",
                                        kGeometryProbeName);
            geometryShader = CompileStage(gl, GL_GEOMETRY_SHADER, kProbeGeometrySource, "geometry",
                                          kGeometryProbeName);
            fragmentShader = CompileStage(gl, GL_FRAGMENT_SHADER, kProbeFragmentSource, "fragment",
                                          kGeometryProbeName);
            if (vertexShader == 0 || geometryShader == 0 || fragmentShader == 0) {
                inconclusive = "one of the probe stages did not compile";
                break;
            }

            program = gl.glCreateProgram();
            if (program == 0) {
                inconclusive = "glCreateProgram returned 0";
                break;
            }
            gl.glAttachShader(program, vertexShader);
            gl.glAttachShader(program, geometryShader);
            gl.glAttachShader(program, fragmentShader);
            gl.glLinkProgram(program);
            GLint linked = 0;
            gl.glGetProgramiv(program, GL_LINK_STATUS, &linked);
            // A driver that REFUSES the program is being honest about not supporting it; that
            // is not the silent drop this looks for.
            if (linked == GL_FALSE) {
                inconclusive = "the driver refused to link the probe program, which is an honest "
                               "refusal rather than a silent drop";
                break;
            }

            gl.glGenBuffers(2, buffers);
            const GLuint zero[4] = {0u, 0u, 0u, 0u};
            for (GLuint i = 0; i < 2; ++i) {
                gl.glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[i]);
                gl.glBufferData(GL_SHADER_STORAGE_BUFFER, kProbeSize, zero, GL_DYNAMIC_DRAW);
                gl.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, buffers[i]);
            }

            gl.glGenRenderbuffers(1, &renderbuffer);
            gl.glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
            gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 4, 4);
            gl.glGenFramebuffers(1, &framebuffer);
            gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
            gl.glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                         renderbuffer);
            if (gl.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                inconclusive = "the probe's own 4x4 RGBA8 framebuffer came back incomplete";
                break;
            }
            gl.glViewport(0, 0, 4, 4);

            gl.glGenVertexArrays(1, &vertexArray);
            gl.glBindVertexArray(vertexArray);

            gl.glDisable(GL_RASTERIZER_DISCARD);
            gl.glDisable(GL_SCISSOR_TEST);
            gl.glDisable(GL_CULL_FACE);

            gl.glUseProgram(program);
            Drain(gl);
            gl.glDrawArrays(GL_POINTS, 0, 1);
            if (const GLenum drawError = gl.glGetError(); drawError != GL_NO_ERROR) {
                inconclusive = "the probe draw itself raised a GL error";
                MGLOG_I("[driver-bug] geometry write-after-emit probe: draw error 0x%x", drawError);
                break;
            }
            gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
            gl.glFinish();

            const GLuint beforeEmit = ReadFirstWord(gl, buffers[kBeforeEmitBinding]);
            const GLuint afterEmit = ReadFirstWord(gl, buffers[kAfterEmitBinding]);

            // The control decides whether the subject means anything. If the BEFORE-emit write
            // did not land either, this driver's geometry stage cannot write storage buffers at
            // all - a different (and much larger) claim, which this probe may not make.
            if (beforeEmit != kProbeMagic) {
                inconclusive = "the before-emit control write did not land either, so the "
                               "after-emit result says nothing about emit ordering";
            } else {
                dropped = afterEmit != kProbeMagic;
            }

            MGLOG_I("[driver-bug] geometry write-after-emit probe: advertised %d block(s); "
                    "before-emit write=%u after-emit write=%u (expected %u each)%s",
                    advertisedGeometryBlocks, beforeEmit, afterEmit, kProbeMagic,
                    dropped ? " - WRITES AFTER EmitVertex ARE DISCARDED" : "");
        } while (false);

        if (inconclusive != nullptr) {
            MGLOG_I("[driver-bug] geometry write-after-emit probe reached no verdict (%s)",
                    inconclusive);
        }

        if (vertexArray != 0) gl.glDeleteVertexArrays(1, &vertexArray);
        if (framebuffer != 0) gl.glDeleteFramebuffers(1, &framebuffer);
        if (renderbuffer != 0) gl.glDeleteRenderbuffers(1, &renderbuffer);
        if (buffers[0] != 0) gl.glDeleteBuffers(2, buffers);
        if (program != 0) gl.glDeleteProgram(program);
        if (vertexShader != 0) gl.glDeleteShader(vertexShader);
        if (geometryShader != 0) gl.glDeleteShader(geometryShader);
        if (fragmentShader != 0) gl.glDeleteShader(fragmentShader);

        Restore(gl, saved);
        return dropped;
    }

    Bool GeometryStageSsboWriteAfterEmitDropped(const GLESFunctionsTable& gl) {
        // One driver per process, and the answer is structural rather than sampled.
        static const Bool dropped = ProbeGeometryStageSsboWriteAfterEmitDropped(gl);
        return dropped;
    }

    namespace {
        // ===================== SHARED PROGRAM BUILDING =====================
        // The four probes below all decide their verdict from whether a program LINKS or from
        // what a draw wrote, so they share one builder that keeps the two apart: a stage that
        // does not COMPILE means the probe could not build its own subject (inconclusive),
        // while a program that compiles and does not LINK is a result.

        struct StageSource {
            GLenum stage = GL_VERTEX_SHADER;
            String source;
            const char* name = "";
        };

        struct ProgramBuild {
            GLuint program = 0;
            Bool compiled = false;
            Bool linked = false;
            String infoLog;
        };

        ProgramBuild BuildProgram(const GLESFunctionsTable& gl, const Vector<StageSource>& stages,
                                  const char* probeName) {
            ProgramBuild build;
            Vector<GLuint> shaders;
            const auto releaseShaders = [&]() {
                for (const GLuint shader : shaders) {
                    gl.glDeleteShader(shader);
                }
            };
            for (const StageSource& stage : stages) {
                const GLuint shader =
                    CompileStage(gl, stage.stage, stage.source.c_str(), stage.name, probeName);
                if (shader == 0) {
                    releaseShaders();
                    return build;
                }
                shaders.push_back(shader);
            }
            build.compiled = true;
            build.program = gl.glCreateProgram();
            if (build.program == 0) {
                build.compiled = false;
                releaseShaders();
                return build;
            }
            for (const GLuint shader : shaders) {
                gl.glAttachShader(build.program, shader);
            }
            gl.glLinkProgram(build.program);
            GLint linked = 0;
            gl.glGetProgramiv(build.program, GL_LINK_STATUS, &linked);
            build.linked = linked != GL_FALSE;
            char log[512] = {0};
            gl.glGetProgramInfoLog(build.program, static_cast<GLsizei>(sizeof(log) - 1), nullptr, log);
            build.infoLog = log;
            releaseShaders();
            return build;
        }

        // The driver's own words, trimmed to the first line: a report that quotes the linker is
        // far more actionable than one that paraphrases it, but the whole log is multi-line and
        // this is a one-liner.
        String FirstLine(const String& text) {
            const SizeT end = text.find_first_of("\r\n");
            String line = end == String::npos ? text : text.substr(0, end);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
                line.pop_back();
            }
            return line;
        }

        // Every probe below rasterizes the same full-viewport quad from a vertex ID, so none of
        // them needs a vertex buffer or an attribute.
        const char* const kFullscreenQuadBody =
            "    vec2 mg_p = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0,\n"
            "                     (gl_VertexID & 2) == 0 ? -1.0 : 1.0);\n"
            "    gl_Position = vec4(mg_p, 0.0, 1.0);\n";

        const char* const kProbeQuadVertexSource =
            "#version 320 es\n"
            "void main()\n{\n"
            "    vec2 mg_p = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0,\n"
            "                     (gl_VertexID & 2) == 0 ? -1.0 : 1.0);\n"
            "    gl_Position = vec4(mg_p, 0.0, 1.0);\n}\n";

        // Puts the context into the shape every probe draw wants. The caller has already saved
        // the state this disturbs.
        void PrepareForProbeDraw(const GLESFunctionsTable& gl) {
            gl.glDisable(GL_RASTERIZER_DISCARD);
            gl.glDisable(GL_SCISSOR_TEST);
            gl.glDisable(GL_CULL_FACE);
            gl.glDisable(GL_DEPTH_TEST);
            gl.glDisable(GL_BLEND);
            gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
            gl.glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        }

        // ===================== R32F MULTISAMPLE SWIZZLE =====================

        constexpr const char* kMsaaProbeName = "R32F multisample swizzle";
        constexpr GLsizei kMsaaOutputSize = 8;
        // The fill draw writes 1.0 to red and nothing else, so an R32F texel reads back as
        // (1, 0, 0, 1) - the missing green/blue default to 0 and the missing alpha to 1. With
        // GL_TEXTURE_SWIZZLE_A pointed at RED the .w a shader sees is that same 1.0, which is
        // what makes a single expected constant enough for both the subject and the controls.
        constexpr GLfloat kMsaaExpected = 1.0f;
        // Neither 0 nor the expected value, so "the clear never landed" and "the draw never
        // landed" are distinguishable from "the driver returned the wrong number".
        constexpr GLfloat kMsaaClearSentinel = 7.0f;

        Bool HasMsaaEntryPoints(const GLESFunctionsTable& gl) {
            return gl.glCreateShader && gl.glShaderSource && gl.glCompileShader && gl.glGetShaderiv &&
                   gl.glGetShaderInfoLog && gl.glCreateProgram && gl.glAttachShader && gl.glLinkProgram &&
                   gl.glGetProgramiv && gl.glGetProgramInfoLog && gl.glDeleteShader &&
                   gl.glDeleteProgram && gl.glUseProgram && gl.glGenTextures && gl.glBindTexture &&
                   gl.glDeleteTextures && gl.glTexParameteri && gl.glTexImage2D &&
                   gl.glTexStorage2DMultisample && gl.glGenFramebuffers && gl.glBindFramebuffer &&
                   gl.glFramebufferTexture2D && gl.glCheckFramebufferStatus && gl.glDeleteFramebuffers &&
                   gl.glGenVertexArrays && gl.glBindVertexArray && gl.glDeleteVertexArrays &&
                   gl.glActiveTexture && gl.glGetUniformLocation && gl.glUniform1i && gl.glViewport &&
                   gl.glClear && gl.glClearColor && gl.glReadPixels && gl.glDrawArrays &&
                   gl.glPixelStorei && gl.glGetIntegerv && gl.glGetIntegeri_v && gl.glGetError &&
                   gl.glFinish && gl.glEnable && gl.glDisable && gl.glIsEnabled;
        }

        const char* const kMsaaFillFragmentSource =
            "#version 320 es\n"
            "precision highp float;\n"
            "layout(location = 0) out highp vec4 o_color;\n"
            "void main() { o_color = vec4(1.0, 0.0, 0.0, 0.0); }\n";

        const char* const kMsaaSampleFragmentSource =
            "#version 320 es\n"
            "precision highp float;\n"
            "layout(location = 0) out highp float o_color;\n"
            "layout(location = 0) flat in highp float v_result;\n"
            "void main() { o_color = v_result; }\n";

        // One vertex shader per round. The `round` term is multiplied by zero, so it changes
        // nothing about the result and everything about the source text - which is the point:
        // the corruption only appears from the SECOND separately compiled sampling program
        // onward, and a driver that recognised an identical source could hand back the first
        // program's binary and hide it.
        String BuildMsaaSampleVertexSource(Int sampleIndex, char component, Int round) {
            return format("#version 320 es\n"
                          "precision highp float;\n"
                          "precision highp int;\n"
                          "uniform highp sampler2DMS mg_probeSampler;\n"
                          "layout(location = 0) flat out float v_result;\n"
                          "void main()\n{{\n"
                          "    v_result = texelFetch(mg_probeSampler, ivec2(0), {}).{} + float({}) * 0.0;\n"
                          "{}}}\n",
                          sampleIndex, component, round, kFullscreenQuadBody);
        }

        // Samples one texel of `source` `rounds` times, each round through its own freshly
        // linked program and its own freshly created R32F output texture, and stores what came
        // back. Returns false when any round could not be set up at all.
        Bool RunMsaaSampledRead(const GLESFunctionsTable& gl, GLuint source, Int sampleIndex,
                                char component, Int rounds, Vector<GLfloat>& values) {
            values.clear();
            for (Int round = 0; round < rounds; ++round) {
                GLuint output = 0;
                GLuint framebuffer = 0;
                Bool ok = false;
                ProgramBuild build;
                do {
                    gl.glGenTextures(1, &output);
                    gl.glBindTexture(GL_TEXTURE_2D, output);
                    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, kMsaaOutputSize, kMsaaOutputSize, 0,
                                    GL_RED, GL_FLOAT, nullptr);
                    gl.glGenFramebuffers(1, &framebuffer);
                    gl.glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
                    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                              output, 0);
                    if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) break;

                    build = BuildProgram(gl,
                                         {{GL_VERTEX_SHADER,
                                           BuildMsaaSampleVertexSource(sampleIndex, component, round),
                                           "vertex"},
                                          {GL_FRAGMENT_SHADER, kMsaaSampleFragmentSource, "fragment"}},
                                         kMsaaProbeName);
                    if (!build.linked) break;

                    gl.glUseProgram(build.program);
                    gl.glActiveTexture(GL_TEXTURE0);
                    gl.glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, source);
                    gl.glUniform1i(gl.glGetUniformLocation(build.program, "mg_probeSampler"), 0);
                    gl.glViewport(0, 0, kMsaaOutputSize, kMsaaOutputSize);
                    gl.glClearColor(kMsaaClearSentinel, kMsaaClearSentinel, kMsaaClearSentinel,
                                    kMsaaClearSentinel);
                    gl.glClear(GL_COLOR_BUFFER_BIT);
                    Drain(gl);
                    gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    if (gl.glGetError() != GL_NO_ERROR) break;
                    gl.glFinish();

                    Vector<GLfloat> pixels(static_cast<SizeT>(kMsaaOutputSize) * kMsaaOutputSize, 0.0f);
                    gl.glReadPixels(0, 0, kMsaaOutputSize, kMsaaOutputSize, GL_RED, GL_FLOAT,
                                    pixels.data());
                    if (gl.glGetError() != GL_NO_ERROR) break;
                    values.push_back(pixels[0]);
                    ok = true;
                } while (false);

                if (build.program != 0) gl.glDeleteProgram(build.program);
                if (framebuffer != 0) gl.glDeleteFramebuffers(1, &framebuffer);
                if (output != 0) gl.glDeleteTextures(1, &output);
                if (!ok) return false;
            }
            return true;
        }

        // Creates a `samples`-sample R32F multisample texture, fills it with (1, 0, 0, 0), and
        // points GL_TEXTURE_SWIZZLE_R/G/B at ALPHA/BLUE/GREEN with GL_TEXTURE_SWIZZLE_A at
        // `alphaSwizzle`. Every source the probe builds is identical except for that one enum.
        GLuint MakeSwizzledMultisampleSource(const GLESFunctionsTable& gl, GLsizei samples,
                                             GLenum alphaSwizzle) {
            GLuint texture = 0;
            gl.glGenTextures(1, &texture);
            gl.glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, texture);
            gl.glTexStorage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_R32F, 1, 1, GL_FALSE);
            if (gl.glGetError() != GL_NO_ERROR) {
                gl.glDeleteTextures(1, &texture);
                return 0;
            }

            GLuint framebuffer = 0;
            gl.glGenFramebuffers(1, &framebuffer);
            gl.glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE,
                                      texture, 0);
            Bool filled = false;
            ProgramBuild fill;
            if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                fill = BuildProgram(gl,
                                    {{GL_VERTEX_SHADER, kProbeQuadVertexSource, "vertex"},
                                     {GL_FRAGMENT_SHADER, kMsaaFillFragmentSource, "fragment"}},
                                    kMsaaProbeName);
                if (fill.linked) {
                    gl.glUseProgram(fill.program);
                    gl.glViewport(0, 0, 1, 1);
                    gl.glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                    gl.glClear(GL_COLOR_BUFFER_BIT);
                    Drain(gl);
                    gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    filled = gl.glGetError() == GL_NO_ERROR;
                    gl.glFinish();
                }
            }
            if (fill.program != 0) gl.glDeleteProgram(fill.program);
            gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (framebuffer != 0) gl.glDeleteFramebuffers(1, &framebuffer);
            if (!filled) {
                gl.glDeleteTextures(1, &texture);
                return 0;
            }

            gl.glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, texture);
            gl.glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_SWIZZLE_R, GL_ALPHA);
            gl.glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_SWIZZLE_G, GL_BLUE);
            gl.glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_SWIZZLE_B, GL_GREEN);
            gl.glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_SWIZZLE_A, alphaSwizzle);
            Drain(gl);
            return texture;
        }

        Bool EveryRoundIsExpected(const Vector<GLfloat>& values) {
            if (values.empty()) return false;
            for (const GLfloat value : values) {
                if (value != kMsaaExpected) return false;
            }
            return true;
        }

        String DescribeRounds(const Vector<GLfloat>& values) {
            String description;
            for (SizeT i = 0; i < values.size(); ++i) {
                if (i != 0) description += ", ";
                description += format("round{}={}", i, values[i]);
            }
            return description;
        }

        // ===================== IMAGE LOCATION PER NAME =====================

        constexpr const char* kImageLocationProbeName = "image-location-per-name";

        Bool HasImageProgramEntryPoints(const GLESFunctionsTable& gl) {
            return gl.glCreateShader && gl.glShaderSource && gl.glCompileShader && gl.glGetShaderiv &&
                   gl.glGetShaderInfoLog && gl.glCreateProgram && gl.glAttachShader && gl.glLinkProgram &&
                   gl.glGetProgramiv && gl.glGetProgramInfoLog && gl.glDeleteShader &&
                   gl.glDeleteProgram && gl.glGetIntegerv && gl.glGetIntegeri_v && gl.glGetError &&
                   gl.glIsEnabled;
        }

        // One stage declaring `count` image uniforms named `namePrefix`0..count-1, each stored to
        // so the compiler cannot eliminate it. The bindings cycle through the driver's image
        // units, so the subject and the control occupy exactly the same units - the names are
        // the only thing that differs between them.
        String BuildImageNameStageSource(GLenum stage, Int count, Int units, const char* namePrefix) {
            String source = "#version 320 es\n";
            if (stage == GL_GEOMETRY_SHADER) {
                source += "layout(points) in;\nlayout(points, max_vertices = 1) out;\n";
            }
            source += "precision highp float;\nprecision highp int;\n";
            for (Int i = 0; i < count; ++i) {
                source += format("layout(binding = {}, rgba32f) uniform writeonly highp image2D {}{};\n",
                                 i % units, namePrefix, i);
            }
            if (stage == GL_FRAGMENT_SHADER) {
                source += "layout(location = 0) out highp vec4 o_color;\n";
            }
            source += "void main()\n{\n";
            for (Int i = 0; i < count; ++i) {
                source += format("    imageStore({}{}, ivec2(0), vec4({}.0));\n", namePrefix, i, i);
            }
            switch (stage) {
            case GL_VERTEX_SHADER:
                source += "    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n";
                break;
            case GL_GEOMETRY_SHADER:
                source += "    gl_Position = gl_in[0].gl_Position;\n    EmitVertex();\n    EndPrimitive();\n";
                break;
            default:
                source += "    o_color = vec4(0.0);\n";
                break;
            }
            source += "}\n";
            return source;
        }

        ProgramBuild BuildThreeStageImageProgram(const GLESFunctionsTable& gl, Int count, Int units,
                                                 const char* vertexPrefix, const char* geometryPrefix,
                                                 const char* fragmentPrefix) {
            return BuildProgram(
                gl,
                {{GL_VERTEX_SHADER, BuildImageNameStageSource(GL_VERTEX_SHADER, count, units, vertexPrefix),
                  "vertex"},
                 {GL_GEOMETRY_SHADER,
                  BuildImageNameStageSource(GL_GEOMETRY_SHADER, count, units, geometryPrefix), "geometry"},
                 {GL_FRAGMENT_SHADER,
                  BuildImageNameStageSource(GL_FRAGMENT_SHADER, count, units, fragmentPrefix), "fragment"}},
                kImageLocationProbeName);
        }

        // ===================== SHARED IMAGE-DRAW SCAFFOLDING =====================
        // Both remaining probes bind one rgba32f image, draw a quad into a small RGBA8 target,
        // and count the texels the shader painted red. Sharing the scaffolding keeps what
        // differs between them - the shader text - the only thing either probe has to explain.

        constexpr GLubyte kProbePassColor[4] = {0, 255, 0, 255};

        struct ImageDrawTargets {
            GLuint image = 0;
            GLuint color = 0;
            GLuint framebuffer = 0;
            GLsizei size = 0;
            Bool valid = false;
        };

        Bool HasImageDrawEntryPoints(const GLESFunctionsTable& gl) {
            return HasImageProgramEntryPoints(gl) && gl.glUseProgram && gl.glGenTextures &&
                   gl.glBindTexture && gl.glDeleteTextures && gl.glTexStorage2D && gl.glTexSubImage2D &&
                   gl.glGenFramebuffers && gl.glBindFramebuffer && gl.glFramebufferTexture2D &&
                   gl.glCheckFramebufferStatus && gl.glDeleteFramebuffers && gl.glGenVertexArrays &&
                   gl.glBindVertexArray && gl.glDeleteVertexArrays && gl.glBindImageTexture &&
                   gl.glViewport && gl.glClear && gl.glClearColor && gl.glReadPixels && gl.glDrawArrays &&
                   gl.glMemoryBarrier && gl.glPixelStorei && gl.glFinish && gl.glEnable && gl.glDisable;
        }

        ImageDrawTargets MakeImageDrawTargets(const GLESFunctionsTable& gl, GLsizei size) {
            ImageDrawTargets targets;
            targets.size = size;
            gl.glGenTextures(1, &targets.image);
            gl.glBindTexture(GL_TEXTURE_2D, targets.image);
            gl.glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, size, size);
            gl.glGenTextures(1, &targets.color);
            gl.glBindTexture(GL_TEXTURE_2D, targets.color);
            gl.glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, size, size);
            gl.glGenFramebuffers(1, &targets.framebuffer);
            gl.glBindFramebuffer(GL_FRAMEBUFFER, targets.framebuffer);
            gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targets.color,
                                      0);
            targets.valid = gl.glGetError() == GL_NO_ERROR &&
                            gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            Drain(gl);
            return targets;
        }

        void ReleaseImageDrawTargets(const GLESFunctionsTable& gl, ImageDrawTargets& targets) {
            if (targets.framebuffer != 0) gl.glDeleteFramebuffers(1, &targets.framebuffer);
            if (targets.color != 0) gl.glDeleteTextures(1, &targets.color);
            if (targets.image != 0) gl.glDeleteTextures(1, &targets.image);
            targets = ImageDrawTargets{};
        }

        // Zeroes the image so a stale value from an earlier draw can never stand in for a store
        // that did not happen.
        void ZeroProbeImage(const GLESFunctionsTable& gl, const ImageDrawTargets& targets) {
            const SizeT texelCount = static_cast<SizeT>(targets.size) * targets.size * 4;
            const Vector<GLfloat> zeroes(texelCount, 0.0f);
            gl.glBindTexture(GL_TEXTURE_2D, targets.image);
            gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, targets.size, targets.size, GL_RGBA, GL_FLOAT,
                               zeroes.data());
            gl.glMemoryBarrier(GL_ALL_BARRIER_BITS);
            gl.glFinish();
            Drain(gl);
        }

        // Reads the colour target back and counts the texels the shader did NOT paint with
        // kProbePassColor. A negative result means the readback itself failed.
        Int CountFailedTexels(const GLESFunctionsTable& gl, const ImageDrawTargets& targets) {
            Vector<GLubyte> pixels(static_cast<SizeT>(targets.size) * targets.size * 4, 0);
            Drain(gl);
            gl.glReadPixels(0, 0, targets.size, targets.size, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            if (gl.glGetError() != GL_NO_ERROR) return -1;
            Int failed = 0;
            for (SizeT texel = 0; texel < pixels.size(); texel += 4) {
                if (pixels[texel + 0] != kProbePassColor[0] || pixels[texel + 1] != kProbePassColor[1] ||
                    pixels[texel + 2] != kProbePassColor[2] || pixels[texel + 3] != kProbePassColor[3]) {
                    ++failed;
                }
            }
            return failed;
        }

        void BeginProbeDraw(const GLESFunctionsTable& gl, const ImageDrawTargets& targets) {
            gl.glBindImageTexture(kProbeImageUnit, targets.image, 0, GL_FALSE, 0, GL_READ_WRITE,
                                  GL_RGBA32F);
            gl.glBindFramebuffer(GL_FRAMEBUFFER, targets.framebuffer);
            gl.glViewport(0, 0, targets.size, targets.size);
            gl.glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            gl.glClear(GL_COLOR_BUFFER_BIT);
        }

        // ===================== CROSS-STAGE QUALIFIER MERGE =====================

        constexpr const char* kQualifierMergeProbeName = "cross-stage image qualifier merge";
        constexpr GLsizei kQualifierMergeSize = 16;
        // The value the vertex stage stores and the fragment stage expects to read back.
        constexpr const char* kQualifierMergeValue = "2.0";

        String BuildQualifierMergeVertexSource(const char* writeName) {
            return format("#version 320 es\n"
                          "precision highp float;\n"
                          "precision highp int;\n"
                          "layout(binding = {}, rgba32f) uniform coherent writeonly highp image2D {};\n"
                          "void main()\n{{\n"
                          "{}"
                          "    imageStore({}, ivec2(0), vec4({}));\n"
                          "    memoryBarrier();\n}}\n",
                          kProbeImageUnit, writeName, kFullscreenQuadBody, writeName,
                          kQualifierMergeValue);
        }

        String BuildQualifierMergeFragmentSource(const char* readName) {
            return format("#version 320 es\n"
                          "precision highp float;\n"
                          "precision highp int;\n"
                          "layout(binding = {}, rgba32f) uniform coherent readonly highp image2D {};\n"
                          "layout(location = 0) out highp vec4 o_color;\n"
                          "void main()\n{{\n"
                          "    o_color = vec4(0.0, 1.0, 0.0, 1.0);\n"
                          "    if (any(notEqual(imageLoad({}, ivec2(0)), vec4({}))))\n"
                          "    {{\n        o_color = vec4(1.0, 0.0, 0.0, 1.0);\n    }}\n}}\n",
                          kProbeImageUnit, readName, readName, kQualifierMergeValue);
        }

        // Draws the pair once and reports how many fragments failed to see the vertex stage's
        // store. -1 means the case could not be built or drawn at all.
        Int RunQualifierMergeCase(const GLESFunctionsTable& gl, const ImageDrawTargets& targets,
                                  const char* writeName, const char* readName) {
            ProgramBuild build =
                BuildProgram(gl,
                             {{GL_VERTEX_SHADER, BuildQualifierMergeVertexSource(writeName), "vertex"},
                              {GL_FRAGMENT_SHADER, BuildQualifierMergeFragmentSource(readName),
                               "fragment"}},
                             kQualifierMergeProbeName);
            Int failed = -1;
            if (build.linked) {
                ZeroProbeImage(gl, targets);
                BeginProbeDraw(gl, targets);
                gl.glUseProgram(build.program);
                Drain(gl);
                gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                if (gl.glGetError() == GL_NO_ERROR) {
                    gl.glMemoryBarrier(GL_ALL_BARRIER_BITS);
                    gl.glFinish();
                    failed = CountFailedTexels(gl, targets);
                }
            }
            if (build.program != 0) gl.glDeleteProgram(build.program);
            return failed;
        }

        // ===================== IMAGE COHERENCY RESIDUAL =====================

        constexpr const char* kCoherencyProbeName = "image write-read coherency";
        constexpr GLsizei kCoherencySize = 64;

        // A split read-write image pair on ONE binding, storing and then reading the same texel
        // back inside one invocation. `qualifiers` and `barriers` are what separate the SUBJECT
        // (the strongest shape ESSL offers) from the shape MobileGL emits today; everything else
        // about the two is identical, so the two numbers are comparable.
        String BuildCoherencyFragmentSource(const char* qualifiers, const char* barriers) {
            return format(
                "#version 320 es\n"
                "precision highp float;\n"
                "precision highp int;\n"
                "layout(binding = {}, rgba32f) uniform {} readonly highp image2D mg_probeImageRead;\n"
                "layout(binding = {}, rgba32f) uniform {} writeonly highp image2D mg_probeImageWrite;\n"
                "layout(location = 0) out highp vec4 o_color;\n"
                "void main()\n{{\n"
                "    ivec2 mg_c = ivec2(gl_FragCoord.xy);\n"
                "    o_color = vec4(0.0, 1.0, 0.0, 1.0);\n"
                "    for (int mg_i = 0; mg_i < 3; ++mg_i)\n    {{\n"
                "        imageStore(mg_probeImageWrite, mg_c, vec4(float(mg_i)));\n"
                "        {}\n"
                "        highp vec4 mg_v = imageLoad(mg_probeImageRead, mg_c);\n"
                "        if (any(notEqual(mg_v, vec4(float(mg_i)))))\n        {{\n"
                "            o_color = vec4(1.0, 0.0, 0.0, 1.0);\n            break;\n        }}\n"
                "    }}\n}}\n",
                kProbeImageUnit, qualifiers, kProbeImageUnit, qualifiers, barriers);
        }

        // Every qualifier and every barrier ESSL has for this. A driver that still misses the
        // store here has nothing left to be told.
        constexpr const char* kStrongestCoherencyQualifiers = "coherent volatile";
        constexpr const char* kStrongestCoherencyBarriers = "memoryBarrierImage(); memoryBarrier();";
        // What SplitReadWriteImageUniforms emits today, measured alongside so the row can report
        // what applications actually get rather than only what is theoretically reachable.
        constexpr const char* kEmittedCoherencyQualifiers = "coherent";
        constexpr const char* kEmittedCoherencyBarriers = "memoryBarrierImage();";

        // The control's two halves. Same image, same binding, same qualifiers, same dependency -
        // the store and the read are just in different draws, with a glMemoryBarrier and a
        // glFinish between them.
        String BuildCoherencyControlStoreSource() {
            return format(
                "#version 320 es\n"
                "precision highp float;\n"
                "precision highp int;\n"
                "layout(binding = {}, rgba32f) uniform coherent writeonly highp image2D mg_probeImageWrite;\n"
                "layout(location = 0) out highp vec4 o_color;\n"
                "void main()\n{{\n"
                "    ivec2 mg_c = ivec2(gl_FragCoord.xy);\n"
                "    imageStore(mg_probeImageWrite, mg_c, vec4(float(mg_c.x * {} + mg_c.y)));\n"
                "    memoryBarrierImage();\n"
                "    o_color = vec4(0.0, 1.0, 0.0, 1.0);\n}}\n",
                kProbeImageUnit, kCoherencySize);
        }

        String BuildCoherencyControlLoadSource() {
            return format(
                "#version 320 es\n"
                "precision highp float;\n"
                "precision highp int;\n"
                "layout(binding = {}, rgba32f) uniform coherent readonly highp image2D mg_probeImageRead;\n"
                "layout(location = 0) out highp vec4 o_color;\n"
                "void main()\n{{\n"
                "    ivec2 mg_c = ivec2(gl_FragCoord.xy);\n"
                "    highp vec4 mg_v = imageLoad(mg_probeImageRead, mg_c);\n"
                "    o_color = all(equal(mg_v, vec4(float(mg_c.x * {} + mg_c.y))))\n"
                "                  ? vec4(0.0, 1.0, 0.0, 1.0)\n"
                "                  : vec4(1.0, 0.0, 0.0, 1.0);\n}}\n",
                kProbeImageUnit, kCoherencySize);
        }

        // One draw, store and dependent read inside the same invocation, in whichever
        // qualifier/barrier shape the caller asked for.
        Int RunCoherencyShape(const GLESFunctionsTable& gl, const ImageDrawTargets& targets,
                              const char* qualifiers, const char* barriers) {
            ProgramBuild build = BuildProgram(
                gl,
                {{GL_VERTEX_SHADER, kProbeQuadVertexSource, "vertex"},
                 {GL_FRAGMENT_SHADER, BuildCoherencyFragmentSource(qualifiers, barriers), "fragment"}},
                kCoherencyProbeName);
            Int failed = -1;
            if (build.linked) {
                ZeroProbeImage(gl, targets);
                BeginProbeDraw(gl, targets);
                gl.glUseProgram(build.program);
                Drain(gl);
                gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                if (gl.glGetError() == GL_NO_ERROR) {
                    gl.glMemoryBarrier(GL_ALL_BARRIER_BITS);
                    gl.glFinish();
                    failed = CountFailedTexels(gl, targets);
                }
            }
            if (build.program != 0) gl.glDeleteProgram(build.program);
            return failed;
        }

        // The control: the same store and the same dependent read, split across two draws with a
        // glMemoryBarrier and a glFinish between them.
        Int RunCoherencyControl(const GLESFunctionsTable& gl, const ImageDrawTargets& targets) {
            ProgramBuild store =
                BuildProgram(gl,
                             {{GL_VERTEX_SHADER, kProbeQuadVertexSource, "vertex"},
                              {GL_FRAGMENT_SHADER, BuildCoherencyControlStoreSource(), "fragment"}},
                             kCoherencyProbeName);
            ProgramBuild load =
                BuildProgram(gl,
                             {{GL_VERTEX_SHADER, kProbeQuadVertexSource, "vertex"},
                              {GL_FRAGMENT_SHADER, BuildCoherencyControlLoadSource(), "fragment"}},
                             kCoherencyProbeName);
            Int failed = -1;
            if (store.linked && load.linked) {
                ZeroProbeImage(gl, targets);
                BeginProbeDraw(gl, targets);
                gl.glUseProgram(store.program);
                Drain(gl);
                gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                gl.glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
                gl.glFinish();
                gl.glUseProgram(load.program);
                gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                if (gl.glGetError() == GL_NO_ERROR) {
                    gl.glMemoryBarrier(GL_ALL_BARRIER_BITS);
                    gl.glFinish();
                    failed = CountFailedTexels(gl, targets);
                }
            }
            if (store.program != 0) gl.glDeleteProgram(store.program);
            if (load.program != 0) gl.glDeleteProgram(load.program);
            return failed;
        }
    } // namespace

    Bool ProbeR32FMultisampleSwizzleCorruption(const GLESFunctionsTable& gl) {
        if (!HasMsaaEntryPoints(gl)) return false;

        // A sample index other than 0 is the whole subject, so a driver that cannot host a
        // multisample colour target at all has nothing to say here.
        Drain(gl);
        GLint maxColorTextureSamples = 0;
        gl.glGetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &maxColorTextureSamples);
        const Bool sampleQueryAccepted = gl.glGetError() == GL_NO_ERROR;
        Drain(gl);
        if (!sampleQueryAccepted || maxColorTextureSamples < 2) return false;
        const GLsizei samples =
            maxColorTextureSamples >= 4 ? 4 : static_cast<GLsizei>(maxColorTextureSamples);
        const Int lastSample = static_cast<Int>(samples) - 1;

        SavedState saved;
        Save(gl, saved);
        Drain(gl);

        Bool corrupted = false;
        const char* inconclusive = nullptr;
        GLuint vertexArray = 0;
        GLuint swizzledSource = 0;
        GLuint defaultSwizzleSource = 0;
        Vector<GLfloat> subject;
        Vector<GLfloat> controlDefaultSwizzle;
        Vector<GLfloat> controlSampleZero;
        Vector<GLfloat> controlRedComponent;

        do {
            gl.glGenVertexArrays(1, &vertexArray);
            gl.glBindVertexArray(vertexArray);
            PrepareForProbeDraw(gl);

            // Two sources, identical but for GL_TEXTURE_SWIZZLE_A. Separate textures rather than
            // one re-swizzled between reads: re-pushing the swizzle is itself a variable, and it
            // was measured not to be the one that matters.
            swizzledSource = MakeSwizzledMultisampleSource(gl, samples, GL_RED);
            defaultSwizzleSource = MakeSwizzledMultisampleSource(gl, samples, GL_ALPHA);
            if (swizzledSource == 0 || defaultSwizzleSource == 0) {
                inconclusive = "the driver would not host a filled R32F multisample colour target";
                break;
            }

            // Two rounds each: the corruption only appears from the second separately linked
            // sampling program onward, so a single read would call an affected driver clean.
            constexpr Int kRounds = 2;
            if (!RunMsaaSampledRead(gl, swizzledSource, lastSample, 'w', kRounds, subject) ||
                !RunMsaaSampledRead(gl, defaultSwizzleSource, lastSample, 'w', kRounds,
                                    controlDefaultSwizzle) ||
                !RunMsaaSampledRead(gl, swizzledSource, 0, 'w', kRounds, controlSampleZero) ||
                !RunMsaaSampledRead(gl, swizzledSource, lastSample, 'x', kRounds, controlRedComponent)) {
                inconclusive = "one of the sampling programs could not be built or drawn";
                break;
            }

            // All three controls have to be right for the subject to mean anything. A driver
            // that simply cannot render R32F, or cannot fetch multisample texels, would fail the
            // subject too - and calling that "the alpha swizzle is corrupted" would be a claim
            // the probe has no evidence for.
            const Bool controlsClean = EveryRoundIsExpected(controlDefaultSwizzle) &&
                                       EveryRoundIsExpected(controlSampleZero) &&
                                       EveryRoundIsExpected(controlRedComponent);
            if (!controlsClean) {
                inconclusive = "a control read came back wrong too, so the subject says nothing "
                               "about the alpha swizzle in particular";
                break;
            }
            corrupted = !EveryRoundIsExpected(subject);

            MGLOG_I("[driver-bug] R32F multisample swizzle probe: %d samples; subject(swizzle_a=RED, "
                    "sample=%d, .w) %s; controls default-swizzle [%s] sample-0 [%s] .x [%s]; expected "
                    "%g everywhere%s",
                    static_cast<int>(samples), static_cast<int>(lastSample),
                    DescribeRounds(subject).c_str(), DescribeRounds(controlDefaultSwizzle).c_str(),
                    DescribeRounds(controlSampleZero).c_str(), DescribeRounds(controlRedComponent).c_str(),
                    static_cast<double>(kMsaaExpected),
                    corrupted ? " - THE SWIZZLED ALPHA READ IS CORRUPTED" : "");
        } while (false);

        if (inconclusive != nullptr) {
            MGLOG_I("[driver-bug] R32F multisample swizzle probe reached no verdict (%s)", inconclusive);
        }

        if (vertexArray != 0) gl.glDeleteVertexArrays(1, &vertexArray);
        if (swizzledSource != 0) gl.glDeleteTextures(1, &swizzledSource);
        if (defaultSwizzleSource != 0) gl.glDeleteTextures(1, &defaultSwizzleSource);

        Restore(gl, saved);
        return corrupted;
    }

    Bool R32FMultisampleSwizzleCorrupted(const GLESFunctionsTable& gl) {
        static const Bool corrupted = ProbeR32FMultisampleSwizzleCorruption(gl);
        return corrupted;
    }

    ImageLocationBudgetMeasurement ProbeImageLocationPerNameBudget(const GLESFunctionsTable& gl) {
        ImageLocationBudgetMeasurement measurement;
        if (!HasImageProgramEntryPoints(gl)) return measurement;

        Drain(gl);
        GLint maxImageUnits = 0;
        GLint maxVertexImages = 0;
        GLint maxFragmentImages = 0;
        GLint maxGeometryImages = 0;
        gl.glGetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);
        gl.glGetIntegerv(GL_MAX_VERTEX_IMAGE_UNIFORMS, &maxVertexImages);
        gl.glGetIntegerv(GL_MAX_FRAGMENT_IMAGE_UNIFORMS, &maxFragmentImages);
        // ES 3.2 only; an older context raises GL_INVALID_ENUM and has no geometry stage to
        // build the shape out of anyway.
        gl.glGetIntegerv(GL_MAX_GEOMETRY_IMAGE_UNIFORMS, &maxGeometryImages);
        const Bool limitsAccepted = gl.glGetError() == GL_NO_ERROR;
        Drain(gl);
        if (!limitsAccepted || maxImageUnits < 1 || maxGeometryImages < 1) {
            MGLOG_I("[driver-bug] image-location-per-name probe reached no verdict (this context "
                    "has no geometry-stage image uniforms to build the shape out of)");
            return measurement;
        }

        // One more than the smallest of the three stages' budgets. Below that, a driver with
        // per-name accounting has no reason to refuse anything; above it, the CONTROL stops
        // linking too and the pair stops being a comparison.
        const Int perStage = static_cast<Int>(maxGeometryImages) + 1;
        if (perStage > maxVertexImages || perStage > maxFragmentImages) {
            MGLOG_I("[driver-bug] image-location-per-name probe reached no verdict (the geometry "
                    "stage's own image budget, %d, is not smaller than the vertex/fragment budgets "
                    "%d/%d, so no shape exceeds one stage without exceeding them all)",
                    static_cast<int>(maxGeometryImages), static_cast<int>(maxVertexImages),
                    static_cast<int>(maxFragmentImages));
            return measurement;
        }

        measurement.perStageImageUniforms = perStage;
        measurement.subjectDistinctNames = perStage * 3;
        measurement.controlDistinctNames = perStage;

        // Same stage count, same per-stage image count, same bindings, same stores. The names
        // are the only difference between these two programs.
        ProgramBuild subject =
            BuildThreeStageImageProgram(gl, perStage, static_cast<Int>(maxImageUnits), "mg_probeVsImage",
                                        "mg_probeGsImage", "mg_probeFsImage");
        ProgramBuild control =
            BuildThreeStageImageProgram(gl, perStage, static_cast<Int>(maxImageUnits),
                                        "mg_probeSharedImage", "mg_probeSharedImage",
                                        "mg_probeSharedImage");
        if (subject.compiled && control.compiled) {
            measurement.driverMessage = FirstLine(subject.infoLog);
            // Both failing is an honest refusal of a shape that is simply too big; both linking
            // is a driver that does not have this bug.
            measurement.detected = !subject.linked && control.linked;
            MGLOG_I("[driver-bug] image-location-per-name probe: %d image uniform(s) per stage over "
                    "%d image unit(s); %d distinct names %s, %d shared names %s%s%s",
                    static_cast<int>(perStage), static_cast<int>(maxImageUnits),
                    static_cast<int>(measurement.subjectDistinctNames),
                    subject.linked ? "link" : "FAIL", static_cast<int>(measurement.controlDistinctNames),
                    control.linked ? "link" : "FAIL",
                    measurement.driverMessage.empty() ? "" : "; driver says: ",
                    measurement.driverMessage.c_str());
        } else {
            MGLOG_I("[driver-bug] image-location-per-name probe reached no verdict (a probe stage "
                    "did not compile)");
        }

        if (subject.program != 0) gl.glDeleteProgram(subject.program);
        if (control.program != 0) gl.glDeleteProgram(control.program);
        Drain(gl);
        return measurement;
    }

    const ImageLocationBudgetMeasurement& ImageLocationPerNameBudget(const GLESFunctionsTable& gl) {
        static const ImageLocationBudgetMeasurement measurement = ProbeImageLocationPerNameBudget(gl);
        return measurement;
    }

    Bool ProbeCrossStageImageQualifierMergeDropsWrites(const GLESFunctionsTable& gl) {
        if (!HasImageDrawEntryPoints(gl)) return false;

        Drain(gl);
        GLint maxImageUnits = 0;
        GLint maxVertexImages = 0;
        GLint maxFragmentImages = 0;
        gl.glGetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);
        gl.glGetIntegerv(GL_MAX_VERTEX_IMAGE_UNIFORMS, &maxVertexImages);
        gl.glGetIntegerv(GL_MAX_FRAGMENT_IMAGE_UNIFORMS, &maxFragmentImages);
        const Bool limitsAccepted = gl.glGetError() == GL_NO_ERROR;
        Drain(gl);
        if (!limitsAccepted || maxVertexImages < 1 || maxFragmentImages < 1 ||
            maxImageUnits <= static_cast<GLint>(kProbeImageUnit)) {
            return false;
        }

        SavedState saved;
        Save(gl, saved);
        Drain(gl);

        Bool dropped = false;
        const char* inconclusive = nullptr;
        GLuint vertexArray = 0;
        ImageDrawTargets targets;
        Int subjectFailed = -1;
        Int controlFailed = -1;

        do {
            gl.glGenVertexArrays(1, &vertexArray);
            gl.glBindVertexArray(vertexArray);
            PrepareForProbeDraw(gl);
            targets = MakeImageDrawTargets(gl, kQualifierMergeSize);
            if (!targets.valid) {
                inconclusive = "the probe's own rgba32f image and RGBA8 target would not come up";
                break;
            }

            subjectFailed = RunQualifierMergeCase(gl, targets, "mg_probeImage", "mg_probeImage");
            // The control names the two halves the way MobileGL's image-uniform repair does.
            controlFailed = RunQualifierMergeCase(gl, targets, "mg_imageWo_mg_probeImage",
                                                  "mg_imageRo_mg_probeImage");
            if (subjectFailed < 0 || controlFailed < 0) {
                inconclusive = "one of the two programs could not be built or drawn";
                break;
            }
            if (controlFailed > 0) {
                inconclusive = "the RENAMED control lost the store too, so this driver does not "
                               "perform vertex-stage image writes at all - a different and much "
                               "larger claim than a same-name merge";
                break;
            }
            dropped = subjectFailed > 0;

            MGLOG_I("[driver-bug] cross-stage image qualifier merge probe: same-name pair lost the "
                    "store for %d/%d texels, renamed control lost %d%s",
                    static_cast<int>(subjectFailed),
                    static_cast<int>(kQualifierMergeSize) * static_cast<int>(kQualifierMergeSize),
                    static_cast<int>(controlFailed),
                    dropped ? " - THE MERGED WRITE IS DISCARDED" : "");
        } while (false);

        if (inconclusive != nullptr) {
            MGLOG_I("[driver-bug] cross-stage image qualifier merge probe reached no verdict (%s)",
                    inconclusive);
        }

        ReleaseImageDrawTargets(gl, targets);
        if (vertexArray != 0) gl.glDeleteVertexArrays(1, &vertexArray);

        Restore(gl, saved);
        return dropped;
    }

    Bool CrossStageImageQualifierMergeDropsWrites(const GLESFunctionsTable& gl) {
        static const Bool dropped = ProbeCrossStageImageQualifierMergeDropsWrites(gl);
        return dropped;
    }

    ImageCoherencyResidualMeasurement ProbeImageWriteReadCoherencyResidual(
        const GLESFunctionsTable& gl) {
        ImageCoherencyResidualMeasurement measurement;
        if (!HasImageDrawEntryPoints(gl)) return measurement;

        Drain(gl);
        GLint maxImageUnits = 0;
        GLint maxFragmentImages = 0;
        gl.glGetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);
        gl.glGetIntegerv(GL_MAX_FRAGMENT_IMAGE_UNIFORMS, &maxFragmentImages);
        const Bool limitsAccepted = gl.glGetError() == GL_NO_ERROR;
        Drain(gl);
        // The subject declares BOTH halves of the split pair in the fragment stage.
        if (!limitsAccepted || maxFragmentImages < 2 ||
            maxImageUnits <= static_cast<GLint>(kProbeImageUnit)) {
            return measurement;
        }

        SavedState saved;
        Save(gl, saved);
        Drain(gl);

        const char* inconclusive = nullptr;
        GLuint vertexArray = 0;
        ImageDrawTargets targets;
        Int subjectFailed = -1;
        Int emittedFailed = -1;
        Int controlFailed = -1;

        do {
            gl.glGenVertexArrays(1, &vertexArray);
            gl.glBindVertexArray(vertexArray);
            PrepareForProbeDraw(gl);
            targets = MakeImageDrawTargets(gl, kCoherencySize);
            if (!targets.valid) {
                inconclusive = "the probe's own rgba32f image and RGBA8 target would not come up";
                break;
            }

            subjectFailed = RunCoherencyShape(gl, targets, kStrongestCoherencyQualifiers,
                                              kStrongestCoherencyBarriers);
            emittedFailed =
                RunCoherencyShape(gl, targets, kEmittedCoherencyQualifiers, kEmittedCoherencyBarriers);
            controlFailed = RunCoherencyControl(gl, targets);
            if (subjectFailed < 0 || emittedFailed < 0 || controlFailed < 0) {
                inconclusive = "one of the three programs could not be built or drawn";
                break;
            }
            if (controlFailed > 0) {
                inconclusive = "the glFinish-separated control was dirty too, so this driver does "
                               "not make image writes visible across draws either - a different "
                               "and much larger claim than an in-invocation ordering residual";
                break;
            }
            measurement.totalTexels = static_cast<Int>(kCoherencySize) * static_cast<Int>(kCoherencySize);
            measurement.mismatchedTexels = subjectFailed;
            measurement.emittedShapeMismatchedTexels = emittedFailed;
            // Only the STRONGEST shape decides. A driver that gets it right there but not in the
            // shape MobileGL emits has a fixable defect, not an unfixable one, and this section
            // is not where that belongs.
            measurement.detected = subjectFailed > 0;

            MGLOG_I("[driver-bug] image write-read coherency probe: the strongest in-shader shape "
                    "(coherent volatile + image and global barriers) missed its own store for "
                    "%d/%d texels; the shape MobileGL emits (coherent + memoryBarrierImage) missed "
                    "%d; the glFinish-separated two-draw control missed %d%s",
                    static_cast<int>(subjectFailed), static_cast<int>(measurement.totalTexels),
                    static_cast<int>(emittedFailed), static_cast<int>(controlFailed),
                    measurement.detected ? " - THE IN-INVOCATION ORDERING IS NOT HONOURED" : "");
        } while (false);

        if (inconclusive != nullptr) {
            MGLOG_I("[driver-bug] image write-read coherency probe reached no verdict (%s)",
                    inconclusive);
        }

        ReleaseImageDrawTargets(gl, targets);
        if (vertexArray != 0) gl.glDeleteVertexArrays(1, &vertexArray);

        Restore(gl, saved);
        return measurement;
    }

    const ImageCoherencyResidualMeasurement& ImageWriteReadCoherencyResidual(
        const GLESFunctionsTable& gl) {
        static const ImageCoherencyResidualMeasurement measurement =
            ProbeImageWriteReadCoherencyResidual(gl);
        return measurement;
    }

    namespace {
        // ===================== EXPLICIT VERTEX INPUT LOCATION CEILING =====================

        constexpr const char* kAttributeLocationProbeName = "explicit vertex input location";

        // COMPILES ONE VERTEX STAGE and reports nothing else. A link would drag in every other
        // reason a program can be refused (varying budgets, the fragment stage, the linker's own
        // location rules), and the defect this measures is in the driver's ESSL COMPILER: it
        // rejects the declaration itself, before any of that can matter.
        Bool ExplicitVertexInputLocationCompiles(const GLESFunctionsTable& gl, Int location,
                                                 String* firstRejectionMessage) {
            const String source = format("#version 320 es\n"
                                         "layout(location = {}) in vec4 a_probe;\n"
                                         "void main() {{ gl_Position = a_probe; }}\n",
                                         location);
            Drain(gl);
            const GLuint shader = gl.glCreateShader(GL_VERTEX_SHADER);
            if (shader == 0) return false;
            const char* text = source.c_str();
            gl.glShaderSource(shader, 1, &text, nullptr);
            gl.glCompileShader(shader);
            GLint compiled = GL_FALSE;
            gl.glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (compiled == GL_FALSE && firstRejectionMessage != nullptr && firstRejectionMessage->empty()) {
                char log[256] = {0};
                gl.glGetShaderInfoLog(shader, static_cast<GLsizei>(sizeof(log) - 1), nullptr, log);
                // One line: the driver's own wording is the report's whole evidential value, and
                // the rest of the log is the same sentence repeated per declaration.
                String message = log;
                if (const SizeT newline = message.find('\n'); newline != String::npos) {
                    message.resize(newline);
                }
                while (!message.empty() && (message.back() == ' ' || message.back() == '\r')) message.pop_back();
                *firstRejectionMessage = Move(message);
            }
            gl.glDeleteShader(shader);
            Drain(gl);
            return compiled != GL_FALSE;
        }

        // THE SECOND CONTROL, and the one that decides whether the cap is about the LAYOUT
        // QUALIFIER or about the attribute itself. The same input, declared with no qualifier at
        // all and placed by glBindAttribLocation instead. If this links and glGetAttribLocation
        // answers with the location asked for, the driver can address that attribute perfectly
        // well and only the qualifier path is capped - which is what makes clamping the
        // advertised count the right response rather than a shrug. If it fails too, the driver
        // genuinely has fewer attributes than it advertises; the clamp is still correct, but the
        // report must not claim the attribute is reachable another way.
        Bool BindAttribLocationReaches(const GLESFunctionsTable& gl, Int location) {
            if (!gl.glCreateProgram || !gl.glAttachShader || !gl.glBindAttribLocation || !gl.glLinkProgram ||
                !gl.glGetProgramiv || !gl.glGetAttribLocation || !gl.glDeleteProgram) {
                return false;
            }
            constexpr const char* kVertexSource = "#version 320 es\n"
                                                  "in vec4 a_probe;\n"
                                                  "void main() { gl_Position = a_probe; }\n";
            constexpr const char* kFragmentSource = "#version 320 es\n"
                                                    "precision highp float;\n"
                                                    "out vec4 o_color;\n"
                                                    "void main() { o_color = vec4(1.0); }\n";
            Drain(gl);
            const GLuint vertexShader =
                CompileStage(gl, GL_VERTEX_SHADER, kVertexSource, "vertex", kAttributeLocationProbeName);
            if (vertexShader == 0) return false;
            const GLuint fragmentShader =
                CompileStage(gl, GL_FRAGMENT_SHADER, kFragmentSource, "fragment", kAttributeLocationProbeName);
            if (fragmentShader == 0) {
                gl.glDeleteShader(vertexShader);
                return false;
            }
            const GLuint program = gl.glCreateProgram();
            gl.glAttachShader(program, vertexShader);
            gl.glAttachShader(program, fragmentShader);
            gl.glBindAttribLocation(program, static_cast<GLuint>(location), "a_probe");
            gl.glLinkProgram(program);
            GLint linked = GL_FALSE;
            gl.glGetProgramiv(program, GL_LINK_STATUS, &linked);
            const Bool reached = linked != GL_FALSE && gl.glGetAttribLocation(program, "a_probe") == location;
            gl.glDeleteShader(vertexShader);
            gl.glDeleteShader(fragmentShader);
            gl.glDeleteProgram(program);
            Drain(gl);
            return reached;
        }
    } // namespace

    VertexInputLocationCeilingMeasurement ProbeExplicitVertexInputLocationCeiling(const GLESFunctionsTable& gl) {
        VertexInputLocationCeilingMeasurement measurement;
        // `usableLocations` is the number a caller clamps to, so it carries the driver's own
        // answer from the first line onward and every early return below leaves it there. A
        // probe that cannot run has to withdraw nothing at all, and a zero here would withdraw
        // every attribute the device has.
        if (gl.glGetIntegerv != nullptr) {
            GLint advertisedEarly = 0;
            gl.glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &advertisedEarly);
            if (gl.glGetError != nullptr) Drain(gl);
            measurement.advertisedMaxVertexAttribs = advertisedEarly;
            measurement.usableLocations = advertisedEarly;
        }
        if (!gl.glCreateShader || !gl.glShaderSource || !gl.glCompileShader || !gl.glGetShaderiv ||
            !gl.glGetShaderInfoLog || !gl.glDeleteShader || !gl.glGetIntegerv || !gl.glGetError) {
            return measurement;
        }

        const GLint advertised = measurement.advertisedMaxVertexAttribs;
        // Nothing to bisect, and nothing a clamp could usefully say.
        if (advertised < 2) return measurement;

        // THE CONTROL, and the reason a compiler that is simply unavailable cannot be reported as
        // this bug: location 0 is the one every ES driver in existence accepts, so a probe that
        // cannot compile even that has measured its own failure, not the driver's.
        if (!ExplicitVertexInputLocationCompiles(gl, 0, nullptr)) {
            MGLOG_I("[driver-bug] %s probe reached no verdict (the location-0 control did not "
                    "compile, so nothing higher says anything)",
                    kAttributeLocationProbeName);
            return measurement;
        }

        // The common case is one compile: a conforming driver takes the highest location it
        // advertises and the probe stops there.
        if (ExplicitVertexInputLocationCompiles(gl, advertised - 1, nullptr)) return measurement;

        // Bisect for the highest location that still compiles. `low` always compiles (the control
        // proved location 0 does) and `high` never does, so the loop closes on the boundary in
        // ceil(log2(advertised)) compiles - five for the 32 attributes Adreno advertises.
        String rejectionMessage;
        ExplicitVertexInputLocationCompiles(gl, advertised - 1, &rejectionMessage);
        Int low = 0;
        Int high = advertised - 1;
        while (high - low > 1) {
            const Int middle = low + (high - low) / 2;
            if (ExplicitVertexInputLocationCompiles(gl, middle, &rejectionMessage)) {
                low = middle;
            } else {
                high = middle;
            }
        }

        measurement.detected = true;
        measurement.usableLocations = low + 1;
        measurement.driverMessage = Move(rejectionMessage);
        measurement.bindAttribLocationReachesAdvertisedMax = BindAttribLocationReaches(gl, advertised - 1);
        MGLOG_I("[driver-bug] %s probe: GL_MAX_VERTEX_ATTRIBS is %d but layout(location = N) on a "
                "vertex input is refused from N = %d upward - only %d location(s) are usable; "
                "glBindAttribLocation(%d) %s%s%s",
                kAttributeLocationProbeName, advertised, measurement.usableLocations,
                measurement.usableLocations, advertised - 1,
                measurement.bindAttribLocationReachesAdvertisedMax ? "still resolves correctly"
                                                                   : "does not resolve either",
                measurement.driverMessage.empty() ? "" : "; the driver says: ",
                measurement.driverMessage.c_str());
        return measurement;
    }

    const VertexInputLocationCeilingMeasurement& ExplicitVertexInputLocationCeiling(const GLESFunctionsTable& gl) {
        static const VertexInputLocationCeilingMeasurement measurement =
            ProbeExplicitVertexInputLocationCeiling(gl);
        return measurement;
    }

    namespace {
        Optional<DriverBugFinding> ProbeExplicitVertexInputLocationCeilingBug(const GLESFunctionsTable& gl) {
            const VertexInputLocationCeilingMeasurement& measurement = ExplicitVertexInputLocationCeiling(gl);
            if (!measurement.detected) return std::nullopt;
            String detail =
                format("GL_MAX_VERTEX_ATTRIBS is {} but the ESSL compiler refuses "
                       "layout(location = N) on a vertex input for every N at or above {}, for float "
                       "and integer inputs alike - so {} of the {} attributes advertised cannot be "
                       "declared at all",
                       measurement.advertisedMaxVertexAttribs, measurement.usableLocations,
                       measurement.advertisedMaxVertexAttribs - measurement.usableLocations,
                       measurement.advertisedMaxVertexAttribs);
            if (!measurement.driverMessage.empty()) {
                detail += format(" - the driver says \"{}\"", measurement.driverMessage);
            }
            detail += measurement.bindAttribLocationReachesAdvertisedMax
                          ? format(". The same driver ACCEPTS glBindAttribLocation({}) on an unqualified "
                                   "input and resolves it correctly, so the attributes are there and only "
                                   "the layout qualifier is capped",
                                   measurement.advertisedMaxVertexAttribs - 1)
                          : ". glBindAttribLocation does not reach those locations either, so the "
                            "attributes appear genuinely absent rather than merely unspellable";
            detail += format(". MobileGL emits its vertex inputs as layout qualifiers, so it advertises the "
                             "{} locations it can actually deliver rather than the {} the driver claims. An "
                             "application asking for more used to be handed a count it could not build a "
                             "shader against, which failed at the stage compile with no way back",
                             measurement.usableLocations, measurement.advertisedMaxVertexAttribs);
            return DriverBugFinding{"Vertex input layout(location) capped below GL_MAX_VERTEX_ATTRIBS",
                                    DriverBugVerdict::Fixed, Move(detail)};
        }

        Optional<DriverBugFinding> ProbeGeometryWriteAfterEmitBug(const GLESFunctionsTable& gl) {
            if (!GeometryStageSsboWriteAfterEmitDropped(gl)) return std::nullopt;
            return DriverBugFinding{
                "Geometry-stage storage writes after EmitVertex",
                DriverBugVerdict::Unfixable,
                "the driver silently discards shader storage buffer writes a geometry shader "
                "issues after its last EmitVertex()/EndPrimitive(); the identical write issued "
                "BEFORE the emit lands, for both point and triangle geometry shaders. "
                "GL_MAX_GEOMETRY_SHADER_STORAGE_BLOCKS is therefore NOT withdrawn - doing so "
                "would break the shaders that write before emitting, which work correctly. "
                "A shader that must write after emitting has no substitute on this driver"};
        }

        Optional<DriverBugFinding> ProbeR32FMultisampleSwizzleBug(const GLESFunctionsTable& gl) {
            if (!R32FMultisampleSwizzleCorrupted(gl)) return std::nullopt;
            return DriverBugFinding{
                "R32F multisample fetch through a swizzled alpha",
                DriverBugVerdict::Unfixable,
                "texelFetch() on an R32F GL_TEXTURE_2D_MULTISAMPLE texture whose "
                "GL_TEXTURE_SWIZZLE_A is not the default returns uninitialised memory - a "
                "different value every run - for any sample index other than 0, from the SECOND "
                "such program in the context onward. The identical fetch with the default alpha "
                "swizzle, at sample index 0, or through .x instead of .w is correct, so neither "
                "R32F multisample targets nor texture swizzles are withdrawn. No substitute was "
                "adopted: widening every R32F multisample target to RG32F would sidestep it and "
                "doubles multisample memory, which was declined. A shader that fetches a non-zero "
                "sample through a swizzled alpha cannot be relied on here"};
        }

        Optional<DriverBugFinding> ProbeImageLocationPerNameBug(const GLESFunctionsTable& gl) {
            const ImageLocationBudgetMeasurement& measurement = ImageLocationPerNameBudget(gl);
            if (!measurement.detected) return std::nullopt;
            String detail = format(
                "this driver's image-location budget is charged per distinct uniform NAME, not per "
                "image unit: a vertex+geometry+fragment program declaring {} image uniform(s) per "
                "stage links when all three stages share one set of names ({} distinct) and is "
                "rejected when each stage names its own copies ({} distinct), with the same "
                "bindings, the same qualifiers and the same stores either way",
                measurement.perStageImageUniforms, measurement.controlDistinctNames,
                measurement.subjectDistinctNames);
            if (!measurement.driverMessage.empty()) {
                detail += format(" - the driver says \"{}\"", measurement.driverMessage);
            }
            detail +=
                ". MobileGL names a repaired image uniform after the REPAIR rather than after the "
                "stage, so stages that use an image alike keep one shared name and stay merged; "
                "applications never see the link failure";
            return DriverBugFinding{"Image locations charged per uniform name",
                                    DriverBugVerdict::Fixed, Move(detail)};
        }

        Optional<DriverBugFinding> ProbeCrossStageImageQualifierMergeBug(const GLESFunctionsTable& gl) {
            if (!CrossStageImageQualifierMergeDropsWrites(gl)) return std::nullopt;
            return DriverBugFinding{
                "Cross-stage image qualifier merge discards writes",
                DriverBugVerdict::Fixed,
                "an image declared `coherent writeonly` in one stage and `coherent readonly` in "
                "another under the SAME name is merged into one uniform whose writing stage's "
                "imageStore()s are then silently discarded - every fragment reads the "
                "pre-store value. The identical shader pair with the two halves renamed keeps "
                "every store, which is what proves the stage's image writes work and only the "
                "merge is at fault. MobileGL renames the two halves (mg_imageWo_ / mg_imageRo_) so "
                "the driver cannot merge them, and application behaviour is correct"};
        }

        Optional<DriverBugFinding> ProbeImageCoherencyResidualBug(const GLESFunctionsTable& gl) {
            const ImageCoherencyResidualMeasurement& measurement = ImageWriteReadCoherencyResidual(gl);
            if (!measurement.detected) return std::nullopt;
            const auto percentOf = [&](Int texels) {
                return measurement.totalTexels > 0 ? 100.0 * texels / measurement.totalTexels : 0.0;
            };
            return DriverBugFinding{
                "Image write-to-read ordering within one invocation",
                DriverBugVerdict::Unfixable,
                format(
                    "an imageLoad() does not observe the imageStore() that precedes it in the same "
                    "fragment invocation for {} of {} texels ({:.2f}%) under the STRONGEST shape "
                    "ESSL offers - a `coherent volatile` readonly/writeonly pair on one binding "
                    "with both memoryBarrierImage() and memoryBarrier() between the store and the "
                    "read - which is what leaves nothing to substitute. The shape MobileGL emits "
                    "today (`coherent` plus memoryBarrierImage()) misses {} ({:.2f}%) on this "
                    "driver. The same dependency split across two draws with a glMemoryBarrier and "
                    "a glFinish between them is clean, so writes do become visible; it is the "
                    "ordering inside one invocation that is not honoured. MobileGL keeps the "
                    "coherent pair and the barrier - without them every texel is wrong - and a "
                    "shader that reads back its own image write within one invocation cannot be "
                    "relied on here",
                    measurement.mismatchedTexels, measurement.totalTexels,
                    percentOf(measurement.mismatchedTexels), measurement.emittedShapeMismatchedTexels,
                    percentOf(measurement.emittedShapeMismatchedTexels))};
        }

        // The table. One row per known driver bug; see the header for how to add a sibling.
        using DriverBugProbeFn = Optional<DriverBugFinding> (*)(const GLESFunctionsTable&);
        constexpr DriverBugProbeFn kGlesDriverBugProbes[] = {
            &ProbeGeometryWriteAfterEmitBug,
            &ProbeR32FMultisampleSwizzleBug,
            &ProbeImageLocationPerNameBug,
            &ProbeCrossStageImageQualifierMergeBug,
            &ProbeImageCoherencyResidualBug,
            &ProbeExplicitVertexInputLocationCeilingBug,
        };
    } // namespace

    Vector<DriverBugFinding> CollectGlesKnownDriverBugs(const GLESFunctionsTable& gl) {
        Vector<DriverBugFinding> findings;
        for (const DriverBugProbeFn probe : kGlesDriverBugProbes) {
            if (Optional<DriverBugFinding> finding = probe(gl)) {
                findings.push_back(Move(*finding));
            }
        }
        return findings;
    }
} // namespace MobileGL::MG_Util::SelfTest
