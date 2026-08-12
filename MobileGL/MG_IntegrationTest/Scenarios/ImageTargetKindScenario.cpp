// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ImageTargetKindScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - ONE IMAGE TARGET KIND AT A TIME, THROUGH A COMPUTE DISPATCH.
//
// KHR-GL44.multi_bind.dispatch_bind_image_textures decomposed. That conformance case declares
// ELEVEN image uniforms of eleven different target kinds in one compute shader, binds a texture
// of the matching kind to each unit, sums one texel from every one of them and compares the sum
// against N*(N-1)/2. It is a single pass/fail bit over eleven independent mechanisms: if any one
// of them is wrong - or merely fails to compile - the case fails and says nothing about which.
// That is what it did here, on both backends, for two waves.
//
// So the eleven are pulled apart into one case each. Each case declares ONE image uniform, binds
// ONE texture and checks the value that comes back, so a failure names the target kind and the
// direction. What the conformance case does with eleven at once, AllKindsInOneProgram at the
// bottom still does - a defect that only appears when several kinds share a program is invisible
// to the single-kind cases by construction.
//
// The shape is deliberately the conformance case's own, not a cleaner equivalent:
//
//   * r32ui / GL_R32UI throughout, 6x6x6 storage, one level, texel (0,0,0) read;
//   * `layout (location = N, r32ui) readonly uniform` - an explicit uniform LOCATION, not a
//     binding, with the image unit then assigned by glUniform1i. That combination is the one ES
//     cannot express directly, because ES forbids glUniform1i on an image uniform and the unit
//     has to be baked into the generated ESSL (RebindImageUniformsToFrontendUnits);
//   * `layout (std140, ...) buffer` for the result block - legal, but unusual enough that a
//     frontend could plausibly mishandle it. Mirroring it means a green scenario cannot be green
//     for a reason the conformance case excludes;
//   * glBindImageTexture with layered = GL_TRUE, which is what glBindImageTextures is specified
//     to pass, and which is where a target kind whose layeredness a backend does not recognise
//     goes wrong.
//
// MULTISAMPLE is the one kind that is not merely an emulation problem, and the conformance case
// already knows it: it reads GL_MAX_IMAGE_SAMPLES and, when that is zero, substitutes a plain 2D
// texture and a plain uimage2D for both multisample entries. MobileGL reports zero, so the
// conformance case never asks it for a multisample image at all. The two cases below are kept
// and skip on that same query, so the coverage is already written the day a backend advertises
// them - and so the skip is a standing record of WHY the conformance case passes without them.

#include <algorithm>
#include <cstddef>
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

        // The conformance case's own dimensions: one level, 6 on every axis (which is also
        // exactly one cube's worth for a cube array), and a single texel read at the origin.
        constexpr int kExtent = 6;
        constexpr GLuint kFilledValue = 7u;
        constexpr GLuint kStoredValue = 13u;

        // Everything that differs between the eleven kinds, in one row.
        struct TargetKind {
            const char* name;      // this scenario's name for it, which failure messages carry
            GLenum target;         // the GL texture target
            const char* imageType; // the GLSL image uniform type
            const char* coord;     // the coordinate expression imageLoad/imageStore takes
            bool multisample;      // needs GL_MAX_IMAGE_SAMPLES > 0
            bool buffer;           // storage comes from a buffer object, not TexStorage
        };

        constexpr TargetKind kKind1D{"1D", GL_TEXTURE_1D, "uimage1D", "0", false, false};
        constexpr TargetKind kKind1DArray{"1DArray", GL_TEXTURE_1D_ARRAY, "uimage1DArray", "ivec2(0, 0)", false,
                                          false};
        constexpr TargetKind kKind2D{"2D", GL_TEXTURE_2D, "uimage2D", "ivec2(0, 0)", false, false};
        constexpr TargetKind kKind2DArray{"2DArray", GL_TEXTURE_2D_ARRAY, "uimage2DArray", "ivec3(0, 0, 0)", false,
                                          false};
        constexpr TargetKind kKind3D{"3D", GL_TEXTURE_3D, "uimage3D", "ivec3(0, 0, 0)", false, false};
        constexpr TargetKind kKindBuffer{"Buffer", GL_TEXTURE_BUFFER, "uimageBuffer", "0", false, true};
        constexpr TargetKind kKindCube{"Cube", GL_TEXTURE_CUBE_MAP, "uimageCube", "ivec3(0, 0, 0)", false, false};
        constexpr TargetKind kKindCubeArray{"CubeArray", GL_TEXTURE_CUBE_MAP_ARRAY, "uimageCubeArray",
                                            "ivec3(0, 0, 0)", false, false};
        constexpr TargetKind kKindRect{"Rect", GL_TEXTURE_RECTANGLE, "uimage2DRect", "ivec2(0, 0)", false, false};
        constexpr TargetKind kKind2DMS{"2DMS", GL_TEXTURE_2D_MULTISAMPLE, "uimage2DMS", "ivec2(0, 0)", true, false};
        constexpr TargetKind kKind2DMSArray{"2DMSArray", GL_TEXTURE_2D_MULTISAMPLE_ARRAY, "uimage2DMSArray",
                                            "ivec3(0, 0, 0)", true, false};

        // A multisample image load/store takes the sample index as an extra argument; no other
        // kind does. Keeping that in one place stops the two spellings drifting apart.
        std::string LoadExpression(const TargetKind& kind, const std::string& name) {
            return "imageLoad(" + name + ", " + kind.coord + (kind.multisample ? ", 0)" : ")");
        }

        std::string StoreStatement(const TargetKind& kind, const std::string& name, const char* value) {
            return "imageStore(" + name + ", " + kind.coord + (kind.multisample ? ", 0, uvec4(" : ", uvec4(") +
                   value + ", 0, 0, 0));";
        }

        const char* kComputePrologue = "#version 440 core\n"
                                       "\n"
                                       "layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n"
                                       "\n";

        const char* kResultBlock = "layout (std140, binding = 0) buffer SSB {\n"
                                   "    uint sum;\n"
                                   "} ssb;\n"
                                   "\n";

        // The conformance case's shader, narrowed to a single image.
        std::string SingleLoadSource(const TargetKind& kind) {
            return std::string(kComputePrologue) + "layout (location = 0, r32ui) readonly uniform " + kind.imageType +
                   " i0;\n" + kResultBlock + "void main()\n{\n    uvec4 v = " + LoadExpression(kind, "i0") +
                   ";\n    ssb.sum = v.r;\n}\n";
        }

        // The other direction. Written as its own program rather than a read-write one so that a
        // backend which gets the store right and the load wrong (or the reverse) is not able to
        // cancel its own defect out.
        std::string SingleStoreSource(const TargetKind& kind) {
            return std::string(kComputePrologue) + "layout (location = 0, r32ui) writeonly uniform " +
                   kind.imageType + " i0;\n\nvoid main()\n{\n    " + StoreStatement(kind, "i0", "13u") + "\n}\n";
        }

        class ImageTargetKindScenario : public ScenarioTest {
        protected:
            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                for (GLuint p : m_programs) glDeleteProgram(p);
                for (GLuint t : m_textures) glDeleteTextures(1, &t);
                for (GLuint b : m_buffers) glDeleteBuffers(1, &b);
                m_programs.clear();
                m_textures.clear();
                m_buffers.clear();
                // Leave no image unit bound. These scenarios share one context, and a stale image
                // binding is exactly the kind of state that makes the NEXT scenario's failure
                // impossible to reproduce on its own.
                GLint maxImageUnits = 0;
                glGetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);
                for (GLint unit = 0; unit < maxImageUnits; ++unit) {
                    glBindImageTexture(static_cast<GLuint>(unit), 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
                }
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
                while (glGetError() != GL_NO_ERROR) {
                }
            }

            bool ImagesAreUsable() const {
                GLint maxImageUnits = 0;
                glGetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);
                GLint maxComputeImageUniforms = 0;
                glGetIntegerv(GL_MAX_COMPUTE_IMAGE_UNIFORMS, &maxComputeImageUniforms);
                while (glGetError() != GL_NO_ERROR) {
                }
                return maxImageUnits >= 1 && maxComputeImageUniforms >= 1;
            }

            // The conformance case's own multisample gate, asked the same way it asks it.
            bool MultisampleImagesAreUsable() const {
                GLint maxImageSamples = 0;
                glGetIntegerv(GL_MAX_IMAGE_SAMPLES, &maxImageSamples);
                while (glGetError() != GL_NO_ERROR) {
                }
                return maxImageSamples > 0;
            }

            GLuint MakeComputeProgram(const std::string& source) {
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                const char* text = source.c_str();
                glShaderSource(shader, 1, &text, nullptr);
                glCompileShader(shader);
                GLint compiled = GL_FALSE;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (compiled == GL_FALSE) {
                    char log[4096] = {};
                    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                    ADD_FAILURE() << "the compute shader did not compile: " << log << "\nsource:\n" << source;
                    glDeleteShader(shader);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                m_programs.push_back(program);
                glAttachShader(program, shader);
                glLinkProgram(program);
                glDeleteShader(shader);
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_FALSE) {
                    char log[4096] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    ADD_FAILURE() << "the compute program did not link: " << log << "\nsource:\n" << source;
                    return 0;
                }
                return program;
            }

            // Storage plus a full fill with `value`, in the spelling each target kind needs.
            // Returns 0 - having already reported - when the target could not be created.
            GLuint MakeTexture(const TargetKind& kind, bool fill, GLuint value = kFilledValue) {
                const std::vector<GLuint> texels(static_cast<std::size_t>(kExtent) * kExtent * kExtent, value);

                if (kind.buffer) {
                    GLuint buffer = 0;
                    glGenBuffers(1, &buffer);
                    m_buffers.push_back(buffer);
                    glBindBuffer(GL_TEXTURE_BUFFER, buffer);
                    glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(texels.size() * sizeof(GLuint)),
                                 fill ? texels.data() : nullptr, GL_DYNAMIC_COPY);
                    GLuint texture = 0;
                    glGenTextures(1, &texture);
                    m_textures.push_back(texture);
                    glBindTexture(GL_TEXTURE_BUFFER, texture);
                    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, buffer);
                    if (const GLenum error = FirstGLError()) {
                        ADD_FAILURE() << kind.name << ": creating the texture buffer errored with "
                                      << GLErrorName(error);
                        return 0;
                    }
                    return texture;
                }

                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                glBindTexture(kind.target, texture);

                switch (kind.target) {
                case GL_TEXTURE_1D:
                    glTexStorage1D(kind.target, 1, GL_R32UI, kExtent);
                    break;
                case GL_TEXTURE_2D:
                case GL_TEXTURE_RECTANGLE:
                case GL_TEXTURE_1D_ARRAY:
                case GL_TEXTURE_CUBE_MAP:
                    glTexStorage2D(kind.target, 1, GL_R32UI, kExtent, kExtent);
                    break;
                case GL_TEXTURE_2D_ARRAY:
                case GL_TEXTURE_3D:
                case GL_TEXTURE_CUBE_MAP_ARRAY:
                    glTexStorage3D(kind.target, 1, GL_R32UI, kExtent, kExtent, kExtent);
                    break;
                case GL_TEXTURE_2D_MULTISAMPLE:
                    glTexStorage2DMultisample(kind.target, 1, GL_R32UI, kExtent, kExtent, GL_FALSE);
                    break;
                case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
                    glTexStorage3DMultisample(kind.target, 1, GL_R32UI, kExtent, kExtent, kExtent, GL_FALSE);
                    break;
                default:
                    ADD_FAILURE() << kind.name << ": no storage spelling for target 0x" << std::hex << kind.target;
                    return 0;
                }
                if (const GLenum error = FirstGLError()) {
                    ADD_FAILURE() << kind.name << ": allocating storage errored with " << GLErrorName(error);
                    return 0;
                }

                // A multisample texture has no TexSubImage - the conformance case fills it with a
                // compute pass, which is what the store cases below do.
                if (!fill || kind.multisample) return texture;

                switch (kind.target) {
                case GL_TEXTURE_1D:
                    glTexSubImage1D(kind.target, 0, 0, kExtent, GL_RED_INTEGER, GL_UNSIGNED_INT, texels.data());
                    break;
                case GL_TEXTURE_2D:
                case GL_TEXTURE_RECTANGLE:
                case GL_TEXTURE_1D_ARRAY:
                    glTexSubImage2D(kind.target, 0, 0, 0, kExtent, kExtent, GL_RED_INTEGER, GL_UNSIGNED_INT,
                                    texels.data());
                    break;
                case GL_TEXTURE_CUBE_MAP:
                    for (int face = 0; face < 6; ++face) {
                        glTexSubImage2D(static_cast<GLenum>(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face), 0, 0, 0, kExtent,
                                        kExtent, GL_RED_INTEGER, GL_UNSIGNED_INT, texels.data());
                    }
                    break;
                case GL_TEXTURE_2D_ARRAY:
                case GL_TEXTURE_3D:
                case GL_TEXTURE_CUBE_MAP_ARRAY:
                    glTexSubImage3D(kind.target, 0, 0, 0, 0, kExtent, kExtent, kExtent, GL_RED_INTEGER,
                                    GL_UNSIGNED_INT, texels.data());
                    break;
                default:
                    break;
                }
                if (const GLenum error = FirstGLError()) {
                    ADD_FAILURE() << kind.name << ": uploading texels errored with " << GLErrorName(error);
                    return 0;
                }
                return texture;
            }

            // A 4-byte `buffer` block bound to base 0, which is where every case puts its answer.
            GLuint MakeResultBuffer() {
                GLuint ssbo = 0;
                glGenBuffers(1, &ssbo);
                m_buffers.push_back(ssbo);
                const GLuint zero = 0u;
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
                glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint), &zero, GL_DYNAMIC_COPY);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
                return ssbo;
            }

            GLuint ReadResult(GLuint ssbo) {
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
                GLuint value = 0xFFFFFFFFu;
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(GLuint), &value);
                return value;
            }

            // Fill a texture of `kind`, read texel (0,0,0) of it through an image uniform in a
            // compute dispatch, and require the value back.
            void RunLoadCase(const TargetKind& kind) {
                const GLuint program = MakeComputeProgram(SingleLoadSource(kind));
                if (program == 0) return;
                const GLuint texture = MakeTexture(kind, true);
                if (texture == 0) return;
                const GLuint ssbo = MakeResultBuffer();

                glBindImageTexture(0, texture, 0, GL_TRUE, 0, GL_READ_ONLY, GL_R32UI);
                ASSERT_EQ(FirstGLError(), 0u) << kind.name << ": glBindImageTexture errored";

                glUseProgram(program);
                // The unit, by LOCATION - the conformance case's own redundant-but-legal
                // assignment, and the one ES cannot take at the API level.
                glUniform1i(0, 0);
                ASSERT_EQ(FirstGLError(), 0u) << kind.name << ": assigning the image unit errored";

                glDispatchCompute(1, 1, 1);
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                EXPECT_EQ(FirstGLError(), 0u) << kind.name << ": the dispatch leaked a GL error";

                EXPECT_EQ(ReadResult(ssbo), kFilledValue)
                    << kind.name << ": the compute dispatch did not read the value the texture was filled with";
                glUseProgram(0);
            }

            // The other direction: store through an image uniform, then read the same texel back
            // through a SECOND program, so a defect cannot cancel itself out.
            void RunStoreCase(const TargetKind& kind) {
                const GLuint storeProgram = MakeComputeProgram(SingleStoreSource(kind));
                const GLuint loadProgram = MakeComputeProgram(SingleLoadSource(kind));
                if (storeProgram == 0 || loadProgram == 0) return;
                const GLuint texture = MakeTexture(kind, false);
                if (texture == 0) return;
                const GLuint ssbo = MakeResultBuffer();

                glBindImageTexture(0, texture, 0, GL_TRUE, 0, GL_READ_WRITE, GL_R32UI);
                ASSERT_EQ(FirstGLError(), 0u) << kind.name << ": glBindImageTexture errored";

                glUseProgram(storeProgram);
                glUniform1i(0, 0);
                glDispatchCompute(1, 1, 1);
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                EXPECT_EQ(FirstGLError(), 0u) << kind.name << ": the storing dispatch leaked a GL error";

                glUseProgram(loadProgram);
                glUniform1i(0, 0);
                glDispatchCompute(1, 1, 1);
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                EXPECT_EQ(FirstGLError(), 0u) << kind.name << ": the loading dispatch leaked a GL error";

                EXPECT_EQ(ReadResult(ssbo), kStoredValue)
                    << kind.name << ": the value stored through the image did not come back";
                glUseProgram(0);
            }

            std::vector<GLuint> m_programs;
            std::vector<GLuint> m_textures;
            std::vector<GLuint> m_buffers;
        };

    } // namespace

    // ---- the load direction, one target kind per case -----------------------
    //
    // Exactly what the conformance case does with each of its eleven uniforms, but alone, so a
    // failure names the kind.

#define MGL_DEFINE_LOAD_CASE(CaseName, Kind)                                                                    \
    TEST_F(ImageTargetKindScenario, Loads##CaseName) {                                                          \
        if (!Ready()) return;                                                                                   \
        if (!ImagesAreUsable()) GTEST_SKIP() << "no compute image uniforms";                                    \
        if ((Kind).multisample && !MultisampleImagesAreUsable()) {                                              \
            GTEST_SKIP() << "GL_MAX_IMAGE_SAMPLES is 0, so the conformance case substitutes a plain 2D image "   \
                            "here and never asks for a multisample one";                                        \
        }                                                                                                       \
        RunLoadCase(Kind);                                                                                      \
    }

#define MGL_DEFINE_STORE_CASE(CaseName, Kind)                                                                   \
    TEST_F(ImageTargetKindScenario, Stores##CaseName) {                                                         \
        if (!Ready()) return;                                                                                   \
        if (!ImagesAreUsable()) GTEST_SKIP() << "no compute image uniforms";                                    \
        if ((Kind).multisample && !MultisampleImagesAreUsable()) {                                              \
            GTEST_SKIP() << "GL_MAX_IMAGE_SAMPLES is 0, so the conformance case substitutes a plain 2D image "   \
                            "here and never asks for a multisample one";                                        \
        }                                                                                                       \
        RunStoreCase(Kind);                                                                                      \
    }

    MGL_DEFINE_LOAD_CASE(Texture1D, kKind1D)
    MGL_DEFINE_LOAD_CASE(Texture1DArray, kKind1DArray)
    MGL_DEFINE_LOAD_CASE(Texture2D, kKind2D)
    MGL_DEFINE_LOAD_CASE(Texture2DArray, kKind2DArray)
    MGL_DEFINE_LOAD_CASE(Texture3D, kKind3D)
    MGL_DEFINE_LOAD_CASE(TextureBuffer, kKindBuffer)
    MGL_DEFINE_LOAD_CASE(TextureCube, kKindCube)
    MGL_DEFINE_LOAD_CASE(TextureCubeArray, kKindCubeArray)
    MGL_DEFINE_LOAD_CASE(TextureRectangle, kKindRect)
    MGL_DEFINE_LOAD_CASE(Texture2DMultisample, kKind2DMS)
    MGL_DEFINE_LOAD_CASE(Texture2DMultisampleArray, kKind2DMSArray)

    MGL_DEFINE_STORE_CASE(Texture1D, kKind1D)
    MGL_DEFINE_STORE_CASE(Texture1DArray, kKind1DArray)
    MGL_DEFINE_STORE_CASE(Texture2D, kKind2D)
    MGL_DEFINE_STORE_CASE(Texture2DArray, kKind2DArray)
    MGL_DEFINE_STORE_CASE(Texture3D, kKind3D)
    MGL_DEFINE_STORE_CASE(TextureBuffer, kKindBuffer)
    MGL_DEFINE_STORE_CASE(TextureCube, kKindCube)
    MGL_DEFINE_STORE_CASE(TextureCubeArray, kKindCubeArray)
    MGL_DEFINE_STORE_CASE(TextureRectangle, kKindRect)
    MGL_DEFINE_STORE_CASE(Texture2DMultisample, kKind2DMS)
    MGL_DEFINE_STORE_CASE(Texture2DMultisampleArray, kKind2DMSArray)

#undef MGL_DEFINE_LOAD_CASE
#undef MGL_DEFINE_STORE_CASE

    // ---- and all of them at once -------------------------------------------
    //
    // The conformance case's actual shape. The single-kind cases above cannot see a defect that
    // needs several kinds in one program - a binding remap that only collides when two image
    // types share a descriptor set, a per-kind rewrite that is not idempotent across declarations
    // - and that class of defect is precisely what "each kind passes alone but the case still
    // fails" would mean.
    //
    // Each unit is filled with its own DISTINCT value rather than a shared one, so a shortfall
    // names WHICH kind is missing rather than merely how many are: with one shared value, "three
    // kinds read zero" and "one kind read zero" differ only by a multiple, and any two kinds are
    // interchangeable in the total. A sum still cannot see two kinds SWAPPING - addition is
    // commutative, and the conformance case has exactly the same blind spot - but the single-kind
    // cases above pin each kind to its own texture already, so a swap cannot hide there.
    TEST_F(ImageTargetKindScenario, AllKindsInOneProgram) {
        if (!Ready()) return;
        if (!ImagesAreUsable()) GTEST_SKIP() << "no compute image uniforms";

        // The two kinds this whole scenario file exists for come FIRST, and that ordering is
        // load-bearing rather than cosmetic. The list has to be truncated to the device's image
        // unit count, and the guaranteed minimum is small - ES 3.1 promises only four compute
        // image uniforms - so a list in the conformance case's own order would put imageBuffer
        // at index five and drop it on exactly the devices most likely to get it wrong. A test
        // that quietly stops covering its own subject is worse than one that fails.
        const bool multisample = MultisampleImagesAreUsable();
        std::vector<TargetKind> kinds{kKind1DArray, kKindBuffer, kKind2D,   kKind1D,      kKind2DArray,
                                      kKind3D,      kKindCube,   kKindRect, kKindCubeArray};
        if (multisample) {
            kinds.push_back(kKind2DMS);
            kinds.push_back(kKind2DMSArray);
        }

        GLint maxComputeImageUniforms = 0;
        glGetIntegerv(GL_MAX_COMPUTE_IMAGE_UNIFORMS, &maxComputeImageUniforms);
        GLint maxImageUnits = 0;
        glGetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);
        while (glGetError() != GL_NO_ERROR) {
        }
        const std::size_t count =
            std::min<std::size_t>(kinds.size(), static_cast<std::size_t>(std::max(0, std::min(maxComputeImageUniforms,
                                                                                              maxImageUnits))));
        if (count == 0) GTEST_SKIP() << "no image units";
        // Named, not silently dropped: `expected` is computed over whatever survives, so a
        // truncated run is self-consistently green and would otherwise never say what it stopped
        // covering.
        if (count < kinds.size()) {
            std::string dropped;
            for (std::size_t i = count; i < kinds.size(); ++i) {
                if (!dropped.empty()) dropped += ", ";
                dropped += kinds[i].name;
            }
            RecordProperty("dropped_image_target_kinds", dropped);
            GTEST_LOG_(INFO) << "only " << count << " image units, so these kinds are not covered by the "
                             << "combined case: " << dropped;
        }
        kinds.resize(count);

        std::string declarations;
        std::string sum;
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            const std::string name = "i" + std::to_string(i);
            declarations += "layout (location = " + std::to_string(i) + ", r32ui) readonly uniform " +
                            kinds[i].imageType + " " + name + ";\n";
            if (!sum.empty()) sum += " + ";
            sum += LoadExpression(kinds[i], name);
        }
        const std::string source = std::string(kComputePrologue) + declarations + kResultBlock +
                                   "void main()\n{\n    uvec4 v = " + sum + ";\n    ssb.sum = v.r;\n}\n";

        const GLuint program = MakeComputeProgram(source);
        if (program == 0) return;

        // Powers of two, so the shortfall's bit pattern names exactly which kinds read zero -
        // no other subset of the values can sum to the same total. Eleven kinds at most, so the
        // largest is 1 << 10 and the sum cannot approach a uint's range.
        GLuint expected = 0;
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            const GLuint value = 1u << i;
            const GLuint texture = MakeTexture(kinds[i], true, value);
            if (texture == 0) return;
            expected += value;
            glBindImageTexture(static_cast<GLuint>(i), texture, 0, GL_TRUE, 0, GL_READ_ONLY, GL_R32UI);
            ASSERT_EQ(FirstGLError(), 0u) << kinds[i].name << ": glBindImageTexture errored";
        }
        const GLuint ssbo = MakeResultBuffer();

        glUseProgram(program);
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            glUniform1i(static_cast<GLint>(i), static_cast<GLint>(i));
        }
        ASSERT_EQ(FirstGLError(), 0u) << "assigning the image units errored";

        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        EXPECT_EQ(FirstGLError(), 0u) << "the dispatch leaked a GL error";

        const GLuint actual = ReadResult(ssbo);
        std::string missing;
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            if ((actual & (1u << i)) == 0u) {
                if (!missing.empty()) missing += ", ";
                missing += kinds[i].name;
            }
        }
        EXPECT_EQ(actual, expected)
            << "the sum over " << kinds.size()
            << " image target kinds is wrong; each kind contributes its own bit, and these read "
               "zero: "
            << (missing.empty() ? "(none - so some kind read a value it was never given)" : missing);
        glUseProgram(0);
    }

} // namespace MGITest
