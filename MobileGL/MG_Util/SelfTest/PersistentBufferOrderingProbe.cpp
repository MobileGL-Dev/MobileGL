// MobileGL - MobileGL/MG_Util/SelfTest/PersistentBufferOrderingProbe.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "PersistentBufferOrderingProbe.h"
#include <MG_Util/Debug/Log.h>

#include <array>
#include <cmath>
#include <cstring>
#include <new>

namespace MobileGL::MG_Util::SelfTest {
    namespace {
        using MG_External::GLESFunctionsTable;
        constexpr GLbitfield kPersistent = 0x0040;
        constexpr GLbitfield kCoherent = 0x0080;
        constexpr GLbitfield kDynamicStorage = 0x0100;
        constexpr GLbitfield kMapFlags = GL_MAP_WRITE_BIT | kPersistent | kCoherent;
        constexpr GLsizeiptr kArenaSize = 128 * 1024 * 1024;
        constexpr GLintptr kOffset = 96 * 1024 * 1024 + 28;
        constexpr GLsizei kSide = 128;
        constexpr GLsizei kSlots = 8;
        constexpr Int kBatches = 10;
        constexpr Int kDraws = 32;
        constexpr GLsizei kQuads = 64 * 32;
        constexpr Int kAttempts = 3;
        constexpr std::array<const char*, 3> kUploadNames = {"SubData", "Copy/persistent staging",
                                                          "Copy/SubData staging"};
        enum class Shape { Unmapped, Mapped, FinishBefore, FinishBoth, MapThenUnmap, BarrierBefore };
        struct Vertex { GLfloat x, y, r, g, b; };
        constexpr GLsizeiptr kPayloadSize = kQuads * 6 * sizeof(Vertex);
        static_assert(kOffset + kPayloadSize <= kArenaSize);

        void DrainErrors(const GLESFunctionsTable& gl) {
            for (Int i = 0; i < 32 && gl.glGetError() != GL_NO_ERROR; ++i) {}
        }

        Bool CanProbe(const GLESFunctionsTable& gl) {
            if (!(gl.glGetIntegerv && gl.glGetBooleanv && gl.glGetFloatv && gl.glGetError &&
                  gl.glGetStringi && gl.glIsEnabled && gl.glEnable && gl.glDisable &&
                  gl.glCreateShader && gl.glShaderSource && gl.glCompileShader && gl.glGetShaderiv &&
                  gl.glGetShaderInfoLog && gl.glDeleteShader && gl.glCreateProgram && gl.glAttachShader &&
                  gl.glLinkProgram && gl.glGetProgramiv && gl.glGetProgramInfoLog && gl.glDeleteProgram &&
                  gl.glUseProgram && gl.glGenBuffers && gl.glBindBuffer && gl.glBufferStorageEXT &&
                  gl.glMapBufferRange && gl.glUnmapBuffer && gl.glBufferData && gl.glBufferSubData &&
                  gl.glCopyBufferSubData && gl.glDeleteBuffers && gl.glGenVertexArrays &&
                  gl.glBindVertexArray && gl.glVertexAttribPointer && gl.glEnableVertexAttribArray &&
                  gl.glDeleteVertexArrays && gl.glGenTextures && gl.glBindTexture && gl.glTexStorage2D &&
                  gl.glDeleteTextures && gl.glGenFramebuffers && gl.glBindFramebuffer &&
                  gl.glFramebufferTexture2D && gl.glCheckFramebufferStatus && gl.glDeleteFramebuffers &&
                  gl.glViewport && gl.glColorMask && gl.glClearColor && gl.glClear && gl.glDrawArrays &&
                  gl.glFinish && gl.glMemoryBarrier && gl.glPixelStorei && gl.glReadPixels)) return false;
            DrainErrors(gl);
            GLint major = 0, minor = 0, count = 0;
            gl.glGetIntegerv(GL_MAJOR_VERSION, &major);
            gl.glGetIntegerv(GL_MINOR_VERSION, &minor);
            gl.glGetIntegerv(GL_NUM_EXTENSIONS, &count);
            if (gl.glGetError() != GL_NO_ERROR || major < 3 || (major == 3 && minor < 1)) return false;
            for (GLint i = 0; i < count; ++i) {
                const auto* extension = gl.glGetStringi(GL_EXTENSIONS, i);
                if (extension && std::strcmp(reinterpret_cast<const char*>(extension),
                                             "GL_EXT_buffer_storage") == 0) return true;
            }
            return false;
        }

        // This probe touches no images/SSBO bindings. Keep its scope independent from the
        // other POST probes, including pack state and the caller's currently active texture unit.
        struct StateScope {
            const GLESFunctionsTable& gl;
            GLint program = 0, vao = 0, array = 0, copyRead = 0, copyWrite = 0;
            GLint drawFbo = 0, readFbo = 0, texture = 0, packBuffer = 0;
            GLint viewport[4]{};
            GLfloat clear[4]{};
            GLboolean colorMask[4]{};
            static constexpr std::array<GLenum, 10> enables = {
                GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE, GL_SCISSOR_TEST,
                GL_RASTERIZER_DISCARD, GL_DITHER, GL_SAMPLE_ALPHA_TO_COVERAGE,
                GL_SAMPLE_COVERAGE, GL_SAMPLE_MASK};
            static constexpr std::array<GLenum, 4> packNames = {
                GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS};
            std::array<GLboolean, enables.size()> enabled{};
            std::array<GLint, packNames.size()> pack{};

            explicit StateScope(const GLESFunctionsTable& api) : gl(api) {
                gl.glGetIntegerv(GL_CURRENT_PROGRAM, &program);
                gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
                gl.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array);
                gl.glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &copyRead);
                gl.glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &copyWrite);
                gl.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
                gl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
                gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
                gl.glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
                gl.glGetIntegerv(GL_VIEWPORT, viewport);
                gl.glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
                gl.glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
                for (SizeT i = 0; i < enables.size(); ++i) enabled[i] = gl.glIsEnabled(enables[i]);
                for (SizeT i = 0; i < packNames.size(); ++i) gl.glGetIntegerv(packNames[i], &pack[i]);
            }
            void Prepare() {
                for (auto cap : enables) gl.glDisable(cap);
                gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                for (auto name : packNames) gl.glPixelStorei(name, name == GL_PACK_ALIGNMENT ? 1 : 0);
                gl.glViewport(0, 0, kSide, kSide);
                gl.glClearColor(0, 0, 0, 1);
            }
            ~StateScope() {
                gl.glUseProgram(program);
                gl.glBindVertexArray(vao);
                gl.glBindBuffer(GL_ARRAY_BUFFER, array);
                gl.glBindBuffer(GL_COPY_READ_BUFFER, copyRead);
                gl.glBindBuffer(GL_COPY_WRITE_BUFFER, copyWrite);
                gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
                gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
                gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
                gl.glBindTexture(GL_TEXTURE_2D, texture);
                gl.glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
                gl.glClearColor(clear[0], clear[1], clear[2], clear[3]);
                gl.glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
                for (SizeT i = 0; i < enables.size(); ++i) {
                    if (enabled[i]) gl.glEnable(enables[i]); else gl.glDisable(enables[i]);
                }
                for (SizeT i = 0; i < packNames.size(); ++i) gl.glPixelStorei(packNames[i], pack[i]);
            }
        };

        struct Resources {
            const GLESFunctionsTable& gl;
            GLuint program = 0, vao = 0;
            std::array<GLuint, kSlots> fbos{}, textures{};
            explicit Resources(const GLESFunctionsTable& api) : gl(api) {}
            ~Resources() {
                gl.glDeleteFramebuffers(kSlots, fbos.data());
                gl.glDeleteTextures(kSlots, textures.data());
                gl.glDeleteVertexArrays(1, &vao);
                if (program) gl.glDeleteProgram(program);
            }
            GLuint Compile(GLenum type, const char* source) {
                GLuint shader = gl.glCreateShader(type);
                if (!shader) return 0;
                gl.glShaderSource(shader, 1, &source, nullptr);
                gl.glCompileShader(shader);
                GLint compiled = 0;
                gl.glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (!compiled) {
                    char log[512]{};
                    gl.glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
                    MGLOG_I("[driver-bug] persistent buffer ordering: shader failed: %s", log);
                    gl.glDeleteShader(shader);
                    return 0;
                }
                return shader;
            }
            Bool Setup() {
                const GLuint vs = Compile(GL_VERTEX_SHADER,
                    "#version 310 es\nlayout(location=0) in vec2 pos; layout(location=1) in vec3 color;\n"
                    "out highp vec3 vColor; void main(){gl_Position=vec4(pos,0,1);vColor=color;}\n");
                const GLuint fs = Compile(GL_FRAGMENT_SHADER,
                    "#version 310 es\nprecision highp float; in highp vec3 vColor;\n"
                    "layout(location=0) out vec4 outColor; void main(){outColor=vec4(vColor,1);}\n");
                if (vs && fs) {
                    program = gl.glCreateProgram();
                    if (program) {
                        gl.glAttachShader(program, vs);
                        gl.glAttachShader(program, fs);
                        gl.glLinkProgram(program);
                    }
                }
                if (vs) gl.glDeleteShader(vs);
                if (fs) gl.glDeleteShader(fs);
                if (!program) return false;
                GLint linked = 0;
                gl.glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (!linked) {
                    char log[512]{};
                    gl.glGetProgramInfoLog(program, sizeof(log), nullptr, log);
                    MGLOG_I("[driver-bug] persistent buffer ordering: link failed: %s", log);
                    return false;
                }
                gl.glUseProgram(program);
                gl.glGenVertexArrays(1, &vao);
                gl.glBindVertexArray(vao);
                gl.glGenFramebuffers(kSlots, fbos.data());
                gl.glGenTextures(kSlots, textures.data());
                for (Int i = 0; i < kSlots; ++i) {
                    if (!vao || !fbos[i] || !textures[i]) return false;
                    gl.glBindTexture(GL_TEXTURE_2D, textures[i]);
                    gl.glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kSide, kSide);
                    gl.glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
                    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                              textures[i], 0);
                    if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
                }
                return gl.glGetError() == GL_NO_ERROR;
            }
        };

        struct Buffers {
            const GLESFunctionsTable& gl;
            GLuint arena = 0, staging = 0;
            explicit Buffers(const GLESFunctionsTable& api) : gl(api) {}
            ~Buffers() {
                // All normal batches finish before cleanup; also retire a partially queued
                // batch on an error path before destroying a mapped staging source.
                gl.glFinish();
                gl.glDeleteBuffers(1, &arena);
                gl.glDeleteBuffers(1, &staging);
            }
        };

        void FillVertices(Vector<Vertex>& vertices, Int channel) {
            constexpr std::array<Vertex, 6> quad = {{{-1,-1,0,0,0}, {1,-1,0,0,0}, {1,1,0,0,0},
                                                    {-1,-1,0,0,0}, {1,1,0,0,0}, {-1,1,0,0,0}}};
            for (SizeT k = 0; k < vertices.size(); ++k) {
                auto& v = vertices[k];
                v = quad[k % 6];
                const SizeT q = k / 6;
                v.x = v.x / 64.f - 1.f + (2 * (q % 64) + 1) / 64.f;
                v.y = v.y / 32.f - 1.f + (2 * (q / 64) + 1) / 32.f;
                v.r = channel == 0 ? 1.f : 0.f;
                v.g = channel == 1 ? 1.f : 0.f;
                v.b = channel == 2 ? 1.f : 0.f;
            }
        }

        BufferOrderingSample Run(const GLESFunctionsTable& gl, const Resources& resources,
                                 const Vector<Uint8>& seed, Int upload, Shape shape) {
            BufferOrderingSample sample;
            sample.status = BufferOrderingProbeStatus::Failed;
            Buffers buffers(gl);
            Vector<Vertex> payload(kQuads * 6);
            Vector<Uint8> pixels(kSide * kSide * 4);
            DrainErrors(gl);
            do {
                gl.glGenBuffers(1, &buffers.arena);
                if (!buffers.arena) break;
                gl.glBindBuffer(GL_ARRAY_BUFFER, buffers.arena);
                gl.glBufferStorageEXT(GL_ARRAY_BUFFER, kArenaSize, seed.data(), kMapFlags | kDynamicStorage);
                sample.error = gl.glGetError();
                if (sample.error != GL_NO_ERROR) break;
                if (shape != Shape::Unmapped) {
                    // Deliberately never dereference the destination pointer. All destination
                    // writes below are ordered GL commands, with no client mapping accesses.
                    if (!gl.glMapBufferRange(GL_ARRAY_BUFFER, 0, kArenaSize, kMapFlags)) break;
                    if (shape == Shape::MapThenUnmap && !gl.glUnmapBuffer(GL_ARRAY_BUFFER)) break;
                }
                gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                          reinterpret_cast<const void*>(kOffset));
                gl.glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                          reinterpret_cast<const void*>(kOffset + 2 * sizeof(GLfloat)));
                gl.glEnableVertexAttribArray(0);
                gl.glEnableVertexAttribArray(1);
                void* sourceMap = nullptr;
                if (upload != 0) {
                    gl.glGenBuffers(1, &buffers.staging);
                    if (!buffers.staging) break;
                    gl.glBindBuffer(GL_COPY_READ_BUFFER, buffers.staging);
                    if (upload == 1) {
                        gl.glBufferStorageEXT(GL_COPY_READ_BUFFER, kSlots * kPayloadSize, nullptr, kMapFlags);
                        sourceMap = gl.glMapBufferRange(GL_COPY_READ_BUFFER, 0, kSlots * kPayloadSize, kMapFlags);
                        if (!sourceMap) break;
                    } else {
                        gl.glBufferData(GL_COPY_READ_BUFFER, kSlots * kPayloadSize, nullptr, GL_STREAM_DRAW);
                    }
                    gl.glBindBuffer(GL_COPY_WRITE_BUFFER, buffers.arena);
                }
                sample.error = gl.glGetError();
                if (sample.error != GL_NO_ERROR) break;
                for (Int batch = 0; batch < kBatches; ++batch) {
                    for (Int slot = 0; slot < kSlots; ++slot) {
                        FillVertices(payload, (batch * kSlots + slot) % 3);
                        if (shape == Shape::FinishBefore || shape == Shape::FinishBoth) gl.glFinish();
                        if (shape == Shape::BarrierBefore) gl.glMemoryBarrier(GL_ALL_BARRIER_BITS);
                        if (upload == 0) {
                            gl.glBufferSubData(GL_ARRAY_BUFFER, kOffset, kPayloadSize, payload.data());
                        } else {
                            // No slot is reused until the entire batch has finished on the GPU.
                            if (upload == 1) {
                                std::memcpy(static_cast<Uint8*>(sourceMap) + slot * kPayloadSize,
                                            payload.data(), kPayloadSize);
                            } else {
                                gl.glBufferSubData(GL_COPY_READ_BUFFER, slot * kPayloadSize,
                                                   kPayloadSize, payload.data());
                            }
                            gl.glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                                                   slot * kPayloadSize, kOffset, kPayloadSize);
                        }
                        if (shape == Shape::FinishBoth) gl.glFinish();
                        gl.glBindFramebuffer(GL_FRAMEBUFFER, resources.fbos[slot]);
                        gl.glClear(GL_COLOR_BUFFER_BIT);
                        for (Int draw = 0; draw < kDraws; ++draw) gl.glDrawArrays(GL_TRIANGLES, 0, kQuads * 6);
                    }
                    // No readback/Finish between subject update/draw pairs. Early readback
                    // would hide precisely the old-reader/new-writer overlap being tested.
                    gl.glFinish();
                    sample.error = gl.glGetError();
                    if (sample.error != GL_NO_ERROR) break;
                    for (Int slot = 0; slot < kSlots; ++slot) {
                        gl.glBindFramebuffer(GL_FRAMEBUFFER, resources.fbos[slot]);
                        gl.glReadPixels(0, 0, kSide, kSide, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                        sample.error = gl.glGetError();
                        if (sample.error != GL_NO_ERROR) break;
                        const Int channel = (batch * kSlots + slot) % 3;
                        Uint bad = 0;
                        for (Int pixel = 0; pixel < kSide * kSide; ++pixel) {
                            for (Int c = 0; c < 3; ++c) {
                                const Int expected = c == channel ? 255 : 0;
                                if (std::abs(Int(pixels[pixel * 4 + c]) - expected) > 8) ++bad;
                            }
                        }
                        ++sample.frames;
                        if (bad != 0) ++sample.badFrames;
                        sample.badComponents += bad;
                    }
                    if (sample.error != GL_NO_ERROR) break;
                }
                if (sample.error == GL_NO_ERROR && sample.frames == kBatches * kSlots)
                    sample.status = BufferOrderingProbeStatus::Complete;
            } while (false);
            if (sample.error == GL_NO_ERROR) sample.error = gl.glGetError();
            return sample;
        }

        String Describe(const BufferOrderingSample& sample) {
            if (sample.status == BufferOrderingProbeStatus::NotRun) return "not run";
            if (sample.status == BufferOrderingProbeStatus::Failed)
                return format("inconclusive (GL error 0x{:x}, {} readbacks)", sample.error, sample.frames);
            return format("{}/{} bad FBOs ({} components)", sample.badFrames, sample.frames, sample.badComponents);
        }

        String DescribeUpload(const BufferOrderingUploadMeasurement& row, Int upload) {
            return format("{}: mapped {}, never-mapped {}, Finish-before {}, Finish-both {}, "
                          "map-then-unmap {}, barrier-before {}", kUploadNames[upload], Describe(row.mapped),
                          Describe(row.unmapped), Describe(row.finishBefore), Describe(row.finishBoth),
                          Describe(row.mapThenUnmap), Describe(row.barrierBefore));
        }
    } // namespace

    PersistentBufferOrderingMeasurement ProbePersistentBufferUpdateOrdering(const GLESFunctionsTable& gl) try {
        PersistentBufferOrderingMeasurement measurement;
        if (!CanProbe(gl)) return measurement;
        measurement.supported = true;
        StateScope state(gl);
        state.Prepare();
        Resources resources(gl);
        if (gl.glGetError() != GL_NO_ERROR || !resources.Setup()) {
            for (auto& row : measurement.uploads) row.unmapped.status = BufferOrderingProbeStatus::Failed;
            MGLOG_I("[driver-bug] persistent buffer ordering: setup failed; inconclusive");
            return measurement;
        }
        Vector<Uint8> seed(kArenaSize, 0);
        for (Int upload = 0; upload < Int(measurement.uploads.size()); ++upload) {
            auto& row = measurement.uploads[upload];
            row.unmapped = Run(gl, resources, seed, upload, Shape::Unmapped);
            if (row.unmapped.Passed()) {
                // A single allocation can miss on Mali. Stop once a mismatch is measured,
                // otherwise retry with fresh storage rather than treating one pass as proof.
                for (Int attempt = 0; attempt < kAttempts; ++attempt) {
                    const auto sample = Run(gl, resources, seed, upload, Shape::Mapped);
                    row.mapped.status = sample.status;
                    row.mapped.error = sample.error;
                    row.mapped.frames += sample.frames;
                    row.mapped.badFrames += sample.badFrames;
                    row.mapped.badComponents += sample.badComponents;
                    if (sample.status != BufferOrderingProbeStatus::Complete || sample.badFrames) break;
                }
                if (row.mapped.status == BufferOrderingProbeStatus::Complete && row.mapped.badFrames) {
                    row.finishBoth = Run(gl, resources, seed, upload, Shape::FinishBoth);
                    row.finishBefore = Run(gl, resources, seed, upload, Shape::FinishBefore);
                    row.mapThenUnmap = Run(gl, resources, seed, upload, Shape::MapThenUnmap);
                    row.barrierBefore = Run(gl, resources, seed, upload, Shape::BarrierBefore);
                }
            }
            MGLOG_I("[driver-bug] persistent buffer ordering: %s; %s", DescribeUpload(row, upload).c_str(),
                    row.Detected() ? "detected" : "not detected or inconclusive");
        }
        return measurement;
    } catch (const std::bad_alloc&) {
        // The CPU initializer is arena-sized too. An allocation failure must not discard
        // the rest of the POST report or turn a partially sampled case into a finding.
        MGLOG_I("[driver-bug] persistent buffer ordering: host allocation failed; inconclusive");
        PersistentBufferOrderingMeasurement measurement;
        measurement.supported = true;
        for (auto& row : measurement.uploads) row.unmapped.status = BufferOrderingProbeStatus::Failed;
        return measurement;
    }

    Optional<DriverBugFinding> DescribePersistentBufferOrderingBug(
        const PersistentBufferOrderingMeasurement& measurement) {
        String detail;
        for (Int upload = 0; upload < Int(measurement.uploads.size()); ++upload) {
            if (!measurement.uploads[upload].Detected()) continue;
            if (!detail.empty()) detail += "; ";
            detail += DescribeUpload(measurement.uploads[upload], upload);
        }
        if (detail.empty()) return std::nullopt;
        detail += ". A 128 MiB immutable vertex destination was mapped WRITE|PERSISTENT|COHERENT, "
                  "but never accessed through its client pointer. Queued uploads/draws corrupt vertex data; "
                  "identical never-mapped and Finish-before-and-after controls pass. "
                  "This POST does not enable a workaround. MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION=1 "
                  "avoids automatic arena adoption; explicit application mappings remain separate. "
                  "FBO counts describe this bounded stress probe, not application flicker frequency.";
        return DriverBugFinding{"Persistent-mapped vertex buffers lose upload/draw ordering",
                                DriverBugVerdict::Unfixable, Move(detail)};
    }
} // namespace MobileGL::MG_Util::SelfTest
