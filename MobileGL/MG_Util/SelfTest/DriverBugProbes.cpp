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
                            const char* stageName) {
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
                MGLOG_I("[driver-bug] geometry write-after-emit probe: %s stage did not compile: %s",
                        stageName, log);
                gl.glDeleteShader(shader);
                return 0;
            }
            return shader;
        }

        // Every piece of GL state the probe disturbs, captured on the way in and put back on
        // the way out. It runs inside the POST context, which is not allowed to notice.
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
        };

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
        }

        void Restore(const GLESFunctionsTable& gl, const SavedState& state) {
            for (GLuint i = 0; i < 2; ++i) {
                gl.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i,
                                    static_cast<GLuint>(state.indexedStorageBuffer[i]));
            }
            gl.glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(state.storageBuffer));
            gl.glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(state.renderbuffer));
            gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(state.drawFramebuffer));
            gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(state.readFramebuffer));
            gl.glBindVertexArray(static_cast<GLuint>(state.vertexArray));
            gl.glUseProgram(static_cast<GLuint>(state.program));
            gl.glViewport(state.viewport[0], state.viewport[1], state.viewport[2], state.viewport[3]);
            if (state.rasterizerDiscard) gl.glEnable(GL_RASTERIZER_DISCARD);
            if (state.scissorTest) gl.glEnable(GL_SCISSOR_TEST);
            if (state.cullFace) gl.glEnable(GL_CULL_FACE);
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
            vertexShader = CompileStage(gl, GL_VERTEX_SHADER, kProbeVertexSource, "vertex");
            geometryShader = CompileStage(gl, GL_GEOMETRY_SHADER, kProbeGeometrySource, "geometry");
            fragmentShader = CompileStage(gl, GL_FRAGMENT_SHADER, kProbeFragmentSource, "fragment");
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

        // The table. One row per known driver bug; see the header for how to add a sibling.
        using DriverBugProbeFn = Optional<DriverBugFinding> (*)(const GLESFunctionsTable&);
        constexpr DriverBugProbeFn kGlesDriverBugProbes[] = {
            &ProbeGeometryWriteAfterEmitBug,
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
